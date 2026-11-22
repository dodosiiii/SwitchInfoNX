# Changelog — SwitchInfoNX

All notable changes to SwitchInfoNX are documented here.  
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [1.0] — 2026-11-22 — Final Release

### Added
- **Overlay completely rewritten** — Tesla/Ultrahand native `.ovl` now shows live CPU / GPU / EMC clock speeds (MHz), operation mode (Docked / Handheld), battery health percentage, charger type, SD card free/total space, and brightness level.
- **Brightness cycling from the overlay** — press **[A]** on the Brightness row to cycle between Low / Medium / High / Max directly from the Ultrahand menu without opening the main app.
- **Power & Quick Actions section in overlay** — Reboot to Hekate/payload, Reboot Console, and Power Off Console actions are now available directly from the overlay with a single button press.
- **Clock services in overlay** (`clkrst`) — real-time CPU / GPU / EMC frequencies exposed via `clkrstGetClockRate`.
- **Brightness service in overlay** (`lbl`) — reads the current backlight level and allows cycling it without touching the main app.
- **Power services in overlay** (`spsm` / `bpc`) — clean reboot and shutdown from the overlay.
- **Hekate payload auto-discovery** — the overlay scans `/bootloader/update.bin`, `/bootloader/payloads/*.bin`, and `/atmosphere/reboot_payload.bin` to find the best reboot target.
- **QR code / transfer stats layout fix** — HTTP transfer statistics (count, total data, progress bar, detail text) moved to the left column (x = 510) so the QR code at the right edge never overlaps any text.
- **About page finalised** — "Planned for later" section replaced with a v1.0 completion confirmation (3 green lines per language stating 100 % complete, all tools active, no updates needed).
- **Performance: SD root scan cached** — `opendir("sdmc:/")` file/folder count is now throttled to one scan every 3 seconds instead of running every frame.
- **Performance: sys-clk detection cached** — `stat()` path check for the sys-clk sysmodule is cached for 5 seconds.
- **Performance: auto-brightness state cached** — `lblIsAutoBrightnessControlEnabled()` result is cached for 2 seconds.

### Changed
- **Overlay title** updated to `SwitchInfoNX` / `v1.0 | Overlay & Actions`.
- **Overlay Makefile** — `APP_TITLE` → `SwitchInfoNX`, `APP_VERSION` → `1.0.0`.
- **Main Makefile** — `VERSION` → `1.0`.
- **`APP_VERSION`** in `source/main.c` → `"1.0"`.
- Changelog section in the About page now describes the v1.0 feature set rather than a traditional diff list.
- Roadmap array reduced from 7 placeholder items to 3 green completion statements (all 11 languages).

### Fixed
- QR code no longer covers the "0 total" / transfer count text on the HTTP card.
- Duplicate `SDL_RenderSetClipRect` / scrollbar block at the end of `draw_pg7()` removed (caused a build error `expected ')' before '(' token`).
- `PsmChargerType` enum values corrected to match the devkitPro libnx version (`PsmChargerType_Unconnected`, `PsmChargerType_EnoughPower`, `PsmChargerType_LowPower`, `PsmChargerType_NotSupported`).
- `psmGetBatteryAgePercentage` now receives a `double *` argument instead of `u32 *`.

---

## [0.7] — HTTP Browser Transfer

### Added
- Opt-in HTTP browser server for SD-card browsing, downloads, and streamed uploads.
- HTTP sessions secured with a temporary random eight-digit access code.
- File paths rooted on the SD card; uploads staged before saving; existing files are never overwritten.
- Browser-transfer status card with address, access code, per-transfer progress, speed, and ETA.
- Shared chronological FTP / MTP / HTTP activity log.
- **A** toggles FTP, **X** toggles USB MTP, **Y** toggles HTTP.

---

## [0.6] — Anti-Ban, Crash Cleaner & Controller Colors

