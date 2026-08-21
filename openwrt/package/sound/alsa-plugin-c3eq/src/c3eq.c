/*
 * c3eq - ALSA external PCM plugin: cascaded biquad EQ in fixed point.
 *
 * The Addon C3 has no DSP anywhere in hardware: the PCM5102A DAC has no volume
 * register and no filters, and the TPA3116 gain is pin-strapped. The stock
 * firmware therefore did its voicing in software (a01controller carries
 * CStandaloneNode_SetEqualizer / DesiredEqualizer / <Equaluzer channel="Master">),
 * and without an equivalent our chain is dead flat and sounds bass-light.
 *
 * LADSPA and everything else off the shelf wants floats, and the MT7628AN has no
 * FPU - soft-float biquads would cost tens of percent of the CPU and bring back
 * the dropouts. So: doubles at open() to run the RBJ cookbook once, Q24 integers
 * in the sample loop. Six sections cost ~1% CPU at 44.1 kHz stereo.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#define MAX_SECTIONS	8
#define MAX_CHANNELS	8
#define QBITS		24
#define QONE		(1 << QBITS)
/* the cookbook keeps normalised coefficients well under 8 for anything sane;
 * past that the Q24 headroom is gone and the accumulator could wrap */
#define QMAX		(8 << QBITS)

struct section {
	int32_t b0, b1, b2, a1, a2;
	int32_t x1[MAX_CHANNELS], x2[MAX_CHANNELS];
	int32_t y1[MAX_CHANNELS], y2[MAX_CHANNELS];
};

/* what the config file asked for, before we know the sample rate */
struct spec {
	int kind;
	double freq, q, gain;
};

enum { K_PEAK, K_LOWSHELF, K_HIGHSHELF, K_HIGHPASS, K_LOWPASS };

struct c3eq {
	snd_pcm_extplug_t ext;
	char *conf_path;
	struct spec spec[MAX_SECTIONS];
	int n_spec;
	double preamp_dB;
	int bypass;
	/* live state, rebuilt on every open so a config edit needs no restart */
	struct section sect[MAX_SECTIONS];
	int n_sect;
	int32_t pre;
};

static int32_t to_q(double v)
{
	double s = v * QONE;

	if (s > (double)QMAX || s < -(double)QMAX)
		return 0;
	return (int32_t)lrint(s);
}

static int design(struct section *s, const struct spec *sp, unsigned int rate)
{
	double w0 = 2.0 * M_PI * sp->freq / (double)rate;
	double cs = cos(w0), sn = sin(w0);
	double q = sp->q > 0.01 ? sp->q : 0.707;
	double alpha = sn / (2.0 * q);
	double A, sq, b0, b1, b2, a0, a1, a2;

	if (sp->freq <= 0.0 || w0 >= M_PI * 0.98)
		return -EINVAL;

	switch (sp->kind) {
	case K_PEAK:
		A = pow(10.0, sp->gain / 40.0);
		b0 = 1.0 + alpha * A;
		b1 = -2.0 * cs;
		b2 = 1.0 - alpha * A;
		a0 = 1.0 + alpha / A;
		a1 = -2.0 * cs;
		a2 = 1.0 - alpha / A;
		break;
	case K_LOWSHELF:
		A = pow(10.0, sp->gain / 40.0);
		sq = 2.0 * sqrt(A) * alpha;
		b0 = A * ((A + 1.0) - (A - 1.0) * cs + sq);
		b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cs);
		b2 = A * ((A + 1.0) - (A - 1.0) * cs - sq);
		a0 = (A + 1.0) + (A - 1.0) * cs + sq;
		a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cs);
		a2 = (A + 1.0) + (A - 1.0) * cs - sq;
		break;
	case K_HIGHSHELF:
		A = pow(10.0, sp->gain / 40.0);
		sq = 2.0 * sqrt(A) * alpha;
		b0 = A * ((A + 1.0) + (A - 1.0) * cs + sq);
		b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cs);
		b2 = A * ((A + 1.0) + (A - 1.0) * cs - sq);
		a0 = (A + 1.0) - (A - 1.0) * cs + sq;
		a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cs);
		a2 = (A + 1.0) - (A - 1.0) * cs - sq;
		break;
	case K_HIGHPASS:
		b0 = (1.0 + cs) / 2.0;
		b1 = -(1.0 + cs);
		b2 = (1.0 + cs) / 2.0;
		a0 = 1.0 + alpha;
		a1 = -2.0 * cs;
		a2 = 1.0 - alpha;
		break;
	case K_LOWPASS:
		b0 = (1.0 - cs) / 2.0;
		b1 = 1.0 - cs;
		b2 = (1.0 - cs) / 2.0;
		a0 = 1.0 + alpha;
		a1 = -2.0 * cs;
		a2 = 1.0 - alpha;
		break;
	default:
		return -EINVAL;
	}

	memset(s, 0, sizeof(*s));
	s->b0 = to_q(b0 / a0);
	s->b1 = to_q(b1 / a0);
	s->b2 = to_q(b2 / a0);
	s->a1 = to_q(a1 / a0);
	s->a2 = to_q(a2 / a0);
	return 0;
}

