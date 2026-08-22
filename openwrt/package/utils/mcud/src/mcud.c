#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <signal.h>
#include <alloca.h>
#include <getopt.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <alsa/asoundlib.h>
#include <mosquitto.h>

#include <libubox/uloop.h>
#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubus.h>

#define SERIAL_PORT          "/dev/ttyS0"
#define CMD_FIFO_PATH        "/tmp/mcu_cmd_fifo"
#define BAUD_RATE            B57600
#define WATCHDOG_CHECK_MS    5000
#define SLEEP_CHECK_MS       5000
#define VOL_STEP_DB          3   /* per key press, see adjust_user_volume */
#define MCU_VOL_TOLERANCE    2   /* MCU rounds our 0..100 onto its own scale */
#define MAX_LINE_LEN         128
#define READ_BUF_SIZE        256
#define BUTTON_DEBOUNCE_MS   120

#define SOUND_BOOT           "/usr/share/sounds/boot.mp3"
#define SOUND_PRESET         "/usr/share/sounds/preset_saved.mp3"
#define SOUND_BT_CONN        "/usr/share/sounds/bt_connected.mp3"
#define SOUND_WIFI_CONN      "/usr/share/sounds/wifi_connected.mp3"
#define SOUND_LOW_BAT        "/usr/share/sounds/low_battery.mp3"

/* STM8S Factory Opcodes (Linkplay A28 / Audio Pro C3 Hardware Spec) */
#define CMD_MCU_READY        "AXX+MCU+RDY\n"
#define CMD_BOOT_DONE        "AXX+BOT+DON\n"
#define CMD_MODE_WIFI        "AXX+PLM+000\n"
#define CMD_MODE_AUX         "AXX+PLM+001\n"
#define CMD_MODE_BT          "AXX+PLM+002\n"
#define CMD_POWER_ON         "AXX+POW+000\n"  /* Stock: 000 = Wakeup from Standby */
#define CMD_STANDBY          "AXX+POW+001\n"  /* Stock: 001 = Low power standby */
#define CMD_POWER_OFF        "AXX+POW+OFF\n"  /* Stock: AXX+POW+OFF cuts secondary rail */
#define CMD_AUDIOPRO_WAKE_M  "AXX+M2S+007\n"  /* AudioPro Master Hardware Wake sync */
#define CMD_AUDIOPRO_WAKE_S  "AXX+S2M+007\n"  /* AudioPro Slave Hardware Wake sync */
#define CMD_UNMUTE           "AXX+MUT+000\n"
#define CMD_MUTE             "AXX+MUT+001\n"
#define CMD_STA_CONN         "AXX+STA+001\n"  /* Verified Stock: 001 = NET_CONNECTED (Solid ON) */
#define CMD_STA_CONNECTING   "AXX+STA+002\n"  /* Verified Stock: 002 = NET_CONNECTING (Blinking 2Hz) */
#define CMD_STA_DISCONN      "AXX+STA+000\n"  /* Verified Stock: 000 = NET_DISCONNECTED (OFF) */
#define CMD_RA0_LINK         "AXX+RA0+001\n"  /* SoftAP Mode Fast Blink / Breathing */
#define CMD_RA0_DISCONN      "AXX+RA0+000\n"  /* SoftAP Off */
#define CMD_WPS_PAIRING      "AXX+WPS+001\n"  /* SmartConfig Breathing */
#define CMD_WPS_END          "AXX+WPS+END\n"  /* WPS pairing complete */
#define CMD_WWW_CONN         "AXX+WWW+001\n"  /* Internet WAN Online */
#define CMD_WWW_DISCONN      "AXX+WWW+000\n"  /* Internet WAN Offline */
#define CMD_ETH_CONN         "AXX+ETH+001\n"  /* uplink is wired */
#define CMD_ETH_DISCONN      "AXX+ETH+000\n"
#define CMD_PLAY_START       "AXX+PLY+001\n"
#define CMD_PLAY_STOP        "AXX+PLY+000\n"

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

static volatile sig_atomic_t g_running = 1;
static int g_uart_fd = -1;
static int g_fifo_fd = -1;
static int g_wdt_fd = -1;
static snd_mixer_t *g_mixer = NULL;
static snd_mixer_elem_t *g_master_elem = NULL;
static struct uloop_fd *g_mixer_ufds = NULL;
static int g_mixer_ufd_count = 0;
static struct mosquitto *g_mosq = NULL;
static struct ubus_context *g_ubus_ctx = NULL;

static struct uloop_fd g_uart_ufd;
static struct uloop_fd g_fifo_ufd;
static struct uloop_timeout g_wdt_timer;
static struct uloop_timeout g_sleep_timer;

static log_level_t g_log_level = LOG_LEVEL_INFO;
static char g_mqtt_host[128] = "";   /* opt-in: no broker unless configured */
static int  g_mqtt_port = 1883;
static char g_mqtt_user[64] = "";
static char g_mqtt_pass[64] = "";
static char g_topic_prefix[64] = "audiopro_c3";
static int  g_mqtt_enabled = 1;

static int  g_user_vol = 25;         /* Master volume 0..100, mirrors the ALSA "Master" control */
static int  g_is_muted = 0;          /* Hardware mute state */
static int  g_current_source = 0;    /* 0: wifi/i2s, 1: bluetooth, 2: aux */
static int  g_battery_pct = 100;     /* Battery percentage (0..100) */
static int  g_is_charging = 0;       /* 1 = Charging (AC plugged), 0 = Battery */
static int  g_auto_sleep_min = 0;    /* 0 = Always-On, >0 = timeout in minutes */
static int  g_net_state = -1;        /* Cached network state: 0=disconn, 1=conn, 2=connecting, 3=ap, 4=wps */
/* The MCU keeps four independent state variables and polls us for each one with
 * MCU+<X>+GET. Stock mv_ioguard answers from its own mirror, so we need one too -
 * a single enum cannot express "setup AP is up AND ethernet is linked", which is
 * exactly this speaker's normal state. Values are stock's: sta 0=disconnected
 * 1=connected 2=connecting, ra0 0=AP no client 1=client joined 2=client left,
 * www/eth 0=down 1=up. */
static int  g_sta = 0;
static int  g_ra0 = 0;
static int  g_www = 0;
static int  g_eth = 0;
static int  g_sleep_in_aux = 0;      /* 0 = keep awake in AUX (default), 1 = timer sleep */
static int  g_sleep_in_bt = 0;       /* 0 = keep awake in BT (default), 1 = timer sleep */
static int  g_sound_prompts_enabled = 0; /* 0 = Disabled to avoid ALSA audio sink deadlock */
static int  g_is_playing = 0;        /* 1 = Audio stream running, 0 = Stopped */
static time_t g_last_activity_time = 0;
static int64_t g_last_button_ms = 0;

static time_t g_mcu_link_ts = 0;      /* when we last ran the MCU handshake */
static char g_mcu_ver[16] = "";
static time_t g_last_vol_pub_sec = 0;
static int    g_pending_vol_pub = -1;
static int    g_last_bat = 100;

#define LOG_ERROR(fmt, ...) do { if (g_log_level >= LOG_LEVEL_ERROR) fprintf(stderr, "[mcud ERROR] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_WARN(fmt, ...)  do { if (g_log_level >= LOG_LEVEL_WARN)  fprintf(stderr, "[mcud WARN] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)  do { if (g_log_level >= LOG_LEVEL_INFO)  fprintf(stdout, "[mcud INFO] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...) do { if (g_log_level >= LOG_LEVEL_DEBUG) fprintf(stdout, "[mcud DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)

static int uart_init(void);
static void graceful_shutdown(void);
static void set_audio_source(int source);
static void set_user_volume(int vol);
static void mqtt_publish_volume(int vol);
static void adjust_user_volume(int delta);
static void set_hardware_mute(int mute);
static const char *get_source_name(int source);
static void mcu_report_state(const char *tag, int val);
static void vol_touch(void);

static int uart_send(const char *cmd) {
    if (g_uart_fd < 0 || !cmd) return -1;
    size_t len = strlen(cmd);
    /* RX was logged and TX was not, which made every "did the MCU get it?"
     * question unanswerable. Trailing \n stripped so lines stay one-per-line. */
    if (g_log_level >= LOG_LEVEL_DEBUG) {
        char pretty[64];
        size_t n = len < sizeof(pretty) - 1 ? len : sizeof(pretty) - 1;
        memcpy(pretty, cmd, n);
        while (n && (pretty[n - 1] == '\n' || pretty[n - 1] == '\r')) n--;
        pretty[n] = '\0';
        LOG_DEBUG("MCU TX: [%s]", pretty);
    }
    ssize_t written = write(g_uart_fd, cmd, len);
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            LOG_WARN("UART write would block, retrying once...");
            usleep(20000);
            written = write(g_uart_fd, cmd, len);
        }
        if (written < 0) {
            LOG_WARN("UART connection lost, reopening %s...", SERIAL_PORT);
            close(g_uart_fd);
            g_uart_fd = uart_init();
            if (g_uart_fd >= 0) {
                written = write(g_uart_fd, cmd, len);
            }
        }
    }
    return (written == (ssize_t)len) ? 0 : -1;
}

