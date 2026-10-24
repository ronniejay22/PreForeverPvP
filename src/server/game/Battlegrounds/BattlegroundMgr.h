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

#ifndef __BATTLEGROUNDMGR_H
#define __BATTLEGROUNDMGR_H

#include "Common.h"
#include "DBCEnums.h"
#include "Battleground.h"
#include "BattlegroundQueue.h"
#include <deque>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

struct BattlemasterListEntry;

typedef std::map<uint32, Battleground*> BattlegroundContainer;
typedef std::set<uint32> BattlegroundClientIdsContainer;

typedef std::unordered_map<uint32, BattlegroundTypeId> BattleMastersMap;

enum BattlegroundMisc
{
    BATTLEGROUND_ARENA_POINT_DISTRIBUTION_DAY   = 86400,    // seconds in a day

    BATTLEGROUND_OBJECTIVE_UPDATE_INTERVAL      = 1000,
    // How often a squad match's enemy-frame feed is looked at (Arena::UpdateSquadFeed).
    BATTLEGROUND_SQUAD_FEED_INTERVAL            = 250,
    BATTLEGROUND_QUEUE_UPDATE_INTERVAL          = 15 * IN_MILLISECONDS
};

struct BattlegroundData
{
    BattlegroundContainer m_Battlegrounds;
    BattlegroundClientIdsContainer m_ClientBattlegroundIds[MAX_BATTLEGROUND_BRACKETS];
    BGFreeSlotQueueContainer BGFreeSlotQueue;
};

struct BattlegroundTemplate
{
    BattlegroundTypeId Id;
    uint16 MinPlayersPerTeam;
    uint16 MaxPlayersPerTeam;
    uint8 MinLevel;
    uint8 MaxLevel;
    Position StartLocation[PVP_TEAMS_COUNT];
    float MaxStartDistSq;
    uint8 Weight;
    uint32 ScriptId;
    BattlemasterListEntry const* BattlemasterEntry;
    BattlegroundBounds Bounds;
    std::vector<BattlegroundDropExit> DropExits;
    BattlegroundStartExit StartExits[PVP_TEAMS_COUNT];
    std::vector<BattlegroundSquadStart> SquadStarts;    // `battleground_squad_start`; empty = two teams

    bool IsArena() const;
};

class TC_GAME_API BattlegroundMgr
{
    private:
        BattlegroundMgr();
        ~BattlegroundMgr();

    public:
        static BattlegroundMgr* instance();

        void Update(uint32 diff);

        /* Packet Building */
        void BuildPlayerJoinedBattlegroundPacket(WorldPacket* data, Player* player);
        void BuildPlayerLeftBattlegroundPacket(WorldPacket* data, ObjectGuid guid);
        void BuildBattlegroundListPacket(WorldPacket* data, ObjectGuid guid, Player* player, BattlegroundTypeId bgTypeId, uint8 fromWhere);
        void BuildGroupJoinedBattlegroundPacket(WorldPacket* data, GroupJoinBattlegroundResult result);
        void BuildBattlegroundStatusPacket(WorldPacket* data, Battleground* bg, uint8 queueSlot, uint8 statusId, uint32 time1, uint32 time2, uint8 arenaType, uint32 arenaFaction);
        void SendAreaSpiritHealerQueryOpcode(Player* player, Battleground* bg, ObjectGuid guid);

        /* Battlegrounds */
        Battleground* GetBattlegroundThroughClientInstance(uint32 instanceId, BattlegroundTypeId bgTypeId);
        Battleground* GetBattleground(uint32 InstanceID, BattlegroundTypeId bgTypeId);
        Battleground* GetBattlegroundTemplate(BattlegroundTypeId bgTypeId);
        // Every battleground of one type, keyed by instance id. Entry 0 is the
        // template and is not a live match. Added for the Violet Hold clone
        // driver, which lives in the scripts library and has to find instances
        // waiting on a wave without holding a guid to any of them.
        BattlegroundContainer const* GetBattlegroundsByType(BattlegroundTypeId bgTypeId) const;
        Battleground* CreateNewBattleground(BattlegroundTypeId bgTypeId, PvPDifficultyEntry const* bracketEntry, uint8 arenaType,
            bool isRated, bool isPrivate = false, bool hasBotParticipants = false);

