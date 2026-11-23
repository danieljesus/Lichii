// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "video_core/pipeline_cache_files.h"
#include "video_core/shader_environment.h"

namespace {

/// A compute program's environment whose constant buffer word (2, 0x10) cannot be read, as a read
/// from an unbound bank throws (GraphicsEnvironment, ComputeEnvironment).
class ThrowingEnvironment final : public VideoCommon::GenericEnvironment {
public:
    ThrowingEnvironment() {
        stage = Shader::Stage::Compute;
        code = {0x123456789abcdef0};
        cached_lowest = 0;
        cached_highest = 0;
    }

    u32 ReadCbufValue(u32 index, u32 offset) override {
        if (index == 2 && offset == 0x10) {
            throw std::runtime_error("unbound bank");
        }
        RecordCbufValue(index, offset, 5);
        return 5;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
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
};

bool Holds(const std::string& bytes, u64 key) {
    for (size_t at = 0; at + sizeof(key) <= bytes.size(); ++at) {
        if (std::memcmp(bytes.data() + at, &key, sizeof(key)) == 0) {
            return true;
        }
    }
    return false;
}

} // Anonymous namespace

TEST_CASE("A handle word read that throws leaves later reads as values", "[video_core]") {
    ThrowingEnvironment env;
    REQUIRE_THROWS(env.ReadCbufHandleWord(2, 0x10));
    (void)env.ReadCbufValue(2, 0x20);
    (void)env.ReadCbufHandleWord(2, 0x30);
    std::stringstream stream;
    env.Serialize(stream, 0);
    const std::string bytes = stream.str();
    // The value read after the throw is not marked as read only as a texture handle; the handle
    // word read after it is.
    REQUIRE(!Holds(bytes, *VideoCommon::CbufHandleWordKey(2, 0x20)));
    REQUIRE(Holds(bytes, *VideoCommon::CbufHandleWordKey(2, 0x30)));
}
