#ifndef FH2_AZIMUTH_HOST_H
#define FH2_AZIMUTH_HOST_H
#include "extra_options.h"

typedef struct {
    unsigned offset, size;
    const unsigned char *bytes;
} HostCodePatch;

typedef struct {
    unsigned rva, size;
    const unsigned char *original_hash, *patched_hash;
    const HostCodePatch *patches;
    unsigned patch_count;
} HostPatchSet;

typedef enum {
    AZIMUTH_UNKNOWN,
    AZIMUTH_096,
    AZIMUTH_097,
    AZIMUTH_098,
    AZIMUTH_099,
    AZIMUTH_0910
} AzimuthBuild;

AzimuthBuild azimuth_host_build(const unsigned char *base);

/* 1 = applied, 2 = already applied, 0 = unknown build, -1 = write failed. */
int azimuth_patch_host_code(unsigned char *base, const HostPatchSet *set);
int azimuth_patch_host_codes(unsigned char *base, const HostPatchSet *sets, unsigned count);
void azimuth_apply_livery_compatibility(void);
void azimuth_apply_save_compatibility(int disable_checksums);
void azimuth_apply_network(const wchar_t *directory);
void azimuth_apply_economy_probe(unsigned char *game_image);
/* azimuth_events.c: entering an event in free roam online goes back to free roam (the relay does
   not run races yet). */
void azimuth_apply_events(unsigned char *game_image);
void azimuth_apply_no_game_clips(unsigned char *game_image);
void azimuth_apply_pad(void);
/* azimuth_ini.c: FH2_ExtraOptions.ini gets ForzaInfinity's [Network] (or the template when there
   is none) before it is read, keeping the player's settings; the log says what changed. */
void azimuth_prepare_ini(const wchar_t *directory);
void azimuth_log_ini(void);

/* azimuth_net.c: the configured server, for the account module. */
const wchar_t *azimuth_net_server_name(void);
int azimuth_net_proxy_enabled(void);
DWORD azimuth_net_call(const wchar_t *method, const wchar_t *path, const wchar_t *extra_header,
                       const char *body, char *out, size_t capacity);
DWORD azimuth_net_request(const wchar_t *method, const wchar_t *path, const wchar_t *headers,
                          const wchar_t *content_type, const void *body, size_t body_size, void *out,
                          size_t capacity, size_t *received);
int azimuth_net_ping(int timeout_ms);
/* The same, with /info's body in `out` (the pause menu reads latestHook from it). */
int azimuth_net_ping_reply(int timeout_ms, char *out, size_t cap);
DWORD azimuth_net_call_quick(const wchar_t *method, const wchar_t *path, const wchar_t *extra_header,
                             const char *body, char *out, size_t capacity, int timeout_ms);
/* GET https://<host><path> into `out` (azimuth_update.c reads the release from GitHub). */
DWORD azimuth_net_fetch(const wchar_t *host, const wchar_t *path, void *out, size_t capacity, size_t *received);
/* [Network] AutoUpdate (default 1). */
int azimuth_net_auto_update(void);
/* azimuth_cloud.c: the online profile's save, kept on the server. */
void azimuth_cloud_start(const wchar_t *bin, const wchar_t *xuid, const wchar_t *token, const wchar_t *ini,
                         const wchar_t *section);
int azimuth_cloud_upload_offline(const wchar_t *bin, const wchar_t *token, const wchar_t *ini, const wchar_t *section,
                                 const char **why);

/* azimuth_account.c: the player's FH2Online account on that server. */
void azimuth_apply_account(const wchar_t *directory);
const wchar_t *azimuth_account_token(void);
/* The same once the profile has been read (waits for it at the start, up to 30 s). */
const wchar_t *azimuth_account_token_wait(void);
/* azimuth_progress.c: a profile sync (OnlineProfile.UpdateAll) beyond what play earns since the
   last one sent is held back (1); the totals it reads and the limit check, for the tests. */
int azimuth_progress_hold(const wchar_t *headers, const void *body, size_t size);
int azimuth_progress_read(const unsigned char *body, size_t size, unsigned out[4]);
int azimuth_progress_over(const unsigned base[4], const unsigned now[4], double hours);
/* The XUID this session plays as online (0 offline). */
unsigned long long azimuth_account_xuid_value(void);
/* azimuth_proxy.c (F3): the joining player's UDP proxy between the game and a world on another
   PC (STUN, the punch exchange, the source filter). start returns 1 with `local` (a 28-byte
   sockaddr_in6) when the game should send there instead of `world`. */
void azimuth_proxy_configure(int direct);
int azimuth_proxy_start(const void *world, void *local);
void azimuth_proxy_stop(void);
/* Sends to the world from the proxy's socket (the address the relay knows this player by); 0
   while there is no world (or no proxy). */
