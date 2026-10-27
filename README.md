# matriXY

An 8×8 pin-matrix synth for the OP-XY (OS 1.2.1, Open-XY SDK 1.3), drawn as
a greyscale 3D wire terrain. Machine ID `matrixy`.

![matriXY running in the Open-XY browser preview: the 3D wire terrain with three pins, playing the chaos garden preset](https://github.com/frankoooooooo/matrixy/releases/download/v1.0.0/matrixy-screen.png)

Rows are sources and columns are destinations. Three pins patch them, and
each pin raises a hill in the mesh where it sits. A pin only reroutes once
it has rested for the Snap time, so moving pins during a set is silent.

## Controls

| Knob | Turn | Press |
| --- | --- | --- |
| X | The held pin's column (destination) | Next pin's X (1 → 2 → 3) |
| Y | The held pin's row (source) | Next pin's Y |
| Tilt | One of 20 ratios of the note, 1:4 to 9:1 | |
| Snap | Reroute delay, 0 to 1 s (`inst` at 0) | |

The header shows the held pin and its cell, such as `2 cut` and `2 saw`; four-letter words drop the space (`1bnce`) to fit.

## Tilt

Tilt is one knob that sets the patch's musical relationship to the note you
play. It steps through 20 ratios: 1:4, 1:3, 1:2, 2:3, 3:4, **1:1** (default),
5:4, 4:3, 3:2, 5:3, 2:1, 5:2, 3:1, 7:2, 4:1, 5:1, 6:1, 7:1, 8:1 and 9:1.
That ratio does three things at once:

1. **Tunes the oscillators.** The saw plays at the note × the ratio (slightly
   detuned for movement), and the square an octave below the saw. At 3:2 the
   saw sounds a fifth above the sine, at 2:1 an octave, at 5:4 a major third.
   Mixed, ring-modulated or used for FM, they land on musical intervals.
2. **Sets how fast the modulators move.** The LFO, envelope, sample and hold
   and bouncing ball run at the note's frequency × the ratio ÷ 128. At A3 and
   1:1 the LFO wobbles at about 1.7 Hz. Higher notes and higher ratios move
   faster, so a wobble stays locked to the pitch across the keyboard.
3. **Sets how hard the pins hit.** Depth rises from 35% at 1:4 to 100% at
   9:1: low tilts are gentle, high tilts deep. On screen, the hills grow
   taller and wider.

## Sources (rows)

| Source | What it is |
| --- | --- |
| `sine` | Sine at the note's pitch |
| `saw` | Band-limited saw, tuned by Tilt |
| `sqr` | Band-limited square, an octave below the saw |
| `nois` | White noise |
| `lfo` | Triangle LFO locked to the note's pitch |
| `env` | Decay envelope that restarts every note |
| `s+h` | Smoothed random steps |
| `bnce` | Bouncing ball: bounces get closer together and quieter |

## Destinations (columns)

| Destination | What a pin there does |
| --- | --- |
| `out` | Audio joins the mix (a plain sine plays if nothing is pinned); modulators shape the volume |
| `ptch` | Modulators bend pitch ±1 octave; audio frequency-modulates the sine, including feedback |
| `fold` | Sine wavefolder |
| `cut` | 2× oversampled lowpass that follows the note, with a 5-octave sweep |
| `res` | Resonance, up to near self-oscillation |
| `ring` | Ring modulation |
| `crsh` | Smooth sample-rate and bit reduction |
| `rate` | Speeds up or slows down the modulators, ±3 octaves |

## Presets

init, kick drum, noise snare, pew pew, bouncing ball, glass bell, wobble
bass, slow drone, growl, broken radio, chaos garden, chiptune.

## Performance and size

| | |
| --- | --- |
| CPU on hardware | 2.126% of the stock voice budget: median of 3 runs on an OP-XY, one voice, controls at their midpoints (noise into the filter). Stock Prism measured 2.127% and Wavetable 2.139% in the same runs. Not a worst case, and not a prediction of polyphony |
| Host estimate, heaviest setting | About 13,600 Blackfin instructions per voice block (the SDK's target is 15,000) |
| Polyphony | 8 voices |
| Firmware memory | 32.6 KiB of the shared 2040 KiB module budget, including state |
| Per voice | 1,260 bytes; screen state 128 bytes |

## Develop

Keep the [Open-XY SDK](https://github.com/etcetc000/open-xy) beside this
folder as `../open-xy` on `staging` (adjust the paths if yours is elsewhere).

```sh
python ../open-xy/sdk.py test --machine . --firmware
python ../open-xy/sdk.py preview --machine . --out ../previews/matrixy-001
python ../open-xy/tools/serve_harness.py --preview ../previews/matrixy-001
```

`tests/sound.cpp` checks every cell at three tilts, stacked pins at extreme
pitches, decay at every tilt, and that moving a pin stays silent for the Snap
time. `tests/display.cpp` checks 80,000 random screen frames.

## Hardware

Follow the SDK's [build guide](https://github.com/etcetc000/open-xy/blob/staging/docs/build.md)
and [hardware profiling](https://github.com/etcetc000/open-xy/blob/staging/docs/profiling.md).
Keep firmware and stock content outside this repository; it holds source only.

Licensed under the MIT licence; see [LICENSE](LICENSE).
