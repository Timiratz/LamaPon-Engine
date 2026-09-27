#include "LamaPon/LamaPon.h"
#include "NetworkProfile.h"

namespace
{
    constexpr char NetworkControllerSchema[] = R"schema({"fields":[
        {"name":"profile","displayName":"通信設定アセット","type":"asset","assetType":"data",
         "dataType":"Network.ConnectionProfile","default":"packages/network-session-workflow/profiles/DirectLocal.asset.json"},
        {"name":"playerName","displayName":"プレイヤー名","type":"string","default":"Player"},
        {"name":"listenAddress","displayName":"ホストの待受先","type":"string","default":"127.0.0.1",
         "tooltip":"同じPCは127.0.0.1、外部のIPv4接続は0.0.0.0、IPv6は::です。"},
        {"name":"hostEvent","displayName":"ホスト開始イベント","type":"string","default":"Network.Host"},
        {"name":"joinEvent","displayName":"参加イベント","type":"string","default":"Network.Join",
         "tooltip":"EventArgs.textへ実行時の接続情報を渡します。アクセスキーをSceneへ保存しないでください。"},
        {"name":"leaveEvent","displayName":"退出イベント","type":"string","default":"Network.Leave"}
    ]})schema";
}

class NetworkSessionController final : public LamaPon::Script
{
public:
    void OnEnable() override
    {
        auto* session = Network();
        if (!session || s_owner != nullptr
            || (session->State() != LamaPon::NetworkState::Stopped
                && session->State() != LamaPon::NetworkState::Error))
        {
            Warn("通信管理Scriptは停止中のセッションに1つだけ置いてください。");
            return;
        }
        // クライアントでシミュレーションを止める同期オブジェクトの下に、
        // 接続そのものを管理するScriptを置くと参加直後に無効化されます。
        for (auto* object = &Owner(); object != nullptr; object = object->Parent())
        {
            if (object->GetComponent<LamaPon::NetworkIdentityComponent>() != nullptr)
            {
                Warn("通信管理ScriptはNetworkIdentityを持たない専用GameObjectへ置いてください。");
                return;
            }
        }
        LamaPon::NetworkConfiguration configuration;
        std::string error;
        const auto asset = LoadDataAsset(LamaPon::PathFromUtf8(m_profile));
        if (!LamaPonNetworkWorkflow::ReadProfile(*asset, session->Configuration(), configuration, error))
        {
            Warn("通信設定を読み込めません: " + error);
            return;
        }
        if (!session->Configure(std::move(configuration)))
        {
            Warn(session->LastError());
            return;
        }
        s_owner = this;
        m_session = session;
        if (!m_hostEvent.empty()) m_subscriptions.push_back(On(m_hostEvent, [this]
        {
            if (OwnsSession() && !Network()->Host(m_playerName, m_listenAddress)) Warn(Network()->LastError());
        }));
        if (!m_joinEvent.empty()) m_subscriptions.push_back(On(m_joinEvent, [this](const LamaPon::EventArgs& args)
        {
            if (OwnsSession() && !Network()->Join(args.text, m_playerName)) Warn(Network()->LastError());
        }));
        if (!m_leaveEvent.empty()) m_subscriptions.push_back(On(m_leaveEvent, [this]
        {
            if (OwnsSession()) StopNetwork();
        }));
    }

    void OnDisable() override { Release(); }
    void OnDestroy() override { Release(); }

    void LoadProperties(const std::string_view text) override
    {
        const auto properties = nlohmann::json::parse(text, nullptr, false);
        if (!properties.is_object()) return;
        const auto values = LamaPon::DataAsset::FromJson(nlohmann::json{{"values", properties}}.dump());
        m_profile = values.GetText("profile", LamaPonNetworkWorkflow::DefaultProfilePath);
        m_playerName = values.GetText("playerName", "Player");
        m_listenAddress = values.GetText("listenAddress", "127.0.0.1");
        m_hostEvent = values.GetText("hostEvent", "Network.Host");
        m_joinEvent = values.GetText("joinEvent", "Network.Join");
        m_leaveEvent = values.GetText("leaveEvent", "Network.Leave");
    }

    [[nodiscard]] std::string SaveProperties() const override
    {
        return nlohmann::json{{"profile", m_profile}, {"playerName", m_playerName},
            {"listenAddress", m_listenAddress}, {"hostEvent", m_hostEvent},
            {"joinEvent", m_joinEvent}, {"leaveEvent", m_leaveEvent}}.dump();
    }

private:
    [[nodiscard]] bool OwnsSession() const
    {
        // Application終了時にはセッションが先に破棄される場合があります。
        // グローバルの生存確認をしてから使い、保存したポインターを直接参照しません。
        return s_owner == this && Network() != nullptr && Network() == m_session;
    }
    void Release()
    {
        for (const auto handle : m_subscriptions) Off(handle);
        m_subscriptions.clear();
        if (OwnsSession()) StopNetwork();
        if (s_owner == this) s_owner = nullptr;
        m_session = nullptr;
    }
    static void Warn(const std::string& text) { LamaPon::Logger::Instance().Warning(text); }

    inline static NetworkSessionController* s_owner{};
    LamaPon::NetworkSession* m_session{};
    std::vector<std::uint64_t> m_subscriptions;
    std::string m_profile{ LamaPonNetworkWorkflow::DefaultProfilePath };
    std::string m_playerName{ "Player" };
    std::string m_listenAddress{ "127.0.0.1" };
    std::string m_hostEvent{ "Network.Host" };
    std::string m_joinEvent{ "Network.Join" };
    std::string m_leaveEvent{ "Network.Leave" };
};

LAMAPON_DATA_ASSET("Network.ConnectionProfile", "通信設定アセット", LamaPonNetworkWorkflow::ProfileSchema);
LAMAPON_SCRIPT_WITH_SCHEMA(NetworkSessionController, "Network.SessionController", "通信セッション管理", NetworkControllerSchema);
