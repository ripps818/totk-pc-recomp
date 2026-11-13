// AMD FSR upscaling for the DLAA path (vk_ffx.h). The FSR SDK 2.4 ships its Vulkan effects as signed DLLs behind a
// loader: amd_fidelityfx_loader_vk.dll exports the five ffx-api entry points and finds the effect DLL
// (amd_fidelityfx_upscaler_vk.dll) next to itself. Both are loaded from the executable's folder at run time; no SDK
// code is compiled in, only the descriptor layouts of ffx_api_min.h.
//
// The upscaler DLL offers several versions (FSR 4.x, FSR 3.1.x) and decides by itself which ones a device can run:
// vk_ffx_init lists them and pins the newest 4.x when there is one, else leaves the DLL's default (3.1 on GPUs without
// FSR 4). Every context is created with that version (ffxOverrideVersion).
//
// What FSR gets is what DLSS gets (vk_dlaa.c evaluate): the R11G11B10F light target as linear HDR colour with automatic
// exposure, the D32F scene depth, motion vectors at the render size in render pixels pointing from this frame's
// position to last frame's without the jitter (motionVectorScale 1), and the jitter the scene was drawn with. That is
// FSR's own convention as well: its guide derives the projection offset as (2 jx / w, -2 jy / h), which moves the
// picture by (jx, jy) window pixels with y down, the way vk_dlaa_viewport moves the viewport. TOTK_FSR_JITTER_SIGN=-1
// flips it to test the other convention.
#include "vk_ffx.h"

#include "cfg.h"
#include "host.h"

#ifndef _WIN32
int vk_ffx_wanted(void) { return 0; }
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max) {
    (void)have, (void)nhave, (void)ext, (void)n, (void)max;
}
void vk_ffx_init(void) {}
int vk_ffx_available(void) { return 0; }
int vk_ffx_fg_pick(char *out, size_t n) { (void)out, (void)n; return 0; }
uint32_t vk_ffx_fg_create(void **ctx, void *desc) { (void)ctx, (void)desc; return 1; }
uint32_t vk_ffx_fg_configure(void **ctx, void *desc) { (void)ctx, (void)desc; return 1; }
uint32_t vk_ffx_fg_dispatch(void **ctx, void *desc) { (void)ctx, (void)desc; return 1; }
void vk_ffx_fg_destroy(void **ctx) { (void)ctx; }
void *vk_ffx_create(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int depth_inverted, int depth_infinite) {
    (void)w, (void)h, (void)ow, (void)oh, (void)depth_inverted, (void)depth_infinite;
    return NULL;
}
int vk_ffx_dispatch(void *ctx, VkCommandBuffer cb, const VkFfxDispatch *d) { return (void)ctx, (void)cb, (void)d, 0; }
void vk_ffx_release(void *ctx) { (void)ctx; }
const char *vk_ffx_version(void) { return ""; }
#else
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffx_api_min.h"

#define FFX_LOADER L"amd_fidelityfx_loader_vk.dll"
#define FFX_MAX_VERSIONS 16

static struct {
    HMODULE dll;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    int tried, loaded, available;
    VkDeviceContext dev;
    uint64_t version_id;   // 0: the DLL's default
    char version[48];
    float jitter_sign;
} F = {.jitter_sign = 1.0f};

int vk_ffx_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = cfg_bool("TOTK_DLAA") && !strcmp(cfg_value("TOTK_DLAA_BACKEND"), "fsr");
    return wanted;
}

