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
extern "C" bool g_bDamageImmunity = false;
static bool g_bInfAmmo = false;
static bool g_bInfGrenades = false;
static bool g_bWireframe = false;
static float g_fCustomFOV = 70.0f;
static bool g_bCustomFOV = false;

// Memory patching helper
static void PatchMemory(uint32_t addr, const void* bytes, size_t len, void* origSave = nullptr) {
    DWORD oldProtect;
    if (VirtualProtect((void*)(uintptr_t)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (origSave) memcpy(origSave, (void*)(uintptr_t)addr, len);
        memcpy((void*)(uintptr_t)addr, bytes, len);
        VirtualProtect((void*)(uintptr_t)addr, len, oldProtect, &oldProtect);
    }
}

// Original bytes for memory cheats
static uint8_t g_OrigAmmo[4] = {0x66, 0xFF, 0x48, 0x18};
static uint8_t g_OrigGren1[4] = {0x66, 0xFF, 0x4E, 0x0E};
static uint8_t g_OrigGren2[4] = {0x66, 0xFF, 0x4E, 0x0E};
static uint8_t g_OrigGren3[4] = {0x66, 0x89, 0x4E, 0x0E};
static uint8_t g_OrigGren4[7] = {0x66, 0x8B, 0x4E, 0x0E, 0x0F, 0xBF, 0xD1};

static uint8_t g_OrigDamageBranch1[8] = {0xF3, 0x0F, 0x10, 0x88, 0x34, 0x01, 0x00, 0x00};
static uint8_t g_OrigDamageBranch2[8] = {0xF3, 0x0F, 0x10, 0x88, 0x34, 0x01, 0x00, 0x00};

static uint32_t s_origFOV1 = 0x002888C8;
static uint32_t s_origFOV2 = 0x002888C8;
static uint32_t s_origFOV3 = 0x002888C8;

static void ApplyCheats() {
    // 1. Infinite Ammo (VA 0x001EFBBB)
    static bool s_lastInfAmmo = false;
    if (g_bInfAmmo != s_lastInfAmmo) {
        s_lastInfAmmo = g_bInfAmmo;
        if (g_bInfAmmo) {
            uint8_t nops[4] = {0x90, 0x90, 0x90, 0x90};
            PatchMemory(0x001EFBBB, nops, 4, g_OrigAmmo);
            Log("Cheat: Infinite Ammo ON");
        } else {
            PatchMemory(0x001EFBBB, g_OrigAmmo, 4);
            Log("Cheat: Infinite Ammo OFF");
        }
    }

    // 2. Infinite Grenades (VA 0x00043820, 0x00043866, 0x00043872, 0x00043845)
    static bool s_lastInfGrenades = false;
    if (g_bInfGrenades != s_lastInfGrenades) {
        s_lastInfGrenades = g_bInfGrenades;
        if (g_bInfGrenades) {
            uint8_t nops4[4] = {0x90, 0x90, 0x90, 0x90};
            uint8_t forceGren[7] = {0x66, 0xC7, 0x46, 0x0E, 0x09, 0x00, 0x90}; // mov word ptr [esi+0x0e], 9; nop
            PatchMemory(0x00043820, nops4, 4, g_OrigGren1);
            PatchMemory(0x00043866, nops4, 4, g_OrigGren2);
            PatchMemory(0x00043872, nops4, 4, g_OrigGren3);
            PatchMemory(0x00043845, forceGren, 7, g_OrigGren4);
            Log("Cheat: Infinite Grenades ON");
        } else {
            PatchMemory(0x00043820, g_OrigGren1, 4);
            PatchMemory(0x00043866, g_OrigGren2, 4);
            PatchMemory(0x00043872, g_OrigGren3, 4);
            PatchMemory(0x00043845, g_OrigGren4, 7);
            Log("Cheat: Infinite Grenades OFF");
        }
    }

    // Ensure player always has grenades when Infinite Grenades is active
    if (g_bInfGrenades) {
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

    // 3. Damage Immunity (Safe return patch at VA 0x001E3BA8 and 0x001E3BE1)
    static bool s_lastImmunity = false;
    if (g_bDamageImmunity != s_lastImmunity) {
        s_lastImmunity = g_bDamageImmunity;
        if (g_bDamageImmunity) {
            // Immediate safe return: pop ebx; mov esp, ebp; pop ebp; ret $4; nop
            uint8_t immPatch[8] = {0x5B, 0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0x00, 0x90};
            PatchMemory(0x001E3BA8, immPatch, 8, g_OrigDamageBranch2);
            PatchMemory(0x001E3BE1, immPatch, 8, g_OrigDamageBranch1);
            Log("Cheat: Damage Immunity ON");
        } else {
            PatchMemory(0x001E3BA8, g_OrigDamageBranch2, 8);
            PatchMemory(0x001E3BE1, g_OrigDamageBranch1, 8);
            Log("Cheat: Damage Immunity OFF");
        }
    }

    // Active health maintenance while Damage Immunity is active
    if (g_bDamageImmunity) {
        uint32_t pGame = *(uint32_t*)0x002D45F0;
        if (pGame > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pGame, 0x20170)) {
            *(uint32_t*)(uintptr_t)(pGame + 0x2014C) = 3;    // Keep 3 health packs
            *(uint32_t*)(uintptr_t)(pGame + 0x20150) = 4;    // 4 health segments
            *(uint32_t*)(uintptr_t)(pGame + 0x20154) = 4;    // 4 max segments
            *(float*)(uintptr_t)(pGame + 0x20158) = 1.0f;    // 100% current segment
            *(float*)(uintptr_t)(pGame + 0x2015C) = 1.0f;    // 100% display
            *(float*)(uintptr_t)(pGame + 0x20160) = 1.0f;    // 100% overall
        }

        *(uint32_t*)0x002F4178 = 4;
        *(uint32_t*)0x002F417C = 100;
        *(uint32_t*)0x002F416C = 100;
        *(uint32_t*)0x002F4170 = 100;

        uint32_t pRend = *(uint32_t*)0x002D67F4;
        if (pRend > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pRend, 0xE000)) {
            *(float*)(uintptr_t)(pRend + 0xD5B4) = 1.0f;
        }
    }

    // 4. Custom Field of View (FOV)
    static bool s_lastCustomFOV = false;
    if (g_bCustomFOV != s_lastCustomFOV) {
        s_lastCustomFOV = g_bCustomFOV;
        if (g_bCustomFOV) {
            s_origFOV1 = *(uint32_t*)0x001BB2CE;
            s_origFOV2 = *(uint32_t*)0x00202F96;
            s_origFOV3 = *(uint32_t*)0x002038E9;

            uint32_t target = (uint32_t)(uintptr_t)&g_fCustomFOV;
            PatchMemory(0x001BB2CE, &target, 4);
            PatchMemory(0x00202F96, &target, 4);
            PatchMemory(0x002038E9, &target, 4);
            Log("Cheat: Custom FOV ON (pointing to %p)", &g_fCustomFOV);
        } else {
            PatchMemory(0x001BB2CE, &s_origFOV1, 4);
            PatchMemory(0x00202F96, &s_origFOV2, 4);
            PatchMemory(0x002038E9, &s_origFOV3, 4);
            Log("Cheat: Custom FOV OFF (restored)");
        }
    }

    if (g_bCustomFOV) {
        uint32_t pCameraMgr = *(uint32_t*)0x002D12B8;
        if (pCameraMgr && pCameraMgr > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pCameraMgr, 0x800)) {
            *(float*)(uintptr_t)(pCameraMgr + 0x750) = g_fCustomFOV;
        }
    }
}

