#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/D3D11Backend.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Resources.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/SpriteEffect.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace LamaPon
{
    namespace
    {
        // 失敗した組込生成の再試行秒数
        constexpr double BuiltInRetrySeconds = 2.0;

        // 単調時計の経過秒数を返します。
        [[nodiscard]] double SteadySeconds() noexcept
        {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now()
                    .time_since_epoch()).count();
        }
    }

    // 組込資源を遅延生成します(slot: 資源の所有先, failure: 前回失敗と試行時刻, factory: 資源を返す生成関数)。
    // 失敗は記録して再送出し、2秒間は再コンパイルせず同じ例外を返します。
    template <typename T, typename Factory>
    T& GraphicsDevice::BuildBuiltIn(
        std::unique_ptr<T>& slot,
        BuiltInFailure& failure,
        Factory&& factory) const
    {
        if (slot)
        {
            return *slot;
        }
        // 現在の単調時計の秒数
        const double now = SteadySeconds();
        if (!failure.message.empty()
            && now - failure.lastAttempt < BuiltInRetrySeconds)
        {

            throw std::runtime_error(failure.message);
        }
        failure.lastAttempt = now;
        try
        {
            slot = factory();
        }
        // 記録して再送出する生成例外
        catch (const std::exception& exception)
        {
            // 失敗のログ出力はApplicationの描画ループが担当します。
            failure.message = exception.what();
            throw;
        }
        failure.message.clear();
        return *slot;
    }

    // D3D11環境描画器を遅延生成して返します。
    EnvironmentRenderer&
        GraphicsDevice::Environment() const
    {
        // 現在のD3D11所有資源
        auto& resources = RequireD3D11ApiResources();
        return BuildBuiltIn(
            resources.environmentRenderer,
            resources.environmentFailure,
            [this]
            {
                // 生成する環境描画器
                auto renderer = std::unique_ptr<EnvironmentRenderer>{
                    new EnvironmentRenderer(
                        Device(),
                        Context(),
                        Assets(),
                        Assets().ResolvePath(
                            "shaders/LamaPonEnvironment.hlsl"))
                };
                renderer->AttachD3D11Backend(
                    dynamic_cast<D3D11Backend*>(m_state->m_backend.get()));
                return renderer;
            });
    }

    // Forward+資源を遅延生成して返します。
    ClusteredLights& GraphicsDevice::Clusters() const
    {
        if (!m_state->m_clusteredLights)
        {
            // プロジェクトにカリングシェーダーがなければエンジン同梱版を使います。
            // 同梱シェーダーの相対パス
            constexpr const char* relativePath =
                "shaders/LamaPonLightCulling.hlsl";
            // 使用するシェーダーのパス
            auto shaderPath =
                Assets().ResolvePath(relativePath);
            if (!Assets().FileExists(shaderPath))
            {
                shaderPath = ExecutableDirectory()
                    / "assets"
                    / relativePath;
            }
            return BuildBuiltIn(
                m_state->m_clusteredLights,
                m_state->m_clustersFailure,
                [this, shaderPath]
                {
                    // 生成するForward+資源
                    auto clusteredLights =
                        std::make_unique<ClusteredLights>();
                    m_state->m_backend->InitializeClusteredLights(
                        *clusteredLights,
                        Assets(),
                        shaderPath);
                    return clusteredLights;
                });
        }
        return *m_state->m_clusteredLights;
    }

    // D3D11標準照明効果を遅延生成して返します。
    LitEffect& GraphicsDevice::Lit() const
    {
        // 現在のD3D11所有資源
        auto& resources = RequireD3D11ApiResources();
        return BuildBuiltIn(
            resources.litEffect,
            resources.litFailure,
            [this]
            {
                return std::make_unique<LitEffect>(
                    Device(),
                    Context(),
                    Assets(),
                    Assets().ResolvePath(
                        "shaders/LamaPonLit.hlsl"));
            });
    }

    // D3D11骨変形の標準照明効果を遅延生成して返します。
    LitEffect& GraphicsDevice::SkinnedLit() const
    {
        // 現在のD3D11所有資源
        auto& resources = RequireD3D11ApiResources();
        return BuildBuiltIn(
            resources.skinnedLitEffect,
            resources.skinnedLitFailure,
            [this]
            {
                return std::make_unique<LitEffect>(
                    Device(),
                    Context(),
                    Assets(),
                    Assets().ResolvePath(
                        "shaders/LamaPonLit.hlsl"),
                    true);
            });
    }

    // 代替表示効果を返し、生成失敗ならnullptrです(skinned: 骨変形の指定)。
    LitEffect* GraphicsDevice::ShaderErrorPlaceholder(
        const bool skinned) const
    {
        // 現在のD3D11所有資源
        auto& resources = RequireD3D11ApiResources();
        // 通常・骨変形の代替資源
        auto& effect =
            skinned ? resources.skinnedErrorEffect : resources.errorEffect;
        // 代替生成の再試行停止状態
        auto& unavailable = skinned
            ? resources.skinnedErrorEffectUnavailable
            : resources.errorEffectUnavailable;
        // 代替シェーダーを渡すたびにshaderFallbackDrawsを加算します。
        if (effect)
        {
            ++m_state->m_frameStatistics.shaderFallbackDraws;
            return effect.get();
        }
        if (unavailable)
        {
            return nullptr;
        }

        // プロジェクトに代替シェーダーがなければエンジン同梱版を使います。
        // 同梱シェーダーの相対パス
        constexpr const char* relativePath =
            "shaders/LamaPonShaderError.hlsl";
        // 使用するシェーダーのパス
        auto shaderPath = Assets().ResolvePath(relativePath);
        if (!Assets().FileExists(shaderPath))
        {
            shaderPath =
                ExecutableDirectory() / "assets" / relativePath;
        }

        try
        {
            effect = std::make_unique<LitEffect>(
                Device(),
                Context(),
                Assets(),
                shaderPath,
                skinned);
        }
        catch (const std::exception&)
        {
            // 代替シェーダーの生成に失敗したら再初期化まで再試行しません。
            unavailable = true;
            return nullptr;
        }
        ++m_state->m_frameStatistics.shaderFallbackDraws;
        return effect.get();
    }

    // スプライトの代替効果を返し、生成失敗ならnullptrです。
    SpriteEffect* GraphicsDevice::SpriteErrorPlaceholder() const
    {
        // 現在のD3D11所有資源
        auto& resources = RequireD3D11ApiResources();
        // 代替シェーダーを渡すたびにshaderFallbackDrawsを加算します。
        if (resources.spriteErrorEffect)
        {
            ++m_state->m_frameStatistics.shaderFallbackDraws;
            return resources.spriteErrorEffect.get();
        }
        if (resources.spriteErrorEffectUnavailable)
        {
            return nullptr;
        }

        // 同梱シェーダーの相対パス
        constexpr const char* relativePath =
            "shaders/LamaPonSpriteError.hlsl";
        // 使用するシェーダーのパス
        auto shaderPath = Assets().ResolvePath(relativePath);
        if (!Assets().FileExists(shaderPath))
        {
            shaderPath =
                ExecutableDirectory() / "assets" / relativePath;
        }

        try
        {
            resources.spriteErrorEffect =
                std::make_unique<SpriteEffect>(
                    Device(),
                    Context(),
                    Assets(),
                    shaderPath);
        }
        catch (const std::exception&)
        {
            resources.spriteErrorEffectUnavailable = true;
            return nullptr;
        }
        ++m_state->m_frameStatistics.shaderFallbackDraws;
        return resources.spriteErrorEffect.get();
    }
}
