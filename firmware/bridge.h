#pragma once
#include <stddef.h>
#include <stdint.h>

#define SINK_BRIDGE_MAGIC 0x314B4E53u /* SNK1 */
#define SINK_BRIDGE_EVENT 0xA2D0u
#define SINK_READ_SIZE 0x3C0u
#define SINK_INFO_SIZE 0x3C1u
#define SINK_RPC_SIZE  0x3C2u
#define SINK_FLUSH_SIZE 0x3C3u
#define SINK_QUEUE_COUNT 4u
#define SINK_CHUNK_SIZE (SINK_READ_SIZE - 64u)

typedef struct {
    uint32_t magic, status;
    uint64_t state;
    uint64_t result;
    uint32_t queued, dropped, controls, event;
    uint16_t sequence, length, offset, chunk;
    uint32_t timestamp;
    uint8_t handle, payload_type, reserved[10];
    uint8_t data[SINK_CHUNK_SIZE];
} SinkBridgeReply;
_Static_assert(offsetof(SinkBridgeReply, data) == 64, "Reply header ABI");
_Static_assert(sizeof(SinkBridgeReply) == SINK_READ_SIZE, "Reply ABI");

typedef struct {
    uint64_t buffer;
    uint32_t timestamp;
    uint8_t handle, payload_type, reserved[2];
} SinkBridgeSlot;

typedef struct {
    uint32_t magic, write, read, dropped;
    uint32_t controls, event, read_offset, reserved;
    uint64_t function, args[6];
    uint64_t result;
    uint8_t event_header[8], peer[8];
    uint8_t negotiated[0x78];
    uint8_t create_stream[0xA0];
    SinkBridgeSlot queue[SINK_QUEUE_COUNT];
} SinkBridgeState;
_Static_assert(sizeof(SinkBridgeState) <= 512, "GKI allocation bound");
