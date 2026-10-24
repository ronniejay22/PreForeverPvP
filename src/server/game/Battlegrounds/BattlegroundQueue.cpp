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

#include "BattlegroundQueue.h"
#include "Arena.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "BattlegroundMgr.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Group.h"
#include "Language.h"
#include "Log.h"
#include "Miscellaneous/TournamentMode.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "WorldSession.h"
#include "World.h"

#include <algorithm>
#include <functional>
#include <vector>

namespace
{
bool GroupHasRealPlayerInvitee(GroupQueueInfo const* ginfo)
{
    if (!ginfo)
        return false;

    for (auto const& playerEntry : ginfo->Players)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(playerEntry.first);
        if (!player)
            continue;

        WorldSession* session = player->GetSession();
        if (!session || (!session->IsVirtualSession() && !session->IsTransientPlayerSession()))
            return true;
    }

    return false;
}

// The Battlegrounds tab's "Arena bots" box, read for a whole group. An arena
// group IS the team, so one member's refusal speaks for all of it: seating the
// group would hand everyone in it the clone-filled match that member said no
// to. Bots in the group have no say - they are the fill.
bool GroupAllowsArenaBotFill(GroupQueueInfo const* ginfo)
{
    if (!ginfo)
        return false;

    for (auto const& playerEntry : ginfo->Players)
    {
        Player const* player = ObjectAccessor::FindConnectedPlayer(playerEntry.first);
        if (!player)
            continue;

        WorldSession const* session = player->GetSession();
        if (!session || session->IsVirtualSession() || session->IsTransientPlayerSession())
            continue;

        if (player->HasArenaBotFillOptOut())
            return false;
    }

    return true;
}

// The bot ladder team (BattlegroundMgr::SetArenaLadderTeams) a rated team the
// queue gives clones to plays: one of the few nearest its matchmaker rating,
// at random, so the opponents rotate - and the one it played last only when
// there is nobody else. A ladder team already in a match sits this one out.
ArenaTeam* PickArenaLadderOpponent(GroupQueueInfo const* ginfo, uint8 arenaType)
{
    constexpr std::size_t NEAREST = 3;

    struct Candidate
    {
        ArenaTeam* team;
        uint32 distance;
    };
    std::vector<Candidate> candidates;
    for (uint32 teamId : sBattlegroundMgr->GetArenaLadderTeams())
    {
        ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
        if (!team || team->GetType() != arenaType || teamId == ginfo->ArenaTeamId || sBattlegroundMgr->IsArenaTeamInMatch(teamId))
            continue;

        int32 const distance = int32(team->GetRosterAverageMMR()) - int32(ginfo->ArenaMatchmakerRating);
        candidates.push_back({ team, uint32(std::abs(distance)) });
    }

    if (candidates.size() > 1)
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [ginfo](Candidate const& candidate)
        {
            return candidate.team->GetId() == ginfo->PreviousOpponentsTeamId;
        }), candidates.end());

    if (candidates.empty())
        return nullptr;

    std::sort(candidates.begin(), candidates.end(), [](Candidate const& left, Candidate const& right)
    {
        return left.distance < right.distance;
    });
    return candidates[urand(0, uint32(std::min(candidates.size(), NEAREST)) - 1)].team;
}

// Two rated teams the stock pairing may put together. Whole teams always can.
// Where rated arenas are bot-filled a team short of the arena size may be
// paired too - the match is made up with clones - unless somebody on either
// side has said no to clone-filled arenas.
bool RatedTeamsCanMeet(GroupQueueInfo const* a, GroupQueueInfo const* b, uint8 arenaType)
{
    if (a->Players.size() >= arenaType && b->Players.size() >= arenaType)
        return true;

    return sBattlegroundMgr->IsBotFillRatedArena(arenaType) && GroupAllowsArenaBotFill(a) && GroupAllowsArenaBotFill(b);
}

bool GroupHasBotInvitee(GroupQueueInfo const* ginfo)
{
    if (!ginfo)
        return false;

    for (auto const& playerEntry : ginfo->Players)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(playerEntry.first);
        if (player && player->GetSession())
        {
            WorldSession const* session = player->GetSession();
            if (session->IsVirtualSession() || session->IsTransientPlayerSession())
                return true;
        }
    }

    return false;
}

bool SelectionPoolsHaveBotInvitees(BattlegroundQueue::SelectionPool const selectionPools[PVP_TEAMS_COUNT])
{
    for (uint32 team = 0; team < PVP_TEAMS_COUNT; ++team)
        for (GroupQueueInfo const* ginfo : selectionPools[team].SelectedGroups)
            if (GroupHasBotInvitee(ginfo))
                return true;

    return false;
}

// "Virtual" throughout this file means a bot occupant the queue may displace
// for a real player: a socketless virtual session, or a transient clone in a
// bot-filled match. Battleground::IsBotParticipantSession is the one judge.
uint32 CountVirtualPlayersOnTeam(Battleground* battleground, uint32 team)
{
    if (!battleground)
        return 0;

    uint32 count = 0;
    for (auto const& [memberGuid, bgPlayer] : battleground->GetPlayers())
    {
        if (bgPlayer.Team != team)
            continue;

        Player* candidate = ObjectAccessor::FindConnectedPlayer(memberGuid);
        if (!candidate)
            continue;

        if (!battleground->IsBotParticipantSession(candidate->GetSession()))
            continue;

        ++count;
    }

    return count;
}

// People on a team or on their way to it: every occupant who is not a bot (a
// disconnected person keeps their seat) and every invite still out.
uint32 CountPeopleOnTeam(Battleground* battleground, uint32 team)
{
    uint32 count = battleground->GetInvitedCount(team);
    for (auto const& [memberGuid, bgPlayer] : battleground->GetPlayers())
    {
        if (bgPlayer.Team != team)
            continue;

        Player* candidate = ObjectAccessor::FindConnectedPlayer(memberGuid);
        if (!candidate || !battleground->IsBotParticipantSession(candidate->GetSession()))
            ++count;
    }

    return count;
}

uint32 GetDesiredSelectionCount(BattlegroundQueue::SelectionPool const& selectionPool, GroupQueueInfo const* ginfo,
    Battleground* bg, uint32 team, int32 freeSlots)
{
    uint32 desiredCount = freeSlots > 0 ? uint32(freeSlots) : 0u;
    if (!ginfo)
        return desiredCount;

    uint32 const neededSelectionCount = selectionPool.GetPlayerCount() + ginfo->Players.size();
    if (desiredCount < neededSelectionCount && GroupHasRealPlayerInvitee(ginfo) &&
        CountVirtualPlayersOnTeam(bg, team) >= ginfo->Players.size())
        desiredCount = neededSelectionCount;

    return desiredCount;
}

bool SelectionCanFitGroup(BattlegroundQueue::SelectionPool const& selectionPool, GroupQueueInfo const* ginfo,
    uint32 desiredCount)
{
    return ginfo && desiredCount >= selectionPool.GetPlayerCount() + ginfo->Players.size();
}

bool AddForcedGroupToSelectionPool(BattlegroundQueue::SelectionPool selectionPools[PVP_TEAMS_COUNT], GroupQueueInfo* ginfo,
    Battleground* bg, int32 hordeFree, int32 allianceFree)
{
    TeamId const forcedTeamIndex = ginfo->Team == HORDE ? TEAM_HORDE : TEAM_ALLIANCE;
    int32 const freeSlots = ginfo->Team == HORDE ? hordeFree : allianceFree;
    uint32 const desiredCount = GetDesiredSelectionCount(selectionPools[forcedTeamIndex], ginfo, bg,
        ginfo->Team, freeSlots);

    return selectionPools[forcedTeamIndex].AddGroup(ginfo, desiredCount, forcedTeamIndex);
}

bool RemoveVirtualPlayersFromTeam(Battleground* battleground, uint32 team, uint32 count)
{
    if (!battleground || !count)
        return false;

    // Never unseat an arena clone at invite time: a team left empty during
    // preparation hands the other side the arena. A skirmish still preparing
    // gives the seat up when the person it was promised to arrives
    // (Battleground::HandOverPreparationSeat); one already fighting admits
    // nobody. Persistent virtual bots retain the old displacement policy.
    bool const protectTransientArenaRoster = battleground->isArena() &&
        (battleground->GetStatus() == STATUS_WAIT_JOIN || battleground->GetStatus() == STATUS_IN_PROGRESS);

    // Transient clones have nowhere to be sent home to (no entry point, no
    // client), so they are only unseated here; the playerbot clone manager
    // notices a clone that is no longer on its match's roster and takes it
    // down on the next world tick. Dead clones go first: nobody misses them.
    struct Removal
    {
        ObjectGuid guid;
        bool transient = false;
    };

    std::vector<Removal> candidates;
    for (auto const& [memberGuid, bgPlayer] : battleground->GetPlayers())
    {
        if (bgPlayer.Team != team)
            continue;

        Player* candidate = ObjectAccessor::FindConnectedPlayer(memberGuid);
        if (!candidate)
            continue;

        WorldSession const* session = candidate->GetSession();
        if (!battleground->IsBotParticipantSession(session))
            continue;
        if (protectTransientArenaRoster && session->IsTransientPlayerSession())
            continue;

        Removal removal;
        removal.guid = memberGuid;
        removal.transient = session->IsTransientPlayerSession();
        if (!candidate->IsAlive())
            candidates.insert(candidates.begin(), removal);
        else
            candidates.push_back(removal);
    }

    if (candidates.size() < count)
        return false;

    candidates.resize(count);
    for (Removal const& removal : candidates)
        battleground->RemovePlayerAtLeave(removal.guid, !removal.transient, !removal.transient);

    return true;
}

// Seats on a team still open to somebody arriving now: capacity less the
// people already in and the invites still outstanding. This is the same sum
// FillPlayersToBG works from; GetFreeSlotsForTeam is not usable for it since
// this fork consumes the invite counter on entry, which leaves that function
// reporting a seat on a full team whenever the two sides are level.
int32 TeamSeatsLeft(Battleground const* battleground, uint32 team)
{
    if (!battleground)
        return 0;

    int32 const occupied = int32(battleground->GetInvitedCount(team) + battleground->GetPlayersCountByTeam(team));
    return int32(battleground->GetMaxPlayersPerTeam()) - occupied;
}
}

 /*********************************************************/
 /***            BATTLEGROUND QUEUE SYSTEM              ***/
 /*********************************************************/

BattlegroundQueue::BattlegroundQueue()
{
    for (uint32 i = 0; i < PVP_TEAMS_COUNT; ++i)
    {
        for (uint32 j = 0; j < MAX_BATTLEGROUND_BRACKETS; ++j)
        {
            m_SumOfWaitTimes[i][j] = 0;
            m_WaitTimeLastPlayer[i][j] = 0;
            for (uint32 k = 0; k < COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME; ++k)
                m_WaitTimes[i][j][k] = 0;
        }
    }
}

BattlegroundQueue::~BattlegroundQueue()
{
    m_events.KillAllEvents(false);

    for (int i = 0; i < MAX_BATTLEGROUND_BRACKETS; ++i)
    {
        for (uint32 j = 0; j < BG_QUEUE_GROUP_TYPES_COUNT; ++j)
        {
            for (GroupsQueueType::iterator itr = m_QueuedGroups[i][j].begin(); itr != m_QueuedGroups[i][j].end(); ++itr)
                delete (*itr);
        }
    }
}

/*********************************************************/
/***      BATTLEGROUND QUEUE SELECTION POOLS           ***/
/*********************************************************/

// selection pool initialization, used to clean up from prev selection
void BattlegroundQueue::SelectionPool::Init()
{
    SelectedGroups.clear();
    PlayerCount = 0;
}

// remove group info from selection pool
// returns true when we need to try to add new group to selection pool
// returns false when selection pool is ok or when we kicked smaller group than we need to kick
// sometimes it can be called on empty selection pool
bool BattlegroundQueue::SelectionPool::KickGroup(uint32 size)
{
    //find maxgroup or LAST group with size == size and kick it
    bool found = false;
    GroupsQueueType::iterator groupToKick = SelectedGroups.begin();
    for (GroupsQueueType::iterator itr = groupToKick; itr != SelectedGroups.end(); ++itr)
    {
        if (abs((int32)((*itr)->Players.size() - size)) <= 1)
        {
            groupToKick = itr;
            found = true;
        }
        else if (!found && (*itr)->Players.size() >= (*groupToKick)->Players.size())
            groupToKick = itr;
    }
    //if pool is empty, do nothing
    if (GetPlayerCount())
    {
        //update player count
        GroupQueueInfo* ginfo = (*groupToKick);
        SelectedGroups.erase(groupToKick);
        PlayerCount -= ginfo->Players.size();
        //return false if we kicked smaller group or there are enough players in selection pool
        if (ginfo->Players.size() <= size + 1)
            return false;
    }
    return true;
}

// add group to selection pool
// used when building selection pools
// returns true if we can invite more players, or when we added group to selection pool
// returns false when selection pool is full
bool BattlegroundQueue::SelectionPool::AddGroup(GroupQueueInfo* ginfo, uint32 desiredCount, uint8 tem)
{
    //if group is larger than desired count - don't allow to add it to pool
    if (!ginfo->IsInvitedToBGInstanceGUID && desiredCount >= PlayerCount + ginfo->Players.size())
    {
        if (tem < 2)
        {
            ginfo->Team = (tem == 0) ? ALLIANCE : HORDE;
        }
        SelectedGroups.push_back(ginfo);
        // increase selected players count
        PlayerCount += ginfo->Players.size();
        return true;
    }
    if (PlayerCount < desiredCount)
        return true;
    return false;
}

/*********************************************************/
/***               BATTLEGROUND QUEUES                 ***/
/*********************************************************/

