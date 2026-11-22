#pragma once
#include <cstdint>

// The resolved text-rendering configuration a reader hands to the layout
// engine. Section-cache validation keys on every field: a section file built
// with a different spec is discarded and rebuilt.
//
// Build one via CrossPointSettings::readerRenderSpec(width, height), which
// fills every field: the settings-derived ones from the store, the viewport
// from the caller. Taking the viewport as arguments is what keeps a spec from
// existing in a half-filled state — the 0 defaults below are a last-resort
// backstop (a 0x0 viewport lays out nothing), not an invitation to omit it.
struct ReaderRenderSpec {
  int fontId = 0;
  float lineCompression = 1.0f;  // Multiplier of font size in pixels, independent of advanceY.
  bool extraParagraphSpacing = false;
  uint8_t paragraphIndentSpaces = 2;
  int8_t characterSpacing = 0;
  uint8_t wordSpacingPercent = 100;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
  bool collectTouchLinks = true;
};

inline bool operator==(const ReaderRenderSpec& a, const ReaderRenderSpec& b) {
  return a.fontId == b.fontId && a.lineCompression == b.lineCompression &&
         a.extraParagraphSpacing == b.extraParagraphSpacing && a.paragraphIndentSpaces == b.paragraphIndentSpaces &&
         a.characterSpacing == b.characterSpacing && a.wordSpacingPercent == b.wordSpacingPercent &&
         a.paragraphAlignment == b.paragraphAlignment && a.viewportWidth == b.viewportWidth &&
         a.viewportHeight == b.viewportHeight && a.hyphenationEnabled == b.hyphenationEnabled &&
         a.embeddedStyle == b.embeddedStyle && a.imageRendering == b.imageRendering &&
         a.focusReadingEnabled == b.focusReadingEnabled && a.collectTouchLinks == b.collectTouchLinks;
}

inline bool operator!=(const ReaderRenderSpec& a, const ReaderRenderSpec& b) { return !(a == b); }

namespace reader_render_spec {

constexpr uint32_t LAYOUT_SIGNATURE_VERSION = 3;
constexpr uint32_t LEGACY_LAYOUT_SIGNATURE_VERSION = 2;

inline uint32_t mix(uint32_t signature, uint32_t value) { return (signature ^ value) * 16777619U; }

inline uint32_t geometrySignature(const ReaderRenderSpec& spec, uint32_t version) {
  uint32_t signature = mix(2166136261U, version);
  const auto append = [&signature](const uint32_t value) { signature = mix(signature, value); };
  append(static_cast<uint32_t>(spec.fontId));
  append(static_cast<uint32_t>(spec.lineCompression * 1000.0f));
  append(spec.extraParagraphSpacing);
  append(static_cast<uint8_t>(spec.characterSpacing));
  append(spec.wordSpacingPercent);
  append(spec.paragraphAlignment);
  append(spec.viewportWidth);
  append(spec.viewportHeight);
  append(spec.hyphenationEnabled);
  append(spec.embeddedStyle);
  append(spec.imageRendering);
  append(spec.focusReadingEnabled);
  return signature;
}

}  // namespace reader_render_spec

inline uint32_t readerRenderSpecSignature(const ReaderRenderSpec& spec) {
  using namespace reader_render_spec;
  const auto signature = mix(geometrySignature(spec, LAYOUT_SIGNATURE_VERSION), spec.paragraphIndentSpaces);
  return signature == 0 ? 1 : signature;
}

inline uint32_t readerRenderSpecLegacySignature(const ReaderRenderSpec& spec) {
  using namespace reader_render_spec;
  const auto signature = geometrySignature(spec, LEGACY_LAYOUT_SIGNATURE_VERSION);
  return signature == 0 ? 1 : signature;
}

inline uint32_t readerRenderSpecLegacySignature(const ReaderRenderSpec& spec, bool collectTouchLinks) {
  using namespace reader_render_spec;
  const auto signature = mix(geometrySignature(spec, LEGACY_LAYOUT_SIGNATURE_VERSION), collectTouchLinks);
  return signature == 0 ? 1 : signature;
}
