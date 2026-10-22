/*
 * dinput_trace.c - name and watch the DirectInput 8 objects the game uses.
 *
 * Input goes through real dinput8.dll (keyboard/mouse/joystick devices,
 * SetCooperativeLevel on the game window, Acquire, GetDeviceState/Data).
 * Every call is logged the first few times and every failing HRESULT is
 * logged always, because a device that silently fails to acquire looks like
 * "the buttons do nothing".
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "native32.h"
#include "recomp_types.h"
#include "nfs_log.h"
#include "nfs_runtime.h"
#include <stdlib.h>

/* ---- synthetic input (testing without focus) ----
 * NFSU2_TAP="appdata@ms[,appdata@ms...]" (hex appdata, ms since start):
 * at that time the next GetDeviceData of the action-mapped device returns one
 * press event with that uAppData, and the following one the release. The
 * window needs no focus: a failing GetDeviceData is turned into S_OK. Enter is
 * appdata 0x20, Escape 0x1F in the tested build. */
static struct { uint32_t app, at, state; } g_tap[16];
static int g_ntap;
static DWORD g_t0;
static void tap_init(void) {
    static int done; if (done) return; done = 1;
    g_t0 = GetTickCount();
    const char *e = getenv("NFSU2_TAP");
    while (e && *e && g_ntap < 16) {
        char *end; g_tap[g_ntap].app = strtoul(e, &end, 16);
        if (*end != '@') break;
        g_tap[g_ntap].at = strtoul(end + 1, &end, 10); g_ntap++;
        e = *end == ',' ? end + 1 : NULL;
    }
    for (int i = 0; i < g_ntap; i++) NFS_LOG(WIN32, "NFSU2_TAP: appdata %02X at %u ms", g_tap[i].app, g_tap[i].at);
}
/* returns 1 if it wrote an event into the guest buffer */
static int tap_inject(const uint32_t *a) {
    DWORD now = GetTickCount() - g_t0;
    for (int i = 0; i < g_ntap; i++) {
        if (g_tap[i].state >= 2 || now < g_tap[i].at + (g_tap[i].state ? 150u : 0u)) continue;
        uint32_t *od = (uint32_t *)(uintptr_t)a[2], *cnt = (uint32_t *)(uintptr_t)a[3];
        if (!od || !cnt || *cnt == 0 && g_tap[i].state == 0 && 0) return 0;
        od[0] = 0; od[1] = g_tap[i].state == 0 ? 0x80 : 0; od[2] = GetTickCount(); od[3] = 0; od[4] = g_tap[i].app;
        *cnt = 1;
        NFS_LOG(WIN32, "NFSU2_TAP: injected appdata %02X %s at %u ms", g_tap[i].app, g_tap[i].state == 0 ? "press" : "release", now);
        g_tap[i].state++;
        return 1;
    }
    return 0;
}

static const char *k_di8[11] = { "QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices",
    "GetDeviceStatus", "RunControlPanel", "Initialize", "FindDevice", "EnumDevicesBySemantics",
    "ConfigureDevices" };
#define NDEVM 32
static const char *k_did8[NDEVM] = { "QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects",
    "GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState", "GetDeviceData",
    "SetDataFormat", "SetEventNotification", "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo",
    "RunControlPanel", "Initialize", "CreateEffect", "EnumEffects", "GetEffectInfo",
    "GetForceFeedbackState", "SendForceFeedbackCommand", "EnumCreatedEffectObjects", "Escape", "Poll",
    "SendDeviceData", "EnumEffectsInFile", "WriteEffectToFile", "BuildActionMap", "SetActionMap",
    "GetImageInfo" };

static uint32_t g_di_vt[11];
#define MAXDEV 8
static struct { uint32_t obj, vt[NDEVM]; uint32_t calls[NDEVM], fails[NDEVM]; } g_dev[MAXDEV];
static int g_ndev;

void nfs_dinput_register(uint32_t di8) {
    const uint32_t *vt = *(const uint32_t **)(uintptr_t)di8;
    for (int i = 0; i < 11; i++) { g_di_vt[i] = vt[i]; native32_add_name(vt[i], "IDirectInput8", k_di8[i]); }
}

