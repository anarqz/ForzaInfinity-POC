/* Host test for adapters/azimuth_freeroam.c, built for Windows by tests/freeroam_test.py and
   run there or under wine. Azimuth's WinRT objects are stood in for by small fakes; the
   Winsock redirection is run on this program's own imports. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../adapters/azimuth_freeroam.c"

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
int memory_read(const void *address, void *output, size_t size) {
    if (!address) return 0;
    memcpy(output, address, size);
    return 1;
}
int text_equal_ignore_case(const char *left, const char *right) {
    return !_stricmp(left, right);
}
int path_join(wchar_t output[PATH_CAPACITY], const wchar_t *directory, const wchar_t *name) {
    _snwprintf(output, PATH_CAPACITY, L"%ls\\%ls", directory, name);
    return 1;
}
int azimuth_net_proxy_enabled(void) {
    return 1;
}
void azimuth_world_set_directory(const wchar_t *directory) {
    (void)directory;
}
void azimuth_world_left(void) {
}
void azimuth_world_hash_car_db(const wchar_t *executable) {
    (void)executable;
}
/* azimuth_proxy.c has its own test (tests/proxy_test.c): here the game goes to the world. */
void azimuth_proxy_configure(int direct) {
    (void)direct;
}
int azimuth_proxy_start(const void *world, void *local) {
    (void)world;
    (void)local;
    return 0;
}
void azimuth_proxy_stop(void) {
}

/* Azimuth's HSTRING layout, as fr_hstring reads it. */
typedef struct {
    unsigned magic, length;
    unsigned long long reserved;
    const wchar_t *text;
    wchar_t storage[128];
} FakeString;
HRESULT azimuth_net_make_hstring(const wchar_t *text, void **out) {
    FakeString *s = calloc(1, sizeof(*s));
    s->magic = FR_HSTRING_MAGIC;
    s->length = (unsigned)wcslen(text);
    wcsncpy(s->storage, text, 127);
    s->text = s->storage;
    *out = s;
    return S_OK;
}

static int failures;
#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

/* ------------------------------------------------- fakes of Azimuth's objects */

typedef struct {
    void *const *lpVtbl;
    LONG references;
    int kind;
} Fake;
static LONG fake_alive;

