#include "LamaPon/Scene/EventBus.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>

namespace
{
    // EventBus本体のサイズを変えず、Observe購読が破棄済みBusへ
    // アクセスしないための寿命レジストリです。
    auto& EventBusLifetimes()
    {
        using Registry = std::unordered_map<
            const LamaPon::EventBus*,
            std::shared_ptr<void>>;
        static auto* registry = new Registry{};
        return *registry;
    }
}

namespace LamaPon
{
    EventBus::EventBus()
    {
        EventBusLifetimes().emplace(
            this,
            std::make_shared<int>(0));
    }

    EventBus::~EventBus()
    {
        EventBusLifetimes().erase(this);
    }

    std::uint64_t EventBus::Subscribe(
        const std::string_view eventName,
        Handler handler)
    {
        if (eventName.empty() || handler == nullptr)
        {
            return 0;
        }
        const auto id = m_nextId++;
        m_subscriptions.push_back({
            id,
            std::string{ eventName },
            std::move(handler) });
        return id;
    }

    void EventBus::Unsubscribe(
        const std::uint64_t handle) noexcept
    {
        for (auto& subscription : m_subscriptions)
        {
            if (subscription.id != handle
                || subscription.id == 0)
            {
                continue;
            }
            if (m_publishDepth > 0)
            {
                // Publish中はハンドラーだけ無効化し、
                // 走査が終わってから取り除きます。
                subscription.id = 0;
                subscription.handler = nullptr;
                m_needsCompaction = true;
            }
            else
            {
                std::erase_if(
                    m_subscriptions,
                    [handle](const Subscription& entry)
                    {
                        return entry.id == handle;
                    });
            }
            return;
        }
    }

    void EventBus::Publish(
        const std::string_view eventName,
        const EventArgs& eventArgs)
    {
        // Publish中に追加された購読は今回の発行では呼びません
        // （購読→発行の連鎖による無限ループを防ぎます）。
        const std::size_t count =
            m_subscriptions.size();
        ++m_publishDepth;
        for (std::size_t index = 0;
            index < count;
            ++index)
        {
            auto& subscription = m_subscriptions[index];
            if (subscription.id == 0
                || subscription.eventName != eventName
                || subscription.handler == nullptr)
            {
                continue;
            }
            // ハンドラーが自分自身をUnsubscribeしても安全な
            // ようにコピーしてから呼びます。
            const auto handler = subscription.handler;
            handler(eventArgs);
        }
        --m_publishDepth;
        if (m_publishDepth == 0 && m_needsCompaction)
        {
            m_needsCompaction = false;
            std::erase_if(
                m_subscriptions,
                [](const Subscription& entry)
                {
                    return entry.id == 0;
                });
        }
    }

    Observable<EventArgs> EventBus::Observe(
        const std::string_view eventName)
    {
        const std::string name{ eventName };
        const std::weak_ptr<void> lifetime =
            EventBusLifetimes().at(this);
        return Observable<EventArgs>{
            [this, name, lifetime](
                Observable<EventArgs>::Observer observer)
            {
                if (lifetime.expired() || observer == nullptr)
                {
                    return LamaPon::Subscription{};
                }
                const auto handle = Subscribe(name, std::move(observer));
                if (handle == 0)
                {
                    return LamaPon::Subscription{};
                }
                return LamaPon::Subscription{
                    [this, lifetime, handle]()
                    {
                        if (!lifetime.expired())
                        {
                            Unsubscribe(handle);
                        }
                    } };
            } };
    }

    void EventBus::Clear() noexcept
    {
        if (m_publishDepth > 0)
        {
            // Publish中は無効化のみ行います。
            for (auto& subscription : m_subscriptions)
            {
                subscription.id = 0;
                subscription.handler = nullptr;
            }
            m_needsCompaction = true;
            return;
        }
        m_subscriptions.clear();
    }

    std::size_t
        EventBus::SubscriptionCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& subscription : m_subscriptions)
        {
            if (subscription.id != 0)
            {
                ++count;
            }
        }
        return count;
    }
}
