// LunarAutoClicker - autoclicker externe pour Lunar Client.
// Simule des clics gauches uniquement quand :
//   1) la fenetre au premier plan appartient a Lunar Client
//   2) le bouton gauche est physiquement maintenu
//   3) l'autoclicker est active (touche F8)
//
// Usage : LunarAutoClicker.exe [--cps N] [--process nom.exe]...

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {

constexpr int kToggleKey = VK_F8;
constexpr int kQuitKey = VK_F10;
constexpr int kMinCps = 1;
constexpr int kMaxCps = 30;

std::atomic<bool> g_physicalLeftDown{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_running{true};
DWORD g_mainThreadId = 0;

int g_cps = 12;
std::vector<std::wstring> g_targetExes = {L"lunarclient.exe", L"lunar client.exe"};

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// Hook souris bas niveau : suit l'etat PHYSIQUE du bouton gauche.
// Les evenements injectes (les notres) sont ignores, sinon nos propres
// LEFTUP simules seraient pris pour un relachement reel.
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
    bool match = std::find(g_targetExes.begin(), g_targetExes.end(), exe) != g_targetExes.end();
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

void PrintStatus() {
    std::printf("\r[%s] %d CPS   (F8 = on/off, F10 = quitter)      ",
                g_enabled ? "ACTIF  " : "INACTIF", g_cps);
    std::fflush(stdout);
}

// Thread de clic. Pendant que le bouton est maintenu physiquement, Windows
// considere le bouton "enfonce" ; chaque cycle envoie donc UP puis DOWN, ce qui
// laisse l'etat final coherent avec le bouton reel quand l'utilisateur relache.
void ClickLoop() {
    using clock = std::chrono::steady_clock;
    const auto interval = std::chrono::microseconds(1'000'000 / g_cps);
    const auto upHold = std::chrono::milliseconds(std::min(15, 400 / g_cps));

    bool toggleWasDown = false;
    auto nextClick = clock::now();

    while (g_running) {
        const bool toggleDown = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
        if (toggleDown && !toggleWasDown) {
            g_enabled = !g_enabled;
            PrintStatus();
        }
        toggleWasDown = toggleDown;

        if ((GetAsyncKeyState(kQuitKey) & 0x8000) != 0) {
            g_running = false;
            PostThreadMessageW(g_mainThreadId, WM_QUIT, 0, 0);
            break;
        }

        if (!g_enabled || !g_physicalLeftDown || !IsLunarForeground()) {
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

        nextClick += interval;
        if (nextClick < clock::now()) nextClick = clock::now() + interval;
    }
}

void ParseArgs(int argc, wchar_t** argv) {
    bool customExes = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--cps" && i + 1 < argc) {
            g_cps = std::clamp(_wtoi(argv[++i]), kMinCps, kMaxCps);
        } else if (arg == L"--process" && i + 1 < argc) {
            if (!customExes) {
                g_targetExes.clear();
                customExes = true;
            }
            g_targetExes.push_back(ToLower(argv[++i]));
        } else if (arg == L"--help" || arg == L"-h") {
            std::wprintf(L"Usage: LunarAutoClicker.exe [--cps N (%d-%d)] [--process nom.exe]...\n", kMinCps, kMaxCps);
            std::exit(0);
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ParseArgs(argc, argv);
    g_mainThreadId = GetCurrentThreadId();

    // Resolution du timer a 1 ms pour des intervalles de clic precis.
    timeBeginPeriod(1);

    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
    if (!hook) {
        std::fprintf(stderr, "Impossible d'installer le hook souris (erreur %lu)\n", GetLastError());
        timeEndPeriod(1);
        return 1;
    }

    std::printf("LunarAutoClicker - maintiens le clic gauche dans Lunar Client.\n");
    PrintStatus();

    std::thread clicker(ClickLoop);

    // Le hook bas niveau a besoin d'une boucle de messages sur le thread qui l'a installe.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    clicker.join();
    UnhookWindowsHookEx(hook);
    timeEndPeriod(1);
    std::printf("\nArret.\n");
    return 0;
}
