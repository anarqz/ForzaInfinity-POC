/* FH2Online account: "name#12345" on the configured server.

   The player creates it from the game (Settings > Infinity, azimuth_online_menu.c):
   the 5 digits come from a fingerprint of this PC (SHA-256 of the Windows MachineGuid and the
   serial of the volume holding the mod). Only those 5 digits and a per-server device id
   (SHA-256 of the fingerprint + "host:port", so servers cannot match players up) leave the
   PC. The server answers with an XUID, a device token and a web key (for the site); they are
   kept in FH2Online_Accounts.ini, one section per server, with Online=1/0 (whether the game
   starts in the online profile). The network layer sends X-Fh2-Token on every request.

   When the game starts online, it sees the account through Azimuth's LocalUser:
   - XboxUserId (0.9.7: vtable slot 0x24ff88; every build in azimuth_offsets.c; Azimuth formats a fixed 0x9000000000000) returns the
     account XUID; Azimuth then keeps a separate save (User_<xuid>), so the online profile
     starts from zero and the offline save is left alone;
   - the gamertag / display names (LocalDisplayInfo slots 0x24ffe8, 0x24fff8, 0x250000) are
     read from Azimuth's player-name std::wstring (RVA 0x318c60), which is rewritten in place.
   The token is checked on a thread, which then syncs the cloud save (azimuth_cloud.c); those
   getters wait for it (at most 30 s), so the game never caches the old identity nor opens the
   save before it is synced. Joining or leaving the online profile restarts the game.

   Another PC: Settings > Infinity > "Sign in" takes Name#12345 and the
   site key; the server issues a new token and the PC that had the profile is logged out (its
   next start finds the token refused, forgets it and says so in the menu). */
#include "azimuth_host.h"
#include "azimuth_offsets.h"
#include "logging.h"
#include "sha256.h"
#include "support.h"

#define ACCOUNT_FILE L"FH2Online_Accounts.ini"
#define ACCOUNT_WAIT_MS 30000u

typedef HRESULT (*AccountGetterFn)(void *, void **);
typedef HRESULT (*AccountHstringCreateFn)(const wchar_t *, unsigned, void **);
typedef BOOL(WINAPI *WritePrivateProfileStringWFn)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
typedef BOOL(WINAPI *GetVolumeInformationWFn)(LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD,
                                               LPWSTR, DWORD);
typedef LONG(WINAPI *RegGetValueWFn)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
typedef void(WINAPI *SleepFn)(DWORD);
typedef BOOL(WINAPI *CreateProcessWFn)(LPCWSTR, LPWSTR, void *, void *, BOOL, DWORD, void *, LPCWSTR,
                                       STARTUPINFOW *, PROCESS_INFORMATION *);

/* The getters' slots and the player-name string for this Azimuth build (azimuth_offsets.c). */
static const AzimuthOffsets *account_offsets;
static AccountGetterFn account_info_original[3];
static AccountGetterFn account_xuid_original;

static unsigned char *account_host;
static wchar_t account_directory[PATH_CAPACITY], account_file[PATH_CAPACITY];
static const wchar_t *account_server;
static volatile LONG account_ready = 1; /* token check finished (or not needed) */
static volatile LONG account_link;      /* server: 0 not checked yet, 1 answering, 2 not answering */
static int account_available;           /* fingerprint and paths ready: the menu can work */
static volatile LONG account_storm_island; /* the online profile has Storm Island */
#define STORM_ISLAND_PRODUCT "06f06bff-35ee-4821-a289-fbc1765d6b76"
static int account_active;              /* this session plays as the account */
static wchar_t account_token[72];
static wchar_t account_xuid[24];
static wchar_t account_gamertag[32]; /* Name#12345: up to 20 + 6 */
static wchar_t account_web_key[24];
static wchar_t account_online[4];
static char account_discriminator[6];
static wchar_t account_discriminator_wide[6];
static char account_device[65];
static wchar_t account_notice[480]; /* shown by the menu, e.g. "logged in elsewhere" */
static SleepFn account_sleep;

/* ------------------------------------------------------------------ text */

static size_t wide_len(const wchar_t *text) {
    size_t n = 0;
    while (text && text[n]) {
        ++n;
    }
    return n;
}

static void hex_text(const unsigned char *data, size_t size, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    out[2 * size] = 0;
}

/* Appends ASCII `text` to `out` (capacity `cap`, kept NUL-terminated). */
static void text_add(char *out, size_t cap, const char *text) {
    size_t n = text_length(out);
    while (*text && n + 1 < cap) {
        out[n++] = *text++;
    }
    out[n] = 0;
}

static void wide_to_ascii(const wchar_t *in, char *out, size_t cap) {
    size_t n = 0;
    while (in && in[n] && n + 1 < cap) {
        out[n] = in[n] < 128 ? (char)in[n] : '?';
        ++n;
    }
    out[n] = 0;
}

/* UTF-8 (server messages) to UTF-16. */
static void utf8_to_wide(const char *in, wchar_t *out, size_t cap) {
    out[0] = 0;
    if (in && *in) {
        int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, (int)cap);
        out[n > 0 ? n - 1 : 0] = 0;
    }
}

/* The string value of "key" in a flat JSON object (no nesting), escapes \" and \\ only. */
static int json_string(const char *json, const char *key, char *out, size_t cap) {
    size_t key_length = text_length(key);
    out[0] = 0;
    for (const char *p = json; p && *p; ++p) {
        if (*p != '"' || !memory_equal(p + 1, key, key_length) || p[1 + key_length] != '"') {
            continue;
        }
        p += key_length + 2;
        while (*p == ' ' || *p == ':') {
            ++p;
        }
        if (*p != '"') {
            return 0;
        }
        size_t n = 0;
        for (++p; *p && *p != '"'; ++p) {
            char c = *p;
            if (c == '\\' && p[1]) {
                c = *++p == 'n' ? '\n' : *p;
            }
            if (n + 1 < cap) {
                out[n++] = c;
            }
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

/* 1 to 20 ASCII letters or digits starting with a letter (the server checks it too). */
int azimuth_account_valid_name(const wchar_t *name) {
    size_t n = wide_len(name);
    if (n < 1 || n > 20 || !((name[0] >= L'A' && name[0] <= L'Z') || (name[0] >= L'a' && name[0] <= L'z'))) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        wchar_t c = name[i];
        if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))) {
            return 0;
        }
    }
    return 1;
}

static void account_log(const char *prefix, const char *detail) {
    static char line[640];
    line[0] = 0;
    text_add(line, sizeof(line), "ACCOUNT: ");
    text_add(line, sizeof(line), prefix);
    text_add(line, sizeof(line), detail);
    text_add(line, sizeof(line), "\r\n");
    log_append(line);
}

/* ----------------------------------------------------------- fingerprint */

