// ============================================================================
// Megaton Hammer playtest hook for Project64.
//
// This is the editor's "bundled PJ64 fork" integration. When Project64 is
// launched on a Megaton-Hammer-injected OoT Master Quest **debug** ROM, the
// editor drops a params file next to the ROM describing the scene/room the
// user wants to play, the desired Link age, and the inventory mode. This hook:
//
//   1. Detects the OoT gc-eu-mq-dbg ROM (the only OoT ROM with a map-select /
//      debug save, and the one the editor injects into).
//   2. Reads the params file once.
//   3. Every video frame, observes the live SaveContext so we can LOG the
//      current scene/entrance/age -- this is the headless verification channel
//      (mirrors the SoH/2Ship boot logs): a test run can confirm the game
//      booted and warped to the requested scene with no crash.
//   4. Drives the warp: forces the debug ROM into its map-select and selects
//      the injected entrance, applying age + (debug) inventory.
//
// All RAM access goes through g_MMU (endianness-correct virtual addressing).
// Addresses are for OoT gc-eu-mq-dbg (verified against OoTMM's link_oot.in and
// the OoT decomp `save.h` field offsets):
//     gSaveContext            = 0x8011A5D0
//       .save.entranceIndex   = +0x0000 (s32)
//       .save.linkAge         = +0x0004 (s32; 0=adult, 1=child)
//       .gameMode             = +0x135C (s32; 0=normal, 1=title, 2=map-select)
//       .sceneLayer           = +0x1360 (s32)
//       .respawnFlag          = +0x1364 (s32)
// ============================================================================
#include "stdafx.h"
#include <Project64-core/N64System/SystemGlobals.h>
#include <Project64-core/N64System/Mips/MemoryVirtualMem.h>
#include <Project64-core/N64System/Mips/Register.h>
#include <Project64-core/N64System/N64Rom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

// ---- gc-eu-mq-dbg addresses & offsets --------------------------------------
// gSaveContext for the gc-eu-mq-dbg DEBUG ROM is 0x8015E660 (confirmed: OotDebugAutoBoot's baked detour
// writes gSaveContext.entranceIndex to 0x8015E660 and the game boots from that entrance). The old value
// 0x8011A5D0 is the NTSC-1.0 RETAIL address — wrong for the debug ROM the editor injects into, so every
// SaveContext poke (linkAge / dayTime / inventory) was hitting unrelated RAM. The field offsets below are
// within the version-stable Save substruct (entranceIndex@0x00), so only the base needed correcting.
static const uint32_t kSaveContext = 0x8015E660;
static const uint32_t kOff_entranceIndex = 0x0000;
static const uint32_t kOff_linkAge = 0x0004;
static const uint32_t kOff_dayTime = 0x000C;   // u16 dayTime (high half of the word at +0xC; "zelda_time")
static const uint32_t kOff_nightFlag = 0x0010;  // s32 nightFlag
static const uint32_t kOff_gameMode = 0x135C;
static const uint32_t kOff_sceneLayer = 0x1360;
static const uint32_t kOff_respawnFlag = 0x1364;

enum { GAMEMODE_NORMAL = 0, GAMEMODE_TITLE_SCREEN = 1, GAMEMODE_FILE_SELECT = 2, GAMEMODE_MAP_SELECT = 3 };

// ---- In-game warp: locate the active PlayState and poke a scene transition ---------------------
// OoT has no global PlayState pointer, but the active GameState (= start of PlayState) stores the
// Play gamestate functions, so we find it by scanning RDRAM for a word equal to Play_Init at the
// GameState.init offset, then validating the neighbouring main/destroy/gfxCtx pointers. With `play`
// in hand we set nextEntranceIndex + transitionTrigger exactly as the game's own area exits do.
static const uint32_t kPlay_Init = 0x8009A750;   // gc-eu-mq-dbg
static const uint32_t kGS_gfxCtx = 0x00;         // GameState.gfxCtx
static const uint32_t kGS_main = 0x04;           // GameState.main
static const uint32_t kGS_destroy = 0x08;        // GameState.destroy
static const uint32_t kGS_init = 0x0C;           // GameState.init  (NOT == Play_Init during gameplay — cleared)
static const uint32_t kGS_size = 0x10;           // GameState.size  (PlayState is ~0x12518 — far bigger than title/file-select)
static const uint32_t kPlay_sceneId = 0x000A4;   // PlayState.sceneId (s16)
static const uint32_t kPlay_transitionTrigger = 0x11E15; // PlayState.transitionTrigger (s8)
static const uint32_t kPlay_nextEntranceIndex = 0x11E1A; // PlayState.nextEntranceIndex (s16)
static const uint8_t TRANS_TRIGGER_START = 20;

// ---- MM (Majora's Mask) EU debug build addresses & offsets ---------------------------------------
// From OoTMM link_mm.in (gSaveContext / Play_Init — the OoT values in link_oot.in match this hook's
// gc-eu-mq-dbg constants, confirming the builds) and mm-main/include z64save.h / z64.h field offsets.
// MM has no child/adult age; the warp sets PlayState.nextEntrance + transitionTrigger like OoT.
static const uint32_t kMM_SaveContext = 0x801EF670;
static const uint32_t kMM_Off_gameMode = 0x3CA8;          // SaveContext.gameMode (s32) "mode"
static const uint32_t kMM_Play_Init = 0x8016A2C8;
static const uint32_t kMM_Play_sceneId = 0x000A4;         // PlayState.sceneId (s16)
static const uint32_t kMM_Play_transitionTrigger = 0x18875; // PlayState.transitionTrigger (s8)
static const uint32_t kMM_Play_nextEntrance = 0x1887A;    // PlayState.nextEntrance (u16)

