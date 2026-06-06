# Changelog

## 0.9.103

- Added local M3U / M3U8 playlist import.
- Added AAC, OGG and OPUS playback through FFmpeg.
- Improved raw ADTS AAC duration and seek handling.
- Improved ALAC-in-M4A probing, duration fallback and seek startup.
- Fixed forced FFmpeg conversion command building to use explicit SoXr / aresample filtering.
- Recalibrated Deep Bass Amount to -1 / 0 / +1.
- Fixed unwanted autoplay after opening new files.
- Added progress animation toggle and log-folder selection.
- Removed the SIMD usage counter from the UI.
- Reused the FFmpeg read buffer and reduced non-error log flushing.
- Fixed small DSP Studio dialog-data leaks.

## 0.9.94

- Restored the older playback-entry transport behavior for all tracks with known sample ranges.
- Applied the same transport rule inside same-format gapless chains.
- Left ALSA, DSP, Deep Bass tuning, SIMD PCM conversion, Processing Rules, CUE playback and decoder selection unchanged.

## 0.9.91

- Added AIFF / AIF playback support through FFmpeg.
- Fixed FFmpeg / FFprobe handling of paths containing apostrophes.
- Improved FFmpeg / FFprobe diagnostics.
- Reduced FFmpeg-backed playlist add latency.
- Added session-only external metadata caching.
- Improved FFmpeg-backed CUE startup and seeking.
- Improved APE + CUE seeking and track changes.
- Added case-insensitive CUE audio-file matching.
- Added faster WAV header probing with FFprobe fallback.
- Added continuous CUE image playback.
- Added same-format gapless playback for compatible playlist files.
- Added codec-aware M4A handling for ALAC and AAC.
- Removed the inactive libFLAC threaded decoder option.
- Added Deep Bass Amount control.
- Scoped SIMD acceleration to final PCM/S16 output conversion.
- Renamed SIMD UI and diagnostics wording to “SIMD PCM conversion”.
- Updated the SIMD benchmark.
- Set the FLAC bit-perfect test default length to 30 seconds.
- Added AppImage notes.
- Refined Settings layout and Processing Rules UI.
- Improved same-format gapless transitions for FFmpeg-backed files.
- Added a silence keepalive fallback when gapless prebuffering is late.

## 0.9.81

- Neutralized GTK theme-dependent accent colors.

## 0.9.80

- Replaced Unicode transport glyphs with GTK symbolic icons.

## 0.9.79

- Improved compatibility with older GLib / GTK 3 environments.

## 0.9.78

- Updated project descriptions and contact information.

## 0.9.77

- Expanded DSP Studio with Bass / Treble, tone graph, Deep Bass, shelf profiles, soft volume and Pre-EQ Headroom.
- Added Processing Rules for optional SoXr resampling and bit-depth conversion.
- Improved diagnostics and playback path reporting.
- Reduced GUI overhead with tiered refresh timing.
- Added defensive UTF-8 sanitizing for GTK / Pango strings.
- Preserved clean DSP bypass when all DSP controls are neutral.

## 0.9.45

- Replaced the two headroom selectors with a single Pre-EQ Headroom control.
- Added automatic headroom calculation from Bass / Treble.
- Reset manual headroom adjustment when Bass or Treble changes.
