#include "sbc_receiver.h"
#include <string.h>

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] * 256 + p[1]; }
static bool one(unsigned value) { return value && !(value & (value - 1)); }

int sbc_receiver_init(SbcReceiver *rx, const uint8_t c[4], SbcPcmCallback callback, void *context) {
    if (!rx || !c || !callback) return -1;
    memset(rx, 0, sizeof(*rx));
    if ((c[0] & 0xc0) || !one(c[0] & 0x30) || !one(c[0] & 15) ||
        !one(c[1] & 0xf0) || !one(c[1] & 12) || !one(c[1] & 3) ||
        c[2] < 2 || c[3] > 250 || c[2] > c[3]) return -1;
    unsigned subbands = (c[1] & 4) ? 8 : 4;
    unsigned maximum = (c[0] & 12) ? 16 * subbands : 32 * subbands;
    if (c[3] > maximum) return -1;
    if (sbc_init(&rx->decoder, 0) < 0) return -1;
    rx->decoder.endian = SBC_LE;
    memcpy(rx->config, c, 4);
    rx->rate = (c[0] & 0x10) ? 48000 : 44100;
    rx->channels = (c[0] & 8) ? 1 : 2;
    rx->callback = callback;
    rx->context = context;
    rx->initialized = true;
    return 0;
}

void sbc_receiver_finish(SbcReceiver *rx) {
    if (rx && rx->initialized) {
        sbc_finish(&rx->decoder);
        memset(rx, 0, sizeof(*rx));
    }
}

static bool matches(const SbcReceiver *rx, const uint8_t *p, size_t size) {
    if (size < 4 || p[0] != 0x9c) return false;
    unsigned h = p[1];
    return rx->config[0] == ((0x80 >> (h >> 6)) | (8 >> ((h >> 2) & 3))) &&
        rx->config[1] == ((0x80 >> ((h >> 4) & 3)) | (8 >> (h & 1)) | (1 << ((h >> 1) & 1))) &&
        p[2] >= rx->config[2] && p[2] <= rx->config[3];
}

static int decode(SbcReceiver *rx, const uint8_t *data, size_t size, unsigned count) {
    size_t lengths[15];
    if (!count || count > 15) return -1;
    // Validate the entire packet before emitting any PCM. A malformed trailing
    // frame must not cause partial playback or unbounded reads.
    size_t offset = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!matches(rx, data + offset, size - offset)) return -1;
        ssize_t length = sbc_parse(&rx->decoder, data + offset, size - offset);
        if (length <= 0 || (size_t)length > size - offset) return -1;
        lengths[i] = (size_t)length;
        offset += (size_t)length;
    }
    if (offset != size) return -1;
    offset = 0;
    for (unsigned i = 0; i < count; i++) {
        int16_t pcm[256];
        size_t written = 0;
        ssize_t length = sbc_decode(&rx->decoder, data + offset, lengths[i], pcm, sizeof(pcm), &written);
        if (length != (ssize_t)lengths[i] || !written || written % (2 * rx->channels)) return -1;
        rx->callback(rx->context, pcm, written / (2 * rx->channels), rx->rate, rx->channels);
        rx->frames++;
        offset += lengths[i];
    }
    return 0;
}

int sbc_receiver_packet(SbcReceiver *rx, const uint8_t *p, size_t size) {
    if (!rx || !rx->initialized || !p) return -1;
    if (size < 13 || (p[0] >> 6) != 2) goto bad;
    size_t offset = 12 + 4 * (p[0] & 15);
    if (offset >= size) goto bad;
    if (p[0] & 0x10) {
        if (size - offset < 4) goto bad;
        size_t extension = 4 + 4 * be16(p + offset + 2);
        if (extension >= size - offset) goto bad;
        offset += extension;
    }
    if (p[0] & 0x20) {
        unsigned padding = p[size - 1];
        if (!padding || padding >= size - offset) goto bad;
        size -= padding;
    }
    return sbc_receiver_payload(rx, (uint16_t)be16(p + 2), p + offset, size - offset);
bad:
    rx->fragment_size = 0;
    if (sbc_reinit(&rx->decoder, 0) == 0) rx->decoder.endian = SBC_LE;
    return -1;
}

int sbc_receiver_payload(SbcReceiver *rx, uint16_t sequence, const uint8_t *p, size_t size) {
    if (!rx || !rx->initialized || !p) return -1;
    if (size < 2) goto bad;
    if (rx->have_sequence) {
        uint16_t delta = (uint16_t)(sequence - rx->next_sequence);
        if (delta >= 0x8000) return -2;
        if (delta) {
            rx->fragment_size = 0;
            rx->discontinuities++;
            if (sbc_reinit(&rx->decoder, 0) < 0) goto bad;
            rx->decoder.endian = SBC_LE;
        }
    }
    rx->have_sequence = true;
    rx->next_sequence = (uint16_t)(sequence + 1);
    unsigned h = p[0];
    size_t offset = 1;
    size_t payload = size - 1;
    if ((h & 0x10) || !payload) goto bad;
    if (h & 0x80) {
        // Fragmented packets contain one SBC frame; frame-count is reserved.
        if ((h & 15) || ((h & 0x60) == 0x60)) goto bad;
        if (h & 0x40) rx->fragment_size = 0;
        else if (!rx->fragment_size) goto bad;
        if (payload > sizeof(rx->fragment) - rx->fragment_size) goto bad;
        memcpy(rx->fragment + rx->fragment_size, p + offset, payload);
        rx->fragment_size += payload;
        if (h & 0x20) {
            size_t length = rx->fragment_size;
            rx->fragment_size = 0;
            if (decode(rx, rx->fragment, length, 1) < 0) goto bad;
        }
    } else {
        rx->fragment_size = 0;
        if ((h & 0x60) || decode(rx, p + offset, payload, h & 15) < 0) goto bad;
    }
    rx->packets++;
    return 0;
bad:
    rx->fragment_size = 0;
    // A bad header/CRC may initialize libsbc's parser; discard its state.
    if (sbc_reinit(&rx->decoder, 0) == 0) rx->decoder.endian = SBC_LE;
    return -1;
}
