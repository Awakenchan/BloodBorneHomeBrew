# Temporal upscaling and frame generation — frame analysis and plan

## Native TAA and missing FSR 4 assets (2026-10-01)

`upscaler=taa` / `BB_UPSCALER=taa` is a separate native-resolution temporal AA
mode, available live in the launcher and overlay. It uses the existing jitter and
camera/object motion vectors, reprojection, depth rejection and 3×3 neighborhood
clipping in one compute pass with two ping-pong RGBA16F histories. History stores
depth in alpha; the scene's original alpha is preserved by the HDR merge. HUD is
composed afterwards and never enters history. Output changes and AA/provider
changes reset history. TAA uses Native AA dimensions while preserving the saved
FSR preset, and does not allocate an FSR context or execute an FSR model/RCAS.
It still adds GPU work compared with disabling temporal AA; native 4K raster cost
also remains. The explicit `BB_RENDER_RES` compatibility path must be unset.

FSR Native AA is not a fast pass-through: it renders at the output resolution and
also executes the temporal reconstruction (including the model for FSR 4) and
optional sharpening. It can therefore be slower than no AA at the same output.

FSR 4.1.1 assets have separate 1080p/2160p tiers and standard/Ultra models:
`t1080_m0`, `t1080_m1`, `t2160_m0`, `t2160_m1`. Outputs above 1080p need the 2160
tier, irrespective of the preset. A missing `t2160_m0/spd.spv` means missing
assets, not a GPU feature failure. These assets are not bundled: build the full
set with `tools/fsr4cap/build_assets.sh` and put it in `BB_FSR411_DIR` or the
packaged data directory's `fsr4_411`. Fatal asset failures now select FSR 3.1 in
the live settings, so the menu shows the active provider, retains the error, and
allows retrying FSR 4 after changing the output/installing assets.

`taa-shader-test` checks real SPIR-V output for accumulation, clipping, depth
disocclusion, invalid history and out-of-frame motion on Vulkan (Lavapipe works).
`out/taa-appimage-build.log` records these checks, settings round trips and 64
Python tests. `out/taa-live-validation.log` and `out/taa-appimage-validation.log`
check TAA at 1080p/720p/1440p/4K, TAA ↔ FSR 3, and a missing 2160-tier FSR 4.1.1
model in one gameplay process, including output captures and camera movement.
`out/taa-launcher-validation.log` checks actual GTK controls and saved settings.
TAA has not yet been tested on the tester's GTX 1060.

## Startup patch is the default again for outputs other than 1080p (2026-10-02)

The live path below made the Steam Deck and a GTX 1060 + 4-core Haswell drop to 7–8 FPS
(release 0.1 AppImage: 40–50 FPS on the Deck). With guest allocations at 1920×1080 the game's
post-processing stays at 1080p even for 720p output, guest compute passes resolve scene proxies
back to 1080p every frame (RX 7800 XT, 720p FSR 3 Performance on 4 cores: 826 vs 576
draws/frame, 185 vs 210 FPS), and on GPUs without shader stencil export every depth/stencil
copy took nine draws.

- `run.sh` again patches the game's render size for outputs other than 1080p (as in 0.1) and
  marks it with `BB_AUTO_RENDER_RES=1`, so an in-game restart recomputes it. Preset and output
  changes then need a restart (the menu offers it). `BB_LIVE_RES=1` selects the live path.
  1080p output and TAA keep the live path.
- Depth/stencil scene proxies need `VK_EXT_shader_stencil_export` again; without it these
  targets stay native (as in 0.1). `BB_SCENE_STENCIL_BITS=1` still forces the portable
  resampler for tests.

## Live resolution changes (2026-10-01)

The in-game output selector now resizes 720p/1080p/1440p/2160p host targets at a
display-pass boundary. Presets use `output / scale`, including raster sizes above
1080p (4K Quality: 2560×1440; Native AA: 3840×2160). Guest allocations stay at
1920×1080; `run.sh` no longer inserts a resolution patch for normal output choices.
The explicit `BB_RENDER_RES` compatibility override is retained.

`SceneTargets` resolves and retires old proxies before changing size. FSR resources
and history are rebuilt for the new input/output, with completion waits for both
FSR 3 and FSR 4. The 1080p path keeps HDR reconstruction before game post;
other outputs reconstruct tonemapped scene proxies at the first Scaleform pass,
then draw HUD/menu geometry into the output-size target. The display buffer is
resized too. Movie shader identification prevents a native-size fullscreen post
pass from being mistaken for UI.

Guest compute passes retain their native dispatch sizes and can resolve proxies
back to 1080p. This change does not scale every post-processing pass; performance
and intermediate detail can differ from the old startup-patched render sizes.

