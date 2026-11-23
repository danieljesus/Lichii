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
    /// A recording classifies a storage pointer by reading both of its words.
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        (void)ReadCbufValue(index, offset);
        (void)ReadCbufValue(index, offset + 4);
        storage_pointer_types.emplace((u64{index} << 32) | offset, u8{1});
        return true;
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
u64 Key(u32 bank, u32 offset) {
    return (u64{bank} << 32) | offset;
}

/// Recorded by a build that read a whole handle word as a plain value: handle 7, a 2D texture.
VideoCommon::FileEnvironment OldLayoutRecord() {
    WordEnvironment recorded{{{{2, 0x10}, 7}}, {{7, Color2D}}};
    (void)recorded.ReadTextureType(recorded.ReadCbufValue(2, 0x10));
    return Record(recorded);
}
} // namespace

TEST_CASE("The preparation's translation tells which words it read only as handles",
          "[video_core]") {
    WordEnvironment recorded{{{{2, 0x10}, 7}, {{2, 0x14}, 3}, {{2, 0x18}, 4}}, {}};
    (void)recorded.ReadCbufValue(2, 0x10);
    (void)recorded.ReadCbufValue(2, 0x14);
    (void)recorded.ReadCbufValue(2, 0x18);
    auto file = Record(recorded);
    Shader::Environment& env = file;
    (void)env.ReadCbufHandleWord(2, 0x10); // before observing: not noted
    file.ObserveCbufReads();
    (void)env.ReadCbufHandleWord(2, 0x10);
    (void)env.ReadCbufValue(2, 0x14);       // a value: a folded constant, a table, a part
    (void)env.ReadCbufHandleWord(2, 0x18);  // read both ways
    (void)env.ReadCbufValue(2, 0x18);
    REQUIRE(file.CbufWordsReadOnlyAsHandles() == std::vector<u64>{Key(2, 0x10)});
}

TEST_CASE("A word the preparation read only as a handle is checked by the texture it names",
          "[video_core]") {
    const std::vector<u64> observed{Key(2, 0x10)};
    const char* why = "";
    SECTION("Another handle naming a texture of the same type") {
        auto file = OldLayoutRecord();
        file.UseKeptCbufHandleWords(observed);
        WordEnvironment live{{{{2, 0x10}, 9}}, {{7, Color2D}, {9, Color2D}}};
        REQUIRE(file.AnswersHoldFor(live, &why));
    }
    SECTION("Another handle naming a texture of another type") {
        auto file = OldLayoutRecord();
        file.UseKeptCbufHandleWords(observed);
        WordEnvironment live{{{{2, 0x10}, 9}}, {{7, Color2D}, {9, Color3D}}};
        REQUIRE(!file.AnswersHoldFor(live, &why));
        REQUIRE(std::string_view{why} == "texture type");
    }
    SECTION("Not observed (read both ways, a part, or no kept observation): exact") {
        auto file = OldLayoutRecord();
        file.UseKeptCbufHandleWords({});
        WordEnvironment live{{{{2, 0x10}, 9}}, {{7, Color2D}, {9, Color2D}}};
        REQUIRE(!file.AnswersHoldFor(live, &why));
        REQUIRE(std::string_view{why} == "constant buffer value");
    }
    SECTION("A word the record does not hold is not marked") {
        auto file = OldLayoutRecord();
        file.UseKeptCbufHandleWords(std::vector<u64>{Key(2, 0x20)});
        WordEnvironment live{{{{2, 0x10}, 9}}, {{7, Color2D}, {9, Color2D}}};
        REQUIRE(!file.AnswersHoldFor(live, &why));
        REQUIRE(std::string_view{why} == "constant buffer value");
    }
}

TEST_CASE("Kept handle words follow the info exactly", "[video_core]") {
    std::vector<u8> bytes{0xaa, 0xbb};
    const std::vector<u64> keys{Key(2, 0x10), Key(3, 0x48)};
    VideoCommon::AppendKeptHandleWords(bytes, keys);
    std::vector<u64> read;
    REQUIRE(VideoCommon::ReadKeptHandleWords(std::span{bytes}.subspan(2), read));
    REQUIRE(read == keys);
    // A kept info of the previous format ends with the info: nothing follows it.
    REQUIRE(!VideoCommon::ReadKeptHandleWords(std::span<const u8>{}, read));
    // Short, or followed by anything else.
    REQUIRE(!VideoCommon::ReadKeptHandleWords(std::span{bytes}.subspan(2, bytes.size() - 3), read));
    bytes.push_back(0);
    REQUIRE(!VideoCommon::ReadKeptHandleWords(std::span{bytes}.subspan(2), read));
    std::vector<u8> none;
    VideoCommon::AppendKeptHandleWords(none, {});
    REQUIRE(VideoCommon::ReadKeptHandleWords(none, read));
    REQUIRE(read.empty());
}

