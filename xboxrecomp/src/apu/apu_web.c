/**
 * Web Audio output backend (Emscripten)
 *
 * The xa2_* contract of apu_xaudio2.h in the browser. SDL's audio cannot be
 * opened from a worker, and the APU frame thread is one, so this backend
 * keeps its own queue: a ring of 48 kHz stereo s16 frames in wasm memory,
 * which the frame thread fills and a ScriptProcessorNode on the page's main
 * thread drains (the memory is shared, so the node reads it directly). The
 * indices are free-running frame counts; queued = written - read.
 *
 * The AudioContext starts suspended (autoplay policy) and resumes on the
 * page's first click or key press.
 *
 * ponytail: ScriptProcessorNode, deprecated but everywhere and simple; move
 * to an AudioWorklet if main-thread jank shows up as crackle.
 */
#include <stdint.h>
#include <stdio.h>
#include "apu_xaudio2.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/threading.h>

#define RING_FRAMES 16384u               /* power of two: 341 ms */
#define DEVICE_FRAMES 1024               /* one ScriptProcessor pull: 21 ms */
#define NUM_BUFS 4

static int16_t s_ring[RING_FRAMES * 2];
static volatile uint32_t s_written, s_read, s_underruns;
static int s_active;

/* Runs on the page's main thread: the AudioContext lives there. */
EM_JS(void, web_audio_start, (int ring_p, int frames, int w_p, int r_p, int u_p, int dev), {
    const ring = ring_p >> 1;
    const mask = frames - 1;
    const wIdx = w_p >> 2;
    const rIdx = r_p >> 2;
    const uIdx = u_p >> 2;
    const ctx = new AudioContext({ sampleRate: 48000 });
    const node = ctx.createScriptProcessor(dev, 0, 2);
    node.onaudioprocess = (e) => {
        const L = e.outputBuffer.getChannelData(0);
        const R = e.outputBuffer.getChannelData(1);
        let r = Atomics.load(HEAPU32, rIdx);
        const w = Atomics.load(HEAPU32, wIdx);
        for (let i = 0; i < L.length; i++) {
            if (r === w) {
                L[i] = 0;
                R[i] = 0;
                if (i === 0) Atomics.add(HEAPU32, uIdx, 1);
                continue;
            }
            const f = (r & mask) * 2;
            L[i] = HEAP16[ring + f] / 32768;
            R[i] = HEAP16[ring + f + 1] / 32768;
            r = (r + 1) >>> 0;
        }
        Atomics.store(HEAPU32, rIdx, r);
    };
    node.connect(ctx.destination);
    const resume = () => { if (ctx.state !== 'running') ctx.resume(); };
    for (const t of ['click', 'keydown', 'pointerdown']) addEventListener(t, resume, true);
    resume();
});

int xa2_init(void)
{
    if (s_active)
        return 1;
    emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VIIIIII, web_audio_start,
        (int)(intptr_t)s_ring, (int)RING_FRAMES, (int)(intptr_t)&s_written,
        (int)(intptr_t)&s_read, (int)(intptr_t)&s_underruns, DEVICE_FRAMES);
    s_active = 1;
    fprintf(stderr, "[WEBAUDIO] 48000 Hz stereo, %d-frame pulls, %u-frame ring\n",
            DEVICE_FRAMES, RING_FRAMES);
    return 1;
}

void xa2_shutdown(void) { s_active = 0; }
int xa2_is_active(void) { return s_active; }

/* One EP period in. A full ring drops the oldest-unplayed newest data: the
 * pacer keeps the queue near NUM_BUFS * DEVICE_FRAMES, far below the ring. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    uint32_t w = __atomic_load_n(&s_written, __ATOMIC_ACQUIRE);
    uint32_t r = __atomic_load_n(&s_read, __ATOMIC_ACQUIRE);
    int i;

    if (!s_active || num_samples <= 0 || w - r + (uint32_t)num_samples > RING_FRAMES)
        return 0;
    for (i = 0; i < num_samples; i++) {
        uint32_t f = ((w + (uint32_t)i) & (RING_FRAMES - 1)) * 2;
        s_ring[f] = samples[2 * i];
        s_ring[f + 1] = samples[2 * i + 1];
    }
    __atomic_store_n(&s_written, w + (uint32_t)num_samples, __ATOMIC_RELEASE);
    return 1;
}

int xa2_get_buffer_size(void) { return DEVICE_FRAMES; }
int xa2_get_buffer_count(void) { return NUM_BUFS; }

int xa2_queued_samples(void)
{
    return (int)(__atomic_load_n(&s_written, __ATOMIC_ACQUIRE) - __atomic_load_n(&s_read, __ATOMIC_ACQUIRE));
}

void xa2_expect_drain(void) {}
unsigned xa2_underruns(void) { return s_underruns; }
uint64_t xa2_samples_played(void) { return __atomic_load_n(&s_read, __ATOMIC_ACQUIRE); }

#endif /* __EMSCRIPTEN__ */
