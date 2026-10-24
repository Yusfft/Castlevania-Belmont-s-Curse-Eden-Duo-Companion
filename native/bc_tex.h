// SPDX-License-Identifier: GPL-3.0-or-later
//
// Texture decoding for the Switch build of the game: uncompressed RGBA / Alpha8, BC1/BC3/BC4
// (decoded here), ASTC (through the host's decoder) and the Switch block-linear layout that
// Texture2D data is stored in (Texture2DArray data in this game is linear).
//
// Images keep the stored row order: row 0 is the first row in the file, which Unity samples at
// v = 0 (the bottom of the picture).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace bc_tex {

struct Image {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    bool Empty() const { return w == 0 || h == 0; }
};

/// ASTC decoder: `in_size` bytes of blocks for a `w` x `h` image with `bw` x `bh` blocks into
/// tightly packed RGBA8 (`out` holds w*h*4 bytes).
using AstcFn = std::function<bool(const std::uint8_t* in, std::size_t in_size, std::uint32_t w,
                                  std::uint32_t h, std::uint32_t bw, std::uint32_t bh,
                                  std::uint8_t* out)>;

enum class Codec : std::uint8_t { RGBA8, BGRA8, Alpha8, R8, BC1, BC3, BC4, ASTC };

struct Format {
    Codec codec = Codec::RGBA8;
    std::uint32_t bw = 1, bh = 1, bpb = 4; // block size in texels, bytes per block
    std::uint32_t sw = 4, sh = 1;          // texels per 16-byte swizzle unit (Switch layout)
    bool srgb = false;
};

/// Unity TextureFormat (Texture2D.m_TextureFormat).
std::optional<Format> FromTextureFormat(int format);
/// Unity GraphicsFormat (Texture2DArray.m_Format).
std::optional<Format> FromGraphicsFormat(int format);

/// Bytes of one image (mip 0, one slice) in the plain linear layout.
std::size_t LinearSize(const Format& f, std::uint32_t w, std::uint32_t h);
/// Bytes of mip 0 in the Switch block-linear layout with 2^`gob_shift` GOBs per block.
std::size_t SwizzledSize(const Format& f, std::uint32_t w, std::uint32_t h, std::uint32_t gob_shift);

/// Decodes mip 0. `gob_shift` < 0 means linear data; otherwise the Switch layout.
std::optional<Image> Decode(const Format& f, std::uint32_t w, std::uint32_t h, const std::uint8_t* data,
                            std::size_t size, int gob_shift, const AstcFn& astc);

/// The Switch GOB shift stored in Texture2D.m_PlatformBlob, or -1 when the data is linear.
int GobShiftFromPlatformBlob(const std::uint8_t* blob, std::size_t size);

} // namespace bc_tex
