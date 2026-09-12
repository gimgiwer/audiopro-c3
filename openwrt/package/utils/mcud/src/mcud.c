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
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/file.h>
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
#define MCU_KEEPALIVE_SEC    60   /* default for -k, see sleep_timer_cb */
#define VOL_STEP_DB          1   /* percent fallback for when Master's raw range is unreadable */
#define MCU_VOL_STEP         2   /* one panel press moves the MCU's own counter this far */
#define MCU_VOL_ECHO_MS      2000 /* the MCU restates a level we sent it about a second later */
#define MCU_WAKE_BURST_MS    3000 /* and restates it twice during its own wake poll */
#define MAX_LINE_LEN         128
#define READ_BUF_SIZE        256
#define BUTTON_DEBOUNCE_MS   120

#define SOUND_BOOT           "/usr/share/sounds/boot.mp3"
#define SOUND_PRESET         "/usr/share/sounds/preset_saved.mp3"
#define SOUND_BT_CONN        "/usr/share/sounds/bt_connected.mp3"
#define SOUND_WIFI_CONN      "/usr/share/sounds/wifi_connected.mp3"
#define SOUND_LOW_BAT        "/usr/share/sounds/low_battery.mp3"

/* STM8S factory opcodes (Linkplay A28 / Audio Pro C3 hardware). */
#define CMD_MCU_READY        "AXX+MCU+RDY\n"
#define CMD_BOOT_DONE        "AXX+BOT+DON\n"
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
static int  g_keepalive_sec = MCU_KEEPALIVE_SEC;  /* 0 = let the MCU power us off */
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
static int  g_sleep_on_no_net = 0;   /* 0 = stay awake on no network, 1 = timer sleep on battery */
static int  g_no_net_timeout_min = 10; /* minutes without network before power off */
static time_t g_no_net_lost_ts = 0;  /* timestamp when network was lost */
static int  g_sound_prompts_enabled = 0; /* 0 = Disabled to avoid ALSA audio sink deadlock */
static int  g_is_playing = 0;        /* 1 = Audio stream running, 0 = Stopped */
static time_t g_last_activity_time = 0;
static int64_t g_last_button_ms = 0;

static time_t g_mcu_link_ts = 0;      /* when we last ran the MCU handshake */
static int  g_mcu_vol = -1;           /* the MCU's own volume counter, as last reported */
static int64_t g_mcu_vol_tx_ms = 0;   /* when we last handed the MCU a level of ours */
static int64_t g_mcu_burst_ms = 0;    /* when the MCU last started its own wake poll */
static char g_mcu_ver[16] = "";
static time_t g_last_vol_pub_sec = 0;
static int    g_pending_vol_pub = -1;
static int    g_last_bat = 100;

/* squeezelite's volume_control, i.e. the fader Music Assistant drives. It sits
 * before Master in the squeeze_in chain, so the two multiply. */
#define MA_VOL_CTL "Slimproto"
static int  g_ma_vol = -1;            /* last seen MA fader level, -1 = never read */
static time_t g_sleep_deadline = 0;   /* MQTT sleep timer, 0 = disarmed */
static int  g_sleep_left_pub = -1;    /* last minutes-left value published */

/* Every deadline in here is an elapsed-time measurement, so none of them may use
 * the wall clock: this board has no RTC, so the clock starts wrong and chronyd
 * steps it forward once the network is up - which used to drag the sleep timer
 * and the activity stamps along with it. */
static time_t now_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#define LOG_ERROR(fmt, ...) do { if (g_log_level >= LOG_LEVEL_ERROR) fprintf(stderr, "[mcud ERROR] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_WARN(fmt, ...)  do { if (g_log_level >= LOG_LEVEL_WARN)  fprintf(stderr, "[mcud WARN] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)  do { if (g_log_level >= LOG_LEVEL_INFO)  fprintf(stdout, "[mcud INFO] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...) do { if (g_log_level >= LOG_LEVEL_DEBUG) fprintf(stdout, "[mcud DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)

static int uart_init(void);
static void graceful_shutdown(void);
static void adopt_audio_source(int source);
static void set_user_volume(int vol);
static void mqtt_publish_volume(int vol);
static void mqtt_publish_ma_volume(int vol);
static void mqtt_publish_sleep(int minutes);
static void adjust_user_volume(int delta);
static void set_hardware_mute(int mute);
static const char *get_source_name(int source);
static void mcu_report_state(const char *tag, int val);
static void vol_touch(void);
static void fire_preset(int preset, int announce);

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
        /* no timeout(1) in the image - execlp used to fail and the sound never played; alarm survives exec */
        alarm(8);
        /* through a dmix slot, not plughw:0,0 - the raw hw is EBUSY while anything else plays */
        execlp("mpg123", "mpg123", "-q", "-a", "tts_in", "--", path, (char *)NULL);
        _exit(1);
    }
}

/* system() waits for the child, and callers here run inside the uloop: a slow
 * /etc/init.d restart would leave the MCU poll unanswered and starve the
 * watchdog for its whole duration. SIGCHLD is SIG_IGN (see main), so the child
 * is reaped by the kernel and never zombies. */
static void spawn_async(const char *cmd) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG_WARN("fork for '%s' failed: %s", cmd, strerror(errno));
        return;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        /* SIG_IGN survives execve, and a shell that cannot reap its own
         * children leaves zombies behind for every command it runs */
        signal(SIGCHLD, SIG_DFL);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
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

/* PLM is stock's source-mode channel, not play/pause: mv_ioguard's
 * Function_SwitchMode (0x40720c) maps these numbers onto
 * setPlayerCmd:switchmode_by_mcu:<name>, and AXX+PLM+%03d is what the app sends
 * back for GNOTIFY=PLAY_SOURCE_MODE. Only three of its fourteen codes mean
 * anything on this board; 012 is stock's second line-in jack, which the C3 lacks
 * but the MCU may still name. */
static int plm_code(int source) {
    switch (source) {
        case 1:  return 6;   /* bluetooth */
        case 2:  return 5;   /* line-in */
        default: return 0;   /* wifi */
    }
}

