// [TCSOLA] The setup window of the release (TrueCrimeLA.exe as players download it): on its first start (or a start
// with --setup, or with Shift held), before the game's code is prepared, it shows what the player has and makes the extra content from it:
//   - True Crime: Streets of LA (Xbox, PAL): detected in assets (the .iso, or the extracted files). Required.
//   - Optional: PC Content: the player browses to their own PC copy, anywhere. From it: the 32 PC songs
//     (assets\extras\Data\Music\084.WMA..., and assets\extras\pc_tracks.txt) and the 9 PC skins (their plate codes).
//   - Optional: Makoto Yuki (Persona 3): Persona 3 FES (USA) detected in assets; Makoto for the M3M3NT0 plate.
// Nothing else is needed: the songs are decoded here (setup\bink_audio.h) and encoded with Windows' own WMA encoder
// (Media Foundation), the skins and Makoto converted here (setup\pc_skins.h, setup\p3_makoto.h). Everything made
// goes into assets\extras, which lies over the game's D:. Nothing of any game ships with the port.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "setup/bink_audio.h"
#include "setup/p3_makoto.h"
#include "setup/pc_skins.h"
#include "setup/pc_tracks.h"

bool g_tc_setup_done = false;  // settings.toml: setup_done (game_info.cpp lists it)

