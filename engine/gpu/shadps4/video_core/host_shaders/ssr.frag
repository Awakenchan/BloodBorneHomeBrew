// SPDX-License-Identifier: GPL-2.0-or-later

// Screen-space reflection ray march, in place of Bloodborne's SSR pixel shader (hash 6c2323c2).
// It keeps that shader's resource interface and constants (decoded from its SPIR-V) so the
// engine's descriptors, targets and the later composite stay as they are.
//
//   ubo_0 (binding 0)  [0..15]  clip position -> view position rows (x, y, z, w)
//                      [16..27] view position -> (x, y, depth) projection rows
//                      [32..42] view normal rows from the encoded normal
//                      [48..63] screen position + depth -> previous frame screen position rows
//                      [64] minimum ray facing, [68,69] depth -> view distance, [72] step scale,
//                      [76,77] hit thickness (minimum, per depth step), [78] max distance,
//                      [79] extra stride
//   img0 / samp0  depth        img1 / samp1  normals        img2 / samp2  scene colour
//
// Output RT0: premultiplied reflected colour, A = confidence (0 where the ray found nothing).
//
// What changed from the original: it stepped at most `step scale` times per screen width
// (coarse, so thin geometry was missed and hits were only linearly interpolated); this one steps
// about every pixel, refines each hit by bisection, drops the circular screen vignette that
// faded reflections out toward the corners, and instead fades only when the hit is near the
// screen border.

#version 450
#extension GL_EXT_scalar_block_layout : require

layout(scalar, set = 0, binding = 0) uniform Cb0 { uint d[16384]; } ubo_0;
layout(set = 0, binding = 1) uniform texture2D img0;
layout(set = 0, binding = 2) uniform texture2D img1;
layout(set = 0, binding = 3) uniform texture2D img2;
layout(set = 0, binding = 4) uniform sampler samp0;
layout(set = 0, binding = 5) uniform sampler samp1;
layout(set = 0, binding = 6) uniform sampler samp2;

layout(location = 1) in vec4 attr1;
layout(location = 0) out vec4 frag_color0;

layout(push_constant) uniform BbPush { layout(offset = 80) uint buf_off; };

const int MAX_STEPS = 256;
const float MIN_STEP_PX = 1.0;
const int REFINE = 6;
const float EDGE_FADE = 0.08;

uint base;
float cb(int i) { return uintBitsToFloat(ubo_0.d[base + uint(i)]); }
vec4 row(int i) { return vec4(cb(i), cb(i + 1), cb(i + 2), cb(i + 3)); }

float sceneDepth(vec2 uv) { return textureLod(sampler2D(img0, samp0), uv, 0.0).x; }

void main() {
    base = (buf_off & 0xffu) >> 2;
    frag_color0 = vec4(0.0);

    vec2 uv0 = attr1.xy;
    float d0 = sceneDepth(uv0);
    if (!(d0 < 1.0)) {
        return;
    }
    vec2 ndc = vec2(uv0.x * 2.0 - 1.0, 1.0 - uv0.y * 2.0);
    vec4 cp = vec4(ndc, d0, 1.0);
    vec4 vp = vec4(dot(row(0), cp), dot(row(4), cp), dot(row(8), cp), dot(row(12), cp));
    vec3 P = vp.xyz / vp.w;
    vec3 V = normalize(P);

    vec3 ne = normalize(textureLod(sampler2D(img1, samp1), uv0, 0.0).xyz * 2.0 - 1.0);
    vec3 N = vec3(dot(vec3(cb(32), cb(33), cb(34)), ne), dot(vec3(cb(36), cb(37), cb(38)), ne),
                  dot(vec3(cb(40), cb(41), cb(42)), ne));
    vec3 R = normalize(V - 2.0 * dot(N, V) * N);
    float minFacing = cb(64);
    if (R.z < minFacing) {
        return;
    }

    vec3 Q = P + R;
    float qz = Q.z;
    vec4 q4 = vec4(Q, 1.0);
    vec4 qp = vec4(dot(row(16), q4), dot(row(20), q4), dot(row(24), q4), 0.0);
    vec3 e = qz > 0.0 ? vec3(qp.x / qz, qp.y / qz, qp.z / qz) : vec3(ndc, d0);
    vec3 D = e - vec3(ndc, d0);
    float dl = length(D);
    if (dl < 1e-9) {
        return;
    }
    D /= dl;
    vec3 w = vec3(0.5 * D.x, -0.5 * D.y, D.z);
    w /= length(w);
    float uvLen = length(w.xy);
    if (uvLen < 1e-6) {
        return;
    }

    float stepScale = cb(72);
    float stride = 1.0 + cb(79);
    float totalLen = uvLen * stride;
    vec2 texel = 1.0 / vec2(textureSize(sampler2D(img0, samp0), 0));
    float stepLen = min(stride / stepScale, MIN_STEP_PX * min(texel.x, texel.y));
    stepLen = max(stepLen, totalLen / float(MAX_STEPS));
    int steps = int(totalLen / stepLen);
    vec3 stepV = w * (stepLen / uvLen); // uv x, uv y, depth per step

    float minThick = cb(76);
    float thickSlope = cb(77);
    float thick = max(minThick, thickSlope * stepV.z);
    float maxDist = cb(78);
    float dA = cb(68);
    float dB = cb(69);

    vec3 start = vec3(uv0, d0);
    float hitT = -1.0;
    float prevT = 0.0;
    for (int i = 1; i <= steps; ++i) {
        vec3 p = start + stepV * float(i);
        if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z >= 1.0) {
            break;
        }
        if (maxDist < dB / (p.z - dA)) {
            break;
        }
        float s = sceneDepth(p.xy);
        float diff = p.z - s;
        if (diff >= 0.0 && diff < thick) {
            hitT = float(i);
            break;
        }
        prevT = float(i);
    }
    if (hitT < 0.0) {
        return;
    }

    float lo = prevT;
    float hi = hitT;
    for (int k = 0; k < REFINE; ++k) {
        float mid = 0.5 * (lo + hi);
        vec3 p = start + stepV * mid;
        if (p.z - sceneDepth(p.xy) >= 0.0) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    vec3 hp = start + stepV * hi;
    float hs = sceneDepth(hp.xy);

    vec4 hc = vec4(hp.x * 2.0 - 1.0, 1.0 - hp.y * 2.0, hs, 1.0);
    float hx = dot(row(48), hc);
    float hy = dot(row(52), hc);
    float hw = dot(row(60), hc);
    vec2 puv = vec2(0.5 + 0.5 * hx / hw, 0.5 - 0.5 * hy / hw);
    if (!(hw > 0.0) || puv.x < 0.0 || puv.x > 1.0 || puv.y < 0.0 || puv.y > 1.0) {
        return;
    }
    vec3 col = textureLod(sampler2D(img2, samp2), puv, 0.0).rgb;
    if (any(isnan(col)) || any(isinf(col))) {
        return;
    }

    float facing = clamp(R.z - minFacing, 0.0, 1.0);
    vec2 edge = min(puv, 1.0 - puv) / EDGE_FADE;
    float edgeFade = clamp(min(edge.x, edge.y), 0.0, 1.0);
    float conf = min(1.0, facing * facing) * edgeFade;
    frag_color0 = vec4(col * conf, conf);
}
