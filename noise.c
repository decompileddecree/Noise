#if !defined(_WIN32) && !defined(__APPLE__)
#define _GNU_SOURCE
#endif

#include <SDL.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef _WIN32
    // --- WINDOWS ONLY ---
    #define _WIN32_WINNT 0x0A00
    #include <immintrin.h>
    #include <windows.h>
    #include <tbs.h>
    #include <bcrypt.h>
    #pragma comment(lib, "bcrypt.lib")
#elif defined(__APPLE__)
    // --- macOS / iOS ONLY ---
    #include <stdlib.h> // arc4random_buf is built natively into Apple's stdlib
#else
    // --- ANDROID / LINUX ONLY ---
    #include <unistd.h>
    #include <sys/syscall.h>
    #if __has_include(<sys/random.h>)
    #include <sys/random.h> // For getrandom()
    #endif
    #ifndef GRND_NONBLOCK
    #define GRND_NONBLOCK 1
    #endif
#endif

/* =====================================================================
 *  True random sources
 * ===================================================================== */

#ifdef _WIN32

static bool cpu_has_rdseed(void) {
    int regs[4];
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 18)) != 0;
}

static bool rdseed64(uint64_t *out) {
    unsigned long long v;
    for (int i = 0; i < 16; i++) {
        if (_rdseed64_step(&v)) { *out = (uint64_t)v; return true; }
        _mm_pause();
    }
    return false;
}

static bool tpm_random(uint64_t *out) {
    // Define what the function looks like in memory
    typedef TBS_RESULT (WINAPI *Tbsi_GetRandom_t)(UINT32, PBYTE, PUINT32);
    static Tbsi_GetRandom_t pTbsi_GetRandom = NULL;
    static bool init_attempted = false;

    // Ask Windows for the DLL directly (only runs once)
    if (!init_attempted) {
        HMODULE hTbs = LoadLibraryA("tbs.dll");
        if (hTbs) {
            pTbsi_GetRandom = (Tbsi_GetRandom_t)GetProcAddress(hTbs, "Tbsi_GetRandom");
        }
        init_attempted = true;
    }

    // If the TPM DLL or function doesn't exist on this PC, fail gracefully
    if (!pTbsi_GetRandom) return false; 

    UINT32 got = 0;
    // Notice the (PBYTE) cast here! This fixes the C4133 incompatible type warning.
    return pTbsi_GetRandom((UINT32)sizeof(*out), (PBYTE)out, &got) == TBS_SUCCESS
        && got == sizeof(*out);
}
#endif

/* =====================================================================
 *  RNG with reseed state machine (only touched by producer / main thread)
 * ===================================================================== */

typedef struct {
    uint64_t state[4];
    uint64_t countdown;
    uint64_t entropy;
    int      entropy_bits;
    bool     allow_slow_sources;
} RngCtx;


/* Forward declarations */
static void do_reseed(RngCtx *r);
static inline uint64_t xoshiro_step(uint64_t *s);
static inline uint64_t rng_next(RngCtx *r);

static inline uint64_t rng_next(RngCtx *r) {
    if (r->countdown == 0) do_reseed(r);
    r->countdown--;
    return xoshiro_step(r->state);
}

