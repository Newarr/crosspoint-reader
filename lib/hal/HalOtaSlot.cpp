#include "HalOtaSlot.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>

namespace {
const esp_partition_t* asPartition(const void* partition) { return static_cast<const esp_partition_t*>(partition); }

struct OtaSelectEntry {
  uint32_t ota_seq;
  uint8_t seq_label[20];
  uint32_t ota_state;
  uint32_t crc;
};
static_assert(sizeof(OtaSelectEntry) == 32);

constexpr HalOtaSlot::RunningImageState classifyImageState(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_PENDING_VERIFY:
      return HalOtaSlot::RunningImageState::PendingVerify;
    case ESP_OTA_IMG_VALID:
      return HalOtaSlot::RunningImageState::Confirmed;
    case ESP_OTA_IMG_UNDEFINED:
      return HalOtaSlot::RunningImageState::Untracked;
    case ESP_OTA_IMG_NEW:
    case ESP_OTA_IMG_INVALID:
    case ESP_OTA_IMG_ABORTED:
      return HalOtaSlot::RunningImageState::Unsafe;
  }
  return HalOtaSlot::RunningImageState::Unsafe;
}

}  // namespace

#ifdef CONFIG_APP_ROLLBACK_ENABLE
extern "C" bool verifyRollbackLater() { return true; }
#endif

HalOtaSlot HalOtaSlot::inactive() {
  const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!partition || !running || partition->address == running->address || partition->type != ESP_PARTITION_TYPE_APP ||
      partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
      partition->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
    return {};
  }
  return HalOtaSlot(partition, partition->size);
}

HalOtaSlot::RunningImageState HalOtaSlot::runningImageState() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || esp_ota_get_boot_partition() != running) return RunningImageState::Unsafe;

  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  const size_t appCount = esp_ota_get_app_partition_count();
  if (!otadata || otadata->size < 2 * ERASE_SIZE || appCount == 0 ||
      running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN) {
    return RunningImageState::Unsafe;
  }

  // Classify the highest-sequence active otadata record directly because
  // esp_ota_get_state_partition() returns the first matching slot rather than the newest.
  OtaSelectEntry slots[2] = {};
  if (esp_partition_read(otadata, 0, &slots[0], sizeof(slots[0])) != ESP_OK ||
      esp_partition_read(otadata, ERASE_SIZE, &slots[1], sizeof(slots[1])) != ESP_OK) {
    return RunningImageState::Unsafe;
  }

  int activeIdx = -1;
  uint32_t activeSeq = 0;
  for (int i = 0; i < 2; ++i) {
    if (slots[i].ota_seq == UINT32_MAX || slots[i].ota_seq == 0) continue;
    const uint32_t crc =
        esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&slots[i].ota_seq), sizeof(slots[i].ota_seq));
    if (slots[i].crc != crc) continue;
    if (slots[i].ota_state == ESP_OTA_IMG_INVALID || slots[i].ota_state == ESP_OTA_IMG_ABORTED) continue;
    if (activeIdx < 0 || slots[i].ota_seq > activeSeq) {
      activeIdx = i;
      activeSeq = slots[i].ota_seq;
    }
  }
  if (activeIdx < 0) {
    return running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? RunningImageState::Untracked
                                                               : RunningImageState::Unsafe;
  }

  const size_t runningSlot = static_cast<size_t>(running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
  if (((activeSeq - 1u) % appCount) != runningSlot) return RunningImageState::Unsafe;
  return classifyImageState(static_cast<esp_ota_img_states_t>(slots[activeIdx].ota_state));
}

bool HalOtaSlot::confirmRunningImage() {
  if (runningImageState() != RunningImageState::PendingVerify) return false;
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

bool HalOtaSlot::safeForScratchWrite() const {
  if (!valid() || asPartition(partition_)->encrypted) return false;

  switch (runningImageState()) {
    case RunningImageState::Confirmed:
    case RunningImageState::Untracked:
      return true;
    case RunningImageState::PendingVerify:
    case RunningImageState::Unsafe:
      return false;
  }
  return false;
}

bool HalOtaSlot::read(size_t offset, void* data, size_t length) const {
  return contains(offset, length) && esp_partition_read(asPartition(partition_), offset, data, length) == ESP_OK;
}

bool HalOtaSlot::erase(size_t offset, size_t length) const {
  return contains(offset, length) && safeForScratchWrite() && offset % ERASE_SIZE == 0 && length % ERASE_SIZE == 0 &&
         esp_partition_erase_range(asPartition(partition_), offset, length) == ESP_OK;
}

bool HalOtaSlot::write(size_t offset, const void* data, size_t length) const {
  return contains(offset, length) && safeForScratchWrite() &&
         esp_partition_write(asPartition(partition_), offset, data, length) == ESP_OK;
}

bool HalOtaSlot::contains(size_t offset, size_t length) const {
  return valid() && offset <= size_ && length <= size_ - offset;
}
