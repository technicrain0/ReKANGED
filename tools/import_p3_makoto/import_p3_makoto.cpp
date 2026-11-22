// import_p3_makoto: makes M3M3NT0.XBX, Makoto Yuki for the port's M3M3NT0 plate code, from the player's own
// Persona 3 FES disc (USA, SLUS-21621). Nothing of either game is in this program or in the port: it reads the
// player's P3 FES .iso and True Crime's NICK.XBX (from assets\Data\Char or the Xbox .iso in assets) and writes
// assets\extras\Data\Char\M3M3NT0.XBX.
//
//   import_p3_makoto <assets folder> [P3 FES .iso]          (without the .iso: the first P3 FES .iso in assets)
//   import_p3_makoto --rmd FC_01_00.RMD NICK.XBX OUT.XBX     (from files already taken off the discs)
//
// Steps: the disc (ISO 9660) -> BTL.CVM (an ISO 9660 image behind a header) -> MODEL\FIELD\FC_01_00.RMD, his winter
// uniform: a RenderWare clump with plain (not PS2-native) geometry, a skin and PS2 textures. Each of his main bones is
// turned and stretched so its joint lands on Nick's (the rest follow their parent), his hands take the weights of
// Nick's own hand where they touch it (the game's fist and grip poses are made for those), and the result replaces
// Nick's skinned body in a copy of NICK.XBX: Nick's skeleton, hit boxes and animations stay. The textures go into
// one 512x512 DXT1 sheet in Nick's head texture slot.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

[[noreturn]] void Fail(const std::string& m) { throw std::runtime_error(m); }

// ---------------------------------------------------------------- little helpers
template <class T> T Get(const Bytes& d, size_t o) {
    if (o + sizeof(T) > d.size()) Fail("a read past the end of a file");
    T v;
    std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}
template <class T> void Set(Bytes& d, size_t o, T v) {
    if (o + sizeof(T) > d.size()) Fail("a write past the end of the file");
    std::memcpy(d.data() + o, &v, sizeof(T));
}
template <class T> void Put(Bytes& d, T v) {
    uint8_t b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    d.insert(d.end(), b, b + sizeof(T));
}
double RoundEven(double x) { return std::nearbyint(x); }  // Python's round()
uint64_t Fnv(const Bytes& d) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : d) h = (h ^ b) * 0x100000001b3ull;
    return h;
}
std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(uint8_t(c)));
    return s;
}

// ---------------------------------------------------------------- small linear algebra
struct V3 {
    double x = 0, y = 0, z = 0;
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Len(const V3& a) { return std::sqrt(Dot(a, a)); }

struct M3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    V3 operator*(const V3& v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    M3 operator*(const M3& o) const {
        M3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                r.m[i][j] = 0;
                for (int k = 0; k < 3; k++) r.m[i][j] += m[i][k] * o.m[k][j];
            }
        return r;
    }
    M3 T() const {
        M3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i][j] = m[j][i];
        return r;
    }
    M3 Inv() const {
        const auto& a = m;
        double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        if (std::fabs(det) < 1e-300) Fail("a singular bone matrix");
        M3 r;
        r.m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
        r.m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
        r.m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
        r.m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
        r.m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
        r.m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
        r.m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
        r.m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
        r.m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
        return r;
    }
};
struct Xf {  // rotation-scale + translation
    M3 r;
    V3 t;
    V3 operator*(const V3& v) const { return r * v + t; }
    Xf operator*(const Xf& o) const { return {r * o.r, r * o.t + t}; }
};
M3 Rx(double a) { M3 r; double c = std::cos(a), s = std::sin(a); r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; return r; }
M3 Ry(double a) { M3 r; double c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][2] = s; r.m[2][0] = -s; r.m[2][2] = c; return r; }
M3 Rz(double a) { M3 r; double c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c; return r; }
M3 Outer(const V3& a, const V3& b) {
    M3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a[i] * b[j];
    return r;
}
M3 Add(const M3& a, const M3& b, double s = 1) {
    M3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[i][j] + s * b.m[i][j];
    return r;
}
M3 RotBetween(V3 a, V3 b) {
    a = a * (1 / Len(a));
    b = b * (1 / Len(b));
    V3 v = Cross(a, b);
    double c = Dot(a, b);
    if (c < -0.9999) {
        V3 p = std::fabs(a.x) < 0.9 ? V3{1, 0, 0} : V3{0, 1, 0};
        v = Cross(a, p);
        v = v * (1 / Len(v));
        M3 r = Outer(v, v);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i][j] = 2 * r.m[i][j] - (i == j ? 1 : 0);
        return r;
    }
    M3 K;
    K.m[0][0] = 0; K.m[0][1] = -v.z; K.m[0][2] = v.y;
    K.m[1][0] = v.z; K.m[1][1] = 0; K.m[1][2] = -v.x;
    K.m[2][0] = -v.y; K.m[2][1] = v.x; K.m[2][2] = 0;
    return Add(Add(M3{}, K), K * K, 1 / (1 + c));
}

// ---------------------------------------------------------------- reading discs
struct Reader {
    std::ifstream f;
    uint64_t size = 0;
    explicit Reader(const fs::path& p) : f(p, std::ios::binary) {
        if (!f) Fail("cannot open " + p.string());
        f.seekg(0, std::ios::end);
        size = uint64_t(f.tellg());
    }
    Bytes Read(uint64_t at, size_t n) {
        if (at + n > size) Fail("a read past the end of the disc image");
        Bytes b(n);
        f.seekg(std::streamoff(at));
        f.read(reinterpret_cast<char*>(b.data()), std::streamsize(n));
        if (!f) Fail("reading the disc image failed");
        return b;
    }
};

// ISO 9660 (the PS2 disc, and the image inside a CVM): LBAs from `base`
struct Iso {
    Reader& r;
    uint64_t base;
    struct Ent { uint64_t at; uint32_t size; bool dir; };
    Ent root{};
    Iso(Reader& rd, uint64_t b) : r(rd), base(b) {
        Bytes pvd = r.Read(base + 16 * 2048, 2048);
        if (pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0) Fail("not an ISO 9660 image");
        root = {base + uint64_t(Get<uint32_t>(pvd, 156 + 2)) * 2048, Get<uint32_t>(pvd, 156 + 10), true};
    }
    bool Find(const Ent& dir, const std::string& name, Ent& out) {
        Bytes t = r.Read(dir.at, dir.size);
        size_t o = 0;
        while (o < t.size()) {
            uint8_t len = t[o];
            if (!len) {
                o = (o / 2048 + 1) * 2048;
                continue;
            }
            if (o + 33 > t.size()) break;
            uint8_t nl = t[o + 32];
            std::string n(reinterpret_cast<const char*>(&t[o + 33]), std::min<size_t>(nl, t.size() - o - 33));
            if (auto semi = n.find(';'); semi != std::string::npos) n.resize(semi);
            if (Upper(n) == Upper(name)) {
                out = {base + uint64_t(Get<uint32_t>(t, o + 2)) * 2048, Get<uint32_t>(t, o + 10), (t[o + 25] & 2) != 0};
                return true;
            }
            o += len;
        }
        return false;
    }
    bool Path(const std::string& path, Ent& out) {
        Ent cur = root;
        size_t s = 0;
        while (s <= path.size()) {
            size_t e = path.find_first_of("/\\", s);
            if (e == std::string::npos) e = path.size();
            if (!Find(cur, path.substr(s, e - s), cur)) return false;
            s = e + 1;
        }
        out = cur;
        return true;
    }
};

