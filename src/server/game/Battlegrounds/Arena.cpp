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

#include "Arena.h"
#include "ArenaScore.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Containers.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "StringFormat.h"
#include "Surnames.h"
#include "TemporarySummon.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldStatePackets.h"

#include <algorithm>
#include <cmath>

void ArenaScore::AppendToPacket(WorldPacket& data)
{
    data << uint64(PlayerGuid);

    data << uint32(KillingBlows);
    data << uint8(TeamId);
    data << uint32(DamageDone);
    data << uint32(HealingDone);

    BuildObjectivesBlock(data);
}

void ArenaScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // Objectives Count
}

void ArenaTeamScore::BuildRatingInfoBlock(WorldPacket& data)
{
    uint32 ratingLost = std::abs(std::min(RatingChange, 0));
    uint32 ratingWon = std::max(RatingChange, 0);

    // should be old rating, new rating, and client will calculate rating change itself
    data << uint32(ratingLost);
    data << uint32(ratingWon);
    data << uint32(MatchmakerRating);
}

void ArenaTeamScore::BuildTeamInfoBlock(WorldPacket& data)
{
    data << TeamName;
}

Arena::Arena()
{
    StartDelayTimes[BG_STARTING_EVENT_FIRST]  = BG_START_DELAY_30S;
    StartDelayTimes[BG_STARTING_EVENT_SECOND] = BG_START_DELAY_15S;
    StartDelayTimes[BG_STARTING_EVENT_THIRD]  = BG_START_DELAY_10S;
    StartDelayTimes[BG_STARTING_EVENT_FOURTH] = BG_START_DELAY_NONE;

    StartMessageIds[BG_STARTING_EVENT_FIRST]  = ARENA_TEXT_START_THIRTY_SECONDS;
    StartMessageIds[BG_STARTING_EVENT_SECOND] = ARENA_TEXT_START_FIFTEEN_SECONDS;
    StartMessageIds[BG_STARTING_EVENT_THIRD]  = ARENA_TEXT_START_TEN_SECONDS;
    StartMessageIds[BG_STARTING_EVENT_FOURTH] = ARENA_TEXT_START_BATTLE_HAS_BEGUN;
}

void Arena::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    Battleground::AddPlayer(player);
    if (IsCustomGame())
        _customGameEliminatedPlayers.erase(player->GetGUID());

    if (!isInBattleground)
        PlayerScores[player->GetGUID().GetCounter()] = new ArenaScore(player->GetGUID(), player->GetBGTeam());

    ApplyArenaFlag(player);

    UpdateArenaWorldState();
}

void Arena::RemoveArenaFlags(Player* player)
{
    for (uint32 spellId : { uint32(SPELL_ALLIANCE_GOLD_FLAG), uint32(SPELL_ALLIANCE_GREEN_FLAG),
        uint32(SPELL_HORDE_GOLD_FLAG), uint32(SPELL_HORDE_GREEN_FLAG) })
        player->RemoveAurasDueToSpell(spellId);
}

void Arena::ApplyArenaFlag(Player* player)
{
    // A squad match: each squad carries its own flag (battleground_squad_start
    // FlagSpell) - two squads share each side, so the side's flag would make
    // two of them look alike.
    if (uint32 const squadFlag = GetSquadFlagSpell(GetPlayerSquad(player->GetGUID())))
    {
        player->CastSpell(player, squadFlag, true);
        return;
    }

    // The side's, and only that one: a shuffle changes sides between rounds.
    RemoveArenaFlags(player);
    if (player->GetBGTeam() == ALLIANCE)        // gold
    {
        if (player->GetTeam() == HORDE)
            player->CastSpell(player, SPELL_HORDE_GOLD_FLAG, true);
        else
            player->CastSpell(player, SPELL_ALLIANCE_GOLD_FLAG, true);
    }
    else                                        // green
    {
        if (player->GetTeam() == HORDE)
            player->CastSpell(player, SPELL_HORDE_GREEN_FLAG, true);
        else
            player->CastSpell(player, SPELL_ALLIANCE_GREEN_FLAG, true);
    }
}

void Arena::RemovePlayer(Player* player, ObjectGuid guid, uint32 /*team*/)
{
    if (IsCustomGame())
        _customGameEliminatedPlayers.erase(guid);

    // SpellInfo::CheckLocation would drop a squad flag once the player is
    // out of the arena; take it off on the way out so it never lingers.
    if (player)
        if (uint32 const squadFlag = GetSquadFlagSpell(GetPlayerSquad(guid)))
            player->RemoveAurasDueToSpell(squadFlag);

    // Their enemy frames go with them; the others see the roster change on
    // the feed's next look.
    if (player && UsesSquadFeed())
        SendSquadFeedLine(player, "SQ:-");
    _squadFeedMine.erase(guid);

    if (GetStatus() == STATUS_WAIT_LEAVE)
        return;

    // Gone for good (not just logged out, which keeps the seat): the rounds
    // still to come are dealt without them.
    if (IsSoloShuffle() && !IsPlayerInBattleground(guid))
        DropFromShuffle(guid);

    UpdateArenaWorldState();
    CheckWinConditions();
}

void Arena::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    // Every arena sends all the squad switches, off unless this is a squad
    // match with that squad in it. The client keeps a world state's value until
    // it logs out: SMSG_INIT_WORLD_STATES only overwrites the states it carries
    // (Wow.exe 0x526937 -> 0x549440; the value table at 0xBE8F58 is emptied
    // only by the leave-world teardown, 0x548B20). Baradin Hold Arena and Four
    // Corners share map 1401 and its WorldStateUI rows, so without this anyone
    // who had played Four Corners saw its squad lines under the gold/green pair
    // of every later match there - Buffle Shuffle included - until they relogged.
    for (uint8 squad = 0; squad < MAX_BATTLEGROUND_SQUADS; ++squad)
    {
        bool const inMatch = squad < GetSquadCount() && IsSquadInMatch(squad);
        packet.Worldstates.emplace_back(ARENA_WORLD_STATE_SQUAD_SHOW_FIRST + squad, inMatch ? 1 : 0);
        if (squad < GetSquadCount())
            packet.Worldstates.emplace_back(ARENA_WORLD_STATE_SQUAD_ALIVE_FIRST + squad, GetArenaAlivePlayersCountBySquad(squad));
    }

    if (IsSquadMatch())
        return;

    packet.Worldstates.emplace_back(ARENA_WORLD_STATE_ALIVE_PLAYERS_GREEN, GetArenaAlivePlayersCountByTeam(HORDE));
    packet.Worldstates.emplace_back(ARENA_WORLD_STATE_ALIVE_PLAYERS_GOLD, GetArenaAlivePlayersCountByTeam(ALLIANCE));
}

// ---------------------------------------------------------------- squad feed
//
// What the multi-team arena enemy frames (client CenturionSquadFrames.lua)
// draw, and what colours the scoreboard by squad - all CCGAME addon lines:
//
//   SQ:<mine>:<squads>        you are in squad <mine> (0-3, "-" for none) of <squads>
//   SQ:-                      not a squad match, or you left it: clear everything
//   SQR:<s>:<team>:<members>  squad s's roster in seat order, "name,class;...",
//                             <team> being its arena team's name (rated only)
//   SQH:<s>:<states>          the same members' states, comma-separated:
//                             <health%>.<power%>.<power type>, or d (dead),
//                             s (stealthed or invisible), o (not on the map)
//   SQW:<s> / SQW:-           the winning squad, or a draw, once it is over
//   SQV:<s>:<wins>            Buffle Shuffle only: the same members' rounds won
//                             so far, comma-separated (the scoreboard's Wins)
//
// A line goes out when it changes, looked at every BATTLEGROUND_SQUAD_FEED_INTERVAL;
// a client that has just loaded asks for all of it (CCGAMEREQ SQUADS). A member
// in stealth is "s" to everyone, the way the stock frames lose an opponent who
// stealths: their health is not sent while they cannot be seen.

Arena::SquadFeedPlayers Arena::GetSquadFeedPlayers() const
{
    // From the map rather than ObjectAccessor: it holds the bot clones too, and
    // it asks nothing of anyone who has already gone.
    SquadFeedPlayers players;
    if (BattlegroundMap* map = FindBgMap())
        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
            if (Player* player = itr->GetSource())
                players[player->GetGUID()] = player;
    return players;
}

uint8 Arena::GetFeedSquad(ObjectGuid guid) const
{
    if (IsSquadMatch())
        return GetPlayerSquad(guid);

    if (!IsSoloShuffle())
        return BATTLEGROUND_NO_SQUAD;

    if (_shufflePhase == ShufflePhase::Finished)
        return WonSoloShuffle(guid) ? 0 : 1;

    uint32 const team = GetPlayerTeam(guid);
    return team == ALLIANCE ? 0 : (team == HORDE ? 1 : BATTLEGROUND_NO_SQUAD);
}

uint8 Arena::GetFeedSquadCount() const
{
    if (IsSquadMatch())
        return GetSquadCount();
    return IsSoloShuffle() ? 2 : 0;
}

std::vector<ObjectGuid> Arena::GetSquadFeedMembers(uint8 squad) const
{
    std::vector<ObjectGuid> members;
    for (auto const& [guid, participant] : m_Players)
    {
        (void)participant;
        if (GetFeedSquad(guid) == squad)
            members.push_back(guid);
    }
    return members;
}

std::string Arena::BuildSquadFeedRoster(uint8 squad, std::vector<ObjectGuid> const& members, SquadFeedPlayers const& players) const
{
    std::string line = Trinity::StringFormat("SQR:{}:", squad);
    if (uint32 const arenaTeamId = GetSquadArenaTeamId(squad))
        if (ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(arenaTeamId))
            line += arenaTeam->GetName();
    line += ':';

    bool first = true;
    for (ObjectGuid guid : members)
    {
        // The name the client knows them by: the display name (a clone's own
        // name is internal) with the family name after it, as every name query
        // answers. The scoreboard matches rows by it and the frames target by
        // it, so "Jahzir" for "Jahzir Frostmask" missed both.
        std::string name;
        uint32 playerClass = 0;
        SquadFeedPlayers::const_iterator const itr = players.find(guid);
        if (itr != players.end())
        {
            name = GetPlayerDisplayName(itr->second);
            playerClass = itr->second->GetClass();
        }
        else if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid))
        {
            name = Surnames::Decorated(guid, entry->Name);
            playerClass = entry->Class;
        }
        if (name.empty())
            name = "?";

        if (!first)
            line += ';';
        first = false;
        line += name;
        line += ',';
        line += std::to_string(playerClass);
    }
    return line;
}

