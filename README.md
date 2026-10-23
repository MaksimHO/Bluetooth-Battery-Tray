# Bluetooth Battery Tray

A lightweight Windows tray app that shows the battery level of your Bluetooth
devices — one tray icon per device, each with its own picture, updated live.

Works with BLE peripherals (mice, keyboards, earbuds) and classic Bluetooth
devices such as HFP headsets. No admin rights required.

> Русская инструкция: [README-RU.txt](README-RU.txt)

## Features

- **Per-device tray icons** — every connected device gets its own icon with a
  battery bar and percentage in the tooltip.
- **Custom images** — pick any PNG, JPG, BMP, ICO or GIF for a device, it is
  drawn above the battery bar. Images are tied to the device's MAC address and
  survive reconnects and restarts.
- **Live updates** — connect/disconnect is picked up immediately through WinRT
  `DeviceWatcher`; battery values are refreshed on a 30-second timer.
- **Stable tray positions** — each device keeps a persistent tray icon ID, so a
  pinned icon returns to the same spot after reconnecting or restarting the app.
- **Explorer-restart safe** — icons are re-registered on the shell's
  `TaskbarCreated` broadcast.
- **Light and dark themes** — the device list follows the Windows app theme and
  switches live when the setting changes; the tray menu follows as well.
- **Device list window** — all paired devices in one list, including devices
  that are currently offline; only connected ones appear in the tray.
- **Single instance** — launching the exe again just opens the device list of
  the running copy.
- **Portable** — statically linked C runtime, no Visual C++ Redistributable
  needed.

## Requirements

- Windows 10 or Windows 11, 64-bit
- A working Bluetooth adapter
- No administrator rights

## Install

### Installer

Download
[`BluetoothBatteryTray-Setup-1.0.0.exe`](dist/BluetoothBatteryTray-Setup-1.0.0.exe) and
run it. The installer is per-user (installs to
`%LOCALAPPDATA%\Programs\Bluetooth Battery Tray`) and offers two optional tasks:

- start with Windows;
- create a desktop shortcut.

### Portable

Download
[`BluetoothBatteryTray-portable-1.0.0.zip`](dist/BluetoothBatteryTray-portable-1.0.0.zip),
unpack it anywhere and run `BluetoothBatteryTray.exe`. Autostart is not
configured in portable mode — add a shortcut to `shell:startup` if you want it.

> The binaries are not code-signed, so SmartScreen may show a warning on first
> launch. Choose "More info" → "Run anyway".

## Usage

- **Double-click** a tray icon — open the device list.
- **Right-click** a tray icon — menu with the current devices, `Show Devices…`,
  `Refresh Now`, `Exit`.
- In the device list:
  - `Refresh` — force an immediate scan;
  - `Set image…` — choose a custom picture for the device;
  - `Clear image` — go back to the default icon.

The picture tile is drawn above the battery bar. Assignments are stored per MAC
address, so they follow the device across reconnects.

When no device is connected, a single neutral icon stays in the tray so the menu
remains reachable.

## Pinning icons in the taskbar

Each device icon uses a persistent ID. If you promote it once in
**Settings → Personalization → Taskbar → Other system tray icons**, it will come
back to the same place after a reconnect instead of being hidden again.

## Where settings are stored

| What | Location |
| --- | --- |
| Custom image paths | `HKCU\Software\BluetoothBatteryTray\DeviceIcons` |
| Persistent tray icon IDs | `HKCU\Software\BluetoothBatteryTray\TrayIds` |
| Copies of custom images | `%LOCALAPPDATA%\BluetoothBatteryTray\icons` |

The uninstaller removes these keys and the image folder.

## How the battery level is read

The app tries several sources and keeps the first value it finds:

1. **BLE GATT Battery Service** — `0x180F` / `0x2A19`.
2. **Classic HFP** — PnP property set
   `{104EA319-6EE2-4701-BD47-8DDBF425BBE5}`, PID 2, on the
   `Hands-Free AG` device node (the same value the Windows Settings app shows).
3. **Windows AEP** — `System.Devices.BatteryLife` and
   `System.Devices.BatteryPlusCharging`.
4. **Registry fallbacks** — `BTHPORT\Parameters\Devices\<MAC>` and
   `Software\Microsoft\Bluetooth\Device\<MAC>`.

Some devices simply do not report a level; for those the tooltip shows `N/A`.

## Build from source

Requirements:

- Visual Studio 2022 (Desktop development with C++) or Build Tools
- CMake 3.20+

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable is produced at `build\Release\BluetoothBatteryTray.exe`.
The CRT is linked statically (`MultiThreaded`), so the exe runs on a clean
Windows installation.

## Build the installer

Install [Inno Setup 6](https://jrsoftware.org/isinfo.php), then:

```bat
ISCC.exe installer\BluetoothBatteryTray.iss
```

The setup executable lands in `dist\`. The script is per-user
(`PrivilegesRequired=lowest`), supports English and Russian, and offers the
autostart and desktop-shortcut tasks.

## Regenerate the icon

`resources/app.ico` is generated from `resources/app-source.png`
(512×512 RGBA) and contains 16, 20, 24, 32, 40, 48, 64, 128 and 256 px frames:

```bat
powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
```

The script uses `System.Drawing` to resize the source and writes both the DIB
frames and the alpha mask, so every size stays sharp and transparent.

## Project structure

```
src/main.cpp              tray icons, rendering, device list window, message loop
src/BatteryMonitor.cpp    device scan pipeline, battery sources, DeviceWatcher
src/BatteryMonitor.h      DeviceBatteryInfo and watcher API
src/DarkMode.cpp          light/dark theme following the Windows setting
src/DarkMode.h            dark mode helpers
resources/app.ico         multi-size application icon
resources/app-source.png  icon source image
resources/resource.rc     icon and version resource
installer/                Inno Setup script
tools/make-icon.ps1       PNG → multi-size ICO converter
CMakeLists.txt            build configuration
```

## Notes

- Battery reporting depends on the device firmware. Headsets that only expose
  battery over a proprietary app may still show `N/A`.
- The app polls battery values every 30 seconds but reacts to connection changes
  immediately, so plugging in a device updates the tray within a moment.
