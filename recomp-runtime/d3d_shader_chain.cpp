// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_shader_chain.h"

#include "d3d_shader_compiler.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>

namespace {

constexpr unsigned kMaxPasses = 64u, kMaxSize = 8192u, kMaxSamplers = 16u;
/* The push constant block is given this binding in the GLSL, so the SPIR-V tells it from the uniform block. */
constexpr uint32_t kPushBinding = 31u;
constexpr SDL_GPUTextureUsageFlags kTargetUsage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

std::string trim(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1u);
}

std::string folderOf(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1u);
}

std::string joined(const std::string &folder, const std::string &name)
{
    if (!name.empty() && (name[0] == '/' || name[0] == '\\' || (name.size() > 1u && name[1] == ':'))) return name;
    return folder + name;
}

bool readText(const std::string &path, std::string &text)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream all;
    all << file.rdbuf();
    text = all.str();
    return true;
}

bool startsWith(const std::string &text, const char *prefix)
{
    return text.rfind(prefix, 0) == 0;
}

/* ---- The preset (.slangp): its values, each with the folder of the file it came from (its paths are relative
   to that), a #reference's first and the file's own over them ---- */

struct Value {
    std::string text, folder;
};
using Values = std::map<std::string, Value>;

bool readPreset(const std::string &path, Values &values, std::string &error, int depth = 0)
{
    if (depth > 16) {
        error = "#reference nests too deeply at " + path;
        return false;
    }
    std::string text;
    if (!readText(path, text)) {
        error = "cannot read " + path;
        return false;
    }
    const std::string folder = folderOf(path);
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        line = trim(line);
        if (startsWith(line, "#reference")) {
            const size_t open = line.find('"'), close = line.rfind('"');
            if (open == std::string::npos || close <= open) {
                error = "a #reference without its file in " + path;
                return false;
            }
            if (!readPreset(joined(folder, line.substr(open + 1u, close - open - 1u)), values, error, depth + 1))
                return false;
            continue;
        }
        if (line.empty() || line[0] == '#' || startsWith(line, "//")) continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(line.substr(0, equals));
        std::string value = trim(line.substr(equals + 1u));
        if (!value.empty() && value[0] == '"') {
            const size_t end = value.find('"', 1u);
            value = value.substr(1u, end == std::string::npos ? std::string::npos : end - 1u);
        } else if (const size_t hash = value.find('#'); hash != std::string::npos) {
            value = trim(value.substr(0, hash));
        }
        values[key] = {value, folder};
    }
    return true;
}

const Value *valueOf(const Values &values, const std::string &key)
{
    const auto found = values.find(key);
    return found == values.end() ? nullptr : &found->second;
}

std::string textOf(const Values &values, const std::string &key, const std::string &otherwise = {})
{
    const Value *value = valueOf(values, key);
    return value != nullptr ? value->text : otherwise;
}

bool flagOf(const Values &values, const std::string &key, bool otherwise)
{
    const Value *value = valueOf(values, key);
    if (value == nullptr) return otherwise;
    return value->text == "true" || value->text == "1";
}

/* ---- A pass's source (.slang): #includes inlined, split into its stages, its pragmas taken out ---- */

bool readShader(const std::string &path, std::string &out, std::string &error, int depth = 0)
{
    if (depth > 32) {
        error = "#include nests too deeply at " + path;
        return false;
    }
    std::string text;
    if (!readText(path, text)) {
        error = "cannot read " + path;
        return false;
    }
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const std::string trimmed = trim(line);
        if (startsWith(trimmed, "#include")) {
            const size_t open = trimmed.find('"'), close = trimmed.rfind('"');
            if (open == std::string::npos || close <= open) {
                error = "an #include without its file in " + path;
                return false;
            }
            if (!readShader(joined(folderOf(path), trimmed.substr(open + 1u, close - open - 1u)), out, error,
                    depth + 1))
                return false;
            continue;
        }
        out += line;
        out += '\n';
    }
    return true;
}

struct Parameter {
    std::string name;
    float value;
};

struct Source {
    std::string vertex, fragment, name, format;
    std::vector<Parameter> parameters;
};

void splitStages(const std::string &text, Source &source)
{
    std::istringstream lines(text);
    enum { Both, Vertex, Fragment } stage = Both;
    for (std::string line; std::getline(lines, line);) {
        const std::string trimmed = trim(line);
        if (startsWith(trimmed, "#pragma")) {
            std::istringstream words(trimmed.substr(7u));
            std::string what, word;
            words >> what;
            if (what == "stage") {
                words >> word;
                stage = word == "vertex" ? Vertex : word == "fragment" ? Fragment : stage;
                line.clear();
            } else if (what == "name") {
                words >> source.name;
                line.clear();
            } else if (what == "format") {
                words >> source.format;
                line.clear();
            } else if (what == "parameter") {
                // #pragma parameter NAME "Description" default minimum maximum step
                Parameter parameter{};
                words >> parameter.name;
                const size_t open = trimmed.find('"'), close = open == std::string::npos
                    ? std::string::npos : trimmed.find('"', open + 1u);
                if (close != std::string::npos)
                    parameter.value = std::strtof(trimmed.c_str() + close + 1u, nullptr);
                if (!parameter.name.empty()) source.parameters.push_back(parameter);
                line.clear();
            }
        }
        // (a blank line for each taken out, so shaderc's line numbers are the file's)
        if (stage != Fragment) source.vertex += line + '\n';
        if (stage != Vertex) source.fragment += line + '\n';
    }
}

