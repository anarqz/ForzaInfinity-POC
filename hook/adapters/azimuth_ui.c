/* Game pages edited in memory as the game reads them from media/UI.zip:
   - Settings (Scenes/Anthem/A_PauseOptions.xaml): its right column becomes two tiles,
     CREDITS (the game's own, now 1x1) on top and INFINITY (PART_Button_Infinity, bound to
     the FH2Online menu by azimuth_online_menu.c) below;
   - the pause menu (Scenes/Anthem/A_PauseMenu.xaml): a line centered at the top says whether
     the Infinity server is connected (rebuilt every time the page is read).

   Every zip entry the game reads is opened by FUN_1411ecbd0(archive, &stream, entry, mode),
   which returns an IOSys stream (vtable: +0x10 AddRef, +0x18 Release, +0x38 Seek(offset,
   origin), +0x50 bytes remaining, +0x90 Read(buffer, size)). That function is detoured: when
   a stream is exactly one of these pages (size and x:Class), it is read, edited, and replaced
   by the game's own CMemoryStream (FUN_1411e0640) over the edited text. Nothing on disk
   changes and no copy of a game file ships in the mod; other content is left untouched. */
#include "azimuth_host.h"
#include "logging.h"
#include "support.h"
#include "azimuth_font_widths.h"

#define UI_ZIP_OPEN_RVA 0x11ecbd0u
#define UI_MEMORY_STREAM_RVA 0x11e0640u
#define UI_GAME_ALLOC_RVA 0x3229c0u
#define UI_OPTIONS_SIZE 17949u
#define UI_PAUSE_SIZE 143644u
#define UI_SOURCE_MAX (UI_PAUSE_SIZE + 1u)
#define UI_OPTIONS_MAX 24576u
#define UI_PAUSE_MAX (UI_PAUSE_SIZE + 2048u)

static const unsigned char ui_zip_open_bytes[16] = {0x48, 0x8b, 0xc4, 0x44, 0x89, 0x48, 0x20, 0x48,
                                                    0x89, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54};

typedef void **(*UiZipOpenFn)(void *, void **, void *, unsigned);
typedef void *(*UiAllocFn)(size_t);
typedef void *(*UiMemoryStreamFn)(void *, const void *, int);
typedef unsigned long (*UiRefFn)(void *);
typedef char (*UiSeekFn)(void *, int, int);
typedef long long (*UiRemainingFn)(void *);
typedef int (*UiReadFn)(void *, void *, unsigned);

typedef struct {
    char *data;
    unsigned length, capacity;
} UiText;

static unsigned char *ui_game;
static UiZipOpenFn ui_zip_open_original;
static char ui_source[UI_SOURCE_MAX];
static char ui_options_buffer[UI_OPTIONS_MAX];
static UiText ui_options = {ui_options_buffer, 0, UI_OPTIONS_MAX};
/* the pause page alternates between two buffers, so a page still being parsed is never
   rewritten under it */
static char ui_pause_buffers[2][UI_PAUSE_MAX];
static UiText ui_pause[2] = {{ui_pause_buffers[0], 0, UI_PAUSE_MAX}, {ui_pause_buffers[1], 0, UI_PAUSE_MAX}};
static unsigned ui_pause_current;
static char ui_pause_banner[256];
static int ui_options_failed, ui_pause_failed;
static volatile LONG ui_busy, ui_logged_options, ui_logged_pause;

static void *ui_slot(void *object, unsigned offset) {
    return (*(void ***)object)[offset / sizeof(void *)];
}

/* ------------------------------------------------------------ text edits */

static const char *ui_find(const char *text, unsigned length, const char *needle, unsigned from) {
    size_t n = text_length(needle);
    for (unsigned i = from; i + n <= length; ++i) {
        if (text[i] == needle[0] && memory_equal(text + i, needle, n)) {
            return text + i;
        }
    }
    return 0;
}

static int ui_append(UiText *out, const char *text, size_t n) {
    if (out->length + n >= out->capacity) {
        return 0;
    }
    memory_copy(out->data + out->length, text, n);
    out->length += (unsigned)n;
    return 1;
}

/* Copies the source from `*at` up to `until`, skips `skip` bytes of it and adds `insert`. */
static int ui_copy(UiText *out, const char **at, const char *until, size_t skip, const char *insert) {
    if (!ui_append(out, *at, (size_t)(until - *at)) || (insert && !ui_append(out, insert, text_length(insert)))) {
        return 0;
    }
    *at = until + skip;
    return 1;
}

#define UI_INFINITY_ITEM                                                                                      \
    "\r\n            <local:TileMenuDataItem x:Key=\"A_MenuDataItem_Infinity\" Heading=\"INFINITY\" "        \
    "Icon=\"/ForzaUI;component/Textures/Anthem/Icons/TiledMenus/Options.png\" />"
#define UI_INFINITY_BUTTON                                                                                    \
    "\r\n                                <Button x:Name=\"PART_Button_Infinity\"\r\n"                        \
    "                                        local:TileMenuNavigationManager.TileState=\"Enabled\"\r\n"      \
    "                                        DataContext=\"{StaticResource A_MenuDataItem_Infinity}\"\r\n"   \
    "                                        local:TileMenuNavigationManager.GridX=\"4\"\r\n"                \
    "                                        local:TileMenuNavigationManager.GridY=\"1\"\r\n"                \
    "                                        Style=\"{DynamicResource A_Style_Button_Tile_1x1}\"\r\n"         \
    "                                        Canvas.Left=\"{StaticResource A_Double_TileCoord_X_4}\"\r\n"     \
    "                                        Canvas.Top=\"{StaticResource A_Double_TileCoord_Y_1}\"\r\n"      \
    "                                        Template=\"{StaticResource A_ControlTemplate_IconTile}\"\r\n"    \
    "                                        local:UIPageTransitionManager.TransitionIndex=\"6\"\r\n"        \
    "                                        local:BindingProxy.C=\"DisableMotionBlur\"/>"

/* Settings: credits 1x1 on top, INFINITY below. 0 when the text is not the one we know. */
static int ui_edit_options(const char *page, unsigned length, UiText *out) {
    const char *item = ui_find(page, length, "x:Key=\"A_MenuDataItem_TeamCredits\"", 0);
    const char *item_end = item ? ui_find(page, length, "/>", (unsigned)(item - page)) : 0;
    const char *button = ui_find(page, length, "x:Name=\"PART_Button_TeamCredits\"", 0);
    if (!item_end || !button || item_end > button || ui_find(page, length, "PART_Button_Infinity", 0)) {
        return 0;
    }
    unsigned from = (unsigned)(button - page);
    const char *size = ui_find(page, length, "A_Style_Button_Tile_1x2", from);
    const char *tile = ui_find(page, length, "A_ControlTemplate_VerticalImageTile", from);
    const char *end = ui_find(page, length, "/>", from);
    if (!size || !tile || !end || size > end || tile > end) {
        return 0;
    }
    const char *at = page;
    out->length = 0;
    return ui_copy(out, &at, item_end + 2, 0, UI_INFINITY_ITEM) &&
           ui_copy(out, &at, size, text_length("A_Style_Button_Tile_1x2"), "A_Style_Button_Tile_1x1") &&
           ui_copy(out, &at, tile, text_length("A_ControlTemplate_VerticalImageTile"), "A_ControlTemplate_IconTile") &&
           ui_copy(out, &at, end + 2, 0, UI_INFINITY_BUTTON) && ui_copy(out, &at, page + length, 0, 0);
}

