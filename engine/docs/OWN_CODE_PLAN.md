# Our own code: replacing what came from other projects

Lantern Engine is to be ours: everything adapted from another port or from community patches is
replaced, one module at a time, by our own work. A project's credit comes off only once none of its
code or patches is left in the build (its license requires the notice while they ship).

Branch: `own-modules` (worktree `E:\BloodborneDev-own`), local until the user says otherwise.

## How a module is replaced

1. **Facts, not code.** From the old module take only facts: the game's addresses and byte
   signatures, structure offsets, the game files' formats, and what the player sees. Each address
   and offset is checked against the player's eboot (CUSA03173 1.09) with our own tools before use.
2. **New code from that spec**: our own structure, names and comments, written fresh, never by
   editing the old file. The old file is replaced whole. New files are GPL-2.0-or-later.
3. **Same behaviour, checked**: the module's tests (ours, rewritten with it) pass, and the user
   tests a build in game.
4. **Bookkeeping**: the module's lines leave `licenses/BBHOST-PORTS.md` (the list of what is still
   bbhost's) or the patch list below.
5. **Credits last**: when a project's list is empty, its credit leaves the README, the public
   repo's README and the launcher. When no bbhost portion is left, `BBHOST-PORTS.md` and
   `GPL-3.0.txt` go and the build is GPL-2.0-or-later again.

## bbhost (droogie): 15 parts, smallest and most independent first