enum MhGame { GAME_NONE, GAME_OOT, GAME_MM };
static MhGame sGame = GAME_NONE;

static const uint32_t kRamBase = 0x80000000;
static const uint32_t kPlayStateSize = 0x18900;  // enough to cover the fields we touch (MM nextEntrance @0x1887A)

static uint32_t sPlayAddr = 0;     // discovered PlayState vaddr (0 = not yet found)
static uint32_t sScanCursor = kRamBase;
static bool sWarpDone = false;
static int sWarpedScene = -1;

// ---- N64 controller buttons (BUTTONS.Value bitfield, PJ64 layout) ----------
// PJ64 stores controller state as a 32-bit value; high half is the digital
// buttons. These match the N64 controller bit layout used by the input plugin.
static const uint32_t BTN_A = 0x80000000;
static const uint32_t BTN_B = 0x40000000;
static const uint32_t BTN_Z = 0x20000000;
static const uint32_t BTN_START = 0x10000000;
static const uint32_t BTN_L = 0x00000020;
static const uint32_t BTN_R = 0x00000010;

struct MhParams
{
    bool valid = false;
    int32_t entranceIndex = -1; // target entrance (scene<<... | spawn), debug-rom encoding
    int32_t linkAge = 0;        // 0 adult, 1 child
    int32_t inventory = 0;      // 0 = debug inventory (map-select default)
    int32_t timeOfDay = 0x8000; // normalized u16 time-of-day (0x8000 = noon), shared with the OTR engines
    int32_t debugControls = 0;  // 1 = enable the debug ROM's L+R+Z map-select + L+D-pad no-clip
    bool loaded = false;
};

static MhParams sParams;
static FILE * sLog = nullptr;
static bool sInit = false;
static bool sIsOotDebug = false;
static int32_t sLastEntrance = -999;
static int32_t sLastGameMode = -999;
static uint64_t sFrame = 0;

static std::string MhTempPath(const char * leaf)
{
    const char * tmp = getenv("TEMP");
    if (tmp == nullptr || tmp[0] == '\0')
    {
        tmp = getenv("TMP");
    }
    if (tmp == nullptr || tmp[0] == '\0')
    {
        tmp = "C:\\Windows\\Temp";
    }
    std::string p = tmp;
    p += "\\MegatonHammer\\";
    p += leaf;
    return p;
}

static void MhLogOpen()
{
    if (sLog != nullptr)
    {
        return;
    }
    std::string p = MhTempPath("mh_n64_playtest.log");
    sLog = fopen(p.c_str(), "w");
}

