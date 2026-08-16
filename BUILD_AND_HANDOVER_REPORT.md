# 📄 Полный технический отчёт о сборке и текущем состоянии проекта (Audio Pro Addon C3 / OpenWrt 23.05)

---

## 1. Аппаратная спецификация (Hardware Overview)

* **Устройство:** Акустическая колонка **Audio Pro Addon C3** (аккумуляторная версия Addon T3 с Wi-Fi/BT модулем).
* **Плата сетевого/аудиопроцессора:** **Linkplay A28 V01** (расположена внутри корпуса на разъеме).
* **Главный процессор (SoC):** **MediaTek MT7688AN / MT7628** (MIPS 24KEc, 580 МГц, 64 КБ I-Cache, 32 КБ D-Cache).
* **Оперативная память (SDRAM):** **64 МБ DDR2** (физический диапазон `0x80000000` – `0x83FFFFFF`).
* **Флеш-память:** **16 МБ SPI NOR Flash** (**Winbond W25Q128BV**, сектор стирания 64 КБ).
* **Вторичный MCU:** **STM8S** (управляет кнопками панели: громкость, Play/Pause, BT, Source, Presets 1–4, питанием и индикацией; общается с MT7688 по UART `/dev/ttyS0` на скорости 57600 бод).
* **Основная консоль отладки:** UART1 Lite (**`/dev/ttyS1`**, 57600 8N1, контакты на плате).
* **Аудиоинтерфейс:** MT7688 Hardware I2S Master -> Проприетарный ЦАП/усилитель через simple-audio-card.
* **Сетевой интерфейс:** Встроенный 10/100 Ethernet (`eth0` / `rt3050-esw`) + Wi-Fi 2.4GHz 802.11bgn (`mt7603e` / `mt76_wmac`).

---

## 2. Разметка Flash-памяти (MTD Layout в DTS)

Разметка зафиксирована в `dts/mt7628an_audiopro_c3.dts` и протестирована на живом ядре Linux 5.15:

| MTD Раздел | Имя в ядре | Смещение | Размер | Назначение / Защита |
| :--- | :--- | :--- | :--- | :--- |
| `mtd0` | `u-boot` | `0x00000000` | 192 KB (`0x00030000`) | Заводской загрузчик Ralink U-Boot 1.1.3 (Read-only). |
| `mtd1` | `u-boot-env` | `0x00030000` | 64 KB (`0x00010000`) | Переменные окружения U-Boot (`ipaddr`, `serverip`, `bootcmd`). |
| `mtd2` | `factory` | `0x00040000` | 64 KB (`0x00010000`) | EEPROM / калибровки радиомодуля Wi-Fi и MAC-адрес (Read-only). |
| `mtd3` | `bkKernel` | `0x00050000` | 2048 KB (`0x00200000`) | Заводское резервное ядро Linkplay (Read-only). |
| `mtd4` | **`firmware`** | **`0x00250000`** | **11.18 MB (`0x00B30000`)** | **Рабочий раздел OpenWrt (Ядро uImage + SquashFS + JFFS2 Overlay).** |
| `mtd5` | `user` | `0x00D80000` | 512 KB (`0x00080000`) | Заводские ключи, сертификаты Linkplay/AudioPro (Read-only). |
| `mtd6` | `user2` | `0x00E00000` | 2048 KB (`0x00200000`) | Заводские пресеты и медиафайлы (Read-only). |

Размер раздела `firmware` жестко ограничен `0x00B30000` (11.18 МБ). Это гарантирует, что авто-форматирование SquashFS overlay (rootfs_data) в OpenWrt никогда не затрет разделы `mtd5` и `mtd6`.

---

## 3. Среда и процесс сборки (Build Pipeline)

Сборка производится через **OpenWrt ImageBuilder 23.05.5 (ramips/mt76x8)** внутри изолированного Podman-контейнера `openwrt-builder:22.04`.

### Расположение каталогов на хосте:
* **Исходники репозитория BSP:** `/home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3`
* **Каталог ImageBuilder:** `/home/gimgiwer/.gemini/antigravity/scratch/audiopro_openwrt/openwrt-imagebuilder-23.05.5-ramips-mt76x8.Linux-x86_64`
* **TFTP Корень для отдачи образов:** `/home/gimgiwer/.gemini/antigravity/scratch/tftp_root`
* **Готовые бинарники для релиза:** `/home/gimgiwer/.gemini/antigravity/scratch/release_github`

### Процесс компиляции прошивки:
1. **Синхронизация файлов overlay:**
   ```bash
   rm -rf /home/gimgiwer/.gemini/antigravity/scratch/audiopro_openwrt/openwrt-imagebuilder-23.05.5-ramips-mt76x8.Linux-x86_64/files
   cp -r /home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3/files /home/gimgiwer/.gemini/antigravity/scratch/audiopro_openwrt/openwrt-imagebuilder-23.05.5-ramips-mt76x8.Linux-x86_64/files
   ```
2. **Запуск сборки в Podman:**
   ```bash
   podman run --userns=keep-id --rm \
     -v /home/gimgiwer/.gemini/antigravity/scratch/audiopro_openwrt/openwrt-imagebuilder-23.05.5-ramips-mt76x8.Linux-x86_64:/builder:Z \
     openwrt-builder:22.04 bash -c "cd /builder && make image PROFILE=audiopro_c3 FILES=files PACKAGES=\"kmod-sound-mt7620 alsa-utils alsa-lib avahi-nodbus-daemon zram-swap uhttpd px5g-mbedtls uhttpd-mod-lua liblua libuci-lua iwinfo libmosquitto-nossl\""
   ```

---