int azimuth_proxy_send_world(const void *data, int size);
/* azimuth_carstate.c: the ExtraOptions headlights (0 the game's, 1 forced on, 2 forced off) and
   roof (0 closed, 1 open) of the players' cars, through the world's relay. */
void azimuth_carstate_local(int lights, int roof);
void azimuth_carstate_received(const unsigned char *data, int size);
int azimuth_carstate_any(void);
/* Whether azimuth_carstate_wanted needs `car`'s position now (else it is passed NULL). */
int azimuth_carstate_needs_position(void *car);
/* What car `car` (at `position`, NULL if unknown) should show, when it is a player's: 1 and the
   state, else 0. */
int azimuth_carstate_wanted(void *car, const float position[3], int *lights, int *roof);
/* TEST CONNECTION's free roam part (about 1.5 s: STUN from a socket of its own). */
void azimuth_proxy_nat_test(wchar_t *out, size_t cap);
/* azimuth_world.c: dedicated_server.net while this PC hosts a world (1), else 0. */
int azimuth_world_net_text(char *out, size_t cap);
/* -1 unavailable, 0 no account, 1 account kept but offline profile, 2 playing online */
int azimuth_account_state(void);
const wchar_t *azimuth_account_gamertag(void);
const wchar_t *azimuth_account_web_key(void);
const wchar_t *azimuth_account_discriminator(void);
const wchar_t *azimuth_account_server(void);
int azimuth_account_valid_name(const wchar_t *name);
/* `noob`: a Noob profile (the offline save as it is, no progress gates; only meets Noob
   players), else Hardcore (a new career, every gate). */
int azimuth_account_register(const wchar_t *name, int noob, wchar_t *message, size_t cap);
int azimuth_account_delete(wchar_t *message, size_t cap);
int azimuth_account_new_web_key(wchar_t *message, size_t cap);
int azimuth_account_add_friend(const wchar_t *gamertag, wchar_t *message, size_t cap);
void azimuth_account_set_online(int online);
void azimuth_account_restart(void);
int azimuth_account_log_in(const wchar_t *gamertag, const wchar_t *web_key, wchar_t *message, size_t cap);
const wchar_t *azimuth_account_notice(void);
/* The active profile is Noob (its own save, no progress gates), from the server's word. */
int azimuth_account_noob(void);
void azimuth_account_force_notice(const char *english);
/* azimuth_cloud.c: progress of this session was not accepted (`why`): nothing more is saved
   online (cloud save, profile syncs), the status line says to restart, and the next start loads
   the cloud save. The game is never closed for it. */
void azimuth_cloud_stop(const char *why);
/* azimuth_ui.c: rewrites the pause menu's status lines now. */
void azimuth_ui_status_refresh(void);
int azimuth_cloud_stopped(void);
/* SWITCH PROFILE: the Infinity profiles of this PC on this server (at most
   ACCOUNT_PROFILES_MAX, the one in use first), from FH2Online_Accounts.ini and, with
   `ask_server`, the server's list of the ones this PC created. */
#define ACCOUNT_PROFILES_MAX 3
typedef struct {
    wchar_t xuid[24], gamertag[32], web_key[24];
    int current;   /* the [host:port] one: 2 playing it now, 1 kept but this session is offline */
    int saved;     /* its token is on this PC */
    int made_here; /* this PC created it: it plays it again without the site key */
    int suspended;
} AccountProfile;
int azimuth_account_profiles(AccountProfile *out, int cap, int ask_server);
/* How many azimuth_account_profiles lists without asking the server. */
int azimuth_account_saved_profiles(void);
/* The next start plays `profile` (1), or `message` says why not (0). */
int azimuth_account_switch(const AccountProfile *profile, wchar_t *message, size_t cap);
/* The friendship with a player picked in the game (player actions): 2 friends, 1 asked them,
   3 they asked, 0 neither, -1 unknown; and adding / removing them. */
int azimuth_account_friend_state(unsigned long long xuid);
int azimuth_account_friend_change(unsigned long long xuid, int add, wchar_t *message, size_t cap);
/* REPORT PLAYER (azimuth_online_menu.c): 1 sent, 0 not (`message` says why). */
int azimuth_account_report(unsigned long long xuid, wchar_t *message, size_t cap);
/* A notice for the INFINITY screen (English UTF-8; azimuth_cloud.c: the server's save restored). */
void azimuth_account_set_notice(const char *english);
/* azimuth_lang.c: Infinity's texts in Portuguese (Brazil) when the game runs in it, else in
   English. azimuth_tr / azimuth_tr8 look a text up by its English. */
