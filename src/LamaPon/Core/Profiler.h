#pragma once

#include "LamaPon/Core/Api.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    struct ProfileSample final
    {
        // 親区間がないことを示す添字
        static constexpr std::uint32_t NoParent = 0xFFFFFFFFu;

        // 計測区間の名前
        std::string name;
        // 区間の累積時間、ミリ秒
        double milliseconds{};
        // 同名かつ同じ親の区間の呼出数
        std::uint32_t callCount{};
        // 同じフレーム内の親区間の添字
        // 親は必ず子より前に並ぶため、先頭から走査するだけで呼び出し木を組み立てられます。
        // 同じ親の下で同じ名前の区間は1つに集計します。
        std::uint32_t parent{ NoParent };
        // 区間の入れ子の深さ、最上位は0
        std::uint32_t depth{};
    };

    struct ProfileFrame final
    {
        // 記録フレームの通し番号
        std::uint64_t index{};
        // フレームの経過時間、ミリ秒
        double milliseconds{};
        // フレーム内の計測区間の集計
        std::vector<ProfileSample> samples;
    };

    // ProfileScopeが開始時に受け取り、終了時に返す識別子です。
    // generationが現在のフレームと一致しない終了は、フレームをまたいだ区間として破棄します。
    struct ProfileScopeToken final
    {
        // 区間を開始したフレームの世代
        std::uint64_t generation{};
        // 開始した区間の集計先の添字
        std::uint32_t index{ ProfileSample::NoParent };

        // 計測区間を開始できた識別子かを返します。
        [[nodiscard]] bool IsValid() const noexcept
        {
            return index != ProfileSample::NoParent;
        }
    };

    // 計測フレームをJSONへ保存し成功可否を返します(path: 保存先, frames: 保存するフレーム列)。
    [[nodiscard]] LAMAPON_API bool WriteProfileJson(
        const std::filesystem::path& path,
        std::span<const ProfileFrame> frames) noexcept;

    class Profiler final
    {
    public:
        // プロセスで共有するプロファイラーを返します。
        [[nodiscard]] static LAMAPON_API Profiler&
            Instance() noexcept;

        // 共有プロファイラーのコピーを禁止します。
        Profiler(const Profiler&) = delete;
        // 共有プロファイラーのコピー代入を禁止します。
        Profiler& operator=(const Profiler&) = delete;

        // 計測を切り替え、無効化時は進行中のフレームを破棄します(enabled: 計測を有効にするか)。
        LAMAPON_API void SetEnabled(bool enabled) noexcept;
        // 計測が有効かを返します。
        [[nodiscard]] LAMAPON_API bool
            IsEnabled() const noexcept;
        // 記録するフレーム数の上限を最小1で設定します(capacity: 保持する最大フレーム数)。
        LAMAPON_API void SetFrameCapacity(
            std::size_t capacity) noexcept;
        // 記録するフレーム数の上限を返します。
        [[nodiscard]] LAMAPON_API std::size_t
            FrameCapacity() const noexcept;

        // 前の開いた区間を無効にしてフレームの計測を開始します。
        LAMAPON_API void BeginFrame();
        // フレームを記録し、閉じていない区間を無効にします。
        LAMAPON_API void EndFrame();
        // 現在の親区間へ計測済みの時間を登録します(name: 計測区間名, duration: 計測した経過時間)。
        LAMAPON_API void Record(
            std::string_view name,
            std::chrono::steady_clock::duration duration);
        // 呼び出し元スレッドで計測区間を開始します(name: 計測区間名)。
        // 計測無効時やフレーム外では無効な識別子を返し、終了は開始時と同じスレッドで行います。
        [[nodiscard]] LAMAPON_API ProfileScopeToken BeginScope(
            std::string_view name);
        // 計測区間を終了して時間を加算します(token: 開始時の識別子, duration: 計測した経過時間)。
        // 開始時の世代に対応するスタックから取り除き、別フレームには加算しません。
        LAMAPON_API void EndScope(
            const ProfileScopeToken& token,
            std::chrono::steady_clock::duration duration) noexcept;

        // 保持する計測フレームを古い順で複製して返します。
        [[nodiscard]] LAMAPON_API std::vector<ProfileFrame>
            Snapshot() const;
        // 記録済みの最新フレーム番号を返し、記録がなければ0を返します。
        [[nodiscard]] LAMAPON_API std::uint64_t
            LatestFrameIndex() const noexcept;
        // 指定番号より後のフレームを古い順に返します(afterIndex: 取得済みの最終フレーム番号)。
        [[nodiscard]] LAMAPON_API std::vector<ProfileFrame>
            SnapshotSince(std::uint64_t afterIndex) const;
        // 計測記録と進行中の区間を消去し、フレーム番号を1へ戻します。
        LAMAPON_API void Clear() noexcept;
        // 保持する計測フレームをJSON保存し成功可否を返します(path: 保存先)。
        [[nodiscard]] LAMAPON_API bool WriteJson(
            const std::filesystem::path& path) const noexcept;

    private:
        // 共有プロファイラーの初期状態を構築します。
        Profiler() = default;

        // 同じ親と名前の集計先を検索または追加します(name: 区間名, parent: 親区間の添字)。
        // 呼び出し側でm_mutexを保持します。
        [[nodiscard]] std::uint32_t FindOrAddSample(
            std::string_view name,
            std::uint32_t parent);
        // 世代を進めて開いたままの計測区間を無効にします。
        void InvalidateOpenScopes() noexcept;

        // 計測状態と記録の排他制御
        mutable std::mutex m_mutex;
        // 進行中のフレームの区間集計
        std::vector<ProfileSample> m_currentSamples;
        // 完了した計測フレームの履歴
        std::vector<ProfileFrame> m_frames;
        // 進行中のフレームの開始時刻
        std::chrono::steady_clock::time_point m_frameStart;
        // 保持する最大フレーム数
        std::size_t m_frameCapacity{ 240 };
        // 次に記録するフレームの番号
        std::uint64_t m_nextFrameIndex{ 1 };
        // 計測区間の有効性を判定する世代
        std::uint64_t m_generation{ 1 };
        // 計測が有効か
        bool m_enabled{ true };
        // フレームを計測中か
        bool m_frameActive{};
    };

    class ProfileScope final
    {
    public:
        // スコープの計測を開始します(name: 計測区間名)。
        // 開始失敗時は計測を省略し、呼び出し側の処理を継続します。
        LAMAPON_API explicit ProfileScope(
            std::string_view name) noexcept;
        // スコープの経過時間を登録して計測を終了します。
        LAMAPON_API ~ProfileScope();

        // 計測区間のコピーを禁止します。
        ProfileScope(const ProfileScope&) = delete;
        // 計測区間のコピー代入を禁止します。
        ProfileScope& operator=(const ProfileScope&) = delete;

    private:
        // 開始した計測区間の識別子
        ProfileScopeToken m_token;
        // スコープ計測の開始時刻
        std::chrono::steady_clock::time_point m_start;
    };
}

// 識別子を連結します(left: 左側の識別子, right: 右側の識別子)。
#define LAMAPON_PROFILE_CONCATENATE_INNER(left, right) left##right
// マクロを展開してから識別子を連結します(left: 左側の識別子, right: 右側の識別子)。
#define LAMAPON_PROFILE_CONCATENATE(left, right) \
    LAMAPON_PROFILE_CONCATENATE_INNER(left, right)
// 現在のスコープが終わるまでの計測を生成します(name: 計測区間名)。
#define LAMAPON_PROFILE_SCOPE(name) \
    ::LamaPon::ProfileScope LAMAPON_PROFILE_CONCATENATE( \
        lamaponProfileScope_, __LINE__){ name }
