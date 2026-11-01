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
        if (hReal) {
            Real_Direct3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(hReal, "Direct3DCreate9");
            Real_Direct3DCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(hReal, "Direct3DCreate9Ex");
        }
    }
    return hReal;
}

// --- VTable Hooking Helper ---
static void** HookVTable(void* pInterface, int index, void* newFunc) {
    void** vtable = *(void***)pInterface;
    DWORD oldProtect;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    void** orig = (void**)vtable[index];
    vtable[index] = newFunc;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    return orig;
}

// --- Function Pointers ---
typedef HRESULT (WINAPI *PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (WINAPI *PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (WINAPI *PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (WINAPI *PFN_EndScene)(IDirect3DDevice9*);

static PFN_CreateDevice Orig_CreateDevice = nullptr;
static PFN_Present Orig_Present = nullptr;
static PFN_Reset Orig_Reset = nullptr;
static PFN_EndScene Orig_EndScene = nullptr;

// --- State & Cheats ---
static HWND g_hWnd = nullptr;
static WNDPROC g_OrigWndProc = nullptr;
static bool g_bMenuOpen = false;
static bool g_bImGuiInit = false;

// Cheat Toggles
static bool g_bGodMode = false;
static bool g_bInfAmmo = false;
static bool g_bInfGrenades = false;
static bool g_bRapidFire = false;
static bool g_bWireframe = false;
static float g_fCustomFOV = 70.0f;
static bool g_bCustomFOV = false;
static float g_fGameSpeed = 1.0f;

// Memory patching helper
static void PatchMemory(uint32_t addr, const void* bytes, size_t len, void* origSave = nullptr) {
    DWORD oldProtect;
    VirtualProtect((void*)(uintptr_t)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect);
    if (origSave) memcpy(origSave, (void*)(uintptr_t)addr, len);
    memcpy((void*)(uintptr_t)addr, bytes, len);
    VirtualProtect((void*)(uintptr_t)addr, len, oldProtect, &oldProtect);
}

// Original bytes for memory cheats
static uint8_t g_OrigAmmo[4] = {0x66, 0xFF, 0x48, 0x18};
static uint8_t g_OrigGren1[4] = {0x66, 0xFF, 0x4E, 0x0E};
static uint8_t g_OrigGren2[4] = {0x66, 0xFF, 0x4E, 0x0E};
static uint8_t g_OrigGren3[4] = {0x66, 0x89, 0x4E, 0x0E};
static uint8_t g_OrigGodPrologue[32] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};

static void ApplyCheats() {
    // 1. Infinite Ammo (VA 0x001EFBBB)
    static bool s_lastInfAmmo = false;
    if (g_bInfAmmo != s_lastInfAmmo) {
        s_lastInfAmmo = g_bInfAmmo;
        if (g_bInfAmmo) {
            uint8_t nops[4] = {0x90, 0x90, 0x90, 0x90};
            PatchMemory(0x001EFBBB, nops, 4, g_OrigAmmo);
        } else {
            PatchMemory(0x001EFBBB, g_OrigAmmo, 4);
        }
    }

    // 2. Infinite Grenades (VA 0x00043820, 0x00043866, 0x00043872)
    static bool s_lastInfGrenades = false;
    if (g_bInfGrenades != s_lastInfGrenades) {
        s_lastInfGrenades = g_bInfGrenades;
        if (g_bInfGrenades) {
            uint8_t nops[4] = {0x90, 0x90, 0x90, 0x90};
            PatchMemory(0x00043820, nops, 4, g_OrigGren1);
            PatchMemory(0x00043866, nops, 4, g_OrigGren2);
            PatchMemory(0x00043872, nops, 4, g_OrigGren3);
        } else {
            PatchMemory(0x00043820, g_OrigGren1, 4);
            PatchMemory(0x00043866, g_OrigGren2, 4);
            PatchMemory(0x00043872, g_OrigGren3, 4);
        }
    }

    // 3. God Mode (Damage Filter Hook at VA 0x001D0730)
    static bool s_lastGod = false;
    if (g_bGodMode != s_lastGod) {
        s_lastGod = g_bGodMode;
        if (g_bGodMode) {
            // Check if victim ([esp+4]) == *(uint32_t*)0x002D45D4
            // If match: ret 0x18
            // Else jump to original prologue
            // 8B 44 24 04          mov eax, [esp+4]
            // 3B 05 D4 45 2D 00    cmp eax, [0x002D45D4]
            // 75 03                jne +3
            // C2 18 00             ret 0x18
            // 55 8B EC             push ebp; mov ebp, esp
            // (We can hook with a trampoline)
            static uint8_t godHook[] = {
                0x8B, 0x44, 0x24, 0x04,             // mov eax, [esp+4]
                0x3B, 0x05, 0xD4, 0x45, 0x2D, 0x00, // cmp eax, [0x002D45D4]
                0x75, 0x03,                         // jne skip
                0xC2, 0x18, 0x00,                   // ret 0x18
                0x55, 0x8B, 0xEC                    // push ebp; mov ebp, esp
            };
            // For simplicity and 100% safety, unconditional ret 0x18 makes all entities invulnerable,
            // or victim check ensures only the player is invulnerable!
            // Let's use the player check trampoline:
            PatchMemory(0x001D0730, godHook, sizeof(godHook), g_OrigGodPrologue);
        } else {
            PatchMemory(0x001D0730, g_OrigGodPrologue, sizeof(g_OrigGodPrologue));
        }
    }

    // 4. Custom FOV in real time
    if (g_bCustomFOV) {
        uint32_t pCameraMgr = *(uint32_t*)0x002D12B8;
        if (pCameraMgr && pCameraMgr > 0x10000) {
            *(float*)(uintptr_t)(pCameraMgr + 0x750) = g_fCustomFOV;
        }
    }
}

// --- Window Procedure Hook ---
static LRESULT CALLBACK Hooked_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && (wParam == VK_INSERT || wParam == VK_F1)) {
        g_bMenuOpen = !g_bMenuOpen;
        if (g_bMenuOpen) {
            ClipCursor(nullptr);
            ShowCursor(TRUE);
        } else {
            ShowCursor(FALSE);
        }
        return 0;
    }

    if (g_bMenuOpen) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        // Intercept mouse clicks and wheel while menu is open so they don't fire guns
        switch (msg) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:
                return 0;
        }
    }

    return CallWindowProcW(g_OrigWndProc, hWnd, msg, wParam, lParam);
}

