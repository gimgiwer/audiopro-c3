#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <alloca.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <alsa/asoundlib.h>
#include <mosquitto.h>

#define SERIAL_PORT          "/dev/ttyS0"
#define CMD_FIFO_PATH        "/tmp/mcu_cmd_fifo"
#define BAUD_RATE            B57600
#define HEARTBEAT_SEC        15
#define VOL_STEP_PERCENT     5
#define MAX_LINE_LEN         128
#define READ_BUF_SIZE        256
#define BUTTON_DEBOUNCE_MS   120

#define SOUND_BOOT           "/usr/share/sounds/boot.wav"
#define SOUND_PRESET         "/usr/share/sounds/preset_saved.wav"
#define SOUND_BT_CONN        "/usr/share/sounds/bt_connected.wav"
#define SOUND_WIFI_CONN      "/usr/share/sounds/wifi_connected.wav"
#define SOUND_LOW_BAT        "/usr/share/sounds/low_battery.wav"

#define CMD_MCU_READY        "AXX+MCU+RDY\n"
#define CMD_BOOT_DONE        "AXX+BOT+DON\n"
#define CMD_PLAY_MODE        "AXX+PLM+001\n"
#define CMD_UNMUTE           "AXX+MUT+000\n"
#define CMD_MUTE             "AXX+MUT+001\n"
#define CMD_HEARTBEAT        "AXX+MCU+RDY\n"

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
static struct mosquitto *g_mosq = NULL;

static log_level_t g_log_level = LOG_LEVEL_INFO;
static char g_mqtt_host[128] = "127.0.0.1";
static int  g_mqtt_port = 1883;
static char g_mqtt_user[64] = "";
static char g_mqtt_pass[64] = "";
static char g_topic_prefix[64] = "audiopro_c3";
static int  g_mqtt_enabled = 1;
static int  g_current_vol = 25;       /* Safe initial room volume (25%) */
static int  g_current_source = 0;    /* 0: wifi/i2s, 1: bluetooth, 2: aux */
static int  g_auto_sleep_min = 0;    /* 0 = Always-On (never sleep), >0 = timeout in minutes */
static int  g_sleep_in_aux = 0;      /* 0 = keep awake in AUX (default), 1 = timer sleep */
static int  g_sleep_in_bt = 0;       /* 0 = keep awake in BT (default), 1 = timer sleep */
static time_t g_last_activity_time = 0;
static int64_t  g_last_button_ms = 0;

static time_t g_last_vol_pub_sec = 0;
static int    g_pending_vol_pub = -1;
static int    g_last_bat = 100;

#define LOG_ERROR(fmt, ...) do { if (g_log_level >= LOG_LEVEL_ERROR) fprintf(stderr, "[mcud ERROR] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_WARN(fmt, ...)  do { if (g_log_level >= LOG_LEVEL_WARN)  fprintf(stderr, "[mcud WARN] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)  do { if (g_log_level >= LOG_LEVEL_INFO)  fprintf(stdout, "[mcud INFO] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...) do { if (g_log_level >= LOG_LEVEL_DEBUG) fprintf(stdout, "[mcud DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)

static int uart_init(void);

static inline void safe_write(int fd, const void *buf, size_t count) {
    if (fd >= 0 && buf && count > 0) {
        ssize_t ret = write(fd, buf, count);
        (void)ret;
    }
}

static void atomic_write_file(const char *path, const char *content) {
    if (!path || !content) return;
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        safe_write(fd, content, strlen(content));
        close(fd);
        rename(tmp_path, path);
    }
}

