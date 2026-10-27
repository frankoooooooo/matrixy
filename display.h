#pragma once
#include <opxy/draw.hpp>

namespace matrixy {
// Matches resources.display in definition.json: paths, points, nodes, texts, palette.
// Paths 0-1 are the row wires, 2 the column wires, 3 the frame; then pins and rings.
using Frame = opxy::draw::Frame<4, 64, 12, 0, 8>;
// One attractor per pin. Positions are Q10 cells; times Q10 seconds.
struct Attractor {
    opxy::i32 x, y, strength, pop, still;
    opxy::u32 wx, wy, lx, ly;
};
struct DisplayState {
    Attractor pins[3];
    opxy::i32 time, swell;
    opxy::u32 serial, track, initialized;
};
}
