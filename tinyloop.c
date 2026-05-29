#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <tinyalsa/asoundlib.h>
#include "ringbuf.h"

static volatile int running = 1;

static void sigint_handler(int s)
{
    (void)s;
    running = 0;
}

struct iec958_enc {
    unsigned int counter;  /* frame counter 0-191 */
    unsigned char status[24];
    unsigned char preamble[3]; /* Z/X/Y */
};

static void iec958_enc_init(struct iec958_enc *e, unsigned int rate)
{
    e->counter = 0;
    memset(e->status, 0, sizeof(e->status));
    /* status[0]: consumer, audio, not-copyright, no emphasis */
    e->status[0] = 0x0C;
    /* status[1]: general category */
    e->status[1] = 0x00;
    /* status[2]: source/channel unspecified */

    /* status[3]: sample rate in upper nibble, clock accuracy level II (1000 ppm) */
    e->status[3] = 0x00;

    /* status[4]: 20/16-bit word length */
    e->status[4] = 0x08;

    {
        static const struct { unsigned int rate; unsigned char val; } fs_tab[] = {
            { 32000,  0x30 }, { 44100,  0x00 }, { 48000,  0x20 },
            { 88200,  0x80 }, { 96000,  0xA0 }, { 176400, 0xC0 },
            { 192000, 0xE0 }, { 22050,  0x40 }, { 24000,  0x60 },
            { 128000, 0x80 }, { 384000, 0x50 }, { 768000, 0x90 },
            { 705600, 0xD0 },
        };
        for (size_t i = 0; i < sizeof(fs_tab)/sizeof(fs_tab[0]); i++)
            if (fs_tab[i].rate == rate) {
                e->status[3] |= fs_tab[i].val;
                break;
            }
    }

    e->preamble[0] = 0x08; /* Z (block start) */
    e->preamble[1] = 0x02; /* X (even subframe) */
    e->preamble[2] = 0x04; /* Y (odd subframe) */
}

static uint32_t iec958_subframe(struct iec958_enc *e, int16_t sample, int channel)
{
    /* left-justify 16-bit sample in 24-bit audio field (bits 12-27) */
    uint32_t sf = (uint32_t)(uint16_t)sample << 12;

    /* bit 30: channel status (one bit per subframe, 192 bits total) */
    {
        unsigned int byte = e->counter >> 3;
        unsigned int mask = 1 << (e->counter - (byte << 3));
        if (e->status[byte] & mask)
            sf |= 0x40000000;
    }

    /* even parity over bits 4-30 */
    {
        uint32_t p = sf & 0x7FFFFFF0;
        p ^= p >> 16;
        p ^= p >> 8;
        p ^= p >> 4;
        p ^= p >> 2;
        p ^= p >> 1;
        sf |= (p & 1) << 31;
    }

    /* preamble in bits 0-3 */
    if (channel)
        sf |= e->preamble[2];  /* Y: odd subframe */
    else if (!e->counter)
        sf |= e->preamble[0];  /* Z: block start */
    else
        sf |= e->preamble[1];  /* X: even subframe */

    return sf;
}

struct state {
    const char *in_spec;
    const char *out_spec;
    unsigned int in_card, in_dev;
    unsigned int out_card, out_dev;
    struct pcm_config cfg;
    struct pcm *pcm_in;
    struct pcm *pcm_out;
    struct ringbuf *rb;
    unsigned int bpf; /* bytes per frame (S16_LE) */
    int iec958;
    enum pcm_format force_format;
};

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -i card:device -o card:device [options]\n"
        "Options:\n"
        "  -i card:device   capture device (required)\n"
        "  -o card:device   playback device (required)\n"
        "                   Run alsalist to discover card:device pairs.\n"
        "  -f format        force output format (S16_LE or IEC958)\n"
        "                   default: auto-detect\n"
        "  -r rate          sample rate (default: 48000)\n"
        "  -c channels      channels (default: 2)\n"
        "  -p period_frames frames per period (default: 512)\n"
        "  -n periods       number of periods (default: 8)\n"
        "  -h               show this help\n", prog);
}

static int parse_pair(const char *s, unsigned int *card, unsigned int *dev)
{
    char *end = NULL;
    unsigned long c = strtoul(s, &end, 10);
    if (!end || *end != ':')
        return -1;
    unsigned long d = strtoul(end + 1, NULL, 10);
    *card = (unsigned int)c;
    *dev  = (unsigned int)d;
    return 0;
}

