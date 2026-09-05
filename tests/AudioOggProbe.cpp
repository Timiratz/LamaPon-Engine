#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace LamaPon
{
    // 実オーディオデバイスの再生速度へ依存せず、製品コードと同じ
    // DecodeChunk、crossfade、seekの各経路でイントロ末尾から
    // ループ先頭への遷移を検証します。
    struct AudioStreamVoiceTestAccess
    {
        static void VerifyCustomLoop(AudioStreamVoice& stream)
        {
            if (stream.m_totalFrames < 64)
            {
                throw std::runtime_error(
                    "Streaming fixture is too short for loop testing.");
            }

            const std::uint64_t loopStart =
                stream.m_totalFrames / 4;
            const std::uint64_t loopEnd =
                stream.m_totalFrames * 3 / 4;
            const std::uint64_t crossfadeFrames = std::min(
                std::uint64_t{32},
                (loopEnd - loopStart) / 4);

            stream.SetLoop(true);
            stream.SetLoopRegionFrames(
                loopStart,
                loopEnd,
                crossfadeFrames);
            if (!stream.m_hasLoopRegion
                || stream.m_loopCrossfadeFrames != crossfadeFrames
                || stream.m_loopHeadPcm.empty())
            {
                throw std::runtime_error(
                    "Custom OGG loop region was not prepared.");
            }

            stream.m_finished = false;
            stream.m_nextBuffer = 0;
            stream.m_crossfadeActive = false;
            stream.m_crossfadeProgressFrames = 0;
            stream.m_completedLoopCount = 0;
            if (!stream.SeekFrame(0))
            {
                throw std::runtime_error(
                    "Could not seek the OGG stream to its intro.");
            }
            const std::size_t bytesPerFrame =
                static_cast<std::size_t>(stream.m_channels)
                * sizeof(std::int16_t);
            std::vector<std::uint8_t> decoded(
                4096 * bytesPerFrame);
            std::uint64_t decodedFrames{};
            while (stream.CompletedLoopCount() == 0)
            {
                const auto bytes = stream.DecodeChunk(
                    decoded.data(),
                    decoded.size());
                if (bytes == 0)
                {
                    throw std::runtime_error(
                        "Custom OGG loop decoder stopped at its boundary.");
                }
                decodedFrames += bytes / bytesPerFrame;
                if (decodedFrames > stream.m_totalFrames)
                {
                    throw std::runtime_error(
                        "Custom OGG loop boundary was not reached in time.");
                }
            }
            if (stream.CompletedLoopCount() == 0)
            {
                throw std::runtime_error(
                    "Custom OGG loop boundary was not crossed.");
            }

        }

        // 再生開始位置。音源の外を指されたら先頭へ戻すこと、指した
        // 位置へ実際にseekできることを確かめます。
        static void VerifyStartFrame(AudioStreamVoice& stream)
        {
            const std::uint64_t middle = stream.m_totalFrames / 2;
            stream.SetStartFrame(middle);
            if (stream.StartFrame() != middle)
            {
                throw std::runtime_error(
                    "The stream did not keep its start frame.");
            }
            if (!stream.SeekFrame(stream.StartFrame())
                || stream.m_frameCursor != middle)
            {
                throw std::runtime_error(
                    "The stream could not seek to its start frame.");
            }
            stream.SetStartFrame(stream.m_totalFrames + 1);
            if (stream.StartFrame() != 0)
            {
                throw std::runtime_error(
                    "An out-of-range start frame must fall back to 0.");
            }
        }

        // 帯域レベルメーター。実デバイスが無くても、製品と同じ
        // 「送るPCMを解析する」経路をそのまま踏んで確かめます。
        static void VerifyLevelMeter(AudioStreamVoice& stream)
        {
            stream.ClearLoopRegion();
            stream.SetLoop(false);
            stream.SetLevelMeterEnabled(true);
            if (!stream.IsLevelMeterEnabled()
                || stream.m_levelRing.empty()
                || stream.m_levelHopFrames == 0
                || stream.m_bandA1[0] <= 0.0f)
            {
                throw std::runtime_error(
                    "The level meter was not armed.");
            }

            if (!stream.SeekFrame(0))
            {
                throw std::runtime_error(
                    "Could not rewind before metering.");
            }
            stream.m_finished = false;
            stream.ResetLevelMeter();

            const std::size_t bytesPerFrame =
                static_cast<std::size_t>(stream.m_channels)
                * sizeof(std::int16_t);
            std::vector<std::uint8_t> decoded(4096 * bytesPerFrame);
            std::uint64_t submitted{};
            // 解析結果が十分たまるまで（or 音源を使い切るまで）流す。
            const std::uint64_t wanted = std::min<std::uint64_t>(
                stream.m_totalFrames,
                static_cast<std::uint64_t>(stream.m_sampleRate) * 2);
            while (submitted < wanted)
            {
                const auto bytes = stream.DecodeChunk(
                    decoded.data(), decoded.size());
                if (bytes == 0)
                {
                    break;
                }
                stream.AnalyzeSubmittedPcm(
                    decoded.data(), bytes, submitted);
                submitted += bytes / bytesPerFrame;
            }

            if (stream.m_levelRingCount == 0)
            {
                throw std::runtime_error(
                    "The level meter produced no samples.");
            }
            float loudest = 0.0f;
            for (std::size_t index = 0;
                 index < stream.m_levelRingCount;
                 ++index)
            {
                for (const float level :
                     stream.m_levelRing[index].bands)
                {
                    if (!(level >= 0.0f) || level > 1.0f)
                    {
                        throw std::runtime_error(
                            "A level band left the 0..1 range.");
                    }
                    loudest = std::max(loudest, level);
                }
            }
            // どの帯域も自分のピークで正規化するので、鳴っている音が
            // あれば必ず1に近い値がどこかへ出ます。
            if (loudest < 0.5f)
            {
                throw std::runtime_error(
                    "The level meter stayed silent on real audio.");
            }
            std::wcout << L"METER samples="
                       << stream.m_levelRingCount
                       << L" loudest=" << loudest << std::endl;

            stream.SetLevelMeterEnabled(false);
            if (stream.IsLevelMeterEnabled()
                || !stream.m_levelRing.empty())
            {
                throw std::runtime_error(
                    "Disabling the level meter must release its ring.");
            }
        }

        // 波形表示用のピーク包絡。0..1に収まること、鳴っている音が
        // あれば平らにならないこと、そして読み出しても再生位置が
        // 動かないことを確かめます。最後のひとつを落とすと、波形を
        // 描いただけで曲が飛びます。
        static void VerifyPeakEnvelope(AudioStreamVoice& stream)
        {
            const std::uint64_t cursor = stream.m_totalFrames / 3;
            if (!stream.SeekFrame(cursor))
            {
                throw std::runtime_error(
                    "Could not park the cursor before reading peaks.");
            }
            std::vector<float> peaks(128, -1.0f);
            const std::size_t written =
                stream.ReadPeakEnvelope(peaks.data(), peaks.size());
            if (written != peaks.size())
            {
                throw std::runtime_error(
                    "The peak envelope was not filled.");
            }
            if (stream.m_frameCursor != cursor)
            {
                throw std::runtime_error(
                    "Reading the peak envelope moved the play cursor.");
            }
            float loudest = 0.0f;
            for (const float peak : peaks)
            {
                if (!(peak >= 0.0f) || peak > 1.0f)
                {
                    throw std::runtime_error(
                        "A peak left the 0..1 range.");
                }
                loudest = std::max(loudest, peak);
            }
            if (loudest < 0.05f)
            {
                throw std::runtime_error(
                    "The peak envelope stayed flat on real audio.");
            }
            std::wcout << L"WAVE buckets=" << written
                       << L" loudest=" << loudest << std::endl;
        }

        // 低音の持ち上げ。音源の中身に左右されないよう、低い正弦波と
        // 高い正弦波を混ぜた信号を自分で作って通します（テスト用の
        // 短い効果音には低域がほとんど無く、比が動かないため）。
        // 直流利得を1.0に正規化してあるので、狙いどおりなら
        // 「低域はそのまま、高域が-6dB」になります。
        static void VerifyBassBoost(AudioStreamVoice& stream)
        {
            constexpr double lowHz = 60.0;
            constexpr double highHz = 3000.0;
            constexpr double boostDb = 6.0;

            const auto channels =
                static_cast<std::size_t>(stream.m_channels);
            const auto sampleRate =
                static_cast<double>(stream.m_sampleRate);
            const std::size_t frames =
                static_cast<std::size_t>(sampleRate * 0.5);
            std::vector<std::uint8_t> pcm(
                frames * channels * sizeof(std::int16_t));
            auto* samples =
                reinterpret_cast<std::int16_t*>(pcm.data());
            for (std::size_t frame = 0; frame < frames; ++frame)
            {
                const double time =
                    static_cast<double>(frame) / sampleRate;
                const double value =
                    std::sin(2.0 * 3.14159265358979 * lowHz * time) * 0.3
                    + std::sin(
                        2.0 * 3.14159265358979 * highHz * time) * 0.3;
                const auto sample = static_cast<std::int16_t>(
                    std::lround(value * 32767.0));
                for (std::size_t channel = 0;
                     channel < channels;
                     ++channel)
                {
                    samples[frame * channels + channel] = sample;
                }
            }

            // 指定周波数の振幅（相関で取り出す）。
            auto amplitude = [&](const std::vector<std::uint8_t>& block,
                                 const double frequency)
            {
                const auto* values =
                    reinterpret_cast<const std::int16_t*>(block.data());
                double real = 0.0;
                double imaginary = 0.0;
                for (std::size_t frame = 0; frame < frames; ++frame)
                {
                    const double time =
                        static_cast<double>(frame) / sampleRate;
                    const double angle =
                        2.0 * 3.14159265358979 * frequency * time;
                    const double value = static_cast<double>(
                        values[frame * channels]);
                    real += value * std::cos(angle);
                    imaginary += value * std::sin(angle);
                }
                return 2.0
                    * std::sqrt(real * real + imaginary * imaginary)
                    / static_cast<double>(frames);
            };

            const double plainLow = amplitude(pcm, lowHz);
            const double plainHigh = amplitude(pcm, highHz);

            std::vector<std::uint8_t> boosted = pcm;
            stream.SetBassBoost(
                static_cast<float>(boostDb), 110.0f);
            stream.ApplyBassBoost(boosted.data(), boosted.size());
            const double boostedLow = amplitude(boosted, lowHz);
            const double boostedHigh = amplitude(boosted, highHz);

            // 低域は素通し（±1.5dB）。
            const double lowDb =
                20.0 * std::log10(boostedLow / plainLow);
            if (std::abs(lowDb) > 1.5)
            {
                throw std::runtime_error(
                    "The bass shelf should pass the low tone through.");
            }
            // 高域は指定ぶん下がる（＝相対的に低音が強くなる）。
            const double highDb =
                20.0 * std::log10(boostedHigh / plainHigh);
            if (std::abs(highDb + boostDb) > 1.5)
            {
                throw std::runtime_error(
                    "The bass shelf did not trim the high tone.");
            }
            // 振り切れていないこと。
            for (std::size_t index = 0;
                 index < boosted.size() / sizeof(std::int16_t);
                 ++index)
            {
                const auto value = reinterpret_cast<const std::int16_t*>(
                    boosted.data())[index];
                if (value == std::numeric_limits<std::int16_t>::min()
                    || value == std::numeric_limits<std::int16_t>::max())
                {
                    throw std::runtime_error(
                        "The bass boost clipped the signal.");
                }
            }
            const float makeup = stream.BassBoostMakeup();
            if (!(makeup > 1.99f && makeup < 2.01f))
            {
                throw std::runtime_error(
                    "BassBoostMakeup should undo the 6 dB trim.");
            }
            std::wcout << L"BASS low " << lowDb << L"dB  high "
                       << highDb << L"dB" << std::endl;
            stream.SetBassBoost(0.0f);
        }

        // 再生位置。デバイスが無いとPCMが流れないので目印は空のまま。
        // その状態では開始位置をそのまま返すのが正しい動きです。
        static void VerifyPlaybackFrame(AudioStreamVoice& stream)
        {
            const std::uint64_t start = stream.m_totalFrames / 4;
            stream.SetStartFrame(start);
            if (stream.PlaybackFrame() != start)
            {
                throw std::runtime_error(
                    "A silent stream must report its start frame.");
            }
            stream.SetStartFrame(0);
        }
    };
}