static int source_from_plm(int code) {
    switch (code) {
        case 0:  return 0;
        case 6:  return 1;
        case 5:
        case 12: return 2;
        default: return -1;
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

#define PCM_STATUS_PATH "/proc/asound/card0/pcm0p/sub0/status"
#define STALL_TICKS   6   /* 6 * SLEEP_CHECK_MS = 30 s of a frozen ring */
#define STALL_TICKS_ZERO 12  /* hw_ptr still 0 after 60 s - see check_stream_stall */
#define STALL_COOLDOWN_SEC 120
#define STALL_MAX_TRIES 3

/* A substream can sit in RUNNING with hw_ptr and tstamp both frozen - measured
 * 258 s of it after a dmix client was SIGKILLed. A stream that is merely starved
 * goes to XRUN instead, so both fields standing still is the pathological case
 * and not a pause. */
static int read_stream_pos(unsigned long long *hw_ptr, char *tstamp, size_t ts_len,
                           int *running, int *owner_pid) {
    int fd = open(PCM_STATUS_PATH, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    *running = (strstr(buf, "RUNNING") != NULL);
    *hw_ptr = 0; *owner_pid = 0; tstamp[0] = '\0';

    char *p = strstr(buf, "hw_ptr");
    if (p && (p = strchr(p, ':'))) *hw_ptr = strtoull(p + 1, NULL, 10);
    p = strstr(buf, "owner_pid");
    if (p && (p = strchr(p, ':'))) *owner_pid = (int)strtol(p + 1, NULL, 10);
    p = strstr(buf, "tstamp");
    if (p && (p = strchr(p, ':'))) {
        p++;
        while (*p == ' ') p++;
        size_t i = 0;
        while (i < ts_len - 1 && *p && *p != '\n') tstamp[i++] = *p++;
        tstamp[i] = '\0';
    }
    return 1;
}

/* owner_pid in the substream status is a TID, not the process pid, and threads
 * carry their own names - librespot runs tokio-runtime-w threads. Match the
 * whitelist against the process: Tgid from /proc/<tid>/status, then the
 * process's own comm. */
static const char *stall_service_for_pid(int tid, int *out_tgid) {
    *out_tgid = 0;
    if (tid <= 0) return NULL;

    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/status", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = '\0';

    char *p = strstr(buf, "\nTgid:");
    if (!p) return NULL;
    int tgid = atoi(p + 6);
    if (tgid <= 0) return NULL;

    snprintf(path, sizeof(path), "/proc/%d/comm", tgid);
    fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char comm[64];
    n = read(fd, comm, sizeof(comm) - 1);
    close(fd);
    if (n <= 0) return NULL;
    comm[n] = '\0';
    char *nl = strchr(comm, '\n');
    if (nl) *nl = '\0';

    /* only things we ship as procd services - anything else gets logged and left
     * alone, because restarting a stranger is worse than a silent speaker */
    const char *svc = NULL;
    if (!strcmp(comm, "squeezelite"))         svc = "squeezelite";
    else if (!strcmp(comm, "shairport-sync")) svc = "shairport-sync";
    else if (!strcmp(comm, "librespot"))      svc = "librespot";
    if (svc) *out_tgid = tgid;
    return svc;
}

static void check_stream_stall(time_t now) {
    static unsigned long long last_hw = 0;
    static char last_ts[48] = "";
    static int ticks = 0;
    static int tries = 0;
    static time_t last_action = 0;

    unsigned long long hw = 0;
    char ts[48];
    int running = 0, pid = 0;

    if (!read_stream_pos(&hw, ts, sizeof(ts), &running, &pid) || !running) {
        ticks = 0;
        tries = 0;
        last_hw = 0;
        last_ts[0] = '\0';
        return;
    }

    if (hw == last_hw && !strcmp(ts, last_ts)) {
        ticks++;
    } else {
        ticks = 0;
        tries = 0;
        last_hw = hw;
        snprintf(last_ts, sizeof(last_ts), "%s", ts);
        return;
    }

    /* hw_ptr 0 needs a longer window than a frozen non-zero one: a fresh open
     * legitimately reads 0 for a moment. It is not exempt though - measured
     * 2026-08-24, a connected squeezelite advances hw_ptr at 44100 frames/s
     * whether or not anything plays, so a zero that stays zero for a minute is
     * a dead dma, and that is exactly the state the box sat in for 40 minutes
     * with every new open blocking in poll(). appl_ptr is no help either: dmix
     * writes into the shared buffer and leaves the slave's appl_ptr at 0 even
     * mid-track, so gating on it would disable the detector outright. */
    if (ticks < (hw == 0 ? STALL_TICKS_ZERO : STALL_TICKS)) return;
    if (now - last_action < STALL_COOLDOWN_SEC) return;

    int owner = 0;
    const char *svc = stall_service_for_pid(pid, &owner);
    if (tries >= STALL_MAX_TRIES) {
        if (tries == STALL_MAX_TRIES) {
            LOG_WARN("stream still stalled after %d attempts, giving up until it moves again", tries);
            tries++;
        }
        last_action = now;
        return;
    }
    LOG_WARN("stream stalled: RUNNING but hw_ptr %llu and tstamp %s frozen for %d s, owner tid %d, process %d (%s)",
             hw, ts, ticks * (SLEEP_CHECK_MS / 1000), pid, owner, svc ? svc : "unknown");
    if (svc) {
        /* if the owner sits in SIGSTOP it can never take TERM and procd will
         * escalate to KILL - and a killed alsa client wedges dmix for everyone,
         * i.e. the cure becomes the disease. wake it up first; player_control.sh
         * does the same dance before poking the players. */
        if (owner > 0) kill(owner, SIGCONT);
        /* not "restart": procd's stop returns as soon as it has queued the TERM,
         * so restart can hand the card to the new instance while the old one
         * still holds the fd, and the new one then attaches to the stale dmix
         * shm and never starts the dma at all. Waiting for the substream to read
         * "closed" is what actually cleared it by hand on 2026-08-24. */
        char cmd[320];
        snprintf(cmd, sizeof(cmd),
                 "/etc/init.d/%s stop; i=0; "
                 "while [ $i -lt 15 ] && ! grep -q closed %s; do sleep 1; i=$((i+1)); done; "
                 "/etc/init.d/%s start", svc, PCM_STATUS_PATH, svc);
        spawn_async(cmd);
        tries++;
        LOG_INFO("stopped and restarted %s to clear the stalled substream (attempt %d)", svc, tries);
    }
    last_action = now;
    ticks = 0;
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

    /* Stop streaming processes first - blocking on purpose: ordering matters
     * here and the event loop is done once we decided to power off. */
    system("killall -TERM librespot shairport-sync squeezelite 2>/dev/null");
    usleep(200000);

    /* Stock mv_ioguard: stop MCU playback state before mute */
    uart_send(CMD_PLAY_STOP);
    usleep(50000);

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
        /* poweroff follows, so nothing will drive the loop again: push it out by
         * hand. loop_stop() is not an option any more - with no loop thread it
         * just returns MOSQ_ERR_INVAL and the publish would die in the queue */
        mosquitto_disconnect(g_mosq);
        mosquitto_loop_write(g_mosq, 1);
    }

    sync();
    system("/sbin/poweroff");
}

/* Percent is the unit everyone here speaks - the MCU bookkeeps it, MQTT and the
 * ubus api report it - but the Master softvol is dB-linear: raw 0..1000 spans
 * -60..0 dB. librespot maps its own slider through a law of its own, so both
 * ends have to agree or they disagree about what the level is: raw 513 read back
 * through the old (v-min)*100/(max-min) came out as 51% while Spotify was
 * showing 25%, and that wrong number went to the MCU, to MQTT and into
 * /etc/mcud.volume.
 *
 * The law is amplitude = pct/100, i.e. a straight amplitude fader, so
 * dB = 20*log10(pct/100) and raw = 1000 * (1 + log10(pct/100)/3). Same as
 * librespot's `linear` volume-ctrl, which is what the factory firmware did too
 * (liblpml_alsa.so: gain_Q16 = vol*65536/100).
 *
 * It replaced the (pct/100)^3 cube that PulseAudio and PipeWire use. The cube is
 * right for a 0..100 dial that spans a whole amp's range, but on this 60 dB
 * control it put 25% at -36 dB and muted everything under 10% outright, so two
 * thirds of the dial were unusable - anything below 40% was inaudible in the
 * room. The fader spreads the quiet end out instead: 1% is -40 dB, 25% is -12 dB,
 * 50% is -6 dB, and nothing clamps to silence except a real 0. The cost is a
 * compressed top - 50..100% is only 6 dB - but that half is near max output
 * anyway, and the physical buttons step in raw (3 dB a press) so their feel does
 * not change.
 *
 * Precomputed so this stays integer-only and mcud does not pull in libm. The top
 * end is free: max_dB is 0.0, which makes softvol's zero_dB_val the last index,
 * so raw 1000 is a straight areas_copy with no multiply at all. Index 0 is a
 * hard 0 because softvol turns raw 0 into real digital silence
 * (pcm_softvol.c: cur_vol[0] == 0 -> areas_silence). */
static const unsigned short vol_curve[101] = {
	   0,    1,  151,  239,  301,  349,  389,  423,
	 452,  477,  500,  521,  540,  557,  573,  588,
	 602,  615,  628,  639,  651,  661,  671,  681,
	 690,  699,  707,  716,  724,  731,  739,  746,
	 753,  759,  766,  772,  778,  784,  790,  796,
	 801,  806,  812,  817,  822,  827,  831,  836,
	 841,  845,  850,  854,  858,  862,  866,  870,
	 874,  878,  882,  885,  889,  893,  896,  900,
	 903,  906,  910,  913,  916,  919,  923,  926,
	 929,  932,  935,  938,  940,  943,  946,  949,
	 952,  954,  957,  960,  962,  965,  967,  970,
	 972,  975,  977,  980,  982,  984,  987,  989,
	 991,  993,  996,  998, 1000
};

static long vol_pct_to_raw(int pct, long min, long max) {
	if (pct <= 0) return min;
	if (pct >= 100) return max;
	if (min == 0 && max == 1000) return (long)vol_curve[pct];
	return min + ((long)vol_curve[pct] * (max - min) + 500L) / 1000L;
}

static int vol_db10(int pct) {
	if (pct <= 0) return -400;
	long raw = (long)vol_curve[pct];
	return (int)((raw - 1000L) * 400L / 1000L);
}

/* Nearest percent rather than a truncation: matches librespot's linear volume law. */
static int vol_raw_to_pct(long v, long min, long max) {
	if (max <= min) return -1;
	long raw = ((v - min) * 1000L + (max - min) / 2) / (max - min);
	if (raw <= 0) return 0;
	if (raw >= 1000) return 100;
	int best = 1; long err = 0x7fffffffL;
	for (int p = 1; p <= 100; p++) {
		long d = (long)vol_curve[p] - raw;
		if (d < 0) d = -d;
		if (d < err) { err = d; best = p; }
	}
	return best;
}

/* Attenuation is software-only here: the PCM5102A has no volume register and the
 * TPA3116 gain is pin-strapped, so the ALSA "Master" softvol is the only thing
 * that can change the level. Stock did the same, just inside its own writei
 * wrapper instead of a plugin. */
static int g_master_set_vol = -1;   /* last level we drove Master to ourselves */

static void alsa_set_master(int vol) {
    if (!g_master_elem) return;
    g_master_set_vol = vol;
    long min = 0, max = 1000;
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
    g_master_set_vol = vol;
    long min = 0, max = 1000, cur = 0;
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
    long min = 0, max = 1000, v = 0;
    if (snd_mixer_selem_get_playback_volume_range(g_master_elem, &min, &max) < 0) return -1;
    if (snd_mixer_selem_get_playback_volume(g_master_elem, SND_MIXER_SCHN_FRONT_LEFT, &v) < 0) return -1;
    return vol_raw_to_pct(v, min, max);
}

/* Any softvol by name. The duck stages span -51..0 dB, Slimproto -23..0, and raw
 * is dB-linear on all of them, so percent-of-range is the stage's own position -
 * not what Music Assistant shows: squeezelite runs its slider through its own
 * curve, so MA 50% lands on raw 166, i.e. 65%. vol_curve is Master's law only. */
static snd_mixer_elem_t *alsa_find_elem(const char *name) {
    snd_mixer_selem_id_t *sid;
    if (!g_mixer) return NULL;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_name(sid, name);
    return snd_mixer_find_selem(g_mixer, sid);
}

static int alsa_get_pct(const char *name) {
    snd_mixer_elem_t *e = alsa_find_elem(name);
    long min = 0, max = 0, v = 0;
    if (!e) return -1;
    if (snd_mixer_selem_get_playback_volume_range(e, &min, &max) < 0) return -1;
    if (snd_mixer_selem_get_playback_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &v) < 0) return -1;
    if (max <= min) return -1;
    return (int)(((v - min) * 100 + (max - min) / 2) / (max - min));
}

static int alsa_set_pct(const char *name, int pct) {
    snd_mixer_elem_t *e = alsa_find_elem(name);
    long min = 0, max = 0;
    if (!e) return -1;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (snd_mixer_selem_get_playback_volume_range(e, &min, &max) < 0) return -1;
    return snd_mixer_selem_set_playback_volume_all(e, min + (max - min) * pct / 100);
}

/* --- Music Assistant over the LMS command line (port 9090) ------------------
 * The Slimproto level really lives in MA: it runs its own curve before
 * squeezelite ever sees a dB value, so the softvol percent we can read from
 * ALSA is not the number on the user's slider - raw 131 of 255 is MA 31, not
 * 51. Talking to the CLI puts the front-panel keys on that same number and
 * lets the play/pause key reach MA without an HA automation in the middle.
 * Plain line protocol, request/response only (no subscribe), replies are
 * URL-encoded. */
#define MA_PORT_DEFAULT   9090
/* MA's CLI accepts "listen 1" and "subscribe:N" and then pushes nothing, so a
 * poll is the only way to notice the phone moving MA's own slider. 1 s because
 * anything slower is a visible lag between HA and MA; one line each way. */
#define MA_POLL_MS        1000
#define MA_RETRY_MIN_MS   5000
#define MA_RETRY_MAX_MS   30000
#define MA_STEP_DEFAULT   5     /* MA units per key press: 20 presses top to bottom */
/* one status reply with metadata tags is one line, and MA answers "status - 1"
 * with the current track plus the next one - two artwork URLs and two titles,
 * ~2 kB. Overflowing drops the whole line, i.e. mode and volume with it. */
#define MA_RX_SIZE        8192

static char g_ma_host[64] = "";     /* empty: same box as the broker */
static int  g_ma_port = MA_PORT_DEFAULT;
static int  g_ma_step = MA_STEP_DEFAULT;
static int  g_ma_owns_volume = 0;   /* -o: nothing else rides Master, see /etc/config/profile */
static int  g_ma_fd = -1;
static int  g_ma_connecting = 0;
static struct uloop_fd g_ma_ufd;
static struct uloop_timeout g_ma_poll_t, g_ma_retry_t;
static int  g_ma_retry_ms = MA_RETRY_MIN_MS;
static char g_ma_id[64] = "";       /* our slimproto player id, as MA prints it */
static char g_ma_rx[MA_RX_SIZE];
static size_t g_ma_rxlen = 0;
static int  g_ma_mode = -1;         /* -1 unknown, 0 stop, 1 play, 2 pause */
static int  g_ma_power = -1;
static char g_ma_title[128] = "";
static char g_ma_artist[128] = "";
static char g_ma_album[128] = "";
static char g_ma_art[256] = "";
static int  g_meta_ours = 0;        /* we wrote the meta cache last, so we may clear it */

static void ma_connect(void);
static void ma_retry_cb(struct uloop_timeout *t);

static void ma_arm_retry(void) {
    g_ma_retry_t.cb = ma_retry_cb;
    uloop_timeout_set(&g_ma_retry_t, g_ma_retry_ms);
    g_ma_retry_ms *= 2;
    if (g_ma_retry_ms > MA_RETRY_MAX_MS) g_ma_retry_ms = MA_RETRY_MAX_MS;
}

static const char *ma_host(void) {
    return g_ma_host[0] ? g_ma_host : g_mqtt_host;
}

static int ma_connected(void) {
    return g_ma_fd >= 0 && !g_ma_connecting;
}

/* MA owns the level only while its player is the thing making sound. Stopped
 * hands the keys back to Master, which is what Spotify and AirPlay ride on.
 * With those two off (profile mode smarthome) there is nothing to hand it back
 * to, so MA keeps it even while idle - it takes "mixer volume N" with power:0
 * and remembers it for the next play, which is what makes the keys, HA and MA
 * one number at all times instead of only during playback. */
static int ma_active(void) {
    if (!ma_connected() || !g_ma_id[0] || g_ma_vol < 0) return 0;
    if (g_ma_owns_volume) return 1;
    return g_ma_power != 0 && (g_ma_mode == 1 || g_ma_mode == 2);
}

/* Whether a transport command has any chance of landing. ma_active() is about the
 * volume and stays true while the player idles; the CLI, though, accepts "play" and
 * "mode play" on a stopped player, echoes them back and does nothing - starting one
 * takes ma_start(). So this is what decides whether a plain toggle belongs to MA or
 * falls through to the local players. */
static int ma_has_transport(void) {
    return ma_connected() && g_ma_id[0] && g_ma_power != 0 &&
           (g_ma_mode == 1 || g_ma_mode == 2);
}

/* One number for the user: whatever the keys move right now is what the MCU
 * bookkeeps and HA shows. */
static void vol_publish_visible(void) {
    int v = ma_active() ? g_ma_vol : g_user_vol;
    if (v < 0) return;
    /* Do NOT send mcu_report_state("VOL", v) here: the MCU does not attenuate
     * audio, and reporting here triggers an echo MCU+VOL+nnn that looks like a
     * key press and causes runaway volume oscillation. The MCU queries volume
     * via MCU+VOL+GET when it needs it. */
    mqtt_publish_volume(v);
}

/* Master is in series ahead of MA's own fader, so while MA owns the level Master
 * has to sit at unity - any lower and the two curves add up, and a stale 0 in
 * there mutes the speaker no matter what MA does. */
static void ma_sync_master(int owner) {
    if (!owner && g_ma_vol >= 0 && g_ma_vol != g_user_vol) {
        /* handing the level back adopts MA's number so the keys carry on from the
         * slider instead of jumping to a level nobody set */
        g_user_vol = g_ma_vol;
        vol_touch();
    }
    if (!g_is_muted) alsa_ramp_master(owner ? 100 : g_user_vol);
}

static void ma_disconnect(int retry) {
    int was_owner = ma_active();
    if (g_ma_fd >= 0) {
        uloop_fd_delete(&g_ma_ufd);
        close(g_ma_fd);
        g_ma_fd = -1;
    }
    g_ma_connecting = 0;
    g_ma_rxlen = 0;
    g_ma_mode = -1;
    uloop_timeout_cancel(&g_ma_poll_t);
    if (was_owner) {
        ma_sync_master(0);
        vol_publish_visible();
    }
    if (retry) ma_arm_retry();
}

static int ma_write(const char *line) {
    size_t len;
    ssize_t n;
    if (!ma_connected() || !line) return -1;
    len = strlen(line);
    n = write(g_ma_fd, line, len);
    if (n != (ssize_t)len) {
        LOG_WARN("MA CLI write failed: %s", strerror(errno));
        ma_disconnect(1);
        return -1;
    }
    /* the status poll runs once a second and would bury every other line */
    if (!strstr(line, "status - 1 tags:"))
        LOG_DEBUG("MA TX: [%.*s]", (int)(len - 1), line);
    return 0;
}

/* Every player command is prefixed with the player id. */
static int ma_send(const char *fmt, ...) {
    char line[256];
    int n;
    va_list ap;
    if (!ma_connected() || !g_ma_id[0]) return -1;
    n = snprintf(line, sizeof(line), "%s ", g_ma_id);
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(line) - 2) return -1;
    line[n++] = '\n';
    line[n] = '\0';
    return ma_write(line);
}

/* Start MA from cold. Its own API can do that from any state, the CLI needs two
 * steps: with power 0 the queue is hidden from every verb, and the only one that
 * makes a stopped player sound is the front-panel emulation - "play", "mode play"
 * and "playlist jump" are all accepted and all do nothing. Refused while the
 * speaker is making sound by other means, because the same press arrives while
 * Spotify or line-in is the source, and MA joining in there is a second source,
 * not a resume. */
static int ma_start(void) {
    if (!ma_connected() || !g_ma_id[0] || is_audio_active()) return -1;
    if (g_ma_power == 0 && ma_send("power 1") != 0) return -1;
    if (ma_send("button jump_rew") != 0) return -1;
    LOG_INFO("Music Assistant woken from stopped");
    return 0;
}

static void url_decode(char *s) {
    char *w = s;
    while (*s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *w++ = *s++;
        }
    }
    *w = '\0';
}

/* Which of MA's players is us: match the address our own socket came from, so
 * this survives a MAC that belongs to an interface we never guessed right. */
static int ma_ip_is_ours(const char *ip) {
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    char mine[INET_ADDRSTRLEN];
    if (g_ma_fd < 0 || getsockname(g_ma_fd, (struct sockaddr *)&sa, &len) < 0) return 0;
    if (!inet_ntop(AF_INET, &sa.sin_addr, mine, sizeof(mine))) return 0;
    return strcmp(mine, ip) == 0;
}

