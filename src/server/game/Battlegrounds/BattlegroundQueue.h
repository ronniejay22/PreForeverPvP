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

#ifndef __BATTLEGROUNDQUEUE_H
#define __BATTLEGROUNDQUEUE_H

#include "Common.h"
#include "DBCEnums.h"
#include "Battleground.h"
#include "EventProcessor.h"

#include <deque>

//this container can't be deque, because deque doesn't like removing the last element - if you remove it, it invalidates next iterator and crash appears
typedef std::list<Battleground*> BGFreeSlotQueueContainer;

#define COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME 10

struct GroupQueueInfo;                                      // type predefinition
struct PlayerQueueInfo                                      // stores information for players in queue
{
    uint32 LastOnlineTime;                                  // for tracking and removing offline players from queue after 5 minutes
    GroupQueueInfo* GroupInfo;                              // pointer to the associated groupqueueinfo
};

struct GroupQueueInfo                                       // stores information about the group in queue (also used when joined as solo!)
{
    std::map<ObjectGuid, PlayerQueueInfo*> Players;         // player queue info map
    uint32  Team;                                           // Player team (ALLIANCE/HORDE)
    bool    IsForcedTeam;                                   // True when a queue entry must stay on Team instead of being rebalanced
    bool    TournamentPool;                                 // CENTURION: matched only with tournament-pool groups (TournamentMode.h)
    BattlegroundTypeId BgTypeId;                            // battleground type id
    bool    IsRated;                                        // rated
    uint8   ArenaType;                                      // 2v2, 3v3, 4v4, 5v5 or 0 when BG
    uint32  ArenaTeamId;                                    // team id if rated match
    uint32  JoinTime;                                       // time when group was added
    uint32  RemoveInviteTime;                               // time when we will remove invite for players in group
    uint32  IsInvitedToBGInstanceGUID;                      // was invited to certain BG
    uint32  ArenaTeamRating;                                // if rated match, inited to the rating of the team
    uint32  ArenaMatchmakerRating;                          // if rated match, inited to the rating of the team
    uint32  OpponentsTeamRating;                            // for rated arena matches
    uint32  OpponentsMatchmakerRating;                      // for rated arena matches
    uint32  PreviousOpponentsTeamId;                        // excluded from the current queue until the timer is met
    uint32  ShuffleParty;                                   // CENTURION: Buffle Shuffle party queued together (one entry each), 0 = alone
};

enum BattlegroundQueueGroupTypes
{
    BG_QUEUE_PREMADE_ALLIANCE = 0,
    BG_QUEUE_PREMADE_HORDE = 1,
    BG_QUEUE_NORMAL_ALLIANCE = 2,
    BG_QUEUE_NORMAL_HORDE = 3
};
#define BG_QUEUE_GROUP_TYPES_COUNT 6

enum BattlegroundQueueInvitationType
{
    BG_QUEUE_INVITATION_TYPE_NO_BALANCE = 0, // no balance: N+M vs N players
    BG_QUEUE_INVITATION_TYPE_BALANCED   = 1, // teams balanced: N+1 vs N players
    BG_QUEUE_INVITATION_TYPE_EVEN       = 2  // teams even: N vs N players
};

class Battleground;
class TC_GAME_API BattlegroundQueue
{
    public:
        BattlegroundQueue();
        ~BattlegroundQueue();

        void BattlegroundQueueUpdate(uint32 diff, BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id, uint8 arenaType = 0, bool isRated = false, uint32 minRating = 0);
        void UpdateEvents(uint32 diff);

        void FillPlayersToBG(Battleground* bg, BattlegroundBracketId bracket_id);
        bool CheckPremadeMatch(BattlegroundBracketId bracket_id, uint32 MinPlayersPerTeam, uint32 MaxPlayersPerTeam);
        bool CheckNormalMatch(Battleground* bg_template, BattlegroundBracketId bracket_id, uint32 minPlayers, uint32 maxPlayers);
        bool CheckSkirmishForSameFaction(BattlegroundBracketId bracket_id, uint32 minPlayersPerTeam);
        GroupQueueInfo* AddGroup(Player* leader, Group* group, BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry, uint8 ArenaType, bool isRated, bool isPremade, uint32 ArenaRating, uint32 MatchmakerRating, uint32 ArenaTeamId = 0, uint32 OpponentsArenaTeamId = 0);
        // Buffle Shuffle takes a party as one queue entry per member, each tagged
        // with the same GroupQueueInfo::ShuffleParty: a shuffle deals its sides
        // per person, so a party only has to arrive in the same match.
        uint32 NewShuffleParty() { return ++m_LastShuffleParty; }
        void RemovePlayer(ObjectGuid guid, bool decreaseInvitedCount);
        bool IsPlayerInvited(ObjectGuid pl_guid, const uint32 bgInstanceGuid, const uint32 removeTime);
        bool GetPlayerGroupInfoData(ObjectGuid guid, GroupQueueInfo* ginfo);
        void PlayerInvitedToBGUpdateAverageWaitTime(GroupQueueInfo* ginfo, BattlegroundBracketId bracket_id);
        uint32 GetAverageQueueWaitTime(GroupQueueInfo* ginfo, BattlegroundBracketId bracket_id) const;

