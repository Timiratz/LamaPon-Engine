#include "LamaPon/Audio/AudioSystem.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/AudioListenerComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Scene/GameObject.h"

#include <stb_vorbis.c>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
    // 帯域レベルメーターの調整値。ここを触るのはメーターの見え方を
    // 変えたいときだけで、再生そのものには影響しません。
    constexpr float LevelPi = 3.14159265358979323846f;
    constexpr float LevelLowestHz = 55.0f;
    constexpr float LevelHighestHz = 12500.0f;
    constexpr float LevelBandQ = 2.6f;
    // 包絡の落ち時間。短いと震え、長いと固まって見えます。
    constexpr float LevelReleaseSeconds = 0.11f;
    // 自動利得の基準ピークを忘れるまでの時間。
    constexpr float LevelPeakSeconds = 6.0f;
    // これ未満は無音として棒を寝かせます。
    constexpr float LevelSilenceFloor = 2.0e-3f;
    // 解析結果を積む間隔（Hz）と保存本数。先読みぶん（最大で
    // 約1.2秒）を必ず覆える長さにしてあります。
    constexpr float LevelHopHz = 90.0f;
    constexpr std::size_t LevelRingCapacity = 512;

    // low shelfの傾き。1.0で素直な棚（RBJのcookbookのS=1）。
    constexpr float BassShelfSlope = 1.0f;
}

namespace
{
    struct DecodedAudio final
    {
        std::unique_ptr<std::uint8_t[]> storage;
        std::size_t formatSize{sizeof(WAVEFORMATEX)};
        std::size_t byteCount{};

        [[nodiscard]] const WAVEFORMATEX* Format() const noexcept
        {
            return reinterpret_cast<const WAVEFORMATEX*>(
                storage.get());
        }

        [[nodiscard]] const std::uint8_t* Samples() const noexcept
        {
            return storage.get() + formatSize;
        }
    };

    [[nodiscard]] DecodedAudio DecodeWav(
        const std::vector<std::uint8_t>& source,
        const std::filesystem::path& path)
    {
        if (source.size() < 12
            || std::memcmp(source.data(), "RIFF", 4) != 0
            || std::memcmp(source.data() + 8, "WAVE", 4) != 0)
        {
            throw std::runtime_error(
                "Not a valid WAV file: "
                + LamaPon::PathToUtf8(path));
        }

        std::vector<std::uint8_t> fmtChunk;
        std::size_t dataOffset{};
        std::size_t dataSize{};
        std::size_t offset = 12;
        while (offset + 8 <= source.size())
        {
            std::uint32_t chunkSize{};
            std::memcpy(
                &chunkSize,
                source.data() + offset + 4,
                sizeof(chunkSize));
            const std::size_t chunkDataOffset = offset + 8;
            if (chunkDataOffset + chunkSize > source.size())
            {
                break;
            }
            if (std::memcmp(
                    source.data() + offset,
                    "fmt ",
                    4) == 0)
            {
                fmtChunk.assign(
                    source.begin() + static_cast<std::ptrdiff_t>(chunkDataOffset),
                    source.begin() + static_cast<std::ptrdiff_t>(chunkDataOffset + chunkSize));
            }
            else if (std::memcmp(
                         source.data() + offset,
                         "data",
                         4) == 0)
            {
                dataOffset = chunkDataOffset;
                dataSize = chunkSize;
            }
            offset = chunkDataOffset + chunkSize + (chunkSize % 2);
        }

        if (fmtChunk.size() < 16 || dataSize == 0)
        {
            throw std::runtime_error(
                "Unsupported or malformed WAV file: "
                + LamaPon::PathToUtf8(path));
        }

        DecodedAudio result;
        result.formatSize = std::max<std::size_t>(
            sizeof(WAVEFORMATEX),
            fmtChunk.size());
        result.byteCount = dataSize;
        result.storage = std::make_unique<std::uint8_t[]>(
            result.formatSize + result.byteCount);
        std::memset(result.storage.get(), 0, result.formatSize);
        std::memcpy(
            result.storage.get(),
            fmtChunk.data(),
            fmtChunk.size());
        std::memcpy(
            result.storage.get() + result.formatSize,
            source.data() + dataOffset,
            result.byteCount);
        return result;
    }

    [[nodiscard]] DecodedAudio DecodeOggVorbis(
        const std::vector<std::uint8_t>& source,
        const std::filesystem::path& path)
    {
        if (source.size() > std::numeric_limits<int>::max())
        {
            throw std::runtime_error(
                "OGG audio file has an unsupported size: "
                + LamaPon::PathToUtf8(path));
        }
        int channels{};
        int sampleRate{};
        short* samples{};
        const int samplesPerChannel = stb_vorbis_decode_memory(
            source.data(),
            static_cast<int>(source.size()),
            &channels,
            &sampleRate,
            &samples);
        std::unique_ptr<short, decltype(&std::free)>
            sampleGuard(samples, &std::free);

        if (samplesPerChannel <= 0
            || samples == nullptr
            || channels <= 0
            || channels > 8
            || sampleRate <= 0)
        {
            throw std::runtime_error(
                "The built-in Vorbis decoder could not decode: "
                + LamaPon::PathToUtf8(path));
        }

        constexpr std::size_t bytesPerSample = sizeof(short);
        const auto sampleCount =
            static_cast<std::size_t>(samplesPerChannel)
            * static_cast<std::size_t>(channels);
        if (sampleCount
            > std::numeric_limits<std::size_t>::max()
                / bytesPerSample)
        {
            throw std::runtime_error(
                "Decoded OGG audio is too large: "
                + LamaPon::PathToUtf8(path));
        }

        DecodedAudio result;
        result.byteCount = sampleCount * bytesPerSample;
        result.storage = std::make_unique<std::uint8_t[]>(
            sizeof(WAVEFORMATEX) + result.byteCount);
        auto* format = reinterpret_cast<WAVEFORMATEX*>(
            result.storage.get());
        *format = {};
        format->wFormatTag = WAVE_FORMAT_PCM;
        format->nChannels = static_cast<WORD>(channels);
        format->nSamplesPerSec =
            static_cast<DWORD>(sampleRate);
        format->wBitsPerSample = 16;
        format->nBlockAlign = static_cast<WORD>(
            channels * static_cast<int>(bytesPerSample));
        format->nAvgBytesPerSec =
            format->nSamplesPerSec * format->nBlockAlign;
        format->cbSize = 0;
        std::memcpy(
            result.storage.get() + sizeof(WAVEFORMATEX),
            samples,
            result.byteCount);
        return result;
    }
}

