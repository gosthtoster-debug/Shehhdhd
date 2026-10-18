// cheat.cpp | arm64 Android | Oxide: Survival Island | ESP
//
// ONE source, TWO binaries (built by @fyxod_cpp.sh):
//   libcheat.so  (-DPAYLOAD)  injected into the game. il2cpp collector only, no UI.
//                             writes entity screen coords into a shared file (SHM_PATH).
//   @fyxod_cpp      (-DHOST)     root process. ptrace-injects libcheat.so, then draws the overlay
//                             (SurfaceFlinger surface + EGL/GLES3 + ImGui + evdev touch) from the shared file.
// why split: libgui (needed to create the overlay surface) is private -> an app process can't dlopen it
//            (linker namespace), and /dev/input needs root. so UI lives in the root process, game-side stays tiny.
//
// build (@fyxod_cpp.sh does this):
//   clang++ -shared -fPIC -O2 -std=c++20 -fvisibility=hidden -DPAYLOAD -fno-exceptions -fno-rtti -nostdlib++ \
//           cheat.cpp -ldl -llog -lm -Wl,--no-undefined -o libcheat.so
//   clang++ -O2 -std=c++20 -DHOST cheat.cpp <imgui cpps> <touch cpp> -Iinclude ... -llog -ldl -landroid -lEGL -lGLESv3 -o @fyxod_cpp
//
// run:  su -c /data/local/tmp/@fyxod_cpp      (menu: tap [M] in the corner)

// ═══════════════════════════════════════════════════════════════════════════
//  OFFSETS / CONFIG — единый блок, меняй здесь
// ═══════════════════════════════════════════════════════════════════════════
namespace off {
    constexpr const char* PKG        = "com.catsbit.oxidesurvivalisland";
    constexpr const char* IL2CPP_NS  = "Oxide";
    constexpr const char* IL2CPP_CLS = "PlayerManager";
    constexpr const char* F_EVENTS   = "playerEventHandler";
    constexpr const char* F_HEALTH   = "Health";
    constexpr const char* F_TEAM     = "teamName";
    constexpr const char* M_HP_A     = "Kek";   // FILL: obfuscated, re-dump with Il2CppDumper after each game update
    constexpr const char* M_HP_B     = "KeA";
    constexpr float       HEAD_H     = 1.8f;
    constexpr int         POLL_MS    = 50;
    constexpr int         MAX_ENT    = 64;
    constexpr float       AIM_STEP   = 8.f;    // max deg/frame written per axis
    constexpr const char* CFG_PATH   = "/data/local/tmp/.oxide_cfg.json";
    constexpr const char* LIB_DST    = "/data/data/com.catsbit.oxidesurvivalisland/cache/libcheat.so";
    constexpr const char* SHM_PATH   = "/data/data/com.catsbit.oxidesurvivalisland/cache/.oxe.shm";
}

#include <stdint.h>
#include <stddef.h>
#ifdef NOLOG
#define EPRINT(...) ((void)0)
#else
#define EPRINT(...) fprintf(stderr, __VA_ARGS__)
#endif

// ── shared between payload and host (identical layout, both compiled from this file) ──
enum Joint { J_HEAD, J_NECK, J_CHEST, J_HIPS, J_LSH, J_LEL, J_LHA, J_RSH, J_REL, J_RHA,
             J_LHIP, J_LKN, J_LFT, J_RHIP, J_RKN, J_RFT, J_N };
struct Ent {
    float fx, fy, hx, hy;   // feet/head screen px, Unity convention: origin bottom-left, game resolution (sw x sh)
    float hp, hpMax, dist;
    int   team;             // 1 = same team as local player
    bool  alive;
    uint32_t jmask;         // bit j set = joint j visible on screen (jx/jy valid); 0 = no skeleton data
    float jx[J_N], jy[J_N]; // joint screen px, same convention as fx/fy
    char  name[32];         // player display name, utf-8, refreshed ~1/s
};
struct Ctl {                // host -> payload (plain writes, torn reads are harmless)
    int32_t aim;            // master toggle
    int32_t active;         // engaged this frame (host resolves always-on vs hold-button)
    int32_t aimTeam;        // also target teammates
    int32_t esp;            // host: esp on (payload idles when esp and aim are both off)
    int32_t ready;          // payload -> host: payload is up
    int32_t noSpread;       // zero weapon spread fields every frame
    float   fov;            // game px radius around screen centre
    float   smooth;         // >= 1, higher = slower
    float   aimH;           // 0 = feet .. 1 = top of head
    float   vfov;           // vertical camera fov, deg (px -> deg)
    float   maxDist;        // m
    int32_t skel;           // host: skeleton esp on -> payload collects joints
    int32_t aimBone;        // 0 head, 1 neck, 2 chest, 3 pelvis, 4 closest joint to crosshair
    int32_t lead;           // predict target motion + bullet drop
    float   leadSpeed;      // m/s, 0 = read BulletSpeed from the equipped hitscan config
};
struct Shm {
    volatile uint32_t seq;  // seqlock: odd = writer busy
    int32_t  cnt, sw, sh;
    uint64_t ts_ms;         // CLOCK_MONOTONIC ms of last write, host drops stale data
    Ent      e[off::MAX_ENT];
    Ctl      ctl;
};

#ifdef PAYLOAD
// ═══════════════════════════════════════════════════════════════════════════
//  PAYLOAD — runs inside the game. pure C-style on purpose (no libc++ dependency).
//  this build's libil2cpp.so does NOT export il2cpp_* (241 exports are hashed), so nothing is resolved by name:
//  everything is base+RVA from dump.cs / fixed field offsets. REGENERATE rva:: + off2:: after every game update.
// ═══════════════════════════════════════════════════════════════════════════
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <android/log.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef NOLOG
#define LOG(...) ((void)0)
#else
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "@fyxod_cpp", __VA_ARGS__)
#endif

namespace rva {                       // dump.cs "RVA:" (== VA), added to libil2cpp load base
    constexpr uintptr_t Update       = 0x6A4FC4C;   // Oxide.PlayerManager.Update()          (private, hooked)
    constexpr uintptr_t IsLocal      = 0xC16A71C;   // Mirror.NetworkBehaviour.get_isLocalPlayer()
    constexpr uintptr_t CamMain      = 0xCCBE5C8;   // UnityEngine.Camera.get_main()          static
    constexpr uintptr_t W2S          = 0xCCBDB1C;   // UnityEngine.Camera.WorldToScreenPoint(Vector3)
    constexpr uintptr_t GetTransform = 0xCD39DD4;   // UnityEngine.Component.get_transform()
    constexpr uintptr_t GetPosition  = 0xCD53504;   // UnityEngine.Transform.get_position()
    constexpr uintptr_t ScrW         = 0xCCD1820;   // UnityEngine.Screen.get_width()         static
    constexpr uintptr_t ScrH         = 0xCCD1848;   // UnityEngine.Screen.get_height()        static
    constexpr uintptr_t BalStep      = 0xC2FBF10;   // Oxide.Utils.Combat.BallisticIntegrator.Step(ref BallisticState, in BallisticParams, float dt) static
    constexpr uintptr_t GetGO        = 0xCD39F78;   // UnityEngine.Component.get_gameObject()
    constexpr uintptr_t GoActive     = 0xCD3F428;   // UnityEngine.GameObject.get_activeInHierarchy()
    constexpr uintptr_t GetBone      = 0xCC9579C;   // UnityEngine.Animator.GetBoneTransform(HumanBodyBones) -> Transform, null if not humanoid
    // first 4 words of Update(): must match, they are copied into the trampoline (PC-independent, checked offline)
    constexpr uint32_t UpdatePrologue[4] = {0xd10203ff, 0x6d0323e9, 0xa90467fe, 0xa9055ff8};
}
namespace off2 {                      // field offsets (dump.cs / il2cpp.h)
    constexpr uintptr_t pm_events = 0x78;    // PlayerManager.playerEventHandler (hsL : hsN)
    constexpr uintptr_t pm_vitals = 0xC8;    // PlayerManager.vitals
    constexpr uintptr_t pm_animator = 0x1B0; // PlayerManager.animator (Animator) - dump.cs, humanoid rig assumed
    constexpr uintptr_t pm_team   = 0x2A0;   // PlayerManager.teamName (string)
    constexpr uintptr_t ev_health = 0x98;    // hsN.Health -> hkz<float,..>
    constexpr uintptr_t hk_value  = 0x20;    // hkz<float,..> current value; verified in libil2cpp: Kek() = ldr s0,[x0,#0x20]
    constexpr uintptr_t gv_maxhp  = 0x88;    // GenericVitals.m_MaxHealth
    // aim write target: PlayerManager.mouseLook @0x70 is Oxide.MouseLook (il2cpp.h: Oxide_PlayerManager_Fields.mouseLook = Oxide_MouseLook_o*).
    //   NOT ECM.Components.MouseLook: the OUE/OUY quaternions @0x48/0x58 belong to that other class, here 0x48 is a float and 0x58 a pointer.
    //   JZC @0x4C = Vector2 look state (getter KvN = `ldp s0,s1,[x0,#0x4C]`, setter Mkz = `stp s0,s1,[x0,#0x4C]`; Update() adds to .y at 0x50).
    //   which component is yaw/pitch, the sign and the unit are NOT known: aimStep measures them at runtime (jacobian probe).
    constexpr uintptr_t pm_look   = 0x70;
    constexpr uintptr_t ml_ang    = 0x4C;
    // weapons (dump.cs). guns are FPHitscan; their HitscanWeaponConfig / DispersionConfig are shared ScriptableObjects
    constexpr uintptr_t klass_name   = 0x10;   // Il2CppClass.name (il2cpp v29+, x64) - used to recognise FPHitscan
    constexpr uintptr_t pm_weapons   = 0x1B8;  // PlayerManager.weapons (FPWeaponBase[])
    constexpr uintptr_t fw_config    = 0x130;  // FPHitscan.config (HitscanWeaponConfig)
    constexpr uintptr_t hc_damage    = 0x18;   // HitscanWeaponConfig.damage            (sanity check)
    constexpr uintptr_t hc_bulletSpeed = 0x30; // HitscanWeaponConfig.BulletSpeed       (lead)
    constexpr uintptr_t hc_baseDisp  = 0x3C;   // HitscanWeaponConfig.BaseDispersion    (no-spread)
    constexpr uintptr_t hc_ballistic = 0x60;   // HitscanWeaponConfig.ballistic (BallisticProfile)
    constexpr uintptr_t hc_dispCfg   = 0x68;   // HitscanWeaponConfig.dispersionConfig  (no-spread)
    constexpr uintptr_t bp_gravity   = 0x10;   // BallisticProfile.gravity              (lead drop)
    constexpr uintptr_t dc_first     = 0x18;   // DispersionConfig: minFire maxFire min max increasePerShot = 5 floats
    constexpr uintptr_t ev_name      = 0x88;   // hsN.DisplayName
}

struct V3 { float x, y, z; };
struct Name { char16_t c[64]; int n; };

static Shm* g_shm = nullptr;
static uintptr_t g_base = 0;

