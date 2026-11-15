#include "voice.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <xaudio2.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <vector>

namespace erv::voice {

float volume = 1.f;
bool muted = false;
bool positional = true;

namespace {

constexpr int kRate = 48000;
constexpr int kRing = 64;            // buffers per speaker; reused only once XAudio2 is done with them
constexpr int kMaxQueued = 12;       // 240 ms behind: drop and catch up rather than drift

template <class T>
void Release(T*& p) {
    if (p) p->Release(), p = nullptr;
}

struct Decoder {
    IMFTransform* mft = nullptr;
    bool float_out = false;
    int channels = 1;
    LONGLONG ts = 0;
    DWORD out_size = 0;
    bool provides = false;

    ~Decoder() { Release(mft); }
    bool Open(const CLSID& clsid, std::string& why);
    void Flush() {
        if (mft) mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        ts = 0;
    }
    // one Opus packet in, its PCM (mono int16) appended to `out`
    bool Decode(const uint8_t* data, uint32_t bytes, std::vector<int16_t>& out);
    void Drain(std::vector<int16_t>& out);
};

struct State {
    bool tried = false, ok = false;
    std::string why;
    CLSID clsid{};
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
};
State st;

struct Speaker {
    Decoder dec;
    IXAudio2SourceVoice* src = nullptr;
    std::vector<int16_t> ring[kRing];
    int ring_i = 0;
    size_t cursor = 0;
    float level = 0;
    double level_at = 0;
    bool mute = false;
    bool started = false;
    ~Speaker() {
        if (src) src->DestroyVoice(), src = nullptr;
    }
};
std::map<uint32_t, Speaker> speakers;
std::map<uint32_t, bool> mutes;
const Part* last_part = nullptr;
double last_t = -1;
bool was_playing = false;

double Clock() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

bool Decoder::Open(const CLSID& clsid, std::string& why) {
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft)))) {
        why = "the Opus decoder would not start";
        return false;
    }
    // OpusHead: mono, 48 kHz, no pre-skip, mapping family 0. Tried with and without, as decoders differ.
    const uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0};
    bool set = false;
    for (int attempt = 0; attempt < 2 && !set; ++attempt) {
        IMFMediaType* in = nullptr;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Opus);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
        if (attempt == 0) in->SetBlob(MF_MT_USER_DATA, head, sizeof head);
        set = SUCCEEDED(mft->SetInputType(0, in, 0));
        in->Release();
    }
    if (!set) {
        why = "the Opus decoder refused 48 kHz mono input";
        return false;
    }
    // output: 16-bit PCM if offered, else float
    IMFMediaType* best = nullptr;
    for (DWORD i = 0;; ++i) {
        IMFMediaType* t = nullptr;
        if (FAILED(mft->GetOutputAvailableType(0, i, &t))) break;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        const UINT32 bits = MFGetAttributeUINT32(t, MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
        const bool pcm16 = sub == MFAudioFormat_PCM && bits == 16, flt = sub == MFAudioFormat_Float;
        if (pcm16 || (flt && !best)) {
            Release(best);
            best = t;
            if (pcm16) break;
        } else {
            t->Release();
        }
    }
    if (!best || FAILED(mft->SetOutputType(0, best, 0))) {
        Release(best);
        why = "the Opus decoder offered no usable output";
        return false;
    }
    GUID sub{};
    best->GetGUID(MF_MT_SUBTYPE, &sub);
    float_out = sub == MFAudioFormat_Float;
    channels = int(MFGetAttributeUINT32(best, MF_MT_AUDIO_NUM_CHANNELS, 1));
    best->Release();
    MFT_OUTPUT_STREAM_INFO info{};
    mft->GetOutputStreamInfo(0, &info);
    provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    out_size = std::max<DWORD>(info.cbSize, 960 * 4 * 8);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

void Decoder::Drain(std::vector<int16_t>& out) {
    for (int guard = 0; guard < 16; ++guard) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        IMFSample* mine = nullptr;
        if (!provides) {
            IMFMediaBuffer* b = nullptr;
            MFCreateSample(&mine);
            MFCreateMemoryBuffer(out_size, &b);
            mine->AddBuffer(b);
            b->Release();
            ob.pSample = mine;
        }
        DWORD status = 0;
        const HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (SUCCEEDED(hr) && ob.pSample) {
            IMFMediaBuffer* b = nullptr;
            if (SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&b))) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                    if (float_out) {
                        const float* f = reinterpret_cast<const float*>(p);
                        const size_t n = len / sizeof(float) / size_t(channels);
                        for (size_t i = 0; i < n; ++i)
                            out.push_back(int16_t(std::clamp(f[i * channels] * 32767.f, -32768.f, 32767.f)));
                    } else {
                        const int16_t* s = reinterpret_cast<const int16_t*>(p);
                        const size_t n = len / sizeof(int16_t) / size_t(channels);
                        for (size_t i = 0; i < n; ++i) out.push_back(s[i * channels]);
                    }
                    b->Unlock();
                }
                b->Release();
            }
        }
        if (provides && ob.pSample) ob.pSample->Release();
        if (mine) mine->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {   // renegotiate: take the first type offered
            IMFMediaType* t = nullptr;
            if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, &t))) {
                mft->SetOutputType(0, t, 0);
                GUID sub{};
                t->GetGUID(MF_MT_SUBTYPE, &sub);
                float_out = sub == MFAudioFormat_Float;
                channels = int(MFGetAttributeUINT32(t, MF_MT_AUDIO_NUM_CHANNELS, 1));
                t->Release();
            }
            continue;
        }
        if (FAILED(hr)) break;   // MF_E_TRANSFORM_NEED_MORE_INPUT, normally
    }
}