namespace {

namespace fs = std::filesystem;

// ------------------------------------------------------------------------------------------- what is where
fs::path Assets() {
    fs::path dir = app::ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return app::ExeDir() / "assets";
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

std::vector<fs::path> Isos(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::wstring ext = e.path().extension().wstring();
        for (auto& c : ext) c = wchar_t(towlower(c));
        if (ext == L".iso" && e.is_regular_file(ec)) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool IsXboxIso(const fs::path& iso) {
    std::ifstream f(iso, std::ios::binary);
    if (!f) return false;
    for (uint64_t base : {uint64_t(0), uint64_t(0x18300000)}) {
        char magic[20] = {};
        f.clear();
        f.seekg(std::streamoff(base + 0x10000));
        f.read(magic, 20);
        if (f && std::memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0) return true;
    }
    return false;
}

struct Found {
    bool game = false;
    std::wstring game_text;
    bool p3 = false;
    std::wstring p3_text;
    fs::path p3_iso;
    // made by an earlier setup (or a .bat): then the disc or the PC copy is not needed again
    bool makoto_ready = false;
    bool pc_ready = false;
    std::wstring pc_ready_text;
};

Found Detect() {
    Found f;
    fs::path a = Assets();
    std::error_code ec;
    if (fs::is_regular_file(a / "default.xbe", ec)) {
        f.game = true;
        f.game_text = L"TCSOLA game files detected (assets\\default.xbe)";
    } else {
        for (auto& iso : Isos(a))
            if (IsXboxIso(iso)) {
                f.game = true;
                f.game_text = L"TCSOLA Xbox ISO detected: " + iso.filename().wstring();
                break;
            }
    }
    if (!f.game) f.game_text = L"Not detected: put the True Crime: Streets of LA (Xbox) .iso in the assets folder";
    for (auto& iso : Isos(a)) {
        std::string why;
        if (p3makoto::IsP3FesUsa(iso, why)) {
            f.p3 = true;
            f.p3_iso = iso;
            f.p3_text = L"Persona 3 FES (USA) detected: " + iso.filename().wstring();
            break;
        }
    }
    if (!f.p3) f.p3_text = L"Not detected: put Persona 3 FES (USA, PS2) .iso in the assets folder";
    // already made: Makoto's file; the 9 PC skins, the PC song list and every song on it
    fs::path chars = a / "extras" / "Data" / "Char";
    auto there = [&](const fs::path& p) { return fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 0; };
    // his model with his own HUD portrait and his blink (one made before either came is made again)
    f.makoto_ready = there(chars / "M3M3NT0.XBX");
    if (f.makoto_ready) {
        try {
            auto xbx = p3makoto::ReadFile(chars / "M3M3NT0.XBX");
            f.makoto_ready = p3makoto::PortraitHash(xbx) != p3makoto::kNickPortrait && p3makoto::HasBlink(xbx);
            // made by an older converter (before his shirt fix): made again while the disc is there, kept if not
            if (f.makoto_ready && f.p3 && p3makoto::MakotoVersion(xbx) < p3makoto::kMakotoVersion) f.makoto_ready = false;
        } catch (const std::exception&) {
            f.makoto_ready = false;
        }
    }
    if (f.makoto_ready)
        f.p3_text = L"Makoto Yuki is already set up (assets\\extras\\Data\\Char\\M3M3NT0.XBX): the Persona 3 FES .iso is not needed";
    bool skins = true;
    for (const auto& sk : pcskins::kSkins) skins = skins && there(chars / (std::string(sk.code) + ".XBX"));
    std::vector<pctracks::Track> songs;
    if (there(a / "extras" / "pc_tracks.txt")) songs = pctracks::Read(a / "extras" / "pc_tracks.txt");
    bool music = !songs.empty();
    for (const auto& t : songs) {
        char n[16];
        std::snprintf(n, sizeof(n), "%03d.WMA", t.song);
        music = music && there(a / "extras" / "Data" / "Music" / n);
    }
    f.pc_ready = skins && music;
    if (f.pc_ready)
        f.pc_ready_text = L"PC Content is already set up (9 PC skins, " + std::to_wstring(songs.size()) +
                          L" PC songs in assets\\extras): the PC copy is not needed";
    return f;
}

// a True Crime PC copy: TrueCrime.exe, its skins and its songs
bool ValidPcCopy(const fs::path& pc, std::wstring& why) {
    std::error_code ec;
    if (pc.empty()) return why = L"Choose your True Crime: Streets of LA PC folder", false;
    if (!fs::is_directory(pc, ec)) return why = L"That folder does not exist", false;
    if (!fs::is_regular_file(pc / "TrueCrime.exe", ec))
        return why = L"Not a True Crime PC copy: no TrueCrime.exe in that folder", false;
    if (!fs::is_regular_file(pc / "Data" / "Char" / "CHAR105.PCM", ec))
        return why = L"That PC copy has no Data\\Char\\CHAR105.PCM (the PC skins)", false;
    if (!fs::is_regular_file(pc / "Data" / "Music" / "084.bik", ec))
        return why = L"That PC copy has no Data\\Music\\084.bik (the PC songs)", false;
    if (pctracks::TrackList(pc).empty()) return why = L"That PC copy has no Data\\Shell\\TCPC*.txt (the song list)", false;
    why = L"Valid True Crime: Streets of LA PC copy";
    return true;
}

// ------------------------------------------------------------------------------------------- the setup log
std::mutex g_log_lock;
fs::path LogFile() { return app::ExeDir() / "logs" / "setup.log"; }
void SetupLog(const std::string& line) {
    std::lock_guard lock(g_log_lock);
    std::error_code ec;
    fs::create_directories(LogFile().parent_path(), ec);
    std::ofstream(LogFile(), std::ios::app) << line << "\n";
    LOGR("setup: %s", line.c_str());
}

// ------------------------------------------------------------------------------------------- WMA (Media Foundation)
template <class T> void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// 16-bit PCM -> a WMA file the console's WMA decoder plays: WMA standard (WAVE_FORMAT_WMAUDIO2) in ASF, about 128 kbps
bool EncodeWma(const bink::Audio& a, const fs::path& out, std::string& error) {
    HRESULT hr;
    IMFCollection* types = nullptr;
    IMFMediaType* pick = nullptr;
    IMFMediaType* in = nullptr;
    IMFSinkWriter* writer = nullptr;
    IMFAttributes* attrs = nullptr;
    bool ok = false;
    auto fail = [&](const char* what, HRESULT h) {
        char b[128];
        std::snprintf(b, sizeof(b), "%s failed (0x%08lX)", what, (unsigned long)h);
        error = b;
    };
    do {
        hr = MFTranscodeGetAudioOutputAvailableTypes(MFAudioFormat_WMAudioV8, MFT_ENUM_FLAG_ALL, nullptr, &types);
        if (FAILED(hr)) { fail("Windows' WMA encoder (is Media Foundation installed? N editions need the Media Feature Pack)", hr); break; }
        DWORD n = 0;
        types->GetElementCount(&n);
        uint32_t best = 0xFFFFFFFF;
        for (DWORD i = 0; i < n; i++) {
            IUnknown* u = nullptr;
            IMFMediaType* t = nullptr;
            if (FAILED(types->GetElement(i, &u)) || !u) continue;
            if (SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&t)))) {
                UINT32 rate = 0, ch = 0, bytes = 0;
                t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes);
                uint32_t d = bytes > 16000 ? bytes - 16000 : 16000 - bytes;  // 128 kbps
                if (rate == a.rate && ch == UINT32(a.channels) && d < best) {
                    best = d;
                    Release(pick);
                    pick = t;
                    t = nullptr;
                }
            }
            Release(t);
            Release(u);
        }
        if (!pick) { error = "Windows' WMA encoder has no 44.1 kHz stereo setting"; break; }
        hr = MFCreateAttributes(&attrs, 1);
        if (FAILED(hr)) { fail("MFCreateAttributes", hr); break; }
        attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_ASF);
        std::error_code ec;
        fs::remove(out, ec);
        hr = MFCreateSinkWriterFromURL(out.c_str(), nullptr, attrs, &writer);
        if (FAILED(hr)) { fail("Creating the WMA file", hr); break; }
        DWORD stream = 0;
        hr = writer->AddStream(pick, &stream);
        if (FAILED(hr)) { fail("AddStream", hr); break; }
        hr = MFCreateMediaType(&in);
        if (FAILED(hr)) { fail("MFCreateMediaType", hr); break; }
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, a.rate);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(a.channels));
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, UINT32(2 * a.channels));
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, a.rate * UINT32(2 * a.channels));
        in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        hr = writer->SetInputMediaType(stream, in, nullptr);
        if (FAILED(hr)) { fail("SetInputMediaType", hr); break; }
        hr = writer->BeginWriting();
        if (FAILED(hr)) { fail("BeginWriting", hr); break; }
        size_t frames = a.pcm.size() / size_t(a.channels), chunk = a.rate / 2, at = 0;
        bool wrote = true;
        while (at < frames) {
            size_t count = std::min(chunk, frames - at);
            DWORD bytes = DWORD(count * size_t(a.channels) * 2);
            IMFMediaBuffer* buf = nullptr;
            IMFSample* sample = nullptr;
            BYTE* p = nullptr;
            hr = MFCreateMemoryBuffer(bytes, &buf);
            if (SUCCEEDED(hr)) hr = buf->Lock(&p, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                std::memcpy(p, &a.pcm[at * size_t(a.channels)], bytes);
                buf->Unlock();
                buf->SetCurrentLength(bytes);
                hr = MFCreateSample(&sample);
            }
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
            if (SUCCEEDED(hr)) hr = sample->SetSampleTime(LONGLONG(at) * 10000000 / a.rate);
            if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(LONGLONG(count) * 10000000 / a.rate);
            if (SUCCEEDED(hr)) hr = writer->WriteSample(stream, sample);
            Release(sample);
            Release(buf);
            if (FAILED(hr)) {
                fail("Writing the WMA file", hr);
                wrote = false;
                break;
            }
            at += count;
        }
        if (!wrote) break;
        hr = writer->Finalize();
        if (FAILED(hr)) { fail("Finishing the WMA file", hr); break; }
        ok = true;
    } while (false);
    Release(writer);
    Release(in);
    Release(pick);
    Release(types);
    Release(attrs);
    if (!ok) {
        std::error_code ec;
        fs::remove(out, ec);
    }
    return ok;
}

