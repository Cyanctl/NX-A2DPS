#include "native_sink.h"
#include "bridge_blob.h"
#include <stdarg.h>
#include <string.h>

static Result bad(void) { return MAKERESULT(Module_Libnx, LibnxError_BadInput); }
static void note(NativeSink *s, const char *format, ...) {
    va_list args;
    va_start(args, format); vprintf(format, args); va_end(args);
    if (s->report) {
        va_start(args, format); vfprintf(s->report, format, args); va_end(args);
        fflush(s->report);
    }
}
static Result call(NativeSink *s, u64 offset, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 *out) {
    const u64 args[6] = {a0,a1,a2,a3,a4,0};
    return native_bridge_call(&s->bridge, offset, args, out);
}

Result native_sink_start(NativeSink *s, u64 pid, const LoaderModuleInfo *module, FILE *report) {
    if (s->active || s->bridge.installed) return bad();
    memset(s, 0, sizeof(*s)); s->report = report;
    Result rc = btdrvInitialize();
    if (R_FAILED(rc)) return rc;
    s->driver = true;
    rc = native_bridge_install(&s->bridge, pid, module);
    if (R_SUCCEEDED(rc)) rc = native_bridge_attach_state(&s->bridge);
    if (R_SUCCEEDED(rc)) rc = native_bridge_trace_disconnects(&s->bridge, true);
    note(s, "Sink bridge setup: 0x%08lX\n", (unsigned long)rc);
    uint8_t config[0xA0] = {0};
    config[0] = 6; // SBC codec information: length, audio media type, codec=0.
    config[3] = 0x3F; config[4] = 0xFF; config[5] = 2; config[6] = 53;
    config[0x64] = 1; config[0x66] = 2;
    const u64 control = s->bridge.base + BRIDGE_CONTROL_OFFSET;
    const u64 media = s->bridge.base + BRIDGE_MEDIA_OFFSET;
    memcpy(config + 0x78, &control, 8); memcpy(config + 0x80, &media, 8);
    const u16 mtu = 1000, flush = 0xFFFF;
    memcpy(config + 0x98, &mtu, 2); memcpy(config + 0x9A, &flush, 2);
    config[0x9C] = 1; // Sink endpoint. Media type at +0x9D is audio (0).
    config[0x9E] = 6; // Reconfiguration and content-protection control unsupported.
    if (R_SUCCEEDED(rc)) rc = native_bridge_write(&s->bridge, offsetof(SinkBridgeState, create_stream), config, sizeof(config));
    u64 result = 0;
    if (R_SUCCEEDED(rc)) rc = call(s, 0x8ECC0,
        s->bridge.state + offsetof(SinkBridgeState, reserved),
        s->bridge.state + offsetof(SinkBridgeState, create_stream),0,0,0,&result);
    if (R_SUCCEEDED(rc) && (result & 0xFFFF)) rc = bad();
    if (R_SUCCEEDED(rc)) rc = native_bridge_read(&s->bridge, offsetof(SinkBridgeState, reserved), &s->handle, 1);
    if (R_SUCCEEDED(rc) && !s->handle) rc = bad();
    note(s, "Create sink SEP: rc=0x%08lX native=%lu handle=%u\n", (unsigned long)rc, (unsigned long)result, s->handle);
    if (R_SUCCEEDED(rc)) {
        u64 stream = 0;
        rc = call(s, 0x948C0,s->handle,0,0,0,0,&stream);
        if (R_SUCCEEDED(rc) && !stream) rc = bad();
        if (R_SUCCEEDED(rc)) {
            // AVDT_Register installs the source-oriented BTA connection
            // multiplexer. While receiving, give signaling to our own handler
            // so the source manager does not open/close a competing stream.
            u64 avdt = stream - 0x158 - (s->handle - 1) * 0x210;
            s->connection_callback_address = avdt + 0x2820;
            rc = native_bridge_exchange_module_pointer(&s->bridge, s->connection_callback_address,
                s->bridge.base + 0x633A0, control);
            if (R_SUCCEEDED(rc)) s->connection_callback_replaced = true;
        }
        note(s, "Isolate sink signaling: 0x%08lX\n", (unsigned long)rc);
    }
    if (R_SUCCEEDED(rc)) {
        rc = call(s, 0xF0160,0,0,0,0,0,&result);
        if (R_SUCCEEDED(rc)) s->record = (u32)result;
        if (R_SUCCEEDED(rc) && !s->record) rc = bad();
    }
    const char name[] = "Switch Audio Sink";
    if (R_SUCCEEDED(rc)) rc = native_bridge_write(&s->bridge, offsetof(SinkBridgeState, create_stream), name, sizeof(name));
    if (R_SUCCEEDED(rc)) rc = call(s, 0x8B380, 0x110B,
        s->bridge.state + offsetof(SinkBridgeState, create_stream),0,1,s->record,&result);
    if (R_SUCCEEDED(rc) && (result & 0xFF)) rc = bad();
    note(s, "Register sink SDP: rc=0x%08lX native=%lu record=%08lX\n", (unsigned long)rc,
        (unsigned long)result, (unsigned long)s->record);
    if (R_SUCCEEDED(rc)) {
        // The native helper defaults to A2DP 1.3. This endpoint implements the
        // 1.2 feature set, so advertise that profile version consistently.
        rc = call(s, 0xF0C50,s->record,0x110D,0x0102,0,0,&result);
        if (R_SUCCEEDED(rc) && !(result & 0xFF)) rc = bad();
        note(s, "Set A2DP 1.2 SDP profile: 0x%08lX\n", (unsigned long)rc);
    }
    if (R_SUCCEEDED(rc)) {
        rc = call(s, 0x836B0,0x110B,0,0,0,0,NULL);
        if (R_SUCCEEDED(rc)) s->uuid_added = true;
    }
    if (R_SUCCEEDED(rc)) rc = audio_pipeline_init(&s->audio);
    if (R_SUCCEEDED(rc)) {
        rc = btdrvSetVisibility(true, true);
        if (R_SUCCEEDED(rc)) s->visible = true;
    }
    if (R_FAILED(rc)) {
        Result cleanup = native_sink_stop(s);
        note(s, "Sink start failed: 0x%08lX; cleanup: 0x%08lX\n", (unsigned long)rc, (unsigned long)cleanup);
        return rc;
    }
    s->active = true;
    note(s, "SINK ACTIVE: pair Nintendo Switch in phone Bluetooth settings.\n");
    note(s, "Keep this app open. ZR stops and removes the sink.\n");
    return 0;
}

