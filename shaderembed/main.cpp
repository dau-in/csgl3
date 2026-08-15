// shader combiner: reads all shader sources, expands their includes and writes
// them as c arrays to stdout (which gets redirected to shader_sources.inl by cmake)
#include <cstring>
#include "ubo_flatten.h"
#include <iomanip>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_INCLUDE_IMPLEMENTATION
#define STB_INCLUDE_LINE_GLSL
#include "stb_include.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

static std::string file_dir(const char *path)
{
    const char *start1 = strrchr(path, '/');
    const char *start2 = strrchr(path, '\\');

    const char *start;
    if (start1 && start2)
    {
        start = (start1 > start2) ? start1 : start2;
    }
    else
    {
        start = start1 ? start1 : (start2 ? start2 : path);
    }

    return { path, start };
}

static std::string file_name(const char *path)
{
    const char *start1 = strrchr(path, '/');
    const char *start2 = strrchr(path, '\\');

    const char *start;
    if (start1 && start2)
    {
        start = (start1 > start2) ? start1 + 1 : start2 + 1;
    }
    else
    {
        start = start1 ? start1 + 1 : (start2 ? start2 + 1 : path);
    }

    return start;
}

static std::string pretty_name(const char *path)
{
    std::string result = file_name(path);

    auto pos = result.rfind('.');
    if (pos != std::string::npos)
    {
        result[pos] = '_';
    }

    return result;
}

static std::string pretty_name_120(const char *path)
{
    std::string result = file_name(path);

    auto pos = result.rfind('.');
    if (pos == std::string::npos)
    {
        return result + "_120";
    }

    return result.substr(0, pos) + "_120_" + result.substr(pos + 1);
}

static void write_byte_array(std::ostream &out, const std::string &symbol, const std::string &source)
{
    out << "static const unsigned char s_" << symbol << "[] =\n{\n";

    size_t length = source.size();
    for (size_t j = 0; j < length; j++)
    {
        if (j && (j < length - 1) && (j % 16) == 0)
        {
            out << "\n";
        }

        out << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (source[j] & 0xff) << ",";
    }

    out << std::dec << std::nouppercase << std::setfill(' ');

    out << "\n};\n";
}

struct shader_data
{
    std::string file_name; // lightmapped.frag
    std::string symbol; // lightmapped_frag
    std::string symbol120; // lightmapped_120_frag
    std::string source; // glsl 1.40 source
    std::string source120; // glsl 1.20 source
    int uboCount;
};

static bool ends_with(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

int main(int argc, char **argv)
{
    std::vector<shader_data> shaders;

    for (int i = 1; i < argc; i++)
    {
        char *path = argv[i];

        char error[256];
        char *data = stb_include_file(path, nullptr, const_cast<char *>(file_dir(path).c_str()), error);
        if (!data)
        {
            std::cerr << error;
            return 1;
        }

        shader_data shader;
        shader.file_name = file_name(path);
        shader.symbol = pretty_name(path);
        shader.symbol120 = pretty_name_120(path);
        shader.source = data;

        free(data);

        if (!process_reflection(path, shader.symbol, shader.source, shader.source120, shader.uboCount))
        {
            return 1;
        }

        shader.source120 = lower_to_120(ends_with(shader.file_name, ".vert"), shader.source120);

        shaders.push_back(std::move(shader));
    }

    std::cout << "// automatically generated\n";

    write_reflection(std::cout);

    std::cout << "\n";
    for (const shader_data &shader : shaders)
    {
        write_byte_array(std::cout, shader.symbol, shader.source);
        write_byte_array(std::cout, shader.symbol120, shader.source120);
    }

    std::cout << "\nstatic const ShaderData s_shaderData[] =\n{\n";
    for (const shader_data &shader : shaders)
    {
        std::cout << "{\"" << shader.file_name << "\",s_" << shader.symbol << ",sizeof(s_" << shader.symbol << ")},\n";
    }
    std::cout << "};\n";

    std::cout << "\nstatic const ShaderData120 s_shaderData_120[] =\n{\n";
    for (const shader_data &shader : shaders)
    {
        std::cout << "{\"" << shader.file_name << "\",s_" << shader.symbol120 << ",sizeof(s_" << shader.symbol120 << "),";
        if (shader.uboCount)
        {
            std::cout << "s_shaderUbos_" << shader.symbol << "," << shader.uboCount;
        }
        else
        {
            std::cout << "nullptr,0";
        }
        std::cout << "},\n";
    }
    std::cout << "};\n";

    return 0;
}
