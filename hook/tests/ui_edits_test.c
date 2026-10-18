/* Runs azimuth_ui.c's page and flow edits on files given on the command line and writes the
   results next to them (*.edited), for an XML check. Built for the host by
   tests/ui_edits_test.py; the game hooks themselves are never called. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adapters/azimuth_host.h"

void log_append(const char *message) {
    fputs(message, stderr);
}
size_t text_length(const char *text) {
    return strlen(text);
}
void memory_copy(void *destination, const void *source, size_t size) {
    memmove(destination, source, size);
}
int memory_equal(const void *left, const void *right, size_t size) {
    return !memcmp(left, right, size);
}
int memory_is_writable(const void *address, size_t size) {
    (void)address;
    (void)size;
    return 1;
}
int memory_read(const void *address, void *output, size_t size) {
    memcpy(output, address, size);
    return 1;
}

static const InfinityTile test_tiles[INFINITY_TILE_COUNT] = {
    {"PROFILE", "Icons/TiledMenus/Autoshow.png", 0},
    {"ADICIONAR AMIGO", "PauseMenuTileBackgrounds/MyRivals.png", 1},
    {"TESTAR CONEX\xc3\x83O", "PauseMenuTileBackgrounds/OnlineFreeroam.png", 1},
    {"AMIGOS", "Icons/TiledMenus/TeamCredits.png", 0},
    {"CHAVE DO SITE", "Icons/TiledMenus/Options.png", 0},
    {"TROCAR PERFIL", "Icons/TiledMenus/Garage.png", 0},
    {0, 0, 0},
    {"UP TO DATE", "Icons/TiledMenus/AutoUpgrade.png", 0},
    {"HOSPEDAR MUNDO", "Icons/TiledMenus/BarnFinds.png", 0},
    {"PROCURAR MUNDO", "Icons/TiledMenus/CustomUpgrade.png", 0}};
const InfinityTile *azimuth_infinity_tiles(void) {
    return test_tiles;
}
void azimuth_lang_set_portuguese(int on) {
    (void)on;
}
int azimuth_cloud_stopped(void) {
    return 0;
}
const char *azimuth_tr8(const char *english) {
    return english;
}
int azimuth_account_banner(char *out, size_t cap, const char **color) {
    snprintf(out, cap, "INFINITY  |  SEM CONEX\xc3\x83O COM O SERVIDOR");
    *color = "#ff4f4f";
    return 1;
}

#include "adapters/azimuth_ui.c"

static char *slurp(const char *path, unsigned *length) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc((size_t)n + 1);
    *length = (unsigned)fread(data, 1, (size_t)n, f);
    data[*length] = 0;
    fclose(f);
    return data;
}

static int spit(const char *path, const char *suffix, const UiText *text) {
    char name[1024];
    snprintf(name, sizeof(name), "%s.%s", path, suffix);
    FILE *f = fopen(name, "wb");
    fwrite(text->data, 1, text->length, f);
    fclose(f);
    printf("%s: %u bytes\n", name, text->length);
    return 1;
}

/* usage: ui_edits_test options.xaml pause.xaml ui_screens.xml pause_flow.xml [MessageCenter.str ...] */
int main(int argc, char **argv) {
    unsigned n;
    char *page;
    if (argc < 5) {
        return 2;
    }
    int ok = 1;
    page = slurp(argv[1], &n);
    ok &= page && ui_edit_options(page, n, &ui_options) && spit(argv[1], "options", &ui_options);
    ok &= page && ui_edit_infinity(page, n, &ui_infinity) && spit(argv[1], "infinity", &ui_infinity);
    page = slurp(argv[2], &n);
    ok &= page && ui_edit_pause(page, n, "{local:Loc KinectSplitscreen.IDS_Kinect_Splitscreen}",
                                       "{local:Loc KinectSplitscreen.IDS_Profile_Stomp_Desc}",
                                       "{local:Loc KinectSplitscreen.IDS_Scroll_Directions}", &ui_pause[0]) &&
          spit(argv[2], "pause", &ui_pause[0]);
    page = slurp(argv[3], &n);
    ok &= page && ui_replace_anchors(page, n, ui_screens_edits, ARRAY_COUNT(ui_screens_edits) / 2, &ui_screens) &&
          spit(argv[3], "flow", &ui_screens);
    page = slurp(argv[4], &n);
    ok &= page && ui_replace_anchors(page, n, ui_pause_flow_edits, ARRAY_COUNT(ui_pause_flow_edits) / 2, &ui_pause_flow) &&
          spit(argv[4], "flow", &ui_pause_flow);
    for (int i = 5; i < argc; ++i) {
        page = slurp(argv[i], &n);
        unsigned changed = page ? ui_edit_message_center((unsigned char *)page, n) : 0;
        UiText table = {page, n, n + 1};
        printf("%s: %u texts\n", argv[i], changed);
        ok &= changed > 0 && spit(argv[i], "mc", &table);
    }
    printf(ok ? "all edits built\n" : "SOME EDIT FAILED\n");
    return ok ? 0 : 1;
}
