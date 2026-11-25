@echo off
setlocal

set "GAME_DIR=%~dp0data"
if not exist "%GAME_DIR%\CRUSADER.EXE" (
    echo No Remorse was not found in "%GAME_DIR%".
    echo Copy the game files into the data folder first.
    pause
    exit /b 1
)

set "ISO_NAME=CrusaderNoRemorseFR108.iso"
set "ISO_DIR=%~dp0isos"
if not exist "%ISO_DIR%\%ISO_NAME%" (
    echo French 1.08 No Remorse ISO was not found in "%ISO_DIR%".
    pause
    exit /b 1
)
set "ISO_FILE=%ISO_DIR%\%ISO_NAME%"

set "DOSBOX=%ProgramFiles(x86)%\DOSBox-0.74-3\DOSBox.exe"
if not exist "%DOSBOX%" set "DOSBOX=%ProgramFiles%\DOSBox-0.74-3\DOSBox.exe"
if not exist "%DOSBOX%" set "DOSBOX=%ProgramFiles%\DOSBox\DOSBox.exe"
if not exist "%DOSBOX%" (
    where.exe DOSBox.exe >nul 2>nul
    if errorlevel 1 (
        echo DOSBox was not found. Install DOSBox or add DOSBox.exe to PATH.
        pause
        exit /b 1
    )
    set "DOSBOX=DOSBox.exe"
)

start "" /D "%GAME_DIR%" "%DOSBOX%" -c "mount c %GAME_DIR%" -c "imgmount e %ISO_FILE% -t iso" -c "c:" -c "CRUSADER.EXE"
