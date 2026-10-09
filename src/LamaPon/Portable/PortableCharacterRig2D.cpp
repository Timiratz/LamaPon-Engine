#include "LamaPon/Portable/PortableCharacterRig2D.h"

#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <utility>

namespace
{
    using Json = nlohmann::json;
    using LamaPon::GameObject;

    // 円周率
    constexpr float Pi = std::numbers::pi_v<float>;
    // 1回の積分で進める最大秒数
    constexpr float MaximumSubstepSeconds = 1.0f / 120.0f;
    // 揺れ物が1フレームで計算する最大秒数
    constexpr float MaximumSwayFrameSeconds = 0.1f;
    // 1フレームの最大積分回数
    constexpr int MaximumSubsteps = 16;
    // 瞬間移動とみなす移動量(先端までの長さに対する倍率)
    constexpr float TeleportLengthRatio = 10.0f;
    // 方向を求められない長さの上限
    constexpr float MinimumLength = 1.0e-4f;
    // 値の絶対値の上限
    constexpr float MaximumMagnitude = 1.0e6f;
    // 逆行列を求められないとみなす行列式の大きさ
    constexpr float MinimumDeterminant = 1.0e-12f;
    // 瞬きが1フレームで進める最大秒数
    constexpr float MaximumBlinkFrameSeconds = 1.0f;
    // 瞬きが1フレームで切り替える段階の最大数
    constexpr int MaximumBlinkSteps = 256;
    // 二度瞬きで次に瞬くまでの最短秒数
    constexpr float DoubleBlinkMinimumSeconds = 0.1f;
    // 二度瞬きで次に瞬くまでの最長秒数
    constexpr float DoubleBlinkMaximumSeconds = 0.25f;
    // 線形合同法の法
    constexpr std::uint64_t RandomModulus = 2147483647u;
    // 線形合同法の乗数
    constexpr std::uint64_t RandomMultiplier = 48271u;
    // 距離による重みで使う最小距離
    constexpr float MinimumWeightDistance = 0.5f;
    // パラメータ名の最大バイト数
    constexpr std::size_t MaximumParameterNameLength = 64;
    // 自動の揺れの時刻を巻き戻す周期秒数
    constexpr float AutoTimeWrapSeconds = 3600.0f;
    // 1チャンネルのキー数の上限
    constexpr std::size_t MaximumKeysPerChannel = 64;
    // チャンネル数の上限
    constexpr std::size_t MaximumChannels = 64;
    // 格子頂点数の上限
    constexpr std::size_t MaximumMeshVertices = 65535;

    // 有限値ならそのまま、非有限なら代替値を返します(value: 確認する値, fallback: 代替値)。
    [[nodiscard]] float FiniteOr(const float value, const float fallback) noexcept
    {
        return std::isfinite(value) ? value : fallback;
    }

    // 値が有限かつ上限内か返します(value: 確認する値)。
    [[nodiscard]] bool IsUsable(const float value) noexcept
    {
        return std::isfinite(value) && std::abs(value) <= MaximumMagnitude;
    }

    // 度をラジアンへ変換します(degrees: 角度の度)。
    [[nodiscard]] float ToRadians(const float degrees) noexcept
    {
        return degrees * (Pi / 180.0f);
    }

    // 度を-180〜180へ収めます(degrees: 角度の度)。
    [[nodiscard]] float WrapDegrees(const float degrees) noexcept
    {
        // 360度で割った余り
        const float wrapped = std::fmod(degrees + 180.0f, 360.0f);
        return wrapped < 0.0f ? wrapped + 180.0f : wrapped - 180.0f;
    }

    // 2つの値を線形補間します(from: 始点, to: 終点, amount: 0〜1の補間率)。
    [[nodiscard]] float Lerp(
        const float from,
        const float to,
        const float amount) noexcept
    {
        return from + (to - from) * amount;
    }

    // 2つのXYの差を返します(left: 引かれる値, right: 引く値)。
    [[nodiscard]] DirectX::XMFLOAT2 Subtract(
        const DirectX::XMFLOAT2& left,
        const DirectX::XMFLOAT2& right) noexcept
    {
        return { left.x - right.x, left.y - right.y };
    }

    // 2つのXYの和を返します(left: 足される値, right: 足す値)。
    [[nodiscard]] DirectX::XMFLOAT2 Add(
        const DirectX::XMFLOAT2& left,
        const DirectX::XMFLOAT2& right) noexcept
    {
        return { left.x + right.x, left.y + right.y };
    }

    // XYを倍率で拡縮します(value: 元の値, factor: 倍率)。
    [[nodiscard]] DirectX::XMFLOAT2 Scale(
        const DirectX::XMFLOAT2& value,
        const float factor) noexcept
    {
        return { value.x * factor, value.y * factor };
    }

    // 2点を線形補間します(from: 始点, to: 終点, amount: 0〜1の補間率)。
    [[nodiscard]] DirectX::XMFLOAT2 Lerp2(
        const DirectX::XMFLOAT2& from,
        const DirectX::XMFLOAT2& to,
        const float amount) noexcept
    {
        return Add(from, Scale(Subtract(to, from), amount));
    }

    // XYの長さを返します(value: 長さを求める値)。
    [[nodiscard]] float Length(const DirectX::XMFLOAT2& value) noexcept
    {
        return std::sqrt(value.x * value.x + value.y * value.y);
    }

    // fromからtoへの符号付き角度を-π〜πで返します(from: 基準方向, to: 対象方向)。
    [[nodiscard]] float SignedAngle(
        const DirectX::XMFLOAT2& from,
        const DirectX::XMFLOAT2& to) noexcept
    {
        return std::atan2(
            from.x * to.y - from.y * to.x,
            from.x * to.x + from.y * to.y);
    }

    // XYをZ回転で回します(value: 回す値, radians: 回転角)。
    [[nodiscard]] DirectX::XMFLOAT2 Rotate(
        const DirectX::XMFLOAT2& value,
        const float radians) noexcept
    {
        // 回転角の余弦
        const float cosine = std::cos(radians);
        // 回転角の正弦
        const float sine = std::sin(radians);
        return {
            value.x * cosine - value.y * sine,
            value.x * sine + value.y * cosine
        };
    }

    // XYの各成分を有限かつ上限内へ収めます(value: 補正する値, fallback: 非有限時の値)。
    [[nodiscard]] DirectX::XMFLOAT2 SanitizeFloat2(
        const DirectX::XMFLOAT2& value,
        const DirectX::XMFLOAT2& fallback) noexcept
    {
        return {
            std::clamp(FiniteOr(value.x, fallback.x), -MaximumMagnitude, MaximumMagnitude),
            std::clamp(FiniteOr(value.y, fallback.y), -MaximumMagnitude, MaximumMagnitude)
        };
    }

    // XY平面の行ベクトル用アフィン変換です(x' = a*x + c*y + tx, y' = b*x + d*y + ty)。
    struct Affine2D final
    {
        // X軸がXへ移る量
        float a{ 1.0f };
        // X軸がYへ移る量
        float b{};
        // Y軸がXへ移る量
        float c{};
        // Y軸がYへ移る量
        float d{ 1.0f };
        // X方向の平行移動
        float tx{};
        // Y方向の平行移動
        float ty{};
    };

    // 点を変換します(matrix: 変換, point: 入力の点)。
    [[nodiscard]] DirectX::XMFLOAT2 Apply(
        const Affine2D& matrix,
        const DirectX::XMFLOAT2& point) noexcept
    {
        return {
            matrix.a * point.x + matrix.c * point.y + matrix.tx,
            matrix.b * point.x + matrix.d * point.y + matrix.ty
        };
    }

    // firstの後にsecondを適用する変換を返します(first: 先の変換, second: 後の変換)。
    [[nodiscard]] Affine2D Compose(
        const Affine2D& first,
        const Affine2D& second) noexcept
    {
        return {
            second.a * first.a + second.c * first.b,
            second.b * first.a + second.d * first.b,
            second.a * first.c + second.c * first.d,
            second.b * first.c + second.d * first.d,
            second.a * first.tx + second.c * first.ty + second.tx,
            second.b * first.tx + second.d * first.ty + second.ty
        };
    }

    // 行列式を返します(matrix: 変換)。
    [[nodiscard]] float Determinant(const Affine2D& matrix) noexcept
    {
        return matrix.a * matrix.d - matrix.b * matrix.c;
    }

    // 有限かつ可逆か返します(matrix: 変換)。
    [[nodiscard]] bool IsInvertible(const Affine2D& matrix) noexcept
    {
        // 行列式
        const float determinant = Determinant(matrix);
        return std::isfinite(determinant)
            && std::abs(determinant) > MinimumDeterminant;
    }

    // 逆変換を返します(matrix: 可逆な変換)。
    [[nodiscard]] Affine2D Inverse(const Affine2D& matrix) noexcept
    {
        // 行列式の逆数
        const float inverseDeterminant = 1.0f / Determinant(matrix);
        // 逆変換の線形部分
        Affine2D result{
            matrix.d * inverseDeterminant,
            -matrix.b * inverseDeterminant,
            -matrix.c * inverseDeterminant,
            matrix.a * inverseDeterminant,
            0.0f,
            0.0f };
        result.tx = -(result.a * matrix.tx + result.c * matrix.ty);
        result.ty = -(result.b * matrix.tx + result.d * matrix.ty);
        return result;
    }

    // Web行列のXY平面成分を取り出します(matrix: 列ベクトル用の4×4行列)。
    [[nodiscard]] Affine2D FromMat4(const LamaPon::Web::Mat4& matrix) noexcept
    {
        return {
            matrix.values[0], matrix.values[1],
            matrix.values[4], matrix.values[5],
            matrix.values[12], matrix.values[13] };
    }

    // 保存形式の行列のXY平面成分を取り出します(matrix: 行ベクトル用の4×4行列)。
    [[nodiscard]] Affine2D FromFloat4x4(const DirectX::XMFLOAT4X4& matrix) noexcept
    {
        return { matrix._11, matrix._12, matrix._21, matrix._22, matrix._41, matrix._42 };
    }

    // 保存形式の行列へ変換します(matrix: XY平面の変換)。
    [[nodiscard]] DirectX::XMFLOAT4X4 ToFloat4x4(const Affine2D& matrix) noexcept
    {
        // 保存形式の行列
        DirectX::XMFLOAT4X4 result{};
        result._11 = matrix.a;
        result._12 = matrix.b;
        result._21 = matrix.c;
        result._22 = matrix.d;
        result._41 = matrix.tx;
        result._42 = matrix.ty;
        return result;
    }

    // 行順の16要素へ並べる行列の成分
    constexpr float DirectX::XMFLOAT4X4::* MatrixFields[16]{
        &DirectX::XMFLOAT4X4::_11, &DirectX::XMFLOAT4X4::_12,
        &DirectX::XMFLOAT4X4::_13, &DirectX::XMFLOAT4X4::_14,
        &DirectX::XMFLOAT4X4::_21, &DirectX::XMFLOAT4X4::_22,
        &DirectX::XMFLOAT4X4::_23, &DirectX::XMFLOAT4X4::_24,
        &DirectX::XMFLOAT4X4::_31, &DirectX::XMFLOAT4X4::_32,
        &DirectX::XMFLOAT4X4::_33, &DirectX::XMFLOAT4X4::_34,
        &DirectX::XMFLOAT4X4::_41, &DirectX::XMFLOAT4X4::_42,
        &DirectX::XMFLOAT4X4::_43, &DirectX::XMFLOAT4X4::_44 };

    // 行順16要素のJSONから行列を読み、形が不正なら単位行列を返します(value: JSON配列)。
    [[nodiscard]] DirectX::XMFLOAT4X4 ReadMatrix(const Json& value)
    {
        // 読み込んだ行列
        DirectX::XMFLOAT4X4 result{};
        if (!value.is_array() || value.size() != 16)
        {
            return result;
        }
        // 読み込む成分番号
        for (std::size_t index = 0; index < 16; ++index)
        {
            result.*MatrixFields[index] = value.at(index).get<float>();
        }
        return result;
    }

