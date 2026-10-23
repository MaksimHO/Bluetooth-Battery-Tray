# Releasing

Public binaries live in **GitHub Releases**. The `dist/` folder keeps only the
current build for convenience, so the working tree stays small; older versions
are available from the releases page.

The README links to `/releases/latest`, so it does not need editing for every
version.

## 1. Bump the version

Update all three places:

- `resources/resource.rc` — `FileVersion` / `ProductVersion`
- `installer/BluetoothBatteryTray.iss` — `AppVersion` and `VersionInfoVersion`
- file names below (`...-1.1.0.exe`, `...-portable-1.1.0.zip`)

Also update the file names mentioned in `README.md` and `README-RU.txt`.

## 2. Build

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ISCC.exe installer\BluetoothBatteryTray.iss
```

## 3. Build the portable zip

```powershell
$stage = "dist\portable"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item build\Release\BluetoothBatteryTray.exe $stage
Copy-Item README-RU.txt $stage
Compress-Archive -Path "$stage\*" -DestinationPath dist\BluetoothBatteryTray-portable-1.1.0.zip -Force
Remove-Item $stage -Recurse -Force
```

## 4. Commit and publish

1. Commit the new `dist/` files and push to `main`.
2. Create a release tagged `vX.Y.Z` (same version as the binaries) and attach
   both files from `dist/`.
3. Verify that `https://github.com/MaksimHO/Bluetooth-Battery-Tray/releases/latest`
   points to the new release and that the asset hashes match the local files.

Old versions do not need to be kept in `dist/`; replace them with the new build.

## Notes

- The installer and exe are not code-signed, so SmartScreen shows a warning on
  first launch.
- Git history keeps every committed binary forever. If the repository size
  becomes a problem, stop committing `dist/` altogether and link only to the
  releases page.
