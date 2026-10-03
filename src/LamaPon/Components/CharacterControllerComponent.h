#pragma once

#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>

namespace LamaPon
{
    class InputSystem;
    class GraphicsDevice;

    // 形状・速度・移動量・経過時間には有限値を指定します。
    class CharacterControllerComponent final : public Component
    {
    public:
        // 軸平行境界で移動する制御部品を作ります(radius: ワールドXZ半幅, height: ワールド全高, moveSpeed: 移動速度毎秒, gravity: 下向き加速度毎秒二乗, jumpSpeed: ジャンプ速度毎秒, stepOffset: 登れる段差高, skinWidth: 境界を内側に縮める幅, layer: 所属層番号, collisionMask: 相手の許可層ビット)。
        CharacterControllerComponent(
            float radius = 0.4f,
            float height = 1.8f,
            float moveSpeed = 4.0f,
            float gravity = 20.0f,
            float jumpSpeed = 7.0f,
            float stepOffset = 0.3f,
            float skinWidth = 0.03f,
            std::uint32_t layer = 2,
            std::uint32_t collisionMask = 0xffffffffu);

        // ワールドXZの半幅を返します。
        [[nodiscard]] float Radius() const noexcept { return m_radius; }
        // 半幅を0.05〜100に制限して全高と境界縮小幅を調整します(value: ワールド半幅)。
        void SetRadius(float value) noexcept;
        // 足元からのワールド全高を返します。
        [[nodiscard]] float Height() const noexcept { return m_height; }
        // 全高を直径〜1000に制限して段差高も調整します(value: ワールド全高)。
        void SetHeight(float value) noexcept;
        // 移動速度をワールド単位毎秒で返します。
        [[nodiscard]] float MoveSpeed() const noexcept { return m_moveSpeed; }
        // 移動速度を0〜1000に制限して設定します(value: ワールド単位毎秒)。
        void SetMoveSpeed(float value) noexcept;
        // 下向き重力加速度をワールド単位毎秒二乗で返します。
        [[nodiscard]] float Gravity() const noexcept { return m_gravity; }
        // 重力加速度を0〜1000に制限して設定します(value: ワールド単位毎秒二乗)。
        void SetGravity(float value) noexcept;
        // ジャンプ速度をワールド単位毎秒で返します。
        [[nodiscard]] float JumpSpeed() const noexcept { return m_jumpSpeed; }
        // ジャンプ速度を0〜1000に制限して設定します(value: ワールド単位毎秒)。
        void SetJumpSpeed(float value) noexcept;
        // 登れる段差高をワールド単位で返します。
        [[nodiscard]] float StepOffset() const noexcept { return m_stepOffset; }
        // 段差高を0〜全高に制限して設定します(value: ワールド段差高)。
        void SetStepOffset(float value) noexcept;
        // 境界を内側に縮める幅をワールド単位で返します。
        [[nodiscard]] float SkinWidth() const noexcept { return m_skinWidth; }
        // 境界縮小幅を0.001〜半径の半分に制限します(value: ワールド縮小幅)。
        void SetSkinWidth(float value) noexcept;
        // 所属層の番号を返します。
        [[nodiscard]] std::uint32_t Layer() const noexcept { return m_layer; }
        // 所属層を32の剰余で設定します(value: 層番号)。
        void SetLayer(std::uint32_t value) noexcept { m_layer = value % 32u; }
        // 衝突を許可する相手層のビット集合を返します。
        [[nodiscard]] std::uint32_t CollisionMask() const noexcept
        {
            return m_collisionMask;
        }
        // 相手層の許可ビットを設定します(value: 層ビット集合)。
        void SetCollisionMask(std::uint32_t value) noexcept
        {
            m_collisionMask = value;
        }

