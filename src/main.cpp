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

enum class Hit { None, Close, Minimize, Dial, Chip, Toggle };

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

void LoadSettings() {
    const std::wstring path = SettingsPath();
    clicker::SetCps(int(GetPrivateProfileIntW(L"clicker", L"cps", 12, path.c_str())));
    clicker::SetEnabled(GetPrivateProfileIntW(L"clicker", L"enabled", 1, path.c_str()) != 0);
}

void SaveSettings() {
    const std::wstring path = SettingsPath();
    WritePrivateProfileStringW(L"clicker", L"cps", std::to_wstring(clicker::GetCps()).c_str(), path.c_str());
    WritePrivateProfileStringW(L"clicker", L"enabled", clicker::IsEnabled() ? L"1" : L"0", path.c_str());
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

    ComPtr<IDWriteTextFormat> fmtLogo_, fmtLogoThin_, fmtBig_, fmtLabel_, fmtSmall_, fmtSmallCenter_,
        fmtChip_, fmtCard_, fmtIcon_;

    // Etat d'interaction
    Hit hover_ = Hit::None;
    int hoverChip_ = -1;
    bool dragging_ = false;
    bool trackingMouse_ = false;

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

    RegisterHotKey(hwnd_, kHotkeyToggle, MOD_NOREPEAT, VK_F8);
    RegisterHotKey(hwnd_, kHotkeyQuit, MOD_NOREPEAT, VK_F10);

    displayCps_ = float(clicker::GetCps());
    enabledAnim_ = clicker::IsEnabled() ? 1.f : 0.f;
    lastClickCount_ = clicker::ClickCount();

    QueryPerformanceFrequency(&freq_);
    QueryPerformanceCounter(&last_);
    SetTimer(hwnd_, kFrameTimer, 8, nullptr);

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
    return fmtLogo_ && fmtBig_ && fmtIcon_;
}

ComPtr<IDWriteTextFormat> App::MakeFormat(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI Variable Display", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
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

    ComPtr<ID2D1StrokeStyle> style;
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
                            style.GetAddressOf());
    brush_->SetColor(color);
    rt_->DrawGeometry(path.Get(), brush_.Get(), width, style.Get());
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

    ComPtr<ID2D1StrokeStyle> round;
    d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
                            round.GetAddressOf());

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
            rt_->DrawLine(p0, p1, brush_.Get(), 13.f, round.Get());
        }
        brush_->SetColor(Mix(WithAlpha(kText, 0.08f), WithAlpha(col, dim), lit));
        rt_->DrawLine(p0, p1, brush_.Get(), 5.f, round.Get());
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
    const std::wstring sub = L"F8 basculer  ·  " + FormatThousands(clicker::ClickCount()) + L" clics";
    Text(sub, fmtSmall_.Get(), {r.left + 18.f, r.top + 26.f, r.right - 90.f, r.top + 46.f}, WithAlpha(kMuted, 0.75f));

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

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
Hit App::HitTest(float x, float y, int* chip) const {
    if (InRect(kCloseBtn, x, y)) return Hit::Close;
    if (InRect(kMinBtn, x, y)) return Hit::Minimize;
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
            hover_ = HitTest(x, y, &hoverChip_);
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
        case Hit::Toggle: ToggleEnabled(); break;
        case Hit::Chip: clicker::SetCps(kPresets[size_t(chip)]); SaveSettings(); break;
        case Hit::Dial:
            dragging_ = true;
            SetCapture(hwnd_);
            SetCpsFromPoint(x, y);
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
        clicker::SetCps(clicker::GetCps() + (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1));
        SaveSettings();
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_RIGHT || wParam == VK_UP) clicker::SetCps(clicker::GetCps() + 1);
        else if (wParam == VK_LEFT || wParam == VK_DOWN) clicker::SetCps(clicker::GetCps() - 1);
        else if (wParam == VK_SPACE) ToggleEnabled();
        else if (wParam == VK_ESCAPE) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
