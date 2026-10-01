#ifndef AUDIO_H
#define AUDIO_H

enum { A_STOPPED, A_PLAYING, A_PAUSED };

#define OUT_RATE 44100
#define EQ_BANDS 10
extern const int EQ_FREQ[EQ_BANDS];

int audio_init(void);                 // returns 1 if a real output device is in use
void audio_quit(void);

// Commands are handled by the audio thread; they return immediately.
void audio_play_file(const char *path, int start_ms, int paused);
void audio_set_paused(int paused);
void audio_stop(void);
void audio_seek(int ms);

int audio_state(void);
int audio_pos_ms(void);
int audio_len_ms(void);               // 0 if unknown
int audio_take_finished(void);        // 1 once after a track plays to its end
int audio_take_error(void);           // 1 once after a file failed to open
void audio_stream_info(int *rate, int *channels, int *kbps);

void audio_set_volume(int v);         // 0..100
void audio_set_balance(int b);        // -100..100
void audio_set_eq(int on, float preamp_db, const float bands_db[EQ_BANDS]);

// copies the most recent n mono samples (as heard), returns n or 0
int audio_get_wave(float *out, int n);

#endif