static uint64_t nowMs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
static bool shmOpen() {
    int fd = open(off::SHM_PATH, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { LOG("shm open failed"); return false; }
    fchmod(fd, 0666);
    if (ftruncate(fd, sizeof(Shm)) != 0) { close(fd); return false; }
    void* p = mmap(nullptr, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    g_shm = (Shm*)p;
    if (g_shm->seq & 1) g_shm->seq++;
    return true;
}
static void publish(const Ent* e, int cnt, int sw, int sh) {
    if (!g_shm) return;
    g_shm->seq++; __sync_synchronize();
    if (cnt > 0) memcpy(g_shm->e, e, (size_t)cnt * sizeof(Ent));
    g_shm->cnt = cnt; g_shm->sw = sw; g_shm->sh = sh; g_shm->ts_ms = nowMs();
    __sync_synchronize(); g_shm->seq++;
}

// il2cpp string: int32 length @0x10, utf16 @0x14
static Name readName(void* s) {
    Name r; r.n = 0;
    if (!s) return r;
    int len = *(int*)((uintptr_t)s + 0x10);
    if (len <= 0 || len > 64) return r;
    memcpy(r.c, (void*)((uintptr_t)s + 0x14), (size_t)len * 2);
    r.n = len; return r;
}
static bool sameName(const Name& a, const Name& b) {
    return a.n > 0 && a.n == b.n && memcmp(a.c, b.c, (size_t)a.n * 2) == 0;
}

// ── minimal arm64 inline hook (16-byte patch, prologue verified against this build) ──
static bool patchCode(uintptr_t addr, const void* data, size_t n) {
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t s = addr & ~(uintptr_t)(ps - 1), e = (addr + n + (uintptr_t)ps - 1) & ~(uintptr_t)(ps - 1);
    if (mprotect((void*)s, e - s, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        memcpy((void*)addr, data, n);
        mprotect((void*)s, e - s, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char*)addr, (char*)(addr + n));
        return true;
    }
    int fd = open("/proc/self/mem", O_RDWR);                 // fallback: selinux denied execmod, write through /proc/self/mem
    if (fd >= 0) {
        ssize_t w = pwrite(fd, data, n, (off_t)addr); close(fd);
        if (w == (ssize_t)n) { __builtin___clear_cache((char*)addr, (char*)(addr + n)); return true; }
    }
    return false;
}
static void* hookArm64(uintptr_t target, void* hook, const uint32_t expect[4]) {
    uint32_t cur[4]; memcpy(cur, (void*)target, 16);
    if (memcmp(cur, expect, 16) != 0) { LOG("prologue mismatch %08x %08x %08x %08x -> game updated, regen rva::", cur[0], cur[1], cur[2], cur[3]); return nullptr; }
    uint32_t* tr = (uint32_t*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tr == MAP_FAILED) { LOG("trampoline mmap failed"); return nullptr; }
    memcpy(tr, cur, 16);                                     // original first 4 instructions
    tr[4] = 0x58000050;                                      // ldr x16, #8
    tr[5] = 0xD61F0200;                                      // br  x16
    uint64_t back = (uint64_t)target + 16; memcpy(&tr[6], &back, 8);
    __builtin___clear_cache((char*)tr, (char*)(tr + 8));
    uint32_t patch[4] = {0x58000050, 0xD61F0200, 0, 0};      // ldr x16,#8 ; br x16 ; .quad hook
    uint64_t h = (uint64_t)hook; memcpy(&patch[2], &h, 8);
    if (!patchCode(target, patch, 16)) { LOG("patch failed (mprotect + /proc/self/mem both denied)"); return nullptr; }
    return tr;
}

// ── game function pointers (base + rva) ──
typedef bool  (*BoolFn)(void*, void*);
typedef int   (*IntFn)(void*);
typedef void* (*ObjFn0)(void*);
typedef void* (*ObjFn1)(void*, void*);
typedef V3    (*PosFn)(void*, void*);
typedef V3    (*W2SFn)(void*, V3, void*);
typedef void* (*BoneFn)(void*, int, void*);
struct BParams { float gravity, airDrag, underwaterDrag; };   // Oxide.Utils.Combat.BallisticParams (dump.cs: 0x0/0x4/0x8)
struct BState  { V3 pos, vel; };                              // BallisticState: Position @0x0, Velocity @0xC
typedef void  (*BalStepFn)(BState*, const BParams*, float, void*);

static BoolFn fIsLocal; static IntFn fScrW, fScrH; static ObjFn0 fCamMain;
static ObjFn1 fGetTransform; static PosFn fGetPosition; static W2SFn fW2S; static BoneFn fGetBone; static ObjFn1 fGetGO; static BoolFn fGoActive; static BalStepFn fBalStep;
static void (*orig_update)(void*, void*) = nullptr;

struct Slot { void* obj; uint64_t ts; uint64_t teamTs; Ent e; V3 wfeet; V3 jw[J_N]; uint32_t jwMask; uint64_t boneRetry; V3 pp, vel; uint64_t pt, nameTs; };
static Slot g_slots[off::MAX_ENT];
static Name g_localTeam;
static int  g_sw = 0, g_sh = 0;
static uint64_t g_lastPub = 0;

static Slot* slotFor(void* obj, uint64_t now) {
    Slot* freeS = nullptr; Slot* oldest = &g_slots[0];
    for (int i = 0; i < off::MAX_ENT; i++) {
        Slot& s = g_slots[i];
        if (s.obj == obj) return &s;
        if (!s.obj || now - s.ts > 2000) { if (!freeS) freeS = &s; }
        if (s.ts < oldest->ts) oldest = &s;
    }
    Slot* s = freeS ? freeS : oldest;
    s->obj = obj; s->ts = 0; s->teamTs = 0; s->nameTs = 0; s->pt = 0; s->vel = V3{0.f, 0.f, 0.f}; s->e.name[0] = 0; return s;
}
static void dropSlot(void* obj) {
    for (int i = 0; i < off::MAX_ENT; i++) if (g_slots[i].obj == obj) { g_slots[i].obj = nullptr; g_slots[i].ts = 0; }
}

static void toUtf8(const Name& n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n.n && o < cap - 4; i++) {
        uint32_t c = n.c[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n.n) { c = 0x10000 + ((c - 0xD800) << 10) + (n.c[++i] - 0xDC00); }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 63)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
}

// ── weapons: no-spread, plus bullet speed / gravity of the equipped gun for the lead calculation ──
// HitscanWeaponConfig + DispersionConfig are shared ScriptableObjects: originals are saved and put back when the toggle goes off.
// NOT verified on device: if the game copies these values into runtime state once at equip, zeroing the config won't tighten the spread.
struct WpSaved { void* cfg; float base; void* disp; float d[5]; bool on; };
static WpSaved  g_ws[6];
static float    g_bulletSpeed = 0.f, g_gravity = 0.f;
static BParams  g_bal{}; static bool g_balOk = false;     // equipped gun's BallisticProfile, fed to the game's own integrator
static uint64_t g_wpTs = 0;
static void*    g_hsKlass = nullptr;
static bool plaus(float v, float lo, float hi) { return __builtin_isfinite(v) && v >= lo && v <= hi; }

static bool isHitscan(void* w) {                                   // class name check: the config field offset only means something on FPHitscan
    void* k = *(void**)w; if (!k) return false;
    if (k == g_hsKlass) return true;
    const char* nm = *(const char**)((uintptr_t)k + off2::klass_name);
    if ((uintptr_t)nm < 0x10000) return false;
    if (strcmp(nm, "FPHitscan") != 0) return false;
    g_hsKlass = k; return true;
}
static void* activeHitscanCfg(void* self) {
    void* arr = *(void**)((uintptr_t)self + off2::pm_weapons); if (!arr) return nullptr;
    uintptr_t len = *(uintptr_t*)((uintptr_t)arr + 0x18);          // il2cpp array: max_length @0x18, items @0x20
    if (len == 0 || len > 16) return nullptr;
    for (uintptr_t i = 0; i < len; i++) {
        void* w = *(void**)((uintptr_t)arr + 0x20 + 8 * i); if (!w) continue;
        void* go = fGetGO(w, nullptr); if (!go || !fGoActive(go, nullptr)) continue;   // the equipped one is the active one
        if (!isHitscan(w)) continue;
        void* cfg = *(void**)((uintptr_t)w + off2::fw_config); if (!cfg) continue;
        float dmg = *(float*)((uintptr_t)cfg + off2::hc_damage), bd = *(float*)((uintptr_t)cfg + off2::hc_baseDisp);
        if (plaus(dmg, 0.f, 100000.f) && plaus(bd, 0.f, 360.f)) return cfg;
    }
    return nullptr;
}
static void noSpreadStep(void* self) {
    if (!g_shm) return;
    uint64_t now = nowMs();
    if (now - g_wpTs < 250) return;
    g_wpTs = now;
    const Ctl& c = g_shm->ctl;
    bool ns = c.noSpread != 0;
    void* cfg = (ns || c.lead) ? activeHitscanCfg(self) : nullptr;
    g_bulletSpeed = 0.f; g_gravity = 0.f; g_balOk = false;
    if (cfg) {
        float bs = *(float*)((uintptr_t)cfg + off2::hc_bulletSpeed);
        if (plaus(bs, 1.f, 10000.f)) g_bulletSpeed = bs;
        void* bal = *(void**)((uintptr_t)cfg + off2::hc_ballistic);
        if (bal) {
            float g = *(float*)((uintptr_t)bal + off2::bp_gravity), dr = *(float*)((uintptr_t)bal + off2::bp_gravity + 4), uw = *(float*)((uintptr_t)bal + off2::bp_gravity + 8);
            if (plaus(g, -60.f, 60.f)) g_gravity = fabsf(g);
            if (plaus(g, -60.f, 60.f) && plaus(dr, 0.f, 50.f) && plaus(uw, 0.f, 500.f)) {
                g_bal = BParams{g, dr, uw}; g_balOk = true;
                static float lg = -1e9f, ld = -1e9f, ls = -1e9f;
                if (g != lg || dr != ld || bs != ls) { lg = g; ld = dr; ls = bs; LOG("ballistics: speed=%.0f gravity=%.2f airDrag=%.3f underwaterDrag=%.2f", bs, g, dr, uw); }
            }
        }
    }
    if (ns && cfg) {
        void* disp = *(void**)((uintptr_t)cfg + off2::hc_dispCfg);
        WpSaved* sv = nullptr;
        for (auto& x : g_ws) if (x.on && x.cfg == cfg) { sv = &x; break; }
        if (!sv) {
            for (auto& x : g_ws) if (!x.on) { sv = &x; break; }
            if (!sv) return;
            sv->cfg = cfg; sv->disp = disp; sv->base = *(float*)((uintptr_t)cfg + off2::hc_baseDisp);
            for (int i = 0; i < 5; i++) sv->d[i] = disp ? *(float*)((uintptr_t)disp + off2::dc_first + 4 * i) : 0.f;
            sv->on = true;
            LOG("nospread: cfg=%p base=%.3f disp=%p (%.3f %.3f %.3f %.3f %.3f) bulletSpeed=%.0f", cfg, sv->base, disp, sv->d[0], sv->d[1], sv->d[2], sv->d[3], sv->d[4], g_bulletSpeed);
        }
        *(float*)((uintptr_t)cfg + off2::hc_baseDisp) = 0.f;
        if (disp) for (int i = 0; i < 5; i++) {
            float* f = (float*)((uintptr_t)disp + off2::dc_first + 4 * i);
            if (plaus(*f, 0.f, 1000.f)) *f = 0.f;                 // wrong offset = value won't look like a dispersion = no write
        }
    }
    if (!ns) for (auto& x : g_ws) if (x.on) {
        *(float*)((uintptr_t)x.cfg + off2::hc_baseDisp) = x.base;
        if (x.disp) for (int i = 0; i < 5; i++) *(float*)((uintptr_t)x.disp + off2::dc_first + 4 * i) = x.d[i];
        x.on = false;
    }
}
static inline V3    v3sub(V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3    v3add(V3 a, V3 b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V3    v3mul(V3 a, float k) { return V3{a.x * k, a.y * k, a.z * k}; }
static inline float v3dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// ballistics: runs the game's own BallisticIntegrator, so gravity + drag match the server model exactly (no guessed drag formula).
// fixed point: fire at A, see where the bullet crosses the plane of the target, move A by the miss, repeat.
// `aim` = point to put the crosshair on, `tof` = time of flight. false = bullet never reaches that range (too slow / too far).
// assumes the shot starts at the camera `o`; if the muzzle offset matters at close range, pass the muzzle position instead.
static bool ballisticSolve(V3 o, V3 T, float v0, const BParams& p, V3& aim, float& tof) {
    V3 toT = v3sub(T, o); float dT = sqrtf(v3dot(toT, toT));
    if (dT < 0.5f) { aim = T; tof = 0.f; return true; }
    V3 nT = v3mul(toT, 1.f / dT);
    V3 A = T;
    for (int it = 0; it < 6; it++) {
        V3 d = v3sub(A, o); float dl = sqrtf(v3dot(d, d)); if (dl < 0.01f) return false;
        BState st{o, v3mul(d, v0 / dl)};
        const float DT = 0.0125f; float t = 0.f; bool hit = false; V3 hp{0.f, 0.f, 0.f};
        for (int i = 0; i < 240; i++) {                                  // up to 3 s of flight
            V3 prev = st.pos; fBalStep(&st, &p, DT, nullptr); t += DT;
            float a0 = v3dot(v3sub(prev, o), nT), a1 = v3dot(v3sub(st.pos, o), nT);   // progress along o->T
            if (a1 >= dT) {
                float f = (a1 - a0) > 1e-6f ? (dT - a0) / (a1 - a0) : 1.f;
                hp = v3add(prev, v3mul(v3sub(st.pos, prev), f)); tof = t - DT + DT * f; hit = true; break;
            }
            if (a1 < a0 - 1.f) return false;                             // flying backwards
        }
        if (!hit) return false;
        V3 miss = v3sub(T, hp);
        A = v3add(A, miss);
        if (v3dot(miss, miss) < 0.0004f) break;                          // < 2 cm
    }
    aim = A; return true;
}
static void aimDbg(const char* tag, float a0, float a1, float e0, float e1, uint64_t now) {   // rate-limited logcat: `logcat -s @fyxod_cpp`
    static uint64_t last = 0;
    if (now - last < 2000) return;
    last = now;
    LOG("aim %s ang=(%.3f %.3f) err=(%.1f %.1f)", tag, a0, a1, e0, e1);
}

// closest-to-crosshair enemy inside fov, steer Oxide.MouseLook.JZC (Vector2 @0x4C) toward it.
// axis order / sign / unit of JZC are unknown, so the 2x2 jacobian d(screen err px)/d(JZC) is measured once:
// nudge each component by PROBE, wait, see how the target moved on screen. then newton step / smooth, closed loop every frame.
struct Cal { int st; uint64_t t; float ex, ey, c0x, c0y, J00, J01, J10, J11, det; bool ok; int tid; };
static Cal g_cal;
static uint64_t g_calRetry = 0;
static int g_lastTid = -1, g_grow = 0;
static float g_lastBest = 0.f;
static const float PROBE = 1.5f;

// camera + its position cached for one frame (16 ms): Camera.main / get_transform / get_position once, not once per player
struct CamCache { void* cam; V3 pos; uint64_t ts; };
static CamCache g_cc;
static CamCache* getCam(uint64_t now) {
    if (g_cc.cam && now - g_cc.ts < 16) return &g_cc;
    g_cc.cam = nullptr;
    void* cam = fCamMain(nullptr);              if (!cam) return nullptr;
    void* ct  = fGetTransform(cam, nullptr);    if (!ct)  return nullptr;
    g_cc.cam = cam; g_cc.pos = fGetPosition(ct, nullptr); g_cc.ts = now;
    return &g_cc;
}

// ── skeleton: 16 joints through Animator.GetBoneTransform (humanoid rig). world pos kept for fresh re-projection by the aimbot ──
static const int HB[J_N] = {10, 9, 8, 0, 13, 15, 17, 14, 16, 18, 1, 3, 5, 2, 4, 6};   // HumanBodyBones: Head Neck Chest Hips, L/R UpperArm LowerArm Hand, L/R UpperLeg LowerLeg Foot
static void collectBones(void* self, Slot* s, CamCache* cc, uint64_t now) {
    if (now < s->boneRetry) return;
    void* an = *(void**)((uintptr_t)self + off2::pm_animator);
    if (!an) { s->boneRetry = now + 2000; return; }
    for (int j = 0; j < J_N; j++) {
        void* t = fGetBone(an, HB[j], nullptr);
        if (!t && j == J_CHEST) t = fGetBone(an, 7, nullptr);          // rigs without a Chest bone: Spine
        if (!t) continue;
        V3 w = fGetPosition(t, nullptr);
        if (!(__builtin_isfinite(w.x) && __builtin_isfinite(w.y) && __builtin_isfinite(w.z))) continue;
        s->jw[j] = w; s->jwMask |= 1u << j;
        V3 sp = fW2S(cc->cam, w, nullptr);
        if (sp.z <= 0.f) continue;
        s->e.jx[j] = sp.x; s->e.jy[j] = sp.y; s->e.jmask |= 1u << j;
    }
    if (!(s->jwMask & ((1u << J_HEAD) | (1u << J_HIPS)))) s->boneRetry = now + 2000;   // not humanoid / not ready: stop hammering it
}

// ── aim: target + joint selection, sticky so it doesn't flip between players or bones ──
static const int AIM_BONES[4] = {J_HEAD, J_NECK, J_CHEST, J_HIPS};
// returns joint index, or -1 when the slot has no skeleton (caller falls back to the aimH line feet->head)
static int pickBone(const Slot& s, int mode, float cx, float cy, int prevBone, float& px, float& py) {
    uint32_t m = s.e.jmask;
    if (!m) return -1;
    if (mode >= 0 && mode < 4) {
        for (int k = 0; k < 4; k++) {
            int j = AIM_BONES[(mode + k) % 4];
            if (m & (1u << j)) { px = s.e.jx[j]; py = s.e.jy[j]; return j; }
        }
    }
    int bj = -1; float bd = 1.0e18f;
    for (int j = 0; j < J_N; j++) {
        if (!(m & (1u << j))) continue;
        float dx = s.e.jx[j] - cx, dy = s.e.jy[j] - cy, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; bj = j; }
    }
    if (bj < 0) return -1;
    if (prevBone >= 0 && (m & (1u << prevBone))) {                 // bone hysteresis: stay on the old joint unless another is clearly closer
        float dx = s.e.jx[prevBone] - cx, dy = s.e.jy[prevBone] - cy;
        if (sqrtf(dx * dx + dy * dy) <= sqrtf(bd) + 14.f) bj = prevBone;
    }
    px = s.e.jx[bj]; py = s.e.jy[bj];
    return bj;
}

static int      g_tSlot = -1, g_tBone = -1;
static void*    g_tObj = nullptr;
static uint64_t g_aimLast = 0;
static float    g_stp0 = 0.f, g_stp1 = 0.f;      // last applied step (output low-pass state)
static bool     g_holdLock = false;              // dead-zone hysteresis latch
static void aimReset() { g_tSlot = -1; g_tBone = -1; g_tObj = nullptr; g_stp0 = g_stp1 = 0.f; g_holdLock = false; }

// what changed vs the old step: (1) target error is re-projected with the CURRENT camera every frame (old code used the
// screen point published up to 150 ms earlier -> the loop chased its own camera motion = the jitter), (2) frame-rate independent
// exponential approach instead of 1/smooth per frame, (3) low-pass on the applied step (no hard start/stop), (4) dead-zone with
// hysteresis (no hunting around the point), (5) sticky target and joint.
static void aimStep(void* self, uint64_t now) {
    float dtms = (float)(now - g_aimLast); g_aimLast = now;
    if (dtms < 1.f) dtms = 1.f; if (dtms > 50.f) dtms = 50.f;
    if (!g_shm || !g_shm->ctl.aim || !g_shm->ctl.active || g_sw <= 0 || g_sh <= 0) { if (!g_cal.ok) g_cal.st = 0; aimReset(); return; }
    const Ctl& c = g_shm->ctl;
    float cx = (float)g_sw * 0.5f, cy = (float)g_sh * 0.5f;
    float maxD = c.maxDist > 1.f ? c.maxDist : 600.f;
    int mode = g_cal.ok ? c.aimBone : 2;                      // calibrate on the chest: central, always there
    float best = c.fov; int bi = -1, bj = -1;
    bool haveCur = false; float curD = 0.f; int curJ = -1;
    for (int i = 0; i < off::MAX_ENT; i++) {
        Slot& s = g_slots[i];
        if (!s.obj || !s.e.alive || now - s.ts > 150) continue;
        if (s.e.team && !c.aimTeam) continue;
        if (s.e.dist > maxD) continue;
        float px, py;
        int j = pickBone(s, mode, cx, cy, i == g_tSlot ? g_tBone : -1, px, py);
        if (j < 0) { px = s.e.fx + (s.e.hx - s.e.fx) * c.aimH; py = s.e.fy + (s.e.hy - s.e.fy) * c.aimH; }
        float dx = px - cx, dy = py - cy, d = sqrtf(dx * dx + dy * dy);
        if (i == g_tSlot && s.obj == g_tObj) { haveCur = true; curD = d; curJ = j; }
        if (d < best) { best = d; bi = i; bj = j; }
    }
    if (haveCur && curD < c.fov * 1.2f && bi != g_tSlot && best > curD * 0.7f) { bi = g_tSlot; bj = curJ; best = curD; }   // keep current target unless the new one is >30% closer
    if (bi < 0) { aimDbg("no-target(fov/dist/team)", 0, 0, 0, 0, now); if (!g_cal.ok) g_cal.st = 0; aimReset(); return; }
    if (bi != g_tSlot || g_slots[bi].obj != g_tObj) { g_stp0 = g_stp1 = 0.f; g_holdLock = false; }   // new target: restart the ramp
    g_tSlot = bi; g_tObj = g_slots[bi].obj; g_tBone = bj;

    // fresh error against the camera as it is right now
    CamCache* cc = getCam(now); if (!cc) return;
    Slot& ts = g_slots[bi];
    V3 wp = (bj >= 0 && (ts.jwMask & (1u << bj))) ? ts.jw[bj] : V3{ts.wfeet.x, ts.wfeet.y + off::HEAD_H * c.aimH, ts.wfeet.z};
    if (c.lead) {                                              // aim where the target will be when the bullet arrives (+ drop)
        float spd = c.leadSpeed > 1.f ? c.leadSpeed : g_bulletSpeed;
        if (spd > 1.f) {
            V3 T0 = wp, aim = wp; float tof = 0.f;
            for (int pass = 0; pass < 2; pass++) {                 // pass 2 re-solves with the target moved by vel * flight time
                V3 Tp{T0.x + ts.vel.x * tof, T0.y, T0.z + ts.vel.z * tof};
                if (g_balOk && fBalStep) { if (!ballisticSolve(cc->pos, Tp, spd, g_bal, aim, tof)) { aim = Tp; break; } }
                else {                                             // no ballistic profile: straight line + plain gravity drop
                    float dx = Tp.x - cc->pos.x, dy = Tp.y - cc->pos.y, dz = Tp.z - cc->pos.z;
                    tof = sqrtf(dx * dx + dy * dy + dz * dz) / spd; if (tof > 2.f) tof = 2.f;
                    aim = Tp; if (g_gravity > 0.f) aim.y += 0.5f * g_gravity * tof * tof;
                }
            }
            wp = aim;
        }
    }
    V3 sp = fW2S(cc->cam, wp, nullptr);
    if (sp.z <= 0.f) return;
    float bdx = sp.x - cx, bdy = sp.y - cy, dist = sqrtf(bdx * bdx + bdy * bdy);

    void* ml = *(void**)((uintptr_t)self + off2::pm_look);
    if (!ml) { aimDbg("mouseLook null (pm_look)", 0, 0, 0, 0, now); return; }
    float* a = (float*)((uintptr_t)ml + off2::ml_ang);
    if (!(__builtin_isfinite(a[0]) && __builtin_isfinite(a[1]) && fabsf(a[0]) < 1.0e4f && fabsf(a[1]) < 1.0e4f)) {
        aimDbg("BAIL JZC not sane: fix off2::ml_ang", a[0], a[1], bdx, bdy, now); return;       // wrong offset, don't write
    }
    Cal& k = g_cal;
    if (!k.ok) {                                              // ── calibration: yaw/pitch axis + sign + px-per-unit
        if (now < g_calRetry) return;
        if (k.st == 0) { k.ex = bdx; k.ey = bdy; k.tid = bi; k.t = now; a[0] += PROBE; k.st = 1; return; }
        if (now - k.t < 140) return;
        if (bi != k.tid) { k.st = 0; return; }                // target changed mid-probe
        if (k.st == 1) {
            k.c0x = (bdx - k.ex) / PROBE; k.c0y = (bdy - k.ey) / PROBE;
            k.ex = bdx; k.ey = bdy; k.t = now; a[1] += PROBE; k.st = 2; return;
        }
        float c1x = (bdx - k.ex) / PROBE, c1y = (bdy - k.ey) / PROBE;
        float det = k.c0x * c1y - c1x * k.c0y;
        float n0 = sqrtf(k.c0x * k.c0x + k.c0y * k.c0y), n1 = sqrtf(c1x * c1x + c1y * c1y);
        k.st = 0;
        if (n0 < 0.5f || n1 < 0.5f || fabsf(det) < 0.25f * n0 * n1) {
            LOG("aim CALIB FAIL (JZC write has no/degenerate screen effect, or target moved): c0=(%.2f %.2f) c1=(%.2f %.2f) px/unit -> retry in 3s", k.c0x, k.c0y, c1x, c1y);
            g_calRetry = now + 3000; return;
        }
        k.J00 = k.c0x; k.J10 = k.c0y; k.J01 = c1x; k.J11 = c1y; k.det = det; k.ok = true; g_grow = 0;
        LOG("aim CALIB ok J=[%.2f %.2f; %.2f %.2f] px/unit  (cols: JZC.x, JZC.y)", k.J00, k.J01, k.J10, k.J11);
        return;
    }
    if (dist < 2.0f) g_holdLock = true; else if (dist > 5.0f) g_holdLock = false;   // dead zone with hysteresis
    if (g_holdLock) { g_stp0 *= 0.5f; g_stp1 *= 0.5f; return; }
    if (bi == g_lastTid && dist > g_lastBest + 0.5f) {        // error keeps growing on the same target -> jacobian is stale/wrong, redo
        if (++g_grow > 60) { k.ok = false; k.st = 0; g_grow = 0; aimDbg("recalibrating (error kept growing)", a[0], a[1], bdx, bdy, now); return; }
    } else if (dist < g_lastBest) g_grow = 0;
    g_lastBest = dist; g_lastTid = bi;

    float tau  = (c.smooth < 1.f ? 1.f : c.smooth) * 14.f;    // ms, smooth 6 -> 84 ms time constant
    float frac = 1.f - expf(-dtms / tau); if (frac > 0.55f) frac = 0.55f;
    float da0 = -( k.J11 * bdx - k.J01 * bdy) / k.det * frac;   // solve J * da = -err, take `frac` of it this frame
    float da1 = -(-k.J10 * bdx + k.J00 * bdy) / k.det * frac;
    g_stp0 += (da0 - g_stp0) * 0.55f;                          // output low-pass: ramps in and out
    g_stp1 += (da1 - g_stp1) * 0.55f;
    if (g_stp0 >  off::AIM_STEP) g_stp0 =  off::AIM_STEP; if (g_stp0 < -off::AIM_STEP) g_stp0 = -off::AIM_STEP;
    if (g_stp1 >  off::AIM_STEP) g_stp1 =  off::AIM_STEP; if (g_stp1 < -off::AIM_STEP) g_stp1 = -off::AIM_STEP;
    a[0] += g_stp0; a[1] += g_stp1;
    aimDbg("engaged", a[0], a[1], bdx, bdy, now);             // err must shrink over successive lines
}

// runs on the Unity main thread, once per PlayerManager per frame
static void hk_update(void* self, void* mi) {
    if (self && g_shm && g_shm->ctl.noSpread && fIsLocal(self, nullptr)) noSpreadStep(self);   // before the game reads/fires
    orig_update(self, mi);
    if (!self) return;
    uint64_t now = nowMs();

    if (fIsLocal(self, nullptr)) {
        g_localTeam = readName(*(void**)((uintptr_t)self + off2::pm_team));
        g_sw = fScrW(nullptr); g_sh = fScrH(nullptr);
        aimStep(self, now);
        noSpreadStep(self);
    } else do {
        if (!g_shm || (!g_shm->ctl.esp && !g_shm->ctl.aim)) goto published;     // nothing enabled: zero work
        void* ev = *(void**)((uintptr_t)self + off2::pm_events);                 if (!ev) break;
        void* hk = *(void**)((uintptr_t)ev + off2::ev_health);                   if (!hk) break;
        float hp = *(float*)((uintptr_t)hk + off2::hk_value);
        if (!(hp > 0.f) || hp > 1.0e6f) break;
        float hpMax = 100.f;
        void* vit = *(void**)((uintptr_t)self + off2::pm_vitals);
        if (vit) { float v = *(float*)((uintptr_t)vit + off2::gv_maxhp); if (v > 1.f && v < 100000.f) hpMax = v; }

        CamCache* cc = getCam(now);                                              if (!cc) break;
        void* tr = fGetTransform(self, nullptr);                                 if (!tr) break;
        V3 p = fGetPosition(tr, nullptr);
        float dx = p.x - cc->pos.x, dy = p.y - cc->pos.y, dz = p.z - cc->pos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        float maxD = g_shm->ctl.maxDist > 1.f ? g_shm->ctl.maxDist : 600.f;
        if (dist > maxD) break;                                                  // cull before the two w2s calls
        V3 sf = fW2S(cc->cam, p, nullptr);                                       // unity: px, origin bottom-left, z<=0 behind camera
        if (sf.z <= 0.f) break;
        V3 sk = fW2S(cc->cam, V3{p.x, p.y + off::HEAD_H, p.z}, nullptr);
        if (sk.z <= 0.f) break;

        Slot* s = slotFor(self, now);
        Ent& e = s->e;
        e.fx = sf.x; e.fy = sf.y; e.hx = sk.x; e.hy = sk.y;
        s->wfeet = p;
        {                                                                        // horizontal velocity, sampled >=30 ms apart, low-passed
            float dt = (float)(now - s->pt) * 0.001f;
            if (s->pt && dt >= 0.03f && dt < 0.3f) {
                s->vel.x += ((p.x - s->pp.x) / dt - s->vel.x) * 0.35f;
                s->vel.z += ((p.z - s->pp.z) / dt - s->vel.z) * 0.35f;
                s->pp = p; s->pt = now;
            } else if (!s->pt || dt >= 0.3f) { s->vel = V3{0.f, 0.f, 0.f}; s->pp = p; s->pt = now; }
        }
        e.jmask = 0; s->jwMask = 0;
        if (g_shm->ctl.skel || g_shm->ctl.aim) collectBones(self, s, cc, now);
        e.hp = hp; e.hpMax = hpMax; e.dist = dist;
        if (now - s->teamTs > 1000) {                                            // team string compare once a second, not per frame
            Name t = readName(*(void**)((uintptr_t)self + off2::pm_team));
            e.team = sameName(t, g_localTeam) ? 1 : 0; s->teamTs = now;
        }
        if (now - s->nameTs > 1000) {                                            // display name, once a second
            s->nameTs = now;
            toUtf8(readName(*(void**)((uintptr_t)ev + off2::ev_name)), e.name, (int)sizeof(e.name));
        }
        e.alive = true;
        s->ts = now;
        goto published;
    } while (0);
    dropSlot(self);
published:
    if (now - g_lastPub >= 8 && g_sw > 0 && g_sh > 0) {
        g_lastPub = now;
        Ent tmp[off::MAX_ENT]; int cnt = 0;
        for (int i = 0; i < off::MAX_ENT; i++)
            if (g_slots[i].obj && now - g_slots[i].ts < 250) tmp[cnt++] = g_slots[i].e;
        publish(tmp, cnt, g_sw, g_sh);
    }
}

static uintptr_t findBase(const char* lib) {
    FILE* f = fopen("/proc/self/maps", "r"); if (!f) return 0;
    char line[512]; uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi, offs;
        if (sscanf(line, "%lx-%lx %*s %lx", &lo, &hi, &offs) == 3 && offs == 0 && strstr(line, lib)) { base = lo; break; }
    }
    fclose(f); return base;
}

static void* init_thread(void*) {
    for (int i = 0; i < 240 && !(g_base = findBase("libil2cpp.so")); i++) sleep(1);
    if (!g_base) { LOG("libil2cpp not found"); return nullptr; }
    sleep(6);                                                // let the game finish its own startup first
    if (!shmOpen()) return nullptr;
    g_shm->ctl.ready = 1;
    LOG("il2cpp base %#lx", (unsigned long)g_base);

    fIsLocal = (BoolFn)(g_base + rva::IsLocal);
    fScrW = (IntFn)(g_base + rva::ScrW); fScrH = (IntFn)(g_base + rva::ScrH);
    fCamMain = (ObjFn0)(g_base + rva::CamMain);
    fGetTransform = (ObjFn1)(g_base + rva::GetTransform);
    fGetPosition = (PosFn)(g_base + rva::GetPosition);
    fW2S = (W2SFn)(g_base + rva::W2S);
    fGetBone = (BoneFn)(g_base + rva::GetBone);
    fBalStep = (BalStepFn)(g_base + rva::BalStep);
    fGetGO = (ObjFn1)(g_base + rva::GetGO); fGoActive = (BoolFn)(g_base + rva::GoActive);

    void* tramp = hookArm64(g_base + rva::Update, (void*)hk_update, rva::UpdatePrologue);
    if (!tramp) return nullptr;
    orig_update = (void (*)(void*, void*))tramp;
    LOG("collector up (hooked PlayerManager.Update)");
    return nullptr;
}

__attribute__((constructor)) static void ctor() {
    pthread_t t;
    pthread_create(&t, nullptr, init_thread, nullptr);
    pthread_detach(t);
}

#else
// ═══════════════════════════════════════════════════════════════════════════
//  HOST — root process: injector + overlay
// ═══════════════════════════════════════════════════════════════════════════
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <android/log.h>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "Android_touch/TouchHelperA.h"
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#ifdef NOLOG
#define LOG(...) ((void)0)
#else
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "@fyxod_cpp", __VA_ARGS__)
#endif