static HRESULT STDMETHODCALLTYPE fake_query(Fake *self, REFIID iid, void **out) {
    (void)iid;
    InterlockedIncrement(&self->references);
    *out = self;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE fake_add_ref(Fake *self) {
    return (ULONG)InterlockedIncrement(&self->references);
}
static ULONG STDMETHODCALLTYPE fake_release(Fake *self) {
    LONG left = InterlockedDecrement(&self->references);
    if (!left) {
        InterlockedDecrement(&fake_alive);
        free(self);
    }
    return (ULONG)left;
}
static HRESULT STDMETHODCALLTYPE fake_nothing(Fake *self) {
    (void)self;
    return E_NOTIMPL;
}
static Fake *fake_new(void *const *vtable, int kind);

/* An address object: Azimuth's answers an empty base64 (slot 10). */
static HRESULT STDMETHODCALLTYPE fake_address_base64(Fake *self, void **out) {
    (void)self;
    return azimuth_net_make_hstring(L"", out);
}
static void *const fake_address_vtable[16] = {
    (void *)fake_query, (void *)fake_add_ref, (void *)fake_release, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_address_base64, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing};

/* The address statics: slot 11 FromBase64String makes an address. */
static HRESULT STDMETHODCALLTYPE fake_from_base64(Fake *self, void *text, void **out) {
    (void)self, (void)text;
    *out = fake_new(fake_address_vtable, 1);
    return S_OK;
}
static void *const fake_address_statics_vtable[14] = {
    (void *)fake_query, (void *)fake_add_ref, (void *)fake_release, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_from_base64,
    (void *)fake_nothing, (void *)fake_nothing};

/* The template statics: slot 6 GetTemplateByName; the template's slot 8 is never reached. */
static void *const fake_template_vtable[16] = {
    (void *)fake_query, (void *)fake_add_ref, (void *)fake_release, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing};
static HRESULT STDMETHODCALLTYPE fake_template_by_name(Fake *self, void *name, void **out) {
    (void)self, (void)name;
    *out = fake_new(fake_template_vtable, 2);
    return S_OK;
}
static void *const fake_template_statics_vtable[10] = {
    (void *)fake_query, (void *)fake_add_ref, (void *)fake_release, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_template_by_name, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing};

/* Statics with nothing of their own (QoS, Party): every slot refuses. */
static void *const fake_plain_vtable[30] = {
    (void *)fake_query, (void *)fake_add_ref, (void *)fake_release, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing};

/* The game's IIterable<SecureDeviceAddress> of two addresses (First 6; iterator 6/7/8). */
typedef struct {
    void *const *lpVtbl;
    LONG references;
    int kind, index;
} FakeIterator;
static HRESULT STDMETHODCALLTYPE fake_iter_current(FakeIterator *self, void **out) {
    (void)self;
    *out = fake_new(fake_address_vtable, 1);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fake_iter_has(FakeIterator *self, BYTE *has) {
    *has = self->index < 2;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fake_iter_next(FakeIterator *self, BYTE *has) {
    ++self->index;
    *has = self->index < 2;
    return S_OK;
}
static void *const fake_iterator_vtable[10] = {
    (void *)fake_query, (void *)fake_add_ref, (void *)fake_release, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_iter_current, (void *)fake_iter_has,
    (void *)fake_iter_next, (void *)fake_nothing};
static HRESULT STDMETHODCALLTYPE fake_iterable_first(Fake *self, void **out) {
    (void)self;
    FakeIterator *it = calloc(1, sizeof(*it));
    it->lpVtbl = fake_iterator_vtable;
    it->references = 1;
    InterlockedIncrement(&fake_alive);
    *out = it;
    return S_OK;
}
static void *const fake_iterable_vtable[7] = {
    (void *)fake_query, (void *)fake_add_ref, (void *)fake_release, (void *)fake_nothing,
    (void *)fake_nothing, (void *)fake_nothing, (void *)fake_iterable_first};

static Fake *fake_new(void *const *vtable, int kind) {
    Fake *f = calloc(1, sizeof(*f));
    f->lpVtbl = vtable;
    f->references = 1;
    f->kind = kind;
    InterlockedIncrement(&fake_alive);
    return f;
}

/* A completion handler: Invoke (slot 3) records the status. */
typedef struct {
    void *const *lpVtbl;
    LONG references;
    int invoked, status;
} Handler;
static HRESULT STDMETHODCALLTYPE handler_query(Handler *self, REFIID iid, void **out) {
    (void)iid;
    *out = self;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE handler_add_ref(Handler *self) {
    return (ULONG)++self->references;
}
static ULONG STDMETHODCALLTYPE handler_release(Handler *self) {
    return (ULONG)--self->references;
}
static HRESULT STDMETHODCALLTYPE handler_invoke(Handler *self, void *operation, int status) {
    (void)operation;
    ++self->invoked;
    self->status = status;
    return S_OK;
}
static void *const handler_vtable[4] = {(void *)handler_query, (void *)handler_add_ref,
                                        (void *)handler_release, (void *)handler_invoke};

/* ------------------------------------------------------------------ helpers */

typedef HRESULT(STDMETHODCALLTYPE *Out1)(void *, void **);
typedef HRESULT(STDMETHODCALLTYPE *In1Out1)(void *, void *, void **);
#define SLOT(object, n) (((void *const *)*(void **)(object))[n])
#define RELEASE(object) ((ULONG(STDMETHODCALLTYPE *)(void *))SLOT(object, 2))(object)

/* Checks a completed operation and returns its result (a new reference), or null. */
static void *completed_result(void *operation) {
    Handler handler = {handler_vtable, 1, 0, 0};
    CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, void *))SLOT(operation, 6))(operation, &handler) == S_OK);
    CHECK(handler.invoked == 1 && handler.status == 1);
    void *info = 0;
    CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, REFIID, void **))SLOT(operation, 0))(
              operation, &fr_iid_async_info, &info) == S_OK);
    int status = 0;
    CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, int *))SLOT(info, 7))(info, &status) == S_OK && status == 1);
    RELEASE(info);
    void *result = 0;
    ((HRESULT(STDMETHODCALLTYPE *)(void *, void **))SLOT(operation, 8))(operation, &result);
    RELEASE(operation);
    return result;
}

static FrSockaddrIn6 world_of(const char *spec) {
    wchar_t base64[128];
    char text[96];
    snprintf(text, sizeof(text), "fh2inf:%s", spec);
    fr_base64_encode(text, base64, 128);
    return fr_world_address(base64);
}

