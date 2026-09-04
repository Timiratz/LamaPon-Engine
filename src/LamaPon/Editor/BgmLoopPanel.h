#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace LamaPon
{
    class AudioSystem;
    class AssetManager;
    class AudioStreamVoice;

    // BGMカタログ・波形・試聴の寿命を所有するパネルです。
    // EditorLayerは表示と保存後のコマンド実行だけを仲介します。
    // audio/assetsはこのパネルより長く生存する必要があります。
    class BgmLoopPanel final
    {
    public:
        using StatusSink = std::function<void(std::string, bool)>;
        BgmLoopPanel(AudioSystem& audio, AssetManager& assets,
            std::filesystem::path projectRoot,
            std::filesystem::path catalogPath, StatusSink status);
        ~BgmLoopPanel();
        BgmLoopPanel(const BgmLoopPanel&) = delete;
        BgmLoopPanel& operator=(const BgmLoopPanel&) = delete;

        void Draw(const std::string& title, bool& open,
            const std::function<void()>& onSaved);
        // 非表示・ゲーム再生への切り替えでも音声を残しません。
        void StopPreview() noexcept;
        [[nodiscard]] bool Matches(const std::filesystem::path& catalog) const
        {
            return m_catalogPath == catalog;
        }

    private:
        bool Load();
        bool Save();
        void BuildWaveform();
        // 試聴範囲か曲のループ範囲かを、編集中の値で切り替えます。
        void StartPreview(std::uint64_t fromFrame, bool usePreviewRange = false);
        void SetStatus(std::string message, bool error = false) const;

        struct State final
        {
            int selectedTrack{};
            bool loaded{};
            bool dirty{};
            std::unique_ptr<nlohmann::json> document;
            // 波形。曲を選び直したときだけ作り直します（全体を
            // デコードするので数百ミリ秒かかります）。
            int waveformTrack{ -1 };
            std::vector<float> waveformPeaks;
            std::uint64_t totalFrames{};
            int sampleRate{ 44100 };
            // 試聴。編集中のループ範囲をそのまま入れて鳴らします。
            std::shared_ptr<AudioStreamVoice> preview;
            int previewTrack{ -1 };
            float previewVolume{ 0.6f };
            // 波形を右クリックした位置。すぐ下のポップアップが読む。
            std::uint64_t contextFrame{};
            // 直前に鳴らし始めた位置と鳴らし方。停止したあと「▶ 再生」で
            // 同じところから鳴らし直すために覚えておく。
            std::uint64_t lastStartFrame{};
            bool lastUsedPreviewRange{ true };
            bool hasLastStart{};
        };

        AudioSystem& m_audio;
        AssetManager& m_assets;
        std::filesystem::path m_projectRoot;
        std::filesystem::path m_catalogPath;
        StatusSink m_status;
        State m_state;
    };
}
