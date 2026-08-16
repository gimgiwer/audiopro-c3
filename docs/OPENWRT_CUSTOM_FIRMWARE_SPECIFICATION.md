# 📋 Исчерпывающая техническая спецификация: Кастомная прошивка OpenWrt 23.05 для Audio Pro Addon C3 (Linkplay A28 V01)

Данный документ представляет собой полную инженерную спецификацию и отчёт портирования современной прошивки **OpenWrt 23.05.5 (Linux Kernel 5.15.167)** на беспроводную активную Hi-Fi акустику **Audio Pro Addon C3** (вычислительный модуль **Linkplay A28 V01**, SoC MediaTek MT7688AN, 64 MB RAM, 16 MB SPI Flash).

---

## 1. Аппаратный профиль устройства

| Компонент | Значение / Маркировка | Описание и особенности |
| :--- | :--- | :--- |
| **Устройство** | Audio Pro Addon C3 | Портативная активная акустическая система с питанием от 3S Li-Ion аккумулятора (18650) |
| **Модуль** | **Linkplay A28 V01** | Модуль на черном текстолите, UUID `FF280012`, OUI `00:22:6C` |
| **SoC** | MediaTek MT7688AN | MIPS 24KEc @ 575 MHz, архитектура `mipsel` (Little-Endian), **Soft-Float** (без аппаратного FPU) |
| **RAM** | **64 MB DDR2 SDRAM** | Winbond W9751G6KB-25 (512 Mbit, 16-bit шина, DDR2-800) |
| **Flash** | **16 MB SPI NOR Flash** | Winbond W25Q128BV (`0x00000000` – `0x01000000`) |
| **Аудиотракт** | I2S Master → DSP/Усилитель | MT7688 генерирует шину I2S (MCLK, BCLK, WS, SDO), 16-bit 44.1/48 кГц stereo напрямую на усилитель TAS57xx |
| **MCU управления** | STM8 / Linkplay MCU | Отдельный микроконтроллер: управление питанием, зарядкой АКБ, кнопками панели, светодиодами и Mute/Vol |
| **UART MCU** | **`/dev/ttyS0` @ 57600 8N1** | Внутренняя шина связи между SoC MT7688 и дополнительным MCU платы |
| **UART Консоль** | **`/dev/ttyS1` @ 57600 8N1** | Выведен на сервисные контактные площадки (GND, TX, RX, 3.3V) |
| **Ethernet** | 10/100 Mbps (MT7688 Switch) | Встроенный свитч MT7688 (`portmap = "llllw"`), MAC в Factory `@ 0x28` |
| **Wi-Fi** | 802.11b/g/n 2.4 GHz (1T1R) | Встроенный радиомодуль MT7603/MT7628, калибровки EEPROM в Factory `@ 0x00` |

---

## 2. Карта флеш-памяти (MTD Layout, 16 MB)

```text
0x00000000 - 0x00030000 (192 KB)  : "u-boot"       (U-Boot 1.1.3 Ralink 4.3.0.0, 57600 baud, READ-ONLY)
0x00030000 - 0x00040000 (64 KB)   : "u-boot-env"   (Переменные загрузчика, READ-WRITE)
0x00040000 - 0x00050000 (64 KB)   : "factory"      (EEPROM калибровки Wi-Fi @ 0x04, Ethernet MAC @ 0x28)
0x00050000 - 0x00250000 (2 MB)    : "bkKernel"     (Аварийное заводское ядро 2016 г., READ-ONLY)
0x00250000 - 0x00D80000 (11.18 MB): "firmware"     (OpenWrt: Kernel + SquashFS + RootFS_data / Overlay)
0x00D80000 - 0x00E00000 (512 KB)  : "user"         (Раздел настроек JFFS2)
0x00E00000 - 0x01000000 (2 MB)    : "user2"        (Вендорный раздел сертификатов и ключей)
```

Размер целевого раздела OpenWrt задан в `IMAGE_SIZE := 11456k` (`0xB30000` = 11,730,944 байт).

---

## 3. Решённые архитектурные проблемы и патчи ядра

1. **Канонический кодек `linux,snd-soc-dummy`:**
   - Для чистого I2S потока без фрейминга S/PDIF используется платформенный кодек `linux,snd-soc-dummy` с включенной опцией `CONFIG_SND_SOC_DUMMY_CODEC=y`.
2. **Исключение конфликтов kmod со статической сборкой ядра:**
   - Драйверы звука собраны **статически в ядро (`=y`)**: `CONFIG_SND_SIMPLE_CARD=y`, `CONFIG_SND_SOC_DUMMY_CODEC=y`, `CONFIG_SND_RALINK_SOC_I2S=y`, `CONFIG_DMA_RALINK=y`.
   - В `DEVICE_PACKAGES` включены исключительно userspace-пакеты (`alsa-utils`, `alsa-lib`, `shairport-sync-mbedtls`, `mcu_buttond`, `zram-swap`), что предотвращает ошибки дублирования символов в `kmodloader`.
