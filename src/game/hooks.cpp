// Hooks for default.xbe (True Crime: Streets of LA, PAL, XDK 5659).
// A strong X86_FUNC definition replaces the recompiled function at that address.
// To call the original body: X86_EXTERN_FUNC(__imp__sub_XXXXXXXX); then __imp__sub_XXXXXXXX(ctx, base);

#include <algorithm>
#include <filesystem>
#include <random>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

#include <imgui.h>

#include "app/app.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

// =============================================================================================
// Two European discs: the five-language disc (En/Fr/De/Es/It, default.xbe v8, sha1 7c665c36... or
// 5e0aeb15... from xiso copies) and the English-only disc (v7, sha1 19aa3175...), built 44 minutes apart.
// Same game code, laid out a little differently: TC_SEL(five-language, English-only) picks an address for
// the disc being built. CMakeLists.txt sets TC_EN_DISC from the xbe in assets and picks its config.
// The English-only addresses were found by matching the code and data references around each one.
// =============================================================================================
#if defined(TC_EN_DISC) && TC_EN_DISC
#define TC_SEL(multi, en) en
#else
#define TC_SEL(multi, en) multi
#endif

// release builds keep only errors and reports in the log
#define TC_LOG_FIRST(n, ...)                                                     \
    do {                                                                         \
        static std::atomic<int> _c{0};                                           \
        if (_c.fetch_add(1, std::memory_order_relaxed) < (n)) LOGR(__VA_ARGS__); \
    } while (0)

// =============================================================================================
// DirectSound: 3D voice setters
//
// The XDK's internal CDirectSoundVoice 3D setters write into voice->[+0x10]->[+0xB4], the voice's
// 3D/HRTF block on the Xbox audio hardware. The host's DirectSound never builds that block, so the
// pointer is null and the game crashed right after audio init. The host plays no 3D audio here,
// so accept them like the original does: S_OK, stdcall arguments popped.
// =============================================================================================
#define TC_VOICE_3D_STUB(fn, nargs)                 \
    X86_FUNC(fn) {                                  \
        (void)base;                                 \
        ::kernel::Return(ctx, 0 /* S_OK */, nargs); \
    }

TC_VOICE_3D_STUB(TC_SEL(sub_002127A6, sub_00212606), 3)  // SetMaxDistance(voice, distance, apply)       callers 0x212A95 / 0x212D4A
TC_VOICE_3D_STUB(TC_SEL(sub_002127D9, sub_00212639), 3)  // SetMinDistance(voice, distance, apply)       callers 0x212AEB / 0x212DA4
TC_VOICE_3D_STUB(TC_SEL(sub_0021280C, sub_0021266C), 3)  // SetMode(voice, mode, apply)                  callers 0x212B41 / 0x212DFE
TC_VOICE_3D_STUB(TC_SEL(sub_0021283F, sub_0021269F), 5)  // SetPosition(voice, x, y, z, apply)           callers 0x212B93 / 0x212E54
TC_VOICE_3D_STUB(TC_SEL(sub_00212891, sub_002126F1), 3)  // writes 3D block +0x4C (velocity/doppler)     callers 0x212BFA / 0x212EBF
TC_VOICE_3D_STUB(TC_SEL(sub_002128C4, sub_00212724), 4)  // SetRolloffCurve(voice, count, curve, apply)  callers 0x212C50 / 0x212F19

// =============================================================================================
// Stutter: the game's modal wait loops sleep 200 ms between frames
//
// 0x001E7866 is XAPI Sleep(ms). Two game loops call it with 200 ms:
//   0x000D7965  in 0x000D7510: while the player object's byte +0x191 is clear (set by the player
//               state machine at 0x000B2E9F), it draws its overlay, sleeps 200 ms, reads the pads
//               (0x001C8EA0) and repeats until a port reports a button (flags 0x1100000)
//   0x001D17BB  an endless draw / sleep 200 ms / read pads loop (fatal message screen)
// On the console they are waits for a button press and their frame rate does not matter much; here
// they ran the game at 5 fps (206 ms a frame in the timing log) for as long as they lasted. They now
// sleep one 60 Hz frame instead: they still wait for the same button, but redraw and read the pads
// at full speed.
// =============================================================================================
X86_EXTERN_FUNC(TC_SEL(__imp__sub_001E7866, __imp__sub_001E76C6));
X86_FUNC(TC_SEL(sub_001E7866, sub_001E76C6)) {
    uint32_t ret = ::kernel::R32(ctx.esp);
    uint32_t ms = ::kernel::R32(ctx.esp + 4);
    if (ms >= 100 && (ret == TC_SEL(0x000D796Au, 0x000D79FAu) || ret == TC_SEL(0x001D17C0u, 0x001D1620u))) {
        TC_LOG_FIRST(20, "tc: wait loop at %08X slept %u ms, now 16 ms", ret, ms);
        ::kernel::W32(ctx.esp + 4, 16);
    } else if (ms >= 100) {
        TC_LOG_FIRST(20, "tc: Sleep(%u) from %08X", ms, ret);
    }
    TC_SEL(__imp__sub_001E7866, __imp__sub_001E76C6)(ctx, base);
}

// =============================================================================================
// Stutter (the real one): occlusion queries never finish, the game spins 100 x Sleep(1) a query
//
// 0x001FF410 is D3DDevice_GetVisibilityTestResult(index, &result, &timestamp). It reads the
// query's report slot through the device's table at CDevice+0x7D4; a status of 0xFFFFFFFF means
// the GPU has not written it yet and it returns D3DERR_TESTINCOMPLETE (0x88760828).
// The game's loop at 0x001C93F0 polls it with Sleep(1) between tries, up to 100 tries (cmp 0x64)
// for every query. On the console the GPU fills the slot within the frame; here it stays pending,
// so each frame burned all 100 sleeps: 491 KeDelayExecutionThread calls a second from SleepEx
// (0x001E7833) in the profiler, ~200 ms a frame, the 5 fps stretches and slow menu model pop-in.
// A pending query now answers at once with that query's last real result, or 0 (not seen yet) before it ever had one.
//
// [2.0] The only caller is the light glare (0x001C93C0): its alpha byte is pixels * 0.0008 (0x0026F98C) * the
// glare's strength * its colour's alpha, truncated to a byte (0x001C97B2) with no clamp, so 1250 pixels is full
// strength. The old "visible" answer, 640x480 = 307200 pixels before a query's first result, was 245 times that;
// the byte wrapped and lights flashed into big white blobs on the ground and in the air. A pending query with no
// result yet now reads 0, and every count is capped at 1250, so a glare is at most as bright as it was made to be.
// =============================================================================================
X86_EXTERN_FUNC(TC_SEL(__imp__sub_001FF410, __imp__sub_001FF270));
X86_FUNC(TC_SEL(sub_001FF410, sub_001FF270)) {
    constexpr uint32_t kTestIncomplete = 0x88760828u;
    constexpr uint32_t kFullGlare = 1250u;  // pixels for full glare strength: 1 / 0.0008
    static std::mutex lock;
    static std::unordered_map<uint32_t, uint32_t> last;  // query index -> last real pixel count
    uint32_t index = ::kernel::R32(ctx.esp + 4);
    uint32_t result = ::kernel::R32(ctx.esp + 8);
    uint32_t stamp = ::kernel::R32(ctx.esp + 12);
    TC_SEL(__imp__sub_001FF410, __imp__sub_001FF270)(ctx, base);  // stdcall: pops its 3 arguments
    std::lock_guard<std::mutex> guard(lock);
    if (ctx.eax == 0) {
        if (result) {
            uint32_t pixels = std::min(::kernel::R32(result), kFullGlare);
            ::kernel::W32(result, pixels);
            last[index] = pixels;
        }
        return;
    }
    if (ctx.eax != kTestIncomplete) return;
    auto it = last.find(index);
    uint32_t pixels = it != last.end() ? it->second : 0u;
    if (result) ::kernel::W32(result, pixels);
    if (stamp) {
        ::kernel::W32(stamp, 0);
        ::kernel::W32(stamp + 4, 0);
    }
    ctx.eax = 0;  // D3D_OK
    TC_LOG_FIRST(4, "tc: visibility test %08X pending, answered %u pixels", index, pixels);
}

// =============================================================================================
// [2.0] SCREEN widescreen: subtitles centred
//
// A subtitle is a text box the game places with 0x0008DFB0 (this = the box; position packed x | y << 16, size, font,
// text): 0x000A0F3E puts it at x 64, 512 wide, and 0x000A0FA7 (five-language disc only) at x 116, 420 wide, both
// centred on x 320 of the 640 wide 4:3 layout. Neither asks the HUD layout to centre it (its x mapping, 0x000BEB80,
// adds the screen's middle only for boxes flagged centred), so in the 1280 wide widescreen frame the subtitles sat
// centred on x 320, a quarter of the way across. Their x is moved here by half of what the frame has past 640, which
// centres them; on a 640 wide frame nothing moves. The frame's width is the game's own (0x001D1870: video mode
// flag 2 is 1280 wide, 4 is 1920, else 640). (The HUD's left and right edges were tried first: they stand inside
// the title safe area, so the move fell 100 short.)
// =============================================================================================
X86_EXTERN_FUNC(TC_SEL(__imp__sub_0008DFB0, __imp__sub_0008E0B0));
X86_FUNC(TC_SEL(sub_0008DFB0, sub_0008E0B0)) {
    constexpr uint32_t kSubtitleRet = TC_SEL(0x000A0F43u, 0x000A0FF3u);
    constexpr uint32_t kSubtitleRet2 = TC_SEL(0x000A0FACu, 0u);  // the English-only disc has the one call
    constexpr uint32_t kVideoMode = TC_SEL(0x002E7DC8u, 0x002E7AE8u);  // read by 0x001D1870 for the frame width
    uint32_t ret = ::kernel::R32(ctx.esp);
    if (ret == kSubtitleRet || (kSubtitleRet2 && ret == kSubtitleRet2)) {
        uint32_t mode = ::kernel::R32(kVideoMode);
        int32_t width = (mode & 2) ? 1280 : (mode & 4) ? 1920 : 640;
        if (width > 640) {
            uint32_t pos = ::kernel::R32(ctx.esp + 4);
            int32_t x = int16_t(pos & 0xFFFF) + (width - 640) / 2;
            ::kernel::W32(ctx.esp + 4, (pos & 0xFFFF0000u) | (uint32_t(uint16_t(int16_t(x)))));
            TC_LOG_FIRST(2, "tc: subtitle moved to x %d for a %d wide frame", x, width);
        }
    }
    TC_SEL(__imp__sub_0008DFB0, __imp__sub_0008E0B0)(ctx, base);
}

// =============================================================================================
// [2.0] Tyre skid level. A car sound's distance curve (0x001D9560: handle, floor, max distance, level, min distance,
// shape; built at 0x001D9350) starts at the level the title asks for and falls to the floor at its max distance. For each wheel's skid (sound 0xD1, set up at
// 0x0016B8B7) that level is 0.1 + 0.2 * the wheel's slip, about 0.5 at speed, and the title slips the wheels at almost
// any speed. On PC the skid measured about twice the music and three times the player's engine, screeching over
// everything while driving (the PC mix has no HRTF filtering of 3D voices, which may account for part of it).
// The skid's level is scaled here by skid_volume in the settings file (percent, 50 by default: half, 6 dB down;
// 100 is the level the game asks for, 0 silences it). The curve, the other car sounds and the game stay as they are.
// =============================================================================================
X86_EXTERN_FUNC(TC_SEL(__imp__sub_001D9560, __imp__sub_001D93C0));
X86_FUNC(TC_SEL(sub_001D9560, sub_001D93C0)) {
    constexpr uint32_t kSkidCallReturn = TC_SEL(0x0016B8D7u, 0x0016B777u);
    if (::kernel::R32(ctx.esp) == kSkidCallReturn) {
        const float kSkidLevel = float(std::min(app::Get().skid_volume, 100u)) / 100.0f;
        uint32_t bits = ::kernel::R32(ctx.esp + 16);  // cdecl: handle, floor, max distance, level, ...
        float level;
        std::memcpy(&level, &bits, 4);
        if (std::isfinite(level)) {
            level *= kSkidLevel;
            std::memcpy(&bits, &level, 4);
            ::kernel::W32(ctx.esp + 16, bits);
        }
        TC_LOG_FIRST(4, "tc: tyre skid level scaled to %.2f", level);
    }
    TC_SEL(__imp__sub_001D9560, __imp__sub_001D93C0)(ctx, base);
}

// =============================================================================================
// Rainbow textures and slow menu city: the title's file streamer, start of a request
//
// 0x001DA710 (cdecl: slot, dest, length) starts a read of `length` bytes from the slot's file offset
// (slot + 8) into `dest`. Slots are 0xA8 bytes at 0x305658 and begin with the OVERLAPPED. A start
// that is not on a 2 KB sector first reads that whole sector into the slot's bounce buffer
// (slot + 0x50). On the console that read always pends and a later poll (0x001DA8E0 -> 0x001DA640)
// copies the right bytes. Here file reads finish at once, which takes the branch at 0x001DA81D: it
// copies a whole 2 KB from bounce + misalignment (bytes past the buffer end) and counts 2 KB done,
// so every later sector landed that far too high. That was the rainbow 4x4 blocks across the top
// rows of streamed road, tree and building textures, and the shifted rest of each one.
// The earlier fix made the kernel answer every asynchronous read STATUS_PENDING instead; correct,
// but each step of a request then cost the title a frame, and the city behind the main menu took
// some twenty seconds to stream in. This is the same routine with that one branch corrected, so
// reads complete at once again and the bytes still land where they belong.
// =============================================================================================
namespace {
constexpr uint32_t kStreamSlots = TC_SEL(0x00305658u, 0x00305378u), kStreamSlotSize = 0xA8u, kSector = 0x800u;
constexpr uint32_t kReadFile = TC_SEL(0x001E9224u, 0x001E9084u), kGetLastError = TC_SEL(0x001E997Fu, 0x001E97DFu), kIoPending = 997u;
}  // namespace