static int uart_send(const char *cmd) {
    if (g_uart_fd < 0) {
        g_uart_fd = uart_init();
        if (g_uart_fd < 0) return -1;
    }
    size_t len = strlen(cmd);
    ssize_t written = write(g_uart_fd, cmd, len);
    if (written < 0) {
        if (errno == EPIPE || errno == EBADF) {
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

static void spawn_async_cmd(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG_WARN("Failed to fork for %s: %s", path, strerror(errno));
        return;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execv(path, argv);
        _exit(127);
    }
}

static void play_sound(const char *path) {
    if (!path || access(path, R_OK) != 0) return;
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
        execlp("aplay", "aplay", "-q", "-D", "music_in", path, (char *)NULL);
        _exit(1);
    }
}

static void watchdog_ping(void) {
    if (g_wdt_fd < 0) {
        g_wdt_fd = open("/dev/watchdog", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (g_wdt_fd >= 0) {
        safe_write(g_wdt_fd, "1", 1);
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
        int fd = open(status_paths[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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

static void graceful_shutdown(void) {
    LOG_INFO("Initiating graceful shutdown of audio services and system...");

    /* Stop streaming processes first */
    char *k_argv[] = {"/usr/bin/killall", "-TERM", "librespot", "shairport-sync", "squeezelite", NULL};
    spawn_async_cmd("/usr/bin/killall", k_argv);
    usleep(300000);

    /* Mute amplifier */
    uart_send(CMD_MUTE);
    usleep(100000);

    /* Publish offline state to Home Assistant */
    if (g_mosq && g_mqtt_enabled) {
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/status", g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, 7, "offline", 0, true);
        mosquitto_loop_stop(g_mosq, true);
    }

    sync();
    char *p_argv[] = {"/sbin/poweroff", NULL};
    spawn_async_cmd("/sbin/poweroff", p_argv);
}

/* Ensure ALSA software volume controls are pegged to 100% (0 dB bit-perfect pass-through) */
static void alsa_reset_softvol_to_max(void) {
    if (g_mixer) return;
    int err = snd_mixer_open(&g_mixer, 0);
    if (err < 0) return;
    if (snd_mixer_attach(g_mixer, "default") < 0 ||
        snd_mixer_selem_register(g_mixer, NULL, NULL) < 0 ||
        snd_mixer_load(g_mixer) < 0) {
        snd_mixer_close(g_mixer);
        g_mixer = NULL;
        return;
    }

    const char *names[] = {"Spotify", "AirPlay", "Music", "Notification", "Squeeze", "TTS", "VoIP", "Alarm", "Timer", NULL};
    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);

    for (int i = 0; names[i]; i++) {
        snd_mixer_selem_id_set_name(sid, names[i]);
        snd_mixer_elem_t *elem = snd_mixer_find_selem(g_mixer, sid);
        if (elem) {
            long min = 0, max = 100;
            snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
            snd_mixer_selem_set_playback_volume_all(elem, max);
        }
    }
    LOG_INFO("ALSA softvol controls initialized to 100%% (0 dB bit-perfect pass-through)");
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

static void set_hardware_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    g_current_vol = vol;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AXX+VOL+%03d\n", g_current_vol);
    uart_send(cmd);
    mqtt_publish_volume(g_current_vol);

    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d\n", g_current_vol);
    atomic_write_file("/tmp/current_volume", vol_str);

    LOG_INFO("Master Hardware Volume set to %d%%", g_current_vol);
}

static void adjust_hardware_volume(int delta) {
    set_hardware_volume(g_current_vol + delta);
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
            uart_send(CMD_PLAY_MODE);
            uart_send(CMD_UNMUTE);
        } else if (strcasecmp(payload, "PAUSE") == 0 || strcasecmp(payload, "STOP") == 0 || strcasecmp(payload, "MUTE") == 0) {
            uart_send(CMD_MUTE);
        } else if (strcasecmp(payload, "TOGGLE") == 0) {
            int fd = open("/tmp/player_cmd", O_WRONLY | O_NONBLOCK);
            if (fd >= 0) { safe_write(fd, "toggle\n", 7); close(fd); }
        }
    } else if (strcmp(msg->topic, vol_topic) == 0) {
        g_last_activity_time = time(NULL);
        int vol = atoi(payload);
        set_hardware_volume(vol);
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
            "\"unique_id\":\"audiopro_c3_battery\","
            "\"device\":{\"identifiers\":[\"audiopro_c3\"],"
            "\"name\":\"Audio Pro C3\"}}",
            g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    {
        char topic[256], payload[512];
        snprintf(topic, sizeof(topic), "homeassistant/sensor/audiopro_c3/source/config");
        snprintf(payload, sizeof(payload),
            "{\"name\":\"Audio Pro C3 Source\","
            "\"icon\":\"mdi:volume-source\","
            "\"state_topic\":\"%s/sensor/source\","
            "\"unique_id\":\"audiopro_c3_source\","
            "\"device\":{\"identifiers\":[\"audiopro_c3\"],"
            "\"name\":\"Audio Pro C3\"}}",
            g_topic_prefix);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    char sub_cmd[128], sub_vol[128];
    snprintf(sub_cmd, sizeof(sub_cmd), "%s/player/command", g_topic_prefix);
    snprintf(sub_vol, sizeof(sub_vol), "%s/player/volume/set", g_topic_prefix);
    mosquitto_subscribe(g_mosq, NULL, sub_cmd, 0);
    mosquitto_subscribe(g_mosq, NULL, sub_vol, 0);

    /* Publish online status */
    char status_topic[128];
    snprintf(status_topic, sizeof(status_topic), "%s/status", g_topic_prefix);
    mosquitto_publish(g_mosq, NULL, status_topic, 6, "online", 0, true);

    LOG_INFO("MQTT Home Assistant Discovery published and subscriptions created.");
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
    mosquitto_lib_init();
    g_mosq = mosquitto_new("audiopro_c3_mcud", true, NULL);
    if (!g_mosq) return -1;
    if (strlen(g_mqtt_user) > 0) {
        mosquitto_username_pw_set(g_mosq, g_mqtt_user, strlen(g_mqtt_pass) > 0 ? g_mqtt_pass : NULL);
    }
    mosquitto_message_callback_set(g_mosq, mqtt_on_message);
    mosquitto_disconnect_callback_set(g_mosq, mqtt_on_disconnect);
    mosquitto_connect_async(g_mosq, g_mqtt_host, g_mqtt_port, 60);
    mosquitto_loop_start(g_mosq);
    mqtt_send_discovery();
    return 0;
}

static int fifo_init(void) {
    unlink(CMD_FIFO_PATH);
    if (mkfifo(CMD_FIFO_PATH, 0660) < 0 && errno != EEXIST) {
        LOG_ERROR("Failed to create command FIFO %s: %s", CMD_FIFO_PATH, strerror(errno));
        return -1;
    }
    g_fifo_fd = open(CMD_FIFO_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (g_fifo_fd < 0) {
        LOG_ERROR("Failed to open command FIFO %s: %s", CMD_FIFO_PATH, strerror(errno));
        return -1;
    }
    return g_fifo_fd;
}

static int uart_init(void) {
    int fd = open(SERIAL_PORT, O_RDWR | O_NOCTTY | O_CLOEXEC);
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
    if (tcsetattr(fd, TCSANOW, &tty) != 0) { close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void set_audio_source(int source) {
    g_current_source = source % 3;
    g_last_activity_time = time(NULL);
    const char *names[] = {"wifi", "bluetooth", "aux"};
    const char *src_name = names[g_current_source];

    mqtt_send_sensor("source", src_name);
    char src_buf[32];
    snprintf(src_buf, sizeof(src_buf), "%s\n", src_name);
    atomic_write_file("/tmp/audio_source", src_buf);

    if (g_current_source == 0) {
        uart_send("AXX+INP+000\n");
        uart_send(CMD_PLAY_MODE);
        uart_send(CMD_UNMUTE);
        play_sound(SOUND_WIFI_CONN);
    } else if (g_current_source == 1) {
        uart_send("AXX+INP+002\n");
        play_sound(SOUND_BT_CONN);
    } else {
        uart_send("AXX+INP+001\n");
    }
}

static void process_mcu_command(const char *cmd) {
    LOG_DEBUG("MCU RX: [%s]", cmd);

    /* Mechanical Debounce for hardware buttons */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if ((now_ms - g_last_button_ms) < BUTTON_DEBOUNCE_MS) {
        return;
    }
    g_last_button_ms = now_ms;
    g_last_activity_time = time(NULL);

    if (strstr(cmd, "MCU+KEY+VOL+")) {
        adjust_hardware_volume(VOL_STEP_PERCENT);
    } else if (strstr(cmd, "MCU+KEY+VOL-")) {
        adjust_hardware_volume(-VOL_STEP_PERCENT);
    } else if (strstr(cmd, "MCU+KEY+PLPA")) {
        mqtt_send_button("play_pause");
        /* Priority alert dismissal: Ringing Timer -> Active Alarm -> Normal Play/Pause toggle */
        if (access("/tmp/timer_ring.pid", F_OK) == 0) {
            char *t_argv[] = {"/usr/bin/smart_timer.sh", "stop", NULL};
            spawn_async_cmd("/usr/bin/smart_timer.sh", t_argv);
        } else if (access("/tmp/alarm.pid", F_OK) == 0) {
            char *a_argv[] = {"/usr/bin/smart_alarm.sh", "stop", NULL};
            spawn_async_cmd("/usr/bin/smart_alarm.sh", a_argv);
        } else {
            char *p_argv[] = {"/usr/bin/player_control.sh", "toggle", "all", NULL};
            spawn_async_cmd("/usr/bin/player_control.sh", p_argv);
            int fd = open("/tmp/player_cmd", O_WRONLY | O_NONBLOCK);
            if (fd >= 0) { safe_write(fd, "toggle\n", 7); close(fd); }
        }
    } else if (strstr(cmd, "MCU+KEY+PRE:")) {
        const char *p = strstr(cmd, "MCU+KEY+PRE:") + 12;
        int preset = atoi(p);
        if (preset >= 1 && preset <= 4) {
            char name[16], buf[32];
            snprintf(name, sizeof(name), "preset_%d", preset);
            mqtt_send_button(name);
            play_sound(SOUND_PRESET);
            snprintf(buf, sizeof(buf), "preset:%d\n", preset);
            int fd = open("/tmp/player_cmd", O_WRONLY | O_NONBLOCK);
            if (fd >= 0) { safe_write(fd, buf, strlen(buf)); close(fd); }
            char pstr[8];
            snprintf(pstr, sizeof(pstr), "%d", preset);
            char *pr_argv[] = {"/usr/bin/audiopro_preset_handler.sh", pstr, NULL};
            spawn_async_cmd("/usr/bin/audiopro_preset_handler.sh", pr_argv);
        }
    } else if (strstr(cmd, "MCU+KEY+SRC")) {
        mqtt_send_button("source");
        set_audio_source(g_current_source + 1);
    } else if (strstr(cmd, "MCU+KEY+BT")) {
        mqtt_send_button("bluetooth");
        set_audio_source(1);
    } else if (strstr(cmd, "MCU+KEY+WIFI")) {
        set_audio_source(0);
    } else if (strstr(cmd, "MCU+KEY+AUX")) {
        set_audio_source(2);
    } else if (strstr(cmd, "MCU+BAT+")) {
        const char *p = strstr(cmd, "MCU+BAT+") + 8;
        int bat = atoi(p);
        char val[16];
        snprintf(val, sizeof(val), "%d", bat);
        mqtt_send_sensor("battery", val);
        if (bat <= 15 && g_last_bat > 15) {
            LOG_WARN("Low battery threshold reached (%d%%)! Playing alert...", bat);
            play_sound(SOUND_LOW_BAT);
            mqtt_send_sensor("battery_alert", "low");
        } else if (bat > 20 && g_last_bat <= 15) {
            mqtt_send_sensor("battery_alert", "normal");
        }
        g_last_bat = bat;
        char bcmd[140];
        snprintf(bcmd, sizeof(bcmd), "%s\n", cmd);
        atomic_write_file("/tmp/battery_status", bcmd);
    } else if (strstr(cmd, "MCU+POW+OFF")) {
        LOG_INFO("MCU reported Power Off event. Initiating graceful shutdown...");
        g_running = 0;
        graceful_shutdown();
    }
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

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
                g_current_vol = atoi(optarg);
                if (g_current_vol < 0) g_current_vol = 0;
                if (g_current_vol > 100) g_current_vol = 100;
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
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) return EXIT_FAILURE;

    g_uart_fd = uart_init();
    if (g_uart_fd < 0) return EXIT_FAILURE;

    g_fifo_fd = fifo_init();
    if (g_fifo_fd < 0) return EXIT_FAILURE;

    alsa_reset_softvol_to_max();
    mqtt_setup();
    sleep(1);

    // 4-Step Linkplay Handshake
    uart_send(CMD_MCU_READY);
    usleep(150000);
    uart_send(CMD_BOOT_DONE);
    usleep(150000);
    uart_send(CMD_PLAY_MODE);
    usleep(150000);
    uart_send(CMD_UNMUTE);
    set_hardware_volume(g_current_vol);
    LOG_INFO("Linkplay MCU initialized: I2S Play Mode selected, Amplifier unmuted.");
    if (g_auto_sleep_min > 0) {
        LOG_INFO("Inactivity Auto-Sleep timer enabled: %d minutes", g_auto_sleep_min);
    } else {
        LOG_INFO("Inactivity Auto-Sleep disabled: Always-On mode active (24/7 ready)");
    }
    play_sound(SOUND_BOOT);

    struct pollfd fds[3] = {
        { .fd = g_uart_fd, .events = POLLIN },
        { .fd = sfd,       .events = POLLIN },
        { .fd = g_fifo_fd, .events = POLLIN }
    };

    char line_buf[MAX_LINE_LEN];
    size_t line_pos = 0;
    time_t last_hb = time(NULL);
    g_last_activity_time = time(NULL);

    char fifo_line[MAX_LINE_LEN];
    size_t fifo_pos = 0;

    while (g_running) {
        time_t now = time(NULL);
        int timeout_ms = (HEARTBEAT_SEC - (now - last_hb)) * 1000;
        if (timeout_ms <= 0) timeout_ms = 10;

        int ret = poll(fds, 3, timeout_ms);
        now = time(NULL);

        /* Refresh activity timestamp if audio is playing or in AUX/BT mode */
        if (is_audio_active()) {
            g_last_activity_time = now;
        }

        /* Check auto-sleep inactivity timeout with re-verification */
        if (g_auto_sleep_min > 0 && (now - g_last_activity_time) >= (time_t)(g_auto_sleep_min * 60)) {
            if (is_audio_active()) {
                g_last_activity_time = now;
                LOG_INFO("Auto-Sleep cancelled: audio stream recently resumed.");
            } else {
                LOG_INFO("Auto-Sleep: No audio activity for %d minutes. Initiating graceful shutdown...", g_auto_sleep_min);
                g_running = 0;
                graceful_shutdown();
                break;
            }
        }

        /* Pending rate-limited volume MQTT flush */
        if (g_pending_vol_pub >= 0 && (now - g_last_vol_pub_sec) >= 1) {
            mqtt_publish_volume(g_pending_vol_pub);
        }

        if ((now - last_hb) >= HEARTBEAT_SEC) {
            uart_send(CMD_HEARTBEAT);
            watchdog_ping();
            last_hb = now;
        }

        if (ret > 0 && (fds[1].revents & POLLIN)) {
            struct signalfd_siginfo si;
            if (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                if (si.ssi_signo == SIGCHLD) {
                    while (waitpid(-1, NULL, WNOHANG) > 0);
                } else {
                    g_running = 0;
                }
            }
        }

        if (ret > 0 && (fds[2].revents & POLLIN)) {
            char temp[READ_BUF_SIZE];
            ssize_t fn = read(g_fifo_fd, temp, sizeof(temp));
            if (fn > 0) {
                for (ssize_t i = 0; i < fn; i++) {
                    char c = temp[i];
                    if (c == '\n' || c == '\r') {
                        if (fifo_pos > 0) {
                            fifo_line[fifo_pos] = '\0';
                            LOG_INFO("FIFO command -> UART: %s", fifo_line);
                            g_last_activity_time = time(NULL);

                            /* Keep internal state and MQTT in sync */
                            if (strncmp(fifo_line, "AXX+VOL+", 8) == 0) {
                                int vol = atoi(fifo_line + 8);
                                if (vol >= 0 && vol <= 100) {
                                    g_current_vol = vol;
                                    mqtt_publish_volume(g_current_vol);
                                }
                            } else if (strncmp(fifo_line, "AXX+INP+", 8) == 0) {
                                int inp = atoi(fifo_line + 8);
                                if (inp == 0) g_current_source = 0;
                                else if (inp == 1) g_current_source = 2;
                                else if (inp == 2) g_current_source = 1;
                            }

                            uart_send(fifo_line);
                            uart_send("\n");
                            fifo_pos = 0;
                        }
                    } else if (fifo_pos < sizeof(fifo_line) - 1) {
                        fifo_line[fifo_pos++] = c;
                    } else {
                        LOG_WARN("FIFO line overflow, dropping line");
                        fifo_pos = 0;
                    }
                }
            }
        }

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            char rx[READ_BUF_SIZE];
            ssize_t n = read(g_uart_fd, rx, sizeof(rx));
            if (n < 0) {
                if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK) {
                    LOG_ERROR("UART read error (%s), reopening...", strerror(errno));
                    close(g_uart_fd);
                    g_uart_fd = -1;
                    fds[0].fd = -1;
                    sleep(1);
                    g_uart_fd = uart_init();
                    fds[0].fd = g_uart_fd;
                }
            } else if (n == 0) {
                LOG_WARN("UART EOF detected, reopening...");
                close(g_uart_fd);
                g_uart_fd = -1;
                fds[0].fd = -1;
                sleep(1);
                g_uart_fd = uart_init();
                fds[0].fd = g_uart_fd;
            } else {
                for (ssize_t i = 0; i < n; i++) {
                    char c = rx[i];
                    if (c == '&' || c == '\n') {
                        line_buf[line_pos] = '\0';
                        if (line_pos > 0) process_mcu_command(line_buf);
                        line_pos = 0;
                    } else if (c != '\r') {
                        if (line_pos < MAX_LINE_LEN - 1) {
                            line_buf[line_pos++] = c;
                        } else {
                            LOG_WARN("UART line overflow (>%d bytes), dropping noise", MAX_LINE_LEN);
                            line_pos = 0;
                        }
                    }
                }
            }
        }
    }

    uart_send(CMD_MUTE);
    usleep(100000);
    close(g_fifo_fd);
    unlink(CMD_FIFO_PATH);
    close(g_uart_fd);
    close(sfd);
    if (g_wdt_fd >= 0) close(g_wdt_fd);
    if (g_mixer) snd_mixer_close(g_mixer);
    if (g_mosq) {
        mosquitto_loop_stop(g_mosq, true);
        mosquitto_destroy(g_mosq);
        mosquitto_lib_cleanup();
    }
    return EXIT_SUCCESS;
}
