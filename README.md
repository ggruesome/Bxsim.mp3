# Bxsim

A real-time audio visualizer written in C++ with SFML 3.1.

Open a song and it plays while the visuals react to it: frequency bars, a mirrored ring, or an oscilloscope, all with bloom glow, beat-reactive pulses and particles.

## Using it

Get a song in any of these ways:

- Drop an audio file onto the window.
- Drag it onto `Bxsim.exe` in Explorer.
- Press **O** (or click the start screen) to browse for one.

Supported formats are MP3, WAV, OGG and FLAC. When a song ends, Bxsim moves on to the next one in the same folder.

| Key | Action |
| --- | --- |
| Space | Play / pause |
| ← / → | Seek 5 s (hold Shift for 15 s) |
| ↑ / ↓ or mouse wheel | Volume |
| M (Shift+M for back) | Switch mode: Bars, Ring, Scope |
| N / P | Next / previous song in the folder |
| L | Loop the current song |
| O | Open a song |
| F11 or F | Fullscreen (Esc to leave) |
| Esc | Quit |

Click or drag on the progress bar to seek. The controls fade out while you watch and come back when you move the mouse.

## Building (VS Code on Windows)

You need:

- **Visual Studio 2026** with the "Desktop development with C++" workload, which provides the compiler.
- **CMake** and **Git** on your PATH.
- The VS Code extensions **CMake Tools** and **C/C++**.

Steps:

1. Open this folder in VS Code.
2. When CMake Tools asks for a kit, choose the **Visual Studio 2026 … amd64** one.
3. The first configure downloads and builds SFML, which takes a few minutes once.
4. Set the build variant to **Release** in the status bar, then click **Build**.
5. Click the **▶ Run** button in the status bar to launch it, or find the exe at `build/bin/Release/Bxsim.exe`.

Release builds are fully static: `Bxsim.exe` is a single file with no DLLs, so you can zip it and share it. Debug builds open a console window for log output.

## Code tour

- `src/Spectrum.*` — the FFT, log-spaced frequency bands, smoothing, peak caps and beat detection.
- `src/Renderer.*` — all drawing: the three modes, particles, bloom shader and on-screen UI.
- `src/Platform.*` — Windows bits: the file dialog, drag-and-drop and system fonts.
- `src/main.cpp` — window, playback and input.
