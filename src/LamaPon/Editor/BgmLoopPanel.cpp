#include "LamaPon/Editor/BgmLoopPanel.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace LamaPon
{
    BgmLoopPanel::BgmLoopPanel(
        AudioSystem& audio, AssetManager& assets,
        std::filesystem::path projectRoot,
        std::filesystem::path catalogPath, StatusSink status)
        : m_audio(audio), m_assets(assets)
        , m_projectRoot(std::move(projectRoot))
        , m_catalogPath(std::move(catalogPath))
        , m_status(std::move(status))
    {
        Load();
    }

    BgmLoopPanel::~BgmLoopPanel()
    {
        StopPreview();
    }

    void BgmLoopPanel::SetStatus(std::string message, const bool error) const
    {
        if (m_status)
        {
            m_status(std::move(message), error);
        }
    }

    bool BgmLoopPanel::Load()
    {
        StopPreview();
        try
        {
            const auto path = m_catalogPath;
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw std::runtime_error("BGMカタログJSONを開けません");
            }
            auto document = std::make_unique<nlohmann::json>();
            input >> *document;
            if (!document->contains("tracks")
                || !document->at("tracks").is_array()
                || document->at("tracks").empty())
            {
                throw std::runtime_error("tracks配列がありません");
            }
            // 描画側が参照する必須文字列をロード境界で検証し、
            // 壊れたカタログでエディターのフレーム全体を中断させません。
            for (const auto& track : document->at("tracks"))
            {
                if (!track.is_object()
                    || !track.contains("name") || !track.at("name").is_string()
                    || !track.contains("asset") || !track.at("asset").is_string())
                {
                    throw std::runtime_error("各trackにはnameとassetの文字列が必要です");
                }
            }
            m_state.document = std::move(document);
            m_state.selectedTrack = 0;
            m_state.waveformTrack = -1;
            m_state.waveformPeaks.clear();
            m_state.totalFrames = 0;
            m_state.loaded = true;
            m_state.dirty = false;
            m_state.hasLastStart = false;
            SetStatus("BGMカタログを読み込みました");
            return true;
        }
        catch (const std::exception& exception)
        {
            m_state.loaded = false;
            SetStatus(
                std::string{ "BGMループエディタを読み込めません: " }
                + exception.what(),
                true);
            return false;
        }
    }

    bool BgmLoopPanel::Save()
    {
        try
        {
            const auto path = m_catalogPath;
            const auto temporary = path.wstring() + L".tmp";
            {
                std::ofstream output(
                    temporary, std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    throw std::runtime_error("一時ファイルを作成できません");
                }
                output << m_state.document->dump(2) << '\n';
                output.flush();
                if (!output)
                {
                    throw std::runtime_error("BGMカタログを書き込めません");
                }
            }
            if (std::filesystem::exists(path))
            {
                CopyFileW(
                    path.c_str(), (path.wstring() + L".bak").c_str(), FALSE);
            }
            if (!MoveFileExW(
                    temporary.c_str(),
                    path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                DeleteFileW(temporary.c_str());
                throw std::runtime_error("BGMカタログを置き換えられません");
            }
            m_state.dirty = false;
            SetStatus("BGMカタログを保存しました");
            return true;
        }
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "BGMカタログを保存できません: " }
                + exception.what(),
                true);
            return false;
        }
    }

    void BgmLoopPanel::BuildWaveform()
    {
        auto& state = m_state;
        state.waveformTrack = state.selectedTrack;
        state.waveformPeaks.clear();
        state.totalFrames = 0;
        if (!state.loaded || !state.document)
        {
            return;
        }
        const auto& tracks = state.document->at("tracks");
        if (state.selectedTrack < 0
            || state.selectedTrack >= static_cast<int>(tracks.size()))
        {
            return;
        }
        try
        {
            const auto root =
                m_projectRoot;
            const auto asset = root / PathFromUtf8(
                tracks.at(static_cast<std::size_t>(state.selectedTrack))
                    .value("asset", std::string{}));
            // 波形専用のstreamで読みます。試聴中のstreamで読むと
            // その場の再生が途切れます。
            auto probe = m_audio.CreateStream(
                m_assets, asset);
            constexpr std::size_t buckets = 1200;
            state.waveformPeaks.assign(buckets, 0.0f);
            probe->ReadPeakEnvelope(state.waveformPeaks.data(), buckets);
            state.totalFrames = probe->TotalFrames();
            state.sampleRate = probe->SampleRate() > 0
                ? probe->SampleRate()
                : 44100;
        }
        catch (const std::exception& exception)
        {
            state.waveformPeaks.clear();
            SetStatus(
                std::string{ "波形を作れません: " } + exception.what(), true);
        }
    }

    void BgmLoopPanel::StartPreview(
        const std::uint64_t fromFrame, const bool usePreviewRange)
    {
        auto& state = m_state;
        if (!state.loaded || !state.document)
        {
            return;
        }
        auto& tracks = state.document->at("tracks");
        if (state.selectedTrack < 0
            || state.selectedTrack >= static_cast<int>(tracks.size()))
        {
            return;
        }
        try
        {
            if (!state.preview || state.previewTrack != state.selectedTrack)
            {
                const auto root =
                    m_projectRoot;
                const auto asset = root / PathFromUtf8(
                    tracks.at(static_cast<std::size_t>(state.selectedTrack))
                        .value("asset", std::string{}));
                state.preview = m_audio.CreateStream(
                    m_assets, asset);
                state.preview->SetLevelMeterEnabled(true);
                state.previewTrack = state.selectedTrack;
            }
            auto& track =
                tracks.at(static_cast<std::size_t>(state.selectedTrack));
            // 編集中の値をそのまま入れるので、保存しなくても継ぎ目を
            // 聞いて確かめられます。
            state.preview->SetLoop(true);
            if (usePreviewRange)
            {
                // ゲームの選曲画面と同じ鳴り方（試聴範囲を繰り返す）。
                state.preview->SetLoopRegionFrames(
                    track.value(
                        "preview_start_frame", std::uint64_t{ 0 }),
                    track.value("preview_end_frame", std::uint64_t{ 0 }),
                    0);
            }
            else
            {
                state.preview->SetLoopRegionFrames(
                    track.value("loop_start_frame", std::uint64_t{ 0 }),
                    track.value("loop_end_frame", std::uint64_t{ 0 }),
                    track.value(
                        "loop_crossfade_frames", std::uint64_t{ 0 }));
            }
            state.preview->SetStartFrame(fromFrame);
            state.preview->SetVolume(state.previewVolume);
            state.preview->Play();
            state.lastStartFrame = fromFrame;
            state.lastUsedPreviewRange = usePreviewRange;
            state.hasLastStart = true;
        }
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "BGMを再生できません: " } + exception.what(),
                true);
        }
    }

    void BgmLoopPanel::StopPreview() noexcept
    {
        auto& state = m_state;
        if (state.preview)
        {
            state.preview->Stop();
        }
        state.preview.reset();
        state.previewTrack = -1;
    }

    void BgmLoopPanel::Draw(
        const std::string& title, bool& open,
        const std::function<void()>& onSaved)
    {
        auto& state = m_state;
        ImGui::SetNextWindowSize(
            ImVec2{ 940.0f, 620.0f }, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
                title.c_str(), &open, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }
        if (!state.loaded || !state.document)
        {
            ImGui::TextUnformatted("BGMカタログを読み込めませんでした。");
            if (ImGui::Button("再読み込み"))
            {
                Load();
            }
            ImGui::End();
            return;
        }

        auto& tracks = state.document->at("tracks");
        std::vector<const char*> labels;
        labels.reserve(tracks.size());
        for (auto& entry : tracks)
        {
            labels.push_back(
                entry.at("name").get_ref<std::string&>().c_str());
        }
        const int previousTrack = state.selectedTrack;
        ImGui::SetNextItemWidth(320.0f);
        ImGui::Combo(
            "曲", &state.selectedTrack, labels.data(),
            static_cast<int>(labels.size()));
        if (state.selectedTrack != previousTrack)
        {
            StopPreview();
            // 別の曲の位置を引きずらない。
            state.hasLastStart = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("保存"))
        {
            if (Save() && onSaved)
            {
                onSaved();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("再読み込み"))
        {
            // documentごと差し替わるので、この後のtracks参照は無効に
            // なります。この行より先はこのフレームでは描きません。
            Load();
            ImGui::End();
            return;
        }
        if (state.dirty)
        {
            ImGui::SameLine();
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.72f, 0.2f, 1.0f }, "未保存");
        }
        ImGui::Separator();

        if (state.waveformTrack != state.selectedTrack)
        {
            BuildWaveform();
        }

        auto& track =
            tracks.at(static_cast<std::size_t>(state.selectedTrack));
        const auto rate = static_cast<double>(
            state.sampleRate > 0 ? state.sampleRate : 44100);
        const std::uint64_t totalFrames = state.totalFrames;
        auto loopStart = track.value("loop_start_frame", std::uint64_t{ 0 });
        auto loopEnd = track.value("loop_end_frame", std::uint64_t{ 0 });
        auto crossfade =
            track.value("loop_crossfade_frames", std::uint64_t{ 0 });
        auto introStart =
            track.value("intro_start_frame", std::uint64_t{ 0 });
        auto previewStart =
            track.value("preview_start_frame", std::uint64_t{ 0 });
        auto previewEnd =
            track.value("preview_end_frame", std::uint64_t{ 0 });

        auto clampFrame = [&](const std::uint64_t frame)
        {
            return totalFrames > 0 ? std::min(frame, totalFrames) : frame;
        };
        auto writeFrame = [&](const char* key, const std::uint64_t frame)
        {
            track[key] = clampFrame(frame);
            // 秒はカタログの読み物であると同時に検証にも使うので、
            // frameを変えたら必ず一緒に書き換えます。
            const std::string_view name{ key };
            if (name == "preview_start_frame"
                || name == "preview_end_frame"
                || name == "intro_start_frame")
            {
                track[std::string{ name.substr(0, name.size() - 5) }
                    + "seconds"] =
                    std::round(
                        static_cast<double>(clampFrame(frame)) / rate * 1000.0)
                    / 1000.0;
            }
            state.dirty = true;
        };

        // ---- 波形 ----
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 area{
            std::max(ImGui::GetContentRegionAvail().x, 240.0f), 170.0f };
        ImGui::InvisibleButton("##BgmWaveform", area);
        const bool waveformHovered = ImGui::IsItemHovered();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(
            origin,
            ImVec2{ origin.x + area.x, origin.y + area.y },
            IM_COL32(13, 18, 15, 255));
        auto frameToX = [&](const std::uint64_t frame)
        {
            const double ratio = totalFrames > 0
                ? static_cast<double>(frame) / static_cast<double>(totalFrames)
                : 0.0;
            return origin.x
                + static_cast<float>(std::clamp(ratio, 0.0, 1.0)) * area.x;
        };
        auto xToFrame = [&](const float x)
        {
            const double ratio = std::clamp(
                static_cast<double>(x - origin.x)
                    / static_cast<double>(area.x),
                0.0, 1.0);
            return static_cast<std::uint64_t>(
                ratio * static_cast<double>(totalFrames));
        };
        if (loopEnd > loopStart)
        {
            draw->AddRectFilled(
                ImVec2{ frameToX(loopStart), origin.y },
                ImVec2{ frameToX(loopEnd), origin.y + area.y },
                IM_COL32(38, 92, 50, 96));
        }
        if (introStart > 0)
        {
            // 序盤より前はレースでは鳴りません。暗く落として
            // 「ここは飛ばす」と一目で分かるようにします。
            draw->AddRectFilled(
                origin,
                ImVec2{ frameToX(introStart), origin.y + area.y },
                IM_COL32(0, 0, 0, 150));
        }
        if (previewEnd > previewStart)
        {
            // 試聴範囲は上端の帯で示します。ループ範囲と重なっても
            // どちらがどちらか分かるようにするためです。
            draw->AddRectFilled(
                ImVec2{ frameToX(previewStart), origin.y },
                ImVec2{ frameToX(previewEnd), origin.y + 16.0f },
                IM_COL32(150, 120, 30, 130));
        }
        const float center = origin.y + area.y * 0.5f;
        if (!state.waveformPeaks.empty())
        {
            const auto buckets = state.waveformPeaks.size();
            for (int x = 0; x < static_cast<int>(area.x); ++x)
            {
                const auto bucket = std::min(
                    buckets - 1,
                    static_cast<std::size_t>(
                        static_cast<double>(x) / area.x
                        * static_cast<double>(buckets)));
                const float peak =
                    state.waveformPeaks[bucket] * area.y * 0.46f;
                draw->AddLine(
                    ImVec2{ origin.x + static_cast<float>(x), center - peak },
                    ImVec2{ origin.x + static_cast<float>(x), center + peak },
                    IM_COL32(92, 200, 120, 190));
            }
        }
        else
        {
            draw->AddText(
                ImVec2{ origin.x + 10.0f, center - 8.0f },
                IM_COL32(160, 170, 165, 255),
                "波形を読み込んでいます…");
        }
        auto drawMarker = [&](const std::uint64_t frame,
                              const ImU32 color,
                              const char* label)
        {
            const float x = frameToX(frame);
            draw->AddLine(
                ImVec2{ x, origin.y },
                ImVec2{ x, origin.y + area.y },
                color, 2.0f);
            draw->AddText(ImVec2{ x + 4.0f, origin.y + 3.0f }, color, label);
        };
        drawMarker(introStart, IM_COL32(120, 200, 255, 255), "序盤");
        drawMarker(previewStart, IM_COL32(255, 214, 80, 255), "試聴始");
        drawMarker(previewEnd, IM_COL32(255, 180, 60, 255), "試聴終");
        drawMarker(loopStart, IM_COL32(120, 255, 140, 255), "LOOP IN");
        drawMarker(loopEnd, IM_COL32(255, 120, 120, 255), "LOOP OUT");
        const bool playing = state.preview
            && state.preview->State() == DirectX::PLAYING;
        if (playing)
        {
            const float x = frameToX(state.preview->PlaybackFrame());
            draw->AddLine(
                ImVec2{ x, origin.y },
                ImVec2{ x, origin.y + area.y },
                IM_COL32(255, 255, 255, 220), 1.5f);
        }
        if (waveformHovered && totalFrames > 0)
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                StartPreview(xToFrame(ImGui::GetIO().MousePos.x));
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                state.contextFrame = xToFrame(ImGui::GetIO().MousePos.x);
                ImGui::OpenPopup("##BgmWaveformMenu");
            }
        }
        if (ImGui::BeginPopup("##BgmWaveformMenu"))
        {
            ImGui::Text(
                "%.3f 秒",
                static_cast<double>(state.contextFrame) / rate);
            ImGui::Separator();
            if (ImGui::MenuItem("ここを序盤の開始にする"))
            {
                writeFrame("intro_start_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここをLOOP INにする"))
            {
                writeFrame("loop_start_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここをLOOP OUTにする"))
            {
                writeFrame("loop_end_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここを試聴の開始にする"))
            {
                writeFrame("preview_start_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここを試聴の終わりにする"))
            {
                writeFrame("preview_end_frame", state.contextFrame);
            }
            ImGui::EndPopup();
        }
        ImGui::TextDisabled(
            "左クリック: その位置から試聴 / 右クリック: この位置を"
            "序盤・LOOP IN・LOOP OUT・試聴の開始/終わりに設定");

        // ---- 数値 ----
        auto secondsField = [&](const char* label,
                                const char* key,
                                const std::uint64_t frame)
        {
            double seconds = static_cast<double>(frame) / rate;
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputDouble(label, &seconds, 0.001, 0.5, "%.3f 秒"))
            {
                writeFrame(
                    key,
                    static_cast<std::uint64_t>(
                        std::max(seconds, 0.0) * rate + 0.5));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%llu frame",
                static_cast<unsigned long long>(frame));
        };
        secondsField(
            "序盤の開始（レースで鳴り始める位置）",
            "intro_start_frame", introStart);
        secondsField("ループ開始 (LOOP IN)", "loop_start_frame", loopStart);
        secondsField("ループ終了 (LOOP OUT)", "loop_end_frame", loopEnd);
        secondsField(
            "試聴の開始（セレクト画面）", "preview_start_frame", previewStart);
        secondsField(
            "試聴の終わり（ここで頭へ戻る）", "preview_end_frame", previewEnd);
        {
            double milliseconds =
                static_cast<double>(crossfade) / rate * 1000.0;
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::InputDouble(
                    "継ぎ目クロスフェード", &milliseconds, 1.0, 10.0,
                    "%.0f ms"))
            {
                writeFrame(
                    "loop_crossfade_frames",
                    static_cast<std::uint64_t>(
                        std::max(milliseconds, 0.0) / 1000.0 * rate + 0.5));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%llu frame",
                static_cast<unsigned long long>(crossfade));
        }
        if (previewEnd <= previewStart)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f },
                "試聴の終わりは開始より後ろにしてください。");
        }
        else
        {
            ImGui::TextDisabled(
                "試聴の長さ %.1f 秒",
                static_cast<double>(previewEnd - previewStart) / rate);
        }
        if (introStart >= loopEnd && loopEnd > 0)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f },
                "序盤の開始は LOOP OUT より前にしてください。");
        }
        if (loopEnd <= loopStart)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f },
                "LOOP OUT は LOOP IN より後ろにしてください。");
        }
        else if (crossfade * 2 >= loopEnd - loopStart)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f },
                "クロスフェードがループ区間の半分を超えています。");
        }

        // ---- 音量補正 ----
        // 曲ごとのマスタリング差をならす値。音源は作り直さず、
        // 再生音量へ掛ける方式なので、ここは何度でも変えられる。
        if (track.contains("gain_db"))
        {
            auto gain = static_cast<float>(
                track.value("gain_db", 0.0));
            ImGui::SetNextItemWidth(240.0f);
            if (ImGui::SliderFloat(
                    "音量補正", &gain, -12.0f, 9.0f, "%+.2f dB"))
            {
                track["gain_db"] =
                    std::round(static_cast<double>(gain) * 100.0) / 100.0;
                state.dirty = true;
                if (state.preview)
                {
                    // 鳴らしながら動かせるように、その場で反映する。
                    state.preview->SetVolume(std::clamp(
                        state.previewVolume * std::pow(10.0f, gain / 20.0f),
                        0.0f, 1.0f));
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled(
                "曲ごとの音量差をならす（音源は書き換えません）");
        }

        // ---- 「おまかせ」で流すコース ----
        // カタログJSONに courses（id と表示名の一覧）があるときだけ
        // 出します。曲ごとに「このコースで流していい」印を付ける欄です。
        if (state.document->contains("courses")
            && state.document->at("courses").is_array()
            && ImGui::CollapsingHeader("おまかせで流すコース"))
        {
            ImGui::TextDisabled(
                "選曲の「おまかせ」でこの曲が候補になるコースです。"
                "1曲も指定が無いコースは全曲から選ばれます。");
            if (!track.contains("courses")
                || !track.at("courses").is_array())
            {
                track["courses"] = nlohmann::json::array();
            }
            auto& assigned = track.at("courses");
            const auto& courses = state.document->at("courses");
            int column = 0;
            for (const auto& course : courses)
            {
                const auto id = course.value("id", std::string{});
                if (id.empty())
                {
                    continue;
                }
                const auto label = course.value("name", id);
                bool enabled = false;
                for (const auto& value : assigned)
                {
                    if (value.is_string()
                        && value.get_ref<const std::string&>() == id)
                    {
                        enabled = true;
                        break;
                    }
                }
                if (column != 0)
                {
                    ImGui::SameLine(static_cast<float>(column) * 220.0f);
                }
                if (ImGui::Checkbox(
                        (label + "##bgmcourse-" + id).c_str(), &enabled))
                {
                    if (enabled)
                    {
                        assigned.push_back(id);
                    }
                    else
                    {
                        for (auto entry = assigned.begin();
                             entry != assigned.end();
                             ++entry)
                        {
                            if (entry->is_string()
                                && entry->get_ref<const std::string&>()
                                    == id)
                            {
                                assigned.erase(entry);
                                break;
                            }
                        }
                    }
                    state.dirty = true;
                }
                column = (column + 1) % 3;
            }
            if (assigned.empty())
            {
                ImGui::TextDisabled(
                    "（この曲はどのコースの「おまかせ」にも出てきません）");
            }
        }

        // ---- 試聴 ----
        ImGui::Separator();
        if (ImGui::Button("▶ 序盤から"))
        {
            // レースとまったく同じ鳴り方（序盤の開始→LOOP OUT→ループ）。
            StartPreview(introStart);
        }
        ImGui::SameLine();
        if (ImGui::Button("▶ 試聴範囲"))
        {
            // ゲームの選曲画面とまったく同じ鳴り方で確かめます。
            StartPreview(previewStart, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("▶ 継ぎ目を聞く"))
        {
            // LOOP OUTの3秒手前から鳴らすと、折り返しがそのまま来ます。
            const auto lead = static_cast<std::uint64_t>(rate * 3.0);
            StartPreview(loopEnd > lead ? loopEnd - lead : 0);
        }
        ImGui::SameLine();
        // 鳴っているかはこの行で取り直す。上のボタンで状態が変わって
        // いることがあるので、フレーム頭の値を使うと表示がずれる。
        const bool transportPlaying = state.preview
            && state.preview->State() == DirectX::PLAYING;
        if (transportPlaying)
        {
            if (ImGui::Button("■ 停止"))
            {
                // streamは残したまま止める。次の「▶ 再生」で読み直さず
                // すぐ鳴らせる。パネルを閉じるときだけ完全に捨てる。
                state.preview->Stop();
            }
        }
        else if (ImGui::Button("▶ 再生"))
        {
            // 直前に鳴らした位置から。まだ一度も鳴らしていなければ
            // 試聴範囲を鳴らす（このパネルで一番よく使う聞き方）。
            StartPreview(
                state.hasLastStart ? state.lastStartFrame : previewStart,
                state.hasLastStart ? state.lastUsedPreviewRange : true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderFloat(
                "音量", &state.previewVolume, 0.0f, 1.0f, "%.2f")
            && state.preview)
        {
            state.preview->SetVolume(state.previewVolume);
        }

        // 上で求めたplayingは、この間にあるボタン（「■ 停止」や曲の
        // 切り替え）でstate.previewが捨てられていると嘘になる。
        // 破棄済みのstreamへPlaybackFrameを呼ぶとその場で落ちるので、
        // ここで必ず取り直す。
        const bool stillPlaying = state.preview
            && state.preview->State() == DirectX::PLAYING;
        if (stillPlaying)
        {
            const auto position = state.preview->PlaybackFrame();
            const double seconds = static_cast<double>(position) / rate;
            const double length =
                static_cast<double>(totalFrames) / rate;
            ImGui::Text(
                "再生位置 %d:%06.3f / %d:%06.3f    折り返し %llu 回",
                static_cast<int>(seconds) / 60,
                seconds - (static_cast<int>(seconds) / 60) * 60,
                static_cast<int>(length) / 60,
                length - (static_cast<int>(length) / 60) * 60,
                static_cast<unsigned long long>(
                    state.preview->CompletedLoopCount()));

            // エンジンの帯域レベルメーター。ゲーム内のBGMプレートと
            // 同じ値なので、ここで動きを確かめられます。
            std::array<float, AudioStreamVoice::LevelBandCount> bands{};
            state.preview->ReadLevelBands(bands.data(), bands.size());
            const ImVec2 meterOrigin = ImGui::GetCursorScreenPos();
            const float meterHeight = 34.0f;
            const float barWidth = 10.0f;
            ImGui::InvisibleButton(
                "##BgmMeter",
                ImVec2{ barWidth * bands.size() * 1.4f, meterHeight });
            for (std::size_t band = 0; band < bands.size(); ++band)
            {
                const float level = std::clamp(bands[band], 0.0f, 1.0f);
                const float x =
                    meterOrigin.x + static_cast<float>(band) * barWidth * 1.4f;
                const float top =
                    meterOrigin.y + meterHeight * (1.0f - level);
                draw->AddRectFilled(
                    ImVec2{ x, top },
                    ImVec2{ x + barWidth, meterOrigin.y + meterHeight },
                    IM_COL32(
                        static_cast<int>(60 + 160 * level),
                        static_cast<int>(140 + 100 * level),
                        90, 220));
            }
        }
        else
        {
            ImGui::TextDisabled(
                "停止中。波形をクリックするか、上のボタンで鳴らします。");
        }

        ImGui::End();
        if (!open)
        {
            StopPreview();
        }
    }

}
