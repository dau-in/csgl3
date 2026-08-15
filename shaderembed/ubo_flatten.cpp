#include "ubo_flatten.h"
#include <map>
#include <vector>

struct type_props
{
    const char *glsl_name;
    const char *enum_name;
    int align;
    int size;
};

struct ubo_member
{
    std::string type;
    std::string name;
    bool is_array;
    int count;
    int offset;
};

struct ubo_layout
{
    std::string block_name;
    int size;
    std::vector<ubo_member> members;
};

// same name can have different layouts, e.g. brush and studio ModelConstants
struct block_group
{
    std::string name;
    std::vector<ubo_layout> variants;
};

struct shader_blocks
{
    std::string pretty;
    std::vector<std::pair<int, int>> refs; // group index, variant index
};

struct parsed_block
{
    size_t span_begin; // -->"layout"
    size_t span_end; // "};"<--
    ubo_layout layout;
};

struct token
{
    std::string text;
    size_t pos;
};

// FIXME: tied to shader.h UboType, and overall this sucks
static const type_props s_type_props[] = {
    { "float", "UboType::Float", 4, 4 },
    { "int", "UboType::Int", 4, 4 },
    { "bool", "UboType::Bool", 4, 4 },
    { "vec2", "UboType::Vec2", 8, 8 },
    { "vec3", "UboType::Vec3", 16, 12 },
    { "vec4", "UboType::Vec4", 16, 16 },
    { "mat4", "UboType::Mat4", 16, 64 },
    { "mat3x4", "UboType::Mat3x4", 16, 48 },
};

static std::vector<block_group> s_block_groups;
static std::vector<shader_blocks> s_shader_blocks;

static const type_props *find_type(const std::string &name)
{
    for (const type_props &props : s_type_props)
    {
        if (name == props.glsl_name)
        {
            return &props;
        }
    }

    return nullptr;
}

static bool members_equal(const ubo_layout &a, const ubo_layout &b)
{
    if (a.members.size() != b.members.size())
    {
        return false;
    }

    for (size_t i = 0; i < a.members.size(); i++)
    {
        const ubo_member &m1 = a.members[i];
        const ubo_member &m2 = b.members[i];
        if (m1.type != m2.type || m1.name != m2.name || m1.is_array != m2.is_array || m1.count != m2.count)
        {
            return false;
        }
    }

    return true;
}

// replaces comments with spaces so that byte offsets stay valid
static std::string strip_comments(const std::string &text)
{
    std::string result = text;

    size_t i = 0;
    while (i + 1 < result.size())
    {
        if (result[i] == '/' && result[i + 1] == '/')
        {
            while (i < result.size() && result[i] != '\n')
            {
                result[i++] = ' ';
            }
        }
        else if (result[i] == '/' && result[i + 1] == '*')
        {
            result[i] = ' ';
            result[i + 1] = ' ';
            i += 2;
            while (i + 1 < result.size() && !(result[i] == '*' && result[i + 1] == '/'))
            {
                if (result[i] != '\n')
                {
                    result[i] = ' ';
                }
                i++;
            }
            if (i + 1 < result.size())
            {
                result[i] = ' ';
                result[i + 1] = ' ';
                i += 2;
            }
        }
        else
        {
            i++;
        }
    }

    return result;
}

// collect all defines with single token values so we can resolve array sizes
static void collect_defines(const std::string &text, std::map<std::string, std::string> &defines)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
        {
            eol = text.size();
        }

        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;

        size_t i = line.find_first_not_of(" \t\r");
        if (i == std::string::npos || line[i] != '#')
        {
            continue;
        }

        i = line.find_first_not_of(" \t\r", i + 1);
        if (i == std::string::npos || line.compare(i, 6, "define") != 0)
        {
            continue;
        }

        i = line.find_first_not_of(" \t\r", i + 6);
        if (i == std::string::npos)
        {
            continue;
        }

        size_t name_start = i;
        while (i < line.size() && (isalnum(line[i]) || line[i] == '_'))
        {
            i++;
        }

        std::string name = line.substr(name_start, i - name_start);
        if (name.empty() || (i < line.size() && line[i] == '('))
        {
            // function-like
            continue;
        }

        size_t value_start = line.find_first_not_of(" \t\r", i);
        if (value_start == std::string::npos)
        {
            continue;
        }

        size_t value_end = line.find_last_not_of(" \t\r") + 1;
        std::string value = line.substr(value_start, value_end - value_start);
        if (value.find_first_of(" \t\r") != std::string::npos)
        {
            // multi token value
            continue;
        }

        defines[name] = value;
    }
}

