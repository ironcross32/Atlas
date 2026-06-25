# Atlas

Atlas is a macOS synth based on Vital, focused on real VoiceOver access.

The goal is simple: the synth should be usable without guessing where the controls are, without losing focus every time something changes, and without depending on the original visual layout.

This is based on the open source Vital codebase, but it is not Vital Audio's official app.

## What is different

- A flat, VoiceOver-friendly layout with groups for presets, global controls, oscillators, routing, filters, busses, envelopes, LFOs, effects, modulations, macros, and zones.
- AU and VST3 macOS builds.
- Four wavetable oscillators.
- Improved sample oscillator controls.
- Three effect busses with routing.
- Extra effects: limiter, frequency shifter, utility, phase shift, and dimension expander.
- 16 macros, macro rename, bipolar macros, and better modulation assignment.
- Wavetable and sample folder browsing.
- Accessible LFO editing and wavetable editing.
- Keyboard shortcuts for moving around the synth faster.

## Install

Download the latest `.pkg` from the releases page and run it.

The installer can install:

- Audio Unit
- VST3
- the accessible layout guide

The guide is installed to:

`~/Documents/Alessio Plugins/Atlas/Accessible Layout Guide.html`

More details are in [INSTALL.md](INSTALL.md).

## Build

This repo builds with CMake and JUCE on macOS.

Short version:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target Atlas_All -j2
./installer/build-pkg.sh
```

More details are in [BUILDING.md](BUILDING.md).

## Notes

- Release builds are recommended. Debug builds can spike CPU harder and are not a good way to test polyphonic patches.
- Presets save as `.atp`, banks as `.atb`, LFO/envelope shapes as `.ats`, FX presets as `.atf`, and wavetables as `.att`. Older `.vital`, `.vitalbank`, `.vitallfo`, and `.vitaltable` files can still be loaded.
- This does not connect to Vital's account, store, or online services.
- Factory Vital presets are not included here.

## License

This project follows Vital's GPLv3 license. See [LICENSE](LICENSE).

Do not use the Vital name, Vital Audio name, Matt Tytel name, or original Vital branding to market this build.