/* The pause menu with the status line after its player bar (same top margin as the bar).
   `text` is either a literal or "{local:Loc Category.Key}". */
/* `alert1` / `alert2`: the texts of two more lines in red, in the same place (Loc bindings of the
   live alert strings; 0 for none). Every line has a fixed width, its text centred in it: the game
   measures a text block once, so a line built with a short text cut a longer one later. */
static int ui_edit_pause(const char *page, unsigned length, const char *text, const char *alert1, const char *alert2,
                         UiText *out) {
    const char *bar = ui_find(page, length, "x:Name=\"TopBar\" />", 0);
    if (!bar || ui_find(page, length, "x:Name=\"TopBar\" />", (unsigned)(bar - page) + 1)) {
        return 0;
    }
    const char *at = page;
    out->length = 0;
    int ok = ui_copy(out, &at, bar + text_length("x:Name=\"TopBar\" />"), 0,
                     "\r\n        <local:FZTextBlock HorizontalAlignment=\"Center\" VerticalAlignment=\"Top\" "
                     "Margin=\"0,40,0,0\" Width=\"900\" TextAlignment=\"Center\" Text=\"") &&
             ui_copy(out, &at, at, 0, text) &&
             ui_copy(out, &at, at, 0,
                     "\" Style=\"{StaticResource A_Style_FZTextBlock_AnthemA}\" Color=\"#f5f5f5\" FontSize=\"24\" />");
    if (ok && alert1 && alert2) {
        ok = ui_copy(out, &at, at, 0,
                     "\r\n        <local:FZTextBlock HorizontalAlignment=\"Center\" VerticalAlignment=\"Top\" "
                     "Margin=\"0,40,0,0\" Width=\"900\" TextAlignment=\"Center\" Text=\"") &&
             ui_copy(out, &at, at, 0, alert1) &&
             ui_copy(out, &at, at, 0,
                     "\" Style=\"{StaticResource A_Style_FZTextBlock_AnthemA}\" Color=\"#ff4f4f\" FontSize=\"24\" />"
                     "\r\n        <local:FZTextBlock HorizontalAlignment=\"Center\" VerticalAlignment=\"Top\" "
                     "Margin=\"0,72,0,0\" Width=\"900\" TextAlignment=\"Center\" Text=\"") &&
             ui_copy(out, &at, at, 0, alert2) &&
             ui_copy(out, &at, at, 0,
                     "\" Style=\"{StaticResource A_Style_FZTextBlock_AnthemA}\" Color=\"#ff4f4f\" FontSize=\"24\" />");
    }
    return ok && ui_copy(out, &at, page + length, 0, 0);
}

/* ------------------------------------------------------- live status line */

/* The game keeps the pause page it built the first time, so its text cannot carry the
   status. The line shows a game string instead: one of the Kinect split-screen strings (never
   shown on PC), the longest in the game's language, whose text in the loaded string table is
   rewritten every few seconds (FUN_14123d4b0: id of a string name; FUN_14123a570(&string,
   category, id): a CLocString, GetText at vtable +0x38). */
#define UI_LOC_ID_RVA 0x123d4b0u
#define UI_LOC_MAKE_RVA 0x123a570u
#define UI_STATUS_CATEGORY "KinectSplitscreen"

typedef unsigned (*UiLocIdFn)(const char *);
typedef void **(*UiLocMakeFn)(void **, const char *, unsigned);
typedef wchar_t *(*UiLocTextFn)(void *);
typedef void(WINAPI *UiSleepFn)(DWORD);

static const char *const ui_status_keys[] = {
    "IDS_P2_Wave_To_Signin", "IDS_Will_Now_Choose_Car", "IDS_Choose_Car", "IDS_Player_2", "IDS_Guest",
    "IDS_Choose_The_Track", "IDS_Choose_Your_Car", "IDS_Kinect_Splitscreen",
    "IDS_Requires_Two_Players", "IDS_Looking_For_Player_2", "IDS_Both_Players_In_Range", "IDS_To_MainMenu",
    "IDS_To_QuitRace", "IDS_Player_Wins", "IDS_Profile_Stomp_Title"};
static wchar_t *ui_status_text;
static size_t ui_status_capacity;
static const char *ui_status_key;
/* Two more of those strings, the red lines under it (empty unless the cloud sync stopped). */
#define UI_ALERT_KEY_1 "IDS_Profile_Stomp_Desc"
#define UI_ALERT_KEY_2 "IDS_Scroll_Directions"
static wchar_t *ui_alert_text[2];
static size_t ui_alert_capacity[2];

static size_t ui_wlen(const wchar_t *text) {
    size_t n = 0;
    while (text && text[n]) {
        ++n;
    }
    return n;
}

/* One of those strings' text buffer (kept: never released); 0 when not found. */
static wchar_t *ui_string_text(const char *key, size_t *capacity) {
    unsigned id = ((UiLocIdFn)(uintptr_t)(ui_game + UI_LOC_ID_RVA))(key);
    void *string = 0;
    ((UiLocMakeFn)(uintptr_t)(ui_game + UI_LOC_MAKE_RVA))(&string, UI_STATUS_CATEGORY, id);
    if (!string) {
        return 0;
    }
    wchar_t *text = ((UiLocTextFn)ui_slot(string, 0x38))(string);
    size_t n = ui_wlen(text);
    if (!n || !memory_is_writable(text, (n + 1) * sizeof(wchar_t))) {
        return 0;
    }
    *capacity = n;
    return text;
}

static int ui_alert_find(void) {
    if (!ui_alert_text[0]) {
        ui_alert_text[0] = ui_string_text(UI_ALERT_KEY_1, &ui_alert_capacity[0]);
    }
    if (!ui_alert_text[1]) {
        ui_alert_text[1] = ui_string_text(UI_ALERT_KEY_2, &ui_alert_capacity[1]);
    }
    return ui_alert_text[0] && ui_alert_text[1];
}

/* Picks the longest of those strings (its text buffer is kept); 0 when none is found. */
static int ui_status_find(void) {
    if (ui_status_text) {
        return 1;
    }
    for (unsigned i = 0; i < ARRAY_COUNT(ui_status_keys); ++i) {
        unsigned id = ((UiLocIdFn)(uintptr_t)(ui_game + UI_LOC_ID_RVA))(ui_status_keys[i]);
        void *string = 0;
        ((UiLocMakeFn)(uintptr_t)(ui_game + UI_LOC_MAKE_RVA))(&string, UI_STATUS_CATEGORY, id);
        if (!string) {
            continue;
        }
        wchar_t *text = ((UiLocTextFn)ui_slot(string, 0x38))(string);
        size_t n = ui_wlen(text);
        /* the string object is kept (never released): its text must stay where it is */
        if (n > ui_status_capacity && memory_is_writable(text, (n + 1) * sizeof(wchar_t))) {
            ui_status_text = text;
            ui_status_capacity = n;
            ui_status_key = ui_status_keys[i];
        }
    }
    return ui_status_text != 0;
}

