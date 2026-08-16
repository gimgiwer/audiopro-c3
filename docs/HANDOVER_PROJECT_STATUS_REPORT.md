# 🚀 Сводный отчёт и пакет передачи проекта: OpenWrt 23.05.5 для Audio Pro Addon C3 (Linkplay A28 V01)

> **Для следующего ИИ-агента:** Данный документ содержит полный контекст, архитектуру, все локальные пути к бинарникам, исходникам, сборочной среде и инструкциям по тестированию.

---

## 1. Репозиторий и статус сборки

* **Git-репозиторий:** [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/)
* **Удалённый репозиторий:** `git@github.com:gimgiwer/audiopro-c3.git` (ветка `main`, полностью синхронизирована)
* **Сборочное окружение OpenWrt:** [`/tmp/openwrt/`](file:///tmp/openwrt/) (OpenWrt 23.05.5 `r24106-10cc5fcd00`, toolchain `mipsel_24kc_musl`)
* **Контейнер сборки:** `openwrt-builder:22.04` (Podman)

---

## 2. Готовые бинарные файлы прошивки

Все бинарники скомпилированы, проверены и доступны в двух локациях:

| Файл | Размер | Назначение | Локальный путь |
| :--- | :--- | :--- | :--- |
| **`openwrt.bin`** | **10.4 MB** (10,874,542 B) | **RAM Boot (SDRAM initramfs uImage)**. Загрузка в память без записи во Flash через TFTP (пункт 5 в U-Boot) | [`/home/gimgiwer/.gemini/antigravity/scratch/tftp_root/openwrt.bin`](file:///home/gimgiwer/.gemini/antigravity/scratch/tftp_root/openwrt.bin) |
| **`openwrt-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin`** | **10.8 MB** (11,272,192 B) | **Flash Sysupgrade Image (SquashFS)**. Для постоянной прошивки раздела `firmware` (пункт 2 в U-Boot или `sysupgrade -v -n`) | [`/home/gimgiwer/.gemini/antigravity/scratch/tftp_root/openwrt-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin`](file:///home/gimgiwer/.gemini/antigravity/scratch/tftp_root/openwrt-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin) |
| **Копии в репозитории** | — | Резервные копии бинарников + sha256sums + manifest | [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/bin/`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/bin/) |

### TFTP-сервер на хосте:
* **IP хоста (TFTP Server):** `192.168.1.202:69` (сервис `tftpd` активен в фоне)
* **IP колонки в U-Boot:** `192.168.1.122`

---

## 3. Архитектура и ключевые компоненты исходного кода

### 3.1. Дерево устройств (Device Tree)
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/dts/audiopro_c3.dts`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/dts/audiopro_c3.dts)
  * `simple-audio-card` в режиме I2S Master (`sound0_cpu: mclk-fs = <256>`).
  * `dummy-codec` (`linux,snd-soc-dummy`).
  * Фиксированный Pinmux `0x54154115` без конфликтующих групп (`"wdt"`, `"wled_an"`).
  * Корректный MTD-лейаут: раздел `firmware` ограничен `0xB30000` (11.18 MB), защищая разделы `user` (`0xD80000`) и `user2` (`0xE00000`) от форматирования оверлеем.

### 3.2. Патчи ядра Linux 5.15
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/openwrt/patches/836-mt7688-i2s-audio-crash-workaround.patch`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/openwrt/patches/836-mt7688-i2s-audio-crash-workaround.patch)
  * Сброс FIFO (`I2S_FIFO_CLR`) перед остановкой I2S-клоков, предотвращающий зависание GDMA-контроллера MediaTek при переключении треков и смене дискретизации 44.1 ↔ 48 кГц.

### 3.3. Демон управления и интеграции с MCU (`mcu_buttond.c`)
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/services/mcu_buttond.c`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/services/mcu_buttond.c)
  * **Монопольное владение UART:** Открывает неблокирующий FIFO `/tmp/mcu_cmd_fifo` для приёма внешних команд от веб-CGI без коллизий с 15-секундным heartbeat (`AXX+MCU+RDY\n`).
  * **Защита от переполнения UART:** Безопасный сброс буфера при получении $>127$ байт шума.
  * **Безопасность MQTT:** Гарантированный нуль-терминированный `payload[64]` в `mqtt_on_message`.
  * **Аппаратный гейн DSP:** `softvol` зафиксирован на 100% (0 dB) для сохранения 16-битной глубины, громкость регулируется исключительно на DSP через `AXX+VOL+...`.
  * **Циклическое переключение источников:** `Wi-Fi (0) → Bluetooth (1) → Aux (2)` по кнопке `MCU+KEY+SRC`.
  * **Home Assistant MQTT Auto-Discovery:** Автоматическая публикация 7 кнопок панели, сенсора АКБ (`/tmp/battery_status`) и сенсора источника (`/tmp/audio_source`).

### 3.4. C-демон безджиттерного AEC Loopback Tap (`aec_bridge.c`)
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/services/aec_bridge.c`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/services/aec_bridge.c)
  * Прямой трансфер PCM-фреймов между `hw:Loopback,1` и `hw:0,0` + неблокирующий стриминг опорного моно-потока в TCP-сокет Home Assistant (нагрузка $\sim 0.2\%$ CPU вместо shell-пайпов `arecord | aplay`).

### 3.5. Аудио-ассеты стоковой прошивки Audio Pro
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/share/sounds/boot.wav`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/share/sounds/boot.wav) — Фирменный Boot Chime Audio Pro при старте.
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/share/sounds/wifi_connected.wav`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/share/sounds/wifi_connected.wav) — "Connected to Wi-Fi".
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/share/sounds/bt_connected.wav`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/share/sounds/bt_connected.wav) — Сигнал подключения Bluetooth.
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/share/sounds/preset_saved.wav`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/share/sounds/preset_saved.wav) — Сигнал пресетов 1–4.
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/share/sounds/bell.wav`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/share/sounds/bell.wav) — Гонг уведомлений HA.