// Xbox disc (XDVDFS): partitions at 0 (xiso) or 0x18300000 (whole dump), volume at sector 32
bool XboxFile(const fs::path& iso, const std::string& path, Bytes& out) {
    Reader r(iso);
    static const char kMagic[] = "MICROSOFT*XBOX*MEDIA";
    for (uint64_t base : {uint64_t(0), uint64_t(0x18300000)}) {
        if (base + 0x10000 + 2048 > r.size) continue;
        Bytes v = r.Read(base + 0x10000, 2048);
        if (std::memcmp(v.data(), kMagic, 20) != 0) continue;
        uint64_t at = base + uint64_t(Get<uint32_t>(v, 0x14)) * 2048;
        uint32_t size = Get<uint32_t>(v, 0x18);
        size_t s = 0;
        while (true) {
            size_t e = path.find_first_of("/\\", s);
            std::string part = path.substr(s, e == std::string::npos ? std::string::npos : e - s);
            if (size == 0 || size > (16u << 20)) return false;
            Bytes t = r.Read(at, size);
            std::vector<uint32_t> todo{0};
            std::set<uint32_t> seen;
            bool found = false;
            while (!todo.empty() && !found) {
                uint32_t o = todo.back();
                todo.pop_back();
                if (o + 0xE > t.size() || !seen.insert(o).second) continue;
                uint16_t left = Get<uint16_t>(t, o), right = Get<uint16_t>(t, o + 2);
                if (left == 0xFFFF) continue;
                uint8_t nl = t[o + 0xD];
                if (o + 0xE + nl > t.size()) continue;
                std::string n(reinterpret_cast<const char*>(&t[o + 0xE]), nl);
                if (Upper(n) == Upper(part)) {
                    at = base + uint64_t(Get<uint32_t>(t, o + 4)) * 2048;
                    size = Get<uint32_t>(t, o + 8);
                    found = true;
                }
                if (left) todo.push_back(uint32_t(left) * 4);
                if (right) todo.push_back(uint32_t(right) * 4);
            }
            if (!found) return false;
            if (e == std::string::npos) {
                out = r.Read(at, size);
                return true;
            }
            s = e + 1;
        }
    }
    return false;
}

Bytes ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) Fail("cannot open " + p.string());
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

// ---------------------------------------------------------------- the P3 model (RenderWare)
struct Chunk { uint32_t type; size_t at, size; };
std::vector<Chunk> Chunks(const Bytes& d, size_t o, size_t end) {
    std::vector<Chunk> r;
    while (o + 12 <= end) {
        uint32_t t = Get<uint32_t>(d, o), s = Get<uint32_t>(d, o + 4);
        if (o + 12 + s > end) break;
        r.push_back({t, o + 12, s});
        o += 12 + s;
    }
    return r;
}
std::vector<Chunk> Of(const std::vector<Chunk>& cs, uint32_t t) {
    std::vector<Chunk> r;
    for (auto& c : cs)
        if (c.type == t) r.push_back(c);
    return r;
}
Chunk One(const std::vector<Chunk>& cs, uint32_t t) {
    for (auto& c : cs)
        if (c.type == t) return c;
    Fail("the model is missing a part (chunk " + std::to_string(t) + ")");
}

struct Image {
    int w = 0, h = 0;
    std::vector<std::array<uint8_t, 3>> px;
};

int ClutFix(int i) { return (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1); }

// PS2 8-bit texture as RenderWare uploads it (PSMT8 in a PSMCT32 buffer): unswizzled
Image PS2Texture(const Bytes& d, int w, int h, int depth, size_t data) {
    Image img{w, h, std::vector<std::array<uint8_t, 3>>(size_t(w) * h)};
    if (depth == 8) {
        size_t px = data + 0x50, pal = px + size_t(w) * h + 0x50;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int block = (y & ~0xF) * w + (x & ~0xF) * 2;
                int swap = (((y + 2) >> 2) & 1) * 4;
                int py = (((y & ~3) >> 1) + (y & 1)) & 7;
                int col = py * w * 2 + ((x + swap) & 7) * 4;
                int byte = ((y >> 1) & 1) + ((x >> 2) & 2);
                int i = ClutFix(Get<uint8_t>(d, px + block + col + byte));
                for (int c = 0; c < 3; c++) img.px[size_t(y) * w + x][c] = Get<uint8_t>(d, pal + 4 * i + c);
            }
    } else if (depth == 4) {
        size_t px = data + 0x50, pal = px + size_t(w) * h / 2 + 0x50;
        for (int k = 0; k < w * h; k++) {
            uint8_t b = Get<uint8_t>(d, px + k / 2);
            int i = (k & 1) ? b >> 4 : b & 15;
            for (int c = 0; c < 3; c++) img.px[k][c] = Get<uint8_t>(d, pal + 4 * i + c);
        }
    } else {
        Fail("a texture of " + std::to_string(depth) + " bits");
    }
    return img;
}

struct Geometry {
    int nv = 0;
    std::vector<V3> pos, nrm;
    std::vector<std::array<float, 2>> uv;
    std::vector<std::array<int, 3>> tri;
    std::vector<int> mat;
    std::vector<std::string> mats;  // texture per material
    bool skin = false;
    std::vector<std::array<uint8_t, 4>> bi;
    std::vector<std::array<float, 4>> bw;
};
struct Frame { M3 rot; V3 pos; int parent; int hanim = -1; };
struct Model {
    std::vector<Frame> frames;
    std::vector<int> hanim_ids;  // HAnim node k's id
    std::vector<Geometry> geoms;
    std::vector<std::array<uint32_t, 2>> atomics;  // frame, geometry
    std::vector<std::string> tex_order;
    std::map<std::string, Image> tex;
};

