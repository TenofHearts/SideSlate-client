#include "h265_receiver.h"

#include <cstdint>
#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>

namespace {
uint32_t GetUint32Arg(napi_env env, napi_value value, uint32_t fallback)
{
    uint32_t result = fallback;
    napi_get_value_uint32(env, value, &result);
    return result;
}

napi_value Start(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value argv[3] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    uint32_t port = argc > 0 ? GetUint32Arg(env, argv[0], 7005) : 7005;
    uint32_t width = argc > 1 ? GetUint32Arg(env, argv[1], 1920) : 1920;
    uint32_t height = argc > 2 ? GetUint32Arg(env, argv[2], 1080) : 1080;
    bool started = H265Receiver::Instance().Start(static_cast<uint16_t>(port), static_cast<int32_t>(width), static_cast<int32_t>(height));
    napi_value result;
    napi_get_boolean(env, started, &result);
    return result;
}

napi_value Stop(napi_env env, napi_callback_info)
{
    H265Receiver::Instance().Stop();
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

napi_value Pause(napi_env env, napi_callback_info)
{
    H265Receiver::Instance().Pause();
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

void SetNamedBool(napi_env env, napi_value object, const char* name, bool value)
{
    napi_value napiValue;
    napi_get_boolean(env, value, &napiValue);
    napi_set_named_property(env, object, name, napiValue);
}

void SetNamedNumber(napi_env env, napi_value object, const char* name, double value)
{
    napi_value napiValue;
    napi_create_double(env, value, &napiValue);
    napi_set_named_property(env, object, name, napiValue);
}

void SetNamedString(napi_env env, napi_value object, const char* name, const std::string& value)
{
    napi_value napiValue;
    napi_create_string_utf8(env, value.c_str(), value.size(), &napiValue);
    napi_set_named_property(env, object, name, napiValue);
}

napi_value GetStats(napi_env env, napi_callback_info)
{
    H265Stats stats = H265Receiver::Instance().GetStats();
    napi_value object;
    napi_create_object(env, &object);
    SetNamedBool(env, object, "running", stats.running);
    SetNamedBool(env, object, "decoderStarted", stats.decoderStarted);
    SetNamedBool(env, object, "surfaceReady", stats.surfaceReady);
    SetNamedNumber(env, object, "packets", static_cast<double>(stats.packets));
    SetNamedNumber(env, object, "bytes", static_cast<double>(stats.bytes));
    SetNamedNumber(env, object, "queuedInputs", static_cast<double>(stats.queuedInputs));
    SetNamedNumber(env, object, "renderedOutputs", static_cast<double>(stats.renderedOutputs));
    SetNamedNumber(env, object, "droppedPackets", static_cast<double>(stats.droppedPackets));
    SetNamedNumber(env, object, "sequenceGaps", static_cast<double>(stats.sequenceGaps));
    SetNamedNumber(env, object, "configPackets", static_cast<double>(stats.configPackets));
    SetNamedNumber(env, object, "keyframes", static_cast<double>(stats.keyframes));
    SetNamedNumber(env, object, "lastSequence", static_cast<double>(stats.lastSequence));
    SetNamedNumber(env, object, "queueDepth", static_cast<double>(stats.queueDepth));
    SetNamedNumber(env, object, "streamWidth", static_cast<double>(stats.streamWidth));
    SetNamedNumber(env, object, "streamHeight", static_cast<double>(stats.streamHeight));
    SetNamedNumber(env, object, "streamFps", static_cast<double>(stats.streamFps));
    SetNamedNumber(env, object, "lastError", static_cast<double>(stats.lastError));
    SetNamedNumber(env, object, "maxReceiveGapMs", stats.maxReceiveGapMs);
    SetNamedNumber(env, object, "maxInputGapMs", stats.maxInputGapMs);
    SetNamedNumber(env, object, "maxRenderGapMs", stats.maxRenderGapMs);
    SetNamedString(env, object, "status", stats.status);
    return object;
}

napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor descriptors[] = {
        {"start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pause", nullptr, Pause, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getStats", nullptr, GetStats, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(descriptors) / sizeof(descriptors[0]), descriptors);

    napi_value xcomponentValue = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xcomponentValue) == napi_ok && xcomponentValue) {
        OH_NativeXComponent* component = nullptr;
        if (napi_unwrap(env, xcomponentValue, reinterpret_cast<void**>(&component)) == napi_ok && component) {
            H265Receiver::Instance().RegisterXComponent(component);
        }
    }
    return exports;
}

static napi_module module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "h265receiver",
    .nm_priv = nullptr,
    .reserved = {nullptr},
};
}

extern "C" __attribute__((constructor)) void RegisterH265ReceiverModule()
{
    napi_module_register(&module);
}