static void MhLog(const char * fmt, ...)
{
    MhLogOpen();
    if (sLog == nullptr)
    {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(sLog, fmt, ap);
    va_end(ap);
    fputc('\n', sLog);
    fflush(sLog);
}

static void MhLoadParams()
{
    sParams.loaded = true;
    std::string p = MhTempPath("mh_n64_playtest.txt");
    FILE * f = fopen(p.c_str(), "r");
    if (f == nullptr)
    {
        MhLog("[mh] no params file at %s -- observe/verify only", p.c_str());
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        char key[64];
        long val;
        if (sscanf(line, "%63[^=]=%ld", key, &val) == 2)
        {
            std::string k = key;
            if (k == "entrance")
                sParams.entranceIndex = (int32_t)val;
            else if (k == "age")
                sParams.linkAge = (int32_t)val;
            else if (k == "inventory")
                sParams.inventory = (int32_t)val;
            else if (k == "timeOfDay")
                sParams.timeOfDay = (int32_t)val;
            else if (k == "debug")
                sParams.debugControls = (int32_t)val;
        }
    }
    fclose(f);
    sParams.valid = (sParams.entranceIndex >= 0);
    MhLog("[mh] params: entrance=0x%04X age=%d inventory=%d timeOfDay=0x%04X debug=%d (valid=%d)", sParams.entranceIndex,
          sParams.linkAge, sParams.inventory, sParams.timeOfDay & 0xFFFF, sParams.debugControls, sParams.valid ? 1 : 0);
}

static bool MhDetectOotDebug()
{
    if (g_Rom == nullptr || g_MMU == nullptr)
    {
        return false;
    }
    // The OoT debug ROM's internal name is "THE LEGEND OF ZELDA" like retail, so
    // identity by name alone is ambiguous. The decisive test: gc-eu-mq-dbg places
    // gSaveContext at 0x8011A5D0 and the debug ROM is the only OoT build with the
    // map-select gameMode. We gate on the ROM being OoT (name) and trust the
    // injected-ROM contract (the editor only injects into the MQ debug ROM).
    std::string name = g_Rom->GetRomName();
    for (char & c : name)
    {
        c = (char)toupper((unsigned char)c);
    }
    // gc-eu-mq-dbg's internal name is "THE LEGEND OF DEBUG"; retail/other OoT builds are
    // "THE LEGEND OF ZELDA". Accept any of these — the addresses below are gc-eu-mq-dbg, so the
    // editor's contract is to inject into the MQ debug ROM (whose internal name carries "DEBUG").
    // Majora's Mask debug ROM → MM path; otherwise an OoT/Ocarina/debug name → OoT path.
    if (name.find("MAJORA") != std::string::npos)
    {
        sGame = GAME_MM;
        MhLog("[mh] ROM name=\"%s\" -> MM", g_Rom->GetRomName().c_str());
        return true;
    }
    bool isZelda = name.find("ZELDA") != std::string::npos || name.find("OCARINA") != std::string::npos ||
                   name.find("DEBUG") != std::string::npos;
    if (isZelda) sGame = GAME_OOT;
    MhLog("[mh] ROM name=\"%s\" -> %s", g_Rom->GetRomName().c_str(), isZelda ? "OoT" : "unknown");
    return isZelda;
}

// Scan up to `chunk` candidate bases of RDRAM for the active PlayState (GameState.init == Play_Init,
// with valid neighbouring pointers). Returns its vaddr or 0; advances sScanCursor across frames.
static uint32_t MhScanForPlayState(uint32_t chunk, uint32_t playInit)
{
    // Match the PLAY gamestate by its distinctive SIZE (~0x12518), NOT init == Play_Init: GameState.init is
    // cleared during gameplay (see kGS_init note), so the old init match never found the live PlayState under
    // auto-boot → MhDoWarp never fired → the requested age + custom inventory silently never applied (the same
    // failure fixed for MM). Size + valid gfx/main/destroy pointers uniquely identify the PlayState.
    (void)playInit;
    uint32_t size = g_MMU->RdramSize();
    if (size == 0 || size > 0x800000) size = 0x800000;
    uint32_t end = kRamBase + size;
    const uint32_t codeLo = 0x80000400, codeHi = 0x80400000;
    for (uint32_t n = 0; n < chunk && sScanCursor + kPlayStateSize < end; sScanCursor += 4, n++)
    {
        uint32_t base = sScanCursor, mainv = 0, destroyv = 0, gfx = 0, szv = 0, sidWord = 0;
        g_MMU->MemoryValue32(base + kGS_gfxCtx, gfx);
        g_MMU->MemoryValue32(base + kGS_main, mainv);
        g_MMU->MemoryValue32(base + kGS_destroy, destroyv);
        if (gfx < kRamBase || gfx >= end) continue;
        if (mainv < codeLo || mainv >= codeHi) continue;
        if (destroyv < codeLo || destroyv >= codeHi) continue;
        if (mainv == destroyv) continue;
        if (!g_MMU->MemoryValue32(base + kGS_size, szv) || szv < 0x11000 || szv > 0x14000) continue;
        if (!g_MMU->MemoryValue32(base + (kPlay_sceneId & ~3u), sidWord)) continue;
        if ((uint16_t)(sidWord >> 16) >= 0x80) continue;   // sceneId sanity
        sScanCursor += 4;
        return base;
    }
    if (sScanCursor + kPlayStateSize >= end) sScanCursor = kRamBase; // wrap and keep looking
    return 0;
}

// DIAGNOSTIC: find the loaded dmadata table in RAM (starts with the makerom entry {0,0x1060,0,0} followed
// by {0x1060,...}) and report whether it covers targetVrom — mirrors DmaMgr_FindDmaEntry. If an appended
// scene's VROM is NOT covered here, the scene DMA faults (hang/black), even if the entry was written to ROM.
static void MhCheckMmDma(uint32_t targetVrom)
{
    uint32_t size = g_MMU->RdramSize();
    if (size == 0 || size > 0x800000) size = 0x800000;
    uint32_t end = kRamBase + size;
    for (uint32_t a = kRamBase; a + 0x20 < end; a += 4)
    {
        uint32_t v0 = 0, v1 = 0, v2 = 0, v3 = 0, w0 = 0;
        g_MMU->MemoryValue32(a, v0); g_MMU->MemoryValue32(a + 4, v1);
        g_MMU->MemoryValue32(a + 8, v2); g_MMU->MemoryValue32(a + 12, v3);
        if (v0 != 0 || v1 != 0x1060 || v2 != 0 || v3 != 0) continue;   // makerom entry {0,0x1060,0,0}
        g_MMU->MemoryValue32(a + 16, w0);
        if (w0 != 0x1060) continue;                                    // 2nd entry vromStart == 0x1060
        int count = 0; bool covered = false; uint32_t lastVe = 0;
        for (uint32_t p = a; p + 16 <= end && count < 4000; p += 16, count++)
        {
            uint32_t vs = 0, ve = 0;
            g_MMU->MemoryValue32(p, vs); g_MMU->MemoryValue32(p + 4, ve);
            if (ve == 0 && vs == 0) break;                             // terminator
            lastVe = ve;
            if (targetVrom >= vs && targetVrom < ve) covered = true;
        }
        MhLog("[mh] MM dmadata @0x%08X entries=%d lastVromEnd=0x%08X covers 0x%08X = %s",
              a, count, lastVe, targetVrom, covered ? "YES" : "NO");
        return;
    }
    MhLog("[mh] MM dmadata table not found in RAM");
}

// Heuristic MM PlayState scan (we don't have a verified Play_Init for the EU debug build): find a GameState
// whose three function pointers (main/destroy/init at +4/+8/+C) are DISTINCT code pointers and whose
// sceneId (+0xA4) is a plausible scene id. Logs the found init value so the real Play_Init can be learned.
static uint32_t MhScanForPlayStateMM(uint32_t chunk)
{
    uint32_t size = g_MMU->RdramSize();
    if (size == 0 || size > 0x800000) size = 0x800000;
    uint32_t end = kRamBase + size;
    const uint32_t codeLo = 0x80000400, codeHi = 0x80400000;
    for (uint32_t n = 0; n < chunk && sScanCursor + kPlayStateSize < end; sScanCursor += 4, n++)
    {
        uint32_t base = sScanCursor, gfx = 0, mainv = 0, destroyv = 0, initv = 0, sidWord = 0;
        g_MMU->MemoryValue32(base + kGS_gfxCtx, gfx);
        g_MMU->MemoryValue32(base + kGS_main, mainv);
        g_MMU->MemoryValue32(base + kGS_destroy, destroyv);
        g_MMU->MemoryValue32(base + kGS_init, initv);
        if (gfx < kRamBase || gfx >= end) continue;
        if (mainv < codeLo || mainv >= codeHi) continue;
        if (destroyv < codeLo || destroyv >= codeHi) continue;
        if (mainv == destroyv) continue;
        // Match the PLAY gamestate by its distinctive SIZE (~0x12518) — far larger than title/file-select.
        // (init is NOT Play_Init during gameplay; matching on it found nothing even for a working scene.)
        uint32_t szv = 0;
        if (!g_MMU->MemoryValue32(base + kGS_size, szv)) continue;
        if (szv < 0x11000 || szv > 0x14000) continue;
        if (!g_MMU->MemoryValue32(base + (kMM_Play_sceneId & ~3u), sidWord)) continue;
        uint16_t sid = (uint16_t)(sidWord >> 16);
        if (sid >= 0x80) continue;
        (void)initv;
        MhLog("[mh] MM PlayState candidate @0x%08X size=0x%X main=0x%08X sceneId=0x%X", base, szv, mainv, sid);
        sScanCursor += 4;
        return base;
    }
    if (sScanCursor + kPlayStateSize >= end) sScanCursor = kRamBase;
    return 0;
}

// ---- Custom/empty inventory: apply the editor's computed SaveContext pokes -------------------
// The editor (Rom/N64SavePokes.cs) computes a flat list of (offset, size, value) writes that reproduce
// the OTR MhApplyCustomInventory loadout, verified field-for-field against SoH. We apply it blindly here
// AFTER the debug save ran (in MhDoWarp, same timing as the linkAge poke), so the playtest's inventory
// matches the SoH/2Ship pipeline. File: %TEMP%\MegatonHammer\mh_n64_save.txt (absent => debug inventory).
struct MhPoke { uint32_t off; int size; uint32_t val; };
static std::vector<MhPoke> sSavePokes;
static bool sSavePokesLoaded = false;

static void MhLoadSavePokes()
{
    sSavePokesLoaded = true;
    sSavePokes.clear();
    std::string p = MhTempPath("mh_n64_save.txt");
    FILE * f = fopen(p.c_str(), "r");
    if (f == nullptr) { MhLog("[mh] no save-poke file -> debug inventory"); return; }
    char line[160];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        unsigned off = 0, val = 0; int size = 0;
        if (sscanf(line, "0x%x %d 0x%x", &off, &size, &val) == 3)
            sSavePokes.push_back({ (uint32_t)off, size, (uint32_t)val });
    }
    fclose(f);
    MhLog("[mh] loaded %u save pokes (custom/empty inventory)", (unsigned)sSavePokes.size());
}