        typedef std::map<ObjectGuid, PlayerQueueInfo> QueuedPlayersMap;
        QueuedPlayersMap m_QueuedPlayers;

        //do NOT use deque because deque.erase() invalidates ALL iterators
        typedef std::list<GroupQueueInfo*> GroupsQueueType;

        /*
        This two dimensional array is used to store All queued groups
        First dimension specifies the bgTypeId
        Second dimension specifies the player's group types -
             BG_QUEUE_PREMADE_ALLIANCE  is used for premade alliance groups and alliance rated arena teams
             BG_QUEUE_PREMADE_HORDE     is used for premade horde groups and horde rated arena teams
             BG_QUEUE_NORMAL_ALLIANCE   is used for normal (or small) alliance groups or non-rated arena matches
             BG_QUEUE_NORMAL_HORDE      is used for normal (or small) horde groups or non-rated arena matches
        */
        GroupsQueueType m_QueuedGroups[MAX_BATTLEGROUND_BRACKETS][BG_QUEUE_GROUP_TYPES_COUNT];

        // class to select and invite groups to bg
        class SelectionPool
        {
        public:
            SelectionPool(): PlayerCount(0) { }
            void Init();
            bool AddGroup(GroupQueueInfo* ginfo, uint32 desiredCount, uint8 tem);
            bool KickGroup(uint32 size);
            uint32 GetPlayerCount() const {return PlayerCount;}
        public:
            GroupsQueueType SelectedGroups;
        private:
            uint32 PlayerCount;
        };

        //one selection pool for horde, other one for alliance
        SelectionPool m_SelectionPools[PVP_TEAMS_COUNT];
        uint32 GetPlayersInQueue(TeamId id);
    private:

        bool InviteGroupToBG(GroupQueueInfo* ginfo, Battleground* bg, uint32 side);