// add group or player (grp == NULL) to bg queue with the given leader and bg specifications
GroupQueueInfo* BattlegroundQueue::AddGroup(Player* leader, Group* grp, BattlegroundTypeId BgTypeId, PvPDifficultyEntry const* bracketEntry, uint8 ArenaType, bool isRated, bool isPremade, uint32 ArenaRating, uint32 MatchmakerRating, uint32 arenateamid, uint32 PreviousOpponentsArenaTeamId)
{
    BattlegroundBracketId bracketId = bracketEntry->GetBracketId();

    // create new ginfo
    GroupQueueInfo* ginfo = new GroupQueueInfo;
    ginfo->BgTypeId = BgTypeId;
    ginfo->ArenaType = ArenaType;
    ginfo->ArenaTeamId = arenateamid;
    ginfo->IsRated = isRated;
    ginfo->IsInvitedToBGInstanceGUID = 0;
    ginfo->JoinTime = GameTime::GetGameTimeMS();
    ginfo->RemoveInviteTime = 0;
    ginfo->Team = leader->GetTeam();
    ginfo->IsForcedTeam = false;
    ginfo->ArenaTeamRating = ArenaRating;
    ginfo->ArenaMatchmakerRating = MatchmakerRating;
    ginfo->PreviousOpponentsTeamId = PreviousOpponentsArenaTeamId;
    ginfo->OpponentsTeamRating = 0;
    ginfo->OpponentsMatchmakerRating = 0;
    ginfo->ShuffleParty = 0;
    // Captured once, from the leader: Group::CanJoinBattlegroundQueue refuses a
    // party that straddles the pools, and the opt-in cannot be toggled while
    // queued, so nothing later moves a group from one pool to the other.
    ginfo->TournamentPool = Tournament::QueuesInTournamentPool(leader);

    // For battlegrounds we use synthetic queue sides instead of the player's real faction.
    // This keeps the first waiting team on one side and pushes the next waiting team to the
    // opposite side so cross-faction queues can always produce a match.
    //
    // Scarlet Chapel may explicitly request a side by temporarily setting the raw BG team override
    // before queueing. Do not use Player::GetBGTeam() here: it falls back to the player's real
    // faction when no override is set, which would make every SCM queue look forced.
    uint32 const explicitBgTeam = IsCustomBattleground(BgTypeId) ? leader->GetBGTeamOverride() : 0;
    bool const hasForcedQueueTeam = explicitBgTeam == ALLIANCE || explicitBgTeam == HORDE;
    if (!ArenaType && !hasForcedQueueTeam)
    {
        uint32 alliancePlayers = 0;
        uint32 hordePlayers = 0;
        uint32 allianceGroups = 0;
        uint32 hordeGroups = 0;
        uint32 allianceIndex = isPremade ? BG_QUEUE_PREMADE_ALLIANCE : BG_QUEUE_NORMAL_ALLIANCE;
        uint32 hordeIndex = isPremade ? BG_QUEUE_PREMADE_HORDE : BG_QUEUE_NORMAL_HORDE;

        for (GroupsQueueType::const_iterator itr = m_QueuedGroups[bracketId][allianceIndex].begin(); itr != m_QueuedGroups[bracketId][allianceIndex].end(); ++itr)
        {
            // Only groups this one can be matched with count toward its side.
            if ((*itr)->IsInvitedToBGInstanceGUID || (*itr)->TournamentPool != ginfo->TournamentPool)
                continue;

            alliancePlayers += (*itr)->Players.size();
            ++allianceGroups;
        }

        for (GroupsQueueType::const_iterator itr = m_QueuedGroups[bracketId][hordeIndex].begin(); itr != m_QueuedGroups[bracketId][hordeIndex].end(); ++itr)
        {
            if ((*itr)->IsInvitedToBGInstanceGUID || (*itr)->TournamentPool != ginfo->TournamentPool)
                continue;

            hordePlayers += (*itr)->Players.size();
            ++hordeGroups;
        }

        bool addHorde = false;
        if (!hordeGroups && allianceGroups)
            addHorde = true;
        else if (!allianceGroups && hordeGroups)
            addHorde = false;
        else if (alliancePlayers != hordePlayers)
            addHorde = alliancePlayers > hordePlayers;
        else if (allianceGroups != hordeGroups)
            addHorde = allianceGroups > hordeGroups;
        else
            addHorde = roll_chance_i(50);

        ginfo->Team = addHorde ? HORDE : ALLIANCE;

        // SCM refill behavior: if there is already an active/free-slot SCM instance,
        // drive new solo entries to the side that currently has more free slots.
        if (IsCustomBattleground(BgTypeId))
        {
            BGFreeSlotQueueContainer& freeSlotQueue = sBattlegroundMgr->GetBGFreeSlotQueueStore(BgTypeId);
            for (Battleground* bg : freeSlotQueue)
            {
                if (!bg)
                    continue;

                if (bg->GetBracketId() != bracketId || bg->IsTournamentPool() != ginfo->TournamentPool)
                    continue;

                uint32 const allianceFree = bg->GetFreeSlotsForTeam(ALLIANCE);
                uint32 const hordeFree = bg->GetFreeSlotsForTeam(HORDE);
                if (!allianceFree && !hordeFree)
                    continue;

                if (allianceFree > hordeFree)
                    ginfo->Team = ALLIANCE;
                else if (hordeFree > allianceFree)
                    ginfo->Team = HORDE;
                break;
            }
        }
    }
    else if (hasForcedQueueTeam)
    {
        ginfo->Team = explicitBgTeam;
        ginfo->IsForcedTeam = true;
    }

    ginfo->Players.clear();

    //compute index (if group is premade or joined a rated match) to queues
    uint32 index = 0;
    if (!isRated && !isPremade)
        index += PVP_TEAMS_COUNT;
    if (ginfo->Team == HORDE)
        index++;
    TC_LOG_DEBUG("bg.battleground", "Adding Group to BattlegroundQueue bgTypeId : {}, bracket_id : {}, index : {}", BgTypeId, bracketId, index);

    uint32 lastOnlineTime = GameTime::GetGameTimeMS();

    //announce world (this don't need mutex)
    if (isRated && sWorld->getBoolConfig(CONFIG_ARENA_QUEUE_ANNOUNCER_ENABLE))
    {
        ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(arenateamid);
        if (team)
            sWorld->SendWorldText(LANG_ARENA_QUEUE_ANNOUNCE_WORLD_JOIN, team->GetName().c_str(), ginfo->ArenaType, ginfo->ArenaType, ginfo->ArenaTeamRating);
    }

    //add players from group to ginfo
    if (grp)
    {
        for (GroupReference* itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member)
                continue;   // this should never happen
            PlayerQueueInfo& pl_info = m_QueuedPlayers[member->GetGUID()];
            pl_info.LastOnlineTime = lastOnlineTime;
            pl_info.GroupInfo = ginfo;
            // add the pinfo to ginfo's list
            ginfo->Players[member->GetGUID()] = &pl_info;
        }
    }
    else
    {
        PlayerQueueInfo& pl_info = m_QueuedPlayers[leader->GetGUID()];
        pl_info.LastOnlineTime = lastOnlineTime;
        pl_info.GroupInfo = ginfo;
        ginfo->Players[leader->GetGUID()] = &pl_info;
    }

    //add GroupInfo to m_QueuedGroups
    {
        m_QueuedGroups[bracketId][index].push_back(ginfo);

        //announce to world, this code needs mutex
        if (!isRated && !isPremade && sWorld->getBoolConfig(CONFIG_BATTLEGROUND_QUEUE_ANNOUNCER_ENABLE))
        {
            if (Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(ginfo->BgTypeId))
            {
                uint32 MinPlayers = bg->GetMinPlayersPerTeam();
                uint32 qHorde = 0;
                uint32 qAlliance = 0;
                uint32 q_min_level = bracketEntry->MinLevel;
                uint32 q_max_level = bracketEntry->MaxLevel;
                GroupsQueueType::const_iterator itr;
                for (itr = m_QueuedGroups[bracketId][BG_QUEUE_NORMAL_ALLIANCE].begin(); itr != m_QueuedGroups[bracketId][BG_QUEUE_NORMAL_ALLIANCE].end(); ++itr)
                    if (!(*itr)->IsInvitedToBGInstanceGUID)
                        qAlliance += (*itr)->Players.size();
                for (itr = m_QueuedGroups[bracketId][BG_QUEUE_NORMAL_HORDE].begin(); itr != m_QueuedGroups[bracketId][BG_QUEUE_NORMAL_HORDE].end(); ++itr)
                    if (!(*itr)->IsInvitedToBGInstanceGUID)
                        qHorde += (*itr)->Players.size();

                // Show queue status to player only (when joining queue)
                if (sWorld->getBoolConfig(CONFIG_BATTLEGROUND_QUEUE_ANNOUNCER_PLAYERONLY))
                {
                    ChatHandler(leader->GetSession()).PSendSysMessage(LANG_BG_QUEUE_ANNOUNCE_SELF, bg->GetName().c_str(), q_min_level, q_max_level,
                        qAlliance, (MinPlayers > qAlliance) ? MinPlayers - qAlliance : (uint32)0, qHorde, (MinPlayers > qHorde) ? MinPlayers - qHorde : (uint32)0);
                }
                // System message
                else
                {
                    sWorld->SendWorldText(LANG_BG_QUEUE_ANNOUNCE_WORLD, bg->GetName().c_str(), q_min_level, q_max_level,
                        qAlliance, (MinPlayers > qAlliance) ? MinPlayers - qAlliance : (uint32)0, qHorde, (MinPlayers > qHorde) ? MinPlayers - qHorde : (uint32)0);
                }
            }
        }
        //release mutex
    }

    return ginfo;
}

void BattlegroundQueue::PlayerInvitedToBGUpdateAverageWaitTime(GroupQueueInfo* ginfo, BattlegroundBracketId bracket_id)
{
    uint32 timeInQueue = getMSTimeDiff(ginfo->JoinTime, GameTime::GetGameTimeMS());
    uint8 team_index = TEAM_ALLIANCE;                    //default set to TEAM_ALLIANCE - or non rated arenas!
    if (!ginfo->ArenaType)
    {
        if (ginfo->Team == HORDE)
            team_index = TEAM_HORDE;
    }
    else
    {
        if (ginfo->IsRated)
            team_index = TEAM_HORDE;                     //for rated arenas use TEAM_HORDE
    }

    //store pointer to arrayindex of player that was added first
    uint32* lastPlayerAddedPointer = &(m_WaitTimeLastPlayer[team_index][bracket_id]);
    //remove his time from sum
    m_SumOfWaitTimes[team_index][bracket_id] -= m_WaitTimes[team_index][bracket_id][(*lastPlayerAddedPointer)];
    //set average time to new
    m_WaitTimes[team_index][bracket_id][(*lastPlayerAddedPointer)] = timeInQueue;
    //add new time to sum
    m_SumOfWaitTimes[team_index][bracket_id] += timeInQueue;
    //set index of last player added to next one
    (*lastPlayerAddedPointer)++;
    (*lastPlayerAddedPointer) %= COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME;
}

uint32 BattlegroundQueue::GetAverageQueueWaitTime(GroupQueueInfo* ginfo, BattlegroundBracketId bracket_id) const
{
    uint8 team_index = TEAM_ALLIANCE;                    //default set to TEAM_ALLIANCE - or non rated arenas!
    if (!ginfo->ArenaType)
    {
        if (ginfo->Team == HORDE)
            team_index = TEAM_HORDE;
    }
    else
    {
        if (ginfo->IsRated)
            team_index = TEAM_HORDE;                     //for rated arenas use TEAM_HORDE
    }
    //check if there is enought values(we always add values > 0)
    if (m_WaitTimes[team_index][bracket_id][COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME - 1])
        return (m_SumOfWaitTimes[team_index][bracket_id] / COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME);
    else
        //if there aren't enough values return 0 - not available
        return 0;
}

//remove player from queue and from group info, if group info is empty then remove it too
void BattlegroundQueue::RemovePlayer(ObjectGuid guid, bool decreaseInvitedCount)
{
    int32 bracket_id = -1;                                     // signed for proper for-loop finish
    QueuedPlayersMap::iterator itr;

    //remove player from map, if he's there
    itr = m_QueuedPlayers.find(guid);
    if (itr == m_QueuedPlayers.end())
    {
        //This happens if a player logs out while in a bg because WorldSession::LogoutPlayer() notifies the bg twice
        std::string playerName = "Unknown";
        if (Player* player = ObjectAccessor::FindPlayer(guid))
            playerName = player->GetName();
        TC_LOG_DEBUG("bg.battleground", "BattlegroundQueue: couldn't find player {} ({})", playerName, guid.ToString());
        return;
    }

    GroupQueueInfo* group = itr->second.GroupInfo;
    GroupsQueueType::iterator group_itr;
    // mostly people with the highest levels are in battlegrounds, thats why
    // we count from MAX_BATTLEGROUND_QUEUES - 1 to 0

    uint32 index = (group->Team == HORDE) ? BG_QUEUE_PREMADE_HORDE : BG_QUEUE_PREMADE_ALLIANCE;

    for (int32 bracket_id_tmp = MAX_BATTLEGROUND_BRACKETS - 1; bracket_id_tmp >= 0 && bracket_id == -1; --bracket_id_tmp)
    {
        //we must check premade and normal team's queue - because when players from premade are joining bg,
        //they leave groupinfo so we can't use its players size to find out index
        for (uint32 j = index; j < BG_QUEUE_GROUP_TYPES_COUNT; j++)
        {
            GroupsQueueType::iterator k = m_QueuedGroups[bracket_id_tmp][j].begin();
            for (; k != m_QueuedGroups[bracket_id_tmp][j].end(); ++k)
            {
                if ((*k) == group)
                {
                    bracket_id = bracket_id_tmp;
                    group_itr = k;
                    //we must store index to be able to erase iterator
                    index = j;
                    break;
                }
            }
        }
    }

    //player can't be in queue without group, but just in case
    if (bracket_id == -1)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundQueue: ERROR Cannot find groupinfo for {}", guid.ToString());
        return;
    }
    TC_LOG_DEBUG("bg.battleground", "BattlegroundQueue: Removing {}, from bracket_id {}", guid.ToString(), (uint32)bracket_id);

    // ALL variables are correctly set
    // We can ignore leveling up in queue - it should not cause crash
    // remove player from group
    // if only one player there, remove group

    // remove player queue info from group queue info
    std::map<ObjectGuid, PlayerQueueInfo*>::iterator pitr = group->Players.find(guid);
    if (pitr != group->Players.end())
        group->Players.erase(pitr);

    // if invited to bg, and should decrease invited count, then do it
    if (decreaseInvitedCount && group->IsInvitedToBGInstanceGUID)
        if (Battleground* bg = sBattlegroundMgr->GetBattleground(group->IsInvitedToBGInstanceGUID, group->BgTypeId))
        {
            bg->DecreaseInvitedCount(group->Team);
            // An invite turned down or left to expire: the squad seat held
            // for it goes back to the match.
            if (!bg->IsPlayerInBattleground(guid))
                bg->ReleaseSquadSeat(guid);
        }

    // remove player queue info
    m_QueuedPlayers.erase(itr);

    // announce to world if arena team left queue for rated match, show only once
    if (group->ArenaType && group->IsRated && group->Players.empty() && sWorld->getBoolConfig(CONFIG_ARENA_QUEUE_ANNOUNCER_ENABLE))
        if (ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(group->ArenaTeamId))
            sWorld->SendWorldText(LANG_ARENA_QUEUE_ANNOUNCE_WORLD_EXIT, team->GetName().c_str(), group->ArenaType, group->ArenaType, group->ArenaTeamRating);

    // if player leaves queue and he is invited to rated arena match, then he have to lose
    if (group->IsInvitedToBGInstanceGUID && group->IsRated && decreaseInvitedCount)
    {
        if (ArenaTeam* at = sArenaTeamMgr->GetArenaTeamById(group->ArenaTeamId))
        {
            TC_LOG_DEBUG("bg.battleground", "UPDATING memberLost's personal arena rating for {} by opponents rating: {}", guid.ToString(), group->OpponentsTeamRating);
            if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
                at->MemberLost(player, group->OpponentsMatchmakerRating);
            else
                at->OfflineMemberLost(guid, group->OpponentsMatchmakerRating);
            at->SaveToDB();
        }
    }

    // remove group queue info if needed
    if (group->Players.empty())
    {
        m_QueuedGroups[bracket_id][index].erase(group_itr);
        delete group;
        return;
    }

    // if group wasn't empty, so it wasn't deleted, and player have left a rated
    // queue -> everyone from the group should leave too
    // don't remove recursively if already invited to bg!
    if (!group->IsInvitedToBGInstanceGUID && group->IsRated)
    {
        // remove next player, this is recursive
        // first send removal information
        if (Player* plr2 = ObjectAccessor::FindConnectedPlayer(group->Players.begin()->first))
        {
            Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(group->BgTypeId);
            BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(group->BgTypeId, group->ArenaType);
            uint32 queueSlot = plr2->GetBattlegroundQueueIndex(bgQueueTypeId);
            plr2->RemoveBattlegroundQueueId(bgQueueTypeId); // must be called this way, because if you move this call to
            // queue->removeplayer, it causes bugs
            WorldPacket data;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_NONE, 0, 0, 0, 0);
            plr2->SendDirectMessage(&data);
        }
        // then actually delete, this may delete the group as well!
        RemovePlayer(group->Players.begin()->first, decreaseInvitedCount);
    }
}