static int open_pcm_in(struct state *st)
{
    if (st->pcm_in)
        pcm_close(st->pcm_in);
    struct pcm_config cfg = st->cfg;
    cfg.start_threshold = cfg.period_size / 2;
    st->pcm_in = pcm_open(st->in_card, st->in_dev, PCM_IN, &cfg);
    if (!st->pcm_in || !pcm_is_ready(st->pcm_in)) {
        fprintf(stderr, "capture %s: %s\n", st->in_spec,
                st->pcm_in ? pcm_get_error(st->pcm_in) : "pcm_open failed");
        return -1;
    }
    return 0;
}

static int open_pcm_out(struct state *st)
{
    if (st->pcm_out)
        pcm_close(st->pcm_out);
    struct pcm_config cfg = st->cfg;
    cfg.start_threshold = cfg.period_size;

    if (st->force_format != PCM_FORMAT_INVALID) {
        cfg.format = st->force_format;
        st->pcm_out = pcm_open(st->out_card, st->out_dev, PCM_OUT, &cfg);
        if (!st->pcm_out || !pcm_is_ready(st->pcm_out)) {
            fprintf(stderr, "playback %s: %s\n", st->out_spec,
                    st->pcm_out ? pcm_get_error(st->pcm_out) : "pcm_open failed");
            return -1;
        }
        st->iec958 = (cfg.format == PCM_FORMAT_IEC958_SUBFRAME_LE);
        return 0;
    }

    cfg.format = PCM_FORMAT_S16_LE;
    st->pcm_out = pcm_open(st->out_card, st->out_dev, PCM_OUT, &cfg);
    if (st->pcm_out && pcm_is_ready(st->pcm_out)) {
        st->iec958 = 0;
        return 0;
    }

    cfg.format = PCM_FORMAT_IEC958_SUBFRAME_LE;
    st->pcm_out = pcm_open(st->out_card, st->out_dev, PCM_OUT, &cfg);
    if (st->pcm_out && pcm_is_ready(st->pcm_out)) {
        st->iec958 = 1;
        fprintf(stderr, "playback %s: using IEC958_SUBFRAME_LE\n", st->out_spec);
        return 0;
    }

    fprintf(stderr, "playback %s: %s\n", st->out_spec,
            st->pcm_out ? pcm_get_error(st->pcm_out) : "pcm_open failed");
    return -1;
}

static void *capture_thread_fn(void *arg)
{
    struct state *st = arg;
    unsigned int period_frames = st->cfg.period_size;
    size_t period_bytes = period_frames * st->bpf;
    void *buf = malloc(period_bytes);
    if (!buf) {
        fprintf(stderr, "capture: malloc failed\n");
        return NULL;
    }

    while (running) {
        int n = pcm_readi(st->pcm_in, buf, period_frames);
        if (n == -EINTR)
            continue;
        if (n < 0) {
            fprintf(stderr, "capture: %s\n", pcm_get_error(st->pcm_in));
            if (!running) break;
            if (pcm_prepare(st->pcm_in) == 0) {
                pcm_start(st->pcm_in);
                continue;
            }
            open_pcm_in(st);
            continue;
        }
        if (n == 0)
            continue;

        size_t bytes = pcm_frames_to_bytes(st->pcm_in, (unsigned int)n);
        size_t written = ringbuf_write(st->rb, buf, bytes);
        if (written < bytes) {
            /* ring buffer full; dropping excess */
        }
    }

    free(buf);
    return NULL;
}

