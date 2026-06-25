# Releasing

This is the checklist I use before publishing a build.

## Before building

- Make sure the repo has no accidental build output committed.
- Update the version in `CMakeLists.txt` if needed.
- Build release, not debug.

## Build

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target Atlas_All -j2
./installer/build-pkg.sh
```

## Test

At minimum:

- open AU in Reaper
- open VST3 in Reaper
- check VoiceOver groups
- load/change presets
- check oscillators, filters, busses, effects, modulations, macros, LFOs, and sample controls
- play a few polyphonic patches

## GitHub

Create a new GitHub repo under your account.

If this checkout still points to the original Vital remote, change it before pushing:

```sh
git remote rename origin upstream
git remote add origin git@github.com:YOUR_USER/YOUR_REPO.git
git push -u origin main
```

Attach the `.pkg` from `dist/` to a GitHub release.

Do not upload factory Vital preset content unless you have the right to distribute it.
