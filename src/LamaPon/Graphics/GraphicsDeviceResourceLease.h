#pragma once

#include <memory>

namespace LamaPon
{
    namespace Detail
    {
        struct GraphicsDeviceResourceLeaseState;
    }

    class GraphicsDevice;

    // 外部が描画資源を保持する期間を数え、保持中はデバイス再初期化を拒否する。
    // 資源自体は所有せず共有ゲートを保持するため、デバイスが先に破棄されても安全に解放できる。
    class GraphicsDeviceResourceLease final
    {
    public:
        // ゲートを持たない空のリースを作成する。
        GraphicsDeviceResourceLease() noexcept = default;
        // 共有ゲートの保持と有効リース数を解除する。
        ~GraphicsDeviceResourceLease() noexcept;

        // リース数を増やさず保持を移し、元を空にする(other: 移動元のリース)。
        GraphicsDeviceResourceLease(
            GraphicsDeviceResourceLease&& other) noexcept;
        // 現在のリースを解放して保持を移す(other: 移動元のリース)。
        GraphicsDeviceResourceLease& operator=(
            GraphicsDeviceResourceLease&& other) noexcept;

        // リースのコピーを禁止する。
        GraphicsDeviceResourceLease(
            const GraphicsDeviceResourceLease&) = delete;
        // リースのコピー代入を禁止する。
        GraphicsDeviceResourceLease& operator=(
            const GraphicsDeviceResourceLease&) = delete;

        // 共有ゲートを保持しているか判定する。
        // デバイスの生存や再利用可能性は判定しない。
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_state != nullptr;
        }

        // 共有参照を外してリース数を減らし、繰返し呼出は無処理とする。
        void Reset() noexcept;

    private:
        // 加算済みの共有ゲートを引き受ける(state: 取得済みリースのゲート)。
        explicit GraphicsDeviceResourceLease(
            std::shared_ptr<
                Detail::GraphicsDeviceResourceLeaseState> state) noexcept;

        // デバイス再初期化を防ぐ共有ゲート
        std::shared_ptr<
            Detail::GraphicsDeviceResourceLeaseState> m_state;

        friend class GraphicsDevice;
    };
}