std::string Arena::BuildSquadFeedStates(uint8 squad, std::vector<ObjectGuid> const& members, SquadFeedPlayers const& players) const
{
    std::string line = Trinity::StringFormat("SQH:{}:", squad);
    bool first = true;
    for (ObjectGuid guid : members)
    {
        if (!first)
            line += ',';
        first = false;

        SquadFeedPlayers::const_iterator const itr = players.find(guid);
        Player* player = itr != players.end() ? itr->second : nullptr;
        if (!player)
            line += 'o';
        else if (!IsArenaAlive(guid))
            line += 'd';
        else if (player->HasStealthAura() || player->HasInvisibilityAura())
            line += 's';
        else
        {
            // Rounded up, so nobody alive reads 0%.
            uint64 const maxHealth = std::max<uint32>(player->GetMaxHealth(), 1);
            uint32 const health = uint32(std::min<uint64>((uint64(player->GetHealth()) * 100 + maxHealth - 1) / maxHealth, 100));
            Powers const powerType = player->GetPowerType();
            uint64 const maxPower = player->GetMaxPower(powerType);
            uint32 const power = maxPower ? uint32(std::min<uint64>(uint64(player->GetPower(powerType)) * 100 / maxPower, 100)) : 0;
            line += Trinity::StringFormat("{}.{}.{}", std::max<uint32>(health, 1), power, uint32(powerType));
        }
    }
    return line;
}

std::string Arena::BuildSquadFeedWins(uint8 squad, std::vector<ObjectGuid> const& members) const
{
    std::string line = Trinity::StringFormat("SQV:{}:", squad);
    bool first = true;
    for (ObjectGuid guid : members)
    {
        if (!first)
            line += ',';
        first = false;

        auto const wins = _shuffleWins.find(guid);
        line += std::to_string(wins != _shuffleWins.end() ? wins->second : 0);
    }
    return line;
}

std::string Arena::BuildSquadFeedWinner() const
{
    // A shuffle's "winning squad" is everyone who won more rounds than they
    // lost (feed squad 0), if anyone did.
    if (IsSoloShuffle())
        return GetWinner() == PVP_TEAM_ALLIANCE ? std::string("SQW:0") : std::string("SQW:-");

    uint8 const winner = GetWinnerSquad();
    return winner < GetSquadCount() ? Trinity::StringFormat("SQW:{}", winner) : std::string("SQW:-");
}

void Arena::BroadcastSquadFeedLine(std::string const& line, SquadFeedPlayers const& players) const
{
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty, ObjectGuid::Empty, "CCGAME\t" + line, 0);
    for (auto const& [guid, player] : players)
    {
        (void)guid;
        if (player->GetSession() && !player->GetSession()->IsVirtualSession())
            player->SendDirectMessage(&data);
    }
}

void Arena::UpdateSquadFeed()
{
    if (!UsesSquadFeed() || !FindBgMap())
        return;

    SquadFeedPlayers const players = GetSquadFeedPlayers();

    // A shuffle moves people from one side to the other between rounds, and
    // their client has to be told which side it is now on before the rosters.
    if (IsSoloShuffle())
    {
        for (auto const& [guid, player] : players)
        {
            if (!IsPlayerInBattleground(guid))
                continue;

            uint8 const mine = GetFeedSquad(guid);
            auto const known = _squadFeedMine.find(guid);
            if (known != _squadFeedMine.end() && known->second == mine)
                continue;

            _squadFeedMine[guid] = mine;
            SendSquadFeedLine(player, Trinity::StringFormat("SQ:{}:{}",
                mine < GetFeedSquadCount() ? std::to_string(mine) : std::string("-"), uint32(GetFeedSquadCount())));
        }
    }

    for (uint8 squad = 0; squad < GetFeedSquadCount() && squad < MAX_BATTLEGROUND_SQUADS; ++squad)
    {
        std::vector<ObjectGuid> const members = GetSquadFeedMembers(squad);
        std::string roster = BuildSquadFeedRoster(squad, members, players);
        if (roster != _squadFeedRoster[squad])
        {
            _squadFeedRoster[squad] = std::move(roster);
            // The states are read in the roster's order, so they follow it out.
            _squadFeedStates[squad].clear();
            _squadFeedWins[squad].clear();
            BroadcastSquadFeedLine(_squadFeedRoster[squad], players);
        }

        std::string states = BuildSquadFeedStates(squad, members, players);
        if (states != _squadFeedStates[squad])
        {
            _squadFeedStates[squad] = std::move(states);
            BroadcastSquadFeedLine(_squadFeedStates[squad], players);
        }

        if (IsSoloShuffle())
        {
            std::string wins = BuildSquadFeedWins(squad, members);
            if (wins != _squadFeedWins[squad])
            {
                _squadFeedWins[squad] = std::move(wins);
                BroadcastSquadFeedLine(_squadFeedWins[squad], players);
            }
        }
    }

    if (GetStatus() == STATUS_WAIT_LEAVE && !_squadFeedWinnerSent)
    {
        _squadFeedWinnerSent = true;
        BroadcastSquadFeedLine(BuildSquadFeedWinner(), players);
    }
}

void Arena::SendSquadFeedTo(Player* player)
{
    if (!UsesSquadFeed())
    {
        Battleground::SendSquadFeedTo(player);
        return;
    }
    if (!player)
        return;

    uint8 const mine = GetFeedSquad(player->GetGUID());
    if (IsSoloShuffle() && IsPlayerInBattleground(player->GetGUID()))
        _squadFeedMine[player->GetGUID()] = mine;
    SendSquadFeedLine(player, Trinity::StringFormat("SQ:{}:{}",
        mine < GetFeedSquadCount() ? std::to_string(mine) : std::string("-"), uint32(GetFeedSquadCount())));

    SquadFeedPlayers const players = GetSquadFeedPlayers();
    for (uint8 squad = 0; squad < GetFeedSquadCount() && squad < MAX_BATTLEGROUND_SQUADS; ++squad)
    {
        std::vector<ObjectGuid> const members = GetSquadFeedMembers(squad);
        SendSquadFeedLine(player, BuildSquadFeedRoster(squad, members, players));
        SendSquadFeedLine(player, BuildSquadFeedStates(squad, members, players));
        if (IsSoloShuffle())
            SendSquadFeedLine(player, BuildSquadFeedWins(squad, members));
    }

    if (_squadFeedWinnerSent)
        SendSquadFeedLine(player, BuildSquadFeedWinner());
}

bool Arena::IsSquadInMatch(uint8 squad) const
{
    // Once the gates are open, the squads that were there; before, anyone
    // seated or on the way in. A corner nobody came for stays off the readout.
    if (_squadsAtStart)
        return (_squadsAtStart & (1u << squad)) != 0;

    return CountSquadSeats(squad) != 0;
}

void Arena::UpdateArenaWorldState()
{
    if (IsSquadMatch())
    {
        UpdateSquadWorldStates();
        return;
    }

    UpdateWorldState(ARENA_WORLD_STATE_ALIVE_PLAYERS_GREEN, GetArenaAlivePlayersCountByTeam(HORDE));
    UpdateWorldState(ARENA_WORLD_STATE_ALIVE_PLAYERS_GOLD, GetArenaAlivePlayersCountByTeam(ALLIANCE));
}

void Arena::HandleKillPlayer(Player* player, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    if (IsCustomGame() && player)
        _customGameEliminatedPlayers.insert(player->GetGUID());

    Battleground::HandleKillPlayer(player, killer);

    UpdateArenaWorldState();
    CheckWinConditions();
}

void Arena::RemovePlayerAtLeave(ObjectGuid guid, bool transport, bool sendPacket)
{
    if (isRated() && GetStatus() == STATUS_IN_PROGRESS)
    {
        BattlegroundPlayerMap::const_iterator itr = m_Players.find(guid);
        if (itr != m_Players.end() && IsSquadMatch())
        {
            // Left a rated squad match while it was being fought: lost, against
            // the other squads taken together.
            uint8 const squad = GetPlayerSquad(guid);
            if (ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(GetSquadArenaTeamId(squad)))
            {
                uint32 const against = GetSquadOpponentsMatchmakerRating(squad);
                if (Player* player = _GetPlayer(itr->first, itr->second.OfflineRemoveTime != 0, "Arena::RemovePlayerAtLeave"))
                    arenaTeam->MemberLost(player, against);
                else
                    arenaTeam->OfflineMemberLost(guid, against);
            }
        }
        else if (itr != m_Players.end()) // check if the player was a participant of the match, or only entered through gm command (appear)
        {
            // if the player was a match participant, calculate rating
            uint32 team = itr->second.Team;

            ArenaTeam* winnerArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(GetOtherTeam(team)));
            ArenaTeam* loserArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(team));

            // left a rated match while the encounter was in progress, consider as loser
            // - against clones as well (EndBotFilledRatedMatch), once there are any
            // to fight; the clones' own side is never charged for one of them
            bool const lost = GetBotFillSide()
                ? loserArenaTeam && team == GetBotFillRatedTeam() && HasBotFillOpponentsFielded()
                : winnerArenaTeam && loserArenaTeam && winnerArenaTeam != loserArenaTeam;
            if (lost)
            {
                if (Player* player = _GetPlayer(itr->first, itr->second.OfflineRemoveTime != 0, "Arena::RemovePlayerAtLeave"))
                    loserArenaTeam->MemberLost(player, GetArenaMatchmakerRating(GetOtherTeam(team)));
                else
                    loserArenaTeam->OfflineMemberLost(guid, GetArenaMatchmakerRating(GetOtherTeam(team)));
            }
        }
    }

    // remove player
    Battleground::RemovePlayerAtLeave(guid, transport, sendPacket);
}

