/* Host test for the per-build tables, built for Windows by tests/host_patch_test.py and run there
   or under wine, on real azimuth.exe files: each one is laid out as Windows maps it (sections at
   their RVAs), then azimuth_offsets.c's entry for its build is checked against the image (the
   hashed functions, and every vtable slot holding its function), and the livery and save
   patches of azimuth_livery.c and azimuth_save.c are applied, applied again (already done) and
   compared with what the build needs. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FH2_TWEAKS_TEST 1
#include "../adapters/azimuth_host.c"
#include "../adapters/azimuth_offsets.c"
#include "../adapters/azimuth_livery.c"
#include "../adapters/azimuth_save.c"
#include "../sha256.c"

void log_append(const char *message) {
    (void)message;
}
void memory_copy(void *destination, const void *source, size_t size) {
    memmove(destination, source, size);
}
int memory_equal(const void *left, const void *right, size_t size) {
    return !memcmp(left, right, size);
}
int memory_read(const void *address, void *output, size_t size) {
    memmove(output, address, size);
    return 1;
}
void azimuth_repair_missing_markers(unsigned char *host) {
    (void)host;
}

static int failures;

static void check(int ok, const char *file, const char *what) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        ++failures;
        fprintf(stderr, "FAIL %s: %s\n", file, what);
    }
}

static unsigned char *map_image(const char *path, unsigned long long *preferred) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char *raw = malloc(size);
    if (!raw || fread(raw, 1, size, file) != (size_t)size) {
        fclose(file);
        return 0;
    }
    fclose(file);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(raw + ((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    unsigned char *image = VirtualAlloc(0, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_READWRITE);
    if (!image) {
        return 0;
    }
    memcpy(image, raw, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        DWORD n = section->SizeOfRawData < section->Misc.VirtualSize ? section->SizeOfRawData
                                                                     : section->Misc.VirtualSize;
        memcpy(image + section->VirtualAddress, raw + section->PointerToRawData, n);
    }
    *preferred = nt->OptionalHeader.ImageBase;
    free(raw);
    return image;
}

static int hash_is(const unsigned char *code, const unsigned char *expected) {
    unsigned char digest[32];
    Sha256Context hash;
    sha256_initialize(&hash);
    sha256_update(&hash, code, 0x20);
    sha256_finalize(&hash, digest);
    return !memcmp(digest, expected, 32);
}

static int slot_holds(const unsigned char *image, unsigned long long preferred, unsigned slot,
                      unsigned target) {
    unsigned long long value;
    memcpy(&value, image + slot, 8);
    return value == preferred + target;
}

/* expected: the build's name ("0.9.10"), or "-" for one FH2Online has no offsets for */
static void test_image(const char *path, const char *expected) {
    unsigned long long preferred = 0;
    unsigned char *image = map_image(path, &preferred);
    printf("%s\n", path);
    if (!image) {
        check(0, path, "the file is read and laid out");
        return;
    }
    AzimuthBuild build = azimuth_host_build(image);
    const AzimuthOffsets *offsets = azimuth_offsets(image);
    check(build != AZIMUTH_UNKNOWN, path, "a known Azimuth build");
    check(offsets ? !strcmp(offsets->name, expected) : !strcmp(expected, "-"), path,
          "its offsets are the expected build's");
    if (offsets) {
        check(hash_is(image + offsets->net_hook, offsets->net_hook_hash), path,
              "CoCreateInstance hashes as recorded");
        check(hash_is(image + offsets->net_activation, offsets->net_activation_hash), path,
              "the activation trace hashes as recorded");
        int slots = 1;
        for (unsigned i = 0; i < 6; ++i) {
            slots &= slot_holds(image, preferred, offsets->net_token_slots[i], offsets->net_token_targets[i]);
        }
        check(slots, path, "the 6 GetTokenAndSignature slots hold their functions");
        check(slot_holds(image, preferred, offsets->account_xuid_slot, offsets->account_xuid_target), path,
              "the XboxUserId slot holds its function");
        slots = 1;
        for (unsigned i = 0; i < 3; ++i) {
            slots &= slot_holds(image, preferred, offsets->account_info_slots[i], offsets->account_info_targets[i]);
        }
        check(slots, path, "the 3 LocalDisplayInfo slots hold their functions");
    }
    int mask = Fh2TestAzimuthLiveryCompatibility(image);
    check(build == AZIMUTH_0910 ? mask == 0 : mask == 1, path,
          build == AZIMUTH_0910 ? "2x MSAA mask: nothing to patch (Azimuth's own)" : "2x MSAA mask patched");
    int placement = Fh2TestAzimuthPlacementTextures(image);
    int placed = build == AZIMUTH_099 || build == AZIMUTH_0910;
    check(placed ? placement == 1 : placement == 0, path,
          placed ? "placement and tiled-write patches applied" : "placement patches skipped");
    check(Fh2TestAzimuthSaveCompatibility(image, 1) == 1, path, "save checksum patches applied");
    check(Fh2TestAzimuthSaveCompatibility(image, 1) == 2, path, "save checksum patches already applied");
    if (placed) {
        check(Fh2TestAzimuthPlacementTextures(image) == 2, path, "placement patches already applied");
    }
}

int main(int argc, char **argv) {
    if (argc < 3 || argc % 2 == 0) {
        fprintf(stderr, "usage: host_patch_test <azimuth.exe> <build name or -> [...]\n");
        return 2;
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        test_image(argv[i], argv[i + 1]);
    }
    printf(failures ? "%d failed\n" : "all ok\n", failures);
    return failures != 0;
}
