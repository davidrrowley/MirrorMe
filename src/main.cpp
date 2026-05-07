#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include "resource.h"

#include <d3d11.h>
#include <dxgi.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace {

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

constexpr wchar_t kWindowClassName[] = L"MirrorMeMainWindow";
constexpr wchar_t kWindowTitle[] = L"MirrorMe";

constexpr UINT_PTR kFrameTimerId = 1;
constexpr UINT kFrameIntervalMs = 16;
constexpr UINT_PTR kNotchTimerId = 2;
constexpr UINT kNotchTimerMs = 16;   // ~60 fps animation
constexpr int kNotchHideDelayMs = 1800; // ms idle before hiding
constexpr int kNotchPeekPx = 4;        // pixels visible when hidden

// Custom popup menu layout
constexpr int  kPopW         = 280;
constexpr int  kPopItemH     = 34;
constexpr int  kPopSepH      = 8;
constexpr int  kPopHdrH      = 26;
constexpr int  kPopListItemH = 44;   // fits 2 wrapped lines per app name
constexpr int  kPopMaxRows   = 7;    // visible rows before scrollbar
constexpr int  kPopPadL      = 14;

constexpr UINT kMenuZoomIn = 1001;
constexpr UINT kMenuZoomOut = 1002;
constexpr UINT kMenuResetZoom = 1003;
constexpr UINT kMenuExit = 1004;
constexpr UINT kOpacityMenuBase = 40000;
constexpr UINT kDisplayModeForegroundCmd = 50001;
constexpr UINT kDisplayModeBackgroundCmd = 50002;
constexpr int  kHotkeyZoomInId = 1;
constexpr int  kHotkeyZoomOutId = 2;
constexpr int  kHotkeyResetZoomId = 3;
constexpr int  kHotkeyMirrorForegroundId = 4;
constexpr int  kHotkeyMirrorForegroundAltId = 5;
constexpr int  kHotkeyResetZoomAltId = 6;
constexpr UINT kTrayMenuToggleVisibility = 1101;
constexpr UINT kTrayMenuExit = 1102;
constexpr UINT kTrayMenuStartMinimized = 1104;
constexpr UINT kTrayMenuMirrorForeground = 1105;

constexpr UINT kSourceMenuBase = 20000;
constexpr UINT kMonitorMenuBase = 30000;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMsg = WM_APP + 10;

constexpr int kNotchWidth = 140;
constexpr int kNotchHeight = 28;

struct WindowEntry {
    HWND hwnd = nullptr;
    std::wstring title;
};

struct MonitorEntry {
    HMONITOR handle = nullptr;
    MONITORINFOEXW info{};
};

enum class NotchState { Visible, Hiding, Hidden, Showing };
enum class DisplayMode { ForegroundExclusive, BackgroundUnderlay };

struct AppState {
    HWND window = nullptr;
    HWND sourceWindow = nullptr;
    HMONITOR targetMonitor = nullptr;
    RECT notchRect{0, 0, 0, 0};      // logical full rect (centred, at y=0)
    int  notchOffsetY = 0;           // pixels the notch is shifted up (0=fully shown)
    NotchState notchState = NotchState::Visible;
    int  notchIdleMs = 0;            // ms since mouse left notch zone
    float zoom = 1.0f;
    POINT panOffset{0, 0};
    bool dragActive = false;
    POINT dragStart{0, 0};
    POINT panAtDragStart{0, 0};
    bool useWgcPath = true;
    bool wgcCaptureActive = false;
    int  opacityPercent = 100;
    std::vector<WindowEntry> sourceWindows;
    std::vector<MonitorEntry> monitors;
};

AppState g_state;
static bool g_appExiting = false;
static UINT g_taskbarCreatedMsg = 0;
static bool g_startMinimizedToTray = false;
static DisplayMode g_displayMode = DisplayMode::ForegroundExclusive;

// ── Popup menu state ────────────────────────────────────────────────────────
// Label = plain action, NavItem = drill-in (shows ›), BackItem = ← Back
enum class PItemType { Label, NavItem, BackItem, Separator };
struct PItem {
    PItemType    type   = PItemType::Label;
    std::wstring text;
    UINT         cmdId  = 0;
    bool         accent = false;
    int          y = 0, h = 0;
};
enum class PopView { Root, Sources, Monitors, Transparency, DisplayMode };
static std::vector<PItem> g_popItems;
static PopView            g_popView       = PopView::Root;
static int                g_popHover      = -1;
static int                g_popListY      = 0;
static int                g_popListH      = 0;
static HWND               g_popHwnd       = nullptr;
static HWND               g_popList       = nullptr;
static HWND               g_popParent     = nullptr;
static HFONT              g_popFont       = nullptr;
static HFONT              g_popFontHdr    = nullptr;
static HBRUSH             g_popListBrush  = nullptr;

// Nav virtual command IDs (never forwarded to main window)
constexpr UINT kNavSources  = 2001;
constexpr UINT kNavMonitors = 2002;
constexpr UINT kNavBack     = 2003;
constexpr UINT kNavTransparency = 2004;
constexpr UINT kNavDisplayMode = 2005;

// Popup colors
const COLORREF kPC_Bg     = RGB(22,  22,  26);
const COLORREF kPC_Hover  = RGB(52,  52,  60);
const COLORREF kPC_Text   = RGB(210, 210, 210);
const COLORREF kPC_Dim    = RGB(105, 105, 115);
const COLORREF kPC_Sep    = RGB(52,  52,  62);
const COLORREF kPC_Accent = RGB(0,   120, 212);

// Forward declaration — defined after WgcCapture.
RECT ComputeDestinationRect(int srcWidth, int srcHeight, const RECT& clientRect, float zoom, POINT pan = {});

class WgcCapture {
public:
    bool IsSupported() const {
        return GraphicsCaptureSession::IsSupported();
    }