void Arena::CheckWinConditions()
{
    if (IsReplay())
    {
        return;
    }
    if (IsSoloShuffle())
    {
        CheckShuffleRound();
        return;
    }
    if (IsSquadMatch())
    {
        CheckSquadWinConditions();
        return;
    }
    if (!GetArenaAlivePlayersCountByTeam(ALLIANCE) && GetPlayersCountByTeam(HORDE))
        EndBattleground(HORDE);
    else if (GetPlayersCountByTeam(ALLIANCE) && !GetArenaAlivePlayersCountByTeam(HORDE))
        EndBattleground(ALLIANCE);
}

namespace
{
    void AnnounceToArena(Battleground* arena, std::string const& text)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_BG_SYSTEM_NEUTRAL, LANG_UNIVERSAL, ObjectGuid::Empty, ObjectGuid::Empty, text, 0);
        arena->SendPacketToAll(&data);
    }

    char const* CountWord(uint32 count)
    {
        switch (count)
        {
            case 1: return "One";
            case 2: return "Two";
            case 3: return "Three";
            case 4: return "Four";
            default: return "No";
        }
    }
}

bool Arena::IsArenaAlive(ObjectGuid guid) const
{
    BattlegroundPlayerMap::const_iterator const itr = m_Players.find(guid);
    if (itr == m_Players.end())
        return false;

    if (IsCustomGame())
        return !itr->second.OfflineRemoveTime &&
            _customGameEliminatedPlayers.find(guid) == _customGameEliminatedPlayers.end();

    Player* player = ObjectAccessor::FindPlayer(guid);
    return player && player->IsAlive() && player->GetShapeshiftForm() != FORM_SPIRITOFREDEMPTION;
}

uint32 Arena::GetArenaAlivePlayersCountBySquad(uint8 squad) const
{
    uint32 aliveCount = 0;
    for (auto const& [guid, participant] : m_Players)
    {
        (void)participant;
        if (GetPlayerSquad(guid) == squad && IsArenaAlive(guid))
            ++aliveCount;
    }

    return aliveCount;
}

void Arena::UpdateSquadWorldStates()
{
    // One line per squad in the top frame ("Blue Team: 2 Players Remaining"):
    // the WorldStateUI rows for the map read these, in place of the gold and
    // green pair the side-based rows show.
    for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
    {
        UpdateWorldState(ARENA_WORLD_STATE_SQUAD_SHOW_FIRST + squad, IsSquadInMatch(squad) ? 1 : 0);
        UpdateWorldState(ARENA_WORLD_STATE_SQUAD_ALIVE_FIRST + squad, GetArenaAlivePlayersCountBySquad(squad));
    }
}

void Arena::CheckSquadWinConditions()
{
    // Before the gates open nobody can be beaten. Whoever is missing then is
    // settled when they open (SkipStartDelay runs this again), the way a
    // two-team arena hands the match to the side that turned up.
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    uint32 present[MAX_BATTLEGROUND_SQUADS] = { };
    for (auto const& [guid, participant] : m_Players)
    {
        (void)participant;
        uint8 const squad = GetPlayerSquad(guid);
        if (squad < GetSquadCount())
            ++present[squad];
    }

    // The squads in this fight are the ones standing in their corner when the
    // gates opened. A squad nobody came for never took part, so it is never
    // announced as beaten.
    if (!_squadsAtStart)
    {
        for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
            if (present[squad])
                _squadsAtStart |= 1u << squad;

        uint32 squadCount = 0;
        for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
            if (_squadsAtStart & (1u << squad))
                ++squadCount;

        if (squadCount > 1)
            AnnounceToArena(this, Trinity::StringFormat("{} teams, one to a corner. The last team standing wins.", CountWord(squadCount)));
    }

    uint8 lastStanding = BATTLEGROUND_NO_SQUAD;
    uint32 standing = 0;
    std::vector<uint8> fallen;
    for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
    {
        if (!(_squadsAtStart & (1u << squad)))
            continue;

        if (GetArenaAlivePlayersCountBySquad(squad))
        {
            lastStanding = squad;
            ++standing;
            continue;
        }

        if (std::find(_eliminatedSquads.begin(), _eliminatedSquads.end(), squad) != _eliminatedSquads.end())
            continue;

        _eliminatedSquads.push_back(squad);
        fallen.push_back(squad);
        TC_LOG_DEBUG("bg.arena", "Arena::CheckSquadWinConditions: squad {} ({}) eliminated in instance {}.",
            uint32(squad), GetSquadName(squad), GetInstanceID());
    }

    // Only while the fight goes on: a squad wiped out by the blow that ends the
    // match hears who won instead.
    if (standing > 1)
        for (uint8 squad : fallen)
            AnnounceToArena(this, Trinity::StringFormat("The {} team has been eliminated. {} teams remain.",
                GetSquadName(squad), CountWord(standing)));

    if (standing == 1)
    {
        EndSquadMatch(lastStanding, Trinity::StringFormat("The {} team is the last one standing and wins!", GetSquadName(lastStanding)));
        return;
    }

    if (!standing)
    {
        // The last two fell to the same blow, or nobody turned up at all.
        SetWinnerSquad(BATTLEGROUND_NO_SQUAD);
        AnnounceToArena(this, "Nobody is left standing. The match is a draw.");
        EndBattleground(0);
    }
}

void Arena::EndSquadMatch(uint8 winnerSquad, std::string const& announcement)
{
    SetWinnerSquad(winnerSquad);
    AnnounceToArena(this, announcement);
    // The side the winning squad sits on. Rewards and the scoreboard ask the
    // squad (Battleground::IsWinningParticipant), so its side-mate squad, which
    // lost, is not paid as a winner.
    EndBattleground(GetSquadSide(winnerSquad));
}

void Arena::EndAtTimeLimit()
{
    if (!IsSquadMatch())
    {
        EndBattleground(0);
        return;
    }

    // Time up in a squad match: the squad with the most still standing takes
    // it, and between equal numbers the one with the most health left between
    // them. Still level is a draw, as a two-team arena's time limit always is.
    uint8 best = BATTLEGROUND_NO_SQUAD;
    uint32 bestAlive = 0;
    uint64 bestHealth = 0;
    bool level = false;
    for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
    {
        uint32 alive = 0;
        uint64 health = 0;
        for (auto const& [guid, participant] : m_Players)
        {
            (void)participant;
            if (GetPlayerSquad(guid) != squad || !IsArenaAlive(guid))
                continue;

            ++alive;
            if (Player* player = ObjectAccessor::FindPlayer(guid))
                health += player->GetHealth();
        }

        if (!alive)
            continue;

        if (best == BATTLEGROUND_NO_SQUAD || alive > bestAlive || (alive == bestAlive && health > bestHealth))
        {
            best = squad;
            bestAlive = alive;
            bestHealth = health;
            level = false;
        }
        else if (alive == bestAlive && health == bestHealth)
            level = true;
    }

    if (best == BATTLEGROUND_NO_SQUAD || level)
    {
        SetWinnerSquad(BATTLEGROUND_NO_SQUAD);
        AnnounceToArena(this, "Time is up and no team is ahead. The match is a draw.");
        EndBattleground(0);
        return;
    }

    EndSquadMatch(best, Trinity::StringFormat("Time is up. The {} team has the most left standing and wins!", GetSquadName(best)));
}

uint32 Arena::GetArenaAlivePlayersCountByTeam(uint32 team) const
{
    if (!IsCustomGame())
        return GetAlivePlayersCountByTeam(team);

    uint32 aliveCount = 0;
    for (auto const& [guid, participant] : m_Players)
        if (participant.Team == team && !participant.OfflineRemoveTime &&
            _customGameEliminatedPlayers.find(guid) == _customGameEliminatedPlayers.end())
            ++aliveCount;

    return aliveCount;
}

