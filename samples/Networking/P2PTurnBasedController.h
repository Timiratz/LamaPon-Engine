#pragma once

#include "LamaPon/Online/NetworkSession.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <string_view>

namespace LamaPon::Samples
{
    // ホストと最初の参加者が三目並べを行い、この管理処理へイベント消費を集約する。
    class P2PTurnBasedController final
    {
    public:
        // 各フレームに参加者と手番を検証し、ホストの盤面状態を共有する(session: 通信の状態とイベント源)。
        void Update(NetworkSession& session)
        {
            if (m_generation != session.Generation())
            {
                m_generation = session.Generation(); Reset(); m_second = 0;
            }
            if (!session.IsHost() && session.State() != NetworkState::Connected) return;
            if (session.IsHost())
            {
                // ホスト以外の最初の参加者ID
                NetworkPeerId second{};
                // 対戦相手の候補となる参加者
                for (const auto& member : session.Members()) if (member.id != 1) { second = member.id; break; }
                if (second != m_second) { m_second = second; Reset(); }
            }
            else Read(session.SessionState());
            // 検証する受信コマンドイベント
            NetworkEvent event;
            while (session.PollEvent(event))
            {
                if (!session.IsHost() || event.kind != NetworkEventKind::Command || event.name != "board.move"
                    || m_second == 0 || event.peer != m_turn || m_winner != 0) continue;
                // 世代番号とセル番号を分ける位置
                const auto colon = event.data.find(':');
                if (colon == std::string::npos || colon + 2 != event.data.size()) continue;
                // 受信コマンドの盤面更新番号
                std::uint32_t revision{};
                // 更新番号または状態整数の解析結果
                const auto parsed = std::from_chars(event.data.data(), event.data.data() + colon, revision);
                // 0～8を表す受信セル文字
                const char cell = event.data.back();
                if (parsed.ec != std::errc{} || parsed.ptr != event.data.data() + colon || revision != m_revision
                    || cell < '0' || cell > '8' || m_board[static_cast<std::size_t>(cell - '0')] != '.') continue;
                m_board[static_cast<std::size_t>(cell - '0')] = event.peer == 1 ? 'X' : 'O';
                ++m_revision; Result();
                m_turn = m_winner == 0 ? (m_turn == 1 ? m_second : 1) : 0;
            }
            if (session.IsHost())
            {
                // T1形式で配信する盤面と手番
                const std::string state = "T1|" + std::string(m_board.data(), m_board.size()) + "|"
                    + std::to_string(m_second) + "|" + std::to_string(m_turn) + "|"
                    + std::to_string(m_winner) + "|" + std::to_string(m_revision);
                if (!session.SetSessionState(state)) session.Abort("ターン状態を送信できません。");
            }
        }
        // 自分の手番の空セルを更新番号付きでホストへ要求する(session: 要求を送るSession, cell: 0～8の置くセル番号)。
        bool RequestMove(NetworkSession& session, const std::size_t cell) const
        {
            return cell < 9 && m_board[cell] == '.' && m_second != 0 && m_winner == 0 && session.LocalPeer() == m_turn
                && session.SendCommand("board.move", std::to_string(m_revision) + ":" + std::to_string(cell));
        }
        // 点が空欄・Xがホスト・Oが参加者の盤面を借用で返す。
        const std::array<char, 9>& Board() const noexcept { return m_board; }
        // 現在の手番のPeer IDを返し、終了時は0を返す。
        NetworkPeerId Turn() const noexcept { return m_turn; }
        // 進行中0・X勝ち1・O勝ち2・引き分け3の結果番号を返す。
        std::uint32_t Winner() const noexcept { return m_winner; }
    private:
        // 盤面を空にし、ホスト先手で更新番号と勝敗を初期化する。
        void Reset() { m_board.fill('.'); m_turn = 1; m_winner = 0; m_revision = 0; }
        // 三目の8組と空欄を調べ、勝者番号か引き分けを記録する。
        void Result()
        {
            // 横縦斜めの三目成立セル8組
            constexpr std::array<std::array<int, 3>, 8> lines{{ {0,1,2}, {3,4,5}, {6,7,8}, {0,3,6}, {1,4,7}, {2,5,8}, {0,4,8}, {2,4,6} }};
            // 同じ印が並ぶか調べるセル3個
            for (const auto& line : lines)
                if (m_board[line[0]] != '.' && m_board[line[0]] == m_board[line[1]] && m_board[line[1]] == m_board[line[2]])
                { m_winner = m_board[line[0]] == 'X' ? 1 : 2; return; }
            // 空欄が残るかを調べ、なければ引き分けにする(c: 盤面のセル文字)。
            if (std::ranges::none_of(m_board, [](const char c) { return c == '.'; })) m_winner = 3;
        }
        // T1形式の盤面と数値を検査し、有効な状態だけ取り込む(state: ホストから共有された状態)。
        void Read(const std::string& state)
        {
            if (!state.starts_with("T1|") || state.size() < 20 || state[12] != '|') return;
            // 検証してから取り込む9セルの盤面
            std::array<char, 9> board{}; std::copy_n(state.data() + 3, 9, board.data());
            // 点・X・O以外のセルを含む状態を拒む(c: 受信盤面のセル文字)。
            if (std::ranges::any_of(board, [](const char c) { return c != '.' && c != 'X' && c != 'O'; })) return;
            // 相手ID・手番・結果・更新番号
            std::array<std::uint32_t, 4> values{};
            // 次の数値の開始文字位置
            std::size_t begin = 13;
            // 読み込む状態整数0～3の番号
            for (std::size_t index = 0; index < values.size(); ++index)
            {
                // 数値を区切る縦棒か文末の位置
                const auto end = index == values.size() - 1 ? state.size() : state.find('|', begin);
                if (end == std::string::npos || end <= begin) return;
                // 更新番号または状態整数の解析結果
                const auto parsed = std::from_chars(state.data() + begin, state.data() + end, values[index]);
                if (parsed.ec != std::errc{} || parsed.ptr != state.data() + end) return;
                begin = end + 1;
            }
            if ((values[0] != 0 && values[0] < 2) || values[2] > 3
                || (values[2] == 0 && values[1] != 1 && values[1] != values[0])
                || (values[2] != 0 && values[1] != 0)) return;
            m_board = board; m_second = values[0]; m_turn = values[1]; m_winner = values[2]; m_revision = values[3];
        }
        // 点・X・Oを保持する9セルの盤面
        std::array<char, 9> m_board{ '.', '.', '.', '.', '.', '.', '.', '.', '.' };
        // m_second: O側Peer ID、m_turn: 現在の手番ID
        NetworkPeerId m_second{}, m_turn{ 1 };
        // m_winner: 勝敗結果番号、m_revision: 盤面更新番号
        std::uint32_t m_winner{}, m_revision{};
        // 前回更新したSessionの世代
        std::uint64_t m_generation{};
    };
}
