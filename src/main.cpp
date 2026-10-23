#include "BatteryMonitor.h"
#include "DarkMode.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <iterator>
#include <cwchar>
#include <cwctype>
#include <cstring>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WM_TRAYICON         (WM_APP + 1)
#define WM_SCAN_COMPLETE    (WM_APP + 2)
#define WM_REFRESH_SCAN     (WM_APP + 3)
#define WM_BT_DEVICE_CHANGED (WM_APP + 4)
#define ID_TRAY_EXIT        1001
#define ID_TRAY_REFRESH     1002
#define ID_TRAY_SHOW        1003
#define POLL_INTERVAL_MS    30000
#define IDC_DEVICE_LIST     2001
#define IDC_REFRESH_BTN     2002
#define IDC_STATUS_TEXT     2003
#define IDC_SET_ICON_BTN    2004
#define IDC_CLEAR_ICON_BTN  2005

HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
std::atomic<bool> g_scanning = false;
std::mutex g_dataMutex;
std::vector<DeviceBatteryInfo> g_devices;

// One tray icon per device. Key is the normalized MAC address so a device keeps
// its icon (and uID) across scans instead of being re-created every 30 seconds.
struct TrayEntry {
    UINT id = 0;
    std::wstring key;
    HICON icon = nullptr;
    NOTIFYICONDATAW nid = {};
    bool lowNotified = false;
};

std::map<std::wstring, TrayEntry> g_trays;
UINT g_taskbarCreatedMessage = 0;

// Windows identifies a notification icon (and remembers its pinned/hidden state)
// by executable + uID. Re-adding a device with a fresh uID makes the shell treat
// it as a brand-new icon and drop it into the overflow area. Persist one uID per
// device MAC so a reconnect reuses the identity the user already pinned.
static constexpr wchar_t kTrayIdRegKey[] = L"Software\\BluetoothBatteryTray\\TrayIds";
std::map<std::wstring, UINT> g_trayIds;
UINT g_nextTrayId = 1;

static void LoadTrayIds()
{
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kTrayIdRegKey, 0, KEY_QUERY_VALUE, &hkey) != ERROR_SUCCESS)
        return;

    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD nameLen = static_cast<DWORD>(std::size(name));
        DWORD type = 0;
        DWORD dataLen = 0;
        LONG rc = RegEnumValueW(hkey, index, name, &nameLen, nullptr, &type, nullptr, &dataLen);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS || type != REG_DWORD || dataLen != sizeof(DWORD))
            continue;

        DWORD id = 0;
        dataLen = sizeof(id);
        if (RegQueryValueExW(hkey, name, nullptr, &type,
                reinterpret_cast<BYTE*>(&id), &dataLen) != ERROR_SUCCESS)
            continue;
        if (type != REG_DWORD || id == 0)
            continue;

        g_trayIds[name] = id;
        if (id >= g_nextTrayId)
            g_nextTrayId = id + 1;
    }
    RegCloseKey(hkey);
}

static UINT TrayIdFor(std::wstring const& key)
{
    auto it = g_trayIds.find(key);
    if (it != g_trayIds.end())
        return it->second;

    auto idInUse = [](UINT id) {
        for (auto const& kv : g_trayIds) {
            if (kv.second == id)
                return true;
        }
        return false;
    };
    while (g_nextTrayId == 0 || idInUse(g_nextTrayId))
        ++g_nextTrayId;

    const UINT id = g_nextTrayId++;
    g_trayIds[key] = id;

    HKEY hkey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kTrayIdRegKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &hkey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hkey, key.c_str(), 0, REG_DWORD,
            reinterpret_cast<BYTE const*>(&id), sizeof(id));
        RegCloseKey(hkey);
    }
    return id;
}

HWND g_win = nullptr;
HWND g_list = nullptr;
HWND g_refreshBtn = nullptr;
HWND g_setIconBtn = nullptr;
HWND g_clearIconBtn = nullptr;
HWND g_status = nullptr;
HFONT g_uiFont = nullptr;
HIMAGELIST g_thumbList = nullptr;
int g_thumbSize = 16;

// A decoded custom device image, premultiplied BGRA, top-down.
struct DeviceIconImage {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
};

// Custom images are keyed by normalized MAC and stored under HKCU. The actual
// file is copied into LOCALAPPDATA so the original can be moved or deleted.
std::map<std::wstring, std::wstring> g_iconPaths;
std::map<std::wstring, DeviceIconImage> g_iconImages;

static constexpr wchar_t kIconRegKey[] = L"Software\\BluetoothBatteryTray\\DeviceIcons";

static std::wstring NormalizeMac(std::wstring const& address)
{
    std::wstring key;
    for (wchar_t c : address) {
        if (iswxdigit(c))
            key.push_back(static_cast<wchar_t>(towupper(c)));
    }
    return key;
}

static void SaveDeviceIconPath(std::wstring const& key, std::wstring const& path)
{
    HKEY hkey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kIconRegKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &hkey, nullptr) != ERROR_SUCCESS)
        return;

    RegSetValueExW(hkey, key.c_str(), 0, REG_SZ,
        reinterpret_cast<BYTE const*>(path.c_str()),
        static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hkey);
}

