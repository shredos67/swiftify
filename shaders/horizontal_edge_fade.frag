#version 440
// scrolling text that fades out at its left and right edges, as the
// immersive player's long titles do. the left fade grows in once the text
// has moved off its start.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    // how far in from each edge the fade reaches, as a fraction of the width
    float leftEdge;
    float rightEdge;
};

layout(binding = 1) uniform sampler2D source;

void main()
{
    float x = qt_TexCoord0.x;
    float left = leftEdge > 0.0 ? clamp(x / leftEdge, 0.0, 1.0) : 1.0;
    float right = rightEdge > 0.0 ? clamp((1.0 - x) / rightEdge, 0.0, 1.0) : 1.0;
    fragColor = texture(source, qt_TexCoord0) * (left * right * qt_Opacity);
}