        void AddBattleground(Battleground* bg);
        void RemoveBattleground(BattlegroundTypeId bgTypeId, uint32 instanceId);
        void AddToBGFreeSlotQueue(BattlegroundTypeId bgTypeId, Battleground* bg);
        void RemoveFromBGFreeSlotQueue(BattlegroundTypeId bgTypeId, uint32 instanceId);
        BGFreeSlotQueueContainer& GetBGFreeSlotQueueStore(BattlegroundTypeId bgTypeId);

        void LoadBattlegroundTemplates();
        void LoadRandomBattlegroundPools();
        void DeleteAllBattlegrounds();

        // Members of a random selection pool, in table order, filtered to those
        // that actually have a battleground template. Empty when the pool is not
        // configured, which callers should treat as "fall back to your own
        // list" rather than "no battlegrounds exist".
        std::vector<BattlegroundTypeId> GetRandomPoolMembers(BattlegroundTypeId poolBgTypeId);

        void SendToBattleground(Player* player, uint32 InstanceID, BattlegroundTypeId bgTypeId);

        // A type whose matches are played by squads (`battleground_squad_start`).
        bool IsSquadBattleground(BattlegroundTypeId bgTypeId) const;

        /* Battleground queues */
        BattlegroundQueue& GetBattlegroundQueue(BattlegroundQueueTypeId bgQueueTypeId) { return m_BattlegroundQueues[bgQueueTypeId]; }
        void ScheduleQueueUpdate(uint32 arenaMatchmakerRating, uint8 arenaType, BattlegroundQueueTypeId bgQueueTypeId, BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id);
        uint32 GetPrematureFinishTime() const;

        void ToggleArenaTesting();
        void ToggleTesting();

        bool isArenaTesting() const { return m_ArenaTesting; }
        bool isTesting() const { return m_Testing; }

        // Bot-filled battlegrounds, unrated arena skirmishes and - where the
        // realm allows it - rated arenas. For queues this policy covers, a
        // match may start for real players alone once the longest-waiting one
        // has reached the applicable wait; the playerbot module then pads both
        // teams with transient clones. The module installs the policy on
        // config load, so a build or a realm without it leaves the queue
        // exactly as stock: nothing here reads the playerbot configuration
        // directly.
        //
        // A rated arena is only ever filled for an arena team the queue found
        // no real opponent for. The clones are no arena team: the match is
        // scored against an opponent of the team's own matchmaker rating
        // (Arena::EndBotFilledRatedMatch).
        struct BotFillPolicy
        {
            bool enabled = false;
            // Empty set = every battleground except Violet Hold.
            std::set<uint32> battlegroundTypes;
            uint32 queueWaitMs = 0;
            bool skirmishArenasEnabled = false;
            uint32 skirmishArenaQueueWaitMs = 0;
            bool ratedArenasEnabled = false;
            uint32 ratedArenaQueueWaitMs = 0;
            // A character queueing rated alone with no team for the size gets
            // a team of one made for it (ArenaTeamMgr::CreateSoloArenaTeam).
            bool ratedArenaAutoCreateTeam = false;
        };
        void SetBotFillPolicy(BotFillPolicy policy) { _botFillPolicy = std::move(policy); }
        bool IsBotFillBattleground(BattlegroundTypeId bgTypeId) const;
        bool IsBotFillSkirmishArena(uint8 arenaType) const;
        bool IsBotFillRatedArena(uint8 arenaType) const;
        // Without an arena size: whether this realm fills skirmishes at all,
        // which is what decides if the Battlegrounds tab offers the "Arena
        // bots" toggle. IsBotFillSkirmishArena answers for one size.
        bool IsBotFillSkirmishArenaEnabled() const { return _botFillPolicy.enabled && _botFillPolicy.skirmishArenasEnabled; }
        // The same for rated arenas; also what the arena window is told
        // (CCGAME ARENARATED) to decide whether its rated rows can be joined.
        bool IsBotFillRatedArenaEnabled() const { return _botFillPolicy.enabled && _botFillPolicy.ratedArenasEnabled; }
        bool AutoCreatesRatedArenaTeams() const { return IsBotFillRatedArenaEnabled() && _botFillPolicy.ratedArenaAutoCreateTeam; }
        uint32 GetBotFillQueueWaitMs(uint8 arenaType = 0) const
        {
            return arenaType ? _botFillPolicy.skirmishArenaQueueWaitMs : _botFillPolicy.queueWaitMs;
        }
        uint32 GetBotFillRatedArenaQueueWaitMs() const { return _botFillPolicy.ratedArenaQueueWaitMs; }