// ------------------------------------------------------------------------------------------- the work
struct Plan {
    bool pc = false;
    fs::path pc_dir;
    bool makoto = false;
    fs::path p3_iso;
};

std::atomic<int> g_done{0}, g_total{1};
std::atomic<bool> g_working{false}, g_failed{false}, g_finished{false}, g_cancel{false};
std::mutex g_text_lock;
std::wstring g_stage = L"";
std::wstring g_error;
HWND g_hwnd = nullptr;
constexpr UINT kWorkDone = WM_APP + 21;

void Stage(const std::wstring& s) {
    {
        std::lock_guard lock(g_text_lock);
        g_stage = s;
    }
    SetupLog(Narrow(s));
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void WriteBytes(const fs::path& p, const std::vector<uint8_t>& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    if (!f) throw std::runtime_error("cannot write " + p.string());
}

void Work(Plan plan) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool mf = false;
    fs::path extras = Assets() / "extras";
    try {
        std::vector<pctracks::Track> tracks;
        int total = 0;
        if (plan.pc) {
            tracks = pctracks::Read(pctracks::TrackList(plan.pc_dir));
            total += int(sizeof(pcskins::kSkins) / sizeof(pcskins::kSkins[0])) + int(tracks.size());
        }
        if (plan.makoto) total += 1;
        g_total = std::max(total, 1);
        g_done = 0;
        if (plan.makoto) {
            Stage(L"Making Makoto Yuki from Persona 3 FES");
            std::vector<uint8_t> nick = p3makoto::NickFrom(Assets());
            WriteBytes(extras / "Data" / "Char" / "M3M3NT0.XBX", p3makoto::MakotoXbx(plan.p3_iso, nick));
            SetupLog("Makoto (with his HUD portrait): assets\\extras\\Data\\Char\\M3M3NT0.XBX");
            g_done++;
        }
        if (plan.pc) {
            fs::path chars = plan.pc_dir / "Data" / "Char";
            for (const auto& s : pcskins::kSkins) {
                if (g_cancel) throw std::runtime_error("stopped");
                Stage(L"PC skins: " + Wide(s.who) + L" (" + Wide(s.code) + L")");
                std::string n = "CHAR" + std::to_string(s.pc_char);
                auto out = pcskins::ConvertPcCharacter(pcskins::ReadFile(chars / (n + ".PCM")), pcskins::ReadFile(chars / (n + ".RES")));
                WriteBytes(extras / "Data" / "Char" / (std::string(s.code) + ".XBX"), out);
                g_done++;
            }
            HRESULT h = MFStartup(MF_VERSION, MFSTARTUP_LITE);
            if (FAILED(h)) throw std::runtime_error("Windows' media components (Media Foundation) did not start");
            mf = true;
            fs::path music = extras / "Data" / "Music";
            fs::create_directories(music);
            std::string list;
            int made = 0;
            for (const auto& t : tracks) {
                if (g_cancel) throw std::runtime_error("stopped");
                char name[16];
                std::snprintf(name, sizeof(name), "%03d", t.song);
                std::string title = t.line.substr(3, t.line.find('|') - 3);
                Stage(L"PC songs: " + Wide(name) + L" " + Wide(title) + L"  (" + std::to_wstring(made + 1) + L" of " +
                      std::to_wstring(tracks.size()) + L")");
                fs::path src = plan.pc_dir / "Data" / "Music" / (std::string(name) + ".bik");
                fs::path dst = music / (std::string(name) + ".WMA");
                std::error_code ec;
                if (fs::is_regular_file(dst, ec) && fs::file_size(dst, ec) > 0) {
                    SetupLog(std::string(name) + ": already made");
                } else if (!fs::is_regular_file(src, ec)) {
                    SetupLog(std::string(name) + ".bik is missing in the PC copy: left out");
                    g_done++;
                    continue;
                } else {
                    bink::Audio a = bink::Decode(pcskins::ReadFile(src));
                    std::string err;
                    if (!EncodeWma(a, dst, err)) throw std::runtime_error(name + std::string(": ") + err);
                }
                list += t.line + "\r\n";
                made++;
                g_done++;
            }
            if (!made) throw std::runtime_error("no PC song was made");
            std::ofstream(extras / "pc_tracks.txt", std::ios::binary) << list;
            SetupLog("PC songs: " + std::to_string(made) + " in assets\\extras\\Data\\Music");
        }
        Stage(L"Done");
        g_failed = false;
    } catch (const std::exception& e) {
        std::lock_guard lock(g_text_lock);
        g_error = Wide(e.what());
        g_failed = true;
    }
    if (mf) MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    if (g_failed) SetupLog("FAILED: " + Narrow(g_error));
    g_working = false;
    g_finished = !g_failed;
    PostMessageW(g_hwnd, kWorkDone, 0, 0);
}

