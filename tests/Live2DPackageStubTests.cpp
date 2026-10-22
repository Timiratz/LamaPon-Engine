#include <LamaPonLive2D/Live2DModel.h>

#include <algorithm>
#include <cstdlib>
#include <ranges>
#include <stdexcept>
#include <string_view>

// SDK未導入時の登録とAPIの無効動作を検証します。
int main()
{
    // 静的初期化で登録されたGame Moduleスクリプト
    const auto& scripts = LamaPon::GameModuleScripts::RegisteredScripts();
    // Live2DModelの登録記述
    const auto descriptor = std::ranges::find_if(
        scripts,
        // Game Moduleへ登録された型名を比較します(script: 登録候補)。
        [](const LamaPon::NativeScriptTypeDescriptor& script)
        {
            return script.typeName != nullptr
                && std::string_view{ script.typeName }
                    == "Game.Live2DModel";
        });
    if (descriptor == scripts.end()
        || descriptor->render2D == nullptr
        || descriptor->propertiesSchemaJson == nullptr)
    {
        return EXIT_FAILURE;
    }

    // SDK未導入でも構築できるLive2Dスクリプト
    LamaPonLive2D::Live2DModel model;
    model.SetPosition(DirectX::XMFLOAT2{ 12.0f, 34.0f });
    model.SetPixelsPerUnit(0.0f);
    if (model.Position().x != 12.0f
        || model.Position().y != 34.0f
        || model.PixelsPerUnit() != 0.0f
        || model.SetParameter("ParamAngleX", 1.0f)
        || model.GetParameter("ParamAngleX").has_value())
    {
        return EXIT_FAILURE;
    }

    // SDK未導入時にモデル読み込みを拒否したか
    bool loadRejected = false;
    try
    {
        model.LoadModel("Models/Example.model3.json");
    }
    catch (const std::runtime_error&)
    {
        loadRejected = true;
    }
    return loadRejected ? EXIT_SUCCESS : EXIT_FAILURE;
}