static int account_fingerprint(void) {
    static wchar_t guid[64];
    static char material[256];
    unsigned char full[32], device[32];
    DWORD size = sizeof(guid), serial = 0;
    HMODULE advapi = LoadLibraryExW(L"advapi32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32);
    RegGetValueWFn reg_get = advapi ? (RegGetValueWFn)(uintptr_t)GetProcAddress(advapi, "RegGetValueW") : 0;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    GetVolumeInformationWFn volume =
        kernel ? (GetVolumeInformationWFn)(uintptr_t)GetProcAddress(kernel, "GetVolumeInformationW") : 0;
    guid[0] = 0;
    /* RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY */
    if (!reg_get || reg_get(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                            0x00000002 | 0x00010000, 0, guid, &size) != ERROR_SUCCESS) {
        log_append("ACCOUNT: MachineGuid unavailable; the fingerprint uses the volume only.\r\n");
        guid[0] = 0;
    }
    wchar_t root[4] = {account_directory[0], L':', L'\\', 0};
    if (!volume || !volume(root, 0, 0, &serial, 0, 0, 0, 0)) {
        serial = 0;
    }
    if (!guid[0] && !serial) {
        return 0;
    }
    char guid_ascii[64], serial_hex[9];
    wide_to_ascii(guid, guid_ascii, sizeof(guid_ascii));
    unsigned char serial_bytes[4] = {(unsigned char)(serial >> 24), (unsigned char)(serial >> 16),
                                     (unsigned char)(serial >> 8), (unsigned char)serial};
    hex_text(serial_bytes, 4, serial_hex);
    material[0] = 0;
    text_add(material, sizeof(material), "FH2Online/v1|");
    text_add(material, sizeof(material), guid_ascii);
    text_add(material, sizeof(material), "|");
    text_add(material, sizeof(material), serial_hex);
    Sha256Context hash;
    sha256_initialize(&hash);
    sha256_update(&hash, (const unsigned char *)material, text_length(material));
    sha256_finalize(&hash, full);

    unsigned long long number = 0;
    for (int i = 0; i < 8; ++i) {
        number = number << 8 | full[i];
    }
    number %= 100000u;
    for (int i = 4; i >= 0; --i) {
        account_discriminator[i] = (char)('0' + number % 10);
        account_discriminator_wide[i] = (wchar_t)account_discriminator[i];
        number /= 10;
    }
    account_discriminator[5] = 0;
    account_discriminator_wide[5] = 0;

    /* per-server device id: SHA-256(hex(fingerprint) | host:port) */
    char full_hex[65], server[300];
    hex_text(full, 32, full_hex);
    wide_to_ascii(account_server, server, sizeof(server));
    material[0] = 0;
    text_add(material, sizeof(material), full_hex);
    text_add(material, sizeof(material), "|");
    text_add(material, sizeof(material), server);
    sha256_initialize(&hash);
    sha256_update(&hash, (const unsigned char *)material, text_length(material));
    sha256_finalize(&hash, device);
    hex_text(device, 32, account_device);
    return 1;
}

/* ---------------------------------------------------------- local storage */

static int account_write(const wchar_t *key, const wchar_t *value) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    WritePrivateProfileStringWFn write =
        kernel ? (WritePrivateProfileStringWFn)(uintptr_t)GetProcAddress(kernel, "WritePrivateProfileStringW") : 0;
    return write && write(account_server, key, value, account_file);
}

static void account_save(void) {
    if (!account_write(L"Token", account_token) || !account_write(L"Xuid", account_xuid) ||
        !account_write(L"Gamertag", account_gamertag) || !account_write(L"WebKey", account_web_key) ||
        !account_write(L"Online", account_online)) {
        log_append("ACCOUNT: could not write FH2Online_Accounts.ini.\r\n");
    }
}

/* ------------------------------------------------- profiles on this PC */
/* A PC keeps up to ACCOUNT_PROFILES_MAX profiles per server (the server lets a PC create as
   many). The one in use is the [host:port] section, as always (older hooks and the cloud save
   read it); the others wait in [host:port|<xuid>] sections with the same keys, so switching
   moves whole sections and each profile keeps its own SaveCommit. A profile signed in on
   another PC loses its token here but stays listed (with its name and last site key), and the
   PC that created it can play it again without the key (POST device/recover).
   The same server reached at another address (127.0.0.1, its LAN address, a name) keeps the
   profiles: a PC with none for the address asks the server which of its other sections'
   tokens it knows, by their SHA-1 (POST accounts/known; the tokens themselves never go to
   another server), and those are copied in. */
#define ACCOUNT_PROFILE_KEYS 6
#define ACCOUNT_SECTIONS_CAP 8192u
static const wchar_t *const account_record_keys[ACCOUNT_PROFILE_KEYS] = {L"Token",  L"Xuid",       L"Gamertag",
                                                                         L"WebKey", L"SaveCommit", L"StormIsland"};
typedef struct {
    wchar_t values[ACCOUNT_PROFILE_KEYS][72];
} AccountRecord;
static wchar_t account_sections[ACCOUNT_SECTIONS_CAP];

static int account_write_in(const wchar_t *section, const wchar_t *key, const wchar_t *value) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    WritePrivateProfileStringWFn write =
        kernel ? (WritePrivateProfileStringWFn)(uintptr_t)GetProcAddress(kernel, "WritePrivateProfileStringW") : 0;
    return write && write(section, key, value, account_file);
}

static void record_read(const wchar_t *section, AccountRecord *record) {
    for (unsigned i = 0; i < ACCOUNT_PROFILE_KEYS; ++i) {
        GetPrivateProfileStringW(section, account_record_keys[i], L"", record->values[i], 72, account_file);
    }
}

static void record_write(const wchar_t *section, const AccountRecord *record) {
    for (unsigned i = 0; i < ACCOUNT_PROFILE_KEYS; ++i) {
        account_write_in(section, account_record_keys[i], record->values[i]);
    }
}

/* [host:port|<xuid>] */
static int record_section(const wchar_t *xuid, wchar_t *out, size_t cap) {
    size_t s = wide_len(account_server), x = wide_len(xuid);
    if (!x || s + x + 2 > cap) {
        return 0;
    }
    memory_copy(out, account_server, s * sizeof(wchar_t));
    out[s] = L'|';
    memory_copy(out + s + 1, xuid, (x + 1) * sizeof(wchar_t));
    return 1;
}

static int wide_equal(const wchar_t *a, const wchar_t *b) {
    size_t n = wide_len(a);
    return n == wide_len(b) && memory_equal(a, b, n * sizeof(wchar_t));
}

/* Every section name of FH2Online_Accounts.ini, one after another (NUL each, a final NUL). */
static const wchar_t *account_section_names(void) {
    account_sections[0] = account_sections[1] = 0;
    GetPrivateProfileStringW(0, 0, L"", account_sections, ACCOUNT_SECTIONS_CAP, account_file);
    return account_sections;
}

/* `name` is one of this server's saved profiles ([host:port|xuid]). */
static int record_is_mine(const wchar_t *name) {
    size_t s = wide_len(account_server);
    return wide_len(name) > s + 1 && memory_equal(name, account_server, s * sizeof(wchar_t)) && name[s] == L'|';
}

/* The profile in use goes to its waiting section (before another takes its place). At most
   ACCOUNT_PROFILES_MAX - 1 wait: one more, and the first one found is forgotten here (it
   stays on the server). */
static void record_stash(const AccountRecord *active) {
    wchar_t section[320];
    if (!active->values[1][0] || !record_section(active->values[1], section, ARRAY_COUNT(section))) {
        return;
    }
    record_write(section, active);
    int waiting = 0;
    const wchar_t *first = 0;
    for (const wchar_t *name = account_section_names(); *name; name += wide_len(name) + 1) {
        if (record_is_mine(name) && !wide_equal(name, section)) {
            ++waiting;
            first = first ? first : name;
        }
    }
    if (waiting >= ACCOUNT_PROFILES_MAX - 1 && first) {
        account_write_in(first, 0, 0);
    }
}

/* The globals again from the [host:port] section (after a section moved in or back). */
static void account_load_active(void) {
    GetPrivateProfileStringW(account_server, L"Token", L"", account_token, ARRAY_COUNT(account_token), account_file);
    GetPrivateProfileStringW(account_server, L"Xuid", L"", account_xuid, ARRAY_COUNT(account_xuid), account_file);
    GetPrivateProfileStringW(account_server, L"Gamertag", L"", account_gamertag, ARRAY_COUNT(account_gamertag),
                             account_file);
    GetPrivateProfileStringW(account_server, L"WebKey", L"", account_web_key, ARRAY_COUNT(account_web_key),
                             account_file);
    GetPrivateProfileStringW(account_server, L"Online", L"1", account_online, ARRAY_COUNT(account_online),
                             account_file);
}

/* SHA-1 (the server keeps device tokens by their SHA-1 hex). */
static unsigned account_rol(unsigned v, int n) {
    return v << n | v >> (32 - n);
}