static void parse_conf(struct c3eq *eq)
{
	FILE *f;
	char line[160];

	eq->n_spec = 0;
	eq->preamp_dB = 0.0;
	eq->bypass = 0;

	f = fopen(eq->conf_path, "r");
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char kw[24];
		double a = 0, b = 0, c = 0;
		int n, kind;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		n = sscanf(line, "%23s %lf %lf %lf", kw, &a, &b, &c);
		if (n < 1)
			continue;
		if (!strcmp(kw, "bypass")) {
			eq->bypass = (n >= 2 && a != 0.0);
			continue;
		}
		if (!strcmp(kw, "preamp")) {
			if (n >= 2)
				eq->preamp_dB = a;
			continue;
		}
		if (!strcmp(kw, "peak"))
			kind = K_PEAK;
		else if (!strcmp(kw, "lowshelf"))
			kind = K_LOWSHELF;
		else if (!strcmp(kw, "highshelf"))
			kind = K_HIGHSHELF;
		else if (!strcmp(kw, "highpass"))
			kind = K_HIGHPASS;
		else if (!strcmp(kw, "lowpass"))
			kind = K_LOWPASS;
		else
			continue;
		if (n < 3 || eq->n_spec >= MAX_SECTIONS)
			continue;
		eq->spec[eq->n_spec].kind = kind;
		eq->spec[eq->n_spec].freq = a;
		eq->spec[eq->n_spec].q = b;
		eq->spec[eq->n_spec].gain = c;
		eq->n_spec++;
	}
	fclose(f);
}

static int c3eq_init(snd_pcm_extplug_t *ext)
{
	struct c3eq *eq = ext->private_data;
	int i;

	parse_conf(eq);
	eq->n_sect = 0;
	eq->pre = QONE;

	if (eq->bypass)
		return 0;

	eq->pre = to_q(pow(10.0, eq->preamp_dB / 20.0));
	if (!eq->pre)
		eq->pre = QONE;

	for (i = 0; i < eq->n_spec; i++) {
		if (design(&eq->sect[eq->n_sect], &eq->spec[i], ext->rate) == 0)
			eq->n_sect++;
		else
			SNDERR("c3eq: bad section %d, skipped", i);
	}
	return 0;
}

static inline int32_t sat(int64_t v)
{
	if (v > INT32_MAX)
		return INT32_MAX;
	if (v < INT32_MIN)
		return INT32_MIN;
	return (int32_t)v;
}

