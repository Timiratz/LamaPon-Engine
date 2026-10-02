#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/ComputeEffect.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/D3D11RenderTargetState.h"
#include "LamaPon/Graphics/D3D12ComputeEffectRenderer.h"
#include "LamaPon/Graphics/D3D12RenderServices.h"
#include "LamaPon/Graphics/D3D12SpriteRenderer.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Resources.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D12Resources.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Access.h"
#include "LamaPon/Graphics/GraphicsDeviceShaderState.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/MaterialShaderDrawRequest.h"
#include "LamaPon/Graphics/LitTextureRequest.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ScreenEffect.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/ShaderDiagnostics.h"
#include "LamaPon/Graphics/ShaderManifest.h"
#include "LamaPon/Graphics/ShaderVariants.h"
#include "LamaPon/Graphics/SpriteEffect.h"

#include <CommonStates.h>
#include <SpriteBatch.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    // 素材定義を検証して実HLSLのパスを返します(assets: 定義の取得元, definitionPath: 公開識別用の定義パス, sourcePath: 実HLSLパスの出力, error: 失敗理由の出力)。
    [[nodiscard]] bool ResolveMaterialShaderSource(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& definitionPath,
        std::filesystem::path& sourcePath,
        std::string& error)
    {
        sourcePath = definitionPath;
        error.clear();
        if (!LamaPon::IsShaderManifestPath(definitionPath))
        {
            return true;
        }

        // 参照HLSLと用途を持つ素材定義
        LamaPon::ShaderAssetDesc description;
        if (!LamaPon::LoadShaderAssetDesc(
                assets,
                definitionPath,
                description,
                error))
        {
            return false;
        }
        if (description.type != LamaPon::ShaderAssetType::Material)
        {
            error = "Material shader requires a shader manifest whose"
                " type is 'material': "
                + LamaPon::PathToUtf8(definitionPath);
            return false;
        }
        sourcePath = assets.ResolvePath(description.source)
            .lexically_normal();
        return true;
    }

    // 元HLSLを読めれば原因別の説明を補います(assets: ソースの取得元, shaderPath: 元HLSLのパス, compilerMessage: 空を許す診断, usage: シェーダーの用途)。
    [[nodiscard]] std::string DescribeShaderFailure(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& shaderPath,
        const char* compilerMessage,
        const LamaPon::ShaderUsage usage)
    {
        // 診断を補う元HLSLの本文
        std::string source;
        try
        {
            // 診断用のHLSLバイト列
            const auto bytes =
                assets.ReadFileBytes(shaderPath);
            source.assign(
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size());
        }
        catch (const std::exception&)
        {
            // ソースを読めなくてもコンパイラーの診断から説明を作ります。
        }
        return LamaPon::ExplainShaderError(
            compilerMessage != nullptr ? compilerMessage : "",
            source,
            usage);
    }

    // 宣言外のキーワードを除いて素材ソースを作ります(graphics: 資源と宣言の取得元, shaderPath: 素材HLSLのパス, keywords: 選択するキーワード)。
    [[nodiscard]] LamaPon::Detail::MaterialShaderSource
        MakeMaterialShaderSource(
            const LamaPon::GraphicsDevice& graphics,
            const std::filesystem::path& shaderPath,
            const LamaPon::ShaderKeywordSet& keywords)
    {
        // 正規化したパスと素材キーワード
        LamaPon::Detail::MaterialShaderSource source;
        source.path =
            graphics.Assets().ResolvePath(shaderPath).lexically_normal();
        // 宣言外を除いたキーワード集合
        const auto normalized = LamaPon::NormalizeKeywords(
            graphics.ShaderVariantsFor(source.path),
            keywords);
        // キーワード集合の整列済みキー
        const auto variantKey = normalized.Key();
        source.cacheKey = variantKey.empty()
            ? source.path
            : std::filesystem::path(
                source.path.wstring()
                + L"?"
                + LamaPon::Utf8ToWide(variantKey));
        source.keywords = normalized.Keywords();
        // コールバックが借用する取得元
        auto* const assets = &graphics.Assets();
        // 素材の診断を説明します(message: 元の診断, path: 固定する素材パス)。
        source.describeFailure =
            [assets, path = source.path](const char* const message)
            {
                return DescribeShaderFailure(
                    *assets,
                    path,
                    message,
                    LamaPon::ShaderUsage::Material);
            };
        return source;
    }

    // D3D12素材サービスを借用し、未対応ならnullptrです(resources: 空を許すAPI資源)。
    [[nodiscard]] LamaPon::Detail::D3D12MaterialShaderServices*
        TryD3D12MaterialShaderServices(
            LamaPon::Detail::GraphicsDeviceApiResources* const resources)
                noexcept
    {
        return resources != nullptr
            ? dynamic_cast<LamaPon::Detail::D3D12MaterialShaderServices*>(
                resources->TryRenderServices())
            : nullptr;
    }

    // XYZの全成分が有限か返します(value: 確認するベクトル)。
    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT3& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z);
    }

    // 事前畳み込みキューブの世代と形式・寸法を検証します(graphics: 解決元の描画機器, handle: 確認するビュー, expectedSize: 必要な一辺の画素数, expectedMipLevels: 必要なミップ数, resolved: 借用SRVの出力)。
    [[nodiscard]] bool TryResolvePrefilteredCube(
        const LamaPon::GraphicsDevice& graphics,
        const LamaPon::GraphicsViewHandle& handle,
        const std::uint32_t expectedSize,
        const std::uint32_t expectedMipLevels,
        ID3D11ShaderResourceView*& resolved) noexcept
    {
        resolved = LamaPon::Detail::GraphicsDeviceD3D11Access::
            TryResolveD3D11ShaderResourceView(graphics, handle);
        if (!handle || resolved == nullptr)
        {
            return false;
        }

        // キューブSRVの形式と範囲
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        resolved->GetDesc(&viewDescription);
        if (viewDescription.Format
                != DXGI_FORMAT_R16G16B16A16_FLOAT
            || viewDescription.ViewDimension
                != D3D11_SRV_DIMENSION_TEXTURECUBE
            || viewDescription.TextureCube.MostDetailedMip != 0
            || viewDescription.TextureCube.MipLevels
                != expectedMipLevels)
        {
            return false;
        }

        // ビューが参照する所有資源
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        resolved->GetResource(resource.ReleaseAndGetAddressOf());
        // ビューが参照する2D画像
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (resource == nullptr || FAILED(resource.As(&texture)))
        {
            return false;
        }
        // 参照キューブ画像の形式と寸法
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        return description.Width == expectedSize
            && description.Height == expectedSize
            && description.MipLevels == expectedMipLevels
            && description.ArraySize == 6
            && description.Format
                == DXGI_FORMAT_R16G16B16A16_FLOAT
            && description.SampleDesc.Count == 1
            && (description.BindFlags
                & D3D11_BIND_SHADER_RESOURCE) != 0
            && (description.MiscFlags
                & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
    }

    // D3D11描画先の実体を借用し、別API・未設定ならnullptrです(target: 調べる描画先)。
    [[nodiscard]] LamaPon::Detail::D3D11RenderTargetState*
        TryD3D11RenderTargetState(
            LamaPon::RenderTarget& target) noexcept
    {
        return dynamic_cast<LamaPon::Detail::D3D11RenderTargetState*>(
            LamaPon::Detail::RenderTargetBackendAccess::Get(target));
    }

}

namespace LamaPon
{
    DirectX::SpriteBatch& GraphicsDevice::BeginSprites(
        const std::filesystem::path& shaderPath,
        const std::array<DirectX::XMFLOAT4, 8>&
            customParameters,
        std::uint64_t* generation,
        std::string* error,
        const Sprite2DLighting* lighting)
    {
        // スプライトの素材・定数・照明
        SpritePassDescription description;
        description.pixelShader = shaderPath;
        description.customParameters = customParameters;
        description.lighting = lighting != nullptr
            ? *lighting
            : Sprite2DLighting{};
        static_cast<void>(BeginD3D11SpritePass(
            description,
            false,
            nullptr,
            generation,
            error));
        return *RequireD3D11ApiResources().spriteBatch;
    }

    std::function<void()> GraphicsDevice::PrepareD3D11SpriteShader(
        const SpritePassDescription& description,
        SpriteShaderStatus& status)
    {
        status = {};
        if (description.pixelShader.empty())
        {
            return {};
        }

        // 正規化したシェーダー絶対パス
        const auto absolutePath = Assets().ResolvePath(
            description.pixelShader).lexically_normal();
        // 用途とキーワード別のキャッシュ
        auto& entry = RequireD3D11ApiResources().spriteShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<SpriteShaderEntry>();
        }