static void account_sha1_hex(const char *text, char out[41]) {
    unsigned h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    size_t length = text_length(text);
    unsigned char block[64];
    unsigned long long bits = (unsigned long long)length * 8;
    size_t total = (length + 9 + 63) / 64 * 64;
    for (size_t offset = 0; offset < total; offset += 64) {
        for (size_t i = 0; i < 64; ++i) {
            size_t at = offset + i;
            block[i] = at < length ? (unsigned char)text[at]
                       : at == length ? 0x80
                       : at >= total - 8 ? (unsigned char)(bits >> (8 * (total - 1 - at)))
                                          : 0;
        }
        unsigned w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (unsigned)block[4 * i] << 24 | (unsigned)block[4 * i + 1] << 16 | (unsigned)block[4 * i + 2] << 8 |
                   block[4 * i + 3];
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = account_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        unsigned a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            unsigned f, k;
            if (i < 20) {
                f = (b & c) | (~b & d), k = 0x5a827999u;
            } else if (i < 40) {
                f = b ^ c ^ d, k = 0x6ed9eba1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdcu;
            } else {
                f = b ^ c ^ d, k = 0xca62c1d6u;
            }
            unsigned t = account_rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = account_rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    unsigned char digest[20];
    for (int i = 0; i < 20; ++i) {
        digest[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
    }
    hex_text(digest, 20, out);
}

/* 64 hex digits, as the server makes tokens. */
static int account_token_shape(const wchar_t *token) {
    if (wide_len(token) != 64) {
        return 0;
    }
    for (int i = 0; i < 64; ++i) {
        wchar_t c = token[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) {
            return 0;
        }
    }
    return 1;
}

/* Sections of other addresses with a token: the candidates account_adopt asks about. */
static int account_other_tokens(void) {
    int found = 0;
    for (const wchar_t *name = account_section_names(); *name; name += wide_len(name) + 1) {
        wchar_t token[72];
        if (wide_equal(name, account_server) || record_is_mine(name)) {
            continue;
        }
        GetPrivateProfileStringW(name, L"Token", L"", token, ARRAY_COUNT(token), account_file);
        found += account_token_shape(token);
    }
    return found;
}

/* The profiles of this server saved under another address are copied in (the first, when
   this address had none, becomes the one in use, with its Online). 1 when one is in use now. */
static int account_adopt(void) {
    static char body[16 * 46 + 32], reply[4096];
    static wchar_t names[16][320];
    static char hashes[16][41];
    int count = 0;
    body[0] = 0;
    text_add(body, sizeof(body), "{\"tokens\":[");
    for (const wchar_t *name = account_section_names(); *name && count < 16; name += wide_len(name) + 1) {
        wchar_t token[72];
        char ascii[72];
        if (wide_equal(name, account_server) || record_is_mine(name) || wide_len(name) >= 320) {
            continue;
        }
        GetPrivateProfileStringW(name, L"Token", L"", token, ARRAY_COUNT(token), account_file);
        if (!account_token_shape(token)) {
            continue;
        }
        wide_to_ascii(token, ascii, sizeof(ascii));
        account_sha1_hex(ascii, hashes[count]);
        memory_copy(names[count], name, (wide_len(name) + 1) * sizeof(wchar_t));
        text_add(body, sizeof(body), count ? ",\"" : "\"");
        text_add(body, sizeof(body), hashes[count]);
        text_add(body, sizeof(body), "\"");
        ++count;
    }
    text_add(body, sizeof(body), "]}");
    if (!count || azimuth_net_call_quick(L"POST", L"/infinity/v1/accounts/known", 0, body, reply, sizeof(reply), 4000) !=
                      200) {
        return account_token[0] != 0;
    }
    size_t length = text_length(reply);
    for (int i = 0; i < count; ++i) {
        if (!text_find(reply, length, hashes[i])) {
            continue;
        }
        AccountRecord record;
        wchar_t online[4], section[320], have[24];
        char tag[32];
        record_read(names[i], &record);
        GetPrivateProfileStringW(names[i], L"Online", L"1", online, ARRAY_COUNT(online), account_file);
        /* already here (in use, or waiting) */
        GetPrivateProfileStringW(account_server, L"Xuid", L"", have, ARRAY_COUNT(have), account_file);
        if (wide_equal(have, record.values[1]) ||
            (record_section(record.values[1], section, ARRAY_COUNT(section)) &&
             GetPrivateProfileStringW(section, L"Xuid", L"", have, ARRAY_COUNT(have), account_file))) {
            continue;
        }
        if (!account_token[0]) {
            record_write(account_server, &record);
            account_write(L"Online", online);
            account_load_active();
        } else {
            record_stash(&record);
        }
        wide_to_ascii(record.values[2], tag, sizeof(tag));
        account_log("this server's profile, saved under another address, is kept here too: ", tag);
    }
    return account_token[0] != 0;
}

/* The profiles this PC created on the server (any address it had), by its device id: for the
   SWITCH PROFILE list, and to tell a PC with none saved that it has some. */
static DWORD account_device_profiles(char *reply, size_t cap, int timeout_ms) {
    static char body[128];
    body[0] = 0;
    text_add(body, sizeof(body), "{\"device\":\"");
    text_add(body, sizeof(body), account_device);
    text_add(body, sizeof(body), "\"}");
    return azimuth_net_call_quick(L"POST", L"/infinity/v1/device/accounts", 0, body, reply, cap, timeout_ms);
}

static int profile_find(const AccountProfile *list, int count, const wchar_t *xuid) {
    for (int i = 0; i < count; ++i) {
        if (wide_equal(list[i].xuid, xuid)) {
            return i;
        }
    }
    return -1;
}

static void profile_copy(wchar_t *out, size_t cap, const wchar_t *in) {
    size_t n = 0;
    while (in && in[n] && n + 1 < cap) {
        out[n] = in[n];
        ++n;
    }
    out[n] = 0;
}

int azimuth_account_profiles(AccountProfile *out, int cap, int ask_server) {
    int count = 0;
    if (!account_available || cap <= 0) {
        return 0;
    }
    /* this server's profiles saved under other addresses join the list (waiting) */
    if (ask_server && account_other_tokens()) {
        account_adopt();
    }
    if (account_xuid[0] && account_gamertag[0]) {
        AccountProfile *p = &out[count++];
        memory_copy(p, &(AccountProfile){0}, sizeof(*p));
        profile_copy(p->xuid, ARRAY_COUNT(p->xuid), account_xuid);
        profile_copy(p->gamertag, ARRAY_COUNT(p->gamertag), account_gamertag);
        profile_copy(p->web_key, ARRAY_COUNT(p->web_key), account_web_key);
        p->current = account_active ? 2 : 1;
        p->saved = account_token[0] != 0;
    }
    for (const wchar_t *name = account_section_names(); *name && count < cap; name += wide_len(name) + 1) {
        if (!record_is_mine(name)) {
            continue;
        }
        AccountRecord record;
        record_read(name, &record);
        if (!record.values[1][0] || !record.values[2][0] || profile_find(out, count, record.values[1]) >= 0) {
            continue;
        }
        AccountProfile *p = &out[count++];
        memory_copy(p, &(AccountProfile){0}, sizeof(*p));
        profile_copy(p->xuid, ARRAY_COUNT(p->xuid), record.values[1]);
        profile_copy(p->gamertag, ARRAY_COUNT(p->gamertag), record.values[2]);
        profile_copy(p->web_key, ARRAY_COUNT(p->web_key), record.values[3]);
        p->saved = account_token_shape(record.values[0]);
    }
    static char reply[2048];
    if (!ask_server || account_device_profiles(reply, sizeof(reply), 2500) != 200) {
        return count;
    }
    size_t length = text_length(reply);
    for (const char *at = reply; (at = text_find(at, length - (size_t)(at - reply), "{\"xuid\"")) != 0; ++at) {
        char xuid[24], tag[32];
        wchar_t wide_xuid[24];
        if (!json_string(at, "xuid", xuid, sizeof(xuid)) || !json_string(at, "gamertag", tag, sizeof(tag))) {
            continue;
        }
        const char *end = text_find(at, length - (size_t)(at - reply), "}");
        int suspended = end && text_find(at, (size_t)(end - at), "\"suspended\":true") != 0;
        utf8_to_wide(xuid, wide_xuid, ARRAY_COUNT(wide_xuid));
        int i = profile_find(out, count, wide_xuid);
        if (i < 0 && count < cap) {
            i = count++;
            memory_copy(&out[i], &(AccountProfile){0}, sizeof(out[i]));
            profile_copy(out[i].xuid, ARRAY_COUNT(out[i].xuid), wide_xuid);
            utf8_to_wide(tag, out[i].gamertag, ARRAY_COUNT(out[i].gamertag));
        }
        if (i >= 0) {
            out[i].made_here = 1;
            out[i].suspended = suspended;
        }
    }
    return count;
}

static void account_drop_waiting(const wchar_t *xuid);

int azimuth_account_saved_profiles(void) {
    AccountProfile list[ACCOUNT_PROFILES_MAX];
    return azimuth_account_profiles(list, ACCOUNT_PROFILES_MAX, 0);
}

/* -------------------------------------------------------- identity patch */

/* Rewrites Azimuth's player-name std::wstring (ptr/inline buffer, size +0x10, capacity
   +0x18) in place; the gamertag fits whenever "Player 1"-style names already did. */
/* The name the game shows: its gamertag fields hold 15 characters, so a longer Name#12345 is
   the name alone (cut at 15), as the server sends it (Store::game_tag). */
static void account_game_tag(wchar_t out[16]) {
    size_t n = wide_len(account_gamertag), k = 0;
    if (n <= 15) {
        memory_copy(out, account_gamertag, (n + 1) * sizeof(wchar_t));
        return;
    }
    while (k < 15 && account_gamertag[k] && account_gamertag[k] != L'#') {
        out[k] = account_gamertag[k];
        ++k;
    }
    out[k] = 0;
}

static int account_set_player_name(void) {
    unsigned char *string = account_host + account_offsets->account_name;
    unsigned long long capacity = 0;
    wchar_t tag[16];
    account_game_tag(tag);
    size_t length = wide_len(tag);
    if (!memory_read(string + 0x18, &capacity, 8) || capacity < length || capacity > 4096) {
        return 0;
    }
    wchar_t *text = capacity > 7 ? *(wchar_t **)string : (wchar_t *)string;
    if (!text || !memory_is_writable(text, (length + 1) * sizeof(wchar_t))) {
        return 0;
    }
    memory_copy(text, tag, (length + 1) * sizeof(wchar_t));
    *(unsigned long long *)(string + 0x10) = length;
    return 1;
}

static void account_wait(void) {
    for (unsigned waited = 0; !account_ready && waited < ACCOUNT_WAIT_MS; waited += 20) {
        account_sleep(20);
    }
}

static HRESULT account_get_xuid(void *self, void **out) {
    account_wait();
    if (!account_active || !out) {
        return account_xuid_original(self, out);
    }
    AccountHstringCreateFn create = (AccountHstringCreateFn)(uintptr_t)(account_host + account_offsets->hstring_create);
    return create(account_xuid, (unsigned)wide_len(account_xuid), out);
}

/* Gamertag / display names: answered from the account directly. Azimuth rewrites its
   player-name string (e.g. when a controller is paired again), so patching it once is not
   enough. */
static HRESULT account_get_name(int slot, void *self, void **out) {
    account_wait();
    if (!account_active || !out) {
        return account_info_original[slot](self, out);
    }
    AccountHstringCreateFn create = (AccountHstringCreateFn)(uintptr_t)(account_host + account_offsets->hstring_create);
    return create(account_gamertag, (unsigned)wide_len(account_gamertag), out);
}
static HRESULT account_get_info_0(void *self, void **out) {
    return account_get_name(0, self, out);
}
static HRESULT account_get_info_1(void *self, void **out) {
    return account_get_name(1, self, out);
}
static HRESULT account_get_info_2(void *self, void **out) {
    return account_get_name(2, self, out);
}
static const AccountGetterFn account_info_hooks[3] = {account_get_info_0, account_get_info_1,
                                                       account_get_info_2};

static int account_patch_slot(unsigned slot_rva, unsigned target_rva, AccountGetterFn hook, AccountGetterFn *original) {
    void **slot = (void **)(account_host + slot_rva);
    void *expected = account_host + target_rva, *current = 0;
    DWORD old_protection, ignored;
    if (!memory_read(slot, &current, sizeof(current)) || current != expected ||
        !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protection)) {
        return 0;
    }
    *original = (AccountGetterFn)(uintptr_t)expected;
    *slot = (void *)(uintptr_t)hook;
    VirtualProtect(slot, sizeof(*slot), old_protection, &ignored);
    return 1;
}

/* ------------------------------------------------------------ server calls */

/* Takes xuid/gamertag (and token, web key when present) from a server reply. */
/* The active profile's mode, from the server (/me, sign-in): Noob profiles have no progress
   gates on the server. */
static volatile LONG account_noob;

int azimuth_account_noob(void) {
    return account_noob != 0;
}

static int account_take(const char *reply) {
    char value[96];
    if (!json_string(reply, "xuid", value, sizeof(value)) || !value[0]) {
        return 0;
    }
    utf8_to_wide(value, account_xuid, ARRAY_COUNT(account_xuid));
    if (!json_string(reply, "gamertag", value, sizeof(value)) || !value[0]) {
        return 0;
    }
    utf8_to_wide(value, account_gamertag, ARRAY_COUNT(account_gamertag));
    if (json_string(reply, "token", value, sizeof(value)) && value[0]) {
        utf8_to_wide(value, account_token, ARRAY_COUNT(account_token));
    }
    if (json_string(reply, "webKey", value, sizeof(value)) && value[0]) {
        utf8_to_wide(value, account_web_key, ARRAY_COUNT(account_web_key));
    }
    if (json_string(reply, "mode", value, sizeof(value))) {
        InterlockedExchange(&account_noob, text_equal_ignore_case(value, "noob"));
    }
    size_t length = text_length(reply);
    if (text_find(reply, length, "\"expansions\"")) {
        int storm = text_find(reply, length, STORM_ISLAND_PRODUCT) != 0;
        InterlockedExchange(&account_storm_island, storm);
        account_write(L"StormIsland", storm ? L"1" : L"0");
        if (storm) {
            azimuth_expansions_granted();
        }
    }
    return 1;
}

static const wchar_t *account_token_header(void) {
    static wchar_t header[96];
    size_t n = sizeof(L"X-Fh2-Token: ") / sizeof(wchar_t) - 1, t = wide_len(account_token);
    memory_copy(header, L"X-Fh2-Token: ", (n + 1) * sizeof(wchar_t));
    if (n + t < ARRAY_COUNT(header)) {
        memory_copy(header + n, account_token, (t + 1) * sizeof(wchar_t));
    }
    return header;
}

/* The server's {"error": "..."} as text for the player, or a fallback. */
static void account_error(DWORD status, const char *reply, wchar_t *message, size_t cap) {
    static char text[400];
    if (status && json_string(reply, "error", text, sizeof(text)) && text[0]) {
        utf8_to_wide(text, message, cap);
    } else if (!status) {
        utf8_to_wide(azimuth_tr8("Could not reach the server. Check its address and that it is running."),
                     message, cap);
    } else if (status == 429) {
        utf8_to_wide(azimuth_tr8("The server is busy. Try again in a moment."), message, cap);
    } else {
        utf8_to_wide(azimuth_tr8("The server refused the request."), message, cap);
    }
}

static DWORD WINAPI account_thread(void *unused) {
    static char reply[2048], text[400], gamertag[32];
    (void)unused;
    /* no profile under this address: one of this server's saved under another comes in */
    if (!account_token[0]) {
        if (!account_adopt() || account_online[0] != L'1') {
            log_append(account_token[0] ? "ACCOUNT: account kept but Online=0; playing the offline profile.\r\n"
                                        : "ACCOUNT: no account on this server yet (Settings > Infinity creates one).\r\n");
            InterlockedExchange(&account_ready, 1);
            return 0;
        }
        wchar_t storm[4];
        GetPrivateProfileStringW(account_server, L"StormIsland", L"0", storm, ARRAY_COUNT(storm), account_file);
        InterlockedExchange(&account_storm_island, storm[0] == L'1');
    }
    DWORD status = azimuth_net_call(L"GET", L"/infinity/v1/me", account_token_header(), 0, reply, sizeof(reply));
    if (status == 200 && account_take(reply)) {
        account_active = 1;
        /* the server's word for the player, in their language (a moderation reset) */
        if (json_string(reply, "notice", text, sizeof(text)) && text[0]) {
            utf8_to_wide(text, account_notice, ARRAY_COUNT(account_notice));
            account_log("the server says: ", text);
        }
        InterlockedExchange(&account_link, 1);
        azimuth_cloud_start(account_directory, account_xuid, account_token, account_file, account_server);
    } else if (status == 401) {
        /* the profile logged in on another PC (or the server forgot it): this token is dead */
        utf8_to_wide(azimuth_tr8("The profile "), account_notice, ARRAY_COUNT(account_notice));
        size_t n = wide_len(account_notice), g = wide_len(account_gamertag);
        if (n + g + 1 < ARRAY_COUNT(account_notice)) {
            memory_copy(account_notice + n, account_gamertag, (g + 1) * sizeof(wchar_t));
        }
        n = wide_len(account_notice);
        utf8_to_wide(azimuth_tr8(" was signed in on another PC. To play it here, sign in again with the profile and its site key."),
                     account_notice + n, ARRAY_COUNT(account_notice) - n);
        account_token[0] = 0;
        account_write(L"Token", L"");
        account_log("the server no longer knows this PC's token (logged in elsewhere); forgotten.", "");
    } else if (status == 403) {
        json_string(reply, "error", text, sizeof(text));
        account_log("the server refused this PC's account; playing offline: ", text);
        char why[16];
        if (json_string(reply, "why", why, sizeof(why)) && text_equal_ignore_case(why, "cars")) {
            utf8_to_wide(azimuth_tr8("This game's car database (media\\stripped\\gamedbRC.slt) is not the original one, so "
                                     "the Infinity server does not take it: only the game's own cars play online. Put the "
                                     "original file back (or reinstall the game) and restart."),
                         account_notice, ARRAY_COUNT(account_notice));
        }
    } else {
        /* server unreachable: keep the stored identity, the token is sent anyway */
        account_active = account_xuid[0] && account_gamertag[0];
        account_log("server unreachable; using the stored account.", "");
    }
    if (account_active) {
        wide_to_ascii(account_gamertag, gamertag, sizeof(gamertag));
        account_log(account_set_player_name() ? "playing as " : "playing as (name not applied) ", gamertag);
    }
    InterlockedExchange(&account_ready, 1);
    return 0;
}

/* ------------------------------------------------------- menu interface */

int azimuth_account_state(void) {
    if (!account_available) {
        return -1;
    }
    if (account_active) {
        return 2;
    }
    return account_token[0] ? 1 : 0;
}

int azimuth_account_storm_island(void) {
    return account_storm_island != 0;
}

const wchar_t *azimuth_account_gamertag(void) {
    return account_gamertag;
}

const wchar_t *azimuth_account_web_key(void) {
    return account_web_key;
}

const wchar_t *azimuth_account_discriminator(void) {
    return account_discriminator_wide;
}

const wchar_t *azimuth_account_server(void) {
    return account_server ? account_server : L"";
}

/* Creates "name#12345" on the server (up to ACCOUNT_PROFILES_MAX per PC) from the player's
   offline progress: the newest offline save becomes the profile's cloud save
   (azimuth_cloud_upload_offline). If that fails the new account is deleted again, so no empty
   profile is left. On success the account is kept with Online=1, so the next start plays as
   it; the profile that was in use waits in its own section (SWITCH PROFILE). */
int azimuth_account_register(const wchar_t *name, int noob, wchar_t *message, size_t cap) {
    static char reply[2048], body[256];
    static AccountRecord before;
    char ascii[24];
    if (!account_available || !azimuth_account_valid_name(name)) {
        utf8_to_wide(azimuth_tr8("The name must have 1 to 20 letters or digits and start with a letter."), message, cap);
        return 0;
    }
    wide_to_ascii(name, ascii, sizeof(ascii));
    body[0] = 0;
    text_add(body, sizeof(body), "{\"name\":\"");
    text_add(body, sizeof(body), ascii);
    text_add(body, sizeof(body), "\",\"discriminator\":\"");
    text_add(body, sizeof(body), account_discriminator);
    text_add(body, sizeof(body), "\",\"device\":\"");
    text_add(body, sizeof(body), account_device);
    text_add(body, sizeof(body), noob ? "\",\"mode\":\"noob\"}" : "\",\"mode\":\"hardcore\"}");
    record_read(account_server, &before);
    DWORD status = azimuth_net_call(L"POST", L"/infinity/v1/register", 0, body, reply, sizeof(reply));
    if (status != 200 || !account_take(reply) || !account_token[0]) {
        account_error(status, reply, message, cap);
        record_write(account_server, &before);
        account_load_active();
        return 0;
    }
    record_stash(&before);
    account_write(L"SaveCommit", L"");
    const char *why = "";
    if (!azimuth_cloud_upload_offline(account_directory, account_token, account_file, account_server, &why)) {
        azimuth_net_call(L"POST", L"/infinity/v1/delete", account_token_header(), "{}", reply, sizeof(reply));
        record_write(account_server, &before);
        account_drop_waiting(before.values[1]);
        account_load_active();
        account_log("profile not created, offline save not uploaded: ", why);
        utf8_to_wide(azimuth_tr8("Your offline progress could not be copied to the server, so the profile was not "
                                 "created. Play until the game saves, check the connection in Test connection, then "
                                 "try again."),
                     message, cap);
        return 0;
    }
    memory_copy(account_online, L"1", sizeof(L"1"));
    account_save();
    wide_to_ascii(account_gamertag, ascii, sizeof(ascii));
    account_log("created from the offline save: ", ascii);
    return 1;
}

/* Deletes this PC's profile and everything it has on the server (POST /infinity/v1/delete:
   saves, scores, designs, friends...). The offline save is not touched; the next start plays
   it. */
int azimuth_account_delete(wchar_t *message, size_t cap) {
    static char reply[512];
    if (!account_available || !account_token[0]) {
        utf8_to_wide(azimuth_tr8("There is no online profile on this PC."), message, cap);
        return 0;
    }
    DWORD status = azimuth_net_call(L"POST", L"/infinity/v1/delete", account_token_header(), "{}", reply, sizeof(reply));
    if (status != 200) {
        account_error(status, reply, message, cap);
        return 0;
    }
    char gamertag[32];
    wide_to_ascii(account_gamertag, gamertag, sizeof(gamertag));
    account_token[0] = account_xuid[0] = account_gamertag[0] = account_web_key[0] = 0;
    memory_copy(account_online, L"0", sizeof(L"0"));
    account_save();
    account_write(L"SaveCommit", L"");
    account_active = 0;
    account_log("deleted by the player: ", gamertag);
    return 1;
}

/* Logs this PC in to an existing profile (Name#12345 + site key). The server issues a new
   token and a new site key; the PC that had the profile is logged out. The next start
   downloads the profile's cloud save. */
int azimuth_account_log_in(const wchar_t *gamertag, const wchar_t *web_key, wchar_t *message, size_t cap) {
    static char reply[2048], body[160];
    char tag[32], key[32];
    wide_to_ascii(gamertag, tag, sizeof(tag));
    wide_to_ascii(web_key, key, sizeof(key));
    /* Name#12345 and K7QM-4XPA-9RT2: letters, digits, '#' and '-' only (no JSON escaping) */
    for (const char *p = tag; *p; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '#')) {
            utf8_to_wide(azimuth_tr8("Use the format Name#12345."), message, cap);
            return 0;
        }
    }
    for (const char *p = key; *p; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) {
            utf8_to_wide(azimuth_tr8("A site key has letters, digits and dashes, like K7QM-4XPA-9RT2."), message, cap);
            return 0;
        }
    }
    if (!account_available) {
        utf8_to_wide(azimuth_tr8("Infinity is not available on this PC."), message, cap);
        return 0;
    }
    body[0] = 0;
    text_add(body, sizeof(body), "{\"gamertag\":\"");
    text_add(body, sizeof(body), tag);
    text_add(body, sizeof(body), "\",\"webKey\":\"");
    text_add(body, sizeof(body), key);
    text_add(body, sizeof(body), "\"}");
    static AccountRecord before;
    record_read(account_server, &before);
    DWORD status = azimuth_net_call(L"POST", L"/infinity/v1/login", 0, body, reply, sizeof(reply));
    if (status != 200 || !account_take(reply) || !account_token[0] || wide_len(account_token) != 64) {
        account_error(status, reply, message, cap);
        record_write(account_server, &before);
        account_load_active();
        return 0;
    }
    if (!wide_equal(before.values[1], account_xuid)) {
        record_stash(&before);
    }
    account_drop_waiting(account_xuid);
    memory_copy(account_online, L"1", sizeof(L"1"));
    account_save();
    account_write(L"SaveCommit", L""); /* this PC never synced this profile */
    account_notice[0] = 0;
    wide_to_ascii(account_gamertag, tag, sizeof(tag));
    account_log("logged in as ", tag);
    return 1;
}

