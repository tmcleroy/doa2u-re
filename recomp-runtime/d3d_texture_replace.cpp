/* Texture packs (d3d_texture_replace.h). */
#include "d3d_texture_replace.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct Folders {
    std::mutex lock;
    std::filesystem::path replace;
    std::filesystem::path dump;
    std::unordered_map<uint64_t, std::filesystem::path> names;   // the replacement folder's textures
    std::unordered_set<uint64_t> dumped;  // the dump folder's (and those written since)
    bool active = false;
};

Folders &folders()
{
    static Folders state;
    return state;
}

bool parseName(const std::filesystem::path &file, uint64_t &key)
{
    const std::string stem = file.stem().string();
    std::string ext = file.extension().string();
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".dds" || stem.size() != 16u) return false;
    char *end = nullptr;
    key = std::strtoull(stem.c_str(), &end, 16);
    return end == stem.c_str() + 16 && key != 0u;
}

std::string nameOf(uint64_t key)
{
    char text[24];
    std::snprintf(text, sizeof text, "%016llx.dds", static_cast<unsigned long long>(key));
    return text;
}

template <typename Found>
void scan(const std::filesystem::path &folder, Found found)
{
    std::error_code error;
    if (folder.empty() || !std::filesystem::is_directory(folder, error)) return;
    for (auto it = std::filesystem::recursive_directory_iterator(folder,
             std::filesystem::directory_options::follow_directory_symlink, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        uint64_t key;
        if (it->is_regular_file(error) && parseName(it->path(), key)) found(key, it->path());
    }
}

uint32_t blockBytes(uint32_t format_byte)
{
    return format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ? 8u
        : format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT3 || format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT5 ? 16u : 0u;
}

/* The first level's size, or 0 for a texture the packs leave alone */
uint32_t levelBytes(const RecompD3dTextureDesc &desc)
{
    if (desc.linear || desc.depth || desc.render_target || desc.volume_depth != 0u || desc.width == 0u ||
        desc.height == 0u || desc.width > 4096u || desc.height > 4096u) return 0u;
    if (const uint32_t block = blockBytes(desc.format_byte))
        return ((desc.width + 3u) / 4u) * ((desc.height + 3u) / 4u) * block;
    if (desc.bits_per_pixel == 0u || desc.bits_per_pixel % 8u) return 0u;
    return desc.width * desc.height * (desc.bits_per_pixel / 8u);
}

void writeDds(const std::filesystem::path &path, uint32_t width, uint32_t height, const char *fourcc,
    const uint8_t *data, size_t size)
{
    uint8_t head[128]{};
    auto put = [&](size_t at, uint32_t value) { std::memcpy(head + at, &value, 4u); };
    std::memcpy(head, "DDS ", 4u);
    put(4, 124u);
    put(8, fourcc ? 0x81007u : 0x100fu);   // caps, height, width, pixel format, (linear size | pitch)
    put(12, height);
    put(16, width);
    put(20, fourcc ? static_cast<uint32_t>(size) : width * 4u);
    put(28, 1u);
    put(76, 32u);
    if (fourcc) {
        put(80, 4u);
        std::memcpy(head + 84, fourcc, 4u);
    } else {
        put(80, 0x41u);   // RGB with alpha
        put(88, 32u);
        put(92, 0x00ff0000u);
        put(96, 0x0000ff00u);
        put(100, 0x000000ffu);
        put(104, 0xff000000u);
    }
    put(108, 0x1000u);
    const std::filesystem::path partial = path.string() + ".part";
    if (std::FILE *file = std::fopen(partial.string().c_str(), "wb")) {
        const bool written = std::fwrite(head, 1u, sizeof head, file) == sizeof head &&
            std::fwrite(data, 1u, size, file) == size;
        std::fclose(file);
        std::error_code error;
        if (written) std::filesystem::rename(partial, path, error);
        else std::filesystem::remove(partial, error);
    }
}

}  // namespace

extern "C" void recomp_d3d_textures_configure(const char *replace_folder, const char *dump_folder)
{
    const char *env_replace = std::getenv("RECOMP_TEXTURES");
    const char *env_dump = std::getenv("RECOMP_TEXTURE_DUMP");
    if (env_replace != nullptr) replace_folder = env_replace;
    if (env_dump != nullptr) dump_folder = env_dump;
    std::unordered_map<uint64_t, std::filesystem::path> names;
    std::unordered_set<uint64_t> dumped;
    const std::filesystem::path replace = replace_folder ? replace_folder : "";
    const std::filesystem::path dump = dump_folder ? dump_folder : "";
    scan(replace, [&](uint64_t key, const std::filesystem::path &file) { names.emplace(key, file); });
    if (!dump.empty()) {
        std::error_code error;
        std::filesystem::create_directories(dump, error);
        scan(dump, [&](uint64_t key, const std::filesystem::path &) { dumped.insert(key); });
    }
    Folders &state = folders();
    std::lock_guard<std::mutex> hold(state.lock);
    state.replace = replace;
    state.dump = dump;
    state.names.swap(names);
    state.dumped.swap(dumped);
    state.active = !state.names.empty() || !state.dump.empty();
    std::fprintf(stderr, "recomp textures: %zu replacements in '%s'%s%s\n", state.names.size(),
        state.replace.string().c_str(), state.dump.empty() ? "" : ", dumping to ", state.dump.string().c_str());
}

extern "C" bool recomp_d3d_textures_active(void)
{
    Folders &state = folders();
    std::lock_guard<std::mutex> hold(state.lock);
    return state.active;
}

