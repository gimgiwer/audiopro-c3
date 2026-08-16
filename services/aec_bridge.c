#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <alsa/asoundlib.h>

#define CHUNK_FRAMES 1024
#define RATE         44100
#define CHANNELS     2

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char *argv[]) {
    const char *ha_ip = (argc > 1) ? argv[1] : "192.168.1.100";
    int ha_port = (argc > 2) ? atoi(argv[2]) : 5000;

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    snd_pcm_t *pcm_in = NULL;
    snd_pcm_t *pcm_out = NULL;

    int err = snd_pcm_open(&pcm_in, "hw:Loopback,1,0", SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "[aec_bridge] Error opening capture: %s\n", snd_strerror(err));
        return 1;
    }
    snd_pcm_set_params(pcm_in, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, CHANNELS, RATE, 1, 50000);

    err = snd_pcm_open(&pcm_out, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "[aec_bridge] Error opening playback: %s\n", snd_strerror(err));
        snd_pcm_close(pcm_in);
        return 1;
    }
    snd_pcm_set_params(pcm_out, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, CHANNELS, RATE, 1, 50000);

    int sock = -1;
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(ha_port);
    inet_pton(AF_INET, ha_ip, &srv.sin_addr);

    short buf[CHUNK_FRAMES * CHANNELS];
    short mono_buf[CHUNK_FRAMES];

    while (g_running) {
        snd_pcm_sframes_t frames = snd_pcm_readi(pcm_in, buf, CHUNK_FRAMES);
        if (frames < 0) {
            frames = snd_pcm_recover(pcm_in, frames, 0);
            if (frames < 0) {
                usleep(5000);
                continue;
            }
        }

        // 1. Play directly to real DAC hw:0,0
        snd_pcm_sframes_t written = snd_pcm_writei(pcm_out, buf, frames);
        if (written < 0) {
            snd_pcm_recover(pcm_out, written, 0);
        }

        // 2. Non-blocking TCP tap to HA
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                fcntl(sock, F_SETFL, O_NONBLOCK);
                connect(sock, (struct sockaddr *)&srv, sizeof(srv));
            }
        }

        if (sock >= 0) {
            // Convert stereo to mono for AEC reference
            for (int i = 0; i < frames; i++) {
                mono_buf[i] = (short)(((int)buf[i * 2] + (int)buf[i * 2 + 1]) / 2);
            }
            ssize_t s = send(sock, mono_buf, frames * sizeof(short), MSG_NOSIGNAL);
            if (s < 0 && (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINPROGRESS)) {
                close(sock);
                sock = -1;
            }
        }
    }

    if (sock >= 0) close(sock);
    if (pcm_in) snd_pcm_close(pcm_in);
    if (pcm_out) snd_pcm_close(pcm_out);
    return 0;
}
