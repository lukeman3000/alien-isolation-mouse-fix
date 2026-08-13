// ============================================================================
//  Alien: Isolation Mouse Fix  (mousefix.asi)
// ----------------------------------------------------------------------------
//  Fixes four separate input problems in AI.exe (32-bit):
//
//   1. DEADZONE FIX      - The engine compares each frame's mouse delta against
//                          a fixed epsilon; sub-threshold input gets applied and
//                          then clawed back by a "settle" routine that overwrites
//                          yaw/pitch with smoothed values. We NOP the two stores
//                          (16 bytes). Framerate-independent by construction.
//
//   2. SMOOTHING FIX     - The cinematic camera (rewire panels, lockers, vents)
//                          uses a target/current pair: mouse moves the target,
//                          an interpolator eases current toward it (the "floaty"
//                          lag). We hook the interpolator's store and force
//                          current = target every frame. Look clamps still apply.
//
//   3. SENS FIX          - The same cinematic camera runs ~12.4x (yaw) / ~3.9x
//                          (pitch) the FPS camera's measured response. We hook
//                          the cinematic input routine and scale each axis by a
//                          measured per-axis correction, multiplied by a live
//                          FOV-compensation factor (0% monitor-distance match
//                          between the cinematic camera's 80 deg vertical FOV
//                          and the player's configured FOV, read from memory).
//
//   4. CLAMP FIX         - The cinematic camera's look limits are cramped and
//                          appear tuned for a controller. Known stock limit
//                          sets are recognised by value and replaced with
//                          wider ones read from the INI. Also covers the
//                          maintenance jack, the door lever and other
//                          shared-limit interactions (comms button, vent
//                          entrance), and the mantle (climb-up) animation -
//                          all locked by default.
//
//  Additions (new behavior the stock game does not have; each can be
//  disabled in the INI):
//
//   5. CLICK TO INTERACT - In cinematic interaction views (security access
//                          tuner, rewire panels, computer terminals and the
//                          terminal hack minigame) left click sends the view's
//                          confirm key (E, or Enter in the minigame) and right
//                          click sends Q (back/exit). At terminals a short
//                          right-click TAP means back while HOLD keeps the
//                          game's native freelook.
//
//   6. SCROLL FIX        - Terminal logs scroll much further per mouse-wheel
//                          notch than the tiny stock step. Wheel notches are
//                          translated into PageDown/PageUp presses while a
//                          terminal is open; speed, backlog and pacing are all
//                          configurable.
//
//   7. MENU CLICK        - Left click advances the 'press any key' title
//                          screen (one harmless key tap); right click sends
//                          Escape (back) throughout the menus. Menu state comes
//                          from a module-level flag the game maintains, and the
//                          title screen is told apart from the main menu by a
//                          field on the screen object captured by two hooks.
//
//  All patch sites are located by AOB signature at runtime - no file edits.
//  If any signature is not found, that fix is skipped and logged; the game
//  runs stock for that subsystem. Never crashes on mismatch.
//
//  Settings are read from mousefix.ini beside this DLL; defaults below.
//
//  Build (x86 ONLY - the game is 32-bit):
//    Open the "x86 Native Tools Command Prompt for VS" and run:
//      cl /LD /O2 mousefix.cpp /link /OUT:mousefix.asi
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#pragma comment(lib, "user32.lib")   // SendInput / window APIs

// Bump this on every release: rebuild, re-zip, tag to match
static const char* MOD_VERSION = "2.0.0";

// Paths resolved from this DLL's own location, so the INI and log always sit
// beside the .asi regardless of the game's working directory
static char  g_iniPath[MAX_PATH] = {0};
static char  g_logPath[MAX_PATH] = {0};

// ----------------------------------------------------------------------------
// Measured calibration constants
// ----------------------------------------------------------------------------
static const float BASE_KY        = 0.080572f;   // yaw angular-parity factor
static const float BASE_KP        = 0.25465f;    // pitch angular-parity factor
static const float CINE_FOV_DEG   = 80.0f;       // cinematic cam vertical FOV
static const float FOV_REF        = 47.0f;       // game's reference FOV
                                                 // (fov ratio in memory = FOV/47)
static const int   FOV_POLL_MS    = 250;
static const int   CLAMP_POLL_MS  = 100;

// ----------------------------------------------------------------------------
// Configuration (mousefix.ini, read once at startup; defaults = calibrated fix)
// ----------------------------------------------------------------------------
static int   g_cfgDeadzoneFix   = 1;
static int   g_cfgSmoothingFix  = 1;
static int   g_cfgSensFix       = 1;
static float g_cfgCineSensMul   = 1.0f;
static float g_cfgCineYawMul    = 1.0f;
static float g_cfgCinePitchMul  = 1.0f;
static int   g_cfgFovComp       = 1;
static int   g_cfgClampFix      = 1;
static int   g_cfgClickToInteract = 1;
static int   g_cfgScrollFix     = 1;
static int   g_cfgMenuClick     = 1;
static float g_cfgScrollMult    = 2.5f;
static int   g_cfgScrollBuffer  = 25;
static int   g_cfgTapSpacingMs  = 6;    // gap between queued page-taps
static int   g_cfgLogging       = 1;

// Clamp profiles: a stock limit set identifies a view; the fixed set replaces
// it. All values in radians. Order: yawMin, yawMax, pitchMin, pitchMax.
struct ClampProfile
{
    const char* name;
    const char* section;      // INI section holding the fixed values
    float stock[4];
    float fixed[4];           // filled from the INI at startup
};

#define DEG(x) ((float)((x) * 3.14159265f / 180.0f))

static ClampProfile g_clampProfiles[] =
{
    { "locker", "Clamping.Locker",
      { DEG(-18), DEG(18), DEG(-25), DEG(10) },
      { DEG(-46), DEG(46), DEG(-25), DEG(32) } },

    { "security access tuner", "Clamping.SecurityAccessTuner",
      { DEG(-1), DEG(0), DEG(-1), DEG(1) },
      { DEG(-1), DEG(-1), DEG(1),  DEG(1) } },

    { "maintenance jack", "Clamping.MaintenanceJack",
      { DEG(-5),  DEG(5),  DEG(-5),  DEG(0)  },
      { 0.0f,     0.0f,    0.0f,     0.0f    } },

    { "door lever / comms / vent entrance", "Clamping.SharedInteractions",
      { DEG(-15), DEG(15), DEG(-15), DEG(15) },
      { 0.0f,     0.0f,    0.0f,     0.0f    } },

    { "mantle animation",   "Clamping.Mantle",
      { DEG(-10), DEG(10), DEG(-10), DEG(10) },
      { 0.0f,     0.0f,    0.0f,     0.0f    } },
};
static const int G_CLAMP_COUNT = sizeof(g_clampProfiles) / sizeof(g_clampProfiles[0]);

// Cine camera struct offsets for the look limits
static const int OFF_YAW_MIN   = 0x9C;
static const int OFF_YAW_MAX   = 0xA0;
static const int OFF_PITCH_MIN = 0x8C;
static const int OFF_PITCH_MAX = 0x90;

