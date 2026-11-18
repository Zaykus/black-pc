#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void Log(const char* fmt, ...) {
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Log, &self);
        GetModuleFileNameA(self, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        strcpy(slash ? slash + 1 : path, "mod_menu.log");
    }
    FILE* f = fopen(path, "a");
    if (!f) return;
    va_list va;
    va_start(va, fmt);
    vfprintf(f, fmt, va);
    va_end(va);
    fprintf(f, "\n");
    fclose(f);
}

// --- Real D3D9 Functions ---
typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT SDKVersion);
typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT SDKVersion, IDirect3D9Ex**);

static PFN_Direct3DCreate9 Real_Direct3DCreate9 = nullptr;
static PFN_Direct3DCreate9Ex Real_Direct3DCreate9Ex = nullptr;

static HMODULE GetRealD3D9() {
    static HMODULE hReal = nullptr;
    if (!hReal) {
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, MAX_PATH);
        strcat_s(sysPath, "\\d3d9.dll");
        hReal = LoadLibraryA(sysPath);
        Log("Loaded system d3d9.dll from %s (handle=%p)", sysPath, hReal);
        if (hReal) {
            Real_Direct3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(hReal, "Direct3DCreate9");
            Real_Direct3DCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(hReal, "Direct3DCreate9Ex");
            Log("Real_Direct3DCreate9=%p", Real_Direct3DCreate9);
        }
    }
    return hReal;
}

// --- VTable Hooking Helper ---
static void* HookVTable(void* pInterface, int index, void* newFunc) {
    void** vtable = *(void***)pInterface;
    DWORD oldProtect;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("VirtualProtect failed on vtable[%d], error=%lu", index, GetLastError());
        return nullptr;
    }
    void* orig = vtable[index];
    vtable[index] = newFunc;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    Log("Hooked vtable[%d]: orig=%p -> new=%p", index, orig, newFunc);
    return orig;
}

