/* Host test for adapters/azimuth_codec.c: unpacks an FH2SAVE1 bundle (methods 0, 1, 2) into a
   folder, and round-trips every file through method 2.
   Usage: codec_test <bundle> <output folder>   (driven by tests/codec_test.py) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../adapters/azimuth_codec.c"

static unsigned get32(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *bundle = malloc(size);
    if (fread(bundle, 1, size, f) != size || size < 20 || memcmp(bundle, "FH2SAVE1", 8)) {
        return 2;
    }
    fclose(f);
    static unsigned work[1 << 16];
    unsigned count = get32(bundle + 16);
    size_t at = 20;
    int failed = 0;
    for (unsigned k = 0; k < count; ++k) {
        size_t name_length = bundle[at] | (size_t)bundle[at + 1] << 8;
        char name[64] = {0};
        memcpy(name, bundle + at + 2, name_length < 63 ? name_length : 63);
        at += 2 + name_length;
        unsigned raw = get32(bundle + at), stored = get32(bundle + at + 4);
        int method = bundle[at + 8];
        at += 9;
        unsigned char *out = malloc(raw ? raw : 1);
        int ok = method == 0   ? (memcpy(out, bundle + at, raw), stored == raw)
                 : method == 1 ? codec_xpress_huffman_decode(bundle + at, stored, out, raw)
                 : method == 2 ? codec_lz_decode(bundle + at, stored, out, raw)
                               : 0;
        at += stored;
        /* method 2 round trip */
        unsigned char *packed = malloc(raw + 64), *back = malloc(raw ? raw : 1);
        size_t lz = ok ? codec_lz_encode(out, raw, packed, raw + 64, work) : 0;
        int round = !raw || (lz && codec_lz_decode(packed, lz, back, raw) && !memcmp(out, back, raw));
        printf("%-24s method=%d raw=%9u stored=%7u decode=%s lz=%7zu roundtrip=%s\n", name, method, raw, stored,
               ok ? "ok" : "FAIL", lz, round ? "ok" : "FAIL");
        failed |= !ok || !round;
        if (ok) {
            char path[1024];
            for (char *p = name; *p; ++p) {
                if (*p == '/') {
                    *p = '_';
                }
            }
            snprintf(path, sizeof(path), "%s/%s", argv[2], name);
            FILE *o = fopen(path, "wb");
            fwrite(out, 1, raw, o);
            fclose(o);
        }
        free(out);
        free(packed);
        free(back);
    }
    return failed || at != size;
}
