#include "ImageDecoderFactory.h"

#include <FsHelpers.h>
#include <Logging.h>

#include <cstring>
#include <new>
#include <string_view>

#include "GifToFramebufferConverter.h"
#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

namespace {

enum class ImageFormat { Unknown, Jpeg, Png, Gif };

ImageFormat formatByExtension(std::string_view path) {
  if (FsHelpers::hasJpgExtension(path)) return ImageFormat::Jpeg;
  if (FsHelpers::hasPngExtension(path)) return ImageFormat::Png;
  if (FsHelpers::hasGifExtension(path)) return ImageFormat::Gif;
  return ImageFormat::Unknown;
}

ImageFormat detectFormat(const std::string& path) {
  auto file = Storage.open(path.c_str());
  if (file.isOpen()) {
    constexpr uint8_t pngSignature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    uint8_t header[sizeof(pngSignature)];
    const int bytesRead = file.read(header, sizeof(header));
    if (bytesRead == sizeof(header) && std::memcmp(header, pngSignature, sizeof(header)) == 0) return ImageFormat::Png;
    if (bytesRead >= 3 && header[0] == 0xff && header[1] == 0xd8 && header[2] == 0xff) return ImageFormat::Jpeg;
  }
  return formatByExtension(path);
}

}  // namespace

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder;
std::unique_ptr<GifToFramebufferConverter> ImageDecoderFactory::gifDecoder;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  switch (detectFormat(imagePath)) {
    case ImageFormat::Jpeg:
      if (!jpegDecoder) jpegDecoder.reset(new (std::nothrow) JpegToFramebufferConverter());
      return jpegDecoder.get();
    case ImageFormat::Png:
      if (!pngDecoder) pngDecoder.reset(new (std::nothrow) PngToFramebufferConverter());
      return pngDecoder.get();
    case ImageFormat::Gif:
      if (!gifDecoder) gifDecoder.reset(new (std::nothrow) GifToFramebufferConverter());
      return gifDecoder.get();
    case ImageFormat::Unknown:
      LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
      return nullptr;
  }
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) {
  return formatByExtension(imagePath) != ImageFormat::Unknown;
}

bool ImageDecoderFactory::decoderNeedsMoreHeap(const std::string& imagePath) {
  if (PngToFramebufferConverter::decoderFitsHeap() && JpegToFramebufferConverter::decoderFitsHeap()) return false;
  switch (detectFormat(imagePath)) {
    case ImageFormat::Png:
      return !PngToFramebufferConverter::decoderFitsHeap();
    case ImageFormat::Jpeg:
      return !JpegToFramebufferConverter::decoderFitsHeap();
    case ImageFormat::Gif:
    case ImageFormat::Unknown:
      return false;
  }
  return false;
}
