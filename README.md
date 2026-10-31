<div align="center">
  <img src="switchinfo_icon.png" width="96" alt="SwitchInfoNX icon"/>
  <h1>SwitchInfoNX</h1>
  <p><strong>Premium system information & hardware diagnostic utility for Nintendo Switch homebrew</strong></p>
  <p>
    <img src="https://img.shields.io/badge/version-0.4-blue?style=flat-square"/>
    <img src="https://img.shields.io/badge/platform-Nintendo%20Switch-e60012?style=flat-square"/>
    <img src="https://img.shields.io/badge/build-devkitPro%20A64-40aef0?style=flat-square"/>
  </p>
  <br>
</div>

---

**Tiny description:**  
> All-in-one hardware monitor, diagnostics, system info & file transfer tool for the Nintendo Switch.

**Long description:**  
SwitchInfoNX is a feature-rich homebrew application that turns your Nintendo Switch into a diagnostics workstation. It provides a real-time overview of CPU/GPU/memory clocks, FPS, RAM & storage space, thermal readings, Joy-Con drift diagnostics, network latency, MTP & FTP transfer servers, and hardware detection (EmuMMC/SysNAND, battery health & voltages).

---

##  Features (v0.4)

###  System Information & Deep Hardware
* **Firmware & Hardware**: Firmware version, serial number, hardware type (Erista V1 / Mariko V2 / Switch Lite / Switch OLED), region, system language, device nickname.
* **Environment Detection**: Real-time detection of **EmuMMC (Active)** vs **SysNAND**.
* **Battery Health Diagnostics**: Live battery percentage, charging state, **Battery Health %** estimation, raw voltage in **mV**, and real-time charge/discharge current (**mA**).
* **Thermal Monitoring**: Skin & SoC internal temperature readings, dynamic color coding (Normal / Warm / Hot), thermal threshold alerts.

###  Storage & File Browser
* **Storage Insights**: SD card usage, NAND partitions (USER / SYSTEM / SAFE), SD read speed test benchmark.
* **Integrated File Browser**: Full SD card file manager with unified byte calculations (B/KB/MB/GB), folder navigation, copy/cut/paste, rename, and file deletion.

###  Network & Latency Diagnostics
* **Network Status**: IP & MAC addresses, connection type, SSID, Wi-Fi signal strength history graph (60s).
* **Speed & Latency**: Real-time latency measurement (**ping in ms** to public DNS), download/upload throughput test.

###  Performance & Hardware Gauges
* **Live Clock Speeds**: Real-time CPU, GPU, and EMC memory clock frequencies.
* **Performance Graphs**: 60-frame live FPS chart, RAM usage gauge, CPU/GPU load score, and historical thermal tracking.
* **Clock Scaling Controls**: Manual CPU/GPU clock target adjustment sliders.

###  Controller Diagnostics & Stick Drift Detector
* **Interactive Controller Test**: Live button state mapping, analog trigger visualization, gyroscope & accelerometer 6-axis readout.
* **Stick Drift & Deadzone Visualizer**: Circular analog stick bounds with deadzone overlay, drift deviation percentage, and center state indicator.

###  Transfer Protocols
* **MTP USB Transfer**: Built-in Media Transfer Protocol server over USB for high-speed drag-and-drop file transfers with PC.
* **Wi-Fi FTP Server**: Dual-mode FTP / FTPD server with compatible command handling, reliable uploads and folder creation, tuned transfer buffers, and live progress / speed feedback on the Switch.

###  Modern UI & Custom Themes
* **Modern Dashboard**: Sleek pill-based navigation tab bar, high-definition cards, continuous vertical scrollbars, and floating translucent toast alerts.
* **17 Built-in Presets**: Including **OLED Pure Black** (true black `#000000` power-saving mode) and **Cyberpunk Neon**, plus a full 10-slot real-time RGB custom theme editor.
* **Internationalization**: 11 languages supported (English, French, Italian, Spanish, German, Portuguese, Dutch, Japanese, Russian, Chinese, Korean).

---

##  Installation

