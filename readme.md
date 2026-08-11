# BME68X Sensor Provider

A [Sensor Hub](../sensor-hub/readme.md) provider usermod for the Bosch
BME680/BME688 - registers `bme68x_temperature`, `bme68x_humidity`,
`bme68x_pressure` and `bme68x_gas_resistance` with the hub by default,
which then handles MQTT, Home Assistant discovery, the JSON API and the
Info tab.

Uses the plain Adafruit BME680 driver, not Bosch's BSEC library - the gas
reading is raw sensor resistance in Ohms, not a calibrated IAQ/eCO2 value.

## Hardware

Wire SDA/SCL to the I2C pins configured on WLED's own **Config > LED
Preferences** page (shared across all I2C usermods). This usermod does not
call `Wire.begin()` itself. Both common breakout-board addresses (`0x76`
and `0x77`) are probed automatically. Retries `begin()` every 10s if the
sensor isn't found; after 3 consecutive failed reads all four sensors are
marked unavailable in Home Assistant, after 10 it re-attempts `begin()`.

## Usage

Self-contained out-of-tree usermod (see `library.json` for its
`adafruit/Adafruit BME680 Library` dependency). Add it to
`custom_usermods` next to the [Sensor Hub](../sensor-hub/readme.md) itself.

## Usermod Settings

| Setting | Default | Description |
|---|---|---|
| Enabled | on | Master on/off switch (also auto-disabled if I2C pins aren't configured) |
| Check interval | 30s | How often the sensor is read |
| Name prefix | `bme68x` | Sensor names become `<prefix>_temperature/_humidity/_pressure/_gas_resistance` - must be unique across every provider registered with the hub |
| Precision | 1 | Decimal places published for temperature/humidity/pressure |