namespace LamaPon
{
    AudioSystem::AudioSystem()
        : m_engine(
            std::make_unique<DirectX::AudioEngine>(
                DirectX::AudioEngine_Default))
    {
    }

    AudioSystem::~AudioSystem() = default;

    std::shared_ptr<DirectX::SoundEffect>
        AudioSystem::LoadSoundEffect(
            AssetManager& assets,
            const std::filesystem::path& path)
    {
        const auto& resolvedPath = path;
        if (!assets.FileExists(resolvedPath))
        {
            throw std::runtime_error(
                "Audio file was not found: "
                + PathToUtf8(path));
        }
        auto extension = resolvedPath.extension().wstring();
        std::ranges::transform(
            extension,
            extension.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });
        if (extension != L".wav"
            && extension != L".ogg")
        {
            throw std::runtime_error(
                "AudioSource supports WAV and OGG files: "
                + PathToUtf8(path));
        }

        const std::wstring key = MakeCacheKey(resolvedPath);
        if (const auto existing = m_soundCache.find(key);
            existing != m_soundCache.end())
        {
            return existing->second;
        }

        const auto fileBytes = assets.ReadFileBytes(resolvedPath);
        std::shared_ptr<DirectX::SoundEffect> sound;
        if (extension == L".wav")
        {
            auto decoded = DecodeWav(fileBytes, resolvedPath);
            const auto* sampleBytes = decoded.Samples();
            sound = std::make_shared<DirectX::SoundEffect>(
                m_engine.get(),
                decoded.storage,
                decoded.Format(),
                sampleBytes,
                decoded.byteCount);
        }
        else
        {
            auto decoded = DecodeOggVorbis(fileBytes, resolvedPath);
            const auto decodedByteCount = decoded.byteCount;
            const auto* sampleBytes = decoded.Samples();
            sound = std::make_shared<DirectX::SoundEffect>(
                m_engine.get(),
                decoded.storage,
                decoded.Format(),
                sampleBytes,
                decoded.byteCount);
            Logger::Instance().Info(
                "Decoded OGG audio with LamaPon's built-in Vorbis decoder: "
                + PathToUtf8(resolvedPath)
                + " ("
                + std::to_string(decodedByteCount)
                + " PCM bytes)");
        }
        m_soundCache.emplace(key, sound);
        return sound;
    }

    bool AudioSystem::Update()
    {
        if (m_engine->Update())
        {
            return true;
        }
        if (m_engine->IsCriticalError())
        {
            const bool wasReset = m_engine->Reset();
            if (wasReset)
            {
                // 既定の再生デバイスを切り替えた場合など、デバイスが
                // リセットされると、既存のSoundEffectInstanceが持つ
                // XAudio2ボイスはすべて無効になります。世代番号を進め、
                // AudioSourceComponentが古いボイスを呼び出さずに
                // 作り直せるようにします。
                ++m_deviceGeneration;
            }
            return wasReset;
        }
        return false;
    }

    void AudioSystem::Clear()
    {
        m_soundCache.clear();
        m_engine->TrimVoicePool();
    }

    void AudioSystem::SetSuspended(const bool suspended)
    {
        if (m_suspended == suspended)
        {
            return;
        }
        m_suspended = suspended;
        if (m_engine == nullptr)
        {
            return;
        }
        if (m_suspended)
        {
            m_engine->Suspend();
            return;
        }
        // Resumeはデバイス消失時に例外を投げることがあります。
        // 一時停止の解除でエディターを落としたくないので、失敗しても
        // 次のUpdateのリセット処理に任せます。
        try
        {
            m_engine->Resume();
        }
        catch (const std::exception&)
        {
            m_suspended = false;
        }
    }

    void AudioSystem::SetMasterVolume(const float volume)
    {
        m_engine->SetMasterVolume(
            std::clamp(volume, 0.0f, 1.0f));
    }

    float AudioSystem::MasterVolume() const noexcept
    {
        return m_engine->GetMasterVolume();
    }

    void AudioSystem::SetBusVolume(
        const AudioBus bus,
        const float volume)
    {
        const auto index = static_cast<std::size_t>(bus);
        if (index < m_busVolumes.size())
        {
            m_busVolumes[index] =
                std::clamp(volume, 0.0f, 1.0f);
        }
    }

    float AudioSystem::BusVolume(
        const AudioBus bus) const noexcept
    {
        const auto index = static_cast<std::size_t>(bus);
        return index < m_busVolumes.size()
            ? m_busVolumes[index]
            : 1.0f;
    }

    std::shared_ptr<AudioStreamVoice>
        AudioSystem::CreateStream(
            AssetManager& assets,
            const std::filesystem::path& path)
    {
        if (!assets.FileExists(path))
        {
            throw std::runtime_error(
                "Audio file was not found: "
                + PathToUtf8(path));
        }
        auto extension = path.extension().wstring();
        std::ranges::transform(
            extension,
            extension.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });

        // make_sharedはprivateコンストラクタを呼べないため
        // newで生成します。
        std::shared_ptr<AudioStreamVoice> stream(
            new AudioStreamVoice());
        stream->m_sourceBytes =
            assets.ReadFileBytes(path);

        if (extension == L".ogg")
        {
            int error{};
            auto* vorbis = stb_vorbis_open_memory(
                stream->m_sourceBytes.data(),
                static_cast<int>(
                    stream->m_sourceBytes.size()),
                &error,
                nullptr);
            if (vorbis == nullptr)
            {
                throw std::runtime_error(
                    "The built-in Vorbis decoder could not open: "
                    + PathToUtf8(path));
            }
            const auto info =
                stb_vorbis_get_info(vorbis);
            stream->m_vorbis = vorbis;
            stream->m_isVorbis = true;
            stream->m_channels = info.channels;
            stream->m_sampleRate =
                static_cast<int>(info.sample_rate);
            stream->m_totalFrames =
                stb_vorbis_stream_length_in_samples(vorbis);
        }
        else if (extension == L".wav")
        {
            // PCM16のWAVのみストリーミング対象にします。
            const auto decodedHeader = DecodeWav(
                stream->m_sourceBytes,
                path);
            const auto* format = decodedHeader.Format();
            if (format->wFormatTag != WAVE_FORMAT_PCM
                || format->wBitsPerSample != 16)
            {
                throw std::runtime_error(
                    "Streaming supports 16-bit PCM WAV only: "
                    + PathToUtf8(path));
            }
            stream->m_channels = format->nChannels;
            stream->m_sampleRate =
                static_cast<int>(
                    format->nSamplesPerSec);
            // dataチャンクの位置を元バイト列から求め直します。
            std::size_t offset = 12;
            while (offset + 8
                <= stream->m_sourceBytes.size())
            {
                std::uint32_t chunkSize{};
                std::memcpy(
                    &chunkSize,
                    stream->m_sourceBytes.data()
                        + offset + 4,
                    sizeof(chunkSize));
                const std::size_t chunkDataOffset =
                    offset + 8;
                if (chunkDataOffset + chunkSize
                    > stream->m_sourceBytes.size())
                {
                    break;
                }
                if (std::memcmp(
                        stream->m_sourceBytes.data()
                            + offset,
                        "data",
                        4) == 0)
                {
                    stream->m_dataOffset =
                        chunkDataOffset;
                    stream->m_dataSize = chunkSize;
                    break;
                }
                offset = chunkDataOffset + chunkSize
                    + (chunkSize % 2);
            }
            const auto bytesPerFrame =
                static_cast<std::uint64_t>(stream->m_channels)
                * sizeof(std::int16_t);
            if (bytesPerFrame > 0)
            {
                stream->m_totalFrames =
                    stream->m_dataSize / bytesPerFrame;
            }
        }
        else
        {
            throw std::runtime_error(
                "Streaming supports WAV and OGG files: "
                + PathToUtf8(path));
        }

        if (stream->m_channels <= 0
            || stream->m_channels > 8
            || stream->m_sampleRate <= 0
            || stream->m_totalFrames == 0)
        {
            throw std::runtime_error(
                "Unsupported audio format for streaming: "
                + PathToUtf8(path));
        }

        auto* voicePointer = stream.get();
        stream->m_instance = std::make_unique<
            DirectX::DynamicSoundEffectInstance>(
            m_engine.get(),
            [voicePointer](
                DirectX::DynamicSoundEffectInstance*
                    instance)
            {
                if (instance != nullptr)
                {
                    voicePointer->FeedBuffer(*instance);
                }
            },
            stream->m_sampleRate,
            stream->m_channels,
            16);
        return stream;
    }

    bool AudioSystem::IsDevicePresent() const noexcept
    {
        return m_engine->IsAudioDevicePresent();
    }

    DirectX::AudioStatistics AudioSystem::Statistics() const
    {
        return m_engine->GetStatistics();
    }

    void AudioSystem::RegisterListener(
        AudioListenerComponent& listener)
    {
        if (std::ranges::find(m_listeners, &listener)
            == m_listeners.end())
        {
            m_listeners.push_back(&listener);
        }
    }

    void AudioSystem::UnregisterListener(
        AudioListenerComponent& listener) noexcept
    {
        std::erase(m_listeners, &listener);
    }

    AudioListenerComponent*
        AudioSystem::ActiveListener() const noexcept
    {
        for (auto* listener : m_listeners)
        {
            if (listener != nullptr
                && listener->IsEnabled()
                && listener->Owner().IsActiveInHierarchy())
            {
                return listener;
            }
        }
        return nullptr;
    }

    std::wstring AudioSystem::MakeCacheKey(
        const std::filesystem::path& path)
    {
        std::wstring key = path.native();
        std::ranges::transform(
            key,
            key.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });
        return key;
    }
}