#ifdef NOLOG
#define printf(...) ((void)0)
#define perror(...) ((void)0)
#endif

#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif

// ── injector (ptrace + remote dlopen) ─────────────────────────────────────
struct Regs { uint64_t x[31]; uint64_t sp, pc, pstate; };

static bool getRegs(pid_t t, Regs& r) { iovec io{&r, sizeof(r)}; return ptrace(PTRACE_GETREGSET, t, (void*)NT_PRSTATUS, &io) == 0; }
static bool setRegs(pid_t t, Regs& r) { iovec io{&r, sizeof(r)}; return ptrace(PTRACE_SETREGSET, t, (void*)NT_PRSTATUS, &io) == 0; }
static int  getSys (pid_t t)          { int n = -1; iovec io{&n, sizeof(n)}; ptrace(PTRACE_GETREGSET, t, (void*)NT_ARM_SYSTEM_CALL, &io); return n; }
static void setSys (pid_t t, int n)   { iovec io{&n, sizeof(n)}; ptrace(PTRACE_SETREGSET, t, (void*)NT_ARM_SYSTEM_CALL, &io); }

static pid_t findPid(const char* pkg) {
    DIR* d = opendir("/proc"); if (!d) return 0;
    pid_t found = 0;
    while (dirent* e = readdir(d)) {
        pid_t p = (pid_t)atoi(e->d_name); if (p <= 0) continue;
        char path[64], buf[256] = {};
        snprintf(path, sizeof(path), "/proc/%d/cmdline", p);
        int fd = open(path, O_RDONLY); if (fd < 0) continue;
        read(fd, buf, sizeof(buf) - 1); close(fd);
        if (!strcmp(buf, pkg)) { found = p; break; }
    }
    closedir(d); return found;
}