    // JSON配列から2成分を読みます(value: JSON値, fallback: 既定値)。
    [[nodiscard]] DirectX::XMFLOAT2 ReadFloat2(
        const Json& value,
        const DirectX::XMFLOAT2& fallback)
    {
        return value.is_array() && value.size() >= 2
            ? DirectX::XMFLOAT2{ value.at(0).get<float>(), value.at(1).get<float>() }
            : fallback;
    }

    // JSON配列から3成分を読みます(value: JSON値, fallback: 既定値)。
    [[nodiscard]] DirectX::XMFLOAT3 ReadFloat3(
        const Json& value,
        const DirectX::XMFLOAT3& fallback)
    {
        return value.is_array() && value.size() >= 3
            ? DirectX::XMFLOAT3{
                value.at(0).get<float>(),
                value.at(1).get<float>(),
                value.at(2).get<float>() }
            : fallback;
    }

    // JSON配列から4成分を読みます(value: JSON値, fallback: 既定値)。
    [[nodiscard]] DirectX::XMFLOAT4 ReadFloat4(
        const Json& value,
        const DirectX::XMFLOAT4& fallback)
    {
        return value.is_array() && value.size() >= 4
            ? DirectX::XMFLOAT4{
                value.at(0).get<float>(),
                value.at(1).get<float>(),
                value.at(2).get<float>(),
                value.at(3).get<float>() }
            : fallback;
    }

    // ローカル変換の行列を返します(transform: 位置・回転・拡縮)。
    [[nodiscard]] LamaPon::Web::Mat4 LocalMatrix(const LamaPon::Transform& transform)
    {
        return LamaPon::Web::Multiply(
            LamaPon::Web::Translation({
                transform.position.x, transform.position.y, transform.position.z }),
            LamaPon::Web::Multiply(
                LamaPon::Web::RotationY(transform.rotation.y),
                LamaPon::Web::Multiply(
                    LamaPon::Web::RotationX(transform.rotation.x),
                    LamaPon::Web::Multiply(
                        LamaPon::Web::RotationZ(transform.rotation.z),
                        LamaPon::Web::Scale({
                            transform.scale.x, transform.scale.y, transform.scale.z })))));
    }

    // 親を含むワールド行列を返します(object: 対象)。
    [[nodiscard]] LamaPon::Web::Mat4 WorldMatrix(const GameObject& object)
    {
        // 親を含まないローカル行列
        const auto local = LocalMatrix(object.GetTransform());
        return object.Parent() != nullptr
            ? LamaPon::Web::Multiply(WorldMatrix(*object.Parent()), local)
            : local;
    }

    // 親を含むXY平面のワールド変換を返します(object: 対象)。
    [[nodiscard]] Affine2D WorldAffine(const GameObject& object)
    {
        return FromMat4(WorldMatrix(object));
    }

    // オイラー角(ラジアン)をクォータニオンへ変換します(euler: X・Y・Z回転)。
    // Z、X、Yの順に回す回転で、DirectXMathのRollPitchYawと同じ向きです。
    [[nodiscard]] DirectX::XMFLOAT4 EulerToQuaternion(const DirectX::XMFLOAT3& euler) noexcept
    {
        // X回転の半角の余弦
        const float cp = std::cos(euler.x * 0.5f);
        // X回転の半角の正弦
        const float sp = std::sin(euler.x * 0.5f);
        // Y回転の半角の余弦
        const float cy = std::cos(euler.y * 0.5f);
        // Y回転の半角の正弦
        const float sy = std::sin(euler.y * 0.5f);
        // Z回転の半角の余弦
        const float cr = std::cos(euler.z * 0.5f);
        // Z回転の半角の正弦
        const float sr = std::sin(euler.z * 0.5f);
        return {
            cy * sp * cr + sy * cp * sr,
            sy * cp * cr - cy * sp * sr,
            cy * cp * sr - sy * sp * cr,
            cy * cp * cr + sy * sp * sr };
    }

    // クォータニオンをオイラー角(ラジアン)へ変換します(rotation: 回転クォータニオン)。
    [[nodiscard]] DirectX::XMFLOAT3 QuaternionToEuler(const DirectX::XMFLOAT4& rotation) noexcept
    {
        // 正規化に使う長さ
        const float length = std::sqrt(
            rotation.x * rotation.x + rotation.y * rotation.y
            + rotation.z * rotation.z + rotation.w * rotation.w);
        if (!std::isfinite(length) || length <= 1.0e-6f)
        {
            return {};
        }
        // 正規化したX
        const float x = rotation.x / length;
        // 正規化したY
        const float y = rotation.y / length;
        // 正規化したZ
        const float z = rotation.z / length;
        // 正規化したW
        const float w = rotation.w / length;
        // 回転行列の1行1列
        const float m11 = 1.0f - 2.0f * (y * y + z * z);
        // 回転行列の1行2列
        const float m12 = 2.0f * (x * y + z * w);
        // 回転行列の1行3列
        const float m13 = 2.0f * (x * z - y * w);
        // 回転行列の2行2列
        const float m22 = 1.0f - 2.0f * (x * x + z * z);
        // 回転行列の3行1列
        const float m31 = 2.0f * (x * z + y * w);
        // 回転行列の3行2列
        const float m32 = 2.0f * (y * z - x * w);
        // 回転行列の3行3列
        const float m33 = 1.0f - 2.0f * (x * x + y * y);
        // X回転の正弦
        const float sinPitch = std::clamp(-m32, -1.0f, 1.0f);
        // X回転
        const float pitch = std::asin(sinPitch);
        if (std::sqrt(std::max(1.0f - sinPitch * sinPitch, 0.0f)) < 1.0e-4f)
        {
            return { pitch, std::atan2(-m13, m11), 0.0f };
        }
        return { pitch, std::atan2(m31, m33), std::atan2(m12, m22) };
    }

    // 点から線分までの距離を返します(point: 測る点, start: 線分の始点, end: 線分の終点)。
    [[nodiscard]] float DistanceToSegment(
        const DirectX::XMFLOAT2& point,
        const DirectX::XMFLOAT2& start,
        const DirectX::XMFLOAT2& end) noexcept
    {
        // 線分の方向
        const auto segment = Subtract(end, start);
        // 線分の長さの二乗
        const float lengthSquared = segment.x * segment.x + segment.y * segment.y;
        // 線分上の最近点の割合
        const float amount = lengthSquared > 1.0e-8f
            ? std::clamp(
                ((point.x - start.x) * segment.x + (point.y - start.y) * segment.y)
                    / lengthSquared,
                0.0f,
                1.0f)
            : 0.0f;
        return Length(Subtract(point, Add(start, Scale(segment, amount))));
    }

    // 重みを正規化し、番号と値が有効か返します(weight: 補正する重み, boneCount: ボーン数)。
    [[nodiscard]] bool NormalizeWeight(
        LamaPon::SpriteSkinWeight& weight,
        const std::size_t boneCount) noexcept
    {
        // 重みの合計
        float total{};
        // 確認する影響の番号
        for (std::size_t index = 0; index < LamaPon::SpriteSkinWeight::MaximumInfluences; ++index)
        {
            if (!std::isfinite(weight.weights[index])
                || weight.weights[index] < 0.0f
                || (weight.weights[index] > 0.0f && weight.bones[index] >= boneCount))
            {
                return false;
            }
            total += weight.weights[index];
        }
        if (total <= 1.0e-6f)
        {
            return false;
        }
        // 正規化する重み
        for (auto& value : weight.weights)
        {
            value /= total;
        }
        return true;
    }

    // キーの全ての値が有限か返します(key: 確認するキー)。
    [[nodiscard]] bool IsUsableKey(const LamaPon::Keyform2DKey& key) noexcept
    {
        if (!IsUsable(key.value)
            || !IsUsable(key.positionOffset.x)
            || !IsUsable(key.positionOffset.y)
            || !IsUsable(key.rotationDegrees)
            || !IsUsable(key.scale.x)
            || !IsUsable(key.scale.y)
            || !IsUsable(key.opacity)
            || key.vertexOffsets.size() > MaximumMeshVertices)
        {
            return false;
        }
        return std::all_of(
            key.vertexOffsets.begin(),
            key.vertexOffsets.end(),
            // 有限の移動量か判定します(offset: 確認する移動量)。
            [](const DirectX::XMFLOAT2& offset)
            {
                return IsUsable(offset.x) && IsUsable(offset.y);
            });
    }

    // 値を挟む2つのキーと補間率です。
    struct KeySpan final
    {
        // 値以下で最も近いキー
        const LamaPon::Keyform2DKey* lower{};
        // 値以上で最も近いキー
        const LamaPon::Keyform2DKey* upper{};
        // lowerからupperへの補間率
        float amount{};
    };

    // 昇順のキーから値を挟む区間を求めます(keys: 空でない昇順のキー, value: パラメータ値)。
    [[nodiscard]] KeySpan Locate(
        const std::vector<LamaPon::Keyform2DKey>& keys,
        const float value) noexcept
    {
        if (value <= keys.front().value)
        {
            return { &keys.front(), &keys.front(), 0.0f };
        }
        if (value >= keys.back().value)
        {
            return { &keys.back(), &keys.back(), 0.0f };
        }
        // 値より大きい最初のキー
        const auto upper = std::upper_bound(
            keys.begin(),
            keys.end(),
            value,
            // 値とキーを比べます(target: 探す値, key: 比べるキー)。
            [](const float target, const LamaPon::Keyform2DKey& key)
            {
                return target < key.value;
            });
        // 値以下で最も近いキー
        const auto lower = std::prev(upper);
        // 2つのキーの値の差
        const float range = upper->value - lower->value;
        return { &*lower, &*upper, range > 0.0f ? (value - lower->value) / range : 0.0f };
    }

    // Rig2Dの一括適用の対象になるKeyform2Dの一覧を返します。
    [[nodiscard]] std::vector<LamaPon::Keyform2DComponent*>& KeyformRegistry()
    {
        // 生成済みで未破棄のKeyform2D
        static std::vector<LamaPon::Keyform2DComponent*> registry;
        return registry;
    }

    // 格子の三角形の頂点番号を作ります(columns: 横の分割数, rows: 縦の分割数, indices: 出力)。
    void BuildGridIndices(
        const int columns,
        const int rows,
        std::vector<std::uint16_t>& indices)
    {
        indices.clear();
        // 三角形を作る格子の行
        for (int row = 0; row < rows; ++row)
        {
            // 三角形を作る格子の列
            for (int column = 0; column < columns; ++column)
            {
                // 区画の左上の頂点番号
                const auto topLeft = static_cast<std::uint16_t>(row * (columns + 1) + column);
                // 区画の左下の頂点番号
                const auto bottomLeft = static_cast<std::uint16_t>(topLeft + columns + 1);
                indices.insert(
                    indices.end(),
                    {
                        topLeft,
                        static_cast<std::uint16_t>(topLeft + 1),
                        bottomLeft,
                        bottomLeft,
                        static_cast<std::uint16_t>(topLeft + 1),
                        static_cast<std::uint16_t>(bottomLeft + 1)
                    });
            }
        }
    }