static void DeleteDeviceIconPath(std::wstring const& key)
{
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kIconRegKey, 0, KEY_SET_VALUE, &hkey) != ERROR_SUCCESS)
        return;

    RegDeleteValueW(hkey, key.c_str());
    RegCloseKey(hkey);
}

static void LoadDeviceIconPaths()
{
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kIconRegKey, 0, KEY_QUERY_VALUE, &hkey) != ERROR_SUCCESS)
        return;

    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD nameLen = static_cast<DWORD>(std::size(name));
        DWORD type = 0;
        DWORD dataLen = 0;
        LONG rc = RegEnumValueW(hkey, index, name, &nameLen, nullptr, &type, nullptr, &dataLen);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA)
            continue;
        if (type != REG_SZ || dataLen < sizeof(wchar_t))
            continue;

        std::wstring value(dataLen / sizeof(wchar_t), L'\0');
        dataLen = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        if (RegQueryValueExW(hkey, name, nullptr, &type,
                reinterpret_cast<BYTE*>(value.data()), &dataLen) != ERROR_SUCCESS)
            continue;
        if (type != REG_SZ)
            continue;

        value.resize(wcsnlen(value.c_str(), value.size()));
        if (!value.empty())
            g_iconPaths[name] = value;
    }
    RegCloseKey(hkey);
}

static std::wstring EnsureIconDirectory()
{
    PWSTR localAppData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData))) {
        dir = localAppData;
        CoTaskMemFree(localAppData);
    }
    if (dir.empty())
        return dir;

    dir += L"\\BluetoothBatteryTray\\icons";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

static std::wstring FileSafeKey(std::wstring key)
{
    for (wchar_t& c : key) {
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c) != nullptr)
            c = L'_';
    }
    if (key.empty())
        key = L"device";
    return key;
}

// Decodes any WIC-supported format, scales it to fit maxWidth x maxHeight and
// converts it to premultiplied BGRA ready for both the tray icon and thumbnails.
static bool DecodeImageToBgra(std::wstring const& path, int maxWidth, int maxHeight, DeviceIconImage& out)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))))
        return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)))
        return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return false;

    UINT srcWidth = 0;
    UINT srcHeight = 0;
    if (FAILED(frame->GetSize(&srcWidth, &srcHeight)) || srcWidth == 0 || srcHeight == 0)
        return false;

    const double scale = std::min(
        static_cast<double>(maxWidth) / srcWidth,
        static_cast<double>(maxHeight) / srcHeight);
    const int outWidth = std::max(1, static_cast<int>(srcWidth * scale + 0.5));
    const int outHeight = std::max(1, static_cast<int>(srcHeight * scale + 0.5));

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), static_cast<UINT>(outWidth),
            static_cast<UINT>(outHeight), WICBitmapInterpolationModeFant)))
        return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;

    out.width = outWidth;
    out.height = outHeight;
    out.pixels.assign(static_cast<size_t>(outWidth) * outHeight, 0);

    const UINT stride = static_cast<UINT>(outWidth * 4);
    const UINT bufferSize = stride * static_cast<UINT>(outHeight);
    if (FAILED(converter->CopyPixels(nullptr, stride, bufferSize,
            reinterpret_cast<BYTE*>(out.pixels.data()))))
        return false;

    return true;
}

static DeviceIconImage const* FindDeviceImage(std::wstring const& key)
{
    auto cached = g_iconImages.find(key);
    if (cached != g_iconImages.end())
        return &cached->second;

    auto path = g_iconPaths.find(key);
    if (path == g_iconPaths.end())
        return nullptr;

    DeviceIconImage image;
    if (!DecodeImageToBgra(path->second, 32, 30, image))
        return nullptr;

    auto inserted = g_iconImages.emplace(key, std::move(image));
    return &inserted.first->second;
}

static bool SetDeviceImage(std::wstring const& key, std::wstring const& sourcePath)
{
    DeviceIconImage image;
    if (!DecodeImageToBgra(sourcePath, 32, 30, image))
        return false;

    std::wstring dir = EnsureIconDirectory();
    if (dir.empty())
        return false;

    std::wstring ext;
    const size_t dot = sourcePath.find_last_of(L'.');
    const size_t slash = sourcePath.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) &&
        sourcePath.size() - dot <= 8) {
        ext = sourcePath.substr(dot);
        for (wchar_t& c : ext)
            c = static_cast<wchar_t>(towlower(c));
    }
    else {
        ext = L".img";
    }

    const std::wstring target = dir + L"\\" + FileSafeKey(key) + ext;

    auto old = g_iconPaths.find(key);
    if (old != g_iconPaths.end() && _wcsicmp(old->second.c_str(), target.c_str()) != 0)
        DeleteFileW(old->second.c_str());

    if (!CopyFileW(sourcePath.c_str(), target.c_str(), FALSE))
        return false;

    g_iconPaths[key] = target;
    g_iconImages[key] = std::move(image);
    SaveDeviceIconPath(key, target);
    return true;
}

static void ClearDeviceImage(std::wstring const& key)
{
    auto it = g_iconPaths.find(key);
    if (it != g_iconPaths.end()) {
        DeleteFileW(it->second.c_str());
        g_iconPaths.erase(it);
    }
    g_iconImages.erase(key);
    DeleteDeviceIconPath(key);
}

