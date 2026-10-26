// SPDX-License-Identifier: GPL-2.0-or-later

// Ground-truth ambient occlusion (Jimenez et al., "Practical Realtime Strategies for Accurate
// Indirect Occlusion", Activision TR-16-01) in place of Bloodborne's ScreenSpaceAO2_WithNormal
// pixel shader (hash cbbca5dc). It keeps that shader's resource interface (decoded from its IR
// dump), so the engine's descriptors, targets and the later bilateral upscale stay as they are.
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
// How fast the horizon relaxes once samples drop below it. Higher = thin occluders (rails,
// wires, foliage) cast less AO; 0 = classic max horizon.
const float THIN_DECAY = 0.8;
// World-space reach of the occlusion (view units, about metres). Occluders farther than this
// from the surface are ignored, which keeps thin objects from darkening things well behind them.
const float MAX_WORLD_RADIUS = 0.5;
// Extra visibility exponent at the far plane; 0 = no distance boost.
const float DISTANCE_BOOST = 2.0;
const float FALLOFF_START = 0.6; // fraction of the radius before samples fade out
const float THICKNESS_BIAS = 0.02; // fraction of the radius, keeps flat surfaces from self-occluding

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

            float low0 = cos(nAng + HALF_PI);
            float low1 = cos(nAng - HALF_PI);
            float h0c = low0, h1c = low1;

            float falloffR = min(radius, MAX_WORLD_RADIUS);
            float falloffRange = max((1.0 - FALLOFF_START) * falloffR, 1e-4);
            float falloffMul = -1.0 / falloffRange;
            float falloffAdd = FALLOFF_START * falloffR / falloffRange + 1.0;
            float bias = THICKNESS_BIAS * falloffR;

            for (int i = 0; i < STEPS; ++i) {
                float t = (float(i) + jitter) / float(STEPS);
                t = t * t;
                vec2 off = stepUv * t;
                off = sign(off) * max(abs(off), texel);

                vec2 uvA = uv + off;
                vec3 pA = viewPos(uvA, readDepth(uvA));
                vec3 dA = pA - pos;
                float lA = length(dA);
                float cA = dot(dA, v) / max(lA, 1e-5);
                cA = mix(low0, cA, clamp(lA * falloffMul + falloffAdd, 0.0, 1.0));
                if (lA > bias) h0c = cA > h0c ? cA : mix(h0c, cA, THIN_DECAY);

                vec2 uvB = uv - off;
                vec3 pB = viewPos(uvB, readDepth(uvB));
                vec3 dB = pB - pos;
                float lB = length(dB);
                float cB = dot(dB, v) / max(lB, 1e-5);
                cB = mix(low1, cB, clamp(lB * falloffMul + falloffAdd, 0.0, 1.0));
                if (lB > bias) h1c = cB > h1c ? cB : mix(h1c, cB, THIN_DECAY);
            }

            float h0 = -fastAcos(clamp(h1c, -1.0, 1.0));
            float h1 = fastAcos(clamp(h0c, -1.0, 1.0));
            h0 = nAng + clamp(h0 - nAng, -HALF_PI, HALF_PI);
            h1 = nAng + clamp(h1 - nAng, -HALF_PI, HALF_PI);
            float sinN = sin(nAng);
            float arc0 = (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - nAng)) * 0.25;
            float arc1 = (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - nAng)) * 0.25;
            accum += clamp(projLen * (arc0 + arc1), 0.0, 1.0);
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