// RMW writers (only 32-bit MMU accessors exist; N64 is big-endian so the byte at the lower address is MSB).
static void MhPoke8(uint32_t base, uint32_t off, uint32_t v)
{
    uint32_t a = base + (off & ~3u), w = 0;
    if (!g_MMU->MemoryValue32(a, w)) return;
    int shift = (3 - (int)(off & 3)) * 8;
    w = (w & ~(0xFFu << shift)) | ((v & 0xFFu) << shift);
    g_MMU->UpdateMemoryValue32(a, w);
}
static void MhPoke16(uint32_t base, uint32_t off, uint32_t v)
{
    uint32_t a = base + (off & ~3u), w = 0;
    if (!g_MMU->MemoryValue32(a, w)) return;
    if ((off & 2u) == 0) w = (w & 0x0000FFFFu) | ((v & 0xFFFFu) << 16);   // high half
    else                 w = (w & 0xFFFF0000u) | (v & 0xFFFFu);            // low half
    g_MMU->UpdateMemoryValue32(a, w);
}
static void MhApplySavePokes(uint32_t saveBase)
{
    if (!sSavePokesLoaded) MhLoadSavePokes();
    if (sSavePokes.empty()) return;
    for (const MhPoke & p : sSavePokes)
    {
        if (p.size == 4)      g_MMU->UpdateMemoryValue32(saveBase + p.off, p.val);
        else if (p.size == 2) MhPoke16(saveBase, p.off, p.val);
        else                  MhPoke8(saveBase, p.off, p.val);
    }
    MhLog("[mh] applied %u inventory save pokes at 0x%08X", (unsigned)sSavePokes.size(), saveBase);
}

// ---- Debug controls (off by default) ---------------------------------------------------------
// Addresses verified by binary analysis of the gc-eu-mq-dbg / MM US-retail ROMs the editor injects:
//   OoT gIsCtrlr2Valid 0x8012DBC0, gDebugCamEnabled 0x8012D394;  MM MapSelect_Init 0x80801B4C.
// GameState (at PlayState+0) layout (both games): input[0].cur.button = u16 @ +0x14 (high half of the
// word); init ptr @ +0x0C; running u8 @ +0x9B. Actor.world.pos f32 x/y/z @ player+0x24/0x28/0x2C.
// Player ptr reachable from PlayState @ +0x1C44 (OoT) / +0x1CCC (MM); bgCheckFlags u16 @ +0x88 / +0x90.
static const uint32_t kOoT_gIsCtrlr2Valid = 0x8012DBC0;
static const uint32_t kMM_MapSelect_Init  = 0x80801B4C;

// N64 button bits (OSContPad.button). MH_-prefixed to avoid clashing with PJ64's own BTN_* macros.
static const uint16_t MH_BTN_L = 0x0020, MH_BTN_R = 0x0010, MH_BTN_Z = 0x2000;
static const uint16_t MH_BTN_DU = 0x0800, MH_BTN_DD = 0x0400, MH_BTN_DL = 0x0200, MH_BTN_DR = 0x0100;

