#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class DataAsset;
    // DataAsset.cpp内の構築ヘルパーです（JSONの読み書きを
    // ヘッダへ持ち込まないための前方宣言）。
    struct DataAssetBuilder;

    // データアセット（`*.asset.json`）が持てる値の種類です。
    // JSONを読んだ時点で確定し、以降は文字列比較なしで引けます。
    enum class DataValueKind
    {
        None,
        Boolean,
        Number,
        Text,
        // JSONの配列。vec3や色も「数値が3つ並んだList」として
        // 扱うため、種類はこれ1つで足ります。
        List
    };

    // キー1つぶんの値です。取り出しはDataAssetのGet～経由で行うため、
    // 中身を直接触る必要はありません。
    struct DataValue final
    {
        std::string key;
        DataValueKind kind{ DataValueKind::None };
        bool boolean{};
        // 整数も実数もここへ入れます（JSONの数値は元々double）。
        double number{};
        std::string text;
        // Listのとき、要素を1つずつDataAssetとして保持します。
        // 数値や文字列の並びは、要素の中の"value"というキーへ
        // 入ります（GetFloatAtなどが代わりに引きます）。
        std::vector<DataAsset> items;
    };

    // GameObjectへぶら下がらない、単体で存在するデータの入れ物です。
    // UnityのScriptableObjectに相当します。型（フィールドの並び）は
    // Game Moduleが`LAMAPON_DATA_ASSET`で宣言し、値はエディターの
    // インスペクターで編集して`*.asset.json`へ保存します。
    //
    // 読み込みは`AssetManager::LoadDataAsset`（Scriptからは
    // `LoadDataAsset`）を使ってください。書き出したゲームでは
    // アセットが暗号化アーカイブへ入るため、`std::ifstream`で
    // 直接開くとエディターでだけ動く実装になります。
    class DataAsset final
    {
    public:
        // 壊れたJSONでも例外は投げず、空のDataAssetを返します。
        // 値が引けないだけで済ませ、ゲームは動き続けます。
        [[nodiscard]] static DataAsset FromJson(
            std::string_view json,
            std::string name = {});
        [[nodiscard]] std::string SerializeToJson() const;

        // 型名（例: "Game.CardData"）。宣言と結び付ける識別子です。
        [[nodiscard]] const std::string& TypeName() const noexcept
        {
            return m_typeName;
        }
        // 拡張子を除いたファイル名。表示やデバッグ用です。
        [[nodiscard]] const std::string& Name() const noexcept
        {
            return m_name;
        }
        void SetName(std::string name)
        {
            m_name = std::move(name);
        }
        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return m_values.empty();
        }
        [[nodiscard]] const std::vector<DataValue>&
            Values() const noexcept
        {
            return m_values;
        }

        [[nodiscard]] bool Has(
            std::string_view key) const noexcept;

        [[nodiscard]] bool GetBool(
            std::string_view key,
            bool defaultValue = false) const noexcept;
        [[nodiscard]] int GetInt(
            std::string_view key,
            int defaultValue = 0) const noexcept;
        [[nodiscard]] float GetFloat(
            std::string_view key,
            float defaultValue = 0.0f) const noexcept;
        [[nodiscard]] std::string GetText(
            std::string_view key,
            std::string defaultValue = {}) const;

        [[nodiscard]] DirectX::XMFLOAT2 GetVector2(
            std::string_view key,
            DirectX::XMFLOAT2 defaultValue = {}) const noexcept;
        [[nodiscard]] DirectX::XMFLOAT3 GetVector3(
            std::string_view key,
            DirectX::XMFLOAT3 defaultValue = {}) const noexcept;
        [[nodiscard]] DirectX::XMFLOAT4 GetVector4(
            std::string_view key,
            DirectX::XMFLOAT4 defaultValue = {}) const noexcept;
        // 色はアルファ既定1で引きます（color3は1が入ります）。
        [[nodiscard]] DirectX::XMFLOAT4 GetColor(
            std::string_view key,
            DirectX::XMFLOAT4 defaultValue = {
                1.0f,
                1.0f,
                1.0f,
                1.0f
            }) const noexcept;

        // 他のアセットへの参照（assetsフォルダーからの相対パス）。
        // そのまま`Instantiate`や`LoadDataAsset`へ渡せます。
        [[nodiscard]] std::filesystem::path GetAssetPath(
            std::string_view key) const;

        // listの要素数。listでなければ0です。
        [[nodiscard]] std::size_t Count(
            std::string_view key) const noexcept;
        // listの要素。範囲外や型違いでは空のDataAssetを返すので、
        // 戻り値をそのまま使っても落ちません。
        [[nodiscard]] const DataAsset& Item(
            std::string_view key,
            std::size_t index) const noexcept;

        // 数値や文字列だけを並べたlist用の近道です。
        [[nodiscard]] bool GetBoolAt(
            std::string_view key,
            std::size_t index,
            bool defaultValue = false) const noexcept;
        [[nodiscard]] int GetIntAt(
            std::string_view key,
            std::size_t index,
            int defaultValue = 0) const noexcept;
        [[nodiscard]] float GetFloatAt(
            std::string_view key,
            std::size_t index,
            float defaultValue = 0.0f) const noexcept;
        [[nodiscard]] std::string GetTextAt(
            std::string_view key,
            std::size_t index,
            std::string defaultValue = {}) const;

        // 参照が空でも必ず有効なDataAssetを返すための共有の空実体。
        [[nodiscard]] static const DataAsset& Empty() noexcept;

    private:
        friend struct DataAssetBuilder;

        [[nodiscard]] const DataValue* Find(
            std::string_view key) const noexcept;

        std::string m_typeName;
        std::string m_name;
        std::vector<DataValue> m_values;
    };

    // `*.asset.json`かどうか。エディターとランタイムの両方から
    // 同じ判定を使うため、ここに1つだけ置きます。
    [[nodiscard]] bool IsDataAssetPath(
        const std::filesystem::path& path);
}
