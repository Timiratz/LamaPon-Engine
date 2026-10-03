#include "LamaPon/Core/Profiler.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>

namespace
{
    // スレッドごとに開いている区間の添字です。
    // Profilerは1つだけなのでスレッドローカルに置き、generationが変わったら中身を捨てます。
    struct ThreadScopeStack final
    {
        // スタックの区間が属する世代
        std::uint64_t generation{};
        // 外側から並べた開いている区間
        std::vector<std::uint32_t> indices;
    };

    // 呼び出し元スレッドの区間スタック
    thread_local ThreadScopeStack t_scopeStack;

    // 現在の世代のスレッド別スタックを返します(generation: フレームの計測世代)。
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

    // JSON文字列の区切りや改行をエスケープします(value: 出力する区間名)。
    std::string EscapeJson(const std::string_view value)
    {
        // JSON用にエスケープした文字列
        std::string escaped;
        escaped.reserve(value.size());
        // エスケープを判定する各文字
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
        // プロセスで共有する計測状態
        static Profiler profiler;
        return profiler;
    }

    void Profiler::SetEnabled(const bool enabled) noexcept
    {
        // 計測状態と記録を保護するロック
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
        // 計測状態と記録を保護するロック
        std::scoped_lock lock(m_mutex);
        return m_enabled;
    }

    void Profiler::SetFrameCapacity(
        const std::size_t capacity) noexcept
    {
        // 計測状態と記録を保護するロック
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
        // 計測状態と記録を保護するロック
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
        // 親より後ろから探す開始位置
        const std::size_t first =
            parent == ProfileSample::NoParent
                ? 0
                : static_cast<std::size_t>(parent) + 1;
        // 同名かつ同じ親の区間を探す位置
        for (std::size_t index = first;
            index < m_currentSamples.size();
            ++index)
        {
            // 名前と親を照合する区間
            const auto& candidate = m_currentSamples[index];
            if (candidate.parent == parent
                && candidate.name == name)
            {
                return static_cast<std::uint32_t>(index);
            }
        }

        // 新規追加する計測区間の集計先
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
        // 計測状態と記録を保護するロック
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
        // フレーム計測の終了時刻
        const auto end = std::chrono::steady_clock::now();
        // 計測状態と記録を保護するロック
        std::scoped_lock lock(m_mutex);
        if (!m_enabled || !m_frameActive)
        {
            return;
        }

        // 完了して履歴へ追加するフレーム
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
        // 計測状態と記録を保護するロック
        std::scoped_lock lock(m_mutex);
        if (!m_enabled || !m_frameActive)
        {
            return;
        }

        // 記録先の親を探す区間スタック
        const auto& stack = CurrentScopeStack(m_generation);
        // 呼び出し元で開いている親区間
        const std::uint32_t parent = stack.indices.empty()
            ? ProfileSample::NoParent
            : stack.indices.back();
        // 計測済み時間を加算する集計先
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
        // 計測状態と記録を保護するロック
        std::scoped_lock lock(m_mutex);
        if (!m_enabled || !m_frameActive)
        {
            return {};
        }

        // 開始する区間を積むスタック
        auto& stack = CurrentScopeStack(m_generation);
        // 呼び出し元で開いている親区間
        const std::uint32_t parent = stack.indices.empty()
            ? ProfileSample::NoParent
            : stack.indices.back();
        // 開始する区間の集計先の添字
        const std::uint32_t index =
            FindOrAddSample(name, parent);
        // 未終了の区間も呼出数へ含めるため、開始時に数えます。
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

        // 計測状態と記録を保護するロック
        std::scoped_lock lock(m_mutex);
        if (t_scopeStack.generation == token.generation)
        {
            // 開始世代に属する未終了区間
            // 終了順が逆でなくても、対象だけを除いて他の区間の親子関係を保持します。
            auto& indices = t_scopeStack.indices;
            // 終了する区間の逆順探索位置
            const auto open = std::find(
                indices.rbegin(),
                indices.rend(),
                token.index);
            if (open != indices.rend())
            {
                indices.erase(std::next(open).base());
            }
        }
        // フレームが切り替わった後に閉じた区間は、別フレームの添字を指している可能性があるため加算しません。
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
        // 計測状態と記録を保護するロック
        std::scoped_lock lock(m_mutex);
        return m_frames;
    }

    std::uint64_t Profiler::LatestFrameIndex() const noexcept
    {
        // 計測状態と記録を保護するロック
        std::scoped_lock lock(m_mutex);
        return m_frames.empty() ? 0 : m_frames.back().index;
    }

    std::vector<ProfileFrame> Profiler::SnapshotSince(
        const std::uint64_t afterIndex) const
    {
        // 計測状態と記録を保護するロック
        std::scoped_lock lock(m_mutex);
        // 指定番号より後の最初の記録位置
        // 取得済み番号より後かを判定します(frame: 履歴の各フレーム)。
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
        // 計測状態と記録を保護するロック
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
            // 計測JSONを書き込むファイル
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
            // JSONへ書き込むフレームの位置
            for (std::size_t frameIndex{};
                frameIndex < frames.size();
                ++frameIndex)
            {
                // JSONへ書き込む計測フレーム
                const auto& frame = frames[frameIndex];
                output
                    << "    {\"index\": " << frame.index
                    << ", \"milliseconds\": "
                    << frame.milliseconds
                    << ", \"samples\": [";
                // フレーム内の区間記録の位置
                for (std::size_t sampleIndex{};
                    sampleIndex < frame.samples.size();
                    ++sampleIndex)
                {
                    // JSONへ書き込む区間の集計結果
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
                    // 最上位区間は版番号1との互換性を保つためparentを省略します。
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
            // 保存時点の計測履歴の複製
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