int nfs_dinput_post(uint32_t fn, const uint32_t *a, uint32_t ret) {
    if (g_di_vt[3] && fn == g_di_vt[3]) {                 /* IDirectInput8::CreateDevice(this, rguid, out, outer) */
        const uint32_t *g = (const uint32_t *)(uintptr_t)a[1];
        uint32_t dev = ret == 0 ? *(uint32_t *)(uintptr_t)a[2] : 0;
        NFS_LOG(WIN32, "DirectInput CreateDevice guid {%08X-...} -> %08X dev %08X", g ? g[0] : 0, ret, dev);
        if (dev && g_ndev < MAXDEV) {
            const uint32_t *vt = *(const uint32_t **)(uintptr_t)dev;
            g_dev[g_ndev].obj = dev;
            for (int i = 0; i < NDEVM; i++) { g_dev[g_ndev].vt[i] = vt[i]; native32_add_name(vt[i], "IDirectInputDevice8", k_did8[i]); }
            g_ndev++;
        }
        return 1;
    }
    for (int i = 0; i < 11; i++)
        if (g_di_vt[i] && fn == g_di_vt[i] && i != 1 && i != 2) {
            NFS_LOG(WIN32, "IDirectInput8 %08X %s(%08X %08X %08X %08X) -> %08X", a[0], k_di8[i], a[1], a[2], a[3], a[4], ret);
            return 1;
        }
    /* Devices handed to the game by EnumDevicesBySemantics' callback were never
     * returned by CreateDevice: recognise any object by its vtable, and give
     * each distinct `this` its own slot. */
    if (!g_ndev) return 0;
    int i;
    for (i = 0; i < NDEVM; i++) if (g_dev[0].vt[i] == fn) break;
    if (i == NDEVM) return 0;
    int d;
    for (d = 0; d < g_ndev; d++) if (g_dev[d].obj == a[0]) break;
    if (d == g_ndev) {
        if (g_ndev == MAXDEV) return 1;
        g_dev[d].obj = a[0];
        memcpy(g_dev[d].vt, g_dev[0].vt, sizeof g_dev[0].vt);
        g_ndev++;
        NFS_LOG(WIN32, "dinput: new device object dev%d = %08X (first call %s)", d, a[0], k_did8[i]);
    }
    uint32_t n = ++g_dev[d].calls[i];
    tap_init();
    if (i == 29 /* BuildActionMap */ || i == 30 /* SetActionMap */) {
        const uint32_t *f = (const uint32_t *)(uintptr_t)a[1];   /* DIACTIONFORMATA */
        /* dwSize, dwActionSize, dwDataSize, dwNumActions, rgoAction, guid[4], dwGenre,
         * dwBufferSize, lAxisMin, lAxisMax, hInstString, ftTimeStamp[2], dwCRC, tszActionMap */
        NFS_LOG(WIN32, "  DIACTIONFORMAT@%08X size %u actsize %u datasize %u n %u genre %08X buf %u hInstString %08X map \"%.40s\"",
                a[1], f[0], f[1], f[2], f[3], f[9], f[10], f[13], (const char *)&f[17]);
        const uint32_t *act = (const uint32_t *)(uintptr_t)f[4];   /* DIACTIONA: 40 bytes (dwActionSize) */
        for (uint32_t k = 0; k < f[3] && k < 3 && i == 29; k++, act += f[1] / 4)
            NFS_LOG(WIN32, "    action %u: appdata %08X semantic %08X flags %08X name/resid %08X how %08X",
                    k, act[0], act[1], act[2], act[3], act[9]);   /* dwHow is dword 9 */
    }
    /* The controller object (0x874C40 in the tested build) keeps an active-low
     * button mask at +8/+0xC, updated from the events right after
     * GetDeviceData. Log it on the call after any frame that had events. */
    /* pad event queue (0x865980: count, then 12-byte entries code/port/...),
     * logged while a synthetic tap is in flight */
    if (i == 25 /* Poll */ && g_ntap) {
        static uint32_t last_cnt = 0xFFFFFFFF; static int frames_after;
        uint32_t cnt = *(uint32_t *)(uintptr_t)0x865980;
        int active = 0;
        for (int k = 0; k < g_ntap; k++) if (g_tap[k].state == 1) active = 1;
        if (active) frames_after = 20;
        if ((frames_after > 0 || cnt != last_cnt) && cnt < 0x100) {
            const uint8_t *e = (const uint8_t *)(uintptr_t)(0x865980 + 4);
            if (cnt) NFS_LOG(WIN32, "padq: count %u%s first [code %04X port %02X src %02X %08X %08X]", cnt,
                    active ? " (key down)" : "", *(uint16_t *)e, e[2], e[3], *(uint32_t *)(e + 4), *(uint32_t *)(e + 8));
            if (frames_after > 0) frames_after--;
        }
        last_cnt = cnt;
    }
    static int s_pending;
    if (i == 25 /* Poll */ && s_pending) {
        s_pending = 0;
        uint32_t ctl = a[2];
        if (ctl >= 0x800000 && ctl < 0x932000)
            NFS_LOG(WIN32, "dinput: controller %08X mask after events: %08X %08X", ctl,
                    *(uint32_t *)(uintptr_t)(ctl + 8), *(uint32_t *)(uintptr_t)(ctl + 12));
    }
    if (i == 10 /* GetDeviceData */ && ret == 0 && a[3] && *(uint32_t *)(uintptr_t)a[3]) s_pending = 1;
    if (i == 10 /* GetDeviceData */ && ret == 0 && a[3] && *(uint32_t *)(uintptr_t)a[3]) {
        uint32_t cnt = *(uint32_t *)(uintptr_t)a[3];
        const uint32_t *od = (const uint32_t *)(uintptr_t)a[2];   /* DIDEVICEOBJECTDATA: ofs data stamp seq appdata */
        static uint32_t logged;
        if (logged++ < 64)
            NFS_LOG(WIN32, "dinput dev%d GetDeviceData -> %u events, first: ofs %08X data %08X appdata %08X",
                    d, cnt, od[0], od[1], od[4]);
    }
    if (ret & 0x80000000u) g_dev[d].fails[i]++;
    if (n <= 8 || ((ret & 0x80000000u) && g_dev[d].fails[i] <= 64))
        NFS_LOG(WIN32, "dinput dev%d %s(%08X %08X %08X) -> %08X%s", d, k_did8[i], a[1], a[2], a[3],
                ret, (ret & 0x80000000u) ? "  <-- FAILED" : "");
    return 1;
}


