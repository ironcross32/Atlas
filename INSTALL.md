# Install

Use the latest `.pkg` from GitHub releases.

The installer lets you choose what to install:

- AU: `/Library/Audio/Plug-Ins/Components/Atlas.component`
- VST3: `/Library/Audio/Plug-Ins/VST3/Atlas.vst3`
- accessible layout guide

The installer also creates the Atlas content folder if needed:

`~/Documents/Alessio Plugins/Atlas`

Inside it you should have Factory and User folders for presets, wavetables, samples, and LFOs.

## After installing

In Reaper or another host, rescan plugins if the new build does not appear.

For the AU, the plugin name is:

`Atlas`

For the VST3, the plugin name is also:

`Atlas`

## Updating

Install the new `.pkg` over the old one.

If a host keeps showing an older build, clear or rescan that host's plugin cache.

## Uninstall

Remove whichever files you installed:

```sh
sudo rm -rf "/Library/Audio/Plug-Ins/Components/Atlas.component"
sudo rm -rf "/Library/Audio/Plug-Ins/VST3/Atlas.vst3"
```

Your user presets and samples live in `~/Documents/Alessio Plugins/Atlas`, so do not delete that folder unless you really want to remove your data too.
