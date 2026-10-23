[env:esp32doit-devkit-v1]
platform = espressif32@6.4.0
board = esp32doit-devkit-v1
framework = arduino
monitor_speed = 115200
upload_speed = 921600

build_flags = 
    -Wl,--allow-multiple-definition
    -DCORE_DEBUG_LEVEL=0

board_build.partitions = huge_app.csv

lib_deps = 
    nrf24/RF24@^1.4.9
    bblanchon/ArduinoJson@^7.1.0
    adafruit/Adafruit SSD1306 @ ^2.5.7
