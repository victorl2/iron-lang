#version 330

// raylib standard MVP vertex pass-through. Iron Phase 71-02 asset.
// Paired with fragment shaders in this directory.

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;

uniform mat4 mvp;

out vec2 fragTexCoord;
out vec4 fragColor;

void main() {
    fragTexCoord = vertexTexCoord;
    fragColor    = vertexColor;
    gl_Position  = mvp * vec4(vertexPosition, 1.0);
}
