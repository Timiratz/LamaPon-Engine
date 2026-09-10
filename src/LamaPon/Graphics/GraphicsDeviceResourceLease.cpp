#include "LamaPon/Graphics/GraphicsDeviceResourceLease.h"

#include "LamaPon/Graphics/GraphicsDevice.h"
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
        auto state = std::move(m_state);
        if (!state)
        {
            return;
        }
        try
        {
            std::scoped_lock lock(state->mutex);
            if (state->activeLeases > 0)
            {
                --state->activeLeases;
            }
        }
        catch (...)
        {
            // noexcept destructorから例外を出さないための最終防壁です。
            // std::mutexのlock失敗以外では到達しません。
        }
    }

    std::shared_ptr<Detail::GraphicsDeviceResourceLeaseState>
        GraphicsDevice::CreateResourceLeaseState(
            GraphicsDevice* const owner)
    {
        return std::make_shared<
            Detail::GraphicsDeviceResourceLeaseState>(owner);
    }

    GraphicsDeviceResourceLease
        GraphicsDevice::AcquireResourceLease()
    {
        auto state = m_resourceLeaseState;
        if (!state)
        {
            throw std::logic_error(
                "GraphicsDevice resource lease state is unavailable.");
        }
        {
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

    void GraphicsDevice::BeginResourceTransition()
    {
        auto state = m_resourceLeaseState;
        if (!state)
        {
            throw std::logic_error(
                "GraphicsDevice resource lease state is unavailable.");
        }
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

    void GraphicsDevice::EndResourceTransition() noexcept
    {
        const auto state = m_resourceLeaseState;
        if (!state)
        {
            return;
        }
        try
        {
            std::scoped_lock lock(state->mutex);
            state->transitionInProgress = false;
        }
        catch (...)
        {
        }
    }

    void GraphicsDevice::CloseResourceLeaseGate() noexcept
    {
        const auto state = m_resourceLeaseState;
        if (!state)
        {
            return;
        }
        try
        {
            {
                std::scoped_lock lock(state->mutex);
                state->closed = true;
                state->transitionInProgress = false;
            }
            // closedを先に公開するので、新しいpass操作はownerへ入りません。
            // 進行中だった操作は上のlock取得時点で完了しています。
            AbortActiveD3D11SpritePass();
            std::scoped_lock lock(state->mutex);
            state->owner = nullptr;
        }
        catch (...)
        {
            try
            {
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
