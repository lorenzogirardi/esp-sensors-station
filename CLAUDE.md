# ESP32 Sensor Station - Project Instructions

## Build & Upload

PlatformIO CLI path: `/Users/lorenzo.girardi/.platformio/penv/bin/pio`

```bash
# Build
/Users/lorenzo.girardi/.platformio/penv/bin/pio run

# Upload to ESP32
/Users/lorenzo.girardi/.platformio/penv/bin/pio run --target upload

# Clean build
/Users/lorenzo.girardi/.platformio/penv/bin/pio run --target clean
```

Serial monitor does not work from CLI (termios error). Use VS Code or:
```bash
screen /dev/cu.usbserial-* 115200
```

## Project Structure

- `src/main.cpp` - Main firmware (sensors, display, WiFi, InfluxDB, web dashboard)
- `src/config.h` - Credentials and network config (WiFi, InfluxDB, Telegram) - **contains secrets, never commit changes**
- `platformio.ini` - PlatformIO build config (ESP32, Arduino framework)
- `.pio/` - Build artifacts and library dependencies

## Hardware

- **Board**: ESP32 DevKit (esp32dev)
- **Sensors**: SHT30 (I2C 0x44), MQ-135 (ADC pin 34), PIR HC-SR501 (pin 27)
- **Display**: TFT 2.0" ST7789V (SPI)
- **Input**: TTP223 touch button (pin 26)
- **Backlight**: TFT_BLK pin 15

## Network Architecture

- WiFi DHCP with static route to InfluxDB subnet
- InfluxDB v1 HTTP API (line protocol, POST to /write)
- Local web dashboard on port 80 (embedded HTML)
- Optional Telegram alerts for PIR motion detection

## Key Design Decisions

- WiFi sleep disabled (`WiFi.setSleep(false)`) for connection stability
- Watchdog timer (30s) for auto-recovery from hangs
- Auto-reboot after 10 consecutive InfluxDB errors
- Progressive WiFi stack reset after 5 failed reconnection attempts
- `WiFi.persistent(false)` to avoid flash wear

## Workflow

Always build before uploading. After upload, check Grafana dashboard (ESP32 Stazione Smart Milano) to verify metrics are flowing.

## Language

Code comments and serial output are in Italian. Commit messages in English.