X86_FUNC(TC_SEL(sub_001DA710, sub_001DA570)) {
    (void)base;
    using ::kernel::R32;
    using ::kernel::W32;
    int32_t index = int32_t(R32(ctx.esp + 4));
    uint32_t dest = R32(ctx.esp + 8), len = R32(ctx.esp + 12);
    auto done = [&](uint32_t r) { ::kernel::Return(ctx, r, 0); };  // cdecl: the caller pops
    if (index < 0) return done(0xFFFFFFFFu);
    uint32_t s = kStreamSlots + uint32_t(index) * kStreamSlotSize;
    if (!R32(s + 0x1C)) return done(0xFFFFFFFFu);  // no file open in the slot
    uint32_t start = R32(s + 8);
    W32(s + 0x18, 1);  // finished, for the poll
    W32(s + 0x3C, 0);
    W32(s + 0x40, 0);
    W32(s + 0x44, 0);
    W32(s + 0x48, 0);
    W32(s + 0x4C, 0);
    W32(s + 0x24, 1);
    W32(s + 0x30, dest);
    W32(s + 0x34, start);
    W32(s + 0x38, len);
    if (len < kSector && !R32(s + 0x28)) return done(1);  // the poll reads short requests
    uint32_t chunk = len & ~(kSector - 1), aligned = start & ~(kSector - 1);
    W32(s + 0x48, chunk);
    W32(s + 0x3C, aligned);
    uint32_t handle = R32(s + 0x14);
    auto failed = [&]() {
        uint32_t err = ::kernel::CallGuest(ctx, kGetLastError, {});
        W32(s + 0x2C, err);
        if (err != kIoPending) return done(0);
        W32(s + 0x18, 0);  // pending: the poll collects it
        done(1);
    };
    if (aligned == start && chunk) {
        W32(s + 0x20, 0);
        if (!::kernel::CallGuest(ctx, kReadFile, {handle, dest, chunk, s + 0xA4, s})) return failed();
        W32(s + 0x4C, std::min(chunk, len));
        return done(1);
    }
    W32(s + 8, aligned);  // OVERLAPPED.Offset
    W32(s + 0x48, kSector);
    W32(s + 0x20, 1);
    uint32_t bounce = R32(s + 0x50);
    if (!::kernel::CallGuest(ctx, kReadFile, {handle, bounce, kSector, s + 0xA4, s})) return failed();
    // the fix: only the sector's bytes from the start on are this request's
    uint32_t skip = start - aligned;
    uint32_t n = std::min(kSector - skip, len);
    std::memcpy(::kernel::HostPtr(dest), ::kernel::HostPtr(bounce + skip), n);
    W32(s + 0x4C, n);
    TC_LOG_FIRST(3, "tc: streamer read at %08X skipped %X bytes of its first sector", start, skip);
    done(1);
}

// =============================================================================================
// Main menu city (and the city in play): streams in one model and one texture a frame
//
// 0x001175D0 (thiscall: cell x, cell z, flag; ret 0xC) is the city streamer's per-frame update. It
// finishes the requests whose reads are done (0x00113BA0), then starts at most one model read
// (TEST.EXM, slot +0x14, request +0x1C) and one texture read (TEST.EXT, slot +0x18, request +0x20).
// On the console a read takes the DVD several frames, so one at a time is all it could use. Here the
// read is done before the call returns and the streamer then idles until the next frame: the 3D
// city behind the main menu came in building by building over several seconds.
// The update now runs again in the same frame while it keeps finishing and starting requests,
// for up to 4 ms of the frame, so the city arrives as fast as the files can be read.
// =============================================================================================
#include <chrono>

X86_EXTERN_FUNC(TC_SEL(__imp__sub_001175D0, __imp__sub_001175A0));
X86_FUNC(TC_SEL(sub_001175D0, sub_001175A0)) {
    using ::kernel::R32;
    using ::kernel::W32;
    uint32_t self = ctx.ecx;
    uint32_t a0 = R32(ctx.esp + 4), a1 = R32(ctx.esp + 8), a2 = R32(ctx.esp + 12);
    uint32_t ret = R32(ctx.esp);
    auto progress = [&] {
        return uint64_t(R32(self + 0x1C)) ^ uint64_t(R32(self + 0x20)) << 32 ^ uint64_t(R32(self + 0x28)) << 7 ^
               uint64_t(R32(self + 0x34)) << 19;
    };
    auto start = std::chrono::steady_clock::now();
    // On the title menu (game state 0x002AEE58 == 5) the backdrop city is loaded whole before the frame goes on,
    // so it is there from the first frame as on the console; in play the 4 ms budget keeps frames smooth.
    bool title_menu = R32(TC_SEL(0x002AEE58, 0x002AEB78)) == 5;
    int max_passes = title_menu ? 4096 : 32;
    auto budget = title_menu ? std::chrono::milliseconds(1500) : std::chrono::milliseconds(4);
    uint64_t before = progress();
    TC_SEL(__imp__sub_001175D0, __imp__sub_001175A0)(ctx, base);  // pops its 3 arguments and the return address
    uint32_t esp = ctx.esp;
    for (int i = 0; i < max_passes; i++) {
        uint64_t now = progress();
        bool all_ready = (ctx.eax & 0xFF) != 0;
        if (all_ready || now == before) break;
        if (std::chrono::steady_clock::now() - start > budget) break;
        before = now;
        // call it again the way the title did: same object, same arguments
        ctx.esp = esp - 16;
        W32(ctx.esp, ret);
        W32(ctx.esp + 4, a0);
        W32(ctx.esp + 8, a1);
        W32(ctx.esp + 12, a2);
        ctx.ecx = self;
        TC_SEL(__imp__sub_001175D0, __imp__sub_001175A0)(ctx, base);
    }
    ctx.esp = esp;
}

// =============================================================================================
// Quit Game: an EXIT entry on the main menu that closes the game, as a PC game's menu does
//
// The main menu is a layout record in the game's data at 0x0028AC34 (title "main menu", entry array
// at +0x14, entry count at +0x18). Each entry is 0x14 bytes: type 1, string id (Data\Shell\TCXBOX??.txt),
// select callback, value, flags. NEW GAME (67), RESUME GAME (68) and OPTIONS (71) all use the callback
// 0x00142A90 (thiscall, 2 args, ret 8), which opens the frontend screen numbered by the value (1, 2, 4).
// 0x00147060 (thiscall, 1 arg: the record) builds a page's widgets from such a record.
//
// Before the main menu is first built, its record gets a copy of the three entries plus a fourth: the
// game's own "EXIT" text (string 457, translated in every language file), the same callback, and a value
// no screen uses. The callback hook takes that value and closes the window like its close button, which
// shuts the runtime down. The game's files on disk are not changed.
//
// The console's own exit prompt ("EXIT TO Xbox DASHBOARD / Are you sure?", in the memory card screens)
// ends in action 0x29 of 0x0014B350, which calls XLaunchNewImage(NULL) for the dashboard; that quits too.
// (The XAPI's startup also asks for the dashboard once, so quitting is done here, not in XLaunchNewImage.)
// =============================================================================================
namespace {
constexpr uint32_t kMainMenuRecord = TC_SEL(0x0028AC34u, 0x0028A974u);
constexpr uint32_t kOpenScreen = TC_SEL(0x00142A90u, 0x001429F0u);
constexpr uint32_t kExitString = 457;     // "EXIT"
constexpr uint32_t kQuitValue = 0x7151u;  // not a screen number: the EXIT entry
std::atomic<bool> g_quit_entry{false};

// =============================================================================================
// Display options: DISPLAY MODE and RESOLUTION on the options' display screen, working like RADAR TYPE
// and SUBTITLES: the focused entry shows a panel on the right with the current choice in large text and
// the choices underneath; A on the entry moves into the panel, A on a choice applies and saves it.
//
// The display screen's list is the layout record at 0x0028ADC8 (RADAR TYPE, SUBTITLES): two entries
// are appended, with the callback those use, 0x00142A90's sibling 0x00142AB0, which hands its value to
// the screen's dispatcher 0x001576B0 (thiscall (code, arg), ret 8). The screen object is made by
// 0x0015AA10 (vtable 0x0026DAC0); its radar and subtitles panels are objects of 0x00158950 at +0x964
// and +0xC64, with choice buttons added by 0x00146FB0 to panel+0x1A8 (each button's +0x74 block holds
// callback, code and the string shown large while it is selected), the panel's own back callback at
// +0x234, and the panel linked into the screen's widget list at +0x84. The dispatcher codes for radar
// are 8 open, 9 choice, 10 back (11/12/13 subtitles); 0x001578B0 (thiscall (old, new), ret 8) shows the
// focused entry's panel. Here two more panels are made the same way, after the screen's constructor,
// and the dispatcher and the focus handler get the new codes. The game's files are not changed.
//
// Their text needs strings of its own. The game keeps the language file as an array of 3192 string
// pointers at [0x002C700C], made once by 0x00142520 (cdecl, the language loader) and refilled in place
// on every language change. After each load the array is swapped once for a copy with room for more,
// which point at text kept here (English only).
// =============================================================================================
constexpr uint32_t kDisplayRecord = TC_SEL(0x0028ADC8u, 0x0028AAF4u);
constexpr uint32_t kStringTable = TC_SEL(0x002C700Cu, 0x002C6D2Cu);  // guest pointer to the string pointer array
constexpr uint32_t kGameStrings = 3192;          // its size: 0x31E0 bytes
constexpr uint32_t kLabels = 48, kLabelSize = 64;
constexpr uint32_t kTrackSlots = 44;  // PC soundtrack lines, past the labels (songs 84..127)
constexpr uint32_t kMessageSize = 96;  // the save messages, kept apart from the short labels
constexpr uint32_t kSubPanel = TC_SEL(0x00142AB0u, 0x00142A10u);      // entry callback: the screen's dispatcher
constexpr uint32_t kPanelCtor = TC_SEL(0x00158950u, 0x001587F0u), kPanelSize = 0x400;  // panels are 0x300 apart in the screen
constexpr uint32_t kAddButton = TC_SEL(0x00146FB0u, 0x00146F30u), kHighlight = TC_SEL(0x001463D0u, 0x00146350u);
constexpr uint32_t kPlaySound = TC_SEL(0x000BED90u, 0x000BEE10u), kScreenFocus = TC_SEL(0x00143280u, 0x001431E0u), kPanelsDone = TC_SEL(0x00154550u, 0x00154430u);
constexpr uint32_t kScreenUnfocus = TC_SEL(0x00143260u, 0x001431C0u);
constexpr uint32_t kOptions = TC_SEL(0x002A4258u, 0x002A3F78u);  // guest pointer to the game's options block
constexpr uint32_t kSubtitlesOn = 0x1B8, kSubtitlesOff = 0x1B9;  // "subtitles on" / "subtitles off"
constexpr uint32_t kSetText = TC_SEL(0x00143350u, 0x001432B0u), kPanelShow = TC_SEL(0x00153490u, 0x00153370u);  // the panel class's show (vtable +0x0C)
constexpr uint32_t kNoOp4 = TC_SEL(0x000CF210u, 0x000CF2A0u);   // a guest function that is only "ret 4"
constexpr uint32_t kVtableCopy = 64;      // entries copied: the class's vtable and some past it  // gives the screen its input back (0x00143280 takes it)
constexpr uint32_t kWidgetCount = TC_SEL(0x002A8EE0u, 0x002A8C00u), kWidgetNodes = TC_SEL(0x002A46D0u, 0x002A43F0u);
constexpr uint32_t kOne = 0x3F800000u;           // 1.0f
// label slots past the game's strings
enum : uint32_t { kLabelMode, kLabelRes, kLabelModeChoice, kLabelModeBig = kLabelModeChoice + 3,
                  kLabelResChoice = kLabelModeBig + 3, kLabelResBig = kLabelResChoice + 5,
                  kLabelExitYesBig = kLabelResBig + 5, kLabelExitNoBig,
                  kLabelScreen, kLabelScreenChoice, kLabelScreenBig = kLabelScreenChoice + 2,
                  kLabelFps = kLabelScreenBig + 2, kLabelFpsChoice, kLabelFpsBig = kLabelFpsChoice + 2,
                  kLabelBoss = kLabelFpsBig + 2, kLabelBossDefaultBig, kLabelBossHeader, kLabelBossTitle,
                  kLabelBossOpenBig, kLabelBossInfoBig, kLabelBossEntry,  // kLabelBossEntry + i: boss i's entry
                  kLabelCount = kLabelBossEntry + 8 };
static_assert(kLabelCount <= kLabels);
// dispatcher codes, past the game's 8..14
enum : uint32_t { kOpenMode = 0x40, kPickMode, kBackMode, kOpenRes, kPickRes, kBackRes, kOpenWide, kPickWide, kBackWide,
                  kOpenFps, kPickFps, kBackFps };
// the audio screen's BOSS TRACK codes (its own dispatcher, 0x00156990, has 4..0x13); kBossEntry + i: boss i's entry
enum : uint32_t { kOpenBoss = 0x50, kBossInfo, kBossBack, kBossCategory, kBossCategoryBack, kBossEntry = 0x58,
                  kBossLast = kBossEntry + 7 };

// BOSS TRACK's bosses, in the order the game has them. A boss fight's music is the music cue of its mission (the cue
// is the mission number; its song comes from the game's cue table, 0x00276EC0), and the player's song for it is
// kept in settings.toml (0: the game's own). Mission numbers: as the game numbers them (options + 0x340, and the
// MUSIC CUE lines in the log as each fight starts); episode 1 has missions 0 to 7, episode 2 starts at 8.
struct Boss {
    const char* name;          // its entry: "TRIAD THUG"
    const char* lower;         // in its large text: "triad thug: Bruises"
    int32_t mission;           // the mission with its fight
    const char* mission_name;  // "locked: beat TRIAD MUSCLE first"
    uint32_t app::Settings::*song;
};
const Boss kBosses[] = {
    {"TRIAD THUG", "triad thug", 4, "TRIAD MUSCLE", &app::Settings::boss_triad_thug},  // the game's: 13, Jay O Felony
    {"CARY", "cary", 9, "DOJO ATTACKER", &app::Settings::boss_cary},  // the game's: 43, Dee Dimes Feat. Bigg Swoop
    {"FENG", "feng", 11, "BAR BRAWL", &app::Settings::boss_feng},  // the game's: 70, NERD
    {"SNIPER", "sniper", 16, "SNIPER ASSASSIN", &app::Settings::boss_sniper},  // episode 2 mission 9; the game's: 66
};
constexpr uint32_t kBossCount = sizeof(kBosses) / sizeof(kBosses[0]);
static_assert(kBossCount <= kBossLast - kBossEntry + 1 && kBossCount <= kLabelCount - kLabelBossEntry);
uint32_t g_boss_current = 0;  // the boss whose song is being picked
uint32_t& BossSong(uint32_t i) { return app::Get().*kBosses[i].song; }
// the boss whose fight a music cue starts, or null
const Boss* BossForCue(int32_t cue) {
    for (const Boss& b : kBosses)
        if (b.mission == cue) return &b;
    return nullptr;
}
constexpr uint32_t kAudioRecord = TC_SEL(0x0028AD7Cu, 0x0028AAA8u);  // VOLUME MIX, SOUNDTRACK, RADIO DISPATCHER
std::atomic<bool> g_audio_entries{false};
// FPS choices: the runtime's frame cap (app::Pace reads it every frame, so a change applies at once)
constexpr uint32_t kFpsChoices[2] = {30, 60};

std::atomic<bool> g_display_entries{false};
std::mutex g_labels_lock;
uint32_t g_table = 0, g_labels = 0;  // the enlarged array and the label text, guest addresses
uint32_t g_screen = 0, g_mode_panel = 0, g_res_panel = 0, g_wide_panel = 0, g_fps_panel = 0;
// the screen mode this session started with: the game picks its frame once, at video init (see below)
std::atomic<bool> g_started_wide{false};
uint32_t g_main_page = 0, g_exit_screen = 0, g_exit_panel = 0;  // the main menu's list, its screen, EXIT's panel
// the EXIT panel's codes, through the main menu's callback 0x00142A90 (values past the screen numbers)
constexpr uint32_t kExitYes = 0x7154u, kExitNo = 0x7155u;
std::vector<app::Mode> g_res_modes;  // the resolution panel's choices, left to right

uint32_t LabelId(uint32_t slot) { return kGameStrings + slot; }

void EnsureLabels() {
    std::lock_guard lock(g_labels_lock);
    if (!g_labels) g_labels = mem::AllocSystem(kLabels * kLabelSize);
}

void WriteLabel(uint32_t slot, const std::string& text) {
    if (!g_labels) return;
    char buf[kLabelSize]{};
    std::memcpy(buf, text.data(), std::min<size_t>(text.size(), kLabelSize - 1));
    std::memcpy(::kernel::HostPtr(g_labels + slot * kLabelSize), buf, kLabelSize);
}

// SCREEN's large text: the choice, and that it needs a restart while it is not the mode the game started in
void ScreenLabels() {
    const char* names[2] = {"original", "widescreen"};
    for (uint32_t i = 0; i < 2; i++)
        WriteLabel(kLabelScreenBig + i, std::string(names[i]) + ((i == 1) != g_started_wide ? " - restart to apply" : ""));
}

void WriteTrackStrings(uint32_t slots);  // the PC soundtrack's lines (below)

void ExtendStrings() {
    EnsureLabels();
    std::lock_guard lock(g_labels_lock);
    uint32_t table = ::kernel::R32(kStringTable);
    if (!table || !g_labels) return;
    if (table != g_table) {
        uint32_t copy = mem::AllocSystem((kGameStrings + kLabels + kTrackSlots) * 4);
        if (!copy) return;
        std::memcpy(::kernel::HostPtr(copy), ::kernel::HostPtr(table), kGameStrings * 4);
        ::kernel::W32(kStringTable, copy);
        g_table = copy;
    }
    for (uint32_t i = 0; i < kLabels; i++) ::kernel::W32(g_table + (kGameStrings + i) * 4, g_labels + i * kLabelSize);
    WriteTrackStrings(g_table + (kGameStrings + kLabels) * 4);
    // the save messages name the console: in the English files only, they name the game instead (the
    // loader has turned the file's line breaks, 0x1B, into '\n' by now; 0x92 is the files' apostrophe)
    static uint32_t messages = 0;
    struct Message {
        uint32_t id;
        const char* text;
    };
    static const Message kMessages[] = {
        {39, "Saving game.\nPlease don\x92t close the game."},
        {43, "Saving game.\nPlease don\x92t close the game."},
        {54, "Auto-saving game.\nPlease don\x92t close the game."},
    };
    constexpr uint32_t kCount = sizeof(kMessages) / sizeof(kMessages[0]);
    if (!messages) messages = mem::AllocSystem(kCount * kMessageSize);
    for (uint32_t i = 0; messages && i < kCount; i++) {
        uint32_t text = ::kernel::R32(g_table + kMessages[i].id * 4);
        if (!text || text == messages + i * kMessageSize) continue;
        const char* str = static_cast<const char*>(::kernel::HostPtr(text));
        std::string old(str, strnlen(str, 128));
        if (old.find("Xbox console") == std::string::npos) continue;  // another language, or already changed
        char buf[kMessageSize]{};
        std::strncpy(buf, kMessages[i].text, kMessageSize - 1);
        std::memcpy(::kernel::HostPtr(messages + i * kMessageSize), buf, kMessageSize);
        ::kernel::W32(g_table + kMessages[i].id * 4, messages + i * kMessageSize);
    }
}

// appends entries to a layout record's entry array (a copy; the record is in the game's data)
bool AppendEntries(uint32_t record, uint32_t expected, const uint32_t (*extra)[5], uint32_t n) {
    uint32_t items = ::kernel::R32(record + 0x14), count = ::kernel::R32(record + 0x18);
    if (count != expected) return false;
    for (uint32_t i = 0; i < count; i++)
        if (::kernel::R32(items + i * 0x14) != 1) return false;
    uint32_t copy = mem::AllocSystem((count + n) * 0x14);
    if (!copy) return false;
    std::memcpy(::kernel::HostPtr(copy), ::kernel::HostPtr(items), count * 0x14);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < 5; k++) ::kernel::W32(copy + (count + i) * 0x14 + k * 4, extra[i][k]);
    ::kernel::W32(record + 0x14, copy);
    ::kernel::W32(record + 0x18, count + n);
    return true;
}

