# inspectrum ng

inspectrum ng (next generation) is a fork of [inspectrum](https://github.com/miek/inspectrum) for analysing captured signals from software-defined radio receivers.

## Features

 * Large (100GB+) file support with memory-mapped I/O
 * Spectrogram with zoom/pan, zero-padding, Y zoom, averaging
 * Plots of amplitude, frequency, phase and threshold with binary/hex/ASCII overlay
 * Cursors with symbol rate detection (ASK/OOK), grid opacity, state preservation
 * Tuner with editable centre frequency and bandwidth, crop-to-tuner, visibility toggle
 * Session save/load (.isession) with full state including bookmarks
 * Export: selected samples, tuner-filtered with resampling, spectrogram to PNG
 * Drag & drop file loading
 * SI unit display and input (k, M, G suffixes)
 * FFTW wisdom with pre-warmed plans for fast startup
 * Native platform UI style (Windows Vista / Fusion)

## Build from source

### Prerequisites

 * cmake >= 3.5
 * fftw 3.x
 * [liquid-dsp](https://github.com/jgaeddert/liquid-dsp) >= v1.3.0
 * pkg-config
 * Qt5 or Qt6

### Linux (Ubuntu / Debian)

Install dependencies:

```bash
# Ubuntu 22.04 / 24.04 LTS, Debian 12 (Qt5)
sudo apt install cmake g++ pkg-config libfftw3-dev libliquid-dev libgl1-mesa-dev qtbase5-dev

# Ubuntu 24.04 LTS, Debian 12 (Qt6)
sudo apt install cmake g++ pkg-config libfftw3-dev libliquid-dev libgl1-mesa-dev qt6-base-dev
```

Build:

```bash
mkdir build && cd build
cmake ../ -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### macOS (x86_64 / ARM64)

Install dependencies:

```bash
brew install cmake fftw liquid-dsp qt@6
```

Build:

```bash
mkdir build && cd build
cmake ../ -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6)"
make -j$(sysctl -n hw.ncpu)
```

### Windows (MinGW64 / MSYS2)

Install MSYS2 packages:

```bash
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-qt5-base mingw-w64-x86_64-fftw mingw-w64-x86_64-pkg-config
```

Build [liquid-dsp](https://github.com/bvernoux/liquid-dsp) from source (static), then:

```bash
mkdir build && cd build
cmake ../ -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFTW_USE_STATIC=ON -DLIQUID_USE_STATIC=ON
ninja
```

### Windows (Visual Studio 2022)

```bash
mkdir build && cd build
cmake ../ -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### Run

    ./inspectrum [filename]

Options:

    -r, --rate <Hz>     Set sample rate
    -f, --format <fmt>  Set file format (cfile, cs8, cu8, cs16, sigmf-meta, etc.)

## Input formats

 * `*.sigmf-meta, *.sigmf-data` - SigMF recordings
 * `*.cf32`, `*.fc32`, `*.cfile` - Complex 32-bit floating point (GNU Radio, osmocom_fft)
 * `*.cf64`, `*.fc64` - Complex 64-bit floating point
 * `*.cs32`, `*.sc32`, `*.c32` - Complex 32-bit signed integer (SDRAngel)
 * `*.cs16`, `*.sc16`, `*.c16` - Complex 16-bit signed integer (BladeRF)
 * `*.cs8`, `*.sc8`, `*.c8` - Complex 8-bit signed integer (HackRF)
 * `*.cu8`, `*.uc8` - Complex 8-bit unsigned integer (RTL-SDR)
 * `*.f32` - Real 32-bit floating point
 * `*.f64` - Real 64-bit floating point
 * `*.s16` - Real 16-bit signed integer
 * `*.s8` - Real 8-bit signed integer
 * `*.u8` - Real 8-bit unsigned integer
 * `*.isession` - inspectrum ng session file
 * `*.json` - Bookmarks file

Unknown extensions default to `*.cf32`. 64-bit samples are truncated to 32-bit internally.

## Session files

inspectrum ng saves/loads complete application state to `.isession` JSON files (version 0.5.0 format). Sessions include signal file path, spectrogram settings, tuner position, cursor state, bookmarks, derived plots, and window layout.

## License

GPL v3 -- see original [inspectrum](https://github.com/miek/inspectrum) for details.