/* The web UI reads whatever played last out of one cache file, and every other
 * source writes it from a shell script. Nothing wrote it for the slimproto
 * player, so the now-playing panel stayed empty (or stuck on the previous
 * source) for everything Music Assistant plays - which in smarthome mode is
 * everything. The status poll already carries the track, so this is free. */
#define META_JSON "/tmp/audiopro_meta.json"

static void meta_json_str(char *out, size_t n, const char *in) {
    size_t w = 0;
    for (; *in && w + 2 < n; in++) {
        if (*in == '"' || *in == '\\') {
            out[w++] = '\\';
            out[w++] = *in;
        } else if ((unsigned char)*in < 0x20) {
            out[w++] = ' ';
        } else {
            out[w++] = *in;
        }
    }
    out[w] = '\0';
}

static void meta_write(int active) {
    char t[260], a[260], b[260], tmp[64];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp", META_JSON);
    f = fopen(tmp, "w");
    if (!f) return;
    if (!active) {
        fputs("{\"active\":false}\n", f);
    } else {
        meta_json_str(t, sizeof(t), g_ma_title);
        meta_json_str(a, sizeof(a), g_ma_artist);
        meta_json_str(b, sizeof(b), g_ma_album);
        /* artwork_url is what the browser prefers over the local jpg, and MA's
         * image proxy is on the same LAN as the browser */
        fprintf(f, "{\"active\":true,\"source\":\"squeeze\",\"title\":\"%s\","
                   "\"artist\":\"%s\",\"album\":\"%s\",\"playing\":%s,"
                   "\"artwork\":false,\"artwork_url\":\"%s\",\"updated\":%ld}\n",
                t, a, b, g_ma_mode == 1 ? "true" : "false", g_ma_art,
                (long)time(NULL));
    }
    fclose(f);
    if (rename(tmp, META_JSON) < 0) unlink(tmp);
    g_meta_ours = active;
}

static void ma_parse_line(char *line) {
    char pending_id[64] = "";
    char title[128] = "", artist[128] = "", album[128] = "", art[256] = "";
    char *tok, *save = NULL, *p;
    int vol = -1, mode = -1, power = -1;
    /* the reply lists the current track and the next one, each opened by its own
     * "playlist index"; -2 keeps the fields before the first block out */
    int cur = -1, idx = -2, mode_changed = 0;

    /* "mixer volume ?" answers with the query echoed back and then the value,
     * everything else is key:value pairs - and in those the key arrives
     * percent-encoded, so this can only ever match the query form. */
    p = strstr(line, " mixer volume ");
    if (p) {
        p += 14;
        if (*p == '?' && p[1] == ' ') p += 2;
        if (isdigit((unsigned char)*p)) vol = atoi(p);
    }

    for (tok = strtok_r(line, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        char *c;
        url_decode(tok);
        c = strchr(tok, ':');
        if (!c) continue;
        *c = '\0';
        if (!strcmp(tok, "playerid")) {
            snprintf(pending_id, sizeof(pending_id), "%s", c + 1);
        } else if (!strcmp(tok, "ip")) {
            char *colon = strchr(c + 1, ':');
            if (colon) *colon = '\0';
            if (!g_ma_id[0] && pending_id[0] && ma_ip_is_ours(c + 1)) {
                snprintf(g_ma_id, sizeof(g_ma_id), "%s", pending_id);
                LOG_INFO("Music Assistant player is %s", g_ma_id);
            }
        } else if (!strcmp(tok, "mixer volume")) {
            vol = atoi(c + 1);
        } else if (!strcmp(tok, "mode")) {
            mode = !strcmp(c + 1, "play") ? 1 : !strcmp(c + 1, "pause") ? 2 : 0;
        } else if (!strcmp(tok, "power")) {
            power = atoi(c + 1);
        } else if (!strcmp(tok, "playlist_cur_index")) {
            cur = atoi(c + 1);
        } else if (!strcmp(tok, "playlist index")) {
            idx = atoi(c + 1);
        } else if (!strcmp(tok, "current_title")) {
            /* a radio stream carries its name here and no track title at all */
            if (!title[0]) snprintf(title, sizeof(title), "%s", c + 1);
        } else if (idx == cur) {
            if (!strcmp(tok, "title")) {
                snprintf(title, sizeof(title), "%s", c + 1);
            } else if (!strcmp(tok, "artist")) {
                snprintf(artist, sizeof(artist), "%s", c + 1);
            } else if (!strcmp(tok, "album")) {
                snprintf(album, sizeof(album), "%s", c + 1);
            } else if (!strcmp(tok, "artwork_url")) {
                snprintf(art, sizeof(art), "%s", c + 1);
            }
        }
    }

    int was_owner = ma_active();
    if (power >= 0) g_ma_power = power;
    if (mode >= 0 && mode != g_ma_mode) {
        g_ma_mode = mode;
        mode_changed = 1;
        LOG_INFO("Music Assistant is %s", mode == 1 ? "playing" : mode == 2 ? "paused" : "stopped");
    }
    if (vol >= 0 && vol != g_ma_vol) {
        g_ma_vol = vol;
        mqtt_publish_ma_volume(vol);
        LOG_INFO("Music Assistant volume: %d%%", vol);
        if (ma_active()) vol_publish_visible();
    }
    /* MA stopping hands the level back to Master and vice versa - without this HA
     * keeps showing the number of whoever used to own it. -o hands MA the level as
     * soon as it connects and the Slimproto watcher has usually seeded g_ma_vol by
     * then, so there may be no transition to hook at all: keep asserting unity for
     * as long as MA owns it. alsa_ramp_master is a read and a compare when nothing
     * moved. */
    if (ma_active()) {
        ma_sync_master(1);
        if (!was_owner) vol_publish_visible();
    } else if (was_owner) {
        ma_sync_master(0);
        vol_publish_visible();
    }

    /* The cache is only worth touching while MA is the one making sound, and the
     * poll runs every second, so only on a real change. Whoever else played last
     * keeps its own entry: we clear the file only if we are the one who filled it. */
    if (g_ma_mode == 1 || g_ma_mode == 2) {
        if (title[0] && (mode_changed || strcmp(title, g_ma_title) ||
                         strcmp(artist, g_ma_artist) || strcmp(album, g_ma_album) ||
                         strcmp(art, g_ma_art))) {
            snprintf(g_ma_title, sizeof(g_ma_title), "%s", title);
            snprintf(g_ma_artist, sizeof(g_ma_artist), "%s", artist);
            snprintf(g_ma_album, sizeof(g_ma_album), "%s", album);
            snprintf(g_ma_art, sizeof(g_ma_art), "%s", art);
            meta_write(1);
            LOG_INFO("Now playing: %s - %s", g_ma_artist, g_ma_title);
        }
    } else if (g_meta_ours) {
        g_ma_title[0] = g_ma_artist[0] = g_ma_album[0] = g_ma_art[0] = '\0';
        meta_write(0);
    }
}

static void ma_poll_cb(struct uloop_timeout *t) {
    (void)t;
    if (!ma_connected()) return;
    if (!g_ma_id[0]) {
        ma_write("players 0 100\n");
        uloop_timeout_set(&g_ma_poll_t, MA_POLL_MS);
        return;
    }
    ma_send("status - 1 tags:alKd");
    /* status carries "mixer volume" only while the player is powered on, and
     * with owns_volume the keys still move MA's own number when it is off */
    if (g_ma_owns_volume && g_ma_power != 1) ma_send("mixer volume ?");
    uloop_timeout_set(&g_ma_poll_t, MA_POLL_MS);
}

static void ma_fd_cb(struct uloop_fd *ufd, unsigned int events) {
    (void)events;
    if (g_ma_connecting) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(g_ma_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
            LOG_WARN("MA CLI connect to %s:%d failed: %s", ma_host(), g_ma_port,
                     strerror(err ? err : errno));
            ma_disconnect(1);
            return;
        }
        g_ma_connecting = 0;
        g_ma_retry_ms = MA_RETRY_MIN_MS;
        uloop_fd_delete(&g_ma_ufd);
        uloop_fd_add(&g_ma_ufd, ULOOP_READ);
        LOG_INFO("Music Assistant CLI connected to %s:%d%s", ma_host(), g_ma_port,
                 g_ma_owns_volume ? " (owns the volume)" : "");
        g_ma_poll_t.cb = ma_poll_cb;
        ma_poll_cb(&g_ma_poll_t);
        return;
    }

    for (;;) {
        ssize_t n = read(g_ma_fd, g_ma_rx + g_ma_rxlen, sizeof(g_ma_rx) - g_ma_rxlen - 1);
        char *nl, *start;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            ma_disconnect(1);
            return;
        }
        if (n == 0) {
            LOG_WARN("MA CLI closed the connection");
            ma_disconnect(1);
            return;
        }
        g_ma_rxlen += n;
        g_ma_rx[g_ma_rxlen] = '\0';
        start = g_ma_rx;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            if (*start) ma_parse_line(start);
            start = nl + 1;
        }
        g_ma_rxlen = strlen(start);
        memmove(g_ma_rx, start, g_ma_rxlen + 1);
        /* a reply longer than the buffer means we lost sync - drop it */
        if (g_ma_rxlen >= sizeof(g_ma_rx) - 1) g_ma_rxlen = 0;
    }
    if (ufd->eof || ufd->error) ma_disconnect(1);
}

static void ma_retry_cb(struct uloop_timeout *t) {
    (void)t;
    ma_connect();
}

static void ma_connect(void) {
    const char *host = ma_host();
    struct sockaddr_in sa;
    int fd;

    if (g_ma_fd >= 0 || !host[0]) return;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(g_ma_port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            LOG_WARN("Cannot resolve Music Assistant host %s", host);
            ma_arm_retry();
            return;
        }
        sa.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ma_arm_retry();
        return;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
        LOG_WARN("MA CLI connect to %s:%d failed: %s", host, g_ma_port, strerror(errno));
        close(fd);
        ma_arm_retry();
        return;
    }
    g_ma_fd = fd;
    g_ma_connecting = 1;
    g_ma_rxlen = 0;
    g_ma_ufd.fd = fd;
    g_ma_ufd.cb = ma_fd_cb;
    uloop_fd_add(&g_ma_ufd, ULOOP_READ | ULOOP_WRITE);
}

/* Someone else moved the control - librespot's alsa mixer, amixer, ha_ducking.
 * Follow it so the MCU and MQTT stay in sync. Our own writes land here too, but
 * the value already matches g_user_vol by then so they stop right here. */
static void mixer_event_cb(struct uloop_fd *ufd, unsigned int events) {
    (void)ufd;
    (void)events;
    if (!g_mixer) return;
    snd_mixer_handle_events(g_mixer);

    if (g_ramp_target < 0) {          /* mid-ramp events are our own writes */
        int vol = alsa_get_master();
        /* the last piece of a ramp clears g_ramp_target before it writes, so its
         * event lands here too - and mute ramps to 0, which used to be adopted as
         * "the user chose 0", persisted, and then restored on unmute: silence. */
        if (vol >= 0 && vol != g_user_vol && vol != g_master_set_vol && !ma_active()) {
            g_user_vol = vol;
            mcu_report_state("VOL", g_user_vol);
            mqtt_publish_volume(g_user_vol);
            vol_touch();
            LOG_INFO("Master volume changed externally: %d%%", g_user_vol);
        }
    }

    /* Music Assistant lands here: squeezelite converts the server's gain to dB
     * and writes this control on every volume change and every reconnect. That
     * write is a free trigger, but the raw value is squeezelite's own curve, not
     * MA's slider - so ask the server what it actually thinks. Without the CLI
     * the softvol position is still better than nothing. */
    if (ma_connected() && g_ma_id[0]) {
        ma_send("mixer volume ?");
    } else if (g_ma_fd < 0) {
        /* only with no CLI socket at all: mid-connect the server's own number is
         * a second away, and publishing the softvol position instead showed HA a
         * wrong level on every restart. */
        int ma = alsa_get_pct(MA_VOL_CTL);
        if (ma >= 0 && ma != g_ma_vol) {
            g_ma_vol = ma;
            mqtt_publish_ma_volume(ma);
            LOG_INFO("Music Assistant level changed externally: %d%%", ma);
        }
    }
}

/* Per-app softvol stages - they exist only for ha_ducking to pull down. Spotify
 * is not in here on purpose: librespot owns that control now and would fight
 * both the park-at-max on startup and the abandoned-duck sweep below. VoIP went
 * with voip_in when it was cut from asound.conf. */
static const char *k_duck_controls[] = {"AirPlay", "Music", "TTS", "Alarm", "Timer", "Squeeze", NULL};

/* ha_ducking.sh holds an flock on this file for its whole run, so the lock
 * frees itself no matter how the script dies. A control sitting below max
 * with no live holder means a duck was abandoned - restore it, or that one
 * source stays quieter than the others forever (bit us as "MA is quieter
 * than Spotify"). */
#define DUCK_LOCK_FILE "/tmp/ha_ducking.lock"

static int duck_lock_held(void) {
    int fd = open(DUCK_LOCK_FILE, O_RDWR);
    if (fd < 0) return 0;
    int got_it = (flock(fd, LOCK_EX | LOCK_NB) == 0);
    close(fd);
    return !got_it;
}

/* player_control.sh parks Squeeze/AirPlay at 0% while they are paused, and
 * says so in this file - restoring those would unpause them. */