static void test_addresses(void) {
    char decoded[64];
    /* The server's example. */
    CHECK(fr_base64_decode(L"ZmgyaW5mOjE5OC41MS4xMDAuNDA6NTEwMQ==", decoded, sizeof(decoded)) == 25);
    CHECK(!strcmp(decoded, "fh2inf:198.51.100.40:5101"));
    wchar_t encoded[64];
    fr_base64_encode("fh2inf:198.51.100.40:5101", encoded, 64);
    CHECK(!wcscmp(encoded, L"ZmgyaW5mOjE5OC41MS4xMDAuNDA6NTEwMQ=="));
    CHECK(fr_base64_decode(L"not*base64", decoded, sizeof(decoded)) < 0);

    FrSockaddrIn6 a = world_of("198.51.100.40:5101");
    static const unsigned char mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 198, 51, 100, 40};
    CHECK(a.family == AF_INET6 && a.port == htons(5101) && !memcmp(a.address, mapped, 16));
    a = world_of("[2001:db8::1]:6000");
    static const unsigned char v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK(a.port == htons(6000) && !memcmp(a.address, v6, 16));
    a = world_of("10.0.0.2"); /* no port */
    CHECK(a.port == htons(5101) && a.address[15] == 2);
    static const unsigned char loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    a = fr_world_address(L"ZmgybmV0LWdhbWUtc2VydmVy"); /* a placeholder address */
    CHECK(a.port == htons(5101) && !memcmp(a.address, loopback, 16));
    a = fr_world_address(L"");
    CHECK(!memcmp(a.address, loopback, 16));
    wcscpy(fr_world_override, L"172.16.0.9:7000");
    a = world_of("198.51.100.40:5101");
    CHECK(a.port == htons(7000) && a.address[15] == 9);
    fr_world_override[0] = 0;
}

static void test_association(void) {
    /* The address statics: GetLocal and FromBase64String hand out wrapped objects whose
       GetBase64String answers what they were made from. */
    void *statics = fake_new(fake_address_statics_vtable, 0);
    CHECK(azimuth_freeroam_filter(L"Windows.Xbox.Networking.SecureDeviceAddress", &fr_iid_address_statics, &statics));
    void *local = 0, *text = 0;
    CHECK(((Out1)SLOT(statics, 9))(statics, &local) == S_OK && local);
    CHECK(((Out1)SLOT(local, 10))(local, &text) == S_OK);
    wchar_t got[96];
    fr_hstring(text, got, 96);
    char decoded[32];
    CHECK(fr_base64_decode(got, decoded, sizeof(decoded)) > 0 && !strcmp(decoded, "fh2inf:local"));
    RELEASE(local);

    void *session_text = 0, *remote = 0;
    wchar_t base64[64];
    fr_base64_encode("fh2inf:198.51.100.40:5101", base64, 64);
    azimuth_net_make_hstring(base64, &session_text);
    CHECK(((In1Out1)SLOT(statics, 11))(statics, session_text, &remote) == S_OK && remote);

    /* The template statics: the template is wrapped, and CreateAssociationAsync completes
       with the world's address. */
    void *template_statics = fake_new(fake_template_statics_vtable, 0);
    CHECK(azimuth_freeroam_filter(L"Windows.Xbox.Networking.SecureDeviceAssociationTemplate",
                                  &fr_iid_template_statics, &template_statics));
    void *name = 0, *template = 0, *operation = 0;
    azimuth_net_make_hstring(L"SecurePeerTraffic", &name);
    CHECK(((In1Out1)SLOT(template_statics, 6))(template_statics, name, &template) == S_OK && template);
    CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, void *, int, void **))SLOT(template, 8))(
              template, remote, 0, &operation) == S_OK);
    void *association = completed_result(operation);
    CHECK(association != 0);
    if (association) {
        struct sockaddr_in6 peer;
        CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, UINT32, BYTE *))SLOT(association, 9))(
                  association, sizeof(peer), (BYTE *)&peer) == S_OK);
        CHECK(peer.sin6_family == AF_INET6 && peer.sin6_port == htons(5101));
        CHECK(peer.sin6_addr.s6_addr[10] == 0xff && peer.sin6_addr.s6_addr[12] == 198 &&
              peer.sin6_addr.s6_addr[15] == 40);
        CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, UINT32, BYTE *))SLOT(association, 9))(
                  association, 8, (BYTE *)&peer) == E_INVALIDARG);
        void *destroy = 0;
        CHECK(((Out1)SLOT(association, 8))(association, &destroy) == S_OK);
        CHECK(completed_result(destroy) == 0);
        RELEASE(association);
    }
    RELEASE(remote);
    RELEASE(template);
    RELEASE(template_statics);
    RELEASE(statics);
}

