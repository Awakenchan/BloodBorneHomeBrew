// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

layout(scalar, set = 0, binding = 0) uniform Constants { uint data[16384]; } constants;
layout(set = 0, binding = 1) uniform texture2D color_image;
layout(set = 0, binding = 2) uniform texture2D velocity_image;
layout(set = 0, binding = 3) uniform sampler color_sampler;
layout(set = 0, binding = 4) uniform sampler velocity_sampler;
layout(push_constant) uniform Push { layout(offset = 80) uint buffer_offset; } pc;
layout(location = 0) in vec4 uv;
layout(location = 0) out vec4 color;

float blurConstant(int index) {
    return uintBitsToFloat(constants.data[index + ((pc.buffer_offset & 255u) >> 2u)]);
}
vec4 blurColor(vec2 p) { return textureLod(sampler2D(color_image, color_sampler), p, 0.0); }
vec2 blurVelocity(vec2 p) {
    return textureLod(sampler2D(velocity_image, velocity_sampler), p, 0.0).rg;
}
ivec2 blurSize() { return textureSize(sampler2D(color_image, color_sampler), 0); }
#include "native_motion_blur.glsl"

void main() { color = nativeMotionBlur(uv.xy, true); }
