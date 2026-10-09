#include "LamaPon/Components/SpriteRendererComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/FrameDebugDescription.h"
#include "LamaPon/Components/SpriteMeshDeformer.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <utility>

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
        // 読み込みに成功した置換画像
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

    SpriteRenderPass
        SpriteRendererComponent::BeginRenderPass(
            GraphicsDevice& graphics)
    {
        // 予約情報を反映する描画定数
        auto renderParameters = m_customParameters;
        // 所有物体のワールド変換
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        // 表示の基準位置ピクセル
        DirectX::XMFLOAT2 position{
            world._41,
            world._42
        };
        // 描画に使う幅・高さ
        DirectX::XMFLOAT2 drawSize = m_size;
        // 描画に使うXY基準点比率
        DirectX::XMFLOAT2 pivot = m_pivot;
        // 所有物体のUI矩形設定
        if (const auto* rectTransform =
                Owner().GetComponent<
                    UIRectTransformComponent>();
            rectTransform != nullptr)
        {
            // UI表示矩形ピクセル
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
            // ワールド2Dの画面補正量
            const auto& offset = graphics.Sprite2DOffset();
            position.x += offset.x;
            position.y += offset.y;
            // ワールドX軸のXY投影長
            const float worldScaleX = std::sqrt(
                world._11 * world._11
                + world._12 * world._12);
            // ワールドY軸のXY投影長
            const float worldScaleY = std::sqrt(
                world._21 * world._21
                + world._22 * world._22);
            drawSize.x *= worldScaleX;
            drawSize.y *= worldScaleY;
        }

        // UV・色を補完する予約定数5・6・7番へ描画色・矩形・画面と元画像の寸法を入れます。
        // 回転前の描画左上位置
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
        // 独自パスのシェーダーと定数
        SpritePassDescription description;
        description.pixelShader = m_shaderPath;
        description.customParameters = renderParameters;
        // 開始したスプライト描画パス
        auto pass = graphics.BeginSpritePass(description);
        // 描画パスのシェーダー状態
        const auto status = pass.ShaderStatus();
        m_shaderGeneration = status.generation;
        m_shaderError = status.error;
        return pass;
    }

    void SpriteRendererComponent::OnRender2D(
        const SpriteDrawContext& sprites)
    {
        using namespace DirectX;

        // 所有物体のワールド変換
        XMFLOAT4X4 world{};
        XMStoreFloat4x4(&world, Owner().WorldMatrix());

        // 所有物体がUI矩形を持つ指定
        const bool usesUIRect =
            Owner().GetComponent<UIRectTransformComponent>() != nullptr;
        // 表示の基準位置ピクセル
        XMFLOAT2 position{ world._41, world._42 };
        if (!usesUIRect && m_graphics != nullptr)
        {
            // ワールド2Dの画面補正量
            const auto& offset = m_graphics->Sprite2DOffset();
            position.x += offset.x;
            position.y += offset.y;
        }
        // ワールドX軸のXY投影長
        const float worldScaleX = std::sqrt(world._11 * world._11 + world._12 * world._12);
        // ワールドY軸のXY投影長
        const float worldScaleY = std::sqrt(world._21 * world._21 + world._22 * world._22);
        // Cameraが描いたレンダーテクスチャがあれば、通常のテクスチャより優先して表示します。
        // カメラ出力画像の保持ビュー
        GraphicsViewHandle renderTextureView;
        // カメラ出力画像の幅ピクセル
        float renderTextureWidth{};
        // カメラ出力画像の高ピクセル
        float renderTextureHeight{};
        if (!m_renderTexture.empty()
            && m_graphics != nullptr)
        {
            // 名前に対応するカメラ出力
            if (const auto* target =
                    m_graphics->FindRenderTexture(
                        m_renderTexture);
                target != nullptr
                && target->IsValid())
            {
                // 取得したカメラ出力のビュー
                auto view = m_graphics->RenderTextureViewHandle(
                    m_renderTexture);
                if (view)
                {
                    renderTextureView = std::move(view);
                    renderTextureWidth =
                        static_cast<float>(target->Width());
                    renderTextureHeight =
                        static_cast<float>(target->Height());
                }
            }
        }
        // 選んだ元画像の幅ピクセル
        const float textureWidth =
            renderTextureView
                ? renderTextureWidth
                : (m_texture ? static_cast<float>(m_texture->width) : 1.0f);
        // 選んだ元画像の高ピクセル
        const float textureHeight =
            renderTextureView
                ? renderTextureHeight
                : (m_texture ? static_cast<float>(m_texture->height) : 1.0f);
        // 描画に使う幅・高さ
        XMFLOAT2 drawSize = m_size;
        // Z回転角ラジアン
        float rotation = std::atan2(world._12, world._11);
        // 所有物体のUI矩形設定
        if (const auto* rectTransform =
            Owner().GetComponent<
                UIRectTransformComponent>();
            rectTransform != nullptr
            && m_graphics != nullptr)
        {
            // UI表示矩形ピクセル
            const auto rect =
                rectTransform->Resolve(
                    static_cast<float>(
                        m_graphics->UIWidth()),
                    static_cast<float>(
                        m_graphics->UIHeight()));
            // UI表示幅・高さピクセル
            const auto rectSize = rect.Size();
            position = {
                rect.minimum.x + rectSize.x * 0.5f,
                rect.minimum.y + rectSize.y * 0.5f };
            drawSize = rect.Size();

        }
        // 正規化領域を元画像のピクセル矩形へ換算します。
        // 通常画像の部分領域を使う指定
        const bool hasSourceRect =
            m_texture != nullptr
            && (m_sourceRect.x != 0.0f
                || m_sourceRect.y != 0.0f
                || m_sourceRect.z != 1.0f
                || m_sourceRect.w != 1.0f);
        // 元画像のピクセル部分矩形
        SpriteSourceRectangle source{};
        if (hasSourceRect)
        {
            source.left = static_cast<std::int32_t>(
                std::lround(
                    m_sourceRect.x * textureWidth));
            source.top = static_cast<std::int32_t>(
                std::lround(
                    m_sourceRect.y * textureHeight));
            source.right = static_cast<std::int32_t>(
                std::lround(
                    (m_sourceRect.x + m_sourceRect.z)
                    * textureWidth));
            source.bottom = static_cast<std::int32_t>(
                std::lround(
                    (m_sourceRect.y + m_sourceRect.w)
                    * textureHeight));
        }
        // 選んだ領域の幅ピクセル
        const float sourceWidth = hasSourceRect
            ? std::max(
                static_cast<float>(
                    source.right - source.left),
                1.0f)
            : textureWidth;
        // 選んだ領域の高ピクセル
        const float sourceHeight = hasSourceRect
            ? std::max(
                static_cast<float>(
                    source.bottom - source.top),
                1.0f)
            : textureHeight;

        // 元画像から表示へのXY倍率
        const XMFLOAT2 scale{
            (drawSize.x / sourceWidth) * worldScaleX,
            (drawSize.y / sourceHeight) * worldScaleY
        };
        // アルファ乗算済みの描画色
        const XMFLOAT4 premultipliedColor{
            m_color.x * m_color.w,
            m_color.y * m_color.w,
            m_color.z * m_color.w,
            m_color.w
        };

        // 基準点比率を元画像のピクセルへ換算し、UIは矩形中心を使います。
        // 描画に使うXY基準点比率
        const XMFLOAT2 pivot =
            Owner().GetComponent<UIRectTransformComponent>()
                    != nullptr
                ? XMFLOAT2{ 0.5f, 0.5f }
                : m_pivot;
        // 元画像の基準位置ピクセル
        const XMFLOAT2 origin{
            sourceWidth * pivot.x,
            sourceHeight * pivot.y
        };

        // 通常画像の保持ビュー
        GraphicsViewHandle textureView;
        if (m_texture)
        {
            // 通常画像のGPU資源の保持
            const auto resources = m_texture->resources.Acquire();
            if (resources)
            {
                textureView = resources->shaderResourceView;
            }
        }

        if (UsesMesh())
        {
            // ワールド2Dの画面補正量
            const XMFLOAT2 offset = m_graphics != nullptr
                ? m_graphics->Sprite2DOffset()
                : XMFLOAT2{};
            // 通常画像だけ部分領域を使い、カメラ出力と単色は全体を使います。
            // 格子へ割り当てる正規化UV矩形
            const XMFLOAT4 uvRect =
                !renderTextureView && m_texture != nullptr
                    ? m_sourceRect
                    : XMFLOAT4{ 0.0f, 0.0f, 1.0f, 1.0f };
            // 描画する格子頂点のローカル位置
            const auto localPositions = DeformedMeshPositions();
            m_meshVertices.resize(localPositions.size());
            // 格子の頂点番号
            for (std::size_t index = 0;
                index < localPositions.size();
                ++index)
            {
                // 格子内の列番号
                const auto column =
                    static_cast<int>(index)
                    % (m_meshColumns + 1);
                // 格子内の行番号
                const auto row =
                    static_cast<int>(index)
                    / (m_meshColumns + 1);
                // 頂点のローカル位置
                const auto& local = localPositions[index];
                m_meshVertices[index] = {
                    {
                        local.x * world._11
                            + local.y * world._21
                            + world._41
                            + offset.x,
                        local.x * world._12
                            + local.y * world._22
                            + world._42
                            + offset.y
                    },
                    {
                        uvRect.x
                            + uvRect.z
                                * static_cast<float>(column)
                                / static_cast<float>(m_meshColumns),
                        uvRect.y
                            + uvRect.w
                                * static_cast<float>(row)
                                / static_cast<float>(m_meshRows)
                    },
                    premultipliedColor
                };
            }
            if (m_meshIndices.empty())
            {
                m_meshIndices.reserve(
                    static_cast<std::size_t>(
                        m_meshColumns * m_meshRows * 6));
                // 三角形を作る格子の行
                for (int row = 0; row < m_meshRows; ++row)
                {
                    // 三角形を作る格子の列
                    for (int column = 0;
                        column < m_meshColumns;
                        ++column)
                    {
                        // 区画の左上の頂点番号
                        const auto topLeft =
                            static_cast<std::uint16_t>(
                                row * (m_meshColumns + 1) + column);
                        // 区画の左下の頂点番号
                        const auto bottomLeft =
                            static_cast<std::uint16_t>(
                                topLeft + m_meshColumns + 1);
                        m_meshIndices.insert(
                            m_meshIndices.end(),
                            {
                                topLeft,
                                static_cast<std::uint16_t>(topLeft + 1),
                                bottomLeft,
                                bottomLeft,
                                static_cast<std::uint16_t>(topLeft + 1),
                                static_cast<std::uint16_t>(bottomLeft + 1)
                            });
                    }
                }
            }
            // 画像と格子頂点の描画要求
            SpriteMeshDrawRequest meshRequest;
            meshRequest.texture = renderTextureView
                ? renderTextureView
                : textureView;
            meshRequest.vertices = m_meshVertices;
            meshRequest.indices = m_meshIndices;
            static_cast<void>(sprites.DrawMesh(meshRequest));
            return;
        }

        // 画像・姿勢・色の描画要求
        SpriteDrawRequest request;
        request.texture = renderTextureView
            ? renderTextureView
            : textureView;
        request.position = position;
        request.hasSourceRectangle = hasSourceRect;
        request.sourceRectangle = source;
        request.tint = premultipliedColor;
        request.rotation = rotation;
        request.origin = origin;
        request.scale = scale;
        static_cast<void>(sprites.Draw(request));
    }

    void SpriteRendererComponent::SetMeshGrid(
        const int columns,
        const int rows)
    {
        // 1〜64へ収めた横の分割数
        const int nextColumns = std::clamp(columns, 1, 64);
        // 1〜64へ収めた縦の分割数
        const int nextRows = std::clamp(rows, 1, 64);
        if (nextColumns == m_meshColumns && nextRows == m_meshRows)
        {
            return;
        }
        m_meshColumns = nextColumns;
        m_meshRows = nextRows;
        m_meshIndices.clear();
        if (!m_meshDeformation.empty()
            && m_meshDeformation.size() != MeshVertexCount())
        {
            m_meshDeformation.clear();
        }
    }

    std::vector<DirectX::XMFLOAT2>
        SpriteRendererComponent::MeshRestPositions() const
    {
        // 上の行から並べる静止頂点
        std::vector<DirectX::XMFLOAT2> positions;
        positions.reserve(MeshVertexCount());
        // 頂点の行番号
        for (int row = 0; row <= m_meshRows; ++row)
        {
            // 頂点の列番号
            for (int column = 0; column <= m_meshColumns; ++column)
            {
                positions.push_back({
                    (static_cast<float>(column)
                            / static_cast<float>(m_meshColumns)
                        - m_pivot.x)
                        * m_size.x,
                    (static_cast<float>(row)
                            / static_cast<float>(m_meshRows)
                        - m_pivot.y)
                        * m_size.y });
            }
        }
        return positions;
    }

    bool SpriteRendererComponent::SetMeshDeformation(
        std::vector<DirectX::XMFLOAT2> positions)
    {
        // 格子と同数で全座標が有限か
        const bool valid =
            positions.size() == MeshVertexCount()
            && std::all_of(
                positions.begin(),
                positions.end(),
                // 有限の座標か判定します(position: 確認する頂点)。
                [](const DirectX::XMFLOAT2& position)
                {
                    return std::isfinite(position.x)
                        && std::isfinite(position.y);
                });
        if (!valid)
        {
            m_meshDeformation.clear();
            return positions.empty();
        }
        m_meshDeformation = std::move(positions);
        return true;
    }

    bool SpriteRendererComponent::UsesMesh() const
    {
        if (Owner().GetComponent<UIRectTransformComponent>() != nullptr)
        {
            return false;
        }
        if (m_meshColumns * m_meshRows > 1
            || !m_meshDeformation.empty())
        {
            return true;
        }
        // 変形部品を探す同じGameObjectの部品
        for (const auto& component : Owner().Components())
        {
            // 頂点を変形する部品
            const auto* deformer =
                dynamic_cast<const SpriteMeshDeformer*>(
                    component.get());
            if (deformer != nullptr
                && deformer->IsEnabled()
                && deformer->DeformsSpriteMesh())
            {
                return true;
            }
        }
        return false;
    }

    std::vector<DirectX::XMFLOAT2>
        SpriteRendererComponent::DeformedMeshPositions(
            const SpriteMeshDeformer* const skipped) const
    {
        // 変形を重ねる格子頂点
        auto positions = m_meshDeformation.empty()
            ? MeshRestPositions()
            : m_meshDeformation;
        // 変形部品を探す同じGameObjectの部品
        for (const auto& component : Owner().Components())
        {
            // 頂点を変形する部品
            auto* const deformer =
                dynamic_cast<SpriteMeshDeformer*>(component.get());
            if (deformer == nullptr
                || deformer == skipped
                || !deformer->IsEnabled())
            {
                continue;
            }
            // 変形前の頂点数
            const auto count = positions.size();
            deformer->DeformSpriteMesh(*this, positions);
            // 変形後の全頂点が有限か
            const bool finite = std::all_of(
                positions.begin(),
                positions.end(),
                // 有限の座標か判定します(position: 確認する頂点)。
                [](const DirectX::XMFLOAT2& position)
                {
                    return std::isfinite(position.x)
                        && std::isfinite(position.y);
                });
            if (positions.size() != count || !finite)
            {
                return m_meshDeformation.empty()
                    ? MeshRestPositions()
                    : m_meshDeformation;
            }
        }
        return positions;
    }

    bool SpriteRendererComponent::DescribeDrawEvent(
        FrameDebugDrawDescription& description) const
    {
        return Detail::DescribeTexturedDraw(
            description,
            std::string(UsesMesh() ? "スプライトメッシュ " : "スプライト ")
                + Detail::FrameDebugNumber(Size().x) + "x"
                + Detail::FrameDebugNumber(Size().y),
            TexturePath(),
            RenderSortOrder());
    }
}
