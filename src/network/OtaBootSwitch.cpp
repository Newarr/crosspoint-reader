#include "OtaBootSwitch.h"

#include <HalOtaSlot.h>
#include <Logging.h>
#include <string.h>

namespace ota_boot {

bool switchTo(const esp_partition_t* dest) {
  if (!dest) return false;

  HalOtaSlot::BootSelection selection;
  if (!HalOtaSlot::readBootSelection(selection)) {
    LOG_ERR("BOOT", "Could not read otadata");
    return false;
  }

  const uint32_t destOtaIdx = dest->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
  if (dest->type != ESP_PARTITION_TYPE_APP || destOtaIdx >= selection.appCount) {
    LOG_ERR("BOOT", "dest is not an OTA app partition (subtype=0x%02X)", dest->subtype);
    return false;
  }

  uint64_t newSeq = selection.active.ota_seq + 1ULL;
  newSeq += (destOtaIdx + selection.appCount - (newSeq - 1) % selection.appCount) % selection.appCount;
  if (newSeq >= UINT32_MAX) {
    LOG_ERR("BOOT", "OTA sequence exhausted");
    return false;
  }

  esp_ota_select_entry_t next{};
  next.ota_seq = static_cast<uint32_t>(newSeq);
  memset(next.seq_label, 0xFF, sizeof(next.seq_label));
  next.ota_state = ESP_OTA_IMG_NEW;
  next.crc = HalOtaSlot::sequenceCrc(next.ota_seq);

  const int targetSlot = selection.activeIndex == 0 ? 1 : 0;
  const size_t targetOff = targetSlot * HalOtaSlot::ERASE_SIZE;

  if (esp_partition_erase_range(selection.partition, targetOff, HalOtaSlot::ERASE_SIZE) != ESP_OK) {
    LOG_ERR("BOOT", "otadata erase failed (slot=%d)", targetSlot);
    return false;
  }
  if (esp_partition_write(selection.partition, targetOff, &next, sizeof(next)) != ESP_OK) {
    LOG_ERR("BOOT", "otadata write failed (slot=%d)", targetSlot);
    return false;
  }

  LOG_INF("BOOT", "otadata: wrote slot=%d seq=%u crc=0x%08x -> %s", targetSlot, static_cast<unsigned>(newSeq),
          static_cast<unsigned>(next.crc), dest->label);
  return true;
}

}  // namespace ota_boot
