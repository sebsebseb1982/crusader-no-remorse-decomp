# PortMaster port (aarch64 consoles: RG40XX-H, etc.)

Builds `crusader-no-remorse` for Linux aarch64 (glibc ≥ 2.28) from Windows or Linux, with Zig as a cross compiler. The binary uses the console's system SDL2 (muOS, Knulli, ROCKNIX, ArkOS…); libc++ is linked statically.

## Building

```
pip install ziglang ninja
python tools/portmaster/build.py          # -> build-portmaster/crusader-no-remorse
```

`build.py` fetches the SDL2 headers, makes an empty `libSDL2-2.0.so.0` (symbols only) for linking, then runs CMake with `zig-aarch64.cmake`.

## Installing on the console

The release zip follows the PortMaster structure (installed automatically by PortMaster). Manually (USB or card reader), the contents of the zip are laid out as follows:

| In the zip | Copy it to (on the SD card holding your ports) | Role |
|---|---|---|
| `Crusader No Remorse.sh` | `ROMS/Ports/Crusader No Remorse.sh` | launcher, shown in the Ports menu |
| `crusader-no-remorse/` (whole folder) | `ports/crusader-no-remorse/` | binary, licenses, artwork, game data |
| `port.json`, `gameinfo.xml`, `README.md`, `cover.png`, `screenshot.png` | nowhere | PortMaster metadata, not needed on the console |

Example on the RG40XX-H running muOS (ports are on the **SD2** card; visible over USB/MTP under "This PC\RG40XX-H\SD2"):

```
SD2/
├── ROMS/Ports/
│   └── Crusader No Remorse.sh      launcher
└── ports/crusader-no-remorse/
    ├── crusader-no-remorse         binary
    ├── licenses/                   licenses
    ├── data/                       HERE: the original game data
    ├── conf/                       created on first launch
    └── log.txt                     created on first launch
```

If your ports are on the SD1 card, use the same layout on SD1. Restart the front-end if the Crusader No Remorse entry does not show up in the Ports list.

### Original game data

You must supply it yourself (no game file is shipped) and put it in **`ports/crusader-no-remorse/data/`**, next to the `crusader-no-remorse` binary, as either:

- the files of the installed game, directly in `data/` (no subfolder):

  ```
  ports/crusader-no-remorse/data/
  ├── STATIC/   USECODE/   SOUND/   FLICS/   IDATA/
  ├── CRUSADER.EXE
  └── ASYLUM.DLL
  ```

- or the CD image: the `.iso` file in `data/` (extracted on first launch, then no longer needed).

Supported versions: French 1.08 and US 1.23. The game writes its saves (`SAVEGAME/`) and `CRUSADER.CFG` in `data/`: when updating, keep that folder (only replace `crusader-no-remorse/crusader-no-remorse` and `ROMS/Ports/Crusader No Remorse.sh`). `scripts/update-rg40xx.ps1` does that and also copies the PC's `data/SAVEGAME/SAVEGAME.<n>` over the console's (`-SkipSaves` to keep the console's saves). If `data/` is empty, the launcher shows "Game data missing" and exits (see `log.txt`). Older versions installed to `ports/crusader/` with `Crusader.sh`: rename that folder to `ports/crusader-no-remorse/` and delete the old `crusader` binary and launcher (the PortMaster folder names the episode so that the No Regret port can sit next to it).

The port's sources are in `tools/portmaster/port/` (`port.json`, `README.md`, `gameinfo.xml`, launcher, `cover.png`, `screenshot.png`).

### Artwork

- `screenshot.png`: 640×480 in-game capture at native scale (what the console shows).
- `cover.png`: 400×300 on a transparent background (gameplay + cover art), the format of PortMaster covers.

Both are generated from `docs/images/` by `python tools/portmaster/make_art.py . <x> <y> tools/portmaster/port`, where `<x> <y>` is the centre of the crop in `docs/images/screenshot.png` (1720 700: the Silencer).

The muOS front-end does not read them from the port: it shows the images of its catalogue, named after the launcher (`MUOS/info/catalogue/External - Ports/box/Crusader No Remorse.png` for the list, `preview/` for the preview). PortMaster copies `<image>` of `gameinfo.xml` (`./crusader-no-remorse/cover.png`, hence the copy in the port folder) and the `screenshot.png` beside it there. After a manual install, `scripts/update-rg40xx.ps1` copies them there itself.

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

Saving: there is no keyboard to type a name, so the launcher passes `--auto-save-names`: choosing a slot on the Save screen proposes the date and time (`12/11/2026 14:32`), A saves under it, B cancels.
