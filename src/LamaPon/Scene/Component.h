#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

#include <string_view>

namespace LamaPon
{
    class GameObject;
    class GraphicsDevice;
    class Script;
    struct FrameDebugDrawDescription;
    class SpriteDrawContext;
    struct Transform;

    class Component
    {
    public:
        // 派生コンポーネントを仮想破棄します。
        virtual ~Component() = default;

        // 所有先と実行状態の重複を防ぐためコピーを禁止します。
        Component(const Component&) = delete;
        // 所有先と実行状態の重複を防ぐためコピー代入を禁止します。
        Component& operator=(const Component&) = delete;

        // 所属済みコンポーネントの所有オブジェクトを参照します。
        [[nodiscard]] GameObject& Owner() const noexcept;
        // 所属オブジェクトのTransformを参照します。
        [[nodiscard]] Transform& GetTransform() const noexcept;

        // スクリプト実体を非所有参照で返し、未対応ならnullptrです。
        // 実体からScriptへの基底変換はGame Module側で行います。
        [[nodiscard]] virtual Script* ScriptInstance() const noexcept
        {
            return nullptr;
        }

        // コンポーネント自身の有効設定を返します。
        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        // 有効設定を変更して稼働状態を更新します(enabled: 新しい有効設定)。
        void SetEnabled(bool enabled);
        // 初期化済みかつ自身・所有オブジェクト・全祖先が有効な稼働状態を返します。
        [[nodiscard]] bool IsActiveAndEnabled() const noexcept
        {
            return m_lastActiveState;
        }
        // コンポーネントの型名を返します。
        [[nodiscard]] virtual std::string_view TypeName() const noexcept
        {
            return "Component";
        }
        // 2D/UIの描画順を返します。
        // 大きい値を後から描画し、描画しないコンポーネントは0です。
        [[nodiscard]] virtual int RenderSortOrder() const noexcept
        {
            return 0;
        }
        // 描画イベントの内容を設定します(description: フレームデバッガーへの出力)。
        // デバッガー有効時の描画直前に呼ばれ、描画する派生型だけがtrueを返します。
        [[nodiscard]] virtual bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const
        {
            static_cast<void>(description);
            return false;
        }

    protected:
        // 未所属・未初期化のコンポーネントを構築します。
        Component() = default;

        // 描画デバイスを受けて一度だけ初期化します。
        virtual void OnInitialize(GraphicsDevice&) {}
        // フレームの経過秒数を受けて通常更新します。
        virtual void OnUpdate(float) {}
        // 通常更新と全固定更新後に経過秒数を受けて描画への追従を更新します。
        virtual void OnLateUpdate(float) {}
        // 固定刻みの秒数を受けて物理計算などの固定更新を行います。
        virtual void OnFixedUpdate(float) {}
        // 同種の連続対象をまとめる3D事前パスが必要か返します。
        [[nodiscard]] virtual bool HasPreRender3DPass() { return false; }
        // アルファ合成で奥から手前への描画が必要か返します。
        // trueは不透明描画後に並べ替えられ、加算合成ではfalseを返します。
        [[nodiscard]] virtual bool IsAlphaBlended3D() const
        {
            return false;
        }
        // ビュー・投影行列を受けて通常描画前の3Dパスを描きます。
        virtual void OnPreRender3D(
            DirectX::FXMMATRIX,
            DirectX::CXMMATRIX) {}
        // ビュー・投影行列を受けて通常の3D描画を行います。
        virtual void OnRender3D(DirectX::FXMMATRIX, DirectX::CXMMATRIX) {}
        // デバイス・ビュー・投影行列を受けて3D補助描画を行います。
        virtual void OnRenderDebug3D(
            GraphicsDevice&,
            DirectX::FXMMATRIX,
            DirectX::CXMMATRIX) {}
        // スプライト描画コンテキストを受けて2Dを描きます。
        virtual void OnRender2D(const SpriteDrawContext&) {}
        // 通常接触の情報を受けて接触開始を処理します。
        virtual void OnCollisionEnter(const CollisionEvent&) {}
        // 通常接触の情報を受けて接触継続を処理します。
        virtual void OnCollisionStay(const CollisionEvent&) {}
        // 通常接触の情報を受けて接触終了を処理します。
        virtual void OnCollisionExit(const CollisionEvent&) {}
        // トリガー接触の情報を受けて接触開始を処理します。
        // トリガー接触は通常接触のフックへ通知しません。
        virtual void OnTriggerEnter(const CollisionEvent&) {}
        // トリガー接触の情報を受けて接触継続を処理します。
        virtual void OnTriggerStay(const CollisionEvent&) {}
        // トリガー接触の情報を受けて接触終了を処理します。
        virtual void OnTriggerExit(const CollisionEvent&) {}
        // 変更後の稼働状態を受けて有効・無効の切り替えを処理します。
        virtual void OnActiveStateChanged(bool) {}

    private:
        friend class GameObject;

        // 未初期化なら初期化して稼働状態を反映します(graphics: 描画デバイス)。
        void InitializeIfNeeded(GraphicsDevice& graphics);
        // 所有階層から稼働状態を求めて変化時に通知します。
        void RefreshActiveState();

        // 所有GameObject
        GameObject* m_owner{};
        // 初期化済み
        bool m_initialized{};
        // 有効設定
        bool m_enabled{ true };
        // 前回の有効状態
        bool m_lastActiveState{};
    };
}
