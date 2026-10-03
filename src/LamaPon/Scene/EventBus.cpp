#include "LamaPon/Scene/EventBus.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>

namespace
{
    // バスの寿命レジストリを返します。
    // ABIを変えず外部に寿命を保持し、静的破棄順を避けるためレジストリ自体は終了まで残します。
    auto& EventBusLifetimes()
    {
        using Registry = std::unordered_map<
            const LamaPon::EventBus*,
            std::shared_ptr<void>>;
        // 終了まで保持する寿命レジストリ
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
        // 今回の購読へ割り当てる番号
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
        // 解除または無効化する購読
        for (auto& subscription : m_subscriptions)
        {
            if (subscription.id != handle
                || subscription.id == 0)
            {
                continue;
            }
            if (m_publishDepth > 0)
            {
                // 発行中は走査の番号を保つため、購読を無効化して最外周で回収します。
                subscription.id = 0;
                subscription.handler = nullptr;
                m_needsCompaction = true;
            }
            else
            {
                // 解除対象の購読か調べます(entry: 登録済み購読)。
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
        // 今回の発行開始時に存在する購読数
        const std::size_t count =
            m_subscriptions.size();
        ++m_publishDepth;
        try
        {
            // 今回通知する登録済み購読番号
            for (std::size_t index = 0;
                index < count;
                ++index)
            {
                // 今回通知する購読
                auto& subscription = m_subscriptions[index];
                if (subscription.id == 0
                    || subscription.eventName != eventName
                    || subscription.handler == nullptr)
                {
                    continue;
                }
                // 自身の解除でも呼び出しを保つ受信関数
                const auto handler = subscription.handler;
                handler(eventArgs);
            }
        }
        catch (...)
        {
            // 受信関数の例外でも入れ子の深さを戻し、最外周の解除済み購読を回収します。
            FinishPublish();
            throw;
        }
        FinishPublish();
    }

    void EventBus::FinishPublish() noexcept
    {
        --m_publishDepth;
        if (m_publishDepth == 0 && m_needsCompaction)
        {
            m_needsCompaction = false;
            // 無効化済みの購読を回収します(entry: 登録済み購読)。
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
        // 購読口が所有するイベント名
        const std::string name{ eventName };
        // バス破棄を検知する非所有の寿命参照
        const std::weak_ptr<void> lifetime =
            EventBusLifetimes().at(this);
        // バスが生存中なら購読を登録します(observer: イベントの受信関数)。
        return Observable<EventArgs>{
            [this, name, lifetime](
                Observable<EventArgs>::Observer observer)
            {
                if (lifetime.expired() || observer == nullptr)
                {
                    return LamaPon::Subscription{};
                }
                // 登録したイベントの購読番号
                const auto handle = Subscribe(name, std::move(observer));
                if (handle == 0)
                {
                    return LamaPon::Subscription{};
                }
                // 生存中のバスに登録した購読を解除します。
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
            // 発行中に無効化する購読
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
        // 解除されていない購読の件数
        std::size_t count = 0;
        // 有効性を数える登録済み購読
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
