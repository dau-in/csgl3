#include "stdafx.h"
#include "immediate.h"
#include "commandbuffer.h"
#include "dynamicbuffer.h"
#include "memory.h"

namespace Render
{

struct ImmediateVertex
{
    Vector3 position;
    Vector2 texCoord;
    uint8_t color[4];
};

static const VertexAttrib s_vertexAttribs[] = {
    { &ImmediateVertex::position, "a_position" },
    { &ImmediateVertex::texCoord, "a_texCoord" },
    { &ImmediateVertex::color, "a_color", true },
};

static const VertexFormat s_vertexFormat{ sizeof(ImmediateVertex), s_vertexAttribs };

static const ShaderUniform s_uniforms[] = {
    { "u_texture", 0 }
};

static constexpr ShaderOption s_shaderOptions[] = {
    { "ALPHA_TEST", 1 }
};

// must match s_shaderOptions
struct SpriteShaderOptions
{
    unsigned alphaTest;
};

static BaseShader s_shaders[shaderVariantCount(s_shaderOptions)];

static bool s_active;

static std::vector<ImmediateVertex> s_stageVertices;
static std::vector<uint16_t> s_stageIndices;

static GLenum s_currentMode;
static size_t s_primitiveStartVertex;

// current vertex attributes
static ImmediateVertex s_currentVertex;

static void Flush()
{
    if (s_stageIndices.empty())
    {
        return;
    }

    // FIXME: vertex count can technically overflow index max
    int vertexCount = (static_cast<int>(s_stageVertices.size()));
    auto vertexSpan = dynamicVertexDataBegin<ImmediateVertex>(vertexCount);
    std::copy(s_stageVertices.begin(), s_stageVertices.end(), vertexSpan.data);
    dynamicVertexDataEnd<ImmediateVertex>(vertexCount);

    int indexCount = static_cast<int>(s_stageIndices.size());
    auto indexSpan = dynamicIndexDataBegin<uint16_t>(static_cast<int>(s_stageIndices.size()));
    std::copy(s_stageIndices.begin(), s_stageIndices.end(), indexSpan.data);
    dynamicIndexDataEnd<uint16_t>(indexCount);

    // FIXME: it would be nice if the base vertex didn't change as often...
    int baseVertex = vertexSpan.byteOffset / sizeof(ImmediateVertex);
    commandBindVertexBuffer(vertexSpan.buffer, s_vertexFormat, baseVertex);

    commandBindIndexBuffer(indexSpan.buffer);

    commandDrawElements(
        GL_TRIANGLES,
        indexCount,
        GL_UNSIGNED_SHORT,
        indexSpan.byteOffset);

    s_stageVertices.clear();
    s_stageIndices.clear();
}

// FIXME: isn't this too small? our dynamic vertex buffers are tiny
constexpr int ReserveVertexCount = 16384;

// all quad strips is the worst case for now
constexpr int ReserveIndexCount = (ReserveVertexCount * 6) / 2;

void immediateInit()
{
    s_stageVertices.reserve(ReserveVertexCount);
    s_stageIndices.reserve(ReserveIndexCount);
    shaderRegister(s_shaders, "sprite", s_vertexAttribs, s_uniforms, s_shaderOptions);
}

void immediateDrawStart(bool alphaTest)
{
    GL3_ASSERT(!s_active);
    s_active = true;

    SpriteShaderOptions options{};
    options.alphaTest = alphaTest ? 1 : 0;
    BaseShader &shader = shaderSelect(s_shaders, s_shaderOptions, options);
    commandUseProgram(&shader);
}

void immediateDrawEnd()
{
    GL3_ASSERT(s_active);

    Flush();

    // restore state... this is dumb but no other way currently
    commandBlendEnable(GL_FALSE);
    commandDepthTest(GL_TRUE);
    commandDepthMask(GL_TRUE);
    commandCullFace(GL_TRUE);

    s_active = false;
}

bool immediateIsActive()
{
    return s_active;
}

void immediateBlendEnable(GLboolean enable)
{
    GL3_ASSERT(s_active);

    if (g_shadowState.blendEnable != enable)
    {
        Flush();
        commandBlendEnable(enable);
    }
}

void immediateBlendFunc(GLenum sfactor, GLenum dfactor)
{
    GL3_ASSERT(s_active);

    if (g_shadowState.blendSrc != sfactor || g_shadowState.blendDst != dfactor)
    {
        Flush();
        commandBlendFunc(sfactor, dfactor);
    }
}

void immediateCullFace(GLboolean enable)
{
    GL3_ASSERT(s_active);

    if (g_shadowState.cullFace != enable)
    {
        Flush();
        commandCullFace(enable);
    }
}

void immediateDepthTest(GLboolean enable)
{
    GL3_ASSERT(s_active);

    if (g_shadowState.depthTest != enable)
    {
        Flush();
        commandDepthTest(enable);
    }
}

void immediateDepthMask(GLboolean flag)
{
    GL3_ASSERT(s_active);

    if (g_shadowState.depthMask != flag)
    {
        Flush();
        commandDepthMask(flag);
    }
}

void immediateBindTexture(GLuint texture)
{
    GL3_ASSERT(s_active);

    if (g_shadowState.texture2Ds[0] != texture)
    {
        Flush();
        commandBindTexture(0, GL_TEXTURE_2D, texture);
    }
}

void immediateBegin(GLenum mode)
{
    GL3_ASSERT(s_active);

    s_currentMode = mode;
    s_primitiveStartVertex = s_stageVertices.size();
}

void immediateColor4f(float r, float g, float b, float a)
{
    GL3_ASSERT(s_active);

    s_currentVertex.color[0] = static_cast<uint8_t>(Q_clamp(r * 255.0f, 0.0f, 255.0f));
    s_currentVertex.color[1] = static_cast<uint8_t>(Q_clamp(g * 255.0f, 0.0f, 255.0f));
    s_currentVertex.color[2] = static_cast<uint8_t>(Q_clamp(b * 255.0f, 0.0f, 255.0f));
    s_currentVertex.color[3] = static_cast<uint8_t>(Q_clamp(a * 255.0f, 0.0f, 255.0f));
}

void immediateTexCoord2f(float s, float t)
{
    GL3_ASSERT(s_active);

    s_currentVertex.texCoord = { s, t };
}

void immediateVertex3f(float x, float y, float z)
{
    GL3_ASSERT(s_active);

    s_currentVertex.position = { x, y, z };
    s_stageVertices.push_back(s_currentVertex);
}

static void EmitTRIANGLES(size_t vertexStart, size_t vertexEnd)
{
    for (size_t i = vertexStart; i < vertexEnd; i += 3)
    {
        uint16_t indices[] = {
            static_cast<uint16_t>(i + 0),
            static_cast<uint16_t>(i + 1),
            static_cast<uint16_t>(i + 2)
        };

        s_stageIndices.insert(s_stageIndices.end(), std::begin(indices), std::end(indices));
    }
}

static void EmitQUADS(size_t vertexStart, size_t vertexEnd)
{
    for (size_t i = vertexStart; i < vertexEnd; i += 4)
    {
        uint16_t indices[] = {
            static_cast<uint16_t>(i + 0),
            static_cast<uint16_t>(i + 1),
            static_cast<uint16_t>(i + 2),
            static_cast<uint16_t>(i + 0),
            static_cast<uint16_t>(i + 2),
            static_cast<uint16_t>(i + 3)
        };

        s_stageIndices.insert(s_stageIndices.end(), std::begin(indices), std::end(indices));
    }
}

static void EmitQUAD_STRIP(size_t vertexStart, size_t vertexEnd)
{
    for (size_t i = vertexStart + 2; i < vertexEnd; i += 2)
    {
        uint16_t indices[] = {
            static_cast<uint16_t>(i - 2),
            static_cast<uint16_t>(i - 1),
            static_cast<uint16_t>(i + 1),
            static_cast<uint16_t>(i - 2),
            static_cast<uint16_t>(i + 1),
            static_cast<uint16_t>(i + 0)
        };

        s_stageIndices.insert(s_stageIndices.end(), std::begin(indices), std::end(indices));
    }
}

void immediateEnd()
{
    GL3_ASSERT(s_active);

    GL3_ASSERT(s_stageVertices.size() >= s_primitiveStartVertex);

    size_t vertexStart = s_primitiveStartVertex;
    size_t vertexEnd = s_stageVertices.size();

    // FIXME: incomplete primitives are not handled
    switch (s_currentMode)
    {
    case GL_TRIANGLES:
        EmitTRIANGLES(vertexStart, vertexEnd);
        break;

    case GL_QUADS:
        EmitQUADS(vertexStart, vertexEnd);
        break;

    case GL_QUAD_STRIP:
        EmitQUAD_STRIP(vertexStart, vertexEnd);
        break;

    default:
        // don't sweep these under the rug
        platformError("Unsupported primitive type 0x%04x", s_currentMode);
        break;
    }
}

}