        // 再確認間隔を判定する現在時刻
        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            // 資源アーカイブを使用中か
            const bool archived = Assets().IsArchived();
            // 保存時刻の取得エラー
            std::error_code fileError;
            // シェーダー元ファイルの有無
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            // 元ファイルの保存時刻
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            // 参照元HLSLと依存先の変更番号
            const auto dependencyRevision =
                ShaderSourceDependencyRevision(
                    Assets(),
                    absolutePath);
            // 再読み込みが必要か
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime)
                || (entry->observed
                    && entry->dependencyRevision
                        != dependencyRevision);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
                entry->dependencyRevision = dependencyRevision;
                if (!sourceExists)
                {
                    entry->error =
                        "Sprite shader file was not found: "
                        + PathToUtf8(absolutePath);
                    entry->effect.reset();
                }
                else
                {
                    try
                    {
                        // 生成成功後に公開する新しい効果
                        auto candidate =
                            std::make_shared<SpriteEffect>(
                                Device(),
                                Context(),
                                Assets(),
                                absolutePath);
                        entry->effect = std::move(candidate);
                        entry->generation =
                            ++m_state->m_spriteShaderGeneration;
                        entry->error.clear();
                    }
                    // 効果の再作成で生じた診断
                    catch (const std::exception& exception)
                    {
                        entry->error = DescribeShaderFailure(
                            Assets(),
                            absolutePath,
                            exception.what(),
                            ShaderUsage::Sprite);
                        // 再コンパイル失敗時は旧効果を外して代替表示へ切り替えます。
                        entry->effect.reset();
                    }
                }
            }
        }

        status.generation = entry->generation;
        status.error = entry->error;
        if (!entry->effect)
        {
            // コンパイル失敗を視認できるよう、マゼンタの代替表示を使います。
            if (!entry->error.empty())
            {
                // 借用する失敗時の代替効果
                if (auto* const placeholder =
                        SpriteErrorPlaceholder())
                {
                    status.fallback =
                        SpriteShaderFallback::ErrorPlaceholder;
                    // マゼンタ表示へ定数を適用します(parameters: 保存したパス定数)。
                    return [
                        placeholder,
                        parameters = description.customParameters]()
                    {
                        placeholder->SetParameters(parameters);
                        placeholder->SetLights(Sprite2DLighting{});
                        placeholder->Apply();
                    };
                }
            }
            status.fallback =
                SpriteShaderFallback::DefaultPipeline;
            return {};
        }

        // Flush時に使う効果とこのパスの値を保持し、Begin～End間の他の描画や再読み込みから独立させます。
        // 描画完了まで保持する効果の世代
        auto effect = entry->effect;
        // 保存した効果へ値を適用します(parameters: パス定数, lighting: パスの照明)。
        return [
            effect = std::move(effect),
            parameters = description.customParameters,
            lighting = description.lighting]()
        {
            effect->SetParameters(parameters);
            effect->SetLights(lighting);
            effect->Apply();
        };
    }

    bool GraphicsDevice::ApplyCustomPixelShader(
        const std::filesystem::path& shaderPath,
        const std::array<
            DirectX::XMFLOAT4,
            8>& customParameters,
        std::uint64_t* generation,
        std::string* error) const
    {
        if (generation != nullptr)
        {
            *generation = 0;
        }
        if (error != nullptr)
        {
            error->clear();
        }
        if (shaderPath.empty())
        {
            return false;
        }

        // 正規化したシェーダー絶対パス
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // 用途とキーワード別のキャッシュ
        auto& entry = RequireD3D11ApiResources().spriteShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<SpriteShaderEntry>();
        }

        // 再確認間隔を判定する現在時刻
        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            // 資源アーカイブを使用中か
            const bool archived = Assets().IsArchived();
            // 保存時刻の取得エラー
            std::error_code fileError;
            // シェーダー元ファイルの有無
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            // 元ファイルの保存時刻
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            // 参照元HLSLと依存先の変更番号
            const auto dependencyRevision =
                ShaderSourceDependencyRevision(
                    Assets(),
                    absolutePath);
            // 再読み込みが必要か
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime)
                || (entry->observed
                    && entry->dependencyRevision
                        != dependencyRevision);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
                entry->dependencyRevision = dependencyRevision;
                if (!sourceExists)
                {
                    entry->error =
                        "Custom pixel shader file was not found: "
                        + PathToUtf8(absolutePath);
                    entry->effect.reset();
                }
                else
                {
                    try
                    {
                        // 生成成功後に公開する新しい効果
                        auto candidate =
                            std::make_shared<SpriteEffect>(
                                Device(),
                                Context(),
                                Assets(),
                                absolutePath);
                        entry->effect = std::move(candidate);
                        entry->generation =
                            ++m_state->m_spriteShaderGeneration;
                        entry->error.clear();
                    }
                    // 効果の再作成で生じた診断
                    catch (const std::exception& exception)
                    {
                        entry->error = DescribeShaderFailure(
                            Assets(),
                            absolutePath,
                            exception.what(),
                            ShaderUsage::Sprite);
                        // 再コンパイル失敗時は旧効果を外します。
                        entry->effect.reset();
                    }
                }
            }
        }

        if (generation != nullptr)
        {
            *generation = entry->generation;
        }
        if (error != nullptr)
        {
            *error = entry->error;
        }
        if (!entry->effect)
        {

            if (!entry->error.empty())
            {
                // 借用する失敗時の代替効果
                if (auto* const placeholder =
                        SpriteErrorPlaceholder())
                {
                    placeholder->SetParameters(
                        customParameters);
                    placeholder->Apply();
                    return true;
                }
            }
            return false;
        }

        entry->effect->SetParameters(customParameters);
        entry->effect->Apply();
        return true;
    }

    void GraphicsDevice::InvalidateCustomPixelShader(
        const std::filesystem::path& shaderPath) const
    {
        InvalidateSpriteShader(shaderPath);
    }

    void GraphicsDevice::ApplyQueuedScreenEffects(
        RenderTarget& target,
        const ScreenEffectPoint point)
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // D3D12のAPI資源
            auto* const resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            // 借用するD3D12画面効果描画器
            auto* const renderer = resources != nullptr
                ? resources->TrySpriteRenderer()
                : nullptr;
            // 対象地点の登録があるか調べます(queued: 確認する登録済み効果)。
            if (renderer == nullptr
                || std::ranges::none_of(
                    resources->queuedScreenEffects,
                    [point](const ScreenEffectRequest& queued)
                    {
                        return queued.point == point;
                    }))
            {
                return;
            }

            // 登録地点ごとに深度を読み取り用へ確定し、無効ならその地点の効果を除去して描画を省きます。
            m_state->m_backend->CaptureOffscreenTargetDepth(target);
            // 対象の深度ビューが現役か
            const bool hasDepth = IsGraphicsViewCurrent(
                target.DepthViewHandle());

            // 深度画像を描いた射影行列
            const auto& projection = SceneProjection();
            // 距離再構成用の射影Z成分
            const DirectX::XMFLOAT4 depthParameters{
                projection._33,
                projection._43,
                1.0f,
                0.0f
            };
            // 位置再構成用の射影XYの逆数
            const DirectX::XMFLOAT4 depthUnprojection{
                1.0f / (std::abs(projection._11) > 1e-6f
                    ? projection._11
                    : 1.0f),
                1.0f / (std::abs(projection._22) > 1e-6f
                    ? projection._22
                    : 1.0f),
                0.0f,
                0.0f
            };
            // 対象地点の登録済み画面効果
            for (const auto& queued : resources->queuedScreenEffects)
            {
                if (queued.point != point || !hasDepth)
                {
                    continue;
                }
                // 補助画像は描画命令の記録完了まで保持し、解決できない入力は白画像へ置換します。
                // 描画まで保持する補助画像の世代
                std::array<std::shared_ptr<
                    const TextureResourceSnapshot>, 2>
                    auxiliaryResources{};
                // 画面効果へ渡す補助画像ビュー
                std::array<GraphicsViewHandle, 2> auxiliaryViews{};
                // 補助画像の枠番号
                for (std::size_t index{};
                    index < queued.auxiliaryTextures.size();
                    ++index)
                {
                    if (queued.auxiliaryTextures[index].empty())
                    {
                        continue;
                    }
                    // 読み込んだ補助画像アセット
                    const auto texture = Assets().LoadTexture(
                        queued.auxiliaryTextures[index]);
                    auxiliaryResources[index] = texture != nullptr
                        ? texture->resources.Acquire()
                        : nullptr;
                    if (auxiliaryResources[index] != nullptr
                        && IsGraphicsViewCurrent(
                            auxiliaryResources[index]
                                ->shaderResourceView))
                    {
                        auxiliaryViews[index] =
                            auxiliaryResources[index]->shaderResourceView;
                    }
                }
                renderer->ApplyScreenEffect(
                    target,
                    m_state->m_whiteTextureView,
                    Assets(),
                    queued.shader,
                    auxiliaryViews,
                    queued.customParameters,
                    depthParameters,
                    depthUnprojection);
            }
            // 対象地点の登録を除去します(queued: 除去対象か調べる効果)。
            std::erase_if(
                resources->queuedScreenEffects,
                [point](const ScreenEffectRequest& queued)
                {
                    return queued.point == point;
                });
            return;
        }

        // 対象地点の登録があるか調べます(queued: 確認する登録済み効果)。
        if (std::ranges::none_of(
                RequireD3D11ApiResources().queuedScreenEffects,
                [point](const QueuedScreenEffect& queued)
                {
                    return queued.point == point;
                }))
        {
            return;
        }
        // D3D11描画先の実体
        auto* const targetState = TryD3D11RenderTargetState(target);
        // 色と深度が現役の描画先か
        const bool targetStateIsCurrent = targetState != nullptr
            && targetState->IsValid()
            && IsGraphicsViewCurrent(targetState->m_currentColorView)
            && IsGraphicsViewCurrent(targetState->m_depthView);
        // 深度と一致するジッター付き射影を使い、距離をy/(深度+x)で再構成します。
        // 深度画像を描いた射影行列
        const auto& projection = SceneProjection();
        // 距離再構成用の射影Z成分
        const DirectX::XMFLOAT4 depthParameters{
            projection._33,
            projection._43,
            1.0f,
            0.0f
        };

        // 位置再構成用の射影XYの逆数
        const DirectX::XMFLOAT4 depthUnprojection{
            1.0f / (std::abs(projection._11) > 1e-6f
                ? projection._11
                : 1.0f),
            1.0f / (std::abs(projection._22) > 1e-6f
                ? projection._22
                : 1.0f),
            0.0f,
            0.0f
        };
        // 代替用の白画像ハンドル
        const auto whiteTextureView = WhiteTextureViewHandle();
        // 借用する代替用の白画像SRV
        auto* const whiteTexture =
            TryResolveD3D11ShaderResourceView(whiteTextureView);
        // 対象地点の登録済み画面効果
        for (const auto& queued : RequireD3D11ApiResources().queuedScreenEffects)
        {
            if (queued.effect == nullptr
                || queued.point != point)
            {
                continue;
            }
            if (!targetStateIsCurrent)
            {
                continue;
            }
            // 描画まで保持する補助画像の世代
            std::array<std::shared_ptr<
                const TextureResourceSnapshot>, 2>
                auxiliaryResources{};
            std::array<ID3D11ShaderResourceView*, 2>
                auxiliaryViews{};
            // 補助画像の枠番号
            for (std::size_t index = 0;
                index < auxiliaryViews.size();
                ++index)
            {
                // 補助画像のアセット参照
                const auto& asset =
                    queued.auxiliaryTextures[index];
                auxiliaryResources[index] = asset != nullptr
                    ? asset->resources.Acquire()
                    : nullptr;
                // 借用する現役の補助画像SRV
                auto* const resolved =
                    auxiliaryResources[index] != nullptr
                    ? TryResolveD3D11ShaderResourceView(
                        *auxiliaryResources[index])
                    : nullptr;
                auxiliaryViews[index] = resolved != nullptr
                    ? resolved
                    : whiteTexture;
            }
            targetState->ApplyScreenEffect(
                *queued.effect,
                auxiliaryViews,
                depthParameters,
                depthUnprojection,
                queued.parameters);
        }
        // 現在地点の登録を除去し、後続地点の登録は残します(queued: 除去対象か調べる効果)。
        std::erase_if(
            RequireD3D11ApiResources().queuedScreenEffects,
            [point](const QueuedScreenEffect& queued)
            {
                return queued.point == point;
            });
    }

    bool GraphicsDevice::QueueScreenEffect(
        const ScreenEffectRequest& request,
        std::uint64_t* generation,
        std::string* error)
    {
        if (generation != nullptr)
        {
            *generation = 0;
        }
        if (error != nullptr)
        {
            error->clear();
        }
        if (request.shader.empty())
        {
            return false;
        }

        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // D3D12のAPI資源
            auto* const resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            // 借用するD3D12画面効果描画器
            auto* const renderer = resources != nullptr
                ? resources->TrySpriteRenderer()
                : nullptr;
            // 画面効果の失敗原因を説明します(message: コンパイラーの診断)。
            const auto describeFailure =
                [this, &request](const char* const message)
                {
                    return DescribeShaderFailure(
                        Assets(),
                        Assets().ResolvePath(request.shader)
                            .lexically_normal(),
                        message,
                        ShaderUsage::ScreenEffect);
                };
            if (renderer == nullptr
                || !renderer->PrepareScreenEffect(
                    Assets(),
                    request.shader,
                    describeFailure,
                    generation,
                    error))
            {
                return false;
            }
            resources->queuedScreenEffects.push_back(request);
            return true;
        }

        // 正規化したシェーダー絶対パス
        const auto absolutePath =
            Assets().ResolvePath(request.shader)
                .lexically_normal();
        // 用途とキーワード別のキャッシュ
        auto& entry = RequireD3D11ApiResources().screenShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<ScreenShaderEntry>();
        }

        // 再確認間隔を判定する現在時刻
        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            // 資源アーカイブを使用中か
            const bool archived = Assets().IsArchived();
            // 保存時刻の取得エラー
            std::error_code fileError;
            // シェーダー元ファイルの有無
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            // 元ファイルの保存時刻
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            // 参照元HLSLと依存先の変更番号
            const auto dependencyRevision =
                ShaderSourceDependencyRevision(
                    Assets(),
                    absolutePath);
            // 再読み込みが必要か
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime)
                || (entry->observed
                    && entry->dependencyRevision
                        != dependencyRevision);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
                entry->dependencyRevision = dependencyRevision;
                if (!sourceExists)
                {
                    entry->error =
                        "Screen effect shader file was not found: "
                        + PathToUtf8(absolutePath);
                }
                else
                {
                    try
                    {
                        // 生成成功後に公開する新しい効果
                        auto candidate =
                            std::make_shared<ScreenEffect>(
                                Device(),
                                Context(),
                                Assets(),
                                absolutePath);
                        entry->effect = std::move(candidate);
                        entry->generation =
                            ++m_state->m_screenShaderGeneration;
                        entry->error.clear();
                    }
                    // 効果の再作成で生じた診断
                    catch (const std::exception& exception)
                    {
                        // 再コンパイルに失敗しても、直前の正常なシェーダーは維持します。
                        entry->error = DescribeShaderFailure(
                            Assets(),
                            absolutePath,
                            exception.what(),
                            ShaderUsage::ScreenEffect);
                    }
                }
            }
        }

        if (generation != nullptr)
        {
            *generation = entry->generation;
        }
        if (error != nullptr)
        {
            *error = entry->error;
        }
        if (!entry->effect)
        {
            return false;
        }

        // 世代と定数を固定した登録効果
        QueuedScreenEffect queued{};
        queued.effect = entry->effect;
        queued.parameters = request.customParameters;
        queued.point = request.point;
        try
        {
            // 読み込む補助画像の枠番号
            for (std::size_t index = 0;
                index < request.auxiliaryTextures.size();
                ++index)
            {
                if (!request.auxiliaryTextures[index].empty())
                {
                    queued.auxiliaryTextures[index] =
                        Assets().LoadTexture(
                            request.auxiliaryTextures[index]);
                }
            }
        }
        // 効果の作成・資源読み込みの診断
        catch (const std::exception& exception)
        {
            if (error != nullptr)
            {
                *error = "Screen effect auxiliary texture could not"
                    " be loaded: " + std::string(exception.what());
            }
            return false;
        }
        catch (...)
        {
            if (error != nullptr)
            {
                *error = "Screen effect auxiliary texture could not"
                    " be loaded.";
            }
            return false;
        }
        RequireD3D11ApiResources().queuedScreenEffects.emplace_back(
            std::move(queued));
        return true;
    }

    bool GraphicsDevice::DispatchComputeEffect(
        const ComputeEffectRequest& request,
        std::string* const error)
    {
        if (error != nullptr)
        {
            error->clear();
        }
        if (request.shader.empty()
            || request.outputTexture.empty()
            || request.outputWidth == 0
            || request.outputHeight == 0)
        {
            if (error != nullptr)
            {
                *error =
                    "A compute effect needs a shader, an"
                    " output texture name and a non-zero"
                    " size.";
            }
            return false;
        }

        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // D3D12のAPI資源
            auto* const resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            // 借用するD3D12計算効果描画器
            auto* const renderer = resources != nullptr
                ? resources->TryComputeEffectRenderer()
                : nullptr;
            if (renderer == nullptr)
            {
                if (error != nullptr)
                {
                    *error =
                        "The DirectX 12 compute effect renderer is not"
                        " available.";
                }
                return false;
            }
            // 計算効果の失敗原因を説明します(message: コンパイラーの診断)。
            const auto describeFailure =
                [this, &request](const char* const message)
                {
                    return DescribeShaderFailure(
                        Assets(),
                        Assets().ResolvePath(request.shader)
                            .lexically_normal(),
                        message,
                        ShaderUsage::Compute);
                };
            if (!renderer->Prepare(
                    Assets(),
                    request.shader,
                    describeFailure,
                    error))
            {
                return false;
            }

            // UAV用途を設定してから画像を作成し、計算書き込み可能な資源を取得します。
            // 名前付きの計算書き込み画像
            auto& target = AcquireComputeTexture(
                request.outputTexture,
                request.outputWidth,
                request.outputHeight);
            if (!IsGraphicsViewCurrent(target.DisplayViewHandle()))
            {
                if (error != nullptr)
                {
                    *error =
                        "The compute output texture could not"
                        " be created for writing.";
                }
                return false;
            }

            // 入力画像は計算命令の記録完了まで保持し、解決できない入力は白画像へ置換します。
            // 計算記録まで保持する入力の世代
            std::array<std::shared_ptr<
                const TextureResourceSnapshot>, 2>
                inputResources{};
            // 計算入力の読み取りビュー列
            std::array<GraphicsViewHandle, 2> inputs{};
            // 代替用の白画像ハンドル
            const auto whiteTextureView = WhiteTextureViewHandle();
            // 計算入力画像の枠番号
            for (std::size_t index = 0;
                index < request.inputTextures.size();
                ++index)
            {
                inputs[index] = whiteTextureView;
                if (request.inputTextures[index].empty())
                {
                    continue;
                }
                // 読み込んだ入力画像アセット
                const auto texture = Assets().LoadTexture(
                    request.inputTextures[index]);
                inputResources[index] = texture != nullptr
                    ? texture->resources.Acquire()
                    : nullptr;
                if (inputResources[index] != nullptr
                    && IsGraphicsViewCurrent(
                        inputResources[index]->shaderResourceView))
                {
                    inputs[index] =
                        inputResources[index]->shaderResourceView;
                }
            }

            // 計算処理のGPU計測区間
            GpuProfiler::SectionScope computeSection{
                m_state->m_gpuProfiler,
                "Compute"
            };
            renderer->Dispatch(
                Assets(),
                request.shader,
                target,
                inputs,
                request.customParameters);
            computeSection.End();
            return true;
        }

        // 正規化したシェーダー絶対パス
        const auto absolutePath =
            Assets().ResolvePath(request.shader)
                .lexically_normal();
        // 用途とキーワード別のキャッシュ
        auto& entry = RequireD3D11ApiResources().computeShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<ComputeShaderEntry>();
        }

        // 保存・依存先変更で再作成し、失敗時は直前の正常版を保持します。
        // 再確認間隔を判定する現在時刻
        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            // 資源アーカイブを使用中か
            const bool archived = Assets().IsArchived();
            // 保存時刻の取得エラー
            std::error_code fileError;
            // シェーダー元ファイルの有無
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            // 元ファイルの保存時刻
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            // 参照元HLSLと依存先の変更番号
            const auto dependencyRevision =
                ShaderSourceDependencyRevision(
                    Assets(),
                    absolutePath);
            // 再読み込みが必要か
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime)
                || (entry->observed
                    && entry->dependencyRevision
                        != dependencyRevision);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
                entry->dependencyRevision = dependencyRevision;
                if (!sourceExists)
                {
                    entry->error =
                        "Compute effect shader file was not"
                        " found: "
                        + PathToUtf8(absolutePath);
                }
                else
                {
                    try
                    {
                        entry->effect =
                            std::make_unique<ComputeEffect>(
                                Device(),
                                Context(),
                                Assets(),
                                absolutePath);
                        entry->error.clear();
                    }
                    // 効果の再作成で生じた診断
                    catch (const std::exception& exception)
                    {
                        entry->error = DescribeShaderFailure(
                            Assets(),
                            absolutePath,
                            exception.what(),
                            ShaderUsage::Compute);
                    }
                }
            }
        }

        if (error != nullptr)
        {
            *error = entry->error;
        }
        if (!entry->effect)
        {
            return false;
        }

        // UAV用途を設定してから画像を作成し、計算書き込み可能な資源を取得します。
        // 名前付きの計算書き込み画像
        auto& target = AcquireComputeTexture(
            request.outputTexture,
            request.outputWidth,
            request.outputHeight);
        // D3D11の書き込み画像の実体
        auto* const targetState = TryD3D11RenderTargetState(target);
        // 借用する計算書き込み先UAV
        auto* const outputView = targetState != nullptr
                && targetState->IsValid()
                && IsGraphicsViewCurrent(targetState->m_displayView)
            ? targetState->m_displayUnorderedAccessView.Get()
            : nullptr;
        if (outputView == nullptr)
        {
            if (error != nullptr)
            {
                *error =
                    "The compute output texture could not"
                    " be created for writing.";
            }
            return false;
        }

        // 計算終了まで保持する入力の世代
        std::array<std::shared_ptr<
            const TextureResourceSnapshot>, 2>
            inputResources{};
        // 計算入力の読み取りビュー列
        std::array<ID3D11ShaderResourceView*, 2> inputs{};
        // 代替用の白画像ハンドル
        const auto whiteTextureView = WhiteTextureViewHandle();
        // 借用する代替用の白画像SRV
        auto* const whiteTexture =
            TryResolveD3D11ShaderResourceView(whiteTextureView);
        // 計算入力画像の枠番号
        for (std::size_t index = 0;
            index < request.inputTextures.size();
            ++index)
        {
            if (request.inputTextures[index].empty())
            {
                inputs[index] = whiteTexture;
                continue;
            }
            // 読み込んだ入力画像アセット
            const auto texture = Assets().LoadTexture(
                request.inputTextures[index]);
            inputResources[index] = texture != nullptr
                ? texture->resources.Acquire()
                : nullptr;
            // 借用する現役の計算入力SRV
            auto* const resolved = inputResources[index] != nullptr
                ? TryResolveD3D11ShaderResourceView(
                    *inputResources[index])
                : nullptr;
            inputs[index] = resolved != nullptr
                ? resolved
                : whiteTexture;
        }

        // 計算処理のGPU計測区間
        GpuProfiler::SectionScope computeSection{
            m_state->m_gpuProfiler,
            "Compute"
        };
        entry->effect->Dispatch(
            inputs,
            outputView,
            targetState->m_width,
            targetState->m_height,
            request.customParameters);
        computeSection.End();
        return true;
    }

    void GraphicsDevice::InvalidateComputeEffectShader(
        const std::filesystem::path& shaderPath) const
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // D3D12のAPI資源
            auto* const d3d12Resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            // 借用する効果の再読込窓口
            auto* const renderer = d3d12Resources != nullptr
                ? d3d12Resources->TryComputeEffectRenderer()
                : nullptr;
            if (renderer != nullptr && TryAssets() != nullptr)
            {
                renderer->Invalidate(Assets(), shaderPath);
            }
            return;
        }
        // 処理するAPIの資源
        auto* const resources = TryD3D11ApiResources();
        if (shaderPath.empty() || !TryAssets() || resources == nullptr)
        {
            return;
        }
        // 正規化したシェーダー絶対パス
        const auto absolutePath =
            Assets().ResolvePath(shaderPath)
                .lexically_normal();
        // 用途別キャッシュの検索結果
        const auto found = resources->computeShaders.find(absolutePath);
        if (found != resources->computeShaders.end()
            && found->second)
        {
            found->second->forceReload = true;
        }
    }

    void GraphicsDevice::InvalidateScreenEffectShader(
        const std::filesystem::path& shaderPath) const
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // 処理するAPIの資源
            auto* const resources = dynamic_cast<
                Detail::GraphicsDeviceD3D12Resources*>(
                    m_state->m_apiResources.get());
            // 借用する効果の再読込窓口
            auto* const renderer = resources != nullptr
                ? resources->TrySpriteRenderer()
                : nullptr;
            if (renderer != nullptr && TryAssets() != nullptr)
            {
                renderer->InvalidateScreenEffect(
                    Assets(),
                    shaderPath);
            }
            return;
        }
        // 処理するAPIの資源
        auto* const resources = TryD3D11ApiResources();
        if (shaderPath.empty() || !TryAssets() || resources == nullptr)
        {
            return;
        }
        // 正規化したシェーダー絶対パス
        const auto absolutePath =
            Assets().ResolvePath(shaderPath)
                .lexically_normal();
        // 用途別キャッシュの検索結果
        const auto found = resources->screenShaders.find(absolutePath);
        if (found != resources->screenShaders.end()
            && found->second)
        {
            found->second->forceReload = true;
        }
    }

    bool GraphicsDevice::IsShaderCompiling(
        const std::filesystem::path& shaderPath,
        const ShaderKeywordSet& keywords) const
    {
        // 処理するAPIの資源
        const auto* const resources = TryD3D11ApiResources();
        if (shaderPath.empty() || resources == nullptr)
        {
            return false;
        }
        // 正規化したシェーダー絶対パス
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // 宣言外を除いたキーワード集合
        const auto normalized = NormalizeKeywords(
            ShaderVariantsFor(absolutePath),
            keywords);
        // キーワード集合の整列済みキー
        const auto variantKey = normalized.Key();
        // パスとキーワード別の識別キー
        const std::filesystem::path cacheKey =
            variantKey.empty()
                ? absolutePath
                : std::filesystem::path(
                    absolutePath.wstring()
                    + L"?"
                    + Utf8ToWide(variantKey));
        // 用途別キャッシュの検索結果
        const auto found = resources->materialShaders.find(cacheKey);
        return found != resources->materialShaders.end()
            && found->second->pending;
    }

    const ShaderVariantDeclaration&
        GraphicsDevice::ShaderVariantsFor(
            const std::filesystem::path& shaderPath) const
    {
        // 空パスに返すキーワード宣言
        static const ShaderVariantDeclaration empty;
        if (shaderPath.empty())
        {
            return empty;
        }
        // 正規化したシェーダー絶対パス
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // 用途別キャッシュの検索結果
        const auto found = m_state->m_shaderVariants.find(absolutePath);
        if (found != m_state->m_shaderVariants.end())
        {
            return found->second;
        }
        // 読み取ったキーワード宣言
        ShaderVariantDeclaration declaration;
        try
        {
            // 素材定義が参照する実HLSL
            std::filesystem::path sourcePath;
            // 素材定義の解決で生じた診断
            std::string manifestError;
            if (ResolveMaterialShaderSource(
                    Assets(),
                    absolutePath,
                    sourcePath,
                    manifestError)
                && Assets().FileExists(sourcePath))
            {
                // 宣言を読み取るHLSLバイト列
                const auto source =
                    Assets().ReadFileBytesFresh(sourcePath);
                declaration = ParseShaderVariants(
                    std::string_view{
                        reinterpret_cast<const char*>(
                            source.data()),
                        source.size() });
            }
        }
        catch (const std::exception&)
        {
            // 読み取り失敗時は宣言なしとして扱い、Inspectorの表示を継続します。
            declaration = {};
        }
        return m_state->m_shaderVariants
            .emplace(absolutePath, std::move(declaration))
            .first->second;
    }

    bool GraphicsDevice::TrySetLitEffectTextures(
        LitEffect& effect,
        const LitTextureRequest& request) const noexcept
    {
        if (!IsInitialized() || effect.m_context == nullptr)
        {
            return false;
        }
        // 効果コンテキストの所有デバイス
        Microsoft::WRL::ComPtr<ID3D11Device> effectDevice;
        effect.m_context->GetDevice(
            effectDevice.ReleaseAndGetAddressOf());
        if (effectDevice.Get() != Device())
        {
            return false;
        }

        // 画像を解決し、空は正常な未指定として扱います(view: 確認するビュー, resolved: 借用SRVの出力)。
        const auto tryResolve = [this](
            const GraphicsViewHandle& view,
            ID3D11ShaderResourceView*& resolved) noexcept
        {
            resolved = TryResolveD3D11ShaderResourceView(view);
            return !view || resolved != nullptr;
        };

        // 借用する基本色のSRV
        ID3D11ShaderResourceView* albedo{};
        // 借用する法線画像のSRV
        ID3D11ShaderResourceView* normal{};
        // 借用するPBR画像と素材設定
        PbrTextures pbrTextures{};
        // 借用する自作素材画像のSRV列
        std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount> customTextures{};
        // 全ての指定画像を解決できたか
        bool valid = tryResolve(request.albedo, albedo)
            && tryResolve(request.normal, normal)
            && tryResolve(
                request.roughness,
                pbrTextures.roughness)
            && tryResolve(
                request.metallic,
                pbrTextures.metallic)
            && tryResolve(
                request.occlusion,
                pbrTextures.occlusion)
            && tryResolve(
                request.emissive,
                pbrTextures.emissive);
        // 自作素材画像の枠番号
        for (std::size_t index{};
            valid && index < customTextures.size();
            ++index)
        {
            valid = tryResolve(
                request.customTextures[index],
                customTextures[index]);
        }
        if (!valid)
        {
            return false;
        }

        pbrTextures.occlusionStrength =
            request.occlusionStrength;
        pbrTextures.emissiveFactor = request.emissiveFactor;
        effect.SetTextures(
            albedo,
            normal,
            pbrTextures);
        effect.SetCustomTextures(customTextures);
        return true;
    }

    bool GraphicsDevice::TrySetLitEffectReflectionProbe(
        LitEffect& effect,
        const ReflectionProbeEnvironment& probe) const noexcept
    {
        if (!IsInitialized() || effect.m_context == nullptr)
        {
            return false;
        }
        // 効果コンテキストの所有デバイス
        Microsoft::WRL::ComPtr<ID3D11Device> effectDevice;
        effect.m_context->GetDevice(
            effectDevice.ReleaseAndGetAddressOf());
        if (effectDevice.Get() != Device())
        {
            return false;
        }

        // 主反射キューブの指定があるか
        const bool hasSpecular = static_cast<bool>(probe.specular);
        // 主放射照度キューブの指定
        const bool hasIrradiance = static_cast<bool>(probe.irradiance);
        if (!hasSpecular && !hasIrradiance)
        {
            // 主プローブの指定がなければ環境IBLを維持し、第2プローブの未使用情報は解決しません。
            return true;
        }
        if (hasSpecular != hasIrradiance)
        {
            return false;
        }

        // 事前畳み込み反射の最終段
        constexpr auto ExpectedMaximumMip = static_cast<float>(
            EnvironmentRenderer::PrefilteredSpecularMipLevels - 1);
        // 検証して借用する反射SRV
        LitEffect::D3D11ReflectionProbeViews nativeViews;
        if (!std::isfinite(probe.intensity)
            || !std::isfinite(probe.specularMaximumMip)
            || probe.specularMaximumMip != ExpectedMaximumMip
            || !std::isfinite(probe.secondaryWeight)
            || !IsFinite(probe.boxCenter)
            || !IsFinite(probe.boxExtents)
            || !TryResolvePrefilteredCube(
                *this,
                probe.specular,
                EnvironmentRenderer::PrefilteredSpecularSize,
                EnvironmentRenderer::PrefilteredSpecularMipLevels,
                nativeViews.specular)
            || !TryResolvePrefilteredCube(
                *this,
                probe.irradiance,
                EnvironmentRenderer::PrefilteredIrradianceSize,
                EnvironmentRenderer::PrefilteredIrradianceMipLevels,
                nativeViews.irradiance))
        {
            return false;
        }

        if (probe.secondaryWeight > 0.0f)
        {
            if (!probe.secondarySpecular
                || !probe.secondaryIrradiance
                || !std::isfinite(
                    probe.secondarySpecularMaximumMip)
                || probe.secondarySpecularMaximumMip
                    != ExpectedMaximumMip
                || !IsFinite(probe.secondaryBoxCenter)
                || !IsFinite(probe.secondaryBoxExtents)
                || !TryResolvePrefilteredCube(
                    *this,
                    probe.secondarySpecular,
                    EnvironmentRenderer::PrefilteredSpecularSize,
                    EnvironmentRenderer::PrefilteredSpecularMipLevels,
                    nativeViews.secondarySpecular)
                || !TryResolvePrefilteredCube(
                    *this,
                    probe.secondaryIrradiance,
                    EnvironmentRenderer::PrefilteredIrradianceSize,
                    EnvironmentRenderer::PrefilteredIrradianceMipLevels,
                    nativeViews.secondaryIrradiance))
            {
                return false;
            }
        }

        effect.SetEnvironmentOverrideD3D11(probe, nativeViews);
        return true;
    }

    bool GraphicsDevice::TrySetLitEffectLighting(
        LitEffect& effect,
        const LightingState& lighting) const noexcept
    {
        if (!IsInitialized() || effect.m_context == nullptr)
        {
            return false;
        }
        // 効果コンテキストの所有デバイス
        Microsoft::WRL::ComPtr<ID3D11Device> effectDevice;
        effect.m_context->GetDevice(
            effectDevice.ReleaseAndGetAddressOf());
        if (effectDevice.Get() != Device())
        {
            return false;
        }

        // 検証済みの借用照明SRV列
        LitEffect::D3D11LightingViews nativeViews;

        // 2D画像の形式・範囲・用途を検証します(handle: 同世代のビュー, expectedFormat: 必要な形式, expectedMipLevels: 必要なミップ数, resolved: 借用SRVの出力, textureDescription: 参照画像設定の出力)。
        const auto tryResolveTexture2D = [this](
            const GraphicsViewHandle& handle,
            const DXGI_FORMAT expectedFormat,
            const std::uint32_t expectedMipLevels,
            ID3D11ShaderResourceView*& resolved,
            D3D11_TEXTURE2D_DESC& textureDescription) noexcept
        {
            resolved = TryResolveD3D11ShaderResourceView(handle);
            if (!handle || resolved == nullptr)
            {
                return false;
            }
            // SRVの形式・種別・読み取り範囲
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            resolved->GetDesc(&viewDescription);
            if (viewDescription.ViewDimension
                    != D3D11_SRV_DIMENSION_TEXTURE2D
                || viewDescription.Format != expectedFormat
                || viewDescription.Texture2D.MostDetailedMip != 0
                || viewDescription.Texture2D.MipLevels
                    != expectedMipLevels)
            {
                return false;
            }
            // SRVが保持する参照元資源
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            resolved->GetResource(resource.ReleaseAndGetAddressOf());
            // 検証する2D画像の所有参照
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (resource == nullptr || FAILED(resource.As(&texture)))
            {
                return false;
            }
            texture->GetDesc(&textureDescription);
            return textureDescription.Format == expectedFormat
                && textureDescription.MipLevels == expectedMipLevels
                && textureDescription.ArraySize == 1
                && textureDescription.SampleDesc.Count == 1
                && (textureDescription.BindFlags
                    & D3D11_BIND_SHADER_RESOURCE) != 0;
        };
        // キューブを検証し、形式指定があれば寸法も照合します(handle: 同世代のビュー, expectedFormat: 未指定を許す形式, expectedSize: 形式指定時の寸法, expectedMipLevels: 形式指定時の段数, resolved: 借用SRVの出力)。
        const auto tryResolveTextureCube = [this](
            const GraphicsViewHandle& handle,
            const DXGI_FORMAT expectedFormat,
            const std::uint32_t expectedSize,
            const std::uint32_t expectedMipLevels,
            ID3D11ShaderResourceView*& resolved) noexcept
        {
            resolved = TryResolveD3D11ShaderResourceView(handle);
            if (!handle || resolved == nullptr)
            {
                return false;
            }
            // SRVの形式・種別・読み取り範囲
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            resolved->GetDesc(&viewDescription);
            if (viewDescription.ViewDimension
                    != D3D11_SRV_DIMENSION_TEXTURECUBE
                || viewDescription.TextureCube.MostDetailedMip != 0
                || viewDescription.TextureCube.MipLevels == 0)
            {
                return false;
            }

            // SRVが保持する参照元資源
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            resolved->GetResource(resource.ReleaseAndGetAddressOf());
            // 検証する2D画像の所有参照
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (resource == nullptr || FAILED(resource.As(&texture)))
            {
                return false;
            }
            // 画像またはSRVのネイティブ設定
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            // SRVの省略を補ったミップ数
            auto viewMipLevels =
                viewDescription.TextureCube.MipLevels;
            if (viewMipLevels == std::numeric_limits<UINT>::max())
            {
                viewMipLevels = description.MipLevels;
            }
            if (description.Width == 0
                || description.Width != description.Height
                || description.ArraySize != 6
                || description.MipLevels == 0
                || viewMipLevels > description.MipLevels
                || description.SampleDesc.Count != 1
                || (description.MiscFlags
                    & D3D11_RESOURCE_MISC_TEXTURECUBE) == 0
                || (description.BindFlags
                    & D3D11_BIND_SHADER_RESOURCE) == 0)
            {
                return false;
            }

            if (expectedFormat != DXGI_FORMAT_UNKNOWN)
            {
                return viewDescription.Format == expectedFormat
                    && description.Format == expectedFormat
                    && description.Width == expectedSize
                    && description.MipLevels == expectedMipLevels
                    && viewMipLevels == expectedMipLevels;
            }

            // サンプル可能な形式の対応状況
            UINT formatSupport{};
            // キューブ読取に必要な形式用途
            constexpr UINT RequiredFormatSupport =
                D3D11_FORMAT_SUPPORT_TEXTURECUBE
                | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
            return viewDescription.Format != DXGI_FORMAT_UNKNOWN
                && (description.BindFlags
                    & D3D11_BIND_DEPTH_STENCIL) == 0
                && SUCCEEDED(Device()->CheckFormatSupport(
                    viewDescription.Format,
                    &formatSupport))
                && (formatSupport & RequiredFormatSupport)
                    == RequiredFormatSupport;
        };
        // 寸法の逆数を検証して整数の画素数へ戻します(inverseDimension: 正の有限な寸法の逆数, dimension: 成功時の画素数出力)。
        const auto tryRecoverDimension = [](
            const float inverseDimension,
            std::uint32_t& dimension) noexcept
        {
            if (!std::isfinite(inverseDimension)
                || !(inverseDimension > 0.0f))
            {
                return false;
            }
            // 寸法の逆数から復元した実数
            const auto exactDimension =
                1.0 / static_cast<double>(inverseDimension);
            if (!std::isfinite(exactDimension)
                || exactDimension < 1.0
                || exactDimension
                    > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            {
                return false;
            }
            // 整数へ丸めた画像寸法
            const auto roundedDimension = std::round(exactDimension);
            if (std::abs(
                    inverseDimension * roundedDimension - 1.0)
                    > 0.0001)
            {
                return false;
            }
            dimension = static_cast<std::uint32_t>(roundedDimension);
            return true;
        };

        // 環境キューブによる照明設定
        const auto& environment = lighting.environment;
        if (environment.enabled)
        {
            if (!std::isfinite(environment.intensity)
                || !tryResolveTextureCube(
                    environment.texture,
                    DXGI_FORMAT_UNKNOWN,
                    0,
                    0,
                    nativeViews.environment[0]))
            {
                return false;
            }

            // 事前畳み込み反射の指定があるか
            const bool hasSpecular =
                static_cast<bool>(environment.specular);
            // 放射照度の指定があるか
            const bool hasIrradiance =
                static_cast<bool>(environment.irradiance);
            if (hasSpecular != hasIrradiance)
            {
                return false;
            }
            if (hasSpecular)
            {
                // 事前畳み込み反射の最終段
                constexpr auto ExpectedMaximumMip = static_cast<float>(
                    EnvironmentRenderer::PrefilteredSpecularMipLevels - 1);
                if (!tryResolveTextureCube(
                        environment.specular,
                        DXGI_FORMAT_R16G16B16A16_FLOAT,
                        EnvironmentRenderer::PrefilteredSpecularSize,
                        EnvironmentRenderer::PrefilteredSpecularMipLevels,
                        nativeViews.environment[1])
                    || !tryResolveTextureCube(
                        environment.irradiance,
                        DXGI_FORMAT_R16G16B16A16_FLOAT,
                        EnvironmentRenderer::PrefilteredIrradianceSize,
                        EnvironmentRenderer::PrefilteredIrradianceMipLevels,
                        nativeViews.environment[2])
                    || !std::isfinite(
                        environment.specularMaximumMip)
                    || environment.specularMaximumMip
                        != ExpectedMaximumMip)
                {
                    return false;
                }
            }
        }

        // 影のビューと深度画像の形式・寸法を検証します(handle: 同世代のビュー, cube: キューブ指定, minimumSlices: 必要な最小面数, maximumSlices: 許可する最大面数, expectedResolution: 整数に近い一辺の寸法, resolved: 借用SRVの出力)。
        const auto tryResolveShadow = [this](
            const GraphicsViewHandle& handle,
            const bool cube,
            const std::uint32_t minimumSlices,
            const std::uint32_t maximumSlices,
            const float expectedResolution,
            ID3D11ShaderResourceView*& resolved) noexcept
        {
            if (!std::isfinite(expectedResolution)
                || expectedResolution < 1.0f
                || expectedResolution
                    > static_cast<float>(
                        D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION))
            {
                return false;
            }
            // 整数へ丸めた影の解像度
            const auto roundedResolution =
                std::round(expectedResolution);
            if (std::abs(
                    static_cast<double>(expectedResolution)
                        - roundedResolution) > 0.0001)
            {
                return false;
            }

            resolved = TryResolveD3D11ShaderResourceView(handle);
            if (!handle || resolved == nullptr)
            {
                return false;
            }
            // SRVの形式・種別・読み取り範囲
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            resolved->GetDesc(&viewDescription);
            if (viewDescription.Format != DXGI_FORMAT_R32_FLOAT)
            {
                return false;
            }
            if (cube)
            {
                if (viewDescription.ViewDimension
                        != D3D11_SRV_DIMENSION_TEXTURECUBE
                    || viewDescription.TextureCube.MostDetailedMip != 0
                    || viewDescription.TextureCube.MipLevels != 1)
                {
                    return false;
                }
            }
            else if (viewDescription.ViewDimension
                    != D3D11_SRV_DIMENSION_TEXTURE2DARRAY
                || viewDescription.Texture2DArray.MostDetailedMip != 0
                || viewDescription.Texture2DArray.MipLevels != 1
                || viewDescription.Texture2DArray.FirstArraySlice != 0
                || viewDescription.Texture2DArray.ArraySize
                    < minimumSlices
                || viewDescription.Texture2DArray.ArraySize
                    > maximumSlices)
            {
                return false;
            }

            // SRVが保持する参照元資源
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            resolved->GetResource(resource.ReleaseAndGetAddressOf());
            // 検証する2D画像の所有参照
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (resource == nullptr || FAILED(resource.As(&texture)))
            {
                return false;
            }
            // 画像またはSRVのネイティブ設定
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            // 検証済みの影の整数解像度
            const auto resolution =
                static_cast<std::uint32_t>(roundedResolution);
            // 画像がキューブ用途を持つか
            const bool isCube = (description.MiscFlags
                & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
            return description.Width == resolution
                && description.Height == resolution
                && description.MipLevels == 1
                && description.ArraySize >= minimumSlices
                && description.ArraySize <= maximumSlices
                && (cube
                    || description.ArraySize
                        == viewDescription.Texture2DArray.ArraySize)
                && description.Format == DXGI_FORMAT_R32_TYPELESS
                && description.SampleDesc.Count == 1
                && (description.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0
                && (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0
                && isCube == cube;
        };

        // 平行光の影設定
        const auto& directionalShadow = lighting.directionalShadow;
        if (directionalShadow.enabled)
        {
            // 定数上限内の平行光の数
            const auto directionalLightCount = std::min(
                lighting.directionalLightCount,
                MaximumDirectionalLights);
            if (directionalShadow.cascadeCount == 0
                || directionalShadow.cascadeCount
                    > MaximumShadowCascades
                || directionalShadow.lightIndex
                    >= directionalLightCount
                || !tryResolveShadow(
                    directionalShadow.texture,
                    false,
                    static_cast<std::uint32_t>(
                        directionalShadow.cascadeCount),
                    static_cast<std::uint32_t>(
                        MaximumShadowCascades),
                    lighting.directionalShadowResolution,
                    nativeViews.directionalShadow))
            {
                return false;
            }
        }

        // 有効なスポット影があるか
        bool hasSpotShadow{};
        // 定数上限内のスポット光の数
        const auto spotLightCount = std::min(
            lighting.spotLightCount,
            MaximumSpotLights);
        // 検証するスポット影の設定
        for (const auto& spotShadow : lighting.spotShadows)
        {
            if (!spotShadow.enabled)
            {
                continue;
            }
            if (spotShadow.lightIndex < 0
                || static_cast<std::size_t>(spotShadow.lightIndex)
                    >= spotLightCount)
            {
                return false;
            }
            hasSpotShadow = true;
        }
        if (hasSpotShadow
            && !tryResolveShadow(
                lighting.spotShadowTexture,
                false,
                static_cast<std::uint32_t>(MaximumSpotShadows),
                static_cast<std::uint32_t>(MaximumSpotShadows),
                lighting.localShadowResolution,
                nativeViews.spotShadow))
        {
            return false;
        }

        // 点光源の影設定
        const auto& pointShadow = lighting.pointShadow;
        if (pointShadow.enabled)
        {
            // 定数上限内の点光源の数
            const auto pointLightCount = std::min(
                lighting.pointLightCount,
                MaximumPointLights);
            if (pointShadow.lightIndex < 0
                || static_cast<std::size_t>(pointShadow.lightIndex)
                    >= pointLightCount
                || !tryResolveShadow(
                    pointShadow.texture,
                    true,
                    6,
                    6,
                    lighting.localShadowResolution,
                    nativeViews.pointShadow))
            {
                return false;
            }
        }

        // 画面空間AOの設定
        const auto& screenOcclusion =
            lighting.screenAmbientOcclusion;
        if (screenOcclusion.enabled)
        {
            if (!std::isfinite(screenOcclusion.inverseWidth)
                || !std::isfinite(screenOcclusion.inverseHeight)
                || !(screenOcclusion.inverseWidth > 0.0f)
                || !(screenOcclusion.inverseHeight > 0.0f))
            {
                return false;
            }
            // 画像またはSRVのネイティブ設定
            D3D11_TEXTURE2D_DESC description{};
            // 定数から復元した画面幅
            std::uint32_t targetWidth{};
            // 定数から復元した画面高
            std::uint32_t targetHeight{};
            if (!tryResolveTexture2D(
                    screenOcclusion.texture,
                    DXGI_FORMAT_R8_UNORM,
                    1,
                    nativeViews.screenAmbientOcclusion,
                    description)
                || !tryRecoverDimension(
                    screenOcclusion.inverseWidth,
                    targetWidth)
                || !tryRecoverDimension(
                    screenOcclusion.inverseHeight,
                    targetHeight)
                || description.Width
                    != std::max(targetWidth / 2u, 1u)
                || description.Height
                    != std::max(targetHeight / 2u, 1u))
            {
                return false;
            }
        }

        // 画面空間反射の設定
        const auto& screenReflection =
            lighting.screenSpaceReflection;
        if (screenReflection.enabled)
        {
            if (!std::isfinite(screenReflection.inverseWidth)
                || !std::isfinite(screenReflection.inverseHeight)
                || !(screenReflection.inverseWidth > 0.0f)
                || !(screenReflection.inverseHeight > 0.0f)
                || screenReflection.depthPyramidMaximumMip
                    >= D3D11_REQ_MIP_LEVELS)
            {
                return false;
            }
            // SSRのHDR履歴画像設定
            D3D11_TEXTURE2D_DESC colorDescription{};
            // SSRの深度ピラミッド設定
            D3D11_TEXTURE2D_DESC depthDescription{};
            // 定数から復元した画面幅
            std::uint32_t targetWidth{};
            // 定数から復元した画面高
            std::uint32_t targetHeight{};
            // SSRで要求する深度のミップ数
            const auto depthMipLevels =
                screenReflection.depthPyramidMaximumMip + 1;
            if (!tryResolveTexture2D(
                    screenReflection.texture,
                    DXGI_FORMAT_R16G16B16A16_FLOAT,
                    1,
                    nativeViews.screenSpaceReflection[0],
                    colorDescription)
                || !tryResolveTexture2D(
                    screenReflection.depth,
                    DXGI_FORMAT_R32_FLOAT,
                    depthMipLevels,
                    nativeViews.screenSpaceReflection[1],
                    depthDescription)
                || colorDescription.Width != depthDescription.Width
                || colorDescription.Height != depthDescription.Height
                || !tryRecoverDimension(
                    screenReflection.inverseWidth,
                    targetWidth)
                || !tryRecoverDimension(
                    screenReflection.inverseHeight,
                    targetHeight)
                || colorDescription.Width != targetWidth
                || colorDescription.Height != targetHeight)
            {
                return false;
            }
            // 画面の最大辺から求める段数
            std::uint32_t fullMipLevels{ 1 };
            // 段数計算で縮小する最大辺
            for (auto maximumDimension =
                    std::max(targetWidth, targetHeight);
                maximumDimension > 1;
                maximumDimension >>= 1)
            {
                ++fullMipLevels;
            }
            if (depthMipLevels != fullMipLevels)
            {
                return false;
            }
        }

        // Forward+のライトと格子設定
        const auto& clustered = lighting.clustered;
        if (clustered.enabled)
        {
            if (clustered.lightCount == 0
                || clustered.lightCount > MaximumClusteredLights
                || !std::isfinite(clustered.nearPlane)
                || !std::isfinite(clustered.farPlane)
                || !std::isfinite(clustered.inverseWidth)
                || !std::isfinite(clustered.inverseHeight)
                || !(clustered.nearPlane > 0.0f)
                || !(clustered.farPlane > clustered.nearPlane)
                || !(clustered.inverseWidth > 0.0f)
                || !(clustered.inverseHeight > 0.0f))
            {
                return false;
            }

            // 検証する3本の資源ハンドル
            const std::array<const GraphicsViewHandle*, 3> handles{
                &clustered.lights,
                &clustered.lightIndices,
                &clustered.clusterCounts
            };
            // 各構造化バッファの要素間隔
            const std::array<std::uint32_t, 3> expectedStrides{
                static_cast<std::uint32_t>(sizeof(GpuLight)),
                static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                static_cast<std::uint32_t>(sizeof(std::uint32_t))
            };
            // 各バッファに必要な要素数
            const std::array<std::uint32_t, 3> expectedElements{
                static_cast<std::uint32_t>(MaximumClusteredLights),
                ClusteredLights::ClusterCount
                    * ClusteredLights::MaximumLightsPerCluster,
                ClusteredLights::ClusterCount
            };
            // クラスタ資源またはGI色の番号
            for (std::size_t index{};
                index < handles.size();
                ++index)
            {
                // 検証する同世代のビュー
                const auto& handle = *handles[index];
                // 借用する検証対象のSRV
                auto* const nativeView =
                    TryResolveD3D11ShaderResourceView(handle);
                if (!handle || nativeView == nullptr)
                {
                    return false;
                }

                // SRVの形式・種別・読み取り範囲
                D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
                nativeView->GetDesc(&viewDescription);
                if (viewDescription.ViewDimension
                        != D3D11_SRV_DIMENSION_BUFFER
                    || viewDescription.Format != DXGI_FORMAT_UNKNOWN
                    || viewDescription.Buffer.FirstElement != 0
                    || viewDescription.Buffer.NumElements
                        != expectedElements[index])
                {
                    return false;
                }

                // SRVが保持する参照元資源
                Microsoft::WRL::ComPtr<ID3D11Resource> resource;
                nativeView->GetResource(
                    resource.ReleaseAndGetAddressOf());
                // 検証する構造化バッファ
                Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
                if (resource == nullptr
                    || FAILED(resource.As(&buffer)))
                {
                    return false;
                }
                // 構造化バッファの容量と間隔
                D3D11_BUFFER_DESC bufferDescription{};
                buffer->GetDesc(&bufferDescription);
                // 必要な要素数と間隔の積
                const auto requiredBytes =
                    static_cast<std::uint64_t>(expectedElements[index])
                    * expectedStrides[index];
                if ((bufferDescription.BindFlags
                        & D3D11_BIND_SHADER_RESOURCE) == 0
                    || (bufferDescription.MiscFlags
                        & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) == 0
                    || bufferDescription.StructureByteStride
                        != expectedStrides[index]
                    || bufferDescription.ByteWidth < requiredBytes)
                {
                    return false;
                }
                nativeViews.clustered[index] = nativeView;
            }
        }

        // ベイクしたGI体積の設定
        const auto& bakedGi = lighting.bakedGlobalIllumination;
        if (bakedGi.enabled)
        {
            // 第1成分に合わせる体積寸法
            D3D11_TEXTURE3D_DESC expectedVolume{};
            // 検証する3本の資源ハンドル
            const std::array<const GraphicsViewHandle*, 3> handles{
                &bakedGi.redCoefficients,
                &bakedGi.greenCoefficients,
                &bakedGi.blueCoefficients
            };
            // クラスタ資源またはGI色の番号
            for (std::size_t index{};
                index < handles.size();
                ++index)
            {
                // 検証する同世代のビュー
                const auto& handle = *handles[index];
                // 借用する検証対象のSRV
                auto* const nativeView =
                    TryResolveD3D11ShaderResourceView(handle);
                if (!handle || nativeView == nullptr)
                {
                    return false;
                }
                // 画像またはSRVのネイティブ設定
                D3D11_SHADER_RESOURCE_VIEW_DESC description{};
                nativeView->GetDesc(&description);
                if (description.ViewDimension
                        != D3D11_SRV_DIMENSION_TEXTURE3D
                    || description.Format
                        != DXGI_FORMAT_R16G16B16A16_FLOAT
                    || description.Texture3D.MostDetailedMip != 0
                    || description.Texture3D.MipLevels != 1)
                {
                    return false;
                }

                // SRVが保持する参照元資源
                Microsoft::WRL::ComPtr<ID3D11Resource> resource;
                nativeView->GetResource(
                    resource.ReleaseAndGetAddressOf());
                // 検証するGIの3D画像
                Microsoft::WRL::ComPtr<ID3D11Texture3D> volume;
                if (resource == nullptr
                    || FAILED(resource.As(&volume)))
                {
                    return false;
                }
                // GI体積画像の形式と寸法
                D3D11_TEXTURE3D_DESC volumeDescription{};
                volume->GetDesc(&volumeDescription);
                if (volumeDescription.Format
                        != DXGI_FORMAT_R16G16B16A16_FLOAT
                    || (index != 0
                        && (volumeDescription.Width
                                != expectedVolume.Width
                            || volumeDescription.Height
                                != expectedVolume.Height
                            || volumeDescription.Depth
                                != expectedVolume.Depth)))
                {
                    return false;
                }
                if (index == 0)
                {
                    expectedVolume = volumeDescription;
                }
                nativeViews.bakedGlobalIllumination[index] =
                    nativeView;
            }
            if (bakedGi.resolution.x
                    != static_cast<float>(expectedVolume.Width)
                || bakedGi.resolution.y
                    != static_cast<float>(expectedVolume.Height)
                || bakedGi.resolution.z
                    != static_cast<float>(expectedVolume.Depth))
            {
                return false;
            }
        }

        effect.SetLightingD3D11(lighting, nativeViews);
        return true;
    }

    LitEffect& GraphicsDevice::MaterialShader(
        const std::filesystem::path& shaderPath,
        std::uint64_t& generation,
        std::string& error,
        const ShaderKeywordSet& keywords) const
    {
        generation = 0;
        error.clear();
        if (shaderPath.empty())
        {
            return Lit();
        }

        // 正規化したシェーダー絶対パス
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();

        // 宣言外を除いたキーワード集合
        const auto normalized = NormalizeKeywords(
            ShaderVariantsFor(absolutePath),
            keywords);

        // キーワード集合の整列済みキー
        const auto variantKey = normalized.Key();
        // パスとキーワード別の識別キー
        const std::filesystem::path cacheKey =
            variantKey.empty()
                ? absolutePath
                : std::filesystem::path(
                    absolutePath.wstring()
                    + L"?"
                    + Utf8ToWide(variantKey));
        // 用途とキーワード別のキャッシュ
        auto& entry = RequireD3D11ApiResources().materialShaders[cacheKey];
        if (!entry)
        {
            entry = std::make_unique<MaterialShaderEntry>();
            entry->keywords = normalized.Keywords();
        }


        // 成功した効果、失敗時の代替効果、標準Litの順で返します。
        const auto resolve = [this, &entry]() -> LitEffect&
        {
            if (entry->effect)
            {
                return *entry->effect;
            }
            if (!entry->error.empty())
            {
                // 借用するマゼンタの代替効果
                if (auto* const placeholder =
                        ShaderErrorPlaceholder(false))
                {
                    return *placeholder;
                }
            }
            return Lit();
        };

        // 非同期のキャッシュ準備完了後、呼び出しスレッドでGPU効果を作成します。
        if (entry->pending)
        {
            if (entry->warming.valid()
                && entry->warming.wait_for(
                        std::chrono::seconds(0))
                    == std::future_status::ready)
            {
                entry->warming.get();
                entry->pending = false;
                try
                {
                    entry->effect =
                        std::make_unique<LitEffect>(
                            Device(),
                            Context(),
                            Assets(),
                            absolutePath,
                            false,
                            entry->keywords);
                    entry->generation =
                        ++m_state->m_materialShaderGeneration;
                    entry->error.clear();
                }
                catch (const std::exception& exception)
                {
                    entry->error = DescribeShaderFailure(
                        Assets(),
                        absolutePath,
                        exception.what(),
                        ShaderUsage::Material);
                }
            }
            else
            {
                // 非同期コンパイルの完了までは標準Litで描画を継続します。
                generation = entry->generation;
                error.clear();
                return Lit();
            }
        }

        // 再確認間隔を判定する現在時刻
        const auto now = std::chrono::steady_clock::now();
        if (entry->observed
            && !entry->forceReload
            && now < entry->nextCheck)
        {
            generation = entry->generation;
            error = entry->error;
            return resolve();
        }
        entry->nextCheck = now + std::chrono::milliseconds(250);

        // アーカイブは保存時刻を調べず、存在状態・依存先の変化または無効化要求で再読み込みします。
        // 資源アーカイブを使用中か
        const bool archived = Assets().IsArchived();
        // 保存時刻の取得エラー
        std::error_code fileError;
        // シェーダー元ファイルの有無
        const bool sourceExists = Assets().FileExists(absolutePath);
        // 元ファイルの保存時刻
        const auto writeTime = (sourceExists && !archived)
            ? std::filesystem::last_write_time(
                absolutePath,
                fileError)
            : std::filesystem::file_time_type{};
        // 依存先を確認する実HLSLのパス
        std::filesystem::path dependencyPath;
        // 素材定義の解決失敗の診断
        std::string dependencyPathError;
        if (!ResolveMaterialShaderSource(
                Assets(),
                absolutePath,
                dependencyPath,
                dependencyPathError))
        {
            dependencyPath = absolutePath;
        }
        // 参照元HLSLと依存先の変更番号
        const auto dependencyRevision =
            ShaderSourceDependencyRevision(
                Assets(),
                dependencyPath,
                entry->keywords);
        // 再読み込みが必要か
        const bool changed = !entry->observed
            || entry->forceReload
            || entry->sourceExists != sourceExists
            || (sourceExists
                && !archived
                && entry->writeTime != writeTime)
            || (entry->observed
                && entry->dependencyRevision
                    != dependencyRevision);
        if (changed)
        {
            entry->observed = true;
            entry->forceReload = false;
            entry->sourceExists = sourceExists;
            entry->writeTime = writeTime;
            entry->dependencyRevision = dependencyRevision;
            if (!sourceExists)
            {
                entry->error =
                    "Shader file was not found: "
                    + PathToUtf8(absolutePath);
                entry->effect.reset();
            }
            else
            {
                try
                {
                    // アーカイブの読み取りはスレッド安全でないため同期処理を使います。
                    if (m_state->m_asyncShaderCompilation
                        && !Assets().IsArchived())
                    {

                        // 非同期処理が借用する取得元
                        auto* const assets = &Assets();
                        // 非同期処理に固定する素材パス
                        const auto path = absolutePath;
                        // 非同期処理に固定するキーワード
                        const auto keywordList = entry->keywords;
                        entry->effect.reset();
                        entry->pending = true;
                        entry->error.clear();
                        // パスとキーワードを固定してCPU側のコンパイルキャッシュを準備します。
                        entry->warming = std::async(
                            std::launch::async,
                            [assets, path, keywordList]
                            {
                                WarmShaderCache(
                                    *assets,
                                    path,
                                    keywordList);
                            });
                        return Lit();
                    }
                    // 生成成功後に公開する新しい効果
                    auto candidate = std::make_unique<LitEffect>(
                        Device(),
                        Context(),
                        Assets(),
                        absolutePath,
                        false,
                        entry->keywords);
                    entry->effect = std::move(candidate);
                    entry->generation =
                        ++m_state->m_materialShaderGeneration;
                    entry->error.clear();
                }
                catch (const std::exception& exception)
                {
                    entry->error = DescribeShaderFailure(
                        Assets(),
                        absolutePath,
                        exception.what(),
                        ShaderUsage::Material);
                    // 再コンパイル失敗時は旧効果を外して代替表示へ切り替えます。
                    entry->effect.reset();
                }
            }
        }

        generation = entry->generation;
        error = entry->error;
        return resolve();
    }

    LitEffect* GraphicsDevice::SkinnedMaterialShader(
        const std::filesystem::path& shaderPath,
        std::uint64_t& generation,
        std::string& error,
        const ShaderKeywordSet& keywords) const
    {
        generation = 0;
        error.clear();
        if (shaderPath.empty())
        {
            return nullptr;
        }

        // 正規化したシェーダー絶対パス
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();

        // 宣言外を除いたキーワード集合
        const auto normalized = NormalizeKeywords(
            ShaderVariantsFor(absolutePath),
            keywords);
        // キーワード集合の整列済みキー
        const auto variantKey = normalized.Key();
        // パスとキーワード別の識別キー
        const std::filesystem::path cacheKey =
            variantKey.empty()
                ? absolutePath
                : std::filesystem::path(
                    absolutePath.wstring()
                    + L"?"
                    + Utf8ToWide(variantKey));
        // 用途とキーワード別のキャッシュ
        auto& entry = RequireD3D11ApiResources().skinnedMaterialShaders[cacheKey];
        if (!entry)
        {
            entry = std::make_unique<MaterialShaderEntry>();
            entry->keywords = normalized.Keywords();
        }


        // 成功した骨変形効果か失敗時の代替効果を返し、未設定はnullptrです。
        const auto resolve = [this, &entry]() -> LitEffect*
        {
            if (entry->effect)
            {
                return entry->effect.get();
            }
            if (!entry->error.empty())
            {
                return ShaderErrorPlaceholder(true);
            }
            return nullptr;
        };

        // 再確認間隔を判定する現在時刻
        const auto now = std::chrono::steady_clock::now();
        if (entry->observed
            && !entry->forceReload
            && now < entry->nextCheck)
        {
            generation = entry->generation;
            error = entry->error;
            return resolve();
        }
        entry->nextCheck = now + std::chrono::milliseconds(250);

        // アーカイブは保存時刻を調べず、存在状態・依存先の変化または無効化要求で再読み込みします。
        // 資源アーカイブを使用中か
        const bool archived = Assets().IsArchived();
        // 保存時刻の取得エラー
        std::error_code fileError;
        // シェーダー元ファイルの有無
        const bool sourceExists = Assets().FileExists(absolutePath);
        // 元ファイルの保存時刻
        const auto writeTime = (sourceExists && !archived)
            ? std::filesystem::last_write_time(
                absolutePath,
                fileError)
            : std::filesystem::file_time_type{};
        // 依存先を確認する実HLSLのパス
        std::filesystem::path dependencyPath;
        // 素材定義の解決失敗の診断
        std::string dependencyPathError;
        if (!ResolveMaterialShaderSource(
                Assets(),
                absolutePath,
                dependencyPath,
                dependencyPathError))
        {
            dependencyPath = absolutePath;
        }
        // 参照元HLSLと依存先の変更番号
        const auto dependencyRevision =
            ShaderSourceDependencyRevision(
                Assets(),
                dependencyPath,
                entry->keywords);
        // 再読み込みが必要か
        const bool changed = !entry->observed
            || entry->forceReload
            || entry->sourceExists != sourceExists
            || (sourceExists
                && !archived
                && entry->writeTime != writeTime)
            || (entry->observed
                && entry->dependencyRevision
                    != dependencyRevision);
        if (changed)
        {
            entry->observed = true;
            entry->forceReload = false;
            entry->sourceExists = sourceExists;
            entry->writeTime = writeTime;
            entry->dependencyRevision = dependencyRevision;
            if (!sourceExists)
            {
                entry->error =
                    "Shader file was not found: "
                    + PathToUtf8(absolutePath);
                entry->effect.reset();
            }
            else
            {
                try
                {
                    // 生成成功後に公開する新しい効果
                    auto candidate = std::make_unique<LitEffect>(
                        Device(),
                        Context(),
                        Assets(),
                        absolutePath,
                        true,
                        entry->keywords);
                    entry->effect = std::move(candidate);
                    entry->generation =
                        ++m_state->m_materialShaderGeneration;
                    entry->error.clear();
                }
                catch (const std::exception& exception)
                {
                    entry->error = DescribeShaderFailure(
                        Assets(),
                        absolutePath,
                        exception.what(),
                        ShaderUsage::Material);
                    // 再コンパイル失敗時は旧効果を外します。
                    entry->effect.reset();
                }
            }
        }

        generation = entry->generation;
        error = entry->error;
        return resolve();
    }

    bool GraphicsDevice::DrawMaterialShaderPrimitive(
        const PrimitiveDrawRequest& request,
        const Detail::MaterialShaderDrawRequest& material,
        std::uint64_t& generation,
        std::string& error)
    {
        generation = 0;
        error.clear();
        // 素材定数とシェーダー指定元を分けられ、指定元がなければ同じ素材を使います。
        // シェーダーとキーワードの指定元
        const auto* const shaderMaterial = material.shaderMaterial != nullptr
            ? material.shaderMaterial
            : material.material;
        if (material.material == nullptr
            || shaderMaterial == nullptr
            || shaderMaterial->Shader().empty()
            || ActiveRenderingApi()
                != RenderingApi::DirectX12Experimental)
        {
            return false;
        }
        // 借用するD3D12素材サービス
        auto* const services = TryD3D12MaterialShaderServices(
            m_state->m_apiResources.get());
        if (services == nullptr)
        {
            return false;
        }

        // キーワード正規化済みの素材指定
        const auto shader = MakeMaterialShaderSource(
            *this,
            shaderMaterial->Shader(),
            shaderMaterial->ShaderKeywords());
        // プロジェクトに代替表示のHLSLがなければエンジン同梱版を使います。
        // 代替表示の標準アセットパス
        constexpr const char* placeholderRelativePath =
            "shaders/LamaPonShaderError.hlsl";
        // 代替表示に使うHLSLの実パス
        auto placeholderPath = Assets().ResolvePath(placeholderRelativePath);
        if (!Assets().FileExists(placeholderPath))
        {
            placeholderPath =
                ExecutableDirectory() / "assets" / placeholderRelativePath;
        }
        // 代替表示のソース指定
        Detail::MaterialShaderSource placeholder;
        placeholder.path = placeholderPath.lexically_normal();
        placeholder.cacheKey = placeholder.path;
        // 描画成否と代替表示・診断
        const auto result = services->DrawMaterialShader(
            Assets(),
            shader,
            placeholder,
            DepthPass() == DepthPassKind::Prepass,
            request,
            material,
            Lighting());
        generation = result.generation;
        // 通常パスが正常でも追加パスの失敗診断を返します。
        error = result.error.empty() ? result.passError : result.error;
        if (result.drawn && result.placeholder)
        {

            ++m_state->m_frameStatistics.shaderFallbackDraws;
        }
        return result.drawn;
    }

    bool GraphicsDevice::TryGetMaterialShaderRenderState(
        const std::filesystem::path& shaderPath,
        const ShaderKeywordSet& keywords,
        ShaderRenderState& state) const
    {
        if (shaderPath.empty()
            || TryAssets() == nullptr
            || ActiveRenderingApi()
                != RenderingApi::DirectX12Experimental)
        {
            return false;
        }
        // 借用するD3D12素材サービス
        auto* const services = TryD3D12MaterialShaderServices(
            m_state->m_apiResources.get());
        return services != nullptr
            && services->TryGetMaterialShaderRenderState(
                MakeMaterialShaderSource(*this, shaderPath, keywords)
                    .cacheKey,
                state);
    }

    Detail::MaterialShaderPasses GraphicsDevice::PrepareMaterialShaderPasses(
        const std::filesystem::path& shaderPath,
        const ShaderKeywordSet& keywords)
    {
        if (shaderPath.empty()
            || TryAssets() == nullptr
            || ActiveRenderingApi()
                != RenderingApi::DirectX12Experimental)
        {
            return {};
        }
        // 借用するD3D12素材サービス
        auto* const services = TryD3D12MaterialShaderServices(
            m_state->m_apiResources.get());
        return services != nullptr
            ? services->PrepareMaterialShaderPasses(
                Assets(),
                MakeMaterialShaderSource(*this, shaderPath, keywords))
            : Detail::MaterialShaderPasses{};
    }

    void GraphicsDevice::InvalidateMaterialShader(
        const std::filesystem::path& shaderPath) const
    {
        if (shaderPath.empty())
        {
            return;
        }
        // 無効化する元素材の絶対パス
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // キーワード宣言のキャッシュも外し、次の取得で再読み込みします。
        m_state->m_shaderVariants.erase(absolutePath);
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // 借用するD3D12素材サービス
            if (auto* const services = TryD3D12MaterialShaderServices(
                    m_state->m_apiResources.get()))
            {
                services->InvalidateMaterialShader(absolutePath);
            }
            return;
        }
        // 借用するD3D11のAPI資源
        auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr)
        {
            return;
        }
        // 完全一致と「パス?」で始まる通常・骨変形の全バリアントを再読み込み対象にします。
        // バリアント識別キーの素材パス
        const auto prefix = absolutePath.wstring();
        // key: バリアント識別キー、value: 無効化するコードと状態
        for (auto& [key, value] : resources->materialShaders)
        {
            // キーワードを含むキャッシュキー
            const auto text = key.wstring();
            if (text == prefix
                || (text.rfind(prefix, 0) == 0
                    && text.size() > prefix.size()
                    && text[prefix.size()] == L'?'))
            {
                value->forceReload = true;
            }
        }
        // key: バリアント識別キー、value: 無効化するコードと状態
        for (auto& [key, value] : resources->skinnedMaterialShaders)
        {
            // キーワードを含むキャッシュキー
            const auto text = key.wstring();
            if (text == prefix
                || (text.rfind(prefix, 0) == 0
                    && text.size() > prefix.size()
                    && text[prefix.size()] == L'?'))
            {
                value->forceReload = true;
            }
        }
    }

    bool GraphicsDevice::DrawD3D12CustomParticles(
        const ParticleDrawRequest& request,
        const std::filesystem::path& shaderPath,
        const std::array<DirectX::XMFLOAT4, 8>& customParameters,
        std::uint64_t* const shaderGeneration,
        std::string* const shaderError)
    {
        // 借用するD3D12素材サービス
        auto* const services = TryD3D12MaterialShaderServices(
            m_state->m_apiResources.get());
        if (services == nullptr
            || shaderPath.empty()
            || TryAssets() == nullptr)
        {
            return false;
        }
        // 画素シェーダーは絶対パスで識別し、失敗は2Dシェーダー用の診断で説明します。
        // パーティクル用画素シェーダー指定
        Detail::MaterialShaderSource shader;
        shader.path = Assets().ResolvePath(shaderPath).lexically_normal();
        shader.cacheKey = shader.path;
        // 診断コールバックが借用する取得元
        auto* const assets = &Assets();
        // 2Dシェーダーの診断を説明します(message: 元の診断, path: 固定するHLSLのパス)。
        shader.describeFailure =
            [assets, path = shader.path](const char* const message)
            {
                return DescribeShaderFailure(
                    *assets,
                    path,
                    message,
                    ShaderUsage::Sprite);
            };
        // プロジェクトに代替表示のHLSLがなければエンジン同梱版を使います。
        // 代替表示の標準アセットパス
        constexpr const char* placeholderRelativePath =
            "shaders/LamaPonSpriteError.hlsl";
        // 代替表示に使うHLSLの実パス
        auto placeholderPath = Assets().ResolvePath(placeholderRelativePath);
        if (!Assets().FileExists(placeholderPath))
        {
            placeholderPath =
                ExecutableDirectory() / "assets" / placeholderRelativePath;
        }
        // 代替表示のソース指定
        Detail::MaterialShaderSource placeholder;
        placeholder.path = placeholderPath.lexically_normal();
        placeholder.cacheKey = placeholder.path;
        // 描画成否と代替表示・診断
        const auto result = services->DrawCustomParticles(
            Assets(),
            shader,
            placeholder,
            request,
            customParameters);
        if (shaderGeneration != nullptr)
        {
            *shaderGeneration = result.generation;
        }
        if (shaderError != nullptr)
        {
            *shaderError = result.error;
        }
        if (result.drawn && result.placeholder)
        {

            ++m_state->m_frameStatistics.shaderFallbackDraws;
        }
        return result.drawn;
    }

    void GraphicsDevice::InvalidateSpriteShader(
        const std::filesystem::path& shaderPath) const
    {
        if (ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // D3D12スプライトは描画ごとにキャッシュを確認するため、粒子用の画素シェーダーだけ無効化します。
            // 借用するD3D12素材サービス
            auto* const services = TryD3D12MaterialShaderServices(
                m_state->m_apiResources.get());
            if (services != nullptr
                && !shaderPath.empty()
                && TryAssets() != nullptr)
            {
                services->InvalidateCustomPixelShader(
                    Assets().ResolvePath(shaderPath).lexically_normal());
            }
            return;
        }
        // 借用するD3D11のAPI資源
        auto* const resources = TryD3D11ApiResources();
        if (shaderPath.empty() || resources == nullptr)
        {
            return;
        }
        // 正規化したシェーダー絶対パス
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // 画像効果キャッシュの検索結果
        const auto found = resources->spriteShaders.find(absolutePath);
        if (found != resources->spriteShaders.end())
        {
            found->second->forceReload = true;
        }
    }
}
