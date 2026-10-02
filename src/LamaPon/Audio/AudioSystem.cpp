#include "LamaPon/Audio/AudioSystem.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/AudioListenerComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/MemorySnapshot.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Scene/GameObject.h"

#include <stb_vorbis.c>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    // 既定エンジンを作り、既知のAudioEngineエラーだけ250ms後に1回再試行する。
    [[nodiscard]] std::unique_ptr<DirectX::AudioEngine> CreateAudioEngine()
    {
        try
        {
            return std::make_unique<DirectX::AudioEngine>(
                DirectX::AudioEngine_Default);
        }
        // 既知のエンジン起動失敗だけ再試行する(error: 初回起動の例外)。
        catch (const std::runtime_error& error)
        {

            if (std::string_view(error.what()) != "AudioEngine")
            {
                throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return std::make_unique<DirectX::AudioEngine>(
                DirectX::AudioEngine_Default);
        }
    }


    // 音声フィルター用の円周率
    constexpr float LevelPi = 3.14159265358979323846f;
    // 解析する最低周波数
    constexpr float LevelLowestHz = 55.0f;
    // 解析する最高周波数
    constexpr float LevelHighestHz = 12500.0f;
    // 帯域フィルターのQ値
    constexpr float LevelBandQ = 2.6f;

    // 振幅包絡の下降時間
    constexpr float LevelReleaseSeconds = 0.11f;

    // 基準ピークの減衰時間
    constexpr float LevelPeakSeconds = 6.0f;

    // 無音として扱う相対振幅
    constexpr float LevelSilenceFloor = 2.0e-3f;

    // 帯域記録の毎秒頻度
    constexpr float LevelHopHz = 90.0f;
    // 帯域記録を保持する件数
    constexpr std::size_t LevelRingCapacity = 512;


    // 低音棚フィルターの傾き
    constexpr float BassShelfSlope = 1.0f;
}

namespace
{
    struct DecodedAudio final
    {
        // 形式記述とPCMの所有領域
        std::unique_ptr<std::uint8_t[]> storage;
        // 形式記述に使う領域容量
        std::size_t formatSize{sizeof(WAVEFORMATEX)};
        // 後続PCMのバイト容量
        std::size_t byteCount{};

        // 所有領域の先頭にある音声形式を借用する。
        [[nodiscard]] const WAVEFORMATEX* Format() const noexcept
        {
            return reinterpret_cast<const WAVEFORMATEX*>(
                storage.get());
        }

        // 所有領域から形式記述の直後のサンプル列を借用する。
        [[nodiscard]] const std::uint8_t* Samples() const noexcept
        {
            return storage.get() + formatSize;
        }
    };

    // WAVの形式記述とdataチャンクを所有領域へ複製する(source: 元のWAVバイト列, path: エラー表示用の音源パス)。
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

