cmake_minimum_required(VERSION 3.16.0)

# The supported owner build is intentionally Arduino-only and is selected by
# platformio.ini. Refuse direct ESP-IDF/mixed-framework configuration so a
# stale sdkconfig can never silently reintroduce one-way silicon settings.
message(FATAL_ERROR
    "Direct ESP-IDF builds are unsupported. Use: pio run -e heltec_wifi_lora_32_V3_reflashable"
)
