#include "preset_dsl.h"
#include "wav_reader.h"
#include "dac_control.h"

#include <FFat.h>
#include <string.h>

namespace {
static constexpr size_t WAV_CACHE_MAX_SAMPLES = DAC_MAX_WAVE_SAMPLES;

// Частоты дискретизации, поддерживаемые DAC PCM5102A (см. SRS раздел 0 / R.7).
static bool isSupportedDacRate(int fs) {
    static const int rates[] = {8000, 16000, 22050, 32000, 44100, 48000, 96000, 192000, 384000};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (fs == rates[i]) return true;
    }
    return false;
}

static String trimCopy(const String& s) {
    String out = s;
    out.trim();
    return out;
}

static String stripComment(const String& s) {
    bool in_quotes = false;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"') in_quotes = !in_quotes;
        if (!in_quotes && c == '#') return s.substring(0, i);
    }
    return s;
}

static int leadingSpaces(const String& s) {
    int n = 0;
    while (n < (int)s.length() && s[n] == ' ') n++;
    return n;
}

static String unquote(const String& s) {
    String v = trimCopy(s);
    if (v.length() >= 2 && v[0] == '"' && v[v.length() - 1] == '"') {
        return v.substring(1, v.length() - 1);
    }
    return v;
}

static bool parseFloat(const String& s, float* out) {
    if (!out) return false;
    String v = trimCopy(s);
    if (v.isEmpty()) return false;
    char* endp = nullptr;
    float f = strtof(v.c_str(), &endp);
    if (!endp || *endp != '\0') return false;
    *out = f;
    return true;
}

static bool parseInt(const String& s, int* out) {
    if (!out) return false;
    String v = trimCopy(s);
    if (v.isEmpty()) return false;
    char* endp = nullptr;
    long x = strtol(v.c_str(), &endp, 10);
    if (!endp || *endp != '\0') return false;
    *out = (int)x;
    return true;
}

static bool isAsciiPresetId(const String& id) {
    if (id.isEmpty() || id.length() > 64) return false;
    for (size_t i = 0; i < id.length(); i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  (c == '_') || (c == '-');
        if (!ok) return false;
    }
    return true;
}

static bool endsWith(const String& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.length() >= n && s.substring(s.length() - n) == suffix;
}

static String baseNameNoExt(const String& path) {
    int slash = path.lastIndexOf('/');
    String name = (slash >= 0) ? path.substring(slash + 1) : path;
    int dot = name.lastIndexOf('.');
    if (dot > 0) return name.substring(0, dot);
    return name;
}

static void setParamField(ParamRange* p, const String& field, const String& value) {
    if (!p) return;
    float v = 0.0f;
    if (!parseFloat(value, &v)) return;
    if (field == "min") p->min_v = v;
    else if (field == "max") p->max_v = v;
    else if (field == "step") p->step = v;
    else if (field == "default") p->default_v = v;
}

static bool validateParam(const ParamRange& p) {
    if (p.step <= 0.0f) return false;
    if (p.min_v > p.max_v) return false;
    if (p.default_v < p.min_v || p.default_v > p.max_v) return false;
    return true;
}

