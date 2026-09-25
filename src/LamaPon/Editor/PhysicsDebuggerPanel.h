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

    // 物理の状態を調べるパネルです（UnityのPhysics Debuggerに相当）。
    // ボディと接触の一覧、Scene Viewへの接触点・法線・速度の重ね描きを
    // 提供します。表示の設定を所有し、Sceneは借用して読むだけです
    // （接触の記録の開始・停止だけはSynchronizeCaptureで切り替えます）。
    class PhysicsDebuggerPanel final
    {
    public:
        using SelectObject = std::function<void(GameObjectId)>;

        enum class BodyFilter
        {
            All,
            Dynamic,
            Kinematic,
            StaticCollider,
            Sleeping
        };

        explicit PhysicsDebuggerPanel(SelectObject select);

        PhysicsDebuggerPanel(const PhysicsDebuggerPanel&) = delete;
        PhysicsDebuggerPanel& operator=(
            const PhysicsDebuggerPanel&) = delete;

        void Draw(
            const char* title,
            bool& open,
            Scene& scene,
            GameObjectId selectedObject);
        // パネルを開いている間だけSceneに接触点を記録させます。
        // EditorLayerが毎フレーム、パネルの開閉状態を渡して呼びます。
        void SynchronizeCapture(Scene& scene, bool panelOpen);
        // Scene Viewの補助表示へ接触点・法線・速度を描きます。パネルを
        // 閉じている間は何も描きません。
        void DrawSceneOverlay(
            const Scene& scene,
            DebugRenderer& debug,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            GameObjectId selectedObject) const;

    private:
        void DrawBodyTable(
            const Scene& scene,
            GameObjectId selectedObject);
        void DrawContactTable(const Scene& scene);

        SelectObject m_select;
        bool m_overlayActive{};
        bool m_showContacts{ true };
        bool m_showVelocities{ true };
        bool m_showAngularVelocities{};
        bool m_onlySelected{};
        float m_velocityScale{ 0.25f };
        float m_normalLength{ 0.4f };
        BodyFilter m_bodyFilter{ BodyFilter::All };
        std::string m_filter;
    };
}