    bool Start(HWND sourceWindow) {
        Stop();

        if (!IsSupported() || sourceWindow == nullptr || !IsWindow(sourceWindow)) {
            return false;
        }

        if (!EnsureDevice()) {
            return false;
        }

        try {
            auto item = CreateItemForWindow(sourceWindow);
            if (!item) {
                return false;
            }

            const auto size = item.Size();
            if (size.Width <= 0 || size.Height <= 0) {
                return false;
            }

            m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_d3dDevice,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                size);

            m_session = m_framePool.CreateCaptureSession(item);
            m_frameArrivedToken = m_framePool.FrameArrived({this, &WgcCapture::OnFrameArrived});
            m_session.StartCapture();
            m_sourceWindow = sourceWindow;
            return true;
        } catch (...) {
            Stop();
            return false;
        }
    }

    void Stop() {
        std::scoped_lock lock(m_frameMutex);

        if (m_framePool && m_frameArrivedToken.value != 0) {
            m_framePool.FrameArrived(m_frameArrivedToken);
        }

        m_frameArrivedToken = {};
        m_session = nullptr;
        m_framePool = nullptr;
        m_latestTexture = nullptr;
        m_stagingTexture = nullptr;
        m_sourceWindow = nullptr;
        m_frameWidth = 0;
        m_frameHeight = 0;
    }

    bool DrawLatestFrame(HDC targetDc, const RECT& clientRect, float zoom) {
        winrt::com_ptr<ID3D11Texture2D> frameTexture;
        int frameWidth = 0;
        int frameHeight = 0;

        {
            std::scoped_lock lock(m_frameMutex);
            if (!m_latestTexture) {
                return false;
            }

            frameTexture = m_latestTexture;
            frameWidth = m_frameWidth;
            frameHeight = m_frameHeight;
        }

        if (!frameTexture || frameWidth <= 0 || frameHeight <= 0) {
            return false;
        }

        D3D11_TEXTURE2D_DESC sourceDesc{};
        frameTexture->GetDesc(&sourceDesc);

        if (!EnsureStagingTexture(sourceDesc)) {
            return false;
        }

        m_d3dContext->CopyResource(m_stagingTexture.get(), frameTexture.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_d3dContext->Map(m_stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        // Use RowPitch/4 as biWidth so GDI reads the correct GPU-aligned row stride.
        // StretchDIBits source rect still samples only [0..frameWidth] columns.
        bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(mapped.RowPitch / 4);
        bitmapInfo.bmiHeader.biHeight = -frameHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        const RECT destination = ComputeDestinationRect(frameWidth, frameHeight, clientRect, zoom, g_state.panOffset);

        SetStretchBltMode(targetDc, HALFTONE);
        StretchDIBits(
            targetDc,
            destination.left,
            destination.top,
            destination.right - destination.left,
            destination.bottom - destination.top,
            0,
            0,
            frameWidth,
            frameHeight,
            mapped.pData,
            &bitmapInfo,
            DIB_RGB_COLORS,
            SRCCOPY);

        m_d3dContext->Unmap(m_stagingTexture.get(), 0);
        return true;
    }

private:
    static GraphicsCaptureItem CreateItemForWindow(HWND hwnd) {
        auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{nullptr};

        check_hresult(interop->CreateForWindow(
            hwnd,
            guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            put_abi(item)));

        return item;
    }

    bool EnsureDevice() {
        if (m_dxgiDevice && m_d3dDevice) {
            return true;
        }

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };

        winrt::com_ptr<ID3D11Device> d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext> d3dContext;
        D3D_FEATURE_LEVEL selectedLevel{};

        const HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            d3dDevice.put(),
            &selectedLevel,
            d3dContext.put());

        if (FAILED(hr)) {
            return false;
        }

        m_d3dContext = d3dContext;
        m_dxgiDevice = d3dDevice.as<IDXGIDevice>();

        // CreateDirect3D11DeviceFromDXGIDevice outputs IInspectable** but winrt::put_abi
        // returns void** — cast is safe because IDirect3DDevice IS IInspectable at ABI level.
        const HRESULT hrWrap = CreateDirect3D11DeviceFromDXGIDevice(
            m_dxgiDevice.get(),
            reinterpret_cast<::IInspectable**>(winrt::put_abi(m_d3dDevice)));
        if (FAILED(hrWrap)) {
            m_dxgiDevice = nullptr;
            m_d3dContext = nullptr;
            return false;
        }
        return true;
    }

    bool EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& sourceDesc) {
        D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.MiscFlags = 0;

        bool recreate = false;
        if (!m_stagingTexture) {
            recreate = true;
        } else {
            D3D11_TEXTURE2D_DESC current{};
            m_stagingTexture->GetDesc(&current);
            recreate =
                current.Width != stagingDesc.Width ||
                current.Height != stagingDesc.Height ||
                current.Format != stagingDesc.Format;
        }

        if (!recreate) {
            return true;
        }

        m_stagingTexture = nullptr;
        return SUCCEEDED(m_dxgiDevice.as<ID3D11Device>()->CreateTexture2D(&stagingDesc, nullptr, m_stagingTexture.put()));
    }

    void OnFrameArrived(Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) {
                return;
            }

            auto surface = frame.Surface();
            winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
            surface.as(access);

            winrt::com_ptr<ID3D11Texture2D> texture;
            check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));

            const auto size = frame.ContentSize();

            std::scoped_lock lock(m_frameMutex);
            m_latestTexture = texture;
            m_frameWidth = size.Width;
            m_frameHeight = size.Height;

            if (size.Width > 0 && size.Height > 0 && m_framePool) {
                m_framePool.Recreate(m_d3dDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            }
        } catch (...) {
            // Ignore transient frame errors and continue capture.
        }
    }

private:
    HWND m_sourceWindow = nullptr;
    IDirect3DDevice m_d3dDevice{nullptr};
    winrt::com_ptr<IDXGIDevice> m_dxgiDevice;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;

    Direct3D11CaptureFramePool m_framePool{nullptr};
    GraphicsCaptureSession m_session{nullptr};
    event_token m_frameArrivedToken{};

    std::mutex m_frameMutex;
    winrt::com_ptr<ID3D11Texture2D> m_latestTexture;
    winrt::com_ptr<ID3D11Texture2D> m_stagingTexture;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
};

WgcCapture g_wgcCapture;

std::wstring TrimWindowTitle(const std::wstring& input) {
    const auto begin = input.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) {
        return L"";
    }

    const auto end = input.find_last_not_of(L" \t\r\n");
    return input.substr(begin, end - begin + 1);
}

static bool IsShellOrDesktopWindowClass(HWND hwnd) {
    wchar_t className[128] = {};
    if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) <= 0) {
        return false;
    }

    return wcscmp(className, L"Progman") == 0 ||
           wcscmp(className, L"WorkerW") == 0 ||
           wcscmp(className, L"Shell_TrayWnd") == 0 ||
           wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

static bool IsWindowCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    const HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

static bool IsUsableSourceWindow(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd) || hwnd == g_state.window) {
        return false;
    }
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }
    if (IsWindowCloaked(hwnd) || IsShellOrDesktopWindowClass(hwnd)) {
        return false;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 || (exStyle & WS_EX_NOACTIVATE) != 0) {
        return false;
    }
    const HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner != nullptr && (exStyle & WS_EX_APPWINDOW) == 0) {
        return false;
    }
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return false;
    }
    return true;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* out = reinterpret_cast<std::vector<WindowEntry>*>(lParam);
    if (!out) {
        return FALSE;
    }

    if (hwnd == g_state.window) {
        return TRUE;
    }

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }

    if (IsWindowCloaked(hwnd) || IsShellOrDesktopWindowClass(hwnd)) {
        return TRUE;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 || (exStyle & WS_EX_NOACTIVATE) != 0) {
        return TRUE;
    }

    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return TRUE;
    }

    const HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner != nullptr && (exStyle & WS_EX_APPWINDOW) == 0) {
        return TRUE;
    }

    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return TRUE;
    }

    std::wstring title(length + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
    title = TrimWindowTitle(title);

    if (title.empty()) {
        return TRUE;
    }

    out->push_back({hwnd, title});
    return TRUE;
}

BOOL CALLBACK EnumMonitorsProc(HMONITOR monitor, HDC, LPRECT, LPARAM lParam) {
    auto* out = reinterpret_cast<std::vector<MonitorEntry>*>(lParam);
    if (!out) {
        return FALSE;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    out->push_back({monitor, info});
    return TRUE;
}

void RefreshSourceWindows() {
    g_state.sourceWindows.clear();
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&g_state.sourceWindows));

    std::sort(g_state.sourceWindows.begin(), g_state.sourceWindows.end(), [](const WindowEntry& a, const WindowEntry& b) {
        return a.title < b.title;
    });

    if (g_state.sourceWindow != nullptr && !IsWindow(g_state.sourceWindow)) {
        g_state.sourceWindow = nullptr;
    }
}