static void *playback_thread_fn(void *arg)
{
    struct state *st = arg;
    unsigned int period_frames = st->cfg.period_size;
    size_t period_bytes = period_frames * st->bpf;  /* S16_LE */
    size_t iec_bytes = period_frames * st->cfg.channels * 4;  /* IEC958 */
    void *buf = malloc(iec_bytes > period_bytes ? iec_bytes : period_bytes);
    void *iec_buf = st->iec958 ? malloc(iec_bytes) : NULL;
    if (!buf || (st->iec958 && !iec_buf)) {
        fprintf(stderr, "playback: malloc failed\n");
        free(buf);
        free(iec_buf);
        return NULL;
    }

    struct iec958_enc enc_st;
    iec958_enc_init(&enc_st, st->cfg.rate);

    /* Prime the DMA buffer: write a buffer-full of (mostly) silent frames.
     * The first 256 frames include a soft-start ramp (sample 0→1→0) at
     * —90 dBFS to settle the HDMI sink's DAC, followed by true silence.
     * This also gives the Audio InfoFrame time to be sent (scheduled during
     * prepare for the next VSYNC) before real audio arrives. */
    {
        unsigned int prime_frames = st->cfg.period_size * st->cfg.period_count;
        unsigned int soft_len = 256;
        if (soft_len > prime_frames) soft_len = prime_frames;
        void *prime_buf;
        size_t prime_bytes;
        if (st->iec958) {
            prime_bytes = prime_frames * st->cfg.channels * 4;
            prime_buf = malloc(prime_bytes);
            if (prime_buf) {
                uint32_t *dst = prime_buf;
                for (unsigned int i = 0; i < prime_frames; i++) {
                    int16_t s = 0;
                    if (i * 2 < soft_len)
                        s = (int16_t)((int32_t)i * 2 / (int32_t)soft_len);
                    else if (i < soft_len)
                        s = (int16_t)((int32_t)(soft_len - i) * 2 / (int32_t)soft_len);
                    for (unsigned int c = 0; c < st->cfg.channels; c++)
                        *dst++ = iec958_subframe(&enc_st, s, c);
                }
                int pw = pcm_writei(st->pcm_out, prime_buf, prime_frames);
                (void)pw;
                free(prime_buf);
            }
        } else {
            prime_bytes = prime_frames * st->bpf;
            prime_buf = malloc(prime_bytes);
            if (prime_buf) {
                int16_t *dst = prime_buf;
                for (unsigned int i = 0; i < prime_frames; i++) {
                    int16_t s = 0;
                    if (i * 2 < soft_len)
                        s = (int16_t)((int32_t)i * 2 / (int32_t)soft_len);
                    else if (i < soft_len)
                        s = (int16_t)((int32_t)(soft_len - i) * 2 / (int32_t)soft_len);
                    for (unsigned int c = 0; c < st->cfg.channels; c++)
                        *dst++ = s;
                }
                int pw = pcm_writei(st->pcm_out, prime_buf, prime_frames);
                (void)pw;
                free(prime_buf);
            }
        }
    }

    uint32_t fade_pos = 0;
    uint32_t fade_len = 4096;

    while (running) {
        size_t got = ringbuf_read(st->rb, buf, period_bytes);

        if (got == 0 && fade_pos < fade_len) {
            /* Ringbuffer empty during fade-in: write silence to avoid underrun */
            if (st->iec958) {
                uint32_t *silence = buf;
                for (unsigned int i = 0; i < period_frames; i++)
                    for (unsigned int c = 0; c < st->cfg.channels; c++)
                        silence[i * st->cfg.channels + c] =
                            iec958_subframe(&enc_st, 0, c);
                int pw = pcm_writei(st->pcm_out, silence, period_frames);
                (void)pw;
            } else {
                memset(buf, 0, period_bytes);
                int pw = pcm_writei(st->pcm_out, buf, period_frames);
                (void)pw;
            }
            continue;
        }

        if (got == 0) {
            usleep(2000);
            continue;
        }

        unsigned int frames = (unsigned int)(got / st->bpf);
        const void *write_buf = buf;

        /* Fade-in to smooth the DAC idle→active transition */
        if (fade_pos < fade_len && frames > 0) {
            uint32_t end = fade_pos + frames;
            if (end > fade_len) end = fade_len;
            for (uint32_t i = fade_pos; i < end; i++) {
                int32_t gain_num = (int32_t)i;
                for (unsigned int c = 0; c < st->cfg.channels; c++) {
                    int16_t *s = &((int16_t *)buf)[(i - fade_pos) * st->cfg.channels + c];
                    *s = (int16_t)((int32_t)*s * gain_num / (int32_t)fade_len);
                }
            }
            fade_pos = end;
        }

        if (st->iec958 && frames > 0) {
            uint32_t *dst = iec_buf;
            int16_t *src = buf;
            unsigned int ch = st->cfg.channels;
            for (unsigned int i = 0; i < frames; i++) {
                for (unsigned int c = 0; c < ch; c++)
                    dst[i * ch + c] = iec958_subframe(&enc_st, src[i * ch + c], c);
                enc_st.counter++;
                enc_st.counter %= 192;
            }
            write_buf = iec_buf;
        }

        int w = pcm_writei(st->pcm_out, write_buf, frames);
        if (w == -EINTR)
            continue;
        if (w < 0) {
            fprintf(stderr, "playback: %s\n", pcm_get_error(st->pcm_out));
            if (!running) break;
            if (pcm_prepare(st->pcm_out) == 0) {
                pcm_start(st->pcm_out);
                continue;
            }
            open_pcm_out(st);
            continue;
        }
    }

    free(iec_buf);
    free(buf);
    return NULL;
}