TEST_CASE("Each environment's pipeline slot", "[video_core]") {
    using VideoCommon::NoKeptSlot;
    const std::array<u64, 6> task_mesh{0, 77, 88, 0, 0, 55};
    const auto slots = VideoCommon::KeptSlotsByEnvironment(task_mesh);
    REQUIRE(slots[0] == 0);
    REQUIRE(slots[1] == 1);
    REQUIRE(slots[2] == 4);
    REQUIRE(slots[3] == NoKeptSlot);
    const std::array<u64, 6> vertex_a{10, 11, 0, 0, 0, 55};
    const auto with_a = VideoCommon::KeptSlotsByEnvironment(vertex_a);
    REQUIRE(with_a[0] == NoKeptSlot);
    REQUIRE(with_a[1] == 0);
    REQUIRE(with_a[2] == 4);
}

TEST_CASE("A record's own marks give way to the kept observation", "[video_core]") {
    // Marked by the build that recorded it, but this build's translation read the word both ways.
    WordEnvironment recorded{{{{2, 0x10}, 7}}, {{7, Color2D}}};
    (void)recorded.ReadTextureType(recorded.ReadCbufHandleWord(2, 0x10));
    auto file = Record(recorded);
    WordEnvironment same{{{{2, 0x10}, 9}}, {{7, Color2D}, {9, Color2D}}};
    const char* why = "";
    REQUIRE(file.AnswersHoldFor(same, &why)); // the record's own mark, without a kept list
    file.UseKeptCbufHandleWords({});
    WordEnvironment again{{{{2, 0x10}, 9}}, {{7, Color2D}, {9, Color2D}}};
    REQUIRE(!file.AnswersHoldFor(again, &why));
    REQUIRE(std::string_view{why} == "constant buffer value");
}

TEST_CASE("A kept mark still asks the recorded handle of its other readers", "[video_core]") {
    // Word 0x10 was read only as a handle, word 0x14 (same handle 7) both ways. Live, 0x10 names
    // another 2D texture while 0x14 still holds 7, whose texture is now 3D: the kept SPIR-V was
    // made for a 2D texture 7.
    WordEnvironment recorded{{{{2, 0x10}, 7}, {{2, 0x14}, 7}}, {{7, Color2D}}};
    (void)recorded.ReadTextureType(recorded.ReadCbufValue(2, 0x10));
    (void)recorded.ReadCbufValue(2, 0x14);
    auto file = Record(recorded);
    file.UseKeptCbufHandleWords(std::vector<u64>{Key(2, 0x10)});
    WordEnvironment live{{{{2, 0x10}, 9}, {{2, 0x14}, 7}}, {{7, Color3D}, {9, Color2D}}};
    const char* why = "";
    REQUIRE(!file.AnswersHoldFor(live, &why));
    REQUIRE(std::string_view{why} == "texture type");
}

TEST_CASE("Storage pointer words are observed as read by value", "[video_core]") {
    // A recording reads both words of a pointer to classify it; the record answers the
    // classification without reading them, and the observation must see them read alike.
    WordEnvironment recorded{{{{2, 0x10}, 0x1000}, {{2, 0x14}, 0}}, {}};
    (void)recorded.IsCbufStorageBufferPointer(2, 0x10);
    auto file = Record(recorded);
    Shader::Environment& env = file;
    file.ObserveCbufReads();
    (void)env.IsCbufStorageBufferPointer(2, 0x10);
    (void)env.ReadCbufHandleWord(2, 0x10);
    REQUIRE(file.CbufWordsReadOnlyAsHandles().empty());
}

TEST_CASE("A record's own mark the observation confirms keeps its checks", "[video_core]") {
    // The record marked the word itself (a current build, or the builder's oracle, whose handles
    // are tokens): its handle is checked by the live word alone, as without a kept list.
    WordEnvironment recorded{{{{2, 0x10}, 7}}, {{7, Color2D}}};
    (void)recorded.ReadTextureType(recorded.ReadCbufHandleWord(2, 0x10));
    auto file = Record(recorded);
    file.UseKeptCbufHandleWords(std::vector<u64>{Key(2, 0x10)});
    WordEnvironment live{{{{2, 0x10}, 9}}, {{7, Color3D}, {9, Color2D}}};
    const char* why = "";
    REQUIRE(file.AnswersHoldFor(live, &why));
}