static uintptr_t modBase(pid_t pid, const std::string& full) {
    char p[64]; snprintf(p, sizeof(p), "/proc/%d/maps", pid);
    FILE* f = fopen(p, "r"); if (!f) return 0;
    std::string base = full.substr(full.rfind('/') == std::string::npos ? 0 : full.rfind('/') + 1);
    char line[1024]; uintptr_t exact = 0, byName = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi, off2; char perm[8];
        if (sscanf(line, "%lx-%lx %7s %lx", &lo, &hi, perm, &off2) != 4 || off2) continue;
        char* path = strchr(line, '/'); if (!path) continue;
        path[strcspn(path, "\n")] = 0;
        if (full == path) { exact = lo; break; }
        const char* b = strrchr(path, '/');
        if (!byName && base == (b ? b + 1 : path)) byName = lo;
    }
    fclose(f); return exact ? exact : byName;
}

static uintptr_t remoteSym(pid_t pid, void* local) {
    Dl_info di{}; if (!dladdr(local, &di) || !di.dli_fname) return 0;
    uintptr_t rb = modBase(pid, di.dli_fname); if (!rb) return 0;
    return rb + ((uintptr_t)local - (uintptr_t)di.dli_fbase);
}

static bool memWrite(pid_t pid, uintptr_t addr, const void* buf, size_t n) {
    char p[64]; snprintf(p, sizeof(p), "/proc/%d/mem", pid);
    int fd = open(p, O_RDWR); if (fd < 0) return false;
    bool ok = pwrite(fd, buf, n, (off_t)addr) == (ssize_t)n; close(fd); return ok;
}

