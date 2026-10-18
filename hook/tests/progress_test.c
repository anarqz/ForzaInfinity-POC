/* Host test for adapters/azimuth_progress.c: the totals read from an UpdateAll body (bin/xtsw)
   and the limits a profile sync is held back by. Built for
   Windows by tests/progress_test.py and run there or under wine. */
#define FH2_PROGRESS_TEST 1
#include "../adapters/azimuth_progress.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static size_t put32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
    return 4;
}

/* An UpdateAll body: one element in playerHistory and playerRaceHistory (and in assists, to see
   it skipped), none elsewhere. */
static size_t body(unsigned char *out, unsigned credits, unsigned level, unsigned xp, unsigned winnings) {
    size_t at = 0;
    for (unsigned i = 0; i < ARRAY_COUNT(pg_element_size); ++i) {
        unsigned count = (i == 0 || i == PG_PLAYER_HISTORY || i == PG_RACE_HISTORY) ? 1 : 0;
        at += put32(out + at, count);
        if (!count) continue;
        memset(out + at, 0xAB, pg_element_size[i]);
        if (i == PG_PLAYER_HISTORY) put32(out + at + PG_CREDITS_AT, credits);
        if (i == PG_RACE_HISTORY) {
            put32(out + at + PG_LEVEL_AT, level);
            put32(out + at + PG_XP_AT, xp);
            put32(out + at + PG_WINNINGS_AT, winnings);
        }
        at += pg_element_size[i];
    }
    return at;
}

/* A real body (a stored profile, xtsw-encoded) and the totals it holds. */
static void real_body(const char *path, char **expect) {
    static unsigned char data[65536];
    unsigned t[4];
    FILE *f = fopen(path, "rb");
    CHECK(f != 0);
    if (!f) return;
    size_t n = fread(data, 1, sizeof(data), f);
    fclose(f);
    CHECK(azimuth_progress_read(data, n, t));
    for (int i = 0; i < 4; ++i) CHECK(t[i] == (unsigned)strtoul(expect[i], 0, 10));
    printf("real body: credits %u level %u xp %u winnings %u\n", t[0], t[1], t[2], t[3]);
}

int main(int argc, char **argv) {
    static unsigned char b[4096];
    unsigned t[4];
    if (argc == 6) real_body(argv[1], argv + 2);
    size_t n = body(b, 16463, 1, 11830, 254354);
    CHECK(azimuth_progress_read(b, n, t));
    CHECK(t[0] == 16463 && t[1] == 1 && t[2] == 11830 && t[3] == 254354);
    /* short, long, a count beyond the body: not read */
    CHECK(!azimuth_progress_read(b, n - 1, t));
    b[n] = 0;
    CHECK(!azimuth_progress_read(b, n + 1, t));
    put32(b, 0x7fffffff);
    CHECK(!azimuth_progress_read(b, n, t));
    CHECK(!azimuth_progress_read(b, 3, t));
    /* no history: nothing to measure */
    static unsigned char empty[17 * 4];
    CHECK(!azimuth_progress_read(empty, sizeof(empty), t));

    unsigned base[4] = {16463, 1, 11830, 254354};
    /* a race: fine; spending: fine */
    unsigned race[4] = {25863, 1, 17472, 263754};
    CHECK(azimuth_progress_over(base, race, 0.15) == 0);
    unsigned spent[4] = {0, 1, 11830, 254354};
    CHECK(azimuth_progress_over(base, spent, 0) == 0);
    /* twice the largest jump production saw (268k): fine at once; more: held */
    unsigned big[4] = {16463 + 536000, 1, 11830, 254354};
    CHECK(azimuth_progress_over(base, big, 0) == 0);
    unsigned huge[4] = {16463 + 2000000, 1, 11830, 254354};
    CHECK(azimuth_progress_over(base, huge, 0) == 1);
    CHECK(azimuth_progress_over(base, huge, 3.0) == 0); /* 540k + 3 x 620k */
    /* XP and levels */
    unsigned xp[4] = {16463, 1, 11830 + 900000, 254354};
    CHECK(azimuth_progress_over(base, xp, 0.5) == 3);
    unsigned levels[4] = {16463, 1 + 14, 11830, 254354};
    CHECK(azimuth_progress_over(base, levels, 0) == 4);
    CHECK(azimuth_progress_over(base, levels, 0.5) == 0);
    /* winnings: measured once known, taken as they are from 0 */
    unsigned won[4] = {16463, 1, 11830, 254354 + 3000000};
    CHECK(azimuth_progress_over(base, won, 0) == 2);
    unsigned zero[4] = {16463, 1, 11830, 0};
    CHECK(azimuth_progress_over(zero, won, 0) == 0);

    printf(failures ? "progress_test: %d failed\n" : "progress_test: ok\n", failures);
    return failures != 0;
}