static bool pull_hw_seed(RngCtx *r, uint64_t *out) {
    static bool audio_printed = false;
    static bool video_printed = false;
    bool is_video = r->allow_slow_sources;
    bool *has_printed = is_video ? &video_printed : &audio_printed;
    const char* engine_name = "AV Engine";

    #ifdef _WIN32
        // --- WINDOWS HARDWARE ENGINE ---
        if (cpu_has_rdseed() && rdseed64(out)) {
            if (!*has_printed) { SDL_Log("[%s] Using Hardware RNG: Intel RDSEED", engine_name); *has_printed = true; }
            return true;
        }
        if (!r->allow_slow_sources) return false;

        if (tpm_random(out)) {
            if (!*has_printed) { SDL_Log("[%s] Using Hardware RNG: TPM Chip", engine_name); *has_printed = true; }
            return true;
        }
        if (BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)out, sizeof *out, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            if (!*has_printed) { SDL_Log("[%s] Using OS RNG: Windows BCrypt", engine_name); *has_printed = true; }
            return true;
        }
    #elif defined(__APPLE__)
        // --- macOS / iOS HARDWARE ENGINE ---
        // arc4random_buf hooks into Apple's Secure Enclave. 
        // It returns void because Apple guarantees it will never fail or block indefinitely.
        arc4random_buf(out, sizeof(*out));
        
        if (!*has_printed) { 
            SDL_Log("[%s] Using OS RNG: Apple arc4random_buf", engine_name); 
            *has_printed = true; 
        }
        return true;
    #else
        // --- ANDROID / LINUX HARDWARE ENGINE ---
        // getrandom reads directly from the Linux kernel's secure entropy pool (urandom)
        // GRND_NONBLOCK ensures the audio thread never freezes waiting for entropy
        #if defined(SYS_getrandom)
            if (syscall(SYS_getrandom, out, sizeof(*out), GRND_NONBLOCK) == sizeof(*out)) {
        #elif defined(__NR_getrandom)
            if (syscall(__NR_getrandom, out, sizeof(*out), GRND_NONBLOCK) == sizeof(*out)) {
        #else
            if (getrandom(out, sizeof(*out), GRND_NONBLOCK) == sizeof(*out)) {
        #endif
                if (!*has_printed) { SDL_Log("[%s] Using OS RNG: Linux getrandom()", engine_name); *has_printed = true; }
                return true;
            }
    #endif

    if (!*has_printed) { 
        SDL_Log("[%s] Using Fallback Math PRNG", engine_name); 
        *has_printed = true; 
    }
    return false;
}

static void refill_entropy(RngCtx *r) {
    uint64_t s;
    if (pull_hw_seed(r, &s)) {
        r->entropy = s; r->entropy_bits = 64; return;
    }
    /* Safely extract one output without triggering countdown loops */
    r->entropy = xoshiro_step(r->state);
    r->entropy_bits = 64;
}

static int next_true_digit(RngCtx *r) {
    for (;;) {
        if (r->entropy_bits < 8) refill_entropy(r);
        uint8_t b = (uint8_t)(r->entropy & 0xFF);
        r->entropy      >>= 8;
        r->entropy_bits -= 8;
        if (b < 250) return b % 10;
    }
}

static uint64_t next_period(RngCtx *r) {
    uint64_t p = 0;
    for (int i = 0; i < 6; i++) p = p * 10 + (uint64_t)next_true_digit(r);
    return (1ULL << 16) + p;
}

static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void do_reseed(RngCtx *r) {
    uint64_t seed;
    if (!pull_hw_seed(r, &seed))
        seed = xoshiro_step(r->state); // Safe manual step
    if (seed == 0) seed = 0x9E3779B97F4A7C15ULL;

    uint64_t sm = seed;
    r->state[0] = splitmix64(&sm);
    r->state[1] = splitmix64(&sm);
    r->state[2] = splitmix64(&sm);
    r->state[3] = splitmix64(&sm);

    r->countdown = next_period(r);
}

static void rng_init(RngCtx *r, bool allow_slow) {
    // Arrays can't be set with a single '=' in C, so we clear them manually
    r->state[0] = r->state[1] = r->state[2] = r->state[3] = 0;
    r->entropy = r->countdown = 0;
    r->entropy_bits = 0;
    r->allow_slow_sources = allow_slow;
    
    do_reseed(r);
}

// Helper function for the math
static inline uint64_t rotl(const uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

// Pure math step - does not trigger reseed loops
static inline uint64_t xoshiro_step(uint64_t *s) {
    const uint64_t result = rotl(s[0] + s[3], 23) + s[0];
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

/* =====================================================================
 *  Lock-free SPSC ring buffer
 * ===================================================================== */

typedef struct {
    int16_t      *buf;
    unsigned int  size;      /* power of two */
    unsigned int  mask;
    SDL_atomic_t  write_pos;
    SDL_atomic_t  read_pos;
} Ring;

static bool ring_init(Ring *r, unsigned int size_pow2) {
    r->buf = (int16_t *)SDL_malloc(size_pow2 * sizeof(int16_t));
    if (!r->buf) return false;
    r->size = size_pow2;
    r->mask = size_pow2 - 1;
    SDL_AtomicSet(&r->write_pos, 0);
    SDL_AtomicSet(&r->read_pos, 0);
    return true;
}

static void ring_destroy(Ring *r) {
    SDL_free(r->buf);
    r->buf = NULL;
}

/* =====================================================================
 *  Audio producer thread
 * ===================================================================== */

#define CHUNK_SAMPLES 4096   /* interleaved L/R samples = 2048 stereo frames */

typedef struct {
    Ring         ring;
    RngCtx       rng;
    SDL_atomic_t running;
} AudioProducer;

static int ProducerThread(void *data) {
    AudioProducer *p = (AudioProducer *)data;
    Ring *r = &p->ring;

    while (SDL_AtomicGet(&p->running)) {
        unsigned int w  = (unsigned int)SDL_AtomicGet(&r->write_pos);
        unsigned int rd = (unsigned int)SDL_AtomicGet(&r->read_pos);
        unsigned int free_slots = r->size - (w - rd);

        if (free_slots < CHUNK_SAMPLES) {
            SDL_Delay(1);
            continue;
        }

        int16_t chunk[CHUNK_SAMPLES];
        for (int i = 0; i < CHUNK_SAMPLES; i += 2) {
            chunk[i    ] = (int16_t)(rng_next(&p->rng) >> 48);  /* L */
            chunk[i + 1] = (int16_t)(rng_next(&p->rng) >> 48);  /* R */
        }

        for (int i = 0; i < CHUNK_SAMPLES; i++)
            r->buf[(w + (unsigned)i) & r->mask] = chunk[i];

        SDL_AtomicSet(&r->write_pos, (int)(w + CHUNK_SAMPLES));
    }
    return 0;
}

/* =====================================================================
 *  Audio callback (real-time thread, pure reader)
 * ===================================================================== */

static void AudioCallback(void *userdata, Uint8 *stream, int len) {
    Ring *r = (Ring *)userdata;
    int16_t *out = (int16_t *)stream;
    int samples = len / (int)sizeof(int16_t);

    unsigned int rd = (unsigned int)SDL_AtomicGet(&r->read_pos);
    unsigned int w  = (unsigned int)SDL_AtomicGet(&r->write_pos);
    unsigned int avail = w - rd;

    unsigned int take = avail < (unsigned)samples ? avail : (unsigned)samples;

    for (unsigned int i = 0; i < take; i++)
        out[i] = r->buf[(rd + i) & r->mask];

    if (take < (unsigned)samples)
        SDL_memset(out + take, 0,
                   ((unsigned)samples - take) * sizeof(int16_t));

    SDL_AtomicSet(&r->read_pos, (int)(rd + take));
}

/* =====================================================================
 *  Main
 * ===================================================================== */

int main(int argc, char* argv[]) {
    const int WIDTH = 800, HEIGHT = 600;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        SDL_Log("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* --- audio producer + ring ----------------------------------- */
    AudioProducer prod = {0};
    if (!ring_init(&prod.ring, 1u << 18)) {      /* 512 KB -> ~3 s stereo */
        SDL_Log("ring alloc failed\n"); return 1;
    }
    rng_init(&prod.rng, true);                  
    SDL_AtomicSet(&prod.running, 1);

    SDL_AudioSpec want = {0}, have;
    want.freq     = 44100;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;                           
    want.samples  = 2048;
    want.callback = AudioCallback;
    want.userdata = &prod.ring;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { SDL_Log("audio: %s\n", SDL_GetError()); return 1; }
    SDL_Log("audio: %d Hz, %d ch, %d samples/buffer\n",
            have.freq, have.channels, have.samples);

    SDL_Thread *producer = SDL_CreateThread(ProducerThread, "audio_prod", &prod);
    if (!producer) { SDL_Log("thread: %s\n", SDL_GetError()); return 1; }

    SDL_PauseAudioDevice(dev, 0);

    /* --- video side ---------------------------------------------- */
    RngCtx video_rng;
    rng_init(&video_rng, true);

    SDL_Window *win = SDL_CreateWindow("Hardware White Noise",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIDTH, HEIGHT, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { SDL_Log("window: %s\n", SDL_GetError()); return 1; }
    SDL_ShowCursor(SDL_DISABLE);   /* moved after the NULL check */

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { SDL_Log("renderer: %s\n", SDL_GetError()); return 1; }

    /* Track the texture's true pixel dimensions, not the window's. */
    int tex_w = 0, tex_h = 0;
    SDL_Texture *tex = NULL;

    Uint32 last_tap_time = 0; // Add this right before the loop
    bool running = true;
    
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            
            // 1. Keyboard and Android Back Button logic
            if (e.type == SDL_KEYDOWN) {
                // SDLK_AC_BACK captures the physical or virtual back navigation on Android
                if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_AC_BACK) {
                    running = false;
                }
                if (e.key.keysym.sym == SDLK_f) {
                    Uint32 flags = SDL_GetWindowFlags(win);
                    SDL_SetWindowFullscreen(win,
                        (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
                            ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
            }

            // 2. Touchscreen Double-Tap logic
            if (e.type == SDL_FINGERDOWN) {
                Uint32 current_time = SDL_GetTicks();
                // If two taps happen within 300 milliseconds, trigger fullscreen toggle
                if (current_time - last_tap_time < 300) {
                    Uint32 flags = SDL_GetWindowFlags(win);
                    SDL_SetWindowFullscreen(win,
                        (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
                            ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    last_tap_time = 0; // Reset to prevent a triple-tap from firing twice
                } else {
                    last_tap_time = current_time;
                }
            }
        }

        /* --- resize the texture to match the drawable ------------- */
        
        int ow, oh;
        if (SDL_GetRendererOutputSize(ren, &ow, &oh) != 0 || ow <= 0 || oh <= 0) {
            /* Renderer lost its target (minimized, etc). Skip the frame. */
            SDL_Delay(1);
            continue;
        }
        if (ow != tex_w || oh != tex_h) {
            SDL_Texture *new_tex = SDL_CreateTexture(ren,
                SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, ow, oh);
            if (new_tex) {
                SDL_DestroyTexture(tex);
                tex    = new_tex;
                tex_w  = ow;
                tex_h  = oh;
            }
            /* If allocation failed (e.g. an enormous fullscreen), fall
            through and keep the old texture — it will stretch, which
            is exactly the behavior we're trying to avoid, but at least
            the app keeps running instead of crashing. */
        }

        /* --- fill the whole buffer including row padding ---------- */
        void *pixels; int pitch;
        SDL_LockTexture(tex, NULL, &pixels, &pitch);

        size_t bytes = (size_t)pitch * (size_t)tex_h;
        size_t words = bytes / sizeof(uint64_t);
        uint64_t *p  = (uint64_t *)pixels;
        for (size_t i = 0; i < words; i++)
            p[i] = rng_next(&video_rng);

        /* Defensive: pitch is a multiple of 4 in practice for XRGB8888,
        but if it ever isn't a multiple of 8, cover the trailing bytes
        with one more PRNG output. */
        size_t tail = bytes - words * sizeof(uint64_t);
        if (tail) {
            uint64_t last = rng_next(&video_rng);
            SDL_memcpy((uint8_t *)pixels + words * sizeof(uint64_t), &last, tail);
        }

        SDL_UnlockTexture(tex);

        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }



    /* --- shutdown order matters ---------------------------------- */
    SDL_AtomicSet(&prod.running, 0);
    SDL_WaitThread(producer, NULL);
    SDL_CloseAudioDevice(dev);
    ring_destroy(&prod.ring);

    /* ... shutdown, unchanged except tex may be NULL if we never drew ... */
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}