/* Writes the current status into that string (padded with NULs to its full length). */
static void ui_cat(char *out, size_t cap, const char *text) {
    size_t n = text_length(out);
    while (*text && n + 1 < cap) {
        out[n++] = *text++;
    }
    out[n] = 0;
}

/* A glyph's advance in Anthem_A, the status lines' font (azimuth_font_widths.h), in thousandths
   of an em. */
static unsigned ui_glyph_width(wchar_t c) {
    unsigned lo = 0, hi = ARRAY_COUNT(azimuth_font_widths);
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (azimuth_font_widths[mid].codepoint == c) {
            return azimuth_font_widths[mid].advance;
        }
        if (azimuth_font_widths[mid].codepoint < c) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 500;
}

/* `utf8` into a kept string buffer, centred, or (0) a line that is off. The game lays a text
   block out once, with as many characters as it had then (and their width), and later draws at
   most that many from where they started (a line built with 40 characters cut a longer text at
   the 40th; one built with one space showed one letter). So a line always holds its string's
   whole length in no-break spaces around the text, and the text sits after half the width it
   leaves. */
static void ui_string_set(wchar_t *buffer, size_t capacity, const char *utf8) {
    static wchar_t wide[400];
    if (!buffer) {
        return;
    }
    size_t length = 0;
    if (utf8 && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)ARRAY_COUNT(wide)) > 0) {
        while (wide[length]) {
            ++length;
        }
    }
    if (length > capacity) {
        length = capacity;
    }
    unsigned width = 0, space = ui_glyph_width(0xA0);
    for (size_t i = 0; i < length; ++i) {
        width += ui_glyph_width(wide[i]);
    }
    size_t pad = width < capacity * space ? (capacity * space - width) / space / 2 : 0;
    if (pad > capacity - length) {
        pad = capacity - length;
    }
    size_t n = 0;
    while (n < pad) {
        buffer[n++] = 0xA0;
    }
    for (size_t i = 0; i < length; ++i) {
        buffer[n++] = wide[i];
    }
    while (n < capacity) {
        buffer[n++] = 0xA0;
    }
    buffer[capacity] = 0;
}

/* Writes the current status into those strings: the white line, or (the cloud sync stopped:
   progress rejected) the two red ones. */
static void ui_status_write(void) {
    static char text[200];
    static volatile LONG busy;
    const char *color;
    /* the live thread and the pause flow both write: one at a time (the other's turn comes) */
    if (!ui_status_text || InterlockedExchange(&busy, 1)) {
        return;
    }
    if (!azimuth_account_banner(text, sizeof(text), &color)) {
        InterlockedExchange(&busy, 0);
        return;
    }
    int alert = azimuth_cloud_stopped() && ui_alert_text[0] && ui_alert_text[1];
    ui_string_set(ui_status_text, ui_status_capacity, alert ? 0 : text);
    ui_string_set(ui_alert_text[0], ui_alert_capacity[0], alert ? azimuth_tr8("SUSPECTED INCOMPATIBLE PROGRESS") : 0);
    ui_string_set(ui_alert_text[1], ui_alert_capacity[1], alert ? azimuth_tr8("RESTART YOUR GAME") : 0);
    InterlockedExchange(&busy, 0);
}

/* The status lines now (the cloud sync just stopped: before the game pauses to show it). */
void azimuth_ui_status_refresh(void) {
    ui_status_write();
}