// Box filter keeps small logos readable when shrinking them to a list thumbnail.
static std::vector<uint32_t> ResizeImage(DeviceIconImage const& source, int width, int height)
{
    std::vector<uint32_t> result(static_cast<size_t>(width) * height, 0);

    for (int y = 0; y < height; ++y) {
        const int sy0 = y * source.height / height;
        const int sy1 = std::max(sy0 + 1, (y + 1) * source.height / height);
        for (int x = 0; x < width; ++x) {
            const int sx0 = x * source.width / width;
            const int sx1 = std::max(sx0 + 1, (x + 1) * source.width / width);

            uint64_t sum[4] = {};
            int count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint32_t* row = source.pixels.data() + static_cast<size_t>(sy) * source.width;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint32_t p = row[sx];
                    sum[0] += p & 0xFF;
                    sum[1] += (p >> 8) & 0xFF;
                    sum[2] += (p >> 16) & 0xFF;
                    sum[3] += (p >> 24) & 0xFF;
                    ++count;
                }
            }
            if (count == 0)
                continue;

            const auto average = [&](int channel) {
                return static_cast<uint32_t>((sum[channel] + count / 2) / count);
            };
            result[static_cast<size_t>(y) * width + x] =
                (average(3) << 24) | (average(2) << 16) | (average(1) << 8) | average(0);
        }
    }

    return result;
}

static HICON CreateIconFromPixels(std::vector<uint32_t> const& pixels, int width, int height)
{
    HDC screen = GetDC(nullptr);
    if (!screen)
        return nullptr;

    BITMAPV5HEADER bmi = {};
    bmi.bV5Size = sizeof(bmi);
    bmi.bV5Width = width;
    bmi.bV5Height = -height; // top-down
    bmi.bV5Planes = 1;
    bmi.bV5BitCount = 32;
    bmi.bV5Compression = BI_BITFIELDS;
    bmi.bV5RedMask = 0x00FF0000;
    bmi.bV5GreenMask = 0x0000FF00;
    bmi.bV5BlueMask = 0x000000FF;
    bmi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bmi),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);

    if (!colorBmp || !bits) {
        if (colorBmp)
            DeleteObject(colorBmp);
        return nullptr;
    }

    memcpy(bits, pixels.data(), pixels.size() * sizeof(uint32_t));

    std::vector<BYTE> maskBits(((width + 15) / 16) * 2 * height, 0);
    HBITMAP maskBmp = CreateBitmap(width, height, 1, 1, maskBits.data());

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);

    if (maskBmp)
        DeleteObject(maskBmp);
    DeleteObject(colorBmp);
    return icon;
}

static HICON CreateThumbnailIcon(DeviceIconImage const& image, int size)
{
    // Fit inside the square slot without distorting the aspect ratio.
    const double scale = std::min(
        static_cast<double>(size) / image.width,
        static_cast<double>(size) / image.height);
    const int width = std::max(1, static_cast<int>(image.width * scale + 0.5));
    const int height = std::max(1, static_cast<int>(image.height * scale + 0.5));
    const std::vector<uint32_t> scaled = ResizeImage(image, width, height);

    if (width == size && height == size)
        return CreateIconFromPixels(scaled, size, size);

    std::vector<uint32_t> canvas(static_cast<size_t>(size) * size, 0);
    const int x0 = (size - width) / 2;
    const int y0 = (size - height) / 2;
    for (int y = 0; y < height; ++y) {
        memcpy(canvas.data() + static_cast<size_t>(y0 + y) * size + x0,
            scaled.data() + static_cast<size_t>(y) * width,
            static_cast<size_t>(width) * sizeof(uint32_t));
    }
    return CreateIconFromPixels(canvas, size, size);
}

