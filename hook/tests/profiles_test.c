/* Host test for the profiles of adapters/azimuth_account.c (SWITCH PROFILE), built for Windows
   by tests/profiles_test.py and run there or under wine, on a temporary FH2Online_Accounts.ini
   and a fake server: the SHA-1 of tokens, a profile of this server saved under another address
   coming in (accounts/known), the list (the INI's and device/accounts'), switching to a saved
   profile, playing again one this PC created (device/recover), and at most three per server. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../adapters/azimuth_account.c"
#include "../sha256.c"
void log_append(const char *message) {
    fputs(message, stderr);
}
size_t text_length(const char *text) {
    return strlen(text);
}
const char *text_find(const char *text, size_t length, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= length; ++i) {
        if (!memcmp(text + i, needle, n)) return text + i;
    }
    return 0;
}
void memory_copy(void *destination, const void *source, size_t size) {
    memmove(destination, source, size);
}
int memory_equal(const void *left, const void *right, size_t size) {
    return !memcmp(left, right, size);
}
int text_equal_ignore_case(const char *left, const char *right) {
    return !_stricmp(left, right);
}
int path_join(wchar_t output[PATH_CAPACITY], const wchar_t *directory, const wchar_t *name) {
    _snwprintf(output, PATH_CAPACITY, L"%ls\\%ls", directory, name);
    return 1;
}
const wchar_t *azimuth_tr(const wchar_t *english) {
    return english;
}
const char *azimuth_tr8(const char *english) {
    return english;
}
const AzimuthOffsets *azimuth_offsets(const unsigned char *base) {
    (void)base;
    return 0;
}
const wchar_t *azimuth_net_server_name(void) {
    return L"127.0.0.1:8081";
}
int azimuth_net_proxy_enabled(void) {
    return 1;
}
void azimuth_cloud_start(const wchar_t *bin, const wchar_t *xuid, const wchar_t *token, const wchar_t *ini,
                         const wchar_t *section) {
    (void)bin, (void)xuid, (void)token, (void)ini, (void)section;
}
int azimuth_cloud_upload_offline(const wchar_t *bin, const wchar_t *token, const wchar_t *ini, const wchar_t *section,
                                 const char **why) {
    (void)bin, (void)token, (void)ini, (void)section, (void)why;
    return 1;
}
void azimuth_expansions_granted(void) {
}
int azimuth_cloud_stopped(void) {
    return 0;
}
int azimuth_update_state(void) {
    return 0;
}
void azimuth_update_offer(const char *latest, const char *release) {
    (void)latest, (void)release;
}
int azimuth_net_ping_reply(int timeout_ms, char *out, size_t cap) {
    (void)timeout_ms;
    snprintf(out, cap, "{}");
    return 1;
}

#define T1 "1111111111111111111111111111111111111111111111111111111111111111"
#define T9 "9999999999999999999999999999999999999999999999999999999999999999"
static int known_calls, recover_calls;
static char last_body[1024];

DWORD azimuth_net_call(const wchar_t *method, const wchar_t *path, const wchar_t *extra_header, const char *body,
                       char *out, size_t capacity) {
    (void)extra_header;
    snprintf(last_body, sizeof(last_body), "%s", body ? body : "");
    out[0] = 0;
    if (wcscmp(method, L"POST")) {
        return 404;
    }
    if (!wcscmp(path, L"/infinity/v1/accounts/known")) {
        ++known_calls;
        /* only T1's SHA-1 is this server's */
        snprintf(out, capacity, strstr(body, "07a1a50a6273e6bc2eb94d647810cdc5b275b924")
                                    ? "{\"accounts\":[{\"token\":\"07a1a50a6273e6bc2eb94d647810cdc5b275b924\",\"xuid\":\"3\",\"gamertag\":\"Toretto#24890\"}]}"
                                    : "{\"accounts\":[]}");
        return 200;
    }
    if (!wcscmp(path, L"/infinity/v1/device/accounts")) {
        snprintf(out, capacity,
                 "{\"accounts\":[{\"xuid\":\"3\",\"gamertag\":\"Toretto#24890\",\"suspended\":false},"
                 "{\"xuid\":\"7\",\"gamertag\":\"Old#24890\",\"suspended\":false}],\"max\":3}");
        return 200;
    }
    if (!wcscmp(path, L"/infinity/v1/device/recover")) {
        ++recover_calls;
        if (!strstr(body, "\"xuid\":\"7\"")) {
            snprintf(out, capacity, "{\"error\":\"This profile was not created on this PC.\"}");
            return 401;
        }
        snprintf(out, capacity, "{\"xuid\":\"7\",\"gamertag\":\"Old#24890\",\"token\":\"" T9 "\",\"webKey\":\"AAAA-BBBB-CCCC\"}");
        return 200;
    }
    if (!wcscmp(path, L"/infinity/v1/login")) {
        snprintf(out, capacity, "{\"xuid\":\"9\",\"gamertag\":\"Nine#24890\",\"token\":\"" T9 "\",\"webKey\":\"NINE-NINE-NINE\"}");
        return 200;
    }
    return 404;
}
DWORD azimuth_net_call_quick(const wchar_t *method, const wchar_t *path, const wchar_t *extra_header, const char *body,
                             char *out, size_t capacity, int timeout_ms) {
    (void)timeout_ms;
    return azimuth_net_call(method, path, extra_header, body, out, capacity);
}