Geometry ReadGeometry(const Bytes& d, const Chunk& g) {
    auto gc = Chunks(d, g.at, g.at + g.size);
    size_t o = gc.at(0).at;
    uint32_t fmt = Get<uint32_t>(d, o), nt = Get<uint32_t>(d, o + 4), nv = Get<uint32_t>(d, o + 8);
    o += 16;
    uint32_t flags = fmt & 0xFFFF;
    if (fmt & 0x01000000) Fail("the model's geometry is PS2-native, which this does not read");
    int ntex = (fmt >> 16) & 0xFF;
    if (!ntex) ntex = (flags & 0x80) ? 2 : (flags & 4) ? 1 : 0;
    Geometry G;
    G.nv = int(nv);
    if (flags & 8) o += size_t(nv) * 4;
    for (int k = 0; k < ntex; k++) {
        if (k == 0)
            for (uint32_t v = 0; v < nv; v++) G.uv.push_back({Get<float>(d, o + 8 * v), Get<float>(d, o + 8 * v + 4)});
        o += size_t(nv) * 8;
    }
    if (G.uv.empty()) G.uv.assign(nv, {0, 0});
    for (uint32_t t = 0; t < nt; t++) {
        uint16_t a = Get<uint16_t>(d, o), b = Get<uint16_t>(d, o + 2), m = Get<uint16_t>(d, o + 4), c = Get<uint16_t>(d, o + 6);
        G.tri.push_back({b, a, c});
        G.mat.push_back(m);
        o += 8;
    }
    o += 16;
    uint32_t hv = Get<uint32_t>(d, o), hn = Get<uint32_t>(d, o + 4);
    o += 8;
    if (!hv) Fail("a mesh without vertices");
    for (uint32_t v = 0; v < nv; v++) G.pos.push_back({Get<float>(d, o + 12 * v), Get<float>(d, o + 12 * v + 4), Get<float>(d, o + 12 * v + 8)});
    o += size_t(nv) * 12;
    if (hn) {
        for (uint32_t v = 0; v < nv; v++) G.nrm.push_back({Get<float>(d, o + 12 * v), Get<float>(d, o + 12 * v + 4), Get<float>(d, o + 12 * v + 8)});
        o += size_t(nv) * 12;
    } else {
        G.nrm.assign(nv, {0, 1, 0});
    }
    // materials: -1 is a new one, else a copy of an earlier entry
    Chunk ml = One(gc, 0x08);
    auto mlc = Chunks(d, ml.at, ml.at + ml.size);
    uint32_t nmi = Get<uint32_t>(d, mlc.at(0).at);
    std::vector<std::string> made;
    for (auto& m : Of(mlc, 0x07)) {
        auto mc = Chunks(d, m.at, m.at + m.size);
        std::string name;
        auto tx = Of(mc, 0x06);
        if (!tx.empty()) {
            auto tc = Chunks(d, tx[0].at, tx[0].at + tx[0].size);
            Chunk s = One(tc, 2);
            name = std::string(reinterpret_cast<const char*>(&d[s.at]), strnlen(reinterpret_cast<const char*>(&d[s.at]), s.size));
        }
        made.push_back(name);
    }
    size_t k = 0;
    for (uint32_t i = 0; i < nmi; i++) {
        int32_t ix = Get<int32_t>(d, mlc.at(0).at + 4 + 4 * i);
        if (ix < 0) {
            if (k >= made.size()) Fail("a broken material list");
            G.mats.push_back(made[k++]);
        } else {
            if (size_t(ix) >= G.mats.size()) Fail("a broken material list");
            G.mats.push_back(G.mats[size_t(ix)]);
        }
    }
    for (auto& e : Of(gc, 3))
        for (auto& c : Chunks(d, e.at, e.at + e.size)) {
            if (c.type != 0x116) continue;
            size_t p = c.at;
            uint8_t nu = d.at(p + 1);
            p += 4 + nu;
            for (uint32_t v = 0; v < nv; v++) G.bi.push_back({d.at(p + 4 * v), d.at(p + 4 * v + 1), d.at(p + 4 * v + 2), d.at(p + 4 * v + 3)});
            p += size_t(nv) * 4;
            for (uint32_t v = 0; v < nv; v++)
                G.bw.push_back({Get<float>(d, p + 16 * v), Get<float>(d, p + 16 * v + 4), Get<float>(d, p + 16 * v + 8), Get<float>(d, p + 16 * v + 12)});
            G.skin = true;
        }
    return G;
}

Model ReadModel(const Bytes& d) {
    Model M;
    auto top = Chunks(d, 0, d.size());
    // textures
    for (auto& td : Of(top, 0x16))
        for (auto& tn : Of(Chunks(d, td.at, td.at + td.size), 0x15)) {
            auto sub = Chunks(d, tn.at, tn.at + tn.size);
            if (sub.size() < 4) continue;
            std::string name(reinterpret_cast<const char*>(&d[sub[1].at]), strnlen(reinterpret_cast<const char*>(&d[sub[1].at]), sub[1].size));
            auto inner = Chunks(d, sub[3].at, sub[3].at + sub[3].size);
            if (inner.size() < 2) continue;
            int w = int(Get<uint32_t>(d, inner[0].at)), h = int(Get<uint32_t>(d, inner[0].at + 4)), depth = int(Get<uint32_t>(d, inner[0].at + 8));
            M.tex[name] = PS2Texture(d, w, h, depth, inner[1].at);
            M.tex_order.push_back(name);
        }
    Chunk clump = One(top, 0x10);
    auto cl = Chunks(d, clump.at, clump.at + clump.size);
    Chunk fl = One(cl, 0x0E);
    auto flc = Chunks(d, fl.at, fl.at + fl.size);
    uint32_t n = Get<uint32_t>(d, flc.at(0).at);
    for (uint32_t i = 0; i < n; i++) {
        size_t o = flc[0].at + 4 + 56 * i;
        Frame f;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) f.rot.m[c][r] = Get<float>(d, o + 4 * (3 * r + c));  // stored by rows: transposed
        f.pos = {Get<float>(d, o + 36), Get<float>(d, o + 40), Get<float>(d, o + 44)};
        f.parent = Get<int32_t>(d, o + 48);
        M.frames.push_back(f);
    }
    for (uint32_t i = 0; i < n && 1 + i < flc.size(); i++)
        for (auto& c : Chunks(d, flc[1 + i].at, flc[1 + i].at + flc[1 + i].size)) {
            if (c.type != 0x11E) continue;
            M.frames[i].hanim = Get<int32_t>(d, c.at + 4);
            int32_t nb = Get<int32_t>(d, c.at + 8);
            if (nb > 0) {
                M.hanim_ids.clear();
                for (int k = 0; k < nb; k++) M.hanim_ids.push_back(Get<int32_t>(d, c.at + 20 + 12 * k));
            }
        }
    Chunk gl = One(cl, 0x1A);
    for (auto& g : Of(Chunks(d, gl.at, gl.at + gl.size), 0x0F)) M.geoms.push_back(ReadGeometry(d, g));
    for (auto& a : Of(cl, 0x14)) {
        auto ac = Chunks(d, a.at, a.at + a.size);
        M.atomics.push_back({Get<uint32_t>(d, ac.at(0).at), Get<uint32_t>(d, ac.at(0).at + 4)});
    }
    if (M.hanim_ids.empty()) Fail("the model has no bone list");
    return M;
}

// ---------------------------------------------------------------- Nick (the .XBX)
const char* const kNames[65] = {
    "Root", "Root_Aux", "Marker_Ground", "Body", "Hips", "Spine1", "Spine2", "Spine3", "Chest", "Head",
    "Face", "Face", "Face", "Face", "Face", "Face", "Face", "Face", "Face", "Face", "Face", "Face", "Face", "Face",
    "Face", "Face", "Face", "Face", "Clavicle_R", "UpperArm_R", "Forearm_R", "Hand_R", "Thumb1_R", "Thumb2_R",
    "Thumb3_R", "Index1_R", "Index2_R", "Index3_R", "Fingers1_R", "Fingers2_R", "Fingers3_R", "Weapon_R",
    "Clavicle_L", "UpperArm_L", "Forearm_L", "Hand_L", "Thumb1_L", "Thumb2_L", "Thumb3_L", "Index1_L", "Index2_L",
    "Index3_L", "Fingers1_L", "Fingers2_L", "Fingers3_L", "Weapon_L", "Thigh_R", "Calf_R", "Foot_R", "Toe_R",
    "Thigh_L", "Calf_L", "Foot_L", "Toe_L", "Extra"};