// --- Window Procedure Hook ---
static volatile LONG g_WheelDelta = 0;

static LRESULT CALLBACK Hooked_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_bMenuOpen) {
        switch (msg) {
            case WM_INPUT:
                // raw mouse drives camera/firing: swallow while menu is open
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
    colors[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.08f, 0.09f, 0.95f);
    colors[ImGuiCol_Border]               = ImVec4(0.40f, 0.40f, 0.42f, 0.80f);
    colors[ImGuiCol_TitleBg]              = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.25f, 0.18f, 0.08f, 1.00f);
    colors[ImGuiCol_Header]               = ImVec4(0.85f, 0.60f, 0.10f, 0.40f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.95f, 0.70f, 0.15f, 0.60f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(1.00f, 0.75f, 0.20f, 0.80f);
    colors[ImGuiCol_Button]               = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.85f, 0.60f, 0.10f, 0.70f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(1.00f, 0.75f, 0.15f, 0.90f);
    colors[ImGuiCol_CheckMark]            = ImVec4(1.00f, 0.80f, 0.20f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.85f, 0.60f, 0.10f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(1.00f, 0.80f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.32f, 0.32f, 0.36f, 1.00f);
    colors[ImGuiCol_Text]                 = ImVec4(0.98f, 0.98f, 0.98f, 1.00f);
}

// --- Render Mod Menu UI ---
static void RenderModMenu() {
    if (!g_bMenuOpen) return;

    ImGui::SetNextWindowSize(ImVec2(520, 430), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("BLACK PC - MOD MENU [F1]", &g_bMenuOpen, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.20f, 1.0f), "CRITERION BLACK (2006) - PC PORT ENHANCER");
        ImGui::TextDisabled("Press [F1] to close / open this menu");
        ImGui::Separator();

        if (ImGui::BeginTabBar("Tabs")) {
            // --- TAB 1: PLAYER ---
            if (ImGui::BeginTabItem("Player")) {
                ImGui::Spacing();
                ImGui::Checkbox("Damage Immunity", &g_bDamageImmunity);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Immunity to bullets, explosions, and all incoming damage!");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                uint32_t pGame = *(uint32_t*)0x002D45F0;
                if (pGame > 0x10000 && !IsBadReadPtr((void*)(uintptr_t)pGame, 0x20170)) {
                    uint32_t pips = *(uint32_t*)(uintptr_t)(pGame + 0x20150);
                    float segHp = *(float*)(uintptr_t)(pGame + 0x20158) * 100.0f;
                    uint32_t packs = *(uint32_t*)(uintptr_t)(pGame + 0x2014C);
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Player Status: Active");
                    ImGui::Text("Health Segments: %u / 4 (%.0f%%)", pips, segHp);
                    ImGui::Text("Health Packs: %u", packs);
                } else {
                    ImGui::TextDisabled("Player inactive (in menu or cutscene)");
                }

                ImGui::EndTabItem();
            }

            // --- TAB 2: WEAPONS ---
            if (ImGui::BeginTabItem("Weapons")) {
                ImGui::Spacing();
                ImGui::Checkbox("Infinite Ammo (No Reload)", &g_bInfAmmo);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Magazine ammo never depletes!");

                ImGui::Checkbox("Infinite Grenades", &g_bInfGrenades);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Throw unlimited grenades without consuming stock!");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "BFG (M249 SAW) Unlock Code:");
                ImGui::Selectable("5SQQ-5THA-ZFFV-7XEV");
                if (ImGui::IsItemClicked()) {
                    ImGui::SetClipboardText("5SQQ-5THA-ZFFV-7XEV");
                }
                ImGui::TextDisabled("(Click to copy code; enter as Profile Name to unlock)");

                ImGui::EndTabItem();
            }

            // --- TAB 3: VISUALS ---
            if (ImGui::BeginTabItem("Visuals")) {
                ImGui::Spacing();
                ImGui::Checkbox("Wireframe Mode", &g_bWireframe);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Render scene geometry in wireframe mode");

                ImGui::Spacing();
                ImGui::Checkbox("Custom Field of View (FOV)", &g_bCustomFOV);
                if (g_bCustomFOV) {
                    ImGui::SliderFloat("FOV Angle", &g_fCustomFOV, 60.0f, 130.0f, "%.0f deg");
                }

                ImGui::EndTabItem();
            }

            // --- TAB 4: HELP ---
            if (ImGui::BeginTabItem("Help")) {
                ImGui::Spacing();
                ImGui::BulletText("[F1]  : Toggle Mod Menu");
                ImGui::BulletText("[Esc] : Pause Game & Release Mouse");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Port: BLACK PC 0.3.0");
                ImGui::TextDisabled("Direct3D 9 ImGui Hook Active");

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// --- Hooked D3D9 Device Methods ---
static HRESULT WINAPI Hook_Present(IDirect3DDevice9* pDevice, const RECT* pSrc, const RECT* pDest, HWND hWndDest, const RGNDATA* pDirty) {
    static int s_frame = 0;
    if (++s_frame % 300 == 1) {
        Log("Hook_Present alive (frame %d)", s_frame);
    }

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

        // Crucial: reset texture transforms and stages so font glyph UVs are not scrambled
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