// --- Function Pointers ---
typedef HRESULT (WINAPI *PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (WINAPI *PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (WINAPI *PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static PFN_CreateDevice Orig_CreateDevice = nullptr;
static PFN_Present Orig_Present = nullptr;
static PFN_Reset Orig_Reset = nullptr;

// --- State & Cheats ---
static HWND g_hWnd = nullptr;
static WNDPROC g_OrigWndProc = nullptr;
static bool g_bMenuOpen = false;
static bool g_bImGuiInit = false;

// Cheat Toggles (Damage Immunity, NEVER "God Mode")
extern "C" bool g_bDamageImmunity = false;
static bool g_bInfAmmo = false;
static bool g_bInfGrenades = false;
static bool g_bRapidFire = false;
static bool g_bNoRecoil = false;
static bool g_bOneHitKill = false;
static bool g_bWireframe = false;
static float g_fCustomFOV = 70.0f;
static bool g_bCustomFOV = false;
static float g_fGameSpeed = 1.0f;
static bool g_bCustomSpeed = false;

// Memory patching helper
static void PatchMemory(uint32_t addr, const void* bytes, size_t len, void* origSave = nullptr) {
    DWORD oldProtect;
    if (VirtualProtect((void*)(uintptr_t)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (origSave) memcpy(origSave, (void*)(uintptr_t)addr, len);
        memcpy((void*)(uintptr_t)addr, bytes, len);
        VirtualProtect((void*)(uintptr_t)addr, len, oldProtect, &oldProtect);
    }
}

// Original bytes preservation
static uint8_t g_OrigDamageHead[5] = {0x55, 0x8B, 0xEC, 0x83, 0xE4};
static uint8_t g_OrigDamageBranch1[8] = {0xF3, 0x0F, 0x10, 0x88, 0x34, 0x01, 0x00, 0x00};
static uint8_t g_OrigDamageBranch2[8] = {0xF3, 0x0F, 0x10, 0x88, 0x34, 0x01, 0x00, 0x00};

static uint8_t g_OrigAmmoCode[19] = {};
static uint8_t g_OrigGrenCheck[2] = {0x74, 0x15};
static uint8_t g_OrigGrenHud[6]   = {0xA1, 0xF0, 0x45, 0x2D, 0x00, 0x8A};
static uint8_t g_OrigGren1[4]     = {0x66, 0xFF, 0x4E, 0x0E};
static uint8_t g_OrigGren2[4]     = {0x66, 0xFF, 0x4E, 0x0E};
static uint8_t g_OrigGren3[4]     = {0x66, 0x89, 0x4E, 0x0E};
static uint8_t g_OrigGren4[7]     = {0x66, 0x8B, 0x4E, 0x0E, 0x0F, 0xBF, 0xD1};

static uint8_t g_OrigRecoil1[8]   = {0xF3, 0x0F, 0x11, 0x8C, 0x81, 0xE0, 0x0A, 0x00};
static uint8_t g_OrigRecoil2[8]   = {0xF3, 0x0F, 0x11, 0x8C, 0x81, 0xE8, 0x0A, 0x00};
static uint8_t g_OrigRecoil3[8]   = {0xF3, 0x0F, 0x11, 0x84, 0x81, 0xE4, 0x0A, 0x00};

static uint8_t g_OrigRapidFire[6] = {0x89, 0x95, 0xDC, 0x00, 0x00, 0x00};
static uint8_t g_OrigEnemyDmg[8]  = {0xF3, 0x0F, 0x10, 0x8F, 0x34, 0x01, 0x00, 0x00};

// --- Player-Only Infinite Ammo Trampoline Hook ---
static const uint32_t g_AmmoHookReturn = 0x001EFBC8;

__attribute__((naked)) static void Hook_AmmoDecrement() {
    __asm__ volatile (
        "mov 0xf4(%%ebp), %%eax\n\t"
        // Check if cheat is active AND if entity is player (0x109(ebp) != 0)
        "cmpb $0, %0\n\t"
        "je 1f\n\t"
        "cmpb $0, 0x109(%%ebp)\n\t"
        "je 1f\n\t"
        // Player with Infinite Ammo: keep clip ammo topped up and skip reserve decrement!
        "cmpw $10, 0x18(%%eax)\n\t"
        "jge 2f\n\t"
        "movw $30, 0x18(%%eax)\n\t"
        "jmp 2f\n\t"
    "1:\n\t"
        // Normal decrement for AI enemies (or when cheat is off)
        "decw 0x18(%%eax)\n\t"
        "mov 0xf4(%%ebp), %%eax\n\t"
        "decl 0x14(%%eax)\n\t"
    "2:\n\t"
        "jmp *%1\n\t"
        :
        : "m"(g_bInfAmmo), "m"(g_AmmoHookReturn)
    );
}

// --- Player-Only Rapid Fire Trampoline Hook ---
static const uint32_t g_RapidFireReturn = 0x001EFC78;

__attribute__((naked)) static void Hook_RapidFire() {
    __asm__ volatile (
        "cmpb $0, %0\n\t"
        "je 1f\n\t"
        "cmpb $0, 0x109(%%ebp)\n\t"
        "je 1f\n\t"
        // Player with Rapid Fire: zero weapon cooldown!
        "movl $0, 0xdc(%%ebp)\n\t"
        "jmp 2f\n\t"
    "1:\n\t"
        // Normal cooldown write
        "mov %%edx, 0xdc(%%ebp)\n\t"
    "2:\n\t"
        "jmp *%1\n\t"
        :
        : "m"(g_bRapidFire), "m"(g_RapidFireReturn)
    );
}

// --- Weapon Definitions & Armory ---
struct WeaponEntry {
    uint8_t id;
    const char* name;
    const char* category;
    const char* description;
    int defaultMag;
};

static const WeaponEntry kWeapons[] = {
    { 0,  "Glock 19",              "Handguns",         "9mm standard sidearm with suppressor capability",  15 },
    { 1,  "Magnum (Colt Anaconda)","Handguns",         ".44 high-powered heavy revolver",                   6 },
    { 2,  "DC3 Elite (Desert Eagle)","Handguns",       ".50 high-impact magnum pistol",                     7 },
    { 3,  "MP5",                   "Submachine Guns",  "9mm high-accuracy tactical SMG",                   30 },
    { 4,  "P90",                   "Submachine Guns",  "5.7mm high-capacity compact PDW",                  50 },
    { 5,  "MAC-10 Elite",          "Submachine Guns",  ".45 extreme rate-of-fire machine pistol",          30 },
    { 6,  "Uzi",                   "Submachine Guns",  "9mm compact automatic submachine gun",             32 },
    { 7,  "AK-47",                 "Assault Rifles",   "7.62mm heavy assault rifle with brutal stopping power", 30 },
    { 8,  "M16A2",                 "Assault Rifles",   "5.56mm military service rifle with 3-round burst & auto", 30 },
    { 9,  "M203 (Underbarrel)",    "Heavy Weapons",    "40mm high-explosive grenade launcher attachment",   1 },
    { 10, "G36C",                  "Assault Rifles",   "5.56mm modern compact tactical assault carbine",   30 },
    { 11, "Remington 870",         "Shotguns",         "12-gauge close-quarters pump-action shotgun",       8 },
    { 12, "SPAS-12",               "Shotguns",         "12-gauge combat assault shotgun",                   8 },
    { 13, "Walther WA2000",        "Sniper Rifles",    ".300 Win Mag semi-automatic bullpup sniper rifle",   6 },
    { 14, "RPG-7",                 "Heavy Weapons",    "High-explosive rocket-propelled anti-armor grenade",1 },
    { 15, "M249 SAW (BFG)",        "Heavy Weapons",    "5.56mm heavy squad automatic machine gun",        100 },
};
static const int kNumWeapons = sizeof(kWeapons) / sizeof(kWeapons[0]);
static int g_SelectedWeaponIdx = 7; // Default selected: AK-47

// Equip weapon into active slot
static bool EquipWeapon(uint8_t weaponId) {
    uint32_t pGame = *(uint32_t*)0x002D45F0;
    if (!pGame || pGame < 0x10000 || IsBadReadPtr((void*)(uintptr_t)pGame, 0x20300))
        return false;

    // Determine active slot (0 = primary, 1 = secondary)
    uint8_t activeSlot = *(uint8_t*)(uintptr_t)(pGame + 0x2020E);
    if (activeSlot > 1) activeSlot = 0;

    // Write weapon ID to active slot
    if (activeSlot == 0) {
        *(uint8_t*)(uintptr_t)(pGame + 0x2020C) = weaponId;
    } else {
        *(uint8_t*)(uintptr_t)(pGame + 0x2020E) = weaponId;
    }

    *(uint8_t*)(uintptr_t)(pGame + 0x2020D) = 1; // Trigger weapon ready/switch

    // Refill clip and reserve for active weapon
    uint32_t pWeaponMgr = *(uint32_t*)(uintptr_t)(pGame + 0x2106C);
    if (pWeaponMgr > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pWeaponMgr, 0x10)) {
        uint8_t count = *(uint8_t*)(uintptr_t)pWeaponMgr;
        uint32_t pArray = *(uint32_t*)(uintptr_t)(pWeaponMgr + 4);
        if (pArray > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pArray, count * 0x24)) {
            for (uint8_t i = 0; i < count; i++) {
                uint32_t entry = pArray + i * 0x24;
                if (*(uint8_t*)(uintptr_t)(entry + 0x10) == weaponId) {
                    *(int16_t*)(uintptr_t)(entry + 0x18) = (int16_t)kWeapons[g_SelectedWeaponIdx].defaultMag;
                    *(int32_t*)(uintptr_t)(entry + 0x14) = 999;
                    break;
                }
            }
        }
    }

    Log("EquipWeapon: slot %u set to weapon ID %u (%s)", activeSlot, weaponId, kWeapons[g_SelectedWeaponIdx].name);
    return true;
}

// Refill active weapon ammo & reserve
static bool RefillActiveAmmo() {
    uint32_t pGame = *(uint32_t*)0x002D45F0;
    if (!pGame || pGame < 0x10000 || IsBadReadPtr((void*)(uintptr_t)pGame, 0x20300))
        return false;

    uint32_t pWeaponMgr = *(uint32_t*)(uintptr_t)(pGame + 0x2106C);
    if (pWeaponMgr > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pWeaponMgr, 0x10)) {
        uint8_t count = *(uint8_t*)(uintptr_t)pWeaponMgr;
        uint32_t pArray = *(uint32_t*)(uintptr_t)(pWeaponMgr + 4);
        if (pArray > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pArray, count * 0x24)) {
            for (uint8_t i = 0; i < count; i++) {
                uint32_t entry = pArray + i * 0x24;
                *(int16_t*)(uintptr_t)(entry + 0x18) = 100;
                *(int32_t*)(uintptr_t)(entry + 0x14) = 999;
            }
        }
    }
    return true;
}