static int loader_load(void) {
    if (F.tried) return F.loaded;
    F.tried = 1;
    wchar_t path[MAX_PATH + 64];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (len && path[len - 1] != L'\\') len--;
    path[len] = 0;
    wcscat(path, FFX_LOADER);
    // LOAD_WITH_ALTERED_SEARCH_PATH: the loader's own imports and the effect DLL resolve from the executable's folder
    F.dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!F.dll) {
        rt_log("fsr: cannot load amd_fidelityfx_loader_vk.dll next to totk.exe (error %lu): FSR off", GetLastError());
        return 0;
    }
    F.create = (PfnFfxCreateContext)(void *)GetProcAddress(F.dll, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(F.dll, "ffxDestroyContext");
    F.query = (PfnFfxQuery)(void *)GetProcAddress(F.dll, "ffxQuery");
    F.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(F.dll, "ffxDispatch");
    if (!F.create || !F.destroy || !F.query || !F.dispatch) {
        rt_log("fsr: amd_fidelityfx_loader_vk.dll lacks the ffx-api entry points: FSR off");
        return 0;
    }
    return F.loaded = 1;
}

// ---- the FSR 4 model (totk.ini fsr_model)
// AMD's upscaler DLL holds one provider class per FSR 4 model (the full network on 8-bit floats, two on 8-bit integers,
// and a lite one). The library asks each in turn, with ffxProvider::IsSupported(device) (the third entry of the class's
// vtable), and takes the first that says yes, so which model runs follows from the device alone. With fsr_model set,
// the DLL's own answer is replaced here: only the named model's provider says yes. A class is found by its RTTI name
// (the type descriptor's text), the complete object locator that names that descriptor, and the vtable that points at
// the locator. fsr_model = probe changes nothing and logs what each provider says of this device.
enum { MODEL_FP8, MODEL_INT8, MODEL_INT8_RDNA2, MODEL_LITE, MODEL_N };
static const struct { const char *name, *key, *cls; int own_check_optional; } MODELS[MODEL_N] = {
    {"FP8", "fp8", "ffxProvider_FSR4", 0},
    {"INT8", "int8", "ffxProvider_FSR4_Int8", 1},
    {"INT8 RDNA2", "int8_rdna2", "ffxProvider_FSR4_Int8_Navi2", 1},
    {"INT8 Lite", "lite", "ffxProvider_FSR4Lite_Int8", 1},
};   // own_check_optional: the DLL limits these to some GPUs, which the chosen model may run on anyway

typedef bool (*PfnIsSupported)(void *self, void *device);
static struct {
    int want;   // a model, or -1 to leave the answers alone (probe)
    void **vtable[MODEL_N];
    PfnIsSupported orig[MODEL_N];
    volatile int said[MODEL_N];
} MH = {.want = -1};

static bool model_is_supported(void *self, void *device) {
    void **vt = *(void ***)self;
    for (int m = 0; m < MODEL_N; m++) {
        if (MH.vtable[m] != vt) continue;
        bool own = MH.orig[m](self, device);
        bool answer = MH.want < 0 ? own : (m == MH.want && (own || MODELS[m].own_check_optional));
        if (device && !__atomic_exchange_n(&MH.said[m], 1, __ATOMIC_RELAXED))
            rt_log("fsr: %s provider: the DLL says %s for this device%s", MODELS[m].name, own ? "supported" : "not supported",
                   MH.want < 0 ? "" : answer ? "; fsr_model lets it run" : "; fsr_model keeps it off");
        return answer;
    }
    return false;
}

typedef struct { const BYTE *p; size_t n; } Range;

// The readable sections of a loaded module
static int readable_ranges(HMODULE m, Range *r, int max) {
    const BYTE *base = (const BYTE *)m;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && n < max; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ) || (sec[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE)) continue;
        r[n++] = (Range){base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
    }
    return n;
}

