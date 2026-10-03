#include "LamaPon/LamaPon.h"
#include "NetworkProfile.h"

namespace
{
    // Scriptの設定項目と既定値
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

// Data Asset設定でNetwork Sessionを管理するScriptです。
class NetworkSessionController final : public LamaPon::Script
{
public:
    // セッション設定を読み込み、イベント購読を開始します。
    void OnEnable() override
    {
        // このScriptが使用するSession
        auto* session = Network();
        // 重複所有や実行中セッションを防ぎます。
        if (!session || s_owner != nullptr
            || (session->State() != LamaPon::NetworkState::Stopped
                && session->State() != LamaPon::NetworkState::Error))
        {
            Warn("通信管理Scriptは停止中のセッションに1つだけ置いてください。");
            return;
        }
        // NetworkIdentityを持たない専用Objectを確認します。
        // object: 親階層の確認中Object。
        for (auto* object = &Owner(); object != nullptr; object = object->Parent())
        {
            // クライアント側の同期停止を避けます。
            if (object->GetComponent<LamaPon::NetworkIdentityComponent>() != nullptr)
            {
                Warn("通信管理ScriptはNetworkIdentityを持たない専用GameObjectへ置いてください。");
                return;
            }
        }
        // Asset設定を反映する接続構成
        LamaPon::NetworkConfiguration configuration;
        // Asset読込・検証の失敗理由
        std::string error;
        // Componentに設定されたProfile Asset
        const auto asset = LoadDataAsset(LamaPon::PathFromUtf8(m_profile));
        // Asset値を共通設定へ反映します。
        if (!LamaPonNetworkWorkflow::ReadProfile(*asset, session->Configuration(), configuration, error))
        {
            Warn("通信設定を読み込めません: " + error);
            return;
        }
        // 検証済み設定でSessionを構成します。
        if (!session->Configure(std::move(configuration)))
        {
            Warn(session->LastError());
            return;
        }
        s_owner = this;
        m_session = session;
        // Hostイベントでセッションを開始します。
        if (!m_hostEvent.empty()) m_subscriptions.push_back(On(m_hostEvent, [this]
        {
            // 所有権を維持し、Host開始に失敗した場合は警告します。
            if (OwnsSession() && !Network()->Host(m_playerName, m_listenAddress)) Warn(Network()->LastError());
        }));
        // Joinイベントの接続情報を処理します(args: 接続先を含むイベント引数)。
        if (!m_joinEvent.empty()) m_subscriptions.push_back(On(m_joinEvent, [this](const LamaPon::EventArgs& args)
        {
            // 所有中のみイベントの接続先へJoinします。
            if (OwnsSession() && !Network()->Join(args.text, m_playerName)) Warn(Network()->LastError());
        }));
        // Leaveイベントで自身のセッションを停止します。
        if (!m_leaveEvent.empty()) m_subscriptions.push_back(On(m_leaveEvent, [this]
        {
            // 所有中の接続だけを停止します。
            if (OwnsSession()) StopNetwork();
        }));
    }

    // 無効化時に購読とセッションを解放します。
    void OnDisable() override { Release(); }
    // 破棄時に購読とセッションを解放します。
    void OnDestroy() override { Release(); }

    // Script設定を読み込みます(text: JSON文字列)。
    void LoadProperties(const std::string_view text) override
    {
        // Script設定のJSONオブジェクト
        const auto properties = nlohmann::json::parse(text, nullptr, false);
        // オブジェクト以外は無視します。
        if (!properties.is_object()) return;
        // DataAsset形式に変換した値
        const auto values = LamaPon::DataAsset::FromJson(nlohmann::json{{"values", properties}}.dump());
        m_profile = values.GetText("profile", LamaPonNetworkWorkflow::DefaultProfilePath);
        m_playerName = values.GetText("playerName", "Player");
        m_listenAddress = values.GetText("listenAddress", "127.0.0.1");
        m_hostEvent = values.GetText("hostEvent", "Network.Host");
        m_joinEvent = values.GetText("joinEvent", "Network.Join");
        m_leaveEvent = values.GetText("leaveEvent", "Network.Leave");
    }

    // Script設定をJSON文字列へ保存します。
    [[nodiscard]] std::string SaveProperties() const override
    {
        return nlohmann::json{{"profile", m_profile}, {"playerName", m_playerName},
            {"listenAddress", m_listenAddress}, {"hostEvent", m_hostEvent},
            {"joinEvent", m_joinEvent}, {"leaveEvent", m_leaveEvent}}.dump();
    }

private:
    // 現在のSessionがこのScriptの所有物か判定します。
    [[nodiscard]] bool OwnsSession() const
    {
        // Application終了後の破棄済みポインター参照を避けます。
        return s_owner == this && Network() != nullptr && Network() == m_session;
    }
    // Session購読を解除し、所有中なら接続を停止します。
    void Release()
    {
        // 保持しているイベント購読
        // すべてのイベント購読を解除します(handle: 購読ID)。
        for (const auto handle : m_subscriptions) Off(handle);
        m_subscriptions.clear();
        // 自身が所有しているSessionだけ停止します。
        if (OwnsSession()) StopNetwork();
        // 所有権が自身の場合だけ解放します。
        if (s_owner == this) s_owner = nullptr;
        m_session = nullptr;
    }
    // Scriptの警告をログへ送ります(text: 警告文)。
    static void Warn(const std::string& text) { LamaPon::Logger::Instance().Warning(text); }

    // 現在の所有Script
    inline static NetworkSessionController* s_owner{};
    // Scriptが借用するNetwork Session
    LamaPon::NetworkSession* m_session{};
    // 解除対象のイベント購読ID
    std::vector<std::uint64_t> m_subscriptions;
    // 接続Profile Assetのパス
    std::string m_profile{ LamaPonNetworkWorkflow::DefaultProfilePath };
    // Host・Join時に使うプレイヤー名
    std::string m_playerName{ "Player" };
    // Hostの待受アドレス
    std::string m_listenAddress{ "127.0.0.1" };
    // セッション開始イベント名
    std::string m_hostEvent{ "Network.Host" };
    // セッション参加イベント名
    std::string m_joinEvent{ "Network.Join" };
    // セッション退出イベント名
    std::string m_leaveEvent{ "Network.Leave" };
};

LAMAPON_DATA_ASSET("Network.ConnectionProfile", "通信設定アセット", LamaPonNetworkWorkflow::ProfileSchema);
LAMAPON_SCRIPT_WITH_SCHEMA(NetworkSessionController, "Network.SessionController", "通信セッション管理", NetworkControllerSchema);