int Bone(const char* name) {
    for (int i = 0; i < 65; i++)
        if (std::strcmp(kNames[i], name) == 0) return i;
    Fail(std::string("no bone ") + name);
}
constexpr int kBodyNode = 2, kBodyBone = 3, kTexSlot = 2, kAtlas = 512;
bool InPalette(int b) { return b >= 4 && b < 64 && b != 41 && b != 55; }
int PaletteIndex(int b) {
    int k = 0;
    for (int i = 4; i < b; i++)
        if (InPalette(i)) k++;
    return k;
}
int PaletteBone(int k) {
    for (int i = 4; i < 64; i++)
        if (InPalette(i) && k-- == 0) return i;
    return 4;
}
bool HandBone(int b) { return (b >= 31 && b <= 40) || (b >= 45 && b <= 54); }
bool LeftBone(int b) { return (b >= 42 && b <= 55) || (b >= 60 && b <= 63); }

struct Section { uint16_t type, count; uint32_t off; };
std::vector<Section> Sections(const Bytes& d) {
    std::vector<Section> s;
    for (int i = 0; i < 8; i++) s.push_back({Get<uint16_t>(d, 0x0C + 8 * i), Get<uint16_t>(d, 0x0E + 8 * i), Get<uint32_t>(d, 0x10 + 8 * i)});
    return s;
}
Section Sec(const Bytes& d, int t) {
    for (auto& s : Sections(d))
        if (s.type == t) return s;
    Fail("NICK.XBX is missing a section");
}

std::vector<Xf> Skeleton(const Bytes& d) {
    Section s = Sec(d, 4);
    int n = s.count;
    std::vector<int> parent(n, -1);
    std::function<void(int, int)> walk = [&](int i, int p) {
        int guard = 0;
        while (i != 0xFFFF && i < n && guard++ < 256) {
            parent[i] = p;
            uint16_t child = Get<uint16_t>(d, s.off + i * 0x30 + 8);
            if (child != 0xFFFF) walk(child, i);
            i = Get<uint16_t>(d, s.off + i * 0x30 + 6);
        }
    };
    walk(0, -1);
    std::vector<Xf> W(n);
    std::vector<bool> done(n);
    std::function<Xf(int)> world = [&](int i) -> Xf {
        if (!done[i]) {
            size_t o = s.off + size_t(i) * 0x30;
            Xf m{Rx(Get<float>(d, o + 32)) * Ry(Get<float>(d, o + 36)) * Rz(Get<float>(d, o + 40)),
                 {Get<float>(d, o + 16), Get<float>(d, o + 20), Get<float>(d, o + 24)}};
            W[i] = parent[i] >= 0 ? world(parent[i]) * m : m;
            done[i] = true;
        }
        return W[i];
    };
    for (int i = 0; i < n; i++) world(i);
    return W;
}

uint32_t PackNormal(V3 n) {
    double l = std::max(Len(n), 1e-9);
    n = n * (1 / l);
    auto c = [](double v) { return std::clamp(v, -1.0, 1.0); };
    uint32_t x = uint32_t(int32_t(RoundEven(c(n.x) * 1023))) & 0x7FF;
    uint32_t y = uint32_t(int32_t(RoundEven(c(n.y) * 1023))) & 0x7FF;
    uint32_t z = uint32_t(int32_t(RoundEven(c(n.z) * 511))) & 0x3FF;
    return x | (y << 11) | (z << 22);
}

// ---------------------------------------------------------------- textures out
Image Resize2x(const Image& s) {  // Lanczos-3 upscale by 2
    auto L = [](double x) {
        x = std::fabs(x);
        if (x < 1e-9) return 1.0;
        if (x >= 3) return 0.0;
        const double pi = 3.14159265358979323846;
        return 3 * std::sin(pi * x) * std::sin(pi * x / 3) / (pi * pi * x * x);
    };
    auto pass = [&](const std::vector<std::array<double, 3>>& in, int w, int h, bool horiz) {
        int ow = horiz ? w * 2 : w, oh = horiz ? h : h * 2;
        std::vector<std::array<double, 3>> out(size_t(ow) * oh);
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) {
                double c = ((horiz ? x : y) + 0.5) / 2 - 0.5;
                int lo = int(std::floor(c)) - 2, hi = int(std::floor(c)) + 3;
                std::array<double, 3> acc{0, 0, 0};
                double ws = 0;
                for (int k = lo; k <= hi; k++) {
                    double w0 = L(k - c);
                    int kk = std::clamp(k, 0, (horiz ? w : h) - 1);
                    const auto& p = horiz ? in[size_t(y) * w + kk] : in[size_t(kk) * w + x];
                    for (int ch = 0; ch < 3; ch++) acc[ch] += w0 * p[ch];
                    ws += w0;
                }
                for (int ch = 0; ch < 3; ch++) out[size_t(y) * ow + x][ch] = acc[ch] / ws;
            }
        return out;
    };
    std::vector<std::array<double, 3>> a(s.px.size());
    for (size_t i = 0; i < a.size(); i++) a[i] = {double(s.px[i][0]), double(s.px[i][1]), double(s.px[i][2])};
    auto b = pass(a, s.w, s.h, true);
    auto c = pass(b, s.w * 2, s.h, false);
    Image o{s.w * 2, s.h * 2, std::vector<std::array<uint8_t, 3>>(c.size())};
    for (size_t i = 0; i < c.size(); i++)
        for (int ch = 0; ch < 3; ch++) o.px[i][ch] = uint8_t(std::clamp(RoundEven(c[i][ch]), 0.0, 255.0));
    return o;
}

Bytes Dxt1(const Image& img) {
    Bytes out;
    auto to565 = [](double r, double g, double b) {
        int R = int(std::clamp(RoundEven(r), 0.0, 255.0)), G = int(std::clamp(RoundEven(g), 0.0, 255.0)), B = int(std::clamp(RoundEven(b), 0.0, 255.0));
        return ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3);
    };
    auto from565 = [](int v) {
        return std::array<double, 3>{((v >> 11) & 31) * 255.0 / 31, ((v >> 5) & 63) * 255.0 / 63, (v & 31) * 255.0 / 31};
    };
    for (int by = 0; by < img.h; by += 4)
        for (int bx = 0; bx < img.w; bx += 4) {
            std::array<std::array<double, 3>, 16> p;
            double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
            for (int i = 0; i < 16; i++) {
                auto& q = img.px[size_t(by + i / 4) * img.w + bx + i % 4];
                for (int c = 0; c < 3; c++) {
                    p[i][c] = q[c];
                    lo[c] = std::min(lo[c], p[i][c]);
                    hi[c] = std::max(hi[c], p[i][c]);
                }
            }
            int a = to565(hi[0], hi[1], hi[2]), b = to565(lo[0], lo[1], lo[2]);
            int c0 = std::max(a, b), c1 = std::min(a, b);
            if (c0 == c1) {
                if (c0 < 0xFFFF) c0++;
                else if (c1 > 0) c1--;
            }
            auto p0 = from565(c0), p1 = from565(c1);
            std::array<std::array<double, 3>, 4> pal{p0, p1, std::array<double, 3>{}, std::array<double, 3>{}};
            for (int c = 0; c < 3; c++) {
                pal[2][c] = (2 * p0[c] + p1[c]) / 3;
                pal[3][c] = (p0[c] + 2 * p1[c]) / 3;
            }
            uint32_t bits = 0;
            for (int i = 0; i < 16; i++) {
                int best = 0;
                double bd = 1e18;
                for (int k = 0; k < 4; k++) {
                    double dd = 0;
                    for (int c = 0; c < 3; c++) dd += (p[i][c] - pal[k][c]) * (p[i][c] - pal[k][c]);
                    if (dd < bd) bd = dd, best = k;
                }
                bits |= uint32_t(best) << (2 * i);
            }
            Put(out, uint16_t(c0));
            Put(out, uint16_t(c1));
            Put(out, bits);
        }
    return out;
}

