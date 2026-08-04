// ============================================================================
//  Alien: Isolation Mouse Fix  (mousefix.asi)
// ----------------------------------------------------------------------------
//  Fixes four separate input problems in AI.exe (32-bit):
//
//   1. DEADZONE FIX    - The engine compares each frame's mouse delta against
//                        a fixed epsilon; sub-threshold input gets applied and
//                        then clawed back by a "settle" routine that overwrites
//                        yaw/pitch with smoothed values. We NOP the two stores
//                        (16 bytes). Framerate-independent by construction.
//
//   2. SMOOTHING FIX   - The cinematic camera (rewire panels, lockers, vents)
//                        uses a target/current pair: mouse moves the target,
//                        an interpolator eases current toward it (the "floaty"
//                        lag). We hook the interpolator's store and force
//                        current = target every frame. Look clamps still apply.
//
//   3. SENS FIX        - The same cinematic camera runs ~12.4x (yaw) / ~3.9x
//                        (pitch) the FPS camera's measured response. We hook
//                        the cinematic input routine and scale each axis by a
//                        measured per-axis correction, multiplied by a live
//                        FOV-compensation factor (0% monitor-distance match
//                        between the cinematic camera's 80 deg vertical FOV
//                        and the player's configured FOV, read from memory).
//
//   4. CLAMP FIX       - The cinematic camera's look limits are cramped and
//                        appear tuned for a controller. Known stock limit
//                        sets are recognised by value and replaced with
//                        wider ones read from the INI.
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

// Bump this on every release: rebuild, re-zip, tag to match
static const char* MOD_VERSION = "1.0.0";

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
};
static const int G_CLAMP_COUNT = sizeof(g_clampProfiles) / sizeof(g_clampProfiles[0]);

// Cine camera struct offsets for the look limits
static const int OFF_YAW_MIN   = 0x9C;
static const int OFF_YAW_MAX   = 0xA0;
static const int OFF_PITCH_MIN = 0x8C;
static const int OFF_PITCH_MAX = 0x90;

// Paths resolved from this DLL's own location, so the INI and log always sit
// beside the .asi regardless of the game's working directory
static char  g_iniPath[MAX_PATH] = {0};
static char  g_logPath[MAX_PATH] = {0};

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

// Return addresses for the naked trampolines (filled in at patch time)
static void* g_smoothRet = NULL;
static void* g_cineRet   = NULL;
static void* g_pitchRet  = NULL;
static void* g_yawcapRet = NULL;

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
"[Clamping]\n"
"\n"
"; The cinematic camera limits how far you can look in each direction.\n"
"; Those limits appear to be tuned for a controller and feel cramped with a\n"
"; mouse. This adjusts them in the views listed below.\n"
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
// Trampolines (x86, __declspec(naked)) - mirror the verified CE hooks exactly
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
// FOV compensation thread (replaces the CE table's Lua script)
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
// Signatures (identical to the verified CE table)
// ----------------------------------------------------------------------------
static const BYTE SIG_DEADZONE[] = { 0xF3,0x0F,0x11,0x8E,0x04,0x02,0x00,0x00,
                                     0xF3,0x0F,0x11,0x86,0x08,0x02,0x00,0x00 };
static const BYTE SIG_SMOOTH[]   = { 0xD9,0x9E,0xE8,0x00,0x00,0x00,
                                     0xF3,0x0F,0x10,0x86,0xE4,0x00,0x00,0x00 };
static const BYTE SIG_CINE[]     = { 0xF3,0x0F,0x10,0x81,0xAC,0x00,0x00,0x00,
                                     0xF3,0x0F,0x10,0x89,0xB0,0x00,0x00,0x00 };
static const BYTE SIG_PITCH[]    = { 0xF3,0x0F,0x59,0x48,0x20,
                                     0xF3,0x0F,0x59,0x05 };
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
    Log("config: DeadzoneFix=%d SmoothingFix=%d SensFix=%d FovCompensation=%d ClampFix=%d",
        g_cfgDeadzoneFix, g_cfgSmoothingFix, g_cfgSensFix, g_cfgFovComp, g_cfgClampFix);
    Log("config: CineSensMultiplier=%g Yaw=%g Pitch=%g",
        g_cfgCineSensMul, g_cfgCineYawMul, g_cfgCinePitchMul);

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
