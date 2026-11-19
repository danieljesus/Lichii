// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <span>
#include <utility>
#include <vector>

#include "audio_core/common/common.h"
#include "audio_core/sink/cubeb_sink.h"
#include "audio_core/sink/sink_stream.h"
#include "audio_core/sink/stream_standby.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/core.h"

#ifdef _WIN32
#include <objbase.h>
#undef CreateEvent
#endif

namespace AudioCore::Sink {
/**
 * Cubeb sink stream, responsible for sinking samples to hardware. Stands in for its device when
 * cubeb gives up on it (see StandbySinkStream).
 */
class CubebSinkStream final : public StandbySinkStream {
public:
    /**
     * Create a new sink stream.
     *
     * @param ctx_             - Cubeb context to create this stream with.
     * @param device_channels_ - Number of channels supported by the hardware.
     * @param system_channels_ - Number of channels the audio systems expect.
     * @param device_name_     - Friendly name of the configured device, empty for the default.
     * @param name_            - Name of this stream.
     * @param type_            - Type of this stream.
     * @param system_          - Core system.
     */
    CubebSinkStream(cubeb* ctx_, u32 device_channels_, u32 system_channels_,
                    std::string device_name_, const std::string& name_, StreamType type_,
                    Core::System& system_)
        : StandbySinkStream(system_, type_), ctx{ctx_}, device_name{std::move(device_name_)} {
#ifdef _WIN32
        com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        name = name_;
        device_channels = device_channels_;
        system_channels = system_channels_;

        if (!ctx) {
            return;
        }

        params.rate = TargetSampleRate;
        params.channels = device_channels;
        params.format = CUBEB_SAMPLE_S16LE;
        params.prefs = CUBEB_STREAM_PREF_NONE;
        switch (params.channels) {
        case 1:
            params.layout = CUBEB_LAYOUT_MONO;
            break;
        case 2:
            params.layout = CUBEB_LAYOUT_STEREO;
            break;
        case 6:
            params.layout = CUBEB_LAYOUT_3F2_LFE;
            break;
        }

        u32 minimum_latency{0};
        const auto latency_error = cubeb_get_min_latency(ctx, &params, &minimum_latency);
        if (latency_error != CUBEB_OK) {
            LOG_CRITICAL(Audio_Sink, "Error getting minimum latency, error: {}", latency_error);
            minimum_latency = TargetSampleCount * 2;
        }

        latency = (std::max)(minimum_latency, TargetSampleCount * 2);

        LOG_INFO(Service_Audio,
                 "Opening cubeb stream {} type {} with: rate {} channels {} (system channels {}) "
                 "latency {}",
                 name, type, params.rate, params.channels, system_channels, latency);

        OpenInitialBackend();
    }

