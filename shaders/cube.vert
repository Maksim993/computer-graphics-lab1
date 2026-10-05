#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 tint;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 0) out vec3 v_color;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);

    // Процедурный цвет из локальной позиции.
    // Каналы специально распределены по разным координатам куба.
    vec3 procedural = vec3(
        in_pos.x,
        in_pos.z,
        in_pos.y
    ) * 0.5 + 0.5;

    v_color = clamp(procedural * pc.tint.rgb, 0.0, 1.0);
}