// Restore / Full Heal Player
static bool FullHealPlayer() {
    uint32_t pGame = *(uint32_t*)0x002D45F0;
    if (!pGame || pGame < 0x10000 || IsBadReadPtr((void*)(uintptr_t)pGame, 0x20170))
        return false;

    *(uint32_t*)(uintptr_t)(pGame + 0x2014C) = 3;    // 3 health packs
    *(uint32_t*)(uintptr_t)(pGame + 0x20150) = 4;    // 4 health segments
    *(uint32_t*)(uintptr_t)(pGame + 0x20154) = 4;    // 4 max segments
    *(float*)(uintptr_t)(pGame + 0x20158) = 1.0f;    // 100% current
    *(float*)(uintptr_t)(pGame + 0x2015C) = 1.0f;    // 100% display
    *(float*)(uintptr_t)(pGame + 0x20160) = 1.0f;    // 100% overall

    *(uint32_t*)0x002F4178 = 4;
    *(uint32_t*)0x002F417C = 100;
    *(uint32_t*)0x002F416C = 100;
    *(uint32_t*)0x002F4170 = 100;
    return true;
}

// --- Apply Cheats Engine Routine ---
static void ApplyCheats() {
    // -------------------------------------------------------------
    // 1. Player-Only Infinite Ammo
    // -------------------------------------------------------------
    static bool s_lastInfAmmo = false;
    static bool s_ammoHookInstalled = false;
    if (!s_ammoHookInstalled) {
        s_ammoHookInstalled = true;
        // Save original 19 bytes from 0x001EFBB5
        memcpy(g_OrigAmmoCode, (void*)0x001EFBB5, 19);
        // Build 5-byte jmp to Hook_AmmoDecrement + 14 NOPs
        uint8_t patchBytes[19];
        memset(patchBytes, 0x90, 19);
        patchBytes[0] = 0xE9;
        int32_t rel = (int32_t)((uintptr_t)Hook_AmmoDecrement - (0x001EFBB5 + 5));
        memcpy(patchBytes + 1, &rel, 4);
        PatchMemory(0x001EFBB5, patchBytes, 19);
        Log("Player-Only Infinite Ammo trampoline installed at 0x001EFBB5 -> %p", Hook_AmmoDecrement);
    }
    if (g_bInfAmmo != s_lastInfAmmo) {
        s_lastInfAmmo = g_bInfAmmo;
        Log("Cheat: Infinite Ammo (Player Only) = %d", g_bInfAmmo);
    }

    // -------------------------------------------------------------
    // 2. Infinite Grenades (with 0-Grenade Bypass & HUD Lock)
    // -------------------------------------------------------------
    static bool s_lastInfGrenades = false;
    if (g_bInfGrenades != s_lastInfGrenades) {
        s_lastInfGrenades = g_bInfGrenades;
        if (g_bInfGrenades) {
            // NOP the 0-grenade throw blocker (0x001ECBFA: 74 15 -> 90 90)
            uint8_t nops2[2] = {0x90, 0x90};
            PatchMemory(0x001ECBFA, nops2, 2, g_OrigGrenCheck);

            // Hook HUD grenade getter (0x001F9760: return 9)
            uint8_t hudCode[6] = {0xB8, 0x09, 0x00, 0x00, 0x00, 0xC3}; // mov eax, 9; ret
            PatchMemory(0x001F9760, hudCode, 6, g_OrigGrenHud);

            // NOP grenade decrements & maintain 9 in inventory slot
            uint8_t nops4[4] = {0x90, 0x90, 0x90, 0x90};
            uint8_t forceGren[7] = {0x66, 0xC7, 0x46, 0x0E, 0x09, 0x00, 0x90}; // mov word ptr [esi+0x0e], 9; nop
            PatchMemory(0x00043820, nops4, 4, g_OrigGren1);
            PatchMemory(0x00043866, nops4, 4, g_OrigGren2);
            PatchMemory(0x00043872, nops4, 4, g_OrigGren3);
            PatchMemory(0x00043845, forceGren, 7, g_OrigGren4);
            Log("Cheat: Infinite Grenades ON (0-grenade blocker bypassed)");
        } else {
            PatchMemory(0x001ECBFA, g_OrigGrenCheck, 2);
            PatchMemory(0x001F9760, g_OrigGrenHud, 6);
            PatchMemory(0x00043820, g_OrigGren1, 4);
            PatchMemory(0x00043866, g_OrigGren2, 4);
            PatchMemory(0x00043872, g_OrigGren3, 4);
            PatchMemory(0x00043845, g_OrigGren4, 7);
            Log("Cheat: Infinite Grenades OFF (restored)");
        }
    }

    if (g_bInfGrenades) {
        // Ensure player data has grenades active
        uint32_t pPtr = *(uint32_t*)0x002D12CC;
        if (pPtr > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pPtr, sizeof(uint32_t))) {
            uint32_t pData = *(uint32_t*)(uintptr_t)pPtr;
            if (pData > 0x10000 && !IsBadWritePtr((void*)(uintptr_t)pData, 0x40)) {
                if (*(int16_t*)(uintptr_t)(pData + 0x1C) < 9) *(int16_t*)(uintptr_t)(pData + 0x1C) = 9;
                if (*(int16_t*)(uintptr_t)(pData + 0x1E) < 9) *(int16_t*)(uintptr_t)(pData + 0x1E) = 9;
                if (*(int16_t*)(uintptr_t)(pData + 0x2A) < 9) *(int16_t*)(uintptr_t)(pData + 0x2A) = 9;
            }
        }
    }

    // -------------------------------------------------------------
    // 3. 100% Unbreakable Damage Immunity (NEVER "God Mode")
    // -------------------------------------------------------------
    static bool s_lastImmunity = false;
    if (g_bDamageImmunity != s_lastImmunity) {
        s_lastImmunity = g_bDamageImmunity;
        if (g_bDamageImmunity) {
            // Immediate safe return at entrance of player's ReceiveDamage (0x001E3B50)
            uint8_t immHead[5] = {0xC2, 0x04, 0x00, 0x90, 0x90}; // ret $4; nop; nop
            PatchMemory(0x001E3B50, immHead, 5, g_OrigDamageHead);

            // Safe return at secondary branches as backup
            uint8_t immPatch[8] = {0x5B, 0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0x00, 0x90}; // pop ebx; mov esp, ebp; pop ebp; ret $4; nop
            PatchMemory(0x001E3BA8, immPatch, 8, g_OrigDamageBranch2);
            PatchMemory(0x001E3BE1, immPatch, 8, g_OrigDamageBranch1);
            Log("Cheat: Damage Immunity ON (100% unbreakable player health)");
        } else {
            PatchMemory(0x001E3B50, g_OrigDamageHead, 5);
            PatchMemory(0x001E3BA8, g_OrigDamageBranch2, 8);
            PatchMemory(0x001E3BE1, g_OrigDamageBranch1, 8);
            Log("Cheat: Damage Immunity OFF (restored)");
        }
    }

    // Active health maintenance while Damage Immunity is active
    if (g_bDamageImmunity) {
        FullHealPlayer();
    }

    // -------------------------------------------------------------
    // 4. Rapid Fire (Zero Cooldown Between Shots)
    // -------------------------------------------------------------
    static bool s_lastRapidFire = false;
    static bool s_rapidHookInstalled = false;
    if (!s_rapidHookInstalled) {
        s_rapidHookInstalled = true;
        memcpy(g_OrigRapidFire, (void*)0x001EFC72, 6);
        uint8_t patchRF[6] = {0xE9, 0x00, 0x00, 0x00, 0x00, 0x90};
        int32_t rel = (int32_t)((uintptr_t)Hook_RapidFire - (0x001EFC72 + 5));
        memcpy(patchRF + 1, &rel, 4);
        PatchMemory(0x001EFC72, patchRF, 6);
        Log("Player Rapid Fire trampoline installed at 0x001EFC72 -> %p", Hook_RapidFire);
    }
    if (g_bRapidFire != s_lastRapidFire) {
        s_lastRapidFire = g_bRapidFire;
        Log("Cheat: Rapid Fire = %d", g_bRapidFire);
    }

    // -------------------------------------------------------------
    // 5. No Recoil / No Screen Kick
    // -------------------------------------------------------------
    static bool s_lastNoRecoil = false;
    if (g_bNoRecoil != s_lastNoRecoil) {
        s_lastNoRecoil = g_bNoRecoil;
        if (g_bNoRecoil) {
            uint8_t nops8[8] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
            PatchMemory(0x001EFB1F, nops8, 8, g_OrigRecoil1);
            PatchMemory(0x001EFB30, nops8, 8, g_OrigRecoil2);
            PatchMemory(0x001EFB44, nops8, 8, g_OrigRecoil3);
            Log("Cheat: No Recoil ON");
        } else {
            PatchMemory(0x001EFB1F, g_OrigRecoil1, 8);
            PatchMemory(0x001EFB30, g_OrigRecoil2, 8);
            PatchMemory(0x001EFB44, g_OrigRecoil3, 8);
            Log("Cheat: No Recoil OFF");
        }
    }

    // -------------------------------------------------------------
    // 6. One-Hit Kill (Lethal Damage to All Enemies)
    // -------------------------------------------------------------
    static bool s_lastOneHitKill = false;
    if (g_bOneHitKill != s_lastOneHitKill) {
        s_lastOneHitKill = g_bOneHitKill;
        if (g_bOneHitKill) {
            // Force enemy damage calculation at 0x001E3974 to instant fatal division
            uint8_t killPatch[8] = {0xF3, 0x0F, 0x10, 0x0D, 0xC8, 0x88, 0x28, 0x00}; // movss xmm1, [70.0f]
            PatchMemory(0x001E3974, killPatch, 8, g_OrigEnemyDmg);
            Log("Cheat: One-Hit Kill ON");
        } else {
            PatchMemory(0x001E3974, g_OrigEnemyDmg, 8);
            Log("Cheat: One-Hit Kill OFF");
        }
    }

    // -------------------------------------------------------------
    // 7. Custom Field of View (FOV)
    // -------------------------------------------------------------
    if (g_bCustomFOV) {
        // Direct write to port's FOV variable pointed to by 0x001BB2CE
        uint32_t pFovPtr = *(uint32_t*)0x001BB2CE;
        if (pFovPtr > 0x10000 && !IsBadWritePtr((void*)(uintptr_t)pFovPtr, sizeof(float))) {
            *(float*)(uintptr_t)pFovPtr = g_fCustomFOV;
        }

        uint32_t pCameraMgr = *(uint32_t*)0x002D12B8;
        if (pCameraMgr > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pCameraMgr, 0x800)) {
            *(float*)(uintptr_t)(pCameraMgr + 0x750) = g_fCustomFOV;
        }
    }

    // -------------------------------------------------------------
    // 8. Custom Game Speed / Simulation Pace
    // -------------------------------------------------------------
    if (g_bCustomSpeed) {
        uint32_t pWorld = *(uint32_t*)0x002D45D4;
        if (pWorld > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pWorld, 0x30)) {
            float baseDt = 1.0f / 60.0f;
            *(float*)(uintptr_t)(pWorld + 0x1C) = baseDt * g_fGameSpeed;
        }
    }
}