Verification: `out/dev/live-resolution-summary.log` switches output, preset and FSR
off/on in one PID. `BB_PRESENT_DUMP_TRIGGER` captures the completed host display
buffer (including HUD) into `BB_DUMP_DIR`. Captures confirm actual 720p/1440p/2160p
buffers. `scene-resolution-test` covers color/depth/stencil round trips through
proxies both smaller and larger than the guest allocation. `BB_PRESET_FILE` accepts
`preset [output-index [upscaler-index]]` for scripted menu-equivalent changes.

Frame analyzer: `BB_CAPTURE_TRIGGER=<file> BB_CAPTURE_DIR=<dir>`; creating the file records
the next frame (boundary: the pass writing a display buffer) — passes, targets, shaders,
sampled textures, and the first 1 KiB of bound constants for small passes and first draws.

## Bloodborne's frame (1920x1080, Hunter's Dream / Nightmare)

| Passes | What |
|---|---|
| first pass of a frame | copy of the previous UI target into the display buffer (sRGB) |
| shadow | 4096x4096 D32 depth |
| G-buffer | 6 targets 1920x1080 (RGBA8 x3, sRGB albedo, B10G11R11, RGBA16F) + D32S8 depth |
| lighting | light volumes into two B10G11R11 targets |
| scene color | RGBA16F 1920x1080, also forward/transparent draws and effects |
| authored fog | scene-combine pixel shader `32e47788`: depth-based exponential distance and height fog using 96-byte parameter records; additional textured transparent effects follow |
| half-res effects | RGBA8 960x540 with half-res depth |
| post | combine/bloom pyramid in RGBA16F / B10G11R11 |
| tonemap | into RGBA8 1920x1080 (the UI target) |
| game AA | ping-pong RGBA8 1920x1080 after tonemap (to be skipped when upscaling) |
| UI | stencil-masked draws over the same RGBA8 target |

There is no velocity buffer, also with the camera moving: motion vectors are computed.

### Directional volumetric-lighting prototype

`effect_volumetric_light` adds a separate, live-toggled scattering layer without
replacing or scaling the authored fog. After shadow resolve `f55643de`, a half-resolution
compute pass uses that draw's inverse projection, cascade transforms, shadow map and
shadow-distance fade. It marches 32 samples toward visible opaque surfaces, up to 40 m,
with four depth comparisons per sample. Sky rays also stop at 40 m; their direction is
reconstructed from a finite projected point, not the potentially infinite far plane.
The pass snapshots full-resolution opaque depth into an owned R32F image. At the first
`ccbf44a6` HDR post copy, after the authored fog, sky and transparent draws, a depth-aware
upsample adds scattering to HDR RGB, preserving alpha. Sky and surface samples never mix.
`32e47788` identifies the HDR scene; its saved image ID is UID-checked at composite and
cleared on the next G-buffer. Guest buffers and sampled guest views are consumed immediately
after their source draws, not retained for the late composite. No upscaler or camera-motion
tracking is required.

The in-game overlay's Picture section has a **Volumetric fog intensity** slider
beside ambient occlusion strength. It applies live from 0 to 3 times the calibrated
scattering strength, with 1 preserving the default appearance and 0 adding no
scattering. It leaves density, range, tint, celestial phase and the game's original
fog unchanged. Both half-resolution integration and thin-surface repair use the
same multiplier. It requires the volumetric-lighting effect, not Picture tuning.
The value is saved as `graphics_volumetric_intensity` in `lantern.ini`; resetting
the Picture section restores 1. The multiplier also scales `BB_VOLUMETRIC_STRENGTH`
when that diagnostic environment override is set.

Thin depth-tested surfaces can be absent from all four half-resolution bilateral
samples. A full-resolution repair pass uses the same sample weights and raymarches
only pixels whose total weight is below `1e-5`, rather than leaving those pixels
without scattering. It runs immediately after integration using the same depth and
shadow inputs. Its owned RGBA16F output receives the shared atmosphere tint and celestial
phase correction before the late composite. Supported half-resolution samples retain
their existing result. This targets depth-represented cutouts such as grass and leaves;
it does not resolve separate alpha-blended layers that do not write opaque depth.
The repair alone partially improved the reported foliage gaps. Sharing the atmosphere
tint instead of selecting it from each receiving surface's LightSet then completely
resolved the remaining mismatch in the user's test scene. Performance impact has not
been measured separately.

