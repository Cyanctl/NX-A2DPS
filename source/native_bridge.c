#include "native_bridge.h"
#include "bridge_blob.h"
#include <stdlib.h>
#include <string.h>

#define BAD() MAKERESULT(Module_Libnx, LibnxError_BadInput)
#define METHOD_OFFSET 0x116C0u
static const u8 expected_build[32] = {
    0xc9,0x1c,0x6f,0xc8,0xaa,0x4c,0x39,0x22,0x2d,0x6c,0xcf,0xe0,0xff,0xf4,0x68,0x54,
    0x31,0x05,0xe5,0x9b,0,0,0,0,0,0,0,0,0,0,0,0
};

static Result remote(NativeBridge *b, void *data, u64 address, size_t size, bool write) {
    Handle debug = INVALID_HANDLE;
    Result rc = svcDebugActiveProcess(&debug, b->pid);
    if (R_SUCCEEDED(rc)) {
        rc = write ? svcWriteDebugProcessMemory(debug, data, address, size)
                   : svcReadDebugProcessMemory(data, debug, address, size);
        svcCloseHandle(debug);
    }
    return rc;
}

Result native_bridge_install(NativeBridge *b, u64 pid, const LoaderModuleInfo *module) {
    memset(b, 0, sizeof(*b));
    if (!pid || module->size != 0x2F4000 || memcmp(module->build_id, expected_build, 32) ||
        !envIsSyscallHinted(0x60) || !envIsSyscallHinted(0x69) ||
        !envIsSyscallHinted(0x6A) || !envIsSyscallHinted(0x6B)) return BAD();
    b->pid = pid; b->base = module->base_address;
    Handle debug = INVALID_HANDLE;
    Result rc = svcDebugActiveProcess(&debug, pid);
    if (R_FAILED(rc)) return rc;
    u8 previous[sizeof(bridge_blob)];
    rc = svcReadDebugProcessMemory(previous, debug, b->base + BRIDGE_ENTRY_OFFSET, sizeof(previous));
    if (R_SUCCEEDED(rc)) {
        for (size_t i = 0; i < sizeof(previous); i++) if (previous[i]) { rc = BAD(); break; }
    }
    if (R_SUCCEEDED(rc)) rc = svcReadDebugProcessMemory(&b->original_entry, debug, b->base + METHOD_OFFSET, 4);
    if (R_SUCCEEDED(rc) && b->original_entry != 0xA9BF7BFDu) rc = BAD();
    bool code_written = false;
    if (R_SUCCEEDED(rc)) {
        rc = svcWriteDebugProcessMemory(debug, bridge_blob, b->base + BRIDGE_ENTRY_OFFSET, sizeof(bridge_blob));
        code_written = R_SUCCEEDED(rc);
    }
    if (R_SUCCEEDED(rc)) {
        u32 branch = 0x14000000u | ((BRIDGE_ENTRY_OFFSET - METHOD_OFFSET) / 4);
        rc = svcWriteDebugProcessMemory(debug, &branch, b->base + METHOD_OFFSET, 4);
        if (R_SUCCEEDED(rc)) b->installed = true;
    }
    if (R_FAILED(rc) && code_written)
        svcWriteDebugProcessMemory(debug, previous, b->base + BRIDGE_ENTRY_OFFSET, sizeof(previous));
    svcCloseHandle(debug);
    return rc;
}

Result native_bridge_query(NativeBridge *b, size_t operation, SinkBridgeReply *reply) {
    if (!b->installed || operation < SINK_READ_SIZE || operation > SINK_FLUSH_SIZE) return BAD();
    // All four request sizes fit within the standard 0x400-byte IPC pointer
    // buffer. A little extra output storage handles the operation selectors.
    u8 output[SINK_FLUSH_SIZE] = {0};
    BtdrvEventType event = 0;
    Result rc = btdrvGetEventInfo(output, operation, &event);
    if (R_FAILED(rc)) return rc;
    memcpy(reply, output, sizeof(*reply));
    if ((u32)event != SINK_BRIDGE_EVENT || reply->magic != SINK_BRIDGE_MAGIC || reply->status) return BAD();
    return 0;
}

Result native_bridge_write(NativeBridge *b, size_t offset, const void *input, size_t size) {
    if (!b->installed || !b->state || offset > sizeof(SinkBridgeState) || size > sizeof(SinkBridgeState) - offset) return BAD();
    return remote(b, (void *)input, b->state + offset, size, true);
}

Result native_bridge_read(NativeBridge *b, size_t offset, void *output, size_t size) {
    if (!b->installed || !b->state || offset > sizeof(SinkBridgeState) || size > sizeof(SinkBridgeState) - offset) return BAD();
    return remote(b, output, b->state + offset, size, false);
}

Result native_bridge_attach_state(NativeBridge *b) {
    SinkBridgeReply reply;
    Result rc = native_bridge_query(b, SINK_INFO_SIZE, &reply);
    if (R_FAILED(rc)) return rc;
    if (!reply.state) return BAD();
    rc = remote(b, &reply.state, b->base + BRIDGE_STATE_OFFSET, sizeof(reply.state), true);
    if (R_SUCCEEDED(rc)) b->state = reply.state;
    return rc;
}