bool Decoder::Decode(const uint8_t* data, uint32_t bytes, std::vector<int16_t>& out) {
    if (!mft || !bytes) return false;
    IMFSample* s = nullptr;
    IMFMediaBuffer* b = nullptr;
    MFCreateSample(&s);
    MFCreateMemoryBuffer(bytes, &b);
    BYTE* p = nullptr;
    b->Lock(&p, nullptr, nullptr);
    memcpy(p, data, bytes);
    b->Unlock();
    b->SetCurrentLength(bytes);
    s->AddBuffer(b);
    s->SetSampleTime(ts);
    s->SetSampleDuration(200000);
    ts += 200000;
    HRESULT hr = mft->ProcessInput(0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
        Drain(out);
        hr = mft->ProcessInput(0, s, 0);
    }
    b->Release();
    s->Release();
    if (FAILED(hr)) return false;
    Drain(out);
    return true;
}

Speaker* SpeakerFor(uint32_t slot) {
    auto it = speakers.find(slot);
    if (it != speakers.end()) return it->second.dec.mft ? &it->second : nullptr;
    Speaker& sp = speakers[slot];
    std::string why;
    if (!sp.dec.Open(st.clsid, why)) return nullptr;
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = kRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = kRate * 2;
    if (FAILED(st.xa->CreateSourceVoice(&sp.src, &wf, 0, 2.0f))) sp.src = nullptr;
    sp.mute = mutes[slot];
    return &sp;
}

void ResetAll() {
    for (auto& [slot, sp] : speakers) {
        if (sp.src) sp.src->Stop(), sp.src->FlushSourceBuffers(), sp.started = false;
        sp.dec.Flush();
    }
}

}  // namespace

const std::string& Why() { return st.why; }
bool Ready() { return st.ok; }

