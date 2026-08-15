#include "stdafx.h"
#include "dynamicbuffer.h"

// we want to introduce a cpu stall if the gpu is still using the buffer
#define WANT_TO_STALL

namespace Render
{

// we're doing manual triple buffering
constexpr int BufferCount = 3;

struct GLBuffer
{
    GLuint handle;
    uint8_t *mapped;
};

class DynamicBuffer
{
    const GLenum m_target;
    int m_bufferSize;

    int m_offset{};
    GLBuffer m_buffers[BufferCount]{};

#ifdef SCHIZO_DEBUG
    bool m_writingRegion{};
#endif

    std::vector<GLuint> m_deleteQueues[BufferCount];

public:
    DynamicBuffer(GLenum target, const int byteSize)
        : m_target{ target }
        , m_bufferSize{ byteSize }
    {
    }

    void Init()
    {
        for (GLBuffer &buffer : m_buffers)
        {
            glGenBuffers(1, &buffer.handle);
            glBindBuffer(m_target, buffer.handle);
            glBufferData(m_target, m_bufferSize, nullptr, GL_STREAM_DRAW);
        }
    }

    void Map(int index)
    {
        GL3_ASSERT(index >= 0 && index < BufferCount);
        GLBuffer &buffer = m_buffers[index];
        GL3_ASSERT(!buffer.mapped);

        glBindBuffer(m_target, buffer.handle);

        void *mapped;
        if (GLAD_GL_ARB_map_buffer_range)
        {
            int flags = GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT;
#ifndef WANT_TO_STALL
            flags |= GL_MAP_INVALIDATE_BUFFER_BIT;
#endif
            mapped = glMapBufferRange(m_target, 0, m_bufferSize, flags);
        }
        else
        {
#ifndef WANT_TO_STALL
            // just hope that this will work...
            glBufferData(m_target, m_bufferSize, nullptr, GL_STREAM_DRAW);
#endif
            mapped = glMapBuffer(m_target, GL_WRITE_ONLY);
        }

        GL3_ASSERT(mapped);
        buffer.mapped = static_cast<uint8_t *>(mapped);
        GL3_ASSERT(m_offset == 0);
    }

    void Unmap(int index)
    {
        GL3_ASSERT(index >= 0 && index < BufferCount);
        for (int i = 0; i < BufferCount; i++)
        {
            if (i != index)
            {
                GL3_ASSERT(!m_buffers[i].mapped);
            }
        }

        GLBuffer &buffer = m_buffers[index];
        GL3_ASSERT(buffer.mapped);

        glBindBuffer(m_target, buffer.handle);

        if (GLAD_GL_ARB_map_buffer_range)
        {
            glFlushMappedBufferRange(m_target, 0, m_offset);
        }

        glUnmapBuffer(m_target);
        buffer.mapped = nullptr;

#ifdef SCHIZO_DEBUG
        switch (m_target)
        {
        case GL_ARRAY_BUFFER:
            g_state.vertexBufferSize += m_offset;
            break;

        case GL_ELEMENT_ARRAY_BUFFER:
            g_state.indexBufferSize += m_offset;
            break;

        case GL_UNIFORM_BUFFER:
            g_state.uniformBufferSize += m_offset;
            break;
        }
#endif

        m_offset = 0;
    }

    void BeginFrame(int index)
    {
#ifdef SCHIZO_DEBUG
        switch (m_target)
        {
        case GL_ARRAY_BUFFER:
            g_state.vertexBufferSize = 0;
            break;

        case GL_ELEMENT_ARRAY_BUFFER:
            g_state.indexBufferSize = 0;
            break;

        case GL_UNIFORM_BUFFER:
            g_state.uniformBufferSize = 0;
            break;
        }
#endif
        std::vector<GLuint> &queue = m_deleteQueues[index];
        if (queue.size())
        {
            glDeleteBuffers(queue.size(), queue.data());
            queue.clear();
        }
    }

    static int ComputeNewSize(int oldSize, int requiredSize)
    {
        while (oldSize < requiredSize)
        {
            oldSize *= 2;
        }

        return oldSize;
    }

    void ResizeBuffers(int currentIndex, int requiredSize)
    {
        Unmap(currentIndex);

        // can't delete these right away since we haven't issued the draw calls
        for (GLBuffer &buffer : m_buffers)
        {
            GL3_ASSERT(!buffer.mapped);
            m_deleteQueues[currentIndex].push_back(buffer.handle);
        }

        int oldSize = m_bufferSize;
        m_bufferSize = ComputeNewSize(m_bufferSize, requiredSize);
        g_engfuncs.Con_Printf("WARNING: resizing dynamic buffers: %04x: %d --> %d bytes\n", m_target, oldSize, m_bufferSize);

        for (GLBuffer &buffer : m_buffers)
        {
            glGenBuffers(1, &buffer.handle);
            glBindBuffer(m_target, buffer.handle);
            glBufferData(m_target, m_bufferSize, nullptr, GL_STREAM_DRAW);
        }

        Map(currentIndex);
    }

