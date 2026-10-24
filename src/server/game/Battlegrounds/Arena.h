/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TRINITY_ARENA_H
#define TRINITY_ARENA_H

#include "Battleground.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

enum ArenaBroadcastTexts
{
    ARENA_TEXT_START_ONE_MINUTE             = 15740,
    ARENA_TEXT_START_THIRTY_SECONDS         = 15741,
    ARENA_TEXT_START_FIFTEEN_SECONDS        = 15739,
    ARENA_TEXT_START_TEN_SECONDS            = 77866,
    ARENA_TEXT_START_BATTLE_HAS_BEGUN       = 15742,
};

enum ArenaSpellIds
{
    SPELL_ALLIANCE_GOLD_FLAG                = 32724,
    SPELL_ALLIANCE_GREEN_FLAG               = 32725,
    SPELL_HORDE_GOLD_FLAG                   = 35774,
    SPELL_HORDE_GREEN_FLAG                  = 35775,

    SPELL_LAST_MAN_STANDING                 = 26549  // Achievement Credit
};

enum ArenaWorldStates
{
    ARENA_WORLD_STATE_ALIVE_PLAYERS_GREEN   = 3600,
    ARENA_WORLD_STATE_ALIVE_PLAYERS_GOLD    = 3601,
    // A squad match's readout: squad N (from 0) has a WorldStateUI row shown
    // while SHOW_FIRST + N is 1, reading ALIVE_FIRST + N (patch-enUS-A rows
    // 90170-90173 for Baradin Hold: Four Corners).
    ARENA_WORLD_STATE_SQUAD_SHOW_FIRST      = 9431,
    ARENA_WORLD_STATE_SQUAD_ALIVE_FIRST     = 9441
};

// Buffle Shuffle (BATTLEGROUND_SSH). People queue alone and are put in one arena,
// four to a 2v2 or six to a 3v3, for a series of rounds. Before every round the
// teams are dealt again, so over the match everyone fights beside and against
// everyone else, and each player is scored by the rounds they won: more won
// than lost is a win.
//
// The match stays STATUS_IN_PROGRESS throughout. Between rounds the dead are
// raised, everyone is put back to full and cleaned of the round, moved to the
// side the schedule gives them for the next round (Battleground::
// MoveParticipantToTeam) and back to that side's start, and the gates shut for
// a short countdown. The rounds are dealt so that every two players share a
// team at least once whenever that can be done.
//
// A match plays as many rounds as there are different ways to deal its teams,
// up to SOLO_SHUFFLE_MAX_ROUNDS: six for 3v3, three for 2v2. With
// Centurion.SoloShuffle.BalanceHealers on, two healers are never on the same
// team, which leaves 3v3 with exactly six ways (each healer with each pair of
// the others) and a 2v2 with two healers only two (each healer with each of the
// others once); off, roles play no part.
uint8 constexpr SOLO_SHUFFLE_MAX_ROUNDS = 6;

class TC_GAME_API Arena : public Battleground
{
    public:
        // Whether a player plays Buffle Shuffle as a healer: most talent points in
        // a healing tree (Discipline, Holy, Restoration).
        static bool IsSoloShuffleHealer(Player const* player);
        // The same test on talent points already counted per TalentTab id, for
        // a character that is not loaded (the bot fill reads them from the DB).
        static bool IsHealerTalentSpend(std::map<uint32, uint32> const& pointsByTab);

        bool IsBetweenRounds() const override;
        bool OwnsPreparationState(Player const* player) const override;

    protected:
        Arena();

        void AddPlayer(Player* player) override;
        void RemovePlayer(Player* /*player*/, ObjectGuid guid, uint32 /*team*/) override;

        void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;
        void UpdateArenaWorldState();

        void HandleKillPlayer(Player* player, Player* killer) override;

        void UpdateSquadFeed() override;
        void SendSquadFeedTo(Player* player) override;

        // Buffle Shuffle's rounds; see SOLO_SHUFFLE_MAX_ROUNDS.
        void UpdateRounds(uint32 diff) override;
        bool IsWinningParticipant(ObjectGuid guid, uint32 team, uint32 winner) const override;

    private:
        uint32 GetArenaAlivePlayersCountByTeam(uint32 team) const;

        void RemovePlayerAtLeave(ObjectGuid guid, bool transport, bool sendPacket) override;
        void CheckWinConditions() override;
        void EndBattleground(uint32 winner) override;
        void EndAtTimeLimit() override;