// ------------------------------------------------------------------------------------------- the window
constexpr COLORREF kBack = RGB(18, 20, 26), kPanel = RGB(30, 33, 42), kText = RGB(232, 234, 240),
                   kDim = RGB(150, 155, 168), kGood = RGB(120, 200, 140), kBad = RGB(230, 110, 100),
                   kAccent = RGB(132, 96, 228), kBox = RGB(52, 56, 70);

Found g_found;
bool g_want_pc = false, g_want_makoto = false;
bool g_start = false;
HWND g_edit = nullptr;
HFONT g_font = nullptr, g_bold = nullptr, g_title = nullptr;
HBRUSH g_edit_brush = nullptr;

int U(HWND h) { return MulDiv(10, GetDpiForWindow(h), 96); }

std::wstring EditText() {
    wchar_t b[2048] = {};
    if (g_edit) GetWindowTextW(g_edit, b, 2048);
    std::wstring s = b;
    while (!s.empty() && (s.back() == L' ' || s.back() == L'"' || s.back() == L'\\')) s.pop_back();
    while (!s.empty() && (s.front() == L' ' || s.front() == L'"')) s.erase(s.begin());
    return s;
}

struct Rects {
    RECT title, sub, game_head, game_status, pc_box, pc_label, edit, browse, pc_status, mk_box, mk_status, note,
        stage, bar, log, recheck, cancel, go;
};

