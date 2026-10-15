// Stand-in for the vendor libmi_ao.so: records what reaches the "chip".
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int eSamplerate, eBitwidth, eWorkmode, eSoundmode; uint32_t a, b, c, d; } attr_t;
typedef struct { int eBitwidth, eSoundmode; void *apVirAddr[16]; uint64_t ts; uint32_t seq, u32Len, pool[2]; void *src[16]; uint32_t srclen; } frame_t;
int16_t *chip_out;
size_t chip_len, chip_cap;
int MI_AO_SetPubAttr(int dev, attr_t *a) { (void)dev; (void)a; return 0; }
int MI_AO_Disable(int dev) { (void)dev; return 0; }
int MI_AO_SendFrame(int dev, int chn, frame_t *f, int ms)
{
    (void)dev; (void)chn; (void)ms;
    size_t n = f->u32Len / 2;
    if (chip_len + n > chip_cap) { chip_cap = (chip_len + n) * 2; chip_out = realloc(chip_out, chip_cap * 2); }
    memcpy(chip_out + chip_len, f->apVirAddr[0], n * 2);
    chip_len += n;
    return 0;
}
