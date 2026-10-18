/* Shooter mode: the guns' own CoD4 sounds (tools/cod4_extract.py writes
 * SHOOTER_GUN_DIR/<gun>/<name>.wav from the user's install).
 *
 * A separate XAudio2 engine from the guest's APU: each sound gets its own
 * source voice, so automatic fire overlaps the way it does in CoD4. Voices
 * that have finished are destroyed on the next call, from the game thread.
 * RECOMP_SHOOTER_VOL (0..1, default 0.7) sets their level. */
#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"

#define MAX_SOUNDS 64
#define MAX_VOICES 32

typedef struct {
    char key[96];
    WAVEFORMATEX fmt;
    BYTE *data;
    UINT32 len;
} Sound;

static IXAudio2 *s_xa;
static IXAudio2MasteringVoice *s_master;
static IXAudio2SourceVoice *s_voice[MAX_VOICES];
static Sound s_snd[MAX_SOUNDS];
static int s_nsnd, s_failed;
static float s_vol = 0.7f;

static int init(void)
{
    const char *e;
    if (s_xa)
        return 1;
    if (s_failed)
        return 0;
    s_failed = 1;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&s_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)))
        return 0;
    if (FAILED(IXAudio2_CreateMasteringVoice(s_xa, &s_master, XAUDIO2_DEFAULT_CHANNELS,
                                             XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL,
                                             AudioCategory_GameEffects))) {
        IXAudio2_Release(s_xa);
        s_xa = NULL;
        return 0;
    }
    if ((e = getenv("RECOMP_SHOOTER_VOL")) != NULL)
        s_vol = (float)atof(e);
    s_failed = 0;
    return 1;
}

/* A PCM .wav (RIFF, fmt, data) into memory, or NULL. */
static Sound *load(const char *gun, const char *name)
{
    char key[96], path[MAX_PATH];
    BYTE *buf = NULL;
    Sound *s;
    long n, p;
    FILE *f;
    int i;

    snprintf(key, sizeof key, "%s/%s", gun, name);
    for (i = 0; i < s_nsnd; i++)
        if (!strcmp(s_snd[i].key, key))
            return s_snd[i].data ? &s_snd[i] : NULL;
    if (s_nsnd == MAX_SOUNDS)
        return NULL;
    s = &s_snd[s_nsnd++];             /* remembered even when missing */
    memset(s, 0, sizeof *s);
    snprintf(s->key, sizeof s->key, "%s", key);
    snprintf(path, sizeof path, SHOOTER_GUN_DIR "/%s/%s.wav", gun, name);
    if (!(f = fopen(path, "rb")))
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (BYTE *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n || n < 12 || memcmp(buf, "RIFF", 4)) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    for (p = 12; p + 8 <= n;) {
        UINT32 sz;
        memcpy(&sz, buf + p + 4, 4);
        if (!memcmp(buf + p, "fmt ", 4) && sz >= 16)
            memcpy(&s->fmt, buf + p + 8, 16);
        else if (!memcmp(buf + p, "data", 4) && p + 8 + (long)sz <= n) {
            s->data = buf + p + 8;
            s->len = sz;
        }
        p += 8 + ((sz + 1) & ~1u);
    }
    if (!s->data || s->fmt.wFormatTag != WAVE_FORMAT_PCM) {
        s->data = NULL;
        free(buf);
        return NULL;
    }
    return s;
}