Rects Arrange(HWND h) {
    RECT c;
    GetClientRect(h, &c);
    int u = U(h), x0 = c.left + u * 3, x1 = c.right - u * 3, y = u * 2;
    auto row = [&](int height) {
        RECT r{x0, y, x1, y + height};
        y += height;
        return r;
    };
    Rects r;
    r.title = row(u * 4);
    r.sub = row(u * 3);
    y += u;
    r.game_head = row(u * 3);
    r.game_status = row(u * 3);
    y += u * 2;
    r.pc_box = row(u * 3);
    r.pc_label = row(u * 3);
    RECT e = row(u * 3);
    r.edit = {e.left, e.top, e.right - u * 12, e.bottom};
    r.browse = {e.right - u * 11, e.top, e.right, e.bottom};
    r.pc_status = row(u * 3);
    y += u * 2;
    r.mk_box = row(u * 3);
    r.mk_status = row(u * 3);
    y += u * 2;
    r.note = row(u * 4);
    r.stage = row(u * 3);
    r.bar = row(u);
    y += u * 2;
    RECT b = row(u * 4);
    r.log = {b.left, b.top, b.left + u * 15, b.bottom};
    r.recheck = {r.log.right + u, b.top, r.log.right + u * 14, b.bottom};
    r.go = {b.right - u * 16, b.top, b.right, b.bottom};
    r.cancel = {r.go.left - u * 11, b.top, r.go.left - u, b.bottom};
    return r;
}

void Fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
void Text(HDC dc, RECT r, const std::wstring& s, COLORREF c, HFONT f, UINT align = DT_LEFT) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), int(s.size()), &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}
void Button(HDC dc, const RECT& r, const std::wstring& s, bool primary, bool enabled) {
    Fill(dc, r, !enabled ? kPanel : primary ? kAccent : kBox);
    Text(dc, r, s, enabled ? kText : kDim, primary ? g_bold : g_font, DT_CENTER);
}
void Check(HDC dc, HWND h, const RECT& r, bool on, const std::wstring& s, bool enabled) {
    int u = U(h), mid = (r.top + r.bottom) / 2;
    RECT box{r.left, mid - u, r.left + u * 2, mid + u};
    Fill(dc, box, kBox);
    if (on) {
        InflateRect(&box, -u / 2, -u / 2);
        Fill(dc, box, kAccent);
    }
    RECT label = r;
    label.left += u * 3;
    Text(dc, label, s, enabled ? kText : kDim, g_bold);
}

