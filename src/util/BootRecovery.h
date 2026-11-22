#pragma once

#include <cstdint>

namespace BootRecovery {

enum class Stage : uint8_t { None, State, Settings, RecentBooks, KOReader, OPDS, OpdsTokens, LcpPassphrases, Count };

void begin(bool manualSafeBoot);
bool enterStore(Stage stage);
void leaveStore();
void complete();
bool shouldSkip(Stage stage);
bool active();

}  // namespace BootRecovery
