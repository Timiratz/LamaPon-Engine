#include "LamaPon/Reactive/Reactive.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    // 失敗したassertionの件数
    int g_failures = 0;

    // 条件不成立を失敗一覧へ追加します。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(const bool condition, const std::string& message)
    {
        // assertion失敗を集計する
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }
}

// Reactiveの購読・合成・時間通知を検証します。
int main()
{
    using namespace LamaPon;

    // 整数通知を送るテストSubject
    Subject<int> numbers;
    // 購読期間中に受け取った通知値の合計
    int total = 0;
    {
        // subscription: スコープ終了で購読解除; value: 通知された整数
        auto subscription = numbers.Subscribe(
            [&total](const int value)
            {
                total += value;
            });
        Require(subscription.IsSubscribed(), "Subscribe must be active.");
        Require(numbers.ObserverCount() == 1, "Observer was not added.");
        numbers.OnNext(3);
    }
    numbers.OnNext(10);
    Require(total == 3, "Subscription must unsubscribe on destruction.");
    Require(numbers.ObserverCount() == 0, "Observer was not removed.");

    // filter/select結果を文字列で受ける一覧
    std::vector<std::string> results;
    // pipeline: 偶数を10倍文字列化; value: 各段の通知値
    auto pipeline = numbers.AsObservable()
        .Where([](const int value) { return value % 2 == 0; })
        .Select([](const int value) { return std::to_string(value * 10); })
        .DistinctUntilChanged();
    // pipelineSubscription(value: 変換済み通知をresultsへ保存)
    auto pipelineSubscription = pipeline.Subscribe(
        [&results](const std::string& value)
        {
            results.push_back(value);
        });
    numbers.OnNext(1);
    numbers.OnNext(2);
    numbers.OnNext(2);
    numbers.OnNext(4);
    Require(
        results == std::vector<std::string>{ "20", "40" },
        "Where, Select, or DistinctUntilChanged failed.");

    // 自分自身を解除する購読
    Subscription selfSubscription;
    // self-unsubscribe callback呼び出し数
    int selfCalls = 0;
    selfSubscription = numbers.Subscribe(
        [&selfSubscription, &selfCalls](const int)
        {
            ++selfCalls;
            selfSubscription.Unsubscribe();
        });
    numbers.OnNext(1);
    numbers.OnNext(1);
    Require(selfCalls == 1, "Self-unsubscribe during OnNext failed.");

    // 複合購読で受信した通知数
    int groupedCalls = 0;
    // 個別購読を一括解除する所有者
    CompositeSubscription subscriptions;
    subscriptions.Add(numbers.Subscribe(
        [&groupedCalls](const int) { ++groupedCalls; }));
    subscriptions.Add(numbers.Subscribe(
        [&groupedCalls](const int) { ++groupedCalls; }));
    Require(subscriptions.Size() == 2, "Composite add failed.");
    numbers.OnNext(1);
    subscriptions.Clear();
    numbers.OnNext(1);
    Require(groupedCalls == 2, "Composite clear failed.");

    // 通知値を保持するリアクティブ体力値
    ReactiveProperty<int> health{ 100 };
    // 初期値と変更時の通知履歴
    std::vector<int> healthValues;
    // healthSubscription(value: 変更値をhealthValuesへ記録)
    auto healthSubscription = health.Observe().Subscribe(
        [&healthValues](const int value)
        {
            healthValues.push_back(value);
        });
    health.Set(80);
    health.Set(80);
    health.Set(50);
    Require(health.Value() == 50, "ReactiveProperty value is wrong.");
    Require(
        healthValues == std::vector<int>{ 100, 80, 50 },
        "ReactiveProperty notifications are wrong.");

    // Take購読で受け取る上限2件の値
    std::vector<int> taken;
    // takeSubscription(value: Takeが通した通知値)
    auto takeSubscription = numbers.AsObservable().Take(2).Subscribe(
        [&taken](const int value) { taken.push_back(value); });
    numbers.OnNext(5);
    numbers.OnNext(6);
    numbers.OnNext(7);
    Require(
        taken == std::vector<int>{ 5, 6 },
        "Take must stop after the requested value count.");

    // Mergeに通知を送る第1ソース
    Subject<int> first;
    // Mergeに通知を送る第2ソース
    Subject<int> second;
    // 2ソースから転送された値
    std::vector<int> merged;
    // mergeSubscription(value: Mergeから転送された通知値)
    auto mergeSubscription = first.AsObservable()
        .Merge(second.AsObservable())
        .Subscribe([&merged](const int value) { merged.push_back(value); });
    first.OnNext(1);
    second.OnNext(2);
    Require(
        merged == std::vector<int>{ 1, 2 },
        "Merge must forward both sources.");

    // CombineLatestが生成する最新値の組
    std::vector<std::pair<int, int>> combined;
    // combineSubscription(value: 各ソースの最新値)
    auto combineSubscription = first.AsObservable()
        .CombineLatest(second.AsObservable())
        .Subscribe(
            [&combined](const std::pair<int, int>& value)
            {
                combined.push_back(value);
            });
    first.OnNext(10);
    second.OnNext(20);
    second.OnNext(30);
    Require(
        combined
            == std::vector<std::pair<int, int>>{
                { 10, 20 }, { 10, 30 } },
        "CombineLatest must combine the newest source values.");

    Reactive::Detail::Reset();
    // NextFrameの発火回数
    int nextFrameCalls = 0;
    // 1フレーム通知の購読
    auto nextFrameSubscription = Reactive::NextFrame().Subscribe(
        [&nextFrameCalls](const std::uint64_t) { ++nextFrameCalls; });
    // Intervalの発火値
    std::vector<std::uint64_t> intervalTicks;
    // intervalSubscription(value: scaled timeの発火番号)
    auto intervalSubscription = Reactive::Interval(0.5f).Subscribe(
        [&intervalTicks](const std::uint64_t value)
        {
            intervalTicks.push_back(value);
        });
    // unscaled timerの発火回数
    int unscaledTimerCalls = 0;
    // valueを使わないunscaled timer購読
    auto timerSubscription = Reactive::Timer(0.5f, true).Subscribe(
        [&unscaledTimerCalls](const std::uint64_t)
        {
            ++unscaledTimerCalls;
        });
    Reactive::Detail::AdvanceFrame(0.25f, 0.25f);
    Reactive::Detail::AdvanceFrame(0.25f, 0.25f);
    Reactive::Detail::AdvanceFrame(0.0f, 0.5f);
    Require(nextFrameCalls == 1, "NextFrame must publish exactly once.");
    Require(
        intervalTicks == std::vector<std::uint64_t>{ 0 },
        "Scaled interval must pause when scaled delta time is zero.");
    Require(
        unscaledTimerCalls == 1,
        "Unscaled timer must use unscaled delta time.");
    Reactive::Detail::Reset();

    // 全assertion成功時だけ成功終了
    if (g_failures == 0)
    {
        std::cout << "Reactive tests passed.\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