        // CENTURION queue pools (Miscellaneous/TournamentMode.h). Tournament-pool
        // groups - tournament characters, and world characters who opted into the
        // tournament queue - are matched only with each other, and world groups
        // only with world groups. BattlegroundQueueUpdate runs the whole matching
        // pass once per pool with m_ActiveTournamentPool set, and every step that
        // picks queued groups or a running match skips the other pool.
        void UpdatePool(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id, uint8 arenaType, bool isRated, uint32 arenaRating);
        bool HasWaitingGroupsInPool(BattlegroundBracketId bracket_id, bool tournamentPool) const;
        bool InActivePool(GroupQueueInfo const* ginfo) const { return ginfo->TournamentPool == m_ActiveTournamentPool; }
        bool InActivePool(Battleground const* bg) const { return bg->IsTournamentPool() == m_ActiveTournamentPool; }
        bool m_ActiveTournamentPool = false;
        // Bot-filled battlegrounds and skirmishes
        // (BattlegroundMgr::BotFillPolicy): start a match for the real players
        // waiting in this bracket, however few, once the longest-waiting one
        // has been queued the policy's wait. The playerbot module fills the
        // rest of both teams afterwards. Rated groups are never selected.
        bool TryStartBotFilledMatch(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry,
            BattlegroundBracketId bracket_id, uint32 maxPlayersPerTeam, uint8 arenaType, uint32 queueWaitMs);
        // Real groups waiting the same wait are invited into bot-filled
        // skirmishes whose gates are still shut, each in the place of clones
        // (Battleground::CountPreparationSeatsHeldByClones).
        void OfferPreparationSeats(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id, uint8 arenaType,
            uint32 queueWaitMs);
        // The stock rated pairing: two arena teams within each other's rating
        // window. Where rated arenas are bot-filled a team short of the arena
        // size may be paired too, and the match is padded to size.
        void UpdateRatedPool(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry,
            BattlegroundBracketId bracket_id, uint8 arenaType, uint32 arenaRating);
        // What UpdateRatedPool could not pair, once it has waited the rated
        // bot-fill wait: each team gets a rated match of its own against
        // clones - copies of a bot ladder team's members, where there is a
        // ladder, else an opponent of the team's own rating.
        void StartBotFilledRatedMatches(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry,
            BattlegroundBracketId bracket_id, uint8 arenaType);
        // After ginfo->Team has been switched: moves the group to that side's
        // list, premade or normal as it was.
        void MoveGroupToSeatedSide(GroupQueueInfo* ginfo, BattlegroundBracketId bracket_id);
        // A squad match (Battleground::IsSquadMatch) cannot be made by pairing
        // two sides, so its queue builds the squads itself - see the comment
        // on the definition for when a match starts.
        void UpdateSquadPool(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id, uint8 arenaType);
        struct QueuedSquad
        {
            std::vector<GroupQueueInfo*> Groups;
            uint32 Size = 0;
        };
        void StartSquadMatch(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry, BattlegroundBracketId bracket_id,
            uint8 arenaType, std::vector<QueuedSquad> const& squads, bool botFill, bool rated = false);
        // Arena teams queued rated for a squad match: one team is one squad.
        void UpdateRatedSquadPool(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry, BattlegroundBracketId bracket_id,
            uint8 squadCount, uint8 arenaType);
        // Buffle Shuffle (Arena.h) is made from people queuing alone, four or six
        // to a match, by role - see the comment on the definition.
        void UpdateSoloShufflePool(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id, uint8 arenaType);
        void StartSoloShuffle(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry, BattlegroundBracketId bracket_id,
            uint8 arenaType, std::vector<GroupQueueInfo*> players, std::vector<bool> const& healers, bool botFill);
        uint32 m_LastShuffleParty = 0;
        uint32 m_WaitTimes[PVP_TEAMS_COUNT][MAX_BATTLEGROUND_BRACKETS][COUNT_OF_PLAYERS_TO_AVERAGE_WAIT_TIME];
        uint32 m_WaitTimeLastPlayer[PVP_TEAMS_COUNT][MAX_BATTLEGROUND_BRACKETS];
        uint32 m_SumOfWaitTimes[PVP_TEAMS_COUNT][MAX_BATTLEGROUND_BRACKETS];

        // Event handler
        EventProcessor m_events;
};

/*
    This class is used to invite player to BG again, when minute lasts from his first invitation
    it is capable to solve all possibilities
*/
class TC_GAME_API BGQueueInviteEvent : public BasicEvent
{
    public:
        BGQueueInviteEvent(ObjectGuid pl_guid, uint32 BgInstanceGUID, BattlegroundTypeId BgTypeId, uint8 arenaType, uint32 removeTime) :
          m_PlayerGuid(pl_guid), m_BgInstanceGUID(BgInstanceGUID), m_BgTypeId(BgTypeId), m_ArenaType(arenaType), m_RemoveTime(removeTime)
          { }
        virtual ~BGQueueInviteEvent() { }

        virtual bool Execute(uint64 e_time, uint32 p_time) override;
        virtual void Abort(uint64 e_time) override;
    private:
        ObjectGuid m_PlayerGuid;
        uint32 m_BgInstanceGUID;
        BattlegroundTypeId m_BgTypeId;
        uint8  m_ArenaType;
        uint32 m_RemoveTime;
};

/*
    This class is used to remove player from BG queue after 1 minute 20 seconds from first invitation
    We must store removeInvite time in case player left queue and joined and is invited again
    We must store bgQueueTypeId, because battleground can be deleted already, when player entered it
*/
class TC_GAME_API BGQueueRemoveEvent : public BasicEvent
{
    public:
        BGQueueRemoveEvent(ObjectGuid pl_guid, uint32 bgInstanceGUID, BattlegroundTypeId BgTypeId, BattlegroundQueueTypeId bgQueueTypeId, uint32 removeTime)
            : m_PlayerGuid(pl_guid), m_BgInstanceGUID(bgInstanceGUID), m_RemoveTime(removeTime), m_BgTypeId(BgTypeId), m_BgQueueTypeId(bgQueueTypeId)
        { }

        virtual ~BGQueueRemoveEvent() { }

        virtual bool Execute(uint64 e_time, uint32 p_time) override;
        virtual void Abort(uint64 e_time) override;
    private:
        ObjectGuid m_PlayerGuid;
        uint32 m_BgInstanceGUID;
        uint32 m_RemoveTime;
        BattlegroundTypeId m_BgTypeId;
        BattlegroundQueueTypeId m_BgQueueTypeId;
};

#endif
