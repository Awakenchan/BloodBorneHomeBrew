# BBFSS

**BBFSS is an unofficial, modified fork of [Lantern Engine by Awaken (https://github.com/Awakenchan/LanternEngine)](https://github.com/Awakenchan/LanternEngine)**, focused on enhanced lighting, reflections, shadows and depth of field for Bloodborne. It is not an official Lantern Engine release.

The underlying Lantern Engine runs your own copy of Bloodborne on Windows and Linux, building on bbport and shadPS4. The game's own code runs natively on your CPU; its graphics are translated to Vulkan. BBFSS extends that renderer with the visual effects below. These additions have been developed and evaluated on Windows; equivalent Linux behavior has not yet been verified.

**No game files are included.** You need your own legal copy of Bloodborne (CUSA03173) with update 1.09, dumped from your own PS4.

**BBFSS:** [Repository](https://github.com/fromsoftserve/BBFSS) and [issue tracker](https://github.com/fromsoftserve/BBFSS/issues). Report issues with this modified version here, rather than treating them as upstream bugs.

**Upstream help:** Lantern Engine's [Help & FAQ](https://lantern-engine.netlify.app) covers the original project's setup, graphics settings and troubleshooting. It does not necessarily describe BBFSS-specific effects or behavior.

## Visual enhancements

The effects integrate with the existing Vulkan renderer and the game's depth, normal, lighting and post-processing passes. They do not require editing each map's assets.

### SSGI indirect lighting

Screen-space global illumination adds approximate bounced light and color bleeding from visible surfaces. The implementation uses the visibility-bitmask method described by Therrien, Levesque and Gilet: depth samples cover angular sectors, and newly visible sectors contribute incoming light from the scene.

A half-resolution gather uses reconstructed surface geometry, temporal reprojection and filtered history feedback for approximate additional bounce lighting. Depth-aware denoising and bilateral upsampling bring the result back to scene resolution; the receiving surface's base color tints the indirect contribution. This is a screen-space approximation, not full-scene path tracing: hidden and off-screen surfaces cannot supply reliable light.

Implementation: [`ssgi.comp`](engine/gpu/shadps4/video_core/host_shaders/ssgi.comp).

### Stochastic SSR

Screen-space reflections reconstruct view-space positions from depth and march reflected rays through the visible depth buffer. The fallback fragment path traces frame-varying subpixel samples; the full-resolution compute path traces one ray per pixel. Temporal reprojection and accumulation stabilize the results.

Depth- and normal-aware filtering avoids borrowing reflection directions from unrelated surfaces. Intersections require a front-to-back depth crossing, bisection refinement and a final view-space hit validation. A depth-aware composite blends reflections with the existing environment lighting, with relaxed rough-surface attenuation to cover more materials.

The saved **SSR intensity** slider applies a live 0-3x multiplier after stabilization and roughness fading. Zero restores the environment-reflection contribution rather than replacing it with black. SSR still cannot reflect geometry outside the screen or hidden behind visible objects.

Implementation: [`ssr.frag`](engine/gpu/shadps4/video_core/host_shaders/ssr.frag), [`ssr_trace.comp`](engine/gpu/shadps4/video_core/host_shaders/ssr_trace.comp), [`ssr_temporal.comp`](engine/gpu/shadps4/video_core/host_shaders/ssr_temporal.comp) and the [composite generator](engine/tools/re/ssr_composite_patch.py).

### GTAO

The original SSAO shader is replaced by a GTAO-style screen-space occlusion implementation. It combines GTAO's view-space slice setup with visibility-bitmask angular coverage to estimate how much nearby geometry blocks ambient light.

The replacement preserves the original shader's resource interface and depth-packed output, allowing the existing bilateral upscale to remain in place. The result strengthens small-scale grounding and crevice shading without changing direct illumination. As with other screen-space effects, occlusion is limited by the visible depth buffer.

Implementation: [`gtao.frag`](engine/gpu/shadps4/video_core/host_shaders/gtao.frag).

### Volumetric lighting

Directional volumetrics integrate scattering along camera rays through a three-dimensional fog volume, using the game's shadow information to form lightshafts. Celestial-direction alignment, moderated light-color saturation and depth-aware reconstruction help the fog follow the scene's lighting without excessive color tint or gaps around thin foliage.

Point lights also contribute genuine volumetric scattering: camera rays are intersected with each light's spherical influence and scattering is integrated within that volume. These are not screen-space glow sprites. Point-light fog is deliberately unshadowed; scene depth stops the viewing ray, but walls do not block light traveling into the volume, so light leakage remains possible.

Separate saved sliders control directional and point-light volumetric intensity.

Implementation: [`volumetric_light.comp`](engine/gpu/shadps4/video_core/host_shaders/volumetric_light.comp) and [`point_volumetric.comp`](engine/gpu/shadps4/video_core/host_shaders/point_volumetric.comp).

### NVIDIA PCSS soft shadows

An implementation of NVIDIA's **Percentage-Closer Soft Shadows (PCSS)** technique replaces the original fixed-width directional shadow filtering. It searches the existing cascaded shadow map for blockers, estimates penumbra width from the separation between blockers and the receiving surface, then performs percentage-closer filtering with that width.

Shadows remain tighter near contact and soften as the receiver moves away from the blocker. This implements the published technique in Vulkan shaders; it is not an RTX feature or an NVIDIA-only SDK dependency.

Implementation: the PCSS path in [`contact_shadow.comp`](engine/gpu/shadps4/video_core/host_shaders/contact_shadow.comp).

### Contact shadows

Short screen-space rays march toward the directional light to find nearby occluders that the shadow map is too coarse to resolve. The result supplements the existing shadow visibility, adding fine grounding around small objects and surface details.

Contact shadows complement PCSS rather than replace shadow maps. They can only detect occluders represented in scene depth, so off-screen or hidden geometry remains the responsibility of the underlying shadow system.

Implementation: the contact-shadow path in [`contact_shadow.comp`](engine/gpu/shadps4/video_core/host_shaders/contact_shadow.comp).

### Bokeh depth of field

Modern depth-aware bokeh preserves each map's resolved far-DOF settings instead of imposing one global photographic preset. The renderer consumes the game's encoded blur curve and measures the native blur kernel's covariance on the GPU to calibrate a disk filter with equivalent second moment and anisotropy.

Bilateral half-resolution preparation, a disk gather and depth-aware full-resolution reconstruction produce the far blur. Sparse full-resolution edge repair stabilizes thin foliage. Independently authored near blur adds upper-centre autofocus, a robust median depth target and time-based focus smoothing.

A feathered central exemption protects typical close-up head, shoulder and cloak framing from near blur. This is a screen-region heuristic, not player identification: nearby scenery inside it is also protected. The saved **Depth of field intensity** slider scales blur after the map-specific settings, without moving the authored far-focus ranges.

Implementation: [`depth_of_field.comp`](engine/gpu/shadps4/video_core/host_shaders/depth_of_field.comp) and [`vk_depth_of_field.cpp`](engine/gpu/shadps4/video_core/renderer_vulkan/vk_depth_of_field.cpp). See the [DOF implementation notes](engine/docs/depth-of-field.md) for calibration, fallbacks and limitations.

## Upstream installation reference

The downloads below are **official Lantern Engine builds, not BBFSS builds**, and do not include this fork's custom visual changes. They are retained as an upstream setup reference. BBFSS-specific binary distribution is separate from those releases.

1. Download `LanternEngine-<version>-win64.zip` from [Releases](https://github.com/Awakenchan/LanternEngine/releases) and extract it anywhere.
2. Run `Lantern Engine.exe` and pick your game folder (the one with `eboot.bin`).
3. Press **Play**. The launcher updates itself. `Bloodborne.exe` starts the game directly (add it to Steam as a non-Steam game).

### Linux (beta)

1. Download `LanternEngine-<version>-linux-x64.tar.gz` from [Releases](https://github.com/Awakenchan/LanternEngine/releases) and unpack it (`tar xzf LanternEngine-*-linux-x64.tar.gz`).
2. Run `Lantern Engine` in the unpacked folder, pick your game folder and press **Play**. From a terminal: `./lantern.sh /path/to/CUSA03173`.
3. It needs a Vulkan driver (Mesa or NVIDIA), Python 3 and glibc 2.38 or newer (Ubuntu 24.04, Fedora 39, Arch, SteamOS 3.6, Bazzite). From 0.9.37 beta 7 the launcher updates itself on Linux too, and DLSS Super Resolution works on RTX cards with NVIDIA's driver (DLSS Frame Generation is Windows only).

## Credits

| | |
|---|---|
| **BBFSS** | Unofficial modified fork maintained by [fromsoftserve](https://github.com/fromsoftserve), adding the renderer enhancements described above |
| **Lantern Engine** | by **Awaken**: the launcher, the built-in game patches, and the engine reworked from its starting point (native shader compiler, direct draw calls, draw pipeline and memory performance work, PC menus and controls, upscaler integration, cheats, occlusion culling, trophies, fixes) |
| Engine starting point | [DarkIzuku/bloodborne_pc](https://github.com/DarkIzuku/bloodborne_pc) (built on [bbport](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4)) |
| Renderer crash fixes | adapted from [AwesomeObserver](https://github.com/AwesomeObserver/bloodborne_pc) and [deadinside28](https://github.com/deadinside28/bloodborne_pc) |
| Online backend for The Hunter's Dream | [droogie/bbhost](https://github.com/droogie/bbhost) |
| Online servers | [The Hunter's Dream](https://thehuntersdream.com) |
| FSR 3.1 and FSR 4 on Vulkan | [FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK |
| FSR 4.1.1 | [fsr4vk](https://github.com/dvj5411/fsr4vk), with the AMD driver fix from [0xCydral](https://github.com/0xCydral/bloodborne_pc) |
| DLSS | NVIDIA DLSS SDK |
| Sound | [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) |
| In-game menu | [Dear ImGui](https://github.com/ocornut/imgui) |
| Controller mappings | [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) |
| UI library | [WPF-UI](https://github.com/lepoco/wpfui) |
| GTAO slice formulation | Jimenez et al., *Practical Real-Time Strategies for Accurate Indirect Occlusion* (Activision TR-16-01) |
| Visibility-bitmask AO and SSGI | Therrien, Levesque and Gilet, [*Screen Space Indirect Lighting with Visibility Bitmask*](https://arxiv.org/abs/2301.11376) |
| PCSS technique | NVIDIA's Percentage-Closer Soft Shadows |

## License

Lantern Engine's own code, the launcher and the built-in game patches are GPL-3.0-or-later with additional terms (see [LICENSE](LICENSE) and [NOTICE](NOTICE)): anything that uses or ships them must keep the credit "Lantern Engine by Awaken", changed versions must say they are changed and must not pass as an official Lantern Engine release, and the name "Lantern Engine" is not licensed for other projects. Parts by others keep their own licenses, listed in the engine's `licenses/` folder.

Source: the engine's full source is in [`engine/`](engine) (0.9.37 beta 7 on), the launchers' in [`launcher/windows/`](launcher/windows), and the Help & FAQ page in [`faq/`](faq).