        // The bot ladder (Playerbot.BgFill.RatedArena.Ladder): arena teams of
        // bot characters, kept by the playerbot module. A rated team the queue
        // gives clones to plays one of these - its clones are copies of that
        // team's members, and both teams are rated - instead of anonymous
        // clones. Empty = no ladder: the clones are no team.
        void SetArenaLadderTeams(std::vector<uint32> teamIds) { _arenaLadderTeams = std::move(teamIds); }
        std::vector<uint32> const& GetArenaLadderTeams() const { return _arenaLadderTeams; }
        bool IsArenaLadderTeam(uint32 arenaTeamId) const;
        // Whether an arena team is seated in an arena that has not ended yet.
        bool IsArenaTeamInMatch(uint32 arenaTeamId) const;

        static BattlegroundQueueTypeId BGQueueTypeId(BattlegroundTypeId bgTypeId, uint8 arenaType);
        static BattlegroundTypeId BGTemplateId(BattlegroundQueueTypeId bgQueueTypeId);
        static uint8 BGArenaType(BattlegroundQueueTypeId bgQueueTypeId);

        uint32 GetMaxRatingDifference() const;
        uint32 GetRatingDiscardTimer()  const;
        void InitAutomaticArenaPointDistribution();
        void LoadBattleMastersEntry();
        void CheckBattleMasters();
        BattlegroundTypeId GetBattleMasterBG(uint32 entry) const
        {
            BattleMastersMap::const_iterator itr = mBattleMastersMap.find(entry);
            if (itr != mBattleMastersMap.end())
                return itr->second;
            return BATTLEGROUND_TYPE_NONE;
        }

        // Battleground and arena honor per hour, evened out between kinds of
        // match (Centurion.Battleground.HourlyHonor). A kind is the
        // battleground actually played - or for an arena the format queued for
        // (All Arenas, Buffle Shuffle) - by team size and level bracket. Every
        // match that pays out with a real person in it is recorded in
        // `centurion_pvp_match_duration`: its length and the median honor its
        // winners and its losers earned during play. The last Window of each
        // kind are kept here, so the medians move as the kind is played.
        struct MatchDurationKey
        {
            uint32 TypeId;
            uint8 ArenaType;
            uint8 MinLevel;

            bool operator<(MatchDurationKey const& other) const
            {
                return std::tie(TypeId, ArenaType, MinLevel) < std::tie(other.TypeId, other.ArenaType, other.MinLevel);
            }
        };
        struct MatchDurationStats
        {
            MatchDurationKey Key;
            uint8 MaxLevel;
            bool Arena;
            uint32 Samples;
            uint32 MedianSeconds;
            uint32 MedianWinHonor;       // earned during play, end-of-match payout excluded
            uint32 MedianLossHonor;
        };
        // What a kind's end-of-match payout should be so that a match of its
        // median length, with its median in-match honor, earns HonorPerHour.
        struct HourlyHonorPayout
        {
            uint32 Winner;
            uint32 Loser;
        };
        static MatchDurationKey GetMatchDurationKey(Battleground const* bg);
        void LoadMatchDurations();
        // winHonor/lossHonor: the median in-match honor of each side, or -1
        // when the side had nobody in it at the end.
        void RecordMatchDuration(Battleground const* bg, uint32 seconds, uint32 humans, uint32 players,
            int32 winHonor, int32 lossHonor, uint32 paidWinner, uint32 paidLoser);
        // False while the feature is off, the kind or the anchor is still short
        // of samples, or there is no rate to pay; the flat payout then stands.
        bool GetHourlyHonorPayout(Battleground const* bg, HourlyHonorPayout& payout) const;
        HourlyHonorPayout HourlyHonorPayoutFor(MatchDurationStats const& stats, uint32 honorPerHour) const;
        // What a match pays at the end without the hourly balance: the
        // configured winner/loser honor, for an arena plus three flag captures
        // and cut by ArenaRewardMultiplier (Battleground::EndBattleground).
        static HourlyHonorPayout GetFlatPayout(bool arena);
        // HonorPerHour, or when that is 0, what the anchor kind pays per hour
        // today (AnchorTypeId/AnchorArenaType, 3v3 All Arenas by default): its
        // flat payout plus its median in-match honor, over its median length,
        // all its level brackets pooled. 0 when there is not yet enough to say.
        uint32 GetHonorPerHour() const;
        // Whether the anchor is an arena format, which sets what ArenaEffort
        // is measured against.
        static bool IsHourlyHonorAnchorArena();
        std::vector<MatchDurationStats> GetMatchDurationStats() const;

