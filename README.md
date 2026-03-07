# ESP32 Sensor Station

ESP32-based environmental monitoring station with TFT display, web dashboard, InfluxDB logging, and optional Telegram alerts.

## Sensors

| Sensor | Type | Interface | Pin |
|--------|------|-----------|-----|
| SHT30 | Temperature + Humidity | I2C (0x44) | GPIO21 (SDA), GPIO22 (SCL) |
| MQ-135 | Air Quality (CO2, NH3, Benzene) | Analog | GPIO34 |
| PIR HC-SR501 | Motion Detection | Digital | GPIO27 |

## Other Components

| Component | Type | Interface | Pins |
|-----------|------|-----------|------|
| TFT ST7789V 2.0" | 240x320 Display | SPI | See below |
| TTP223 | Touch Button | Digital | GPIO26 |

## Wiring

### SHT30 (I2C)

```
SHT30           ESP32
─────           ─────
VCC  ──────────  3.3V
GND  ──────────  GND
SDA  ──────────  GPIO21
SCL  ──────────  GPIO22
```

### MQ-135 (Analog)

```
MQ-135          ESP32
──────          ─────
VCC  ──────────  5V (VIN)
GND  ──────────  GND
AOUT ──────────  GPIO34
```

**Note:** MQ-135 requires 24-48h of preheating for stable readings.

### PIR HC-SR501 (Digital)

```
PIR             ESP32
───             ─────
VCC  ──────────  5V (VIN)
GND  ──────────  GND
OUT  ──────────  GPIO27
```

**Note:** PIR needs 30-60s to stabilize after power-on (handled in firmware).

### TTP223 Touch Button (Digital)

```
TTP223          ESP32
──────          ─────
VCC  ──────────  3.3V
GND  ──────────  GND
OUT  ──────────  GPIO26
```

### TFT ST7789V 2.0" (SPI)

```
TFT             ESP32
───             ─────
VCC  ──────────  3.3V
GND  ──────────  GND
SCL  ──────────  GPIO18 (SPI CLK)
SDA  ──────────  GPIO23 (SPI MOSI)
RES  ──────────  GPIO4
DC   ──────────  GPIO2
CS   ──────────  GPIO5
BLK  ──────────  GPIO15
```

**Important:** GPIO5 (CS) and GPIO2 (DC) can interfere with ESP32 flash programming. Disconnect TFT from these pins before uploading firmware, then reconnect after upload.

### Complete Wiring Diagram

```
                                ┌─────────────────────┐
                                │       ESP32          │
                                │                      │
    ┌─────────┐                 │                      │
    │  SHT30  │                 │                      │
    │         │ VCC ────────────┤ 3.3V                 │
    │         │ GND ────────────┤ GND                  │
    │         │ SDA ────────────┤ GPIO21 (I2C SDA)     │
    │         │ SCL ────────────┤ GPIO22 (I2C SCL)     │
    └─────────┘                 │                      │
                                │                      │
    ┌─────────┐                 │                      │
    │ MQ-135  │                 │                      │
    │         │ VCC ────────────┤ 5V (VIN)             │
    │         │ GND ────────────┤ GND                  │
    │         │ AOUT ───────────┤ GPIO34 (ADC)         │
    └─────────┘                 │                      │
                                │                      │
    ┌─────────┐                 │                      │
    │   PIR   │                 │                      │
    │ HC-SR501│ VCC ────────────┤ 5V (VIN)             │
    │         │ GND ────────────┤ GND                  │
    │         │ OUT ────────────┤ GPIO27               │
    └─────────┘                 │                      │
                                │                      │
    ┌─────────┐                 │                      │
    │ TTP223  │                 │                      │
    │         │ VCC ────────────┤ 3.3V                 │
    │         │ GND ────────────┤ GND                  │
    │         │ OUT ────────────┤ GPIO26               │
    └─────────┘                 │                      │
                                │                      │
    ┌─────────────┐             │                      │
    │ TFT ST7789  │             │                      │
    │             │ VCC ────────┤ 3.3V                 │
    │             │ GND ────────┤ GND                  │
    │             │ SCL ────────┤ GPIO18 (SPI CLK)     │
    │             │ SDA ────────┤ GPIO23 (SPI MOSI)    │
    │             │ RES ────────┤ GPIO4                │
    │             │ DC  ────────┤ GPIO2                │
    │             │ CS  ────────┤ GPIO5                │
    │             │ BLK ────────┤ GPIO15               │
    └─────────────┘             │                      │
                                └──────────────────────┘
```

## Features

- **4 TFT display screens** (cycle with touch button):
  1. Temperature, humidity, dew point, heat index, comfort zone
  2. Air quality (MQ-135 ADC value + status bar)
  3. Security (PIR motion, alarm count, last event)
  4. Network info (WiFi, InfluxDB, uptime, RSSI)

- **Web dashboard** at `http://<esp32-ip>/` with real-time updates
- **REST API** at `http://<esp32-ip>/api/data` (JSON)
- **InfluxDB v1** logging every 60s:
  - `sensor_data`: temp, humidity, dewpoint, heatindex, air_quality, motion, alarm_count
  - `esp32_stats`: free_heap, min_free_heap, rssi, uptime, cpu_freq, flash/sketch sizes
- **Telegram alerts** on motion detection (optional, configurable cooldown)
- **DHCP WiFi** with configurable static route for cross-subnet InfluxDB access

## Setup

1. Clone this repo
2. Copy `src/config.h.example` to `src/config.h`
3. Edit `src/config.h` with your WiFi, InfluxDB, and Telegram credentials
4. Build and upload with PlatformIO:
   ```
   pio run --target upload
   ```

## Grafana Dashboard

Import `grafana-dashboard.json` into Grafana. It expects an InfluxDB v1 datasource with the measurements above.

## Derived Metrics

| Metric | Formula | Source |
|--------|---------|--------|
| Dew Point | Magnus formula | SHT30 temp + humidity |
| Heat Index | NOAA Rothfusz regression | SHT30 temp + humidity |
| Comfort Zone | Temp 20-26C + Humidity 30-60% | SHT30 temp + humidity |