static int fails;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

static int ini_is(const wchar_t *section, const wchar_t *key, const wchar_t *want) {
    wchar_t value[128];
    GetPrivateProfileStringW(section, key, L"", value, 128, account_file);
    if (wcscmp(value, want)) {
        fprintf(stderr, "  [%ls] %ls = '%ls', expected '%ls'\n", section, key, value, want);
        return 0;
    }
    return 1;
}

static int waiting_sections(void) {
    int n = 0;
    for (const wchar_t *name = account_section_names(); *name; name += wide_len(name) + 1) {
        n += record_is_mine(name);
    }
    return n;
}

int main(void) {
    /* SHA-1: the standard vectors, and two blocks (a 64-character token) */
    char hex[41];
    account_sha1_hex("abc", hex);
    CHECK(!strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    account_sha1_hex("", hex);
    CHECK(!strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    account_sha1_hex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", hex);
    CHECK(!strcmp(hex, "0098ba824b5c16427bd7a1122a5a442a25ec644d"));

    /* a PC that played this server at 127.0.0.1 and at the public one, now at its LAN address */
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    _snwprintf(account_file, PATH_CAPACITY, L"%lsfh2_profiles_test_%lu.ini", dir, GetCurrentProcessId());
    DeleteFileW(account_file);
    WritePrivateProfileStringW(L"127.0.0.1:8081", L"Token", L"" T1, account_file);
    WritePrivateProfileStringW(L"127.0.0.1:8081", L"Xuid", L"3", account_file);
    WritePrivateProfileStringW(L"127.0.0.1:8081", L"Gamertag", L"Toretto#24890", account_file);
    WritePrivateProfileStringW(L"127.0.0.1:8081", L"WebKey", L"K7QM-4XPA-9RT2", account_file);
    WritePrivateProfileStringW(L"127.0.0.1:8081", L"Online", L"1", account_file);
    WritePrivateProfileStringW(L"127.0.0.1:8081", L"SaveCommit", L"abc", account_file);
    WritePrivateProfileStringW(L"infinity.anarqz.dev:443", L"Token", L"2222222222222222222222222222222222222222222222222222222222222222", account_file);
    WritePrivateProfileStringW(L"infinity.anarqz.dev:443", L"Xuid", L"3", account_file);
    WritePrivateProfileStringW(L"infinity.anarqz.dev:443", L"Gamertag", L"Toretto#24890", account_file);
    account_server = L"198.51.100.40:8081";
    memcpy(account_device, "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", 65);
    account_available = 1;
    account_load_active();
    CHECK(!account_token[0]);
    CHECK(account_other_tokens() == 2);

    /* the server knows 127.0.0.1's token (by its SHA-1, the tokens never sent): it comes in */
    CHECK(account_adopt());
    CHECK(known_calls == 1 && !strstr(last_body, T1) && strstr(last_body, "07a1a50a6273e6bc2eb94d647810cdc5b275b924"));
    CHECK(ini_is(L"198.51.100.40:8081", L"Token", L"" T1) && ini_is(L"198.51.100.40:8081", L"Xuid", L"3"));
    CHECK(ini_is(L"198.51.100.40:8081", L"SaveCommit", L"abc") && ini_is(L"198.51.100.40:8081", L"Online", L"1"));
    CHECK(ini_is(L"127.0.0.1:8081", L"Token", L"" T1)); /* the old address keeps it */
    CHECK(!wcscmp(account_gamertag, L"Toretto#24890"));
    CHECK(account_adopt() && waiting_sections() == 0); /* nothing twice */

    /* a second profile waiting, and the server's list of this PC's profiles */
    WritePrivateProfileStringW(L"198.51.100.40:8081|5", L"Token", L"5555555555555555555555555555555555555555555555555555555555555555", account_file);
    WritePrivateProfileStringW(L"198.51.100.40:8081|5", L"Xuid", L"5", account_file);
    WritePrivateProfileStringW(L"198.51.100.40:8081|5", L"Gamertag", L"Anarqz#24890", account_file);
    WritePrivateProfileStringW(L"198.51.100.40:8081|5", L"WebKey", L"ANAR-QZQZ-2489", account_file);
    WritePrivateProfileStringW(L"198.51.100.40:8081|5", L"SaveCommit", L"five", account_file);
    account_active = 1;
    AccountProfile list[ACCOUNT_PROFILES_MAX];
    int n = azimuth_account_profiles(list, ACCOUNT_PROFILES_MAX, 1);
    CHECK(n == 3);
    CHECK(n >= 1 && !wcscmp(list[0].gamertag, L"Toretto#24890") && list[0].current == 2 && list[0].saved && list[0].made_here);
    CHECK(n >= 2 && !wcscmp(list[1].gamertag, L"Anarqz#24890") && !list[1].current && list[1].saved &&
          !wcscmp(list[1].web_key, L"ANAR-QZQZ-2489") && !list[1].made_here);
    CHECK(n >= 3 && !wcscmp(list[2].gamertag, L"Old#24890") && !list[2].saved && list[2].made_here);
    CHECK(azimuth_account_saved_profiles() == 2);

    /* to the saved one: sections swap, each with its own SaveCommit */
    wchar_t message[400];
    CHECK(azimuth_account_switch(&list[1], message, 400));
    CHECK(ini_is(L"198.51.100.40:8081", L"Xuid", L"5") && ini_is(L"198.51.100.40:8081", L"SaveCommit", L"five"));
    CHECK(ini_is(L"198.51.100.40:8081|3", L"Token", L"" T1) && ini_is(L"198.51.100.40:8081|3", L"SaveCommit", L"abc"));
    CHECK(ini_is(L"198.51.100.40:8081|5", L"Xuid", L""));
    CHECK(!wcscmp(account_gamertag, L"Anarqz#24890") && waiting_sections() == 1);

    /* to the one this PC created but is not signed in to: device/recover, a new token */
    CHECK(azimuth_account_switch(&list[2], message, 400));
    CHECK(recover_calls == 1 && strstr(last_body, "\"device\":\"dddd") != 0);
    CHECK(ini_is(L"198.51.100.40:8081", L"Xuid", L"7") && ini_is(L"198.51.100.40:8081", L"Token", L"" T9));
    CHECK(ini_is(L"198.51.100.40:8081", L"SaveCommit", L"") && ini_is(L"198.51.100.40:8081", L"WebKey", L"AAAA-BBBB-CCCC"));
    CHECK(ini_is(L"198.51.100.40:8081|5", L"SaveCommit", L"five") && waiting_sections() == 2);

    /* a profile neither saved nor made here cannot be played without its key */
    AccountProfile stranger = {0};
    wcscpy(stranger.xuid, L"8");
    wcscpy(stranger.gamertag, L"Friend#11111");
    CHECK(!azimuth_account_switch(&stranger, message, 400) && message[0]);

    /* a fourth (signed in with a key): three at most, one waiting is forgotten here */
    CHECK(azimuth_account_log_in(L"Nine#24890", L"NINE-NINE-NINE", message, 400));
    CHECK(ini_is(L"198.51.100.40:8081", L"Xuid", L"9") && waiting_sections() == 2);
    CHECK(azimuth_account_profiles(list, ACCOUNT_PROFILES_MAX, 0) == 3);
    CHECK(ini_is(L"198.51.100.40:8081|7", L"Token", L"" T9)); /* the one just used stays */

    /* in use already, and this server's profile saved under another address: the list brings it in */
    WritePrivateProfileStringW(L"localhost:8081", L"Token", L"" T1, account_file);
    WritePrivateProfileStringW(L"localhost:8081", L"Xuid", L"3", account_file);
    WritePrivateProfileStringW(L"localhost:8081", L"Gamertag", L"Toretto#24890", account_file);
    WritePrivateProfileStringW(L"198.51.100.40:8081|3", 0, 0, account_file);
    n = azimuth_account_profiles(list, ACCOUNT_PROFILES_MAX, 1);
    int toretto = 0;
    for (int i = 0; i < n; ++i) {
        toretto += !wcscmp(list[i].gamertag, L"Toretto#24890") && list[i].saved;
    }
    CHECK(toretto == 1 && ini_is(L"198.51.100.40:8081", L"Xuid", L"9"));

    DeleteFileW(account_file);
    printf(fails ? "profiles_test: %d failure(s)\n" : "profiles_test: ok\n", fails);
    return fails != 0;
}
