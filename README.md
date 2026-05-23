# PCM Transport v0.9.91

**PCM Transport** is a Linux desktop audio player inspired by early digital audio systems, focused on **transparent PCM transport, predictable DSP, and honest signal path reporting**.

---

## Author

**Andrey Berestov**

Website: https://andreyberestov.github.io/pcm-transport/

© 2026 Andrey Berestov

---

## Screenshots

### Main window
![Main window](screenshots/main.png)

### DSP Studio
![DSP Studio](screenshots/dsp.png)

---

## Current direction

- GTK 3 desktop interface  
- Direct ALSA playback (no mandatory PulseAudio / PipeWire layer)  
- Native FLAC decoding through libFLAC (bit-accurate path when possible)  
- FFmpeg / FFprobe runtime decoding for MP3 / M4A / WAV / AIFF / APE / WV and FLAC processing paths  
- Codec-aware M4A handling: ALAC is treated as lossless, AAC remains conservative/lossy  
- CUE support with case-insensitive audio-file matching  
- Continuous CUE image playback for adjacent tracks in one album image  
- Same-format gapless chaining for ordinary playlist files where possible, with earlier preparation for FFmpeg-backed tracks  
- Small CD-like ALSA PCM period / buffer configuration (low-latency oriented design)  

### DSP Studio

- Baxandall-like Bass / Treble with selectable shelf profiles  
- Tone response graph driven by the same shelf logic as playback  
- Deep Bass with adaptive contour shaping, controlled harmonic reinforcement, and adjustable amount  
- Soft volume  
- Automatic Pre-EQ Headroom with temporary manual trim  
- Optional Processing Rules for SoXr resampling and bit-depth conversion  
- Optional SIMD PCM output conversion where supported  
- Level meter and clip detection with visual indicators  

### Diagnostics

- Detailed ALSA and FFmpeg / FFprobe diagnostics  
- Session-only external metadata cache for faster repeated FFmpeg/CUE probing  
- Gapless/continuous playback behavior for compatible CUE and same-format file sequences, including keepalive diagnostics when prebuffering is late  
- Explicit playback path display (reflects the real active chain)  
- FLAC / libFLAC capability notes  
- SIMD PCM conversion availability, self-test status, and usage counter  
- Offline FLAC bit-perfect test using libFLAC / flac CLI before ALSA  
- Offline SIMD PCM conversion benchmark for the current processing path before ALSA  

---

## DSP philosophy

PCM Transport uses a **minimal, transparent DSP model**:

- No compressors, limiters, or loudness processing  
- No hidden processing outside the reported signal path  
- Fully predictable behavior  

DSP is fully bypassed when:

- Bass = 0 dB  
- Treble = 0 dB  
- Volume = 100%  
- Pre-EQ Headroom = 0 dB  
- Deep Bass is disabled  

In this state, the internal SoftDSP stage is **not applied to the signal path**.

Level metering, clip detection, diagnostics, and SIMD PCM conversion counters are reporting tools. They do not intentionally change the audio signal.

The CLIP indicator reflects **actual overload events before final output clamping**, not just peak level.

---

## Audio path notes

- Native FLAC playback uses libFLAC when no resampling or bit-depth conversion is required  
- When conversion is needed, FFmpeg is used  
- High-quality SoXr resampling is used when available  
- Output format depends on ALSA device capabilities  

⚠️ Important:

Even with DSP disabled, audio may still be processed if:

- FFmpeg resampling or bit-depth conversion is active  
- ALSA device or plugin layer performs conversion (`default`, `plughw`)  

---

## Quick start (clean / bit-perfect path)

To get the cleanest possible playback path:

- Use **FLAC files**
- Disable DSP (all controls at neutral)
- Select **ALSA device `hw:X,Y`**
- Avoid resampling / bit-depth conversion
- Ensure output format matches the source

---

## Download

A ready-to-run Linux x86_64 AppImage is available on the GitHub Releases page.

The recommended release asset name is:

```text
PCM-Transport-latest-x86_64.AppImage
```

Run it with:

```bash
chmod +x PCM-Transport-latest-x86_64.AppImage
./PCM-Transport-latest-x86_64.AppImage
```

The AppImage is the easiest way to try PCM Transport without building it from source.
For development, packaging, or source-based installation, use the build instructions below.

---

## Dependencies

### Build dependencies

#### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake pkgconf alsa-lib flac gtk3 ffmpeg
```

#### Debian / Ubuntu

```bash
sudo apt install build-essential cmake pkg-config \
    libasound2-dev libflac++-dev libgtk-3-dev ffmpeg
```

---

### Runtime dependencies

- ALSA
- GTK 3
- libFLAC++
- ffmpeg
- ffprobe

⚠️ `ffmpeg` and `ffprobe` are required for:

- MP3 / M4A / WAV / AIFF / APE / WV playback  
- FLAC processing paths involving conversion  
- Metadata probing  

Native FLAC playback works without FFmpeg.

---

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

---

## Run

```bash
./build/pcm_transport
```

---

## Compatibility

- Works on GNOME, KDE, and other GTK-compatible environments  
- Not tied to a specific desktop environment  
- Works on X11 and Wayland via GTK  
- Designed for standard Linux distributions (Debian, Ubuntu, Arch, etc.)

---

## Design principles

- Transparent signal path  
- No hidden processing  
- Predictable DSP behavior  
- Honest runtime reporting  
- Minimal but meaningful controls  

---

## License

GNU General Public License v3.0. See `LICENSE`.

---

## Support

If you enjoy PCM Transport, you can buy the author a cup of coffee:

ETH / USDT (ERC-20): 0x37385DA1388F2921583d4750FB44Def7D76cAb23

---

## Feedback and bug reports

Please use GitHub Issues for bug reports and feature requests:
https://github.com/andreyberestov/pcm-transport/issues