void Paint(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT c;
    GetClientRect(h, &c);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, c.right, c.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, c, kBack);
    Rects r = Arrange(h);
    bool busy = g_working;
    Text(dc, r.title, L"TRUE CRIME: STREETS OF LA", kText, g_title);
    Text(dc, r.sub, L"The ISOs go in the assets folder, and are detected. The PC copy can be anywhere: browse to it.", kDim, g_font);

    Text(dc, r.game_head, L"True Crime: Streets of LA · Xbox (PAL)", kText, g_bold);
    Text(dc, r.game_status, g_found.game_text, g_found.game ? kGood : kBad, g_font);

    Check(dc, h, r.pc_box, g_want_pc, L"Optional: PC Content (the 32 PC songs and the 9 PC skins)", true);
    Text(dc, r.pc_label, L"True Crime: Streets of LA · PC copy (the folder with TrueCrime.exe)", g_want_pc ? kDim : kBox, g_font);
    Button(dc, r.browse, L"Browse", false, g_want_pc && !busy);
    if (g_want_pc) {
        std::wstring why;
        bool ok = ValidPcCopy(EditText(), why);
        if (g_found.pc_ready && !ok) Text(dc, r.pc_status, g_found.pc_ready_text, kGood, g_font);
        else Text(dc, r.pc_status, why, ok ? kGood : kBad, g_font);
    }

    Check(dc, h, r.mk_box, g_want_makoto, L"Optional: Makoto Yuki (Persona 3)", true);
    if (g_want_makoto) Text(dc, r.mk_status, g_found.p3_text, g_found.p3 || g_found.makoto_ready ? kGood : kBad, g_font);

    Text(dc, r.note, L"Use your own game files. Nothing is uploaded, and nothing else needs installing.", kDim, g_font);
    std::wstring stage, error;
    {
        std::lock_guard lock(g_text_lock);
        stage = g_stage;
        error = g_error;
    }
    if (g_failed) Text(dc, r.stage, L"Failed: " + error, kBad, g_font);
    else Text(dc, r.stage, busy ? stage : L"Ready to set up", busy ? kText : kDim, g_font);
    Fill(dc, r.bar, kPanel);
    RECT f = r.bar;
    f.right = r.bar.left + LONG(double(r.bar.right - r.bar.left) * g_done / std::max(1, g_total.load()));
    if (busy || g_finished) Fill(dc, f, kAccent);

    Button(dc, r.log, L"Open setup log", false, true);
    Button(dc, r.recheck, L"Check again", false, !busy);
    Button(dc, r.cancel, busy ? L"Stop" : L"Exit", false, true);
    Button(dc, r.go, L"Set up & play", true, !busy);
    BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

