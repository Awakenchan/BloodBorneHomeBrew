// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(location=0) in vec4 color;
layout(location=1) in vec4 properties;
layout(location=2) in vec4 uv;
layout(location=0) out vec4 out_color;
layout(location=1) out vec4 out_properties;
layout(location=2) out vec4 out_uv;
layout(location=3) out vec4 out_position;
void main() {
    out_color = color;
    out_properties = properties;
    out_uv = uv;
    out_position = gl_FragCoord;
}
