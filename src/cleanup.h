#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Nettoyage des donnees locales creees par l'application, et UNIQUEMENT
// celles-ci. Perimetre strict : le dossier %APPDATA%\LunarAutoClicker.
// Ne touche jamais aux journaux systeme ni aux fichiers d'autres logiciels.
namespace cleanup {

struct Item {
    std::wstring path;
    uint64_t size = 0;
};

struct Result {
    int deleted = 0;
    int failed = 0;
    std::vector<std::wstring> errors;  // "chemin : raison"
};

// Dossier de donnees de l'appli. Chaine vide si introuvable.
std::wstring DataDir();

// Fichiers presents dans le dossier de l'appli (avec leur taille).
std::vector<Item> ListAppData();

// Supprime les fichiers du dossier de l'appli, puis le dossier s'il est vide.
// secure = true : reecriture du contenu avant suppression.
// N'opere JAMAIS en dehors du dossier de l'appli ; les erreurs sont collectees
// sans interrompre le traitement.
Result CleanAppData(bool secure);

}  // namespace cleanup