/* The waiting section of `xuid` goes (it is the one in use now). */
static void account_drop_waiting(const wchar_t *xuid) {
    wchar_t section[320];
    if (record_section(xuid, section, ARRAY_COUNT(section))) {
        account_write_in(section, 0, 0);
    }
}

/* SWITCH PROFILE: the next start plays `profile`. A saved one swaps sections with the one in
   use; one this PC created but no longer has the token of (signed in elsewhere, or the INI was
   lost) is signed in again by the device id (POST device/recover: the other PC goes offline,
   the site key is replaced). The game is restarted by the caller. A cloud upload of the
   profile that was in use, landing between this and the restart, only makes the next start
   take the server's save. */
int azimuth_account_switch(const AccountProfile *profile, wchar_t *message, size_t cap) {
    static char reply[2048], body[160];
    static AccountRecord before, chosen;
    wchar_t section[320];
    char tag[32];
    if (!account_available || !profile || !profile->xuid[0]) {
        utf8_to_wide(azimuth_tr8("Infinity is not available on this PC."), message, cap);
        return 0;
    }
    if (profile->suspended) {
        utf8_to_wide(azimuth_tr8("This profile is suspended on this server."), message, cap);
        return 0;
    }
    wide_to_ascii(profile->gamertag, tag, sizeof(tag));
    record_read(account_server, &before);
    if (profile->current && profile->saved) {
        azimuth_account_set_online(1);
        account_log("switching back on: ", tag);
        return 1;
    }
    if (profile->saved && record_section(profile->xuid, section, ARRAY_COUNT(section))) {
        record_read(section, &chosen);
        record_stash(&before);
        record_write(account_server, &chosen);
        account_drop_waiting(profile->xuid);
        azimuth_account_set_online(1);
        account_load_active();
        account_log("switched to the saved profile ", tag);
        return 1;
    }
    if (!profile->made_here) {
        utf8_to_wide(azimuth_tr8("This PC no longer has this profile's sign-in. Sign in with the profile and its site key (shown in Settings > Infinity on the PC that has it now)."),
                     message, cap);
        return 0;
    }
    char xuid[24];
    wide_to_ascii(profile->xuid, xuid, sizeof(xuid));
    body[0] = 0;
    text_add(body, sizeof(body), "{\"device\":\"");
    text_add(body, sizeof(body), account_device);
    text_add(body, sizeof(body), "\",\"xuid\":\"");
    text_add(body, sizeof(body), xuid);
    text_add(body, sizeof(body), "\"}");
    DWORD status = azimuth_net_call(L"POST", L"/infinity/v1/device/recover", 0, body, reply, sizeof(reply));
    if (status != 200 || !account_take(reply) || !account_token_shape(account_token)) {
        account_error(status, reply, message, cap);
        record_write(account_server, &before);
        account_load_active();
        return 0;
    }
    if (!wide_equal(before.values[1], account_xuid)) {
        record_stash(&before);
    }
    account_drop_waiting(account_xuid);
    memory_copy(account_online, L"1", sizeof(L"1"));
    account_save();
    account_write(L"SaveCommit", L""); /* the server's save is this profile's */
    account_notice[0] = 0;
    account_log("signed in again on the PC that created it: ", tag);
    return 1;
}

