#pragma once
#include <opxy/audio.hpp>

namespace matrixy {
using opxy::i32;
using opxy::u32;

// One patch cable. Positions are enum indices: 0 is off, 1..8 a column or row.
// A settled move crossfades from the previous route to the new one.
struct Pin {
    u32 x, y;    // the route fading in (or playing)
    u32 px, py;  // the route fading out
    u32 wx, wy;  // where the knobs are now
    i32 still;   // blocks the knobs have held
    i32 fade;    // Q15 weight of x, y; px, py get the rest
};

struct Voice {
    Pin pins[3];
    u32 sine, saw, sub, lfo, hold, noise;
    i32 src[8];       // latest value of each source row, Q15
    i32 previous[4];  // modulator rows at the last block, for per-sample ramps
    i32 hold_target, env, bounce, bounce_height, bounce_gap, bounce_wait;
    i32 pitch_scale, previous_pitch_scale;  // Q16 pitch multiplier from modulators
    i32 low, band, crushed, crush_phase, dc_in, dc_out;
    i32 depth, last_sine;
    u32 fresh;
    // Block scratch: each audio row's samples and the pitch steps. Kept here,
    // not on the stack, which the audio thread keeps small.
    opxy::i16 bus[4][64];
    u32 steps[64], tuned[64];
};
// Voices share nothing.
struct Shared {};
}