namespace LamaPon
{
    AudioStreamVoice::~AudioStreamVoice()
    {
        Stop();
        m_instance.reset();
        if (m_vorbis != nullptr)
        {
            stb_vorbis_close(
                static_cast<stb_vorbis*>(m_vorbis));
            m_vorbis = nullptr;
        }
    }

    void AudioStreamVoice::Play()
    {
        if (!m_instance)
        {
            return;
        }
        // 任意ループ区間があっても、イントロを一度鳴らすためPlayは
        // 必ず開始位置（既定は先頭）から再生し直します。
        m_playRequested = false;
        m_instance->Stop();
        m_finished = false;
        m_nextBuffer = 0;
        m_crossfadeActive = false;
        m_crossfadeProgressFrames = 0;
        m_completedLoopCount = 0;
        // 再生位置の推定をやり直します。ここで基準時刻を置かないと、
        // 最初のbuffer完了までの間だけ位置が進みません。
        m_submittedFrames = 0;
        m_consumedFrames = 0;
        m_queuedHead = 0;
        m_queuedCount = 0;
        m_queuedFrames.fill(0);
        m_positionNext = 0;
        m_positionCount = 0;
        m_consumedAt = std::chrono::steady_clock::now();
        m_bassState = {};
        ResetLevelMeter();
        if (!SeekFrame(m_startFrame))
        {
            m_finished = true;
            return;
        }
        m_playRequested = true;
        m_instance->Play();
    }

    float AudioStreamVoice::BassBoostMakeup() const noexcept
    {
        return m_bassBoostDb > 0.0f
            ? std::pow(10.0f, m_bassBoostDb / 20.0f)
            : 1.0f;
    }