// --- Window Procedure Hook ---
static volatile LONG g_WheelDelta = 0;

static LRESULT CALLBACK Hooked_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_bMenuOpen) {
        switch (msg) {
            case WM_INPUT:
                // Raw mouse drives game camera and firing: swallow while mod menu is open
                return DefWindowProcW(hWnd, msg, wParam, lParam);
            case WM_MOUSEWHEEL:
                InterlockedExchangeAdd(&g_WheelDelta, (SHORT)HIWORD(wParam));
                return 0;
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                return 0;
            case WM_SETCURSOR:
                SetCursor(nullptr);
                return TRUE;
        }
    }

    return CallWindowProcW(g_OrigWndProc, hWnd, msg, wParam, lParam);
}

// --- ImGui Style Setup (Modern Carbon & Amber Palette) ---
static void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 8.0f;
    style.FrameRounding     = 5.0f;
    style.GrabRounding      = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding       = 5.0f;
    style.FrameBorderSize   = 1.0f;
    style.WindowBorderSize  = 1.0f;
    style.WindowPadding     = ImVec2(12.0f, 12.0f);
    style.FramePadding      = ImVec2(8.0f, 4.0f);
    style.ItemSpacing       = ImVec2(8.0f, 7.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.09f, 0.09f, 0.11f, 0.96f);
    colors[ImGuiCol_Border]               = ImVec4(0.35f, 0.35f, 0.38f, 0.70f);
    colors[ImGuiCol_TitleBg]              = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.25f, 0.18f, 0.06f, 1.00f);
    colors[ImGuiCol_Header]               = ImVec4(0.85f, 0.58f, 0.10f, 0.35f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.95f, 0.68f, 0.15f, 0.55f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(1.00f, 0.75f, 0.20f, 0.75f);
    colors[ImGuiCol_Button]               = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.85f, 0.58f, 0.10f, 0.70f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(1.00f, 0.72f, 0.15f, 0.90f);
    colors[ImGuiCol_CheckMark]            = ImVec4(1.00f, 0.78f, 0.20f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.88f, 0.60f, 0.10f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(1.00f, 0.75f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBg]              = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.30f, 0.30f, 0.35f, 1.00f);
    colors[ImGuiCol_Tab]                  = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.85f, 0.58f, 0.10f, 0.80f);
    colors[ImGuiCol_TabSelected]          = ImVec4(0.70f, 0.48f, 0.08f, 1.00f);
    colors[ImGuiCol_TabDimmed]            = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TabDimmedSelected]    = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_Text]                 = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.55f, 0.58f, 1.00f);
}

