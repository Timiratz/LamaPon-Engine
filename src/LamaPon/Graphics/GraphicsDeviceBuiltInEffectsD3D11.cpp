#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
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
        // 失敗した組み込みシェーダーの再試行間隔です。連続コンパイルを避けつつ、
        // 修正後に復帰できる時間として設定します。
        constexpr double BuiltInRetrySeconds = 2.0;

        [[nodiscard]] double SteadySeconds() noexcept
        {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now()
                    .time_since_epoch()).count();
        }
    }

    // 組み込みシェーダーの失敗を記録して毎フレームの再コンパイルを防ぎ、
    // 再試行間隔後に復帰を試みます。代替描画経路が無いため失敗は送出し、
    // Application側が描画失敗を処理してエディターUIを継続します。
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
        const double now = SteadySeconds();
        if (!failure.message.empty()
            && now - failure.lastAttempt < BuiltInRetrySeconds)
        {
            // 再試行時刻までは記録済みの失敗を返し、コンパイルを省略します。
            throw std::runtime_error(failure.message);
        }
        failure.lastAttempt = now;
        try
        {
            slot = factory();
        }
        catch (const std::exception& exception)
        {
            // ログはApplicationの描画ループが1箇所で出します
            // （組み込みシェーダー以外の描画失敗も同じ扱いに
            // したいので、種類ごとに書き分けません）。
            failure.message = exception.what();
            throw;
        }
        failure.message.clear();
        return *slot;
    }

    EnvironmentRenderer&
        GraphicsDevice::Environment() const
    {
        return BuildBuiltIn(
            m_environmentRenderer,
            m_environmentFailure,
            [this]
            {
                return std::make_unique<EnvironmentRenderer>(
                    Device(),
                    Context(),
                    Assets(),
                    Assets().ResolvePath(
                        "shaders/LamaPonEnvironment.hlsl"));
            });
    }

    ClusteredLights& GraphicsDevice::Clusters() const
    {
        if (!m_clusteredLights)
        {
            // カリングCSがプロジェクトに無い場合は、互換性維持のため
            // エンジン同梱のアセットから読み込みます。
            constexpr const char* relativePath =
                "shaders/LamaPonLightCulling.hlsl";
            auto shaderPath =
                Assets().ResolvePath(relativePath);
            if (!Assets().FileExists(shaderPath))
            {
                shaderPath = ExecutableDirectory()
                    / "assets"
                    / relativePath;
            }
            return BuildBuiltIn(
                m_clusteredLights,
                m_clustersFailure,
                [this, shaderPath]
                {
                    return std::make_unique<ClusteredLights>(
                        Device(),
                        Assets(),
                        shaderPath);
                });
        }
        return *m_clusteredLights;
    }

    LitEffect& GraphicsDevice::Lit() const
    {
        return BuildBuiltIn(
            m_litEffect,
            m_litFailure,
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

    LitEffect& GraphicsDevice::SkinnedLit() const
    {
        return BuildBuiltIn(
            m_skinnedLitEffect,
            m_skinnedLitFailure,
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

    LitEffect* GraphicsDevice::ShaderErrorPlaceholder(
        const bool skinned) const
    {
        auto& effect =
            skinned ? m_skinnedErrorEffect : m_errorEffect;
        auto& unavailable = skinned
            ? m_skinnedErrorEffectUnavailable
            : m_errorEffectUnavailable;
        // 代替シェーダーを設定した回数をFrameStatisticsへ記録します。
        if (effect)
        {
            ++m_frameStatistics.shaderFallbackDraws;
            return effect.get();
        }
        if (unavailable)
        {
            return nullptr;
        }

        // プロジェクトに代替シェーダーが無い場合はエンジン同梱版を使い、
        // プロジェクトの版に関係なく同じ失敗表示を提供します。
        constexpr const char* relativePath =
            "shaders/LamaPonShaderError.hlsl";
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
            // 代役すら用意できないときは標準Litのままにします。
            // 知らせ方が無いだけで、描画は続けられます。
            unavailable = true;
            return nullptr;
        }
        ++m_frameStatistics.shaderFallbackDraws;
        return effect.get();
    }

    SpriteEffect* GraphicsDevice::SpriteErrorPlaceholder() const
    {
        // 3D側と同じく、渡した回数を数えます
        // （FrameStatistics::shaderFallbackDrawsを参照）。
        if (m_spriteErrorEffect)
        {
            ++m_frameStatistics.shaderFallbackDraws;
            return m_spriteErrorEffect.get();
        }
        if (m_spriteErrorEffectUnavailable)
        {
            return nullptr;
        }

        constexpr const char* relativePath =
            "shaders/LamaPonSpriteError.hlsl";
        auto shaderPath = Assets().ResolvePath(relativePath);
        if (!Assets().FileExists(shaderPath))
        {
            shaderPath =
                ExecutableDirectory() / "assets" / relativePath;
        }

        try
        {
            m_spriteErrorEffect =
                std::make_unique<SpriteEffect>(
                    Device(),
                    Context(),
                    Assets(),
                    shaderPath);
        }
        catch (const std::exception&)
        {
            m_spriteErrorEffectUnavailable = true;
            return nullptr;
        }
        ++m_frameStatistics.shaderFallbackDraws;
        return m_spriteErrorEffect.get();
    }
}
