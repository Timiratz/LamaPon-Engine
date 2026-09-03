#include "LamaPon/Reactive/Reactive.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void Require(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }
}

int main()
{
    using namespace LamaPon;

    Subject<int> numbers;
    int total = 0;
    {
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

    std::vector<std::string> results;
    auto pipeline = numbers.AsObservable()
        .Where([](const int value) { return value % 2 == 0; })
        .Select([](const int value) { return std::to_string(value * 10); })
        .DistinctUntilChanged();
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

    Subscription selfSubscription;
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

    int groupedCalls = 0;
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

    ReactiveProperty<int> health{ 100 };
    std::vector<int> healthValues;
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

    std::vector<int> taken;
    auto takeSubscription = numbers.AsObservable().Take(2).Subscribe(
        [&taken](const int value) { taken.push_back(value); });
    numbers.OnNext(5);
    numbers.OnNext(6);
    numbers.OnNext(7);
    Require(
        taken == std::vector<int>{ 5, 6 },
        "Take must stop after the requested value count.");

    Subject<int> first;
    Subject<int> second;
    std::vector<int> merged;
    auto mergeSubscription = first.AsObservable()
        .Merge(second.AsObservable())
        .Subscribe([&merged](const int value) { merged.push_back(value); });
    first.OnNext(1);
    second.OnNext(2);
    Require(
        merged == std::vector<int>{ 1, 2 },
        "Merge must forward both sources.");

    std::vector<std::pair<int, int>> combined;
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
    int nextFrameCalls = 0;
    auto nextFrameSubscription = Reactive::NextFrame().Subscribe(
        [&nextFrameCalls](const std::uint64_t) { ++nextFrameCalls; });
    std::vector<std::uint64_t> intervalTicks;
    auto intervalSubscription = Reactive::Interval(0.5f).Subscribe(
        [&intervalTicks](const std::uint64_t value)
        {
            intervalTicks.push_back(value);
        });
    int unscaledTimerCalls = 0;
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

    if (g_failures == 0)
    {
        std::cout << "Reactive tests passed.\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