static void play_sound(const char *path) {
    if (!g_sound_prompts_enabled || !path || access(path, R_OK) != 0) return;
    pid_t pid = fork();
    if (pid < 0) {
        LOG_WARN("Failed to fork for play_sound: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        /* timeout(1) в образе нет - раньше execlp падал и звук не играл никогда; alarm переживает exec */
        alarm(8);
        /* через dmix-слот, а не plughw:0,0 - прямой hw занят пока что-то играет (EBUSY) */
        execlp("mpg123", "mpg123", "-q", "-a", "tts_in", "--", path, (char *)NULL);
        _exit(1);
    }
}

static void watchdog_ping(void) {
    if (g_wdt_fd < 0) {
        g_wdt_fd = open("/dev/watchdog", O_WRONLY | O_NONBLOCK);
    }
    if (g_wdt_fd >= 0) {
        write(g_wdt_fd, "1", 1);
    }
}

static const char *get_source_name(int source) {
    switch (source) {
        case 0: return "wifi";
        case 1: return "bluetooth";
        case 2: return "aux";
        default: return "unknown";
    }
}

/* Checks if audio is playing or if external inputs (AUX/BT) are active */
static int is_audio_active(void) {
    if (g_current_source == 2 && !g_sleep_in_aux) return 1;
    if (g_current_source == 1 && !g_sleep_in_bt) return 1;

    const char *status_paths[] = {
        "/proc/asound/card0/pcm0p/sub0/status",
        "/proc/asound/card1/pcm0p/sub0/status",
        NULL
    };

    for (int i = 0; status_paths[i]; i++) {
        int fd = open(status_paths[i], O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            char buf[128];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "RUNNING") || strstr(buf, "DRAINING")) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void ubus_publish_event(const char *event_name, struct blob_attr *msg) {
    if (!g_ubus_ctx) return;
    ubus_send_event(g_ubus_ctx, event_name, msg);
}

static void ubus_notify_button(const char *btn_name) {
    struct blob_buf b;
    memset(&b, 0, sizeof(b));
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "button", btn_name);
    ubus_publish_event("mcud.button", b.head);
    blob_buf_free(&b);
}

static void ubus_notify_battery(int bat_pct, int is_charging) {
    struct blob_buf b;
    memset(&b, 0, sizeof(b));
    blob_buf_init(&b, 0);
    blobmsg_add_u32(&b, "battery", bat_pct);
    blobmsg_add_u8(&b, "charging", is_charging ? 1 : 0);
    ubus_publish_event("mcud.battery", b.head);
    blob_buf_free(&b);
}

static void ubus_notify_source(const char *src_name) {
    struct blob_buf b;
    memset(&b, 0, sizeof(b));
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "source", src_name);
    ubus_publish_event("mcud.source", b.head);
    blob_buf_free(&b);
}

static void graceful_shutdown(void) {
    LOG_INFO("Initiating graceful shutdown of audio services and system...");

    /* Stop streaming processes first */
    system("killall -TERM librespot shairport-sync squeezelite 2>/dev/null");
    usleep(200000);

    /* Hardware Mute amplifier */
    uart_send(CMD_MUTE);
    usleep(100000);

    /* Battery drain prevention: cut off secondary MCU / amplifier power rail */
    uart_send(CMD_POWER_OFF);
    usleep(150000);

    /* Publish offline state to Home Assistant */
    if (g_mosq && g_mqtt_enabled) {
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/status", g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, 7, "offline", 0, true);
        mosquitto_loop_stop(g_mosq, true);
    }

    sync();
    system("/sbin/poweroff");
}

/* Percent is the unit everyone here speaks - the MCU bookkeeps it, MQTT and the
 * ubus api report it - but the Master softvol is dB-linear: raw 0..1000 spans
 * -60..0 dB. Mapping percent straight onto raw made the dial a pure dB ladder
 * where 25% sat at -45 dB, which is why the bottom half sounded dead. librespot
 * maps its own slider with CubicMapping plus alsa's antilog correction, so both
 * ends have to agree on one law or they disagree about what the level is: raw
 * 513 read back through the old (v-min)*100/(max-min) came out as 51% while
 * Spotify was showing 25%, and that wrong number went to the MCU, to MQTT and
 * into /etc/mcud.volume.
 *
 * Table is 1000 * (1 + ln((0.009*pct + 0.1)^3) / ln(1000)), i.e. exactly
 * librespot's cubic over a 60 dB range, precomputed so this stays integer-only
 * and mcud does not have to pull in libm. */
static const unsigned short vol_curve[101] = {
	   0,   37,   72,  104,  134,  161,  188,  212,
	 236,  258,  279,  299,  318,  336,  354,  371,
	 387,  403,  418,  433,  447,  461,  474,  487,
	 500,  512,  524,  535,  547,  558,  568,  579,
	 589,  599,  609,  618,  627,  636,  645,  654,
	 663,  671,  679,  688,  695,  703,  711,  719,
	 726,  733,  740,  747,  754,  761,  768,  775,
	 781,  787,  794,  800,  806,  812,  818,  824,
	 830,  836,  841,  847,  852,  858,  863,  869,
	 874,  879,  884,  889,  894,  899,  904,  909,
	 914,  919,  923,  928,  932,  937,  942,  946,
	 950,  955,  959,  963,  968,  972,  976,  980,
	 984,  988,  992,  996, 1000
};

static long vol_pct_to_raw(int pct, long min, long max) {
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	return min + ((long)vol_curve[pct] * (max - min) + 500) / 1000;
}

/* Nearest percent rather than a truncation: librespot writes whatever its own
 * rounding produced, which lands a point or two off our grid. */
static int vol_raw_to_pct(long v, long min, long max) {
	if (max <= min) return -1;
	long permille = ((v - min) * 1000 + (max - min) / 2) / (max - min);
	int lo = 0, hi = 100;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (vol_curve[mid] < permille) lo = mid + 1; else hi = mid;
	}
	if (lo > 0 && permille - vol_curve[lo - 1] < vol_curve[lo] - permille) lo--;
	return lo;
}

/* Attenuation is software-only here: the PCM5102A has no volume register and the
 * TPA3116 gain is pin-strapped, so the ALSA "Master" softvol is the only thing
 * that can change the level. Stock did the same, just inside its own writei
 * wrapper instead of a plugin. */
static void alsa_set_master(int vol) {
    if (!g_master_elem) return;
    long min = 0, max = 100;
    if (snd_mixer_selem_get_playback_volume_range(g_master_elem, &min, &max) < 0) return;
    long v = vol_pct_to_raw(vol, min, max);
    if (snd_mixer_selem_set_playback_volume_all(g_master_elem, v) < 0)
        LOG_WARN("Failed to set ALSA Master to %d%%", vol);
}

/* softvol picks its gain up between transfer chunks (10-25 ms here), so a 5% key
 * press lands as a 3 dB step in the middle of the waveform - that is the click
 * you hear when turning the volume on a sustained note. Walk there in six pieces
 * instead: 0.5 dB each at one press, quiet enough to vanish into the music, and
 * the whole move still lands well inside the panel's 120 ms debounce. */
#define VOL_RAMP_MS     20
#define VOL_RAMP_TICKS  6

static struct uloop_timeout g_vol_ramp;
static long g_ramp_target = -1;   /* raw, -1 when no ramp is in flight */
static long g_ramp_step;

static void vol_ramp_cb(struct uloop_timeout *t) {
    (void)t;
    long cur = 0;
    if (g_ramp_target < 0 || !g_master_elem) return;
    if (snd_mixer_selem_get_playback_volume(g_master_elem, SND_MIXER_SCHN_FRONT_LEFT, &cur) < 0) {
        g_ramp_target = -1;
        return;
    }
    long next = cur + g_ramp_step;
    if ((g_ramp_step > 0 && next >= g_ramp_target) ||
        (g_ramp_step < 0 && next <= g_ramp_target)) {
        next = g_ramp_target;
        g_ramp_target = -1;
    }
    if (snd_mixer_selem_set_playback_volume_all(g_master_elem, next) < 0) {
        LOG_WARN("Failed to set ALSA Master to raw %ld", next);
        g_ramp_target = -1;
        return;
    }
    if (g_ramp_target >= 0)
        uloop_timeout_set(&g_vol_ramp, VOL_RAMP_MS);
}

static void alsa_ramp_master(int vol) {
    if (!g_master_elem) return;
    long min = 0, max = 100, cur = 0;
    if (snd_mixer_selem_get_playback_volume_range(g_master_elem, &min, &max) < 0) return;
    if (snd_mixer_selem_get_playback_volume(g_master_elem, SND_MIXER_SCHN_FRONT_LEFT, &cur) < 0) {
        alsa_set_master(vol);
        return;
    }
    long target = vol_pct_to_raw(vol, min, max);
    long delta = target - cur;
    if (!delta) {
        g_ramp_target = -1;
        return;
    }
    g_ramp_step = delta / VOL_RAMP_TICKS;
    if (!g_ramp_step) g_ramp_step = delta > 0 ? 1 : -1;
    g_ramp_target = target;
    g_vol_ramp.cb = vol_ramp_cb;
    vol_ramp_cb(&g_vol_ramp);     /* first piece now, the rest on the timer */
}

