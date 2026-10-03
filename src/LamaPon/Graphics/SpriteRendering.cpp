#include "LamaPon/Graphics/SpriteRendering.h"

#include "LamaPon/Graphics/D3D12SpriteRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D12Resources.h"
#include "LamaPon/Graphics/GraphicsDeviceResourceLease.h"
#include "LamaPon/Graphics/GraphicsDeviceResourceLeaseState.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace LamaPon::Detail
{
    class SpriteRenderPassState final
    {
    public:
        // デバイス資源を保持してパス状態を作ります(gate: デバイス寿命の共有ゲート, lease: 再初期化を防ぐ資源リース)。
        SpriteRenderPassState(
            std::shared_ptr<GraphicsDeviceResourceLeaseState> gate,
            GraphicsDeviceResourceLease lease) noexcept
            : m_gate(std::move(gate))
            , m_lease(std::move(lease))
            , m_renderThread(std::this_thread::get_id())
        {
        }

        // 例外を外へ出さず描画パスを終了します。
        ~SpriteRenderPassState() noexcept
        {
            Abort();
        }

        // パス状態のコピーを禁止します。
        SpriteRenderPassState(
            const SpriteRenderPassState&) = delete;
        // パス状態のコピー代入を禁止します。
        SpriteRenderPassState& operator=(
            const SpriteRenderPassState&) = delete;

        // 開始処理が設定するシェーダー状態を返します。
        [[nodiscard]] SpriteShaderStatus& MutableStatus() noexcept
        {
            return m_status;
        }

        // 寿命ゲートで保護したシェーダー状態のコピーを返します。
        [[nodiscard]] SpriteShaderStatus Status() const
        {
            if (m_gate == nullptr)
            {
                return {};
            }
            // デバイス寿命の排他ロック
            std::scoped_lock lock(m_gate->mutex);
            return m_status;
        }

        // パスを使用中にします(token: 描画パス番号, d3d12Renderer: 借用する描画器で空ならD3D11)。
        void Activate(
            const std::uint64_t token,
            D3D12SpriteRenderer* const d3d12Renderer = nullptr) noexcept
        {
            m_token = token;
            m_d3d12Renderer = d3d12Renderer;
            m_phase = Phase::Active;
        }

        // パスとデバイスが有効か確認します。
        [[nodiscard]] bool Active() const noexcept
        {
            if (m_gate == nullptr)
            {
                return false;
            }
            try
            {
                // デバイス寿命の排他ロック
                std::scoped_lock lock(m_gate->mutex);
                return m_phase == Phase::Active
                    && !m_gate->closed
                    && m_gate->owner != nullptr;
            }
            catch (...)
            {
                return false;
            }
        }

        // 開始スレッドから描画器へ画像を送ります(request: 画像と位置・色・変形の指定)。
        bool Draw(const SpriteDrawRequest& request)
        {
            if (m_gate == nullptr)
            {
                return false;
            }
            // デバイス寿命の排他ロック
            std::scoped_lock lock(m_gate->mutex);
            if (m_phase != Phase::Active
                || m_gate->closed
                || m_gate->owner == nullptr)
            {
                return false;
            }
            RequireRenderThread();
            if (m_d3d12Renderer != nullptr)
            {
                return m_d3d12Renderer->Draw(m_token, request);
            }
            return m_gate->owner->DrawD3D11Sprite(
                m_token,
                request);
        }

        // 開始スレッドからクリップ範囲を積みます(rectangle: ピクセル座標の矩形)。
        bool PushScissor(
            const SpriteClipRectangle& rectangle)
        {
            if (m_gate == nullptr)
            {
                return false;
            }
            // デバイス寿命の排他ロック
            std::scoped_lock lock(m_gate->mutex);
            if (m_phase != Phase::Active
                || m_gate->closed
                || m_gate->owner == nullptr)
            {
                return false;
            }
            RequireRenderThread();
            if (m_d3d12Renderer != nullptr)
            {
                return m_d3d12Renderer->PushScissor(m_token, rectangle);
            }
            return m_gate->owner->PushD3D11SpriteScissor(
                m_token,
                rectangle);
        }

        // 開始スレッドから直前のクリップ範囲へ戻します。
        bool PopScissor()
        {
            if (m_gate == nullptr)
            {
                return false;
            }
            // デバイス寿命の排他ロック
            std::scoped_lock lock(m_gate->mutex);
            if (m_phase != Phase::Active
                || m_gate->closed
                || m_gate->owner == nullptr)
            {
                return false;
            }
            RequireRenderThread();
            if (m_d3d12Renderer != nullptr)
            {
                return m_d3d12Renderer->PopScissor(m_token);
            }
            return m_gate->owner->PopD3D11SpriteScissor(
                m_token);
        }

        // 開始スレッドで終了し、リース解放後に送信例外を伝えます。
        void End()
        {
            if (m_gate == nullptr)
            {
                return;
            }
            // 終了後のリース解放有無
            bool releaseLease{};
            // 終了処理で保存した例外
            std::exception_ptr failure;
            {
                // デバイス寿命の排他ロック
                std::scoped_lock lock(m_gate->mutex);
                if (m_phase != Phase::Active)
                {
                    return;
                }
                releaseLease = true;
                if (m_gate->closed || m_gate->owner == nullptr)
                {
                    m_phase = Phase::Failed;
                }
                else
                {
                    // 異なるスレッドでは終了せず、開始スレッドから終了できる状態を保ちます。
                    RequireRenderThread();
                    try
                    {
                        if (m_d3d12Renderer != nullptr)
                        {
                            m_d3d12Renderer->End(m_token);
                        }
                        else
                        {
                            m_gate->owner->EndD3D11SpritePass(m_token);
                        }
                        m_phase = Phase::Ended;
                    }
                    catch (...)
                    {

                        m_phase = Phase::Failed;
                        failure = std::current_exception();
                    }
                }
            }
            if (releaseLease)
            {
                m_lease.Reset();
            }
            if (failure != nullptr)
            {
                std::rethrow_exception(failure);
            }
        }

        // 例外を外へ出さず終了してリースを解放します。
        void Abort() noexcept
        {
            if (m_gate == nullptr)
            {
                return;
            }
            // 終了後のリース解放有無
            bool releaseLease{};
            try
            {
                // デバイス寿命の排他ロック
                std::scoped_lock lock(m_gate->mutex);
                if (m_phase != Phase::Active)
                {
                    return;
                }
                releaseLease = true;
                if (!m_gate->closed
                    && m_gate->owner != nullptr
                    && std::this_thread::get_id() == m_renderThread)
                {
                    if (m_d3d12Renderer != nullptr)
                    {
                        m_d3d12Renderer->Abort(m_token);
                    }
                    else
                    {
                        m_gate->owner->AbortD3D11SpritePass(m_token);
                    }
                }
                m_phase = Phase::Failed;
            }
            catch (...)
            {
                releaseLease = true;
            }
            // 別スレッドから破棄したパスは描画開始を拒否したまま残り、再初期化で回復します。
            if (releaseLease)
            {
                m_lease.Reset();
            }
        }

    private:
        enum class Phase : std::uint8_t
        {
            Prepared,
            Active,
            Ended,
            Failed
        };

        // 開始スレッド以外ならlogic_errorを投げます。
        void RequireRenderThread() const
        {
            if (std::this_thread::get_id() != m_renderThread)
            {
                throw std::logic_error(
                    "A sprite render pass must be used from the thread "
                    "that began it.");
            }
        }

        // デバイス寿命の共有ゲート
        std::shared_ptr<GraphicsDeviceResourceLeaseState> m_gate;
        // 再初期化を防ぐ資源リース
        GraphicsDeviceResourceLease m_lease;
        // 描画を開始したスレッド
        std::thread::id m_renderThread;
        // 描画パスの識別番号
        std::uint64_t m_token{};
        // リース中だけ借用する描画器
        D3D12SpriteRenderer* m_d3d12Renderer{};
        // 描画パスの進行状態
        Phase m_phase{ Phase::Prepared };
        // シェーダーの使用状態
        SpriteShaderStatus m_status;
    };
}

