// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments
// The dev harness: Open303 (the float original, from its own clone: build.sh's OPEN303, never copied here) against ACID's integer
// port (acid_devhost.c), the same notes into both, for each preset and the 16-step line at a few settings. Says per case the level
// of each (RMS, dB), their difference, and how far their spectra are apart (third-octave bands 40 Hz .. 16 kHz, those within 60 dB of
// the loudest: the mean and the largest difference, dB). Writes both renders as WAVs into OUTDIR (argv[1]) when given.
//   build.sh && ./build/acid_compare [OUTDIR]
#include "rosic_Open303.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" {
#include "pattern.h"
void acid_dev_init(const int *vals);
void acid_dev_note(int note, int vel, int on);
void acid_dev_render(float *out, int n);
const char *acid_dev_preset(int i, int *vals);
}

enum { A_WAVE, A_CUT, A_RES, A_ENV, A_DCY, A_ACC, A_TUNE, A_SLID, A_DRV, A_N };
static const int SR = 44100;

static double kf_time(int v) { return pow(10000.0, v / 127.0); }
static void set303(rosic::Open303 &o, const int *v)
{
    o.setSampleRate(SR);
    o.setTanhShaperDrive(36.9);   // the VST's defaults for the square (Open303VST.cpp)
    o.setTanhShaperOffset(4.37);
    o.setSquarePhaseShift(189.0);
    o.setAmpSustain(-60.0);
    o.setVolume(0.0);
    o.setWaveform(v[A_WAVE] ? 1.0 : 0.0);
    o.setCutoff(314.0 * pow(2394.0 / 314.0, v[A_CUT] / 127.0));
    o.setResonance(v[A_RES] * 100.0 / 127.0);
    o.setEnvMod(v[A_ENV] * 100.0 / 127.0);
    o.setDecay(kf_time(v[A_DCY]));
    o.setAccent(v[A_ACC] * 100.0 / 127.0);
    o.setTuning(440.0 * pow(2.0, v[A_TUNE] / 1200.0));
    o.setSlideTime(kf_time(v[A_SLID]));
}

static void wav(const char *path, const std::vector<float> &x)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const uint32_t n = (uint32_t)x.size(), bytes = n * 2;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(1); u32(SR); u32(SR * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(bytes);
    for (float s : x) { long q = lrintf(s * 0.5f * 32767.f); q = q > 32767 ? 32767 : q < -32768 ? -32768 : q; u16((uint16_t)(int16_t)q); }
    fclose(f);
}

static void fft(std::vector<double> &re, std::vector<double> &im)
{
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double a = -2 * M_PI / len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; k++) {
                const double c = cos(a * k), s = sin(a * k);
                const double xr = re[i + k + len / 2] * c - im[i + k + len / 2] * s, xi = re[i + k + len / 2] * s + im[i + k + len / 2] * c;
                re[i + k + len / 2] = re[i + k] - xr; im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr; im[i + k] += xi;
            }
    }
}
// the power in third-octave bands (Welch, 8192-point Hann frames, half overlapped)
static std::vector<double> bands(const std::vector<float> &x, std::vector<double> &fc)
{
    const size_t N = 8192;
    std::vector<double> pw(N / 2, 0.0);
    for (size_t at = 0; at + N <= x.size(); at += N / 2) {
        std::vector<double> re(N), im(N, 0.0);
        for (size_t i = 0; i < N; i++) re[i] = x[at + i] * (0.5 - 0.5 * cos(2 * M_PI * i / N));
        fft(re, im);
        for (size_t k = 0; k < N / 2; k++) pw[k] += re[k] * re[k] + im[k] * im[k];
    }
    std::vector<double> b;
    fc.clear();
    for (double f = 40; f <= 16000; f *= pow(2.0, 1 / 3.0)) {
        const double lo = f / pow(2.0, 1 / 6.0), hi = f * pow(2.0, 1 / 6.0);
        double s = 1e-30;
        for (size_t k = (size_t)(lo * N / SR); k <= (size_t)(hi * N / SR) && k < N / 2; k++) s += pw[k];
        b.push_back(s);
        fc.push_back(f);
    }
    return b;
}
static double rms_db(const std::vector<float> &x)
{
    double s = 0;
    for (float v : x) s += (double)v * v;
    return 10 * log10(s / x.size() + 1e-30);
}