3. **Устранение Race Condition на шине UART `/dev/ttyS0`:**
   - Команды инициализации MCU перенесены целиком в демон `mcu_buttond`. В `/etc/rc.local` нет прямых записей в `/dev/ttyS0`, что гарантирует отсутствие коллизий.
4. **Heartbeat и надежность `mcu_buttond`:**
   - Демон каждые 15 секунд отправляет команду `AXX+MCU+RDY\n`, предотвращая отключение питания со стороны MCU при длительном простое, и использует `signalfd` для корректного Mute при завершении.
5. **Pinctrl `-EINVAL` и `EPROBE_DEFER`:**
   - Убрана невалидная группа `"jtag"` из `state_default` (`groups = "wdt", "wled_an";`).
   - Убран фантомный узел `refclk_pins`.
   - Включен контроллер DMA: `&gdma { status = "okay"; };`.
6. **Патч I2S DMA FIFO Flush (`patches-5.15/836-mt7688-i2s-audio-crash-workaround.patch`):**
   - Выполняет аппаратный сброс FIFO (`I2S_FIFO_CLR`) перед остановкой потока для предотвращения зависания контроллера GDMA при смене частоты дискретизации.
7. **Совместимость с U-Boot Linkplay (`-O svr4`):**
   - Сборка с параметром `-O svr4` (OS ID 20) обеспечивает 100% совместимость.

---

## 4. Дерево устройств (`target/linux/ramips/dts/audiopro_c3.dts`)

```dts
// SPDX-License-Identifier: GPL-2.0-or-later
/dts-v1/;

#include "mt7628an.dtsi"
#include <dt-bindings/gpio/gpio.h>
#include <dt-bindings/input/input.h>

/ {
    compatible = "audiopro,c3", "linkplay,a28", "mediatek,mt7628an-soc";
    model = "Audio Pro Addon C3 (Linkplay A28 V01)";

    aliases {
        serial0 = &uart1;
        serial1 = &uartlite;
    };

    chosen {
        bootargs = "console=ttyS1,57600";
    };

    sound {
        compatible = "simple-audio-card";
        simple-audio-card,name = "AudioPro-C3-I2S";
        simple-audio-card,format = "i2s";
        simple-audio-card,bitclock-master = <&sound0_cpu>;
        simple-audio-card,frame-master = <&sound0_cpu>;
        simple-audio-card,mclk-fs = <256>;

        sound0_cpu: simple-audio-card,cpu {
            sound-dai = <&i2s>;
        };

        simple-audio-card,codec {
            sound-dai = <&codec_dummy>;
        };
    };

    codec_dummy: dummy-codec {
        compatible = "linux,snd-soc-dummy";
        #sound-dai-cells = <0>;
        status = "okay";
    };
};

&pinctrl {
    state_default: pinctrl0 {
        gpio {
            groups = "wdt", "wled_an";
            function = "gpio";
        };
    };

    i2s_pins: i2s {
        i2s {
            groups = "i2s";
            function = "i2s";
        };
    };
};

&spi0 {
    status = "okay";
    flash@0 {
        compatible = "jedec,spi-nor";
        reg = <0>;
        spi-max-frequency = <40000000>;

        partitions {
            compatible = "fixed-partitions";
            #address-cells = <1>;
            #size-cells = <1>;

            partition@0 {
                label = "u-boot";
                reg = <0x0 0x30000>;
                read-only;
            };

            partition@30000 {
                label = "u-boot-env";
                reg = <0x30000 0x10000>;
            };

            factory: partition@40000 {
                label = "factory";
                reg = <0x40000 0x10000>;
                read-only;
            };

            partition@50000 {
                label = "bkKernel";
                reg = <0x50000 0x200000>;
                read-only;
            };

            firmware: partition@250000 {
                label = "firmware";
                reg = <0x250000 0xB30000>;
                compatible = "denx,uimage";
            };

            partition@d80000 {
                label = "user";
                reg = <0xd80000 0x80000>;
            };

            partition@e00000 {
                label = "user2";
                reg = <0xe00000 0x200000>;
            };
        };
    };
};

&factory {
    compatible = "nvmem-cells";
    #address-cells = <1>;
    #size-cells = <1>;

    macaddr_factory_wifi_4: macaddr@4 {
        reg = <0x4 0x6>;
    };
    macaddr_factory_eth_28: macaddr@28 {
        reg = <0x28 0x6>;
    };
};

&ethernet {
    nvmem-cells = <&macaddr_factory_eth_28>;
    nvmem-cell-names = "mac-address";
    mediatek,portmap = "llllw";
    mtd-mac-address = <&factory 0x28>;
};

&wmac {
    status = "okay";
    mediatek,mtd-eeprom = <&factory 0x0>;
    nvmem-cells = <&macaddr_factory_wifi_4>;
    nvmem-cell-names = "mac-address";
    mtd-mac-address = <&factory 0x4>;
};

&i2s {
    #sound-dai-cells = <0>;
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&i2s_pins>;
};

&uartlite {
    status = "okay";
};

&uart1 {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&uart1_pins>;
};

&gdma {
    status = "okay";
};
```

