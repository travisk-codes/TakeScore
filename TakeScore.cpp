#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <map>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <set>

// ─────────────────────────────────────────────
//  WAV File Parser
// ─────────────────────────────────────────────

struct WavFile {
    std::string        filename, shortName;
    uint32_t           sampleRate = 0;
    uint16_t           numChannels = 0;
    uint16_t           bitsPerSample = 0;
    std::vector<float> samples;
    bool               valid = false;
    std::string        error;
};

WavFile loadWav(const std::string& path) {
    WavFile wav;
    wav.filename = path;
    size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    wav.shortName = (dot == std::string::npos) ? name : name.substr(0, dot);

    std::ifstream f(path, std::ios::binary);
    if (!f) { wav.error = "Cannot open file"; return wav; }

    // Read RIFF header (12 bytes)
    char riffHdr[12];
    f.read(riffHdr, 12);
    if (!f || std::strncmp(riffHdr, "RIFF", 4) != 0 || std::strncmp(riffHdr + 8, "WAVE", 4) != 0) {
        wav.error = "Not a valid WAV file"; return wav;
    }

    // Scan chunks to find "fmt " and "data".
    // Many DAWs insert extra chunks (bext, iXML, JUNK, LIST, etc.) before
    // or between fmt and data, so we cannot assume a fixed layout.
    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0, fmtSize = 0;
    uint16_t effectiveFormat = 0;
    bool fmtFound = false;
    uint32_t dataSize = 0;
    bool dataFound = false;

    char chunkId[4]; uint32_t chunkSize;
    while (f.read(chunkId, 4) && f.read(reinterpret_cast<char*>(&chunkSize), 4)) {
        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            fmtSize = chunkSize;
            auto fmtStart = f.tellg();
            // Read base WAVEFORMATEX fields (16 bytes)
            if (fmtSize < 16) { wav.error = "Truncated fmt chunk"; return wav; }
            f.read(reinterpret_cast<char*>(&audioFormat), 2);
            f.read(reinterpret_cast<char*>(&numChannels), 2);
            f.read(reinterpret_cast<char*>(&sampleRate), 4);
            f.seekg(4, std::ios::cur); // skip byteRate
            f.seekg(2, std::ios::cur); // skip blockAlign
            f.read(reinterpret_cast<char*>(&bitsPerSample), 2);

            effectiveFormat = audioFormat;
            // WAVE_FORMAT_EXTENSIBLE: real format is in the SubFormat GUID
            if (audioFormat == 0xFFFE && fmtSize >= 40) {
                f.seekg(2, std::ios::cur); // cbSize
                f.seekg(2, std::ios::cur); // validBitsPerSample
                f.seekg(4, std::ios::cur); // channelMask
                uint16_t subFormat;
                f.read(reinterpret_cast<char*>(&subFormat), 2);
                effectiveFormat = subFormat;
            }
            // Seek to end of fmt chunk (handles any extra bytes)
            f.seekg(fmtStart + (std::streamoff)fmtSize);
            // Word-align
            if (fmtSize & 1) f.seekg(1, std::ios::cur);
            fmtFound = true;
        }
        else if (std::strncmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            dataFound = true;
            break; // data chunk body follows — stop scanning
        }
        else {
            // Skip unknown chunk (word-aligned)
            f.seekg(chunkSize + (chunkSize & 1), std::ios::cur);
        }
        if (!f) break;
    }

    if (!fmtFound) { wav.error = "No fmt chunk found"; return wav; }
    if (!dataFound) { wav.error = "No data chunk found"; return wav; }
    if (effectiveFormat != 1 && effectiveFormat != 3) {
        wav.error = "Unsupported format (code " + std::to_string(effectiveFormat) + ")";
        return wav;
    }

    wav.sampleRate = sampleRate;
    wav.numChannels = numChannels;
    wav.bitsPerSample = bitsPerSample;

    uint32_t numFrames = dataSize / (numChannels * bitsPerSample / 8);
    wav.samples.reserve(numFrames);
    for (uint32_t i = 0; i < numFrames && f.good(); ++i) {
        float mono = 0.f;
        for (uint16_t ch = 0; ch < numChannels; ++ch) {
            float s = 0.f;
            if (bitsPerSample == 16) {
                int16_t v; f.read(reinterpret_cast<char*>(&v), 2); s = v / 32768.f;
            }
            else if (bitsPerSample == 24) {
                uint8_t b[3]; f.read(reinterpret_cast<char*>(b), 3);
                int32_t v = (b[2] << 16) | (b[1] << 8) | b[0];
                if (v & 0x800000) v |= ~0x00FFFFFF; // sign-extend
                s = v / 8388608.f;
            }
            else if (bitsPerSample == 32 && effectiveFormat == 3) {
                f.read(reinterpret_cast<char*>(&s), 4);
            }
            else if (bitsPerSample == 32) {
                int32_t v; f.read(reinterpret_cast<char*>(&v), 4); s = v / 2147483648.f;
            }
            else if (bitsPerSample == 8) {
                uint8_t v; f.read(reinterpret_cast<char*>(&v), 1); s = (v - 128) / 128.f;
            }
            mono += s;
        }
        wav.samples.push_back(mono / numChannels);
    }
    wav.valid = true;
    return wav;
}

// ─────────────────────────────────────────────
//  Key / Scale
// ─────────────────────────────────────────────
struct Scale {
    std::string   name;
    std::set<int> pitchClasses;
};

Scale parseScale(const std::string& keyStr) {
    Scale sc;
    if (keyStr.empty()) return sc;
    static const std::map<char, int> base = { {'C',0},{'D',2},{'E',4},{'F',5},{'G',7},{'A',9},{'B',11} };
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(keyStr[0])));
    if (base.find(c) == base.end()) {
        std::cerr << "Warning: unknown key '" << keyStr << "'\n"; return sc;
    }
    int root = base.at(c), consumed = 1;
    if (keyStr.size() > 1 && keyStr[1] == '#') { root = (root + 1) % 12; consumed = 2; }
    else if (keyStr.size() > 1 && keyStr[1] == 'b') { root = (root + 11) % 12; consumed = 2; }

    std::string suf = keyStr.substr(consumed);
    for (auto& ch : suf) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    // Supported modes/scales:
    //   maj/major          C D E F G A B         (ionian)
    //   m/min/minor        C D Eb F G Ab Bb      (aeolian / natural minor)
    //   dor/dorian         C D Eb F G A Bb       (minor with raised 6th)
    //   mix/mixolydian     C D E F G A Bb        (major with flat 7th)
    //   lyd/lydian         C D E F# G A B        (major with raised 4th)
    //   phry/phrygian      C Db Eb F G Ab Bb     (minor with flat 2nd)
    //   loc/locrian        C Db Eb F Gb Ab Bb
    //   pent/pentatonic    C D E G A             (major pentatonic)
    //   mpent/minpent      C Eb F G Bb           (minor pentatonic)
    //   blues              C Eb F F# G Bb
    struct ModeEntry { const char* suffix; std::vector<int> intervals; const char* label; };
    static const ModeEntry modes[] = {
        { "dor",          {0,2,3,5,7,9,10},     "dor"  },
        { "dorian",       {0,2,3,5,7,9,10},     "dor"  },
        { "mix",          {0,2,4,5,7,9,10},     "mix"  },
        { "mixolydian",   {0,2,4,5,7,9,10},     "mix"  },
        { "lyd",          {0,2,4,6,7,9,11},     "lyd"  },
        { "lydian",       {0,2,4,6,7,9,11},     "lyd"  },
        { "phry",         {0,1,3,5,7,8,10},     "phry" },
        { "phrygian",     {0,1,3,5,7,8,10},     "phry" },
        { "loc",          {0,1,3,5,6,8,10},     "loc"  },
        { "locrian",      {0,1,3,5,6,8,10},     "loc"  },
        { "pent",         {0,2,4,7,9},           "pent" },
        { "pentatonic",   {0,2,4,7,9},           "pent" },
        { "mpent",        {0,3,5,7,10},          "mpent"},
        { "minpent",      {0,3,5,7,10},          "mpent"},
        { "blues",        {0,3,5,6,7,10},        "blues"},
        { "minor",        {0,2,3,5,7,8,10},     "min"  },
        { "min",          {0,2,3,5,7,8,10},     "min"  },
        { "m",            {0,2,3,5,7,8,10},     "min"  },
        { "major",        {0,2,4,5,7,9,11},     "maj"  },
        { "maj",          {0,2,4,5,7,9,11},     "maj"  },
        { "",             {0,2,4,5,7,9,11},     "maj"  },  // default = major
    };

    static const char* nn[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    for (const auto& m : modes) {
        if (suf == m.suffix) {
            sc.name = std::string(nn[root]) + m.label;
            for (int i : m.intervals) sc.pitchClasses.insert((root + i) % 12);
            return sc;
        }
    }

    // Fallback: unrecognized suffix → treat as major
    std::cerr << "Warning: unknown mode '" << suf << "', defaulting to major\n";
    sc.name = std::string(nn[root]) + "maj";
    for (int i : {0,2,4,5,7,9,11}) sc.pitchClasses.insert((root + i) % 12);
    return sc;
}

// ─────────────────────────────────────────────
//  YIN Pitch Detection (improved)
// ─────────────────────────────────────────────
//
// Improvements over baseline YIN:
//   1. Adaptive window size: scales with sample rate so low frequencies
//      always get at least two full periods inside the analysis window.
//   2. Confidence output: the CMNDF dip value is returned alongside the
//      pitch, letting the caller make better voiced/unvoiced decisions.
//   3. Octave-error correction: after finding a candidate period T, we
//      check whether 2T (the sub-harmonic / true fundamental) has a
//      CMNDF value that is nearly as good.  If so, we pick 2T.  This is
//      the single most common YIN failure mode.
//   4. Global-minimum fallback: when no dip crosses the primary threshold,
//      the best local minimum is returned with a reduced confidence score
//      instead of silently returning 0 Hz.
// ─────────────────────────────────────────────

static const float YIN_THR       = 0.12f;   // primary CMNDF threshold (slightly tighter)
static const float YIN_THR_FALL  = 0.30f;   // fallback: accept global minimum up to this

