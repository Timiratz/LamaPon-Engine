#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    // ビルボードの向け方。
    //
    // 「カメラの位置を向く」系と「カメラの向きに合わせる」系の2種類が
    // あります。違いが出るのは画面の端です。
    //   ・位置を向く … 板ごとにカメラの座標へ向くので、画面の端の板は
    //     こちらへ振り向くように回って見えます。球や球状の光には
    //     こちらが自然です
    //   ・向きに合わせる … 全部が画面と平行になるので、どこにあっても
    //     同じ向き・同じ形で見えます。歪みが出ないため、板状の絵
    //     （アイコン、パーティクル）にはこちらが向きます
    enum class BillboardMode
    {
        // カメラの座標を向きます。
        FaceCameraPosition,
        // 画面と平行になります（カメラの向きに合わせる）。
        ScreenAligned,
        // 上下は起こしたまま、横方向だけカメラの座標を向きます。
        // 木・草・頭上のHPバーなど「立っているもの」に向きます。
        UprightFaceCameraPosition,
        // 上下は起こしたまま、横方向だけカメラの向きに合わせます。
        // 並べた草がすべて同じ向きになるので、列が揃って見えます。
        UprightScreenAligned,
        // カメラではなく、指定したワールド座標を向きます。
        LookAtPosition
    };

    // どのローカル軸をカメラへ向けるか。
    //
    // 「3D空間に置いた画像」はMesh RendererのPlaneで作るのが普通で、
    // Planeの面は+Y（真上）を向いています。そのため既定はUpです。
    // glTF/FBXのモデルのように正面が+Zのものは Forward にします。
    enum class BillboardFacingAxis
    {
        Up,
        Forward
    };

    // GameObjectをカメラの方へ向け続けます。
    //
    // 回転だけを毎フレーム上書きします（位置と大きさは触りません）。
    // 親がいる場合は、親の回転を打ち消したローカル回転を入れるので、
    // 親ごと回してもカメラを向いたままになります。
    //
    // 向くのは「メインカメラ」です。エディターのシーンビューは
    // 自分のカメラで見ているため、再生していないあいだは向きが
    // 変わりません（付けた向きのまま編集できます）。
    class BillboardComponent final : public Component
    {
    public:
        explicit BillboardComponent(
            BillboardMode mode =
                BillboardMode::ScreenAligned,
            BillboardFacingAxis facingAxis =
                BillboardFacingAxis::Up) noexcept;

        [[nodiscard]] BillboardMode Mode() const noexcept
        {
            return m_mode;
        }
        void SetMode(const BillboardMode mode) noexcept
        {
            m_mode = mode;
        }

        [[nodiscard]] BillboardFacingAxis
            FacingAxis() const noexcept
        {
            return m_facingAxis;
        }
        void SetFacingAxis(
            const BillboardFacingAxis axis) noexcept
        {
            m_facingAxis = axis;
        }

        // LookAtPositionのときに向く先（ワールド座標）。
        [[nodiscard]] const DirectX::XMFLOAT3&
            TargetPosition() const noexcept
        {
            return m_targetPosition;
        }
        void SetTargetPosition(
            const DirectX::XMFLOAT3& position) noexcept
        {
            m_targetPosition = position;
        }

        // OnUpdateではなくOnLateUpdateで向けます。カメラを追従
        // させるスクリプトはOnUpdateでカメラを動かすので、そこで
        // 向けると1フレーム前のカメラ位置を見てしまいます。
        void OnLateUpdate(float deltaTime) override;

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "Billboard";
        }

    private:
        // 向きを決めるのに必要なカメラの情報。
        struct CameraFrame final
        {
            DirectX::XMFLOAT3 position{};
            // カメラが見ている向き（ローカル+Z）。
            DirectX::XMFLOAT3 forward{ 0.0f, 0.0f, 1.0f };
            DirectX::XMFLOAT3 up{ 0.0f, 1.0f, 0.0f };
        };
        // メインカメラが取れなければfalse。座標指定モードでは
        // カメラを使わないのでtrueを返します。
        [[nodiscard]] bool ResolveCamera(
            CameraFrame& frame) const noexcept;
        // このモードが「カメラの向きに合わせる」系か。
        [[nodiscard]] bool UsesViewDirection() const noexcept;
        // このモードが上下を起こしたままにする（Y軸のみ）か。
        [[nodiscard]] bool IsUpright() const noexcept;

        BillboardMode m_mode{
            BillboardMode::ScreenAligned };
        BillboardFacingAxis m_facingAxis{
            BillboardFacingAxis::Up };
        DirectX::XMFLOAT3 m_targetPosition{};
    };
}
