#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <functional>
#include <string>

namespace LamaPon
{
    class DebugRenderer;
    class Scene;
    using GameObjectId = std::uint64_t;

    // 物理ボディと接触を一覧化し、Scene Viewへ接触・速度を重ね描きします。
    class PhysicsDebuggerPanel final
    {
    public:
        // オブジェクトを選択します（GameObjectId: 選択対象のID）。
        using SelectObject = std::function<void(GameObjectId)>;

        enum class BodyFilter
        {
            All,
            Dynamic,
            Kinematic,
            StaticCollider,
            Sleeping
        };

        // 選択通知を保持します(select: オブジェクトの選択通知・空も可)。
        explicit PhysicsDebuggerPanel(SelectObject select);

        // 表示設定の独立性を保つためコピーを禁止します。
        PhysicsDebuggerPanel(const PhysicsDebuggerPanel&) = delete;
        // 表示設定の独立性を保つためコピー代入を禁止します。
        PhysicsDebuggerPanel& operator=(
            const PhysicsDebuggerPanel&) = delete;

        // 物理統計とボディ・接触一覧を描画します(title: ウィンドウ名, open: 表示状態, scene: 表示対象のシーン, selectedObject: 選択対象のID)。
        void Draw(
            const char* title,
            bool& open,
            Scene& scene,
            GameObjectId selectedObject);
        // 毎フレーム呼び、開いている間だけ接触記録と補助描画を有効にします(scene: 接触を記録するシーン, panelOpen: パネルが開いているか)。
        void SynchronizeCapture(Scene& scene, bool panelOpen);
        // 有効な間だけ接触・速度の補助線を描画します(scene: 読み取るシーン, debug: 補助線の描画先, view: ビュー行列, projection: 射影行列, selectedObject: 絞り込み対象のID)。
        void DrawSceneOverlay(
            const Scene& scene,
            DebugRenderer& debug,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            GameObjectId selectedObject) const;

    private:
        // 名前と種類で絞り込んだボディ一覧を描画します(scene: 読み取るシーン, selectedObject: 選択対象のID)。
        void DrawBodyTable(
            const Scene& scene,
            GameObjectId selectedObject);
        // 記録済みの接触一覧を描画します(scene: 接触を読み取るシーン)。
        void DrawContactTable(const Scene& scene);

        // 選択通知の所有先
        SelectObject m_select;
        // 補助描画と接触記録が有効か
        bool m_overlayActive{};
        // 接触点と法線を描画するか
        bool m_showContacts{ true };
        // 速度を描画するか
        bool m_showVelocities{ true };
        // 角速度を描画するか
        bool m_showAngularVelocities{};
        // 選択対象だけを重ね描きするか
        bool m_onlySelected{};
        // 速度ベクトルの表示倍率
        float m_velocityScale{ 0.25f };
        // 接触法線の表示長・m
        float m_normalLength{ 0.4f };
        // 一覧表示するボディの種類
        BodyFilter m_bodyFilter{ BodyFilter::All };
        // 名前の部分一致フィルター
        std::string m_filter;
    };
}
