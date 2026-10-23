#include "BatteryMonitor.h"
#include <windows.h>
#include <setupapi.h>
#include <devpkey.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "setupapi.lib")

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Storage::Streams;

static const guid BATTERY_SERVICE_UUID{ 0x0000180F, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };
static const guid BATTERY_LEVEL_UUID{ 0x00002A19, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };

// Scan runs on a worker thread, so every call must initialize COM for that thread.
// MTA is what we want: WinRT async objects are then usable from any MTA thread.
static void InitApartment()
{
    try {
        init_apartment(apartment_type::multi_threaded);
    }
    catch (hresult_error const&) {
        // Already initialized on this thread with another model - keep going.
    }
}

// --- Connection state notifications -----------------------------------------
// Polling every 30 s makes the tray lag behind reality. A DeviceWatcher raises
// an Updated event as soon as System.Devices.Aep.IsConnected changes, which is
// exactly what connect/disconnect produces for both classic and LE devices.
namespace {

struct WatcherSubscriptions {
    DeviceWatcher watcher{ nullptr };
    event_token added{};
    event_token removed{};
    event_token updated{};
    event_token completed{};
    event_token stopped{};
};

WatcherSubscriptions g_classicWatcher;
WatcherSubscriptions g_bleWatcher;
std::atomic<HWND> g_watchHwnd{ nullptr };
std::atomic<UINT> g_watchMessage{ 0 };

void NotifyWatcher()
{
    const HWND hWnd = g_watchHwnd.load();
    const UINT message = g_watchMessage.load();
    if (hWnd && message)
        PostMessageW(hWnd, message, 0, 0);
}

bool IsConnectionUpdate(DeviceInformationUpdate const& update)
{
    try {
        return update.Properties().HasKey(L"System.Devices.Aep.IsConnected");
    }
    catch (...) {
        return false;
    }
}

void StopWatcher(WatcherSubscriptions& subscriptions)
{
    if (!subscriptions.watcher)
        return;

    try { subscriptions.watcher.Stop(); } catch (...) {}
    try {
        subscriptions.watcher.Added(subscriptions.added);
        subscriptions.watcher.Removed(subscriptions.removed);
        subscriptions.watcher.Updated(subscriptions.updated);
        subscriptions.watcher.EnumerationCompleted(subscriptions.completed);
        subscriptions.watcher.Stopped(subscriptions.stopped);
    }
    catch (...) {}
    subscriptions.watcher = nullptr;
}

bool StartWatcher(WatcherSubscriptions& subscriptions, hstring const& selector,
    DeviceInformationKind kind, bool explicitKind)
{
    try {
        std::vector<hstring> propertyList{
            L"System.Devices.Aep.DeviceAddress",
            L"System.Devices.Aep.IsConnected",
            L"System.Devices.BatteryLife",
            L"System.Devices.BatteryPlusCharging",
        };
        auto properties = single_threaded_vector<hstring>(std::move(propertyList));

        subscriptions.watcher = explicitKind
            ? DeviceInformation::CreateWatcher(selector, properties, kind)
            : DeviceInformation::CreateWatcher(selector, properties);

        subscriptions.added = subscriptions.watcher.Added(
            [](DeviceWatcher const&, DeviceInformation const&) { NotifyWatcher(); });
        subscriptions.removed = subscriptions.watcher.Removed(
            [](DeviceWatcher const&, DeviceInformationUpdate const&) { NotifyWatcher(); });
        subscriptions.updated = subscriptions.watcher.Updated(
            [](DeviceWatcher const&, DeviceInformationUpdate const& update) {
                if (IsConnectionUpdate(update))
                    NotifyWatcher();
            });
        subscriptions.completed = subscriptions.watcher.EnumerationCompleted(
            [](DeviceWatcher const&, IInspectable const&) {});
        subscriptions.stopped = subscriptions.watcher.Stopped(
            [](DeviceWatcher const&, IInspectable const&) {});

        subscriptions.watcher.Start();
        return true;
    }
    catch (...) {
        StopWatcher(subscriptions);
        return false;
    }
}

} // namespace

