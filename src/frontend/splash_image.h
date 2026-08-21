#pragma once

namespace gba::frontend {

// Splash-screen icon, embedded as raw RGBA8888 pixels (row-major,
// top-to-bottom) - see splash_image.cpp for how it was generated.
constexpr int kSplashImageWidth = 96;
constexpr int kSplashImageHeight = 96;
extern const unsigned char kSplashImageRgba[kSplashImageWidth * kSplashImageHeight * 4];

} // namespace gba::frontend
