#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstddef>
#include <span>
#include <vector>

namespace LamaPon
{
    class NavMeshComponent;

    // 数値と経路座標は有限値を前提とし、移動は衝突判定を行わず所有者の高さを維持する。
    class NavMeshAgentComponent final : public Component
    {
    public:
        // XZ平面の経路を移動するエージェントを作る(speed: 毎秒の移動距離, stoppingDistance: 最終点の到達許容距離, rotateToPath: 進行方向で回転する指定)。
        explicit NavMeshAgentComponent(
            float speed = 3.0f,
            float stoppingDistance = 0.1f,
            bool rotateToPath = true) noexcept;

        // 速度の絶対値を0〜1000に収める(speed: 毎秒の移動距離)。
        void SetSpeed(float speed) noexcept;
        // 到達許容距離の絶対値を0〜100に収める(distance: 最終点の許容距離)。
        void SetStoppingDistance(
            float distance) noexcept;
        // 進行方向からローカル回転のヨーを設定するか切り替える(enabled: 回転を更新する指定)。
        void SetRotateToPath(
            bool enabled) noexcept
        {
            m_rotateToPath = enabled;
        }

        // 毎秒の移動距離を取得する。
        [[nodiscard]] float Speed() const noexcept
        {
            return m_speed;
        }
        // 最終点のXZ平面での到達許容距離を取得する。
        [[nodiscard]] float
            StoppingDistance() const noexcept
        {
            return m_stoppingDistance;
        }
        // 進行方向でローカル回転を更新する設定か確認する。
        [[nodiscard]] bool
            RotateToPath() const noexcept
        {
            return m_rotateToPath;
        }
        // 最後に経路とともに設定した目的地を取得する。
        [[nodiscard]] const DirectX::XMFLOAT3&
            Destination() const noexcept
        {
            return m_destination;
        }
        // 始点を含むワールド座標の経路を取得する。
        [[nodiscard]] const std::vector<
            DirectX::XMFLOAT3>&
            Path() const noexcept
        {
            return m_path;
        }
        // 次に向かう経路点が残っているか確認する。
        [[nodiscard]] bool
            HasPath() const noexcept
        {
            return m_currentWaypoint
                < m_path.size();
        }
        // 経路の完了・空の経路・Stopによる停止のいずれかの状態か確認する。
        [[nodiscard]] bool
            HasArrived() const noexcept
        {
            return m_arrived;
        }

        // 指定サーフェスで経路を設定し、非空の経路が得られたか返す(destination: ワールド座標の目的地, navMesh: 探索するサーフェス)。
        bool SetDestination(
            const DirectX::XMFLOAT3&
                destination,
            const NavMeshComponent& navMesh);
        // ベイク済みサーフェスを選んで経路を設定し、候補がなければStopしてfalseを返す(destination: ワールド座標の目的地)。
        bool SetDestination(
            const DirectX::XMFLOAT3& destination);
        // 現在地と目的地のXZ包含を優先し、同点なら現在地に高さが近いベイク済みサーフェスを選ぶ(destination: ワールド座標の目的地)。
        // コンポーネントの有効状態は候補選択で判定しない。
        [[nodiscard]] const NavMeshComponent*
            FindBestNavMesh(
                const DirectX::XMFLOAT3&
                    destination) const;
        // 始点を含む経路を複製し、二番目の点から移動を始める(destination: 記録するワールド目的地, path: 始点と以降の経路点)。
        // 非空の経路は二点以上を前提とし、高さ方向には移動しない。
        void SetPath(
            const DirectX::XMFLOAT3&
                destination,
            std::span<
                const DirectX::XMFLOAT3>
                    path);
        // 経路を解除して到着状態とし、記録した目的地は保持する。
        void Stop() noexcept;

        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "NavMeshAgent";
        }

    protected:
        // 次の経路点へXZ方向に移動し、一回の更新で最大一つの点を消化する(deltaTime: 有限な経過秒数)。
        void OnUpdate(float deltaTime) override;
        // 記録した経路を少し高い位置に線で描く(graphics: デバッグ描画機器, view: ビュー行列, projection: 射影行列)。
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX
                projection) override;

    private:
        // 毎秒の移動距離
        float m_speed;
        // 最終点の到達許容距離
        float m_stoppingDistance;
        // ローカル回転を更新する指定
        bool m_rotateToPath;
        // 記録したワールド目的地
        DirectX::XMFLOAT3 m_destination{};
        // 始点を含むワールド経路
        std::vector<DirectX::XMFLOAT3> m_path;
        // 次に向かう経路点の番号
        std::size_t m_currentWaypoint{};
        // 到着または停止の状態
        bool m_arrived{ true };
    };
}
