#pragma once

#include <ImageBakeQuantizer.h>

inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) { return imagebake::quantizeGray2bit(x, y, gray); }