    /**
     * Destroy the sink stream.
     */
    ~CubebSinkStream() override {
        LOG_DEBUG(Service_Audio, "Destructing cubeb stream {}", name);

        // Joins the standby worker, which calls the hooks below.
        Finalize();

#ifdef _WIN32
        if (SUCCEEDED(com_init_result)) {
            CoUninitialize();
        }
#endif
    }

private:
    /// Open on the configured device, or on the default one when none is configured. A reopen
    /// waits for the configured device to come back, and tries the default one after
    /// ConfiguredDeviceAttempts. The device is looked up by name at each open, and its id used
    /// only while the enumeration that gave it is alive.
    void* OpenBackend(u32 attempt) override {
        const bool configured = !device_name.empty() && attempt <= ConfiguredDeviceAttempts;
        if (!device_name.empty() && attempt == ConfiguredDeviceAttempts + 1) {
            LOG_WARNING(Audio_Sink,
                        "Cubeb stream {}: {} has not come back, trying the default device", name,
                        device_name);
        }
        cubeb_device_collection collection{};
        const bool enumerated =
            configured && cubeb_enumerate_devices(ctx,
                                                  type == StreamType::In ? CUBEB_DEVICE_TYPE_INPUT
                                                                         : CUBEB_DEVICE_TYPE_OUTPUT,
                                                  &collection) == CUBEB_OK;
        SCOPE_EXIT {
            if (enumerated) {
                cubeb_device_collection_destroy(ctx, &collection);
            }
        };
        cubeb_devid device{};
        for (size_t i = 0; enumerated && i < collection.count; ++i) {
            const cubeb_device_info& info = collection.device[i];
            if (info.friendly_name && device_name == info.friendly_name &&
                info.state == CUBEB_DEVICE_STATE_ENABLED) {
                device = info.devid;
                break;
            }
        }
        if (configured && !device && attempt > 0) {
            LOG_DEBUG(Audio_Sink, "Cubeb stream {}: {} is not back yet", name, device_name);
            return nullptr;
        }
        cubeb_stream* opened{};
        int error{};
        if (type == StreamType::In) {
            error = cubeb_stream_init(ctx, &opened, name.c_str(), device, &params, nullptr, nullptr,
                                      latency, &CubebSinkStream::DataCallback,
                                      &CubebSinkStream::StateCallback, this);
        } else {
            error = cubeb_stream_init(ctx, &opened, name.c_str(), nullptr, nullptr, device, &params,
                                      latency, &CubebSinkStream::DataCallback,
                                      &CubebSinkStream::StateCallback, this);
        }
        if (error != CUBEB_OK) {
            if (attempt == 0) {
                LOG_ERROR(Audio_Sink, "Error initializing cubeb stream {}, error: {}", name, error);
            } else {
                LOG_DEBUG(Audio_Sink, "Cubeb stream {} could not reopen yet, error {}", name,
                          error);
            }
            return nullptr;
        }
        return opened;
    }

    bool StartBackend(void* backend) override {
        return cubeb_stream_start(static_cast<cubeb_stream*>(backend)) == CUBEB_OK;
    }

    bool StopBackend(void* backend) override {
        return cubeb_stream_stop(static_cast<cubeb_stream*>(backend)) == CUBEB_OK;
    }

    void DestroyBackend(void* backend) override {
        cubeb_stream_destroy(static_cast<cubeb_stream*>(backend));
    }

    /**
     * Main callback from Cubeb. Either expects samples from us (audio render/audio out), or will
     * provide samples to be copied (audio in).
     *
     * @param stream      - Cubeb-specific data about the stream.
     * @param user_data   - Custom data pointer passed along, points to a CubebSinkStream.
     * @param in_buff     - Input buffer to be used if the stream is an input type.
     * @param out_buff    - Output buffer to be used if the stream is an output type.
     * @param num_frames_ - Number of frames of audio in the buffers. Note: Not number of samples.
     */
    static long DataCallback([[maybe_unused]] cubeb_stream* stream, void* user_data,
                             [[maybe_unused]] const void* in_buff, void* out_buff,
                             long num_frames_) {
        auto* impl = static_cast<CubebSinkStream*>(user_data);
        if (!impl) {
            return -1;
        }

        const std::size_t num_channels = impl->GetDeviceChannels();
        const std::size_t frame_size = num_channels;
        const std::size_t num_frames{static_cast<size_t>(num_frames_)};

        if (impl->type == StreamType::In) {
            std::span<const s16> input_buffer{reinterpret_cast<const s16*>(in_buff),
                                              num_frames * frame_size};
            impl->ProcessAudioIn(input_buffer, num_frames);
        } else {
            std::span<s16> output_buffer{reinterpret_cast<s16*>(out_buff), num_frames * frame_size};
            impl->ProcessAudioOutAndRender(output_buffer, num_frames);
        }

        return num_frames_;
    }

