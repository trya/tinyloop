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

struct state {
    unsigned int in_card, in_dev;
    unsigned int out_card, out_dev;
    struct pcm_config cfg;
    struct pcm *pcm_in;
    struct pcm *pcm_out;
    struct ringbuf *rb;
    unsigned int bpf; /* bytes per frame */
};

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -i card:device -o card:device [options]\n"
        "Options:\n"
        "  -i card:device   capture device (required)\n"
        "  -o card:device   playback device (required)\n"
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
        fprintf(stderr, "capture %u:%u: %s\n",
                st->in_card, st->in_dev,
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
    st->pcm_out = pcm_open(st->out_card, st->out_dev, PCM_OUT, &cfg);
    if (!st->pcm_out || !pcm_is_ready(st->pcm_out)) {
        fprintf(stderr, "playback %u:%u: %s\n",
                st->out_card, st->out_dev,
                st->pcm_out ? pcm_get_error(st->pcm_out) : "pcm_open failed");
        return -1;
    }
    return 0;
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
    size_t period_bytes = period_frames * st->bpf;
    void *buf = malloc(period_bytes);
    if (!buf) {
        fprintf(stderr, "playback: malloc failed\n");
        return NULL;
    }

    while (running) {
        size_t got = ringbuf_read(st->rb, buf, period_bytes);
        if (got == 0) {
            usleep(2000);
            continue;
        }

        unsigned int frames = pcm_bytes_to_frames(st->pcm_out, (unsigned int)got);
        int w = pcm_writei(st->pcm_out, buf, frames);
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

    while ((opt = getopt(argc, argv, "i:o:r:c:p:n:h")) != -1) {
        switch (opt) {
        case 'i': parse_pair(optarg, &st.in_card, &st.in_dev); got_i = 1; break;
        case 'o': parse_pair(optarg, &st.out_card, &st.out_dev); got_o = 1; break;
        case 'r': rate = (unsigned int)atoi(optarg); break;
        case 'c': channels = (unsigned int)atoi(optarg); break;
        case 'p': period_size = (unsigned int)atoi(optarg); break;
        case 'n': period_count = (unsigned int)atoi(optarg); break;
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

    printf("tinyloop: %u:%u -> %u:%u  %u Hz %u ch %u fps %u periods  buffer=%u rb=%zu\n",
           st.in_card, st.in_dev, st.out_card, st.out_dev,
           rate, channels, period_size, period_count,
           period_size * period_count,
           (size_t)(ringbuf_periods * period_size * st.bpf));

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
