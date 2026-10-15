// Writes a MiniAmp music feed (bgm_shm) holding a steady 1 kHz tone.
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "bgm_shm.h"
int main(void)
{
    int fd = open(BGM_SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, sizeof(bgm_shm)) != 0) return 1;
    bgm_shm *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memset(s, 0, sizeof *s);
    for (int i = 0; i < BGM_RING; i++) {
        int16_t v = (int16_t)(12000 * sin(2 * M_PI * 1000.0 * i / BGM_RATE));
        s->ring[i * 2] = s->ring[i * 2 + 1] = v;
    }
    s->volume = 256;
    s->wpos = 0x7fffffff & ~(BGM_RING - 1);  // pretend it's far ahead; ring is a loop of the tone
    s->rpos = s->wpos - (BGM_RING - 16);
    s->magic = BGM_MAGIC;
    return 0;
}
