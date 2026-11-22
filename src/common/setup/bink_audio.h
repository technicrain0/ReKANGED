// [TCSOLA] Bink audio (as the True Crime PC version's Data\Music\*.bik carry it): the audio track of a Bink file decoded to
// 16-bit PCM. Written from the format's public description (frames of quantised transform coefficients in critical
// bands, an inverse DCT or real DFT per block, blocks cross-faded over a sixteenth of their length). Checked against
// another decoder on the PC's 084.bik: the same samples, within one step of rounding.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace bink {

struct Audio {
    uint32_t rate = 0;
    int channels = 0;
    std::vector<int16_t> pcm;  // interleaved
};

namespace detail {

class Bits {  // little-endian bit reader
public:
    Bits(const uint8_t* p, size_t n) : p_(p), n_(n * 8) {}
    uint32_t Get(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++, pos_++) {
            if (pos_ >= n_) throw std::runtime_error("a Bink audio packet ends early");
            v |= uint32_t((p_[pos_ >> 3] >> (pos_ & 7)) & 1) << i;
        }
        return v;
    }
    bool Bit() { return Get(1) != 0; }
    void Align32() { pos_ = (pos_ + 31) & ~size_t(31); }
    size_t Left() const { return pos_ >= n_ ? 0 : n_ - pos_; }
    void Skip(size_t n) { pos_ += n; }

private:
    const uint8_t* p_;
    size_t n_, pos_ = 0;
};

inline void Fft(std::vector<std::complex<double>>& a, bool inverse) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    const double pi = 3.14159265358979323846;
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * pi / double(len) * (inverse ? 1 : -1);
        std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; k++) {
                auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

}  // namespace detail

