/* wav.h - minimal 16-bit mono PCM WAV writer (C99) */
#ifndef SID_WAV_H
#define SID_WAV_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef struct wav_writer {
    FILE *f;
    uint32_t frames;
    int sample_rate;
} wav_writer;

/* streaming API */
int  wav_open(wav_writer *w, const char *path, int sample_rate);  /* 0 = ok */
int  wav_append(wav_writer *w, const int16_t *samples, size_t n);
int  wav_close(wav_writer *w);   /* patches the header sizes */

/* one-shot */
int  wav_write(const char *path, const int16_t *samples, size_t n, int sample_rate);

#endif
