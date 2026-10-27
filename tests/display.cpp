// The display's frame stays valid for any input: random knobs, pins stacked
// on one cell, every tilt and snap, odd timing, note activity, track changes
// and reduced motion. Run by python sdk.py test --machine <folder>.
#include "../display.cpp"
#include <cstdio>

int main() {
    static matrixy::DisplayState state;
    static matrixy::Frame frame;
    opxy::u32 seed = 12345;
    const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed; };
    int bad = 0;
    for (int run = 0; run < 400 && !bad; ++run) {
        matrixy::display_reset(state);
        opxy::draw::Input input = {};
        for (int frame_index = 0; frame_index < 200; ++frame_index) {
            for (int i = 0; i < 8; ++i) input.slots[i] = static_cast<opxy::i32>(next() >> 1);
            if (run % 3 == 0) input.slots[4] = input.slots[6] = input.slots[0], input.slots[5] = input.slots[7] = input.slots[1];
            for (int i = 0; i < 4; ++i) {
                input.parameters[i] = input.slots[i];
                input.shown[i] = static_cast<opxy::u8>(i < 2 ? (next() % 3 == 0 ? i : next() % 2 ? 4 + i : 6 + i) : i);
            }
            input.elapsed = static_cast<opxy::i32>(next() % 400) - 50;
            input.track = run % 7 == 0 ? next() % 3 : 1;
            input.note_serial += next() % 5 == 0;
            input.held_notes = next() % 9 == 0 ? 0xffffffffu : next() % 4;
            input.reduced_motion = run % 5 == 0;
            matrixy::display(state, input, frame);
            if (!opxy::draw::valid(frame)) { std::printf("FAIL invalid frame: run %d frame %d\n", run, frame_index); bad = 1; break; }
        }
    }
    if (!bad) std::puts("matriXY display: 80,000 fuzzed frames valid");
    return bad;
}
