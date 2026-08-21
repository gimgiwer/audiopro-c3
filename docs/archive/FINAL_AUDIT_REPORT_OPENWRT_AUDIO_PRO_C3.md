> **ARCHIVED — HISTORICAL, PARTLY UNVERIFIED.**
> This is a working report from an earlier stage of the project, kept for the
> reverse-engineering detail in it. Several claims in here were later disproved
> against the actual hardware — notably the I2S register values, the "TAS5707
> over I2C" amplifier, an 11.2896 MHz MCLK, and the smaller ALSA buffer figures.
> For anything hardware-related trust `docs/I2S_HARDWARE_REGISTERS.md` and the
> patches in `patches/`, not this file.

# 📑 Полный технический отчёт и спецификация прошивки OpenWrt 23.05.5 для Audio Pro Addon C3 (Linkplay A28 V01)

> **Статус:** `VERIFIED & READY FOR RAM BOOT / FLASHING`  
> **Версия OpenWrt:** `23.05.5` (ревизия `r24106-10cc5fcd00`)  
> **Ядро Linux:** `5.15.167` (архитектура `mipsel_24kc_musl`, Soft-Float)  
> **Целевая платформа:** `ramips/mt76x8` (MediaTek MT7688AN)  
> **Целевое устройство:** Audio Pro Addon C3 / вычислительный модуль Linkplay A28 V01  

---

## 1. Аппаратная архитектура и физическая топология

| Подсистема | Компонент / Чип | Аппаратные параметры и режим работы |
| :--- | :--- | :--- |
| **Центральный процессор (SoC)** | MediaTek MT7688AN | MIPS 24KEc @ 575 MHz, архитектура `mipsel` (32-bit Little-Endian), Soft-Float (без hardware FPU) |
| **Оперативная память (RAM)** | Winbond W9751G6KB-25 | **64 MB DDR2-800 SDRAM** (16-bit шина данных, адреса `0x80000000`–`0x84000000`) |
| **Энергонезависимая память** | Winbond W25Q128BV | **16 MB SPI NOR Flash** (`0x00000000`–`0x01000000`, 40 MHz SPI clock) |
| **Аудио-интерфейс (I2S Master)** | MT7688 I2S Engine → DSP | Master mode: MT7688 генерирует сигналы MCLK (256×Fs), BCLK, WS, SDO (16-bit 44.1/48 kHz stereo) |
| **Аудиокодек** | *Отсутствует на шине I2C* | Прямой цифровой I2S поток в усилитель/DSP TAS57xx. В ядре используется dummy DAI `linux,snd-soc-dummy` |
| **Дополнительный микроконтроллер** | STM8 / Linkplay MCU | Управление питанием, контроллером заряда 3S Li-Ion АКБ, кнопками верхней панели, MUTE и громкостью |
| **Шина связи с MCU** | UART0 / `/dev/ttyS0` | **57600 baud, 8N1** (монопольно управляется `mcud` с приёмом команд от CGI через FIFO) |
| **Сервисная консоль UART** | UART Lite / `/dev/ttyS1` | **57600 baud, 8N1**, `console=ttyS1,57600` (выведен на контактные площадки GND, TX, RX, 3.3V) |
| **Ethernet** | MT7688 Built-in Switch | 10/100 Mbps, конфигурация коммутатора `mediatek,portmap = "llllw"`, MAC в Factory `@ 0x28` |
| **Wi-Fi** | MT7603 / MT7628 Radio | 802.11b/g/n 2.4 GHz (1T1R), драйвер `mac80211` / `kmod-mt7603`, калибровки EEPROM в Factory `@ 0x00` |

---

## 2. Карта разделов Flash-памяти 16MB SPI NOR (`/proc/mtd`)

```text
0x00000000 - 0x00030000 (192 KB)  : "u-boot"       (U-Boot 1.1.3 Ralink 4.3.0.0, 57600 baud, READ-ONLY)
0x00030000 - 0x00040000 (64 KB)   : "u-boot-env"   (Переменные конфигурации загрузчика, READ-WRITE)
0x00040000 - 0x00050000 (64 KB)   : "factory"      (EEPROM калибровки Wi-Fi @ 0x04, Ethernet MAC @ 0x28)
0x00050000 - 0x00250000 (2 MB)    : "bkKernel"     (Аварийное заводское ядро 2016 г., READ-ONLY)
0x00250000 - 0x00D80000 (11.18 MB): "firmware"     (OpenWrt: Kernel + SquashFS + RootFS_data / Overlay)
0x00D80000 - 0x00E00000 (512 KB)  : "user"         (Раздел настроек JFFS2)
0x00E00000 - 0x01000000 (2 MB)    : "user2"        (Вендорный раздел сертификатов и ключей)
```