static bool parseYamlPreset(File& f, const String& file_path, PresetDefinition* out, String* err) {
    if (!out || !err) return false;
    *out = PresetDefinition{};
    out->file_name = file_path;
    out->id = baseNameNoExt(file_path);
    out->name = out->id;

    std::vector<int> indents;
    std::vector<String> keys;

    auto popToIndent = [&](int indent) {
        while (!indents.empty() && indent <= indents.back()) {
            indents.pop_back();
            keys.pop_back();
        }
    };

    while (f.available()) {
        String raw = f.readStringUntil('\n');
        raw.replace("\r", "");
        String line = trimCopy(stripComment(raw));
        if (line.isEmpty()) continue;

        int indent = leadingSpaces(raw);
        String s = trimCopy(stripComment(raw));
        if (s.isEmpty()) continue;

        int colon = s.indexOf(':');
        if (colon <= 0) continue;
        String key = trimCopy(s.substring(0, colon));
        String value = trimCopy(s.substring(colon + 1));

        popToIndent(indent);
        if (value.isEmpty()) {
            indents.push_back(indent);
            keys.push_back(key);
            continue;
        }

        String full;
        for (size_t i = 0; i < keys.size(); i++) {
            if (!full.isEmpty()) full += ".";
            full += keys[i];
        }
        if (!full.isEmpty()) full += ".";
        full += key;

        if (full == "dsl_version") {
            int dsl = 0;
            if (!parseInt(value, &dsl) || dsl != 1) {
                *err = "dsl_version must be 1";
                return false;
            }
        } else if (full == "name") {
            out->name = unquote(value);
        } else if (full == "type") {
            String t = unquote(value);
            if (t == "CONST") out->type = PresetType::CONST_DC;
            else if (t == "SIN") out->type = PresetType::SIN;
            else if (t == "WAV") out->type = PresetType::WAV;
            else {
                *err = "unknown type: " + t;
                return false;
            }
        } else if (full == "channels.mode") {
            String m = unquote(value);
            if (m == "left") out->channels_both = false;
            else if (m == "both") out->channels_both = true;
            else {
                *err = "channels.mode must be left|both";
                return false;
            }
        } else if (full == "feedback.amp_estimation_base") {
            String b = unquote(value);
            if (b == "MEAN") out->feedback_base = FeedbackBase::MEAN;
            else if (b == "STD") out->feedback_base = FeedbackBase::STD;
            else {
                *err = "feedback.amp_estimation_base must be MEAN|STD";
                return false;
            }
        } else if (full == "feedback.amp_estimation_coeff") {
            float c = 0.0f;
            if (!parseFloat(value, &c) || c <= 0.0f) {
                *err = "feedback.amp_estimation_coeff must be > 0";
                return false;
            }
            out->feedback_coeff = c;
        } else if (full == "scope.sync_mode") {
            String sm = unquote(value);
            if (sm == "two_periods") out->scope_sync = ScopeSyncMode::TWO_PERIODS;
            else if (sm == "one_period") out->scope_sync = ScopeSyncMode::ONE_PERIOD;
            else if (sm == "no_sync") out->scope_sync = ScopeSyncMode::NO_SYNC;
            else {
                *err = "scope.sync_mode invalid";
                return false;
            }
        } else if (full == "wave_file") {
            out->wave_file = unquote(value);
        } else if (full == "sample_rate_hz.value") {
            int sr = 0;
            if (!parseInt(value, &sr) || sr <= 0) {
                *err = "sample_rate_hz.value must be positive int";
                return false;
            }
            out->sample_rate_hz = sr;
        } else if (full.startsWith("params.amplitude_mA.")) {
            setParamField(&out->amplitude_mA, full.substring(String("params.amplitude_mA.").length()), value);
            out->amplitude_mA.valid = true;
        } else if (full.startsWith("params.frequency_hz.")) {
            setParamField(&out->frequency_hz, full.substring(String("params.frequency_hz.").length()), value);
            out->frequency_hz.valid = true;
        } else if (full.startsWith("params.fade_in_sec.")) {
            setParamField(&out->fade_in_sec, full.substring(String("params.fade_in_sec.").length()), value);
            out->fade_in_sec.valid = true;
        } else if (full.startsWith("params.fade_out_sec.")) {
            setParamField(&out->fade_out_sec, full.substring(String("params.fade_out_sec.").length()), value);
            out->fade_out_sec.valid = true;
        } else if (full.startsWith("params.duration_min.")) {
            setParamField(&out->duration_min, full.substring(String("params.duration_min.").length()), value);
            out->duration_min.valid = true;
        }
    }

    if (!isAsciiPresetId(out->id)) {
        *err = "invalid filename (ASCII [a-z0-9_-], len<=64)";
        return false;
    }
    if (out->name.isEmpty()) {
        *err = "name is required";
        return false;
    }
    if (!out->amplitude_mA.valid || !validateParam(out->amplitude_mA)) {
        *err = "params.amplitude_mA is missing/invalid";
        return false;
    }
    if (!out->fade_in_sec.valid || !validateParam(out->fade_in_sec)) {
        *err = "params.fade_in_sec is missing/invalid";
        return false;
    }
    if (!out->fade_out_sec.valid || !validateParam(out->fade_out_sec)) {
        *err = "params.fade_out_sec is missing/invalid";
        return false;
    }
    if (!out->duration_min.valid || !validateParam(out->duration_min)) {
        *err = "params.duration_min is missing/invalid";
        return false;
    }

    if (out->type == PresetType::CONST_DC || out->type == PresetType::SIN) {
        if (out->sample_rate_hz <= 0) {
            *err = "sample_rate_hz.value required for CONST/SIN";
            return false;
        }
        if (!isSupportedDacRate(out->sample_rate_hz)) {
            *err = "sample_rate_hz not supported by DAC (PCM5102A)";
            return false;
        }
    }
    if (out->type == PresetType::SIN) {
        if (!out->frequency_hz.valid || !validateParam(out->frequency_hz)) {
            *err = "params.frequency_hz is required for SIN";
            return false;
        }
        // O.12: период строится целочисленно при текущем Fs и обязан укладываться
        // в лимит буфера, без фазовых разрывов (Nyquist).
        const float fs = (float)out->sample_rate_hz;
        if (out->frequency_hz.max_v >= fs / 4.0f) {
            *err = "frequency_hz.max too high for Fs (period < 4 samples)";
            return false;
        }
        if (out->frequency_hz.min_v <= fs / (float)DAC_MAX_WAVE_SAMPLES) {
            *err = "frequency_hz.min too low for Fs (period > wave buffer)";
            return false;
        }
    }
    if (out->type == PresetType::WAV) {
        if (out->wave_file.isEmpty()) {
            *err = "wave_file required for WAV";
            return false;
        }
    }
    if (out->type == PresetType::CONST_DC && out->scope_sync != ScopeSyncMode::NONE) {
        *err = "scope.sync_mode forbidden for CONST";
        return false;
    }
    return true;
}

} // namespace

