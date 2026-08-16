# Audio Pro C3 / Linkplay A28 - Полная Карта Проекта (AI Context)

Этот документ является **Единым Источником Истины (Single Source of Truth)** для всех AI-агентов, работающих с данным репозиторием. Здесь описаны все файлы, скрипты, демоны, службы и конфигурации кастомной прошивки OpenWrt для умной колонки Audio Pro C3.

---

## 1. Аппаратная платформа и Сборка
* **SoC:** MediaTek MT7688 / MT7628 (MIPS32, 24Kc).
* **RAM:** 64 MB.
* **Flash:** 16 MB SPI Flash.
* **Критическая Безопасность Flash:** Файл `image/mt76x8.mk` (и любые скрипты сборки) **обязаны** использовать `IMAGE_SIZE := 11456k`. Заводские партиции `user1`, `user2` (содержат DRM сертификаты Spotify/AirPlay) и `rf` (калибровка Wi-Fi) находятся по адресу `0xB30000` (11.45 MB) и до конца флешки. Если OpenWrt сгенерирует образ большего размера, JFFS2 навсегда затрет эти уникальные ключи при форматировании overlay!

### Сборочные файлы (Build Pipeline):
* `dts/mt7628an_audiopro_c3.dts` — Device Tree. Включает поддержку `i2s-master`, `spdif-dit`, правильную распиновку и защиту MTD.
* `openwrt/install_to_openwrt.sh` — Баш-скрипт. Автоматически копирует все исходники (демоны, оверлей `files/`, DTS) в чистое дерево исходников OpenWrt.

---

## 2. Аудио-Стек и Маршрутизация (ALSA)
Колонка работает полностью локально без облаков Linkplay. Все аудиоплееры — это стандартные пакеты OpenWrt.

### Конфигурация Сборки (`image/mt76x8.mk`)
Для того чтобы все плееры и службы попали в финальную прошивку, блок устройства **обязан** выглядеть следующим образом. Никаких урезанных вариантов!

```makefile
define Device/audiopro_c3
  SOC := mt7628an
  IMAGE_SIZE := 11456k
  DEVICE_VENDOR := Audio Pro
  DEVICE_MODEL := Addon C3
  DEVICE_VARIANT := Linkplay A28 V01
  DEVICE_DTS := mt7628an_audiopro_c3
  KERNEL := kernel-bin | append-dtb | lzma | uImage lzma -O svr4
  
  # Обязательный полный список пакетов для "всеядной" колонки
  DEVICE_PACKAGES := \
    alsa-utils alsa-lib zram-swap mcud \
    shairport-sync-mbedtls \
    librespot \
    gmrender-resurrect \
    snapclient \
    squeezelite-full \
    mpg123 \
    baresip \
    kmod-snd-aloop \
    wpad-basic-mbedtls \
    libmosquitto-nossl mosquitto-client-nossl iwinfo \
    uhttpd uhttpd-mod-lua
  
  SUPPORTED_DEVICES += audiopro,c3 linkplay,a28
endef
TARGET_DEVICES += audiopro_c3
```

### Файлы маршрутизации и логики:
* `files/etc/asound.conf` — Главный конфиг ALSA. Создает программный микшер (dmix) и разделяет потоки на два виртуальных канала:
  1. `music_in` (Слот 1) — для музыки (Spotify, AirPlay и т.д.).
  2. `tts_in` (Слот 0) — для системных звуков, SIP-звонков и TTS (Home Assistant).
* `files/usr/bin/audio_arbiter.sh` — **Аудио-Арбитр**. Разрешает конфликты между плеерами. Имеет строгую иерархию (Priority): Snapcast > AirPlay > Spotify > DLNA > WebRadio. Если запускается приоритетный поток, скрипт убивает (kill) фоновые процессы плееров с низким приоритетом, чтобы звук не смешивался в кашу. Читает настройки из `uci get mcud.main`.
* `files/usr/bin/ha_ducking.sh` — **Аппаратный Ducking**. Работает в фоне (через `inotifywait` на `/proc/asound/pcm`). Когда в `tts_in` (Слот 0) поступает звук (звонок, будильник, уведомление), скрипт аппаратно снижает громкость `music_in` (Слот 1), проигрывает звук `/usr/share/sounds/bell.wav` и возвращает громкость музыки обратно.
* `files/usr/share/sounds/` — Системные WAV-файлы обратной связи (`boot.wav`, `wifi_connected.wav`, `low_battery.wav`).

---

## 3. C-Демоны и Службы (C Daemons)
Кастомные демоны написаны на C и собираются с помощью OpenWrt Makefile (`openwrt/package/utils/mcud/`). Исходники в `services/`.