bool StartBluetoothDeviceWatcher(HWND hWnd, UINT message)
{
    if (!hWnd || message == 0)
        return false;

    InitApartment();
    g_watchHwnd = hWnd;
    g_watchMessage = message;

    const bool classic = StartWatcher(g_classicWatcher,
        BluetoothDevice::GetDeviceSelectorFromPairingState(true),
        DeviceInformationKind::AssociationEndpoint, true);

    const bool ble = StartWatcher(g_bleWatcher,
        BluetoothLEDevice::GetDeviceSelectorFromPairingState(true),
        DeviceInformationKind::AssociationEndpoint, false);

    if (!classic && !ble) {
        StopBluetoothDeviceWatcher();
        return false;
    }
    return true;
}

void StopBluetoothDeviceWatcher()
{
    StopWatcher(g_classicWatcher);
    StopWatcher(g_bleWatcher);
    g_watchHwnd = nullptr;
    g_watchMessage = 0;
}

// C++/WinRT has no built-in timeout for IAsyncOperation. A stuck GATT call would
// otherwise block the scan thread forever and freeze all further refreshes.
template <typename T>
static bool WaitFor(IAsyncOperation<T> const& op, DWORD timeoutMs)
{
    const ULONGLONG start = GetTickCount64();
    while (op.Status() == AsyncStatus::Started) {
        if (GetTickCount64() - start > timeoutMs) {
            try { op.Cancel(); } catch (...) {}
            return false;
        }
        Sleep(15);
    }
    return op.Status() == AsyncStatus::Completed;
}

static std::wstring MacFromDeviceId(std::wstring const& id)
{
    std::wstring hex;
    for (auto it = id.rbegin(); it != id.rend(); ++it) {
        const wchar_t c = *it;
        if (iswxdigit(c)) {
            hex.push_back(static_cast<wchar_t>(towupper(c)));
            if (hex.size() == 12)
                break;
        }
        else if (c == L':' || c == L'-') {
            continue;
        }
        else {
            break;
        }
    }
    std::reverse(hex.begin(), hex.end());
    return hex;
}

static std::wstring NormalizeMac(std::wstring const& value)
{
    std::wstring hex;
    for (wchar_t c : value) {
        if (iswxdigit(c))
            hex.push_back(static_cast<wchar_t>(towupper(c)));
    }
    if (hex.size() > 12)
        hex = hex.substr(hex.size() - 12);
    return hex;
}

static int BatteryFromAep(DeviceInformation const& di)
{
    static const wchar_t* const names[] = {
        L"System.Devices.BatteryLife",
        L"System.Devices.BatteryPlusCharging",
    };

    for (auto name : names) {
        try {
            auto value = di.Properties().Lookup(name);
            if (!value)
                continue;

            int percent = -1;
            if (auto r = value.try_as<IReference<uint8_t>>())
                percent = static_cast<int>(r.Value());
            else if (auto r = value.try_as<IReference<uint16_t>>())
                percent = static_cast<int>(r.Value());
            else if (auto r = value.try_as<IReference<uint32_t>>())
                percent = static_cast<int>(r.Value());
            else if (auto r = value.try_as<IReference<int32_t>>())
                percent = r.Value();

            if (percent >= 0 && percent <= 100)
                return percent;
        }
        catch (...) {}
    }
    return -1;
}

static bool ConnectedFromAep(DeviceInformation const& di)
{
    try {
        auto value = di.Properties().Lookup(L"System.Devices.Aep.IsConnected");
        if (!value)
            return false;
        if (auto r = value.try_as<IReference<bool>>())
            return r.Value();
        return unbox_value<bool>(value);
    }
    catch (...) {
        return false;
    }
}

