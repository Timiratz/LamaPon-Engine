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
        SpriteRenderPassState(
            std::shared_ptr<GraphicsDeviceResourceLeaseState> gate,
            GraphicsDeviceResourceLease lease) noexcept
            : m_gate(std::move(gate))
            , m_lease(std::move(lease))
            , m_renderThread(std::this_thread::get_id())
        {
        }

        ~SpriteRenderPassState() noexcept
        {
            Abort();
        }

        SpriteRenderPassState(
            const SpriteRenderPassState&) = delete;
        SpriteRenderPassState& operator=(
            const SpriteRenderPassState&) = delete;

        [[nodiscard]] SpriteShaderStatus& MutableStatus() noexcept
        {
            return m_status;
        }

        [[nodiscard]] SpriteShaderStatus Status() const
        {
            if (m_gate == nullptr)
            {
                return {};
            }
            std::scoped_lock lock(m_gate->mutex);
            return m_status;
        }

        // d3d12Rendererはpassのleaseが再初期化を拒否する間だけ生存する
        // DirectX 12 driverです。nullptrならD3D11 driverへ送ります。
        void Activate(
            const std::uint64_t token,
            D3D12SpriteRenderer* const d3d12Renderer = nullptr) noexcept
        {
            m_token = token;
            m_d3d12Renderer = d3d12Renderer;
            m_phase = Phase::Active;
        }

        [[nodiscard]] bool Active() const noexcept
        {
            if (m_gate == nullptr)
            {
                return false;
            }
            try
            {
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

        bool Draw(const SpriteDrawRequest& request)
        {
            if (m_gate == nullptr)
            {
                return false;
            }
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

        bool PushScissor(
            const SpriteClipRectangle& rectangle)
        {
            if (m_gate == nullptr)
            {
                return false;
            }
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

        bool PopScissor()
        {
            if (m_gate == nullptr)
            {
                return false;
            }
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

        void End()
        {
            if (m_gate == nullptr)
            {
                return;
            }
            bool releaseLease{};
            std::exception_ptr failure;
            {
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
                    // A wrong-thread call is a recoverable usage error: leave
                    // the pass active so its render thread can still end it.
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
                        // Context copies observe m_phase under this same gate.
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

        void Abort() noexcept
        {
            if (m_gate == nullptr)
            {
                return;
            }
            bool releaseLease{};
            try
            {
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
            // Immediate Contextは別threadから閉じません。誤用時は現在の
            // API stateを開始不可のまま残し、再初期化だけを回復経路に
            // します。
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

        void RequireRenderThread() const
        {
            if (std::this_thread::get_id() != m_renderThread)
            {
                throw std::logic_error(
                    "A sprite render pass must be used from the thread "
                    "that began it.");
            }
        }

        std::shared_ptr<GraphicsDeviceResourceLeaseState> m_gate;
        GraphicsDeviceResourceLease m_lease;
        std::thread::id m_renderThread;
        std::uint64_t m_token{};
        D3D12SpriteRenderer* m_d3d12Renderer{};
        Phase m_phase{ Phase::Prepared };
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
        const auto state = m_state.lock();
        return state != nullptr && state->Active();
    }

    bool SpriteDrawContext::Draw(
        const SpriteDrawRequest& request) const
    {
        const auto state = m_state.lock();
        return state != nullptr && state->Draw(request);
    }

    bool SpriteDrawContext::PushScissor(
        const SpriteClipRectangle& rectangle) const
    {
        const auto state = m_state.lock();
        return state != nullptr && state->PushScissor(rectangle);
    }

    bool SpriteDrawContext::PopScissor() const
    {
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

    SpriteRenderPass GraphicsDevice::BeginSpritePass(
        const SpritePassDescription& description)
    {
        // 実効APIのSprite driverを選びます。D3D12 driverはAPI資源が所有し、
        // passのleaseが再初期化を拒否する間だけ参照します。
        auto state = std::make_shared<
            Detail::SpriteRenderPassState>(
                m_state->m_resourceLeaseState,
                AcquireResourceLease());
        {
            std::scoped_lock lock(m_state->m_resourceLeaseState->mutex);
            if (m_state->m_resourceLeaseState->closed
                || m_state->m_resourceLeaseState->owner != this)
            {
                throw std::logic_error(
                    "GraphicsDevice is shutting down.");
            }
            if (auto* const d3d12Resources = dynamic_cast<
                    Detail::GraphicsDeviceD3D12Resources*>(
                        m_state->m_apiResources.get()))
            {
                auto* const renderer =
                    d3d12Resources->TrySpriteRenderer();
                if (renderer == nullptr)
                {
                    throw std::logic_error(
                        "The DirectX 12 sprite renderer is not initialized.");
                }
                const auto token = renderer->Begin(
                    description,
                    m_state->m_whiteTextureView,
                    state->MutableStatus());
                state->Activate(token, renderer);
            }
            else
            {
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
