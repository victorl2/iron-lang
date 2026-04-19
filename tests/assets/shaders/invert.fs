#version 330

// Color-invert post-FX with intensity uniform. Iron Phase 71-02 asset.
// Exercises Shader.set_value(.float) via u_intensity (0.0 = identity, 1.0 = fully inverted).

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float u_intensity;

out vec4 finalColor;

void main() {
    vec4 texelColor = texture(texture0, fragTexCoord);
    vec3 inverted = vec3(1.0) - texelColor.rgb;
    vec3 rgb = mix(texelColor.rgb, inverted, u_intensity);
    finalColor = vec4(rgb, texelColor.a) * colDiffuse * fragColor;
}
