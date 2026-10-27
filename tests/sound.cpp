// matriXY's own sound checks, compiled for the host and run by
// python sdk.py test --machine <folder>.
#include "../engine.cpp"
#include <cstdio>

namespace {
using namespace matrixy;
int failures = 0;
void fail(const char* what, const char* name) { std::printf("FAIL %s: %s\n", what, name); ++failures; }
opxy::i32 enumerated(int index, int count) { return static_cast<opxy::i32>((index + 0.5) / count * 2147483647.0); }

struct Patch { const char* name; int pins[3][2]; int tilt; double snap; };

// One second at 220 Hz: sounds, stays within the declared half-scale peak,
// and leaves no DC once settled.
void check(const Patch& patch) {
    static Voice voice;
    static Shared shared;
    opxy::i32 parameters[8] = {enumerated(patch.pins[0][0], 9), enumerated(patch.pins[0][1], 9),
                               enumerated(patch.tilt, 20), static_cast<opxy::i32>(patch.snap * 2147483647.0),
                               enumerated(patch.pins[1][0], 9), enumerated(patch.pins[1][1], 9),
                               enumerated(patch.pins[2][0], 9), enumerated(patch.pins[2][1], 9)};
    opxy::u32 pitch[opxy::block_frames];
    for (unsigned i = 0; i < opxy::block_frames; ++i) pitch[i] = 21426140u;
    opxy::i32 output[opxy::block_frames];
    initialize(voice, shared, 7);
    double energy = 0, sum = 0;
    bool bounded = true;
    for (unsigned block = 0; block < 690; ++block) {
        render(voice, shared, 7, output, parameters, pitch);
        for (unsigned i = 0; i < opxy::block_frames; ++i) {
            const double f = output[i] / 2147483648.0;
            energy += f * f;
            if (block >= 345) sum += f;
            bounded = bounded && output[i] >= -1073741824 && output[i] <= 1073741824;
        }
    }
    if (energy / (690.0 * 64) < 0.003 * 0.003) fail("silent", patch.name);
    if (!bounded) fail("above the declared peak", patch.name);
    if (sum / (345.0 * 64) > 0.01 || sum / (345.0 * 64) < -0.01) fail("DC offset", patch.name);
}

// Sweeping a pin mid-note is silent until it holds still for SNAP, then the
// new route plays.
void check_latch(double snap, unsigned settle_blocks) {
    static Voice still, moved;
    static Shared shared;
    opxy::i32 a[8] = {enumerated(1, 9), enumerated(2, 9), enumerated(5, 20), static_cast<opxy::i32>(snap * 2147483647.0),
                      enumerated(0, 9), enumerated(0, 9), enumerated(0, 9), enumerated(0, 9)};
    opxy::i32 b[8];
    for (int i = 0; i < 8; ++i) b[i] = a[i];
    opxy::u32 pitch[opxy::block_frames];
    for (unsigned i = 0; i < opxy::block_frames; ++i) pitch[i] = 42852281u;
    opxy::i32 oa[opxy::block_frames], ob[opxy::block_frames];
    initialize(still, shared, 1);
    initialize(moved, shared, 1);
    const unsigned park = 200;
    bool changed = false;
    for (unsigned block = 0; block < park + settle_blocks + 120; ++block) {
        if (block >= 10 && block < park) { b[0] = enumerated(1 + (block / 20) % 8, 9); b[1] = enumerated(1 + (block / 7) % 8, 9); }
        if (block == park) { b[0] = enumerated(4, 9); b[1] = enumerated(5, 9); }  // lfo -> cut
        render(still, shared, 1, oa, a, pitch);
        render(moved, shared, 1, ob, b, pitch);
        bool same = true;
        for (unsigned i = 0; i < opxy::block_frames; ++i) same = same && oa[i] == ob[i];
        if (!same && block < park && settle_blocks > 1) { fail("sound changed while moving", snap ? "snap" : "inst"); return; }
        if (!same && block >= park && block < park + settle_blocks && settle_blocks > 1) { fail("rerouted before Snap", "snap"); return; }
        changed = changed || !same;
        // With Snap at 0, moves reroute right away, so sweeping already changes the sound.
        if (settle_blocks <= 1 && block == park && !changed) { fail("instant Snap waited", "inst"); return; }
    }
    if (!changed) fail("parked route never played", snap ? "snap" : "inst");
}
// env -> out must fade at every tilt, and depth (a mul() coefficient) must stay
// within Q15 there.
void check_decay() {
    static Voice voice;
    static Shared shared;
    opxy::u32 pitch[opxy::block_frames];
    for (unsigned i = 0; i < opxy::block_frames; ++i) pitch[i] = 21426140u;
    opxy::i32 output[opxy::block_frames];
    for (int tilt = 0; tilt < 20; ++tilt) {
        const opxy::i32 parameters[8] = {enumerated(1, 9), enumerated(6, 9), enumerated(tilt, 20), 0,
                                         enumerated(0, 9), enumerated(0, 9), enumerated(0, 9), enumerated(0, 9)};
        initialize(voice, shared, 3);
        double early = 0, late = 0;
        for (unsigned block = 0; block < 690; ++block) {
            render(voice, shared, 3, output, parameters, pitch);
            for (unsigned i = 0; i < opxy::block_frames; ++i) {
                const double f = output[i] / 2147483648.0;
                if (block < 35) early += f * f;
                if (block >= 552) late += f * f;
            }
        }
        char name[24];
        std::snprintf(name, sizeof name, "tilt %d", tilt);
        if (late * 35 >= early * 138) fail("env -> out did not fade", name);
        // Depth is a mul() coefficient, which must stay within Q15: 32768
        // wraps to a gain of -1 on the Blackfin.
        if (voice.depth < 1 || voice.depth > 32767) fail("depth outside Q15", name);
    }
}

// Three pins stacked on every cell at the deepest tilt, extreme and gliding
// pitches, and a note restarted on a voice that just played: always bounded.
void check_extremes() {
    static Voice voice;
    static Shared shared;
    const opxy::u32 steady[4] = {1u, 21426140u, 0x7fff0000u, 0xfffffff0u};
    opxy::u32 pitch[opxy::block_frames];
    opxy::i32 output[opxy::block_frames];
    for (int x = 1; x <= 8; ++x)
        for (int y = 1; y <= 8; ++y)
            for (int p = 0; p < 5; ++p) {
                for (unsigned i = 0; i < opxy::block_frames; ++i)
                    pitch[i] = p < 4 ? steady[p] : 1000000u + i * 30000000u;  // a glide inside the block
                const opxy::i32 parameters[8] = {enumerated(x, 9), enumerated(y, 9), enumerated(19, 20), 0,
                                                 enumerated(x, 9), enumerated(y, 9), enumerated(x, 9), enumerated(y, 9)};
                initialize(voice, shared, 5);
                for (unsigned block = 0; block < 120; ++block) {
                    if (block == 60) start(voice, shared, 5);
                    render(voice, shared, 5, output, parameters, pitch);
                    for (unsigned i = 0; i < opxy::block_frames; ++i)
                        if (output[i] < -1073741824 || output[i] > 1073741824) { fail("above the declared peak", "stacked extremes"); return; }
                }
            }
}
} // namespace