Image Half(const Image& s) {
    int w = std::max(s.w / 2, 4), h = std::max(s.h / 2, 4);
    Image o{w, h, std::vector<std::array<uint8_t, 3>>(size_t(w) * h)};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                int sum = 0, n = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int sx = std::min(x * 2 + dx, s.w - 1), sy = std::min(y * 2 + dy, s.h - 1);
                        sum += s.px[size_t(sy) * s.w + sx][c];
                        n++;
                    }
                o.px[size_t(y) * w + x][c] = uint8_t((sum + n / 2) / n);
            }
    return o;
}

// ---------------------------------------------------------------- the conversion
// P3's HAnim node -> Nick's bone. Makoto faces +Z with his left at +X, as Nick's _L bones.
const std::map<int, const char*> kMap = {
    {3, "Hips"}, {14, "Spine1"}, {15, "Spine2"}, {16, "Spine3"}, {17, "Chest"}, {34, "Head"},
    {18, "Clavicle_L"}, {19, "UpperArm_L"}, {20, "Forearm_L"}, {21, "Hand_L"},
    {22, "Thumb1_L"}, {23, "Thumb2_L"}, {24, "Thumb3_L"}, {25, "Index1_L"}, {26, "Index2_L"}, {27, "Index3_L"},
    {28, "Fingers1_L"}, {29, "Fingers2_L"}, {30, "Fingers3_L"},
    {51, "Clavicle_R"}, {52, "UpperArm_R"}, {53, "Forearm_R"}, {54, "Hand_R"},
    {55, "Thumb1_R"}, {56, "Thumb2_R"}, {57, "Thumb3_R"}, {58, "Index1_R"}, {59, "Index2_R"}, {60, "Index3_R"},
    {61, "Fingers1_R"}, {62, "Fingers2_R"}, {63, "Fingers3_R"},
    {4, "Thigh_L"}, {5, "Calf_L"}, {6, "Foot_L"}, {7, "Toe_L"},
    {9, "Thigh_R"}, {10, "Calf_R"}, {11, "Foot_R"}, {12, "Toe_R"}};
// the joint each mapped bone points at (its direction and length come from these)
const std::map<int, int> kChild = {{3, 14}, {14, 15}, {15, 16}, {16, 17}, {17, 34}, {18, 19}, {19, 20}, {20, 21},
                                   {22, 23}, {23, 24}, {25, 26}, {26, 27}, {28, 29}, {29, 30}, {51, 52}, {52, 53},
                                   {53, 54}, {55, 56}, {56, 57}, {58, 59}, {59, 60}, {61, 62}, {62, 63}, {4, 5},
                                   {5, 6}, {6, 7}, {9, 10}, {10, 11}, {11, 12}};
const std::set<int> kSpine = {3, 14, 15, 16, 17, 34};

using Weights = std::vector<std::pair<int, double>>;  // Nick bone, weight, in the order they were added
void AddW(Weights& w, int bone, double x) {
    for (auto& p : w)
        if (p.first == bone) {
            p.second += x;
            return;
        }
    w.push_back({bone, x});
}

struct Corner { int v; V3 n; double u, vv; };

std::vector<int> Stripify(const std::vector<std::array<int, 3>>& tris) {
    std::map<std::pair<int, int>, std::vector<int>> edge;
    for (int t = 0; t < int(tris.size()); t++) {
        auto [a, b, c] = tris[t];
        edge[{a, b}].push_back(t);
        edge[{b, c}].push_back(t);
        edge[{c, a}].push_back(t);
    }
    std::vector<bool> used(tris.size());
    auto take = [&](std::pair<int, int> e) {
        auto it = edge.find(e);
        if (it == edge.end()) return -1;
        for (int t : it->second)
            if (!used[t]) return t;
        return -1;
    };
    std::vector<int> S;
    for (int t0 = 0; t0 < int(tris.size()); t0++) {
        if (used[t0]) continue;
        used[t0] = true;
        auto [p, q, r] = tris[t0];
        if (!S.empty()) {
            int last = S.back();
            S.push_back(last);
            S.push_back(p);
            S.push_back(p);
            if ((S.size() - 1) % 2) S.push_back(p);
        } else {
            S.push_back(p);
        }
        S.push_back(q);
        S.push_back(r);
        while (true) {
            size_t i = S.size() - 2;
            std::pair<int, int> e = i % 2 == 0 ? std::make_pair(S[S.size() - 2], S.back()) : std::make_pair(S.back(), S[S.size() - 2]);
            int t = take(e);
            if (t < 0) break;
            used[t] = true;
            auto [a, b, c] = tris[t];
            int z = (a != e.first && a != e.second) ? a : (b != e.first && b != e.second) ? b : c;
            S.push_back(z);
        }
    }
    return S;
}