static void **find_vtable(HMODULE m, const char *cls) {
    char name[128];
    snprintf(name, sizeof name, ".?AV%s@@", cls);   // the type descriptor's name field, MSVC's decoration
    size_t nl = strlen(name) + 1;
    Range r[24];
    int nr = readable_ranges(m, r, 24);
    const BYTE *base = (const BYTE *)m, *td = NULL;
    for (int i = 0; i < nr && !td; i++)
        for (size_t o = 16; o + nl <= r[i].n; o++)   // the name starts 16 bytes into its descriptor
            if (r[i].p[o] == (BYTE)name[0] && !memcmp(r[i].p + o, name, nl)) {
                td = r[i].p + o - 16;
                break;
            }
    if (!td) return NULL;
    const DWORD td_rva = (DWORD)(td - base);
    const BYTE *col = NULL;   // signature 1 (64-bit), offset 0, this descriptor, and the locator's own address
    for (int i = 0; i < nr && !col; i++)
        for (size_t o = 0; o + 24 <= r[i].n; o += 4) {
            const DWORD *w = (const DWORD *)(r[i].p + o);
            if (w[0] == 1 && w[1] == 0 && w[3] == td_rva && w[5] == (DWORD)(r[i].p + o - base)) {
                col = r[i].p + o;
                break;
            }
        }
    if (!col) return NULL;
    for (int i = 0; i < nr; i++)   // the vtable's first entry follows the pointer to the locator
        for (size_t o = 0; o + 16 <= r[i].n; o += 8)
            if (*(const uint64_t *)(r[i].p + o) == (uint64_t)(uintptr_t)col) return (void **)(r[i].p + o + 8);
    return NULL;
}

static void model_hook(void) {
    const char *m = cfg_get("TOTK_FSR_MODEL");
    if (!m || !*m || !strcmp(m, "auto")) return;
    int want = -1;
    if (strcmp(m, "probe")) {
        for (int i = 0; i < MODEL_N; i++)
            if (!strcmp(m, MODELS[i].key)) want = i;
        if (want < 0) {
            rt_log("fsr: fsr_model = %s is not one of auto, probe, fp8, int8, int8_rdna2, lite: automatic", m);
            return;
        }
    }
    wchar_t path[MAX_PATH + 64];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (len && path[len - 1] != L'\\') len--;
    path[len] = 0;
    wcscat(path, L"amd_fidelityfx_upscaler_vk.dll");
    // the loader takes this module by its name when it asks for the upscaler, so the patch is in before any question
    HMODULE mod = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        rt_log("fsr: fsr_model: cannot load amd_fidelityfx_upscaler_vk.dll (error %lu): automatic", GetLastError());
        return;
    }
    int found = 0;
    for (int i = 0; i < MODEL_N; i++) {
        MH.vtable[i] = find_vtable(mod, MODELS[i].cls);
        found += MH.vtable[i] != NULL;
    }
    if (found < 2) {   // an older DLL has one model and another layout
        rt_log("fsr: fsr_model: the DLL has %d FSR 4 provider classes, nothing to choose from: automatic", found);
        return;
    }
    MH.want = want;
    char have[160] = "";
    for (int i = 0; i < MODEL_N; i++) {
        if (!MH.vtable[i]) continue;
        MH.orig[i] = (PfnIsSupported)MH.vtable[i][2];
        DWORD old = 0;
        if (!VirtualProtect(&MH.vtable[i][2], sizeof(void *), PAGE_READWRITE, &old)) {
            MH.vtable[i] = NULL;
            continue;
        }
        MH.vtable[i][2] = (void *)model_is_supported;
        VirtualProtect(&MH.vtable[i][2], sizeof(void *), old, &old);
        size_t used = strlen(have);
        snprintf(have + used, sizeof have - used, "%s%s", used ? ", " : "", MODELS[i].name);
    }
    rt_log("fsr: fsr_model = %s: FSR 4 models in the DLL: %s", m, have);
}

static int has(const VkExtensionProperties *have, uint32_t nhave, const char *name) {
    for (uint32_t i = 0; i < nhave; i++)
        if (!strcmp(have[i].extensionName, name)) return 1;
    return 0;
}

static void add(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max,
                const char *name) {
    for (uint32_t i = 0; i < *n; i++)
        if (!strcmp(ext[i], name)) return;
    if (has(have, nhave, name) && *n < max) ext[(*n)++] = name;
}

