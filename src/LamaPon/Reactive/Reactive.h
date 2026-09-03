#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace LamaPon
{
    // Observableの購読寿命を所有します。破棄時に自動で購読を解除します。
    class Subscription final
    {
    public:
        Subscription() = default;
        explicit Subscription(std::function<void()> unsubscribe)
            : m_unsubscribe(std::move(unsubscribe))
        {
        }

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& other) noexcept
            : m_unsubscribe(std::move(other.m_unsubscribe))
        {
            other.m_unsubscribe = nullptr;
        }

        Subscription& operator=(Subscription&& other) noexcept
        {
            if (this != &other)
            {
                Unsubscribe();
                m_unsubscribe = std::move(other.m_unsubscribe);
                other.m_unsubscribe = nullptr;
            }
            return *this;
        }

        ~Subscription()
        {
            Unsubscribe();
        }

        void Unsubscribe() noexcept
        {
            if (m_unsubscribe == nullptr)
            {
                return;
            }
            auto unsubscribe = std::move(m_unsubscribe);
            m_unsubscribe = nullptr;
            try
            {
                unsubscribe();
            }
            catch (...)
            {
                // デストラクターから例外を出さないため、解除処理の
                // 例外は境界で止めます。
            }
        }

        [[nodiscard]] bool IsSubscribed() const noexcept
        {
            return m_unsubscribe != nullptr;
        }

    private:
        std::function<void()> m_unsubscribe;
    };

    // Scriptなどが複数の購読をまとめて所有するための入れ物です。
    class CompositeSubscription final
    {
    public:
        CompositeSubscription() = default;
        CompositeSubscription(const CompositeSubscription&) = delete;
        CompositeSubscription& operator=(
            const CompositeSubscription&) = delete;
        CompositeSubscription(CompositeSubscription&&) noexcept = default;
        CompositeSubscription& operator=(
            CompositeSubscription&&) noexcept = default;

        void Add(Subscription subscription)
        {
            if (subscription.IsSubscribed())
            {
                m_subscriptions.push_back(std::move(subscription));
            }
        }

        void Clear() noexcept
        {
            m_subscriptions.clear();
        }

        [[nodiscard]] std::size_t Size() const noexcept
        {
            return m_subscriptions.size();
        }

    private:
        std::vector<Subscription> m_subscriptions;
    };

    template<typename T>
    class Observable final
    {
    public:
        using ValueType = T;
        using Observer = std::function<void(const T&)>;
        using SubscribeFunction =
            std::function<Subscription(Observer)>;

        Observable() = default;
        explicit Observable(SubscribeFunction subscribe)
            : m_subscribe(std::move(subscribe))
        {
        }

        [[nodiscard]] Subscription Subscribe(Observer observer) const
        {
            if (m_subscribe == nullptr || observer == nullptr)
            {
                return {};
            }
            return m_subscribe(std::move(observer));
        }

        template<typename Predicate>
        [[nodiscard]] Observable<T> Where(Predicate predicate) const
        {
            auto source = *this;
            return Observable<T>{
                [source, predicate = std::move(predicate)](
                    Observer observer) mutable
                {
                    return source.Subscribe(
                        [predicate, observer = std::move(observer)](
                            const T& value) mutable
                        {
                            if (std::invoke(predicate, value))
                            {
                                observer(value);
                            }
                        });
                } };
        }

        [[nodiscard]] Observable<T> Take(const std::size_t count) const
        {
            if (count == 0)
            {
                return Observable<T>{};
            }

            auto source = *this;
            return Observable<T>{
                [source, count](Observer observer)
                {
                    struct TakeState final
                    {
                        std::size_t remaining{};
                        bool cancelRequested{};
                        std::optional<Subscription> upstream;
                    };

                    auto state = std::make_shared<TakeState>();
                    state->remaining = count;
                    auto upstream = source.Subscribe(
                        [state, observer = std::move(observer)](
                            const T& value)
                        {
                            if (state->remaining == 0)
                            {
                                return;
                            }
                            --state->remaining;
                            observer(value);
                            if (state->remaining == 0)
                            {
                                if (state->upstream.has_value())
                                {
                                    state->upstream->Unsubscribe();
                                }
                                else
                                {
                                    state->cancelRequested = true;
                                }
                            }
                        });
                    state->upstream.emplace(std::move(upstream));
                    if (state->cancelRequested)
                    {
                        state->upstream->Unsubscribe();
                    }
                    return Subscription{
                        [state]()
                        {
                            if (state->upstream.has_value())
                            {
                                state->upstream->Unsubscribe();
                            }
                        } };
                } };
        }

        [[nodiscard]] Observable<T> Merge(
            const Observable<T>& other) const
        {
            auto first = *this;
            return Observable<T>{
                [first, other](Observer observer)
                {
                    auto subscriptions =
                        std::make_shared<CompositeSubscription>();
                    auto sharedObserver =
                        std::make_shared<Observer>(std::move(observer));
                    subscriptions->Add(first.Subscribe(
                        [sharedObserver](const T& value)
                        {
                            (*sharedObserver)(value);
                        }));
                    subscriptions->Add(other.Subscribe(
                        [sharedObserver](const T& value)
                        {
                            (*sharedObserver)(value);
                        }));
                    return Subscription{
                        [subscriptions]()
                        {
                            subscriptions->Clear();
                        } };
                } };
        }

        template<typename U>
        [[nodiscard]] Observable<std::pair<T, U>> CombineLatest(
            const Observable<U>& other) const
        {
            auto first = *this;
            using Result = std::pair<T, U>;
            return Observable<Result>{
                [first, other](
                    typename Observable<Result>::Observer observer)
                {
                    struct LatestState final
                    {
                        std::optional<T> firstValue;
                        std::optional<U> secondValue;
                        typename Observable<Result>::Observer observer;
                    };

                    auto state = std::make_shared<LatestState>();
                    state->observer = std::move(observer);
                    auto subscriptions =
                        std::make_shared<CompositeSubscription>();
                    subscriptions->Add(first.Subscribe(
                        [state](const T& value)
                        {
                            state->firstValue = value;
                            if (state->secondValue.has_value())
                            {
                                state->observer(Result{
                                    *state->firstValue,
                                    *state->secondValue });
                            }
                        }));
                    subscriptions->Add(other.Subscribe(
                        [state](const U& value)
                        {
                            state->secondValue = value;
                            if (state->firstValue.has_value())
                            {
                                state->observer(Result{
                                    *state->firstValue,
                                    *state->secondValue });
                            }
                        }));
                    return Subscription{
                        [subscriptions]()
                        {
                            subscriptions->Clear();
                        } };
                } };
        }

        template<typename Selector>
        [[nodiscard]] auto Select(Selector selector) const
        {
            using Result = std::remove_cvref_t<
                std::invoke_result_t<Selector, const T&>>;
            static_assert(
                !std::is_void_v<Result>,
                "Observable::Select must return a value.");

            auto source = *this;
            return Observable<Result>{
                [source, selector = std::move(selector)](
                    typename Observable<Result>::Observer observer) mutable
                {
                    return source.Subscribe(
                        [selector, observer = std::move(observer)](
                            const T& value) mutable
                        {
                            observer(std::invoke(selector, value));
                        });
                } };
        }

        template<typename Equal = std::equal_to<T>>
        [[nodiscard]] Observable<T> DistinctUntilChanged(
            Equal equal = {}) const
        {
            auto source = *this;
            return Observable<T>{
                [source, equal = std::move(equal)](
                    Observer observer) mutable
                {
                    struct DistinctState final
                    {
                        bool hasValue{};
                        std::shared_ptr<T> previous;
                    };
                    auto state = std::make_shared<DistinctState>();
                    return source.Subscribe(
                        [state,
                         equal,
                         observer = std::move(observer)](
                            const T& value) mutable
                        {
                            if (state->hasValue
                                && std::invoke(
                                    equal,
                                    *state->previous,
                                    value))
                            {
                                return;
                            }
                            state->previous =
                                std::make_shared<T>(value);
                            state->hasValue = true;
                            observer(value);
                        });
                } };
        }

    private:
        SubscribeFunction m_subscribe;
    };

    // 値を手動発行する同期ストリームです。ゲームのメインスレッドで
    // 使うことを想定しています。
    template<typename T>
    class Subject final
    {
    private:
        struct State final
        {
            struct Entry final
            {
                std::uint64_t id{};
                typename Observable<T>::Observer observer;
            };

            std::vector<Entry> observers;
            std::uint64_t nextId{ 1 };
            int publishDepth{};
            bool needsCompaction{};

            void Remove(const std::uint64_t id) noexcept
            {
                for (auto& entry : observers)
                {
                    if (entry.id != id)
                    {
                        continue;
                    }
                    if (publishDepth > 0)
                    {
                        entry.id = 0;
                        entry.observer = nullptr;
                        needsCompaction = true;
                    }
                    else
                    {
                        std::erase_if(
                            observers,
                            [id](const Entry& candidate)
                            {
                                return candidate.id == id;
                            });
                    }
                    return;
                }
            }

            void FinishPublish() noexcept
            {
                --publishDepth;
                if (publishDepth == 0 && needsCompaction)
                {
                    needsCompaction = false;
                    std::erase_if(
                        observers,
                        [](const Entry& entry)
                        {
                            return entry.id == 0;
                        });
                }
            }

            void Publish(const T& value)
            {
                const auto count = observers.size();
                ++publishDepth;
                try
                {
                    for (std::size_t index = 0;
                        index < count;
                        ++index)
                    {
                        auto& entry = observers[index];
                        if (entry.id == 0 || entry.observer == nullptr)
                        {
                            continue;
                        }
                        const auto observer = entry.observer;
                        observer(value);
                    }
                }
                catch (...)
                {
                    FinishPublish();
                    throw;
                }
                FinishPublish();
            }
        };

    public:
        Subject()
            : m_state(std::make_shared<State>())
        {
        }

        Subject(const Subject&) = delete;
        Subject& operator=(const Subject&) = delete;
        Subject(Subject&&) = delete;
        Subject& operator=(Subject&&) = delete;

        [[nodiscard]] Observable<T> AsObservable() const
        {
            const auto state = m_state;
            return Observable<T>{
                [state](typename Observable<T>::Observer observer)
                {
                    if (observer == nullptr)
                    {
                        return Subscription{};
                    }
                    const auto id = state->nextId++;
                    state->observers.push_back(
                        { id, std::move(observer) });
                    return Subscription{
                        [weakState = std::weak_ptr<State>{ state }, id]()
                        {
                            if (const auto locked = weakState.lock())
                            {
                                locked->Remove(id);
                            }
                        } };
                } };
        }

        [[nodiscard]] Subscription Subscribe(
            typename Observable<T>::Observer observer) const
        {
            return AsObservable().Subscribe(std::move(observer));
        }

        void OnNext(const T& value)
        {
            m_state->Publish(value);
        }

        [[nodiscard]] std::size_t ObserverCount() const noexcept
        {
            std::size_t count = 0;
            for (const auto& entry : m_state->observers)
            {
                if (entry.id != 0)
                {
                    ++count;
                }
            }
            return count;
        }

    private:
        std::shared_ptr<State> m_state;
    };

    // 現在値を保持し、変更時だけ通知するプロパティです。Observe()を
    // 購読すると、最初に現在値が1回届きます。
    template<std::equality_comparable T>
    class ReactiveProperty final
    {
    private:
        struct State final
        {
            explicit State(T initialValue)
                : value(std::move(initialValue))
            {
            }

            T value;
            Subject<T> changes;
        };

    public:
        explicit ReactiveProperty(T initialValue = {})
            : m_state(std::make_shared<State>(std::move(initialValue)))
        {
        }

        [[nodiscard]] const T& Value() const noexcept
        {
            return m_state->value;
        }

        void Set(T value)
        {
            if (m_state->value == value)
            {
                return;
            }
            m_state->value = std::move(value);
            m_state->changes.OnNext(m_state->value);
        }

        [[nodiscard]] Observable<T> Observe() const
        {
            const auto state = m_state;
            return Observable<T>{
                [state](typename Observable<T>::Observer observer)
                {
                    auto subscription =
                        state->changes.Subscribe(observer);
                    observer(state->value);
                    return subscription;
                } };
        }

        [[nodiscard]] Observable<T> Changes() const
        {
            return m_state->changes.AsObservable();
        }

    private:
        std::shared_ptr<State> m_state;
    };

    namespace Reactive
    {
        // 次のゲーム更新で0を1回通知します。
        [[nodiscard]] Observable<std::uint64_t> NextFrame();
        // ゲーム更新ごとに0から始まる連番を通知します。
        [[nodiscard]] Observable<std::uint64_t> EveryFrame();
        // 指定秒後に0を1回通知します。
        [[nodiscard]] Observable<std::uint64_t> Timer(
            float seconds,
            bool useUnscaledTime = false);
        // 指定秒ごとに0から始まる連番を通知します。
        [[nodiscard]] Observable<std::uint64_t> Interval(
            float seconds,
            bool useUnscaledTime = false);

        namespace Detail
        {
            void AdvanceFrame(float deltaTime, float unscaledDeltaTime);
            void Reset() noexcept;
        }
    }
}
