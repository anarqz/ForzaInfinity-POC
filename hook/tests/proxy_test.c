/* Host test for adapters/azimuth_proxy.c (F3), built for Windows by tests/proxy_test.py and run
   there or under wine, with real sockets: the session names a world that never answers, the
   (fake) ForzaInfinity server names a second address that answers STUN and echoes, and a
   stranger sends to the proxy's public port. The game (a socket on loopback) must reach the
   answering world, get its echo, and never hear the stranger.
   Usage: proxy_test.exe <this PC's LAN IPv4 address> */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../adapters/azimuth_proxy.c"

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
const wchar_t *azimuth_account_token(void) {
    return L"test-token";
}
const wchar_t *azimuth_tr(const wchar_t *english) {
    return english;
}
/* the relay's car states (azimuth_carstate.c): not under test here */
void azimuth_carstate_received(const unsigned char *data, int size) {
    (void)data;
    (void)size;
}
int azimuth_world_net_text(char *out, size_t cap) {
    snprintf(out, cap, "nat cone\npublic 203.0.113.5:5101\nupnp none\n");
    return 1;
}

static char lan[64];
static unsigned short echo_port;
static volatile LONG punch_calls;
static char punch_body[512];

DWORD azimuth_net_call(const wchar_t *method, const wchar_t *path, const wchar_t *extra_header, const char *body,
                       char *out, size_t capacity) {
    (void)extra_header;
    if (wcscmp(method, L"POST") || wcscmp(path, L"/infinity/v1/worlds/punch")) {
        return 404;
    }
    InterlockedIncrement(&punch_calls);
    snprintf(punch_body, sizeof(punch_body), "%s", body);
    snprintf(out, capacity, "{\"world\":[\"%s:%u\"],\"nat\":\"cone\"}", lan, echo_port);
    return 200;
}

static int fails;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

static SOCKET udp4(unsigned short *port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    bind(s, (struct sockaddr *)&a, sizeof(a));
    int n = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &n);
    *port = ntohs(a.sin_port);
    DWORD t = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
    return s;
}

