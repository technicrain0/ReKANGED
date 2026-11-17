// Hooks for default.xbe (True Crime: Streets of LA, PAL, XDK 5659).
// A strong X86_FUNC definition replaces the recompiled function at that address.
// To call the original body: X86_EXTERN_FUNC(__imp__sub_XXXXXXXX); then __imp__sub_XXXXXXXX(ctx, base);

#include <algorithm>
#include <cctype>
#include <cmath>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

#include "app/app.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

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
#define TC_VOICE_3D_STUB(addr, nargs)               \
    X86_FUNC(sub_##addr) {                          \
        (void)base;                                 \
        ::kernel::Return(ctx, 0 /* S_OK */, nargs); \
    }

TC_VOICE_3D_STUB(002127A6, 3)  // SetMaxDistance(voice, distance, apply)       callers 0x212A95 / 0x212D4A
TC_VOICE_3D_STUB(002127D9, 3)  // SetMinDistance(voice, distance, apply)       callers 0x212AEB / 0x212DA4
TC_VOICE_3D_STUB(0021280C, 3)  // SetMode(voice, mode, apply)                  callers 0x212B41 / 0x212DFE
TC_VOICE_3D_STUB(0021283F, 5)  // SetPosition(voice, x, y, z, apply)           callers 0x212B93 / 0x212E54
TC_VOICE_3D_STUB(00212891, 3)  // writes 3D block +0x4C (velocity/doppler)     callers 0x212BFA / 0x212EBF
TC_VOICE_3D_STUB(002128C4, 4)  // SetRolloffCurve(voice, count, curve, apply)  callers 0x212C50 / 0x212F19

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
X86_EXTERN_FUNC(__imp__sub_001E7866);
X86_FUNC(sub_001E7866) {
    uint32_t ret = ::kernel::R32(ctx.esp);
    uint32_t ms = ::kernel::R32(ctx.esp + 4);
    if (ms >= 100 && (ret == 0x000D796Au || ret == 0x001D17C0u)) {
        TC_LOG_FIRST(20, "tc: wait loop at %08X slept %u ms, now 16 ms", ret, ms);
        ::kernel::W32(ctx.esp + 4, 16);
    } else if (ms >= 100) {
        TC_LOG_FIRST(20, "tc: Sleep(%u) from %08X", ms, ret);
    }
    __imp__sub_001E7866(ctx, base);
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
// A pending query now answers at once with that query's last real result, or "visible" (one full
// 640x480 screen of pixels) before it ever had one, which is what flares and glare expect.
// =============================================================================================
X86_EXTERN_FUNC(__imp__sub_001FF410);
X86_FUNC(sub_001FF410) {
    constexpr uint32_t kTestIncomplete = 0x88760828u;
    constexpr uint32_t kVisible = 640u * 480u;
    static std::mutex lock;
    static std::unordered_map<uint32_t, uint32_t> last;  // query index -> last real pixel count
    uint32_t index = ::kernel::R32(ctx.esp + 4);
    uint32_t result = ::kernel::R32(ctx.esp + 8);
    uint32_t stamp = ::kernel::R32(ctx.esp + 12);
    __imp__sub_001FF410(ctx, base);  // stdcall: pops its 3 arguments
    std::lock_guard<std::mutex> guard(lock);
    if (ctx.eax == 0) {
        if (result) last[index] = ::kernel::R32(result);
        return;
    }
    if (ctx.eax != kTestIncomplete) return;
    auto it = last.find(index);
    uint32_t pixels = it != last.end() ? it->second : kVisible;
    if (result) ::kernel::W32(result, pixels);
    if (stamp) {
        ::kernel::W32(stamp, 0);
        ::kernel::W32(stamp + 4, 0);
    }
    ctx.eax = 0;  // D3D_OK
    TC_LOG_FIRST(4, "tc: visibility test %08X pending, answered %u pixels", index, pixels);
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
constexpr uint32_t kStreamSlots = 0x00305658u, kStreamSlotSize = 0xA8u, kSector = 0x800u;
constexpr uint32_t kReadFile = 0x001E9224u, kGetLastError = 0x001E997Fu, kIoPending = 997u;
}  // namespace

X86_FUNC(sub_001DA710) {
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

X86_EXTERN_FUNC(__imp__sub_001175D0);
X86_FUNC(sub_001175D0) {
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
    bool title_menu = R32(0x002AEE58) == 5;
    int max_passes = title_menu ? 4096 : 32;
    auto budget = title_menu ? std::chrono::milliseconds(1500) : std::chrono::milliseconds(4);
    uint64_t before = progress();
    __imp__sub_001175D0(ctx, base);  // pops its 3 arguments and the return address
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
        __imp__sub_001175D0(ctx, base);
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
constexpr uint32_t kMainMenuRecord = 0x0028AC34u;
constexpr uint32_t kOpenScreen = 0x00142A90u;
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
constexpr uint32_t kDisplayRecord = 0x0028ADC8u;
constexpr uint32_t kStringTable = 0x002C700Cu;  // guest pointer to the string pointer array
constexpr uint32_t kGameStrings = 3192;          // its size: 0x31E0 bytes
constexpr uint32_t kLabels = 32, kLabelSize = 32;
constexpr uint32_t kMessageSize = 96;  // the save messages, kept apart from the short labels
constexpr uint32_t kSubPanel = 0x00142AB0u;      // entry callback: the screen's dispatcher
constexpr uint32_t kPanelCtor = 0x00158950u, kPanelSize = 0x400;  // panels are 0x300 apart in the screen
constexpr uint32_t kAddButton = 0x00146FB0u, kHighlight = 0x001463D0u;
constexpr uint32_t kPlaySound = 0x000BED90u, kScreenFocus = 0x00143280u, kPanelsDone = 0x00154550u;
constexpr uint32_t kScreenUnfocus = 0x00143260u;
constexpr uint32_t kOptions = 0x002A4258u;  // guest pointer to the game's options block
constexpr uint32_t kSubtitlesOn = 0x1B8, kSubtitlesOff = 0x1B9;  // "subtitles on" / "subtitles off"
constexpr uint32_t kSetText = 0x00143350u, kPanelShow = 0x00153490u;  // the panel class's show (vtable +0x0C)
constexpr uint32_t kNoOp4 = 0x000CF210u;   // a guest function that is only "ret 4"
constexpr uint32_t kVtableCopy = 64;      // entries copied: the class's vtable and some past it  // gives the screen its input back (0x00143280 takes it)
constexpr uint32_t kWidgetCount = 0x002A8EE0u, kWidgetNodes = 0x002A46D0u;
constexpr uint32_t kOne = 0x3F800000u;           // 1.0f
// label slots past the game's strings
enum : uint32_t { kLabelMode, kLabelRes, kLabelModeChoice, kLabelModeBig = kLabelModeChoice + 3,
                  kLabelResChoice = kLabelModeBig + 3, kLabelResBig = kLabelResChoice + 5,
                  kLabelExitYesBig = kLabelResBig + 5, kLabelExitNoBig,
                  kLabelScreen, kLabelScreenChoice, kLabelScreenBig = kLabelScreenChoice + 2,
                  kLabelFps = kLabelScreenBig + 2, kLabelFpsChoice, kLabelFpsBig = kLabelFpsChoice + 2,
                  kLabelCount = kLabelFpsBig + 2 };
static_assert(kLabelCount <= kLabels);
// dispatcher codes, past the game's 8..14
enum : uint32_t { kOpenMode = 0x40, kPickMode, kBackMode, kOpenRes, kPickRes, kBackRes, kOpenWide, kPickWide, kBackWide,
                  kOpenFps, kPickFps, kBackFps };
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

void ExtendStrings() {
    EnsureLabels();
    std::lock_guard lock(g_labels_lock);
    uint32_t table = ::kernel::R32(kStringTable);
    if (!table || !g_labels) return;
    if (table != g_table) {
        uint32_t copy = mem::AllocSystem((kGameStrings + kLabels) * 4);
        if (!copy) return;
        std::memcpy(::kernel::HostPtr(copy), ::kernel::HostPtr(table), kGameStrings * 4);
        ::kernel::W32(kStringTable, copy);
        g_table = copy;
    }
    for (uint32_t i = 0; i < kLabels; i++) ::kernel::W32(g_table + (kGameStrings + i) * 4, g_labels + i * kLabelSize);
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

X86_EXTERN_FUNC(__imp__sub_00142520);
X86_FUNC(sub_00142520) {
    __imp__sub_00142520(ctx, base);
    ExtendStrings();
}

// the display screen's constructor: its panels for display mode and resolution
X86_EXTERN_FUNC(__imp__sub_0015AA10);
X86_FUNC(sub_0015AA10) {
    uint32_t screen = ctx.ecx;
    __imp__sub_0015AA10(ctx, base);
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
X86_EXTERN_FUNC(__imp__sub_00154500);
X86_FUNC(sub_00154500) {
    uint32_t screen = ctx.ecx;
    __imp__sub_00154500(ctx, base);
    if (screen != g_screen) return;
    for (uint32_t p : {g_mode_panel, g_res_panel, g_wide_panel, g_fps_panel})
        if (p) Call(ctx, VFn(p, 0x08), p, {0});
    SyncPanels(ctx);
}

// the display screen's dispatcher: the new panels' open, choice and back
X86_EXTERN_FUNC(__imp__sub_001576B0);
X86_FUNC(sub_001576B0) {
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
        __imp__sub_001576B0(ctx, base);
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
X86_EXTERN_FUNC(__imp__sub_001578B0);
X86_FUNC(sub_001578B0) {
    uint32_t screen = ctx.ecx, old_index = ::kernel::R32(ctx.esp + 4), new_index = ::kernel::R32(ctx.esp + 8);
    __imp__sub_001578B0(ctx, base);
    if (screen != g_screen) return;
    if (uint32_t w = EntryAt(screen, old_index))
        if (uint32_t p = PanelFor(::kernel::R32(w + 0x7C))) Call(ctx, VFn(p, 0x08), p, {1});
    if (uint32_t w = EntryAt(screen, new_index))
        if (uint32_t p = PanelFor(::kernel::R32(w + 0x7C))) {
            SyncPanels(ctx);  // the setting may have changed outside the menu (Alt+Enter)
            Call(ctx, VFn(p, 0x0C), p, {1});
        }
}

X86_EXTERN_FUNC(__imp__sub_00147060);
X86_FUNC(sub_00147060) {
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
    __imp__sub_00147060(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00142A90);
X86_FUNC(sub_00142A90) {
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
    __imp__sub_00142A90(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0014B350);
X86_FUNC(sub_0014B350) {
    if (::kernel::R32(ctx.esp + 4) == 0x29) {
        LOGR("tc: exit to dashboard chosen, quitting");
        app::RequestQuit();
        ::kernel::Return(ctx, 0, 2);  // thiscall, ret 8
        return;
    }
    __imp__sub_0014B350(ctx, base);
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
X86_EXTERN_FUNC(__imp__sub_001E992E);
X86_FUNC(sub_001E992E) {
    if (app::Get().wide_screen && app::Widescreen()) {
        constexpr uint32_t kWidescreen = 0x01, kHdtv720p = 0x02, kHdtv480p = 0x08;
        LOGR("tc: video flags reported as widescreen 720p (SCREEN widescreen)");
        g_started_wide = true;
        ::kernel::Return(ctx, kWidescreen | kHdtv720p | kHdtv480p, 0);
        return;
    }
    __imp__sub_001E992E(ctx, base);
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
constexpr uint32_t kGameState = 0x002AEE58;  // 5 while the title's main menu runs
constexpr uint32_t kStateMainMenu = 5;
constexpr uint32_t kSceneSetCameraRet = 0x000D7F09;
constexpr float kMenuPitchDegrees = 5.0f;  // the street's edge sits about 5-6 deg below level at the worst spots
uint32_t g_menu_cam = 0;  // the tilted copy the view gets
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001D3EF0);
X86_FUNC(sub_001D3EF0) {
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
    __imp__sub_001D3EF0(ctx, base);
}