// The extensions the FSR SDK's own sample enables for its effects (Cauldron2 device_vk.cpp): cooperative matrix and
// 8-bit floats for FSR 4's network where the device has them. The rest it uses (16-bit floats and storage, integer
// dot products, subgroup size control) is core Vulkan 1.3, which vk_device.c enables as far as the driver reports it.
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max) {
    if (!vk_ffx_wanted()) return;
    add(have, nhave, ext, n, max, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
#ifdef VK_EXT_SHADER_FLOAT8_EXTENSION_NAME
    add(have, nhave, ext, n, max, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
#endif
}

void vk_ffx_init(void) {
    if (!vk_ffx_wanted() || !loader_load()) return;
    const VkCtx *c = vk_ctx();
    F.dev = (VkDeviceContext){c->device, c->phys, vkGetDeviceProcAddr};
    model_hook();   // before the library asks which providers support this device
    const char *js = cfg_get("TOTK_FSR_JITTER_SIGN");
    if (js && !strcmp(js, "-1")) F.jitter_sign = -1.0f;
    uint64_t count = FFX_MAX_VERSIONS, ids[FFX_MAX_VERSIONS] = {0};
    const char *names[FFX_MAX_VERSIONS] = {0};
    struct ffxQueryDescGetVersions q = {.header = {FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, NULL},
                                        .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .device = &F.dev,
                                        .outputCount = &count, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t r = F.query(NULL, &q.header);
    if (r != FFX_API_RETURN_OK || !count) {
        rt_log("fsr: no upscaler for this device (query %u, %llu versions; is amd_fidelityfx_upscaler_vk.dll next to "
               "totk.exe?): FSR off", (unsigned)r, (unsigned long long)count);
        return;
    }
    if (count > FFX_MAX_VERSIONS) count = FFX_MAX_VERSIONS;
    char list[512] = "";
    int best = -1, best_major = 0, best_minor = 0, best_patch = 0, want = -1;
    // TOTK_FSR_VERSION: a version's name from the list in the log ("4.1.2", or the start of one, "3.1"), or its id in
    // hex ("0x..."), instead of the newest FSR 4; anything the library does not offer leaves the choice automatic
    const char *pin = cfg_get("TOTK_FSR_VERSION");
    if (pin && (!*pin || !strcmp(pin, "auto"))) pin = NULL;
    for (uint64_t i = 0; i < count; i++) {
        const char *nm = names[i] ? names[i] : "?";
        size_t used = strlen(list);
        snprintf(list + used, sizeof list - used, "%s%s (id %#llx)", i ? ", " : "", nm, (unsigned long long)ids[i]);
        if (pin && want < 0 && (!strncmp(nm, pin, strlen(pin)) || (!strncmp(pin, "0x", 2) && strtoull(pin, NULL, 16) == ids[i])))
            want = (int)i;
        int ma = 0, mi = 0, pa = 0;
        if (sscanf(nm, "%d.%d.%d", &ma, &mi, &pa) < 1 || ma < 4) continue;
        if (best < 0 || ma > best_major || (ma == best_major && (mi > best_minor || (mi == best_minor && pa > best_patch))))
            best = (int)i, best_major = ma, best_minor = mi, best_patch = pa;
    }
    if (pin && want < 0) rt_log("fsr: fsr_version = %s is not among the versions offered: automatic", pin);
    if (want >= 0) best = want;   // a pinned version, FSR 4 or not
    if (best >= 0) {
        F.version_id = ids[best];
        snprintf(F.version, sizeof F.version, "FSR %s", names[best]);
    } else {
        snprintf(F.version, sizeof F.version, "FSR %s", names[0] ? names[0] : "?");   // the DLL's default
    }
    F.available = 1;
    rt_log("fsr: upscaler versions for this device: %s; using %s%s", list, F.version,
           best >= 0 ? "" : " (FSR 4 is not offered for this GPU)");
}

int vk_ffx_available(void) { return vk_ffx_wanted() && F.available; }

const char *vk_ffx_version(void) { return F.version; }

// ---- frame generation through the loader (vk_ffx.h vk_ffx_fg_pick)
#define FG_EFFECT 0x00020000u
#define FG_DESC_CREATE FFX_API_MAKE_EFFECT_SUB_ID(FG_EFFECT, 0x01)
#define FG_DESC_CREATE_VERSION FFX_API_MAKE_EFFECT_SUB_ID(FG_EFFECT, 0x0e)
#define FG_API_VERSION ((4u << 22) | (0u << 12) | 2u)   // FFX_FRAMEGENERATION_VERSION of the SDK 2.4 header: 4.0.2
typedef ffxReturnCode_t (*PfnFfxConfigure2)(ffxContext *context, const ffxConfigureDescHeader *desc);
static struct {
    int tried, ok;
    PfnFfxConfigure2 configure;
    uint64_t id;
    char name[48];
} FG;

// fg_provider = ml: the ML provider's own support check (ffxProvider::IsSupported, the third vtable entry as for the
// upscaler's models above) answers yes whatever the DLL thinks of the device, to try FSR 4 frame generation on a GPU
// AMD does not list (RDNA 3). Its networks may then fail on the device; the log says what the DLL itself answered.
static PfnIsSupported fg_ml_orig;
static volatile int fg_ml_said;
static bool fg_ml_is_supported(void *self, void *device) {
    bool own = fg_ml_orig(self, device);
    if (device && !__atomic_exchange_n(&fg_ml_said, 1, __ATOMIC_RELAXED))
        rt_log("fg: ML frame generation provider: the DLL says %s for this device; fg_provider = ml lets it run",
               own ? "supported" : "not supported");
    return true;
}

static void fg_ml_hook(void) {
    wchar_t path[MAX_PATH + 64];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (len && path[len - 1] != L'\\') len--;
    path[len] = 0;
    wcscat(path, L"amd_fidelityfx_framegeneration_vk.dll");
    HMODULE mod = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    void **vt = mod ? find_vtable(mod, "ffxProvider_MLFrameGeneration") : NULL;
    DWORD old = 0;
    if (!vt || !VirtualProtect(&vt[2], sizeof(void *), PAGE_READWRITE, &old)) {
        rt_log("fg: fg_provider = ml: no ML frame generation provider found in amd_fidelityfx_framegeneration_vk.dll");
        return;
    }
    fg_ml_orig = (PfnIsSupported)vt[2];
    vt[2] = (void *)fg_ml_is_supported;
    VirtualProtect(&vt[2], sizeof(void *), old, &old);
    rt_log("fg: fg_provider = ml: the ML frame generation provider is let run on any device");
}

int vk_ffx_fg_pick(char *out, size_t n) {
    if (!FG.tried) {
        FG.tried = 1;
        const char *want = cfg_value("TOTK_FG_PROVIDER");
        if (!strcmp(want, "fsr3")) {
            rt_log("fg: fg_provider = fsr3: FSR 3.1 frame generation of amd_fidelityfx_vk.dll");
        } else if (loader_load() && (FG.configure = (PfnFfxConfigure2)(void *)GetProcAddress(F.dll, "ffxConfigure"))) {
            const VkCtx *c = vk_ctx();
            F.dev = (VkDeviceContext){c->device, c->phys, vkGetDeviceProcAddr};
            if (!strcmp(want, "ml")) fg_ml_hook();   // before the library asks the providers
            uint64_t count = FFX_MAX_VERSIONS, ids[FFX_MAX_VERSIONS] = {0};
            const char *names[FFX_MAX_VERSIONS] = {0};
            struct ffxQueryDescGetVersions q = {.header = {FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, NULL},
                                                .createDescType = FG_DESC_CREATE, .device = &F.dev,
                                                .outputCount = &count, .versionIds = ids, .versionNames = names};
            ffxReturnCode_t r = F.query(NULL, &q.header);
            if (count > FFX_MAX_VERSIONS) count = FFX_MAX_VERSIONS;
            char list[512] = "";
            int best = -1, bma = 0, bmi = 0, bpa = 0;
            for (uint64_t i = 0; r == FFX_API_RETURN_OK && i < count; i++) {
                const char *nm = names[i] ? names[i] : "?";
                size_t used = strlen(list);
                snprintf(list + used, sizeof list - used, "%s%s (id %#llx)", i ? ", " : "", nm, (unsigned long long)ids[i]);
                int ma = 0, mi = 0, pa = 0;
                if (sscanf(nm, "%d.%d.%d", &ma, &mi, &pa) < 1 || ma < 4) continue;
                if (best < 0 || ma > bma || (ma == bma && (mi > bmi || (mi == bmi && pa > bpa))))
                    best = (int)i, bma = ma, bmi = mi, bpa = pa;
            }
            rt_log("fg: frame generation versions of the FSR SDK loader for this device (query %u): %s", (unsigned)r,
                   *list ? list : "none (is amd_fidelityfx_framegeneration_vk.dll next to totk.exe?)");
            if (best >= 0) {
                FG.id = ids[best], FG.ok = 1;
                snprintf(FG.name, sizeof FG.name, "FSR Frame Generation %s", names[best]);
            } else {
                rt_log("fg: no FSR 4 frame generation offered%s: FSR 3.1 frame generation of amd_fidelityfx_vk.dll",
                       !strcmp(want, "ml") ? " (fg_provider = ml)" : "");
            }
        }
    }
    if (FG.ok && out && n) snprintf(out, n, "%s", FG.name);
    return FG.ok;
}

uint32_t vk_ffx_fg_create(void **ctx, void *desc) {
    struct ffxOverrideVersion ov = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, NULL}, .versionId = FG.id};
    struct { ffxCreateContextDescHeader header; uint32_t version; } ver = {{FG_DESC_CREATE_VERSION, &ov.header},
                                                                          FG_API_VERSION};
    struct ffxCreateBackendVKDesc be = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, &ver.header},
                                        .deviceContext = F.dev};
    ((ffxApiHeader *)desc)->pNext = &be.header;
    return F.create((ffxContext *)ctx, (ffxCreateContextDescHeader *)desc, NULL);
}

