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
    // カタログを読み込んで編集状態を初期化します。
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

    // 試聴音声を停止して解放します。
    BgmLoopPanel::~BgmLoopPanel()
    {
        StopPreview();
    }

    // 登録された状態通知へ内容を渡します。
    void BgmLoopPanel::SetStatus(std::string message, const bool error) const
    {
        if (m_status)
        {
            m_status(std::move(message), error);
        }
    }

    // 曲名と資産パスを検証してカタログを読み込みます。
    bool BgmLoopPanel::Load()
    {
        StopPreview();
        try
        {
            // 編集対象カタログのパス
            const auto path = m_catalogPath;
            // カタログの入力ストリーム
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw std::runtime_error("BGMカタログJSONを開けません");
            }
            // 検証前の読込文書
            auto document = std::make_unique<nlohmann::json>();
            // 検証前の読込文書
            input >> *document;
            if (!document->contains("tracks")
                || !document->at("tracks").is_array()
                || document->at("tracks").empty())
            {
                throw std::runtime_error("tracks配列がありません");
            }
            // 各曲のnameとassetを検証してから編集中の文書を置き換えます。
            // 選択曲の設定
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
        // 失敗理由を状態表示へ渡します(exception: 失敗理由)。
        catch (const std::exception& exception)
        {
            m_state.loaded = false;
            SetStatus(
                std::string{ "BGMループエディターを読み込めません: " }
                + exception.what(),
                true);
            return false;
        }
    }

    // 書込を確認してカタログを置換し、失敗時は未保存状態を保持します。
    bool BgmLoopPanel::Save()
    {
        try
        {
            // 編集対象カタログのパス
            const auto path = m_catalogPath;
            // 置換前の書込ファイルパス
            const auto temporary = path.wstring() + L".tmp";
            {
                // 置換用ファイルの出力ストリーム
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
        // 失敗理由を状態表示へ渡します(exception: 失敗理由)。
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "BGMカタログを保存できません: " }
                + exception.what(),
                true);
            return false;
        }
    }

    // 試聴を中断せず、選択曲の全体波形を作成します。
    void BgmLoopPanel::BuildWaveform()
    {
        // 編集と試聴の共有状態
        auto& state = m_state;
        state.waveformTrack = state.selectedTrack;
        state.waveformPeaks.clear();
        state.totalFrames = 0;
        if (!state.loaded || !state.document)
        {
            return;
        }
        // カタログの曲配列
        const auto& tracks = state.document->at("tracks");
        if (state.selectedTrack < 0
            || state.selectedTrack >= static_cast<int>(tracks.size()))
        {
            return;
        }
        try
        {
            // 資産パスの基準ディレクトリ
            const auto root =
                m_projectRoot;
            // 選択曲の音源パス
            const auto asset = root / PathFromUtf8(
                tracks.at(static_cast<std::size_t>(state.selectedTrack))
                    .value("asset", std::string{}));

            // 波形抽出専用ストリーム
            auto probe = m_audio.CreateStream(
                m_assets, asset);
            // 波形の区間数
            constexpr std::size_t buckets = 1200;
            state.waveformPeaks.assign(buckets, 0.0f);
            probe->ReadPeakEnvelope(state.waveformPeaks.data(), buckets);
            state.totalFrames = probe->TotalFrames();
            state.sampleRate = probe->SampleRate() > 0
                ? probe->SampleRate()
                : 44100;
        }
        // 失敗理由を状態表示へ渡します(exception: 失敗理由)。
        catch (const std::exception& exception)
        {
            state.waveformPeaks.clear();
            SetStatus(
                std::string{ "波形を作れません: " } + exception.what(), true);
        }
    }

    // 保存前の設定を適用して試聴を開始します。
    void BgmLoopPanel::StartPreview(
        const std::uint64_t fromFrame, const bool usePreviewRange)
    {
        // 編集と試聴の共有状態
        auto& state = m_state;
        if (!state.loaded || !state.document)
        {
            return;
        }
        // カタログの曲配列
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
                // 資産パスの基準ディレクトリ
                const auto root =
                    m_projectRoot;
                // 選択曲の音源パス
                const auto asset = root / PathFromUtf8(
                    tracks.at(static_cast<std::size_t>(state.selectedTrack))
                        .value("asset", std::string{}));
                state.preview = m_audio.CreateStream(
                    m_assets, asset);
                state.preview->SetLevelMeterEnabled(true);
                state.previewTrack = state.selectedTrack;
            }
            // 選択曲の設定
            auto& track =
                tracks.at(static_cast<std::size_t>(state.selectedTrack));
            // 編集中の値を適用し、保存前でも継ぎ目を確認できます。
            state.preview->SetLoop(true);
            if (usePreviewRange)
            {
                // 指定した試聴範囲を繰り返します。
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
        // 失敗理由を状態表示へ渡します(exception: 失敗理由)。
        catch (const std::exception& exception)
        {
            SetStatus(
                std::string{ "BGMを再生できません: " } + exception.what(),
                true);
        }
    }

    // 試聴音声を停止して解放します。
    void BgmLoopPanel::StopPreview() noexcept
    {
        // 編集と試聴の共有状態
        auto& state = m_state;
        if (state.preview)
        {
            state.preview->Stop();
        }
        state.preview.reset();
        state.previewTrack = -1;
    }

    // 波形とループ範囲の編集、試聴操作を表示します。
    void BgmLoopPanel::Draw(
        const std::string& title, bool& open,
        const std::function<void()>& onSaved)
    {
        // 編集と試聴の共有状態
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

        // カタログの曲配列
        auto& tracks = state.document->at("tracks");
        // 曲選択用の借用文字列
        std::vector<const char*> labels;
        labels.reserve(tracks.size());
        // 曲または割当グループの要素
        for (auto& entry : tracks)
        {
            labels.push_back(
                entry.at("name").get_ref<std::string&>().c_str());
        }
        // 変更前の曲添字
        const int previousTrack = state.selectedTrack;
        ImGui::SetNextItemWidth(320.0f);
        ImGui::Combo(
            "曲", &state.selectedTrack, labels.data(),
            static_cast<int>(labels.size()));
        if (state.selectedTrack != previousTrack)
        {
            StopPreview();
            // 曲を変更したときは再生位置を初期化します。
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
            // documentの差し替え後はtracksの参照が無効になるため、このフレームの描画を終了します。
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

        // 選択曲の設定
        auto& track =
            tracks.at(static_cast<std::size_t>(state.selectedTrack));
        // 秒換算用サンプル周波数・Hz
        const auto rate = static_cast<double>(
            state.sampleRate > 0 ? state.sampleRate : 44100);
        // 選択曲の総フレーム数
        const std::uint64_t totalFrames = state.totalFrames;
        // ループ開始フレーム
        auto loopStart = track.value("loop_start_frame", std::uint64_t{ 0 });
        // ループ終了フレーム
        auto loopEnd = track.value("loop_end_frame", std::uint64_t{ 0 });
        // 継ぎ目の重複フレーム数
        auto crossfade =
            track.value("loop_crossfade_frames", std::uint64_t{ 0 });
        // 通常再生の開始フレーム
        auto introStart =
            track.value("intro_start_frame", std::uint64_t{ 0 });
        // 試聴範囲の開始フレーム
        auto previewStart =
            track.value("preview_start_frame", std::uint64_t{ 0 });
        // 試聴範囲の終了フレーム
        auto previewEnd =
            track.value("preview_end_frame", std::uint64_t{ 0 });

        // フレーム位置を曲の末尾までに制限します(frame: 制限前の位置)。
        auto clampFrame = [&](const std::uint64_t frame)
        {
            return totalFrames > 0 ? std::min(frame, totalFrames) : frame;
        };
        // 位置を更新し、対象項目の秒数も同期します(key: 更新するJSONキー, frame: 設定する位置)。
        auto writeFrame = [&](const char* key, const std::uint64_t frame)
        {
            track[key] = clampFrame(frame);

            // 更新対象のJSONキー
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


        // 波形領域の左上・画面座標
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        // 波形領域の幅と高さ・px
        const ImVec2 area{
            std::max(ImGui::GetContentRegionAvail().x, 240.0f), 170.0f };
        ImGui::InvisibleButton("##BgmWaveform", area);
        // マウスが波形上にあるか
        const bool waveformHovered = ImGui::IsItemHovered();
        // 波形とメーターの描画先
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(
            origin,
            ImVec2{ origin.x + area.x, origin.y + area.y },
            IM_COL32(13, 18, 15, 255));
        // 曲中の位置を波形のX座標へ換算します(frame: 曲中のフレーム)。
        auto frameToX = [&](const std::uint64_t frame)
        {
            // 曲全体に対する位置比率
            const double ratio = totalFrames > 0
                ? static_cast<double>(frame) / static_cast<double>(totalFrames)
                : 0.0;
            return origin.x
                + static_cast<float>(std::clamp(ratio, 0.0, 1.0)) * area.x;
        };
        // 波形のX座標を曲中の位置へ換算します(x: 画面上のX座標)。
        auto xToFrame = [&](const float x)
        {
            // 曲全体に対する位置比率
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
            // イントロ開始より前は通常再生で使わないため、暗く表示して再生対象外であることを示します。
            draw->AddRectFilled(
                origin,
                ImVec2{ frameToX(introStart), origin.y + area.y },
                IM_COL32(0, 0, 0, 150));
        }
        if (previewEnd > previewStart)
        {
            // ループ範囲と重なる試聴範囲を上端の帯で区別します。
            draw->AddRectFilled(
                ImVec2{ frameToX(previewStart), origin.y },
                ImVec2{ frameToX(previewEnd), origin.y + 16.0f },
                IM_COL32(150, 120, 30, 130));
        }
        // 波形の縦中心・画面座標
        const float center = origin.y + area.y * 0.5f;
        if (!state.waveformPeaks.empty())
        {
            // 波形の区間数
            const auto buckets = state.waveformPeaks.size();
            // 波形列またはマーカーのX座標
            for (int x = 0; x < static_cast<int>(area.x); ++x)
            {
                // 描画列に対応する波形区間
                const auto bucket = std::min(
                    buckets - 1,
                    static_cast<std::size_t>(
                        static_cast<double>(x) / area.x
                        * static_cast<double>(buckets)));
                // 波形の縦振幅・px
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
        // 範囲端の縦線とラベルを描きます(frame: 曲中の位置, color: 表示色, label: 表示名)。
        auto drawMarker = [&](const std::uint64_t frame,
                              const ImU32 color,
                              const char* label)
        {
            // 波形列またはマーカーのX座標
            const float x = frameToX(frame);
            draw->AddLine(
                ImVec2{ x, origin.y },
                ImVec2{ x, origin.y + area.y },
                color, 2.0f);
            draw->AddText(ImVec2{ x + 4.0f, origin.y + 3.0f }, color, label);
        };
        drawMarker(introStart, IM_COL32(120, 200, 255, 255), "イントロ");
        drawMarker(previewStart, IM_COL32(255, 214, 80, 255), "試聴開始");
        drawMarker(previewEnd, IM_COL32(255, 180, 60, 255), "試聴終了");
        drawMarker(loopStart, IM_COL32(120, 255, 140, 255), "LOOP IN");
        drawMarker(loopEnd, IM_COL32(255, 120, 120, 255), "LOOP OUT");
        // 波形描画時点の再生状態
        const bool playing = state.preview
            && state.preview->State() == DirectX::PLAYING;
        if (playing)
        {
            // 波形列またはマーカーのX座標
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
            if (ImGui::MenuItem("ここをイントロ開始位置に設定"))
            {
                writeFrame("intro_start_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここをLOOP INに設定"))
            {
                writeFrame("loop_start_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここをLOOP OUTに設定"))
            {
                writeFrame("loop_end_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここを試聴開始位置に設定"))
            {
                writeFrame("preview_start_frame", state.contextFrame);
            }
            if (ImGui::MenuItem("ここを試聴終了位置に設定"))
            {
                writeFrame("preview_end_frame", state.contextFrame);
            }
            ImGui::EndPopup();
        }
        ImGui::TextDisabled(
            "左クリック: その位置から試聴／右クリック: この位置を"
            "イントロ・LOOP IN・LOOP OUT・試聴開始・試聴終了に設定");


        // 秒数入力をフレームへ換算して設定します(label: 入力欄の表示名, key: 設定するJSONキー, frame: 現在の位置)。
        auto secondsField = [&](const char* label,
                                const char* key,
                                const std::uint64_t frame)
        {
            // 入力または再生位置の秒数
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
            "イントロ開始（通常再生の開始位置）",
            "intro_start_frame", introStart);
        secondsField("ループ開始 (LOOP IN)", "loop_start_frame", loopStart);
        secondsField("ループ終了 (LOOP OUT)", "loop_end_frame", loopEnd);
        secondsField(
            "試聴開始", "preview_start_frame", previewStart);
        secondsField(
            "試聴終了（開始位置へ戻る）", "preview_end_frame", previewEnd);
        {
            // クロスフェードの時間・ms
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
                "試聴終了位置は開始位置より後にしてください。");
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
                "イントロ開始位置はLOOP OUTより前にしてください。");
        }
        if (loopEnd <= loopStart)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f },
                "LOOP OUTはLOOP INより後にしてください。");
        }
        else if (crossfade * 2 >= loopEnd - loopStart)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.5f, 0.4f, 1.0f },
                "クロスフェードがループ区間の半分を超えています。");
        }

        // 音源を変更せず、試聴中の音量補正を音声へ反映します。
        if (track.contains("gain_db"))
        {
            // 曲の音量補正・dB
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

                    state.preview->SetVolume(std::clamp(
                        state.previewVolume * std::pow(10.0f, gain / 20.0f),
                        0.0f, 1.0f));
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled(
                "曲ごとの音量差をならす（音源は書き換えません）");
        }


        // 新旧形式のグループ一覧
        const nlohmann::json* groupList = nullptr;
        // 曲のグループ割当キー
        const char* assignmentKey = "groups";
        if (state.document->contains("groups")
            && state.document->at("groups").is_array())
        {
            groupList = &state.document->at("groups");
            if (!track.contains("groups")
                && track.contains("courses")
                && track.at("courses").is_array())
            {
                assignmentKey = "courses";
            }
        }
        else if (state.document->contains("courses")
            && state.document->at("courses").is_array())
        {
            groupList = &state.document->at("courses");
            assignmentKey = "courses";
        }
        if (groupList != nullptr
            && ImGui::CollapsingHeader("自動選曲グループ"))
        {
            ImGui::TextDisabled(
                "この曲を自動選曲の候補に含めるグループです。"
                "曲が未指定のグループでは、全曲が候補になります。");
            if (!track.contains(assignmentKey)
                || !track.at(assignmentKey).is_array())
            {
                track[assignmentKey] = nlohmann::json::array();
            }
            // 選択曲の割当グループ配列
            auto& assigned = track.at(assignmentKey);
            // グループ表示列・0から2
            int column = 0;
            // 自動選曲グループの定義
            for (const auto& group : *groupList)
            {
                // グループの識別子
                const auto id = group.value("id", std::string{});
                if (id.empty())
                {
                    continue;
                }
                // グループの表示名
                const auto label = group.value("name", id);
                // 曲がグループに割当済みか
                bool enabled = false;
                // 割当済みグループの識別値
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
                        (label + "##bgmgroup-" + id).c_str(), &enabled))
                {
                    if (enabled)
                    {
                        assigned.push_back(id);
                    }
                    else
                    {
                        // 曲または割当グループの要素
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
                    "（この曲は自動選曲グループに割り当てられていません）");
            }
        }


        ImGui::Separator();
        if (ImGui::Button("▶ イントロから"))
        {
            // イントロ開始からLOOP OUTまで再生し、その後ループへ入ります。
            StartPreview(introStart);
        }
        ImGui::SameLine();
        if (ImGui::Button("▶ 試聴範囲"))
        {
            // 指定した試聴範囲を繰り返します。
            StartPreview(previewStart, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("▶ 継ぎ目を聞く"))
        {
            // LOOP OUTの3秒手前から鳴らすと、折り返しがそのまま来ます。
            // 継ぎ目前の試聴時間・フレーム
            const auto lead = static_cast<std::uint64_t>(rate * 3.0);
            StartPreview(loopEnd > lead ? loopEnd - lead : 0);
        }
        ImGui::SameLine();

        // 再生操作後の再生状態
        const bool transportPlaying = state.preview
            && state.preview->State() == DirectX::PLAYING;
        if (transportPlaying)
        {
            if (ImGui::Button("■ 停止"))
            {
                // 再開用にストリームを保持して停止します。
                state.preview->Stop();
            }
        }
        else if (ImGui::Button("▶ 再生"))
        {
            // 直前の試聴開始位置と範囲を再使用し、未再生なら試聴範囲を使います。
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


        // 位置取得直前の再生状態
        const bool stillPlaying = state.preview
            && state.preview->State() == DirectX::PLAYING;
        if (stillPlaying)
        {
            // 現在の再生フレーム
            const auto position = state.preview->PlaybackFrame();
            // 入力または再生位置の秒数
            const double seconds = static_cast<double>(position) / rate;
            // 曲全体の長さ・秒
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

            // AudioSourceComponentと同じ帯域レベルを表示します。
            // 帯域別の音声レベル
            std::array<float, AudioStreamVoice::LevelBandCount> bands{};
            state.preview->ReadLevelBands(bands.data(), bands.size());
            // メーターの左上・画面座標
            const ImVec2 meterOrigin = ImGui::GetCursorScreenPos();
            // メーターの高さ・px
            const float meterHeight = 34.0f;
            // 帯域バーの幅・px
            const float barWidth = 10.0f;
            ImGui::InvisibleButton(
                "##BgmMeter",
                ImVec2{ barWidth * bands.size() * 1.4f, meterHeight });
            // 表示する帯域の添字
            for (std::size_t band = 0; band < bands.size(); ++band)
            {
                // 帯域レベル・0から1
                const float level = std::clamp(bands[band], 0.0f, 1.0f);
                // 波形列またはマーカーのX座標
                const float x =
                    meterOrigin.x + static_cast<float>(band) * barWidth * 1.4f;
                // 帯域バーの上端・画面座標
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
