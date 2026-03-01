#version 330 core

in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform vec4 uTintColor;

out vec4 fragColor;

void main() {
    fragColor = texture(uTexture, vTexCoord) * uTintColor;
}
