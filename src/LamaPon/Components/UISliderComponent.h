#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class GraphicsDevice;

    // 表示矩形をクリックまたはドラッグして値を変える水平スライダー。
    // 数値設定は有限値を前提とし、描画と入力判定は所有者の回転・拡縮を適用しない。
    class UISliderComponent final : public Component
    {
    public:
        // 上限を下限以上に収めて水平スライダーを作る(minimumValue: 値の下限, maximumValue: 値の上限, value: 範囲に収める初期値)。
        explicit UISliderComponent(
            float minimumValue = 0.0f,
            float maximumValue = 1.0f,
            float value = 0.5f) noexcept;

        // 上限を下限以上に収め、現在値へ制約を適用する(minimumValue: 値の下限, maximumValue: 値の上限)。
        void SetRange(
            float minimumValue,
            float maximumValue) noexcept;
        // 値へ制約を適用し、値が変わった場合だけ変更通知を記録する(value: 設定する値)。
        void SetValue(float value) noexcept;
        // 0〜1に収めた割合から値を設定する(normalized: 下限から上限までの割合)。
        void SetNormalizedValue(float normalized) noexcept;
        // 整数への丸めを切り替え、現在値へ制約を適用する(wholeNumbers: 整数へ丸める指定)。
        // 範囲に収めた後に丸めるため、範囲内を保証するには上下限を整数にする。
        void SetWholeNumbers(bool wholeNumbers) noexcept;
        // 操作可否を設定し、操作禁止時は押下状態を解除する(interactable: 操作を許可する指定)。
        void SetInteractable(bool interactable) noexcept;
        // バー全体の背景色を設定する(color: アルファ乗算前のRGBA)。
        void SetBackgroundColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_backgroundColor = color;
        }
        // 値までの塗りつぶし色を設定する(color: アルファ乗算前のRGBA)。
        void SetFillColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_fillColor = color;
        }
        // ハンドルの色を設定する(color: アルファ乗算前のRGBA)。
        void SetHandleColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_handleColor = color;
        }
        // UI矩形がない場合の表示サイズを各軸1以上に収める(size: 表示幅と高さのピクセル数)。
        void SetFallbackSize(
            const DirectX::XMFLOAT2& size) noexcept;
        // 2D描画の並び順を設定する(sortOrder: 小さいほど先に描く順番)。
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }

        // 値の下限を取得する。
        [[nodiscard]] float MinimumValue() const noexcept
        {
            return m_minimumValue;
        }
        // 値の上限を取得する。
        [[nodiscard]] float MaximumValue() const noexcept
        {
            return m_maximumValue;
        }
        // 制約適用後の現在値を取得する。
        [[nodiscard]] float Value() const noexcept
        {
            return m_value;
        }
        // 現在値の範囲内での割合を求め、範囲の幅が0なら0を返す。
        [[nodiscard]] float NormalizedValue() const noexcept;
        // 範囲への制限後に整数へ丸める設定か確認する。
        [[nodiscard]] bool WholeNumbers() const noexcept
        {
            return m_wholeNumbers;
        }
        // 値の変更通知を取得し、その通知を消費する。
        [[nodiscard]] bool ConsumeValueChanged() noexcept
        {
            // 消費する値変更通知
            const bool changed = m_valueChanged;
            m_valueChanged = false;
            return changed;
        }
        // 操作を許可しているか確認する。
        [[nodiscard]] bool Interactable() const noexcept
        {
            return m_interactable;
        }
        // このスライダーで開始したドラッグが継続しているか確認する。
        [[nodiscard]] bool IsDragging() const noexcept
        {
            return m_dragging;
        }
        // バー背景のアルファ乗算前の色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            BackgroundColor() const noexcept
        {
            return m_backgroundColor;
        }
        // 塗りつぶし部分のアルファ乗算前の色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            FillColor() const noexcept
        {
            return m_fillColor;
        }
        // ハンドルのアルファ乗算前の色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            HandleColor() const noexcept
        {
            return m_handleColor;
        }
        // UI矩形がない場合の表示幅と高さを取得する。
        [[nodiscard]] const DirectX::XMFLOAT2&
            FallbackSize() const noexcept
        {
            return m_fallbackSize;
        }
        // 設定された2D描画順を取得する。
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }

        // UI部品の描画情報を記録する(description: 書き込み先の描画説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UISlider";
        }
        // 2D描画を整列するための順番を取得する。
        [[nodiscard]] int
            RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        // 描画機器を借用して初期化する(graphics: 所有者より長寿命の描画機器)。
        void OnInitialize(
            GraphicsDevice& graphics) override;
        // ポインターから操作状態を更新する(deltaTime: 未使用の経過秒数)。
        void OnUpdate(float deltaTime) override;
        // UI部品の背景と表示内容を描く(sprites: スプライト描画先)。
        void OnRender2D(
            const SpriteDrawContext& sprites) override;

    private:
        // 範囲に収めてから整数設定なら丸めた値を返す(value: 制約を適用する値)。
        [[nodiscard]] float ApplyConstraints(
            float value) const noexcept;

        // 値の下限
        float m_minimumValue;
        // 値の上限
        float m_maximumValue;
        // 制約適用後の現在値
        float m_value;
        // 整数に丸める指定
        bool m_wholeNumbers{};
        // 操作を許可する指定
        bool m_interactable{ true };
        // 継続中のドラッグ状態
        bool m_dragging{};
        // 未消費の値変更通知
        bool m_valueChanged{};
        // バー背景のRGBA
        DirectX::XMFLOAT4 m_backgroundColor{
            0.14f, 0.16f, 0.22f, 0.96f };
        // 塗りつぶし部分のRGBA
        DirectX::XMFLOAT4 m_fillColor{
            0.12f, 0.42f, 0.76f, 1.0f };
        // ハンドルのRGBA
        DirectX::XMFLOAT4 m_handleColor{
            0.92f, 0.94f, 0.98f, 1.0f };
        // 矩形未指定時の表示サイズ
        DirectX::XMFLOAT2 m_fallbackSize{
            220.0f, 24.0f };
        // 2D描画の並び順
        int m_sortOrder{};
        // 借用した描画機器
        GraphicsDevice* m_graphics{};
    };
}
