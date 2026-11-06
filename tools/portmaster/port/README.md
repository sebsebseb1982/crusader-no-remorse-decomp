# Crusader: No Remorse

Reimplementation of the engine of *Crusader: No Remorse* (Origin, 1995) in C++/SDL2.
Source: https://github.com/sebsebseb1982/crusader-no-remorse-decomp

Many thanks to Origin Systems for the original game.

## Installation

The zip contains:

| In the zip | Copy it to (on the SD card holding your ports) | Role |
|---|---|---|
| `Crusader.sh` | `ROMS/Ports/Crusader.sh` (muOS, Knulli, ROCKNIX, ArkOS: your `roms/ports` folder) | Launcher shown in the Ports menu |
| `crusader/` (whole folder) | `ports/crusader/` | Binary, licenses and game data |
| `port.json`, `gameinfo.xml`, `README.md`, `screenshot.png` | nowhere | PortMaster metadata, not needed on the console |

Installing through PortMaster does this for you. Manual install (USB / SD card reader), for example
on a muOS console (RG40XX-H) where the ports live on the SD2 card:

```
SD2/
├── ROMS/Ports/
│   └── Crusader.sh                 <- launcher
└── ports/crusader/
    ├── crusader                    <- binary
    ├── licenses/
    ├── data/                       <- PUT YOUR ORIGINAL GAME FILES HERE
    ├── conf/                       (created on first launch)
    └── log.txt                     (created on first launch)
```

If your ports are on SD1 instead of SD2, use the same layout on SD1. Restart the frontend
if the Crusader entry does not show up in the Ports list.

## Game data

The port contains **no game files**. Copy your own copy of the game to
**`ports/crusader/data/`** (right next to the `crusader` binary), either:

- the installed game files, directly in `data/` (not in a sub-folder):

  ```
  ports/crusader/data/
  ├── STATIC/      USECODE/    SOUND/    FLICS/    IDATA/
  ├── CRUSADER.EXE
  └── ASYLUM.DLL
  ```

- or the CD image: drop the `.iso` file in `data/`; it is extracted on first launch
  (this takes a moment, then the `.iso` is no longer needed).

Supported: French 1.08 and US 1.23. Saves (`SAVEGAME/`) and `CRUSADER.CFG` are written in
`data/` too: keep this folder when updating the port (only replace `crusader/crusader`
and `ROMS/Ports/Crusader.sh`).

If `data/` is empty at launch, the game shows "Game data missing" and exits; see `log.txt`.

## Controls

| Input | Action |
|---|---|
| Left stick | Move / rotate |
| Right stick up / down | Aim |
| Right stick left / right | Roll left / right |
| RT | Fire |
| A / B / X / Y | Select / Jump / Confirm / Next weapon |
| LB / RB | Step left / right |
| L3 / R3 | Shift / Crouch or stand |
| D-pad | Use item / medkit / previous item / next item |
| Start | Menu |
| Hotkey + Start | Quit |

Buttons can be remapped in Options > Enhancements > Gamepad.

## Compiling

`python tools/portmaster/build.py` (Zig cross-compiler), see the repository.
