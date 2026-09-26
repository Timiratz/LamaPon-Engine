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
        // parentが最上位区間であることを示す値です。
        static constexpr std::uint32_t NoParent = 0xFFFFFFFFu;

        std::string name;
        double milliseconds{};
        std::uint32_t callCount{};
        // 同じフレームのsamples内で親区間を指す添字です。親は必ず子より
        // 前に並ぶため、先頭から走査するだけで呼び出し木を組み立てられます。
        // 同じ親の下で同じ名前の区間は1つに集計します。
        std::uint32_t parent{ NoParent };
        // 入れ子の深さです（最上位が0）。
        std::uint32_t depth{};
    };

    struct ProfileFrame final
    {
        std::uint64_t index{};
        double milliseconds{};
        std::vector<ProfileSample> samples;
    };

    // ProfileScopeが開始時に受け取り、終了時に返す識別子です。
    // generationが現在のフレームと一致しない終了は、フレームをまたいだ
    // 区間として破棄します。
    struct ProfileScopeToken final
    {
        std::uint64_t generation{};
        std::uint32_t index{ ProfileSample::NoParent };

        [[nodiscard]] bool IsValid() const noexcept
        {
            return index != ProfileSample::NoParent;
        }
    };

    // framesをLamaPonProfile形式（version 2）のJSONへ書き出します。
    // Profiler::WriteJsonと、解析範囲を絞った保存の両方で使います。
    [[nodiscard]] LAMAPON_API bool WriteProfileJson(
        const std::filesystem::path& path,
        std::span<const ProfileFrame> frames) noexcept;

    class Profiler final
    {
    public:
        [[nodiscard]] static LAMAPON_API Profiler&
            Instance() noexcept;

        Profiler(const Profiler&) = delete;
        Profiler& operator=(const Profiler&) = delete;

        LAMAPON_API void SetEnabled(bool enabled) noexcept;
        [[nodiscard]] LAMAPON_API bool
            IsEnabled() const noexcept;
        LAMAPON_API void SetFrameCapacity(
            std::size_t capacity) noexcept;
        [[nodiscard]] LAMAPON_API std::size_t
            FrameCapacity() const noexcept;

        LAMAPON_API void BeginFrame();
        LAMAPON_API void EndFrame();
        // 呼び出し元スレッドで開いている区間の子として、終了済みの時間を
        // 加算します。区間の入れ子を自分で管理しない計測値の登録用です。
        LAMAPON_API void Record(
            std::string_view name,
            std::chrono::steady_clock::duration duration);
        // 区間を開始し、呼び出し元スレッドの区間スタックへ積みます。
        // 無効時やフレーム外では無効なtokenを返します。開始と終了は
        // 同じスレッドで、開始と逆順に対応させてください。
        [[nodiscard]] LAMAPON_API ProfileScopeToken BeginScope(
            std::string_view name);
        LAMAPON_API void EndScope(
            const ProfileScopeToken& token,
            std::chrono::steady_clock::duration duration) noexcept;

        [[nodiscard]] LAMAPON_API std::vector<ProfileFrame>
            Snapshot() const;
        // 記録済みの最新フレームのindexです（無ければ0）。Clearで
        // indexが1から振り直されたことを表示側が検出するために使います。
        [[nodiscard]] LAMAPON_API std::uint64_t
            LatestFrameIndex() const noexcept;
        // indexがafterIndexより大きいフレームだけを古い順に返します。
        // 表示側が履歴を差分で取り込み、毎フレームの全件コピーを避けます。
        [[nodiscard]] LAMAPON_API std::vector<ProfileFrame>
            SnapshotSince(std::uint64_t afterIndex) const;
        LAMAPON_API void Clear() noexcept;
        [[nodiscard]] LAMAPON_API bool WriteJson(
            const std::filesystem::path& path) const noexcept;

    private:
        Profiler() = default;

        // m_mutexを保持した状態で呼びます。
        [[nodiscard]] std::uint32_t FindOrAddSample(
            std::string_view name,
            std::uint32_t parent);
        void InvalidateOpenScopes() noexcept;

        mutable std::mutex m_mutex;
        std::vector<ProfileSample> m_currentSamples;
        std::vector<ProfileFrame> m_frames;
        std::chrono::steady_clock::time_point m_frameStart;
        std::size_t m_frameCapacity{ 240 };
        std::uint64_t m_nextFrameIndex{ 1 };
        // フレームの開始・終了・消去ごとに進め、各スレッドの区間スタックと
        // 開いたままのProfileScopeを無効にします。
        std::uint64_t m_generation{ 1 };
        bool m_enabled{ true };
        bool m_frameActive{};
    };

    class ProfileScope final
    {
    public:
        LAMAPON_API explicit ProfileScope(
            std::string_view name) noexcept;
        LAMAPON_API ~ProfileScope();

        ProfileScope(const ProfileScope&) = delete;
        ProfileScope& operator=(const ProfileScope&) = delete;

    private:
        ProfileScopeToken m_token;
        std::chrono::steady_clock::time_point m_start;
    };
}

#define LAMAPON_PROFILE_CONCATENATE_INNER(left, right) left##right
#define LAMAPON_PROFILE_CONCATENATE(left, right) \
    LAMAPON_PROFILE_CONCATENATE_INNER(left, right)
#define LAMAPON_PROFILE_SCOPE(name) \
    ::LamaPon::ProfileScope LAMAPON_PROFILE_CONCATENATE( \
        lamaponProfileScope_, __LINE__){ name }
