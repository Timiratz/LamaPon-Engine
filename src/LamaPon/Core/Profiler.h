#pragma once

#include "LamaPon/Core/Api.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    struct ProfileSample final
    {
        std::string name;
        double milliseconds{};
        std::uint32_t callCount{};
    };

    struct ProfileFrame final
    {
        std::uint64_t index{};
        double milliseconds{};
        std::vector<ProfileSample> samples;
    };

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

        LAMAPON_API void BeginFrame();
        LAMAPON_API void EndFrame();
        LAMAPON_API void Record(
            std::string_view name,
            std::chrono::steady_clock::duration duration);

        [[nodiscard]] LAMAPON_API std::vector<ProfileFrame>
            Snapshot() const;
        LAMAPON_API void Clear() noexcept;
        [[nodiscard]] LAMAPON_API bool WriteJson(
            const std::filesystem::path& path) const noexcept;

    private:
        Profiler() = default;

        mutable std::mutex m_mutex;
        std::vector<ProfileSample> m_currentSamples;
        std::vector<ProfileFrame> m_frames;
        std::chrono::steady_clock::time_point m_frameStart;
        std::size_t m_frameCapacity{ 240 };
        std::uint64_t m_nextFrameIndex{ 1 };
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
        std::string m_name;
        std::chrono::steady_clock::time_point m_start;
        bool m_enabled{};
    };
}

#define LAMAPON_PROFILE_CONCATENATE_INNER(left, right) left##right
#define LAMAPON_PROFILE_CONCATENATE(left, right) \
    LAMAPON_PROFILE_CONCATENATE_INNER(left, right)
#define LAMAPON_PROFILE_SCOPE(name) \
    ::LamaPon::ProfileScope LAMAPON_PROFILE_CONCATENATE( \
        lamaponProfileScope_, __LINE__){ name }
