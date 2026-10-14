# LunarAutoClicker

Autoclicker externe en C++ (API Win32) qui simule des clics gauches uniquement quand :

1. la fenêtre au premier plan est Lunar Client (`LunarClient.exe`, ou `javaw.exe`/`java.exe` dont le titre contient « Lunar Client ») ;
2. le bouton gauche est maintenu physiquement ;
3. l'autoclicker est activé.

## Compilation

```
cmake -S . -B build
cmake --build build --config Release
```

Un workflow GitHub Actions compile aussi l'outil à chaque *push* et publie
automatiquement l'exécutable en Release à chaque tag `v*`.

## Interface

Fenêtre Direct2D sans bordure, sans dépendance externe :

- **Cadran CPS** : 30 segments (1 segment = 1 CPS), réglables en glissant, à la molette ou avec ← →
- **Anneau intérieur** : CPS réellement mesuré
- **Oscilloscope** : chaque clic simulé apparaît en temps réel
- **Préréglages** : 8 / 10 / 12 / 14 / 16 / 20
- **Sélecteur de bouton** : clic gauche ou droit (dans l'en-tête)
- **Raccourci configurable** : clique la pastille de touche dans la carte AUTOCLICK, puis appuie sur la touche voulue
- **Réglages & profils** (icône engrenage) : statistiques de session (durée, clics, CPS max) et 3 profils (CPS + bouton) à enregistrer / charger
- **Indicateurs** : Lunar détecté, bouton maintenu (HOLD)
- Réglages sauvegardés dans `%APPDATA%\LunarAutoClicker\settings.ini`
- Rendu adaptatif 60 fps / 30 fps au repos (faible consommation)

## Confidentialité (bouclier dans l'en-tête)

Option de nettoyage des données créées par l'outil, **désactivée par défaut** :

- liste exacte, avec chemins, de ce qui sera supprimé ;
- périmètre strict : uniquement le dossier `%APPDATA%\LunarAutoClicker` ;
- ne touche **jamais** aux journaux Windows, à l'anti-triche ni aux fichiers d'autres logiciels ;
- effacement à la fermeture et/ou déclenchement manuel (« Nettoyer maintenant ») ;
- option d'effacement sécurisé (réécriture du contenu avant suppression) ;
- erreurs (fichier verrouillé, droits insuffisants) gérées sans planter.

## Utilisation

```
LunarAutoClicker.exe [--process nom.exe]...
```

- **F8** (global) ou **Espace** : activer / désactiver
- **F10** (global) ou **Échap** : quitter

Si Lunar tourne en administrateur, lance aussi l'autoclicker en administrateur.

> La plupart des serveurs PvP (Hypixel, etc.) interdisent les autoclickers.
