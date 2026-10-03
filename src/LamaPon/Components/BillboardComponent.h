#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    // 対象の位置を向く方式と、カメラの軸方向に合わせる方式を選びます。
    enum class BillboardMode
    {
        // カメラの座標を向きます。
        FaceCameraPosition,
        // 画面と平行になります（カメラの向きに合わせる）。
        ScreenAligned,
        // 対象への水平方向に向け、上方向の基準にワールドY軸を使います。
        UprightFaceCameraPosition,
        // カメラ軸の水平方向に合わせ、上方向の基準にワールドY軸を使います。
        UprightScreenAligned,
        // カメラではなく、指定したワールド座標を向きます。
        LookAtPosition
    };

    // 対象方向へ向けるローカル軸を選び、Planeの面法線が+Yなので既定はUpです。
    enum class BillboardFacingAxis
    {
        // ローカル+Y軸を対象方向へ向けます。
        Up,
        // ローカル+Z軸を対象方向へ向けます。
        Forward
    };

    // Main Cameraまたは指定位置を基準に回転だけを毎更新上書きします。
    class BillboardComponent final : public Component
    {
    public:
        // 指定軸をカメラ基準の方向へ向ける部品を作ります(mode: 向け方, facingAxis: 向けるローカル軸)。
        explicit BillboardComponent(
            BillboardMode mode =
                BillboardMode::ScreenAligned,
            BillboardFacingAxis facingAxis =
                BillboardFacingAxis::Up) noexcept;

        // 方向の決定方式を返します。
        [[nodiscard]] BillboardMode Mode() const noexcept
        {
            return m_mode;
        }
        // 方向の決定方式を設定します(mode: 向け方)。
        void SetMode(const BillboardMode mode) noexcept
        {
            m_mode = mode;
        }

        // 対象方向へ向けるローカル軸を返します。
        [[nodiscard]] BillboardFacingAxis
            FacingAxis() const noexcept
        {
            return m_facingAxis;
        }
        // 対象方向へ向ける軸を設定します(axis: ローカル軸)。
        void SetFacingAxis(
            const BillboardFacingAxis axis) noexcept
        {
            m_facingAxis = axis;
        }

        // 座標指定モードで向くワールド位置を返します。
        [[nodiscard]] const DirectX::XMFLOAT3&
            TargetPosition() const noexcept
        {
            return m_targetPosition;
        }
        // 座標指定モードの対象を設定します(position: 向くワールド位置)。
        void SetTargetPosition(
            const DirectX::XMFLOAT3& position) noexcept
        {
            m_targetPosition = position;
        }

        // 更新後のカメラ基準でローカル回転を上書きします(deltaTime: 使用しない経過秒数)。
        // 親の変換を分解できない場合は親回転を打ち消せず、方向が短すぎる場合は前の回転を保持します。
        void OnLateUpdate(float deltaTime) override;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "Billboard";
        }

    private:
        // 向きを決めるのに必要なカメラの情報。
        struct CameraFrame final
        {
            // カメラのワールド位置
            DirectX::XMFLOAT3 position{};
            // カメラのワールド+Z単位軸
            DirectX::XMFLOAT3 forward{ 0.0f, 0.0f, 1.0f };
            // カメラのワールド+Y単位軸
            DirectX::XMFLOAT3 up{ 0.0f, 1.0f, 0.0f };
        };
        // 有効なMain Cameraの情報を取得します(frame: カメラ基準の出力)。
        // 座標指定モードでは出力を変更せずtrue、カメラを取得できなければfalseを返します。
        [[nodiscard]] bool ResolveCamera(
            CameraFrame& frame) const noexcept;
        // カメラの軸方向に合わせる方式か返します。
        [[nodiscard]] bool UsesViewDirection() const noexcept;
        // 水平方向に向け上の基準にワールドY軸を使う方式か返します。
        [[nodiscard]] bool IsUpright() const noexcept;

        // 方向の決定方式
        BillboardMode m_mode{
            BillboardMode::ScreenAligned };
        // 対象方向へ向けるローカル軸
        BillboardFacingAxis m_facingAxis{
            BillboardFacingAxis::Up };
        // 座標指定モードの対象位置
        DirectX::XMFLOAT3 m_targetPosition{};
    };
}
