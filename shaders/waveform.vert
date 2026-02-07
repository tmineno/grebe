#version 450

layout(location = 0) in int inSample;

layout(push_constant) uniform PushConstants {
    float amplitude_scale;
    float vertical_offset;
    float horizontal_scale;
    float horizontal_offset;
    int   vertex_count;
    int   first_vertex;
    float color_r;
    float color_g;
    float color_b;
    float color_a;
} pc;

layout(location = 0) out vec4 vColor;

void main() {
    // X: spread vertices across [-1, 1] based on local index and vertex count
    float x = (float(gl_VertexIndex - pc.first_vertex) / float(pc.vertex_count - 1)) * 2.0 - 1.0;
    x = x * pc.horizontal_scale + pc.horizontal_offset;

    // Y: normalize int16 [-32768, 32767] to [-1, 1], then apply scale and offset
    float y = (float(inSample) / 32767.0) * pc.amplitude_scale + pc.vertical_offset;

    gl_Position = vec4(x, -y, 0.0, 1.0); // flip Y for screen coords
    vColor = vec4(pc.color_r, pc.color_g, pc.color_b, pc.color_a);
}