// calls guest code (thiscall with self, or self 0), every register and the stack put back
uint32_t Call(X86Context& ctx, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args) {
    X86Context saved = ctx;
    ctx.ecx = self;
    uint32_t r = ::kernel::CallGuest(ctx, fn, args);
    uint32_t esp = saved.esp;
    ctx = saved;
    ctx.esp = esp;
    return r;
}
uint32_t VFn(uint32_t obj, uint32_t offset) { return ::kernel::R32(::kernel::R32(obj) + offset); }

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

uint32_t ModeIndex() {  // the mode panel's order: windowed, fullscreen, borderless
    app::DisplayMode m = app::Get().display_mode;
    return m == app::DisplayMode::Windowed ? 0 : m == app::DisplayMode::Fullscreen ? 1 : 2;
}
app::Mode CurrentResolution() {
    const app::Settings& s = app::Get();
    return s.width && s.height ? app::Mode{s.width, s.height} : app::Desktop();
}
uint32_t ResIndex() {
    app::Mode cur = CurrentResolution();
    for (uint32_t i = 0; i < g_res_modes.size(); i++)
        if (g_res_modes[i].width == cur.width && g_res_modes[i].height == cur.height) return i;
    return 0;
}

// the resolution choices, left to right
void PickResolutions() {
    g_res_modes = {{800, 600}, {1024, 768}, {1280, 720}, {1600, 1200}, {1920, 1080}};
}

// a panel like the radar one: choice buttons left to right, each {label string, string shown large while
// it is selected, code}, the callback the buttons and the panel's back (code `back`) call with their code
struct Choice {
    uint32_t label, big, code;
};
uint32_t MakePanel(X86Context& ctx, uint32_t screen, const std::vector<Choice>& choices, uint32_t callback,
                   uint32_t back) {
    uint32_t panel = mem::AllocSystem(kPanelSize);
    if (!panel) return 0;
    std::memset(::kernel::HostPtr(panel), 0, kPanelSize);
    Call(ctx, kPanelCtor, panel, {});
    uint32_t list = panel + 0x1A8;
    for (auto it = choices.rbegin(); it != choices.rend(); ++it) {  // added in front: added last, shown first
        uint32_t widget = Call(ctx, kAddButton, list, {it->label, 0});
        if (!widget) continue;
        uint32_t button = widget + 0x74;
        ::kernel::W32(button + 4, callback);
        ::kernel::W32(button + 8, it->code);
        ::kernel::W32(button + 12, it->big);
    }
    Call(ctx, VFn(list, 0x2C), list, {0});
    ::kernel::W32(panel + 0x234, callback);
    ::kernel::W32(panel + 0x238, back);
    ::kernel::W32(panel + 0x23C, 0);
    Call(ctx, VFn(panel, 0x08), panel, {0});  // hidden until its entry is focused
    // into the screen's widget list, as the screen's constructor does for its own panels
    using ::kernel::R32;
    using ::kernel::W32;
    W32(kWidgetCount, R32(kWidgetCount) - 1);
    uint32_t node = R32(kWidgetNodes);
    W32(R32(node) + 4, R32(node + 4));
    W32(R32(node + 4), R32(node));
    W32(node + 8, panel);
    uint32_t head = R32(screen + 0x84), first = R32(head);
    W32(first + 4, node);
    W32(head, node);
    W32(node + 4, head);
    W32(node, first);
    *static_cast<uint8_t*>(::kernel::HostPtr(panel + 4)) = 1;  // as the constructor sets its panels
    return panel;
}

// a panel's choices are widgets in the list at [panel+0x88]+0x74 (next at +0, widget at +8); each one's
// +0x80 is the string shown large while it is selected, which names the choice here. The list's order is
// not the order on screen, so choices are found and read by that string, never by position.
uint32_t ChoiceWidget(uint32_t panel, uint32_t big) {
    uint32_t list = ::kernel::R32(panel + 0x88);
    for (uint32_t n = list ? ::kernel::R32(list + 0x74) : 0, guard = 0; n && guard < 32; n = ::kernel::R32(n), guard++)
        if (uint32_t w = ::kernel::R32(n + 8); w && ::kernel::R32(w + 0x80) == big) return w;
    return 0;
}
uint32_t SelectedBig(uint32_t panel) {  // the highlighted choice's string, 0 without one
    uint32_t list = ::kernel::R32(panel + 0x88);
    for (uint32_t n = list ? ::kernel::R32(list + 0x74) : 0, guard = 0; n && guard < 32; n = ::kernel::R32(n), guard++)
        if (uint32_t w = ::kernel::R32(n + 8); w && (::kernel::R32(w + 8) & 0x10400)) return ::kernel::R32(w + 0x80);
    return 0;
}
// highlights the choice with that string, as 0x00146480 does for an index (0x001463D0: thiscall, the widget)
void Highlight(X86Context& ctx, uint32_t panel, uint32_t big) {
    uint32_t list = ::kernel::R32(panel + 0x88);
    if (uint32_t w = list ? ChoiceWidget(panel, big) : 0) {
        ::kernel::W32(list + 8, ::kernel::R32(list + 8) | 0x40000);
        Call(ctx, kHighlight, list, {w});
    }
}
// the slot of a choice's large string within a group of labels, or -1
int ChoiceOf(uint32_t big, uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (LabelId(first + i) == big) return int(i);
    return -1;
}

void SyncPanels(X86Context& ctx) {
    if (g_mode_panel) Highlight(ctx, g_mode_panel, LabelId(kLabelModeBig + ModeIndex()));
    if (g_res_panel && !g_res_modes.empty()) Highlight(ctx, g_res_panel, LabelId(kLabelResBig + ResIndex()));
    if (g_wide_panel) Highlight(ctx, g_wide_panel, LabelId(kLabelScreenBig + (app::Get().wide_screen ? 1 : 0)));
    if (g_fps_panel) Highlight(ctx, g_fps_panel, LabelId(kLabelFpsBig + (app::Get().fps_cap == kFpsChoices[0] ? 0 : 1)));
}

uint32_t PanelFor(uint32_t code) {
    return code == kOpenMode   ? g_mode_panel
           : code == kOpenRes  ? g_res_panel
           : code == kOpenWide ? g_wide_panel
           : code == kOpenFps  ? g_fps_panel
                               : 0;
}

// the widget at a list index of the screen's entry list, 0 past its end
uint32_t EntryAt(uint32_t screen, uint32_t index) {
    uint32_t n = ::kernel::R32(::kernel::R32(screen + 0x88) + 0x74);
    for (uint32_t i = 0; n && i < index; i++) n = ::kernel::R32(n);
    return n ? ::kernel::R32(n + 8) : 0;
}
}  // namespace

X86_EXTERN_FUNC(TC_SEL(__imp__sub_00142520, __imp__sub_001424B0));
X86_FUNC(TC_SEL(sub_00142520, sub_001424B0)) {
    TC_SEL(__imp__sub_00142520, __imp__sub_001424B0)(ctx, base);
    ExtendStrings();
}

