# Lantern Engine without emulation

The goal: Lantern stops being "shadPS4 with patches" and becomes an engine that runs Bloodborne's
own code on our own system layer and our own renderer. This is the top-level plan. The renderer's
details are in `GX_NATIVE_PLAN.md`, the module-by-module rewrite in `OWN_CODE_PLAN.md`.

## Progress (updated with every step)

Last update: 2026-10-30, phase J step 5d.

**System library imports** the game and its modules need from us (537 in all, from
`tools/re/import_census.py`; libc and Fios2 are the game's own Sony modules and not counted):

| Served by | Imports | Share |
|---|---|---|
| **Lantern, `src/sys/`** | **226** | **42%** |
| bbport's C runtime, `src/runtime_*.c` | 49 | 9% |
| shadPS4, `gpu/shadps4/core/libraries` | 77 | 14% |
| Online (The Hunter's Dream, shadNet; the user's call) | 165 | 31% |
| Served by the game's libc module (Fios2's libc imports) or left unresolved | 20 | 4% |

**Lines of code** in the system layer: ours 6,410 (`src/sys/`, 23 files); bbport's runtime left
2,675 (was ~8,300 before phase J).

Known inherited issue, kept as it was for now (both old and new behave the same): when gapless
playback ends inside an ATRAC9 superframe, Ajm decodes the superframe's remaining frames without
checking that the input buffer holds them, so a short buffer is read past its end. The game passes
whole superframes, so it does not happen in play; fix it on its own later with a test.

| Phase | State |
|---|---|
| A. Tools (Ghidra, full dump, GX facts) | done 2026-10-29: 158,440 functions decompiled, 162 GX facts checked |
| B. GX trace | next for the renderer |
| C–I. Renderer | not started |
| J. System libraries | in progress: Rtc (10), services and dialogs (65), AppContent and Sysmodule (6), `gettimeofday` (1), Pad (9), AudioOut (6), Ajm (12), SaveData (11), semaphores (5), rwlocks (9), mutexes and condition variables (30), threads, thread-specific data and once (38), clocks, sleep, signals and process calls (24) |

Phase J log:

