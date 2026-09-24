#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void tap_open(void);
void tap_push(int channel, const int16_t * samples, unsigned count); // 0 = other party, 1 = user
void tap_close(void);
#ifdef __cplusplus
}
#endif
