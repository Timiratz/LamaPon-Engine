#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace LamaPon
{
    // 描画先資源を再作成します(target: 更新する描画先, width: 画像幅, height: 画像高)。
    void GraphicsDevice::ResizeOffscreenTarget(
        RenderTarget& target,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "ResizeOffscreenTarget requires an initialized device.");
        }

        m_state->m_backend->ResizeOffscreenTarget(target, width, height);
        if (!target.IsValid()
            || !IsGraphicsViewCurrent(
                target.CurrentColorViewHandle())
            || !IsGraphicsViewCurrent(
                target.DisplayViewHandle()))
        {
            throw std::logic_error(
                "ResizeOffscreenTarget failed to publish current color "
                "and display views.");
        }
    }

    // 描画先を設定して色と深度を消去します(target: 描画先, clearColor: RGBAの消去色)。
    void GraphicsDevice::BeginOffscreenTarget(
        RenderTarget& target,
        const float clearColor[4])
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginOffscreenTarget requires an initialized device.");
        }
        if (clearColor == nullptr)
        {
            throw std::invalid_argument(
                "BeginOffscreenTarget requires a clear color.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "BeginOffscreenTarget requires a valid target.");
        }

        m_state->m_backend->BeginOffscreenTarget(target, clearColor);
    }

    // 画像を消さず描画先を設定します(target: 描画先)。
    void GraphicsDevice::BindOffscreenTarget(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BindOffscreenTarget requires an initialized device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "BindOffscreenTarget requires a valid target.");
        }

        m_state->m_backend->BindOffscreenTarget(
            target);
    }

    // 描画先を変えず表示用画像へ確定します(target: 確定する描画先)。
    void GraphicsDevice::PublishOffscreenTarget(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "PublishOffscreenTarget requires an initialized device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "PublishOffscreenTarget requires a valid target.");
        }
        if (!IsGraphicsViewCurrent(
                target.CurrentColorViewHandle())
            || !IsGraphicsViewCurrent(
                target.DisplayViewHandle()))
        {
            throw std::invalid_argument(
                "PublishOffscreenTarget requires a target owned by the "
                "active backend.");
        }

        // 表示画像への確定は描画先を変えず、フレーム制御側でバックバッファへ戻します。
        m_state->m_backend->PublishOffscreenTarget(
            target);
    }

    // 消去・復元せず深度だけを設定します(target: 深度の描画先)。
    void GraphicsDevice::BindOffscreenTargetDepthOnly(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BindOffscreenTargetDepthOnly requires an initialized "
                "device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "BindOffscreenTargetDepthOnly requires a valid target.");
        }

        m_state->m_backend->BindOffscreenTargetDepthOnly(target);
    }

    // 描画先を変えず読み取り用深度を保存します(target: 保存先)。
    void GraphicsDevice::CaptureOffscreenTargetDepth(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetDepth requires an initialized "
                "device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetDepth requires a valid target.");
        }

        m_state->m_backend->CaptureOffscreenTargetDepth(target);
    }

    // SSR用HDR履歴を保存します(target: 保存先, viewProjection: 画像描画時の行列)。
    void GraphicsDevice::CaptureOffscreenTargetColorHistory(
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetColorHistory requires an "
                "initialized device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetColorHistory requires a valid "
                "target.");
        }

        m_state->m_backend->CaptureOffscreenTargetColorHistory(
            target,
            viewProjection);
    }

    // TAA用履歴を保存します(target: 保存先, viewProjection: ジッターなしの再投影行列)。
    void GraphicsDevice::CaptureOffscreenTargetTemporalHistory(
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetTemporalHistory requires an "
                "initialized device.");
        }
        if (!target.IsValid())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetTemporalHistory requires a valid "
                "target.");
        }

        m_state->m_backend->CaptureOffscreenTargetTemporalHistory(
            target,
            viewProjection);
    }

    // 名前で共有する描画先を取得・生成します(name: 登録名, width: 画像幅で0は1, height: 画像高で0は1)。
    RenderTarget& GraphicsDevice::AcquireRenderTexture(
        const std::string& name,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        // 0を1に補正した画像幅
        const std::uint32_t safeWidth =
            width == 0 ? 1 : width;
        // 0を1に補正した画像高
        const std::uint32_t safeHeight =
            height == 0 ? 1 : height;

        // 名前で共有する描画先の所有先
        auto& slot = m_state->m_renderTextures[name];
        // 今回の新規登録有無
        const bool createdTarget = !slot;
        try
        {
            if (!slot)
            {
                slot = std::make_unique<RenderTarget>();
            }

            ResizeOffscreenTarget(
                *slot,
                safeWidth,
                safeHeight);
            return *slot;
        }
        catch (...)
        {
            if (createdTarget)
            {
                m_state->m_renderTextures.erase(name);
            }
            throw;
        }
    }

    // 名前で共有する計算用描画先を取得・生成します(name: 通常描画先と別の登録名, width: 画像幅で0は1, height: 画像高で0は1)。
    RenderTarget& GraphicsDevice::AcquireComputeTexture(
        const std::string& name,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        // 0を1に補正した画像幅
        const std::uint32_t safeWidth =
            width == 0 ? 1 : width;
        // 0を1に補正した画像高
        const std::uint32_t safeHeight =
            height == 0 ? 1 : height;

        // 名前で共有する描画先の所有先
        auto& slot = m_state->m_renderTextures[name];
        // 今回の新規登録有無
        const bool createdTarget = !slot;
        try
        {
            if (!slot)
            {
                slot = std::make_unique<RenderTarget>();
            }
            // 通常の描画先とは別名にし、UAV指定を資源作成前に設定します。
            slot->SetComputeWritable(true);
            ResizeOffscreenTarget(
                *slot,
                safeWidth,
                safeHeight);
            return *slot;
        }
        catch (...)
        {
            if (createdTarget)
            {
                m_state->m_renderTextures.erase(name);
            }
            throw;
        }
    }

    // 描画先を借用し、未登録ならnullptrです(name: 登録名)。
    const RenderTarget* GraphicsDevice::FindRenderTexture(
        const std::string& name) const noexcept
    {
        // 名前に対応する描画先登録
        const auto entry = m_state->m_renderTextures.find(name);
        if (entry == m_state->m_renderTextures.end())
        {
            return nullptr;
        }
        return entry->second.get();
    }

    // 表示画像を返し、無効な世代なら空です(name: 描画先の登録名)。
    GraphicsViewHandle GraphicsDevice::RenderTextureViewHandle(
        const std::string& name) const noexcept
    {
        // 名前で検索した描画先
        const auto* const target = FindRenderTexture(name);
        if (target == nullptr || !target->IsValid())
        {
            return {};
        }
        // 表示画像のビュー
        auto view = target->DisplayViewHandle();
        return IsGraphicsViewCurrent(view)
            ? view
            : GraphicsViewHandle{};
    }

    // 描画先の登録を解放したか返します(name: 登録名)。
    bool GraphicsDevice::ReleaseRenderTexture(
        const std::string& name)
    {
        return m_state->m_renderTextures.erase(name) > 0;
    }

    // 登録した全ての描画先を解放します。
    void GraphicsDevice::ClearRenderTextures() noexcept
    {
        m_state->m_renderTextures.clear();
    }

    // 登録した描画先の名前を名前順で返します。
    std::vector<std::string>
        GraphicsDevice::RenderTextureNames() const
    {
        // 名前順に返す描画先一覧
        std::vector<std::string> names;
        names.reserve(m_state->m_renderTextures.size());
        // 登録した描画先の名前と所有資源
        for (const auto& [name, target] : m_state->m_renderTextures)
        {
            static_cast<void>(target);
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    // シーンのHDR描画先を設定して消去します(clearColor: RGBAの消去色)。
    void GraphicsDevice::BeginSceneComposition(
        const float clearColor[4])
    {
        ResizeOffscreenTarget(
            *m_state->m_sceneCompositionTarget,
            RenderWidth(),
            RenderHeight());
        BeginOffscreenTarget(
            *m_state->m_sceneCompositionTarget,
            clearColor);
    }

    // シーンを後処理して画面へ合成します(bloom: ブルーム設定, colorGrading: 色調補正設定)。
    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ColorGradingSettings& colorGrading)
    {
        EndSceneComposition(
            bloom,
            ScreenSpaceLensFlareSettings{},
            colorGrading,
            VolumetricLightFrame{},
            TemporalAntiAliasingFrame{});
    }

    // シーンを後処理して画面へ合成します(bloom: ブルーム設定, lensFlare: レンズフレア設定, colorGrading: 色調補正設定)。
    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ScreenSpaceLensFlareSettings& lensFlare,
        const ColorGradingSettings& colorGrading)
    {
        EndSceneComposition(
            bloom,
            lensFlare,
            colorGrading,
            VolumetricLightFrame{},
            TemporalAntiAliasingFrame{});
    }

    // シーンを後処理して画面へ合成します(bloom: ブルーム設定, colorGrading: 色調補正設定, volumetric: 光の筋のフレーム入力, temporal: TAAのフレーム入力)。
    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric,
        const TemporalAntiAliasingFrame& temporal)
    {
        EndSceneComposition(
            bloom,
            ScreenSpaceLensFlareSettings{},
            colorGrading,
            volumetric,
            temporal);
    }

    // シーンを後処理して画面へ合成します(bloom: ブルーム設定, lensFlare: レンズフレア設定, colorGrading: 色調補正設定, volumetric: 光の筋のフレーム入力, temporal: TAAのフレーム入力)。
    void GraphicsDevice::EndSceneComposition(
        const BloomSettings& bloom,
        const ScreenSpaceLensFlareSettings& lensFlare,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric,
        const TemporalAntiAliasingFrame& temporal)
    {
        // 集約するフレームの後処理設定
        PostProcessFrame frame{};
        frame.bloom = bloom;
        frame.lensFlare = lensFlare;
        frame.colorGrading = colorGrading;
        frame.volumetric = volumetric;
        frame.temporal = temporal;
        EndSceneComposition(frame);
    }

    // 登録効果を含む後処理を実行して画面へ合成します(frame: フレームの後処理設定と入力)。
    void GraphicsDevice::EndSceneComposition(
        const PostProcessFrame& frame)
    {
        // 追加効果を後処理中に適用します(target: 処理中の描画先, point: 効果の挿入位置)。
        // 計測区間はRunPostProcessが開始します。
        RunPostProcess(
            *this,
            *m_state->m_sceneCompositionTarget,
            frame,
            [this](
                RenderTarget& target,
                const ScreenEffectPoint point)
            {
                ApplyQueuedScreenEffects(target, point);
            });
        // バックバッファ転送の計測区間
        GpuProfiler::SectionScope transferSection{
            m_state->m_gpuProfiler,
            "画面へ転送"
        };

        CopyOffscreenTargetToBackBuffer(
            *m_state->m_sceneCompositionTarget);
    }
}