int main() {
    const char* xs[] = {"out", "ptch", "fold", "cut", "res", "ring", "crsh", "rate"};
    const char* ys[] = {"sine", "saw", "sqr", "nois", "lfo", "env", "s+h", "bnce"};
    const int tilts[] = {0, 5, 19};  // 1:4, 1:1, 9:1
    char names[3][8][8][32];
    for (int t = 0; t < 3; ++t)
        for (int x = 1; x <= 8; ++x)
            for (int y = 1; y <= 8; ++y) {
                std::snprintf(names[t][x - 1][y - 1], 32, "%s>%s tilt %d", ys[y - 1], xs[x - 1], tilts[t]);
                const Patch patch = {names[t][x - 1][y - 1], {{x, y}, {0, 0}, {0, 0}}, tilts[t], 0.4};
                check(patch);
            }
    const Patch stress[] = {
        {"feedback fm, ring, fold", {{2, 1}, {6, 1}, {3, 2}}, 19, 0.4},
        {"noise squelch", {{1, 4}, {4, 5}, {5, 6}}, 18, 0.4},
        {"rate chaos", {{8, 5}, {8, 7}, {2, 5}}, 19, 0.4},
        {"stacked fm", {{2, 2}, {2, 3}, {2, 4}}, 19, 0.4},
    };
    for (const Patch& patch : stress) check(patch);
    // The engine's own settle arithmetic: Q15 Snap x 690 blocks.
    check_latch(0.4, static_cast<unsigned>((static_cast<opxy::i32>(0.4 * 2147483647.0) >> 16) * 690 >> 15));
    check_latch(1.0, 689);
    check_latch(0.0, 1);
    check_decay();
    check_extremes();
    if (!failures) std::puts("matriXY sound: 192 cells, stress patches, decay at all tilts, stacked extremes and the Snap latch pass");
    return failures ? 1 : 0;
}
