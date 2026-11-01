// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
struct Record { vec4 attributes[4]; };
layout(std430, set=0, binding=2) readonly buffer Records { Record records[]; };
layout(std430, set=0, binding=3) readonly buffer Indices { uint indices[]; };
layout(push_constant) uniform Params {
    int base_vertex;
    uint first_index;
    uint first_instance;
} params;

#ifdef INSTANCED
layout(location=0) out vec4 color;
layout(location=1) out vec4 properties;
layout(location=2) out vec4 uv;
void main() {
    const vec2 corners[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
    vec2 corner = corners[gl_VertexIndex];
    uint ordinal = gl_InstanceIndex - params.first_instance + params.first_index;
    Record record = records[int(indices[ordinal]) + params.base_vertex];
    precise vec2 offset = record.attributes[3].xy * (2.0 * corner - 1.0);
    precise vec2 position = record.attributes[2].xy + offset;
    gl_Position = vec4(position, record.attributes[2].zw);
    color = record.attributes[0];
    properties = record.attributes[1];
    uv = vec4(corner.x, 1.0 - corner.y, record.attributes[3].z, 1.0);
}
#else
layout(location=0) out vec4 attributes[4];
void main() {
    for (int i=0; i<4; ++i) attributes[i] = records[gl_VertexIndex].attributes[i];
    gl_Position = records[gl_VertexIndex].attributes[2];
}
#endif
