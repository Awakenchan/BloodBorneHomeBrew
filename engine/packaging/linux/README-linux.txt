Lantern Engine for Linux (x86-64)

Needs: a Vulkan 1.3 GPU driver (Mesa RADV for AMD, NVIDIA's own driver, Intel ANV), glibc 2.38 or
later (Ubuntu 24.04, Debian 13, Fedora 39, Arch, SteamOS 3.6, Bazzite), Python 3, X11 or Wayland.

Start: open "Lantern Engine" (the launcher, the same as on Windows): pick the game folder (the
one with eboot.bin and sce_module), then Play. If your file manager does not run it, run
    chmod +x "Lantern Engine" lantern.sh engine/out/bb-start
once in this folder.

From a terminal instead:
    ./lantern.sh /path/to/CUSA03173
The folder is remembered, so later starts need only ./lantern.sh. A CUSA03173 folder next to
this one, or inside it, is found without the argument.

Shaders: the first start after an update recompiles the shader cache, and the shipped shader
catalog is added to it once, so most pipelines are built before the game starts (a GPU from
another vendor than the catalog's still builds its own as it plays).

Wayland: the game runs natively. If the window closes at the start with "Wayland display
connection closed by server" in the log, the compositor ended the connection: a driver and
compositor mismatch (NVIDIA 615 drivers on KDE Plasma did; 580 ran clean). Another driver
version, or SDL_VIDEODRIVER=x11 before ./lantern.sh (XWayland), avoids it. KDE Plasma 6.8.1
flickers on some hybrid NVIDIA laptops; 6.8.0 does not.

Settings: the launcher, lantern.ini in this folder, or the in-game menu (Insert, or L3+R3 on a
controller). Saves go to user/, mods to mods/, and each session's log to logs/ (attach the newest
one to a bug report).

Updates: the launcher updates itself and the engine from the releases page (Settings > Updates),
keeping lantern.ini, user/, mods/ and logs/.

DLSS: NVIDIA's DLSS runtime is in engine/out (libnvidia-ngx-dlss.so.*); it needs an RTX GPU and
NVIDIA's own driver (libnvidia-ngx.so.1). DLSS Frame Generation is Windows only.

Windows only: FSR 4 through AMD's Windows DLL and DLSS Frame Generation (NVIDIA's Streamline has no
Linux runtime). Everything else, seamless co-op included, is the same as on Windows.
