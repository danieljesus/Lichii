// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <sstream>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/shader_environment.h"
#include "video_core/surface.h"

namespace {

/// A compute program's environment answering from fixed constant buffer words and textures, and
/// recording its reads as the backends' environments do.
class WordEnvironment final : public VideoCommon::GenericEnvironment {
public:
    WordEnvironment(std::map<std::pair<u32, u32>, u32> words_,
                    std::map<u32, Shader::TextureType> textures_)
        : words{std::move(words_)}, textures{std::move(textures_)} {
        stage = Shader::Stage::Compute;
        code = {0x123456789abcdef0};
        cached_lowest = 0;
        cached_highest = 0;
        texture_bound = 2;
    }

    u32 ReadCbufValue(u32 index, u32 offset) override {
        const auto it = words.find({index, offset});
        const u32 value = it == words.end() ? 0 : it->second;
        RecordCbufValue(index, offset, value);
        return value;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        const auto it = textures.find(handle);
        const auto type = it == textures.end() ? Shader::TextureType::Color2D : it->second;
        texture_types.emplace(handle, type);
        return type;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        const auto it = formats.find(handle);
        if (it == formats.end()) {
            return {};
        }
        texture_pixel_formats.emplace(handle, it->second);
        return it->second;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        return VideoCore::Surface::IsPixelFormatInteger(
            static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    /// The live register value, and the driver constant folding the backends derive from it.
    void SetTextureBound(u32 value, bool proprietary_driver) {
        texture_bound = value;
        is_proprietary_driver = proprietary_driver;
    }

    /// The pixel format of the texture a handle names (none answered without one).
    void SetFormat(u32 handle, Shader::TexturePixelFormat format) {
        formats[handle] = format;
    }
    /// Code of a native program: it ends with an address map (Shader::Ampere).
    void MakeNative() {
        code = {0x123456789abcdef0, 0, Shader::Ampere::NativeAddressMapMagic};
        cached_highest = 16;
    }
    /// Code with an indirect branch (Maxwell BRX) and an LDC from 	able_bank, where translation
    /// would read its table as values.
    void MakeIndirectBranch(u32 table_bank) {
        code = {0xe25000000000000fULL, 0xef90000000000000ULL | (u64{table_bank} << 36)};
        cached_highest = 8;
    }

private:
    std::map<std::pair<u32, u32>, u32> words;
    std::map<u32, Shader::TextureType> textures;
    std::map<u32, Shader::TexturePixelFormat> formats;
};

VideoCommon::FileEnvironment Load(std::string bytes) {
    std::stringstream stream{std::move(bytes)};
    const auto end = static_cast<std::streamoff>(stream.str().size());
    VideoCommon::FileEnvironment file;
    file.Deserialize(stream, end);
    return file;
}

std::string Serialized(WordEnvironment& env) {
    std::stringstream stream;
    env.Serialize(stream, 0);
    return stream.str();
}

VideoCommon::FileEnvironment Record(WordEnvironment& env) {
    return Load(Serialized(env));
}

constexpr auto Color2D = Shader::TextureType::Color2D;
constexpr auto Color3D = Shader::TextureType::Color3D;

} // Anonymous namespace

TEST_CASE("Handle word marker keys", "[video_core]") {
    const auto key = VideoCommon::CbufHandleWordKey(17, 0xfffc);
    REQUIRE(key.has_value());
    REQUIRE(VideoCommon::CbufHandleWordFromKey(*key) == ((u64{17} << 32) | 0xfffc));
    REQUIRE(!VideoCommon::CbufHandleWordKey(0, 0x10000).has_value());
    REQUIRE(!VideoCommon::CbufHandleWordKey(0x4000, 0).has_value());
    // A marker of the retired layout names no word.
    const u64 retired = *key & ~u64{0x40000000};
    REQUIRE(VideoCommon::IsRetiredCbufHandleWordKey(retired));
    REQUIRE(!VideoCommon::CbufHandleWordFromKey(retired).has_value());
    REQUIRE(!VideoCommon::IsRetiredCbufHandleWordKey(*key));
    // The read properties and the null texture marker are no handle words.
    REQUIRE(!VideoCommon::CbufHandleWordFromKey(u64{~0U} << 32).has_value());
    REQUIRE(!VideoCommon::CbufHandleWordFromKey((u64{~0U} << 32) | 1).has_value());
    REQUIRE(!VideoCommon::CbufHandleWordFromKey((u64{2} << 32) | 0x80000000).has_value());
}

TEST_CASE("Only words read as handles alone may change value", "[video_core]") {
    // Word (2, 0x10) is a handle; word (1, 0x20) is an ordinary value that equals handle 7.
    WordEnvironment recorded{{{{2, 0x10}, 7}, {{1, 0x20}, 7}}, {{7, Color2D}}};
    (void)recorded.ReadTextureType(recorded.ReadCbufHandleWord(2, 0x10));
    (void)recorded.ReadCbufValue(1, 0x20);
    auto file = Record(recorded);

    SECTION("Another handle naming a texture of the same type") {
        WordEnvironment live{{{{2, 0x10}, 9}, {{1, 0x20}, 7}}, {{7, Color2D}, {9, Color2D}}};
        const char* why = "";
        REQUIRE(file.AnswersHoldFor(live, &why));
        // The live environment notes the word as a handle word too, for the disk cache.
        auto rerecorded = Record(live);
        WordEnvironment next{{{{2, 0x10}, 11}, {{1, 0x20}, 7}}, {{7, Color2D}, {11, Color2D}}};
        REQUIRE(rerecorded.AnswersHoldFor(next, &why));
    }
    SECTION("Another handle naming a texture of another type") {
        WordEnvironment live{{{{2, 0x10}, 9}, {{1, 0x20}, 7}}, {{7, Color2D}, {9, Color3D}}};
        const char* why = "";
        REQUIRE(!file.AnswersHoldFor(live, &why));
        REQUIRE(std::string_view{why} == "texture type");
    }
    SECTION("An ordinary value that changed to another handle's value") {
        WordEnvironment live{{{{2, 0x10}, 7}, {{1, 0x20}, 9}}, {{7, Color2D}, {9, Color2D}}};
        const char* why = "";
        REQUIRE(!file.AnswersHoldFor(live, &why));
        REQUIRE(std::string_view{why} == "constant buffer value");
    }
}

TEST_CASE("A word read as a handle and as a value keeps its value", "[video_core]") {
    WordEnvironment recorded{{{{2, 0x10}, 7}}, {{7, Color2D}}};
    (void)recorded.ReadTextureType(recorded.ReadCbufHandleWord(2, 0x10));
    (void)recorded.ReadCbufValue(2, 0x10);
    auto file = Record(recorded);
    WordEnvironment live{{{{2, 0x10}, 9}}, {{7, Color2D}, {9, Color2D}}};
    const char* why = "";
    REQUIRE(!file.AnswersHoldFor(live, &why));
    REQUIRE(std::string_view{why} == "constant buffer value");
}

TEST_CASE("The words of an assembled handle keep their values", "[video_core]") {
    // Handle (A << 1) | B with A = 1 and B = 0 names handle 2; word D is handle 1 alone, so A's
    // value is a recorded handle too. Translation reads A and B as values (texture_pass).
    WordEnvironment recorded{{{{2, 0x10}, 1}, {{2, 0x14}, 0}, {{2, 0x18}, 1}},
                             {{1, Color2D}, {2, Color2D}}};
    (void)recorded.ReadTextureType(recorded.ReadCbufHandleWord(2, 0x18));
    const u32 handle = (recorded.ReadCbufValue(2, 0x10) << 1) | recorded.ReadCbufValue(2, 0x14);
    (void)recorded.ReadTextureType(handle);
    auto file = Record(recorded);
    // Live A = 2: the assembled handle 4 names a 3D texture, while A's value names a 2D one.
    WordEnvironment live{{{{2, 0x10}, 2}, {{2, 0x14}, 0}, {{2, 0x18}, 1}},
                         {{1, Color2D}, {2, Color2D}, {4, Color3D}}};
    const char* why = "";
    REQUIRE(!file.AnswersHoldFor(live, &why));
    REQUIRE(std::string_view{why} == "constant buffer value");
}

TEST_CASE("Markers of the retired layout are dropped", "[video_core]") {
    // A record made when the words of an assembled handle were marked too: A's value is also
    // handle 1, which word D names alone, so A's mark would let A change to any 2D handle.
    WordEnvironment recorded{{{{2, 0x10}, 1}, {{2, 0x14}, 0}, {{2, 0x18}, 1}},
                             {{1, Color2D}, {2, Color2D}}};
    (void)recorded.ReadTextureType(recorded.ReadCbufHandleWord(2, 0x18));
    const u32 handle =
        (recorded.ReadCbufHandleWord(2, 0x10) << 1) | recorded.ReadCbufHandleWord(2, 0x14);
    (void)recorded.ReadTextureType(handle);
    auto bytes = Serialized(recorded);
    for (const u32 offset : {0x10U, 0x14U}) {
        const u64 marker = *VideoCommon::CbufHandleWordKey(2, offset);
        const u64 retired = marker & ~u64{0x40000000};
        const std::string_view from{reinterpret_cast<const char*>(&marker), sizeof(marker)};
        const auto at = bytes.find(from);
        REQUIRE(at != std::string::npos);
        bytes.replace(at, sizeof(retired), reinterpret_cast<const char*>(&retired),
                      sizeof(retired));
    }
    auto file = Load(std::move(bytes));
    const char* why = "";
    WordEnvironment same{{{{2, 0x10}, 1}, {{2, 0x14}, 0}, {{2, 0x18}, 1}},
                         {{1, Color2D}, {2, Color2D}}};
    REQUIRE(file.AnswersHoldFor(same, &why));
    WordEnvironment live{{{{2, 0x10}, 3}, {{2, 0x14}, 0}, {{2, 0x18}, 1}},
                         {{1, Color2D}, {2, Color2D}, {3, Color2D}, {6, Color3D}}};
    REQUIRE(!file.AnswersHoldFor(live, &why));
    REQUIRE(std::string_view{why} == "constant buffer value");
}

namespace {
/// A kept translation with one texture descriptor on `bank` (a handle table's if `handle_program`).
Shader::Info OneTexture(u32 bank, u32 handle_program = ~0U) {
    Shader::Info info{};
    Shader::TextureDescriptor desc{};
    desc.type = Color2D;
    desc.cbuf_index = bank;
    desc.cbuf_offset = 0x10;
    desc.count = 1;
    desc.handle_program = handle_program;
    info.texture_descriptors.push_back(desc);
    return info;
}

/// Recorded with the fixture's texture bound buffer, 2 (the record's driver folding is then on).
VideoCommon::FileEnvironment RecordedWithBound2() {
    WordEnvironment recorded{{{{1, 0x20}, 5}}, {}};
    (void)recorded.ReadCbufValue(1, 0x20);
    return Record(recorded);
}
} // namespace

TEST_CASE("A translation with no descriptor on the recorded bound buffer holds under another value",
          "[video_core]") {
    // A bound texture instruction's descriptor takes the bound buffer as its bank (texture_pass
    // MakeInst), so a translation with none on the recorded bank had none.
    auto file = RecordedWithBound2();
    const char* why = "";
    const Shader::Info no_handles{};
    const Shader::Info other_bank = OneTexture(3);

    SECTION("No handle descriptor, the same driver constant folding") {
        WordEnvironment live{{{{1, 0x20}, 5}}, {}};
        live.SetTextureBound(0, true);
        REQUIRE(file.AnswersHoldFor(live, &why, ~0U, &no_handles));
    }
    SECTION("A bindless descriptor on another bank, the same driver constant folding") {
        WordEnvironment live{{{{1, 0x20}, 5}}, {}};
        live.SetTextureBound(0, true);
        REQUIRE(file.AnswersHoldFor(live, &why, ~0U, &other_bank));
    }
    SECTION("Another value that turns the driver constant folding off") {
        WordEnvironment live{{{{1, 0x20}, 5}}, {}};
        live.SetTextureBound(0, false);
        REQUIRE(!file.AnswersHoldFor(live, &why, ~0U, &no_handles));
        REQUIRE(std::string_view{why} == "texture bound buffer folding");
    }
}

TEST_CASE("A translation with a descriptor on the recorded bound buffer keeps its value",
          "[video_core]") {
    // It may be a bound instruction's, which read its handle from that bank.
    auto file = RecordedWithBound2();
    WordEnvironment live{{{{1, 0x20}, 5}}, {}};
    live.SetTextureBound(0, true);
    const char* why = "";
    const auto rejected = [&](const Shader::Info& on_bank) {
        return !file.AnswersHoldFor(live, &why, ~0U, &on_bank) &&
               std::string_view{why} == "texture bound buffer";
    };
    REQUIRE(rejected(OneTexture(2)));
    Shader::Info image{};
    Shader::ImageDescriptor image_desc{};
    image_desc.type = Color2D;
    image_desc.cbuf_index = 2;
    image_desc.count = 1;
    image_desc.handle_program = ~0U;
    image.image_descriptors.push_back(image_desc);
    REQUIRE(rejected(image));
    Shader::Info texture_buffer{};
    Shader::TextureBufferDescriptor texture_buffer_desc{};
    texture_buffer_desc.cbuf_index = 2;
    texture_buffer_desc.count = 1;
    texture_buffer_desc.handle_program = ~0U;
    texture_buffer.texture_buffer_descriptors.push_back(texture_buffer_desc);
    REQUIRE(rejected(texture_buffer));
    Shader::Info image_buffer{};
    Shader::ImageBufferDescriptor image_buffer_desc{};
    image_buffer_desc.cbuf_index = 2;
    image_buffer_desc.count = 1;
    image_buffer_desc.handle_program = ~0U;
    image_buffer.image_buffer_descriptors.push_back(image_buffer_desc);
    REQUIRE(rejected(image_buffer));
}

TEST_CASE("A handle table descriptor does not read the bound buffer", "[video_core]") {
    // MakeInst gives a handle computed by a handle program bank 0 and offset 0 as placeholders:
    // the handle comes from a storage buffer, so the record holds even when the recorded value is 0.
    // (Recorded with 0, a guest record's driver folding is off; the live value keeps it off.)
    WordEnvironment recorded{{{{1, 0x20}, 5}}, {}};
    recorded.SetTextureBound(0, false);
    (void)recorded.ReadCbufValue(1, 0x20);
    auto file = Record(recorded);
    const Shader::Info table = OneTexture(0, 0);
    WordEnvironment live{{{{1, 0x20}, 5}}, {}};
    live.SetTextureBound(3, false);
    const char* why = "";
    REQUIRE(file.AnswersHoldFor(live, &why, ~0U, &table));
}

TEST_CASE("Without a kept translation the texture bound buffer is compared", "[video_core]") {
    auto file = RecordedWithBound2();
    WordEnvironment live{{{{1, 0x20}, 5}}, {}};
    live.SetTextureBound(0, true);
    const char* why = "";
    REQUIRE(!file.AnswersHoldFor(live, &why));
    REQUIRE(std::string_view{why} == "texture bound buffer");
}

TEST_CASE("Each environment takes the kept translation of its pipeline slot", "[video_core]") {
    // Infos by slot, vertex B to fragment; environments in slot order with empty slots skipped.
    const std::array<Shader::Info, 5> kept{};
    const std::array<const Shader::Info*, 5> slots{&kept[0], &kept[1], &kept[2], &kept[3],
                                                   &kept[4]};
    SECTION("Vertex B and fragment, with gaps between") {
        const std::array<u64, 6> hashes{0, 11, 0, 0, 0, 55};
        const auto infos = VideoCommon::KeptInfosByEnvironment(hashes, slots);
        REQUIRE(infos[0] == &kept[0]);
        REQUIRE(infos[1] == &kept[4]);
        REQUIRE(infos[2] == nullptr);
    }
    SECTION("Task and mesh programs in the vertex B and tessellation control slots") {
        const std::array<u64, 6> hashes{0, 77, 88, 0, 0, 55};
        const auto infos = VideoCommon::KeptInfosByEnvironment(hashes, slots);
        REQUIRE(infos[0] == &kept[0]);
        REQUIRE(infos[1] == &kept[1]);
        REQUIRE(infos[2] == &kept[4]);
    }
    SECTION("A vertex A program has none, and shifts the others") {
        const std::array<u64, 6> hashes{10, 11, 0, 0, 44, 55};
        const auto infos = VideoCommon::KeptInfosByEnvironment(hashes, slots);
        REQUIRE(infos[0] == nullptr);
        REQUIRE(infos[1] == &kept[0]);
        REQUIRE(infos[2] == &kept[3]);
        REQUIRE(infos[3] == &kept[4]);
    }
    SECTION("A slot without a kept translation") {
        std::array<const Shader::Info*, 5> partial = slots;
        partial[4] = nullptr;
        const std::array<u64, 6> hashes{0, 11, 0, 0, 0, 55};
        const auto infos = VideoCommon::KeptInfosByEnvironment(hashes, partial);
        REQUIRE(infos[0] == &kept[0]);
        REQUIRE(infos[1] == nullptr);
    }
}

namespace {
/// A texture descriptor whose handle is (word 0x10 << 1) | word 0x14, both in bank `bank`.
Shader::Info AssembledHandleInfo(u32 bank) {
    Shader::TextureDescriptor desc{};
    desc.type = Color2D;
    desc.has_secondary = true;
    desc.cbuf_index = bank;
    desc.cbuf_offset = 0x10;
    desc.shift_left = 1;
    desc.secondary_cbuf_index = bank;
    desc.secondary_cbuf_offset = 0x14;
    desc.secondary_shift_left = 0;
    desc.count = 1;
    desc.handle_program = ~0U;
    Shader::Info info{};
    info.texture_descriptors.push_back(desc);
    return info;
}

/// A texture descriptor whose handle is the whole word (bank, 0x10), translated as `type`.
Shader::Info WholeWordInfo(u32 bank, Shader::TextureType type) {
    Shader::TextureDescriptor desc{};
    desc.type = type;
    desc.cbuf_index = bank;
    desc.cbuf_offset = 0x10;
    desc.count = 1;
    desc.handle_program = ~0U;
    Shader::Info info{};
    info.texture_descriptors.push_back(desc);
    return info;
}

/// Recorded by a build that read a whole handle word as a plain value: handle 7, a 2D texture.
VideoCommon::FileEnvironment OldLayoutRecord(bool native) {
    WordEnvironment recorded{{{{2, 0x10}, 7}}, {{7, Color2D}}};
    if (native) {
        recorded.MakeNative();
    }
    (void)recorded.ReadTextureType(recorded.ReadCbufValue(2, 0x10));
    return Record(recorded);
}
} // namespace

TEST_CASE("With the kept translation an assembled handle is checked by the texture it names",
          "[video_core]") {
    // Recorded: words 1 and 0, handle 2 (2D). The texture bank is 2.
    WordEnvironment recorded{{{{2, 0x10}, 1}, {{2, 0x14}, 0}}, {{2, Color2D}}};
    const u32 handle = (recorded.ReadCbufValue(2, 0x10) << 1) | recorded.ReadCbufValue(2, 0x14);
    (void)recorded.ReadTextureType(handle);
    auto file = Record(recorded);
    const auto info = AssembledHandleInfo(2);
    const char* why = "";

    SECTION("Other words naming a texture of the same type") {
        WordEnvironment live{{{{2, 0x10}, 2}, {{2, 0x14}, 1}}, {{2, Color2D}, {5, Color2D}}};
        REQUIRE(file.AnswersHoldFor(live, &why, ~0U, &info));
        // Without the kept translation the words keep their values.
        WordEnvironment again{{{{2, 0x10}, 2}, {{2, 0x14}, 1}}, {{2, Color2D}, {5, Color2D}}};
        REQUIRE(!file.AnswersHoldFor(again, &why));
        REQUIRE(std::string_view{why} == "constant buffer value");
    }
    SECTION("Other words naming a texture of another type") {
        WordEnvironment live{{{{2, 0x10}, 2}, {{2, 0x14}, 1}}, {{2, Color2D}, {5, Color3D}}};
        REQUIRE(!file.AnswersHoldFor(live, &why, ~0U, &info));
        REQUIRE(std::string_view{why} == "texture type");
    }
}

TEST_CASE("Words outside the texture bank keep their values in a handle", "[video_core]") {
    // Bank 1 holds the driver constants folded into the code: a handle there stays exact.
    WordEnvironment recorded{{{{1, 0x10}, 1}, {{1, 0x14}, 0}}, {{2, Color2D}}};
    const u32 handle = (recorded.ReadCbufValue(1, 0x10) << 1) | recorded.ReadCbufValue(1, 0x14);
    (void)recorded.ReadTextureType(handle);
    auto file = Record(recorded);
    const auto info = AssembledHandleInfo(1);
    WordEnvironment live{{{{1, 0x10}, 2}, {{1, 0x14}, 1}}, {{2, Color2D}, {5, Color2D}}};
    const char* why = "";
    REQUIRE(!file.AnswersHoldFor(live, &why, ~0U, &info));
    REQUIRE(std::string_view{why} == "constant buffer value");
}

TEST_CASE("A record made before handle words were marked is checked by the kept translation",
          "[video_core]") {
    const auto info = WholeWordInfo(2, Color2D);
    const char* why = "";
    for (const bool native : {false, true}) {
        // The same reasoning holds for a guest and a native program: translation of either
        // reads the handle only to ask the texture it names.
        auto file = OldLayoutRecord(native);
        const auto live_with = [native](Shader::TextureType other) {
            WordEnvironment live{{{{2, 0x10}, 9}}, {{7, Color2D}, {9, other}}};
            if (native) {
                live.MakeNative();
            }
            return live;
        };
        auto same = live_with(Color2D);
        REQUIRE(file.AnswersHoldFor(same, &why, ~0U, &info));
        auto again = live_with(Color2D);
        REQUIRE(!file.AnswersHoldFor(again, &why));
        REQUIRE(std::string_view{why} == "constant buffer value");
        auto other = live_with(Color3D);
        REQUIRE(!file.AnswersHoldFor(other, &why, ~0U, &info));
        REQUIRE(std::string_view{why} == "texture type");
    }
}

TEST_CASE("The recorded handle's own answers are still asked", "[video_core]") {
    // Another descriptor may read the recorded handle: its texture must still be the same.
    auto file = OldLayoutRecord(false);
    const auto info = WholeWordInfo(2, Color2D);
    WordEnvironment live{{{{2, 0x10}, 9}}, {{7, Color3D}, {9, Color2D}}};
    const char* why = "";
    REQUIRE(!file.AnswersHoldFor(live, &why, ~0U, &info));
    REQUIRE(std::string_view{why} == "texture type");
}

TEST_CASE("A handle's pixel format traits are compared", "[video_core]") {
    using Format = Shader::TexturePixelFormat;
    WordEnvironment recorded{{{{2, 0x10}, 7}}, {{7, Color2D}}};
    recorded.SetFormat(7, Format::A8B8G8R8_UNORM);
    (void)recorded.ReadTexturePixelFormat(recorded.ReadCbufValue(2, 0x10));
    auto file = Record(recorded);
    const auto info = WholeWordInfo(2, Color2D);
    const char* why = "";
    const auto live_with = [](Format format) {
        WordEnvironment live{{{{2, 0x10}, 9}}, {{7, Color2D}, {9, Color2D}}};
        live.SetFormat(7, Format::A8B8G8R8_UNORM);
        live.SetFormat(9, format);
        return live;
    };
    auto same = live_with(Format::A8B8G8R8_UNORM);
    REQUIRE(file.AnswersHoldFor(same, &why, ~0U, &info));
    auto snorm = live_with(Format::A8B8G8R8_SNORM);
    REQUIRE(!file.AnswersHoldFor(snorm, &why, ~0U, &info));
    REQUIRE(std::string_view{why} == "texture pixel format");
}

TEST_CASE("Without a recorded texture answer the descriptor is the reference", "[video_core]") {
    // The record read the word but kept no texture answer for it (a played record drops them).
    WordEnvironment recorded{{{{2, 0x10}, 7}}, {}};
    (void)recorded.ReadCbufValue(2, 0x10);
    auto file = Record(recorded);
    const auto info = WholeWordInfo(2, Color2D);
    const char* why = "";
    WordEnvironment same{{{{2, 0x10}, 9}}, {{9, Color2D}}};
    REQUIRE(file.AnswersHoldFor(same, &why, ~0U, &info));
    WordEnvironment other{{{{2, 0x10}, 9}}, {{9, Color3D}}};
    REQUIRE(!file.AnswersHoldFor(other, &why, ~0U, &info));
    REQUIRE(std::string_view{why} == "texture type");
    // The descriptor was translated with a non-integer format.
    WordEnvironment integer{{{{2, 0x10}, 9}}, {{9, Color2D}}};
    integer.SetFormat(9, Shader::TexturePixelFormat::A8B8G8R8_UINT);
    REQUIRE(!file.AnswersHoldFor(integer, &why, ~0U, &info));
    REQUIRE(std::string_view{why} == "texture pixel format");
}

TEST_CASE("A program whose branch table may be in the texture bank keeps its handle words exact",
          "[video_core]") {
    // A branch table is read as values from the bank of the LDC that loads it: from the texture
    // bank, such a word cannot be told apart from a handle's.
    const auto info = WholeWordInfo(2, Color2D);
    const char* why = "";
    const auto run = [&](u32 table_bank) {
        WordEnvironment recorded{{{{2, 0x10}, 7}}, {{7, Color2D}}};
        recorded.MakeIndirectBranch(table_bank);
        (void)recorded.ReadTextureType(recorded.ReadCbufValue(2, 0x10));
        auto file = Record(recorded);
        WordEnvironment live{{{{2, 0x10}, 9}}, {{7, Color2D}, {9, Color2D}}};
        live.MakeIndirectBranch(table_bank);
        return file.AnswersHoldFor(live, &why, ~0U, &info);
    };
    REQUIRE(!run(2));
    REQUIRE(std::string_view{why} == "constant buffer value");
    // A table loaded from another bank leaves the texture bank's words to the handles.
    REQUIRE(run(3));
}
