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
    // 解除処理を一度だけ実行し、移動元や解除済みの破棄では呼び出さない。
    class Subscription final
    {
    public:
        // 解除処理を持たない空の購読を作る。
        Subscription() = default;
        // 解除処理の所有権を受け取る(unsubscribe: 1回だけ呼び出す解除処理)。
        explicit Subscription(std::function<void()> unsubscribe)
            : m_unsubscribe(std::move(unsubscribe))
        {
        }

        // 解除処理の二重所有を禁止する。
        Subscription(const Subscription&) = delete;
        // 解除処理のコピー代入を禁止する。
        Subscription& operator=(const Subscription&) = delete;

        // 移動元を解除せず購読を引き継ぐ(other: 移動元の購読)。
        Subscription(Subscription&& other) noexcept
            : m_unsubscribe(std::move(other.m_unsubscribe))
        {
            other.m_unsubscribe = nullptr;
        }

        // 現在の購読を解除して引き継ぐ(other: 移動元の購読)。
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

        // 例外を出さず購読を解除する。
        ~Subscription()
        {
            Unsubscribe();
        }

        // 解除済みなら何もせず、解除処理の例外を抑止する。
        void Unsubscribe() noexcept
        {
            if (m_unsubscribe == nullptr)
            {
                return;
            }
            // 一度だけ実行する解除処理
            auto unsubscribe = std::move(m_unsubscribe);
            m_unsubscribe = nullptr;
            try
            {
                unsubscribe();
            }
            catch (...)
            {

            }
        }

        // 解除処理が保持されているか返す。
        [[nodiscard]] bool IsSubscribed() const noexcept
        {
            return m_unsubscribe != nullptr;
        }

    private:
        // 所有する購読の解除処理
        std::function<void()> m_unsubscribe;
    };

    // 複数の購読を所有し、破棄・Clear時に一括解除する。
    class CompositeSubscription final
    {
    public:
        // 空の購読一覧を作る。
        CompositeSubscription() = default;
        // 購読一覧の二重所有を禁止する。
        CompositeSubscription(const CompositeSubscription&) = delete;
        // 購読一覧のコピー代入を禁止する。
        CompositeSubscription& operator=(
            const CompositeSubscription&) = delete;
        // 購読一覧の所有を引き継ぐ。
        CompositeSubscription(CompositeSubscription&&) noexcept = default;
        // 現在の購読を解除して一覧の所有を引き継ぐ。
        CompositeSubscription& operator=(
            CompositeSubscription&&) noexcept = default;

        // 有効な購読だけ所有してまとめる(subscription: 所有を移す購読)。
        void Add(Subscription subscription)
        {
            if (subscription.IsSubscribed())
            {
                m_subscriptions.push_back(std::move(subscription));
            }
        }

        // 保持する全購読を解除する。
        void Clear() noexcept
        {
            m_subscriptions.clear();
        }

        // 保持する購読数を返す。
        [[nodiscard]] std::size_t Size() const noexcept
        {
            return m_subscriptions.size();
        }

    private:
        // 所有する購読一覧
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

        // 購読を作成しない空の通知窓口を作る。
        Observable() = default;
        // 購読の作成処理を保持する(subscribe: 通知先を受け取り解除手段を返す処理)。
        explicit Observable(SubscribeFunction subscribe)
            : m_subscribe(std::move(subscribe))
        {
        }

        // 空の通知先を除外して購読を作る(observer: 同期で値を受け取る処理)。
        [[nodiscard]] Subscription Subscribe(Observer observer) const
        {
            if (m_subscribe == nullptr || observer == nullptr)
            {
                return {};
            }
            return m_subscribe(std::move(observer));
        }

        // 条件を満たす通知だけ通す(predicate: 値の採用条件)。
        template<typename Predicate>
        [[nodiscard]] Observable<T> Where(Predicate predicate) const
        {
            // 変換元の通知窓口
            auto source = *this;
            // 条件付き購読を作る(observer: 採用値の通知先)。
            return Observable<T>{
                [source, predicate = std::move(predicate)](
                    Observer observer) mutable
                {
                    // 条件を満たす値を転送する(value: 借用する通知値)。
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

        // 指定回数で元の購読を解除する(count: 受け取る最大回数)。
        [[nodiscard]] Observable<T> Take(const std::size_t count) const
        {
            if (count == 0)
            {
                return Observable<T>{};
            }

            // 変換元の通知窓口
            auto source = *this;
            // 回数制限付き購読を作る(observer: 制限内の通知先)。
            return Observable<T>{
                [source, count](Observer observer)
                {
                    struct TakeState final
                    {
                        // 残りの採用通知回数
                        std::size_t remaining{};
                        // 元購読受領後の解除予約
                        bool cancelRequested{};
                        // 元ストリームの解除手段
                        std::optional<Subscription> upstream;
                    };

                    // 購読またはプロパティの共有状態
                    auto state = std::make_shared<TakeState>();
                    state->remaining = count;
                    // 回数を減らして通知し最後に解除する(value: 借用する通知値)。
                    // 元ストリームの購読
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
                                // Subscribe中の同期通知では元購読の受領後に解除する。
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
                    // 元ストリームの購読を解除する。
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

        // 両方の通知を同じ通知先へ渡す(other: 合流するストリーム)。
        [[nodiscard]] Observable<T> Merge(
            const Observable<T>& other) const
        {
            // 合成元の第1通知窓口
            auto first = *this;
            // 両通知元を購読する(observer: 合流先の通知処理)。
            return Observable<T>{
                [first, other](Observer observer)
                {
                    // 合成した通知元の購読一覧
                    auto subscriptions =
                        std::make_shared<CompositeSubscription>();
                    // 両通知元の共有通知先
                    auto sharedObserver =
                        std::make_shared<Observer>(std::move(observer));
                    // 第1通知元の値を渡す(value: 借用する通知値)。
                    subscriptions->Add(first.Subscribe(
                        [sharedObserver](const T& value)
                        {
                            (*sharedObserver)(value);
                        }));
                    // 第2通知元の値を渡す(value: 借用する通知値)。
                    subscriptions->Add(other.Subscribe(
                        [sharedObserver](const T& value)
                        {
                            (*sharedObserver)(value);
                        }));
                    // 両通知元の購読をまとめて解除する。
                    return Subscription{
                        [subscriptions]()
                        {
                            subscriptions->Clear();
                        } };
                } };
        }

        // 双方の値が揃ってから最新値の組を通知する(other: 組み合わせるストリーム)。
        template<typename U>
        [[nodiscard]] Observable<std::pair<T, U>> CombineLatest(
            const Observable<U>& other) const
        {
            // 合成元の第1通知窓口
            auto first = *this;
            using Result = std::pair<T, U>;
            // 双方の最新値を保持する購読を作る(observer: 値の組を受ける通知先)。
            return Observable<Result>{
                [first, other](
                    typename Observable<Result>::Observer observer)
                {
                    struct LatestState final
                    {
                        // 第1通知元の最新値
                        std::optional<T> firstValue;
                        // 第2通知元の最新値
                        std::optional<U> secondValue;
                        // 最新値の組を受ける通知先
                        typename Observable<Result>::Observer observer;
                    };

                    // 購読またはプロパティの共有状態
                    auto state = std::make_shared<LatestState>();
                    state->observer = std::move(observer);
                    // 合成した通知元の購読一覧
                    auto subscriptions =
                        std::make_shared<CompositeSubscription>();
                    // 第1の値を更新し双方が揃えば通知する(value: 第1通知元の値)。
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
                    // 第2の値を更新し双方が揃えば通知する(value: 第2通知元の値)。
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
                    // 両通知元の購読をまとめて解除する。
                    return Subscription{
                        [subscriptions]()
                        {
                            subscriptions->Clear();
                        } };
                } };
        }

        // 通知値を変換して渡す(selector: void以外の値を返す変換)。
        template<typename Selector>
        [[nodiscard]] auto Select(Selector selector) const
        {
            using Result = std::remove_cvref_t<
                std::invoke_result_t<Selector, const T&>>;
            static_assert(
                !std::is_void_v<Result>,
                "Observable::Select must return a value.");

            // 変換元の通知窓口
            auto source = *this;
            // 変換結果を受ける購読を作る(observer: 変換後の通知先)。
            return Observable<Result>{
                [source, selector = std::move(selector)](
                    typename Observable<Result>::Observer observer) mutable
                {
                    // 借用値から変換結果を同期通知する(value: 変換前の値)。
                    return source.Subscribe(
                        [selector, observer = std::move(observer)](
                            const T& value) mutable
                        {
                            observer(std::invoke(selector, value));
                        });
                } };
        }

        // 直前の採用値と等しい通知を除く(equal: 値の同一判定)。
        template<typename Equal = std::equal_to<T>>
        [[nodiscard]] Observable<T> DistinctUntilChanged(
            Equal equal = {}) const
        {
            // 変換元の通知窓口
            auto source = *this;
            // 直前の採用値を保持する購読を作る(observer: 重複を除いた通知先)。
            return Observable<T>{
                [source, equal = std::move(equal)](
                    Observer observer) mutable
                {
                    struct DistinctState final
                    {
                        // 採用済みの直前値の有無
                        bool hasValue{};
                        // 最後に採用した通知値
                        std::shared_ptr<T> previous;
                    };
                    // 購読またはプロパティの共有状態
                    auto state = std::make_shared<DistinctState>();
                    // 直前の採用値と異なる値だけ渡す(value: 借用する通知値)。
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
        // 購読を生成する処理
        SubscribeFunction m_subscribe;
    };

    // メインスレッドで同期通知し、通知開始後の新規購読は次回以降に含める。
    template<typename T>
    class Subject final
    {
    private:
        struct State final
        {
            struct Entry final
            {
                // 購読を識別する番号
                std::uint64_t id{};
                // この購読の同期通知先
                typename Observable<T>::Observer observer;
            };

            // 通知先の購読項目列
            std::vector<Entry> observers;
            // 次に発行する購読番号
            std::uint64_t nextId{ 1 };
            // 同期通知の再入深度
            int publishDepth{};
            // 通知後に必要な項目圧縮
            bool needsCompaction{};

            // 通知中なら削除を遅らせて購読を解除する(id: 対象の購読番号)。
            void Remove(const std::uint64_t id) noexcept
            {
                // 確認中の購読項目
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
                        // 指定購読を取り除く(candidate: 確認する購読項目)。
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

            // 最外の通知が終わったとき削除済み購読を詰める。
            void FinishPublish() noexcept
            {
                --publishDepth;
                if (publishDepth == 0 && needsCompaction)
                {
                    needsCompaction = false;
                    // 解除済みの購読を詰める(entry: 確認する購読項目)。
                    std::erase_if(
                        observers,
                        [](const Entry& entry)
                        {
                            return entry.id == 0;
                        });
                }
            }

            // 通知中の解除を許容し、新規購読は今回の範囲へ加えない。
            // 通知先の例外は以降の通知を中断し、呼び出し元へ伝える。
            // 開始時の購読者へ順に同期通知する(value: 通知中に借用する値)。
            void Publish(const T& value)
            {
                // 開始時または現在の購読数
                const auto count = observers.size();
                ++publishDepth;
                try
                {
                    // 開始時の購読者番号
                    for (std::size_t index = 0;
                        // 開始時または現在の購読数
                        index < count;
                        ++index)
                    {
                        // 確認中の購読項目
                        auto& entry = observers[index];
                        if (entry.id == 0 || entry.observer == nullptr)
                        {
                            continue;
                        }
                        // 今回の同期通知先
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
        // 共有する通知先の状態を作る。
        Subject()
            : m_state(std::make_shared<State>())
        {
        }

        // 独立した通知元の複製を禁止する。
        Subject(const Subject&) = delete;
        // 通知元のコピー代入を禁止する。
        Subject& operator=(const Subject&) = delete;
        // 通知元の移動を禁止する。
        Subject(Subject&&) = delete;
        // 通知元の移動代入を禁止する。
        Subject& operator=(Subject&&) = delete;

        // 通知元が破棄されても状態を保持する購読窓口を作る。
        [[nodiscard]] Observable<T> AsObservable() const
        {
            // 購読またはプロパティの共有状態
            const auto state = m_state;
            // 変更を購読して現在値を即時通知する(observer: 現在値と変更の受け手)。
            return Observable<T>{
                [state](typename Observable<T>::Observer observer)
                {
                    if (observer == nullptr)
                    {
                        return Subscription{};
                    }
                    // 新しい購読の識別番号
                    const auto id = state->nextId++;
                    state->observers.push_back(
                        { id, std::move(observer) });
                    // 通知元が存続していれば解除する(weakState: 通知元の弱参照)。
                    return Subscription{
                        [weakState = std::weak_ptr<State>{ state }, id]()
                        {
                            // 存続中の通知元の状態
                            if (const auto locked = weakState.lock())
                            {
                                locked->Remove(id);
                            }
                        } };
                } };
        }

        // 同期通知の購読を作る(observer: 値を受け取る処理)。
        [[nodiscard]] Subscription Subscribe(
            typename Observable<T>::Observer observer) const
        {
            return AsObservable().Subscribe(std::move(observer));
        }

        // 現在の購読者へ同期通知する(value: 通知中に借用する値)。
        void OnNext(const T& value)
        {
            m_state->Publish(value);
        }

        // 解除済みを除いた購読者数を数える。
        [[nodiscard]] std::size_t ObserverCount() const noexcept
        {
            // 開始時または現在の購読数
            std::size_t count = 0;
            // 確認中の購読項目
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
        // 通知や現在値の共有状態
        std::shared_ptr<State> m_state;
    };

    // コピーしたプロパティも同じ値と通知状態を共有する。
    template<std::equality_comparable T>
    class ReactiveProperty final
    {
    private:
        struct State final
        {
            // プロパティの初期値を保持する(initialValue: 所有を移す値)。
            explicit State(T initialValue)
                : value(std::move(initialValue))
            {
            }

            // 共有するプロパティ値
            T value;
            // 値の変更を通知する元
            Subject<T> changes;
        };

    public:
        // プロパティの参照・更新・通知はメインスレッドで行う。
        // 現在値と変更通知を共有する状態を作る(initialValue: 初期値)。
        explicit ReactiveProperty(T initialValue = {})
            : m_state(std::make_shared<State>(std::move(initialValue)))
        {
        }

        // 次の変更まで有効な現在値を参照する。
        [[nodiscard]] const T& Value() const noexcept
        {
            return m_state->value;
        }

        // 値が変わった場合だけ保存して同期通知する(value: 新しい値)。
        void Set(T value)
        {
            if (m_state->value == value)
            {
                return;
            }
            m_state->value = std::move(value);
            m_state->changes.OnNext(m_state->value);
        }

        // 購読時に現在値を同期通知し以後の変更を通知する。
        [[nodiscard]] Observable<T> Observe() const
        {
            // 購読またはプロパティの共有状態
            const auto state = m_state;
            // 通知先を登録する(observer: 同期通知の受け手)。
            return Observable<T>{
                [state](typename Observable<T>::Observer observer)
                {
                    // 現在値通知を含む購読
                    auto subscription =
                        state->changes.Subscribe(observer);
                    observer(state->value);
                    return subscription;
                } };
        }

        // 購読時の現在値を含めず変更だけを通知する。
        [[nodiscard]] Observable<T> Changes() const
        {
            return m_state->changes.AsObservable();
        }

    private:
        // 通知や現在値の共有状態
        std::shared_ptr<State> m_state;
    };

    // 予約の追加・解除・時計更新はメインスレッドで行う。
    namespace Reactive
    {
        // 次のゲーム更新で0を1回通知します。
        [[nodiscard]] Observable<std::uint64_t> NextFrame();
        // ゲーム更新ごとに0から始まる連番を通知します。
        [[nodiscard]] Observable<std::uint64_t> EveryFrame();
        // 通知の時計は窓口の作成時ではなく購読開始時に動き始める。
        // 次の更新以降に指定秒後の0を1回通知する(seconds: 負値・非有限値は0の待機秒数, useUnscaledTime: 時間倍率を無視するか)。
        [[nodiscard]] Observable<std::uint64_t> Timer(
            float seconds,
            bool useUnscaledTime = false);
        // 非有限値は最小間隔に置換し、購読開始時から計時する。
        // 更新ごとに最大1回ずつ連番を通知する(seconds: 最低1マイクロ秒の間隔, useUnscaledTime: 時間倍率を無視するか)。
        [[nodiscard]] Observable<std::uint64_t> Interval(
            float seconds,
            bool useUnscaledTime = false);

        namespace Detail
        {
            // 予約した通知の時計を進める(deltaTime: 倍率適用後の秒数, unscaledDeltaTime: 倍率適用前の秒数)。
            void AdvanceFrame(float deltaTime, float unscaledDeltaTime);
            // 購読番号を維持し、通知中でも全予約を無効化する。
            void Reset() noexcept;
        }
    }
}
