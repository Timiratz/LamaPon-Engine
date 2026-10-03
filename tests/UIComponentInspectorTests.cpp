#include "LamaPon/Editor/UIComponentInspectors.h"
#include "LamaPon/Editor/EditorLayerShared.h"
#include "LamaPon/Editor/ShaderProperties.h"
#include "LamaPon/Editor/SimpleMaterialShaderGenerator.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Components/UIButtonComponent.h"
#include "LamaPon/Components/UIImageComponent.h"
#include "LamaPon/Components/UIToggleComponent.h"
#include "LamaPon/Components/UISliderComponent.h"
#include "LamaPon/Components/UIInputFieldComponent.h"
#include "LamaPon/Components/UILayoutGroupComponent.h"
#include "LamaPon/Components/UIScrollViewComponent.h"
#include "LamaPon/Components/RotatorComponent.h"

#include <imgui.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
    // Require(condition: 検証条件, message: 失敗理由)は不成立時に例外を送出する。
    void Require(const bool condition, const char* message)
    {
        // 条件違反を検出する
        if (!condition) throw std::runtime_error(message);
    }

    // TestMalformedShaderPropertiesFallBackWithoutThrowing()は不正なProperty定義の安全な処理を検証する。
    void TestMalformedShaderPropertiesFallBackWithoutThrowing()
    {
        // 型が文字列ではない不正なProperty定義
        const auto invalidTarget = LamaPon::ParseShaderProperties(
            R"(/* LAMAPON_PROPERTIES
            [{"target":1,"type":"float","name":"Amount"}]
            */)");
        Require(
            invalidTarget.declared
                && !invalidTarget.error.empty()
                && invalidTarget.fields.empty(),
            "A non-string property target must fall back with an error");

        // 既定値が型と一致しない不正なProperty定義
        const auto invalidDefault = LamaPon::ParseShaderProperties(
            R"(/* LAMAPON_PROPERTIES
            [{"target":"0.x","type":"float","name":"Amount",
              "default":["invalid"]}]
            */)");
        Require(
            invalidDefault.declared
                && !invalidDefault.error.empty()
                && invalidDefault.fields.empty(),
            "An invalid property default must fall back with an error");
    }

    // TestManifestPropertyConversion()はManifest Propertyの自動割当と既定値を検証する。
    void TestManifestPropertyConversion()
    {
        // 自動割当の対象となるfloat Property
        LamaPon::ShaderPropertyDesc amount;
        amount.name = "Amount";
        amount.type = "float";
        amount.target = "0.x";
        amount.defaultValue = "0.75";
        amount.minimum = 0.0;
        amount.maximum = 1.0;

        // Manifestから変換したProperty一覧
        const auto converted =
            LamaPon::ConvertShaderManifestProperties({ amount });
        Require(
            converted.declared
                && converted.error.empty()
                && converted.fields.size() == 1
                && converted.fields.front().hasRange
                && converted.fields.front().defaultValue.has_value()
                && (*converted.fields.front().defaultValue)[0] == 0.75f,
            "A valid manifest property must retain its Inspector metadata");

        amount.target.clear();
        // Binding先を指定していないProperty
        const auto targetless =
            LamaPon::ConvertShaderManifestProperties({ amount });
        Require(
            targetless.declared
                && targetless.error.empty()
                && targetless.fields.size() == 1
                && targetless.fields[0].parameterIndex == 0
                && targetless.fields[0].components[0] == 0,
            "A targetless manifest property must be auto-bound");

        // 自動Binding対象の色Property
        LamaPon::ShaderPropertyDesc tint;
        tint.name = "Tint";
        tint.type = "color";
        tint.defaultValue = "[1.0,0.5,0.25]";
        // 自動Binding対象のTexture Property
        LamaPon::ShaderPropertyDesc mask;
        mask.name = "Mask";
        mask.type = "texture";
        // 明示・自動Bindingを混ぜた変換結果
        const auto automatic =
            LamaPon::ConvertShaderManifestProperties(
                { amount, tint, mask });
        Require(
            automatic.error.empty()
                && automatic.fields.size() == 3
                && automatic.fields[0].parameterIndex == 0
                && automatic.fields[0].components[0] == 0
                && automatic.fields[1].parameterIndex == 0
                && automatic.fields[1].componentCount == 3
                && automatic.fields[1].components[0] == 1
                && automatic.fields[2].kind
                    == LamaPon::ShaderPropertyKind::Texture
                && automatic.fields[2].parameterIndex == 0,
            "Automatic bindings must pack constants and start textures at t7");

        // 自動割当前から予約されたProperty
        LamaPon::ShaderPropertyDesc reserved = amount;
        reserved.name = "Reserved";
        reserved.target = "0.x";
        // 最初に自動BindingするProperty
        LamaPon::ShaderPropertyDesc allocated = amount;
        allocated.name = "Allocated first";
        // 明示・自動Binding混在時の変換結果
        const auto mixed =
            LamaPon::ConvertShaderManifestProperties(
                { allocated, reserved });
        Require(
            mixed.error.empty()
                && mixed.fields.size() == 2
                && mixed.fields[0].parameterIndex == 0
                && mixed.fields[0].components[0] == 1
                && mixed.fields[1].components[0] == 0,
            "Automatic bindings must not consume a later explicit target");
    }

    // TestShaderAssetSelectionAndOpenRoutes()はShader資産の選択ルールを検証する。
    void TestShaderAssetSelectionAndOpenRoutes()
    {
        using namespace LamaPon::EditorDetail;

        // 開く対象のShader Manifest
        const std::filesystem::path manifest{
            "Shaders/Toon.lamashader.json" };
        Require(
            IsOpenableShaderAsset(manifest),
            "A shader manifest must be openable from the Asset Browser");
        Require(
            IsOpenableShaderAsset("Shaders/Legacy.HLSL"),
            "A legacy HLSL shader must remain openable");
        Require(
            !IsShaderAsset(manifest),
            "A manifest must not enter legacy-only assignment routes");
        Require(
            !IsOpenableShaderAsset("Materials/Toon.material.json"),
            "An unrelated JSON asset must not enter the shader editor route");

        Require(
            !IsAssetSelectionChange(manifest, manifest),
            "Re-selecting the active manifest must not count as a change");
        Require(
            !IsAssetSelectionChange(
                manifest,
                "shaders/./TOON.LAMASHADER.JSON"),
            "Equivalent manifest references must not count as a change");
        Require(
            !IsAssetSelectionChange({}, {}),
            "Re-selecting the default shader must not count as a change");
        Require(
            IsAssetSelectionChange(manifest, "Shaders/Lit.hlsl"),
            "Selecting a different shader must count as a change");
        Require(
            IsAssetSelectionChange({}, manifest),
            "Selecting a manifest from the default shader must count as a change");
    }

    // TestShaderPropertyEditCommitIsIndependentFromChange()はUndo確定を値変更と分けて検証する。
    void TestShaderPropertyEditCommitIsIndependentFromChange()
    {
        // ドラッグ中の編集結果
        LamaPon::ShaderPropertyEditResult dragging;
        dragging.Observe(true, false);
        Require(
            dragging.changed && !dragging.committed,
            "Dragging must update the value without growing Undo history");

        // ドラッグ解除時の編集結果
        LamaPon::ShaderPropertyEditResult released;
        released.Observe(false, true);
        Require(
            !released.changed && released.committed,
            "The release frame must commit Undo even without a new value");

        // 即時確定するボタン操作
        LamaPon::ShaderPropertyEditResult button;
        button.Observe(true, true);
        Require(
            button.changed && button.committed,
            "An immediate property action must update and commit together");
    }

    // TestSimpleMaterialShaderGeneration()は生成HLSLと各EntryPointを検証する。
    void TestSimpleMaterialShaderGeneration()
    {
        // 機能を有効化した簡易Material Graph
        LamaPon::SimpleMaterialShaderGraph graph;
        graph.emission = true;
        graph.rimLight = true;
        graph.uvScroll = true;
        graph.maskTexture = true;
        // Graphから生成したHLSL
        const auto source = LamaPon::GenerateSimpleMaterialShader(graph);
        Require(
            source.find("LamaPonSimpleMaterialGraph.hlsli")
                    != std::string::npos
                && source.find("UV Scroll") != std::string::npos
                && source.find("Mask") != std::string::npos
                && source.find("register(") == std::string::npos
                && source.find("PSMain") != std::string::npos
                && source.find("PSSkinnedMain") != std::string::npos,
            "A simple graph must generate register-free material HLSL");

        // 生成Shaderが参照する共通Include
        std::ifstream includeFile(
            "assets/shaders/LamaPonSimpleMaterialGraph.hlsli",
            std::ios::binary);
        // 共通Includeが配布されていることを確認する
        Require(
            static_cast<bool>(includeFile),
            "The generated graph support include must be distributed");
        // Includeファイルの内容
        std::ostringstream includeContents;
        includeContents << includeFile.rdbuf();
        // コンパイル用にIncludeを展開したShader
        auto compilable = source;
        // 生成コード内のInclude行
        const std::string includeLine =
            "#include \"shaders/LamaPonSimpleMaterialGraph.hlsli\"";
        // Include行の開始位置
        const auto includePosition = compilable.find(includeLine);
        Require(
            includePosition != std::string::npos,
            "The generated source must reference its support include");
        compilable.replace(
            includePosition,
            includeLine.size(),
            includeContents.str());

        // compile(entry: EntryPoint名, target: Shader Model)は指定EntryPointをコンパイルする。
        const auto compile = [&compilable](
            const char* entry,
            const char* target)
        {
            // 生成ShaderのByteCode出力先
            Microsoft::WRL::ComPtr<ID3DBlob> byteCode;
            // コンパイラーエラー出力先
            Microsoft::WRL::ComPtr<ID3DBlob> errors;
            // コンパイル実行結果
            const auto result = D3DCompile(
                compilable.data(),
                compilable.size(),
                "GeneratedSimpleMaterial.hlsl",
                nullptr,
                nullptr,
                entry,
                target,
                D3DCOMPILE_ENABLE_STRICTNESS,
                0,
                byteCode.ReleaseAndGetAddressOf(),
                errors.ReleaseAndGetAddressOf());
            // 失敗時にコンパイラー出力を例外へ渡す
            if (FAILED(result))
            {
                // 診断があれば診断文を使う
                const auto message = errors
                    ? static_cast<const char*>(errors->GetBufferPointer())
                    : "Generated shader compilation failed";
                throw std::runtime_error(message);
            }
        };
        compile("VSMain", "vs_5_0");
        compile("PSMain", "ps_5_0");
        compile("VSSkinnedMain", "vs_5_0");
        compile("PSSkinnedMain", "ps_5_0");
    }
}

