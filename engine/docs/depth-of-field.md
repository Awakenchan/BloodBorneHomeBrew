# Map-driven depth of field

Stock Yebis filtering is the default. Set `BB_MODERN_DOF=1` before launch to opt into
the modern replacement. With that opt-in, when the existing **Depth of
field (DoF)** option is enabled, supported passes use map-driven far bokeh plus
autofocus-driven foreground bokeh.
No GPARAM files are changed, and there is no map-name table in the renderer.

The in-game graphics overlay's **Depth of field intensity** slider is saved as
`graphics_dof_intensity` (0 to 3, default 1). It multiplies the calibrated modern
far-blur radius **after** the map's resolved curve and kernel are evaluated.
It also scales the added near-blur radius, without changing autofocus distance.
It does not change the authored far ranges or enable blur in the near focus zone.
Zero restores sharp RGB while preserving the original
output alpha on the recognized recombination path. Stock fallback pixels fade
toward sharp below 1; they cannot be amplified above their original blur.
The control requires DOF enabled and `BB_MODERN_DOF=1`, applies live
independently of Picture tuning, and resets to 1 with **Game's own picture**.

## Authored settings

Bloodborne resolves the active `Dof[Yebis]` settings into a depth lookup, encodes the
lookup result into HDR scene alpha, and supplies a map-dependent blur kernel and
recombination constants. The replacement consumes that encoded alpha and those
constants. The game's selection of map version, parameter row, blending and runtime
overrides remains authoritative; the replacement does not independently reinterpret
GPARAM defaults, duplicate `Enable` names or empty parameter groups.

Live Central Yharnam and Hunter's Dream captures matched the binder profiles'
far ramps (50 to 200 and 10 to 150 respectively). The plateau lookup values matched
`CocSize * CocScale` (0.0098 and 0.03), not a physical pixel radius. Neither these
particular profiles nor their distances are hardcoded.

For the verified recombination shader, `q = alpha * constants[76] + constants[77]`
and the original blurred-image weight is `clamp(q - 1, 0, 1)`. To calibrate a
replacement without arbitrary intensity scaling, the renderer measures the seven-tap
kernel's weighted UV covariance on the GPU. A uniform disk of radius R has variance
R squared / 4 along either axis. Its Cholesky transform reproduces the stock
kernel's second moment, rotation and anisotropy. The local disk is scaled by the
square root of the original blur weight: its variance therefore follows that weight.
This is a filtering calibration, not a claim that the original artistic controls
describe a real-world aperture or sensor.

## Foreground autofocus

The added near effect is independent of the authored far curve. It does not use
the unused GPARAM autofocus fields or reinterpret a zero near curve as an authored
blur. After depth reconstruction, a GPU pass meters a 7 by 7 grid over the
upper-centre of the view (38-62% of width, 23-41% of height), above the hunter's
usual lower-centre silhouette. This is a framing heuristic, not a character mask:
unusual camera angles can still put the hunter in the metering area.

The median rejects isolated leaves and outliers. At least 12 finite samples
between 0.1 and 200 camera-depth units are required; mostly-sky views use a
30-unit focus target instead of infinity. Focus is clamped to 3-40 units and
smoothed in inverse distance with a 0.25-second time constant and 3% dead band.
Smoothing is time-based rather than a fixed number of frames. First use, a
resolution change, a gap longer than 0.25 seconds, or a projection change resets
history; invalid camera data invalidates focus. No depth is read back to the CPU.

The sharp near zone ends at `min(0.45 * focus_distance, 2.5)` units. Blur then
increases smoothly in inverse depth toward full strength at 0.45 units. This
intentionally conservative foreground range keeps normal third-person gameplay
and typical hunter distances sharp rather than turning a distant focus target
into an out-of-focus player. At 1x intensity, the maximum circular near-bokeh
radius is 10 pixels at 1080p, scaled with resolution and the existing intensity
slider. Near autofocus does not change the map-driven far blur at all.

A softly feathered central safety region additionally suppresses near blur for
close-up player framing. Its fully protected half-width expands from about 16%
of the screen at the top to 44% below 85% screen height, covering the head,
shoulders and cloak. Protected pixels neither generate near bokeh nor receive
foreground blur from outside the region. This is a screen-position heuristic,
not player identification: nearby scenery inside it is also protected, and an
off-centre player outside it is not guaranteed protection. Map-authored far blur
is unchanged.

A conservative GPU tile classification skips regions without near contributors.
Remaining pixels use a 48-sample full-resolution gather with masked bilinear
taps. Defocused foreground is allowed to spread over farther layers but is
rejected behind closer occluders. Coverage is composited over the already
resolved far image, preserving HDR and alpha. There is no temporally accumulated
colour to leave trails behind moving leaves.

## Rendering and fallback

The implementation uses ordinary FP32 Vulkan compute shaders, without a new SDK,
FP16 arithmetic or subgroup requirements:

- Capture the forward-Z inverse projection from the current shadow-resolve pass and
  snapshot scene depth when the alpha-encoding pass executes.
- Capture and validate the stock blur's pixel weights and vertex offsets when that
  pass executes, before its constant storage can be reused.
- At the verified far recombination, run bilateral half-resolution preparation,
  a 48-sample disk gather and depth/CoC-aware full-resolution reconstruction.
  Sharp foreground samples are excluded from the far gather.
  Thin depth/CoC boundaries receive a sparse full-resolution repair with masked
  bilinear taps, and reconstruction confidence fades smoothly to stock rather
  than switching filters as subpixel foliage crosses a half-resolution block.

The stock passes still execute. Authored far-focused pixels (`q <= 1`) retain their
original far result, but can receive the independent near effect if close enough.
Unknown resource chains and unsupported alternate layouts keep their original result.
Invalid camera/kernel constants are rejected on the GPU and leave the stock result
intact, without a readback or CPU/GPU wait. Host-side layout rejection is reported
once. The added near effect requires valid camera/focus data, but does not require
a nonzero far CoC or borrow the far kernel's radius. Legacy alternate near-only
render passes have not been independently integrated.

Only supported DOF RGB is overwritten. HDR values are not tone mapped or
clamped to SDR, and the original output alpha is preserved. Volumetrics remain
before DOF, bloom/tone mapping remain after it, and temporal upscaling is not
relocated. Working images follow the bound scene/proxy size, including odd
dimensions; map-dependent Yebis working-image dimensions are not hardcoded.

This bridge inherits the original depth lookup's quantization. It does not claim
to recover an analytic curve from a few samples or solve transparent-layer DOF
with a single scene-depth buffer. Stock fallback and the environment switch are
intentional while additional maps and alternate passes are evaluated.

## Regression test

Build `depth-of-field-shader-test` in the GPU CMake build and run it with Vulkan.
The test dispatches the real shader variants on synthetic HDR/depth images. It
covers kernel calibration, nonzero constant offsets, odd dimensions, foreground
occlusion, focused/near fallback, invalid camera/kernel/curve fallback, HDR/alpha
preservation and measurable blur of a high-frequency input. It also checks
autofocus convergence, frame-rate independence, lower-centre subject exclusion,
sky fallback, and near blur when the authored far curve is zero.