namespace
{
    int RunProbe(const std::filesystem::path& root)
    {
        std::vector<std::filesystem::path> files;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            auto extension = entry.path().extension().wstring();
            std::ranges::transform(
                extension,
                extension.begin(),
                ::towlower);
            if (extension == L".ogg")
            {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
        if (files.empty())
        {
            throw std::runtime_error(
                "No OGG test fixture was found.");
        }

        LamaPon::AssetManager assets(nullptr, nullptr);
        LamaPon::AudioSystem audio;
        for (const auto& path : files)
        {
            std::wcout << L"TEST " << path.filename().wstring()
                       << std::endl;
            auto sound = audio.LoadSoundEffect(assets, path);
            if (!sound)
            {
                throw std::runtime_error(
                    "Decoded OGG contains no samples.");
            }
            const auto* format = sound->GetFormat();
            std::wcout
                << L"FORMAT tag=" << format->wFormatTag
                << L" channels=" << format->nChannels
                << L" rate=" << format->nSamplesPerSec
                << L" bits=" << format->wBitsPerSample
                << L" align=" << format->nBlockAlign
                << L" bytes=" << sound->GetSampleSizeInBytes()
                << std::endl;
            auto instance = sound->CreateInstance();
            instance->Play(false);
            instance->Stop();
            auto stream = audio.CreateStream(assets, path);
            LamaPon::AudioStreamVoiceTestAccess::VerifyCustomLoop(
                *stream);
            std::wcout
                << L"LOOP wraps="
                << stream->CompletedLoopCount()
                << std::endl;
            LamaPon::AudioStreamVoiceTestAccess::VerifyStartFrame(
                *stream);
            LamaPon::AudioStreamVoiceTestAccess::VerifyLevelMeter(
                *stream);
            LamaPon::AudioStreamVoiceTestAccess::VerifyPeakEnvelope(
                *stream);
            LamaPon::AudioStreamVoiceTestAccess::VerifyPlaybackFrame(
                *stream);
            LamaPon::AudioStreamVoiceTestAccess::VerifyBassBoost(
                *stream);
            std::wcout
                << L"OK " << path.filename().wstring()
                << std::endl;
            stream.reset();
            instance.reset();
            sound.reset();
            audio.Clear();
        }
        std::wcout << L"Decoded " << files.size()
                   << L" OGG files.\n";
        return 0;
    }
}

int wmain(const int argumentCount, wchar_t** arguments)
{
    if (argumentCount != 2)
    {
        std::wcerr
            << L"Usage: LamaPonAudioOggProbe <audio directory>\n";
        return 2;
    }

    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);

    // AssetManagerのWIC/D2D/DWriteファクトリとXAudio2の音声はCOMを保持します。
    // これらをRunProbe内で破棄してからCoUninitializeを呼びます。
    int exitCode = 1;
    try
    {
        exitCode = RunProbe(std::filesystem::path(arguments[1]));
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        exitCode = 1;
    }

    if (uninitialize)
    {
        CoUninitialize();
    }
    return exitCode;
}
