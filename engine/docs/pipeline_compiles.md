# Pipeline compiles and the waits behind them

A pipeline first seen during play is compiled on the compile threads (`BB_ASYNC_PIPELINES`,
default on). With a cold NVIDIA cache a compile takes 100-300 ms; entering an area brings
20-90 at once. Pipelines known from the shader database are created at start (preload) and
come out of the driver cache (`<serial>.vkcache`) quickly.

The start also restores cached shader modules that no saved pipeline references.
Object-motion selection first inspects ordinary shader variants, then selects the
motion-enabled variants for the final pipeline. Warming only saved pipelines left
those ordinary modules out, translating the same shaders again on every launch
despite their `.meta` and `.spv` files already being on disk. Standalone restoration
uses the same profile, specialization, binary-version and SPIR-V validation as the
pipeline warm-up; incompatible records are reported and rebuilt on demand.

## What a draw does with a pipeline still compiling

Before (0.9.10 beta 1) the draw recording thread (stage B of the draw pipe) waited for it when
it recorded the bind: every new pipeline stalled the frame by its compile time (user logs, RTX
5060: `2 compiles 594 ms` -> a 607 ms frame, `18 compiles 1110 ms` -> 1665 ms), and the GPU
command thread and the guest threads that waited for stage B (drains, see below) stalled with
it. Now (`Rasterizer::DrawRecord`, `HandleIfReady`):

