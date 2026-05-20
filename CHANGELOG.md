# Changelog

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
