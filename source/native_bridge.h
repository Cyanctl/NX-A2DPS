#pragma once
#include <switch.h>
#include <stdio.h>
#include "bridge.h"

typedef struct {
    u64 pid, base, state;
    bool installed;
    u32 original_entry;
} NativeBridge;

Result native_bridge_install(NativeBridge *bridge, u64 pid, const LoaderModuleInfo *module);
Result native_bridge_query(NativeBridge *, size_t operation, SinkBridgeReply *);
Result native_bridge_write(NativeBridge *, size_t state_offset, const void *, size_t);
Result native_bridge_read(NativeBridge *, size_t state_offset, void *, size_t);
Result native_bridge_attach_state(NativeBridge *);
Result native_bridge_call(NativeBridge *, u64 function_offset, const u64 args[6], u64 *result);
Result native_bridge_remove(NativeBridge *);
int native_bridge_test(u64 pid, const LoaderModuleInfo *module, FILE *report);
