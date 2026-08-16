# Audio Pro C3 / Linkplay A28 - Полная Карта Проекта (AI Context)

Этот документ является **Единым Источником Истины (Single Source of Truth)** для всех AI-агентов, работающих с данным репозиторием. Здесь описаны все файлы, скрипты, демоны, службы и конфигурации кастомной прошивки OpenWrt для умной колонки Audio Pro C3.

---

## 1. Аппаратная платформа и Сборка
* **SoC:** MediaTek MT7688 / MT7628 (MIPS32, 24Kc @ 575 MHz).
* **RAM:** 64 MB DDR2.
* **Flash:** 16 MB SPI NOR Flash (Winbond W25Q128).
* **Критическая Безопасность Flash:** Файл `image/mt76x8.mk` (и любые скрипты сборки) **обязаны** использовать `IMAGE_SIZE := 11456k`. Заводские партиции `user` (`0xD80000`) и `user2` (`0xE00000`, содержат калибровки Wi-Fi, вендорные ключи и сертификаты) начинаются сразу за разделом `firmware` (`0x250000`–`0xD80000`, ровно 11.18 MB = 11456k). Если образ превысит этот размер, OpenWrt затрёт заводские ключи при форматировании rootfs_data!

### Сборочные файлы (Build Pipeline):
* `dts/mt7628an_audiopro_c3.dts` — Device Tree. Включает `i2s` (Master mode, 44.1kHz, `mclk-fs = 256`), `dummy-codec`, Pinmux `0x54154115`, UART-маппинг (`serial0 = &uart1` для MCU, `serial1 = &uart0` для консоли) и защищённую MTD-разметку.
* `openwrt/install_to_openwrt.sh` — Скрипт автоматической установки BSP, DTS, пакета `mcud` и корневого оверлея `files/` в дерево исходников OpenWrt 23.05.

---

## 2. Аудио-Стек и Маршрутизация (ALSA)
Колонка работает полностью локально без сторонних облаков Linkplay. Все плееры выводят звук через нативный ALSA-стек Linux 5.15 (`kmod-sound-mt7620`).

### Конфигурация Сборки (`image/mt76x8.mk`):
```makefile
define Device/audiopro_c3
  SOC := mt7628an
  IMAGE_SIZE := 11456k
  DEVICE_VENDOR := Audio Pro
  DEVICE_MODEL := Addon C3
  DEVICE_VARIANT := Linkplay A28 V01
  DEVICE_DTS := mt7628an_audiopro_c3
  KERNEL := kernel-bin | append-dtb | lzma | uImage lzma -O svr4
  
  DEVICE_PACKAGES := \
    kmod-sound-mt7620 alsa-utils alsa-lib zram-swap mcud \
    shairport-sync-mbedtls \
    librespot \
    mpg123 \
    wpad-basic-mbedtls \
    libmosquitto-ssl \
    uhttpd uhttpd-mod-lua liblua libuci-lua
  
  SUPPORTED_DEVICES += audiopro,c3 linkplay,a28
endef
TARGET_DEVICES += audiopro_c3
```

### Файлы маршрутизации и логики:
* `files/etc/asound.conf` — Главный конфиг ALSA. Создает программный микшер `dmixer` (Direct Mixing 44.1 kHz 16-bit) и разделяет виртуальные каналы:
  1. `music_in` (Канал 1) — Музыка (Spotify, AirPlay, Web Radio, потоки).
  2. `tts_in` (Канал 0) — Системные звуки, SIP-звонки и TTS Home Assistant.
  3. `spotify_in`, `airplay_in` — Промежуточные регуляторы softvol.
