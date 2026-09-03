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

    class AudioSourceComponent final : public Component
    {
    public:
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
        ~AudioSourceComponent() override;

        void SetAudioPath(std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path& AudioPath() const noexcept
        {
            return m_audioPath;
        }

        void SetVolume(float volume);
        [[nodiscard]] float Volume() const noexcept { return m_volume; }
        void SetPitch(float pitch);
        [[nodiscard]] float Pitch() const noexcept { return m_pitch; }
        void SetPan(float pan);
        [[nodiscard]] float Pan() const noexcept { return m_pan; }
        void SetLoop(bool loop);
        [[nodiscard]] bool Loop() const noexcept { return m_loop; }
        // streaming音源の任意区間ループ。単位は各チャンネル共通の
        // sample frameです。Play時は必ず0から再生し、最初のend到達後に
        // [start, end)を繰り返します。crossfadeFramesはend直前のtailと
        // start直後のheadを線形合成する長さです。不正な範囲は
        // 全体ループへ戻します。
        void SetLoopRegionFrames(
            std::uint64_t startFrame,
            std::uint64_t endFrame,
            std::uint64_t crossfadeFrames = 0) noexcept;
        void ClearLoopRegion() noexcept;
        [[nodiscard]] std::uint64_t CompletedLoopCount() const noexcept
        {
            return m_stream ? m_stream->CompletedLoopCount() : 0;
        }
        void SetPlayOnStart(bool playOnStart) noexcept
        {
            m_playOnStart = playOnStart;
        }
        [[nodiscard]] bool PlayOnStart() const noexcept
        {
            return m_playOnStart;
        }
        void SetSpatial(bool spatial);
        [[nodiscard]] bool IsSpatial() const noexcept
        {
            return m_spatial;
        }
        void SetMinimumDistance(float distance);
        [[nodiscard]] float MinimumDistance() const noexcept
        {
            return m_minimumDistance;
        }
        void SetMaximumDistance(float distance);
        [[nodiscard]] float MaximumDistance() const noexcept
        {
            return m_maximumDistance;
        }
        // ストリーミング再生（長尺BGM用）。圧縮データのみ保持し
        // 再生しながらデコードします。3D定位とPlayOneShotは
        // 非対応（Playとして扱われます）。
        void SetStreaming(bool streaming);
        [[nodiscard]] bool IsStreaming() const noexcept
        {
            return m_streaming;
        }
        void SetBus(AudioBus bus);
        [[nodiscard]] AudioBus Bus() const noexcept
        {
            return m_bus;
        }

        // ストリーミング音源の再生開始位置（各チャンネル共通の
        // sample frame）。Playはここから鳴り始めます。曲の途中だけを
        // 試聴させたいときに、音源を切り出さずに済ませるための入口
        // です。ループ範囲と併用すると「開始位置→loopEnd」を一度
        // 鳴らしてから[start, end)の反復へ入ります。
        // 音源の長さを超える値は先頭（0）として扱います。
        void SetStartFrame(std::uint64_t frame) noexcept;
        [[nodiscard]] std::uint64_t StartFrame() const noexcept
        {
            return m_startFrame;
        }

        // ---- 帯域レベルメーター（音楽ビジュアライザ用） ----
        // 既定は無効で、無効な間は解析も確保もしません。
        // 有効にすると、デバイスへ送るPCMを12帯域のbandpassで追い、
        // **いま鳴っている位置**のレベルを0..1で返します。先読み
        // されたPCMではなく再生位置に合わせて返すので、画面の棒と
        // 耳で聞こえる音がずれません。ストリーミング音源専用です。
        static constexpr int LevelBandCount =
            AudioStreamVoice::LevelBandCount;
        void SetLevelMeterEnabled(bool enabled);
        [[nodiscard]] bool IsLevelMeterEnabled() const noexcept
        {
            return m_levelMeterEnabled;
        }
        // 低域→高域の順に書き込み、書き込めた数を返します。
        std::size_t ReadLevelBands(
            float* destination,
            std::size_t capacity) const noexcept;

        // いま鳴っている位置（音源内のsample frame）。ループ区間の
        // 折り返しも追えます。ストリーミング音源専用で、それ以外は0。
        [[nodiscard]] std::uint64_t PlaybackFrame() const noexcept;
        // 音源の長さとサンプリング周波数（ストリーミング音源のみ）。
        // PlaybackFrameを秒や進捗率へ直すのに使います。
        [[nodiscard]] std::uint64_t TotalFrames() const noexcept
        {
            return m_stream ? m_stream->TotalFrames() : 0;
        }
        [[nodiscard]] int SampleRate() const noexcept
        {
            return m_stream ? m_stream->SampleRate() : 0;
        }

        // 低音の持ち上げ（low shelf）。cornerHzより下をgainDbだけ
        // 持ち上げます。0dB（既定）なら一切処理しません。
        // クリップ防止に内部でPCMを下げますが、**下げたぶんは再生
        // 音量側で自動的に戻る**ので、呼ぶ側はSetVolumeをそのままに
        // しておけます。ストリーミング音源専用です。
        void SetBassBoost(float gainDb, float cornerHz = 110.0f);
        [[nodiscard]] float BassBoostDb() const noexcept
        {
            return m_bassBoostDb;
        }
        [[nodiscard]] float BassBoostMakeup() const noexcept
        {
            return m_bassBoostDb > 0.0f
                ? std::pow(10.0f, m_bassBoostDb / 20.0f)
                : 1.0f;
        }

        // 表示用のピーク包絡（0..1）。波形を描くための値です。
        // **重い呼び出しで、鳴らしている最中に呼ぶと音が途切れます。**
        // 詳細はAudioStreamVoice::ReadPeakEnvelopeの説明を見てください。
        std::size_t ReadPeakEnvelope(
            float* destination,
            std::size_t bucketCount);

        void Play();
        void PlayOneShot();
        void Pause() noexcept;
        void Resume();
        void Stop() noexcept;
        [[nodiscard]] DirectX::SoundState State() const noexcept;

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "AudioSource";
        }

    protected:
        void OnInitialize(GraphicsDevice& graphics) override;
        void OnUpdate(float deltaTime) override;

    private:
        void ReloadSound();
        void ApplyProperties();
        void ApplySpatial(
            DirectX::SoundEffectInstance& instance);
        void UpdateDistanceCurve() noexcept;

        [[nodiscard]] float
            EffectiveVolume() const noexcept;

        std::filesystem::path m_audioPath;
        float m_volume{ 1.0f };
        float m_pitch{};
        float m_pan{};
        bool m_loop{};
        bool m_playOnStart{};
        bool m_spatial{};
        bool m_streaming{};
        AudioBus m_bus{ AudioBus::Effects };
        float m_lastBusVolume{ 1.0f };
        float m_minimumDistance{ 1.0f };
        float m_maximumDistance{ 20.0f };
        bool m_started{};
        bool m_playRequested{};
        bool m_oneShotRequested{};
        AssetManager* m_assets{};
        AudioSystem* m_audio{};
        std::uint64_t m_deviceGeneration{};
        std::shared_ptr<DirectX::SoundEffect> m_sound;
        std::unique_ptr<DirectX::SoundEffectInstance> m_instance;
        std::shared_ptr<AudioStreamVoice> m_stream;
        std::vector<
            std::unique_ptr<DirectX::SoundEffectInstance>>
            m_spatialOneShots;
        DirectX::AudioEmitter m_emitter;
        X3DAUDIO_DISTANCE_CURVE_POINT
            m_distanceCurvePoints[3]{};
        X3DAUDIO_DISTANCE_CURVE m_distanceCurve{};
        // ---- ここから下は後から足した項目です。**必ず末尾へ足す**
        // こと（途中へ入れるとこのクラスのレイアウトがずれ、
        // 作り直していないGame Module DLLが壊れます）。
        // sizeofが変わるので GameModuleApiVersion も上げること。----
        std::uint64_t m_loopStartFrame{};
        std::uint64_t m_loopEndFrame{};
        std::uint64_t m_loopCrossfadeFrames{};
        bool m_hasLoopRegion{};
        // 2026-08-29追加。再生開始位置と帯域レベルメーターの有無。
        std::uint64_t m_startFrame{};
        bool m_levelMeterEnabled{};
        // 2026-08-30追加。低音の持ち上げ。
        float m_bassBoostDb{};
        float m_bassCornerHz{ 110.0f };
    };
}