After directional lighting `d92266b9`, a tint pass reads its resolved 288-byte lighting
records. The highest-luminance bounded primary record supplies a shared atmosphere tint for
geometry, repaired cutouts and sky; the receiving surface's material/LightSet no longer
selects the fog colour. Words 16-18 are the primary directional diffuse RGB, used directly
by the original shader.
These are already resolved lighting values, so no additional gamma conversion or alpha
multiplication is applied. The fog strength is calibrated against unit-white scattering,
not the HDR surface radiance stored in these records. RGB is divided by
`max(1, max(R, G, B))`: sub-unit lights are unchanged and brighter lights retain their
hue without amplifying the scattering above its calibrated strength. This prevents
high-intensity backdrop/material lighting records from turning distant buildings into
bright silhouettes. The ray length still saturates at 40 m; distant surfaces do not
accumulate a longer column than the sky.
Scene-wide words 76-78 instead tint shadowed
lighting and are not the primary diffuse colour. The atmosphere anchor comes from the
current lighting table without parsing GPARAM files or hard-coding map names.
All records are reduced cooperatively per workgroup. Selection uses linear luminance
after the RGB bound, with lexicographic RGB tie-breaking, not the record's index.
The earlier first-populated-record rule flickered in Old Yharnam: two GPU captures
showed record 0 switching between moonlight RGB (0.449710, 0.830904, 1.600000) and
a dim region RGB (0.111474, 0.128867, 0.156326), while moonlight remained elsewhere
in the table. The new rule selects the same moonlight from both captured tables,
independent of packing order and duplicate records.
The user confirmed the updated selection removed the Old Yharnam camera-pan flicker.
It does not select a light from visible materials: screen-centre/grid selection caused
whole-sky colour and brightness jumps when panning across different lighting records.
For an unchanged set of lighting colours, tint is independent of camera orientation, screen contents
and geometry/sky coverage. Surface LightSet differences cannot create tint-shaped holes
in the added scattering. This is still a table-defined approximation; runtime record
indices have not been proven to map directly to GPARAM IDs, so it is not guaranteed to
select file ID 0.

The bounded atmosphere colour is blended 75% toward the sampled RGB from its
equal-luminance neutral grey (linear RGB luminance weights 0.2126/0.7152/0.0722).
This reduces strong blue and other colour casts without reducing luminance,
changing the intensity multiplier, or reintroducing surface LightSet selection.

The directional layer adds no additional extinction, animated density, or temporal history. Its
shadow visibility comes from the shadow transforms. The phase direction also defaults
to those transforms, with the verified Central daytime celestial override described below.
The cascade rows are stored in ordinary U/V/depth/divisor order: divide row 2 by
row 3, matching the original resolve shader. Swapping those last rows produces
reciprocal shadow depths above 1 and rejects valid scattering samples.
It fades out with the game's shadow coverage rather than assuming unshadowed air outside
the map. FogBank colours/density are not mixed into this layer; the original fog keeps its
own 96-byte records and distance/height blends. The added scattering remains an artistic
approximation; it can brighten interiors where
the directional shadow map lacks suitable occluders. Fixed spatial jitter and half-resolution
integration can show grain, and non-depth-writing transparent surfaces are not volume
termination surfaces.

### Central Yharnam daytime celestial phase prototype

The daytime prototype aligns angular scattering brightness with verified sun geometry.
It does not rotate shadow visibility or change tint, density, strength, range or the
game's original fog. Integration saves its inverse projection and original phase
direction in an owned 112-byte GPU buffer, resetting the same-frame match count.
After a candidate sky-dome draw, a single compute invocation fingerprints all 393 dome
vertex positions (word-wise FNV-1a, `0xf7004131`) and checks the centroid against the
daytime asset. It also checks every vertex's packed transform index is zero. Shader hashes and
index count are only a broad prefilter, never sufficient identification. Vertex data
comes from the actual GPU-bound streams, including their stride and aligned-offset
adjustments; the potentially stale CPU backing is not used for identification.

On a match, the shader verifies the actual dome palette entry selected by model
word 20 is identity. The daytime dome and disk are distinct nodes with baked world-space
vertices; nonidentity placement is not assumed to transfer between them. The shader
uses the verified daytime disk centre (-67.5080062, 99.7654301, 577.6338876), subtracts
the current scene camera, and uses the scene's world-to-view rotation.
Guest vertex/model/scene buffers are consumed immediately
after the draw. Only the resulting direction is kept in owned storage for the late
composite; no guest descriptors are retained across draws or frames. Normal rendering
uses no readback, CPU/GPU wait or previous-frame celestial history.

