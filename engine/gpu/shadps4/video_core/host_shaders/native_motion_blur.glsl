// SPDX-License-Identifier: GPL-2.0-or-later

vec2 decodeMotion(vec2 packed) {
    return packed * vec2(blurConstant(744), blurConstant(745)) +
                    vec2(blurConstant(746), blurConstant(747));
}

vec4 nativeMotionBlur(vec2 uv, bool high_quality) {
    vec4 center = blurColor(uv);
    vec2 packed = blurVelocity(uv);
    vec2 motion = decodeMotion(packed);
    if (abs(motion.x) + abs(motion.y) < blurConstant(754)) return center;

    vec2 step_uv = packed * vec2(blurConstant(748), blurConstant(749)) +
                           vec2(blurConstant(750), blurConstant(751));
    int half_samples = 2;
    if (high_quality) {
        float radius_pixels = length(2.0 * step_uv * vec2(blurSize()));
        half_samples = int(clamp(ceil(radius_pixels), 2.0, 16.0));
    }
    float depth_limit = center.a + blurConstant(757);
    float agreement = dot(motion, motion) * blurConstant(756);
    vec4 total = center;
    float count = 1.0;
    for (int side = -1; side <= 1; side += 2) {
        for (int i = 1; i <= half_samples; ++i) {
            vec2 tap_uv = uv + float(side) * (2.0 * float(i) / float(half_samples)) * step_uv;
            vec2 bounds = tap_uv * vec2(blurConstant(760), blurConstant(761)) +
                                  vec2(blurConstant(762), blurConstant(763));
            if (any(greaterThan(abs(bounds), vec2(1.0)))) continue;
            vec4 value = blurColor(tap_uv);
            vec2 other_motion = decodeMotion(blurVelocity(tap_uv));
            if (value.a >= depth_limit || dot(other_motion, motion) > agreement) {
                total += value;
                count += 1.0;
            }
        }
    }
    vec4 gain = vec4(blurConstant(636), blurConstant(637),
                     blurConstant(638), blurConstant(639));
    return total * (gain / count);
}