Result native_sink_stop(NativeSink *s) {
    Result rc = 0;
    if (s->visible) {
        rc = btdrvSetVisibility(false, true);
        if (R_FAILED(rc)) return rc;
        s->visible = false;
    }
    if (s->handle) {
        rc = call(s, 0x8ED30,s->handle,0,0,0,0,NULL);
        if (R_FAILED(rc)) return rc;
        // RemoveStream can finish after a close handshake. Keep callbacks and
        // their state alive until the native stream lookup confirms removal.
        u64 pointer = 1;
        for (unsigned i = 0; i < 100 && pointer; i++) {
            rc = call(s, 0x948C0,s->handle,0,0,0,0,&pointer);
            if (R_FAILED(rc)) return rc;
            if (pointer) svcSleepThread(10000000);
        }
        if (pointer) { note(s, "Waiting for sink stream to close; ZR retries cleanup.\n"); return bad(); }
        s->handle = 0;
    }
    if (s->uuid_added) {
        rc = call(s, 0x836D0,0x110B,0,0,0,0,NULL);
        if (R_FAILED(rc)) return rc;
        s->uuid_added = false;
    }
    if (s->connection_callback_replaced) {
        rc = native_bridge_exchange_module_pointer(&s->bridge, s->connection_callback_address,
            s->bridge.base + BRIDGE_CONTROL_OFFSET, s->bridge.base + 0x633A0);
        if (R_FAILED(rc)) return rc;
        s->connection_callback_replaced = false;
    }
    if (s->record) {
        u64 result = 0;
        rc = call(s, 0xF04B0,s->record,0,0,0,0,&result);
        if (R_FAILED(rc) || !(result & 0xFF)) return R_FAILED(rc) ? rc : bad();
        s->record = 0;
    }
    if (s->bridge.installed) {
        rc = native_bridge_remove(&s->bridge);
        if (R_FAILED(rc)) return rc;
    }
    sbc_receiver_finish(&s->decoder);
    audio_pipeline_finish(&s->audio);
    if (s->driver) { btdrvExit(); s->driver = false; }
    if (s->active) note(s, "Sink removed. Packets=%lu rejected=%lu underruns=%lu overruns=%lu\n",
        (unsigned long)s->received, (unsigned long)s->rejected,
        (unsigned long)s->audio.underruns, (unsigned long)s->audio.overruns);
    s->active = s->configured = false;
    return rc;
}