| Step | Libraries | Functions | Checked against the old code | In game |
|---|---|---|---|---|
| 1 | Rtc | 10 | 2,000,000 random calls, 0 differences | played, works (2026-10-30) |
| 2 | UserService, SystemService, NpTrophy, the five dialogs, ImeDialog, PlayGo, DiscMap, Mouse, Voice, AudioIn, AppContent, Sysmodule, Posix `gettimeofday` | 72 | 347-line scripted transcript, identical | played, works (2026-10-30) |
| 3 | Pad, AudioOut, Ajm | 27 | Pad: 90,000 reads on two SDL virtual controllers (hot-swap, touchpads, keyboard, menu remaps, overlay capture, pad-file injection, three configs), 0 differences. AudioOut: every port's processed audio byte-identical over 4 runs, 7.1-to-stereo fold included. Ajm: 41 real ATRAC9 streams from the game's sound banks, ~8,500 decode jobs with random layouts, identical. Planted bugs caught in all three | played, works (2026-10-30) |
| 4 | SaveData | 11 | A scripted save session on copies of a real save folder (mounts, new saves, params, icons, search with patterns and every sort, delete, save-data memory, the sound-flag fix, bad names, 16-slot limit): transcript and the final 52 files (30 MB) identical. Planted bugs caught | played, works (2026-10-30) |
| 5a | Semaphores, rwlocks | 14 | Scripted transcript with real waiting threads (queue order, partial signals, timeouts and the time left, cancel and delete wake-ups, deadlock, EBUSY, EPERM, destroyed and bogus handles): identical. Stress on 8 threads: 0 rwlock violations, 120,000 semaphore items balanced, like the old. Faster: semaphore stress 26 ms (old 97), lookups among 3,000 objects 7 ms (old 257), about 1 probe each instead of 59-77 list steps. Planted bugs caught | played, works (2026-10-30) |
| 5b | Mutexes, condition variables (libkernel and Posix) | 30 | 256-line scripted transcript with real threads (every type's self-lock, recursion depth, foreign unlock, timed locks, destroy while held or waited on, static initializers, cond waits on unowned and foreign-held mutexes, signal/broadcast counts, recursive mutex 3 deep in a cond wait, every Posix code): identical to the old Windows code on Windows and on Linux. Stress: 8 threads, 1.6 M locks, and a 200,000-item cond queue, balanced. Faster on Windows (medians of 10): contended 54 ms (old 66), cond queue 13.5 ms (old 20), uncontended the same; Linux within noise. 10 of 13 planted bugs caught, the other 3 equivalent there or new checks the old code lacked (the unit test covers those) | played, works (2026-10-30), see below |
| 5c | Threads (create, join, detach, exit, self, priorities, affinity, names, attributes), thread-specific data, once, `__error` (libkernel and Posix) | 38 | 174-line scripted transcript with real threads (every attribute setter and getter with good and bad values, destroyed and bogus handles, create and join results, exit from nested calls, join self, main, twice, detached and detached-later threads, per-thread errno, key destructors at thread exit, 255-key limit, once among 8 threads, the audio-priority threads, the Posix codes, the engine's own spawn calls): identical to the old code on Windows and Linux. Faster: thread lookups 9 ms for 1.28 M (old 65 ms; about 2 probes instead of 51 list steps), lookups among 3,000 live attributes 9 ms (old 2.5 s). 10 of 12 planted bugs caught, the other 2 cannot be seen through any import (a default nothing reads back, a freed attribute left in the registry) | played, works (2026-10-30), see below |
| 5d | Clocks, sleep, time zone, process time, TSC, `time`, page size, pid, yield, `_exit`, debug exceptions, signals and masks, `getrusage`, `sysctl` (libkernel, Posix, LibcInternal) | 24 | 293-line transcript (every clock id with good and bad pointers, kernel and Posix error forms and errno, the coarse clock's zero nanoseconds, sleeps and the sleep counters, nanosleep's checks, signals and every mask operation, rusage, every sysctl branch, the host-to-guest errno map for 141 values): identical to the old code on Windows and on Linux. 12 of 12 planted bugs caught. The five functions the game never imports (no NID) are no longer exported; the engine's own process-time and TSC helpers keep their names | in test (2026-10-30) |

Step 5b in-game test (2026-10-30, played by the user, Windows, game on a hard disk):

| Check | Result |
|---|---|
| Boot to the title screen, load a save | works; first load 53.1 s (cold disk cache, fresh package), in game 67.8 s after start |
| Change area, travel by lamp | works; second load 12.9 s |
| Save, quit to the title, load again | works; save data mounted and written, no errors |
| Sound, music, pad | work, no stutter or hang |
| Length | about 3 minutes; no `Freeze:` or `STOP` lines |
| Exit | clean shutdown in 732 ms, exit code 0 (the runtime counter lines are not printed on this exit path, as before 5b) |

Step 5c in-game test (2026-10-30, played by the user, Windows, offline):

| Check | Result |
|---|---|
| Boot, load a save | works; 74 game threads started through the new code; first load 12.2 s, in game 32.2 s after start |
| Change area | works; the next load 49.6 s, read-bound on the hard disk (the loaders reading 30-100% of the time at 8-34 MB/s), not threads |
| Sound, music, pad | work; the audio threads ran at normal priority because the launcher's audio priority setting is off by default (`BB_AUDIO_THREAD_PRIORITY=0`), as before |
| Online (summon or join, leave) | works: a seamless co-op session on this build (06:32-06:39, Play online on), clean shutdown, exit code 0 |
| Exit | clean shutdown in 328 ms, exit code 0, no `STOP` or `Freeze:` lines |

## What we are today

The game's CPU code already runs natively: a PS4 is x86-64, so the eboot runs on the PC's CPU as
it is. Decompiling it and compiling it again would buy no speed. What makes Lantern look like a
wrapper is everything under the game:

| Layer | Today | Inherited lines |
|---|---|---|
| GPU: PM4 command processor, register state, texture and buffer caches | shadPS4 `video_core` | ~65k |
| Shaders: GCN to SPIR-V | shadPS4 `shader_recompiler` (our native compiler beside it) | ~40k |
| System libraries (kernel, pthreads, Gnm, Np, Net, audio, pad, saves) | bbport runtime and shadPS4 `core/libraries`, ours in part | ~20k |
| Online pool | shadNet | ~54k |

The game draws through GX, FromSoftware's Direct3D 11-shaped layer. GX turns each call into Gnm
packets; the renderer then reads those packets back and guesses what the draw was. Both sides of
that do work only because of the console: the game encodes, we decode.

## Where we are going

```
Bloodborne's code (native, unchanged)
  ├─ GX device and context calls ──▶ Lantern GX: our objects and Vulkan, at the call
  ├─ YEBIS / Scaleform / flip pass ─▶ the same, through their own context calls
  ├─ GCN shader binaries ───────────▶ our lifter, built to SPIR-V at install
  └─ system library imports ────────▶ Lantern's own library layer
```

No PM4 parsing, no GPU register emulation, no address-and-hash texture guessing, no memory watching
to find what the CPU changed. The PM4 path stays behind a switch until the GX path has covered
everything for a release, then it is deleted.

Why this is the optimized way and not just "our own":

- **Half the CPU work goes.** The game no longer encodes packets for us to decode. Each GX call
  becomes a Vulkan command (or a state change) directly.
- **Objects, not addresses.** GX creates every buffer, texture, view, shader and state object
  through a known creator with a D3D11-style description. Each becomes its Vulkan object at
  creation, so the texture cache's guessing, the write faults and re-protects and the detiling
  on the fly all go.
- **Threads the game already has.** GX's deferred contexts record on the game's worker threads and
  the immediate context replays them. That maps onto Vulkan secondary command buffers recorded on
  the same threads.
- **Pipelines known ahead.** Shader and state objects are known at creation, so every pipeline can
  be built at install or at load, not mid-frame.

## The decompilation is our map, not our code

We do not recompile the game from decompiled C. The decompiler shows us which function does what
and how GX's structures are laid out, so our hooks and our replacement GX are right.

Tools (all ours, in `engine/tools/re/`):

| Tool | What it does |
|---|---|
| `ghidra_elf.py` | turns the player's decrypted eboot into a plain ELF Ghidra loads: applies the 231k pointer relocations, names 687 of the 703 imports from `src/import_names.inc`, lists the 162k functions from the eboot's unwind table |
| `ghidra/LanternSeed.java` | creates those functions and import names before analysis |
| `ghidra/LanternDecompile.java` | decompiles the functions asked for, or exports the function list |
| `ghidra/LanternDump.java` | decompiles every function into one file per 1 MB of code |
| `ghidra.sh` | `import` (convert and analyse, ~50 min), `decompile <addr>...`, `dump` (~45 min) |
| `dumpfn.py` | prints a function from the dump by address, `--depth N` follows its calls |
| `gx_facts.py` | checks every GX fact we rely on against the eboot (must print `0 failed`) |

Setup: Ghidra 12.2 and Temurin JDK 25 in `E:\re-tools` (Ghidra 12.2 needs JDK 25). The project is
`E:\re-tools\projects\Bloodborne`, the dump `E:\re-tools\work\dump` (158,440 functions, 7 the
decompiler could not do). The heap is 6 GB (`GHIDRA_HEADLESS_MAXMEM`); 10 GB ran out of memory on
16 GB of RAM. Ghidra's settings go to `E:\re-tools\settings`, because its plugin framework does not
start from the default settings folder on this machine.

Rules for it:

- **The dump and the Ghidra project never go into git or a release.** They are the game's code.
  Only facts (addresses, offsets, sizes, what a function does) go into our files, each checked by
  a script like `gx_facts.py`.
- Our code is written fresh from those facts, as for the bbhost rewrite.
- Addresses are image offsets (the eboot at 0). Paleblood and bbhost use a 0x400000 base.

## Phases

Each phase is behind a `LANTERN_*` switch, off by default, the old path kept, checked under Vulkan
validation for every GPU family, shipped in a beta, and made the default only after AMD and older
NVIDIA players confirm it.

### A. Tools (done 2026-10-29)

The Ghidra project, the full dump, and 162 GX facts checked. GX's pending state, its 36-command
replay table, its creators and its deferred context are mapped (`GX_NATIVE_PLAN.md`).

### B. GX trace (`LANTERN_GX_TRACE`): small

Hook the 4 input-assembler setters, the 24 stage setters, the output-merger setters, the 8 draws,
Dispatch and Commit. Count each per frame, and at each draw compare the pending state with what
the PM4 path resolves for the same draw. Zero differences in Central Yharnam, Cathedral Ward, the
Hunter's Dream and one boss are the gate to C.

The four view creators and the geometry shader creators are named (`GX_NATIVE_PLAN.md`). Before
coding: find the four state-object creators and the rasterizer and viewport slots in the dump with
`dumpfn.py` and add them to `gx_facts.py`. The trace itself names the other five shader creators'
stages (which creator made what each SetShader receives).

### C. GX objects (`LANTERN_GX_OBJECTS`): medium

Hook the creators and releases. Each GX object gets an entry in our tables with a generation:
buffers and textures with their memory, format and size, views with their resource, shaders with
their GCN binary, state objects with their description. Find Map/Unmap and UpdateSubresource and
every place the game writes a resource's memory without them (those still need watching).

### D. Draws from GX (`LANTERN_GX_DRAWS`): big

At a draw, the pending state plus the object tables give the whole draw. It is recorded as our own
record and the command buffer only keeps a token for ordering. A draw the record cannot cover goes
the PM4 way, and the log counts those by reason until the count is zero.

### E. Buffers and textures by object: big

Immutable buffers live in VRAM from creation, dynamic ones in a host-visible ring, textures as
Vulkan images from creation, filled by GX's upload and copy calls and by the shaders that write
them. This replaces most of `buffer_cache` and `texture_cache` and the memory watching
(`EMULATION_REMOVAL_PLAN.md` steps 4 and 6 become this).

### F. Compute, clears, copies, queries, completions: medium

Every remaining GX call as a token. The labels and fences GX waits on are written when our
submission has run. After this the PM4 walk only orders tokens.

### G. Shaders built at install: big

The lifter takes each shader object's GCN binary to structured SPIR-V (our native compiler is the
start). If, as expected, every shader comes from the game's files (to confirm by logging the shader
creators' binaries against the files), every one is built at install and validated per GPU family. Pipelines are built at load from the shader and state objects.

### H. YEBIS, Scaleform and the flip pass: medium

YEBIS has its own D3D11-shaped context (vtable `0x530a380`, 133 methods): the same approach through
its methods. Scaleform's renderer and the Gnmx flip pass are found in the dump and moved the same way.

### I. The translator goes: deletion

With D–H the default everywhere, `video_core`'s PM4 processor, register state, SRT walker,
constant-engine emulation, GPU-write tracking and address-keyed caches are deleted, and so is
shadPS4's `shader_recompiler`. That is about 100k inherited lines.

### J. Our own system libraries: started 2026-10-30

`tools/re/import_census.py` lists the eboot's 701 imports by library and which file serves each
(`--list` names the functions). libc (216) and Fios2 (17) are the game's own Sony modules
(`sce_module/*.prx`), linked and run natively, so they are not ours to write. The rest:

| Served by | Libraries | Functions |
|---|---|---|
| **Lantern, `src/sys/`** (moved in steps 1-5d) | Rtc, the service and dialog libraries, Pad, AudioOut, Ajm, SaveData, semaphores, rwlocks, mutexes, condition variables, threads, thread-specific data, clocks, sleep, signals, process | 226 |
| bbport's C runtime, `src/runtime_*.c` | kernel and Posix: memory (16), files (24), the C library start-up in `runtime.c` (9) | 49 |
| shadPS4, `gpu/shadps4/core/libraries` | GnmDriver 50, AvPlayer 16, VideoOut 8, kernel equeue 3 | 77 |
| online, `gpu/bbnet` | Net, NetCtl, Http, Ssl, Np* | 165 (The Hunter's Dream and shadNet: the user's call) |

Our libraries live in `src/sys/`, one file per Sony library, each a table keyed by NID
(`SYS_LIBRARY`). `runtime_resolve` asks `sys_resolve` first, so a library moves by adding its file
and deleting the old one. Each is checked against the old one before the old one goes: a scratch
program links both and compares them on random input (Rtc: 2,000,000 checks, 0 differences), and a
unit test stays (`tests/test_sys_rtc.c`, in `build.sh --test`).

Order, small and testable first, stability-critical once the pattern is proven:

1. **Rtc** (10): done, `src/sys/rtc.c`.
2. **Services and dialogs** (72): done, `src/sys/` (user and system services, trophies, dialogs,
   ImeDialog, PlayGo, DiscMap, Mouse, Voice, AudioIn, AppContent, Sysmodule, `gettimeofday`).
3. **Pad, AudioOut, Ajm** (27): done, `src/sys/pad*.c`, `audio_out.c`, `ajm.c` (LibAtrac9 stays as a
   library).
4. **SaveData** (11): done, `src/sys/save_data.c`.
5. Kernel synchronisation, in three parts: **semaphores and rwlocks** (14): done, `src/sys/semaphore.c`,
   `rwlock.c`, hash-table registries in place of the old lists; **mutexes and condition variables** (30):
   done, `src/sys/mutex.c`, our types, owners and error codes over SRWLOCK and CONDITION_VARIABLE on Windows
   and plain pthread objects on Linux, so both behave the same (the old Linux code used glibc's typed
   mutexes: a held normal mutex could be destroyed and a recursive mutex held more than once was never
   released by a cond wait); a mutex a cond waiter still uses is not destroyed (EBUSY), a cond destroyed
   with waiters is freed by the last one out, and the lock counters are per thread instead of one shared
   racy line; **threads** (38, with thread-specific data, once and `__error`): done, `src/sys/thread.c`,
   hash-table registries for threads and attributes in place of the old lists (the `BB_THREAD_FRONT`
   move-to-front switch is gone with them); the engine's own `runtime_thread_spawn` and friends, which the
   online code calls, keep their names. **Clocks, sleep, signals and process calls** (24): done,
   `src/sys/clock.c` and `process.c`; `runtime_kernel.c` keeps only the engine's wait and sampler
   diagnostics.
6. Kernel memory and files: the largest and riskiest (fault handling, the guest address space).
7. shadPS4's: VideoOut and equeue (with our presenter), AvPlayer, then GnmDriver (shrinks as the GX
   path lands; after I only what GX calls at start).

Every step needs an in-game check by the user before the next one depends on it.

## Order and what comes next

1. **B**, starting with naming the creators and stages in the dump. This is the next session.
2. **C**, then **D** on a branch, measured against the PM4 path frame by frame.
3. **E** and **F** turn D into the speed gain. **G** can start beside them.
4. **H**, then **I** in one release once the betas are clean.
5. **J** runs alongside, one library at a time.

Done means: no shadPS4 `video_core` or `shader_recompiler`, no PM4 parsing in a normal frame,
every system library function the game calls implemented by us, and the credits list only the
libraries and SDKs we link.