struct YinResult { float hz; float confidence; };

// Adaptive window: 4096 @ 44100/48000, 8192 @ 88200/96000, etc.
// Rounded up to next power of two for cache-friendliness.
// Using 0.085s (~4096 @ 44.1k) gives YIN ~10 periods for notes around
// A#3 (233 Hz), producing much more reliable CMNDF dips at the true
// fundamental.  The old 0.042s (2048) only gave ~5 periods, causing
// YIN to frequently lock onto harmonics on synth timbres with strong
// overtones.
static int yinWindowSize(int sr) {
    int target = (int)(sr * 0.085f);
    int w = 256;
    while (w < target) w <<= 1;
    return w;
}

// Parabolic interpolation around index `tau` in array `c` of length `len`.
// Returns fractional offset from tau (in range roughly -0.5 .. +0.5).
static float parabolicShift(const std::vector<float>& c, int tau, int len) {
    if (tau < 1 || tau >= len - 1) return 0.f;
    float s0 = c[tau - 1], s1 = c[tau], s2 = c[tau + 1];
    float denom = 2.f * s1 - s2 - s0;
    if (std::abs(denom) < 1e-12f) return 0.f;
    return (s2 - s0) / (2.f * denom);
}

// hintHz: optional pitch from previous frame for continuity tracking.
// When provided, if there's a reasonable CMNDF dip near the hint period,
// prefer it over a slightly-better dip elsewhere.  This prevents YIN from
// hopping between harmonics frame-to-frame on signals with rich overtones.
YinResult yinPitch(const float* buf, int N, int sr, float hintHz = 0.f) {
    int H = N / 2;
    // Lag range: MIN_PER corresponds to ~2200 Hz, MAX_PER to ~55 Hz
    int minPer = std::max(2, (int)(sr / 2200.f));
    int maxPer = std::min(H - 2, (int)(sr / 55.f));

    // Step 1 – Difference function  d(tau) = sum_j (buf[j] - buf[j+tau])^2
    std::vector<float> d(H, 0.f);
    for (int tau = 1; tau < H; ++tau)
        for (int j = 0; j < H; ++j) { float dv = buf[j] - buf[j + tau]; d[tau] += dv * dv; }

    // Step 2 – Cumulative Mean Normalized Difference Function (CMNDF)
    std::vector<float> c(H, 0.f); c[0] = 1.f;
    float rs = 0.f;
    for (int tau = 1; tau < H; ++tau) { rs += d[tau]; c[tau] = d[tau] * tau / (rs + 1e-10f); }

    // Step 3 – Threshold search: find first dip below YIN_THR, then walk to its local minimum.
    int bestTau = -1;
    float bestVal = 1e9f;
    // Also track global best minimum for fallback
    int globalBestTau = -1;
    float globalBestVal = 1e9f;

    for (int tau = minPer; tau <= maxPer; ++tau) {
        // Track global minimum
        if (c[tau] < globalBestVal && tau > 0 && tau < H - 1 &&
            c[tau] <= c[tau - 1] && c[tau] <= c[tau + 1]) {
            globalBestVal = c[tau];
            globalBestTau = tau;
        }
    }

    // Primary search: first dip below threshold, walked to local minimum
    {
        int tau = minPer;
        while (tau <= maxPer && tau < H - 1) {
            if (c[tau] < YIN_THR) {
                while (tau + 1 < H - 1 && tau + 1 <= maxPer && c[tau + 1] < c[tau]) ++tau;
                bestTau = tau;
                bestVal = c[tau];
                break;
            }
            ++tau;
        }
    }

    // Step 4 – Fallback to global minimum if primary search found nothing
    if (bestTau < 0 && globalBestTau > 0 && globalBestVal < YIN_THR_FALL) {
        bestTau = globalBestTau;
        bestVal = globalBestVal;
    }

    if (bestTau < 0) return { 0.f, 0.f };

    // Step 5 – Bidirectional octave-error correction.
    //
    // 5a: Octave-up fix (YIN locked onto 2nd harmonic → period is half
    //     the true fundamental).  Check if 2*tau has a reasonable dip.
    //     A moderate tolerance of 0.05 lets the true fundamental win when
    //     it's close, without the false-drop problems of the old 0.15.
    {
        int tau2 = bestTau * 2;
        if (tau2 + 1 < H && tau2 <= maxPer) {
            // Walk tau2 to its local minimum
            while (tau2 + 1 < H - 1 && tau2 + 1 <= maxPer && c[tau2 + 1] < c[tau2]) ++tau2;
            if (tau2 > 0 && tau2 < H - 1 && c[tau2] < bestVal + 0.05f) {
                bestTau = tau2;
                bestVal = c[tau2];
            }
        }
    }

    // 5b: Octave-down fix (YIN locked onto sub-harmonic → period is
    //     double the true fundamental).  Check if tau/2 has a good dip.
    //     Only prefer it if the dip is below the primary threshold,
    //     meaning it's a genuine pitch candidate, not just noise.
    {
        int tauH = bestTau / 2;
        if (tauH >= minPer && tauH > 0 && tauH < H - 1) {
            // Walk to local minimum around tau/2
            while (tauH - 1 >= minPer && c[tauH - 1] < c[tauH]) --tauH;
            while (tauH + 1 < H - 1 && tauH + 1 <= maxPer && c[tauH + 1] < c[tauH]) ++tauH;
            if (tauH > 0 && tauH < H - 1 && c[tauH] < YIN_THR) {
                bestTau = tauH;
                bestVal = c[tauH];
            }
        }
    }

    // Step 6 – Pitch-continuity hint: if a previous frame's pitch is
    // available, check whether there's a good CMNDF dip near the expected
    // period.  Accept it if its CMNDF is within a small tolerance of the
    // current best, which prevents harmonic-hopping on sustained notes.
    if (hintHz > 0.f && bestTau > 0) {
        int hintTau = (int)(sr / hintHz + 0.5f);
        // Search ±6% around hint (covers ~1 semitone of drift)
        int hLo = std::max(minPer, (int)(hintTau * 0.94f));
        int hHi = std::min(maxPer, std::min(H - 2, (int)(hintTau * 1.06f)));
        int hBest = -1;
        float hVal = 1e9f;
        for (int tau = hLo; tau <= hHi; ++tau) {
            if (tau > 0 && tau < H - 1 && c[tau] <= c[tau - 1] && c[tau] <= c[tau + 1] && c[tau] < hVal) {
                hVal = c[tau];
                hBest = tau;
            }
        }
        // Prefer hint candidate if it's reasonably good (within 0.1 of best)
        if (hBest > 0 && hVal < bestVal + 0.10f) {
            bestTau = hBest;
            bestVal = hVal;
        }
    }

    // Step 7 – Parabolic interpolation for sub-sample accuracy
    float shift = parabolicShift(c, bestTau, H);
    float period = bestTau + shift;
    if (period < 1.f) return { 0.f, 0.f };

    float hz = (float)sr / period;
    float confidence = 1.f - bestVal;  // CMNDF dip value inverted: 1.0 = perfect, 0.0 = noise
    return { hz, std::max(0.f, std::min(1.f, confidence)) };
}

// ─────────────────────────────────────────────
//  BPM Auto-Detection
// ─────────────────────────────────────────────
float detectBPM(const std::vector<float>& samples, int sr) {
    const int hop = sr / 20; // 50ms
    int N = (int)samples.size();
    std::vector<float> energy;
    for (int i = 0; i + hop <= N; i += hop) {
        float e = 0; for (int j = i; j < i + hop; ++j) e += samples[j] * samples[j];
        energy.push_back(std::sqrt(e / hop));
    }
    if ((int)energy.size() < 20) return 0.f;
    // Autocorrelation over range 50-240 BPM (50ms hop)
    const int lo = 5, hi = 24;
    float best = -1; int bLag = 10;
    for (int lag = lo; lag <= hi && lag < (int)energy.size(); ++lag) {
        float corr = 0; int cnt = 0;
        for (int i = 0; i + lag < (int)energy.size(); ++i) { corr += energy[i] * energy[i + lag]; ++cnt; }
        corr /= (cnt + 1e-10f);
        if (corr > best) { best = corr; bLag = lag; }
    }
    float secPerHop = (float)hop / sr;
    float bpm = 60.f / (bLag * secPerHop);
    // Snap to nearest common BPM
    static const float common[] = { 60,70,75,80,85,90,95,100,105,110,115,120,
                                   125,128,130,135,140,145,150,160,170,180,0 };
    float bst = bpm, bdist = 1e9f;
    for (int i = 0; common[i] > 0; ++i) { float d = std::abs(bpm - common[i]); if (d < bdist) { bdist = d; bst = common[i]; } }
    return bdist < 15.f ? bst : bpm;
}

// ─────────────────────────────────────────────
//  Pitch Analysis
// ─────────────────────────────────────────────
struct PitchFrame { float timeS, hz, midiNote, confidence; bool voiced; };

float hzToMidi(float hz) { return hz <= 0 ? 0 : 69.f + 12.f * std::log2(hz / 440.f); }

