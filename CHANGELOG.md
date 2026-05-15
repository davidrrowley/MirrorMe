# Changelog

All notable changes to this project will be documented in this file.

## [1.1.3] - 2026-05-15

### Fixed

- Idle notch now renders with correct flat-top/round-bottom pill shape; black rectangular corners behind the pill are eliminated
- Shadow silhouette no longer visible at rest; window region shrinks to an invisible peek strip when the notch is fully hidden
- Notch smoothly restores its full shape when hovered from the hidden state
- KVM / capture-card reconnect: idle notch reliably reappears on the secondary monitor after disconnect/reconnect cycles

## [1.1.2] - 2026-05-14

### Fixed

- Mirroring now stops automatically when the source window is minimised or closed

## [1.1.1] - 2026-05-08

### Changed

- App now always starts minimised to the system tray; removed the redundant "Start Minimised To Tray" toggle
- First-run experience: balloon notification and notch peek guide new users to the tray icon on first launch

## [1.1.0] - 2026-05-08

### Fixed

- Resolved window discovery issues on ARM (WhatsApp and other apps were incorrectly excluded)

### Added

- Drawing capabilities: freehand pen and rectangle annotation overlay with colour and size picker, toggled via notch bar or Ctrl+Alt+D

## [Unreleased]

### Added

- Tray-first source and monitor selection workflow
- Display modes: Foreground Exclusive and Background Underlay
- Transparency controls
- Global hotkeys for zoom and mirror-foreground workflow
- Notch popup drill-down navigation

### Changed

- In-app Exit now hides to tray; tray Exit fully closes app
- Startup and visibility behavior aligned with tray-first usage
- README and contributor-facing documentation updated for current behavior
