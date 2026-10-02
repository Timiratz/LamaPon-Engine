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

    // senderはイベント処理中だけ有効な非所有参照です。
    struct EventArgs final
    {
        // イベント発行元の非所有参照
        GameObject* sender{};
        // 任意の数値ペイロード
        float number{};
        // 任意の文字列ペイロード
        std::string text;
    };

    // 全操作は同じスレッドで行い、Sceneの読み替えではバスを保持します。
    // Script::OnはScriptの破棄で、Observeは返すSubscriptionの破棄で購読を解除します。
    class EventBus final
    {
    public:
        // 発行されたイベントの受信関数
        using Handler =
            std::function<void(const EventArgs&)>;

        // リアクティブ購読用の寿命情報を登録します。
        EventBus();
        // 寿命情報を解除し購読を破棄します。
        ~EventBus();
        // 寿命情報と購読の重複を防ぐためコピーを禁止します。
        EventBus(const EventBus&) = delete;
        // 寿命情報と購読の重複を防ぐためコピー代入を禁止します。
        EventBus& operator=(const EventBus&) = delete;
        // 購読の参照先アドレスを保つため移動を禁止します。
        EventBus(EventBus&&) = delete;
        // 購読の参照先アドレスを保つため移動代入を禁止します。
        EventBus& operator=(EventBus&&) = delete;

        // 名前付きイベントを購読します(eventName: イベント名, handler: 所有する受信関数)。
        // 空の名前または空の関数なら0を返し、通常の解除は返す番号でUnsubscribeします。
        std::uint64_t Subscribe(
            std::string_view eventName,
            Handler handler);
        // 番号に対応する購読を解除します(handle: 購読番号)。
        void Unsubscribe(std::uint64_t handle) noexcept;
        // 同じ名前の有効な購読を順に呼びます(eventName: イベント名, eventArgs: 処理中に保持する内容)。
        // 発行中の追加は次の発行から有効とし、解除された購読は呼びません。
        // 受信関数の例外では内部状態を戻し、後続の呼び出しを中断して再送出します。
        void Publish(
            std::string_view eventName,
            const EventArgs& eventArgs = {});
        // 名前付きイベントのリアクティブ購読口を作ります(eventName: イベント名)。
        // バスの破棄後に購読または解除しても、破棄済みバスへアクセスしません。
        [[nodiscard]] Observable<EventArgs> Observe(
            std::string_view eventName);
        // 全購読を解除し、発行中なら無効化して後で回収します。
        void Clear() noexcept;
        // 解除されていない購読の件数を返します。
        [[nodiscard]] std::size_t
            SubscriptionCount() const noexcept;

    private:
        // 発行の深さを戻し、最外周で解除済み購読を回収します。
        void FinishPublish() noexcept;

        struct Subscription final
        {
            // 購読番号か解除済みの0
            std::uint64_t id{};
            // 受信するイベント名
            std::string eventName;
            // 所有するイベント受信関数
            Handler handler;
        };

        // 登録順のイベント購読
        std::vector<Subscription> m_subscriptions;
        // 次に発行する購読番号
        std::uint64_t m_nextId{ 1 };
        // 入れ子のイベント発行深度
        int m_publishDepth{};
        // 解除済み購読の回収待ち
        bool m_needsCompaction{};
    };
}