Bytes Convert(const Bytes& rmd, const Bytes& nick_in) {
    Model M = ReadModel(rmd);
    Bytes d = nick_in;
    std::vector<Xf> NW = Skeleton(d);
    if (NW.size() < 64) Fail("NICK.XBX's skeleton is not Nick's");
    auto N = [&](const char* name) { return NW[size_t(Bone(name))].t; };

    // P3 bind pose
    int nf = int(M.frames.size());
    std::vector<Xf> Wf(nf);
    std::vector<bool> wd(nf);
    std::function<Xf(int)> fw = [&](int i) -> Xf {
        if (!wd[i]) {
            Xf m{M.frames[i].rot, M.frames[i].pos};
            Wf[i] = M.frames[i].parent >= 0 ? fw(M.frames[i].parent) * m : m;
            wd[i] = true;
        }
        return Wf[i];
    };
    for (int i = 0; i < nf; i++) fw(i);
    std::vector<int> nodes;
    for (int id : M.hanim_ids) {
        int f = -1;
        for (int i = 0; i < nf; i++)
            if (M.frames[i].hanim == id) f = i;
        if (f < 0) Fail("a bone without its frame");
        nodes.push_back(f);
    }
    if (nodes.size() < 64) Fail("this is not the P3 protagonist's skeleton");
    std::map<int, int> f2k;
    for (int k = 0; k < int(nodes.size()); k++) f2k[nodes[k]] = k;
    std::vector<int> parent(nodes.size(), -1);
    for (int k = 0; k < int(nodes.size()); k++) {
        int p = M.frames[nodes[k]].parent;
        while (p >= 0 && !f2k.count(p)) p = M.frames[p].parent;
        parent[k] = p >= 0 ? f2k[p] : -1;
    }
    // Makoto's space -> Nick's: Y down, scaled so the hips are at Nick's height
    double s = std::fabs(N("Hips").y) / Wf[nodes[3]].t.y;
    M3 C;
    C.m[0][0] = s; C.m[1][1] = -s; C.m[2][2] = s;
    M3 Flip;
    Flip.m[1][1] = -1;
    std::vector<V3> B(nodes.size());
    for (int k = 0; k < int(nodes.size()); k++) B[k] = C * Wf[nodes[k]].t;
    struct A { M3 m; V3 b, n; };
    std::map<int, A> A_;
    for (auto& [k, name] : kMap) {
        V3 b = B[k], n = N(name);
        M3 m;
        if (auto it = kChild.find(k); it != kChild.end()) {
            V3 db = B[it->second] - b, dn = N(kMap.at(it->second)) - n;
            double lb = Len(db), ln = Len(dn);
            if (lb > 1e-6 && ln > 1e-6) {
                M3 R = kSpine.count(k) ? M3{} : RotBetween(db, dn);
                V3 u = db * (1 / lb);
                double st = std::clamp(ln / lb, 0.6, 1.6);
                m = R * Add(M3{}, Outer(u, u), st - 1);
            }
        }
        A_[k] = {m, b, n};
    }
    for (auto& [k, name] : kMap) {
        if (kChild.count(k)) continue;
        int p = parent[k];
        while (p >= 0 && !kChild.count(p)) p = parent[p];
        if (p >= 0) A_[k].m = A_[p].m;
    }
    // fingers move with the hand (Nick's finger joints do not lie along his fingers)
    for (int k = 22; k <= 30; k++) A_[k] = A_[21];
    for (int k = 55; k <= 63; k++) A_[k] = A_[54];
    auto mapped = [&](int k) {
        while (k >= 0 && !kMap.count(k)) k = parent[k];
        return k >= 0 ? k : 3;
    };

    // textures -> one 512 sheet, each 2x, largest first
    std::vector<std::string> order = M.tex_order;
    std::stable_sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
        return M.tex.at(a).w * M.tex.at(a).h > M.tex.at(b).w * M.tex.at(b).h;
    });
    Image sheet{kAtlas, kAtlas, std::vector<std::array<uint8_t, 3>>(size_t(kAtlas) * kAtlas, {0, 0, 0})};
    std::map<std::string, std::array<int, 4>> place;
    int x = 0, y = 0, rowh = 0;
    for (auto& t : order) {
        Image img = Resize2x(M.tex.at(t));
        if (x + img.w > kAtlas) x = 0, y += rowh, rowh = 0;
        if (y + img.h > kAtlas) Fail("his textures do not fit one sheet");
        for (int yy = 0; yy < img.h; yy++)
            for (int xx = 0; xx < img.w; xx++) sheet.px[size_t(y + yy) * kAtlas + x + xx] = img.px[size_t(yy) * img.w + xx];
        place[t] = {x, y, img.w, img.h};
        x += img.w;
        rowh = std::max(rowh, img.h);
    }

    // his vertices, posed on Nick
    std::vector<V3> verts, vnorm;
    std::vector<Weights> weights;
    struct Tri { std::array<Corner, 3> c; };
    std::vector<Tri> tris;
    for (auto [frame, geo] : M.atomics) {
        if (geo >= M.geoms.size() || frame >= uint32_t(nf)) continue;
        const Geometry& g = M.geoms[geo];
        if (!g.skin || g.nv < 4) continue;
        const Xf& Wg = Wf[frame];
        int base = int(verts.size());
        for (int v = 0; v < g.nv; v++) {
            V3 x0 = C * (Wg * g.pos[v]);
            V3 n0 = Flip * (Wg.r * g.nrm[v]);
            V3 acc, nacc;
            Weights wn;
            for (int j = 0; j < 4; j++) {
                double w = g.bw[v][j];
                if (w <= 0) continue;
                int k = g.bi[v][j];
                if (k >= int(nodes.size())) Fail("a vertex on a bone that is not there");
                int m = mapped(k);
                const A& a = A_.at(m);
                acc = acc + (a.n + a.m * (x0 - a.b)) * w;
                nacc = nacc + (a.m.Inv().T() * n0) * w;
                AddW(wn, Bone(kMap.at(m)), w);
            }
            double nl = Len(nacc);
            verts.push_back(acc);
            vnorm.push_back(nl > 0 ? nacc * (1 / nl) : nacc);
            weights.push_back(wn);
        }
        for (size_t t = 0; t < g.tri.size(); t++) {
            int mi = g.mat[t];
            if (mi < 0 || mi >= int(g.mats.size()) || !place.count(g.mats[mi])) Fail("a triangle without its texture");
            auto [px, py, pw, ph] = place[g.mats[mi]];
            Tri tr;
            for (int c = 0; c < 3; c++) {
                int v = g.tri[t][c];
                if (v >= g.nv) Fail("a triangle past the vertices");
                double u = std::clamp(double(g.uv[v][0]), 0.0, 1.0), vv = std::clamp(double(g.uv[v][1]), 0.0, 1.0);
                double ax = (px + u * pw) / kAtlas, ay = (py + vv * ph) / kAtlas;
                tr.c[c] = {base + v, vnorm[base + v], ax, 1 - ay};
            }
            tris.push_back(tr);
        }
    }

    // Nick's skinned body and his hand vertices' weights
    Section s0 = Sec(d, 0);
    std::vector<uint32_t> offs(s0.count + 1);
    for (int i = 0; i <= s0.count; i++) offs[i] = Get<uint32_t>(d, s0.off + 4 * i);
    if (s0.count <= kBodyNode) Fail("NICK.XBX has no body mesh");
    size_t node = s0.off + offs[kBodyNode], node_end = s0.off + offs[kBodyNode + 1];
    {
        uint32_t nb = Get<uint32_t>(d, node) >> 24, hs = Get<uint32_t>(d, node + 8), n = 0;
        for (uint32_t b = 0; b < nb; b++) n += Get<uint16_t>(d, node + 0x40 + 24 * b + 6);
        size_t sv = node + hs, sw = sv + 20 * size_t(n);
        std::vector<V3> hv;
        std::vector<std::pair<Weights, bool>> hw;
        std::set<std::vector<uint8_t>> seen;
        for (uint32_t k = 0; k < n; k++) {
            Weights w;
            bool hand = false, left = false;
            for (int j = 0; j < 4; j++) {
                int16_t r = Get<int16_t>(d, sw + 12 * k + 2 * j);
                uint8_t x8 = d.at(sw + 12 * k + 8 + j);
                if (!x8) continue;
                int b = PaletteBone((r + 96) / 3);
                AddW(w, b, x8 / 255.0);
                hand |= HandBone(b);
                left |= LeftBone(b);
            }
            if (!hand) continue;
            std::vector<uint8_t> key(d.begin() + long(sv + 20 * k), d.begin() + long(sv + 20 * k + 12));
            key.insert(key.end(), d.begin() + long(sw + 12 * k), d.begin() + long(sw + 12 * k + 12));
            if (!seen.insert(key).second) continue;
            V3 p{Get<float>(d, sv + 20 * k), Get<float>(d, sv + 20 * k + 4), Get<float>(d, sv + 20 * k + 8)};
            hv.push_back(NW[kBodyBone] * p);
            hw.push_back({w, left});
        }
        for (size_t i = 0; i < weights.size(); i++) {
            bool hand = false;
            for (auto& [b, w] : weights[i]) hand |= HandBone(b);
            if (!hand) continue;
            bool left = verts[i].x > 0;
            std::vector<std::pair<double, size_t>> dist;
            for (size_t j = 0; j < hv.size(); j++)
                if (hw[j].second == left) dist.push_back({Len(hv[j] - verts[i]), j});
            std::stable_sort(dist.begin(), dist.end(), [](auto& a, auto& b) { return a.first < b.first; });
            Weights acc;
            for (size_t q = 0; q < std::min<size_t>(3, dist.size()); q++) {
                double f = 1.0 / std::max(dist[q].first, 1e-4);
                for (auto& [b, w] : hw[dist[q].second].first) AddW(acc, b, f * w);
            }
            double tsum = 0;
            for (auto& p : acc) tsum += p.second;
            if (tsum > 0) {
                for (auto& p : acc) p.second /= tsum;
                weights[i] = acc;
            }
        }
    }

    // into Nick's body space, weights onto the palette
    Xf body = NW[kBodyBone];
    M3 binv = body.r.Inv();
    std::vector<V3> Pb(verts.size());
    for (size_t i = 0; i < verts.size(); i++) Pb[i] = binv * (verts[i] - body.t);
    std::vector<std::array<int16_t, 4>> WI(verts.size());
    std::vector<std::array<uint8_t, 4>> WW(verts.size());
    for (size_t i = 0; i < verts.size(); i++) {
        Weights acc;
        for (auto& [b, w] : weights[i]) AddW(acc, InPalette(b) ? PaletteIndex(b) : 0, w);
        std::stable_sort(acc.begin(), acc.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (acc.size() > 4) acc.resize(4);
        if (acc.empty()) acc.push_back({0, 1.0});
        double sum = 0;
        for (auto& p : acc) sum += p.second;
        std::array<int, 4> q{0, 0, 0, 0};
        int qs = 0;
        for (size_t j = 0; j < acc.size(); j++) qs += q[j] = int(RoundEven(255 * acc[j].second / sum));
        q[0] += 255 - qs;
        for (int j = 0; j < 4; j++) {
            WI[i][j] = int16_t(-96 + 3 * (j < int(acc.size()) ? acc[j].first : 0));
            WW[i][j] = uint8_t(j < int(acc.size()) ? q[j] : 0);
        }
    }
    // corners -> strip vertices; triangles faced by their normals
    std::map<std::tuple<int, long long, long long, long long, long long, long long>, int> keys;
    struct CV { int v; V3 n; double u, vv; };
    std::vector<CV> corners;
    std::vector<std::array<int, 3>> st;
    M3 Nmat = binv;
    for (auto& t : tris) {
        std::array<int, 3> cc;
        for (int c = 0; c < 3; c++) {
            const Corner& k = t.c[c];
            V3 nn = Nmat * k.n;
            double u = k.u, v = 1 - k.vv;
            auto r5 = [](double a) { return (long long)std::llround(a * 1e5); };
            auto r3 = [](double a) { return (long long)std::llround(a * 1e3); };
            auto key = std::make_tuple(k.v, r5(u), r5(v), r3(nn.x), r3(nn.y), r3(nn.z));
            auto it = keys.find(key);
            if (it == keys.end()) {
                it = keys.emplace(key, int(corners.size())).first;
                corners.push_back({k.v, nn, u, v});
            }
            cc[c] = it->second;
        }
        int a = cc[0], b = cc[1], c = cc[2];
        V3 pa = Pb[corners[a].v], pb = Pb[corners[b].v], pc = Pb[corners[c].v];
        V3 nsum = corners[a].n + corners[b].n + corners[c].n;
        if (Dot(Cross(pb - pa, pc - pa), nsum) < 0) std::swap(b, c);
        if (a != b && b != c && a != c) st.push_back({a, c, b});
    }
    std::vector<int> order2 = Stripify(st);
    if (order2.size() > 65535) Fail("too many strip vertices");
    uint32_t n = uint32_t(order2.size());
    Bytes sv, sw;
    V3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
    for (int k : order2) {
        const CV& c = corners[k];
        V3 p = Pb[c.v];
        Put(sv, float(p.x)); Put(sv, float(p.y)); Put(sv, float(p.z));
        Put(sv, PackNormal(c.n));
        Put(sv, int16_t(RoundEven(std::clamp(c.u, 0.0, 1.0) * 32767)));
        Put(sv, int16_t(RoundEven(std::clamp(c.vv, 0.0, 1.0) * 32767)));
        for (int j = 0; j < 4; j++) Put(sw, WI[c.v][j]);
        for (int j = 0; j < 4; j++) sw.push_back(WW[c.v][j]);
        for (int a = 0; a < 3; a++) lo[a] = std::min(lo[a], p[a]), hi[a] = std::max(hi[a], p[a]);
    }
    // the body node: Nick's header, one batch on the texture slot, the new vertices
    Bytes hdr(d.begin() + long(node), d.begin() + long(node + 0x40));
    uint32_t flags = Get<uint32_t>(hdr, 0);
    Set<uint32_t>(hdr, 0, (1u << 24) | (flags & 0xFFFFFF));
    Set<uint32_t>(hdr, 8, 0x60);
    Set<uint32_t>(hdr, 0x14, 0x60);
    V3 ctr = (lo + hi) * 0.5;
    double rad = 0;
    for (int k : order2) rad = std::max(rad, Len(Pb[corners[k].v] - ctr));
    float bounds[7] = {float(ctr.x), float(ctr.y), float(ctr.z), float(rad), float((hi.x - lo.x) / 2), float((hi.y - lo.y) / 2), float((hi.z - lo.z) / 2)};
    std::memcpy(&hdr[0x20], bounds, sizeof(bounds));
    Bytes rec(d.begin() + long(node + 0x40 + 2 * 24), d.begin() + long(node + 0x40 + 3 * 24));
    Set<uint16_t>(rec, 6, uint16_t(n));
    Set<uint16_t>(rec, 12, uint16_t(kTexSlot));
    Set<uint16_t>(rec, 14, 0);
    Bytes bodyb = hdr;
    bodyb.insert(bodyb.end(), rec.begin(), rec.end());
    bodyb.resize(0x60, 0x55);
    bodyb.insert(bodyb.end(), sv.begin(), sv.end());
    bodyb.insert(bodyb.end(), sw.begin(), sw.end());
    bodyb.resize((bodyb.size() + 15) / 16 * 16, 0x55);
    size_t old_len = node_end - node;
    long long diff = (long long)bodyb.size() - (long long)old_len;
    long long pad = ((-diff) % 128 + 128) % 128;
    bodyb.resize(bodyb.size() + size_t(pad), 0x55);
    long long delta = (long long)bodyb.size() - (long long)old_len;
    // Nick's eyes (nodes 0 and 1, rigid): emptied
    for (int k : {0, 1}) {
        size_t e = s0.off + offs[k];
        uint16_t cnt = Get<uint16_t>(d, e + 0x40 + 6);
        size_t vo = e + Get<uint32_t>(d, e + 8);
        for (uint16_t j = 0; j < cnt; j++)
            for (int b = 0; b < 6; b++) Set<uint8_t>(d, vo + 14 * j + b, 0);
    }
    Bytes out(d.begin(), d.begin() + long(node));
    out.insert(out.end(), bodyb.begin(), bodyb.end());
    out.insert(out.end(), d.begin() + long(node_end), d.end());
    for (int j = kBodyNode + 1; j <= s0.count; j++) Set<uint32_t>(out, s0.off + 4 * j, uint32_t((long long)offs[j] + delta));
    auto secs = Sections(d);
    for (int i = 0; i < 8; i++)
        if (secs[i].off > node) Set<uint32_t>(out, 0x10 + 8 * i, uint32_t((long long)secs[i].off + delta));
    // the head texture slot: the sheet, 512x512 DXT1 with the slot's mip levels
    Section t2 = Sec(out, 2);
    size_t th = t2.off + 20 * kTexSlot;
    uint32_t data = Get<uint32_t>(out, th + 4), fmt = Get<uint32_t>(out, th + 12);
    uint32_t levels = (fmt >> 16) & 15;
    uint32_t want = (fmt & 0xFF) | (0x0C << 8) | (levels << 16) | (9 << 20) | (9 << 24) | (fmt & 0xF0000000);
    Bytes tb;
    Image lvl = sheet;
    for (uint32_t l = 0; l < levels; l++) {
        Bytes b = Dxt1(lvl);
        tb.insert(tb.end(), b.begin(), b.end());
        lvl = Half(lvl);
    }
    uint32_t next = kTexSlot + 1 < t2.count ? Get<uint32_t>(out, t2.off + 20 * (kTexSlot + 1) + 4) : uint32_t(out.size() - t2.off);
    if (fmt != want || tb.size() > next - data) Fail("NICK.XBX's head texture is not the 512x512 slot this expects");
    std::memcpy(&out[t2.off + data], tb.data(), tb.size());
    std::printf("  Makoto: %zu vertices, %zu triangles, %u strip vertices\n", verts.size(), tris.size(), n);
    return out;
}