/* The push constant block as a uniform block at kPushBinding of set 0 (SDL_GPU has no push constants; a uniform
   block gets std140, which for the scalars and vectors push blocks hold is their layout). */
std::string pushAsUniform(std::string glsl)
{
    static const std::string word = "push_constant";
    for (size_t at = 0; (at = glsl.find(word, at)) != std::string::npos;) {
        const bool alone = (at == 0 || !(std::isalnum(static_cast<unsigned char>(glsl[at - 1u])) || glsl[at - 1u] == '_'))
            && (at + word.size() >= glsl.size() ||
                !(std::isalnum(static_cast<unsigned char>(glsl[at + word.size()])) || glsl[at + word.size()] == '_'));
        if (!alone) {
            at += word.size();
            continue;
        }
        const std::string to = "set = 0, binding = " + std::to_string(kPushBinding);
        glsl.replace(at, word.size(), to);
        at += to.size();
    }
    return glsl;
}

/* ---- The SPIR-V: what its uniform blocks hold and which textures it samples, and its bindings moved to
   SDL_GPU's: a vertex shader's uniform buffers in set 1, a fragment shader's in set 3 (the uniform block in slot
   0, the push block in 1), textures in set 0 (vertex) or 2 (fragment) in the order of their bindings ---- */

enum class Kind { Float, Int, Uint };

struct Member {
    std::string name;
    uint32_t offset;
    Kind kind;
    uint32_t count;   // floats or integers: 1, 2, 3, 4, or 16 for a mat4
};

struct Block {
    bool present = false;
    uint32_t size = 0u;
    std::vector<Member> members;
};

struct Reflected {
    Block blocks[2];                    // the uniform block, the push block
    std::vector<std::string> textures;  // by slot
};

bool reflect(std::vector<uint8_t> &bytes, bool vertex, Reflected &out, std::string &error)
{
    if (bytes.size() < 20u || bytes.size() % 4u != 0u) {
        error = "shaderc gave no SPIR-V";
        return false;
    }
    std::vector<uint32_t> words(bytes.size() / 4u);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    struct Type {
        uint32_t op = 0u, a = 0u, b = 0u, c = 0u;
        std::vector<uint32_t> members;
    };
    std::map<uint32_t, Type> types;
    std::map<uint32_t, std::string> names;
    std::map<std::pair<uint32_t, uint32_t>, std::string> member_names;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> offsets;
    std::map<uint32_t, size_t> binding_at, set_at;
    struct Variable {
        uint32_t id, type, storage;
    };
    std::vector<Variable> variables;
    const auto text = [&](size_t from, size_t end) {
        std::string s;
        for (size_t i = from; i < end; ++i)
            for (unsigned b = 0u; b < 4u; ++b) {
                const char c = static_cast<char>((words[i] >> (8u * b)) & 0xffu);
                if (c == '\0') return s;
                s += c;
            }
        return s;
    };
    for (size_t at = 5u; at < words.size();) {
        const uint32_t count = words[at] >> 16u, op = words[at] & 0xffffu;
        if (count == 0u || at + count > words.size()) {
            error = "SPIR-V that does not parse";
            return false;
        }
        const uint32_t *w = &words[at];
        switch (op) {
        case 5u: names[w[1]] = text(at + 2u, at + count); break;                                  // OpName
        case 6u: member_names[{w[1], w[2]}] = text(at + 3u, at + count); break;                   // OpMemberName
        case 71u:                                                                                  // OpDecorate
            if (count >= 4u && w[2] == 33u) binding_at[w[1]] = at + 3u;
            if (count >= 4u && w[2] == 34u) set_at[w[1]] = at + 3u;
            break;
        case 72u:                                                                                  // OpMemberDecorate
            if (count >= 5u && w[3] == 35u) offsets[{w[1], w[2]}] = w[4];
            break;
        case 21u: types[w[1]] = {op, w[2], w[3], 0u, {}}; break;                                  // OpTypeInt
        case 22u: types[w[1]] = {op, w[2], 0u, 0u, {}}; break;                                    // OpTypeFloat
        case 23u: case 24u: types[w[1]] = {op, w[2], w[3], 0u, {}}; break;                        // vector, matrix
        case 27u: types[w[1]] = {op, w[2], 0u, 0u, {}}; break;                                    // sampled image
        case 30u: types[w[1]] = {op, 0u, 0u, 0u, std::vector<uint32_t>(w + 2, w + count)}; break;// OpTypeStruct
        case 32u: types[w[1]] = {op, w[2], w[3], 0u, {}}; break;                                  // OpTypePointer
        case 59u: variables.push_back({w[2], w[1], w[3]}); break;                                 // OpVariable
        default: break;
        }
        at += count;
    }
    // (a member's type: its kind and how many numbers)
    const auto describe = [&](uint32_t id, Kind &kind, uint32_t &numbers) {
        numbers = 0u;
        for (uint32_t scale = 1u; types.count(id);) {
            const Type &type = types[id];
            if (type.op == 22u) { kind = Kind::Float; numbers = scale; return; }
            if (type.op == 21u) { kind = type.b ? Kind::Int : Kind::Uint; numbers = scale; return; }
            if (type.op != 23u && type.op != 24u) return;
            scale *= type.b;
            id = type.a;
        }
    };
    struct Sampled {
        uint32_t binding, id;
    };
    std::vector<Sampled> sampled;
    for (const Variable &variable : variables) {
        if (!types.count(variable.type)) continue;
        const uint32_t pointee = types[variable.type].b;
        if (!types.count(pointee)) continue;
        const Type &type = types[pointee];
        if (!binding_at.count(variable.id) || !set_at.count(variable.id)) continue;
        const uint32_t binding = words[binding_at[variable.id]];
        if (variable.storage == 2u && type.op == 30u) {   // Uniform: a block
            const unsigned which = binding == kPushBinding ? 1u : 0u;
            Block &block = out.blocks[which];
            block.present = true;
            for (uint32_t i = 0; i < type.members.size(); ++i) {
                Member member{member_names[{pointee, i}], offsets[{pointee, i}], Kind::Float, 0u};
                describe(type.members[i], member.kind, member.count);
                block.size = std::max(block.size, member.offset + 4u * std::max(member.count, 1u));
                if (member.count != 0u) block.members.push_back(member);
            }
            words[set_at[variable.id]] = vertex ? 1u : 3u;
            words[binding_at[variable.id]] = which;
        } else if (variable.storage == 0u && type.op == 27u) {   // UniformConstant: a sampled texture
            sampled.push_back({binding, variable.id});
        }
    }
    std::sort(sampled.begin(), sampled.end(), [](const Sampled &a, const Sampled &b) { return a.binding < b.binding; });
    if (sampled.size() > kMaxSamplers) {
        error = "more than 16 textures in one stage";
        return false;
    }
    for (uint32_t slot = 0; slot < sampled.size(); ++slot) {
        words[set_at[sampled[slot].id]] = vertex ? 0u : 2u;
        words[binding_at[sampled[slot].id]] = slot;
        out.textures.push_back(names[sampled[slot].id]);
    }
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return true;
}

