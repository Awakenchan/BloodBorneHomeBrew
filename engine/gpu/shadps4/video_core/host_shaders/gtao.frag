// SPDX-License-Identifier: GPL-2.0-or-later

// Ambient occlusion from a visibility bitmask (Therrien, Levesque, Gilet, "Screen Space Indirect
// Lighting with Visibility Bitmask") on top of the GTAO slice setup (Jimenez et al., Activision
// TR-16-01), in place of Bloodborne's ScreenSpaceAO2_WithNormal pixel shader (hash cbbca5dc). It
// keeps that shader's resource interface (decoded from its IR dump), so the engine's
// descriptors, targets and the later bilateral upscale stay as they are.
//
//   ubo_0 (binding 0)  constants: [0..15] inverse projection rows (depth -> view position),
//                      [16,17] target size, [18,19] screen-space radius scale, [20..30] world ->
//                      view normal rows, [36,37] strength scale and offset, [40..42] radius
//                      scaling by distance (a, min, max)
//   ubo_1 (binding 1)  the SSAO sample pattern (unused here, declared to keep the layout)
//   ubo_2 (binding 2)  [2] far distance, for the packed depth output
//   img0 / samp0       depth        img1 / samp1  normals        img2 / samp2  64x64 rotation noise
//
// Output RT0: R = visibility (1 = open), GBA = view depth packed for the bilateral upscale.

#version 450
#extension GL_EXT_scalar_block_layout : require

layout(scalar, set = 0, binding = 0) uniform Cb0 { uint d[16384]; } ubo_0;
layout(scalar, set = 0, binding = 1) uniform Cb1 { uint d[16384]; } ubo_1;
layout(scalar, set = 0, binding = 2) uniform Cb2 { uint d[16384]; } ubo_2;
layout(set = 0, binding = 3) uniform texture2D img0;
layout(set = 0, binding = 4) uniform texture2D img1;
layout(set = 0, binding = 5) uniform texture2D img2;
layout(set = 0, binding = 6) uniform sampler samp0;
layout(set = 0, binding = 7) uniform sampler samp1;
layout(set = 0, binding = 8) uniform sampler samp2;

layout(location = 0) out vec4 frag_color0;

const float PI = 3.14159265358979;
const float HALF_PI = 1.57079632679490;
const int SLICES = 1;
const int STEPS = 5;
const float MIN_RADIUS_PX = 28.0;
const int SECTORS = 32; // bits in the visibility mask
// Assumed thickness of every depth sample (view units): light passes behind anything thinner,
// which is what keeps rails, wires and foliage from darkening their surroundings. It grows with
// distance so far-away geometry is not over-attenuated.
const float THICKNESS_NEAR = 0.10;
const float THICKNESS_FAR = 2.0;
// World-space reach of the occlusion (view units, about metres). Occluders farther than this
// from the surface are ignored, which keeps thin objects from darkening things well behind them.
const float MAX_WORLD_RADIUS = 0.3;
// Extra visibility exponent at the far plane; 0 = no distance boost.
const float DISTANCE_BOOST = 2.0;
const float THICKNESS_BIAS = 0.02; // fraction of the reach, keeps flat surfaces from self-occluding

float cb0(int i) { return uintBitsToFloat(ubo_0.d[i]); }

float readDepth(vec2 uv) { return textureLod(sampler2D(img0, samp0), uv, 0.0).x; }

// inverse projection rows, loaded once per pixel
vec4 gR0, gR1, gR2, gR3;

vec3 viewPos(vec2 uv, float depth) {
    vec4 h = vec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    vec4 r = vec4(dot(gR0, h), dot(gR1, h), dot(gR2, h), dot(gR3, h));
    return r.xyz / r.w;
}

float fastAcos(float x) {
    float a = abs(x);
    float r = sqrt(1.0 - a) * (1.5707963267948966 + a * (-0.2146018 + a * (0.0889789 + a * -0.0501743)));
    return x >= 0.0 ? r : PI - r;
}

// Sector bits covered by one depth sample treated as a slab of thickness `thick` along the view
// ray (visibility bitmask, Therrien et al. 2023). A sector is taken when at least half covered.
uint sectorMask(vec2 uvS, vec3 pos, vec3 v, float side, float lo, float thick, float reach, float bias) {
    vec3 d = viewPos(uvS, readDepth(uvS)) - pos;
    float l = length(d);
    if (l > reach || l < bias) return 0u;
    // occluders near the edge of the reach cover less, so contact shadows taper off instead of
    // ending in a halo
    thick *= 1.0 - smoothstep(0.3, 1.0, l / reach);
    vec3 db = d - v * thick;
    float af = side * fastAcos(clamp(dot(d, v) / l, -1.0, 1.0));
    float ab = side * fastAcos(clamp(dot(db, v) / max(length(db), 1e-4), -1.0, 1.0));
    float s0 = clamp((min(af, ab) - lo) * (1.0 / PI), 0.0, 1.0) * float(SECTORS);
    float s1 = clamp((max(af, ab) - lo) * (1.0 / PI), 0.0, 1.0) * float(SECTORS);
    uint a = uint(s0 + 0.5);
    uint n = uint(s1 + 0.5) - a;
    if (n == 0u || a >= uint(SECTORS)) return 0u;
    uint m = n >= 32u ? 0xFFFFFFFFu : ((1u << n) - 1u);
    return m << a;
}