void Arena::EndRatedSquadMatch()
{
    std::vector<uint8> squads;
    for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
        if (sArenaTeamMgr->GetArenaTeamById(GetSquadArenaTeamId(squad)))
            squads.push_back(squad);

    if (squads.size() < 2)
        return;

    uint8 const winnerSquad = GetWinnerSquad();

    // Nobody won - time ran out level, or the last two fell together. A
    // two-team arena takes ARENA_TIMELIMIT_POINTS_LOSS off both teams then, and
    // so does this, off every team, and counts it a loss for everyone.
    if (winnerSquad == BATTLEGROUND_NO_SQUAD)
    {
        for (uint8 squad : squads)
        {
            ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(GetSquadArenaTeamId(squad));
            uint32 const against = GetSquadOpponentsMatchmakerRating(squad);
            arenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS);
            _squadArenaScores[squad].Assign(ARENA_TIMELIMIT_POINTS_LOSS, GetSquadMatchmakerRating(squad), arenaTeam->GetName());
            for (auto const& [guid, participant] : m_Players)
            {
                if (GetPlayerSquad(guid) != squad)
                    continue;

                if (Player* player = _GetPlayer(guid, participant.OfflineRemoveTime != 0, "Arena::EndRatedSquadMatch"))
                {
                    arenaTeam->MemberLost(player, against, 0);
                    player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_NO_LOSE, 0);
                }
                else
                    arenaTeam->OfflineMemberLost(guid, against, 0);
            }

            m_SquadScoreboardRival[squad] = squads.front() != squad ? squads.front() : squads.back();
            arenaTeam->SaveToDB();
            arenaTeam->NotifyStatsChanged();
        }
        return;
    }

    // Finishing places, lower is better: the winner; then any squad still on
    // its feet when time ran out, level with each other; then the eliminated,
    // the last to fall first; then, level and last, any team that never came
    // in (its members already lost when they let the invite go).
    uint32 place[MAX_BATTLEGROUND_SQUADS];
    for (uint8 squad = 0; squad < MAX_BATTLEGROUND_SQUADS; ++squad)
        place[squad] = MAX_BATTLEGROUND_SQUADS;

    auto const tookPart = [this](uint8 squad) { return (_squadsAtStart & (1u << squad)) != 0; };
    auto const eliminated = [this](uint8 squad)
    {
        return std::find(_eliminatedSquads.begin(), _eliminatedSquads.end(), squad) != _eliminatedSquads.end();
    };

    place[winnerSquad] = 0;
    uint32 next = 1;
    bool survivors = false;
    for (uint8 squad : squads)
    {
        if (squad != winnerSquad && tookPart(squad) && !eliminated(squad))
        {
            place[squad] = next;
            survivors = true;
        }
    }
    if (survivors)
        ++next;
    for (auto itr = _eliminatedSquads.rbegin(); itr != _eliminatedSquads.rend(); ++itr)
        if (*itr != winnerSquad)
            place[*itr] = next++;
    for (uint8 squad : squads)
        if (squad != winnerSquad && !tookPart(squad))
            place[squad] = next;

    // Everyone's opponents are read before any rating moves.
    std::vector<ArenaTeam::PlacedOpponent> opponents[MAX_BATTLEGROUND_SQUADS];
    for (uint8 squad : squads)
    {
        for (uint8 other : squads)
        {
            if (other == squad)
                continue;

            float const score = place[squad] < place[other] ? 1.0f : (place[squad] > place[other] ? 0.0f : 0.5f);
            opponents[squad].push_back({ GetSquadMatchmakerRating(other), score });
        }

        // The scoreboard sets each squad against the winner, and the winner
        // against whoever finished closest to it.
        uint8 rival = BATTLEGROUND_NO_SQUAD;
        for (uint8 other : squads)
            if (other != squad && (rival == BATTLEGROUND_NO_SQUAD || place[other] < place[rival]))
                rival = other;
        m_SquadScoreboardRival[squad] = rival;
    }

    for (uint8 squad : squads)
    {
        ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(GetSquadArenaTeamId(squad));
        bool const won = squad == winnerSquad;
        int32 ratingChange = 0;
        int32 const matchmakerChange = arenaTeam->PlacedAgainst(GetSquadMatchmakerRating(squad), opponents[squad], won, ratingChange);
        _squadArenaScores[squad].Assign(ratingChange, GetSquadMatchmakerRating(squad), arenaTeam->GetName());

        TC_LOG_DEBUG("bg.arena", "Four Corners rated: squad {} ({}) team {} placed {} of {}, rating {:+}, MMR {:+}.",
            uint32(squad), GetSquadName(squad), arenaTeam->GetId(), place[squad] + 1, uint32(squads.size()), ratingChange, matchmakerChange);

        for (auto const& [guid, participant] : m_Players)
        {
            if (GetPlayerSquad(guid) != squad)
                continue;

            Player* player = _GetPlayer(guid, participant.OfflineRemoveTime != 0, "Arena::EndRatedSquadMatch");
            if (player && won)
            {
                // achievement BEFORE the personal rating moves, as for a two-team win
                uint32 const rating = player->GetArenaPersonalRating(arenaTeam->GetSlot());
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_RATED_ARENA, rating ? rating : 1);
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_ARENA, GetMapId());
            }
            else if (player)
                player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_NO_LOSE, 0);

            arenaTeam->MemberPlaced(player, guid, opponents[squad], matchmakerChange, won);
        }

        arenaTeam->SaveToDB();
        arenaTeam->NotifyStatsChanged();
    }
}

void Arena::EndBattleground(uint32 winner)
{
    // A rated squad match is settled by finishing place, not by two sides.
    if (isRated() && IsSquadMatch())
    {
        EndRatedSquadMatch();
        Battleground::EndBattleground(winner);
        return;
    }

    // One arena team against clones: the stock calculation below needs a team
    // on both sides and would rate nothing at all.
    if (isRated() && GetBotFillRatedTeam())
    {
        EndBotFilledRatedMatch(winner);
        Battleground::EndBattleground(winner);
        return;
    }

    // arena rating calculation
    if (isRated())
    {
        uint32 loserTeamRating        = 0;
        uint32 loserMatchmakerRating  = 0;
        int32  loserChange            = 0;
        int32  loserMatchmakerChange  = 0;
        uint32 winnerTeamRating       = 0;
        uint32 winnerMatchmakerRating = 0;
        int32  winnerChange           = 0;
        int32  winnerMatchmakerChange = 0;

        // In case of arena draw, follow this logic:
        // winnerArenaTeam => ALLIANCE, loserArenaTeam => HORDE
        ArenaTeam* winnerArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(winner == 0 ? uint32(ALLIANCE) : winner));
        ArenaTeam* loserArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(winner == 0 ? uint32(HORDE) : GetOtherTeam(winner)));

        if (winnerArenaTeam && loserArenaTeam && winnerArenaTeam != loserArenaTeam)
        {
            // In case of arena draw, follow this logic:
            // winnerMatchmakerRating => ALLIANCE, loserMatchmakerRating => HORDE
            loserTeamRating = loserArenaTeam->GetRating();
            loserMatchmakerRating = GetArenaMatchmakerRating(winner == 0 ? uint32(HORDE) : GetOtherTeam(winner));
            winnerTeamRating = winnerArenaTeam->GetRating();
            winnerMatchmakerRating = GetArenaMatchmakerRating(winner == 0 ? uint32(ALLIANCE) : winner);

            if (winner != 0)
            {
                winnerMatchmakerChange = winnerArenaTeam->WonAgainst(winnerMatchmakerRating, loserMatchmakerRating, winnerChange);
                loserMatchmakerChange = loserArenaTeam->LostAgainst(loserMatchmakerRating, winnerMatchmakerRating, loserChange);

                TC_LOG_DEBUG("bg.arena", "match Type: {} --- Winner: old rating: {}, rating gain: {}, old MMR: {}, MMR gain: {} --- Loser: old rating: {}, rating loss: {}, old MMR: {}, MMR loss: {} ---",
                    GetArenaType(), winnerTeamRating, winnerChange, winnerMatchmakerRating, winnerMatchmakerChange,
                    loserTeamRating, loserChange, loserMatchmakerRating, loserMatchmakerChange);

                SetArenaMatchmakerRating(winner, winnerMatchmakerRating + winnerMatchmakerChange);
                SetArenaMatchmakerRating(GetOtherTeam(winner), loserMatchmakerRating + loserMatchmakerChange);

                // bg team that the client expects is different to TeamId
                // alliance 1, horde 0
                uint8 winnerTeam = winner == ALLIANCE ? PVP_TEAM_ALLIANCE : PVP_TEAM_HORDE;
                uint8 loserTeam = winner == ALLIANCE ? PVP_TEAM_HORDE : PVP_TEAM_ALLIANCE;

                _arenaTeamScores[winnerTeam].Assign(winnerChange, winnerMatchmakerRating, winnerArenaTeam->GetName());
                _arenaTeamScores[loserTeam].Assign(loserChange, loserMatchmakerRating, loserArenaTeam->GetName());

                TC_LOG_DEBUG("bg.arena", "Arena match Type: {} for Team1Id: {} - Team2Id: {} ended. WinnerTeamId: {}. Winner rating: +{}, Loser rating: {}",
                    GetArenaType(), GetArenaTeamIdByIndex(TEAM_ALLIANCE), GetArenaTeamIdByIndex(TEAM_HORDE), winnerArenaTeam->GetId(), winnerChange, loserChange);

                if (sWorld->getBoolConfig(CONFIG_ARENA_LOG_EXTENDED_INFO))
                    for (auto const& score : PlayerScores)
                        if (Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(HighGuid::Player, score.first)))
                        {
                            TC_LOG_DEBUG("bg.arena", "Statistics match Type: {} for {} (GUID: {}, Team: {}, IP: {}): {}",
                                GetArenaType(), player->GetName(), score.first, player->GetArenaTeamId(GetArenaType() == 5 ? 2 : GetArenaType() == 3),
                                player->GetSession()->GetRemoteAddress(), score.second->ToString());
                        }
            }
            // Deduct 16 points from each teams arena-rating if there are no winners after 45+2 minutes
            else
            {
                _arenaTeamScores[PVP_TEAM_ALLIANCE].Assign(ARENA_TIMELIMIT_POINTS_LOSS, winnerMatchmakerRating, winnerArenaTeam->GetName());
                _arenaTeamScores[PVP_TEAM_HORDE].Assign(ARENA_TIMELIMIT_POINTS_LOSS, loserMatchmakerRating, loserArenaTeam->GetName());

                winnerArenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS);
                loserArenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS);
            }

            uint8 aliveWinners = GetArenaAlivePlayersCountByTeam(winner);

            for (auto const& i : GetPlayers())
            {
                uint32 team = i.second.Team;

                if (i.second.OfflineRemoveTime)
                {
                    // if rated arena match - make member lost!
                    if (team == winner)
                        winnerArenaTeam->OfflineMemberLost(i.first, loserMatchmakerRating, winnerMatchmakerChange);
                    else
                    {
                        if (winner == 0)
                            winnerArenaTeam->OfflineMemberLost(i.first, loserMatchmakerRating, winnerMatchmakerChange);

                        loserArenaTeam->OfflineMemberLost(i.first, winnerMatchmakerRating, loserMatchmakerChange);
                    }
                    continue;
                }

                Player* player = _GetPlayer(i.first, i.second.OfflineRemoveTime != 0, "Arena::EndBattleground");
                if (!player)
                    continue;

                // per player calculation
                if (team == winner)
                {
                    // update achievement BEFORE personal rating update
                    uint32 rating = player->GetArenaPersonalRating(winnerArenaTeam->GetSlot());
                    player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_RATED_ARENA, rating ? rating : 1);
                    player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_ARENA, GetMapId());

                    // Last standing - Rated 5v5 arena & be solely alive player
                    if (GetArenaType() == ARENA_TYPE_5v5 && aliveWinners == 1 && player->IsAlive())
                        player->CastSpell(player, SPELL_LAST_MAN_STANDING, true);

                    winnerArenaTeam->MemberWon(player, loserMatchmakerRating, winnerMatchmakerChange);
                }
                else
                {
                    if (winner == 0)
                        winnerArenaTeam->MemberLost(player, loserMatchmakerRating, winnerMatchmakerChange);

                    loserArenaTeam->MemberLost(player, winnerMatchmakerRating, loserMatchmakerChange);

                    // Arena lost => reset the win_rated_arena having the "no_lose" condition
                    player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_NO_LOSE, 0);
                }
            }

            // update previous opponents for arena queue
            winnerArenaTeam->SetPreviousOpponents(loserArenaTeam->GetId());
            loserArenaTeam->SetPreviousOpponents(winnerArenaTeam->GetId());

            // save the stat changes
            winnerArenaTeam->SaveToDB();
            loserArenaTeam->SaveToDB();
            // send updated arena team stats to players
            // this way all arena team members will get notified, not only the ones who participated in this match
            winnerArenaTeam->NotifyStatsChanged();
            loserArenaTeam->NotifyStatsChanged();
        }
    }

    // end battleground
    Battleground::EndBattleground(winner);
}

