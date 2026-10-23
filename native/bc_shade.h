// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU shading of the level map, matching the look of the game's map tiles.
//
// Inputs per pixel: the tile's data texture (R = outside-room mask, G = distance gradient,
// B = room outline / edge field, A = water lines), the biome silhouette, the tileable parchment,
// a noise texture, the district state at this point (selected / completed) and the fog value
// (0 = revealed, larger = further into the unexplored area). All tunables are the values of the
// biome's map material, read from the player's romfs at runtime.
//
// The maths is a set of remapped smoothsteps: fog bands make the torn gold edges, the edge
// field makes the room outlines, district state picks the standard / completed / selected
// palette, and the silhouette darkens the parchment behind the castle.
#pragma once

#include <algorithm>
#include <cmath>

namespace bc_shade {

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
};

/// Material values (names follow the material's properties).
struct Params {
    Vec4 sdg_noise_st;         // noise tiling (xy) and scroll (zw) for the fog edge noise
    Vec4 sdg_noise_strength;   // per-channel weights of that noise
    float sdg_global_range = 0, sdg_global_contrast = 0;
    float main_line_thickness = 0, main_line_add_range = 0, main_line_contrast = 0;
    float main_line_noise_strength = 0, main_line_noise_contrast = 0;
    float second_line_thickness = 0, second_line_add_range = 0, second_line_contrast = 0;
    float second_line_noise_strength = 0, second_line_noise_contrast = 0;
    float cloud_range = 0, cloud_contrast = 0;
    float collision_range = 0, collision_contrast = 0;
    float alpha_range = 0, alpha_contrast = 0;
    float gradient_range = 0, gradient_contrast = 0;
    Vec4 gradient_color;
    Vec4 gradient_noise_mask;
    Vec4 gradient_perlin_st;
    float gradient_perlin_range = 0, gradient_perlin_contrast = 0;
    Vec4 line_color;
    Vec4 global_noise_st;
    Vec4 color_noise_mask;
    float move_noise_range = 0, move_noise_contrast = 0;
    float move_noise_back_strength = 0, move_noise_line_strength = 0;
    Vec4 standard_back, completed_back, selected_back;
    Vec4 standard_edge, completed_edge, selected_edge;
    float edge_min = 0, edge_max = 0;
    Vec4 selected_displace_st, selected_displace_strength;
    float selected_displace_range = 0, selected_displace_contrast = 0;
    float water_thickness = 0, water_contrast = 0;
    Vec4 water_color, water_noise_strength;
    float water_line_speed = 0, water_line_number = 0, water_line_strength = 0;
    float water_sdf_range = 0, water_sdf_contrast = 0;
    Vec4 background_st, background_noise_mask, background_tex_noise_strength;
    float is_discovered = 1;
    bool is_zmap = false;
};

/// Texture access. Coordinates are GL style: (0,0) is the first stored row.
struct Textures {
    virtual ~Textures() = default;
    virtual Vec4 Data(float u, float v) const = 0;       // the tile's data texture
    virtual Vec4 Silhouette(float u, float v) const = 0; // the tile's slice of the biome shape
    virtual Vec4 Parchment(float u, float v) const = 0;  // tileable background (repeat)
    virtual Vec4 Noise(float u, float v) const = 0;      // noise (repeat)
};

struct Pixel {
    float u = 0, v = 0;         // position in the tile, 0..1
    float wx = 0, wy = 0;       // world position (tiles)
    float fog = 0;              // fog value as stored by the game's fog pass (x10 below)
    float selected = 0;         // district state at this point
    float completed = 0;
    float time = 0;
};