static void report_add(wchar_t *out, size_t cap, const wchar_t *text) {
    size_t n = wide_len(out);
    text = azimuth_tr(text);
    while (text && *text && n + 1 < cap) {
        out[n++] = *text++;
    }
    out[n] = 0;
}

static void report_number(wchar_t *out, size_t cap, unsigned long long value) {
    wchar_t digits[24];
    int n = 0;
    do {
        digits[n++] = (wchar_t)(L'0' + value % 10);
        value /= 10;
    } while (value && n < 23);
    wchar_t text[24];
    for (int i = 0; i < n; ++i) {
        text[i] = digits[n - 1 - i];
    }
    text[n] = 0;
    report_add(out, cap, text);
}

/* Settings > Infinity > Testar conexão: is the server up, how fast, and does it still
   accept this PC's profile. */
void azimuth_account_test(wchar_t *out, size_t cap) {
    static char reply[1024], text[400];
    static wchar_t wide[400];
    out[0] = 0;
    report_add(out, cap, L"Server: ");
    report_add(out, cap, azimuth_account_server());
    unsigned long long start = GetTickCount64();
    DWORD status = azimuth_net_call(L"GET", L"/infinity/v1/info", 0, 0, reply, sizeof(reply));
    unsigned long long took = GetTickCount64() - start;
    if (status != 200) {
        report_add(out, cap, status ? L"\nThe server answered with an error (HTTP " : L"\nNo answer from the server.");
        if (status) {
            report_number(out, cap, status);
            report_add(out, cap, L").");
        }
        report_add(out, cap, L"\n\nCheck the address in FH2_ExtraOptions.ini ([Network] Server), that the server is "
                             L"running and that its PC's firewall allows the port.");
        return;
    }
    report_add(out, cap, L"\nConnected: answered in ");
    report_number(out, cap, took);
    report_add(out, cap, L" ms.");
    if (!account_token[0]) {
        report_add(out, cap, L"\nProfile: none on this server yet.");
        return;
    }
    status = azimuth_net_call(L"GET", L"/infinity/v1/me", account_token_header(), 0, reply, sizeof(reply));
    if (status == 200) {
        report_add(out, cap, L"\nProfile ");
        report_add(out, cap, account_gamertag);
        report_add(out, cap, account_active ? L": accepted, playing online." : L": accepted (restart to play online).");
    } else {
        json_string(reply, "error", text, sizeof(text));
        utf8_to_wide(text[0] ? text : azimuth_tr8("refused."), wide, ARRAY_COUNT(wide));
        report_add(out, cap, L"\nProfile: ");
        report_add(out, cap, wide);
    }
}

