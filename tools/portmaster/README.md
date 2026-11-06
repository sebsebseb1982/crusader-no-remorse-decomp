# Portage PortMaster (consoles aarch64 : RG40XX-H, etc.)

Compile `crusader` pour Linux aarch64 (glibc ≥ 2.28) depuis Windows ou Linux, avec Zig comme compilateur croisé. Le binaire utilise le SDL2 du système de la console (muOS, Knulli, ROCKNIX, ArkOS…) ; libc++ est liée statiquement.

## Compiler

```
pip install ziglang ninja
python tools/portmaster/build.py          # -> build-portmaster/crusader
```

`build.py` récupère les en-têtes SDL2, fabrique une `libSDL2-2.0.so.0` vide (symboles seulement) pour l'édition de liens, puis lance CMake avec `zig-aarch64.cmake`.

## Installer sur la console

Le zip de release suit la structure PortMaster (installé automatiquement par PortMaster). En manuel (USB ou lecteur de carte), le contenu du zip se répartit ainsi :

| Dans le zip | À copier vers (carte SD des ports) | Rôle |
|---|---|---|
| `Crusader.sh` | `ROMS/Ports/Crusader.sh` | lanceur, affiché dans le menu Ports |
| `crusader/` (dossier entier) | `ports/crusader/` | binaire, licences, données du jeu |
| `port.json`, `gameinfo.xml`, `README.md`, `screenshot.png` | nulle part | métadonnées PortMaster, inutiles sur la console |

Exemple sur la RG40XX-H sous muOS (les ports sont sur la carte **SD2** ; visible en USB/MTP sous « Ce PC\RG40XX-H\SD2 ») :

```
SD2/
├── ROMS/Ports/
│   └── Crusader.sh                 lanceur
└── ports/crusader/
    ├── crusader                    binaire
    ├── licenses/                   licences
    ├── data/                       ICI : données du jeu d'origine
    ├── conf/                       créé au premier lancement
    └── log.txt                     créé au premier lancement
```

Si les ports sont sur la carte SD1, même arborescence sur SD1. Redémarrer l'interface si l'entrée Crusader n'apparaît pas dans la liste des Ports.

### Données du jeu d'origine

Elles sont à fournir soi-même (aucun fichier du jeu n'est livré) et se déposent dans **`ports/crusader/data/`**, à côté du binaire `crusader`, soit :

- les fichiers du jeu installé, directement dans `data/` (sans sous-dossier) :

  ```
  ports/crusader/data/
  ├── STATIC/   USECODE/   SOUND/   FLICS/   IDATA/
  ├── CRUSADER.EXE
  └── ASYLUM.DLL
  ```

- ou l'image du CD : le fichier `.iso` dans `data/` (extrait au premier lancement, puis plus nécessaire).

Versions gérées : française 1.08 et US 1.23. Le jeu écrit ses sauvegardes (`SAVEGAME/`) et `CRUSADER.CFG` dans `data/` : lors d'une mise à jour, conserver ce dossier (ne remplacer que `crusader/crusader` et `ROMS/Ports/Crusader.sh`, ce que fait `scripts/update-rg40xx.ps1`). Si `data/` est vide, le lanceur affiche « Game data missing » et quitte (voir `log.txt`).

Les sources du port sont dans `tools/portmaster/port/` (`port.json`, `README.md`, `gameinfo.xml`, lanceur). Un `screenshot.png` (4:3, ≥ 640×480, pris en jeu) est à ajouter dans ce dossier avant la PR PortMaster.

## Manette

Le jeu lit la manette lui-même (SDL_GameController) : `gptokeyb` n'est pas utilisé (ses touches par défaut doublaient chaque entrée : RT envoyait aussi Fin, qui fait reculer). Select + Start quitte le jeu. `CRUSADER_PAD_DEBUG=1` journalise la manette dans `log.txt`. Les réglages par défaut sont ceux de l'écran « MANETTE » des options, où chaque action se réassigne (écrits dans `data/CRUSADER.CFG`, clés `padmap`, `padaim`, `paddeadzone`) :

| Entrée | Action |
|---|---|
| Stick gauche | flèches (déplacement, rotation) |
| Stick droit haut / bas | visée (souris) |
| Stick droit gauche / droite | roulade gauche / droite (Ctrl + gauche / droite) |
| RT | tir |
| A / B / X / Y | sélectionner (S) / saut / valider / arme suivante |
| LB / RB | pas à gauche / à droite (Alt + gauche / droite) |
| L3 / R3 | Maj / se baisser ou se lever (Ctrl + bas) |
| Croix | utiliser l'objet / trousse / objet précédent / objet suivant |
| Start | menu |

Dans les menus : croix = flèches, stick droit = souris, A = Entrée, B et Start = Échap.