static int QueryBatteryFromRegistry(std::wstring const& macAddrClean)
{
    if (macAddrClean.empty())
        return -1;

    struct RegPath {
        HKEY root;
        std::wstring path;
        std::wstring valueName;
    };

    const RegPath paths[] = {
        { HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices\\" + macAddrClean, L"BatteryLevel" },
        { HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices\\" + macAddrClean, L"Battery" },
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Bluetooth\\Device\\" + macAddrClean, L"BatteryPercent" },
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Bluetooth\\Device\\" + macAddrClean, L"Battery" },
    };

    for (auto const& p : paths) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(p.root, p.path.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
            continue;

        DWORD type = 0, data = 0, size = sizeof(DWORD);
        const LSTATUS status = RegQueryValueExW(hKey, p.valueName.c_str(), nullptr, &type,
            reinterpret_cast<LPBYTE>(&data), &size);
        RegCloseKey(hKey);

        if (status == ERROR_SUCCESS && type == REG_DWORD && data <= 100)
            return static_cast<int>(data);
    }

    return -1;
}

// Windows receives the battery level of classic headsets over HFP and caches it
// on the "Hands-Free AG" device node. PID 2 of this property set is the
// percentage (BYTE), PID 7 is the last report time (FILETIME).
// Verified against Settings > Bluetooth on Windows 11 24H2.
static const DEVPROPKEY kBluetoothBatteryLevel = {
    { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2
};

static std::vector<std::pair<std::wstring, int>> QueryHfpBatteriesFromPnp()
{
    // Uppercased instance id (contains the device MAC) -> battery percentage.
    std::vector<std::pair<std::wstring, int>> batteries;

    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE)
        return batteries;

    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA data = {};
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInfo(devices, index, &data))
            break;

        wchar_t instanceId[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(devices, &data, instanceId,
                static_cast<DWORD>(std::size(instanceId)), nullptr))
            continue;

        union {
            BYTE bytes[16];
            USHORT u16;
            UINT u32;
            INT i32;
        } level = {};
        DEVPROPTYPE type = 0;
        DWORD needed = 0;
        if (!SetupDiGetDevicePropertyW(devices, &data, &kBluetoothBatteryLevel,
                &type, level.bytes, sizeof(level.bytes), &needed, 0))
            continue;

        int percent = -1;
        switch (type) {
        case DEVPROP_TYPE_BYTE:
            percent = level.bytes[0];
            break;
        case DEVPROP_TYPE_UINT16:
            percent = level.u16;
            break;
        case DEVPROP_TYPE_UINT32:
            percent = static_cast<int>(level.u32);
            break;
        case DEVPROP_TYPE_INT32:
            percent = level.i32;
            break;
        default:
            break;
        }

        if (percent < 0 || percent > 100)
            continue;

        std::wstring upperId(instanceId);
        for (auto& c : upperId)
            c = static_cast<wchar_t>(towupper(c));
        batteries.emplace_back(std::move(upperId), percent);
    }

    SetupDiDestroyDeviceInfoList(devices);
    return batteries;
}

static int ReadGattBattery(BluetoothLEDevice const& bleDevice)
{
    try {
        auto servicesOp = bleDevice.GetGattServicesForUuidAsync(BATTERY_SERVICE_UUID, BluetoothCacheMode::Uncached);
        if (!WaitFor(servicesOp, 8000))
            return -1;

        auto servicesResult = servicesOp.GetResults();
        if (servicesResult.Status() != GattCommunicationStatus::Success)
            return -1;

        auto services = servicesResult.Services();
        if (services.Size() == 0)
            return -1;

        auto charsOp = services.GetAt(0).GetCharacteristicsForUuidAsync(BATTERY_LEVEL_UUID, BluetoothCacheMode::Uncached);
        if (!WaitFor(charsOp, 8000))
            return -1;

        auto charsResult = charsOp.GetResults();
        if (charsResult.Status() != GattCommunicationStatus::Success)
            return -1;

        auto chars = charsResult.Characteristics();
        if (chars.Size() == 0)
            return -1;

        auto readOp = chars.GetAt(0).ReadValueAsync(BluetoothCacheMode::Uncached);
        if (!WaitFor(readOp, 8000))
            return -1;

        auto readResult = readOp.GetResults();
        if (readResult.Status() != GattCommunicationStatus::Success)
            return -1;

        auto reader = DataReader::FromBuffer(readResult.Value());
        if (reader.UnconsumedBufferLength() < 1)
            return -1;

        const uint8_t level = reader.ReadByte();
        return level <= 100 ? static_cast<int>(level) : -1;
    }
    catch (...) {
        return -1;
    }
}

static bool FindByAddress(std::vector<DeviceBatteryInfo> const& list, std::wstring const& address)
{
    if (address.empty())
        return false;
    for (auto const& d : list) {
        if (NormalizeMac(d.deviceAddress) == address)
            return true;
    }
    return false;
}