uint32_t vk_ffx_fg_configure(void **ctx, void *desc) {
    return FG.configure((ffxContext *)ctx, (const ffxConfigureDescHeader *)desc);
}

uint32_t vk_ffx_fg_dispatch(void **ctx, void *desc) {
    return F.dispatch((ffxContext *)ctx, (const ffxDispatchDescHeader *)desc);
}

void vk_ffx_fg_destroy(void **ctx) { F.destroy((ffxContext *)ctx, NULL); }

// Messages from the upscaler (errors and warnings), in UTF-16; into the log as UTF-8.
static void on_message(uint32_t type, const wchar_t *msg) {
    char s[512];
    if (!msg || !WideCharToMultiByte(CP_UTF8, 0, msg, -1, s, (int)sizeof s, NULL, NULL)) return;
    rt_log("fsr: %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", s);
}

void *vk_ffx_create(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int depth_inverted, int depth_infinite) {
    if (!F.available) return NULL;
    struct ffxOverrideVersion ov = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, NULL}, .versionId = F.version_id};
    struct ffxCreateContextDescUpscaleVersion ver = {
        .header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, F.version_id ? &ov.header : NULL},
        .version = FFX_UPSCALER_VERSION};
    struct ffxCreateBackendVKDesc be = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, &ver.header},
                                        .deviceContext = F.dev};
    // Linear HDR scene light with the upscaler's own exposure, as DLSS gets it ("IsHDR | AutoExposure")
    uint32_t flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (depth_inverted) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (depth_infinite) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (rt_knob("FSR_DEBUG", "0")) flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;   // TOTK_FSR_DEBUG=1: API checks
    struct ffxCreateContextDescUpscale cd = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &be.header},
                                             .flags = flags, .maxRenderSize = {w, h}, .maxUpscaleSize = {ow, oh},
                                             .fpMessage = on_message};
    ffxContext ctx = NULL;
    ffxReturnCode_t r = F.create(&ctx, &cd.header, NULL);
    if (r != FFX_API_RETURN_OK || !ctx) {
        rt_log("fsr: context creation failed (%u) at %ux%u -> %ux%u", (unsigned)r, w, h, ow, oh);
        return NULL;
    }
    struct ffxQueryGetProviderVersion pv = {.header = {FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, NULL}};
    F.query(&ctx, &pv.header);
    rt_log("fsr: context created at %ux%u -> %ux%u with %s (flags %#x)", w, h, ow, oh,
           pv.versionName ? pv.versionName : F.version, flags);
    return ctx;
}