// the display screen's constructor: its panels for display mode and resolution
X86_EXTERN_FUNC(TC_SEL(__imp__sub_0015AA10, __imp__sub_0015A8B0));
X86_FUNC(TC_SEL(sub_0015AA10, sub_0015A8B0)) {
    uint32_t screen = ctx.ecx;
    TC_SEL(__imp__sub_0015AA10, __imp__sub_0015A8B0)(ctx, base);
    EnsureLabels();
    if (!g_labels) return;
    WriteLabel(kLabelMode, "DISPLAY MODE");
    WriteLabel(kLabelRes, "RESOLUTION");
    const char* modes[3] = {"WINDOWED", "FULLSCREEN", "BORDERLESS"};
    std::vector<Choice> mode_choices, res_choices;
    for (uint32_t i = 0; i < 3; i++) {
        WriteLabel(kLabelModeChoice + i, modes[i]);
        WriteLabel(kLabelModeBig + i, Lower(modes[i]));
        mode_choices.push_back({LabelId(kLabelModeChoice + i), LabelId(kLabelModeBig + i), kPickMode});
    }
    PickResolutions();
    for (uint32_t i = 0; i < g_res_modes.size(); i++) {
        std::string w = std::to_string(g_res_modes[i].width), h = std::to_string(g_res_modes[i].height);
        WriteLabel(kLabelResChoice + i, w + "X" + h);
        WriteLabel(kLabelResBig + i, w + " x " + h);
        res_choices.push_back({LabelId(kLabelResChoice + i), LabelId(kLabelResBig + i), kPickRes});
    }
    ExtendStrings();
    g_screen = screen;
    g_mode_panel = MakePanel(ctx, screen, mode_choices, kSubPanel, kBackMode);
    g_res_panel = MakePanel(ctx, screen, res_choices, kSubPanel, kBackRes);
    WriteLabel(kLabelScreen, "SCREEN");
    WriteLabel(kLabelScreenChoice, "ORIGINAL");
    WriteLabel(kLabelScreenChoice + 1, "WIDESCREEN");
    ScreenLabels();
    g_wide_panel = MakePanel(ctx, screen,
                             {{LabelId(kLabelScreenChoice), LabelId(kLabelScreenBig), kPickWide},
                              {LabelId(kLabelScreenChoice + 1), LabelId(kLabelScreenBig + 1), kPickWide}},
                             kSubPanel, kBackWide);
    WriteLabel(kLabelFps, "FPS");
    for (uint32_t i = 0; i < 2; i++) {
        WriteLabel(kLabelFpsChoice + i, std::to_string(kFpsChoices[i]));
        WriteLabel(kLabelFpsBig + i, std::to_string(kFpsChoices[i]) + " fps");
    }
    g_fps_panel = MakePanel(ctx, screen,
                            {{LabelId(kLabelFpsChoice), LabelId(kLabelFpsBig), kPickFps},
                             {LabelId(kLabelFpsChoice + 1), LabelId(kLabelFpsBig + 1), kPickFps}},
                            kSubPanel, kBackFps);
    SyncPanels(ctx);
    LOGR("tc: display screen %08X: mode panel %08X, resolution panel %08X (%zu resolutions)", screen, g_mode_panel,
         g_res_panel, g_res_modes.size());
}

// the display screen hides its radar and subtitles panels with 0x00154500 (thiscall, 1 arg) whenever it
// sets itself up; the new panels are hidden with them, so they only show while their entry is focused
X86_EXTERN_FUNC(TC_SEL(__imp__sub_00154500, __imp__sub_001543E0));
X86_FUNC(TC_SEL(sub_00154500, sub_001543E0)) {
    uint32_t screen = ctx.ecx;
    TC_SEL(__imp__sub_00154500, __imp__sub_001543E0)(ctx, base);
    if (screen != g_screen) return;
    for (uint32_t p : {g_mode_panel, g_res_panel, g_wide_panel, g_fps_panel})
        if (p) Call(ctx, VFn(p, 0x08), p, {0});
    SyncPanels(ctx);
}

// the display screen's dispatcher: the new panels' open, choice and back
X86_EXTERN_FUNC(TC_SEL(__imp__sub_001576B0, __imp__sub_00157590));
X86_FUNC(TC_SEL(sub_001576B0, sub_00157590)) {
    uint32_t screen = ctx.ecx, code = ::kernel::R32(ctx.esp + 4);
    // SUBTITLES choice (code 12). The game reads the choice back as the panel's highlighted position
    // (0x001555B0), which in this port does not match the choice taken (the same lookup gave the display
    // panels the wrong choices), so subtitles could not be turned on. The chosen button passes its large
    // string, "subtitles on" (0x1B8) or "subtitles off" (0x1B9): the setting at [0x002A4258]+0x992 is
    // taken from that, then the game's own steps follow. 1 is on: the dialogue subtitles at 0x00087DD0
    // are skipped when it is 0 (0x000CCC96 and 0x000CD7ED, which skip when it is not 0, are other text),
    // and the defaults at 0x00149DAB turn them on only for the languages past the first two.
    if (code == 0x0C) {
        uint32_t big = ::kernel::R32(ctx.esp + 8), opts = ::kernel::R32(kOptions);
        if (opts && (big == kSubtitlesOn || big == kSubtitlesOff)) {
            ::kernel::W16(opts + 0x992, big == kSubtitlesOn ? 1 : 0);
            LOGR("tc: subtitles %s", big == kSubtitlesOn ? "on" : "off");
            uint32_t panel = screen + 0xC64;  // the subtitles panel
            Call(ctx, kPlaySound, 0, {0xFF, kOne, kOne});
            Call(ctx, VFn(panel, 0x2C), panel, {0});
            Call(ctx, kPanelsDone, screen, {});  // also highlights the stored choice again
            ::kernel::Return(ctx, 1, 2);
            return;
        }
    }
    if (code < kOpenMode || code > kBackFps || screen != g_screen) {
        TC_SEL(__imp__sub_001576B0, __imp__sub_00157590)(ctx, base);
        return;
    }
    uint32_t group = (code - kOpenMode) / 3;  // 0 display mode, 1 resolution, 2 screen, 3 fps
    bool mode = group == 0;
    uint32_t panel = group == 0 ? g_mode_panel : group == 1 ? g_res_panel : group == 2 ? g_wide_panel : g_fps_panel;
    uint32_t step = (code - kOpenMode) % 3;  // 0 open, 1 choice, 2 back
    if (panel && step == 0) {
        SyncPanels(ctx);
        Call(ctx, kPlaySound, 0, {0xFF, kOne, kOne});
        Call(ctx, kScreenFocus, screen, {0});
        uint32_t list = screen + 0x2D8;
        Call(ctx, VFn(list, 0x08), list, {1});
        Call(ctx, VFn(panel, 0x28), panel, {0});
        ::kernel::W32(screen + 0x2D4, code);
    } else if (panel && step == 1) {
        // the chosen button passes its large string as the argument; the highlighted one's is the fallback
        uint32_t big = ::kernel::R32(ctx.esp + 8);
        uint32_t first = group == 0 ? kLabelModeBig : group == 1 ? kLabelResBig : group == 2 ? kLabelScreenBig : kLabelFpsBig;
        uint32_t count = group == 0 ? 3u : group == 1 ? uint32_t(g_res_modes.size()) : 2u;
        int choice = ChoiceOf(big, first, count);
        if (choice < 0) choice = ChoiceOf(SelectedBig(panel), first, count);
        uint32_t i = choice < 0 ? ~0u : uint32_t(choice);
        app::Settings& s = app::Get();
        if (group == 3) {
            if (i < 2) s.fps_cap = kFpsChoices[i];
            LOGR("tc: fps choice %d (string %u): %u fps", choice, big, s.fps_cap);
        } else if (group == 2) {
            // the 2D's shape on a widescreen output: stretched as the console shows it, or kept (gpu Squeeze)
            if (i < 2) s.wide_screen = i == 1;
            LOGR("tc: screen choice %d (string %u): %s", choice, big, s.wide_screen ? "widescreen" : "original");
        } else if (mode) {
            const app::DisplayMode modes[3] = {app::DisplayMode::Windowed, app::DisplayMode::Fullscreen,
                                               app::DisplayMode::Borderless};
            if (i < 3) s.display_mode = modes[i];
            LOGR("tc: display mode choice %d (string %u): mode %u", choice, big, uint32_t(s.display_mode));
        } else if (i < g_res_modes.size()) {
            app::Mode m = g_res_modes[i], desk = app::Desktop();
            bool desktop = m.width == desk.width && m.height == desk.height;
            s.width = desktop ? 0 : m.width;  // 0 keeps following the desktop, as the default does
            s.height = desktop ? 0 : m.height;
            LOGR("tc: resolution choice %d (string %u): %ux%u", choice, big, m.width, m.height);
        }
        if (group != 3) app::ApplyDisplay();  // the frame cap needs no window change
        app::Save();
        Call(ctx, kPlaySound, 0, {0xFF, kOne, kOne});
        Call(ctx, VFn(panel, 0x2C), panel, {0});
        Call(ctx, kPanelsDone, screen, {});
        SyncPanels(ctx);
    } else if (panel) {
        Call(ctx, kPlaySound, 0, {0x100, kOne, kOne});
        Call(ctx, VFn(panel, 0x2C), panel, {0});
        Call(ctx, kPanelsDone, screen, {});
        SyncPanels(ctx);
    }
    ::kernel::Return(ctx, 1, 2);  // al = 1, ret 8
}

// the display screen's focus change: the focused new entry shows its panel, the one left hides it
X86_EXTERN_FUNC(TC_SEL(__imp__sub_001578B0, __imp__sub_00157790));
X86_FUNC(TC_SEL(sub_001578B0, sub_00157790)) {
    uint32_t screen = ctx.ecx, old_index = ::kernel::R32(ctx.esp + 4), new_index = ::kernel::R32(ctx.esp + 8);
    TC_SEL(__imp__sub_001578B0, __imp__sub_00157790)(ctx, base);
    if (screen != g_screen) return;
    if (uint32_t w = EntryAt(screen, old_index))
        if (uint32_t p = PanelFor(::kernel::R32(w + 0x7C))) Call(ctx, VFn(p, 0x08), p, {1});
    if (uint32_t w = EntryAt(screen, new_index))
        if (uint32_t p = PanelFor(::kernel::R32(w + 0x7C))) {
            SyncPanels(ctx);  // the setting may have changed outside the menu (Alt+Enter)
            Call(ctx, VFn(p, 0x0C), p, {1});
        }
}

X86_EXTERN_FUNC(TC_SEL(__imp__sub_00147060, __imp__sub_00146FE0));
X86_FUNC(TC_SEL(sub_00147060, sub_00146FE0)) {
    if (::kernel::R32(ctx.esp + 4) == kMainMenuRecord) g_main_page = ctx.ecx;  // the list the EXIT panel replaces
    if (::kernel::R32(ctx.esp + 4) == kMainMenuRecord && !g_quit_entry.exchange(true)) {
        uint32_t items = ::kernel::R32(kMainMenuRecord + 0x14), count = ::kernel::R32(kMainMenuRecord + 0x18);
        bool expected = count == 3;
        for (uint32_t i = 0; expected && i < 3; i++)
            expected = ::kernel::R32(items + i * 0x14) == 1 && ::kernel::R32(items + i * 0x14 + 8) == kOpenScreen;
        uint32_t copy = expected ? mem::AllocSystem(4 * 0x14) : 0;
        if (copy) {
            std::memcpy(::kernel::HostPtr(copy), ::kernel::HostPtr(items), 3 * 0x14);
            uint32_t e = copy + 3 * 0x14;
            ::kernel::W32(e, 1);
            ::kernel::W32(e + 4, kExitString);
            ::kernel::W32(e + 8, kOpenScreen);
            ::kernel::W32(e + 12, kQuitValue);
            ::kernel::W32(e + 16, 0);
            ::kernel::W32(kMainMenuRecord + 0x14, copy);
            ::kernel::W32(kMainMenuRecord + 0x18, 4);
            LOGR("tc: main menu has an EXIT entry");
        } else {
            LOGR("tc: main menu record is not as expected, no EXIT entry added");
        }
    }
    if (::kernel::R32(ctx.esp + 4) == kAudioRecord && !g_audio_entries) {
        ExtendStrings();
        if (g_table) {
            g_audio_entries = true;
            WriteLabel(kLabelBoss, "BOSS TRACK");
            const uint32_t extra[1][5] = {{1, LabelId(kLabelBoss), kSubPanel, kOpenBoss, 0}};
            if (AppendEntries(kAudioRecord, 3, extra, 1))
                LOGR("tc: audio screen has a BOSS TRACK entry");
            else
                LOGR("tc: audio screen record is not as expected, no BOSS TRACK entry added");
        }
    }
    if (::kernel::R32(ctx.esp + 4) == kDisplayRecord && !g_display_entries) {
        ExtendStrings();
        if (g_table) {  // the language file is in: the entries' text has somewhere to live
            g_display_entries = true;
            WriteLabel(kLabelMode, "DISPLAY MODE");
            WriteLabel(kLabelRes, "RESOLUTION");
            WriteLabel(kLabelScreen, "SCREEN");
            WriteLabel(kLabelFps, "FPS");
            const uint32_t extra[4][5] = {{1, LabelId(kLabelMode), kSubPanel, kOpenMode, 0},
                                          {1, LabelId(kLabelRes), kSubPanel, kOpenRes, 0},
                                          {1, LabelId(kLabelScreen), kSubPanel, kOpenWide, 0},
                                          {1, LabelId(kLabelFps), kSubPanel, kOpenFps, 0}};
            if (AppendEntries(kDisplayRecord, 2, extra, 4))
                LOGR("tc: display screen has DISPLAY MODE, RESOLUTION, SCREEN and FPS entries");
            else
                LOGR("tc: display screen record is not as expected, no display entries added");
        }
    }
    TC_SEL(__imp__sub_00147060, __imp__sub_00146FE0)(ctx, base);
}