/* ---- The textures ---- */

struct Texture {
    SDL_GPUTexture *texture = nullptr;
    uint32_t width = 0u, height = 0u, levels = 1u;
};

uint32_t levelsFor(uint32_t width, uint32_t height)
{
    uint32_t levels = 1u;
    while ((std::max(width, height) >> levels) != 0u) ++levels;
    return levels;
}

SDL_GPUTextureFormat formatNamed(const std::string &name)
{
    static const std::pair<const char *, SDL_GPUTextureFormat> kFormats[] = {
        {"R8_UNORM", SDL_GPU_TEXTUREFORMAT_R8_UNORM},
        {"R8G8_UNORM", SDL_GPU_TEXTUREFORMAT_R8G8_UNORM},
        {"R8G8B8A8_UNORM", SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM},
        {"R8G8B8A8_SRGB", SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB},
        {"A2B10G10R10_UNORM_PACK32", SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM},
        {"R16_SFLOAT", SDL_GPU_TEXTUREFORMAT_R16_FLOAT},
        {"R16G16_SFLOAT", SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT},
        {"R16G16B16A16_SFLOAT", SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT},
        {"R32_SFLOAT", SDL_GPU_TEXTUREFORMAT_R32_FLOAT},
        {"R32G32_SFLOAT", SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT},
        {"R32G32B32A32_SFLOAT", SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT},
    };
    for (const auto &format : kFormats)
        if (name == format.first) return format.second;
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

struct SamplerKey {
    bool linear, mipmap;
    SDL_GPUSamplerAddressMode wrap;
    bool operator<(const SamplerKey &other) const
    {
        return std::tie(linear, mipmap, wrap) < std::tie(other.linear, other.mipmap, other.wrap);
    }
};

SDL_GPUSamplerAddressMode wrapNamed(const std::string &name)
{
    if (name == "repeat") return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    if (name == "mirrored_repeat") return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    // (clamp_to_border, the default, too: SDL_GPU has no border colour)
    return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
}

/* ---- The chain ---- */

enum class Scale { Source, Viewport, Absolute };

struct Pass {
    SDL_GPUGraphicsPipeline *pipeline = nullptr;
    Reflected stages[2];   // vertex, fragment
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    std::string alias;
    Scale scale_type[2] = {Scale::Source, Scale::Source};
    float scale[2] = {1.0f, 1.0f};
    SamplerKey sampler{false, false, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
    uint32_t frame_count_mod = 0u;
    bool mipmap_output = false;   // (the next pass samples it with mipmaps)
    bool feedback = false;        // (a pass samples last frame's output)
    Texture output, previous;
};

struct Lut {
    std::string name;
    Texture texture;
    SamplerKey sampler;
};

} // namespace

struct RecompShaderChain {
    SDL_GPUDevice *device = nullptr;
    std::vector<Pass> passes;
    std::vector<Lut> luts;
    std::map<std::string, float> parameters;
    std::map<SamplerKey, SDL_GPUSampler *> samplers;
    SDL_GPUBuffer *quad = nullptr;
    // (the game's picture at its own size: this frame's and, for OriginalHistory#, those before it, a ring)
    std::vector<Texture> originals;
    bool original_mipmap = false;
    // (halving steps from the internal resolution down to the game's own, each a 2x2 average)
    std::vector<Texture> steps;
    uint32_t frame = 0u;
    uint32_t viewport_width = 0u, viewport_height = 0u;
    bool failed = false;
};

namespace {

void release(SDL_GPUDevice *device, Texture &texture)
{
    if (texture.texture != nullptr) SDL_ReleaseGPUTexture(device, texture.texture);
    texture = {};
}

bool ensure(SDL_GPUDevice *device, Texture &texture, SDL_GPUTextureFormat format, uint32_t width, uint32_t height,
    bool mipmap)
{
    const uint32_t levels = mipmap ? levelsFor(width, height) : 1u;
    if (texture.texture != nullptr && texture.width == width && texture.height == height && texture.levels == levels)
        return true;
    release(device, texture);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = kTargetUsage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1u;
    info.num_levels = levels;
    texture.texture = SDL_CreateGPUTexture(device, &info);
    if (texture.texture == nullptr) return false;
    texture.width = width;
    texture.height = height;
    texture.levels = levels;
    return true;
}

SDL_GPUSampler *samplerFor(RecompShaderChain *chain, const SamplerKey &key)
{
    SDL_GPUSampler *&sampler = chain->samplers[key];
    if (sampler != nullptr) return sampler;
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = info.mag_filter = key.linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    info.mipmap_mode = key.linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = info.address_mode_v = info.address_mode_w = key.wrap;
    info.max_lod = key.mipmap ? 1000.0f : 0.0f;
    sampler = SDL_CreateGPUSampler(chain->device, &info);
    return sampler;
}

SDL_GPUShader *makeShader(SDL_GPUDevice *device, SDL_GPUShaderFormat format, const std::string &glsl, bool vertex,
    Reflected &reflected, std::string &error)
{
    std::vector<uint8_t> spirv;
    if (!recomp_shader_compile_glsl(pushAsUniform(glsl), vertex, spirv, error)) return nullptr;
    if (!reflect(spirv, vertex, reflected, error)) return nullptr;
    RecompCompiledShader compiled;
    if (format == SDL_GPU_SHADERFORMAT_MSL) {
        if (!recomp_shader_spirv_to_msl(spirv, vertex, compiled, error)) return nullptr;
    } else {
        compiled.code = std::move(spirv);
        compiled.entry_point = "main";
    }
    SDL_GPUShaderCreateInfo info{};
    info.code = compiled.code.data();
    info.code_size = compiled.code.size();
    info.entrypoint = compiled.entry_point.c_str();
    info.format = format;
    info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = static_cast<Uint32>(reflected.textures.size());
    info.num_uniform_buffers = 2u;
    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
    if (shader == nullptr) error = std::string("SDL rejected the shader: ") + SDL_GetError();
    return shader;
}

bool loadLut(RecompShaderChain *chain, SDL_GPUCommandBuffer *commands, const std::string &path, bool mipmap,
    Texture &texture, std::string &error)
{
    SDL_Surface *loaded = SDL_LoadPNG(path.c_str());
    SDL_Surface *surface = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ABGR8888) : nullptr;
    if (loaded != nullptr) SDL_DestroySurface(loaded);
    if (surface == nullptr) {
        error = "cannot read the texture " + path + ": " + SDL_GetError();
        return false;
    }
    const uint32_t width = static_cast<uint32_t>(surface->w), height = static_cast<uint32_t>(surface->h);
    bool made = ensure(chain->device, texture, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, width, height, mipmap);
    SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, width * height * 4u, 0};
    SDL_GPUTransferBuffer *upload = made ? SDL_CreateGPUTransferBuffer(chain->device, &info) : nullptr;
    if (upload != nullptr) {
        if (auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(chain->device, upload, false))) {
            for (uint32_t row = 0; row < height; ++row)
                std::memcpy(mapped + row * width * 4u, static_cast<const uint8_t *>(surface->pixels) +
                    row * static_cast<size_t>(surface->pitch), width * 4u);
            SDL_UnmapGPUTransferBuffer(chain->device, upload);
        }
        if (SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commands)) {
            SDL_GPUTextureTransferInfo from{upload, 0u, 0u, 0u};
            SDL_GPUTextureRegion to{texture.texture, 0u, 0u, 0u, 0u, 0u, width, height, 1u};
            SDL_UploadToGPUTexture(pass, &from, &to, false);
            SDL_EndGPUCopyPass(pass);
        }
        if (mipmap) SDL_GenerateMipmapsForGPUTexture(commands, texture.texture);
        SDL_ReleaseGPUTransferBuffer(chain->device, upload);
    } else {
        made = false;
    }
    SDL_DestroySurface(surface);
    if (!made) error = std::string("cannot make the texture ") + path + ": " + SDL_GetError();
    return made;
}