extern "C" uint64_t recomp_d3d_texture_key(const RecompD3dTextureDesc *desc, const void *bytes, uint32_t count,
    const void *palette)
{
    if (desc == nullptr || bytes == nullptr) return 0u;
    const uint32_t size = levelBytes(*desc);
    if (size == 0u || size > count) return 0u;
    uint64_t key = XXH64(bytes, size, 0u);
    if (desc->format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8) {
        if (palette == nullptr) return 0u;
        key = XXH64(palette, 1024u, key);
    }
    return key ? key : 1u;
}

extern "C" bool recomp_d3d_texture_parse_dds(const uint8_t *file, size_t size, RecompD3dReplacement *out)
{
    if (file == nullptr || out == nullptr || size < 128u || std::memcmp(file, "DDS ", 4u) != 0) return false;
    auto word = [&](size_t at) { uint32_t value; std::memcpy(&value, file + at, 4u); return value; };
    const uint32_t height = word(12), width = word(16), levels = word(28) ? word(28) : 1u;
    const uint32_t pf_flags = word(80), fourcc = word(84), bits = word(88);
    if (width == 0u || height == 0u || width > 16384u || height > 16384u || levels > 15u) return false;
    RecompD3dReplacementFormat format;
    uint32_t block = 0u;
    if (pf_flags & 4u) {
        if (std::memcmp(file + 84, "DXT1", 4u) == 0) format = RECOMP_D3D_REPLACEMENT_DXT1, block = 8u;
        else if (std::memcmp(file + 84, "DXT3", 4u) == 0) format = RECOMP_D3D_REPLACEMENT_DXT3, block = 16u;
        else if (std::memcmp(file + 84, "DXT5", 4u) == 0) format = RECOMP_D3D_REPLACEMENT_DXT5, block = 16u;
        else return false;
        (void)fourcc;
    } else if ((pf_flags & 0x40u) && bits == 32u && word(92) == 0x00ff0000u && word(96) == 0x0000ff00u &&
               word(100) == 0x000000ffu) {
        format = RECOMP_D3D_REPLACEMENT_BGRA8;
    } else {
        return false;
    }
    size_t total = 0u;
    uint32_t w = width, h = height, kept = 0u;
    for (; kept < levels; ++kept) {
        const size_t level = block ? static_cast<size_t>((w + 3u) / 4u) * ((h + 3u) / 4u) * block
                                   : static_cast<size_t>(w) * h * 4u;
        if (128u + total + level > size) break;
        total += level;
        w = w > 1u ? w / 2u : 1u;
        h = h > 1u ? h / 2u : 1u;
    }
    if (kept == 0u) return false;
    out->format = format;
    out->width = width;
    out->height = height;
    out->levels = kept;
    out->data = file + 128u;
    out->size = total;
    return true;
}

extern "C" bool recomp_d3d_texture_replacement(uint64_t key, RecompD3dReplacement *out)
{
    static thread_local std::vector<uint8_t> file;
    std::filesystem::path path;
    {
        Folders &state = folders();
        std::lock_guard<std::mutex> hold(state.lock);
        const auto found = key == 0u ? state.names.end() : state.names.find(key);
        if (found == state.names.end()) return false;
        path = found->second;
    }
    std::FILE *handle = std::fopen(path.string().c_str(), "rb");
    if (handle == nullptr) return false;
    std::fseek(handle, 0, SEEK_END);
    const long length = std::ftell(handle);
    std::fseek(handle, 0, SEEK_SET);
    file.resize(length > 0 ? static_cast<size_t>(length) : 0u);
    const bool read = length > 0 && std::fread(file.data(), 1u, file.size(), handle) == file.size();
    std::fclose(handle);
    if (!read || !recomp_d3d_texture_parse_dds(file.data(), file.size(), out)) {
        static unsigned reported;
        if (reported++ < 8u) std::fprintf(stderr, "recomp textures: %s is not a DDS this reads\n", path.string().c_str());
        return false;
    }
    return true;
}

extern "C" void recomp_d3d_texture_dump(uint64_t key, const RecompD3dTextureDesc *desc, const void *bytes,
    uint32_t count, const void *palette)
{
    std::filesystem::path path;
    {
        Folders &state = folders();
        std::lock_guard<std::mutex> hold(state.lock);
        if (key == 0u || state.dump.empty() || !state.dumped.insert(key).second) return;
        path = state.dump / nameOf(key);
    }
    const uint32_t size = levelBytes(*desc);
    if (size == 0u || size > count) return;
    const auto *texels = static_cast<const uint8_t *>(bytes);
    const uint32_t width = desc->width, height = desc->height;
    switch (desc->format_byte) {
    case RECOMP_D3D_TEXTURE_FORMAT_DXT1: writeDds(path, width, height, "DXT1", texels, size); return;
    case RECOMP_D3D_TEXTURE_FORMAT_DXT3: writeDds(path, width, height, "DXT3", texels, size); return;
    case RECOMP_D3D_TEXTURE_FORMAT_DXT5: writeDds(path, width, height, "DXT5", texels, size); return;
    default: break;
    }
    std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4u);
    if (desc->format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8 && palette != nullptr) {
        std::vector<uint8_t> indices(static_cast<size_t>(width) * height);
        if (!recomp_d3d_texture_unswizzle(texels, indices.data(), width, height, 1u)) return;
        const auto *colours = static_cast<const uint8_t *>(palette);
        for (size_t i = 0u; i < indices.size(); ++i) std::memcpy(bgra.data() + i * 4u, colours + indices[i] * 4u, 4u);
    } else if (desc->format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 && desc->bits_per_pixel == 32u) {
        if (!recomp_d3d_texture_unswizzle(texels, bgra.data(), width, height, 4u)) return;
    } else {
        return;   // (other formats: not dumped yet)
    }
    writeDds(path, width, height, nullptr, bgra.data(), bgra.size());
}