//returns true when player pl_guid is in queue and is invited to bgInstanceGuid
bool BattlegroundQueue::IsPlayerInvited(ObjectGuid pl_guid, const uint32 bgInstanceGuid, const uint32 removeTime)
{
    QueuedPlayersMap::const_iterator qItr = m_QueuedPlayers.find(pl_guid);
    return (qItr != m_QueuedPlayers.end()
        && qItr->second.GroupInfo->IsInvitedToBGInstanceGUID == bgInstanceGuid
        && qItr->second.GroupInfo->RemoveInviteTime == removeTime);
}

bool BattlegroundQueue::GetPlayerGroupInfoData(ObjectGuid guid, GroupQueueInfo* ginfo)
{
    if (!ginfo)
        return false;

    QueuedPlayersMap::const_iterator qItr = m_QueuedPlayers.find(guid);
    if (qItr == m_QueuedPlayers.end())
        return false;

    // A stale queue entry can survive a bot lifecycle transition with its
    // group pointer cleared. Treat it as absent so callers can prune the
    // player's local queue slot instead of dereferencing null and taking down
    // the worldserver while the client is waiting for a battleground status.
    if (!qItr->second.GroupInfo)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundQueue: null groupinfo for {}", guid.ToString());
        return false;
    }

    *ginfo = *(qItr->second.GroupInfo);
    return true;
}

uint32 BattlegroundQueue::GetPlayersInQueue(TeamId id)
{
    return m_SelectionPools[id].GetPlayerCount();
}

bool BattlegroundQueue::InviteGroupToBG(GroupQueueInfo* ginfo, Battleground* bg, uint32 side)
{
    // Never across the queue pools, whatever path got here.
    if (bg && ginfo->TournamentPool != bg->IsTournamentPool())
        return false;

    // set side if needed
    if (side)
        ginfo->Team = side;

    // Let bots fully populate battlegrounds, but if a real queued group needs room in an
    // already-running battleground, admit it by dropping as many bots from that target
    // side as it is short of seats. If that side has fewer bots than the shortfall, leave
    // the battleground as-is so grouped players are not partially admitted or split by side.
    if (bg && bg->isBattleground() && GroupHasRealPlayerInvitee(ginfo))
    {
        int32 const shortfall = int32(ginfo->Players.size()) - std::max<int32>(TeamSeatsLeft(bg, ginfo->Team), 0);
        if (shortfall > 0 && !RemoveVirtualPlayersFromTeam(bg, ginfo->Team, uint32(shortfall)))
            return false;
    }

    if (!ginfo->IsInvitedToBGInstanceGUID)
    {
        // not yet invited
        // set invitation
        ginfo->IsInvitedToBGInstanceGUID = bg->GetInstanceID();
        BattlegroundTypeId bgTypeId = bg->GetTypeID();
        BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, bg->GetArenaType());
        BattlegroundBracketId bracket_id = bg->GetBracketId();

        // set ArenaTeamId for rated matches
        if (bg->isArena() && bg->isRated())
            bg->SetArenaTeamIdForTeam(ginfo->Team, ginfo->ArenaTeamId);

        ginfo->RemoveInviteTime = GameTime::GetGameTimeMS() + INVITE_ACCEPT_WAIT_TIME;

        // loop through the players
        for (std::map<ObjectGuid, PlayerQueueInfo*>::iterator itr = ginfo->Players.begin(); itr != ginfo->Players.end(); ++itr)
        {
            // get the player
            Player* player = ObjectAccessor::FindConnectedPlayer(itr->first);
            // if offline, skip him, this should not happen - player is removed from queue when he logs out
            if (!player)
                continue;

            // invite the player
            PlayerInvitedToBGUpdateAverageWaitTime(ginfo, bracket_id);
            //sBattlegroundMgr->InvitePlayer(player, bg, ginfo->Team);

            // set invited player counters
            bg->IncreaseInvitedCount(ginfo->Team);

            player->SetInviteForBattlegroundQueueType(bgQueueTypeId, ginfo->IsInvitedToBGInstanceGUID);

            // create remind invite events
            BGQueueInviteEvent* inviteEvent = new BGQueueInviteEvent(player->GetGUID(), ginfo->IsInvitedToBGInstanceGUID, bgTypeId, ginfo->ArenaType, ginfo->RemoveInviteTime);
            m_events.AddEvent(inviteEvent, m_events.CalculateTime(Milliseconds(INVITATION_REMIND_TIME)));
            // create automatic remove events
            BGQueueRemoveEvent* removeEvent = new BGQueueRemoveEvent(player->GetGUID(), ginfo->IsInvitedToBGInstanceGUID, bgTypeId, bgQueueTypeId, ginfo->RemoveInviteTime);
            m_events.AddEvent(removeEvent, m_events.CalculateTime(Milliseconds(INVITE_ACCEPT_WAIT_TIME)));

            WorldPacket data;

            uint32 queueSlot = player->GetBattlegroundQueueIndex(bgQueueTypeId);

            TC_LOG_DEBUG("bg.battleground", "Battleground: invited player {} {} to BG instance {} queueindex {} bgtype {}",
                player->GetName(), player->GetGUID().ToString(), bg->GetInstanceID(), queueSlot, bg->GetTypeID());

            // send status packet
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME, 0, ginfo->ArenaType, 0);
            player->SendDirectMessage(&data);
        }
        return true;
    }

    return false;
}

/*
This function is inviting players to already running battlegrounds
Invitation type is based on config file
large groups are disadvantageous, because they will be kicked first if invitation type = 1
*/
void BattlegroundQueue::FillPlayersToBG(Battleground* bg, BattlegroundBracketId bracket_id)
{
    int32 const numHordeInGame = bg->GetInvitedCount(HORDE) + bg->GetPlayersCountByTeam(HORDE);
    int32 const numAliInGame = bg->GetInvitedCount(ALLIANCE) + bg->GetPlayersCountByTeam(ALLIANCE);

    // One waiting group's turn at this match. False means the selection pools
    // are full, which is where the walk over a queue list stops.
    auto const offerSeats = [&](GroupQueueInfo* ginfo) -> bool
    {
        int32 const projectedHordeInGame = numHordeInGame + int32(m_SelectionPools[TEAM_HORDE].GetPlayerCount());
        int32 const projectedAliInGame = numAliInGame + int32(m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount());
        int32 const hordeFree = bg->GetMaxPlayersPerTeam() - projectedHordeInGame;
        int32 const aliFree = bg->GetMaxPlayersPerTeam() - projectedAliInGame;

        if (ginfo->IsForcedTeam)
            return AddForcedGroupToSelectionPool(m_SelectionPools, ginfo, bg, hordeFree, aliFree);

        bool addToHorde = (projectedHordeInGame < projectedAliInGame) ||
            (projectedHordeInGame == projectedAliInGame && roll_chance_i(50));

        uint32 const hordeDesired = GetDesiredSelectionCount(m_SelectionPools[TEAM_HORDE], ginfo, bg,
            HORDE, hordeFree);
        uint32 const allianceDesired = GetDesiredSelectionCount(m_SelectionPools[TEAM_ALLIANCE], ginfo, bg,
            ALLIANCE, aliFree);
        bool const hordeCanFit = SelectionCanFitGroup(m_SelectionPools[TEAM_HORDE], ginfo, hordeDesired);
        bool const allianceCanFit = SelectionCanFitGroup(m_SelectionPools[TEAM_ALLIANCE], ginfo, allianceDesired);
        if (addToHorde && !hordeCanFit && allianceCanFit)
            addToHorde = false;
        else if (!addToHorde && !allianceCanFit && hordeCanFit)
            addToHorde = true;

        if (addToHorde)
            return m_SelectionPools[TEAM_HORDE].AddGroup(ginfo, hordeDesired, TEAM_HORDE);

        return m_SelectionPools[TEAM_ALLIANCE].AddGroup(ginfo, allianceDesired, TEAM_ALLIANCE);
    };

    auto const fillFromQueue = [&](uint32 queueIndex)
    {
        for (GroupQueueInfo* ginfo : m_QueuedGroups[bracket_id][queueIndex])
        {
            if (ginfo->IsInvitedToBGInstanceGUID || !InActivePool(ginfo))
                continue;

            if (!offerSeats(ginfo))
                break;
        }
    };

    // A party big enough to be a team waits in the premade queue, and the stock
    // rules only ever offer a running match to the loose queue - a party is held
    // back so the matchmaker can find it another party to play. In a clone-padded
    // match there is no other party coming, and every seat is held by a bot that
    // stands up for anyone real, so holding the party back only buys it an empty
    // battleground of its own while the one already running goes short. Offered
    // first, because a party cannot be split across what few seats are left.
    if (bg->IsBotFillMatch())
    {
        fillFromQueue(BG_QUEUE_PREMADE_ALLIANCE);
        fillFromQueue(BG_QUEUE_PREMADE_HORDE);
    }

    fillFromQueue(BG_QUEUE_NORMAL_ALLIANCE);
    //the same thing for horde
    fillFromQueue(BG_QUEUE_NORMAL_HORDE);

    //if ofc like BG queue invitation is set in config, then we are happy
    if (sWorld->getIntConfig(CONFIG_BATTLEGROUND_INVITATION_TYPE) == BG_QUEUE_INVITATION_TYPE_NO_BALANCE)
        return;
}

// this method checks if premade versus premade battleground is possible
// then after 30 mins (default) in queue it moves premade group to normal queue
// it tries to invite as much players as it can - to MaxPlayersPerTeam, because premade groups have more than MinPlayersPerTeam players
bool BattlegroundQueue::CheckPremadeMatch(BattlegroundBracketId bracket_id, uint32 MinPlayersPerTeam, uint32 MaxPlayersPerTeam)
{
    //check match
    if (!m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].empty() && !m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].empty())
    {
        //start premade match
        //if groups aren't invited
        GroupsQueueType::const_iterator ali_group, horde_group;
        // The first waiting premade of THIS pool on each side. It has to be
        // filtered here, not only by the selection pools below: a first premade
        // from the other pool would leave both pools empty, and an empty
        // "match" still returns true and creates a battleground.
        for (ali_group = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].begin(); ali_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].end(); ++ali_group)
            if (!(*ali_group)->IsInvitedToBGInstanceGUID && InActivePool(*ali_group))
                break;
        for (horde_group = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].begin(); horde_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].end(); ++horde_group)
            if (!(*horde_group)->IsInvitedToBGInstanceGUID && InActivePool(*horde_group))
                break;

        if (ali_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].end() && horde_group != m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].end())
        {
            m_SelectionPools[TEAM_ALLIANCE].AddGroup((*ali_group), MaxPlayersPerTeam, TEAM_ALLIANCE);
            m_SelectionPools[TEAM_HORDE].AddGroup((*horde_group), MaxPlayersPerTeam, TEAM_HORDE);
            //add groups/players from normal queue to size of bigger group
            uint32 maxPlayers = std::min(m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount(), m_SelectionPools[TEAM_HORDE].GetPlayerCount());
            GroupsQueueType::const_iterator itr;
            for (uint32 i = 0; i < PVP_TEAMS_COUNT; i++)
            {
                for (itr = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].begin(); itr != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].end(); ++itr)
                {
                    //if itr can join BG and player count is less that maxPlayers, then add group to selectionpool
                    if (!(*itr)->IsInvitedToBGInstanceGUID && InActivePool(*itr) && !m_SelectionPools[i].AddGroup((*itr), maxPlayers, TEAM_ALLIANCE + i))
                        break;
                }
            }
            //premade selection pools are set
            return true;
        }
    }
    // now check if we can move group from Premade queue to normal queue (timer has expired) or group size lowered!!
    // this could be 2 cycles but i'm checking only first team in queue - it can cause problem -
    // if first is invited to BG and seconds timer expired, but we can ignore it, because players have only 80 seconds to click to enter bg
    // and when they click or after 80 seconds the queue info is removed from queue
    uint32 time_before = GameTime::GetGameTimeMS() - sWorld->getIntConfig(CONFIG_BATTLEGROUND_PREMADE_GROUP_WAIT_FOR_MATCH);
    for (uint32 i = 0; i < PVP_TEAMS_COUNT; i++)
    {
        if (!m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].empty())
        {
            GroupsQueueType::iterator itr = m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].begin();
            if (!(*itr)->IsInvitedToBGInstanceGUID && ((*itr)->JoinTime < time_before || (*itr)->Players.size() < MinPlayersPerTeam))
            {
                //we must insert group to normal queue and erase pointer from premade queue
                m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].push_front((*itr));
                m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE + i].erase(itr);
            }
        }
    }
    //selection pools are not set
    return false;
}

