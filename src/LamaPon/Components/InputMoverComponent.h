#pragma once

#include "LamaPon/Scene/Component.h"

#include <string>

namespace LamaPon
{
    class InputSystem;

    class InputMoverComponent final : public Component
    {
    public:
        // 入力でワールドXZ移動する部品を作ります(horizontalAction: X移動の入力名, verticalAction: 負Z移動の入力名, speed: 非負に制限する毎秒速度)。
        // 速度は有限値を指定し、入力名は空白だけでない1〜64バイトを指定します。
        explicit InputMoverComponent(
            std::string horizontalAction = "MoveHorizontal",
            std::string verticalAction = "MoveVertical",
            float speed = 3.0f);

        // X移動の入力名を検証して設定します(action: 空白だけでない1〜64バイトの入力名)。
        void SetHorizontalAction(std::string action);
        // X移動に使う入力名を返します。
        [[nodiscard]] const std::string&
            HorizontalAction() const noexcept
        {
            return m_horizontalAction;
        }
        // 負Z移動の入力名を検証して設定します(action: 空白だけでない1〜64バイトの入力名)。
        void SetVerticalAction(std::string action);
        // 負Z移動に使う入力名を返します。
        [[nodiscard]] const std::string&
            VerticalAction() const noexcept
        {
            return m_verticalAction;
        }
        // 速度を0〜1000に制限して設定します(speed: 有限のワールド単位毎秒)。
        void SetSpeed(float speed) noexcept;
        // 移動速度をワールド単位毎秒で返します。
        [[nodiscard]] float Speed() const noexcept
        {
            return m_speed;
        }

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "InputMover";
        }

    protected:
        // 入力システムを借用します(graphics: 入力システムを持つ描画装置)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // 入力の長さを1以下にしてワールドXZ移動します(deltaTime: 経過秒数)。
        void OnUpdate(float deltaTime) override;

    private:
        // 入力名が空・容量超過・空白だけなら例外を送出します(action: 検証する入力名)。
        static void ValidateActionName(
            std::string_view action);

        // X移動に使う入力名
        std::string m_horizontalAction;
        // 負Z移動に使う入力名
        std::string m_verticalAction;
        // 移動速度ワールド単位毎秒
        float m_speed{ 3.0f };
        // 借用する入力システム
        InputSystem* m_input{};
    };
}
