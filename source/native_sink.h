#pragma once
#include "native_bridge.h"
#include "audio_pipeline.h"
#include "sbc_receiver.h"
typedef struct {
    NativeBridge bridge;
    AudioPipeline audio;
    SbcReceiver decoder;
    FILE *report;
    uint32_t record, controls;
    uint32_t disconnect_sent, disconnect_received;
    uint16_t packet_sequence;
    size_t packet_length, packet_offset;
    uint8_t packet[4096];
    uint8_t handle;
    bool active, driver, uuid_added, visible, configured;
    uint64_t received, rejected;
    uint64_t connection_callback_address;
    bool connection_callback_replaced;
} NativeSink;
Result native_sink_start(NativeSink *, u64 pid, const LoaderModuleInfo *, FILE *);
Result native_sink_stop(NativeSink *);
Result native_sink_pump(NativeSink *);