// Live multipliers consumed by the hooks (updated by the FOV thread)
// Initialised for FOV = 47 (M = tan(40)/tan(23.5) = 1.9299) so behavior is
// sane before the camera pointer is captured; config multipliers are folded
// in by the FOV thread on its first tick
static volatile float g_Ky = 0.155487f;
static volatile float g_Kp = 0.491422f;

// Captured by the cinematic input hook: cine camera struct base (clamp fix)
static void* volatile g_cineCam = NULL;

// Captured by the yaw-writer hook: player camera struct base
// FOV ratio (currentFOV / 47) lives at [g_camBase - 0x774]
static void* volatile g_camBase = NULL;

// Menu screen object, captured by both screen-state hooks (one fires on
// entering the title screen, one on leaving it, so the pointer is known
// after the first transition in either direction).
// [g_menuObj + 0x574] reads 0 on the 'press any key' title screen and
// 1 in the main menu. The object gets recycled, so validate the field is
// 0 or 1 before trusting it.
static volatile void* g_menuObj = NULL;

// Return addresses for the naked trampolines (filled in at patch time)
static void* g_smoothRet  = NULL;
static void* g_cineRet    = NULL;
static void* g_pitchRet   = NULL;
static void* g_yawcapRet  = NULL;
static void* g_menuRet    = NULL;   // entering the title screen
static void* g_menuSetRet = NULL;   // leaving it


// ----------------------------------------------------------------------------
// Logging
// ----------------------------------------------------------------------------
static bool g_logStarted = false;

