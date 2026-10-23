/*
 * The keyboard: Windows' own DirectInput, with one seam.
 *
 * POD polls the keyboard with IDirectInputDevice::GetDeviceState (DirectX 3,
 * a 256-byte array indexed by DIK_ scan code). The device is the real one,
 * but its vtable's Acquire and GetDeviceState are replaced (each vtable once,
 * like ddraw.c's):
 *   - headless, a cloaked window is never the foreground one, so a
 *     DISCL_FOREGROUND keyboard can never be acquired and every poll fails.
 *     Acquire always succeeds, and a poll that fails reads as no keys down;
 *   - scripted keys (--key, host.c) are ORed into a keyboard poll, by scan
 *     code, so a headless run can drive the menus and the car.
 * Everything else, including the mouse and joysticks, is DirectInput's.
 */
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0300
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <string.h>

#include "dinput_host.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define RET(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (nargs); } while (0)

enum { DI_CREATEDEVICE = 3, DEV_ACQUIRE = 7, DEV_GETDEVICESTATE = 9, DEV_SETEVENTNOTIFICATION = 12 };

static int g_headless;
typedef HRESULT (WINAPI *create_device_t)(void*, REFGUID, void**, void*);
typedef HRESULT (WINAPI *acquire_t)(void*);
typedef HRESULT (WINAPI *get_state_t)(void*, DWORD, void*);
static create_device_t g_create_device;
static acquire_t g_acquire;
static get_state_t g_get_state;
typedef HRESULT (WINAPI *set_event_t)(void*, HANDLE);
static set_event_t g_set_event;
static HANDLE g_kbd_event;

void dih_init(int headless) { g_headless = headless; }

/* POD reads the keyboard only when DirectInput signals the event it handed
 * to SetEventNotification ("DInputKeyboard"): its message loop waits on it.
 * A cloaked window never gets a real key, so a scripted one signals it. */
void dih_key_changed(void) { if (g_kbd_event) SetEvent(g_kbd_event); }

static HRESULT WINAPI my_set_event(void* dev, HANDLE e) {
    if (e) g_kbd_event = e;
    return g_set_event(dev, e);
}

static void patch(void* obj, int slot, void* fn, void** real) {
    void** vt = *(void***)obj;
    if (vt[slot] == fn) return;
    DWORD old;
    *real = vt[slot];
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old);
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

static HRESULT WINAPI my_acquire(void* dev) {
    HRESULT r = g_acquire(dev);
    return g_headless ? DI_OK : r;
}

static HRESULT WINAPI my_get_state(void* dev, DWORD size, void* data) {
    HRESULT r = g_headless ? DIERR_NOTACQUIRED : g_get_state(dev, size, data);
    if (size != 256) return r;                   /* not the keyboard */
    if (FAILED(r)) memset(data, 0, size);
    for (int vk = 1; vk < 256; vk++) {
        if (!host_key_held(vk)) continue;
        UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        /* The arrows are extended keys: DIK_UP is 0xC8, not the keypad's 0x48. */
        if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) sc |= 0x80;
        if (sc && sc < 256) ((BYTE*)data)[sc] = 0x80;
    }
    return DI_OK;
}

static HRESULT WINAPI my_create_device(void* di, REFGUID guid, void** out, void* outer) {
    HRESULT r = g_create_device(di, guid, out, outer);
    if (SUCCEEDED(r) && *out && IsEqualGUID(guid, &GUID_SysKeyboard)) {
        patch(*out, DEV_ACQUIRE, (void*)my_acquire, (void**)&g_acquire);
        patch(*out, DEV_GETDEVICESTATE, (void*)my_get_state, (void**)&g_get_state);
        patch(*out, DEV_SETEVENTNOTIFICATION, (void*)my_set_event, (void**)&g_set_event);
    }
    return r;
}

/* DirectInputCreateA(hinst, version, out, outer). The guest's hinst is its
 * own image, which DirectInput does not know as a module: the host's is
 * passed instead. */
void dih_shim_DirectInputCreateA(void) {
    void** out = (void**)(uintptr_t)ARG(2);
    /* From dinput.dll at run time: the farm's SDK has only dinput8.lib. */
    typedef HRESULT (WINAPI *create_t)(HINSTANCE, DWORD, void**, void*);
    static create_t create;
    if (!create) create = (create_t)GetProcAddress(LoadLibraryA("dinput.dll"), "DirectInputCreateA");
    HRESULT r = create ? create(GetModuleHandleA(NULL), ARG(1), out, (void*)(uintptr_t)ARG(3)) : DIERR_GENERIC;
    if (SUCCEEDED(r) && *out) patch(*out, DI_CREATEDEVICE, (void*)my_create_device, (void**)&g_create_device);
    else fprintf(stderr, "[dinput] DirectInputCreateA failed 0x%08lX\n", (unsigned long)r);
    RET(r, 4);
}
