#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <freertos/task.h>
#include <soc/soc_caps.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>

#include "HalGPIO.h"

#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO
#include <HalFrontlight.h>
#endif

#if FREEINK_DEVICE_PICCO || FREEINK_MCU_C3
#include <driver/gpio.h>
#endif

#if FREEINK_MCU_C3
#include <esp32-hal-i2c.h>
#include <esp_timer.h>
#include <soc/gpio_periph.h>
#include <soc/i2c_reg.h>
#include <soc/io_mux_reg.h>
#endif

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif

HalPowerManager powerManager;  // Singleton instance

// GPIO13 controls the X4 battery latch and the X3 SD power rail on the C3
// Xteink boards. Other boards use it for unrelated signals, including the
// X4 Pro display chip select.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  // Board charger setup (Picco SGM41562: stock charge voltage and limits); a
  // no-op on boards without a configurable charger.
  if (BatteryMonitor::configureCharger()) LOG_INF("PWR", "Charger configured");
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Held across the check and the switch: a Lock taken in between would find full speed, do
  // nothing, and then run at LOW_POWER_FREQ until the next key press.
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const LockMode mode = currentLockMode;
  if (normalSpeedDwell) {
    normalSpeedDwell = millis() - lastNormalSpeedPokeMs < NORMAL_POWER_DWELL_MS;
    enabled = enabled && !normalSpeedDwell;
  }

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      xSemaphoreGive(modeMutex);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      xSemaphoreGive(modeMutex);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
  xSemaphoreGive(modeMutex);
}

void HalPowerManager::pokeNormalSpeed() {
  if (gpio.deviceIsX3()) {
    xSemaphoreTake(modeMutex, portMAX_DELAY);
    lastNormalSpeedPokeMs = millis();
    normalSpeedDwell = true;
    xSemaphoreGive(modeMutex);
  }
  setPowerSaving(false);
}

#if FREEINK_MCU_C3
namespace {

constexpr uint8_t X3_I2C_BUS = 0;

struct LightSleepPin {
  gpio_num_t pin;
  gpio_int_type_t level;
};

template <size_t Count>
esp_sleep_wakeup_cause_t lightSleep(const LightSleepPin (&pins)[Count], uint64_t timeoutUs) {
  constexpr uint32_t WAKE_PAD_FLAGS = FUN_IE | SLP_SEL;
  uint32_t savedPadFlags[Count];
  size_t armed = 0;
  bool ready = true;
  for (; armed < Count; ++armed) {
    const auto& wake = pins[armed];
    savedPadFlags[armed] = REG_READ(GPIO_PIN_MUX_REG[wake.pin]) & WAKE_PAD_FLAGS;
    gpio_input_enable(wake.pin);
    if (gpio_wakeup_enable(wake.pin, wake.level) != ESP_OK) {
      ++armed;
      ready = false;
      break;
    }
  }

  esp_sleep_wakeup_cause_t cause = ESP_SLEEP_WAKEUP_UNDEFINED;
  if (ready && esp_sleep_enable_gpio_wakeup() == ESP_OK && esp_sleep_enable_timer_wakeup(timeoutUs) == ESP_OK &&
      !(REG_READ(I2C_SR_REG(X3_I2C_BUS)) & I2C_BUS_BUSY) && esp_light_sleep_start() == ESP_OK) {
    cause = esp_sleep_get_wakeup_cause();
  }
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  for (size_t i = 0; i < armed; ++i) {
    gpio_wakeup_disable(pins[i].pin);
    gpio_set_intr_type(pins[i].pin, GPIO_INTR_DISABLE);
    const auto mux = GPIO_PIN_MUX_REG[pins[i].pin];
    REG_WRITE(mux, (REG_READ(mux) & ~WAKE_PAD_FLAGS) | savedPadFlags[i]);
  }
  return cause;
}

}  // namespace

bool HalPowerManager::canLightSleep() const {
  return gpio.deviceIsX3() && WiFi.getMode() == WIFI_MODE_NULL && !gpio.isUsbSerialActive() &&
         !x3BatteryReadPending.load(std::memory_order_relaxed) && esp_timer_get_next_alarm() == INT64_MAX;
}

