#pragma once

#include <array>
#include <string>

namespace LamaPon
{
    class OnlineServices;
    struct OnlineProjectSettings;

    // 外部サービスの診断入力を所有し、ゲーム通信や設定草稿には依存しません。
    class ServiceDiagnosticsPanel final
    {
    public:
        // 保存済み設定でPresenceのテスト表示・消去を操作する(services: 操作する外部サービスの借用, settings: 保存済みプロジェクト設定)。
        void Draw(OnlineServices& services, const OnlineProjectSettings& settings);
    private:
        // テスト表示の説明文の入力
        std::array<char, 129> m_details{ "エディターで動作確認" };
        // テスト表示の状態文の入力
        std::array<char, 129> m_state{ "開発中" };
        // 送信操作の結果または失敗理由
        std::string m_message;
    };
}
