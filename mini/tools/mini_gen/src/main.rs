// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
// MINI's integer path's tables (plugins/mini/mini_int_tables.h), made from CA-72's own laws (ca72::light, ca72::voice; by Idle
// Foundry, GPL-3.0-or-later): the integer voice (mini_int.h) reads its knobs through these, at 44.1 kHz, a block of 32 samples.
//   cargo run --release --manifest-path plugins/mini/tools/mini_gen/Cargo.toml > plugins/mini/mini_int_tables.h

use ca72::freq_dial::Pot;
use ca72::light::{self, laws};
use ca72::modulation::mod_wheel_r;
use ca72::tuning::Range;
use ca72::voice::{
    FILTER_ATTACK, FILTER_DECAY, LOUDNESS_ATTACK, LOUDNESS_DECAY, Waveform, glide_r, sustain_track, time_pot, volume_track,
};

const FS: f64 = 44100.0;
const BLOCK: f64 = 32.0;
const LOG2Q: f64 = 3072.0; // log2 (octaves) in 1/3072
const ST: f64 = 4096.0; // semitones in 1/4096
const VQ: f64 = 65536.0; // volts in Q16
const IQ: f64 = 16e9; // amperes in nA Q4

fn interp(xs: &[f64], ys: &[f64], x: f64) -> f64 {
    if x <= xs[0] {
        return ys[0];
    }
    for i in 1..xs.len() {
        if x <= xs[i] {
            let a = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
            return ys[i - 1] + (ys[i] - ys[i - 1]) * a;
        }
    }
    ys[ys.len() - 1]
}
fn arr<T: std::fmt::Display>(name: &str, ty: &str, v: &[T]) {
    println!("static const {ty} {name}[{}] = {{", v.len());
    for c in v.chunks(12) {
        println!("    {},", c.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(", "));
    }
    println!("}};");
}
fn r(x: f64) -> i64 {
    x.round() as i64
}
fn knob(i: usize) -> f64 {
    i as f64 / 127.0
}
fn per_block(tau: f64) -> f64 {
    1.0 - (-BLOCK / (tau * FS)).exp()
}
/// FREQUENCY's x (-64..63) as MINI's dial: semitones x |x| 7.5 / 4096
fn dial(x: i32) -> f64 {
    f64::from(x * x.abs()) * 7.5 / 4096.0
}

