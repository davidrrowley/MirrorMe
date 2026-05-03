# MirrorMe

MirrorMe is a tiny Windows desktop utility that mirrors one app window to another monitor with a tray-first workflow.

Built for quick sharing and second-screen setups: pick a source window, pick a target monitor, and mirror immediately.

I built MirrorMe for my own setup: an ultrawide screen plus a small Elgato Prompter. I often want to control an app on the ultrawide while seeing that same window on the Prompter, for example keeping Teams visible and usable while reading a script. There are plenty of other uses I'm sure, but this tiny tool was created to solve this exact thing for me.

## Project Status

- Active prototype with stable core functionality.
- Windows-only.
- Local build and run flow is the current distribution path.

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
- Global hotkeys:
  - `Ctrl+Alt++` Zoom in
  - `Ctrl+Alt+-` Zoom out
  - `Ctrl+Alt+0` Reset zoom
  - `Ctrl+Alt+R` Reset zoom (alternate)
  - `Ctrl+Alt+Shift+M` Mirror current foreground window to default monitor

## Quick Start

1. Build and run MirrorMe.
2. Right-click the tray icon.
3. Select a source window.
4. Select a target monitor.
5. MirrorMe appears on the selected monitor.

## Tiny Tool Town Submission Notes

- Tool Name: MirrorMe
- Author: David Rowley (@davidrrowley)
- License: GPL-3.0
- Language: C++
- Platform: Windows
- Suggested tags: `windows, monitor, screen-mirroring, tray, desktop-utility, cplusplus`
- Alternate tags (utility-focused): `windows, desktop, monitor, mirroring, tray, cplusplus`
- Alternate tags (playful-focused): `windows, tiny, fun, second-screen, tray, cplusplus`
- Suggested theme: `terminal` (alternate: `ocean`)

Add a hero screenshot or short GIF near the top of this README before submitting so Tiny Tool Town can pick up a strong preview image.

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