// Prefer the first non-primary monitor; fall back to the primary if there is only one.
HMONITOR PickDefaultMonitor() {
    for (const auto& entry : g_state.monitors) {
        if (!(entry.info.dwFlags & MONITORINFOF_PRIMARY)) {
            return entry.handle;
        }
    }
    // All monitors are primary (single-monitor system) — use the window's monitor.
    return MonitorFromWindow(g_state.window, MONITOR_DEFAULTTOPRIMARY);
}

void RefreshMonitors() {
    g_state.monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, reinterpret_cast<LPARAM>(&g_state.monitors));

    if (g_state.targetMonitor == nullptr) {
        g_state.targetMonitor = PickDefaultMonitor();
        return;
    }

    const bool stillExists = std::any_of(g_state.monitors.begin(), g_state.monitors.end(), [](const MonitorEntry& entry) {
        return entry.handle == g_state.targetMonitor;
    });

    if (!stillExists) {
        g_state.targetMonitor = PickDefaultMonitor();
    }
}

RECT ComputeDestinationRect(int srcWidth, int srcHeight, const RECT& clientRect, float zoom, POINT pan) {
    RECT result = clientRect;

    const int clientWidth = clientRect.right - clientRect.left;
    const int clientHeight = clientRect.bottom - clientRect.top;

    if (srcWidth <= 0 || srcHeight <= 0 || clientWidth <= 0 || clientHeight <= 0) {
        return result;
    }

    const double fitScale = std::min(
        static_cast<double>(clientWidth) / static_cast<double>(srcWidth),
        static_cast<double>(clientHeight) / static_cast<double>(srcHeight));

    const double scaled = fitScale * std::max(0.1f, zoom);
    const int dstWidth = std::max(1, static_cast<int>(srcWidth * scaled));
    const int dstHeight = std::max(1, static_cast<int>(srcHeight * scaled));

    // Centre then apply pan, clamped so at least half the image stays visible.
    const int maxPanX = std::max(0, dstWidth / 2);
    const int maxPanY = std::max(0, dstHeight / 2);
    const int clampedPanX = std::max(-maxPanX, std::min(maxPanX, static_cast<int>(pan.x)));
    const int clampedPanY = std::max(-maxPanY, std::min(maxPanY, static_cast<int>(pan.y)));

    const int left = (clientWidth - dstWidth) / 2 + clampedPanX;
    const int top  = (clientHeight - dstHeight) / 2 + clampedPanY;

    result.left = left;
    result.top = top;
    result.right = left + dstWidth;
    result.bottom = top + dstHeight;
    return result;
}

void UpdateNotchRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);

    const UINT dpi = GetDpiForWindow(hwnd);
    const int scaledW = MulDiv(kNotchWidth,  dpi, 96);
    const int scaledH = MulDiv(kNotchHeight, dpi, 96);

    const int width = client.right - client.left;
    const int x = std::max(0, (width - scaledW) / 2);

    g_state.notchRect.left = x;
    g_state.notchRect.top = 0;
    g_state.notchRect.right = x + scaledW;
    g_state.notchRect.bottom = scaledH;
}

void ApplyTargetMonitorPlacement() {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);

    if (!GetMonitorInfoW(g_state.targetMonitor, &info)) {
        return;
    }

    const RECT& monitorRect = info.rcMonitor;
    const bool foregroundExclusive = (g_displayMode == DisplayMode::ForegroundExclusive);
    SetWindowPos(
        g_state.window,
        foregroundExclusive ? HWND_TOPMOST : HWND_BOTTOM,
        monitorRect.left,
        monitorRect.top,
        monitorRect.right - monitorRect.left,
        monitorRect.bottom - monitorRect.top,
        SWP_SHOWWINDOW | (foregroundExclusive ? 0 : SWP_NOACTIVATE));
}

void DrawMirrorContentGdiFallback(HDC paintDc, const RECT& clientRect) {
    HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(paintDc, &clientRect, background);
    DeleteObject(background);

    if (g_state.sourceWindow == nullptr || !IsWindow(g_state.sourceWindow) || IsIconic(g_state.sourceWindow)) {
        SetBkMode(paintDc, TRANSPARENT);
        SetTextColor(paintDc, RGB(220, 220, 220));
        const wchar_t* text = L"Select Source Window from the notch menu";
        TextOutW(paintDc, 28, 40, text, lstrlenW(text));
        return;
    }

    RECT sourceRect{};
    if (!GetWindowRect(g_state.sourceWindow, &sourceRect)) {
        return;
    }

    const int srcWidth = sourceRect.right - sourceRect.left;
    const int srcHeight = sourceRect.bottom - sourceRect.top;
    if (srcWidth <= 0 || srcHeight <= 0) {
        return;
    }

    HDC sourceBufferDc = CreateCompatibleDC(paintDc);
    HBITMAP sourceBitmap = CreateCompatibleBitmap(paintDc, srcWidth, srcHeight);
    HGDIOBJ oldBitmap = SelectObject(sourceBufferDc, sourceBitmap);

    bool captured = PrintWindow(g_state.sourceWindow, sourceBufferDc, PW_RENDERFULLCONTENT) != FALSE;
    if (!captured) {
        HDC sourceWindowDc = GetWindowDC(g_state.sourceWindow);
        if (sourceWindowDc != nullptr) {
            captured = BitBlt(sourceBufferDc, 0, 0, srcWidth, srcHeight, sourceWindowDc, 0, 0, SRCCOPY) != FALSE;
            ReleaseDC(g_state.sourceWindow, sourceWindowDc);
        }
    }

    if (captured) {
        const RECT dest = ComputeDestinationRect(srcWidth, srcHeight, clientRect, g_state.zoom, g_state.panOffset);
        SetStretchBltMode(paintDc, HALFTONE);
        StretchBlt(
            paintDc,
            dest.left,
            dest.top,
            dest.right - dest.left,
            dest.bottom - dest.top,
            sourceBufferDc,
            0,
            0,
            srcWidth,
            srcHeight,
            SRCCOPY);
    }

    SelectObject(sourceBufferDc, oldBitmap);
    DeleteObject(sourceBitmap);
    DeleteDC(sourceBufferDc);
}

void DrawMirrorContent(HDC paintDc, const RECT& clientRect) {
    if (g_state.sourceWindow == nullptr || !IsWindow(g_state.sourceWindow) || IsIconic(g_state.sourceWindow)) {
        SetBkMode(paintDc, TRANSPARENT);
        SetTextColor(paintDc, RGB(220, 220, 220));
        const wchar_t* text = L"Select Source Window from the notch menu";
        TextOutW(paintDc, 28, 40, text, lstrlenW(text));
        return;
    }

    bool drawnByWgc = false;
    if (g_state.useWgcPath && g_state.wgcCaptureActive) {
        drawnByWgc = g_wgcCapture.DrawLatestFrame(paintDc, clientRect, g_state.zoom);
    }

    if (!drawnByWgc) {
        DrawMirrorContentGdiFallback(paintDc, clientRect);
    }
}