static void DrawImageOnIcon(uint32_t* dst, int size, DeviceIconImage const& image)
{
    const int areaHeight = size - 2; // leave the battery bar untouched
    const int x0 = (size - image.width) / 2;
    const int y0 = (areaHeight - image.height) / 2;

    for (int y = 0; y < image.height; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= areaHeight)
            continue;

        const uint32_t* srcRow = image.pixels.data() + static_cast<size_t>(y) * image.width;
        uint32_t* dstRow = dst + static_cast<size_t>(dy) * size;
        for (int x = 0; x < image.width; ++x) {
            const int dx = x0 + x;
            if (dx < 0 || dx >= size)
                continue;

            const uint32_t s = srcRow[x];
            const uint32_t alpha = s >> 24;
            if (alpha == 0)
                continue;

            uint32_t& d = dstRow[dx];
            const uint32_t inv = 255 - alpha;
            const auto over = [inv](uint32_t src, uint32_t dstChannel) {
                return std::min<uint32_t>(255, src + (dstChannel * inv + 127) / 255);
            };

            const uint32_t r = over((s >> 16) & 0xFF, (d >> 16) & 0xFF);
            const uint32_t g = over((s >> 8) & 0xFF, (d >> 8) & 0xFF);
            const uint32_t b = over(s & 0xFF, d & 0xFF);
            const uint32_t a = over(alpha, d >> 24);
            d = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

// Renders the battery into an icon sized like a small tray icon. A custom
// device image, when present, replaces the percentage text above the bar.
static HICON CreateBatteryIcon(int percent, bool connected, DeviceIconImage const* image)
{
    // Windows 11 renders overflow tiles at 32px and downscales for the taskbar,
    // so render at 32 for the sharpest result in both places.
    int size = 32;

    HDC screen = GetDC(nullptr);
    if (!screen)
        return nullptr;

    BITMAPV5HEADER bmi = {};
    bmi.bV5Size = sizeof(bmi);
    bmi.bV5Width = size;
    bmi.bV5Height = -size; // top-down
    bmi.bV5Planes = 1;
    bmi.bV5BitCount = 32;
    bmi.bV5Compression = BI_BITFIELDS;
    bmi.bV5RedMask = 0x00FF0000;
    bmi.bV5GreenMask = 0x0000FF00;
    bmi.bV5BlueMask = 0x000000FF;
    bmi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    if (!colorBmp || !dc || !bits) {
        if (dc)
            DeleteDC(dc);
        if (colorBmp)
            DeleteObject(colorBmp);
        return nullptr;
    }

    HGDIOBJ oldBmp = SelectObject(dc, colorBmp);

    RECT rc = { 0, 0, size, size };
    HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &rc, bgBrush);
    DeleteObject(bgBrush);

    COLORREF color;
    if (!connected || percent < 0)
        color = RGB(140, 140, 140); // offline / unknown
    else if (percent <= 20)
        color = RGB(255, 70, 70);
    else if (percent <= 50)
        color = RGB(255, 200, 60);
    else
        color = RGB(90, 220, 90);

    if (!image) {
        wchar_t text[8];
        if (percent < 0)
            wcscpy_s(text, L"?");
        else
            swprintf_s(text, L"%d", percent);

        // Shrink the font until the text fits the icon width.
        HFONT font = nullptr;
        for (int height = size - 2; height >= 7; --height) {
            HFONT candidate = CreateFontW(-height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            if (!candidate)
                break;

            HGDIOBJ oldFont = SelectObject(dc, candidate);
            SIZE extent = {};
            GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &extent);
            SelectObject(dc, oldFont);

            if (extent.cx <= size - 2) {
                font = candidate;
                break;
            }
            DeleteObject(candidate);
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;

        // Leave room for the battery bar at the bottom.
        RECT textRc = { 0, 0, size, size - 2 };
        DrawTextW(dc, text, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (oldFont)
            SelectObject(dc, oldFont);
        if (font)
            DeleteObject(font);
    }

    SelectObject(dc, oldBmp);

    // GDI does not write alpha: derive it from luminance (black background -> transparent).
    auto* pixels = static_cast<uint32_t*>(bits);
    const int count = size * size;
    for (int i = 0; i < count; ++i) {
        const uint32_t p = pixels[i];
        const uint32_t r = (p >> 16) & 0xFF;
        const uint32_t g = (p >> 8) & 0xFF;
        const uint32_t b = p & 0xFF;
        const uint32_t alpha = (r > g) ? (r > b ? r : b) : (g > b ? g : b);
        pixels[i] = (alpha << 24) | (p & 0x00FFFFFF);
    }

    if (image)
        DrawImageOnIcon(pixels, size, *image);

    // Draw the bar directly into the pixels so it keeps full alpha.
    if (percent >= 0) {
        const uint32_t barPixel = 0xFF000000u
            | ((color & 0x000000FFu) << 16)
            | (color & 0x0000FF00u)
            | ((color & 0x00FF0000u) >> 16);
        for (int y = size - 2; y < size; ++y) {
            for (int x = 1; x < size - 1; ++x)
                pixels[y * size + x] = barPixel;
        }
    }

    std::vector<BYTE> maskBits(((size + 15) / 16) * 2 * size, 0);
    HBITMAP maskBmp = CreateBitmap(size, size, 1, 1, maskBits.data());

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);

    if (maskBmp)
        DeleteObject(maskBmp);
    DeleteDC(dc);
    DeleteObject(colorBmp);

    return icon;
}

static std::wstring TrayKey(DeviceBatteryInfo const& device)
{
    std::wstring key = NormalizeMac(device.deviceAddress);
    if (key.empty())
        key = device.deviceName.empty() ? L"__status__" : device.deviceName;
    return key;
}

static std::wstring TrayTooltip(DeviceBatteryInfo const& device)
{
    std::wstring tip = device.deviceName + L": ";
    if (device.batteryPercent >= 0)
        tip += std::to_wstring(device.batteryPercent) + L"%";
    else
        tip += L"N/A";
    tip += device.isConnected ? L" (connected)" : L" (disconnected)";
    return tip;
}

static void UpdateTrayEntry(TrayEntry& entry, DeviceBatteryInfo const& device, bool isNew)
{
    wcsncpy_s(entry.nid.szTip, TrayTooltip(device).c_str(), _TRUNCATE);

    HICON icon = CreateBatteryIcon(device.batteryPercent, device.isConnected,
        FindDeviceImage(entry.key));

    entry.nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    entry.nid.uCallbackMessage = WM_TRAYICON;
    entry.nid.uID = entry.id;
    if (icon)
        entry.nid.hIcon = icon;

    Shell_NotifyIconW(isNew ? NIM_ADD : NIM_MODIFY, &entry.nid);

    // Keep the icon alive while the shell uses it; free the previous one.
    if (icon) {
        if (entry.icon)
            DestroyIcon(entry.icon);
        entry.icon = icon;
    }

    // Warn once when a connected device drops to 15% or below.
    if (device.isConnected && device.batteryPercent >= 0 && device.batteryPercent <= 15) {
        if (!entry.lowNotified) {
            NOTIFYICONDATAW balloon = entry.nid;
            balloon.uFlags = NIF_INFO;
            balloon.dwInfoFlags = NIIF_WARNING;
            wcscpy_s(balloon.szInfoTitle, L"Low Bluetooth Battery");
            const std::wstring info = device.deviceName + L" is at " +
                std::to_wstring(device.batteryPercent) + L"%. Please charge it.";
            wcsncpy_s(balloon.szInfo, info.c_str(), _TRUNCATE);
            Shell_NotifyIconW(NIM_MODIFY, &balloon);
            entry.lowNotified = true;
        }
    }
    else if (device.batteryPercent > 20 || device.batteryPercent < 0) {
        entry.lowNotified = false;
    }
}

static void RemoveTrayEntry(TrayEntry& entry)
{
    Shell_NotifyIconW(NIM_DELETE, &entry.nid);
    if (entry.icon) {
        DestroyIcon(entry.icon);
        entry.icon = nullptr;
    }
}

static void SyncTrayIcons(HWND hWnd)
{
    std::vector<DeviceBatteryInfo> scanned;
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        scanned = g_devices;
    }

    // Only connected devices get a tray icon. The list window still shows all of
    // them; here we just skip anything that is currently offline. Keys are the
    // normalized MAC address so each entry keeps a stable uID.
    std::vector<std::pair<std::wstring, DeviceBatteryInfo>> devices;
    devices.reserve(scanned.size() + 1);
    for (auto const& device : scanned) {
        if (device.isConnected)
            devices.emplace_back(TrayKey(device), device);
    }

    // With nothing connected there would be no icon at all, i.e. no way to open
    // the menu or exit. Keep a single neutral icon in that case, with a fixed
    // key so its uID stays stable too.
    if (devices.empty()) {
        DeviceBatteryInfo none;
        none.deviceName = scanned.empty() ? L"No Bluetooth devices" : L"No connected devices";
        none.batteryPercent = -1;
        none.isConnected = false;
        devices.emplace_back(L"__status__", none);
    }

    std::vector<std::wstring> alive;
    alive.reserve(devices.size());

    for (auto const& item : devices) {
        const std::wstring& key = item.first;
        const DeviceBatteryInfo& device = item.second;

        alive.push_back(key);

        auto it = g_trays.find(key);
        if (it == g_trays.end()) {
            TrayEntry entry;
            entry.id = TrayIdFor(key);
            entry.key = key;
            entry.nid.cbSize = sizeof(NOTIFYICONDATAW);
            entry.nid.hWnd = hWnd;
            entry.nid.uID = entry.id;
            it = g_trays.emplace(key, std::move(entry)).first;
            UpdateTrayEntry(it->second, device, true);
        }
        else {
            UpdateTrayEntry(it->second, device, false);
        }
    }

    for (auto it = g_trays.begin(); it != g_trays.end();) {
        if (std::find(alive.begin(), alive.end(), it->first) == alive.end()) {
            RemoveTrayEntry(it->second);
            it = g_trays.erase(it);
        }
        else {
            ++it;
        }
    }
}

static TrayEntry* FindTrayEntry(UINT id)
{
    for (auto& kv : g_trays) {
        if (kv.second.id == id)
            return &kv.second;
    }
    return nullptr;
}

static void DoScan(HWND hWnd)
{
    if (g_scanning)
        return;
    g_scanning = true;

    std::thread([hWnd]() {
        std::vector<DeviceBatteryInfo> devices;
        try {
            devices = ScanBluetoothDevicesBattery();
        }
        catch (...) {}
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_devices = std::move(devices);
        }
        PostMessageW(hWnd, WM_SCAN_COMPLETE, 0, 0);
        g_scanning = false;
    }).detach();
}