    BufferSpan BeginRegion(int index, int maxSize, int alignment)
    {
#ifdef SCHIZO_DEBUG
        GL3_ASSERT(!m_writingRegion);
        m_writingRegion = true;
#endif

        m_offset = AlignUp(m_offset, alignment);
        if (m_offset + maxSize > m_bufferSize)
        {
            ResizeBuffers(index, m_offset + maxSize);
        }

        GLBuffer &buffer = m_buffers[index];

        BufferSpan span;
        span.buffer = buffer.handle;
        span.byteOffset = m_offset;
        span.data = &buffer.mapped[m_offset];

        return span;
    }

    void EndRegion(int finalSize)
    {
#ifdef SCHIZO_DEBUG
        GL3_ASSERT(m_writingRegion);
        m_writingRegion = false;
#endif

        // m_offset was aligned by BeginRegion
        GL3_ASSERT(m_offset + finalSize <= m_bufferSize);
        m_offset += finalSize;
    }
};

// current index of the dynamic buffers, so [0, BufferCount[
static int s_bufferFrame;

static int s_uniformBufferOffsetAlignment;

static DynamicBuffer s_vertex{ GL_ARRAY_BUFFER, 1 << 19 };
static DynamicBuffer s_index{ GL_ELEMENT_ARRAY_BUFFER, 1 << 19 };
static DynamicBuffer s_uniform{ GL_UNIFORM_BUFFER, 1 << 19 };

// only used when GLAD_GL_ARB_uniform_buffer_object is not supported
// could trivially make this reallocating, but eh
constexpr int FlatUboSize = 1 << 21;
uint8_t g_flatUboData[FlatUboSize];
static int s_flatUboOffset;

void dynamicBuffersInit()
{
    s_vertex.Init();
    s_index.Init();

    if (GLAD_GL_ARB_uniform_buffer_object)
    {
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &s_uniformBufferOffsetAlignment);
        s_uniform.Init();
    }
}

void dynamicBuffersMap()
{
    s_vertex.Map(s_bufferFrame);
    s_vertex.BeginFrame(s_bufferFrame);

    s_index.Map(s_bufferFrame);
    s_index.BeginFrame(s_bufferFrame);

    if (GLAD_GL_ARB_uniform_buffer_object)
    {
        s_uniform.Map(s_bufferFrame);
        s_uniform.BeginFrame(s_bufferFrame);
    }
    else
    {
        s_flatUboOffset = 0;
    }
}

void dynamicBuffersUnmap()
{
    s_vertex.Unmap(s_bufferFrame);
    s_index.Unmap(s_bufferFrame);

    if (GLAD_GL_ARB_uniform_buffer_object)
    {
        s_uniform.Unmap(s_bufferFrame);
    }

    s_bufferFrame = (s_bufferFrame + 1) % BufferCount;
}

BufferSpan dynamicVertexDataBegin(int maxVertexCount, int vertexSize)
{
    return s_vertex.BeginRegion(s_bufferFrame, maxVertexCount * vertexSize, vertexSize);
}

void dynamicVertexDataEnd(int actualVertexCount, int vertexSize)
{
    s_vertex.EndRegion(actualVertexCount * vertexSize);
}

BufferSpan dynamicIndexDataBegin(int maxIndexCount, int indexSize)
{
    return s_index.BeginRegion(s_bufferFrame, maxIndexCount * indexSize, indexSize);
}

void dynamicIndexDataEnd(int actualIndexCount, int indexSize)
{
    s_index.EndRegion(actualIndexCount * indexSize);
}

BufferSpan dynamicUniformData(const void *data, int size)
{
    if (!GLAD_GL_ARB_uniform_buffer_object)
    {
        int offset = AlignUp(s_flatUboOffset, 16);
        if (offset + size > FlatUboSize)
        {
            platformError("Flat UBO overflow");
        }

        memcpy(&g_flatUboData[offset], data, size);
        s_flatUboOffset = offset + size;

        BufferSpan result;
        result.buffer = ~0u; // can't use 0 since it's treated as invalid
        result.byteOffset = offset;
        result.data = &g_flatUboData[offset];
        return result;
    }

    BufferSpan result = s_uniform.BeginRegion(s_bufferFrame, size, s_uniformBufferOffsetAlignment);
    memcpy(result.data, data, size);
    s_uniform.EndRegion(size);
    return result;
}

}
