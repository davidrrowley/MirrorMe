# MirrorMe

MirrorMe is a tiny Windows desktop utility that mirrors one app window to another monitor with a tray-first workflow.

Built for quick sharing and second-screen setups: pick a source window, pick a target monitor, and mirror immediately.

I built MirrorMe for my own setup: an ultrawide screen plus a small Elgato Prompter. I often want to control an app on the ultrawide while seeing that same window on the Prompter, for example keeping Teams visible and usable while reading a script. 

My other usecase is when I need to present windows where I'm demonstrating something, on an ultrawide that becomes a juggle of switching shared windows in teams as sharing the ultrawide desktop looks terrible for remote viewers and is super painful and jarring for those watching.  With MirrorMe  I can keep the window on the ultrawide and share the secondary monitor (my Elgato prompter in this instance).  I can quickly switch to the app, duplicate to the monitor, do what I need to do then switch etc. until I'm done, no fuss, no drama and I get to work on my ultrawide - cool beans!

There are plenty of other uses I'm sure, but this tiny tool was created to solve these exact things for me.

## Screenshots

**MirrorMe lives quietly in the system tray, ready when you need it.**

![System tray icon](assets/System%20Tray.png)

**Pick a window, pick a monitor — the mirror appears full-screen instantly (bottom is my ultrawide, top is my Elgato Prompter).**

![Mirror in action across two monitors](assets/Mirror%20In%20Action.png)

**The notch popup gives quick access to zoom, transparency, display mode, and source selection.**

![MirrorMe notch menu](assets/MirrorMe%20Notch%20Menu.png)

## Project Status

- Active prototype with stable core functionality.
- Windows-only.
- Pre-built binaries available on the Releases page; source build also supported.

## Why It Is Tiny

- Does one job: mirror one window to one monitor.
- Fast controls from tray and notch popup.
- No installer required for local use.

## Features

- Native C++ Win32 app (no managed runtime dependency).
- Tray-first workflow with source and monitor selection.
- Fullscreen borderless mirror surface on selected monitor.
- Notch popup controls for quick in-window actions.
- Display modes:
  - Foreground Exclusive
  - Background Underlay
- Transparency control (0% to 100%).
- Zoom and pan controls.
  - `Ctrl+Scroll` Zoom in/out with the mouse wheel (floors at 1:1 fit-to-window)
- Global hotkeys:
  - `Ctrl+Alt+` Zoom in (hold to zoom continuously)
  - `Ctrl+Alt-` Zoom out (hold to zoom continuously; floors at 1:1 fit-to-window)
  - `Ctrl+Alt+0` Reset zoom and pan to 1:1
  - `Ctrl+Alt+R` Reset zoom and pan to 1:1 (alternate)
  - `Ctrl+Alt+Shift+M` Mirror current foreground window to second non-primary monitor; press again to stop mirroring
  - `Ctrl+Alt+D` Start Drawing, holding right mouse and drawing will generate a rectangle.  Selecting Ctrl+Alt+D again will clear any drawing on the screen.
  

## Download

Pre-built binaries are available on the [Releases](https://github.com/davidrrowley/MirrorMe/releases) page — no build required.

> **Windows SmartScreen warning:** Windows may warn that MirrorMe is from an unknown publisher. This is expected for an unsigned open-source tool. Click the dropdown arrow next to "Delete" and choose **Keep anyway**, then run the file. The full source code is available here to review.

## Quick Start

1. Download `MirrorMe.exe` from [Releases](https://github.com/davidrrowley/MirrorMe/releases) (or build from source below).
2. Run `MirrorMe.exe` — no installer needed.
3. Right-click the tray icon.
3. Select a source window.
4. Select a target monitor.
5. MirrorMe appears on the selected monitor.

## Build (Windows, Visual Studio)

### Configure

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

### Build

```powershell
cmake --build build --config Release
```

### Run

```powershell
.\build\Release\MirrorMe.exe
```

## Notes

- Phase 2 capture path now prefers Windows Graphics Capture + Direct3D11 for better latency and reliability.
- If WGC is unavailable or capture start fails, MirrorMe automatically falls back to Win32/GDI capture (PrintWindow with BitBlt fallback).
- Some protected or elevated-content windows may still block capture depending on OS and app security model.

## Known Limitations

- Capture can fail for DRM-protected, elevated, or security-hardened windows.
- No installer package yet (MSIX is planned for a later phase).
- No cross-platform support.

## Contributing

See CONTRIBUTING.md for development workflow and pull request expectations.

## Security

See SECURITY.md for responsible disclosure guidance.