static int ScaleForDpi(int value, HWND hWnd)
{
    const UINT dpi = hWnd ? GetDpiForWindow(hWnd) : GetDpiForSystem();
    if (dpi == 0)
        return value;
    return MulDiv(value, static_cast<int>(dpi), 96);
}

static HFONT CreateUiFont()
{
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        return CreateFontIndirectW(&metrics.lfMessageFont);
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

static std::wstring SelectedDeviceKey()
{
    if (!g_list)
        return {};

    const int selected = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (selected < 0)
        return {};

    wchar_t name[512] = {};
    wchar_t address[128] = {};
    ListView_GetItemText(g_list, selected, 0, name, static_cast<int>(std::size(name)));
    ListView_GetItemText(g_list, selected, 1, address, static_cast<int>(std::size(address)));

    std::wstring key = NormalizeMac(address);
    if (key.empty())
        key = name;
    return key;
}

static void UpdateImageButtons()
{
    const std::wstring key = SelectedDeviceKey();
    const bool selected = !key.empty();

    if (g_setIconBtn)
        EnableWindow(g_setIconBtn, selected);
    if (g_clearIconBtn)
        EnableWindow(g_clearIconBtn, selected && g_iconPaths.find(key) != g_iconPaths.end());
}

static void RefreshDeviceList()
{
    if (!g_list)
        return;

    const std::wstring selectedKey = SelectedDeviceKey();

    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    if (g_thumbList)
        ImageList_RemoveAll(g_thumbList);

    std::vector<DeviceBatteryInfo> devices;
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        devices = g_devices;
    }

    int row = 0;
    int selectedRow = -1;
    for (auto const& d : devices) {
        const std::wstring key = TrayKey(d);
        if (!selectedKey.empty() && key == selectedKey)
            selectedRow = row;

        int imageIndex = I_IMAGENONE;
        if (g_thumbList) {
            if (DeviceIconImage const* image = FindDeviceImage(key)) {
                HICON thumb = CreateThumbnailIcon(*image, g_thumbSize);
                if (thumb) {
                    imageIndex = ImageList_AddIcon(g_thumbList, thumb);
                    DestroyIcon(thumb);
                }
            }
        }

        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = row;
        item.iSubItem = 0;
        item.iImage = imageIndex;
        item.pszText = const_cast<wchar_t*>(d.deviceName.c_str());
        // percent + 1 in the low word (0 = unknown), connected flag in bit 16
        item.lParam = static_cast<LPARAM>((d.batteryPercent + 1) | (d.isConnected ? 0x10000 : 0));
        const int index = ListView_InsertItem(g_list, &item);
        if (index < 0)
            continue;

        ListView_SetItemText(g_list, index, 1, const_cast<wchar_t*>(d.deviceAddress.c_str()));

        wchar_t battery[16];
        if (d.batteryPercent >= 0)
            swprintf_s(battery, L"%d%%", d.batteryPercent);
        else
            wcscpy_s(battery, L"N/A");
        ListView_SetItemText(g_list, index, 2, battery);

        ListView_SetItemText(g_list, index, 3,
            const_cast<wchar_t*>(d.isConnected ? L"Connected" : L"Disconnected"));
        ++row;
    }

    if (selectedRow >= 0) {
        ListView_SetItemState(g_list, selectedRow,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_list, selectedRow, FALSE);
    }

    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t status[80];
    swprintf_s(status, L"Devices: %d     Updated: %02d:%02d:%02d",
        static_cast<int>(devices.size()), st.wHour, st.wMinute, st.wSecond);
    SetWindowTextW(g_status, status);
    UpdateImageButtons();
}

