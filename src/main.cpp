// LunarAutoClicker - autoclicker externe pour Lunar Client, interface Direct2D.
//
// Les clics ne sont simules que si Lunar Client est au premier plan, que le
// bouton gauche est maintenu physiquement et que l'autoclick est active.
//
// Usage : LunarAutoClicker.exe [--process nom.exe]...
// Raccourcis globaux : F8 = on/off, F10 = quitter.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "clicker.h"
#include "cleanup.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------
// Layout (en DIPs)
// ---------------------------------------------------------------------------
constexpr float kW = 440.f;
constexpr float kH = 650.f;
constexpr float kHeaderH = 56.f;
constexpr D2D1_POINT_2F kDialCenter = {kW / 2.f, 282.f};
constexpr float kDialR = 126.f;
constexpr D2D1_RECT_F kCloseBtn = {kW - 46.f, 14.f, kW - 16.f, 42.f};
constexpr D2D1_RECT_F kMinBtn = {kW - 80.f, 14.f, kW - 50.f, 42.f};
constexpr D2D1_RECT_F kShieldBtn = {kW - 114.f, 14.f, kW - 84.f, 42.f};
constexpr D2D1_RECT_F kGearBtn = {kW - 148.f, 14.f, kW - 118.f, 42.f};
// Selecteur de bouton (clic gauche / droit) dans l'en-tete.
constexpr D2D1_RECT_F kBtnSeg = {kW - 244.f, 15.f, kW - 156.f, 41.f};
// Pastille du raccourci (dans la carte AUTOCLICK), cliquable pour redefinir.
constexpr D2D1_RECT_F kHotkeyChip = {42.f, 603.f, 110.f, 623.f};

// Panneau "Confidentialite" (superpose, occupe toute la fenetre).
constexpr D2D1_RECT_F kPrivPanel = {20.f, 64.f, kW - 20.f, kH - 18.f};
constexpr D2D1_RECT_F kPrivExitRow = {kPrivPanel.left + 14.f, 400.f, kPrivPanel.right - 14.f, 442.f};
constexpr D2D1_RECT_F kPrivSecureRow = {kPrivPanel.left + 14.f, 450.f, kPrivPanel.right - 14.f, 492.f};
constexpr D2D1_RECT_F kPrivCleanBtn = {kPrivPanel.left + 14.f, 540.f, kW / 2.f - 6.f, 582.f};
constexpr D2D1_RECT_F kPrivBackBtn = {kW / 2.f + 6.f, 540.f, kPrivPanel.right - 14.f, 582.f};
constexpr D2D1_RECT_F kGraph = {24.f, 448.f, kW - 24.f, 512.f};
constexpr D2D1_RECT_F kToggleCard = {24.f, 578.f, kW - 24.f, 630.f};
constexpr float kChipsTop = 528.f;
constexpr float kChipsH = 34.f;
constexpr std::array<int, 6> kPresets = {8, 10, 12, 14, 16, 20};
constexpr float kArcStart = 135.f;  // degres, bas-gauche
constexpr float kArcSweep = 270.f;
constexpr float kPi = 3.14159265358979f;
constexpr double kGraphWindow = 3.0;  // secondes affichees dans l'oscilloscope

constexpr UINT_PTR kFrameTimer = 1;
constexpr int kHotkeyToggle = 1;
constexpr int kHotkeyQuit = 2;

enum class Hit {
    None, Close, Minimize, Shield, Gear, BtnLeft, BtnRight, Dial, Chip, Toggle, HotkeyChip,
    PrivExit, PrivSecure, PrivClean, PrivBack,
    GearBack, ProfLoad, ProfSave
};

// ---------------------------------------------------------------------------
// Couleurs
// ---------------------------------------------------------------------------
D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1.f) {
    return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a);
}

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

const D2D1_COLOR_F kCyan = Rgb(0, 229, 255);
const D2D1_COLOR_F kViolet = Rgb(139, 92, 246);
const D2D1_COLOR_F kPink = Rgb(255, 46, 136);
const D2D1_COLOR_F kText = Rgb(236, 236, 245);
const D2D1_COLOR_F kMuted = Rgb(140, 140, 165);
const D2D1_COLOR_F kGreen = Rgb(52, 245, 160);
const D2D1_COLOR_F kAmber = Rgb(255, 184, 76);

// Degrade signature cyan -> violet -> rose, t dans [0,1].
D2D1_COLOR_F Spectrum(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t < 0.5f ? Mix(kCyan, kViolet, t * 2.f) : Mix(kViolet, kPink, (t - 0.5f) * 2.f);
}

D2D1_POINT_2F Polar(D2D1_POINT_2F c, float r, float deg) {
    const float rad = deg * kPi / 180.f;
    return {c.x + r * std::cos(rad), c.y + r * std::sin(rad)};
}

float CpsToAngle(float cps) {
    const float t = (cps - clicker::kMinCps) / float(clicker::kMaxCps - clicker::kMinCps);
    return kArcStart + kArcSweep * std::clamp(t, 0.f, 1.f);
}

bool InRect(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

// Rapproche `value` de `target` independamment du framerate.
float Approach(float value, float target, float speed, float dt) {
    return target + (value - target) * std::exp(-speed * dt);
}

std::wstring FormatThousands(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), L" ");
    return s;
}

// ---------------------------------------------------------------------------
// Persistance des reglages (%APPDATA%\LunarAutoClicker\settings.ini)
// ---------------------------------------------------------------------------
std::wstring SettingsPath() {
    wchar_t* appData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        path = std::wstring(appData) + L"\\LunarAutoClicker";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\settings.ini";
    }
    CoTaskMemFree(appData);
    return path;
}

// Option de confidentialite, desactivee par defaut.
bool g_cleanOnExit = false;
bool g_secureWipe = false;

// Raccourci global on/off (configurable).
int g_toggleVk = VK_F8;

// Profils : CPS + bouton memorises dans 3 emplacements.
struct Profile {
    bool used = false;
    int cps = 12;
    bool right = false;
};
constexpr int kProfileCount = 3;
Profile g_profiles[kProfileCount];

// Nom lisible d'une touche virtuelle (pour l'affichage du raccourci).
std::wstring KeyName(int vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, wchar_t(vk));
    switch (vk) {
    case VK_SPACE: return L"Espace";
    case VK_OEM_3: return L"²";
    case VK_TAB: return L"Tab";
    case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return L"Maj";
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return L"Ctrl";
    case VK_XBUTTON1: return L"Souris4";
    case VK_XBUTTON2: return L"Souris5";
    default: break;
    }
    const UINT sc = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
    wchar_t name[64];
    if (sc && GetKeyNameTextW(LONG(sc << 16), name, 64) > 0) return name;
    return L"Touche " + std::to_wstring(vk);
}

void LoadSettings() {
    const std::wstring path = SettingsPath();
    clicker::SetCps(int(GetPrivateProfileIntW(L"clicker", L"cps", 12, path.c_str())));
    clicker::SetEnabled(GetPrivateProfileIntW(L"clicker", L"enabled", 1, path.c_str()) != 0);
    clicker::SetRightButton(GetPrivateProfileIntW(L"clicker", L"right_button", 0, path.c_str()) != 0);
    g_toggleVk = int(GetPrivateProfileIntW(L"hotkey", L"toggle_vk", VK_F8, path.c_str()));
    g_cleanOnExit = GetPrivateProfileIntW(L"privacy", L"clean_on_exit", 0, path.c_str()) != 0;
    g_secureWipe = GetPrivateProfileIntW(L"privacy", L"secure_wipe", 0, path.c_str()) != 0;
    for (int i = 0; i < kProfileCount; ++i) {
        const std::wstring s = L"profile" + std::to_wstring(i);
        g_profiles[i].used = GetPrivateProfileIntW(s.c_str(), L"used", 0, path.c_str()) != 0;
        g_profiles[i].cps = int(GetPrivateProfileIntW(s.c_str(), L"cps", 12, path.c_str()));
        g_profiles[i].right = GetPrivateProfileIntW(s.c_str(), L"right", 0, path.c_str()) != 0;
    }
}