        // 入力による移動とジャンプの指定を返します。
        [[nodiscard]] bool UseInput() const noexcept { return m_useInput; }
        // 入力の使用を設定します(value: 入力を使う指定)。
        // 無効にしても重力・Move・Jumpの処理は続きます。
        void SetUseInput(bool value) noexcept { m_useInput = value; }
        // X移動に使う入力名を返します。
        [[nodiscard]] const std::string& HorizontalAction() const noexcept
        {
            return m_horizontalAction;
        }
        // X移動の入力名を検証して設定します(value: 空白だけでない1〜64バイトの入力名)。
        void SetHorizontalAction(std::string value);
        // 負Z移動に使う入力名を返します。
        [[nodiscard]] const std::string& VerticalAction() const noexcept
        {
            return m_verticalAction;
        }
        // 負Z移動の入力名を検証して設定します(value: 空白だけでない1〜64バイトの入力名)。
        void SetVerticalAction(std::string value);
        // ジャンプに使う入力名を返します。
        [[nodiscard]] const std::string& JumpAction() const noexcept
        {
            return m_jumpAction;
        }
        // ジャンプの入力名を検証して設定します(value: 空白だけでない1〜64バイトの入力名)。
        void SetJumpAction(std::string value);

        // 直近の更新で地面を検出したか返します。
        [[nodiscard]] bool IsGrounded() const noexcept { return m_grounded; }
        // 鉛直速度をワールド単位毎秒で返します。
        [[nodiscard]] float VerticalVelocity() const noexcept
        {
            return m_verticalVelocity;
        }
        // 足元のワールド位置から縮小済みの軸平行境界を返します。
        // 物体の回転とスケールは形状に適用しません。
        [[nodiscard]] Bounds3D WorldBounds() const noexcept;

        // 次の更新で消費する移動量を加算します(displacement: ワールドXYZ移動量)。
        void Move(const DirectX::XMFLOAT3& displacement) noexcept;
        // 次の更新で接地していれば実行するジャンプを要求します。
        void Jump() noexcept { m_jumpRequested = true; }

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "CharacterController";
        }

    protected:
        // 入力と描画装置を借用します(graphics: 入力を提供する描画装置)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // 入力・重力・予約移動を分割して衝突を避けます(deltaTime: 0〜0.1秒に制限する経過時間)。
        // 分割内ではX・Z・Yの順に移動し、ジャンプ要求は接地の有無によらず消費します。
        void OnUpdate(float deltaTime) override;
        // 接地状態に応じた色で境界を描画します(graphics: 描画装置, view: ビュー行列, projection: 射影行列)。
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // 入力名が空・容量超過・空白だけなら例外を送出します(value: 検証する入力名)。
        static void ValidateActionName(std::string_view value);
        // 自身を除いた非トリガーと双方のマスク・層行列で衝突判定します(bounds: ワールド境界)。
        [[nodiscard]] bool HasCollision(const Bounds3D& bounds) const;
        // 足元直下の薄い境界で地面を調べます(probeDistance: 下方へ調べるワールド距離)。
        [[nodiscard]] bool HasGround(float probeDistance) const;
        // 衝突しない1軸の移動を試みます(axis: 0〜2のXYZ軸番号, amount: ワールド移動量, allowStep: 接地中の段差上昇を試す指定)。
        bool MoveAxis(int axis, float amount, bool allowStep);

        // ワールドXZ半幅
        float m_radius{ 0.4f };
        // 足元からのワールド全高
        float m_height{ 1.8f };
        // 移動速度ワールド単位毎秒
        float m_moveSpeed{ 4.0f };
        // 下向き加速度毎秒二乗
        float m_gravity{ 20.0f };
        // ジャンプ速度毎秒
        float m_jumpSpeed{ 7.0f };
        // 登れるワールド段差高
        float m_stepOffset{ 0.3f };
        // 境界を内側に縮める幅
        float m_skinWidth{ 0.03f };
        // 所属層の番号
        std::uint32_t m_layer{ 2 };
        // 相手層の許可ビット集合
        std::uint32_t m_collisionMask{ 0xffffffffu };
        // 入力を使う指定
        bool m_useInput{ true };
        // X移動に使う入力名
        std::string m_horizontalAction{ "MoveHorizontal" };
        // 負Z移動に使う入力名
        std::string m_verticalAction{ "MoveVertical" };
        // ジャンプに使う入力名
        std::string m_jumpAction{ "Jump" };
        // 借用する入力システム
        InputSystem* m_input{};
        // 借用する描画装置
        GraphicsDevice* m_graphics{};
        // 次回更新の予約ワールド移動
        DirectX::XMFLOAT3 m_queuedDisplacement{};
        // 鉛直速度ワールド単位毎秒
        float m_verticalVelocity{};
        // 直近の更新で接地した状態
        bool m_grounded{};
        // 次回更新のジャンプ要求
        bool m_jumpRequested{};
    };
}