// ---------------------------------------------------------------- finding the files
constexpr uint64_t kRmdHash = 0x357fcc88916ded85ull;  // BTL.CVM\MODEL\FIELD\FC_01_00.RMD on SLUS-21621
constexpr size_t kRmdSize = 365023;

bool IsP3FesUsa(const fs::path& iso, std::string& why) {
    try {
        Reader r(iso);
        Iso disc(r, 0);
        Iso::Ent cnf;
        if (!disc.Path("SYSTEM.CNF", cnf)) return why = "no SYSTEM.CNF", false;
        Bytes b = r.Read(cnf.at, std::min<uint32_t>(cnf.size, 4096));
        std::string text(b.begin(), b.end());
        if (text.find("SLUS_216.21") == std::string::npos) return why = "not the USA Persona 3 FES disc (SLUS-21621)", false;
        return true;
    } catch (const std::exception& e) {
        why = e.what();
        return false;
    }
}

Bytes MakotoFromDisc(const fs::path& iso) {
    Reader r(iso);
    Iso disc(r, 0);
    Iso::Ent cvm;
    if (!disc.Path("BTL.CVM", cvm)) Fail("no BTL.CVM on the disc");
    // a CVM is an ISO 9660 image behind a header: find its volume descriptor
    uint64_t inner = 0;
    bool found = false;
    for (uint64_t o = 0; o <= 0x40000 && !found; o += 0x800) {
        if (cvm.at + o + 0x8000 + 6 > r.size) break;
        Bytes v = r.Read(cvm.at + o + 0x8000, 6);
        if (v[0] == 1 && std::memcmp(&v[1], "CD001", 5) == 0) inner = cvm.at + o, found = true;
    }
    if (!found) Fail("BTL.CVM is not readable (no image inside)");
    Iso btl(r, inner);
    Iso::Ent f;
    if (!btl.Path("MODEL/FIELD/FC_01_00.RMD", f)) Fail("no MODEL\\FIELD\\FC_01_00.RMD in BTL.CVM");
    Bytes rmd = r.Read(f.at, f.size);
    if (rmd.size() != kRmdSize || Fnv(rmd) != kRmdHash) Fail("FC_01_00.RMD is not the one on the USA disc (a modded or other disc?)");
    return rmd;
}

