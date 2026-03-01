#version 330 core

in vec3 vColor;
uniform vec4 uTintColor;
out vec4 fragColor;

void main() {
    fragColor = vec4(vColor, 1.0) * uTintColor;
}