* `files/usr/bin/audio_arbiter.sh` — **Аудио-Арбитр**. Управляет приоритетами плееров (`Snapcast (1)` > `AirPlay (2)` > `Spotify (3)` > `DLNA (4)` > `WebRadio (5)` > `Squeezelite (6)`). Приоритеты читаются из `uci get mcud.main`.
* `files/usr/bin/ha_ducking.sh` — **Zero-Latency Ducking**. Автоматически приглушает фоновую музыку (`amixer sset Music -30%`), воспроизводит TTS-уведомление через `tts_in` и плавно возвращает громкость музыки.
* `files/usr/share/sounds/` — Звуки обратной связи (`boot.wav`, `bell.wav`, `wifi_connected.wav`, `low_battery.wav`).

---

## 3. C-Демоны и Службы
* **Демон `mcud` (`services/mcud.c`, `openwrt/package/utils/mcud/`):**
  * Монопольно владеет внутренним портом UART `/dev/ttyS0` (57600 8N1) для общения со вторичным MCU платы.
  * Опрашивает кнопки верхней панели (Preset 1–4, Play/Pause, Vol+/Vol-, Source, Bluetooth).
  * Управляет зарядкой АКБ, светодиодами и регулировкой громкости на чипе усилителя TAS5707 DSP.
  * Принимает внешние команды через неблокирующий FIFO `/tmp/mcu_cmd_fifo`.
  * Публикует Home Assistant MQTT Auto-Discovery (`sensor/battery`, `sensor/source`, `device_automation` для кнопок).
* **Демон `aec_bridge` (`services/aec_bridge.c`):**
  * Захватывает опорный аудиопоток динамиков с виртуальной петли ALSA `snd-aloop` и транслирует его по TCP для подавления эха на стороне Home Assistant Voice.
* `files/usr/bin/aec_tap_control.sh` — Управление виртуальной картой loopback.

---

## 4. Веб-Интерфейс и REST API
* `files/www/api.lua` — Высокопроизводительный in-process REST API движок для `uhttpd-mod-lua`.
  * Обрабатывает команды громкости, пресетов, эквалайзера, Wi-Fi, потоков и обновлений.
  * Читает и сохраняет настройки через `uci`.
  * Реализует криптостойкую сессионную авторизацию (`/tmp/ap_sessions`) с проверкой по `/etc/shadow`.
* `files/www/index.html` — SPA веб-интерфейс управления колонкой.

---

## 5. Безопасность и RCE Kill-Switch
Настройки безопасности хранятся в `/etc/config/mcud` (секция `mcud.main`):
* `option auth_enabled '0/1'` — Включение/отключение парольной защиты веб-интерфейса.
* `option allow_custom_commands '0/1'` — Глобальный рубильник безопасности для выполнения bash-команд по пресетам.
* `files/usr/bin/audiopro_preset_handler.sh` — Обработчик физических кнопок 1–4. Отказывается от выполнения команд (`eval "$CMD"`), если авторизация выключена или заблокированы кастомные команды.

---

## 6. Вспомогательные Службы и Автономность
* `files/etc/baresip/` — SIP/Интерком-клиент для голосовых вызовов из Home Assistant на порт 5060.
* `files/usr/bin/smart_alarm.sh` — Автономный умный будильник с плавным нарастанием громкости (Fade-in) через `mcud` FIFO и воспроизведением интернет-радио через `mpg123`.
* `files/etc/crontabs/root` — Расписание будильника в системном cron:
  ```bash
  # Запуск умного будильника каждый будний день (ПН-ПТ) в 07:00 утра
  # 0 7 * * 1-5 /usr/bin/smart_alarm.sh 40 "http://icecast.vrtcdn.be/klara-high.mp3"
  ```

---

## 7. Конфигурационные файлы
* `files/etc/config/mcud` — Настройки демона MCU, безопасности, MQTT и приоритетов арбитра.
* `files/etc/config/shairport-sync` — Настройки AirPlay.
* `files/etc/init.d/mcud`, `files/etc/init.d/librespot`, `files/etc/init.d/baresip` — Сервисы автозапуска `procd`.
* `files/etc/uci-defaults/99-network-init` — Начальная инициализация сети, ZRAM swap (32MB) и firewall без несуществующих WAN-зон.
