# Native shader compiler (2026-10-21)

The engine translates the game's GCN shaders to SPIR-V with the compiler it inherited from
shadPS4 (`gpu/shadps4/shader_recompiler/`). That compiler is written for every PS4 game; this one
is for Bloodborne's shaders only, which are a closed set: about 860 programs in the shader catalog,
all compiled by the same PSSL compiler. The plan here replaces the inherited path piece by piece,
each piece checked against it per captured shader in the shader lab before the game uses it. It is
step 7 of `EMULATION_REMOVAL_PLAN.md` ("shaders fully precompiled per GPU").

## Why

Two player reports at Ebrietas traced to the inherited compiler. Her particle draws read their
resource tables (SRT) through null pointers. The resource table walker, x86 code the compiler
generates per shader and runs on the CPU at every draw, faulted on each such read: a Windows
exception per read, tens of thousands a frame (90 to 25 FPS), and the faulting draws were skipped,
so her particles stayed behind. The walker now checks for null tables (zeros, as the PS4 reads,
and the draw runs), but the design is the problem: machine code reading the game's memory without
knowing whether it is mapped, recovered by an exception handler. A table pointer that is not null
but unmapped still faults, and still skips the draw. The Nexus "SFX Intel fix" works around effect
bugs of this kind by replacing the game's effect files; the engine should not need that.

The other inherited parts that matter for Bloodborne:

- **Control flow.** The structurizer (`frontend/structured_control_flow.cpp`, yuzu's goto
  elimination) gives every block a goto variable and moves gotos outward and inward until they are
  ifs and loops over those variables; SSA then turns the variables into phis and selects. It
  handles any graph, at the price of flag logic in every shader, also in those whose graph is
  already structured.
- **Descriptor layout.** All stages share one push descriptor set; the vertex stage's bindings
  follow the fragment stage's, so each vertex module is specialized on that offset
  (`pipeline_compiles.md`). Modules depend on their partner stage: no pipeline libraries, and
  SPIR-V cannot be made before the pipeline is known.
- **Compiles in play.** A shader is translated at the first draw that needs it. The cache
  recompile from shader states (`--recompile-cache`, `--seed-cache`) already moves most of this
  before the game starts.

## What the game's shaders use

`bb-shader-lab --census <folder>` (new) compiles each capture with the current compiler and
reports its GCN opcodes, the shape of its control flow graph, its resources and its resource
tables. The numbers below are from the two capture folders: a player's (857 captures) and the
development install's (412).

| | player | dev install |
|---|---|---|
| captures compiled (of) | 838 (856) | 412 (412) |
| pixel / vertex / compute | 480 / 305 / 24 | 217 / 157 / 19 |
| geometry, export, hull, local (tessellation) | 6, 2, 12, 9 | 4, 2, 7, 6 |
| GCN instructions | 263 737 | 114 554 |
| distinct GCN opcodes | 162 | 158 |

The 18 the current compiler does not compile are captures of an older engine (capture version 3)
of hull and domain shaders, without the tessellation constants version 4 added; not compiler bugs.

**Resource tables.** Every table any shader reads is one level below the user data: no table
holds a pointer the shader follows, and no offset into a table is computed (0 dynamic offsets in
1 035 shaders with tables). 120 + 95 shaders read no table; the rest read 1 (488), 2 (218) or 3
(12) tables, 56 flattened dwords on average, at most 192 (pixel). So for Bloodborne a table walk
is "copy these constant offsets of the table the user data register points at", and the only
way it fails is the pointer itself: null or not mapped, as at Ebrietas.

**Control flow.** Branches come from the PSSL compiler's EXEC scopes (1 392 `s_and_saveexec_b64`,
1 903 `s_cbranch_execz`) and scalar conditions (`s_cbranch_scc0/1`, `vccz`); no `s_setpc`
jumps except the fetch shader call (`s_swappc_b64`, 297 vertex shaders). 836 of 838 shaders are
structured in the sense below: every graph is reducible, all 38 loops (31 shaders, two nested
twice) leave to one place. The 2 exceptions are compute shaders of ~1 000 blocks and 560 branches
(`6_000000002da7fe60`, `6_0000000042f2a521`), each with 2 branch regions entered from the side.

