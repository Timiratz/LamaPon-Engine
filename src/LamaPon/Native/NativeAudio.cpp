#include "LamaPon/Web/WebAudioRuntime.h"
#include "LamaPon/Web/WebMath.h"
#include "LamaPon/Native/NativeServices.h"
#include <stb_vorbis.c>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace LamaPon::Native
{
    namespace
    {
        constexpr int SampleRate = 48000;
        struct Voice
        {
            std::shared_ptr<std::vector<float>> samples;
            double cursor{};
            float volume{1}, pan{}, pitch{1};
            bool loop{}, spatial{};
            Web::Vec3 position{};
            float minimumDistance{1}, maximumDistance{20};
        };
        SDL_AudioStream* device{};
        std::mutex voiceMutex;
        std::map<std::uint32_t, Voice> voices;
        std::unordered_map<std::string, std::shared_ptr<std::vector<float>>> clips;
        std::uint32_t nextHandle{1};
        float masterVolume{1};
        Web::Vec3 listener{}, right{1, 0, 0};
        float Clamp(const float value, const float low, const float high)
        { return std::isfinite(value) ? std::clamp(value, low, high) : low; }

        void SDLCALL Mix(void*, SDL_AudioStream* stream, int additional, int)
        {
            std::array<float, 1024> output{};
            std::scoped_lock lock(voiceMutex);
            while (additional > 0)
            {
                const int frames = std::min(512, (additional + 7) / 8);
                output.fill(0);
                for (auto iterator = voices.begin(); iterator != voices.end();)
                {
                    auto& voice = iterator->second;
                    const auto count = voice.samples->size() / 2;
                    float pan = voice.pan, gain = voice.volume * masterVolume;
                    if (voice.spatial)
                    {
                        const auto relative = voice.position - listener;
                        if (!std::isfinite(relative.x) || !std::isfinite(relative.y) || !std::isfinite(relative.z))
                            gain = 0;
                        else
                        {
                            const float distance = Web::Length(relative);
                            gain *= 1 - Clamp((distance - voice.minimumDistance)
                                / (voice.maximumDistance - voice.minimumDistance), 0, 1);
                            if (distance > 0.001f) pan = Clamp(Web::Dot(relative, right) / distance, -1, 1);
                        }
                    }
                    const float leftGain = gain * (pan > 0 ? 1 - pan : 1);
                    const float rightGain = gain * (pan < 0 ? 1 + pan : 1);
                    bool complete = count == 0;
                    for (int frame = 0; frame < frames && !complete; ++frame)
                    {
                        if (voice.cursor >= static_cast<double>(count))
                        {
                            if (!voice.loop) { complete = true; break; }
                            voice.cursor = std::fmod(voice.cursor, static_cast<double>(count));
                        }
                        const auto first = static_cast<std::size_t>(voice.cursor);
                        const auto second = first + 1 < count ? first + 1 : (voice.loop ? 0 : first);
                        const float blend = static_cast<float>(voice.cursor - static_cast<double>(first));
                        const auto& samples = *voice.samples;
                        output[static_cast<std::size_t>(frame) * 2] += leftGain
                            * (samples[first * 2] + (samples[second * 2] - samples[first * 2]) * blend);
                        output[static_cast<std::size_t>(frame) * 2 + 1] += rightGain
                            * (samples[first * 2 + 1] + (samples[second * 2 + 1] - samples[first * 2 + 1]) * blend);
                        voice.cursor += voice.pitch;
                    }
                    if (complete) iterator = voices.erase(iterator); else ++iterator;
                }
                for (int index = 0; index < frames * 2; ++index)
                    output[static_cast<std::size_t>(index)] = Clamp(output[static_cast<std::size_t>(index)], -1, 1);
                SDL_PutAudioStreamData(stream, output.data(), frames * 8);
                additional -= frames * 8;
            }
        }

        std::shared_ptr<std::vector<float>> LoadClip(const std::string& path)
        {
            if (const auto found = clips.find(path); found != clips.end()) return found->second;
            const auto bytes = ReadAsset(path.c_str());
            if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
            SDL_AudioSpec source{};
            Uint8* wav{};
            Uint32 wavLength{};
            short* ogg{};
            const Uint8* input{};
            int length{};
            struct Release
            {
                Uint8*& wav; short*& ogg;
                ~Release() { SDL_free(wav); std::free(ogg); }
            } release{wav, ogg};
            if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0)
            {
                int channels{}, rate{};
                const int frames = stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()), &channels, &rate, &ogg);
                if (frames <= 0 || rate <= 0 || channels <= 0 || channels > 8
                    || frames > std::numeric_limits<int>::max() / channels / 2) return {};
                source.format = SDL_AUDIO_S16;
                source.channels = channels;
                source.freq = rate;
                input = reinterpret_cast<const Uint8*>(ogg);
                length = frames * channels * 2;
            }
            else
            {
                if (!SDL_LoadWAV_IO(SDL_IOFromConstMem(bytes.data(), bytes.size()), true, &source, &wav, &wavLength)) return {};
                if (wavLength > static_cast<Uint32>(std::numeric_limits<int>::max())) return {};
                input = wav;
                length = static_cast<int>(wavLength);
            }
            SDL_AudioSpec target{};
            target.format = SDL_AUDIO_F32;
            target.channels = 2;
            target.freq = SampleRate;
            Uint8* converted{};
            int convertedLength{};
            if (!SDL_ConvertAudioSamples(&source, input, length, &target, &converted, &convertedLength)) return {};
            struct Free { Uint8* value; ~Free() { SDL_free(value); } } free{converted};
            if (!converted || convertedLength <= 0 || convertedLength % (2 * sizeof(float)) != 0) return {};
            auto result = std::make_shared<std::vector<float>>(static_cast<std::size_t>(convertedLength) / sizeof(float));
            std::memcpy(result->data(), converted, static_cast<std::size_t>(convertedLength));
            if (!std::all_of(result->begin(), result->end(), [](const float sample) { return std::isfinite(sample); }))
            {
                SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Audio asset %s contains non-finite PCM samples", path.c_str());
                return {};
            }
            clips.emplace(path, result);
            return result;
        }

        std::uint32_t AddVoice(Voice voice)
        {
            std::scoped_lock lock(voiceMutex);
            if (!device || !voice.samples || voice.samples->empty() || voices.size() >= 256) return 0;
            // 生存中のhandleとの衝突と0を避ける。
            while (nextHandle == 0 || voices.contains(nextHandle)) ++nextHandle;
            const auto handle = nextHandle++;
            voices.emplace(handle, std::move(voice));
            return handle;
        }
    }

    void ShutdownAudio() noexcept
    {
        if (device) SDL_DestroyAudioStream(device);
        device = nullptr;
        std::scoped_lock lock(voiceMutex);
        voices.clear(); clips.clear();
    }
    void UpdateAudio(float) noexcept {}
    void SuspendAudio(const bool suspend) noexcept
    {
        if (!device) return;
        if (suspend) SDL_PauseAudioStreamDevice(device); else SDL_ResumeAudioStreamDevice(device);
    }