At composite, each half-resolution fog sample is multiplied by the ratio of celestial
phase to its original shadow-direction phase, before bilateral upsampling. This replaces
the phase, rather than adding another light or rotating the shadow-map coordinates.
Exactly one valid geometry match is required; absent, unrecognized, invalid or duplicate
matches retain the original phase. More than four eligible draw attempts also fall back.
CPU-side unsupported buffers and absent/excessive candidates produce bounded log messages;
GPU fingerprint rejection keeps the documented shadow fallback without claiming detection.
This path supports the 1,824-index, single-instance, zero-base-vertex daytime dome
draw only. Unlike the culled disk, this surrounding geometry remains drawn when
facing away from the sun, identifying the active sky afresh each frame. No cached sun,
timeout, camera-motion threshold or GPARAM-colour heuristic is used. Evening, night,
red moon and other maps do not match its geometry. Their dome position fingerprints
are distinct (`0x6d352c6f`, `0x070d76da`, `0xa26549c9` for the other Central variants).
Other vertex formats, placements and LODs are not
validated for automatic alignment and retain shadow phase. There is no GPARAM identity
mapping or per-variant override UI.

Central Yharnam's `900000`, `900010`, `900020` and `900030` sky models each have a separate
1,986-vertex sun/moon mesh (24-byte vertices; 11,904/7,140/2,376 indices by LOD). Their
model-space centres differ between day, evening, night and red moon. The normal texture
is `m24_00_moon_a`; the red-moon model instead uses `m24_00_moon_red_a`. Neither a model
centre alone nor the shared `b28f5a56`/`dda49e89` shader pair establishes world direction.

The temporary celestial GPU-readback and vertex-dump hooks used for the captures
below have been removed, including their explicit GPU waits and diagnostic buffer
accessor. The runtime geometry fingerprint remains necessary for stable celestial
alignment; it is rendering logic, not tracing. Existing opt-in engine diagnostics
remain available but are not enabled for normal launches.

The Central Yharnam research capture `frame_1793364982` contains one candidate
11,904-index draw sampling a 1024x1024 texture. Its GPU transform buffer starts with
identity rows, whereas its CPU backing dump is all zero. The same snapshot recovers
the camera at approximately (-194.159, -27.791, 64.352). Under that identity transform,
the day and evening model centres project to normalized image coordinates
(0.6070, 0.3904) and (0.6016, 0.3292), respectively (image Y downward). The two night
centres project behind this camera. Fitting the visible rim in the corresponding
2000x1125 screenshot gives a centre of about (1215.1, 437.9); the daytime model predicts
(1213.9, 439.2), within 1.8 pixels, whereas the evening prediction is about 69 pixels
away. This supports the daytime model for that view. The existing scene shadow vector
has about 37.5 degrees elevation versus 13.6 degrees for the daytime centre ray,
a direction difference of about 25 degrees.

A second, rotated-view capture (`frame_1793365413`) still reads identity model rows.
Its camera is approximately (-194.984, -27.807, 64.526), and the daytime centre now
projects to normalized image coordinates (0.4461, 0.3985). Its matching screenshot
has a visible rim centre of about (890.6, 445.3) at 2000x1125, versus the predicted
(892.2, 448.3), a difference of 3.4 pixels. Together these views support recovering the
daytime sun direction from the authored geometry and actual draw transform across
camera rotation. They do not yet validate the other sky variants or a general runtime
mesh-identification method. No celestial direction is wired into the rendering effect
at the time of those two research captures. Aligning the scattering phase does not rotate shadow-map
occlusion or make its shafts physically consistent with a different light direction.

Runtime capture `frame_1793365917` verifies the implemented path: the owned buffer
contains integer match count 1 and celestial validity 1. Its sun view direction is
approximately (0.129166, 0.015237, 0.991506), agreeing with independent reconstruction
from the captured camera and model centroid within 0.000002 per component. The
shadow direction remains approximately (-0.006951, 0.418095, 0.908377). The corrected
phase is about 25 degrees away from the original, while integration still uses the
original shadow map. The original disk-driven version visibly switched to fallback
when the game culled the off-screen disk; the dome-driven version replaces that
visibility-dependent identification.

Runtime capture `frame_1793366410` verifies the dome version with the disk visible:
one valid dome match, identity palette, view direction (0.141729, 0.064607, 0.987795).
Capture `frame_1793366453` has no 11,904-index sun draw at all, but still has the
1,824-index dome draw and exactly one valid match. Its recovered direction is
(0.115818, 0.297297, -0.947734), correctly behind the camera. Both agree with independent
reconstruction from the current scene constants within 0.000003 per component.
This validates off-screen operation without retaining any previous-frame source.

