/* FH2Online: FH2_ExtraOptions.ini is the player's, ForzaInfinity only adds what it needs.
   At startup, before the INI is read:
   - no INI (a DLL copied by hand, an INI deleted): the ForzaInfinity template is written;
   - an INI without [Network], Mode or Server (an ExtraOptions INI from before ForzaInfinity), or
     with an empty or "__SERVER__" Server (a template copied by hand): only those are added or
     filled in, with Mode=Proxy and the public server. Every other line stays as it was, Mode=Off
     included;
   - before the first change, the file as it was is kept as FH2_ExtraOptions.ini.pre-infinity.
   An INI that cannot be read as text (UTF-16, too big) is left alone: settings.c says why. */
#include "azimuth_host.h"
#include "azimuth_ini_template.h"
#include "extra_options.h"
#include "logging.h"
#include "support.h"

#define INI_LIMIT 8192 /* settings.c refuses a bigger INI */
#define INI_NETWORK_KEYS "Mode=Proxy\r\nServer=" AZIMUTH_INI_SERVER "\r\n"
#define INI_NETWORK_SECTION                                                                        \
    "\r\n[Network]\r\n"                                                                            \
    "; ForzaInfinity: Off = offline, Capture = log the requests, Proxy = the ForzaInfinity "      \
    "server.\r\n"                                                                                  \
    "; Server: https://host or host:port.\r\n" INI_NETWORK_KEYS

static char ini_old[INI_LIMIT + 1];
static char ini_new[INI_LIMIT + 1];
static size_t ini_new_size;
static wchar_t ini_path[PATH_CAPACITY], ini_temp[PATH_CAPACITY], ini_backup[PATH_CAPACITY];
static const char *ini_note;

static int ini_put(const char *data, size_t size) {
    if (size > INI_LIMIT - ini_new_size) {
        return 0;
    }
    memory_copy(ini_new + ini_new_size, data, size);
    ini_new_size += size;
    return 1;
}

static int ini_put_text(const char *text) {
    return ini_put(text, text_length(text));
}

