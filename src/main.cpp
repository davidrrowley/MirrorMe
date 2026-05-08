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
constexpr wchar_t kAppVersion[]      = L"1.0.6";
constexpr wchar_t kWindowTitle[] = L"MirrorMe";

constexpr UINT_PTR kFrameTimerId = 1;
constexpr UINT kFrameIntervalMs = 16;
constexpr UINT_PTR kNotchTimerId = 2;
constexpr UINT_PTR kPopCloseTimerId = 10;  // grace-period before closing flyout on mouse-leave
constexpr UINT kPopCloseDelayMs = 150;
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
constexpr UINT kTrayMenuAbout    = 1106;
constexpr UINT kMenuAnnotate     = 1005;
constexpr UINT kMenuAnnoClear    = 1006;
constexpr UINT kAnnoPenMenuBase  = 60000;
constexpr UINT kAnnoSizeMenuBase = 61000;
constexpr int  kHotkeyAnnotateId = 7;

constexpr UINT kSourceMenuBase = 20000;
constexpr UINT kMonitorMenuBase = 30000;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMsg = WM_APP + 10;

constexpr int kNotchWidth  = 180;  // wider to accommodate dot-grid handles
constexpr int kNotchHeight = 28;

// Horizontal (DevBox-style) bar menu layout
constexpr int kPopBarH    = 72;   // total bar height at 96 DPI
constexpr int kPopItemW   = 68;   // width per button at 96 DPI
constexpr int kPopSepW    = 14;   // separator column width at 96 DPI
constexpr int kPopBarPad  = 8;    // left/right padding at 96 DPI
constexpr int kPopIconH   = 28;   // icon area height at 96 DPI
constexpr int kPopLabelH  = 16;   // label area height at 96 DPI
constexpr int kPopIconPtSz = 16;  // icon font size in pt

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

// ── Annotation state ─────────────────────────────────────────────────────────
struct Stroke {
    std::vector<POINT> points;
    COLORREF            color     = RGB(220, 50, 50);
    int                 thickness = 3;
    bool                isRect    = false;
};

constexpr size_t kAnnoteColorCount = 6;
constexpr size_t kAnnoteSizeCount  = 3;

static const COLORREF kAnnoteColors[kAnnoteColorCount] = {
    RGB(220,  50,  50),   // Red
    RGB(255, 140,   0),   // Orange
    RGB(255, 220,   0),   // Yellow
    RGB(  0, 200,  80),   // Green
    RGB( 60, 130, 255),   // Blue
    RGB(240, 240, 240),   // White
};
static const wchar_t* kAnnoteColorNames[kAnnoteColorCount] = {
    L"Red", L"Orange", L"Yellow", L"Green", L"Blue", L"White"
};
static const int      kAnnoteSizes[kAnnoteSizeCount]      = { 2, 4, 8 };
static const wchar_t* kAnnoteSizeNames[kAnnoteSizeCount]  = { L"Thin", L"Medium", L"Thick" };

static bool              g_annotateMode    = false;
static bool              g_annotateDrawing = false;
static bool              g_annotateRectDrawing = false;
static COLORREF          g_annotePenColor  = RGB(220, 50, 50);
static int               g_annotePenSize   = 3;
static std::vector<Stroke> g_strokes;

// ── Popup menu state ────────────────────────────────────────────────────────
// Label = plain action, NavItem = drill-in (shows ›), BackItem = ← Back
enum class PItemType { Label, NavItem, BackItem, Separator };
struct PItem {
    PItemType    type   = PItemType::Label;
    std::wstring text;
    std::wstring icon;          // Segoe MDL2 Assets glyph (may be empty)
    UINT         cmdId  = 0;
    bool         accent = false;
    int          x = 0, w = 0; // horizontal layout (main bar)
    int          y = 0, h = 0; // vertical layout (flyout child)
};
enum class PopView { Root, Sources, Monitors, Transparency, DisplayMode, Annotate };
static std::vector<PItem> g_popItems;
static PopView            g_popView       = PopView::Root;
static int                g_popHover      = -1;
static int                g_popListY      = 0;
static int                g_popListH      = 0;
static HWND               g_popHwnd       = nullptr;
static HWND               g_popList       = nullptr;
static WNDPROC            g_popListOrigProc = nullptr;
static HWND               g_popParent     = nullptr;
static HFONT              g_popFont       = nullptr;
static HFONT              g_popFontHdr    = nullptr;
static HFONT              g_popIconFont   = nullptr;
static HBRUSH             g_popListBrush  = nullptr;
static UINT               g_popDpi        = 96;

// Child flyout popup state
static HWND               g_popChildHwnd    = nullptr;
static std::vector<PItem> g_popChildItems;
static int                g_popChildHover   = -1;
static PopView            g_popChildView    = PopView::Root;
static int                g_popActiveNavIdx = -1;  // index in g_popItems whose flyout is open

// Scale a 96-DPI pixel constant to the current popup DPI.
static int PopScale(int px) { return MulDiv(px, (int)g_popDpi, 96); }