bool HalPowerManager::lightSleepBetweenTurns() const {
  static constexpr LightSleepPin pins[] = {
      {GPIO_NUM_1, GPIO_INTR_LOW_LEVEL}, {GPIO_NUM_2, GPIO_INTR_LOW_LEVEL}, {GPIO_NUM_3, GPIO_INTR_LOW_LEVEL}};
  return canLightSleep() && lightSleep(pins, 5000) != ESP_SLEEP_WAKEUP_UNDEFINED;
}

bool HalPowerManager::lightSleepWhileDisplayBusy(int8_t busyPin, uint8_t busyLevel) const {
  if (busyPin != EPD_BUSY || !canLightSleep()) return false;
  const LightSleepPin pins[] = {
      {static_cast<gpio_num_t>(busyPin), busyLevel ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL}};
  return lightSleep(pins, 5000) != ESP_SLEEP_WAKEUP_UNDEFINED;
}

bool HalPowerManager::lightSleepStandby() const {
  static constexpr LightSleepPin pins[] = {{GPIO_NUM_3, GPIO_INTR_LOW_LEVEL}};
  static constexpr uint64_t STANDBY_US = 3600ULL * 1000000;
  if (!canLightSleep()) return false;
  while (gpio.isPowerButtonPhysicallyPressed()) delay(5);
  if (!canLightSleep()) return false;
  const auto cause = lightSleep(pins, STANDBY_US);
  return cause == ESP_SLEEP_WAKEUP_GPIO || gpio.isPowerButtonPhysicallyPressed();
}
#endif

void HalPowerManager::startDeepSleep(HalGPIO& gpio, const bool wakeOnHeldButton) const {
#if FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO
  // The frontlight driver and touch share I2C; turn the light off before touch
  // releases the bus. On Picco this also puts the TMA525C to sleep.
  Frontlight.setOn(false);
  gpio.prepareForDeepSleep();
#endif
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // GPIO13 gates the battery MOSFET on both Xteink C3 boards; driving it low
    // is the battery power-off (the SDK wake source still handles USB power).
    // Release any surviving pad hold first: hold_en survives deep sleep via
    // the SDK's deepSleep() (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and a held pad silently ignores the drive.
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Hold every configured power-latch pin HIGH through deep sleep. These are
  // keep-alive enables (the X4 Pro's master peripheral rail on GPIO1, the
  // Sticky's PWR_HOLD/PWR_LOCK): deepSleep() isolates all pads
  // (esp_sleep_config_gpio_isolate), so a latch without an armed hold loses its
  // output driver and floats — on the X4 Pro the latch drops as soon as
  // external power leaves (serial/pogo adapter unplugged), and the next power-
  // button press cold-boots instead of fast-waking. holdPowerRails() asserted
  // the latches at boot but arms no sleep hold; arm it here instead. Skips
  // XTEINK_C3_GPIO13: it IS power.latch0 on the C3 Xteink boards, where the
  // block above drives it LOW on purpose (battery power-off).
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the
    // drive below (same trap as the GPIO13 block above).
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en(g);
  }

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

#if FREEINK_DEVICE_PAPERMONO
  // Its power button is behind the M5PM1 PMIC rather than an ESP GPIO, so
  // normal GPIO deep sleep would have no wake source. Ask the PMIC to shut the
  // device down; a button click then restarts it through a cold boot.
  if (freeink::m5pm1::requestShutdown()) {
    delay(1000);  // allow the PMIC firmware time to drop power
  }
#endif
#if FREEINK_DEVICE_METALIO_EINK4
  // Deep sleep leaves the board, 4G modem and audio module powered, so cut
  // power instead; the button cold-boots it. Returns only on USB power.
  freeink::metalio::powerOff();
#endif

  if (wakeOnHeldButton) {
    freeink::PowerManager::armPowerButtonWakeup();
    freeink::PowerManager::deepSleep();
  }
  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

#if FREEINK_DEVICE_PICCO
// ponytail: 30 s poll; the SGM41562 status is I2C-only, so USB unplug and
// charge progress are only seen when the timer fires.
static constexpr uint64_t USB_HOLD_POLL_US = 30ULL * 1000 * 1000;