static bool remoteCall(pid_t t, uintptr_t fn, std::initializer_list<uint64_t> args, uint64_t& ret, const Regs& saved) {
    Regs r = saved; int i = 0;
    for (uint64_t v : args) r.x[i++] = v;
    r.x[30] = 0; r.pc = fn; r.sp = (saved.sp - 0x1000) & ~0xFull;   // lr=0 -> SIGSEGV at pc 0 marks return
    setSys(t, -1);
    if (!setRegs(t, r)) return false;
    if (ptrace(PTRACE_CONT, t, 0, 0)) return false;
    for (int k = 0; k < 64; k++) {
        int st; if (waitpid(t, &st, __WALL) < 0 || !WIFSTOPPED(st)) return false;
        int sig = WSTOPSIG(st);
        if (sig == SIGSEGV) break;
        ptrace(PTRACE_CONT, t, 0, (sig == SIGTRAP || sig == SIGSTOP) ? 0 : sig);
    }
    Regs o; if (!getRegs(t, o) || o.pc != 0) return false;
    ret = o.x[0]; return true;
}

static bool copyFile(const char* src, const char* dst) {
    if (!strcmp(src, dst)) return true;   // never O_TRUNC a file onto itself
    int fi = open(src, O_RDONLY); if (fi < 0) return false;
    int fo = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755); if (fo < 0) { close(fi); return false; }
    char buf[65536]; ssize_t n;
    while ((n = read(fi, buf, sizeof(buf))) > 0) if (write(fo, buf, (size_t)n) != n) { close(fi); close(fo); return false; }
    close(fi); close(fo); return true;
}

#ifdef EMBED_PAYLOAD
#include "payload_blob.h"      // PL_KEY / PL_LEN / PL_BLOB: libcheat.so, xorshift64-keystream encrypted at build time
static bool dropEmbedded(const char* dst) {
    int fo = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755); if (fo < 0) return false;
    uint64_t x = PL_KEY; uint8_t buf[4096]; size_t i = 0;
    while (i < PL_LEN) {
        size_t n = PL_LEN - i < sizeof(buf) ? PL_LEN - i : sizeof(buf);
        for (size_t j = 0; j < n; j++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; buf[j] = PL_BLOB[i + j] ^ (uint8_t)(x >> 24); }
        if (write(fo, buf, n) != (ssize_t)n) { close(fo); return false; }
        i += n;
    }
    close(fo); return true;
}
#endif

static bool waitForIl2cpp(pid_t pid, int maxSec) {
    for (int i = 0; i < maxSec; i++) {
        char p[64]; snprintf(p, sizeof(p), "/proc/%d/maps", pid);
        FILE* f = fopen(p, "r");
        if (f) {
            char line[512]; bool found = false;
            while (fgets(line, sizeof(line), f)) if (strstr(line, "libil2cpp.so")) { found = true; break; }
            fclose(f);
            if (found) return true;
        }
        sleep(1);
    }
    return false;
}

static bool injectLib(const char* lib) {
    printf("[*] lib: %s\n[*] pkg: %s\n", lib ? lib : "(embedded)", off::PKG);
    pid_t pid = 0;
    for (int i = 0; i < 60 && !pid; i++) {
        pid = findPid(off::PKG);
        if (!pid) { printf("[*] waiting for game...\n"); sleep(2); }
    }
    if (!pid) { EPRINT("[-] game not running\n"); return false; }
    printf("[+] pid: %d\n[*] waiting for il2cpp...\n", pid);
    if (!waitForIl2cpp(pid, 120)) { EPRINT("[-] libil2cpp not loaded\n"); return false; }
    sleep(3);

    {   // already injected? (host restarted while game kept running)
        char p[64]; snprintf(p, sizeof(p), "/proc/%d/maps", pid);
        FILE* f = fopen(p, "r");
        if (f) {
            char line[512]; bool has = false;
            while (fgets(line, sizeof(line), f)) if (strstr(line, "/libcheat.so")) { has = true; break; }
            fclose(f);
            if (has) { printf("[+] libcheat.so already in game, skipping inject\n"); return true; }
        }
    }

    printf("[*] copy -> %s\n", off::LIB_DST);
    bool placed;
    #ifdef EMBED_PAYLOAD
    placed = lib ? copyFile(lib, off::LIB_DST) : dropEmbedded(off::LIB_DST);
    #else
    placed = copyFile(lib, off::LIB_DST);
    #endif
    if (!placed) { perror("copy"); return false; }
    struct stat gst{}; stat((std::string("/data/data/") + off::PKG).c_str(), &gst);
    chown(off::LIB_DST, gst.st_uid, gst.st_gid);
    chmod(off::LIB_DST, 0755);
    char cmd[512];   // selinux: give the lib the same label as the app's cache dir
    snprintf(cmd, sizeof(cmd), "chcon \"$(stat -c %%C /data/data/%s/cache)\" %s 2>/dev/null", off::PKG, off::LIB_DST);
    system(cmd);

    if (ptrace(PTRACE_ATTACH, pid, 0, 0)) { perror("attach"); return false; }
    int st; waitpid(pid, &st, __WALL);
    Regs saved; getRegs(pid, saved); int savedSys = getSys(pid);
    bool ok = false;
    uintptr_t rdlopen = remoteSym(pid, (void*)dlopen);
    if (!rdlopen) EPRINT("[-] remote dlopen not found\n");
    else {
        uintptr_t str = (saved.sp - 0x800) & ~0xFull;
        uint64_t h = 0;
        if (!memWrite(pid, str, off::LIB_DST, strlen(off::LIB_DST) + 1)) perror("write path");
        else if (!remoteCall(pid, rdlopen, {str, (uint64_t)RTLD_NOW}, h, saved)) EPRINT("[-] remote call failed\n");
        else if (!h) EPRINT("[-] dlopen returned NULL (check: logcat -s @fyxod_cpp linker)\n");
        else { printf("[+] injected\n"); ok = true; }
    }
    setSys(pid, savedSys); setRegs(pid, saved);
    ptrace(PTRACE_DETACH, pid, 0, 0);
    if (ok) unlink(off::LIB_DST);          // mapped pages stay valid; no plain .so left on disk
    return ok;
}

// ── shared memory reader ──────────────────────────────────────────────────
static Shm* g_shm = nullptr;
static Ent  g_ents[off::MAX_ENT];
static int  g_cnt = 0, g_sw = 0, g_sh = 0;

static uint64_t nowMs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
static void shmTryOpen() {
    int fd = open(off::SHM_PATH, O_RDWR); if (fd < 0) return;
    struct stat st{};
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(Shm)) { close(fd); return; }   // payload hasn't sized it yet (SIGBUS guard)
    void* p = mmap(nullptr, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p != MAP_FAILED) g_shm = (Shm*)p;
}
static void shmPoll() {
    g_cnt = 0;
    if (!g_shm) return;
    for (int t = 0; t < 4; t++) {
        uint32_t a = g_shm->seq; if (a & 1) continue;
        __sync_synchronize();
        int cnt = g_shm->cnt, sw = g_shm->sw, sh = g_shm->sh; uint64_t ts = g_shm->ts_ms;
        if (cnt < 0) cnt = 0;
        if (cnt > off::MAX_ENT) cnt = off::MAX_ENT;
        memcpy(g_ents, g_shm->e, (size_t)cnt * sizeof(Ent));
        __sync_synchronize();
        if (g_shm->seq != a) continue;
        if (nowMs() - ts > 1000) cnt = 0;   // collector dead / game paused
        g_cnt = cnt; g_sw = sw; g_sh = sh;
        return;
    }
}

// ── config ────────────────────────────────────────────────────────────────
struct Cfg {
    bool  esp = true, box = true, hp = true, dist = true, skeleton = false, lines = false, team = false;
    float maxDist = 300.f, headH = off::HEAD_H, lineWidth = 2.f;
    float colEnemy[3] = {1.f, 0.2f, 0.f};
    float colTeam [3] = {0.2f, 1.f, 0.4f};
    float menuOpacity = 0.92f;
    bool  aim = false, aimHold = false, aimTeam = false, aimCircle = true, noSpread = false;   // hold=false: engaged whenever a target is in fov
    float aimFov = 180.f, aimSmooth = 6.f, aimH = 0.85f, vfov = 60.f;        // fov in overlay px
    bool  lead = false, names = true;   // lead: predict motion + drop;  leadSpeed 0 = auto from the equipped gun
    float leadSpeed = 0.f;
    int   aimBone = 4;      // 0 head, 1 neck, 2 chest, 3 pelvis, 4 closest joint to crosshair (aimH = fallback when no skeleton)
} g_cfg;

static void cfgSave() {
    FILE* f = fopen(off::CFG_PATH, "w"); if (!f) return;
    fprintf(f, "{\"esp\":%d,\"box\":%d,\"hp\":%d,\"dist\":%d,\"skeleton\":%d,"
               "\"lines\":%d,\"team\":%d,\"maxDist\":%.0f,\"headH\":%.2f,"
               "\"lw\":%.1f,\"opacity\":%.2f,"
               "\"aim\":%d,\"aimHold\":%d,\"aimTeam\":%d,\"aimCircle\":%d,\"noSpread\":%d,"
               "\"aimFov\":%.0f,\"aimSmooth\":%.1f,\"aimH\":%.2f,\"vfov\":%.0f,\"aimBone\":%d,\"lead\":%d,\"leadSpeed\":%.0f,\"names\":%d}\n",
        g_cfg.esp, g_cfg.box, g_cfg.hp, g_cfg.dist, g_cfg.skeleton,
        g_cfg.lines, g_cfg.team, (double)g_cfg.maxDist, (double)g_cfg.headH,
        (double)g_cfg.lineWidth, (double)g_cfg.menuOpacity,
        g_cfg.aim, g_cfg.aimHold, g_cfg.aimTeam, g_cfg.aimCircle, g_cfg.noSpread,
        (double)g_cfg.aimFov, (double)g_cfg.aimSmooth, (double)g_cfg.aimH, (double)g_cfg.vfov, g_cfg.aimBone,
        g_cfg.lead, (double)g_cfg.leadSpeed, g_cfg.names);
    fclose(f);
}
static void cfgLoad() {
    FILE* f = fopen(off::CFG_PATH, "r"); if (!f) return;
    char buf[768] = {}; fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
    auto jd = [&](const char* k, double& v) -> bool {
        char kk[32]; snprintf(kk, sizeof(kk), "\"%s\":", k);
        const char* p = strstr(buf, kk); if (!p) return false;
        v = strtod(p + strlen(kk), nullptr); return true;
    };
    double v;
    if (jd("esp", v))      g_cfg.esp = v != 0;
    if (jd("box", v))      g_cfg.box = v != 0;
    if (jd("hp", v))       g_cfg.hp = v != 0;
    if (jd("dist", v))     g_cfg.dist = v != 0;
    if (jd("skeleton", v)) g_cfg.skeleton = v != 0;
    if (jd("lines", v))    g_cfg.lines = v != 0;
    if (jd("team", v))     g_cfg.team = v != 0;
    if (jd("maxDist", v))  g_cfg.maxDist = (float)v;
    if (jd("headH", v))    g_cfg.headH = (float)v;
    if (jd("lw", v))       g_cfg.lineWidth = (float)v;
    if (jd("opacity", v))  g_cfg.menuOpacity = (float)v;
    if (jd("aim", v))      g_cfg.aim = v != 0;
    if (jd("aimHold", v))  g_cfg.aimHold = v != 0;
    if (jd("aimTeam", v))  g_cfg.aimTeam = v != 0;
    if (jd("aimCircle", v)) g_cfg.aimCircle = v != 0;
    if (jd("noSpread", v)) g_cfg.noSpread = v != 0;
    if (jd("aimFov", v))   g_cfg.aimFov = (float)v;
    if (jd("aimSmooth", v)) g_cfg.aimSmooth = (float)v;
    if (jd("aimH", v))     g_cfg.aimH = (float)v;
    if (jd("vfov", v))     g_cfg.vfov = (float)v;
    if (jd("aimBone", v))  g_cfg.aimBone = (int)v < 0 ? 0 : ((int)v > 4 ? 4 : (int)v);
    if (jd("lead", v))     g_cfg.lead = v != 0;
    if (jd("leadSpeed", v)) g_cfg.leadSpeed = (float)v;
    if (jd("names", v))    g_cfg.names = v != 0;
}