    // 三角形ごとのDOM要素で2Dメッシュを描きます(objectName/objectId: 対象, texturePath: 画像の仮想パス, r/g/b/a: 色, sortOrder: 描画順, positions: 画面XY列, uvs: UV列, vertexCount: 頂点数, indices: 頂点番号列, indexCount: 頂点番号数)。
    // 各三角形は画像全体を1024四方へ広げた要素を三角形で切り抜き、CSSのアフィン行列で画面へ写します。
    EM_JS(void, RenderPortableSpriteMesh,
          (const char* objectName, double objectId, const char* texturePath,
           float r, float g, float b, float a, int sortOrder,
           const float* positions, const float* uvs, int vertexCount,
           const unsigned short* indices, int indexCount), {
        // DOM表示へ使うobject名
        const name = UTF8ToString(objectName);
        // object IDで一意にしたメッシュ要素ID
        const meshId = "lamapon-portable-mesh-" + String(Math.floor(objectId));
        // 三角形をまとめる要素
        let container = document.getElementById(meshId);
        // 初回描画時だけDOM nodeを作ります。
        if (!container) {
            // HUD layerがなければbodyを使います。
            const layer = document.getElementById("hud") || document.body;
            container = document.createElement("div");
            container.id = meshId;
            container.dataset.lamaponPortableUi = name;
            container.dataset.lamaponPortableMesh = "1";
            Object.assign(container.style, {
                position: "absolute", left: "0px", top: "0px",
                width: "0px", height: "0px", pointerEvents: "none"
            });
            layer.appendChild(container);
        }
        container.dataset.lamaponPortableFrame = String(
            document.body?.__lamaponPortableFrame || 0);
        // DOMへ設定するtexture path
        const path = UTF8ToString(texturePath);
        container.style.display = "block";
        container.style.zIndex = String(sortOrder);
        container.style.opacity = path ? String(a) : "1";
        // pathごとに一度だけ作るData URLの表
        const urls = globalThis.__lamaponPortableTextureUrls
            || (globalThis.__lamaponPortableTextureUrls = {});
        // Textureが未読込ならvirtual filesystemから取得します。
        if (path && urls[path] === undefined && globalThis.FS) {
            // Asset読込・Data URL変換の失敗を握り潰します。
            try {
                // Virtual filesystem上のtexture byte列
                const bytes = FS.readFile(path);
                // Base64変換用のbyte string
                let binary = "";
                // 巨大spreadを避けてbyte列を小分けにします(i: byte offset)。
                for (let i = 0; i < bytes.length; i += 0x8000) {
                    binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
                }
                // Header byteから判定したimage MIME type
                const imageMime = bytes.length >= 12
                    && bytes[0] === 0x52 && bytes[1] === 0x49
                    && bytes[2] === 0x46 && bytes[3] === 0x46
                    && bytes[8] === 0x57 && bytes[9] === 0x45
                    && bytes[10] === 0x42 && bytes[11] === 0x50
                        ? "image/webp"
                        : bytes.length >= 2 && bytes[0] === 0xff && bytes[1] === 0xd8
                            ? "image/jpeg"
                            : "image/png";
                urls[path] = "data:" + imageMime + ";base64," + btoa(binary);
            }
            // texture不在時は無地で描きます(error: browser例外)。
            catch (error) {
                urls[path] = null;
            }
        }
        // 描画に使うData URL
        const url = path ? urls[path] : null;
        // 画像を広げる仮想の一辺
        const size = 1024;
        // 描く三角形の数
        const triangleCount = Math.floor(indexCount / 3);
        // 画面XY列
        const xy = HEAPF32.subarray(positions >> 2, (positions >> 2) + vertexCount * 2);
        // UV列
        const uv = HEAPF32.subarray(uvs >> 2, (uvs >> 2) + vertexCount * 2);
        // 頂点番号列
        const order = HEAPU16.subarray(indices >> 1, (indices >> 1) + indexCount);
        // 余った三角形の要素を除きます。
        while (container.children.length > triangleCount) {
            container.lastChild.remove();
        }
        // 足りない三角形の要素を作ります。
        while (container.children.length < triangleCount) {
            // 新しい三角形の要素
            const created = document.createElement("div");
            Object.assign(created.style, {
                position: "absolute", left: "0px", top: "0px",
                width: size + "px", height: size + "px",
                transformOrigin: "0 0", backgroundSize: "100% 100%",
                backgroundRepeat: "no-repeat"
            });
            container.appendChild(created);
        }
        container.dataset.lamaponPortableTriangles = String(triangleCount);
        // 描く三角形の番号
        for (let i = 0; i < triangleCount; ++i) {
            // 三角形の要素
            const element = container.children[i];
            // 三角形の3頂点の番号
            const ids = [order[i * 3], order[i * 3 + 1], order[i * 3 + 2]];
            // 画像側の3頂点
            const t = ids.map((id) => [uv[id * 2] * size, uv[id * 2 + 1] * size]);
            // 画面側の3頂点
            const p = ids.map((id) => [xy[id * 2], xy[id * 2 + 1]]);
            // 画像側の2辺
            const du1 = t[1][0] - t[0][0], dv1 = t[1][1] - t[0][1];
            const du2 = t[2][0] - t[0][0], dv2 = t[2][1] - t[0][1];
            // 画像側の2辺の行列式
            const det = du1 * dv2 - du2 * dv1;
            // 潰れた三角形は描きません。
            if (Math.abs(det) < 1e-9) {
                element.style.display = "none";
                continue;
            }
            // 画面側の2辺
            const dx1 = p[1][0] - p[0][0], dy1 = p[1][1] - p[0][1];
            const dx2 = p[2][0] - p[0][0], dy2 = p[2][1] - p[0][1];
            // 画像から画面への線形部分
            const m11 = (dx1 * dv2 - dx2 * dv1) / det;
            const m12 = (dx2 * du1 - dx1 * du2) / det;
            const m21 = (dy1 * dv2 - dy2 * dv1) / det;
            const m22 = (dy2 * du1 - dy1 * du2) / det;
            // 画像から画面への平行移動
            const e = p[0][0] - m11 * t[0][0] - m12 * t[0][1];
            const f = p[0][1] - m21 * t[0][0] - m22 * t[0][1];
            // 隣の三角形との隙間を隠すため、画面で約0.5ピクセル外側へ広げます。
            // 画面の拡大率
            const scale = Math.sqrt(Math.abs(m11 * m22 - m12 * m21)) || 1;
            // 画像側で広げる量
            const grow = 0.5 / scale;
            // 画像側の三角形の重心
            const cu = (t[0][0] + t[1][0] + t[2][0]) / 3;
            const cv = (t[0][1] + t[1][1] + t[2][1]) / 3;
            // 切り抜きに使う広げた三角形
            const clip = t.map(([u, v]) => {
                // 重心からの方向
                const du = u - cu, dv = v - cv;
                // 重心からの距離
                const length = Math.hypot(du, dv) || 1;
                return (u + du / length * grow) + "px " + (v + dv / length * grow) + "px";
            });
            element.style.display = "block";
            element.style.clipPath = "polygon(" + clip.join(",") + ")";
            element.style.transform = "matrix(" + [m11, m21, m12, m22, e, f].join(",") + ")";
            // 画像の有無で背景を切り替えます。
            if (url) {
                // 画像が変わった要素だけ背景を差し替えます。
                if (element.dataset.lamaponTexturePath !== path) {
                    element.style.backgroundImage = "url(" + url + ")";
                    element.style.backgroundColor = "transparent";
                    element.dataset.lamaponTexturePath = path;
                }
            }
            // 画像のない三角形は色で塗ります。
            else {
                element.style.backgroundImage = "none";
                element.dataset.lamaponTexturePath = "";
                element.style.backgroundColor = "rgba(" + (r * 255) + "," + (g * 255)
                    + "," + (b * 255) + "," + a + ")";
            }
        }
    });
}

namespace LamaPon
{
    void SpriteRendererComponent::SetMeshGrid(const int columns, const int rows)
    {
        m_meshColumns = std::clamp(columns, 1, 64);
        m_meshRows = std::clamp(rows, 1, 64);
        if (!m_meshDeformation.empty() && m_meshDeformation.size() != MeshVertexCount())
        {
            m_meshDeformation.clear();
        }
    }

    std::vector<DirectX::XMFLOAT2> SpriteRendererComponent::MeshRestPositions() const
    {
        // 上の行から並べる静止頂点
        std::vector<DirectX::XMFLOAT2> positions;
        positions.reserve(MeshVertexCount());
        // 頂点の行番号
        for (int row = 0; row <= m_meshRows; ++row)
        {
            // 頂点の列番号
            for (int column = 0; column <= m_meshColumns; ++column)
            {
                positions.push_back({
                    (static_cast<float>(column) / static_cast<float>(m_meshColumns) - m_pivot.x)
                        * m_size.x,
                    (static_cast<float>(row) / static_cast<float>(m_meshRows) - m_pivot.y)
                        * m_size.y });
            }
        }
        return positions;
    }

    bool SpriteRendererComponent::SetMeshDeformation(std::vector<DirectX::XMFLOAT2> positions)
    {
        // 格子と同数で全座標が有限か
        const bool valid = positions.size() == MeshVertexCount()
            && std::all_of(
                positions.begin(),
                positions.end(),
                // 有限の座標か判定します(position: 確認する頂点)。
                [](const DirectX::XMFLOAT2& position)
                {
                    return std::isfinite(position.x) && std::isfinite(position.y);
                });
        if (!valid)
        {
            m_meshDeformation.clear();
            return positions.empty();
        }
        m_meshDeformation = std::move(positions);
        return true;
    }

    bool SpriteRendererComponent::UsesMesh() const
    {
        if (Owner().GetComponent<UIRectTransformComponent>() != nullptr)
        {
            return false;
        }
        if (m_meshColumns * m_meshRows > 1 || !m_meshDeformation.empty())
        {
            return true;
        }
        // 変形部品を探す同じGameObjectの部品
        for (const auto& component : Owner().Components())
        {
            // 頂点を変形する部品
            const auto* deformer = dynamic_cast<const SpriteMeshDeformer*>(component.get());
            if (deformer != nullptr && deformer->IsEnabled() && deformer->DeformsSpriteMesh())
            {
                return true;
            }
        }
        return false;
    }

    std::vector<DirectX::XMFLOAT2> SpriteRendererComponent::DeformedMeshPositions(
        const SpriteMeshDeformer* const skipped) const
    {
        // 変形の起点にする頂点
        const auto base = m_meshDeformation.empty() ? MeshRestPositions() : m_meshDeformation;
        // 変形を重ねる格子頂点
        auto positions = base;
        // 変形部品を探す同じGameObjectの部品
        for (const auto& component : Owner().Components())
        {
            // 頂点を変形する部品
            auto* const deformer = dynamic_cast<SpriteMeshDeformer*>(component.get());
            if (deformer == nullptr || deformer == skipped || !deformer->IsEnabled())
            {
                continue;
            }
            deformer->DeformSpriteMesh(*this, positions);
            // 変形後の全頂点が有限で数が保たれたか
            const bool valid = positions.size() == base.size()
                && std::all_of(
                    positions.begin(),
                    positions.end(),
                    // 有限の座標か判定します(position: 確認する頂点)。
                    [](const DirectX::XMFLOAT2& position)
                    {
                        return std::isfinite(position.x) && std::isfinite(position.y);
                    });
            if (!valid)
            {
                return base;
            }
        }
        return positions;
    }

    Sway2DComponent::Sway2DComponent(const Sway2DSettings& settings) noexcept
        : m_settings(Sanitize(settings))
    {
    }

    void Sway2DComponent::SetSettings(const Sway2DSettings& settings) noexcept
    {
        m_settings = Sanitize(settings);
    }

    Sway2DSettings Sway2DComponent::Sanitize(Sway2DSettings settings) noexcept
    {
        // 非有限値の置き換えに使う既定設定
        const Sway2DSettings defaults{};
        settings.tipOffset = SanitizeFloat2(settings.tipOffset, defaults.tipOffset);
        settings.stiffness = std::clamp(FiniteOr(settings.stiffness, defaults.stiffness), 0.0f, 10000.0f);
        settings.damping = std::clamp(FiniteOr(settings.damping, defaults.damping), 0.0f, 1000.0f);
        settings.inertia = std::clamp(FiniteOr(settings.inertia, defaults.inertia), 0.0f, 1.0f);
        settings.gravity = SanitizeFloat2(settings.gravity, defaults.gravity);
        settings.maxAngleDegrees = std::clamp(
            FiniteOr(settings.maxAngleDegrees, defaults.maxAngleDegrees), 0.0f, 180.0f);
        settings.windAmplitudeDegrees = std::clamp(
            FiniteOr(settings.windAmplitudeDegrees, defaults.windAmplitudeDegrees), 0.0f, 180.0f);
        settings.windFrequency = std::clamp(
            FiniteOr(settings.windFrequency, defaults.windFrequency), 0.0f, 60.0f);
        settings.windPhaseDegrees = std::fmod(
            std::clamp(
                FiniteOr(settings.windPhaseDegrees, defaults.windPhaseDegrees),
                -MaximumMagnitude,
                MaximumMagnitude),
            360.0f);
        return settings;
    }

    void Sway2DComponent::ResetSimulation() noexcept
    {
        m_simulationReady = false;
        m_velocity = {};
        m_angle = 0.0f;
    }