static void LayoutDevicesWindow(HWND hWnd)
{
    if (!g_list)
        return;

    RECT rc;
    GetClientRect(hWnd, &rc);

    const int margin = ScaleForDpi(12, hWnd);
    const int gap = ScaleForDpi(8, hWnd);
    const int buttonW = ScaleForDpi(110, hWnd);
    const int buttonH = ScaleForDpi(28, hWnd);
    const int bottomY = rc.bottom - margin - buttonH;
    const int listHeight = bottomY - gap - margin;

    MoveWindow(g_list, margin, margin,
        rc.right - 2 * margin, listHeight > 0 ? listHeight : 0, TRUE);

    int x = rc.right - margin;
    MoveWindow(g_refreshBtn, x - buttonW, bottomY, buttonW, buttonH, TRUE);
    x -= buttonW + gap;
    MoveWindow(g_clearIconBtn, x - buttonW, bottomY, buttonW, buttonH, TRUE);
    x -= buttonW + gap;
    MoveWindow(g_setIconBtn, x - buttonW, bottomY, buttonW, buttonH, TRUE);
    x -= buttonW + gap;

    const int statusWidth = x - margin;
    MoveWindow(g_status, margin, bottomY + ScaleForDpi(8, hWnd),
        statusWidth > 0 ? statusWidth : 0, buttonH, TRUE);
}

static void ApplyDeviceListTheme()
{
    if (!g_list)
        return;

    ListView_SetBkColor(g_list, darkmode::BackgroundColor());
    ListView_SetTextBkColor(g_list, darkmode::BackgroundColor());
    ListView_SetTextColor(g_list, darkmode::TextColor());

    darkmode::ApplyToControl(g_list);
    if (HWND header = ListView_GetHeader(g_list))
        darkmode::ApplyToControl(header, L"DarkMode_ItemsView");
}