static bool resolve_int(const std::map<std::string, std::string> &defines, const std::string &token, int &result, int depth = 0)
{
    if (depth > 8)
    {
        return false;
    }

    bool numeric = !token.empty();
    for (char c : token)
    {
        if (!isdigit(c))
        {
            numeric = false;
            break;
        }
    }

    if (numeric)
    {
        result = atoi(token.c_str());
        return true;
    }

    auto it = defines.find(token);
    if (it == defines.end())
    {
        return false;
    }

    return resolve_int(defines, it->second, result, depth + 1);
}

static std::vector<token> tokenize(const std::string &text)
{
    std::vector<token> tokens;

    size_t i = 0;
    while (i < text.size())
    {
        char c = text[i];
        if (isspace(static_cast<unsigned char>(c)))
        {
            i++;
            continue;
        }

        size_t start = i;
        if (isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            while (i < text.size() && (isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_'))
            {
                i++;
            }
        }
        else if (isdigit(static_cast<unsigned char>(c)))
        {
            while (i < text.size() && (isalnum(static_cast<unsigned char>(text[i])) || text[i] == '.'))
            {
                i++;
            }
        }
        else
        {
            i++;
        }

        tokens.push_back({ text.substr(start, i - start), start });
    }

    return tokens;
}

static bool compute_std140(const char *path, ubo_layout &layout)
{
    int offset = 0;

    for (ubo_member &member : layout.members)
    {
        const type_props *props = find_type(member.type);

        int align = props->align;
        int size = props->size;

        if (member.is_array)
        {
            if ((size % 16) != 0)
            {
                // FIXME: unfuck this
                std::cerr << path << ": " << layout.block_name << "." << member.name << ": array of " << member.type << " has bad stride\n";
                return false;
            }

            align = 16;
            size *= member.count;
        }

        offset = (offset + align - 1) & ~(align - 1);
        member.offset = offset;
        offset += size;
    }

    layout.size = (offset + 15) & ~15;
    return true;
}

static void register_layout(const ubo_layout &layout, int &group_index, int &variant_index)
{
    for (size_t i = 0; i < s_block_groups.size(); i++)
    {
        if (s_block_groups[i].name != layout.block_name)
        {
            continue;
        }

        for (size_t j = 0; j < s_block_groups[i].variants.size(); j++)
        {
            if (members_equal(s_block_groups[i].variants[j], layout))
            {
                group_index = static_cast<int>(i);
                variant_index = static_cast<int>(j);
                return;
            }
        }

        s_block_groups[i].variants.push_back(layout);
        group_index = static_cast<int>(i);
        variant_index = static_cast<int>(s_block_groups[i].variants.size() - 1);
        return;
    }

    s_block_groups.push_back({ layout.block_name, { layout } });
    group_index = static_cast<int>(s_block_groups.size() - 1);
    variant_index = 0;
}

static bool parse_blocks(const char *path, const std::vector<token> &tokens, const std::map<std::string, std::string> &defines, std::vector<parsed_block> &blocks)
{
    for (size_t i = 0; i + 6 < tokens.size(); i++)
    {
        if (tokens[i].text != "layout"
            || tokens[i + 1].text != "("
            || tokens[i + 2].text != "std140"
            || tokens[i + 3].text != ")"
            || tokens[i + 4].text != "uniform"
            || tokens[i + 6].text != "{")
        {
            continue;
        }

        parsed_block block;
        block.span_begin = tokens[i].pos;
        block.layout.block_name = tokens[i + 5].text;

        size_t j = i + 7;
        while (j < tokens.size() && tokens[j].text != "}")
        {
            ubo_member member;
            member.type = tokens[j].text;
            member.is_array = false;
            member.count = 1;
            member.offset = 0;

            if (!find_type(member.type))
            {
                std::cerr << path << ": " << block.layout.block_name << ": unsupported member type " << member.type << "\n";
                return false;
            }

            if (++j >= tokens.size())
            {
                break;
            }

            member.name = tokens[j].text;

            if (++j >= tokens.size())
            {
                break;
            }

            if (tokens[j].text == "[")
            {
                if (j + 2 >= tokens.size() || tokens[j + 2].text != "]")
                {
                    std::cerr << path << ": " << block.layout.block_name << "." << member.name << ": malformed array\n";
                    return false;
                }

                if (!resolve_int(defines, tokens[j + 1].text, member.count))
                {
                    std::cerr << path << ": " << block.layout.block_name << "." << member.name
                              << ": cannot resolve array size " << tokens[j + 1].text << "\n";
                    return false;
                }

                member.is_array = true;
                j += 3;
            }

            if (j >= tokens.size() || tokens[j].text != ";")
            {
                std::cerr << path << ": " << block.layout.block_name << "." << member.name << ": expected ;\n";
                return false;
            }

            j++;
            block.layout.members.push_back(member);
        }

        if (j + 1 >= tokens.size() || tokens[j + 1].text != ";")
        {
            std::cerr << path << ": " << block.layout.block_name << ": expected }; at end of block\n";
            return false;
        }

        block.span_end = tokens[j + 1].pos + 1;

        if (!compute_std140(path, block.layout))
        {
            return false;
        }

        blocks.push_back(block);
        i = j + 1;
    }

    return true;
}

// returns the expanded source with uniform blocks rewritten as plain uniforms
static std::string flatten_source(const std::string &raw, const std::vector<parsed_block> &blocks)
{
    std::string result;
    result.reserve(raw.size());

    size_t last = 0;
    for (const parsed_block &block : blocks)
    {
        result.append(raw, last, block.span_begin - last);

        for (const ubo_member &member : block.layout.members)
        {
            result.append("uniform ");
            result.append(member.type);
            result.append(" ");
            result.append(member.name);
            if (member.is_array)
            {
                result.append("[");
                result.append(std::to_string(member.count));
                result.append("]");
            }
            result.append(";\n");
        }

        // consume the newline that followed "};" so the output doesn't gain blank lines
        last = block.span_end;
        if (last < raw.size() && raw[last] == '\r')
        {
            last++;
        }
        if (last < raw.size() && raw[last] == '\n')
        {
            last++;
        }
    }

    result.append(raw, last, std::string::npos);

    return result;
}

bool process_reflection(const char *path, const std::string &pretty, const std::string &raw, std::string &flattened, int &uboCount)
{
    std::string stripped = strip_comments(raw);

    std::map<std::string, std::string> defines;
    collect_defines(stripped, defines);

    std::vector<token> tokens = tokenize(stripped);

    std::vector<parsed_block> blocks;
    if (!parse_blocks(path, tokens, defines, blocks))
    {
        return false;
    }

    s_shader_blocks.push_back({});
    shader_blocks &shader = s_shader_blocks.back();
    shader.pretty = pretty;

    for (const parsed_block &block : blocks)
    {
        int group_index, variant_index;
        register_layout(block.layout, group_index, variant_index);
        shader.refs.push_back({ group_index, variant_index });
    }

    flattened = flatten_source(raw, blocks);
    uboCount = static_cast<int>(s_shader_blocks.back().refs.size());
    return true;
}

// ModelConstants has two different layouts (brush and studio), disambiguate
// the generated symbols with the first member name (FIXME: flimsy!!! some kind of hash instead?)
static std::string layout_symbol(const block_group &group, int variant_index)
{
    if (group.variants.size() == 1)
    {
        return group.name;
    }

    return group.name + "_" + group.variants[variant_index].members[0].name;
}

void write_reflection(std::ostream &out)
{
    for (const block_group &group : s_block_groups)
    {
        for (size_t i = 0; i < group.variants.size(); i++)
        {
            const ubo_layout &layout = group.variants[i];
            std::string symbol = layout_symbol(group, static_cast<int>(i));

            out << "\nstatic const UboMember s_ubo_" << symbol << "[] = {\n";
            for (const ubo_member &member : layout.members)
            {
                out << "    { \"" << member.name << "\", " << find_type(member.type)->enum_name
                    << ", " << member.count << ", " << member.offset << " },\n";
            }
            out << "};\n\n";

            out << "static const UboLayout s_layout_" << symbol << " = { \"" << group.name << "\", "
                << layout.size << ", s_ubo_" << symbol << ", " << layout.members.size() << " };\n";
        }
    }

    for (const shader_blocks &shader : s_shader_blocks)
    {
        if (shader.refs.empty())
        {
            continue;
        }

        out << "static const UboLayout *const s_shaderUbos_" << shader.pretty << "[] = { ";
        for (size_t i = 0; i < shader.refs.size(); i++)
        {
            const block_group &group = s_block_groups[shader.refs[i].first];
            out << (i ? ", " : "") << "&s_layout_" << layout_symbol(group, shader.refs[i].second);
        }
        out << " };\n";
    }
}

// maps every sampler to the gl 1.20 texture function (e,g, sampler2D -> texture2D)
static void build_sampler_map(const std::string &src, std::map<std::string, std::string> &result)
{
    size_t pos = 0;
    while (pos < src.size())
    {
        size_t eol = src.find('\n', pos);
        if (eol == std::string::npos)
        {
            eol = src.size();
        }

        std::vector<token> tokens = tokenize(src.substr(pos, eol - pos));
        pos = eol + 1;

        // uniform <samplerType> <name>
        if (tokens.size() >= 3 && tokens[0].text == "uniform")
        {
            const std::string &type = tokens[1].text;
            const char *func = nullptr;

            // FIXME: handle all of them??? 1d, 2d, 3d, cube, any others in 1.20???
            if (type == "sampler2D")
            {
                func = "texture2D";
            }
            else if (type == "samplerCube")
            {
                func = "textureCube";
            }

            if (func)
            {
                result[tokens[2].text] = func;
            }
        }
    }
}

static bool is_ident_char(char c)
{
    return isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string lower_to_120(bool is_vertex, const std::string &src)
{
    // replace in/out with glsl 1.20 equivalents
    std::string pass1;
    pass1.reserve(src.size()); // not enough.. too bad!!!

    size_t pos = 0;
    while (pos < src.size())
    {
        size_t eol = src.find('\n', pos);
        bool has_nl = (eol != std::string::npos);
        if (!has_nl)
        {
            eol = src.size();
        }

        std::string line = src.substr(pos, eol - pos);
        pos = has_nl ? eol + 1 : eol;

        size_t p = line.find_first_not_of(" \t\r");
        if (p != std::string::npos)
        {
            size_t e = p;
            while (e < line.size() && is_ident_char(line[e]))
            {
                e++;
            }

            std::string tok = line.substr(p, e - p);

            if (tok == "in")
            {
                line = line.substr(0, p) + (is_vertex ? "attribute" : "varying") + line.substr(e);
            }
            else if (tok == "out")
            {
                if (is_vertex)
                {
                    line = line.substr(0, p) + "varying" + line.substr(e);
                }
                else
                {
                    // #define the fragment shader output to gl_FragColor
                    // so we don't need to do any additional replacements
                    std::vector<token> tokens = tokenize(line);
                    std::string name = (tokens.size() >= 3) ? tokens[2].text : "fragColor";
                    line = line.substr(0, p) + "#define " + name + " gl_FragColor";
                }
            }
        }

        pass1 += line;

        if (has_nl)
        {
            pass1 += '\n';
        }
    }

    // replace texture function names depending on the sampler
    std::map<std::string, std::string> sampler_map;
    build_sampler_map(src, sampler_map);

    std::string pass2;
    pass2.reserve(pass1.size()); // not enough.. too bad!!!

    for (size_t i = 0; i < pass1.size();)
    {
        if (pass1.compare(i, 7, "texture") == 0 && (i == 0 || !is_ident_char(pass1[i - 1])))
        {
            size_t k = i + 7;
            while (k < pass1.size() && isspace(static_cast<unsigned char>(pass1[k])))
            {
                k++;
            }

            if (k < pass1.size() && pass1[k] == '(')
            {
                size_t a = k + 1;
                while (a < pass1.size() && isspace(static_cast<unsigned char>(pass1[a])))
                {
                    a++;
                }

                size_t b = a;
                while (b < pass1.size() && is_ident_char(pass1[b]))
                {
                    b++;
                }

                // FIXME: guaranteed compile error bruhg
                auto it = sampler_map.find(pass1.substr(a, b - a));
                pass2 += (it != sampler_map.end()) ? it->second : "texture2D";

                i += 7;
                continue;
            }
        }

        pass2 += pass1[i];
        i++;
    }

    return pass2;
}
