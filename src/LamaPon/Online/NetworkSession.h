#pragma once

#include "LamaPon/Core/Api.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    using NetworkPeerId = std::uint32_t;
    using NetworkObjectId = std::uint32_t;

    enum class NetworkBackend : std::uint8_t { Lan, EpicOnlineServices, Direct };
    enum class NetworkSyncMode : std::uint8_t { Continuous, OnChange };
    enum class NetworkState : std::uint8_t
    {
        Stopped, Starting, Hosting, Connecting, Connected, Error
    };

    struct NetworkPrefabRegistration final
    {
        // 同期Prefabの一意な識別子
        std::string key;
        // assetsからの相対パス
        std::string assetPath;
    };

    // ゲームID・バージョン・シーンのすべてが一致する相手だけを接続します。
    struct NetworkConfiguration final
    {
        // ゲームの互換識別子
        std::string gameId{ "lamapon.game" };
        // ゲームの互換バージョン
        std::string gameVersion{ "1" };
        // 同期するシーンの識別子
        std::string sceneId{ "main" };
        // ホストを含む定員・2〜4人
        std::uint32_t maxPlayers{ 4 };
        // 同期の送信頻度・1〜60Hz
        std::uint32_t tickRate{ 20 };
        // 無通信の許容秒数・5〜120
        float timeoutSeconds{ 15.0f };
        // TCP待受の既定ポート
        std::uint16_t port{ 27840 };
        // 通信バックエンド
        NetworkBackend backend{ NetworkBackend::Lan };
        // 常時送信または変更時送信
        NetworkSyncMode syncMode{ NetworkSyncMode::Continuous };
        // 短期ポート転送の要求有無
        bool automaticPortMapping{};
        // LANへ公開すると、DirectのアクセスキーもLAN参加者へ配信されます。
        // LAN部屋検索への公開有無
        bool advertiseLan{};
        // LAN検索に表示する部屋名
        std::string roomName{ "Room" };
        // LAN検索のUDPポート
        std::uint16_t discoveryPort{ 27841 };
        // EOS製品の識別子
        std::string eosProductId;
        // EOS Sandbox識別子
        std::string eosSandboxId;
        // EOS Deployment識別子
        std::string eosDeploymentId;
        // EOSゲームClient識別子
        std::string eosClientId;
        // ゲームクライアントに限定したEOS資格情報は環境変数に置き、設定には変数名だけを保存します。
        // EOS資格情報の環境変数名
        std::string eosClientSecretEnvironment{ "LAMAPON_EOS_CLIENT_SECRET" };
        // 同期Prefabの登録一覧
        std::vector<NetworkPrefabRegistration> prefabs;
    };

    struct NetworkRoom;

    struct NetworkTransform final
    {
        // 位置のXYZ
        std::array<float, 3> position{ 0, 0, 0 };
        // 回転クォータニオンのXYZW
        std::array<float, 4> rotation{ 0, 0, 0, 1 };
        // 倍率のXYZ
        std::array<float, 3> scale{ 1, 1, 1 };
        // 変換の全要素を比較する。
        bool operator==(const NetworkTransform&) const = default;
    };

    // sceneKeyとprefabKeyは片方だけを指定し、assetのパスを通信へ含めません。
    struct NetworkObjectState final
    {
        // 通信オブジェクトID
        NetworkObjectId id{};
        // 入力を送る所有者の参加者ID
        NetworkPeerId owner{ 1 };
        // 固定シーン対象の識別子
        std::string sceneKey;
        // 動的生成するPrefabの識別子
        std::string prefabKey;
        // ホストが決定した変換
        NetworkTransform transform;
        // オブジェクトの有効状態
        bool enabled{ true };
        // 最大256バイトの同期データ
        std::string data;
        // 同期状態の全要素を比較する。
        bool operator==(const NetworkObjectState&) const = default;
    };

    struct NetworkMember final
    {
        // 参加者ID・ホストは1
        NetworkPeerId id{};
        // 参加者の表示名
        std::string name;
    };

    enum class NetworkEventKind : std::uint8_t
    {
        Started, Joined, Left, Input, GameEvent, Stopped, Error, Command, SessionState
    };

    struct NetworkEvent final
    {
        // 通信イベントの種別
        NetworkEventKind kind{ NetworkEventKind::Started };
        // イベントの送信者ID
        NetworkPeerId peer{};
        // 対象の通信オブジェクトID
        NetworkObjectId object{};
        // 入力・イベント・コマンド名
        std::string name;
        // 内容またはエラー理由
        std::string data;
    };

    struct NetworkStatistics final
    {
        // キューへ登録した送信バイト数
        std::uint64_t sentBytes{};
        // 受け取った通信内容のバイト数
        std::uint64_t receivedBytes{};
        // 拒否したメッセージの件数
        std::uint64_t rejectedMessages{};
        // 直近の往復遅延・ミリ秒
        float roundTripMilliseconds{};
    };

    namespace Detail { class NetworkSessionTestAccess; }

    // Applicationと同じスレッドから使い、DLL関数を保持せずイベントを値で受け取ります。
    // 各送信APIの戻り値で、相手の受信完了は確認できません。
    class NetworkSession final
    {
    public:
        // 停止状態の通信セッションを作る。
        LAMAPON_API NetworkSession();
        // 通信を停止し、アクティブ登録を解除する。
        LAMAPON_API ~NetworkSession();
        // 通信状態の複製を禁止する。
        NetworkSession(const NetworkSession&) = delete;
        // 通信状態のコピー代入を禁止する。
        NetworkSession& operator=(const NetworkSession&) = delete;

        // 停止中またはエラー時に設定を検証して保存する(configuration: 通信設定)。
        LAMAPON_API bool Configure(NetworkConfiguration configuration);
        // 部屋の作成を開始する(name: ホストの表示名, address: 待受アドレス)。
        // TCPは数値IPv4・IPv6で待ち、既定は同一PCからのみ接続できる127.0.0.1です。
        LAMAPON_API bool Host(std::string name = "Host",
            std::string address = "127.0.0.1");
        // 部屋への接続を開始する(address: バックエンド別の接続情報, name: 参加者の表示名)。
        // LANは数値IP、Directは接続コード、EOSはホストが表示した部屋IDを指定します。
        LAMAPON_API bool Join(std::string address, std::string name = "Player");
        // 通信と同期状態を消し、使用中なら停止イベントを残す。
        LAMAPON_API void Stop();
        // 通信を終了し、エラー状態とイベントを残す(reason: 継続できない失敗理由)。
        LAMAPON_API void Abort(std::string reason);
        // 通信・タイムアウト・同期送信を進める(elapsedSeconds: 経過秒数)。
        LAMAPON_API void Update(float elapsedSeconds);

        // 現在の接続状態を返す。
        [[nodiscard]] LAMAPON_API NetworkState State() const noexcept;
        // 停止や失敗で変わる接続世代を返す。
        [[nodiscard]] LAMAPON_API std::uint64_t Generation() const noexcept;
        // 部屋の作成が完了したホストかを返す。
        [[nodiscard]] LAMAPON_API bool IsHost() const noexcept;
        // 自身の参加者IDを返し、未確定なら0を返す。
        [[nodiscard]] LAMAPON_API NetworkPeerId LocalPeer() const noexcept;
        // バックエンドの公開接続情報を返す。
        [[nodiscard]] LAMAPON_API std::string RoomAddress() const;
        // 直近のエラーを借用する。
        [[nodiscard]] LAMAPON_API const std::string& LastError() const noexcept;
        // 現在の通信設定を借用する。
        [[nodiscard]] LAMAPON_API const NetworkConfiguration& Configuration() const noexcept;
        // 参加者一覧を借用し、要素の参照は通信更新で無効になり得る。
        [[nodiscard]] LAMAPON_API const std::vector<NetworkMember>& Members() const noexcept;
        // 同期状態一覧を借用し、要素の参照は更新・生成・削除で無効になり得る。
        [[nodiscard]] LAMAPON_API const std::vector<NetworkObjectState>& Objects() const noexcept;
        // 同期状態を借用し、更新・生成・削除後は取得し直す(id: 通信オブジェクトID)。
        [[nodiscard]] LAMAPON_API const NetworkObjectState* FindObject(NetworkObjectId id) const noexcept;
        // 通信内容の集計と往復遅延を返す。
        [[nodiscard]] LAMAPON_API NetworkStatistics Statistics() const noexcept;
        // キュー先頭のイベントを消費し、空ならfalseを返す(event: イベントの出力先)。
        // 全種類を単一キューから消費するため、複数Scriptへは集約して振り分けます。
        LAMAPON_API bool PollEvent(NetworkEvent& event);

        // ホストで同期対象を登録して配信し、失敗時は0を返す(object: 初期同期状態)。
        [[nodiscard]] LAMAPON_API NetworkObjectId Spawn(NetworkObjectState object);
        // ホストで識別情報を保った状態変更を次の通信tickへ登録する(object: 更新する同期状態)。
        LAMAPON_API bool SetObject(NetworkObjectState object);
        // ホストで同期対象を削除して配信する(id: 通信オブジェクトID)。
        LAMAPON_API bool Despawn(NetworkObjectId id);
        // 所有者の入力をホストへ送る(object: 通信オブジェクトID, name: 入力名, data: 最大256バイトの内容)。
        // ホストのScriptが内容とゲームのルールを検証し、参加者のTransformをそのまま信用しません。
        LAMAPON_API bool SendInput(NetworkObjectId object,
            std::string name, std::string data);
        // ホストのゲームイベントを自身と参加者へ配信する(name: イベント名, data: 最大256バイトの内容)。
        LAMAPON_API bool BroadcastEvent(std::string name, std::string data);
        // オブジェクトなしの操作をホストへ送る(name: コマンド名, data: 最大256バイトの内容)。
        // ホストのScriptが送信者とゲームのルールを検証します。
        LAMAPON_API bool SendCommand(std::string name, std::string data);
        // ホストで部屋の状態を更新し途中参加にも配信する(data: 最大256バイトの状態)。
        LAMAPON_API bool SetSessionState(std::string data);
        // 部屋全体の最新状態を借用する。
        [[nodiscard]] LAMAPON_API const std::string& SessionState() const noexcept;
        // Direct部屋への接続を始める(endpoint: TCP接続先, accessKey: 共有アクセスキー, name: 参加者の表示名)。
        LAMAPON_API bool JoinDirect(std::string endpoint, std::string accessKey, std::string name = "Player");
        // 部屋の共有アクセスキーを返す。
        [[nodiscard]] LAMAPON_API std::string AccessKey() const;
        // 招待用の接続コードを返す(endpoint: 指定接続先・空なら自動選択)。
        [[nodiscard]] LAMAPON_API std::string ConnectionCode(std::string endpoint = {}) const;
        // バックエンドの接続状態を返す。
        [[nodiscard]] LAMAPON_API std::string ConnectionStatus() const;
        // ローカルの待受アドレスを返す。
        [[nodiscard]] LAMAPON_API std::string LocalAddress() const;
        // 互換性と空きを確かめて部屋への接続を始める(room: 検索した部屋, name: 参加者の表示名)。
        LAMAPON_API bool JoinRoom(const NetworkRoom& room, std::string name = "Player");

    private:
        struct Implementation;
        // 通信セッション状態の所有先
        std::unique_ptr<Implementation> m_impl;
        friend class Detail::NetworkSessionTestAccess;
    };

    // 範囲・識別子・Prefab相対パスを検証し、不正なら例外にする(configuration: 通信設定)。
    LAMAPON_API void ValidateNetworkConfiguration(const NetworkConfiguration& configuration);
    // 接続状態の表示名を返す(state: 接続状態)。
    [[nodiscard]] LAMAPON_API std::string_view NetworkStateName(NetworkState state) noexcept;
    // EOSバックエンドを含むビルドかを返す。
    [[nodiscard]] LAMAPON_API bool HasEpicNetworkBackend() noexcept;
    // 現在の通信セッションを借用する。
    [[nodiscard]] LAMAPON_API NetworkSession* ActiveNetworkSession() noexcept;
    // 所有権を持たずアクティブ通信を登録する(session: 登録する通信・空で解除)。
    LAMAPON_API void SetActiveNetworkSession(NetworkSession* session) noexcept;
}
