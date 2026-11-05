// The mix through SDL, in place of audio/xaudio2.cpp: phones and tablets play stereo, so the 5.1 mix folds down as
// XAudio2's matrix folds it for stereo speakers.
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include <atomic>
#include <vector>

#include "audio/output.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr float kCentreFold = 0.707f;
constexpr float kBackFold = 0.707f;

std::atomic<bool> g_mono{false};
std::function<void(float*, uint32_t)> g_render;
SDL_AudioStream* g_stream = nullptr;

// SDL's audio thread, asking for more
void SDLCALL Feed(void*, SDL_AudioStream* stream, int additional, int) {
    static std::vector<float> mix, out;
    uint32_t frames = uint32_t(additional) / (2 * sizeof(float));
    if (!frames) return;
    mix.resize(frames * kChannels);
    out.resize(frames * 2);
    g_render(mix.data(), frames);
    bool mono = g_mono;
    for (uint32_t i = 0; i < frames; i++) {
        const float* f = &mix[i * kChannels];
        float l = f[FrontLeft] + kCentreFold * f[Centre] + kBackFold * f[BackLeft];
        float r = f[FrontRight] + kCentreFold * f[Centre] + kBackFold * f[BackRight];
        if (mono) l = r = (l + r) * 0.5f;
        out[i * 2] = l;
        out[i * 2 + 1] = r;
    }
    SDL_PutAudioStreamData(stream, out.data(), int(out.size() * sizeof(float)));
}

}  // namespace

void StartOutput(std::function<void(float* out, uint32_t frames)> render) {
    g_render = std::move(render);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kRate)};
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO) ||
        !(g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, nullptr))) {
        LOGE("audio: no output: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    LOGI("audio: stereo output through SDL");
}

bool IsSurround() { return false; }
bool IsMono() { return g_mono; }
void SetOutputMode(uint32_t mode) { g_mono = mode == kOutputMono; }

void SetPaused(bool paused) {
    if (!g_stream) return;
    if (paused) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
}

}  // namespace audio