int snd_play(const char *gun, const char *name)
{
    XAUDIO2_BUFFER xb;
    XAUDIO2_VOICE_STATE st;
    Sound *s;
    int i, slot = -1;

    if (!gun || !*gun || !init() || !(s = load(gun, name)))
        return 0;
    for (i = 0; i < MAX_VOICES; i++) {
        if (s_voice[i]) {
            IXAudio2SourceVoice_GetState(s_voice[i], &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued)
                continue;
            IXAudio2SourceVoice_DestroyVoice(s_voice[i]);
            s_voice[i] = NULL;
        }
        if (slot < 0)
            slot = i;
    }
    if (slot < 0)
        return 1;
    if (FAILED(IXAudio2_CreateSourceVoice(s_xa, &s_voice[slot], &s->fmt, 0,
                                          XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)))
        return 1;
    memset(&xb, 0, sizeof xb);
    xb.Flags = XAUDIO2_END_OF_STREAM;
    xb.AudioBytes = s->len;
    xb.pAudioData = s->data;
    IXAudio2SourceVoice_SetVolume(s_voice[slot], s_vol, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_SubmitSourceBuffer(s_voice[slot], &xb, NULL);
    IXAudio2SourceVoice_Start(s_voice[slot], 0, XAUDIO2_COMMIT_NOW);
    return 1;
}

/* -- Raw PCM (skate mode: sounds made in code) ------------------------------ */

static WAVEFORMATEX pcm_fmt(void)
{
    WAVEFORMATEX f;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = SND_PCM_RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = SND_PCM_RATE * 2;
    return f;
}

/* A one-shot of 16-bit mono at SND_PCM_RATE; pcm must outlive the sound. */
void snd_pcm_play(const short *pcm, int n, float vol, float pitch)
{
    XAUDIO2_BUFFER xb;
    XAUDIO2_VOICE_STATE st;
    WAVEFORMATEX f = pcm_fmt();
    int i, slot = -1;
    if (!pcm || n <= 0 || !init())
        return;
    for (i = 0; i < MAX_VOICES; i++) {
        if (s_voice[i]) {
            IXAudio2SourceVoice_GetState(s_voice[i], &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued)
                continue;
            IXAudio2SourceVoice_DestroyVoice(s_voice[i]);
            s_voice[i] = NULL;
        }
        if (slot < 0)
            slot = i;
    }
    if (slot < 0 || FAILED(IXAudio2_CreateSourceVoice(s_xa, &s_voice[slot], &f, 0, 4.0f,
                                                      NULL, NULL, NULL)))
        return;
    memset(&xb, 0, sizeof xb);
    xb.Flags = XAUDIO2_END_OF_STREAM;
    xb.AudioBytes = (UINT32)n * 2;
    xb.pAudioData = (const BYTE *)pcm;
    IXAudio2SourceVoice_SetVolume(s_voice[slot], vol, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_SetFrequencyRatio(s_voice[slot], pitch, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_SubmitSourceBuffer(s_voice[slot], &xb, NULL);
    IXAudio2SourceVoice_Start(s_voice[slot], 0, XAUDIO2_COMMIT_NOW);
}

/* A looping voice per buffer (a few): started on the first call, then only
 * its level and pitch change; a level of 0 keeps it running silently. */
void snd_pcm_loop(const short *pcm, int n, float vol, float pitch)
{
    static struct { const short *pcm; IXAudio2SourceVoice *v; } loops[4];
    IXAudio2SourceVoice *v;
    int i;
    if (!pcm || n <= 0 || !init())
        return;
    for (i = 0; i < 3 && loops[i].pcm && loops[i].pcm != pcm; i++)
        ;
    loops[i].pcm = pcm;
    v = loops[i].v;
    if (!v) {
        XAUDIO2_BUFFER xb;
        WAVEFORMATEX f = pcm_fmt();
        if (FAILED(IXAudio2_CreateSourceVoice(s_xa, &v, &f, 0, 4.0f, NULL, NULL, NULL)))
            return;
        memset(&xb, 0, sizeof xb);
        xb.AudioBytes = (UINT32)n * 2;
        xb.pAudioData = (const BYTE *)pcm;
        xb.LoopCount = XAUDIO2_LOOP_INFINITE;
        IXAudio2SourceVoice_SubmitSourceBuffer(v, &xb, NULL);
        IXAudio2SourceVoice_Start(v, 0, XAUDIO2_COMMIT_NOW);
        loops[i].v = v;
    }
    IXAudio2SourceVoice_SetVolume(v, vol, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_SetFrequencyRatio(v, pitch < 0.05f ? 0.05f : pitch, XAUDIO2_COMMIT_NOW);
}