// UI Component InspectorのShader処理・描画・編集確定を検証する
int main()
{
    ImGui::CreateContext();
    // テスト失敗を返す終了コード
    int result{};
    // テスト失敗を終了コードへ変換する
    try
    {
        TestMalformedShaderPropertiesFallBackWithoutThrowing();
        TestManifestPropertyConversion();
        TestShaderAssetSelectionAndOpenRoutes();
        TestShaderPropertyEditCommitIsIndependentFromChange();
        TestSimpleMaterialShaderGeneration();

        // ImGuiのテスト用IO設定
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::GetPlatformIO().Renderer_TextureMaxWidth = 4096;
        ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;

        // 全UI Inspectorを描画して編集履歴を確認する
        // UI Inspectorを保持するGameObject
        LamaPon::GameObject object(1, "UI inspector regression");
        // RenderTexture選択を検証する画像コンポーネント
        auto& image = object.AddComponent<LamaPon::UIImageComponent>();
        // Inspectorの描画対象となるコンポーネント群
        const std::vector<LamaPon::Component*> components{
            &object.AddComponent<LamaPon::UICanvasComponent>(),
            &object.AddComponent<LamaPon::UIRectTransformComponent>(),
            &object.AddComponent<LamaPon::UIButtonComponent>(), &image,
            &object.AddComponent<LamaPon::UIToggleComponent>(),
            &object.AddComponent<LamaPon::UISliderComponent>(),
            &object.AddComponent<LamaPon::UIInputFieldComponent>(),
            &object.AddComponent<LamaPon::UILayoutGroupComponent>(),
            &object.AddComponent<LamaPon::UIScrollViewComponent>() };
        // Inspector対象外のコンポーネント
        LamaPon::RotatorComponent unsupported;
        // Asset Pickerへ渡す選択中の資産
        std::filesystem::path selectedAsset;
        // Undo履歴数・Picker呼出数・描画フレーム番号
        int historyCount{}, pickerCalls{}, frame{};
        // UI Inspectorが使う描画・Undo・Picker連携
        LamaPon::UIInspectorContext context{
            selectedAsset, 1280, 720,
            [&] { ++historyCount; },
            // log(message: 表示する検査エラー)はテスト失敗を例外で通知する
            [](const std::string& message, bool) { throw std::runtime_error(message); },
            // picker(id: Picker識別子, current: 現在値)は選択値と確定状態を返す
            [&](const char* id, const std::string& current)
            {
                Require(std::string_view(id) == "UIImageRenderTexture", "Unexpected picker request");
                ++pickerCalls;
                // 初回フレームでは画像に候補を設定する
                if (frame == 0)
                {
                    Require(current.empty(), "Initial image must have no render texture");
                    return LamaPon::RenderTexturePickerResult{ "Camera preview", false };
                }
                Require(current == "Camera preview", "Edited value must survive until commit");
                return LamaPon::RenderTexturePickerResult{ std::nullopt, frame == 1 };
            } };
        // frame: 編集中・確定・確定後の描画番号
        for (; frame < 3; ++frame)
        {
            ImGui::NewFrame();
            ImGui::Begin("UI inspector regression");
            // component: Inspectorを表示する対象
            for (auto* component : components)
            {
                ImGui::PushID(component);
                Require(LamaPon::DrawUIComponentInspector(*component, context), "UI component must have an inspector");
                ImGui::PopID();
            }
            // 非対応コンポーネントを呼び出し元へ委譲することを確認する
            Require(!LamaPon::DrawUIComponentInspector(unsupported, context),
                "Unsupported component must be delegated without side effects");
            ImGui::End();
            ImGui::Render();
            // texture: 作成・更新を要求するImGuiテクスチャ
            for (auto* texture : ImGui::GetPlatformIO().Textures)
            {
                // 作成または更新待ちのテクスチャを準備済みにする
                if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
                {
                    texture->SetTexID(1);
                    texture->SetStatus(ImTextureStatus_OK);
                }
            }
            // 編集時だけ履歴を作成し、確定時に一度だけ記録する
            Require(historyCount == (frame == 0 ? 0 : 1),
                "Edit must be recorded once at commit, not during every frame");
        }
        // Picker値とInspector描画結果を検証する
        Require(pickerCalls == 3 && image.RenderTexture() == "Camera preview",
            "Image inspector must apply the narrow picker result");
        Require(ImGui::GetDrawData()->TotalVtxCount > 0, "Independent inspectors must emit draw commands");
    }
    // 例外内容を出力して失敗状態を記録する
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