X86_EXTERN_FUNC(TC_SEL(__imp__sub_00142A90, __imp__sub_001429F0));
X86_FUNC(TC_SEL(sub_00142A90, sub_001429F0)) {
    uint32_t value = ::kernel::R32(ctx.esp + 4);
    if (value == kQuitValue || value == kExitYes || value == kExitNo) {
        // the callback's object holds the current screen at +0 (0x00142AB0 dispatches through it)
        uint32_t screen = ::kernel::R32(ctx.ecx);
        bool usable = screen && ::kernel::R32(screen + 0x84) && ::kernel::R32(screen + 0x88) && g_main_page;
        if (value == kQuitValue && usable) {
            if (!g_exit_panel || g_exit_screen != screen) {
                EnsureLabels();
                WriteLabel(kLabelExitYesBig, "exit game");
                WriteLabel(kLabelExitNoBig, "exit game");
                ExtendStrings();
                g_exit_screen = screen;
                g_exit_panel = MakePanel(ctx, screen,
                                         {{7, LabelId(kLabelExitYesBig), kExitYes},   // "YES"
                                          {8, LabelId(kLabelExitNoBig), kExitNo}},    // "NO"
                                         kOpenScreen, kExitNo);
                if (uint32_t panel = g_exit_panel) {
                    // no "SETTINGS" header: the panel's header widget (+0x90) gets the empty string 1
                    Call(ctx, kSetText, panel + 0x90, {1});
                    // the main menu shows all its widgets whenever it comes back (from OPTIONS): this panel
                    // gets its own copy of its class's vtable with "show" (+0x0C) a do-nothing, and is shown
                    // here by calling the class's show directly
                    uint32_t vt = mem::AllocSystem(kVtableCopy * 4);
                    if (vt) {
                        std::memcpy(::kernel::HostPtr(vt), ::kernel::HostPtr(::kernel::R32(panel)), kVtableCopy * 4);
                        ::kernel::W32(vt + 0x0C, kNoOp4);
                        ::kernel::W32(panel, vt);
                    }
                }
                LOGR("tc: EXIT panel %08X on screen %08X", g_exit_panel, screen);
            }
        }
        uint32_t panel = g_exit_panel;
        if (value == kQuitValue && usable && panel) {  // ARE YOU SURE: the panel in place of the menu, on NO
            Highlight(ctx, panel, LabelId(kLabelExitNoBig));
            Call(ctx, kPlaySound, 0, {0xFF, kOne, kOne});
            Call(ctx, kScreenFocus, screen, {0});
            Call(ctx, VFn(g_main_page, 0x08), g_main_page, {1});
            Call(ctx, kPanelShow, panel, {1});
            Call(ctx, VFn(panel, 0x28), panel, {0});
        } else if (value == kExitNo && panel) {  // NO or back: the menu again
            Call(ctx, kPlaySound, 0, {0x100, kOne, kOne});
            Call(ctx, VFn(panel, 0x2C), panel, {0});
            Call(ctx, VFn(panel, 0x08), panel, {1});
            Call(ctx, VFn(g_main_page, 0x0C), g_main_page, {1});
            Call(ctx, kScreenUnfocus, g_exit_screen, {0});
        } else {  // YES, or no panel could be made: quit
            if (value == kExitYes) Call(ctx, kPlaySound, 0, {0xFF, kOne, kOne});
            LOGR("tc: EXIT chosen on the main menu, quitting");
            app::RequestQuit();
        }
        ::kernel::Return(ctx, 1, 2);  // thiscall, ret 8: handled
        return;
    }
    TC_SEL(__imp__sub_00142A90, __imp__sub_001429F0)(ctx, base);
}

X86_EXTERN_FUNC(TC_SEL(__imp__sub_0014B350, __imp__sub_0014B270));
X86_FUNC(TC_SEL(sub_0014B350, sub_0014B270)) {
    if (::kernel::R32(ctx.esp + 4) == 0x29) {
        LOGR("tc: exit to dashboard chosen, quitting");
        app::RequestQuit();
        ::kernel::Return(ctx, 0, 2);  // thiscall, ret 8
        return;
    }
    TC_SEL(__imp__sub_0014B350, __imp__sub_0014B270)(ctx, base);
}

// =============================================================================================
// SCREEN widescreen: the game's own HD widescreen layout
//
// The game picks its video mode once, at video init (0x001D18F0), from XGetVideoFlags (0x001E992E): with
// the 720p bit (2) it sets up a 1280x720 frame and its widescreen flag, and lays its menus and HUD out for that
// frame (the HUD's x mapping, 0x000BEBE0, takes its widescreen path for widths past 640) instead of the 640x480
// layout a 16:9 output stretches. The XAPI only reports HD modes for an NTSC console with the HDTV AV pack,
// which this PAL title never sees, so with SCREEN set to widescreen on a widescreen output the flags are
// reported as widescreen, 720p and 480p here. ORIGINAL keeps the console's own answer. Read once: a change
// applies from the next start.
// =============================================================================================
X86_EXTERN_FUNC(TC_SEL(__imp__sub_001E992E, __imp__sub_001E978E));
X86_FUNC(TC_SEL(sub_001E992E, sub_001E978E)) {
    if (app::Get().wide_screen && app::Widescreen()) {
        constexpr uint32_t kWidescreen = 0x01, kHdtv720p = 0x02, kHdtv480p = 0x08;
        LOGR("tc: video flags reported as widescreen 720p (SCREEN widescreen)");
        g_started_wide = true;
        ::kernel::Return(ctx, kWidescreen | kHdtv720p | kHdtv480p, 0);
        return;
    }
    TC_SEL(__imp__sub_001E992E, __imp__sub_001E978E)(ctx, base);
}

// =============================================================================================
// SCREEN widescreen, main menu: tilt the backdrop camera up a little
//
// The main menu's city backdrop cycles through fixed camera spots. At some of them the bottom of the
// view looks past the edge of the street model, and the clear colour shows as a black band under the
// "floating" street. The 720p widescreen view is a little taller than the 4:3 one, so the band shows there.
// 0x001D3EF0 (thiscall: camera-to-world matrix; ret 4) hands the scene view its camera. Its call from the
// scene render (0x000D7F04, returning to 0x000D7F09) uses the active camera; while the game is in its
// main-menu state (0x002AEE58 == 5, set when the title menu opens and left when play starts) and the game
// started in its widescreen mode, that call gets a copy of the matrix pitched up by kMenuPitchDegrees about
// the camera's right axis, so the street's edge falls below the screen. The camera itself is not changed,
// and culling follows the tilted view. ORIGINAL and gameplay (cut-scenes too) are not touched.
// The matrix rows are right, up, forward, position; the sign of the turn was set from captures before and after.
// =============================================================================================
namespace {
constexpr uint32_t kGameState = TC_SEL(0x002AEE58, 0x002AEB78);  // 5 while the title's main menu runs
constexpr uint32_t kStateMainMenu = 5;
constexpr uint32_t kSceneSetCameraRet = TC_SEL(0x000D7F09, 0x000D7F99);
constexpr float kMenuPitchDegrees = 5.0f;  // the street's edge sits about 5-6 deg below level at the worst spots
uint32_t g_menu_cam = 0;  // the tilted copy the view gets
}  // namespace

X86_EXTERN_FUNC(TC_SEL(__imp__sub_001D3EF0, __imp__sub_001D3D50));
X86_FUNC(TC_SEL(sub_001D3EF0, sub_001D3D50)) {
    using ::kernel::R32;
    if (g_started_wide && R32(ctx.esp) == kSceneSetCameraRet && R32(kGameState) == kStateMainMenu) {
        uint32_t src = R32(ctx.esp + 4);
        float m[16];
        std::memcpy(m, ::kernel::HostPtr(src), sizeof(m));
        float* r = m + 0;  // right
        float* u = m + 4;  // up
        float* f = m + 8;  // forward
        // only for a sane camera: unit axes and an upright up vector
        auto len = [](const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
        bool sane = std::fabs(len(r) - 1.f) < 0.05f && std::fabs(len(u) - 1.f) < 0.05f &&
                    std::fabs(len(f) - 1.f) < 0.05f && u[1] > 0.3f;
        if (sane) {
            if (!g_menu_cam) g_menu_cam = mem::AllocSystem(sizeof(m), 16);
            const float a = kMenuPitchDegrees * 3.14159265f / 180.f, c = std::cos(a), s = std::sin(a);
            for (int i = 0; i < 3; i++) {
                float fi = f[i], ui = u[i];
                f[i] = fi * c - ui * s;  // turns the view up (checked against captures: + turned it down)
                u[i] = ui * c + fi * s;
            }
            std::memcpy(::kernel::HostPtr(g_menu_cam), m, sizeof(m));
            ::kernel::W32(ctx.esp + 4, g_menu_cam);
            TC_LOG_FIRST(1, "tc: main menu camera tilted up %.1f deg (SCREEN widescreen)", kMenuPitchDegrees);
        } else {
            TC_LOG_FIRST(3, "tc: main menu camera not tilted, unexpected matrix r(%.2f %.2f %.2f) u(%.2f %.2f %.2f) f(%.2f %.2f %.2f)",
                         r[0], r[1], r[2], u[0], u[1], u[2], f[0], f[1], f[2]);
        }
    }
    TC_SEL(__imp__sub_001D3EF0, __imp__sub_001D3D50)(ctx, base);
}

// =============================================================================================
// [2.0] PC soundtrack: the 32 rock tracks the PC version added, in the Cruising list as on PC
//
// Nothing of them ships with the port. import_pc_tracks.bat converts a PC install's Data\Music\084.bik to 115.bik
// into the Xbox's song format, D:\Data\Music\084.WMA and on, and copies their lines from the PC's track list
// ("084Them Bones|Alice In Chains") into D:\pc_tracks.txt. Both go to assets\extras, which the file system lays
// over the game's D: (kernel/files.cpp). With those files there, this adds the songs in three places:
//
// - Picking a song (0x0007FF10, cdecl (current, list), returns a song number, 0 for none): the game counts the
//   songs of a list over its 83 (category bytes at 0x002770B0, 2 Cruising, 1 Fast Action, 0 Slow Action; the
//   on/off switches at options + 0x9F8 + song, room for 128), leaves out the song playing unless it is the only
//   one, and picks at random from those switched on (all of them when none is). The same is done here over 83
//   and the PC songs, which are Cruising songs as the PC version made them (its category table has 2 for
//   84..115). The Xbox's own custom soundtracks, from the console's hard disk, keep the game's code.
// - The TRACK LIST screen (0x00156F90, thiscall (screen)): it fills its rows at +0x320, 12 bytes each (switch
//   byte, string id, song), from the language file's lines for the list, then the hard disk's tracks, and keeps
//   the count at +0x318 (+0x2D8 is the list: 0 Cruising). The PC songs' rows go after the Cruising songs, below
//   "Yu Who | Kam", as on PC, with string ids past the language file's (ExtendStrings).
// - Playing it: the song's file is D:\Data\Music\%03d.WMA, the PC songs' too; their switches are in the 128.
// =============================================================================================
namespace {
constexpr uint32_t kGameSongs = 83, kMaxSong = 127;
constexpr uint32_t kSongCategory = TC_SEL(0x002770B0u, 0x00276DF0u);  // byte per song, song 1 first
constexpr uint32_t kSongSwitches = 0x9F8;  // options block + song
constexpr uint8_t kCruising = 2;
constexpr uint32_t kTrackRow = 12, kTrackRows = 0x320, kTrackCount = 0x318, kTrackListKind = 0x2D8;
constexpr uint32_t kMaxRows = 160;  // rows kept within what the screen has room for (it takes the hard disk's too)

struct PcTrack {
    uint32_t song;
    std::string line;  // as the language file has it: "084Them Bones|Alice In Chains"
    uint32_t text = 0;  // guest copy
};
std::vector<PcTrack> g_pc_tracks;
std::once_flag g_pc_once;

void LoadPcTracks() {
    std::call_once(g_pc_once, [] {
        std::vector<uint8_t> file;
        if (!::kernel::ReadGameFile("D:\\pc_tracks.txt", file)) return;
        std::string all(file.begin(), file.end());
        size_t at = 0;
        while (at < all.size() && g_pc_tracks.size() < kTrackSlots) {
            size_t end = all.find('\n', at);
            if (end == std::string::npos) end = all.size();
            std::string line = all.substr(at, end - at);
            at = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.size() < 5 || !isdigit(uint8_t(line[0])) || !isdigit(uint8_t(line[1])) || !isdigit(uint8_t(line[2])))
                continue;
            uint32_t song = uint32_t(std::stoi(line.substr(0, 3)));
            if (song <= kGameSongs || song > kMaxSong) continue;
            char name[32];
            std::snprintf(name, sizeof(name), "D:\\Data\\Music\\%03u.WMA", song);
            std::string host = ::kernel::ResolvePath(name, 0);
            std::error_code ec;
            if (host.empty() || !std::filesystem::is_regular_file(host, ec)) {
                LOGR("tc: PC soundtrack: %s is missing, song %u left out", name, song);
                continue;
            }
            PcTrack t{song, line};
            t.text = mem::AllocSystem(uint32_t(line.size() + 1));
            if (!t.text) continue;
            std::memcpy(::kernel::HostPtr(t.text), line.c_str(), line.size() + 1);
            g_pc_tracks.push_back(std::move(t));
        }
        LOGR("tc: PC soundtrack: %zu tracks added to Cruising", g_pc_tracks.size());
    });
}

uint8_t Byte(uint32_t va) { return *static_cast<const uint8_t*>(::kernel::HostPtr(va)); }

// a song's list: the game's table for its own, Cruising for the PC's, none for the rest
int SongList(uint32_t song) {
    if (song >= 1 && song <= kGameSongs) return Byte(kSongCategory + song - 1);
    for (const PcTrack& t : g_pc_tracks)
        if (t.song == song) return kCruising;
    return -1;
}
void WriteTrackStrings(uint32_t slots) {
    LoadPcTracks();
    for (uint32_t i = 0; i < g_pc_tracks.size(); i++) ::kernel::W32(slots + i * 4, g_pc_tracks[i].text);
}
}  // namespace

namespace {
// seconds since the first music event, so log lines can be matched to what was on screen
double MusicClock() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
}  // namespace