* **Демон `mcud` (`services/mcud.c`)**:
  * Слушает `/dev/ttyS1` (UART). Общается с внешним микроконтроллером (MCU) на плате.
  * Читает нажатия железных кнопок, уровень заряда батареи, управляет светодиодом Wi-Fi, переключает аппаратные входы (Wi-Fi, Bluetooth, AUX, LINE-IN).
  * Транслирует события (например, "батарея садится") в Home Assistant через MQTT (`libmosquitto`).
* **Демон `aec_bridge` (`services/aec_bridge.c`)**:
  * Служит для реализации Acoustic Echo Cancellation (Подавление эха).
  * Захватывает финальный аудиопоток, который физически идет на динамики (через ALSA loopback `snd-aloop`), и стримит его по TCP в локальную сеть. Home Assistant забирает этот поток, чтобы вычитать музыку из микрофонов комнаты.
* `files/usr/bin/aec_tap_control.sh` — Вспомогательный скрипт, поднимающий виртуальную звуковую карту `snd-aloop` для работы `aec_bridge`.

---

## 4. Веб-Интерфейс и Настройки (Web API)
* `files/www/api.lua` — Бекенд веб-интерфейса (написан на Lua для uhttpd).
  * Обрабатывает все REST API запросы (громкость, пресеты, статус Wi-Fi, эквалайзер).
  * Читает и пишет конфигурации **только через `uci`**.
  * Осуществляет проверку токена авторизации сессии (`ap_sid`), защищая роутер от локальных атак. Сверяет пароль с системным `/etc/shadow`.
* `files/www/index.html` — Фронтенд (Vanilla JS, CSS), Single Page Application.

---

## 5. Безопасность и RCE Kill-Switch
Настройки безопасности хранятся в едином UCI-конфиге: `files/etc/config/mcud` (блок `mcud 'main'`).
* `option auth_enabled '0/1'` — Включает/выключает пароль на веб-интерфейс.
* `option allow_custom_commands '0/1'` — Глобальный рубильник безопасности.
* `files/usr/bin/audiopro_preset_handler.sh` — Скрипт, который вызывается демоном `mcud`, когда пользователь жмет кнопки `1`, `2`, `3` или `4` на колонке.
  * Позволяет выполнять bash-команды из пресетов.
  * **Защита (Kill-Switch):** Скрипт категорически отказывается выполнять команду (`eval "$CMD"`), если включен беспарольный режим (`auth_enabled=0`) или стоит запрет на команды (`allow_custom_commands=0`). Это блокирует уязвимости RCE (Command Injection) внутри домашней LAN-сети.

---

## 6. Вспомогательные Службы
* `files/etc/baresip/config` — Конфигурация SIP-клиента. Установлен в режим Интеркома (`answermode=auto`), принимает звонки на порт 5060, выводит звук в ALSA-канал `tts_in`. Микрофон заглушен (`audio_source=null`).
* `files/usr/bin/smart_alarm.sh` — Скрипт **Умного Будильника**. Приглушает ALSA, плавно увеличивает громкость (fade-in) и запускает Web-радио через `mpg123`.
* `files/etc/crontabs/root` — Системный планировщик `cron`, отвечающий за автономный запуск будильника.
  **Точный формат записи (пример включен в файл по умолчанию):**
  ```bash
  # Запуск умного будильника каждый будний день (ПН-ПТ) в 07:00 утра
  # Аргументы: [Громкость %] [URL потока]
  # 0 7 * * 1-5 /usr/bin/smart_alarm.sh 40 "http://icecast.vrtcdn.be/klara-high.mp3"
  ```

## 7. Файлы настройки конфигураций (UCI/Init)
* `files/etc/config/mcud` — Здесь лежат и настройки безопасности, и приоритеты Audio Arbiter.
* `files/etc/config/squeezelite` — Настройка LMS-клиента.
* `files/etc/config/shairport-sync` — Настройка AirPlay.
* `files/etc/init.d/mcud` и `files/etc/init.d/librespot` — Скрипты автозапуска демонов.

---
**ИНСТРУКЦИЯ ДЛЯ AI АГЕНТОВ:** Прежде чем менять архитектуру или предлагать новые инструменты, обязательно сверьтесь с этим документом. Не создавайте дублирующие конфиги. Вся система построена на парадигме **"UCI + ALSA + Shell/Lua + C-Daemons"**, и она должна оставаться легковесной, безопасной и стабильной в условиях встроенных (Embedded) систем с 16 MB флэш-памяти.
