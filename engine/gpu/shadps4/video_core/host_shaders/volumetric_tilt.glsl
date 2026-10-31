// SPDX-License-Identifier: GPL-3.0-or-later
vec3 shaft_direction(vec3 direction, vec3 up, float degrees) {
    if (degrees <= 0.0) return direction;
    float vertical = clamp(dot(direction, up), -1.0, 1.0);
    vec3 horizontal = direction - vertical * up;
    // A perfectly vertical light has no defined azimuth to preserve.
    if (dot(horizontal, horizontal) < 1e-10) return direction;
    float elevation = max(0.0, asin(abs(vertical)) - radians(degrees));
    return normalize(horizontal) * cos(elevation) + up * sign(vertical) * sin(elevation);
}

vec2 shaft_shadow_uv(vec3 sample_pos, float blocker_depth, vec3 light_step) {
    // One blocker-relative parallax step, not a re-rendered shadow map.
    return sample_pos.xy + light_step.xy * ((blocker_depth - sample_pos.z) / light_step.z);
}

bool shaft_inside_tile(vec2 uv, int cascade, vec2 texel) {
    vec2 origin = vec2(cascade & 1, cascade >> 1) * 0.5;
    return all(greaterThanEqual(uv, origin + texel)) &&
           all(lessThanEqual(uv, origin + 0.5 - texel));
}
