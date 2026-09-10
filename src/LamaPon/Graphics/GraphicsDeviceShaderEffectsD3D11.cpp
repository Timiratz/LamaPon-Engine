#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/ComputeEffect.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Access.h"
#include "LamaPon/Graphics/GraphicsDeviceShaderState.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/LitTextureRequest.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ScreenEffect.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/ShaderDiagnostics.h"
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
    // コンパイル失敗時にソースを読み、原因に対応する診断を追加します。
    [[nodiscard]] std::string DescribeShaderFailure(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& shaderPath,
        const char* compilerMessage,
        const LamaPon::ShaderUsage usage)
    {
        std::string source;
        try
        {
            const auto bytes =
                assets.ReadFileBytes(shaderPath);
            source.assign(
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size());
        }
        catch (const std::exception&)
        {
            // 読めなくても説明は返せます（includeの取りこぼしなど、
            // ソースを見なくても分かるものがあるため）。
        }
        return LamaPon::ExplainShaderError(
            compilerMessage != nullptr ? compilerMessage : "",
            source,
            usage);
    }

    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT3& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z);
    }

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

        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        resolved->GetResource(resource.ReleaseAndGetAddressOf());
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (resource == nullptr || FAILED(resource.As(&texture)))
        {
            return false;
        }
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

        const auto absolutePath = Assets().ResolvePath(
            description.pixelShader).lexically_normal();
        auto& entry = m_state->m_spriteShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<SpriteShaderEntry>();
        }

        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            const bool archived = Assets().IsArchived();
            std::error_code fileError;
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
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
                    catch (const std::exception& exception)
                    {
                        entry->error = DescribeShaderFailure(
                            Assets(),
                            absolutePath,
                            exception.what(),
                            ShaderUsage::Sprite);
                        // 直前に成功したものを描き続けると、書き
                        // 間違えたシェーダーが前のまま出ます。
                        // 3Dと同じく捨てて代役に任せます。
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
                if (auto* const placeholder =
                        SpriteErrorPlaceholder())
                {
                    status.fallback =
                        SpriteShaderFallback::ErrorPlaceholder;
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

        // SpriteBatch invokes this callback only when it flushes. Capture both
        // the compiled generation and this pass's values: another renderer may
        // use or hot-reload the same shader between Begin and End.
        auto effect = entry->effect;
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

        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        auto& entry = m_state->m_spriteShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<SpriteShaderEntry>();
        }

        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            const bool archived = Assets().IsArchived();
            std::error_code fileError;
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
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
                    catch (const std::exception& exception)
                    {
                        entry->error = DescribeShaderFailure(
                            Assets(),
                            absolutePath,
                            exception.what(),
                            ShaderUsage::Sprite);
                        // スプライトと同じく、失敗したら直前の
                        // シェーダーは残しません。
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
            // スプライトと同じく、失敗はマゼンタで知らせます。
            if (!entry->error.empty())
            {
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
        // 対象地点にエフェクトが無い場合は、深度変換係数の計算を省略します。
        if (std::ranges::none_of(
                m_state->m_queuedScreenEffects,
                [point](const QueuedScreenEffect& queued)
                {
                    return queued.point == point;
                }))
        {
            return;
        }
        // 深度を距離へ直す係数。式は距離＝y/(深度+x)で、SSRの
        // Hi-Z作成（PSReflectionDepthLinearize）と同じものです。
        // SSRの深度変換と同じ式を使い、変換規則を一致させます。
        // 射影はこの画像を描いたときのもの
        // （TAAのずらし込み＝深度バッファと噛み合う方）。
        const auto& projection = SceneProjection();
        const DirectX::XMFLOAT4 depthParameters{
            projection._33,
            projection._43,
            1.0f,
            0.0f
        };
        // ビュー空間の位置（＝法線の再構成）用。SSAOが持っている
        // AmbientOcclusionProjectionのzwと同じ中身です。
        // 0除算よけの1e-6は、射影が空のときに無限大を配らないため。
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
        const auto whiteTextureView = WhiteTextureViewHandle();
        auto* const whiteTexture =
            TryResolveD3D11ShaderResourceView(whiteTextureView);
        for (const auto& queued : m_state->m_queuedScreenEffects)
        {
            if (queued.effect == nullptr
                || queued.point != point)
            {
                continue;
            }
            std::array<std::shared_ptr<
                const TextureResourceSnapshot>, 2>
                auxiliaryResources{};
            std::array<ID3D11ShaderResourceView*, 2>
                auxiliaryViews{};
            for (std::size_t index = 0;
                index < auxiliaryViews.size();
                ++index)
            {
                const auto& asset =
                    queued.auxiliaryTextures[index];
                auxiliaryResources[index] = asset != nullptr
                    ? asset->resources.Acquire()
                    : nullptr;
                auto* const resolved =
                    auxiliaryResources[index] != nullptr
                    ? TryResolveD3D11ShaderResourceView(
                        *auxiliaryResources[index])
                    : nullptr;
                auxiliaryViews[index] = resolved != nullptr
                    ? resolved
                    : whiteTexture;
            }
            target.ApplyScreenEffect(
                *queued.effect,
                auxiliaryViews,
                depthParameters,
                depthUnprojection,
                queued.parameters);
        }
        // 現在の地点で適用したエフェクトだけを取り除きます。同じフレームの
        // 後続地点に登録されたエフェクトはキューへ残します。
        std::erase_if(
            m_state->m_queuedScreenEffects,
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

        const auto absolutePath =
            Assets().ResolvePath(request.shader)
                .lexically_normal();
        auto& entry = m_state->m_screenShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<ScreenShaderEntry>();
        }

        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            const bool archived = Assets().IsArchived();
            std::error_code fileError;
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
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
                        auto candidate =
                            std::make_unique<ScreenEffect>(
                                Device(),
                                Context(),
                                Assets(),
                                absolutePath);
                        entry->effect = std::move(candidate);
                        entry->generation =
                            ++m_state->m_screenShaderGeneration;
                        entry->error.clear();
                    }
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

        QueuedScreenEffect queued{};
        queued.effect = entry->effect.get();
        queued.parameters = request.customParameters;
        queued.point = request.point;
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
        m_state->m_queuedScreenEffects.emplace_back(
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

        const auto absolutePath =
            Assets().ResolvePath(request.shader)
                .lexically_normal();
        auto& entry = m_state->m_computeShaders[absolutePath];
        if (!entry)
        {
            entry = std::make_unique<ComputeShaderEntry>();
        }

        // 更新の見張り方はScreenEffectと同じです（保存したら
        // 作り直す、失敗しても直前の正常な版を残す）。
        const auto now = std::chrono::steady_clock::now();
        if (!entry->observed
            || entry->forceReload
            || now >= entry->nextCheck)
        {
            entry->nextCheck =
                now + std::chrono::milliseconds(250);
            const bool archived = Assets().IsArchived();
            std::error_code fileError;
            const bool sourceExists =
                Assets().FileExists(absolutePath);
            const auto writeTime =
                (sourceExists && !archived)
                ? std::filesystem::last_write_time(
                    absolutePath,
                    fileError)
                : std::filesystem::file_time_type{};
            const bool changed = !entry->observed
                || entry->forceReload
                || entry->sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry->writeTime != writeTime);
            if (changed)
            {
                entry->observed = true;
                entry->forceReload = false;
                entry->sourceExists = sourceExists;
                entry->writeTime = writeTime;
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

        // 書き込み先。UAVが要るので、Resizeの前に印を付けます
        // （バインドフラグは作成時にしか決められません）。
        auto& target = AcquireComputeTexture(
            request.outputTexture,
            request.outputWidth,
            request.outputHeight);
        if (target.DisplayUnorderedAccessView() == nullptr)
        {
            if (error != nullptr)
            {
                *error =
                    "The compute output texture could not"
                    " be created for writing.";
            }
            return false;
        }

        std::array<std::shared_ptr<
            const TextureResourceSnapshot>, 2>
            inputResources{};
        std::array<ID3D11ShaderResourceView*, 2> inputs{};
        const auto whiteTextureView = WhiteTextureViewHandle();
        auto* const whiteTexture =
            TryResolveD3D11ShaderResourceView(whiteTextureView);
        for (std::size_t index = 0;
            index < request.inputTextures.size();
            ++index)
        {
            if (request.inputTextures[index].empty())
            {
                inputs[index] = whiteTexture;
                continue;
            }
            const auto texture = Assets().LoadTexture(
                request.inputTextures[index]);
            inputResources[index] = texture != nullptr
                ? texture->resources.Acquire()
                : nullptr;
            auto* const resolved = inputResources[index] != nullptr
                ? TryResolveD3D11ShaderResourceView(
                    *inputResources[index])
                : nullptr;
            inputs[index] = resolved != nullptr
                ? resolved
                : whiteTexture;
        }

        GpuProfiler::SectionScope computeSection{
            m_state->m_gpuProfiler,
            "Compute"
        };
        entry->effect->Dispatch(
            inputs,
            target.DisplayUnorderedAccessView(),
            target.Width(),
            target.Height(),
            request.customParameters);
        computeSection.End();
        return true;
    }

    void GraphicsDevice::InvalidateComputeEffectShader(
        const std::filesystem::path& shaderPath) const
    {
        if (shaderPath.empty() || !TryAssets())
        {
            return;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath)
                .lexically_normal();
        const auto found =
            m_state->m_computeShaders.find(absolutePath);
        if (found != m_state->m_computeShaders.end()
            && found->second)
        {
            found->second->forceReload = true;
        }
    }

    void GraphicsDevice::InvalidateScreenEffectShader(
        const std::filesystem::path& shaderPath) const
    {
        if (shaderPath.empty() || !TryAssets())
        {
            return;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath)
                .lexically_normal();
        const auto found =
            m_state->m_screenShaders.find(absolutePath);
        if (found != m_state->m_screenShaders.end()
            && found->second)
        {
            found->second->forceReload = true;
        }
    }

    bool GraphicsDevice::IsShaderCompiling(
        const std::filesystem::path& shaderPath,
        const ShaderKeywordSet& keywords) const
    {
        if (shaderPath.empty())
        {
            return false;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        const auto normalized = NormalizeKeywords(
            ShaderVariantsFor(absolutePath),
            keywords);
        const auto variantKey = normalized.Key();
        const std::filesystem::path cacheKey =
            variantKey.empty()
                ? absolutePath
                : std::filesystem::path(
                    absolutePath.wstring()
                    + L"?"
                    + Utf8ToWide(variantKey));
        const auto found = m_state->m_materialShaders.find(cacheKey);
        return found != m_state->m_materialShaders.end()
            && found->second->pending;
    }

    const ShaderVariantDeclaration&
        GraphicsDevice::ShaderVariantsFor(
            const std::filesystem::path& shaderPath) const
    {
        static const ShaderVariantDeclaration empty;
        if (shaderPath.empty())
        {
            return empty;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        const auto found = m_state->m_shaderVariants.find(absolutePath);
        if (found != m_state->m_shaderVariants.end())
        {
            return found->second;
        }
        ShaderVariantDeclaration declaration;
        try
        {
            if (Assets().FileExists(absolutePath))
            {
                const auto source =
                    Assets().ReadFileBytes(absolutePath);
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
        Microsoft::WRL::ComPtr<ID3D11Device> effectDevice;
        effect.m_context->GetDevice(
            effectDevice.ReleaseAndGetAddressOf());
        if (effectDevice.Get() != Device())
        {
            return false;
        }

        const auto tryResolve = [this](
            const GraphicsViewHandle& view,
            ID3D11ShaderResourceView*& resolved) noexcept
        {
            resolved = TryResolveD3D11ShaderResourceView(view);
            return !view || resolved != nullptr;
        };

        ID3D11ShaderResourceView* albedo{};
        ID3D11ShaderResourceView* normal{};
        PbrTextures pbrTextures{};
        std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount> customTextures{};
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
        Microsoft::WRL::ComPtr<ID3D11Device> effectDevice;
        effect.m_context->GetDevice(
            effectDevice.ReleaseAndGetAddressOf());
        if (effectDevice.Get() != Device())
        {
            return false;
        }

        const bool hasSpecular = static_cast<bool>(probe.specular);
        const bool hasIrradiance = static_cast<bool>(probe.irradiance);
        if (!hasSpecular && !hasIrradiance)
        {
            // ProbeなしはSetLightingが設定したSky IBLを維持します。
            // secondary側の古いhandleも無効なmetadataとして解決しません。
            return true;
        }
        if (hasSpecular != hasIrradiance)
        {
            return false;
        }

        constexpr auto ExpectedMaximumMip = static_cast<float>(
            EnvironmentRenderer::PrefilteredSpecularMipLevels - 1);
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
        Microsoft::WRL::ComPtr<ID3D11Device> effectDevice;
        effect.m_context->GetDevice(
            effectDevice.ReleaseAndGetAddressOf());
        if (effectDevice.Get() != Device())
        {
            return false;
        }

        LitEffect::D3D11LightingViews nativeViews;

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
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            resolved->GetResource(resource.ReleaseAndGetAddressOf());
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
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            resolved->GetDesc(&viewDescription);
            if (viewDescription.ViewDimension
                    != D3D11_SRV_DIMENSION_TEXTURECUBE
                || viewDescription.TextureCube.MostDetailedMip != 0
                || viewDescription.TextureCube.MipLevels == 0)
            {
                return false;
            }

            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            resolved->GetResource(resource.ReleaseAndGetAddressOf());
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (resource == nullptr || FAILED(resource.As(&texture)))
            {
                return false;
            }
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
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

            UINT formatSupport{};
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
        const auto tryRecoverDimension = [](
            const float inverseDimension,
            std::uint32_t& dimension) noexcept
        {
            if (!std::isfinite(inverseDimension)
                || !(inverseDimension > 0.0f))
            {
                return false;
            }
            const auto exactDimension =
                1.0 / static_cast<double>(inverseDimension);
            if (!std::isfinite(exactDimension)
                || exactDimension < 1.0
                || exactDimension
                    > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            {
                return false;
            }
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

            const bool hasSpecular =
                static_cast<bool>(environment.specular);
            const bool hasIrradiance =
                static_cast<bool>(environment.irradiance);
            if (hasSpecular != hasIrradiance)
            {
                return false;
            }
            if (hasSpecular)
            {
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

            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            resolved->GetResource(resource.ReleaseAndGetAddressOf());
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (resource == nullptr || FAILED(resource.As(&texture)))
            {
                return false;
            }
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            const auto resolution =
                static_cast<std::uint32_t>(roundedResolution);
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

        const auto& directionalShadow = lighting.directionalShadow;
        if (directionalShadow.enabled)
        {
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

        bool hasSpotShadow{};
        const auto spotLightCount = std::min(
            lighting.spotLightCount,
            MaximumSpotLights);
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

        const auto& pointShadow = lighting.pointShadow;
        if (pointShadow.enabled)
        {
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
            D3D11_TEXTURE2D_DESC description{};
            std::uint32_t targetWidth{};
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
            D3D11_TEXTURE2D_DESC colorDescription{};
            D3D11_TEXTURE2D_DESC depthDescription{};
            std::uint32_t targetWidth{};
            std::uint32_t targetHeight{};
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
            std::uint32_t fullMipLevels{ 1 };
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

            const std::array<const GraphicsViewHandle*, 3> handles{
                &clustered.lights,
                &clustered.lightIndices,
                &clustered.clusterCounts
            };
            const std::array<std::uint32_t, 3> expectedStrides{
                static_cast<std::uint32_t>(sizeof(GpuLight)),
                static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                static_cast<std::uint32_t>(sizeof(std::uint32_t))
            };
            const std::array<std::uint32_t, 3> expectedElements{
                static_cast<std::uint32_t>(MaximumClusteredLights),
                ClusteredLights::ClusterCount
                    * ClusteredLights::MaximumLightsPerCluster,
                ClusteredLights::ClusterCount
            };
            for (std::size_t index{};
                index < handles.size();
                ++index)
            {
                const auto& handle = *handles[index];
                auto* const nativeView =
                    TryResolveD3D11ShaderResourceView(handle);
                if (!handle || nativeView == nullptr)
                {
                    return false;
                }

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

                Microsoft::WRL::ComPtr<ID3D11Resource> resource;
                nativeView->GetResource(
                    resource.ReleaseAndGetAddressOf());
                Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
                if (resource == nullptr
                    || FAILED(resource.As(&buffer)))
                {
                    return false;
                }
                D3D11_BUFFER_DESC bufferDescription{};
                buffer->GetDesc(&bufferDescription);
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

        const auto& bakedGi = lighting.bakedGlobalIllumination;
        if (bakedGi.enabled)
        {
            D3D11_TEXTURE3D_DESC expectedVolume{};
            const std::array<const GraphicsViewHandle*, 3> handles{
                &bakedGi.redCoefficients,
                &bakedGi.greenCoefficients,
                &bakedGi.blueCoefficients
            };
            for (std::size_t index{};
                index < handles.size();
                ++index)
            {
                const auto& handle = *handles[index];
                auto* const nativeView =
                    TryResolveD3D11ShaderResourceView(handle);
                if (!handle || nativeView == nullptr)
                {
                    return false;
                }
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

                Microsoft::WRL::ComPtr<ID3D11Resource> resource;
                nativeView->GetResource(
                    resource.ReleaseAndGetAddressOf());
                Microsoft::WRL::ComPtr<ID3D11Texture3D> volume;
                if (resource == nullptr
                    || FAILED(resource.As(&volume)))
                {
                    return false;
                }
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

        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // 宣言に無いキーワードは落とします。シェーダーを差し替えた
        // 後のマテリアルが、存在しないキーワードでコンパイルを
        // 走らせないようにするためです。
        const auto normalized = NormalizeKeywords(
            ShaderVariantsFor(absolutePath),
            keywords);
        // 同じHLSLでもバリアントごとに別のエントリーです。キーは
        // 「パス?キーワード」で、キーワードは常に整列済みなので
        // 同じ組み合わせなら必ず同じキーになります。
        const auto variantKey = normalized.Key();
        const std::filesystem::path cacheKey =
            variantKey.empty()
                ? absolutePath
                : std::filesystem::path(
                    absolutePath.wstring()
                    + L"?"
                    + Utf8ToWide(variantKey));
        auto& entry = m_state->m_materialShaders[cacheKey];
        if (!entry)
        {
            entry = std::make_unique<MaterialShaderEntry>();
            entry->keywords = normalized.Keywords();
        }

        // コンパイル失敗を明示するため標準Litではなくマゼンタの代替表示を使います。
        const auto resolve = [this, &entry]() -> LitEffect&
        {
            if (entry->effect)
            {
                return *entry->effect;
            }
            if (!entry->error.empty())
            {
                if (auto* const placeholder =
                        ShaderErrorPlaceholder(false))
                {
                    return *placeholder;
                }
            }
            return Lit();
        };

        // 非同期コンパイル完了後にLitEffectを組み立てます
        // （キャッシュに当たるので一瞬で終わります）。
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

        // アーカイブで配布したゲームには変更監視の対象となる展開済みファイルが
        // ありません。ホットリロードの更新日時確認はエディター上の展開済み
        // アセットだけに行い、アーカイブ内のシェーダーは一度だけ読み込みます。
        const bool archived = Assets().IsArchived();
        std::error_code fileError;
        const bool sourceExists = Assets().FileExists(absolutePath);
        const auto writeTime = (sourceExists && !archived)
            ? std::filesystem::last_write_time(
                absolutePath,
                fileError)
            : std::filesystem::file_time_type{};
        const bool changed = !entry->observed
            || entry->forceReload
            || entry->sourceExists != sourceExists
            || (sourceExists
                && !archived
                && entry->writeTime != writeTime);
        if (changed)
        {
            entry->observed = true;
            entry->forceReload = false;
            entry->sourceExists = sourceExists;
            entry->writeTime = writeTime;
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
                    // アーカイブ（書き出したゲーム）は全部事前
                    // コンパイル済みなので待ち時間が無く、かつ
                    // アーカイブ読み取りはスレッド安全ではないため
                    // 同期のままにします。
                    if (m_state->m_asyncShaderCompilation
                        && !Assets().IsArchived())
                    {
                        // effectを破棄すると、失敗時は代替表示へ切り替わります。
                        auto* const assets = &Assets();
                        const auto path = absolutePath;
                        const auto keywordList = entry->keywords;
                        entry->effect.reset();
                        entry->pending = true;
                        entry->error.clear();
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
                    // 再コンパイル失敗を視認できるよう、直前のシェーダーを破棄して
                    // 代替表示へ切り替えます。
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

        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // 通常マテリアルと同じく、バリアントごとに別エントリーです。
        const auto normalized = NormalizeKeywords(
            ShaderVariantsFor(absolutePath),
            keywords);
        const auto variantKey = normalized.Key();
        const std::filesystem::path cacheKey =
            variantKey.empty()
                ? absolutePath
                : std::filesystem::path(
                    absolutePath.wstring()
                    + L"?"
                    + Utf8ToWide(variantKey));
        auto& entry = m_state->m_skinnedMaterialShaders[cacheKey];
        if (!entry)
        {
            entry = std::make_unique<MaterialShaderEntry>();
            entry->keywords = normalized.Keywords();
        }

        // 通常マテリアルと同じく、失敗時はマゼンタの代替表示を使い、
        // シェーダーを作成できなかったモデルを画面上で特定できます。
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

        // アーカイブで配布したゲームには変更監視の対象となる展開済みファイルが
        // ありません。ホットリロードの更新日時確認はエディター上の展開済み
        // アセットだけに行い、アーカイブ内のシェーダーは一度だけ読み込みます。
        const bool archived = Assets().IsArchived();
        std::error_code fileError;
        const bool sourceExists = Assets().FileExists(absolutePath);
        const auto writeTime = (sourceExists && !archived)
            ? std::filesystem::last_write_time(
                absolutePath,
                fileError)
            : std::filesystem::file_time_type{};
        const bool changed = !entry->observed
            || entry->forceReload
            || entry->sourceExists != sourceExists
            || (sourceExists
                && !archived
                && entry->writeTime != writeTime);
        if (changed)
        {
            entry->observed = true;
            entry->forceReload = false;
            entry->sourceExists = sourceExists;
            entry->writeTime = writeTime;
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
                    // 通常マテリアルと同じく、失敗したら直前の
                    // シェーダーは残しません。
                    entry->effect.reset();
                }
            }
        }

        generation = entry->generation;
        error = entry->error;
        return resolve();
    }

    void GraphicsDevice::InvalidateMaterialShader(
        const std::filesystem::path& shaderPath) const
    {
        if (shaderPath.empty())
        {
            return;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        // 宣言そのものも読み直します（multi_compileの行を
        // 足し引きしたときに追従するため）。
        m_state->m_shaderVariants.erase(absolutePath);
        // バリアントごとに別エントリーなので、そのHLSLから作られた
        // ものを全部立て直します（キーは「パス?キーワード」）。
        const auto prefix = absolutePath.wstring();
        for (auto& [key, value] : m_state->m_materialShaders)
        {
            const auto text = key.wstring();
            if (text == prefix
                || (text.rfind(prefix, 0) == 0
                    && text.size() > prefix.size()
                    && text[prefix.size()] == L'?'))
            {
                value->forceReload = true;
            }
        }
        for (auto& [key, value] : m_state->m_skinnedMaterialShaders)
        {
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

    void GraphicsDevice::InvalidateSpriteShader(
        const std::filesystem::path& shaderPath) const
    {
        if (shaderPath.empty())
        {
            return;
        }
        const auto absolutePath =
            Assets().ResolvePath(shaderPath).lexically_normal();
        const auto found =
            m_state->m_spriteShaders.find(absolutePath);
        if (found != m_state->m_spriteShaders.end())
        {
            found->second->forceReload = true;
        }
    }
}
