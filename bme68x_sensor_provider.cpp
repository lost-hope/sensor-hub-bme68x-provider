#include "wled.h"
#include "sensor_bus.h"
#include <Adafruit_BME680.h>

/*
 * BME68X (BME680/BME688) temperature + humidity + pressure + gas resistance
 * sensor provider.
 *
 * Reads a Bosch BME68X over I2C (address 0x76 or 0x77 - both are probed)
 * and pushes the readings into the Sensor Hub (see
 * ../sensor-hub/usermod_sensor_hub.cpp and ../sensor-hub/sensor_bus.h) as
 * "<prefix>_temperature", "<prefix>_humidity", "<prefix>_pressure" and
 * "<prefix>_gas_resistance". This usermod never talks to MQTT, the JSON
 * API or the Info tab itself - the hub takes care of all of that once a
 * sensor is registered here.
 *
 * This uses the plain Adafruit BME680 driver (raw gas resistance in Ohms),
 * not Bosch's BSEC library - so there is no calibrated IAQ/eCO2 output,
 * just the four raw readings. Wiring: SDA/SCL go to the I2C pins
 * configured on WLED's own Config > LED Preferences page (the shared
 * "i2c_sda"/"i2c_scl" globals). WLED core already calls Wire.begin() with
 * those pins while loading cfg.json at boot (wled00/cfg.cpp), before any
 * usermod's setup() runs - so this usermod only needs to confirm the pins
 * are set, then use the shared Wire bus. It must NOT call Wire.begin()
 * itself.
 */
class BME68XSensorUsermod : public Usermod {
  private:
    Adafruit_BME680 bme;
    SensorHub* hub = nullptr;
    uint8_t tempHandle = SENSOR_HANDLE_INVALID;
    uint8_t humidityHandle = SENSOR_HANDLE_INVALID;
    uint8_t pressureHandle = SENSOR_HANDLE_INVALID;
    uint8_t gasHandle = SENSOR_HANDLE_INVALID;

    bool enabled = true;
    bool sensorFound = false;
    bool initDone = false;

    unsigned long lastRead = 0;
    unsigned long lastBeginAttempt = 0;
    uint8_t consecutiveFailures = 0;

    // config
    uint16_t checkIntervalS = 30; // how often to read the sensor
    String namePrefix = "bme68x"; // sensor names become "<prefix>_temperature/_humidity/_pressure/_gas_resistance"
    uint8_t precision = 1;        // decimal places published for temperature/humidity/pressure
    uint8_t priority = 100;       // getValue() selection priority - lower wins among sensors of the same SensorType (see sensor_bus.h)

    static const char _name[];
    static const char _enabled[];
    static const char _checkInterval[];
    static const char _namePrefix[];
    static const char _precision[];
    static const char _priority[];

    bool beginSensor() {
      // BME68X breakout boards commonly strap the address to either 0x76 or 0x77.
      if (!(bme.begin(0x76, &Wire) || bme.begin(0x77, &Wire))) return false;
      bme.setTemperatureOversampling(BME680_OS_8X);
      bme.setHumidityOversampling(BME680_OS_2X);
      bme.setPressureOversampling(BME680_OS_4X);
      bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
      bme.setGasHeater(320, 150); // 320 degC for 150 ms, per Adafruit's example defaults
      return true;
    }

    void registerSensors() {
      if (!hub || tempHandle != SENSOR_HANDLE_INVALID) return; // already registered
      tempHandle     = hub->registerSensor((namePrefix + "_temperature").c_str(),  SensorType::Temperature, nullptr, nullptr, precision, priority);
      humidityHandle = hub->registerSensor((namePrefix + "_humidity").c_str(),     SensorType::Humidity,    nullptr, nullptr, precision, priority);
      pressureHandle = hub->registerSensor((namePrefix + "_pressure").c_str(),     SensorType::Pressure,    nullptr, nullptr, precision, priority);
      gasHandle      = hub->registerSensor((namePrefix + "_gas_resistance").c_str(), SensorType::Generic,    "ohm", nullptr, 0, priority); // no standard HA device_class for raw gas resistance
    }

