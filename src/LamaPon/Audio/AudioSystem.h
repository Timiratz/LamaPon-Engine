#pragma once

#include <Audio.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    class AssetManager;
    class AudioListenerComponent;
    class AudioSystem;
    struct AudioStreamVoiceTestAccess;

    // ミキサーのバス。ソースはいずれかのバスに属し、
    // バス音量×ソース音量×マスター音量で再生されます。
    enum class AudioBus : std::uint8_t
    {
        Effects = 0,
        Music = 1,
        Count
    };

    // ストリーミング再生ボイス（長尺BGM用）。圧縮データだけを
    // メモリに保持し、再生しながら少しずつPCMへデコードします。
    class AudioStreamVoice final
    {
    public:
        ~AudioStreamVoice();

        AudioStreamVoice(const AudioStreamVoice&) = delete;
        AudioStreamVoice& operator=(
            const AudioStreamVoice&) = delete;

        void Play();
        void Pause() noexcept;
        void Resume();
        void Stop() noexcept;
        // 音量。1.0が原音で、**1.0を超える増幅も指定できます**
        // （上限4.0）。小さくマスタリングされた音源を持ち上げるのに
        // 要ります。音源のピークによっては歪むので、上げるなら
        // ピークを測ってから決めてください。
        void SetVolume(float volume);
        void SetPitch(float pitch);
        void SetPan(float pan);
        void SetLoop(const bool loop) noexcept
        {
            m_loop = loop;
        }
        // ループ範囲は各チャンネル共通のsample frameで指定します。
        // Playは常にファイル先頭から始まり、最初にendへ達した後だけ
        // [start, end)を繰り返します。crossfadeFramesを指定すると
        // end直前のtailとstart直後のheadを線形合成し、合成済みheadの
        // 重複を避けてstart+crossfadeFramesから続けます。範囲が不正なら
        // 従来どおりファイル全体をループします。
        void SetLoopRegionFrames(
            std::uint64_t startFrame,
            std::uint64_t endFrame,
            std::uint64_t crossfadeFrames = 0) noexcept;
        void ClearLoopRegion() noexcept;
        [[nodiscard]] DirectX::SoundState
            State() const noexcept;
        // 再生デバイスへ先読みするPCM生成時点で完了したloop回数。
        // 実際にスピーカーから鳴り終えた回数ではありません。
        [[nodiscard]] std::uint64_t CompletedLoopCount() const noexcept
        {
            return m_completedLoopCount;
        }

        // 再生開始位置（各チャンネル共通のsample frame）。Playはここから
        // 鳴り始めます。ループ範囲を併用した場合は「開始位置→loopEnd」を
        // 一度鳴らしてから[start, end)の反復へ入ります。曲の途中だけを
        // 試聴させたいとき、音源を切り出さずに済ませるための入口です。
        void SetStartFrame(std::uint64_t frame) noexcept;
        [[nodiscard]] std::uint64_t StartFrame() const noexcept
        {
            return m_startFrame;
        }

        // ---- 帯域レベルメーター（音楽ビジュアライザ用） ----
        // 既定は無効で、無効な間は解析も確保も一切行いません
        // （PCMを触らないので再生経路のコストは0のままです）。
        // 有効にすると、デバイスへ送るPCMそのものを12帯域の
        // bandpassで追い、**いま鳴っている位置**のレベルを返します。
        // 先読みしたPCMではなくbuffer完了callbackを起点に実時間で
        // 補間するため、画面の棒と耳で聞こえる音がずれません。
        static constexpr int LevelBandCount = 12;
        void SetLevelMeterEnabled(bool enabled);
        [[nodiscard]] bool IsLevelMeterEnabled() const noexcept
        {
            return m_levelMeterEnabled;
        }
        // 低域→高域の順に0..1で書き込み、書き込めた数を返します。
        // 無効時・停止中は0を書き込みます。
        std::size_t ReadLevelBands(
            float* destination,
            std::size_t capacity) const noexcept;

        // いま鳴っている位置（音源内のsample frame）。ループ区間の
        // 折り返しも追えます。再生していないときは開始位置を返します。
        // 進捗バー、歌詞の同期、波形上の再生ヘッドなどに使えます。
        [[nodiscard]] std::uint64_t PlaybackFrame() const noexcept;

        // 音源そのものの諸元。秒へ直したり進捗を出したりするのに使います。
        [[nodiscard]] std::uint64_t TotalFrames() const noexcept
        {
            return m_totalFrames;
        }
        [[nodiscard]] int SampleRate() const noexcept
        {
            return m_sampleRate;
        }
        [[nodiscard]] int ChannelCount() const noexcept
        {
            return m_channels;
        }

        // ---- 低音の持ち上げ（low shelf） ----
        // cornerHzより下をgainDbだけ持ち上げます。BGMを車内やスマホの
        // スピーカーで鳴らすと低域が痩せるので、音源を作り直さずに
        // 補うための入口です。0dB（既定）なら一切処理しません。
        //
        // 中では、クリップを避けるためPCM全体をgainDbぶん下げてから
        // 棚を掛けます（int16を持ち上げると簡単に振り切れるため）。
        // **下げたぶんは再生音量側で自動的に戻す**ので、呼ぶ側は
        // SetVolumeを変える必要はありません。聞こえ方は
        // 「中高域はそのまま、低域だけ上がる」になります。
        void SetBassBoost(float gainDb, float cornerHz = 110.0f);
        [[nodiscard]] float BassBoostDb() const noexcept
        {
            return m_bassBoostDb;
        }
        // 内部で下げたぶんを戻すために音量へ掛けている倍率
        // （0dBなら1.0）。自動で掛かるので普段は見る必要はありません。
        [[nodiscard]] float BassBoostMakeup() const noexcept;

        // 表示用のピーク包絡。音源全体をbucketCount個の区間へ均等に
        // 割り、区間ごとの|最大振幅|を0..1で書き込みます。書き込めた
        // 数を返します。
        //
        // **重い呼び出しです。** 音源全体をデコードし直すので、5分の
        // OGGで0.3秒ほど止まります。読み終えたら元の再生位置へ戻し
        // ますが、鳴らしている最中に呼ぶとその間の供給が滞って音が
        // 途切れます。波形の絵は停止中に一度だけ作るか、表示用に別の
        // streamを作って呼んでください。
        std::size_t ReadPeakEnvelope(
            float* destination,
            std::size_t bucketCount);

    private:
        friend class AudioSystem;
        friend struct AudioStreamVoiceTestAccess;

        AudioStreamVoice() = default;
        void FeedBuffer(
            DirectX::DynamicSoundEffectInstance& voice);
        [[nodiscard]] std::size_t DecodeChunk(
            std::uint8_t* destination,
            std::size_t capacity);
        [[nodiscard]] std::size_t DecodeRawFrames(
            std::uint8_t* destination,
            std::uint64_t frameCount);
        [[nodiscard]] bool SeekFrame(
            std::uint64_t frame) noexcept;
        [[nodiscard]] bool PrepareLoopHead() noexcept;
        void ResetLevelMeter() noexcept;
        // 送信直前のPCMへ低音の持ち上げを掛けます（その場で書き換え）。
        void ApplyBassBoost(
            std::uint8_t* pcm, std::size_t bytes) noexcept;
        // m_volume×戻し倍率をvoiceへ流し込みます。
        void ApplyVoiceVolume();
        // 送信直前のPCMを解析し、hopごとの結果をringへ積みます。
        void AnalyzeSubmittedPcm(
            const std::uint8_t* pcm,
            std::size_t bytes,
            std::uint64_t streamFrameStart) noexcept;
        // buffer完了を起点に実時間で補間した「再生済みframe数」。
        [[nodiscard]] std::uint64_t PlayedStreamFrame() const noexcept;

        std::vector<std::uint8_t> m_sourceBytes;
        // stb_vorbisハンドル（.cpp内でのみ実体型を使用）。
        void* m_vorbis{};
        bool m_isVorbis{};
        bool m_loop{};
        bool m_finished{};
        int m_channels{};
        int m_sampleRate{};
        std::uint64_t m_totalFrames{};
        std::uint64_t m_frameCursor{};
        std::uint64_t m_loopStartFrame{};
        std::uint64_t m_loopEndFrame{};
        std::uint64_t m_loopCrossfadeFrames{};
        std::uint64_t m_crossfadeProgressFrames{};
        bool m_hasLoopRegion{};
        bool m_crossfadeActive{};
        bool m_playRequested{};
        std::uint64_t m_completedLoopCount{};
        std::vector<std::int16_t> m_loopHeadPcm;
        // WAV（PCM16）のdataチャンク位置。
        std::size_t m_dataOffset{};
        std::size_t m_dataSize{};
        std::size_t m_dataCursor{};
        std::unique_ptr<
            DirectX::DynamicSoundEffectInstance>
            m_instance;
        // 送信済みバッファは再生し終わるまで保持が必要なため
        // リングで回します。
        static constexpr std::size_t BufferCount = 4;
        static constexpr std::size_t BufferBytes =
            64 * 1024;
        std::array<
            std::vector<std::uint8_t>,
            BufferCount> m_buffers;
        std::size_t m_nextBuffer{};

        // ---- ここから下は後から足した項目です（2026-08-29） ----
        // 再生開始位置。Playはここへseekしてから鳴らします。
        std::uint64_t m_startFrame{};

        // 再生位置の推定。SubmitBufferした累計frameと、完了した
        // bufferの累計frameを持ち、完了callbackの瞬間を「ぴったり
        // 合った時刻」として実時間で補間します。毎フレームの更新は
        // 不要で、問い合わせたときだけ計算します。
        std::uint64_t m_submittedFrames{};
        std::uint64_t m_consumedFrames{};
        std::chrono::steady_clock::time_point m_consumedAt{};
        // 送信済みbufferのframe数（FIFO。GetPendingBufferCountと
        // 突き合わせて完了ぶんを確定させる）
        std::array<std::uint64_t, BufferCount> m_queuedFrames{};
        std::size_t m_queuedHead{};
        std::size_t m_queuedCount{};
        // 「送信済みPCMの位置」→「音源内の位置」の対応表。decodeを
        // 呼ぶたびに1件積むので、ループの折り返しも境目で必ず1件
        // 入ります。再生ヘッドを出すためだけの数十バイトです。
        struct PositionMarker final
        {
            std::uint64_t streamFrame{};
            std::uint64_t fileFrame{};
        };
        static constexpr std::size_t PositionMarkerCount = 32;
        std::array<PositionMarker, PositionMarkerCount> m_positions{};
        std::size_t m_positionNext{};
        std::size_t m_positionCount{};

        // 帯域レベルメーター。ringは有効化したときだけ確保します。
        struct LevelSample final
        {
            std::uint64_t streamFrame{};
            std::array<float, LevelBandCount> bands{};
        };
        bool m_levelMeterEnabled{};
        // TPT state-variable filterの係数と状態（帯域ごと）
        std::array<float, LevelBandCount> m_bandA1{};
        std::array<float, LevelBandCount> m_bandA2{};
        std::array<float, LevelBandCount> m_bandA3{};
        std::array<float, LevelBandCount> m_bandIc1{};
        std::array<float, LevelBandCount> m_bandIc2{};
        std::array<float, LevelBandCount> m_bandEnvelope{};
        // 帯域ごとの自動利得。低域と高域の音量差を吸収して
        // どの棒も0..1を使い切るようにします。
        std::array<float, LevelBandCount> m_bandPeak{};
        float m_levelReleaseCoefficient{ 1.0f };
        float m_levelPeakDecay{ 1.0f };
        std::uint64_t m_levelHopFrames{ 1 };
        std::uint64_t m_levelHopCursor{};
        std::vector<LevelSample> m_levelRing;
        std::size_t m_levelRingNext{};
        std::size_t m_levelRingCount{};

        // 低音の持ち上げ。low shelfのbiquadをチャンネルごとに持つ。
        static constexpr std::size_t MaximumChannels = 8;
        // SetVolumeで指定された音量。実際にvoiceへ渡すのは
        // これに低音補正の戻し倍率を掛けた値。
        float m_volume{ 1.0f };
        float m_bassBoostDb{};
        float m_bassCornerHz{ 110.0f };
        float m_bassB0{ 1.0f };
        float m_bassB1{};
        float m_bassB2{};
        float m_bassA1{};
        float m_bassA2{};
        std::array<std::array<float, 4>, MaximumChannels> m_bassState{};
    };

    class AudioSystem final
    {
    public:
        AudioSystem();
        ~AudioSystem();

        AudioSystem(const AudioSystem&) = delete;
        AudioSystem& operator=(const AudioSystem&) = delete;

        [[nodiscard]] std::shared_ptr<DirectX::SoundEffect>
            LoadSoundEffect(
                AssetManager& assets,
                const std::filesystem::path& path);
        // ストリーミング再生ボイスを作ります（.ogg / PCM16の.wav）。
        [[nodiscard]] std::shared_ptr<AudioStreamVoice>
            CreateStream(
                AssetManager& assets,
                const std::filesystem::path& path);
        bool Update();
        void Clear();

        // 音声処理を丸ごと止めます／再開します（エディターの一時停止用）。
        // 個々のAudioSourceを止めるのではなくエンジン側で止めるので、
        // 再開したときに元の位置から続きます。
        void SetSuspended(bool suspended);
        [[nodiscard]] bool IsSuspended() const noexcept
        {
            return m_suspended;
        }

        void SetMasterVolume(float volume);
        [[nodiscard]] float MasterVolume() const noexcept;
        void SetBusVolume(AudioBus bus, float volume);
        [[nodiscard]] float BusVolume(
            AudioBus bus) const noexcept;
        [[nodiscard]] bool IsDevicePresent() const noexcept;
        [[nodiscard]] std::size_t CachedSoundCount() const noexcept
        {
            return m_soundCache.size();
        }
        [[nodiscard]] DirectX::AudioStatistics Statistics() const;
        [[nodiscard]] std::uint64_t DeviceGeneration() const noexcept
        {
            return m_deviceGeneration;
        }
        void RegisterListener(AudioListenerComponent& listener);
        void UnregisterListener(
            AudioListenerComponent& listener) noexcept;
        [[nodiscard]] AudioListenerComponent*
            ActiveListener() const noexcept;

    private:
        static std::wstring MakeCacheKey(
            const std::filesystem::path& path);

        std::unique_ptr<DirectX::AudioEngine> m_engine;
        std::unordered_map<
            std::wstring,
            std::shared_ptr<DirectX::SoundEffect>> m_soundCache;
        std::vector<AudioListenerComponent*> m_listeners;
        std::uint64_t m_deviceGeneration{};
        bool m_suspended{};
        std::array<
            float,
            static_cast<std::size_t>(AudioBus::Count)>
            m_busVolumes{ 1.0f, 1.0f };
    };
}
