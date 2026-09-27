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
        void Draw(OnlineServices& services, const OnlineProjectSettings& settings);
    private:
        std::array<char, 129> m_details{ "エディターで動作確認" };
        std::array<char, 129> m_state{ "開発中" };
        std::string m_message;
    };
}
