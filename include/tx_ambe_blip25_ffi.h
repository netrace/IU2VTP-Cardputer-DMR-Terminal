#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Iu2vtpAmbeEncoder Iu2vtpAmbeEncoder;

Iu2vtpAmbeEncoder* iu2vtp_ambe_encoder_create(void);
void iu2vtp_ambe_encoder_destroy(Iu2vtpAmbeEncoder* enc);
bool iu2vtp_ambe_encoder_reset(Iu2vtpAmbeEncoder* enc);
bool iu2vtp_ambe_encode_pcm160(Iu2vtpAmbeEncoder* enc,
                               const int16_t* pcm160,
                               uint8_t* ambe9);

#ifdef __cplusplus
}
#endif