std::vector<PresetDefinition> PresetDsl::scanAll() {
    std::vector<PresetDefinition> out;
    std::vector<String> errors;

    if (!FFat.begin(true, "/ffat", 10, "ffat")) {
        return out;
    }

    File root = FFat.open("/");
    if (!root || !root.isDirectory()) {
        FFat.end();
        return out;
    }

    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (f.isDirectory()) continue;
        String path = String("/") + f.name();
        if (!endsWith(path, ".yaml")) continue;

        PresetDefinition p{};
        String err;
        if (!parseYamlPreset(f, path, &p, &err)) {
            errors.push_back(path + ": " + err);
            f.close();
            continue;
        }

        if (p.type == PresetType::WAV) {
            String wav_path = String("/") + p.wave_file;
            WavInfo wi{};
            size_t got = 0;
            p.wav_left_samples.resize(WAV_CACHE_MAX_SAMPLES);
            p.wav_right_samples.resize(WAV_CACHE_MAX_SAMPLES);
            if (!WavReader::readFrames(wav_path.c_str(),
                                       p.wav_left_samples.data(),
                                       p.wav_right_samples.data(),
                                       WAV_CACHE_MAX_SAMPLES, &got, &wi, 0) || got == 0) {
                errors.push_back(path + ": wave_file not found/invalid (need 16-bit PCM mono/stereo): " + wav_path);
                f.close();
                continue;
            }
            if (!isSupportedDacRate((int)wi.sampleRate)) {
                errors.push_back(path + ": WAV sample rate not supported by DAC (PCM5102A): " + wav_path);
                f.close();
                continue;
            }
            p.wav_channels = wi.channels;
            p.sample_rate_hz = (int)wi.sampleRate;
            p.wav_left_samples.resize(got);
            p.wav_right_samples.resize(got);
        }
        out.push_back(p);
        f.close();
    }

    File elog = FFat.open("/errors.log", "w");
    if (elog) {
        for (size_t i = 0; i < errors.size(); i++) {
            elog.println(errors[i]);
        }
        elog.close();
    }
    FFat.end();
    return out;
}

