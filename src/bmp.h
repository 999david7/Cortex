// Writes a 24-bit BMP from a block of 32-bit pixels. --screenshot uses it on
// macOS and Linux to save what the window showed.
#pragma once

#include <string>
#include <vector>

#include "sys.h"

namespace bmp {

// `pixels` is width*height 4-byte pixels, tightly packed. `bgra` says the
// bytes come in B,G,R,A order (Metal) rather than R,G,B,A (OpenGL), and
// `top_down` that the first row is the top of the image (Metal) rather than
// the bottom (OpenGL).
inline bool save(const std::string& path, int width, int height, const unsigned char* pixels,
                 bool bgra, bool top_down) {
    if (width <= 0 || height <= 0 || !pixels) return false;
    const int row = width * 3;
    const int stride = row + (4 - row % 4) % 4;
    const int image_bytes = stride * height;
    std::string blob(54 + (size_t)image_bytes, '\0');
    unsigned char* h = (unsigned char*)&blob[0];
    auto put32 = [h](int at, int v) {
        for (int k = 0; k < 4; ++k) h[at + k] = (unsigned char)(v >> (8 * k));
    };
    h[0] = 'B';
    h[1] = 'M';
    put32(2, 54 + image_bytes);
    h[10] = 54;
    h[14] = 40;
    put32(18, width);
    put32(22, height);
    h[26] = 1;
    h[28] = 24;
    put32(34, image_bytes);
    // BMP rows run bottom-up, in B,G,R order.
    for (int y = 0; y < height; ++y) {
        const int src_row = top_down ? height - 1 - y : y;
        const unsigned char* s = pixels + (size_t)src_row * width * 4;
        unsigned char* d = h + 54 + (size_t)y * stride;
        for (int x = 0; x < width; ++x) {
            d[x * 3 + 0] = bgra ? s[x * 4 + 0] : s[x * 4 + 2];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = bgra ? s[x * 4 + 2] : s[x * 4 + 0];
        }
    }
    std::string err;
    return sys::write_file(path, blob, err);
}

} // namespace bmp
