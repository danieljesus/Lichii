// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <sstream>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "video_core/pipeline_cache_files.h"
#include "video_core/shader_environment.h"

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
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return {};
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
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

private:
    std::map<std::pair<u32, u32>, u32> words;
    std::map<u32, Shader::TextureType> textures;
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

TEST_CASE("A record that asked no texture question holds under another texture bound buffer",
          "[video_core]") {
    // Translation asks for the bound buffer only for a bound texture instruction; a program that
    // reads no texture never used it. Recorded with 2 (the record's driver folding is then on).
    WordEnvironment recorded{{{{1, 0x20}, 5}}, {}};
    (void)recorded.ReadCbufValue(1, 0x20);
    auto file = Record(recorded);
    const char* why = "";

    SECTION("Another value with the same driver constant folding") {
        WordEnvironment live{{{{1, 0x20}, 5}}, {}};
        live.SetTextureBound(0, true);
        REQUIRE(file.AnswersHoldFor(live, &why));
    }
    SECTION("Another value that turns the driver constant folding off") {
        WordEnvironment live{{{{1, 0x20}, 5}}, {}};
        live.SetTextureBound(0, false);
        REQUIRE(!file.AnswersHoldFor(live, &why));
        REQUIRE(std::string_view{why} == "texture bound buffer");
    }
}

TEST_CASE("A record that read a texture keeps its texture bound buffer", "[video_core]") {
    WordEnvironment recorded{{{{2, 0x10}, 7}}, {{7, Color2D}}};
    (void)recorded.ReadTextureType(recorded.ReadCbufHandleWord(2, 0x10));
    auto file = Record(recorded);
    WordEnvironment live{{{{2, 0x10}, 7}}, {{7, Color2D}}};
    live.SetTextureBound(0, true);
    const char* why = "";
    REQUIRE(!file.AnswersHoldFor(live, &why));
    REQUIRE(std::string_view{why} == "texture bound buffer");
}
namespace {

/// A texture descriptor whose handle is (word A << 1) | word B, both in bank `bank`.
Shader::Info AssembledHandleInfo(u32 bank) {
    Shader::TextureDescriptor desc{};
    desc.type = Shader::TextureType::Color2D;
    desc.has_secondary = true;
    desc.cbuf_index = bank;
    desc.cbuf_offset = 0x10;
    desc.shift_left = 1;
    desc.secondary_cbuf_index = bank;
    desc.secondary_cbuf_offset = 0x14;
    desc.secondary_shift_left = 0;
    desc.count = 1;
    Shader::Info info{};
    info.texture_descriptors.push_back(desc);
    return info;
}

} // Anonymous namespace

TEST_CASE("With the kept translation an assembled handle is checked by the texture it names",
          "[video_core]") {
    // Recorded: A = 1, B = 0, handle 2 (2D). The texture bank is 2.
    WordEnvironment recorded{{{{2, 0x10}, 1}, {{2, 0x14}, 0}}, {{2, Color2D}}};
    const u32 handle = (recorded.ReadCbufValue(2, 0x10) << 1) | recorded.ReadCbufValue(2, 0x14);
    (void)recorded.ReadTextureType(handle);
    auto file = Record(recorded);
    const auto info = AssembledHandleInfo(2);
    const char* why = "";

    SECTION("Other words naming a texture of the same type") {
        WordEnvironment live{{{{2, 0x10}, 2}, {{2, 0x14}, 1}}, {{2, Color2D}, {5, Color2D}}};
        REQUIRE(file.AnswersHoldFor(live, &why, ~0U, &info));
        // Without the kept translation the words keep their values, as before.
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

TEST_CASE("Words outside the texture bank keep their values in an assembled handle",
          "[video_core]") {
    // Bank 1 holds the driver constants folded into the code (and branch tables may live
    // anywhere but the texture bank's handle words): an assembled handle there stays exact.
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