bool Init() {
    if (st.tried) return st.ok;
    st.tried = true;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return st.why = "Media Foundation is not available", false;
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Audio, MFAudioFormat_Opus};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr,
              &acts, &n);
    bool found = false;
    for (UINT32 i = 0; i < n; ++i) {
        if (!found && SUCCEEDED(acts[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &st.clsid))) found = true;
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    if (!found) return st.why = "Windows has no Opus decoder installed (needs Windows 10 1607 or later)", false;
    if (FAILED(XAudio2Create(&st.xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return st.why = "XAudio2 is not available", false;
    if (FAILED(st.xa->CreateMasteringVoice(&st.master))) return st.why = "no audio output device", false;
    st.ok = true;
    return true;
}

void SetPlayerMuted(uint32_t slot, bool m) {
    mutes[slot] = m;
    auto it = speakers.find(slot);
    if (it != speakers.end()) it->second.mute = m;
}
bool PlayerMuted(uint32_t slot) {
    auto it = mutes.find(slot);
    return it != mutes.end() && it->second;
}

float Level(uint32_t slot) {
    auto it = speakers.find(slot);
    if (it == speakers.end()) return 0;
    const double age = Clock() - it->second.level_at;
    return age > 0.15 ? 0.f : it->second.level;
}

void Stop() {
    ResetAll();
    last_part = nullptr;
    last_t = -1;
}

void Update(const Part& part, double t, double speed, bool playing, V3 ear, V3 right) {
    if (!st.ok) return;
    st.master->SetVolume(muted ? 0.f : volume);
    const bool audible = playing && speed >= 0.49 && speed <= 2.01;
    const bool jump = &part != last_part || t < last_t || t - last_t > 0.5 || !audible;
    if (jump) {
        ResetAll();
        for (const VoiceTrack& p : part.voices) {
            Speaker* sp = speakers.count(p.slot) ? &speakers[p.slot] : nullptr;
            if (sp) sp->cursor = size_t(std::lower_bound(p.t.begin(), p.t.end(), t) - p.t.begin());
        }
        last_part = &part;
        last_t = t;
        was_playing = false;
        if (!audible) return;
    }
    for (const VoiceTrack& p : part.voices) {
        if (p.t.empty() || p.off.size() != p.t.size() + 1) continue;
        Speaker* sp = SpeakerFor(p.slot);
        if (!sp || !sp->src) continue;
        if (jump || sp->cursor > p.t.size()) sp->cursor = size_t(std::lower_bound(p.t.begin(), p.t.end(), last_t) - p.t.begin());
        sp->src->SetFrequencyRatio(float(speed));
        {   // position: pan by the speaker's side of the camera, fade with distance (full inside 1.5 m)
            float l = 0.7071f, r = 0.7071f;
            if (positional) {
                for (const PlayerTrack& pt : part.players) {
                    if (pt.slot != p.slot) continue;
                    PlayerPose pose;
                    if (!SamplePlayer(pt, t, pose)) break;
                    const V3 d = pose.head - ear;
                    const float dist = Len(d);
                    const float gain = dist < 1.5f ? 1.f : std::max(0.06f, 1.5f / dist);
                    const float pan = dist < 0.3f ? 0.f : std::clamp(Dot(d, right) / dist, -1.f, 1.f);
                    l = gain * std::sqrt((1 - pan) * 0.5f), r = gain * std::sqrt((1 + pan) * 0.5f);
                    break;
                }
            }
            XAUDIO2_VOICE_DETAILS md{};
            st.master->GetVoiceDetails(&md);
            float m[8] = {};
            const UINT32 ch = std::min<UINT32>(md.InputChannels, 8);
            if (ch >= 2) m[0] = l, m[1] = r;
            else if (ch == 1) m[0] = std::max(l, r);
            sp->src->SetOutputMatrix(st.master, 1, ch, m);
        }
        XAUDIO2_VOICE_STATE vs{};
        while (sp->cursor < p.t.size() && p.t[sp->cursor] <= t) {
            const size_t k = sp->cursor++;
            const uint32_t a = p.off[k], b = p.off[k + 1];
            std::vector<int16_t>& buf = sp->ring[sp->ring_i];
            buf.clear();
            if (!sp->dec.Decode(p.bytes.data() + a, b - a, buf) || buf.empty()) continue;
            sp->src->GetState(&vs, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (vs.BuffersQueued >= kMaxQueued) sp->src->FlushSourceBuffers();   // fell behind: catch up
            if (sp->mute) continue;
            double sum = 0;
            for (int16_t v : buf) sum += double(v) * v;
            sp->level = std::min(1.f, float(std::sqrt(sum / double(buf.size())) / 6000.0));
            sp->level_at = Clock();
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = UINT32(buf.size() * sizeof(int16_t));
            xb.pAudioData = reinterpret_cast<const BYTE*>(buf.data());
            sp->src->SubmitSourceBuffer(&xb);
            sp->ring_i = (sp->ring_i + 1) % kRing;
        }
        if (!sp->started) sp->src->Start(), sp->started = true;
    }
    last_t = t;
}

int DecodeToWav(const VoiceTrack& p, const std::wstring& wav, std::string& why) {
    if (!Init() && st.clsid == CLSID{}) return why = st.why, -1;
    Decoder d;
    if (!d.Open(st.clsid, why)) return -1;
    std::vector<int16_t> pcm;
    int ok = 0;
    for (size_t k = 0; k + 1 < p.off.size(); ++k)
        if (d.Decode(p.bytes.data() + p.off[k], p.off[k + 1] - p.off[k], pcm)) ++ok;
    std::ofstream f(wav, std::ios::binary);
    const uint32_t data = uint32_t(pcm.size() * 2), riff = 36 + data, fmt = 16, rate = kRate, bps = kRate * 2;
    const uint16_t pcmTag = 1, ch = 1, align = 2, bits = 16;
    f.write("RIFF", 4).write((const char*)&riff, 4).write("WAVEfmt ", 8).write((const char*)&fmt, 4);
    f.write((const char*)&pcmTag, 2).write((const char*)&ch, 2).write((const char*)&rate, 4).write((const char*)&bps, 4);
    f.write((const char*)&align, 2).write((const char*)&bits, 2).write("data", 4).write((const char*)&data, 4);
    f.write((const char*)pcm.data(), data);
    if (pcm.empty()) why = "decoded no samples";
    return ok;
}

}  // namespace erv::voice
