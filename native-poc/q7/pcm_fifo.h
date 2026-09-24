// Single-threaded PCM FIFO used to pass 8 kHz CVSD audio between the phone link
// and the headset link. Both links are serviced on the BTstack run loop thread,
// so no locking is needed.
// ponytail: drift between the two SCO clocks is handled by dropping backlog above
// PCM_FIFO_HIGH and zero-filling underruns (an audible tick every few minutes at
// worst). Upgrade path: fractional resampling like audio/audio_ring_buffer.h.
#pragma once
#include <stdint.h>
#include <string.h>

#define PCM_FIFO_CAPACITY 1600u /* 200 ms at 8 kHz */
#define PCM_FIFO_HIGH      480u /* 60 ms: above this, trim */
#define PCM_FIFO_TARGET    160u /* 20 ms: trim back to this */

typedef struct {
    int16_t data[PCM_FIFO_CAPACITY];
    unsigned read, count;
    unsigned trimmed, underrun;
} pcm_fifo_t;

static inline void pcm_fifo_reset(pcm_fifo_t * f) { memset(f, 0, sizeof(*f)); }

static inline void pcm_fifo_push(pcm_fifo_t * f, const int16_t * samples, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        if (f->count == PCM_FIFO_CAPACITY) { f->read = (f->read + 1) % PCM_FIFO_CAPACITY; f->count--; f->trimmed++; }
        f->data[(f->read + f->count) % PCM_FIFO_CAPACITY] = samples[i];
        f->count++;
    }
    if (f->count > PCM_FIFO_HIGH) {
        const unsigned drop = f->count - PCM_FIFO_TARGET;
        f->read = (f->read + drop) % PCM_FIFO_CAPACITY; f->count -= drop; f->trimmed += drop;
    }
}

static inline void pcm_fifo_pull(pcm_fifo_t * f, int16_t * out, unsigned n) {
    unsigned i = 0;
    for (; i < n && f->count; ++i) { out[i] = f->data[f->read]; f->read = (f->read + 1) % PCM_FIFO_CAPACITY; f->count--; }
    if (i < n) { memset(out + i, 0, (n - i) * sizeof(int16_t)); f->underrun += n - i; }
}
