#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sbc.h"

// Single-threaded RTP/SBC media path. A future firmware transport must supply
// complete L2CAP media SDUs here, after AVDTP configuration and START.
// The callback consumes PCM synchronously; the sample pointer is temporary.
typedef void (*SbcPcmCallback)(void *, const int16_t *, size_t, unsigned, unsigned);
typedef struct {
    sbc_t decoder;
    uint8_t config[4];
    uint8_t fragment[1024];
    size_t fragment_size;
    uint16_t next_sequence;
    bool have_sequence;
    bool initialized;
    unsigned rate;
    unsigned channels;
    uint64_t frames;
    uint64_t packets;
    uint64_t discontinuities;
    SbcPcmCallback callback;
    void *context;
} SbcReceiver;

// config is the four SBC codec-information octets negotiated by AVDTP.
// Only one choice per codec field is accepted. Supports 44.1 and 48 kHz.
int sbc_receiver_init(SbcReceiver *, const uint8_t config[4], SbcPcmCallback, void *);
void sbc_receiver_finish(SbcReceiver *);
// 0: accepted, -1: malformed/unsupported packet, -2: old or duplicate sequence.
int sbc_receiver_packet(SbcReceiver *, const uint8_t *, size_t);
// For a native AVDTP callback that already removed RTP. Input includes the
// one-octet SBC payload header. Supply the original RTP sequence number.
int sbc_receiver_payload(SbcReceiver *, uint16_t sequence, const uint8_t *, size_t);
int sbc_receiver_selftest(void);
