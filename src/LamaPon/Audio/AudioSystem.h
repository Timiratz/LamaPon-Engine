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
    struct MemorySnapshotEntry;

    // バス音量と一時倍率・ソース音量・マスター音量を掛け合わせて再生する。
    enum class AudioBus : std::uint8_t
    {
        // 効果音用のバス
        Effects = 0,
        // 音楽用のバス
        Music = 1,
        // 音量設定列のバス件数
        Count
    };

    // 元音源を保持し、送信前にPCM16へ復号する。
    // AudioSystemは外部の効果音・ストリーム・再生インスタンスより長く存続させる。
    class AudioStreamVoice final
    {
    public:
        // 再生を停止して音声ボイスとVorbisデコーダーを解放する。
        ~AudioStreamVoice();

        // ストリームボイスの複製を禁止する。
        AudioStreamVoice(const AudioStreamVoice&) = delete;
        // ストリームボイスのコピー代入を禁止する。
        AudioStreamVoice& operator=(
            const AudioStreamVoice&) = delete;

        // 開始位置から再生し直しループ回数と推定位置を初期化する。
        void Play();
        // 再生ボイスを一時停止する。
        void Pause() noexcept;
        // 一時停止した再生ボイスを再開する。
        void Resume();
        // 再生とPCM供給を停止する。
        void Stop() noexcept;
        // ボイス音量を設定し低音補正も反映する(volume: 0〜4の倍率)。
        void SetVolume(float volume);
        // 再生ピッチを設定する(pitch: -1〜1の変化量)。
        void SetPitch(float pitch);
        // 左右の定位を設定する(pan: -1〜1の左右位置)。
        void SetPan(float pan);
        // ループ再生の設定を切り替える(loop: 反復再生の許可)。
        void SetLoop(const bool loop) noexcept
        {
            m_loop = loop;
        }
        // 無効な範囲は全曲ループに戻し、先頭PCM準備の失敗では重ね合成だけを解除する。
        // 重ねた先頭区間は再生し直さず、その直後からループを再開する。
        // 半開区間と任意の線形クロスフェードを設定する(startFrame: 反復の開始フレーム, endFrame: 含めない反復終端, crossfadeFrames: 区間長の半分までの重ね幅)。
        void SetLoopRegionFrames(
            std::uint64_t startFrame,
            std::uint64_t endFrame,
            std::uint64_t crossfadeFrames = 0) noexcept;
        // 指定ループ区間とクロスフェードを解除する。
        void ClearLoopRegion() noexcept;
        // 音声ボイスの状態を返し未作成ならSTOPPEDとする。
        [[nodiscard]] DirectX::SoundState
            State() const noexcept;
        // スピーカーで鳴り終えた回数を表すものではない。
        // PCM先読みで完了したループ回数を返す。
        [[nodiscard]] std::uint64_t CompletedLoopCount() const noexcept
        {
            return m_completedLoopCount;
        }

        // 次のPlayの開始位置を設定する(frame: 範囲外なら先頭へ戻す音源位置)。
        void SetStartFrame(std::uint64_t frame) noexcept;
        // 次のPlayで使う音源内の開始位置を返す。
        [[nodiscard]] std::uint64_t StartFrame() const noexcept
        {
            return m_startFrame;
        }


        // 解析する帯域の件数
        static constexpr int LevelBandCount = 12;
        // 帯域メーターを切り替え無効時は記録領域も解放する(enabled: PCM解析の許可)。
        void SetLevelMeterEnabled(bool enabled);
        // 帯域メーターの設定を返す。
        [[nodiscard]] bool IsLevelMeterEnabled() const noexcept
        {
            return m_levelMeterEnabled;
        }
        // メーターが無効・記録なし・再生中以外では書き込める範囲を0にする。
        // 低域から高域の相対レベルを出力し件数を返す(destination: 出力先, capacity: 出力可能な要素数)。
        std::size_t ReadLevelBands(
            float* destination,
            std::size_t capacity) const noexcept;

        // 位置記録がなければ開始位置を返し、停止・ピッチ変更による補正はしない。
        // 送信完了量と実時間から音源内の再生位置を推定する。
        [[nodiscard]] std::uint64_t PlaybackFrame() const noexcept;

        // 音源の全チャンネル共通の総フレーム数を返す。
        [[nodiscard]] std::uint64_t TotalFrames() const noexcept
        {
            return m_totalFrames;
        }
        // 音源の毎秒サンプルフレーム数を返す。
        [[nodiscard]] int SampleRate() const noexcept
        {
            return m_sampleRate;
        }
        // 音源のチャンネル数を返す。
        [[nodiscard]] int ChannelCount() const noexcept
        {
            return m_channels;
        }

        // 減衰の戻し倍率はボイスへ反映するが、最終音量は4倍までに制限する。
        // PCMを減衰して低音棚フィルターを設定する(gainDb: 最低0dBの補正量, cornerHz: 最低20Hzの境界周波数)。
        void SetBassBoost(float gainDb, float cornerHz = 110.0f);
        // 設定した低音補正のdB値を返す。
        [[nodiscard]] float BassBoostDb() const noexcept
        {
            return m_bassBoostDb;
        }
        // PCMの減衰を補うボイス音量倍率を返す。
        [[nodiscard]] float BassBoostMakeup() const noexcept;

        // 全音源を復号して読み取り位置の復元を試すため、停止中か別ストリームで行う。
        // 音源全体の最大振幅を区間別に出力する(destination: 出力先, bucketCount: 出力区間数)。
        std::size_t ReadPeakEnvelope(
            float* destination,
            std::size_t bucketCount);

    private:
        friend class AudioSystem;
        friend struct AudioStreamVoiceTestAccess;

        // AudioSystemから空の再生状態を作る。
        AudioStreamVoice() = default;
        // 先読みPCMをリングから供給し再生完了量を記録する(voice: このストリームの再生ボイス)。
        void FeedBuffer(
            DirectX::DynamicSoundEffectInstance& voice);
        // ループ境界と重ね幅を考慮してPCMを出力する(destination: 出力先, capacity: 出力先のバイト容量)。
        [[nodiscard]] std::size_t DecodeChunk(
            std::uint8_t* destination,
            std::size_t capacity);
        // 位置を進めながらPCM16フレームを復号する(destination: 全チャンネル分の出力先, frameCount: 出力先が収容するフレーム数)。
        [[nodiscard]] std::size_t DecodeRawFrames(
            std::uint8_t* destination,
            std::uint64_t frameCount);
        // 音源の読み取り位置を変更する(frame: 音源終端を含むフレーム位置)。
        [[nodiscard]] bool SeekFrame(
            std::uint64_t frame) noexcept;
        // 通常デコーダーを進めずクロスフェードの先頭PCMを準備する。
        [[nodiscard]] bool PrepareLoopHead() noexcept;
        // 帯域フィルターと解析履歴を初期化する。
        void ResetLevelMeter() noexcept;
        // 転送前のPCM16へ低音補正を上書きする(pcm: 全チャンネルのPCM, bytes: PCMバイト容量)。
        void ApplyBassBoost(
            std::uint8_t* pcm, std::size_t bytes) noexcept;
        // 低音補正の戻し倍率と要求音量を合わせ0〜4で適用する。
        void ApplyVoiceVolume();
        // 低音補正後のPCMを帯域別に解析し履歴へ記録する(pcm: 借用PCM16, bytes: PCMバイト容量, streamFrameStart: 送信列の開始フレーム)。
        void AnalyzeSubmittedPcm(
            const std::uint8_t* pcm,
            std::size_t bytes,
            std::uint64_t streamFrameStart) noexcept;
        // 完了バッファ量と標準サンプルレートで送信列の位置を推定する。
        [[nodiscard]] std::uint64_t PlayedStreamFrame() const noexcept;

        // デコーダーが借用する元音源
        std::vector<std::uint8_t> m_sourceBytes;
        // 所有するVorbisデコーダー
        void* m_vorbis{};
        // OGG復号経路の識別
        bool m_isVorbis{};
        // ループ再生の許可
        bool m_loop{};
        // PCM供給の完了状態
        bool m_finished{};
        // 音源のチャンネル数
        int m_channels{};
        // 毎秒サンプルフレーム数
        int m_sampleRate{};
        // 音源の総フレーム数
        std::uint64_t m_totalFrames{};
        // 音源の次回復号位置
        std::uint64_t m_frameCursor{};
        // 反復の開始フレーム
        std::uint64_t m_loopStartFrame{};
        // 含めない反復終端
        std::uint64_t m_loopEndFrame{};
        // 末尾と先頭の重ね幅
        std::uint64_t m_loopCrossfadeFrames{};
        // 重ね区間の進捗フレーム
        std::uint64_t m_crossfadeProgressFrames{};
        // 指定反復区間の有効性
        bool m_hasLoopRegion{};
        // 末尾と先頭の合成中判定
        bool m_crossfadeActive{};
        // PCM供給の受付状態
        bool m_playRequested{};
        // 先読み中の反復完了回数
        std::uint64_t m_completedLoopCount{};
        // 合成用の先頭PCM16
        std::vector<std::int16_t> m_loopHeadPcm;
        // 元WAV内のPCM開始位置
        std::size_t m_dataOffset{};
        // 元WAVのPCMバイト容量
        std::size_t m_dataSize{};
        // WAV内の次回読込バイト
        std::size_t m_dataCursor{};
        // 所有する再生ボイス
        std::unique_ptr<
            DirectX::DynamicSoundEffectInstance>
            m_instance;
        // 送信PCMを保持する枠数
        static constexpr std::size_t BufferCount = 4;
        // 各送信枠のバイト容量
        static constexpr std::size_t BufferBytes =
            64 * 1024;
        // 送信完了まで保持するPCM
        std::array<
            std::vector<std::uint8_t>,
            BufferCount> m_buffers;
        // 次に使うPCM送信枠
        std::size_t m_nextBuffer{};


        // 次のPlayの開始位置
        std::uint64_t m_startFrame{};


        // 累積送信フレーム数
        std::uint64_t m_submittedFrames{};
        // 完了確認済みフレーム数
        std::uint64_t m_consumedFrames{};
        // 完了量を確定した単調時刻
        std::chrono::steady_clock::time_point m_consumedAt{};
        // 送信順の各PCM枠フレーム数
        std::array<std::uint64_t, BufferCount> m_queuedFrames{};
        // 未完了PCM枠の先頭
        std::size_t m_queuedHead{};
        // 未完了PCM枠の件数
        std::size_t m_queuedCount{};

        struct PositionMarker final
        {
            // 送信列の累積フレーム位置
            std::uint64_t streamFrame{};
            // 対応する音源内フレーム
            std::uint64_t fileFrame{};
        };
        // 復号位置を保持する枠数
        static constexpr std::size_t PositionMarkerCount = 32;
        // 送信列と音源位置の対応列
        std::array<PositionMarker, PositionMarkerCount> m_positions{};
        // 次の位置記録枠
        std::size_t m_positionNext{};
        // 有効な位置記録の件数
        std::size_t m_positionCount{};


        struct LevelSample final
        {
            // 解析済み送信フレーム位置
            std::uint64_t streamFrame{};
            // 低域から高域の相対レベル
            std::array<float, LevelBandCount> bands{};
        };
        // 帯域メーターの解析許可
        bool m_levelMeterEnabled{};
        // 帯域フィルターの係数A1
        std::array<float, LevelBandCount> m_bandA1{};
        // 帯域フィルターの係数A2
        std::array<float, LevelBandCount> m_bandA2{};
        // 帯域フィルターの係数A3
        std::array<float, LevelBandCount> m_bandA3{};
        // 帯域フィルターの状態1
        std::array<float, LevelBandCount> m_bandIc1{};
        // 帯域フィルターの状態2
        std::array<float, LevelBandCount> m_bandIc2{};
        // 帯域別の振幅包絡
        std::array<float, LevelBandCount> m_bandEnvelope{};
        // 帯域別の基準ピーク
        std::array<float, LevelBandCount> m_bandPeak{};
        // 振幅包絡の減衰係数
        float m_levelReleaseCoefficient{ 1.0f };
        // 基準ピークの減衰係数
        float m_levelPeakDecay{ 1.0f };
        // 解析記録の間隔フレーム
        std::uint64_t m_levelHopFrames{ 1 };
        // 次の解析記録までの進捗
        std::uint64_t m_levelHopCursor{};
        // 有効時のみ保持する解析列
        std::vector<LevelSample> m_levelRing;
        // 次の解析記録枠
        std::size_t m_levelRingNext{};
        // 有効な解析記録の件数
        std::size_t m_levelRingCount{};


        // 低音補正の最大チャンネル数
        static constexpr std::size_t MaximumChannels = 8;
        // 要求されたボイス音量
        float m_volume{ 1.0f };
        // 低音補正のdB値
        float m_bassBoostDb{};
        // 低音補正の境界周波数
        float m_bassCornerHz{ 110.0f };
        // 低音棚の入力係数B0
        float m_bassB0{ 1.0f };
        // 低音棚の入力係数B1
        float m_bassB1{};
        // 低音棚の入力係数B2
        float m_bassB2{};
        // 低音棚の出力係数A1
        float m_bassA1{};
        // 低音棚の出力係数A2
        float m_bassA2{};
        // チャンネル別の入出力履歴
        std::array<std::array<float, 4>, MaximumChannels> m_bassState{};
    };

    // 設定・更新・ボイス操作は同じスレッドで直列化する。
    class AudioSystem final
    {
    public:
        // 既定の音声デバイスで音声エンジンを作る。
        AudioSystem();
        // 保持する効果音と音声エンジンを解放する。
        ~AudioSystem();

        // 音声エンジンの複製を禁止する。
        AudioSystem(const AudioSystem&) = delete;
        // 音声エンジンのコピー代入を禁止する。
        AudioSystem& operator=(const AudioSystem&) = delete;

        // 指定パス文字列がキーとなるため、呼び出し側でパス表記を正規化する。
        // WAV・OGGを全復号して効果音を共有する(assets: 読み込み管理, path: 音源パス)。
        [[nodiscard]] std::shared_ptr<DirectX::SoundEffect>
            LoadSoundEffect(
                AssetManager& assets,
                const std::filesystem::path& path);
        // OGGまたはPCM16のWAVから未再生のストリームを作る(assets: 読み込み管理, path: 音源パス)。
        [[nodiscard]] std::shared_ptr<AudioStreamVoice>
            CreateStream(
                AssetManager& assets,
                const std::filesystem::path& path);
        // リセット成功時に世代を更新し、既存ボイスの再生成を呼び出し側へ委ねる。
        // 音声エンジンを更新し重大エラー時はデバイスをリセットする。
        bool Update();
        // 共有効果音を解除して未使用ボイスを整理する。
        void Clear();

        // エンジン全体を停止または再開する(suspended: 一時停止するか)。
        void SetSuspended(bool suspended);
        // エンジン全体の一時停止設定を返す。
        [[nodiscard]] bool IsSuspended() const noexcept
        {
            return m_suspended;
        }

        // 全バス共通の音量を設定する(volume: 0〜1の倍率)。
        void SetMasterVolume(float volume);
        // 音声エンジンの全体音量を返す。
        [[nodiscard]] float MasterVolume() const noexcept;
        // 有効なバスの設定音量を更新する(bus: 対象バス, volume: 0〜1の倍率)。
        void SetBusVolume(AudioBus bus, float volume);
        // バス設定音量を返し無効なバスなら1とする(bus: 対象バス)。
        [[nodiscard]] float BusVolume(
            AudioBus bus) const noexcept;
        // 設定音量を変えず一時倍率を更新する(bus: 対象バス, gain: 非有限値は1の0〜1倍率)。
        void SetBusFade(AudioBus bus, float gain);
        // バスの一時倍率を返し無効なバスなら1とする(bus: 対象バス)。
        [[nodiscard]] float BusFade(
            AudioBus bus) const noexcept;
        // 設定音量と一時倍率の積を返す(bus: 対象バス)。
        [[nodiscard]] float EffectiveBusVolume(
            AudioBus bus) const noexcept;
        // 再生デバイスの接続状態を返す。
        [[nodiscard]] bool IsDevicePresent() const noexcept;
        // 全復号した効果音のキャッシュ件数を返す。
        [[nodiscard]] std::size_t CachedSoundCount() const noexcept
        {
            return m_soundCache.size();
        }
        // ストリームを除く効果音PCMの内訳を追記する(entries: 既存内容を保つ追加先)。
        void AppendMemoryEntries(
            std::vector<MemorySnapshotEntry>& entries) const;
        // 音声エンジンの資源統計を返す。
        [[nodiscard]] DirectX::AudioStatistics Statistics() const;
        // 音声デバイスのリセット世代を返す。
        [[nodiscard]] std::uint64_t DeviceGeneration() const noexcept
        {
            return m_deviceGeneration;
        }
        // 重複を除いて借用リスナーを登録する(listener: 登録中に存続する対象)。
        void RegisterListener(AudioListenerComponent& listener);
        // 借用リスナーの登録を解除する(listener: 登録解除する対象)。
        void UnregisterListener(
            AudioListenerComponent& listener) noexcept;
        // 有効な所有物体の有効リスナーを登録順に借用する。
        [[nodiscard]] AudioListenerComponent*
            ActiveListener() const noexcept;

    private:
        // 指定パスの文字列を小文字化する(path: 呼び出し側が正規化する音源パス)。
        static std::wstring MakeCacheKey(
            const std::filesystem::path& path);

        // 所有する音声エンジン
        std::unique_ptr<DirectX::AudioEngine> m_engine;
        // 共有する全復号済み効果音
        std::unordered_map<
            std::wstring,
            std::shared_ptr<DirectX::SoundEffect>> m_soundCache;
        // 登録中の借用リスナー
        std::vector<AudioListenerComponent*> m_listeners;
        // デバイスリセットの世代
        std::uint64_t m_deviceGeneration{};
        // エンジン全体の停止設定
        bool m_suspended{};
        // プレイヤー設定のバス音量
        std::array<
            float,
            static_cast<std::size_t>(AudioBus::Count)>
            m_busVolumes{ 1.0f, 1.0f };
        // バス別の一時音量倍率
        std::array<
            float,
            static_cast<std::size_t>(AudioBus::Count)>
            m_busFades{ 1.0f, 1.0f };
    };
}
