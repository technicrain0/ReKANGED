#pragma once
// [TCSOLA] The PC version's extra player skins (its own license plate codes), made for the port from the player's
// own PC copy at setup (tc_setup.cpp). Nothing of the game is in here: it reads the PC files and writes Xbox
// character files the port loads like the console's own skins.
//   <PC copy>\Data\Char\CHARn.PCM + CHARn.RES  ->  assets\extras\Data\Char\<CODE>.XBX
//
// The PC's character files are the Xbox's with the big data moved into the .RES: skeleton, collision boxes, bounds
// and textures are the same bytes, the meshes use plain floats and index buffers. This rebuilds the Xbox vertex
// formats and triangle strips (checked against Nick and M1K3, which both versions have: identical geometry, bone
// weights, textures and skeleton).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace pcskins {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// the PC version's own plate codes, with the PC file each one loads (TrueCrime.exe's skin table, skins 21 to 29)
struct Skin {
    const char* code;  // plate code, also the file name: <code>.XBX
    int pc_char;       // Data\Char\CHAR<n>.PCM / .RES
    const char* who;
};
const Skin kSkins[] = {
    {"KNOICHI", 105, "Ayame"},
    {"VAKLAM", 106, "Jeanette"},
    {"DOGTAGS", 107, "Call of Duty soldier"},
    {"GENIN", 108, "Rikimaru"},
    {"AWYEAH", 109, "Taurus"},
    {"RECKON", 110, "Desperado"},
    {"THUGLYF", 111, "Officer Dick"},
    {"NOMAYO", 112, "Sandwich Guy"},
    {"ACTVZZL", 99, "Snoop Dogg"},
};

constexpr uint8_t kPad = 0x55;

[[noreturn]] void Fail(const std::string& m) { throw std::runtime_error(m); }

uint32_t Align(uint32_t n, uint32_t a) { return (n + a - 1) / a * a; }

uint32_t U32(const Bytes& d, size_t o) {
    if (o + 4 > d.size()) Fail("read past the end of a file");
    uint32_t v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}