static void MhAddFloat(uint32_t addr, float d)
{
    uint32_t w = 0;
    if (!g_MMU->MemoryValue32(addr, w)) return;
    float f; memcpy(&f, &w, 4); f += d; memcpy(&w, &f, 4);
    g_MMU->UpdateMemoryValue32(addr, w);
}

// Per-frame debug-control handling for a live PlayState. mm picks MM vs OoT addresses.
static void MhDebugControls(uint32_t playAddr, bool mm)
{
    uint32_t w = 0;
    if (!g_MMU->MemoryValue32(playAddr + 0x14, w)) return;   // input[0].cur.button (high half)
    uint16_t btn = (uint16_t)(w >> 16);

    // L+R+Z -> map select. OoT handles this itself once gIsCtrlr2Valid=1 (poked in the caller), so we only
    // need to trigger it for MM, whose entry path was stripped: switch the gamestate to ovl_select's Init.
    if (mm && (btn & (MH_BTN_L | MH_BTN_R | MH_BTN_Z)) == (MH_BTN_L | MH_BTN_R | MH_BTN_Z))
    {
        g_MMU->UpdateMemoryValue32(playAddr + 0x0C, kMM_MapSelect_Init);   // GameState.init = MapSelect_Init
        uint32_t wr = 0;
        if (g_MMU->MemoryValue32(playAddr + 0x98, wr))
            g_MMU->UpdateMemoryValue32(playAddr + 0x98, wr & 0xFFFFFF00u); // running (byte @ +0x9B) = 0
        return;
    }

    // No-clip: hold L + D-pad (D-L/R = ±X, D-U/D = ±Y; add Z by also holding Z). Clear bgCheckFlags so the
    // engine doesn't snap the player back to the ground/wall, and write world.pos directly.
    if (btn & MH_BTN_L)
    {
        uint32_t playerPtrOff = mm ? 0x1CCCu : 0x1C44u;
        uint32_t bgFlagsOff   = mm ? 0x090u  : 0x088u;
        uint32_t player = 0;
        if (!g_MMU->MemoryValue32(playAddr + playerPtrOff, player)) return;
        if (player < 0x80000000u || player >= 0x80800000u) return;   // sanity: a real RDRAM pointer
        float dx = 0, dy = 0, dz = 0; const float spd = 40.0f;
        if (btn & MH_BTN_DL) dx -= spd;
        if (btn & MH_BTN_DR) dx += spd;
        if (btn & MH_BTN_DU) dy += spd;
        if (btn & MH_BTN_DD) dy -= spd;
        if (btn & MH_BTN_Z) { dz = dy; dy = 0; }   // L+Z + D-U/D moves along Z instead of Y
        if (dx != 0) MhAddFloat(player + 0x24, dx);
        if (dy != 0) MhAddFloat(player + 0x28, dy);
        if (dz != 0) MhAddFloat(player + 0x2C, dz);
        uint32_t wf = 0;
        uint32_t fa = player + (bgFlagsOff & ~3u);
        if (g_MMU->MemoryValue32(fa, wf))
            g_MMU->UpdateMemoryValue32(fa, (bgFlagsOff & 2u) ? (wf & 0xFFFF0000u) : (wf & 0x0000FFFFu));
    }
}

// Poke the area-transition the way an exit volume does: set the age, the next entrance, then start.
// Byte/halfword fields are written via 32-bit read-modify-write (the byte accessors are private),
// preserving the other bytes in each aligned word. N64 is big-endian.
static void MhDoWarp(int32_t entrance, int32_t age)
{
    g_MMU->UpdateMemoryValue32(kSaveContext + kOff_linkAge, (uint32_t)age);

    // Normalized playtest time-of-day (editor SceneSettings.PlaytestTimeOfDay): dayTime is the high 16
    // bits of the word at +0xC (big-endian); nightFlag (s32 @ +0x10) follows it (day ~6:00..18:00). The
    // same u16 the OTR engines apply to dayTime/save.time, so PJ64 starts at the identical time. The
    // debug save already ran (it set its own dayTime) — poke here, after it, the way the linkAge poke does.
    uint32_t wTime = 0;
    if (g_MMU->MemoryValue32(kSaveContext + kOff_dayTime, wTime))
        g_MMU->UpdateMemoryValue32(kSaveContext + kOff_dayTime,
                                   (wTime & 0x0000FFFFu) | ((uint32_t)(sParams.timeOfDay & 0xFFFF) << 16));
    g_MMU->UpdateMemoryValue32(kSaveContext + kOff_nightFlag,
                               (sParams.timeOfDay < 0x4555 || sParams.timeOfDay > 0xC000) ? 1u : 0u);

    // nextEntranceIndex (s16) is the low 16 bits of the word at (0x11E1A & ~3) == 0x11E18.
    uint32_t wEnt = 0;
    if (g_MMU->MemoryValue32(sPlayAddr + (kPlay_nextEntranceIndex & ~3u), wEnt))
        g_MMU->UpdateMemoryValue32(sPlayAddr + (kPlay_nextEntranceIndex & ~3u),
                                   (wEnt & 0xFFFF0000u) | ((uint32_t)entrance & 0xFFFFu));

    // transitionTrigger (s8 at 0x11E15) is byte 1 of the word at 0x11E14 (bits 16..23, big-endian).
    uint32_t wTrig = 0;
    if (g_MMU->MemoryValue32(sPlayAddr + (kPlay_transitionTrigger & ~3u), wTrig))
        g_MMU->UpdateMemoryValue32(sPlayAddr + (kPlay_transitionTrigger & ~3u),
                                   (wTrig & 0xFF00FFFFu) | ((uint32_t)TRANS_TRIGGER_START << 16));

    // Custom/empty inventory parity: overwrite the debug save's inventory with the editor's computed
    // loadout (no-op when the file is absent, i.e. debug-inventory mode). After the warp pokes, matching
    // the linkAge timing — the new scene's HUD/inventory reads these values.
    if (sParams.inventory != 0) MhApplySavePokes(kSaveContext);
}