/* The server's /info names the newest hook (latestHook, in release latestRelease): a newer one
   than this is installed by azimuth_update.c. */
static void account_check_update(const char *reply) {
    /* on the caller's stack: the start's check and the pause menu's may run at once (the offer
       itself happens once per run: azimuth_update.c's update_started / relay_started) */
    char latest[48], release[24];
    if (json_string(reply, "latestHook", latest, sizeof(latest))) {
        json_string(reply, "latestRelease", release, sizeof(release));
        azimuth_update_offer(latest, release);
    }
}

/* Once, as the game starts: is the server up, and is there a newer ForzaInfinity (which also
   checks dedicated_server.exe: azimuth_update.c's relay_offer). After that the pause menu asks
   /info each time it opens (azimuth_account_banner), so nothing polls it in between. */
static DWORD WINAPI account_link_thread(void *unused) {
    static char reply[1024];
    (void)unused;
    DWORD status = azimuth_net_call(L"GET", L"/infinity/v1/info", 0, 0, reply, sizeof(reply));
    InterlockedExchange(&account_link, status == 200 ? 1 : 2);
    if (status == 200) {
        account_check_update(reply);
    }
    /* nothing to play here, but the server knows profiles this PC created (the INI was lost,
       or they were signed in elsewhere): the INFINITY screen says where they are */
    for (unsigned waited = 0; !account_ready && waited < ACCOUNT_WAIT_MS; waited += 100) {
        account_sleep(100);
    }
    if (status == 200 && !account_active && !account_token[0]) {
        static AccountProfile list[ACCOUNT_PROFILES_MAX];
        int count = azimuth_account_profiles(list, ACCOUNT_PROFILES_MAX, 1), made = 0;
        for (int i = 0; i < count; ++i) {
            made += list[i].made_here && !list[i].saved;
        }
        if (made) {
            azimuth_account_set_notice("This PC has Infinity profiles on this server that it is not signed in to. Open SWITCH PROFILE to play one again: no site key needed on the PC that created it.");
            account_log("profiles of this PC on the server, not signed in here: listed in SWITCH PROFILE.", "");
        }
    }
    return 0;
}