Environment overrides (read at startup):
`BB_VOLUMETRIC_LIGHT=0`, `BB_VOLUMETRIC_STRENGTH` (default 0.08, range 0..1),
`BB_VOLUMETRIC_RANGE` (40 m, 1..100), `BB_VOLUMETRIC_DENSITY` (0.025, 0.001..0.2),
`BB_VOLUMETRIC_ANISOTROPY` (0.4, 0..0.8), and `BB_VOLUMETRIC_STEPS` (32, 8..96).
The directional toggle is available in the overlay and launcher; the game's custom
options menu does not yet have a dedicated row. `BB_VOLUMETRIC_LIGHT=0` disables
both directional and point-light scattering; the remaining environment overrides
above affect only the directional layer.

### Unshadowed point-light volumetric fog

The Picture section's **Point-light fog intensity** slider independently controls
an additive 3D scattering layer: 0 disables it, 1 is the default, and 3 triples its
strength. It works with directional volumetrics, point shadows and Picture tuning
disabled. The value is persisted as `graphics_point_fog_intensity` in `lantern.ini`;
Picture reset restores 1. It does not change the game's original fog or surface lights.

After each supported unshadowed point-light batch, a full-resolution compute pass
consumes that draw's current view-space light positions, radii, diffuse RGB and
inverse projection. Sixteen-word light records store inverse radius **squared**
at word 3, not inverse radius. The shader intersects each camera ray with each
light sphere, clips to opaque scene depth (at most 100 m, also used for sky), and
integrates 12 midpoint samples over the surviving segment. It uses the original
radial attenuation `(1 - distance / radius)^2`, homogeneous density 0.025 and
view-ray transmittance. RGB is bounded to peak 4 and uses 75% saturation.
The accumulated RGB is added before tone mapping without changing scene alpha.

This is not a screen-space halo or screen-space shadow trace. Off-screen light
centres can contribute when their spheres intersect visible air; camera-inside
volumes are supported. There is no temporal history or source-position cache.
The full-resolution depth snapshot avoids the half-resolution foliage coverage
problem. Shared depth acquisition still relies on the game's normal shadow-resolve
pass, even when directional scattering is disabled.

Fourteen decoded pure point-batch shaders cover fixed counts of 1 through 12
lights; `9f9ef267` contains 12 unrolled records, not the 11 suggested by its
unused constant. Mixed spotlight and special/shadowed variants are excluded.
The source is the game's current light list: captures include off-screen centres,
but this does not guarantee that the game retains every light whose fog sphere
would intersect visible air. Live camera-edge behavior and performance need
in-game confirmation.

There is deliberately no light-to-air occlusion: foreground geometry terminates
the camera ray, but walls do not cast shadows into these point-light volumes.
Light can therefore leak into air on the camera side of a wall. This is the
accepted tradeoff for supporting non-shadowcasting lights without screen-space
visibility approximations or new point-shadow rendering.

## Scene constants (864 bytes, bound by most passes)

Signature: `[0]=3000 (far) [1]=1/3000 [4]=1920 [5]=1080 [6]=1/1920 [7]=1/1080`.

| Floats | Meaning |
|---|---|
| 8–19 | view matrix, 3x4 rows (rotation + translation); the only block that changes with the camera |
| 36–51 | inverse projection (0.700285 = 1/1.42799, 0.39391 = 1/2.53865) |
| 52, 57, 62, 63 | projection (rows 52–67), D3D depth 0..1: x 1.42799, y 2.53865, z 1.00002 / -0.0500679 (near 0.05, far 3000) |
| 112–175 | shadow cascade matrices |
| 180–191 | inverse view (camera to world), 3x4 (176–179: 2, 8, 15, 0) |

The previous frame's matrices are not there; the port keeps them itself.

## Plan