- **Scene geometry is left out** while its pipeline compiles (`SkipUncompiledDraw`): draws with
  the scene depth (the G-buffer pass's, which was also the scene depth the frame before) that
  are not full-screen passes (more than 6 indices or instanced) - the draws the upscaler
  jitters. G-buffer, light volumes, forward/transparent draws and effects drawn with the scene
  depth. The frame draws these targets again from a clear, so the object is missing for the
  frames the compile takes (pop-in), nothing more. Everything else of the draw runs as before
  (targets, barriers, render pass, resources): only `vkCmdDraw` is not recorded. Never left
  out: draws the game measures (occlusion queries: a draw left out makes its query block read
  plainly visible, so nothing would be culled), indirect draws, compute dispatches, shadow maps, half-resolution effects,
  post-processing and UI (what they draw may be kept over frames or read as a whole), and
  draws during a frame capture. A pipeline's draws are left out for at most
  `BB_SKIP_UNCOMPILED_MS` (1000) from its first left-out draw; then they wait for it. "A few
  frames" at 120 FPS is 25-40 ms, below every compile measured, so the bound is in time.
  `BB_SKIP_UNCOMPILED=0`: never left out.
- **Everything else binds the pipeline on a recording thread** (`BindWhenCompiled`): the bind
  becomes a command of its own, and the recording thread that records it into the command
  buffer waits for the compile there. Stage B goes on with the next draws, so drains of the
  GPU command thread (and the guest threads behind them) do not wait for the compile. The
  frame itself still waits for it (its submission needs the command). `BB_LATE_PIPELINE_BIND=0`
  (or commands recorded on the calling thread: `BB_VK_RECORD_THREAD=0`, direct segments):
  stage B waits, as before.
- **A thread that must wait compiles the pipeline itself** when no compile thread has started
  it (`Pipeline::WaitReady`, `TakePipelineJob`): in a burst it was queued behind the other
  pipelines first. `BB_PIPELINE_TAKE=0`: waits for the compile threads.

An already compiled pipeline costs what it did: `HandleIfReady` is the load and branch
`Handle()` was. The draw command gained a branch on the recording thread (bind or not).

## Why not VK_EXT_graphics_pipeline_library

DXVK links pipelines from libraries compiled when the game creates its shaders, long before the
draws. Here (measured on the user's shader database, 855 graphics pipelines):

- Shaders reach the port at the draw that needs them: the guest binary is translated there.
  Libraries would be compiled at that draw too; only linking is fast.
- All stages of a pipeline share descriptor set 0 (one push descriptor set; the vertex stage's
  bindings follow the fragment stage's, and the vertex module is specialized on that offset).
  Libraries need identically defined layouts, so they are shared only between pipelines with
  the same stage modules: 780 distinct stage sets among the 855 pipelines (75, 9%, would reuse
  libraries). Separate sets per stage would end push descriptors for one of them: a descriptor
  set write per draw on the thread that limits the frame rate.
- NVIDIA compiles each library stage about as fully as in a pipeline: a draw would wait for
  the slower of the vertex and fragment libraries instead of both (perhaps 30-40% less), at
  about twice the compile work (libraries and the optimized pipeline after them).

The start line reports whether the device offers it (`graphics pipeline library offered (fast
linking)`) for later work, e.g. if shader translation could run ahead of the draws.

## Waits behind the draw recording thread (write faults, drains)

`Stall: 335.8 ms frame; ... write faults 324.6 ms; GPU thread on CPU 328.1 ms (kernel 203.1
ms)`, and from a boss fight `image create 575 ms`, `image refresh 608.6 ms` with `write faults
605.1 ms`. Two different waits:

- **Guest threads** handle their write faults with locks (`OnWriteFault`, `assume_locks`
  false); they never drain. `TextureCache::InvalidateMemory` takes the texture cache's lock,
  which stage B holds through `FindImage` (image creation, page protection) and `UpdateImage`
  (`RefreshImage`: the upload): 0.5-0.6 s with memory short. Most faults are on buffer pages
  (the game rewriting constants and vertices) that no image watches. The texture cache now
  counts, per 4 KiB page, the tracked images that protect it (`WatchedPages`, kept with the
  page manager's watchers, readable without the lock); a fault on a page no image watches
  updates the buffer tracking only. Images learn of CPU writes through their own page
  protection alone, so this is what the lock gave them: an untracked image is already dirty,
  or new (dirty), or hash-checked (MaybeCpuDirty). `BB_FAULT_SKIP_TEXTURES=0`: the lock for
  every fault. A fault on a page an image watches still waits for the lock (unchanged), and
  a guest thread's fault on GPU-written data still needs the GPU thread for the readback.
- **The GPU command thread** (stage A) drains before it handles its own faults (stream order:
  the queued draws read the old bytes first; a readback records commands) and at many other
  points. It waited with `SwitchToThread` in a loop for as long as stage B took: a core burnt
  in the kernel while stage B (or a compile thread) needed it. Its waits (`Drain`,
  `WaitForPacket`, a full ring) now spin, yield for `BB_DRAIN_SPIN_US` (500), then sleep until
  stage B has run the packet they need (stage B checks after each packet whether stage A
  sleeps for it: a load and a branch). `BB_DRAIN_SLEEP=0`: spin and yield as before. The wait
  itself is as long as stage B's work: with compiles off stage B that is now mostly image
  creation and uploads (memory pressure).

## Statistics and log lines

At start:

    Pipelines: new ones compiled on 4 threads; draws of one still compiling: scene geometry left out for up to 1000 ms, other draws bind it on the recording threads (they wait for it) (BB_SKIP_UNCOMPILED, BB_LATE_PIPELINE_BIND); graphics pipeline library offered (fast linking), not used
    Waits: GPU command thread drains sleep after a spin (stage B wakes them); write faults on pages no image watches leave the texture cache alone (BB_DRAIN_SLEEP, BB_FAULT_SKIP_TEXTURES)

`BB_FRAME_STATS`, every 5 s when anything happened:

    Pipelines: 18 compiled in the background (2 by the thread that needed them); 340 draws of 12 pipelines left out while compiling; 6 draws/dispatches bound on the recording threads; 3 waits, 210.4 ms, longest 120.3 ms

and `(N/s slept)` in the `Draw pipe:` line. The `shader/pipeline compiles, ms` figure of
`Frame stats` still adds the waits (now on recording threads too) to the translation time.

## Risks

- Left-out scene draws: an object (or light volume, decal, particle) appears a few frames
  late the first time its pipeline is needed. A transparent layer whose pipeline is ready
  over an opaque one still compiling shows over the background for those frames.
- A recording thread waiting for a compile delays its segment, the submission and the guest's
  fences after it, as stage B's wait did; ordered tasks (guest copies, fence signals) queue
  behind it.
- `ReplaceShader` (shader patching from the debug UI) destroys pipelines at once; a late bind
  still queued for one would use it after that.

## Why pipelines were not compiled before play (0.9.37 beta 3, measured)

The pop-in above only happens for a pipeline the start did not build. Measured on the dev
install (`out\BloodbornePC`, RTX 5060) and the shipped catalog:

- **The shipped shader catalog was refused on every PC since 0.9.34 beta 9.9.** Its
  `profile.bin` is 64 bytes; beta 9.9 appended the engine's own compile switches to
  `Shader::Profile` (`uniform_dword_layout` ... `flatbuf_storage`, 72 bytes), and `--seed-cache`
  compared the whole file: `Shader catalog: made on another kind of GPU than this one; not used`,
  and bb-start wrote `catalog-1990-6261149.applied` with exit code 5, so it never tried again. Its
  specializations were also meta version 7 (beta 9.9 made it 8: `HwFragmentRuntimeInfo::
  depth_first`), which `ReadShaderMetaSpec` rejects. Every player started with an empty cache and
  compiled every pipeline of every area in play.
- **A profile change empties the cache, shader states included.** `PipelineCache::WarmUp` clears
  the folder when the profile differs (a new profile field, `BB_UBO_*`/`BB_EARLY_Z`, a driver or
  GPU change). The dev cache was emptied at 17:37 on 21 Oct; 331 pipeline keys were then written
  in play over the next five minutes (pop-in), and the catalog that should have refilled it was
  refused (above).
- **Geometry shaders could not be seeded.** A geometry shader and its copy shader carry the same
  footer hash (62 pairs in the game's bundles; `BinaryInfo::type` 5 and 6). The lab's code index
  kept the first program of a hash, so a GS could get its copy shader's code: the compiler crashed
  on it and the GS and its keys were left out (3 GS, 28 keys in a fresh seed).
- **Pipeline keys outlived their shaders.** A recompile that leaves a permutation out kept its
  keys: 22 of 387 keys in the dev cache and 80 of 567 in the catalog named a stage without a
  specialization (`Failed to open ... .meta` at every start, rejected by the warm-up). Worse, a
  later permutation of that program takes the freed number and the old key would join it to the
  old one's vertex inputs.
- In play on a seeded cache (the last dev session) only 2 pipelines were compiled and no draw was
  left out: what is in the cache is built at the start; what is missing is the catalog's coverage.

## Fixes (bb-shader-lab, bb-start, the catalog)

- `--seed-cache` compares only the GPU's part of the profile (the fields before
  `uniform_constant_buffers`). A catalog made with other compile switches is still checked
  permutation by permutation: `RecompileBatch` keeps one only if this compiler, with this cache's
  switches, specializes it the same. Version 7 catalog records are read as version 8 (the new field
  sat in padding the state carries alike).
- The code index keeps copy shaders (type 6) apart: `Code(hash, copy_shader)`.
- After every recompile (bb-start's, and the one after a seed) keys whose stages have no
  specialization are removed; the `Shader cache:` line counts them. `--export-catalog` exports only
  keys whose stages are all in the catalog and only states with their specialization.
- The seed summary counts `records of another engine` (a catalog an engine change made unreadable:
  it must be exported again), instead of skipping them silently.
- The catalog records the GPU vendor it was made on (`gpu.txt`, from the driver cache header beside
  the cache folder). On a first start (no cache folder yet, so no profile of this GPU) bb-start puts
  the catalog's `profile.bin` there when a display adapter has that vendor, so the first session
  is seeded too. A GPU of that vendor with another profile only loses the seeding time: the engine
  starts the cache again.
- The catalog lists its own files (`files.txt`): `--seed-cache` reads only those, so files of an
  older catalog an update left beside it are not mixed in (the updater copies over the install;
  it now replaces a catalog folder whole, as `build.ps1` does).
- The shipped catalog (`package-template\engine\shader-catalog\CUSA03173`) was made again: the old
  catalog seeded into the dev cache (127 permutations added, 544 there already, 15 left out), then
  exported: 732 states, 531 pipeline keys (was 725 / 567, of which 0 usable).

Seeded into an empty cache with this PC's profile (15 s):

| catalog | permutations kept | pipeline keys built at the start |
|---|---|---|
| shipped before (any engine since beta 9.9) | 0 | 0 |
| shipped before, with the fixed lab | 578 | 430 |
| new | 627 | 510 |

22 programs of the catalog are not in the game's shader bundles or the executable (another
file holds them); their 21 keys are not seeded.

## The catalog grown from a player's (0.9.37 beta 9)

The shipped catalog again, from the dev cache after a play session (113 pipelines compiled in play
in an area the old catalog did not cover) with a player's RTX 4060 catalog seeded in (303
permutations added, 556 keys), exported: 1104 states, 1085 keys (was 732 / 531). Seeded into an
empty cache with this PC's profile, then `--check-cache`:

| catalog | permutations kept | keys after the seed | built at the start |
|---|---|---|---|
| shipped in beta 8 | 627 | 510 | 431 |
| beta 9 | 937 | 960 | 806 |

What the start left out was nearly all one kind: keys made by engines up to beta 6, whose vertex
module was compiled beside another fragment shader (`a stage compiled after another fragment shader
(binding start)`: 77 in the old catalog, 150 in the new one). Such a pipeline was built once in play
on every PC the catalog seeded.

`bb-shader-lab --fix-keys <cache>` (run by every `--recompile-cache` and `--seed-cache`, so by
bb-start after a compiler change and at every seeding, in a child process) rewrites them: each
stage whose bindings do not start where the key's earlier stages end is given a permutation of its
program that does (the specialization's own comparison leaves the start out for a stage without
guest resources, so the start is compared as the warm-up does), compiled from its state when the
cache has none, and the key is written again under the name the engine gives it. Of the 150, 145
had such a permutation already and 5 were compiled; 79 keys turned out to be pipelines the cache
had under their right stages too. The catalog was exported again after it:

| catalog | keys after the seed | built at the start |
|---|---|---|
| shipped in beta 8 | 510 | 431 |
| beta 9, keys rewritten | 877 | 869 |

The 8 left are keys whose fragment shader reads the motion vectors their vertex shader does not
write.

## A catalog per GPU vendor (0.9.37 beta 9)

The catalogs: `engine\shader-catalog\<game>` (NVIDIA, as before) and
`engine\shader-catalog\amd\<game>`, made from a player's RX 7800 XT catalog (vendor 1002, device
747E): seeded into an empty cache with its own profile (the lab compiles on the CPU; no AMD GPU
needed), 929 permutations, 125 keys rewritten by `--fix-keys`, `--check-cache` 771 of 771 keys
built at the start, 0 layouts that do not match, then exported (942 states, 771 keys; the states
carry no game code). bb-start (`SeedGpuCache`) and the Linux start (`scripts/gpu_cache.py`) seed one
catalog per game: of the vendor the engine last ran that cache on (the driver cache beside it,
`<game>.vkcache`, "BPBC" with the vendor at 8), else on a first start of this PC's adapters NVIDIA
first, then AMD, then Intel, as the engine picks the faster GPU. A laptop with a Ryzen APU and an
RTX card must not get the AMD catalog's profile on its first start: the NVIDIA one would then be
marked applied to a cache the engine starts again. Intel has no catalog yet.

## Remaining (not done)

- **The catalog covers what the dev played.** Areas nobody captured still compile in play. Export
  the catalog again after playing new areas (or from testers' caches: `--seed-cache` merges a
  catalog into a cache, renumbering clashes, then `--export-catalog`).
- **Export the catalog again after every change of `ShaderMetaVersion`, `CaptureVersion` or the
  GPU part of `Shader::Profile`**: the seed line's `records of another engine` shows a stale one.
- **The warm-up waits for every cached pipeline.** Since 2026-10-22 the driver builds them on
  `BB_WARMUP_THREADS` threads (default the hardware threads less one, at most 8; 601 pipelines
  with the driver cache cold: 13.9 s on one thread, 3.1 s on 8; `native_shader_compiler.md`,
  "Compile speed and output quality"); their state is still rebuilt on the main thread. Letting
  the game start at once (draws of a pipeline not ready yet behave as in play) would hide the
  rest.
- **A profile change still empties the cache.** Keeping the states and keys and seeding them like
  a catalog would refill it at the next start.
- The catalog is seeded only into `<serial>`, not `<serial>-scene-curve` (BB_HDR_SCENE_CURVE=1):
  the lab's recompile does not turn the tone curve on for that folder.
- Seeding needs the GPU vendor's catalog: AMD and Intel players get none until a catalog is made
  on such a GPU.

## Sharing catalogs between players (launcher, 0.9.37 beta 4)

Settings › Shader catalog: "Export catalog" writes this cache as a catalog (`bb-shader-lab
--export-catalog`, zipped: `CUSA03173/` with states, specializations, keys, `profile.bin`,
`gpu.txt`, `files.txt`; no game code). "Add catalog" merges a catalog into the cache
(`--seed-cache` with the game folder and `engine\out\eboot.elf`), so several players' catalogs
add up and any engine version that reads them can use them; it needs a cache the engine has
started once (its `profile.bin`) and a GPU of the same kind. A zip of an older launcher (a whole
cache with `engine.txt`) still replaces the cache as before (`ShaderCacheShare.Import`).

To grow the shipped catalog from players' files: seed each into a copy of the dev cache
(`--seed-cache`), then `--export-catalog` the result into
`package-template\engine\shader-catalog\CUSA03173` (replace the folder whole).

## Players' CPU and GPU under-used (0.9.37 beta 3 logs, fixed in the launcher for beta 4)

An i7-10700F with an RTX 4060 Ti held 46-69 FPS with the CPU at 32% and the GPU at 30%: one thread
bound, not the hardware. His log against the dev's: 9,230 protect calls/s (141k pages/s) and
1,800 write faults/s against ~170 protects/s, because the launcher's defaults switched off half of
the engine's own speedups (`GameEnvironment`: `BB_GPU_THREAD_PRIORITY=0`, `BB_SLEEP_SPIN_US=600`,
`BB_PRECISE_PACING=0` unless their experimental toggles were on) and the dev ran with the PC memory
model (`BB_GUEST_IN_PLACE=1`), native draw calls (`BB_NATIVE_CALLS=1`), hot pages, fast loading
and the native shader compiler. Beta 4's launcher makes that set the default
(`LauncherSettings.PerfDefaults`, switched once over older files by `perf_defaults_b4`; every one
stays a toggle in Graphics › Experimental engine features, and safe mode turns them off).
The catalog seeds a legacy-compiler cache as fully as a native one (510 pipelines each).
OptiScaler's compatibility mode sends every submission from one thread while its frame generation
is on: players capped by it should try without OptiScaler's frame generation.
