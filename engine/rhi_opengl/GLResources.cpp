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
    : m_size(desc.size),
      m_usage(desc.usage),
      m_vertexLayout(desc.vertexLayout),
      m_handle(reinterpret_cast<ResourceHandle>(this)) {
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

VertexLayout GLBuffer::vertexLayout() const {
    return m_vertexLayout;
}

std::uint32_t GLBuffer::id() const {
    return m_bufferId;
}

GLImage::GLImage(const ImageDesc& desc, const void* initialData)
    : m_width(desc.width),
      m_height(desc.height),
      m_handle(reinterpret_cast<ResourceHandle>(this)) {
    glGenTextures(1, &m_textureId);
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, desc.generateMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        static_cast<GLsizei>(desc.width),
        static_cast<GLsizei>(desc.height),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        initialData);

    if (desc.generateMipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

GLImage::~GLImage() {
    if (m_textureId != 0) {
        glDeleteTextures(1, &m_textureId);
        m_textureId = 0;
    }
}

std::uint32_t GLImage::width() const {
    return m_width;
}

std::uint32_t GLImage::height() const {
    return m_height;
}

ResourceHandle GLImage::handle() const {
    return m_handle;
}

std::uint32_t GLImage::id() const {
    return m_textureId;
}

} // namespace engine::rhi::gl