std::vector<DeviceBatteryInfo> ScanBluetoothDevicesBattery()
{
    InitApartment();

    std::vector<DeviceBatteryInfo> results;

    // --- 1. Paired BLE devices: GATT Battery Service (0x180F / 0x2A19) ---
    try {
        auto infos = DeviceInformation::FindAllAsync(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)).get();

        for (auto const& di : infos) {
            DeviceBatteryInfo info;
            info.deviceName = di.Name().c_str();
            info.deviceAddress = MacFromDeviceId(di.Id().c_str());
            info.batteryPercent = -1;
            info.isConnected = false;

            try {
                auto op = BluetoothLEDevice::FromIdAsync(di.Id());
                if (WaitFor(op, 5000)) {
                    auto bleDevice = op.GetResults();
                    if (bleDevice) {
                        info.isConnected = bleDevice.ConnectionStatus() == BluetoothConnectionStatus::Connected;
                        if (info.isConnected)
                            info.batteryPercent = ReadGattBattery(bleDevice);
                    }
                }
            }
            catch (...) {}

            if (!info.deviceName.empty() || info.batteryPercent >= 0)
                results.push_back(info);
        }
    }
    catch (...) {}

    // --- 2. Paired classic Bluetooth devices: AEP properties, then LE-by-address ---
    try {
        std::vector<hstring> properties{
            L"System.Devices.Aep.DeviceAddress",
            L"System.Devices.Aep.IsConnected",
            L"System.Devices.BatteryLife",
            L"System.Devices.BatteryPlusCharging",
        };

        auto infos = DeviceInformation::FindAllAsync(
            BluetoothDevice::GetDeviceSelectorFromPairingState(true),
            single_threaded_vector<hstring>(std::move(properties)),
            DeviceInformationKind::AssociationEndpoint).get();

        for (auto const& di : infos) {
            DeviceBatteryInfo info;
            info.deviceName = di.Name().c_str();

            std::wstring address;
            try {
                auto value = di.Properties().Lookup(L"System.Devices.Aep.DeviceAddress");
                if (value)
                    address = NormalizeMac(unbox_value<hstring>(value).c_str());
            }
            catch (...) {}

            if (address.empty())
                address = MacFromDeviceId(di.Id().c_str());

            info.deviceAddress = address;
            info.isConnected = ConnectedFromAep(di);
            info.batteryPercent = BatteryFromAep(di);

            if (FindByAddress(results, address))
                continue;

            // Dual-mode devices may expose the GATT battery service over LE.
            if (info.batteryPercent < 0 && info.isConnected && address.size() == 12) {
                try {
                    uint64_t mac = std::stoull(address, nullptr, 16);
                    auto op = BluetoothLEDevice::FromBluetoothAddressAsync(mac);
                    if (WaitFor(op, 5000)) {
                        auto bleDevice = op.GetResults();
                        if (bleDevice)
                            info.batteryPercent = ReadGattBattery(bleDevice);
                    }
                }
                catch (...) {}
            }

            if (!info.deviceName.empty() || info.batteryPercent >= 0)
                results.push_back(info);
        }
    }
    catch (...) {}

    // --- 3. HFP battery report cached by the Windows Bluetooth stack ---
    // Classic headsets do not expose GATT; their level is cached on the
    // Hands-Free AG node, which is what the Settings app displays.
    const auto hfpBatteries = QueryHfpBatteriesFromPnp();
    for (auto& info : results) {
        if (info.batteryPercent >= 0)
            continue;
        const std::wstring mac = NormalizeMac(info.deviceAddress);
        if (mac.size() != 12)
            continue;
        for (auto const& entry : hfpBatteries) {
            if (entry.first.find(mac) != std::wstring::npos) {
                info.batteryPercent = entry.second;
                break;
            }
        }
    }

    // --- 4. Registry fallback (older Windows builds / vendor drivers) ---
    for (auto& info : results) {
        if (info.batteryPercent >= 0)
            continue;
        info.batteryPercent = QueryBatteryFromRegistry(NormalizeMac(info.deviceAddress));
    }

    // Connected devices first, then alphabetical.
    std::sort(results.begin(), results.end(), [](DeviceBatteryInfo const& a, DeviceBatteryInfo const& b) {
        if (a.isConnected != b.isConnected)
            return a.isConnected;
        return _wcsicmp(a.deviceName.c_str(), b.deviceName.c_str()) < 0;
    });

    return results;
}