**Resources, most per shader** (player captures):

| stage | constant buffers | texel buffers | images | samplers | flattened dwords |
|---|---|---|---|---|---|
| pixel | 9 | 3 | 16 | 8 | 192 |
| vertex | 6 | 2 | 1 | 1 | 36 |
| compute | 3 | 4 | 4 | 0 | 60 |

No storage buffers other than the engine's own (flattened user data, GDS counters); buffers
written only by compute shaders (14) and storage images only there (10). Constant buffers are read
only with scalar loads (`s_buffer_load_dword*`) and are uniform buffers already
(`BB_NATIVE_SHADERS`). Vertex inputs come through `tbuffer_load_format_*` from the fetch shader.

**Not used:** fp64, image gathers and size queries, FMASK, storage buffers, EXEC read or written
as a value. **Rare:** LDS (35, only in the tessellation stages), `ds_swizzle` lane operations (30
pixel shaders), `v_readlane`/`v_writelane` (12), ballots (3), fp16 conversions (11), GDS
append/consume counters (5 compute shaders), `image_store` (10). Per shader and per opcode:
`bb-shader-lab --census <folder> report.csv` writes `report.csv` and `report-gcn.csv`.

## The path from GCN to SPIR-V

Kept from the inherited compiler, because they are data, not design: the GCN decoder tables
(`frontend/decode.cpp`, `format.cpp`), the instruction semantics of the translator where the lab
shows them right, and the SPIR-V emission helpers. Replaced, in this order:

### 1. Resource tables without a walker that faults