// The stock calculation above, for a match the people's team played against
// clones. The clones' side is one of two things (BattlegroundQueue::
// StartBotFilledRatedMatches):
//  - a team of the bot ladder, whose members the clones were copies of: the
//    two teams are rated against each other as stock rates two teams, the bot
//    team's members as members who were not there to be handed it;
//  - no team at all: it was given the people's own matchmaker rating, so a win
//    or a loss moves them as one against an even opponent would, and there is
//    no previous opponent to remember.
// The time limit is the stock draw.
//
// No contest - no rating change for anybody - when the team never had a clone
// to fight (the gates opened on an empty side, which CheckWinConditions would
// otherwise hand it as a win), or when the match was ended without a winner
// for any reason but the time limit (a GM, the playerbot module's own empty-
// match rules, which say PVP_TEAM_NEUTRAL).
void Arena::EndBotFilledRatedMatch(uint32 winner)
{
    uint32 const team = GetBotFillRatedTeam();
    uint32 const clones = GetOtherTeam(team);
    ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(team));
    ArenaTeam* botTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(clones));
    std::string const botTeamName = botTeam ? botTeam->GetName() : std::string("Bot Team");

    // bg team that the client expects is different to TeamId: alliance 1, horde 0
    uint8 const teamScore = team == ALLIANCE ? PVP_TEAM_ALLIANCE : PVP_TEAM_HORDE;
    uint8 const cloneScore = team == ALLIANCE ? PVP_TEAM_HORDE : PVP_TEAM_ALLIANCE;

    uint32 const ownMatchmakerRating = GetArenaMatchmakerRating(team);
    uint32 const cloneMatchmakerRating = GetArenaMatchmakerRating(clones);

    bool const won = winner == team;
    bool const lost = winner == clones;
    bool const draw = winner == 0 && GetStatus() == STATUS_IN_PROGRESS && GetStartTime() >= 20 * MINUTE * IN_MILLISECONDS;

    if (!arenaTeam || !HasBotFillOpponentsFielded() || (!won && !lost && !draw))
    {
        _arenaTeamScores[teamScore].Assign(0, ownMatchmakerRating, arenaTeam ? arenaTeam->GetName() : "");
        _arenaTeamScores[cloneScore].Assign(0, cloneMatchmakerRating, botTeamName);
        TC_LOG_WARN("bg.arena", "Arena::EndBotFilledRatedMatch: instance {} arena team {} - no contest (winner {}, opponents fielded {}); no rating change.",
            GetInstanceID(), GetArenaTeamIdForTeam(team), winner, HasBotFillOpponentsFielded() ? "yes" : "no");
        return;
    }

    int32 ratingChange = 0;
    int32 matchmakerChange = 0;
    if (won)
        matchmakerChange = arenaTeam->WonAgainst(ownMatchmakerRating, cloneMatchmakerRating, ratingChange);
    else if (lost)
        matchmakerChange = arenaTeam->LostAgainst(ownMatchmakerRating, cloneMatchmakerRating, ratingChange);
    else
    {
        ratingChange = ARENA_TIMELIMIT_POINTS_LOSS;
        arenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS);
    }

    SetArenaMatchmakerRating(team, ownMatchmakerRating + matchmakerChange);
    _arenaTeamScores[teamScore].Assign(ratingChange, ownMatchmakerRating, arenaTeam->GetName());

    int32 botRatingChange = 0;
    if (botTeam)
    {
        int32 botMatchmakerChange = 0;
        if (won)
            botMatchmakerChange = botTeam->LostAgainst(cloneMatchmakerRating, ownMatchmakerRating, botRatingChange);
        else if (lost)
            botMatchmakerChange = botTeam->WonAgainst(cloneMatchmakerRating, ownMatchmakerRating, botRatingChange);
        else
        {
            botRatingChange = ARENA_TIMELIMIT_POINTS_LOSS;
            botTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS);
        }

        std::vector<ObjectGuid> botMembers;
        for (auto itr = botTeam->m_membersBegin(); itr != botTeam->m_membersEnd(); ++itr)
            botMembers.push_back(itr->Guid);
        for (ObjectGuid const& member : botMembers)
        {
            if (lost)
                botTeam->OfflineMemberWon(member, ownMatchmakerRating, botMatchmakerChange);
            else
                botTeam->OfflineMemberLost(member, ownMatchmakerRating, botMatchmakerChange);
        }

        SetArenaMatchmakerRating(clones, cloneMatchmakerRating + botMatchmakerChange);
        arenaTeam->SetPreviousOpponents(botTeam->GetId());
        botTeam->SetPreviousOpponents(arenaTeam->GetId());
        botTeam->SaveToDB();
        botTeam->NotifyStatsChanged();
    }
    _arenaTeamScores[cloneScore].Assign(botRatingChange, cloneMatchmakerRating, botTeamName);

    TC_LOG_DEBUG("bg.arena", "Arena::EndBotFilledRatedMatch: {}v{} instance {} arena team {} {} against {}: rating {:+} / {:+}, MMR {} {:+}.",
        GetArenaType(), GetArenaType(), GetInstanceID(), arenaTeam->GetId(), won ? "won" : lost ? "lost" : "drew",
        botTeamName, ratingChange, botRatingChange, ownMatchmakerRating, matchmakerChange);

    uint8 const aliveWinners = won ? GetArenaAlivePlayersCountByTeam(team) : 0;

    for (auto const& [guid, participant] : GetPlayers())
    {
        if (participant.Team != team)
            continue;                               // the clones - their team, if any, is rated above

        if (participant.OfflineRemoveTime)
        {
            arenaTeam->OfflineMemberLost(guid, cloneMatchmakerRating, matchmakerChange);
            continue;
        }

        // a clone dealt in beside the people is in no team and earns nothing
        Player* player = _GetPlayer(guid, false, "Arena::EndBotFilledRatedMatch");
        if (!player || IsBotParticipantSession(player->GetSession()))
            continue;

        if (won)
        {
            // update achievement BEFORE personal rating update
            uint32 const rating = player->GetArenaPersonalRating(arenaTeam->GetSlot());
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_RATED_ARENA, rating ? rating : 1);
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_ARENA, GetMapId());

            // Last standing - Rated 5v5 arena & be solely alive player
            if (GetArenaType() == ARENA_TYPE_5v5 && aliveWinners == 1 && player->IsAlive())
                player->CastSpell(player, SPELL_LAST_MAN_STANDING, true);

            arenaTeam->MemberWon(player, cloneMatchmakerRating, matchmakerChange);
        }
        else
        {
            arenaTeam->MemberLost(player, cloneMatchmakerRating, matchmakerChange);

            // Arena lost => reset the win_rated_arena having the "no_lose" condition
            player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_NO_LOSE, 0);
        }
    }

    // The team's own bots (the playerbot module's teammates for it) fought as
    // copies under other guids, and are rated as members who were not there
    // to be handed it.
    std::vector<ObjectGuid> absent;
    for (auto itr = arenaTeam->m_membersBegin(); itr != arenaTeam->m_membersEnd(); ++itr)
        if (!m_Players.count(itr->Guid) && WasRosterBotDealt(itr->Guid))
            absent.push_back(itr->Guid);
    for (ObjectGuid const& member : absent)
    {
        if (won)
            arenaTeam->OfflineMemberWon(member, cloneMatchmakerRating, matchmakerChange);
        else
            arenaTeam->OfflineMemberLost(member, cloneMatchmakerRating, matchmakerChange);
    }

    arenaTeam->SaveToDB();
    arenaTeam->NotifyStatsChanged();
}

// ---------------------------------------------------------------- buffle shuffle
//
// See SOLO_SHUFFLE_MAX_ROUNDS in Arena.h for the shape of the match. The rounds are
// driven from Battleground::Update through UpdateRounds, and a round is decided
// by CheckWinConditions, the same call every arena makes on a death or a leave.

namespace
{
    // From a round being decided to the next one's teams being dealt: long
    // enough to see who won it.
    constexpr uint32 SHUFFLE_INTERMISSION_MS = 4 * IN_MILLISECONDS;
    // Behind the gates before every round after the first: time to buff and
    // call a pet again, every cast instant and free (SPELL_ARENA_PREPARATION).
    constexpr uint32 SHUFFLE_PREPARATION_MS = 20 * IN_MILLISECONDS;
    // A round still undecided this long goes to the side with the most still
    // standing, then the most health between them; level is a draw.
    constexpr uint32 SHUFFLE_ROUND_TIME_LIMIT_MS = 5 * MINUTE * IN_MILLISECONDS;
    // More than this many in one shuffle cannot happen through the queue or the
    // bot fill (three a side); past it the sides are simply kept every round.
    constexpr uint32 SHUFFLE_MAX_ROSTER = 10;
    // Whole schedules weighed against each other before falling back to dealing
    // one round at a time. Six players have 210 at most.
    constexpr uint64 SHUFFLE_MAX_SCHEDULES = 20000;

    // n choose k, stopping as soon as it passes SHUFFLE_MAX_SCHEDULES.
    uint64 CountSchedules(uint32 n, uint32 k)
    {
        uint64 count = 1;
        for (uint32 i = 1; i <= k; ++i)
        {
            count = count * (n - k + i) / i;
            if (count > SHUFFLE_MAX_SCHEDULES)
                break;
        }
        return count;
    }