// the whole file in memory -> its first audio track
inline Audio Decode(const std::vector<uint8_t>& f) {
    using detail::Bits;
    auto u32 = [&](size_t o) {
        if (o + 4 > f.size()) throw std::runtime_error("a Bink file cut short");
        uint32_t v;
        std::memcpy(&v, &f[o], 4);
        return v;
    };
    if (f.size() < 44 || f[0] != 'B' || f[1] != 'I' || f[2] != 'K') throw std::runtime_error("not a Bink file");
    bool version_b = f[3] == 'b';
    uint32_t frames = u32(8), tracks = u32(40);
    if (!tracks) throw std::runtime_error("the Bink file has no audio");
    size_t o = 44 + 4 * size_t(tracks);
    uint32_t rate = u32(o) & 0xFFFF, flags = u32(o) >> 16;
    o += 8 * size_t(tracks);
    std::vector<uint32_t> index(frames + 1);
    for (uint32_t i = 0; i <= frames; i++) index[i] = u32(o + 4 * i);

    int channels = (flags & 0x2000) ? 2 : 1;
    bool dct = (flags & 0x1000) != 0;
    if (!dct) throw std::runtime_error("this Bink audio kind (real DFT) is not supported");
    // the real-DFT kind codes the channels interleaved as one at a channel-times higher rate
    uint32_t srate = rate;
    int ch = channels;
    int frame_bits = rate < 22050 ? 9 : rate < 44100 ? 10 : 11;
    if (!dct) {
        srate *= uint32_t(channels);
        ch = 1;
        if (channels == 2) frame_bits += 1;
    }
    int frame_len = 1 << frame_bits, overlap = frame_len / 16;
    uint32_t half = (srate + 1) / 2;
    double root = dct ? frame_len / (std::sqrt(double(frame_len)) * 32768.0) : 2.0 / (std::sqrt(double(frame_len)) * 32768.0);
    double quant_table[96];
    for (int i = 0; i < 96; i++) quant_table[i] = std::exp(i * 0.15289164787221953823) * root;
    static const uint16_t kCritical[25] = {100,  200,  300,  400,  510,  630,  770,  920,  1080, 1270,  1480,  1720, 2000,
                                           2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500};
    int bands_n = 1;
    for (; bands_n < 25; bands_n++)
        if (half <= kCritical[bands_n - 1]) break;
    std::vector<int> bands(bands_n + 1);
    bands[0] = 2;
    for (int i = 1; i < bands_n; i++) bands[i] = int((uint64_t(kCritical[i - 1]) * frame_len / half) & ~1ull);
    bands[bands_n] = frame_len;
    static const int kRle[16] = {2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64};

    std::vector<std::vector<double>> prev(ch, std::vector<double>(overlap));
    std::vector<std::vector<double>> coeffs(ch, std::vector<double>(frame_len));
    std::vector<std::complex<double>> work;
    bool first = true;
    Audio out;
    out.rate = rate;
    out.channels = channels;
    auto get_float = [](Bits& b) {
        int power = int(b.Get(5));
        double f = std::ldexp(double(b.Get(23)), power - 23);
        return b.Bit() ? -f : f;
    };
    const double pi = 3.14159265358979323846;

    for (uint32_t fr = 0; fr < frames; fr++) {
        size_t start = index[fr] & ~1u, end = index[fr + 1] & ~1u;
        if (end <= start || end > f.size()) continue;
        uint32_t size = u32(start);
        if (size < 4 || start + 4 + size > end) continue;
        Bits b(&f[start + 4], size);
        b.Skip(32);  // the packet's decoded size
        while (b.Left() > 32) {
            if (dct) b.Skip(2);
            for (int c = 0; c < ch; c++) {
                auto& co = coeffs[c];
                co[0] = get_float(b) * root;
                co[1] = get_float(b) * root;
                std::vector<double> quant(bands_n);
                for (int i = 0; i < bands_n; i++) quant[i] = quant_table[std::min<uint32_t>(b.Get(8), 95)];
                int k = 0;
                double q = quant[0];
                int i = 2;
                while (i < frame_len) {
                    int j;
                    if (version_b) {
                        j = i + 16;
                    } else if (b.Bit()) {
                        j = i + kRle[b.Get(4)] * 8;
                    } else {
                        j = i + 8;
                    }
                    j = std::min(j, frame_len);
                    int width = int(b.Get(4));
                    if (width == 0) {
                        std::fill(co.begin() + i, co.begin() + j, 0.0);
                        i = j;
                        while (k < bands_n && bands[k] < i) q = quant[k++];
                    } else {
                        while (i < j) {
                            if (k <= bands_n && bands[k] == i) q = quant[k++];
                            uint32_t v = b.Get(width);
                            co[i] = v ? (b.Bit() ? -q * v : q * v) : 0.0;
                            i++;
                        }
                    }
                }
                // inverse transform in place
                if (dct) {
                    // DCT-III: y[k] = x0/2 + sum x[n] cos(pi n (2k+1) / 2N), with the DC term doubled first
                    co[0] /= 0.5;
                    work.assign(size_t(frame_len) * 2, 0);
                    for (int n = 0; n < frame_len; n++)
                        work[n] = std::polar((n == 0 ? 0.5 : 1.0) * co[n], pi * n / (2.0 * frame_len));
                    detail::Fft(work, true);
                    for (int n = 0; n < frame_len; n++) co[n] = work[n].real() * (2.0 / frame_len);
                } else {
                    // inverse real DFT of the packed spectrum: re[0], re[N/2], then re/im pairs
                    int n2 = frame_len;
                    work.assign(size_t(n2), 0);
                    work[0] = co[0];
                    work[n2 / 2] = co[1];
                    for (int m = 1; m < n2 / 2; m++) {
                        work[m] = {co[2 * m], co[2 * m + 1]};
                        work[n2 - m] = std::conj(work[m]);
                    }
                    detail::Fft(work, true);
                    for (int n = 0; n < n2; n++) co[n] = work[n].real() * 0.5;
                }
            }
            // cross-fade with the last block's tail
            for (int c = 0; c < ch; c++) {
                auto& co = coeffs[c];
                if (!first) {
                    int count = overlap * ch;
                    for (int i = 0, j = c; i < overlap; i++, j += ch) co[i] = (prev[c][i] * (count - j) + co[i] * j) / count;
                }
                std::copy(co.begin() + (frame_len - overlap), co.end(), prev[c].begin());
            }
            first = false;
            int n = frame_len - overlap;
            if (dct) {
                for (int i = 0; i < n; i++)
                    for (int c = 0; c < ch; c++)
                        out.pcm.push_back(int16_t(std::clamp(std::lrint(coeffs[c][i] * 32768.0), -32768L, 32767L)));
            } else {
                for (int i = 0; i < n; i++)
                    out.pcm.push_back(int16_t(std::clamp(std::lrint(coeffs[0][i] * 32768.0), -32768L, 32767L)));
            }
            b.Align32();
        }
    }
    return out;
}

}  // namespace bink