struct Case { const char *name; int v[A_N]; };

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : 0;
    std::vector<Case> cases;
    const int D[A_N] = {0, 40, 85, 50, 83, 80, 0, 56, 0};   // the defaults (acid.c's descriptor)
    auto add = [&](const char *n, int cut, int res, int env, int wave) {
        Case c; c.name = n; memcpy(c.v, D, sizeof D); c.v[A_CUT] = cut; c.v[A_RES] = res; c.v[A_ENV] = env; c.v[A_WAVE] = wave;
        cases.push_back(c);
    };
    add("line_defaults", 40, 85, 50, 0);
    add("line_dark_lowres", 15, 30, 20, 0);
    add("line_mid", 55, 100, 70, 0);
    add("line_open_highres", 90, 120, 90, 0);
    add("line_square_mid", 55, 100, 70, 1);
    add("line_max", 127, 127, 127, 0);
    static char names[16][48];
    for (int i = 0; i < 16; i++) {
        Case c;
        const char *pn = acid_dev_preset(i, c.v);
        if (!pn) break;
        snprintf(names[i], sizeof names[i], "preset_%s", pn);
        for (char *q = names[i]; *q; q++) if (*q == ' ') *q = '_';
        c.name = names[i];
        c.v[A_DRV] = 0;   // (DRV is Fune's addition: out of the comparison)
        cases.push_back(c);
    }
    AcidEvent ev[256];
    const int nev = acid_line_events(ev, 256, 2, 127, 100);
    const int len = 2 * 16 * ACID_STEP_SAMPLES + SR / 2;
    printf("| case | Open303 dB | Acid dB | level diff dB | spectrum mean diff dB | spectrum max diff dB (at Hz) |\n");
    printf("| --- | --- | --- | --- | --- | --- |\n");
    double worst_level = 0, sum_mean = 0;
    for (const Case &c : cases) {
        std::vector<float> a(len), b(len);
        // the float original: Open303's velocity: an accent at 100 and above
        rosic::Open303 *op = new rosic::Open303();
        rosic::Open303 &o = *op;
        set303(o, c.v);
        acid_dev_init(c.v);
        int e = 0;
        for (int i = 0; i < len; i += 32) {
            for (; e < nev && ev[e].at <= i; e++) {
                o.noteOn(ev[e].note, ev[e].on ? (ev[e].vel > 110 ? 127 : 64) : 0, 0.0);
                acid_dev_note(ev[e].note, ev[e].vel, ev[e].on);
            }
            for (int k = 0; k < 32 && i + k < len; k++) a[i + k] = (float)o.getSample();
            acid_dev_render(&b[i], len - i < 32 ? len - i : 32);
        }
        delete op;
        std::vector<double> fc;
        const std::vector<double> ba = bands(a, fc), bb = bands(b, fc);
        double mean = 0, mx = 0, mxf = 0, top = 0;
        int nb = 0;
        for (double v : ba) top = v > top ? v : top;
        for (size_t k = 0; k < ba.size(); k++) {
            if (ba[k] < top * 1e-6) continue;   // (a band 60 dB under the loudest: below what is heard against it)
            const double d = fabs(10 * log10(bb[k] / ba[k]));
            mean += d; nb++;
            if (d > mx) { mx = d; mxf = fc[k]; }
        }
        mean /= nb ? nb : 1;
        const double la = rms_db(a), lb = rms_db(b);
        printf("| %s | %.1f | %.1f | %+.2f | %.2f | %.1f (%.0f) |\n", c.name, la, lb, lb - la, mean, mx, mxf);
        if (fabs(lb - la) > fabs(worst_level)) worst_level = lb - la;
        sum_mean += mean;
        if (outdir) {
            char p[512];
            snprintf(p, sizeof p, "%s/%s_open303.wav", outdir, c.name); wav(p, a);
            snprintf(p, sizeof p, "%s/%s_acid.wav", outdir, c.name); wav(p, b);
        }
    }
    printf("\nworst level difference %+.2f dB; spectrum mean difference over the cases %.2f dB\n", worst_level, sum_mean / cases.size());
    return 0;
}