        // WAVの形式記述バイト列
        std::vector<std::uint8_t> fmtChunk;
        // 元WAVのPCM開始位置
        std::size_t dataOffset{};
        // 元WAVのPCMバイト容量
        std::size_t dataSize{};
        // RIFFチャンクの走査位置
        std::size_t offset = 12;
        while (offset + 8 <= source.size())
        {
            // RIFFチャンクの本文容量
            std::uint32_t chunkSize{};
            std::memcpy(
                &chunkSize,
                source.data() + offset + 4,
                sizeof(chunkSize));
            // RIFFチャンクの本文位置
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

        // 所有する復号音声データ
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

    // OGGを全復号してPCM16と形式記述を保持する(source: 元のOGGバイト列, path: エラー表示用の音源パス)。
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
        // PCMのチャンネル数
        int channels{};
        // 毎秒サンプルフレーム数
        int sampleRate{};
        // 全チャンネルのPCM16列
        short* samples{};
        // 復号した共通フレーム数
        const int samplesPerChannel = stb_vorbis_decode_memory(
            source.data(),
            static_cast<int>(source.size()),
            &channels,
            &sampleRate,
            &samples);
        // Vorbisのmalloc領域の解放
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

        // PCM16の1サンプル容量
        constexpr std::size_t bytesPerSample = sizeof(short);
        // 全チャンネルのサンプル数
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

        // 所有する復号音声データ
        DecodedAudio result;
        result.byteCount = sampleCount * bytesPerSample;
        result.storage = std::make_unique<std::uint8_t[]>(
            sizeof(WAVEFORMATEX) + result.byteCount);
        // PCMの音声形式記述
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
        : m_engine(CreateAudioEngine())
    {
    }

    AudioSystem::~AudioSystem() = default;

    std::shared_ptr<DirectX::SoundEffect>
        AudioSystem::LoadSoundEffect(
            AssetManager& assets,
            const std::filesystem::path& path)
    {
        // 指定された音源のパス
        const auto& resolvedPath = path;
        if (!assets.FileExists(resolvedPath))
        {
            throw std::runtime_error(
                "Audio file was not found: "
                + PathToUtf8(path));
        }
        // 音源の小文字拡張子
        auto extension = resolvedPath.extension().wstring();
        // 拡張子を小文字化する(character: 元の拡張子文字)。
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

        // 音源パス文字列の再利用キー
        const std::wstring key = MakeCacheKey(resolvedPath);
        // 共有効果音の検索結果
        if (const auto existing = m_soundCache.find(key);
            existing != m_soundCache.end())
        {
            return existing->second;
        }

        // 元音源の読み込みバイト列
        const auto fileBytes = assets.ReadFileBytes(resolvedPath);
        // 全復号済みの共有効果音
        std::shared_ptr<DirectX::SoundEffect> sound;
        if (extension == L".wav")
        {
            // 形式記述とPCMの所有データ
            auto decoded = DecodeWav(fileBytes, resolvedPath);
            // 復号した借用PCM先頭
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
            // 形式記述とPCMの所有データ
            auto decoded = DecodeOggVorbis(fileBytes, resolvedPath);
            // ログへ記録するPCM容量
            const auto decodedByteCount = decoded.byteCount;
            // 復号した借用PCM先頭
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
            // デバイスの回復成功判定
            const bool wasReset = m_engine->Reset();
            if (wasReset)
            {
                // リセットで無効になったボイスを再生成できるよう世代を進める。
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
        // 再開失敗はここで抑止し、次のUpdateのデバイス回復へ委ねる。
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
        // バス・帯域・PCM項目の番号
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
        // バス・帯域・PCM項目の番号
        const auto index = static_cast<std::size_t>(bus);
        return index < m_busVolumes.size()
            ? m_busVolumes[index]
            : 1.0f;
    }

    void AudioSystem::SetBusFade(
        const AudioBus bus,
        const float gain)
    {
        // バス・帯域・PCM項目の番号
        const auto index = static_cast<std::size_t>(bus);
        if (index < m_busFades.size())
        {
            m_busFades[index] = std::isfinite(gain)
                ? std::clamp(gain, 0.0f, 1.0f)
                : 1.0f;
        }
    }

    float AudioSystem::BusFade(
        const AudioBus bus) const noexcept
    {
        // バス・帯域・PCM項目の番号
        const auto index = static_cast<std::size_t>(bus);
        return index < m_busFades.size()
            ? m_busFades[index]
            : 1.0f;
    }

    float AudioSystem::EffectiveBusVolume(
        const AudioBus bus) const noexcept
    {
        return BusVolume(bus) * BusFade(bus);
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
        // 音源の小文字拡張子
        auto extension = path.extension().wstring();
        // 拡張子を小文字化する(character: 元の拡張子文字)。
        std::ranges::transform(
            extension,
            extension.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });


        // 所有する未再生のストリーム
        std::shared_ptr<AudioStreamVoice> stream(
            new AudioStreamVoice());
        stream->m_sourceBytes =
            assets.ReadFileBytes(path);

        if (extension == L".ogg")
        {
            // Vorbisの復号エラー番号
            int error{};
            // 借用するVorbisデコーダー
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
            // Vorbis音源の形式情報
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

            // WAVの形式記述とPCM控え
            const auto decodedHeader = DecodeWav(
                stream->m_sourceBytes,
                path);
            // PCMの音声形式記述
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

            // RIFFチャンクの走査位置
            std::size_t offset = 12;
            while (offset + 8
                <= stream->m_sourceBytes.size())
            {
                // RIFFチャンクの本文容量
                std::uint32_t chunkSize{};
                std::memcpy(
                    &chunkSize,
                    stream->m_sourceBytes.data()
                        + offset + 4,
                    sizeof(chunkSize));
                // RIFFチャンクの本文位置
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
            // 全チャンネル1フレーム容量
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

        // 供給先ストリームの借用参照
        auto* voicePointer = stream.get();
        // 生存中のストリームへPCMを補充する(instance: 供給を求める再生ボイス)。
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
        // 登録順に確認するリスナー
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
        // 音源パス文字列の再利用キー
        std::wstring key = path.native();
        // パスキーを小文字化する(character: 元のパス文字)。
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

        m_playRequested = false;
        m_instance->Stop();
        m_finished = false;
        m_nextBuffer = 0;
        m_crossfadeActive = false;
        m_crossfadeProgressFrames = 0;
        m_completedLoopCount = 0;
        // 最初のバッファ完了前にも位置を推定できるようPlay時に基準時刻を置く。
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

            m_bassB0 = 1.0f;
            m_bassB1 = 0.0f;
            m_bassB2 = 0.0f;
            m_bassA1 = 0.0f;
            m_bassA2 = 0.0f;
            return;
        }

        // 低音棚はRBJの係数を使い、a²が直流の振幅倍率となる。
        // 毎秒サンプルフレーム数
        const auto sampleRate = static_cast<float>(m_sampleRate);
        // 直流振幅倍率の平方根
        const float a = std::pow(10.0f, m_bassBoostDb / 40.0f);
        // 低音棚の角周波数
        const float w0 = 2.0f * LevelPi
            * std::min(m_bassCornerHz, sampleRate * 0.45f) / sampleRate;
        // 低音棚の周波数余弦
        const float cosW0 = std::cos(w0);
        // 低音棚の帯域幅係数
        const float alpha = std::sin(w0) * 0.5f
            * std::sqrt((a + 1.0f / a) * (1.0f / BassShelfSlope - 1.0f)
                        + 2.0f);
        // 低音棚の増幅補助係数
        const float beta = 2.0f * std::sqrt(a) * alpha;

        // 正規化前の入力係数B0
        const float b0 = a * ((a + 1.0f) - (a - 1.0f) * cosW0 + beta);
        // 正規化前の入力係数B1
        const float b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cosW0);
        // 正規化前の入力係数B2
        const float b2 = a * ((a + 1.0f) - (a - 1.0f) * cosW0 - beta);
        // 低音棚の係数正規化量
        const float a0 = (a + 1.0f) + (a - 1.0f) * cosW0 + beta;
        // 正規化前の出力係数A1
        const float a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cosW0);
        // 正規化前の出力係数A2
        const float a2 = (a + 1.0f) + (a - 1.0f) * cosW0 - beta;

        // PCM全体を補正量だけ減衰し、低音棚の直流利得を1へ正規化する。
        // PCMの補正前減衰倍率
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
        // PCMのチャンネル数
        const auto channels = static_cast<std::size_t>(m_channels);
        // 今回のPCMフレーム数
        const std::size_t frames =
            bytes / (channels * sizeof(std::int16_t));
        // 全チャンネルのPCM16列
        auto* samples = reinterpret_cast<std::int16_t*>(pcm);

        // 処理中のPCMフレーム番号
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            // 処理中のチャンネル番号
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                // 低音棚の過去入出力履歴
                auto& state = m_bassState[channel];
                // バス・帯域・PCM項目の番号
                const auto index = frame * channels + channel;
                // 低音補正前のPCM振幅
                const float input = static_cast<float>(samples[index]);
                // Direct Form Iの状態順序は過去の入力2点と過去の出力2点とする。
                // 低音補正後のPCM振幅
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

        // 毎秒サンプルフレーム数
        const auto sampleRate = static_cast<float>(m_sampleRate);

        // 解析の最低周波数
        const float lowest = LevelLowestHz;
        // 標本化周波数内の解析上限
        const float highest = std::min(
            LevelHighestHz,
            sampleRate * 0.42f);
        // 隣接帯域の周波数比
        const float ratio = highest > lowest
            ? std::pow(
                highest / lowest,
                1.0f / static_cast<float>(LevelBandCount - 1))
            : 1.0f;
        // 現在の帯域中心周波数
        float center = lowest;
        // 低域から高域の帯域番号
        for (int band = 0; band < LevelBandCount; ++band)
        {
            // バス・帯域・PCM項目の番号
            const auto index = static_cast<std::size_t>(band);
            // 帯域解析にはTPT型状態変数フィルターを使う。
            // 帯域フィルターの周波数係数
            const float g = std::tan(
                LevelPi * std::min(center, sampleRate * 0.45f)
                    / sampleRate);
            // 帯域フィルターの減衰係数
            const float k = 1.0f / LevelBandQ;
            // 帯域フィルターの正規化係数
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
        // PCMのチャンネル数
        const auto channels = static_cast<std::size_t>(m_channels);
        // 全チャンネル1フレーム容量
        const std::size_t bytesPerFrame =
            channels * sizeof(std::int16_t);
        // 今回のPCMフレーム数
        const std::size_t frames = bytes / bytesPerFrame;
        // 全チャンネルのPCM16列
        const auto* samples =
            reinterpret_cast<const std::int16_t*>(pcm);
        // 平均モノラル振幅への倍率
        const float monoScale =
            1.0f / (32768.0f * static_cast<float>(channels));

        // 処理中のPCMフレーム番号
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            // 平均化したモノラル振幅
            float mono = 0.0f;
            // 処理中のチャンネル番号
            for (std::size_t channel = 0;
                 // PCMのチャンネル数
                 channel < channels;
                 ++channel)
            {
                mono += static_cast<float>(
                    samples[frame * channels + channel]);
            }
            mono *= monoScale;

            // 低域から高域の帯域番号
            for (int band = 0; band < LevelBandCount; ++band)
            {
                // バス・帯域・PCM項目の番号
                const auto index = static_cast<std::size_t>(band);
                // 第2状態との差分振幅
                const float v3 = mono - m_bandIc2[index];
                // 帯域通過フィルター出力
                const float v1 = m_bandA1[index] * m_bandIc1[index]
                    + m_bandA2[index] * v3;
                // 低域通過フィルター出力
                const float v2 = m_bandIc2[index]
                    + m_bandA2[index] * m_bandIc1[index]
                    + m_bandA3[index] * v3;
                m_bandIc1[index] = 2.0f * v1 - m_bandIc1[index];
                m_bandIc2[index] = 2.0f * v2 - m_bandIc2[index];
                // 帯域通過出力の振幅を即座に採用し、下降時だけ包絡を減衰させる。
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

            // 送信位置に対応する帯域記録
            LevelSample sample{};
            sample.streamFrame = streamFrameStart
                + static_cast<std::uint64_t>(frame) + 1;
            // 低域から高域の帯域番号
            for (int band = 0; band < LevelBandCount; ++band)
            {
                // バス・帯域・PCM項目の番号
                const auto index = static_cast<std::size_t>(band);

                // 帯域の基準ピーク振幅
                const float peak = m_bandPeak[index];
                // 基準ピークに対する相対振幅
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

        // 完了量の確定からの実時間
        const auto elapsed =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_consumedAt)
            .count();
        // 基準位置からの推定進行量
        const double advanced = std::max(elapsed, 0.0)
            * static_cast<double>(m_sampleRate);
        // 送信列の推定再生フレーム
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
        // 出力する帯域の件数
        const std::size_t count = std::min(
            capacity,
            static_cast<std::size_t>(LevelBandCount));
        // バス・帯域・PCM項目の番号
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

        // 送信列の推定再生フレーム
        const std::uint64_t played = PlayedStreamFrame();
        // リングが保持する枠数
        const std::size_t size = m_levelRing.size();
        // 有効な最古の記録枠
        const std::size_t oldest =
            (m_levelRingNext + size - m_levelRingCount) % size;
        // 再生位置に対応する推定記録
        const LevelSample* chosen = &m_levelRing[oldest];
        // 最古から辿る記録番号
        for (std::size_t step = 0; step < m_levelRingCount; ++step)
        {
            // 解析記録またはPCM位置
            const LevelSample& sample =
                m_levelRing[(oldest + step) % size];
            if (sample.streamFrame > played)
            {
                break;
            }
            chosen = &sample;
        }
        // バス・帯域・PCM項目の番号
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
        // 送信列の推定再生フレーム
        const std::uint64_t played = PlayedStreamFrame();
        // リングが保持する枠数
        const std::size_t size = m_positions.size();
        // 有効な最古の記録枠
        const std::size_t oldest =
            (m_positionNext + size - m_positionCount) % size;
        // 再生位置に対応する推定記録
        const PositionMarker* chosen = &m_positions[oldest];
        // 最古から辿る記録番号
        for (std::size_t step = 0; step < m_positionCount; ++step)
        {
            // 送信列と音源の位置記録
            const PositionMarker& marker =
                m_positions[(oldest + step) % size];
            if (marker.streamFrame > played)
            {
                break;
            }
            chosen = &marker;
        }

        // 基準位置からの推定進行量
        const std::uint64_t advanced = played > chosen->streamFrame
            ? played - chosen->streamFrame
            : 0;
        // 推定した音源内のフレーム
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
        // バス・帯域・PCM項目の番号
        for (std::size_t index = 0; index < bucketCount; ++index)
        {
            destination[index] = 0.0f;
        }


        // 全音源測定前の復号位置
        const std::uint64_t restore = m_frameCursor;
        if (!SeekFrame(0))
        {
            return 0;
        }

        // PCMのチャンネル数
        const auto channels = static_cast<std::size_t>(m_channels);
        // 全チャンネル1フレーム容量
        const std::size_t bytesPerFrame =
            channels * sizeof(std::int16_t);
        // ピーク測定のPCM読込領域
        std::vector<std::uint8_t> block(16384 * bytesPerFrame);
        // 音源全体の復号進捗フレーム
        std::uint64_t frame{};
        while (frame < m_totalFrames)
        {
            // 復号または送信するPCM容量
            const std::size_t bytes = DecodeRawFrames(
                block.data(),
                std::min<std::uint64_t>(
                    16384, m_totalFrames - frame));
            if (bytes == 0)
            {
                break;
            }
            // 今回のPCMフレーム数
            const std::size_t frames = bytes / bytesPerFrame;
            // 全チャンネルのPCM16列
            const auto* samples =
                reinterpret_cast<const std::int16_t*>(block.data());
            // 読込領域内のフレーム番号
            for (std::size_t offset = 0; offset < frames; ++offset)
            {
                // 最大振幅の出力区間番号
                const std::size_t bucket = static_cast<std::size_t>(
                    (frame + offset) * bucketCount / m_totalFrames);
                if (bucket >= bucketCount)
                {
                    continue;
                }
                // 処理中のチャンネル番号
                for (std::size_t channel = 0;
                     // PCMのチャンネル数
                     channel < channels;
                     ++channel)
                {
                    // 基準ピークに対する相対振幅
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

        m_loopCrossfadeFrames = std::min(
            crossfadeFrames,
            (endFrame - startFrame) / 2);
        m_hasLoopRegion = true;
        m_crossfadeActive = false;
        m_crossfadeProgressFrames = 0;
        if (m_loopCrossfadeFrames > 0
            && !PrepareLoopHead())
        {

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

        m_volume = std::clamp(volume, 0.0f, 4.0f);
        ApplyVoiceVolume();
    }

    void AudioStreamVoice::ApplyVoiceVolume()
    {
        if (!m_instance)
        {
            return;
        }

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
        // 全チャンネル1フレーム容量
        const std::size_t bytesPerFrame =
            static_cast<std::size_t>(m_channels)
            * sizeof(std::int16_t);
        if (destination == nullptr
            || bytesPerFrame == 0
            || capacity < bytesPerFrame)
        {
            return 0;
        }
        // 出力先に収容するフレーム数
        const auto capacityFrames = static_cast<std::uint64_t>(
            capacity / bytesPerFrame);
        // 先頭PCMを使える重ね合成
        const bool crossfadeEnabled =
            m_loop
            && m_hasLoopRegion
            && m_loopCrossfadeFrames > 0
            && m_loopHeadPcm.size()
                == m_loopCrossfadeFrames
                    * static_cast<std::uint64_t>(m_channels);
        // 末尾で重ね合成を始める位置
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
            // 今回復号する最大フレーム数
            const auto requestedFrames = std::min(
                capacityFrames,
                m_loopCrossfadeFrames
                    - m_crossfadeProgressFrames);
            // 復号または送信するPCM容量
            const std::size_t bytes = DecodeRawFrames(
                destination,
                requestedFrames);
            // 復号済みの共通フレーム数
            const auto decodedFrames = static_cast<std::uint64_t>(
                bytes / bytesPerFrame);
            // 合成で上書きする末尾PCM
            auto* tail = reinterpret_cast<std::int16_t*>(destination);
            // 処理中のPCMフレーム番号
            for (std::uint64_t frame = 0;
                 // 復号済みの共通フレーム数
                 frame < decodedFrames;
                 ++frame)
            {
                // 重ね区間内のフレーム位置
                const std::uint64_t fadeFrame =
                    m_crossfadeProgressFrames + frame;
                // 先頭PCMの線形合成比率
                const float headGain = m_loopCrossfadeFrames > 1
                    ? static_cast<float>(fadeFrame)
                        / static_cast<float>(
                            m_loopCrossfadeFrames - 1)
                    : 1.0f;
                // 末尾PCMの線形合成比率
                const float tailGain = 1.0f - headGain;
                // 処理中のチャンネル番号
                for (int channel = 0; channel < m_channels; ++channel)
                {
                    // 末尾PCMのサンプル位置
                    const auto sample = static_cast<std::size_t>(
                        frame * static_cast<std::uint64_t>(m_channels)
                        + static_cast<std::uint64_t>(channel));
                    // 先頭PCMのサンプル位置
                    const auto headSample = static_cast<std::size_t>(
                        fadeFrame
                            * static_cast<std::uint64_t>(m_channels)
                        + static_cast<std::uint64_t>(channel));
                    // 末尾と先頭の合成振幅
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
                    // 先頭直後へのシーク失敗では重ね合成を解除し通常の区間反復へ戻す。
                    m_loopCrossfadeFrames = 0;
                    m_loopHeadPcm.clear();
                }
            }
            return bytes;
        }

        // 今回の復号範囲の終端
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
        // 今回復号する最大フレーム数
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
        // 全チャンネル1フレーム容量
        const std::size_t bytesPerFrame =
            static_cast<std::size_t>(m_channels)
            * sizeof(std::int16_t);
        // 今回復号する最大フレーム数
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
            // 借用するVorbisデコーダー
            auto* vorbis =
                static_cast<stb_vorbis*>(m_vorbis);
            if (vorbis == nullptr)
            {
                return 0;
            }
            // 全チャンネルの復号要素数
            const auto requestedShorts =
                requestedFrames
                * static_cast<std::uint64_t>(m_channels);
            // intで渡せる復号要素数
            const int maximumShorts = static_cast<int>(std::min(
                requestedShorts,
                static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max())));
            // 復号した共通フレーム数
            const int samplesPerChannel =
                stb_vorbis_get_samples_short_interleaved(
                    vorbis,
                    m_channels,
                    reinterpret_cast<short*>(destination),
                    maximumShorts);
            // 復号済みの共通フレーム数
            const auto decodedFrames = static_cast<std::uint64_t>(
                std::max(samplesPerChannel, 0));
            m_frameCursor += decodedFrames;
            return static_cast<std::size_t>(decodedFrames)
                * bytesPerFrame;
        }


        // 今回のPCMフレーム数
        const auto frames = static_cast<std::size_t>(
            requestedFrames);
        // 復号または送信するPCM容量
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
            // 借用するVorbisデコーダー
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
            // 全チャンネル1フレーム容量
            const auto bytesPerFrame =
                static_cast<std::uint64_t>(m_channels)
                * sizeof(std::int16_t);
            // PCM区間の開始バイト位置
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
        // 先頭PCMのチャンネル数
        const auto channelCount =
            static_cast<std::uint64_t>(m_channels);
        if (m_loopCrossfadeFrames
            > std::numeric_limits<std::size_t>::max() / channelCount)
        {
            return false;
        }
        // 全チャンネルのサンプル数
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
            // bytesPerFrame: 全チャンネル1フレームのbyte容量。
            const auto bytesPerFrame = channelCount
                * sizeof(std::int16_t);
            // byteOffset: PCM loop区間の開始byte位置。
            const auto byteOffset =
                m_loopStartFrame * bytesPerFrame;
            // byteCount: 先頭PCM区間のbyte容量。
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

        // Vorbisの復号エラー番号
        int error{};
        // 先頭PCM専用のデコーダー
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

        // 復号済みの共通フレーム数
        std::uint64_t decodedFrames{};
        while (decodedFrames < m_loopCrossfadeFrames)
        {
            // 先頭PCMの未復号フレーム
            const auto remainingFrames =
                m_loopCrossfadeFrames - decodedFrames;
            // remainingShorts: 未復号PCMのchannel要素数。
            const auto remainingShorts =
                remainingFrames * channelCount;
            // intで渡せる復号要素数
            const int maximumShorts = static_cast<int>(std::min(
                remainingShorts,
                static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max())));
            // 今回の復号フレーム数
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
        // 未完了キューとの差から完了フレーム量を確定し、位置推定の基準を更新する。
        {
            // 未完了のPCM送信枠数
            const auto pending = static_cast<std::size_t>(
                std::max(voice.GetPendingBufferCount(), 0));
            // 完了量の更新があった判定
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

            if (voice.GetPendingBufferCount() == 0)
            {
                voice.Stop();
            }
            return;
        }

        // 全チャンネル1フレーム容量
        const std::size_t bytesPerFrame =
            static_cast<std::size_t>(m_channels)
            * sizeof(std::int16_t);
        // リングの上書きとキュー枯渇を避けるため、4枠のうち3枠を先読みする。
        while (!m_finished
            && voice.GetPendingBufferCount() < BufferCount - 1)
        {
            // 再利用するPCM送信枠
            auto& buffer = m_buffers[m_nextBuffer];
            m_nextBuffer =
                (m_nextBuffer + 1) % BufferCount;
            buffer.resize(BufferBytes);
            // 復号または送信するPCM容量
            std::size_t bytes{};
            // 反復先での復号失敗判定
            bool loopSeekFailed{};
            while (buffer.size() - bytes >= bytesPerFrame)
            {
                // 今回復号する音源の開始位置
                const std::uint64_t fileFrameBefore = m_frameCursor;
                // 今回の復号バイト数
                const std::size_t decoded = DecodeChunk(
                    buffer.data() + bytes,
                    buffer.size() - bytes);
                if (decoded > 0)
                {
                    // 各復号区間の送信位置と音源位置を記録し、折り返し後は別の目印を置く。
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
                // 同じ送信枠の残りへ反復開始点から続けて復号し、境界の無音を避ける。
                // 通常反復の戻りフレーム
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
                // シーク後も復号できない場合は次の試行を打ち切る。
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
            // 帯域メーターが転送する音を解析するよう、低音補正を先に適用する。
            ApplyBassBoost(buffer.data(), bytes);

            AnalyzeSubmittedPcm(
                buffer.data(),
                bytes,
                m_submittedFrames);
            voice.SubmitBuffer(buffer.data(), bytes);
            // 今回送信した共通フレーム数
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

    void AudioSystem::AppendMemoryEntries(
        std::vector<MemorySnapshotEntry>& entries) const
    {
        // 音源パスキーと共有効果音
        for (const auto& [key, sound] : m_soundCache)
        {
            if (sound == nullptr)
            {
                continue;
            }
            // メモリ内訳の追加項目
            MemorySnapshotEntry entry;
            entry.category = MemoryCategory::Audio;
            entry.name = WideToUtf8(key);

            entry.cpuBytes = sound->GetSampleSizeInBytes();
            entry.detail =
                std::to_string(sound->GetSampleDurationMS()) + " ms";
            entries.push_back(std::move(entry));
        }
    }
}
