#pragma once

#include "LamaPon/Online/NetworkSession.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <string_view>

namespace LamaPon::Samples
{
    // ホストと最初の参加者が対戦し、それ以外は観戦する小さな盤面の例です。
    // Scene・Prefab・常時Transform送信は不要です。管理Scriptがイベントを一度だけ消費します。
    class P2PTurnBasedController final
    {
    public:
        void Update(NetworkSession& session)
        {
            if (m_generation != session.Generation())
            {
                m_generation = session.Generation(); Reset(); m_second = 0;
            }
            if (!session.IsHost() && session.State() != NetworkState::Connected) return;
            if (session.IsHost())
            {
                NetworkPeerId second{};
                for (const auto& member : session.Members()) if (member.id != 1) { second = member.id; break; }
                if (second != m_second) { m_second = second; Reset(); }
            }
            else Read(session.SessionState());
            NetworkEvent event;
            while (session.PollEvent(event))
            {
                if (!session.IsHost() || event.kind != NetworkEventKind::Command || event.name != "board.move"
                    || m_second == 0 || event.peer != m_turn || m_winner != 0) continue;
                const auto colon = event.data.find(':');
                if (colon == std::string::npos || colon + 2 != event.data.size()) continue;
                std::uint32_t revision{};
                const auto parsed = std::from_chars(event.data.data(), event.data.data() + colon, revision);
                const char cell = event.data.back();
                if (parsed.ec != std::errc{} || parsed.ptr != event.data.data() + colon || revision != m_revision
                    || cell < '0' || cell > '8' || m_board[static_cast<std::size_t>(cell - '0')] != '.') continue;
                m_board[static_cast<std::size_t>(cell - '0')] = event.peer == 1 ? 'X' : 'O';
                ++m_revision; Result();
                m_turn = m_winner == 0 ? (m_turn == 1 ? m_second : 1) : 0;
            }
            if (session.IsHost())
            {
                const std::string state = "T1|" + std::string(m_board.data(), m_board.size()) + "|"
                    + std::to_string(m_second) + "|" + std::to_string(m_turn) + "|"
                    + std::to_string(m_winner) + "|" + std::to_string(m_revision);
                if (!session.SetSessionState(state)) session.Abort("ターン状態を送信できません。");
            }
        }
        bool RequestMove(NetworkSession& session, const std::size_t cell) const
        {
            return cell < 9 && m_board[cell] == '.' && m_second != 0 && m_winner == 0 && session.LocalPeer() == m_turn
                && session.SendCommand("board.move", std::to_string(m_revision) + ":" + std::to_string(cell));
        }
        const std::array<char, 9>& Board() const noexcept { return m_board; }
        NetworkPeerId Turn() const noexcept { return m_turn; }
        // 0=進行中、1=X、2=O、3=引き分け。
        std::uint32_t Winner() const noexcept { return m_winner; }
    private:
        void Reset() { m_board.fill('.'); m_turn = 1; m_winner = 0; m_revision = 0; }
        void Result()
        {
            constexpr std::array<std::array<int, 3>, 8> lines{{ {0,1,2}, {3,4,5}, {6,7,8}, {0,3,6}, {1,4,7}, {2,5,8}, {0,4,8}, {2,4,6} }};
            for (const auto& line : lines)
                if (m_board[line[0]] != '.' && m_board[line[0]] == m_board[line[1]] && m_board[line[1]] == m_board[line[2]])
                { m_winner = m_board[line[0]] == 'X' ? 1 : 2; return; }
            if (std::ranges::none_of(m_board, [](const char c) { return c == '.'; })) m_winner = 3;
        }
        void Read(const std::string& state)
        {
            if (!state.starts_with("T1|") || state.size() < 20 || state[12] != '|') return;
            std::array<char, 9> board{}; std::copy_n(state.data() + 3, 9, board.data());
            if (std::ranges::any_of(board, [](const char c) { return c != '.' && c != 'X' && c != 'O'; })) return;
            std::array<std::uint32_t, 4> values{}; std::size_t begin = 13;
            for (std::size_t index = 0; index < values.size(); ++index)
            {
                const auto end = index == values.size() - 1 ? state.size() : state.find('|', begin);
                if (end == std::string::npos || end <= begin) return;
                const auto parsed = std::from_chars(state.data() + begin, state.data() + end, values[index]);
                if (parsed.ec != std::errc{} || parsed.ptr != state.data() + end) return;
                begin = end + 1;
            }
            if ((values[0] != 0 && values[0] < 2) || values[2] > 3
                || (values[2] == 0 && values[1] != 1 && values[1] != values[0])
                || (values[2] != 0 && values[1] != 0)) return;
            m_board = board; m_second = values[0]; m_turn = values[1]; m_winner = values[2]; m_revision = values[3];
        }
        std::array<char, 9> m_board{ '.', '.', '.', '.', '.', '.', '.', '.', '.' };
        NetworkPeerId m_second{}, m_turn{ 1 };
        std::uint32_t m_winner{}, m_revision{};
        std::uint64_t m_generation{};
    };
}