// --- Render Mod Menu UI ---
static void RenderModMenu() {
    if (!g_bMenuOpen) return;

    ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("BLACK PC - MOD MENU [F1]", &g_bMenuOpen, ImGuiWindowFlags_NoCollapse)) {
        // Header
        ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.20f, 1.0f), "CRITERION BLACK (2006) - PC PORT ENHANCER");
        ImGui::SameLine(ImGui::GetWindowWidth() - 170);
        ImGui::TextDisabled("Press [F1] to close");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_None)) {
            // =========================================================
            // TAB 1: PLAYER & HEALTH
            // =========================================================
            if (ImGui::BeginTabItem("Player")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "DEFENSE & SURVIVAL");
                ImGui::Separator();
                ImGui::Spacing();

                // Damage Immunity
                ImGui::Checkbox("Damage Immunity (Never Die)", &g_bDamageImmunity);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("100% immunity to bullets, heavy explosions, fall damage, and fatal cutscene triggers.");

                ImGui::SameLine();
                if (g_bDamageImmunity)
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[ACTIVE]");
                else
                    ImGui::TextDisabled("[OFF]");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Live Player Vitals
                uint32_t pGame = *(uint32_t*)0x002D45F0;
                if (pGame > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pGame, 0x20170)) {
                    uint32_t pips = *(uint32_t*)(uintptr_t)(pGame + 0x20150);
                    uint32_t maxPips = *(uint32_t*)(uintptr_t)(pGame + 0x20154);
                    float segHp = *(float*)(uintptr_t)(pGame + 0x20158) * 100.0f;
                    uint32_t packs = *(uint32_t*)(uintptr_t)(pGame + 0x2014C);

                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "Player Status: In-Game Active");
                    ImGui::Text("Health Segments: %u / %u (Current: %.0f%%)", pips, maxPips ? maxPips : 4, segHp);
                    ImGui::Text("Health Packs: %u / 3", packs);

                    ImGui::Spacing();
                    if (ImGui::Button("Instant Full Heal (4 Pips / 100%)")) {
                        FullHealPlayer();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Refill Medkits (3 Packs)")) {
                        *(uint32_t*)(uintptr_t)(pGame + 0x2014C) = 3;
                    }
                } else {
                    ImGui::TextDisabled("Player inactive (in mission load, cutscene, or menu)");
                }

                ImGui::EndTabItem();
            }

            // =========================================================
            // TAB 2: WEAPONS & AMMO
            // =========================================================
            if (ImGui::BeginTabItem("Weapons & Ammo")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "AMMUNITION & FIRING CONTROLS");
                ImGui::Separator();
                ImGui::Spacing();

                // 1. Infinite Ammo (Player Only)
                ImGui::Checkbox("Infinite Ammo (Player Only)", &g_bInfAmmo);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Magazine and reserve ammo never deplete for you.\nEnemies consume ammo and reload normally!");
                ImGui::SameLine();
                if (g_bInfAmmo)
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[ACTIVE - PLAYER ONLY]");
                else
                    ImGui::TextDisabled("[OFF]");

                // 2. Infinite Grenades
                ImGui::Checkbox("Infinite Grenades", &g_bInfGrenades);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Throw unlimited grenades freely, even if the mission started with 0 grenades!\nHUD remains locked at 9 grenades.");
                ImGui::SameLine();
                if (g_bInfGrenades)
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[ACTIVE]");
                else
                    ImGui::TextDisabled("[OFF]");

                // 3. Rapid Fire
                ImGui::Checkbox("Rapid Fire (Minigun Rate)", &g_bRapidFire);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Completely eliminates firing delay! Any weapon fires at maximum possible speed.");
                ImGui::SameLine();
                if (g_bRapidFire)
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[ACTIVE]");
                else
                    ImGui::TextDisabled("[OFF]");

                // 4. No Recoil
                ImGui::Checkbox("No Recoil / Perfect Spread", &g_bNoRecoil);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Eliminates weapon kick and screen shake while firing.");
                ImGui::SameLine();
                if (g_bNoRecoil)
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[ACTIVE]");
                else
                    ImGui::TextDisabled("[OFF]");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Refill Current Weapon Ammo & Reserve (999)")) {
                    RefillActiveAmmo();
                }

                ImGui::EndTabItem();
            }

            // =========================================================
            // TAB 3: WEAPON ARMORY & SELECTOR
            // =========================================================
            if (ImGui::BeginTabItem("Weapon Armory")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "WEAPON SELECTOR & REPLACER");
                ImGui::TextDisabled("Select any weapon in BLACK and equip it directly into your active slot.");
                ImGui::Separator();
                ImGui::Spacing();

                // Current Equipped Status
                uint32_t pGame = *(uint32_t*)0x002D45F0;
                if (pGame > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pGame, 0x20300)) {
                    uint8_t slot0 = *(uint8_t*)(uintptr_t)(pGame + 0x2020C);
                    uint8_t slot1 = *(uint8_t*)(uintptr_t)(pGame + 0x2020E);
                    uint8_t activeSlot = *(uint8_t*)(uintptr_t)(pGame + 0x2020D);

                    const char* name0 = (slot0 < kNumWeapons) ? kWeapons[slot0].name : "Custom / Unknown";
                    const char* name1 = (slot1 < kNumWeapons) ? kWeapons[slot1].name : "Custom / Unknown";

                    ImGui::Text("Primary Slot (1)   : %s [ID %u]", name0, slot0);
                    ImGui::Text("Secondary Slot (2) : %s [ID %u]", name1, slot1);
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Active Slot        : Slot %u", (activeSlot > 1 ? 0 : activeSlot) + 1);
                }

                ImGui::Spacing();

                // Weapon List Selector
                ImGui::Text("Available Weapons:");
                if (ImGui::BeginChild("WeaponList", ImVec2(0, 160), true)) {
                    for (int i = 0; i < kNumWeapons; i++) {
                        char label[128];
                        snprintf(label, sizeof(label), "%s - %s [%s]", kWeapons[i].name, kWeapons[i].category, kWeapons[i].description);
                        bool isSelected = (g_SelectedWeaponIdx == i);
                        if (ImGui::Selectable(label, isSelected)) {
                            g_SelectedWeaponIdx = i;
                        }
                    }
                    ImGui::EndChild();
                }

                ImGui::Spacing();
                if (g_SelectedWeaponIdx >= 0 && g_SelectedWeaponIdx < kNumWeapons) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Selected: %s (%s)", kWeapons[g_SelectedWeaponIdx].name, kWeapons[g_SelectedWeaponIdx].category);
                }

                if (ImGui::Button("Equip Selected Weapon into Active Slot", ImVec2(280, 28))) {
                    EquipWeapon(kWeapons[g_SelectedWeaponIdx].id);
                }

                ImGui::SameLine();
                if (ImGui::Button("Swap Primary / Secondary", ImVec2(200, 28))) {
                    if (pGame > 0x10000 && !IsBadWritePtr((void*)(uintptr_t)pGame, 0x20300)) {
                        uint8_t cur = *(uint8_t*)(uintptr_t)(pGame + 0x2020E);
                        *(uint8_t*)(uintptr_t)(pGame + 0x2020E) = (cur == 0) ? 1 : 0;
                        *(uint8_t*)(uintptr_t)(pGame + 0x2020D) = 1;
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Built-in Weapon Unlock Codes
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Official Profile Cheat Codes:");
                ImGui::BulletText("M249 SAW (BFG in City Streets): ");
                ImGui::SameLine();
                if (ImGui::SmallButton("5SQQ-5THA-ZFFV-7XEV (Copy)")) {
                    ImGui::SetClipboardText("5SQQ-5THA-ZFFV-7XEV");
                }

                ImGui::BulletText("Silver Weapons (All Levels): ");
                ImGui::SameLine();
                if (ImGui::SmallButton("HQ6G-ZP3C-C9RK-X865 (Copy)")) {
                    ImGui::SetClipboardText("HQ6G-ZP3C-C9RK-X865");
                }
                ImGui::TextDisabled("Enter either code as Profile Name to unlock.");

                ImGui::EndTabItem();
            }

            // =========================================================
            // TAB 4: COMBAT & MOVEMENT
            // =========================================================
            if (ImGui::BeginTabItem("Combat & Speed")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "COMBAT & SIMULATION PACE");
                ImGui::Separator();
                ImGui::Spacing();

                // One-Hit Kill
                ImGui::Checkbox("One-Hit Kill (Lethal Damage to Enemies)", &g_bOneHitKill);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("All enemy targets die in a single shot!");
                ImGui::SameLine();
                if (g_bOneHitKill)
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[ACTIVE]");
                else
                    ImGui::TextDisabled("[OFF]");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Game Speed Multiplier
                ImGui::Checkbox("Enable Custom Game Speed", &g_bCustomSpeed);
                if (g_bCustomSpeed) {
                    ImGui::SliderFloat("Speed Scale", &g_fGameSpeed, 0.5f, 3.0f, "%.2fx");
                    if (ImGui::Button("Reset to Normal (1.0x)")) {
                        g_fGameSpeed = 1.0f;
                    }
                }

                ImGui::EndTabItem();
            }

            // =========================================================
            // TAB 5: VISUALS
            // =========================================================
            if (ImGui::BeginTabItem("Visuals")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "CAMERA & RENDER SETTINGS");
                ImGui::Separator();
                ImGui::Spacing();

                // Custom FOV
                ImGui::Checkbox("Custom Field of View (FOV)", &g_bCustomFOV);
                if (g_bCustomFOV) {
                    ImGui::SliderFloat("FOV Angle", &g_fCustomFOV, 50.0f, 120.0f, "%.0f degrees");
                    if (ImGui::Button("Default FOV (70 deg)")) {
                        g_fCustomFOV = 70.0f;
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Wireframe Mode
                ImGui::Checkbox("Wireframe Mode", &g_bWireframe);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Render 3D scene geometry in wireframe mode.");

                ImGui::EndTabItem();
            }

            // =========================================================
            // TAB 6: HELP & CONTROLS
            // =========================================================
            if (ImGui::BeginTabItem("Help")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "CONTROLS & SHORTCUTS");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::BulletText("[F1]  : Toggle Mod Menu");
                ImGui::BulletText("[Esc] : Pause Game & Release Cursor");
                ImGui::BulletText("[1/2] : Switch Weapons In-Game");
                ImGui::BulletText("[G]   : Throw Grenade");
                ImGui::BulletText("[R]   : Reload Weapon");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Platform: BLACK PC 0.3.0");
                ImGui::TextDisabled("Direct3D 9 ImGui Hook Active & Running");

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// --- Hooked D3D9 Device Methods ---
static HRESULT WINAPI Hook_Present(IDirect3DDevice9* pDevice, const RECT* pSrc, const RECT* pDest, HWND hWndDest, const RGNDATA* pDirty) {
    ApplyCheats();

    // F1 strictly polled every frame
    static bool s_bF1WasDown = false;
    bool bF1IsDown = (GetAsyncKeyState(VK_F1) & 0x8000) != 0 && (!g_hWnd || GetForegroundWindow() == g_hWnd);
    if (bF1IsDown && !s_bF1WasDown) {
        g_bMenuOpen = !g_bMenuOpen;
        Log("F1 PRESSED! g_bMenuOpen = %d", g_bMenuOpen);
        if (g_bMenuOpen) ClipCursor(nullptr);
    }
    s_bF1WasDown = bF1IsDown;
    if (g_bMenuOpen) ClipCursor(nullptr);

    if (!g_bImGuiInit) {
        if (!g_hWnd) {
            D3DDEVICE_CREATION_PARAMETERS cp = {};
            pDevice->GetCreationParameters(&cp);
            g_hWnd = cp.hFocusWindow;
            if (!g_hWnd) g_hWnd = FindWindowW(L"BlackLoaderWindow", nullptr);
            if (!g_hWnd) g_hWnd = GetActiveWindow();
        }

        if (g_hWnd) {
            Log("Initializing ImGui on hWnd=%p", g_hWnd);
            g_OrigWndProc = (WNDPROC)SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, (LONG_PTR)Hooked_WndProc);
            ImGui::CreateContext();
            SetupImGuiStyle();
            ImGui_ImplWin32_Init(g_hWnd);
            ImGui_ImplDX9_Init(pDevice);
            g_bImGuiInit = true;
            Log("ImGui successfully initialized!");
        }
    }

    if (g_bImGuiInit && g_bMenuOpen) {
        IDirect3DSurface9* backBuffer = nullptr;
        if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
            return Orig_Present(pDevice, pSrc, pDest, hWndDest, pDirty);
        D3DSURFACE_DESC bbDesc = {};
        backBuffer->GetDesc(&bbDesc);
        float frameW = pSrc ? (float)(pSrc->right - pSrc->left) : (float)bbDesc.Width;
        float frameH = pSrc ? (float)(pSrc->bottom - pSrc->top) : (float)bbDesc.Height;

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = true;
        RECT client = {};
        GetClientRect(g_hWnd, &client);
        float clientW = (float)(client.right - client.left), clientH = (float)(client.bottom - client.top);
        io.DisplaySize = ImVec2(frameW, frameH);

        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(g_hWnd, &pt) && clientW > 0 && clientH > 0)
            io.AddMousePosEvent(pt.x * frameW / clientW, pt.y * frameH / clientH);
        bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(swap ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(swap ? VK_LBUTTON : VK_RBUTTON) & 0x8000) != 0);
        LONG wheel = InterlockedExchange(&g_WheelDelta, 0);
        if (wheel) io.AddMouseWheelEvent(0.0f, (float)wheel / WHEEL_DELTA);

        ImGui::NewFrame();
        RenderModMenu();
        ImGui::EndFrame();
        ImGui::Render();

        // Save D3D9 state and render onto the real back buffer
        IDirect3DStateBlock9* sb = nullptr;
        pDevice->CreateStateBlock(D3DSBT_ALL, &sb);
        IDirect3DSurface9* oldRT = nullptr;
        IDirect3DSurface9* oldDS = nullptr;
        pDevice->GetRenderTarget(0, &oldRT);
        pDevice->GetDepthStencilSurface(&oldDS);

        pDevice->SetRenderTarget(0, backBuffer);
        for (DWORD i = 1; i < 4; i++) pDevice->SetRenderTarget(i, nullptr);
        pDevice->SetDepthStencilSurface(nullptr);
        pDevice->SetVertexShader(nullptr);
        pDevice->SetPixelShader(nullptr);
        pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        pDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        pDevice->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);

        // Reset texture transforms and stages so font glyph UVs are not scrambled
        D3DMATRIX mat_identity = { { { 1.0f, 0.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f } } };
        for (DWORD i = 0; i < 8; i++) {
            pDevice->SetTextureStageState(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            pDevice->SetTextureStageState(i, D3DTSS_TEXCOORDINDEX, i);
            pDevice->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + i), &mat_identity);
            pDevice->SetSamplerState(i, D3DSAMP_SRGBTEXTURE, FALSE);
            pDevice->SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            if (i > 0) {
                pDevice->SetTextureStageState(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
                pDevice->SetTextureStageState(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                pDevice->SetTexture(i, nullptr);
            }
        }

        HRESULT hrBegin = pDevice->BeginScene();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        if (SUCCEEDED(hrBegin)) pDevice->EndScene();

        pDevice->SetRenderTarget(0, oldRT);
        pDevice->SetDepthStencilSurface(oldDS);
        if (oldRT) oldRT->Release();
        if (oldDS) oldDS->Release();
        if (sb) { sb->Apply(); sb->Release(); }
        backBuffer->Release();
    }

    // Wireframe Mode
    static bool s_lastWire = false;
    if (g_bWireframe != s_lastWire) {
        s_lastWire = g_bWireframe;
        pDevice->SetRenderState(D3DRS_FILLMODE, g_bWireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    }

    return Orig_Present(pDevice, pSrc, pDest, hWndDest, pDirty);
}