// --- ImGui Style Setup ---
static void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.08f, 0.09f, 0.94f);
    colors[ImGuiCol_Border]               = ImVec4(0.35f, 0.35f, 0.38f, 0.70f);
    colors[ImGuiCol_TitleBg]              = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.20f, 0.16f, 0.08f, 1.00f);
    colors[ImGuiCol_Header]               = ImVec4(0.80f, 0.55f, 0.10f, 0.35f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.95f, 0.65f, 0.12f, 0.50f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(1.00f, 0.70f, 0.15f, 0.70f);
    colors[ImGuiCol_Button]               = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.80f, 0.55f, 0.10f, 0.60f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.95f, 0.65f, 0.15f, 0.85f);
    colors[ImGuiCol_CheckMark]            = ImVec4(1.00f, 0.75f, 0.15f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.80f, 0.55f, 0.10f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(1.00f, 0.75f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBg]              = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
    colors[ImGuiCol_Text]                 = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
}

// --- Render Mod Menu UI ---
static void RenderModMenu() {
    if (!g_bMenuOpen) return;

    ImGui::SetNextWindowSize(ImVec2(480, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("BLACK PC - OVERLAY MOD MENU", &g_bMenuOpen, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.15f, 1.0f), "CRITERION BLACK (2006) - PC PORT ENHANCER");
        ImGui::TextDisabled("Press [INSERT] or [F1] to toggle this menu");
        ImGui::Separator();

        if (ImGui::BeginTabBar("Tabs")) {
            // --- TAB 1: PLAYER & HEALTH ---
            if (ImGui::BeginTabItem("Player")) {
                ImGui::Spacing();
                ImGui::Checkbox("God Mode (Invulnerability)", &g_bGodMode);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Immunity to bullets, explosions, and all incoming damage!");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                uint32_t pPlayer = *(uint32_t*)0x002D45D4;
                if (pPlayer) {
                    ImGui::Text("Player Object: 0x%08X", pPlayer);
                } else {
                    ImGui::TextDisabled("Player not active (in menu or loading)");
                }

                ImGui::EndTabItem();
            }

            // --- TAB 2: WEAPONS & AMMO ---
            if (ImGui::BeginTabItem("Weapons")) {
                ImGui::Spacing();
                ImGui::Checkbox("Infinite Ammo (No Reload)", &g_bInfAmmo);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Current magazine never depletes - infinite continuous firing!");

                ImGui::Checkbox("Infinite Grenades", &g_bInfGrenades);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Throw unlimited grenades without losing inventory!");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "BFG (M249 SAW) Profile Code:");
                ImGui::Selectable("5SQQ-5THA-ZFFV-7XEV");
                if (ImGui::IsItemClicked()) {
                    ImGui::SetClipboardText("5SQQ-5THA-ZFFV-7XEV");
                }
                ImGui::TextDisabled("(Click to copy; enter as Profile Name to unlock BFG on Level 1)");

                ImGui::EndTabItem();
            }

            // --- TAB 3: VISUALS & CAMERA ---
            if (ImGui::BeginTabItem("Visuals")) {
                ImGui::Spacing();
                ImGui::Checkbox("Wireframe Mode", &g_bWireframe);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Render scene geometry in wireframe mode");

                ImGui::Spacing();
                ImGui::Checkbox("Override Field of View (FOV)", &g_bCustomFOV);
                if (g_bCustomFOV) {
                    ImGui::SliderFloat("FOV Angle", &g_fCustomFOV, 60.0f, 130.0f, "%.0f deg");
                }

                ImGui::EndTabItem();
            }

            // --- TAB 4: MISC & SHORTCUTS ---
            if (ImGui::BeginTabItem("Help")) {
                ImGui::Spacing();
                ImGui::Text("Controls:");
                ImGui::BulletText("[Insert] or [F1] : Toggle Mod Menu");
                ImGui::BulletText("[Esc]            : Pause Game & Free Mouse");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Port Version: BLACK PC 0.3.0");
                ImGui::TextDisabled("Direct3D 9 x86 Native Hook");

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

    if (!g_bImGuiInit) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        pDevice->GetCreationParameters(&cp);
        g_hWnd = cp.hFocusWindow ? cp.hFocusWindow : FindWindowW(L"BlackLoaderWindow", nullptr);

        if (g_hWnd) {
            g_OrigWndProc = (WNDPROC)SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, (LONG_PTR)Hooked_WndProc);
            ImGui::CreateContext();
            SetupImGuiStyle();
            ImGui_ImplWin32_Init(g_hWnd);
            ImGui_ImplDX9_Init(pDevice);
            g_bImGuiInit = true;
        }
    }

    if (g_bImGuiInit) {
        if (g_bWireframe) {
            pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderModMenu();

        ImGui::EndFrame();
        pDevice->BeginScene();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        pDevice->EndScene();

        if (g_bWireframe) {
            pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        }
    }

    return Orig_Present(pDevice, pSrc, pDest, hWndDest, pDirty);
}

static HRESULT WINAPI Hook_Reset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPP) {
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
    HRESULT hr = Orig_CreateDevice(pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppReturnedDevice);
    if (SUCCEEDED(hr) && ppReturnedDevice && *ppReturnedDevice) {
        IDirect3DDevice9* pDev = *ppReturnedDevice;
        Orig_Present = (PFN_Present)HookVTable(pDev, 17, (void*)Hook_Present);
        Orig_Reset = (PFN_Reset)HookVTable(pDev, 16, (void*)Hook_Reset);
    }
    return hr;
}

// --- DLL Exports ---
extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    GetRealD3D9();
    if (!Real_Direct3DCreate9) return nullptr;

    IDirect3D9* pD3D = Real_Direct3DCreate9(SDKVersion);
    if (pD3D) {
        Orig_CreateDevice = (PFN_CreateDevice)HookVTable(pD3D, 16, (void*)Hook_CreateDevice);
    }
    return pD3D;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
    GetRealD3D9();
    if (!Real_Direct3DCreate9Ex) return D3DERR_NOTAVAILABLE;
    return Real_Direct3DCreate9Ex(SDKVersion, ppD3D);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

}
