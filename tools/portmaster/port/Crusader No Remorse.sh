#!/bin/bash
# PORTMASTER: crusader-no-remorse.zip, Crusader No Remorse.sh

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR=/$directory/ports/crusader-no-remorse
CONFDIR="$GAMEDIR/conf"

mkdir -p "$GAMEDIR/data" "$CONFDIR"
cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# The game reads ./data (STATIC, USECODE, SOUND, FLICS... or the CD .iso, which it
# extracts on first launch) and writes its saves and CRUSADER.CFG there.
if [ ! -d "$GAMEDIR/data/STATIC" ] && ! ls "$GAMEDIR"/data/*.[iI][sS][oO] >/dev/null 2>&1; then
  echo "Game data missing: copy the files of Crusader: No Remorse (or the .iso) to $GAMEDIR/data"
  # Older versions lived in ports/crusader: their data and saves move here.
  [ -d "/$directory/ports/crusader/data" ] && echo "An older version kept them in /$directory/ports/crusader/data: move that folder's contents to $GAMEDIR/data"
  if command -v pm_message >/dev/null 2>&1; then
    pm_message "Game data missing: put Crusader: No Remorse files (or the .iso) in ports/crusader-no-remorse/data"
    sleep 5
  fi
  pm_finish
  exit 1
fi

chmod +x "$GAMEDIR/crusader-no-remorse"

# The game reads the controller itself (SDL_GameController, Select + Start quits);
# gptokeyb is not used: its default key mapping would double every input.
# CRUSADER_PAD_DEBUG=1 logs the controller and its inputs in log.txt.
# No keyboard to type a save's name: --auto-save-names proposes the date and
# time, confirmed with A.
pm_platform_helper "$GAMEDIR/crusader-no-remorse"
./crusader-no-remorse --fullscreen --auto-save-names data

pm_finish