/* The pause menu's status line (ASCII/UTF-8, no XML specials) and its dot color; 0 when the
   Infinity is not configured. */
int azimuth_account_banner(char *out, size_t cap, const char **color) {
    char tag[32];
    out[0] = 0;
    if (!account_available) {
        return 0;
    }
    /* checked now, every time the pause menu opens (a short wait, so the menu never hangs); a
       newer ForzaInfinity seen here is offered as at the start (once per run) */
    static char info[1024];
    int up = azimuth_net_ping_reply(1000, info, sizeof(info));
    InterlockedExchange(&account_link, up ? 1 : 2);
    if (up) {
        account_check_update(info);
    }
    LONG link = account_link;
    int update = azimuth_update_state();
    if (azimuth_cloud_stopped()) {
        text_add(out, cap, azimuth_tr8("INFINITY  |  RESTART YOUR GAME"));
        *color = "#ff4f4f";
    } else if (update == UPDATE_INSTALLED) {
        text_add(out, cap, azimuth_tr8("INFINITY  |  UPDATE INSTALLED  |  RESTART THE GAME"));
        *color = "#f5c400";
    } else if (update == UPDATE_AVAILABLE && link != 2) {
        text_add(out, cap, azimuth_tr8("INFINITY  |  UPDATE AVAILABLE  |  SEE SETTINGS > INFINITY"));
        *color = "#f5c400";
    } else if ((account_active && link != 2) || (account_token[0] && account_gamertag[0] && link == 2)) {
        /* INFINITY | CONNECTED / DISCONNECTED | NB (Noob) or HC (Hardcore), the players it meets |
           the profile */
        wide_to_ascii(account_gamertag, tag, sizeof(tag));
        text_add(out, cap, azimuth_tr8(link == 2 ? "INFINITY  |  DISCONNECTED  |  " : "INFINITY  |  CONNECTED  |  "));
        text_add(out, cap, account_noob ? "NB  |  " : "HC  |  ");
        text_add(out, cap, tag);
        *color = link == 2 ? "#ff4f4f" : "#3ddc84";
    } else if (link == 2) {
        text_add(out, cap, azimuth_tr8("INFINITY  |  NO CONNECTION TO THE SERVER"));
        *color = "#ff4f4f";
    } else if (account_token[0]) {
        text_add(out, cap, azimuth_tr8("INFINITY  |  ONLINE PROFILE OFF"));
        *color = "#9a9a9a";
    } else {
        text_add(out, cap, azimuth_tr8(link == 1 ? "INFINITY  |  SERVER ONLINE  |  CREATE YOUR PROFILE IN SETTINGS"
                                                 : "INFINITY  |  CHECKING THE SERVER..."));
        *color = link == 1 ? "#f5c400" : "#9a9a9a";
    }
    return 1;
}

/* A notice for the INFINITY screen (English UTF-8, translated), unless one is shown already
   (the server's, which says more). */
void azimuth_account_set_notice(const char *english) {
    if (!account_notice[0]) {
        utf8_to_wide(azimuth_tr8(english), account_notice, ARRAY_COUNT(account_notice));
    }
}

/* A notice that replaces whatever was shown (the sync stopping matters more). */
void azimuth_account_force_notice(const char *english) {
    utf8_to_wide(azimuth_tr8(english), account_notice, ARRAY_COUNT(account_notice));
}

const wchar_t *azimuth_account_notice(void) {
    return account_notice;
}


/* A new site key (the old one stops working). */
int azimuth_account_new_web_key(wchar_t *message, size_t cap) {
    static char reply[1024], key[64];
    DWORD status = azimuth_net_call(L"POST", L"/infinity/v1/webkey", account_token_header(), "{}", reply, sizeof(reply));
    if (status != 200 || !json_string(reply, "webKey", key, sizeof(key)) || !key[0]) {
        account_error(status, reply, message, cap);
        return 0;
    }
    utf8_to_wide(key, account_web_key, ARRAY_COUNT(account_web_key));
    account_write(L"WebKey", account_web_key);
    return 1;
}

/* Asks the server to befriend "Name#12345"; `message` is the server's answer for the
   player either way (friends once both added each other). */
/* {"xuid":"<decimal>"} */
static void account_xuid_body(unsigned long long xuid, char *body, size_t cap) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = (char)('0' + xuid % 10);
        xuid /= 10;
    } while (xuid && n < 23);
    body[0] = 0;
    text_add(body, cap, "{\"xuid\":\"");
    size_t at = text_length(body);
    while (n && at + 1 < cap) {
        body[at++] = digits[--n];
    }
    body[at] = 0;
    text_add(body, cap, "\"}");
}

/* Where this profile stands with the player `xuid` (the game's player actions): 2 friends,
   1 asked them, 3 they asked, 0 neither, -1 unknown (offline, no profile, no answer). Asks
   with a short wait: the menu is open meanwhile. */
int azimuth_account_friend_state(unsigned long long xuid) {
    static char reply[512], body[64];
    char state[16];
    if (!account_active || !account_token[0] || !xuid) {
        return -1;
    }
    account_xuid_body(xuid, body, sizeof(body));
    DWORD status = azimuth_net_call_quick(L"POST", L"/infinity/v1/friends/status", account_token_header(), body, reply,
                                          sizeof(reply), 1500);
    if (status != 200 || !json_string(reply, "state", state, sizeof(state))) {
        return -1;
    }
    return memory_equal(state, "friends", 8) ? 2 : memory_equal(state, "requested", 10) ? 1
                                               : memory_equal(state, "incoming", 9)    ? 3
                                                                                       : 0;
}

/* Adds (add) or removes the friend `xuid`; the server's answer (or why it failed) in
   `message`. Returns 1 when the server took it. */
