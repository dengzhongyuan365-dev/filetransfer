# Changelog

All notable changes to LAN File Transfer are documented here.

## v0.2.0 - 2026-07-02

### Added

- Added a Qt GUI focused on device discovery, connection confirmation, drag-and-drop transfer, tray operation, settings, receive history, and debug logs.
- Added multi-device linking with selectable send targets.
- Added transfer scheduling with global and per-device concurrency limits; extra tasks queue instead of starting all at once.
- Added per-device transfer views so each linked machine has its own task list context.
- Added clipboard sending for copied files, folders, and screenshot images.
- Added receiver-side clipboard restore for transferred clipboard images, plus a notification when the image is ready to paste.
- Added Debian packaging assets: desktop entry, SVG app icon, Chinese translation, package script, and package content verification.
- Added documentation for packaging, protocol details, development history, and technical roadmap.

### Changed

- Improved the GUI layout for a compact 400x600 style window.
- Moved debug logs out of the transfer page and into settings.
- Moved global linked-device count to the device discovery page.
- Improved dark theme styling and guarded style refreshes to avoid recursive Qt stylesheet updates.
- Improved transfer progress reporting and logging during scanning, planning, sending, receiving, and failures.
- Directory scanning now avoids precomputing every file hash, reducing long stalls before transfer starts.

### Fixed

- Fixed receiver startup and TCP connection visibility issues with clearer logs.
- Fixed task stop, pause, resume, clear, and remove semantics so clearing a card is separate from stopping work.
- Fixed directory transfer root preservation so sending a folder keeps the top-level folder name.
- Fixed settings dialog layout clipping on narrow themes and localized builds.
- Fixed translation loading from installed package paths.

## v0.1.0 - 2026-06-30

### Added

- Initial LAN file transfer prototype.
- Added CLI sender, receiver, and local copy tools.
- Added TCP frame protocol, single-file transfer, SHA256 verification, `.part` files, resume support, and basic directory synchronization.
- Added CMake build, unit tests, and initial CPack Debian package generation.