    Blink2DComponent::Blink2DComponent(const Blink2DSettings& settings) noexcept
        : m_settings(Sanitize(settings))
    {
    }

    void Blink2DComponent::SetSettings(const Blink2DSettings& settings) noexcept
    {
        m_settings = Sanitize(settings);
        m_secondsUntilBlink = std::min(m_secondsUntilBlink, m_settings.intervalMaxSeconds);
    }

    Blink2DSettings Blink2DComponent::Sanitize(Blink2DSettings settings) noexcept
    {
        // 非有限値の置き換えに使う既定設定
        const Blink2DSettings defaults{};
        settings.columns = std::clamp(settings.columns, 1, 256);
        settings.rows = std::clamp(settings.rows, 1, 256);
        settings.openFrame = std::clamp(settings.openFrame, 0, 65535);
        settings.closingStartFrame = std::clamp(settings.closingStartFrame, 0, 65535);
        settings.closingFrameCount = std::clamp(settings.closingFrameCount, 1, 64);
        settings.frameSeconds = std::clamp(
            FiniteOr(settings.frameSeconds, defaults.frameSeconds), 0.001f, 10.0f);
        settings.closedSeconds = std::clamp(
            FiniteOr(settings.closedSeconds, defaults.closedSeconds), 0.0f, 10.0f);
        settings.intervalMinSeconds = std::clamp(
            FiniteOr(settings.intervalMinSeconds, defaults.intervalMinSeconds), 0.05f, 3600.0f);
        settings.intervalMaxSeconds = std::clamp(
            FiniteOr(settings.intervalMaxSeconds, defaults.intervalMaxSeconds),
            settings.intervalMinSeconds,
            3600.0f);
        settings.doubleBlinkChance = std::clamp(
            FiniteOr(settings.doubleBlinkChance, defaults.doubleBlinkChance), 0.0f, 1.0f);
        return settings;
    }

    void Blink2DComponent::Blink() noexcept
    {
        if (m_phase == 0)
        {
            m_phase = 1;
            m_phaseSeconds = 0.0f;
        }
    }

    void Blink2DComponent::SetRandomSeed(const std::uint32_t seed) noexcept
    {
        m_random = static_cast<std::uint32_t>(seed % RandomModulus);
        if (m_random == 0)
        {
            m_random = 1;
        }
        m_seeded = true;
        ScheduleNextBlink(false);
    }

    int Blink2DComponent::CurrentFrame() const noexcept
    {
        // 閉じきる前の途中コマ数
        const int transitional = m_settings.closingFrameCount - 1;
        // 経過秒数から求めた途中コマの位置
        const int step = transitional > 0
            ? std::min(static_cast<int>(m_phaseSeconds / m_settings.frameSeconds), transitional - 1)
            : 0;
        switch (m_phase)
        {
        case 1:
            return m_settings.closingStartFrame + step;
        case 2:
            return m_settings.closingStartFrame + transitional;
        case 3:
            return m_settings.closingStartFrame + std::max(transitional - 1 - step, 0);
        default:
            return m_settings.openFrame;
        }
    }

    void Blink2DComponent::Advance(float deltaTime) noexcept
    {
        // 片道の途中コマを表示する合計秒数
        const float transitionSeconds =
            static_cast<float>(m_settings.closingFrameCount - 1) * m_settings.frameSeconds;
        // 段階を切り替えた回数
        for (int stepCount = 0; stepCount < MaximumBlinkSteps; ++stepCount)
        {
            switch (m_phase)
            {
            case 0:
                if (!m_holdClosed)
                {
                    if (!m_settings.autoBlink || m_secondsUntilBlink > deltaTime)
                    {
                        if (m_settings.autoBlink)
                        {
                            m_secondsUntilBlink -= deltaTime;
                        }
                        return;
                    }
                    deltaTime -= m_secondsUntilBlink;
                }
                m_secondsUntilBlink = 0.0f;
                m_phase = 1;
                m_phaseSeconds = 0.0f;
                break;
            case 1:
                if (m_phaseSeconds + deltaTime < transitionSeconds)
                {
                    m_phaseSeconds += deltaTime;
                    return;
                }
                deltaTime -= transitionSeconds - m_phaseSeconds;
                m_phase = 2;
                m_phaseSeconds = 0.0f;
                break;
            case 2:
                if (m_holdClosed)
                {
                    return;
                }
                if (m_phaseSeconds + deltaTime < m_settings.closedSeconds)
                {
                    m_phaseSeconds += deltaTime;
                    return;
                }
                deltaTime -= std::max(m_settings.closedSeconds - m_phaseSeconds, 0.0f);
                m_phase = 3;
                m_phaseSeconds = 0.0f;
                break;
            default:
                if (m_phaseSeconds + deltaTime < transitionSeconds)
                {
                    m_phaseSeconds += deltaTime;
                    return;
                }
                deltaTime -= transitionSeconds - m_phaseSeconds;
                m_phase = 0;
                m_phaseSeconds = 0.0f;
                ScheduleNextBlink(true);
                break;
            }
            deltaTime = std::max(deltaTime, 0.0f);
        }
    }

    void Blink2DComponent::ScheduleNextBlink(const bool allowDouble) noexcept
    {
        if (allowDouble
            && !m_lastWasDouble
            && m_settings.doubleBlinkChance > 0.0f
            && RandomRange(0.0f, 1.0f) < m_settings.doubleBlinkChance)
        {
            m_secondsUntilBlink = RandomRange(DoubleBlinkMinimumSeconds, DoubleBlinkMaximumSeconds);
            m_lastWasDouble = true;
            return;
        }
        m_secondsUntilBlink = RandomRange(
            m_settings.intervalMinSeconds,
            m_settings.intervalMaxSeconds);
        m_lastWasDouble = false;
    }