void main() {
    gR0 = vec4(cb0(0), cb0(1), cb0(2), cb0(3));
    gR1 = vec4(cb0(4), cb0(5), cb0(6), cb0(7));
    gR2 = vec4(cb0(8), cb0(9), cb0(10), cb0(11));
    gR3 = vec4(cb0(12), cb0(13), cb0(14), cb0(15));
    vec2 size = vec2(cb0(16), cb0(17));
    vec2 uv = gl_FragCoord.xy / size;
    float depth = readDepth(uv);

    float visibility = 1.0;
    vec3 pos = viewPos(uv, depth);

    if (depth < 1.0) {
        vec3 wn = textureLod(sampler2D(img1, samp1), uv, 0.0).xyz * 2.0 - 1.0;
        wn = normalize(wn);
        vec3 n = vec3(dot(vec3(cb0(20), cb0(21), cb0(22)), wn),
                      dot(vec3(cb0(24), cb0(25), cb0(26)), wn),
                      dot(vec3(cb0(28), cb0(29), cb0(30)), wn));
        n = normalize(n);

        vec3 v = -normalize(pos);
        float scale = max(cb0(41), min(cb0(42), cb0(40) / pos.z));
        vec2 texel = 1.0 / vec2(textureSize(sampler2D(img0, samp0), 0));
        // keep a minimum footprint so distant surfaces still get occlusion instead of fading out
        vec2 radiusUv = max(vec2(cb0(18), cb0(19)) * scale, texel * MIN_RADIUS_PX);

        vec2 noise = textureLod(sampler2D(img2, samp2), gl_FragCoord.xy / 64.0, 0.0).xy * 2.0 - 1.0;
        noise = normalize(noise + vec2(1e-5));
        float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        float thick = THICKNESS_NEAR + THICKNESS_FAR * clamp(pos.z / uintBitsToFloat(ubo_2.d[2]), 0.0, 1.0);

        float accum = 0.0;
        for (int s = 0; s < SLICES; ++s) {
            // slices are a quarter turn apart around the pixel
            vec2 dir = s == 0 ? noise : vec2(-noise.y, noise.x);
            vec2 stepUv = dir * radiusUv;

            // direction of the slice in view space, taken at this pixel's depth
            vec3 full = viewPos(uv + stepUv, depth) - pos;
            float radius = max(length(full), 1e-4);
            vec3 dirV = full / radius;

            vec3 ortho = dirV - dot(dirV, v) * v;
            vec3 axis = normalize(cross(ortho, v));
            vec3 projN = n - axis * dot(n, axis);
            float projLen = length(projN);
            if (projLen < 1e-4) {
                accum += 1.0;
                continue;
            }
            float cosN = clamp(dot(projN, v) / projLen, -1.0, 1.0);
            float sgn = dot(ortho, projN) < 0.0 ? -1.0 : 1.0;
            float nAng = sgn * fastAcos(cosN);

            // the 32 sectors cover the hemisphere around the projected normal, one bit each
            float lo = nAng - HALF_PI;
            float reach = min(radius, MAX_WORLD_RADIUS);
            float bias = THICKNESS_BIAS * reach;
            uint mask = 0u;

            for (int i = 0; i < STEPS; ++i) {
                float t = (float(i) + jitter) / float(STEPS);
                t = t * t;
                vec2 off = stepUv * t;
                off = sign(off) * max(abs(off), texel);
                mask |= sectorMask(uv + off, pos, v, 1.0, lo, thick, reach, bias);
                mask |= sectorMask(uv - off, pos, v, -1.0, lo, thick, reach, bias);
            }

            // open sectors, weighted by the cosine around the normal
            float open = 0.0;
            float cPrev = 1.0; // cos(0)
            for (int j = 0; j < SECTORS; ++j) {
                float cNext = cos(PI * float(j + 1) / float(SECTORS));
                if (((mask >> uint(j)) & 1u) == 0u) open += 0.5 * (cPrev - cNext);
                cPrev = cNext;
            }
            accum += clamp(projLen * open, 0.0, 1.0);
        }
        visibility = accum / float(SLICES);
        // darken more with distance, where the pixel footprint hides detail
        float farFrac = clamp(pos.z / uintBitsToFloat(ubo_2.d[2]), 0.0, 1.0);
        visibility = pow(clamp(visibility, 0.0, 1.0), 1.0 + DISTANCE_BOOST * sqrt(farFrac));
    }

    float ao = cb0(36) * visibility + cb0(37);

    // linear depth over far, in three 8-bit steps for the bilateral upscale
    float lin = pos.z / uintBitsToFloat(ubo_2.d[2]);
    float p = fract(65025.0 * lin);
    float q = fract(255.0 * lin);
    float b = q - p * (1.0 / 255.0);
    float a = lin - q * (1.0 / 255.0);
    frag_color0 = vec4(ao, p, b, a);
}
