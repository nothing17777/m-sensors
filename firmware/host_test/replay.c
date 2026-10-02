/* Replays a frames file through motion_core and prints one CSV row per frame.
 * File format: "ESFR" | uint16 w | uint16 h | uint32 n | n * (uint32 t_ms | w*h bytes), little-endian. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "motion_core.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s frames.bin\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    char magic[4]; uint16_t w, h; uint32_t n;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "ESFR", 4) || fread(&w, 2, 1, f) != 1 ||
        fread(&h, 2, 1, f) != 1 || fread(&n, 4, 1, f) != 1) { fprintf(stderr, "bad header\n"); return 2; }
    uint8_t *buf = malloc((size_t)w * h);
    motion_core_t m;
    motion_config_t cfg = MOTION_CONFIG_DEFAULT();
    if (!buf || motion_core_init(&m, &cfg, w, h) != 0) { fprintf(stderr, "init failed\n"); return 2; }
    motion_result_t r;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t t;
        if (fread(&t, 4, 1, f) != 1 || fread(buf, 1, (size_t)w * h, f) != (size_t)w * h) { fprintf(stderr, "short read\n"); return 2; }
        motion_core_process(&m, buf, t, &r);
        printf("%u,%d,%d,%d,%d,%u,%.6f,%.6f,%u,%u,%u,%u,%.6f,%.6f,%u,%d,%d,%u\n", t, r.event, r.level, r.arousal,
               r.active, r.changed_cells, r.centroid_x, r.centroid_y, r.bbox_x0, r.bbox_y0, r.bbox_x1, r.bbox_y1,
               r.speed, r.energy, r.duration_ms, r.trend, r.settling, r.threshold);
    }
    motion_core_deinit(&m); free(buf); fclose(f);
    return 0;
}