void DrawNotch(HDC paintDc) {
    // Pill rect shifted by current animation offset
    RECT r = g_state.notchRect;
    OffsetRect(&r, 0, -g_state.notchOffsetY);

    // Soft drop-shadow: draw a slightly larger, darker, semi-blurred pill underneath.
    // GDI has no alpha blend for shapes, so approximate with two offset fills.
    const HWND notchHwnd = g_state.window;
    const UINT notchDpi  = notchHwnd ? GetDpiForWindow(notchHwnd) : 96;
    const int shadowBlur = MulDiv(4, notchDpi, 96);
    for (int i = shadowBlur; i >= 1; --i) {
        RECT sr = r;
        InflateRect(&sr, i, i);
        OffsetRect(&sr, 0, i);
        const int alpha = 30 - i * 5; // fade out
        HBRUSH sb = CreateSolidBrush(RGB(alpha, alpha, alpha));
        const int radius = (sr.bottom - sr.top); // full round
        HPEN sp = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ op = SelectObject(paintDc, sp);
        HGDIOBJ ob = SelectObject(paintDc, sb);
        RoundRect(paintDc, sr.left, sr.top, sr.right, sr.bottom, radius, radius);
        SelectObject(paintDc, op); SelectObject(paintDc, ob);
        DeleteObject(sb); DeleteObject(sp);
    }

    // Pill background
    const int radius = (r.bottom - r.top); // makes a perfect pill
    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 18));
    HPEN   np = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ oldPen  = SelectObject(paintDc, np);
    HGDIOBJ oldBrush = SelectObject(paintDc, bg);
    RoundRect(paintDc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(paintDc, oldPen);
    SelectObject(paintDc, oldBrush);
    DeleteObject(bg);
    DeleteObject(np);

    // Only draw label text when notch is meaningfully visible
    if (g_state.notchOffsetY < (g_state.notchRect.bottom - g_state.notchRect.top) * 3 / 4) {
        // Clip to pill so text doesn't bleed outside
        HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, radius, radius);
        SelectClipRgn(paintDc, rgn);
        DeleteObject(rgn);

        SetBkMode(paintDc, TRANSPARENT);
        SetTextColor(paintDc, RGB(210, 210, 210));

        // Build a small Segoe UI font scaled to notch height
        LOGFONTW lf{};
        lf.lfHeight  = -(g_state.notchRect.bottom - g_state.notchRect.top - 8);
        lf.lfWeight  = FW_MEDIUM;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        HFONT font = CreateFontIndirectW(&lf);
        HGDIOBJ oldFont = SelectObject(paintDc, font);

        RECT tr = r;
        DrawTextW(paintDc, L"MirrorMe", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(paintDc, oldFont);
        DeleteObject(font);
        SelectClipRgn(paintDc, nullptr); // remove clip
    }
}

// ── Custom dark popup menu helpers ──────────────────────────────────────────

// Forward-declare so PopupNavigateTo can be called from the wndproc
static void PopupNavigateTo(HWND hwnd, PopView view);

static int PopupWidthForView(PopView view) {
    return (view == PopView::Root) ? kPopW : (kPopW * 9) / 4;
}

static POINT PopupAnchorBelowNotch(HWND parentHwnd) {
    const int notchCx = (g_state.notchRect.left + g_state.notchRect.right) / 2;
    const int notchVisBottom = g_state.notchRect.bottom - g_state.notchOffsetY;
    POINT pt{ notchCx, notchVisBottom + 6 };
    ClientToScreen(parentHwnd, &pt);
    return pt;
}

static POINT PopupClampToMonitor(const POINT& desiredTopLeft, int width, int height) {
    HMONITOR hmon = MonitorFromPoint(desiredTopLeft, MONITOR_DEFAULTTONEAREST);
    MONITORINFO minfo{ sizeof(minfo) };
    GetMonitorInfoW(hmon, &minfo);

    POINT clamped = desiredTopLeft;
    clamped.x = std::max(minfo.rcWork.left, std::min(clamped.x, minfo.rcWork.right - width));
    clamped.y = std::max(minfo.rcWork.top, std::min(clamped.y, minfo.rcWork.bottom - height));
    return clamped;
}

static void PopupLayout() {
    int y = 8;
    for (auto& it : g_popItems) {
        it.y = y;
        it.h = (it.type == PItemType::Separator) ? kPopSepH : kPopItemH;
        y += it.h;
    }
}

static int PopupHitTest(int my) {
    for (int i = 0; i < (int)g_popItems.size(); ++i) {
        const auto& it = g_popItems[i];
        if (it.type != PItemType::Separator)
            if (my >= it.y && my < it.y + it.h) return i;
    }
    return -1;
}

