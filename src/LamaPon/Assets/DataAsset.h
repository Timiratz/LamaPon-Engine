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

    // データアセット（*.asset.json）が持てる値の種類です。
    // JSONの読み込み時に確定し、以降は文字列比較せずに参照できます。
    enum class DataValueKind
    {
        None,
        Boolean,
        Number,
        Text,
        // JSON配列です。vec3や色も数値のListとして扱います。
        List
    };

    // 1つのキーに対応する値です。DataAssetのアクセサーを介して
    // 参照します。
    struct DataValue final
    {
        std::string key;
        DataValueKind kind{ DataValueKind::None };
        bool boolean{};
        // JSONの整数と実数を共通のdouble値として保持します。
        double number{};
        std::string text;
        // Listでは各要素をDataAssetとして保持します。数値や文字列の
        // 配列は各要素の"value"キーへ格納し、GetFloatAtなどから参照します。
        std::vector<DataAsset> items;
    };

    // GameObjectに属さず、単独で保持するデータコンテナーです。
    // UnityのScriptableObjectに相当します。型（フィールドの並び）は
    // Game ModuleがLAMAPON_DATA_ASSETで宣言し、値はエディターの
    // インスペクターで編集して*.asset.jsonへ保存します。
    //
    // 読み込みはAssetManager::LoadDataAsset（Scriptからは
    // LoadDataAsset）を使ってください。書き出したゲームでは
    // アセットが暗号化アーカイブへ入るため、std::ifstreamで
    // 直接開くとエディターでだけ動く実装になります。
    class DataAsset final
    {
    public:
        // 不正なJSONでは例外を送出せず、空のDataAssetを返します。
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
        // color3ではアルファに既定値1を設定します。
        [[nodiscard]] DirectX::XMFLOAT4 GetColor(
            std::string_view key,
            DirectX::XMFLOAT4 defaultValue = {
                1.0f,
                1.0f,
                1.0f,
                1.0f
            }) const noexcept;

        // 別のアセットを参照する、assetsフォルダーからの相対パスです。
        // InstantiateやLoadDataAssetへ直接渡せます。
        [[nodiscard]] std::filesystem::path GetAssetPath(
            std::string_view key) const;

        // Listの要素数を返します。List以外なら0です。
        [[nodiscard]] std::size_t Count(
            std::string_view key) const noexcept;
        // Listの要素を返します。範囲外または型不一致の場合は、
        // 空のDataAssetを返します。
        [[nodiscard]] const DataAsset& Item(
            std::string_view key,
            std::size_t index) const noexcept;

        // 数値や文字列のList要素へ直接アクセスするための関数です。
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

        // 範囲外または型不一致の要素に対して返す共有DataAssetです。
        [[nodiscard]] static const DataAsset& Empty() noexcept;

    private:
        friend struct DataAssetBuilder;

        [[nodiscard]] const DataValue* Find(
            std::string_view key) const noexcept;

        std::string m_typeName;
        std::string m_name;
        std::vector<DataValue> m_values;
    };

    // エディターとランタイムで*.asset.jsonの判定を共有します。
    [[nodiscard]] bool IsDataAssetPath(
        const std::filesystem::path& path);
}
