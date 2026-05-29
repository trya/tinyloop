#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tinyalsa/asoundlib.h>

static const unsigned int MAX_CARDS = 16;
static const unsigned int MAX_DEVICES = 32;

static int try_open(unsigned int card, unsigned int dev, unsigned int flags, struct pcm_config *cfg)
{
    struct pcm *pcm = pcm_open(card, dev, flags, cfg);
    int ready = pcm && pcm_is_ready(pcm);
    if (ready) { pcm_close(pcm); return 1; }

    /* playback: retry with IEC958 when S16_LE fails (e.g. vc4-hdmi) */
    if (!(flags & PCM_IN) && cfg->format == PCM_FORMAT_S16_LE) {
        cfg->format = PCM_FORMAT_IEC958_SUBFRAME_LE;
        pcm = pcm_open(card, dev, flags, cfg);
        ready = pcm && pcm_is_ready(pcm);
        cfg->format = PCM_FORMAT_S16_LE; /* restore */
        if (ready) { pcm_close(pcm); return 1; }
    }

    if (pcm) pcm_close(pcm);
    return 0;
}

static const char *read_proc_name(unsigned int card, unsigned int dev, int playback)
{
    static char namebuf[256];
    char path[256];
    snprintf(path, sizeof(path), "/proc/asound/card%u/pcm%u%c/info",
             card, dev, playback ? 'p' : 'c');
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    namebuf[0] = '\0';
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "name:", 5) == 0 || strncmp(line, "id:", 3) == 0) {
            char *p = strchr(line, ':');
            if (p) {
                while (*(++p) == ' ' || *p == '\t');
                snprintf(namebuf, sizeof(namebuf), "%s", p);
                char *nl = strchr(namebuf, '\n');
                if (nl) *nl = '\0';
                break;
            }
        }
    }
    fclose(f);
    if (namebuf[0] == '\0') return NULL;
    return namebuf;
}

/* Check if a PCM device supports the given direction by parsing
   /proc/asound/pcm (e.g. "01-00: ... : playback 1 : capture 1"). */
static int pcm_direction_supported(unsigned int card, unsigned int dev, int playback)
{
    FILE *f = fopen("/proc/asound/pcm", "r");
    if (!f)
        return 0;

    char line[256];
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%02u-%02u:", card, dev);
    const char *needle = playback ? "playback" : "capture";

    int supported = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, prefix, strlen(prefix)) != 0)
            continue;

        const char *p = line;
        while ((p = strstr(p, needle)) != NULL) {
            p += strlen(needle);
            while (*p == ' ') p++;
            if (*p >= '1' && *p <= '9') {
                supported = 1;
                break;
            }
        }
        break;
    }

    fclose(f);
    return supported;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  -P    playback devices only\n"
        "  -C    capture devices only\n"
        "  -c    CSV output: \"name\",card,dev,status\n"
        "  -h    show this help\n", prog);
}

int main(int argc, char **argv)
{
    int only_playback = 0;
    int only_capture = 0;
    int csv = 0;
    int opt;

    while ((opt = getopt(argc, argv, "PcCh")) != -1) {
        switch (opt) {
        case 'P': only_playback = 1; break;
        case 'C': only_capture = 1; break;
        case 'c': csv = 1; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    int show_playback = !only_capture || only_playback;
    int show_capture  = !only_playback || only_capture;

    const unsigned int channels = 2;
    const unsigned int rate = 48000;
    struct pcm_config cfg = {
        .channels = channels,
        .rate = rate,
        .format = PCM_FORMAT_S16_LE,
        .period_size = 1024,
        .period_count = 4,
        .start_threshold = 0,
        .stop_threshold = 0,
        .silence_threshold = 0,
    };

    for (unsigned int card = 0; card < MAX_CARDS; ++card) {
        int card_has = 0;
        for (unsigned int dev = 0; dev < MAX_DEVICES; ++dev) {
            if (show_playback && pcm_direction_supported(card, dev, 1)) {
                int ready = try_open(card, dev, PCM_OUT, &cfg);
                const char *name = read_proc_name(card, dev, 1);

                if (csv) {
                    printf("\"%s\",%u,%u,%s\n",
                           name ? name : "hw:?,?", card, dev,
                           ready ? "available" : "busy");
                } else {
                    if (!card_has) { printf("card%u:\n", card); card_has = 1; }
                    if (ready)
                        printf("  device %u playback: available      - name: %s\n",
                               dev, name ? name : "(no name)");
                    else
                        printf("  device %u playback: in-use         - name: %s\n",
                               dev, name ? name : "(no name)");
                }
            }

            if (show_capture && pcm_direction_supported(card, dev, 0)) {
                int ready = try_open(card, dev, PCM_IN, &cfg);
                const char *name = read_proc_name(card, dev, 0);

                if (csv) {
                    printf("\"%s\",%u,%u,%s\n",
                           name ? name : "hw:?,?", card, dev,
                           ready ? "available" : "busy");
                } else {
                    if (!card_has) { printf("card%u:\n", card); card_has = 1; }
                    if (ready)
                        printf("  device %u capture:  available      - name: %s\n",
                               dev, name ? name : "(no name)");
                    else
                        printf("  device %u capture:  in-use         - name: %s\n",
                               dev, name ? name : "(no name)");
                }
            }
        }
    }

    return 0;
}
