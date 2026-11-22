#include "BootRecovery.h"

#include <Logging.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace BootRecovery {
namespace {

constexpr uint32_t BOOT_MAGIC = 0x35425243;
constexpr uint32_t stageBit(Stage stage) { return 1u << static_cast<uint8_t>(stage); }
constexpr uint32_t STORE_MASK = stageBit(Stage::Count) - stageBit(Stage::State);

RTC_NOINIT_ATTR uint32_t bootMagic;
RTC_NOINIT_ATTR uint32_t failedStores;
RTC_NOINIT_ATTR Stage currentStage;
bool manualRecovery = false;
uint32_t skipStores = 0;

bool resetInterruptedBoot() {
  const auto reason = esp_reset_reason();
  return reason == ESP_RST_PANIC || reason == ESP_RST_CPU_LOCKUP || reason == ESP_RST_WDT ||
         reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT;
}

}  // namespace

void begin(bool manualSafeBoot) {
  manualRecovery = manualSafeBoot;
  skipStores = 0;
  if (resetInterruptedBoot() && bootMagic == BOOT_MAGIC) {
    skipStores = failedStores & STORE_MASK;
    if (currentStage >= Stage::State && currentStage < Stage::Count) {
      skipStores |= stageBit(currentStage);
    }
  }
  bootMagic = BOOT_MAGIC;
  failedStores = skipStores;
  currentStage = Stage::None;
}

bool enterStore(Stage stage) {
  if (shouldSkip(stage)) {
    LOG_INF("BOOT", "Skipping startup store %u", static_cast<unsigned>(stage));
    return false;
  }
  currentStage = stage;
  return true;
}

void leaveStore() { currentStage = Stage::None; }

void complete() {
  currentStage = Stage::None;
  failedStores = 0;
  bootMagic = 0;
}

bool shouldSkip(Stage stage) { return manualRecovery || (skipStores & stageBit(stage)) != 0; }

bool active() { return manualRecovery || skipStores != 0; }

}  // namespace BootRecovery