void azimuth_lang_set_portuguese(int on);
int azimuth_lang_portuguese(void);
const wchar_t *azimuth_tr(const wchar_t *english);
const char *azimuth_tr8(const char *english);
/* azimuth_update.c: the self-update, offered the server's latestHook / latestRelease. */
enum { UPDATE_NONE, UPDATE_AVAILABLE, UPDATE_DOWNLOADING, UPDATE_INSTALLED };
void azimuth_update_offer(const char *latest, const char *release);
int azimuth_update_state(void);
/* What the menu says about it (empty while there is no update). */
const wchar_t *azimuth_update_text(void);
void azimuth_account_test(wchar_t *out, size_t cap);
int azimuth_account_banner(char *out, size_t cap, const char **color);

/* azimuth_net.c: an HSTRING made by Azimuth's WindowsCreateString. */
HRESULT azimuth_net_make_hstring(const wchar_t *text, void **out);
/* azimuth_freeroam.c: free roam online ([FreeRoam] Enabled=1). The filter may wrap a WinRT
   class's statics that Azimuth answered; it returns 1 if it did. */
void azimuth_apply_freeroam(unsigned char *game_image, const wchar_t *directory, const wchar_t *executable);
int azimuth_freeroam_filter(const wchar_t *name, const void *iid, void **out);
/* Cuts the game's traffic to the world it is in (the car database changed). */
void azimuth_freeroam_cut(void);
/* azimuth_world.c: the world this player hosts (HOSPEDAR MUNDO). */
void azimuth_world_set_directory(const wchar_t *directory);
int azimuth_world_hosting(void);
/* The relay's UDP port while this PC hosts (server.cfg Port), else 0. */
unsigned azimuth_world_port(void);
unsigned azimuth_world_players(void);
const wchar_t *azimuth_world_config_path(void);
int azimuth_world_start(const wchar_t **why);
int azimuth_world_listed(void);
void azimuth_world_left(void);
/* The SHA-256 of the game's car database (X-Fh2-CarDb), hashed on a thread at start. */
void azimuth_world_hash_car_db(const wchar_t *executable);
const wchar_t *azimuth_world_car_db_header(void);
/* Waits while the car database is still being hashed (at most `ms`). */
void azimuth_world_car_db_wait(unsigned ms);
/* The world a player is in (1, 0 none, -1 no answer), and choosing it for the next FREE ROAM
   ONLINE (ENTRAR NO MUNDO). */
int azimuth_world_of(unsigned long long xuid, char *host, size_t host_capacity, int *password, char *why,
                     size_t why_capacity);
int azimuth_world_join(unsigned long long host, const char *password, char *error, size_t error_capacity);
/* FIND WORLD: the next open world after the one of `after` (1 found, 0 none, -1 no answer). */
int azimuth_world_browse(unsigned long long after, unsigned long long *id, char *name, size_t name_capacity, char *host,
                         size_t host_capacity, unsigned *players, unsigned *max_players, int *friend_world);
void azimuth_world_on_request(const wchar_t *method, const wchar_t *url, const unsigned char *body, size_t size);
void azimuth_world_stop(void);
void azimuth_world_describe(char *visibility, size_t visibility_capacity, int *has_password,
                            unsigned *max_players, unsigned *port);
/* azimuth_expansions.c: Storm Island for profiles that have it (the server's /me lists it). */
void azimuth_apply_expansions(unsigned char *game_image);
void azimuth_expansions_granted(void);
/* azimuth_account.c: whether the online profile has Storm Island (the last reply, kept in the
   accounts file). */
int azimuth_account_storm_island(void);

/* azimuth_online_menu.c: Settings > INFINITY. */
void azimuth_apply_online_menu(unsigned char *game_image);
/* One tile of the INFINITY screen: heading (UTF-8, NULL = no tile), image or icon under
   Textures/Anthem/, photo = dark image tile with light text. */
typedef struct {
    const char *heading;
    const char *icon;
    int photo;
} InfinityTile;
/* The tiles of the INFINITY screen: 0-7 as before, 8 the small tile next to tile 1 and 9 the
   small tile next to tile 2 (tiles 1 and 2 are then small too; azimuth_ui.c). */
#define INFINITY_TILE_COUNT 10
/* The INFINITY_TILE_COUNT tiles for the profile's current state (azimuth_online_menu.c). */
const InfinityTile *azimuth_infinity_tiles(void);
/* azimuth_ui.c: the CREDITS + INFINITY tiles in Settings, the INFINITY screen and the
   pause menu's status line. */
void azimuth_apply_ui(unsigned char *game_image);
int azimuth_ui_open_infinity(void *page);
#endif