namespace LamaPon
{
    SpriteDrawContext::SpriteDrawContext(
        std::weak_ptr<Detail::SpriteRenderPassState>
            state) noexcept
        : m_state(std::move(state))
    {
    }

    SpriteDrawContext::operator bool() const noexcept
    {
        // 描画パスの共有状態
        const auto state = m_state.lock();
        return state != nullptr && state->Active();
    }

    bool SpriteDrawContext::Draw(
        const SpriteDrawRequest& request) const
    {
        // 描画パスの共有状態
        const auto state = m_state.lock();
        return state != nullptr && state->Draw(request);
    }

    bool SpriteDrawContext::PushScissor(
        const SpriteClipRectangle& rectangle) const
    {
        // 描画パスの共有状態
        const auto state = m_state.lock();
        return state != nullptr && state->PushScissor(rectangle);
    }

    bool SpriteDrawContext::PopScissor() const
    {
        // 描画パスの共有状態
        const auto state = m_state.lock();
        return state != nullptr && state->PopScissor();
    }

    SpriteRenderPass::SpriteRenderPass(
        std::shared_ptr<Detail::SpriteRenderPassState>
            state) noexcept
        : m_state(std::move(state))
    {
    }

    SpriteRenderPass::~SpriteRenderPass() noexcept
    {
        Abort();
    }

