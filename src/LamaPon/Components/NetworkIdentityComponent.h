#pragma once

#include "LamaPon/Online/NetworkSession.h"
#include "LamaPon/Scene/Component.h"

namespace LamaPon
{
    // 固定物体のSceneKeyは双方で同じ一意な値を指定します。
    // 動的生成用プリハブのSceneKeyは空にし、通信ID・所有者は保存しません。
    class NetworkIdentityComponent final : public Component
    {
    public:
        // 通信上の識別情報を作ります(sceneKey: 固定物体の共有識別鍵)。
        explicit NetworkIdentityComponent(std::string sceneKey = {});
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override { return "NetworkIdentity"; }
        // 固定物体を双方で照合する識別鍵を返します。
        [[nodiscard]] const std::string& SceneKey() const noexcept { return m_sceneKey; }
        // 識別鍵を設定し不正文字・長さで例外を送出します(key: 固定物体の共有識別鍵)。
        // 空または64文字以内のASCII英数字・._-を指定します。
        void SetSceneKey(std::string key);
        // 通信物体IDを返します。
        [[nodiscard]] NetworkObjectId NetworkId() const noexcept { return m_id; }
        // 所有者の通信相手IDを返します。
        [[nodiscard]] NetworkPeerId OwnerPeer() const noexcept { return m_ownerPeer; }
        // 有効な通信IDの所有者が稼働中セッションの自分か返します。
        [[nodiscard]] bool IsLocalOwner() const noexcept;
        // ホストだけでシミュレーションする設定を返します。
        [[nodiscard]] bool HostOnlySimulation() const noexcept { return m_hostOnly; }
        // 実行側を設定します(value: ホストだけで実行する指定)。
        void SetHostOnlySimulation(bool value) noexcept { m_hostOnly = value; }
        // 補間時間を秒で返します。
        [[nodiscard]] float InterpolationSeconds() const noexcept { return m_interpolation; }
        // 補間時間を設定し範囲外で例外を送出します(seconds: 有限の0〜1秒)。
        void SetInterpolationSeconds(float seconds);
        // 同期する追加データを返します。
        [[nodiscard]] const std::string& ReplicatedData() const noexcept { return m_data; }
        // 追加データを設定し容量超過で例外を送出します(data: 256バイト以内の同期データ)。
        void SetReplicatedData(std::string data);
        // 通信情報を直接設定します(id: 通信物体ID, owner: 所有者ID)。
        // Scene Bridge用で、スクリプトではNetworkSpawn・NetworkDespawnを使います。
        void BindNetworkId(NetworkObjectId id, NetworkPeerId owner) noexcept;

    private:
        // 固定物体の共有識別鍵
        std::string m_sceneKey;
        // 通信物体ID
        NetworkObjectId m_id{};
        // 所有者の通信相手ID
        NetworkPeerId m_ownerPeer{ 1 };
        // ホストだけで実行する指定
        bool m_hostOnly{ true };
        // 補間時間秒
        float m_interpolation{ 0.1f };
        // 同期する追加データ
        std::string m_data;
    };
}