bool loadChain(RecompShaderChain *chain, SDL_GPUShaderFormat format, const std::string &preset, std::string &error)
{
    Values values;
    if (!readPreset(preset, values, error)) return false;
    const int count = std::atoi(textOf(values, "shaders").c_str());
    if (count <= 0 || count > static_cast<int>(kMaxPasses)) {
        error = "the preset names no passes (shaders = N)";
        return false;
    }
    chain->passes.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        Pass &pass = chain->passes[static_cast<size_t>(i)];
        const std::string n = std::to_string(i);
        const Value *path = valueOf(values, "shader" + n);
        if (path == nullptr) {
            error = "the preset has no shader" + n;
            return false;
        }
        std::string text;
        Source source;
        if (!readShader(joined(path->folder, path->text), text, error)) return false;
        splitStages(text, source);
        for (const Parameter &parameter : source.parameters) chain->parameters.emplace(parameter.name, parameter.value);
        pass.alias = textOf(values, "alias" + n, source.name);
        const bool last = i + 1 == count;
        // (scale_type sets both, scale_type_x and _y each; none: the source's size, the last pass the viewport's)
        const bool scaled = valueOf(values, "scale_type" + n) || valueOf(values, "scale_type_x" + n) ||
            valueOf(values, "scale_type_y" + n);
        for (unsigned axis = 0u; axis < 2u; ++axis) {
            const std::string type = textOf(values, std::string(axis ? "scale_type_y" : "scale_type_x") + n,
                textOf(values, "scale_type" + n, scaled || !last ? "source" : "viewport"));
            pass.scale_type[axis] = type == "viewport" ? Scale::Viewport : type == "absolute" ? Scale::Absolute
                : Scale::Source;
            const std::string scale = textOf(values, std::string(axis ? "scale_y" : "scale_x") + n,
                textOf(values, "scale" + n, "1.0"));
            pass.scale[axis] = std::strtof(scale.c_str(), nullptr);
        }
        pass.sampler.linear = flagOf(values, "filter_linear" + n, false);
        pass.sampler.wrap = wrapNamed(textOf(values, "wrap_mode" + n));
        pass.sampler.mipmap = flagOf(values, "mipmap_input" + n, false);
        if (pass.sampler.mipmap) {
            if (i == 0) chain->original_mipmap = true;
            else chain->passes[static_cast<size_t>(i - 1)].mipmap_output = true;
        }
        pass.frame_count_mod = static_cast<uint32_t>(std::atoi(textOf(values, "frame_count_mod" + n).c_str()));
        // The format: the shader's #pragma format, else the preset's float or sRGB framebuffer, else 8 bits. The
        // last pass's picture goes to the window, which is not sRGB.
        pass.format = formatNamed(source.format);
        if (pass.format == SDL_GPU_TEXTUREFORMAT_INVALID) {
            pass.format = flagOf(values, "float_framebuffer" + n, false) ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
                : flagOf(values, "srgb_framebuffer" + n, false) ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
                : last ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        }
        if (last && pass.format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB)
            pass.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        if (!SDL_GPUTextureSupportsFormat(chain->device, pass.format, SDL_GPU_TEXTURETYPE_2D, kTargetUsage))
            pass.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        SDL_GPUShader *vertex = makeShader(chain->device, format, source.vertex, true, pass.stages[0], error);
        SDL_GPUShader *fragment = vertex
            ? makeShader(chain->device, format, source.fragment, false, pass.stages[1], error) : nullptr;
        if (fragment != nullptr) {
            SDL_GPUColorTargetDescription color{};
            color.format = pass.format;
            const SDL_GPUVertexBufferDescription buffer{0u, 6u * sizeof(float), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0u};
            const SDL_GPUVertexAttribute attributes[] = {
                {0u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0u},
                {1u, 0u, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 4u * sizeof(float)},
            };
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vertex;
            info.fragment_shader = fragment;
            info.vertex_input_state.vertex_buffer_descriptions = &buffer;
            info.vertex_input_state.num_vertex_buffers = 1u;
            info.vertex_input_state.vertex_attributes = attributes;
            info.vertex_input_state.num_vertex_attributes = 2u;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
            info.target_info.color_target_descriptions = &color;
            info.target_info.num_color_targets = 1u;
            pass.pipeline = SDL_CreateGPUGraphicsPipeline(chain->device, &info);
            if (pass.pipeline == nullptr) error = std::string("SDL rejected the pipeline: ") + SDL_GetError();
        }
        if (vertex != nullptr) SDL_ReleaseGPUShader(chain->device, vertex);
        if (fragment != nullptr) SDL_ReleaseGPUShader(chain->device, fragment);
        if (pass.pipeline == nullptr) {
            error = path->text + ": " + error;
            return false;
        }
    }
    // The preset's own values for the parameters
    for (auto &parameter : chain->parameters) {
        if (const Value *value = valueOf(values, parameter.first))
            parameter.second = std::strtof(value->text.c_str(), nullptr);
    }
    // Which outputs a pass samples from the frame before (PassFeedback#, an alias's Feedback), and how far back
    // the game's pictures go (OriginalHistory#)
    size_t history = 0u;
    for (Pass &pass : chain->passes) {
        for (const Reflected &stage : pass.stages) {
            for (const std::string &name : stage.textures) {
                if (startsWith(name, "OriginalHistory"))
                    history = std::max(history, static_cast<size_t>(std::atoi(name.c_str() + 15)));
                if (startsWith(name, "PassFeedback")) {
                    const size_t index = static_cast<size_t>(std::atoi(name.c_str() + 12));
                    if (index < chain->passes.size()) chain->passes[index].feedback = true;
                }
                for (Pass &other : chain->passes)
                    if (!other.alias.empty() && name == other.alias + "Feedback") other.feedback = true;
            }
        }
    }
    chain->originals.resize(history + 1u);
    // The quad every pass draws, and the look-up textures
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(chain->device);
    if (commands == nullptr) {
        error = std::string("no command buffer: ") + SDL_GetError();
        return false;
    }
    bool loaded = true;
    {
        static const float kQuad[] = {
            0, 0, 0, 1, 0, 0,
            1, 0, 0, 1, 1, 0,
            0, 1, 0, 1, 0, 1,
            1, 1, 0, 1, 1, 1,
        };
        const SDL_GPUBufferCreateInfo buffer{SDL_GPU_BUFFERUSAGE_VERTEX, sizeof kQuad, 0};
        chain->quad = SDL_CreateGPUBuffer(chain->device, &buffer);
        const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, sizeof kQuad, 0};
        SDL_GPUTransferBuffer *upload = chain->quad ? SDL_CreateGPUTransferBuffer(chain->device, &info) : nullptr;
        void *mapped = upload ? SDL_MapGPUTransferBuffer(chain->device, upload, false) : nullptr;
        if (mapped != nullptr) {
            std::memcpy(mapped, kQuad, sizeof kQuad);
            SDL_UnmapGPUTransferBuffer(chain->device, upload);
            if (SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commands)) {
                const SDL_GPUTransferBufferLocation from{upload, 0u};
                const SDL_GPUBufferRegion to{chain->quad, 0u, sizeof kQuad};
                SDL_UploadToGPUBuffer(pass, &from, &to, false);
                SDL_EndGPUCopyPass(pass);
            }
        } else {
            error = std::string("cannot make the quad: ") + SDL_GetError();
            loaded = false;
        }
        if (upload != nullptr) SDL_ReleaseGPUTransferBuffer(chain->device, upload);
    }
    std::istringstream names(textOf(values, "textures"));
    for (std::string name; loaded && std::getline(names, name, ';');) {
        name = trim(name);
        const Value *path = valueOf(values, name);
        if (name.empty() || path == nullptr) continue;
        Lut lut;
        lut.name = name;
        lut.sampler = {flagOf(values, name + "_linear", false), flagOf(values, name + "_mipmap", false),
            wrapNamed(textOf(values, name + "_wrap_mode"))};
        loaded = loadLut(chain, commands, joined(path->folder, path->text), lut.sampler.mipmap, lut.texture, error);
        chain->luts.push_back(lut);
    }
    SDL_SubmitGPUCommandBuffer(commands);
    return loaded;
}