static void PopupPaintItems(HDC dc) {
    const int popW = PopupWidthForView(g_popView);
    for (int i = 0; i < (int)g_popItems.size(); ++i) {
        const auto& it = g_popItems[i];

        if (it.type == PItemType::Separator) {
            HPEN sp = CreatePen(PS_SOLID, 1, kPC_Sep);
            HGDIOBJ op = SelectObject(dc, sp);
            const int my = it.y + it.h / 2;
            MoveToEx(dc, 10, my, nullptr);
            LineTo(dc, popW - 10, my);
            SelectObject(dc, op);
            DeleteObject(sp);
            continue;
        }

        const bool hov = (i == g_popHover);
        if (hov) {
            HBRUSH hb = CreateSolidBrush(kPC_Hover);
            HPEN   hp = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ op = SelectObject(dc, hp), ob = SelectObject(dc, hb);
            RoundRect(dc, 4, it.y + 1, popW - 4, it.y + it.h - 1, 6, 6);
            SelectObject(dc, op); SelectObject(dc, ob);
            DeleteObject(hb); DeleteObject(hp);
        }

        SetBkMode(dc, TRANSPARENT);

        if (it.type == PItemType::BackItem) {
            SetTextColor(dc, hov ? kPC_Text : kPC_Dim);
            HGDIOBJ of = SelectObject(dc, g_popFontHdr);
            RECT tr{ kPopPadL, it.y, popW, it.y + it.h };
            DrawTextW(dc, L"\u2190  Back", -1, &tr, DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, of);
            continue;
        }

        // Label or NavItem
        SetTextColor(dc, it.accent ? kPC_Accent : kPC_Text);
        HGDIOBJ of = SelectObject(dc, g_popFont);
        RECT tr{ kPopPadL, it.y, popW - kPopPadL, it.y + it.h };
        DrawTextW(dc, it.text.c_str(), -1, &tr, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (it.type == PItemType::NavItem) {
            RECT cr{ popW - 28, it.y, popW - 8, it.y + it.h };
            SetTextColor(dc, kPC_Dim);
            DrawTextW(dc, L"\u203a", -1, &cr, DT_VCENTER | DT_SINGLELINE | DT_RIGHT);
        }
        SelectObject(dc, of);
    }
}

static void PopupDestroyList() {
    if (g_popList) { DestroyWindow(g_popList); g_popList = nullptr; }
    if (g_popListBrush) { DeleteObject(g_popListBrush); g_popListBrush = nullptr; }
}

static void PopupCreateList(HWND hwnd) {
    PopupDestroyList();
    RefreshSourceWindows();
    const int popW = PopupWidthForView(g_popView);
    const int srcCount = (int)g_state.sourceWindows.size();
    const int visRows  = std::min(std::max(srcCount, 1), kPopMaxRows);
    g_popListH = visRows * kPopListItemH;

    const int lastItemBottom = g_popItems.empty() ? 8
        : g_popItems.back().y + g_popItems.back().h;
    g_popListY = lastItemBottom + 4;

    g_popList = CreateWindowExW(0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        0, g_popListY, popW, g_popListH,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_popList, LB_SETITEMHEIGHT, 0, kPopListItemH);
    if (srcCount == 0) {
        SendMessageW(g_popList, LB_ADDSTRING, 0, (LPARAM)L"No visible windows");
    } else {
        for (const auto& w : g_state.sourceWindows)
            SendMessageW(g_popList, LB_ADDSTRING, 0, (LPARAM)w.title.c_str());
    }
}

static void PopupNavigateTo(HWND hwnd, PopView view) {
    g_popView  = view;
    g_popHover = -1;
    g_popItems.clear();
    PopupDestroyList();
    g_popListH = 0;

    auto addItem = [](PItemType t, const wchar_t* text, UINT id, bool accent = false) {
        g_popItems.push_back({ t, text, id, accent });
    };
    auto addLabel = [&](const wchar_t* text, UINT id, bool accent = false) {
        addItem(PItemType::Label, text, id, accent);
    };
    auto addNav = [&](const wchar_t* text, UINT id) {
        addItem(PItemType::NavItem, text, id);
    };
    auto addSep = [&]() { addItem(PItemType::Separator, L"", 0); };
    auto addBack = [&]() {
        addItem(PItemType::BackItem, L"", kNavBack);
        addSep();
    };

    switch (view) {
    case PopView::Root:
        addLabel(L"Zoom In",    kMenuZoomIn);
        addLabel(L"Zoom Out",   kMenuZoomOut);
        addLabel(L"Reset Zoom", kMenuResetZoom);
        addSep();
        addLabel(L"Mirror Foreground Window", kTrayMenuMirrorForeground);
        addSep();
        addNav(L"Select Source Window",  kNavSources);
        addNav(L"Select Target Monitor", kNavMonitors);
        addNav(L"Display Mode", kNavDisplayMode);
        addNav(L"Transparency", kNavTransparency);
        addSep();
        addLabel(L"Exit", kMenuExit);
        break;
    case PopView::Sources:
        addBack();
        break;
    case PopView::Monitors:
        addBack();
        RefreshMonitors();
        for (size_t i = 0; i < g_state.monitors.size(); ++i) {
            std::wstring label = g_state.monitors[i].info.szDevice;
            const bool cur = g_state.monitors[i].handle == g_state.targetMonitor;
            if (cur) label += L" (current)";
            addLabel(label.c_str(), kMonitorMenuBase + static_cast<UINT>(i), cur);
        }
        break;
    case PopView::Transparency:
        addBack();
        for (int pct = 0; pct <= 100; pct += 10) {
            const std::wstring label = std::to_wstring(pct) + L"%";
            addLabel(label.c_str(), kOpacityMenuBase + static_cast<UINT>(pct), pct == g_state.opacityPercent);
        }
        break;
    case PopView::DisplayMode:
        addBack();
        addLabel(L"Foreground Exclusive", kDisplayModeForegroundCmd,
            g_displayMode == DisplayMode::ForegroundExclusive);
        addLabel(L"Background Underlay", kDisplayModeBackgroundCmd,
            g_displayMode == DisplayMode::BackgroundUnderlay);
        break;
    }

    PopupLayout();

    if (view == PopView::Sources)
        PopupCreateList(hwnd);

    const int newW = PopupWidthForView(view);
    const int newH = (g_popItems.empty() ? 0 : g_popItems.back().y + g_popItems.back().h)
                   + (g_popList ? g_popListH + 8 : 8);

    const POINT anchor = PopupAnchorBelowNotch(g_popParent);
    const POINT desiredTopLeft{ anchor.x - (newW / 2), anchor.y };
    const POINT clamped = PopupClampToMonitor(desiredTopLeft, newW, newH);
    SetWindowPos(hwnd, nullptr, clamped.x, clamped.y, newW, newH, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cl; GetClientRect(hwnd, &cl);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cl.right, cl.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        HBRUSH bgBr = CreateSolidBrush(kPC_Bg);
        FillRect(mem, &cl, bgBr); DeleteObject(bgBr);
        PopupPaintItems(mem);
        BitBlt(hdc, 0, 0, cl.right, cl.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int newHov = PopupHitTest(GET_Y_LPARAM(lp));
        if (newHov != g_popHover) {
            g_popHover = newHov;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_popHover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const int idx = PopupHitTest(GET_Y_LPARAM(lp));
        if (idx < 0) return 0;
        const UINT cmdId = g_popItems[idx].cmdId;
        if (cmdId == kNavSources)  { PopupNavigateTo(hwnd, PopView::Sources);  return 0; }
        if (cmdId == kNavMonitors) { PopupNavigateTo(hwnd, PopView::Monitors); return 0; }
        if (cmdId == kNavDisplayMode) { PopupNavigateTo(hwnd, PopView::DisplayMode); return 0; }
        if (cmdId == kNavTransparency) { PopupNavigateTo(hwnd, PopView::Transparency); return 0; }
        if (cmdId == kNavBack)     { PopupNavigateTo(hwnd, PopView::Root);     return 0; }
        PostMessageW(g_popParent, WM_COMMAND, cmdId, 0);
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_popView != PopView::Root) PopupNavigateTo(hwnd, PopView::Root);
            else DestroyWindow(hwnd);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PopupDestroyList();
        g_popHwnd = nullptr;
        if (g_popFont)    { DeleteObject(g_popFont);    g_popFont    = nullptr; }
        if (g_popFontHdr) { DeleteObject(g_popFontHdr); g_popFontHdr = nullptr; }
        return 0;
    case WM_CTLCOLORLISTBOX: {
        if (!g_popListBrush) g_popListBrush = CreateSolidBrush(kPC_Bg);
        SetBkColor((HDC)wp, kPC_Bg);
        SetTextColor((HDC)wp, kPC_Text);
        return (LRESULT)g_popListBrush;
    }
    case WM_MEASUREITEM: {
        auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        mi->itemHeight = kPopListItemH;
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (!di) return FALSE;
        HDC dc = di->hDC;
        RECT r = di->rcItem;
        const bool sel = (di->itemState & ODS_SELECTED) != 0;
        HBRUSH bgBr = CreateSolidBrush(sel ? kPC_Hover : kPC_Bg);
        FillRect(dc, &r, bgBr); DeleteObject(bgBr);
        if (di->itemID == (UINT)-1) return TRUE;
        wchar_t buf[512] = {};
        SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)buf);
        const bool empty = g_state.sourceWindows.empty();
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, empty ? kPC_Dim : kPC_Text);
        HGDIOBJ of = SelectObject(dc, g_popFont);
        RECT tr{ r.left + kPopPadL, r.top + 6, r.right - kPopPadL, r.bottom - 6 };
        DrawTextW(dc, buf, -1, &tr, DT_WORDBREAK | DT_END_ELLIPSIS);
        SelectObject(dc, of);
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == LBN_SELCHANGE && g_popList) {
            const int sel = (int)SendMessageW(g_popList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR && !g_state.sourceWindows.empty()) {
                PostMessageW(g_popParent, WM_COMMAND, kSourceMenuBase + (UINT)sel, 0);
                DestroyWindow(hwnd);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowNotchMenu(HWND parentHwnd) {
    // Toggle: click notch again while open -> close
    if (g_popHwnd && IsWindow(g_popHwnd)) {
        DestroyWindow(g_popHwnd);
        return;
    }

    g_popParent = parentHwnd;
    g_popView   = PopView::Root;

    // Build DPI-scaled fonts
    const int dpi = GetDpiForWindow(parentHwnd);
    {
        LOGFONTW lf{};
        lf.lfHeight  = -MulDiv(10, dpi, 72);
        lf.lfWeight  = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        g_popFont    = CreateFontIndirectW(&lf);
        lf.lfHeight  = -MulDiv(8, dpi, 72);
        lf.lfWeight  = FW_SEMIBOLD;
        g_popFontHdr = CreateFontIndirectW(&lf);
    }

    // Build root items for initial height measurement
    g_popItems.clear();
    g_popHover = -1;
    g_popListH = 0;
    {
        auto add = [](PItemType t, const wchar_t* text, UINT id) {
            g_popItems.push_back({ t, text, id });
        };
        auto sep = []() { g_popItems.push_back({ PItemType::Separator }); };
        add(PItemType::Label,   L"Zoom In",               kMenuZoomIn);
        add(PItemType::Label,   L"Zoom Out",              kMenuZoomOut);
        add(PItemType::Label,   L"Reset Zoom",            kMenuResetZoom);
        sep();
        add(PItemType::Label,   L"Mirror Foreground Window", kTrayMenuMirrorForeground);
        sep();
        add(PItemType::NavItem, L"Select Source Window",  kNavSources);
        add(PItemType::NavItem, L"Select Target Monitor", kNavMonitors);
        add(PItemType::NavItem, L"Display Mode",          kNavDisplayMode);
        add(PItemType::NavItem, L"Transparency",          kNavTransparency);
        sep();
        add(PItemType::Label,   L"Exit",                  kMenuExit);
    }
    PopupLayout();
    const int initW = PopupWidthForView(PopView::Root);
    const int initH = g_popItems.back().y + g_popItems.back().h + 8;

    // Register window class once
    static bool s_classReg = false;
    if (!s_classReg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc   = PopupWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MirrorMePopup";
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassExW(&wc);
        s_classReg = true;
    }

    // Position: below the visible notch, centred horizontally
    const POINT anchor = PopupAnchorBelowNotch(parentHwnd);
    const POINT desiredTopLeft{ anchor.x - initW / 2, anchor.y };
    const POINT pt = PopupClampToMonitor(desiredTopLeft, initW, initH);

    g_popHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"MirrorMePopup", nullptr,
        WS_POPUP,
        pt.x, pt.y, initW, initH,
        parentHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_popHwnd) return;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_popHwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_popHwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    ShowWindow(g_popHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_popHwnd);
}

static void RequestAppExit(HWND hwnd) {
    g_appExiting = true;
    DestroyWindow(hwnd);
}

static void ShowMainWindow(HWND hwnd) {
    const bool foregroundExclusive = (g_displayMode == DisplayMode::ForegroundExclusive);
    ApplyTargetMonitorPlacement();
    if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, foregroundExclusive ? SW_SHOW : SW_SHOWNOACTIVATE);
    }
    if (foregroundExclusive) {
        SetForegroundWindow(hwnd);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void ToggleMainWindowVisibility(HWND hwnd) {
    if (IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowMainWindow(hwnd);
    }
}

static void ApplyWindowOpacity(HWND hwnd) {
    const int pct = std::max(0, std::min(100, g_state.opacityPercent));
    const BYTE alpha = static_cast<BYTE>((pct * 255) / 100);
    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
}

static std::wstring SettingsIniPath() {
    wchar_t appData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    std::wstring dir;
    if (n > 0 && n < MAX_PATH) {
        dir = appData;
    } else {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        dir = modulePath;
        const size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            dir.resize(pos);
        }
    }

    dir += L"\\MirrorMe";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

static void LoadSettings() {
    const std::wstring iniPath = SettingsIniPath();
    g_startMinimizedToTray = GetPrivateProfileIntW(
        L"MirrorMe", L"StartMinimizedToTray", 0, iniPath.c_str()) != 0;
    g_displayMode = GetPrivateProfileIntW(
        L"MirrorMe", L"ForegroundExclusive", 1, iniPath.c_str()) != 0
        ? DisplayMode::ForegroundExclusive
        : DisplayMode::BackgroundUnderlay;
    const int opacity = static_cast<int>(GetPrivateProfileIntW(
        L"MirrorMe", L"OpacityPercent", 100, iniPath.c_str()));
    g_state.opacityPercent = std::max(0, std::min(100, opacity));
}

static void SaveSettings() {
    const std::wstring iniPath = SettingsIniPath();
    WritePrivateProfileStringW(
        L"MirrorMe", L"StartMinimizedToTray",
        g_startMinimizedToTray ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(
        L"MirrorMe", L"ForegroundExclusive",
        (g_displayMode == DisplayMode::ForegroundExclusive) ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(
        L"MirrorMe", L"OpacityPercent",
        std::to_wstring(std::max(0, std::min(100, g_state.opacityPercent))).c_str(), iniPath.c_str());
}

static std::wstring BuildTrayTooltip() {
    std::wstring tip = L"MirrorMe";
    if (g_state.sourceWindow && IsWindow(g_state.sourceWindow)) {
        const int len = GetWindowTextLengthW(g_state.sourceWindow);
        if (len > 0) {
            std::wstring title(len + 1, L'\0');
            GetWindowTextW(g_state.sourceWindow, title.data(), static_cast<int>(title.size()));
            title = TrimWindowTitle(title);
            if (!title.empty()) {
                tip = L"MirrorMe: " + title;
            }
        }
    }
    if (tip.size() > 120) {
        tip.resize(117);
        tip += L"...";
    }
    return tip;
}

static void FillTrayIconData(HWND hwnd, NOTIFYICONDATAW& nid) {
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    HICON appIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        0, 0,
        LR_DEFAULTSIZE | LR_SHARED));
    nid.hIcon = appIcon ? appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    const std::wstring tip = BuildTrayTooltip();
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
}

static void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    FillTrayIconData(hwnd, nid);
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

static void UpdateTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    FillTrayIconData(hwnd, nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowTrayContextMenu(HWND hwnd) {
    RefreshSourceWindows();
    RefreshMonitors();

    HMENU root = CreatePopupMenu();
    HMENU src = CreatePopupMenu();
    HMENU mon = CreatePopupMenu();
    HMENU opa = CreatePopupMenu();
    HMENU mode = CreatePopupMenu();
    if (!root || !src || !mon || !opa || !mode) {
        if (root) DestroyMenu(root);
        if (src) DestroyMenu(src);
        if (mon) DestroyMenu(mon);
        if (opa) DestroyMenu(opa);
        if (mode) DestroyMenu(mode);
        return;
    }

    const wchar_t* toggleLabel = IsWindowVisible(hwnd) ? L"Hide MirrorMe" : L"Show MirrorMe";
    AppendMenuW(root, MF_STRING, kTrayMenuToggleVisibility, toggleLabel);
    AppendMenuW(root, MF_STRING, kTrayMenuMirrorForeground, L"Mirror Foreground Window");
    AppendMenuW(root,
        MF_STRING | (g_startMinimizedToTray ? MF_CHECKED : 0),
        kTrayMenuStartMinimized,
        L"Start Minimized To Tray");
    AppendMenuW(root, MF_SEPARATOR, 0, nullptr);

    if (g_state.sourceWindows.empty()) {
        AppendMenuW(src, MF_STRING | MF_GRAYED, 0, L"(No available windows)");
    } else {
        for (size_t i = 0; i < g_state.sourceWindows.size(); ++i) {
            UINT flags = MF_STRING;
            if (g_state.sourceWindows[i].hwnd == g_state.sourceWindow) {
                flags |= MF_CHECKED;
            }
            AppendMenuW(src, flags, kSourceMenuBase + static_cast<UINT>(i), g_state.sourceWindows[i].title.c_str());
        }
    }

    for (size_t i = 0; i < g_state.monitors.size(); ++i) {
        std::wstring label = g_state.monitors[i].info.szDevice;
        const bool cur = g_state.monitors[i].handle == g_state.targetMonitor;
        if (cur) label += L" (current)";
        UINT flags = MF_STRING;
        if (cur) flags |= MF_CHECKED;
        AppendMenuW(mon, flags, kMonitorMenuBase + static_cast<UINT>(i), label.c_str());
    }

    for (int pct = 0; pct <= 100; pct += 10) {
        const UINT flags = MF_STRING | ((pct == g_state.opacityPercent) ? MF_CHECKED : 0);
        const std::wstring label = std::to_wstring(pct) + L"%";
        AppendMenuW(opa, flags, kOpacityMenuBase + static_cast<UINT>(pct), label.c_str());
    }

    AppendMenuW(mode,
        MF_STRING | ((g_displayMode == DisplayMode::ForegroundExclusive) ? MF_CHECKED : 0),
        kDisplayModeForegroundCmd,
        L"Foreground Exclusive");
    AppendMenuW(mode,
        MF_STRING | ((g_displayMode == DisplayMode::BackgroundUnderlay) ? MF_CHECKED : 0),
        kDisplayModeBackgroundCmd,
        L"Background Underlay");

    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(src), L"Source Window");
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(mon), L"Target Monitor");
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(mode), L"Display Mode");
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(opa), L"Transparency");
    AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(root, MF_STRING, kTrayMenuExit, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(root, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
        pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(root);
}

static void MirrorForegroundWindowToDefaultMonitor(HWND hwnd) {
    HWND candidate = GetForegroundWindow();
    if (candidate) {
        candidate = GetAncestor(candidate, GA_ROOTOWNER);
    }

    if (!IsUsableSourceWindow(candidate)) {
        POINT pt{};
        GetCursorPos(&pt);
        candidate = WindowFromPoint(pt);
        if (candidate) {
            candidate = GetAncestor(candidate, GA_ROOTOWNER);
        }
    }

    if (!IsUsableSourceWindow(candidate)) {
        return;
    }

    // Toggle: pressing the hotkey again on the same window stops mirroring.
    if (candidate == g_state.sourceWindow && IsWindowVisible(hwnd)) {
        g_wgcCapture.Stop();
        g_state.wgcCaptureActive = false;
        g_state.sourceWindow = nullptr;
        ShowWindow(hwnd, SW_HIDE);
        UpdateTrayIcon(hwnd);
        return;
    }

    RefreshMonitors();
    g_state.targetMonitor = PickDefaultMonitor();
    g_state.sourceWindow = candidate;
    g_state.panOffset = {0, 0};
    g_state.wgcCaptureActive = g_state.useWgcPath && g_wgcCapture.Start(g_state.sourceWindow);
    ApplyTargetMonitorPlacement();
    UpdateTrayIcon(hwnd);
    ShowMainWindow(hwnd);
}

void HandleMenuCommand(HWND hwnd, UINT commandId) {
    switch (commandId) {
    case kMenuZoomIn:
        g_state.zoom = std::min(5.0f, g_state.zoom + 0.1f);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    case kMenuZoomOut:
        g_state.zoom = std::max(0.2f, g_state.zoom - 0.1f);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    case kMenuResetZoom:
        g_state.zoom = 1.0f;
        g_state.panOffset = {0, 0};
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    case kTrayMenuToggleVisibility:
        ToggleMainWindowVisibility(hwnd);
        return;
    case kTrayMenuMirrorForeground:
        MirrorForegroundWindowToDefaultMonitor(hwnd);
        return;
    case kTrayMenuStartMinimized:
        g_startMinimizedToTray = !g_startMinimizedToTray;
        SaveSettings();
        return;
    case kDisplayModeForegroundCmd:
        g_displayMode = DisplayMode::ForegroundExclusive;
        SaveSettings();
        ApplyTargetMonitorPlacement();
        return;
    case kDisplayModeBackgroundCmd:
        g_displayMode = DisplayMode::BackgroundUnderlay;
        SaveSettings();
        ApplyTargetMonitorPlacement();
        return;
    case kTrayMenuExit:
        RequestAppExit(hwnd);
        return;
    case kMenuExit:
        ShowWindow(hwnd, SW_HIDE);
        return;
    default:
        break;
    }

    if (commandId >= kOpacityMenuBase && commandId <= kOpacityMenuBase + 100) {
        const int pct = static_cast<int>(commandId - kOpacityMenuBase);
        if (pct % 10 == 0) {
            g_state.opacityPercent = pct;
            ApplyWindowOpacity(hwnd);
            SaveSettings();
        }
        return;
    }

    if (commandId >= kSourceMenuBase && commandId < kMonitorMenuBase) {
        const size_t index = static_cast<size_t>(commandId - kSourceMenuBase);
        if (index < g_state.sourceWindows.size()) {
            g_state.sourceWindow = g_state.sourceWindows[index].hwnd;
            g_state.panOffset = {0, 0};
            g_state.wgcCaptureActive = g_state.useWgcPath && g_wgcCapture.Start(g_state.sourceWindow);
            ApplyTargetMonitorPlacement();
            UpdateTrayIcon(hwnd);
            ShowMainWindow(hwnd);
        }
        return;
    }

    if (commandId >= kMonitorMenuBase) {
        const size_t index = static_cast<size_t>(commandId - kMonitorMenuBase);
        if (index < g_state.monitors.size()) {
            g_state.targetMonitor = g_state.monitors[index].handle;
            ApplyTargetMonitorPlacement();
            InvalidateRect(hwnd, nullptr, TRUE);
        }
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreatedMsg) {
        AddTrayIcon(hwnd);
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        g_state.window = hwnd;
        g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
        g_state.useWgcPath = g_wgcCapture.IsSupported();
        if (g_state.sourceWindow != nullptr && IsWindow(g_state.sourceWindow)) {
            g_state.wgcCaptureActive = g_state.useWgcPath && g_wgcCapture.Start(g_state.sourceWindow);
        }
        AddTrayIcon(hwnd);
        ApplyWindowOpacity(hwnd);
        ApplyTargetMonitorPlacement();
        SetTimer(hwnd, kFrameTimerId, kFrameIntervalMs, nullptr);
        SetTimer(hwnd, kNotchTimerId, kNotchTimerMs, nullptr);
        RegisterHotKey(hwnd, kHotkeyZoomInId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_OEM_PLUS);
        RegisterHotKey(hwnd, kHotkeyZoomOutId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_OEM_MINUS);
        RegisterHotKey(hwnd, kHotkeyResetZoomId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, '0');
        RegisterHotKey(hwnd, kHotkeyResetZoomAltId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R');
        RegisterHotKey(hwnd, kHotkeyMirrorForegroundId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'M');
        return 0;

    case kTrayCallbackMsg: {
        const UINT evt = LOWORD(static_cast<DWORD>(lParam));
        if (evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU) {
            ShowTrayContextMenu(hwnd);
            return 0;
        }
        if (evt == WM_LBUTTONUP || evt == WM_LBUTTONDBLCLK || evt == NIN_SELECT || evt == NIN_KEYSELECT) {
            ShowMainWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == kFrameTimerId) {
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == kNotchTimerId) {
            const int notchH = g_state.notchRect.bottom - g_state.notchRect.top;
            const int hiddenY = notchH - kNotchPeekPx; // fully hidden offset
            // Advance idle timer
            if (g_state.notchState == NotchState::Visible) {
                g_state.notchIdleMs += kNotchTimerMs;
                if (g_state.notchIdleMs >= kNotchHideDelayMs) {
                    g_state.notchState = NotchState::Hiding;
                }
            }
            // Animate slide
            const int step = std::max(1, notchH / 12); // ~12 frames to travel full height
            if (g_state.notchState == NotchState::Hiding) {
                g_state.notchOffsetY = std::min(hiddenY, g_state.notchOffsetY + step);
                if (g_state.notchOffsetY >= hiddenY) {
                    g_state.notchOffsetY = hiddenY;
                    g_state.notchState  = NotchState::Hidden;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (g_state.notchState == NotchState::Showing) {
                g_state.notchOffsetY = std::max(0, g_state.notchOffsetY - step);
                if (g_state.notchOffsetY <= 0) {
                    g_state.notchOffsetY = 0;
                    g_state.notchState  = NotchState::Visible;
                    g_state.notchIdleMs = 0;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_HOTKEY:
        if (wParam == kHotkeyZoomInId) {
            g_state.zoom = std::min(5.0f, g_state.zoom + 0.1f);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == kHotkeyZoomOutId) {
            g_state.zoom = std::max(0.2f, g_state.zoom - 0.1f);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == kHotkeyResetZoomId || wParam == kHotkeyResetZoomAltId) {
            g_state.zoom = 1.0f;
            g_state.panOffset = {0, 0};
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == kHotkeyMirrorForegroundId || wParam == kHotkeyMirrorForegroundAltId) {
            MirrorForegroundWindowToDefaultMonitor(hwnd);
            return 0;
        }
        return 0;

    case WM_DISPLAYCHANGE:
        RefreshMonitors();
        ApplyTargetMonitorPlacement();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_SIZE:
        UpdateNotchRect(hwnd);
        return 0;

    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT animNotch = g_state.notchRect;
        OffsetRect(&animNotch, 0, -g_state.notchOffsetY);
        if (!PtInRect(&animNotch, point)) {
            g_state.dragActive = true;
            g_state.dragStart = point;
            g_state.panAtDragStart = g_state.panOffset;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        const POINT cur{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (g_state.dragActive) {
            g_state.panOffset.x = g_state.panAtDragStart.x + (cur.x - g_state.dragStart.x);
            g_state.panOffset.y = g_state.panAtDragStart.y + (cur.y - g_state.dragStart.y);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        // Notch proximity: show if cursor within activation zone near top-centre
        {
            RECT client{};
            GetClientRect(hwnd, &client);
            const int cw = client.right - client.left;
            const int notchH = g_state.notchRect.bottom - g_state.notchRect.top;
            const int zoneHalfW = (g_state.notchRect.right - g_state.notchRect.left) / 2 + 24;
            const int zoneH     = notchH + 32; // activation band below visible pill
            const int zoneCx    = cw / 2;
            const bool inZone   = std::abs(cur.x - zoneCx) <= zoneHalfW && cur.y <= zoneH;
            if (inZone) {
                g_state.notchIdleMs = 0;
                if (g_state.notchState == NotchState::Hidden ||
                    g_state.notchState == NotchState::Hiding) {
                    g_state.notchState = NotchState::Showing;
                }
            } else {
                // Not in zone: if visible and idle clock hasn't started, let the timer handle it
                if (g_state.notchState == NotchState::Visible) {
                    // idle timer already ticking in WM_TIMER
                } else if (g_state.notchState == NotchState::Showing) {
                    g_state.notchState = NotchState::Hiding;
                }
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (g_state.dragActive) {
            g_state.dragActive = false;
            ReleaseCapture();
        } else {
            // Hit-test against the animated notch rect
            RECT animRect = g_state.notchRect;
            OffsetRect(&animRect, 0, -g_state.notchOffsetY);
            if (PtInRect(&animRect, point)) {
                g_state.notchIdleMs = 0; // keep visible after menu
                ShowNotchMenu(hwnd);
            }
        }
        return 0;
    }

    case WM_COMMAND:
        HandleMenuCommand(hwnd, LOWORD(wParam));
        return 0;

    case WM_ERASEBKGND:
        return 1; // prevent background erase — back-buffer handles it

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC hdc = BeginPaint(hwnd, &paint);

        RECT client{};
        GetClientRect(hwnd, &client);
        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;

        // Compose into an off-screen back-buffer, then blit once to avoid flicker.
        HDC memDc = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, cw, ch);
        HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

        // Fill background once in the back-buffer.
        HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDc, &client, bgBrush);
        DeleteObject(bgBrush);

        DrawMirrorContent(memDc, client);
        DrawNotch(memDc);

        BitBlt(hdc, 0, 0, cw, ch, memDc, 0, 0, SRCCOPY);

        SelectObject(memDc, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDc);

        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_CLOSE:
        if (!g_appExiting) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);

    case WM_DESTROY:
        RemoveTrayIcon(hwnd);
        UnregisterHotKey(hwnd, kHotkeyZoomInId);
        UnregisterHotKey(hwnd, kHotkeyZoomOutId);
        UnregisterHotKey(hwnd, kHotkeyResetZoomId);
        UnregisterHotKey(hwnd, kHotkeyResetZoomAltId);
        UnregisterHotKey(hwnd, kHotkeyMirrorForegroundId);
        UnregisterHotKey(hwnd, kHotkeyMirrorForegroundAltId);
        KillTimer(hwnd, kFrameTimerId);
        KillTimer(hwnd, kNotchTimerId);
        g_wgcCapture.Stop();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LoadSettings();

    // Tray-first startup: build lists up front; source/monitor can be chosen from tray menu.
    RefreshMonitors();
    RefreshSourceWindows();

    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        0, 0,
        LR_DEFAULTSIZE | LR_SHARED));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_SHARED));

    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kWindowClassName,
        kWindowTitle,
        WS_POPUP,
        0, 0, 1280, 720,
        nullptr, nullptr, instance, nullptr);

    if (window == nullptr) {
        return 1;
    }

    const bool hasSource = (g_state.sourceWindow != nullptr && IsWindow(g_state.sourceWindow));
    ShowWindow(window, (g_startMinimizedToTray || !hasSource) ? SW_HIDE : SW_SHOW);
    UpdateWindow(window);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