    uint32 CountBits(uint32 mask)
    {
        uint32 count = 0;
        for (; mask; mask &= mask - 1)
            ++count;
        return count;
    }

    // The different ways to deal a roster's teams, each once: gold gets n / 2
    // (so an odd roster plays one short on gold), and with an even roster a
    // split and its mirror are the same two teams, counted once as the one with
    // member 0 on gold. Only the splits that spread the healers as evenly as
    // they will go - with two, one each. Empty past SHUFFLE_MAX_ROSTER.
    std::vector<uint32> ShuffleSplits(std::vector<bool> const& healers)
    {
        std::vector<uint32> splits;
        uint32 const n = uint32(healers.size());
        if (n < 2 || n > SHUFFLE_MAX_ROSTER)
            return splits;

        uint32 const full = (1u << n) - 1;
        uint32 healerMask = 0;
        for (uint32 i = 0; i < n; ++i)
            if (healers[i])
                healerMask |= 1u << i;
        uint32 const healerCount = CountBits(healerMask);
        auto const imbalance = [healerMask, healerCount](uint32 mask)
        {
            int32 const gold = int32(CountBits(mask & healerMask));
            return uint32(std::abs(gold - (int32(healerCount) - gold)));
        };

        uint32 bestImbalance = ~0u;
        for (uint32 mask = 1; mask < full; ++mask)
        {
            if (CountBits(mask) != n / 2 || (n % 2 == 0 && !(mask & 1)))
                continue;
            splits.push_back(mask);
            bestImbalance = std::min(bestImbalance, imbalance(mask));
        }
        splits.erase(std::remove_if(splits.begin(), splits.end(),
            [&](uint32 mask) { return imbalance(mask) != bestImbalance; }), splits.end());
        return splits;
    }

    // Deals the rounds of a shuffle. Roster member i plays a round on the gold
    // (Alliance) side when bit i of its mask is set.
    //
    // A round uses one of ShuffleSplits, and the rounds are chosen together,
    // never repeating a split while an unplayed one is left: of every such set,
    // the one that gives the two players who share a team least often the most
    // rounds together - so everybody plays beside everybody else at least once
    // whenever that can be done - then the fewest repeats of the most frequent
    // pairing, then the most even spread. Picking round by round instead left
    // about one 3v3 in eight with a pair that never met. `played` are the rounds
    // already fought, which count towards all of it; `first`, when it is one of
    // the allowed splits, opens what is dealt. Rounds beyond the unplayed splits
    // are dealt one at a time on the same terms.
    std::vector<uint32> DealShuffleRounds(std::vector<bool> const& healers, std::vector<uint32> const& played,
        uint32 first, uint32 rounds)
    {
        std::vector<uint32> dealt;
        uint32 const n = uint32(healers.size());
        if (!rounds)
            return dealt;

        if (n < 2 || n > SHUFFLE_MAX_ROSTER)
        {
            dealt.assign(rounds, first ? first : (played.empty() ? 0 : played.back()));
            return dealt;
        }

        uint32 const full = (1u << n) - 1;
        // A split and its mirror (an even roster) are counted as one.
        auto const canonical = [n, full](uint32 mask) { return (n % 2 == 0 && !(mask & 1)) ? (full & ~mask) : mask; };

        std::vector<uint32> splits = ShuffleSplits(healers);
        Trinity::Containers::RandomShuffle(splits);

        auto const sameSide = [](uint32 mask, uint32 a, uint32 b) { return ((mask >> a) & 1) == ((mask >> b) & 1); };

        // The pairs some allowed split puts on one team. Two healers kept apart
        // never are, and do not count against a schedule.
        std::vector<std::pair<uint32, uint32>> pairs;
        for (uint32 a = 0; a < n; ++a)
            for (uint32 b = a + 1; b < n; ++b)
                if (std::any_of(splits.begin(), splits.end(), [&](uint32 mask) { return sameSide(mask, a, b); }))
                    pairs.emplace_back(a, b);

        std::vector<uint32> together(pairs.size(), 0);
        std::vector<uint32> used;
        auto const addPairs = [&](std::vector<uint32>& counts, uint32 mask)
        {
            for (size_t i = 0; i < pairs.size(); ++i)
                if (sameSide(mask, pairs[i].first, pairs[i].second))
                    ++counts[i];
        };
        auto const record = [&](uint32 mask)
        {
            addPairs(together, mask);
            used.push_back(canonical(mask & full));
        };
        // How good a set of pair counts is: the pair that teams least, as often
        // as possible; then the pair that teams most, as rarely as possible;
        // then the least uneven. True when `counts` beats the best so far.
        uint32 bestLeast = 0;
        uint32 bestMost = 0;
        uint64 bestSquares = 0;
        bool haveBest = false;
        auto const better = [&](std::vector<uint32> const& counts)
        {
            uint32 least = counts.empty() ? 0 : *std::min_element(counts.begin(), counts.end());
            uint32 most = counts.empty() ? 0 : *std::max_element(counts.begin(), counts.end());
            uint64 squares = 0;
            for (uint32 count : counts)
                squares += uint64(count) * count;

            if (haveBest && (least < bestLeast || (least == bestLeast && (most > bestMost ||
                (most == bestMost && squares >= bestSquares)))))
                return false;

            haveBest = true;
            bestLeast = least;
            bestMost = most;
            bestSquares = squares;
            return true;
        };
        // Which of the two teams wears gold changes nothing about who plays with
        // whom; toss for it, so nobody always starts from one end.
        auto const deal = [&](uint32 mask)
        {
            if (n % 2 == 0 && roll_chance_i(50))
                mask = full & ~mask;
            dealt.push_back(mask);
            record(mask);
        };

        for (uint32 mask : played)
            record(mask);

        if (first && std::find(splits.begin(), splits.end(), canonical(first & full)) != splits.end())
        {
            dealt.push_back(first & full);
            record(first);
        }

        // Every set of unplayed splits as large as what is left to deal (the
        // splits are in random order, so equal sets are a toss-up).
        std::vector<uint32> unused;
        for (uint32 mask : splits)
            if (std::find(used.begin(), used.end(), mask) == used.end())
                unused.push_back(mask);

        uint32 const need = rounds > dealt.size() ? uint32(rounds - dealt.size()) : 0;
        uint32 const take = std::min<uint32>(need, uint32(unused.size()));
        if (take && CountSchedules(uint32(unused.size()), take) <= SHUFFLE_MAX_SCHEDULES)
        {
            std::vector<uint32> pick(take);
            for (uint32 i = 0; i < take; ++i)
                pick[i] = i;

            std::vector<uint32> best;
            std::vector<uint32> counts;
            for (;;)
            {
                counts = together;
                for (uint32 index : pick)
                    addPairs(counts, unused[index]);
                if (better(counts))
                    best = pick;

                // the next combination, in order
                int32 i = int32(take) - 1;
                while (i >= 0 && pick[i] == uint32(unused.size()) - take + uint32(i))
                    --i;
                if (i < 0)
                    break;
                ++pick[i];
                for (uint32 j = uint32(i) + 1; j < take; ++j)
                    pick[j] = pick[j - 1] + 1;
            }

            std::vector<uint32> chosen;
            for (uint32 index : best)
                chosen.push_back(unused[index]);
            Trinity::Containers::RandomShuffle(chosen);
            for (uint32 mask : chosen)
                deal(mask);
        }

        while (dealt.size() < rounds)
        {
            // One at a time: the split played least so far, then as above.
            haveBest = false;
            uint32 bestMask = 0;
            uint32 bestUses = ~0u;
            std::vector<uint32> counts;
            for (uint32 mask : splits)
            {
                uint32 const uses = uint32(std::count(used.begin(), used.end(), mask));
                if (uses > bestUses)
                    continue;
                if (uses < bestUses)
                {
                    haveBest = false;
                    bestUses = uses;
                }

                counts = together;
                addPairs(counts, mask);
                if (better(counts))
                    bestMask = mask;
            }

            deal(bestMask);
        }

        return dealt;
    }

    std::string ShuffleName(ObjectGuid guid, std::unordered_map<ObjectGuid, Player*> const& players)
    {
        auto const itr = players.find(guid);
        if (itr != players.end())
            return Battleground::GetPlayerDisplayName(itr->second);
        if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid))
            return Surnames::Decorated(guid, entry->Name);
        return "someone";
    }

    // "A", "A and B", "A, B and C".
    std::string JoinNames(std::vector<std::string> const& names)
    {
        std::string joined;
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (i)
                joined += i + 1 == names.size() ? " and " : ", ";
            joined += names[i];
        }
        return joined;
    }
}

bool Arena::IsSoloShuffleHealer(Player const* player)
{
    if (!player)
        return false;

    std::map<uint32, uint32> pointsByTab;
    for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
    {
        TalentEntry const* talent = sTalentStore.LookupEntry(talentId);
        if (!talent)
            continue;

        for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
        {
            if (talent->SpellRank[rank] && player->HasTalent(talent->SpellRank[rank], player->GetActiveSpec()))
            {
                pointsByTab[talent->TabID] += uint32(rank) + 1;
                break;
            }
        }
    }

    return IsHealerTalentSpend(pointsByTab);
}

bool Arena::IsHealerTalentSpend(std::map<uint32, uint32> const& pointsByTab)
{
    // TalentTab.dbc's healing trees: Discipline and Holy (priest), Holy
    // (paladin), Restoration (shaman, druid). The same ids the AzerothCore
    // solo queue sorts roles by.
    static constexpr uint32 HealingTabs[] = { 201, 202, 382, 262, 282 };

    uint32 bestTab = 0;
    uint32 bestPoints = 0;
    for (auto const& [tab, points] : pointsByTab)
    {
        if (points > bestPoints)
        {
            bestTab = tab;
            bestPoints = points;
        }
    }

    return bestPoints && std::find(std::begin(HealingTabs), std::end(HealingTabs), bestTab) != std::end(HealingTabs);
}

bool Arena::IsBetweenRounds() const
{
    return IsSoloShuffle() && (_shufflePhase == ShufflePhase::Intermission || _shufflePhase == ShufflePhase::Preparation);
}

