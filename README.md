# MouseMover

MouseMover is a small native Windows tray app that moves the mouse for a configured duration after a configured rest interval.

## Build

```powershell
.\build.ps1 -Configuration Release
```

The built executable is written to:

```text
bin\x64\Release\MouseMover.exe
```

## Run

Launch `MouseMover.exe` to open the settings window and tray icon. Closing or minimizing the window keeps the app in the tray. Use the tray menu or the main window to start, stop, run one movement cycle immediately, or exit.

Settings are saved in:

```text
%APPDATA%\MouseMover\settings.ini
```

## Self-test

```powershell
bin\x64\Release\MouseMover.exe --self-test
```

The self-test validates settings persistence, clamping, path generation, and scheduler math without moving the mouse.