void vk_ffx_release(void *ctx) {
    if (!ctx) return;
    ffxContext c = ctx;
    F.destroy(&c, NULL);
}

static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// Every image of the DLAA path stays in VK_IMAGE_LAYOUT_GENERAL: GENERIC_READ and UNORDERED_ACCESS are the states the
// Vulkan backend maps to that layout (ffx_vk.cpp getVKImageLayoutFromResourceState), and it puts each resource back
// into the state it was given at the end of the dispatch.
static struct FfxApiResource resource(const VkFfxImage *im, uint32_t usage, uint32_t state) {
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    if (!im->image) return r;
    r.resource = (void *)im->image;
    r.description = (struct FfxApiResourceDescription){.type = FFX_API_RESOURCE_TYPE_TEXTURE2D,
                                                       .format = surface_format(im->format), .width = im->width,
                                                       .height = im->height, .depth = 1, .mipCount = 1, .usage = usage};
    r.state = state;
    return r;
}

int vk_ffx_dispatch(void *ctx, VkCommandBuffer cb, const VkFfxDispatch *d) {
    if (!ctx) return 0;
    ffxContext c = ctx;
    struct ffxDispatchDescUpscale u;
    memset(&u, 0, sizeof u);
    u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    u.commandList = (void *)cb;
    u.color = resource(&d->color, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.depth = resource(&d->depth, FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.motionVectors = resource(&d->mv, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.reactive = resource(&d->reactive, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.output = resource(&d->out, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    u.jitterOffset = (struct FfxApiFloatCoords2D){d->jx * F.jitter_sign, d->jy * F.jitter_sign};
    u.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};
    u.renderSize = (struct FfxApiDimensions2D){d->color.width, d->color.height};
    u.upscaleSize = (struct FfxApiDimensions2D){d->out.width, d->out.height};
    u.frameTimeDelta = d->dt_ms;
    u.preExposure = 1.0f;
    u.reset = d->reset != 0;
    // FSR's sharpening pass (RCAS), as the FSR 3 path (vk_fsr3.c) sets it: without it the upscaled picture keeps about
    // half of the native picture's edge contrast (measured on the Temple of Time from the opening dive, 1.4.3).
    u.enableSharpening = d->sharpness > 0.0f;
    u.sharpness = d->sharpness;
    static int sharp_logged;
    if (!sharp_logged) sharp_logged = 1, rt_log("fsr: sharpening %s (sharpness %.2f)", u.enableSharpening ? "on" : "off", (double)d->sharpness);
    u.cameraNear = d->cam_near;
    u.cameraFar = d->cam_far;
    u.cameraFovAngleVertical = d->fov_y;
    u.viewSpaceToMetersFactor = 1.0f;   // TotK's world units are metres
    ffxReturnCode_t r = F.dispatch(&c, &u.header);
    if (r != FFX_API_RETURN_OK) {
        rt_log("fsr: dispatch failed (%u)", (unsigned)r);
        return 0;
    }
    return 1;
}
#endif