static void test_qos(void) {
    void *statics = fake_new(fake_plain_vtable, 0);
    CHECK(azimuth_freeroam_filter(L"Windows.Xbox.Networking.QualityOfService", &fr_iid_qos_statics, &statics));
    Fake *addresses = fake_new(fake_iterable_vtable, 3);
    void *operation = 0;
    CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, void *, void *, UINT32, UINT32, void **))SLOT(statics, 6))(
              statics, addresses, 0, 1000, 4, &operation) == S_OK);
    void *result = completed_result(operation);
    CHECK(result != 0);
    void *list = 0, *iterable = 0, *iterator = 0;
    CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, int, void **))SLOT(result, 8))(result, 2, &list) == S_OK);
    UINT32 size = 0;
    CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, UINT32 *))SLOT(list, 7))(list, &size) == S_OK && size == 2);
    CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, REFIID, void **))SLOT(list, 0))(list, &fr_iid_qos_iterable,
                                                                                &iterable) == S_OK);
    CHECK(iterable != list);
    CHECK(((Out1)SLOT(iterable, 6))(iterable, &iterator) == S_OK);
    unsigned seen = 0;
    BYTE has = 0;
    ((HRESULT(STDMETHODCALLTYPE *)(void *, BYTE *))SLOT(iterator, 7))(iterator, &has);
    while (has) {
        void *measurement = 0, *value = 0, *address = 0;
        int status = 0, metric = 0;
        UINT32 latency = 0;
        CHECK(((Out1)SLOT(iterator, 6))(iterator, &measurement) == S_OK);
        CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, int *))SLOT(measurement, 6))(measurement, &status) == S_OK && status == 1);
        CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, int *))SLOT(measurement, 7))(measurement, &metric) == S_OK && metric == 2);
        CHECK(((Out1)SLOT(measurement, 8))(measurement, &value) == S_OK);
        CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, UINT32 *))SLOT(value, 11))(value, &latency) == S_OK && latency == 20);
        CHECK(((Out1)SLOT(measurement, 9))(measurement, &address) == S_OK && ((Fake *)address)->kind == 1);
        RELEASE(address);
        RELEASE(value);
        RELEASE(measurement);
        ++seen;
        ((HRESULT(STDMETHODCALLTYPE *)(void *, BYTE *))SLOT(iterator, 8))(iterator, &has);
    }
    CHECK(seen == 2);
    RELEASE(iterator);
    RELEASE(iterable);
    RELEASE(list);
    RELEASE(result);
    RELEASE(addresses);
    RELEASE(statics);
}

static void test_party(void) {
    void *statics = fake_new(fake_plain_vtable, 0);
    CHECK(azimuth_freeroam_filter(L"Windows.Xbox.Multiplayer.Party", &fr_iid_party_statics, &statics));
    for (int slot = 6; slot <= 7; ++slot) {
        void *action = 0;
        CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *, void *, void *, void **))SLOT(statics, slot))(
                  statics, 0, 0, &action) == S_OK);
        CHECK(completed_result(action) == 0);
    }
    /* Other slots still reach Azimuth's object. */
    CHECK(((HRESULT(STDMETHODCALLTYPE *)(void *))SLOT(statics, 9))(statics) == E_NOTIMPL);
    RELEASE(statics);
    /* Another class is left alone. */
    void *other = fake_new(fake_plain_vtable, 0), *before = other;
    CHECK(!azimuth_freeroam_filter(L"Windows.Xbox.Multiplayer.Party", &fr_iid_qos_statics, &other));
    CHECK(other == before);
    RELEASE(other);
}

/* The redirection, run on this program's imports: bind goes through fr_bind, which makes an
   IPv6 socket dual-stack. */
static void test_sockets(void) {
    WSADATA data;
    CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    fr_game = (unsigned char *)GetModuleHandleW(NULL);
    fr_sockets_state = 0;
    fr_sockets();
    CHECK(fr_sockets_state == 1);
    SOCKET s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(s != INVALID_SOCKET);
    struct sockaddr_in6 any;
    memset(&any, 0, sizeof(any));
    any.sin6_family = AF_INET6;
    CHECK(bind(s, (struct sockaddr *)&any, sizeof(any)) == 0);
    DWORD v6only = 1;
    int length = sizeof(v6only);
    CHECK(getsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&v6only, &length) == 0 && v6only == 0);
    CHECK(fr_game_socket == s);
    closesocket(s);
    WSACleanup();
}

int main(void) {
    fr_enabled = 1;
    fr_sockets_state = 2; /* the COM tests run before the Winsock one */
    fr_base64_encode("fh2inf:local", fr_local_base64, 48);
    test_addresses();
    test_association();
    test_qos();
    test_party();
    CHECK(fake_alive == 0);
    if (fake_alive) fprintf(stderr, "  %ld fake objects still alive\n", fake_alive);
    test_sockets();
    printf(failures ? "freeroam_test: %d FAILED\n" : "freeroam_test: all ok\n", failures);
    return failures ? 1 : 0;
}