static Result control(NativeSink *s, const SinkBridgeReply *reply) {
    if (reply->controls == s->controls) return 0;
    uint32_t previous = s->controls;
    s->controls = reply->controls;
    uint8_t trace[32];
    Result event_rc = native_bridge_read(&s->bridge, offsetof(SinkBridgeState, create_stream) + 128, trace, sizeof(trace));
    if (R_FAILED(event_rc)) return event_rc;
    if (s->controls - previous > 8) previous = s->controls - 8;
    for (uint32_t i = previous; i != s->controls; i++) {
        const uint8_t *e = trace + (i & 7) * 4;
        note(s, "AVDTP event=%u status=%u parameter=%u label=%u count=%lu\n",
            e[0],e[1],e[2],e[3],(unsigned long)(i + 1));
    }
    uint8_t header[6] = {0};
    event_rc = native_bridge_read(&s->bridge, offsetof(SinkBridgeState, event_header), header, sizeof(header));
    if (R_FAILED(event_rc)) return event_rc;
    if (reply->event == 4 || (!s->configured && (reply->event == 3 || reply->event == 6))) {
        uint8_t config[0x78];
        Result rc = native_bridge_read(&s->bridge, offsetof(SinkBridgeState, negotiated), config, sizeof(config));
        if (R_FAILED(rc)) return rc;
        note(s, "Negotiated codec: %02X %02X %02X %02X %02X %02X %02X protect=%u\n",
            config[0],config[1],config[2],config[3],config[4],config[5],config[6],config[0x65]);
        sbc_receiver_finish(&s->decoder); s->configured = false;
        audio_pipeline_clear(&s->audio);
        if (config[0] != 6 || config[1] || config[2] || config[0x65] ||
            sbc_receiver_init(&s->decoder, config + 3, audio_pipeline_input, &s->audio)) return bad();
        s->configured = true;
    } else if (reply->event == 10 || reply->event == 17) {
        sbc_receiver_finish(&s->decoder); s->configured = false;
        audio_pipeline_clear(&s->audio);
    }
    return 0;
}

Result native_sink_pump(NativeSink *s) {
    if (!s->active) return 0;
    uint32_t trace[4];
    Result trace_rc = native_bridge_read(&s->bridge, offsetof(SinkBridgeState, disconnect_sent), trace, sizeof(trace));
    if (R_FAILED(trace_rc)) return trace_rc;
    if (trace[0] != s->disconnect_sent) {
        uint64_t caller = 0;
        trace_rc = native_bridge_read(&s->bridge, offsetof(SinkBridgeState, disconnect_caller), &caller, sizeof(caller));
        if (R_FAILED(trace_rc)) return trace_rc;
        note(s, "HCI local Disconnect: handle=%03lX reason=%02lX count=%lu\n",
            (unsigned long)(trace[1] & 0xFFF), (unsigned long)((trace[1] >> 16) & 0xFF), (unsigned long)trace[0]);
        note(s, "Disconnect caller: module+0x%lX\n", (unsigned long)(caller - s->bridge.base));
        s->disconnect_sent = trace[0];
    }
    if (trace[2] != s->disconnect_received) {
        note(s, "HCI Disconnect Complete: status=%02lX handle=%03lX reason=%02lX count=%lu\n",
            (unsigned long)(trace[3] & 0xFF), (unsigned long)((trace[3] >> 8) & 0xFFF),
            (unsigned long)(trace[3] >> 24), (unsigned long)trace[2]);
        s->disconnect_received = trace[2];
    }
    for (unsigned i = 0; i < 12; i++) {
        SinkBridgeReply reply;
        Result rc = native_bridge_query(&s->bridge, SINK_READ_SIZE, &reply);
        if (R_FAILED(rc)) return rc;
        rc = control(s, &reply);
        if (R_FAILED(rc)) return rc;
        if (!reply.queued || !reply.chunk) break;
        if (!reply.offset) {
            s->packet_length = reply.length; s->packet_offset = 0;
            s->packet_sequence = reply.sequence;
        }
        if (reply.offset != s->packet_offset || reply.sequence != s->packet_sequence ||
            reply.length != s->packet_length || reply.length > sizeof(s->packet) ||
            reply.chunk > SINK_CHUNK_SIZE || reply.chunk > s->packet_length - s->packet_offset) return bad();
        memcpy(s->packet + s->packet_offset, reply.data, reply.chunk);
        s->packet_offset += reply.chunk;
        if (s->packet_offset == s->packet_length) {
            s->received++;
            if (!s->configured || sbc_receiver_payload(&s->decoder, reply.sequence, s->packet, s->packet_length)) s->rejected++;
            if (s->received == 1 || s->received % 250 == 0)
                note(s, "Audio packets=%lu decoded frames=%lu rejected=%lu queue drops=%lu\n",
                    (unsigned long)s->received, (unsigned long)s->decoder.frames,
                    (unsigned long)s->rejected, (unsigned long)reply.dropped);
        }
    }
    return audio_pipeline_pump(&s->audio);
}
