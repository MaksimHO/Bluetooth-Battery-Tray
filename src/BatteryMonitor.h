#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct DeviceBatteryInfo {
    std::wstring deviceName;
    std::wstring deviceAddress;
    int batteryPercent; // 0-100, or -1 if unknown
    bool isConnected = false;
};

std::vector<DeviceBatteryInfo> ScanBluetoothDevicesBattery();

// Posts `message` to `hWnd` when a paired Bluetooth device is added, removed or
// changes its connection state, so the UI can refresh without waiting for the
// periodic poll. Call from a COM-initialized thread (the UI thread is fine).
bool StartBluetoothDeviceWatcher(HWND hWnd, UINT message);
void StopBluetoothDeviceWatcher();
