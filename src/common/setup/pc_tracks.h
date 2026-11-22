// [TCSOLA] The PC version's soundtrack list: its songs' lines from the PC's track list (Data\\Shell\\TCPC??.txt),
// "084Them Bones|Alice In Chains", songs 084 to 127, each once. The bytes stay as the file has them (8-bit text).
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pctracks {

struct Track {
    int song;
    std::string line;
};

inline std::filesystem::path TrackList(const std::filesystem::path& pc) {
    namespace fs = std::filesystem;
    fs::path shell = pc / "Data" / "Shell";
    std::error_code ec;
    for (const char* n : {"TCPCUK.txt", "TCPCUS.txt"}) {
        if (fs::is_regular_file(shell / n, ec)) return shell / n;
        for (auto& e : fs::directory_iterator(shell, ec)) {
            std::string a = e.path().filename().string(), b = n;
            std::transform(a.begin(), a.end(), a.begin(), ::toupper);
            std::transform(b.begin(), b.end(), b.begin(), ::toupper);
            if (a == b) return e.path();
        }
    }
    for (auto& e : fs::directory_iterator(shell, ec)) {
        std::string a = e.path().filename().string();
        std::transform(a.begin(), a.end(), a.begin(), ::toupper);
        if (a.rfind("TCPC", 0) == 0 && a.size() > 4 && a.substr(a.size() - 4) == ".TXT") return e.path();
    }
    return {};
}

inline std::vector<Track> Read(const std::filesystem::path& list) {
    std::ifstream f(list, std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(f)), {});
    std::vector<Track> out;
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 6 || !isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1]) || !isdigit((unsigned char)line[2])) continue;
        size_t bar = line.find('|', 3);
        if (bar == std::string::npos || bar == 3 || bar + 1 >= line.size()) continue;
        int n = std::stoi(line.substr(0, 3));
        if (n < 84 || n > 127) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Track& t) { return t.song == n; })) continue;
        out.push_back({n, line});
    }
    std::sort(out.begin(), out.end(), [](const Track& a, const Track& b) { return a.song < b.song; });
    return out;
}

}  // namespace pctracks
