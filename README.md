# PCM Transport v0.9.103

**PCM Transport** is a Linux desktop audio player focused on direct PCM playback, predictable DSP, and clear signal-path reporting.

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

## Features

- GTK 3 desktop interface
- Direct ALSA output
- Native FLAC decoding through libFLAC
- Runtime FFmpeg / FFprobe support for MP3, M4A, AAC, OGG, OPUS, WAV, AIFF/AIF, APE and WV
- CUE support, including continuous CUE image playback
- Local M3U / M3U8 playlist import
- Same-format gapless playback where possible
- Optional SoXr resampling and bit-depth rules
- Baxandall-style Bass / Treble controls
- Deep Bass with Reference and Punch presets
- Soft volume, Pre-EQ Headroom, level meter and clip indicator
- Optional SIMD PCM output conversion
- FLAC bit-perfect test and SIMD PCM conversion benchmark

---

## Playback notes

- Native FLAC is used when no Processing Rules are applied.
- ALAC-in-M4A is decoded through FFmpeg.
- FFmpeg is used for external formats and conversion paths.
- DSP is bypassed when Bass/Treble are neutral, volume is 100%, Pre-EQ Headroom is 0 dB, and Deep Bass is off.
- The main display shows the active playback path.
- For the cleanest ALSA path, select a direct `hw:X,Y` device and avoid forced conversion rules.

---

## AppImage

A ready-to-run Linux x86_64 AppImage is available on the GitHub Releases page.

Recommended asset name:

```text
PCM-Transport-latest-x86_64.AppImage
```

Run:

```bash
chmod +x PCM-Transport-latest-x86_64.AppImage
./PCM-Transport-latest-x86_64.AppImage
```

---

## Dependencies

### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake pkgconf alsa-lib flac gtk3 ffmpeg
```

### Debian / Ubuntu

```bash
sudo apt install build-essential cmake pkg-config \
    libasound2-dev libflac++-dev libgtk-3-dev ffmpeg
```

### Runtime

- ALSA
- GTK 3
- libFLAC
- ffmpeg
- ffprobe

Native FLAC playback works without FFmpeg. FFmpeg and FFprobe are required for external formats, metadata probing and conversion paths.

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

- C++17
- GTK 3
- Arch, Debian, Ubuntu and similar Linux distributions
- X11 and Wayland through GTK

---

## License

GNU General Public License v3.0. See `LICENSE`.

---

## Support

If you enjoy PCM Transport, you can buy the author a cup of coffee:

```text
ETH / USDT (ERC-20):
0x37385DA1388F2921583d4750FB44Def7D76cAb23
```