// this method tries to create battleground or arena with MinPlayersPerTeam against MinPlayersPerTeam
bool BattlegroundQueue::CheckNormalMatch(Battleground* bg_template, BattlegroundBracketId bracket_id, uint32 minPlayers, uint32 maxPlayers)
{
    GroupsQueueType::const_iterator itr_team[PVP_TEAMS_COUNT];
    for (uint32 i = 0; i < PVP_TEAMS_COUNT; i++)
    {
        itr_team[i] = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].begin();
        for (; itr_team[i] != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + i].end(); ++(itr_team[i]))
        {
            if (!(*(itr_team[i]))->IsInvitedToBGInstanceGUID && InActivePool(*(itr_team[i])))
            {
                m_SelectionPools[i].AddGroup(*(itr_team[i]), maxPlayers, TEAM_ALLIANCE + i);
                if (m_SelectionPools[i].GetPlayerCount() >= minPlayers)
                    break;
            }
        }
    }
    //try to invite same number of players - this cycle may cause longer wait time even if there are enough players in queue, but we want ballanced bg
    uint32 j = TEAM_ALLIANCE;
    if (m_SelectionPools[TEAM_HORDE].GetPlayerCount() < m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount())
        j = TEAM_HORDE;
    if (sWorld->getIntConfig(CONFIG_BATTLEGROUND_INVITATION_TYPE) != BG_QUEUE_INVITATION_TYPE_NO_BALANCE
        && m_SelectionPools[TEAM_HORDE].GetPlayerCount() >= minPlayers && m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount() >= minPlayers)
    {
        //we will try to invite more groups to team with less players indexed by j
        ++(itr_team[j]);                                         //this will not cause a crash, because for cycle above reached break;
        for (; itr_team[j] != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + j].end(); ++(itr_team[j]))
        {
            if (!(*(itr_team[j]))->IsInvitedToBGInstanceGUID && InActivePool(*(itr_team[j])))
                if (!m_SelectionPools[j].AddGroup(*(itr_team[j]), m_SelectionPools[(j + 1) % PVP_TEAMS_COUNT].GetPlayerCount(), TEAM_ALLIANCE + j))
                    break;
        }
        // do not allow to start bg with more than 2 players more on 1 faction
        if (abs((int32)(m_SelectionPools[TEAM_HORDE].GetPlayerCount() - m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount())) > 2)
            return false;
    }
    //allow 1v0 if debug bg
    if (sBattlegroundMgr->isTesting() && bg_template->isBattleground() && (m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount() || m_SelectionPools[TEAM_HORDE].GetPlayerCount()))
        return true;
    //return true if there are enough players in selection pools - enable to work .debug bg command correctly
    return m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount() >= minPlayers && m_SelectionPools[TEAM_HORDE].GetPlayerCount() >= minPlayers;
}

// this method will check if we can invite players to same faction skirmish match
bool BattlegroundQueue::CheckSkirmishForSameFaction(BattlegroundBracketId bracket_id, uint32 minPlayersPerTeam)
{
    if (m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount() < minPlayersPerTeam && m_SelectionPools[TEAM_HORDE].GetPlayerCount() < minPlayersPerTeam)
        return false;
    uint32 teamIndex = TEAM_ALLIANCE;
    uint32 otherTeam = TEAM_HORDE;
    uint32 otherTeamId = HORDE;
    if (m_SelectionPools[TEAM_HORDE].GetPlayerCount() == minPlayersPerTeam)
    {
        teamIndex = TEAM_HORDE;
        otherTeam = TEAM_ALLIANCE;
        otherTeamId = ALLIANCE;
    }
    //clear other team's selection
    m_SelectionPools[otherTeam].Init();
    //store last ginfo pointer
    GroupQueueInfo* ginfo = m_SelectionPools[teamIndex].SelectedGroups.back();
    //set itr_team to group that was added to selection pool latest
    GroupsQueueType::iterator itr_team = m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].begin();
    for (; itr_team != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end(); ++itr_team)
        if (ginfo == *itr_team)
            break;
    if (itr_team == m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end())
        return false;
    GroupsQueueType::iterator itr_team2 = itr_team;
    ++itr_team2;
    //invite players to other selection pool
    for (; itr_team2 != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end(); ++itr_team2)
    {
        //if selection pool is full then break;
        if (!(*itr_team2)->IsInvitedToBGInstanceGUID && InActivePool(*itr_team2) && !m_SelectionPools[otherTeam].AddGroup(*itr_team2, minPlayersPerTeam, TEAM_ALLIANCE + teamIndex))
            break;
    }
    if (m_SelectionPools[otherTeam].GetPlayerCount() != minPlayersPerTeam)
        return false;

    //here we have correct 2 selections and we need to change one teams team and move selection pool teams to other team's queue
    for (GroupsQueueType::iterator itr = m_SelectionPools[otherTeam].SelectedGroups.begin(); itr != m_SelectionPools[otherTeam].SelectedGroups.end(); ++itr)
    {
        //set correct team
        (*itr)->Team = otherTeamId;
        //add team to other queue
        m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + otherTeam].push_front(*itr);
        //remove team from old queue
        GroupsQueueType::iterator itr2 = itr_team;
        ++itr2;
        for (; itr2 != m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].end(); ++itr2)
        {
            if (*itr2 == *itr)
            {
                m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE + teamIndex].erase(itr2);
                break;
            }
        }
    }
    return true;
}

void BattlegroundQueue::UpdateEvents(uint32 diff)
{
    m_events.Update(diff);
}

bool BattlegroundQueue::TryStartBotFilledMatch(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry,
    BattlegroundBracketId bracket_id, uint32 maxPlayersPerTeam, uint8 arenaType, uint32 queueWaitMs)
{
    // CheckNormalMatch leaves whatever it had gathered in the pools when it
    // gives up; start from nothing.
    m_SelectionPools[TEAM_ALLIANCE].Init();
    m_SelectionPools[TEAM_HORDE].Init();

    // Every real group waiting in this bracket rides along, premades included -
    // the thirty-minute premade hold exists to find them premade opponents, and
    // here the opponents are clones. Groups made only of managed bots are left
    // waiting as stock: a match is summoned for people, and the bots can fill
    // one that exists.
    //
    // Collected before any of them is seated, because seating a group can move
    // it from one side's queue list to the other and cut this walk out from
    // under itself.
    std::vector<GroupQueueInfo*> waiting;
    for (uint32 queueIndex = BG_QUEUE_PREMADE_ALLIANCE; queueIndex < BG_QUEUE_GROUP_TYPES_COUNT; ++queueIndex)
    {
        for (GroupQueueInfo* ginfo : m_QueuedGroups[bracket_id][queueIndex])
        {
            if (ginfo->IsInvitedToBGInstanceGUID || ginfo->IsRated || !InActivePool(ginfo) ||
                (arenaType && ginfo->ArenaType != arenaType) || !GroupHasRealPlayerInvitee(ginfo))
                continue;

            // Arenas only, and only where somebody asked to be left out: they
            // keep waiting for people the way the stock queue would, while the
            // rest of the bracket is seated around them.
            if (arenaType && !GroupAllowsArenaBotFill(ginfo))
                continue;

            waiting.push_back(ginfo);
        }
    }

    uint32 const nowMs = GameTime::GetGameTimeMS();
    bool someoneHasWaitedLongEnough = false;
    std::vector<GroupQueueInfo*> crossedOver;   // seated opposite the side they queued on

    for (GroupQueueInfo* ginfo : waiting)
    {
        TeamId const queuedSide = ginfo->Team == HORDE ? TEAM_HORDE : TEAM_ALLIANCE;
        uint32 before = m_SelectionPools[queuedSide].GetPlayerCount();
        m_SelectionPools[queuedSide].AddGroup(ginfo, maxPlayersPerTeam, queuedSide);
        bool seated = m_SelectionPools[queuedSide].GetPlayerCount() != before;

        // The side the queue put this group on is full, so try the other one.
        // An arena queue never really picks a side - AddGroup only balances
        // sides for battlegrounds - so on a realm where everybody shares a
        // faction every arena group is nominally Alliance, and the second party
        // waiting used to be dropped here and handed a clone-filled match of its
        // own instead of being seated opposite the first. Battleground sides are
        // already balanced when the group joins and Scarlet Chapel asks for its
        // side outright, so only an arena group reaches across.
        if (!seated && arenaType && !ginfo->IsForcedTeam)
        {
            TeamId const otherSide = queuedSide == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
            before = m_SelectionPools[otherSide].GetPlayerCount();
            m_SelectionPools[otherSide].AddGroup(ginfo, maxPlayersPerTeam, otherSide);
            if (m_SelectionPools[otherSide].GetPlayerCount() != before)
            {
                seated = true;
                crossedOver.push_back(ginfo);
            }
        }

        if (!seated)
            continue;   // too large for what is left of either side

        if (getMSTimeDiff(ginfo->JoinTime, nowMs) >= queueWaitMs)
            someoneHasWaitedLongEnough = true;
    }

    // Nothing starts, so nothing moved: AddGroup already rewrote the team of
    // everyone it seated, and the groups that crossed over have to keep waiting
    // on the side they queued on.
    auto const abandonSelection = [this, &crossedOver]()
    {
        for (GroupQueueInfo* ginfo : crossedOver)
            ginfo->Team = ginfo->Team == HORDE ? ALLIANCE : HORDE;

        m_SelectionPools[TEAM_ALLIANCE].Init();
        m_SelectionPools[TEAM_HORDE].Init();
    };

    uint32 const selected = m_SelectionPools[TEAM_ALLIANCE].GetPlayerCount() + m_SelectionPools[TEAM_HORDE].GetPlayerCount();
    if (!selected || !someoneHasWaitedLongEnough)
    {
        abandonSelection();
        return false;
    }

    // This path creates a clone-filled match even though its queued groups are
    // real-player groups, so select from the bot-safe arena subset.
    Battleground* bg = sBattlegroundMgr->CreateNewBattleground(bgTypeId, bracketEntry, arenaType, false, false, true);
    if (!bg)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::TryStartBotFilledMatch - Cannot create battleground: {}", uint32(bgTypeId));
        abandonSelection();
        return false;
    }

    for (GroupQueueInfo* ginfo : crossedOver)
        MoveGroupToSeatedSide(ginfo, bracket_id);

    // Flagged before anyone is invited so the first person's entry is
    // accounted as a human's under the bot-fill reading of the roster.
    bg->SetBotFillMatch(true);
    bg->SetTournamentPool(m_ActiveTournamentPool);

    for (uint32 i = 0; i < PVP_TEAMS_COUNT; ++i)
        for (GroupQueueInfo* ginfo : m_SelectionPools[TEAM_ALLIANCE + i].SelectedGroups)
            InviteGroupToBG(ginfo, bg, ginfo->Team);

    TC_LOG_DEBUG("bg.battleground", "BattlegroundQueue::TryStartBotFilledMatch: started bot-filled match type {} instance {} bracket {} arenaType {} for {} real player(s).",
        uint32(bgTypeId), bg->GetInstanceID(), uint32(bracket_id), uint32(arenaType), selected);

    bg->StartBattleground();
    if (arenaType)
        bg->RemoveFromBGFreeSlotQueue();

    m_SelectionPools[TEAM_ALLIANCE].Init();
    m_SelectionPools[TEAM_HORDE].Init();
    return true;
}

