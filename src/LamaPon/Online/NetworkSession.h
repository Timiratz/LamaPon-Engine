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
        std::string key;
        std::string assetPath;
    };

    struct NetworkConfiguration final
    {
        // 同じID・バージョン・シーンのゲーム同士だけを接続します。
        std::string gameId{ "lamapon.game" };
        std::string gameVersion{ "1" };
        std::string sceneId{ "main" };
        std::uint32_t maxPlayers{ 4 };
        std::uint32_t tickRate{ 20 };
        float timeoutSeconds{ 15.0f };
        std::uint16_t port{ 27840 };
        NetworkBackend backend{ NetworkBackend::Lan };
        NetworkSyncMode syncMode{ NetworkSyncMode::Continuous };
        // 対応するIPv4ルーターで短期のポート転送を要求します。既定では変更しません。
        bool automaticPortMapping{};
        // LAN検索への公開はゲーム側で選びます。公開した部屋はLAN参加者が接続できます。
        bool advertiseLan{};
        std::string roomName{ "Room" };
        std::uint16_t discoveryPort{ 27841 };
        std::string eosProductId;
        std::string eosSandboxId;
        std::string eosDeploymentId;
        std::string eosClientId;
        // クライアントポリシーを限定したEOSゲームクライアント用資格情報。
        // プロジェクトには環境変数の名前だけを保存します。
        std::string eosClientSecretEnvironment{ "LAMAPON_EOS_CLIENT_SECRET" };
        std::vector<NetworkPrefabRegistration> prefabs;
    };

    struct NetworkRoom;

    struct NetworkTransform final
    {
        std::array<float, 3> position{ 0, 0, 0 };
        std::array<float, 4> rotation{ 0, 0, 0, 1 };
        std::array<float, 3> scale{ 1, 1, 1 };
        bool operator==(const NetworkTransform&) const = default;
    };

    struct NetworkObjectState final
    {
        NetworkObjectId id{};
        NetworkPeerId owner{ 1 };
        // sceneKeyが非空なら既存シーンのオブジェクトです。
        // 空なら双方で登録したprefabKeyから生成します。パスは送信しません。
        std::string sceneKey;
        std::string prefabKey;
        NetworkTransform transform;
        bool enabled{ true };
        std::string data;
        bool operator==(const NetworkObjectState&) const = default;
    };

    struct NetworkMember final
    {
        NetworkPeerId id{};
        std::string name;
    };

    enum class NetworkEventKind : std::uint8_t
    {
        Started, Joined, Left, Input, GameEvent, Stopped, Error, Command, SessionState
    };

    struct NetworkEvent final
    {
        NetworkEventKind kind{ NetworkEventKind::Started };
        NetworkPeerId peer{};
        NetworkObjectId object{};
        std::string name;
        std::string data;
    };

    struct NetworkStatistics final
    {
        std::uint64_t sentBytes{};
        std::uint64_t receivedBytes{};
        std::uint64_t rejectedMessages{};
        float roundTripMilliseconds{};
    };

    namespace Detail { class NetworkSessionTestAccess; }

    // Applicationと同じスレッドから使います。DLLの関数ポインターを
    // 保存せず、イベントを値として取り出すためHot Reloadで無効化されません。
    class NetworkSession final
    {
    public:
        LAMAPON_API NetworkSession();
        LAMAPON_API ~NetworkSession();
        NetworkSession(const NetworkSession&) = delete;
        NetworkSession& operator=(const NetworkSession&) = delete;

        LAMAPON_API bool Configure(NetworkConfiguration configuration);
        // TCPは数値IPv4またはIPv6。ホストの既定は同じPCからの接続だけです。
        LAMAPON_API bool Host(std::string name = "Host",
            std::string address = "127.0.0.1");
        // LAN: 接続先、Direct: 接続情報、EOS: ホストが表示した部屋ID。
        LAMAPON_API bool Join(std::string address, std::string name = "Player");
        LAMAPON_API void Stop();
        // Scene同期など継続できないエラーを接続状態とイベントに残します。
        LAMAPON_API void Abort(std::string reason);
        LAMAPON_API void Update(float elapsedSeconds);

        [[nodiscard]] LAMAPON_API NetworkState State() const noexcept;
        [[nodiscard]] LAMAPON_API std::uint64_t Generation() const noexcept;
        [[nodiscard]] LAMAPON_API bool IsHost() const noexcept;
        [[nodiscard]] LAMAPON_API NetworkPeerId LocalPeer() const noexcept;
        [[nodiscard]] LAMAPON_API std::string RoomAddress() const;
        [[nodiscard]] LAMAPON_API const std::string& LastError() const noexcept;
        [[nodiscard]] LAMAPON_API const NetworkConfiguration& Configuration() const noexcept;
        [[nodiscard]] LAMAPON_API const std::vector<NetworkMember>& Members() const noexcept;
        [[nodiscard]] LAMAPON_API const std::vector<NetworkObjectState>& Objects() const noexcept;
        [[nodiscard]] LAMAPON_API const NetworkObjectState* FindObject(NetworkObjectId id) const noexcept;
        [[nodiscard]] LAMAPON_API NetworkStatistics Statistics() const noexcept;
        LAMAPON_API bool PollEvent(NetworkEvent& event);

        // 生成・変更・削除とゲームイベントの配信はホストだけが行えます。
        [[nodiscard]] LAMAPON_API NetworkObjectId Spawn(NetworkObjectState object);
        LAMAPON_API bool SetObject(NetworkObjectState object);
        LAMAPON_API bool Despawn(NetworkObjectId id);
        // 所有者は操作だけを送ります。ゲームのScriptが内容を検証して
        // ホスト上で処理します。受信したTransformをそのまま信用しません。
        LAMAPON_API bool SendInput(NetworkObjectId object,
            std::string name, std::string data);
        LAMAPON_API bool BroadcastEvent(std::string name, std::string data);
        // オブジェクトを持たないターン操作など。ホストが送信者とゲームのルールを検証します。
        LAMAPON_API bool SendCommand(std::string name, std::string data);
        // ターン・盤面など部屋全体の256バイト以下の状態。途中参加にも送ります。
        LAMAPON_API bool SetSessionState(std::string data);
        [[nodiscard]] LAMAPON_API const std::string& SessionState() const noexcept;
        // 接続先と秘密部分を別々に受け取れるため、招待UIに限定されません。
        LAMAPON_API bool JoinDirect(std::string endpoint, std::string accessKey, std::string name = "Player");
        [[nodiscard]] LAMAPON_API std::string AccessKey() const;
        [[nodiscard]] LAMAPON_API std::string ConnectionCode(std::string endpoint = {}) const;
        [[nodiscard]] LAMAPON_API std::string ConnectionStatus() const;
        [[nodiscard]] LAMAPON_API std::string LocalAddress() const;
        LAMAPON_API bool JoinRoom(const NetworkRoom& room, std::string name = "Player");

    private:
        struct Implementation;
        std::unique_ptr<Implementation> m_impl;
        friend class Detail::NetworkSessionTestAccess;
    };

    LAMAPON_API void ValidateNetworkConfiguration(const NetworkConfiguration& configuration);
    [[nodiscard]] LAMAPON_API std::string_view NetworkStateName(NetworkState state) noexcept;
    [[nodiscard]] LAMAPON_API bool HasEpicNetworkBackend() noexcept;
    [[nodiscard]] LAMAPON_API NetworkSession* ActiveNetworkSession() noexcept;
    LAMAPON_API void SetActiveNetworkSession(NetworkSession* session) noexcept;
}