---

## 5. Опции ядра Linux 5.15 (`target/linux/ramips/mt76x8/config-5.15`)

```config
CONFIG_SOUND=y
CONFIG_SND=y
CONFIG_SND_SOC=y
CONFIG_SND_SOC_DUMMY_CODEC=y
CONFIG_SND_SIMPLE_CARD=y
CONFIG_SND_SIMPLE_CARD_UTILS=y
CONFIG_SND_SOC_SPDIF=y
CONFIG_SND_RALINK_SOC_I2S=y
CONFIG_DMADEVICES=y
CONFIG_DMA_ENGINE=y
CONFIG_DMA_VIRTUAL_CHANNELS=y
CONFIG_DMA_RALINK=y
CONFIG_SND_DMAENGINE_PCM=y
CONFIG_SND_SOC_GENERIC_DMAENGINE_PCM=y
```

---

## 6. Конфигурации и сервисы (`files/`)

### 6.1. Демон связи с микроконтроллером (`mcu_buttond.c`)
Включает 15-секундный heartbeat, обработку сигналов `SIGTERM`/`SIGINT`, неблокирующий ввод-вывод и плавную регулировку громкости через ALSA mixer:

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
#include <sys/signalfd.h>
#include <alsa/asoundlib.h>

#define SERIAL_PORT      "/dev/ttyS0"
#define BAUD_RATE        B57600
#define HEARTBEAT_SEC    15
#define VOL_STEP_PERCENT 5
#define MAX_LINE_LEN     64
#define READ_BUF_SIZE    128

#define CMD_MCU_READY    "AXX+MCU+RDY\n"
#define CMD_BOOT_DONE    "AXX+BOT+DON\n"
#define CMD_PLAY_MODE    "AXX+PLM+001\n"
#define CMD_UNMUTE       "AXX+MUT+000\n"
#define CMD_MUTE         "AXX+MUT+001\n"
#define CMD_HEARTBEAT    "AXX+MCU+RDY\n"

static volatile sig_atomic_t g_running = 1;
static int g_uart_fd = -1;
static snd_mixer_t *g_mixer = NULL;

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

static void alsa_adjust_volume(int delta_pct) {
    if (!g_mixer && alsa_init() != 0) return;
    if (!g_mixer) return;

    const char *names[] = {"Spotify", "AirPlay", "Music", "Notification", "Master", NULL};
    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);

    for (int i = 0; names[i]; i++) {
        snd_mixer_selem_id_set_name(sid, names[i]);
        snd_mixer_elem_t *elem = snd_mixer_find_selem(g_mixer, sid);
        if (elem) {
            long min = 0, max = 100, cur = 50;
            snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
            snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &cur);
            long range = max - min;
            long step = (range * VOL_STEP_PERCENT) / 100;
            if (step == 0) step = 1;
            long new_vol = cur + (delta_pct > 0 ? step : -step);
            if (new_vol < min) new_vol = min;
            if (new_vol > max) new_vol = max;
            snd_mixer_selem_set_playback_volume_all(elem, new_vol);
        }
    }
}

static int uart_send(const char *cmd) {
    if (g_uart_fd < 0) return -1;
    size_t len = strlen(cmd);
    ssize_t written = write(g_uart_fd, cmd, len);
    return (written == (ssize_t)len) ? 0 : -1;
}

