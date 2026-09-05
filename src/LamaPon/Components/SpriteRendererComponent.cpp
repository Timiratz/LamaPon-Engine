#include "LamaPon/Components/SpriteRendererComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Transform.h"

#include <SpriteBatch.h>

#include <algorithm>
#include <cmath>

namespace LamaPon
{
    SpriteRendererComponent::SpriteRendererComponent(
        const DirectX::XMFLOAT2 size,
        const DirectX::XMFLOAT4 color,
        std::filesystem::path texturePath) noexcept
        : m_size(size)
        , m_color(color)
        , m_texturePath(std::move(texturePath))
    {
    }

    void SpriteRendererComponent::OnInitialize(GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        m_assets = &graphics.Assets();
        if (!m_texturePath.empty())
        {
            m_texture = m_assets->LoadTexture(m_texturePath);
        }
    }

    void SpriteRendererComponent::SetTexturePath(std::filesystem::path texturePath)
    {
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !texturePath.empty())
        {
            texture = m_assets->LoadTexture(texturePath);
        }

        m_texturePath = std::move(texturePath);
        m_texture = std::move(texture);
    }

    void SpriteRendererComponent::ReloadShader()
    {
        if (m_graphics != nullptr
            && !m_shaderPath.empty())
        {
            m_graphics->InvalidateSpriteShader(
                m_shaderPath);
        }
    }

    DirectX::SpriteBatch&
        SpriteRendererComponent::BeginRenderBatch(
            GraphicsDevice& graphics)
    {
        auto renderParameters = m_customParameters;
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        DirectX::XMFLOAT2 position{
            world._41,
            world._42
        };
        DirectX::XMFLOAT2 drawSize = m_size;
        DirectX::XMFLOAT2 pivot = m_pivot;
        if (const auto* rectTransform =
                Owner().GetComponent<
                    UIRectTransformComponent>();
            rectTransform != nullptr)
        {
            const auto rect = rectTransform->Resolve(
                static_cast<float>(graphics.UIWidth()),
                static_cast<float>(graphics.UIHeight()));
            position = rect.minimum;
            drawSize = rect.Size();
            // UIはRect Transformが左上を決めるので、Pivotは使いません。
            pivot = { 0.0f, 0.0f };
        }
        else
        {
            const auto& offset = graphics.Sprite2DOffset();
            position.x += offset.x;
            position.y += offset.y;
            const float worldScaleX = std::sqrt(
                world._11 * world._11
                + world._12 * world._12);
            const float worldScaleY = std::sqrt(
                world._21 * world._21
                + world._22 * world._22);
            drawSize.x *= worldScaleX;
            drawSize.y *= worldScaleY;
        }

        // SpriteBatch実装差でTEXCOORDを利用できない場合にも
        // SV_Positionから正しい0～1 UVを復元できるようにする。
        // COLORも同様に予約パラメーターへ複製する。
        //
        // Shaderへ渡すのは左上の座標です。Pivotを動かすと
        // Transformの位置は左上ではなくなるため、その分を戻します。
        const DirectX::XMFLOAT2 topLeft{
            position.x - pivot.x * drawSize.x,
            position.y - pivot.y * drawSize.y
        };
        renderParameters[5] = m_color;
        renderParameters[6] = {
            topLeft.x,
            topLeft.y,
            std::max(drawSize.x, 0.0001f),
            std::max(drawSize.y, 0.0001f)
        };
        renderParameters[7] = {
            static_cast<float>(graphics.UIWidth()),
            static_cast<float>(graphics.UIHeight()),
            m_texture
                ? static_cast<float>(m_texture->width)
                : 1.0f,
            m_texture
                ? static_cast<float>(m_texture->height)
                : 1.0f
        };
        return graphics.BeginSprites(
            m_shaderPath,
            renderParameters,
            &m_shaderGeneration,
            &m_shaderError);
    }

    void SpriteRendererComponent::OnRender2D(
        DirectX::SpriteBatch& spriteBatch,
        ID3D11ShaderResourceView* whiteTexture)
    {
        using namespace DirectX;

        XMFLOAT4X4 world{};
        XMStoreFloat4x4(&world, Owner().WorldMatrix());

        const bool usesUIRect =
            Owner().GetComponent<UIRectTransformComponent>() != nullptr;
        XMFLOAT2 position{ world._41, world._42 };
        if (!usesUIRect && m_graphics != nullptr)
        {
            const auto& offset = m_graphics->Sprite2DOffset();
            position.x += offset.x;
            position.y += offset.y;
        }
        const float worldScaleX = std::sqrt(world._11 * world._11 + world._12 * world._12);
        const float worldScaleY = std::sqrt(world._21 * world._21 + world._22 * world._22);
        // Cameraが描いたレンダーテクスチャがあれば、通常の
        // テクスチャより優先して表示します。
        ID3D11ShaderResourceView* renderTextureView{};
        float renderTextureWidth{};
        float renderTextureHeight{};
        if (!m_renderTexture.empty()
            && m_graphics != nullptr)
        {
            if (const auto* target =
                    m_graphics->FindRenderTexture(
                        m_renderTexture);
                target != nullptr
                && target->IsValid())
            {
                renderTextureView =
                    target->DisplayShaderResourceView();
                renderTextureWidth =
                    static_cast<float>(target->Width());
                renderTextureHeight =
                    static_cast<float>(target->Height());
            }
        }
        const float textureWidth =
            renderTextureView != nullptr
                ? renderTextureWidth
                : (m_texture ? static_cast<float>(m_texture->width) : 1.0f);
        const float textureHeight =
            renderTextureView != nullptr
                ? renderTextureHeight
                : (m_texture ? static_cast<float>(m_texture->height) : 1.0f);
        XMFLOAT2 drawSize = m_size;
        float rotation = std::atan2(world._12, world._11);
        if (const auto* rectTransform =
            Owner().GetComponent<
                UIRectTransformComponent>();
            rectTransform != nullptr
            && m_graphics != nullptr)
        {
            const auto rect =
                rectTransform->Resolve(
                    static_cast<float>(
                        m_graphics->UIWidth()),
                    static_cast<float>(
                        m_graphics->UIHeight()));
            const auto rectSize = rect.Size();
            position = {
                rect.minimum.x + rectSize.x * 0.5f,
                rect.minimum.y + rectSize.y * 0.5f };
            drawSize = rect.Size();
            // UI矩形は従来どおり左上基準で配置しつつ、所有GameObjectの
            // Z回転も反映します。以下でOriginを中央に固定し、回転中も
            // 表示位置がずれないようにします。
        }
        // ソース矩形（アトラス/スプライトシートの1コマ）指定時は
        // ピクセルRECTへ変換し、表示サイズの基準もコマの大きさに
        // します。
        const bool hasSourceRect =
            m_texture != nullptr
            && (m_sourceRect.x != 0.0f
                || m_sourceRect.y != 0.0f
                || m_sourceRect.z != 1.0f
                || m_sourceRect.w != 1.0f);
        RECT source{};
        if (hasSourceRect)
        {
            source.left = static_cast<LONG>(
                std::lround(
                    m_sourceRect.x * textureWidth));
            source.top = static_cast<LONG>(
                std::lround(
                    m_sourceRect.y * textureHeight));
            source.right = static_cast<LONG>(
                std::lround(
                    (m_sourceRect.x + m_sourceRect.z)
                    * textureWidth));
            source.bottom = static_cast<LONG>(
                std::lround(
                    (m_sourceRect.y + m_sourceRect.w)
                    * textureHeight));
        }
        const float sourceWidth = hasSourceRect
            ? std::max(
                static_cast<float>(
                    source.right - source.left),
                1.0f)
            : textureWidth;
        const float sourceHeight = hasSourceRect
            ? std::max(
                static_cast<float>(
                    source.bottom - source.top),
                1.0f)
            : textureHeight;

        const XMFLOAT2 scale{
            (drawSize.x / sourceWidth) * worldScaleX,
            (drawSize.y / sourceHeight) * worldScaleY
        };
        const XMFLOAT4 premultipliedColor{
            m_color.x * m_color.w,
            m_color.y * m_color.w,
            m_color.z * m_color.w,
            m_color.w
        };

        // SpriteBatchのoriginは「元画像のピクセル」で指定します
        // （scaleが元画像→表示サイズの倍率なので、割合を元画像の
        // 大きさへ掛け直します）。
        //
        // UIはRect Transformが位置を決めるため中心固定です。
        // ワールド空間のスプライトはPivotに従い、既定の{0,0}なら
        // 従来どおり左上が基準になります。
        const XMFLOAT2 pivot =
            Owner().GetComponent<UIRectTransformComponent>()
                    != nullptr
                ? XMFLOAT2{ 0.5f, 0.5f }
                : m_pivot;
        const XMFLOAT2 origin{
            sourceWidth * pivot.x,
            sourceHeight * pivot.y
        };

        spriteBatch.Draw(
            renderTextureView != nullptr
                ? renderTextureView
                : (m_texture
                    ? m_texture->view.Get()
                    : whiteTexture),
            position,
            hasSourceRect ? &source : nullptr,
            XMLoadFloat4(&premultipliedColor),
            rotation,
            origin,
            scale);
    }
}