    float Blink2DComponent::RandomRange(const float minimum, const float maximum) noexcept
    {
        m_random = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(m_random) * RandomMultiplier) % RandomModulus);
        // 0〜1へ正規化した乱数
        const double unit = static_cast<double>(m_random - 1u)
            / static_cast<double>(RandomModulus - 2u);
        return maximum <= minimum
            ? minimum
            : minimum + static_cast<float>(unit) * (maximum - minimum);
    }

    SpriteSkin2DComponent::SpriteSkin2DComponent(std::vector<std::uint64_t> bones)
        : m_bones(std::move(bones))
    {
    }

    void SpriteSkin2DComponent::SetBones(std::vector<std::uint64_t> bones)
    {
        m_bones = std::move(bones);
        m_boneObjects.clear();
        m_weights.clear();
        Unbind();
    }

    bool SpriteSkin2DComponent::RemapBones(std::vector<std::uint64_t> bones)
    {
        if (bones.size() != m_bones.size())
        {
            return false;
        }
        m_bones = std::move(bones);
        m_boneObjects.clear();
        return true;
    }

    void SpriteSkin2DComponent::SetWeightFalloff(const float falloff) noexcept
    {
        m_weightFalloff = std::isfinite(falloff)
            ? std::clamp(falloff, 0.5f, 16.0f)
            : DefaultWeightFalloff;
    }

    bool SpriteSkin2DComponent::Bind()
    {
        // 格子を持つ同じGameObjectのSprite Renderer
        const auto* sprite = Owner().GetComponent<SpriteRendererComponent>();
        if (sprite == nullptr
            || m_bones.empty()
            || m_bones.size() > std::numeric_limits<std::uint16_t>::max()
            || m_boneObjects.size() != m_bones.size())
        {
            return false;
        }
        // 今回記録するボーンの姿勢
        std::vector<DirectX::XMFLOAT4X4> bonePoses;
        bonePoses.reserve(m_boneObjects.size());
        // 姿勢を記録するボーン
        for (const auto* bone : m_boneObjects)
        {
            if (bone == nullptr)
            {
                return false;
            }
            // ボーンのワールド変換
            const auto world = WorldAffine(*bone);
            if (!IsInvertible(world))
            {
                return false;
            }
            bonePoses.push_back(ToFloat4x4(world));
        }
        // Spriteのワールド変換
        const auto spriteWorld = WorldAffine(Owner());
        if (!IsInvertible(spriteWorld))
        {
            return false;
        }
        m_boneBindPoses = std::move(bonePoses);
        m_spriteBindPose = ToFloat4x4(spriteWorld);
        m_boundColumns = sprite->MeshColumns();
        m_boundRows = sprite->MeshRows();
        m_bound = true;
        if (m_weights.size() != sprite->MeshVertexCount())
        {
            static_cast<void>(ComputeAutomaticWeights());
        }
        return true;
    }

    bool SpriteSkin2DComponent::ComputeAutomaticWeights()
    {
        // 格子を持つ同じGameObjectのSprite Renderer
        const auto* sprite = Owner().GetComponent<SpriteRendererComponent>();
        if (!m_bound
            || sprite == nullptr
            || sprite->MeshColumns() != m_boundColumns
            || sprite->MeshRows() != m_boundRows)
        {
            return false;
        }
        // 各ボーンの影響範囲の始点
        std::vector<DirectX::XMFLOAT2> starts;
        // 各ボーンの影響範囲の終点
        std::vector<DirectX::XMFLOAT2> ends;
        // 始点を求めるボーンの姿勢
        for (const auto& pose : m_boneBindPoses)
        {
            starts.push_back({ pose._41, pose._42 });
            ends.push_back({ pose._41, pose._42 });
        }
        // 子のボーンへ向かう線分を影響範囲にします。
        // 子を探すボーン番号
        for (std::size_t child = 0; child < m_boneObjects.size(); ++child)
        {
            // 子の親のGameObject
            const auto* parentObject = m_boneObjects[child] != nullptr
                ? m_boneObjects[child]->Parent()
                : nullptr;
            // 親にあたるボーン番号
            for (std::size_t parent = 0; parent < m_boneObjects.size(); ++parent)
            {
                if (parentObject != nullptr
                    && parentObject == m_boneObjects[parent]
                    && ends[parent].x == starts[parent].x
                    && ends[parent].y == starts[parent].y)
                {
                    ends[parent] = starts[child];
                }
            }
        }
        // 子のない末端のボーンは、親のボーンからの向きと長さで先へ延ばします。
        // 延ばす末端のボーン番号
        for (std::size_t tip = 0; tip < m_boneObjects.size(); ++tip)
        {
            if (ends[tip].x != starts[tip].x || ends[tip].y != starts[tip].y)
            {
                continue;
            }
            // 末端の親のGameObject
            const auto* parentObject = m_boneObjects[tip] != nullptr
                ? m_boneObjects[tip]->Parent()
                : nullptr;
            // 親にあたるボーン番号
            for (std::size_t parent = 0; parent < m_boneObjects.size(); ++parent)
            {
                if (parentObject != nullptr && parentObject == m_boneObjects[parent])
                {
                    ends[tip] = {
                        starts[tip].x * 2.0f - starts[parent].x,
                        starts[tip].y * 2.0f - starts[parent].y };
                    break;
                }
            }
        }

        // バインド時のSpriteのワールド変換
        const auto spriteBind = FromFloat4x4(m_spriteBindPose);
        // 重みを付ける静止頂点
        const auto rest = sprite->MeshRestPositions();
        // 各ボーンの距離による重み
        std::vector<float> scores(m_bones.size());
        // 重みの大きい順のボーン番号
        std::vector<std::uint16_t> order(m_bones.size());
        // 新しい頂点ごとの重み
        std::vector<SpriteSkinWeight> weights;
        weights.reserve(rest.size());
        // 重みを付ける頂点
        for (const auto& local : rest)
        {
            // 頂点のバインド時のワールド位置
            const auto world = Apply(spriteBind, local);
            // 距離を測るボーン番号
            for (std::size_t bone = 0; bone < m_bones.size(); ++bone)
            {
                scores[bone] = 1.0f / std::pow(
                    std::max(DistanceToSegment(world, starts[bone], ends[bone]), MinimumWeightDistance),
                    m_weightFalloff);
            }
            std::iota(order.begin(), order.end(), std::uint16_t{});
            // 影響として残すボーンの数
            const std::size_t kept = std::min(order.size(), SpriteSkinWeight::MaximumInfluences);
            std::partial_sort(
                order.begin(),
                order.begin() + static_cast<std::ptrdiff_t>(kept),
                order.end(),
                // 重みの大きい順に並べます(left: 一方の番号, right: 他方の番号)。
                [&scores](const std::uint16_t left, const std::uint16_t right)
                {
                    return scores[left] > scores[right];
                });
            // この頂点の重み
            SpriteSkinWeight weight{};
            weight.weights = {};
            // 残す影響の番号
            for (std::size_t index = 0; index < kept; ++index)
            {
                weight.bones[index] = order[index];
                weight.weights[index] = scores[order[index]];
            }
            if (!NormalizeWeight(weight, m_bones.size()))
            {
                weight = {};
            }
            weights.push_back(weight);
        }
        m_weights = std::move(weights);
        return true;
    }

    void SpriteSkin2DComponent::Unbind() noexcept
    {
        m_bound = false;
        m_boneBindPoses.clear();
        m_boundColumns = 0;
        m_boundRows = 0;
    }

    bool SpriteSkin2DComponent::SetWeights(std::vector<SpriteSkinWeight> weights)
    {
        if (!m_bound
            || weights.size()
                != static_cast<std::size_t>(m_boundColumns + 1)
                    * static_cast<std::size_t>(m_boundRows + 1))
        {
            return false;
        }
        // 正規化する頂点の重み
        for (auto& weight : weights)
        {
            if (!NormalizeWeight(weight, m_bones.size()))
            {
                return false;
            }
        }
        m_weights = std::move(weights);
        return true;
    }

    bool SpriteSkin2DComponent::RestoreBinding(
        std::vector<DirectX::XMFLOAT4X4> boneBindPoses,
        const DirectX::XMFLOAT4X4& spriteBindPose,
        std::vector<SpriteSkinWeight> weights,
        const int columns,
        const int rows)
    {
        Unbind();
        if (boneBindPoses.size() != m_bones.size()
            || m_bones.empty()
            || columns < 1
            || rows < 1
            || weights.size()
                != static_cast<std::size_t>(columns + 1) * static_cast<std::size_t>(rows + 1)
            || !IsInvertible(FromFloat4x4(spriteBindPose)))
        {
            return false;
        }
        // 確認するボーンの姿勢
        for (const auto& pose : boneBindPoses)
        {
            if (!IsInvertible(FromFloat4x4(pose)))
            {
                return false;
            }
        }
        // 正規化する頂点の重み
        for (auto& weight : weights)
        {
            if (!NormalizeWeight(weight, m_bones.size()))
            {
                return false;
            }
        }
        m_boneBindPoses = std::move(boneBindPoses);
        m_spriteBindPose = spriteBindPose;
        m_weights = std::move(weights);
        m_boundColumns = columns;
        m_boundRows = rows;
        m_bound = true;
        return true;
    }

    std::vector<GameObject*> SpriteSkin2DComponent::CreateBoneChain(
        const int boneCount,
        const bool addSway)
    {
        // 長さを決めるSprite Renderer
        const auto* sprite = Owner().GetComponent<SpriteRendererComponent>();
        if (sprite == nullptr)
        {
            return {};
        }
        // 1〜16へ収めたボーン数
        const int count = std::clamp(boneCount, 1, 16);
        // 基準点から反対側の端までのローカル位置
        DirectX::XMFLOAT2 tip{
            (1.0f - 2.0f * sprite->Pivot().x) * sprite->Size().x,
            (1.0f - 2.0f * sprite->Pivot().y) * sprite->Size().y };
        if (std::abs(tip.x) + std::abs(tip.y) < 1.0e-3f)
        {
            tip = { 0.0f, sprite->Size().y * 0.5f };
        }
        // 1本のボーンの長さと向き
        const auto segment = Scale(tip, 1.0f / static_cast<float>(count));
        // 作ったボーン
        std::vector<GameObject*> bones;
        // 作ったボーンのID列
        std::vector<std::uint64_t> ids;
        // 新しいボーンの親
        GameObject* parent = &Owner();
        // 作るボーンの番号
        for (int index = 0; index < count; ++index)
        {
            // 新しいボーン
            auto& bone = Owner().GetScene().CreateGameObject(
                Owner().Name() + "_Bone" + std::to_string(index));
            bone.SetParent(parent);
            bone.GetTransform().position = index == 0
                ? DirectX::XMFLOAT3{}
                : DirectX::XMFLOAT3{ segment.x, segment.y, 0.0f };
            if (addSway && (index > 0 || count == 1))
            {
                // 次のボーンへ向かう揺れ設定
                Sway2DSettings settings{};
                settings.tipOffset = segment;
                bone.AddComponent<Sway2DComponent>(settings);
            }
            bones.push_back(&bone);
            ids.push_back(bone.Id());
            parent = &bone;
        }
        SetBones(std::move(ids));
        m_boneObjects = bones;
        static_cast<void>(Bind());
        return bones;
    }

    void SpriteSkin2DComponent::DeformSpriteMesh(
        const SpriteRendererComponent& sprite,
        std::vector<DirectX::XMFLOAT2>& positions)
    {
        if (!m_bound
            || sprite.MeshColumns() != m_boundColumns
            || sprite.MeshRows() != m_boundRows
            || positions.size() != m_weights.size())
        {
            return;
        }
        // 現在のSpriteのワールド変換
        const auto spriteWorld = WorldAffine(Owner());
        if (!IsInvertible(spriteWorld))
        {
            return;
        }
        // ワールドからSpriteのローカルへ戻す変換
        const auto worldToSprite = Inverse(spriteWorld);
        // バインド時のSpriteのワールド変換
        const auto spriteBind = FromFloat4x4(m_spriteBindPose);
        // 見つからないボーンはSpriteと一緒に動かします。
        // Spriteの動きだけを写す変換
        const auto rigid = Compose(Inverse(spriteBind), spriteWorld);
        // バインド時から現在へ頂点を移すボーンごとの変換
        std::vector<Affine2D> skinning(m_bones.size(), rigid);
        // 変換を求めるボーン番号
        for (std::size_t index = 0; index < m_bones.size(); ++index)
        {
            // ボーンにするGameObject
            const auto* bone = index < m_boneObjects.size() ? m_boneObjects[index] : nullptr;
            if (bone != nullptr)
            {
                skinning[index] = Compose(
                    Inverse(FromFloat4x4(m_boneBindPoses[index])),
                    WorldAffine(*bone));
            }
        }
        // 変形する頂点番号
        for (std::size_t vertex = 0; vertex < positions.size(); ++vertex)
        {
            // 頂点のバインド時のワールド位置
            const auto bindWorld = Apply(spriteBind, positions[vertex]);
            // 重みで混ぜた現在のワールド位置
            DirectX::XMFLOAT2 blended{};
            // 頂点の重み
            const auto& weight = m_weights[vertex];
            // 混ぜる影響の番号
            for (std::size_t influence = 0; influence < SpriteSkinWeight::MaximumInfluences; ++influence)
            {
                if (weight.weights[influence] <= 0.0f)
                {
                    continue;
                }
                blended = Add(
                    blended,
                    Scale(
                        Apply(skinning[weight.bones[influence]], bindWorld),
                        weight.weights[influence]));
            }
            positions[vertex] = Apply(worldToSprite, blended);
        }
    }

    Rig2DComponent::Rig2DComponent(std::vector<Rig2DParameter> parameters)
    {
        SetParameters(std::move(parameters));
    }

    void Rig2DComponent::SetParameters(std::vector<Rig2DParameter> parameters)
    {
        m_parameters.clear();
        // 登録するパラメータ
        for (auto& parameter : parameters)
        {
            if (FindParameter(parameter.name) == nullptr)
            {
                static_cast<void>(AddParameter(std::move(parameter)));
            }
        }
    }

    bool Rig2DComponent::AddParameter(Rig2DParameter parameter)
    {
        if (parameter.name.empty() || parameter.name.size() > MaximumParameterNameLength)
        {
            return false;
        }
        parameter.minimum = std::clamp(FiniteOr(parameter.minimum, 0.0f), -MaximumMagnitude, MaximumMagnitude);
        parameter.maximum = std::clamp(FiniteOr(parameter.maximum, 1.0f), -MaximumMagnitude, MaximumMagnitude);
        if (parameter.maximum < parameter.minimum)
        {
            std::swap(parameter.minimum, parameter.maximum);
        }
        parameter.defaultValue = std::clamp(
            FiniteOr(parameter.defaultValue, parameter.minimum), parameter.minimum, parameter.maximum);
        parameter.value = std::clamp(
            FiniteOr(parameter.value, parameter.defaultValue), parameter.minimum, parameter.maximum);
        parameter.autoAmplitude = std::clamp(FiniteOr(parameter.autoAmplitude, 0.0f), 0.0f, MaximumMagnitude);
        parameter.autoFrequency = std::clamp(FiniteOr(parameter.autoFrequency, 0.25f), 0.0f, 60.0f);
        // 同名を探す登録済みのパラメータ
        for (auto& existing : m_parameters)
        {
            if (existing.name == parameter.name)
            {
                existing = std::move(parameter);
                return true;
            }
        }
        m_parameters.push_back(std::move(parameter));
        return true;
    }

    bool Rig2DComponent::RemoveParameter(const std::string_view name)
    {
        // 除去前の数
        const auto count = m_parameters.size();
        std::erase_if(
            m_parameters,
            // 指定名のパラメータか判定します(parameter: 判定するパラメータ)。
            [name](const Rig2DParameter& parameter)
            {
                return parameter.name == name;
            });
        return m_parameters.size() != count;
    }

    const Rig2DParameter* Rig2DComponent::FindParameter(const std::string_view name) const noexcept
    {
        // 名前を比べるパラメータ
        for (const auto& parameter : m_parameters)
        {
            if (parameter.name == name)
            {
                return &parameter;
            }
        }
        return nullptr;
    }

    bool Rig2DComponent::SetParameter(const std::string_view name, const float value) noexcept
    {
        // 名前を比べるパラメータ
        for (auto& parameter : m_parameters)
        {
            if (parameter.name == name)
            {
                parameter.value = std::clamp(
                    FiniteOr(value, parameter.value), parameter.minimum, parameter.maximum);
                return true;
            }
        }
        return false;
    }

    float Rig2DComponent::ParameterValue(const std::string_view name) const noexcept
    {
        // 指定名のパラメータ
        const auto* parameter = FindParameter(name);
        if (parameter == nullptr)
        {
            return 0.0f;
        }
        // 自動の揺れの量
        const float wave = parameter->autoAmplitude > 0.0f
            ? parameter->autoAmplitude
                * std::sin(2.0f * Pi * parameter->autoFrequency * m_time)
            : 0.0f;
        return std::clamp(parameter->value + wave, parameter->minimum, parameter->maximum);
    }

    void Rig2DComponent::ResetParameters() noexcept
    {
        // 既定値へ戻すパラメータ
        for (auto& parameter : m_parameters)
        {
            parameter.value = parameter.defaultValue;
        }
    }

    void Rig2DComponent::ApplyToHierarchy()
    {
        // 現在の値の姿勢を適用するKeyform2D
        for (auto* keyform : KeyformRegistry())
        {
            if (keyform->FindRig() == this)
            {
                keyform->ApplyPose();
            }
        }
    }

    void Rig2DComponent::RestoreHierarchyRestPose()
    {
        // 基準姿勢へ戻すKeyform2D
        for (auto* keyform : KeyformRegistry())
        {
            if (keyform->FindRig() == this)
            {
                keyform->RestoreRestPose();
            }
        }
    }

    Keyform2DComponent::Keyform2DComponent(std::vector<Keyform2DChannel> channels)
    {
        SetChannels(std::move(channels));
        KeyformRegistry().push_back(this);
    }

    Keyform2DComponent::~Keyform2DComponent()
    {
        std::erase(KeyformRegistry(), this);
    }

    void Keyform2DComponent::SetChannels(std::vector<Keyform2DChannel> channels)
    {
        m_channels.clear();
        // 登録するチャンネル
        for (auto& channel : channels)
        {
            // 登録するキー
            for (auto& key : channel.keys)
            {
                static_cast<void>(SetKey(channel.parameter, std::move(key)));
            }
        }
        RefreshUsage();
    }

    bool Keyform2DComponent::SetKey(const std::string_view parameter, Keyform2DKey key)
    {
        if (parameter.empty() || !IsUsableKey(key))
        {
            return false;
        }
        key.opacity = std::clamp(key.opacity, 0.0f, 1.0f);
        // キーを追加するチャンネル
        auto channel = std::find_if(
            m_channels.begin(),
            m_channels.end(),
            // 指定名のチャンネルか判定します(candidate: 判定するチャンネル)。
            [parameter](const Keyform2DChannel& candidate)
            {
                return candidate.parameter == parameter;
            });
        if (channel == m_channels.end())
        {
            if (m_channels.size() >= MaximumChannels)
            {
                return false;
            }
            m_channels.push_back({ std::string(parameter), {} });
            channel = std::prev(m_channels.end());
        }
        // 同じ値か挿入位置のキー
        const auto position = std::lower_bound(
            channel->keys.begin(),
            channel->keys.end(),
            key.value,
            // キーと値を比べます(existing: 比べるキー, target: 探す値)。
            [](const Keyform2DKey& existing, const float target)
            {
                return existing.value < target;
            });
        if (position != channel->keys.end() && position->value == key.value)
        {
            *position = std::move(key);
        }
        else if (channel->keys.size() >= MaximumKeysPerChannel)
        {
            return false;
        }
        else
        {
            channel->keys.insert(position, std::move(key));
        }
        RefreshUsage();
        return true;
    }

    bool Keyform2DComponent::RemoveKey(const std::string_view parameter, const float value)
    {
        // 除去したか
        bool removed{};
        // キーを探すチャンネル
        for (auto& channel : m_channels)
        {
            if (channel.parameter != parameter)
            {
                continue;
            }
            // 除去前のキー数
            const auto count = channel.keys.size();
            std::erase_if(
                channel.keys,
                // 指定値のキーか判定します(key: 判定するキー)。
                [value](const Keyform2DKey& key)
                {
                    return key.value == value;
                });
            removed = removed || channel.keys.size() != count;
        }
        std::erase_if(
            m_channels,
            // キーのないチャンネルか判定します(channel: 判定するチャンネル)。
            [](const Keyform2DChannel& channel)
            {
                return channel.keys.empty();
            });
        RefreshUsage();
        return removed;
    }

    void Keyform2DComponent::CaptureRestPose()
    {
        // 基準にする現在のローカル変換
        const auto& transform = Owner().GetTransform();
        m_restPosition = transform.position;
        m_restEuler = transform.rotation;
        m_restScale = transform.scale;
        // 不透明度を持つSprite Renderer
        const auto* sprite = Owner().GetComponent<SpriteRendererComponent>();
        m_restOpacity = sprite != nullptr ? sprite->Color().w : 1.0f;
        m_hasRestPose = true;
    }

    void Keyform2DComponent::SetRestPose(
        const DirectX::XMFLOAT3& position,
        const DirectX::XMFLOAT4& rotation,
        const DirectX::XMFLOAT3& scale,
        const float opacity) noexcept
    {
        m_restPosition = position;
        m_restEuler = QuaternionToEuler(rotation);
        m_restScale = scale;
        m_restOpacity = std::isfinite(opacity) ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
        m_hasRestPose = true;
    }

    DirectX::XMFLOAT4 Keyform2DComponent::RestRotation() const noexcept
    {
        return EulerToQuaternion(m_restEuler);
    }

    bool Keyform2DComponent::RecordPoseKey(const std::string_view parameter, const float value)
    {
        if (!m_hasRestPose || !IsUsable(value))
        {
            return false;
        }
        // 記録する現在のローカル変換
        const auto& transform = Owner().GetTransform();
        // 記録するキー
        Keyform2DKey key;
        key.value = value;
        key.positionOffset = {
            transform.position.x - m_restPosition.x,
            transform.position.y - m_restPosition.y };
        key.rotationDegrees = WrapDegrees(
            (transform.rotation.z - m_restEuler.z) * (180.0f / Pi));
        key.scale = {
            std::abs(m_restScale.x) > 1.0e-6f ? transform.scale.x / m_restScale.x : 1.0f,
            std::abs(m_restScale.y) > 1.0e-6f ? transform.scale.y / m_restScale.y : 1.0f };
        // 不透明度を持つSprite Renderer
        const auto* sprite = Owner().GetComponent<SpriteRendererComponent>();
        key.opacity = sprite != nullptr && m_restOpacity > 1.0e-6f
            ? std::clamp(sprite->Color().w / m_restOpacity, 0.0f, 1.0f)
            : 1.0f;
        // 頂点移動を引き継ぐ既存のキーを探すチャンネル
        for (const auto& channel : m_channels)
        {
            if (channel.parameter != parameter)
            {
                continue;
            }
            // 同じ値か比べるキー
            for (const auto& existing : channel.keys)
            {
                if (existing.value == value)
                {
                    key.vertexOffsets = existing.vertexOffsets;
                }
            }
        }
        return SetKey(parameter, std::move(key));
    }

    bool Keyform2DComponent::RecordMeshKey(const std::string_view parameter, const float value)
    {
        // 格子を持つSprite Renderer
        const auto* sprite = Owner().GetComponent<SpriteRendererComponent>();
        if (sprite == nullptr || !IsUsable(value))
        {
            return false;
        }
        // 変形の起点にする頂点
        const auto base = sprite->MeshDeformation().empty()
            ? sprite->MeshRestPositions()
            : sprite->MeshDeformation();
        // この部品以外を適用した頂点
        const auto deformed = sprite->DeformedMeshPositions(this);
        if (deformed.size() != base.size())
        {
            return false;
        }
        // 記録するキー
        Keyform2DKey key;
        key.value = value;
        // 姿勢を引き継ぐ既存のキーを探すチャンネル
        for (const auto& channel : m_channels)
        {
            if (channel.parameter != parameter)
            {
                continue;
            }
            // 同じ値か比べるキー
            for (const auto& existing : channel.keys)
            {
                if (existing.value == value)
                {
                    key = existing;
                }
            }
        }
        key.vertexOffsets.resize(base.size());
        // 移動量を求める頂点番号
        for (std::size_t index = 0; index < base.size(); ++index)
        {
            key.vertexOffsets[index] = Subtract(deformed[index], base[index]);
        }
        return SetKey(parameter, std::move(key));
    }

    Keyform2DPose Keyform2DComponent::EvaluatePose() const
    {
        // 合成した差
        Keyform2DPose pose;
        // 値を読むリグ
        const auto* rig = FindRig();
        if (rig == nullptr)
        {
            return pose;
        }
        // 合成するチャンネル
        for (const auto& channel : m_channels)
        {
            if (channel.keys.empty() || rig->FindParameter(channel.parameter) == nullptr)
            {
                continue;
            }
            // 現在の値を挟むキー
            const auto span = Locate(channel.keys, rig->ParameterValue(channel.parameter));
            pose.positionOffset.x += Lerp(span.lower->positionOffset.x, span.upper->positionOffset.x, span.amount);
            pose.positionOffset.y += Lerp(span.lower->positionOffset.y, span.upper->positionOffset.y, span.amount);
            pose.rotationDegrees += Lerp(span.lower->rotationDegrees, span.upper->rotationDegrees, span.amount);
            pose.scale.x *= Lerp(span.lower->scale.x, span.upper->scale.x, span.amount);
            pose.scale.y *= Lerp(span.lower->scale.y, span.upper->scale.y, span.amount);
            pose.opacity *= Lerp(span.lower->opacity, span.upper->opacity, span.amount);
        }
        pose.opacity = std::clamp(pose.opacity, 0.0f, 1.0f);
        return pose;
    }

    void Keyform2DComponent::ApplyPose()
    {
        if (!m_hasRestPose)
        {
            return;
        }
        // 基準に足す差
        const auto pose = EvaluatePose();
        if (m_usesTransform)
        {
            // 書き換えるローカル変換
            auto& transform = Owner().GetTransform();
            transform.position = {
                m_restPosition.x + pose.positionOffset.x,
                m_restPosition.y + pose.positionOffset.y,
                m_restPosition.z };
            transform.rotation = {
                m_restEuler.x,
                m_restEuler.y,
                m_restEuler.z + ToRadians(pose.rotationDegrees) };
            transform.scale = {
                m_restScale.x * pose.scale.x,
                m_restScale.y * pose.scale.y,
                m_restScale.z };
        }
        if (m_usesOpacity)
        {
            // 不透明度を書き換えるSprite Renderer
            if (auto* sprite = Owner().GetComponent<SpriteRendererComponent>())
            {
                // 不透明度だけを変えた描画色
                auto color = sprite->Color();
                color.w = m_restOpacity * pose.opacity;
                sprite->SetColor(color);
            }
        }
    }

    void Keyform2DComponent::RestoreRestPose()
    {
        if (!m_hasRestPose)
        {
            return;
        }
        if (m_usesTransform)
        {
            // 戻すローカル変換
            auto& transform = Owner().GetTransform();
            transform.position = m_restPosition;
            transform.rotation = m_restEuler;
            transform.scale = m_restScale;
        }
        if (m_usesOpacity)
        {
            // 不透明度を戻すSprite Renderer
            if (auto* sprite = Owner().GetComponent<SpriteRendererComponent>())
            {
                // 不透明度だけを戻した描画色
                auto color = sprite->Color();
                color.w = m_restOpacity;
                sprite->SetColor(color);
            }
        }
    }

    void Keyform2DComponent::DeformSpriteMesh(
        const SpriteRendererComponent&,
        std::vector<DirectX::XMFLOAT2>& positions)
    {
        // 値を読むリグ
        const auto* rig = FindRig();
        if (rig == nullptr)
        {
            return;
        }
        // 足し合わせるチャンネル
        for (const auto& channel : m_channels)
        {
            if (channel.keys.empty() || rig->FindParameter(channel.parameter) == nullptr)
            {
                continue;
            }
            // 現在の値を挟むキー
            const auto span = Locate(channel.keys, rig->ParameterValue(channel.parameter));
            // 下側のキーの頂点移動を使えるか
            const bool lowerUsable = span.lower->vertexOffsets.size() == positions.size();
            // 上側のキーの頂点移動を使えるか
            const bool upperUsable = span.upper->vertexOffsets.size() == positions.size();
            if (!lowerUsable && !upperUsable)
            {
                continue;
            }
            // 移動する頂点番号
            for (std::size_t index = 0; index < positions.size(); ++index)
            {
                // 下側のキーの移動量
                const DirectX::XMFLOAT2 lower =
                    lowerUsable ? span.lower->vertexOffsets[index] : DirectX::XMFLOAT2{};
                // 上側のキーの移動量
                const DirectX::XMFLOAT2 upper =
                    upperUsable ? span.upper->vertexOffsets[index] : DirectX::XMFLOAT2{};
                positions[index] = Add(positions[index], Lerp2(lower, upper, span.amount));
            }
        }
    }

    bool Keyform2DComponent::DeformsSpriteMesh() const
    {
        // 頂点移動を探すチャンネル
        for (const auto& channel : m_channels)
        {
            // 頂点移動を探すキー
            for (const auto& key : channel.keys)
            {
                if (!key.vertexOffsets.empty())
                {
                    return true;
                }
            }
        }
        return false;
    }

    Rig2DComponent* Keyform2DComponent::FindRig() const
    {
        // 自身から祖先へたどる物体
        for (auto* current = &Owner(); current != nullptr; current = current->Parent())
        {
            // 物体のリグ
            if (auto* rig = current->GetComponent<Rig2DComponent>())
            {
                return rig;
            }
        }
        return nullptr;
    }

    void Keyform2DComponent::RefreshUsage() noexcept
    {
        m_usesTransform = false;
        m_usesOpacity = false;
        // 確認するチャンネル
        for (const auto& channel : m_channels)
        {
            // 確認するキー
            for (const auto& key : channel.keys)
            {
                m_usesTransform = m_usesTransform
                    || key.positionOffset.x != 0.0f
                    || key.positionOffset.y != 0.0f
                    || key.rotationDegrees != 0.0f
                    || key.scale.x != 1.0f
                    || key.scale.y != 1.0f;
                m_usesOpacity = m_usesOpacity || key.opacity != 1.0f;
            }
        }
    }

    bool CharacterRig2DRuntime::LoadComponent(
        GameObject& object,
        const std::string& type,
        const Json& component)
    {
        // 復元した部品
        Component* loaded{};
        if (type == "Sway2D")
        {
            // 省略された項目を既定値で埋める揺れ設定
            Sway2DSettings settings{};
            settings.tipOffset = ReadFloat2(component.value("tipOffset", Json::array()), settings.tipOffset);
            settings.stiffness = component.value("stiffness", settings.stiffness);
            settings.damping = component.value("damping", settings.damping);
            settings.inertia = component.value("inertia", settings.inertia);
            settings.gravity = ReadFloat2(component.value("gravity", Json::array()), settings.gravity);
            settings.maxAngleDegrees = component.value("maxAngle", settings.maxAngleDegrees);
            settings.windAmplitudeDegrees = component.value("windAmplitude", settings.windAmplitudeDegrees);
            settings.windFrequency = component.value("windFrequency", settings.windFrequency);
            settings.windPhaseDegrees = component.value("windPhase", settings.windPhaseDegrees);
            loaded = &object.AddComponent<Sway2DComponent>(settings);
        }
        else if (type == "Blink2D")
        {
            // 省略された項目を既定値で埋める瞬き設定
            Blink2DSettings settings{};
            settings.columns = component.value("columns", settings.columns);
            settings.rows = component.value("rows", settings.rows);
            settings.openFrame = component.value("openFrame", settings.openFrame);
            settings.closingStartFrame = component.value("closingStartFrame", settings.closingStartFrame);
            settings.closingFrameCount = component.value("closingFrameCount", settings.closingFrameCount);
            settings.frameSeconds = component.value("frameSeconds", settings.frameSeconds);
            settings.closedSeconds = component.value("closedSeconds", settings.closedSeconds);
            settings.intervalMinSeconds = component.value("intervalMin", settings.intervalMinSeconds);
            settings.intervalMaxSeconds = component.value("intervalMax", settings.intervalMaxSeconds);
            settings.doubleBlinkChance = component.value("doubleBlinkChance", settings.doubleBlinkChance);
            settings.autoBlink = component.value("autoBlink", settings.autoBlink);
            settings.includeChildren = component.value("includeChildren", settings.includeChildren);
            loaded = &object.AddComponent<Blink2DComponent>(settings);
        }
        else if (type == "SpriteSkin2D")
        {
            // 保存時のIDで書かれたボーン一覧
            std::vector<std::uint64_t> bones;
            // 読み込むボーンID
            for (const auto& bone : component.value("bones", Json::array()))
            {
                bones.push_back(bone.get<std::uint64_t>());
            }
            // 復元した2Dスキン
            auto& skin = object.AddComponent<SpriteSkin2DComponent>(std::move(bones));
            skin.SetWeightFalloff(component.value("weightFalloff", SpriteSkin2DComponent::DefaultWeightFalloff));
            if (component.value("bound", false))
            {
                // 復元するボーンのバインド姿勢
                std::vector<DirectX::XMFLOAT4X4> bonePoses;
                // 読み込むボーンの姿勢
                for (const auto& pose : component.value("boneBindPoses", Json::array()))
                {
                    bonePoses.push_back(ReadMatrix(pose));
                }
                // 復元する頂点ごとの重み
                std::vector<SpriteSkinWeight> weights;
                // 読み込む頂点の重み
                for (const auto& entry : component.value("weights", Json::array()))
                {
                    // 復元する1頂点の重み
                    SpriteSkinWeight weight{};
                    if (entry.is_array() && entry.size() == 8)
                    {
                        // 読み込む影響の番号
                        for (std::size_t index = 0; index < 4; ++index)
                        {
                            weight.bones[index] = entry.at(index).get<std::uint16_t>();
                            weight.weights[index] = entry.at(index + 4).get<float>();
                        }
                    }
                    weights.push_back(weight);
                }
                static_cast<void>(skin.RestoreBinding(
                    std::move(bonePoses),
                    ReadMatrix(component.value("spriteBindPose", Json::array())),
                    std::move(weights),
                    component.value("boundColumns", 1),
                    component.value("boundRows", 1)));
            }
            loaded = &skin;
        }
        else if (type == "Rig2D")
        {
            // 復元するパラメータ
            std::vector<Rig2DParameter> parameters;
            // 読み込むパラメータ
            for (const auto& entry : component.value("parameters", Json::array()))
            {
                // 省略された項目を既定値で埋めるパラメータ
                Rig2DParameter parameter{};
                parameter.name = entry.value("name", std::string{});
                parameter.minimum = entry.value("minimum", parameter.minimum);
                parameter.maximum = entry.value("maximum", parameter.maximum);
                parameter.defaultValue = entry.value("defaultValue", parameter.defaultValue);
                parameter.value = entry.value("value", parameter.defaultValue);
                parameter.autoAmplitude = entry.value("autoAmplitude", parameter.autoAmplitude);
                parameter.autoFrequency = entry.value("autoFrequency", parameter.autoFrequency);
                parameters.push_back(std::move(parameter));
            }
            loaded = &object.AddComponent<Rig2DComponent>(std::move(parameters));
        }
        else if (type == "Keyform2D")
        {
            // 復元するチャンネル
            std::vector<Keyform2DChannel> channels;
            // 読み込むチャンネル
            for (const auto& entry : component.value("channels", Json::array()))
            {
                // 復元する1チャンネル
                Keyform2DChannel channel;
                channel.parameter = entry.value("parameter", std::string{});
                // 読み込むキー
                for (const auto& serializedKey : entry.value("keys", Json::array()))
                {
                    // 省略された項目を既定値で埋めるキー
                    Keyform2DKey key{};
                    key.value = serializedKey.value("value", 0.0f);
                    key.positionOffset = ReadFloat2(serializedKey.value("position", Json::array()), {});
                    key.rotationDegrees = serializedKey.value("rotation", 0.0f);
                    key.scale = ReadFloat2(serializedKey.value("scale", Json::array()), { 1.0f, 1.0f });
                    key.opacity = serializedKey.value("opacity", 1.0f);
                    // 読み込む頂点移動
                    for (const auto& offset : serializedKey.value("vertices", Json::array()))
                    {
                        key.vertexOffsets.push_back(ReadFloat2(offset, {}));
                    }
                    channel.keys.push_back(std::move(key));
                }
                channels.push_back(std::move(channel));
            }
            // 復元した2Dキーフォーム
            auto& keyform = object.AddComponent<Keyform2DComponent>(std::move(channels));
            if (const auto rest = component.find("rest"); rest != component.end() && rest->is_object())
            {
                keyform.SetRestPose(
                    ReadFloat3(rest->value("position", Json::array()), {}),
                    ReadFloat4(rest->value("rotation", Json::array()), { 0.0f, 0.0f, 0.0f, 1.0f }),
                    ReadFloat3(rest->value("scale", Json::array()), { 1.0f, 1.0f, 1.0f }),
                    rest->value("opacity", 1.0f));
            }
            loaded = &keyform;
        }
        if (loaded == nullptr)
        {
            return false;
        }
        loaded->SetEnabled(component.value("enabled", true));
        return true;
    }

    void CharacterRig2DRuntime::LoadSpriteMesh(
        SpriteRendererComponent& sprite,
        const Json& component)
    {
        sprite.SetMeshGrid(component.value("meshColumns", 1), component.value("meshRows", 1));
    }

    void CharacterRig2DRuntime::ResolveReferences(
        const std::unordered_map<std::int64_t, GameObject*>& bySourceId)
    {
        // スクリプトが動く前の読み込み直後の姿勢を、スキンとキーフォームの基準にします。
        // 参照を置き換える物体
        for (const auto& [sourceId, object] : bySourceId)
        {
            static_cast<void>(sourceId);
            if (object == nullptr)
            {
                continue;
            }
            // 基準姿勢が保存されていないキーフォーム
            if (auto* keyform = object->GetComponent<Keyform2DComponent>();
                keyform != nullptr && !keyform->m_hasRestPose)
            {
                keyform->CaptureRestPose();
            }
            // 物体の2Dスキン
            auto* skin = object->GetComponent<SpriteSkin2DComponent>();
            if (skin == nullptr)
            {
                continue;
            }
            // 生成後のIDへ置き換えたボーン一覧
            auto bones = skin->Bones();
            // 見つけたボーン
            std::vector<GameObject*> boneObjects;
            // 置き換えるボーンID
            for (auto& bone : bones)
            {
                // 保存時のIDに対応する物体の位置
                const auto found = bySourceId.find(static_cast<std::int64_t>(bone));
                // 対応する物体
                auto* boneObject = found != bySourceId.end() ? found->second : nullptr;
                bone = boneObject != nullptr ? boneObject->Id() : 0;
                boneObjects.push_back(boneObject);
            }
            static_cast<void>(skin->RemapBones(std::move(bones)));
            skin->m_boneObjects = std::move(boneObjects);
            if (!skin->m_bound && !skin->m_bones.empty())
            {
                static_cast<void>(skin->Bind());
            }
        }
    }

    void CharacterRig2DRuntime::Update(
        const std::vector<std::unique_ptr<GameObject>>& objects,
        const float deltaTime)
    {
        // 有限で0以上へ収めた経過秒数
        const float frameSeconds =
            std::isfinite(deltaTime) ? std::max(deltaTime, 0.0f) : 0.0f;
        // IDから物体を引く表
        std::unordered_map<std::uint64_t, GameObject*> byId;
        // 表へ登録する物体
        for (const auto& object : objects)
        {
            byId[object->Id()] = object.get();
        }

        // ボーンを探し、未バインドのスキンを現在の姿勢でバインドします。
        // スキンを探す物体
        for (const auto& object : objects)
        {
            // 物体の2Dスキン
            auto* skin = object->GetComponent<SpriteSkin2DComponent>();
            if (skin == nullptr)
            {
                continue;
            }
            skin->m_boneObjects.resize(skin->m_bones.size());
            // 探すボーンの番号
            for (std::size_t index = 0; index < skin->m_bones.size(); ++index)
            {
                // IDに対応する物体の位置
                const auto found = byId.find(skin->m_bones[index]);
                skin->m_boneObjects[index] = found != byId.end() ? found->second : nullptr;
            }
            if (!skin->m_bound && !skin->m_bones.empty())
            {
                static_cast<void>(skin->Bind());
            }
        }

        // リグの時刻とキーフォームの姿勢を進めます。
        // リグとキーフォームを探す物体
        for (const auto& object : objects)
        {
            if (!object->IsEnabled())
            {
                continue;
            }
            // 物体のリグ
            if (auto* rig = object->GetComponent<Rig2DComponent>();
                rig != nullptr && rig->IsEnabled() && frameSeconds > 0.0f)
            {
                rig->m_time = std::fmod(rig->m_time + frameSeconds, AutoTimeWrapSeconds);
            }
        }
        // 姿勢を適用する物体
        for (const auto& object : objects)
        {
            // 物体のキーフォーム
            auto* keyform = object->GetComponent<Keyform2DComponent>();
            if (keyform == nullptr || !object->IsEnabled() || !keyform->IsEnabled())
            {
                continue;
            }
            if (!keyform->m_hasRestPose)
            {
                keyform->CaptureRestPose();
            }
            keyform->ApplyPose();
        }

        // 瞬きを進めます。
        // 瞬きを探す物体
        for (const auto& object : objects)
        {
            // 物体の瞬き
            auto* blink = object->GetComponent<Blink2DComponent>();
            if (blink == nullptr || !object->IsEnabled() || !blink->IsEnabled())
            {
                continue;
            }
            if (!blink->m_ready)
            {
                if (!blink->m_seeded)
                {
                    blink->m_random = static_cast<std::uint32_t>(
                        ((object->Id() * 2654435761ull) ^ 0x5bd1e995ull) % RandomModulus);
                    if (blink->m_random == 0)
                    {
                        blink->m_random = 1;
                    }
                }
                blink->ScheduleNextBlink(false);
                blink->m_ready = true;
            }
            blink->Advance(std::min(frameSeconds, MaximumBlinkFrameSeconds));
        }
        // 最も近い祖先の瞬きのコマへ切り替えるSprite
        for (const auto& object : objects)
        {
            // コマを切り替えるSprite Renderer
            auto* sprite = object->GetComponent<SpriteRendererComponent>();
            if (sprite == nullptr)
            {
                continue;
            }
            // 自身から祖先へたどる物体
            for (auto* current = object.get(); current != nullptr; current = current->Parent())
            {
                // 物体の瞬き
                const auto* blink = current->GetComponent<Blink2DComponent>();
                if (blink == nullptr)
                {
                    continue;
                }
                if (blink->IsEnabled()
                    && blink->m_ready
                    && (current == object.get() || blink->m_settings.includeChildren))
                {
                    // シートのコマ数
                    const int totalFrames = blink->m_settings.columns * blink->m_settings.rows;
                    // シート周回後の表示コマ番号
                    const int frame = blink->CurrentFrame() % totalFrames;
                    // 正規化したコマの幅
                    const float cellWidth = 1.0f / static_cast<float>(blink->m_settings.columns);
                    // 正規化したコマの高さ
                    const float cellHeight = 1.0f / static_cast<float>(blink->m_settings.rows);
                    sprite->SetSourceRect({
                        static_cast<float>(frame % blink->m_settings.columns) * cellWidth,
                        static_cast<float>(frame / blink->m_settings.columns) * cellHeight,
                        cellWidth,
                        cellHeight });
                }
                break;
            }
        }

        // 揺れ物は祖先から順に解き、子の静止姿勢へ親の揺れを含めます。
        // 準備する物体
        for (const auto& object : objects)
        {
            // 物体の揺れ物
            auto* sway = object->GetComponent<Sway2DComponent>();
            if (sway == nullptr)
            {
                continue;
            }
            if (!object->IsEnabled() || !sway->IsEnabled())
            {
                // 無効になった揺れ物は足した回転を外します。
                if (sway->m_rotationApplied
                    && object->GetTransform().rotation.z == sway->m_appliedRotation)
                {
                    object->GetTransform().rotation.z = sway->m_restRotation;
                }
                sway->m_rotationApplied = false;
                sway->ResetSimulation();
                sway->m_solvedThisFrame = true;
                continue;
            }
            sway->m_pendingDeltaTime = std::min(frameSeconds, MaximumSwayFrameSeconds);
            sway->m_solvedThisFrame = false;
        }
        // 解く物体
        for (const auto& object : objects)
        {
            // 物体の揺れ物
            if (auto* sway = object->GetComponent<Sway2DComponent>())
            {
                SolveSway(*sway);
            }
        }
    }

    void CharacterRig2DRuntime::SolveSway(Sway2DComponent& sway)
    {
        if (sway.m_solvedThisFrame)
        {
            return;
        }
        sway.m_solvedThisFrame = true;
        // 祖先をたどる途中の物体
        for (auto* ancestor = sway.Owner().Parent(); ancestor != nullptr; ancestor = ancestor->Parent())
        {
            // 祖先が持つ揺れ物
            auto* ancestorSway = ancestor->GetComponent<Sway2DComponent>();
            if (ancestorSway != nullptr && !ancestorSway->m_solvedThisFrame)
            {
                SolveSway(*ancestorSway);
                break;
            }
        }

        // 揺れを足す前のローカル変換
        auto& transform = sway.Owner().GetTransform();
        if (!sway.m_rotationApplied || transform.rotation.z != sway.m_appliedRotation)
        {
            sway.m_restRotation = transform.rotation.z;
        }
        transform.rotation.z = sway.m_restRotation;
        // 今フレームで進める秒数
        const float deltaTime = std::exchange(sway.m_pendingDeltaTime, 0.0f);
        // 静止姿勢のワールド変換
        const auto world = WorldAffine(sway.Owner());
        // 回転中心のワールドXY
        const DirectX::XMFLOAT2 pivot{ world.tx, world.ty };
        // 先端の静止位置のワールドXY
        const auto restTip = Apply(world, sway.m_settings.tipOffset);
        // 回転中心から静止先端までのワールド長
        const float length = Length(Subtract(restTip, pivot));
        if (!std::isfinite(length) || length < MinimumLength)
        {
            sway.ResetSimulation();
            sway.m_rotationApplied = false;
            return;
        }
        if (!sway.m_simulationReady
            || Length(Subtract(pivot, sway.m_previousPivot)) > length * TeleportLengthRatio)
        {
            sway.m_tip = restTip;
            sway.m_velocity = {};
            sway.m_previousPivot = pivot;
            sway.m_previousRestTip = restTip;
            sway.m_simulationReady = true;
        }
        if (deltaTime > 0.0f)
        {
            SimulateSway(sway, pivot, restTip, deltaTime);
            sway.m_windPhase = std::fmod(
                sway.m_windPhase + 2.0f * Pi * sway.m_settings.windFrequency * deltaTime,
                2.0f * Pi);
        }
        sway.m_previousPivot = pivot;
        sway.m_previousRestTip = restTip;

        // 親を基準にした回転中心
        const DirectX::XMFLOAT2 localPivot{ transform.position.x, transform.position.y };
        // 親を基準にした静止先端
        const auto localRestTip = Apply(FromMat4(LocalMatrix(transform)), sway.m_settings.tipOffset);
        // ワールドの先端を親基準へ戻す変換
        Affine2D worldToParent{};
        if (const auto* parent = sway.Owner().Parent())
        {
            // 親のワールド変換
            const auto parentWorld = WorldAffine(*parent);
            if (!IsInvertible(parentWorld))
            {
                sway.ResetSimulation();
                sway.m_rotationApplied = false;
                return;
            }
            worldToParent = Inverse(parentWorld);
        }
        // 親を基準にした先端の点
        const auto localTip = Apply(worldToParent, sway.m_tip);
        // 物理と風を合わせた回転角
        float angle = SignedAngle(
            Subtract(localRestTip, localPivot),
            Subtract(localTip, localPivot));
        angle += ToRadians(sway.m_settings.windAmplitudeDegrees)
            * std::sin(sway.m_windPhase + ToRadians(sway.m_settings.windPhaseDegrees));
        // 振れ角の上限ラジアン
        const float maximumAngle = ToRadians(sway.m_settings.maxAngleDegrees);
        sway.m_angle = std::isfinite(angle) ? std::clamp(angle, -maximumAngle, maximumAngle) : 0.0f;
        transform.rotation.z = sway.m_restRotation + sway.m_angle;
        sway.m_appliedRotation = transform.rotation.z;
        sway.m_rotationApplied = true;
    }

    void CharacterRig2DRuntime::SimulateSway(
        Sway2DComponent& sway,
        const DirectX::XMFLOAT2 pivot,
        const DirectX::XMFLOAT2 restTip,
        const float deltaTime)
    {
        // 揺れ設定
        const auto& settings = sway.m_settings;
        // 回転中心から静止先端までの長さ
        const float length = Length(Subtract(restTip, pivot));
        // 振れ角の上限ラジアン
        const float maximumAngle = ToRadians(settings.maxAngleDegrees);
        // フレーム内の積分回数
        const int substeps = std::clamp(
            static_cast<int>(std::ceil(deltaTime / MaximumSubstepSeconds)),
            1,
            MaximumSubsteps);
        // 1回の積分秒数
        const float step = deltaTime / static_cast<float>(substeps);
        // 直前の積分時点の回転中心
        auto stepPivot = sway.m_previousPivot;
        // 積分回数の添字
        for (int index = 1; index <= substeps; ++index)
        {
            // 前フレームから今フレームへの補間率
            const float amount = static_cast<float>(index) / static_cast<float>(substeps);
            // 今回の回転中心
            const auto currentPivot = Lerp2(sway.m_previousPivot, pivot, amount);
            // 今回の静止先端
            const auto currentRestTip = Lerp2(sway.m_previousRestTip, restTip, amount);
            // 今回の回転中心の移動量
            const auto pivotDelta = Subtract(currentPivot, stepPivot);
            stepPivot = currentPivot;
            sway.m_tip = Add(sway.m_tip, Scale(pivotDelta, 1.0f - settings.inertia));
            // 積分前の先端位置
            const auto previousTip = sway.m_tip;
            // 回転中心に対する先端の相対速度
            const auto relativeVelocity = Subtract(
                sway.m_velocity,
                Scale(pivotDelta, settings.inertia / step));
            // ばね・重力・減衰による加速度
            const DirectX::XMFLOAT2 acceleration{
                settings.stiffness * (currentRestTip.x - sway.m_tip.x)
                    + settings.gravity.x - settings.damping * relativeVelocity.x,
                settings.stiffness * (currentRestTip.y - sway.m_tip.y)
                    + settings.gravity.y - settings.damping * relativeVelocity.y };
            sway.m_velocity = Add(sway.m_velocity, Scale(acceleration, step));
            sway.m_tip = Add(sway.m_tip, Scale(sway.m_velocity, step));
            // 回転中心から静止先端への方向
            auto restDirection = Subtract(currentRestTip, currentPivot);
            // 補間中の静止方向の長さ
            const float restLength = Length(restDirection);
            if (restLength < MinimumLength)
            {
                continue;
            }
            restDirection = Scale(restDirection, length / restLength);
            // 回転中心から先端への方向
            const auto tipDirection = Subtract(sway.m_tip, currentPivot);
            // 静止方向から先端方向への角度
            const float angle = std::clamp(
                Length(tipDirection) < MinimumLength ? 0.0f : SignedAngle(restDirection, tipDirection),
                -maximumAngle,
                maximumAngle);
            sway.m_tip = Add(currentPivot, Rotate(restDirection, angle));
            sway.m_velocity = Scale(Subtract(sway.m_tip, previousTip), 1.0f / step);
        }
        if (!std::isfinite(sway.m_tip.x) || !std::isfinite(sway.m_tip.y)
            || !std::isfinite(sway.m_velocity.x) || !std::isfinite(sway.m_velocity.y))
        {
            sway.m_tip = restTip;
            sway.m_velocity = {};
        }
    }

    bool CharacterRig2DRuntime::RenderSpriteMesh(
        GameObject& object,
        SpriteRendererComponent& sprite,
        const Web::Mat4& model,
        const std::string& texturePath)
    {
        if (!sprite.UsesMesh())
        {
            return false;
        }
        // 描画に使う格子頂点のローカル位置
        const auto local = sprite.DeformedMeshPositions();
        // 画面へ写すワールド変換
        const auto world = FromMat4(model);
        // 画像の部分領域
        const auto uvRect = texturePath.empty()
            ? DirectX::XMFLOAT4{ 0.0f, 0.0f, 1.0f, 1.0f }
            : sprite.m_sourceRect;
        // 画面XY列
        std::vector<float> positions;
        // UV列
        std::vector<float> uvs;
        positions.reserve(local.size() * 2);
        uvs.reserve(local.size() * 2);
        // 変換する頂点番号
        for (std::size_t index = 0; index < local.size(); ++index)
        {
            // 格子内の列番号
            const int column = static_cast<int>(index) % (sprite.m_meshColumns + 1);
            // 格子内の行番号
            const int row = static_cast<int>(index) / (sprite.m_meshColumns + 1);
            // 画面上の頂点
            const auto screen = Apply(world, local[index]);
            positions.push_back(screen.x);
            positions.push_back(screen.y);
            uvs.push_back(uvRect.x + uvRect.z * static_cast<float>(column) / static_cast<float>(sprite.m_meshColumns));
            uvs.push_back(uvRect.y + uvRect.w * static_cast<float>(row) / static_cast<float>(sprite.m_meshRows));
        }
        // 格子の三角形の頂点番号
        std::vector<std::uint16_t> indices;
        BuildGridIndices(sprite.m_meshColumns, sprite.m_meshRows, indices);
        RenderPortableSpriteMesh(
            object.Name().c_str(),
            static_cast<double>(object.Id()),
            texturePath.c_str(),
            sprite.m_color.x,
            sprite.m_color.y,
            sprite.m_color.z,
            sprite.m_color.w,
            sprite.m_sortOrder,
            positions.data(),
            uvs.data(),
            static_cast<int>(local.size()),
            indices.data(),
            static_cast<int>(indices.size()));
        return true;
    }
}
