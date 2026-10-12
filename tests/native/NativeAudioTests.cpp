#include "LamaPon/Native/NativeServices.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Web/WebAudioRuntime.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace LamaPon::Native
{
    std::vector<float> ProbeAudioMix(int frames);
    bool ProbeAudioPaused();
}

namespace
{
    void Check(const bool condition, const char* message)
    { if (!condition) throw std::runtime_error(message); }
    void CheckSamples(const std::vector<float>& samples, float left, float right)
    {
        for (std::size_t index = 0; index < samples.size(); index += 2)
            if (!(std::isfinite(samples[index]) && std::isfinite(samples[index + 1])
                && std::abs(samples[index] - left) < 0.0001f
                && std::abs(samples[index + 1] - right) < 0.0001f))
                throw std::runtime_error("Mixed PCM differs at frame " + std::to_string(index / 2)
                    + ": expected " + std::to_string(left) + ", " + std::to_string(right)
                    + "; actual " + std::to_string(samples[index]) + ", " + std::to_string(samples[index + 1]));
    }
}

int RunAudioTests(const std::filesystem::path& assetDirectory,
    const std::filesystem::path& saveDirectory, const std::filesystem::path& oggDirectory)
{
    try
    {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        Check(SDL_Init(SDL_INIT_AUDIO), "Cannot initialize SDL dummy audio");
        LamaPon::Native::ConfigureServices(nullptr, assetDirectory, saveDirectory);
        LamaPon::Web::WebAudioRuntime audio;
        audio.Initialize();
        LamaPon::Native::SuspendAudio(true);
        Check(LamaPon::Native::ProbeAudioPaused(), "Audio device did not pause");
        LamaPon::Native::SuspendAudio(false);
        Check(!LamaPon::Native::ProbeAudioPaused(), "Audio device did not resume");
        LamaPon::Native::SuspendAudio(true);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, 0);
        Check(!audio.PlayLoop("/assets/scenes/白画像.bmp"), "Invalid WAV became a playing voice");
        Check(!audio.PlayLoop("/assets/audio/missing.wav"), "Missing audio became a playing voice");
        const auto voice = audio.PlayLoop("/assets/audio/ステレオ.wav");
        Check(voice != 0, "UTF-8 WAV did not decode into a voice");
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0.25f, -0.5f);
        audio.SetMasterVolume(0.5f); audio.SetVolume(voice, 0.5f); audio.SetPan(voice, 1);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, -0.125f);
        audio.SetPan(voice, -1);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0.0625f, 0);
        audio.SetMasterVolume(1); audio.SetVolume(voice, 1); audio.SetPan(voice, 0);
        audio.SetPitch(voice, 1);
        CheckSamples(LamaPon::Native::ProbeAudioMix(2048), 0.25f, -0.5f);
        audio.Stop(voice);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, 0);
        const auto spatial = audio.PlayLoop("/assets/audio/ステレオ.wav", 1, 0, true, 1, 0, 0);
        Check(spatial != 0, "Spatial WAV did not start");
        audio.SetListener(0, 0, 0, 0, 0, -1, 0, 1, 0);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, -0.5f);
        audio.SetPosition(spatial, -1, 0, 0);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0.25f, 0);
        audio.SetPosition(spatial, 100, 0, 0);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, 0);
        audio.SetPosition(spatial, std::numeric_limits<float>::quiet_NaN(), 0, 0);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, 0);
        audio.SetPosition(spatial, 0, std::numeric_limits<float>::infinity(), 0);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, 0);
        audio.SetPosition(spatial, 1, 0, 0);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, -0.5f);
        audio.SetListener(0, 0, std::numeric_limits<float>::quiet_NaN(), 0, 0, -1, 0, 1, 0);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, 0);
        audio.SetListener(0, 0, 0, 0, 0, -1, 0, 1, 0);
        audio.Stop(spatial);
        const auto mono = audio.PlayLoop("/assets/audio/mono-24000.wav");
        Check(mono != 0, "Mono WAV did not convert to 48kHz stereo");
        const auto resampled = LamaPon::Native::ProbeAudioMix(256);
        CheckSamples({resampled.begin() + 64, resampled.end() - 64}, 0.125f, 0.125f);
        audio.Stop(mono);
        audio.PlayWav("/assets/audio/ステレオ.wav", 1, false);
        CheckSamples(LamaPon::Native::ProbeAudioMix(1024), 0.25f, -0.5f);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, 0);
        std::vector<LamaPon::Web::AudioHandle> handles;
        for (int index = 0; index < 256; ++index)
        {
            handles.push_back(audio.PlayLoop("/assets/audio/ステレオ.wav"));
            Check(handles.back() != 0, "Voice capacity was reached prematurely");
        }
        Check(!audio.PlayLoop("/assets/audio/ステレオ.wav"), "Voice capacity overflow was accepted");
        for (auto handle : handles) audio.Stop(handle);
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, 0);
        // Reuse the existing project-owned Vorbis fixture without duplicating it.
        LamaPon::Native::ConfigureServices(nullptr, oggDirectory, saveDirectory);
        Check(!audio.PlayLoop("/assets/nonfinite.wav"), "Non-finite decoded PCM became a playing voice");
        CheckSamples(LamaPon::Native::ProbeAudioMix(32), 0, 0);
        const auto ogg = audio.PlayLoop("/assets/startup.ogg");
        Check(ogg != 0, "Ogg Vorbis did not decode into a voice");
        for (int block = 0; block < 4; ++block)
        {
            const auto samples = LamaPon::Native::ProbeAudioMix(4096);
            double energy{};
            for (std::size_t index = 0; index < samples.size(); index += 2)
            {
                Check(std::isfinite(samples[index]) && std::abs(samples[index]) <= 1
                    && std::abs(samples[index] - samples[index + 1]) < 0.0001f,
                    "Mono Vorbis conversion produced invalid stereo PCM");
                energy += samples[index] * samples[index];
            }
            Check(energy > 0.001, "Ogg loop lost its audio signal across the loop boundary");
        }
        audio.Stop(ogg);
        audio.PlayWav("/assets/startup.ogg", 1, false);
        for (int block = 0; block < 3; ++block)
            static_cast<void>(LamaPon::Native::ProbeAudioMix(4096));
        CheckSamples(LamaPon::Native::ProbeAudioMix(4096), 0, 0);
        LamaPon::Native::ShutdownAudio();
        SDL_Quit();
        std::cout << "Native audio PCM, loop, pan, spatial, failure and pause tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        LamaPon::Native::ShutdownAudio(); SDL_Quit();
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    if (argc != 4)
    {
        std::cerr << "Pass existing asset, save and Ogg fixture directories\n";
        return 1;
    }
    return RunAudioTests(argv[1], argv[2], argv[3]);
}