void SaveSettings() {
    const std::wstring path = SettingsPath();
    WritePrivateProfileStringW(L"clicker", L"cps", std::to_wstring(clicker::GetCps()).c_str(), path.c_str());
    WritePrivateProfileStringW(L"clicker", L"enabled", clicker::IsEnabled() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"clicker", L"right_button", clicker::UsesRightButton() ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"hotkey", L"toggle_vk", std::to_wstring(g_toggleVk).c_str(), path.c_str());
    WritePrivateProfileStringW(L"privacy", L"clean_on_exit", g_cleanOnExit ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"privacy", L"secure_wipe", g_secureWipe ? L"1" : L"0", path.c_str());
    for (int i = 0; i < kProfileCount; ++i) {
        const std::wstring s = L"profile" + std::to_wstring(i);
        WritePrivateProfileStringW(s.c_str(), L"used", g_profiles[i].used ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(s.c_str(), L"cps", std::to_wstring(g_profiles[i].cps).c_str(), path.c_str());
        WritePrivateProfileStringW(s.c_str(), L"right", g_profiles[i].right ? L"1" : L"0", path.c_str());
    }
}

// ---------------------------------------------------------------------------
// Fenetre + rendu
// ---------------------------------------------------------------------------
class App {
public:
    bool Create(HINSTANCE instance);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);

    bool CreateDeviceIndependent();
    bool CreateDeviceResources();
    void DiscardDeviceResources();
    ComPtr<IDWriteTextFormat> MakeFormat(float size, DWRITE_FONT_WEIGHT weight,
                                         DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);

    void Tick();
    void Render();
    void DrawBackground();
    void DrawHeader();
    void DrawStatus();
    void DrawDial();
    void DrawGraph();
    void DrawChips();
    void DrawToggle();
    void DrawPrivacy();
    void RunCleanup();
    void DrawGear();
    D2D1_RECT_F ProfileRow(int i) const;
    D2D1_RECT_F ProfileLoadBtn(int i) const;
    D2D1_RECT_F ProfileSaveBtn(int i) const;
    void RegisterToggleHotkey();

    void FillRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c);
    void StrokeRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width = 1.f);
    void Text(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r, D2D1_COLOR_F c);
    void Arc(D2D1_POINT_2F c, float r, float fromDeg, float toDeg, D2D1_COLOR_F color, float width);

    Hit HitTest(float x, float y, int* chip = nullptr) const;
    D2D1_RECT_F ChipRect(int i) const;
    void SetCpsFromPoint(float x, float y);
    void ToggleEnabled();

    HWND hwnd_ = nullptr;
    float scale_ = 1.f;

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1LinearGradientBrush> spectrumBrush_;
    ComPtr<ID2D1LinearGradientBrush> bgBrush_;
    ComPtr<ID2D1RadialGradientBrush> glowBrush_;
    ComPtr<ID2D1StrokeStyle> roundStroke_;  // reutilise (evite une recreation par frame)

    ComPtr<IDWriteTextFormat> fmtLogo_, fmtLogoThin_, fmtBig_, fmtLabel_, fmtSmall_, fmtSmallCenter_,
        fmtChip_, fmtCard_, fmtIcon_, fmtBody_;

    // Etat d'interaction
    Hit hover_ = Hit::None;
    int hoverChip_ = -1;
    bool dragging_ = false;
    bool trackingMouse_ = false;

    // Panneau confidentialite
    bool showPrivacy_ = false;
    float privacyAnim_ = 0.f;
    std::wstring cleanupResult_;

    // Panneau reglages / profils / stats
    bool showGear_ = false;
    float gearAnim_ = 0.f;
    int hoverProfile_ = -1;
    bool capturingHotkey_ = false;  // en attente d'une touche pour le raccourci
    float maxCps_ = 0.f;            // CPS reel max de la session

    // Animation
    LARGE_INTEGER freq_{}, last_{};
    double time_ = 0.0;
    float displayCps_ = 12.f;
    float measuredCps_ = 0.f;
    float enabledAnim_ = 1.f;
    float holdAnim_ = 0.f;
    float focusAnim_ = 0.f;
    float pulse_ = 0.f;
    float hoverClose_ = 0.f, hoverMin_ = 0.f, hoverDial_ = 0.f, hoverToggle_ = 0.f;
    std::array<float, kPresets.size()> hoverChips_{};
    uint64_t lastClickCount_ = 0;
    std::deque<double> clickTimes_;
    UINT curTimerMs_ = 16;
};

bool App::Create(HINSTANCE instance) {
    if (!CreateDeviceIndependent()) return false;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"LunarAutoClickerWnd";
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    scale_ = dpi / 96.f;
    const int w = int(std::lround(kW * scale_));
    const int h = int(std::lround(kH * scale_));
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + (work.right - work.left - w) / 2;
    const int y = work.top + (work.bottom - work.top - h) / 2;

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Lunar AutoClicker", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                            x, y, w, h, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    // Coins arrondis + ombre natifs Windows 11.
    const DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const MARGINS shadow{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd_, &shadow);

    RegisterToggleHotkey();
    RegisterHotKey(hwnd_, kHotkeyQuit, MOD_NOREPEAT, VK_F10);

    displayCps_ = float(clicker::GetCps());
    enabledAnim_ = clicker::IsEnabled() ? 1.f : 0.f;
    lastClickCount_ = clicker::ClickCount();

    QueryPerformanceFrequency(&freq_);
    QueryPerformanceCounter(&last_);
    curTimerMs_ = 16;
    SetTimer(hwnd_, kFrameTimer, curTimerMs_, nullptr);  // 60 fps ; abaisse a 30 au repos

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

bool App::CreateDeviceIndependent() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;

    fmtLogo_ = MakeFormat(17.f, DWRITE_FONT_WEIGHT_BOLD);
    fmtLogoThin_ = MakeFormat(17.f, DWRITE_FONT_WEIGHT_LIGHT);
    fmtBig_ = MakeFormat(92.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmtLabel_ = MakeFormat(11.f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmtSmall_ = MakeFormat(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmtSmallCenter_ = MakeFormat(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmtChip_ = MakeFormat(14.f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmtCard_ = MakeFormat(15.f, DWRITE_FONT_WEIGHT_BOLD);
    fmtIcon_ = MakeFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
                            roundStroke_.GetAddressOf());
    fmtBody_ = MakeFormat(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    if (fmtBody_) {
        fmtBody_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        fmtBody_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    return fmtLogo_ && fmtBig_ && fmtIcon_ && fmtBody_;
}

ComPtr<IDWriteTextFormat> App::MakeFormat(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"fr-fr", fmt.GetAddressOf())))
        return nullptr;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return fmt;
}

bool App::CreateDeviceResources() {
    if (rt_) return true;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    const auto props = D2D1::RenderTargetProperties();
    const auto hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(UINT32(rc.right - rc.left), UINT32(rc.bottom - rc.top)));
    if (FAILED(d2d_->CreateHwndRenderTarget(props, hwndProps, rt_.GetAddressOf()))) return false;
    rt_->SetDpi(96.f * scale_, 96.f * scale_);
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    rt_->CreateSolidColorBrush(kText, brush_.GetAddressOf());

    const D2D1_GRADIENT_STOP spectrumStops[] = {{0.f, kCyan}, {0.5f, kViolet}, {1.f, kPink}};
    ComPtr<ID2D1GradientStopCollection> spectrum;
    rt_->CreateGradientStopCollection(spectrumStops, 3, spectrum.GetAddressOf());
    rt_->CreateLinearGradientBrush({}, spectrum.Get(), spectrumBrush_.GetAddressOf());

    const D2D1_GRADIENT_STOP bgStops[] = {{0.f, Rgb(13, 12, 22)}, {1.f, Rgb(6, 6, 11)}};
    ComPtr<ID2D1GradientStopCollection> bg;
    rt_->CreateGradientStopCollection(bgStops, 2, bg.GetAddressOf());
    rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {kW * 0.4f, kH}), bg.Get(),
                                   bgBrush_.GetAddressOf());

    const D2D1_GRADIENT_STOP glowStops[] = {{0.f, WithAlpha(kViolet, 0.55f)},
                                            {0.45f, WithAlpha(kViolet, 0.16f)},
                                            {1.f, WithAlpha(kViolet, 0.f)}};
    ComPtr<ID2D1GradientStopCollection> glow;
    rt_->CreateGradientStopCollection(glowStops, 3, glow.GetAddressOf());
    rt_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(kDialCenter, {0, 0}, 230.f, 230.f),
                                   glow.Get(), glowBrush_.GetAddressOf());
    return brush_ && spectrumBrush_ && bgBrush_ && glowBrush_;
}

void App::DiscardDeviceResources() {
    glowBrush_.Reset();
    bgBrush_.Reset();
    spectrumBrush_.Reset();
    brush_.Reset();
    rt_.Reset();
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------
void App::FillRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
}

void App::StrokeRound(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width) {
    brush_->SetColor(c);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get(), width);
}