// ── EGL / ImGui ───────────────────────────────────────────────────────────
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_sur = EGL_NO_SURFACE;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static ANativeWindow* g_win = nullptr;
static int  g_dw = 0, g_dh = 0;        // window (square, long x long) — same trick as enenH/AndroidImgui, covers both rotations
static int  g_scrW = 0, g_scrH = 0;    // visible landscape area: long x short
static bool g_menuOpen = true;
static volatile bool g_run = true;
// writes where it died (module+offset) to stderr -> ends up in @fyxod_cpp.log; debuggerd still gets the re-raised signal
static void crashWhere(uintptr_t a, const char* tag) {
    FILE* f = fopen("/proc/self/maps", "r"); if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi, off;
        if (sscanf(line, "%lx-%lx %*s %lx", &lo, &hi, &off) == 3 && a >= lo && a < hi) {
            const char* name = "[anon]"; char* p = strchr(line, '/');
            if (p) { p[strcspn(p, "\n")] = 0; name = p; }
            EPRINT("[!] %s %#lx = %s+%#lx\n", tag, (unsigned long)a, name, (unsigned long)(a - lo + off));
            break;
        }
    }
    fclose(f);
}
static void crashHandler(int sig, siginfo_t* si, void* ucv) {
    ucontext_t* uc = (ucontext_t*)ucv;
    EPRINT("\n[!] CRASH sig=%d fault_addr=%p\n", sig, si ? si->si_addr : nullptr);
    crashWhere((uintptr_t)uc->uc_mcontext.pc, "pc");
    crashWhere((uintptr_t)uc->uc_mcontext.regs[30], "lr");
    fflush(stderr);                      // SA_RESETHAND: returning re-faults into the default action
}
static void onSig(int) { g_run = false; }

static bool initEGL() {
    printf("[*] stage: GetDisplayInfo\n");
    auto di = anwc::ANativeWindowCreator::GetDisplayInfo();
    printf("[*] stage: display %dx%d rot %d\n", (int)di.width, (int)di.height, (int)di.orientation);
    int W = di.width, H = di.height;
    if (W <= 0 || H <= 0) { EPRINT("[-] GetDisplayInfo failed (libgui symbols for this Android version?)\n"); return false; }
    if (H > W) std::swap(W, H);
    g_scrW = W; g_scrH = H; g_dw = W; g_dh = W;
    printf("[*] stage: create surface %dx%d\n", g_dw, g_dh);
    g_win = anwc::ANativeWindowCreator::Create("@fyxod_cpp", g_dw, g_dh, false);
    printf("[*] stage: surface %p\n", (void*)g_win);
    if (!g_win) { EPRINT("[-] surface create failed\n"); return false; }

    printf("[*] stage: egl\n");
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g_dpy, nullptr, nullptr);
    const EGLint attr[] = {EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 16,
                           EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(g_dpy, attr, &cfg, 1, &n) || !n) { EPRINT("[-] eglChooseConfig\n"); return false; }
    EGLint fmt = 0; eglGetConfigAttrib(g_dpy, cfg, EGL_NATIVE_VISUAL_ID, &fmt);
    ANativeWindow_setBuffersGeometry(g_win, 0, 0, fmt);
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ca);
    g_sur = eglCreateWindowSurface(g_dpy, cfg, g_win, nullptr);
    if (!eglMakeCurrent(g_dpy, g_sur, g_sur, g_ctx)) { EPRINT("[-] eglMakeCurrent\n"); return false; }
    eglSwapInterval(g_dpy, 1);
    printf("[+] overlay %dx%d (screen %dx%d)\n", g_dw, g_dh, g_scrW, g_scrH);
    return true;
}

static void loadFont(float px) {
    ImGuiIO& io = ImGui::GetIO();
    const char* fonts[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf",
                           "/system/fonts/DroidSans.ttf", "/system/fonts/NotoSansCJK-Regular.ttc"};
    for (const char* f : fonts)
    #if IMGUI_VERSION_NUM < 19200
        if (access(f, R_OK) == 0 && io.Fonts->AddFontFromFileTTF(f, px, nullptr, io.Fonts->GetGlyphRangesCyrillic())) return;
    #else
        if (access(f, R_OK) == 0 && io.Fonts->AddFontFromFileTTF(f, px)) return;
    #endif
    ImGui::GetStyle().FontSizeBase = px;   // fallback: scale built-in font (imgui >= 1.92)
}

static const ImVec4 ACC = {0.36f, 0.92f, 0.60f, 1.f};   // accent
static const ImVec4 INK = {0.05f, 0.08f, 0.06f, 1.f};   // text on accent

static bool initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 18; s.ChildRounding = 14; s.FrameRounding = 12; s.GrabRounding = 12;
    s.PopupRounding = 12;  s.ScrollbarRounding = 12; s.TabRounding = 10;
    s.WindowBorderSize = 1; s.ChildBorderSize = 1; s.FrameBorderSize = 0; s.PopupBorderSize = 1;
    s.WindowPadding = {18, 16}; s.FramePadding = {14, 10};
    s.ItemSpacing = {14, 12};   s.ItemInnerSpacing = {10, 8};
    s.ScrollbarSize = 16; s.GrabMinSize = 34;
    auto C = [&](ImGuiCol i, float r, float g, float b, float a = 1.f) { s.Colors[i] = ImVec4(r, g, b, a); };
    C(ImGuiCol_Text,              0.92f, 0.94f, 0.97f);
    C(ImGuiCol_TextDisabled,      0.48f, 0.52f, 0.60f);
    C(ImGuiCol_WindowBg,          0.07f, 0.075f, 0.10f, 0.95f);
    C(ImGuiCol_ChildBg,           0.11f, 0.115f, 0.15f, 0.95f);
    C(ImGuiCol_PopupBg,           0.09f, 0.095f, 0.13f, 0.98f);
    C(ImGuiCol_Border,            0.22f, 0.24f, 0.30f, 0.60f);
    C(ImGuiCol_FrameBg,           0.16f, 0.17f, 0.22f);
    C(ImGuiCol_FrameBgHovered,    0.20f, 0.21f, 0.28f);
    C(ImGuiCol_FrameBgActive,     0.24f, 0.26f, 0.33f);
    C(ImGuiCol_Button,            0.16f, 0.17f, 0.22f);
    C(ImGuiCol_ButtonHovered,     0.22f, 0.24f, 0.31f);
    C(ImGuiCol_ButtonActive,      0.28f, 0.30f, 0.38f);
    C(ImGuiCol_Header,            ACC.x, ACC.y, ACC.z, 0.25f);
    C(ImGuiCol_HeaderHovered,     ACC.x, ACC.y, ACC.z, 0.40f);
    C(ImGuiCol_HeaderActive,      ACC.x, ACC.y, ACC.z, 0.55f);
    C(ImGuiCol_Separator,         0.22f, 0.24f, 0.30f, 0.80f);
    C(ImGuiCol_CheckMark,         ACC.x, ACC.y, ACC.z);
    C(ImGuiCol_SliderGrab,        ACC.x, ACC.y, ACC.z);
    C(ImGuiCol_SliderGrabActive,  0.55f, 1.00f, 0.75f);
    C(ImGuiCol_ScrollbarBg,       0.f, 0.f, 0.f, 0.f);
    C(ImGuiCol_ScrollbarGrab,     0.28f, 0.30f, 0.38f);
    C(ImGuiCol_ScrollbarGrabHovered, 0.34f, 0.36f, 0.45f);
    C(ImGuiCol_ScrollbarGrabActive,  0.40f, 0.43f, 0.52f);
    C(ImGuiCol_ResizeGrip,        ACC.x, ACC.y, ACC.z, 0.20f);
    C(ImGuiCol_ResizeGripHovered, ACC.x, ACC.y, ACC.z, 0.60f);
    C(ImGuiCol_ResizeGripActive,  ACC.x, ACC.y, ACC.z, 0.90f);
    loadFont(26.f);
    ImGui_ImplAndroid_Init(g_win);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    return true;
}

// ── menu ──────────────────────────────────────────────────────────────────
static int g_tab = 0;

static ImU32 u32(ImVec4 c, float a = -1.f) { if (a >= 0.f) c.w = a; return ImGui::ColorConvertFloat4ToU32(c); }

// finger-drag scrolling for the current child (imgui only scrolls by wheel/scrollbar)
static void touchScroll() {
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.f))
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
}

// iOS-style switch, label left / switch right, whole row is the hit area
static bool Toggle(const char* label, bool* v) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetFrameHeight(), w = h * 1.75f;
    float avail = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool hit = ImGui::InvisibleButton("##t", {avail, h});
    if (hit) *v = !*v;
    float t = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), *v ? 1.f : 0.f);
    t += ((*v ? 1.f : 0.f) - t) * std::min(1.f, ImGui::GetIO().DeltaTime * 16.f);
    ImGui::GetStateStorage()->SetFloat(ImGui::GetItemID(), t);
    ImGui::PopID();
    const ImVec4 off(0.20f, 0.21f, 0.27f, 1.f);
    ImVec4 c(off.x + (ACC.x - off.x) * t, off.y + (ACC.y - off.y) * t, off.z + (ACC.z - off.z) * t, 1.f);
    ImVec2 sp(p.x + avail - w, p.y);
    dl->AddText({p.x, p.y + (h - ImGui::GetFontSize()) * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), label);
    dl->AddRectFilled(sp, {sp.x + w, sp.y + h}, u32(c), h * 0.5f);
    dl->AddCircleFilled({sp.x + h * 0.5f + t * (w - h), sp.y + h * 0.5f}, h * 0.5f - 4.f, IM_COL32_WHITE, 24);
    return hit;
}

static void SliderRow(const char* label, const char* id, float* v, float lo, float hi, const char* fmt) {
    ImGui::TextDisabled("%s", label);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat(id, v, lo, hi, fmt);
}

// label left, color swatch right (tap swatch = picker popup)
static void ColorRow(const char* label, float* col) {
    ImGui::PushID(label);
    float h = ImGui::GetFrameHeight(), avail = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + avail - h);
    ImGui::ColorEdit3("##c", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    ImGui::PopID();
}

static void drawHeader() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), W = ImGui::GetContentRegionAvail().x, bh = fs * 1.5f;
    ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##drag", {W, bh});            // header = drag handle
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize(), d = ImGui::GetIO().MouseDelta;
        wp.x = std::max(0.f, std::min((float)g_scrW - ws.x, wp.x + d.x));
        wp.y = std::max(0.f, std::min((float)g_scrH - ws.y, wp.y + d.y));
        ImGui::SetWindowPos(wp);
    }

    float bfs = fs * 1.3f;
    dl->AddText(font, bfs, {p.x, p.y + (bh - bfs) * 0.5f}, u32(ACC), "TONG DLC");
    ImVec2 bsz = font->CalcTextSizeA(bfs, 1e9f, 0.f, "TONG DLC");
    float sfs = fs * 0.75f;
    dl->AddText(font, sfs, {p.x + bsz.x + 12.f, p.y + (bh - sfs) * 0.5f + 2.f}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "esp");

    // status pill, right aligned
    char st[40]; bool live = g_cnt > 0;
    if (live) snprintf(st, sizeof(st), "%d tracked", g_cnt); else snprintf(st, sizeof(st), "waiting for data");
    float pfs = fs * 0.8f, ph = fs * 1.3f;
    ImVec2 tsz = font->CalcTextSizeA(pfs, 1e9f, 0.f, st);
    ImVec4 sc = live ? ACC : ImVec4(1.f, 0.72f, 0.30f, 1.f);
    float x1 = p.x + W, x0 = x1 - tsz.x - ph * 1.3f, y0 = p.y + (bh - ph) * 0.5f;
    dl->AddRectFilled({x0, y0}, {x1, y0 + ph}, u32(sc, 0.16f), ph * 0.5f);
    dl->AddCircleFilled({x0 + ph * 0.5f, y0 + ph * 0.5f}, ph * 0.14f, u32(sc), 12);
    dl->AddText(font, pfs, {x0 + ph * 0.5f + ph * 0.28f, y0 + (ph - pfs) * 0.5f}, u32(sc), st);

    // accent underline, fades to the right
    float ly = p.y + bh + 4.f;
    dl->AddRectFilledMultiColor({p.x, ly}, {p.x + W, ly + 3.f}, u32(ACC, 0.9f), u32(ACC, 0.f), u32(ACC, 0.f), u32(ACC, 0.9f));
    ImGui::Dummy({0, 4.f});
}

