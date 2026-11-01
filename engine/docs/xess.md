# Intel XeSS (2026-11-01)

`upscaler=xess` in `lantern.ini` (the menu's Upscaler > XeSS (Intel), the launcher's Graphics page)
runs Intel XeSS Super Resolution (XeSS-SR) from Intel's XeSS SDK 3.0.2 (`vk_xess.cpp`). Windows only.
XeSS Frame Generation and multi frame generation are not in it: see "Frame generation" below.

## What runs where

| GPU | What libxess.dll runs |
|---|---|
| Intel Arc (Alchemist, Battlemage), Core Ultra with Arc graphics | the XMX model |
| Other GPUs with `shaderIntegerDotProduct` or `shaderInt8` (NVIDIA, AMD, older Intel) | the DP4a model |
| GPUs without `VK_EXT_mutable_descriptor_type` or `shaderStorageImageWriteWithoutFormat` | none: `upscaler=xess` falls back to FSR 3.1 |

The SDK chooses the model by itself. The log names the library and the outcome:
`XeSS: libxess.dll 2.0.2 loaded` (the XeSS-SR version inside SDK 3.0.2),
`Upscaler: Intel XeSS found (...): XeSS-SR available`, `XeSS: Super Resolution 2.0.2 available`,
and on each new size or mode
`XeSS: 1280x720 -> 2560x1440, quality mode 102, LDR input`.

## Files

`engine/xess/libxess.dll` (XeSS SDK 3.0.2, about 75 MB), `LICENSE-XeSS.txt` and
`third-party-programs-XeSS.txt`, downloaded by `tools/fetch_xess.sh` from Intel's GitHub release
(SHA-256 of the SDK zip checked). They are not in the repository (`.gitignore`). `BB_XESS_DIR`
overrides the folder; `BB_XESS=0` hides the library. `libxess.dll` needs the Microsoft Visual C++
2015-2022 runtime (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`); without it the load
fails and the log says so.

**Licence.** The SDK is under the Intel Simplified Software License: the binary may be
redistributed unchanged with its copyright notice and terms (LICENSE-XeSS.txt next to it), and no
reverse engineering. No Intel header is vendored or included: `vk_xess.cpp` declares the few
structures and functions of `xess.h` / `xess_vk.h` it calls, the way `vk_dlss.cpp` does for NGX, and
loads the DLL at run time, never linked. Whether shipping it beside a GPL engine is acceptable is the
maintainer's call, as for NVIDIA's DLLs (docs/dlss_frame_generation.md, Licence).

## How it fits in the engine

- **Before the instance and device** (`vk_platform.cpp`, `vk_instance.cpp`): with the library
  present, `xessVKGetRequiredInstanceExtensions` and `xessVKGetRequiredDeviceExtensions` name what
  XeSS needs; those extensions and `VK_EXT_mutable_descriptor_type` (with its feature) are enabled.
  `shaderStorageImageWriteWithoutFormat`, `shaderInt8` and `shaderIntegerDotProduct` are on
  already where the GPU has them. Without the library nothing changes for other paths.
- **The frame** is the one DLSS takes (`Fsr4Upscaler::Frame`): scene colour, depth, motion vectors
  (RG16F, render pixels, previous - current: XeSS's own convention, velocity scale 1), jitter in
  pixels with the same sign as DLSS and FSR, and the reactive mask (menu) as XeSS's responsive pixel
  mask. Low-resolution motion vectors, standard depth. Inputs go to ShaderReadOnlyOptimal for the
  dispatch and back to General, the output stays in General (XeSS synchronizes nothing itself).
- **Colour.** Like DLSS, XeSS takes the frame as it is: HDR scenes with XeSS's auto exposure, the
  tonemapped LDR frame (outputs other than 1080p) with `XESS_INIT_FLAG_LDR_INPUT_COLOR`.
- **Quality mode.** The engine's preset (or Custom `render_scale`) sets the render size as for every
  upscaler; XeSS is initialized with the mode whose ratio is nearest (1.0 Native AA, 1.3 Ultra
  Quality Plus, 1.5 Ultra Quality, 1.7 Quality, 2.0 Balanced, 2.3 Performance, 3.0 Ultra
  Performance), so the render size stays inside that mode's range. A new mode, output size, HDR
  state or mask setting initializes XeSS again (after the earlier command buffers finish).
- **Sharpening.** XeSS has none of its own: the engine's RCAS pass applies the whole 0..2, as for
  DLSS. **Mip bias:** the ratio's bias (log2(render / output)), which is Intel's recommendation.
- **Fallback.** No library, no supported device, or a failed initialization: the menu says why,
  the log names the XeSS result, and the upscaler becomes FSR 3.1.
- **OptiScaler.** OptiScaler hooks `libxess.dll` by name for its own XeSS inputs; untested with it.

## Frame generation

XeSS Frame Generation (`libxess_fg.dll`, `xefg_swapchain_d3d12.h`), its 3x/4x multi frame generation
(XeSS 3, Intel Arc only) and XeLL (`libxell.dll`, required by XeSS-FG) are D3D12 only in every SDK
release up to 3.0.2: the SDK has no Vulkan header for them. The engine renders in Vulkan only, so
XeSS-FG would need a Vulkan-to-D3D12 bridge (Vulkan images shared with D3D12, a DXGI swapchain
presenting real and generated frames). That is not done; FSR 3.1 and DLSS frame generation stay the
choices. To revisit when Intel ships a Vulkan XeSS-FG.

## Settings

| `lantern.ini` | Env | Values | Live |
|---|---|---|---|
| `upscaler` | `BB_UPSCALER` | `xess` (with the others) | yes, once the library loaded at start |
| | `BB_XESS_DIR` | the folder with libxess.dll (default `engine/xess`) | start |
| | `BB_XESS` | `0`: the library is not loaded | start |
| | `BB_XESS_LOG` | `1`: the SDK's debug messages in the log (errors only otherwise) | start |

## Not verified

Built on Windows (MSYS2 CLANG64); `settings-test` passes. A standalone smoke test on an RTX 4090
(driver's Vulkan 1.3) drove libxess.dll through the same declarations as `vk_xess.cpp`: the only
device extension asked for is `VK_EXT_mutable_descriptor_type`; context, init (1920x1080 Balanced,
LDR: 960x540 input), three executes on real images, a second init (Native AA, HDR) and destroy all
returned success without a device loss. Not run in game: no game dump on that PC, and no Intel GPU
(the XMX path). To check in game on an Intel Arc and on one NVIDIA or AMD card: the picture while moving
(jitter sign, motion vectors), Native AA at 1080p, the LDR path at 1440p and 4K output, a preset
change in the menu, the responsive mask, and the cost against FSR and DLSS.
