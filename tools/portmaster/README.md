# PortMaster port (aarch64 consoles: RG40XX-H, etc.)

Builds `crusader` for Linux aarch64 (glibc ≥ 2.28) from Windows or Linux, with Zig as a cross compiler. The binary uses the console's system SDL2 (muOS, Knulli, ROCKNIX, ArkOS…); libc++ is linked statically.

## Building

```
pip install ziglang ninja
python tools/portmaster/build.py          # -> build-portmaster/crusader
```

`build.py` fetches the SDL2 headers, makes an empty `libSDL2-2.0.so.0` (symbols only) for linking, then runs CMake with `zig-aarch64.cmake`.

## Installing on the console

The release zip follows the PortMaster structure (installed automatically by PortMaster). Manually (USB or card reader), the contents of the zip are laid out as follows:

| In the zip | Copy it to (on the SD card holding your ports) | Role |
|---|---|---|
| `Crusader.sh` | `ROMS/Ports/Crusader.sh` | launcher, shown in the Ports menu |
| `crusader/` (whole folder) | `ports/crusader/` | binary, licenses, game data |
| `port.json`, `gameinfo.xml`, `README.md`, `screenshot.png` | nowhere | PortMaster metadata, not needed on the console |

Example on the RG40XX-H running muOS (ports are on the **SD2** card; visible over USB/MTP under "This PC\RG40XX-H\SD2"):

```
SD2/
├── ROMS/Ports/
│   └── Crusader.sh                 launcher
└── ports/crusader/
    ├── crusader                    binary
    ├── licenses/                   licenses
    ├── data/                       HERE: the original game data
    ├── conf/                       created on first launch
    └── log.txt                     created on first launch
```

If your ports are on the SD1 card, use the same layout on SD1. Restart the front-end if the Crusader entry does not show up in the Ports list.

### Original game data

You must supply it yourself (no game file is shipped) and put it in **`ports/crusader/data/`**, next to the `crusader` binary, as either:

- the files of the installed game, directly in `data/` (no subfolder):

  ```
  ports/crusader/data/
  ├── STATIC/   USECODE/   SOUND/   FLICS/   IDATA/
  ├── CRUSADER.EXE
  └── ASYLUM.DLL
  ```

- or the CD image: the `.iso` file in `data/` (extracted on first launch, then no longer needed).

Supported versions: French 1.08 and US 1.23. The game writes its saves (`SAVEGAME/`) and `CRUSADER.CFG` in `data/`: when updating, keep that folder (only replace `crusader/crusader` and `ROMS/Ports/Crusader.sh`, which `scripts/update-rg40xx.ps1` does). If `data/` is empty, the launcher shows "Game data missing" and exits (see `log.txt`).

The port's sources are in `tools/portmaster/port/` (`port.json`, `README.md`, `gameinfo.xml`, launcher). A `screenshot.png` (4:3, ≥ 640×480, taken in game) must be added to that folder before the PortMaster PR.

## Gamepad

The game reads the gamepad itself (SDL_GameController): `gptokeyb` is not used (its default keys doubled every input: RT also sent End, which moves backward). Select + Start quits the game. `CRUSADER_PAD_DEBUG=1` logs the gamepad to `log.txt`. The defaults are those of the "CONTROLLER" options screen, where each action can be reassigned (written to `data/CRUSADER.CFG`, keys `padmap`, `padaim`, `paddeadzone`):

| Input | Action |
|---|---|
| Left stick | arrows (movement, turning) |
| Right stick up / down | aim (mouse) |
| Right stick left / right | roll left / right (Ctrl + left / right) |
| RT | fire |
| A / B / X / Y | select (S) / jump / confirm / next weapon |
| LB / RB | step left / right (Alt + left / right) |
| L3 / R3 | Shift / crouch or stand up (Ctrl + down) |
| D-pad | use item / medkit / previous item / next item |
| Start | menu |

In menus: D-pad = arrows, right stick = mouse, A = Enter, B and Start = Esc.
