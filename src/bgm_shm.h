// Shared memory between the background music service and the sound hook
// that mixes its music into whatever program is playing audio.
#ifndef BGM_SHM_H
#define BGM_SHM_H
#include <stdint.h>

#define BGM_SHM_PATH "/tmp/miniamp_bgm.shm"
#define BGM_MAGIC 0x4d414247u   // "GBAM"
#define BGM_RING 16384          // stereo frames (power of two)
#define BGM_RATE 48000

typedef struct {
    uint32_t magic, version;
    volatile uint32_t wpos;       // frames written by the service
    volatile uint32_t rpos;       // frames mixed by the hook
    volatile int32_t owner_pid;   // process currently mixing the music
    volatile uint32_t owner_ms;   // last time (ms, CLOCK_MONOTONIC) the owner mixed
    volatile int32_t volume;      // 0..256
    volatile int32_t paused;      // 1: hook mixes nothing
    volatile uint32_t underruns;  // hook wanted music but the ring was empty
    volatile uint32_t mixed_writes;
    char owner_name[32];
    int16_t ring[BGM_RING * 2];
} bgm_shm;

#endif
