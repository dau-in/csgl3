#ifndef DYNAMICBUFFER_H
#define DYNAMICBUFFER_H

namespace Render
{
template<typename T>
struct BufferSpanT
{
    GLuint buffer;
    int byteOffset; // offset in buffer object
    T *data; // agp pinned memory at byteOffset
};

using BufferSpan = BufferSpanT<void>;

// used when the implementation doesn't support constant buffers,
// the constant data gets copied to corresponding registers from here
extern uint8_t g_flatUboData[];

void dynamicBuffersInit();

void dynamicBuffersMap();
void dynamicBuffersUnmap();

BufferSpan dynamicVertexDataBegin(int maxVertexCount, int vertexSize);
void dynamicVertexDataEnd(int actualVertexCount, int vertexSize);
BufferSpan dynamicIndexDataBegin(int maxIndexCount, int indexSize);
void dynamicIndexDataEnd(int actualIndexCount, int indexSize);

// this can cause redundant copying, but is easier to use
BufferSpan dynamicUniformData(const void *data, int size);

template<typename VertexType>
BufferSpanT<VertexType> dynamicVertexDataBegin(int maxVertexCount)
{
    BufferSpan span = dynamicVertexDataBegin(maxVertexCount, sizeof(VertexType));
    return { span.buffer, span.byteOffset, static_cast<VertexType *>(span.data) };
}

template<typename VertexType>
void dynamicVertexDataEnd(int actualVertexCount)
{
    dynamicVertexDataEnd(actualVertexCount, sizeof(VertexType));
}

template<typename IndexType>
BufferSpanT<IndexType> dynamicIndexDataBegin(int maxIndexCount)
{
    BufferSpan span = dynamicIndexDataBegin(maxIndexCount, sizeof(IndexType));
    return { span.buffer, span.byteOffset, static_cast<IndexType *>(span.data) };
}

template<typename IndexType>
void dynamicIndexDataEnd(int actualIndexCount)
{
    dynamicIndexDataEnd(actualIndexCount, sizeof(IndexType));
}

}

#endif
