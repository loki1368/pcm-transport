# Changelog

## 0.9.94

- Restored the legacy range-limited transport wrapper for all playback entries with known sample ranges, matching the mature 0.9.77 bounded decoder behavior beyond native FLAC.
- Applied the same range-limited discipline inside same-format gapless chains while preserving the 0.9.91 gapless playback architecture, prebuffering and keepalive behavior.
- Kept ALSA backend settings, DSP / Deep Bass behavior, SIMD PCM conversion, Processing Rules, CUE continuous playback and decoder selection logic unchanged.
- Updated playback diagnostics wording to report the generic bounded transport path instead of limiting the note to native FLAC.

## 0.9.93

- Restored the legacy bounded native FLAC transport path as a permanent rule for ordinary full-file libFLAC playback, preserving the mature 0.9.77 playback behavior while keeping the 0.9.91/0.9.92 architecture.
- Extended the same bounded native FLAC transport discipline into same-format gapless chains for native libFLAC tracks with known sample ranges.
- Kept FFmpeg-backed playback, external decoder natural EOF behavior, ALSA backend settings, DSP behavior, CUE handling, SIMD PCM conversion and processing rules unchanged.
- Added debug logging when the legacy bounded native FLAC transport path is activated.

## 0.9.92

- Restored the legacy range-limited wrapper for ordinary full-file native libFLAC playback as a conservative diagnostic A/B step against the 0.9.77 playback path.
- Kept ALSA backend settings, DSP behavior, FFmpeg-backed playback, CUE handling, same-format gapless chaining, SIMD PCM conversion and existing processing rules unchanged.

## 0.9.91

This release includes the accumulated updates since 0.9.81, focused on external format handling, CUE/gapless playback, diagnostics, SIMD cleanup, and Settings UI polish.

- Added AIFF / AIF playback support through the existing FFmpeg external decode path.
- Fixed FFmpeg / FFprobe handling of file paths containing apostrophes.
- Improved FFmpeg / FFprobe diagnostics, including retained stderr details for troubleshooting.
- Reduced FFmpeg-backed playlist add latency by combining external format probing and tag reading where possible.
- Added session-only external metadata caching and clear it when loading a new file set.
- Added faster FFmpeg-backed CUE startup and seeking with known metadata reuse and direct open-at-sample support.
- Improved APE + CUE seeking and track changes.
- Added case-insensitive matching for CUE-referenced audio files, such as `.APE` matching an existing `.ape` file.
- Added a lightweight WAV header probe with FFprobe fallback for faster WAV playlist loading.
- Added continuous CUE image playback for adjacent CUE tracks sharing the same audio file and output format.
- Added same-format gapless chaining for ordinary playlist files with background next-track prebuffering where possible.
- Added codec-aware M4A handling: ALAC/M4A is treated as lossless, while AAC/M4A remains conservative/lossy.
- Improved ALAC/M4A lossless badge handling and allows ALAC/M4A in safe same-format gapless chains.
- Avoided hard range-limiting ordinary full-file tracks so external decoders can play to their natural EOF.
- Added FFprobe `codec_name`, `duration_ts`, and `time_base` parsing to improve external metadata and duration handling.
- Removed the inactive libFLAC threaded decoder option and related UI/status plumbing.
- Added Deep Bass Amount control (-2 to +2), scaling the final Deep Bass contribution while preserving the Reference / Punch character at amount 0.
- Scoped SIMD acceleration to final PCM/S16 output conversion only.
- Removed the previous SIMD Bass/Treble and Deep Bass processing paths.
- Renamed SIMD-related UI and diagnostics wording to “SIMD PCM conversion”.
- Updated the SIMD benchmark to compare scalar PCM conversion vs SIMD PCM conversion on the current processing path.
- Set the FLAC bit-perfect test default length to 30 seconds for consistency with diagnostics.
- Added AppImage usage notes to README and the project site.
- Refined Settings layout, logging controls, Processing Rules spacing, and the classic OPEN / eject-style file button.
- Reworked Processing Rules so added rules appear below the Add rule controls in a stable-height list area without forced GTK resize/event flushing.
- Improved same-format gapless transitions with earlier next-track preparation for FFmpeg-backed files and larger prebuffering for external decoders.
- Added a short silence keepalive fallback when a gapless prebuffer is not ready in time, keeping the ALSA stream active instead of forcing an immediate device restart.
- Added diagnostic logging when gapless playback has to use the silence keepalive fallback.
- Kept native FLAC bit-perfect path, CD-like ALSA buffers, DSP tuning, Deep Bass Reference/Punch and normal playback behavior unchanged.

## 0.9.81

- Neutralized GTK theme-dependent accent colors for playlist selection, sliders, checkboxes and notebook selection styling for more consistent cross-distribution UI.

## 0.9.80

- Replaced Unicode transport button glyphs with GTK symbolic icons for more consistent appearance across Linux desktop themes.

## 0.9.79

- Improved compatibility with older GLib/GTK 3 environments on Linux distributions such as Ubuntu.

## 0.9.78

- Updated project descriptions and contact information.

## 0.9.77

Summary of changes since **0.9.45**:

- Expanded DSP Studio with Baxandall-like Bass / Treble refinement, tone response graph, Deep Bass, selectable tone shelf profiles, soft volume, and automatic Pre-EQ Headroom.
- Added Deep Bass processing with adaptive contour shaping, harmonic reinforcement, safer headroom handling, and stable Reference / Punch presets.
- Added Processing Rules for optional SoXr resampling and bit-depth conversion, while preserving native playback when rules are not used.
- Improved diagnostics with explicit playback path reporting, ALSA format negotiation logging, FLAC/libFLAC capability notes, and an offline FLAC bit-perfect test.
- Reduced GUI overhead with tiered refresh timing, avoided redundant label updates, and kept the level meter smooth without changing the audio path.
- Optimized internal buffering and ALSA conversion work, including optional SIMD acceleration and a SIMD usage counter for diagnostic visibility.
- Added defensive UTF-8 sanitizing for GTK/Pango display strings.
- Preserved the neutral DSP bypass behavior: when all DSP controls are neutral and Deep Bass is disabled, the SoftDSP stage is not applied.

## 0.9.45

- Replaced the two headroom selectors with a single **Pre-EQ Headroom** control.
- The control is set automatically from Bass/Treble using the safer base model.
- Manual headroom adjustment is reset when Bass or Treble changes.
- Added a tooltip explaining the new Pre-EQ Headroom behavior.