| # | Part | Files | Status |
|---|---|---|---|
| 1 | Hook layer (prologue stubs, guest calls), SHA-256 | `gpu/shim/engine/engine_hooks.*`, `tools/engine/sha256.*`, `tests/test_engine_hooks.cpp` | **ours** (2026-10-23) |
| 2 | Online fixes: login character id, summon sign timeout | `gpu/shim/engine/online_fixes.*` | **ours** (2026-10-23) |
| 3 | Live graphics settings, fog | `gpu/shim/engine/graphics.*` | **ours** (2026-10-23) |
| 4 | Widescreen: camera blend, stage pins, plates | `gpu/shim/engine/widescreen.*`, `tests/test_widescreen.cpp`, a comment in `ui_composition.h` | **ours** (2026-10-23) |
| 5 | Camera sliders | `gpu/shim/engine/camera.*` | **ours** (2026-10-23) |
| 6 | Loading flag, engine state lookups, boss flags | `loading.*`, `engine_state.*` (`event_flag_layout.h` folded in), `trophy_sync.*` (its table from `tools/re/emevd_trophies.py`) | **ours** (2026-10-23) |
| 7 | Appearance editor (Dream mirror), menu heap sizes | `change_appearance.*`, `features.cpp` (the install and its two hooks), `menu_memory.*`, `scripts/engine_assets.py`, `start/assets.*` (DCX, layout) | **ours** (2026-10-23) |
| 8 | Rebirth (Altar of Despair) | `rebirth.*`, `rebirth_cost.h` (was `rebirth_refund.h`), `tools/engine/esd.*` (an inserting editor, every 1.09 talk script reads and writes back unchanged), `tools/engine/rebirth_script.*` (our own states; known result `94a2a6be…`) | **ours** (2026-10-23) |
| 9 | Cheats: boss rush refill, param rows, echoes on death | `gpu/shim/engine/cheats.cpp` (its bbhost parts) | **ours** (2026-10-23): the code was ours; every fact it took from bbhost (the debug fill's helpers and arguments, the goods and weapon row fields, the data module, the bloodstain commit) confirmed in the eboot with tools/re |
| 10 | Native PC menus | `option_menu.*` (now with `option_rows.*`, `option_keys.*`), `tools/engine/menu_assets.*` (CLI `menu_assets_main.cpp`), `tests/test_option_rows.cpp`, `tests/test_menu_assets.cpp` | **ours** (2026-10-23): every address and layout checked by `tools/re/option_menu_facts.py` (reader `tools/re/eboot.py`); the tool's messages match the old ones byte for byte in all 22 languages, the movie's seven sections match in content |
| 11 | Keyboard and mouse | `gpu/shim/input/*` (bindings, mouse camera, menu pointer, input bridge), `tests/test_pc_input.cpp` | **ours** (2026-10-23): every address, signature and offset confirmed with `tools/re/input_facts.py`; the menus' list update hooked once at its entry (our hook layer) instead of its vtable slot and five callers; our own register-saving stub at the free camera's store; `compat.h`, `mouse_camera_step.h` and the stubs files gone (`pad.h`, `mouse_turn.h`, `mouse_camera.h`, `menu_pointer.h` new); pointer log `BB_POINTER_LOG=1` |
| 12 | Occlusion culling (design) | `renderer_vulkan/vk_occlusion.*`, `occlusion_model.h`, `tests/test_occlusion.cpp` | **ours** (2026-10-23): a new design, answered per query block: the recording side tracks the game's open blocks (a query per draw only while one is open, `BB_OCCLUSION_CAP` per block, nested blocks measured exactly), a block is complete or plainly visible, and both halves are written at once when the GPU is done; no running total, no 64/1 cap per dump interval. BBHOST-PORTS.md never listed it (the code was ours; the design was credited) |
| 13 | The Hunter's Dream online backend and sign-in | `gpu/bbnet/thd/*`, `scripts/online.py` (play logs), launcher `Core/HuntersDream.cs` | **kept** (the user, 2026-10-23: their servers must not break or get hammered) |
| 14 | Frame-rate patch corrections, Old Hunters list, camera at 30 | `scripts/patches.py`, `start/patches.cpp`, `tests/test_patches.py` | **ours** (2026-10-23): every site, original byte and replacement confirmed in the eboot (`tools/re/fps_corrections.py`, 0 failures), written fresh in both; `patches.bin` byte-identical to before in 120 Python and 216 native-start cases (every rate, camera tuning and switch, DLC on and off, a differing eboot) |
| 15 | Launcher leftovers | `Views/CheatsPage.cs` (`bbhost-0.4.0-fps*` ids), comments in `InputMap.cs`, `OnlinePage.cs` | **ours** (2026-10-23): the ids were old packages' `engine` field, not settings; engines before cheats are now told by build number (the same for every id, nothing to migrate); comments reworded |

The Hunter's Dream backend (13) stays as it is: it talks to servers the site's owners run, and a
rewrite risks breaking their protocol or loading their servers. So bbhost keeps a credit for that
part alone (its GPL notice must stay while the code ships), narrowed to "online backend for The
Hunter's Dream" once everything else is ours.

## Community game patches

**Done (2026-10-23).** All 61 community patches that shipped built in are remade as ours, under their old
names so players' choices keep working. `engine/patches/Bloodborne.xml` holds 61 patches of our own
(`Author="Lantern Engine"`), written by `tools/re/game_patches.py` from the 1.09 eboot: every site is found by
what the game's code does, each original byte is checked, and the caves are our own code (several of them
in a block of unused executable memory, `0x3ABBDE2`-`0x3ABCDC6`, that no code or relocation reaches).
`python tools/re/game_patches.py out/eboot.elf` checks the shipped file; `--write` rebuilds it, `--list`
prints every write. Players can still load their own patch files.