int azimuth_account_friend_change(unsigned long long xuid, int add, wchar_t *message, size_t cap) {
    static char reply[1024], body[64], text[400];
    if (!account_active || !account_token[0]) {
        utf8_to_wide(azimuth_tr8("Friends need your online profile: create one or sign in in Settings > Infinity."), message, cap);
        return 0;
    }
    account_xuid_body(xuid, body, sizeof(body));
    DWORD status = azimuth_net_call(L"POST", add ? L"/infinity/v1/friends" : L"/infinity/v1/friends/remove",
                                    account_token_header(), body, reply, sizeof(reply));
    if (status == 200 && json_string(reply, "message", text, sizeof(text)) && text[0]) {
        utf8_to_wide(text, message, cap);
        return 1;
    }
    account_error(status, reply, message, cap);
    return 0;
}

/* PLAYER ACTIONS > REPORT PLAYER: POST /infinity/v1/report {xuid}; `message` is the server's
   answer (or the error). Five different players' reports send an account to moderation. */
int azimuth_account_report(unsigned long long xuid, wchar_t *message, size_t cap) {
    static char reply[1024], body[64], text[400];
    if (!account_active || !account_token[0]) {
        utf8_to_wide(azimuth_tr8("Reporting a player needs your online profile: create one or sign in in Settings > Infinity."),
                     message, cap);
        return 0;
    }
    account_xuid_body(xuid, body, sizeof(body));
    DWORD status = azimuth_net_call(L"POST", L"/infinity/v1/report", account_token_header(), body, reply, sizeof(reply));
    if (status == 200 && json_string(reply, "message", text, sizeof(text)) && text[0]) {
        utf8_to_wide(text, message, cap);
        return 1;
    }
    account_error(status, reply, message, cap);
    return 0;
}

int azimuth_account_add_friend(const wchar_t *gamertag, wchar_t *message, size_t cap) {
    static char reply[1024], body[96], text[400];
    char tag[32];
    wide_to_ascii(gamertag, tag, sizeof(tag));
    /* Name#12345 only: letters, digits and one '#' (no JSON escaping needed) */
    for (const char *p = tag; *p; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '#')) {
            utf8_to_wide(azimuth_tr8("Use the format Name#12345."), message, cap);
            return 0;
        }
    }
    body[0] = 0;
    text_add(body, sizeof(body), "{\"gamertag\":\"");
    text_add(body, sizeof(body), tag);
    text_add(body, sizeof(body), "\"}");
    DWORD status = azimuth_net_call(L"POST", L"/infinity/v1/friends", account_token_header(), body, reply, sizeof(reply));
    if (status == 200 && json_string(reply, "message", text, sizeof(text)) && text[0]) {
        utf8_to_wide(text, message, cap);
        return 1;
    }
    account_error(status, reply, message, cap);
    return 0;
}

/* Whether the next start plays as the account (1) or as the offline profile (0). */
void azimuth_account_set_online(int online) {
    memory_copy(account_online, online ? L"1" : L"0", sizeof(L"1"));
    account_write(L"Online", account_online);
}

/* Closes the game and opens azimuth.exe again (its launcher shows Start). */
void azimuth_account_restart(void) {
    static wchar_t exe[PATH_CAPACITY], folder[PATH_CAPACITY];
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    CreateProcessWFn create = kernel ? (CreateProcessWFn)(uintptr_t)GetProcAddress(kernel, "CreateProcessW") : 0;
    DWORD n = GetModuleFileNameW(0, exe, PATH_CAPACITY);
    if (create && n && n < PATH_CAPACITY) {
        memory_copy(folder, exe, (n + 1) * sizeof(wchar_t));
        while (n && folder[n - 1] != L'\\' && folder[n - 1] != L'/') {
            --n;
        }
        folder[n] = 0;
        STARTUPINFOW startup = {0};
        PROCESS_INFORMATION process = {0};
        startup.cb = sizeof(startup);
        if (create(exe, 0, 0, 0, FALSE, 0, 0, folder, &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
    log_append("ACCOUNT: restarting the game for the profile change.\r\n");
    ExitProcess(0);
}

unsigned long long azimuth_account_xuid_value(void) {
    unsigned long long xuid = 0;
    if (!account_ready || !account_active) {
        return 0;
    }
    for (const wchar_t *p = account_xuid; *p >= L'0' && *p <= L'9'; ++p) {
        xuid = xuid * 10 + (unsigned)(*p - L'0');
    }
    return xuid;
}

const wchar_t *azimuth_account_token(void) {
    return account_ready && account_active && account_token[0] ? account_token : 0;
}

const wchar_t *azimuth_account_token_wait(void) {
    account_wait();
    return azimuth_account_token();
}

static void account_apply(const wchar_t *directory) {
    if (!azimuth_net_proxy_enabled() || !(account_server = azimuth_net_server_name())) {
        return;
    }
    account_host = (unsigned char *)GetModuleHandleW(NULL);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    account_sleep = kernel ? (SleepFn)(uintptr_t)GetProcAddress(kernel, "Sleep") : 0;
    size_t n = wide_len(directory);
    account_offsets = azimuth_offsets(account_host);
    if (!account_sleep || !account_offsets || n >= PATH_CAPACITY ||
        !path_join(account_file, directory, ACCOUNT_FILE)) {
        log_append("ACCOUNT: unsupported host or paths; FH2Online accounts unavailable.\r\n");
        return;
    }
    memory_copy(account_directory, directory, (n + 1) * sizeof(wchar_t));
    if (!account_fingerprint()) {
        log_append("ACCOUNT: no fingerprint for this PC; FH2Online accounts unavailable.\r\n");
        return;
    }
    account_available = 1;
    GetPrivateProfileStringW(account_server, L"Token", L"", account_token, ARRAY_COUNT(account_token), account_file);
    GetPrivateProfileStringW(account_server, L"Xuid", L"", account_xuid, ARRAY_COUNT(account_xuid), account_file);
    GetPrivateProfileStringW(account_server, L"Gamertag", L"", account_gamertag, ARRAY_COUNT(account_gamertag),
                             account_file);
    GetPrivateProfileStringW(account_server, L"WebKey", L"", account_web_key, ARRAY_COUNT(account_web_key),
                             account_file);
    GetPrivateProfileStringW(account_server, L"Online", L"1", account_online, ARRAY_COUNT(account_online),
                             account_file);
    account_log("fingerprint #", account_discriminator);
    /* none under this address, but tokens under others: the account thread asks the server
       whether one of them is its (account_adopt) before the game reads the identity */
    int adopt = !account_token[0] && account_other_tokens();
    if (!account_token[0] && !adopt) {
        log_append("ACCOUNT: no account on this server yet (Settings > Infinity creates one).\r\n");
        return;
    }
    if (!adopt && account_online[0] != L'1') {
        log_append("ACCOUNT: account kept but Online=0; playing the offline profile.\r\n");
        return;
    }
    /* the last reply's expansions, for the game's first licence pass (before /me answers) */
    wchar_t storm[4];
    GetPrivateProfileStringW(account_server, L"StormIsland", L"0", storm, ARRAY_COUNT(storm), account_file);
    InterlockedExchange(&account_storm_island, storm[0] == L'1');
    int patched = account_patch_slot(account_offsets->account_xuid_slot, account_offsets->account_xuid_target,
                                     account_get_xuid, &account_xuid_original);
    for (unsigned i = 0; i < 3; ++i) {
        patched += account_patch_slot(account_offsets->account_info_slots[i], account_offsets->account_info_targets[i],
                                      account_info_hooks[i], &account_info_original[i]);
    }
    if (patched != 4) {
        log_append("ACCOUNT: LocalUser getters not where expected; playing the offline profile.\r\n");
        return;
    }
    account_ready = 0;
    HANDLE thread = CreateThread(0, 0, account_thread, 0, 0, 0);
    if (!thread) {
        account_ready = 1;
        return;
    }
    CloseHandle(thread);
}

void azimuth_apply_account(const wchar_t *directory) {
    account_apply(directory);
    /* after the stored profile is read (and its check started): the link thread looks at it */
    if (account_available) {
        HANDLE link = CreateThread(0, 0, account_link_thread, 0, 0, 0);
        if (link) {
            CloseHandle(link);
        }
    }
}
