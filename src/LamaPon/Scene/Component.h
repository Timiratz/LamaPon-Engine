#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

#include <string_view>

namespace DirectX
{
    inline namespace DX11
    {
        class SpriteBatch;
    }
}

struct ID3D11ShaderResourceView;

namespace LamaPon
{
    class GameObject;
    class GraphicsDevice;
    class Script;
    struct Transform;

    class Component
    {
    public:
        virtual ~Component() = default;

        Component(const Component&) = delete;
        Component& operator=(const Component&) = delete;

        [[nodiscard]] GameObject& Owner() const noexcept;
        [[nodiscard]] Transform& GetTransform() const noexcept;

        // このComponentがC++スクリプトの実体を持っているなら、それを
        // Script*として返します（持たないComponentはnullptr）。
        // GameObject::GetScript<T>()が自作インターフェースを引くために
        // 使います。NativeScriptComponentは実体をvoid*で保持している
        // ため、上位変換はGame Module側の関数に任せます。
        [[nodiscard]] virtual Script* ScriptInstance() const noexcept
        {
            return nullptr;
        }

        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        void SetEnabled(bool enabled);
        // Componentが有効・初期化済みで、かつ祖先GameObjectが
        // すべて有効な時にtrueになります。
        [[nodiscard]] bool IsActiveAndEnabled() const noexcept
        {
            return m_lastActiveState;
        }
        [[nodiscard]] virtual std::string_view TypeName() const noexcept
        {
            return "Component";
        }
        // 2D/UIスプライトパスの描画順です。値が大きいほど後から描画し、
        // 描画を行わないコンポーネントは0を返します。
        [[nodiscard]] virtual int RenderSortOrder() const noexcept
        {
            return 0;
        }

    protected:
        Component() = default;

        virtual void OnInitialize(GraphicsDevice&) {}
        virtual void OnUpdate(float) {}
        // 全OnUpdateと全固定更新・物理計算の後。描画の追従に使います。
        virtual void OnLateUpdate(float) {}
        // プロジェクト設定の固定間隔（既定60Hz）。継続的な力を加えます。
        virtual void OnFixedUpdate(float) {}
        // 通常の3D描画より前に、同種の連続した対象をまとめて描画するための事前パスです。
        // 遮蔽シルエットなど、複数パーツ間で深度を共有したくない描画に使用します。
        [[nodiscard]] virtual bool HasPreRender3DPass() { return false; }
        // アルファ合成で描く3D描画か。trueを返したものは不透明を
        // 描き終えた後、カメラから遠い順に並べ替えて描かれます
        // （手前から描くと、後ろのものが深度に阻まれて消えるため）。
        //
        // 加算合成はここでtrueを返さないでください。足し算は順番に
        // よらないので並べ替えても絵が変わらず、インスタンシングで
        // まとめられる利点だけを失います。
        [[nodiscard]] virtual bool IsAlphaBlended3D() const
        {
            return false;
        }
        virtual void OnPreRender3D(
            DirectX::FXMMATRIX,
            DirectX::CXMMATRIX) {}
        virtual void OnRender3D(DirectX::FXMMATRIX, DirectX::CXMMATRIX) {}
        virtual void OnRenderDebug3D(
            GraphicsDevice&,
            DirectX::FXMMATRIX,
            DirectX::CXMMATRIX) {}
        virtual void OnRender2D(DirectX::SpriteBatch&, ID3D11ShaderResourceView*) {}
        virtual void OnCollisionEnter(const CollisionEvent&) {}
        virtual void OnCollisionStay(const CollisionEvent&) {}
        virtual void OnCollisionExit(const CollisionEvent&) {}
        // トリガー接触（isTriggerコライダー）はCollisionコール
        // バックの代わりにこちらへ届きます。
        virtual void OnTriggerEnter(const CollisionEvent&) {}
        virtual void OnTriggerStay(const CollisionEvent&) {}
        virtual void OnTriggerExit(const CollisionEvent&) {}
        // IsActiveAndEnabled()が変化した時に呼ばれます。
        virtual void OnActiveStateChanged(bool) {}

    private:
        friend class GameObject;

        void InitializeIfNeeded(GraphicsDevice& graphics);
        void RefreshActiveState();

        GameObject* m_owner{};
        bool m_initialized{};
        bool m_enabled{ true };
        bool m_lastActiveState{};
    };
}
