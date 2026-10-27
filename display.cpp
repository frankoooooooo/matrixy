#include "display.h"

namespace matrixy {
namespace {
using opxy::i32;
using opxy::u32;
using namespace opxy::draw;
const i32 sine[65] = {
    0, 100, 200, 297, 392, 483, 569, 650, 724, 792, 851, 903, 946, 980, 1004, 1019,
    1024, 1019, 1004, 980, 946, 903, 851, 792, 724, 650, 569, 483, 392, 297, 200, 100,
    0, -100, -200, -297, -392, -483, -569, -650, -724, -792, -851, -903, -946, -980, -1004, -1019,
    -1024, -1019, -1004, -980, -946, -903, -851, -792, -724, -650, -569, -483, -392, -297, -200, -100, 0,
};
// exp(-u/4) in Q10 for u in half-row-squared steps: each hill's falloff.
const i32 falloff[64] = {
    1024, 904, 797, 704, 621, 548, 484, 427, 377, 332, 293, 259, 228, 202, 178, 157,
    139, 122, 108, 95, 84, 74, 65, 58, 51, 45, 40, 35, 31, 27, 24, 21,
    19, 17, 15, 13, 11, 10, 9, 8, 7, 6, 5, 5, 4, 4, 3, 3,
    3, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0,
};
const u32 knob_x[3] = {0, 4, 6};
const u32 knob_y[3] = {1, 5, 7};
// Header words. TILT's 20 ratios of the note match definition.json's labels.
const char* const destinations[9] = {"off", "out", "ptch", "fold", "cut", "res", "ring", "crsh", "rate"};
const char* const sources[9] = {"off", "sine", "saw", "sqr", "nois", "lfo", "env", "s+h", "bnce"};
const char* const ratios[20] = {"1:4", "1:3", "1:2", "2:3", "3:4", "1:1", "5:4", "4:3", "3:2", "5:3",
                                "2:1", "5:2", "3:1", "7:2", "4:1", "5:1", "6:1", "7:1", "8:1", "9:1"};
// A tilted sheet: a flat frame around 8 row wires and 8 column wires, pins on
// the crossings. Mesh coordinates are Q10 cells, 0..9; the frame is 0 and 9.
// Paths are drawn at scale 2 around the centre, within 2 x path_extent.
const i32 center_x = 240, center_y = 126, column_pitch = 28, row_pitch = 14, last = 9 * unit;
// Paths: 0-1 row wires, 2 column wires, 3 frame. Then pins and rings.
const u32 column_path = 2, frame_path = 3, dot = 4, aim = 7, ring = 10, y_ring = 11;
const u32 wire = 0, sounding = 1, holding = 2, placed = 3, ring_paint = 4, aim_paint = 5, frame_paint = 6, y_paint = 7;
const i32 hill_pixels = 18;

inline u32 choice(i32 q31) { return static_cast<u32>(q31 < 0 ? 0 : q31 >> 16) * 9 >> 15; }
inline u32 pin_of(opxy::u8 shown) { return shown >= 6 ? 2 : shown >= 4 ? 1 : 0; }
inline i32 ease(i32 value, i32 target, i32 k) { return value + ((target - value) * k >> 10); }
// phase in Q10 turns.
inline i32 sin_q10(i32 phase) {
    const u32 p = static_cast<u32>(phase) & 1023, i = p >> 4;
    return sine[i] + ((sine[i + 1] - sine[i]) * static_cast<i32>(p & 15) >> 4);
}
// Smoothstep over the outer 1.5 cells keeps the frame flat.
inline i32 ramp(i32 cells) {
    const i32 r = clamp(cells * 683 >> 10, 0, unit);  // two thirds
    return (r * r >> 10) * (3 * unit - 2 * r) >> 10;
}

// "<pin> <word>", or "<pin><word>" for 4-letter words: 6 characters run into the next title.
inline void label(char* out, const char* pin, const char* word) {
    u32 n = 0;
    out[n++] = pin[0];
    if (!word[3]) out[n++] = ' ';
    while (*word && n < header_capacity - 1) out[n++] = *word++;
    out[n] = 0;
}

struct Field { i32 strength[3]; i32 reach, time, swell; };

// Mesh coordinates to screen pixels (Q10): hills, an idle swell, then perspective.
inline void warp(const DisplayState& state, const Field& field, i32 u, i32 v, i32& x, i32& y) {
    i32 w = ramp(u);
    const i32 sides[3] = {ramp(last - u), ramp(v), ramp(last - v)};
    for (u32 i = 0; i < 3; ++i) w = sides[i] < w ? sides[i] : w;
    // Slow swell rolling across the sheet like cloth.
    i32 height = (sin_q10((u * 150 >> 10) + (field.time * 123 >> 10))
               * sin_q10((v * 120 >> 10) - (field.time * 92 >> 10)) >> 10) * field.swell >> 12;  // up to a quarter
    for (u32 p = 0; p < 3; ++p) {
        if (field.strength[p] <= 0) continue;
        const Attractor& a = state.pins[p];
        // Distances in row pitches, so hills are round on the sheet.
        const i32 nx = (u - a.x) * 2, ny = v - a.y;
        const i32 index = ((nx * nx + ny * ny) >> 10) * field.reach >> 19;
        if (index < 64) height += field.strength[p] * falloff[index] >> 10;
    }
    // Back rows recede. Divides by the 9-cell span are multiply-shifts: Blackfin
    // GCC turns every division, even by a power of two, into a library call.
    const i32 near = 768 + (v * 1820 >> 16);  // 768 + 256 v / last
    x = center_x * unit + ((u - last / 2) * column_pitch * near >> 10);
    y = (center_y - 63) * unit + (v * row_pitch * (819 + (v * 1458 >> 16)) >> 10)
      - (((height * w >> 10) * hill_pixels * near) >> 10);
}

// Spends a wire's points where it bends: samples gather toward each pin's
// line. Ends stay put; at most 0.9 total pull keeps the order monotonic.
inline i32 gather(const DisplayState& state, const Field& field, i32 s, bool along_rows) {
    if (s <= 0 || s >= last) return s;
    i32 shift = 0;
    for (u32 p = 0; p < 3; ++p) {
        if (field.strength[p] <= 0) continue;
        const i32 d = s - (along_rows ? state.pins[p].x : state.pins[p].y);
        const i32 index = (d * d >> 10) >> 9;
        if (index < 64) shift += (d * clamp(field.strength[p] * 3 >> 3, 0, 300) >> 10) * falloff[index] >> 10;
    }
    return clamp(s - shift, 0, last);
}

inline void put(Frame& frame, u32 node, const DisplayState& state, const Field& field,
                i32 u, i32 v, u32 shade, Paint paint) {
    i32 x, y;
    warp(state, field, u, v, x, y);
    place(frame, node, x, y, unit, shade, paint);
}
} // namespace

inline void display_reset(DisplayState& state) {
    for (u32 p = 0; p < 3; ++p) {
        Attractor& a = state.pins[p];
        a.x = a.y = a.strength = a.pop = a.still = 0;
        a.wx = a.wy = a.lx = a.ly = 0;
    }
    state.time = state.swell = 0;
    state.serial = state.track = state.initialized = 0;
}

// Only screen work. The settle rule mirrors the engine's, so a pin drops on
// screen when its route starts sounding.
inline void display(DisplayState& state, const opxy::draw::Input& input, Frame& frame) {
    const bool first = !state.initialized;
    const bool snap = first || state.track != input.track || input.reduced_motion;
    const i32 dt = snap ? 0 : clamp(input.elapsed, 0, 205);
    // Eases settle within about 0.15 s; reduced motion and track changes jump.
    const i32 k = snap ? unit : clamp(dt * 20, 0, unit);
    const u32 held = pin_of(input.shown[0]), held_y = pin_of(input.shown[1]);
    const u32 tilt = static_cast<u32>(clamp(input.slots[2], 0, 0x7fffffff) >> 16) * 20 >> 15;
    const i32 depth = (11468 + static_cast<i32>(tilt) * 1121) >> 5;  // the engine's depth, Q10
    // SNAP in Q10 seconds, mirroring the engine's settle time.
    const i32 settle = clamp(input.slots[3], 0, 0x7fffffff) >> 21;
    const bool note = input.note_serial != state.serial;
    // The cloth swells only while notes are held and rolls faster at higher
    // tilts; an idle screen is still.
    const bool holding_notes = input.held_notes != 0 && input.held_notes <= 128;
    state.swell = snap ? (holding_notes && !input.reduced_motion ? unit : 0)
                       : ease(state.swell, holding_notes ? unit : 0, clamp(dt * 6, 0, unit));
    if (state.swell) state.time = (state.time + (dt * (256 + static_cast<i32>(tilt) * 64) >> 10)) & 0xfffff;
    state.serial = input.note_serial;
    state.track = input.track;
    state.initialized = 1;

    Field field;
    field.reach = 640 - depth / 3;  // deeper tilts widen the hills
    field.time = state.time;
    field.swell = state.swell;
    for (u32 p = 0; p < 3; ++p) {
        Attractor& a = state.pins[p];
        const u32 wx = choice(input.slots[knob_x[p]]), wy = choice(input.slots[knob_y[p]]);
        const bool active = wx && wy;
        if (wx != a.wx || wy != a.wy) { a.wx = wx; a.wy = wy; a.still = 0; }
        else a.still = clamp(a.still + dt, 0, settle);
        // A new note plays the knobs as they are, as the engine does.
        if (snap || ((a.still >= settle || note) && (a.lx != wx || a.ly != wy))) {
            if (!snap && active) a.pop = unit;
            a.lx = wx; a.ly = wy;
        }
        if (active) {
            a.x = ease(a.x, static_cast<i32>(wx) * unit, k);
            a.y = ease(a.y, static_cast<i32>(wy) * unit, k);
        }
        if (note && active && !snap && a.pop < 400) a.pop = 400;
        a.pop = clamp(a.pop - dt * 6, 0, unit);
        const i32 target = !active ? 0 : (p == held ? 1024 : 560) * (512 + depth) >> 10;
        a.strength = ease(a.strength, target, k);
        field.strength[p] = a.strength * (unit + (a.pop >> 1)) >> 10;
    }

    for (u32 path = 0; path < Frame::path_count; ++path) {
        Frame::path_type& outline = frame.paths[path];
        bool changed = first;
        for (u32 i = 0; i < Frame::point_count; ++i) {
            // Wires snake so every turn and each closing edge lies on the frame.
            i32 u, v;
            if (path < column_path) {         // rows 1-4 or 5-8, 16 points each
                const u32 j = i / 16, t = i % 16;
                u = gather(state, field, static_cast<i32>(j & 1 ? 15 - t : t) * 39322 >> 6, true);  // t last / 15
                v = static_cast<i32>(1 + 4 * path + j) * unit;
            } else if (path == column_path) { // columns 1-8, 8 points each
                const u32 j = i / 8, t = i % 8;
                u = static_cast<i32>(1 + j) * unit;
                v = gather(state, field, static_cast<i32>(j & 1 ? 7 - t : t) * 84261 >> 6, false);  // t last / 7
            } else {                          // the frame, 16 points a side
                const i32 t = static_cast<i32>(i % 16) * (last >> 4);
                const u32 side = i / 16;
                u = side == 0 ? t : side == 1 ? last : side == 2 ? last - t : 0;
                v = side == 0 ? 0 : side == 1 ? t : side == 2 ? last : last - t;
            }
            i32 x, y;
            warp(state, field, u, v, x, y);
            const Point point = {clamp((x - center_x * unit) >> 1, -path_extent * unit, path_extent * unit),
                                 clamp((y - center_y * unit) >> 1, -path_extent * unit, path_extent * unit)};
            if (point.x != outline.points[i].x || point.y != outline.points[i].y) changed = true;
            outline.points[i] = point;
        }
        if (changed || !outline.key) outline.key = outline.key + 1 ? outline.key + 1 : 1;
        place(frame, path, center_x * unit, center_y * unit, 2 * unit, path == frame_path ? frame_paint : wire, stroke);
    }

    for (u32 p = 0; p < 3; ++p) {
        const Attractor& a = state.pins[p];
        const bool settled = a.lx == a.wx && a.ly == a.wy;
        if (a.lx && a.ly) put(frame, dot + p, state, field, static_cast<i32>(a.lx) * unit, static_cast<i32>(a.ly) * unit,
                              !settled ? sounding : p == held ? holding : placed, fill_and_stroke);
        else hide(frame, dot + p);
        if (!settled && a.wx && a.wy) put(frame, aim + p, state, field, static_cast<i32>(a.wx) * unit,
                                          static_cast<i32>(a.wy) * unit, aim_paint, stroke);
        else hide(frame, aim + p);
    }
    const Attractor& h = state.pins[held];
    if (h.wx && h.wy) put(frame, ring, state, field, h.x, h.y, ring_paint, stroke);
    else hide(frame, ring);
    // Encoders cycle pins independently; a grey ring marks Y's pin when it differs.
    const Attractor& hy = state.pins[held_y];
    if (held_y != held && hy.wx && hy.wy) put(frame, y_ring, state, field, hy.x, hy.y, y_paint, stroke);
    else hide(frame, y_ring);

    // Header values in real words: "2 cut", "2 saw", "3:2", "0.4s".
    char text[header_capacity];
    const char digit[2] = {static_cast<char>('1' + held), 0}, digit_y[2] = {static_cast<char>('1' + held_y), 0};
    label(text, digit, destinations[state.pins[held].wx]);
    header(frame, 0, text);
    label(text, digit_y, sources[state.pins[held_y].wy]);
    header(frame, 1, text);
    header(frame, 2, ratios[tilt]);
    const i32 tenths = (settle * 10 + 512) >> 10;
    const char seconds[5] = {static_cast<char>('0' + tenths / 10), '.', static_cast<char>('0' + tenths % 10), 's', 0};
    header(frame, 3, tenths ? seconds : "inst");
}
}