// MM warp: same mechanism, MM offsets, no age. nextEntrance is a u16 (low 16 bits of the word at
// 0x18878); transitionTrigger is the s8 at byte 1 of the word at 0x18874. TRANS_TRIGGER_START=20.
static void MhDoWarpMM(int32_t entrance)
{
    uint32_t wEnt = 0;
    if (g_MMU->MemoryValue32(sPlayAddr + (kMM_Play_nextEntrance & ~3u), wEnt))
        g_MMU->UpdateMemoryValue32(sPlayAddr + (kMM_Play_nextEntrance & ~3u),
                                   (wEnt & 0xFFFF0000u) | ((uint32_t)entrance & 0xFFFFu));

    uint32_t wTrig = 0;
    if (g_MMU->MemoryValue32(sPlayAddr + (kMM_Play_transitionTrigger & ~3u), wTrig))
        g_MMU->UpdateMemoryValue32(sPlayAddr + (kMM_Play_transitionTrigger & ~3u),
                                   (wTrig & 0xFF00FFFFu) | ((uint32_t)TRANS_TRIGGER_START << 16));
    // (MM custom/empty inventory is applied from the per-frame block at kMM_SaveContext once the PlayState
    // is live — MM auto-boots without a warp entrance, so MhDoWarpMM usually doesn't run.)
}

// Called from N64System.cpp's emulation-thread catch(...) when the game crashes ("Stopping emulation").
// Records the CPU state so a crash (e.g. grabbing a ledge in a Megaton Hammer scene) is diagnosable:
// PC is where it died; EPC/BadVAddr pinpoint a memory fault (the bad address read/written). Cross-
// reference PC/EPC against the decomp map to find the exact game function.
extern "C" void MegatonHammer_LogCrash()
{
    MhLogOpen();
    if (g_Reg != nullptr)
    {
        MhLog("[mh] *** EMULATION CRASH at frame=%llu ***", (unsigned long long)sFrame);
        MhLog("[mh]   PC       = 0x%08X  (where execution died)", (uint32_t)g_Reg->m_PROGRAM_COUNTER);
        MhLog("[mh]   EPC      = 0x%08X  (faulting instruction, if a CPU exception)", (uint32_t)g_Reg->EPC_REGISTER);
        MhLog("[mh]   BadVAddr = 0x%08X  (bad memory address accessed)", (uint32_t)g_Reg->BAD_VADDR_REGISTER);
    }
    else
    {
        MhLog("[mh] *** EMULATION CRASH (g_Reg null) frame=%llu ***", (unsigned long long)sFrame);
    }
}

