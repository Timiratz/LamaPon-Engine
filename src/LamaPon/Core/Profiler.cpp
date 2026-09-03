#include "LamaPon/Core/Profiler.h"

#include <algorithm>
#include <fstream>
#include <iomanip>

namespace
{
    std::string EscapeJson(const std::string_view value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char character : value)
        {
            switch (character)
            {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += character;
                break;
            }
        }
        return escaped;
    }
}

namespace LamaPon
{
    Profiler& Profiler::Instance() noexcept
    {
        static Profiler profiler;
        return profiler;
    }

    void Profiler::SetEnabled(const bool enabled) noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_enabled = enabled;
        if (!enabled)
        {
            m_frameActive = false;
            m_currentSamples.clear();
        }
    }

    bool Profiler::IsEnabled() const noexcept
    {
        std::scoped_lock lock(m_mutex);
        return m_enabled;
    }

    void Profiler::SetFrameCapacity(
        const std::size_t capacity) noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_frameCapacity = std::max<std::size_t>(capacity, 1);
        if (m_frames.size() > m_frameCapacity)
        {
            m_frames.erase(
                m_frames.begin(),
                m_frames.end()
                    - static_cast<std::ptrdiff_t>(
                        m_frameCapacity));
        }
    }

    void Profiler::BeginFrame()
    {
        std::scoped_lock lock(m_mutex);
        if (!m_enabled)
        {
            return;
        }
        m_currentSamples.clear();
        m_frameStart = std::chrono::steady_clock::now();
        m_frameActive = true;
    }

    void Profiler::EndFrame()
    {
        const auto end = std::chrono::steady_clock::now();
        std::scoped_lock lock(m_mutex);
        if (!m_enabled || !m_frameActive)
        {
            return;
        }

        ProfileFrame frame;
        frame.index = m_nextFrameIndex++;
        frame.milliseconds =
            std::chrono::duration<double, std::milli>(
                end - m_frameStart).count();
        frame.samples = std::move(m_currentSamples);
        m_currentSamples.clear();
        m_frames.push_back(std::move(frame));
        if (m_frames.size() > m_frameCapacity)
        {
            m_frames.erase(m_frames.begin());
        }
        m_frameActive = false;
    }

    void Profiler::Record(
        const std::string_view name,
        const std::chrono::steady_clock::duration duration)
    {
        std::scoped_lock lock(m_mutex);
        if (!m_enabled || !m_frameActive)
        {
            return;
        }

        const double milliseconds =
            std::chrono::duration<double, std::milli>(
                duration).count();
        const auto sample = std::ranges::find_if(
            m_currentSamples,
            [name](const ProfileSample& candidate)
            {
                return candidate.name == name;
            });
        if (sample == m_currentSamples.end())
        {
            m_currentSamples.push_back({
                std::string(name),
                milliseconds,
                1
            });
            return;
        }
        sample->milliseconds += milliseconds;
        ++sample->callCount;
    }

    std::vector<ProfileFrame> Profiler::Snapshot() const
    {
        std::scoped_lock lock(m_mutex);
        return m_frames;
    }

    void Profiler::Clear() noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_frames.clear();
        m_currentSamples.clear();
        m_frameActive = false;
        m_nextFrameIndex = 1;
    }

    bool Profiler::WriteJson(
        const std::filesystem::path& path) const noexcept
    {
        try
        {
            const auto frames = Snapshot();
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(
                    path.parent_path());
            }
            std::ofstream output(
                path,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return false;
            }

            output << std::fixed << std::setprecision(4);
            output
                << "{\n  \"format\": \"LamaPonProfile\",\n"
                << "  \"version\": 1,\n  \"frames\": [\n";
            for (std::size_t frameIndex{};
                frameIndex < frames.size();
                ++frameIndex)
            {
                const auto& frame = frames[frameIndex];
                output
                    << "    {\"index\": " << frame.index
                    << ", \"milliseconds\": "
                    << frame.milliseconds
                    << ", \"samples\": [";
                for (std::size_t sampleIndex{};
                    sampleIndex < frame.samples.size();
                    ++sampleIndex)
                {
                    const auto& sample =
                        frame.samples[sampleIndex];
                    if (sampleIndex > 0)
                    {
                        output << ", ";
                    }
                    output
                        << "{\"name\": \""
                        << EscapeJson(sample.name)
                        << "\", \"milliseconds\": "
                        << sample.milliseconds
                        << ", \"calls\": "
                        << sample.callCount << '}';
                }
                output << "]}";
                if (frameIndex + 1 < frames.size())
                {
                    output << ',';
                }
                output << '\n';
            }
            output << "  ]\n}\n";
            return output.good();
        }
        catch (...)
        {
            return false;
        }
    }

    ProfileScope::ProfileScope(
        const std::string_view name) noexcept
        : m_name(name)
        , m_enabled(Profiler::Instance().IsEnabled())
    {
        if (m_enabled)
        {
            m_start = std::chrono::steady_clock::now();
        }
    }

    ProfileScope::~ProfileScope()
    {
        if (m_enabled)
        {
            Profiler::Instance().Record(
                m_name,
                std::chrono::steady_clock::now() - m_start);
        }
    }
}
