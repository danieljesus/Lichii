// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/logging.h"
#include "common/scope_exit.h"
#include "video_core/vulkan_common/vulkan_instance.h"

namespace {

std::vector<VkExtensionProperties> Listed(std::initializer_list<const char*> names) {
    std::vector<VkExtensionProperties> properties;
    for (const char* name : names) {
        VkExtensionProperties property{};
        std::strncpy(property.extensionName, name, sizeof(property.extensionName) - 1);
        property.specVersion = 1;
        properties.push_back(property);
    }
    return properties;
}

bool Has(const std::vector<const char*>& extensions, std::string_view name) {
    return std::ranges::any_of(extensions, [name](const char* e) { return name == e; });
}

} // Anonymous namespace

TEST_CASE("Optional instance extensions a driver lacks are left out without an error",
          "[video_core][vulkan]") {
    // The headless surface, debug utils and (on macOS) portability enumeration probes used the
    // required extensions' check, which logs "Required instance extension ... is not available"
    // at error level for each one the driver lacks.
    Common::Log::Initialize();
    Common::Log::SetRingCapacity(64);
    SCOPE_EXIT {
        Common::Log::SetRingCapacity(0);
    };
    const Vulkan::vk::InstanceDispatch dld{};
    const auto none = Vulkan::RequiredExtensions(dld, Listed({}),
                                                 Core::Frontend::WindowSystemType::Headless, true);
    CHECK(none.empty());
    for (const auto& line : Common::Log::ReadRing(0)) {
        INFO(line.text);
        CHECK(line.text.find("instance extension") == std::string::npos);
    }

    const auto all = Vulkan::RequiredExtensions(
        dld,
        Listed({VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_EXTENSION_NAME,
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME}),
        Core::Frontend::WindowSystemType::Headless, true);
    CHECK(Has(all, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME));
    CHECK(Has(all, VK_KHR_SURFACE_EXTENSION_NAME));
    CHECK(Has(all, VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
#ifdef __APPLE__
    CHECK(Has(all, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME));
#endif
}
