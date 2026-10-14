#include "clicker.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <thread>

namespace clicker {
namespace {

std::atomic<bool> g_physicalLeftDown{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_running{false};
std::atomic<bool> g_targetFocused{false};
std::atomic<int> g_cps{12};
std::atomic<uint64_t> g_clicks{0};

std::mutex g_exesMutex;
std::vector<std::wstring> g_targetExes = {L"lunarclient.exe", L"lunar client.exe"};

std::thread g_hookThread;
std::thread g_clickThread;
std::atomic<DWORD> g_hookThreadId{0};

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// Suit l'etat PHYSIQUE du bouton gauche. Les evenements injectes (les notres)
// sont ignores, sinon nos LEFTUP simules seraient pris pour un relachement reel.
LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const bool injected = (info->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0;
        if (!injected) {
            if (wParam == WM_LBUTTONDOWN) g_physicalLeftDown = true;
            else if (wParam == WM_LBUTTONUP) g_physicalLeftDown = false;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

std::wstring ProcessNameForWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return {};

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};

    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        name.assign(path, size);
        const auto slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
    }
    CloseHandle(process);
    return ToLower(name);
}

std::wstring WindowTitle(HWND hwnd) {
    wchar_t title[256];
    const int len = GetWindowTextW(hwnd, title, 256);
    return std::wstring(title, len > 0 ? len : 0);
}

// Le jeu Lunar tourne dans javaw.exe lance par le launcher : on accepte donc
// soit un exe cible, soit un java/javaw dont le titre contient "Lunar Client".
bool IsLunarForeground() {
    static HWND cachedHwnd = nullptr;
    static std::wstring cachedTitle;
    static bool cachedResult = false;

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;

    const std::wstring title = WindowTitle(hwnd);
    if (hwnd == cachedHwnd && title == cachedTitle) return cachedResult;

    const std::wstring exe = ProcessNameForWindow(hwnd);
    bool match;
    {
        std::lock_guard<std::mutex> lock(g_exesMutex);
        match = std::find(g_targetExes.begin(), g_targetExes.end(), exe) != g_targetExes.end();
    }
    if (!match && (exe == L"javaw.exe" || exe == L"java.exe")) {
        match = ToLower(title).find(L"lunar client") != std::wstring::npos;
    }

    cachedHwnd = hwnd;
    cachedTitle = title;
    cachedResult = match;
    return match;
}

void SendLeft(DWORD flag) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    SendInput(1, &input, sizeof(INPUT));
}

// Le hook bas niveau a besoin d'une boucle de messages sur le thread qui l'installe.
void HookLoop(std::atomic<bool>* ready, std::atomic<bool>* ok) {
    g_hookThreadId = GetCurrentThreadId();
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
    *ok = hook != nullptr;
    *ready = true;
    if (!hook) return;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(hook);
}

// Pendant que le bouton est maintenu physiquement, Windows le considere
// "enfonce" ; chaque cycle envoie donc UP puis DOWN, ce qui laisse l'etat final
// coherent avec le bouton reel quand l'utilisateur relache.
void ClickLoop() {
    using clock = std::chrono::steady_clock;
    auto nextClick = clock::now();

    while (g_running) {
        const int cps = g_cps;
        const auto interval = std::chrono::microseconds(1'000'000 / cps);
        const auto upHold = std::chrono::milliseconds(std::min(15, 400 / cps));

        const bool focused = IsLunarForeground();
        g_targetFocused = focused;

        if (!g_enabled || !g_physicalLeftDown || !focused) {
            nextClick = clock::now() + interval;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const auto now = clock::now();
        if (now < nextClick) {
            std::this_thread::sleep_for(std::min<clock::duration>(nextClick - now, std::chrono::milliseconds(5)));
            continue;
        }

        SendLeft(MOUSEEVENTF_LEFTUP);
        std::this_thread::sleep_for(upHold);
        // Si l'utilisateur a relache pendant la pause, on ne renvoie pas DOWN
        // (sinon le bouton resterait bloque enfonce).
        if (g_physicalLeftDown) SendLeft(MOUSEEVENTF_LEFTDOWN);
        ++g_clicks;

        nextClick += interval;
        if (nextClick < clock::now()) nextClick = clock::now() + interval;
    }
}

}  // namespace

bool Start() {
    if (g_running) return true;

    std::atomic<bool> ready{false};
    std::atomic<bool> ok{false};
    g_hookThread = std::thread(HookLoop, &ready, &ok);
    while (!ready) std::this_thread::yield();
    if (!ok) {
        g_hookThread.join();
        return false;
    }

    g_running = true;
    g_clickThread = std::thread(ClickLoop);
    return true;
}

void Stop() {
    if (!g_running) return;
    g_running = false;
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_clickThread.joinable()) g_clickThread.join();
    if (g_hookThread.joinable()) g_hookThread.join();
}

void SetCps(int cps) { g_cps = std::clamp(cps, kMinCps, kMaxCps); }
int GetCps() { return g_cps; }

void SetEnabled(bool enabled) { g_enabled = enabled; }
bool IsEnabled() { return g_enabled; }

void SetTargetExes(const std::vector<std::wstring>& exes) {
    std::lock_guard<std::mutex> lock(g_exesMutex);
    g_targetExes.clear();
    for (const auto& exe : exes) g_targetExes.push_back(ToLower(exe));
}

bool IsHolding() { return g_physicalLeftDown; }
bool IsTargetFocused() { return g_targetFocused; }
uint64_t ClickCount() { return g_clicks; }

}  // namespace clicker
