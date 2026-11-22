// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
// A dev tool (not part of the plugin): renders the same notes through Mutable Instruments Plaits' float engines (MIT,
// https://github.com/pichenettes/eurorack, built at 44.1 kHz by build_compare.sh) and through Macro's integer models
// (macro_int.c), and says how far apart they are, per model and setting:
//   snr   the float output against the difference, dB (time domain, at the best of +-48 samples' lag: Plaits ramps its
//         frequency from rest in its first block; the pitch is the same; a noise model's has no meaning)
//   band  the spectra's difference in sixth-octave bands, dB rms over the bands within 60 dB of the float's strongest
//   lvl   the integer output's rms against the float's, dB
// Usage: plaits_compare [out.tsv]
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "stmlib/utils/buffer_allocator.h"
#include "plaits/dsp/engine/additive_engine.h"
#include "plaits/dsp/engine/chord_engine.h"
#include "plaits/dsp/engine/fm_engine.h"
#include "plaits/dsp/engine/grain_engine.h"
#include "plaits/dsp/engine/noise_engine.h"
#include "plaits/dsp/engine/virtual_analog_engine.h"
#include "plaits/dsp/engine/waveshaping_engine.h"
#include "plaits/dsp/engine/wavetable_engine.h"
#include "plaits/dsp/envelope.h"
#include "plaits/dsp/fx/low_pass_gate.h"

extern "C" void mh_voice(const int16_t *val, int note, int vel, int held, float *out, int n);
extern "C" void mh_model(int model, int harm, int timb, int morf, double hz, float *out, int n);

static const int SR = 44100, N = SR * 3 / 2, SKIP = SR / 2, NFFT = 16384;

