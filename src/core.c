// Shared by the player screen and the background service: settings and
// playlist navigation (shuffle, repeat, next/previous).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "audio.h"
#include "playlist.h"

settings S = { .volume = 80, .vis_mode = 1, .cur_index = -1 };
char app_dir[512], data_dir[512], root_dir[512];
int current = -1;
static int *shuffle_order, shuffle_n;

void path_join(char *out, int n, const char *a, const char *b) { snprintf(out, n, "%s/%s", a, b); }

void settings_load(void)
{
    char p[1024];
    path_join(p, sizeof p, data_dir, "settings.ini");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "volume")) S.volume = atoi(v);
        else if (!strcmp(k, "balance")) S.balance = atoi(v);
        else if (!strcmp(k, "eq_on")) S.eq_on = atoi(v);
        else if (!strcmp(k, "eq_pre")) S.eq_pre = (float)atof(v);
        else if (!strcmp(k, "eq")) {
            const char *p2 = v;
            for (int i = 0; i < EQ_BANDS && *p2; i++) {
                S.eq[i] = strtof(p2, (char **)&p2);
                while (*p2 == ',' || *p2 == ' ') p2++;
            }
        }
        else if (!strcmp(k, "eq_preset")) snprintf(S.eq_preset, sizeof S.eq_preset, "%s", v);
        else if (!strcmp(k, "shuffle")) S.shuffle = atoi(v);
        else if (!strcmp(k, "repeat")) S.repeat = atoi(v);
        else if (!strcmp(k, "vis_mode")) S.vis_mode = atoi(v);
        else if (!strcmp(k, "vis_preset")) S.vis_preset = atoi(v);
        else if (!strcmp(k, "remaining")) S.remaining = atoi(v);
        else if (!strcmp(k, "skin")) snprintf(S.skin, sizeof S.skin, "%s", v);
        else if (!strcmp(k, "last_dir")) snprintf(S.last_dir, sizeof S.last_dir, "%s", v);
        else if (!strcmp(k, "cur_index")) S.cur_index = atoi(v);
        else if (!strcmp(k, "cur_pos_ms")) S.cur_pos_ms = atoi(v);
        else if (!strcmp(k, "game_music")) S.game_music = atoi(v);
        else if (!strcmp(k, "resume_playing")) S.resume_playing = atoi(v);
    }
    fclose(f);
}

void settings_save(void)
{
    char p[1024], tmp[1100];
    path_join(p, sizeof p, data_dir, "settings.ini");
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "volume=%d\nbalance=%d\neq_on=%d\neq_pre=%.1f\neq=", S.volume, S.balance, S.eq_on, S.eq_pre);
    for (int i = 0; i < EQ_BANDS; i++) fprintf(f, "%s%.1f", i ? "," : "", S.eq[i]);
    fprintf(f, "\neq_preset=%s\nshuffle=%d\nrepeat=%d\nvis_mode=%d\nvis_preset=%d\nremaining=%d\nskin=%s\nlast_dir=%s\ncur_index=%d\ncur_pos_ms=%d\ngame_music=%d\nresume_playing=%d\n",
            S.eq_preset, S.shuffle, S.repeat, S.vis_mode, S.vis_preset, S.remaining, S.skin, S.last_dir, S.cur_index, S.cur_pos_ms, S.game_music, S.resume_playing);
    fclose(f);
    rename(tmp, p);
}


void core_build_shuffle(void)
{
    int n = pl_count();
    free(shuffle_order);
    shuffle_order = malloc(sizeof(int) * (n ? n : 1));
    shuffle_n = n;
    for (int i = 0; i < n; i++) shuffle_order[i] = i;
    for (int i = n - 1; i > 0; i--) { int j = rand() % (i + 1); int t = shuffle_order[i]; shuffle_order[i] = shuffle_order[j]; shuffle_order[j] = t; }
    for (int i = 0; i < n; i++) if (shuffle_order[i] == current) { shuffle_order[i] = shuffle_order[0]; shuffle_order[0] = current; break; }
}

int core_play_index(int i, int start_ms, int paused)
{
    const char *p = pl_path(i);
    if (!p) return 0;
    current = i;
    audio_play_file(p, start_ms, paused);
    S.cur_index = i;
    if (S.shuffle && shuffle_n != pl_count()) core_build_shuffle();
    return 1;
}

int core_next_index(int dir)
{
    int n = pl_count();
    if (n == 0) return -1;
    if (S.shuffle) {
        if (shuffle_n != n) core_build_shuffle();
        int k = 0;
        for (int i = 0; i < n; i++) if (shuffle_order[i] == current) { k = i; break; }
        k += dir;
        if (k >= n) { if (!S.repeat) return -1; core_build_shuffle(); k = shuffle_order[0] == current && n > 1 ? 1 : 0; }
        if (k < 0) k = 0;
        return shuffle_order[k];
    }
    int k = current + dir;
    if (k >= n) return S.repeat ? 0 : -1;
    if (k < 0) return S.repeat ? n - 1 : 0;
    return k;
}

void core_skip(int dir)
{
    int k = core_next_index(dir);
    if (dir < 0 && audio_pos_ms() > 3000 && current >= 0) k = current;   // "previous" restarts first
    if (k < 0) return;
    core_play_index(k, 0, 0);
}

void core_toggle_pause(void)
{
    int st = audio_state();
    if (st == A_PLAYING) audio_set_paused(1);
    else if (st == A_PAUSED) audio_set_paused(0);
    else if (pl_count()) core_play_index(current >= 0 ? current : 0, 0, 0);
}

void core_apply_audio(void)
{
    audio_set_volume(S.volume);
    audio_set_balance(S.balance);
    audio_set_eq(S.eq_on, S.eq_pre, S.eq);
}

// handles a finished track; returns 1 if something new started
int core_track_finished(void)
{
    if (S.repeat == 2 && current >= 0) { core_play_index(current, 0, 0); return 1; }
    int k = core_next_index(1);
    if (k >= 0) { core_play_index(k, 0, 0); return 1; }
    S.cur_pos_ms = 0;
    return 0;
}