static int uart_init(void) {
    int fd = open(SERIAL_PORT, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;
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

static void process_mcu_command(const char *cmd) {
    if (strstr(cmd, "MCU+KEY+VOL+")) {
        alsa_adjust_volume(1);
    } else if (strstr(cmd, "MCU+KEY+VOL-")) {
        alsa_adjust_volume(-1);
    } else if (strstr(cmd, "MCU+KEY+PLPA")) {
        int fd = open("/tmp/player_cmd", O_WRONLY | O_NONBLOCK);
        if (fd >= 0) { write(fd, "toggle\n", 7); close(fd); }
    } else if (strstr(cmd, "MCU+KEY+PRE:")) {
        const char *p = strstr(cmd, "MCU+KEY+PRE:") + 12;
        int preset = atoi(p);
        if (preset >= 1 && preset <= 4) {
            char buf[32];
            snprintf(buf, sizeof(buf), "preset:%d\n", preset);
            int fd = open("/tmp/player_cmd", O_WRONLY | O_NONBLOCK);
            if (fd >= 0) { write(fd, buf, strlen(buf)); close(fd); }
        }
    } else if (strstr(cmd, "MCU+KEY+SRC") || strstr(cmd, "MCU+KEY+BT")) {
        int fd = open("/tmp/audio_source", O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
        if (fd >= 0) { write(fd, "bluetooth\n", 10); close(fd); }
    } else if (strstr(cmd, "MCU+KEY+WIFI") || strstr(cmd, "MCU+KEY+AUX")) {
        int fd = open("/tmp/audio_source", O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK, 0644);
        if (fd >= 0) { write(fd, "i2s\n", 4); close(fd); }
        uart_send(CMD_PLAY_MODE);
        uart_send(CMD_UNMUTE);
    } else if (strstr(cmd, "MCU+BAT+")) {
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

int main(void) {
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

    alsa_init();
    sleep(1);

    // 4-Step Linkplay Handshake
    uart_send(CMD_MCU_READY);
    usleep(150000);
    uart_send(CMD_BOOT_DONE);
    usleep(150000);
    uart_send(CMD_PLAY_MODE);
    usleep(150000);
    uart_send(CMD_UNMUTE);

    struct pollfd fds[2] = {
        { .fd = g_uart_fd, .events = POLLIN },
        { .fd = sfd,       .events = POLLIN }
    };

    char line_buf[MAX_LINE_LEN];
    size_t line_pos = 0;
    time_t last_hb = time(NULL);

    while (g_running) {
        time_t now = time(NULL);
        int timeout_ms = (HEARTBEAT_SEC - (now - last_hb)) * 1000;
        if (timeout_ms <= 0) timeout_ms = 10;

        int ret = poll(fds, 2, timeout_ms);
        now = time(NULL);

        if ((now - last_hb) >= HEARTBEAT_SEC) {
            uart_send(CMD_HEARTBEAT);
            last_hb = now;
        }

        if (ret > 0 && (fds[1].revents & POLLIN)) {
            struct signalfd_siginfo si;
            if (read(sfd, &si, sizeof(si)) == sizeof(si)) g_running = 0;
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
                } else if (c != '\r' && line_pos < MAX_LINE_LEN - 1) {
                    line_buf[line_pos++] = c;
                }
            }
        }
    }

    uart_send(CMD_MUTE);
    usleep(100000);
    close(g_uart_fd);
    close(sfd);
    if (g_mixer) snd_mixer_close(g_mixer);
    return EXIT_SUCCESS;
}
```

### 6.2. ALSA Multi-Channel Mixing (`/etc/asound.conf`)
```text
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
    bindings {
        0 0
        1 1
    }
}

pcm.spotify_in {
    type softvol
    slave.pcm "dmixer"
    control {
        name "Spotify"
        card 0
    }
    min_dB -51.0
    max_dB 0.0
}

pcm.airplay_in {
    type softvol
    slave.pcm "dmixer"
    control {
        name "AirPlay"
        card 0
    }
    min_dB -51.0
    max_dB 0.0
}

pcm.music_in {
    type softvol
    slave.pcm "dmixer"
    control {
        name "Music"
        card 0
    }
    min_dB -51.0
    max_dB 0.0
}

pcm.!default {
    type plug
    slave.pcm "music_in"
}

ctl.!default {
    type hw
    card 0
}
```

---

## 7. Готовые бинарные образы

| Файл | Размер | Назначение |
| :--- | :--- | :--- |
| [**`tftp_root/openwrt.bin`**](file:///home/gimgiwer/.gemini/antigravity/scratch/tftp_root/openwrt.bin) | **6.93 MB** | **Целевой образ для TFTP RAM Boot** (загрузка в оперативную память без прошивки) |
| [`audiopro-c3/bin/openwrt-ramips-mt76x8-audiopro_c3-initramfs-kernel.bin`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/bin/openwrt-ramips-mt76x8-audiopro_c3-initramfs-kernel.bin) | 6.93 MB | Резервная копия initramfs |
| [`audiopro-c3/bin/openwrt-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/bin/openwrt-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin) | 7.1 MB | Образ для постоянной прошивки во Flash через `sysupgrade` или `mtd write` |

### Заголовок uImage (`mkimage -l`):
```text
Image Name:   MIPS OpenWrt Linux-5.15.167
Image Type:   MIPS SVR4 Kernel Image (lzma compressed)
Data Size:    7267634 Bytes = 6.93 MiB
Load Address: 80000000
Entry Point:  80000000
```
