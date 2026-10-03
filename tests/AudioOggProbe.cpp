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
    // 実機再生に依存せずOGGのloop・seek経路を検証します。
    struct AudioStreamVoiceTestAccess
    {
        // VerifyCustomLoop(stream: test stream): OGG loopのcrossfadeと境界遷移を確認します。
        static void VerifyCustomLoop(AudioStreamVoice& stream)
        {
            // crossfade検証に足りない短い音源を拒否します。
            if (stream.m_totalFrames < 64)
            {
                throw std::runtime_error(
                    "Streaming fixture is too short for loop testing.");
            }

            // 音源の前半をloop開始位置にします。
            const std::uint64_t loopStart =
                stream.m_totalFrames / 4;
            // 音源の後半をloop終端にします。
            const std::uint64_t loopEnd =
                stream.m_totalFrames * 3 / 4;
            // loop区間の1/4または32 frameまでをcrossfadeします。
            const std::uint64_t crossfadeFrames = std::min(
                std::uint64_t{32},
                (loopEnd - loopStart) / 4);

            stream.SetLoop(true);
            stream.SetLoopRegionFrames(
                loopStart,
                loopEnd,
                crossfadeFrames);
            // 計算したloop区間をstreamへ設定します。
            if (!stream.m_hasLoopRegion
                || stream.m_loopCrossfadeFrames != crossfadeFrames
                || stream.m_loopHeadPcm.empty())
            {
                throw std::runtime_error(
                    "Custom OGG loop region was not prepared.");
            }

            // decoderとcrossfade状態を初期化します。
            stream.m_finished = false;
            stream.m_nextBuffer = 0;
            stream.m_crossfadeActive = false;
            stream.m_crossfadeProgressFrames = 0;
            stream.m_completedLoopCount = 0;
            // intro先頭へseekできることを確認します。
            if (!stream.SeekFrame(0))
            {
                throw std::runtime_error(
                    "Could not seek the OGG stream to its intro.");
            }
            // PCM frameあたりのbyte数です。
            const std::size_t bytesPerFrame =
                static_cast<std::size_t>(stream.m_channels)
                * sizeof(std::int16_t);
            // DecodeChunk出力を受けるPCM bufferです。
            std::vector<std::uint8_t> decoded(
                4096 * bytesPerFrame);
            // loop境界までにdecodeしたframe数です。
            std::uint64_t decodedFrames{};
            // loopが一周完了するまでPCMをdecodeします。
            while (stream.CompletedLoopCount() == 0)
            {
                // 次のPCM chunkをdecodeします。
                const auto bytes = stream.DecodeChunk(
                    decoded.data(),
                    decoded.size());
                // 終端到達前にdecodeが止まれば失敗です。
                if (bytes == 0)
                {
                    throw std::runtime_error(
                        "Custom OGG loop decoder stopped at its boundary.");
                }
                // decode済みframe数を集計します。
                decodedFrames += bytes / bytesPerFrame;
                // 一周前に全音源を越えた場合は境界処理失敗です。
                if (decodedFrames > stream.m_totalFrames)
                {
                    throw std::runtime_error(
                        "Custom OGG loop boundary was not reached in time.");
                }
            }
            // loop境界を一度も越えない結果を失敗にします。
            if (stream.CompletedLoopCount() == 0)
            {
                throw std::runtime_error(
                    "Custom OGG loop boundary was not crossed.");
            }

        }

        // VerifyStartFrame(stream: test stream): start frameの範囲補正とseekを確認します。
        static void VerifyStartFrame(AudioStreamVoice& stream)
        {
            // 音源中央のframeです。
            const std::uint64_t middle = stream.m_totalFrames / 2;
            stream.SetStartFrame(middle);
            // 指定位置を保持することを確認します。
            if (stream.StartFrame() != middle)
            {
                throw std::runtime_error(
                    "The stream did not keep its start frame.");
            }
            // start frameへ実際にseekすることを確認します。
            if (!stream.SeekFrame(stream.StartFrame())
                || stream.m_frameCursor != middle)
            {
                throw std::runtime_error(
                    "The stream could not seek to its start frame.");
            }
            // 範囲外start frameは0へ戻ることを確認します。
            stream.SetStartFrame(stream.m_totalFrames + 1);
            // fallback値が0になったことを確認します。
            if (stream.StartFrame() != 0)
            {
                throw std::runtime_error(
                    "An out-of-range start frame must fall back to 0.");
            }
        }

        // VerifyLevelMeter(stream: test stream): PCM帯域meterの生成と解放を確認します。
        static void VerifyLevelMeter(AudioStreamVoice& stream)
        {
            stream.ClearLoopRegion();
            stream.SetLoop(false);
            stream.SetLevelMeterEnabled(true);
            // meter bufferと係数が初期化されたことを確認します。
            if (!stream.IsLevelMeterEnabled()
                || stream.m_levelRing.empty()
                || stream.m_levelHopFrames == 0
                || stream.m_bandA1[0] <= 0.0f)
            {
                throw std::runtime_error(
                    "The level meter was not armed.");
            }

            // meter解析前にstreamを巻き戻します。
            if (!stream.SeekFrame(0))
            {
                throw std::runtime_error(
                    "Could not rewind before metering.");
            }
            stream.m_finished = false;
            stream.ResetLevelMeter();

            // PCM frameあたりのbyte数です。
            const std::size_t bytesPerFrame =
                static_cast<std::size_t>(stream.m_channels)
                * sizeof(std::int16_t);
            // DecodeChunk出力を受けるPCM bufferです。
            std::vector<std::uint8_t> decoded(4096 * bytesPerFrame);
            // meterへ送ったframe数です。
            std::uint64_t submitted{};
            // 2秒分または音源終端まで解析します。
            const std::uint64_t wanted = std::min<std::uint64_t>(
                stream.m_totalFrames,
                static_cast<std::uint64_t>(stream.m_sampleRate) * 2);
            // 必要なframe数までdecodeしてmeterへ渡します。
            while (submitted < wanted)
            {
                // 次のPCM chunkをdecodeします。
                const auto bytes = stream.DecodeChunk(
                    decoded.data(), decoded.size());
                // 音源終端なら解析を終えます。
                if (bytes == 0)
                {
                    break;
                }
                // PCMをmeterへ送り、解析開始frameを伝えます。
                stream.AnalyzeSubmittedPcm(
                    decoded.data(), bytes, submitted);
                submitted += bytes / bytesPerFrame;
            }

            // meterにband sampleがない場合は失敗です。
            if (stream.m_levelRingCount == 0)
            {
                throw std::runtime_error(
                    "The level meter produced no samples.");
            }
            // 解析bandの最大値です。
            float loudest = 0.0f;
            // meter ringの各sampleを検査します。
            for (std::size_t index = 0;
                 index < stream.m_levelRingCount;
                 ++index)
            {
                // 1 sample内の全bandを検査します。
                for (const float level :
                     stream.m_levelRing[index].bands)
                {
                    // 各bandが0から1の範囲内か確認します。
                    if (!(level >= 0.0f) || level > 1.0f)
                    {
                        throw std::runtime_error(
                            "A level band left the 0..1 range.");
                    }
                    loudest = std::max(loudest, level);
                }
            }
            // 音声があるのにmeterが小さい場合は失敗です。
            if (loudest < 0.5f)
            {
                throw std::runtime_error(
                    "The level meter stayed silent on real audio.");
            }
            std::wcout << L"METER samples="
                       << stream.m_levelRingCount
                       << L" loudest=" << loudest << std::endl;

            stream.SetLevelMeterEnabled(false);
            // 無効化後にmeter bufferが解放されたことを確認します。
            if (stream.IsLevelMeterEnabled()
                || !stream.m_levelRing.empty())
            {
                throw std::runtime_error(
                    "Disabling the level meter must release its ring.");
            }
        }

        // VerifyPeakEnvelope(stream: test stream): peak範囲と読出し非破壊性を確認します。
        static void VerifyPeakEnvelope(AudioStreamVoice& stream)
        {
            // 現在位置を置く音源内frameです。
            const std::uint64_t cursor = stream.m_totalFrames / 3;
            // peak読出し前に再生位置を設定します。
            if (!stream.SeekFrame(cursor))
            {
                throw std::runtime_error(
                    "Could not park the cursor before reading peaks.");
            }
            // 読み込んだpeak値を格納するbufferです。
            std::vector<float> peaks(128, -1.0f);
            // bufferへ書き込まれたpeak数です。
            const std::size_t written =
                stream.ReadPeakEnvelope(peaks.data(), peaks.size());
            // 要求した全bucketが埋まることを確認します。
            if (written != peaks.size())
            {
                throw std::runtime_error(
                    "The peak envelope was not filled.");
            }
            // peak読出しが再生位置を変えないことを確認します。
            if (stream.m_frameCursor != cursor)
            {
                throw std::runtime_error(
                    "Reading the peak envelope moved the play cursor.");
            }
            // 読み出したpeakの最大値です。
            float loudest = 0.0f;
            // peak値の範囲と最大値を集計します。
            for (const float peak : peaks)
            {
                // peakは0から1の範囲である必要があります。
                if (!(peak >= 0.0f) || peak > 1.0f)
                {
                    throw std::runtime_error(
                        "A peak left the 0..1 range.");
                }
                loudest = std::max(loudest, peak);
            }
            // 実音声のpeakが平坦でないことを確認します。
            if (loudest < 0.05f)
            {
                throw std::runtime_error(
                    "The peak envelope stayed flat on real audio.");
            }
            std::wcout << L"WAVE buckets=" << written
                       << L" loudest=" << loudest << std::endl;
        }

        // VerifyBassBoost(stream: test stream): 合成toneで低域通過と高域減衰を確認します。
        static void VerifyBassBoost(AudioStreamVoice& stream)
        {
            // 低域test toneの周波数です。
            constexpr double lowHz = 60.0;
            // 高域test toneの周波数です。
            constexpr double highHz = 3000.0;
            // bass shelfに設定する減衰dBです。
            constexpr double boostDb = 6.0;

            // 音源のchannel数です。
            const auto channels =
                static_cast<std::size_t>(stream.m_channels);
            // 音源のsample rateです。
            const auto sampleRate =
                static_cast<double>(stream.m_sampleRate);
            // 0.5秒分の合成test frame数です。
            const std::size_t frames =
                static_cast<std::size_t>(sampleRate * 0.5);
            // 低域と高域toneを格納するPCM bufferです。
            std::vector<std::uint8_t> pcm(
                frames * channels * sizeof(std::int16_t));
            // PCM bufferをsigned 16-bit sample列として書きます。
            auto* samples =
                reinterpret_cast<std::int16_t*>(pcm.data());
                // 合成toneのframeをPCMへ生成します。
            for (std::size_t frame = 0; frame < frames; ++frame)
            {
                // frameの経過秒です。
                const double time =
                    static_cast<double>(frame) / sampleRate;
                // 低域と高域を合成したsample値です。
                const double value =
                    std::sin(2.0 * 3.14159265358979 * lowHz * time) * 0.3
                    + std::sin(
                        2.0 * 3.14159265358979 * highHz * time) * 0.3;
                // 正規化信号をsigned 16-bitへ変換したsampleです。
                const auto sample = static_cast<std::int16_t>(
                    std::lround(value * 32767.0));
                // 同じtest signalを各channelへ複製します。
                for (std::size_t channel = 0;
                     channel < channels;
                     ++channel)
                {
                    samples[frame * channels + channel] = sample;
                }
            }

            // amplitude(block: PCM, frequency: Hz): 指定周波数のPCM振幅を返します。
            auto amplitude = [&](const std::vector<std::uint8_t>& block,
                                 const double frequency)
            {
                // PCM bufferをsigned 16-bit sample列として読みます。
                const auto* values =
                    reinterpret_cast<const std::int16_t*>(block.data());
                // 周波数相関の実部累積値です。
                double real = 0.0;
                // 周波数相関の虚部累積値です。
                double imaginary = 0.0;
                // 全frameを指定周波数と相関します。
                for (std::size_t frame = 0; frame < frames; ++frame)
                {
                    // frameの経過秒です。
                    const double time =
                        static_cast<double>(frame) / sampleRate;
                    // 指定周波数の位相角です。
                    const double angle =
                        2.0 * 3.14159265358979 * frequency * time;
                    // 左channelのsigned PCM sampleです。
                    const double value = static_cast<double>(
                        values[frame * channels]);
                    real += value * std::cos(angle);
                    imaginary += value * std::sin(angle);
                }
                return 2.0
                    * std::sqrt(real * real + imaginary * imaginary)
                    / static_cast<double>(frames);
            };

            // bass boost前の低域振幅です。
            const double plainLow = amplitude(pcm, lowHz);
            // bass boost前の高域振幅です。
            const double plainHigh = amplitude(pcm, highHz);

            // boost適用後のPCM copyです。
            std::vector<std::uint8_t> boosted = pcm;
            stream.SetBassBoost(
                static_cast<float>(boostDb), 110.0f);
            stream.ApplyBassBoost(boosted.data(), boosted.size());
            // boost後の低域振幅です。
            const double boostedLow = amplitude(boosted, lowHz);
            // boost後の高域振幅です。
            const double boostedHigh = amplitude(boosted, highHz);

            // lowDb: boost前後の低域差(dB)。
            const double lowDb =
                20.0 * std::log10(boostedLow / plainLow);
            // 低域の変化が許容幅を超えたら失敗です。
            if (std::abs(lowDb) > 1.5)
            {
                throw std::runtime_error(
                    "The bass shelf should pass the low tone through.");
            }
            // highDb: boost前後の高域差(dB)。
            const double highDb =
                20.0 * std::log10(boostedHigh / plainHigh);
            // 高域が設定値どおり減衰しない場合は失敗です。
            if (std::abs(highDb + boostDb) > 1.5)
            {
                throw std::runtime_error(
                    "The bass shelf did not trim the high tone.");
            }
            // boost後PCMにclippingがないことを確認します。
            for (std::size_t index = 0;
                 index < boosted.size() / sizeof(std::int16_t);
                 ++index)
            {
                // 確認対象のsigned 16-bit sampleです。
                const auto value = reinterpret_cast<const std::int16_t*>(
                    boosted.data())[index];
                // 最大または最小sampleに達したらclippingです。
                if (value == std::numeric_limits<std::int16_t>::min()
                    || value == std::numeric_limits<std::int16_t>::max())
                {
                    throw std::runtime_error(
                        "The bass boost clipped the signal.");
                }
            }
            // boost処理が使うmakeup gainです。
            const float makeup = stream.BassBoostMakeup();
            // 6dB補正を戻すgain値を確認します。
            if (!(makeup > 1.99f && makeup < 2.01f))
            {
                throw std::runtime_error(
                    "BassBoostMakeup should undo the 6 dB trim.");
            }
            std::wcout << L"BASS low " << lowDb << L"dB  high "
                       << highDb << L"dB" << std::endl;
            stream.SetBassBoost(0.0f);
        }

        // VerifyPlaybackFrame(stream: test stream): 無音streamのplayback位置を確認します。
        static void VerifyPlaybackFrame(AudioStreamVoice& stream)
        {
            // 音源の1/4位置を再生開始位置にします。
            const std::uint64_t start = stream.m_totalFrames / 4;
            stream.SetStartFrame(start);
            // PCM未送信中は開始位置を返すことを確認します。
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
    // RunProbe(root: fixture directory): root以下のOGGをdecodeして音声経路を検証します。
    int RunProbe(const std::filesystem::path& root)
    {
        // 検証対象のOGG file一覧です。
        std::vector<std::filesystem::path> files;
        // root以下を再帰的に走査します。
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root))
        {
            // 通常file以外は対象外です。
            if (!entry.is_regular_file())
            {
                continue;
            }
            // 拡張子比較用の小文字文字列です。
            auto extension = entry.path().extension().wstring();
            std::ranges::transform(
                extension,
                extension.begin(),
                ::towlower);
            // OGG fileのみ一覧へ追加します。
            if (extension == L".ogg")
            {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
        // fixtureがない場合はprobeを失敗させます。
        if (files.empty())
        {
            throw std::runtime_error(
                "No OGG test fixture was found.");
        }

        // filesystem経由のasset load用managerです。
        LamaPon::AssetManager assets(nullptr, nullptr);
        // probe対象のaudio decoderです。
        LamaPon::AudioSystem audio;
        // 各OGG fileのdecode・streaming動作を確認します。
        for (const auto& path : files)
        {
            std::wcout << L"TEST " << path.filename().wstring()
                       << std::endl;
            // OGG fileをsound effectとしてdecodeします。
            auto sound = audio.LoadSoundEffect(assets, path);
            // decode結果が空ならfixture不正です。
            if (!sound)
            {
                throw std::runtime_error(
                    "Decoded OGG contains no samples.");
            }
            // decode結果のWAVE formatです。
            const auto* format = sound->GetFormat();
            std::wcout
                << L"FORMAT tag=" << format->wFormatTag
                << L" channels=" << format->nChannels
                << L" rate=" << format->nSamplesPerSec
                << L" bits=" << format->wBitsPerSample
                << L" align=" << format->nBlockAlign
                << L" bytes=" << sound->GetSampleSizeInBytes()
                << std::endl;
            // sound effect instanceの再生・停止経路を通します。
            auto instance = sound->CreateInstance();
            instance->Play(false);
            instance->Stop();
            // OGG streaming decoderを作成します。
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
        // 正常にprobeしたfile数を表示します。
        std::wcout << L"Decoded " << files.size()
                   << L" OGG files.\n";
        return 0;
    }
}

// wmain(argumentCount: 個数, arguments: 値): 引数を検査しCOM寿命内でprobeを実行します。
int wmain(const int argumentCount, wchar_t** arguments)
{
    // audio directory引数が1つだけ必要です。
    if (argumentCount != 2)
    {
        std::wcerr
            << L"Usage: LamaPonAudioOggProbe <audio directory>\n";
        return 2;
    }

    // 現threadのCOM初期化結果です。
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // この関数でCOM初期化した場合は終了時に解除します。
    const bool uninitialize = SUCCEEDED(comResult);

    // exitCode: probeのprocess終了値。
    int exitCode = 1;
    // probe例外をprocess failureへ変換します。
    try
    {
        exitCode = RunProbe(std::filesystem::path(arguments[1]));
    }
    // std exceptionの内容を診断出力へ表示します。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        exitCode = 1;
    }

    // RunProbeのCOM依存を破棄した後、初期化済みCOMを終了します。
    if (uninitialize)
    {
        CoUninitialize();
    }
    return exitCode;
}
