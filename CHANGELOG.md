# Changelog

All notable changes to this project will be documented in this file.

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