### 3.6. Системные скрипты и сервисы
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/etc/asound.conf`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/etc/asound.conf) — ALSA dmixer + softvol каналы `Spotify`, `AirPlay`, `Music`, `Master`.
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/bin/librespot-wrapper.sh`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/bin/librespot-wrapper.sh) — Неблокирующий NTP-гард для валидности TLS-сертификатов Spotify.
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/bin/ha_ducking.sh`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/bin/ha_ducking.sh) — TTS Ducking (приглушение музыки при голосовых анонсах).
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/bin/aec_tap_control.sh`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/usr/bin/aec_tap_control.sh) — Включение/отключение AEC-тапа с микропаузами `sync` и перезапуском сервисов.
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/www/cgi-bin/api`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/www/cgi-bin/api) — HTTP REST CGI API для Home Assistant.
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/etc/config/squeezelite`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/etc/config/squeezelite) — Конфигурация Squeezelite (Music Assistant / LMS).
* [`/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/etc/config/shairport-sync`](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files/etc/config/shairport-sync) — Конфигурация AirPlay.

---

## 4. Документация в репозитории

1. 📑 [**`FINAL_AUDIT_REPORT_OPENWRT_AUDIO_PRO_C3.md`**](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/docs/FINAL_AUDIT_REPORT_OPENWRT_AUDIO_PRO_C3.md) — Полный технический отчёт аудита, спецификация протоколов MCU и параметры бинарников.
2. 📘 [**`OPENWRT_PORTING_GUIDE.md`**](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/docs/OPENWRT_PORTING_GUIDE.md) — Руководство по портированию OpenWrt на Linkplay A28 V01.
3. 🔬 [**`STOCK_FIRMWARE_ANALYSIS.md`**](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/docs/STOCK_FIRMWARE_ANALYSIS.md) — Анализ стоковой прошивки 2021 г., карта разделов `/proc/mtd`.
4. 🎛️ [**`I2S_HARDWARE_REGISTERS.md`**](file:///home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/docs/I2S_HARDWARE_REGISTERS.md) — Дампы аппаратных регистров I2S, GDMA и Pinmux MT7688.

---

## 5. Инструкция для следующего агента: Проверка и тестирование

### Шаг 1: Подключение к UART и загрузка в RAM
```bash
picocom -b 57600 /dev/ttyUSB0
```
В меню U-Boot нажать `5`:
* **Device IP:** `192.168.1.122`
* **Server IP:** `192.168.1.202`
* **File name:** `openwrt.bin`

### Шаг 2: Тестирование живой системы через SSH (`root@192.168.1.1`)
```bash
# 1. Проверка инициализации звуковой карты:
aplay -l
# Ожидается: card 0: AudioProC3I2S [AudioPro-C3-I2S], device 0: ...

# 2. Воспроизведение синуса:
speaker-test -D hw:0,0 -t sine -f 440 -c 2

# 3. Воспроизведение оригинального boot chime:
aplay -D music_in /usr/share/sounds/boot.wav

# 4. Проверка демона кнопок панели:
logread | grep mcu_buttond

# 5. Проверка статуса стриминговых сервисов:
ps | grep -E "librespot|squeezelite|shairport"
```

### Шаг 3: Постоянная прошивка во Flash
```bash
scp /srv/tftp/sysupgrade.bin root@192.168.1.1:/tmp/sysupgrade.bin
ssh root@192.168.1.1 "sysupgrade -n /tmp/sysupgrade.bin"
```