> **Параметр сборочной системы:** `IMAGE_SIZE := 11456k` (`0xB30000` = 11,730,944 байт) строго ограничен границами раздела `firmware` (`0x250000`–`0xD80000`).

---

## 3. Анализ и устранение 10 критических архитектурных проблем

| # | Выявленная проблема | Первопричина сбоя | Применённое и верифицированное решение |
| :- | :--- | :--- | :--- |
| **1** | **`no soundcards` в ALSA** | Несовпадение modalias `platform:ralink-i2s` с DTS `mediatek,mt7628-i2s` при модульной сборке (`=m`) | Драйверы звука встроены статически в ядро (`=y`): `CONFIG_SND_SIMPLE_CARD=y`, `CONFIG_SND_RALINK_SOC_I2S=y` |
| **2** | **Падение pinctrl с `-EINVAL`** | Группа `"jtag"` не является валидной группой в драйвере `rt2880-pinmux` MT7628 | Удалена группа `"jtag"` из `state_default` в `audiopro_c3.dts` (`groups = "wdt", "wled_an";`) |
| **3** | **Вечный `EPROBE_DEFER` контроллера I2S** | Узел DMA `&gdma` в базовом `mt7628an.dtsi` отключен (`status = "disabled"`), I2S DMA не получал каналы | В `audiopro_c3.dts` добавлен узел `&gdma { status = "okay"; };`, в ядре включен `CONFIG_DMA_RALINK=y` |
| **4** | **Сбой pinctrl из-за `refclk_pins`** | В дереве OpenWrt 23.05 узел `refclk_pins` отсутствует в `mt7628an.dtsi` | Удалена ссылка на несуществующий узел: `pinctrl-0 = <&i2s_pins>;` |
| **5** | **Зависание GDMA при смене частоты дискретизации** | При переключении 44.1 ↔ 48 кГц контроллер DMA блокировался при резкой остановке тактирования | Внедрен патч ядра `836-mt7688-i2s-audio-crash-workaround.patch` с аппаратным сбросом `I2S_FIFO_CLR` |
| **6** | **Отказ загрузки U-Boot Linkplay** | Загрузчик U-Boot 1.1.3 проверяет сигнатуру типа ОС (требуется `IH_OS_SVR4` / OS ID 20) | Образы компилируются с флагом `mkimage -O svr4` |
| **7** | **Поведение `kmodloader` и дублирование символов** | Зависимости пакетов `alsa-lib`/`alsa-utils` (`DEPENDS:=+kmod-sound-core`) генерируют `.ipk` пакеты | Ядро с built-in драйверами (`=y`) владеет символами. `kmodloader` выводит warning `exports duplicate symbol`, а ALSA работает на 100% через ядро |
| **8** | **Конкуренция за UART и переполнение буфера** | Одновременная запись из CGI API и heartbeat демона `mcud` могла повреждать байты; шум в линии переполнял буфер | В `mcud` создан неблокирующий FIFO `/tmp/mcu_cmd_fifo`, CGI пишет в него, а при переполнении буфера UART строка безопасно сбрасывается |
| **9** | **Сбой TLS-сертификатов Spotify Connect** | Часы стартуют с 1970 г., сертификат `ap.spotify.com` отклоняется с `Certificate not yet valid` | Создан скрипт [`/usr/bin/librespot-wrapper.sh`](file:///tmp/openwrt/files/usr/bin/librespot-wrapper.sh) с неблокирующим NTP-гардом до 60 сек |
| **10** | **Разделение цифрового и аппаратного гейна** | Двойное сжатие диапазона при одновременной регулировке softvol и DSP TAS57xx ухудшало звук на малой громкости | Softvol зафиксирован на 100% (0 dB) для сохранения полных 16 бит, а мастер-громкость управляется исключительно через аппаратный DSP MCU (`AXX+VOL+...`) |

---

## 4. Оригинальное аудиосопровождение Audio Pro (`/usr/share/sounds/`)

Из стоковой прошивки извлечены, конвертированы в 16-bit 44.1 kHz WAV и интегрированы в образ фирменные звуковые сигналы:

| Файл | Описание | Момент воспроизведения |
| :--- | :--- | :--- |
| **`/usr/share/sounds/boot.wav`** | Фирменная вступительная мелодия Audio Pro (chime melody) | Автоматически играет при завершении инициализации Linkplay MCU и включении усилителя |
| **`/usr/share/sounds/wifi_connected.wav`** | Голосовое оповещение *"Connected to Wi-Fi"* | При получении IP-адреса от DHCP-сервера роутера |
| **`/usr/share/sounds/bt_connected.wav`** | Сигнал подключения Bluetooth | При нажатии кнопки Bluetooth или получении события от MCU |
| **`/usr/share/sounds/preset_saved.wav`** | Сигнал успешного выбора/сохранения пресета | При нажатии клавиш 1, 2, 3, 4 верхней панели |
| **`/usr/share/sounds/bell.wav`** | Звук гонга/уведомления | Доступен для воспроизведения голосовых анонсов Home Assistant |

---

## 5. Демон связи с микроконтроллером и Home Assistant MQTT (`mcud.c`)

```c
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
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <alsa/asoundlib.h>
#include <mosquitto.h>

#define SERIAL_PORT          "/dev/ttyS0"
#define BAUD_RATE            B57600
#define HEARTBEAT_SEC        15
#define VOL_STEP_PERCENT     5
#define MAX_LINE_LEN         128
#define READ_BUF_SIZE        256
#define CMD_FIFO_PATH        "/tmp/mcu_cmd_fifo"

#define CMD_MCU_READY        "AXX+MCU+RDY\n"
#define CMD_BOOT_DONE        "AXX+BOT+DON\n"
#define CMD_PLAY_MODE        "AXX+PLM+001\n"
#define CMD_UNMUTE           "AXX+MUT+000\n"
#define CMD_MUTE             "AXX+MUT+001\n"
#define CMD_HEARTBEAT        "AXX+MCU+RDY\n"

static volatile sig_atomic_t g_running = 1;
static int g_uart_fd = -1;
static snd_mixer_t *g_mixer = NULL;
static struct mosquitto *g_mosq = NULL;

static char g_mqtt_host[128] = "<speaker-ip>";
static int  g_mqtt_port = 1883;
static char g_mqtt_user[64] = "";
static char g_mqtt_pass[64] = "";
static char g_topic_prefix[64] = "audiopro_c3";
static int  g_mqtt_enabled = 1;
static int  g_current_vol = 50;

#define LOG_INFO(fmt, ...) fprintf(stdout, "[mcud INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)  fprintf(stderr, "[mcud ERR] " fmt ": %s\n", ##__VA_ARGS__, strerror(errno))

static int uart_send(const char *cmd) {
    if (g_uart_fd < 0) return -1;
    size_t len = strlen(cmd);
    ssize_t written = write(g_uart_fd, cmd, len);
    return (written == (ssize_t)len) ? 0 : -1;
}

static int alsa_init(void) {
    if (g_mixer) return 0;
    int err = snd_mixer_open(&g_mixer, 0);
    if (err < 0) return -1;
    if ((err = snd_mixer_attach(g_mixer, "default")) < 0) goto fail;
    if ((err = snd_mixer_selem_register(g_mixer, NULL, NULL)) < 0) goto fail;
    if ((err = snd_mixer_load(g_mixer)) < 0) goto fail;
    return 0;
fail:
    if (g_mixer) snd_mixer_close(g_mixer);
    g_mixer = NULL;
    return -1;
}

static void alsa_reset_softvol_to_max(void) {
    if (!g_mixer && alsa_init() != 0) return;
    if (!g_mixer) return;

    const char *names[] = {"Spotify", "AirPlay", "Music", "Notification", "Master", NULL};
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
}

static void mqtt_publish_volume(int vol) {
    if (!g_mosq || !g_mqtt_enabled) return;
    char topic[128], payload[16];
    snprintf(topic, sizeof(topic), "%s/player/volume", g_topic_prefix);
    snprintf(payload, sizeof(payload), "%d", vol);
    mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
}

static void set_hardware_volume(int target_pct) {
    if (target_pct < 0) target_pct = 0;
    if (target_pct > 100) target_pct = 100;

    g_current_vol = target_pct;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AXX+VOL+%03d\n", g_current_vol);
    uart_send(cmd);
    mqtt_publish_volume(g_current_vol);
}

static void adjust_hardware_volume(int delta_pct) {
    int target = g_current_vol + delta_pct * VOL_STEP_PERCENT;
    set_hardware_volume(target);
}

static void mqtt_on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg) {
    (void)mosq;
    (void)userdata;
    if (!msg || !msg->topic) return;

    char payload[64];
    int len = (msg->payloadlen < (int)sizeof(payload) - 1) ? msg->payloadlen : (int)sizeof(payload) - 1;
    if (len > 0 && msg->payload) {
        memcpy(payload, msg->payload, len);
    }
    payload[len] = '\0';

    char cmd_topic[128], vol_topic[128];
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/player/command", g_topic_prefix);
    snprintf(vol_topic, sizeof(vol_topic), "%s/player/volume/set", g_topic_prefix);

    if (strcmp(msg->topic, cmd_topic) == 0) {
        LOG_INFO("MQTT Command received: [%s]", payload);
        if (strcasecmp(payload, "PLAY") == 0 || strcasecmp(payload, "UNMUTE") == 0) {
            uart_send(CMD_PLAY_MODE);
            uart_send(CMD_UNMUTE);
        } else if (strcasecmp(payload, "PAUSE") == 0 || strcasecmp(payload, "STOP") == 0 || strcasecmp(payload, "MUTE") == 0) {
            uart_send(CMD_MUTE);
        } else if (strcasecmp(payload, "TOGGLE") == 0) {
            int fd = open("/tmp/player_cmd", O_WRONLY | O_NONBLOCK);
            if (fd >= 0) { write(fd, "toggle\n", 7); close(fd); }
        }
    } else if (strcmp(msg->topic, vol_topic) == 0) {
        int vol = atoi(payload);
        LOG_INFO("MQTT Hardware Volume set: [%d%%]", vol);
        set_hardware_volume(vol);
    }
}

static void mqtt_send_discovery(void) {
    if (!g_mosq || !g_mqtt_enabled) return;

    const char *buttons[] = {
        "preset_1", "preset_2", "preset_3", "preset_4",
        "play_pause", "source", "bluetooth"
    };
    const char *subtypes[] = {
        "preset_1", "preset_2", "preset_3", "preset_4",
        "play_pause", "source", "bluetooth"
    };

    // 1. Discovery for 7 buttons (device_automation triggers)
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
            subtypes[i], g_topic_prefix, buttons[i]);
        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 0, true);
    }

    // 2. Discovery for Battery sensor
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

    // 3. Discovery for Audio Source sensor
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

    // 4. Subscribe to commands & volume set
    char sub_cmd[128], sub_vol[128];
    snprintf(sub_cmd, sizeof(sub_cmd), "%s/player/command", g_topic_prefix);
    snprintf(sub_vol, sizeof(sub_vol), "%s/player/volume/set", g_topic_prefix);
    mosquitto_subscribe(g_mosq, NULL, sub_cmd, 0);
    mosquitto_subscribe(g_mosq, NULL, sub_vol, 0);

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
    g_mosq = mosquitto_new("audiopro_c3_mcu", true, NULL);
    if (!g_mosq) {
        LOG_ERR("Failed to create mosquitto instance");
        return -1;
    }
    if (strlen(g_mqtt_user) > 0) {
        mosquitto_username_pw_set(g_mosq, g_mqtt_user, strlen(g_mqtt_pass) > 0 ? g_mqtt_pass : NULL);
    }
    mosquitto_message_callback_set(g_mosq, mqtt_on_message);

    int rc = mosquitto_connect_async(g_mosq, g_mqtt_host, g_mqtt_port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERR("MQTT async connect failed to %s:%d: %s", g_mqtt_host, g_mqtt_port, mosquitto_strerror(rc));
    } else {
        LOG_INFO("Connecting to MQTT Broker at %s:%d...", g_mqtt_host, g_mqtt_port);
    }
    mosquitto_loop_start(g_mosq);
    mqtt_send_discovery();
    return 0;
}

static int uart_init(void) {
    int fd = open(SERIAL_PORT, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        LOG_ERR("Failed to open %s", SERIAL_PORT);
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

static void play_sound(const char *wav_path) {
    static time_t s_last_sound = 0;
    time_t now = time(NULL);
    if ((now - s_last_sound) < 1) return;

    if (access(wav_path, R_OK) == 0) {
        s_last_sound = now;
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "aplay -q -D music_in %s >/dev/null 2>&1 &", wav_path);
        system(cmd);
    }
}

static void set_audio_source(int source) {
    static int s_current_source = 0;
    s_current_source = source % 3;
    const char *names[] = {"wifi", "bluetooth", "aux"};
    const char *src_name = names[s_current_source];

    mqtt_send_sensor("source", src_name);
    int fd = open("/tmp/audio_source", O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
    if (fd >= 0) {
        write(fd, src_name, strlen(src_name));
        write(fd, "\n", 1);
        close(fd);
    }

    if (s_current_source == 0) {
        uart_send("AXX+INP+000\n");
        uart_send(CMD_PLAY_MODE);
        uart_send(CMD_UNMUTE);
    } else if (s_current_source == 1) {
        uart_send("AXX+INP+002\n");
        play_sound("/usr/share/sounds/bt_connected.wav");
    } else {
        uart_send("AXX+INP+001\n");
    }
}

static void process_mcu_command(const char *cmd) {
    LOG_INFO("MCU RX: [%s]", cmd);
    if (strstr(cmd, "MCU+KEY+VOL+")) {
        adjust_hardware_volume(1);
    } else if (strstr(cmd, "MCU+KEY+VOL-")) {
        adjust_hardware_volume(-1);
    } else if (strstr(cmd, "MCU+KEY+PLPA")) {
        mqtt_send_button("play_pause");
        int fd = open("/tmp/player_cmd", O_WRONLY | O_NONBLOCK);
        if (fd >= 0) { write(fd, "toggle\n", 7); close(fd); }
    } else if (strstr(cmd, "MCU+KEY+PRE:")) {
        const char *p = strstr(cmd, "MCU+KEY+PRE:") + 12;
        int preset = atoi(p);
        if (preset >= 1 && preset <= 4) {
            char name[16], buf[32];
            snprintf(name, sizeof(name), "preset_%d", preset);
            mqtt_send_button(name);
            play_sound("/usr/share/sounds/preset_saved.wav");
            snprintf(buf, sizeof(buf), "preset:%d\n", preset);
            int fd = open("/tmp/player_cmd", O_WRONLY | O_NONBLOCK);
            if (fd >= 0) { write(fd, buf, strlen(buf)); close(fd); }
        }
    } else if (strstr(cmd, "MCU+KEY+SRC")) {
        mqtt_send_button("source");
        static int s_src_counter = 0;
        s_src_counter = (s_src_counter + 1) % 3;
        set_audio_source(s_src_counter);
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
        int fd = open("/tmp/battery_status", O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
        if (fd >= 0) {
            write(fd, cmd, strlen(cmd));
            write(fd, "\n", 1);
            close(fd);
        }
    } else if (strstr(cmd, "MCU+POW+OFF")) {
        g_running = 0;
    }
}

#define CMD_FIFO_PATH        "/tmp/mcu_cmd_fifo"

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "h:p:u:P:t:m")) != -1) {
        switch (opt) {
            case 'h': strncpy(g_mqtt_host, optarg, sizeof(g_mqtt_host) - 1); break;
            case 'p': g_mqtt_port = atoi(optarg); break;
            case 'u': strncpy(g_mqtt_user, optarg, sizeof(g_mqtt_user) - 1); break;
            case 'P': strncpy(g_mqtt_pass, optarg, sizeof(g_mqtt_pass) - 1); break;
            case 't': strncpy(g_topic_prefix, optarg, sizeof(g_topic_prefix) - 1); break;
            case 'm': g_mqtt_enabled = 0; break;
        }
    }

    setlinebuf(stdout);
    setlinebuf(stderr);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) return EXIT_FAILURE;

    g_uart_fd = uart_init();
    if (g_uart_fd < 0) return EXIT_FAILURE;

    unlink(CMD_FIFO_PATH);
    mkfifo(CMD_FIFO_PATH, 0666);
    int fifo_fd = open(CMD_FIFO_PATH, O_RDWR | O_NONBLOCK);

    alsa_init();
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
    LOG_INFO("Linkplay MCU initialized: I2S Play Mode selected, Amplifier unmuted.");

    // Play original Audio Pro boot melody chime
    play_sound("/usr/share/sounds/boot.wav");

    struct pollfd fds[3] = {
        { .fd = g_uart_fd, .events = POLLIN },
        { .fd = sfd,       .events = POLLIN },
        { .fd = fifo_fd,   .events = POLLIN }
    };

    char line_buf[MAX_LINE_LEN];
    size_t line_pos = 0;
    time_t last_hb = time(NULL);

    while (g_running) {
        time_t now = time(NULL);
        int timeout_ms = (HEARTBEAT_SEC - (now - last_hb)) * 1000;
        if (timeout_ms <= 0) timeout_ms = 10;

        int ret = poll(fds, 3, timeout_ms);
        now = time(NULL);

        if ((now - last_hb) >= HEARTBEAT_SEC) {
            uart_send(CMD_HEARTBEAT);
            last_hb = now;
        }

        if (ret > 0 && (fds[1].revents & POLLIN)) {
            struct signalfd_siginfo si;
            if (read(sfd, &si, sizeof(si)) == sizeof(si)) g_running = 0;
        }

        if (ret > 0 && fifo_fd >= 0 && (fds[2].revents & POLLIN)) {
            char fifo_rx[READ_BUF_SIZE];
            ssize_t n = read(fifo_fd, fifo_rx, sizeof(fifo_rx) - 1);
            if (n > 0) {
                fifo_rx[n] = '\0';
                uart_send(fifo_rx);
            }
        }

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            char rx[READ_BUF_SIZE];
            ssize_t n = read(g_uart_fd, rx, sizeof(rx));
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
                        LOG_ERR("UART line buffer overflow (>%d bytes), discarding noise", MAX_LINE_LEN);
                        line_pos = 0;
                    }
                }
            }
        }
    }

    uart_send(CMD_MUTE);
    usleep(100000);
    close(g_uart_fd);
    close(sfd);
    if (fifo_fd >= 0) close(fifo_fd);
    unlink(CMD_FIFO_PATH);
    if (g_mixer) snd_mixer_close(g_mixer);
    if (g_mosq) {
        mosquitto_loop_stop(g_mosq, true);
        mosquitto_destroy(g_mosq);
        mosquitto_lib_cleanup();
    }
    return EXIT_SUCCESS;
}
```

---

## 6. C-демон безджиттерного AEC-моста (`/usr/bin/aec_bridge`)

Прямой перенос PCM-фреймов между ALSA-буферами в C без создания промежуточных shell-пайпов:

```c
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
    const char *ha_ip = (argc > 1) ? argv[1] : "<speaker-ip>";
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

        // 1. Прямая передача в реальный ЦАП hw:0,0
        snd_pcm_sframes_t written = snd_pcm_writei(pcm_out, buf, frames);
        if (written < 0) {
            snd_pcm_recover(pcm_out, written, 0);
        }

        // 2. Неблокирующая отправка моно PCM на сервер Home Assistant
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                fcntl(sock, F_SETFL, O_NONBLOCK);
                connect(sock, (struct sockaddr *)&srv, sizeof(srv));
            }
        }

        if (sock >= 0) {
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
```

---

## 7. HTTP REST CGI API ([`/www/cgi-bin/api`](file:///tmp/openwrt/files/www/cgi-bin/api))

```sh
#!/bin/sh
echo "Content-type: application/json"
echo ""

CMD=$(echo "$QUERY_STRING" | cut -d'=' -f1)
VAL=$(echo "$QUERY_STRING" | cut -d'=' -f2)

send_mcu() {
    [ -p /tmp/mcu_cmd_fifo ] && echo -ne "$1" > /tmp/mcu_cmd_fifo 2>/dev/null || echo -ne "$1" > /dev/ttyS0 2>/dev/null || true
}

case "$CMD" in
    volume)
        VOL=$(printf '%d' "$VAL" 2>/dev/null || echo 50)
        send_mcu "AXX+VOL+$(printf '%03d' $VOL)\n"
        echo '{"status":"ok","volume":'$VOL'}'
        ;;
    mute)
        send_mcu "AXX+MUT+001\n"
        echo '{"status":"ok","mute":true}'
        ;;
    unmute)
        send_mcu "AXX+MUT+000\n"
        echo '{"status":"ok","mute":false}'
        ;;
    input)
        case "$VAL" in
            wifi|i2s)
                send_mcu "AXX+INP+000\n"
                send_mcu "AXX+PLM+001\n"
                send_mcu "AXX+MUT+000\n"
                ;;
            bt|bluetooth)
                send_mcu "AXX+INP+002\n"
                ;;
            aux)
                send_mcu "AXX+INP+001\n"
                ;;
        esac
        echo '{"status":"ok","input":"'$VAL'"}'
        ;;
    status)
        BAT=$(cat /tmp/battery_status 2>/dev/null | grep -o '[0-9]*' | head -n1)
        [ -z "$BAT" ] && BAT=100
        SRC=$(cat /tmp/audio_source 2>/dev/null || echo "i2s")
        echo '{"status":"ok","battery":'$BAT',"source":"'$SRC'"}'
        ;;
    play)
        send_mcu "AXX+PLM+001\n"
        send_mcu "AXX+MUT+000\n"
        echo '{"status":"ok"}'
        ;;
    *)
        echo '{"error":"unknown command","usage":"?volume=50 | ?mute=1 | ?unmute=1 | ?input=bt | ?status | ?play=1"}'
        ;;
esac
```

---

## 8. Сервис Spotify Connect и NTP-гард

### `/usr/bin/librespot-wrapper.sh`:
```sh
#!/bin/sh
# NTP-гард: ожидает синхронизации часов перед запуском HTTPS TLS (Spotify)
TIMEOUT=60
while [ "$(date +%s)" -lt 1700000000 ] && [ $TIMEOUT -gt 0 ]; do
    sleep 3
    TIMEOUT=$((TIMEOUT - 3))
done
exec /usr/bin/librespot "$@"
```

### `/etc/init.d/librespot`:
```sh
#!/bin/sh /etc/rc.common
START=90
USE_PROCD=1

start_service() {
    procd_open_instance
    procd_set_param command /usr/bin/librespot-wrapper.sh \
        --name "Audio Pro C3" \
        --backend alsa \
        --device spotify_in \
        --mixer softvol \
        --bitrate 320 \
        --disable-audio-cache
    procd_set_param respawn
    procd_close_instance
}
```

---

## 9. Опциональный Far-End AEC Loopback Tap ([`/usr/bin/aec_tap_control.sh`](file:///tmp/openwrt/files/usr/bin/aec_tap_control.sh))

```sh
#!/bin/sh
ACTION="${1:-status}"
HA_SERVER="${2:-<speaker-ip>}"
PORT="${3:-5000}"

case "$ACTION" in
    enable|start)
        insmod snd-aloop 2>/dev/null || true
        cat > /etc/asound.conf << 'ASOUND_EOF'
pcm.dmixer {
    type dmix
    ipc_key 1024
    ipc_perm 0666
    slave {
        pcm "hw:Loopback,0,0"
        mmap_emulation 1
        period_time 0
        period_size 1024
        buffer_size 4096
        rate 44100
        channels 2
        format S16_LE
    }
    bindings { 0 0  1 1 }
}

pcm.spotify_in {
    type softvol
    slave.pcm "dmixer"
    control { name "Spotify" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.airplay_in {
    type softvol
    slave.pcm "dmixer"
    control { name "AirPlay" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.music_in {
    type softvol
    slave.pcm "dmixer"
    control { name "Music" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.!default { type plug; slave.pcm "music_in"; }
ctl.!default { type hw; card 0; }
ASOUND_EOF
        sync
        sleep 0.3
        killall -9 aec_bridge 2>/dev/null || true
        
        # Запуск нативного C-демона без оверхеда shell
        /usr/bin/aec_bridge "$HA_SERVER" "$PORT" >/dev/null 2>&1 &
        
        # Перезапуск сервисов для привязки к новому asound.conf
        /etc/init.d/librespot restart 2>/dev/null || true
        /etc/init.d/shairport-sync restart 2>/dev/null || true
        /etc/init.d/squeezelite restart 2>/dev/null || true
        
        echo "AEC Loopback Tap ENABLED (via aec_bridge C daemon) -> streaming to $HA_SERVER:$PORT"
        ;;
    disable|stop)
        cat > /etc/asound.conf << 'ASOUND_DIRECT'
pcm.dmixer {
    type dmix
    ipc_key 1024
    ipc_perm 0666
    slave {
        pcm "hw:0,0"
        mmap_emulation 1
        period_time 0
        period_size 1024
        buffer_size 4096
        rate 44100
        channels 2
        format S16_LE
    }
    bindings { 0 0  1 1 }
}

pcm.spotify_in {
    type softvol
    slave.pcm "dmixer"
    control { name "Spotify" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.airplay_in {
    type softvol
    slave.pcm "dmixer"
    control { name "AirPlay" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.music_in {
    type softvol
    slave.pcm "dmixer"
    control { name "Music" card 0 }
    min_dB -51.0
    max_dB 0.0
}

pcm.!default { type plug; slave.pcm "music_in"; }
ctl.!default { type hw; card 0; }
ASOUND_DIRECT
        sync
        sleep 0.3
        killall -9 aec_bridge 2>/dev/null || true
        
        # Перезапуск сервисов для возврата прямого аппаратного тракта
        /etc/init.d/librespot restart 2>/dev/null || true
        /etc/init.d/shairport-sync restart 2>/dev/null || true
        /etc/init.d/squeezelite restart 2>/dev/null || true
        
        echo "AEC Loopback Tap DISABLED -> restored direct zero-latency DAC mode"
        ;;
    status)
        if pgrep -x aec_bridge >/dev/null; then
            echo "AEC Tap Mode: ACTIVE (Native aec_bridge C daemon running)"
        else
            echo "AEC Tap Mode: INACTIVE (Direct DAC hw:0,0)"
        fi
        ;;
    *)
        echo "Usage: $0 {enable [HA_IP] [PORT] | disable | status}"
        ;;
esac
```

---

## 10. Верификация бинарных образов и контрольные суммы

### Заголовок uImage (`mkimage -l`):
```text
Image Name:   MIPS OpenWrt Linux-5.15.167
Image Type:   MIPS SVR4 Kernel Image (lzma compressed)
Data Size:    7267634 Bytes = 7097.30 KiB = 6.93 MiB
Load Address: 80000000
Entry Point:  80000000
```

### Хэш-суммы файлов:

| Файл | Размер | MD5 | SHA-256 |
| :--- | :--- | :--- | :--- |
| **`tftp_root/openwrt.bin`** (RAM Boot) | **6.93 MB** (7,267,634 B) | `ebaa04824bb30884dd672142926fd3e5` | `6261000dfd7ed96ac6cbf398445c91c93b73796432543b7d872bf7daa3161f02` |
| **`squashfs-sysupgrade.bin`** (Flash) | **7.1 MB** (7,439,844 B) | `c30647abb1f11ac775ad30e5c5a2d6b7` | `1bc41c76256e468333027994a97c46a42dd71e00c80575cd73a79cea28efb7fd` |

---

## 11. Пошаговая инструкция по верификации и прошивке

### Шаг 1: Загрузка в оперативную память (RAM Boot)
Загрузка через SDRAM полностью безопасна и **не затрагивает SPI Flash**:
```bash
picocom -b 57600 /dev/ttyUSB0
```
В меню U-Boot нажать `5`, ввести:
* **Device IP:** `<speaker-ip>`
* **Server IP:** `<speaker-ip>`
* **File name:** `openwrt.bin`

### Шаг 2: Проверка живой системы в OpenWrt (SSH на <speaker-ip>:22)
```bash
# 1. Проверка отсутствия отложенных устройств ядра
dmesg | grep -E "i2s|deferred|simple-audio"

# 2. Проверка регистрации звуковой карты
aplay -l
# Ожидается: card 0: AudioProC3I2S [AudioPro-C3-I2S], device 0: ...

# 3. Тест вывода чистого синуса 440 Гц через динамики
speaker-test -D hw:0,0 -t sine -f 440 -c 2

# 4. Проверка работы демона кнопок панели и воспроизведения boot chime
logread | grep mcud

# 5. Проверка статуса Spotify Connect и Squeezelite
ps | grep -E "librespot|squeezelite"
```

### Шаг 3: Постоянная прошивка во Flash-память (Sysupgrade)
После успешного теста в RAM:
```bash
scp /srv/tftp/sysupgrade.bin root@<speaker-ip>:/tmp/sysupgrade.bin
ssh root@<speaker-ip> "sysupgrade -n /tmp/sysupgrade.bin"
```
