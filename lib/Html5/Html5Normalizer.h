#pragma once

#include <cstdint>
#include <string>

namespace html5 {

enum class NormalizeResult : uint8_t { Ok, Aborted, OutOfMemory, Error };

NormalizeResult normalizeHtmlForXml(const std::string& inputPath, const std::string& outputPath,
                                    bool (*shouldAbort)() = nullptr);

}  // namespace html5