    private:
        bool CreateBattleground(BattlegroundTemplate const* bgTemplate);
        bool IsPoolMemberSelectable(BattlegroundTypeId bgTypeId);
        uint32 CreateClientVisibleInstanceId(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id);
        static bool IsArenaType(BattlegroundTypeId bgTypeId);
        BattlegroundTypeId GetRandomBG(BattlegroundTypeId id, bool hasBotParticipants);

        typedef std::map<BattlegroundTypeId, BattlegroundData> BattlegroundDataContainer;
        BattlegroundDataContainer bgDataStore;

        BattlegroundQueue m_BattlegroundQueues[MAX_BATTLEGROUND_QUEUE_TYPES];

        std::vector<uint64> m_QueueUpdateScheduler;
        uint32 m_NextPeriodicQueueUpdateTime;
        uint32 m_NextRatedArenaUpdate;
        time_t m_NextAutoDistributionTime;
        uint32 m_AutoDistributionTimeChecker;
        uint32 m_UpdateTimer;
        uint32 m_SquadFeedTimer;
        bool   m_ArenaTesting;
        bool   m_Testing;
        BattleMastersMap mBattleMastersMap;

        BattlegroundTemplate const* GetBattlegroundTemplateByTypeId(BattlegroundTypeId id)
        {
            BattlegroundTemplateMap::const_iterator itr = _battlegroundTemplates.find(id);
            if (itr != _battlegroundTemplates.end())
                return &itr->second;
            return nullptr;
        }

        BattlegroundTemplate const* GetBattlegroundTemplateByMapId(uint32 mapId)
        {
            BattlegroundMapTemplateContainer::const_iterator itr = _battlegroundMapTemplates.find(mapId);
            if (itr != _battlegroundMapTemplates.end())
                return itr->second;
            return nullptr;
        }

        // Random selection pools, keyed by the type id players queue for
        // (BATTLEGROUND_AA for All Arenas, BATTLEGROUND_RB for Random
        // Battleground) and holding the candidates with their weights. This
        // exists because BattlemasterList.dbc can only name eight maps per row,
        // which capped All Arenas at eight arenas no matter how many were
        // installed. Populated from `battleground_random_pool`.
        typedef std::vector<std::pair<BattlegroundTypeId, double /*weight*/>> RandomPoolMembers;
        std::map<BattlegroundTypeId, RandomPoolMembers> _randomPools;

        typedef std::map<BattlegroundTypeId, uint8 /*weight*/> BattlegroundSelectionWeightMap;

        typedef std::map<BattlegroundTypeId, BattlegroundTemplate> BattlegroundTemplateMap;
        typedef std::map<uint32 /*mapId*/, BattlegroundTemplate*> BattlegroundMapTemplateContainer;
        BattlegroundTemplateMap _battlegroundTemplates;
        BattlegroundMapTemplateContainer _battlegroundMapTemplates;
        BotFillPolicy _botFillPolicy;
        std::vector<uint32> _arenaLadderTeams;              // see SetArenaLadderTeams()

        struct MatchDurationSample
        {
            time_t EndedAt;
            uint32 Seconds;
            int32 WinHonor;     // -1: no winners at the end
            int32 LossHonor;    // -1: no losers at the end
        };
        struct MatchDurationWindow
        {
            uint8 MaxLevel = 0;
            bool Arena = false;
            std::deque<MatchDurationSample> Samples;     // oldest first
        };
        void AddMatchDurationSample(MatchDurationKey const& key, uint8 maxLevel, bool arena, MatchDurationSample sample);
        // The window's samples still inside MaxAgeDays, newest first, at most Window.
        static void CollectRecentSamples(MatchDurationWindow const& window, time_t cutoff, std::vector<MatchDurationSample>& out);
        static MatchDurationStats MakeStats(MatchDurationKey const& key, MatchDurationWindow const& window,
            std::vector<MatchDurationSample> const& samples);
        static time_t GetMatchDurationCutoff();
        static uint32 Median(std::vector<uint32>& values);
        uint32 GetHonorPerHourLocked() const;
        std::map<MatchDurationKey, MatchDurationWindow> _matchDurations;
        bool _matchDurationTableExists = false;
        mutable std::mutex _matchDurationLock;
};

#define sBattlegroundMgr BattlegroundMgr::instance()

#endif // __BATTLEGROUNDMGR_H
