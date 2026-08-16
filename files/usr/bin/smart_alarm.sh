#!/bin/sh
# Audio Pro C3 - Smart Alarm Script
# Плавно увеличивает громкость и включает любимую радиостанцию

# Настройки по умолчанию
TARGET_VOL=${1:-40} # Целевая громкость 40%
STREAM_URL=${2:-"http://icecast.vrtcdn.be/klara-high.mp3"} # Классическая музыка (Klara)
FADE_STEP=2  # Шаг увеличения громкости (%)
FADE_DELAY=10 # Задержка между шагами (секунды)

# 1. Прибиваем текущие плееры, если они играют (через Arbiter или напрямую)
killall mpg123 >/dev/null 2>&1
killall snapclient >/dev/null 2>&1
killall shairport-sync >/dev/null 2>&1

# 2. Сбрасываем громкость ALSA на минимум (чтобы не было резкого удара по ушам)
amixer sset Master 5% >/dev/null 2>&1

# 3. Играем приветственный звук будильника
aplay /usr/share/sounds/bell.wav >/dev/null 2>&1

# 4. Запускаем интернет-радио в фоне
mpg123 "$STREAM_URL" >/dev/null 2>&1 &
ALARM_PID=$!

# 5. Плавное нарастание громкости (Fade-in)
CURRENT_VOL=5
while [ $CURRENT_VOL -lt $TARGET_VOL ]; do
    CURRENT_VOL=$((CURRENT_VOL + FADE_STEP))
    amixer sset Master ${CURRENT_VOL}% >/dev/null 2>&1
    sleep $FADE_DELAY
done

# 6. Будильник играет ровно 1 час, затем сам выключается
sleep 3600
kill $ALARM_PID >/dev/null 2>&1