static int alsa_get_master(void) {
    if (!g_master_elem) return -1;
    long min = 0, max = 100, v = 0;
    if (snd_mixer_selem_get_playback_volume_range(g_master_elem, &min, &max) < 0) return -1;
    if (snd_mixer_selem_get_playback_volume(g_master_elem, SND_MIXER_SCHN_FRONT_LEFT, &v) < 0) return -1;
    return vol_raw_to_pct(v, min, max);
}

/* Someone else moved the control - librespot's alsa mixer, amixer, ha_ducking.
 * Follow it so the MCU and MQTT stay in sync. Our own writes land here too, but
 * the value already matches g_user_vol by then so they stop right here. */
static void mixer_event_cb(struct uloop_fd *ufd, unsigned int events) {
    (void)ufd;
    (void)events;
    if (!g_mixer) return;
    snd_mixer_handle_events(g_mixer);
    if (g_ramp_target >= 0) return;   /* mid-ramp, this is our own write */

    int vol = alsa_get_master();
    if (vol < 0 || vol == g_user_vol) return;

    g_user_vol = vol;
    mcu_report_state("VOL", g_user_vol);
    mqtt_publish_volume(g_user_vol);
    vol_touch();
    LOG_INFO("Master volume changed externally: %d%%", g_user_vol);
}

/* softvol controls are created when the PCM is first opened and don't survive a
 * reboot, so open "master" once just to make the control exist. */
static void alsa_probe_master_pcm(void) {
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, "master", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        LOG_WARN("Cannot open ALSA 'master' (%s) - volume control will be unavailable", snd_strerror(err));
        return;
    }
    snd_pcm_close(pcm);
}

static void alsa_init(void) {
    if (g_mixer) return;
    alsa_probe_master_pcm();

    if (snd_mixer_open(&g_mixer, 0) < 0) return;
    if (snd_mixer_attach(g_mixer, "default") < 0 ||
        snd_mixer_selem_register(g_mixer, NULL, NULL) < 0 ||
        snd_mixer_load(g_mixer) < 0) {
        snd_mixer_close(g_mixer);
        g_mixer = NULL;
        return;
    }

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);

    /* Per-app stages exist only for ducking - park them at 0 dB so they are a
     * plain memcpy until ha_ducking.sh pulls one down. */
    const char *names[] = {"Spotify", "AirPlay", "Music", "TTS", "VoIP", "Alarm", "Timer", "Squeeze", NULL};
    for (int i = 0; names[i]; i++) {
        snd_mixer_selem_id_set_name(sid, names[i]);
        snd_mixer_elem_t *elem = snd_mixer_find_selem(g_mixer, sid);
        if (elem) {
            long min = 0, max = 100;
            snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
            snd_mixer_selem_set_playback_volume_all(elem, max);
        }
    }

    snd_mixer_selem_id_set_name(sid, "Master");
    g_master_elem = snd_mixer_find_selem(g_mixer, sid);
    if (!g_master_elem) {
        LOG_WARN("ALSA 'Master' control missing - check pcm.master in /etc/asound.conf");
        return;
    }

    int n = snd_mixer_poll_descriptors_count(g_mixer);
    if (n > 0) {
        struct pollfd *pfds = alloca(sizeof(*pfds) * n);
        n = snd_mixer_poll_descriptors(g_mixer, pfds, n);
        if (n > 0) {
            g_mixer_ufds = calloc(n, sizeof(*g_mixer_ufds));
            if (g_mixer_ufds) {
                for (int i = 0; i < n; i++) {
                    g_mixer_ufds[i].fd = pfds[i].fd;
                    g_mixer_ufds[i].cb = mixer_event_cb;
                    uloop_fd_add(&g_mixer_ufds[i], ULOOP_READ);
                }
                g_mixer_ufd_count = n;
            }
        }
    }
    LOG_INFO("ALSA Master volume control ready (cubic, 25%% = -29.3 dB, 0 = mute)");
}

static void mqtt_publish_volume(int vol) {
    if (!g_mosq || !g_mqtt_enabled) return;
    time_t now = time(NULL);
    if (now == g_last_vol_pub_sec) {
        g_pending_vol_pub = vol;
        return;
    }

    char topic[128], payload[16];
    snprintf(topic, sizeof(topic), "%s/player/volume", g_topic_prefix);
    snprintf(payload, sizeof(payload), "%d", vol);
    mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    g_last_vol_pub_sec = now;
    g_pending_vol_pub = -1;
}

static void set_hardware_mute(int mute) {
    g_is_muted = mute ? 1 : 0;
    if (g_is_muted) {
        uart_send(CMD_MUTE);
        LOG_INFO("Master Hardware MUTE engaged");
    } else {
        uart_send(CMD_UNMUTE);
        LOG_INFO("Master Hardware MUTE disengaged");
    }
}

/* Stock keeps the volume in nvram; we had nothing, so every restart slammed the
 * level back to default_volume. Debounced because this is NOR flash - a volume
 * ramp must not turn into one jffs2 node per step. */
#define VOL_STATE_FILE  "/etc/mcud.volume"
#define VOL_PERSIST_MS  10000

static struct uloop_timeout g_vol_persist;

static int vol_load(void) {
    FILE *f = fopen(VOL_STATE_FILE, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1 || v < 0 || v > 100) v = -1;
    fclose(f);
    return v;
}

static void vol_persist_cb(struct uloop_timeout *t) {
    (void)t;
    if (vol_load() == g_user_vol) return;      /* nothing to write */
    FILE *f = fopen(VOL_STATE_FILE, "w");
    if (!f) { LOG_WARN("Cannot persist volume to %s", VOL_STATE_FILE); return; }
    fprintf(f, "%d\n", g_user_vol);
    fclose(f);
}

static void vol_touch(void) {
    g_vol_persist.cb = vol_persist_cb;
    uloop_timeout_set(&g_vol_persist, VOL_PERSIST_MS);
}

static void set_user_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    g_user_vol = vol;
    alsa_ramp_master(g_user_vol);

    /* Raw 0..100, exactly as stock sent it. The MCU only bookkeeps the number so
     * its own +/- buttons start from the right base - it does not attenuate. */
    mcu_report_state("VOL", g_user_vol);
    mqtt_publish_volume(g_user_vol);
    vol_touch();
    if (g_user_vol == 0)
        LOG_INFO("Master volume: 0%% (muted)");
    else {
        int dbt = (vol_curve[g_user_vol] * 6) / 10 - 600;   /* tenths of a dB */
        LOG_INFO("Master volume: %d%% (%d.%d dB)", g_user_vol, dbt / 10, (-dbt) % 10);
    }
}

/* A key press has to move a fixed number of dB, not a fixed percent: on the
 * cubic curve 0->5% is nearly 10 dB while 95->100% is barely one. raw is
 * dB-linear, so step there and convert back to the percent everyone reports.
 * Only the sign of delta matters now. */
static void adjust_user_volume(int delta) {
    long min = 0, max = 100;
    if (!g_master_elem ||
        snd_mixer_selem_get_playback_volume_range(g_master_elem, &min, &max) < 0) {
        set_user_volume(g_user_vol + (delta > 0 ? VOL_STEP_DB : -VOL_STEP_DB));
        return;
    }
    long step = ((long)VOL_STEP_DB * (max - min)) / 60;
    long raw = vol_pct_to_raw(g_user_vol, min, max) + (delta > 0 ? step : -step);
    if (raw < min) raw = min;
    if (raw > max) raw = max;
    int pct = vol_raw_to_pct(raw, min, max);
    /* the curve is coarse down low, so a 3 dB step can round back onto the
     * percent we started from - nudge past it or the key would do nothing */
    if (pct == g_user_vol) pct += (delta > 0 ? 1 : -1);
    set_user_volume(pct);
}

static void mqtt_on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg) {
    (void)mosq;
    (void)userdata;
    if (!msg || !msg->topic || !msg->payload) return;

    char payload[64];
    int len = msg->payloadlen;
    if (len <= 0 || len >= (int)sizeof(payload)) return;
    memcpy(payload, msg->payload, len);
    payload[len] = '\0';

    char cmd_topic[128], vol_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/player/command", g_topic_prefix);
    snprintf(vol_topic, sizeof(vol_topic), "%s/player/volume/set", g_topic_prefix);

    if (strcmp(msg->topic, cmd_topic) == 0) {
        LOG_INFO("MQTT Command received: [%s]", payload);
        g_last_activity_time = time(NULL);
        if (strcasecmp(payload, "PLAY") == 0 || strcasecmp(payload, "UNMUTE") == 0) {
            uart_send(CMD_MODE_WIFI);
            set_hardware_mute(0);
        } else if (strcasecmp(payload, "PAUSE") == 0 || strcasecmp(payload, "STOP") == 0 || strcasecmp(payload, "MUTE") == 0) {
            set_hardware_mute(1);
        } else if (strcasecmp(payload, "TOGGLE") == 0) {
            system("/usr/bin/player_control.sh toggle >/dev/null 2>&1 &");
        }
    } else if (strcmp(msg->topic, vol_topic) == 0) {
        g_last_activity_time = time(NULL);
        int vol = atoi(payload);
        set_user_volume(vol);
    }
}