X86_EXTERN_FUNC(TC_SEL(__imp__sub_0007FF10, __imp__sub_0007FF50));
X86_FUNC(TC_SEL(sub_0007FF10, sub_0007FF50)) {
    LoadPcTracks();
    uint32_t ret = ::kernel::R32(ctx.esp);
    uint32_t options = ::kernel::R32(kOptions);
    int32_t current = int32_t(::kernel::R32(ctx.esp + 4));
    uint8_t list = Byte(ctx.esp + 8);
    uint32_t custom = list == 2 ? ::kernel::R32(options + 0x1078) : list == 1 ? ::kernel::R32(options + 0x107C)
                    : list == 0 ? ::kernel::R32(options + 0x1080) : 0;
    // the console's hard disk tracks, or nothing of the PC's to add: the game's own code
    if (g_pc_tracks.empty() || !options || custom || current >= 0x40000000) {
        TC_SEL(__imp__sub_0007FF10, __imp__sub_0007FF50)(ctx, base);
        return;
    }
    std::vector<uint32_t> songs;
    for (uint32_t s = 1; s <= kGameSongs; s++) songs.push_back(s);
    for (const PcTrack& t : g_pc_tracks) songs.push_back(t.song);
    uint32_t total = 0, on = 0;
    for (uint32_t s : songs) {
        if (SongList(s) != list) continue;
        total++;
        if (Byte(options + kSongSwitches + s)) on++;
    }
    int32_t choices = int32_t(on ? on : total);
    // the song playing is left out, unless it is the only one
    if (current <= 0 || choices < 1 || (choices == 1) || SongList(uint32_t(current)) != list) current = -1;
    else choices--;
    if (choices <= 0) {
        TC_SEL(__imp__sub_0007FF10, __imp__sub_0007FF50)(ctx, base);
        return;
    }
    static std::mt19937 rng{std::random_device{}()};
    int32_t pick = int32_t(std::uniform_int_distribution<int32_t>(0, choices - 1)(rng));
    uint32_t result = 0;
    for (uint32_t s : songs) {
        if (SongList(s) != list || int32_t(s) == current) continue;
        if (on && !Byte(options + kSongSwitches + s)) continue;
        if (pick-- == 0) {
            result = s;
            break;
        }
    }
    TC_LOG_FIRST(300, "tc: [%7.1fs] song picked for list %u: %u (of %d), from %08X", MusicClock(), list, result, choices, ret);
    ::kernel::Return(ctx, result, 0);
}

namespace {
// BOSS TRACK (below): the track list opened from it picks a song instead of switching songs on and off
std::atomic<bool> g_boss_assign{false};
uint32_t g_track_screen = 0;

void AddPcRows(uint32_t screen) {
    LoadPcTracks();
    if (g_pc_tracks.empty() || ::kernel::R32(screen + kTrackListKind) != 0) return;  // Cruising only
    ExtendStrings();
    if (!g_table) return;
    uint32_t options = ::kernel::R32(kOptions);
    uint32_t count = ::kernel::R32(screen + kTrackCount);
    uint32_t n = uint32_t(g_pc_tracks.size());
    if (!options || count + n > kMaxRows) return;
    // after the last of the game's songs (string ids above 0); the hard disk's rows move down
    uint32_t after = 0;
    for (uint32_t i = 0; i < count; i++)
        if (int32_t(::kernel::R32(screen + kTrackRows + i * kTrackRow + 4)) > 0) after = i + 1;
    uint32_t rows = screen + kTrackRows;
    std::memmove(::kernel::HostPtr(rows + (after + n) * kTrackRow), ::kernel::HostPtr(rows + after * kTrackRow),
                 (count - after) * kTrackRow);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t row = rows + (after + i) * kTrackRow;
        uint32_t song = g_pc_tracks[i].song;
        ::kernel::W32(row, Byte(options + kSongSwitches + song) ? 1u : 0u);
        ::kernel::W32(row + 4, kGameStrings + kLabels + i);
        ::kernel::W32(row + 8, song);
    }
    ::kernel::W32(screen + kTrackCount, count + n);
    TC_LOG_FIRST(2, "tc: TRACK LIST (Cruising): %u PC tracks after row %u, %u rows", n, after, count + n);
}
}  // namespace

X86_EXTERN_FUNC(TC_SEL(__imp__sub_00156F90, __imp__sub_00156E70));
X86_FUNC(TC_SEL(sub_00156F90, sub_00156E70)) {
    uint32_t screen = ctx.ecx;
    TC_SEL(__imp__sub_00156F90, __imp__sub_00156E70)(ctx, base);
    g_track_screen = screen;
    AddPcRows(screen);
    if (g_boss_assign) {
        // picking for BOSS TRACK: the tick shows the song picked, the header says what for
        uint32_t song = BossSong(g_boss_current), count = ::kernel::R32(screen + kTrackCount);
        for (uint32_t i = 0; i < count && i < kMaxRows + 32; i++) {
            uint32_t row = screen + kTrackRows + i * kTrackRow;
            *static_cast<uint8_t*>(::kernel::HostPtr(row)) = song && ::kernel::R32(row + 8) == song ? 1 : 0;
        }
        Call(ctx, kSetText, screen + 0x1FB0, {LabelId(kLabelBossHeader)});
        LOGR("tc: BOSS TRACK: track list %u opened to pick %s's song (now %u)", ::kernel::R32(screen + kTrackListKind),
             kBosses[g_boss_current].name, song);
    }
}


// =============================================================================================
// Music cues (diagnostic for BOSS TRACK)
//
// thiscall music.Cue(cue, fade): the game's music player (a global) starts the music for a cue.
// Cue -1 is free roam; a mission's cue is its mission number, and a table of 123 cues gives each
// its song (or -1 / -2 for a Fast Action / Slow Action pick, 0 for ambience). This logs every cue
// with its caller, the mission running (options + 0x340) and the song the player ended up on, so
// one run of TRIAD MUSCLE shows where the fist fight's music comes from.
// =============================================================================================
// =============================================================================================
// Song caption: the PC version names a boss fight's song at the bottom of the screen, the artist over the title,
// as the fight starts. The recompiled game has no such text, so it is drawn over the frame with the runtime's
// ImGui (GameInfo::ui, game_info.cpp), in Arial Bold (loaded when the window is up, GameInfo::window), white with
// a dark outline, about 5 seconds, fading in and out.
// =============================================================================================
namespace {
std::mutex g_caption_lock;
std::string g_caption_artist, g_caption_title;
std::chrono::steady_clock::time_point g_caption_start;
bool g_caption_on = false;
ImFont* g_caption_font = nullptr;
constexpr float kCaptionFadeIn = 0.4f, kCaptionHold = 5.0f, kCaptionFadeOut = 1.0f;

float FadeAlpha(float t, float in, float hold, float out) {
    if (t < in) return t / in;
    if (t < in + hold) return 1.0f;
    return std::max(0.0f, 1.0f - (t - in - hold) / out);
}

// one line centred across the screen, with a dark outline
void CenteredText(ImDrawList* draw, ImFont* font, float size, float y, ImU32 color, float alpha, const std::string& text) {
    if (text.empty()) return;
    float w = ImGui::GetIO().DisplaySize.x;
    ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    float x = std::floor((w - ext.x) / 2);
    float outline = std::max(1.0f, size / 18.0f);
    ImU32 dark = IM_COL32(0, 0, 0, int(200 * alpha));
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (dx || dy) draw->AddText(font, size, ImVec2(x + dx * outline, y + dy * outline), dark, text.c_str());
    draw->AddText(font, size, ImVec2(x, y), color, text.c_str());
}
}  // namespace



void TcShowSongCaption(const std::string& artist, const std::string& title) {
    std::lock_guard lock(g_caption_lock);
    g_caption_artist = artist;
    g_caption_title = title;
    g_caption_start = std::chrono::steady_clock::now();
    g_caption_on = true;
    LOGR("tc: song caption: %s / %s", artist.c_str(), title.c_str());
}

// the game window is up, before the first frame: the caption's font, while the font atlas can still take one
void TcWindow(void*) {
    std::lock_guard ui(app::UiLock());
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    const char* paths[] = {"C:\\Windows\\Fonts\\arialbd.ttf", "C:\\Windows\\Fonts\\arial.ttf"};
    for (const char* path : paths) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) continue;
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 2;
        g_caption_font = io.Fonts->AddFontFromFileTTF(path, 64.0f, &config);
        if (g_caption_font) break;
    }
    LOGR("tc: song caption font %s", g_caption_font ? "loaded" : "missing, ImGui's own is used");
}

// each frame, inside the ImGui frame: the song caption
void TcUi(double) {
    std::string artist, title;
    float caption = 0;
    {
        std::lock_guard lock(g_caption_lock);
        auto now = std::chrono::steady_clock::now();
        if (g_caption_on) {
            float t = std::chrono::duration<float>(now - g_caption_start).count();
            if (t >= kCaptionFadeIn + kCaptionHold + kCaptionFadeOut) g_caption_on = false;
            else caption = FadeAlpha(t, kCaptionFadeIn, kCaptionHold, kCaptionFadeOut);
            artist = g_caption_artist;
            title = g_caption_title;
        }
    }
    if (caption <= 0) return;
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = g_caption_font ? g_caption_font : ImGui::GetFont();
    float h = io.DisplaySize.y;
    if (h <= 0 || io.DisplaySize.x <= 0) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (caption > 0) {
        float size = h * 0.036f, gap = size * 1.15f, bottom_y = h * 0.93f;
        ImU32 white = IM_COL32(255, 255, 255, int(255 * caption));
        if (!artist.empty()) {
            CenteredText(draw, font, size, bottom_y - 2 * gap, white, caption, artist);
            CenteredText(draw, font, size, bottom_y - gap, white, caption, title);
        } else {
            CenteredText(draw, font, size, bottom_y - gap, white, caption, title);
        }
    }
}

bool TcMissionSolved(int32_t mission);  // BOSS TRACK, below

namespace {
constexpr uint32_t kCueTable = TC_SEL(0x00276EC0u, 0x00276C00u), kCueCount = 123;  // {int16 cue, int16 song}

uint32_t CueEntry(int32_t cue) {
    for (uint32_t i = 0; i < kCueCount; i++) {
        int16_t key;
        std::memcpy(&key, ::kernel::HostPtr(kCueTable + i * 4), 2);
        if (key == cue) return kCueTable + i * 4;
    }
    return 0;
}

// a song's line in the track lists ("074(Not Just) Knee Deep|Funkadelic"), the PC's for the PC tracks
std::string SongLine(uint32_t song) {
    for (const PcTrack& t : g_pc_tracks)
        if (t.song == song) return t.line;
    uint32_t table = ::kernel::R32(kStringTable);
    for (uint32_t id = 0x152; table && id < 0x152 + 160 && id < kGameStrings; id++) {
        uint32_t text = ::kernel::R32(table + id * 4);
        if (!text) continue;
        const char* str = static_cast<const char*>(::kernel::HostPtr(text));
        std::string l(str, strnlen(str, 96));
        if (l.size() > 3 && isdigit(uint8_t(l[0])) && isdigit(uint8_t(l[1])) && isdigit(uint8_t(l[2])) &&
            uint32_t(std::stoi(l.substr(0, 3))) == song)
            return l;
    }
    return {};
}

bool SongFileExists(uint32_t song) {
    char name[32];
    std::snprintf(name, sizeof(name), "D:\\Data\\Music\\%03u.WMA", song);
    std::string host = ::kernel::ResolvePath(name, 0);
    std::error_code ec;
    return !host.empty() && std::filesystem::is_regular_file(host, ec);
}

// the song BOSS TRACK puts on a cue, 0 for the game's own: only in a save where the boss is unlocked (its
// mission solved), as the menu has it; a save that has not got there plays the game's own music
uint32_t BossSongFor(int32_t cue) {
    const Boss* boss = BossForCue(cue);
    if (!boss) return 0;
    uint32_t song = app::Get().*boss->song;
    if (song < 1 || song > kMaxSong) return 0;
    if (!TcMissionSolved(boss->mission)) {
        LOGR("tc: BOSS TRACK: %s unsolved in this save, the game's own fight music plays", boss->mission_name);
        return 0;
    }
    if (!SongFileExists(song)) {
        TC_LOG_FIRST(4, "tc: BOSS TRACK: song %u has no file, the game's own music plays", song);
        return 0;
    }
    return song;
}
}  // namespace

X86_EXTERN_FUNC(TC_SEL(__imp__sub_00080E20, __imp__sub_00080E60));
X86_FUNC(TC_SEL(sub_00080E20, sub_00080E60)) {
    uint32_t music = ctx.ecx;
    uint32_t ret = ::kernel::R32(ctx.esp);
    int32_t cue = int32_t(::kernel::R32(ctx.esp + 4));
    uint32_t fade_bits = ::kernel::R32(ctx.esp + 8);
    float fade;
    std::memcpy(&fade, &fade_bits, 4);
    uint32_t options = ::kernel::R32(kOptions);
    int32_t mission = options ? int32_t(::kernel::R32(options + 0x340)) : -99;
    uint32_t entry = 0, forced = BossSongFor(cue);
    int16_t old_song = 0;
    uint8_t old_switch = 0;
    if (forced && options) entry = CueEntry(cue);
    if (entry) {
        // the cue's song for this one call, switched on so the game takes it as it is
        std::memcpy(&old_song, ::kernel::HostPtr(entry + 2), 2);
        old_switch = Byte(options + kSongSwitches + forced);
        int16_t song = int16_t(forced);
        std::memcpy(::kernel::HostPtr(entry + 2), &song, 2);
        *static_cast<uint8_t*>(::kernel::HostPtr(options + kSongSwitches + forced)) = 1;
    }
    TC_SEL(__imp__sub_00080E20, __imp__sub_00080E60)(ctx, base);
    if (entry) {
        std::memcpy(::kernel::HostPtr(entry + 2), &old_song, 2);
        *static_cast<uint8_t*>(::kernel::HostPtr(options + kSongSwitches + forced)) = old_switch;
        LOGR("tc: BOSS TRACK: song %u for the %s fight (the game's is %d)", forced, BossForCue(cue)->name,
             int32_t(old_song));
    }
    uint32_t source = ::kernel::R32(music + 0x124);
    uint32_t kind = source ? ::kernel::R32(source) : 0;
    uint32_t song = source ? ::kernel::R32(source + 8) : 0;
    // a boss fight's song, the player's or the game's, named at the bottom of the screen as the PC version does
    if (BossForCue(cue) && song >= 1 && song <= kMaxSong) {
        std::string line = SongLine(song);
        if (line.size() > 3) {
            std::string title = line.substr(3), artist;
            if (size_t bar = title.find('|'); bar != std::string::npos) {
                artist = title.substr(bar + 1);
                title.resize(bar);
            }
            TcShowSongCaption(artist, title);
        }
    }
    TC_LOG_FIRST(300, "tc: [%7.1fs] MUSIC CUE %d (fade %.2f) from %08X, mission %d -> source %08X kind %08X song %d",
                 MusicClock(), cue, fade, ret, mission, source, kind, int32_t(song));
}