static int ini_write(const wchar_t *path, const char *data, size_t size) {
    HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD written = 0;
    int ok = WriteFile(file, data, (DWORD)size, &written, NULL) && written == size &&
             FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

/* Written beside it, then renamed over it: a crash never leaves half an INI. */
static int ini_replace(const char *data, size_t size) {
    if (!ini_write(ini_temp, data, size)) {
        DeleteFileW(ini_temp);
        return 0;
    }
    if (!MoveFileExW(ini_temp, ini_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(ini_temp);
        return 0;
    }
    return 1;
}

static int span_equal_ignore_case(const char *span, size_t size, const char *word) {
    size_t length = text_length(word);
    if (size != length) {
        return 0;
    }
    for (size_t i = 0; i < size; ++i) {
        char a = span[i], b = word[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + 32);
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + 32);
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static void span_trim(const char **start, const char **end) {
    while (*start < *end && (**start == ' ' || **start == '\t')) {
        ++*start;
    }
    while (*end > *start && ((*end)[-1] == ' ' || (*end)[-1] == '\t' || (*end)[-1] == '\r')) {
        --*end;
    }
}

/* The new INI for an existing one of `size` bytes, in ini_new: 1 ready, 0 nothing must change,
   -1 it would not fit. */
static int ini_merge(size_t size) {
    /* The first [Network] is the one GetPrivateProfileString and settings.c read. */
    int network = 0, in_network = 0, has_mode = 0, has_server = 0;
    size_t header_end = 0, bad_server = 0, bad_server_end = 0;
    for (size_t at = 0; at < size;) {
        size_t line = at;
        while (at < size && ini_old[at] != '\n') {
            ++at;
        }
        size_t line_end = at;
        if (at < size) {
            ++at;
        }
        const char *start = ini_old + line, *end = ini_old + line_end;
        if (line == 0 && end - start >= 3 && memory_equal(start, "\xef\xbb\xbf", 3)) {
            start += 3;
        }
        for (const char *c = start; c < end; ++c) {
            if (*c == ';' || *c == '#') {
                end = c;
                break;
            }
        }
        span_trim(&start, &end);
        if (start == end) {
            continue;
        }
        if (*start == '[') {
            const char *name = start + 1, *name_end = end;
            if (name_end > name && name_end[-1] == ']') {
                --name_end;
            }
            span_trim(&name, &name_end);
            in_network = !network && span_equal_ignore_case(name, (size_t)(name_end - name), "Network");
            if (in_network) {
                network = 1;
                header_end = at;
            }
            continue;
        }
        if (!in_network) {
            continue;
        }
        const char *equals = start;
        while (equals < end && *equals != '=') {
            ++equals;
        }
        const char *key = start, *key_end = equals, *value = equals < end ? equals + 1 : end;
        const char *value_end = end;
        span_trim(&key, &key_end);
        span_trim(&value, &value_end);
        if (span_equal_ignore_case(key, (size_t)(key_end - key), "Mode")) {
            has_mode = 1;
        } else if (span_equal_ignore_case(key, (size_t)(key_end - key), "Server") && !has_server) {
            has_server = 1;
            if (value == value_end ||
                span_equal_ignore_case(value, (size_t)(value_end - value), "__SERVER__")) {
                bad_server = line;
                bad_server_end = at;
            }
        }
    }

    ini_new_size = 0;
    if (!network) {
        ini_note = "INI: [Network] added to FH2_ExtraOptions.ini (Mode=Proxy, Server=" AZIMUTH_INI_SERVER
                   "); every other setting was kept.\r\n";
        return ini_put(ini_old, size) &&
                       (!size || ini_old[size - 1] == '\n' || ini_put_text("\r\n")) &&
                       ini_put_text(INI_NETWORK_SECTION)
                   ? 1
                   : -1;
    }
    if (has_mode && has_server && bad_server_end == 0) {
        return 0;
    }
    ini_note = "INI: [Network] of FH2_ExtraOptions.ini completed (Mode=Proxy and/or Server="
               AZIMUTH_INI_SERVER "); every other setting was kept.\r\n";
    if (!ini_put(ini_old, header_end) ||
        (header_end && ini_old[header_end - 1] != '\n' && !ini_put_text("\r\n")) ||
        (!has_mode && !ini_put_text("Mode=Proxy\r\n")) ||
        (!has_server && !ini_put_text("Server=" AZIMUTH_INI_SERVER "\r\n"))) {
        return -1;
    }
    if (!bad_server_end) {
        return ini_put(ini_old + header_end, size - header_end) ? 1 : -1;
    }
    return ini_put(ini_old + header_end, bad_server - header_end) &&
                   ini_put_text("Server=" AZIMUTH_INI_SERVER "\r\n") &&
                   ini_put(ini_old + bad_server_end, size - bad_server_end)
               ? 1
               : -1;
}

void azimuth_prepare_ini(const wchar_t *directory) {
    ini_note = 0;
    if (!path_join(ini_path, directory, MOD_INI_NAME) ||
        !path_join(ini_temp, directory, L"FH2_ExtraOptions.ini.infinity-new") ||
        !path_join(ini_backup, directory, L"FH2_ExtraOptions.ini.pre-infinity")) {
        return;
    }
    HANDLE file = CreateFileW(ini_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return;
        }
        ini_note = ini_replace(azimuth_ini_template, sizeof(azimuth_ini_template) - 1)
                       ? "INI: no FH2_ExtraOptions.ini, the ForzaInfinity template was written "
                         "(Server=" AZIMUTH_INI_SERVER ").\r\n"
                       : "INI: no FH2_ExtraOptions.ini, and the template could not be written.\r\n";
        return;
    }
    LARGE_INTEGER size;
    DWORD got = 0;
    int read = GetFileSizeEx(file, &size) && size.QuadPart <= INI_LIMIT &&
               ReadFile(file, ini_old, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart;
    CloseHandle(file);
    if (!read) {
        return;
    }
    for (DWORD i = 0; i < got; ++i) {
        if (!ini_old[i]) {
            return; /* UTF-16: settings.c refuses it with its own message */
        }
    }
    int merged = ini_merge(got);
    if (merged <= 0) {
        ini_note = merged ? "INI: FH2_ExtraOptions.ini lacks [Network] settings, but adding them "
                            "would pass 8192 bytes; it was left as it is.\r\n"
                          : 0;
        return;
    }
    if (GetFileAttributesW(ini_backup) == INVALID_FILE_ATTRIBUTES &&
        !ini_write(ini_backup, ini_old, got)) {
        ini_note = "INI: FH2_ExtraOptions.ini lacks [Network] settings, but its backup could not "
                   "be written; it was left as it is.\r\n";
        return;
    }
    if (!ini_replace(ini_new, ini_new_size)) {
        ini_note = "INI: FH2_ExtraOptions.ini lacks [Network] settings, but it could not be "
                   "rewritten.\r\n";
    }
}

void azimuth_log_ini(void) {
    if (ini_note) {
        log_append(ini_note);
    }
}