static void mqtt_on_disconnect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq;
    (void)userdata;
    if (rc != 0) {
        LOG_WARN("MQTT disconnected unexpectedly (rc=%d), will auto-reconnect...", rc);
    } else {
        LOG_INFO("MQTT disconnected cleanly");
    }
}

static void mqtt_send_discovery(void);

/* Discovery has to go out from here, not from mqtt_setup(): connect_async() returns
 * before the socket is up, so publishing there hit MOSQ_ERR_NO_CONN and Home Assistant
 * never saw a single entity. Doing it on every connect also re-announces after a broker
 * restart, which is what HA needs. */
static void mqtt_on_connect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq;
    (void)userdata;
    if (rc != 0) {
        LOG_WARN("MQTT connect refused by %s:%d (rc=%d)", g_mqtt_host, g_mqtt_port, rc);
        return;
    }
    LOG_INFO("MQTT connected to %s:%d", g_mqtt_host, g_mqtt_port);
    mqtt_send_discovery();
}

static void mqtt_send_discovery(void) {
    if (!g_mosq || !g_mqtt_enabled) return;

    const char *buttons[] = {
        "preset_1", "preset_2", "preset_3", "preset_4",
        "play_pause", "source", "bluetooth"
    };

    for (int i = 0; i < 7; i++) {
        char topic[256], payload[512];
        snprintf(topic, sizeof(topic), "homeassistant/device_automation/audiopro_c3/%s/config", buttons[i]);
        snprintf(payload, sizeof(payload),
            "{\"automation_type\":\"trigger\","
            "\"type\":\"button_short_press\","
            "\"subtype\":\"%s\","
            "\"topic\":\"%s/button/%s\","
            "\"payload\":\"pressed\","
            "\"device\":{\"identifiers\":[\"audiopro_c3\"],"
            "\"name\":\"Audio Pro C3\","
            "\"manufacturer\":\"Audio Pro\","
            "\"model\":\"Addon C3 (Linkplay A28)\","
            "\"sw_version\":\"OpenWrt 23.05.5\"}}",
            buttons[i], g_topic_prefix, buttons[i]);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        char topic[256], payload[512];
        snprintf(topic, sizeof(topic), "homeassistant/sensor/audiopro_c3/battery/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Audio Pro C3 Battery\","
            "\"device_class\":\"battery\","
            "\"state_class\":\"measurement\","
            "\"unit_of_measurement\":\"%%\","
            "\"state_topic\":\"%s/sensor/battery\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_battery\","
            "\"device\":{\"identifiers\":[\"audiopro_c3\"],"
            "\"name\":\"Audio Pro C3\"}}",
            g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        char topic[256], payload[512];
        snprintf(topic, sizeof(topic), "homeassistant/sensor/audiopro_c3/source/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Audio Pro C3 Source\","
            "\"icon\":\"mdi:volume-source\","
            "\"state_topic\":\"%s/sensor/source\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_source\","
            "\"device\":{\"identifiers\":[\"audiopro_c3\"],"
            "\"name\":\"Audio Pro C3\"}}",
            g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        char topic[256], payload[512];
        snprintf(topic, sizeof(topic), "homeassistant/number/audiopro_c3/volume/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Audio Pro C3 Volume\","
            "\"icon\":\"mdi:volume-high\","
            "\"state_topic\":\"%s/player/volume\","
            "\"command_topic\":\"%s/player/volume/set\","
            "\"availability_topic\":\"%s/status\","
            "\"min\":0,\"max\":100,\"step\":1,\"unit_of_measurement\":\"%%\","
            "\"unique_id\":\"audiopro_c3_volume\","
            "\"device\":{\"identifiers\":[\"audiopro_c3\"],"
            "\"name\":\"Audio Pro C3\"}}",
            g_topic_prefix, g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    char sub_cmd[128], sub_vol[128];
    snprintf(sub_cmd, sizeof(sub_cmd), "%s/player/command", g_topic_prefix);
    snprintf(sub_vol, sizeof(sub_vol), "%s/player/volume/set", g_topic_prefix);
    mosquitto_subscribe(g_mosq, NULL, sub_cmd, 0);
    mosquitto_subscribe(g_mosq, NULL, sub_vol, 0);

    char status_topic[128];
    snprintf(status_topic, sizeof(status_topic), "%s/status", g_topic_prefix);
    mosquitto_publish(g_mosq, NULL, status_topic, 6, "online", 0, true);

    LOG_INFO("MQTT Home Assistant discovery published, %s/player/# subscribed", g_topic_prefix);
}

static void mqtt_send_button(const char *name) {
    if (!g_mosq || !g_mqtt_enabled) return;
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/button/%s", g_topic_prefix, name);
    mosquitto_publish(g_mosq, NULL, topic, 7, "pressed", 0, false);
}

static void mqtt_send_sensor(const char *sensor, const char *val) {
    if (!g_mosq || !g_mqtt_enabled) return;
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/sensor/%s", g_topic_prefix, sensor);
    mosquitto_publish(g_mosq, NULL, topic, strlen(val), val, 0, true);
}

static int mqtt_setup(void) {
    if (!g_mqtt_enabled) return 0;
    /* shipped default is an empty host: no broker configured means stay off rather than
     * reconnect at a phantom address forever */
    if (g_mqtt_host[0] == '\0') {
        g_mqtt_enabled = 0;
        LOG_INFO("MQTT disabled: no broker host configured");
        return 0;
    }
    mosquitto_lib_init();
    g_mosq = mosquitto_new("audiopro_c3_mcud", true, NULL);
    if (!g_mosq) return -1;
    if (strlen(g_mqtt_user) > 0) {
        mosquitto_username_pw_set(g_mosq, g_mqtt_user, strlen(g_mqtt_pass) > 0 ? g_mqtt_pass : NULL);
    }
    mosquitto_message_callback_set(g_mosq, mqtt_on_message);
    mosquitto_disconnect_callback_set(g_mosq, mqtt_on_disconnect);
    mosquitto_connect_callback_set(g_mosq, mqtt_on_connect);
    /* the poweroff path publishes "offline" itself, but a crash, kill or Wi-Fi drop
     * would leave HA believing we are online forever - that is what the will covers */
    {
        char will_topic[128];
        snprintf(will_topic, sizeof(will_topic), "%s/status", g_topic_prefix);
        mosquitto_will_set(g_mosq, will_topic, 7, "offline", 0, true);
    }
    /* without this libmosquitto retries once a second forever - pointless on a box
     * where CPU is the scarce resource and the broker may simply not exist */
    mosquitto_reconnect_delay_set(g_mosq, 2, 60, true);
    int rc = mosquitto_connect_async(g_mosq, g_mqtt_host, g_mqtt_port, 60);
    if (rc != MOSQ_ERR_SUCCESS)
        LOG_WARN("MQTT initial connect to %s:%d failed (%s), retrying in background",
                 g_mqtt_host, g_mqtt_port, mosquitto_strerror(rc));
    mosquitto_loop_start(g_mosq);
    return 0;
}

static int fifo_init(void) {
    unlink(CMD_FIFO_PATH);
    if (mkfifo(CMD_FIFO_PATH, 0666) < 0 && errno != EEXIST) {
        LOG_ERROR("Failed to create command FIFO %s: %s", CMD_FIFO_PATH, strerror(errno));
        return -1;
    }
    g_fifo_fd = open(CMD_FIFO_PATH, O_RDWR | O_NONBLOCK);
    if (g_fifo_fd < 0) {
        LOG_ERROR("Failed to open command FIFO %s: %s", CMD_FIFO_PATH, strerror(errno));
        return -1;
    }
    return g_fifo_fd;
}

static int uart_init(void) {
    int fd = open(SERIAL_PORT, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        LOG_ERROR("Failed to open %s: %s", SERIAL_PORT, strerror(errno));
        return -1;
    }
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) { close(fd); return -1; }
    cfsetospeed(&tty, BAUD_RATE);
    cfsetispeed(&tty, BAUD_RATE);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8 | CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG | IEXTEN);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &tty) != 0) { close(fd); return -1; }
    return fd;
}

