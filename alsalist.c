// probe_tinyalsa_names.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tinyalsa/asoundlib.h>

static const unsigned int MAX_CARDS = 16;
static const unsigned int MAX_DEVICES = 32;

/* Read name from /proc/asound/cardN/pcmM[c|p]/info if present.
   Returns pointer to static buffer or NULL. */
static const char *read_proc_name(unsigned int card, unsigned int dev, int playback) {
    static char namebuf[256];
    char path[256];
    snprintf(path, sizeof(path), "/proc/asound/card%u/pcm%u%c/info", card, dev, playback ? 'p' : 'c');
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
                char *nl = strchr(namebuf, '\n'); if (nl) *nl = '\0';
                break;
            }
        }
    }
    fclose(f);
    if (namebuf[0] == '\0') return NULL;
    return namebuf;
}

int main(void) {
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
            /* Playback probe */
            struct pcm *pcm = pcm_open(card, dev, PCM_OUT, &cfg);
            if (pcm && pcm_is_ready(pcm)) {
                if (!card_has) { printf("card%u:\n", card); card_has = 1; }
                const char *name = read_proc_name(card, dev, 1);
                if (name)
                    printf("  device %u playback: available  - name: %s\n", dev, name);
                else
                    printf("  device %u playback: available  - (no proc name)\n", dev);
                pcm_close(pcm);
            } else {
                if (pcm) pcm_close(pcm);
            }

            /* Capture probe */
            pcm = pcm_open(card, dev, PCM_IN, &cfg);
            if (pcm && pcm_is_ready(pcm)) {
                if (!card_has) { printf("card%u:\n", card); card_has = 1; }
                const char *name = read_proc_name(card, dev, 0);
                if (name)
                    printf("  device %u capture:  available  - name: %s\n", dev, name);
                else
                    printf("  device %u capture:  available  - (no proc name)\n", dev);
                pcm_close(pcm);
            } else {
                if (pcm) pcm_close(pcm);
            }
        }
    }

    return 0;
}