    SpriteRenderPass::SpriteRenderPass(
        SpriteRenderPass&& other) noexcept
        : m_state(std::move(other.m_state))
    {
    }

    SpriteRenderPass& SpriteRenderPass::operator=(
        SpriteRenderPass&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        Abort();
        m_state = std::move(other.m_state);
        return *this;
    }

    SpriteRenderPass::operator bool() const noexcept
    {
        return m_state != nullptr && m_state->Active();
    }

    SpriteDrawContext SpriteRenderPass::Context() const noexcept
    {
        return SpriteDrawContext{ m_state };
    }

    SpriteShaderStatus SpriteRenderPass::ShaderStatus() const
    {
        return m_state != nullptr
            ? m_state->Status()
            : SpriteShaderStatus{};
    }

    bool SpriteRenderPass::Draw(
        const SpriteDrawRequest& request) const
    {
        return m_state != nullptr && m_state->Draw(request);
    }

    bool SpriteRenderPass::PushScissor(
        const SpriteClipRectangle& rectangle) const
    {
        return m_state != nullptr
            && m_state->PushScissor(rectangle);
    }

    bool SpriteRenderPass::PopScissor() const
    {
        return m_state != nullptr && m_state->PopScissor();
    }

    void SpriteRenderPass::End()
    {
        if (m_state != nullptr)
        {
            m_state->End();
        }
    }

    void SpriteRenderPass::Abort() noexcept
    {
        if (m_state != nullptr)
        {
            m_state->Abort();
        }
    }

    // 実効APIで描画パスを開始します(description: 合成方式・シェーダー・定数・ライト情報)。
    SpriteRenderPass GraphicsDevice::BeginSpritePass(
        const SpritePassDescription& description)
    {

        // 描画パスの共有状態
        auto state = std::make_shared<
            Detail::SpriteRenderPassState>(
                m_state->m_resourceLeaseState,
                AcquireResourceLease());
        {
            // デバイス寿命の排他ロック
            std::scoped_lock lock(m_state->m_resourceLeaseState->mutex);
            if (m_state->m_resourceLeaseState->closed
                || m_state->m_resourceLeaseState->owner != this)
            {
                throw std::logic_error(
                    "GraphicsDevice is shutting down.");
            }
            // 実効APIのD3D12資源
            if (auto* const d3d12Resources = dynamic_cast<
                    Detail::GraphicsDeviceD3D12Resources*>(
                        m_state->m_apiResources.get()))
            {
                // 借用するD3D12描画器
                auto* const renderer =
                    d3d12Resources->TrySpriteRenderer();
                if (renderer == nullptr)
                {
                    throw std::logic_error(
                        "The DirectX 12 sprite renderer is not initialized.");
                }
                // 開始したパスの識別番号
                const auto token = renderer->Begin(
                    description,
                    m_state->m_whiteTextureView,
                    &Assets(),
                    state->MutableStatus());
                state->Activate(token, renderer);
            }
            else
            {
                // 開始したパスの識別番号
                const auto token = BeginD3D11SpritePass(
                    description,
                    true,
                    &state->MutableStatus(),
                    nullptr,
                    nullptr);
                state->Activate(token);
            }
        }
        return SpriteRenderPass{ std::move(state) };
    }
}