// Arenas never enter the free-slot queue, so they are found here by type. A
// clone's seat is offered, and so is a seat nobody holds yet - not even a clone
// still loading into it (Battleground::CountUnclaimedPreparationSeats), which
// is what lets two people who queue a second apart meet: the first one's match
// has no clones yet when the second clicks. A clone's seat holds the countdown
// while its taker is on the way in and restarts it when they arrive
// (Battleground::HandOverPreparationSeat).
void BattlegroundQueue::OfferPreparationSeats(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id,
    uint8 arenaType, uint32 queueWaitMs)
{
    BattlegroundContainer const* instances = sBattlegroundMgr->GetBattlegroundsByType(bgTypeId);
    if (!instances)
        return;

    // Oldest first. Entry 0 is the template. Skirmishes, and the battlegrounds
    // seated like them (Battleground::TakesSeatsInPreparationOnly, arenaType 0).
    std::vector<Battleground*> arenas;
    for (auto const& [instanceId, bg] : *instances)
        if (instanceId && (bg->isArena() ? !bg->isRated() && bg->GetArenaType() == arenaType : bg->TakesSeatsInPreparationOnly()) &&
            !bg->IsCustomGame() && bg->IsBotFillMatch() && bg->GetBracketId() == bracket_id && InActivePool(bg) &&
            bg->GetStatus() == STATUS_WAIT_JOIN)
            arenas.push_back(bg);

    if (arenas.empty())
        return;

    // The same groups TryStartBotFilledMatch would seat, collected before any
    // of them is moved to the other side's list. That includes the "Arena bot
    // matches" box: a group is offered a clone's seat only when every real
    // member has it ticked (re-checked at Enter Battle, since it can change).
    // A Buffle Shuffle party, queued as one entry per member, is taken the same
    // way - all of it or none, so one member who has not waited the wait or
    // has the box off keeps the whole party in the queue.
    struct Waiting
    {
        std::vector<GroupQueueInfo*> Entries;
        bool Ready = true;      // every entry has waited and lets bots in
        bool Real = false;      // somebody in it is a person
    };

    uint32 const nowMs = GameTime::GetGameTimeMS();
    std::vector<Waiting> waiting;
    std::map<uint32, size_t> shuffleParties;
    for (uint32 queueIndex = BG_QUEUE_PREMADE_ALLIANCE; queueIndex < BG_QUEUE_GROUP_TYPES_COUNT; ++queueIndex)
    {
        for (GroupQueueInfo* ginfo : m_QueuedGroups[bracket_id][queueIndex])
        {
            if (ginfo->IsInvitedToBGInstanceGUID || ginfo->IsRated || !InActivePool(ginfo) || ginfo->ArenaType != arenaType)
                continue;

            Waiting* entry = nullptr;
            if (ginfo->ShuffleParty)
            {
                auto const [itr, added] = shuffleParties.try_emplace(ginfo->ShuffleParty, waiting.size());
                if (added)
                    waiting.emplace_back();
                entry = &waiting[itr->second];
            }
            else
                entry = &waiting.emplace_back();

            entry->Entries.push_back(ginfo);
            // the "Arena bot matches" box is an arena queue's; a battleground's
            // queue is one with bots already
            entry->Ready = entry->Ready && getMSTimeDiff(ginfo->JoinTime, nowMs) >= queueWaitMs &&
                (!arenaType || GroupAllowsArenaBotFill(ginfo));
            entry->Real = entry->Real || GroupHasRealPlayerInvitee(ginfo);
        }
    }

    // One queue entry's seats, in one of `candidates`. False when none of them
    // has the room.
    auto const seat = [&](GroupQueueInfo* ginfo, std::vector<Battleground*> const& candidates)
    {
        // A group is a team: all of it goes to one side or none of it does.
        uint32 const groupSize = uint32(ginfo->Players.size());
        Battleground* target = nullptr;
        uint32 targetTeam = 0;
        uint32 targetPeople = 0;
        uint32 targetMatchPeople = 0;
        bool targetUnclaimed = false;
        for (Battleground* bg : candidates)
        {
            // A shuffle deals its own sides every round, so what matters is
            // that people end up in one match: the one with the most already
            // in or on the way.
            uint32 const matchPeople = bg->IsSoloShuffle() ? CountPeopleOnTeam(bg, ALLIANCE) + CountPeopleOnTeam(bg, HORDE) : 0;
            for (uint32 team : { uint32(ALLIANCE), uint32(HORDE) })
            {
                // Seats nobody holds first, so no clone has to stand down.
                bool const unclaimed = bg->CountUnclaimedPreparationSeats(team) >= groupSize;
                if (!unclaimed && bg->CountPreparationSeatsHeldByClones(team) < groupSize)
                    continue;

                // Then the side with fewer people on it, so somebody queuing
                // alone is set against the person already waiting, not beside them.
                uint32 const people = CountPeopleOnTeam(bg, team);
                bool better = !target;
                if (!better && matchPeople != targetMatchPeople)
                    better = matchPeople > targetMatchPeople;
                else if (!better && people != targetPeople)
                    better = people < targetPeople;
                else if (!better)
                    better = unclaimed && !targetUnclaimed;

                if (better)
                {
                    target = bg;
                    targetTeam = team;
                    targetPeople = people;
                    targetMatchPeople = matchPeople;
                    targetUnclaimed = unclaimed;
                }
            }
        }

        if (!target)
            return false;

        if (ginfo->Team != targetTeam)
        {
            ginfo->Team = targetTeam;
            MoveGroupToSeatedSide(ginfo, bracket_id);
        }

        if (!InviteGroupToBG(ginfo, target, targetTeam))
            return false;

        if (!targetUnclaimed)
            for (auto const& playerEntry : ginfo->Players)
                target->ReservePreparationSeat(playerEntry.first, targetTeam);

        TC_LOG_DEBUG("bg.battleground", "BattlegroundQueue::OfferPreparationSeats: invited {} player(s) into {} on team {} of preparing arena instance {} (bracket {}, arenaType {}).",
            groupSize, targetUnclaimed ? "open seats" : "clones' seats", targetTeam, target->GetInstanceID(), uint32(bracket_id), uint32(arenaType));
        return true;
    };

    for (Waiting const& entry : waiting)
    {
        if (!entry.Ready || !entry.Real)
            continue;

        if (entry.Entries.size() == 1)
        {
            seat(entry.Entries.front(), arenas);
            continue;
        }

        // A shuffle party wants one match with a seat for each member, on
        // either side - the match deals the sides itself, and friends fight
        // each other in it like anyone else. Each seat taken there costs that
        // count exactly one (open seats on a side go before its clones'), so
        // the members are then seated one by one as anybody alone would be.
        Battleground* target = nullptr;
        uint32 targetPeople = 0;
        for (Battleground* bg : arenas)
        {
            uint32 seats = 0;
            for (uint32 team : { uint32(ALLIANCE), uint32(HORDE) })
                seats += bg->CountUnclaimedPreparationSeats(team) + bg->CountPreparationSeatsHeldByClones(team);
            if (!bg->IsSoloShuffle() || seats < entry.Entries.size())
                continue;

            uint32 const people = CountPeopleOnTeam(bg, ALLIANCE) + CountPeopleOnTeam(bg, HORDE);
            if (!target || people > targetPeople)
            {
                target = bg;
                targetPeople = people;
            }
        }

        if (target)
            for (GroupQueueInfo* member : entry.Entries)
                seat(member, { target });
    }
}

// A group seated opposite the side it queued on belongs in that side's list,
// the way CheckSkirmishForSameFaction moves one, so that everything reading the
// lists by side - RemovePlayer among them - agrees with the team it plays on.
void BattlegroundQueue::MoveGroupToSeatedSide(GroupQueueInfo* ginfo, BattlegroundBracketId bracket_id)
{
    TeamId const seatedSide = ginfo->Team == HORDE ? TEAM_HORDE : TEAM_ALLIANCE;
    TeamId const queuedSide = seatedSide == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    for (uint32 base : { uint32(BG_QUEUE_PREMADE_ALLIANCE), uint32(BG_QUEUE_NORMAL_ALLIANCE) })
    {
        GroupsQueueType& from = m_QueuedGroups[bracket_id][base + queuedSide];
        GroupsQueueType::iterator itr = std::find(from.begin(), from.end(), ginfo);
        if (itr == from.end())
            continue;

        from.erase(itr);
        m_QueuedGroups[bracket_id][base + seatedSide].push_front(ginfo);
        break;
    }
}

// The queue of a squad match (Baradin Hold: Four Corners). Groups are made into
// squads of the arena size here - a party is never split, people queuing alone
// make up squads with each other - and a match starts in one of three ways:
//
//   1. Every squad can be filled with people: at once, nobody else needed.
//   2. Where this realm fills skirmishes with bots, once someone who has "Arena
//      bot matches" on has waited the skirmish wait: everyone waiting who has it
//      on plays now, spread over the squads, and the bot fill finishes every
//      squad - so a lone player gets a full four-squad match.
//   3. Otherwise, two or more squads full of people that have waited
//      kSquadGatherMs, so a third and fourth have a chance to form: just those.
//      The match is closed to bots, and the corners nobody came for stay empty.
//
// Squads are numbered as the corners are in `battleground_squad_start`; the
// even ones are seated on the Alliance side and the odd ones on the Horde side
// (Battleground::GetSquadSide), so a two-squad match uses opposite corners.
namespace
{
    constexpr uint32 kSquadGatherMs = 60 * IN_MILLISECONDS;
}

void BattlegroundQueue::UpdateSquadPool(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id, uint8 arenaType)
{
    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate || !bgTemplate->IsSquadMatch() || !arenaType)
        return;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketById(bgTemplate->GetMapId(), bracket_id);
    if (!bracketEntry)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::UpdateSquadPool: no bracket {} for map {}", uint32(bracket_id), bgTemplate->GetMapId());
        return;
    }

    uint8 const squadCount = bgTemplate->GetSquadCount();
    uint32 const squadSize = arenaType;
    uint32 const nowMs = GameTime::GetGameTimeMS();

    UpdateRatedSquadPool(bgTypeId, bracketEntry, bracket_id, squadCount, arenaType);

    // Everyone waiting for this squad size, longest-waiting first.
    std::vector<GroupQueueInfo*> waiting;
    for (uint32 queueIndex = BG_QUEUE_PREMADE_ALLIANCE; queueIndex < BG_QUEUE_GROUP_TYPES_COUNT; ++queueIndex)
        for (GroupQueueInfo* ginfo : m_QueuedGroups[bracket_id][queueIndex])
            if (!ginfo->IsInvitedToBGInstanceGUID && !ginfo->IsRated && InActivePool(ginfo) &&
                ginfo->ArenaType == arenaType && !ginfo->Players.empty() && ginfo->Players.size() <= squadSize)
                waiting.push_back(ginfo);

    if (waiting.empty())
        return;

    std::stable_sort(waiting.begin(), waiting.end(),
        [](GroupQueueInfo const* left, GroupQueueInfo const* right) { return left->JoinTime < right->JoinTime; });

    // Into the fullest squad a group still fits when the point is to finish
    // squads; into the emptiest when bots will finish them anyway, so the
    // people waiting are spread over the corners rather than stacked in one.
    auto const makeSquads = [&](std::vector<GroupQueueInfo*> const& groups, bool spread)
    {
        std::vector<QueuedSquad> squads(squadCount);
        for (GroupQueueInfo* ginfo : groups)
        {
            uint32 const groupSize = uint32(ginfo->Players.size());
            QueuedSquad* target = nullptr;
            for (QueuedSquad& squad : squads)
            {
                if (squad.Size + groupSize > squadSize)
                    continue;

                if (!target || (spread ? squad.Size < target->Size : squad.Size > target->Size))
                    target = &squad;
            }

            if (target)
            {
                target->Groups.push_back(ginfo);
                target->Size += groupSize;
            }
        }

        return squads;
    };

    auto const waitedLongest = [nowMs](QueuedSquad const& squad)
    {
        uint32 longest = 0;
        for (GroupQueueInfo const* ginfo : squad.Groups)
            longest = std::max(longest, getMSTimeDiff(ginfo->JoinTime, nowMs));
        return longest;
    };

    // 1. Full squads all round.
    {
        std::vector<QueuedSquad> const squads = makeSquads(waiting, false);
        if (std::all_of(squads.begin(), squads.end(), [squadSize](QueuedSquad const& squad) { return squad.Size == squadSize; }))
        {
            StartSquadMatch(bgTypeId, bracketEntry, bracket_id, arenaType, squads, false);
            return;
        }
    }

    // 2. Bots finish the squads.
    if (sBattlegroundMgr->IsBotFillSkirmishArena(arenaType))
    {
        uint32 const queueWaitMs = sBattlegroundMgr->GetBotFillQueueWaitMs(arenaType);
        std::vector<GroupQueueInfo*> botsWelcome;
        bool someoneHasWaitedLongEnough = false;
        for (GroupQueueInfo* ginfo : waiting)
        {
            if (!GroupHasRealPlayerInvitee(ginfo) || !GroupAllowsArenaBotFill(ginfo))
                continue;

            botsWelcome.push_back(ginfo);
            if (getMSTimeDiff(ginfo->JoinTime, nowMs) >= queueWaitMs)
                someoneHasWaitedLongEnough = true;
        }

        if (someoneHasWaitedLongEnough)
        {
            StartSquadMatch(bgTypeId, bracketEntry, bracket_id, arenaType, makeSquads(botsWelcome, true), true);
            return;
        }
    }

    // 3. The full squads there are, once they have given others time to form.
    std::vector<QueuedSquad> fullSquads;
    bool gathered = false;
    for (QueuedSquad const& squad : makeSquads(waiting, false))
    {
        if (squad.Size != squadSize)
            continue;

        if (waitedLongest(squad) >= kSquadGatherMs)
            gathered = true;
        fullSquads.push_back(squad);
    }

    if (fullSquads.size() >= 2 && gathered)
        StartSquadMatch(bgTypeId, bracketEntry, bracket_id, arenaType, fullSquads, false);
}

// Rated: each arena team is a whole squad, so this is matchmaking proper. The
// longest-waiting team picks the field: teams within the arena rating window of
// it (CONFIG_ARENA_MAX_RATING_DIFFERENCE, or anyone once they have waited out
// the rating discard timer), closest first, never two of the same arena team.
// A full field starts at once; two or three teams start once the first has
// waited kSquadGatherMs. A rated match is never padded with bots.
void BattlegroundQueue::UpdateRatedSquadPool(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry,
    BattlegroundBracketId bracket_id, uint8 squadCount, uint8 arenaType)
{
    std::vector<GroupQueueInfo*> teams;
    for (uint32 queueIndex = BG_QUEUE_PREMADE_ALLIANCE; queueIndex < BG_QUEUE_GROUP_TYPES_COUNT; ++queueIndex)
        for (GroupQueueInfo* ginfo : m_QueuedGroups[bracket_id][queueIndex])
            if (!ginfo->IsInvitedToBGInstanceGUID && ginfo->IsRated && InActivePool(ginfo) &&
                ginfo->ArenaType == arenaType && ginfo->Players.size() == arenaType)
                teams.push_back(ginfo);

    if (teams.size() < 2)
        return;

    std::stable_sort(teams.begin(), teams.end(),
        [](GroupQueueInfo const* left, GroupQueueInfo const* right) { return left->JoinTime < right->JoinTime; });

    GroupQueueInfo* anchor = teams.front();
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32 const maxDifference = sBattlegroundMgr->GetMaxRatingDifference();
    int32 const discardTime = int32(nowMs) - int32(sBattlegroundMgr->GetRatingDiscardTimer());
    auto const difference = [anchor](GroupQueueInfo const* ginfo)
    {
        return uint32(std::abs(int32(ginfo->ArenaMatchmakerRating) - int32(anchor->ArenaMatchmakerRating)));
    };

    std::vector<GroupQueueInfo*> candidates;
    for (auto itr = std::next(teams.begin()); itr != teams.end(); ++itr)
        if (difference(*itr) <= maxDifference || int32((*itr)->JoinTime) < discardTime || int32(anchor->JoinTime) < discardTime)
            candidates.push_back(*itr);

    std::stable_sort(candidates.begin(), candidates.end(),
        [&difference](GroupQueueInfo const* left, GroupQueueInfo const* right) { return difference(left) < difference(right); });

    std::vector<QueuedSquad> field(1);
    field.back().Groups.push_back(anchor);
    field.back().Size = arenaType;
    for (GroupQueueInfo* ginfo : candidates)
    {
        if (field.size() >= squadCount)
            break;

        bool const sameTeam = std::any_of(field.begin(), field.end(),
            [ginfo](QueuedSquad const& squad) { return squad.Groups.front()->ArenaTeamId == ginfo->ArenaTeamId; });
        if (sameTeam)
            continue;

        field.emplace_back();
        field.back().Groups.push_back(ginfo);
        field.back().Size = arenaType;
    }

    if (field.size() < 2)
        return;

    if (field.size() < squadCount && getMSTimeDiff(anchor->JoinTime, nowMs) < kSquadGatherMs)
        return;

    StartSquadMatch(bgTypeId, bracketEntry, bracket_id, arenaType, field, false, true);
}

