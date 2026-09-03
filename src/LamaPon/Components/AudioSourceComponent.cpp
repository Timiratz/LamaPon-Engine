#include "LamaPon/Components/AudioSourceComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Components/AudioListenerComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace LamaPon
{
    AudioSourceComponent::AudioSourceComponent(
        std::filesystem::path audioPath,
        const float volume,
        const float pitch,
        const float pan,
        const bool loop,
        const bool playOnStart,
        const bool spatial,
        const float minimumDistance,
        const float maximumDistance)
        : m_audioPath(std::move(audioPath))
        , m_volume(std::clamp(volume, 0.0f, 1.0f))
        , m_pitch(std::clamp(pitch, -1.0f, 1.0f))
        , m_pan(std::clamp(pan, -1.0f, 1.0f))
        , m_loop(loop)
        , m_playOnStart(playOnStart)
        , m_spatial(spatial)
        , m_minimumDistance(std::max(minimumDistance, 0.01f))
        , m_maximumDistance(
            std::max(
                maximumDistance,
                m_minimumDistance + 0.01f))
    {
        UpdateDistanceCurve();
    }

    AudioSourceComponent::~AudioSourceComponent()
    {
        Stop();
    }

    void AudioSourceComponent::SetAudioPath(
        std::filesystem::path path)
    {
        m_audioPath = std::move(path);
        ReloadSound();
    }

    void AudioSourceComponent::SetVolume(const float volume)
    {
        m_volume = std::clamp(volume, 0.0f, 1.0f);
        const float effective = EffectiveVolume();
        if (m_stream)
        {
            m_stream->SetVolume(effective);
        }
        if (m_instance)
        {
            m_instance->SetVolume(effective);
        }
        for (const auto& oneShot : m_spatialOneShots)
        {
            oneShot->SetVolume(effective);
        }
    }

    float AudioSourceComponent::EffectiveVolume()
        const noexcept
    {
        const float busVolume = m_audio != nullptr
            ? m_audio->BusVolume(m_bus)
            : 1.0f;
        return m_volume * busVolume;
    }

    void AudioSourceComponent::SetStreaming(
        const bool streaming)
    {
        if (m_streaming == streaming)
        {
            return;
        }
        const bool wasPlaying =
            State() == DirectX::PLAYING;
        m_streaming = streaming;
        ReloadSound();
        if (wasPlaying)
        {
            Play();
        }
    }

    void AudioSourceComponent::SetBus(const AudioBus bus)
    {
        m_bus = bus;
        SetVolume(m_volume);
    }

    void AudioSourceComponent::SetPitch(const float pitch)
    {
        m_pitch = std::clamp(pitch, -1.0f, 1.0f);
        if (m_stream)
        {
            m_stream->SetPitch(m_pitch);
        }
        if (m_instance)
        {
            m_instance->SetPitch(m_pitch);
        }
        for (const auto& oneShot : m_spatialOneShots)
        {
            oneShot->SetPitch(m_pitch);
        }
    }

    void AudioSourceComponent::SetPan(const float pan)
    {
        m_pan = std::clamp(pan, -1.0f, 1.0f);
        if (m_stream)
        {
            m_stream->SetPan(m_pan);
        }
        if (m_instance && !m_spatial)
        {
            m_instance->SetPan(m_pan);
        }
    }

    void AudioSourceComponent::SetSpatial(const bool spatial)
    {
        if (m_spatial == spatial)
        {
            return;
        }

        const bool wasPlaying =
            State() == DirectX::PLAYING;
        m_spatial = spatial;
        ReloadSound();
        if (wasPlaying)
        {
            Play();
        }
    }

    void AudioSourceComponent::SetMinimumDistance(
        const float distance)
    {
        m_minimumDistance = std::max(distance, 0.01f);
        m_maximumDistance = std::max(
            m_maximumDistance,
            m_minimumDistance + 0.01f);
        UpdateDistanceCurve();
        if (m_instance && m_spatial)
        {
            ApplySpatial(*m_instance);
        }
    }

    void AudioSourceComponent::SetMaximumDistance(
        const float distance)
    {
        m_maximumDistance = std::max(
            distance,
            m_minimumDistance + 0.01f);
        UpdateDistanceCurve();
        if (m_instance && m_spatial)
        {
            ApplySpatial(*m_instance);
        }
    }

    void AudioSourceComponent::SetLoop(const bool loop)
    {
        if (m_loop == loop)
        {
            return;
        }

        const bool wasPlaying =
            State() == DirectX::PLAYING;
        Stop();
        m_loop = loop;
        if (wasPlaying)
        {
            Play();
        }
    }

    void AudioSourceComponent::SetLoopRegionFrames(
        const std::uint64_t startFrame,
        const std::uint64_t endFrame,
        const std::uint64_t crossfadeFrames) noexcept
    {
        if (startFrame >= endFrame)
        {
            ClearLoopRegion();
            return;
        }
        m_loopStartFrame = startFrame;
        m_loopEndFrame = endFrame;
        m_loopCrossfadeFrames = crossfadeFrames;
        m_hasLoopRegion = true;
        if (m_stream)
        {
            m_stream->SetLoopRegionFrames(
                startFrame,
                endFrame,
                crossfadeFrames);
        }
    }

    void AudioSourceComponent::SetStartFrame(
        const std::uint64_t frame) noexcept
    {
        m_startFrame = frame;
        if (m_stream)
        {
            m_stream->SetStartFrame(frame);
        }
    }

    void AudioSourceComponent::SetBassBoost(
        const float gainDb, const float cornerHz)
    {
        m_bassBoostDb = std::max(gainDb, 0.0f);
        m_bassCornerHz = cornerHz;
        if (m_stream)
        {
            m_stream->SetBassBoost(m_bassBoostDb, m_bassCornerHz);
        }
    }

    void AudioSourceComponent::SetLevelMeterEnabled(
        const bool enabled)
    {
        m_levelMeterEnabled = enabled;
        if (m_stream)
        {
            m_stream->SetLevelMeterEnabled(enabled);
        }
    }

    std::size_t AudioSourceComponent::ReadLevelBands(
        float* destination,
        const std::size_t capacity) const noexcept
    {
        if (m_stream)
        {
            return m_stream->ReadLevelBands(destination, capacity);
        }
        if (destination == nullptr || capacity == 0)
        {
            return 0;
        }
        // 音源をまだ持っていないときも、呼ぶ側が長さを気にせず
        // 描けるように0で埋めて本数だけ返します。
        const std::size_t count = std::min(
            capacity,
            static_cast<std::size_t>(LevelBandCount));
        for (std::size_t index = 0; index < count; ++index)
        {
            destination[index] = 0.0f;
        }
        return count;
    }

    std::uint64_t
        AudioSourceComponent::PlaybackFrame() const noexcept
    {
        return m_stream ? m_stream->PlaybackFrame() : 0;
    }

    std::size_t AudioSourceComponent::ReadPeakEnvelope(
        float* destination,
        const std::size_t bucketCount)
    {
        return m_stream
            ? m_stream->ReadPeakEnvelope(destination, bucketCount)
            : 0;
    }

    void AudioSourceComponent::ClearLoopRegion() noexcept
    {
        m_loopStartFrame = 0;
        m_loopEndFrame = 0;
        m_loopCrossfadeFrames = 0;
        m_hasLoopRegion = false;
        if (m_stream)
        {
            m_stream->ClearLoopRegion();
        }
    }

    void AudioSourceComponent::Play()
    {
        if (m_streaming)
        {
            if (!m_stream)
            {
                m_playRequested = true;
                return;
            }
            ApplyProperties();
            m_stream->SetLoop(m_loop);
            m_stream->Play();
            m_playRequested = false;
            return;
        }
        if (!m_instance)
        {
            m_playRequested = true;
            return;
        }

        ApplyProperties();
        m_instance->Play(m_loop);
        m_playRequested = false;
    }

    void AudioSourceComponent::PlayOneShot()
    {
        // ストリーミングは多重再生できないためPlayとして扱います。
        if (m_streaming)
        {
            Play();
            m_oneShotRequested = false;
            return;
        }
        if (!m_sound)
        {
            m_oneShotRequested = true;
            return;
        }

        if (!m_spatial)
        {
            m_sound->Play(
                EffectiveVolume(),
                m_pitch,
                m_pan);
        }
        else
        {
            auto instance = m_sound->CreateInstance(
                DirectX::SoundEffectInstance_Use3D);
            instance->SetVolume(EffectiveVolume());
            instance->SetPitch(m_pitch);
            ApplySpatial(*instance);
            instance->Play(false);
            m_spatialOneShots.push_back(
                std::move(instance));
        }
        m_oneShotRequested = false;
    }

    void AudioSourceComponent::Pause() noexcept
    {
        if (m_stream)
        {
            m_stream->Pause();
        }
        if (m_instance)
        {
            m_instance->Pause();
        }
    }

    void AudioSourceComponent::Resume()
    {
        if (m_stream)
        {
            m_stream->Resume();
        }
        if (m_instance)
        {
            m_instance->Resume();
        }
    }

    void AudioSourceComponent::Stop() noexcept
    {
        m_playRequested = false;
        if (m_stream)
        {
            m_stream->Stop();
        }
        if (m_instance)
        {
            m_instance->Stop();
        }
        for (const auto& oneShot : m_spatialOneShots)
        {
            oneShot->Stop();
        }
        m_spatialOneShots.clear();
    }

    DirectX::SoundState AudioSourceComponent::State() const noexcept
    {
        if (m_stream)
        {
            return m_stream->State();
        }
        return m_instance
            ? m_instance->GetState()
            : DirectX::STOPPED;
    }

    void AudioSourceComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_assets = &graphics.Assets();
        m_audio = &graphics.Audio();
        ReloadSound();
        m_deviceGeneration = m_audio->DeviceGeneration();
    }

    void AudioSourceComponent::OnUpdate(float)
    {
        if (m_audio != nullptr
            && m_audio->DeviceGeneration() != m_deviceGeneration)
        {
            // The audio device was reset (e.g. the user switched
            // playback devices). Any previously created voice is
            // now invalid, so recreate it instead of calling into
            // it and risking a crash.
            m_deviceGeneration = m_audio->DeviceGeneration();
            const bool wasPlaying = State() == DirectX::PLAYING;
            ReloadSound();
            if (wasPlaying)
            {
                Play();
            }
        }

        if (m_spatial)
        {
            std::erase_if(
                m_spatialOneShots,
                [](const auto& instance)
                {
                    return instance->GetState()
                        == DirectX::STOPPED;
                });
            if (m_instance)
            {
                ApplySpatial(*m_instance);
            }
            for (const auto& oneShot : m_spatialOneShots)
            {
                ApplySpatial(*oneShot);
            }
        }

        // バス音量が変わっていたら再生中ボイスへ反映します。
        if (m_audio != nullptr)
        {
            const float busVolume =
                m_audio->BusVolume(m_bus);
            if (busVolume != m_lastBusVolume)
            {
                m_lastBusVolume = busVolume;
                SetVolume(m_volume);
            }
        }

        if (!m_started)
        {
            m_started = true;
            if (m_playOnStart)
            {
                Play();
            }
        }
    }

    void AudioSourceComponent::ReloadSound()
    {
        const bool playRequested = m_playRequested;
        Stop();
        m_playRequested = playRequested;
        m_instance.reset();
        m_stream.reset();
        m_sound.reset();
        if (m_audio == nullptr
            || m_assets == nullptr
            || m_audioPath.empty())
        {
            return;
        }

        if (m_streaming)
        {
            m_stream = m_audio->CreateStream(
                *m_assets,
                m_assets->ResolvePath(m_audioPath));
        }
        else
        {
            m_sound = m_audio->LoadSoundEffect(
                *m_assets,
                m_assets->ResolvePath(m_audioPath));
            m_instance = m_sound->CreateInstance(
                m_spatial
                    ? DirectX::SoundEffectInstance_Use3D
                    : DirectX::SoundEffectInstance_Default);
        }
        ApplyProperties();
        if (m_oneShotRequested)
        {
            PlayOneShot();
        }
        if (m_playRequested)
        {
            Play();
        }
    }

    void AudioSourceComponent::ApplyProperties()
    {
        m_lastBusVolume = m_audio != nullptr
            ? m_audio->BusVolume(m_bus)
            : 1.0f;
        if (m_stream)
        {
            m_stream->SetVolume(EffectiveVolume());
            m_stream->SetPitch(m_pitch);
            m_stream->SetPan(m_pan);
            m_stream->SetLoop(m_loop);
            m_stream->SetStartFrame(m_startFrame);
            m_stream->SetLevelMeterEnabled(m_levelMeterEnabled);
            m_stream->SetBassBoost(m_bassBoostDb, m_bassCornerHz);
            if (m_hasLoopRegion)
            {
                m_stream->SetLoopRegionFrames(
                    m_loopStartFrame,
                    m_loopEndFrame,
                    m_loopCrossfadeFrames);
            }
            else
            {
                m_stream->ClearLoopRegion();
            }
            return;
        }
        if (!m_instance)
        {
            return;
        }
        m_instance->SetVolume(EffectiveVolume());
        m_instance->SetPitch(m_pitch);
        if (m_spatial)
        {
            ApplySpatial(*m_instance);
        }
        else
        {
            m_instance->SetPan(m_pan);
        }
    }

    void AudioSourceComponent::ApplySpatial(
        DirectX::SoundEffectInstance& instance)
    {
        if (m_audio == nullptr)
        {
            return;
        }

        const auto* listener = m_audio->ActiveListener();
        if (listener == nullptr)
        {
            instance.SetVolume(0.0f);
            return;
        }

        using namespace DirectX;
        const XMMATRIX world = Owner().WorldMatrix();
        m_emitter.SetPosition(world.r[3]);
        m_emitter.SetOrientation(
            XMVector3Normalize(XMVectorNegate(world.r[2])),
            XMVector3Normalize(world.r[1]));

        const unsigned int channels =
            instance.GetChannelCount();
        if (channels > 1)
        {
            m_emitter.EnableDefaultMultiChannel(channels);
        }
        else
        {
            m_emitter.ChannelCount = 1;
            m_emitter.ChannelRadius = 1.0f;
            m_emitter.pChannelAzimuths =
                m_emitter.EmitterAzimuths;
        }
        UpdateDistanceCurve();

        instance.SetVolume(m_volume);
        instance.Apply3D(
            listener->BuildListener(),
            m_emitter,
            true);
    }

    void AudioSourceComponent::UpdateDistanceCurve() noexcept
    {
        const float minimumRatio = std::clamp(
            m_minimumDistance / m_maximumDistance,
            0.0f,
            0.999f);
        m_distanceCurvePoints[0] = { 0.0f, 1.0f };
        m_distanceCurvePoints[1] = {
            minimumRatio,
            1.0f
        };
        m_distanceCurvePoints[2] = { 1.0f, 0.0f };
        m_distanceCurve = {
            m_distanceCurvePoints,
            3
        };
        m_emitter.pVolumeCurve = &m_distanceCurve;
        m_emitter.CurveDistanceScaler =
            m_maximumDistance;
        m_emitter.DopplerScaler = 0.0f;
    }
}