LRESULT CALLBACK DevicesWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        {
            g_uiFont = CreateUiFont();

            g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_DEVICE_LIST), g_hInst, nullptr);
            ListView_SetExtendedListViewStyle(g_list,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
            SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);

            g_thumbSize = ScaleForDpi(16, hWnd);
            g_thumbList = ImageList_Create(g_thumbSize, g_thumbSize, ILC_COLOR32, 8, 8);
            if (g_thumbList)
                ListView_SetImageList(g_list, g_thumbList, LVSIL_SMALL);

            struct Column { const wchar_t* title; int width; };
            const Column columns[] = {
                { L"Device", 240 },
                { L"Address", 150 },
                { L"Battery", 80 },
                { L"Status", 110 },
            };
            for (int i = 0; i < static_cast<int>(std::size(columns)); ++i) {
                LVCOLUMNW column = {};
                column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                column.pszText = const_cast<wchar_t*>(columns[i].title);
                column.cx = ScaleForDpi(columns[i].width, hWnd);
                column.iSubItem = i;
                ListView_InsertColumn(g_list, i, &column);
            }

            g_refreshBtn = CreateWindowExW(0, L"BUTTON", L"Refresh",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_REFRESH_BTN), g_hInst, nullptr);
            SendMessageW(g_refreshBtn, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);

            g_setIconBtn = CreateWindowExW(0, L"BUTTON", L"Set image...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_SET_ICON_BTN), g_hInst, nullptr);
            SendMessageW(g_setIconBtn, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);

            g_clearIconBtn = CreateWindowExW(0, L"BUTTON", L"Clear image",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_CLEAR_ICON_BTN), g_hInst, nullptr);
            SendMessageW(g_clearIconBtn, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);

            g_status = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_STATUS_TEXT), g_hInst, nullptr);
            SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);

            darkmode::ApplyToWindow(hWnd);
            ApplyDeviceListTheme();
            darkmode::ApplyToControl(g_refreshBtn);
            darkmode::ApplyToControl(g_setIconBtn);
            darkmode::ApplyToControl(g_clearIconBtn);
            darkmode::ApplyToControl(g_status);

            RefreshDeviceList();
        }
        return 0;

    case WM_SIZE:
        LayoutDevicesWindow(hWnd);
        return 0;

    case WM_GETMINMAXINFO:
        {
            auto info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = ScaleForDpi(600, hWnd);
            info->ptMinTrackSize.y = ScaleForDpi(240, hWnd);
        }
        return 0;

    case WM_ERASEBKGND:
        {
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, darkmode::BackgroundBrush());
        }
        return 1;

    case WM_CTLCOLORSTATIC:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, darkmode::TextColor());
            return reinterpret_cast<LRESULT>(darkmode::BackgroundBrush());
        }

    case WM_SETTINGCHANGE:
        if (lParam && wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
            if (darkmode::OnSystemThemeChanged()) {
                darkmode::ApplyToWindow(hWnd);
                ApplyDeviceListTheme();
                darkmode::ApplyToControl(g_refreshBtn);
                darkmode::ApplyToControl(g_setIconBtn);
                darkmode::ApplyToControl(g_clearIconBtn);
                darkmode::ApplyToControl(g_status);
                RedrawWindow(hWnd, nullptr, nullptr,
                    RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_REFRESH_BTN:
            SetWindowTextW(g_status, L"Scanning...");
            DoScan(g_hWnd);
            return 0;

        case IDC_SET_ICON_BTN:
            {
                const std::wstring key = SelectedDeviceKey();
                if (key.empty())
                    return 0;

                wchar_t file[MAX_PATH] = {};
                OPENFILENAMEW ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hWnd;
                ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.ico;*.gif\0All files\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = static_cast<DWORD>(std::size(file));
                ofn.lpstrTitle = L"Choose device image";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                if (!GetOpenFileNameW(&ofn))
                    return 0;

                if (SetDeviceImage(key, file)) {
                    SyncTrayIcons(g_hWnd);
                    RefreshDeviceList();
                }
                else {
                    MessageBoxW(hWnd, L"Could not load the selected image.",
                        L"Bluetooth Battery Monitor", MB_ICONERROR | MB_OK);
                }
            }
            return 0;

        case IDC_CLEAR_ICON_BTN:
            {
                const std::wstring key = SelectedDeviceKey();
                if (!key.empty()) {
                    ClearDeviceImage(key);
                    SyncTrayIcons(g_hWnd);
                    RefreshDeviceList();
                }
            }
            return 0;
        }
        return 0;

    case WM_NOTIFY:
        {
            auto header = reinterpret_cast<LPNMHDR>(lParam);
            if (header->idFrom == IDC_DEVICE_LIST && header->code == LVN_ITEMCHANGED) {
                UpdateImageButtons();
                return 0;
            }
            if (header->idFrom == IDC_DEVICE_LIST && header->code == NM_DBLCLK) {
                PostMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_SET_ICON_BTN, BN_CLICKED), 0);
                return 0;
            }
            if (header->idFrom == IDC_DEVICE_LIST && header->code == NM_CUSTOMDRAW) {
                auto draw = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
                switch (draw->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT:
                    return CDRF_NOTIFYSUBITEMDRAW;
                case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                    {
                        const LPARAM data = draw->nmcd.lItemlParam;
                        const int percent = static_cast<int>(data & 0xFFFF) - 1;
                        const bool connected = (data & 0x10000) != 0;

                        if (draw->iSubItem == 2) {
                            if (percent < 0 || !connected)
                                draw->clrText = RGB(130, 130, 130);
                            else if (percent <= 20)
                                draw->clrText = RGB(200, 0, 0);
                            else if (percent <= 50)
                                draw->clrText = RGB(175, 120, 0);
                            else
                                draw->clrText = RGB(0, 135, 0);
                            return CDRF_NEWFONT;
                        }
                        if (draw->iSubItem == 3) {
                            draw->clrText = connected ? RGB(0, 135, 0) : RGB(130, 130, 130);
                            return CDRF_NEWFONT;
                        }
                    }
                    break;
                }
            }
        }
        break;

    case WM_CLOSE:
        // Keep the window (and its position) alive; just hide it.
        ShowWindow(hWnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        if (g_uiFont) {
            DeleteObject(g_uiFont);
            g_uiFont = nullptr;
        }
        if (g_thumbList) {
            ImageList_Destroy(g_thumbList);
            g_thumbList = nullptr;
        }
        g_win = nullptr;
        g_list = nullptr;
        g_refreshBtn = nullptr;
        g_setIconBtn = nullptr;
        g_clearIconBtn = nullptr;
        g_status = nullptr;
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowDevicesWindow()
{
    if (!g_win) {
        g_win = CreateWindowExW(0, L"BluetoothBatteryWindowClass", L"Bluetooth Battery Monitor",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            ScaleForDpi(640, nullptr), ScaleForDpi(420, nullptr),
            nullptr, nullptr, g_hInst, nullptr);
        if (!g_win)
            return;
    }

    RefreshDeviceList();
    ShowWindow(g_win, SW_SHOW);
    SetForegroundWindow(g_win);
    DoScan(g_hWnd);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Explorer restarting wipes every notification icon from the tray. It
    // broadcasts TaskbarCreated when the new shell is ready, so re-register all
    // icons from the state we already keep in g_trays.
    if (g_taskbarCreatedMessage && msg == g_taskbarCreatedMessage) {
        for (auto& kv : g_trays) {
            kv.second.nid.hWnd = hWnd;
            Shell_NotifyIconW(NIM_ADD, &kv.second.nid);
        }
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        SetTimer(hWnd, 1, POLL_INTERVAL_MS, nullptr);
        PostMessageW(hWnd, WM_REFRESH_SCAN, 0, 0);
        return 0;

    case WM_TRAYICON:
        {
            TrayEntry* entry = FindTrayEntry(static_cast<UINT>(wParam));

            if (lParam == WM_RBUTTONUP) {
                HMENU menu = CreatePopupMenu();
                if (entry) {
                    // Device name as a disabled header, so it is clear which icon was clicked.
                    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, entry->nid.szTip);
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                }
                AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show Devices...");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_TRAY_REFRESH, L"Refresh Now");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hWnd);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, nullptr);
                DestroyMenu(menu);
                PostMessageW(hWnd, WM_NULL, 0, 0);
            }
            else if (lParam == WM_LBUTTONUP) {
                PostMessageW(hWnd, WM_REFRESH_SCAN, 0, 0);
            }
            else if (lParam == WM_LBUTTONDBLCLK) {
                PostMessageW(hWnd, WM_COMMAND, ID_TRAY_SHOW, 0);
            }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_EXIT:
            DestroyWindow(hWnd);
            return 0;
        case ID_TRAY_REFRESH:
            DoScan(hWnd);
            return 0;
        case ID_TRAY_SHOW:
            ShowDevicesWindow();
            return 0;
        }
        return 0;

    case WM_SCAN_COMPLETE:
        SyncTrayIcons(hWnd);
        if (g_win && IsWindowVisible(g_win))
            RefreshDeviceList();
        return 0;

    case WM_REFRESH_SCAN:
        DoScan(hWnd);
        return 0;

    case WM_BT_DEVICE_CHANGED:
        // A connect/disconnect can raise several watcher events at once; debounce
        // them into one scan so the refresh is fast but not duplicated.
        SetTimer(hWnd, 2, 350, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == 2)
            KillTimer(hWnd, 2);
        DoScan(hWnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, 1);
        KillTimer(hWnd, 2);
        StopBluetoothDeviceWatcher();
        for (auto& kv : g_trays)
            RemoveTrayEntry(kv.second);
        g_trays.clear();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    g_hInst = hInstance;

    // A tray app is easy to launch twice, which would produce two sets of icons.
    // If an instance is already running, just open its device list instead.
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\BluetoothBatteryTrayInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND running = FindWindowW(L"BluetoothBatteryTrayClass", L"Bluetooth Battery Monitor"))
            PostMessageW(running, WM_COMMAND, ID_TRAY_SHOW, 0);
        if (instanceMutex)
            CloseHandle(instanceMutex);
        return 0;
    }

    // WIC needs COM; all image work happens on this (UI) thread.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadDeviceIconPaths();
    LoadTrayIds();

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Opt the process into dark mode before any window exists; the setting only
    // applies to windows and controls created afterwards.
    darkmode::Initialize();

    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"BluetoothBatteryTrayClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc))
        return 0;

    WNDCLASSEXW wcDevices = {};
    wcDevices.cbSize = sizeof(WNDCLASSEXW);
    wcDevices.lpfnWndProc = DevicesWndProc;
    wcDevices.hInstance = hInstance;
    wcDevices.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcDevices.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wcDevices.lpszClassName = L"BluetoothBatteryWindowClass";
    wcDevices.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wcDevices.hIconSm = wcDevices.hIcon;
    if (!RegisterClassExW(&wcDevices))
        return 0;

    g_hWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"BluetoothBatteryTrayClass", L"Bluetooth Battery Monitor",
        WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr
    );

    if (!g_hWnd)
        return 0;

    ShowWindow(g_hWnd, SW_HIDE);

    // Registered by the shell; used to restore tray icons after Explorer restarts.
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    // React to Bluetooth connect/disconnect immediately instead of waiting for
    // the next 30-second poll.
    StartBluetoothDeviceWatcher(g_hWnd, WM_BT_DEVICE_CHANGED);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    if (instanceMutex)
        CloseHandle(instanceMutex);
    return (int)msg.wParam;
}
