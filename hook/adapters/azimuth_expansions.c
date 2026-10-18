/* Expansions the Infinity profile has (the server lists them in /me: every profile has Storm
   Island). Storm Island's data is in the base game (ContentOffers 31, MountPath game:\); what
   the game lacks is a licence, so its rival events and the island asked the marketplace for the
   offer and stopped at MARKETPLACE ERROR.

   Each licence pass (FUN_14028b8e0) first runs "UPDATE ContentOffers SET IsPurchased=0", then
   marks the licensed offers and reads them into its caches. Its lea r8 at game+0x28b998 is
   pointed at our statement instead, which also marks offer 31 purchased and installed, written
   in the zero padding at the end of .rdata's last page (within the lea's reach). The game's own
   statement stays as it was for profiles without the expansion. */

#include "azimuth_host.h"
#include "logging.h"
#include "support.h"

#define EXPANSIONS_RESET_LEA_RVA 0x28b998u
#define EXPANSIONS_RESET_SQL_RVA 0x1e03770u
#define EXPANSIONS_CAVE_RVA 0x2c46f30u
#define EXPANSIONS_CAVE_SIZE 0xd0u

static const char expansions_reset_sql[] = "UPDATE ContentOffers SET IsPurchased=0";
static const char expansions_unlock_sql[] =
    "UPDATE ContentOffers SET IsPurchased=(Id=31),IsInstalled=(IsInstalled OR Id=31)";

static unsigned char *expansions_game;
static volatile LONG expansions_done;

static int expansions_write(void *target, const void *bytes, size_t n) {
    DWORD old_protection, ignored;
    if (!VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &old_protection)) {
        return 0;
    }
    memory_copy(target, bytes, n);
    VirtualProtect(target, n, old_protection, &ignored);
    return 1;
}

static void expansions_unlock(void) {
    unsigned char *lea = expansions_game + EXPANSIONS_RESET_LEA_RVA;
    unsigned char *cave = expansions_game + EXPANSIONS_CAVE_RVA;
    unsigned char code[7], free_bytes[EXPANSIONS_CAVE_SIZE];
    char current[sizeof(expansions_reset_sql)];
    int32_t displacement;
    if (InterlockedCompareExchange(&expansions_done, 1, 0) != 0) {
        return;
    }
    /* lea r8, [rip + disp32] to the game's reset statement, and an unused cave */
    if (!memory_read(lea, code, sizeof(code)) || code[0] != 0x4c || code[1] != 0x8d || code[2] != 0x05) {
        log_append("EXPANSIONS: licence pass not as expected; Storm Island stays locked.\r\n");
        return;
    }
    memory_copy(&displacement, code + 3, sizeof(displacement));
    if (lea + sizeof(code) + displacement != expansions_game + EXPANSIONS_RESET_SQL_RVA ||
        !memory_read(expansions_game + EXPANSIONS_RESET_SQL_RVA, current, sizeof(current)) ||
        !memory_equal(current, expansions_reset_sql, sizeof(current))) {
        log_append("EXPANSIONS: licence pass not as expected; Storm Island stays locked.\r\n");
        return;
    }
    if (!memory_read(cave, free_bytes, sizeof(free_bytes))) {
        log_append("EXPANSIONS: no room for the statement; Storm Island stays locked.\r\n");
        return;
    }
    for (size_t i = 0; i < sizeof(free_bytes); ++i) {
        if (free_bytes[i]) {
            log_append("EXPANSIONS: no room for the statement; Storm Island stays locked.\r\n");
            return;
        }
    }
    int32_t to_cave = (int32_t)(cave - (lea + sizeof(code)));
    if (!expansions_write(cave, expansions_unlock_sql, sizeof(expansions_unlock_sql)) ||
        !expansions_write(lea + 3, &to_cave, sizeof(to_cave))) {
        log_append("EXPANSIONS: could not write the statement; Storm Island stays locked.\r\n");
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), lea, sizeof(code));
    log_append("EXPANSIONS: Storm Island unlocked for this profile.\r\n");
}

void azimuth_expansions_granted(void) {
    if (expansions_game) {
        expansions_unlock();
    }
}

void azimuth_apply_expansions(unsigned char *game_image) {
    expansions_game = game_image;
    if (azimuth_account_storm_island()) {
        expansions_unlock();
    }
}
