#include "LamaPon/Reactive/Reactive.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
    enum class ScheduleKind
    {
        NextFrame,
        EveryFrame,
        Timer,
        Interval
    };

    struct ScheduledEntry final
    {
        std::uint64_t id{};
        ScheduleKind kind{};
        float remaining{};
        float interval{};
        bool useUnscaledTime{};
        std::uint64_t nextValue{};
        LamaPon::Observable<std::uint64_t>::Observer observer;
    };

    struct SchedulerState final
    {
        std::vector<ScheduledEntry> entries;
        std::uint64_t nextId{ 1 };
        int publishDepth{};
        bool needsCompaction{};

        void Remove(const std::uint64_t id) noexcept
        {
            for (auto& entry : entries)
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
                        entries,
                        [id](const ScheduledEntry& candidate)
                        {
                            return candidate.id == id;
                        });
                }
                return;
            }
        }

        void FinishAdvance() noexcept
        {
            --publishDepth;
            if (publishDepth == 0 && needsCompaction)
            {
                needsCompaction = false;
                std::erase_if(
                    entries,
                    [](const ScheduledEntry& entry)
                    {
                        return entry.id == 0;
                    });
            }
        }

        void Advance(const float deltaTime, const float unscaledDeltaTime)
        {
            const auto count = entries.size();
            ++publishDepth;
            try
            {
                for (std::size_t index = 0; index < count; ++index)
                {
                    auto& entry = entries[index];
                    if (entry.id == 0 || entry.observer == nullptr)
                    {
                        continue;
                    }

                    bool shouldPublish = false;
                    bool completes = false;
                    switch (entry.kind)
                    {
                    case ScheduleKind::NextFrame:
                        shouldPublish = true;
                        completes = true;
                        break;
                    case ScheduleKind::EveryFrame:
                        shouldPublish = true;
                        break;
                    case ScheduleKind::Timer:
                    case ScheduleKind::Interval:
                    {
                        const float elapsed = entry.useUnscaledTime
                            ? unscaledDeltaTime
                            : deltaTime;
                        entry.remaining -= std::max(elapsed, 0.0f);
                        shouldPublish = entry.remaining <= 0.0f;
                        completes = shouldPublish
                            && entry.kind == ScheduleKind::Timer;
                        if (shouldPublish
                            && entry.kind == ScheduleKind::Interval)
                        {
                            // 長いフレームでも通知を大量発生させず、次の
                            // 区間へ位相だけ繰り越します。
                            entry.remaining = std::max(
                                entry.remaining + entry.interval,
                                0.0f);
                        }
                        break;
                    }
                    }

                    if (!shouldPublish)
                    {
                        continue;
                    }
                    const auto id = entry.id;
                    const auto value = entry.nextValue++;
                    const auto observer = entry.observer;
                    if (completes)
                    {
                        Remove(id);
                    }
                    observer(value);
                }
            }
            catch (...)
            {
                FinishAdvance();
                throw;
            }
            FinishAdvance();
        }
    };

    SchedulerState& Scheduler()
    {
        static SchedulerState state;
        return state;
    }

    LamaPon::Observable<std::uint64_t> Schedule(
        const ScheduleKind kind,
        const float seconds,
        const bool useUnscaledTime)
    {
        return LamaPon::Observable<std::uint64_t>{
            [kind, seconds, useUnscaledTime](
                LamaPon::Observable<std::uint64_t>::Observer observer)
            {
                if (observer == nullptr)
                {
                    return LamaPon::Subscription{};
                }
                auto& scheduler = Scheduler();
                const auto id = scheduler.nextId++;
                scheduler.entries.push_back({
                    id,
                    kind,
                    std::max(seconds, 0.0f),
                    std::max(seconds, 0.000001f),
                    useUnscaledTime,
                    0,
                    std::move(observer) });
                return LamaPon::Subscription{
                    [id]()
                    {
                        Scheduler().Remove(id);
                    } };
            } };
    }
}

namespace LamaPon::Reactive
{
    Observable<std::uint64_t> NextFrame()
    {
        return Schedule(ScheduleKind::NextFrame, 0.0f, false);
    }

    Observable<std::uint64_t> EveryFrame()
    {
        return Schedule(ScheduleKind::EveryFrame, 0.0f, false);
    }

    Observable<std::uint64_t> Timer(
        const float seconds,
        const bool useUnscaledTime)
    {
        return Schedule(
            ScheduleKind::Timer,
            std::isfinite(seconds) ? seconds : 0.0f,
            useUnscaledTime);
    }

    Observable<std::uint64_t> Interval(
        const float seconds,
        const bool useUnscaledTime)
    {
        const float safeSeconds =
            std::isfinite(seconds)
                ? std::max(seconds, 0.000001f)
                : 0.000001f;
        return Schedule(
            ScheduleKind::Interval,
            safeSeconds,
            useUnscaledTime);
    }

    namespace Detail
    {
        void AdvanceFrame(
            const float deltaTime,
            const float unscaledDeltaTime)
        {
            Scheduler().Advance(deltaTime, unscaledDeltaTime);
        }

        void Reset() noexcept
        {
            auto& scheduler = Scheduler();
            if (scheduler.publishDepth > 0)
            {
                for (auto& entry : scheduler.entries)
                {
                    entry.id = 0;
                    entry.observer = nullptr;
                }
                scheduler.needsCompaction = true;
                return;
            }
            scheduler.entries.clear();
        }
    }
}
