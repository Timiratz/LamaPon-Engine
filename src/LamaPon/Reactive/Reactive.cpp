#include "LamaPon/Reactive/Reactive.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
    enum class ScheduleKind
    {
        // 次のゲーム更新に1回通知
        NextFrame,
        // ゲーム更新ごとに通知
        EveryFrame,
        // 指定時間後に1回通知
        Timer,
        // 指定間隔で繰り返し通知
        Interval
    };

    struct ScheduledEntry final
    {
        // 通知予約の識別番号
        std::uint64_t id{};
        // 予約した通知の種類
        ScheduleKind kind{};
        // 次回通知までの残り秒数
        float remaining{};
        // 繰り返し通知の間隔秒数
        float interval{};
        // 時間倍率を無視する設定
        bool useUnscaledTime{};
        // 次回通知する連番
        std::uint64_t nextValue{};
        // 予約の同期通知先
        LamaPon::Observable<std::uint64_t>::Observer observer;
    };

    struct SchedulerState final
    {
        // 保持する通知予約列
        std::vector<ScheduledEntry> entries;
        // 次に発行する予約番号
        std::uint64_t nextId{ 1 };
        // 予約通知の再入深度
        int publishDepth{};
        // 更新後に必要な予約圧縮
        bool needsCompaction{};

        // 通知中なら削除を遅らせて予約を解除する(id: 対象の予約番号)。
        void Remove(const std::uint64_t id) noexcept
        {
            // 確認中の通知予約
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
                    // 指定予約を取り除く(candidate: 確認する予約)。
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

        // 最外の更新が終わったとき解除済み予約を詰める。
        void FinishAdvance() noexcept
        {
            --publishDepth;
            if (publishDepth == 0 && needsCompaction)
            {
                needsCompaction = false;
                // 解除済みの予約を詰める(entry: 確認する予約)。
                std::erase_if(
                    entries,
                    [](const ScheduledEntry& entry)
                    {
                        return entry.id == 0;
                    });
            }
        }

        // 通知先の例外は以降の予約処理を中断し、呼び出し元へ伝える。
        // 開始時の予約を最大1回ずつ通知する(deltaTime: 倍率適用後の秒数, unscaledDeltaTime: 倍率適用前の秒数)。
        void Advance(const float deltaTime, const float unscaledDeltaTime)
        {
            // 更新開始時の予約件数
            const auto count = entries.size();
            ++publishDepth;
            try
            {
                // 開始時の通知予約番号
                for (std::size_t index = 0; index < count; ++index)
                {
                    // 確認中の通知予約
                    auto& entry = entries[index];
                    if (entry.id == 0 || entry.observer == nullptr)
                    {
                        continue;
                    }

                    // 今回の通知期限の到達
                    bool shouldPublish = false;
                    // 通知後に終了する予約
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
                        // 予約で使う経過秒数
                        const float elapsed = entry.useUnscaledTime
                            ? unscaledDeltaTime
                            : deltaTime;
                        entry.remaining -= std::max(elapsed, 0.0f);
                        shouldPublish = entry.remaining <= 0.0f;
                        completes = shouldPublish
                            && entry.kind == ScheduleKind::Timer;
                        // 各更新の通知は最大1回とし、遅延分をまとめて発行しない。
                        if (shouldPublish
                            && entry.kind == ScheduleKind::Interval)
                        {

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
                    // 通知予約の識別番号
                    const auto id = entry.id;
                    // 通知する予約の連番
                    const auto value = entry.nextValue++;
                    // 今回の同期通知先
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

    // メインスレッド用の共有予約状態を返す。
    SchedulerState& Scheduler()
    {
        // 共有する通知予約状態
        static SchedulerState state;
        return state;
    }

    // 購読時に予約を追加する通知窓口を作る(kind: 通知種別, seconds: 待機秒数, useUnscaledTime: 時間倍率を無視するか)。
    LamaPon::Observable<std::uint64_t> Schedule(
        const ScheduleKind kind,
        const float seconds,
        const bool useUnscaledTime)
    {
        // 購読時に通知先を登録する(observer: 連番を受け取る通知先)。
        return LamaPon::Observable<std::uint64_t>{
            [kind, seconds, useUnscaledTime](
                LamaPon::Observable<std::uint64_t>::Observer observer)
            {
                if (observer == nullptr)
                {
                    return LamaPon::Subscription{};
                }
                // メインスレッドの予約状態
                auto& scheduler = Scheduler();
                // 通知予約の識別番号
                const auto id = scheduler.nextId++;
                scheduler.entries.push_back({
                    id,
                    kind,
                    std::max(seconds, 0.0f),
                    std::max(seconds, 0.000001f),
                    useUnscaledTime,
                    0,
                    std::move(observer) });
                // 識別番号に対応する予約を解除する。
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
        // 最低1マイクロ秒の通知間隔
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
            // メインスレッドの予約状態
            auto& scheduler = Scheduler();
            if (scheduler.publishDepth > 0)
            {
                // 確認中の通知予約
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