void Browse(HWND h) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Your True Crime: Streets of LA PC folder (the one with TrueCrime.exe)");
    if (SUCCEEDED(dlg->Show(h))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                SetWindowTextW(g_edit, path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    InvalidateRect(h, nullptr, FALSE);
}

void ShowEdit(HWND h) {
    Rects r = Arrange(h);
    SetWindowPos(g_edit, nullptr, r.edit.left, r.edit.top + 2, r.edit.right - r.edit.left, r.edit.bottom - r.edit.top - 4,
                 SWP_NOZORDER);
    EnableWindow(g_edit, g_want_pc && !g_working);
}

void Start(HWND h) {
    g_failed = false;
    g_finished = false;
    g_found = Detect();
    std::wstring problem;
    if (!g_found.game) problem = L"True Crime: Streets of LA (Xbox) is not in the assets folder.";
    std::wstring why;
    bool pc_path = g_want_pc && ValidPcCopy(EditText(), why);
    if (problem.empty() && g_want_pc && !pc_path && !g_found.pc_ready) problem = L"PC Content: " + why + L".";
    if (problem.empty() && g_want_makoto && !g_found.p3 && !g_found.makoto_ready)
        problem = L"Makoto Yuki: Persona 3 FES (USA) is not in the assets folder.";
    if (!problem.empty()) {
        {
            std::lock_guard lock(g_text_lock);
            g_error = problem;
        }
        g_failed = true;
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    Plan p;
    // already made: kept as it is (a valid PC copy browsed to makes the PC content again)
    p.pc = g_want_pc && pc_path;
    p.pc_dir = fs::path(EditText());
    p.makoto = g_want_makoto && !g_found.makoto_ready;
    p.p3_iso = g_found.p3_iso;
    SetupLog("---- setup: PC content " + std::string(p.pc ? "yes (" + Narrow(p.pc_dir.wstring()) + ")" : "no") +
             ", Makoto " + (p.makoto ? "yes" : "no"));
    if (!p.pc && !p.makoto) {
        g_start = true;
        DestroyWindow(h);
        return;
    }
    g_working = true;
    g_cancel = false;
    ShowEdit(h);
    std::thread(Work, p).detach();
    InvalidateRect(h, nullptr, FALSE);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_PAINT:
            Paint(h);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = HDC(wp);
            SetTextColor(dc, g_want_pc ? kText : kDim);
            SetBkColor(dc, kBox);
            return LRESULT(g_edit_brush);
        }
        case WM_COMMAND:
            if (HWND(lp) == g_edit && HIWORD(wp) == EN_CHANGE) InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            POINT p{short(LOWORD(lp)), short(HIWORD(lp))};
            Rects r = Arrange(h);
            bool busy = g_working;
            if (PtInRect(&r.log, p)) {
                SetupLog("(log opened)");
                ShellExecuteW(h, L"open", LogFile().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (busy) {
                if (PtInRect(&r.cancel, p)) g_cancel = true;
            } else if (PtInRect(&r.pc_box, p)) {
                g_want_pc = !g_want_pc;
                ShowEdit(h);
                if (g_want_pc) SetFocus(g_edit);
            } else if (PtInRect(&r.mk_box, p)) {
                g_want_makoto = !g_want_makoto;
                g_found = Detect();
            } else if (PtInRect(&r.browse, p) && g_want_pc) {
                Browse(h);
            } else if (PtInRect(&r.recheck, p)) {
                g_found = Detect();
                g_failed = false;
            } else if (PtInRect(&r.cancel, p)) {
                DestroyWindow(h);
            } else if (PtInRect(&r.go, p)) {
                Start(h);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case kWorkDone:
            ShowEdit(h);
            if (g_finished) {
                g_start = true;
                DestroyWindow(h);
            } else {
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_CLOSE:
            if (g_working) {
                g_cancel = true;
                return 0;
            }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

HFONT Font(int size, int weight, UINT dpi) {
    NONCLIENTMETRICSW nm{sizeof(nm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(nm), &nm, 0, dpi);
    LOGFONTW lf = nm.lfMessageFont;
    lf.lfHeight = -MulDiv(size, int(dpi), 72);
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

bool RunWindow() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    std::wstring cls = std::wstring(app::kGame.name) + L"Setup";
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, MulDiv(720, int(dpi), 96), MulDiv(560, int(dpi), 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    g_hwnd = CreateWindowExW(0, cls.c_str(), (std::wstring(app::kGame.title) + L" - Setup").c_str(), style,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2, w, hgt,
                             nullptr, nullptr, inst, nullptr);
    g_font = Font(10, FW_NORMAL, dpi);
    g_bold = Font(10, FW_SEMIBOLD, dpi);
    g_title = Font(20, FW_BOLD, dpi);
    g_edit_brush = CreateSolidBrush(kBox);
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, nullptr, inst,
                             nullptr);
    SendMessageW(g_edit, WM_SETFONT, WPARAM(g_font), TRUE);
    g_found = Detect();
    g_want_makoto = g_found.p3 || g_found.makoto_ready;  // ticked when its disc, or Makoto already made, is there
    g_want_pc = g_found.pc_ready;                        // ticked when the PC content is already made
    ShowEdit(g_hwnd);
    SetTimer(g_hwnd, 1, 200, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_hwnd = nullptr;
    DeleteObject(g_font);
    DeleteObject(g_bold);
    DeleteObject(g_title);
    DeleteObject(g_edit_brush);
    UnregisterClassW(cls.c_str(), inst);
    if (SUCCEEDED(co)) CoUninitialize();
    return g_start;
}

// ------------------------------------------------------------------------------------------- from a .bat
// TrueCrimeLA.exe --import-pc-skins "<PC copy>"     the 9 PC skins (import_pc_skins.bat)
// TrueCrimeLA.exe --import-makoto ["<P3 FES .iso>"]  Makoto (makoto_skin.bat); without the .iso: the one in assets
// No window: what it does goes to the .bat's console, then the exe ends (0 done, 1 failed).
int ImportFromCommandLine(const std::vector<std::wstring>& args) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    auto say = [](const std::string& s) {
        std::printf("  %s\n", s.c_str());
        std::fflush(stdout);
        SetupLog(s);
    };
    std::wstring what = args[1];
    std::wstring arg = args.size() > 2 ? args[2] : L"";
    while (!arg.empty() && (arg.back() == L'\\' || arg.back() == L'"' || arg.back() == L' ')) arg.pop_back();
    try {
        fs::path extras = Assets() / "extras";
        if (what == L"--import-pc-skins") {
            std::wstring why;
            if (!ValidPcCopy(arg, why)) throw std::runtime_error(Narrow(why));
            fs::path chars = fs::path(arg) / "Data" / "Char";
            for (const auto& sk : pcskins::kSkins) {
                std::string n = "CHAR" + std::to_string(sk.pc_char);
                auto out = pcskins::ConvertPcCharacter(pcskins::ReadFile(chars / (n + ".PCM")), pcskins::ReadFile(chars / (n + ".RES")));
                WriteBytes(extras / "Data" / "Char" / (std::string(sk.code) + ".XBX"), out);
                char line[96];
                std::snprintf(line, sizeof(line), "%-8s %s", sk.code, sk.who);
                say(line);
            }
            say("Done: 9 PC skins in " + Narrow((extras / "Data" / "Char").wstring()));
            say("Enter a code above as the license plate (hold L and R on OK, as the console's codes).");
        } else {
            fs::path iso = arg;
            std::string why;
            if (iso.empty()) {
                for (auto& p : Isos(Assets()))
                    if (p3makoto::IsP3FesUsa(p, why)) {
                        iso = p;
                        break;
                    }
                if (iso.empty())
                    throw std::runtime_error("no Persona 3 FES (USA) .iso in " + Narrow(Assets().wstring()) +
                                             ": put your own copy there, or drag it onto makoto_skin.bat");
            } else if (!p3makoto::IsP3FesUsa(iso, why)) {
                throw std::runtime_error(Narrow(iso.filename().wstring()) + ": " + why);
            }
            say("Persona 3 FES: " + Narrow(iso.filename().wstring()));
            auto out = p3makoto::MakotoXbx(iso, p3makoto::NickFrom(Assets()));
            WriteBytes(extras / "Data" / "Char" / "M3M3NT0.XBX", out);
            say("Done: " + Narrow((extras / "Data" / "Char" / "M3M3NT0.XBX").wstring()) + " (with his HUD portrait)");
            say("Enter M3M3NT0 as the license plate (hold L and R on OK, as the console's codes).");
        }
        return 0;
    } catch (const std::exception& e) {
        say(std::string("Failed: ") + e.what());
        return 1;
    }
}

}  // namespace

// GameInfo::init: after the settings load, before the game is looked for and its code prepared
void TcSetup() {
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::vector<std::wstring> args(argv, argv + argc);
        LocalFree(argv);
        if (args.size() >= 2 && (args[1] == L"--import-pc-skins" || args[1] == L"--import-makoto")) {
            int code = ImportFromCommandLine(args);
            logx::Flush();
            ExitProcess(UINT(code));
        }
    }
    bool asked = std::wcsstr(GetCommandLineW(), L"--setup") != nullptr || (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    if (app::Instance() > 1 || std::wcsstr(GetCommandLineW(), L"--relaunched")) return;
    // the release opens it at every start: done content shows as already set up, new content can be added
    // only the release players download opens it by itself: a build from the source (out\build\...) has its CMake
    // files beside the exe, and gets the window only when asked for
    std::error_code ec;
    if (!asked && (std::filesystem::exists(app::ExeDir() / "CMakeCache.txt", ec) ||
                   std::filesystem::exists(app::ExeDir() / "build.ninja", ec)))
        return;
    if (!RunWindow()) {
        LOGR("setup: closed without setting up");
        logx::Flush();
        ExitProcess(0);
    }
    g_tc_setup_done = true;
    app::Save();
}