static void fft(std::vector<std::complex<double>> &a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / len;
        const std::complex<double> wl(cos(ang), sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; j++) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
static std::vector<double> spectrum(const float *x)
{
    std::vector<std::complex<double>> a(NFFT);
    for (int i = 0; i < NFFT; i++) a[i] = x[i] * (0.5 - 0.5 * cos(2 * M_PI * i / (NFFT - 1)));
    fft(a);
    std::vector<double> m(NFFT / 2);
    for (int i = 0; i < NFFT / 2; i++) m[i] = 20 * log10(std::abs(a[i]) + 1e-12);
    return m;
}

static plaits::Engine *make(int model, stmlib::BufferAllocator *al)
{
    plaits::Engine *e = nullptr;
    switch (model) {
    case 0: e = new plaits::VirtualAnalogEngine; break;
    case 1: e = new plaits::WaveshapingEngine; break;
    case 2: e = new plaits::FMEngine; break;
    case 3: e = new plaits::GrainEngine; break;
    case 4: e = new plaits::AdditiveEngine; break;
    case 5: e = new plaits::WavetableEngine; break;
    case 6: e = new plaits::ChordEngine; break;
    default: e = new plaits::NoiseEngine; break;
    }
    e->Init(al);
    e->LoadUserData(nullptr);
    e->Reset();
    return e;
}

static void plaits_render(int model, int h, int t, int m, double hz, float *out)
{
    static char mem[65536 * 4];
    stmlib::BufferAllocator al(mem, sizeof mem);
    plaits::Engine *e = make(model, &al);
    plaits::EngineParameters p;
    float o[12], aux[12];
    p.note = 69.0f + 12.0f * (float)log2(hz / 440.0);
    p.harmonics = h * 258 / 32768.0f; p.timbre = t * 258 / 32768.0f; p.morph = m * 258 / 32768.0f;
    if (model == 6) p.morph /= 2.15f;   // (Chord: Macro's MORF spans the registrations Plaits' spans to 0.465)
    p.accent = 0.8f;
    for (int i = 0; i < N; i += 12) {
        bool env = false;
        p.trigger = i == 0 ? (plaits::TRIGGER_RISING_EDGE | plaits::TRIGGER_HIGH) : plaits::TRIGGER_HIGH;
        e->Render(p, o, aux, 12, &env);
        for (int k = 0; k < 12 && i + k < N; k++) out[i + k] = o[k];
    }
    (void)e;   // (Plaits' engines have no destructor: an embedded program never frees them)
}

// The gate: a 220 Hz sine through Plaits' LPGEnvelope (ProcessPing) and LowPassGate at its own 48 kHz, blocks of 12, and a
// Macro voice (FM at TIMB 0: a sine) at 44.1 kHz, velocity 127: their levels every 10 ms (dB from each one's top), compared
// over the first 3 s where the float is above -60 dB.
static void gate_compare(void)
{
    static const int D[4] = {20, 50, 80, 110}, C[3] = {0, 64, 127};
    printf("\ngate (DCAY, COLR): the largest and the rms difference of the level every 10 ms, dB, while above -60 dB\n");
    for (int di = 0; di < 4; di++)
        for (int ci = 0; ci < 3; ci++) {
            const double decay = D[di] * 258 / 32768.0, hf = C[ci] * 258 / 32768.0;
            const int n48 = 48000 * 3, n44 = 44100 * 3;
            std::vector<float> x(n48), y(n44);
            plaits::LPGEnvelope env; plaits::LowPassGate lpg;
            env.Init(); lpg.Init(); env.Trigger();
            const float short_decay = (200.0f * 12) / 48000.0f * powf(2.0f, -96.0f * decay / 12.0f);
            const float tail = (20.0f * 12) / 48000.0f * powf(2.0f, (-72.0f * decay + 12.0f * hf) / 12.0f) - short_decay;
            const float attack = 220.0f / 48000.0f * 12 * 2;
            for (int i = 0; i < n48; i += 12) {
                float b[12];
                for (int k = 0; k < 12; k++) b[k] = sinf(2 * M_PI * 220.0 * (i + k) / 48000.0);
                env.ProcessPing(attack, short_decay, tail, hf);
                lpg.Process(env.gain(), env.frequency(), env.hf_bleed(), b, 12);
                for (int k = 0; k < 12; k++) x[i + k] = b[k];
            }
            const int16_t val[8] = {2, 64, 0, 64, (int16_t)D[di], (int16_t)C[ci], 0, 127};
            mh_voice(val, 57, 127, n44, y.data(), n44);
            std::vector<double> la, lb;
            for (int t = 0; t < 300; t++) {
                double ea = 0, eb = 0;
                for (int k = 0; k < 480; k++) ea += x[t * 480 + k] * x[t * 480 + k];
                for (int k = 0; k < 441; k++) eb += y[t * 441 + k] * y[t * 441 + k];
                la.push_back(10 * log10(ea / 480 + 1e-30)); lb.push_back(10 * log10(eb / 441 + 1e-30));
            }
            double ma = -1e9, mb = -1e9, worst = 0, rms = 0;
            int cnt = 0;
            for (int t = 0; t < 300; t++) { ma = la[t] > ma ? la[t] : ma; mb = lb[t] > mb ? lb[t] : mb; }
            for (int t = 0; t < 300; t++)
                if (la[t] - ma > -60) { const double d = (la[t] - ma) - (lb[t] - mb); worst = fabs(d) > worst ? fabs(d) : worst; rms += d * d; cnt++; }
            if (getenv("GATE_CURVE") && atoi(getenv("GATE_CURVE")) == D[di] * 10 + ci)
                for (int t = 0; t < 300; t += 5) printf("    %3d0 ms  %6.1f  %6.1f\n", t, la[t] - ma, lb[t] - mb);
            printf("  DCAY %3d COLR %3d   worst %5.1f dB   rms %4.1f dB   (%d frames)\n", D[di], C[ci], worst, cnt ? sqrt(rms / cnt) : 0, cnt);
        }
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "gate")) { gate_compare(); return 0; }
    if (argc == 7 && !strcmp(argv[1], "dump")) {   // plaits_compare dump MODEL H T M HZ: the two outputs, a line a sample
        std::vector<float> x(N), y(N);
        plaits_render(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atof(argv[6]), x.data());
        mh_model(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atof(argv[6]), y.data(), N);
        for (int i = 0; i < N; i++) printf("%d %.6f %.6f\n", i, x[i], y[i]);
        return 0;
    }
    static const char *const NAMES[8] = {"Pair", "Fold", "FM", "Grain", "Harm", "Wave", "Chord", "Noise"};
    struct Case { int model, h, t, m; double hz; };
    std::vector<Case> cases;
    static const double HZ[3] = {110.0, 440.0, 1760.0};
    static const int SET[5][3] = {{64, 64, 64}, {20, 30, 100}, {100, 100, 20}, {0, 127, 127}, {127, 0, 0}};
    for (int model = 0; model < 8; model++)
        for (int s = 0; s < 5; s++)
            for (int f = 0; f < 3; f++) cases.push_back({model, SET[s][0], SET[s][1], SET[s][2], HZ[f]});
    FILE *tsv = argc > 1 ? fopen(argv[1], "w") : nullptr;
    if (tsv) fprintf(tsv, "model\tharm\ttimb\tmorf\thz\tsnr_db\tband_db\tlvl_db\n");
    std::vector<float> a(N), b(N);
    double sum_lsd[8] = {0}, sum_snr[8] = {0}, worst_lsd[8] = {0};
    int count[8] = {0};
    for (const Case &c : cases) {
        plaits_render(c.model, c.h, c.t, c.m, c.hz, a.data());
        mh_model(c.model, c.h, c.t, c.m, c.hz, b.data(), N);
        double ea = 0, ed = 1e30, eb = 0;
        for (int i = SKIP; i < N; i++) { ea += a[i] * a[i]; eb += b[i] * b[i]; }
        for (int lag = -48; lag <= 48; lag++) {   // the best of the lags (Plaits' first block ramps its frequency up from rest)
            double e = 0;
            for (int i = SKIP; i < N - 64; i++) { const double d = a[i] - b[i + lag]; e += d * d; }
            if (e < ed) ed = e;
        }
        const double snr = 10 * log10((ea + 1e-20) / (ed + 1e-20)), lvl = 10 * log10((eb + 1e-20) / (ea + 1e-20));
        // the spectra in sixth-octave bands, 40 Hz .. 20 kHz: the rms of the bands' differences (dB) over those within 60 dB of
        // the float's strongest band (a noise model's or an inharmonic one's too)
        const auto sa = spectrum(&a[SKIP]), sb = spectrum(&b[SKIP]);
        std::vector<double> ba, bb;
        for (double lo = 40; lo < 20000; lo *= pow(2.0, 1.0 / 6)) {
            const int i0 = (int)(lo / SR * NFFT), i1 = (int)(lo * pow(2.0, 1.0 / 6) / SR * NFFT);
            double xa = 1e-30, xb = 1e-30;
            for (int i = i0; i <= i1 && i < NFFT / 2; i++) { xa += pow(10, sa[i] / 10); xb += pow(10, sb[i] / 10); }
            ba.push_back(10 * log10(xa)); bb.push_back(10 * log10(xb));
        }
        double peak = -1e9, lsd = 0;
        int nb = 0;
        for (double v : ba) peak = v > peak ? v : peak;
        for (size_t i = 0; i < ba.size(); i++)
            if (ba[i] > peak - 60) { const double d = ba[i] - (bb[i] > peak - 80 ? bb[i] : peak - 80); lsd += d * d; nb++; }
        lsd = nb ? sqrt(lsd / nb) : 0;
        printf("%-6s h%3d t%3d m%3d %6.0f Hz   snr %6.1f dB   band %5.1f dB   lvl %+5.1f dB\n", NAMES[c.model], c.h, c.t, c.m, c.hz,
               snr, lsd, lvl);
        if (tsv) fprintf(tsv, "%s\t%d\t%d\t%d\t%.0f\t%.2f\t%.2f\t%.2f\n", NAMES[c.model], c.h, c.t, c.m, c.hz, snr, lsd, lvl);
        sum_lsd[c.model] += lsd; sum_snr[c.model] += snr; count[c.model]++;
        if (lsd > worst_lsd[c.model]) worst_lsd[c.model] = lsd;
    }
    printf("\nmodel   mean snr  mean band  worst band\n");
    for (int k = 0; k < 8; k++)
        printf("%-6s  %6.1f dB  %6.1f dB  %6.1f dB\n", NAMES[k], sum_snr[k] / count[k], sum_lsd[k] / count[k], worst_lsd[k]);
    if (tsv) fclose(tsv);
    return 0;
}
// (debug) dump: plaits_compare dump MODEL H T M HZ -> two columns
