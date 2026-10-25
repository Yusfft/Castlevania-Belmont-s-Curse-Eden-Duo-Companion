// SPDX-License-Identifier: GPL-3.0-or-later
// Texture decoding. See bc_tex.h.

#include "bc_tex.h"

#include <algorithm>
#include <cstring>

namespace bc_tex {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr std::size_t kMaxPixels = 4096u * 4096u;

u32 CeilDiv(u32 a, u32 b) {
    return (a + b - 1) / b;
}

void Rgb565(u16 c, u8* out) {
    const u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = static_cast<u8>((r * 527 + 23) >> 6);
    out[1] = static_cast<u8>((g * 259 + 33) >> 6);
    out[2] = static_cast<u8>((b * 527 + 23) >> 6);
}

/// BC1 colour block into a 4x4 RGBA tile. `alpha_mode` false is the BC3 colour part (always
/// four colours).
void DecodeBc1Block(const u8* s, u8 tile[16][4], bool bc1) {
    const u16 c0 = static_cast<u16>(s[0] | s[1] << 8), c1 = static_cast<u16>(s[2] | s[3] << 8);
    u8 pal[4][4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (!bc1 || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<u8>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<u8>((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<u8>((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    const u32 bits = static_cast<u32>(s[4]) | static_cast<u32>(s[5]) << 8 | static_cast<u32>(s[6]) << 16 |
                     static_cast<u32>(s[7]) << 24;
    for (int i = 0; i < 16; ++i)
        std::memcpy(tile[i], pal[(bits >> (2 * i)) & 3], 4);
}

/// BC4-style 8-bit channel block (BC3 alpha, BC4 red).
void DecodeChannelBlock(const u8* s, u8 out[16]) {
    const u32 a0 = s[0], a1 = s[1];
    u8 pal[8];
    pal[0] = static_cast<u8>(a0);
    pal[1] = static_cast<u8>(a1);
    if (a0 > a1) {
        for (u32 i = 1; i < 7; ++i)
            pal[i + 1] = static_cast<u8>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (u32 i = 1; i < 5; ++i)
            pal[i + 1] = static_cast<u8>(((5 - i) * a0 + i * a1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= static_cast<u64>(s[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i)
        out[i] = pal[(bits >> (3 * i)) & 7];
}

/// Switch block-linear layout to linear, in 16-byte units (UnityPy's TextureSwizzler).
bool Deswizzle(const u8* in, std::size_t size, u32 width, u32 height, u32 sw, u32 sh, u32 gobs, std::vector<u8>& out) {
    const u32 bcx = CeilDiv(width, sw), bcy = CeilDiv(height, sh);
    const u32 gcx = bcx / 4, gcy = bcy / 8;
    out.assign(size, 0);
    std::size_t src = 0;
    for (u32 i = 0; i < gcy / gobs; ++i)
        for (u32 j = 0; j < gcx; ++j)
            for (u32 k = 0; k < gobs; ++k) {
                const u32 base_y = (i * gobs + k) * 8, base_x = j * 4;
                for (u32 v = 0; v < 32; ++v) {
                    const u32 gx = ((v >> 3) & 2) | ((v >> 1) & 1);
                    const u32 gy = ((v >> 1) & 6) | (v & 1);
                    const u64 dst = (static_cast<u64>(base_y + gy) * bcx + base_x + gx) * 16;
                    if (src + 16 > size || dst + 16 > out.size())
                        return false;
                    std::memcpy(out.data() + dst, in + src, 16);
                    src += 16;
                }
            }
    return true;
}

} // namespace

std::optional<Format> FromTextureFormat(int format) {
    switch (format) {
    case 1:
        return Format{Codec::Alpha8, 1, 1, 1, 16, 1, false};
    case 4:
        return Format{Codec::RGBA8, 1, 1, 4, 4, 1, false};
    case 14:
        return Format{Codec::BGRA8, 1, 1, 4, 4, 1, false};
    case 63:
        return Format{Codec::R8, 1, 1, 1, 16, 1, false};
    case 10:
        return Format{Codec::BC1, 4, 4, 8, 8, 4, false};
    case 12:
        return Format{Codec::BC3, 4, 4, 16, 4, 4, false};
    case 26:
        return Format{Codec::BC4, 4, 4, 8, 8, 4, false};
    default:
        break;
    }
    if (format >= 48 && format <= 53) { // ASTC_4x4 .. ASTC_12x12
        static constexpr u32 kSizes[6]{4, 5, 6, 8, 10, 12};
        const u32 b = kSizes[format - 48];
        return Format{Codec::ASTC, b, b, 16, b, b, false};
    }
    return std::nullopt;
}

std::optional<Format> FromGraphicsFormat(int format) {
    switch (format) {
    case 4: // R8G8B8A8_SRGB
        return Format{Codec::RGBA8, 1, 1, 4, 4, 1, true};
    case 8: // R8G8B8A8_UNorm
        return Format{Codec::RGBA8, 1, 1, 4, 4, 1, false};
    case 96:
    case 97: // RGBA_DXT1
        return Format{Codec::BC1, 4, 4, 8, 8, 4, format == 96};
    case 100:
    case 101: // RGBA_DXT5
        return Format{Codec::BC3, 4, 4, 16, 4, 4, format == 100};
    default:
        break;
    }
    if (format >= 129 && format <= 140) { // RGBA_ASTC4X4_SRGB .. RGBA_ASTC12X12_UNorm
        static constexpr u32 kSizes[6]{4, 5, 6, 8, 10, 12};
        const int i = format - 129;
        const u32 b = kSizes[i / 2];
        return Format{Codec::ASTC, b, b, 16, b, b, (i % 2) == 0};
    }
    return std::nullopt;
}

std::size_t LinearSize(const Format& f, u32 w, u32 h) {
    return static_cast<std::size_t>(CeilDiv(w, f.bw)) * CeilDiv(h, f.bh) * f.bpb;
}

std::size_t SwizzledSize(const Format& f, u32 w, u32 h, u32 gob_shift) {
    if (gob_shift >= 16)
        return 0;
    const u32 gobs = 1u << gob_shift;
    const u32 pw = CeilDiv(w, f.sw * 4) * f.sw * 4;
    const u32 ph = CeilDiv(h, f.sh * 8 * gobs) * f.sh * 8 * gobs;
    return static_cast<std::size_t>(CeilDiv(pw, f.sw)) * CeilDiv(ph, f.sh) * 16;
}

int GobShiftFromPlatformBlob(const u8* blob, std::size_t size) {
    if (!blob || size < 12)
        return -1;
    const u32 shift = static_cast<u32>(blob[8]) | static_cast<u32>(blob[9]) << 8 | static_cast<u32>(blob[10]) << 16 |
                      static_cast<u32>(blob[11]) << 24;
    if (shift >= 16)
        return -1;
    return static_cast<int>(shift);
}

std::optional<Image> Decode(const Format& f, u32 w, u32 h, const u8* data, std::size_t size, int gob_shift,
                            const AstcFn& astc) {
    if (w == 0 || h == 0 || static_cast<u64>(w) * h > kMaxPixels || !data)
        return std::nullopt;
    // Layout dimensions: padded for the Switch layout, block-rounded otherwise.
    u32 dw = CeilDiv(w, f.bw) * f.bw, dh = CeilDiv(h, f.bh) * f.bh;
    std::vector<u8> linear;
    const u8* src = data;
    std::size_t src_size = size;
    if (gob_shift >= 0) {
        const u32 gobs = 1u << gob_shift;
        dw = CeilDiv(w, f.sw * 4) * f.sw * 4;
        dh = CeilDiv(h, f.sh * 8 * gobs) * f.sh * 8 * gobs;
        const std::size_t need = static_cast<std::size_t>(CeilDiv(dw, f.sw)) * CeilDiv(dh, f.sh) * 16;
        if (need > size || static_cast<u64>(dw) * dh > kMaxPixels)
            return std::nullopt;
        if (!Deswizzle(data, need, dw, dh, f.sw, f.sh, gobs, linear))
            return std::nullopt;
        src = linear.data();
        src_size = linear.size();
    }
    const u32 nbx = CeilDiv(dw, f.bw), nby = CeilDiv(dh, f.bh);
    if (static_cast<u64>(nbx) * nby * f.bpb > src_size)
        return std::nullopt;
    // Decode the full layout, then crop to w x h.
    std::vector<u8> full(static_cast<std::size_t>(nbx * f.bw) * nby * f.bh * 4, 0);
    const u32 fw = nbx * f.bw;
    switch (f.codec) {
    case Codec::RGBA8:
    case Codec::BGRA8:
        for (std::size_t p = 0; p < static_cast<std::size_t>(fw) * nby; ++p) {
            const u8* s = src + p * 4;
            u8* o = full.data() + p * 4;
            if (f.codec == Codec::RGBA8) {
                std::memcpy(o, s, 4);
            } else {
                o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = s[3];
            }
        }
        break;
    case Codec::Alpha8:
    case Codec::R8:
        for (std::size_t p = 0; p < static_cast<std::size_t>(fw) * nby; ++p) {
            u8* o = full.data() + p * 4;
            if (f.codec == Codec::Alpha8) {
                o[0] = o[1] = o[2] = 255;
                o[3] = src[p];
            } else {
                o[0] = src[p];
                o[3] = 255;
            }
        }
        break;
    case Codec::BC1:
    case Codec::BC3:
    case Codec::BC4:
        for (u32 by = 0; by < nby; ++by)
            for (u32 bx = 0; bx < nbx; ++bx) {
                const u8* s = src + (static_cast<std::size_t>(by) * nbx + bx) * f.bpb;
                u8 tile[16][4];
                if (f.codec == Codec::BC4) {
                    u8 r[16];
                    DecodeChannelBlock(s, r);
                    for (int i = 0; i < 16; ++i)
                        tile[i][0] = tile[i][1] = tile[i][2] = r[i], tile[i][3] = 255;
                } else if (f.codec == Codec::BC1) {
                    DecodeBc1Block(s, tile, true);
                } else {
                    u8 a[16];
                    DecodeChannelBlock(s, a);
                    DecodeBc1Block(s + 8, tile, false);
                    for (int i = 0; i < 16; ++i)
                        tile[i][3] = a[i];
                }
                for (int i = 0; i < 16; ++i)
                    std::memcpy(full.data() + ((static_cast<std::size_t>(by) * 4 + i / 4) * fw + bx * 4 + i % 4) * 4, tile[i], 4);
            }
        break;
    case Codec::ASTC: {
        if (!astc)
            return std::nullopt;
        // The host decodes at most 2048 x 2048 texels a call, and the block-padded layout can
        // be wider than the texture (2048 texels of 10x10 blocks are 205 blocks, 2050 texels):
        // decode in tiles of whole blocks.
        constexpr u32 kHostMax = 2048;
        const u32 tbx = std::max<u32>(1, kHostMax / f.bw), tby = std::max<u32>(1, kHostMax / f.bh);
        std::vector<u8> blocks, tile;
        for (u32 by0 = 0; by0 < nby; by0 += tby) {
            const u32 bny = std::min(tby, nby - by0);
            for (u32 bx0 = 0; bx0 < nbx; bx0 += tbx) {
                const u32 bnx = std::min(tbx, nbx - bx0);
                blocks.resize(static_cast<std::size_t>(bnx) * bny * 16);
                for (u32 r = 0; r < bny; ++r)
                    std::memcpy(blocks.data() + static_cast<std::size_t>(r) * bnx * 16,
                                src + (static_cast<std::size_t>(by0 + r) * nbx + bx0) * 16, static_cast<std::size_t>(bnx) * 16);
                const u32 tw = bnx * f.bw, th = bny * f.bh;
                tile.assign(static_cast<std::size_t>(tw) * th * 4, 0);
                if (!astc(blocks.data(), blocks.size(), tw, th, f.bw, f.bh, tile.data()))
                    return std::nullopt;
                for (u32 y = 0; y < th; ++y)
                    std::memcpy(full.data() + ((static_cast<std::size_t>(by0) * f.bh + y) * fw + static_cast<std::size_t>(bx0) * f.bw) * 4,
                                tile.data() + static_cast<std::size_t>(y) * tw * 4, static_cast<std::size_t>(tw) * 4);
            }
        }
        break;
    }
    }
    Image img;
    img.w = w;
    img.h = h;
    img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        std::memcpy(img.rgba.data() + static_cast<std::size_t>(y) * w * 4, full.data() + static_cast<std::size_t>(y) * fw * 4,
                    static_cast<std::size_t>(w) * 4);
    return img;
}

} // namespace bc_tex