static void Log(const char* fmt, ...)
{
    if (!g_cfgLogging || !g_logPath[0]) return;
    FILE* f = NULL;
    // First write of each run truncates; the rest append
    if (fopen_s(&f, g_logPath, g_logStarted ? "a" : "w") != 0 || !f) return;
    g_logStarted = true;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

// ----------------------------------------------------------------------------
// Resolve mousefix.ini / mousefix.log next to this DLL, then read settings
// ----------------------------------------------------------------------------
static float IniFloat(const char* section, const char* key, float def)
{
    char buf[64], defbuf[64];
    sprintf_s(defbuf, "%g", def);
    GetPrivateProfileStringA(section, key, defbuf, buf, sizeof(buf), g_iniPath);
    float v = (float)atof(buf);
    return (v == 0.0f && buf[0] != '0') ? def : v;   // unparsable -> default
}

// Written verbatim if mousefix.ini is missing, so a deleted or mangled file
// can always be recovered by deleting it and relaunching
static const char* DEFAULT_INI =
"; ============================================================================\n"
";  Alien: Isolation Mouse Fix - configuration\n"
";\n"
";  Every setting here is optional. If you run into problems, delete this\n"
";  file and it will be recreated with defaults on the next launch.\n"
";\n"
";  Edit with any text editor. Changes take effect on the next game launch.\n"
";  Check mousefix.log (same folder) to confirm what was applied.\n"
"; ============================================================================\n"
"\n"
"; ============================================================================\n"
";  FIXES - repairs to the game's own mouse handling\n"
"; ============================================================================\n"
"\n"
"[Deadzone]\n"
"\n"
"; Low-speed deadzone fix (normal gameplay)\n"
"; Removes the logic that dampens slow mouse movement\n"
"; 1 = fixed (default), 0 = stock game behavior\n"
"DeadzoneFix = 1\n"
"\n"
"\n"
"[Smoothing]\n"
"\n"
"; Cinematic-camera smoothing fix (rewire panels, lockers, etc.)\n"
"; Makes the camera track the mouse directly instead of drifting after it\n"
"; 1 = fixed (default), 0 = stock game behavior\n"
"SmoothingFix = 1\n"
"\n"
"\n"
"[Sensitivity]\n"
"\n"
"; Cinematic-camera sensitivity fix (rewire panels, lockers, etc.)\n"
"; Matches sensitivity in those views to normal gameplay\n"
"; 1 = fixed (default), 0 = stock game behavior\n"
"SensFix = 1\n"
"\n"
"; FOV compensation for the cinematic camera\n"
"; Those views render at a wider FOV than normal gameplay, so identical\n"
"; rotation covers less of the screen. With this on, the fix compensates\n"
"; so both views FEEL the same, recalculated live from the in-game FOV\n"
"; setting\n"
"; 1 = compensate (default, recommended)\n"
"; 0 = match raw rotation per mouse count instead; the cinematic camera\n"
";     will feel slower than normal gameplay\n"
"FovCompensation = 1\n"
"\n"
"; Extra multiplier on cinematic-camera sensitivity, applied on top of the\n"
"; calibrated correction. 1.0 = matched to normal gameplay (default)\n"
"; Raise for faster panel/locker aiming, lower for slower\n"
"; Only used when SensFix = 1\n"
"CineSensMultiplier = 1.0\n"
"\n"
"; Per-axis trim for the cinematic camera (rewire panels, lockers, etc.),\n"
"; multiplied on top of CineSensMultiplier\n"
"; Leave at 1.0 unless the yaw/pitch balance feels off on an unusual\n"
"; aspect ratio or display setup\n"
"CineYawMultiplier = 1.0\n"
"CinePitchMultiplier = 1.0\n"
"\n"
"\n"
"; ============================================================================\n"
";  ADDITIONS - behavior the stock game does not have\n"
"; ============================================================================\n"
"\n"
"[ClickToInteract]\n"
"\n"
"; In cinematic interaction views (security access tuner, rewire panels,\n"
"; computer terminals and the terminal hack minigame) LEFT CLICK sends\n"
"; that view's confirm key (E, or Enter in the minigame) and RIGHT CLICK\n"
"; sends Q (back / exit).\n"
"; At computer terminals a short right-click TAP means back, while HOLD\n"
"; keeps the game's normal freelook.\n"
"; 1 = on (default), 0 = off\n"
"Enabled = 1\n"
"\n"
"\n"
"[ScrollFix]\n"
"\n"
"; Mouse-wheel notches scroll terminal logs much further than the\n"
"; stock wheel step (by sending PageDown / PageUp presses).\n"
"; Only active while a computer terminal is open.\n"
"; 1 = on (default), 0 = off\n"
"Enabled = 1\n"
"\n"
"; Scroll speed multiplier: page-taps sent per wheel notch\n"
"; 1.0 = one page per notch. 2.0 = two pages per notch. Default 2.5\n"
"; Fractions work too: 1.5 alternates one and two pages per notch\n"
"SpeedMultiplier = 2.5\n"
"\n"
"; How many extra pages can wait in line when you scroll faster than\n"
"; pages can be sent. The scroll that starts a payout always counts in\n"
"; full; this only limits the backlog behind it.\n"
"; The unit is PAGES, not wheel notches - with a 2.5 multiplier, a\n"
"; buffer of 25 is ten notches' worth.\n"
";  0 = no buffer: extra scrolling during a payout is ignored\n"
"; 25 = default: far more than anyone rolls in one burst, so nothing\n"
";      is lost in practice, while still bounding the backlog\n"
"; -1 = unlimited: every notch is honored no matter what\n"
"; Scrolling the other way always cancels whatever is still queued\n"
"ScrollBuffer = 25\n"
"\n"
"; Milliseconds between queued page-taps. Lower = faster payout of big\n"
"; multipliers/buffers, but more pages become irreversible when you\n"
"; reverse direction mid-scroll (the game glides through pages already\n"
"; sent). Higher = every reversal feels instant, slower total payout.\n"
"TapSpacing = 6\n"
"\n"
"\n"
"[MenuClick]\n"
"\n"
"; LEFT CLICK advances the title screen (the 'press any key' screen,\n"
"; where the mouse natively does nothing), and RIGHT CLICK sends Escape\n"
"; (back) throughout the menus. Left click is left alone inside the\n"
"; menus, which already support the mouse natively.\n"
"; 1 = on (default), 0 = off\n"
"Enabled = 1\n"
"\n"
"\n"
"; ============================================================================\n"
";  CLAMP FIX - per-view look limits\n"
"; ============================================================================\n"
"\n"
"[Clamping]\n"
"\n"
"; The cinematic camera limits how far you can look in each direction.\n"
"; These limits are inconsistent and appear to be tuned for a controller.\n"
"; This adjusts them per view - widening the cramped ones, and locking\n"
"; those where a small amount of freedom feels unnatural.\n"
"; 1 = widened (default), 0 = stock game behavior\n"
"ClampFix = 1\n"
"\n"
"\n"
"[Clamping.Locker]\n"
"\n"
"; Look limits in degrees from center\n"
"; Negative yaw is left, negative pitch is down\n"
"; Stock game values: -18 / 18 / -25 / 10 (set ClampFix = 0 to use those)\n"
"YawMin = -46\n"
"YawMax = 46\n"
"PitchMin = -25\n"
"PitchMax = 32\n"
"\n"
"\n"
"[Clamping.SecurityAccessTuner]\n"
"\n"
"; Look limits in degrees from center\n"
"; Negative yaw is left, negative pitch is down\n"
"; Stock game values: -1 / 0 / -1 / 1 (set ClampFix = 0 to use those)\n"
"; Setting min equal to max locks that axis, which is what the defaults do\n"
"YawMin = -1\n"
"YawMax = -1\n"
"PitchMin = 1\n"
"PitchMax = 1\n"
"\n"
"\n"
"[Clamping.MaintenanceJack]\n"
"\n"
"; Look limits in degrees from center\n"
"; Negative yaw is left, negative pitch is down\n"
"; Stock game values: -5 / 5 / -5 / 0 (set ClampFix = 0 to use those)\n"
"; Defaults lock the view fully - the stock freedom is too small to be\n"
"; useful\n"
"YawMin = 0\n"
"YawMax = 0\n"
"PitchMin = 0\n"
"PitchMax = 0\n"
"\n"
"\n"
"[Clamping.SharedInteractions]\n"
"\n"
"; Door levers, small button presses (e.g. comms link) and the vent\n"
"; entrance animation all share one set of stock limits\n"
"; Stock game values: -15 / 15 / -15 / 15 (set ClampFix = 0 to use those)\n"
"; Defaults lock the view fully - the stock freedom is too small to be\n"
"; useful\n"
"YawMin = 0\n"
"YawMax = 0\n"
"PitchMin = 0\n"
"PitchMax = 0\n"
"\n"
"\n"
"[Clamping.Mantle]\n"
"\n"
"; The brief mantle (climb-up) animation\n"
"; Stock game values: -10 / 10 / -10 / 10 (set ClampFix = 0 to use those)\n"
"; Defaults lock the view fully - the stock freedom is too small to be\n"
"; useful\n"
"YawMin = 0\n"
"YawMax = 0\n"
"PitchMin = 0\n"
"PitchMax = 0\n"
"\n"
"\n"
"[Advanced]\n"
"\n"
"; Write a log of what was patched to mousefix.log\n"
"; 1 = on (default). Useful for troubleshooting; harmless to leave on\n"
"Logging = 1\n";

static bool g_iniWasCreated = false;

static void LoadConfig(HMODULE self)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(self, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = '\0';

    sprintf_s(g_iniPath, "%smousefix.ini", dir);
    sprintf_s(g_logPath, "%smousefix.log", dir);

    // Recreate the INI with documented defaults if it is missing
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES)
    {
        FILE* f = NULL;
        if (fopen_s(&f, g_iniPath, "w") == 0 && f)
        {
            fputs(DEFAULT_INI, f);
            fclose(f);
            g_iniWasCreated = true;
        }
    }

    g_cfgDeadzoneFix  = GetPrivateProfileIntA("Deadzone",    "DeadzoneFix",     1, g_iniPath);
    g_cfgSmoothingFix = GetPrivateProfileIntA("Smoothing",   "SmoothingFix",    1, g_iniPath);
    g_cfgSensFix      = GetPrivateProfileIntA("Sensitivity", "SensFix",         1, g_iniPath);
    g_cfgFovComp      = GetPrivateProfileIntA("Sensitivity", "FovCompensation", 1, g_iniPath);
    g_cfgClampFix     = GetPrivateProfileIntA("Clamping",    "ClampFix",        1, g_iniPath);
    g_cfgClickToInteract = GetPrivateProfileIntA("ClickToInteract", "Enabled",  1, g_iniPath);
    g_cfgScrollFix    = GetPrivateProfileIntA("ScrollFix",   "Enabled",         1, g_iniPath);
    g_cfgScrollMult   = IniFloat("ScrollFix", "SpeedMultiplier", 2.5f);
    if (g_cfgScrollMult < 0.1f || g_cfgScrollMult > 10.0f) g_cfgScrollMult = 2.5f;
    g_cfgScrollBuffer = GetPrivateProfileIntA("ScrollFix", "ScrollBuffer", 25, g_iniPath);
    if (g_cfgScrollBuffer < -1) g_cfgScrollBuffer = 25;     // -1 = unlimited
    if (g_cfgScrollBuffer > 1000) g_cfgScrollBuffer = 1000;
    g_cfgTapSpacingMs = GetPrivateProfileIntA("ScrollFix", "TapSpacing", 6, g_iniPath);
    g_cfgMenuClick    = GetPrivateProfileIntA("MenuClick",   "Enabled",         1, g_iniPath);
    if (g_cfgTapSpacingMs < 0)    g_cfgTapSpacingMs = 6;
    if (g_cfgTapSpacingMs > 1000) g_cfgTapSpacingMs = 1000;
    g_cfgLogging      = GetPrivateProfileIntA("Advanced",    "Logging",         1, g_iniPath);

    g_cfgCineSensMul  = IniFloat("Sensitivity", "CineSensMultiplier",  1.0f);
    g_cfgCineYawMul   = IniFloat("Sensitivity", "CineYawMultiplier",   1.0f);
    g_cfgCinePitchMul = IniFloat("Sensitivity", "CinePitchMultiplier", 1.0f);

    // Clamp profiles: read the fixed limits (degrees in the INI, radians here)
    for (int i = 0; i < G_CLAMP_COUNT; ++i)
    {
        ClampProfile& p = g_clampProfiles[i];
        const char* keys[4] = { "YawMin", "YawMax", "PitchMin", "PitchMax" };
        for (int k = 0; k < 4; ++k)
        {
            float deg = p.fixed[k] * 180.0f / 3.14159265f;   // current default
            float v   = IniFloat(p.section, keys[k], deg);
            if (v < -180.0f || v > 180.0f) v = deg;          // guard typos
            p.fixed[k] = DEG(v);
        }
    }

    // Guard against typos that would make the game unplayable
    if (g_cfgCineSensMul  <= 0.0f || g_cfgCineSensMul  > 100.0f) g_cfgCineSensMul  = 1.0f;
    if (g_cfgCineYawMul   <= 0.0f || g_cfgCineYawMul   > 100.0f) g_cfgCineYawMul   = 1.0f;
    if (g_cfgCinePitchMul <= 0.0f || g_cfgCinePitchMul > 100.0f) g_cfgCinePitchMul = 1.0f;
}