static DWORD WINAPI ui_status_thread(void *unused) {
    (void)unused;
    UiSleepFn sleep = (UiSleepFn)(uintptr_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep");
    if (!sleep) {
        return 0;
    }
    for (;;) {
        ui_status_write();
        sleep(5000);
    }
}

/* --------------------------------------------------------- INFINITY page */

/* The INFINITY screen is the Settings page class shown again by a new flow exit
   ("infinity", added below), with its tiles replaced: same grid, same styles. Each slot is
   {style, template, grid x, grid y, x coordinate, y coordinate}. Slot 8 is the second half of
   slot 1's place (X_2 = 564 is in Shared_TiledMenus.xaml): when it has a tile, slot 1 is a
   small icon tile too (ui_small_one). */
static const char *const ui_slots[INFINITY_TILE_COUNT][6] = {
    {"1x1", "IconTile", "0", "0", "X_0", "Y_0"},             {"2x1", "HorizontalImageTile", "1", "0", "X_1", "Y_0"},
    {"2x1", "HorizontalImageTile", "1", "1", "X_1", "Y_1"}, {"1x1", "IconTile", "3", "0", "X_3", "Y_0"},
    {"1x1", "IconTile", "3", "1", "X_3", "Y_1"},             {"1x1", "IconTile", "4", "0", "X_4", "Y_0"},
    {"1x1", "IconTile", "4", "1", "X_4", "Y_1"},             {"1x1", "IconTile", "0", "1", "X_0", "Y_1"},
    {"1x1", "IconTile", "2", "0", "X_2", "Y_0"},             {"1x1", "IconTile", "2", "1", "X_2", "Y_1"}};
static const char *const ui_small_one[6] = {"1x1", "IconTile", "1", "0", "X_1", "Y_0"};
static const char *const ui_small_two[6] = {"1x1", "IconTile", "1", "1", "X_1", "Y_1"};

static char ui_infinity_buffer[UI_OPTIONS_MAX];
static UiText ui_infinity = {ui_infinity_buffer, 0, UI_OPTIONS_MAX};
static volatile LONG ui_infinity_pending;

static int ui_add(UiText *out, const char *text) {
    return ui_append(out, text, text_length(text));
}

/* Settings with the INFINITY title and tiles (from azimuth_infinity_tiles). */
static int ui_edit_infinity(const char *page, unsigned length, UiText *out) {
    const InfinityTile *tiles = azimuth_infinity_tiles();
    const char *title = ui_find(page, length, "Content=\"{local:Loc ScreenTitles.IDS_MyProfile}\"", 0);
    const char *item = ui_find(page, length, "x:Key=\"A_MenuDataItem_TeamCredits\"", 0);
    const char *item_end = item ? ui_find(page, length, "/>", (unsigned)(item - page)) : 0;
    const char *canvas = ui_find(page, length, "<local:AutoSizeCanvas x:Name=\"PART_SCOPE_Home\">", 0);
    const char *canvas_end = canvas ? ui_find(page, length, "</local:AutoSizeCanvas>", (unsigned)(canvas - page)) : 0;
    /* resources (the data items), then the title, then the tiles */
    if (!tiles || !title || !item_end || !canvas_end || item_end > title || title > canvas) {
        return 0;
    }
    const char *at = page;
    char number[2] = {0, 0};
    out->length = 0;
    if (!ui_copy(out, &at, item_end + 2, 0, 0)) {
        return 0;
    }
    for (int i = 0; i < INFINITY_TILE_COUNT; ++i) {
        if (!tiles[i].heading) {
            continue;
        }
        number[0] = (char)('0' + i);
        if (!ui_add(out, "\r\n            <local:TileMenuDataItem x:Key=\"A_MenuDataItem_I") || !ui_add(out, number) ||
            !ui_add(out, "\" Heading=\"") || !ui_add(out, tiles[i].heading) ||
            !ui_add(out, "\" Icon=\"/ForzaUI;component/Textures/Anthem/") || !ui_add(out, tiles[i].icon) ||
            !ui_add(out, tiles[i].photo ? "\" UseAlternateColors=\"True\" />" : "\" />")) {
            return 0;
        }
    }
    /* the page class looks PART_Button_Assists up when it opens (and dereferences it): a
       collapsed one stays for it */
    if (!ui_copy(out, &at, title, text_length("Content=\"{local:Loc ScreenTitles.IDS_MyProfile}\""),
                 "Content=\"INFINITY\"") ||
        !ui_copy(out, &at, canvas + text_length("<local:AutoSizeCanvas x:Name=\"PART_SCOPE_Home\">"), 0,
                 /* a complete tile (the navigation manager reads every tile's grid
                    properties), disabled, collapsed and outside the used grid */
                 "\r\n                                <Button x:Name=\"PART_Button_Assists\"\r\n"
                 "                                        local:TileMenuNavigationManager.TileState=\"Disabled\"\r\n"
                 "                                        Visibility=\"Collapsed\"\r\n"
                 "                                        DataContext=\"{StaticResource A_MenuDataItem_Assists}\"\r\n"
                 "                                        local:TileMenuNavigationManager.GridX=\"6\"\r\n"
                 "                                        local:TileMenuNavigationManager.GridY=\"0\"\r\n"
                 "                                        Style=\"{DynamicResource A_Style_Button_Tile_1x1}\"\r\n"
                 "                                        Canvas.Left=\"{StaticResource A_Double_TileCoord_X_4}\"\r\n"
                 "                                        Canvas.Top=\"{StaticResource A_Double_TileCoord_Y_0}\"\r\n"
                 "                                        Template=\"{StaticResource A_ControlTemplate_IconTile}\"\r\n"
                 "                                        local:UIPageTransitionManager.TransitionIndex=\"8\"\r\n"
                 "                                        local:BindingProxy.C=\"DisableMotionBlur\"/>")) {
        return 0;
    }
    for (int i = 0; i < INFINITY_TILE_COUNT; ++i) {
        const char *const *slot = i == 1 && tiles[8].heading   ? ui_small_one
                                  : i == 2 && tiles[9].heading ? ui_small_two
                                                               : ui_slots[i];
        if (!tiles[i].heading) {
            continue;
        }
        number[0] = (char)('0' + i);
        if (!ui_add(out, "\r\n                                <Button x:Name=\"PART_Button_I") || !ui_add(out, number) ||
            !ui_add(out, "\"\r\n                                        local:TileMenuNavigationManager.TileState=\"Enabled\""
                         "\r\n                                        DataContext=\"{StaticResource A_MenuDataItem_I") ||
            !ui_add(out, number) ||
            !ui_add(out, "}\"\r\n                                        local:TileMenuNavigationManager.GridX=\"") ||
            !ui_add(out, slot[2]) ||
            !ui_add(out, "\"\r\n                                        local:TileMenuNavigationManager.GridY=\"") ||
            !ui_add(out, slot[3]) ||
            !ui_add(out, "\"\r\n                                        Style=\"{DynamicResource A_Style_Button_Tile_") ||
            !ui_add(out, slot[0]) ||
            !ui_add(out, "}\"\r\n                                        Canvas.Left=\"{StaticResource A_Double_TileCoord_") ||
            !ui_add(out, slot[4]) ||
            !ui_add(out, "}\"\r\n                                        Canvas.Top=\"{StaticResource A_Double_TileCoord_") ||
            !ui_add(out, slot[5]) ||
            !ui_add(out, "}\"\r\n                                        Template=\"{StaticResource A_ControlTemplate_") ||
            !ui_add(out, slot[1]) ||
            !ui_add(out, "}\"\r\n                                        local:UIPageTransitionManager.TransitionIndex=\"") ||
            !ui_add(out, number) ||
            !ui_add(out, "\"\r\n                                        local:BindingProxy.C=\"DisableMotionBlur\"/>")) {
            return 0;
        }
    }
    /* the page's own tiles are left out */
    at = canvas_end;
    return ui_add(out, "\r\n                            ") && ui_copy(out, &at, page + length, 0, 0);
}

/* ------------------------------------------------------------ flow edits */

#define UI_SCREENS_SIZE 101115u
#define UI_PAUSE_FLOW_SIZE 55717u
#define UI_FLOW_MAX (UI_SCREENS_SIZE + 2048u)

static char ui_screens_buffer[UI_FLOW_MAX], ui_pause_flow_buffer[UI_FLOW_MAX];
static UiText ui_screens = {ui_screens_buffer, 0, UI_FLOW_MAX};
static UiText ui_pause_flow = {ui_pause_flow_buffer, 0, UI_FLOW_MAX};
static int ui_screens_failed, ui_pause_flow_failed;

/* Replaces each anchor (unique in the source, in order) by its replacement. */
static int ui_replace_anchors(const char *source, unsigned length, const char *const *pairs, unsigned count,
                              UiText *out) {
    const char *at = source;
    out->length = 0;
    for (unsigned i = 0; i < count; ++i) {
        const char *anchor = ui_find(source, length, pairs[2 * i], 0);
        if (!anchor || anchor < at ||
            ui_find(source, length, pairs[2 * i], (unsigned)(anchor - source) + 1)) {
            return 0;
        }
        if (!ui_copy(out, &at, anchor, text_length(pairs[2 * i]), pairs[2 * i + 1])) {
            return 0;
        }
    }
    return ui_copy(out, &at, source + length, 0, 0);
}

/* ui_screens.xml: a show_infinity state shows the Settings page class again. It keeps every
   exit of Settings: should the INFINITY page fail to build, the plain page shown there still
   works. (A page only takes the exits its class registers, so INFINITY has none of its own.) */
#define UI_MENU_FLOW_COMMENT "<!--Pause Menu flow-->"
static const char *const ui_screens_edits[] = {
    UI_MENU_FLOW_COMMENT,
    UI_MENU_FLOW_COMMENT
    "\r\n  <state id=\"show_infinity\">\r\n    <entry id=\"enter\" target=\".\" />\r\n"
    "    <entry id=\"enterback\" target=\".\" />\r\n    <exit id=\"back\" />\r\n"
    "    <exit id=\"assists\" />\r\n    <exit id=\"controls\" />\r\n    <exit id=\"hud\" />\r\n"
    "    <exit id=\"video\" />\r\n    <exit id=\"audio\" />\r\n    <exit id=\"team_credits\" />\r\n"
    "    <behavior id=\"UIPagePauseOptionsMenu\" isBack=\"{IsEntry enterback}\">\r\n"
    "      <transition id=\"assists\" target=\".assists\" />\r\n      <transition id=\"controls\" target=\".controls\" />\r\n"
    "      <transition id=\"audio\" target=\".audio\" />\r\n      <transition id=\"video\" target=\".video\" />\r\n"
    "      <transition id=\"hud\" target=\".hud\" />\r\n      <transition id=\"team_credits\" target=\".team_credits\" />\r\n"
    "      <transition id=\"back\" target=\".back\" />\r\n    </behavior>\r\n"
    "    <behavior id=\"OnParentTransition\">\r\n      <transition id=\"ParentTransition\" target=\".back\" />\r\n"
    "    </behavior>\r\n  </state>\r\n"};

/* pause.xml: the INFINITY tile takes Settings' team_credits exit (a registered one) with the
   Fh2InfinityOpen sticky answered true (ui_sticky_enter), so team_credits goes either to the
   credits or to INFINITY. Both ways pass through the spinner scene (a page of its own): going
   between two states of the same page class the game keeps the page it has, and the page must
   be read again to switch between Settings and INFINITY. */
#define UI_INFINITY_STICKY "Fh2InfinityOpen"
static const char *const ui_pause_flow_edits[] = {
    /* the sticky check with its name written in (global.check_bool_sticky takes it as a
       parameter, which ui_sticky_enter does not read) */
    "<state id=\"my_profile_flow\">",
    "<state id=\"fh2_infinity_or_credits\">\r\n    <entry target=\".\" />\r\n"
    "    <exit id=\"true\" />\r\n    <exit id=\"false\" />\r\n    <exit id=\"unset\" />\r\n"
    "    <behavior id=\"BoolSwitchSticky\" name=\"" UI_INFINITY_STICKY "\">\r\n"
    "      <transition id=\"true\" target=\".true\" />\r\n      <transition id=\"false\" target=\".false\" />\r\n"
    "      <transition id=\"unset\" target=\".unset\" />\r\n    </behavior>\r\n  </state>\r\n\r\n"
    "  <state id=\"my_profile_flow\">",    "<exit id=\"team_credits\" target=\"team_credits\" />",
    "<exit id=\"team_credits\" target=\"credits_or_infinity\" />",
    "<node id=\"team_credits\" state=\"ui_screens.show_team_credits\">\r\n"
    "      <exit id=\"back\" target=\"my_profile.enterback\" />\r\n    </node>",
    "<node id=\"team_credits\" state=\"ui_screens.show_team_credits\">\r\n"
    "      <exit id=\"back\" target=\"my_profile.enterback\" />\r\n    </node>\r\n"
    "\r\n    <node id=\"credits_or_infinity\" state=\"fh2_infinity_or_credits\">\r\n"
    "      <exit id=\"true\" target=\"infinity_in\" />\r\n      <exit id=\"false\" target=\"team_credits\" />\r\n"
    "      <exit id=\"unset\" target=\"team_credits\" />\r\n    </node>\r\n"
    "\r\n    <node id=\"infinity_in\" state=\"ui_screens.show_empty_scene_with_spinner_and_continue\">\r\n"
    "      <exit target=\"infinity.enter\" />\r\n    </node>\r\n"
    "\r\n    <node id=\"infinity_out\" state=\"ui_screens.show_empty_scene_with_spinner_and_continue\">\r\n"
    "      <exit target=\"my_profile.enterback\" />\r\n    </node>\r\n"
    "\r\n    <node id=\"infinity\" state=\"ui_screens.show_infinity\">\r\n"
    "      <entry id=\"enter\" />\r\n"
    "      <exit id=\"back\" target=\"infinity_out\" />\r\n"
    "      <exit id=\"assists\" target=\"assists\" />\r\n      <exit id=\"controls\" target=\"control_options.enter\" />\r\n"
    "      <exit id=\"hud\" target=\"hud_options\" />\r\n      <exit id=\"video\" target=\"video_options\" />\r\n"
    "      <exit id=\"audio\" target=\"audio_options\" />\r\n      <exit id=\"team_credits\" target=\"team_credits\" />\r\n"
    "    </node>"};

/* The sticky check (States::BoolSwitchSticky, vtable +0x20 = FUN_140945990) picks the
   transition at +0x80 (true), +0xb0 (false) or +0xe0 (unset) with FUN_141925260; its name is
   a std::string at +0x60 when written literally in the flow. For Fh2InfinityOpen the answer is
   whether the INFINITY tile asked for it (once). */
#define UI_STICKY_SLOT_RVA (0x2d29a90u + 0x20u)
#define UI_STICKY_ENTER_RVA 0x945990u
#define UI_TAKE_TRANSITION_RVA 0x1925260u

typedef void (*UiStickyEnterFn)(void *, void *, void *, void *);
typedef void (*UiTakeTransitionFn)(void *, void *);

static UiStickyEnterFn ui_sticky_enter_original;
static volatile LONG ui_infinity_sticky, ui_sticky_bound_logged;

static void ui_sticky_enter(void *self, void *a, void *b, void *c) {
    unsigned char *state = self;
    if (!*(void **)(state + 0x30)) {
        unsigned char *name = state + 0x60; /* std::string: inline or pointer, size +0x10, capacity +0x18 */
        size_t size = *(size_t *)(name + 0x10), capacity = *(size_t *)(name + 0x18);
        const char *text = capacity > 15 ? *(const char **)name : (const char *)name;
        if (size == sizeof(UI_INFINITY_STICKY) - 1 && text && memory_equal(text, UI_INFINITY_STICKY, size)) {
            int open = InterlockedExchange(&ui_infinity_sticky, 0) != 0;
            log_append(open ? "UI: Settings > INFINITY.\r\n" : "UI: Settings > credits.\r\n");
            ((UiTakeTransitionFn)(uintptr_t)(ui_game + UI_TAKE_TRANSITION_RVA))(self, state + (open ? 0x80 : 0xb0));
            return;
        }
    }
    if (*(void **)(state + 0x30) && !InterlockedExchange(&ui_sticky_bound_logged, 1)) {
        log_append("UI: a sticky check with a bound name (not ours).\r\n");
    }
    ui_sticky_enter_original(self, a, b, c);
}

/* Settings' INFINITY tile: the team_credits exit (the game's own std::string, object
   0x14352f438 + 0x10), as the credits tile handler does (FUN_140dcc6c0: remember the focused
   tile, then FUN_141422f10(page, &exit name) when the page has a flow at +0x310), with the
   sticky armed so the flow goes to INFINITY. The next read of the page is the INFINITY
   version. 0 when the page has no flow or the flow edits are not in place. */
#define UI_TAKE_EXIT_RVA 0x1422f10u
#define UI_REMEMBER_FOCUS_RVA 0x140e210u
#define UI_FOCUS_SLOT_RVA 0x352f478u
#define UI_CREDITS_EXIT_RVA 0x352f448u

typedef void (*UiTakeExitFn)(void *, const void *);
typedef void (*UiRememberFn)(void *, void *);

int azimuth_ui_open_infinity(void *page) {
    if (!ui_game || !ui_sticky_enter_original || !*(void **)((unsigned char *)page + 0x310) || ui_screens_failed ||
        ui_pause_flow_failed || !ui_screens.length || !ui_pause_flow.length) {
        return 0;
    }
    unsigned char focus[16];
    ((UiRememberFn)(uintptr_t)(ui_game + UI_REMEMBER_FOCUS_RVA))(*(void **)((unsigned char *)page + 0x410), focus);
    memory_copy(ui_game + UI_FOCUS_SLOT_RVA, focus, 12);
    InterlockedExchange(&ui_infinity_pending, 1);
    InterlockedExchange(&ui_infinity_sticky, 1);
    ((UiTakeExitFn)(uintptr_t)(ui_game + UI_TAKE_EXIT_RVA))(page, ui_game + UI_CREDITS_EXIT_RVA);
    log_append("UI: INFINITY clicked; flow exit taken.\r\n");
    return 1;
}
/* ------------------------------------------------------- message center */

/* The Message Center's texts (MessageCenter.str in media/stripped/stringtables/<language>.zip,
   LSB2) name Turn 10 as the sender of the server's messages. In
   each language the team's name (key 0x229b) becomes INFINITY in every text that holds it
   (also spelled without the space in "Turn 10"), and the list title (0x3232, "TURN 10
   MESSAGE") becomes INFINITY. A text is rewritten in place and never grows, so the table keeps
   its layout; one that INFINITY does not fit in stays as it is. The files are told apart by
   their size (each language's is different; cz and HU are the same file). */
static const unsigned ui_message_center_sizes[] = {10516u, 12210u, 12604u, 15664u, 15668u, 15670u, 15906u, 16050u,
                                                   16189u, 16346u, 16372u, 16604u, 16648u, 16982u, 17058u, 17274u};
#define UI_MESSAGE_CENTER_BR 16604u
#define UI_STRINGS_MAX 17280u
#define UI_STRINGS_TEXT 600u
static char ui_strings_buffer[UI_STRINGS_MAX];
static UiText ui_strings = {ui_strings_buffer, 0, UI_STRINGS_MAX};
static volatile LONG ui_logged_strings;

/* Each language's MessageCenter.str as its zip stores it: compressed size (the same order as
   ui_message_center_sizes; LOC.zip's 16519/3680 is not a language the game runs in). */
static const unsigned ui_message_center_packed[] = {4662u, 4866u, 4792u, 5339u, 5343u, 5345u, 5436u, 5492u,
                                                    5303u, 5520u, 5702u, 5509u, 5849u, 5615u, 5500u, 5706u};

/* Whether a zip entry is a Message Center table: its size and its compressed size (the entry's
   +0x18 and +0x1c, which the stream gets, FUN_1411df7b0) both one language's. Files of other
   zips share the sizes alone (car models, animations, shaders), and opening one of those a
   second time to look (as before) could break the game's own read of it: the car loads on the
   START menu crashed with a closed handle. */
static int ui_entry_is(const unsigned field[2], long long size, unsigned full, unsigned packed) {
    return size == (long long)full &&
           ((field[0] == full && field[1] == packed) || (field[0] == packed && field[1] == full));
}

static int ui_is_message_center(long long size, const unsigned field[2]) {
    for (unsigned i = 0; i < ARRAY_COUNT(ui_message_center_sizes); ++i) {
        if (ui_entry_is(field, size, ui_message_center_sizes[i], ui_message_center_packed[i])) {
            return 1;
        }
    }
    return 0;
}

/* The pages edited here as their zips store them (size, compressed size): stateflow.zip's
   ui_screens.xml and pause.xml, UI.zip's A_PauseOptions.xaml and A_PauseMenu.xaml. Only
   entries with both are looked at: a game stream of another file is never read here. */
static int ui_is_page(long long size, const unsigned field[2]) {
    return ui_entry_is(field, size, UI_SCREENS_SIZE, 9874u) || ui_entry_is(field, size, UI_PAUSE_FLOW_SIZE, 6759u) ||
           ui_entry_is(field, size, UI_OPTIONS_SIZE, 2605u) || ui_entry_is(field, size, UI_PAUSE_SIZE, 8434u);
}

static unsigned ui_u16(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static unsigned ui_u32(const unsigned char *p) {
    return ui_u16(p) | ui_u16(p + 2) << 16;
}

/* `text` with every `find` (n units) replaced by `with`, into `out` (cap units); its length,
   or `count` + 1 when it did not fit. */
static unsigned ui_wide_replace(const wchar_t *text, unsigned count, const wchar_t *find, unsigned n,
                                const wchar_t *with, unsigned w, wchar_t *out, unsigned cap) {
    unsigned at = 0, length = 0;
    while (at < count) {
        if (n && at + n <= count && memory_equal(text + at, find, n * sizeof(wchar_t))) {
            for (unsigned i = 0; i < w; ++i) {
                if (length >= cap) {
                    return count + 1;
                }
                out[length++] = with[i];
            }
            at += n;
        } else {
            if (length >= cap) {
                return count + 1;
            }
            out[length++] = text[at++];
        }
    }
    return length;
}

/* A sender line still naming Turn 10 ("De : l'equipe de communaute Turn 10", "Van: Communityteam
   Turn 10"): everything after its label's colon becomes INFINITY. `text` is changed in place
   (never longer); returns its length. */
static unsigned ui_sender_line(wchar_t *text, unsigned length) {
    static const wchar_t infinity[] = L"INFINITY";
    unsigned line = 0, colon = 0, turn = 0;
    while (line < length && text[line] != L'\n') {
        ++line;
    }
    for (unsigned i = 0; i < line; ++i) {
        if (!colon && (text[i] == L':' || text[i] == 0xff1a)) {
            colon = i + 1;
        }
        if (i + 4 <= line && (text[i] == L'T' || text[i] == L't') && (text[i + 1] | 0x20) == L'u' &&
            (text[i + 2] | 0x20) == L'r' && (text[i + 3] | 0x20) == L'n') {
            turn = 1;
        }
    }
    while (colon && colon < line && text[colon] == L' ') {
        ++colon;
    }
    unsigned w = ARRAY_COUNT(infinity) - 1;
    if (!turn || !colon || colon + w > line) {
        return length;
    }
    memory_copy(text + colon, infinity, w * sizeof(wchar_t));
    for (unsigned i = line; i < length; ++i) {
        text[colon + w + (i - line)] = text[i];
    }
    return length - (line - colon - w);
}

/* Rewrites `table` (`size` bytes) in place; returns how many texts changed. */
static unsigned ui_edit_message_center(unsigned char *table, unsigned size) {
    static const wchar_t infinity[] = L"INFINITY";
    static wchar_t text[UI_STRINGS_TEXT], first[UI_STRINGS_TEXT], second[UI_STRINGS_TEXT];
    wchar_t team[64], tight[64];
    unsigned team_units = 0, tight_units = 0, changed = 0;
    if (size < 0x2c || !memory_equal(table, "LSB2", 4)) {
        return 0;
    }
    unsigned count = ui_u32(table + 0x28), texts = 0x2c + 6 * count + 6;
    if (count > 4096 || texts >= size) {
        return 0;
    }
    /* pass 0 reads the team's name, pass 1 rewrites */
    for (int pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < count; ++i) {
            unsigned key = ui_u16(table + 0x2c + 6 * i);
            unsigned long long at = texts + 2ull * ui_u32(table + 0x2e + 6 * i);
            unsigned units = 0;
            while (at + 2 * units + 1 < size && units < UI_STRINGS_TEXT && ui_u16(table + at + 2 * units)) {
                text[units] = (wchar_t)ui_u16(table + at + 2 * units);
                ++units;
            }
            if (at + 2 * units + 1 >= size || units >= UI_STRINGS_TEXT) {
                continue;
            }
            if (pass == 0) {
                if (key == 0x229b && units >= ARRAY_COUNT(infinity) - 1 && units < ARRAY_COUNT(team)) {
                    memory_copy(team, text, units * sizeof(wchar_t));
                    team_units = units;
                    /* "Turn 10" also appears as "Turn10" */
                    for (unsigned k = 0; k < units; ++k) {
                        if (!(text[k] == L' ' && k + 2 < units && text[k + 1] == L'1' && text[k + 2] == L'0' && k >= 4 &&
                              (text[k - 1] == L'n' || text[k - 1] == L'N'))) {
                            tight[tight_units++] = text[k];
                        }
                    }
                }
                continue;
            }
            unsigned length;
            const wchar_t *result = second;
            if (key == 0x3232) {
                memory_copy(second, infinity, sizeof(infinity) - sizeof(wchar_t));
                length = ARRAY_COUNT(infinity) - 1;
            } else {
                unsigned a = ui_wide_replace(text, units, team, team_units, infinity, ARRAY_COUNT(infinity) - 1, first,
                                             UI_STRINGS_TEXT);
                if (a > units) {
                    continue;
                }
                length = tight_units != team_units
                             ? ui_wide_replace(first, a, tight, tight_units, infinity, ARRAY_COUNT(infinity) - 1, second,
                                               UI_STRINGS_TEXT)
                             : a;
                if (tight_units == team_units) {
                    result = first;
                }
                length = ui_sender_line((wchar_t *)result, length);
            }
            if (length > units || (length == units && memory_equal(result, text, units * sizeof(wchar_t)))) {
                continue;
            }
            for (unsigned k = 0; k < length; ++k) {
                table[at + 2 * k] = (unsigned char)(result[k] & 0xff);
                table[at + 2 * k + 1] = (unsigned char)(result[k] >> 8);
            }
            table[at + 2 * length] = 0;
            table[at + 2 * length + 1] = 0;
            ++changed;
        }
        if (pass == 0 && !team_units) {
            return 0;
        }
    }
    return changed;
}

/* ------------------------------------------------------------- the hook */

/* Reads a stream of exactly `size` bytes into ui_source; 1 when it holds `marker`. */
static int ui_read(void *stream, unsigned size, const char *marker) {
    unsigned got = 0;
    while (got < size) {
        int n = ((UiReadFn)ui_slot(stream, 0x90))(stream, ui_source + got, size - got);
        if (n <= 0) {
            break;
        }
        got += (unsigned)n;
    }
    ui_source[got] = 0;
    return got == size && ui_find(ui_source, got, marker, 0);
}

static void ui_rewind(void *stream) {
    ((UiSeekFn)ui_slot(stream, 0x38))(stream, 0, 0);
}

/* `*out` becomes a memory stream over `text`. */
static int ui_serve(void **out, void *stream, const UiText *text) {
    void *memory = ((UiAllocFn)(uintptr_t)(ui_game + UI_GAME_ALLOC_RVA))(0x60);
    if (!memory) {
        return 0;
    }
    ((UiMemoryStreamFn)(uintptr_t)(ui_game + UI_MEMORY_STREAM_RVA))(memory, text->data, (int)text->length);
    ((UiRefFn)ui_slot(memory, 0x10))(memory);
    *out = memory;
    ((UiRefFn)ui_slot(stream, 0x18))(stream);
    return 1;
}

/* A flow file edited once (the game reads them at start). */
static void ui_replace_flow(void **out, void *stream, unsigned size, const char *marker, const char *const *edits,
                            unsigned count, UiText *text, int *failed, const char *name) {
    if (!ui_read(stream, size, marker)) {
        ui_rewind(stream);
        return;
    }
    if (!text->length && !ui_replace_anchors(ui_source, size, edits, count, text)) {
        text->length = 0;
        *failed = 1;
        log_append("UI: ");
        log_append(name);
        log_append(" not as expected; no INFINITY screen.\r\n");
    }
    if (!text->length || !ui_serve(out, stream, text)) {
        ui_rewind(stream);
        return;
    }
    log_append("UI: ");
    log_append(name);
    log_append(" routes Settings > INFINITY.\r\n");
}

static void ui_replace(void **out, void *stream) {
    long long size = ((UiRemainingFn)ui_slot(stream, 0x50))(stream);
    if (size == UI_SCREENS_SIZE && !ui_screens_failed) {
        ui_replace_flow(out, stream, UI_SCREENS_SIZE, "<state id=\"show_my_profile\">", ui_screens_edits,
                        ARRAY_COUNT(ui_screens_edits) / 2, &ui_screens, &ui_screens_failed, "ui_screens.xml");
    } else if (size == UI_PAUSE_FLOW_SIZE && !ui_pause_flow_failed) {
        ui_replace_flow(out, stream, UI_PAUSE_FLOW_SIZE, "<state id=\"my_profile_flow\">", ui_pause_flow_edits,
                        ARRAY_COUNT(ui_pause_flow_edits) / 2, &ui_pause_flow, &ui_pause_flow_failed, "pause.xml");
    } else if (size == UI_OPTIONS_SIZE && InterlockedExchange(&ui_infinity_pending, 0)) {
        /* the INFINITY screen: built now, with the profile as it is now */
        if (!ui_read(stream, UI_OPTIONS_SIZE, "x:Class=\"ForzaUI.A_PauseOptions\"") ||
            !ui_edit_infinity(ui_source, UI_OPTIONS_SIZE, &ui_infinity) || !ui_serve(out, stream, &ui_infinity)) {
            log_append("UI: INFINITY screen could not be built; showing Settings.\r\n");
            ui_rewind(stream);
            ui_replace(out, stream);
        } else {
            log_append("UI: INFINITY screen shown.\r\n");
        }
    } else if (size == UI_OPTIONS_SIZE && !ui_options_failed) {
        if (!ui_read(stream, UI_OPTIONS_SIZE, "x:Class=\"ForzaUI.A_PauseOptions\"")) {
            ui_rewind(stream);
            return;
        }
        if (!ui_options.length && !ui_edit_options(ui_source, UI_OPTIONS_SIZE, &ui_options)) {
            ui_options.length = 0;
            ui_options_failed = 1;
            log_append("UI: A_PauseOptions.xaml not as expected; Settings left as is.\r\n");
        }
        if (!ui_options.length || !ui_serve(out, stream, &ui_options)) {
            ui_rewind(stream);
        } else if (InterlockedIncrement(&ui_logged_options) <= 20) {
            log_append("UI: Settings page read (CREDITS + INFINITY tiles).\r\n");
        }
    } else if (size == UI_PAUSE_SIZE && !ui_pause_failed) {
        static char text[200];
        const char *color = 0;
        if (!azimuth_account_banner(text, sizeof(text), &color) ||
            !ui_read(stream, UI_PAUSE_SIZE, "x:Class=\"ForzaUI.A_PauseMenu\"")) {
            ui_rewind(stream);
            return;
        }
        /* the live line (a game string rewritten by ui_status_thread), else this text; under it
           the two red alert lines, bound the same way */
        static char alert1[96], alert2[96];
        int alerts = 0;
        if (ui_status_find()) {
            if (ui_alert_find()) {
                const char *prefix = "{local:Loc " UI_STATUS_CATEGORY ".";
                alert1[0] = alert2[0] = 0;
                ui_cat(alert1, sizeof(alert1), prefix);
                ui_cat(alert1, sizeof(alert1), UI_ALERT_KEY_1 "}");
                ui_cat(alert2, sizeof(alert2), prefix);
                ui_cat(alert2, sizeof(alert2), UI_ALERT_KEY_2 "}");
                alerts = 1;
            } else {
                log_append("UI: the alert strings were not found; no red lines on the pause menu.\r\n");
            }
            static int started;
            text[0] = 0;
            memory_copy(text, "{local:Loc " UI_STATUS_CATEGORY ".", sizeof("{local:Loc " UI_STATUS_CATEGORY "."));
            size_t n = text_length(text), k = text_length(ui_status_key);
            memory_copy(text + n, ui_status_key, k);
            text[n + k] = '}';
            text[n + k + 1] = 0;
            ui_status_write();
            if (!started) {
                started = 1;
                HANDLE thread = CreateThread(0, 0, ui_status_thread, 0, 0, 0);
                if (thread) {
                    CloseHandle(thread);
                }
                log_append("UI: pause status line follows the server live.\r\n");
            }
        }
        /* a new buffer only when the line changes */
        UiText *page = &ui_pause[ui_pause_current];
        if (!page->length || !memory_equal(ui_pause_banner, text, text_length(text) + 1)) {
            ui_pause_current ^= 1;
            page = &ui_pause[ui_pause_current];
            if (!ui_edit_pause(ui_source, UI_PAUSE_SIZE, text, alerts ? alert1 : 0, alerts ? alert2 : 0, page)) {
                page->length = 0;
                ui_pause_failed = 1;
                log_append("UI: A_PauseMenu.xaml not as expected; no Infinity status line.\r\n");
                ui_rewind(stream);
                return;
            }
            memory_copy(ui_pause_banner, text, text_length(text) + 1);
        }
        if (!ui_serve(out, stream, page)) {
            ui_rewind(stream);
        } else if (InterlockedIncrement(&ui_logged_pause) <= 3) {
            log_append(alerts ? "UI: pause menu read with the Infinity status line and its red alert lines.\r\n"
                              : "UI: pause menu read with the Infinity status line.\r\n");
        }
    }
}

/* A Message Center table: other game files share its sizes (car models, animations and shaders,
   compressed in the car zips), so the game's stream is never read to tell: the entry is opened
   a second time, that copy is read and dropped, and the game's stream is replaced only when it
   is the table. */
static void ui_message_center(void *archive, void **out, void *entry, unsigned mode, void *stream, long long size) {
    void *probe = 0;
    ui_zip_open_original(archive, &probe, entry, mode);
    if (!probe) {
        return;
    }
    int table = ((UiRemainingFn)ui_slot(probe, 0x50))(probe) == size && ui_read(probe, (unsigned)size, "LSB2") &&
                memory_equal(ui_source, "LSB2", 4);
    ((UiRefFn)ui_slot(probe, 0x18))(probe);
    if (!table) {
        return;
    }
    /* the game's language: br.zip's table (pt-BR) is the only one of this size */
    azimuth_lang_set_portuguese(size == UI_MESSAGE_CENTER_BR);
    memory_copy(ui_strings.data, ui_source, (size_t)size);
    ui_strings.length = (unsigned)size;
    if (ui_edit_message_center((unsigned char *)ui_strings.data, ui_strings.length) &&
        ui_serve(out, stream, &ui_strings) && InterlockedIncrement(&ui_logged_strings) <= 3) {
        log_append("UI: Message Center texts name INFINITY as the sender.\r\n");
    }
}

static void **ui_zip_open(void *archive, void **out, void *entry, unsigned mode) {
    void **result = ui_zip_open_original(archive, out, entry, mode);
    void *stream = out ? *out : 0;
    /* zip entries open on several threads; the page buffers serve one at a time */
    if (!stream || InterlockedCompareExchange(&ui_busy, 1, 0)) {
        return result;
    }
    long long size = ((UiRemainingFn)ui_slot(stream, 0x50))(stream);
    unsigned field[2] = {0, 0};
    if (!entry || !memory_read((const unsigned char *)entry + 0x18, field, sizeof(field))) {
        field[0] = field[1] = 0;
    }
    if (ui_is_message_center(size, field)) {
        ui_message_center(archive, out, entry, mode, stream, size);
    } else if (ui_is_page(size, field)) {
        ui_replace(out, stream);
    }
    InterlockedExchange(&ui_busy, 0);
    return result;
}

void azimuth_apply_ui(unsigned char *game_image) {
    unsigned char *target = game_image + UI_ZIP_OPEN_RVA, current[16];
    ui_game = game_image;
    if (!memory_read(target, current, sizeof(current)) || !memory_equal(current, ui_zip_open_bytes, sizeof(current))) {
        log_append("UI: zip reader not as expected; game pages left as is.\r\n");
        return;
    }
    /* trampoline: the 16 original bytes (position independent), then jmp back */
    unsigned char *trampoline = VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) {
        return;
    }
    uintptr_t back = (uintptr_t)(target + sizeof(current));
    memory_copy(trampoline, current, sizeof(current));
    trampoline[16] = 0xff;
    trampoline[17] = 0x25;
    trampoline[18] = trampoline[19] = trampoline[20] = trampoline[21] = 0;
    memory_copy(trampoline + 22, &back, sizeof(back));
    ui_zip_open_original = (UiZipOpenFn)(uintptr_t)trampoline;

    unsigned char jump[14] = {0xff, 0x25, 0, 0, 0, 0};
    uintptr_t hook = (uintptr_t)ui_zip_open;
    memory_copy(jump + 6, &hook, sizeof(hook));
    DWORD old_protection, ignored;
    if (!VirtualProtect(target, sizeof(jump), PAGE_EXECUTE_READWRITE, &old_protection)) {
        return;
    }
    memory_copy(target, jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    VirtualProtect(target, sizeof(jump), old_protection, &ignored);

    /* the Fh2InfinityOpen sticky (Settings > INFINITY) */
    void **sticky_slot = (void **)(game_image + UI_STICKY_SLOT_RVA);
    void *sticky_value = 0;
    if (memory_read(sticky_slot, &sticky_value, sizeof(sticky_value)) && sticky_value == game_image + UI_STICKY_ENTER_RVA &&
        VirtualProtect(sticky_slot, sizeof(*sticky_slot), PAGE_READWRITE, &old_protection)) {
        ui_sticky_enter_original = (UiStickyEnterFn)(uintptr_t)sticky_value;
        *sticky_slot = (void *)(uintptr_t)ui_sticky_enter;
        VirtualProtect(sticky_slot, sizeof(*sticky_slot), old_protection, &ignored);
    } else {
        log_append("UI: sticky check not as expected; INFINITY uses the popup menu.\r\n");
    }
    log_append("UI: zip reader hooked for the Settings and pause pages.\r\n");
}