static void set_audio_source(int source) {
    g_current_source = source % 3;
    g_last_activity_time = time(NULL);
    const char *src_name = get_source_name(g_current_source);

    mqtt_send_sensor("source", src_name);
    ubus_notify_source(src_name);

    int fd = open("/tmp/audio_source", O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
    if (fd >= 0) {
        write(fd, src_name, strlen(src_name));
        write(fd, "\n", 1);
        close(fd);
    }

    if (g_current_source == 0) {
        uart_send(CMD_BOOT_DONE);
        usleep(50000);
        /* no STA/WWW here: picking an input says nothing about the uplink, and
         * faking "connected" is what made the wifi LED lie. Stock only sends PLM. */
        uart_send(CMD_POWER_ON);
        uart_send(CMD_AUDIOPRO_WAKE_M);
        uart_send(CMD_MODE_WIFI);
        set_hardware_mute(0);
        play_sound(SOUND_WIFI_CONN);
        LOG_INFO("Audio source switched: Wi-Fi / I2S (AXX+PLM+000)");
    } else if (g_current_source == 1) {
        uart_send(CMD_BOOT_DONE);
        usleep(50000);
        uart_send(CMD_POWER_ON);
        uart_send(CMD_AUDIOPRO_WAKE_M);
        uart_send(CMD_MODE_BT);
        set_hardware_mute(0);
        play_sound(SOUND_BT_CONN);
        LOG_INFO("Audio source switched: Bluetooth BT2 (AXX+PLM+002)");
    } else {
        uart_send(CMD_BOOT_DONE);
        usleep(50000);
        uart_send(CMD_POWER_ON);
        uart_send(CMD_AUDIOPRO_WAKE_M);
        uart_send(CMD_MODE_AUX);
        set_hardware_mute(0);
        LOG_INFO("Audio source switched: AUX Line-In (AXX+PLM+001)");
    }
}

/* Lowest-metric default route straight out of procfs - no fork, and it answers
 * both questions at once: is there an uplink at all, and is it the wired one.
 * Needed at startup because the old code just asserted "connected" to the MCU. */
static int default_route_iface(char *out, size_t n) {
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return 0;
    char line[256], iface[32];
    unsigned long dest, mask;
    int metric, best = -1, found = 0;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }   /* header */
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%31s %lx %*x %*x %*d %*d %d %lx",
                   iface, &dest, &metric, &mask) == 4
            && dest == 0 && mask == 0 && (best < 0 || metric < best)) {
            best = metric;
            snprintf(out, n, "%s", iface);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* Answer one of the four state polls. Stock builds these in MCUCommandSend
 * cases 4..8 out of an int mirror and replies on the channel the query came in
 * on; we only have the one UART, so the tag is all we need. */
static void mcu_report_state(const char *tag, int val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "AXX+%s+%03d\n", tag, val);
    uart_send(buf);
}