        // Squad matches (see BattlegroundSquadStart). Alive is counted the way
        // GetArenaAlivePlayersCountByTeam counts it, per squad.
        bool IsArenaAlive(ObjectGuid guid) const;
        uint32 GetArenaAlivePlayersCountBySquad(uint8 squad) const;
        void CheckSquadWinConditions();
        void EndSquadMatch(uint8 winnerSquad, std::string const& announcement);
        // A rated squad match's ratings, by finishing place (ArenaTeam::PlacedAgainst).
        void EndRatedSquadMatch();
        // A rated match one arena team played against clones (Battleground::
        // GetBotFillRatedTeam): rated against the bot ladder team the clones
        // were copies of, or, with no ladder team, against an opponent of its
        // own matchmaker rating.
        void EndBotFilledRatedMatch(uint32 winner);
        // One "N Players Remaining" line per squad (ARENA_WORLD_STATE_SQUAD_*),
        // for every squad that is in the match.
        void UpdateSquadWorldStates();
        bool IsSquadInMatch(uint8 squad) const;
        // The squad feed's pieces (see UpdateSquadFeed in Arena.cpp). Members
        // come in seat order - m_Players order - in both the roster and the
        // states, which is what lets the client pair them up.
        typedef std::unordered_map<ObjectGuid, Player*> SquadFeedPlayers;
        SquadFeedPlayers GetSquadFeedPlayers() const;
        std::vector<ObjectGuid> GetSquadFeedMembers(uint8 squad) const;
        std::string BuildSquadFeedRoster(uint8 squad, std::vector<ObjectGuid> const& members, SquadFeedPlayers const& players) const;
        std::string BuildSquadFeedStates(uint8 squad, std::vector<ObjectGuid> const& members, SquadFeedPlayers const& players) const;
        std::string BuildSquadFeedWins(uint8 squad, std::vector<ObjectGuid> const& members) const;
        std::string BuildSquadFeedWinner() const;
        void BroadcastSquadFeedLine(std::string const& line, SquadFeedPlayers const& players) const;
        std::string _squadFeedRoster[MAX_BATTLEGROUND_SQUADS];
        std::string _squadFeedStates[MAX_BATTLEGROUND_SQUADS];
        std::string _squadFeedWins[MAX_BATTLEGROUND_SQUADS];
        bool _squadFeedWinnerSent = false;
        // Squads that had people when the gates opened and have since been
        // wiped out, in the order they fell.
        std::vector<uint8> _eliminatedSquads;
        uint32 _squadsAtStart = 0;

        // Buffle Shuffle.
        enum class ShufflePhase : uint8
        {
            NotStarted,     // round 1's gates have not opened yet
            Fighting,
            Intermission,   // a round is decided; the next is about to be dealt
            Preparation,    // the next round's teams behind shut gates
            Finished
        };
        // The team each roster member plays a round on, one entry per round:
        // bit i set means _shuffleRoster[i] plays on the Alliance (gold) side.
        typedef std::vector<uint32> ShuffleSchedule;
        void BeginSoloShuffle();
        void CheckShuffleRound();
        void EndShuffleRound(uint32 winner, std::string const& why);
        void EndShuffleRoundAtTimeLimit();
        void PrepareShuffleRound();
        void StartShuffleRound();
        void FinishSoloShuffle();
        void ResetForShuffleRound(Player* player);
        // A participant who has left the match for good drops out of the
        // roster; the rounds still to come are dealt again without them.
        void DropFromShuffle(ObjectGuid guid);
        // Anyone seated after the roster was made (a bot whose load finished
        // late) joins it, and the rounds to come are dealt again with them.
        void SyncShuffleRoster();
        // Deals the rounds after the current one again for the roster as it
        // stands, the rounds already fought counting towards who has teamed
        // with whom.
        void RedealRemainingShuffleRounds();
        uint32 GetShuffleSideMask() const;
        void SendToShuffleStart(Player* player);
        std::string GetShuffleTeams(uint32 team) const;
        std::string GetShuffleStandings() const;
        bool WonSoloShuffle(ObjectGuid guid) const;
        // The team flag of the side a player is on (or, in a squad match, of
        // their squad).
        void ApplyArenaFlag(Player* player);
        void RemoveArenaFlags(Player* player);
        ShufflePhase _shufflePhase = ShufflePhase::NotStarted;
        uint8 _shuffleRound = 0;             // the round being played or prepared, from 1
        uint8 _shuffleRoundCount = 0;        // how many the match plays, set when it begins
        uint32 _shuffleTimer = 0;
        bool _shuffleTenSecondsSent = false;
        uint32 _shuffleRoundsPlayed = 0;
        std::vector<ObjectGuid> _shuffleRoster;
        ShuffleSchedule _shuffleSchedule;    // index 0 is round 1
        std::map<ObjectGuid, uint32> _shuffleWins;
        std::set<ObjectGuid> _shuffleHealers;  // read when the roster is made

        // The squad feed reads a shuffle as two squads: its two sides while it
        // is fought (0 gold, 1 green), and once it is over the winners (0) and
        // everyone else (1). A squad match reads its own squads.
        uint8 GetFeedSquad(ObjectGuid guid) const;
        uint8 GetFeedSquadCount() const;
        bool UsesSquadFeed() const { return IsSquadMatch() || IsSoloShuffle(); }
        // The squad each player's client was last told is theirs (SQ:), so a
        // shuffle can tell them again when a new round moves them.
        std::map<ObjectGuid, uint8> _squadFeedMine;

        // In-memory custom-game clones are authoritative battleground-roster
        // participants even when the global connected-player accessor cannot
        // resolve them during a death transition. Arenas are elimination games,
        // so retain the eliminated GUIDs and count survivors from m_Players.
        std::set<ObjectGuid> _customGameEliminatedPlayers;
};

#endif // TRINITY_ARENA_H