    void AudioStreamVoice::SetBassBoost(
        const float gainDb, const float cornerHz)
    {
        m_bassBoostDb = std::max(gainDb, 0.0f);
        m_bassCornerHz = std::max(cornerHz, 20.0f);
        m_bassState = {};
        ApplyVoiceVolume();
        if (m_bassBoostDb <= 0.0f || m_sampleRate <= 0)
        {
            // 素通し。以降ApplyBassBoostは何もしません。
            m_bassB0 = 1.0f;
            m_bassB1 = 0.0f;
            m_bassB2 = 0.0f;
            m_bassA1 = 0.0f;
            m_bassA2 = 0.0f;
            return;
        }

        // RBJ cookbookのlow shelf。Aは「棚の高さの平方根」で、
        // 直流での利得はA^2＝10^(dB/20)になります。
        const auto sampleRate = static_cast<float>(m_sampleRate);
        const float a = std::pow(10.0f, m_bassBoostDb / 40.0f);
        const float w0 = 2.0f * LevelPi
            * std::min(m_bassCornerHz, sampleRate * 0.45f) / sampleRate;
        const float cosW0 = std::cos(w0);
        const float alpha = std::sin(w0) * 0.5f
            * std::sqrt((a + 1.0f / a) * (1.0f / BassShelfSlope - 1.0f)
                        + 2.0f);
        const float beta = 2.0f * std::sqrt(a) * alpha;

        const float b0 = a * ((a + 1.0f) - (a - 1.0f) * cosW0 + beta);
        const float b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cosW0);
        const float b2 = a * ((a + 1.0f) - (a - 1.0f) * cosW0 - beta);
        const float a0 = (a + 1.0f) + (a - 1.0f) * cosW0 + beta;
        const float a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cosW0);
        const float a2 = (a + 1.0f) + (a - 1.0f) * cosW0 - beta;

        // 出力全体をgainDbぶん下げてからa0で正規化します。こうすると
        // 直流利得が1.0になり、int16へ戻すときに振り切れません。
        const float trim = 1.0f / (a * a);
        m_bassB0 = b0 * trim / a0;
        m_bassB1 = b1 * trim / a0;
        m_bassB2 = b2 * trim / a0;
        m_bassA1 = a1 / a0;
        m_bassA2 = a2 / a0;
    }

    void AudioStreamVoice::ApplyBassBoost(
        std::uint8_t* pcm, const std::size_t bytes) noexcept
    {
        if (m_bassBoostDb <= 0.0f
            || pcm == nullptr
            || m_channels <= 0
            || static_cast<std::size_t>(m_channels) > MaximumChannels)
        {
            return;
        }
        const auto channels = static_cast<std::size_t>(m_channels);
        const std::size_t frames =
            bytes / (channels * sizeof(std::int16_t));
        auto* samples = reinterpret_cast<std::int16_t*>(pcm);

        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                auto& state = m_bassState[channel];
                const auto index = frame * channels + channel;
                const float input = static_cast<float>(samples[index]);
                // Direct Form I。state = {x1, x2, y1, y2}
                const float output =
                    m_bassB0 * input
                    + m_bassB1 * state[0]
                    + m_bassB2 * state[1]
                    - m_bassA1 * state[2]
                    - m_bassA2 * state[3];
                state[1] = state[0];
                state[0] = input;
                state[3] = state[2];
                state[2] = output;
                samples[index] = static_cast<std::int16_t>(std::clamp(
                    std::lround(output),
                    static_cast<long>(
                        std::numeric_limits<std::int16_t>::min()),
                    static_cast<long>(
                        std::numeric_limits<std::int16_t>::max())));
            }
        }
    }

    void AudioStreamVoice::SetStartFrame(
        const std::uint64_t frame) noexcept
    {
        // 音源の外を指されたら先頭へ戻します。ここで弾いておかないと
        // Playのseekが必ず失敗して「無音のまま鳴らない」になります。
        m_startFrame = frame < m_totalFrames ? frame : 0;
    }

    void AudioStreamVoice::SetLevelMeterEnabled(const bool enabled)
    {
        if (m_levelMeterEnabled == enabled)
        {
            return;
        }
        m_levelMeterEnabled = enabled;
        if (!enabled)
        {
            // 使わないときはringも持ちません（確保も解析も0）。
            m_levelRing.clear();
            m_levelRing.shrink_to_fit();
            m_levelRingNext = 0;
            m_levelRingCount = 0;
            return;
        }
        m_levelRing.assign(LevelRingCapacity, LevelSample{});
        ResetLevelMeter();
    }

    void AudioStreamVoice::ResetLevelMeter() noexcept
    {
        m_levelRingNext = 0;
        m_levelRingCount = 0;
        m_levelHopCursor = 0;
        m_bandIc1.fill(0.0f);
        m_bandIc2.fill(0.0f);
        m_bandEnvelope.fill(0.0f);
        m_bandPeak.fill(0.0f);
        if (!m_levelMeterEnabled || m_sampleRate <= 0)
        {
            return;
        }

        const auto sampleRate = static_cast<float>(m_sampleRate);
        // 55Hzからナイキスト手前までを等比で12分割します。
        const float lowest = LevelLowestHz;
        const float highest = std::min(
            LevelHighestHz,
            sampleRate * 0.42f);
        const float ratio = highest > lowest
            ? std::pow(
                highest / lowest,
                1.0f / static_cast<float>(LevelBandCount - 1))
            : 1.0f;
        float center = lowest;
        for (int band = 0; band < LevelBandCount; ++band)
        {
            const auto index = static_cast<std::size_t>(band);
            // TPT（台形積分）型のstate-variable filter。双一次変換の
            // 素朴な形と違い中心周波数がナイキストへ寄っても発散
            // しないので、最高帯域まで同じ式で置けます。
            const float g = std::tan(
                LevelPi * std::min(center, sampleRate * 0.45f)
                    / sampleRate);
            const float k = 1.0f / LevelBandQ;
            const float a1 = 1.0f / (1.0f + g * (g + k));
            m_bandA1[index] = a1;
            m_bandA2[index] = g * a1;
            m_bandA3[index] = g * m_bandA2[index];
            center *= ratio;
        }
        m_levelReleaseCoefficient = std::exp(
            -1.0f / (LevelReleaseSeconds * sampleRate));
        m_levelPeakDecay = std::exp(
            -1.0f / (LevelPeakSeconds * sampleRate));
        m_levelHopFrames = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(sampleRate / LevelHopHz));
    }

    void AudioStreamVoice::AnalyzeSubmittedPcm(
        const std::uint8_t* pcm,
        const std::size_t bytes,
        const std::uint64_t streamFrameStart) noexcept
    {
        if (!m_levelMeterEnabled
            || pcm == nullptr
            || m_channels <= 0
            || m_levelRing.empty())
        {
            return;
        }
        const auto channels = static_cast<std::size_t>(m_channels);
        const std::size_t bytesPerFrame =
            channels * sizeof(std::int16_t);
        const std::size_t frames = bytes / bytesPerFrame;
        const auto* samples =
            reinterpret_cast<const std::int16_t*>(pcm);
        const float monoScale =
            1.0f / (32768.0f * static_cast<float>(channels));

        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            float mono = 0.0f;
            for (std::size_t channel = 0;
                 channel < channels;
                 ++channel)
            {
                mono += static_cast<float>(
                    samples[frame * channels + channel]);
            }
            mono *= monoScale;

            for (int band = 0; band < LevelBandCount; ++band)
            {
                const auto index = static_cast<std::size_t>(band);
                const float v3 = mono - m_bandIc2[index];
                const float v1 = m_bandA1[index] * m_bandIc1[index]
                    + m_bandA2[index] * v3;
                const float v2 = m_bandIc2[index]
                    + m_bandA2[index] * m_bandIc1[index]
                    + m_bandA3[index] * v3;
                m_bandIc1[index] = 2.0f * v1 - m_bandIc1[index];
                m_bandIc2[index] = 2.0f * v2 - m_bandIc2[index];
                // v1がbandpass出力。立ち上がりは即座、落ちは緩やかに
                // してアナライザらしい動きにします。
                m_bandEnvelope[index] = std::max(
                    std::fabs(v1),
                    m_bandEnvelope[index]
                        * m_levelReleaseCoefficient);
                m_bandPeak[index] = std::max(
                    m_bandEnvelope[index],
                    m_bandPeak[index] * m_levelPeakDecay);
            }

            if (++m_levelHopCursor < m_levelHopFrames)
            {
                continue;
            }
            m_levelHopCursor = 0;

            LevelSample sample{};
            sample.streamFrame = streamFrameStart
                + static_cast<std::uint64_t>(frame) + 1;
            for (int band = 0; band < LevelBandCount; ++band)
            {
                const auto index = static_cast<std::size_t>(band);
                // 帯域ごとの自動利得。低域と高域では桁が違うので、
                // 生の値のままだと高域の棒がほとんど動きません。
                const float peak = m_bandPeak[index];
                const float level = peak > LevelSilenceFloor
                    ? std::clamp(
                        m_bandEnvelope[index] / peak, 0.0f, 1.0f)
                    : 0.0f;
                sample.bands[index] = std::pow(level, 0.6f);
            }
            m_levelRing[m_levelRingNext] = sample;
            m_levelRingNext =
                (m_levelRingNext + 1) % m_levelRing.size();
            if (m_levelRingCount < m_levelRing.size())
            {
                ++m_levelRingCount;
            }
        }
    }

    std::uint64_t
        AudioStreamVoice::PlayedStreamFrame() const noexcept
    {
        if (m_sampleRate <= 0)
        {
            return m_consumedFrames;
        }
        // buffer完了callbackの瞬間はm_consumedFramesが実際の再生位置と
        // 一致します。その間だけ実時間で補間し、送信済みを超えない
        // ように抑えます（毎フレームの更新処理は要りません）。
        const auto elapsed =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_consumedAt)
            .count();
        const double advanced = std::max(elapsed, 0.0)
            * static_cast<double>(m_sampleRate);
        const double played =
            static_cast<double>(m_consumedFrames) + advanced;
        return static_cast<std::uint64_t>(std::min(
            played,
            static_cast<double>(m_submittedFrames)));
    }

    std::size_t AudioStreamVoice::ReadLevelBands(
        float* destination,
        const std::size_t capacity) const noexcept
    {
        if (destination == nullptr || capacity == 0)
        {
            return 0;
        }
        const std::size_t count = std::min(
            capacity,
            static_cast<std::size_t>(LevelBandCount));
        for (std::size_t index = 0; index < count; ++index)
        {
            destination[index] = 0.0f;
        }
        if (!m_levelMeterEnabled
            || m_levelRingCount == 0
            || State() != DirectX::PLAYING)
        {
            return count;
        }

        const std::uint64_t played = PlayedStreamFrame();
        const std::size_t size = m_levelRing.size();
        const std::size_t oldest =
            (m_levelRingNext + size - m_levelRingCount) % size;
        const LevelSample* chosen = &m_levelRing[oldest];
        for (std::size_t step = 0; step < m_levelRingCount; ++step)
        {
            const LevelSample& sample =
                m_levelRing[(oldest + step) % size];
            if (sample.streamFrame > played)
            {
                break;
            }
            chosen = &sample;
        }
        for (std::size_t index = 0; index < count; ++index)
        {
            destination[index] = chosen->bands[index];
        }
        return count;
    }

    std::uint64_t AudioStreamVoice::PlaybackFrame() const noexcept
    {
        if (m_positionCount == 0)
        {
            return m_startFrame;
        }
        const std::uint64_t played = PlayedStreamFrame();
        const std::size_t size = m_positions.size();
        const std::size_t oldest =
            (m_positionNext + size - m_positionCount) % size;
        const PositionMarker* chosen = &m_positions[oldest];
        for (std::size_t step = 0; step < m_positionCount; ++step)
        {
            const PositionMarker& marker =
                m_positions[(oldest + step) % size];
            if (marker.streamFrame > played)
            {
                break;
            }
            chosen = &marker;
        }
        // 目印からの差分をそのまま足します。目印はdecodeのたびに
        // 積むので、ループの折り返しをまたぐことはありません。
        const std::uint64_t advanced = played > chosen->streamFrame
            ? played - chosen->streamFrame
            : 0;
        const std::uint64_t frame = chosen->fileFrame + advanced;
        return std::min(frame, m_totalFrames);
    }

    std::size_t AudioStreamVoice::ReadPeakEnvelope(
        float* destination,
        const std::size_t bucketCount)
    {
        if (destination == nullptr
            || bucketCount == 0
            || m_channels <= 0
            || m_totalFrames == 0)
        {
            return 0;
        }
        for (std::size_t index = 0; index < bucketCount; ++index)
        {
            destination[index] = 0.0f;
        }

        // 読み終えたら必ず元の位置へ戻します。ここを忘れると、
        // 波形を描いただけで再生位置が飛びます。
        const std::uint64_t restore = m_frameCursor;
        if (!SeekFrame(0))
        {
            return 0;
        }

        const auto channels = static_cast<std::size_t>(m_channels);
        const std::size_t bytesPerFrame =
            channels * sizeof(std::int16_t);
        std::vector<std::uint8_t> block(16384 * bytesPerFrame);
        std::uint64_t frame{};
        while (frame < m_totalFrames)
        {
            const std::size_t bytes = DecodeRawFrames(
                block.data(),
                std::min<std::uint64_t>(
                    16384, m_totalFrames - frame));
            if (bytes == 0)
            {
                break;
            }
            const std::size_t frames = bytes / bytesPerFrame;
            const auto* samples =
                reinterpret_cast<const std::int16_t*>(block.data());
            for (std::size_t offset = 0; offset < frames; ++offset)
            {
                const std::size_t bucket = static_cast<std::size_t>(
                    (frame + offset) * bucketCount / m_totalFrames);
                if (bucket >= bucketCount)
                {
                    continue;
                }
                for (std::size_t channel = 0;
                     channel < channels;
                     ++channel)
                {
                    const float level =
                        std::fabs(static_cast<float>(
                            samples[offset * channels + channel]))
                        / 32768.0f;
                    destination[bucket] =
                        std::max(destination[bucket], level);
                }
            }
            frame += frames;
        }

        static_cast<void>(SeekFrame(restore));
        return bucketCount;
    }

    void AudioStreamVoice::SetLoopRegionFrames(
        const std::uint64_t startFrame,
        const std::uint64_t endFrame,
        const std::uint64_t crossfadeFrames) noexcept
    {
        if (startFrame >= endFrame
            || endFrame > m_totalFrames)
        {
            ClearLoopRegion();
            return;
        }
        m_loopStartFrame = startFrame;
        m_loopEndFrame = endFrame;
        // tailとheadが互いに重ならない範囲に抑える。
        m_loopCrossfadeFrames = std::min(
            crossfadeFrames,
            (endFrame - startFrame) / 2);
        m_hasLoopRegion = true;
        m_crossfadeActive = false;
        m_crossfadeProgressFrames = 0;
        if (m_loopCrossfadeFrames > 0
            && !PrepareLoopHead())
        {
            // headを用意できなくても区間ループ自体は維持する。
            m_loopCrossfadeFrames = 0;
        }
    }

    void AudioStreamVoice::ClearLoopRegion() noexcept
    {
        m_loopStartFrame = 0;
        m_loopEndFrame = 0;
        m_loopCrossfadeFrames = 0;
        m_crossfadeProgressFrames = 0;
        m_hasLoopRegion = false;
        m_crossfadeActive = false;
        m_loopHeadPcm.clear();
    }

    void AudioStreamVoice::Pause() noexcept
    {
        if (m_instance)
        {
            m_instance->Pause();
        }
    }

    void AudioStreamVoice::Resume()
    {
        if (m_instance)
        {
            m_instance->Resume();
        }
    }

    void AudioStreamVoice::Stop() noexcept
    {
        m_playRequested = false;
        if (m_instance)
        {
            m_instance->Stop();
        }
    }

    void AudioStreamVoice::SetVolume(const float volume)
    {
        // 1.0を超える増幅を許します（上限4.0）。小さい音源を
        // 持ち上げるのに要り、低音補正の戻しでも使います。
        m_volume = std::clamp(volume, 0.0f, 4.0f);
        ApplyVoiceVolume();
    }

    void AudioStreamVoice::ApplyVoiceVolume()
    {
        if (!m_instance)
        {
            return;
        }
        // 低音補正で下げたぶんをここで戻します。1.0を超えますが、
        // PCM側を同じだけ下げてあるので元の振幅は超えません。
        m_instance->SetVolume(std::clamp(
            m_volume * BassBoostMakeup(), 0.0f, 4.0f));
    }

    void AudioStreamVoice::SetPitch(const float pitch)
    {
        if (m_instance)
        {
            m_instance->SetPitch(
                std::clamp(pitch, -1.0f, 1.0f));
        }
    }

    void AudioStreamVoice::SetPan(const float pan)
    {
        if (m_instance)
        {
            m_instance->SetPan(
                std::clamp(pan, -1.0f, 1.0f));
        }
    }

    DirectX::SoundState
        AudioStreamVoice::State() const noexcept
    {
        return m_instance
            ? m_instance->GetState()
            : DirectX::STOPPED;
    }

    std::size_t AudioStreamVoice::DecodeChunk(
        std::uint8_t* destination,
        const std::size_t capacity)
    {
        const std::size_t bytesPerFrame =
            static_cast<std::size_t>(m_channels)
            * sizeof(std::int16_t);
        if (destination == nullptr
            || bytesPerFrame == 0
            || capacity < bytesPerFrame)
        {
            return 0;
        }
        const auto capacityFrames = static_cast<std::uint64_t>(
            capacity / bytesPerFrame);
        const bool crossfadeEnabled =
            m_loop
            && m_hasLoopRegion
            && m_loopCrossfadeFrames > 0
            && m_loopHeadPcm.size()
                == m_loopCrossfadeFrames
                    * static_cast<std::uint64_t>(m_channels);
        const std::uint64_t crossfadeStart = crossfadeEnabled
            ? m_loopEndFrame - m_loopCrossfadeFrames
            : 0;

        if (!crossfadeEnabled)
        {
            m_crossfadeActive = false;
            m_crossfadeProgressFrames = 0;
        }
        if (crossfadeEnabled
            && !m_crossfadeActive
            && m_frameCursor >= crossfadeStart
            && m_frameCursor < m_loopEndFrame)
        {
            m_crossfadeActive = true;
            m_crossfadeProgressFrames =
                m_frameCursor - crossfadeStart;
        }
        if (m_crossfadeActive)
        {
            const auto requestedFrames = std::min(
                capacityFrames,
                m_loopCrossfadeFrames
                    - m_crossfadeProgressFrames);
            const std::size_t bytes = DecodeRawFrames(
                destination,
                requestedFrames);
            const auto decodedFrames = static_cast<std::uint64_t>(
                bytes / bytesPerFrame);
            auto* tail = reinterpret_cast<std::int16_t*>(destination);
            for (std::uint64_t frame = 0;
                 frame < decodedFrames;
                 ++frame)
            {
                const std::uint64_t fadeFrame =
                    m_crossfadeProgressFrames + frame;
                const float headGain = m_loopCrossfadeFrames > 1
                    ? static_cast<float>(fadeFrame)
                        / static_cast<float>(
                            m_loopCrossfadeFrames - 1)
                    : 1.0f;
                const float tailGain = 1.0f - headGain;
                for (int channel = 0; channel < m_channels; ++channel)
                {
                    const auto sample = static_cast<std::size_t>(
                        frame * static_cast<std::uint64_t>(m_channels)
                        + static_cast<std::uint64_t>(channel));
                    const auto headSample = static_cast<std::size_t>(
                        fadeFrame
                            * static_cast<std::uint64_t>(m_channels)
                        + static_cast<std::uint64_t>(channel));
                    const float mixed =
                        static_cast<float>(tail[sample]) * tailGain
                        + static_cast<float>(m_loopHeadPcm[headSample])
                            * headGain;
                    tail[sample] = static_cast<std::int16_t>(std::clamp(
                        std::lround(mixed),
                        static_cast<long>(
                            std::numeric_limits<std::int16_t>::min()),
                        static_cast<long>(
                            std::numeric_limits<std::int16_t>::max())));
                }
            }
            m_crossfadeProgressFrames += decodedFrames;
            if (decodedFrames > 0
                && m_crossfadeProgressFrames
                    >= m_loopCrossfadeFrames)
            {
                m_crossfadeActive = false;
                m_crossfadeProgressFrames = 0;
                if (SeekFrame(
                        m_loopStartFrame
                        + m_loopCrossfadeFrames))
                {
                    ++m_completedLoopCount;
                }
                else
                {
                    // seekに失敗しても通常の区間ループへ退避する。
                    m_loopCrossfadeFrames = 0;
                    m_loopHeadPcm.clear();
                }
            }
            return bytes;
        }

        const std::uint64_t decodeEnd =
            crossfadeEnabled
                ? crossfadeStart
                : (m_loop && m_hasLoopRegion
                    ? m_loopEndFrame
                    : m_totalFrames);
        if (m_frameCursor >= decodeEnd)
        {
            return 0;
        }
        const auto requestedFrames = std::min(
            capacityFrames,
            decodeEnd - m_frameCursor);
        if (requestedFrames == 0)
        {
            return 0;
        }

        return DecodeRawFrames(destination, requestedFrames);
    }

    std::size_t AudioStreamVoice::DecodeRawFrames(
        std::uint8_t* destination,
        const std::uint64_t frameCount)
    {
        const std::size_t bytesPerFrame =
            static_cast<std::size_t>(m_channels)
            * sizeof(std::int16_t);
        const std::uint64_t requestedFrames = std::min(
            frameCount,
            m_totalFrames - std::min(m_frameCursor, m_totalFrames));
        if (destination == nullptr
            || bytesPerFrame == 0
            || requestedFrames == 0)
        {
            return 0;
        }
        if (m_isVorbis)
        {
            auto* vorbis =
                static_cast<stb_vorbis*>(m_vorbis);
            if (vorbis == nullptr)
            {
                return 0;
            }
            const auto requestedShorts =
                requestedFrames
                * static_cast<std::uint64_t>(m_channels);
            const int maximumShorts = static_cast<int>(std::min(
                requestedShorts,
                static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max())));
            const int samplesPerChannel =
                stb_vorbis_get_samples_short_interleaved(
                    vorbis,
                    m_channels,
                    reinterpret_cast<short*>(destination),
                    maximumShorts);
            const auto decodedFrames = static_cast<std::uint64_t>(
                std::max(samplesPerChannel, 0));
            m_frameCursor += decodedFrames;
            return static_cast<std::size_t>(decodedFrames)
                * bytesPerFrame;
        }

        // WAV（PCM16）はdataチャンクからそのまま切り出します。
        const auto frames = static_cast<std::size_t>(
            requestedFrames);
        const std::size_t bytes = frames * bytesPerFrame;
        std::memcpy(
            destination,
            m_sourceBytes.data()
                + m_dataOffset
                + m_dataCursor,
            bytes);
        m_dataCursor += bytes;
        m_frameCursor += frames;
        return bytes;
    }

    bool AudioStreamVoice::SeekFrame(
        const std::uint64_t frame) noexcept
    {
        if (frame > m_totalFrames)
        {
            return false;
        }
        if (m_isVorbis)
        {
            auto* vorbis =
                static_cast<stb_vorbis*>(m_vorbis);
            if (vorbis == nullptr
                || frame > std::numeric_limits<unsigned int>::max()
                || stb_vorbis_seek(
                       vorbis,
                       static_cast<unsigned int>(frame)) == 0)
            {
                return false;
            }
        }
        else
        {
            const auto bytesPerFrame =
                static_cast<std::uint64_t>(m_channels)
                * sizeof(std::int16_t);
            const auto byteOffset = frame * bytesPerFrame;
            if (byteOffset > m_dataSize)
            {
                return false;
            }
            m_dataCursor = static_cast<std::size_t>(byteOffset);
        }
        m_frameCursor = frame;
        return true;
    }

    bool AudioStreamVoice::PrepareLoopHead() noexcept
    {
        m_loopHeadPcm.clear();
        if (m_loopCrossfadeFrames == 0
            || m_channels <= 0)
        {
            return true;
        }
        const auto channelCount =
            static_cast<std::uint64_t>(m_channels);
        if (m_loopCrossfadeFrames
            > std::numeric_limits<std::size_t>::max() / channelCount)
        {
            return false;
        }
        const auto sampleCount = static_cast<std::size_t>(
            m_loopCrossfadeFrames * channelCount);
        try
        {
            m_loopHeadPcm.resize(sampleCount);
        }
        catch (...)
        {
            return false;
        }

        if (!m_isVorbis)
        {
            const auto bytesPerFrame = channelCount
                * sizeof(std::int16_t);
            const auto byteOffset =
                m_loopStartFrame * bytesPerFrame;
            const auto byteCount =
                m_loopCrossfadeFrames * bytesPerFrame;
            if (byteOffset > m_dataSize
                || byteCount > m_dataSize - byteOffset)
            {
                m_loopHeadPcm.clear();
                return false;
            }
            std::memcpy(
                m_loopHeadPcm.data(),
                m_sourceBytes.data()
                    + m_dataOffset
                    + static_cast<std::size_t>(byteOffset),
                static_cast<std::size_t>(byteCount));
            return true;
        }

        int error{};
        auto* headDecoder = stb_vorbis_open_memory(
            m_sourceBytes.data(),
            static_cast<int>(m_sourceBytes.size()),
            &error,
            nullptr);
        if (headDecoder == nullptr
            || m_loopStartFrame
                > std::numeric_limits<unsigned int>::max()
            || stb_vorbis_seek(
                   headDecoder,
                   static_cast<unsigned int>(m_loopStartFrame)) == 0)
        {
            if (headDecoder != nullptr)
            {
                stb_vorbis_close(headDecoder);
            }
            m_loopHeadPcm.clear();
            return false;
        }

        std::uint64_t decodedFrames{};
        while (decodedFrames < m_loopCrossfadeFrames)
        {
            const auto remainingFrames =
                m_loopCrossfadeFrames - decodedFrames;
            const auto remainingShorts =
                remainingFrames * channelCount;
            const int maximumShorts = static_cast<int>(std::min(
                remainingShorts,
                static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max())));
            const int decoded =
                stb_vorbis_get_samples_short_interleaved(
                    headDecoder,
                    m_channels,
                    reinterpret_cast<short*>(
                        m_loopHeadPcm.data()
                        + decodedFrames * channelCount),
                    maximumShorts);
            if (decoded <= 0)
            {
                break;
            }
            decodedFrames += static_cast<std::uint64_t>(decoded);
        }
        stb_vorbis_close(headDecoder);
        if (decodedFrames != m_loopCrossfadeFrames)
        {
            m_loopHeadPcm.clear();
            return false;
        }
        return true;
    }

    void AudioStreamVoice::FeedBuffer(
        DirectX::DynamicSoundEffectInstance& voice)
    {
        if (!m_playRequested)
        {
            return;
        }
        // 完了したbufferぶんを確定させます。この瞬間だけは
        // 「送信した量 − まだ残っている量」が実際の再生位置と
        // 一致するので、再生位置推定の基準時刻に使います。
        {
            const auto pending = static_cast<std::size_t>(
                std::max(voice.GetPendingBufferCount(), 0));
            bool consumed = false;
            while (m_queuedCount > pending)
            {
                m_consumedFrames += m_queuedFrames[m_queuedHead];
                m_queuedHead = (m_queuedHead + 1) % BufferCount;
                --m_queuedCount;
                consumed = true;
            }
            if (consumed)
            {
                m_consumedAt = std::chrono::steady_clock::now();
            }
        }
        if (m_finished)
        {
            // 供給終了後、送信済みバッファが尽きたら停止します。
            if (voice.GetPendingBufferCount() == 0)
            {
                voice.Stop();
            }
            return;
        }

        const std::size_t bytesPerFrame =
            static_cast<std::size_t>(m_channels)
            * sizeof(std::int16_t);
        // Play直後と各buffer-end callbackで常に複数bufferを先読みする。
        // 1本だけだとAudioEngine::Updateまでqueueが空になり、約1 frameの
        // 無音が周期的に入る可能性がある。リングの上書きを避けつつ
        // 3本を維持する。
        while (!m_finished
            && voice.GetPendingBufferCount() < BufferCount - 1)
        {
            auto& buffer = m_buffers[m_nextBuffer];
            m_nextBuffer =
                (m_nextBuffer + 1) % BufferCount;
            buffer.resize(BufferBytes);
            std::size_t bytes{};
            bool loopSeekFailed{};
            while (buffer.size() - bytes >= bytesPerFrame)
            {
                const std::uint64_t fileFrameBefore = m_frameCursor;
                const std::size_t decoded = DecodeChunk(
                    buffer.data() + bytes,
                    buffer.size() - bytes);
                if (decoded > 0)
                {
                    // 「このPCMは音源のどこか」を1件残します。
                    // 折り返しの直後は必ず新しいdecodeになるので、
                    // 目印が区間をまたぐことはありません。
                    m_positions[m_positionNext] = PositionMarker{
                        m_submittedFrames
                            + static_cast<std::uint64_t>(
                                bytes / bytesPerFrame),
                        fileFrameBefore };
                    m_positionNext =
                        (m_positionNext + 1) % PositionMarkerCount;
                    if (m_positionCount < PositionMarkerCount)
                    {
                        ++m_positionCount;
                    }
                    bytes += decoded;
                    loopSeekFailed = false;
                    continue;
                }
                if (!m_loop)
                {
                    m_finished = true;
                    break;
                }
                // 残り容量へループ開始点から続けてdecodeし、境界を同じ
                // submitted PCM buffer内に収める。指定範囲が不正だった場合は
                // m_hasLoopRegion=falseなので従来どおり先頭へ戻る。
                const std::uint64_t loopStart =
                    m_hasLoopRegion ? m_loopStartFrame : 0;
                m_crossfadeActive = false;
                m_crossfadeProgressFrames = 0;
                if (loopSeekFailed || !SeekFrame(loopStart))
                {
                    m_finished = true;
                    break;
                }
                ++m_completedLoopCount;
                // seek先でもdecode不能な壊れた音源で無限ループしない。
                loopSeekFailed = true;
            }
            if (bytes == 0)
            {
                if (voice.GetPendingBufferCount() == 0)
                {
                    voice.Stop();
                }
                return;
            }
            // 低音の持ち上げは解析より先に掛けます。棒の高さが
            // 「実際に鳴る音」と食い違わないようにするためです。
            ApplyBassBoost(buffer.data(), bytes);
            // 送るPCMそのものを解析します。ここで見た波形が、この
            // bufferが鳴る番になったときの棒の高さになります。
            AnalyzeSubmittedPcm(
                buffer.data(),
                bytes,
                m_submittedFrames);
            voice.SubmitBuffer(buffer.data(), bytes);
            const auto submittedFrames = static_cast<std::uint64_t>(
                bytes / bytesPerFrame);
            m_submittedFrames += submittedFrames;
            if (m_queuedCount < BufferCount)
            {
                m_queuedFrames[
                    (m_queuedHead + m_queuedCount) % BufferCount] =
                    submittedFrames;
                ++m_queuedCount;
            }
        }
    }
}