| Old patch (author) | Ours | Site and what it changes |
|---|---|---|
| 60 FPS++, 90 FPS++ | yes | The game's own 60 FPS frame-timer mode (`0x2434806`); an eased frame time in `0x575A908` from the timer's measured time; frame counts, the limiter's capped sleep, every 1/30 step, 30 FPS rates and constants; the movement steps of `0x1915EC0` (two loads, one never patched before) read the frame time; cloth stiffness scaled each frame by min(1, (60 × frame time)²) through a value in `0x575A90C`; the airborne horizontal drag (0.95 a frame at `0x1E4ABCA`, in the character physics update) as 0.95^(30 × the step) so jumps carry as far as at 30. |
| Uncap FPS++ | yes | Frame mode 0 with no minimum frame time or sync, flips without sync, 1 ms limiter cap, the 90 design's values, `_Assert` returns, the physics step's factor reset; our caves keep the pause check and a null check the old list dropped; cloth as 90 (the old list's damping and integrator changes, which made cloth jump above 90, are gone). |
| 30 FPS++ | yes | The frame timer's 60 FPS-mode settings, the eased frame time and capped limiter at 30: lower input delay, the game's own speed. |
| 60FPS (no deltatime) | yes | 60 FPS with every step fixed at 1/60 s (no measured frame time). |
| Sprint Fix (High FPS) (yogesh1239) | yes | `0x1914C24`: the wall check divides by the frame's time. |
| Skip Intro | yes | The logo steps' enable flags cleared (`0x1FB50FA`, `0x1FB5145`, `0x1FB5190`). |
| Disable Motion Blur, SSAO, AA, Dynamic Light Shadows | yes | The renderer constructor's switch bytes. |
| Disable Chromatic Aberration, Disable DoF | yes | `0x269FAA8` zero from `xor eax, eax`; `0x25D7BBC` the pass skipped. |
| Enable Screen Space Reflections | yes | The renderer's SSR flag, setup block and full-size buffer; its 2.0 in unused memory instead of over the game's clock (the old one broke `0x4632D0`). |
| Model LOD -2 / 1 / 2 | yes | `0x216FC09`: the LOD bias as a constant. |
| Increased Graphics Heap Sizes (auser1337) | yes | The heap table `0x4B36DD8`: 3952 and 2560 MiB. |
| Intel Black Tonemap Fix, Intel 12th Gen+ SFX workaround | yes | `0x25843E5` always grades; `0x2CF83E0` returns. |
| Restore Debug Menu (Whitehawkx, auser1337) | yes | The font loader, the menu flags and our cave loading the debug font from `adhoc:/font`. |
| Restore Debug Camera (Lance McDonald) | yes | The game's debug camera controller driven by our cave (Cross + L3), not code over a live game function. |
| Enemy Control (stagvant) | yes | On lock-on the target is marked and given control (`0x1CCDE10`), a pad button hands it back; caves of ours, the controller's null check kept. |
| Unlock Game Region | yes | `0x23B9190` returns region 5, `0x23B8730` returns 0. |
| Disable Camera Auto Rotation via Movement | yes | The four auto-turn stores as nops. |
| Increased camera distance | yes | The camera row ids at `[rbx+4]`/`[rbx+8]` set to 10002 and the flag at `0x5527A94` cleared. |
| No Rally Decay, Disable Rally | yes | The Rally parameters at `0x5527B0C` and two flags; `0x1966AEC` forces "no regain". |
| Player No Dead / Stealth / Silent | yes | The game's debug flags `0x593E888`, `0x593E88E`, `0x593E88F`. |
| DS1-like physics, Lower object/corpse physics | yes | The corpse ragdoll weight and flags in its constructor (`0x1BF9439`, `0x1BF9463`). |
| Sensitive Analog Input, 50% Text scale | yes | `0x4D27714` 1.2; `0x4D2913C` 0.5. |
| FMOD Crash Fix (Dasaav) | yes | FMOD's channel setup clears its field `+0x170`; the channel index checked as 16 bits. |
| Disable HTTP Requests (bloo) | yes | The two calls that start web requests and the date lookup that stamps them. |
| Bookmark and Capture outputs (Foxy Hooligans) | yes | The capture path `system:/../capture` as `/data/bloodborne/`. |
| 4 Light Grids | yes | The light grid's tile size (`0x2695CBC`, `0x2695CC6`) at the window size. |
| Optimal 1080p and 17 Resolution Patches | yes | Generated from one rule: scene and UI size, the HUD loads at the shape's 1080-high size (2560 for 21:9, 3840 for 32:9, 1200 and 1440 high for 16:10 and 4:3), the camera aspect, the HUD's scale constants and limits, the larger heaps above 1080p. |
| Performance Patch | yes, the part that exists in 1.09 | Renderer debug parameters, shorter worker sleeps, profiling skipped, assorted calls and branches; sleeps capped by caves of ours. Left out: two writes past the eboot, a typo into data, ~40 writes from another game version that land mid-instruction (they would corrupt code; the engine never applied the old patch because of them), and function rewrites that only removed frame pointers. |

## Other credited projects

- **Engine starting point**: DarkIzuku/bloodborne_pc, itself bbport (deadinside28) and shadPS4.
  Next after bbhost: compare our files with those repositories and list what is still theirs.
- **AwesomeObserver, deadinside28** (renderer crash fixes), **0xCydral** (the FSR 4 Vulkan path):
  still in the renderer; replace each piece. VRAM cleanup, frame pacing, the network bridge and the
  trophy list are already ours.
- **shadPS4**: the renderer and shader recompiler at their core, replaced by the native path
  (`EMULATION_REMOVAL_PLAN.md`, "Moving up to GX"). Its credit stays until then.

## What stays credited

Libraries and SDKs we link or ship, as every project does: FSR-Vulkan and the AMD FidelityFX SDK,
fsr4vk, the NVIDIA DLSS SDK and Streamline, LibAtrac9, Dear ImGui, SDL and SDL_GameControllerDB,
WPF-UI, cpp-httplib, wepoll, the fonts. And The Hunter's Dream servers while we use them.

## After the parallel branches are merged

1. **Lantern names for every switch.** The engine's ~415 `BB_*` environment variables become
   `LANTERN_*`. The old names are still read for one release so existing launchers, scripts and
   players' setups keep working.
2. **A toggle for every switch.** Each variable also gets a setting (`lantern.ini`, the launcher,
   the in-game overlay where it is live), so every option works both ways: variable or toggle.
3. **Clean, sleek, optimized.** Dead switches and experiments that never shipped are removed, not
   renamed.
4. **Then shadPS4.** The shadPS4-derived renderer and core are replaced or reduced to what Lantern
   needs, module by module, the same way as bbhost.

## Where it stands (2026-10-28)

About 186k inherited lines are left, libraries excluded:

| Part | Inherited lines | Plan |
|---|---|---|
| Renderer and shader compiler | ~101k (54%) | rewrite |
| Online: shadNet pool | ~54k | drop or rewrite (user's call) |
| Start, tools, tests | ~14k | mostly delete |
| Runtime (loader, memory, saves) | ~8k | rewrite |
| PC layer leftovers | ~7k | rewrite |
| Online: Hunter's Dream backend | ~3.6k | kept on purpose |

Steps, in order:

1. Delete dead bbport code: the old Python launcher, the GTK launcher, the fsr4cap tools (small).
2. Rename the ~415 `BB_` switches to `LANTERN_` with toggles; rename `bbport_` and shadPS4 names (small).
3. Rewrite the credited bits: AwesomeObserver, deadinside28, 0xCydral fixes (small).
4. Rewrite the runtime and PC-layer leftovers, module by module (medium).
5. shadNet online pool: drop it or rewrite it (user's call).
6. Renderer: the GPU reads descriptors, textures as objects, all shaders built at install (big, weeks).

Compared with bbhost's render/next-13 branch:

| | bbhost next-13 | Lantern 0.9.37 |
|---|---|---|
| Game CPU code | native | native |
| System libraries | their own, all 660 | mostly bbport's (16% ours) |
| Draws | built from the engine's objects | read from the GPU command stream, then translated |
| Shaders | decompiled to structured SPIR-V/HLSL | our native compiler translates them, shadPS4-style |
| Textures | known by engine object | guessed by address and hash |
| GPU command stream | still emulated, only for labels and fences | the core of the renderer |
| Own code | almost all | about 28% |

## Native frame-rate changer (user, 2026-10-28)

The frame rate stops being byte lists in `patches/Bloodborne.xml` that the start applies before boot, the
way shadPS4 applies patches. It becomes an engine module: typed sites (frame timer, step constants,
frame counts, eases, cloth, air drag, physics step) found and checked in the eboot at start, written by
the engine and changeable while playing, so it survives the move off emulation. 0.9.37's regressions
belong here: charged strong attacks letting go early, slow ragdolls and corpses, jittery elevators and
ladder tops, slower stamina above 90 FPS, cloth (Cainhurst ghosts, Queen Yharnam). Beta 9's lists did
not have them; the stable lists (`tools/re/game_patches.py`) changed or dropped about 100 sites a rate.
Each site is compared with beta 9's behaviour in the game before the switch.