// ----------------------------------------------------------------------------
// AOB scan over the main module
// ----------------------------------------------------------------------------
static BYTE* FindPattern(const BYTE* pat, size_t len)
{
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base) return NULL;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage;

    for (size_t i = 0; i + len <= size; ++i)
    {
        if (memcmp(base + i, pat, len) == 0)
            return base + i;
    }
    return NULL;
}

static void WriteBytes(void* dst, const void* src, size_t len)
{
    DWORD old;
    VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(dst, src, len);
    VirtualProtect(dst, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
}

// Write E9 rel32 jump at 'site' to 'target', pad remainder of stolen bytes
// with NOPs
static void WriteJump(BYTE* site, void* target, size_t stolen)
{
    BYTE buf[16];
    buf[0] = 0xE9;
    *(INT32*)(buf + 1) = (INT32)((BYTE*)target - (site + 5));
    for (size_t i = 5; i < stolen; ++i) buf[i] = 0x90;
    WriteBytes(site, buf, stolen);
}

// ----------------------------------------------------------------------------
// Trampolines (x86, __declspec(naked))
// ----------------------------------------------------------------------------

// SMOOTHING: hooked at "fstp dword ptr [esi+E8]" (6 bytes stolen)
// Re-execute the fstp (keeps x87 stack balanced), then current = target
// for both yaw and pitch, then return to original flow
static __declspec(naked) void SmoothTramp()
{
    __asm {
        fstp dword ptr [esi+0E8h]
        movss xmm0, dword ptr [esi+0F0h]
        movss dword ptr [esi+0E8h], xmm0     // current yaw  = target yaw
        movss xmm0, dword ptr [esi+0ECh]
        movss dword ptr [esi+0E4h], xmm0     // current pitch = target pitch
        jmp   dword ptr [g_smoothRet]
    }
}

// SENS (yaw) + cine camera capture: hooked at "movss xmm0,[ecx+AC]"
// (8 bytes stolen). Stores ecx (the cine camera) for the clamp fix,
// re-executes the load, scales by g_Ky, returns
// g_Ky is 1.0 when the sens fix is off, so the multiply is a no-op then
static __declspec(naked) void CineTramp()
{
    __asm {
        mov   dword ptr [g_cineCam], ecx
        movss xmm0, dword ptr [ecx+0ACh]
        mulss xmm0, dword ptr [g_Ky]
        jmp   dword ptr [g_cineRet]
    }
}

// SCREEN STATE capture (entering the title screen): hooked at
// "mov [esi+574],0" (10 bytes stolen). esi = screen object.
static __declspec(naked) void MenuTramp()
{
    __asm {
        mov   dword ptr [g_menuObj], esi
        mov   dword ptr [esi+574h], 0
        jmp   dword ptr [g_menuRet]
    }
}

// SCREEN STATE capture (leaving the title screen): hooked at
// "mov [esi+574],eax" (6 bytes stolen). Same object as above.
static __declspec(naked) void MenuSetTramp()
{
    __asm {
        mov   dword ptr [g_menuObj], esi
        mov   dword ptr [esi+574h], eax
        jmp   dword ptr [g_menuSetRet]
    }
}

// SENS (pitch): hooked at "mulss xmm1,[eax+20]" (5 bytes stolen)
static __declspec(naked) void PitchTramp()
{
    __asm {
        mulss xmm1, dword ptr [eax+20h]
        mulss xmm1, dword ptr [g_Kp]
        jmp   dword ptr [g_pitchRet]
    }
}

// CAMERA CAPTURE: hooked at "addss xmm1,[esi+204]" in the player camera's
// fast-path yaw writer (8 bytes stolen). Stores the struct base so the FOV
// thread can read the fov ratio at base-0x774
static __declspec(naked) void YawCapTramp()
{
    __asm {
        mov   dword ptr [g_camBase], esi
        addss xmm1, dword ptr [esi+204h]
        jmp   dword ptr [g_yawcapRet]
    }
}

// ----------------------------------------------------------------------------
// FOV compensation thread
//   M = tan(cineFOV/2) / tan(playerFOV/2), applied to both base factors
// ----------------------------------------------------------------------------
static DWORD WINAPI FovThread(LPVOID)
{
    const float tanCine = tanf(CINE_FOV_DEG * 0.5f * 3.14159265f / 180.0f);

    for (;;)
    {
        Sleep(FOV_POLL_MS);

        BYTE* cam = (BYTE*)g_camBase;
        if (!cam) continue;

        float ratio = 0.0f;
        __try {
            ratio = *(float*)(cam - 0x774);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;                       // stale pointer: skip this tick
        }

        // Sanity gate - on garbage, keep last known-good values
        if (ratio < 0.5f || ratio > 3.0f) continue;

        float fovDeg = ratio * FOV_REF;
        float M = tanCine / tanf(fovDeg * 0.5f * 3.14159265f / 180.0f);

        g_Ky = BASE_KY * M * g_cfgCineSensMul * g_cfgCineYawMul;
        g_Kp = BASE_KP * M * g_cfgCineSensMul * g_cfgCinePitchMul;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Clamp fix thread
//   Watches the cine camera's look limits. When they match a known stock set,
//   they are replaced with the configured values. Because the match is against
//   the STOCK values, this re-applies every time the game stamps them back
//   (on entering a locker, etc.) and never compounds.
// ----------------------------------------------------------------------------
static bool NearRad(float a, float b) { return fabsf(a - b) < 0.0001f; }

static DWORD WINAPI ClampThread(LPVOID)
{
    int lastLogged = -1;

    for (;;)
    {
        Sleep(CLAMP_POLL_MS);

        BYTE* cam = (BYTE*)g_cineCam;
        if (!cam) continue;

        float cur[4];
        __try {
            cur[0] = *(float*)(cam + OFF_YAW_MIN);
            cur[1] = *(float*)(cam + OFF_YAW_MAX);
            cur[2] = *(float*)(cam + OFF_PITCH_MIN);
            cur[3] = *(float*)(cam + OFF_PITCH_MAX);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;                       // stale pointer: skip this tick
        }

        for (int i = 0; i < G_CLAMP_COUNT; ++i)
        {
            const ClampProfile& p = g_clampProfiles[i];
            if (NearRad(cur[0], p.stock[0]) && NearRad(cur[1], p.stock[1]) &&
                NearRad(cur[2], p.stock[2]) && NearRad(cur[3], p.stock[3]))
            {
                __try {
                    *(float*)(cam + OFF_YAW_MIN)   = p.fixed[0];
                    *(float*)(cam + OFF_YAW_MAX)   = p.fixed[1];
                    *(float*)(cam + OFF_PITCH_MIN) = p.fixed[2];
                    *(float*)(cam + OFF_PITCH_MAX) = p.fixed[3];
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
                if (lastLogged != i)
                {
                    Log("clamp profile applied: %s", p.name);
                    lastLogged = i;
                }
                break;
            }
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Additions: click-to-interact, terminal scroll fix, menu click
// ----------------------------------------------------------------------------

// --- key sender (SendInput, VK + scan code; the game reads scan codes) ------
static void SendKeyTap(WORD vk, WORD scan, bool extended)
{
    INPUT in[2];
    ZeroMemory(in, sizeof(in));
    DWORD flags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    in[0].type           = INPUT_KEYBOARD;
    in[0].ki.wVk         = vk;
    in[0].ki.wScan       = scan;
    in[0].ki.dwFlags     = flags;
    in[1] = in[0];
    in[1].ki.dwFlags     = flags | KEYEVENTF_KEYUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(25);
    SendInput(1, &in[1], sizeof(INPUT));
}

// --- shared view tests ------------------------------------------------------
// Two cine-camera fields used to identify and qualify a view:
//
//   +0xAC  fov. Reads 0 at a terminal and 1.3963 in every other cinematic
//          view, so terminal identity is two-factor: all-zero clamps AND
//          fov ~ 0.
//   +0x78  seconds since the last interaction in the current view. Resets
//          to ~0.402 on each interaction and counts up while the view is
//          open, so it changes continuously whenever a view is live and
//          stops once the view closes. Used as a liveness test - has it
//          moved recently, not what does it read.
static const int OFF_CINE_FOV = 0xAC;
static const int OFF_CINE_ACT = 0x78;

static bool ReadClamps(BYTE* cam, float* out4, float* fov)
{
    __try {
        out4[0] = *(float*)(cam + OFF_YAW_MIN);
        out4[1] = *(float*)(cam + OFF_YAW_MAX);
        out4[2] = *(float*)(cam + OFF_PITCH_MIN);
        out4[3] = *(float*)(cam + OFF_PITCH_MAX);
        if (fov) *fov = *(float*)(cam + OFF_CINE_FOV);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SigMatch(const float* cur, const float* sig)
{
    return NearRad(cur[0], sig[0]) && NearRad(cur[1], sig[1]) &&
           NearRad(cur[2], sig[2]) && NearRad(cur[3], sig[3]);
}

// Used by the scroll fix. Signature (zero clamps + zero fov) AND liveness:
// clamp values persist after a view closes, so without the liveness test a
// terminal you have walked away from still looks open, and mouse wheel
// scrolling would fire page keys during ordinary gameplay.
// Keeps its OWN liveness state - this runs on the window thread while
// CineViewActive() runs on the interact thread, and they must not share.
static bool IsTerminalOpen()
{
    BYTE* cam = (BYTE*)g_cineCam;
    if (!cam) return false;

    static int   sLastAct    = 0;
    static bool  sActInit    = false;
    static DWORD sLastChange = 0;
    int act = 0;
    __try { act = *(int*)(cam + OFF_CINE_ACT); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    DWORD now = GetTickCount();
    if (!sActInit) { sLastAct = act; sActInit = true; sLastChange = 0; }
    else if (act != sLastAct) { sLastAct = act; sLastChange = now; }
    if (!sLastChange || (now - sLastChange) >= 300) return false;

    float c[4], fov;
    if (!ReadClamps(cam, c, &fov)) return false;
    return NearRad(c[0], 0) && NearRad(c[1], 0) &&
           NearRad(c[2], 0) && NearRad(c[3], 0) && NearRad(fov, 0);
}

// --- click-to-interact ------------------------------------------------------
// Views matched BY NAME on their clamp signatures; rows can be reordered
// freely. lmb: 'e' = send E, 'n' = send Enter. qmode: 'p' = Q on press,
// 't' = Q on release if held < TAP_MS (terminal: hold = native freelook).
static const int   TAP_MS          = 500;   // higher = lazier taps count as "back",
                                            //  but an aborted freelook may fire Q
static const int   DISGUISE_MS     = 400;
static const int   LIVENESS_MS     = 150;
static const int   INTERACT_POLL   = 15;

static const float SIG_TUNER_STOCK[4]  = { DEG(-1),    DEG(0),    DEG(-1),    DEG(1)    };
static const float SIG_PANEL[4]        = { DEG(-57.5f),DEG(57.5f),DEG(-32.5f),DEG(32.5f)};
static const float SIG_MINIGAME[4]     = { DEG(-80),   DEG(80),   DEG(-40),   DEG(40)   };
static const float SIG_TERMINAL[4]     = { 0, 0, 0, 0 };

struct InteractView
{
    const char*  name;
    const float* sig;         // 4 clamp values (radians)
    char         lmb;         // 'e' or 'n' (Enter)
    char         qmode;       // 'p' press | 't' tap
    bool         needFovZero; // terminal second factor
};

// NOTE: "tuner (locked)" points at the tuner clamp profile's FIXED values,
// which are INI-configurable - resolved at thread start so a user-tuned
// lock is still recognised.
static float g_tunerLocked[4];

static InteractView g_views[] =
{
    { "tuner (stock)",          SIG_TUNER_STOCK, 'e', 'p', false },
    { "tuner (locked)",         g_tunerLocked,   'e', 'p', false },
    { "rewire panel",           SIG_PANEL,       'e', 'p', false },
    { "terminal hack minigame", SIG_MINIGAME,    'n', 'p', false },
    { "terminal",               SIG_TERMINAL,    'e', 't', true  },
};
static const int G_VIEW_COUNT = sizeof(g_views) / sizeof(g_views[0]);

static bool GameHasFocus()
{
    // The foreground window must belong to THIS process.
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// --- menu detection (feeds menu click) --------------------------------------
// Two module-relative flags, both verified by census:
//   [AI.exe+OFF_INGAME] : 1 during gameplay, 0 in the main menu AND the
//                         in-game pause menu (a gameplay/menu timer pair
//                         drives it - whichever context is inactive freezes)
//   [g_menuObj+0x68]    : 1 on the 'press any key' title screen, 0 past
//                         it (captured by the two menu hooks)
// No inference, no state machine.
static const size_t OFF_INGAME = 0x15A3158;
static bool  g_inMenu = false;

static bool AtTitleScreen()
{
    if (!g_menuObj) return false;
    int t = -1;
    __try { t = *(int*)((BYTE*)g_menuObj + 0x574); }
    __except (EXCEPTION_EXECUTE_HANDLER) { t = -1; }
    // The field is a 0/1 flag. Anything else means the object we captured
    // has been recycled for something else - do not trust it.
    if (t != 0 && t != 1) return false;
    return t == 0;                    // 0 = title screen, 1 = main menu
}

// Entering menu state is debounced: the flag must read 0 continuously for
// MENU_SETTLE_MS before we believe it. Transient dips (e.g. while a
// cinematic view spins up) would otherwise turn a right-click meant for Q
// into an Escape. Leaving menu state is immediate - a stale "in menu"
// belief is the harmful direction. The title screen path does not consult
// this flag at all, so title -> main menu -> right-click stays instant.
static const DWORD MENU_SETTLE_MS = 250;

// A recognised cinematic view outranks the menu flag: if the cine camera
// is showing a tuner / panel / terminal / minigame, we are in gameplay no
// matter what a transient flag dip says. This keeps a right-click meant
// for Q from ever becoming an Escape.
static bool CineViewActive()
{
    BYTE* cam = (BYTE*)g_cineCam;
    if (!cam) return false;

    // Clamp values PERSIST after a view ends - the camera keeps the last
    // view's limits while you are back in gameplay or even in the menus.
    // So a signature match alone is not enough: [cam+78] must also still
    // be moving, which it does continuously while a view is open.
    static int   lastAct = 0;
    static bool  actInit = false;
    static DWORD lastChange = 0;
    int act = 0;
    __try { act = *(int*)(cam + OFF_CINE_ACT); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    DWORD now = GetTickCount();
    if (!actInit) { lastAct = act; actInit = true; lastChange = 0; }
    else if (act != lastAct) { lastAct = act; lastChange = now; }
    if (!lastChange || (now - lastChange) >= 300) return false;

    float cur[4], fov;
    if (!ReadClamps(cam, cur, &fov)) return false;
    for (int i = 0; i < G_VIEW_COUNT; ++i)
    {
        if (SigMatch(cur, g_views[i].sig))
        {
            if (g_views[i].needFovZero && !NearRad(fov, 0)) continue;
            return true;
        }
    }
    return false;
}

// Menu state comes from the module flag alone: census shows it reads 0 in
// the main menu, the pause menu and the title screen, and 1 in gameplay and
// in cinematic views (terminal, tuner, panels). The screen object is not
// consulted here - it only tells the title screen apart from the other
// menus, for the G tap.
static void UpdateMenuState()
{
    static DWORD zeroSince = 0;

    // A recognised cinematic view is gameplay, whatever the flag says.
    if (CineViewActive()) { g_inMenu = false; zeroSince = 0; return; }

    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    if (!base) { g_inMenu = false; zeroSince = 0; return; }

    int v = 1;
    __try { v = *(int*)(base + OFF_INGAME); }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 1; }

    if (v != 0)                       // gameplay: believe it immediately
    {
        g_inMenu  = false;
        zeroSince = 0;
        return;
    }

    if (CineViewActive())             // cine view outranks the flag
    {
        g_inMenu  = false;
        zeroSince = 0;
        return;
    }

    DWORD now = GetTickCount();       // menu: only after it settles
    if (!zeroSince) zeroSince = now;
    if (now - zeroSince >= MENU_SETTLE_MS) g_inMenu = true;
}

static DWORD WINAPI InteractThread(LPVOID)
{
    // resolve the locked-tuner signature from the (possibly INI-tuned)
    // tuner profile
    for (int i = 0; i < G_CLAMP_COUNT; ++i)
        if (strcmp(g_clampProfiles[i].name, "security access tuner") == 0)
            for (int k = 0; k < 4; ++k)
                g_tunerLocked[k] = g_clampProfiles[i].fixed[k];

    bool  prevL = true, prevR = true;      // true: swallow a held button at start
    DWORD rDownTime = 0;  bool rTimerArmed = false;
    int   lastAct = 0;    bool actInit = false;
    DWORD lastActTime = 0, lastZeros = 0;

    for (;;)
    {
        Sleep(INTERACT_POLL);

        BYTE* cam = (BYTE*)g_cineCam;
        // NOTE: cam may be NULL at the boot title screen; the menu-click
        // branch below must still run, so the NULL check happens after it

        if (!GameHasFocus())               // reset edges so a click made
        {                                  // elsewhere can't fire on refocus
            prevL = prevR = true;
            rTimerArmed = false;
            continue;
        }

        DWORD now = GetTickCount();


        // menu click: at the title screen / main menu, LMB = Enter and
        // RMB = Escape; cine logic below never runs while in the menu
        if (g_cfgMenuClick)
        {
            UpdateMenuState();
            if (g_inMenu)
            {
                // LMB sends 'G' on the "press any key" screen only -
                // any key advances it, and an unbound letter is inert
                // everywhere else, including the key-binding listener.
                // Before either screen-state hook has fired nothing has
                // been captured, and the only menu reachable in that
                // window is the boot title screen, so assume it.
                // Fire on RELEASE, not press: if the menu appears while
                // the button is still physically held, the game ignores
                // the next click - it never saw that button go down on
                // this screen. Releasing first matches the keyboard case.
                bool atTitle = g_menuObj ? AtTitleScreen() : true;
                bool Lm = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                if (!Lm && prevL && atTitle) SendKeyTap('G', 0x22, false);
                prevL = Lm;

                // RMB sends Escape (back) anywhere in the menus
                bool Rm = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
                if (Rm && !prevR) SendKeyTap(VK_ESCAPE, 0x01, false);
                prevR = Rm;
                rTimerArmed = false;
                continue;
            }
        }

        if (!cam) continue;               // no cine struct yet (and not menu)

        // liveness: [cam+78] must have CHANGED recently (first sample is
        // baseline only - nil-to-value is not an edge)
        int act = 0;
        __try { act = *(int*)(cam + OFF_CINE_ACT); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!actInit) { lastAct = act; actInit = true; lastActTime = 0; }
        else if (act != lastAct) { lastAct = act; lastActTime = now; }
        bool live = lastActTime && (now - lastActTime) < LIVENESS_MS;

        float cur[4], fov;
        if (!ReadClamps(cam, cur, &fov)) continue;

        // view lookup by signature (+ terminal second factor)
        const InteractView* view = NULL;
        for (int i = 0; i < G_VIEW_COUNT; ++i)
        {
            if (SigMatch(cur, g_views[i].sig))
            {
                if (g_views[i].needFovZero && !NearRad(fov, 0)) continue;
                view = &g_views[i];
                break;
            }
        }

        char lmb = view ? view->lmb : 0;
        char qmode = view ? view->qmode : 0;

        if (view && strcmp(view->name, "terminal") == 0) lastZeros = now;

        // disguise guard: a "rewire panel" seen within DISGUISE_MS of
        // terminal-zeros is terminal RMB-freelook wearing the panel's exact
        // clamp values - keep the terminal's tap behavior
        if (view && strcmp(view->name, "rewire panel") == 0 &&
            lastZeros && (now - lastZeros) < DISGUISE_MS)
        {
            qmode = 't';
        }

        // LMB edge: send the view's confirm key
        bool L = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (L && !prevL && live && lmb)
        {
            if (lmb == 'e')      SendKeyTap('E',  0x12, false);
            else if (lmb == 'n') SendKeyTap(VK_RETURN, 0x1C, false);
        }
        prevL = L;

        // RMB: behavior decided by the effective view at press time
        bool R = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        if (R && !prevR)
        {
            if (live && qmode == 'p')      SendKeyTap('Q', 0x10, false);
            else if (live && qmode == 't') { rDownTime = now; rTimerArmed = true; }
            else                           rTimerArmed = false;
        }
        else if (!R && prevR)
        {
            if (rTimerArmed && (now - rDownTime) < (DWORD)TAP_MS)
                SendKeyTap('Q', 0x10, false);   // short press at terminal = back
            rTimerArmed = false;
        }
        prevR = R;
    }
    return 0;
}

// --- terminal scroll fix (WndProc subclass, WM_MOUSEWHEEL -> page keys) -----
static WNDPROC g_origWndProc = NULL;

// Pending page-taps, written by the WndProc (window thread - must never
// block) and consumed by ScrollWorkThread. Positive = PageUp notches,
// negative = PageDown notches.
static volatile LONG g_pendingNotches = 0;

static LRESULT CALLBACK ScrollWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEWHEEL && g_cfgScrollFix && IsTerminalOpen())
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        InterlockedExchangeAdd(&g_pendingNotches, delta / WHEEL_DELTA);
        // fall through immediately: never block the window thread
    }
    return CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}

static DWORD WINAPI ScrollWorkThread(LPVOID)
{
    // One dial (INI: ScrollBuffer) - how many pages beyond the notch
    // currently being paid may wait in line:
    //    0  = none: extra notches arriving mid-payout are dropped
    //    N  = up to N pages of backlog; excess is dropped
    //   -1  = unlimited: every notch is honored, whatever the trail
    // The notch that TRIGGERS a payout is always paid in full (multiplier
    // included) - the buffer bounds backlog only, never notch value.
    // Debt is signed, so reversing direction cancels queued pages.
    float tapDebt = 0.0f;
    for (;;)
    {
        // focus guard: taps are SendInput and land wherever focus is, so a
        // payout must never continue after an alt-tab; stale scroll intent
        // is dumped, not paused
        if (!GameHasFocus())
        {
            tapDebt = 0.0f;
            InterlockedExchange(&g_pendingNotches, 0);
            Sleep(50);
            continue;
        }

        // Any context change dumps pending pages: moving between logs with
        // W/S (or the arrow keys), or leaving the terminal altogether.
        // Otherwise a queued payout keeps scrolling whatever comes next.
        if ((GetAsyncKeyState('W')    & 0x8000) ||
            (GetAsyncKeyState('S')    & 0x8000) ||
            (GetAsyncKeyState(VK_UP)  & 0x8000) ||
            (GetAsyncKeyState(VK_DOWN)& 0x8000) ||
            !IsTerminalOpen())
        {
            tapDebt = 0.0f;
            InterlockedExchange(&g_pendingNotches, 0);
            Sleep(10);
            continue;
        }

        LONG n = InterlockedExchange(&g_pendingNotches, 0);
        if (n != 0)
        {
            // reversal = hard cancel: an opposite-direction notch wipes all
            // queued debt before banking, so direction changes take effect
            // instantly (no residue, no absorbed notches)
            if ((n > 0 && tapDebt < 0.0f) || (n < 0 && tapDebt > 0.0f))
                tapDebt = 0.0f;
            tapDebt += (float)n * g_cfgScrollMult;
            if (g_cfgScrollBuffer >= 0)
            {
                // cap: current notch's worth (up to the multiplier) plus
                // ScrollBuffer pages of backlog
                float limit = g_cfgScrollMult + (float)g_cfgScrollBuffer;
                if (limit < 1.0f) limit = 1.0f;
                if (tapDebt >  limit) tapDebt =  limit;
                if (tapDebt < -limit) tapDebt = -limit;
            }
        }

        if (tapDebt >= 1.0f)
        {
            SendKeyTap(VK_PRIOR, 0x49, true);        // PageUp  (E0-extended)
            tapDebt -= 1.0f;
            Sleep(g_cfgTapSpacingMs); // pace payout: keeps queued debt
        }                            // cancellable and lets the game's
        else if (tapDebt <= -1.0f)   // scroll glide keep up per page
        {
            SendKeyTap(VK_NEXT, 0x51, true);         // PageDown (E0-extended)
            tapDebt += 1.0f;
            Sleep(g_cfgTapSpacingMs);
        }
        else
        {
            Sleep(5);                                // idle: poll for notches
        }
    }
    return 0;
}

static BOOL CALLBACK FindGameWindowCb(HWND h, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    char title[8];
    if (GetWindowTextA(h, title, sizeof(title)) <= 0) return TRUE;
    *(HWND*)out = h;
    return FALSE;
}

static DWORD WINAPI ScrollHookThread(LPVOID)
{
    // the game window may not exist yet at DLL attach; wait for it
    HWND hwnd = NULL;
    for (int tries = 0; tries < 120 && !hwnd; ++tries)
    {
        EnumWindows(FindGameWindowCb, (LPARAM)&hwnd);
        if (!hwnd) Sleep(500);
    }
    if (!hwnd) { Log("scroll fix: game window not found - skipped"); return 0; }

    g_origWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                                               (LONG_PTR)ScrollWndProc);
    if (g_origWndProc)
    {
        HANDLE t = CreateThread(NULL, 0, ScrollWorkThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        Log("scroll fix: wheel hook installed (hwnd %p)", hwnd);
    }
    else Log("scroll fix: SetWindowLongPtr failed - skipped");
    return 0;
}

// ----------------------------------------------------------------------------
// Signatures
// ----------------------------------------------------------------------------
static const BYTE SIG_DEADZONE[] = { 0xF3,0x0F,0x11,0x8E,0x04,0x02,0x00,0x00,
                                     0xF3,0x0F,0x11,0x86,0x08,0x02,0x00,0x00 };
static const BYTE SIG_SMOOTH[]   = { 0xD9,0x9E,0xE8,0x00,0x00,0x00,
                                     0xF3,0x0F,0x10,0x86,0xE4,0x00,0x00,0x00 };
static const BYTE SIG_CINE[]     = { 0xF3,0x0F,0x10,0x81,0xAC,0x00,0x00,0x00,
                                     0xF3,0x0F,0x10,0x89,0xB0,0x00,0x00,0x00 };
static const BYTE SIG_PITCH[]    = { 0xF3,0x0F,0x59,0x48,0x20,
                                     0xF3,0x0F,0x59,0x05 };
static const BYTE SIG_MENUSET[]  = { 0x89,0x86,0x74,0x05,0x00,0x00,
                                     0x88,0x86,0x70,0x05,0x00,0x00,
                                     0x8B,0xCE,0x5E };
// title-screen handler tail: mov byte [esi+68],1 / pop esi / ret / int3
static const BYTE SIG_MENUTITLE[]= { 0x80,0x7E,0x28,0x00,0x74,0x18,
                                     0x8B,0xCE,
                                     0xC7,0x86,0x74,0x05,0x00,0x00,
                                     0x00,0x00,0x00,0x00,
                                     0xC6,0x86,0x70,0x05,0x00,0x00,0x01 };
static const size_t OFF_MENUTITLE_HOOK = 8;   // skip the lead-in bytes
static const BYTE SIG_YAWCAP[]   = { 0xF3,0x0F,0x58,0x8E,0x04,0x02,0x00,0x00,
                                     0xF3,0x0F,0x11,0x8E,0x04,0x02,0x00,0x00 };

// ----------------------------------------------------------------------------
// Init
// ----------------------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID)
{
    time_t now = time(NULL);
    char ts[64] = {0};
    ctime_s(ts, sizeof(ts), &now);
    if (ts[0]) { size_t n = strlen(ts); if (n && ts[n-1] == '\n') ts[n-1] = '\0'; }
    Log("--- Alien: Isolation Mouse Fix v%s --- %s", MOD_VERSION, ts);
    Log(g_iniWasCreated ? "mousefix.ini not found - created with defaults"
                        : "config loaded from mousefix.ini");
    Log("config: DeadzoneFix=%d SmoothingFix=%d SensFix=%d FovCompensation=%d ClampFix=%d "
        "ClickToInteract=%d ScrollFix=%d MenuClick=%d",
        g_cfgDeadzoneFix, g_cfgSmoothingFix, g_cfgSensFix, g_cfgFovComp, g_cfgClampFix,
        g_cfgClickToInteract, g_cfgScrollFix, g_cfgMenuClick);
    Log("config: CineSensMultiplier=%g Yaw=%g Pitch=%g",
        g_cfgCineSensMul, g_cfgCineYawMul, g_cfgCinePitchMul);
    Log("config: SpeedMultiplier=%g ScrollBuffer=%d TapSpacing=%d",
        g_cfgScrollMult, g_cfgScrollBuffer, g_cfgTapSpacingMs);

    // 1) Deadzone: NOP both settle stores (16 bytes)
    if (!g_cfgDeadzoneFix) Log("deadzone fix disabled by config");
    else if (BYTE* p = FindPattern(SIG_DEADZONE, sizeof(SIG_DEADZONE)))
    {
        BYTE nops[16]; memset(nops, 0x90, 16);
        WriteBytes(p, nops, 16);
        Log("deadzone fix applied @ %p", p);
    }
    else Log("deadzone signature NOT FOUND - fix skipped");

    // 2) Smoothing: hook the interpolator's yaw store (steal 6 bytes)
    if (!g_cfgSmoothingFix) Log("smoothing fix disabled by config");
    else if (BYTE* p = FindPattern(SIG_SMOOTH, sizeof(SIG_SMOOTH)))
    {
        g_smoothRet = p + 6;
        WriteJump(p, SmoothTramp, 6);
        Log("smoothing fix applied @ %p", p);
    }
    else Log("smoothing signature NOT FOUND - fix skipped");

    // 3a) Sens yaw + cine camera capture: hook the cinematic input entry
    //     (steal 8 bytes). The clamp fix needs the same hook for the camera
    //     pointer, so install it when either fix is on; g_Ky = 1.0 makes the
    //     multiply a no-op when the sens fix itself is off.
    if (!g_cfgSensFix) { g_Ky = 1.0f; g_Kp = 1.0f; Log("sens fix disabled by config"); }
    if (!g_cfgSensFix && !g_cfgClampFix) { /* hook not needed */ }
    else if (BYTE* p = FindPattern(SIG_CINE, sizeof(SIG_CINE)))
    {
        g_cineRet = p + 8;
        WriteJump(p, CineTramp, 8);
        Log(g_cfgSensFix ? "sens fix (yaw) applied @ %p"
                         : "cine camera capture applied @ %p (for clamp fix)", p);
    }
    else Log("cine signature NOT FOUND - sens yaw and clamp fix skipped");

    // 3b) Sens pitch: hook the pitch multiply (steal 5 bytes exactly)
    if (!g_cfgSensFix) { /* logged above */ }
    else if (BYTE* p = FindPattern(SIG_PITCH, sizeof(SIG_PITCH)))
    {
        g_pitchRet = p + 5;
        WriteJump(p, PitchTramp, 5);
        Log("sens fix (pitch) applied @ %p", p);
    }
    else Log("pitch signature NOT FOUND - sens pitch skipped");

    // 3c) Player camera capture: feeds the FOV thread (sens fix only).
    //     With FovCompensation = 0 the correction is a constant, so no hook
    //     or thread is needed at all.
    if (!g_cfgSensFix) { /* not needed */ }
    else if (!g_cfgFovComp)
    {
        g_Ky = BASE_KY * g_cfgCineSensMul * g_cfgCineYawMul;
        g_Kp = BASE_KP * g_cfgCineSensMul * g_cfgCinePitchMul;
        Log("FOV compensation disabled by config - angular parity only");
    }
    else if (BYTE* p = FindPattern(SIG_YAWCAP, sizeof(SIG_YAWCAP)))
    {
        g_yawcapRet = p + 8;
        WriteJump(p, YawCapTramp, 8);
        Log("camera capture applied @ %p", p);
        HANDLE t = CreateThread(NULL, 0, FovThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        Log("FOV auto-compensation thread started");
    }
    else Log("yaw-capture signature NOT FOUND - FOV auto-scaling disabled "
             "(sens fix will use FOV=47 defaults)");

    // 4) Clamp fix: needs the cine camera pointer from the hook in 3a
    if (!g_cfgClampFix) Log("clamp fix disabled by config");
    else if (g_cineRet)
    {
        HANDLE t = CreateThread(NULL, 0, ClampThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        Log("clamp fix active (%d profiles)", G_CLAMP_COUNT);
    }
    else Log("clamp fix skipped - cine camera hook not installed");

    // 4b) Menu screen capture (addition): powers menu click's title-screen
    //     test. Optional - menu click falls back to the boot-only rule if
    //     the signature is missing.
    if (!g_cfgMenuClick) Log("menu click disabled by config");
    else if (BYTE* p = FindPattern(SIG_MENUTITLE, sizeof(SIG_MENUTITLE)))
    {
        p += OFF_MENUTITLE_HOOK;          // hook the store itself
        g_menuRet = p + 10;
        WriteJump(p, MenuTramp, 10);
        Log("screen state capture (title screen) applied @ %p", p);
    }
    else Log("screen state (title screen) signature NOT FOUND");

    if (g_cfgMenuClick)
    {
        if (BYTE* p = FindPattern(SIG_MENUSET, sizeof(SIG_MENUSET)))
        {
            g_menuSetRet = p + 6;
            WriteJump(p, MenuSetTramp, 6);
            Log("screen state capture (main menu) applied @ %p", p);
        }
        else Log("screen state (main menu) signature NOT FOUND - menu click "
                 "limited to the boot title screen");
    }

    // 5) Click-to-interact (addition): needs the cine camera pointer
    if (!g_cfgClickToInteract) Log("click-to-interact disabled by config");
    else if (g_cineRet)
    {
        HANDLE t = CreateThread(NULL, 0, InteractThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        Log("click-to-interact active (%d views), menu click %s",
            G_VIEW_COUNT, g_cfgMenuClick ? "on" : "off");
    }
    else Log("click-to-interact skipped - cine camera hook not installed");

    // 6) Terminal scroll fix (addition): needs the cine camera pointer for
    //    the terminal gate; wheel hook installs once the game window exists
    if (!g_cfgScrollFix) Log("scroll fix disabled by config");
    else if (g_cineRet)
    {
        HANDLE t = CreateThread(NULL, 0, ScrollHookThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    else Log("scroll fix skipped - cine camera hook not installed");

    Log("--- mousefix init done ---");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);
        LoadConfig(h);
        HANDLE t = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
