#pragma once

#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Scene/Component.h"

#include <Audio.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace LamaPon
{
    class AssetManager;

    // 数値設定は有限値を前提とし、借用する音声管理とアセットはこの部品より長寿命とする。
    class AudioSourceComponent final : public Component
    {
    public:
        // 音源パスと再生・定位設定を持つ部品を作る(audioPath: 音源のパス, volume: 0〜1の音量, pitch: −1〜1のオクターブ指定, pan: −1〜1の左右バランス, loop: 周回する指定, playOnStart: 初回更新で再生する指定, spatial: 3D定位する指定, minimumDistance: 減衰開始の距離, maximumDistance: 無音となる距離)。
        explicit AudioSourceComponent(
            std::filesystem::path audioPath = {},
            float volume = 1.0f,
            float pitch = 0.0f,
            float pan = 0.0f,
            bool loop = false,
            bool playOnStart = false,
            bool spatial = false,
            float minimumDistance = 1.0f,
            float maximumDistance = 20.0f);
        // 管理する再生を停止して音源部品を破棄する。
        ~AudioSourceComponent() override;

        // 音源パスを更新して再読み込みする(path: 音源のパスで空は解除)。
        // 旧音源を先に解除するため、読み込み失敗時も新パスを保持し以前の音源へ戻らない。
        void SetAudioPath(std::filesystem::path path);
        // 設定した音源パスを取得する。
        [[nodiscard]] const std::filesystem::path& AudioPath() const noexcept
        {
            return m_audioPath;
        }

        // 音量を0〜1に収め、管理するボイスへバス音量込みで反映する(volume: 音源の音量)。
        void SetVolume(float volume);
        // バス音量を掛ける前の音源の音量を取得する。
        [[nodiscard]] float Volume() const noexcept { return m_volume; }
        // 音程指定を−1〜1に収めて管理するボイスへ反映する(pitch: オクターブ単位の音程指定)。
        void SetPitch(float pitch);
        // オクターブ単位の音程指定を取得する。
        [[nodiscard]] float Pitch() const noexcept { return m_pitch; }
        // 左右バランスを−1〜1に収め、ストリームか非3Dの主ボイスへ反映する(pan: 左−1から右1のバランス)。
        void SetPan(float pan);
        // 設定した左右バランスを取得する。
        [[nodiscard]] float Pan() const noexcept { return m_pan; }
        // 周回設定を変更し、再生中なら停止後に開始位置から再生し直す(loop: 周回する指定)。
        void SetLoop(bool loop);
        // 周回する設定か確認する。
        [[nodiscard]] bool Loop() const noexcept { return m_loop; }
        // ストリームの周回区間と末尾・先頭の線形合成長を設定する(startFrame: 周回区間の開始フレーム, endFrame: 周回区間の終端で含まない, crossfadeFrames: 合成するサンプルフレーム数)。
        // SetLoop(true)で有効となり、フレームは全チャンネル共通で、不正区間は全体周回に戻り合成長は区間の半分以下となる。
        void SetLoopRegionFrames(
            std::uint64_t startFrame,
            std::uint64_t endFrame,
            std::uint64_t crossfadeFrames = 0) noexcept;
        // ストリームの区間指定を解除して全体周回へ戻す。
        void ClearLoopRegion() noexcept;
        // ストリームが数えた完了ループ数を取得し、未生成なら0を返す。
        [[nodiscard]] std::uint64_t CompletedLoopCount() const noexcept
        {
            return m_stream ? m_stream->CompletedLoopCount() : 0;
        }
        // 初回更新での自動再生を設定する(playOnStart: 自動再生する指定)。
        void SetPlayOnStart(bool playOnStart) noexcept
        {
            m_playOnStart = playOnStart;
        }
        // 初回更新で自動再生する設定か確認する。
        [[nodiscard]] bool PlayOnStart() const noexcept
        {
            return m_playOnStart;
        }
        // 3D定位を切り替えて音源を再生成し、再生中なら再生し直す(spatial: 3D定位する指定)。
        void SetSpatial(bool spatial);
        // 3D定位する設定か確認する。
        [[nodiscard]] bool IsSpatial() const noexcept
        {
            return m_spatial;
        }
        // 減衰開始距離を0.01以上に収め、最大距離もそれより0.01以上離す(distance: 減衰開始距離)。
        void SetMinimumDistance(float distance);
        // 3D定位の減衰開始距離を取得する。
        [[nodiscard]] float MinimumDistance() const noexcept
        {
            return m_minimumDistance;
        }
        // 最大距離を減衰開始距離より0.01以上離して設定する(distance: 無音となる距離)。
        void SetMaximumDistance(float distance);
        // 3D定位で無音となる最大距離を取得する。
        [[nodiscard]] float MaximumDistance() const noexcept
        {
            return m_maximumDistance;
        }
        // 逐次デコードの再生方式を切り替えて再生成し、再生中なら再生し直す(streaming: ストリーム再生する指定)。
        // ストリームは3D定位に対応せず、PlayOneShotも通常のPlayとして扱う。
        void SetStreaming(bool streaming);
        // 逐次デコードのストリーム再生設定か確認する。
        [[nodiscard]] bool IsStreaming() const noexcept
        {
            return m_streaming;
        }
        // 音量バスを変更して現在の音量を管理ボイスへ反映する(bus: 使用する音量バス)。
        void SetBus(AudioBus bus);
        // 使用する音量バスを取得する。
        [[nodiscard]] AudioBus Bus() const noexcept
        {
            return m_bus;
        }

        // 次のストリーム再生を始める位置を記録する(frame: 全チャンネル共通の開始フレーム)。
        // 再生時は音源長以上の指定を0とし、区間周回なら開始位置から区間終端まで再生後に区間を繰り返す。
        void SetStartFrame(std::uint64_t frame) noexcept;
        // 記録した開始フレームの指定値を取得する。
        [[nodiscard]] std::uint64_t StartFrame() const noexcept
        {
            return m_startFrame;
        }

        // 低域から高域へ並ぶ帯域数
        static constexpr int LevelBandCount =
            AudioStreamVoice::LevelBandCount;
        // ストリームの12帯域解析を切り替える(enabled: 帯域解析を行う指定)。
        void SetLevelMeterEnabled(bool enabled);
        // ストリームの帯域解析を有効にしているか確認する。
        [[nodiscard]] bool IsLevelMeterEnabled() const noexcept
        {
            return m_levelMeterEnabled;
        }
        // 低域から高域へ最大12帯域の0〜1の値を書き、書き込んだ数を返す(destination: capacity個分の書き込み先, capacity: 格納可能な要素数)。
        // 音源なし・解析無効・再生停止時は0を詰め、nullptrまたは容量0なら何も書かず0を返す。
        std::size_t ReadLevelBands(
            float* destination,
            std::size_t capacity) const noexcept;

        // ストリームの現在位置を全チャンネル共通のサンプルフレームで取得し、未生成なら0を返す。
        [[nodiscard]] std::uint64_t PlaybackFrame() const noexcept;
        // ストリーム音源の全サンプルフレーム数を取得し、未生成なら0を返す。
        [[nodiscard]] std::uint64_t TotalFrames() const noexcept
        {
            return m_stream ? m_stream->TotalFrames() : 0;
        }
        // ストリーム音源のサンプリング周波数をHzで取得し、未生成なら0を返す。
        [[nodiscard]] int SampleRate() const noexcept
        {
            return m_stream ? m_stream->SampleRate() : 0;
        }

        // ストリームの低域補正を設定し、負のゲインを0に収める(gainDb: 低域を持ち上げるdB値, cornerHz: 補正する低域の境界Hz)。
        // 0dBなら補正を行わず、内部PCMの減衰分はボイス音量側で自動補償する。
        void SetBassBoost(float gainDb, float cornerHz = 110.0f);
        // 設定した低域補正のゲインをdBで取得する。
        [[nodiscard]] float BassBoostDb() const noexcept
        {
            return m_bassBoostDb;
        }
        // ストリーム内部のPCM減衰を音量側で補償する倍率を取得する。
        [[nodiscard]] float BassBoostMakeup() const noexcept
        {
            return m_bassBoostDb > 0.0f
                ? std::pow(10.0f, m_bassBoostDb / 20.0f)
                : 1.0f;
        }

        // ストリーム音源全体を均等区間に分け、区間ごとの最大振幅を書いて要素数を返す(destination: bucketCount個分の書き込み先, bucketCount: 波形を分割する区間数)。
        // 音源全体をデコードして供給を止めるため、停止中に行い、非ストリームなら0を返す。
        std::size_t ReadPeakEnvelope(
            float* destination,
            std::size_t bucketCount);

        // 主ボイスを設定された開始位置から再生し、未生成なら再生を予約する。
        void Play();
        // 単発再生を追加し、未生成なら予約し、ストリームなら通常のPlayを行う。
        void PlayOneShot();
        // 主ボイスまたはストリームを一時停止し、単発ボイスは停止しない。
        void Pause() noexcept;
        // 一時停止した主ボイスまたはストリームを再開する。
        void Resume();
        // 主ボイス・ストリーム・管理する3D単発ボイスを停止して通常再生予約を解除する。
        // 非3Dの単発再生は管理対象外で、未生成時の単発再生予約もここでは解除しない。
        void Stop() noexcept;
        // 主ボイスまたはストリームの状態を取得し、単発ボイスの状態は含めない。
        [[nodiscard]] DirectX::SoundState State() const noexcept;

        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "AudioSource";
        }

    protected:
        // 音源と描画機器内の音声管理・アセットを初期化する(graphics: 所有者より長寿命の描画機器)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // 機器の世代変更、3D定位、バス音量と初回自動再生を更新する(deltaTime: 未使用の経過秒数)。
        void OnUpdate(float deltaTime) override;

    private:
        // 旧ボイスを破棄して再読み込みし、保留中の再生要求を適用する。
        void ReloadSound();
        // 現在の再生設定を主ボイスまたはストリームへ反映する。
        void ApplyProperties();
        // 有効なリスナーとの3D定位を反映し、リスナー不在なら無音にする(instance: 定位する音声ボイス)。
        // 定位更新は音量をm_volumeで上書きするため、バス音量の反映とは別の経路となる。
        void ApplySpatial(
            DirectX::SoundEffectInstance& instance);
        // 開始距離まで一定、最大距離まで線形減衰する曲線を更新し、ドップラーを無効にする。
        void UpdateDistanceCurve() noexcept;

        // 音源の音量へ現在のバス音量を掛けた値を取得する。
        [[nodiscard]] float
            EffectiveVolume() const noexcept;

        // 音源のパス
        std::filesystem::path m_audioPath;
        // バス乗算前の音源音量
        float m_volume{ 1.0f };
        // オクターブ単位の音程指定
        float m_pitch{};
        // 左右バランス
        float m_pan{};
        // 周回する指定
        bool m_loop{};
        // 初回更新で自動再生する指定
        bool m_playOnStart{};
        // 3D定位する指定
        bool m_spatial{};
        // 逐次デコードする指定
        bool m_streaming{};
        // 使用する音量バス
        AudioBus m_bus{ AudioBus::Effects };
        // 最後に反映したバス音量
        float m_lastBusVolume{ 1.0f };
        // 3D減衰開始の距離
        float m_minimumDistance{ 1.0f };
        // 3Dで無音となる距離
        float m_maximumDistance{ 20.0f };
        // 初回更新を実行したか
        bool m_started{};
        // 音源生成後の通常再生予約
        bool m_playRequested{};
        // 音源生成後の単発再生予約
        bool m_oneShotRequested{};
        // 借用したアセット管理
        AssetManager* m_assets{};
        // 借用した音声管理
        AudioSystem* m_audio{};
        // ボイス生成時の機器世代
        std::uint64_t m_deviceGeneration{};
        // 共有する展開済み音源
        std::shared_ptr<DirectX::SoundEffect> m_sound;
        // 所有する主再生ボイス
        std::unique_ptr<DirectX::SoundEffectInstance> m_instance;
        // 共有する逐次再生ストリーム
        std::shared_ptr<AudioStreamVoice> m_stream;
        // 所有する3D単発再生ボイス
        std::vector<
            std::unique_ptr<DirectX::SoundEffectInstance>>
            m_spatialOneShots;
        // 3D定位する音源の状態
        DirectX::AudioEmitter m_emitter;
        // 一定音量と線形減衰を結ぶ3点
        X3DAUDIO_DISTANCE_CURVE_POINT
            m_distanceCurvePoints[3]{};
        // 距離による音量減衰の曲線
        X3DAUDIO_DISTANCE_CURVE m_distanceCurve{};
        // 新しいフィールドは末尾へ追加し、クラスサイズ変更時はGameModuleApiVersionも更新する。
        // 指定した周回の開始フレーム
        std::uint64_t m_loopStartFrame{};
        // 指定した周回の終端フレーム
        std::uint64_t m_loopEndFrame{};
        // 周回境界の合成フレーム数
        std::uint64_t m_loopCrossfadeFrames{};
        // 周回区間が指定されているか
        bool m_hasLoopRegion{};

        // 指定した再生開始フレーム
        std::uint64_t m_startFrame{};
        // 帯域解析を有効にする指定
        bool m_levelMeterEnabled{};

        // 低域補正のゲインdB
        float m_bassBoostDb{};
        // 低域補正の境界Hz
        float m_bassCornerHz{ 110.0f };
    };
}