bool HalPowerManager::holdWhileUsbPowered(HalGPIO& gpio, void (*onStatus)(bool charging, uint16_t percent)) const {
  if (!gpio.isUsbConnected()) return false;
  LOG_DBG("PWR", "USB power present, holding in light sleep");
  const auto powerPin = static_cast<gpio_num_t>(BoardConfig::ACTIVE.input.power);
  // The press that requested sleep may still be down; it must not end the hold.
  while (digitalRead(powerPin) == LOW) delay(10);  // Picco power button is active-low
  const BatteryMonitor battery;
  int lastCharging = -1;
  int lastPercent = -1;
  while (gpio.isUsbConnected()) {
    const bool charging = battery.isCharging();
    const uint16_t percent = getBatteryPercentage();
    if (static_cast<int>(charging) != lastCharging || percent != lastPercent) {
      lastCharging = charging;
      lastPercent = percent;
      onStatus(charging, percent);
    }
    esp_sleep_enable_timer_wakeup(USB_HOLD_POLL_US);
    gpio_wakeup_enable(powerPin, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    gpio_wakeup_disable(powerPin);
    // gpio_wakeup_disable() keeps the LOW_LEVEL type, and a later pinMode() would
    // enable it as an interrupt that storms while the button is held.
    gpio_set_intr_type(powerPin, GPIO_INTR_DISABLE);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (digitalRead(powerPin) == LOW) return true;
  }
  return false;
}
#endif

bool HalPowerManager::readBatteryPercentage(uint16_t& percent) const {
  static const BatteryMonitor battery;
  return battery.readPercentageChecked(percent);
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  if (gpio.deviceIsX3()) return getCachedBatteryPercent();
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

int HalPowerManager::getCachedBatteryPercent() const {
  if (!gpio.deviceIsX3()) return getBatteryPercentage();
  return std::max(0, x3BatteryPercent.load(std::memory_order_relaxed));
}

void HalPowerManager::beginBatteryCache() {
  if (!gpio.deviceIsX3()) return;
  pollBatteryCache();
  const auto readBatteryInBackground = [](void* context) {
    auto& power = *static_cast<HalPowerManager*>(context);
    for (;;) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      power.updateBatteryCache();
      power.x3BatteryReadPending.store(false, std::memory_order_relaxed);
    }
  };
  constexpr uint32_t BATTERY_TASK_STACK_BYTES = 3072;
  if (xTaskCreate(readBatteryInBackground, "BatteryCache", BATTERY_TASK_STACK_BYTES, this, tskIDLE_PRIORITY + 1,
                  &x3BatteryTask) != pdPASS) {
    LOG_ERR("PWR", "Could not start battery cache task");
  }
}

void HalPowerManager::pollBatteryCache() {
  if (!gpio.deviceIsX3()) return;
  constexpr uint32_t X3_BATTERY_POLL_MS = 30000;
  const uint32_t now = millis();
  if ((x3BatteryPolled && now - x3BatteryLastPollMs < X3_BATTERY_POLL_MS) ||
      x3BatteryReadPending.load(std::memory_order_relaxed)) {
    return;
  }
  x3BatteryPolled = true;
  x3BatteryLastPollMs = now;
  if (!x3BatteryTask) {
    updateBatteryCache();
    return;
  }
  x3BatteryReadPending.store(true, std::memory_order_relaxed);
  xTaskNotifyGive(x3BatteryTask);
}

void HalPowerManager::updateBatteryCache() {
#if FREEINK_MCU_C3
  constexpr uint32_t BATTERY_READ_TIMEOUT_MS = 10;
  constexpr int HYSTERESIS_PERCENT = 2;
  const uint8_t reg = BQ27220_SOC_REG;
  uint8_t reply[2];
  size_t received = 0;
  // Wire releases its lock before callers consume its shared receive buffer.
  if (!i2cIsInit(X3_I2C_BUS) ||
      i2cWriteReadNonStop(X3_I2C_BUS, I2C_ADDR_BQ27220, &reg, sizeof(reg), reply, sizeof(reply),
                          BATTERY_READ_TIMEOUT_MS, &received) != ESP_OK ||
      received != sizeof(reply)) {
    return;
  }
  const int percent = reply[0] | (reply[1] << 8);
  if (percent > 100) return;
  const int cached = x3BatteryPercent.load(std::memory_order_relaxed);
  if (cached < 0 || std::abs(percent - cached) >= HYSTERESIS_PERCENT) {
    x3BatteryPercent.store(percent, std::memory_order_relaxed);
  }
#endif
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
