#include "engine/rhi_opengl/GLUtils.h"

#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "engine/core/Log.h"

namespace engine::rhi::gl {

namespace {

GLenum shaderTypeToGLenum(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:
            return GL_VERTEX_SHADER;
        case ShaderStage::Fragment:
            return GL_FRAGMENT_SHADER;
        default:
            throw std::runtime_error("Unsupported shader stage for OpenGL backend");
    }
}

} // namespace

std::string readTextFile(const std::string& path) {
    std::ifstream file(path, std::ios::in);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + path);
    }

    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::uint32_t compileShaderFromSource(
    ShaderStage stage,
    const std::string& source,
    const std::string& debugName) {
    const GLenum glStage = shaderTypeToGLenum(stage);
    const GLuint shader = glCreateShader(glStage);
    const GLchar* sourcePtr = source.c_str();
    glShaderSource(shader, 1, &sourcePtr, nullptr);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        std::array<char, 2048> infoLog{};
        glGetShaderInfoLog(shader, static_cast<GLsizei>(infoLog.size()), nullptr, infoLog.data());
        glDeleteShader(shader);
        throw std::runtime_error("GL shader compile failed (" + debugName + "): " + infoLog.data());
    }

    return shader;
}

std::uint32_t linkProgram(std::uint32_t vertexShaderId, std::uint32_t fragmentShaderId) {
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertexShaderId);
    glAttachShader(program, fragmentShaderId);
    glLinkProgram(program);

    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        std::array<char, 2048> infoLog{};
        glGetProgramInfoLog(program, static_cast<GLsizei>(infoLog.size()), nullptr, infoLog.data());
        glDeleteProgram(program);
        throw std::runtime_error(std::string("GL program link failed: ") + infoLog.data());
    }

    return program;
}

void setMat4Uniform(std::uint32_t programId, std::string_view uniformName, const glm::mat4& matrix) {
    glUseProgram(programId);
    const GLint location = glGetUniformLocation(programId, std::string(uniformName).c_str());
    if (location >= 0) {
        glUniformMatrix4fv(location, 1, GL_FALSE, &matrix[0][0]);
    }
}

void checkGlError(std::string_view where) {
    GLenum errorCode = glGetError();
    while (errorCode != GL_NO_ERROR) {
        ENGINE_LOG_WARN("OpenGL error 0x{:X} at {}", static_cast<unsigned int>(errorCode), where);
        errorCode = glGetError();
    }
}

} // namespace engine::rhi::gl