static void drawTabs(const char* const* names, int n, int& cur) {
    float sp = ImGui::GetStyle().ItemSpacing.x;
    float w = (ImGui::GetContentRegionAvail().x - sp * (n - 1)) / n;
    for (int i = 0; i < n; i++) {
        if (i) ImGui::SameLine();
        bool a = (i == cur);
        ImGui::PushStyleColor(ImGuiCol_Button,        a ? ImVec4(ACC.x, ACC.y, ACC.z, 0.95f) : ImVec4(0.16f, 0.17f, 0.22f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a ? ImVec4(ACC.x, ACC.y, ACC.z, 1.00f) : ImVec4(0.22f, 0.24f, 0.31f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  a ? ImVec4(ACC.x, ACC.y, ACC.z, 1.00f) : ImVec4(0.28f, 0.30f, 0.38f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          a ? INK : ImGui::GetStyle().Colors[ImGuiCol_Text]);
        if (ImGui::Button(names[i], {w, 0})) cur = i;
        ImGui::PopStyleColor(4);
    }
}

static void drawMenu() {
    ImGuiStyle& st = ImGui::GetStyle();

    // floating launcher (round)
    ImGui::SetNextWindowPos({14, 14});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##tog", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 40.f);
    ImGui::PushStyleColor(ImGuiCol_Button,        g_menuOpen ? ImVec4(ACC.x, ACC.y, ACC.z, 0.95f) : ImVec4(0.10f, 0.11f, 0.15f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_menuOpen ? ACC : ImVec4(0.16f, 0.17f, 0.22f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  g_menuOpen ? ACC : ImVec4(0.20f, 0.22f, 0.28f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Text,          g_menuOpen ? INK : ACC);
    if (ImGui::Button(g_menuOpen ? "X" : "TD", {80, 80})) g_menuOpen = !g_menuOpen;
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (!g_menuOpen) return;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.075f, 0.10f, g_cfg.menuOpacity));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,  ImVec4(0.11f, 0.115f, 0.15f, std::min(1.f, g_cfg.menuOpacity + 0.05f)));
    ImGui::SetNextWindowPos({(float)g_scrW * 0.5f, (float)g_scrH * 0.5f}, ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({820, 560}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({460, 320}, {(float)g_scrW, (float)g_scrH});
    ImGui::Begin("TONG DLC", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav);

    drawHeader();
    static const char* const tabs[] = {"ESP", "Aim", "Style", "Config"};
    drawTabs(tabs, 4, g_tab);
    ImGui::Dummy({0, 2.f});

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float sp = st.ItemSpacing.x;

    if (g_tab == 0) {
        float cw = (avail.x - sp) * 0.5f;
        ImGui::BeginChild("c_tog", {cw, avail.y}, ImGuiChildFlags_Borders, 0);
        ImGui::TextDisabled("TOGGLES");
        Toggle("ESP",         &g_cfg.esp);
        Toggle("Box",         &g_cfg.box);
        Toggle("Health bar",  &g_cfg.hp);
        Toggle("Distance",    &g_cfg.dist);
        Toggle("Names",       &g_cfg.names);
        Toggle("Skeleton",    &g_cfg.skeleton);
        Toggle("Tracers",     &g_cfg.lines);
        Toggle("Teammates",   &g_cfg.team);
        touchScroll();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("c_lim", {cw, avail.y}, ImGuiChildFlags_Borders, 0);
        ImGui::TextDisabled("LIMITS");
        ImGui::Spacing();
        SliderRow("Max distance", "##md", &g_cfg.maxDist, 25.f, 600.f, "%.0f m");
        ImGui::Spacing();
        SliderRow("Box height",   "##hh", &g_cfg.headH, 1.0f, 2.6f, "%.2f m");
        touchScroll();
        ImGui::EndChild();
    } else if (g_tab == 1) {
        float cw = (avail.x - sp) * 0.5f;
        ImGui::BeginChild("c_aim", {cw, avail.y}, ImGuiChildFlags_Borders, 0);
        ImGui::TextDisabled("AIM");
        Toggle("Aim assist",  &g_cfg.aim);
        Toggle("Hold button", &g_cfg.aimHold);
        Toggle("Teammates",   &g_cfg.aimTeam);
        Toggle("FOV circle",  &g_cfg.aimCircle);
        Toggle("No spread",   &g_cfg.noSpread);
        Toggle("Lead target", &g_cfg.lead);
        if (g_shm && !g_shm->ctl.ready)
            ImGui::TextColored({1.f, 0.72f, 0.30f, 1.f}, "payload not ready (inject first)");
        touchScroll();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("c_aimv", {cw, avail.y}, ImGuiChildFlags_Borders, 0);
        ImGui::TextDisabled("TUNING");
        ImGui::Spacing();
        SliderRow("FOV radius",  "##af", &g_cfg.aimFov,    40.f, 600.f, "%.0f px");
        ImGui::Spacing();
        SliderRow("Smoothing",   "##as", &g_cfg.aimSmooth, 1.f, 20.f,   "%.1f");
        ImGui::Spacing();
        SliderRow("Proj. speed (0=auto)", "##ls", &g_cfg.leadSpeed, 0.f, 800.f, "%.0f m/s");
        ImGui::Spacing();
        ImGui::TextDisabled("TARGET BONE");
        {
            static const char* const bn[5] = {"Head", "Neck", "Chest", "Pelvis", "Closest"};
            float bw = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
            for (int i = 0; i < 5; i++) {
                bool on = g_cfg.aimBone == i;
                if (on) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(ACC.x, ACC.y, ACC.z, 0.90f)); ImGui::PushStyleColor(ImGuiCol_Text, INK); }
                if (ImGui::Button(bn[i], {i == 4 ? -1.f : bw, 0})) g_cfg.aimBone = i;
                if (on) ImGui::PopStyleColor(2);
                if (i < 4 && (i & 1) == 0) ImGui::SameLine();
            }
        }
        ImGui::Spacing();
        SliderRow("Aim height (no skeleton)", "##ah", &g_cfg.aimH, 0.3f, 1.0f, "%.2f");
        ImGui::Spacing();
        SliderRow("Camera VFOV", "##av", &g_cfg.vfov,      30.f, 110.f, "%.0f deg");
        touchScroll();
        ImGui::EndChild();
    } else if (g_tab == 2) {
        ImGui::BeginChild("c_sty", {avail.x, avail.y}, ImGuiChildFlags_Borders, 0);
        ImGui::TextDisabled("LOOK");
        ImGui::Spacing();
        SliderRow("Line width",   "##lw", &g_cfg.lineWidth, 1.f, 5.f, "%.1f px");
        ImGui::Spacing();
        SliderRow("Menu opacity", "##op", &g_cfg.menuOpacity, 0.3f, 1.f, "%.2f");
        ImGui::Spacing();
        ImGui::TextDisabled("COLORS");
        ColorRow("Enemy", g_cfg.colEnemy);
        ColorRow("Team",  g_cfg.colTeam);
        touchScroll();
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("c_cfg", {avail.x, avail.y}, ImGuiChildFlags_Borders, 0);
        ImGui::TextDisabled("PROFILE");
        float half = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(ACC.x, ACC.y, ACC.z, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ACC);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ACC);
        ImGui::PushStyleColor(ImGuiCol_Text,          INK);
        if (ImGui::Button("Save", {half, 0})) cfgSave();
        ImGui::PopStyleColor(4);
        ImGui::SameLine();
        if (ImGui::Button("Load", {half, 0})) cfgLoad();
        ImGui::Spacing();
        ImGui::TextDisabled("STATUS");
        if (g_cnt > 0) ImGui::TextColored(ACC, "%d tracked | game %dx%d", g_cnt, g_sw, g_sh);
        else           ImGui::TextColored({1.f, 0.72f, 0.30f, 1.f}, "waiting for game data...");
        ImGui::PushStyleColor(ImGuiCol_Text, st.Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("cfg: %s", off::CFG_PATH);
        ImGui::PopStyleColor();
        touchScroll();
        ImGui::EndChild();
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
}

// ── ESP ───────────────────────────────────────────────────────────────────
static void drawEsp() {
    if (!g_cfg.esp || g_cnt <= 0 || g_sw <= 0 || g_sh <= 0) return;
    // game px (Unity, bottom-left origin) -> overlay px (ImGui, top-left): y flipped, scaled to the visible landscape area
    float kx = (float)g_scrW / (float)g_sw, ky = (float)g_scrH / (float)g_sh;
    float hs = g_cfg.headH / off::HEAD_H;   // box-height slider rescales collector's head point
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    for (int i = 0; i < g_cnt; i++) {
        Ent& e = g_ents[i];
        if (!e.alive || e.dist > g_cfg.maxDist) continue;
        if (e.team && !g_cfg.team) continue;

        float fx = e.fx * kx, fy = ((float)g_sh - e.fy) * ky;
        float hx = e.hx * kx, hy0 = ((float)g_sh - e.hy) * ky;
        float hy = fy - (fy - hy0) * hs;
        float h = fy - hy; if (h < 4.f) continue;
        float w = h * 0.45f, cx = (fx + hx) * 0.5f;
        float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;

        float* c = e.team ? g_cfg.colTeam : g_cfg.colEnemy;
        ImU32 col = IM_COL32((int)(c[0] * 255), (int)(c[1] * 255), (int)(c[2] * 255), 255);
        ImU32 blk = IM_COL32(0, 0, 0, 200);
        float lw = g_cfg.lineWidth;

        if (g_cfg.lines) dl->AddLine({(float)g_scrW * 0.5f, (float)g_scrH}, {fx, fy}, col, 1.5f);
        if (g_cfg.box) {
            dl->AddRect({x0 - 1, hy - 1}, {x1 + 1, fy + 1}, blk, 0, 0, lw + 2);
            dl->AddRect({x0, hy}, {x1, fy}, col, 0, 0, lw);
        }
        if (g_cfg.skeleton) {
            if (e.jmask) {                                       // real joints from Animator.GetBoneTransform
                auto JP = [&](int j, ImVec2& q) -> bool {
                    if (!(e.jmask & (1u << j))) return false;
                    q = {e.jx[j] * kx, ((float)g_sh - e.jy[j]) * ky}; return true;
                };
                static const int B[][2] = {
                    {J_NECK, J_CHEST}, {J_CHEST, J_HIPS},
                    {J_CHEST, J_LSH}, {J_LSH, J_LEL}, {J_LEL, J_LHA},
                    {J_CHEST, J_RSH}, {J_RSH, J_REL}, {J_REL, J_RHA},
                    {J_HIPS, J_LHIP}, {J_LHIP, J_LKN}, {J_LKN, J_LFT},
                    {J_HIPS, J_RHIP}, {J_RHIP, J_RKN}, {J_RKN, J_RFT}};
                ImVec2 pa, pb, jh, jn, jc;
                bool hasH = JP(J_HEAD, jh), hasN = JP(J_NECK, jn), hasC = JP(J_CHEST, jc);
                for (int pass = 0; pass < 2; pass++) {
                    ImU32 lc = pass == 0 ? blk : col; float lt = pass == 0 ? lw + 2.f : lw;
                    for (auto& bb : B) if (JP(bb[0], pa) && JP(bb[1], pb)) dl->AddLine(pa, pb, lc, lt);
                    if (hasH && hasN) dl->AddLine(jn, jh, lc, lt); else if (hasH && hasC) dl->AddLine(jc, jh, lc, lt);
                    if (hasH) {
                        float r = hasN ? sqrtf((jh.x - jn.x) * (jh.x - jn.x) + (jh.y - jn.y) * (jh.y - jn.y)) * 0.8f : h * 0.07f;
                        if (r < 3.f) r = 3.f;
                        dl->AddCircle(jh, r, lc, 16, lt);
                    }
                }
            } else {                                             // fallback: no humanoid animator -> proportional stick figure from the box
                float hipY = hy + h * 0.52f, shY = hy + h * 0.24f, hr = h * 0.07f;
                for (int pass = 0; pass < 2; pass++) {
                    ImU32 lc = pass == 0 ? blk : col; float lt = pass == 0 ? lw + 2.f : lw;
                    dl->AddCircle({cx, hy + hr}, hr, lc, 16, lt);
                    dl->AddLine({cx, hy + hr * 2.f}, {cx, hipY}, lc, lt);
                    dl->AddLine({cx - w * 0.55f, hy + h * 0.45f}, {cx, shY}, lc, lt);
                    dl->AddLine({cx + w * 0.55f, hy + h * 0.45f}, {cx, shY}, lc, lt);
                    dl->AddLine({cx, hipY}, {cx - w * 0.28f, fy}, lc, lt);
                    dl->AddLine({cx, hipY}, {cx + w * 0.28f, fy}, lc, lt);
                }
            }
        }
        if (g_cfg.hp && e.hpMax > 1.f) {
            float f = e.hp / e.hpMax; f = f < 0 ? 0 : (f > 1 ? 1 : f);
            dl->AddRectFilled({x0 - 9, hy - 1}, {x0 - 4, fy + 1}, blk);
            dl->AddRectFilled({x0 - 8, fy - h * f}, {x0 - 5, fy}, IM_COL32((int)(255 * (1 - f)), (int)(255 * f), 0, 255));
        }
        if (g_cfg.names && e.name[0]) {
            float fs = ImGui::GetFontSize() * 0.75f;
            ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0.f, e.name);
            ImVec2 np = {cx - sz.x * 0.5f, hy - sz.y - 4.f};
            dl->AddText(ImGui::GetFont(), fs, {np.x + 1.f, np.y + 1.f}, IM_COL32(0, 0, 0, 220), e.name);
            dl->AddText(ImGui::GetFont(), fs, np, IM_COL32_WHITE, e.name);
        }
        if (g_cfg.dist) {
            char t[24]; snprintf(t, sizeof(t), "%dm", (int)e.dist);
            float fs = ImGui::GetFontSize() * 0.7f;
            ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0.f, t);
            dl->AddText(ImGui::GetFont(), fs, {cx - sz.x * 0.5f, fy + 3}, IM_COL32_WHITE, t);
        }
    }
}

// ── AIM: fov circle, optional hold button, push settings to payload ───────
static void drawAim() {
    int active = 0;
    if (g_cfg.aim) {
        if (g_cfg.aimCircle)
            ImGui::GetBackgroundDrawList()->AddCircle({(float)g_scrW * 0.5f, (float)g_scrH * 0.5f}, g_cfg.aimFov, IM_COL32(255, 255, 255, 110), 64, 1.5f);
        if (g_cfg.aimHold) {   // imgui = single pointer: can't hold this while another finger swipes the camera, hence default off
            ImGui::SetNextWindowPos({40.f, (float)g_scrH * 0.35f});
            ImGui::Begin("##aimbtn", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
            ImGui::Button("AIM", {110, 110});
            active = ImGui::IsItemActive() ? 1 : 0;
            ImGui::End();
        } else active = 1;
    }
    if (!g_shm || g_scrW <= 0) return;
    float k = (float)g_sw / (float)g_scrW;   // overlay px -> game px
    Ctl& c = g_shm->ctl;
    c.fov = g_cfg.aimFov * k; c.smooth = g_cfg.aimSmooth; c.aimH = g_cfg.aimH; c.vfov = g_cfg.vfov;
    c.maxDist = g_cfg.maxDist; c.aimTeam = g_cfg.aimTeam ? 1 : 0; c.noSpread = g_cfg.noSpread ? 1 : 0;
    c.active = active; c.aim = g_cfg.aim ? 1 : 0; c.esp = g_cfg.esp ? 1 : 0;
    c.skel = (g_cfg.skeleton && g_cfg.esp) ? 1 : 0; c.aimBone = g_cfg.aimBone;
    c.lead = g_cfg.lead ? 1 : 0; c.leadSpeed = g_cfg.leadSpeed;
}

// ── license check (HOST side only) ───────────────────────────────────────────
// GET https://license-validator.gosthtoster.workers.dev/check?key=<KEY> over TLS.
// Uses Android's own libssl.so (BoringSSL, classic OpenSSL symbol names) via dlopen,
// so no TLS lib needs to be linked or bundled. Needs libssl.so/libcrypto.so on-device
// (stock on every Android) and internet access for the root shell.
namespace lic {
    constexpr const char* SRV_HOST = "license-validator.gosthtoster.workers.dev";   // not HOST: clashes with NDK linux/input-event-codes.h
    constexpr const char* PATH_FMT = "/check?key=%s";          // FILL: confirm path against the real worker route
    constexpr const char* KEY_PATH = "/data/local/tmp/.@fyxod_cpp_key";                // not KEY_FILE: clashes with the same header (KEY_FILE=144)
}

static int tcpConnect(const char* host, int port) {
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portStr[8]; snprintf(portStr, sizeof(portStr), "%d", port);
    if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

// classic OpenSSL symbol names, resolved by hand: BoringSSL on-device keeps them for NDK/system compat,
// same pattern this project already uses for libgui.so in ANativeWindowCreator.h.
static bool httpsGet(const char* host, const char* path, char* out, size_t outCap) {
    void* libssl = dlopen("libssl.so", RTLD_NOW);
    if (!libssl) { EPRINT("license: libssl.so not found\n"); return false; }
    auto TLS_client_method  = (void* (*)())dlsym(libssl, "TLS_client_method");
    auto SSL_CTX_new        = (void* (*)(void*))dlsym(libssl, "SSL_CTX_new");
    auto SSL_CTX_set_verify = (void  (*)(void*, int, void*))dlsym(libssl, "SSL_CTX_set_verify");
    auto SSL_CTX_free       = (void  (*)(void*))dlsym(libssl, "SSL_CTX_free");
    auto SSL_new            = (void* (*)(void*))dlsym(libssl, "SSL_new");
    auto SSL_set_fd         = (int   (*)(void*, int))dlsym(libssl, "SSL_set_fd");
    auto SSL_ctrl           = (long  (*)(void*, int, long, void*))dlsym(libssl, "SSL_ctrl");
    auto SSL_connect        = (int   (*)(void*))dlsym(libssl, "SSL_connect");
    auto SSL_write          = (int   (*)(void*, const void*, int))dlsym(libssl, "SSL_write");
    auto SSL_read           = (int   (*)(void*, void*, int))dlsym(libssl, "SSL_read");
    auto SSL_shutdown       = (int   (*)(void*))dlsym(libssl, "SSL_shutdown");
    auto SSL_free           = (void  (*)(void*))dlsym(libssl, "SSL_free");
    if (!TLS_client_method || !SSL_CTX_new || !SSL_new || !SSL_set_fd || !SSL_ctrl || !SSL_connect || !SSL_write || !SSL_read) {
        EPRINT("license: libssl symbol resolve failed (vendor BoringSSL layout differs)\n"); return false;
    }

    int fd = tcpConnect(host, 443);
    if (fd < 0) { EPRINT("license: tcp connect failed (no network?)\n"); return false; }

    void* ctx = SSL_CTX_new(TLS_client_method());
    if (SSL_CTX_set_verify) SSL_CTX_set_verify(ctx, 0 /*SSL_VERIFY_NONE*/, nullptr);   // no system CA lookup from a root shell
    void* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_ctrl(ssl, 55 /*SSL_CTRL_SET_TLSEXT_HOSTNAME*/, 0 /*TLSEXT_NAMETYPE_host_name*/, (void*)host);  // SNI, Cloudflare routes on it
    if (SSL_connect(ssl) != 1) {
        EPRINT("license: TLS handshake failed\n");
        SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return false;
    }

    char req[512];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: @fyxod_cpp\r\n\r\n", path, host);
    SSL_write(ssl, req, n);

    size_t total = 0;
    while (total + 1 < outCap) {
        int r = SSL_read(ssl, out + total, (int)(outCap - 1 - total));
        if (r <= 0) break;
        total += (size_t)r;
    }
    out[total] = 0;
    SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
    return total > 0;
}

static bool licenseCheck(const char* key) {
    if (!key || !key[0]) return false;
    char path[256]; snprintf(path, sizeof(path), lic::PATH_FMT, key);
    char resp[4096];
    if (!httpsGet(lic::SRV_HOST, path, resp, sizeof(resp))) { EPRINT("license: request failed\n"); return false; }
    bool httpOk = !strncmp(resp, "HTTP/1.1 2", 10) || !strncmp(resp, "HTTP/1.0 2", 10);
    char* body = strstr(resp, "\r\n\r\n"); body = body ? body + 4 : resp;
    // FILL: body contract assumed as {"valid":true,...} — adjust the needle below to match the real worker response
    bool valid = httpOk && strstr(body, "\"valid\":true") != nullptr;
    if (!valid) EPRINT("license: rejected (%.80s)\n", body);
    return valid;
}
static void loadSavedKey(char* out, size_t cap) {
    out[0] = 0;
    FILE* f = fopen(lic::KEY_PATH, "r"); if (!f) return;
    if (fgets(out, (int)cap, f)) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    }
    fclose(f);
}
static void saveKey(const char* key) {
    FILE* f = fopen(lic::KEY_PATH, "w"); if (!f) return;
    fputs(key, f); fclose(f); chmod(lic::KEY_PATH, 0600);
}

// ── license lock screen — drawn instead of the menu until a valid key is entered ──
static char        g_keyBuf[128] = {};
static bool        g_licensed    = false;
static std::string g_licStatus;

static void drawLockScreen() {
    ImGui::SetNextWindowPos({(float)g_scrW * 0.5f, (float)g_scrH * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({420.f, 0.f}, ImGuiCond_Always);
    ImGui::Begin("License", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::TextWrapped("Enter your license key");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##key", g_keyBuf, sizeof(g_keyBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_Password);
    bool click = ImGui::Button("Unlock", {-1.f, 0.f});
    if (!g_licStatus.empty()) ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", g_licStatus.c_str());
    if ((enter || click) && g_keyBuf[0]) {
        if (licenseCheck(g_keyBuf)) { saveKey(g_keyBuf); g_licensed = true; g_licStatus.clear(); }
        else g_licStatus = "invalid key";
    }
    ImGui::End();
}

// resolves the libcheat.so path the way main() used to (argv override / embedded / next to the binary)
static bool resolveLib(int argc, char** argv, bool overlayOnly, char* lib, size_t cap, bool& emb) {
    emb = false;
    if (overlayOnly) return true;
    if (argc > 1) { snprintf(lib, cap, "%s", argv[1]); return true; }
    #ifdef EMBED_PAYLOAD
    emb = true; return true;
    #else
    char self[512] = {};
    ssize_t r = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (r <= 0) { perror("readlink"); return false; }
    char* sl = strrchr(self, '/');
    if (sl) { *sl = 0; snprintf(lib, cap, "%s/libcheat.so", self); } else snprintf(lib, cap, "./libcheat.so");
    return true;
    #endif
}

// usage: @fyxod_cpp [libcheat.so path]   |   @fyxod_cpp --overlay   (skip injection)
// injection now happens lazily, once the license lock screen accepts a key — the overlay (and the key
// input it draws) has to be up before that can happen at all.
static int runOverlay(int argc, char** argv) {
    if (!initEGL()) return 1;
    printf("[*] stage: imgui\n");
    initImGui();
    printf("[*] stage: touch\n");
    cfgLoad();
    Touch_Init(g_scrW, g_scrH, 0, true);

    loadSavedKey(g_keyBuf, sizeof(g_keyBuf));
    g_licensed = g_keyBuf[0] && licenseCheck(g_keyBuf);
    if (!g_licensed) g_keyBuf[0] = 0;              // don't prefill a key that just failed

    bool overlayOnly = argc > 1 && !strcmp(argv[1], "--overlay");
    bool injected = false;
    printf("[+] overlay up. unlock with your license key, then tap [M] in the top-left corner. Ctrl-C to quit.\n");

    uint64_t lastOpen = 0;
    while (g_run) {
        if (g_licensed && !injected) {
            char lib[512]; bool emb = false;
            if (!resolveLib(argc, argv, overlayOnly, lib, sizeof(lib), emb)) { g_run = false; break; }
            injected = overlayOnly || injectLib(emb ? nullptr : lib);
            if (!injected) { EPRINT("inject failed\n"); g_run = false; break; }
        }

        if (!g_shm && nowMs() - lastOpen > 1000) { lastOpen = nowMs(); shmTryOpen(); }
        shmPoll();
        Touch_UpdateImGui();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();
        if (g_licensed) { drawEsp(); drawAim(); drawMenu(); } else { drawLockScreen(); }
        ImGui::Render();

        glViewport(0, 0, g_dw, g_dh);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(g_dpy, g_sur);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g_dpy, g_ctx);
    eglDestroySurface(g_dpy, g_sur);
    eglTerminate(g_dpy);
    anwc::ANativeWindowCreator::Destroy(g_win);
    Touch_Close();
    return 0;
}

int main(int argc, char** argv) {
    if (geteuid() != 0) { EPRINT("need root\n"); return 1; }
    signal(SIGINT, onSig); signal(SIGTERM, onSig);
    { struct sigaction sa{}; sa.sa_sigaction = crashHandler; sa.sa_flags = SA_SIGINFO | SA_RESETHAND; sigemptyset(&sa.sa_mask);
      for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE}) sigaction(s, &sa, nullptr); }
    setvbuf(stdout, nullptr, _IONBF, 0);
    return runOverlay(argc, argv);
}
#endif // PAYLOAD