// Nav virtual command IDs (never forwarded to main window)
constexpr UINT kNavSources  = 2001;
constexpr UINT kNavMonitors = 2002;
constexpr UINT kNavBack     = 2003;
constexpr UINT kNavTransparency = 2004;
constexpr UINT kNavDisplayMode  = 2005;
constexpr UINT kNavAnnotate     = 2006;

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
    if (!IsWindowVisible(hwnd)) {
        return false;
    }
    if (IsWindowCloaked(hwnd) || IsShellOrDesktopWindowClass(hwnd)) {
        return false;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
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

    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    if (IsWindowCloaked(hwnd) || IsShellOrDesktopWindowClass(hwnd)) {
        return TRUE;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
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

    // Centre then apply pan, clamped to the overflow only — no pan when content fits.
    const int maxPanX = std::max(0, (dstWidth  - clientWidth)  / 2);
    const int maxPanY = std::max(0, (dstHeight - clientHeight) / 2);
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

void DrawAnnotations(HDC dc) {
    for (const auto& stroke : g_strokes) {
        if (stroke.points.empty()) continue;
        if (stroke.isRect) {
            if (stroke.points.size() < 2) continue;
            const int x1 = std::min(stroke.points[0].x, stroke.points[1].x);
            const int y1 = std::min(stroke.points[0].y, stroke.points[1].y);
            const int x2 = std::max(stroke.points[0].x, stroke.points[1].x);
            const int y2 = std::max(stroke.points[0].y, stroke.points[1].y);
            HPEN pen = CreatePen(PS_SOLID, stroke.thickness, stroke.color);
            HGDIOBJ oldPen   = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, x1, y1, x2, y2);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(pen);
        } else if (stroke.points.size() == 1) {
            // Single click — draw a filled dot
            const int r = stroke.thickness / 2 + 1;
            const POINT& p = stroke.points[0];
            HBRUSH br = CreateSolidBrush(stroke.color);
            HPEN   np = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ ob = SelectObject(dc, br);
            HGDIOBJ op = SelectObject(dc, np);
            Ellipse(dc, p.x - r, p.y - r, p.x + r, p.y + r);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(br); DeleteObject(np);
        } else {
            LOGBRUSH lb{ BS_SOLID, stroke.color, 0 };
            HPEN pen = ExtCreatePen(
                PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                static_cast<DWORD>(stroke.thickness), &lb, 0, nullptr);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, stroke.points[0].x, stroke.points[0].y, nullptr);
            for (size_t i = 1; i < stroke.points.size(); ++i)
                LineTo(dc, stroke.points[i].x, stroke.points[i].y);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
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

    // Only draw contents when notch is meaningfully visible
    if (g_state.notchOffsetY < (g_state.notchRect.bottom - g_state.notchRect.top) * 3 / 4) {
        // Clip to pill
        HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, radius, radius);
        SelectClipRgn(paintDc, rgn);
        DeleteObject(rgn);

        const int W  = r.right - r.left;
        const int H  = r.bottom - r.top;
        const int cx = r.left + W / 2;
        const int cy = r.top  + H / 2;

        // ── Dot grids: 3 cols × 2 rows on each side ──────────────────────
        // Dot diameter and spacing (centre-to-centre)
        const int dotD    = MulDiv(4, notchDpi, 96);
        const int dotGapX = MulDiv(6, notchDpi, 96);
        const int dotGapY = MulDiv(6, notchDpi, 96);
        // Physical grid size: (cols-1)*gapX + dotD wide, (rows-1)*gapY + dotD tall
        const int gridW   = 2 * dotGapX + dotD;   // 3 cols
        const int gridH   = 1 * dotGapY + dotD;   // 2 rows
        const int inset   = MulDiv(16, notchDpi, 96); // from pill edge to grid left
        const int gridTopY = cy - gridH / 2;

        HBRUSH dotBrush = CreateSolidBrush(RGB(130, 130, 145));
        HPEN   dotPen   = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ odp = SelectObject(paintDc, dotPen);
        HGDIOBJ odb = SelectObject(paintDc, dotBrush);

        // Left grid and right grid
        const int gridStartX[2] = { r.left + inset, r.right - inset - gridW };
        for (int g = 0; g < 2; ++g) {
            for (int col = 0; col < 3; ++col) {
                for (int row = 0; row < 2; ++row) {
                    const int dx = gridStartX[g] + col * dotGapX;
                    const int dy = gridTopY + row * dotGapY;
                    Ellipse(paintDc, dx, dy, dx + dotD, dy + dotD);
                }
            }
        }
        SelectObject(paintDc, odp);
        SelectObject(paintDc, odb);
        DeleteObject(dotBrush);
        DeleteObject(dotPen);

        // ── "MirrorMe" label + chevron stacked vertically in centre zone ─────
        const int chevW  = MulDiv(10, notchDpi, 96);
        const int chevH  = MulDiv(5,  notchDpi, 96);
        const int chevPW = MulDiv(2,  notchDpi, 96);
        const int fontPx = MulDiv(10, notchDpi, 72); // 10pt in pixels (~40% bigger than 7pt)
        const int lblGap = MulDiv(6,  notchDpi, 96); // gap between label and chevron
        // Total stack height, centred on cy
        const int stackH   = fontPx + lblGap + chevH;
        const int stackTop = cy - stackH / 2;

        // Label: horizontal zone between the inner edges of the dot grids
        {
            static HFONT s_notchFont = nullptr;
            static int   s_notchFontPx = 0;
            if (!s_notchFont || s_notchFontPx != fontPx) {
                if (s_notchFont) { DeleteObject(s_notchFont); s_notchFont = nullptr; }
                s_notchFontPx = fontPx;
                s_notchFont = CreateFont(
                    -fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            }
            HGDIOBJ oldFont = SelectObject(paintDc, s_notchFont);
            SetTextColor(paintDc, RGB(170, 170, 185));
            SetBkMode(paintDc, TRANSPARENT);
            const int midLeft  = r.left  + inset + gridW + MulDiv(4, notchDpi, 96);
            const int midRight = r.right - inset - gridW - MulDiv(4, notchDpi, 96);
            RECT textR = { midLeft, stackTop, midRight, stackTop + fontPx };
            DrawText(paintDc, L"MirrorMe", -1, &textR,
                     DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            SelectObject(paintDc, oldFont);
        }

        // Chevron: positioned just below the label
        {
            const int chevTop = stackTop + fontPx + lblGap;
            HPEN chevPen = CreatePen(PS_SOLID, chevPW, RGB(200, 200, 215));
            HGDIOBJ ocp  = SelectObject(paintDc, chevPen);
            MoveToEx(paintDc, cx - chevW / 2, chevTop,          nullptr);
            LineTo(paintDc,   cx,             chevTop + chevH);
            LineTo(paintDc,   cx + chevW / 2, chevTop);
            SelectObject(paintDc, ocp);
            DeleteObject(chevPen);
        }

        SelectClipRgn(paintDc, nullptr);
    }
}

// ── Custom dark popup menu helpers ──────────────────────────────────────────

// Forward-declare so PopupNavigateTo can be called from the wndproc
static void PopupCloseFlyout();
static void PopupOpenFlyout(HWND parentHwnd, int navItemIdx, PopView view);

static int PopupWidthForView(PopView /*view*/) {
    return PopScale(kPopW);
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

static void PopupLayout(std::vector<PItem>& items) {
    int y = PopScale(8);
    for (auto& it : items) {
        it.y = y;
        it.h = (it.type == PItemType::Separator) ? PopScale(kPopSepH) : PopScale(kPopItemH);
        y += it.h;
    }
}

static int PopupHitTest(const std::vector<PItem>& items, int my) {
    for (int i = 0; i < (int)items.size(); ++i) {
        const auto& it = items[i];
        if (it.type != PItemType::Separator)
            if (my >= it.y && my < it.y + it.h) return i;
    }
    return -1;
}

// ── Horizontal bar helpers (main DevBox-style popup) ──────────────────────

static void PopupLayoutH(std::vector<PItem>& items) {
    int x = PopScale(kPopBarPad);
    for (auto& it : items) {
        it.x = x;
        it.w = (it.type == PItemType::Separator) ? PopScale(kPopSepW) : PopScale(kPopItemW);
        x += it.w;
    }
}

static int PopupTotalWidth(const std::vector<PItem>& items) {
    if (items.empty()) return PopScale(kPopBarPad) * 2;
    const auto& last = items.back();
    return last.x + last.w + PopScale(kPopBarPad);
}

static int PopupHitTestH(const std::vector<PItem>& items, int mx) {
    for (int i = 0; i < (int)items.size(); ++i) {
        const auto& it = items[i];
        if (it.type != PItemType::Separator)
            if (mx >= it.x && mx < it.x + it.w) return i;
    }
    return -1;
}

// Paint items in the horizontal DevBox-style bar.
// iconFont must already be created by the caller.
static void PopupPaintItemsH(HDC dc, const std::vector<PItem>& items, int hover,
                              int activeNavIdx, HFONT labelFont, HFONT iconFont) {
    const int barH   = PopScale(kPopBarH);
    // Vertical zones within the bar
    const int iconY  = PopScale(10);
    const int iconH  = PopScale(kPopIconH);
    const int lblY   = iconY + iconH + PopScale(2);
    const int lblH   = PopScale(kPopLabelH);

    // 1. Hover backgrounds
    for (int i = 0; i < (int)items.size(); ++i) {
        const auto& it = items[i];
        if (it.type == PItemType::Separator) continue;
        if (i != hover && i != activeNavIdx) continue;
        HBRUSH hb = CreateSolidBrush(kPC_Hover);
        HPEN   hp = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ op = SelectObject(dc, hp), ob = SelectObject(dc, hb);
        const int rr = PopScale(6);
        RoundRect(dc, it.x + PopScale(2), PopScale(4),
                      it.x + it.w - PopScale(2), barH - PopScale(4), rr, rr);
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(hb); DeleteObject(hp);
    }

    // 2. Vertical separators
    for (const auto& it : items) {
        if (it.type != PItemType::Separator) continue;
        HPEN sp = CreatePen(PS_SOLID, 1, kPC_Sep);
        HGDIOBJ op = SelectObject(dc, sp);
        const int mx = it.x + it.w / 2;
        MoveToEx(dc, mx, PopScale(12), nullptr);
        LineTo(dc, mx, barH - PopScale(12));
        SelectObject(dc, op);
        DeleteObject(sp);
    }

    // 3. Icons and labels
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < (int)items.size(); ++i) {
        const auto& it = items[i];
        if (it.type == PItemType::Separator) continue;
        const bool hov = (i == hover) || (i == activeNavIdx);
        const COLORREF col = it.accent ? kPC_Accent
                           : hov      ? kPC_Text
                                      : RGB(175, 175, 185);
        SetTextColor(dc, col);

        if (!it.icon.empty() && iconFont) {
            HGDIOBJ of = SelectObject(dc, iconFont);
            RECT ir{ it.x, iconY, it.x + it.w, iconY + iconH };
            DrawTextW(dc, it.icon.c_str(), -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, of);
        }

        if (labelFont) {
            HGDIOBJ of = SelectObject(dc, labelFont);
            RECT lr{ it.x + PopScale(2), lblY, it.x + it.w - PopScale(2), lblY + lblH };
            DrawTextW(dc, it.text.c_str(), -1, &lr,
                      DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);
        }
    }
}

static void PopupPaintItems(HDC dc, const std::vector<PItem>& items, int hover,
                            PopView view, int activeNavIdx = -1) {
    const int popW = PopupWidthForView(view);
    const int padL  = PopScale(kPopPadL);
    const int arrowW = PopScale(28);
    const int arrowR = PopScale(8);
    for (int i = 0; i < (int)items.size(); ++i) {
        const auto& it = items[i];

        if (it.type == PItemType::Separator) {
            HPEN sp = CreatePen(PS_SOLID, 1, kPC_Sep);
            HGDIOBJ op = SelectObject(dc, sp);
            const int my = it.y + it.h / 2;
            MoveToEx(dc, PopScale(10), my, nullptr);
            LineTo(dc, popW - PopScale(10), my);
            SelectObject(dc, op);
            DeleteObject(sp);
            continue;
        }

        const bool hov = (i == hover) || (i == activeNavIdx);
        if (hov) {
            HBRUSH hb = CreateSolidBrush(kPC_Hover);
            HPEN   hp = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ op = SelectObject(dc, hp), ob = SelectObject(dc, hb);
            RoundRect(dc, PopScale(4), it.y + 1, popW - PopScale(4), it.y + it.h - 1, PopScale(6), PopScale(6));
            SelectObject(dc, op); SelectObject(dc, ob);
            DeleteObject(hb); DeleteObject(hp);
        }

        SetBkMode(dc, TRANSPARENT);

        if (it.type == PItemType::BackItem) {
            SetTextColor(dc, hov ? kPC_Text : kPC_Dim);
            HGDIOBJ of = SelectObject(dc, g_popFontHdr);
            RECT tr{ padL, it.y, popW, it.y + it.h };
            DrawTextW(dc, L"\u2190  Back", -1, &tr, DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, of);
            continue;
        }

        // Label or NavItem
        SetTextColor(dc, it.accent ? kPC_Accent : kPC_Text);
        HGDIOBJ of = SelectObject(dc, g_popFont);
        // For nav items leave room for the arrow glyph; for labels use full width.
        const int textRight = (it.type == PItemType::NavItem) ? (popW - arrowW - arrowR) : (popW - padL);
        RECT tr{ padL, it.y, textRight, it.y + it.h };
        DrawTextW(dc, it.text.c_str(), -1, &tr, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (it.type == PItemType::NavItem) {
            RECT cr{ popW - arrowW, it.y, popW - arrowR, it.y + it.h };
            SetTextColor(dc, kPC_Dim);
            DrawTextW(dc, L"\u203a", -1, &cr, DT_VCENTER | DT_SINGLELINE | DT_RIGHT);
        }
        SelectObject(dc, of);
    }
}

static void PopupDestroyList() {
    if (g_popList) { DestroyWindow(g_popList); g_popList = nullptr; }
    g_popListOrigProc = nullptr;
    if (g_popListBrush) { DeleteObject(g_popListBrush); g_popListBrush = nullptr; }
}

// Subclass proc for the source-window listbox — tracks hover so each item highlights.
static int g_popListHoverIdx = -1;

static LRESULT CALLBACK PopListSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        // Suppress — WM_DRAWITEM fills every item rect, so erase just causes a flash.
        return 1;
    case WM_MOUSEMOVE: {
        const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
        const int idx = (HIWORD(hit) == 0) ? static_cast<int>(LOWORD(hit)) : -1;
        if (idx != g_popListHoverIdx) {
            g_popListHoverIdx = idx;
            SendMessageW(hwnd, LB_SETCURSEL, (idx >= 0) ? idx : (WPARAM)-1, 0);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        // Cancel parent bar's close-flyout timer while mouse is in the list
        if (g_popHwnd) KillTimer(g_popHwnd, kPopCloseTimerId);
        break;
    }
    case WM_MOUSELEAVE:
        g_popListHoverIdx = -1;
        SendMessageW(hwnd, LB_SETCURSEL, (WPARAM)-1, 0);
        break;
    case WM_LBUTTONUP: {
        // Hover pre-selects the item via LB_SETCURSEL, so LBN_SELCHANGE won't fire on
        // click (selection didn't change). Handle click explicitly here instead.
        const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
        if (HIWORD(hit) == 0 && !g_state.sourceWindows.empty()) {
            PostMessageW(g_popParent, WM_COMMAND, kSourceMenuBase + LOWORD(hit), 0);
            DestroyWindow(g_popHwnd);
        }
        return 0;
    }
    }
    return CallWindowProcW(g_popListOrigProc, hwnd, msg, wp, lp);
}

static void PopupCreateList(HWND hwnd) {
    PopupDestroyList();
    RefreshSourceWindows();
    const int popW = PopScale(kPopW);
    const int srcCount = (int)g_state.sourceWindows.size();
    const int visRows  = std::min(std::max(srcCount, 1), kPopMaxRows);
    g_popListH = visRows * PopScale(kPopListItemH);

    const int lastItemBottom = g_popChildItems.empty() ? PopScale(8)
        : g_popChildItems.back().y + g_popChildItems.back().h;
    g_popListY = lastItemBottom + 4;

    g_popList = CreateWindowExW(0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        0, g_popListY, popW, g_popListH,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_popList, LB_SETITEMHEIGHT, 0, PopScale(kPopListItemH));
    if (srcCount == 0) {
        SendMessageW(g_popList, LB_ADDSTRING, 0, (LPARAM)L"No visible windows");
    } else {
        for (const auto& w : g_state.sourceWindows)
            SendMessageW(g_popList, LB_ADDSTRING, 0, (LPARAM)w.title.c_str());
    }
    // Subclass to enable hover highlighting
    g_popListOrigProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_popList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PopListSubclassProc)));
}