void BattlegroundQueue::StartSquadMatch(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry,
    BattlegroundBracketId bracket_id, uint8 arenaType, std::vector<QueuedSquad> const& squads, bool botFill, bool rated)
{
    Battleground* bg = sBattlegroundMgr->CreateNewBattleground(bgTypeId, bracketEntry, arenaType, rated, false, botFill);
    if (!bg)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::StartSquadMatch: cannot create battleground {}", uint32(bgTypeId));
        return;
    }

    bg->SetTournamentPool(m_ActiveTournamentPool);
    // Flagged before anyone is invited, as TryStartBotFilledMatch does, so the
    // first person in is accounted as a person among bots.
    bg->SetBotFillMatch(botFill);
    bg->SetBotFillRefused(!botFill);

    // A rated match knows every team before anyone is invited, so each can be
    // told who it plays against: the others' average, which is what anyone who
    // refuses the invite is counted as losing to (RemovePlayer).
    if (rated)
    {
        for (uint8 squad = 0; squad < squads.size() && squad < bg->GetSquadCount(); ++squad)
            if (!squads[squad].Groups.empty())
                bg->SetSquadArenaTeam(squad, squads[squad].Groups.front()->ArenaTeamId, squads[squad].Groups.front()->ArenaMatchmakerRating);

        for (uint8 squad = 0; squad < squads.size() && squad < bg->GetSquadCount(); ++squad)
        {
            if (squads[squad].Groups.empty())
                continue;

            uint32 teamRatingSum = 0;
            uint32 others = 0;
            for (uint8 other = 0; other < squads.size(); ++other)
            {
                if (other == squad || squads[other].Groups.empty())
                    continue;
                teamRatingSum += squads[other].Groups.front()->ArenaTeamRating;
                ++others;
            }

            GroupQueueInfo* ginfo = squads[squad].Groups.front();
            ginfo->OpponentsMatchmakerRating = bg->GetSquadOpponentsMatchmakerRating(squad);
            ginfo->OpponentsTeamRating = others ? teamRatingSum / others : ginfo->ArenaTeamRating;
        }
    }

    uint32 invited = 0;
    for (uint8 squad = 0; squad < squads.size() && squad < bg->GetSquadCount(); ++squad)
    {
        uint32 const side = Battleground::GetSquadSide(squad);
        for (GroupQueueInfo* ginfo : squads[squad].Groups)
        {
            if (ginfo->Team != side)
            {
                ginfo->Team = side;
                MoveGroupToSeatedSide(ginfo, bracket_id);
            }

            for (auto const& [playerGuid, playerInfo] : ginfo->Players)
            {
                (void)playerInfo;
                bg->SetSquadSeat(playerGuid, squad);
            }

            if (InviteGroupToBG(ginfo, bg, side))
                invited += uint32(ginfo->Players.size());
        }
    }

    TC_LOG_DEBUG("bg.battleground", "BattlegroundQueue::StartSquadMatch: started {} instance {} ({}, squads of {}, bracket {}) for {} player(s), bot fill {}.",
        bg->GetName(), bg->GetInstanceID(), rated ? "rated" : "skirmish", uint32(arenaType), uint32(bracket_id), invited, botFill ? "on" : "off");

    bg->StartBattleground();
    // An arena admits nobody once it is made; latecomers wait for the next.
    bg->RemoveFromBGFreeSlotQueue();
}

// Buffle Shuffle's queues, one per size: people queued alone, made into one match
// of four (2v2) or six (3v3), two or three a side for the first round; the
// match deals the rest. A party of up to a whole match can queue too - one
// queue entry per member, all with the same ShuffleParty - and is only ever
// taken whole; inside the match its members are dealt like anyone else, so
// friends play with and against each other. Roles count only with
// Centurion.SoloShuffle.BalanceHealers on; off, every waiting player is taken
// as a non-healer and a full lobby is the longest-waiting four or six that
// parties can make up exactly. A match starts
//
//   1. as soon as enough are waiting to fill it - with healers balanced, two
//      healers and the others, one healer to a side every round;
//   2. where this realm fills skirmishes with bots, once someone with "Arena
//      bot matches" ticked has waited the skirmish wait: everyone waiting with
//      it ticked plays now, up to a full match, and the bot fill takes the
//      other seats.
//      A shuffle still behind its first gates is offered first, in place of
//      its clones (OfferPreparationSeats), so people meet rather than each
//      getting a match of bots;
//   3. otherwise (healers balanced, and not two of them waiting), once the
//      longest-waiting has waited kShuffleAnyLobbyMs, any full match: two
//      healers at most while others are there to take the seats.
//
// Rated Buffle Shuffle does not exist: the window only offers the skirmish.
namespace
{
    constexpr uint32 kShuffleAnyLobbyMs = 60 * IN_MILLISECONDS;
    constexpr uint32 kShuffleHealers = 2;
    // Lobbies FindShuffleLobby tries before it gives up. Nothing real comes
    // close: a queue of people alone is answered by the first one it tries.
    constexpr uint32 kShuffleLobbySearchLimit = 20000;

    struct ShuffleCandidate
    {
        GroupQueueInfo* Group;
        bool Healer;
    };

    // Somebody queued alone, or a party queued together.
    struct ShuffleParty
    {
        std::vector<ShuffleCandidate> Members;
        uint32 Healers = 0;
        uint32 JoinTime = 0;
    };

    // Whole parties making up exactly `count` people, with `minHealers` to
    // `maxHealers` healers among them - or nothing, when no choice of them
    // does. Parties with a healer are looked at first and then the rest, each
    // longest-waiting first, and the first choice that works in that order is
    // taken: of people queued alone, the healers wanted and then everyone else
    // by how long they have waited.
    std::vector<ShuffleCandidate> FindShuffleLobby(std::vector<ShuffleParty> const& waiting, uint32 count,
        uint32 minHealers, uint32 maxHealers)
    {
        std::vector<ShuffleParty const*> order;
        for (ShuffleParty const& party : waiting)
            if (!party.Members.empty() && party.Members.size() <= count)
                order.push_back(&party);
        std::stable_partition(order.begin(), order.end(), [](ShuffleParty const* party) { return party->Healers > 0; });

        // What is left from each place on, so a search stops once it cannot be enough.
        std::vector<uint32> peopleFrom(order.size() + 1, 0);
        std::vector<uint32> healersFrom(order.size() + 1, 0);
        for (size_t i = order.size(); i-- > 0;)
        {
            peopleFrom[i] = peopleFrom[i + 1] + uint32(order[i]->Members.size());
            healersFrom[i] = healersFrom[i + 1] + order[i]->Healers;
        }

        std::vector<ShuffleParty const*> chosen;
        uint32 tried = 0;
        std::function<bool(size_t, uint32, uint32)> search = [&](size_t from, uint32 seats, uint32 healers)
        {
            if (!seats)
                return healers >= minHealers;

            for (size_t i = from; i < order.size(); ++i)
            {
                if (++tried > kShuffleLobbySearchLimit || peopleFrom[i] < seats || healers + healersFrom[i] < minHealers)
                    return false;

                ShuffleParty const* party = order[i];
                uint32 const size = uint32(party->Members.size());
                uint32 const withParty = healers + party->Healers;
                // Too many, too many healers, or too few seats left for the healers still wanted.
                if (size > seats || withParty > maxHealers || (withParty < minHealers && minHealers - withParty > seats - size))
                    continue;

                chosen.push_back(party);
                if (search(i + 1, seats - size, withParty))
                    return true;
                chosen.pop_back();
            }

            return false;
        };

        std::vector<ShuffleCandidate> lobby;
        if (count && search(0, count, 0))
            for (ShuffleParty const* party : chosen)
                lobby.insert(lobby.end(), party->Members.begin(), party->Members.end());
        return lobby;
    }

    // A full lobby, with at most `maxHealers` healers unless there are not
    // enough others to fill the seats; nothing when parties cannot make one up.
    std::vector<ShuffleCandidate> FindFullShuffleLobby(std::vector<ShuffleParty> const& waiting, uint32 count, uint32 maxHealers)
    {
        std::vector<ShuffleCandidate> lobby = FindShuffleLobby(waiting, count, 0, maxHealers);
        if (lobby.empty())
            lobby = FindShuffleLobby(waiting, count, 0, count);
        return lobby;
    }

    // As many as one match seats, up to `count`, the same way.
    std::vector<ShuffleCandidate> PickShuffleLobby(std::vector<ShuffleParty> const& waiting, uint32 count, uint32 maxHealers)
    {
        for (uint32 people = count; people > 0; --people)
            if (std::vector<ShuffleCandidate> lobby = FindFullShuffleLobby(waiting, people, maxHealers); !lobby.empty())
                return lobby;

        return {};
    }
}

void BattlegroundQueue::UpdateSoloShufflePool(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id, uint8 arenaType)
{
    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate || !arenaType)
        return;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketById(bgTemplate->GetMapId(), bracket_id);
    if (!bracketEntry)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::UpdateSoloShufflePool: no bracket {} for map {}", uint32(bracket_id), bgTemplate->GetMapId());
        return;
    }

    uint32 const seats = uint32(arenaType) * 2;
    uint32 const nowMs = GameTime::GetGameTimeMS();
    bool const balanceHealers = sWorld->getBoolConfig(CONFIG_CENTURION_SOLO_SHUFFLE_BALANCE_HEALERS);

    // Everyone waiting, alone or as a party, longest-waiting first, with the
    // role they would play now: talents can be changed in the queue.
    auto const collect = [&]()
    {
        std::vector<ShuffleParty> waiting;
        std::map<uint32, size_t> parties;
        for (uint32 queueIndex = BG_QUEUE_PREMADE_ALLIANCE; queueIndex < BG_QUEUE_GROUP_TYPES_COUNT; ++queueIndex)
        {
            for (GroupQueueInfo* ginfo : m_QueuedGroups[bracket_id][queueIndex])
            {
                if (ginfo->IsInvitedToBGInstanceGUID || ginfo->IsRated || !InActivePool(ginfo) ||
                    ginfo->ArenaType != arenaType || ginfo->Players.size() != 1)
                    continue;

                Player const* player = ObjectAccessor::FindConnectedPlayer(ginfo->Players.begin()->first);
                if (!player)
                    continue;

                ShuffleParty* party = nullptr;
                if (ginfo->ShuffleParty)
                {
                    auto const [itr, added] = parties.try_emplace(ginfo->ShuffleParty, waiting.size());
                    if (added)
                        waiting.emplace_back();
                    party = &waiting[itr->second];
                }
                else
                    party = &waiting.emplace_back();

                bool const healer = balanceHealers && Arena::IsSoloShuffleHealer(player);
                party->JoinTime = party->Members.empty() ? ginfo->JoinTime : std::min(party->JoinTime, ginfo->JoinTime);
                party->Members.push_back({ ginfo, healer });
                party->Healers += healer ? 1 : 0;
            }
        }

        std::stable_sort(waiting.begin(), waiting.end(),
            [](ShuffleParty const& left, ShuffleParty const& right) { return left.JoinTime < right.JoinTime; });
        return waiting;
    };

    auto const start = [&](std::vector<ShuffleCandidate> const& picked, bool botFill)
    {
        std::vector<GroupQueueInfo*> groups;
        std::vector<bool> healers;
        for (ShuffleCandidate const& candidate : picked)
        {
            groups.push_back(candidate.Group);
            healers.push_back(candidate.Healer);
        }
        StartSoloShuffle(bgTypeId, bracketEntry, bracket_id, arenaType, groups, healers, botFill);
    };

    std::vector<ShuffleParty> waiting = collect();
    if (waiting.empty())
        return;

    // 1. A full match: any, or with healers balanced two healers and the others.
    {
        std::vector<ShuffleCandidate> const lobby = balanceHealers
            ? FindShuffleLobby(waiting, seats, kShuffleHealers, kShuffleHealers)
            : FindShuffleLobby(waiting, seats, 0, seats);
        if (!lobby.empty())
        {
            start(lobby, false);
            return;
        }
    }

    // 2. Bots take the seats nobody is waiting for.
    if (sBattlegroundMgr->IsBotFillSkirmishArena(arenaType))
    {
        uint32 const queueWaitMs = sBattlegroundMgr->GetBotFillQueueWaitMs(arenaType);
        OfferPreparationSeats(bgTypeId, bracket_id, arenaType, queueWaitMs);

        // A party plays among bots only when nobody in it has said no to them.
        std::vector<ShuffleParty> botsWelcome;
        bool someoneHasWaitedLongEnough = false;
        for (ShuffleParty const& party : collect())
        {
            auto const hasPerson = [](ShuffleCandidate const& member) { return GroupHasRealPlayerInvitee(member.Group); };
            auto const allowsBots = [](ShuffleCandidate const& member) { return GroupAllowsArenaBotFill(member.Group); };
            if (std::none_of(party.Members.begin(), party.Members.end(), hasPerson) ||
                !std::all_of(party.Members.begin(), party.Members.end(), allowsBots))
                continue;

            botsWelcome.push_back(party);
            if (getMSTimeDiff(party.JoinTime, nowMs) >= queueWaitMs)
                someoneHasWaitedLongEnough = true;
        }

        if (someoneHasWaitedLongEnough)
        {
            start(PickShuffleLobby(botsWelcome, seats, kShuffleHealers), true);
            return;
        }

        waiting = collect();
        if (waiting.empty())
            return;
    }

    // 3. A full match of anyone, once the first in line has waited for a better mix.
    if (getMSTimeDiff(waiting.front().JoinTime, nowMs) >= kShuffleAnyLobbyMs)
        if (std::vector<ShuffleCandidate> const lobby = FindFullShuffleLobby(waiting, seats, kShuffleHealers); !lobby.empty())
            start(lobby, false);
}

void BattlegroundQueue::StartSoloShuffle(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry,
    BattlegroundBracketId bracket_id, uint8 arenaType, std::vector<GroupQueueInfo*> players, std::vector<bool> const& healers, bool botFill)
{
    if (players.empty())
        return;

    bool hasBotParticipants = botFill;
    for (GroupQueueInfo const* ginfo : players)
        if (GroupHasBotInvitee(ginfo))
            hasBotParticipants = true;

    Battleground* bg = sBattlegroundMgr->CreateNewBattleground(bgTypeId, bracketEntry, arenaType, false, false, hasBotParticipants);
    if (!bg)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::StartSoloShuffle: cannot create battleground {}", uint32(bgTypeId));
        return;
    }

    bg->SetTournamentPool(m_ActiveTournamentPool);
    // Flagged before anyone is invited, as TryStartBotFilledMatch does, so the
    // first person in is accounted as a person among bots.
    bg->SetBotFillMatch(botFill);
    bg->SetBotFillRefused(!botFill);

    // Round 1's sides: healers first, then everyone else, dealt alternately -
    // healers apart and the sides level. People waiting together on a bot
    // match are set against each other, not beside each other.
    std::vector<size_t> order(players.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_partition(order.begin(), order.end(), [&healers](size_t i) { return i < healers.size() && healers[i]; });

    uint32 invited = 0;
    for (size_t seat = 0; seat < order.size(); ++seat)
    {
        GroupQueueInfo* ginfo = players[order[seat]];
        uint32 const side = (seat % 2) == 0 ? ALLIANCE : HORDE;
        if (ginfo->Team != side)
        {
            ginfo->Team = side;
            MoveGroupToSeatedSide(ginfo, bracket_id);
        }

        if (InviteGroupToBG(ginfo, bg, side))
            invited += uint32(ginfo->Players.size());
    }

    TC_LOG_DEBUG("bg.battleground", "BattlegroundQueue::StartSoloShuffle: started instance {} (bracket {}) for {} player(s), bot fill {}.",
        bg->GetInstanceID(), uint32(bracket_id), invited, botFill ? "on" : "off");

    bg->StartBattleground();
    // An arena admits nobody once it is made; latecomers wait for the next,
    // or take a clone's seat while the first gates are shut.
    bg->RemoveFromBGFreeSlotQueue();
}