extern "C" void MegatonHammer_PerFrame()
{
    // Diagnostic: MH_NOHOOK fully disables the per-frame hook (no reads/writes/heartbeat), to prove
    // whether the hook is implicated in a boot crash. Crash logging still works (MegatonHammer_LogCrash
    // self-opens the log from the stock EmulationStarting catch).
    static int sNoHook = -1;
    if (sNoHook < 0) sNoHook = (getenv("MH_NOHOOK") != nullptr) ? 1 : 0;
    if (sNoHook) return;

    if (!sInit)
    {
        sInit = true;
        MhLogOpen();
        MhLog("[mh] Megaton Hammer PJ64 playtest hook active");
        sIsOotDebug = MhDetectOotDebug();
        MhLog("[mh] game detected = %s", sGame == GAME_OOT ? "OoT" : sGame == GAME_MM ? "MM" : "none");
    }

    // ---- Unconditional liveness heartbeat (independent of ROM detection) -------------------------
    // This runs for ANY ROM and BEFORE the OoT gate so a frozen boot is diagnosable: if these stop
    // appearing, emulation stalled (the per-frame hook is the VI handler — no frames => no calls).
    // Logs the live CPU PC so a hang's location is visible. Also honours MH_MAXFRAMES: once reached,
    // the run self-terminates (clean automation; no external timeout-kill needed).
    sFrame++;
    static uint64_t sMaxFrames = 0;
    static bool sReadMax = false;
    if (!sReadMax)
    {
        sReadMax = true;
        const char * mf = getenv("MH_MAXFRAMES");
        sMaxFrames = (mf != nullptr) ? strtoull(mf, nullptr, 10) : 0;
    }
    if ((sFrame % 30) == 0)
    {
        uint32_t pc = (g_Reg != nullptr) ? (uint32_t)g_Reg->m_PROGRAM_COUNTER : 0;
        MhLog("[mh] alive frame=%llu pc=0x%08X", (unsigned long long)sFrame, pc);
    }
    if (sMaxFrames != 0 && sFrame >= sMaxFrames)
    {
        MhLog("[mh] MH_MAXFRAMES=%llu reached -- exiting", (unsigned long long)sMaxFrames);
        if (sLog != nullptr) { fflush(sLog); fclose(sLog); sLog = nullptr; }
        _exit(0);
    }

    if (sGame == GAME_NONE || g_MMU == nullptr)
    {
        return;
    }
    if (!sParams.loaded)
    {
        MhLoadParams();
    }

    // ---- MM (Majora's Mask) branch -----------------------------------------------------------------
    if (sGame == GAME_MM)
    {
        uint32_t gm = 0;
        if (!g_MMU->MemoryValue32(kMM_SaveContext + kMM_Off_gameMode, gm))
        {
            return; // RDRAM not mapped yet
        }
        int32_t curMode = (int32_t)gm;
        if (curMode != sLastGameMode)
        {
            MhLog("[mh] frame=%llu MM gameMode=%d (0x%X)", (unsigned long long)sFrame, curMode, (uint32_t)curMode);
            sLastGameMode = curMode;
        }
        // "Save is up" = the boot debug save has run and memcpy'd the Save struct (entrance + inventory
        // together) into gSaveContext. save.entrance @ kMM_SaveContext+0 goes 0 -> 0x5400 at that point.
        // This is the robust gameplay signal: MM AUTO-BOOTS (MmInjectScene) straight into Play and NEVER
        // shows GAMEMODE_TITLE_SCREEN, so the old sSeenTitle gate never fired and inventory/debug-controls
        // never ran. gameMode also reads 0 both before boot (zeroed RAM) and in-game (NORMAL), so it can't
        // gate on its own — pair it with entrance != 0.
        static int  sMmInvApplied = 0;
        static uint64_t sMmSaveUpFrame = 0;   // first frame save.entrance became non-zero (0 = not yet)
        uint32_t saveEntr = 0;
        g_MMU->MemoryValue32(kMM_SaveContext, saveEntr);
        bool saveUp = (saveEntr != 0) && (curMode == GAMEMODE_NORMAL);
        if (saveUp && sMmSaveUpFrame == 0) sMmSaveUpFrame = sFrame;

        // Custom/empty inventory does NOT depend on locating the PlayState — the pokes target the FIXED
        // kMM_SaveContext. Apply once the save is up, then RE-ASSERT a couple more times (spaced) so we win
        // even if the debug save writes inventory slightly after entrance. The player can't have changed
        // inventory this early, so re-applying is harmless. This is what makes MM N64 honour the editor's
        // playtest inventory instead of only the debug loadout.
        if (sParams.inventory != 0 && sMmSaveUpFrame != 0 && sMmInvApplied < 3 &&
            (sFrame - sMmSaveUpFrame) >= (uint64_t)(4 + sMmInvApplied * 20))
        {
            MhApplySavePokes(kMM_SaveContext);
            sMmInvApplied++;
            MhLog("[mh] MM custom inventory applied @0x%08X (%zu pokes, pass %d, frame=%llu)",
                  kMM_SaveContext, sSavePokes.size(), sMmInvApplied, (unsigned long long)sFrame);
        }

        // MM normally AUTO-BOOTS via MmInjectScene (no warp entrance in params), so activate on any work we
        // owe: a warp (rare) or debug controls. These DO need a live PlayState, so keep the scan gate here.
        bool wantWork = sParams.valid || sParams.debugControls;
        if (wantWork && saveUp)
        {
            if (sPlayAddr == 0)
            {
                uint32_t found = MhScanForPlayStateMM(0x8000);
                if (found != 0)
                {
                    sPlayAddr = found;
                    MhLog("[mh] MM PlayState found at 0x%08X", sPlayAddr);
                }
            }
            if (sPlayAddr != 0)
            {
                if (sParams.valid && !sWarpDone)
                {
                    MhDoWarpMM(sParams.entranceIndex);
                    sWarpDone = true;
                    MhLog("[mh] MM WARP triggered: entrance=0x%04X (via play 0x%08X)", sParams.entranceIndex, sPlayAddr);
                }
                if (sParams.debugControls) MhDebugControls(sPlayAddr, true);
            }
        }
        // PASSIVE render probe (independent of the warp/inventory work-gating above): every heartbeat, find
        // and LOG the live PlayState's sceneId so the log always shows WHICH scene actually loaded. A blank
        // scene (e.g. an appended scene the DMA couldn't reach) shows "no PlayState" or a garbage sceneId here,
        // instead of the old silent absence of the sceneId line. Cheap once found (sPlayAddr is cached).
        if ((sFrame % 30) == 0)
        {
            // Re-validate the cached PlayState (its size must still look like a PlayState) — the gamestate
            // changes as the game boots, so a stale address would report a defunct scene.
            if (sPlayAddr != 0)
            {
                uint32_t sv = 0;
                if (!g_MMU->MemoryValue32(sPlayAddr + kGS_size, sv) || sv < 0x11000 || sv > 0x14000) sPlayAddr = 0;
            }
            if (sPlayAddr == 0)
            {
                uint32_t found = MhScanForPlayStateMM(0x8000);
                if (found != 0) { sPlayAddr = found; MhLog("[mh] MM PlayState found at 0x%08X", sPlayAddr); }
            }
            uint32_t entr = 0; g_MMU->MemoryValue32(kMM_SaveContext, entr);   // save.entrance (scene_no)
            if (sPlayAddr != 0)
            {
                uint32_t sid = 0;
                g_MMU->MemoryValue32(sPlayAddr + kMM_Play_sceneId, sid);
                MhLog("[mh] MM render probe frame=%llu sceneId=0x%X save.entrance=0x%08X gameMode=%d",
                      (unsigned long long)sFrame, (uint16_t)(sid >> 16), entr, curMode);
            }
            else
            {
                MhLog("[mh] MM render probe frame=%llu: no live play scene (save.entrance=0x%08X gameMode=%d)",
                      (unsigned long long)sFrame, entr, curMode);
            }
        }
        // One-shot DMA diagnostic: is the append target VROM (0x02EDB000 = Termina-clone) in the RAM dmadata?
        // (Stable for a Termina-cloned append. For overwrite it's expected NO — that just confirms the probe.)
        static bool sMmDmaChecked = false;
        if (!sMmDmaChecked && sFrame >= 60) { sMmDmaChecked = true; MhCheckMmDma(0x02EDB000); }
        return;
    }

    // ---- Observe live SaveContext (verification channel) -------------------
    uint32_t entrance = 0, gameMode = 0;
    bool okE = g_MMU->MemoryValue32(kSaveContext + kOff_entranceIndex, entrance);
    bool okM = g_MMU->MemoryValue32(kSaveContext + kOff_gameMode, gameMode);
    if (!okE || !okM)
    {
        return; // RDRAM not mapped yet (very early boot)
    }
    int32_t curEntrance = (int32_t)entrance;
    int32_t curMode = (int32_t)gameMode;
    if (curEntrance != sLastEntrance || curMode != sLastGameMode)
    {
        MhLog("[mh] frame=%llu gameMode=%d (0x%X) entranceIndex=0x%04X", (unsigned long long)sFrame, curMode,
              (uint32_t)curMode, (uint16_t)curEntrance);
        sLastEntrance = curEntrance;
        sLastGameMode = curMode;
    }

    // ---- Custom/empty inventory + time-of-day (DECOUPLED from the PlayState scan) ----------
    // The inventory pokes + dayTime target the FIXED kSaveContext, so they need no PlayState. Applying them
    // only inside MhDoWarp (scan-gated) meant they silently never applied under auto-boot — the OoT twin of
    // the MM bug (GameState.init is cleared in gameplay so the scan misses the PlayState). Gate instead on
    // "save is up" = entranceIndex != 0 (the debug save / auto-boot has run) && a valid gameMode, and apply
    // 3x spaced to beat any late debug-save write. Age is applied pre-scene-load by OotDebugAutoBoot so Link's
    // model is correct; here we only need the inventory + time (HUD/sky read them live).
    static int sOotApplied = 0;
    static uint64_t sOotSaveUpFrame = 0;
    if (curEntrance != 0 && curMode >= 0 && curMode <= 3 && sOotSaveUpFrame == 0) sOotSaveUpFrame = sFrame;
    if (sParams.inventory != 0 && sOotSaveUpFrame != 0 && sOotApplied < 3 &&
        (sFrame - sOotSaveUpFrame) >= (uint64_t)(4 + sOotApplied * 20))
    {
        MhApplySavePokes(kSaveContext);
        uint32_t wTime = 0;
        if (g_MMU->MemoryValue32(kSaveContext + kOff_dayTime, wTime))
            g_MMU->UpdateMemoryValue32(kSaveContext + kOff_dayTime,
                                       (wTime & 0x0000FFFFu) | ((uint32_t)(sParams.timeOfDay & 0xFFFF) << 16));
        g_MMU->UpdateMemoryValue32(kSaveContext + kOff_nightFlag,
                                   (sParams.timeOfDay < 0x4555 || sParams.timeOfDay > 0xC000) ? 1u : 0u);
        sOotApplied++;
        MhLog("[mh] OoT custom inventory applied @0x%08X (%zu pokes, pass %d, frame=%llu)",
              kSaveContext, sSavePokes.size(), sOotApplied, (unsigned long long)sFrame);
    }

    // ---- Auto-warp to the editor's entrance once a PlayState exists ------------------------
    // Only acts when params carry a target entrance and the gc-eu-mq-dbg addresses verify (a valid
    // gameMode 0..3), so a wrong ROM is never poked. The warp fires once; the resulting scene is
    // logged for verification. Reaching any gameplay (opening demo, map-select, new game) triggers it.
    if (sParams.valid && !sWarpDone && curMode >= 0 && curMode <= 3)
    {
        if (sPlayAddr == 0)
        {
            uint32_t found = MhScanForPlayState(0x8000, kPlay_Init);
            if (found != 0)
            {
                sPlayAddr = found;
                uint32_t sid = 0;
                g_MMU->MemoryValue32(sPlayAddr + kPlay_sceneId, sid);
                MhLog("[mh] PlayState found at 0x%08X (current sceneId=0x%X)", sPlayAddr, (uint16_t)sid);
            }
        }
        if (sPlayAddr != 0)
        {
            MhDoWarp(sParams.entranceIndex, sParams.linkAge);
            sWarpDone = true;
            MhLog("[mh] WARP triggered: entrance=0x%04X age=%d (via play 0x%08X)", sParams.entranceIndex,
                  sParams.linkAge, sPlayAddr);
        }
    }

    // ---- Debug controls (OoT, off by default) --------------------------------------------------
    // gIsCtrlr2Valid=1 each frame makes the debug ROM's own L+R+Z map-select hotkey work with a single
    // controller (it re-derives the flag from validCtrlrsMask each frame, so the poke must repeat). The
    // L+D-pad no-clip is driven here. Neither touches the inventory.
    if (sParams.debugControls)
    {
        g_MMU->UpdateMemoryValue32(kOoT_gIsCtrlr2Valid, 1);
        if (sPlayAddr != 0) MhDebugControls(sPlayAddr, false);
    }

    // Heartbeat: prove emulation is advancing + capture the gameMode trajectory. Also flag when a
    // *valid* OoT gameMode (0..3) appears at our gc-eu-mq-dbg gSaveContext address — that confirms
    // the loaded ROM's RAM layout matches (i.e. this debug ROM is address-compatible).
    if ((sFrame % 30) == 0)
    {
        uint32_t age = 0;
        g_MMU->MemoryValue32(kSaveContext + kOff_linkAge, age);
        bool validMode = curMode >= 0 && curMode <= 3;
        MhLog("[mh] hb frame=%llu gameMode=%d(0x%X)%s entrance=0x%04X linkAge=%d",
              (unsigned long long)sFrame, curMode, (uint32_t)curMode, validMode ? " VALID" : "",
              (uint16_t)curEntrance, (int32_t)age);
    }
}