// ── Flyout close / open ──────────────────────────────────────────────────────

static void PopupCloseFlyout() {
    if (g_popChildHwnd && IsWindow(g_popChildHwnd))
        DestroyWindow(g_popChildHwnd);
    // g_popChildHwnd = nullptr and g_popActiveNavIdx = -1 are set in child WM_DESTROY
    g_popActiveNavIdx = -1;  // defensive reset in case child was already gone
    if (g_popHwnd && IsWindow(g_popHwnd))
        InvalidateRect(g_popHwnd, nullptr, FALSE);
}

static void PopupOpenFlyout(HWND parentHwnd, int navItemIdx, PopView view) {
    // Close any existing flyout first
    if (g_popChildHwnd && IsWindow(g_popChildHwnd)) {
        DestroyWindow(g_popChildHwnd);
        // child WM_DESTROY sets g_popChildHwnd = nullptr, g_popActiveNavIdx = -1
    }
    g_popActiveNavIdx = navItemIdx;
    g_popChildView    = view;
    g_popChildHover   = -1;
    g_popChildItems.clear();

    // Build items for this view (no Back button — flyout is dismissed by hovering away)
    {
        auto addLabel = [&](const wchar_t* text, UINT id, bool accent = false) {
            PItem it;
            it.type   = PItemType::Label;
            it.text   = text;
            it.cmdId  = id;
            it.accent = accent;
            g_popChildItems.push_back(std::move(it));
        };
        switch (view) {
        case PopView::Monitors:
            RefreshMonitors();
            for (size_t i = 0; i < g_state.monitors.size(); ++i) {
                std::wstring label = g_state.monitors[i].info.szDevice;
                const bool cur = g_state.monitors[i].handle == g_state.targetMonitor;
                if (cur) label += L" (current)";
                addLabel(label.c_str(), kMonitorMenuBase + static_cast<UINT>(i), cur);
            }
            break;
        case PopView::Transparency:
            for (int pct = 0; pct <= 100; pct += 10) {
                const std::wstring lbl = std::to_wstring(pct) + L"%";
                addLabel(lbl.c_str(), kOpacityMenuBase + static_cast<UINT>(pct),
                    pct == g_state.opacityPercent);
            }
            break;
        case PopView::DisplayMode:
            addLabel(L"Foreground Exclusive", kDisplayModeForegroundCmd,
                g_displayMode == DisplayMode::ForegroundExclusive);
            addLabel(L"Background Underlay", kDisplayModeBackgroundCmd,
                g_displayMode == DisplayMode::BackgroundUnderlay);
            break;
        case PopView::Annotate: {
            PItem toggleIt;
            toggleIt.type   = PItemType::Label;
            toggleIt.text   = g_annotateMode ? L"Stop Drawing" : L"Start Drawing";
            toggleIt.cmdId  = kMenuAnnotate;
            toggleIt.accent = g_annotateMode;
            g_popChildItems.push_back(std::move(toggleIt));
            { PItem s; s.type = PItemType::Separator; g_popChildItems.push_back(s); }
            for (size_t i = 0; i < kAnnoteColorCount; ++i)
                addLabel(kAnnoteColorNames[i], kAnnoPenMenuBase + (UINT)i,
                    g_annotePenColor == kAnnoteColors[i]);
            { PItem s; s.type = PItemType::Separator; g_popChildItems.push_back(s); }
            for (size_t i = 0; i < kAnnoteSizeCount; ++i)
                addLabel(kAnnoteSizeNames[i], kAnnoSizeMenuBase + (UINT)i,
                    g_annotePenSize == kAnnoteSizes[i]);
            { PItem s; s.type = PItemType::Separator; g_popChildItems.push_back(s); }
            addLabel(L"Clear All", kMenuAnnoClear, false);
            break;
        }
        default:
            break; // Sources: no label items, just the listbox below
        }
    }
    PopupLayout(g_popChildItems);

    const int childW = PopScale(kPopW);
    const int itemsH = g_popChildItems.empty() ? PopScale(8)
        : g_popChildItems.back().y + g_popChildItems.back().h + PopScale(8);
    const int srcRows = std::min(kPopMaxRows, std::max(1, (int)g_state.sourceWindows.size()));
    const int estimatedH = (view == PopView::Sources)
        ? itemsH + srcRows * PopScale(kPopListItemH) + PopScale(8)
        : itemsH;

    // Position below the specific nav item column (centred on that column)
    const int colCx = g_popItems[navItemIdx].x + g_popItems[navItemIdx].w / 2;
    const int barBottom = PopScale(kPopBarH);
    POINT navPt = { colCx - childW / 2, barBottom };
    ClientToScreen(g_popHwnd, &navPt);
    const POINT desired = { navPt.x, navPt.y + PopScale(4) };
    const POINT pos = PopupClampToMonitor(desired, childW, estimatedH);

    g_popChildHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"MirrorMePopup", nullptr,
        WS_POPUP,
        pos.x, pos.y, childW, estimatedH,
        parentHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_popChildHwnd) {
        g_popActiveNavIdx = -1;
        InvalidateRect(parentHwnd, nullptr, FALSE);
        return;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_popChildHwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_popChildHwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    if (view == PopView::Sources) {
        PopupCreateList(g_popChildHwnd);
        const int actualH = g_popListY + g_popListH + PopScale(8);
        SetWindowPos(g_popChildHwnd, nullptr, pos.x, pos.y, childW, actualH,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ShowWindow(g_popChildHwnd, SW_SHOWNOACTIVATE);
    InvalidateRect(parentHwnd, nullptr, FALSE); // show active-nav highlight
}

LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool isChild = (hwnd == g_popChildHwnd);
    std::vector<PItem>& items = isChild ? g_popChildItems : g_popItems;
    int& hover  = isChild ? g_popChildHover : g_popHover;
    PopView& view = isChild ? g_popChildView  : g_popView;

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
        if (!isChild) {
            PopupPaintItemsH(mem, items, hover, g_popActiveNavIdx, g_popFont, g_popIconFont);
        } else {
            PopupPaintItems(mem, items, hover, view, -1);
        }
        BitBlt(hdc, 0, 0, cl.right, cl.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int newHov = isChild
            ? PopupHitTest(items, GET_Y_LPARAM(lp))
            : PopupHitTestH(items, GET_X_LPARAM(lp));
        // Mouse is back in the bar — cancel any pending flyout-close timer.
        if (!isChild && g_popHwnd)
            KillTimer(g_popHwnd, kPopCloseTimerId);
        if (isChild && g_popHwnd)
            KillTimer(g_popHwnd, kPopCloseTimerId);
        if (newHov != hover) {
            hover = newHov;
            if (!isChild) {
                // Hovering a nav item opens its flyout; hovering elsewhere closes it.
                if (newHov >= 0 && items[newHov].type == PItemType::NavItem) {
                    if (newHov != g_popActiveNavIdx) {
                        const UINT cmdId = items[newHov].cmdId;
                        PopView flyView = PopView::Sources;
                        if (cmdId == kNavMonitors)          flyView = PopView::Monitors;
                        else if (cmdId == kNavDisplayMode)  flyView = PopView::DisplayMode;
                        else if (cmdId == kNavTransparency) flyView = PopView::Transparency;
                        else if (cmdId == kNavAnnotate)     flyView = PopView::Annotate;
                        PopupOpenFlyout(hwnd, newHov, flyView);
                    }
                } else if (newHov >= 0) {
                    PopupCloseFlyout();
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        hover = -1;
        if (!isChild) {
            // Defer close by a short grace period so slow mouse movement into the flyout
            // doesn't dismiss it before the cursor arrives.
            SetTimer(hwnd, kPopCloseTimerId, kPopCloseDelayMs, nullptr);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const int idx = isChild
            ? PopupHitTest(items, GET_Y_LPARAM(lp))
            : PopupHitTestH(items, GET_X_LPARAM(lp));
        if (idx < 0) return 0;
        const UINT cmdId = items[idx].cmdId;
        if (isChild) {
            // Click inside the flyout: execute command and close everything.
            PostMessageW(g_popParent, WM_COMMAND, cmdId, 0);
            DestroyWindow(g_popHwnd);
            return 0;
        }
        // Click in main (horizontal) bar.
        if (items[idx].type == PItemType::NavItem) {
            if (idx == g_popActiveNavIdx) {
                // Second click on the same nav item toggles the flyout off.
                PopupCloseFlyout();
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                const UINT ncId = items[idx].cmdId;
                PopView flyView = PopView::Sources;
                if (ncId == kNavMonitors)          flyView = PopView::Monitors;
                else if (ncId == kNavDisplayMode)  flyView = PopView::DisplayMode;
                else if (ncId == kNavTransparency) flyView = PopView::Transparency;
                else if (ncId == kNavAnnotate)     flyView = PopView::Annotate;
                PopupOpenFlyout(hwnd, idx, flyView);
            }
            return 0;
        }
        PopupCloseFlyout();
        PostMessageW(g_popParent, WM_COMMAND, cmdId, 0);
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == kPopCloseTimerId) {
            KillTimer(hwnd, kPopCloseTimerId);
            // Only close if cursor is not in the flyout child.
            bool inChild = false;
            if (g_popChildHwnd && IsWindow(g_popChildHwnd)) {
                POINT cur; GetCursorPos(&cur);
                RECT cr; GetWindowRect(g_popChildHwnd, &cr);
                inChild = (PtInRect(&cr, cur) != 0);
            }
            if (!inChild) PopupCloseFlyout();
            return 0;
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_popChildHwnd && IsWindow(g_popChildHwnd)) {
                PopupCloseFlyout();
                if (g_popHwnd && IsWindow(g_popHwnd)) InvalidateRect(g_popHwnd, nullptr, FALSE);
            } else {
                DestroyWindow(g_popHwnd);
            }
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            // Child has WS_EX_NOACTIVATE so clicking it never deactivates the parent;
            // any other deactivation means the user clicked away — close everything.
            HWND toActivate = (HWND)lp;
            if (toActivate != g_popChildHwnd)
                DestroyWindow(g_popHwnd);
        }
        return 0;
    case WM_DESTROY:
        if (isChild) {
            PopupDestroyList();
            g_popChildItems.clear();
            g_popChildHwnd    = nullptr;
            g_popActiveNavIdx = -1;
        } else {
            KillTimer(hwnd, kPopCloseTimerId);
            if (g_popChildHwnd && IsWindow(g_popChildHwnd))
                DestroyWindow(g_popChildHwnd);
            g_popHwnd = nullptr;
            if (g_popFont)     { DeleteObject(g_popFont);     g_popFont     = nullptr; }
            if (g_popFontHdr)  { DeleteObject(g_popFontHdr);  g_popFontHdr  = nullptr; }
            if (g_popIconFont) { DeleteObject(g_popIconFont); g_popIconFont = nullptr; }
        }
        return 0;
    case WM_CTLCOLORLISTBOX: {
        if (!g_popListBrush) g_popListBrush = CreateSolidBrush(kPC_Bg);
        SetBkColor((HDC)wp, kPC_Bg);
        SetTextColor((HDC)wp, kPC_Text);
        return (LRESULT)g_popListBrush;
    }
    case WM_MEASUREITEM: {
        auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        mi->itemHeight = PopScale(kPopListItemH);
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
                DestroyWindow(g_popHwnd);
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

    g_popParent       = parentHwnd;
    g_popDpi          = GetDpiForWindow(parentHwnd);
    g_popView         = PopView::Root;
    g_popActiveNavIdx = -1;
    g_popChildHwnd    = nullptr;

    // Build DPI-scaled fonts
    const int dpi = GetDpiForWindow(parentHwnd);
    {
        LOGFONTW lf{};
        // Label font (small, Segoe UI)
        lf.lfHeight  = -MulDiv(9, dpi, 72);
        lf.lfWeight  = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        g_popFont    = CreateFontIndirectW(&lf);
        // Header font used by flyout Back items
        lf.lfHeight  = -MulDiv(8, dpi, 72);
        lf.lfWeight  = FW_SEMIBOLD;
        g_popFontHdr = CreateFontIndirectW(&lf);
        // Icon font (Segoe MDL2 Assets glyphs)
        lf.lfHeight  = -MulDiv(kPopIconPtSz, dpi, 72);
        lf.lfWeight  = FW_NORMAL;
        wcscpy_s(lf.lfFaceName, L"Segoe MDL2 Assets");
        g_popIconFont = CreateFontIndirectW(&lf);
    }

    // Build horizontal bar items (icon glyph + short label + command id)
    // Icons are Segoe MDL2 Assets code points — adjust as desired.
    g_popItems.clear();
    g_popHover = -1;
    g_popListH = 0;
    {
        auto add = [](PItemType t, const wchar_t* text, UINT id, const wchar_t* icon) {
            PItem it;
            it.type  = t;
            it.text  = text;
            it.icon  = icon;
            it.cmdId = id;
            g_popItems.push_back(std::move(it));
        };
        auto sep = []() {
            PItem it;
            it.type = PItemType::Separator;
            g_popItems.push_back(std::move(it));
        };
        // Group 1 — zoom
        add(PItemType::Label,   L"Zoom In",  kMenuZoomIn,              L"\uE8A3"); // ZoomIn
        add(PItemType::Label,   L"Zoom Out", kMenuZoomOut,             L"\uE71F"); // ZoomOut
        add(PItemType::Label,   L"Reset",    kMenuResetZoom,           L"\uE72C"); // Refresh
        sep();
        // Group 2 — source, monitor, display, opacity
        add(PItemType::NavItem, L"Source",   kNavSources,              L"\uE737"); // AllApps
        add(PItemType::NavItem, L"Monitor",  kNavMonitors,             L"\uE7F4"); // TVMonitor
        add(PItemType::NavItem, L"Display",  kNavDisplayMode,          L"\uE7C4"); // DisplayExternalMirrored
        add(PItemType::NavItem, L"Opacity",  kNavTransparency,         L"\uEB9D"); // Brightness
        sep();
        // Group 3 — annotate
        {
            PItem drawIt;
            drawIt.type   = PItemType::NavItem;
            drawIt.text   = L"Draw";
            drawIt.icon   = L"\uE932";   // Draw icon (Segoe MDL2 Assets)
            drawIt.cmdId  = kNavAnnotate;
            drawIt.accent = g_annotateMode;
            g_popItems.push_back(std::move(drawIt));
        }
        sep();
        // Group 4 — exit
        add(PItemType::Label,   L"Exit",     kMenuExit,                L"\uE8BB"); // Leave
    }
    PopupLayoutH(g_popItems);
    const int initW = PopupTotalWidth(g_popItems);
    const int initH = PopScale(kPopBarH);

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

// CBT hook that centres the next MessageBox on the primary monitor
static HHOOK g_aboutHook = nullptr;
static LRESULT CALLBACK AboutCbtProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_ACTIVATE) {
        HWND dlg = reinterpret_cast<HWND>(wParam);
        wchar_t cls[32]{};
        GetClassNameW(dlg, cls, 32);
        if (wcscmp(cls, L"#32770") == 0) { // MessageBox class
            RECT wa{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0); // primary monitor work area
            RECT wr{};
            GetWindowRect(dlg, &wr);
            const int dlgW = wr.right  - wr.left;
            const int dlgH = wr.bottom - wr.top;
            const int x = wa.left + (wa.right  - wa.left - dlgW) / 2;
            const int y = wa.top  + (wa.bottom - wa.top  - dlgH) / 2;
            SetWindowPos(dlg, nullptr, x, y, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            // Unhook immediately — one-shot
            if (g_aboutHook) { UnhookWindowsHookEx(g_aboutHook); g_aboutHook = nullptr; }
        }
    }
    return CallNextHookEx(g_aboutHook, code, wParam, lParam);
}

static void ShowAboutDialog(HWND hwnd) {
    const std::wstring msg =
        L"MirrorMe  v" + std::wstring(kAppVersion) + L"\r\n"
        L"\r\n"
        L"Mirrors one app window to another monitor.\r\n"
        L"Built for ultrawide + Elgato Prompter setups (and anything similar).\r\n"
        L"Hover over the notch to open the control bar.\r\n"
        L"\r\n"
        L"Keyboard shortcuts\r\n"
        L"  Ctrl+Alt +/\u2212          Zoom in / out\r\n"
        L"  Ctrl+Alt 0 or R      Reset zoom\r\n"
        L"  Ctrl+Alt+Shift+M     Mirror foreground window\r\n"
        L"\r\n"
        L"github.com/davidrrowley/MirrorMe\r\n"
        L"GNU General Public License v3 \u2022 \u00a9 2025\u20132026 David Rowley";

    // Install one-shot CBT hook to move the dialog to the primary monitor
    g_aboutHook = SetWindowsHookExW(WH_CBT, AboutCbtProc,
        nullptr, GetCurrentThreadId());
    MessageBoxW(hwnd, msg.c_str(), L"About MirrorMe", MB_OK | MB_ICONINFORMATION);
    // Safety: unhook if MessageBox returned without HCBT_ACTIVATE firing
    if (g_aboutHook) { UnhookWindowsHookEx(g_aboutHook); g_aboutHook = nullptr; }
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
    AppendMenuW(root, MF_STRING, kTrayMenuAbout, L"About MirrorMe\u2026");
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
        g_state.zoom = 1.0f;
        g_state.panOffset = {0, 0};
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
        g_state.zoom = std::max(1.0f, g_state.zoom - 0.1f);
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
    case kTrayMenuAbout:
        ShowAboutDialog(hwnd);
        return;
    case kTrayMenuExit:
        RequestAppExit(hwnd);
        return;
    case kMenuExit:
        g_wgcCapture.Stop();
        g_state.wgcCaptureActive = false;
        g_state.sourceWindow = nullptr;
        g_state.zoom = 1.0f;
        g_state.panOffset = {0, 0};
        ShowWindow(hwnd, SW_HIDE);
        UpdateTrayIcon(hwnd);
        return;
    case kMenuAnnotate:
        g_annotateMode = !g_annotateMode;
        if (!g_annotateMode) {
            if (g_annotateDrawing) {
                g_annotateDrawing = false;
                ReleaseCapture();
            }
            if (g_annotateRectDrawing) {
                g_annotateRectDrawing = false;
                ReleaseCapture();
            }
            g_strokes.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return;
    case kMenuAnnoClear:
        g_strokes.clear();
        g_annotateDrawing = false;
        InvalidateRect(hwnd, nullptr, FALSE);
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

    if (commandId >= kMonitorMenuBase && commandId < kAnnoPenMenuBase) {
        const size_t index = static_cast<size_t>(commandId - kMonitorMenuBase);
        if (index < g_state.monitors.size()) {
            g_state.targetMonitor = g_state.monitors[index].handle;
            ApplyTargetMonitorPlacement();
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return;
    }

    if (commandId >= kAnnoPenMenuBase && commandId < kAnnoPenMenuBase + (UINT)kAnnoteColorCount) {
        g_annotePenColor = kAnnoteColors[commandId - kAnnoPenMenuBase];
        g_annotateMode = true;
        return;
    }

    if (commandId >= kAnnoSizeMenuBase && commandId < kAnnoSizeMenuBase + (UINT)kAnnoteSizeCount) {
        g_annotePenSize = kAnnoteSizes[commandId - kAnnoSizeMenuBase];
        g_annotateMode = true;
        return;
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
        RegisterHotKey(hwnd, kHotkeyZoomInId,  MOD_CONTROL | MOD_ALT, VK_OEM_PLUS);
        RegisterHotKey(hwnd, kHotkeyZoomOutId,  MOD_CONTROL | MOD_ALT, VK_OEM_MINUS);
        RegisterHotKey(hwnd, kHotkeyResetZoomId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, '0');
        RegisterHotKey(hwnd, kHotkeyResetZoomAltId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R');
        RegisterHotKey(hwnd, kHotkeyMirrorForegroundId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'M');
        RegisterHotKey(hwnd, kHotkeyAnnotateId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D');
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
            const int step = std::max(2, notchH / 6); // ~6 frames to travel full height
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
            g_state.zoom = std::max(1.0f, g_state.zoom - 0.1f);
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
        if (wParam == kHotkeyAnnotateId) {
            HandleMenuCommand(hwnd, kMenuAnnotate);
            return 0;
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0)
                g_state.zoom = std::min(5.0f, g_state.zoom + 0.1f);
            else
                g_state.zoom = std::max(1.0f, g_state.zoom - 0.1f);
            InvalidateRect(hwnd, nullptr, FALSE);
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
            if (g_annotateMode) {
                Stroke stroke;
                stroke.color     = g_annotePenColor;
                stroke.thickness = g_annotePenSize;
                stroke.points.push_back(point);
                g_strokes.push_back(std::move(stroke));
                g_annotateDrawing = true;
            } else {
                g_state.dragActive = true;
                g_state.dragStart = point;
                g_state.panAtDragStart = g_state.panOffset;
            }
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        const POINT cur{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (g_annotateDrawing && !g_strokes.empty()) {
            g_strokes.back().points.push_back(cur);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g_annotateRectDrawing && !g_strokes.empty()) {
            g_strokes.back().points[1] = cur;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g_state.dragActive) {
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
        if (g_annotateDrawing) {
            g_annotateDrawing = false;
            ReleaseCapture();
        } else if (g_state.dragActive) {
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

    case WM_RBUTTONDOWN: {
        if (g_annotateMode && !g_annotateDrawing) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT animNotch = g_state.notchRect;
            OffsetRect(&animNotch, 0, -g_state.notchOffsetY);
            if (!PtInRect(&animNotch, point)) {
                Stroke stroke;
                stroke.color     = g_annotePenColor;
                stroke.thickness = g_annotePenSize;
                stroke.isRect    = true;
                stroke.points.push_back(point);
                stroke.points.push_back(point);
                g_strokes.push_back(std::move(stroke));
                g_annotateRectDrawing = true;
                SetCapture(hwnd);
            }
        }
        return 0;
    }

    case WM_RBUTTONUP:
        if (g_annotateRectDrawing) {
            g_annotateRectDrawing = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        if (g_annotateMode && LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && g_annotateMode) {
            HandleMenuCommand(hwnd, kMenuAnnotate);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);

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
        DrawAnnotations(memDc);
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
        UnregisterHotKey(hwnd, kHotkeyAnnotateId);
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