A shader finds its constants and descriptors (T#, V#, S#) through pointers: the user data hold a
table pointer, the table holds values and further pointers. The current pass copies every value
the shader reads from those tables into the "flattened user data" (`flatbuf`), which the shader
then reads as a uniform buffer and from which the CPU reads the descriptors to bind. That
contract stays: the GPU never chases the game's pointers, and binding needs the descriptors on
the CPU anyway. What changes is how the tables are read:

- **The table plan** (`shader_recompiler/native/table_plan.{h,cpp}`): the walk as data, one entry
  per flattened value: which table it is in (a user data register, or the flattened value that
  is the table's pointer) and at which offset (a constant, or a small postfix expression over
  flattened values for computed offsets). It is built from the program right after
  `FlattenExtendedUserdataPass`, so it names exactly the places the shader reads.
- **Checked reads.** `TablePlan::Run` asks a `readable(address, bytes)` callback before every
  read of the game's memory. A table that is null (below 64 KiB) or not readable reads as zeros,
  its subtree too: the draw runs with null descriptors, as on the console, instead of faulting or
  being skipped. Nothing in it can fault, so it needs no exception handler.
- **Data, not code.** A plan can be written to the pipeline cache and the shader catalog as it is.
  Today the cache stores walker machine code and copies it back into an executable buffer.
- **Bloodborne's case is the simple one.** The census found only tables one level below the user
  data at constant offsets. The plan keeps nested tables and computed offsets (the walker supports
  them, and a plan that is wrong must not be possible to build), but the engine's version of
  `Run` can check each table once (its highest offset is known) and copy runs of dwords, cheaper
  than the walker's per-dword loads. A shader whose plan has computed offsets would keep the
  walker; none does.

Later (with "descriptors on the GPU", step 3 of the emulation plan) the shader could read
constants straight from the game's memory, which the PC memory model already maps for the GPU;
the plan then shrinks to the descriptors the CPU must bind. That needs the GPU to survive null
and stale pointers itself, so it comes after this.

### 2. Structured control flow without goto variables

The PSSL compiler emits structured code: ifs as EXEC mask scopes (`s_and_saveexec_b64` ...
`s_cbranch_execz` ... restore), loops as back edges with `s_cbranch_scc*`/`vccz`. The current CFG
already turns EXEC scopes into blocks with per-lane conditions (`SplitDivergenceScopes`). On that
graph the native structurizer does structural analysis directly:

- dominators and post-dominators of the block graph;
- a loop per back edge (its header dominates its latch), `break` for its exit edge, `continue`
  for its back edges;
- an if per two-way branch, its arms ending at the branch's immediate post-dominator;
- one SPIR-V selection or loop construct per region, with the merge and continue blocks SPIR-V
  requires, and no flag variables.

The census measures whether this covers the game: a shader qualifies when its graph is reducible,
each loop leaves to one place, and no branch region is entered from the side (`irreducible`,
`multi_exit_loops`, `side_entries`). 836 of 838 do. The two large compute shaders that do not
keep the current structurizer (chosen per shader at compile time, reported by the lab), so the
change never makes a shader fail to compile. EXEC scopes stay per-lane ifs, as the current
translation makes them; the census found no shader that reads or writes EXEC as a value, so no
lane-mask emulation is needed outside the few ballots and lane reads, which keep their subgroup
operations.

### 3. Exact descriptor layouts

Each stage gets bindings of its own: the fragment stage from 0, the vertex stage from a fixed base,
so no module depends on its partner stage and no vertex module is specialized on a binding offset.
The census bounds it: a pixel shader uses at most 9 constant buffers, 3 texel buffers, 16 images
and 8 samplers plus the flattened user data (37 bindings), a vertex shader at most 6 + 2 + 1 + 1
+ 1, a compute shader 3 + 4 + 4 + 1. Fragment bindings 0-39 and vertex bindings from 40 leave room
under the engine's limits (`NUM_BUFFERS` 40, `NUM_IMAGES` 64). Constant buffers stay uniform buffers in dword
layout (`BB_NATIVE_SHADERS`, `BB_UBO_*`), the flattened user data a uniform buffer. The layout of
every module is then known from the shader alone, which is what precompiling (4) and pipeline
libraries need.

### 4. Precompiled per GPU from the shader catalog

The catalog (`--export-catalog`) holds each shader's draw state without code; `--seed-cache`
takes the code from the player's own game files and compiles. With (1)-(3) each module depends
only on the shader, the profile of the GPU and its catalog state, so `bb-start` compiles the whole
catalog once per GPU and driver (the cache recompile does this today for the states a player has
already met), stores SPIR-V, plans and pipeline keys, and the warm-up builds the pipelines before
the game starts. A shader missing from the catalog still compiles in play, as now.

## Checked against the current compiler

Every step is compared per capture in the shader lab, without the game:

| Step | Lab check |
|---|---|
| 1 table plan | `--tables`: the walker against the plan over null tables, synthetic tables and the captured tables rebuilt from the plan |
| 2 structurizer | IR of both structurizers interpreted on the same inputs; `spirv-val` on the output |
| 3 layouts | resource interface of both modules equal apart from binding numbers; `spirv-val` |
| 4 precompile | `--recompile-cache` over the catalog: every pipeline key has its modules |

and then in the game in a verify mode (both paths run, differences logged) before it is the
default, as with `BB_NATIVE_CALLS`.

## Step 1: the table plan and its lab check (done)

`shader_recompiler/native/table_plan.{h,cpp}`, built in `recompiler.cpp` (the lab takes a copy
through `TablePlan::lab_target`), and `bb-shader-lab --tables <folder> [report.csv]`. Per capture,
in a child process:

1. **Null tables**: user data all zero. Walker and plan must both give zeros.
2. **Synthetic tables**, two seeds: a 64 MiB region below 4 GiB in which every dword is the
   address of one of its 1024 64 KiB slots (pseudo-random), so every pointer the walker reads
   lands in the region and a read one dword off gives another value. The plan's reads go through
   `VirtualQuery`, the walker's faults through the game's own fault handler, so both read the
   same memory.
3. **Unmapped tables**: user data pointers aimed at reserved memory that is not committed, above
   the null range. The walker faults on every read (its handler gives 0), the plan refuses every
   read; both must give zeros.
4. **Captured tables**: the flattened values the walker read in the game (the capture holds them)
   are put back into fresh tables at the places the plan names, pointers aimed at the copies (a
   table that was null in the game stays null). The walker must read the captured values again,
   and the plan must give what the walker gives.

Results over both folders (1 035 captures with tables; 215 have none; the 18 old tessellation
captures do not compile):

| | player | dev install |
|---|---|---|
| pass (5 comparisons each) | 718 | 317 |
| fail | 0 | 0 |
| walker faults on unmapped tables / plan reads refused | 33 260 / 33 260 | 11 824 / 11 824 |
| captured nonzero dwords read again from the rebuilt tables | 24 517 | 8 851 |
| a plan with one offset moved by a dword caught | 718 of 718 | 317 of 317 |

The unmapped case is the one the null check in the walker does not cover: each of those 45 084
faults is an exception and a skipped draw in the game today; the plan reads zeros without one.
A last check proves the comparison can fail: a copy of each plan with one offset moved by a dword
must give different values on the synthetic tables, and does for every shader.

## Step 1b: the engine runs the plan (done, off by default)

- **Every compile builds the plan** (`recompiler.cpp`), into `Info::table_plan`. The pass's own
  read of the tables waits for it (`Info::tables_pending`), so a compile reads them once, the way
  the mode says.
- **`BB_NATIVE_TABLES`** (`TablePlan::GetMode`, logged once as `Native shaders: resource tables
  read by ...`):
  - `0` or unset: the walker, as before;
  - `1`: the plan only;
  - `verify`: the plan's values are used and the walker runs beside it into a copy. Each
    difference is logged (`table plan and walker differ`, first 32), and so is each draw the
    walker would have skipped (`the walker faulted N times`, first 16). The walker's faults still
    cost their exceptions in this mode; the draw is not skipped.
- **Where it runs**: `Info::RefreshFlatBuf` (the pipeline selection's per-draw read) and the draw
  pipe's check that the tables did not change before stage B (`vk_rasterizer.cpp`, which no
  longer needs fault recovery when the plan reads).
- **`readable`** is the engine's mapping table, `MemoryManager::ClampRangeSize` (its cached
  region per thread makes the common case a compare), set in `shim/bbgpu.cpp`. A table is checked
  once over the span of its constant offsets, then copied without a check per dword.