/*
this method is called when group is inserted, or player / group is removed from BG Queue - there is only one player's status changed, so we don't use while (true) cycles to invite whole queue
it must be called after fully adding the members of a group to ensure group joining
should be called from Battleground::RemovePlayer function in some cases
*/
void BattlegroundQueue::BattlegroundQueueUpdate(uint32 /*diff*/, BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id, uint8 arenaType, bool isRated, uint32 arenaRating)
{
    //if no players in queue - do nothing
    if (m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].empty() &&
        m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].empty() &&
        m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_ALLIANCE].empty() &&
        m_QueuedGroups[bracket_id][BG_QUEUE_NORMAL_HORDE].empty())
        return;

    // One full matching pass per queue pool, world first. A pool with nobody
    // waiting has nothing to invite, refill or start, so it is skipped - which
    // on a realm without tournament characters leaves exactly one pass, as before.
    for (bool const tournamentPool : { false, true })
    {
        if (!HasWaitingGroupsInPool(bracket_id, tournamentPool))
            continue;

        m_ActiveTournamentPool = tournamentPool;
        UpdatePool(bgTypeId, bracket_id, arenaType, isRated, arenaRating);
    }

    m_ActiveTournamentPool = false;
}

bool BattlegroundQueue::HasWaitingGroupsInPool(BattlegroundBracketId bracket_id, bool tournamentPool) const
{
    for (uint32 queueIndex = BG_QUEUE_PREMADE_ALLIANCE; queueIndex < BG_QUEUE_GROUP_TYPES_COUNT; ++queueIndex)
        for (GroupQueueInfo const* ginfo : m_QueuedGroups[bracket_id][queueIndex])
            if (!ginfo->IsInvitedToBGInstanceGUID && ginfo->TournamentPool == tournamentPool)
                return true;

    return false;
}

// The matching pass for the pool in m_ActiveTournamentPool.
void BattlegroundQueue::UpdatePool(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id, uint8 arenaType, bool isRated, uint32 arenaRating)
{
    // Violet Hold never waits for an opponent. The enemy side is summoned by the
    // clone driver once the match is live, so there is no second team to match
    // against and no reason to hold anyone in the queue: every waiting group is
    // given its own instance immediately.
    //
    // Groups are deliberately not merged. A run's difficulty is pinned to the
    // party size that started it, so dropping a latecomer into someone else's
    // wave would change a curve that is already underway.
    if (bgTypeId == BATTLEGROUND_VHR)
    {
        Battleground* vhrTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!vhrTemplate)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::Update - Cannot find Violet Hold template: {}", bgTypeId);
            return;
        }

        PvPDifficultyEntry const* vhrBracket = GetBattlegroundBracketById(vhrTemplate->GetMapId(), bracket_id);
        if (!vhrBracket)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::Update - Cannot find Violet Hold bracket for map {} bracket {}", vhrTemplate->GetMapId(), bracket_id);
            return;
        }

        for (uint32 queueIndex = BG_QUEUE_PREMADE_ALLIANCE; queueIndex < BG_QUEUE_GROUP_TYPES_COUNT; ++queueIndex)
        {
            for (GroupQueueInfo* ginfo : m_QueuedGroups[bracket_id][queueIndex])
            {
                if (ginfo->IsInvitedToBGInstanceGUID || !InActivePool(ginfo))
                    continue;

                Battleground* vhr = sBattlegroundMgr->CreateNewBattleground(bgTypeId, vhrBracket, 0, false);
                if (!vhr)
                {
                    TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::Update - Cannot create Violet Hold battleground: {}", bgTypeId);
                    return;
                }

                vhr->SetTournamentPool(m_ActiveTournamentPool);

                // The party always holds one side outright, whatever their real
                // factions are, so the clones have the other to themselves.
                ginfo->Team = ALLIANCE;
                InviteGroupToBG(ginfo, vhr, ALLIANCE);
                vhr->StartBattleground();
            }
        }

        return;
    }

    if (sBattlegroundMgr->IsSquadBattleground(bgTypeId))
    {
        UpdateSquadPool(bgTypeId, bracket_id, arenaType);
        return;
    }

    if (bgTypeId == BATTLEGROUND_SSH)
    {
        UpdateSoloShufflePool(bgTypeId, bracket_id, arenaType);
        return;
    }

    // battleground with free slot for player should be always in the beggining of the queue
    // maybe it would be better to create bgfreeslotqueue for each bracket_id
    BGFreeSlotQueueContainer& bgQueues = sBattlegroundMgr->GetBGFreeSlotQueueStore(bgTypeId);
    for (BGFreeSlotQueueContainer::iterator itr = bgQueues.begin(); itr != bgQueues.end();)
    {
        Battleground* bg = *itr; ++itr;
        // A running match only ever takes groups of its own pool.
        if (!InActivePool(bg))
            continue;

        // A bot-filled skirmish - and a battleground seated like one, the Rift -
        // admits latecomers only into its clones' seats and only before its gates
        // open (OfferPreparationSeats); never as a free-slot backfill.
        if (!bg->TakesQueueBackfill())
            continue;

        // DO NOT allow queue manager to invite new player to rated games
        if (!bg->isRated() && bg->GetTypeID() == bgTypeId && bg->GetBracketId() == bracket_id &&
            (!bg->isArena() || bg->GetArenaType() == arenaType) &&
            bg->GetStatus() > STATUS_WAIT_QUEUE && bg->GetStatus() < STATUS_WAIT_LEAVE)
        {
            // clear selection pools
            m_SelectionPools[TEAM_ALLIANCE].Init();
            m_SelectionPools[TEAM_HORDE].Init();

            // call a function that does the job for us
            FillPlayersToBG(bg, bracket_id);

            // now everything is set, invite players
            for (GroupsQueueType::const_iterator citr = m_SelectionPools[TEAM_ALLIANCE].SelectedGroups.begin(); citr != m_SelectionPools[TEAM_ALLIANCE].SelectedGroups.end(); ++citr)
                InviteGroupToBG((*citr), bg, (*citr)->Team);

            for (GroupsQueueType::const_iterator citr = m_SelectionPools[TEAM_HORDE].SelectedGroups.begin(); citr != m_SelectionPools[TEAM_HORDE].SelectedGroups.end(); ++citr)
                InviteGroupToBG((*citr), bg, (*citr)->Team);

            if (!bg->HasFreeSlots())
                bg->RemoveFromBGFreeSlotQueue();
            else if (IsCustomBattleground(bgTypeId))
            {
                // Keep SCM refill progressing even when no new external queue events
                // occur. Without a follow-up update pulse, SCM can stall at an
                // intermediate invite wave despite queued managed bots waiting.
                sBattlegroundMgr->ScheduleQueueUpdate(0, arenaType, BattlegroundMgr::BGQueueTypeId(bgTypeId, arenaType), bgTypeId, bracket_id);
            }
        }
    }

    // SCM should always saturate existing free-slot instances before creating another one.
    // This prevents queue fragmentation where late joiners get split into a second SCM while
    // the first instance still has open seats.
    if (IsCustomBattleground(bgTypeId))
    {
        for (Battleground* bg : bgQueues)
        {
            if (!bg)
                continue;

            // Only a match this pool could join holds back a new one; a world
            // match with seats open must not stall the tournament queue.
            if (bg->GetTypeID() != bgTypeId || bg->GetBracketId() != bracket_id || !InActivePool(bg))
                continue;

            if (bg->GetStatus() <= STATUS_WAIT_QUEUE || bg->GetStatus() >= STATUS_WAIT_LEAVE)
                continue;

            // nor a match the queue may not seat anyone in (a Rift under way)
            if (!bg->TakesQueueBackfill())
                continue;

            if (bg->HasFreeSlots())
                return;
        }
    }

    // finished iterating through the bgs with free slots, maybe we need to create a new bg

    Battleground* bg_template = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bg_template)
    {
        TC_LOG_ERROR("bg.battleground", "Battleground: Update: bg template not found for {}", bgTypeId);
        return;
    }

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketById(bg_template->GetMapId(), bracket_id);
    if (!bracketEntry)
    {
        TC_LOG_ERROR("bg.battleground", "Battleground: Update: bg bracket entry not found for map {} bracket id {}", bg_template->GetMapId(), bracket_id);
        return;
    }

    // get the min. players per team, properly for larger arenas as well. (must have full teams for arena matches!)
    uint32 MinPlayersPerTeam = bg_template->GetMinPlayersPerTeam();
    uint32 MaxPlayersPerTeam = bg_template->GetMaxPlayersPerTeam();

    if (bg_template->isArena())
    {
        MaxPlayersPerTeam = arenaType;
        MinPlayersPerTeam = sBattlegroundMgr->isArenaTesting() ? 1 : arenaType;
    }
    else if (sBattlegroundMgr->isTesting())
        MinPlayersPerTeam = 1;

    m_SelectionPools[TEAM_ALLIANCE].Init();
    m_SelectionPools[TEAM_HORDE].Init();

    if (bg_template->isBattleground())
    {
        if (CheckPremadeMatch(bracket_id, MinPlayersPerTeam, MaxPlayersPerTeam))
        {
            // create new battleground
            Battleground* bg2 = sBattlegroundMgr->CreateNewBattleground(bgTypeId, bracketEntry, 0, false);
            if (!bg2)
            {
                TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::Update - Cannot create battleground: {}", bgTypeId);
                return;
            }
            bg2->SetTournamentPool(m_ActiveTournamentPool);
            // invite those selection pools
            for (uint32 i = 0; i < PVP_TEAMS_COUNT; i++)
                for (GroupsQueueType::const_iterator citr = m_SelectionPools[TEAM_ALLIANCE + i].SelectedGroups.begin(); citr != m_SelectionPools[TEAM_ALLIANCE + i].SelectedGroups.end(); ++citr)
                    InviteGroupToBG((*citr), bg2, (*citr)->Team);

            bg2->StartBattleground();
            //clear structures
            m_SelectionPools[TEAM_ALLIANCE].Init();
            m_SelectionPools[TEAM_HORDE].Init();
        }
    }

    // now check if there are in queues enough players to start new game of (normal battleground, or non-rated arena)
    if (!isRated)
    {
        // if there are enough players in pools, start new battleground or non rated arena
        if (CheckNormalMatch(bg_template, bracket_id, MinPlayersPerTeam, MaxPlayersPerTeam)
            || (bg_template->isArena() && CheckSkirmishForSameFaction(bracket_id, MinPlayersPerTeam)))
        {
            // we successfully created a pool
            bool const hasBotParticipants = SelectionPoolsHaveBotInvitees(m_SelectionPools);
            Battleground* bg2 = sBattlegroundMgr->CreateNewBattleground(bgTypeId, bracketEntry, arenaType, false,
                false, hasBotParticipants);
            if (!bg2)
            {
                TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::Update - Cannot create battleground: {}", bgTypeId);
                return;
            }
            bg2->SetTournamentPool(m_ActiveTournamentPool);

            // invite those selection pools
            for (uint32 i = 0; i < PVP_TEAMS_COUNT; i++)
                for (GroupsQueueType::const_iterator citr = m_SelectionPools[TEAM_ALLIANCE + i].SelectedGroups.begin(); citr != m_SelectionPools[TEAM_ALLIANCE + i].SelectedGroups.end(); ++citr)
                    InviteGroupToBG((*citr), bg2, (*citr)->Team);
            // start bg
            bg2->StartBattleground();
        }
        // Not enough people for a stock match. Where the playerbot module
        // pads battlegrounds with clones, the people waiting get one anyway
        // once they have waited long enough for company to show up.
        else if (bg_template->isBattleground() && sBattlegroundMgr->IsBotFillBattleground(bgTypeId))
        {
            // Seated like a skirmish: a clone's seat in a match still preparing
            // comes first, so the people waiting meet the ones already in it.
            if (bg_template->TakesSeatsInPreparationOnly())
                OfferPreparationSeats(bgTypeId, bracket_id, 0, sBattlegroundMgr->GetBotFillQueueWaitMs());
            TryStartBotFilledMatch(bgTypeId, bracketEntry, bracket_id, MaxPlayersPerTeam, 0,
                sBattlegroundMgr->GetBotFillQueueWaitMs());
        }
        else if (bg_template->isArena() && sBattlegroundMgr->IsBotFillSkirmishArena(arenaType))
        {
            // A clone's seat in a skirmish still preparing comes first, so the
            // people waiting meet the ones already behind the gates instead of
            // each being handed clones of their own. Whoever it cannot place
            // gets a new match.
            OfferPreparationSeats(bgTypeId, bracket_id, arenaType, sBattlegroundMgr->GetBotFillQueueWaitMs(arenaType));
            TryStartBotFilledMatch(bgTypeId, bracketEntry, bracket_id, MaxPlayersPerTeam, arenaType,
                sBattlegroundMgr->GetBotFillQueueWaitMs(arenaType));
        }
    }
    else if (bg_template->isArena())
    {
        UpdateRatedPool(bgTypeId, bracketEntry, bracket_id, arenaType, arenaRating);

        // Whoever the stock pairing left waiting is offered a match against
        // clones once they have waited long enough for a real team to show.
        if (sBattlegroundMgr->IsBotFillRatedArena(arenaType))
            StartBotFilledRatedMatches(bgTypeId, bracketEntry, bracket_id, arenaType);
    }
}