std::string midiToName(float midi) {
    static const char* N[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    int n = (int)std::round(midi);
    return std::string(N[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1);
}

void fillVoicedGaps(std::vector<PitchFrame>& frames, int maxGap = 3, float tol = 2.5f) {
    int N = (int)frames.size();
    for (int i = 1; i < N; ++i) {
        if (frames[i].voiced) continue;
        int gs = i, ge = i;
        while (ge + 1 < N && !frames[ge + 1].voiced) ++ge;
        int gl = ge - gs + 1;
        if (gl > maxGap) { i = ge; continue; }
        if (gs > 0 && ge + 1 < N && frames[gs - 1].voiced && frames[ge + 1].voiced
            && std::abs(frames[gs - 1].midiNote - frames[ge + 1].midiNote) < tol) {
            for (int j = gs; j <= ge; ++j) {
                float t = (float)(j - gs + 1) / (gl + 1);
                frames[j].midiNote = frames[gs - 1].midiNote * (1 - t) + frames[ge + 1].midiNote * t;
                frames[j].hz = 440.f * std::pow(2.f, (frames[j].midiNote - 69.f) / 12.f);
                frames[j].confidence = std::min(frames[gs - 1].confidence, frames[ge + 1].confidence) * 0.7f;
                frames[j].voiced = true;
            }
        }
        i = ge;
    }
}

struct TakeAnalysis {
    std::string name, filename;
    std::vector<PitchFrame> frames;
    std::vector<float> waveformPeaks;
    float voicedRatio, pitchMean, pitchStdDev;
    float rmsLevel, stabilityScore, pitchAccuracyScore, avgCentsOff, overallScore;
    float noteAvgCents[12]; int noteCount[12];
    uint32_t sampleRate;
    float durationS;
};

// Chromatic nearest semitone (no scale constraint) - used for per-note stability
static float nearestNoteSimple(float midi) { return std::round(midi); }

TakeAnalysis analyzeTake(const WavFile& wav, const Scale& scale) {
    TakeAnalysis ta;
    ta.name = wav.shortName; ta.filename = wav.filename;
    ta.sampleRate = wav.sampleRate;
    ta.durationS = (float)wav.samples.size() / wav.sampleRate;

    int N = (int)wav.samples.size();
    float sumSq = 0; for (float s : wav.samples) sumSq += s * s;
    ta.rmsLevel = std::sqrt(sumSq / std::max(1, N));

    // Waveform peaks (500 points)
    {
        int chunk = std::max(1, N / 500);
        for (int i = 0; i < N; i += chunk) {
            float pk = 0; for (int j = i; j < std::min(i + chunk, N); ++j) pk = std::max(pk, std::abs(wav.samples[j]));
            ta.waveformPeaks.push_back(pk);
        }
    }

    // Pitch frames (adaptive window size, confidence-based voicing)
    const int WIN = yinWindowSize(wav.sampleRate);
    const int HOP = WIN / 2;
    const float VOICE_CONF_THR = 0.65f;  // minimum YIN confidence to consider voiced
    // RMS energy gate: when the analysis window straddles a note boundary
    // (half silence, half signal), YIN produces unreliable pitch estimates
    // that show up as U-shaped dips at note onsets/offsets.  Computing
    // per-window RMS and requiring a minimum energy level cleanly trims
    // these garbage frames.
    const float RMS_GATE = 0.005f;  // ~-46 dBFS — below this is silence/noise
    float prevVoicedHz = 0.f;  // pitch continuity hint for YIN
    for (int i = 0; i + WIN <= N; i += HOP) {
        float t = (float)i / wav.sampleRate;
        // Per-window RMS
        float winRms = 0.f;
        for (int j = i; j < i + WIN; ++j) winRms += wav.samples[j] * wav.samples[j];
        winRms = std::sqrt(winRms / WIN);
        if (winRms < RMS_GATE) {
            ta.frames.push_back({ t, 0.f, 0.f, 0.f, false });
            prevVoicedHz = 0.f;  // reset hint on silence
            continue;
        }
        YinResult yr = yinPitch(wav.samples.data() + i, WIN, wav.sampleRate, prevVoicedHz);
        float midi = hzToMidi(yr.hz);
        bool voiced = (yr.hz > 55.f && yr.hz < 1800.f && yr.confidence >= VOICE_CONF_THR);
        if (voiced) prevVoicedHz = yr.hz;
        ta.frames.push_back({ t, yr.hz, midi, yr.confidence, voiced });
    }
    fillVoicedGaps(ta.frames);

    // Outlier rejection: mark frames as unvoiced if they deviate more than
    // 4 semitones from the local median (window ±8 frames).
    // Catches octave-jump artifacts and random YIN misfires.
    {
        const int HW = 8;
        int NF = (int)ta.frames.size();
        std::vector<bool> outlier(NF, false);
        for (int i = 0; i < NF; ++i) {
            if (!ta.frames[i].voiced) continue;
            std::vector<float> win;
            win.reserve(HW * 2 + 1);
            for (int j = std::max(0, i - HW); j <= std::min(NF - 1, i + HW); ++j)
                if (ta.frames[j].voiced) win.push_back(ta.frames[j].midiNote);
            if (win.size() < 3) continue;
            std::sort(win.begin(), win.end());
            float median = win[win.size() / 2];
            if (std::abs(ta.frames[i].midiNote - median) > 4.0f)
                outlier[i] = true;
        }
        for (int i = 0; i < NF; ++i)
            if (outlier[i]) ta.frames[i].voiced = false;
        // Re-run gap fill after outlier removal
        fillVoicedGaps(ta.frames);
    }

    // Median filter on MIDI values.
    // Unlike Gaussian smoothing (which averages and can blend correct frames
    // with octave-error frames, producing smooth but wrong curves), a median
    // filter is robust to outliers — it selects the middle value, so a few
    // bad frames among many good ones are simply ignored.
    // Half-width of ~3 frames (~70 ms at 44.1k) is enough to reject
    // isolated YIN misfires without smearing real note transitions.
    // Never bridges voiced/unvoiced boundaries.
    {
        const float MF_SEC = 0.070f;
        const float frameDurS = (float)HOP / wav.sampleRate;
        const int   HW = std::max(1, (int)(MF_SEC / frameDurS + 0.5f));
        int NF = (int)ta.frames.size();
        std::vector<float> filtered(NF, 0.f);
        std::vector<bool>  filtValid(NF, false);
        for (int i = 0; i < NF; ++i) {
            if (!ta.frames[i].voiced) continue;
            std::vector<float> win;
            for (int j = std::max(0, i - HW); j <= std::min(NF - 1, i + HW); ++j) {
                if (ta.frames[j].voiced) win.push_back(ta.frames[j].midiNote);
            }
            if (win.empty()) continue;
            std::sort(win.begin(), win.end());
            filtered[i] = win[win.size() / 2];
            filtValid[i] = true;
        }
        for (int i = 0; i < NF; ++i) {
            if (!filtValid[i]) continue;
            ta.frames[i].midiNote = filtered[i];
            ta.frames[i].hz = 440.f * std::pow(2.f, (filtered[i] - 69.f) / 12.f);
        }
    }

    std::vector<float> vm;
    for (auto& f : ta.frames) if (f.voiced) vm.push_back(f.midiNote);
    ta.voicedRatio = ta.frames.empty() ? 0.f : (float)vm.size() / ta.frames.size();

    if (!vm.empty()) {
        float sum = std::accumulate(vm.begin(), vm.end(), 0.f);
        ta.pitchMean = sum / vm.size();
        float var = 0; for (float m : vm) var += (m - ta.pitchMean) * (m - ta.pitchMean);
        ta.pitchStdDev = std::sqrt(var / vm.size());
    }
    else { ta.pitchMean = ta.pitchStdDev = 0.f; }

    // Per-note stability: avg std dev of cents *within* each sustained note.
    // This is meaningful for melodies - it measures wobble on each note
    // independently, ignoring the intentional pitch movement between notes.
    {
        float sumSD = 0.f; int segCount = 0;
        int NF = (int)ta.frames.size(), i = 0;
        while (i < NF) {
            if (!ta.frames[i].voiced) { ++i; continue; }
            float note = nearestNoteSimple(ta.frames[i].midiNote);
            int start = i;
            while (i < NF && ta.frames[i].voiced &&
                nearestNoteSimple(ta.frames[i].midiNote) == note) ++i;
            int len = i - start;
            if (len < 3) continue;
            float sum = 0.f, sum2 = 0.f;
            for (int j = start; j < i; ++j) {
                float c = (ta.frames[j].midiNote - note) * 100.f;
                sum += c; sum2 += c * c;
            }
            float mean = sum / len;
            float sd = std::sqrt(std::max(0.f, sum2 / len - mean * mean));
            sumSD += sd; ++segCount;
        }
        float avgSD = segCount > 0 ? sumSD / segCount : 0.f;
        ta.stabilityScore = std::max(0.f, 100.f - avgSD * 4.f);
    }

    // Nearest scale note helper.
    // Search all semitones within +/-6 of the raw midi value and pick the one
    // with the smallest absolute distance. Using round() as the starting point
    // caused notes near a semitone boundary to snap to the wrong neighbour,
    // making a flat A# appear sharp relative to A (and vice-versa).
    bool hasScale = !scale.pitchClasses.empty();
    auto nearestNote = [&](float midi)->float {
        // Search ±6 semitones to handle gapped scales (pentatonic gaps can be
        // 3 semitones; ±6 covers any 7-note-or-fewer scale comfortably).
        int center = (int)std::round(midi);
        float bestDist = 1e9f;
        int bestNote = center;
        for (int c = center - 6; c <= center + 6; ++c) {
            if (hasScale) {
                int pc = ((c % 12) + 12) % 12;
                if (!scale.pitchClasses.count(pc)) continue;
            }
            float dist = std::abs(midi - (float)c);
            if (dist < bestDist) { bestDist = dist; bestNote = c; }
        }
        return (float)bestNote;
    };

    // Vibrato-aware accuracy: windowed median → nearest note → cents deviation
    if (!vm.empty()) {
        const int HW = 6;
        float sumC = 0; int cnt = 0;
        for (int i = 0; i < (int)ta.frames.size(); ++i) {
            if (!ta.frames[i].voiced) continue;
            std::vector<float> win;
            for (int j = std::max(0, i - HW); j <= std::min((int)ta.frames.size() - 1, i + HW); ++j)
                if (ta.frames[j].voiced) win.push_back(ta.frames[j].midiNote);
            std::sort(win.begin(), win.end());
            float median = win[win.size() / 2];
            float target = nearestNote(median);
            sumC += std::min(50.f, std::abs(ta.frames[i].midiNote - target) * 100.f);
            ++cnt;
        }
        ta.avgCentsOff = cnt ? sumC / cnt : 50.f;
        ta.pitchAccuracyScore = std::max(0.f, 100.f - ta.avgCentsOff * 2.f);
    }
    else { ta.avgCentsOff = 50.f; ta.pitchAccuracyScore = 0.f; }

    // Per-note accuracy breakdown by pitch class
    std::fill(ta.noteAvgCents, ta.noteAvgCents + 12, 0.f);
    std::fill(ta.noteCount, ta.noteCount + 12, 0);
    {
        float noteSumCents[12] = {};
        for (auto& f : ta.frames) {
            if (!f.voiced) continue;
            float target = nearestNote(f.midiNote);
            float cents = (f.midiNote - target) * 100.f;
            int pc = (((int)std::round(target)) % 12 + 12) % 12;
            noteSumCents[pc] += cents;
            ta.noteCount[pc]++;
        }
        for (int pc = 0; pc < 12; ++pc)
            if (ta.noteCount[pc] > 0)
                ta.noteAvgCents[pc] = noteSumCents[pc] / ta.noteCount[pc];
    }

    // Score: Accuracy 45%, Stability 35%, Voiced 15%, Level 5%
    float vs = std::min(100.f, ta.voicedRatio * 120.f);
    float ls = std::min(100.f, ta.rmsLevel * 400.f);
    ta.overallScore = 0.45f * ta.pitchAccuracyScore + 0.35f * ta.stabilityScore + 0.15f * vs + 0.05f * ls;
    return ta;
}

// ─────────────────────────────────────────────
//  HTML Report Generator
// ─────────────────────────────────────────────
std::string escJ(const std::string& s) {
    std::string o; for (char c : s) { if (c == '"')o += "\\\""; else if (c == '\\')o += "\\\\"; else o += c; } return o;
}

void generateReport(const std::vector<TakeAnalysis>& takes,
    const std::string& outPath, float bpm, const Scale& scale) {
    int wi = 0;
    for (int i = 1; i < (int)takes.size(); ++i)
        if (takes[i].overallScore > takes[wi].overallScore) wi = i;

    // JSON
    std::ostringstream jss; jss << std::fixed << std::setprecision(3);
    jss << "[\n";
    for (int ti = 0; ti < (int)takes.size(); ++ti) {
        const auto& t = takes[ti];
        jss << "  {\n";
        jss << "    \"name\":\"" << escJ(t.name) << "\",\n";
        jss << "    \"filename\":\"" << escJ(t.filename) << "\",\n";
        jss << "    \"duration\":" << t.durationS << ",\n";
        jss << "    \"sampleRate\":" << t.sampleRate << ",\n";
        jss << "    \"voicedRatio\":" << t.voicedRatio << ",\n";
        jss << "    \"pitchMean\":" << t.pitchMean << ",\n";
        jss << "    \"pitchStdDev\":" << t.pitchStdDev << ",\n";
        jss << "    \"stabilityScore\":" << t.stabilityScore << ",\n";
        jss << "    \"pitchAccuracyScore\":" << t.pitchAccuracyScore << ",\n";
        jss << "    \"avgCentsOff\":" << t.avgCentsOff << ",\n";
        jss << "    \"noteAcc\":["; {
            bool naFirst = true;
            for (int pc = 0; pc < 12; ++pc) {
                if (t.noteCount[pc] < 5) continue;
                if (!naFirst) jss << ",";
                jss << "{\"pc\":" << pc << ",\"avg\":" << t.noteAvgCents[pc]
                    << ",\"n\":" << t.noteCount[pc] << "}";
                naFirst = false;
            }
            jss << "],\n";
        }

        jss << "    \"wave\":[";
        for (int i = 0; i < (int)t.waveformPeaks.size(); ++i) {
            jss << t.waveformPeaks[i];
            if (i + 1 < (int)t.waveformPeaks.size()) jss << ",";
        }
        jss << "],\n";

        int step = std::max(1, (int)t.frames.size() / 2000);
        jss << "    \"frames\":[";
        for (int fi = 0; fi < (int)t.frames.size(); fi += step) {
            const auto& fr = t.frames[fi];
            jss << "{\"t\":" << fr.timeS << ",\"hz\":" << fr.hz
                << ",\"midi\":" << fr.midiNote << ",\"v\":" << (fr.voiced ? 1 : 0) << "}";
            if (fi + step < (int)t.frames.size()) jss << ",";
        }
        jss << "]\n  }";
        if (ti + 1 < (int)takes.size()) jss << ",";
        jss << "\n";
    }
    jss << "]";

    std::ostringstream scaleJ; scaleJ << "[";
    bool first = true;
    for (int pc : scale.pitchClasses) { if (!first)scaleJ << ","; scaleJ << pc; first = false; }
    scaleJ << "]";

    std::ofstream html(outPath);
    auto W = [&](const std::string& s) {html << s; };

    W("<!DOCTYPE html><html lang=\"en\"><head>\n");
    W("<meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n");
    W("<title>TakeAnalyzer</title>\n");
    W("<link href=\"https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;600&family=IBM+Plex+Sans:wght@400;500;600&display=swap\" rel=\"stylesheet\">\n");

    // ── CSS ──────────────────────────────────
    W("<style>\n");
    W(":root{--bg:#0c0c0c;--s1:#141414;--s2:#1a1a1a;--s3:#202020;--border:#252525;--border2:#2e2e2e;--text:#d8d8d8;--muted:#4a4a4a;--dim:#333;--win:#22c55e;--acc:#3b82f6;}\n");
    W("*{box-sizing:border-box;margin:0;padding:0}\n");
    W("html,body{height:100vh;overflow:hidden;background:var(--bg);color:var(--text);font-family:'IBM Plex Sans',sans-serif;font-size:13px;}\n");
    W(".app{display:flex;flex-direction:column;height:100vh;overflow:hidden}\n");
    // Header
    W(".hdr{display:flex;align-items:center;gap:12px;padding:0 14px;height:38px;border-bottom:1px solid var(--border);flex-shrink:0;background:var(--s1)}\n");
    W(".hdr-title{font-weight:600;font-size:13px;letter-spacing:-.2px}\n");
    W(".hdr-title span{color:var(--muted);font-weight:400}\n");
    W(".hdr-meta{font-family:'IBM Plex Mono',monospace;font-size:10px;color:var(--muted)}\n");
    W(".badge{background:var(--s2);border:1px solid var(--border2);border-radius:3px;padding:1px 7px;font-family:'IBM Plex Mono',monospace;font-size:10px;color:var(--muted)}\n");
    // Body
    W(".body{display:flex;flex:1;min-height:0}\n");
    // Left panel
    W(".lpanel{width:300px;flex-shrink:0;display:flex;flex-direction:column;border-right:1px solid var(--border);overflow:hidden}\n");
    W(".ph{display:flex;align-items:center;justify-content:space-between;padding:7px 10px;border-bottom:1px solid var(--border);flex-shrink:0}\n");
    W(".ph .lbl{font-family:'IBM Plex Mono',monospace;font-size:9px;letter-spacing:2px;text-transform:uppercase;color:var(--muted)}\n");
    W(".exp-btn{background:var(--s2);border:1px solid var(--border2);color:var(--muted);font-family:'IBM Plex Mono',monospace;font-size:9px;padding:3px 8px;border-radius:3px;cursor:pointer;transition:color .15s}\n");
    W(".exp-btn:hover{color:var(--text);border-color:var(--dim)}\n");
    W(".tw{overflow-y:auto;flex:1}\n");
    W("table{width:100%;border-collapse:collapse;font-family:'IBM Plex Mono',monospace;font-size:11px}\n");
    W("thead th{padding:5px 7px;color:var(--muted);font-size:9px;font-weight:400;letter-spacing:1px;text-transform:uppercase;border-bottom:1px solid var(--border);text-align:left;position:sticky;top:0;background:var(--bg)}\n");
    W("tbody td{padding:6px 7px;border-bottom:1px solid var(--border);vertical-align:middle}\n");
    W("tbody tr{cursor:pointer;transition:background .1s}\n");
    W("tbody tr:hover td{background:var(--s1)}\n");
    W("tbody tr.arow td{background:var(--s2)}\n");
    W("tbody tr.winner td.nc{color:var(--win)}\n");
    W(".rnk{display:inline-flex;align-items:center;justify-content:center;width:17px;height:17px;border-radius:3px;background:var(--s2);font-size:9px;font-weight:600}\n");
    W(".rnk.r1{background:var(--win);color:#0c0c0c}\n");
    W(".sb{display:flex;align-items:center;gap:5px}\n");
    W(".sb-t{height:2px;width:44px;background:var(--s3);border-radius:1px}\n");
    W(".sb-f{height:100%;border-radius:1px;background:var(--dim)}\n");
    W(".sb-f.win{background:var(--win)}\n");
    W(".g{color:var(--win)}.b{color:#ef4444}\n");
    // Right panel
    W(".rpanel{flex:1;display:flex;flex-direction:column;min-width:0;overflow:hidden}\n");
    W(".toolbar{display:flex;align-items:stretch;border-bottom:1px solid var(--border);flex-shrink:0;height:34px;overflow:hidden}\n");
    W(".tabs{display:flex;overflow-x:auto;flex-shrink:0}\n");
    W(".tab{background:none;border:none;border-right:1px solid var(--border);border-bottom:2px solid transparent;color:var(--muted);font-family:'IBM Plex Mono',monospace;font-size:10px;padding:0 11px;cursor:pointer;white-space:nowrap;transition:color .1s}\n");
    W(".tab:hover{color:var(--text)}\n");
    W(".tab.active{color:var(--text);border-bottom-color:var(--text)}\n");
    W(".tab.wt.active{color:var(--win);border-bottom-color:var(--win)}\n");
    W(".sbar{display:flex;align-items:center;gap:0;flex:1;overflow:hidden;font-family:'IBM Plex Mono',monospace;font-size:10px;color:var(--muted)}\n");
    W(".si{white-space:nowrap;padding:0 8px;border-right:1px solid var(--border);height:100%;display:flex;align-items:center;gap:4px}\n");
    W(".si .l{color:var(--muted)}\n");
    W(".si .v{color:var(--text)}\n");
    W(".si.ws .v{color:var(--win)}\n");
    // Chart area
    W(".chart-area{flex:1;display:flex;min-height:0}\n");
    W(".ccol{flex:1;display:flex;flex-direction:column;min-width:0}\n");
    W(".cwrap{flex:1;position:relative;min-height:0}\n");
    W("#cv{position:absolute;inset:0;width:100%;height:100%;cursor:grab;display:block}\n");
    W("#cv.drag{cursor:grabbing}\n");
    W(".wstrip{height:42px;flex-shrink:0;border-top:1px solid var(--border);position:relative}\n");
    W("#wv{display:block;width:100%;height:100%;cursor:grab}\n");
    W(".hcol{width:84px;flex-shrink:0;border-left:1px solid var(--border);position:relative}\n");
    W("#hc{display:block;width:100%;height:100%}\n");
    // Legend
    W(".legend{display:flex;gap:10px;align-items:center;padding:4px 12px;border-top:1px solid var(--border);font-family:'IBM Plex Mono',monospace;font-size:9px;color:var(--muted);flex-shrink:0;flex-wrap:wrap}\n");
    W(".leg{display:flex;align-items:center;gap:4px}\n");
    W(".lsw{width:11px;height:3px;border-radius:1px}\n");
    W(".lsw.sh{background:rgba(239,68,68,.7)}\n");
    W(".lsw.fl{background:rgba(96,165,250,.7)}\n");
    W(".lsw.nt{background:rgba(255,255,255,.28)}\n");
    W(".lsw.pt{background:#d0d0d0}\n");
    W(".lsw.sc{background:rgba(59,130,246,.55)}\n");
    W(".hint{margin-left:auto;opacity:.4;font-size:9px}\n");
    W(".note-acc{display:flex;gap:4px;padding:4px 10px;border-bottom:1px solid var(--border);flex-shrink:0;flex-wrap:wrap;align-items:center;min-height:22px}\n");
    W(".na{padding:1px 6px;border-radius:2px;border:1px solid;font-family:'IBM Plex Mono',monospace;font-size:9px}\n");
    W("</style></head><body>\n");

    // ── HTML ─────────────────────────────────
    W("<div class=\"app\">\n");

    // Header
    W("<div class=\"hdr\">\n");
    W("  <div class=\"hdr-title\">TakeAnalyzer <span>/ pitch report</span></div>\n");
    W("  <div class=\"hdr-meta\" id=\"hdr-meta\"></div>\n");
    if (!scale.name.empty()) { W("  <div class=\"badge\">key: "); W(scale.name); W("</div>\n"); }
    if (bpm > 0) {
        std::ostringstream bs; bs << std::fixed << std::setprecision(1) << bpm;
        W("  <div class=\"badge\">"); W(bs.str()); W(" bpm</div>\n");
    }
    W("  <button id=\"tog-btn\" class=\"exp-btn\">scores</button>\n");
    W("</div>\n");

    // Body
    W("<div class=\"body\">\n");

    // Left panel
    W("<div class=\"lpanel\">\n");
    W("  <div class=\"ph\"><span class=\"lbl\">Scores</span>"
        "<button class=\"exp-btn\" id=\"exp-btn\">Export TSV</button></div>\n");
    W("  <div class=\"tw\"><table>\n<thead><tr>\n");
    W("    <th></th><th>Take</th>\n");
    W("    <th>Acc</th><th>+/-c</th>\n");
    W("    <th>Stab</th><th>Voiced</th>\n");
    W("  </tr></thead>\n<tbody id=\"stbody\"></tbody>\n</table></div>\n</div>\n");

    // Right panel
    W("<div class=\"rpanel\">\n");
    W("  <div class=\"toolbar\">\n");
    W("    <div class=\"tabs\" id=\"tabs\"></div>\n");
    W("    <div class=\"sbar\">\n");
    W("      <div class=\"si\"><span class=\"l\">acc</span><span class=\"v\" id=\"si-acc\"></span></div>\n");
    W("      <div class=\"si\"><span class=\"l\">+/-c</span><span class=\"v\" id=\"si-cents\"></span></div>\n");
    W("      <div class=\"si\"><span class=\"l\">stab</span><span class=\"v\" id=\"si-stab\"></span></div>\n");
    W("      <div class=\"si\"><span class=\"l\">voiced</span><span class=\"v\" id=\"si-voiced\"></span></div>\n");
    W("      <div class=\"si\"><span class=\"l\">dur</span><span class=\"v\" id=\"si-dur\"></span></div>\n");
    W("    </div>\n  </div>\n");
    W("  <div id=\"note-acc\" class=\"note-acc\"></div>\n");
    W("  <div class=\"chart-area\">\n");
    W("    <div class=\"ccol\">\n");
    W("      <div class=\"cwrap\"><canvas id=\"cv\"></canvas></div>\n");
    W("      <div class=\"wstrip\"><canvas id=\"wv\"></canvas></div>\n");
    W("    </div>\n");
    W("    <div class=\"hcol\"><canvas id=\"hc\"></canvas></div>\n");
    W("  </div>\n");
    W("  <div class=\"legend\">\n");
    W("    <span class=\"leg\"><span class=\"lsw sh\"></span>sharp</span>\n");
    W("    <span class=\"leg\"><span class=\"lsw fl\"></span>flat</span>\n");
    W("    <span class=\"leg\"><span class=\"lsw nt\"></span>target note</span>\n");
    W("    <span class=\"leg\"><span class=\"lsw pt\"></span>detected pitch</span>\n");
    if (!scale.name.empty()) W("    <span class=\"leg\"><span class=\"lsw sc\"></span>scale note</span>\n");
    if (bpm > 0) W("    <span class=\"leg\"><span class=\"lsw nt\"></span>beat/bar</span>\n");
    W("    <span class=\"hint\">scroll=zoom H &nbsp;|&nbsp; shift+scroll=zoom V &nbsp;|&nbsp; drag chart=pan &nbsp;|&nbsp; drag overview=pan &nbsp;|&nbsp; dbl-click=reset &nbsp;|&nbsp; &larr;&rarr;=take &nbsp;|&nbsp; R=reset</span>\n");
    W("  </div>\n</div>\n</div>\n</div>\n");

    // ── Script ───────────────────────────────
    W("<script>\n");
    W("const BPM="); html << std::fixed << std::setprecision(2) << bpm;
    W(";\nconst SCALE="); html << scaleJ.str();
    W(";\nconst TAKES="); html << jss.str();
    W(";\n");

    W(R"JS(
const f1=v=>v.toFixed(1), f2=v=>v.toFixed(2);
const N12=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
function midiToName(m){const n=Math.round(m);return N12[((n%12)+12)%12]+(Math.floor(n/12)-1);}
function fmtDur(s){const m=Math.floor(s/60),sec=(s%60).toFixed(1);return m>0?`${m}m ${sec}s`:`${sec}s`;}
function nearestNote(midi){
  // Search +/-6 semitones to handle gapped scales (pentatonic gaps can be
  // 3 semitones; +/-6 covers any 7-note-or-fewer scale comfortably).
  const center=Math.round(midi);
  let bestDist=Infinity,bestNote=center;
  for(let c=center-6;c<=center+6;c++){
    if(SCALE.length&&!SCALE.includes(((c%12)+12)%12))continue;
    const dist=Math.abs(midi-c);
    if(dist<bestDist){bestDist=dist;bestNote=c;}
  }
  return bestNote;
}

// Init
const sorted=[...TAKES].sort((a,b)=>b.pitchAccuracyScore-a.pitchAccuracyScore);
const winner=sorted[0];
document.getElementById('hdr-meta').textContent=`${TAKES.length} takes \u00b7 ${new Date().toLocaleDateString('en-US',{month:'short',day:'numeric',year:'numeric'})}`;

// Score table
const tbody=document.getElementById('stbody');
sorted.forEach((t,rank)=>{
  const tr=document.createElement('tr');
  tr.innerHTML=`
    <td><span class="rnk ${rank===0?'r1':''}">${rank+1}</span></td>
    <td class="nc">${t.name}</td>
    <td class="${t.pitchAccuracyScore>=70?'g':t.pitchAccuracyScore<45?'b':''}">${f1(t.pitchAccuracyScore)}</td>
    <td>+/-${f1(t.avgCentsOff)}c</td>
    <td>${f1(t.stabilityScore)}</td>
    <td>${(t.voicedRatio*100).toFixed(0)}%</td>
  `;
  tr.addEventListener('click',()=>switchTake(TAKES.indexOf(t)));
  tbody.appendChild(tr);
});

// Export TSV
document.getElementById('exp-btn').addEventListener('click',()=>{
  const h=['Rank','Take','Accuracy','+/-Cents','Stability','Voiced%','Duration'];
  const rows=sorted.map((t,i)=>[i+1,t.name,f1(t.pitchAccuracyScore),
    f1(t.avgCentsOff),f1(t.stabilityScore),(t.voicedRatio*100).toFixed(0),fmtDur(t.duration)]);
  const tsv=[h,...rows].map(r=>r.join('\t')).join('\n');
  navigator.clipboard.writeText(tsv).then(()=>{
    const btn=document.getElementById('exp-btn');
    btn.textContent='Copied!';
    setTimeout(()=>btn.textContent='Export TSV',1500);
  });
});

// Viewports
const vps={};
function initVP(ti){
  if(vps[ti])return;
  const t=TAKES[ti];
  const v=t.frames.filter(f=>f.v).map(f=>f.midi);
  const mn=v.length?Math.min(...v):55,mx=v.length?Math.max(...v):75;
  vps[ti]={t0:0,t1:t.duration,midiMid:(mn+mx)/2,midiRange:Math.max(mx-mn+4,8)};
}
TAKES.forEach((_,ti)=>initVP(ti));

// Canvases
const cv=document.getElementById('cv'), ctx=cv.getContext('2d');
const wv=document.getElementById('wv'), wctx=wv.getContext('2d');
const hc=document.getElementById('hc'), hctx=hc.getContext('2d');
let ati=0;

function resizeAll(){
  [cv,wv,hc].forEach(c=>{
    const dpr=window.devicePixelRatio||1,r=c.getBoundingClientRect();
    c.width=r.width*dpr; c.height=r.height*dpr;
    c.getContext('2d').setTransform(dpr,0,0,dpr,0,0);
  });
  redraw();
}
let rafP=false;
function redraw(){if(!rafP){rafP=true;requestAnimationFrame(()=>{rafP=false;drawAll();});}}

// Stats
function updateStats(ti){
  const t=TAKES[ti];
  const voiced=t.frames.filter(f=>f.v);
  const avgHz=voiced.length?(voiced.reduce((s,f)=>s+f.hz,0)/voiced.length).toFixed(0):' - ';
  document.getElementById('si-acc').textContent=f1(t.pitchAccuracyScore);
  document.getElementById('si-cents').textContent='+/-'+f1(t.avgCentsOff)+'c';
  document.getElementById('si-stab').textContent=f1(t.stabilityScore);
  document.getElementById('si-voiced').textContent=(t.voicedRatio*100).toFixed(0)+'%';
  document.getElementById('si-dur').textContent=fmtDur(t.duration);
  // Note accuracy breakdown
  const naEl=document.getElementById('note-acc');
  if(t.noteAcc&&t.noteAcc.length){
    naEl.innerHTML=t.noteAcc.map(n=>{
      const avg=n.avg,sign=avg>=0?'+':'';
      const col=avg>5?'#ef4444':avg<-5?'#60a5fa':'#4ade80';
      return '<span class="na" style="color:'+col+';border-color:'+col+'55">'+N12[n.pc]+' '+sign+avg.toFixed(1)+'c</span>';
    }).join('');
  } else { naEl.innerHTML='<span style="font-family:IBM Plex Mono,monospace;font-size:9px;color:#333">no note data</span>'; }
  document.querySelectorAll('#stbody tr').forEach((tr,i)=>{
    tr.classList.toggle('arow',TAKES.indexOf(sorted[i])===ti);
  });
}

// Tabs
const tabsEl=document.getElementById('tabs');
TAKES.forEach((t,ti)=>{
  const btn=document.createElement('button');
  btn.className='tab'+(ti===0?' active':'');
  btn.textContent=t.name;
  btn.addEventListener('click',()=>switchTake(ti));
  tabsEl.appendChild(btn);
});

function switchTake(ti){
  ati=ti;
  document.querySelectorAll('.tab').forEach((b,i)=>b.classList.toggle('active',i===ti));
  updateStats(ti);
  redraw();
}
updateStats(0);

// -- State variables ------------------------------------------
let hovBin=null;
let seekTime=null;
let cvHover=null;

// -- Draw pitch chart -----------------------------------------
const P={L:46,R:14,T:12,B:20};

function drawPitch(){
  const t=TAKES[ati],vp=vps[ati];
  const CW=cv.offsetWidth,CH=cv.offsetHeight;
  const W=CW-P.L-P.R,H=CH-P.T-P.B;
  const tX=time=>P.L+(time-vp.t0)/(vp.t1-vp.t0)*W;
  const mY=midi=>P.T+H-(midi-(vp.midiMid-vp.midiRange/2))/vp.midiRange*H;

  ctx.fillStyle='#0c0c0c';
  ctx.fillRect(0,0,CW,CH);
  ctx.save(); ctx.beginPath(); ctx.rect(P.L,P.T,W,H); ctx.clip();

  const mLo=vp.midiMid-vp.midiRange/2,mHi=vp.midiMid+vp.midiRange/2;

  // Grid
  for(let m=Math.floor(mLo);m<=Math.ceil(mHi);m++){
    const y=mY(m),isC=m%12===0;
    const inSc=SCALE.length&&SCALE.includes(((m%12)+12)%12);
    ctx.strokeStyle=isC?'#1e1e1e':inSc?'rgba(59,130,246,0.10)':'#141414';
    ctx.lineWidth=isC?1:0.5;
    ctx.beginPath();ctx.moveTo(P.L,y);ctx.lineTo(P.L+W,y);ctx.stroke();
  }

  // Beat lines
  if(BPM>0){
    const spb=60/BPM,spbar=spb*4;
    let bt=Math.ceil(vp.t0/spb)*spb;
    while(bt<=vp.t1){
      const x=tX(bt),isBar=(bt%spbar)<spb*0.02;
      ctx.strokeStyle=isBar?'rgba(255,255,255,0.07)':'rgba(255,255,255,0.025)';
      ctx.lineWidth=isBar?1:0.5;
      ctx.setLineDash(isBar?[]:[3,4]);
      ctx.beginPath();ctx.moveTo(x,P.T);ctx.lineTo(x,P.T+H);ctx.stroke();
      ctx.setLineDash([]);
      bt+=spb;
    }
  }

  // Sharp/flat polygon fills.
  // Color is determined purely by geometry: is the pitch line ABOVE the note
  // line (sharp = red) or BELOW it (flat = blue)?  In canvas coordinates,
  // smaller Y = higher pitch, so pitch-above-note means py < ny.
  // Using geometry instead of the arithmetic sign of (midi - nearestNote)
  // prevents color flips at semitone boundaries where the same continuous pitch
  // movement crosses from "sharp of note A" to "flat of note B" mid-segment.
  const sharpColor='rgba(239,68,68,0.42)', flatColor='rgba(96,165,250,0.42)';
  const fillColor=(pitchY,noteY)=>pitchY<noteY?sharpColor:flatColor;
  const frames=t.frames;
  for(let i=0;i<frames.length-1;i++){
    const fr0=frames[i],fr1=frames[i+1];
    if(!fr0.v||!fr1.v)continue;
    if(fr1.t-fr0.t>0.15)continue; // skip gap-filled seams
    const n0=nearestNote(fr0.midi),n1=nearestNote(fr1.midi);
    const c0=(fr0.midi-n0)*100,c1=(fr1.midi-n1)*100;
    if(Math.abs(c0)<1&&Math.abs(c1)<1)continue;
    const x0=tX(fr0.t),x1=tX(fr1.t);
    const py0=mY(fr0.midi),py1=mY(fr1.midi);
    if(n0===n1){
      const ny=mY(n0);
      if(Math.abs(py0-ny)<0.5&&Math.abs(py1-ny)<0.5)continue;
      const sharp0=py0<ny, sharp1=py1<ny;
      if(sharp0===sharp1){
        // Same side  -  simple non-crossing trapezoid
        ctx.fillStyle=fillColor(py0,ny);
        ctx.beginPath();
        ctx.moveTo(x0,py0);ctx.lineTo(x1,py1);
        ctx.lineTo(x1,ny);ctx.lineTo(x0,ny);
        ctx.closePath();ctx.fill();
      } else {
        // Pitch crosses through the note line  -  drawing a single trapezoid
        // produces a self-intersecting (bowtie) shape and wrong color.
        // Split at the crossing point and draw two triangles instead.
        const frac=Math.max(0,Math.min(1,(n0-fr0.midi)/(fr1.midi-fr0.midi)));
        const xC=x0+frac*(x1-x0);
        // First triangle: from start to crossing
        ctx.fillStyle=fillColor(py0,ny);
        ctx.beginPath();ctx.moveTo(x0,py0);ctx.lineTo(xC,ny);ctx.lineTo(x0,ny);ctx.closePath();ctx.fill();
        // Second triangle: from crossing to end
        ctx.fillStyle=fillColor(py1,ny);
        ctx.beginPath();ctx.moveTo(xC,ny);ctx.lineTo(x1,py1);ctx.lineTo(x1,ny);ctx.closePath();ctx.fill();
      }
    } else {
      // Note changes mid-segment  -  split at boundary, flat bottom for each half
      const denom=(fr1.midi-fr0.midi)-(n1-n0);
      const frac=Math.abs(denom)>0.01?Math.max(0,Math.min(1,(n0-fr0.midi)/denom)):0.5;
      const xC=x0+frac*(x1-x0);
      const pyC=py0+frac*(py1-py0);
      // First half: pitch vs n0
      if(Math.abs(c0)>=1){
        const ny0=mY(n0);
        ctx.fillStyle=fillColor(py0, ny0);
        ctx.beginPath();
        ctx.moveTo(x0,py0);ctx.lineTo(xC,pyC);
        ctx.lineTo(xC,ny0);ctx.lineTo(x0,ny0);
        ctx.closePath();ctx.fill();
      }
      // Second half: pitch vs n1
      if(Math.abs(c1)>=1){
        const ny1=mY(n1);
        ctx.fillStyle=fillColor(py1, ny1);
        ctx.beginPath();
        ctx.moveTo(xC,pyC);ctx.lineTo(x1,py1);
        ctx.lineTo(x1,ny1);ctx.lineTo(xC,ny1);
        ctx.closePath();ctx.fill();
      }
    }
  }

)JS");
    W(R"JS(
  // Target note lines
  ctx.strokeStyle='rgba(255,255,255,0.22)';ctx.lineWidth=1;ctx.setLineDash([5,5]);
  let rn=-999,rx0=0,rx1=0;
  const flR=()=>{if(rn===-999)return;const y=mY(rn);if(y>=P.T&&y<=P.T+H){ctx.beginPath();ctx.moveTo(rx0,y);ctx.lineTo(rx1,y);ctx.stroke();}rn=-999;};
  for(const f of frames){
    if(!f.v||f.t<vp.t0-.2||f.t>vp.t1+.2){flR();continue;}
    const n=nearestNote(f.midi),x=tX(f.t);
    if(n!==rn){flR();rn=n;rx0=x;}rx1=x;
  }
  flR();
  ctx.setLineDash([]);

  // Pitch line
  ctx.strokeStyle='#d0d0d0';ctx.lineWidth=1.5;ctx.lineJoin='round';
  let open=false;ctx.beginPath();
  for(const f of frames){
    if(!f.v||f.t<vp.t0-.05||f.t>vp.t1+.05){open=false;continue;}
    const x=tX(f.t),y=mY(f.midi);
    if(!open){ctx.moveTo(x,y);open=true;}else ctx.lineTo(x,y);
  }
  ctx.stroke();

  // Histogram hover highlight: draw bright dots on frames in the hovered cent bin
  if(hovBin!==null){
    const BINS=100,MID=50;
    ctx.fillStyle='rgba(255,255,255,0.9)';
    for(const f of frames){
      if(!f.v||f.t<vp.t0-.05||f.t>vp.t1+.05)continue;
      const target=nearestNote(f.midi);
      const bin=Math.max(0,Math.min(BINS-1,Math.floor((f.midi-target)*100)+MID));
      if(bin!==hovBin)continue;
      const x=tX(f.t),y=mY(f.midi);
      ctx.beginPath();ctx.arc(x,y,2.5,0,Math.PI*2);ctx.fill();
    }
  }

  // Seek cursor
  if(seekTime!==null){
    const sx=tX(seekTime);
    if(sx>=P.L&&sx<=P.L+W){
      ctx.strokeStyle='rgba(255,210,40,0.8)';ctx.lineWidth=1.5;ctx.setLineDash([4,3]);
      ctx.beginPath();ctx.moveTo(sx,P.T);ctx.lineTo(sx,P.T+H);ctx.stroke();
      ctx.setLineDash([]);
    }
  }

  ctx.restore();

  // Hover tooltip (outside clip so it never gets cut off)
  if(cvHover){
    const {px,py,noteName,cents,time}=cvHover;
    const sign=cents>=0?'+':'';
    const line1=noteName+' '+sign+cents.toFixed(1)+'c';
    const line2=BPM>0?(time/(60/BPM)).toFixed(2)+'b':time.toFixed(3)+'s';
    ctx.font='10px IBM Plex Mono,monospace';
    const tw=Math.max(ctx.measureText(line1).width,ctx.measureText(line2).width)+14;
    let tx=px+12,ty=py-30;
    if(tx+tw>CW-4)tx=px-tw-12;
    if(ty<4)ty=py+6;
    ctx.fillStyle='rgba(18,18,18,0.93)';
    ctx.fillRect(tx,ty,tw,28);
    ctx.strokeStyle='rgba(255,255,255,0.12)';ctx.lineWidth=1;
    ctx.strokeRect(tx,ty,tw,28);
    const col=cents>5?'#ef4444':cents<-5?'#60a5fa':'#4ade80';
    ctx.fillStyle=col;ctx.textAlign='left';
    ctx.fillText(line1,tx+7,ty+11);
    ctx.fillStyle='#555';
    ctx.fillText(line2,tx+7,ty+23);
  }

  // Y axis
  ctx.font='9px IBM Plex Mono,monospace';ctx.textAlign='right';
  for(let m=Math.floor(mLo);m<=Math.ceil(mHi);m++){
    const y=mY(m);if(y<P.T||y>P.T+H)continue;
    const isC=m%12===0,inSc=SCALE.length&&SCALE.includes(((m%12)+12)%12);
    ctx.fillStyle=isC?'#555':inSc?'rgba(96,165,250,0.65)':'#2a2a2a';
    ctx.fillText(N12[((m%12)+12)%12]+(Math.floor(m/12)-1),P.L-5,y+3.5);
    ctx.strokeStyle=isC?'#2a2a2a':'#1a1a1a';ctx.lineWidth=1;
    ctx.beginPath();ctx.moveTo(P.L-3,y);ctx.lineTo(P.L,y);ctx.stroke();
  }

  // X axis
  ctx.fillStyle='#2e2e2e';ctx.textAlign='center';
  const nL=Math.min(10,Math.max(4,Math.floor(W/70)));
  const vis=vp.t1-vp.t0;
  for(let i=0;i<=nL;i++){
    const frac=i/nL,time=vp.t0+frac*vis,x=P.L+frac*W;
    ctx.fillText(BPM>0?(time/(60/BPM)).toFixed(1)+'b':time.toFixed(1)+'s',x,P.T+H+15);
  }

  // Cents scale on right margin
  const semH=H/vp.midiRange;
  if(semH>14){
    ctx.fillStyle='#252525';ctx.textAlign='left';ctx.font='8px IBM Plex Mono,monospace';
    const ref=Math.round(vp.midiMid);
    for(const c of[-50,-25,0,25,50]){
      const y=mY(ref+c/100);if(y<P.T||y>P.T+H)continue;
      ctx.fillText((c>=0?'+':'')+c+'c',P.L+W+3,y+3);
    }
  }
}

// -- Waveform strip --------------------------------------------
function drawWave(){
  const t=TAKES[ati],vp=vps[ati];
  const CW=wv.offsetWidth,CH=wv.offsetHeight;
  const W=CW-P.L-P.R;
  wctx.fillStyle='#0c0c0c';wctx.fillRect(0,0,CW,CH);

  TAKES.forEach((tk,tki)=>{
    const peaks=tk.wave;if(!peaks||!peaks.length)return;
    const isA=tki===ati;
    wctx.fillStyle=isA?'rgba(96,165,250,0.5)':'rgba(255,255,255,0.05)';
    for(let i=0;i<W;i++){
      const frac=i/W,pi=Math.floor(frac*peaks.length);
      const pk=peaks[Math.min(pi,peaks.length-1)];
      const bH=Math.max(1,pk*(CH-4));
      wctx.fillRect(P.L+i,(CH-bH)/2,1,bH);
    }
  });

  // Viewport indicator
  const vs=vps[ati].t0/t.duration,ve=vps[ati].t1/t.duration;
  wctx.fillStyle='rgba(255,255,255,0.05)';
  wctx.fillRect(P.L+vs*W,0,(ve-vs)*W,CH);
  wctx.strokeStyle='rgba(255,255,255,0.18)';wctx.lineWidth=1;
  wctx.strokeRect(P.L+vs*W,0,(ve-vs)*W,CH);

  // Time labels
  wctx.fillStyle='#252525';wctx.font='8px IBM Plex Mono,monospace';wctx.textAlign='center';
  const steps=Math.min(8,Math.floor(W/60));
  for(let i=0;i<=steps;i++){
    const time=(i/steps)*t.duration,x=P.L+(i/steps)*W;
    wctx.fillText(BPM>0?(time/(60/BPM)).toFixed(0)+'b':time.toFixed(1)+'s',x,CH-2);
  }
  // Seek cursor on waveform (absolute position in the full-duration overview)
  if(seekTime!==null){
    const t2=TAKES[ati];
    const W2=wv.offsetWidth-P.L-P.R;
    const frac2=t2.duration>0?seekTime/t2.duration:0;
    if(frac2>=0&&frac2<=1){
      const sx=P.L+frac2*W2;
      wctx.strokeStyle='rgba(255,210,40,0.8)';wctx.lineWidth=1.5;
      wctx.beginPath();wctx.moveTo(sx,0);wctx.lineTo(sx,wv.offsetHeight);wctx.stroke();
    }
  }
}

// -- Histogram -------------------------------------------------
function drawHistogram(){
  const t=TAKES[ati];
  const CW=hc.offsetWidth,CH=hc.offsetHeight;
  // 100 bins = \u00b150 cents, one bin per cent.
  // bin 0 = -50c (most flat), bin 50 = 0c, bin 99 = +49c (most sharp).
  // Using Math.floor avoids the half-cent rounding bias that Math.round
  // introduces (0.5 always rounds up = slightly sharp bias).
  const BINS=100,MID=50;
  hctx.fillStyle='#0c0c0c';hctx.fillRect(0,0,CW,CH);
  const counts=new Array(BINS).fill(0);
  for(const f of t.frames){
    if(!f.v)continue;
    const target=nearestNote(f.midi);
    const cents=Math.floor((f.midi-target)*100);
    const bin=Math.max(0,Math.min(BINS-1,cents+MID));
    counts[bin]++;
  }
  const max=Math.max(1,...counts);
  const bH=CH/BINS;
  // Draw sharp (positive cents) at TOP, flat (negative) at BOTTOM.
  // bin index i has cents = i - MID. We flip by drawing at y=(BINS-1-i)*bH
  // so that the sharpest bin (i=BINS-1) is at y=0 (top).
  for(let i=0;i<BINS;i++){
    const cents=i-MID,frac=counts[i]/max,bW=frac*(CW-18);
    const hov=hovBin===i;
    const alpha=hov?1.0:(0.25+frac*0.6);
    hctx.fillStyle=cents>0?`rgba(239,68,68,${alpha})`:
                   cents<0?`rgba(96,165,250,${alpha})`:
                           `rgba(255,255,255,${alpha})`;
    const rowY=(BINS-1-i)*bH;
    hctx.fillRect(0,rowY,hov?CW-18:bW,Math.max(1,bH-.5));
    if(hov){
      hctx.strokeStyle='rgba(255,255,255,0.7)';
      hctx.lineWidth=1;
      hctx.strokeRect(0,rowY,CW-18,Math.max(1,bH-.5));
    }
  }
  // Zero line  -  sits between bin MID-1 (-1c) and bin MID (0c).
  // After flip, bin MID draws at (BINS-1-MID)*bH = (BINS-1-MID)*bH.
  const midY=(BINS-1-MID)*bH;
  hctx.strokeStyle='rgba(255,255,255,0.12)';hctx.lineWidth=1;
  hctx.beginPath();hctx.moveTo(0,midY);hctx.lineTo(CW,midY);hctx.stroke();
  // Labels: after flip, cents c is at y=(BINS-1-(c+MID))*bH=(MID-1-c)*bH
  hctx.fillStyle='#2e2e2e';hctx.font='8px IBM Plex Mono,monospace';hctx.textAlign='right';
  for(const c of[-40,-20,0,20,40]){
    const y=(MID-1-c)*bH;if(y<0||y>CH)continue;
    hctx.fillText((c>=0?'+':'')+c+'c',CW-1,y+3);
  }
}

function drawAll(){drawPitch();drawWave();drawHistogram();}

// -- Histogram hover -------------------------------------------
hc.addEventListener('mousemove',e=>{
  const BINS=100;
  const bH=hc.offsetHeight/BINS;
  const r=hc.getBoundingClientRect();
  const mouseY=e.clientY-r.top;
  // bin i is drawn at y=(BINS-1-i)*bH, so row at mouseY has i=BINS-1-floor(mouseY/bH)
  const i=BINS-1-Math.floor(mouseY/bH);
  const bin=Math.max(0,Math.min(BINS-1,i));
  if(bin!==hovBin){hovBin=bin;redraw();}
});
hc.addEventListener('mouseleave',()=>{
  if(hovBin!==null){hovBin=null;redraw();}
});

// -- Pitch chart hover tooltip --------------------------------
cv.addEventListener('mousemove',e=>{
  if(drag)return;
  const t=TAKES[ati],vp=vps[ati];
  const W=cv.offsetWidth-P.L-P.R,H=cv.offsetHeight-P.T-P.B;
  const r=cv.getBoundingClientRect();
  const mx=e.clientX-r.left,my=e.clientY-r.top;
  if(mx<P.L||mx>P.L+W||my<P.T||my>P.T+H){
    if(cvHover){cvHover=null;redraw();}return;
  }
  const time=vp.t0+(mx-P.L)/W*(vp.t1-vp.t0);
  let best=null,bestDt=1e9;
  for(const f of t.frames){
    if(!f.v)continue;
    const dt=Math.abs(f.t-time);
    if(dt<bestDt){bestDt=dt;best=f;}
  }
  if(!best||bestDt>0.15){if(cvHover){cvHover=null;redraw();}return;}
  const target=nearestNote(best.midi);
  const cents=(best.midi-target)*100;
  const noteName=midiToName(Math.round(target));
  cvHover={px:mx,py:my,noteName,cents,time:best.t};
  redraw();
});
cv.addEventListener('mouseleave',()=>{if(cvHover){cvHover=null;redraw();}});


// -- Waveform click to seek ------------------------------------
// -- Waveform strip drag-to-pan --------------------------------
let wvDrag=null;
wv.style.cursor='grab';
wv.addEventListener('mousedown',e=>{
  if(e.button!==0)return;
  const vp=vps[ati];
  wvDrag={x:e.clientX,t0:vp.t0,t1:vp.t1};
  wv.style.cursor='grabbing';
  e.preventDefault();
});
window.addEventListener('mousemove',e=>{
  if(!wvDrag)return;
  const vp=vps[ati],t=TAKES[ati];
  const W=wv.offsetWidth-P.L-P.R;
  const dx=e.clientX-wvDrag.x;
  // Dragging right pans right (later in time); dragging left pans left (earlier in time)
  const dT=(dx/W)*t.duration;
  const dur=wvDrag.t1-wvDrag.t0;
  const t0=Math.max(0,Math.min(t.duration-dur,wvDrag.t0+dT));
  vp.t0=t0;vp.t1=t0+dur;
  redraw();
});
window.addEventListener('mouseup',e=>{
  if(wvDrag){
    if(Math.abs(e.clientX-wvDrag.x)<5){
      // The waveform strip shows the full duration, so map click to absolute time
      const t=TAKES[ati];
      const W=wv.offsetWidth-P.L-P.R;
      const r=wv.getBoundingClientRect();
      const frac=Math.max(0,Math.min(1,(e.clientX-r.left-P.L)/W));
      seekTime=frac*t.duration;
    }
    wvDrag=null;wv.style.cursor='grab';redraw();
  }
});

// -- Mouse pan -------------------------------------------------
let drag=null;
cv.addEventListener('mousedown',e=>{
  if(e.button!==0)return;
  drag={x:e.clientX,y:e.clientY,vp:{...vps[ati]}};cv.classList.add('drag');
});
window.addEventListener('mousemove',e=>{
  if(!drag)return;
  const vp=vps[ati];
  const W=cv.offsetWidth-P.L-P.R,H=cv.offsetHeight-P.T-P.B;
  const dT=-(e.clientX-drag.x)/W*(drag.vp.t1-drag.vp.t0);
  const dM=(e.clientY-drag.y)/H*drag.vp.midiRange;
  const dur=drag.vp.t1-drag.vp.t0;
  const t0=Math.max(0,Math.min(TAKES[ati].duration-dur,drag.vp.t0+dT));
  vp.t0=t0;vp.t1=t0+dur;vp.midiMid=drag.vp.midiMid+dM;redraw();
});
window.addEventListener('mouseup',()=>{drag=null;cv.classList.remove('drag');});

// -- Scroll zoom -----------------------------------------------
cv.addEventListener('wheel',e=>{
  e.preventDefault();
  const vp=vps[ati];
  const W=cv.offsetWidth-P.L-P.R,H=cv.offsetHeight-P.T-P.B;
  const f=e.deltaY>0?1.10:1/1.10;
  const r=cv.getBoundingClientRect();
  const mx=e.clientX-r.left,my=e.clientY-r.top;
  if(e.shiftKey||e.ctrlKey){
    const aM=(vp.midiMid-vp.midiRange/2)+(1-(my-P.T)/H)*vp.midiRange;
    const nR=Math.max(0.8,Math.min(60,vp.midiRange*f));
    const af=(aM-(vp.midiMid-vp.midiRange/2))/vp.midiRange;
    vp.midiMid=aM-af*nR+nR/2;vp.midiRange=nR;
  } else {
    const aT=vp.t0+(mx-P.L)/W*(vp.t1-vp.t0);
    const dur=vp.t1-vp.t0;
    const nD=Math.max(0.1,Math.min(TAKES[ati].duration,dur*f));
    const af=(aT-vp.t0)/dur;
    const t0=Math.max(0,Math.min(TAKES[ati].duration-nD,aT-af*nD));
    vp.t0=t0;vp.t1=t0+nD;
  }
  redraw();
},{passive:false});

// -- Double-click reset ----------------------------------------
cv.addEventListener('dblclick',()=>{delete vps[ati];initVP(ati);redraw();});

// -- Keyboard --------------------------------------------------
window.addEventListener('keydown',e=>{
  if(e.target.tagName==='INPUT'||e.target.tagName==='TEXTAREA')return;
  if(e.key==='ArrowRight'){e.preventDefault();switchTake((ati+1)%TAKES.length);}
  else if(e.key==='ArrowLeft'){e.preventDefault();switchTake((ati-1+TAKES.length)%TAKES.length);}
  else if(e.key==='r'||e.key==='R'){delete vps[ati];initVP(ati);redraw();}
});

// -- Resize ----------------------------------------------------
new ResizeObserver(resizeAll).observe(document.querySelector('.app'));
document.getElementById('tog-btn').addEventListener('click',()=>{
  const lp=document.querySelector('.lpanel');
  const shown=lp.style.display!=='none';
  lp.style.display=shown?'none':'';
  document.getElementById('tog-btn').textContent=shown?'scores (hidden)':'scores';
  redraw();
});
resizeAll();
)JS");

    W("</script></body></html>\n");
    html.close();
    std::cout << "HTML report written to: " << outPath << "\n";
}

// ---------------------------------------------
//  CLI Summary
// ---------------------------------------------
void printSummary(const std::vector<TakeAnalysis>& takes) {
    std::vector<const TakeAnalysis*> ranked;
    for (auto& t : takes) ranked.push_back(&t);
    std::sort(ranked.begin(), ranked.end(),
        [](auto a, auto b) { return a->pitchAccuracyScore > b->pitchAccuracyScore; });
    std::cout << "\n+==========================================================+\n";
    std::cout << "|           T A K E   A N A L Y Z E R   R E S U L T S     |\n";
    std::cout << "+==========================================================+\n\n";
    for (int i = 0; i < (int)ranked.size(); ++i) {
        const auto& t = *ranked[i];
        std::cout << std::setw(2) << (i + 1) << ". " << std::left << std::setw(28) << t.name
            << "  acc=" << std::fixed << std::setprecision(1) << t.pitchAccuracyScore
            << "/100  " << (i == 0 ? " <- BEST" : "") << "\n";
        std::cout << "    Accuracy  : " << t.pitchAccuracyScore << "/100  (avg +-" << t.avgCentsOff << " cents)\n";
        std::cout << "    Stability : " << t.stabilityScore << "/100  (sigma=" << std::setprecision(2) << t.pitchStdDev << " st)\n";
        std::cout << "    Voiced    : " << std::setprecision(0) << t.voicedRatio * 100.f << "%\n\n";
    }
    std::cout << "  * BEST ACCURACY: " << ranked[0]->name << "\n\n";
}

// ---------------------------------------------
//  main
// ---------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: take_analyzer <file.wav> [...] [--out report.html] [--bpm 120] [--key Cmaj]\n";
        return 1;
    }
    std::vector<std::string> wavPaths;
    std::string outPath = "pitch_report.html", keyStr;
    float bpm = 0.f;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--out" && i + 1 < argc)      outPath = argv[++i];
        else if (arg == "--bpm" && i + 1 < argc) bpm = std::stof(argv[++i]);
        else if (arg == "--key" && i + 1 < argc) keyStr = argv[++i];
        else wavPaths.push_back(arg);
    }
    if (wavPaths.empty()) { std::cerr << "Error: no WAV files.\n"; return 1; }

    Scale scale = parseScale(keyStr);
    if (!scale.name.empty()) std::cout << "Key: " << scale.name << "\n";

    std::vector<TakeAnalysis> takes;
    bool bpmDetected = false;
    for (auto& p : wavPaths) {
        std::cout << "Loading: " << p << " ... ";
        WavFile wav = loadWav(p);
        if (!wav.valid) { std::cerr << "FAILED (" << wav.error << ")\n"; continue; }
        std::cout << (int)(wav.samples.size() / wav.sampleRate) << "s  "
            << wav.sampleRate << "Hz  " << wav.bitsPerSample << "-bit\n";
        // Auto-detect BPM from first file
        if (bpm <= 0.f && !bpmDetected) {
            std::cout << "Detecting BPM... "; std::cout.flush();
            float det = detectBPM(wav.samples, wav.sampleRate);
            if (det > 40.f && det < 250.f) {
                bpm = det; bpmDetected = true;
                std::cout << std::fixed << std::setprecision(1) << bpm << " bpm\n";
            }
            else { std::cout << "inconclusive\n"; }
        }
        std::cout << "Analyzing... "; std::cout.flush();
        takes.push_back(analyzeTake(wav, scale));
        std::cout << "done.\n";
    }
    if (takes.empty()) { std::cerr << "No valid files.\n"; return 1; }
    printSummary(takes);
    generateReport(takes, outPath, bpm, scale);
    return 0;
}