uint16_t U16(const Bytes& d, size_t o) {
    if (o + 2 > d.size()) Fail("read past the end of a file");
    uint16_t v;
    std::memcpy(&v, d.data() + o, 2);
    return v;
}
float F32(const Bytes& d, size_t o) {
    uint32_t u = U32(d, o);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
template <class T> void Put(Bytes& d, T v) {
    uint8_t b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    d.insert(d.end(), b, b + sizeof(T));
}
template <class T> void PutAt(Bytes& d, size_t o, T v) { std::memcpy(d.data() + o, &v, sizeof(T)); }
void PadTo(Bytes& d, uint32_t a) { d.resize(Align(uint32_t(d.size()), a), kPad); }

// Python's round(): halves to even, as the reference converter does
double RoundEven(double x) { return std::nearbyint(x); }

int16_t S16(float x) {
    double v = RoundEven(double(x) * 32767.0);
    return int16_t(std::clamp(v, -32768.0, 32767.0));
}

uint32_t PackNormal(float x, float y, float z) {
    auto c = [](float v) { return std::clamp(double(v), -1.0, 1.0); };
    uint32_t px = uint32_t(int32_t(RoundEven(c(x) * 1023))) & 0x7FF;
    uint32_t py = uint32_t(int32_t(RoundEven(c(y) * 1023))) & 0x7FF;
    uint32_t pz = uint32_t(int32_t(RoundEven(c(z) * 511))) & 0x3FF;
    return px | (py << 11) | (pz << 22);
}

struct V8 { float f[8]; };  // position, normal, uv as the PC stores them

V8 Vert(const Bytes& d, size_t o) {
    V8 v;
    for (int i = 0; i < 8; i++) v.f[i] = F32(d, o + 4 * i);
    return v;
}

// rigid: NORMSHORT3 position, NORMPACKED3 normal, NORMSHORT2 uv (14 bytes)
void Vert14(Bytes& out, const V8& v) {
    Put(out, S16(v.f[0])); Put(out, S16(v.f[1])); Put(out, S16(v.f[2]));
    Put(out, PackNormal(v.f[3], v.f[4], v.f[5]));
    Put(out, S16(v.f[6])); Put(out, S16(v.f[7]));
}
// skinned: float3 position, NORMPACKED3 normal, NORMSHORT2 uv (20 bytes)
void Vert20(Bytes& out, const V8& v) {
    Put(out, v.f[0]); Put(out, v.f[1]); Put(out, v.f[2]);
    Put(out, PackNormal(v.f[3], v.f[4], v.f[5]));
    Put(out, S16(v.f[6])); Put(out, S16(v.f[7]));
}
// The Xbox shader keeps bone slot 19 for itself: PC slots 19 and up are one higher on the Xbox
int XboxSlot(int i) { return i < 19 ? i : i + 1; }

struct Batch {
    uint32_t flags, count, colour, tex, start;
    bool colour_block;  // colour is an index into the colour blocks, not a colour
    float f;
};
struct Node {
    uint32_t flags;
    uint8_t bounds[32];
    std::vector<Batch> batches;
    Bytes data;
};

struct Converter {
    const Bytes& P;
    const Bytes& R;
    std::vector<Bytes> colour_blocks;  // 16-byte material colours, placed at 0x60 as in the Xbox files

    uint32_t Colour(uint32_t flags, uint32_t value, bool& block) {
        block = false;
        if (!(flags & 0x4000)) return value;
        if (value + 16 > P.size()) Fail("a colour block lies outside the PCM");
        Bytes b(P.begin() + value, P.begin() + value + 16);
        auto it = std::find(colour_blocks.begin(), colour_blocks.end(), b);
        block = true;
        if (it != colour_blocks.end()) return uint32_t(it - colour_blocks.begin());
        colour_blocks.push_back(b);
        return uint32_t(colour_blocks.size() - 1);
    }

    Node MakeNode(uint32_t o, int kind) {
        uint32_t h[8];
        for (int i = 0; i < 8; i++) h[i] = U32(P, o + 4 * i);
        uint32_t nb = h[0] >> 24;
        Node n;
        n.flags = (h[0] & 0xFFFFFF) & ~4u;
        if (o + 0x40 > P.size()) Fail("a mesh lies outside the PCM");
        std::memcpy(n.bounds, P.data() + o + 0x20, 32);
        uint32_t total = 0;
        auto batch = [&](uint32_t b, uint32_t w[8]) {
            for (int i = 0; i < 8; i++) w[i] = U32(P, o + 0x50 + 32 * b + 4 * i);
        };
        if (kind == 7 || (h[1] == 1 && h[2] == 0x70)) {
            // inline, non-indexed strip: u32 x3, vertex count, 32-byte batches, then the vertices
            uint32_t nv = U32(P, o + 0x4C);
            uint32_t vo = o + 0x50 + 32 * nb;
            for (uint32_t b = 0; b < nb; b++) {
                uint32_t w[8];
                batch(b, w);
                Batch bt;
                bt.flags = w[0];
                bt.count = w[1] >> 16;
                bt.tex = w[3] & 0xFFFF;
                uint32_t start = w[3] >> 16;
                std::memcpy(&bt.f, &w[5], 4);
                bt.colour = Colour(bt.flags, w[2], bt.colour_block);
                bt.start = total;
                if (start + bt.count > nv) Fail("a strip runs past its vertices");
                for (uint32_t k = start; k < start + bt.count; k++) Vert14(n.data, Vert(P, vo + 32 * k));
                total += bt.count;
                n.batches.push_back(bt);
            }
            return n;
        }
        // vertices and indices in the RES
        uint32_t stride = h[1] >> 16, voff = h[2];
        if (stride != 32) Fail("unexpected vertex stride " + std::to_string(stride));
        bool skinned = kind == 0 && (h[4] >> 16) == 8;
        uint32_t ioff = U32(P, o + 0x44), counts = U32(P, o + 0x4C);
        uint32_t nidx = counts >> 16, nvert = counts & 0xFFFF;
        std::vector<uint16_t> idx(nidx);
        for (uint32_t i = 0; i < nidx; i++) idx[i] = U16(R, ioff + 2 * i);
        Bytes s1;
        for (uint32_t b = 0; b < nb; b++) {
            uint32_t w[8];
            batch(b, w);
            Batch bt;
            bt.flags = w[0];
            bt.tex = w[3] & 0xFFFF;
            uint32_t sidx = w[6] & 0xFFFF, icnt = w[6] >> 16, prim = w[7];
            std::memcpy(&bt.f, &w[5], 4);
            if (prim != 0x105 && (prim & 0xFF) != 5) Fail("a mesh is not a triangle strip");
            if (sidx + icnt > nidx) Fail("a strip runs past its indices");
            bt.count = icnt;
            bt.colour = Colour(bt.flags, w[2], bt.colour_block);
            bt.start = total;
            for (uint32_t k = sidx; k < sidx + icnt; k++) {
                uint32_t i = idx[k];
                if (i >= nvert) Fail("an index past the vertices");
                V8 v = Vert(R, voff + 32 * i);
                if (!skinned) {
                    Vert14(n.data, v);
                } else {
                    Vert20(n.data, v);
                    uint32_t so = h[5] + 8 * i;
                    if (so + 8 > R.size()) Fail("bone weights past the end of the RES");
                    for (int j = 0; j < 4; j++) Put(s1, int16_t(-96 + 3 * XboxSlot(R[so + j])));
                    for (int j = 0; j < 4; j++) s1.push_back(R[so + 4 + j]);
                }
            }
            total += icnt;
            n.batches.push_back(bt);
        }
        n.data.insert(n.data.end(), s1.begin(), s1.end());
        return n;
    }

    Bytes Build() {
        if (P.size() < 0x60) Fail("not a character PCM");
        struct Sec { uint16_t type, count; uint32_t off; };
        Sec sec[8];
        std::map<int, std::pair<uint32_t, uint32_t>> st;
        for (int i = 0; i < 8; i++) {
            sec[i] = {U16(P, 0x0C + 8 * i), U16(P, 0x0E + 8 * i), U32(P, 0x10 + 8 * i)};
            st[sec[i].type] = {sec[i].count, sec[i].off};
        }
        if (!st.count(4) || !st.count(0) || !st.count(2)) Fail("not a character PCM");
        auto [nb4, o4] = st[4];
        if (o4 + 0x30 * nb4 > P.size()) Fail("the skeleton lies outside the PCM");
        std::map<int, std::vector<Node>> nodes;
        for (int t : {0, 7}) {
            auto [c, o] = st[t];
            for (uint32_t i = 0; i < c; i++) nodes[t].push_back(MakeNode(o + U32(P, o + 4 * i), t));
        }
        Bytes out(P.begin(), P.begin() + 0x60);
        std::vector<uint32_t> colour_at;
        for (auto& b : colour_blocks) {
            colour_at.push_back(uint32_t(out.size()));
            out.insert(out.end(), b.begin(), b.end());
        }
        PadTo(out, 16);
        std::map<int, uint32_t> at;
        at[4] = uint32_t(out.size());
        out.insert(out.end(), P.begin() + o4, P.begin() + o4 + 0x30 * nb4);

        auto put_meshes = [&](int t) {
            PadTo(out, 16);
            uint32_t base = uint32_t(out.size());
            at[t] = base;
            auto& ns = nodes[t];
            uint32_t table = Align(4 * uint32_t(ns.size() + 1), 16);
            Bytes body;
            std::vector<uint32_t> offs;
            for (size_t k = 0; k < ns.size(); k++) {
                if (k) body.resize(Align(table + uint32_t(body.size()), 16) - table, kPad);
                offs.push_back(table + uint32_t(body.size()));
                const Node& n = ns[k];
                uint32_t hs = Align(0x40 + 24 * uint32_t(n.batches.size()), 16);
                Put(body, uint32_t((uint32_t(n.batches.size()) << 24) | n.flags));
                Put(body, 1u); Put(body, hs); Put(body, 0u); Put(body, 1u); Put(body, hs); Put(body, 0u); Put(body, 0u);
                body.insert(body.end(), n.bounds, n.bounds + 32);
                for (const Batch& b : n.batches) {
                    Put(body, b.flags); Put(body, uint16_t(0)); Put(body, uint16_t(b.count));
                    Put(body, b.colour_block ? colour_at[b.colour] : b.colour);
                    Put(body, uint16_t(b.tex)); Put(body, uint16_t(b.start)); Put(body, 0u); Put(body, b.f);
                }
                body.resize(offs.back() - table + hs, kPad);
                body.insert(body.end(), n.data.begin(), n.data.end());
            }
            offs.push_back(table + uint32_t(body.size()));
            for (uint32_t v : offs) Put(out, v);
            out.resize(base + table, kPad);
            out.insert(out.end(), body.begin(), body.end());
        };
        put_meshes(0);
        PadTo(out, 16);
        // type 1 (collision boxes): copied as they are, offset table and all
        at[1] = uint32_t(out.size());
        if (auto [c1, o1] = st[1]; c1) {
            uint32_t end = U32(P, o1 + 4 * c1);
            if (o1 + end > P.size()) Fail("the collision boxes lie outside the PCM");
            out.insert(out.end(), P.begin() + o1, P.begin() + o1 + end);
        }
        if (!nodes[7].empty()) put_meshes(7);
        else at[7] = uint32_t(out.size());
        PadTo(out, 16);
        // textures: the same headers, the data copied from the RES, 128-byte aligned in the file
        auto [c2, o2] = st[2];
        uint32_t tex_at = uint32_t(out.size());
        at[2] = at[8] = at[5] = tex_at;
        std::vector<std::array<uint32_t, 5>> heads(c2);
        for (uint32_t t = 0; t < c2; t++)
            for (int i = 0; i < 5; i++) heads[t][i] = U32(P, o2 + 20 * t + 4 * i);
        out.resize(out.size() + 20 * c2, 0);
        if (auto [c3, o3] = st[3]; c3) {  // type 3 (some PC characters): self-contained, copied as it is
            uint32_t end3 = 0;
            for (uint32_t i = 0; i < c3; i++) end3 = std::max(end3, U32(P, o3 + 4 * i) + 0x1C);
            if (o3 + end3 > P.size()) Fail("section 3 runs past the PCM");
            PadTo(out, 16);
            at[3] = uint32_t(out.size());
            out.insert(out.end(), P.begin() + o3, P.begin() + o3 + end3);
        }
        for (uint32_t t = 0; t < c2; t++) {
            auto& hd = heads[t];
            uint32_t fmt = hd[3], f8 = (fmt >> 8) & 0xFF, size = 0;
            if (hd[4]) {  // linear (the PC's HUD portraits are LIN_A8R8G8B8): pitch * height
                size = (((hd[4] >> 24) & 0xFF) + 1) * 64 * (((hd[4] >> 12) & 0xFFF) + 1);
            } else {
                uint32_t w = 1u << ((fmt >> 20) & 15), hh = 1u << ((fmt >> 24) & 15);
                uint32_t levels = std::max(1u, (fmt >> 16) & 15);
                uint32_t bs = f8 == 0x0C ? 8 : (f8 == 0x0E || f8 == 0x0F) ? 16 : 0;
                if (!bs) Fail("texture " + std::to_string(t) + " has a format this does not know");
                for (uint32_t l = 0; l < levels; l++)
                    size += std::max(1u, (w >> l) / 4) * std::max(1u, (hh >> l) / 4) * bs;
            }
            PadTo(out, 128);
            uint32_t src = hd[1];
            if (size_t(src) + size > R.size()) Fail("texture " + std::to_string(t) + " is cut short in the RES");
            hd[1] = uint32_t(out.size()) - tex_at;
            out.insert(out.end(), R.begin() + src, R.begin() + src + size);
            for (int i = 0; i < 5; i++) PutAt(out, tex_at + 20 * t + 4 * i, hd[i]);
        }
        PadTo(out, 128);
        for (int i = 0; i < 8; i++) {
            const Sec& s = sec[i];
            uint32_t off = (s.count || s.type == 8 || s.type == 5) ? (at.count(s.type) ? at[s.type] : 0) : 0;
            PutAt(out, 0x0C + 8 * i, s.type);
            PutAt(out, 0x0E + 8 * i, s.count);
            PutAt(out, 0x10 + 8 * i, off);
        }
        return out;
    }
};

Bytes ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) Fail("cannot open " + p.string());
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

inline Bytes ConvertPcCharacter(const Bytes& pcm, const Bytes& res) {
    Converter c{pcm, res, {}};
    return c.Build();
}

}  // namespace pcskins