int main(int argc, char **argv)
{
    struct state st;
    memset(&st, 0, sizeof(st));

    unsigned int rate = 48000;
    unsigned int channels = 2;
    unsigned int period_size = 512;
    unsigned int period_count = 8;
    int got_i = 0, got_o = 0;
    int opt;

    st.force_format = PCM_FORMAT_INVALID;

    while ((opt = getopt(argc, argv, "i:o:r:c:p:n:f:h")) != -1) {
        switch (opt) {
        case 'i': if (parse_pair(optarg, &st.in_card, &st.in_dev) < 0) {
                      fprintf(stderr, "Invalid capture spec '%s' (use card:device)\n", optarg);
                      return 1;
                  } st.in_spec = optarg; got_i = 1; break;
        case 'o': if (parse_pair(optarg, &st.out_card, &st.out_dev) < 0) {
                      fprintf(stderr, "Invalid playback spec '%s' (use card:device)\n", optarg);
                      return 1;
                  } st.out_spec = optarg; got_o = 1; break;
        case 'r': rate = (unsigned int)atoi(optarg); break;
        case 'c': channels = (unsigned int)atoi(optarg); break;
        case 'p': period_size = (unsigned int)atoi(optarg); break;
        case 'n': period_count = (unsigned int)atoi(optarg); break;
        case 'f': if (strcmp(optarg, "S16_LE") == 0)
                      st.force_format = PCM_FORMAT_S16_LE;
                  else if (strcmp(optarg, "IEC958") == 0)
                      st.force_format = PCM_FORMAT_IEC958_SUBFRAME_LE;
                  else {
                      fprintf(stderr, "Invalid format '%s' (use S16_LE or IEC958)\n", optarg);
                      return 1;
                  }
                  break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (!got_i || !got_o) {
        print_usage(argv[0]);
        return 1;
    }

    st.cfg.channels = channels;
    st.cfg.rate = rate;
    st.cfg.format = PCM_FORMAT_S16_LE;
    st.cfg.period_size = period_size;
    st.cfg.period_count = period_count;
    st.cfg.start_threshold = 0;
    st.cfg.stop_threshold = 0;
    st.cfg.silence_threshold = 0;
    st.cfg.silence_size = 0;
    st.cfg.avail_min = 0;

    st.bpf = channels * 2; /* S16_LE = 2 bytes per sample */

    if (open_pcm_in(&st) < 0) return 1;
    if (open_pcm_out(&st) < 0) { pcm_close(st.pcm_in); return 1; }

    unsigned int ringbuf_periods = 4;
    st.rb = ringbuf_create(period_size * ringbuf_periods * st.bpf);
    if (!st.rb) {
        fprintf(stderr, "ringbuf_create failed\n");
        pcm_close(st.pcm_in);
        pcm_close(st.pcm_out);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("tinyloop: %u:%u -> %u:%u  %u Hz %u ch %u fps %u periods  buffer=%u rb=%zu  fmt=%s\n",
           st.in_card, st.in_dev, st.out_card, st.out_dev,
           rate, channels, period_size, period_count,
           period_size * period_count,
           (size_t)(ringbuf_periods * period_size * st.bpf),
           st.iec958 ? "IEC958" : "S16_LE");

    pthread_t capture_tid, playback_tid;
    pthread_create(&capture_tid, NULL, capture_thread_fn, &st);
    pthread_create(&playback_tid, NULL, playback_thread_fn, &st);

    pthread_join(capture_tid, NULL);
    pthread_join(playback_tid, NULL);

    printf("\nshutting down\n");

    ringbuf_destroy(st.rb);
    pcm_close(st.pcm_in);
    pcm_close(st.pcm_out);
    return 0;
}