- **The pipeline cache** keeps the plan at the end of the shader record (`TPL1`), after the walker.
  A record of an older engine has none: that shader runs the walker in every mode until the cache
  is recompiled (`--recompile-cache`, which `bb-start` runs when the compiler changes), so no cache
  starts again. A plan read back is checked before it can run (parents and expression operands in
  bounds, the expression stack balanced).
- **Lab**: `--tables` runs every check on the plan as written to and read back from a cache
  record.

Still to do once verify is clean in the game:

- `BB_NATIVE_TABLES=1` by default; then the walker generator (Xbyak, the 32 MiB code buffer), its
  fault handler and `srt_walk_faults` go, and the cache stops storing walker code.

Its check in the game: Ebrietas' fight with `BB_NATIVE_TABLES=verify` (launcher, Extra
variables): no `differ` lines in the log, her particles drawn, frame rate as with the walker
before the null fix. The game is started only after asking.

## Compile speed and output quality (2026-10-22)

A pass over the whole compiler for speed (translation and the driver's pipeline builds) and for
the GPU time of what it emits. Every change below was measured on the dev cache's 620 shader
states (`out\BloodbornePC\user\cache\CUSA03173`, NVIDIA profile) and, for the output, on the
real driver (RTX 5060, driver 617.42).

### How it is measured

- **`bb-shader-lab --bench <cache folder> [rounds] [report.csv] [--spv-dir DIR]`** (new): every
  shader state of a cache compiled in one process, as `CompileModule` does, with the cache's
  profile. Per shader the fastest round of each phase (the `Shader translation:` phases of
  `compile_stats.h`), IR and SPIR-V size, a hash of the SPIR-V and whether the specialization
  still matches the stored one. `BB_LAB_PROFILE="field=0,field=1"` changes profile switches for
  an experiment (also in `--one` and the folder run). Nothing in the cache is written.
- **`bb-shader-gpu-stats`** (new, `tests/shader_gpu_stats.cpp`, built by hand:
  `clang++ -std=c++20 -O2 tests/shader_gpu_stats.cpp -lvulkan-1 -o bb-shader-gpu-stats.exe`):
  builds a pipeline for each module on this PC's driver and reads the driver's statistics
  (`VK_KHR_pipeline_executable_properties`; NVIDIA reports binary size, registers, inputs and
  outputs per stage), and how long `vkCreate*Pipelines` took without any cache (each module's id
  bound is raised by a per-run number, so NVIDIA's disk cache misses too). `[threads]` builds on
  several threads, as the warm-up does. `tests/shader_gpu_stats.py <spv dir> <work dir>` writes
  its manifest from `spirv-cross --reflect`, with a partner stage per vertex or pixel shader
  (`glslangValidator`) that writes every input the pixel shader reads, or reads every output of
  the vertex shader, so the driver drops none; `--compare a.csv b.csv` sums the module's own
  stage per kind.
- The driver's statistics repeat exactly from run to run; its compile times vary by 5-30% on
  this PC (other builds running), so compile time is compared only back to back.

### What the output costs on NVIDIA (measured)

| Experiment (pixel shaders unless said) | NVIDIA binary | registers |
|---|---|---|
| constant buffers as storage buffers (the legacy compiler) against the native compiler | +18.9% (vertex +15.6%, compute +9.0%) | +6.4% (vertex +28.8%) |
| every input interpolated by the hardware, not GCN's formula (upper bound) | -8.7% | -5.6% |
| `NoContraction` dropped (the driver may fuse a multiply and an add) | -10.4% (vertex -14.0%, compute -7.4%) | -0.3% |
| the same in vertex shaders, with `Invariant` on the position | vertex -4.3% | vertex +1.6% |
| legacy multiply as a plain multiply (upper bound, not exact) | -6.8% | -1.7% |
| legacy multiply in DXVK's form, `(b==0?0:a)*(a==0?0:b)` | +2.1% | +0.8% |
| legacy multiply as `min(|a|,|b|)==0 ? 0 : a*b` | +1.6% | 0 |
| `x*x` without the zero check | 0 | 0 |
| `SignedZeroInfNanPreserve` dropped (not exact) | 0 | 0 |
| `spirv-opt -O` over every module | -0.1% | -0.2% (compute -5.5%) |

So on this driver:

- **The native compiler's uniform buffers are the right choice**: storage buffers cost 16-19%
  more code and many more registers. The beta 9.8 frame-rate loss in dense scenes did not come
  from the shader code.
- **Manual interpolation is the largest single cost** after the constant buffers. shadPS4 added
  it for Bloodborne (PR 1644): instanced objects pass their instance index as a float varying
  and truncate it; NVIDIA's interpolation of three equal values is not always exact, and the
  objects flickered. It is applied to every input of every pixel shader.
- **`NoContraction` on every float operation** (GCN's `v_mad_f32` is unfused) costs 10% of the
  pixel shaders: NVIDIA emits FMUL and FADD where it would emit one FFMA.
- **GCN's legacy multiply** (`v_mul_legacy_f32`: 0 times anything is 0) costs 6.8%, but the
  NVIDIA driver does not recognise any exact form of it (Mesa turns DXVK's form into one
  instruction on RADV and NVK, `nir_opt_algebraic.py` "open-coded fmulz", and DXVK says NVIDIA
  565+ optimizes its pattern; with `SignedZeroInfNanPreserve` and `NoContraction`, which the
  emulation needs, it does not here). The current form is the smallest exact one; unchanged.
- **spirv-opt** buys nothing the driver does not do itself (upstream shadPS4 issue 1138 asked
  the same and never adopted it).

### Changes

1. **Pixel shader arithmetic may be fused** (`Profile::fragment_exact_fp`, `BB_FS_CONTRACT=0`
   turns it off): a pixel shader's float add, multiply and fma lose `NoContraction`, except in a
   shader that writes depth. Results differ in the last bit at most; integer-valued floats stay
   exact (an fma of integers below 2^24 is exact either way). Vertex shaders keep it: positions
   must come out bit for bit the same in the depth pre-pass and the colour pass, and with
   `Invariant` on the position the saving falls to 4%. Pixel shaders -10.4%.
2. **Hardware interpolation where the shader cannot tell** (`ir/passes/
   hardware_interpolation_pass.cpp`, `Profile::manual_interpolation_all`, `BB_INTERP_HW=0` turns
   it off): with manual interpolation, an input goes back to the hardware when every read of it
   is GCN's two-step formula with the hardware's barycentrics and no use needs the exact bits.
   The uses are followed through float arithmetic, selects, phis, bit casts and lane shuffles;
   conversions to integers, floor/ceil/trunc/fract/rounding, comparisons with integer constants
   other than zero or with computed values, and any other integer or boolean result keep the
   formula. Comparisons with zero (the legacy multiply's check: a value zero at the three
   vertices is exactly zero either way) or with a constant between integers, `|a|` against `|b|`
   (a cube face: the faces share their edge texels), image sampling, exports (also as 16-bit
   floats) and stores end the walk. Two inputs of one vertex output keep what they had (see
   below). 880 of the dev cache's 1201 per-vertex inputs go back to the hardware; the 321 left
   include every truncated one (the instance index). Pixel shaders -6.1% code, -4.2% registers.
3. **Both together: pixel shaders -16.3% NVIDIA code, -3.7% registers** (284 of 296 changed),
   vertex and compute unchanged; SPIR-V -12.2%, IR -5.2%. `spirv-val` passes on every module.
4. **The warm-up builds pipelines on several threads** (`BB_WARMUP_THREADS`, default the
   hardware threads less one, at most 8; `vk_pipeline_serialization.cpp`). The cached
   pipelines' state is still rebuilt on the main thread (it shares selection, programs and the
   archive); their creation by the driver is collected (`WarmupJobs`, the same owned create infos
   as the compile threads' jobs) and run in parallel, and the game starts once all are built, as
   before. `warmup.running` lists every pipeline being built; after a driver crash the next start
   leaves all of them out. A pipeline the driver refuses counts as a damaged cache entry, as
   before. Measured with `bb-shader-gpu-stats` over the 601 modules, cache cold: 13.9 s on one
   thread, 10.0 s on 2, 6.0 s on 4, 3.1 s on 8, 4.2 s on 15 (this PC has 12). The warm-up
   after every compiler or driver change (this one too) waited 100-300 ms per pipeline, one
   after the other.
5. **Faster translation**: the local CSE behind `InverseBallotEliminationPass` compared every
   bit cast with every earlier one in its block (a GCN register holds bits, so blocks hold
   hundreds); it now looks them up by hash (196 to 93 ms over three rounds of the cache). The
   pass's other work and every module's SPIR-V are unchanged.
6. **Deterministic SPIR-V**: the two large compute shaders (`2da7fe60`, `42f2a521`) compiled
   differently from run to run (`PhiSimplificationPass` grouped SCCs in a hash table's order,
   and the hashes hold block addresses). Seeded in the SCCs' order (upstream shadPS4 #5364):
   six runs, one result.

Translation of the 620 states: 640-650 ms before, 600-640 ms after (one round on this PC varies
by 10%), with the new interpolation pass in it. The phases (ms, fastest of three rounds):

| | decode | translate | SSA | resources | passes | structurize | late passes | SPIR-V |
|---|---|---|---|---|---|---|---|---|
| before | 29.6 | 77.7 | 174.4 | 48.8 | 84.0 | 14.7 | 161.1 | 49.7 |
| after | 30.9 | 83.4 | 217.3 | 49.1 | 49.2 | 14.6 | 151.4 | 44.4 |

(`SSA` now holds the interpolation pass, ~30 ms of it.) Per pass, the largest left are the two
SSA rewrites (~180 ms a round), dead code elimination (~110 ms) and the translator (~80 ms):
the IR's own costs (a heap node per use and per instruction), not an algorithm to fix in an
afternoon. A first-seen shader costs ~1 ms of translation on the GPU command thread against
100-300 ms of driver compile on the compile threads, so the driver side is where the waits are.

Driver compile time per pipeline did not change measurably with the smaller pixel shaders
(601 pipelines on one thread, back to back: 15.1 and 15.5 s before, 14.8 and 14.8 s after).

### Checked

- `bb-shader-lab --recompile-cache` on a copy of the dev cache: 617 recompiled, 3 left out (the
  same 3 as with the old compiler: runtime info changed), 24 orphan keys removed (as before);
  every module passes `spirv-val`. The two new profile fields sit in what was padding at the end
  of `Shader::Profile` (bytes 69 and 70, zero in every stored `profile.bin`), and false means
  the new behaviour, so no player's cache is started again: bb-start sees the compiler changed
  (`gpu-cache.id`) and recompiles it.
- The player's 856 captures (`--native`): 0 new failures. 30 pixel shaders fail `spirv-val` with
  "conflicting input location": two GCN inputs read one vertex output with different
  interpolation. They fail identically with the old compiler, and the interpolation pass leaves
  such inputs as they were (their decorations compared, all 30 equal).
- `--curves` over the dev captures: 0 failures. `pipeline-compile-test` passes.

### Not done, for the game test

- **In the game, before release** (the game is started only after asking): frame rate at the
  usual spots (Central Yharnam, Forbidden Woods) against `BB_FS_CONTRACT=0 BB_INTERP_HW=0`; the
  instanced objects that flickered on NVIDIA (coffins on the bridge, shadPS4 PR 1644) must not
  flicker; the start after the update must show `GPU cache: N of N cached pipelines built at
  the start in X s (... then the driver on 8 threads, BB_WARMUP_THREADS)`.
- The 30 conflicting-location pixel shaders: one vertex output read with two interpolations
  needs one per-vertex input and both interpolations computed from it (a compiler fix of its
  own; drivers accept it today).
- Vertex shader contraction (14%) with a proof that the depth pre-pass and colour pass compute
  their positions with the same instructions.
- Shader translation still runs on the GPU command thread for a shader first seen in play.

Sources read for this pass: shadPS4 PRs 1644 (manual interpolation for Bloodborne's flicker),
3245 (guest barycentrics), 5364 (phi order) and issue 1138 (spirv-opt); Mesa
`src/compiler/nir/nir_opt_algebraic.py` (open-coded `fmulz` patterns, `fmulz(a, a)`) and
`nir_opcodes.py`; DXVK `src/dxso/dxso_compiler.cpp` (`emitMulOperand`, d3d9 float emulation) and
its changelog (strict emulation on AMDVLK and NVIDIA 565+); AMDVLK issue 346; the Vulkan spec of
`VK_KHR_pipeline_executable_properties` and `Invariant`.

## The lifter (Phase G, 2026-10-30)

`gpu/shim/lifter/` is our own path from GCN to SPIR-V: a decoder, structured control flow without
flags, typed SSA and a SPIR-V writer, with no code of the recompiler. It is off by default.
`LANTERN_LIFTER=1` uses it in `CompileModule` and falls back to the recompiler's module when it
declines. The recompiler still runs first in that path, because today's pipeline code needs its
`Info`.

**Where the shaders come from.** Every shader of the dev cache comes from the game's files: 559
programs, 0 missing, 0 with different code. The files hold 9,847 distinct programs, 4,376 in the
shader bundles and 5,471 in the eboot and its modules (`tools/re/shader_files.py`), so they can be
compiled ahead of time.

**Numbers** (dev cache, 1034 states; `bb-shader-lab --lift`, `tests/spirv_diff.cpp`):

| | recompiler | lifter |
|---|---|---|
| Lifted | | 1034 of 1034 |
| Translation | about 1400 ms | about 260 ms |
| SPIR-V words | 5.88M | 4.82M (-18%) |
| `spirv-val` | | 1034 pass |
| Same result over 24 inputs | | 1033, 0 different, 1 cannot run (the old module's function call) |

Every family's profile (nvidia, gtx10, rdna, polaris, intel, storage-buffer constants, vec4 uniform
buffers): 1034 lift, all pass `spirv-val`, 0 different over 8 inputs. Forced user clip planes and
the HDR tone curve extension (`BB_HDR_SCENE_CURVE=1`, 2 composite states): 0 different. About 89%
of the blocks ran in the comparison. With a profile that offers both AMD and KHR barycentrics the
recompiler reads the raw barycentrics one component too far (40 states); the lifter does not.

Driver, RTX 5060 (621.1), 997 pipelines, 0 failed, lifter against recompiler:

| Stage | Binary | Registers | Pipeline create |
|---|---|---|---|
| vertex | -0.2% | 0.0% | -19% |
| pixel | -0.3% | 0.0% | -9% |
| compute | -1.7% | -0.5% | -30% |

**All the game's programs.** Discover mode (`Input.discover`, `bb-shader-lab --lift-files`) lifts
from the code alone and assigns its own slots and flat table, which is the direct-slot design for GX
draws: 9,847 of 9,847 in 3.1 s, all pass `spirv-val`. These states are synthetic, so this proves
coverage and not behaviour.

**Vulkan validation.** The pipelines were built on lavapipe with the Khronos layer (not through
`tools/validate_pipelines.sh`, which starts the game). nvidia, gtx10 and intel: 997 of 997 built.
rdna and polaris: 802 built, 195 failed because lavapipe lacks the AMD extensions (recompiler on
rdna: 801 built, 196 failed). The lifter added one kind of message, and it is not a defect: it
declares `SPV_AMD_gcn_shader` where the recompiler uses the instruction without declaring it. The
other messages are lavapipe's missing extensions and its limit on motion vector locations, which the
recompiler also hits. The recompiler has 3 kinds the lifter does not: an undeclared `DenormPreserve`,
its denorm requirement, and a fragment input the vertex stage does not write.

**Not done.**

- In-game GPU time, and any run in the game: the lifter stays off by default until then.
- Driver numbers on anything but the RTX 5060. AMD, Intel and older NVIDIA are checked only through
  their profiles, `spirv-val`, the comparison and lavapipe.
- `tools/validate_pipelines.sh` with the lifter, and `--recompile-cache`, which does not use it yet.
- Fallback gaps: image atomics, the SAMPLE_CD family, compute LDS (none in the census).
- The renderer session's GX draws must drive discover mode, with runtime state from GX objects.

**When the recompiler can go.** Not yet. `video_core` still takes `Info`, `resource.h`,
`runtime_info.h` and the fetch shader parser from it, and `LANTERN_LIFTER` must first become the
default on measured game numbers. It can be deleted once the renderer no longer needs those types.
