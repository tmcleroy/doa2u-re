/* d3d_texture_replace.h: the names, the DDS reader, and a dump read back as a replacement. */
#include "d3d_texture_replace.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

RecompD3dTextureDesc texture(uint32_t format, uint32_t bits, uint32_t width, uint32_t height)
{
    RecompD3dTextureDesc desc{};
    desc.format_byte = format;
    desc.bits_per_pixel = bits;
    desc.width = width;
    desc.height = height;
    desc.mip_levels = 1u;
    return desc;
}

}  // namespace

int main()
{
    // A DXT1's name: its first level's blocks (later levels and bytes past them do not count)
    std::vector<uint8_t> blocks(8u * 4u * 4u + 64u);
    for (size_t i = 0; i < blocks.size(); ++i) blocks[i] = static_cast<uint8_t>(i * 7u + 3u);
    const RecompD3dTextureDesc dxt1 = texture(RECOMP_D3D_TEXTURE_FORMAT_DXT1, 4u, 16u, 16u);
    const uint64_t key = recomp_d3d_texture_key(&dxt1, blocks.data(), static_cast<uint32_t>(blocks.size()), nullptr);
    check(key == XXH64(blocks.data(), 128u, 0u), "a DXT1's name is XXH64 of its first level");
    blocks[150] ^= 0xffu;
    check(recomp_d3d_texture_key(&dxt1, blocks.data(), static_cast<uint32_t>(blocks.size()), nullptr) == key,
        "bytes past the first level leave the name alone");
    check(recomp_d3d_texture_key(&dxt1, blocks.data(), 100u, nullptr) == 0u, "too few bytes: no name");

    // A P8's name takes its palette
    std::vector<uint8_t> indices(64u * 64u, 5u), palette(1024u, 0x40u);
    const RecompD3dTextureDesc p8 = texture(RECOMP_D3D_TEXTURE_FORMAT_P8, 8u, 64u, 64u);
    const uint64_t p8_key = recomp_d3d_texture_key(&p8, indices.data(), 4096u, palette.data());
    check(p8_key == XXH64(palette.data(), 1024u, XXH64(indices.data(), 4096u, 0u)), "a P8's name has its palette");
    palette[9] = 0x41u;
    check(recomp_d3d_texture_key(&p8, indices.data(), 4096u, palette.data()) != p8_key, "another palette, another name");
    check(recomp_d3d_texture_key(&p8, indices.data(), 4096u, nullptr) == 0u, "a P8 without its palette: no name");

    // Left alone: linear, render targets
    RecompD3dTextureDesc linear = dxt1;
    linear.linear = true;
    check(recomp_d3d_texture_key(&linear, blocks.data(), 256u, nullptr) == 0u, "a linear texture has no name");

    // A dump, read back as the replacement of the same name
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "recomp_texture_replace_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    recomp_d3d_textures_configure(nullptr, folder.string().c_str());
    check(recomp_d3d_textures_active(), "a dump folder makes it active");
    recomp_d3d_texture_dump(key, &dxt1, blocks.data(), static_cast<uint32_t>(blocks.size()), nullptr);
    recomp_d3d_textures_configure(folder.string().c_str(), nullptr);
    RecompD3dReplacement replacement{};
    check(recomp_d3d_texture_replacement(key, &replacement), "the dump is found by its name");
    check(replacement.format == RECOMP_D3D_REPLACEMENT_DXT1 && replacement.width == 16u &&
        replacement.height == 16u && replacement.levels == 1u && replacement.size == 128u &&
        std::memcmp(replacement.data, blocks.data(), 128u) == 0, "and is the same texture");
    check(!recomp_d3d_texture_replacement(key + 1u, &replacement), "another name finds nothing");

    // The DDS reader: a truncated file keeps the levels it has, a strange one is refused
    std::vector<uint8_t> dds(128u + 32u + 8u, 0u);
    std::memcpy(dds.data(), "DDS ", 4u);
    auto put = [&](size_t at, uint32_t value) { std::memcpy(dds.data() + at, &value, 4u); };
    put(12, 8u);
    put(16, 8u);
    put(28, 4u);
    put(80, 4u);
    std::memcpy(dds.data() + 84, "DXT1", 4u);
    check(recomp_d3d_texture_parse_dds(dds.data(), dds.size(), &replacement) && replacement.levels == 2u,
        "a DDS cut short keeps its whole levels");
    std::memcpy(dds.data() + 84, "ATI2", 4u);
    check(!recomp_d3d_texture_parse_dds(dds.data(), dds.size(), &replacement), "an unknown FourCC is refused");

    std::filesystem::remove_all(folder, error);
    if (failures == 0) std::printf("d3d_texture_replace: ok\n");
    return failures == 0 ? 0 : 1;
}
