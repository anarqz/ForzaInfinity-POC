#include "azimuth_host.h"
#include "logging.h"
#include "support.h"

/* Scriptable controller for unattended tests. Azimuth 0.9.7 resolves XInputGetState with
   GetProcAddress and keeps the pointer at azimuth+0x319520; a small thread swaps it for
   pad_get_state once XInput is loaded. A tool sets the shared memory Local\FH2Online_Pad:
   while `active` is 1, controller 0 reports `buttons`, triggers and sticks from there
   instead of the real pad. Inactive, everything passes through untouched. */

#define PAD_SLOT_RVA 0x319520u

typedef struct {
    DWORD packet;
    WORD buttons;
    BYTE left_trigger, right_trigger;
    SHORT lx, ly, rx, ry;
} PadState;

typedef struct {
    volatile LONG active;
    volatile LONG sequence;
    volatile WORD buttons;
    volatile BYTE left_trigger, right_trigger;
    volatile SHORT lx, ly, rx, ry;
    volatile LONG reads;
} PadShared;

typedef DWORD(WINAPI *PadGetStateFn)(DWORD, PadState *);
typedef HANDLE(WINAPI *CreateFileMappingWFn)(HANDLE, void *, DWORD, DWORD, DWORD, const wchar_t *);
typedef void *(WINAPI *MapViewOfFileFn)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
typedef void(WINAPI *SleepFn)(DWORD);

static PadGetStateFn pad_real;
static PadShared *pad_shared;
static unsigned char *pad_host;

static DWORD WINAPI pad_get_state(DWORD user, PadState *state) {
    DWORD result = pad_real ? pad_real(user, state) : ERROR_DEVICE_NOT_CONNECTED;
    PadShared *shared = pad_shared;
    if (user || !shared || !shared->active || !state) {
        return result;
    }
    shared->reads++;
    state->packet = (DWORD)shared->sequence;
    state->buttons = shared->buttons;
    state->left_trigger = shared->left_trigger;
    state->right_trigger = shared->right_trigger;
    state->lx = shared->lx;
    state->ly = shared->ly;
    state->rx = shared->rx;
    state->ry = shared->ry;
    return ERROR_SUCCESS;
}

static PadGetStateFn pad_system_get_state(void) {
    static const wchar_t *const names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        HMODULE module = GetModuleHandleW(names[i]);
        if (module) {
            return (PadGetStateFn)(uintptr_t)GetProcAddress(module, "XInputGetState");
        }
    }
    return 0;
}

static DWORD WINAPI pad_thread(void *unused) {
    (void)unused;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    SleepFn sleep = kernel ? (SleepFn)(uintptr_t)GetProcAddress(kernel, "Sleep") : 0;
    if (!sleep) {
        return 0;
    }
    PadGetStateFn volatile *slot = (PadGetStateFn volatile *)(pad_host + PAD_SLOT_RVA);
    int logged = 0;
    for (;;) {
        PadGetStateFn current = *slot;
        if (current && current != pad_get_state) {
            PadGetStateFn system = pad_system_get_state();
            if (current == system) {
                pad_real = current;
                *slot = pad_get_state;
                if (!logged) {
                    log_append("PAD: XInputGetState routed through Local\\FH2Online_Pad.\r\n");
                    logged = 1;
                }
            }
        }
        sleep(500);
    }
}

void azimuth_apply_pad(void) {
    pad_host = (unsigned char *)GetModuleHandleW(NULL);
    if (azimuth_host_build(pad_host) != AZIMUTH_097) {
        log_append("PAD: skipped; host is not the inspected Azimuth 0.9.7 build.\r\n");
        return;
    }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    CreateFileMappingWFn create_mapping =
        kernel ? (CreateFileMappingWFn)(uintptr_t)GetProcAddress(kernel, "CreateFileMappingW") : 0;
    MapViewOfFileFn map_view =
        kernel ? (MapViewOfFileFn)(uintptr_t)GetProcAddress(kernel, "MapViewOfFile") : 0;
    HANDLE mapping = create_mapping ? create_mapping(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0,
                                                     4096, L"Local\\FH2Online_Pad")
                                    : 0;
    pad_shared = mapping ? (PadShared *)map_view(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 4096) : 0;
    if (!pad_shared) {
        log_append("PAD: shared memory unavailable; scripted controller disabled.\r\n");
        return;
    }
    HANDLE thread = CreateThread(0, 0, pad_thread, 0, 0, 0);
    if (thread) {
        CloseHandle(thread);
    }
}
