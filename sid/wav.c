/* wav.c - minimal 16-bit mono PCM WAV writer (C99) */
#include "wav.h"

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

static int write_header(FILE *f, int rate, uint32_t frames)
{
    unsigned char h[44];
    uint32_t data = frames * 2u;
    h[0] = 'R'; h[1] = 'I'; h[2] = 'F'; h[3] = 'F';
    put_u32(h + 4, 36u + data);
    h[8] = 'W'; h[9] = 'A'; h[10] = 'V'; h[11] = 'E';
    h[12] = 'f'; h[13] = 'm'; h[14] = 't'; h[15] = ' ';
    put_u32(h + 16, 16);          /* fmt chunk size  */
    put_u16(h + 20, 1);           /* PCM             */
    put_u16(h + 22, 1);           /* mono            */
    put_u32(h + 24, (uint32_t)rate);
    put_u32(h + 28, (uint32_t)rate * 2u);
    put_u16(h + 32, 2);           /* block align     */
    put_u16(h + 34, 16);          /* bits per sample */
    h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
    put_u32(h + 40, data);
    return fwrite(h, 1, 44, f) == 44 ? 0 : -1;
}

int wav_open(wav_writer *w, const char *path, int sample_rate)
{
    w->f = fopen(path, "wb");
    w->frames = 0;
    w->sample_rate = sample_rate;
    if (!w->f) return -1;
    return write_header(w->f, sample_rate, 0);
}

int wav_append(wav_writer *w, const int16_t *s, size_t n)
{
    unsigned char buf[2048];
    while (n > 0) {
        size_t k = n > 1024 ? 1024 : n;
        for (size_t i = 0; i < k; i++) put_u16(buf + 2 * i, (uint16_t)s[i]);
        if (fwrite(buf, 2, k, w->f) != k) return -1;
        w->frames += (uint32_t)k;
        s += k; n -= k;
    }
    return 0;
}

int wav_close(wav_writer *w)
{
    int r = 0;
    if (!w->f) return -1;
    if (fseek(w->f, 0, SEEK_SET) != 0 || write_header(w->f, w->sample_rate, w->frames) != 0) r = -1;
    if (fclose(w->f) != 0) r = -1;
    w->f = NULL;
    return r;
}

int wav_write(const char *path, const int16_t *samples, size_t n, int sample_rate)
{
    wav_writer w;
    if (wav_open(&w, path, sample_rate) != 0) return -1;
    int r = wav_append(&w, samples, n);
    if (wav_close(&w) != 0) r = -1;
    return r;
}