void App::Text(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    rt_->DrawTextW(s.c_str(), UINT32(s.size()), fmt, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void App::Arc(D2D1_POINT_2F c, float r, float fromDeg, float toDeg, D2D1_COLOR_F color, float width) {
    if (toDeg - fromDeg < 0.1f) return;
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(d2d_->CreatePathGeometry(path.GetAddressOf()))) return;
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(sink.GetAddressOf());
    sink->BeginFigure(Polar(c, r, fromDeg), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(Polar(c, r, toDeg), {r, r}, 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  toDeg - fromDeg > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    brush_->SetColor(color);
    rt_->DrawGeometry(path.Get(), brush_.Get(), width, roundStroke_.Get());
}

// ---------------------------------------------------------------------------
// Boucle d'animation
// ---------------------------------------------------------------------------
void App::Tick() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = std::min(0.1f, float(now.QuadPart - last_.QuadPart) / float(freq_.QuadPart));
    last_ = now;
    time_ += dt;

    // Nouveaux clics -> horodatage pour le CPS reel et l'oscilloscope.
    const uint64_t count = clicker::ClickCount();
    if (count > lastClickCount_) {
        const uint64_t added = std::min<uint64_t>(count - lastClickCount_, 64);
        for (uint64_t i = 0; i < added; ++i) clickTimes_.push_back(time_);
        pulse_ = 1.f;
    }
    lastClickCount_ = count;
    while (!clickTimes_.empty() && time_ - clickTimes_.front() > kGraphWindow) clickTimes_.pop_front();

    const auto inLastSecond = std::count_if(clickTimes_.begin(), clickTimes_.end(),
                                            [&](double t) { return time_ - t <= 1.0; });

    displayCps_ = Approach(displayCps_, float(clicker::GetCps()), 14.f, dt);
    measuredCps_ = Approach(measuredCps_, float(inLastSecond), 6.f, dt);
    maxCps_ = std::max(maxCps_, float(inLastSecond));  // CPS reel max de la session
    enabledAnim_ = Approach(enabledAnim_, clicker::IsEnabled() ? 1.f : 0.f, 12.f, dt);
    holdAnim_ = Approach(holdAnim_, clicker::IsHolding() ? 1.f : 0.f, 18.f, dt);
    focusAnim_ = Approach(focusAnim_, clicker::IsTargetFocused() ? 1.f : 0.f, 8.f, dt);
    pulse_ = Approach(pulse_, 0.f, 9.f, dt);

    hoverClose_ = Approach(hoverClose_, hover_ == Hit::Close ? 1.f : 0.f, 16.f, dt);
    hoverMin_ = Approach(hoverMin_, hover_ == Hit::Minimize ? 1.f : 0.f, 16.f, dt);
    hoverDial_ = Approach(hoverDial_, (hover_ == Hit::Dial || dragging_) ? 1.f : 0.f, 10.f, dt);
    hoverToggle_ = Approach(hoverToggle_, hover_ == Hit::Toggle ? 1.f : 0.f, 14.f, dt);
    for (size_t i = 0; i < hoverChips_.size(); ++i)
        hoverChips_[i] = Approach(hoverChips_[i], (hover_ == Hit::Chip && hoverChip_ == int(i)) ? 1.f : 0.f, 16.f, dt);
    privacyAnim_ = Approach(privacyAnim_, showPrivacy_ ? 1.f : 0.f, 16.f, dt);
    gearAnim_ = Approach(gearAnim_, showGear_ ? 1.f : 0.f, 16.f, dt);

    // Cadence adaptative : 60 fps quand on interagit ou clique, 30 fps au repos
    // (le fond respire lentement, 30 fps reste fluide) -> CPU/GPU reduits.
    const bool active = clicker::IsHolding() || dragging_ || showPrivacy_ || showGear_ || hover_ != Hit::None ||
                        pulse_ > 0.02f || measuredCps_ > 0.05f || (privacyAnim_ > 0.01f && privacyAnim_ < 0.99f) ||
                        (gearAnim_ > 0.01f && gearAnim_ < 0.99f);
    const UINT target = active ? 16u : 33u;
    if (target != curTimerMs_) {
        curTimerMs_ = target;
        SetTimer(hwnd_, kFrameTimer, curTimerMs_, nullptr);
    }

    if (!IsIconic(hwnd_)) Render();
}

void App::Render() {
    if (!CreateDeviceResources()) return;

    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(Rgb(6, 6, 11));

    DrawBackground();
    DrawHeader();
    DrawStatus();
    DrawDial();
    DrawGraph();
    DrawChips();
    DrawToggle();
    if (privacyAnim_ > 0.004f) DrawPrivacy();
    if (gearAnim_ > 0.004f) DrawGear();

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

void App::DrawBackground() {
    rt_->FillRectangle({0, 0, kW, kH}, bgBrush_.Get());

    // Halo qui respire derriere le cadran, plus intense a chaque clic.
    const float breathe = 0.5f + 0.5f * std::sin(float(time_) * 1.6f);
    glowBrush_->SetOpacity(0.35f + 0.25f * enabledAnim_ + 0.12f * breathe + 0.5f * pulse_);
    const float r = 210.f + 14.f * breathe + 30.f * pulse_;
    glowBrush_->SetRadiusX(r);
    glowBrush_->SetRadiusY(r);
    rt_->FillEllipse(D2D1::Ellipse(kDialCenter, r, r), glowBrush_.Get());

    // Grille de points facon HUD.
    brush_->SetColor(WithAlpha(kText, 0.045f));
    for (float y = kHeaderH + 10.f; y < kH; y += 22.f)
        for (float x = 11.f; x < kW; x += 22.f) rt_->FillEllipse(D2D1::Ellipse({x, y}, 0.9f, 0.9f), brush_.Get());

    // Liseré lumineux en haut + bordure.
    spectrumBrush_->SetStartPoint({0, 0});
    spectrumBrush_->SetEndPoint({kW, 0});
    spectrumBrush_->SetOpacity(0.35f + 0.5f * enabledAnim_);
    rt_->FillRectangle({0, 0, kW, 2.f}, spectrumBrush_.Get());
    spectrumBrush_->SetOpacity(1.f);
    StrokeRound({0.5f, 0.5f, kW - 0.5f, kH - 0.5f}, 8.f, WithAlpha(kText, 0.07f));
}

void App::DrawHeader() {
    // Logo : losange degrade + "LUNAR" gras + "CLICKER" fin.
    const D2D1_POINT_2F logo = {34.f, kHeaderH / 2.f + 2.f};
    rt_->SetTransform(D2D1::Matrix3x2F::Rotation(45.f, logo));
    spectrumBrush_->SetStartPoint({logo.x - 8.f, logo.y - 8.f});
    spectrumBrush_->SetEndPoint({logo.x + 8.f, logo.y + 8.f});
    rt_->FillRoundedRectangle(D2D1::RoundedRect({logo.x - 7.f, logo.y - 7.f, logo.x + 7.f, logo.y + 7.f}, 3.f, 3.f),
                              spectrumBrush_.Get());
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    brush_->SetColor(Rgb(13, 12, 22));
    rt_->FillEllipse(D2D1::Ellipse(logo, 2.6f, 2.6f), brush_.Get());

    Text(L"LUNAR", fmtLogo_.Get(), {54.f, 14.f, 130.f, 44.f}, kText);
    Text(L"CLICKER", fmtLogoThin_.Get(), {113.f, 14.f, 220.f, 44.f}, WithAlpha(kText, 0.55f));

    // Bouton bouclier (confidentialite).
    const float shieldHover = (hover_ == Hit::Shield) ? 1.f : 0.f;
    FillRound(kShieldBtn, 8.f, WithAlpha(kText, 0.08f * shieldHover + 0.06f * privacyAnim_));
    const D2D1_COLOR_F shieldCol = Mix(kMuted, g_cleanOnExit ? kGreen : kText, std::max(shieldHover, privacyAnim_));
    const float sx = (kShieldBtn.left + kShieldBtn.right) / 2.f, sy = (kShieldBtn.top + kShieldBtn.bottom) / 2.f;
    {
        ComPtr<ID2D1PathGeometry> sh;
        if (SUCCEEDED(d2d_->CreatePathGeometry(sh.GetAddressOf()))) {
            ComPtr<ID2D1GeometrySink> sink;
            sh->Open(sink.GetAddressOf());
            sink->BeginFigure({sx, sy - 8.f}, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine({sx + 6.f, sy - 5.f});
            sink->AddLine({sx + 6.f, sy + 1.f});
            sink->AddLine({sx, sy + 8.f});
            sink->AddLine({sx - 6.f, sy + 1.f});
            sink->AddLine({sx - 6.f, sy - 5.f});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            brush_->SetColor(shieldCol);
            rt_->DrawGeometry(sh.Get(), brush_.Get(), 1.4f);
            if (g_cleanOnExit) {
                brush_->SetColor(WithAlpha(kGreen, 0.9f));
                rt_->DrawLine({sx - 2.5f, sy}, {sx - 0.5f, sy + 2.5f}, brush_.Get(), 1.5f);
                rt_->DrawLine({sx - 0.5f, sy + 2.5f}, {sx + 3.f, sy - 3.f}, brush_.Get(), 1.5f);
            }
        }
    }

    // Selecteur de bouton simule : clic gauche / droit.
    const bool right = clicker::UsesRightButton();
    FillRound(kBtnSeg, 8.f, WithAlpha(kText, 0.05f));
    StrokeRound(kBtnSeg, 8.f, WithAlpha(kText, 0.08f));
    const float mid = (kBtnSeg.left + kBtnSeg.right) / 2.f;
    const D2D1_RECT_F cellL = {kBtnSeg.left + 2.f, kBtnSeg.top + 2.f, mid - 1.f, kBtnSeg.bottom - 2.f};
    const D2D1_RECT_F cellR = {mid + 1.f, kBtnSeg.top + 2.f, kBtnSeg.right - 2.f, kBtnSeg.bottom - 2.f};
    const D2D1_RECT_F active = right ? cellR : cellL;
    spectrumBrush_->SetStartPoint({active.left, 0});
    spectrumBrush_->SetEndPoint({active.right, 0});
    spectrumBrush_->SetOpacity(0.9f);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(active, 6.f, 6.f), spectrumBrush_.Get());
    spectrumBrush_->SetOpacity(1.f);
    Text(L"GAUCHE", fmtLabel_.Get(), cellL, right ? WithAlpha(kMuted, 0.8f) : Rgb(10, 10, 18));
    Text(L"DROIT", fmtLabel_.Get(), cellR, right ? Rgb(10, 10, 18) : WithAlpha(kMuted, 0.8f));

    // Bouton engrenage (reglages / profils).
    const float gearHover = (hover_ == Hit::Gear) ? 1.f : 0.f;
    FillRound(kGearBtn, 8.f, WithAlpha(kText, 0.08f * gearHover + 0.06f * gearAnim_));
    const D2D1_COLOR_F gearCol = Mix(kMuted, kText, std::max(gearHover, gearAnim_));
    const float gx = (kGearBtn.left + kGearBtn.right) / 2.f, gy = (kGearBtn.top + kGearBtn.bottom) / 2.f;
    brush_->SetColor(gearCol);
    for (int i = 0; i < 6; ++i) {
        const float ang = kPi * i / 3.f + float(time_) * 0.4f;
        rt_->DrawLine({gx + 4.f * std::cos(ang), gy + 4.f * std::sin(ang)},
                      {gx + 7.5f * std::cos(ang), gy + 7.5f * std::sin(ang)}, brush_.Get(), 1.8f);
    }
    rt_->DrawEllipse(D2D1::Ellipse({gx, gy}, 3.2f, 3.2f), brush_.Get(), 1.5f);

    // Boutons fenetre.
    FillRound(kMinBtn, 8.f, WithAlpha(kText, 0.08f * hoverMin_));
    brush_->SetColor(Mix(kMuted, kText, hoverMin_));
    const float my = (kMinBtn.top + kMinBtn.bottom) / 2.f;
    rt_->DrawLine({kMinBtn.left + 10.f, my}, {kMinBtn.right - 10.f, my}, brush_.Get(), 1.4f);

    FillRound(kCloseBtn, 8.f, WithAlpha(kPink, 0.85f * hoverClose_));
    brush_->SetColor(Mix(kMuted, kText, hoverClose_));
    const D2D1_POINT_2F cc = {(kCloseBtn.left + kCloseBtn.right) / 2.f, (kCloseBtn.top + kCloseBtn.bottom) / 2.f};
    rt_->DrawLine({cc.x - 5.f, cc.y - 5.f}, {cc.x + 5.f, cc.y + 5.f}, brush_.Get(), 1.4f);
    rt_->DrawLine({cc.x - 5.f, cc.y + 5.f}, {cc.x + 5.f, cc.y - 5.f}, brush_.Get(), 1.4f);
}

void App::DrawStatus() {
    // Pastille cible : verte si Lunar est au premier plan.
    const D2D1_RECT_F target = {24.f, 66.f, 232.f, 96.f};
    const D2D1_COLOR_F targetColor = Mix(kAmber, kGreen, focusAnim_);
    FillRound(target, 15.f, WithAlpha(targetColor, 0.08f));
    StrokeRound(target, 15.f, WithAlpha(targetColor, 0.28f));
    const float blink = focusAnim_ > 0.5f ? 1.f : 0.45f + 0.55f * (0.5f + 0.5f * std::sin(float(time_) * 4.f));
    brush_->SetColor(WithAlpha(targetColor, 0.25f * blink));
    rt_->FillEllipse(D2D1::Ellipse({42.f, 81.f}, 7.f, 7.f), brush_.Get());
    brush_->SetColor(WithAlpha(targetColor, blink));
    rt_->FillEllipse(D2D1::Ellipse({42.f, 81.f}, 3.5f, 3.5f), brush_.Get());
    Text(focusAnim_ > 0.5f ? L"LUNAR CLIENT DÉTECTÉ" : L"EN ATTENTE DE LUNAR", fmtSmall_.Get(),
         {56.f, 66.f, 232.f, 96.f}, Mix(kText, targetColor, 0.35f));

    // Pastille "HOLD" : s'allume quand le clic gauche est maintenu.
    const D2D1_RECT_F hold = {kW - 128.f, 66.f, kW - 24.f, 96.f};
    FillRound(hold, 15.f, WithAlpha(kCyan, 0.04f + 0.18f * holdAnim_));
    StrokeRound(hold, 15.f, WithAlpha(kCyan, 0.15f + 0.6f * holdAnim_));
    // Mini souris.
    const D2D1_RECT_F mouse = {hold.left + 16.f, 73.f, hold.left + 28.f, 89.f};
    StrokeRound(mouse, 6.f, Mix(kMuted, kCyan, holdAnim_), 1.3f);
    brush_->SetColor(WithAlpha(kCyan, holdAnim_));
    rt_->FillRectangle({mouse.left + 2.f, mouse.top + 2.f, (mouse.left + mouse.right) / 2.f - 0.5f, 80.f},
                       brush_.Get());
    Text(L"HOLD", fmtSmall_.Get(), {hold.left + 38.f, 66.f, hold.right, 96.f}, Mix(kMuted, kText, holdAnim_));
}

void App::DrawDial() {
    const D2D1_POINT_2F c = kDialCenter;
    const float R = kDialR;
    const int ticks = clicker::kMaxCps - clicker::kMinCps + 1;
    const float dim = 0.35f + 0.65f * enabledAnim_;
    ID2D1StrokeStyle* round = roundStroke_.Get();

    // Anneau exterieur fin + graduations "bourse".
    Arc(c, R + 26.f, kArcStart, kArcStart + kArcSweep, WithAlpha(kText, 0.06f), 1.f);
    for (int i = 0; i <= ticks; ++i) {
        const float a = kArcStart + kArcSweep * i / float(ticks);
        brush_->SetColor(WithAlpha(kText, i % 5 == 0 ? 0.25f : 0.08f));
        rt_->DrawLine(Polar(c, R + 22.f, a), Polar(c, R + (i % 5 == 0 ? 30.f : 26.f), a), brush_.Get(), 1.f);
    }

    // Segments principaux : 1 segment = 1 CPS, allumes jusqu'a la cible.
    for (int i = 0; i < ticks; ++i) {
        const float a = CpsToAngle(float(clicker::kMinCps + i));
        const float lit = std::clamp(displayCps_ - float(clicker::kMinCps + i) + 1.f, 0.f, 1.f);
        const D2D1_COLOR_F col = Spectrum(i / float(ticks - 1));
        const D2D1_POINT_2F p0 = Polar(c, R - 11.f, a);
        const D2D1_POINT_2F p1 = Polar(c, R + 11.f, a);

        if (lit > 0.f) {
            brush_->SetColor(WithAlpha(col, 0.18f * lit * dim));
            rt_->DrawLine(p0, p1, brush_.Get(), 13.f, round);
        }
        brush_->SetColor(Mix(WithAlpha(kText, 0.08f), WithAlpha(col, dim), lit));
        rt_->DrawLine(p0, p1, brush_.Get(), 5.f, round);
    }

    // Anneau interieur : CPS reellement mesure.
    const float innerR = R - 30.f;
    Arc(c, innerR, kArcStart, kArcStart + kArcSweep, WithAlpha(kText, 0.06f), 3.f);
    if (measuredCps_ > 0.05f) {
        const float end = CpsToAngle(std::max(measuredCps_, float(clicker::kMinCps)));
        Arc(c, innerR, kArcStart, end, WithAlpha(kCyan, 0.25f), 9.f);
        Arc(c, innerR, kArcStart, end, kCyan, 3.f);
    }

    // Curseur du cadran.
    const float knobA = CpsToAngle(displayCps_);
    const D2D1_POINT_2F knob = Polar(c, R, knobA);
    const D2D1_COLOR_F knobCol = Spectrum((displayCps_ - clicker::kMinCps) / float(ticks - 1));
    const float knobR = 8.f + 2.f * hoverDial_;
    brush_->SetColor(WithAlpha(knobCol, 0.25f + 0.2f * hoverDial_));
    rt_->FillEllipse(D2D1::Ellipse(knob, knobR + 8.f, knobR + 8.f), brush_.Get());
    brush_->SetColor(kText);
    rt_->FillEllipse(D2D1::Ellipse(knob, knobR, knobR), brush_.Get());
    brush_->SetColor(knobCol);
    rt_->FillEllipse(D2D1::Ellipse(knob, knobR - 4.f, knobR - 4.f), brush_.Get());

    // Onde de choc a chaque clic.
    if (pulse_ > 0.02f) {
        const float pr = innerR - 16.f - 10.f * (1.f - pulse_);
        brush_->SetColor(WithAlpha(knobCol, 0.35f * pulse_));
        rt_->DrawEllipse(D2D1::Ellipse(c, pr, pr), brush_.Get(), 2.f);
    }

    // Disque central en verre.
    const float discR = innerR - 18.f;
    brush_->SetColor(Rgb(16, 15, 28, 0.85f));
    rt_->FillEllipse(D2D1::Ellipse(c, discR, discR), brush_.Get());
    brush_->SetColor(WithAlpha(kText, 0.06f + 0.06f * hoverDial_));
    rt_->DrawEllipse(D2D1::Ellipse(c, discR, discR), brush_.Get(), 1.f);

    // Valeur cible.
    const int shown = int(std::lround(displayCps_));
    Text(L"CPS CIBLE", fmtLabel_.Get(), {c.x - 80.f, c.y - 62.f, c.x + 80.f, c.y - 44.f}, WithAlpha(kMuted, 0.9f));
    spectrumBrush_->SetStartPoint({c.x - 50.f, c.y - 40.f});
    spectrumBrush_->SetEndPoint({c.x + 50.f, c.y + 40.f});
    spectrumBrush_->SetOpacity(dim);
    const std::wstring big = std::to_wstring(shown);
    rt_->DrawTextW(big.c_str(), UINT32(big.size()), fmtBig_.Get(), {c.x - 90.f, c.y - 52.f, c.x + 90.f, c.y + 42.f},
                   spectrumBrush_.Get());
    spectrumBrush_->SetOpacity(1.f);

    wchar_t real[32];
    swprintf_s(real, L"RÉEL  %.1f", measuredCps_);
    Text(real, fmtSmallCenter_.Get(), {c.x - 80.f, c.y + 40.f, c.x + 80.f, c.y + 60.f},
         Mix(kMuted, kCyan, std::min(1.f, measuredCps_ / 3.f)));

    // Indice d'interaction.
    Text(L"glisser  ·  molette  ·  ← →", fmtSmallCenter_.Get(), {c.x - 120.f, c.y + R + 16.f, c.x + 120.f, c.y + R + 36.f},
         WithAlpha(kMuted, 0.35f + 0.4f * hoverDial_));
}

void App::DrawGraph() {
    const D2D1_RECT_F& g = kGraph;
    FillRound(g, 12.f, WithAlpha(kText, 0.025f));
    StrokeRound(g, 12.f, WithAlpha(kText, 0.06f));
    Text(L"ACTIVITÉ", fmtLabel_.Get(), {g.left + 12.f, g.top + 6.f, g.left + 70.f, g.top + 20.f},
         WithAlpha(kMuted, 0.7f));

    const float baseY = g.bottom - 12.f;
    const float top = g.top + 22.f;
    const float left = g.left + 10.f;
    const float right = g.right - 10.f;

    brush_->SetColor(WithAlpha(kText, 0.07f));
    rt_->DrawLine({left, baseY}, {right, baseY}, brush_.Get(), 1.f);

    // Signal = somme de gaussiennes centrees sur chaque clic, defilant vers la gauche.
    constexpr int samples = 140;
    std::array<D2D1_POINT_2F, samples> pts{};
    for (int s = 0; s < samples; ++s) {
        const float u = s / float(samples - 1);
        const double t = time_ - kGraphWindow * (1.0 - u);
        float v = 0.f;
        for (double ct : clickTimes_) {
            const float d = float((t - ct) / 0.022);
            v += std::exp(-d * d);
        }
        v = std::min(v, 1.f);
        pts[size_t(s)] = {left + (right - left) * u, baseY - (baseY - top) * v};
    }

    ComPtr<ID2D1PathGeometry> fill, line;
    d2d_->CreatePathGeometry(fill.GetAddressOf());
    d2d_->CreatePathGeometry(line.GetAddressOf());
    ComPtr<ID2D1GeometrySink> sink;
    fill->Open(sink.GetAddressOf());
    sink->BeginFigure({left, baseY}, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(pts.data(), UINT32(pts.size()));
    sink->AddLine({right, baseY});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink.Reset();
    line->Open(sink.GetAddressOf());
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(pts.data() + 1, UINT32(pts.size() - 1));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    spectrumBrush_->SetStartPoint({left, 0});
    spectrumBrush_->SetEndPoint({right, 0});
    spectrumBrush_->SetOpacity(0.18f);
    rt_->FillGeometry(fill.Get(), spectrumBrush_.Get());
    spectrumBrush_->SetOpacity(1.f);
    rt_->DrawGeometry(line.Get(), spectrumBrush_.Get(), 1.6f);

    // Tete de lecture.
    brush_->SetColor(WithAlpha(kPink, 0.5f + 0.5f * pulse_));
    rt_->FillEllipse(D2D1::Ellipse(pts.back(), 2.5f + 2.f * pulse_, 2.5f + 2.f * pulse_), brush_.Get());
}

D2D1_RECT_F App::ChipRect(int i) const {
    constexpr float gap = 8.f;
    const float w = (kW - 48.f - gap * (kPresets.size() - 1)) / kPresets.size();
    const float x = 24.f + i * (w + gap);
    return {x, kChipsTop, x + w, kChipsTop + kChipsH};
}

void App::DrawChips() {
    const int cps = clicker::GetCps();
    for (int i = 0; i < int(kPresets.size()); ++i) {
        const D2D1_RECT_F r = ChipRect(i);
        const float h = hoverChips_[size_t(i)];
        const bool selected = kPresets[size_t(i)] == cps;
        const D2D1_COLOR_F col = Spectrum((kPresets[size_t(i)] - clicker::kMinCps) /
                                          float(clicker::kMaxCps - clicker::kMinCps));
        if (selected) {
            FillRound({r.left - 2.f, r.top - 2.f, r.right + 2.f, r.bottom + 2.f}, 12.f, WithAlpha(col, 0.2f));
            FillRound(r, 10.f, col);
            Text(std::to_wstring(kPresets[size_t(i)]), fmtChip_.Get(), r, Rgb(10, 10, 18));
        } else {
            FillRound(r, 10.f, WithAlpha(kText, 0.035f + 0.05f * h));
            StrokeRound(r, 10.f, Mix(WithAlpha(kText, 0.08f), WithAlpha(col, 0.7f), h));
            Text(std::to_wstring(kPresets[size_t(i)]), fmtChip_.Get(), r, Mix(kMuted, kText, h));
        }
    }
}

void App::DrawToggle() {
    const D2D1_RECT_F& r = kToggleCard;
    FillRound(r, 14.f, WithAlpha(kText, 0.03f + 0.03f * hoverToggle_));
    StrokeRound(r, 14.f, Mix(WithAlpha(kText, 0.07f), WithAlpha(kViolet, 0.5f), enabledAnim_ * 0.6f));

    Text(L"AUTOCLICK", fmtCard_.Get(), {r.left + 18.f, r.top + 6.f, r.left + 200.f, r.top + 28.f},
         Mix(kMuted, kText, enabledAnim_));

    // Pastille du raccourci (cliquable pour redefinir la touche).
    const bool khov = hover_ == Hit::HotkeyChip;
    const D2D1_COLOR_F keyCol = capturingHotkey_ ? kAmber : kCyan;
    FillRound(kHotkeyChip, 6.f, WithAlpha(keyCol, (khov || capturingHotkey_ ? 0.22f : 0.12f)));
    StrokeRound(kHotkeyChip, 6.f, WithAlpha(keyCol, 0.5f));
    Text(capturingHotkey_ ? L"…" : KeyName(g_toggleVk), fmtSmall_.Get(), kHotkeyChip, WithAlpha(kText, 0.95f));
    const std::wstring sub = capturingHotkey_ ? L"appuie sur une touche"
                                              : L"basculer  ·  " + FormatThousands(clicker::ClickCount()) + L" clics";
    Text(sub, fmtSmall_.Get(), {kHotkeyChip.right + 8.f, r.top + 26.f, r.right - 90.f, r.top + 46.f},
         WithAlpha(kMuted, 0.75f));

    // Interrupteur anime.
    const float sw = 56.f, sh = 30.f;
    const D2D1_RECT_F track = {r.right - 18.f - sw, (r.top + r.bottom - sh) / 2.f, r.right - 18.f,
                               (r.top + r.bottom + sh) / 2.f};
    FillRound(track, sh / 2.f, WithAlpha(kText, 0.1f));
    if (enabledAnim_ > 0.01f) {
        spectrumBrush_->SetStartPoint({track.left, 0});
        spectrumBrush_->SetEndPoint({track.right, 0});
        spectrumBrush_->SetOpacity(enabledAnim_);
        rt_->FillRoundedRectangle(D2D1::RoundedRect(track, sh / 2.f, sh / 2.f), spectrumBrush_.Get());
        spectrumBrush_->SetOpacity(1.f);
    }
    const float thumbR = sh / 2.f - 4.f;
    const float tx = track.left + sh / 2.f + (sw - sh) * enabledAnim_;
    const D2D1_POINT_2F thumb = {tx, (track.top + track.bottom) / 2.f};
    brush_->SetColor(Rgb(0, 0, 0, 0.3f));
    rt_->FillEllipse(D2D1::Ellipse({thumb.x, thumb.y + 1.5f}, thumbR, thumbR), brush_.Get());
    brush_->SetColor(Mix(kMuted, kText, enabledAnim_));
    rt_->FillEllipse(D2D1::Ellipse(thumb, thumbR, thumbR), brush_.Get());
}

namespace {
std::wstring FormatBytes(uint64_t n) {
    if (n < 1024) return std::to_wstring(n) + L" o";
    if (n < 1024 * 1024) return std::to_wstring((n + 512) / 1024) + L" Ko";
    return std::to_wstring((n + 512 * 1024) / (1024 * 1024)) + L" Mo";
}
}  // namespace

// Panneau de confidentialite : liste exacte des donnees de l'appli, options de
// nettoyage, effacement securise et declenchement manuel.
void App::DrawPrivacy() {
    const float a = privacyAnim_;
    brush_->SetColor(Rgb(4, 4, 8, 0.72f * a));  // voile sombre sur toute la fenetre
    rt_->FillRectangle({0, 0, kW, kH}, brush_.Get());

    const D2D1_RECT_F& p = kPrivPanel;
    FillRound(p, 16.f, WithAlpha(Rgb(16, 15, 26), a));
    StrokeRound(p, 16.f, WithAlpha(kViolet, 0.35f * a));

    Text(L"CONFIDENTIALITÉ", fmtCard_.Get(), {p.left + 20, 78, p.right - 20, 102}, WithAlpha(kText, a));
    Text(L"Nettoyage des données créées par l'outil", fmtSmall_.Get(), {p.left + 20, 102, p.right - 20, 120},
         WithAlpha(kMuted, a));
    Text(L"Supprime uniquement les fichiers créés par Lunar AutoClicker dans son "
         L"propre dossier. Ne touche jamais aux journaux Windows, à l'anti-triche "
         L"ni aux fichiers d'autres logiciels.",
         fmtBody_.Get(), {p.left + 20, 126, p.right - 20, 188}, WithAlpha(kMuted, a));

    Text(L"CES DONNÉES SERONT SUPPRIMÉES", fmtSmall_.Get(), {p.left + 20, 192, p.right - 20, 210},
         WithAlpha(kCyan, 0.85f * a));

    const D2D1_RECT_F list = {p.left + 14, 212, p.right - 14, 392};
    FillRound(list, 10.f, WithAlpha(Rgb(8, 8, 14), a));
    StrokeRound(list, 10.f, WithAlpha(kText, 0.06f * a));
    const auto items = cleanup::ListAppData();
    if (items.empty()) {
        Text(L"Aucune donnée pour le moment — rien à supprimer.", fmtSmall_.Get(),
             {list.left + 14, list.top, list.right - 14, list.bottom}, WithAlpha(kMuted, a));
    } else {
        // Raccourci d'affichage : remplace le profil utilisateur par %APPDATA%.
        wchar_t* appData = nullptr;
        std::wstring prefix;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) prefix = appData;
        CoTaskMemFree(appData);

        float y = list.top + 10;
        for (const auto& it : items) {
            if (y + 20 > list.bottom) break;
            std::wstring shown = it.path;
            if (!prefix.empty() && shown.rfind(prefix, 0) == 0) shown = L"%APPDATA%" + shown.substr(prefix.size());
            brush_->SetColor(WithAlpha(kCyan, 0.7f * a));
            rt_->FillEllipse(D2D1::Ellipse({list.left + 16, y + 10}, 2.f, 2.f), brush_.Get());
            Text(shown, fmtSmall_.Get(), {list.left + 28, y, list.right - 72, y + 20}, WithAlpha(kText, a));
            Text(FormatBytes(it.size), fmtSmall_.Get(), {list.right - 68, y, list.right - 12, y + 20},
                 WithAlpha(kMuted, a));
            y += 22;
        }
        Text(L"+ données en mémoire (compteur, historique d'activité)", fmtSmall_.Get(),
             {list.left + 14, y + 2, list.right - 14, y + 22}, WithAlpha(kMuted, 0.7f * a));
    }

    // Interrupteur reutilisable (couleur verte quand actif).
    auto drawSwitch = [&](const D2D1_RECT_F& row, bool on) {
        const float sw = 46.f, sh = 26.f;
        const D2D1_RECT_F track = {row.right - 14 - sw, (row.top + row.bottom - sh) / 2, row.right - 14,
                                   (row.top + row.bottom + sh) / 2};
        FillRound(track, sh / 2, WithAlpha(kText, 0.1f * a));
        if (on) FillRound(track, sh / 2, WithAlpha(kGreen, 0.9f * a));
        const float tr = sh / 2 - 4;
        const float tx = track.left + sh / 2 + (sw - sh) * (on ? 1.f : 0.f);
        brush_->SetColor(Rgb(0, 0, 0, 0.3f * a));
        rt_->FillEllipse(D2D1::Ellipse({tx, (track.top + track.bottom) / 2 + 1.f}, tr, tr), brush_.Get());
        brush_->SetColor(WithAlpha(on ? Rgb(10, 20, 14) : kText, a));
        rt_->FillEllipse(D2D1::Ellipse({tx, (track.top + track.bottom) / 2}, tr, tr), brush_.Get());
    };

    const D2D1_RECT_F& ex = kPrivExitRow;
    FillRound(ex, 10.f, WithAlpha(kText, (hover_ == Hit::PrivExit ? 0.06f : 0.03f) * a));
    Text(L"Effacer mes données à la fermeture", fmtSmall_.Get(), {ex.left + 14, ex.top + 5, ex.right - 70, ex.top + 23},
         WithAlpha(kText, a));
    Text(L"désactivé par défaut", fmtSmall_.Get(), {ex.left + 14, ex.top + 21, ex.right - 70, ex.top + 39},
         WithAlpha(kMuted, 0.7f * a));
    drawSwitch(ex, g_cleanOnExit);

    const D2D1_RECT_F& se = kPrivSecureRow;
    FillRound(se, 10.f, WithAlpha(kText, (hover_ == Hit::PrivSecure ? 0.06f : 0.03f) * a));
    Text(L"Effacement sécurisé (réécriture)", fmtSmall_.Get(), {se.left + 14, se.top + 5, se.right - 70, se.top + 23},
         WithAlpha(kText, a));
    Text(L"réécrit le contenu avant suppression", fmtSmall_.Get(), {se.left + 14, se.top + 21, se.right - 70, se.top + 39},
         WithAlpha(kMuted, 0.7f * a));
    drawSwitch(se, g_secureWipe);

    Text(L"Sur SSD, l'écrasement physique n'est pas garanti.", fmtSmall_.Get(),
         {p.left + 20, 504, p.right - 20, 524}, WithAlpha(kMuted, 0.6f * a));

    // Bouton "Nettoyer maintenant" (declenchement manuel).
    const bool chov = hover_ == Hit::PrivClean;
    FillRound(kPrivCleanBtn, 11.f, WithAlpha(kAmber, (chov ? 0.22f : 0.12f) * a));
    StrokeRound(kPrivCleanBtn, 11.f, WithAlpha(kAmber, 0.6f * a));
    Text(L"Nettoyer maintenant", fmtChip_.Get(), kPrivCleanBtn, WithAlpha(Mix(kAmber, kText, chov ? 0.4f : 0.f), a));

    const bool bhov = hover_ == Hit::PrivBack;
    FillRound(kPrivBackBtn, 11.f, WithAlpha(kText, (bhov ? 0.09f : 0.04f) * a));
    StrokeRound(kPrivBackBtn, 11.f, WithAlpha(kText, 0.1f * a));
    Text(L"Fermer", fmtChip_.Get(), kPrivBackBtn, WithAlpha(Mix(kMuted, kText, bhov ? 1.f : 0.6f), a));

    if (!cleanupResult_.empty()) {
        Text(cleanupResult_, fmtBody_.Get(), {p.left + 20, 590, p.right - 20, p.bottom - 8}, WithAlpha(kGreen, a));
    }
}

void App::RunCleanup() {
    const bool secure = g_secureWipe;
    const cleanup::Result r = cleanup::CleanAppData(secure);
    clicker::ResetClickCount();  // donnees en memoire
    clickTimes_.clear();
    lastClickCount_ = 0;

    if (r.deleted == 0 && r.failed == 0) {
        cleanupResult_ = L"Aucune donnée sur disque — mémoire effacée.";
    } else {
        std::wstring msg = std::to_wstring(r.deleted) + L" fichier(s) supprimé(s)";
        if (secure) msg += L" avec réécriture";
        msg += L", mémoire effacée.";
        if (r.failed) {
            msg += L"\n" + std::to_wstring(r.failed) + L" échec : ";
            msg += r.errors.empty() ? L"" : r.errors.front();
        }
        cleanupResult_ = msg;
    }
}

void App::RegisterToggleHotkey() {
    UnregisterHotKey(hwnd_, kHotkeyToggle);
    if (!RegisterHotKey(hwnd_, kHotkeyToggle, MOD_NOREPEAT, UINT(g_toggleVk))) {
        // Touche indisponible (deja prise) : on revient a F8 par defaut.
        g_toggleVk = VK_F8;
        RegisterHotKey(hwnd_, kHotkeyToggle, MOD_NOREPEAT, VK_F8);
    }
}

D2D1_RECT_F App::ProfileRow(int i) const {
    const float top = 230.f + i * 56.f;
    return {kPrivPanel.left + 14.f, top, kPrivPanel.right - 14.f, top + 48.f};
}
D2D1_RECT_F App::ProfileSaveBtn(int i) const {
    const D2D1_RECT_F r = ProfileRow(i);
    return {r.right - 78.f, r.top + 9.f, r.right - 10.f, r.bottom - 9.f};
}
D2D1_RECT_F App::ProfileLoadBtn(int i) const {
    const D2D1_RECT_F s = ProfileSaveBtn(i);
    return {s.left - 74.f, s.top, s.left - 6.f, s.bottom};
}

// Panneau reglages : statistiques de session et profils.
void App::DrawGear() {
    const float a = gearAnim_;
    brush_->SetColor(Rgb(4, 4, 8, 0.72f * a));
    rt_->FillRectangle({0, 0, kW, kH}, brush_.Get());

    const D2D1_RECT_F& p = kPrivPanel;
    FillRound(p, 16.f, WithAlpha(Rgb(16, 15, 26), a));
    StrokeRound(p, 16.f, WithAlpha(kCyan, 0.3f * a));

    Text(L"RÉGLAGES & PROFILS", fmtCard_.Get(), {p.left + 20, 78, p.right - 20, 102}, WithAlpha(kText, a));

    // --- Statistiques de session ---
    Text(L"STATISTIQUES DE SESSION", fmtSmall_.Get(), {p.left + 20, 112, p.right - 20, 130},
         WithAlpha(kCyan, 0.85f * a));
    const D2D1_RECT_F stats = {p.left + 14, 132, p.right - 14, 198};
    FillRound(stats, 10.f, WithAlpha(Rgb(8, 8, 14), a));
    StrokeRound(stats, 10.f, WithAlpha(kText, 0.06f * a));

    const int secs = int(time_);
    wchar_t dur[16];
    swprintf_s(dur, L"%d:%02d", secs / 60, secs % 60);
    const float third = (stats.right - stats.left) / 3.f;
    auto stat = [&](int col, const std::wstring& value, const std::wstring& label, D2D1_COLOR_F col2) {
        const D2D1_RECT_F cell = {stats.left + third * col, stats.top, stats.left + third * (col + 1), stats.bottom};
        Text(value, fmtCard_.Get(), {cell.left, cell.top + 12, cell.right, cell.top + 36}, WithAlpha(col2, a));
        Text(label, fmtLabel_.Get(), {cell.left, cell.bottom - 24, cell.right, cell.bottom - 8}, WithAlpha(kMuted, a));
    };
    stat(0, dur, L"DURÉE", kText);
    stat(1, FormatThousands(clicker::ClickCount()), L"CLICS", kViolet);
    wchar_t mx[16];
    swprintf_s(mx, L"%.1f", maxCps_);
    stat(2, mx, L"CPS MAX", kCyan);

    // --- Profils ---
    Text(L"PROFILS", fmtSmall_.Get(), {p.left + 20, 208, p.right - 20, 226}, WithAlpha(kCyan, 0.85f * a));
    const int curCps = clicker::GetCps();
    const bool curRight = clicker::UsesRightButton();
    for (int i = 0; i < kProfileCount; ++i) {
        const D2D1_RECT_F row = ProfileRow(i);
        const bool rowHov = hover_ == Hit::ProfLoad && hoverProfile_ == i;
        FillRound(row, 10.f, WithAlpha(kText, (rowHov ? 0.06f : 0.03f) * a));
        StrokeRound(row, 10.f, WithAlpha(kText, 0.06f * a));

        const std::wstring name = L"Profil " + std::to_wstring(i + 1);
        Text(name, fmtCard_.Get(), {row.left + 14, row.top + 6, row.left + 120, row.top + 26}, WithAlpha(kText, a));
        std::wstring desc = g_profiles[i].used
                                ? std::to_wstring(g_profiles[i].cps) + L" CPS · " +
                                      (g_profiles[i].right ? L"Droit" : L"Gauche")
                                : L"vide";
        Text(desc, fmtSmall_.Get(), {row.left + 14, row.top + 24, row.left + 160, row.bottom - 4},
             WithAlpha(kMuted, a));

        // Bouton Charger (actif seulement si le profil est rempli).
        const D2D1_RECT_F lb = ProfileLoadBtn(i);
        const bool lhov = hover_ == Hit::ProfLoad && hoverProfile_ == i;
        const float lAlpha = g_profiles[i].used ? 1.f : 0.3f;
        FillRound(lb, 9.f, WithAlpha(kCyan, (lhov ? 0.25f : 0.12f) * lAlpha * a));
        StrokeRound(lb, 9.f, WithAlpha(kCyan, 0.5f * lAlpha * a));
        Text(L"Charger", fmtSmall_.Get(), lb, WithAlpha(Mix(kMuted, kText, lhov ? 1.f : 0.5f), lAlpha * a));

        // Bouton Enregistrer (memorise la config courante).
        const D2D1_RECT_F sb = ProfileSaveBtn(i);
        const bool shov = hover_ == Hit::ProfSave && hoverProfile_ == i;
        const bool isCur = g_profiles[i].used && g_profiles[i].cps == curCps && g_profiles[i].right == curRight;
        FillRound(sb, 9.f, WithAlpha(kViolet, (shov ? 0.3f : 0.15f) * a));
        StrokeRound(sb, 9.f, WithAlpha(kViolet, (isCur ? 0.9f : 0.5f) * a));
        Text(isCur ? L"Actif" : L"Enreg.", fmtSmall_.Get(), sb, WithAlpha(Mix(kMuted, kText, shov ? 1.f : 0.6f), a));
    }

    Text(L"« Enreg. » mémorise le CPS et le bouton actuels.", fmtSmall_.Get(),
         {p.left + 20, 408, p.right - 20, 428}, WithAlpha(kMuted, 0.6f * a));

    const bool bhov = hover_ == Hit::GearBack;
    FillRound(kPrivBackBtn, 11.f, WithAlpha(kText, (bhov ? 0.09f : 0.04f) * a));
    StrokeRound(kPrivBackBtn, 11.f, WithAlpha(kText, 0.1f * a));
    Text(L"Fermer", fmtChip_.Get(), kPrivBackBtn, WithAlpha(Mix(kMuted, kText, bhov ? 1.f : 0.6f), a));
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
Hit App::HitTest(float x, float y, int* chip) const {
    // Quand le panneau confidentialite est ouvert, il capte toute l'interaction.
    if (showPrivacy_) {
        if (InRect(kPrivCleanBtn, x, y)) return Hit::PrivClean;
        if (InRect(kPrivBackBtn, x, y)) return Hit::PrivBack;
        if (InRect(kPrivExitRow, x, y)) return Hit::PrivExit;
        if (InRect(kPrivSecureRow, x, y)) return Hit::PrivSecure;
        if (InRect(kShieldBtn, x, y)) return Hit::Shield;
        if (InRect(kCloseBtn, x, y)) return Hit::Close;
        if (InRect(kMinBtn, x, y)) return Hit::Minimize;
        return Hit::None;
    }
    // Idem pour le panneau reglages / profils.
    if (showGear_) {
        for (int i = 0; i < kProfileCount; ++i) {
            if (InRect(ProfileLoadBtn(i), x, y)) { if (chip) *chip = i; return Hit::ProfLoad; }
            if (InRect(ProfileSaveBtn(i), x, y)) { if (chip) *chip = i; return Hit::ProfSave; }
        }
        if (InRect(kPrivBackBtn, x, y)) return Hit::GearBack;
        if (InRect(kGearBtn, x, y)) return Hit::Gear;
        if (InRect(kCloseBtn, x, y)) return Hit::Close;
        if (InRect(kMinBtn, x, y)) return Hit::Minimize;
        return Hit::None;
    }
    if (InRect(kCloseBtn, x, y)) return Hit::Close;
    if (InRect(kMinBtn, x, y)) return Hit::Minimize;
    if (InRect(kShieldBtn, x, y)) return Hit::Shield;
    if (InRect(kGearBtn, x, y)) return Hit::Gear;
    if (InRect(kBtnSeg, x, y)) return x < (kBtnSeg.left + kBtnSeg.right) / 2.f ? Hit::BtnLeft : Hit::BtnRight;
    if (InRect(kHotkeyChip, x, y)) return Hit::HotkeyChip;
    if (InRect(kToggleCard, x, y)) return Hit::Toggle;
    for (int i = 0; i < int(kPresets.size()); ++i) {
        if (InRect(ChipRect(i), x, y)) {
            if (chip) *chip = i;
            return Hit::Chip;
        }
    }
    const float dx = x - kDialCenter.x, dy = y - kDialCenter.y;
    if (std::sqrt(dx * dx + dy * dy) <= kDialR + 32.f) return Hit::Dial;
    return Hit::None;
}

void App::SetCpsFromPoint(float x, float y) {
    float deg = std::atan2(y - kDialCenter.y, x - kDialCenter.x) * 180.f / kPi;
    if (deg < 0.f) deg += 360.f;
    float off = deg - kArcStart;
    if (off < 0.f) off += 360.f;
    // Zone morte en bas du cadran : on colle a l'extremite la plus proche.
    if (off > kArcSweep) off = off > kArcSweep + (360.f - kArcSweep) / 2.f ? 0.f : kArcSweep;
    const int cps = clicker::kMinCps + int(std::lround(off / kArcSweep * (clicker::kMaxCps - clicker::kMinCps)));
    clicker::SetCps(cps);
}

void App::ToggleEnabled() {
    clicker::SetEnabled(!clicker::IsEnabled());
    SaveSettings();
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<App*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return app ? app->Handle(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == kFrameTimer) Tick();
        return 0;

    case WM_PAINT: {
        Render();
        ValidateRect(hwnd_, nullptr);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_NCCALCSIZE:
        if (wParam) return 0;  // toute la fenetre est zone client
        break;

    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &pt);
        const float x = pt.x / scale_, y = pt.y / scale_;
        if (y < kHeaderH && HitTest(x, y) == Hit::None) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_SIZE:
        if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        return 0;

    case WM_DPICHANGED: {
        scale_ = HIWORD(wParam) / 96.f;
        const auto* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, int(std::lround(kW * scale_)), int(std::lround(kH * scale_)),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (rt_) rt_->SetDpi(96.f * scale_, 96.f * scale_);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const float x = GET_X_LPARAM(lParam) / scale_, y = GET_Y_LPARAM(lParam) / scale_;
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            trackingMouse_ = true;
        }
        if (dragging_) {
            SetCpsFromPoint(x, y);
        } else {
            hoverChip_ = -1;
            hoverProfile_ = -1;
            int idx = -1;
            hover_ = HitTest(x, y, &idx);
            if (hover_ == Hit::Chip) hoverChip_ = idx;
            else if (hover_ == Hit::ProfLoad || hover_ == Hit::ProfSave) hoverProfile_ = idx;
        }
        SetCursor(LoadCursorW(nullptr, (hover_ == Hit::None) ? IDC_ARROW : IDC_HAND));
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        if (!dragging_) hover_ = Hit::None;
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, (hover_ == Hit::None) ? IDC_ARROW : IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        const float x = GET_X_LPARAM(lParam) / scale_, y = GET_Y_LPARAM(lParam) / scale_;
        int chip = -1;
        switch (HitTest(x, y, &chip)) {
        case Hit::Close: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
        case Hit::Minimize: ShowWindow(hwnd_, SW_MINIMIZE); break;
        case Hit::Shield:
            showPrivacy_ = !showPrivacy_;
            if (showPrivacy_) { cleanupResult_.clear(); showGear_ = false; }
            break;
        case Hit::Gear:
            showGear_ = !showGear_;
            if (showGear_) showPrivacy_ = false;
            break;
        case Hit::BtnLeft: clicker::SetRightButton(false); SaveSettings(); break;
        case Hit::BtnRight: clicker::SetRightButton(true); SaveSettings(); break;
        case Hit::Toggle: ToggleEnabled(); break;
        case Hit::HotkeyChip: capturingHotkey_ = true; SetFocus(hwnd_); break;
        case Hit::Chip: clicker::SetCps(kPresets[size_t(chip)]); SaveSettings(); break;
        case Hit::Dial:
            dragging_ = true;
            SetCapture(hwnd_);
            SetCpsFromPoint(x, y);
            break;
        case Hit::PrivExit: g_cleanOnExit = !g_cleanOnExit; SaveSettings(); break;
        case Hit::PrivSecure: g_secureWipe = !g_secureWipe; SaveSettings(); break;
        case Hit::PrivClean: RunCleanup(); break;
        case Hit::PrivBack: showPrivacy_ = false; break;
        case Hit::GearBack: showGear_ = false; break;
        case Hit::ProfLoad:
            if (chip >= 0 && g_profiles[chip].used) {
                clicker::SetCps(g_profiles[chip].cps);
                clicker::SetRightButton(g_profiles[chip].right);
                SaveSettings();
            }
            break;
        case Hit::ProfSave:
            if (chip >= 0) {
                g_profiles[chip] = {true, clicker::GetCps(), clicker::UsesRightButton()};
                SaveSettings();
            }
            break;
        case Hit::None: break;
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
            SaveSettings();
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (showPrivacy_ || showGear_) return 0;  // le cadran est masque par un panneau
        clicker::SetCps(clicker::GetCps() + (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1));
        SaveSettings();
        return 0;

    case WM_KEYDOWN:
        // Capture d'une nouvelle touche de raccourci.
        if (capturingHotkey_) {
            if (wParam != VK_ESCAPE) {
                g_toggleVk = int(wParam);
                RegisterToggleHotkey();
                SaveSettings();
            }
            capturingHotkey_ = false;
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (showPrivacy_) showPrivacy_ = false;  // referme le panneau d'abord
            else if (showGear_) showGear_ = false;
            else PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return 0;
        }
        if (showPrivacy_ || showGear_) break;  // pas de reglage CPS tant qu'un panneau est ouvert
        if (wParam == VK_RIGHT || wParam == VK_UP) clicker::SetCps(clicker::GetCps() + 1);
        else if (wParam == VK_LEFT || wParam == VK_DOWN) clicker::SetCps(clicker::GetCps() - 1);
        else if (wParam == VK_SPACE) ToggleEnabled();
        else break;
        SaveSettings();
        return 0;

    case WM_HOTKEY:
        if (wParam == kHotkeyToggle) ToggleEnabled();
        else if (wParam == kHotkeyQuit) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, kFrameTimer);
        UnregisterHotKey(hwnd_, kHotkeyToggle);
        UnregisterHotKey(hwnd_, kHotkeyQuit);
        SaveSettings();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void ParseArgs() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    std::vector<std::wstring> exes;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--process" && i + 1 < argc) exes.emplace_back(argv[++i]);
    }
    if (!exes.empty()) clicker::SetTargetExes(exes);
    LocalFree(argv);
}

}  // namespace

// Active la conscience DPI sans dependance de chargement : les API recentes
// n'existent pas sur les anciens Windows, un import statique empecherait alors
// l'exe de demarrer. On resout donc la meilleure API disponible a l'execution.
void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto fn = reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
        }
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using SetAwarenessFn = HRESULT(WINAPI*)(int);
        if (auto fn = reinterpret_cast<SetAwarenessFn>(GetProcAddress(shcore, "SetProcessDpiAwareness"))) {
            if (SUCCEEDED(fn(2 /* PROCESS_PER_MONITOR_DPI_AWARE */))) {
                FreeLibrary(shcore);
                return;
            }
        }
        FreeLibrary(shcore);
    }
    SetProcessDPIAware();  // repli universel (Vista+)
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Instance unique : deux copies doubleraient hooks et clics. Si une fenetre
    // existe deja, on la met au premier plan et on quitte.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"LunarAutoClicker_SingleInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"LunarAutoClickerWnd", nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Resolution du timer a 1 ms pour des intervalles de clic precis.
    timeBeginPeriod(1);

    ParseArgs();
    LoadSettings();

    if (!clicker::Start()) {
        MessageBoxW(nullptr, L"Impossible d'installer le hook souris.", L"Lunar AutoClicker", MB_ICONERROR);
        return 1;
    }

    App app;
    if (!app.Create(instance)) {
        clicker::Stop();
        MessageBoxW(nullptr, L"Impossible de créer la fenêtre (Direct2D).", L"Lunar AutoClicker", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    clicker::Stop();

    // Nettoyage a la fermeture, si l'utilisateur l'a active dans Confidentialite.
    // Perimetre strict : le dossier de l'appli uniquement (voir cleanup.cpp).
    if (g_cleanOnExit) cleanup::CleanAppData(g_secureWipe);

    timeEndPeriod(1);
    CoUninitialize();
    if (instanceMutex) {
        ReleaseMutex(instanceMutex);
        CloseHandle(instanceMutex);
    }
    return 0;
}
