#include "LamaPon/Core/Profiler.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>

namespace
{
    // スレッドごとに開いている区間の添字です。Profilerは1つだけなので
    // スレッドローカルに置き、generationが変わったら中身を捨てます。
    struct ThreadScopeStack final
    {
        std::uint64_t generation{};
        std::vector<std::uint32_t> indices;
    };

    thread_local ThreadScopeStack t_scopeStack;

    ThreadScopeStack& CurrentScopeStack(
        const std::uint64_t generation) noexcept
    {
        if (t_scopeStack.generation != generation)
        {
            t_scopeStack.generation = generation;
            t_scopeStack.indices.clear();
        }
        return t_scopeStack;
    }

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
            InvalidateOpenScopes();
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

    std::size_t Profiler::FrameCapacity() const noexcept
    {
        std::scoped_lock lock(m_mutex);
        return m_frameCapacity;
    }

    void Profiler::InvalidateOpenScopes() noexcept
    {
        ++m_generation;
    }

    std::uint32_t Profiler::FindOrAddSample(
        const std::string_view name,
        const std::uint32_t parent)
    {
        // 子は親より後ろにしか登録されないため、親の位置から探します。
        const std::size_t first =
            parent == ProfileSample::NoParent
                ? 0
                : static_cast<std::size_t>(parent) + 1;
        for (std::size_t index = first;
            index < m_currentSamples.size();
            ++index)
        {
            const auto& candidate = m_currentSamples[index];
            if (candidate.parent == parent
                && candidate.name == name)
            {
                return static_cast<std::uint32_t>(index);
            }
        }

        ProfileSample sample;
        sample.name = std::string(name);
        sample.parent = parent;
        sample.depth =
            parent == ProfileSample::NoParent
                ? 0
                : m_currentSamples[parent].depth + 1;
        m_currentSamples.push_back(std::move(sample));
        return static_cast<std::uint32_t>(
            m_currentSamples.size() - 1);
    }

    void Profiler::BeginFrame()
    {
        std::scoped_lock lock(m_mutex);
        if (!m_enabled)
        {
            return;
        }
        m_currentSamples.clear();
        InvalidateOpenScopes();
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
        InvalidateOpenScopes();
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

        const auto& stack = CurrentScopeStack(m_generation);
        const std::uint32_t parent = stack.indices.empty()
            ? ProfileSample::NoParent
            : stack.indices.back();
        auto& sample =
            m_currentSamples[FindOrAddSample(name, parent)];
        sample.milliseconds +=
            std::chrono::duration<double, std::milli>(
                duration).count();
        ++sample.callCount;
    }

    ProfileScopeToken Profiler::BeginScope(
        const std::string_view name)
    {
        std::scoped_lock lock(m_mutex);
        if (!m_enabled || !m_frameActive)
        {
            return {};
        }

        auto& stack = CurrentScopeStack(m_generation);
        const std::uint32_t parent = stack.indices.empty()
            ? ProfileSample::NoParent
            : stack.indices.back();
        const std::uint32_t index =
            FindOrAddSample(name, parent);
        // 呼び出し回数は開始時に数えます。フレーム末尾で閉じられなかった
        // 区間も「呼ばれた」ことは残ります。
        ++m_currentSamples[index].callCount;
        stack.indices.push_back(index);
        return { m_generation, index };
    }

    void Profiler::EndScope(
        const ProfileScopeToken& token,
        const std::chrono::steady_clock::duration duration) noexcept
    {
        if (!token.IsValid())
        {
            return;
        }

        std::scoped_lock lock(m_mutex);
        if (t_scopeStack.generation == token.generation)
        {
            // 通常は末尾にありますが、GPU区間のEnd()のように内側の区間より
            // 先に閉じられた場合も、自分の添字だけを取り除いて後続の親子
            // 関係を保ちます。
            auto& indices = t_scopeStack.indices;
            const auto open = std::find(
                indices.rbegin(),
                indices.rend(),
                token.index);
            if (open != indices.rend())
            {
                indices.erase(std::next(open).base());
            }
        }
        // フレームが切り替わった後に閉じた区間は、別フレームの添字を
        // 指している可能性があるため加算しません。
        if (!m_enabled
            || !m_frameActive
            || token.generation != m_generation
            || token.index >= m_currentSamples.size())
        {
            return;
        }
        m_currentSamples[token.index].milliseconds +=
            std::chrono::duration<double, std::milli>(
                duration).count();
    }

    std::vector<ProfileFrame> Profiler::Snapshot() const
    {
        std::scoped_lock lock(m_mutex);
        return m_frames;
    }

    std::uint64_t Profiler::LatestFrameIndex() const noexcept
    {
        std::scoped_lock lock(m_mutex);
        return m_frames.empty() ? 0 : m_frames.back().index;
    }

    std::vector<ProfileFrame> Profiler::SnapshotSince(
        const std::uint64_t afterIndex) const
    {
        std::scoped_lock lock(m_mutex);
        // indexは単調増加なので、条件を満たす範囲は末尾側に連続します。
        const auto first = std::ranges::find_if(
            m_frames,
            [afterIndex](const ProfileFrame& frame)
            {
                return frame.index > afterIndex;
            });
        return { first, m_frames.end() };
    }

    void Profiler::Clear() noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_frames.clear();
        m_currentSamples.clear();
        m_frameActive = false;
        m_nextFrameIndex = 1;
        InvalidateOpenScopes();
    }

    bool WriteProfileJson(
        const std::filesystem::path& path,
        const std::span<const ProfileFrame> frames) noexcept
    {
        try
        {
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
                << "  \"version\": 2,\n  \"frames\": [\n";
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
                        << sample.callCount
                        << ", \"depth\": "
                        << sample.depth;
                    // 最上位区間はparentを省略します。version 1の
                    // 読み込み側と同じ形のまま扱えます。
                    if (sample.parent != ProfileSample::NoParent)
                    {
                        output << ", \"parent\": " << sample.parent;
                    }
                    output << '}';
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

    bool Profiler::WriteJson(
        const std::filesystem::path& path) const noexcept
    {
        try
        {
            const auto frames = Snapshot();
            return WriteProfileJson(path, frames);
        }
        catch (...)
        {
            return false;
        }
    }

    ProfileScope::ProfileScope(
        const std::string_view name) noexcept
    {
        // 計測の失敗（メモリ不足など）で呼び出し側の処理を止めないよう、
        // 例外は計測なしとして扱います。
        try
        {
            m_token = Profiler::Instance().BeginScope(name);
        }
        catch (...)
        {
            m_token = {};
        }
        if (m_token.IsValid())
        {
            m_start = std::chrono::steady_clock::now();
        }
    }

    ProfileScope::~ProfileScope()
    {
        if (m_token.IsValid())
        {
            Profiler::Instance().EndScope(
                m_token,
                std::chrono::steady_clock::now() - m_start);
        }
    }
}
