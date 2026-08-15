#ifndef SHADER_H
#define SHADER_H

namespace Render
{

struct VertexAttrib;

// FIXME: determine from s_blockNames???
constexpr int MaxUniformBlocks = 4;

// FIXME: unfuck
// NOTE: used by shaderembed
enum class UboType
{
    Float,
    Int,
    Bool,
    Vec2,
    Vec3,
    Vec4,
    Mat4,
    Mat3x4,
};

// NOTE: used by shaderembed
struct UboMember
{
    const char *name;
    UboType type;
    unsigned short count;
    unsigned short offset;
};

// NOTE: used by shaderembed
struct UboLayout
{
    const char *blockName;
    unsigned int size;
    const UboMember *members;
    int memberCount;
};

// for mapping UBO locations to plain uniforms, only
// used when ARB_uniform_buffer_object is not available
struct FlatBlock
{
    const UboLayout *layout; // null if not present
    const GLint *locations; // -1 if not present
    unsigned sequence; // draw time dirtiness checks
};

struct ShaderUniform
{
    // mutable value: location will be stored at "field"
    template<typename Field, typename Struct>
    ShaderUniform(const char *_name, const Field Struct::*ptr)
    {
        const Struct *object = nullptr;
        const Field *field = &(object->*ptr);
        offset = static_cast<int>(reinterpret_cast<intptr_t>(field));
        name = _name;
    }

    // constant value: set after linking, not to be changed after
    ShaderUniform(const char *_name, int constantValue)
    {
        offset = -1 - constantValue;
        name = _name;
    }

    int offset;
    const char *name;
};

struct ShaderOption
{
    const char *name;
    int maxValue;
};

// for consistency i gues...
#define SHADER_OPTION(name, maxValue) { #name, maxValue }
#define SHADER_OPTION_TERM() { nullptr, 0 }

union UniformValue
{
    int int_[2]{};
    float float_[2];
};

// open addressed on the uniform location, the key is location + 1 so a zeroed table is empty
constexpr int MaxShaderUniforms = 8;

// we're shadowing the default block to greatly reduce the size of command buffers
struct UniformShadow
{
    GLuint keys[MaxShaderUniforms];
    UniformValue values[MaxShaderUniforms];

    UniformValue &operator[](GLint location)
    {
        GLuint key = location + 1;

        unsigned slot = key & (MaxShaderUniforms - 1);

        for (int i = 0; i < MaxShaderUniforms; i++)
        {
            if (keys[slot] == key)
            {
                return values[slot];
            }

            if (!keys[slot])
            {
                keys[slot] = key;
                return values[slot];
            }

            slot = (slot + 1) & (MaxShaderUniforms - 1);
        }

        // never gets here
        platformError("Uniform shadow overflow");
    }

    void clear()
    {
        memset(this, 0, sizeof(*this));
    }
};

struct BaseShader
{
    GLuint program;

    // only used when ARB_uniform_buffer_object is not available
    FlatBlock *flatBlocks;

    UniformShadow uniformState;
};

void shaderInit();
void shaderUpdate(bool forceRecompile = false);
void shaderUpdateGamma(float brightness, float gamma, float lightgamma, bool overbright);

void shaderRegister(
    byte *shaderStructs,
    int shaderStructSize,
    int shaderCount,
    const char *name,
    Span<const VertexAttrib> attributes,
    Span<const ShaderUniform> uniforms,
    Span<const ShaderOption> options);

template<typename T, int ShaderCount>
void shaderRegister(
    T (&shaderStructs)[ShaderCount],
    const char *name,
    Span<const VertexAttrib> attributes,
    Span<const ShaderUniform> uniforms,
    Span<const ShaderOption> options)
{
    static_assert(std::is_base_of<BaseShader, T>::value, "bruh");
    shaderRegister(reinterpret_cast<byte *>(shaderStructs), sizeof(T), ShaderCount, name, attributes, uniforms, options);
}

template<typename T>
void shaderRegister(
    T &shaderStruct,
    const char *name,
    Span<const VertexAttrib> attributes,
    Span<const ShaderUniform> uniforms)
{
    static_assert(std::is_base_of<BaseShader, T>::value, "bruh");
    shaderRegister(reinterpret_cast<byte *>(&shaderStruct), sizeof(T), 1, name, attributes, uniforms, {});
}

template<size_t N>
constexpr int shaderVariantCount(const ShaderOption (&options)[N])
{
    int count = 1;

    for (const ShaderOption &option : options)
    {
        count *= (option.maxValue + 1);
    }

    if (count > 99)
    {
        // wtf
        return -1;
    }

    return count;
}

template<typename S, int ShaderCount, typename T, int OptionCount>
S &shaderSelect(S (&shaders)[ShaderCount], const ShaderOption (&optionInfo)[OptionCount], const T &options)
{
    static_assert((sizeof(options) / sizeof(int)) == OptionCount, "Option structure size mismatch");
    const int *values = reinterpret_cast<const int *>(&options);

    int index = 0;
    int accum = 1;

    for (int i = 0; i < OptionCount; i++)
    {
        GL3_ASSERT(values[i] <= optionInfo[i].maxValue);
        index += (accum * values[i]);
        accum *= (optionInfo[i].maxValue + 1);
    }

    GL3_ASSERT(index < ShaderCount);
    return shaders[index];
}

}

#endif