    void setSensorsAvailable(bool available) {
      if (!hub) return;
      if (tempHandle != SENSOR_HANDLE_INVALID)     hub->setSensorAvailable(tempHandle, available);
      if (humidityHandle != SENSOR_HANDLE_INVALID) hub->setSensorAvailable(humidityHandle, available);
      if (pressureHandle != SENSOR_HANDLE_INVALID) hub->setSensorAvailable(pressureHandle, available);
      if (gasHandle != SENSOR_HANDLE_INVALID)      hub->setSensorAvailable(gasHandle, available);
    }

  public:
    void setup() override {
      // I2C bus is configured (and Wire.begin() already called) via WLED's
      // own Config > LED Preferences page - nothing to do here if it's unset.
      if (i2c_sda < 0 || i2c_scl < 0) { enabled = false; return; }
      sensorFound = beginSensor();
      initDone = true;
    }

    void loop() override {
      if (!enabled || !initDone) return;

      if (!hub) hub = getSensorHub(); // Sensor Hub usermod may finish init after us
      if (hub) registerSensors();

      unsigned long now = millis();

      if (!sensorFound) {
        // sensor missing at boot (or lost) - keep retrying rather than giving up forever
        if (now - lastBeginAttempt < 10000) return;
        lastBeginAttempt = now;
        sensorFound = beginSensor();
        if (!sensorFound) return;
      }

      if (now - lastRead < (unsigned long)checkIntervalS * 1000UL) return;
      lastRead = now;

      if (!bme.performReading()) {
        consecutiveFailures++;
        if (consecutiveFailures >= 3) setSensorsAvailable(false);
        if (consecutiveFailures >= 10) sensorFound = false; // force a fresh begin() next loop
        return;
      }

      consecutiveFailures = 0;
      setSensorsAvailable(true);
      if (hub) {
        if (tempHandle != SENSOR_HANDLE_INVALID)     hub->updateSensor(tempHandle, bme.temperature);
        if (humidityHandle != SENSOR_HANDLE_INVALID) hub->updateSensor(humidityHandle, bme.humidity);
        if (pressureHandle != SENSOR_HANDLE_INVALID) hub->updateSensor(pressureHandle, bme.pressure / 100.0f); // Pa -> hPa
        if (gasHandle != SENSOR_HANDLE_INVALID)      hub->updateSensor(gasHandle, (float)bme.gas_resistance);
      }
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top[FPSTR(_checkInterval)] = checkIntervalS;
      top[FPSTR(_namePrefix)] = namePrefix;
      top[FPSTR(_precision)] = precision;
      top[FPSTR(_priority)] = priority;
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
      configComplete &= getJsonValue(top[FPSTR(_checkInterval)], checkIntervalS);
      configComplete &= getJsonValue(top[FPSTR(_namePrefix)], namePrefix);
      configComplete &= getJsonValue(top[FPSTR(_precision)], precision);
      configComplete &= getJsonValue(top[FPSTR(_priority)], priority);
      return configComplete;
    }

    void appendConfigData(Print& settingsScript) override {
      settingsScript.print(F("addInfo('BME68XSensor:checkInterval',1,'seconds between sensor reads');"));
      settingsScript.print(F("addInfo('BME68XSensor:namePrefix',1,'sensor names become &lt;prefix&gt;_temperature/_humidity/_pressure/_gas_resistance - must be unique across all sensor providers');"));
      settingsScript.print(F("addInfo('BME68XSensor:precision',1,'decimal places published for temperature/humidity/pressure');"));
      settingsScript.print(F("addInfo('BME68XSensor:priority',1,'getValue() selection priority - lower wins if another provider also registers a Temperature/Humidity/Pressure sensor');"));
    }
};

const char BME68XSensorUsermod::_name[]          PROGMEM = "BME68XSensor";
const char BME68XSensorUsermod::_enabled[]       PROGMEM = "enabled";
const char BME68XSensorUsermod::_checkInterval[] PROGMEM = "checkInterval";
const char BME68XSensorUsermod::_namePrefix[]    PROGMEM = "namePrefix";
const char BME68XSensorUsermod::_precision[]     PROGMEM = "precision";
const char BME68XSensorUsermod::_priority[]      PROGMEM = "priority";

static BME68XSensorUsermod bme68x_sensor;
REGISTER_USERMOD(bme68x_sensor);