Bytes NickFrom(const fs::path& assets) {
    std::error_code ec;
    fs::path chars = assets / "Data" / "Char";
    if (fs::is_directory(chars, ec))
        for (auto& e : fs::directory_iterator(chars, ec))
            if (Upper(e.path().filename().string()) == "NICK.XBX") return ReadFile(e.path());
    for (auto& e : fs::directory_iterator(assets, ec)) {
        if (Upper(e.path().extension().string()) != ".ISO") continue;
        Bytes b;
        try {
            if (XboxFile(e.path(), "Data/Char/NICK.XBX", b)) return b;
        } catch (const std::exception&) {
        }
    }
    Fail("True Crime's Data\\Char\\NICK.XBX was not found: put the game (its files or its Xbox .iso) in " + assets.string());
}

void Write(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    if (!f) Fail("cannot write " + p.string());
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("\n  True Crime: Streets of LA - Makoto Yuki (M3M3NT0) from Persona 3 FES\n");
    std::printf("  ====================================================================\n\n");
    try {
        if (argc >= 5 && std::string(argv[1]) == "--rmd") {
            Write(argv[4], Convert(ReadFile(argv[2]), ReadFile(argv[3])));
            std::printf("  wrote %s\n", argv[4]);
            return 0;
        }
        if (argc < 2) {
            std::printf("  usage: import_p3_makoto <assets folder> [Persona 3 FES .iso]\n");
            return 2;
        }
        fs::path assets = argv[1];
        fs::path iso;
        std::string why;
        if (argc >= 3 && argv[2][0]) {
            iso = argv[2];
            if (!IsP3FesUsa(iso, why)) Fail(iso.filename().string() + ": " + why);
        } else {
            std::error_code ec;
            std::vector<fs::path> isos;
            for (auto& e : fs::directory_iterator(assets, ec))
                if (Upper(e.path().extension().string()) == ".ISO") isos.push_back(e.path());
            std::sort(isos.begin(), isos.end());
            for (auto& p : isos)
                if (IsP3FesUsa(p, why)) {
                    iso = p;
                    break;
                }
            if (iso.empty())
                Fail("no Persona 3 FES (USA) .iso in " + assets.string() +
                     "\n  Put your own copy there, or drag it onto makoto_skin.bat. Other versions are not supported yet.");
        }
        std::printf("  Persona 3 FES: %s\n", iso.filename().string().c_str());
        Bytes rmd = MakotoFromDisc(iso);
        Bytes nick = NickFrom(assets);
        Bytes out = Convert(rmd, nick);
        fs::path dst = assets / "extras" / "Data" / "Char" / "M3M3NT0.XBX";
        Write(dst, out);
        std::printf("\n  Done: %s\n", dst.string().c_str());
        std::printf("  Enter M3M3NT0 as the license plate (hold L and R on OK, as the console's codes).\n");
        std::printf("  The Persona 3 FES .iso is not needed any more and can be moved out of assets.\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("  %s\n", e.what());
        return 1;
    }
}