/* What a pass's texture of this name is: its texture and sampler; nullptr if the chain has none. */
const Texture *textureNamed(RecompShaderChain *chain, size_t index, const std::string &name, SamplerKey &sampler)
{
    const Pass &pass = chain->passes[index];
    sampler = pass.sampler;
    const auto original = [&](size_t back) -> const Texture * {
        if (back >= chain->originals.size()) return nullptr;
        const size_t ring = chain->originals.size();
        return &chain->originals[(chain->frame + ring - back) % ring];
    };
    if (name == "Source") return index == 0u ? original(0u) : &chain->passes[index - 1u].output;
    if (name == "Original") return original(0u);
    if (startsWith(name, "OriginalHistory")) return original(static_cast<size_t>(std::atoi(name.c_str() + 15)));
    if (startsWith(name, "PassOutput")) {
        const size_t at = static_cast<size_t>(std::atoi(name.c_str() + 10));
        return at < index ? &chain->passes[at].output : nullptr;
    }
    if (startsWith(name, "PassFeedback")) {
        const size_t at = static_cast<size_t>(std::atoi(name.c_str() + 12));
        return at < chain->passes.size() ? &chain->passes[at].previous : nullptr;
    }
    for (size_t at = 0; at < chain->passes.size(); ++at) {
        const std::string &alias = chain->passes[at].alias;
        if (alias.empty()) continue;
        if (name == alias && at < index) return &chain->passes[at].output;
        if (name == alias + "Feedback") return &chain->passes[at].previous;
    }
    for (Lut &lut : chain->luts) {
        if (name == lut.name) {
            sampler = lut.sampler;
            return &lut.texture;
        }
    }
    return nullptr;
}

