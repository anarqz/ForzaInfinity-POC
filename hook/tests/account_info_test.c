/* Host test for adapters/azimuth_account.c's use of /infinity/v1/info, built for Windows by
   tests/account_info_test.py and run there or under wine: the game's start asks /info once (no
   15 s loop) and offers a newer ForzaInfinity from it; each pause menu asks /info again through
   the ping and offers a newer one from its body too. The network and the update are fakes. */
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
    return 0;
}
void azimuth_expansions_granted(void) {
}
int azimuth_cloud_stopped(void) {
    return 0;
}
int azimuth_update_state(void) {
    return 0;
}

/* /info as the server answers it: latestHook and latestRelease at the end */
static const char INFO[] =
    "{\"name\":\"ForzaInfinity\",\"api\":1,\"registration\":\"open\",\"online\":12,\"queued\":0,\"capacity\":150,"
    "\"freeroam\":true,\"latestHook\":\"0.2.74+fh2online.47\",\"latestRelease\":\"v0.2.1\"}";
static int info_calls, ping_calls, offers, up = 1;
static char offered[64], offered_release[32];

DWORD azimuth_net_call(const wchar_t *method, const wchar_t *path, const wchar_t *extra_header, const char *body,
                       char *out, size_t capacity) {
    (void)extra_header, (void)body;
    if (!wcscmp(method, L"GET") && !wcscmp(path, L"/infinity/v1/info")) {
        ++info_calls;
        snprintf(out, capacity, "%s", INFO);
        return up ? 200 : 0;
    }
    out[0] = 0;
    return 404;
}
DWORD azimuth_net_call_quick(const wchar_t *method, const wchar_t *path, const wchar_t *extra_header, const char *body,
                             char *out, size_t capacity, int timeout_ms) {
    (void)timeout_ms;
    return azimuth_net_call(method, path, extra_header, body, out, capacity);
}
int azimuth_net_ping_reply(int timeout_ms, char *out, size_t cap) {
    (void)timeout_ms;
    ++ping_calls;
    snprintf(out, cap, "%s", INFO);
    return up;
}
int azimuth_net_ping(int timeout_ms) {
    static char reply[1024];
    return azimuth_net_ping_reply(timeout_ms, reply, sizeof(reply));
}
void azimuth_update_offer(const char *latest, const char *release) {
    ++offers;
    snprintf(offered, sizeof(offered), "%s", latest);
    snprintf(offered_release, sizeof(offered_release), "%s", release);
}

static int fails;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

static DWORD WINAPI run_link(void *unused) {
    return account_link_thread(unused);
}

int main(void) {
    /* the start: one /info, then the thread ends (it used to loop every 15 s) */
    HANDLE t = CreateThread(0, 0, run_link, 0, 0, 0);
    CHECK(WaitForSingleObject(t, 3000) == WAIT_OBJECT_0);
    CloseHandle(t);
    CHECK(info_calls == 1);
    CHECK(account_link == 1);
    /* ... and it offered the newer version from it (the relay's check rides on that) */
    CHECK(offers == 1 && !strcmp(offered, "0.2.74+fh2online.47") && !strcmp(offered_release, "v0.2.1"));

    /* the pause menu: its ping asks /info, reads latestHook from the end of the body, offers it */
    account_available = 1;
    char line[160];
    const char *color = 0;
    CHECK(azimuth_account_banner(line, sizeof(line), &color));
    CHECK(ping_calls == 1 && offers == 2 && account_link == 1);
    CHECK(info_calls == 1); /* nothing else asked it */

    /* the server down at a pause: no offer, the line says so */
    up = 0;
    CHECK(azimuth_account_banner(line, sizeof(line), &color));
    CHECK(ping_calls == 2 && offers == 2 && account_link == 2);
    CHECK(strstr(line, "NO CONNECTION") != 0);

    printf(fails ? "account_info_test: %d failure(s)\n" : "account_info_test: ok\n", fails);
    return fails != 0;
}