static snd_pcm_sframes_t c3eq_transfer(snd_pcm_extplug_t *ext,
			const snd_pcm_channel_area_t *dst_areas,
			snd_pcm_uframes_t dst_offset,
			const snd_pcm_channel_area_t *src_areas,
			snd_pcm_uframes_t src_offset,
			snd_pcm_uframes_t size)
{
	struct c3eq *eq = ext->private_data;
	unsigned int ch, nch = ext->channels;
	snd_pcm_uframes_t n;
	int s;

	if (nch > MAX_CHANNELS)
		nch = MAX_CHANNELS;

	for (ch = 0; ch < nch; ch++) {
		const snd_pcm_channel_area_t *sa = &src_areas[ch];
		const snd_pcm_channel_area_t *da = &dst_areas[ch];
		char *src = (char *)sa->addr + sa->first / 8 + src_offset * (sa->step / 8);
		char *dst = (char *)da->addr + da->first / 8 + dst_offset * (da->step / 8);
		int sstep = sa->step / 8, dstep = da->step / 8;

		for (n = 0; n < size; n++) {
			int32_t x;

			memcpy(&x, src, 4);
			if (eq->n_sect) {
				x = sat(((int64_t)x * eq->pre) >> QBITS);
				for (s = 0; s < eq->n_sect; s++) {
					struct section *f = &eq->sect[s];
					int64_t acc;
					int32_t y;

					acc = (int64_t)f->b0 * x
					    + (int64_t)f->b1 * f->x1[ch]
					    + (int64_t)f->b2 * f->x2[ch]
					    - (int64_t)f->a1 * f->y1[ch]
					    - (int64_t)f->a2 * f->y2[ch];
					y = sat(acc >> QBITS);
					f->x2[ch] = f->x1[ch];
					f->x1[ch] = x;
					f->y2[ch] = f->y1[ch];
					f->y1[ch] = y;
					x = y;
				}
			}
			memcpy(dst, &x, 4);
			src += sstep;
			dst += dstep;
		}
	}
	return size;
}

static int c3eq_close(snd_pcm_extplug_t *ext)
{
	struct c3eq *eq = ext->private_data;

	free(eq->conf_path);
	free(eq);
	return 0;
}

static const snd_pcm_extplug_callback_t c3eq_callback = {
	.transfer = c3eq_transfer,
	.init = c3eq_init,
	.close = c3eq_close,
};

SND_PCM_PLUGIN_DEFINE_FUNC(c3eq)
{
	snd_config_iterator_t i, next;
	snd_config_t *slave = NULL;
	const char *path = "/etc/c3eq.conf";
	struct c3eq *eq;
	int err;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;

		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (!strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint"))
			continue;
		if (!strcmp(id, "slave")) {
			slave = n;
			continue;
		}
		if (!strcmp(id, "conf")) {
			if (snd_config_get_string(n, &path) < 0) {
				SNDERR("c3eq: conf must be a string");
				return -EINVAL;
			}
			continue;
		}
		SNDERR("c3eq: unknown field %s", id);
		return -EINVAL;
	}
	if (!slave) {
		SNDERR("c3eq: no slave defined");
		return -EINVAL;
	}

	eq = calloc(1, sizeof(*eq));
	if (!eq)
		return -ENOMEM;
	eq->conf_path = strdup(path);
	if (!eq->conf_path) {
		free(eq);
		return -ENOMEM;
	}

	eq->ext.version = SND_PCM_EXTPLUG_VERSION;
	eq->ext.name = "Audio Pro C3 EQ";
	eq->ext.callback = &c3eq_callback;
	eq->ext.private_data = eq;

	err = snd_pcm_extplug_create(&eq->ext, name, root, slave, stream, mode);
	if (err < 0) {
		free(eq->conf_path);
		free(eq);
		return err;
	}

	snd_pcm_extplug_set_param(&eq->ext, SND_PCM_EXTPLUG_HW_FORMAT,
				  SND_PCM_FORMAT_S32_LE);
	snd_pcm_extplug_set_slave_param(&eq->ext, SND_PCM_EXTPLUG_HW_FORMAT,
					SND_PCM_FORMAT_S32_LE);

	*pcmp = eq->ext.pcm;
	return 0;
}

SND_PCM_PLUGIN_SYMBOL(c3eq);