1. Download the latest `SwitchInfoNX.nro` from [Releases](https://github.com/dodosiiii/SwitchInfoNX/releases)
2. Copy it to `sdmc:/switch/SwitchInfoNX/` on your Switch SD card
3. Launch via any homebrew launcher (Atmosphère, HBMenu, etc.)

---

##  Building from source

### Prerequisites

Install [devkitPro](https://devkitpro.org/) with devkitA64 and Switch portlibs:

```bash
sudo dkp-pacman -S switch-dev switch-sdl2 switch-sdl2_ttf switch-sdl2_gfx \
  switch-freetype switch-harfbuzz switch-libpng switch-bzip2 switch-glm
```

### Build

```bash
git clone https://github.com/dodosiiii/SwitchInfoNX.git
cd SwitchInfoNX
make
```

The output `SwitchInfoNX.nro` will be placed in the project root directory.

---

##  Usage

| Button | Action |
|--------|--------|
| **← →** / **Touch Tabs** | Navigate tabs (Pills navigation) |
| **↑ ↓** | Scroll within pages (System, Storage, Network, Perf, Controller, About, Tools, Settings) |
| **Y** | Force refresh / export perf metrics |
| **L / R** | Previous / next tab |
| **B** | Return / Export system report (in Tools) |
| **ZL + ZR** | Instant screenshot captured to SD card |
| **Touch & drag** | Smooth page scrolling, sliders adjustment (brightness, theme RGB) |

---

##  Configuration

Settings are saved to `sdmc:/switch/SwitchInfoNX/config.txt`:

* **Language** — select among 11 languages
* **Theme** — 17 built-in presets or custom RGB palette
* **Auto-refresh** — Off / 1s / 3s / 5s / 10s
* **Temperature alert** — custom threshold (default 70.0 °C)
* **Memory alert** — custom threshold (default 85 %)

Diagnostic reports are automatically exported to:
* `sdmc:/switch/SwitchInfoNX/system_report.txt` (Human readable report)
* `sdmc:/switch/SwitchInfoNX/sysinfo.json` (Structured JSON diagnostics)

---

##  Tech Stack

| Component | Library |
|-----------|---------|
| Graphics | SDL2 + SDL2_ttf + SDL2_gfx |
| Fonts | FreeType + HarfBuzz (Nintendo Switch shared system font) |
| Hardware APIs | libnx (`switch.h`, `setsys`, `psm`, `clkrst`, `tc`, `hid`, `nifm`) |
| Transfer Stack | Native MTP C++ implementation + FTP socket server with tuned TCP buffers |
| Toolchain | devkitA64 (`aarch64-none-elf-gcc`) |

---

##  Changelog v0.4

* **Fix**: Unified binary/decimal byte calculation (`format_bytes`) across File Browser, FTP, and MTP.
* **Fix**: Synchronized global `APP_VERSION` definition throughout UI, FTP daemon, and export banners.
* **Fix**: Replaced primitive text arrows with modern continuous vertical scrollbars.
* **Fix**: Tab bar label clipping resolved with modern capsule "pill" navigation.
* **Fix**: Safe folder navigation and heap-allocated sort buffers in File Browser.
* **New**: Real-time **EmuMMC vs SysNAND** environment detection.
* **New**: **Battery Health %** and raw millivolt (**mV**) voltage diagnostics.
* **New**: **Stick Drift detector** & interactive deadzone visualizer on Controller page.
* **New**: Structured **JSON export** (`sysinfo.json`) alongside standard text report.
* **New**: 2 brand-new themes: **OLED Pure Black** and **Cyberpunk Neon** (18 themes total).
* **New**: Modern floating translucent toast notification badges with colored status pips.
* **New**: Public DNS network latency measurement (**ping in ms**).
* **Fix**: Improved FTP client compatibility, including explicit plain-FTP fallback when clients request unsupported TLS and idle-session cleanup.
* **Performance**: Increased FTP TCP and transfer buffers; verified a 9.4 MB upload in about 3 seconds on the tested network.
* **New**: Live FTP upload/download progress, byte count, and transfer rate on the Transfer page; percentage is shown when the client announces the file size.
* **UI**: Tightened and vertically aligned the Transfer activity log to keep its latest entries clear of the scrollbar and card edge.

---

<div align="center">
  <sub>Built with  by dodosi</sub>
  <br>
  <sub>Nintendo Switch is a trademark of Nintendo Co., Ltd. This is an unofficial homebrew project.</sub>
</div>