/* A uniform member's value by its name; false if the chain gives it none (it is left 0). */
bool uniformNamed(RecompShaderChain *chain, size_t index, const std::string &name, float (&value)[16])
{
    const Pass &pass = chain->passes[index];
    const auto size = [&](const Texture *texture) {
        if (texture == nullptr || texture->width == 0u) return false;
        value[0] = static_cast<float>(texture->width);
        value[1] = static_cast<float>(texture->height);
        value[2] = 1.0f / value[0];
        value[3] = 1.0f / value[1];
        return true;
    };
    if (name == "MVP") {
        // (0,0) the top left and (1,1) the bottom right; SDL_GPU's clip space has y up
        static const float kMvp[16] = {2, 0, 0, 0, 0, -2, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
        std::memcpy(value, kMvp, sizeof kMvp);
        return true;
    }
    if (name == "OutputSize") return size(&pass.output);
    if (name == "FinalViewportSize") {
        Texture viewport;
        viewport.width = chain->viewport_width;
        viewport.height = chain->viewport_height;
        return size(&viewport);
    }
    if (name == "FrameCount") {
        value[0] = static_cast<float>(pass.frame_count_mod ? chain->frame % pass.frame_count_mod : chain->frame);
        return true;
    }
    if (name == "FrameDirection" || name == "TotalSubFrames" || name == "CurrentSubFrame") {
        value[0] = 1.0f;
        return true;
    }
    if (name == "OriginalFPS") {
        value[0] = 60.0f;
        return true;
    }
    if (name == "FrameTimeDelta") {
        value[0] = 16667.0f;
        return true;
    }
    if (name == "Rotation") {
        value[0] = 0.0f;
        return true;
    }
    if (name == "OriginalAspect" || name == "OriginalAspectRotated") {
        value[0] = 4.0f / 3.0f;
        return true;
    }
    const auto parameter = chain->parameters.find(name);
    if (parameter != chain->parameters.end()) {
        value[0] = parameter->second;
        return true;
    }
    // A texture's size: SourceSize, OriginalSize, OriginalHistorySize#, PassOutputSize#, PassFeedbackSize#,
    // an alias's or a look-up texture's NameSize
    std::string texture = name;
    for (const char *kind : {"OriginalHistory", "PassOutput", "PassFeedback"}) {
        const std::string sized = std::string(kind) + "Size";
        if (startsWith(name, sized.c_str())) texture = kind + name.substr(sized.size());
    }
    if (texture == name) {
        if (name.size() <= 4u || name.compare(name.size() - 4u, 4u, "Size") != 0) return false;
        texture = name.substr(0, name.size() - 4u);
    }
    SamplerKey unused;
    return size(textureNamed(chain, index, texture, unused));
}

void fillBlock(RecompShaderChain *chain, size_t index, const Block &block, std::vector<uint8_t> &data)
{
    data.assign(std::max<size_t>(block.size, 16u), 0u);
    for (const Member &member : block.members) {
        float value[16] = {};
        if (!uniformNamed(chain, index, member.name, value)) continue;
        for (uint32_t i = 0; i < member.count && i < 16u && member.offset + 4u * (i + 1u) <= data.size(); ++i) {
            if (member.kind == Kind::Float) {
                std::memcpy(&data[member.offset + 4u * i], &value[i], 4u);
            } else {
                const int32_t number = static_cast<int32_t>(std::lround(value[i]));
                std::memcpy(&data[member.offset + 4u * i], &number, 4u);
            }
        }
    }
}

uint32_t sized(Scale type, float scale, uint32_t source, uint32_t viewport)
{
    const float size = type == Scale::Absolute ? scale : (type == Scale::Viewport ? viewport : source) * scale;
    return std::clamp(static_cast<uint32_t>(std::lround(size)), 1u, kMaxSize);
}

void blit(SDL_GPUCommandBuffer *commands, const SDL_GPUBlitRegion &from, const Texture &to)
{
    SDL_GPUBlitInfo info{};
    info.source = from;
    info.destination = {to.texture, 0u, 0u, 0u, 0u, to.width, to.height};
    info.load_op = SDL_GPU_LOADOP_DONT_CARE;
    info.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(commands, &info);
}

} // namespace

