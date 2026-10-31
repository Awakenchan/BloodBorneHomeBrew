// SPDX-License-Identifier: GPL-2.0-or-later

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

layout(push_constant) uniform BbPush {
    layout(offset = 80) uint buf_off;
    layout(offset = 120) uint frame_index;
};

float radical(uint i, uint b) {
    float f = 1.0, r = 0.0;
    for (i += 1u; i > 0u; i /= b) {
        f /= float(b);
        r += f * float(i % b);
    }
    return r;
}

vec2 halton(uint i) {
    i &= 15u;
    return vec2(radical(i, 2u), radical(i, 3u));
}

const int MAX_STEPS = 256;
const float MIN_STEP_PX = 1.0;
const int REFINE = 6;
const float EDGE_FADE = 0.08;
const float GEO_BLEND = 0.6;

uint base;
float cb(int i) { return uintBitsToFloat(ubo_0.d[base + uint(i)]); }
vec4 row(int i) { return vec4(cb(i), cb(i + 1), cb(i + 2), cb(i + 3)); }

float sceneDepth(vec2 uv) { return textureLod(sampler2D(img0, samp0), uv, 0.0).x; }

vec3 viewPosAt(vec2 uv, float depth) {
    vec4 cp = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    vec4 vp = vec4(dot(row(0), cp), dot(row(4), cp), dot(row(8), cp), dot(row(12), cp));
    return vp.xyz / vp.w;
}

vec3 viewPos(vec2 uv) { return viewPosAt(uv, sceneDepth(uv)); }

vec3 viewNormal(vec2 uv) {
    vec3 ne = normalize(textureLod(sampler2D(img1, samp1), uv, 0.0).xyz * 2.0 - 1.0);
    return vec3(dot(vec3(cb(32), cb(33), cb(34)), ne), dot(vec3(cb(36), cb(37), cb(38)), ne),
                dot(vec3(cb(40), cb(41), cb(42)), ne));
}

vec3 stableNormal(vec2 uv) {
    vec2 t = 1.0 / vec2(textureSize(sampler2D(img0, samp0), 0));
    vec3 center = normalize(viewNormal(uv));
    vec3 p = viewPos(uv);
    vec3 n = center;
    for (int i = 0; i < 8; ++i) {
        const vec2 k[8] = vec2[8](vec2(-1, -1), vec2(0, -1), vec2(1, -1), vec2(-1, 0), vec2(1, 0),
                                  vec2(-1, 1), vec2(0, 1), vec2(1, 1));
        vec2 s = uv + k[i] * t * 1.5;
        vec3 sample_n = normalize(viewNormal(s));
        float dz = abs(viewPos(s).z - p.z) / max(p.z, 0.001);
        float weight = (1.0 - smoothstep(0.01, 0.03, dz)) *
                       smoothstep(0.8, 0.95, dot(center, sample_n));
        n += sample_n * weight;
    }
    n = normalize(n);

    vec3 px = viewPos(uv + vec2(t.x, 0.0));
    vec3 nx = viewPos(uv - vec2(t.x, 0.0));
    vec3 py = viewPos(uv + vec2(0.0, t.y));
    vec3 ny = viewPos(uv - vec2(0.0, t.y));
    vec3 dx = abs(px.z - p.z) < abs(p.z - nx.z) ? px - p : p - nx;
    vec3 dy = abs(py.z - p.z) < abs(p.z - ny.z) ? py - p : p - ny;
    vec3 g = cross(dx, dy);
    if (dot(g, g) > 1e-12) {
        g = normalize(g);
        if (dot(g, p) > 0.0) {
            g = -g;
        }
        float continuity = max(abs(dx.z), abs(dy.z)) / max(p.z, 0.001);
        float weight = (1.0 - smoothstep(0.01, 0.03, continuity)) *
                       smoothstep(0.9, 0.98, dot(g, center));
        if (weight > 0.0) {
            n = normalize(mix(n, g, GEO_BLEND * weight));
        }

        float footprint = max(max(length(px - p), length(nx - p)),
                              max(length(py - p), length(ny - p)));
        float plane_error = max(max(abs(dot(g, px - p)), abs(dot(g, nx - p))),
                                max(abs(dot(g, py - p)), abs(dot(g, ny - p)))) /
                            max(footprint, 0.0001);
        float planar = (1.0 - smoothstep(0.02, 0.08, plane_error)) *
                       (1.0 - smoothstep(0.01, 0.03, continuity)) *
                       smoothstep(0.8, 0.95, dot(g, center));
        vec3 ripple = n - g * dot(n, g);
        ripple *= min(1.0, 0.02 / max(length(ripple), 0.0001));
        n = normalize(mix(n, normalize(g + ripple), planar));
    }
    return n;
}

