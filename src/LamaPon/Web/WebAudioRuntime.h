#pragma once

#include <cstdint>
#include <string_view>

namespace LamaPon::Web
{
    using AudioHandle = std::uint32_t;

    // Browser向けの薄いWeb Audio Adapterです。Game CodeはURLまたはToneで
    // 再生を要求し、自動再生制限はUnlockFromUserGesture()で処理します。
    class WebAudioRuntime final
    {
    public:
        WebAudioRuntime() = default;

        void Initialize() noexcept;
        void UnlockFromUserGesture() noexcept;
        void SetMasterVolume(float volume) noexcept;
        void PlayTone(
            float frequency,
            float durationSeconds,
            float volume = 0.12f) noexcept;
        void PlayWav(
            std::string_view url,
            float volume = 1.0f,
            bool loop = false,
            float pan = 0.0f,
            bool spatial = false,
            float x = 0.0f,
            float y = 0.0f,
            float z = 0.0f,
            float minimumDistance = 1.0f,
            float maximumDistance = 20.0f) noexcept;
        [[nodiscard]] AudioHandle PlayLoop(
            std::string_view url,
            float volume = 1.0f,
            float pan = 0.0f,
            bool spatial = false,
            float x = 0.0f,
            float y = 0.0f,
            float z = 0.0f,
            float minimumDistance = 1.0f,
            float maximumDistance = 20.0f) noexcept;
        void SetVolume(AudioHandle handle, float volume) noexcept;
        void SetPitch(AudioHandle handle, float octaves) noexcept;
        void SetPan(AudioHandle handle, float pan) noexcept;
        void SetPosition(
            AudioHandle handle,
            float x,
            float y,
            float z) noexcept;
        void SetListener(
            float x,
            float y,
            float z,
            float forwardX,
            float forwardY,
            float forwardZ,
            float upX,
            float upY,
            float upZ) noexcept;
        void Stop(AudioHandle handle) noexcept;

    private:
        bool m_initialized{};
        float m_masterVolume{ 1.0f };
    };
}
