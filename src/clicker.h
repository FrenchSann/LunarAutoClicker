#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Moteur de clic : hook souris bas niveau + thread de clic.
// Thread-safe : toutes les fonctions peuvent etre appelees depuis l'UI.
namespace clicker {

constexpr int kMinCps = 1;
constexpr int kMaxCps = 30;

bool Start();
void Stop();

void SetCps(int cps);
int GetCps();

void SetEnabled(bool enabled);
bool IsEnabled();

// Remplace la liste des exe cibles (noms de fichier, insensibles a la casse).
void SetTargetExes(const std::vector<std::wstring>& exes);

bool IsHolding();        // bouton gauche maintenu physiquement
bool IsTargetFocused();  // Lunar Client au premier plan
uint64_t ClickCount();   // nombre total de clics simules

}  // namespace clicker