Result native_bridge_call(NativeBridge *b, u64 offset, const u64 args[6], u64 *result) {
    if (!b->state || offset >= 0x118000 || (offset & 3)) return BAD();
    struct { u64 function, args[6]; } call = { .function = b->base + offset };
    memcpy(call.args, args, sizeof(call.args));
    Result rc = native_bridge_write(b, offsetof(SinkBridgeState, function), &call, sizeof(call));
    SinkBridgeReply reply;
    if (R_SUCCEEDED(rc)) rc = native_bridge_query(b, SINK_RPC_SIZE, &reply);
    if (R_SUCCEEDED(rc) && result) *result = reply.result;
    return rc;
}

Result native_bridge_remove(NativeBridge *b) {
    if (!b->installed) return 0;
    if (b->state) {
        SinkBridgeReply reply;
        Result rc = native_bridge_query(b, SINK_FLUSH_SIZE, &reply);
        if (R_FAILED(rc)) return rc;
        b->state = 0;
    }
    Handle debug = INVALID_HANDLE;
    Result rc = svcDebugActiveProcess(&debug, b->pid);
    if (R_FAILED(rc)) return rc;
    u32 current = 0;
    u32 branch = 0x14000000u | ((BRIDGE_ENTRY_OFFSET - METHOD_OFFSET) / 4);
    rc = svcReadDebugProcessMemory(&current, debug, b->base + METHOD_OFFSET, 4);
    if (R_SUCCEEDED(rc) && current != branch) rc = BAD();
    if (R_SUCCEEDED(rc)) rc = svcWriteDebugProcessMemory(debug, &b->original_entry, b->base + METHOD_OFFSET, 4);
    if (R_SUCCEEDED(rc)) {
        u8 zero[sizeof(bridge_blob)] = {0};
        rc = svcWriteDebugProcessMemory(debug, zero, b->base + BRIDGE_ENTRY_OFFSET, sizeof(zero));
        if (R_SUCCEEDED(rc)) b->installed = false;
    }
    svcCloseHandle(debug);
    return rc;
}

static void test_log(FILE *report, const char *name, Result rc) {
    printf("%-32s 0x%08lX\n", name, (unsigned long)rc);
    if (report) { fprintf(report, "%-32s 0x%08lX\n", name, (unsigned long)rc); fflush(report); }
}

int native_bridge_test(u64 pid, const LoaderModuleInfo *module, FILE *report) {
    NativeBridge b;
    Result rc = btdrvInitialize();
    test_log(report, "Bridge btdrvInitialize", rc);
    if (R_FAILED(rc)) return 1;
    rc = native_bridge_install(&b, pid, module);
    test_log(report, "Install temporary IPC bridge", rc);
    SinkBridgeReply reply;
    if (R_SUCCEEDED(rc)) rc = native_bridge_query(&b, SINK_INFO_SIZE, &reply);
    test_log(report, "Native IPC handshake", rc);
    if (R_SUCCEEDED(rc)) {
        b.state = reply.state;
        if (!b.state) rc = BAD();
        else rc = remote(&b, &b.state, b.base + BRIDGE_STATE_OFFSET, sizeof(b.state), true);
        test_log(report, "Attach native bridge state", rc);
    }
    u64 target = 0;
    if (R_SUCCEEDED(rc)) {
        const u64 args[6] = {1016,0,0,0,0,0};
        rc = native_bridge_call(&b, 0x89070, args, &target);
        if (R_SUCCEEDED(rc) && !target) rc = BAD();
        test_log(report, "Allocate native test packet", rc);
    }
    if (R_SUCCEEDED(rc)) {
        u8 packet[1016] = {0};
        u16 header[4] = {0,1000,4,0xABCD};
        memcpy(packet, header, sizeof(header));
        for (unsigned i = 0; i < 1000; i++) packet[12 + i] = (u8)(i * 13 + 7);
        rc = remote(&b, packet, target, sizeof(packet), true);
        if (R_SUCCEEDED(rc)) {
            const u64 args[6] = {7,target,0x12345678,96,0,0};
            rc = native_bridge_call(&b, BRIDGE_MEDIA_OFFSET, args, NULL);
            if (R_SUCCEEDED(rc)) target = 0; // Native queue now owns the packet.
        }
        size_t offset = 0;
        for (unsigned part = 0; R_SUCCEEDED(rc) && part < 2; part++) {
            rc = native_bridge_query(&b, SINK_READ_SIZE, &reply);
            if (R_FAILED(rc)) break;
            if (reply.sequence != 0xABCD || reply.length != 1000 || reply.offset != offset ||
                !reply.chunk || reply.chunk > SINK_CHUNK_SIZE || reply.handle != 7 ||
                reply.timestamp != 0x12345678 || reply.payload_type != 96) { rc = BAD(); break; }
            for (unsigned i = 0; i < reply.chunk; i++)
                if (reply.data[i] != (u8)((offset + i) * 13 + 7)) { rc = BAD(); break; }
            offset += reply.chunk;
        }
        if (R_SUCCEEDED(rc) && offset != 1000) rc = BAD();
        test_log(report, "Native media IPC roundtrip", rc);
    }
    if (target && b.state) {
        const u64 args[6] = {target,0,0,0,0,0};
        native_bridge_call(&b, 0x892F0, args, NULL);
    }
    Result cleanup = native_bridge_remove(&b);
    test_log(report, "Restore Bluetooth code", cleanup);
    btdrvExit();
    return R_FAILED(rc) || R_FAILED(cleanup);
}