void BattlegroundQueue::UpdateRatedPool(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry,
    BattlegroundBracketId bracket_id, uint8 arenaType, uint32 arenaRating)
{
    // found out the minimum and maximum ratings the newly added team should battle against
    // arenaRating is the rating of the latest joined team, or 0
    // 0 is on (automatic update call) and we must set it to team's with longest wait time
    if (!arenaRating)
    {
        // The front of each list, counting only this pool's teams.
        auto const firstInPool = [this](GroupsQueueType const& groups) -> GroupQueueInfo*
        {
            for (GroupQueueInfo* ginfo : groups)
                if (InActivePool(ginfo))
                    return ginfo;
            return nullptr;
        };

        GroupQueueInfo* front1 = firstInPool(m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE]);
        GroupQueueInfo* front2 = firstInPool(m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE]);
        if (front1)
            arenaRating = front1->ArenaMatchmakerRating;
        if (front2)
            arenaRating = front2->ArenaMatchmakerRating;
        if (front1 && front2)
        {
            if (front1->JoinTime < front2->JoinTime)
                arenaRating = front1->ArenaMatchmakerRating;
        }
        else if (!front1 && !front2)
            return; //queues are empty
    }

    //set rating range
    uint32 arenaMinRating = (arenaRating <= sBattlegroundMgr->GetMaxRatingDifference()) ? 0 : arenaRating - sBattlegroundMgr->GetMaxRatingDifference();
    uint32 arenaMaxRating = arenaRating + sBattlegroundMgr->GetMaxRatingDifference();
    // if max rating difference is set and the time past since server startup is greater than the rating discard time
    // (after what time the ratings aren't taken into account when making teams) then
    // the discard time is current_time - time_to_discard, teams that joined after that, will have their ratings taken into account
    // else leave the discard time on 0, this way all ratings will be discarded
    // this has to be signed value - when the server starts, this value would be negative and thus overflow
    int32 discardTime = GameTime::GetGameTimeMS() - sBattlegroundMgr->GetRatingDiscardTimer();

    // timer for previous opponents
    int32 discardOpponentsTime = GameTime::GetGameTimeMS() - sWorld->getIntConfig(CONFIG_ARENA_PREV_OPPONENTS_DISCARD_TIMER);

    // we need to find 2 teams which will play next game
    GroupsQueueType::iterator itr_teams[PVP_TEAMS_COUNT];
    uint8 found = 0;
    uint8 team = 0;

    for (uint8 i = BG_QUEUE_PREMADE_ALLIANCE; i < BG_QUEUE_NORMAL_ALLIANCE; i++)
    {
        // take the group that joined first
        GroupsQueueType::iterator itr2 = m_QueuedGroups[bracket_id][i].begin();
        for (; itr2 != m_QueuedGroups[bracket_id][i].end(); ++itr2)
        {
            // if group match conditions, then add it to pool
            if (!(*itr2)->IsInvitedToBGInstanceGUID && InActivePool(*itr2)
                && (((*itr2)->ArenaMatchmakerRating >= arenaMinRating && (*itr2)->ArenaMatchmakerRating <= arenaMaxRating)
                    || (int32)(*itr2)->JoinTime < discardTime))
            {
                itr_teams[found++] = itr2;
                team = i;
                break;
            }
        }
    }

    if (!found)
        return;

    // The front of each list may not meet - one is short of the arena size and
    // bots may not make up the difference. The one that has waited longer looks
    // for an opponent in its own list instead.
    if (found == 2 && !RatedTeamsCanMeet(*itr_teams[0], *itr_teams[1], arenaType))
    {
        if ((*itr_teams[1])->JoinTime < (*itr_teams[0])->JoinTime)
        {
            itr_teams[0] = itr_teams[1];
            team = BG_QUEUE_PREMADE_HORDE;
        }
        else
            team = BG_QUEUE_PREMADE_ALLIANCE;
        found = 1;
    }

    if (found == 1)
    {
        for (GroupsQueueType::iterator itr3 = itr_teams[0]; itr3 != m_QueuedGroups[bracket_id][team].end(); ++itr3)
        {
            if (!(*itr3)->IsInvitedToBGInstanceGUID && InActivePool(*itr3)
                && (((*itr3)->ArenaMatchmakerRating >= arenaMinRating && (*itr3)->ArenaMatchmakerRating <= arenaMaxRating) || (int32)(*itr3)->JoinTime < discardTime)
                && ((*itr_teams[0])->ArenaTeamId != (*itr3)->PreviousOpponentsTeamId || ((int32)(*itr3)->JoinTime < discardOpponentsTime))
                && (*itr_teams[0])->ArenaTeamId != (*itr3)->ArenaTeamId
                && RatedTeamsCanMeet(*itr_teams[0], *itr3, arenaType))
            {
                itr_teams[found++] = itr3;
                break;
            }
        }
    }

    //if we have 2 teams, then start new arena and invite players!
    if (found == 2)
    {
        GroupQueueInfo* aTeam = *itr_teams[TEAM_ALLIANCE];
        GroupQueueInfo* hTeam = *itr_teams[TEAM_HORDE];
        // A team short of the arena size is made up with clones, as a skirmish
        // would be (RatedTeamsCanMeet allowed it). Both sides are still real
        // arena teams, so the match is rated exactly as stock.
        bool const shortHanded = aTeam->Players.size() < arenaType || hTeam->Players.size() < arenaType;
        bool const hasBotParticipants = shortHanded || GroupHasBotInvitee(aTeam) || GroupHasBotInvitee(hTeam);
        Battleground* arena = sBattlegroundMgr->CreateNewBattleground(bgTypeId, bracketEntry, arenaType, true,
            false, hasBotParticipants);
        if (!arena)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::Update couldn't create arena instance for rated arena match!");
            return;
        }
        arena->SetTournamentPool(m_ActiveTournamentPool);
        // before any invite, as TryStartBotFilledMatch does
        arena->SetBotFillMatch(shortHanded);

        aTeam->OpponentsTeamRating = hTeam->ArenaTeamRating;
        hTeam->OpponentsTeamRating = aTeam->ArenaTeamRating;
        aTeam->OpponentsMatchmakerRating = hTeam->ArenaMatchmakerRating;
        hTeam->OpponentsMatchmakerRating = aTeam->ArenaMatchmakerRating;
        TC_LOG_DEBUG("bg.battleground", "setting oposite teamrating for team {} to {}", aTeam->ArenaTeamId, aTeam->OpponentsTeamRating);
        TC_LOG_DEBUG("bg.battleground", "setting oposite teamrating for team {} to {}", hTeam->ArenaTeamId, hTeam->OpponentsTeamRating);

        // now we must move team if we changed its faction to another faction queue, because then we will spam log by errors in Queue::RemovePlayer
        if (aTeam->Team != ALLIANCE)
        {
            m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].push_front(aTeam);
            m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].erase(itr_teams[TEAM_ALLIANCE]);
        }
        if (hTeam->Team != HORDE)
        {
            m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_HORDE].push_front(hTeam);
            m_QueuedGroups[bracket_id][BG_QUEUE_PREMADE_ALLIANCE].erase(itr_teams[TEAM_HORDE]);
        }

        arena->SetArenaMatchmakerRating(ALLIANCE, aTeam->ArenaMatchmakerRating);
        arena->SetArenaMatchmakerRating(HORDE, hTeam->ArenaMatchmakerRating);
        InviteGroupToBG(aTeam, arena, ALLIANCE);
        InviteGroupToBG(hTeam, arena, HORDE);

        TC_LOG_DEBUG("bg.battleground", "Starting rated arena match!");
        arena->StartBattleground();
    }
}

// A rated team the stock pairing found no real opponent for plays clones once it
// has waited the rated bot-fill wait - the same match a skirmish would be, on
// the same bot-safe arenas, padded by the same driver. Where there is a bot
// ladder the clones' side is one of its teams (PickArenaLadderOpponent): the
// clones are copies of that team's members, and both teams are rated against
// each other. Without one the clones are no team, and the people's team is
// scored against an opponent of its own matchmaker rating
// (Arena::EndBotFilledRatedMatch).
//
// Collected before any team is invited, as TryStartBotFilledMatch does.
void BattlegroundQueue::StartBotFilledRatedMatches(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry,
    BattlegroundBracketId bracket_id, uint8 arenaType)
{
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32 const waitMs = sBattlegroundMgr->GetBotFillRatedArenaQueueWaitMs();

    std::vector<GroupQueueInfo*> ready;
    for (uint32 queueIndex : { BG_QUEUE_PREMADE_ALLIANCE, BG_QUEUE_PREMADE_HORDE })
    {
        for (GroupQueueInfo* ginfo : m_QueuedGroups[bracket_id][queueIndex])
        {
            if (ginfo->IsInvitedToBGInstanceGUID || !ginfo->IsRated || ginfo->ArenaType != arenaType || !InActivePool(ginfo))
                continue;

            if (getMSTimeDiff(ginfo->JoinTime, nowMs) < waitMs)
                continue;

            // A team of managed bots waits as stock, and so does one where
            // somebody unticked "Arena bots": they are waiting for people.
            if (!GroupHasRealPlayerInvitee(ginfo) || !GroupAllowsArenaBotFill(ginfo))
                continue;

            if (!sArenaTeamMgr->GetArenaTeamById(ginfo->ArenaTeamId))
                continue;

            ready.push_back(ginfo);
        }
    }

    for (GroupQueueInfo* ginfo : ready)
    {
        Battleground* arena = sBattlegroundMgr->CreateNewBattleground(bgTypeId, bracketEntry, arenaType, true, false, true);
        if (!arena)
        {
            TC_LOG_ERROR("bg.battleground", "BattlegroundQueue::StartBotFilledRatedMatches - Cannot create a rated arena for arena team {}",
                ginfo->ArenaTeamId);
            return;
        }

        arena->SetTournamentPool(m_ActiveTournamentPool);
        // before any invite, as TryStartBotFilledMatch does
        arena->SetBotFillMatch(true);

        // The team keeps the side whose list it waits in, so nothing has to be
        // moved between lists; the clones take the other side.
        uint32 const side = ginfo->Team == HORDE ? HORDE : ALLIANCE;
        uint32 const clones = side == ALLIANCE ? HORDE : ALLIANCE;

        ArenaTeam* ladder = PickArenaLadderOpponent(ginfo, arenaType);
        uint32 const cloneMatchmakerRating = ladder ? ladder->GetRosterAverageMMR() : ginfo->ArenaMatchmakerRating;

        // What a declined invite is charged against (RemovePlayer)
        ginfo->OpponentsTeamRating = ladder ? ladder->GetRating() : ginfo->ArenaTeamRating;
        ginfo->OpponentsMatchmakerRating = cloneMatchmakerRating;

        arena->SetBotFillSide(clones);
        if (ladder)
            arena->SetArenaTeamIdForTeam(clones, ladder->GetId());
        arena->SetArenaMatchmakerRating(side, ginfo->ArenaMatchmakerRating);
        arena->SetArenaMatchmakerRating(clones, cloneMatchmakerRating);
        InviteGroupToBG(ginfo, arena, side);

        TC_LOG_DEBUG("bg.battleground", "BattlegroundQueue::StartBotFilledRatedMatches: rated {}v{} instance {} for arena team {} ({} real, mmr {}) against {} (mmr {}).",
            uint32(arenaType), uint32(arenaType), arena->GetInstanceID(), ginfo->ArenaTeamId, uint32(ginfo->Players.size()),
            ginfo->ArenaMatchmakerRating, ladder ? ladder->GetName() : std::string("clones"), cloneMatchmakerRating);

        arena->StartBattleground();
        arena->RemoveFromBGFreeSlotQueue();
    }
}

/*********************************************************/
/***            BATTLEGROUND QUEUE EVENTS              ***/
/*********************************************************/

bool BGQueueInviteEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* player = ObjectAccessor::FindConnectedPlayer(m_PlayerGuid);
    // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
    if (!player)
        return true;

    Battleground* bg = sBattlegroundMgr->GetBattleground(m_BgInstanceGUID, m_BgTypeId);
    //if battleground ended and its instance deleted - do nothing
    if (!bg)
        return true;

    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
    uint32 queueSlot = player->GetBattlegroundQueueIndex(bgQueueTypeId);
    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES)         // player is in queue or in battleground
    {
        // check if player is invited to this bg
        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        if (bgQueue.IsPlayerInvited(m_PlayerGuid, m_BgInstanceGUID, m_RemoveTime))
        {
            WorldPacket data;
            //we must send remaining time in queue
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_JOIN, INVITE_ACCEPT_WAIT_TIME - INVITATION_REMIND_TIME, 0, m_ArenaType, 0);
            player->SendDirectMessage(&data);
        }
    }
    return true;                                            //event will be deleted
}

void BGQueueInviteEvent::Abort(uint64 /*e_time*/)
{
    //do nothing
}

/*
    this event has many possibilities when it is executed:
    1. player is in battleground (he clicked enter on invitation window)
    2. player left battleground queue and he isn't there any more
    3. player left battleground queue and he joined it again and IsInvitedToBGInstanceGUID = 0
    4. player left queue and he joined again and he has been invited to same battleground again -> we should not remove him from queue yet
    5. player is invited to bg and he didn't choose what to do and timer expired - only in this condition we should call queue::RemovePlayer
    we must remove player in the 5. case even if battleground object doesn't exist!
*/
bool BGQueueRemoveEvent::Execute(uint64 /*e_time*/, uint32 /*p_time*/)
{
    Player* player = ObjectAccessor::FindConnectedPlayer(m_PlayerGuid);
    if (!player)
        // player logged off (we should do nothing, he is correctly removed from queue in another procedure)
        return true;

    Battleground* bg = sBattlegroundMgr->GetBattleground(m_BgInstanceGUID, m_BgTypeId);
    //battleground can be deleted already when we are removing queue info
    //bg pointer can be NULL! so use it carefully!

    uint32 queueSlot = player->GetBattlegroundQueueIndex(m_BgQueueTypeId);
    if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES)         // player is in queue, or in Battleground
    {
        // check if player is in queue for this BG and if we are removing his invite event
        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(m_BgQueueTypeId);
        if (bgQueue.IsPlayerInvited(m_PlayerGuid, m_BgInstanceGUID, m_RemoveTime))
        {
            // track if player leaves the BG by not clicking enter button
            if (bg && bg->isBattleground() && sWorld->getBoolConfig(CONFIG_BATTLEGROUND_TRACK_DESERTERS) &&
                (bg->GetStatus() == STATUS_IN_PROGRESS || bg->GetStatus() == STATUS_WAIT_JOIN))
            {
                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_DESERTER_TRACK);
                stmt->setUInt32(0, player->GetGUID().GetCounter());
                stmt->setUInt8(1, BG_DESERTION_TYPE_NO_ENTER_BUTTON);
                CharacterDatabase.Execute(stmt);
            }

            TC_LOG_DEBUG("bg.battleground", "Battleground: removing player {} from bg queue for instance {} because of not pressing enter battle in time.", player->GetGUID().ToString(), m_BgInstanceGUID);

            player->RemoveBattlegroundQueueId(m_BgQueueTypeId);
            bgQueue.RemovePlayer(m_PlayerGuid, true);
            //update queues if battleground isn't ended
            if (bg && bg->isBattleground() && bg->GetStatus() != STATUS_WAIT_LEAVE)
                sBattlegroundMgr->ScheduleQueueUpdate(0, 0, m_BgQueueTypeId, m_BgTypeId, bg->GetBracketId());

            WorldPacket data;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_NONE, 0, 0, 0, 0);
            player->SendDirectMessage(&data);
        }
    }

    //event will be deleted
    return true;
}

void BGQueueRemoveEvent::Abort(uint64 /*e_time*/)
{
    //do nothing
}