/* ---- GetDeviceData vtable hook: NFSU2_TAP for lifted and native guests ----
 * Every IDirectInputDevice8A in dinput8 shares one vtable, so a throwaway
 * keyboard device yields the slot the game's action-mapped device uses. */
typedef HRESULT (__stdcall *getdata_t)(void *, DWORD, void *, DWORD *, DWORD);
static getdata_t g_orig_getdata;
static HRESULT __stdcall hk_getdata(void *dev, DWORD cb, void *od, DWORD *cnt, DWORD flags) {
    HRESULT hr = g_orig_getdata(dev, cb, od, cnt, flags);
    if (g_ntap && cb == 0x14 && od && cnt) {
        DWORD want = *cnt;
        uint32_t a[4] = { (uint32_t)(uintptr_t)dev, cb, (uint32_t)(uintptr_t)od, (uint32_t)(uintptr_t)cnt };
        if ((hr < 0 || *cnt == 0) && want) { if (tap_inject(a)) hr = 0; }
    }
    return hr;
}
void nfs_dinput_hook(uint32_t di8) {
    static const GUID kbd = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    typedef HRESULT (__stdcall *createdev_t)(void *, const GUID *, void **, void *);
    tap_init();
    if (g_orig_getdata) return;
    uint32_t *vt = *(uint32_t **)(uintptr_t)di8;
    void *dev = NULL;
    if (((createdev_t)(uintptr_t)vt[3])((void *)(uintptr_t)di8, &kbd, &dev, NULL) != 0 || !dev) return;
    uint32_t *dvt = *(uint32_t **)dev;
    DWORD old;
    VirtualProtect(&dvt[10], 4, PAGE_EXECUTE_READWRITE, &old);
    g_orig_getdata = (getdata_t)(uintptr_t)dvt[10];
    dvt[10] = (uint32_t)(uintptr_t)hk_getdata;
    VirtualProtect(&dvt[10], 4, old, &old);
    ((ULONG (__stdcall *)(void *))(uintptr_t)dvt[2])(dev);     /* Release */
    NFS_LOG(WIN32, "dinput: GetDeviceData hooked (vtable %08X)", (uint32_t)(uintptr_t)dvt);
}