#if defined(LAMAPON_NATIVE_AUDIO_PROBE)
    // Test the actual callback through an SDL stream while the device is paused.
    std::vector<float> ProbeAudioMix(const int frames)
    {
        if (frames <= 0 || frames > 4096) throw std::invalid_argument("Invalid audio probe size");
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, SampleRate};
        auto* stream = SDL_CreateAudioStream(&spec, &spec);
        if (!stream) throw std::runtime_error(SDL_GetError());
        struct Release { SDL_AudioStream* stream; ~Release() { SDL_DestroyAudioStream(stream); } } release{stream};
        Mix(nullptr, stream, frames * 8, frames * 8);
        std::vector<float> output(static_cast<std::size_t>(frames) * 2);
        if (SDL_GetAudioStreamData(stream, output.data(), frames * 8) != frames * 8)
            throw std::runtime_error("Audio probe did not receive the mixed PCM frames");
        return output;
    }
    bool ProbeAudioPaused() { return device && SDL_AudioStreamDevicePaused(device); }
#endif
}

namespace LamaPon::Web
{
    using namespace Native;
    void WebAudioRuntime::Initialize() noexcept
    {
        if (m_initialized) return;
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32; spec.channels = 2; spec.freq = SampleRate;
        device = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Mix, nullptr);
        if (!device) { SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", SDL_GetError()); return; }
        m_initialized = true;
        SetMasterVolume(m_masterVolume);
        SDL_ResumeAudioStreamDevice(device);
    }
    void WebAudioRuntime::UnlockFromUserGesture() noexcept {}
    void WebAudioRuntime::SetMasterVolume(const float value) noexcept
    {
        std::scoped_lock lock(voiceMutex);
        masterVolume = m_masterVolume = Clamp(value, 0, 1);
    }
    void WebAudioRuntime::PlayTone(float frequency, float duration, float volume) noexcept
    {
        try
        {
            frequency = Clamp(frequency, 20, 20000); duration = Clamp(duration, 0.02f, 10);
            auto samples = std::make_shared<std::vector<float>>(static_cast<std::size_t>(duration * SampleRate) * 2);
            for (std::size_t index = 0; index < samples->size() / 2; ++index)
            {
                const double phase = std::fmod(static_cast<double>(index) * frequency / SampleRate, 1.0);
                const float sample = static_cast<float>(4 * std::abs(phase - 0.5) - 1);
                (*samples)[index * 2] = (*samples)[index * 2 + 1] = sample;
            }
            Voice voice; voice.samples = std::move(samples); voice.volume = Clamp(volume, 0, 1);
            static_cast<void>(AddVoice(std::move(voice)));
        }
        catch (...) { SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Cannot allocate tone"); }
    }
    AudioHandle WebAudioRuntime::PlayLoop(std::string_view path, float volume, float pan, bool spatial,
        float x, float y, float z, float minimum, float maximum) noexcept
    {
        try
        {
            Voice voice;
            voice.samples = LoadClip(std::string(path)); voice.loop = true;
            voice.volume = Clamp(volume, 0, 1); voice.pan = Clamp(pan, -1, 1); voice.spatial = spatial;
            voice.position = {x, y, z}; voice.minimumDistance = Clamp(minimum, 0.001f, 100000);
            voice.maximumDistance = std::max(voice.minimumDistance + 0.001f, Clamp(maximum, 0.001f, 100000));
            return AddVoice(std::move(voice));
        }
        catch (...) { SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Cannot read audio asset"); return 0; }
    }
    void WebAudioRuntime::PlayWav(std::string_view path, float volume, bool loop, float pan, bool spatial,
        float x, float y, float z, float minimum, float maximum) noexcept
    {
        const auto handle = PlayLoop(path, volume, pan, spatial, x, y, z, minimum, maximum);
        if (handle && !loop) { std::scoped_lock lock(voiceMutex); if (voices.contains(handle)) voices.at(handle).loop = false; }
    }
    void WebAudioRuntime::SetVolume(AudioHandle handle, float volume) noexcept
    { std::scoped_lock lock(voiceMutex); if (voices.contains(handle)) voices.at(handle).volume = Clamp(volume, 0, 1); }
    void WebAudioRuntime::SetPitch(AudioHandle handle, float octaves) noexcept
    { std::scoped_lock lock(voiceMutex); if (voices.contains(handle)) voices.at(handle).pitch = std::exp2(Clamp(octaves, -1, 1)); }
    void WebAudioRuntime::SetPan(AudioHandle handle, float pan) noexcept
    { std::scoped_lock lock(voiceMutex); if (voices.contains(handle)) voices.at(handle).pan = Clamp(pan, -1, 1); }
    void WebAudioRuntime::SetPosition(AudioHandle handle, float x, float y, float z) noexcept
    { std::scoped_lock lock(voiceMutex); if (voices.contains(handle)) voices.at(handle).position = {x, y, z}; }
    void WebAudioRuntime::SetListener(float x, float y, float z, float fx, float fy, float fz, float ux, float uy, float uz) noexcept
    {
        std::scoped_lock lock(voiceMutex); listener = {x, y, z};
        right = Normalize(Cross(Vec3{fx, fy, fz}, Vec3{ux, uy, uz}));
    }
    void WebAudioRuntime::Stop(AudioHandle handle) noexcept
    { std::scoped_lock lock(voiceMutex); voices.erase(handle); }
}