fn main() {
    println!("/* SPDX-License-Identifier: GPL-3.0-or-later");
    println!(" * MINI's integer path's tables, made by plugins/mini/light/src/bin/mini_gen.rs from CA-72's laws (copyright (C) 2026");
    println!(" * Idle Foundry Ltd., GPL-3.0-or-later). Do not edit: run the generator. 44.1 kHz, blocks of 32 samples.");
    println!(" * Units: log2 Hz x 3072, semitones x 4096, volts Q16, currents nA x 16, gains Q15 (k Q8). */");

    // the cutoff: CUTOFF's and the contour's currents into the control node, its 21 points, the ladder's law there
    let kc: Vec<i64> = (0..128).map(|i| r(light::knob_current(knob(i)) * IQ)).collect();
    let cc: Vec<i64> = (0..128).map(|i| r(light::contour_current(knob(i)) * IQ)).collect();
    let pts = light::cutoff_points();
    arr("MI_KC", "int32_t", &kc);
    arr("MI_CC", "int32_t", &cc);
    arr("MI_PTS", "int32_t", &pts.map(|p| r(p * IQ)));
    arr("MI_PLOG2", "int32_t", &laws::CUTOFF_LOG2.map(|v| r(f64::from(v) * LOG2Q)));
    arr("MI_TRACK", "int16_t", &laws::KEYBOARD_CONTROL.map(|v| r(v * 4096.0)));
    println!("#define MI_TRACK_FROM ({})", r(laws::TRACKING_FROM * ST));
    // the node's law between its points: each segment's slope, log2 x 3072 a nA x 16, Q24 (the ends continued at theirs)
    let ys = laws::CUTOFF_LOG2.map(f64::from);
    let sl: Vec<i64> = (0..20).map(|k| r((ys[k + 1] - ys[k]) * LOG2Q / ((pts[k + 1] - pts[k]) * IQ) * 16777216.0)).collect();
    arr("MI_PSLOPE", "int32_t", &sl);
    // what depends on the cutoff, on a grid of 1/3 octave from log2 4 (16 Hz) to 15: EMPHASIS's loop gain for each knob (Q8),
    // the corner's rise and the ring's raise (log2 x 3072), the passband (Q15), the pair's limit when ringing (top, its
    // reciprocal, its output at the bias: Q15), the saturation above CUTOFF_SAT_AT[0] (log2 x 3072, on its own grid from there)
    let grid: Vec<f64> = (0..34).map(|n| 4.0 + n as f64 / 3.0).collect();
    println!("#define MI_GRID0 ({})
#define MI_GRIDN 34", r(4.0 * LOG2Q));
    let mut ek = Vec::new();
    let mut ecorner = Vec::new();
    for i in 0..128 {
        let (g, c) = light::emphasis_law(knob(i));
        for &l in &grid {
            ek.push(r(interp(&laws::LOOP_AT_LOG2, &g, l) * 256.0));
        }
        ecorner.push(r(c * 32767.0));
    }
    arr("MI_EK", "uint16_t", &ek);
    arr("MI_ECORNER", "int16_t", &ecorner);
    arr("MI_CORNER", "int32_t", &grid.iter().map(|&l| r(interp(&laws::CORNER_AT_LOG2, &laws::CORNER, l) * LOG2Q)).collect::<Vec<_>>());
    arr("MI_RAISE", "int32_t", &grid.iter().map(|&l| r(interp(&laws::RING_RAISE_AT_LOG2, &laws::RING_RAISE, l) * LOG2Q)).collect::<Vec<_>>());
    arr("MI_PASS", "int32_t", &grid.iter().map(|&l| r(f64::from(light::passband(l)) * 32768.0)).collect::<Vec<_>>());
    let lift = |l: f64| f64::from(light::ring_lift(l, 0.0));
    arr("MI_TOP", "int32_t", &grid.iter().map(|&l| r(lift(l) * 32768.0)).collect::<Vec<_>>());
    arr("MI_TOPINV", "int32_t", &grid.iter().map(|&l| r(32768.0 / lift(l))).collect::<Vec<_>>());
    arr("MI_TOPRB", "int32_t", &grid.iter().map(|&l| r(lift(l) * (f64::from(laws::BIAS) / lift(l)).tanh() * 32768.0)).collect::<Vec<_>>());
    println!("#define MI_RB1 ({})", r((f64::from(laws::BIAS)).tanh() * 32768.0));
    let sat0 = laws::CUTOFF_SAT_AT[0];
    println!("#define MI_SAT0 ({})", r(sat0 * LOG2Q));
    arr("MI_SAT", "int32_t", &(0..31).map(|n| r(interp(&laws::CUTOFF_SAT_AT, &laws::CUTOFF_SAT, sat0 + n as f64 / 3.0) * LOG2Q)).collect::<Vec<_>>());
    // the ladder's one-pole gain g / (1 + g), g = tan(pi f / FS) at the voice's rate (f held at 0.45 FS), Q16, log2 Hz 4 + n / 12
    let gt: Vec<i64> = (0..132)
        .map(|n| {
            let hz = (2f64).powf(4.0 + n as f64 / 12.0).min(0.45 * FS);
            let t = (std::f64::consts::PI * hz / FS).tan();
            r(t / (1.0 + t) * 65536.0).min(65535)
        })
        .collect();
    arr("MI_G", "uint16_t", &gt);
    println!("#define MI_G_LOG2_0 ({})", r(4.0 * LOG2Q));
    println!("#define MI_DRIVE {}\n#define MI_BIAS {}\n#define MI_OUT {}", r(f64::from(laws::DRIVE) * 32768.0),
             r(f64::from(laws::BIAS) * 32768.0), r(f64::from(laws::OUT) * 32768.0));
    println!("#define MI_COUPLING {}", r((1.0 - (-2.0 * std::f64::consts::PI * laws::COUPLING_HZ / FS).exp()) * 65536.0));

    // the contours: each's coefficients a block for its knobs, its levels
    for (w, (al, dl)) in [(&FILTER_ATTACK, &FILTER_DECAY), (&LOUDNESS_ATTACK, &LOUDNESS_DECAY)].iter().enumerate() {
        let time = |(a, b, t0): (f64, f64, f64), ohms: f64| a * (ohms / 1e5).powf(b) + t0;
        let (rest, peak) = (laws::REST[w], laws::PEAK[w]);
        let aim = rest + laws::ATTACK_AIM[w] * (peak - rest);
        let (mut atk, mut stop, mut dec, mut relon, mut reloff, mut reldoff, mut sus) =
            (vec![], vec![], vec![], vec![], vec![], vec![], vec![]);
        for i in 0..128 {
            let tau_a = time(laws::ATTACK_TIME[w], time_pot(knob(i), al.as_slice()));
            let r_decay = time_pot(knob(i), dl.as_slice());
            atk.push(r(per_block(tau_a) * 65536.0));
            stop.push(r((peak - (aim - peak) * laws::ATTACK_ESR[w] / tau_a) * VQ));
            dec.push(r(per_block(time(laws::DECAY_TIME[w], r_decay)) * 65536.0));
            relon.push(r(per_block(time(laws::RELEASE_TIME[w], r_decay)) * 65536.0));
            reloff.push(r(per_block(interp(&laws::RELEASE_OFF_AT, &laws::RELEASE_OFF[w], knob(i))) * 65536.0));
            reldoff.push(r(interp(&laws::RELEASE_OFF_AT, &laws::RELEASED_OFF[w], knob(i)) * VQ));
            let [c0, c1, c2, c3] = laws::SUSTAIN[w];
            let s = sustain_track(knob(i));
            sus.push(r((c0 + s * (c1 + s * (c2 + s * c3))) * VQ));
        }
        let n = ["F", "L"][w];
        arr(&format!("MI_{n}ATK"), "uint16_t", &atk.iter().map(|&x| x.min(65535)).collect::<Vec<_>>());
        arr(&format!("MI_{n}STOP"), "int32_t", &stop);
        arr(&format!("MI_{n}DEC"), "uint16_t", &dec.iter().map(|&x| x.min(65535)).collect::<Vec<_>>());
        arr(&format!("MI_{n}RELON"), "uint16_t", &relon.iter().map(|&x| x.min(65535)).collect::<Vec<_>>());
        arr(&format!("MI_{n}RELOFF"), "uint16_t", &reloff.iter().map(|&x| x.min(65535)).collect::<Vec<_>>());
        arr(&format!("MI_{n}RELDOFF"), "int32_t", &reldoff);
        arr(&format!("MI_{n}SUS"), "int32_t", &sus);
        println!("#define MI_{n}AIM ({})\n#define MI_{n}RELEASED ({})", r(aim * VQ), r(laws::RELEASED[w] * VQ));
    }
    println!("#define MI_TRIG_ON {}\n#define MI_TRIG_OFF {}\n#define MI_REARM {}", r(laws::TRIGGER_ON * FS),
             r(laws::TRIGGER_OFF * FS), r(laws::REARM * FS));
    arr("MI_VCA", "int16_t", &laws::VCA_GAIN.map(|g| r(f64::from(g) * 32767.0).min(32767)));

    // GLIDE: semitones a block (x 4096) rising; falling GLIDE_FALLING of it
    let gl: Vec<i64> = (0..128)
        .map(|i| r((laws::GLIDE_RATE / (glide_r(knob(i), i > 0) + 330.0) * BLOCK / FS * ST).min(1e9)))
        .collect();
    arr("MI_GLIDE", "int32_t", &gl);
    println!("#define MI_GLIDE_FALL ({})", r(laws::GLIDE_FALLING * 32768.0));
    // beyond the keyboard's keys: the pitch's RC through GLIDE and the hold's 1 uF, its share left a block (Q16); the keys' ends
    arr("MI_BEYOND", "uint16_t", &(0..128).map(|i| {
        let ohms = glide_r(knob(i), i > 0) + 330.0;
        r((-(BLOCK / FS) / (ohms * laws::GLIDE_FARADS)).exp() * 65536.0).min(65535)
    }).collect::<Vec<_>>());
    println!("#define MI_KEYS_LO ({})\n#define MI_KEYS_HI ({})", r(laws::KEYBOARD_KEYS.0 * ST), r(laws::KEYBOARD_KEYS.1 * ST));
    println!("#define MI_PITCH_REST ({})", r(laws::PITCH_AT_REST * ST));

    // the mixer: each source's channel (triangle, sawtooth, rectangle, shark tooth) at each VOLUME, Q15; the noise's
    let ch: Vec<i64> = laws::SOURCE_OHMS
        .iter()
        .flat_map(|&ohms| (0..128).map(move |i| r(f64::from(light::channel(volume_track(knob(i)), ohms, 33e3)) * 32768.0)))
        .collect();
    arr("MI_CH", "uint16_t", &ch.iter().map(|&x| x.min(65535)).collect::<Vec<_>>());
    arr("MI_NOISE", "uint16_t", &(0..128).map(|i| r(f64::from(light::noise_gain(knob(i), false)) * 32768.0)).collect::<Vec<_>>());
    println!("#define MI_OSC3_LEVEL {}", r(f64::from(laws::OSC3_LEVEL) * 32768.0));
    // the waveforms' levels (Q15) and the rectangles' duty cycles (of 2^32)
    let waves = [
        Waveform::Triangle, Waveform::SharkTooth, Waveform::ReverseSawtooth, Waveform::Sawtooth,
        Waveform::Square, Waveform::WideRectangle, Waveform::NarrowRectangle,
    ];
    arr("MI_WLEVEL", "int32_t", &waves.map(|w| r(f64::from(light::Osc::new(w).peak()) * 32768.0)));
    println!("#define MI_SHARK_TRI {}\n#define MI_SHARK_SAW {}", r(f64::from(laws::SHARK_TRIANGLE) * 32768.0),
             r(f64::from(laws::SHARK_SAWTOOTH) * 32768.0));
    arr("MI_DUTY", "uint32_t", &laws::DUTY.map(|d| r(f64::from(d) * 4294967296.0).min(4294967295)));

    // the pitches: each RANGE's semitones from 8', FREQUENCY (MINI's dial) for oscillators 2 and 3 and 3 free of the keys
    let ranges = [Range::Lo, Range::R32, Range::R16, Range::R8, Range::R4, Range::R2];
    arr("MI_RANGE", "int32_t", &ranges.map(|g| r((2.0 - g.octaves_below_2()) * 12.0 * ST - 0.0)));
    let cubic = |c: [f64; 4], x: f64| c[0] + x * (c[1] + x * (c[2] + x * c[3]));
    let quint = |x: f64| laws::OSC3_FREE_CENTS.iter().rev().fold(0.0, |a, c| a * x + c);
    let centre = ca72::tuning::FREQ_CENTRE;
    let f2: Vec<i64> = (-64..64).map(|x| {
        let w = Pot::Two.wiper(dial(x));
        r((cubic(laws::OSC2_CENTS, w) - cubic(laws::OSC2_CENTS, centre)) / 100.0 * ST)
    }).collect();
    let f3: Vec<i64> = (-64..64).map(|x| {
        let w = Pot::Three.wiper(dial(x));
        r((cubic(laws::OSC3_CENTS, w) - cubic(laws::OSC3_CENTS, centre)) / 100.0 * ST)
    }).collect();
    let f3f: Vec<i64> = (-64..64).map(|x| {
        let w = Pot::Three.wiper(dial(x));
        r((quint(w) - quint(centre)) / 100.0 * ST)
    }).collect();
    arr("MI_FRQ2", "int32_t", &f2);
    arr("MI_FRQ3", "int32_t", &f3);
    arr("MI_FRQ3FREE", "int32_t", &f3f);
    println!("#define MI_OSC3_FREE_KEY ({})", r(laws::OSC3_FREE_KEY * ST));

    // the modulation: the wheel's depth and offset depth (Q15); oscillator 3 as its source, by its waveform: its swing to +-1 (Q12),
    // the rectangles' bias (Q12), semitones at the swing and its centre (x 4096); the noise's
    let depth = |wheel: f64| {
        let rr = mod_wheel_r(wheel);
        let w = (3.0 * (1.0 + rr / 1.2e3 * (81f64.powf(1.0 / 3.0) - 1.0)).ln() / 81f64.ln()).clamp(0.0, 1.0);
        let [a, b, c] = laws::WHEEL;
        w * (a + w * (b + w * c))
    };
    arr("MI_DEPTH", "int16_t", &(0..128).map(|i| r(depth(knob(i)) * 32767.0)).collect::<Vec<_>>());
    arr("MI_OFFD", "int16_t", &(0..128).map(|i| {
        let rr = mod_wheel_r(knob(i));
        r(laws::OFFSET.0 * rr / (rr + laws::OFFSET.1) * 32767.0)
    }).collect::<Vec<_>>());
    let kind = |w: Waveform| match w {
        Waveform::Triangle => 0,
        Waveform::ReverseSawtooth | Waveform::SharkTooth => 1,
        Waveform::Sawtooth => 2,
        _ => 3,
    };
    arr("MI_MKIND", "uint8_t", &waves.map(kind));
    arr("MI_MSWING", "int32_t", &laws::MOD_SWING.map(|v| r(v * ST)));
    arr("MI_MCENTRE", "int32_t", &laws::MOD_CENTRE.map(|v| r(v * ST)));
    arr("MI_SWING", "int32_t", &waves.map(|w| r(4096.0 / f64::from(light::Osc::new(w).peak()))));
    arr("MI_SWBIAS", "int32_t", &[0, 0, 0, 0, 1, 2, 3].map(|d: usize| if d == 0 { 0 } else { r((2.0 * f64::from(laws::DUTY[d - 1]) - 1.0) * 4096.0) }));
    println!("#define MI_FILTER_PER_ST {}", r(laws::FILTER_PER_SEMITONE * 4096.0));
    println!("#define MI_MOD_OFFSET0 ({})\n#define MI_MOD_OFFSET1 ({})", r(laws::MOD_OFFSET.0 * ST), r(laws::MOD_OFFSET.1 * LOG2Q));
    println!("#define MI_NOISE_OFFSET0 ({})\n#define MI_NOISE_OFFSET1 ({})", r(laws::NOISE_OFFSET.0 * ST), r(laws::NOISE_OFFSET.1 * LOG2Q));
    println!("#define MI_NOISE_ST_RMS ({})\n#define MI_NOISE_OCT_RMS ({})", r(laws::NOISE_SEMITONES_RMS * ST), r(laws::NOISE_OCTAVES_RMS * LOG2Q));
}