1. Find the scene constants every frame (signature), keep the previous view/projection.
2. Camera motion vectors: compute pass from depth and current/previous matrices into an
   RG16F target; debug view to check them. Object motion later (vertex shader replay with the
   previous frame's constants through the recompiler).
3. Jitter: sub-pixel offset of clip-space positions in the scene passes (recompiler).
4. Upscaler at the scene color stage (before post and UI): FSR 3.1 first (open, native Vulkan,
   also frame generation), then DLSS (native on Linux), XeSS/XeFG and OptiScaler through a
   loader for their Windows DLLs.

## Status

- Camera motion vectors: verified by reprojecting the previous frame (`BB_DEBUG_MOTION=1`,
  toggles 1<<22 reprojected frame, 1<<23 error map): static geometry matches.
- FSR 3.1 (`BB_UPSCALER=fsr3`, FireBurn/FSR-Vulkan submodule): scene color before the
  post-processing combine (compute shader 9a9cf8a9), 1:1, RGB written back (the game keeps data
  in the alpha). Toggle 1<<24 switches it off at run time.
- Jitter: viewport offset of scene geometry (drawn with the scene depth, not full-screen
  quads), Halton(2,3) 8 phases; sign checked by sharpness (correct 255, flipped 208, off 271 —
  1:1 jitter trades high-frequency aliasing for a slightly softer image). Toggle 1<<25 off.
- Frame-to-frame difference while standing still: ~33% lower with FSR.
- Missing: motion of animated objects (characters, cloth, foliage), reactive/transparency
  masks for particles and fog, render-resolution scaling, frame generation.


## 2026-10-01: FSR 4 did not work at 1440p/2160p output (flicker and shaking)

Video `video_2026-10-01_03-58-59.mp4` (3840x2160 output, FSR 4 Performance): every object flickered
and shook. The cause: in this mode the upscaler did not run at all while jitter stayed on, so the
screen showed the stretched scene frame, each frame shifted to its own Halton phase.

1. The UI pass was recognized by the exact size of the `BB_RENDER_RES` target (1916x1078), but the
   game allocates targets with an aligned height (1916x1080; the scene constants say 1916x1080
   too). `RunScaled` was never called (the log had no `UI: native composition`). Alignment of up to
   8 pixels is now allowed (`RenderTarget`), and the scene size for FSR comes from the scene
   constants (`CameraMotion::RenderSize`, `SceneSize`).
2. After that FSR 4 failed with `external image registration failed (-1000069000)`: `RunScaled`
   created new image views every frame, and FSR 4's registry holds eight. It now uses the same
   `CachedView`s as the Native AA path.

Checked with a dump (`BB_DUMP_TRIGGER=<file> BB_DUMP_DIR=<folder>`, `BB_DUMP_FRAMES`, 8 by default:
FSR's input, motion vectors and output, raw): with the camera still, the PSNR of consecutive output
frames is ~45 dB against ~28 dB for the jittered input; turning the camera and walking leave no
trails. FPS in this mode is ~107 instead of ~220: before, FSR 4 simply did not run.

Left: sprites (4 indices each) drawn into the scene colour with the scene depth (glows, small
lights) are still not jittered (the rule "≤ 6 indices = a full-screen pass").

## 2026-10-01: a faster FSR 4: the post pass (2.9 → 0.8 ms at 4K)

Measurements: `BB_FSR4_PROFILE=1` (the time of each FSR 4 pass, a provider patch in the submodule,
see `gpu/patches/fsr-vulkan`), the out-of-game benchmark `out/gpu/fsr4-bench` (`ninja -C out/gpu
fsr4-bench`; `--stats`: registers and instructions from RADV). 4K Balanced (2260x1272 → 3840x2160),
RX 7800 XT: all of FSR 4 takes 5.8 ms, of which **post 2.5–2.9 ms** (not the neural network: the
last layers, a 2x2 pixel shuffle and blending with the history), the model's 12 passes 2.4 ms, pre
0.55.

The cause: each thread computes a 2x2 block of output pixels and writes them one by one into three
images (the recurrent state, the history, the output), so each write instruction of a wave writes
every other pixel. Removing any one of the three writes made the pass 1.3–4 times faster with the
same code.

The fix: `tools/fsr4_optimize.sh` decompiles post (spirv-cross), `tools/fsr4_post_lds.pl` gathers
the workgroup's 16x16 block in shared memory and writes whole rows, glslang compiles it back into
`fsr4_shaders/opt/`; `vk_fsr4.cpp` takes it from there (`BB_FSR4_OPT=0`: the original). Two
subtleties found by comparing outputs:
- spirv-cross translates the signed int8 unpack (`OpBitcast` to `i8vec4`) as `unpack8(uint)`
  (unsigned): without a fix the result is entirely different (PSNR 17 dB);
- values in shared memory must stay float: the compiler turns half from shared memory into a
  16-bit write, which rounds differently in unorm8.

`tools/fsr4_verify.sh` compares the original and the optimized post for all presets at 1080p,
1440p and 4K output on pseudo-random inputs: 1440p and 4K are bit-identical; post at 4K 2.1–3.5 →
0.81–0.87 ms, 1440p 1.2–1.5 → 0.36–0.38 ms, 1080p 0.37–0.69 → 0.20–0.22 ms. In the game (4K
Balanced): FSR 4 5.8 → 4.0 ms, the GPU frame 13.1 → ~11.6 ms; beyond that the FPS is limited by the
CPU (the GPU command thread waits for guest memory copies, "host copies" ~20%).

Found along the way: **the original FSR 4 at the 1080 tier (1920x1080 output) is nondeterministic**:
from run to run on identical inputs a stripe at the left edge (columns 0–132) changes, so there is a
race or a read of uninitialized memory somewhere in the model or provider. At 1080p output this can
show as flicker at the left edge of the frame. Not investigated.

The reactive mask is no longer computed with FSR 4 (FSR 4 does not take it).

### Also checked (same place, 4K Balanced)

- **A/B in one process** (`ab.sh`, bit 24: FSR off, the UI copy without upscaling): 86.5 FPS with
  FSR 4 against 87.7 without. After the post optimization the frame is limited by the CPU (the GPU
  command thread, waiting for guest memory copies), not the GPU. Async compute for FSR (overlapping
  it with the start of the next frame) would give no more than this ~1%: postponed until the CPU
  side is faster; it would also need the UI and output commands of frame N reordered after frame
  N+1's work.
- **WMMA (`VK_KHR_cooperative_matrix`, supported by RADV on RDNA3)**: a prototype of the model's
  pass 1 (a 16-channel residual block: 3x3 16→16, 1x1 16→32 ReLU, 1x1 32→16): 0.85 ms in the first
  version, 0.49 ms with a word layout in shared memory, against 0.30 ms for the original dot4. The
  arithmetic on WMMA would take ~0.1 ms, but with 16 channels the tile layout, epilogues through
  shared memory and occupancy (6 waves per SIMD, limited by LDS) eat the gain. The ceiling is about
  1 ms for all 12 passes with careful hand work on each; not started.
- `RADV_PERFTEST=cswave32` (wave32 for compute): FSR 4 is slower, 4.2 → 5.4 ms.
- The pre pass (0.54 ms) is not limited by its writes (0.50 ms without them).

## 2026-10-01: FSR 4 flicker at the left edge at 1080p output: a race in the model's pass 11

The 1080 tier's nondeterminism (see above) is a bug in the v07 model's shader, pass 11 (the
decoder, 1/4 → 1/2 resolution). Each thread is one input pixel at 1/4 resolution and writes a 2x2
block of the 1/2 resolution output. The dispatch rounds the width up to 64 threads, and threads past
the input's width do not stop: at the 1080 tier (an input 480 wide) threads 480..511 write output
pixels 960..1023, that is the first pixels of the next row (any tensor's row is 15360 bytes), and
race with their real writers. At 4K output the width 960 divides by 64, so there is no error.

`tools/fsr4_pass11_guard.pl` adds an early exit for threads past the edge (the size comes from the
pass's own neighbour check); `tools/fsr4_optimize.sh` builds it into `fsr4_shaders/opt` for all
presets together with post; `vk_fsr4.cpp` and the benchmark take any pass from `opt/`.
`tools/fsr4_verify.sh`: 1440p/4K are bit-identical to the original (the guard does not trigger
there); 1080p gives the same result from run to run, and differs from the original after the first
frame only in the former stripe at the left edge.

Found at the same time: spirv-cross translates every signed int8 unpack (`OpBitcast` to `v4char`)
as the unsigned `unpack8(uint)`; pass 11 has nine different forms of it. The fix is shared by the
scripts: `tools/Fsr4SpirvCrossFixes.pm` (every `unpack8` through a signed helper).

## 2026-10-01: FSR 4.1.1, what is inside and how to run it on Vulkan (exploration)

Source: `amd_fidelityfx_upscaler_dx12.dll` 4.1.1.2740 from OptiScaler (`FSR4_LATEST`).
- The DLL has the provider `ffxProvider_FSR4_Int8` / `Fsr4Int8UpscalerModel` (version "FSR4-i8
  4.1.1"). 1028 DXIL containers (25.7 MB): the model passes `fsr4_model_v07_fp8_no_scale_pass1..12`
  (+ `_post`), 270 `prepass` variants, 144 `postpass`, `fsr_rcas_pass` and a fallback FSR 3.1/2.
  Nothing is compressed.
- Despite "fp8" in the name, the model passes are plain DXIL with `dot4AddPacked` (int8 dot4),
  without AMD extensions (WMMA). The architecture is the same v07 (12 passes), but the weights and
  biases are not built into the shader: they are read from `InitializerBuffer` (t18, a typed
  buffer), the tensor sizes from the cbuffer `CsTensorSizes`, scratch is `u11`. Each pass has 9
  variants: the grid is built in (tiers 1080 / 4K / 8K: 480x270, 960x540, 1920x1080 for pass 5) and,
  apparently, the presets.
- `amdxcffx64.dll` (loader 2.3.0.2913, which Proton loads) has the same FP8 FSR 4 passes and ML frame
  generation (`mlfi_*`), no separate INT8 model. `FSR4_INT8` 4.0.2 is `v07_i8`, which the port
  already uses.
- dxil-spirv (HansKristian-Work/dxil-spirv, built from source) translates the passes into correct
  SPIR-V: `dxil-spirv <pass> --enable-shader-i8-dot --ssbo-uav --use-reflection-names`.

What a provider of our own for 4.1.1 lacks: which container is which (preset, tier, pass), the
contents of `InitializerBuffer`, the values of `CsTensorSizes`, the prepass/postpass permutations,
constants and dispatch sizes. The most reliable path is not to take apart the provider's x86 code
but to record one frame: a small test program on the FFX API (DX12, MinGW) under Wine/Proton with
vkd3d-proton, the "FSR4-i8" provider chosen explicitly through a version override, and a Vulkan
layer that writes the pipelines (SPIR-V from dxil-spirv), buffer contents and dispatch sizes. Then
replay it with our own provider in the same benchmark (`fsr4-bench`), checked against the recorded
output.

## 2026-10-01: FSR 4.1.1 in the port (upscaler=fsr411)

AMD's 4.1.1 model (INT8) runs natively on Vulkan and matches the DLL bit for bit.

**How it was obtained.** `tools/fsr4cap/fsr4cap.exe` (C, MinGW) calls the FidelityFX API 2.3 from
`amd_fidelityfx_loader_dx12.dll` with an explicit version 4.1.1 and, by replacing D3D12 method
tables, records everything the DLL does under vkd3d-proton: pipelines (DXIL), root signatures,
descriptors, uploads, constants, dispatches. A frame is 29 dispatches: auto-exposure SPD, prepass,
pass0_post, model passes 1..12 (each followed by `_post`, clearing the tensor's border), postpass,
RCAS.
- There are two models: m0 (Native..Performance) and m1 (Ultra Performance, ratio 3.0), with 128 KB
  of weights (`InitializerBuffer`). Tiers: t1080 (output up to 1920x1080), t2160 (larger, including
  2560x1080).
- Tensors are computed from the output aligned to 8; the `CsTensorSizes` table (17 sizes), the group
  counts and the constants (MLSR as in v07, but width/height not aligned and `inv_scale = 1 /
  scale`; SPD; RCAS) are checked by `extract.py` on 20 recordings.
- dxil-spirv translates the DXIL the same way vkd3d-proton does: `--mixed-float-dot-product`
  (`VK_VALVE_shader_mixed_float_dot_product`, dot2 on half with accumulation in float); without it
  the PSNR is 40 dB instead of an exact match. `--class-bindings` (a patch) lays out the SRV, UAV,
  CBV and sampler registers at bindings +0/+32/+64/+96, and the runtime binds resources by name.
- `fsr411/fsr411.cpp` is an independent Vulkan core (the game and `fsr4-bench --fsr411`).
- `verify.sh`: the DLL and the replay on identical inputs, 8 frames: bit-identical for 1080p (both
  models), 1440p, 2560x1080 and 4K (both models). 1600x900 differs only at the right edge, where the
  DLL itself is nondeterministic (a race in its shaders when the tensor width is not a multiple of
  64).

**Speed-up.** The postpass was rewritten with workgroup memory at the level of SPIR-V assembly
(`postpass_lds.py`; going through GLSL is inexact: `DenormPreserve` and the exact placement of
`RelaxedPrecision` are lost, and something else changes the result too): 2.18 → 0.95 ms at 4K, bit
for bit as the DLL. All of FSR 4.1.1: 1.09 ms at 1080p (1280x720), 4.2 ms at 4K Balanced. In the
game (4K Balanced) the upscaler takes 4.12 ms against 4.32 ms for the accelerated v07.

**Building the assets** (by the user, from their own DLLs; nothing from AMD is in the repository):

    bash tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll 4.1.x> <amd_fidelityfx_loader_dx12.dll 2.3.x>

Needs nix-shell (or MinGW, cmake, ninja, python3, SPIRV-Tools, umu-run), and network access for
dxil-spirv, the FidelityFX SDK headers and Proton (GE-Proton). The DLLs come with OptiScaler, for
example (`FSR4_LATEST/`), and with games that have FSR 3.1/4. A build from scratch gives the same
files; `VERIFY=1` at the end compares with the DLL.

**A correction to the A/B above ("the frame is limited by the CPU").** Those measurements ran while
12 hung shadPS4 processes each took a core. On an idle system: "host copies" 1.6%, waiting for the
GPU ~41%: the frame is limited by the GPU, so async compute for FSR makes sense again.
