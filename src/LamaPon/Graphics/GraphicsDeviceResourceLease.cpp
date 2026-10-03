#include "LamaPon/Graphics/GraphicsDeviceResourceLease.h"

#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"
#include "LamaPon/Graphics/GraphicsDeviceResourceLeaseState.h"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace LamaPon
{
    GraphicsDeviceResourceLease::GraphicsDeviceResourceLease(
        std::shared_ptr<
            Detail::GraphicsDeviceResourceLeaseState> state) noexcept
        : m_state(std::move(state))
    {
    }

    GraphicsDeviceResourceLease::~GraphicsDeviceResourceLease() noexcept
    {
        Reset();
    }

    GraphicsDeviceResourceLease::GraphicsDeviceResourceLease(
        GraphicsDeviceResourceLease&& other) noexcept
        : m_state(std::move(other.m_state))
    {
    }

    GraphicsDeviceResourceLease&
        GraphicsDeviceResourceLease::operator=(
            GraphicsDeviceResourceLease&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        Reset();
        m_state = std::move(other.m_state);
        return *this;
    }

    void GraphicsDeviceResourceLease::Reset() noexcept
    {
        // 操作中に保持する共有寿命ゲート
        auto state = std::move(m_state);
        if (!state)
        {
            return;
        }
        try
        {
            // ゲート状態の排他制御
            std::scoped_lock lock(state->mutex);
            if (state->activeLeases > 0)
            {
                --state->activeLeases;
            }
        }
        catch (...)
        {
            // 解放時のロック失敗を破棄処理から送出しない。
        }
    }

    // 新しいデバイスの共有ゲートを作成する(owner: 借用する所有デバイス)。
    std::shared_ptr<Detail::GraphicsDeviceResourceLeaseState>
        GraphicsDevice::CreateResourceLeaseState(
            GraphicsDevice* const owner)
    {
        return std::make_shared<
            Detail::GraphicsDeviceResourceLeaseState>(owner);
    }

    // 終了・再初期化中を拒否し、リース数を増やして共有ゲートを渡す。
    GraphicsDeviceResourceLease
        GraphicsDevice::AcquireResourceLease()
    {
        // 操作中に保持する共有寿命ゲート
        auto state = m_state->m_resourceLeaseState;
        if (!state)
        {
            throw std::logic_error(
                "GraphicsDevice resource lease state is unavailable.");
        }
        {
            // ゲート状態の排他制御
            std::scoped_lock lock(state->mutex);
            if (state->closed)
            {
                throw std::logic_error(
                    "GraphicsDevice is shutting down.");
            }
            if (state->transitionInProgress)
            {
                throw std::logic_error(
                    "GraphicsDevice is being initialized.");
            }
            ++state->activeLeases;
        }
        return GraphicsDeviceResourceLease{
            std::move(state) };
    }

    // リースの不在を確認して再初期化へ入り、遷移できない場合はlogic_errorを送出する。
    void GraphicsDevice::BeginResourceTransition()
    {
        // 操作中に保持する共有寿命ゲート
        auto state = m_state->m_resourceLeaseState;
        if (!state)
        {
            throw std::logic_error(
                "GraphicsDevice resource lease state is unavailable.");
        }
        // ゲート状態の排他制御
        std::scoped_lock lock(state->mutex);
        if (state->closed)
        {
            throw std::logic_error(
                "GraphicsDevice is shutting down.");
        }
        if (state->transitionInProgress)
        {
            throw std::logic_error(
                "GraphicsDevice initialization is already in progress.");
        }
        if (state->activeLeases != 0)
        {
            throw std::logic_error(
                "GraphicsDevice cannot be initialized while a Scene or "
                "another external graphics resource owner is alive. "
                "Destroy those owners and restart the editor or game.");
        }
        state->transitionInProgress = true;
    }

    // 再初期化中のフラグを解除する。
    void GraphicsDevice::EndResourceTransition() noexcept
    {
        // 操作中に保持する共有寿命ゲート
        const auto state = m_state->m_resourceLeaseState;
        if (!state)
        {
            return;
        }
        try
        {
            // ゲート状態の排他制御
            std::scoped_lock lock(state->mutex);
            state->transitionInProgress = false;
        }
        catch (...)
        {
        }
    }

    // ゲートを閉じて進行中のパスを中止し、所有デバイスの参照を解除する。
    void GraphicsDevice::CloseResourceLeaseGate() noexcept
    {
        // 操作中に保持する共有寿命ゲート
        const auto state = m_state->m_resourceLeaseState;
        if (!state)
        {
            return;
        }
        try
        {
            {
                // ゲート状態の排他制御
                std::scoped_lock lock(state->mutex);
                state->closed = true;
                state->transitionInProgress = false;
            }
            // 既存操作の完了後にclosedを公開し、ロック外でパスを中止してからownerを解除する。
            AbortActiveD3D11SpritePass();
            // ゲート状態の排他制御
            std::scoped_lock lock(state->mutex);
            state->owner = nullptr;
        }
        catch (...)
        {
            try
            {
                // ゲート状態の排他制御
                std::scoped_lock lock(state->mutex);
                state->owner = nullptr;
                state->closed = true;
                state->transitionInProgress = false;
            }
            catch (...)
            {
            }
        }
    }
}
