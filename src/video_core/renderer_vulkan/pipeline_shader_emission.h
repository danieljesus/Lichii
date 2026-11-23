// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <span>
#include <vector>

#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"

namespace Vulkan {

inline std::vector<u32> EmitPipelineShader(const Shader::Profile& profile,
                                           const Shader::RuntimeInfo& runtime_info,
                                           Shader::IR::Program& program,
                                           Shader::Backend::Bindings& binding, u64 shader_hash,
                                           std::span<const u32> reference = {}) {
    Shader::Profile routed_profile;
    const Shader::Profile* used{&profile};
    const bool hangs_on_routes{shader_hash == 0xf98012dc9a4465ffULL ||
                               shader_hash == 0xe78efceda31f97a8ULL};
    if (profile.select_global_memory_buffer && program.stage == Shader::Stage::Compute &&
        !hangs_on_routes) {
        routed_profile = profile;
        routed_profile.route_single_candidates = true;
        used = &routed_profile;
    }
    std::vector<u32> code{
        Shader::Backend::SPIRV::EmitSPIRV(*used, runtime_info, program, binding)};
    // LOCAL ONLY: FNV-1a of this emission and of the reference, for a comparison across builds.
    if (std::getenv("LICHII_EMIT_PROBE")) {
        const auto fnv = [](std::span<const u32> words) {
            u64 h = 0xcbf29ce484222325ULL;
            for (const u32 w : words) {
                h = (h ^ w) * 0x100000001b3ULL;
            }
            return h;
        };
        LOG_INFO(Render_Vulkan, "EMITPROBE shader={:016x} stage={} words={} code={:016x} reference={:016x}",
                 shader_hash, static_cast<u32>(program.stage), code.size(), fnv(code),
                 reference.empty() ? u64{0} : fnv(reference));
    }
    if (!reference.empty() && !std::ranges::equal(code, reference)) {
        LOG_ERROR(Render_Vulkan,
                  "Nx2Perf async_pipelines: shader {:016x} worker SPIR-V differs; "
                  "using GPU-thread SPIR-V",
                  shader_hash);
        code.assign(reference.begin(), reference.end());
    }
    return code;
}

/// A compute program's SPIR-V, as EmitPipelineShader emits a graphics stage's
inline std::vector<u32> EmitComputeShader(const Shader::Profile& profile,
                                          Shader::IR::Program& program, u64 shader_hash,
                                          std::span<const u32> reference = {}) {
    Shader::Backend::Bindings binding;
    return EmitPipelineShader(profile, {}, program, binding, shader_hash, reference);
}

} // namespace Vulkan