bool Arena::OwnsPreparationState(Player const* /*player*/) const
{
    // The playerbots hold where they stand while this is true, and keep the
    // preparation aura that has them buff and call their pets rather than fight.
    return IsBetweenRounds();
}

bool Arena::IsWinningParticipant(ObjectGuid guid, uint32 team, uint32 winner) const
{
    if (IsSoloShuffle())
        return WonSoloShuffle(guid);

    return Battleground::IsWinningParticipant(guid, team, winner);
}

bool Arena::WonSoloShuffle(ObjectGuid guid) const
{
    auto const itr = _shuffleWins.find(guid);
    return itr != _shuffleWins.end() && _shuffleRoundsPlayed && itr->second * 2 > _shuffleRoundsPlayed;
}

uint32 Arena::GetShuffleSideMask() const
{
    uint32 mask = 0;
    for (size_t i = 0; i < _shuffleRoster.size() && i < 32; ++i)
        if (GetPlayerTeam(_shuffleRoster[i]) == ALLIANCE)
            mask |= 1u << i;
    return mask;
}

std::string Arena::GetShuffleTeams(uint32 team) const
{
    SquadFeedPlayers const players = GetSquadFeedPlayers();
    std::vector<std::string> names;
    for (ObjectGuid guid : _shuffleRoster)
        if (GetPlayerTeam(guid) == team)
            names.push_back(ShuffleName(guid, players));
    return names.empty() ? std::string("nobody") : JoinNames(names);
}

std::string Arena::GetShuffleStandings() const
{
    SquadFeedPlayers const players = GetSquadFeedPlayers();
    std::vector<std::pair<uint32, std::string>> standings;
    for (ObjectGuid guid : _shuffleRoster)
    {
        auto const wins = _shuffleWins.find(guid);
        standings.emplace_back(wins != _shuffleWins.end() ? wins->second : 0, ShuffleName(guid, players));
    }

    std::stable_sort(standings.begin(), standings.end(),
        [](std::pair<uint32, std::string> const& left, std::pair<uint32, std::string> const& right) { return left.first > right.first; });

    std::string line;
    for (auto const& [wins, name] : standings)
    {
        if (!line.empty())
            line += ", ";
        line += Trinity::StringFormat("{} {}", name, wins);
    }
    return line;
}

void Arena::UpdateRounds(uint32 diff)
{
    switch (_shufflePhase)
    {
        case ShufflePhase::NotStarted:
            // SkipStartDelay asks CheckWinConditions as the gates open, which
            // begins the shuffle; this only catches a start that went round it.
            if (GetStatus() == STATUS_IN_PROGRESS)
                CheckShuffleRound();
            break;
        case ShufflePhase::Fighting:
            _shuffleTimer += diff;
            if (_shuffleTimer >= SHUFFLE_ROUND_TIME_LIMIT_MS)
                EndShuffleRoundAtTimeLimit();
            break;
        case ShufflePhase::Intermission:
            if (_shuffleTimer > diff)
                _shuffleTimer -= diff;
            else
                PrepareShuffleRound();
            break;
        case ShufflePhase::Preparation:
            KeepPlayersAtStarts(diff);
            if (_shuffleTimer > diff)
            {
                _shuffleTimer -= diff;
                if (!_shuffleTenSecondsSent && _shuffleTimer <= 10 * IN_MILLISECONDS)
                {
                    _shuffleTenSecondsSent = true;
                    SendBroadcastText(ARENA_TEXT_START_TEN_SECONDS, CHAT_MSG_BG_SYSTEM_NEUTRAL);
                }
            }
            else
                StartShuffleRound();
            break;
        default:
            break;
    }
}

void Arena::CheckShuffleRound()
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    if (_shufflePhase == ShufflePhase::NotStarted)
        BeginSoloShuffle();

    if (_shufflePhase != ShufflePhase::Fighting)
        return;

    uint32 const goldStanding = GetArenaAlivePlayersCountByTeam(ALLIANCE);
    uint32 const greenStanding = GetArenaAlivePlayersCountByTeam(HORDE);
    if (goldStanding && greenStanding)
        return;

    if (!goldStanding && !greenStanding)
        EndShuffleRound(0, "Both teams fell together. ");
    else
        EndShuffleRound(goldStanding ? ALLIANCE : HORDE, "");
}

void Arena::BeginSoloShuffle()
{
    _shuffleRoster.clear();
    _shuffleWins.clear();
    _shuffleHealers.clear();
    _shuffleRoundsPlayed = 0;

    // Roles count only with Centurion.SoloShuffle.BalanceHealers on; otherwise
    // nobody is dealt as a healer.
    bool const balanceHealers = sWorld->getBoolConfig(CONFIG_CENTURION_SOLO_SHUFFLE_BALANCE_HEALERS);
    SquadFeedPlayers const players = GetSquadFeedPlayers();
    std::vector<bool> healers;
    for (auto const& [guid, participant] : m_Players)
    {
        (void)participant;
        auto const itr = players.find(guid);
        bool const healer = balanceHealers && itr != players.end() && IsSoloShuffleHealer(itr->second);
        _shuffleRoster.push_back(guid);
        _shuffleWins[guid] = 0;
        healers.push_back(healer);
        if (healer)
            _shuffleHealers.insert(guid);
    }

    if (_shuffleRoster.size() < 2)
    {
        // Nobody to play against: nobody came for the other seats.
        _shufflePhase = ShufflePhase::Finished;
        AnnounceToArena(this, "Nobody else came to the shuffle. The match is over.");
        EndBattleground(0);
        return;
    }

    // As many rounds as there are different ways to deal the teams, up to the
    // most a match plays: six for a full 3v3, three for a 2v2 (two with two
    // healers kept apart). Kept for the whole match, whoever leaves.
    size_t const splits = ShuffleSplits(healers).size();
    _shuffleRoundCount = uint8(splits ? std::min<size_t>(splits, SOLO_SHUFFLE_MAX_ROUNDS) : SOLO_SHUFFLE_MAX_ROUNDS);

    // Round 1 is fought as the queue and the bot fill seated it, unless that
    // left the sides uneven or the healers on one side.
    uint32 const seated = GetShuffleSideMask();
    _shuffleSchedule = DealShuffleRounds(healers, {}, seated, _shuffleRoundCount);
    _shuffleRound = 1;

    if (_shuffleSchedule.front() != seated)
    {
        for (size_t i = 0; i < _shuffleRoster.size(); ++i)
        {
            ObjectGuid const guid = _shuffleRoster[i];
            uint32 const team = (_shuffleSchedule.front() & (1u << i)) ? ALLIANCE : HORDE;
            if (GetPlayerTeam(guid) == team)
                continue;

            auto const itr = players.find(guid);
            Player* player = itr != players.end() ? itr->second : nullptr;
            MoveParticipantToTeam(guid, player, team);
            if (!player)
                continue;

            ApplyArenaFlag(player);
            SendToShuffleStart(player);
            SendInProgressStatusTo(player);
        }
        UpdateArenaWorldState();
    }

    _shufflePhase = ShufflePhase::Fighting;
    _shuffleTimer = 0;

    TC_LOG_DEBUG("bg.arena", "Buffle Shuffle: instance {} on map {} ({}v{}) begins with {} players ({} healers), {} rounds.",
        GetInstanceID(), GetMapId(), uint32(GetArenaType()), uint32(GetArenaType()), uint32(_shuffleRoster.size()),
        uint32(_shuffleHealers.size()), uint32(_shuffleRoundCount));

    AnnounceToArena(this, Trinity::StringFormat("Buffle Shuffle: {} rounds, and new teams before each one. Win more rounds than you lose.",
        uint32(_shuffleSchedule.size())));
    AnnounceToArena(this, Trinity::StringFormat("Round 1 - Gold: {}. Green: {}.", GetShuffleTeams(ALLIANCE), GetShuffleTeams(HORDE)));
}

void Arena::EndShuffleRound(uint32 winner, std::string const& why)
{
    ++_shuffleRoundsPlayed;

    for (ObjectGuid guid : _shuffleRoster)
        if (winner && GetPlayerTeam(guid) == winner)
            ++_shuffleWins[guid];

    uint32 const rounds = uint32(_shuffleSchedule.size());
    if (winner)
        AnnounceToArena(this, Trinity::StringFormat("{}{} wins round {} of {}: {}.", why, winner == ALLIANCE ? "Gold" : "Green",
            uint32(_shuffleRound), rounds, GetShuffleTeams(winner)));
    else
        AnnounceToArena(this, Trinity::StringFormat("{}Round {} of {} is a draw.", why, uint32(_shuffleRound), rounds));

    TC_LOG_DEBUG("bg.arena", "Buffle Shuffle: instance {} round {} -> {}.", GetInstanceID(), uint32(_shuffleRound),
        winner == ALLIANCE ? "gold" : (winner == HORDE ? "green" : "draw"));

    if (_shuffleRound >= rounds || _shuffleRoster.size() < 2)
    {
        FinishSoloShuffle();
        return;
    }

    AnnounceToArena(this, "Rounds won: " + GetShuffleStandings() + ".");
    _shufflePhase = ShufflePhase::Intermission;
    _shuffleTimer = SHUFFLE_INTERMISSION_MS;
}

void Arena::EndShuffleRoundAtTimeLimit()
{
    SquadFeedPlayers const players = GetSquadFeedPlayers();
    uint32 standing[PVP_TEAMS_COUNT] = { };
    float health[PVP_TEAMS_COUNT] = { };
    for (auto const& [guid, participant] : m_Players)
    {
        if (!IsArenaAlive(guid))
            continue;

        TeamId const side = GetTeamIndexByTeamId(participant.Team);
        ++standing[side];
        auto const itr = players.find(guid);
        if (itr != players.end())
            health[side] += itr->second->GetHealthPct();
    }

    uint32 winner = 0;
    if (standing[TEAM_ALLIANCE] != standing[TEAM_HORDE])
        winner = standing[TEAM_ALLIANCE] > standing[TEAM_HORDE] ? ALLIANCE : HORDE;
    else if (std::abs(health[TEAM_ALLIANCE] - health[TEAM_HORDE]) >= 1.0f)
        winner = health[TEAM_ALLIANCE] > health[TEAM_HORDE] ? ALLIANCE : HORDE;

    EndShuffleRound(winner, winner ? "Time is up, and the side with the most left standing takes it. " : "Time is up and neither side is ahead. ");
}

