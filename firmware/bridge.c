// Runs inside the Bluetooth process. No libc globals, heap ownership in the
// homebrew process, event-queue consumption, or persistent filesystem changes.
#include "bridge.h"

extern void *fw_alloc(unsigned);
extern void fw_free(void *);
extern void *fw_copy(void *, const void *, size_t);
extern void *fw_zero(void *, int, size_t);
extern unsigned fw_config_response(unsigned, unsigned, unsigned, unsigned);

// Installer fills this pointer while the process is suspended. Code remains RX.
__attribute__((used, section(".bridge_pointer")))
SinkBridgeState * const volatile bridge_state = 0;

typedef struct { uint16_t event, length, offset, sequence; } BufferHeader;

static SinkBridgeState *state(void) { return (SinkBridgeState *)bridge_state; }

void bridge_media(uint8_t handle, BufferHeader *buffer, uint32_t timestamp, uint8_t payload_type) {
    SinkBridgeState *s = state();
    if (!s || !buffer) { if (buffer) fw_free(buffer); return; }
    uint32_t write = s->write;
    uint32_t read = __atomic_load_n(&s->read, __ATOMIC_ACQUIRE);
    if (write - read == SINK_QUEUE_COUNT) {
        s->dropped++;
        fw_free(buffer);
        return;
    }
    SinkBridgeSlot *slot = &s->queue[write & (SINK_QUEUE_COUNT - 1)];
    slot->buffer = (uintptr_t)buffer;
    slot->timestamp = timestamp;
    slot->handle = handle;
    slot->payload_type = payload_type;
    __atomic_store_n(&s->write, write + 1, __ATOMIC_RELEASE);
}

void bridge_control(uint8_t handle, const uint8_t *peer, uint8_t event, const uint8_t *data) {
    (void)handle;
    SinkBridgeState *s = state();
    if (!s) return;
    s->event = event;
    if (peer) fw_copy(s->peer, peer, 6);
    if (data) {
        fw_copy(s->event_header, data, 6);
        if (event == 4) {
            const void *configuration = *(const void *const *)(data + 8);
            unsigned error = 0x29, category = 7;
            if (configuration) {
                fw_copy(s->negotiated, configuration, sizeof(s->negotiated));
                const uint8_t *c = s->negotiated;
                unsigned channel = c[3] & 15, blocks = c[4] & 0xf0;
                unsigned bands = c[4] & 12, allocation = c[4] & 3;
                if (c[0] == 6 && !c[1] && !c[2] &&
                    ((c[3] & 0xf0) == 0x10 || (c[3] & 0xf0) == 0x20) &&
                    channel && !(channel & (channel - 1)) && blocks && !(blocks & (blocks - 1)) &&
                    bands && !(bands & (bands - 1)) && allocation && !(allocation & (allocation - 1)) &&
                    c[5] >= 2 && c[6] <= 53 && c[5] <= c[6] && c[0x64] == 1) error = 0;
                if (c[0x65]) { error = 0x29; category = 4; }
            }
            fw_config_response(handle, data[2], error, category);
        }
    }
    __atomic_add_fetch(&s->controls, 1, __ATOMIC_RELEASE);
}

uint32_t bridge_dispatch(void *output, uint32_t *event_type, size_t size) {
    SinkBridgeReply *reply = output;
    SinkBridgeState *s = state();
    fw_zero(output, 0, size);
    *event_type = SINK_BRIDGE_EVENT;
    reply->magic = SINK_BRIDGE_MAGIC;
    if (!s) {
        if (size != SINK_INFO_SIZE) { reply->status = 1; return 0; }
        s = fw_alloc(sizeof(*s));
        if (s) {
            fw_zero(s, 0, sizeof(*s));
            s->magic = SINK_BRIDGE_MAGIC;
        }
        reply->state = (uintptr_t)s;
        if (!s) reply->status = 2;
        return 0;
    }
    reply->state = (uintptr_t)s;
    if (size == SINK_FLUSH_SIZE) {
        // Caller must remove its SEP before destroying callback state.
        uint32_t write = __atomic_load_n(&s->write, __ATOMIC_ACQUIRE);
        for (uint32_t read = s->read; read != write; read++)
            fw_free((void *)(uintptr_t)s->queue[read & (SINK_QUEUE_COUNT - 1)].buffer);
        fw_free(s);
        reply->state = 0;
        return 0;
    }
    if (size == SINK_RPC_SIZE && s->function) {
        typedef uint64_t (*Call)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        s->result = ((Call)(uintptr_t)s->function)(s->args[0], s->args[1], s->args[2], s->args[3], s->args[4], s->args[5]);
    }
    reply->result = s->result;
    uint32_t write = __atomic_load_n(&s->write, __ATOMIC_ACQUIRE);
    uint32_t read = s->read;
    reply->queued = write - read;
    reply->dropped = s->dropped;
    reply->controls = __atomic_load_n(&s->controls, __ATOMIC_ACQUIRE);
    reply->event = s->event;
    if (size == SINK_READ_SIZE && read != write) {
        SinkBridgeSlot *slot = &s->queue[read & (SINK_QUEUE_COUNT - 1)];
        BufferHeader *b = (void *)(uintptr_t)slot->buffer;
        unsigned offset = s->read_offset;
        unsigned length = b->length;
        if (offset <= length && length <= 4096 && b->offset <= 4096) {
            unsigned chunk = length - offset;
            if (chunk > SINK_CHUNK_SIZE) chunk = SINK_CHUNK_SIZE;
            reply->sequence = b->sequence;
            reply->length = length;
            reply->offset = offset;
            reply->chunk = chunk;
            reply->timestamp = slot->timestamp;
            reply->handle = slot->handle;
            reply->payload_type = slot->payload_type;
            fw_copy(reply->data, (uint8_t *)(b + 1) + b->offset + offset, chunk);
            s->read_offset += chunk;
        } else {
            reply->status = 3;
            s->read_offset = length;
        }
        if (s->read_offset == length) {
            s->read_offset = 0;
            fw_free(b);
            __atomic_store_n(&s->read, read + 1, __ATOMIC_RELEASE);
        }
    }
    return 0;
}
