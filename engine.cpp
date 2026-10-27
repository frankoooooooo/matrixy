#include "core.h"
#include <opxy/q31.hpp>

namespace matrixy {
namespace {
// Rows (sources) and columns (destinations), in enum order after "off".
enum Source { sine, saw, sqr, noise, lfo, env, hold, bnce };
enum Destination { out, ptch, fold, cut, res, ring, crsh, rate };
const i32 one = 32767;
// SNAP: pins reroute only after the knobs hold still this long (0..1 s, 690
// blocks), so sweeping a knob is silent.
const i32 snap_blocks = 690;
// TILT: 20 ratios of the note, 1:4 .. 9:1, in Q12 and as Q12 octaves.
const u32 tilts = 20;
const i32 tilt_ratio[tilts] = {1024, 1365, 2048, 2731, 3072, 4096, 5120, 5461, 6144, 6827, 8192, 10240, 12288, 14336, 16384, 20480, 24576, 28672, 32768, 36864};
const i32 tilt_octaves[tilts] = {-8192, -6492, -4096, -2396, -1700, 0, 1319, 1700, 2396, 3019, 4096, 5415, 6492, 7403, 8192, 9511, 10588, 11499, 12288, 12984};
const i32 fade_step = 512;  // ~90 ms crossfade between routes
const u32 knob_x[3] = {0, 4, 6};
const u32 knob_y[3] = {1, 5, 7};
const u32 clip = 0x7fff0000u;  // highest phase step kept
// 2^(k/16) in Q16.
const i32 octave_table[17] = {
    65536, 68438, 71468, 74632, 77936, 81386, 84990, 88752, 92682,
    96785, 101070, 105545, 110218, 115098, 120194, 125515, 131072,
};

inline i32 clamp(i32 v, i32 lo, i32 hi) { return v < lo ? lo : v > hi ? hi : v; }
// Any signal times a Q15 coefficient (|c| <= 32767): one Blackfin instruction.
inline i32 mul(i32 value, i32 coefficient) { return opxy::q31::multiply(value, coefficient * 65536); }
// A plain 32-bit Q15 product, for operands whose product is known to stay
// under 2^31 (each within about 2.0 and 1.0). Cheapest on every target.
inline i32 fast(i32 a, i32 b) { return a * b >> 15; }
inline i32 unit(i32 q31) { return q31 < 0 ? 0 : q31 >> 16; }
// Same quantization as the stock enum path: Q15 times the label count.
inline u32 choice(i32 q31) { return static_cast<u32>(unit(q31)) * 9 >> 15; }
inline bool audio(u32 source) { return source <= noise; }

inline i32 random(u32& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return static_cast<i32>(s) >> 16;
}
// 2^octaves in Q16, octaves in Q12, within -12..+14.
inline i32 exp2_q16(i32 octaves) {
    const i32 o = clamp(octaves, -12 * 4096, 14 * 4096 - 1);
    const i32 whole = o >> 12, i = (o & 4095) >> 8, sub = o & 255;
    const i32 v = octave_table[i] + ((octave_table[i + 1] - octave_table[i]) * sub >> 8);
    return whole >= 0 ? v << whole : v >> -whole;
}
// Index of the highest set bit (v > 0). The SDK links no libgcc, so no
// __builtin_clz: five halving steps instead.
inline i32 top_bit(u32 v) {
    i32 n = 0;
    if (v >> 16) { v >>= 16; n += 16; }
    if (v >> 8) { v >>= 8; n += 8; }
    if (v >> 4) { v >>= 4; n += 4; }
    if (v >> 2) { v >>= 2; n += 2; }
    return n + static_cast<i32>(v >> 1);
}
// log2 in Q12 by Mitchell's approximation (under 0.09 octave error).
inline i32 log2_q12(u32 v) {
    if (!v) return 0;
    const i32 msb = top_bit(v);
    const u32 mantissa = msb >= 12 ? v >> (msb - 12) : v << (12 - msb);
    return msb * 4096 + static_cast<i32>(mantissa) - 4096;
}
// part / whole in Q15 for part < whole, with a 32-bit divide.
inline i32 ratio(u32 part, u32 whole) {
    const i32 shift = top_bit(whole) - 15;
    if (shift > 0) { part >>= shift; whole >>= shift; }
    return static_cast<i32>((part << 15) / whole);
}
// Two parabola passes: a sine within 0.1%.
inline i32 sine_of(u32 phase) {
    const i32 s = static_cast<i32>(phase) >> 16;
    const i32 y = (s * (32768 - (s < 0 ? -s : s))) >> 13;
    return clamp(y + fast((y * (y < 0 ? -y : y) >> 15) - y, 7373), -one, one);
}
inline i32 triangle(u32 phase) {
    const i32 s = static_cast<i32>(phase) >> 16;
    return (s < 0 ? -s : s) * 2 - 32768;
}
// PolyBLEP residual for a unit step at phase 0; the divide only runs on the
// sample either side of the edge.
inline i32 blep(u32 phase, u32 step) {
    if (phase < step) {
        const i32 x = ratio(phase, step);
        return 2 * x - fast(x, x) - one;
    }
    const u32 before = 0u - phase;
    if (before < step) {
        const i32 x = ratio(before, step);
        return fast(x, x) - 2 * x + one;
    }
    return 0;
}
// x^3 in Q15 for |x| < 4, kept inside 32 bits.
inline i32 cube(i32 x) {
    const i32 h = x >> 2;
    return (h * h >> 15) * h >> 9;
}
// Limit to +-2 for the resonant filter: untouched within +-1, then a knee of
// half slope, so it sings instead of exploding at almost no cost per pass.
inline i32 saturate(i32 v) {
    if (v > one) return v < 3 * one ? one + ((v - one) >> 1) : 2 * one;
    if (v < -one) return v > -3 * one ? -one + ((v + one) >> 1) : -2 * one;
    return v;
}
// Soft clip to +-1 (x - 4/27 x^3 on +-1.5).
inline i32 soft_clip(i32 v) {
    const i32 x = clamp(v, -49151, 49151);
    return x - fast(cube(x), 4855);
}
// Unipolar view of a source, for the output VCA.
inline i32 positive(u32 source, i32 value) {
    return source == lfo || source == hold || audio(source) ? (value + 32768) >> 1 : value;
}

// A pin's route as heard this block: either its current or its fading route.
// Modulators ramp from `from` by `delta` across the block.
struct Route { u32 s, d; i32 gain, amount, from, delta; };
// One column's modulation for a block: a ramp from all its modulators plus
// up to six audio rows.
struct Lane { i32 from, delta; u32 count; u32 s[6]; i32 amount[6]; };
inline i32 lane_at(const Lane& lane, const Voice& voice, u32 i) {
    i32 m = lane.from + (lane.delta * static_cast<i32>(i + 1) >> 6);
    for (u32 k = 0; k < lane.count; ++k) m += fast(voice.bus[lane.s[k]][i], lane.amount[k]);
    return m;
}
} // namespace

inline void start(Voice& voice, Shared&, u32);

// Runs whenever a voice goes to a new owner, so it sets up all sound state.
inline void initialize(Voice& voice, Shared& shared, u32 owner) {
    voice.noise = owner * 2654435761u | 1u;
    voice.lfo = voice.hold = 0;
    voice.pitch_scale = voice.previous_pitch_scale = 65536;
    for (u32 s = 0; s < 8; ++s) voice.src[s] = 0;
    start(voice, shared, owner);
}

inline void start(Voice& voice, Shared&, u32) {
    voice.sine = voice.sub = voice.lfo = 0;
    voice.saw = 0x80000000u;  // starts mid-ramp, at zero
    voice.env = voice.bounce_height = one;
    voice.bounce = voice.bounce_gap = voice.bounce_wait = 0;
    voice.low = voice.band = voice.dc_in = voice.dc_out = voice.last_sine = 0;
    voice.crushed = 0;
    voice.crush_phase = 65536;
    voice.src[hold] = voice.hold_target = random(voice.noise);
    // Every note starts from the same readings, not the last note's.
    voice.src[sine] = 0;
    voice.src[lfo] = triangle(0);
    voice.src[env] = one;
    voice.src[bnce] = 0;
    // New notes play the knobs as they are now.
    voice.fresh = 1;
}

inline void destroy(Voice&, Shared&, u32) {}

inline void render(Voice& voice, Shared&, u32, opxy::Output output,
                   opxy::Parameters parameters, opxy::Pitch pitch) {
    const bool fresh = voice.fresh;
    voice.fresh = 0;
    const i32 settle = unit(parameters[3]) * snap_blocks >> 15;
    const u32 tilt = static_cast<u32>(unit(parameters[2])) * tilts >> 15;
    for (u32 p = 0; p < 3; ++p) {
        Pin& pin = voice.pins[p];
        const u32 x = choice(parameters[knob_x[p]]), y = choice(parameters[knob_y[p]]);
        if (fresh) {
            pin.x = pin.wx = x; pin.y = pin.wy = y; pin.px = pin.py = 0;
            pin.still = settle; pin.fade = one;
            continue;
        }
        if (x != pin.wx || y != pin.wy) { pin.wx = x; pin.wy = y; pin.still = 0; }
        else if (pin.still < settle) ++pin.still;
        if (pin.fade < one) {
            pin.fade = pin.fade + fade_step > one ? one : pin.fade + fade_step;
        } else if (pin.still >= settle && (pin.x != pin.wx || pin.y != pin.wy)) {
            pin.px = pin.x; pin.py = pin.y; pin.x = pin.wx; pin.y = pin.wy; pin.fade = 0;
        }
    }
    // Low tilts are gentle, high tilts deep: depth 0.35 .. 1.
    const i32 depth = 11468 + static_cast<i32>(tilt) * 1121;  // tops out at exactly 32767
    if (fresh) voice.depth = depth;
    voice.depth += (depth - voice.depth) >> 3;

    // Up to six routes: each pin's current one plus any it is fading out of.
    Route routes[6];
    u32 count = 0;
    for (u32 p = 0; p < 3; ++p) {
        const Pin& pin = voice.pins[p];
        if (pin.x && pin.y)
            routes[count++] = Route{pin.y - 1, pin.x - 1, pin.fade, mul(pin.fade, voice.depth), 0, 0};
        if (pin.fade < one && pin.px && pin.py)
            routes[count++] = Route{pin.py - 1, pin.px - 1, one - pin.fade, mul(one - pin.fade, voice.depth), 0, 0};
    }
    // weight: how engaged each column is. voiced: audio already on OUT.
    // used: which audio rows anything listens to, so idle ones cost nothing.
    i32 weight[8] = {0, 0, 0, 0, 0, 0, 0, 0}, voiced = 0, rate_mod = 0, pitch_mod = 0;
    bool used[4] = {false, false, false, false};
    for (u32 r = 0; r < count; ++r) {
        const Route& route = routes[r];
        weight[route.d] += route.amount;
        if (audio(route.s)) used[route.s] = true;
        if (route.d == out && audio(route.s)) voiced += route.gain;
        if (route.d == rate) rate_mod += mul(voice.src[route.s], route.amount);
        if (route.d == ptch && !audio(route.s)) pitch_mod += mul(voice.src[route.s], route.amount);
    }

    // Modulators tick once per block and ramp across it; RATE bends their clocks.
    for (u32 s = lfo; s <= bnce; ++s) voice.previous[s - lfo] = voice.src[s];
    // Modulators run at the note's frequency x TILT / 128, so a wobble stays
    // locked to the pitch across the keyboard. Worked in octaves (no 64-bit
    // product): per-block increment = pitch step x 64 x ratio / 128.
    // Stacked RATE pins still bend at most +-3 octaves, so the clock never
    // stalls the bouncing ball.
    rate_mod = clamp(rate_mod, -one, one);
    const i32 clock = exp2_q16(rate_mod * 3 >> 3);  // Q16, +-3 octaves
    const u32 increment = static_cast<u32>(exp2_q16(
        log2_q12(pitch[0]) + tilt_octaves[tilt] + (rate_mod * 3 >> 3) - 17 * 4096));
    voice.lfo += increment;
    voice.src[lfo] = triangle(voice.lfo);
    voice.env -= mul(voice.env, clamp(static_cast<i32>(increment >> 15), 1, 16384));
    voice.src[env] = voice.env;
    const u32 before = voice.hold;
    voice.hold += increment * 4;
    if (voice.hold < before) voice.hold_target = random(voice.noise);
    // Sample and hold, with a ~6 ms glide so steps never click.
    voice.src[hold] += (voice.hold_target - voice.src[hold]) >> 2;
    // A dropped ball: restitution 0.8 shortens each gap by 0.8 and its height by
    // 0.64; each hit rings for about a third of the next gap. Times in Q8 blocks.
    // The first gap is half a modulator period: one divide per note.
    if (!voice.bounce_gap) voice.bounce_gap = clamp(static_cast<i32>(0x7fffffffu / (increment | 1)), 12, 220) << 8;
    if (voice.bounce_gap > 0) {
        voice.bounce_wait -= clock >> 8;
        if (voice.bounce_wait <= 0) {
            voice.bounce = voice.bounce_height;
            voice.bounce_height = mul(voice.bounce_height, 20972);
            voice.bounce_gap = mul(voice.bounce_gap, 26214);
            voice.bounce_wait += voice.bounce_gap;
            if (voice.bounce_gap < 2 << 8) voice.bounce_gap = -1;
        }
    }
    const i32 ring_down = voice.bounce_gap > 0 ? clamp(25165824 / voice.bounce_gap, 64, 8192) : 4096;
    voice.bounce -= mul(voice.bounce, ring_down);
    voice.src[bnce] = voice.bounce;
    for (u32 r = 0; r < count; ++r) {
        Route& route = routes[r];
        if (audio(route.s)) continue;
        route.from = voice.previous[route.s - lfo];
        route.delta = voice.src[route.s] - route.from;
    }

    // Each stage runs as its own tight pass over the block, and only when
    // something is pinned to it; stock engines are built the same way. A
    // lane is one column's modulation: modulators summed into a single ramp,
    // plus any audio rows read from the voice's buses.
    Lane lanes[8];
    for (u32 d = 0; d < 8; ++d) lanes[d].from = lanes[d].delta = 0, lanes[d].count = 0;
    i32 feedback = 0;
    bool gating = false;
    for (u32 r = 0; r < count; ++r) {
        const Route& route = routes[r];
        Lane& lane = lanes[route.d];
        if (route.d == out) {
            gating = gating || !audio(route.s);
        } else if (route.d == ptch && route.s == sine) {
            feedback = feedback + route.amount > one ? one : feedback + route.amount;
        } else if (audio(route.s)) {
            lane.s[lane.count] = route.s;
            lane.amount[lane.count++] = route.amount;
        } else if (route.d != ptch && route.d != rate) {
            const i32 from = mul(route.from, route.amount);
            lane.from += from;
            lane.delta += mul(route.from + route.delta, route.amount) - from;
        }
    }

    // Modulators on PTCH bend pitch exponentially, +-1 octave at full depth.
    voice.previous_pitch_scale = fresh ? exp2_q16(pitch_mod >> 3) : voice.pitch_scale;
    voice.pitch_scale = exp2_q16(pitch_mod >> 3);
    const i32 bare = one - (voiced > one ? one : voiced);
    if (bare || feedback) used[sine] = true;
    const i32 scale_from = voice.previous_pitch_scale, scale_delta = voice.pitch_scale - scale_from;
    // Steps scale by 0.5..2 in 32 bits: pitches are held under 2^30 (about
    // 11 kHz), so p + 2 * (p/2 * c) stays within p/2 .. 2p.
    if (!scale_delta && scale_from == 65536) {
        for (u32 i = 0; i < opxy::block_frames; ++i) voice.steps[i] = pitch[i] > clip ? clip : pitch[i];
    } else {
        for (u32 i = 0; i < opxy::block_frames; ++i) {
            const i32 c = clamp((scale_from + (scale_delta * static_cast<i32>(i + 1) >> 6) - 65536) >> 1, -one, one);
            const u32 p = pitch[i] > 0x40000000u ? 0x40000000u : pitch[i];
            const u32 scaled = p + static_cast<u32>(mul(static_cast<i32>(p >> 1), c)) * 2u;
            voice.steps[i] = scaled > clip ? clip : scaled;
        }
    }

    // Sources. The saw and sub run first so the sine can FM from them this
    // sample, tuned to TILT's ratio of the note (the sub an octave below that),
    // so mixes, FM and ring land on musical intervals. ratio = whole + frac/4096;
    // the overflow bound costs one divide per block.
    const u32 whole = static_cast<u32>(tilt_ratio[tilt]) >> 12, frac = static_cast<u32>(tilt_ratio[tilt]) & 4095u;
    const u32 bound = whole ? clip / whole : 0xffffffffu;
    if (used[saw] || used[sqr]) {
        for (u32 i = 0; i < opxy::block_frames; ++i) {
            const u32 step = voice.steps[i];
            const u32 tuned = step > bound ? clip : step * whole + (step >> 12) * frac;
            voice.tuned[i] = tuned > clip ? clip : tuned;
        }
    }
    if (used[saw]) {
        u32 phase = voice.saw;
        for (u32 i = 0; i < opxy::block_frames; ++i) {
            const u32 step = voice.tuned[i] + (voice.tuned[i] >> 8);  // ~7 cents sharp
            phase += step;
            voice.bus[saw][i] = static_cast<opxy::i16>(clamp((static_cast<i32>(phase ^ 0x80000000u) >> 16) - blep(phase, step), -one, one));
        }
        voice.saw = phase;
        voice.src[saw] = voice.bus[saw][63];
    }
    if (used[sqr]) {
        u32 phase = voice.sub;
        for (u32 i = 0; i < opxy::block_frames; ++i) {
            const u32 step = voice.tuned[i] >> 1;
            phase += step;
            voice.bus[sqr][i] = static_cast<opxy::i16>((phase < 0x80000000u ? 22000 : -22000)
                + mul(blep(phase, step) - blep(phase ^ 0x80000000u, step), 22000));
        }
        voice.sub = phase;
        voice.src[sqr] = voice.bus[sqr][63];
    }
    if (used[noise]) {
        u32 seed = voice.noise;
        for (u32 i = 0; i < opxy::block_frames; ++i) voice.bus[noise][i] = static_cast<opxy::i16>(random(seed));
        voice.noise = seed;
        voice.src[noise] = voice.bus[noise][63];
    }
    if (used[sine]) {
        // Audio on PTCH is 2-op linear FM of the sine alone (index 2), so the
        // saw and sub keep running as modulators; sine -> ptch is feedback FM,
        // averaged over two samples to tame its hiss. The floor stays above
        // zero so feedback can never stall.
        const Lane& fm = lanes[ptch];
        u32 phase = voice.sine;
        i32 last = voice.last_sine, now = voice.src[sine];
        if (fm.count || feedback) {
            // The carrier is held under 0x2aaa0000 (about 7 kHz), so step x 0.1..3
            // stays inside 32 bits.
            for (u32 i = 0; i < opxy::block_frames; ++i) {
                const u32 base = voice.steps[i] > 0x2aaa0000u ? 0x2aaa0000u : voice.steps[i];
                i32 m = feedback ? mul((now + last) >> 1, feedback) : 0;
                for (u32 k = 0; k < fm.count; ++k) m += mul(voice.bus[fm.s[k]][i], fm.amount[k]);
                phase += static_cast<u32>(static_cast<i32>(base) + 4 * mul(static_cast<i32>(base >> 1), clamp(m * 2, -29491, 2 * one) >> 1));
                last = now;
                now = sine_of(phase);
                voice.bus[sine][i] = static_cast<opxy::i16>(now);
            }
        } else {
            for (u32 i = 0; i < opxy::block_frames; ++i) {
                phase += voice.steps[i];
                last = now;
                now = sine_of(phase);
                voice.bus[sine][i] = static_cast<opxy::i16>(now);
            }
        }
        voice.sine = phase;
        voice.last_sine = last;
        voice.src[sine] = now;
    }

    // Mix into the output buffer, which then carries the signal through each stage.
    {
        // One pass per sounding row, each scaled by 3/4 headroom.
        i32 gains[4] = {bare, 0, 0, 0};
        for (u32 r = 0; r < count; ++r)
            if (routes[r].d == out && audio(routes[r].s)) gains[routes[r].s] += routes[r].gain;
        bool first = true;
        for (u32 s = sine; s <= noise; ++s) {
            if (!gains[s]) continue;
            const i32 gain = (gains[s] > one ? one : gains[s]) * 3 >> 2;
            const opxy::i16* row = voice.bus[s];
            if (first) for (u32 i = 0; i < opxy::block_frames; ++i) output[i] = fast(row[i], gain);
            else for (u32 i = 0; i < opxy::block_frames; ++i) output[i] += fast(row[i], gain);
            first = false;
        }
        if (first) for (u32 i = 0; i < opxy::block_frames; ++i) output[i] = 0;
    }
    // Sine wavefolder: gentle saturation when barely pinned, west-coast folds
    // when pushed. It fades in so pinning never clicks.
    if (weight[fold]) {
        const Lane& lane = lanes[fold];
        for (u32 i = 0; i < opxy::block_frames; ++i) {
            const i32 folding = clamp((weight[fold] + lane_at(lane, voice, i)) >> 1, 0, one);
            const i32 x = output[i];
            const i32 driven = clamp(x + mul(x, folding * 7 >> 3) * 8, -8 * one, 8 * one);
            output[i] = x + mul(sine_of(static_cast<u32>(driven) << 15) - x, clamp(folding * 4, 0, one));
        }
    }
    // 2x oversampled state variable lowpass, open 5 octaves above the note
    // (log2 once per block); a pin closes it 5 octaves and its source sweeps 5
    // back. f = 2 sin(pi fc / 2fs) ~ pi step 2^oct / 2^17. Idle, its state
    // follows the signal so it engages without a click.
    if (weight[cut] || weight[res]) {
        const Lane& sweep = lanes[cut];
        const Lane& peak = lanes[res];
        const i32 keyed = 6765 + log2_q12(pitch[0]) - 33 * 4096;
        const i32 closed = 20480 - mul(weight[cut], 20480);
        // Modulators move cutoff and resonance at block rate: a linear ramp
        // between the block's ends. Only audio on CUT or RES works per sample.
        const i32 f0 = clamp(exp2_q16(keyed + closed + mul(sweep.from, 20480)), 40, 30000);
        const i32 f1 = clamp(exp2_q16(keyed + closed + mul(sweep.from + sweep.delta, 20480)), 40, 30000);
        // damping is half of 1.41 (flat) down to 0.07 (singing).
        const i32 d0 = 23170 - mul(clamp(mul(weight[res], 19660) + mul(peak.from, 13107), 0, one), 22000);
        const i32 d1 = 23170 - mul(clamp(mul(weight[res], 19660) + mul(peak.from + peak.delta, 13107), 0, one), 22000);
        i32 low = voice.low, band = voice.band;
        for (u32 i = 0; i < opxy::block_frames; ++i) {
            const i32 ramp = static_cast<i32>(i + 1);
            const i32 f = sweep.count ? clamp(exp2_q16(keyed + closed + mul(lane_at(sweep, voice, i), 20480)), 40, 30000)
                                      : f0 + ((f1 - f0) * ramp >> 6);
            const i32 damping = peak.count
                ? 23170 - mul(clamp(mul(weight[res], 19660) + mul(lane_at(peak, voice, i), 13107), 0, one), 22000)
                : d0 + ((d1 - d0) * ramp >> 6);
            const i32 x = output[i];
            // Two passes per sample. band is saturated to +-2 each pass, so every
            // product stays in 32 bits; the wide input term drops 3 bits first.
            for (u32 k = 0; k < 2; ++k) {
                low = clamp(low + fast(band, f), -4 * one, 4 * one);
                band = saturate(band + (((x - low - 2 * fast(band, damping)) >> 3) * f >> 12));
            }
            output[i] = low;
        }
        voice.low = low;
        voice.band = band;
    } else {
        voice.low = output[opxy::block_frames - 1];
        voice.band = 0;
    }
    if (weight[ring]) {
        const Lane& lane = lanes[ring];
        for (u32 i = 0; i < opxy::block_frames; ++i)
            output[i] = mul(output[i], clamp(one + lane_at(lane, voice, i) - weight[ring], -one, one));
    }
    // Crush: a fractional sample-rate divider (down to 1/32) and rounded bit loss.
    if (weight[crsh]) {
        const Lane& lane = lanes[crsh];
        i32 phase = voice.crush_phase, held = voice.crushed;
        // As with the filter, the divider's rate ramps unless audio drives it.
        const i32 c0 = clamp(mul(weight[crsh], 19660) + mul(lane.from, 13107), 0, one);
        const i32 c1 = clamp(mul(weight[crsh], 19660) + mul(lane.from + lane.delta, 13107), 0, one);
        const i32 r0 = exp2_q16(-mul(c0, 20480)), r1 = exp2_q16(-mul(c1, 20480));
        for (u32 i = 0; i < opxy::block_frames; ++i) {
            const i32 crush = clamp(mul(weight[crsh], 19660) + mul(lane_at(lane, voice, i), 13107), 0, one);
            phase += lane.count ? exp2_q16(-mul(crush, 20480)) : r0 + ((r1 - r0) * static_cast<i32>(i + 1) >> 6);
            if (phase >= 65536) {
                phase -= 65536;
                const i32 bits = crush * 10 >> 15;
                held = (output[i] + ((1 << bits) >> 1)) & ~((1 << bits) - 1);
            }
            output[i] = held;
        }
        voice.crush_phase = phase;
        voice.crushed = held;
    } else {
        voice.crush_phase = 65536;
    }
    // Modulators on OUT shape the level: one ramped gain per such route.
    if (gating) {
        for (u32 r = 0; r < count; ++r) {
            const Route& route = routes[r];
            if (route.d != out || audio(route.s)) continue;
            const i32 g0 = one - route.amount + mul(positive(route.s, route.from), route.amount);
            const i32 g1 = one - route.amount + mul(positive(route.s, route.from + route.delta), route.amount);
            for (u32 i = 0; i < opxy::block_frames; ++i)
                output[i] = mul(output[i], g0 + ((g1 - g0) * static_cast<i32>(i + 1) >> 6));
        }
    }
    // Ring, fold, crush and the soft clip itself can leave DC; a 35 Hz blocker
    // after the clip removes it, then a hard clamp keeps half scale.
    i32 dc_in = voice.dc_in, dc_out = voice.dc_out;
    for (u32 i = 0; i < opxy::block_frames; ++i) {
        const i32 x = soft_clip(output[i]);
        dc_out = x - dc_in + mul(dc_out, 32604);
        dc_in = x;
        output[i] = clamp(dc_out, -one, one) * 32768;
    }
    voice.dc_in = dc_in;
    voice.dc_out = dc_out;
}
}