static int player_paused(const char *word) {
    char buf[128];
    int fd = open("/tmp/paused_players.state", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    /* echo ends the file with \n, which would break the word matching below */
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') buf[i] = ' ';
    char pat[32];
    snprintf(pat, sizeof(pat), " %s ", word);
    char *sp = alloca(n + 3);
    snprintf(sp, n + 3, " %s ", buf);
    return (strstr(sp, pat) != NULL);
}

static void check_duck_controls(void) {
    if (duck_lock_held()) return;

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);
    for (int i = 0; k_duck_controls[i]; i++) {
        const char *name = k_duck_controls[i];
        if (!strcmp(name, "Squeeze") && player_paused("squeeze")) continue;
        if (!strcmp(name, "AirPlay") && player_paused("airplay")) continue;
        snd_mixer_selem_id_set_name(sid, name);
        snd_mixer_elem_t *elem = snd_mixer_find_selem(g_mixer, sid);
        if (!elem) continue;
        long min = 0, max = 100, cur = 0;
        snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
        snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &cur);
        if (cur < max) {
            snd_mixer_selem_set_playback_volume_all(elem, max);
            LOG_WARN("%s softvol left ducked at %ld/%ld with no active ha_ducking - restored",
                     k_duck_controls[i], cur, max);
        }
    }
}

/* softvol controls are created when the PCM is first opened and don't survive a
 * reboot, and every client that wants a fader looks it up before it opens a pcm:
 * squeezelite falls back to its own software gain, librespot errors out and procd
 * respawns it forever, shairport-sync reverts to software volume. ha_ducking.sh
 * has the same problem for a stage nothing has played through yet. So open the
 * whole chain once at startup - each open costs a few ms and creates the controls
 * of everything it slaves to as well. master first, the rest in any order. */