### Added
- **CFW Anti-Ban & Telemetry Shield** — live async DNS.mitm / 90DNS tests against Nintendo telemetry endpoints (`ctest`, `dauth-lp1`, `receive-lp1`).
- **Atmosphere Crash Dumps & Junk Cleaner** — scan and delete crash / fatal / erpt logs with reclaimed-space reporting.
- **Authentic Joy-Con Colors** — rendered from internal SPI flash with `#RRGGBB` hex badges and Central Power Dashboard.
- **Interactive HD Rumble Suite** — touch / button triggers for Left, Dual, and Right motors with active wave animation.
- **Stick Drift & Deadzone Live Analyzer** — real-time deviation %, neutral point precision, peak rest jitter, and Reset Peak button.

### Fixed / UI
- Card title dot accents, battery % badge in header, inactive tab hover highlights, modern scrollbar on Storage.
- Resolved `Environnement d'execution` text card overflow.
- Fixed battery power estimation label overlap.
- Fixed vertical spacing between Installed Titles, Running Games, and Game Launch Stats cards.
- Enabled display of running processes and system title IDs even when game title names are unmapped.
- Resolved game launch stats label truncation and display of empty/unknown entries.

---

## [0.5] — Save Manager, Overlay, Traffic Graph

### Added
- Game-save snapshots — browse dated backups and restore a selected one; live save backed up first.
- Ultrahand native `.ovl` — network monitor with live rates, session totals, 60-second history, connection details, reset, and CSV export.
- Step-by-step Ultrahand installation and transfer guide in Tools (11 languages).
- Live RX/TX rates and 60-second traffic graph for active system interfaces.
- Five-step first-run tutorial tracked in `tutorial.json`; replayable from Settings (11 languages).
- Blue analog-stick pointer with **A**-to-activate navigation.
- SD Space Analyzer — manual scan with progress, category totals, in-memory caching, and file-browser shortcuts.
- Vertical page scrolling with the right stick.
- Auto-brightness toggle in Settings.
- Ocean / Sunset / Forest quick palettes in the custom theme editor.
- Signal history at one-second samples; known-size transfers show estimated remaining time.
- 60-second min / average / max FPS summaries, battery-condition gauge, selected-file details, stick center references, live theme preview.

### Fixed
- Touch-adjustable auto-refresh and temperature/memory alert thresholds.
- SD storage usage bar alignment; fan and clock-scaling cards separated.
- Useful title ID shown when a game name cannot be read.
- Libnx error code included when a user profile cannot be read.
- Save-manager arrow taps no longer scroll the Tools page.
- Touch scrolling restored across the save-backup card.
- Browser interactions no longer trigger page scrolling.
- Cursor hint banner removed.

---

## [0.4] — Battery Health, Themes & FTP Polish

### Added
- Battery Health % and raw millivolt (mV) voltage diagnostics.
- Stick Drift detector and interactive deadzone visualizer.
- Structured JSON export (`sysinfo.json`).
- **OLED Pure Black** and **Cyberpunk Neon** themes (18 total).
- Modern floating translucent toast notifications with colored status pips.
- TCP connection-time measurement against the Nintendo test endpoint.
- Live FTP upload/download progress, byte count, and transfer rate.
- EmuMMC configuration-file/folder hint.

### Fixed
- Unified binary/decimal byte calculation (`format_bytes`) across File Browser, FTP, and MTP.
- Global `APP_VERSION` synchronised across UI, FTP daemon, and export banners.
- Primitive text arrows replaced with continuous vertical scrollbars.
- Tab bar label clipping fixed with pill navigation.
- Safe folder navigation and heap-allocated sort buffers in File Browser.
- Improved FTP client compatibility; explicit plain-FTP fallback; idle-session cleanup.
- Transfer activity log vertically aligned and clear of scrollbar.

---

## [0.3] and earlier

Initial releases establishing the core dashboard: system info, storage, network, performance graphs, controller diagnostics, FTP/MTP transfers, and the first theme system.

---

> Nintendo Switch is a trademark of Nintendo Co., Ltd. SwitchInfoNX is an unofficial homebrew project.
