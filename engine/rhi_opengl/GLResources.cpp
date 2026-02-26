#include "engine/rhi_opengl/GLResources.h"

#include "engine/rhi_opengl/GLUtils.h"

namespace engine::rhi::gl {

namespace {

GLenum bufferUsageToTarget(BufferUsage usage) {
    switch (usage) {
        case BufferUsage::Vertex:
            return GL_ARRAY_BUFFER;
        case BufferUsage::Index:
            return GL_ELEMENT_ARRAY_BUFFER;
        case BufferUsage::Uniform:
            return GL_UNIFORM_BUFFER;
        default:
            return GL_ARRAY_BUFFER;
    }
}

} // namespace

GLBuffer::GLBuffer(const BufferDesc& desc, const void* initialData)
    : m_size(desc.size), m_usage(desc.usage), m_handle(reinterpret_cast<ResourceHandle>(this)) {
    const GLenum target = bufferUsageToTarget(desc.usage);
    const GLenum drawUsage = desc.dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;

    glGenBuffers(1, &m_bufferId);
    glBindBuffer(target, m_bufferId);
    glBufferData(target, static_cast<GLsizeiptr>(desc.size), initialData, drawUsage);
    glBindBuffer(target, 0);
}

GLBuffer::~GLBuffer() {
    if (m_bufferId != 0) {
        glDeleteBuffers(1, &m_bufferId);
        m_bufferId = 0;
    }
}

std::size_t GLBuffer::size() const {
    return m_size;
}

BufferUsage GLBuffer::usage() const {
    return m_usage;
}

ResourceHandle GLBuffer::handle() const {
    return m_handle;
}

std::uint32_t GLBuffer::id() const {
    return m_bufferId;
}

} // namespace engine::rhi::gl