namespace detail {
inline float Sat(float x) {
    return std::min(std::max(x, 0.0f), 1.0f);
}
inline float Smooth(float x) {
    return x * x * (3.0f - 2.0f * x);
}
inline float Mix(float a, float b, float t) {
    return a + (b - a) * t;
}
inline float Dot(const Vec4& a, const Vec4& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
inline Vec4 Add(const Vec4& a, const Vec4& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline Vec4 Scale(const Vec4& a, float s) {
    return {a.x * s, a.y * s, a.z * s, a.w * s};
}
inline Vec4 Lerp(const Vec4& a, const Vec4& b, float t) {
    return {Mix(a.x, b.x, t), Mix(a.y, b.y, t), Mix(a.z, b.z, t), Mix(a.w, b.w, t)};
}
inline Vec4 Minus(const Vec4& a, float s) {
    return {a.x - s, a.y - s, a.z - s, a.w - s};
}
/// Rising edge: 0 below `range`, 1 above `range + (1 - contrast)`.
inline float Rise(float x, float range, float contrast) {
    return Sat((x - range) / (1.0f - contrast));
}
/// Falling edge of the fog value around `level`: 1 when fog <= level, 0 when fog >= level +
/// (1 - contrast) (contrast is clamped like the game does).
inline float Below(float fog, float level, float contrast) {
    return Sat((level - fog) / (1.0f - std::min(contrast, 0.99f)));
}
} // namespace detail

namespace detail {
/// Shade() where the fog terms are constant: `revealed` 1 (inside) or 0 (unexplored); the
/// fog edge lines and the glow are 0 either way.
template <class Tex>
inline Vec4 ShadeOutsideEdge(const Params& p, const Tex& t, const Pixel& px, bool revealed_in) {
    const float T_ = px.time;
    const float u = px.u, v = px.v;
    const Vec4 data = t.Data(u, v);
    const float alpha = (1.0f - std::min(Smooth(Rise(data.y, std::max(p.alpha_range, 0.001f), p.alpha_contrast)), 1.0f)) *
                        (p.is_discovered >= 0.5f ? 1.0f : 0.0f);
    if (alpha <= 0.0f)
        return {0, 0, 0, 0};
    const float revealed = revealed_in ? 1.0f : 0.0f;
    const Vec4 gn = t.Noise(px.wx * p.global_noise_st.x + T_ * p.global_noise_st.z,
                            px.wy * p.global_noise_st.y + T_ * p.global_noise_st.w);
    const float color_noise = 1.0f - 0.25f * Dot(gn, p.color_noise_mask);
    const Vec4 sil = t.Silhouette(u, v);
    const float sil_k = p.is_zmap ? 1.0f : 1.0f - sil.x;
    const Vec4 bg = t.Parchment(px.wx * p.background_st.x, px.wy * p.background_st.y);
    const float bg_noise = Dot(gn, p.background_noise_mask);
    const float dark = std::min(1.0f - 0.25f * Dot(gn, p.background_tex_noise_strength), sil_k);
    const auto parchment = [&](float c) {
        const float n = c - 0.25f * bg_noise * c;
        return color_noise * (n - dark * n);
    };
    float r = parchment(bg.x), g = parchment(bg.y), b = parchment(bg.z);
    if (!revealed_in)
        return {r * (1.0f - dark) + 0.0f * dark, g * (1.0f - dark), b * (1.0f - dark), alpha};
    // Revealed: rooms, outlines and water as in Shade().
    const Vec4 e = t.Noise(px.wx * p.selected_displace_st.x + T_ * p.selected_displace_st.z,
                           px.wy * p.selected_displace_st.y + T_ * p.selected_displace_st.w);
    const float move = Smooth(Rise((e.x + e.y + e.z) / 3.0f, p.move_noise_range, p.move_noise_contrast));
    const float back_k = 1.0f + p.move_noise_back_strength * (move - 1.0f);
    const float line_k = 1.0f + p.move_noise_line_strength * (move - 1.0f);
    const float dc = 1.0f - p.selected_displace_contrast;
    const float dr = p.selected_displace_range * 0.5f;
    const float lo_a = dr - dc, lo_b = (1.0f - dr) - dc;
    const float wob = Smooth(Sat((e.x - lo_a) / (dr + dc - lo_a))) *
                      (1.0f - Smooth(Sat((e.x - lo_b) / (dc + (1.0f - dr) - lo_b))));
    const auto wobble = [&](float c, float k) { return (Mix(c, 0.5f, wob) - 0.5f) * k; };
    const float shift = 2.0f * (wobble(e.x, p.selected_displace_strength.x) + wobble(e.y, p.selected_displace_strength.y)) +
                        2.0f * wobble(e.z, p.selected_displace_strength.z);
    const Vec4 data_shift = t.Data(u + shift / 3.0f, v + shift / 3.0f);
    const float inside = 1.0f - data.x;
    const float edge = Smooth(Sat((data.z - p.edge_min) / (p.edge_max - p.edge_min)));
    const float edge_shift_t = Sat((data_shift.z - p.edge_min) / (p.edge_max - p.edge_min));
    const float edge_sel = std::max(edge, Smooth(edge_shift_t));
    const float sel = px.selected, done = px.completed;
    const auto room = [&](float std_back, float done_back, float sel_back, float std_edge, float done_edge,
                          float sel_edge) {
        const float sb = inside * std_back * back_k;
        const float standard = Mix(sb, line_k * std_edge, edge) * color_noise;
        const float db = back_k * (inside * done_back);
        const float completed = Mix(standard, color_noise * (Mix(db, line_k * done_edge, edge) * done), done);
        const float selb = back_k * (inside * Mix(sel_back, done_back, done));
        return Mix(completed, color_noise * (sel * Mix(selb, line_k * sel_edge, edge_sel)), sel);
    };
    const float rx = room(p.standard_back.x, p.completed_back.x, p.selected_back.x, p.standard_edge.x, p.completed_edge.x, p.selected_edge.x);
    const float ry = room(p.standard_back.y, p.completed_back.y, p.selected_back.y, p.standard_edge.y, p.completed_edge.y, p.selected_edge.y);
    const float rz = room(p.standard_back.z, p.completed_back.z, p.selected_back.z, p.standard_edge.z, p.completed_edge.z, p.selected_edge.z);
    r = Mix(r, rx, dark);
    g = Mix(g, ry, dark);
    b = Mix(b, rz, dark);
    const float phase = ((T_ * p.water_line_speed + px.wx) * 0.5f) * p.water_line_number;
    const float tri = 1.0f - std::fabs((phase - std::floor(phase)) * 2.0f - 1.0f);
    const float wave = (2.0f * Smooth(tri) - 1.5f) * p.water_line_strength;
    const Vec4 data_wave = t.Data(u, v + 2.0f * wave);
    const float water_body = Smooth(Rise(data_shift.z, p.water_sdf_range, p.water_sdf_contrast));
    const float water_lines = Smooth(Rise(data_wave.w, p.water_thickness, p.water_contrast));
    const float water = revealed * ((water_body - 0.25f * water_body * Dot(gn, p.water_noise_strength)) * water_lines);
    r = Mix(r, p.water_color.x, water);
    g = Mix(g, p.water_color.y, water);
    b = Mix(b, p.water_color.z, water);
    return {r, g, b, alpha};
}
} // namespace detail

/// Fog values (x10, i.e. world units from the revealed area) outside which the fog terms are
/// constant: at or beyond `far` nothing of the fog edge shows (unexplored), at or below `in`
/// the point is fully revealed. Shade then skips the samples those terms need; the result is
/// the same.
struct Limits {
    float far = 1e30f;
    float in = -1e30f;
};

inline Limits ComputeLimits(const Params& p) {
    const float s = std::fabs(p.sdg_noise_strength.x) + std::fabs(p.sdg_noise_strength.y) +
                    std::fabs(p.sdg_noise_strength.z) + std::fabs(p.sdg_noise_strength.w);
    const float g = p.sdg_global_range;
    const float main_half = 0.49f * p.main_line_contrast + p.main_line_thickness - 0.5f;
    const float sec_half = p.second_line_contrast * 0.49f + p.second_line_thickness - 0.5f;
    const float sa = p.second_line_add_range;
    Limits l;
    // Every fog term is a falling edge Below(fog, level, contrast): 0 at fog >= level, 1 at
    // fog <= level - (1 - min(contrast, 0.99)). The displacements are at most 1 and at least -s.
    float far = g + p.gradient_range + 1.0f;
    far = std::max(far, 1.0f + (p.main_line_add_range + main_half) + g + 1.0f);
    for (const float off : {sa * 4.0f, sa * 2.0f, sa})
        far = std::max(far, 1.0f + (off + sec_half) + g + 1.0f);
    far = std::max(far, 1.0f + g + 1.0f);
    l.far = far + 0.01f;
    const auto width = [](float c) { return 1.0f - std::min(c, 0.99f); };
    float in = -s + g + 1.0f - width(p.sdg_global_contrast);                                         // revealed = 1
    in = std::min(in, -s + (p.main_line_add_range - main_half) + g + 1.0f - width(p.main_line_contrast)); // main inner
    in = std::min(in, -s + (sa - sec_half) + g + 1.0f - width(p.second_line_contrast));              // l2 cut
    l.in = in - 0.01f;
    return l;
}

/// Shades one pixel of a map tile. Returns straight (non-premultiplied) RGBA in the colour
/// space of the material values. `T` provides Data / Silhouette / Parchment / Noise like
/// Textures does (a Textures works, so does any type with those members).
template <class Tex>
inline Vec4 Shade(const Params& p, const Tex& t, const Pixel& px, const Limits& lim = Limits{}) {
    using namespace detail;
    const float fog10 = px.fog * 10.0f;
    if (fog10 >= lim.far || fog10 <= lim.in)
        return ShadeOutsideEdge(p, t, px, fog10 <= lim.in);
    const float T = px.time;
    const float u = px.u, v = px.v;
    // ---- fog edge noise: three samples of the noise at two scales, blended in four layers.
    const Vec4& st = p.sdg_noise_st;
    const Vec4 n0 = t.Noise(u * st.x * 2.0f - T * st.z, v * st.y * 2.0f - T * st.w);
    const Vec4 n1 = t.Noise((u * 2.0f + 0.5f) * st.x - T * st.z, (v * 2.0f + 0.5f) * st.y - T * st.w);
    const Vec4 n2 = t.Noise(u * st.x + T * st.z, v * st.y + T * st.w);
    const Vec4 a = Scale(Add(n0, n1), 0.5f); // coarse layer
    const Vec4 b = Scale(Add(n0, n2), 0.5f); // fine layer
    const Vec4 l1 = Lerp(b, a, 0.75f);
    const Vec4 l2 = Lerp(b, a, 0.25f);
    const Vec4& s = p.sdg_noise_strength;
    const float disp_a = std::min(Dot(s, Minus(a, 0.5f)), 1.0f);
    const float disp_b = std::min(2.0f * Dot(s, Minus(b, 0.5f)), 1.0f);
    const float disp_l1 = std::min(1.5f * Dot(s, Minus(l1, 0.5f)), 1.0f);
    const float disp_l2 = std::min(2.0f * Dot(s, Minus(l2, 0.5f)), 1.0f);
    const auto avg3 = [](const Vec4& q) { return (q.x + q.y + q.z) / 3.0f; };

    const float fog = px.fog * 10.0f;
    const float g = p.sdg_global_range;
    // Main line: a band just outside the revealed area.
    const float main_half = 0.49f * p.main_line_contrast + p.main_line_thickness - 0.5f;
    const float main_in = Below(fog, disp_b + (p.main_line_add_range - main_half) + g + 1.0f, p.main_line_contrast);
    const float main_out = Below(fog, disp_b + (p.main_line_add_range + main_half) + g + 1.0f, p.main_line_contrast);
    const float main_inner = 1.0f - Smooth(main_in);
    const float main_gate = Smooth(Rise(avg3(b), p.main_line_noise_strength, p.main_line_noise_contrast));
    // Second lines: three more bands further out, each broken up by its own noise layer.
    const float sec_half = p.second_line_contrast * 0.49f + p.second_line_thickness - 0.5f;
    const float sc = p.second_line_contrast, sa = p.second_line_add_range;
    const auto band = [&](float disp, float offset) {
        const float outer = Smooth(Below(fog, disp + (offset + sec_half) + g + 1.0f, sc));
        const float inner = Smooth(Below(fog, disp + (offset - sec_half) + g + 1.0f, sc));
        return outer * (1.0f - inner);
    };
    const float gate_hi = p.second_line_noise_strength + 0.1f;
    const float sec_l2_cut = 1.0f - Smooth(Below(fog, disp_l2 + (sa - sec_half) + g + 1.0f, sc));
    const float sec_l2_band = Smooth(Below(fog, disp_l2 + (sa + sec_half) + g + 1.0f, sc));
    const float line_a = sec_l2_cut * band(disp_a, sa * 4.0f) *
                         Smooth(Rise(avg3(a), gate_hi, p.second_line_noise_contrast));
    const float line_l1 = band(disp_l1, sa * 2.0f) *
                          Smooth(Rise(avg3(l1), gate_hi, p.second_line_noise_contrast)) * sec_l2_cut;
    const float line_l2 = sec_l2_cut * sec_l2_band *
                          Smooth(Rise(avg3(l2), p.second_line_noise_strength, p.second_line_noise_contrast)) *
                          main_inner;
    const float line_main = main_gate * main_inner * Smooth(main_out);

    const Vec4 data = t.Data(u, v);
    // No lines where the room-distance gradient says we are far from any room.
    const float near_rooms = 1.0f - std::min(Smooth(Rise(data.y, p.cloud_range, p.cloud_contrast)), 1.0f);
    const float lines = std::max(std::max(line_l1, line_a), std::max(line_l2, line_main)) * near_rooms;

    // Revealed area and the soft gold gradient around it.
    const float revealed = Smooth(Below(fog, disp_b + g + 1.0f, p.sdg_global_contrast));
    const float glow = 0.25f * Smooth(Below(fog, g + p.gradient_range + 1.0f, p.gradient_contrast)) * (1.0f - revealed);
    const Vec4 perlin = t.Noise(u * p.gradient_perlin_st.x + T * p.gradient_perlin_st.z,
                                v * p.gradient_perlin_st.y + T * p.gradient_perlin_st.w);
    const float perlin_cut = 1.0f - std::min(Smooth(Rise(perlin.z, p.gradient_perlin_range, p.gradient_perlin_contrast)), 1.0f);
    const float perlin_w = Smooth(Sat(perlin.w * 6.66796875f));
    const float glow_lines = std::max(lines, near_rooms * glow * (n2.z * perlin_cut * perlin_w * perlin_w));
    const float glow_all = Sat(near_rooms * glow + glow_lines);

    // World-space noises.
    const Vec4 gn = t.Noise(px.wx * p.global_noise_st.x + T * p.global_noise_st.z,
                            px.wy * p.global_noise_st.y + T * p.global_noise_st.w);
    const float color_noise = 1.0f - 0.25f * Dot(gn, p.color_noise_mask);
    const float glow_noisy = glow_all - 0.25f * glow_all * Dot(gn, p.gradient_noise_mask);
    const Vec4 e = t.Noise(px.wx * p.selected_displace_st.x + T * p.selected_displace_st.z,
                           px.wy * p.selected_displace_st.y + T * p.selected_displace_st.w);
    const float move = Smooth(Rise(avg3(e), p.move_noise_range, p.move_noise_contrast));
    const float back_k = 1.0f + p.move_noise_back_strength * (move - 1.0f);
    const float line_k = 1.0f + p.move_noise_line_strength * (move - 1.0f);

    // Wobble of the selected outline.
    const float dc = 1.0f - p.selected_displace_contrast;
    const float dr = p.selected_displace_range * 0.5f;
    const float lo_a = dr - dc, lo_b = (1.0f - dr) - dc;
    const float wob = Smooth(Sat((e.x - lo_a) / (dr + dc - lo_a))) *
                      (1.0f - Smooth(Sat((e.x - lo_b) / (dc + (1.0f - dr) - lo_b))));
    const auto wobble = [&](float c, float k) { return (Mix(c, 0.5f, wob) - 0.5f) * k; };
    const float shift = 2.0f * (wobble(e.x, p.selected_displace_strength.x) + wobble(e.y, p.selected_displace_strength.y)) +
                        2.0f * wobble(e.z, p.selected_displace_strength.z);
    const Vec4 data_shift = t.Data(u + shift / 3.0f, v + shift / 3.0f);

    // Room colours.
    const float inside = 1.0f - data.x;
    const float edge = Smooth(Sat((data.z - p.edge_min) / (p.edge_max - p.edge_min)));
    const float edge_shift_t = Sat((data_shift.z - p.edge_min) / (p.edge_max - p.edge_min));
    const float edge_sel = std::max(edge, Smooth(edge_shift_t));
    const float sel = px.selected, done = px.completed;
    const auto room = [&](float std_back, float done_back, float sel_back, float std_edge, float done_edge,
                          float sel_edge) {
        const float sb = inside * std_back * back_k;
        const float standard = Mix(sb, line_k * std_edge, edge) * color_noise;
        const float db = back_k * (inside * done_back);
        const float completed = Mix(standard, color_noise * (Mix(db, line_k * done_edge, edge) * done), done);
        const float selb = back_k * (inside * Mix(sel_back, done_back, done));
        return Mix(completed, color_noise * (sel * Mix(selb, line_k * sel_edge, edge_sel)), sel);
    };
    const float rx = room(p.standard_back.x, p.completed_back.x, p.selected_back.x, p.standard_edge.x, p.completed_edge.x, p.selected_edge.x);
    const float ry = room(p.standard_back.y, p.completed_back.y, p.selected_back.y, p.standard_edge.y, p.completed_edge.y, p.selected_edge.y);
    const float rz = room(p.standard_back.z, p.completed_back.z, p.selected_back.z, p.standard_edge.z, p.completed_edge.z, p.selected_edge.z);

    // Parchment and silhouette.
    const Vec4 sil = t.Silhouette(u, v);
    const float sil_k = p.is_zmap ? 1.0f : 1.0f - sil.x;
    const Vec4 bg = t.Parchment(px.wx * p.background_st.x, px.wy * p.background_st.y);
    const float bg_noise = Dot(gn, p.background_noise_mask);
    const float dark = std::min(1.0f - 0.25f * Dot(gn, p.background_tex_noise_strength), sil_k);
    const auto parchment = [&](float c) {
        const float n = c - 0.25f * bg_noise * c;
        return color_noise * (n - dark * n);
    };
    const float collision = 1.0f - std::min(Smooth(Rise(data.y, p.collision_range, p.collision_contrast)), 1.0f);
    const float glow_k = std::max(collision, dark);
    const auto compose = [&](float room_c, float grad_c, float line_c, float bg_c) {
        const float lit = Mix(glow_noisy * grad_c, line_c, Sat(glow_lines));
        return 2.0f * (collision * (lit * glow_k)) + Mix(bg_c, revealed * room_c, dark);
    };
    float r = compose(rx, p.gradient_color.x, p.line_color.x, parchment(bg.x));
    float gch = compose(ry, p.gradient_color.y, p.line_color.y, parchment(bg.y));
    float bch = compose(rz, p.gradient_color.z, p.line_color.z, parchment(bg.z));

    // Water.
    const float phase = ((T * p.water_line_speed + px.wx) * 0.5f) * p.water_line_number;
    const float tri = 1.0f - std::fabs((phase - std::floor(phase)) * 2.0f - 1.0f);
    const float wave = (2.0f * Smooth(tri) - 1.5f) * p.water_line_strength;
    const Vec4 data_wave = t.Data(u, v + 2.0f * wave);
    const float water_body = Smooth(Rise(data_shift.z, p.water_sdf_range, p.water_sdf_contrast));
    const float water_lines = Smooth(Rise(data_wave.w, p.water_thickness, p.water_contrast));
    const float water = revealed * ((water_body - 0.25f * water_body * Dot(gn, p.water_noise_strength)) * water_lines);
    r = Mix(r, p.water_color.x, water);
    gch = Mix(gch, p.water_color.y, water);
    bch = Mix(bch, p.water_color.z, water);

    const float alpha = (1.0f - std::min(Smooth(Rise(data.y, std::max(p.alpha_range, 0.001f), p.alpha_contrast)), 1.0f)) *
                        (p.is_discovered >= 0.5f ? 1.0f : 0.0f);
    return {r, gch, bch, alpha};
}

} // namespace bc_shade