// =============================================================================================
// BOSS TRACK (SETTINGS > AUDIO, below RADIO DISPATCHER): the player's own song for a boss fight
//
// The bosses are kBosses (above). The entry is added to the audio screen's layout record (as the display screen's
// entries are). Each boss is listed as LOCKED until its mission is solved in the save loaded: a mission's status is
// options + 0x4F4 + (episode * 14 + index) * 4, its episode and index from the table at 0x0027D7F8 (0x000A06F0
// reads it), and REPLAY EPISODE shows "mission solved" for 2, "mission unsolved" otherwise (0x00150410); so does
// this.
//
// BOSS TRACK opens a screen of its own, a list of the bosses as the game's settings pages list their
// entries. It is a second object of the audio screen's class (0x0015BB20, 0x12B4 bytes) with its own layout
// record (one entry per boss), switched to with 0x001429A0 as the game switches screens (B comes back to AUDIO
// through the screen's +0x8C, which that sets). The audio class's dispatcher (0x00156990), focus change
// (0x00156EA0), panel hiding (0x00153B60) and set-up (0x001568F0) serve both, told apart by the object.
// A boss shows "boss: song" while highlighted; A on it opens CRUISING, FAST ACTION, SLOW ACTION and DEFAULT.
// A list opens the game's TRACK LIST (screen 8, through 0x00142A90 as the soundtrack's buttons do; its value is
// the list's string, 0x14B..0x14D). There, while picking, A takes the highlighted song (its input handler,
// 0x00157220), and A or B come back to the bosses' lists (set-up value 1) instead of the soundtrack's. The pick
// is kept in settings.toml (boss_triad_thug, boss_cary, boss_feng, boss_sniper). In a save where the mission is solved, the music cue for
// the boss's mission (its number, at the start of the fight) gets that song (above).
// =============================================================================================
namespace {
constexpr uint32_t kAudioCtor = TC_SEL(0x0015BB20u, 0x0015B9C0u), kAudioScreenSize = 0x4AD * 4;
constexpr uint32_t kAudioPanelsDone = TC_SEL(0x00153C10u, 0x00153AF0u);
constexpr uint32_t kSwitchScreen = TC_SEL(0x001429A0u, 0x00142900u);  // thiscall (menu, screen object, value)
constexpr uint32_t kAudioEntryList = 0x1228;
constexpr uint32_t kRecordSize = 0x24;
constexpr uint32_t kMenuCallbacks = TC_SEL(0x002C7010u, 0x002C6D30u);  // guest pointer: the menu object
constexpr uint32_t kTrackListScreen = 8;
constexpr uint32_t kCruisingTracks = 0x14B, kSlowTracks = 0x14D;  // "cruising tracks" .. "slow action tracks"
constexpr uint32_t kMusicPlayer = TC_SEL(0x00299660u, 0x00299380u);
constexpr uint32_t kMusicHold = TC_SEL(0x00081180u, 0x000811C0u), kMusicResume = TC_SEL(0x000812C0u, 0x00081300u);
constexpr uint32_t kMusicStopPreview = TC_SEL(0x00080540u, 0x00080580u);
constexpr uint32_t kMissionSlots = TC_SEL(0x0027D7F8u, 0x0027D538u);  // per mission: episode, index
constexpr uint32_t kMissionStatus = 0x4F4, kSolved = 2;
constexpr uint32_t kDefaultString = 216;    // "DEFAULT"
constexpr uint32_t kCruisingString = 0x14E;  // "CRUISING", then "FAST ACTION", "SLOW ACTION"
constexpr uint32_t kEmptyString = 1, kSettingsString = 0xC3;  // "", "SETTINGS"
constexpr uint32_t kPressBack = 0x220, kPressChoose = 0x110, kPressSwitch = 0x0C;

uint32_t g_audio_screen = 0, g_boss_open_panel = 0;  // on AUDIO
uint32_t g_boss_screen = 0, g_boss_info_panel = 0, g_boss_lists_panel = 0;   // the bosses' screen
bool g_building_boss = false, g_boss_return = false;

bool MissionSolved(int32_t mission) {
    uint32_t options = ::kernel::R32(kOptions);
    if (!options || mission < 0 || mission >= 0x7F) return false;
    uint32_t episode = ::kernel::R32(kMissionSlots + mission * 8), index = ::kernel::R32(kMissionSlots + mission * 8 + 4);
    if (episode >= 12 || index >= 14) return false;
    return ::kernel::R32(options + kMissionStatus + (episode * 14 + index) * 4) == kSolved;
}
bool BossUnlocked(uint32_t i) { return i < kBossCount && MissionSolved(kBosses[i].mission); }

// a song's name from its track list line ("074(Not Just) Knee Deep|Funkadelic"): the title
std::string SongTitle(uint32_t song) {
    std::string line = SongLine(song);
    if (line.size() <= 3) return "song " + std::to_string(song);
    std::string title = line.substr(3);
    if (size_t bar = title.find('|'); bar != std::string::npos) title.resize(bar);
    return title;
}

// boss i's large text: its song, or what unlocks it
std::string BossBig(uint32_t i) {
    const Boss& b = kBosses[i];
    if (!BossUnlocked(i)) return std::string("locked: beat ") + b.mission_name + " first";
    uint32_t song = BossSong(i);
    std::string big = std::string(b.lower) + ": " + (song ? SongTitle(song) : std::string("the game's own music"));
    if (big.size() > kLabelSize - 1) big = big.substr(0, kLabelSize - 4) + "...";
    return big;
}

// each boss's entry: its name once its mission is solved, LOCKED before; the large text of the boss being looked at
void BossLabels() {
    LoadPcTracks();
    for (uint32_t i = 0; i < kBossCount; i++) WriteLabel(kLabelBossEntry + i, BossUnlocked(i) ? kBosses[i].name : "LOCKED");
    WriteLabel(kLabelBossInfoBig, BossBig(g_boss_current));
    WriteLabel(kLabelBossHeader, std::string("BOSS TRACK: ") + kBosses[g_boss_current].name);
}

void Show(X86Context& ctx, uint32_t panel, bool show) {
    if (panel) Call(ctx, VFn(panel, show ? 0x0C : 0x08), panel, {1});
}
void Sound(X86Context& ctx, uint32_t id) { Call(ctx, kPlaySound, 0, {id, kOne, kOne}); }

// the bosses' screen, made the first time BOSS TRACK opens (the strings are in by then)
uint32_t BossScreen(X86Context& ctx) {
    if (g_boss_screen) return g_boss_screen;
    uint32_t screen = mem::AllocSystem(kAudioScreenSize, 16);
    uint32_t record = mem::AllocSystem(kRecordSize + 0x14 * kBossCount, 16);
    if (!screen || !record) return 0;
    std::memset(::kernel::HostPtr(screen), 0, kAudioScreenSize);
    g_building_boss = true;
    Call(ctx, kAudioCtor, screen, {});
    g_building_boss = false;
    // AUDIO's record, with its own title and one entry per boss
    std::memcpy(::kernel::HostPtr(record), ::kernel::HostPtr(kAudioRecord), kRecordSize);
    uint32_t items = record + kRecordSize;
    for (uint32_t i = 0; i < kBossCount; i++) {
        const uint32_t entry[5] = {1, LabelId(kLabelBossEntry + i), kSubPanel, kBossEntry + i, 0};
        for (uint32_t k = 0; k < 5; k++) ::kernel::W32(items + i * 0x14 + k * 4, entry[k]);
    }
    ::kernel::W32(record + 0x00, LabelId(kLabelBossTitle));
    ::kernel::W32(record + 0x14, items);
    ::kernel::W32(record + 0x18, kBossCount);
    BossLabels();
    Call(ctx, VFn(screen, 0x38), screen, {record});  // as the menu builds its pages (0x00148D95)
    Call(ctx, kSetText, screen + kAudioEntryList, {kSettingsString});  // its header, as AUDIO's
    Call(ctx, VFn(screen, 0x2C), screen, {0});
    Call(ctx, VFn(screen, 0x08), screen, {1});  // hidden until switched to
    g_boss_screen = screen;
    LOGR("tc: BOSS TRACK screen %08X: panels %08X %08X", screen, g_boss_info_panel, g_boss_lists_panel);
    return screen;
}

// the bosses' lists panel in front, its entries put aside, as the soundtrack's open does (0x00156C80)
void OpenLists(X86Context& ctx, uint32_t screen) {
    Call(ctx, kScreenFocus, screen, {0});
    uint32_t list = screen + kAudioEntryList;
    Call(ctx, VFn(list, 0x08), list, {1});
    Show(ctx, g_boss_info_panel, false);
    Call(ctx, VFn(g_boss_lists_panel, 0x28), g_boss_lists_panel, {0});
    Show(ctx, g_boss_lists_panel, true);
}
// and back to the bosses
void CloseLists(X86Context& ctx, uint32_t screen, uint32_t sound) {
    Call(ctx, kMusicResume, kMusicPlayer, {});
    BossLabels();
    Call(ctx, VFn(g_boss_lists_panel, 0x2C), g_boss_lists_panel, {0});
    Show(ctx, g_boss_lists_panel, false);
    Call(ctx, kAudioPanelsDone, screen, {});
    Show(ctx, g_boss_info_panel, true);
    Sound(ctx, sound);
}
}  // namespace

// the audio class's constructor: AUDIO's BOSS TRACK panels, or the bosses' screen's
X86_EXTERN_FUNC(TC_SEL(__imp__sub_0015BB20, __imp__sub_0015B9C0));
X86_FUNC(TC_SEL(sub_0015BB20, sub_0015B9C0)) {
    uint32_t screen = ctx.ecx;
    TC_SEL(__imp__sub_0015BB20, __imp__sub_0015B9C0)(ctx, base);
    EnsureLabels();
    if (!g_labels) return;
    WriteLabel(kLabelBoss, "BOSS TRACK");
    WriteLabel(kLabelBossOpenBig, "boss track");
    WriteLabel(kLabelBossTitle, "boss track");
    WriteLabel(kLabelBossDefaultBig, "the game's own fight music");
    BossLabels();
    ExtendStrings();
    if (g_building_boss) {
        g_boss_info_panel = MakePanel(ctx, screen, {{kEmptyString, LabelId(kLabelBossInfoBig), kBossInfo}},
                                      kSubPanel, kBossBack);
        g_boss_lists_panel = MakePanel(ctx, screen,
                                       {{kCruisingString, kCruisingTracks, kBossCategory},
                                        {kCruisingString + 1, kCruisingTracks + 1, kBossCategory},
                                        {kCruisingString + 2, kCruisingTracks + 2, kBossCategory},
                                        {kDefaultString, LabelId(kLabelBossDefaultBig), kBossCategory}},
                                       kSubPanel, kBossCategoryBack);
        return;
    }
    if (g_audio_screen != screen) g_boss_screen = 0;  // a new menu: its own bosses' screen
    g_audio_screen = screen;
    g_boss_open_panel = MakePanel(ctx, screen, {{kEmptyString, LabelId(kLabelBossOpenBig), kOpenBoss}}, kSubPanel, kBossBack);
    LOGR("tc: audio screen %08X: BOSS TRACK panel %08X", screen, g_boss_open_panel);
}

// the audio class hides its panels when it sets itself up: the new ones with them
X86_EXTERN_FUNC(TC_SEL(__imp__sub_00153B60, __imp__sub_00153A40));
X86_FUNC(TC_SEL(sub_00153B60, sub_00153A40)) {
    uint32_t screen = ctx.ecx;
    TC_SEL(__imp__sub_00153B60, __imp__sub_00153A40)(ctx, base);
    if (screen == g_audio_screen)
        if (g_boss_open_panel) Call(ctx, VFn(g_boss_open_panel, 0x08), g_boss_open_panel, {0});
    if (screen == g_boss_screen && screen)
        for (uint32_t p : {g_boss_info_panel, g_boss_lists_panel})
            if (p) Call(ctx, VFn(p, 0x08), p, {0});
}

// the audio class's focus change: BOSS TRACK, and each boss, show their panel while focused
X86_EXTERN_FUNC(TC_SEL(__imp__sub_00156EA0, __imp__sub_00156D80));
X86_FUNC(TC_SEL(sub_00156EA0, sub_00156D80)) {
    uint32_t screen = ctx.ecx, old_index = ::kernel::R32(ctx.esp + 4), new_index = ::kernel::R32(ctx.esp + 8);
    TC_SEL(__imp__sub_00156EA0, __imp__sub_00156D80)(ctx, base);
    if (!screen || (screen != g_audio_screen && screen != g_boss_screen)) return;
    // the entry's code: BOSS TRACK on AUDIO, kBossEntry + i for boss i on the bosses' screen
    auto code_at = [&](uint32_t index) {
        uint32_t w = EntryAt(screen, index);
        return w ? ::kernel::R32(w + 0x7C) : 0u;
    };
    uint32_t was = code_at(old_index), now = code_at(new_index);
    if (screen == g_audio_screen) {
        if (was == kOpenBoss) Show(ctx, g_boss_open_panel, false);
        if (now == kOpenBoss) Show(ctx, g_boss_open_panel, true);
        return;
    }
    bool was_boss = was >= kBossEntry && was < kBossEntry + kBossCount;
    bool now_boss = now >= kBossEntry && now < kBossEntry + kBossCount;
    // the panel takes its text as it is shown: hidden and shown again for each boss highlighted
    if (was_boss) Show(ctx, g_boss_info_panel, false);
    if (now_boss) {
        g_boss_current = now - kBossEntry;
        BossLabels();
        Show(ctx, g_boss_info_panel, true);
    }
}