RecompShaderChain *recomp_shader_chain_load(SDL_GPUDevice *device, SDL_GPUShaderFormat format,
    const std::string &preset, std::string &error)
{
    if (!recomp_shader_compiler_available()) {
        error = "shaderc is not there";
        return nullptr;
    }
    auto *chain = new RecompShaderChain;
    chain->device = device;
    if (!loadChain(chain, format, preset, error)) {
        recomp_shader_chain_free(chain);
        return nullptr;
    }
    return chain;
}

void recomp_shader_chain_free(RecompShaderChain *chain)
{
    if (chain == nullptr) return;
    for (Pass &pass : chain->passes) {
        if (pass.pipeline != nullptr) SDL_ReleaseGPUGraphicsPipeline(chain->device, pass.pipeline);
        release(chain->device, pass.output);
        release(chain->device, pass.previous);
    }
    for (Lut &lut : chain->luts) release(chain->device, lut.texture);
    for (Texture &texture : chain->originals) release(chain->device, texture);
    for (Texture &texture : chain->steps) release(chain->device, texture);
    for (auto &sampler : chain->samplers)
        if (sampler.second != nullptr) SDL_ReleaseGPUSampler(chain->device, sampler.second);
    if (chain->quad != nullptr) SDL_ReleaseGPUBuffer(chain->device, chain->quad);
    delete chain;
}