vec4 trace(vec2 uv0, vec3 N) {
    float d0 = sceneDepth(uv0);
    if (!(d0 < 1.0)) {
        return vec4(0.0);
    }
    vec2 ndc = vec2(uv0.x * 2.0 - 1.0, 1.0 - uv0.y * 2.0);
    vec4 cp = vec4(ndc, d0, 1.0);
    vec4 vp = vec4(dot(row(0), cp), dot(row(4), cp), dot(row(8), cp), dot(row(12), cp));
    vec3 P = vp.xyz / vp.w;
    vec3 V = normalize(P);

    vec3 R = normalize(V - 2.0 * dot(N, V) * N);
    float minFacing = cb(64);
    if (R.z < minFacing) {
        return vec4(0.0);
    }

    vec3 Q = P + R;
    float qz = Q.z;
    vec4 q4 = vec4(Q, 1.0);
    vec4 qp = vec4(dot(row(16), q4), dot(row(20), q4), dot(row(24), q4), 0.0);
    vec3 e = qz > 0.0 ? vec3(qp.x / qz, qp.y / qz, qp.z / qz) : vec3(ndc, d0);
    vec3 D = e - vec3(ndc, d0);
    float dl = length(D);
    if (dl < 1e-9) {
        return vec4(0.0);
    }
    D /= dl;
    vec3 w = vec3(0.5 * D.x, -0.5 * D.y, D.z);
    w /= length(w);
    float uvLen = length(w.xy);
    if (uvLen < 1e-6) {
        return vec4(0.0);
    }

    float stepScale = cb(72);
    float stride = 1.0 + cb(79);
    float totalLen = uvLen * stride;
    vec2 texel = 1.0 / vec2(textureSize(sampler2D(img0, samp0), 0));
    float stepLen = min(stride / stepScale, MIN_STEP_PX * min(texel.x, texel.y));
    stepLen = max(stepLen, totalLen / float(MAX_STEPS));
    int steps = int(totalLen / stepLen);
    vec3 stepV = w * (stepLen / uvLen);

    float minThick = cb(76);
    float thickSlope = cb(77);
    float thick = max(minThick, thickSlope * stepV.z);
    float maxDist = cb(78);
    float dA = cb(68);
    float dB = cb(69);

    vec3 start = vec3(uv0, d0);
    float hitT = -1.0;
    float prevT = 0.0;
    float prevDiff = 0.0;
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
        if (prevDiff < 0.0 && diff >= 0.0 && diff < thick && s < 1.0) {
            float lo = prevT;
            float hi = float(i);
            for (int k = 0; k < REFINE; ++k) {
                float mid = 0.5 * (lo + hi);
                vec3 candidate = start + stepV * mid;
                if (candidate.z - sceneDepth(candidate.xy) >= 0.0) hi = mid;
                else lo = mid;
            }
            vec3 candidate = start + stepV * hi;
            float surface_depth = sceneDepth(candidate.xy);
            vec3 surface = viewPosAt(candidate.xy, surface_depth);
            vec3 ray = viewPosAt(candidate.xy, candidate.z);
            vec3 delta = surface - P;
            float distance_along_ray = dot(delta, R);
            float tolerance = clamp(0.002 * abs(surface.z), 0.02, 0.2);
            float off_ray = length(delta - R * distance_along_ray);
            if (surface_depth < 1.0 && !any(isnan(surface)) && !any(isinf(surface)) &&
                distance_along_ray > max(0.02, 0.001 * abs(P.z)) &&
                abs(surface.z - ray.z) <= tolerance && off_ray <= tolerance) {
                hitT = hi;
                break;
            }
        }
        prevT = float(i);
        prevDiff = diff;
    }
    if (hitT < 0.0) {
        return vec4(0.0);
    }

    vec3 hp = start + stepV * hitT;
    float hs = sceneDepth(hp.xy);

    vec4 hc = vec4(hp.x * 2.0 - 1.0, 1.0 - hp.y * 2.0, hs, 1.0);
    float hx = dot(row(48), hc);
    float hy = dot(row(52), hc);
    float hw = dot(row(60), hc);
    vec2 puv = vec2(0.5 + 0.5 * hx / hw, 0.5 - 0.5 * hy / hw);
    if (!(hw > 0.0) || puv.x < 0.0 || puv.x > 1.0 || puv.y < 0.0 || puv.y > 1.0) {
        return vec4(0.0);
    }
    vec3 col = textureLod(sampler2D(img2, samp2), puv, 0.0).rgb;
    if (any(isnan(col)) || any(isinf(col))) {
        return vec4(0.0);
    }

    float facing = clamp(R.z - minFacing, 0.0, 1.0);
    vec2 edge = min(puv, 1.0 - puv) / EDGE_FADE;
    float edgeFade = clamp(min(edge.x, edge.y), 0.0, 1.0);
    float conf = min(1.0, facing * facing) * edgeFade;
    return vec4(col * conf, conf);
}

void main() {
    base = (buf_off & 0xffu) >> 2;

    vec2 texel = 1.0 / vec2(textureSize(sampler2D(img0, samp0), 0));
    vec2 c = attr1.xy;
    vec3 N = stableNormal(c);
    vec4 sum = vec4(0.0);
    for (uint i = 0u; i < 2u; ++i) {
        sum += trace(c + (halton(frame_index * 2u + i) - 0.5) * texel, N);
    }
    frag_color0 = sum * 0.5;
}