// the audio class's set-up: the bosses' screen, back from the track list, opens its lists again
X86_EXTERN_FUNC(TC_SEL(__imp__sub_001568F0, __imp__sub_001567D0));
X86_FUNC(TC_SEL(sub_001568F0, sub_001567D0)) {
    uint32_t screen = ctx.ecx;
    bool boss = screen && screen == g_boss_screen;
    bool back = boss && g_boss_return && g_boss_lists_panel;
    if (boss || screen == g_audio_screen) g_boss_return = false;
    if (boss) ::kernel::W32(ctx.esp + 4, 0);  // never the soundtrack's lists (value 1) on this screen
    TC_SEL(__imp__sub_001568F0, __imp__sub_001567D0)(ctx, base);
    if (back) {
        BossLabels();
        OpenLists(ctx, screen);
    }
}

// the audio class's dispatcher: BOSS TRACK's codes, on AUDIO and on the bosses' screen
X86_EXTERN_FUNC(TC_SEL(__imp__sub_00156990, __imp__sub_00156870));
X86_FUNC(TC_SEL(sub_00156990, sub_00156870)) {
    uint32_t screen = ctx.ecx, code = ::kernel::R32(ctx.esp + 4);
    bool audio = screen && screen == g_audio_screen, boss = screen && screen == g_boss_screen;
    if (code < kOpenBoss || code > kBossLast || !(audio || boss)) {
        if (code == 0x0A || code == 0x0D) g_boss_assign = false;  // the soundtrack's own lists
        TC_SEL(__imp__sub_00156990, __imp__sub_00156870)(ctx, base);
        return;
    }
    uint32_t lists = g_boss_lists_panel;
    switch (code) {
    case kOpenBoss:
        if (!audio) break;
        BossLabels();  // a save loaded since: LOCKED or the name
        if (uint32_t target = BossScreen(ctx)) {
            Show(ctx, g_boss_open_panel, false);
            Call(ctx, kSwitchScreen, ::kernel::R32(kMenuCallbacks), {target, 0});
        }
        break;
    case kBossInfo:  // the boss panel's own choice: as A on the boss
        if (!boss || !lists) break;
        if (!BossUnlocked(g_boss_current)) {
            Sound(ctx, 0x100);
            break;
        }
        Call(ctx, kMusicHold, kMusicPlayer, {0});
        BossLabels();
        OpenLists(ctx, screen);
        Sound(ctx, 0xFF);
        break;
    case kBossCategoryBack:
        if (boss && lists) CloseLists(ctx, screen, 0x100);
        break;
    case kBossCategory: {
        if (!boss || !lists) break;
        uint32_t big = ::kernel::R32(ctx.esp + 8);
        auto known = [](uint32_t b) {
            return (b >= kCruisingTracks && b <= kSlowTracks) || b == LabelId(kLabelBossDefaultBig);
        };
        if (!known(big)) big = SelectedBig(lists);
        if (big == LabelId(kLabelBossDefaultBig)) {
            BossSong(g_boss_current) = 0;
            app::Save();
            LOGR("tc: BOSS TRACK: %s back to the game's own music", kBosses[g_boss_current].name);
            CloseLists(ctx, screen, 0xFF);
        } else if (big >= kCruisingTracks && big <= kSlowTracks) {
            Sound(ctx, 0xFF);
            g_boss_assign = true;
            Call(ctx, kOpenScreen, ::kernel::R32(kMenuCallbacks), {kTrackListScreen, big});
        }
        break;
    }
    default:
        // A on boss i: its lists (the menu music is held while songs preview, as 0x00156D70); LOCKED opens nothing
        if (!boss || !lists || code < kBossEntry || code >= kBossEntry + kBossCount) break;
        g_boss_current = code - kBossEntry;
        if (!BossUnlocked(g_boss_current)) {
            Sound(ctx, 0x100);
            break;
        }
        Call(ctx, kMusicHold, kMusicPlayer, {0});
        BossLabels();
        OpenLists(ctx, screen);
        Sound(ctx, 0xFF);
        break;
    }
    ::kernel::Return(ctx, 1, 2);  // al = 1, ret 8
}

// the track list's input (thiscall, the pad's new presses). While picking for BOSS TRACK: A (or START, 0x110, the
// presses the menus take as "choose", 0x001465C0) takes the highlighted song; A or B then go back to the bosses'
// lists, the preview stopped first as the game's own back does (0x00157473). Left and right (0x0C), which switch
// a song on or off in the game's own track list, do nothing while picking. Rows are found as the game finds them
// (0x00157261).
X86_EXTERN_FUNC(TC_SEL(__imp__sub_00157220, __imp__sub_00157100));
X86_FUNC(TC_SEL(sub_00157220, sub_00157100)) {
    using ::kernel::R32;
    uint32_t screen = ctx.ecx, presses = R32(ctx.esp + 4);
    if (!g_boss_assign || screen != g_track_screen || !g_boss_screen) {
        TC_SEL(__imp__sub_00157220, __imp__sub_00157100)(ctx, base);
        return;
    }
    bool leave = (presses & kPressBack) != 0;
    if (presses & kPressChoose) {
        uint32_t pick = 0, list = R32(screen + 0x88);
        if (list) {
            uint32_t node = R32(list + 0x74), focus = R32(list + 0x84);
            uint32_t next = node ? R32(node) : 0, index = 0;
            bool found = false;
            while (next && index < 1024) {
                if (R32(node + 8) == focus) {
                    found = true;
                    break;
                }
                node = next;
                next = R32(next);
                index++;
            }
            if (found) {
                uint32_t row = screen + 0x314 + (R32(screen + 0x31C) + index) * kTrackRow;
                int32_t text = int32_t(R32(row + 4));
                uint32_t song = R32(row + 8);
                if (text > 0 && song >= 1 && song <= kMaxSong) pick = song;
            }
        }
        if (pick) {
            BossSong(g_boss_current) = pick;
            app::Save();
            LOGR("tc: BOSS TRACK: %s gets song %u", kBosses[g_boss_current].name, pick);
            Sound(ctx, 0x6D);
            leave = true;
        }
    }
    if (leave) {
        g_boss_assign = false;
        g_boss_return = true;
        Call(ctx, kMusicStopPreview, kMusicPlayer, {});
        Call(ctx, kSwitchScreen, R32(kMenuCallbacks), {g_boss_screen, 1});
        ::kernel::Return(ctx, 1, 1);  // al = 1, ret 4
        return;
    }
    ::kernel::W32(ctx.esp + 4, presses & ~(kPressChoose | kPressSwitch | kPressBack));  // up and down as before
    TC_SEL(__imp__sub_00157220, __imp__sub_00157100)(ctx, base);
}

bool TcMissionSolved(int32_t mission) { return MissionSolved(mission); }

// =============================================================================================
// Extra license plate codes, as the game's own (5WAT, M1K3, HURT M3...): M3M3NT0 for Makoto Yuki and the PC
// version's nine codes (KNOICHI, VAKLAM, DOGTAGS, GENIN, AWYEAH, RECKON, THUGLYF, NOMAYO, ACTVZZL)
//
// The plate screen (0x0014D365) calls 0x000A0A20 with the player's profile (options + 0x350) when L and R are
// held on OK: it compares the plate (profile + 0x13C, 8 bytes) with each code and, on a match, sets the skin
// (profile + 0x618, that is options + 0x968) to 1..20 and returns 1, and the screen plays the cheat sound. The
// character loader (0x000A9F10) then copies D:\Data\Char\ + the skin's file name from the table at 0x0027D640
// (the only place that table is read by skin; elsewhere the skin is only tested for 0). The table ends at slot 21,
// other data follows it, so the extra skins 21..30 never live in it: for the loader's call only, the slot of the
// skin being loaded points at its file name, and afterwards it is put back.
//
// None of these files ship with the port. The setup window (src/common/tc_setup.cpp) makes M3M3NT0.XBX from the
// player's own P3 FES disc;
// the PC skins from the player's own PC copy, as the PC's skin table has them:
// skin 21..29 = CHAR105..112 and CHAR99. They go in assets\extras\Data\Char (or the game's Data\Char). Without
// its file a code does nothing, as any other plate, and a save with that skin loads Nick. The PC version unlocks
// its codes with its own collectibles, which the console version has not: here they work as the console's codes.
// =============================================================================================
namespace {
constexpr uint32_t kSkinNames = TC_SEL(0x0027D640u, 0x0027D380u);  // skin i's file name, i 1..20
constexpr uint32_t kProfilePlate = 0x13C, kProfileSkin = 0x618;     // in the profile (options + 0x350)
constexpr uint32_t kOptionsSkin = 0x350 + kProfileSkin;             // 0x968, as the loader reads it

struct ExtraSkin {
    uint32_t skin;     // the profile's skin number, past the game's 1..20
    const char* code;  // the plate, also the file: <code>.XBX
    const char* who;
    uint32_t name;     // guest copy of the file name, made once
};
ExtraSkin g_extra_skins[] = {
    {21, "M3M3NT0", "Makoto Yuki", 0},
    {22, "KNOICHI", "Ayame (PC)", 0},
    {23, "VAKLAM", "Jeanette (PC)", 0},
    {24, "DOGTAGS", "Call of Duty soldier (PC)", 0},
    {25, "GENIN", "Rikimaru (PC)", 0},
    {26, "AWYEAH", "Taurus (PC)", 0},
    {27, "RECKON", "Desperado (PC)", 0},
    {28, "THUGLYF", "Officer Dick (PC)", 0},
    {29, "NOMAYO", "Sandwich Guy (PC)", 0},
    {30, "ACTVZZL", "Snoop Dogg (PC)", 0},
};

std::string ExtraSkinFile(const ExtraSkin& s) { return std::string(s.code) + ".XBX"; }

bool ExtraSkinFileExists(const ExtraSkin& s) {
    std::string host = ::kernel::ResolvePath("D:\\Data\\Char\\" + ExtraSkinFile(s), 0);
    std::error_code ec;
    return !host.empty() && std::filesystem::is_regular_file(host, ec);
}

ExtraSkin* ExtraSkinBySkin(uint32_t skin) {
    for (ExtraSkin& s : g_extra_skins)
        if (s.skin == skin) return &s;
    return nullptr;
}

// the plate as the game compares it: the code and its terminating zero
ExtraSkin* ExtraSkinByPlate(const char plate[8]) {
    for (ExtraSkin& s : g_extra_skins) {
        size_t n = std::strlen(s.code) + 1;
        if (n <= 8 && std::memcmp(plate, s.code, n) == 0) return &s;
    }
    return nullptr;
}

uint32_t ExtraSkinName(ExtraSkin& s) {
    if (!s.name) {
        std::string file = ExtraSkinFile(s);
        uint32_t name = mem::AllocSystem(uint32_t(file.size() + 1));
        if (!name) return 0;
        std::memcpy(::kernel::HostPtr(name), file.c_str(), file.size() + 1);
        s.name = name;
    }
    return s.name;
}
}  // namespace

// the plate codes: the game's own first, then the extra ones
X86_EXTERN_FUNC(TC_SEL(__imp__sub_000A0A20, __imp__sub_000A0AD0));
X86_FUNC(TC_SEL(sub_000A0A20, sub_000A0AD0)) {
    uint32_t profile = ctx.ecx;
    TC_SEL(__imp__sub_000A0A20, __imp__sub_000A0AD0)(ctx, base);
    if (ctx.eax & 0xFF) return;  // one of the game's codes
    char plate[8];
    std::memcpy(plate, ::kernel::HostPtr(profile + kProfilePlate), sizeof(plate));
    ExtraSkin* s = ExtraSkinByPlate(plate);
    if (!s) return;
    if (!ExtraSkinFileExists(*s)) {
        LOGR("tc: plate %s entered, but Data\\Char\\%s is not there: no code", s->code, ExtraSkinFile(*s).c_str());
        return;
    }
    ::kernel::W32(profile + kProfileSkin, s->skin);
    ctx.eax = (ctx.eax & ~0xFFu) | 1;  // a code: the cheat sound, as the game's own
    LOGR("tc: plate %s: %s (skin %u)", s->code, s->who, s->skin);
}

// the character loader: an extra skin's file name in its slot for this call only; Nick when the file is not there
X86_EXTERN_FUNC(TC_SEL(__imp__sub_000A9F10, __imp__sub_000A9F50));
X86_FUNC(TC_SEL(sub_000A9F10, sub_000A9F50)) {
    uint32_t options = ctx.ecx;
    uint32_t slot = 0, saved = 0;
    if (options) {
        uint32_t skin = ::kernel::R32(options + kOptionsSkin);
        if (ExtraSkin* s = ExtraSkinBySkin(skin)) {
            uint32_t name = ExtraSkinFileExists(*s) ? ExtraSkinName(*s) : 0;
            if (!name) {
                ::kernel::W32(options + kOptionsSkin, 0);
                TC_LOG_FIRST(4, "tc: skin %u: Data\\Char\\%s is not there, Nick is loaded", skin,
                             ExtraSkinFile(*s).c_str());
            } else {
                slot = kSkinNames + skin * 4;
                saved = ::kernel::R32(slot);
                ::kernel::W32(slot, name);
                TC_LOG_FIRST(4, "tc: skin %u: %s, from Data\\Char\\%s", skin, s->who, ExtraSkinFile(*s).c_str());
            }
        }
    }
    TC_SEL(__imp__sub_000A9F10, __imp__sub_000A9F50)(ctx, base);
    if (slot) ::kernel::W32(slot, saved);
}
