#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "LamaPon/Reactive/Reactive.h"

namespace LamaPon
{
    class GameObject;

    // Publishで届くイベントの内容。テンプレートではなく
    // 固定の入れ物なので、必要な項目だけ埋めて使います。
    struct EventArgs final
    {
        GameObject* sender{};
        float number{};
        std::string text;
    };

    // 名前付きイベントバス。
    // 「ボタンが押された」「敵が倒された」のような出来事を
    // 名前で購読・発行でき、コンポーネント同士が直接参照せずに
    // 連携できます。Sceneが1つ保持し、Scene切り替えでは
    // 消えません。Script::Onの購読はScript破棄時に解除されます。
    // SubscribeのハンドルはUnsubscribeで、Observeの購読は返された
    // Subscriptionの破棄で解除します。操作は同じスレッドで行います。
    class EventBus final
    {
    public:
        using Handler =
            std::function<void(const EventArgs&)>;

        EventBus();
        ~EventBus();
        EventBus(const EventBus&) = delete;
        EventBus& operator=(const EventBus&) = delete;
        EventBus(EventBus&&) = delete;
        EventBus& operator=(EventBus&&) = delete;

        // 購読を開始し、解除用ハンドルを返します。
        std::uint64_t Subscribe(
            std::string_view eventName,
            Handler handler);
        void Unsubscribe(std::uint64_t handle) noexcept;
        // 同名イベントの全ハンドラーを呼びます。Publish中に追加した
        // 購読は次回から有効です。購読解除にも対応します。例外時は
        // 内部状態を復元して呼び出し元へ例外を返し、処理を中断します。
        void Publish(
            std::string_view eventName,
            const EventArgs& eventArgs = {});
        // 名前付きイベントをリアクティブストリームとして購読します。
        [[nodiscard]] Observable<EventArgs> Observe(
            std::string_view eventName);
        void Clear() noexcept;
        [[nodiscard]] std::size_t
            SubscriptionCount() const noexcept;

    private:
        void FinishPublish() noexcept;

        struct Subscription final
        {
            std::uint64_t id{};
            std::string eventName;
            Handler handler;
        };

        std::vector<Subscription> m_subscriptions;
        std::uint64_t m_nextId{ 1 };
        int m_publishDepth{};
        bool m_needsCompaction{};
    };
}