    /**
     * Cubeb callback for if a device state changes.
     *
     * An error is cubeb giving up on the device, such as when it was unplugged while the system
     * had no default device to move the stream to. It does not retry, so the stream stands in for
     * the device and reopens.
     *
     * @param stream      - Cubeb-specific data about the stream.
     * @param user_data   - Custom data pointer passed along, points to a CubebSinkStream.
     * @param state       - New state of the device.
     */
    static void StateCallback(cubeb_stream*, void* user_data, cubeb_state state) {
        auto* const impl = static_cast<CubebSinkStream*>(user_data);
        if (state == CUBEB_STATE_ERROR) {
            LOG_WARNING(Audio_Sink,
                        "Cubeb stream {} lost its device: playing it into nothing until it reopens",
                        impl->name);
            impl->BackendLost();
            return;
        }
        LOG_DEBUG(Audio_Sink, "Cubeb stream {} state {}", impl->name, static_cast<int>(state));
    }

    /// Main Cubeb context
    cubeb* ctx{};
    /// Friendly name of the configured device; empty follows the system default
    std::string device_name;
    cubeb_stream_params params{};
    u32 latency{};
#ifdef _WIN32
    /// Result of this stream's COM initialization, balanced only when it succeeded
    HRESULT com_init_result{};
#endif
};

/// Streams open on the device named in the settings, or on the defaults when unspecified.
CubebSink::CubebSink(std::string_view target_device_name) {
    // Cubeb requires COM to be initialized on the thread calling cubeb_init on Windows
#ifdef _WIN32
    com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (cubeb_init(&ctx, "Eden", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        return;
    }

    // Looked up by name at each stream open: a cubeb device id is only valid with the
    // enumeration that returned it.
    if (target_device_name != auto_device_name && !target_device_name.empty()) {
        device_name = target_device_name;
    }

    cubeb_get_max_channel_count(ctx, &device_channels);
    device_channels = device_channels >= 6U ? 6U : 2U;
}

CubebSink::~CubebSink() {
    if (!ctx) {
        return;
    }

    for (auto& sink_stream : sink_streams) {
        sink_stream.reset();
    }

    cubeb_destroy(ctx);

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif
}

/// Open a unidirectional stream on the configured device and with its channel layout.
SinkStream* CubebSink::AcquireSinkStream(Core::System& system, u32 system_channels_,
                                         const std::string& name, StreamType type) {
    system_channels = system_channels_;
    SinkStreamPtr& stream = sink_streams.emplace_back(
        std::make_unique<CubebSinkStream>(ctx, type == StreamType::In ? 2U : device_channels,
                                          system_channels, device_name, name, type, system));
    stream->SetDeviceVolume(device_volume);

    return stream.get();
}

void CubebSink::CloseStream(SinkStream* stream) {
    for (size_t i = 0; i < sink_streams.size(); i++) {
        if (sink_streams[i].get() == stream) {
            sink_streams[i].reset();
            sink_streams.erase(sink_streams.begin() + i);
            break;
        }
    }
}

void CubebSink::CloseStreams() {
    sink_streams.clear();
}

f32 CubebSink::GetDeviceVolume() const {
    return device_volume;
}

void CubebSink::SetDeviceVolume(f32 volume) {
    device_volume = volume;
    for (auto& stream : sink_streams) {
        stream->SetDeviceVolume(volume);
    }
}

void CubebSink::SetSystemVolume(f32 volume) {
    for (auto& stream : sink_streams) {
        stream->SetSystemVolume(volume);
    }
}

std::vector<std::string> ListCubebSinkDevices(bool capture) {
    std::vector<std::string> device_list;
    cubeb* ctx;

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    if (cubeb_init(&ctx, "Eden Device Enumerator", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        return {};
    }

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    auto type{capture ? CUBEB_DEVICE_TYPE_INPUT : CUBEB_DEVICE_TYPE_OUTPUT};
    cubeb_device_collection collection;
    if (cubeb_enumerate_devices(ctx, type, &collection) != CUBEB_OK) {
        LOG_WARNING(Audio_Sink, "Audio output device enumeration not supported");
    } else {
        for (std::size_t i = 0; i < collection.count; i++) {
            const cubeb_device_info& device = collection.device[i];
            if (device.friendly_name && device.friendly_name[0] != '\0' &&
                device.state == CUBEB_DEVICE_STATE_ENABLED) {
                device_list.emplace_back(device.friendly_name);
            }
        }
        cubeb_device_collection_destroy(ctx, &collection);
    }

    cubeb_destroy(ctx);
    return device_list;
}

/* REVERSION TO 3833 - function GetCubebLatency REINTRODUCED FROM 3833 - DIABLO 3 FIX */
u32 GetCubebLatency() {
    cubeb* ctx;

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Init cubeb
    if (cubeb_init(&ctx, "Lichii Latency Getter", nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "cubeb_init failed");
        // Return a large latency so we choose SDL instead.
        return 10000u;
    }

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    // Get min latency
    cubeb_stream_params params{};
    params.rate = TargetSampleRate;
    params.channels = 2;
    params.format = CUBEB_SAMPLE_S16LE;
    params.prefs = CUBEB_STREAM_PREF_NONE;
    params.layout = CUBEB_LAYOUT_STEREO;

    u32 latency{0};
    const auto latency_error = cubeb_get_min_latency(ctx, &params, &latency);
    if (latency_error != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "Error getting minimum latency, error: {}", latency_error);
        latency = TargetSampleCount * 2;
    }
    latency = (std::max)(latency, TargetSampleCount * 2);
    cubeb_destroy(ctx);
    return latency;
}

// REVERTED back to 3833 - Below namespace section and function IsCubebSuitable() removed, reverting to GetCubebLatency() above. - DIABLO 3 FIX
/*
namespace {
static long TmpDataCallback(cubeb_stream*, void*, const void*, void*, long) {
    return TargetSampleCount;
}
static void TmpStateCallback(cubeb_stream*, void*, cubeb_state) {}
} // namespace

bool IsCubebSuitable() {
#if !defined(HAVE_CUBEB)
    return false;
#else
    cubeb* ctx{nullptr};

#ifdef _WIN32
    auto com_init_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    // Init cubeb
    if (cubeb_init(&ctx, "Eden Latency Getter", nullptr) != CUBEB_OK) {
        LOG_ERROR(Audio_Sink, "Cubeb failed to init, it is not suitable.");
        return false;
    }

    SCOPE_EXIT {
        cubeb_destroy(ctx);
    };

#ifdef _WIN32
    if (SUCCEEDED(com_init_result)) {
        CoUninitialize();
    }
#endif

    // Get min latency
    cubeb_stream_params params{};
    params.rate = TargetSampleRate;
    params.channels = 2;
    params.format = CUBEB_SAMPLE_S16LE;
    params.prefs = CUBEB_STREAM_PREF_NONE;
    params.layout = CUBEB_LAYOUT_STEREO;

    u32 latency{0};
    const auto latency_error = cubeb_get_min_latency(ctx, &params, &latency);
    if (latency_error != CUBEB_OK) {
        LOG_ERROR(Audio_Sink, "Cubeb could not get min latency, it is not suitable.");
        return false;
    }
    latency = (std::max)(latency, TargetSampleCount * 2);

    // Test opening a device with standard parameters
    cubeb_devid output_device{0};
    cubeb_devid input_device{0};
    std::string name{"Eden test"};
    cubeb_stream* stream{nullptr};

    if (cubeb_stream_init(ctx, &stream, name.c_str(), input_device, nullptr, output_device, &params,
                          latency, &TmpDataCallback, &TmpStateCallback, nullptr) != CUBEB_OK) {
        LOG_CRITICAL(Audio_Sink, "Cubeb could not open a device, it is not suitable.");
        return false;
    }

    cubeb_stream_stop(stream);
    cubeb_stream_destroy(stream);
    return true;
#endif
}
*/

} // namespace AudioCore::Sink