static HRESULT WINAPI Hook_Reset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPP) {
    Log("Hook_Reset called");
    if (g_bImGuiInit) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    HRESULT hr = Orig_Reset(pDevice, pPP);
    if (SUCCEEDED(hr) && g_bImGuiInit) {
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    return hr;
}

static HRESULT WINAPI Hook_CreateDevice(IDirect3D9* pD3D, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                       DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPP, IDirect3DDevice9** ppReturnedDevice) {
    Log("Hook_CreateDevice called (hFocusWindow=%p, pPP->hDeviceWindow=%p)", hFocusWindow, pPP ? pPP->hDeviceWindow : 0);
    if (hFocusWindow) g_hWnd = hFocusWindow;
    else if (pPP && pPP->hDeviceWindow) g_hWnd = pPP->hDeviceWindow;

    HRESULT hr = Orig_CreateDevice(pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppReturnedDevice);
    if (SUCCEEDED(hr) && ppReturnedDevice && *ppReturnedDevice) {
        IDirect3DDevice9* pDev = *ppReturnedDevice;
        Log("Hooking IDirect3DDevice9 at %p", pDev);
        Orig_Present = (PFN_Present)HookVTable(pDev, 17, (void*)Hook_Present);
        Orig_Reset = (PFN_Reset)HookVTable(pDev, 16, (void*)Hook_Reset);
    }
    return hr;
}

// --- DLL Exports ---
extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    Log("Direct3DCreate9 called (SDKVersion=%u)", SDKVersion);
    GetRealD3D9();
    if (!Real_Direct3DCreate9) {
        Log("ERROR: Real_Direct3DCreate9 is NULL");
        return nullptr;
    }

    IDirect3D9* pD3D = Real_Direct3DCreate9(SDKVersion);
    Log("Real Direct3DCreate9 returned %p", pD3D);
    if (pD3D) {
        Orig_CreateDevice = (PFN_CreateDevice)HookVTable(pD3D, 16, (void*)Hook_CreateDevice);
    }
    return pD3D;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
    Log("Direct3DCreate9Ex called");
    GetRealD3D9();
    if (!Real_Direct3DCreate9Ex) return D3DERR_NOTAVAILABLE;
    return Real_Direct3DCreate9Ex(SDKVersion, ppD3D);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        Log("=== BLACK PC Mod Menu DLL Attached (process=%lu) ===", GetCurrentProcessId());
    }
    return TRUE;
}

}