static void process_mcu_command(const char *cmd) {
    LOG_DEBUG("MCU RX: [%s]", cmd);
    g_last_activity_time = time(NULL);

    /* The MCU polls us for state and waits for an answer. Ignoring these is why
     * the front LEDs used to sit in whatever state the MCU last guessed. Codes
     * and the pick-a-string logic come from stock mv_ioguard: uart_cmd_parse
     * turns MCU+STA+GET into event 522 (RA0 526, ETH 527, WWW 528, MIC 529) and
     * ParseCommonMCUEvent answers each from its own mirror. */
    if (strstr(cmd, "MCU+STA+GET")) {
        mcu_report_state("STA", g_sta);
        return;
    } else if (strstr(cmd, "MCU+RA0+GET")) {
        mcu_report_state("RA0", g_ra0);
        return;
    } else if (strstr(cmd, "MCU+WWW+GET")) {
        mcu_report_state("WWW", g_www);
        return;
    } else if (strstr(cmd, "MCU+ETH+GET")) {
        mcu_report_state("ETH", g_eth);
        return;
    } else if (strstr(cmd, "MCU+MIC+GET")) {
        /* stock answers 001 only when /dev/dsp1 exists; this board has no
         * capture device, so the honest answer is 000 */
        mcu_report_state("MIC", access("/dev/dsp1", F_OK) == 0 ? 1 : 0);
        return;
    } else if (strstr(cmd, "MCU+SLV+CHK") || strstr(cmd, "MCU+SLV+GET")) {
        /* stock says YES only for a speaker that joined a group as slave;
         * we are always standalone */
        uart_send("AXX+SLV+NOT\n");
        return;
    } else if (strstr(cmd, "MCU+VOL+GET")) {
        /* stock's AXX+VOL+00%d / 0%d / 100 triple is just %03d */
        mcu_report_state("VOL", g_user_vol);
        return;
    } else if (strstr(cmd, "MCU+PLM+GET")) {
        uart_send(g_current_source == 1 ? CMD_MODE_BT :
                  g_current_source == 2 ? CMD_MODE_AUX : CMD_MODE_WIFI);
        return;
    }

    if (!strncmp(cmd, "MCU+VER+", 8)) {
        snprintf(g_mcu_ver, sizeof(g_mcu_ver), "%.15s", cmd + 8);
        LOG_INFO("MCU firmware version: %s", g_mcu_ver);
        return;
    }

    /* An absolute volume report, as opposed to MCU+VOL+GET. The panel sends
     * MCU+KEY+VOL+/- for real presses, so this is the MCU telling us the number
     * it has been bookkeeping - and it re-quantizes on its own scale (send 049,
     * get 048 back, MCU+VMX+030 says it thinks in 30 steps). Adopting that
     * blindly walked the level down one step per handshake. We own the only
     * attenuator, so within the quantization noise we say "in sync", right after
     * a handshake we re-assert our value, and only an unsolicited jump counts as
     * something the MCU changed on its own. */
    if (!strncmp(cmd, "MCU+VOL+", 8) && isdigit((unsigned char)cmd[8])) {
        int v = atoi(cmd + 8);
        if (v < 0 || v > 100) return;
        int d = v > g_user_vol ? v - g_user_vol : g_user_vol - v;
        if (d <= MCU_VOL_TOLERANCE) {
            LOG_DEBUG("MCU volume %d%% vs ours %d%%, within MCU quantization", v, g_user_vol);
        } else if (time(NULL) - g_mcu_link_ts <= 5) {
            LOG_INFO("MCU came up holding %d%%, re-asserting %d%%", v, g_user_vol);
            mcu_report_state("VOL", g_user_vol);
        } else {
            LOG_INFO("Volume changed on the MCU side: %d%%", v);
            g_user_vol = v;
            alsa_set_master(v);
            mqtt_publish_volume(v);
            vol_touch();
        }
        return;
    }

    if (strstr(cmd, "MCU+KEY+")) {
        /* Mechanical Debounce only for hardware button presses */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if ((now_ms - g_last_button_ms) < BUTTON_DEBOUNCE_MS) {
            return;
        }
        g_last_button_ms = now_ms;
    }

    if (strstr(cmd, "MCU+KEY+VOL+")) {
        adjust_user_volume(+1);
        ubus_notify_button("vol_up");
    } else if (strstr(cmd, "MCU+KEY+VOL-")) {
        adjust_user_volume(-1);
        ubus_notify_button("vol_down");
    } else if (strstr(cmd, "MCU+KEY+PLPA")) {
        mqtt_send_button("play_pause");
        ubus_notify_button("play_pause");
        /* O_CREAT on /tmp/player_cmd just made a plain file nothing ever read,
         * so this key did nothing at all until now */
        system("/usr/bin/player_control.sh toggle >/dev/null 2>&1 &");
    } else if (strstr(cmd, "MCU+KEY+PRE:")) {
        const char *p = strstr(cmd, "MCU+KEY+PRE:") + 12;
        int preset = atoi(p);
        if (preset >= 1 && preset <= 4) {
            char pstr[16];
            snprintf(pstr, sizeof(pstr), "preset_%d", preset);
            mqtt_send_button(pstr);
            ubus_notify_button(pstr);
            play_sound(SOUND_PRESET);
            char hcmd[64];
            snprintf(hcmd, sizeof(hcmd), "/usr/bin/audiopro_preset_handler.sh %d >/dev/null 2>&1 &", preset);
            system(hcmd);
        }
    } else if (strstr(cmd, "MCU+KEY+SRC")) {
        mqtt_send_button("source");
        ubus_notify_button("source");
        set_audio_source(g_current_source + 1);
    } else if (strstr(cmd, "MCU+KEY+BT") || strstr(cmd, "MCU+KEY+PAIR")) {
        mqtt_send_button("bluetooth");
        ubus_notify_button("bluetooth");
        set_audio_source(1);
    } else if (strstr(cmd, "MCU+KEY+WIFI")) {
        set_audio_source(0);
    } else if (strstr(cmd, "MCU+KEY+AUX")) {
        set_audio_source(2);
    } else if (strstr(cmd, "MCU+M2S+")) {
        const char *p = strstr(cmd, "MCU+M2S+") + 8;
        LOG_INFO("MCU M2S Event received: [%.3s]", p);
        /* M2S is the multiroom role channel; stock maps GNOTIFY=Master2Slave:<n>
         * onto AXX+M2S+%03d. Code 007 is the AudioPro wake handshake
         * (GNOTIFY=audiopro_wake at 0x426104 in stock mv_ioguard). The MCU walks
         * 008 -> 007 -> 011 on its own and answers 011 to any AXX+MCU+RDY. */
        if (strncmp(p, "011", 3) == 0) {
            uart_send(CMD_AUDIOPRO_WAKE_M);
            uart_send("AXX+M2S+011\n");
        } else if (strncmp(p, "008", 3) == 0) {
            /* wake request: keep the amp rail up, the MCU expects us alive */
            uart_send(CMD_POWER_ON);
        }
    } else if (strstr(cmd, "MCU+S2M+")) {
        const char *p = strstr(cmd, "MCU+S2M+") + 8;
        LOG_INFO("MCU S2M Event received: [%.3s]", p);
    } else if (strstr(cmd, "MCU+CHA+ON")) {
        LOG_INFO("Charger plugged in (MCU+CHA+ON) - Disabling Auto-Sleep timer");
        g_is_charging = 1;
        mqtt_send_sensor("charging", "true");
        ubus_notify_battery(g_battery_pct, g_is_charging);
    } else if (strstr(cmd, "MCU+CHA+OFF")) {
        LOG_INFO("Charger disconnected (MCU+CHA+OFF) - Running on Battery");
        g_is_charging = 0;
        mqtt_send_sensor("charging", "false");
        ubus_notify_battery(g_battery_pct, g_is_charging);
    } else if (strstr(cmd, "MCU+CHA+FULL")) {
        LOG_INFO("Battery Fully Charged (MCU+CHA+FULL)");
        g_is_charging = 1;
        g_battery_pct = 100;
        mqtt_send_sensor("charging", "full");
        ubus_notify_battery(g_battery_pct, g_is_charging);
    } else if (strstr(cmd, "MCU+BAT+OFF")) {
        LOG_WARN("Critical battery undervoltage (MCU+BAT+OFF). Emergency shutdown!");
        g_running = 0;
        graceful_shutdown();
    } else if (strstr(cmd, "MCU+BAT+LOW")) {
        LOG_WARN("Low battery alert (MCU+BAT+LOW)");
        play_sound(SOUND_LOW_BAT);
        mqtt_send_sensor("battery_alert", "low");
    } else if (strstr(cmd, "MCU+BAT+")) {
        const char *p = strstr(cmd, "MCU+BAT+") + 8;
        int bat = atoi(p);
        g_battery_pct = bat;
        char val[16];
        snprintf(val, sizeof(val), "%d", bat);
        mqtt_send_sensor("battery", val);
        ubus_notify_battery(g_battery_pct, g_is_charging);

        if (bat <= 15 && g_last_bat > 15) {
            LOG_WARN("Low battery threshold reached (%d%%)! Playing alert...", bat);
            play_sound(SOUND_LOW_BAT);
            mqtt_send_sensor("battery_alert", "low");
        } else if (bat > 20 && g_last_bat <= 15) {
            mqtt_send_sensor("battery_alert", "normal");
        }
        g_last_bat = bat;
        int fd = open("/tmp/battery_status", O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
        if (fd >= 0) {
            write(fd, cmd, strlen(cmd));
            write(fd, "\n", 1);
            close(fd);
        }
    } else if (strstr(cmd, "MCU+PLY-STP")) {
        /* Stock (ParseCommonMCUEvent event 262) writes "asr_silence_add" to
         * /tmp/RequestASRTTS, "alexaExit" to /tmp/ALEXA_EVENT, then stops the player.
         * That is the Alexa barge-in cancel path. This board has no mic (no /dev/dsp1,
         * MIC+GET answers 000) and no voice assistant, so mirroring the stop would just
         * kill playback on a stray byte. Recognized so it stops showing up as unknown. */
        LOG_DEBUG("MCU+PLY-STP (Alexa cancel) ignored - no voice assistant on this board");
    } else if (strstr(cmd, "MCU+POW+OFF") || strstr(cmd, "MCU+KEY+POW:LONG")) {
        LOG_INFO("MCU reported Power Off event. Initiating graceful shutdown...");
        g_running = 0;
        graceful_shutdown();
    }
}

/* ========================================================================= */
/* UBUS Methods & Handlers                                                   */
/* ========================================================================= */

enum {
    SET_VOL_VOLUME,
    __SET_VOL_MAX
};

static const struct blobmsg_policy set_vol_policy[__SET_VOL_MAX] = {
    [SET_VOL_VOLUME] = { .name = "volume", .type = BLOBMSG_TYPE_INT32 },
};

enum {
    SET_SRC_SOURCE,
    __SET_SRC_MAX
};

static const struct blobmsg_policy set_src_policy[__SET_SRC_MAX] = {
    [SET_SRC_SOURCE] = { .name = "source", .type = BLOBMSG_TYPE_STRING },
};

enum {
    SET_MUTE_MUTE,
    __SET_MUTE_MAX
};

static const struct blobmsg_policy set_mute_policy[__SET_MUTE_MAX] = {
    [SET_MUTE_MUTE] = { .name = "mute", .type = BLOBMSG_TYPE_INT32 },
};

enum {
    SET_NET_STATE,
    SET_NET_LINK,
    SET_NET_ERROR,
    __SET_NET_MAX
};

static const struct blobmsg_policy set_net_policy[__SET_NET_MAX] = {
    [SET_NET_STATE] = { .name = "state", .type = BLOBMSG_TYPE_STRING },
    [SET_NET_LINK]  = { .name = "link",  .type = BLOBMSG_TYPE_STRING },
    [SET_NET_ERROR] = { .name = "error", .type = BLOBMSG_TYPE_STRING },
};

static int mcud_ubus_status(struct ubus_context *ctx, struct ubus_object *obj,
                            struct ubus_request_data *req, const char *method,
                            struct blob_attr *msg) {
    (void)obj;
    (void)method;
    (void)msg;
    struct blob_buf b;
    memset(&b, 0, sizeof(b));
    blob_buf_init(&b, 0);

    blobmsg_add_u8(&b, "power", true);
    blobmsg_add_u32(&b, "volume", g_user_vol);
    blobmsg_add_u32(&b, "mcu_volume", g_user_vol);
    blobmsg_add_string(&b, "mcu_version", g_mcu_ver);
    blobmsg_add_string(&b, "source", get_source_name(g_current_source));
    blobmsg_add_u32(&b, "battery", g_battery_pct);
    blobmsg_add_u8(&b, "charging", g_is_charging ? 1 : 0);
    blobmsg_add_u8(&b, "mute", g_is_muted ? 1 : 0);

    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

static int mcud_ubus_set_volume(struct ubus_context *ctx, struct ubus_object *obj,
                                struct ubus_request_data *req, const char *method,
                                struct blob_attr *msg) {
    (void)obj;
    (void)method;
    struct blob_attr *tb[__SET_VOL_MAX];
    blobmsg_parse(set_vol_policy, __SET_VOL_MAX, tb, blob_data(msg), blob_len(msg));

    if (!tb[SET_VOL_VOLUME]) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    int vol = blobmsg_get_u32(tb[SET_VOL_VOLUME]);
    set_user_volume(vol);

    struct blob_buf b;
    memset(&b, 0, sizeof(b));
    blob_buf_init(&b, 0);
    blobmsg_add_u32(&b, "volume", g_user_vol);
    blobmsg_add_u32(&b, "mcu_volume", g_user_vol);
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

static int mcud_ubus_set_source(struct ubus_context *ctx, struct ubus_object *obj,
                                struct ubus_request_data *req, const char *method,
                                struct blob_attr *msg) {
    (void)obj;
    (void)method;
    struct blob_attr *tb[__SET_SRC_MAX];
    blobmsg_parse(set_src_policy, __SET_SRC_MAX, tb, blob_data(msg), blob_len(msg));

    if (!tb[SET_SRC_SOURCE]) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    const char *src = blobmsg_get_string(tb[SET_SRC_SOURCE]);
    int new_src = 0;
    if (strcasecmp(src, "wifi") == 0 || strcasecmp(src, "i2s") == 0 || strcmp(src, "0") == 0) {
        new_src = 0;
    } else if (strcasecmp(src, "bluetooth") == 0 || strcasecmp(src, "bt") == 0 || strcmp(src, "1") == 0) {
        new_src = 1;
    } else if (strcasecmp(src, "aux") == 0 || strcasecmp(src, "line-in") == 0 || strcmp(src, "2") == 0) {
        new_src = 2;
    } else {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    set_audio_source(new_src);

    struct blob_buf b;
    memset(&b, 0, sizeof(b));
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "source", get_source_name(g_current_source));
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

static int mcud_ubus_set_mute(struct ubus_context *ctx, struct ubus_object *obj,
                              struct ubus_request_data *req, const char *method,
                              struct blob_attr *msg) {
    (void)obj;
    (void)method;
    struct blob_attr *tb[__SET_MUTE_MAX];
    blobmsg_parse(set_mute_policy, __SET_MUTE_MAX, tb, blob_data(msg), blob_len(msg));

    if (!tb[SET_MUTE_MUTE]) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    int mute = blobmsg_get_u32(tb[SET_MUTE_MUTE]);
    set_hardware_mute(mute);

    struct blob_buf b;
    memset(&b, 0, sizeof(b));
    blob_buf_init(&b, 0);
    blobmsg_add_u8(&b, "mute", g_is_muted ? 1 : 0);
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

static int mcud_ubus_set_network_state(struct ubus_context *ctx, struct ubus_object *obj,
                                       struct ubus_request_data *req, const char *method,
                                       struct blob_attr *msg) {
    (void)ctx; (void)obj; (void)req; (void)method;
    struct blob_attr *tb[__SET_NET_MAX];
    blobmsg_parse(set_net_policy, __SET_NET_MAX, tb, blob_data(msg), blob_len(msg));
    if (!tb[SET_NET_STATE]) return UBUS_STATUS_INVALID_ARGUMENT;

    const char *st = blobmsg_get_string(tb[SET_NET_STATE]);
    const char *link = tb[SET_NET_LINK] ? blobmsg_get_string(tb[SET_NET_LINK]) : NULL;
    const char *err = tb[SET_NET_ERROR] ? blobmsg_get_string(tb[SET_NET_ERROR]) : NULL;
    LOG_INFO("UBUS RPC set_network_state: %s%s%s%s%s", st,
             link ? " link=" : "", link ? link : "",
             err ? " error=" : "", err ? err : "");

    /* Re-send unconditionally rather than only on change: this path is
     * edge-triggered from hotplug, and a repeat is exactly what re-syncs an LED
     * the MCU got wrong. Stock does the same on every GNOTIFY. */
    if (strcasecmp(st, "connected") == 0) {
        g_net_state = 1;
        g_sta = 1; g_www = 1; g_ra0 = 0;
        uart_send(CMD_WPS_END);          /* stock sends this on NET_CONNECTED */
        mcu_report_state("STA", g_sta);
        mcu_report_state("WWW", g_www);
        mcu_report_state("RA0", g_ra0);
    } else if (strcasecmp(st, "connecting") == 0) {
        g_net_state = 2;
        g_sta = 2;
        mcu_report_state("STA", g_sta);
    } else if (strcasecmp(st, "ap") == 0 || strcasecmp(st, "softap") == 0) {
        g_net_state = 3;
        g_ra0 = 0;                       /* AP is up, nobody joined yet */
        mcu_report_state("RA0", g_ra0);
    } else if (strcasecmp(st, "ap_link") == 0) {
        g_ra0 = 1;                       /* a phone joined the setup AP */
        mcu_report_state("RA0", g_ra0);
    } else if (strcasecmp(st, "ap_unlink") == 0) {
        g_ra0 = 2;
        mcu_report_state("RA0", g_ra0);
    } else if (strcasecmp(st, "wps") == 0) {
        g_net_state = 4;
        uart_send(CMD_WPS_PAIRING);
    } else {
        g_net_state = 0;
        g_sta = 0; g_www = 0;
        mcu_report_state("STA", g_sta);
        mcu_report_state("WWW", g_www);
    }

    /* eth is its own variable in the MCU, independent of the wifi state: the
     * speaker can sit on ethernet while the setup AP is still up */
    if (link) {
        g_eth = (strcasecmp(link, "eth") == 0) ? 1 : 0;
        mcu_report_state("ETH", g_eth);
    }

    /* One-shot failure codes. Stock resets STA to 0 first, then reports the
     * reason, so the MCU shows a steady error instead of a stale "connecting". */
    if (err) {
        static const struct { const char *name; int code; } errs[] = {
            { "connect_fail", 3 }, { "timeout",     3 },
            { "wrong_pwd",    4 }, { "unsupport",   5 },
            { "miss_router",  6 }, { "dhcp_fail",   7 },
        };
        for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
            if (strcasecmp(err, errs[i].name) == 0) {
                g_sta = 0;
                mcu_report_state("STA", 0);
                mcu_report_state("STA", errs[i].code);
                break;
            }
        }
    }
    return 0;
}

static int mcud_ubus_poweroff(struct ubus_context *ctx, struct ubus_object *obj,
                              struct ubus_request_data *req, const char *method,
                              struct blob_attr *msg) {
    (void)ctx;
    (void)obj;
    (void)req;
    (void)method;
    (void)msg;
    LOG_INFO("UBUS RPC poweroff requested");
    uloop_end();
    graceful_shutdown();
    return 0;
}

static const struct ubus_method mcud_methods[] = {
    UBUS_METHOD_NOARG("status", mcud_ubus_status),
    UBUS_METHOD("set_volume", mcud_ubus_set_volume, set_vol_policy),
    UBUS_METHOD("set_source", mcud_ubus_set_source, set_src_policy),
    UBUS_METHOD("set_mute", mcud_ubus_set_mute, set_mute_policy),
    UBUS_METHOD("set_network_state", mcud_ubus_set_network_state, set_net_policy),
    UBUS_METHOD_NOARG("poweroff", mcud_ubus_poweroff),
};

static struct ubus_object_type mcud_obj_type =
    UBUS_OBJECT_TYPE("mcud", mcud_methods);

static struct ubus_object mcud_obj = {
    .name = "mcud",
    .type = &mcud_obj_type,
    .methods = mcud_methods,
    .n_methods = ARRAY_SIZE(mcud_methods),
};

static int ubus_init_service(void) {
    g_ubus_ctx = ubus_connect(NULL);
    if (!g_ubus_ctx) {
        LOG_WARN("Failed to connect to ubus daemon");
        return -1;
    }
    ubus_add_uloop(g_ubus_ctx);
    int ret = ubus_add_object(g_ubus_ctx, &mcud_obj);
    if (ret) {
        LOG_ERROR("Failed to publish ubus object 'mcud': %s", ubus_strerror(ret));
        return -1;
    }
    LOG_INFO("ubus object 'mcud' published with status/set_volume/set_source/set_mute/poweroff methods");
    return 0;
}

/* ========================================================================= */
/* ULOOP Callbacks & I/O Handlers                                            */
/* ========================================================================= */

static void uart_read_cb(struct uloop_fd *u, unsigned int events) {
    (void)events;
    static char line_buf[MAX_LINE_LEN * 2];
    static size_t line_pos = 0;

    char rx[READ_BUF_SIZE];
    ssize_t n = read(u->fd, rx, sizeof(rx));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        LOG_WARN("UART read error or EOF: %s", strerror(errno));
        return;
    }

    if (line_pos + n < sizeof(line_buf)) {
        memcpy(line_buf + line_pos, rx, n);
        line_pos += n;
        line_buf[line_pos] = '\0';

        /* Streaming Frame Parser for non-delimited / delimited MCU+ tokens */
        while (line_pos > 0) {
            char *hdr = strstr(line_buf, "MCU+");
            if (!hdr) {
                /* No MCU+ header in buffer, discard leading garbage */
                line_pos = 0;
                break;
            }
            if (hdr != line_buf) {
                /* Shift buffer to align with MCU+ header */
                size_t offset = hdr - line_buf;
                memmove(line_buf, hdr, line_pos - offset);
                line_pos -= offset;
                line_buf[line_pos] = '\0';
            }

            if (line_pos < 4) break;

            /* Check if there is a next MCU+ header in stream */
            char *next_hdr = strstr(line_buf + 4, "MCU+");
            char *delim = strpbrk(line_buf, "\r\n&");
            size_t token_len = 0;

            if (delim && (!next_hdr || delim < next_hdr)) {
                token_len = delim - line_buf;
            } else if (next_hdr) {
                token_len = next_hdr - line_buf;
            } else if (line_pos >= 16) {
                token_len = 11;
            } else {
                /* Incomplete token, wait for more bytes */
                break;
            }

            if (token_len > 0) {
                char token[MAX_LINE_LEN];
                if (token_len >= sizeof(token)) token_len = sizeof(token) - 1;
                memcpy(token, line_buf, token_len);
                token[token_len] = '\0';

                process_mcu_command(token);

                /* Advance past delimiter if present */
                size_t advance = token_len;
                while (advance < line_pos && (line_buf[advance] == '\r' || line_buf[advance] == '\n' || line_buf[advance] == '&')) {
                    advance++;
                }

                memmove(line_buf, line_buf + advance, line_pos - advance);
                line_pos -= advance;
                line_buf[line_pos] = '\0';
            } else {
                /* Delimiter at start or empty */
                memmove(line_buf, line_buf + 1, line_pos - 1);
                line_pos -= 1;
                line_buf[line_pos] = '\0';
            }
        }
    } else {
        LOG_WARN("UART line buffer overflow, resetting");
        line_pos = 0;
    }
}

static void fifo_read_cb(struct uloop_fd *u, unsigned int events) {
    (void)events;
    static char fifo_line[MAX_LINE_LEN];
    static size_t fifo_pos = 0;

    char temp[READ_BUF_SIZE];
    ssize_t fn = read(u->fd, temp, sizeof(temp));
    if (fn <= 0) return;

    for (ssize_t i = 0; i < fn; i++) {
        char c = temp[i];
        if (c == '\n' || c == '\r') {
            if (fifo_pos > 0) {
                fifo_line[fifo_pos] = '\0';
                LOG_INFO("FIFO command: %s", fifo_line);
                g_last_activity_time = time(NULL);

                if (strncmp(fifo_line, "AXX+VOL+", 8) == 0 && isdigit((unsigned char)fifo_line[8])) {
                    set_user_volume(atoi(fifo_line + 8));
                } else if (strncmp(fifo_line, "AXX+PLM+000", 11) == 0) {
                    set_audio_source(0);
                } else if (strncmp(fifo_line, "AXX+PLM+001", 11) == 0) {
                    set_audio_source(2);
                } else if (strncmp(fifo_line, "AXX+PLM+002", 11) == 0) {
                    set_audio_source(1);
                } else {
                    uart_send(fifo_line);
                    uart_send("\n");
                }
                fifo_pos = 0;
            }
        } else if (fifo_pos < sizeof(fifo_line) - 1) {
            fifo_line[fifo_pos++] = c;
        }
    }
}

static void watchdog_timer_cb(struct uloop_timeout *t) {
    /* Periodic hardware SoC watchdog ping (Event-Driven UART requires NO periodic heartbeat) */
    watchdog_ping();
    uloop_timeout_set(t, WATCHDOG_CHECK_MS);
}

static void sleep_timer_cb(struct uloop_timeout *t) {
    time_t now = time(NULL);

    /* Refresh activity timestamp if audio stream is active or external inputs in use */
    if (is_audio_active()) {
        g_last_activity_time = now;
        if (!g_is_playing) {
            g_is_playing = 1;
            uart_send(CMD_PLAY_START);
        }
    } else {
        if (g_is_playing) {
            g_is_playing = 0;
            uart_send(CMD_PLAY_STOP);
        }
    }

    /* Pending rate-limited volume MQTT flush */
    if (g_pending_vol_pub >= 0 && (now - g_last_vol_pub_sec) >= 1) {
        mqtt_publish_volume(g_pending_vol_pub);
    }

    /* Auto-sleep check: strictly disabled when charging or always-on */
    if (!g_is_charging && g_auto_sleep_min > 0) {
        if ((now - g_last_activity_time) >= (time_t)(g_auto_sleep_min * 60)) {
            if (is_audio_active()) {
                g_last_activity_time = now;
                LOG_INFO("Auto-Sleep cancelled: audio stream active");
            } else {
                LOG_INFO("Auto-Sleep: Inactivity timeout of %d min reached. Shutting down...", g_auto_sleep_min);
                uloop_end();
                graceful_shutdown();
                return;
            }
        }
    }

    uloop_timeout_set(t, SLEEP_CHECK_MS);
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
    uloop_end();
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    int opt;
    while ((opt = getopt(argc, argv, "h:p:u:P:t:v:s:l:abm")) != -1) {
        switch (opt) {
            case 'l':
                g_log_level = atoi(optarg);
                break;
            case 'a':
                g_sleep_in_aux = 1;
                break;
            case 'b':
                g_sleep_in_bt = 1;
                break;
            case 'v':
                g_user_vol = atoi(optarg);
                if (g_user_vol < 0) g_user_vol = 0;
                if (g_user_vol > 100) g_user_vol = 100;
                break;
            case 's':
                g_auto_sleep_min = atoi(optarg);
                if (g_auto_sleep_min < 0) g_auto_sleep_min = 0;
                break;
            case 'h':
                strncpy(g_mqtt_host, optarg, sizeof(g_mqtt_host) - 1);
                g_mqtt_host[sizeof(g_mqtt_host) - 1] = '\0';
                break;
            case 'p':
                g_mqtt_port = atoi(optarg);
                break;
            case 'u':
                strncpy(g_mqtt_user, optarg, sizeof(g_mqtt_user) - 1);
                g_mqtt_user[sizeof(g_mqtt_user) - 1] = '\0';
                break;
            case 'P':
                strncpy(g_mqtt_pass, optarg, sizeof(g_mqtt_pass) - 1);
                g_mqtt_pass[sizeof(g_mqtt_pass) - 1] = '\0';
                break;
            case 't':
                strncpy(g_topic_prefix, optarg, sizeof(g_topic_prefix) - 1);
                g_topic_prefix[sizeof(g_topic_prefix) - 1] = '\0';
                break;
            case 'm':
                g_mqtt_enabled = 0;
                break;
        }
    }

    setlinebuf(stdout);
    setlinebuf(stderr);
    signal(SIGCHLD, SIG_IGN);

    /* -v is only the first-boot default now */
    int saved_vol = vol_load();
    if (saved_vol >= 0) {
        LOG_INFO("Restored persisted volume: %d%% (default_volume %d%% ignored)",
                 saved_vol, g_user_vol);
        g_user_vol = saved_vol;
    }

    uloop_init();

    g_uart_fd = uart_init();
    if (g_uart_fd < 0) return EXIT_FAILURE;

    g_fifo_fd = fifo_init();
    if (g_fifo_fd < 0) return EXIT_FAILURE;

    alsa_init();
    mqtt_setup();
    ubus_init_service();

    /* Verified Audio Pro C3 Linkplay Handshake Sequence */
    uart_send(CMD_MCU_READY);
    usleep(100000);
    uart_send(CMD_POWER_ON);
    usleep(100000);
    uart_send(CMD_BOOT_DONE);
    usleep(100000);
    uart_send(CMD_AUDIOPRO_WAKE_M);
    uart_send("AXX+M2S+011\n");
    usleep(100000);
    /* Report what is actually up instead of asserting "connected": hotplug fires
     * later and will correct us, but until then the LEDs should not lie.
     * A default route is our stand-in for WWW; a real reachability probe belongs
     * in the hotplug script, which can override us over ubus. */
    char rif[32];
    if (default_route_iface(rif, sizeof(rif))) {
        g_sta = 1; g_www = 1; g_net_state = 1;
        g_eth = (strncmp(rif, "eth", 3) == 0);
    } else {
        g_sta = 0; g_www = 0; g_eth = 0; g_net_state = 0;
    }
    g_ra0 = 0;
    mcu_report_state("RA0", g_ra0);
    mcu_report_state("STA", g_sta);
    mcu_report_state("WWW", g_www);
    mcu_report_state("ETH", g_eth);
    usleep(100000);
    uart_send(CMD_MODE_WIFI);
    usleep(100000);
    uart_send(CMD_UNMUTE);
    uart_send(CMD_PLAY_START);

    /* Straight write first: softvol creates the control at 0 dB when its params
     * change, and fading down from full scale is not something to do while
     * something might already be playing. */
    alsa_set_master(g_user_vol);
    set_user_volume(g_user_vol);
    /* The MCU only volunteers its version when asked, and it answers this one
     * (AXX+VER+GET gets nothing). Worth having in the log next to a bug report. */
    uart_send("AXX+MCU+VER\n");
    g_mcu_link_ts = time(NULL);
    LOG_INFO("Linkplay MCU initialized: I2S Play Mode (AXX+PLM+000), Amplifier Unmuted, AudioPro Wake synced.");
    if (g_auto_sleep_min > 0) {
        LOG_INFO("Inactivity Auto-Sleep timer enabled: %d minutes", g_auto_sleep_min);
    } else {
        LOG_INFO("Inactivity Auto-Sleep disabled: Always-On mode active (24/7 ready)");
    }
    play_sound(SOUND_BOOT);

    g_last_activity_time = time(NULL);

    g_uart_ufd.fd = g_uart_fd;
    g_uart_ufd.cb = uart_read_cb;
    uloop_fd_add(&g_uart_ufd, ULOOP_READ);

    g_fifo_ufd.fd = g_fifo_fd;
    g_fifo_ufd.cb = fifo_read_cb;
    uloop_fd_add(&g_fifo_ufd, ULOOP_READ);

    g_wdt_timer.cb = watchdog_timer_cb;
    uloop_timeout_set(&g_wdt_timer, WATCHDOG_CHECK_MS);

    g_sleep_timer.cb = sleep_timer_cb;
    uloop_timeout_set(&g_sleep_timer, SLEEP_CHECK_MS);

    uloop_run();

    /* Cleanup */
    uart_send(CMD_MUTE);
    usleep(100000);
    uloop_done();

    if (g_ubus_ctx) {
        ubus_free(g_ubus_ctx);
        g_ubus_ctx = NULL;
    }
    if (g_fifo_fd >= 0) {
        close(g_fifo_fd);
        unlink(CMD_FIFO_PATH);
    }
    if (g_uart_fd >= 0) close(g_uart_fd);
    if (g_wdt_fd >= 0) close(g_wdt_fd);
    for (int i = 0; i < g_mixer_ufd_count; i++) uloop_fd_delete(&g_mixer_ufds[i]);
    free(g_mixer_ufds);
    if (g_mixer) snd_mixer_close(g_mixer);
    if (g_mosq) {
        mosquitto_loop_stop(g_mosq, true);
        mosquitto_destroy(g_mosq);
        mosquitto_lib_cleanup();
    }

    return EXIT_SUCCESS;
}