static void alsa_probe_pcms(void) {
    static const char *const pcms[] = {
        "master", "squeeze_in", "spotify_in", "airplay_in",
        "music_in", "tts_in", "alarm_in", "timer_in", NULL
    };

    for (int i = 0; pcms[i]; i++) {
        snd_pcm_t *pcm = NULL;
        int err = snd_pcm_open(&pcm, pcms[i], SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
        if (err < 0) {
            LOG_WARN("Cannot open ALSA '%s' (%s) - its fader will be missing",
                     pcms[i], snd_strerror(err));
            continue;
        }
        snd_pcm_close(pcm);
    }
}

static void alsa_init(void) {
    if (g_mixer) return;
    alsa_probe_pcms();

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
    for (int i = 0; k_duck_controls[i]; i++) {
        snd_mixer_selem_id_set_name(sid, k_duck_controls[i]);
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
    /* Read the number out of the table instead of hardcoding it - the last two
       times the curve changed this line kept printing the old dB and sent the
       next reader down the wrong path. */
    {
        int mute_top = 0;
        while (mute_top < 100 && vol_curve[mute_top + 1] == 0) mute_top++;
        int dbt = vol_db10(25);
        LOG_INFO("ALSA Master volume control ready (fader, 25%% = %d.%d dB, <=%d%% = mute)",
                 dbt / 10, (-dbt) % 10, mute_top);
    }
}

static void mqtt_publish_volume(int vol) {
    if (!g_mosq || !g_mqtt_enabled) return;
    char topic[128], payload[16];
    snprintf(topic, sizeof(topic), "%s/player/volume", g_topic_prefix);
    snprintf(payload, sizeof(payload), "%d", vol);
    mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    g_last_vol_pub_sec = now_mono();
    g_pending_vol_pub = -1;
}

static void mqtt_publish_mute(void) {
    if (!g_mosq || !g_mqtt_enabled) return;
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/player/mute", g_topic_prefix);
    const char *p = g_is_muted ? "ON" : "OFF";
    mosquitto_publish(g_mosq, NULL, topic, strlen(p), p, 0, true);
}

static void mqtt_publish_ma_volume(int vol) {
    if (!g_mosq || !g_mqtt_enabled) return;
    char topic[128], payload[16];
    snprintf(topic, sizeof(topic), "%s/player/ma_volume", g_topic_prefix);
    snprintf(payload, sizeof(payload), "%d", vol);
    mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
}

static void mqtt_publish_sleep(int minutes) {
    if (!g_mosq || !g_mqtt_enabled) return;
    char topic[128], payload[16];
    snprintf(topic, sizeof(topic), "%s/player/sleep", g_topic_prefix);
    snprintf(payload, sizeof(payload), "%d", minutes);
    mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    g_sleep_left_pub = minutes;
}

/* MQTT sleep timer: an absolute deadline, unlike -s which watches for silence.
 * Deliberately not gated on charging - "power off in 30 min" has to work while
 * the speaker is plugged in and playing, which is exactly when it is asked for. */
static void sleep_timer_arm(int minutes) {
    if (minutes <= 0) {
        g_sleep_deadline = 0;
        mqtt_publish_sleep(0);
        LOG_INFO("Sleep timer cancelled");
        return;
    }
    if (minutes > 240) minutes = 240;
    g_sleep_deadline = now_mono() + (time_t)minutes * 60;
    mqtt_publish_sleep(minutes);
    LOG_INFO("Sleep timer armed: %d min", minutes);
}

/* AXX+MUT is the same family as AXX+VOL, which only mirrors state in the MCU, and
 * the TPA3116 mute pin is strapped - so the cut has to happen in softvol, where raw
 * 0 is real digital silence. MUT still goes out so the MCU's bookkeeping matches.
 * g_user_vol is left alone: that is what unmute ramps back to. */
static void set_hardware_mute(int mute) {
    g_is_muted = mute ? 1 : 0;
    alsa_ramp_master(g_is_muted ? 0 : ma_active() ? 100 : g_user_vol);
    uart_send(g_is_muted ? CMD_MUTE : CMD_UNMUTE);
    mqtt_publish_mute();
    LOG_INFO("Master mute %s", g_is_muted ? "engaged" : "released");
}

/* Stock keeps the volume in nvram; we had nothing, so every restart slammed the
 * level back to default_volume. Debounced because this is NOR flash - a volume
 * ramp must not turn into one jffs2 node per step. */
#define VOL_STATE_FILE  "/etc/mcud.volume"
#define BAT_STATE_FILE  "/tmp/battery_status"
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

/* mcud gets restarted while the speaker runs on battery, and the MCU only
 * volunteers a level when it changes - so a fresh process would report the 100%
 * default to HA and hold it there until the charge actually moved. The file is
 * our own last observation, rewritten on every MCU+BAT+. */
static void bat_load(void) {
    FILE *f = fopen(BAT_STATE_FILE, "r");
    if (!f) return;
    int v = -1;
    if (fscanf(f, "MCU+BAT+%d", &v) == 1 && v >= 0 && v <= 100) {
        g_battery_pct = v;
        g_last_bat = v;
    }
    fclose(f);
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
    /* HA and the keys send a level, never an explicit unmute, so any level means
     * unmute - otherwise the switch would read ON while sound came back out */
    if (g_is_muted) {
        g_is_muted = 0;
        uart_send(CMD_UNMUTE);
        mqtt_publish_mute();
    }
    /* While MA is the live source, its fader is the number everyone looks at.
     * Moving Master as well would stack two curves and the dB add up. */
    if (ma_active() && ma_send("mixer volume %d", vol) == 0) {
        g_ma_vol = vol;
        mqtt_publish_ma_volume(vol);
        vol_publish_visible();
        LOG_INFO("Music Assistant volume: %d%%", vol);
        return;
    }
    g_user_vol = vol;
    alsa_ramp_master(g_user_vol);

    /* No AXX+VOL here. The MCU does not attenuate anything, it only bookkeeps the
     * number - and it restates whatever we give it as a MCU+VOL+nnn that looks like
     * a key press. It still gets an answer when it asks with MCU+VOL+GET. */
    mqtt_publish_volume(g_user_vol);
    vol_touch();
    if (g_user_vol == 0)
        LOG_INFO("Master volume: %d%% (muted)", g_user_vol);
    else {
        int db10 = vol_db10(g_user_vol);
        LOG_INFO("Master volume: %d%% (%d.%d dB)", g_user_vol, db10 / 10, (-db10) % 10);
    }
}

/* A key press has to move a fixed number of dB, not a fixed percent: on the
 * fader curve 0->5% is 26 dB while 95->100% is barely half of one. raw is
 * dB-linear, so step there and convert back to the percent everyone reports.
 * Only the sign of delta matters now. */
static void adjust_user_volume(int delta) {
    long min = 0, max = 1000;
    /* MA applies its own curve server-side, so hand it a relative step and read
     * back whatever it decided rather than predicting the result. */
    if (ma_active()) {
        if (g_is_muted) {
            g_is_muted = 0;
            uart_send(CMD_UNMUTE);
            mqtt_publish_mute();
        }
        if (ma_send("mixer volume %+d", delta > 0 ? g_ma_step : -g_ma_step) == 0) {
            ma_send("mixer volume ?");
            return;
        }
    }
    if (!g_master_elem ||
        snd_mixer_selem_get_playback_volume_range(g_master_elem, &min, &max) < 0) {
        set_user_volume(g_user_vol + (delta > 0 ? VOL_STEP_DB : -VOL_STEP_DB));
        return;
    }
    /* 16 presses from full to mute, so ~3.8 dB each on Master's -60 dB span. */
    long step = ((long)(max - min) + 8) / 16;
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

    char cmd_topic[128], vol_topic[128], src_topic[128], mute_topic[128];
    char ma_topic[128], sleep_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/player/command", g_topic_prefix);
    snprintf(vol_topic, sizeof(vol_topic), "%s/player/volume/set", g_topic_prefix);
    snprintf(src_topic, sizeof(src_topic), "%s/player/source/set", g_topic_prefix);
    snprintf(mute_topic, sizeof(mute_topic), "%s/player/mute/set", g_topic_prefix);
    snprintf(ma_topic, sizeof(ma_topic), "%s/player/ma_volume/set", g_topic_prefix);
    snprintf(sleep_topic, sizeof(sleep_topic), "%s/player/sleep/set", g_topic_prefix);

    if (strcmp(msg->topic, cmd_topic) == 0) {
        LOG_INFO("MQTT Command received: [%s]", payload);
        g_last_activity_time = now_mono();
        if (strcasecmp(payload, "UNMUTE") == 0) {
            set_hardware_mute(0);
        } else if (strcasecmp(payload, "MUTE") == 0) {
            set_hardware_mute(1);
        } else if (strcasecmp(payload, "VOLUP") == 0) {
            adjust_user_volume(1);
        } else if (strcasecmp(payload, "VOLDOWN") == 0) {
            adjust_user_volume(-1);
        } else if (!strncasecmp(payload, "PRESET", 6) && payload[6] >= '1' && payload[6] <= '4') {
            fire_preset(payload[6] - '0', 0);
        } else if (strcasecmp(payload, "REBOOT") == 0) {
            /* async: procd stops us as part of the reboot, and blocking here
             * would hold the loop while the broker waits for our PUBACK */
            spawn_async("/sbin/reboot");
        } else if (strcasecmp(payload, "POWEROFF") == 0) {
            LOG_INFO("Power off requested over MQTT");
            uloop_end();
            graceful_shutdown();
        } else {
            /* Transport goes through the one script that knows how to reach each
             * player - librespot over its fifo, the others by ducking their own
             * softvol. Muting Master instead would leave the sender running. */
            const char *ma_cmd = NULL;
            if (strcasecmp(payload, "TOGGLE") == 0)      ma_cmd = "button play_pause";
            else if (strcasecmp(payload, "PLAY") == 0) {
                if (ma_active() && g_ma_mode == 0) {
                    if (ma_start() == 0) {
                        ma_send("status - 1 tags:alKd");
                        return;
                    }
                }
                ma_cmd = "mode play";
            }
            else if (strcasecmp(payload, "PAUSE") == 0)  ma_cmd = "mode pause";
            else if (strcasecmp(payload, "STOP") == 0)   ma_cmd = "mode stop";
            else if (strcasecmp(payload, "NEXT") == 0)   ma_cmd = "playlist index +1";
            else if (strcasecmp(payload, "PREV") == 0)   ma_cmd = "playlist index -1";
            /* MA's stream is pushed from the server, so ducking squeezelite's
             * softvol only mutes it - the transport has to go to the server. */
            if (ma_cmd && ma_active() && ma_send("%s", ma_cmd) == 0) {
                ma_send("status - 1 tags:alKd");
                return;
            }
            const char *act = NULL;
            if (strcasecmp(payload, "PLAY") == 0)        act = "play";
            else if (strcasecmp(payload, "PAUSE") == 0)  act = "pause";
            else if (strcasecmp(payload, "TOGGLE") == 0) act = "toggle";
            else if (strcasecmp(payload, "NEXT") == 0)   act = "next";
            else if (strcasecmp(payload, "PREV") == 0)   act = "prev";
            else if (strcasecmp(payload, "STOP") == 0)   act = "stop";
            if (!act) { LOG_WARN("MQTT unknown command [%s]", payload); return; }
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "/usr/bin/player_control.sh %s", act);
            spawn_async(cmd);
        }
    } else if (strcmp(msg->topic, vol_topic) == 0) {
        g_last_activity_time = now_mono();
        int vol = atoi(payload);
        set_user_volume(vol);
    } else if (strcmp(msg->topic, src_topic) == 0) {
        /* still subscribed only to swallow a retained payload left by the select
         * this used to publish - the MCU ignores every input write we send */
        LOG_WARN("MQTT source [%s] ignored, the MCU owns the input", payload);
    } else if (strcmp(msg->topic, mute_topic) == 0) {
        g_last_activity_time = now_mono();
        set_hardware_mute(strcasecmp(payload, "ON") == 0);
    } else if (strcmp(msg->topic, ma_topic) == 0) {
        int v = atoi(payload);
        g_last_activity_time = now_mono();
        if (ma_connected() && g_ma_id[0]) {
            if (ma_send("mixer volume %d", v) == 0) {
                ma_send("mixer volume ?");
                return;
            }
        }
        if (alsa_set_pct(MA_VOL_CTL, v) < 0) {
            LOG_WARN("No %s control - is squeezelite running?", MA_VOL_CTL);
            return;
        }
        /* the mixer event this write raises publishes the state, so no echo here */
        LOG_INFO("Music Assistant level set to %d%% over MQTT", v);
    } else if (strcmp(msg->topic, sleep_topic) == 0) {
        sleep_timer_arm(atoi(payload));
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
static void mqtt_send_sensor(const char *sensor, const char *val);

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

/* Which leg carries the default route, for HA. Both are up here (eth metric 700,
 * wifi 600), so this is "where the audio actually goes", not "what is plugged in".
 * -1 means the probe in main() has not run yet, and that is not the same as being
 * offline: mqtt_setup() runs before it, and the first discovery burst used to
 * announce a freshly booted, perfectly online speaker as uplink=offline. */
static const char *uplink_name(void) {
    if (g_net_state < 0) return "unknown";
    if (g_net_state != 1) return "offline";
    return g_eth ? "eth" : "wifi";
}

/* One device block for every entity, because HA merges them per identifier and
 * the last payload published wins - the sensors carried a shorter block and kept
 * stripping the model and manufacturer back off the device again. */
#define HA_DEV \
    "\"device\":{\"identifiers\":[\"audiopro_c3\"]," \
    "\"name\":\"Audio Pro C3\"," \
    "\"manufacturer\":\"Audio Pro\"," \
    "\"model\":\"Addon C3 (Linkplay A28)\"," \
    "\"sw_version\":\"OpenWrt 23.05.5\"}"

static void mqtt_send_discovery(void) {
    if (!g_mosq || !g_mqtt_enabled) return;

    const char *buttons[] = {
        "preset_1", "preset_2", "preset_3", "preset_4",
        "play_pause", "source", "bluetooth"
    };

    for (int i = 0; i < 7; i++) {
        char topic[256], payload[768];
        snprintf(topic, sizeof(topic), "homeassistant/device_automation/audiopro_c3/%s/config", buttons[i]);
        snprintf(payload, sizeof(payload),
            "{\"automation_type\":\"trigger\","
            "\"type\":\"button_short_press\","
            "\"subtype\":\"%s\","
            "\"topic\":\"%s/button/%s\","
            "\"payload\":\"pressed\","
            HA_DEV "}",
            buttons[i], g_topic_prefix, buttons[i]);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        char topic[256], payload[768];
        snprintf(topic, sizeof(topic), "homeassistant/sensor/audiopro_c3/battery/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Battery\","
            "\"device_class\":\"battery\","
            "\"state_class\":\"measurement\","
            "\"unit_of_measurement\":\"%%\","
            "\"state_topic\":\"%s/sensor/battery\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_battery\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        /* Read-only again. The select this used to be offered a switch the MCU
         * refuses: MCU+CAP+PLM00000006 says line-in and bluetooth are the MCU's
         * own inputs and it ignored every AXX+PLM we sent. The empty retained
         * payload deletes the select - leave it out and HA keeps a second,
         * permanently stale copy. */
        char topic[256], payload[768];
        snprintf(topic, sizeof(topic), "homeassistant/select/audiopro_c3/input/config");
        mosquitto_publish(g_mosq, NULL, topic, 0, "", 0, true);

        snprintf(topic, sizeof(topic), "homeassistant/sensor/audiopro_c3/source/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Source\","
            "\"icon\":\"mdi:audio-input-stereo-minijack\","
            "\"state_topic\":\"%s/sensor/source\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_source\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        char topic[256], payload[768];
        snprintf(topic, sizeof(topic), "homeassistant/number/audiopro_c3/volume/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Volume\","
            "\"icon\":\"mdi:volume-high\","
            "\"state_topic\":\"%s/player/volume\","
            "\"command_topic\":\"%s/player/volume/set\","
            "\"availability_topic\":\"%s/status\","
            "\"min\":0,\"max\":100,\"step\":1,\"unit_of_measurement\":\"%%\","
            "\"unique_id\":\"audiopro_c3_volume\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        char topic[256], payload[768];
        snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/audiopro_c3/charging/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Charging\","
            "\"device_class\":\"battery_charging\","
            "\"state_topic\":\"%s/sensor/charging\","
            "\"payload_on\":\"true\",\"payload_off\":\"false\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_charging\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        char topic[256], payload[768];
        snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/audiopro_c3/battery_low/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Battery low\","
            "\"device_class\":\"battery\","
            "\"state_topic\":\"%s/sensor/battery_alert\","
            "\"payload_on\":\"low\",\"payload_off\":\"normal\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_battery_low\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        char topic[256], payload[768];
        snprintf(topic, sizeof(topic), "homeassistant/sensor/audiopro_c3/uplink/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Uplink\","
            "\"icon\":\"mdi:lan-connect\","
            "\"state_topic\":\"%s/sensor/uplink\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_uplink\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);

        snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/audiopro_c3/playing/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Playing\","
            "\"icon\":\"mdi:music-note\","
            "\"state_topic\":\"%s/sensor/playing\","
            "\"payload_on\":\"true\",\"payload_off\":\"false\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_playing\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        char topic[256], payload[768];
        snprintf(topic, sizeof(topic), "homeassistant/switch/audiopro_c3/mute/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Mute\","
            "\"icon\":\"mdi:volume-off\","
            "\"state_topic\":\"%s/player/mute\","
            "\"command_topic\":\"%s/player/mute/set\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_mute\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    /* Transport as plain buttons: MQTT discovery has no media_player platform, and
     * the Music Assistant entity disappears from the dashboard the moment MA is not
     * the one playing. These reach the players directly and work either way. */
    {
        static const struct { const char *id, *name, *icon, *press; } btns[] = {
            { "play_pause", "Play/pause",     "mdi:play-pause",    "TOGGLE"  },
            { "next",       "Next track",     "mdi:skip-next",     "NEXT"    },
            { "previous",   "Previous track", "mdi:skip-previous", "PREV"    },
            { "preset_1",   "Preset 1",        "mdi:numeric-1-box", "PRESET1" },
            { "preset_2",   "Preset 2",        "mdi:numeric-2-box", "PRESET2" },
            { "preset_3",   "Preset 3",        "mdi:numeric-3-box", "PRESET3" },
            { "preset_4",   "Preset 4",        "mdi:numeric-4-box", "PRESET4" },
        };
        for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
            char topic[256], payload[768];
            snprintf(topic, sizeof(topic),
                     "homeassistant/button/audiopro_c3/%s/config", btns[i].id);
            snprintf(payload, sizeof(payload),
                "{\"name\":\"%s\",\"icon\":\"%s\","
                "\"command_topic\":\"%s/player/command\","
                "\"payload_press\":\"%s\","
                "\"availability_topic\":\"%s/status\","
                "\"unique_id\":\"audiopro_c3_btn_%s\","
                HA_DEV "}",
                btns[i].name, btns[i].icon, g_topic_prefix, btns[i].press,
                g_topic_prefix, btns[i].id);
            mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
        }
    }

    /* Power controls. Restart comes back on its own; the off button does not -
     * the MCU cuts the amp rail and only the front panel key wakes it again,
     * which is exactly what the stock speaker does. */
    {
        char topic[256], payload[768];
        snprintf(topic, sizeof(topic), "homeassistant/button/audiopro_c3/restart/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Restart\",\"device_class\":\"restart\","
            "\"entity_category\":\"config\","
            "\"command_topic\":\"%s/player/command\","
            "\"payload_press\":\"REBOOT\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_restart\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);

        snprintf(topic, sizeof(topic), "homeassistant/button/audiopro_c3/power_off/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Power off\",\"icon\":\"mdi:power\","
            "\"entity_category\":\"config\","
            "\"command_topic\":\"%s/player/command\","
            "\"payload_press\":\"POWEROFF\","
            "\"availability_topic\":\"%s/status\","
            "\"unique_id\":\"audiopro_c3_power_off\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    /* The fader Music Assistant drives. It is a second attenuator in series with
     * Volume, so a low value here is the "MA sounds quieter than Spotify" bug -
     * park it at 100% and let Volume be the only level in the house. */
    {
        char topic[256], payload[768];
        snprintf(topic, sizeof(topic), "homeassistant/number/audiopro_c3/ma_volume/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Music Assistant level\","
            "\"icon\":\"mdi:tune-variant\","
            "\"entity_category\":\"config\","
            "\"state_topic\":\"%s/player/ma_volume\","
            "\"command_topic\":\"%s/player/ma_volume/set\","
            "\"availability_topic\":\"%s/status\","
            "\"min\":0,\"max\":100,\"step\":1,\"unit_of_measurement\":\"%%\","
            "\"unique_id\":\"audiopro_c3_ma_volume\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);

        snprintf(topic, sizeof(topic), "homeassistant/number/audiopro_c3/sleep/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Sleep timer\","
            "\"icon\":\"mdi:timer-outline\","
            "\"state_topic\":\"%s/player/sleep\","
            "\"command_topic\":\"%s/player/sleep/set\","
            "\"availability_topic\":\"%s/status\","
            "\"min\":0,\"max\":240,\"step\":5,\"unit_of_measurement\":\"min\","
            "\"mode\":\"box\","
            "\"unique_id\":\"audiopro_c3_sleep\","
            HA_DEV "}",
            g_topic_prefix, g_topic_prefix, g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    char sub_cmd[128], sub_vol[128], sub_src[128], sub_mute[128];
    char sub_ma[128], sub_sleep[128];
    snprintf(sub_cmd, sizeof(sub_cmd), "%s/player/command", g_topic_prefix);
    snprintf(sub_vol, sizeof(sub_vol), "%s/player/volume/set", g_topic_prefix);
    snprintf(sub_src, sizeof(sub_src), "%s/player/source/set", g_topic_prefix);
    snprintf(sub_mute, sizeof(sub_mute), "%s/player/mute/set", g_topic_prefix);
    snprintf(sub_ma, sizeof(sub_ma), "%s/player/ma_volume/set", g_topic_prefix);
    snprintf(sub_sleep, sizeof(sub_sleep), "%s/player/sleep/set", g_topic_prefix);
    mosquitto_subscribe(g_mosq, NULL, sub_cmd, 0);
    mosquitto_subscribe(g_mosq, NULL, sub_vol, 0);
    mosquitto_subscribe(g_mosq, NULL, sub_src, 0);
    mosquitto_subscribe(g_mosq, NULL, sub_mute, 0);
    mosquitto_subscribe(g_mosq, NULL, sub_ma, 0);
    mosquitto_subscribe(g_mosq, NULL, sub_sleep, 0);

    char status_topic[128];
    snprintf(status_topic, sizeof(status_topic), "%s/status", g_topic_prefix);
    mosquitto_publish(g_mosq, NULL, status_topic, 6, "online", 0, true);

    /* HA leaves a discovered entity unknown until something lands on its state
     * topic, and source/battery/charging only publish when they change - so
     * after every reconnect the panel sat half empty until the user pressed a
     * button. Retained, so a broker restart does not blank it either. */
    char bat[16];
    snprintf(bat, sizeof(bat), "%d", g_battery_pct);
    mqtt_send_sensor("battery", bat);
    mqtt_send_sensor("charging", g_is_charging ? "true" : "false");
    mqtt_send_sensor("battery_alert", g_battery_pct <= 15 ? "low" : "normal");
    mqtt_send_sensor("source", get_source_name(g_current_source));
    mqtt_send_sensor("playing", g_is_playing ? "true" : "false");
    mqtt_send_sensor("uplink", uplink_name());
    mqtt_publish_mute();
    vol_publish_visible();
    /* only the CLI knows MA's own number; the softvol percent is a different
     * scale entirely, and seeding from it used to publish a wrong level. */
    if (g_ma_vol >= 0) mqtt_publish_ma_volume(g_ma_vol);
    else if (!ma_connected()) {
        int ma = alsa_get_pct(MA_VOL_CTL);
        if (ma >= 0) mqtt_publish_ma_volume(ma);
    }
    mqtt_publish_sleep(g_sleep_deadline
                       ? (int)((g_sleep_deadline - now_mono() + 59) / 60) : 0);

    LOG_INFO("MQTT Home Assistant discovery published, %s/player/# subscribed", g_topic_prefix);
}

static void mqtt_send_button(const char *name) {
    if (!g_mosq || !g_mqtt_enabled) return;
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/button/%s", g_topic_prefix, name);
    mosquitto_publish(g_mosq, NULL, topic, 7, "pressed", 0, false);
}

/* every preset trigger comes through here - front-panel key, mqtt command and the web's
 * ubus call - so all three get the same beep and the same handler. announce is off for
 * the mqtt one: HA sent it, publishing the trigger back would only come round again. */
static void fire_preset(int preset, int announce) {
    char buf[96];
    if (announce) {
        snprintf(buf, sizeof(buf), "preset_%d", preset);
        mqtt_send_button(buf);
        ubus_notify_button(buf);
    }
    play_sound(SOUND_PRESET);
    snprintf(buf, sizeof(buf), "/usr/bin/audiopro_preset_handler.sh %d", preset);
    spawn_async(buf);
}

static void mqtt_send_sensor(const char *sensor, const char *val) {
    if (!g_mosq || !g_mqtt_enabled) return;
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/sensor/%s", g_topic_prefix, sensor);
    mosquitto_publish(g_mosq, NULL, topic, strlen(val), val, 0, true);
}

/* libmosquitto's own loop_start() thread is unusable here. Its callbacks run on
 * whichever thread drives the loop, and a callback that ends up in
 * ubus_send_event() blocks: that call is synchronous, libubus parks in
 * poll(ubus_fd, POLLIN|POLLERR, -1) until the reply arrives, and uloop on the main
 * thread owns that fd and consumes it first. Measured 2026-08-25 with the first HA
 * command ever sent: thread "mosquitto loop" stuck in exactly that poll, no PINGREQ
 * after it, broker logged "exceeded timeout" 96 s later, and mcud still thought it was
 * connected - socket in CLOSE_WAIT with 71 bytes unread and every command topic dead.
 * Running the protocol from uloop keeps every callback on the thread that owns ubus,
 * the mixer and the UART, which is also the only thread that may touch them. */
static struct uloop_fd g_mqtt_ufd;
static struct uloop_timeout g_mqtt_timer;
static unsigned int g_mqtt_armed;   /* flags g_mqtt_ufd is currently in uloop with */
static int g_mqtt_retry;            /* ticks to wait before the next connect attempt */
static int g_mqtt_backoff = 2;      /* seconds, doubling to 60 as libmosquitto did */

static void mqtt_unarm(void) {
    if (g_mqtt_ufd.registered) uloop_fd_delete(&g_mqtt_ufd);
    g_mqtt_armed = 0;
}

/* ULOOP_WRITE only while something is actually queued: the socket is writable
 * almost always, so a permanent POLLOUT would spin uloop at 100% */
static void mqtt_arm(void) {
    int fd = mosquitto_socket(g_mosq);
    unsigned int flags;
    if (fd < 0) { mqtt_unarm(); return; }
    flags = ULOOP_READ | (mosquitto_want_write(g_mosq) ? ULOOP_WRITE : 0);
    if (g_mqtt_ufd.registered && g_mqtt_ufd.fd == fd && flags == g_mqtt_armed) return;
    if (g_mqtt_ufd.registered && g_mqtt_ufd.fd != fd) mqtt_unarm();
    g_mqtt_ufd.fd = fd;
    g_mqtt_armed = flags;
    uloop_fd_add(&g_mqtt_ufd, flags);
}

static void mqtt_fd_cb(struct uloop_fd *u, unsigned int events) {
    int rc = MOSQ_ERR_SUCCESS;
    (void)u;
    /* write first: connect_async() hands back a socket still in the TCP handshake
     * with CONNECT sitting in the out queue, and this is what flushes it */
    if (events & ULOOP_WRITE) rc = mosquitto_loop_write(g_mosq, 1);
    if (rc == MOSQ_ERR_SUCCESS && (events & ULOOP_READ)) rc = mosquitto_loop_read(g_mosq, 1);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_CONN_LOST)
        LOG_WARN("MQTT loop: %s", mosquitto_strerror(rc));
    /* both close the socket themselves on error and fire on_disconnect, so this
     * either re-arms with fresh flags or takes the dead fd back out of uloop */
    mqtt_arm();
}

static void mqtt_connect_start(void) {
    /* 20 s keepalive, not the usual 60. When the eth cable is pulled mid-track the
     * socket stays ESTABLISHED on a source address that no longer exists, and this
     * timer is the only thing that ever notices: at 60 s it took 2m49s (measured
     * 2026-08-25, 14:51:05 link down -> 14:53:54 rc=19), and Home Assistant showed
     * a stale player all that time. A PINGREQ every 20 s costs nothing here.
     * The players are re-dialled by hotplug.d/iface/40-rebind-players instead;
     * restarting mcud would reload the volume onto Master and that is audible. */
    int rc = mosquitto_connect_async(g_mosq, g_mqtt_host, g_mqtt_port, 20);
    mqtt_unarm();
    /* CONN_PENDING is the normal answer - the connect is still in flight */
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_CONN_PENDING) {
        LOG_WARN("MQTT connect to %s:%d failed (%s), retrying in %d s",
                 g_mqtt_host, g_mqtt_port, mosquitto_strerror(rc), g_mqtt_backoff);
        return;
    }
    mqtt_arm();
}

static void mqtt_timer_cb(struct uloop_timeout *t) {
    int rc;
    uloop_timeout_set(t, 1000);
    if (!g_mosq) return;
    if (mosquitto_socket(g_mosq) < 0) {
        if (g_mqtt_retry-- > 0) return;
        g_mqtt_retry = g_mqtt_backoff;
        if (g_mqtt_backoff < 60) g_mqtt_backoff *= 2;
        mqtt_connect_start();
        return;
    }
    g_mqtt_backoff = 2;
    /* keepalive lives here: loop_misc sends the PINGREQ and is what notices a
     * broker that stopped answering one */
    rc = mosquitto_loop_misc(g_mosq);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN)
        LOG_WARN("MQTT keepalive: %s", mosquitto_strerror(rc));
    mqtt_arm();
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
    /* reconnect_delay_set() is deliberately not called: it only steers
     * loop_forever()'s own retry, and the backoff now lives in mqtt_timer_cb */
    g_mqtt_ufd.cb = mqtt_fd_cb;
    g_mqtt_timer.cb = mqtt_timer_cb;
    uloop_timeout_set(&g_mqtt_timer, 1000);
    mqtt_connect_start();
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

/* The MCU owns the switch on this cabinet, so this is a mirror and not a setter.
 * Three independent reasons: MCU+CAP+PLM00000006 declares line-in and bluetooth
 * as the MCU's own inputs; every host write (AXX+PLM/CHN/MMM/PLP/AXA/BYP/SLI)
 * left its own readback at MCU+PLM+000; and stock's cached getStatusEx in this
 * speaker's config partition says plm_support 0x0 / external 0x0. No cooldown
 * here either - stock's 1499 ms throttle guards the STM8S ring buffer against
 * our writes, and applying it to an inbound announcement only makes us lie about
 * the source until the next keypress, which is what cycling the front-panel key
 * did: PLM+006 adopted, PLM+005 dropped, state stuck on bluetooth. */
static void adopt_audio_source(int source) {
    if (source < 0 || source > 2 || source == g_current_source) return;

    g_current_source = source;
    g_last_activity_time = now_mono();
    const char *src_name = get_source_name(g_current_source);

    /* Pause Music Assistant if user switched away from Wi-Fi (I2S) */
    if (source != 0 && ma_has_transport()) {
        ma_send("pause 1");
    }

    mqtt_send_sensor("source", src_name);
    ubus_notify_source(src_name);

    int fd = open("/tmp/audio_source", O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
    if (fd >= 0) {
        write(fd, src_name, strlen(src_name));
        write(fd, "\n", 1);
        close(fd);
    }
    LOG_INFO("Audio source is now %s (the MCU switched it)", src_name);
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
    g_last_activity_time = now_mono();

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
        /* stock's AXX+VOL+00%d / 0%d / 100 triple is just %03d. The MCU adopts
         * whatever we answer and restates it, so mark the time or the restatement
         * looks like a key press. */
        mcu_report_state("VOL", ma_active() ? g_ma_vol : g_user_vol);
        g_mcu_vol_tx_ms = mono_ms();
        return;
    } else if (strstr(cmd, "MCU+PLY+GET")) {
        /* asked right after a preset key is held. Stock's own reply is
         * AXX+PLY+00%d and it defaults to 1 (MCUCommandSend id 17, 0x40cf34);
         * g_is_playing comes from MA's mode, so this is a real answer. */
        uart_send(g_is_playing ? "AXX+PLY+001\n" : "AXX+PLY+000\n");
        return;
    } else if (strstr(cmd, "MCU+PLM+GET")) {
        /* stock answers this from its own stored source mode (mv_ioguard
         * ParseCommonMCUEvent 0x41783c, cfg+968), so ours is the mirror */
        mcu_report_state("PLM", plm_code(g_current_source));
        return;
    }

    if (!strncmp(cmd, "MCU+VER+", 8)) {
        snprintf(g_mcu_ver, sizeof(g_mcu_ver), "%.15s", cmd + 8);
        LOG_INFO("MCU firmware version: %s", g_mcu_ver);
        return;
    }

    if (!strncmp(cmd, "MCU+PLM+", 8) && isdigit((unsigned char)cmd[8])) {
        int src = source_from_plm(atoi(cmd + 8));
        if (src < 0) {
            LOG_INFO("MCU announced source mode %.3s - no name for it here", cmd + 8);
        } else {
            adopt_audio_source(src);
        }
        return;
    }

    /* This is the panel's volume key. MCU+KEY+VOL+/- is what the code used to wait
     * for and this MCU (fw 0109) never sends it - captured with a human pressing the
     * keys, every press arrives here instead. The value is the MCU's own counter and
     * has nothing to do with ours: it keeps one per source (line-in came back 100,
     * wifi 000) and moves in steps of MCU_VOL_STEP, so only the sign is usable.
     *
     * Nothing writes our level back with AXX+VOL any more. The MCU quantizes what we
     * send onto its own even grid and restates it as MCU+VOL+nnn a second later - a
     * -2 delta that reads exactly like a key press, which answered with another
     * AXX+VOL. That is the feedback loop that used to walk the volume to 0, and it is
     * why one press in four came back with the wrong sign. Its counter free-runs now
     * and pins at its own rails, which costs nothing: the pinned direction is the one
     * MA has already saturated.
     *
     * A jump bigger than one step is the MCU loading a stored level, which it does on
     * every source switch; that one leads the M2S/PLM pair by up to a second, so
     * magnitude is the only thing that can tell them apart. delta 0 at a rail is a
     * real press with nowhere left to go - the MCU restates a pinned counter instead
     * of moving it. */
    if (!strncmp(cmd, "MCU+VOL+", 8) && isdigit((unsigned char)cmd[8])) {
        int v = atoi(cmd + 8);
        int64_t ms = mono_ms();
        int seen = g_mcu_vol >= 0;
        int d;
        if (v < 0 || v > 100) return;
        d = seen ? v - g_mcu_vol : 0;
        g_mcu_vol = v;
        if (ms - g_mcu_vol_tx_ms < MCU_VOL_ECHO_MS) {
            LOG_DEBUG("MCU volume counter now %d%% (its echo of our own level)", v);
        } else if (ms - g_mcu_burst_ms < MCU_WAKE_BURST_MS) {
            LOG_DEBUG("MCU volume counter now %d%% (restated by its wake poll)", v);
        } else if (d > 0 && d <= MCU_VOL_STEP) {
            LOG_INFO("Panel volume key up (MCU counter %d)", v);
            adjust_user_volume(+1);
            ubus_notify_button("vol_up");
        } else if (d < 0 && -d <= MCU_VOL_STEP) {
            LOG_INFO("Panel volume key down (MCU counter %d)", v);
            adjust_user_volume(-1);
            ubus_notify_button("vol_down");
        } else if (seen && d == 0 && (v == 0 || v == 100)) {
            LOG_INFO("Panel volume key %s (MCU counter pinned at %d)", v ? "up" : "down", v);
            adjust_user_volume(v ? +1 : -1);
            ubus_notify_button(v ? "vol_up" : "vol_down");
        } else {
            LOG_DEBUG("MCU volume counter now %d%% (jump of %d, not a key)", v, d);
        }
        return;
    }

    /* MCU+PLY+PUS is the play/pause key on this fw, so it needs the debounce too -
     * a bounce on a toggle cancels itself out and the press looks like it was lost */
    if (strstr(cmd, "MCU+KEY+") || strstr(cmd, "MCU+PLY+PUS")) {
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
    } else if (strstr(cmd, "MCU+PLY+PUS") || strstr(cmd, "MCU+KEY+PLPA")) {
        /* MCU+PLY+PUS, with a plus, is what the panel key really sends here: two
         * presses in a row both came back PUS, so the MCU keeps no play state and
         * this is a bare toggle, not the pause half of a pair. Stock's own pause
         * event is MCU+PLY-PUS with a minus, handled further down and never seen. */
        mqtt_send_button("play_pause");
        ubus_notify_button("play_pause");
        /* O_CREAT on /tmp/player_cmd just made a plain file nothing ever read,
         * so this key did nothing at all until now. MA is the exception: its
         * stream comes from the server, so the key has to go there. */
        if (ma_has_transport() && ma_send("button play_pause") == 0)
            ma_send("status - 1 tags:alKd");
        else if (ma_start() == 0)
            ma_send("status - 1 tags:alKd");
        else
            spawn_async("/usr/bin/player_control.sh toggle");
    } else if (!strncmp(cmd, "MCU+KEY+", 8) && isdigit((unsigned char)cmd[8])) {
        /* Short press on playlist key 1-4, and the only MCU+KEY+ the panel really
         * sends: the VOL+/VOL-/PLPA/SRC forms above and below come from stock's own
         * tables and never appear on fw 0109. Stock acks it from the app side with
         * AXX+KEY+RDY, which is what GNOTIFY=PRESET_KEY:OK turns into at 0x412060. */
        int preset = atoi(cmd + 8);
        if (preset >= 1 && preset <= 4) {
            fire_preset(preset, 1);
            uart_send("AXX+KEY+RDY\n");
        } else {
            uart_send("AXX+KEY+NIL\n");
        }
    } else if (!strncmp(cmd, "MCU+PRE+", 8) && isdigit((unsigned char)cmd[8])) {
        /* The same key held until it blinks. Stock stored whatever was playing into
         * that slot; nothing here can, so it goes out as its own button and whatever
         * HA has bound to it decides. The MCU still wants OK%01d or FF%01d back
         * (stock's GNOTIFY=PRESET_SET_FAIL path) to stop blinking. */
        int preset = atoi(cmd + 8);
        char b[24];
        if (preset >= 1 && preset <= 4) {
            snprintf(b, sizeof(b), "preset_%d_hold", preset);
            mqtt_send_button(b);
            ubus_notify_button(b);
            LOG_INFO("Playlist key %d held", preset);
            snprintf(b, sizeof(b), "AXX+PRE+OK%d\n", preset);
        } else {
            snprintf(b, sizeof(b), "AXX+PRE+FF%d\n", preset % 10);
        }
        uart_send(b);
    } else if (strstr(cmd, "MCU+KEY+SRC") || strstr(cmd, "MCU+KEY+WIFI") ||
               strstr(cmd, "MCU+KEY+AUX") || strstr(cmd, "MCU+KEY+BT") ||
               strstr(cmd, "MCU+KEY+PAIR")) {
        /* nothing to drive: the MCU has already switched itself by the time it
         * tells us, and it announces the result as M2S+011/012/013 + PLM+nnn */
        const char *btn = (strstr(cmd, "MCU+KEY+BT") || strstr(cmd, "MCU+KEY+PAIR"))
                          ? "bluetooth" : "source";
        mqtt_send_button(btn);
        ubus_notify_button(btn);
    } else if (strstr(cmd, "MCU+M2S+")) {
        const char *p = strstr(cmd, "MCU+M2S+") + 8;
        LOG_INFO("MCU M2S Event received: [%.3s]", p);
        /* 011/012/013 is the MCU's current source: wifi / bluetooth / line-in.
         * Captured while cycling the front-panel key - each one is followed by the
         * matching MCU+PLM+000/006/005, and AXX+MCU+RDY answers with the same
         * pair. Not a multiroom role, whatever GNOTIFY=Master2Slave suggests.
         * 007/008 are the AudioPro wake handshake (GNOTIFY=audiopro_wake at
         * 0x426104 in stock mv_ioguard). */
        if (strncmp(p, "011", 3) == 0) {
            adopt_audio_source(0);
            uart_send(CMD_AUDIOPRO_WAKE_M);
            uart_send("AXX+M2S+011\n");
        } else if (strncmp(p, "012", 3) == 0) {
            adopt_audio_source(1);
        } else if (strncmp(p, "013", 3) == 0) {
            adopt_audio_source(2);
        } else if (strncmp(p, "008", 3) == 0) {
            /* wake request: keep the amp rail up, the MCU expects us alive */
            uart_send(CMD_POWER_ON);
        }
        /* 007/008 open the MCU's own poll burst, and it restates its volume twice
         * inside it - two phantom key presses before this gate existed. */
        if (strncmp(p, "007", 3) == 0 || strncmp(p, "008", 3) == 0)
            g_mcu_burst_ms = mono_ms();
    } else if (strstr(cmd, "MCU+S2M+")) {
        const char *p = strstr(cmd, "MCU+S2M+") + 8;
        LOG_INFO("MCU S2M Event received: [%.3s]", p);
        if (strncmp(p, "011", 3) == 0) {
            uart_send(CMD_AUDIOPRO_WAKE_S);
            uart_send("AXX+S2M+011\n");
        } else if (strncmp(p, "008", 3) == 0) {
            uart_send(CMD_POWER_ON);
        }
    /* Charge state comes in on MCU+BAT++ON / MCU+BAT+OFF. There is no MCU+CHA+
     * family - that string appears in no stock binary, so the handlers that used
     * to sit here could never fire and g_is_charging stayed 0 forever.
     * Stock: mv_ioguard maps these in its strncmp chain at 0x4098c8/0x4098e8 to
     * event 7 and event 8, and ParseCommonMCUEvent dispatches them to
     * Function_BatteryCharge (0x407e78) and Function_BatteryUncharge (0x407e98).
     * Both are a handful of instructions that store 1 or 0 into
     * g_wiimu_shm+0x764 and return. */
    } else if (strstr(cmd, "MCU+BAT++ON") || strstr(cmd, "MCU+BAT+ON") || strstr(cmd, "BAT+ON")) {
        LOG_INFO("Charger connected (%s)", strstr(cmd, "MCU+BAT++ON") ? "MCU+BAT++ON" : "MCU+BAT+ON");
        g_is_charging = 1;
        mqtt_send_sensor("charging", "true");
        ubus_notify_battery(g_battery_pct, g_is_charging);
        spawn_async("/usr/sbin/iw dev phy0-sta0 set power_save off");
    } else if (strstr(cmd, "MCU+BAT+OFF") || strstr(cmd, "BAT+OFF")) {
        /* Mains removed, not a dying cell. This arm used to call
         * graceful_shutdown(), so unplugging the speaker killed it mid-song at 98%
         * charge about 20 s later. Stock has no host-side undervoltage path at
         * all - the MCU drops the rail itself when the cell is empty, and the only
         * thing it tells us on the way down is MCU+BAT+LOW. */
        LOG_INFO("Charger disconnected (MCU+BAT+OFF) - running on battery");
        g_is_charging = 0;
        mqtt_send_sensor("charging", "false");
        ubus_notify_battery(g_battery_pct, g_is_charging);
        spawn_async("/usr/sbin/iw dev phy0-sta0 set power_save on");
    } else if (strstr(cmd, "MCU+BAT+LOW")) {
        LOG_WARN("Low battery alert (MCU+BAT+LOW)");
        play_sound(SOUND_LOW_BAT);
        mqtt_send_sensor("battery_alert", "low");
    } else if (strstr(cmd, "MCU+BAT+")) {
        const char *p = strstr(cmd, "MCU+BAT+") + 8;
        int bat = atoi(p);
        if (!g_is_charging && bat > g_battery_pct && (bat - g_battery_pct) <= 3) {
            bat = g_battery_pct;
        }
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
        int fd = open(BAT_STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
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
    } else if (strstr(cmd, "MCU+PLY-PLA")) {
        /* Stock event 260 (0x104): Function_Resume @ 0x416c94
         * Physical resume key from STM8S - resume paused playback. */
        LOG_INFO("MCU+PLY-PLA: physical resume key");
        ubus_notify_button("resume");
        spawn_async("/usr/bin/player_control.sh play");
    } else if (strstr(cmd, "MCU+PLY-PUS")) {
        /* Stock event 261 (0x105): Function_Pause @ 0x416c80
         * Physical pause key from STM8S - pause active playback. */
        LOG_INFO("MCU+PLY-PUS: physical pause key");
        ubus_notify_button("pause");
        spawn_async("/usr/bin/player_control.sh pause");
    } else if (strstr(cmd, "MCU+PLY+PRV")) {
        /* Stock event 257 (0x101): Function_Prev @ 0x405e4c
         * Physical previous track key from STM8S. */
        LOG_INFO("MCU+PLY+PRV: previous track key");
        ubus_notify_button("prev");
        spawn_async("/usr/bin/player_control.sh prev");
    } else if (strstr(cmd, "MCU+PLY+NXT")) {
        /* Stock event 258 (0x102): Function_Next @ 0x405ee8
         * Physical next track key from STM8S. */
        LOG_INFO("MCU+PLY+NXT: next track key");
        ubus_notify_button("next");
        spawn_async("/usr/bin/player_control.sh next");
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

enum {
    SET_PWR_KEEPALIVE,
    SET_PWR_AUTO_SLEEP,
    SET_PWR_SLEEP_ON_NO_NET,
    SET_PWR_NO_NET_TIMEOUT,
    SET_PWR_SLEEP_IN_AUX,
    SET_PWR_SLEEP_IN_BT,
    __SET_PWR_MAX
};

static const struct blobmsg_policy set_pwr_policy[__SET_PWR_MAX] = {
    [SET_PWR_KEEPALIVE]        = { .name = "keepalive_sec",          .type = BLOBMSG_TYPE_INT32 },
    [SET_PWR_AUTO_SLEEP]       = { .name = "auto_sleep_min",         .type = BLOBMSG_TYPE_INT32 },
    [SET_PWR_SLEEP_ON_NO_NET]  = { .name = "sleep_on_no_network",    .type = BLOBMSG_TYPE_INT32 },
    [SET_PWR_NO_NET_TIMEOUT]   = { .name = "no_network_timeout_min", .type = BLOBMSG_TYPE_INT32 },
    [SET_PWR_SLEEP_IN_AUX]     = { .name = "sleep_in_aux",           .type = BLOBMSG_TYPE_INT32 },
    [SET_PWR_SLEEP_IN_BT]      = { .name = "sleep_in_bt",            .type = BLOBMSG_TYPE_INT32 },
};

enum {
    TRIG_PRESET_ID,
    __TRIG_PRESET_MAX
};

static const struct blobmsg_policy trig_preset_policy[__TRIG_PRESET_MAX] = {
    [TRIG_PRESET_ID] = { .name = "preset", .type = BLOBMSG_TYPE_INT32 },
};

enum {
    TRANSPORT_CMD,
    __TRANSPORT_MAX
};

static const struct blobmsg_policy transport_policy[__TRANSPORT_MAX] = {
    [TRANSPORT_CMD] = { .name = "cmd", .type = BLOBMSG_TYPE_STRING },
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
    /* same number the keys and HA see, so scripts don't get a second opinion */
    blobmsg_add_u32(&b, "volume", ma_active() ? g_ma_vol : g_user_vol);
    blobmsg_add_string(&b, "mcu_version", g_mcu_ver);
    blobmsg_add_string(&b, "source", get_source_name(g_current_source));
    blobmsg_add_u32(&b, "battery", g_battery_pct);
    blobmsg_add_u8(&b, "charging", g_is_charging ? 1 : 0);
    blobmsg_add_u8(&b, "mute", g_is_muted ? 1 : 0);
    blobmsg_add_u32(&b, "keepalive_sec", g_keepalive_sec);
    blobmsg_add_u32(&b, "auto_sleep_min", g_auto_sleep_min);
    blobmsg_add_u8(&b, "sleep_on_no_network", g_sleep_on_no_net ? 1 : 0);
    blobmsg_add_u32(&b, "no_network_timeout_min", g_no_net_timeout_min);
    blobmsg_add_u8(&b, "sleep_in_aux", g_sleep_in_aux ? 1 : 0);
    blobmsg_add_u8(&b, "sleep_in_bt", g_sleep_in_bt ? 1 : 0);
    blobmsg_add_string(&b, "power_mode", (g_keepalive_sec > 0 && g_auto_sleep_min == 0) ? "always_on" :
                                         (g_keepalive_sec == 0) ? "stock_19min_cutoff" : "custom_auto_sleep");

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
    blobmsg_add_u32(&b, "volume", ma_active() ? g_ma_vol : g_user_vol);
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

static int mcud_ubus_set_source(struct ubus_context *ctx, struct ubus_object *obj,
                                struct ubus_request_data *req, const char *method,
                                struct blob_attr *msg) {
    (void)ctx;
    (void)obj;
    (void)method;
    (void)req;
    struct blob_attr *tb[__SET_SRC_MAX];
    blobmsg_parse(set_src_policy, __SET_SRC_MAX, tb, blob_data(msg), blob_len(msg));

    if (!tb[SET_SRC_SOURCE]) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    /* Kept so callers get a straight answer instead of "no such method": the MCU
     * owns the input on this cabinet and drops every AXX+PLM we send. Front panel
     * key only; we mirror the result. */
    LOG_WARN("UBUS set_source [%s] refused - only the front panel switches the input",
             blobmsg_get_string(tb[SET_SRC_SOURCE]));
    return UBUS_STATUS_NOT_SUPPORTED;
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

    /* Which leg is actually carrying traffic, for HA. This unit's cable flaps, so
     * knowing eth vs wifi from the outside is worth one retained string. */
    mqtt_send_sensor("uplink", uplink_name());

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

static int mcud_ubus_set_power_policy(struct ubus_context *ctx, struct ubus_object *obj,
                                      struct ubus_request_data *req, const char *method,
                                      struct blob_attr *msg) {
    (void)obj;
    (void)method;
    struct blob_attr *tb[__SET_PWR_MAX];
    blobmsg_parse(set_pwr_policy, __SET_PWR_MAX, tb, blob_data(msg), blob_len(msg));

    if (tb[SET_PWR_KEEPALIVE]) {
        int k = blobmsg_get_u32(tb[SET_PWR_KEEPALIVE]);
        g_keepalive_sec = (k < 0) ? 0 : k;
        LOG_INFO("UBUS RPC: MCU keepalive updated to %d s (%s)", g_keepalive_sec,
                 g_keepalive_sec > 0 ? "on, though the 19-min battery cutoff ignores it" :
                                       "off");
    }

    if (tb[SET_PWR_AUTO_SLEEP]) {
        int s = blobmsg_get_u32(tb[SET_PWR_AUTO_SLEEP]);
        g_auto_sleep_min = (s < 0) ? 0 : s;
        LOG_INFO("UBUS RPC: Inactivity auto-sleep updated to %d min (%s)", g_auto_sleep_min,
                 g_auto_sleep_min == 0 ? "Always-On" : "Auto power-off on silence enabled");
    }

    if (tb[SET_PWR_SLEEP_ON_NO_NET]) {
        g_sleep_on_no_net = blobmsg_get_u32(tb[SET_PWR_SLEEP_ON_NO_NET]) ? 1 : 0;
        LOG_INFO("UBUS RPC: sleep_on_no_network updated to %d", g_sleep_on_no_net);
    }

    if (tb[SET_PWR_NO_NET_TIMEOUT]) {
        int t = blobmsg_get_u32(tb[SET_PWR_NO_NET_TIMEOUT]);
        g_no_net_timeout_min = (t < 1) ? 1 : t;
        LOG_INFO("UBUS RPC: no_network_timeout_min updated to %d min", g_no_net_timeout_min);
    }

    if (tb[SET_PWR_SLEEP_IN_AUX]) {
        g_sleep_in_aux = blobmsg_get_u32(tb[SET_PWR_SLEEP_IN_AUX]) ? 1 : 0;
        LOG_INFO("UBUS RPC: sleep_in_aux updated to %d", g_sleep_in_aux);
    }

    if (tb[SET_PWR_SLEEP_IN_BT]) {
        g_sleep_in_bt = blobmsg_get_u32(tb[SET_PWR_SLEEP_IN_BT]) ? 1 : 0;
        LOG_INFO("UBUS RPC: sleep_in_bt updated to %d", g_sleep_in_bt);
    }

    struct blob_buf b;
    memset(&b, 0, sizeof(b));
    blob_buf_init(&b, 0);
    blobmsg_add_u32(&b, "keepalive_sec", g_keepalive_sec);
    blobmsg_add_u32(&b, "auto_sleep_min", g_auto_sleep_min);
    blobmsg_add_u8(&b, "sleep_on_no_network", g_sleep_on_no_net ? 1 : 0);
    blobmsg_add_u32(&b, "no_network_timeout_min", g_no_net_timeout_min);
    blobmsg_add_u8(&b, "sleep_in_aux", g_sleep_in_aux ? 1 : 0);
    blobmsg_add_u8(&b, "sleep_in_bt", g_sleep_in_bt ? 1 : 0);
    blobmsg_add_string(&b, "power_mode", (g_keepalive_sec > 0 && g_auto_sleep_min == 0) ? "always_on" :
                                         (g_keepalive_sec == 0) ? "stock_19min_cutoff" : "custom_auto_sleep");
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
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

/* the web used to spawn the preset handler itself, which meant a preset pressed in the
 * browser fired no mqtt trigger and no beep - only the front-panel key did */
static int mcud_ubus_trigger_preset(struct ubus_context *ctx, struct ubus_object *obj,
                                    struct ubus_request_data *req, const char *method,
                                    struct blob_attr *msg) {
    (void)obj;
    (void)method;
    struct blob_attr *tb[__TRIG_PRESET_MAX];
    blobmsg_parse(trig_preset_policy, __TRIG_PRESET_MAX, tb, blob_data(msg), blob_len(msg));

    if (!tb[TRIG_PRESET_ID]) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }

    int preset = blobmsg_get_u32(tb[TRIG_PRESET_ID]);
    if (preset < 1 || preset > 4) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    fire_preset(preset, 1);

    struct blob_buf b;
    memset(&b, 0, sizeof(b));
    blob_buf_init(&b, 0);
    blobmsg_add_u32(&b, "preset", preset);
    ubus_send_reply(ctx, req, b.head);
    blob_buf_free(&b);
    return 0;
}

/* Transport for whoever is really playing. The physical key already knew that MA
 * has to be driven over the CLI and everything else went to player_control.sh,
 * which can only mute a fader for a stream it does not own - so the web ui's
 * play/pause did nothing at all while MA was the source. NOT_FOUND means "not
 * mine", i.e. the caller should fall back to the local players. */
static int mcud_ubus_transport(struct ubus_context *ctx, struct ubus_object *obj,
                               struct ubus_request_data *req, const char *method,
                               struct blob_attr *msg) {
    (void)ctx;
    (void)obj;
    (void)req;
    (void)method;
    struct blob_attr *tb[__TRANSPORT_MAX];
    const char *cmd;
    blobmsg_parse(transport_policy, __TRANSPORT_MAX, tb, blob_data(msg), blob_len(msg));
    if (!tb[TRANSPORT_CMD]) return UBUS_STATUS_INVALID_ARGUMENT;
    cmd = blobmsg_get_string(tb[TRANSPORT_CMD]);
    /* "play" and the toggle may also wake a stopped or powered-off player - that is
     * what makes the panel key work on a speaker nobody has touched since boot -
     * and ma_start() is what decides whether that is safe. Everything else needs MA
     * to be the thing making sound. */
    if (!strcmp(cmd, "play") || !strcmp(cmd, "toggle")) {
        if (!ma_has_transport()) {
            if (ma_start() != 0) return UBUS_STATUS_NOT_FOUND;
            ma_send("status - 1 tags:alKd");
            return 0;
        }
    } else if (!ma_has_transport()) {
        return UBUS_STATUS_NOT_FOUND;
    }

    /* bare "pause" is the toggle, "pause 1" only ever pauses. "play", "mode play"
     * and "playlist jump" are all accepted and all do nothing; the only verb that
     * really starts a stopped queue is the front-panel emulation, and it restarts
     * the current track rather than resuming it. From pause the toggle resumes in
     * place, so try that first. */
    if (!strcmp(cmd, "toggle")) ma_send("button play_pause");
    /* only paused needs anything: jump_rew here used to be the stopped case and it
     * restarted the track under a player that was already playing. */
    else if (!strcmp(cmd, "play")) { if (g_ma_mode == 2) ma_send("button play_pause"); }
    else if (!strcmp(cmd, "pause")) ma_send("button play_pause");
    else if (!strcmp(cmd, "stop")) ma_send("stop");
    else if (!strcmp(cmd, "next")) ma_send("playlist index +1");
    else if (!strcmp(cmd, "prev")) ma_send("playlist index -1");
    else return UBUS_STATUS_INVALID_ARGUMENT;

    LOG_INFO("Transport %s sent to Music Assistant", cmd);
    ma_send("status - 1 tags:alKd");
    return 0;
}

static const struct ubus_method mcud_methods[] = {
    UBUS_METHOD_NOARG("status", mcud_ubus_status),
    UBUS_METHOD("set_volume", mcud_ubus_set_volume, set_vol_policy),
    UBUS_METHOD("set_source", mcud_ubus_set_source, set_src_policy),
    UBUS_METHOD("set_mute", mcud_ubus_set_mute, set_mute_policy),
    UBUS_METHOD("set_network_state", mcud_ubus_set_network_state, set_net_policy),
    UBUS_METHOD("set_power_policy", mcud_ubus_set_power_policy, set_pwr_policy),
    UBUS_METHOD("trigger_preset", mcud_ubus_trigger_preset, trig_preset_policy),
    UBUS_METHOD("transport", mcud_ubus_transport, transport_policy),
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
            } else {
                /* MCU frames are newline/& terminated; wait for the rest. */
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
                g_last_activity_time = now_mono();

                if (strncmp(fifo_line, "AXX+VOL+", 8) == 0 && isdigit((unsigned char)fifo_line[8])) {
                    set_user_volume(atoi(fifo_line + 8));
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
    time_t now = now_mono();
    static time_t last_keepalive = 0;

    /* The MCU cuts the rail 19 min after the charger is unplugged and nothing the
     * host sends changes that. Measured end to end: MCU+BAT+OFF at 19:27:07, this
     * keepalive firing every 60 s the whole time, dead at 19:46:25 with the pack
     * at 100 %. The faked "track is playing" burst (PLY+PLA + an advancing
     * POS/DUR pair, which is what stock streams while playing) did not fool it
     * either, so it is gone. Real playback does look like it holds the rail up,
     * on stock as well as here, but that is an observation and not a measurement.
     * On AC the charger holds the rails and none of this applies, hence
     * !g_is_charging. Only with an uplink, too: with no route out nobody can
     * reach us anyway, so let the MCU have its power-off. */
    if (g_keepalive_sec > 0 && !g_is_charging && now - last_keepalive >= g_keepalive_sec) {
        char kaif[32];
        last_keepalive = now;
        if (g_auto_sleep_min == 0 || default_route_iface(kaif, sizeof(kaif)) ||
            g_current_source != 0 || is_audio_active()) {
            uart_send(CMD_POWER_ON);
        }
    }

    /* Refresh activity timestamp if audio stream is active or external inputs in use */
    if (is_audio_active()) g_last_activity_time = now;
    /* dmix keeps the substream RUNNING forever, so is_audio_active() cannot tell
     * paused from playing. MA knows, and it is the source 95% of the time. */
    {
        int playing = ma_active() ? (g_ma_mode == 1) : is_audio_active();
        if (playing != g_is_playing) {
            g_is_playing = playing;
            uart_send(playing ? CMD_PLAY_START : CMD_PLAY_STOP);
            mqtt_send_sensor("playing", playing ? "true" : "false");
        }
    }

    check_stream_stall(now);
    check_duck_controls();

    /* Pending rate-limited volume MQTT flush */
    if (g_pending_vol_pub >= 0 && (now - g_last_vol_pub_sec) >= 1) {
        mqtt_publish_volume(g_pending_vol_pub);
    }

    /* MQTT sleep timer. Counts down in HA so the number entity shows what is
     * left, and fires whatever is playing - see sleep_timer_arm(). */
    if (g_sleep_deadline) {
        int left = (int)((g_sleep_deadline - now + 59) / 60);
        if (left <= 0) {
            LOG_INFO("Sleep timer expired, powering off");
            g_sleep_deadline = 0;
            mqtt_publish_sleep(0);
            uloop_end();
            graceful_shutdown();
            return;
        }
        if (left != g_sleep_left_pub) mqtt_publish_sleep(left);
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

    /* No-network sleep check on battery in Wi-Fi mode */
    char net_if[32];
    int has_net = default_route_iface(net_if, sizeof(net_if));
    if (has_net) {
        g_no_net_lost_ts = 0;
    } else {
        if (g_no_net_lost_ts == 0) {
            g_no_net_lost_ts = now;
        }
        if (!g_is_charging && g_sleep_on_no_net && g_current_source == 0) {
            if ((now - g_no_net_lost_ts) >= (time_t)(g_no_net_timeout_min * 60)) {
                if (is_audio_active()) {
                    g_no_net_lost_ts = now;
                } else {
                    LOG_INFO("No-Network-Sleep: No network uplink for %d min on battery in Wi-Fi mode. Powering off...", g_no_net_timeout_min);
                    uloop_end();
                    graceful_shutdown();
                    return;
                }
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
    while ((opt = getopt(argc, argv, "h:p:u:P:t:v:s:l:k:A:Q:G:abmnoN:")) != -1) {
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
            case 'n':
                g_sleep_on_no_net = 1;
                break;
            case 'N':
                g_no_net_timeout_min = atoi(optarg);
                if (g_no_net_timeout_min < 1) g_no_net_timeout_min = 1;
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
            case 'k':
                g_keepalive_sec = atoi(optarg);
                if (g_keepalive_sec < 0) g_keepalive_sec = 0;
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
            case 'A':
                strncpy(g_ma_host, optarg, sizeof(g_ma_host) - 1);
                g_ma_host[sizeof(g_ma_host) - 1] = '\0';
                break;
            case 'Q':
                g_ma_port = atoi(optarg);
                if (g_ma_port <= 0) g_ma_port = MA_PORT_DEFAULT;
                break;
            case 'G':
                g_ma_step = atoi(optarg);
                if (g_ma_step < 1) g_ma_step = 1;
                if (g_ma_step > 25) g_ma_step = 25;
                break;
            case 'o':
                g_ma_owns_volume = 1;
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
    bat_load();

    uloop_init();

    g_uart_fd = uart_init();
    if (g_uart_fd < 0) return EXIT_FAILURE;

    g_fifo_fd = fifo_init();
    if (g_fifo_fd < 0) return EXIT_FAILURE;

    alsa_init();
    /* Establish the persisted safe level before any MCU unmute/play command.
     * The softvol may come up at 0 dB; never expose that transient to the amp. */
    alsa_set_master(g_user_vol);
    mqtt_setup();
    ubus_init_service();

    /* Verified Audio Pro C3 Linkplay Handshake Sequence. RDY doubles as the only
     * state readback fw 0109 answers: it replies with the current source
     * (MCU+PLM+nnn / MCU+M2S+011..013) and MCU+VOL+nnn, so a restart while the box
     * sits on line-in no longer leaves our mirror claiming wifi. */
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
    usleep(100000);
    uart_send(CMD_UNMUTE);
    uart_send(CMD_PLAY_START);

    /* Master was already set before UNMUTE/PLAY_START above; this call only
     * synchronizes the MCU/MQTT state and leaves the ALSA level unchanged. */
    set_user_volume(g_user_vol);
    /* The MCU only volunteers its version when asked, and it answers this one
     * (AXX+VER+GET gets nothing). Worth having in the log next to a bug report. */
    uart_send("AXX+MCU+VER\n");
    /* fw 0109 does not answer this one - kept because it costs one line and other
     * MCU builds might, and charging=false until the next plug event is what we
     * get otherwise. */
    uart_send("AXX+BAT+GET\n");
    g_mcu_link_ts = now_mono();
    LOG_INFO("Linkplay MCU initialized: I2S playback, amplifier unmuted, AudioPro wake synced.");
    if (g_auto_sleep_min > 0) {
        LOG_INFO("Inactivity Auto-Sleep timer enabled: %d minutes", g_auto_sleep_min);
    } else {
        LOG_INFO("Inactivity Auto-Sleep disabled: Always-On mode active (24/7 ready)");
    }
    if (g_keepalive_sec > 0) {
        LOG_INFO("MCU keepalive every %d s while an uplink is up", g_keepalive_sec);
    } else {
        LOG_INFO("MCU keepalive disabled: the MCU will cut power after ~20 min on battery");
    }
    play_sound(SOUND_BOOT);

    g_last_activity_time = now_mono();

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

    g_ma_retry_t.cb = ma_retry_cb;
    ma_connect();

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
    if (g_wdt_fd >= 0) {
        write(g_wdt_fd, "V", 1); /* Magic Close to safely disarm kernel watchdog */
        close(g_wdt_fd);
        g_wdt_fd = -1;
    }
    ma_disconnect(0);
    for (int i = 0; i < g_mixer_ufd_count; i++) uloop_fd_delete(&g_mixer_ufds[i]);
    free(g_mixer_ufds);
    if (g_mixer) snd_mixer_close(g_mixer);
    if (g_mosq) {
        mqtt_unarm();
        mosquitto_disconnect(g_mosq);
        mosquitto_loop_write(g_mosq, 1);
        mosquitto_destroy(g_mosq);
        mosquitto_lib_cleanup();
    }

    return EXIT_SUCCESS;
}
