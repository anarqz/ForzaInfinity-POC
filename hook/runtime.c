#include "runtime.h"
#include "features.h"
#include "features/video_settings.h"
#include "logging.h"
#include "patch_engine.h"
#include "settings.h"
#include "support.h"

#ifdef FH2_AZIMUTH
#include "adapters/azimuth_host.h"
#endif

static void log_enabled_features(const wchar_t *settings_directory, const ModOptions *options) {
    log_set_directory(settings_directory);
    if (options->garage_from_pause) {
        log_append("GARAGE: XML parse hook installed; awaiting menu-flow resources.\r\n");
    }
    if (options->skip_ineligible_car_dialogue) {
        log_append("FRONTEND: race dialogue skip enabled; awaiting race activation "
                   "flow.\r\n");
    }
    if (options->skip_car_meet_on_startup) {
        log_append("STARTUP: mainland car-meet boot skipped; Storm Island retains "
                   "normal boot.\r\n");
    }
    if (options->spawn_at_festival && options->skip_car_meet_on_startup) {
        log_append("STARTUP: mainland festival spawn enabled; Storm Island retains "
                   "normal boot.\r\n");
    }
    if (options->media_overrides) {
        log_append("FILES: native override hook installed; folders are mounted on "
                   "the first "
                   "file lookup outside DLL initialization.\r\n");
    }
    if (options->autodrive) {
        log_append("AUTODRIVE: experimental F2 GPS/AI hooks installed; cinematic "
                   "delay is 3 "
                   "seconds.\r\n");
    }
    if (options->all_rims_category) {
        log_append("UPGRADES: virtual All Rims category enabled; database file "
                   "unchanged.\r\n");
    }
    log_append("VEHICLE: right-stick press toggles headlights; stationary "
               "2-second hold toggles "
               "supported roofs.\r\n");
    log_append("SETTINGS: Video and HUD menu hooks installed; awaiting menu "
               "resources.\r\n");
    if (options->custom_gamerpic) {
        log_append("GAMERPIC: converter hook installed; Gamerpic.png is read from "
                   "the settings "
                   "folder on the first local-player request.\r\n");
    }
}

int runtime_initialize(unsigned char *game_image, const wchar_t *executable_path,
                       const wchar_t *settings_directory) {
    ModOptions options;
    settings_defaults(&options);
    gamerpic_set_directory(settings_directory);
    log_reset();
#ifdef FH2_AZIMUTH
    azimuth_prepare_ini(settings_directory);
#endif
    int initialized = settings_read(settings_directory, &options);
    if (initialized) {
        video_settings_load(settings_directory);
        video_settings_seed(&options);
        video_settings_apply(&options);
    }
    if (initialized && options.enabled) {
        initialized = verify_game_file(executable_path);
    }
    if (initialized) {
        initialized = patch_engine_apply(game_image, &options);
    }
    log_start(settings_directory, &options);
#ifdef FH2_AZIMUTH
    if (initialized && options.enabled) {
        log_set_directory(settings_directory);
        azimuth_log_ini();
        azimuth_apply_livery_compatibility();
        azimuth_apply_save_compatibility(options.disable_azimuth_save_checksums);
        azimuth_apply_network(settings_directory);
        /* the car database's SHA-256 goes with every request (the server only lets the game's
           own cars play online) */
        if (azimuth_net_proxy_enabled()) {
            azimuth_world_hash_car_db(executable_path);
        }
        azimuth_apply_freeroam(game_image, settings_directory, executable_path);
        azimuth_apply_account(settings_directory);
        azimuth_apply_ui(game_image);
        azimuth_apply_online_menu(game_image);
        azimuth_apply_expansions(game_image);
        azimuth_apply_events(game_image);
        azimuth_apply_economy_probe(game_image);
        azimuth_apply_no_game_clips(game_image);
        azimuth_apply_pad();
    }
#endif
    if (initialized && options.enabled) {
        log_enabled_features(settings_directory, &options);
    }
    return initialized;
}

#ifdef FH2_TWEAKS_TEST
__declspec(dllexport) int Fh2TestInitialize(void *base, const wchar_t *path,
                                            const wchar_t *directory) {
    return runtime_initialize(base, path, directory);
}

__declspec(dllexport) int Fh2TestVerifyFile(const wchar_t *path) {
    return verify_game_file(path);
}

__declspec(dllexport) int Fh2TestReadOptions(const wchar_t *directory, ModOptions *options) {
    return settings_read(directory, options);
}
#endif
