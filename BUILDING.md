# Building

macOS is the main supported build target right now.

## Requirements

- macOS
- Xcode command line tools
- CMake 3.22 or newer
- the JUCE folder next to this repo

The folder layout should look like this:

```text
Vital/
  JUCE/
  vital/
```

## Release build

Use release builds for testing and release. They behave much better with CPU-heavy patches.

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target Atlas_All -j2
```

For a Windows build, visual studio and the windows SDK are required, and the build must be done in one step like this:
`cmake --build build-release --target Atlas_VST3 --config Release -j2`
The built files are placed under:

`build-release/Atlas_artefacts/Release`

Atlas only builds AU and VST3 now. There is no standalone target or app bundle.

## Package installer

After the release build:

```sh
./installer/build-pkg.sh
```

The package is written to:

`dist/`

Example:

`dist/Atlas-1.1.5.pkg`

If a package with that name already exists, the script adds a timestamp.

## Clean rebuild

```sh
rm -rf build-release
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target Atlas_All -j2
```

## Debug builds

Debug builds are useful for development only. Do not judge voice count or CPU from them.
