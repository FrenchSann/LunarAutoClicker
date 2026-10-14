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

## Utilisation

```
LunarAutoClicker.exe [--cps N (1-30)] [--process nom.exe]...
```

- **F8** : activer / désactiver
- **F10** : quitter

Si Lunar tourne en administrateur, lance aussi l'autoclicker en administrateur.

> La plupart des serveurs PvP (Hypixel, etc.) interdisent les autoclickers.
