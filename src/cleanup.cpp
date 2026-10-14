#include "cleanup.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>

#include <algorithm>

#pragma comment(lib, "bcrypt.lib")

namespace cleanup {
namespace {

const wchar_t* kFolderName = L"LunarAutoClicker";

std::wstring Join(const std::wstring& a, const std::wstring& b) { return a + L"\\" + b; }

std::wstring ErrorText(DWORD code) {
    switch (code) {
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return L"fichier verrouillé (en cours d'utilisation)";
    case ERROR_ACCESS_DENIED:
        return L"droits insuffisants";
    case ERROR_FILE_NOT_FOUND:
        return L"déjà absent";
    default:
        return L"erreur " + std::to_wstring(code);
    }
}

// Remplit le buffer d'octets aleatoires (repli deterministe si l'API echoue :
// la qualite cryptographique n'est pas requise pour une simple reecriture).
void FillRandom(unsigned char* buf, size_t len) {
    if (BCryptGenRandom(nullptr, buf, ULONG(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return;
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<unsigned char>(0xA5 ^ (i * 31 + 7));
}

// Reecrit tout le contenu du fichier avant sa suppression. Note : sur SSD, le
// nivellement d'usure ne garantit pas l'ecrasement physique du bloc d'origine ;
// c'est une precaution, pas une garantie materielle.
bool SecureOverwrite(const std::wstring& path, uint64_t size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    unsigned char buf[4096];
    uint64_t written = 0;
    bool ok = true;
    while (written < size) {
        const DWORD chunk = DWORD(std::min<uint64_t>(sizeof(buf), size - written));
        FillRandom(buf, chunk);
        DWORD wrote = 0;
        if (!WriteFile(f, buf, chunk, &wrote, nullptr) || wrote != chunk) {
            ok = false;
            break;
        }
        written += wrote;
    }
    FlushFileBuffers(f);
    CloseHandle(f);
    return ok;
}

}  // namespace

std::wstring DataDir() {
    wchar_t* appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = Join(appData, kFolderName);
    }
    CoTaskMemFree(appData);
    return dir;
}

std::vector<Item> ListAppData() {
    std::vector<Item> items;
    const std::wstring dir = DataDir();
    if (dir.empty()) return items;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return items;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  // fichiers seulement
        Item it;
        it.path = Join(dir, fd.cFileName);
        it.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        items.push_back(std::move(it));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return items;
}

Result CleanAppData(bool secure) {
    Result r;
    const std::wstring dir = DataDir();

    // Verrou de securite : ne jamais agir si le chemin ne correspond pas
    // exactement au dossier de l'appli.
    if (dir.empty() || dir.size() < wcslen(kFolderName) ||
        _wcsicmp(dir.c_str() + dir.size() - wcslen(kFolderName), kFolderName) != 0) {
        return r;
    }

    for (const auto& it : ListAppData()) {
        SetFileAttributesW(it.path.c_str(), FILE_ATTRIBUTE_NORMAL);  // lever un eventuel read-only
        const bool wiped = (secure && it.size > 0) ? SecureOverwrite(it.path, it.size) : true;

        if (DeleteFileW(it.path.c_str())) {
            ++r.deleted;
            if (secure && !wiped)
                r.errors.push_back(it.path + L" : réécriture incomplète (fichier tout de même supprimé)");
        } else {
            ++r.failed;
            r.errors.push_back(it.path + L" : " + ErrorText(GetLastError()));
        }
    }

    // Retire le dossier s'il est devenu vide (echec ignore s'il ne l'est pas).
    RemoveDirectoryW(dir.c_str());
    return r;
}

}  // namespace cleanup