void Arena::ResetForShuffleRound(Player* player)
{
    // Spirit of Redemption is a shapeshift into a body that is not dead yet.
    if (player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
        player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

    if (!player->IsAlive())
    {
        player->ResurrectPlayer(1.0f);
        player->SpawnCorpseBones(false);
    }

    player->CombatStopWithPets(true);
    ClearCombatControl(player);

    // What coming into the arena did (Player::TeleportTo): the round's buffs
    // and debuffs off, and every cooldown back.
    player->RemoveArenaAuras();
    player->RemoveArenaSpellCooldowns(true);

    // What the round left lying about: totems, ground effects, traps, and every
    // summon but the pet.
    player->UnsummonAllTotems();
    player->RemoveAllDynObjects();
    player->RemoveAllGameObjects();

    Pet* pet = player->GetPet();
    std::vector<Unit*> const controlled(player->m_Controlled.begin(), player->m_Controlled.end());
    for (Unit* unit : controlled)
        if (unit && unit != pet && unit->GetTypeId() == TYPEID_UNIT && unit->IsSummon() && !unit->IsTotem())
            unit->ToTempSummon()->UnSummon();

    if (pet && pet->IsAlive())
    {
        pet->RemoveArenaAuras();
        pet->CombatStop(true);
        pet->SetFullHealth();
        for (uint8 i = POWER_MANA; i < MAX_POWERS; ++i)
            if (pet->GetMaxPower(Powers(i)))
                pet->SetFullPower(Powers(i));
    }
    else
        // A hunter's companion comes back with him; anyone else calls theirs
        // again behind the gates, where it costs nothing.
        player->ResurrectPetAtSpiritGuide();

    player->SetFullHealth();
    player->ResetAllPowers();
    player->ClearComboPoints();
}

void Arena::SendToShuffleStart(Player* player)
{
    Position const* start = GetStartPositionFor(player->GetGUID(), player->GetBGTeam());
    if (!start)
        return;

    // Pets first, as a start exit moves them (Battleground::_CheckStartExits):
    // already standing at the start, they are not sent away by their owner's
    // teleport for being out of range - and a bot's pet has nowhere to be
    // called back from.
    std::vector<Unit*> const controlled(player->m_Controlled.begin(), player->m_Controlled.end());
    for (Unit* unit : controlled)
        if (unit && unit->GetTypeId() != TYPEID_PLAYER && !unit->IsTotem() && unit->IsInWorld() && unit->IsAlive() && unit->GetMapId() == GetMapId())
            unit->NearTeleportTo(start->GetPositionX(), start->GetPositionY(), start->GetPositionZ(), start->GetOrientation());

    player->TeleportTo(GetMapId(), start->GetPositionX(), start->GetPositionY(), start->GetPositionZ(), start->GetOrientation());
}

void Arena::PrepareShuffleRound()
{
    SyncShuffleRoster();

    ++_shuffleRound;
    if (_shuffleRound > _shuffleSchedule.size() || _shuffleRoster.size() < 2)
    {
        FinishSoloShuffle();
        return;
    }

    uint32 const mask = _shuffleSchedule[_shuffleRound - 1];
    SquadFeedPlayers const players = GetSquadFeedPlayers();
    for (size_t i = 0; i < _shuffleRoster.size(); ++i)
    {
        ObjectGuid const guid = _shuffleRoster[i];
        uint32 const team = (mask & (1u << i)) ? ALLIANCE : HORDE;
        auto const itr = players.find(guid);
        Player* player = itr != players.end() ? itr->second : nullptr;

        if (player)
            ResetForShuffleRound(player);

        // Offline, their seat still moves, so they come back to the right side.
        MoveParticipantToTeam(guid, player, team);
        if (!player)
            continue;

        ApplyArenaFlag(player);
        SendToShuffleStart(player);
        player->CastSpell(player, SPELL_ARENA_PREPARATION, true);
        player->CastSpell(player, SPELL_INSTANT_CAST, true);
        player->ResetAllPowers();
        SendInProgressStatusTo(player);
    }

    ResetStartingAreaForRound();
    UpdateArenaWorldState();

    _shufflePhase = ShufflePhase::Preparation;
    _shuffleTimer = SHUFFLE_PREPARATION_MS;
    _shuffleTenSecondsSent = false;

    AnnounceToArena(this, Trinity::StringFormat("Round {} of {} - Gold: {}. Green: {}. The gates open in {} seconds.",
        uint32(_shuffleRound), uint32(_shuffleSchedule.size()), GetShuffleTeams(ALLIANCE), GetShuffleTeams(HORDE),
        SHUFFLE_PREPARATION_MS / IN_MILLISECONDS));
}

void Arena::StartShuffleRound()
{
    StartingEventOpenDoors();
    if (StartMessageIds[BG_STARTING_EVENT_FOURTH])
        SendBroadcastText(StartMessageIds[BG_STARTING_EVENT_FOURTH], CHAT_MSG_BG_SYSTEM_NEUTRAL);

    for (auto const& [guid, player] : GetSquadFeedPlayers())
        if (IsPlayerInBattleground(guid))
            ClearArenaPreparation(player);

    _shufflePhase = ShufflePhase::Fighting;
    _shuffleTimer = 0;

    // Somebody may have left while the gates were shut.
    CheckShuffleRound();
}

void Arena::FinishSoloShuffle()
{
    if (_shufflePhase == ShufflePhase::Finished || GetStatus() != STATUS_IN_PROGRESS)
        return;

    _shufflePhase = ShufflePhase::Finished;

    // The scoreboard sets the winners - more rounds won than lost - on gold
    // against everyone else on green, and the banner names gold if anyone won.
    SquadFeedPlayers const players = GetSquadFeedPlayers();
    std::vector<std::string> winners;
    for (auto const& [guid, participant] : m_Players)
    {
        (void)participant;
        bool const won = WonSoloShuffle(guid);
        if (won)
            winners.push_back(ShuffleName(guid, players));

        BattlegroundScoreMap::iterator const score = PlayerScores.find(guid.GetCounter());
        if (score != PlayerScores.end())
            if (ArenaScore* arenaScore = dynamic_cast<ArenaScore*>(score->second))
                arenaScore->TeamId = won ? PVP_TEAM_ALLIANCE : PVP_TEAM_HORDE;
    }

    AnnounceToArena(this, Trinity::StringFormat("The shuffle is over after {} rounds. Rounds won: {}.",
        _shuffleRoundsPlayed, GetShuffleStandings()));
    AnnounceToArena(this, winners.empty()
        ? std::string("Nobody won more rounds than they lost.")
        : Trinity::StringFormat("Won more rounds than they lost: {}.", JoinNames(winners)));

    TC_LOG_DEBUG("bg.arena", "Buffle Shuffle: instance {} finished after {} rounds, {} winner(s).",
        GetInstanceID(), _shuffleRoundsPlayed, uint32(winners.size()));

    EndBattleground(winners.empty() ? 0 : ALLIANCE);
}

void Arena::RedealRemainingShuffleRounds()
{
    std::vector<bool> healers;
    for (ObjectGuid guid : _shuffleRoster)
        healers.push_back(_shuffleHealers.count(guid) != 0);

    // Through the round being fought or prepared: its sides are already set.
    std::vector<uint32> kept(_shuffleSchedule.begin(),
        _shuffleSchedule.begin() + std::min<size_t>(_shuffleRound, _shuffleSchedule.size()));
    if (!kept.empty())
        kept.back() = GetShuffleSideMask();

    uint32 const remaining = _shuffleRoundCount > kept.size() ? uint32(_shuffleRoundCount - kept.size()) : 0;
    std::vector<uint32> const rest = DealShuffleRounds(healers, kept, 0, remaining);
    _shuffleSchedule = kept;
    _shuffleSchedule.insert(_shuffleSchedule.end(), rest.begin(), rest.end());
}

void Arena::DropFromShuffle(ObjectGuid guid)
{
    if (_shufflePhase == ShufflePhase::NotStarted || _shufflePhase == ShufflePhase::Finished)
        return;

    auto const itr = std::find(_shuffleRoster.begin(), _shuffleRoster.end(), guid);
    if (itr == _shuffleRoster.end())
        return;

    uint32 const index = uint32(itr - _shuffleRoster.begin());
    _shuffleRoster.erase(itr);
    _shuffleWins.erase(guid);
    _shuffleHealers.erase(guid);

    // The rounds played keep their sides, without the bit of whoever left.
    for (uint32& mask : _shuffleSchedule)
        mask = (mask & ((1u << index) - 1)) | ((mask >> (index + 1)) << index);

    if (_shuffleRoster.size() < 2)
    {
        FinishSoloShuffle();
        return;
    }

    RedealRemainingShuffleRounds();
    AnnounceToArena(this, "A player has left the shuffle. The rounds still to come are dealt again without them.");
}

void Arena::SyncShuffleRoster()
{
    bool joined = false;
    SquadFeedPlayers const players = GetSquadFeedPlayers();
    for (auto const& [guid, participant] : m_Players)
    {
        (void)participant;
        if (std::find(_shuffleRoster.begin(), _shuffleRoster.end(), guid) != _shuffleRoster.end())
            continue;

        if (_shuffleRoster.size() >= SHUFFLE_MAX_ROSTER)
            break;

        _shuffleRoster.push_back(guid);
        _shuffleWins[guid] = 0;
        auto const itr = players.find(guid);
        if (sWorld->getBoolConfig(CONFIG_CENTURION_SOLO_SHUFFLE_BALANCE_HEALERS) &&
            itr != players.end() && IsSoloShuffleHealer(itr->second))
            _shuffleHealers.insert(guid);
        joined = true;
    }

    if (joined)
        RedealRemainingShuffleRounds();
}