/* The world that answers: Binding success to STUN requests, "echo:" + data otherwise. */
static volatile LONG stop;
static DWORD WINAPI echo_world(void *socket_pointer) {
    SOCKET s = *(SOCKET *)socket_pointer;
    unsigned char b[2048];
    while (!stop) {
        struct sockaddr_storage from;
        int fl = sizeof(from);
        int n = recvfrom(s, (char *)b, sizeof(b), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        if (n == 20 && b[4] == 0x21 && b[5] == 0x12 && b[6] == 0xa4 && b[7] == 0x42 && b[1] == 1) {
            unsigned char r[32];
            memcpy(r, b, 20);
            r[0] = 1, r[1] = 1, r[2] = 0, r[3] = 12;
            struct sockaddr_in *f = (struct sockaddr_in *)&from;
            r[20] = 0, r[21] = 0x20, r[22] = 0, r[23] = 8, r[24] = 0, r[25] = 1;
            unsigned short p = f->sin_port;
            r[26] = ((unsigned char *)&p)[0] ^ 0x21, r[27] = ((unsigned char *)&p)[1] ^ 0x12;
            for (int i = 0; i < 4; ++i) r[28 + i] = ((unsigned char *)&f->sin_addr)[i] ^ r[4 + i];
            sendto(s, (const char *)r, 32, 0, (struct sockaddr *)&from, fl);
            continue;
        }
        char e[2100];
        memcpy(e, "echo:", 5);
        memcpy(e + 5, b, (size_t)n);
        sendto(s, e, n + 5, 0, (struct sockaddr *)&from, fl);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: proxy_test.exe <LAN IPv4>\n");
        return 2;
    }
    snprintf(lan, sizeof(lan), "%s", argv[1]);
    WSADATA wsa;
    WSAStartup(0x0202, &wsa);
    unsigned short silent_port;
    SOCKET silent = udp4(&silent_port);
    SOCKET echo = udp4(&echo_port);
    HANDLE t = CreateThread(0, 0, echo_world, &echo, 0, 0);

    /* the session's world: the silent one, at the LAN address */
    PxAddr world = {0}, local = {0};
    char spec[96];
    snprintf(spec, sizeof(spec), "%s:%u", lan, silent_port);
    CHECK(px_load());
    CHECK(px_parse(spec, strlen(spec), &world));
    /* loopback worlds (the host itself) need no proxy */
    PxAddr loop = world;
    memset(loop.address, 0, 16);
    loop.address[15] = 1;
    CHECK(!azimuth_proxy_start(&loop, &local));
    CHECK(azimuth_proxy_start(&world, &local));
    CHECK(local.address[15] == 1 && local.port != 0);

    /* the game on loopback */
    SOCKET game = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in6 g = {0};
    g.sin6_family = AF_INET6;
    g.sin6_addr.s6_addr[15] = 1;
    bind(game, (struct sockaddr *)&g, sizeof(g));
    DWORD to = 100;
    setsockopt(game, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    int got = 0;
    char b[2048];
    for (int k = 0; k < 100 && !got; ++k) {
        sendto(game, "hello", 5, 0, (const struct sockaddr *)&local, sizeof(local));
        int n = recv(game, b, sizeof(b), 0);
        got = n == 10 && !memcmp(b, "echo:hello", 10);
    }
    CHECK(got);
    CHECK(punch_calls == 1);
    CHECK(strstr(punch_body, "\"port\":") != 0);
    CHECK(px_have_world && ntohs(px_world.port) == echo_port);

    /* a stranger at the proxy's public port: never reaches the game */
    unsigned short stranger_port;
    SOCKET stranger = udp4(&stranger_port);
    struct sockaddr_in proxy_public = {0};
    proxy_public.sin_family = AF_INET;
    inet_pton(AF_INET, lan, &proxy_public.sin_addr);
    proxy_public.sin_port = htons(px_local_port);
    int heard = 0;
    for (int k = 0; k < 10; ++k) {
        sendto(stranger, "evil", 4, 0, (struct sockaddr *)&proxy_public, sizeof(proxy_public));
        int n = recv(game, b, sizeof(b), 0);
        heard |= n == 4 && !memcmp(b, "evil", 4);
    }
    CHECK(!heard);
    /* the silent world, now answering, is not the world either */
    for (int k = 0; k < 5; ++k) {
        sendto(silent, "spoof", 5, 0, (struct sockaddr *)&proxy_public, sizeof(proxy_public));
        int n = recv(game, b, sizeof(b), 0);
        heard |= n == 5 && !memcmp(b, "spoof", 5);
    }
    CHECK(!heard);
    /* the game still gets the world */
    got = 0;
    for (int k = 0; k < 20 && !got; ++k) {
        sendto(game, "again", 5, 0, (const struct sockaddr *)&local, sizeof(local));
        int n = recv(game, b, sizeof(b), 0);
        got = n == 10 && !memcmp(b, "echo:again", 10);
    }
    CHECK(got);

    azimuth_proxy_stop();
    CHECK(px_socket == PX_INVALID);

    /* TEST CONNECTION's free roam part, on this PC's real network */
    static wchar_t report[2400];
    azimuth_proxy_nat_test(report, 2400);
    fprintf(stderr, "%ls\n", report);
    CHECK(wcsstr(report, L"FREE ROAM") && wcsstr(report, L"YOUR WORLD") && wcsstr(report, L"203.0.113.5:5101"));
    CHECK(wcsstr(report, L"NAT: ") || wcsstr(report, L"No answer from the STUN servers"));
    CHECK(wcsstr(report, L"not available on this router"));
    stop = 1;
    WaitForSingleObject(t, 2000);
    printf(fails ? "proxy_test: %d failure(s)\n" : "proxy_test: ok\n", fails);
    return fails != 0;
}