## 4. Что реализовано в коде репозитория

1. **Супервизор кнопок и питания `mcud.c` (`services/mcud.c`):**
   * Демон на чистом C, взаимодействует с STM8 MCU через `/dev/ttyS0` (57600 бод).
   * Читает события кнопок: `MCU+KEY+VOL+`, `MCU+KEY+VOL-`, `MCU+KEY+PLPA`, `MCU+KEY+PRE:1..4`, `MCU+KEY+SRC`.
   * Управляет питанием, авто-засыпанием, передает уровень заряда батареи (`MCU+BAT+`).
   * Реализовано Home Assistant MQTT Discovery (кнопки, сенсоры батареи, переключатели источников, регулятор громкости).
   * **Безопасность:** Вызовы `system()` полностью заменены на асинхронный `spawn_async_cmd()` через `fork() + execv()`.

2. **Движок REST API и Web UI (`files/www/api.lua`, `files/www/index.html`):**
   * Работает под `uhttpd` с модулем `uhttpd-mod-lua` (без внешних зависимостей, легковесный).
   * Управление будильником (`smart_alarm.sh`), кухонным таймером (`smart_timer.sh`), пресетами, эквалайзером ALSA, сканированием Wi-Fi.
   * **Безопасность:** Внедрена функция `sanitize_shell()`, полностью устраняющая Command Injection (CWE-78) через кавычки, переводы строк (`\r\n`) и нулевые байты. Автоматически отключает Setup AP после успешного подключения к домашней сети.

3. **Аудиостек ALSA (`files/etc/asound.conf`):**
   * Аппаратный dmixer с параметрами `period_size 512`, `buffer_size 2048` (частота прерываний 86 IRQ/s, физическая аппаратная задержка **46.4 мс**).
   * Защита от клиппинга (`-1.0 dBFS` headroom).
   * Поддержка 10 аудиосервисов (Spotify Connect, AirPlay 1, LMS/Squeezelite, DLNA, Snapcast, TTS, VoIP, Alarm, Timer, BT/AUX).

4. **Сетевая конфигурация (`files/etc/config/network`):**
   * Интерфейс `lan` настроен в режиме `proto 'dhcp'` для автоматического получения IP при включении в роутер.

---

## 5. Готовые собранные файлы прошивки

Все собранные образы лежат в каталоге `/home/gimgiwer/.gemini/antigravity/scratch/tftp_root/`:

| Файл | Размер | SHA-256 Checksum | Назначение |
| :--- | :--- | :--- | :--- |
| **`openwrt-23.05.5-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin`** | **9.9 MB** | `0a482a4041b7e4774c723d35965a7d54718a5ee2a45fd26c0ca45deaf65eb1c9` | **Полный боевой образ для записи в SPI Flash (`mtd4: firmware`).** Содержит все 10 сервисов, Web UI, mcud, SquashFS + JFFS2. |
| **`openwrt.bin` (initramfs)** | **5.44 MB** | `9ffb892a061df89ab8a8dc4e341cb8d9e6027a44aeb6885df4b37fc90fa9d2b2` | Легковесный Rescue RAM-образ для загрузки через U-Boot пункт 5. |

---

## 6. Текущий статус устройства и команды для работы

1. **Колонка в данный момент:**
   * Запущена в оперативной памяти (RAM) под управлением OpenWrt 23.05.5 (ядро Linux 5.15.167).
   * Доступна в локальной сети по IP: **`10.0.100.3`**.
   * SSH открыт: `ssh root@10.0.100.3` (без пароля).
   * Web-сервер LuCI отвечает на `http://10.0.100.3/`.
   * SPI Flash **не прошивалась** (чистая, безопасная заводская разметка сохранена).

2. **Команда для постоянной прошивки полного образа во флеш (через SSH):**
   ```bash
   scp -O /home/gimgiwer/.gemini/antigravity/scratch/tftp_root/openwrt-23.05.5-ramips-mt76x8-audiopro_c3-squashfs-sysupgrade.bin root@10.0.100.3:/tmp/firmware.bin
   ssh root@10.0.100.3 "sysupgrade -n /tmp/firmware.bin"
   ```

3. **Команда для повторной сборки образа после правок кода:**
   ```bash
   cd /home/gimgiwer/.gemini/antigravity/scratch/audiopro-c3
   openwrt/install_to_openwrt.sh /home/gimgiwer/.gemini/antigravity/scratch/openwrt_build/openwrt
   rm -rf /home/gimgiwer/.gemini/antigravity/scratch/audiopro_openwrt/openwrt-imagebuilder-23.05.5-ramips-mt76x8.Linux-x86_64/files
   cp -r files /home/gimgiwer/.gemini/antigravity/scratch/audiopro_openwrt/openwrt-imagebuilder-23.05.5-ramips-mt76x8.Linux-x86_64/files
   podman run --userns=keep-id --rm -v /home/gimgiwer/.gemini/antigravity/scratch/audiopro_openwrt/openwrt-imagebuilder-23.05.5-ramips-mt76x8.Linux-x86_64:/builder:Z openwrt-builder:22.04 bash -c "cd /builder && make image PROFILE=audiopro_c3 FILES=files PACKAGES=\"kmod-sound-mt7620 alsa-utils alsa-lib avahi-nodbus-daemon zram-swap uhttpd px5g-mbedtls uhttpd-mod-lua liblua libuci-lua iwinfo libmosquitto-nossl\""
   ```

4. **GitHub репозиторий проекта:**
   * Все коммиты подписаны и синхронизированы в ветку `main`: [github.com/gimgiwer/audiopro-c3](https://github.com/gimgiwer/audiopro-c3).
