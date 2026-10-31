BBFSS - BloodborneFSS (Powered by Lantern Engine)

This is an unofficial modified build of:
Lantern Engine by Awaken (https://github.com/Awakenchan/LanternEngine)
Fork: https://github.com/fromsoftserve/BBFSS

GETTING STARTED
1. Keep this entire folder together in a writable location, such as C:\Games\BBFSS.
2. Run BBFSS.exe. The launcher includes .NET; no .NET SDK, Python, MSYS2 or
   development tools are required.
3. Choose your own legally obtained Bloodborne game folder containing eboot.bin.
   The supported enhanced rendering targets Bloodborne 1.09. Game data, DLC,
   saves, accounts and prepared game images are NOT included.
4. Choose your display and controller settings in the launcher, then play.
5. Press Insert or L3+R3 for the in-game graphics menu.

Requires 64-bit Windows 10/11 and a compatible Vulkan GPU with up-to-date drivers.
The Vulkan loader comes from the graphics driver. DLSS requires supported NVIDIA
hardware. FSR 3.1 is selected initially for broader GPU compatibility.
Optional upscalers are available only when their required assets and hardware
are present. See README.md for rendering features and their requirements.
Directional and local shadows start at their highest currently implemented
quality. Lower them in the in-game menu if GPU memory or performance is limited;
shadow resolution changes require restarting Bloodborne.
Picture tuning and effect intensities start with the fork's calibrated settings;
the game's native depth of field remains the default. Upscaling defaults to FSR 3.1,
not the developer's hardware-specific DLSS selection.
The development setup's Performance Patch behavior is built into the native start:
renderer defaults (including its native visual changes), profiling bypasses and
shorter worker waits. Its light-grid tile size follows the selected output/window
resolution automatically. The old Performance Patch and resolution-specific Light
Grid toggles are no longer needed or offered. Existing saved selections are ignored.

SHARING
Give the recipient the WHOLE folder, not only BBFSS.exe.
The executable is unsigned; Windows SmartScreen may show a reputation warning.
Use trusted copies and SHA256SUMS.txt to check file integrity.
This fork's distribution build does not check for updates by default, and its
update source is the BBFSS fork, not upstream Lantern Engine.
Launcher preferences use the existing per-user Lantern Engine configuration
location. On a PC already using Lantern Engine, select this package's engine
folder in Settings if the launcher points to a different installation.

LICENSES AND SOURCE
This is free software distributed WITHOUT WARRANTY. Keep LICENSE, NOTICE and
engine\licenses with the build. BBFSS-source.zip contains the corresponding
checked-out project source, including the distribution build configuration.
Native dependencies and their license notices are included separately.
To rebuild the launcher: dotnet publish
launcher\windows\BloodborneLauncher\BloodborneLauncher.csproj -c Release
-r win-x64 --self-contained true -p:BBFSSDistribution=true
-p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true
To rebuild the native engine, follow engine\README.md and engine\build.sh.