namespace {

SDL_GPUTexture *drawChain(RecompShaderChain *chain, SDL_GPUCommandBuffer *commands,
    const SDL_GPUBlitRegion &source, uint32_t original_width, uint32_t original_height, uint32_t viewport_width,
    uint32_t viewport_height, uint32_t frame_count, uint32_t &drawn_width, uint32_t &drawn_height)
{
    SDL_GPUDevice *device = chain->device;
    chain->frame = frame_count;
    chain->viewport_width = viewport_width;
    chain->viewport_height = viewport_height;
    // The game's picture at its own size, halved a step at a time from the internal resolution
    Texture &original = chain->originals[chain->frame % chain->originals.size()];
    if (!ensure(device, original, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, original_width, original_height,
            chain->original_mipmap))
        return nullptr;
    unsigned halvings = 0u;
    while ((original_height << (halvings + 1u)) <= source.h && (original_width << (halvings + 1u)) <= source.w)
        ++halvings;
    chain->steps.resize(halvings > 1u ? halvings - 1u : 0u);
    SDL_GPUBlitRegion from = source;
    for (unsigned step = 0u; step < chain->steps.size(); ++step) {
        const unsigned shift = halvings - 1u - step;
        Texture &half = chain->steps[step];
        if (!ensure(device, half, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, original_width << shift,
                original_height << shift, false))
            return nullptr;
        blit(commands, from, half);
        from = {half.texture, 0u, 0u, 0u, 0u, half.width, half.height};
    }
    blit(commands, from, original);
    if (chain->original_mipmap) SDL_GenerateMipmapsForGPUTexture(commands, original.texture);
    // Each pass's size, and last frame's output kept for those sampled from the frame before
    uint32_t width = original_width, height = original_height;
    for (Pass &pass : chain->passes) {
        width = sized(pass.scale_type[0], pass.scale[0], width, viewport_width);
        height = sized(pass.scale_type[1], pass.scale[1], height, viewport_height);
        if (pass.feedback) {
            std::swap(pass.output, pass.previous);
            if (!ensure(device, pass.previous, pass.format, width, height, pass.mipmap_output)) return nullptr;
        }
        if (!ensure(device, pass.output, pass.format, width, height, pass.mipmap_output)) return nullptr;
    }
    std::vector<uint8_t> data;
    for (size_t index = 0; index < chain->passes.size(); ++index) {
        Pass &pass = chain->passes[index];
        SDL_GPUColorTargetInfo target{};
        target.texture = pass.output.texture;
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *render = SDL_BeginGPURenderPass(commands, &target, 1u, nullptr);
        if (render == nullptr) return nullptr;
        SDL_BindGPUGraphicsPipeline(render, pass.pipeline);
        const SDL_GPUBufferBinding quad{chain->quad, 0u};
        SDL_BindGPUVertexBuffers(render, 0u, &quad, 1u);
        for (unsigned stage = 0u; stage < 2u; ++stage) {
            const Reflected &reflected = pass.stages[stage];
            SDL_GPUTextureSamplerBinding bindings[kMaxSamplers]{};
            for (size_t slot = 0; slot < reflected.textures.size(); ++slot) {
                SamplerKey key;
                const Texture *texture = textureNamed(chain, index, reflected.textures[slot], key);
                if (texture == nullptr || texture->texture == nullptr) {
                    texture = &original;
                    key = pass.sampler;
                }
                key.mipmap = key.mipmap && texture->levels > 1u;
                bindings[slot] = {texture->texture, samplerFor(chain, key)};
            }
            if (!reflected.textures.empty()) {
                const Uint32 count = static_cast<Uint32>(reflected.textures.size());
                if (stage == 0u) SDL_BindGPUVertexSamplers(render, 0u, bindings, count);
                else SDL_BindGPUFragmentSamplers(render, 0u, bindings, count);
            }
            for (unsigned slot = 0u; slot < 2u; ++slot) {
                fillBlock(chain, index, reflected.blocks[slot], data);
                const Uint32 bytes = static_cast<Uint32>(data.size());
                if (stage == 0u) SDL_PushGPUVertexUniformData(commands, slot, data.data(), bytes);
                else SDL_PushGPUFragmentUniformData(commands, slot, data.data(), bytes);
            }
        }
        SDL_DrawGPUPrimitives(render, 4u, 1u, 0u, 0u);
        SDL_EndGPURenderPass(render);
        if (pass.mipmap_output) SDL_GenerateMipmapsForGPUTexture(commands, pass.output.texture);
    }
    drawn_width = chain->passes.back().output.width;
    drawn_height = chain->passes.back().output.height;
    return chain->passes.back().output.texture;
}

} // namespace

SDL_GPUTexture *recomp_shader_chain_draw(RecompShaderChain *chain, SDL_GPUCommandBuffer *commands,
    const SDL_GPUBlitRegion &source, uint32_t original_width, uint32_t original_height, uint32_t viewport_width,
    uint32_t viewport_height, uint32_t frame_count, uint32_t &drawn_width, uint32_t &drawn_height)
{
    SDL_GPUTexture *drawn = drawChain(chain, commands, source, original_width, original_height, viewport_width,
        viewport_height, frame_count, drawn_width, drawn_height);
    // (said once a chain: the picture is shown without it meanwhile)
    if (drawn == nullptr && !chain->failed) std::fprintf(stderr, "recomp shader chain: cannot draw: %s\n", SDL_GetError());
    chain->failed = drawn == nullptr;
    return drawn;
}
