/* Replays a pad-reading file through touch_core and prints one CSV row per sample.
 * Format: "ESTP" | uint8 pads | uint32 n | n * (uint32 t_ms | pads * uint16), little-endian. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "touch_core.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s pads.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    char magic[4]; uint8_t pads; uint32_t n;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "ESTP", 4) || fread(&pads, 1, 1, f) != 1 || fread(&n, 4, 1, f) != 1) {
        fprintf(stderr, "bad header\n"); return 2;
    }
    touch_core_t core; touch_config_t cfg = TOUCH_CONFIG_DEFAULT();
    cfg.pads = pads; touch_core_init(&core, &cfg);
    uint16_t v[TOUCH_MAX_PADS]; touch_result_t r;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t t;
        if (fread(&t, 4, 1, f) != 1 || fread(v, 2, pads, f) != pads) { fprintf(stderr, "short read\n"); return 2; }
        touch_core_process(&core, v, t, &r);
        printf("%u,%d,%d,%d,%u,%u,%d,%.4f,%.4f,%.4f,%.4f,%u,%d,%d,%d,%u,%d,%d\n", t, r.event, r.gesture, r.active,
               r.pad_mask, r.pad_count, r.has_angle, r.angle, r.travel, r.span, r.strength, r.duration_ms,
               r.recalibrated, r.crown, r.zone, r.zone_mask, r.sustained, r.handled);
    }
    fclose(f);
    return 0;
}
