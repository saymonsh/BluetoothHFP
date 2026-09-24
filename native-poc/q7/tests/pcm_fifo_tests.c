// Runnable check for the phone<->headset FIFO: order, underrun fill, backlog trim.
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "../pcm_fifo.h"

static pcm_fifo_t f;

int main(void) {
    int16_t in[600], out[600];
    for (int i = 0; i < 600; ++i) in[i] = (int16_t)(i + 1);

    pcm_fifo_reset(&f);
    pcm_fifo_push(&f, in, 24);
    pcm_fifo_pull(&f, out, 24);
    for (int i = 0; i < 24; ++i) assert(out[i] == i + 1);          // FIFO order

    pcm_fifo_pull(&f, out, 10);
    for (int i = 0; i < 10; ++i) assert(out[i] == 0);              // underrun -> silence
    assert(f.underrun == 10);

    pcm_fifo_reset(&f);
    pcm_fifo_push(&f, in, 500);                                    // above HIGH (480)
    assert(f.count == PCM_FIFO_TARGET);                            // trimmed to target
    pcm_fifo_pull(&f, out, 1);
    assert(out[0] == 500 - PCM_FIFO_TARGET + 1);                   // newest audio kept

    puts("pcm_fifo: ok");
    return 0;
}
