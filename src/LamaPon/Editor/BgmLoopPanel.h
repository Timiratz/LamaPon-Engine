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

    // BGMのループ範囲を編集し、保存前の設定で試聴します。
    class BgmLoopPanel final
    {
    public:
        // 状態を通知します（文字列: 通知内容、bool: エラーか）。
        using StatusSink = std::function<void(std::string, bool)>;
        // 音声系と資産管理を借用してカタログを読み込みます(audio: 借用する音声系, assets: 借用する資産管理, projectRoot: 資産パスの基準, catalogPath: 編集対象のパス, status: 状態通知・空も可)。
        BgmLoopPanel(AudioSystem& audio, AssetManager& assets,
            std::filesystem::path projectRoot,
            std::filesystem::path catalogPath, StatusSink status);
        // 試聴音声を止めて解放します。
        ~BgmLoopPanel();
        // 試聴音声の所有権を共有しないためコピーを禁止します。
        BgmLoopPanel(const BgmLoopPanel&) = delete;
        // 試聴音声の所有権を共有しないためコピー代入を禁止します。
        BgmLoopPanel& operator=(const BgmLoopPanel&) = delete;

        // ループ範囲を編集して試聴します(title: ウィンドウ名, open: 表示状態, onSaved: 保存成功時の通知・空も可)。
        void Draw(const std::string& title, bool& open,
            const std::function<void()>& onSaved);
        // 試聴音声を停止して解放し、非表示時にも音声を残さないようにします。
        void StopPreview() noexcept;
        // 編集対象のカタログパスと一致するか返します(catalog: 照合するパス)。
        [[nodiscard]] bool Matches(const std::filesystem::path& catalog) const
        {
            return m_catalogPath == catalog;
        }

    private:
        // 曲名と資産パスを検証して読み込み、失敗時は編集を無効にします。
        bool Load();
        // 置換用ファイルを書き終えてからカタログを置換し、成功時だけ未保存状態を解除します。
        bool Save();
        // 選択曲全体を専用ストリームで読み、選曲時の波形を作成します。
        void BuildWaveform();
        // 編集値のループ範囲で試聴を始めます(fromFrame: 再生開始フレーム, usePreviewRange: 試聴範囲をループするか)。
        void StartPreview(std::uint64_t fromFrame, bool usePreviewRange = false);
        // 状態通知があれば呼び出します(message: 通知内容, error: エラー通知か)。
        void SetStatus(std::string message, bool error = false) const;

        struct State final
        {
            // 選択曲の配列添字
            int selectedTrack{};
            // カタログ読込済みか
            bool loaded{};
            // 未保存の編集があるか
            bool dirty{};
            // 編集中のカタログJSON
            std::unique_ptr<nlohmann::json> document;

            // 波形を作成済みの曲添字
            int waveformTrack{ -1 };
            // 曲全体の区間別最大振幅
            std::vector<float> waveformPeaks;
            // 選択曲の総サンプルフレーム数
            std::uint64_t totalFrames{};
            // 選択曲のサンプル周波数・Hz
            int sampleRate{ 44100 };

            // 試聴用ストリームの所有先
            std::shared_ptr<AudioStreamVoice> preview;
            // 試聴音声の曲添字
            int previewTrack{ -1 };
            // 試聴音量・0から1
            float previewVolume{ 0.6f };

            // 右クリック位置のフレーム
            std::uint64_t contextFrame{};

            // 直前の試聴開始フレーム
            std::uint64_t lastStartFrame{};
            // 直前は試聴範囲のループか
            bool lastUsedPreviewRange{ true };
            // 直前の試聴位置があるか
            bool hasLastStart{};
        };

        // パネルより長く生存する音声系
        AudioSystem& m_audio;
        // パネルより長く生存する資産管理
        AssetManager& m_assets;
        // 曲の資産パスの基準ディレクトリ
        std::filesystem::path m_projectRoot;
        // 編集対象カタログのパス
        std::filesystem::path m_catalogPath;
        // 状態通知の所有先
        StatusSink m_status;
        // カタログと試聴状態
        State m_state;
    };
}
