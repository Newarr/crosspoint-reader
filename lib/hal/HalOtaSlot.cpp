#include "HalOtaSlot.h"

#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>

namespace {
const esp_partition_t* asPartition(const void* partition) { return static_cast<const esp_partition_t*>(partition); }

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

uint32_t HalOtaSlot::sequenceCrc(uint32_t sequence) {
  return esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&sequence), sizeof(sequence));
}

bool HalOtaSlot::readBootSelection(BootSelection& selection) {
  selection = {};
  selection.partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  selection.appCount = esp_ota_get_app_partition_count();
  if (!selection.partition || selection.partition->size < 2 * ERASE_SIZE || selection.appCount == 0) return false;

  for (int index = 0; index < 2; ++index) {
    esp_ota_select_entry_t entry{};
    if (esp_partition_read(selection.partition, index * ERASE_SIZE, &entry, sizeof(entry)) != ESP_OK) return false;
    if (entry.ota_seq == 0 || entry.ota_seq == UINT32_MAX || entry.crc != sequenceCrc(entry.ota_seq) ||
        entry.ota_state == ESP_OTA_IMG_INVALID || entry.ota_state == ESP_OTA_IMG_ABORTED) {
      continue;
    }
    if (entry.ota_seq > selection.active.ota_seq) {
      selection.activeIndex = index;
      selection.active = entry;
    }
  }
  return true;
}

HalOtaSlot::RunningImageState HalOtaSlot::runningImageState() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || esp_ota_get_boot_partition() != running || running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN) {
    return RunningImageState::Unsafe;
  }

  // The IDF state query can return an older record when both slots select the same app.
  BootSelection selection;
  if (!readBootSelection(selection)) return RunningImageState::Unsafe;
  if (selection.activeIndex < 0) {
    return running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? RunningImageState::Untracked
                                                               : RunningImageState::Unsafe;
  }

  const size_t runningSlot = running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
  if ((selection.active.ota_seq - 1u) % selection.appCount != runningSlot) return RunningImageState::Unsafe;
  return classifyImageState(static_cast<esp_ota_img_states_t>(selection.active.ota_state));
}

bool HalOtaSlot::confirmRunningImage() {
  if (runningImageState() != RunningImageState::PendingVerify) return false;
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

bool HalOtaSlot::safeForScratchWrite() const {
  const auto* partition = asPartition(partition_);
  uint8_t header[4];
  if (!contains(0, sizeof(header)) || partition->encrypted ||
      esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK || header[0] == ESP_IMAGE_HEADER_MAGIC) {
    return false;
  }

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
