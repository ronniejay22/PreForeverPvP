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

#include "PlayerbotBgFillDriver.h"

#include "Arena.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "CharacterCache.h"
#include "Configuration/Config.h"
#include "Containers.h"
#include "DataStores/DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotObcClone.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"
#include "PlayerbotPvpCore.h"
#include "PlayerbotRandomBotParticipation.h"
#include "PlayerbotResourceGovernor.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
constexpr uint32 kTickIntervalMs = 500;
constexpr uint32 kQueueWakeIntervalMs = 2 * IN_MILLISECONDS;
constexpr uint32 kOfflinePoolRefreshMs = 60 * IN_MILLISECONDS;
// An offline copy is a background character load; if it has not produced a
// clone in this long the request is presumed lost (the session carrying the
// callback logged out, most likely) and the seat is tried again. As long as a
// bot-filled arena's countdown: a busy database can take most of that, and a
// request given up on too soon is only made twice.
constexpr uint32 kOfflineCloneTimeoutMs = 30 * IN_MILLISECONDS;

// Difficulty tiers, weakest first; the names are the config key segments.
constexpr uint8 kTierCount = 3;
constexpr uint8 kNoTier = kTierCount;
constexpr char const* kTierNames[kTierCount] = { "Easy", "Medium", "Hard" };
// A typo such as "1-900000" must not build a table the size of the character list.
constexpr uint32 kMaxTierIdRange = 100000;

struct BgFillConfig
{
    bool enabled = false;
    std::set<uint32> battlegroundTypes;
    uint32 queueWaitMs = 15 * IN_MILLISECONDS;
    bool skirmishArenasEnabled = true;
    uint32 skirmishArenaQueueWaitMs = 0;
    bool ratedArenasEnabled = false;
    uint32 ratedArenaQueueWaitMs = 30 * IN_MILLISECONDS;
    bool ratedArenaAutoCreateTeam = true;
    // Playerbot.BgFill.RatedArena.Ladder.*: arena teams of the level-cap
    // PvP-only bots for rated teams to play (see SyncArenaLadder).
    bool ladderEnabled = false;
    std::array<uint32, ARENA_TYPE_5v5 + 1> ladderTeams = {};   // by arena type
    uint32 ladderSimulateMs = 5 * MINUTE * IN_MILLISECONDS;
    uint32 ladderSimulateGames = 1;                             // a size, each round
    uint32 maxPerTeam = 15;
    uint32 clonesPerTick = 2;
    bool useOfflineBots = true;
    bool allowHumanMirrors = true;
    // Playerbot.BgFill.DuplicateSources.*: a battleground whose template seats
    // at least this many a side may deal one character in more than once, and
    // fills up to duplicateMaxPerTeam (0 = the template's own size) instead of
    // maxPerTeam. 0 turns duplicates off.
    uint32 duplicateMinTeamSize = 40;
    uint32 duplicateMaxPerTeam = 0;
    // Playerbot.BgFill.Tier.*: the tier of each listed character (low guid) and
    // the tier each listed battleground type is filled for.
    std::unordered_map<uint32, uint8> tierByCharacter;
    std::unordered_map<uint32, uint8> tierByBattleground;
    // Playerbot.BgFill.ClassVariety.*: how much less likely a bot is to be dealt
    // for every copy of its class already on the team / anywhere in the match.
    // A weight, not a rule: 0 and 0 deal in plain tier order.
    float classVarietyTeamPenalty = 4.0f;
    float classVarietyMatchPenalty = 1.0f;
    // Playerbot.BgFill.SkirmishArena.HealersPerTeam / ...OfFive: healers each
    // side of an arena (each squad in a squad match) is dealt up to, counting
    // the people there - for teams of two to four, and of five. 0 = roles
    // play no part.
    uint32 healersPerTeam = 1;
    uint32 healersPerTeamOfFive = 2;
};

BgFillConfig g_Config;
uint32 g_TickAccumulatorMs = 0;
uint32 g_QueueWakeAccumulatorMs = 0;
uint32 g_LadderSimulateAccumulatorMs = 0;
// Arena teams are loaded long after the first config load; the ladder is only
// synced from OnStartup on.
bool g_WorldStarted = false;

// Offline characters on the managed-bot accounts, refreshed at most once a
// minute and only when a match actually runs out of online bots to copy.
struct OfflineCandidate
{
    uint32 lowGuid = 0;
    uint32 accountId = 0;
    uint8 level = 1;
    uint8 classId = 0;
};

struct OfflinePool
{
    bool loaded = false;
    uint32 loadedMs = 0;
    std::vector<OfflineCandidate> entries;
    std::unordered_map<uint32, uint8> classByGuid;
    std::unordered_set<uint32> healers;     // low guids whose active spec heals
};

OfflinePool g_OfflinePool;

// Copies requested from offline characters that have not materialised yet.
// They hold their seat so a slow load is not doubled up by the next tick.
struct PendingOfflineClone
{
    uint32 instanceId = 0;
    // Instance ids are handed out again; the serial says which match it was
    // (Battleground::GetMatchSerial).
    uint32 matchSerial = 0;
    uint32 team = 0;
    ObjectGuid sourceGuid;
    uint32 requestedMs = 0;
    // Copies of sourceGuid live in the instance once this one has arrived. A
    // character dealt in twice already has a copy there before its second
    // load finishes, so "any copy is live" would close the request early.
    uint32 expectedLive = 1;
};

std::vector<PendingOfflineClone> g_PendingOfflineClones;

// Instances announced in the log as being filled, so the notice goes out once
// per match rather than once per tick. Ids are recycled by later matches, so
// entries are dropped as soon as the id stops naming a live one.
std::unordered_set<uint32> g_AnnouncedInstances;

// Healers each side (each squad in a squad match) of an arena is dealt up to,
// settled once when its fill begins (SettleHealerTarget). Pruned with
// g_AnnouncedInstances.
std::unordered_map<uint32, uint32> g_HealerTargets;

struct TeamTally
{
    uint32 humans = 0;          // people, plus seats held by people who dropped and may return
    uint32 virtualBots = 0;     // persistent managed bots that queued in on their own
    uint32 clones = 0;          // transient copies, this driver's and anyone else's
    std::vector<ObjectGuid> humanGuids;
};

struct MatchTally
{
    TeamTally teams[PVP_TEAMS_COUNT];

    uint32 Humans() const { return teams[TEAM_ALLIANCE].humans + teams[TEAM_HORDE].humans; }
    uint32 Clones() const { return teams[TEAM_ALLIANCE].clones + teams[TEAM_HORDE].clones; }
};

MatchTally TallyMatch(Battleground const* bg)
{
    MatchTally tally;
    for (auto const& [guid, bgPlayer] : bg->GetPlayers())
    {
        TeamId const teamIndex = Battleground::GetTeamIndexByTeamId(bgPlayer.Team);
        if (teamIndex >= PVP_TEAMS_COUNT)
            continue;

        TeamTally& team = tally.teams[teamIndex];
        Player const* player = ObjectAccessor::FindConnectedPlayer(guid);
        // A participant the server no longer has connected is a person who
        // dropped: the battleground keeps the seat for a while in case they
        // come back, and so does this count.
        WorldSession const* session = player ? player->GetSession() : nullptr;
        if (session && session->IsTransientPlayerSession())
            ++team.clones;
        else if (session && session->IsVirtualSession())
            ++team.virtualBots;
        else
        {
            ++team.humans;
            if (player)
                team.humanGuids.push_back(guid);
        }
    }

    return tally;
}

bool IsFillCandidate(Battleground const* bg, uint32 instanceId)
{
    // A squad match the queue made of real squads only (BattlegroundQueue::
    // UpdateSquadPool) is closed to bots: its empty corners stay empty.
    if (!instanceId || !bg || bg->IsCustomGame() || bg->IsBotFillRefused())
        return false;

    // A rated arena is padded only where the queue started it as a bot match
    // (BattlegroundQueue::StartBotFilledRatedMatches, or two teams paired
    // short-handed): one of two whole teams is never touched.
    if (bg->isArena() && bg->isRated())
        return bg->IsBotFillMatch() && sBattlegroundMgr->IsBotFillRatedArena(bg->GetArenaType());

    if (bg->isRated())
        return false;

    if (bg->isArena())
        return sBattlegroundMgr->IsBotFillSkirmishArena(bg->GetArenaType());

    // A Random Battleground instance carries the queued-for type and the
    // rolled one; both have to be covered, or the roll could land on a
    // battleground the operator excluded.
    return sBattlegroundMgr->IsBotFillBattleground(bg->GetTypeID()) &&
        sBattlegroundMgr->IsBotFillBattleground(bg->GetTypeID(true));
}

// A battleground seating more a side than the bracket's pool has characters:
// at level 60 that pool is the 27 PvP-only characters, so Alterac Valley and
// Isle of Conquest dealt one copy per character stopped near 13 a side. Read
// from the template's size, never from which battleground it is; arenas seat
// five at most and keep one copy per character.
bool AllowsDuplicateSources(Battleground const* bg)
{
    return g_Config.duplicateMinTeamSize && !bg->isArena() &&
        bg->GetMaxPlayersPerTeam() >= g_Config.duplicateMinTeamSize;
}

uint32 FillCapForMatch(Battleground const* bg)
{
    uint32 const battlegroundMax = bg->GetMaxPlayersPerTeam();
    uint32 const configured = AllowsDuplicateSources(bg) ? g_Config.duplicateMaxPerTeam : g_Config.maxPerTeam;
    if (!configured)
        return battlegroundMax;

    return std::min(configured, battlegroundMax);
}

// Copies of one character in an instance, live and still loading.
uint32 CountCopiesOfSource(uint32 instanceId, ObjectGuid const& sourceGuid,
    std::vector<playerbot::PlayerbotObcCloneManager::CustomGameCloneInfo> const& clones)
{
    uint32 count = 0;
    for (auto const& clone : clones)
        if (clone.sourceGuid == sourceGuid)
            ++count;

    for (PendingOfflineClone const& pending : g_PendingOfflineClones)
        if (pending.instanceId == instanceId && pending.sourceGuid == sourceGuid)
            ++count;

    return count;
}

// Sources already represented in this match, so nobody is dealt twice into one
// game: every live clone's source plus every offline copy still on its way.
std::unordered_set<ObjectGuid> CollectUsedSources(uint32 instanceId,
    std::vector<playerbot::PlayerbotObcCloneManager::CustomGameCloneInfo> const& clones)
{
    std::unordered_set<ObjectGuid> used;
    for (auto const& clone : clones)
        used.insert(clone.sourceGuid);

    for (PendingOfflineClone const& pending : g_PendingOfflineClones)
        if (pending.instanceId == instanceId)
            used.insert(pending.sourceGuid);

    return used;
}

// The tier a battleground is filled for. The battleground actually played
// decides, so a Random Battleground that rolled a tiered one is dealt like it.
uint8 TierOfBattleground(Battleground const* bg)
{
    auto itr = g_Config.tierByBattleground.find(uint32(bg->GetTypeID(true)));
    if (itr == g_Config.tierByBattleground.end())
        itr = g_Config.tierByBattleground.find(uint32(bg->GetTypeID()));
    return itr != g_Config.tierByBattleground.end() ? itr->second : kNoTier;
}

uint8 TierOfSource(ObjectGuid const& guid)
{
    auto const itr = g_Config.tierByCharacter.find(guid.GetCounter());
    return itr != g_Config.tierByCharacter.end() ? itr->second : kNoTier;
}

// A tiered battleground is filled from its own tier first, then the tier
// beside it, then the far one, and bots in no tier last - on both teams, so
// the battleground plays at the difficulty it is meant to have whichever side
// you are on. A tier holds nine bots and Scarlet Chapel seats eleven of them
// around one player, so reserving the tier for the opposing team only left
// easy bots on the bench and put hard ones in the easy battleground. Sources
// arrive shuffled and the sort is stable, so bots of equal rank stay in random
// order.
uint8 TierRank(ObjectGuid const& guid, uint8 matchTier)
{
    if (matchTier == kNoTier)
        return 0;

    uint8 const tier = TierOfSource(guid);
    if (tier == kNoTier)
        return kTierCount;

    return tier > matchTier ? uint8(tier - matchTier) : uint8(matchTier - tier);
}

void OrderSourcesByTier(std::vector<ObjectGuid>& sources, uint8 matchTier)
{
    if (matchTier == kNoTier)
        return;

    std::stable_sort(sources.begin(), sources.end(), [matchTier](ObjectGuid const& left, ObjectGuid const& right)
    {
        return TierRank(left, matchTier) < TierRank(right, matchTier);
    });
}

// An online source answers for itself; an offline one from the pool's read.
uint8 ClassOfSource(ObjectGuid const& guid)
{
    if (Player const* player = ObjectAccessor::FindConnectedPlayer(guid))
        return player->GetClass();

    auto const itr = g_OfflinePool.classByGuid.find(guid.GetCounter());
    return itr != g_OfflinePool.classByGuid.end() ? itr->second : 0;
}

// Whether a source plays a healer, as Buffle Shuffle judges it. A clone copies
// its source's talents, so the copy answers the same.
bool IsHealerSource(ObjectGuid const& guid)
{
    if (Player const* player = ObjectAccessor::FindConnectedPlayer(guid))
        return Arena::IsSoloShuffleHealer(player);

    return g_OfflinePool.healers.count(guid.GetCounter()) != 0;
}

// Classes already seated, or on their way, on one team and in the whole match.
struct ClassMix
{
    std::array<uint32, MAX_CLASSES> onTeam = {};
    std::array<uint32, MAX_CLASSES> inMatch = {};

    void Add(uint8 classId, bool sameTeam)
    {
        if (!classId || classId >= MAX_CLASSES)
            return;

        ++inMatch[classId];
        if (sameTeam)
            ++onTeam[classId];
    }
};

ClassMix CountClasses(Battleground const* bg, uint32 team)
{
    ClassMix mix;
    for (auto const& [guid, bgPlayer] : bg->GetPlayers())
        if (Player const* player = ObjectAccessor::FindConnectedPlayer(guid))
            mix.Add(player->GetClass(), bgPlayer.Team == team);

    // A copy still loading holds a seat and will be that class in a moment;
    // without it a slow load lets the next tick deal the same class again.
    for (PendingOfflineClone const& pending : g_PendingOfflineClones)
        if (pending.instanceId == bg->GetInstanceID())
            mix.Add(ClassOfSource(pending.sourceGuid), pending.team == team);

    return mix;
}

// Scarlet Chapel seats its easy tier around one player, and three easy mages in
// a 3v3 or on five Scarlet Chapel seats is a bad draw however fair. So the next
// few seats are drawn one at a time from the best tier band still available,
// each bot weighted down for every copy of its class already on the team and,
// more gently, in the match. A nudge, not a rule: the tier order is never
// broken, and a repeat class can still come up. `mix` is advanced as if every
// draw succeeds; a source that then fails to load only skews the weights.
void OrderSourcesForVariety(std::vector<ObjectGuid>& sources, uint8 matchTier, ClassMix mix, uint32 draws)
{
    float const teamPenalty = g_Config.classVarietyTeamPenalty;
    float const matchPenalty = g_Config.classVarietyMatchPenalty;
    if (teamPenalty <= 0.0f && matchPenalty <= 0.0f)
        return;

    std::vector<double> weights;
    for (std::size_t position = 0; position < sources.size() && position < draws; ++position)
    {
        uint8 const bandRank = TierRank(sources[position], matchTier);
        std::size_t bandEnd = position;
        while (bandEnd < sources.size() && TierRank(sources[bandEnd], matchTier) == bandRank)
            ++bandEnd;

        weights.clear();
        for (std::size_t index = position; index < bandEnd; ++index)
        {
            uint8 const classId = ClassOfSource(sources[index]);
            bool const known = classId && classId < MAX_CLASSES;
            weights.push_back(1.0 / (1.0 +
                (known ? teamPenalty * mix.onTeam[classId] + matchPenalty * mix.inMatch[classId] : 0.0)));
        }

        std::size_t const chosen = position + urandweighted(weights.size(), weights.data());
        std::rotate(sources.begin() + position, sources.begin() + chosen, sources.begin() + chosen + 1);
        mix.Add(ClassOfSource(sources[position]), true);
    }
}

// Online managed bots whose level sits inside the bracket, shuffled. A bot
// already inside a battleground is mid-match somewhere; cloning it works, but
// its double walking around while the original fights elsewhere reads as a
// bug to anyone who knows it.
std::vector<ObjectGuid> CollectOnlineSources(uint32 minLevel, uint32 maxLevel, std::unordered_set<ObjectGuid> const& excluded)
{
    std::vector<ObjectGuid> sources;
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, candidate] : ObjectAccessor::GetPlayers())
        {
            if (!candidate || !candidate->IsInWorld())
                continue;

            if (!playerbot::IsManagedRandomBot(candidate))
                continue;

            if (candidate->InBattleground())
                continue;

            uint32 const level = candidate->GetLevel();
            if (level < minLevel || level > maxLevel)
                continue;

            // Dedicated PvP-only accounts are the level-cap pool, and the only
            // one: pre-60 brackets draw ordinary random-bot characters, a
            // level-60 bracket draws nothing else. PvP-only characters are never
            // logged in as persistent bots, so at 60 this pass finds nobody and
            // the offline pass clones them instead.
            if ((maxLevel < 60) == playerbot::PveManager::IsPvpOnlyBot(candidate))
                continue;

            if (excluded.count(guid))
                continue;

            sources.push_back(guid);
        }
    }

    Trinity::Containers::RandomShuffle(sources);
    return sources;
}

void RefreshOfflinePoolIfStale(uint32 nowMs)
{
    if (g_OfflinePool.loaded && nowMs - g_OfflinePool.loadedMs < kOfflinePoolRefreshMs)
        return;

    g_OfflinePool.loaded = true;
    g_OfflinePool.loadedMs = nowMs;
    g_OfflinePool.entries.clear();
    g_OfflinePool.classByGuid.clear();
    g_OfflinePool.healers.clear();

    // The random population's accounts plus the PvP-only pool. The PvP-only
    // accounts are deliberately left out of Playerbot.RandomPopulation.BotAccountIds
    // so nothing ever logs those characters in; they exist here only as sources
    // for transient copies.
    std::vector<uint32> accounts = playerbot::RandomBotParticipationManager::GetConfiguredBotAccountIds();
    auto const& pvpOnlyAccounts = playerbot::PveManager::GetConfig().pvpOnlyAccountIds;
    accounts.insert(accounts.end(), pvpOnlyAccounts.begin(), pvpOnlyAccounts.end());
    std::sort(accounts.begin(), accounts.end());
    accounts.erase(std::unique(accounts.begin(), accounts.end()), accounts.end());
    if (accounts.empty())
        return;

    std::string accountList;
    accountList.reserve(accounts.size() * 6);
    for (uint32 accountId : accounts)
    {
        if (!accountList.empty())
            accountList += ',';
        accountList += std::to_string(accountId);
    }

    // Same shape as the population manager's own pool query. The Obcc names
    // are the legacy on-disk Obsidian Colosseum clones: not real characters.
    QueryResult result = CharacterDatabase.PQuery(
        "SELECT guid, account, level, class FROM characters WHERE online = 0 AND account IN ({}) AND name NOT LIKE 'Obcc%'", accountList);
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        OfflineCandidate candidate;
        candidate.lowGuid = fields[0].GetUInt32();
        candidate.accountId = fields[1].GetUInt32();
        candidate.level = fields[2].GetUInt8();
        candidate.classId = fields[3].GetUInt8();
        g_OfflinePool.entries.push_back(candidate);
        g_OfflinePool.classByGuid[candidate.lowGuid] = candidate.classId;
    } while (result->NextRow());

    // Roles, counted the way Arena::IsSoloShuffleHealer counts a loaded
    // character: the top rank held of every talent in the active spec, summed
    // per tree. The level-60 pool is never online, so this is the only way to
    // tell its healers before one is dealt.
    if (QueryResult talents = CharacterDatabase.PQuery(
        "SELECT ct.guid, ct.spell FROM character_talent ct JOIN characters c ON c.guid = ct.guid AND ct.talentGroup = c.activeTalentGroup "
        "WHERE c.online = 0 AND c.account IN ({}) AND c.name NOT LIKE 'Obcc%'", accountList))
    {
        std::unordered_map<uint32, std::map<uint32, uint32>> ranksByTalent;
        do
        {
            Field* fields = talents->Fetch();
            TalentSpellPos const* pos = GetTalentSpellPos(fields[1].GetUInt32());
            if (!pos)
                continue;

            uint32& ranks = ranksByTalent[fields[0].GetUInt32()][pos->talent_id];
            ranks = std::max<uint32>(ranks, uint32(pos->rank) + 1);
        } while (talents->NextRow());

        for (auto const& [lowGuid, talentRanks] : ranksByTalent)
        {
            std::map<uint32, uint32> pointsByTab;
            for (auto const& [talentId, ranks] : talentRanks)
                if (TalentEntry const* talent = sTalentStore.LookupEntry(talentId))
                    pointsByTab[talent->TabID] += ranks;

            if (Arena::IsHealerTalentSpend(pointsByTab))
                g_OfflinePool.healers.insert(lowGuid);
        }
    }

    TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: offline bot pool refreshed, {} characters, {} of them healers.",
        uint32(g_OfflinePool.entries.size()), uint32(g_OfflinePool.healers.size()));
}

std::vector<ObjectGuid> CollectOfflineSources(uint32 minLevel, uint32 maxLevel, std::unordered_set<ObjectGuid> const& excluded)
{
    std::vector<ObjectGuid> sources;
    for (OfflineCandidate const& candidate : g_OfflinePool.entries)
    {
        if (candidate.level < minLevel || candidate.level > maxLevel)
            continue;

        // Same policy as the online pass: ordinary random bots below 60, the
        // PvP-only pool and nothing else at 60.
        auto const& pvpOnlyAccounts = playerbot::PveManager::GetConfig().pvpOnlyAccountIds;
        if ((maxLevel < 60) == std::binary_search(pvpOnlyAccounts.begin(), pvpOnlyAccounts.end(), candidate.accountId))
            continue;

        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(candidate.lowGuid);
        if (excluded.count(guid))
            continue;

        // Came online since the pool was read: the online pass owns it now,
        // and it may well be in a match already.
        if (ObjectAccessor::FindConnectedPlayer(guid))
            continue;

        sources.push_back(guid);
    }

    Trinity::Containers::RandomShuffle(sources);
    return sources;
}

// A real player's session in the match to hang an offline character load on;
// the callback runs when that session next updates. Anyone will do, a
// teammate of the seat being filled by preference so the request outlives the
// other side emptying out.
WorldSession* FindCallbackSession(MatchTally const& tally, TeamId preferredTeam)
{
    for (int pass = 0; pass < 2; ++pass)
    {
        TeamId const teamIndex = pass == 0 ? preferredTeam : (preferredTeam == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE);
        for (ObjectGuid const& guid : tally.teams[teamIndex].humanGuids)
            if (Player* human = ObjectAccessor::FindConnectedPlayer(guid))
                if (WorldSession* session = human->GetSession())
                    return session;
    }

    return nullptr;
}

// Drop pending offline requests that have produced their clone, timed out, or
// belong to a match that no longer exists - including one whose instance id a
// newer match has been given since.
void ReconcilePendingOfflineClones(uint32 nowMs)
{
    for (auto itr = g_PendingOfflineClones.begin(); itr != g_PendingOfflineClones.end();)
    {
        bool done = nowMs - itr->requestedMs >= kOfflineCloneTimeoutMs;
        if (!done)
        {
            Battleground const* bg = sBattlegroundMgr->GetBattleground(itr->instanceId, BATTLEGROUND_TYPE_NONE);
            if (!bg || bg->GetMatchSerial() != itr->matchSerial)
                done = true;
            else
            {
                uint32 live = 0;
                for (auto const& clone : playerbot::PlayerbotObcCloneManager::GetCustomGameClones(itr->instanceId))
                    if (clone.sourceGuid == itr->sourceGuid)
                        ++live;
                done = live >= itr->expectedLive;
            }
        }

        if (done)
            itr = g_PendingOfflineClones.erase(itr);
        else
            ++itr;
    }
}

uint32 CountPendingOfflineClones(uint32 instanceId, uint32 team)
{
    uint32 count = 0;
    for (PendingOfflineClone const& pending : g_PendingOfflineClones)
        if (pending.instanceId == instanceId && pending.team == team)
            ++count;
    return count;
}

// Remove `count` of this driver's clones from one team, dead ones first.
void ShedClonesFromTeam(Battleground* bg, uint32 team, uint32 count,
    std::vector<playerbot::PlayerbotObcCloneManager::CustomGameCloneInfo> const& clones)
{
    std::vector<ObjectGuid> victims;
    for (auto const& clone : clones)
    {
        if (clone.team != team)
            continue;

        Player const* player = ObjectAccessor::FindConnectedPlayer(clone.cloneGuid);
        if (player && !player->IsAlive())
            victims.insert(victims.begin(), clone.cloneGuid);
        else
            victims.push_back(clone.cloneGuid);
    }

    if (victims.size() > count)
        victims.resize(count);

    for (ObjectGuid const& victim : victims)
    {
        if (playerbot::PlayerbotObcCloneManager::DestroyCustomGameClone(victim))
            TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: removed surplus clone {} from team {} of instance {}.",
                victim.ToString(), team, bg->GetInstanceID());
    }
}

// Healers on one team: everyone seated (people, bots, clones) and every copy
// still loading for it.
uint32 CountHealers(Battleground const* bg, uint32 team)
{
    uint32 count = 0;
    for (auto const& [guid, bgPlayer] : bg->GetPlayers())
        if (bgPlayer.Team == team && Arena::IsSoloShuffleHealer(ObjectAccessor::FindConnectedPlayer(guid)))
            ++count;

    for (PendingOfflineClone const& pending : g_PendingOfflineClones)
        if (pending.instanceId == bg->GetInstanceID() && pending.team == team && IsHealerSource(pending.sourceGuid))
            ++count;

    return count;
}

// The configured healers for one team of an arena - one squad in a squad
// match, whose side holds several - or 0 where roles play no part.
uint32 ConfiguredHealersPerTeam(Battleground const* bg)
{
    if (!bg->isArena())
        return 0;

    uint32 const size = bg->IsSquadMatch() ? bg->GetSquadSize() : bg->GetArenaType();
    return size >= 5 ? g_Config.healersPerTeamOfFive : g_Config.healersPerTeam;
}

uint32 SquadsPerSide(Battleground const* bg)
{
    return bg->IsSquadMatch() ? std::max<uint32>(bg->GetSquadCount() / PVP_TEAMS_COUNT, 1) : 1;
}

// How many healers each team of an arena gets: the configured number or,
// where the bracket has too few healer bots left to give every team that many,
// the most they all can have. Nobody seated is taken away, so a side already
// holding more lifts the number for the other. Settled once, when the fill
// begins, so a scarce bracket is not decided one seat at a time.
uint32 SettleHealerTarget(Battleground* bg, MatchTally const& tally, std::unordered_set<ObjectGuid> const& usedSources, uint32 nowMs)
{
    uint32 const configured = ConfiguredHealersPerTeam(bg);
    uint32 const squads = SquadsPerSide(bg);
    uint32 const cap = FillCapForMatch(bg);

    uint32 have[PVP_TEAMS_COUNT] = {};
    uint32 open[PVP_TEAMS_COUNT] = {};
    for (uint32 teamIndex = TEAM_ALLIANCE; teamIndex < PVP_TEAMS_COUNT; ++teamIndex)
    {
        uint32 const team = teamIndex == TEAM_ALLIANCE ? ALLIANCE : HORDE;
        TeamTally const& side = tally.teams[teamIndex];
        uint32 const taken = side.humans + side.virtualBots + bg->GetInvitedCount(team);
        uint32 const desired = cap > taken ? cap - taken : 0;
        uint32 const present = side.clones + CountPendingOfflineClones(bg->GetInstanceID(), team);
        have[teamIndex] = CountHealers(bg, team);
        open[teamIndex] = desired > present ? desired - present : 0;
    }

    uint32 const minLevel = bg->GetMinLevel();
    uint32 const maxLevel = std::max(bg->GetMaxLevel(), minLevel);
    std::vector<ObjectGuid> sources = CollectOnlineSources(minLevel, maxLevel, usedSources);
    if (g_Config.useOfflineBots)
    {
        RefreshOfflinePoolIfStale(nowMs);
        std::vector<ObjectGuid> const offline = CollectOfflineSources(minLevel, maxLevel, usedSources);
        sources.insert(sources.end(), offline.begin(), offline.end());
    }
    uint32 const available = uint32(std::count_if(sources.begin(), sources.end(), IsHealerSource));

    auto const reachable = [&](uint32 perSide)
    {
        uint32 needed = 0;
        for (uint32 teamIndex = TEAM_ALLIANCE; teamIndex < PVP_TEAMS_COUNT; ++teamIndex)
        {
            uint32 const missing = perSide > have[teamIndex] ? perSide - have[teamIndex] : 0;
            if (missing > open[teamIndex])
                return false;
            needed += missing;
        }
        return needed <= available;
    };

    uint32 const mostHeld = std::max(have[TEAM_ALLIANCE], have[TEAM_HORDE]);
    uint32 const lowest = (mostHeld + squads - 1) / squads;
    uint32 target = std::max(configured, lowest);
    while (target > lowest && !reachable(target * squads))
        --target;

    if (bg->IsSquadMatch())
        bg->SetSquadHealersWanted(uint8(std::min<uint32>(target, 255)));

    TC_LOG_INFO("playerbots.bgfill", "PlayerbotBgFillDriver: {} instance {} deals {} healer(s) a {} ({} configured); healers seated: alliance {}, horde {}; {} healer bot(s) free in levels {}-{}.",
        bg->GetName(), bg->GetInstanceID(), target, bg->IsSquadMatch() ? "squad" : "side", configured,
        have[TEAM_ALLIANCE], have[TEAM_HORDE], available, minLevel, maxLevel);
    if (!reachable(target * squads))
        TC_LOG_WARN("playerbots.bgfill", "PlayerbotBgFillDriver: instance {} cannot give both sides {} healer(s); they will not match.",
            bg->GetInstanceID(), target);
    else if (target < configured)
        TC_LOG_WARN("playerbots.bgfill", "PlayerbotBgFillDriver: instance {} has too few healer bots for {} a {}; dealing {}.",
            bg->GetInstanceID(), configured, bg->IsSquadMatch() ? "squad" : "side", target);

    return target;
}

// A team's sources by role: the healers and everyone else, each in the usual
// tier and variety order. Dealt without roles, everyone is in `others`.
struct SourceDeck
{
    std::vector<ObjectGuid> healers;
    std::vector<ObjectGuid> others;
};

SourceDeck MakeDeck(std::vector<ObjectGuid> sources, uint8 matchTier, ClassMix const& mix, uint32 draws, bool byRole)
{
    SourceDeck deck;
    if (byRole)
    {
        auto const split = std::stable_partition(sources.begin(), sources.end(), IsHealerSource);
        deck.healers.assign(sources.begin(), split);
        sources.erase(sources.begin(), split);
        OrderSourcesByTier(deck.healers, matchTier);
        OrderSourcesForVariety(deck.healers, matchTier, mix, draws);
    }

    deck.others = std::move(sources);
    OrderSourcesByTier(deck.others, matchTier);
    OrderSourcesForVariety(deck.others, matchTier, mix, draws);
    return deck;
}

// The next source for a seat: one of the role wanted or, with `anyRole`, of
// the other once that has run out. Nothing when the deck is spent.
Optional<ObjectGuid> DrawSource(SourceDeck& deck, bool healer, bool anyRole)
{
    std::vector<ObjectGuid>* const wanted = healer ? &deck.healers : &deck.others;
    std::vector<ObjectGuid>* const other = healer ? &deck.others : &deck.healers;
    for (std::vector<ObjectGuid>* pile : { wanted, anyRole ? other : nullptr })
    {
        if (!pile || pile->empty())
            continue;

        ObjectGuid const guid = pile->front();
        pile->erase(pile->begin());
        return guid;
    }

    return {};
}

// Seat up to `wanted` clones on one team. Returns how many were created or
// requested. `matchClones` and `totalClones` feed the governor and are
// advanced as seats are taken. With `healersNeeded` (an arena), a seat goes
// to a healer while the team still needs one and to anyone else after that;
// healers come first, so a fill spread over several ticks lands on the count.
std::vector<ObjectGuid> LadderRosterFor(Battleground const* bg, uint32 team);

uint32 AddClonesToTeam(Battleground* bg, uint32 team, uint32 wanted, MatchTally const& tally,
    std::unordered_set<ObjectGuid>& usedSources, uint32& matchClones, uint32& totalClones, uint32 nowMs,
    Optional<uint32> healersNeeded = {})
{
    if (!wanted)
        return 0;

    uint32 const minLevel = bg->GetMinLevel();
    uint32 const maxLevel = std::max(bg->GetMaxLevel(), minLevel);
    TeamId const teamIndex = Battleground::GetTeamIndexByTeamId(team);
    TeamId const otherIndex = teamIndex == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    uint8 const matchTier = TierOfBattleground(bg);

    uint32 added = 0;
    // The governor wants this match's bots and everyone else's; totalClones
    // already holds this match's, which counted them twice and stopped a
    // full Alterac Valley at half the server-wide limit.
    auto governorAllows = [&]()
    {
        return playerbot::ResourceGovernor::CanAddCustomMatchBot(matchClones, totalClones - std::min(totalClones, matchClones));
    };

    // Draw a few more than wanted: a source that fails to clone is skipped, and
    // the one after it should still be a varied pick.
    uint32 const draws = wanted * 2 + 2;
    ClassMix mix = CountClasses(bg, team);

    auto const wantsHealer = [&]() { return healersNeeded && *healersNeeded > 0; };
    uint32 offRole = 0;
    auto const seated = [&](ObjectGuid const& sourceGuid, uint8 classId)
    {
        if (healersNeeded)
        {
            bool const healer = IsHealerSource(sourceGuid);
            if (healer != wantsHealer())
                ++offRole;
            if (healer && wantsHealer())
                --*healersNeeded;
        }

        usedSources.insert(sourceGuid);
        mix.Add(classId, true);
        ++added;
        ++matchClones;
        ++totalClones;
    };

    // 0. The side of a rated bot match held by a team of the bot ladder is that
    // team: its own members, every one of them. A member that cannot be dealt
    // in leaves its seat to the pools below.
    std::vector<ObjectGuid> const roster = LadderRosterFor(bg, team);
    if (!roster.empty())
    {
        RefreshOfflinePoolIfStale(nowMs);
        WorldSession* rosterCallback = nullptr;
        for (ObjectGuid const& sourceGuid : roster)
        {
            if (added >= wanted || !governorAllows())
                break;

            if (usedSources.count(sourceGuid))
                continue;

            if (Player* source = ObjectAccessor::FindConnectedPlayer(sourceGuid))
            {
                if (playerbot::PlayerbotObcCloneManager::CreateCustomGameClone(source, bg, team, ""))
                    seated(sourceGuid, source->GetClass());
                continue;
            }

            if (!rosterCallback && !(rosterCallback = FindCallbackSession(tally, teamIndex)))
                break;

            if (!playerbot::PlayerbotObcCloneManager::QueueCustomGameClone(sourceGuid, rosterCallback, bg, team, ""))
                continue;

            PendingOfflineClone pending;
            pending.instanceId = bg->GetInstanceID();
            pending.matchSerial = bg->GetMatchSerial();
            pending.team = team;
            pending.sourceGuid = sourceGuid;
            pending.requestedMs = nowMs;
            g_PendingOfflineClones.push_back(pending);

            seated(sourceGuid, ClassOfSource(sourceGuid));
            TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: requested a copy of ladder team member {} for team {} of instance {}.",
                sourceGuid.ToString(), team, bg->GetInstanceID());
        }
    }

    // The pools below never deal in a member of the ladder team on the other
    // side: that team's seats are its own.
    auto const excludedFromPools = [&]()
    {
        std::unordered_set<ObjectGuid> excluded = usedSources;
        for (ObjectGuid const& guid : LadderRosterFor(bg, team == ALLIANCE ? uint32(HORDE) : uint32(ALLIANCE)))
            excluded.insert(guid);
        return excluded;
    };

    // 1. Online bots in the bracket.
    SourceDeck online = MakeDeck(CollectOnlineSources(minLevel, maxLevel, excludedFromPools()), matchTier, mix, draws, healersNeeded.has_value());
    auto const seatOnline = [&](bool anyRole)
    {
        while (added < wanted && governorAllows())
        {
            Optional<ObjectGuid> const sourceGuid = DrawSource(online, wantsHealer(), anyRole);
            if (!sourceGuid)
                return;

            Player* source = ObjectAccessor::FindConnectedPlayer(*sourceGuid);
            if (!source)
                continue;

            // Managed bots keep their own names; "Dark" is reserved for a copy of
            // a real person, as in the Violet Hold waves.
            Player* clone = playerbot::PlayerbotObcCloneManager::CreateCustomGameClone(source, bg, team, "");
            if (!clone)
            {
                TC_LOG_WARN("playerbots.bgfill", "PlayerbotBgFillDriver: failed to clone {} into instance {}.", source->GetName(), bg->GetInstanceID());
                continue;
            }

            seated(*sourceGuid, source->GetClass());
            TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: seated {} (copy of online bot {}) on team {} of instance {}.",
                clone->GetName(), source->GetName(), team, bg->GetInstanceID());
        }
    };

    // 2. Offline characters from the bot accounts, loaded in the background.
    // Read on first need, after the online pass has had its go.
    SourceDeck offline;
    WorldSession* callbackSession = nullptr;
    bool offlineRead = false;
    auto const seatOffline = [&](bool anyRole)
    {
        if (added >= wanted || !g_Config.useOfflineBots)
            return;

        if (!offlineRead)
        {
            offlineRead = true;
            RefreshOfflinePoolIfStale(nowMs);
            offline = MakeDeck(CollectOfflineSources(minLevel, maxLevel, excludedFromPools()), matchTier, mix, draws, healersNeeded.has_value());
            if (!offline.healers.empty() || !offline.others.empty())
                callbackSession = FindCallbackSession(tally, teamIndex);
        }

        while (added < wanted && callbackSession && governorAllows())
        {
            Optional<ObjectGuid> const sourceGuid = DrawSource(offline, wantsHealer(), anyRole);
            if (!sourceGuid)
                return;

            if (!playerbot::PlayerbotObcCloneManager::QueueCustomGameClone(*sourceGuid, callbackSession, bg, team, ""))
                continue;

            PendingOfflineClone pending;
            pending.instanceId = bg->GetInstanceID();
            pending.matchSerial = bg->GetMatchSerial();
            pending.team = team;
            pending.sourceGuid = *sourceGuid;
            pending.requestedMs = nowMs;
            g_PendingOfflineClones.push_back(pending);

            seated(*sourceGuid, ClassOfSource(*sourceGuid));
            TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: requested a copy of offline character {} for team {} of instance {}.",
                sourceGuid->ToString(), team, bg->GetInstanceID());
        }
    };

    // The role wanted changes at most once (healers, then everyone else), so
    // two rounds through both pools seat every seat a role can have. A role
    // the bracket has run out of leaves its seats to the other, so the match
    // still fills; the settled target makes that rare.
    for (int round = 0; round < 2; ++round)
    {
        seatOnline(false);
        seatOffline(false);
    }
    if (healersNeeded)
    {
        seatOnline(true);
        seatOffline(true);
    }
    if (offRole)
        TC_LOG_WARN("playerbots.bgfill", "PlayerbotBgFillDriver: ran out of {} bots in levels {}-{}; {} seat(s) on team {} of instance {} went to the other role.",
            wantsHealer() ? "healer" : "non-healer", minLevel, maxLevel, offRole, team, bg->GetInstanceID());

    if (!governorAllows())
        return added;

    // 2b. The pool is spent and the battleground is bigger than it: deal
    // characters in again. Fewest copies on this team first, then fewest in
    // the match, then tier, so a side repeats a name only once every
    // character is on it, and the two sides share names evenly.
    if (added < wanted && AllowsDuplicateSources(bg))
    {
        uint32 const instanceId = bg->GetInstanceID();
        std::vector<playerbot::PlayerbotObcCloneManager::CustomGameCloneInfo> const clones =
            playerbot::PlayerbotObcCloneManager::GetCustomGameClones(instanceId);
        std::unordered_map<ObjectGuid, uint32> copiesOnTeam;
        std::unordered_map<ObjectGuid, uint32> copiesInMatch;
        for (auto const& clone : clones)
        {
            ++copiesInMatch[clone.sourceGuid];
            if (clone.team == team)
                ++copiesOnTeam[clone.sourceGuid];
        }
        for (PendingOfflineClone const& pending : g_PendingOfflineClones)
        {
            if (pending.instanceId != instanceId)
                continue;

            ++copiesInMatch[pending.sourceGuid];
            if (pending.team == team)
                ++copiesOnTeam[pending.sourceGuid];
        }

        std::vector<ObjectGuid> sources = CollectOnlineSources(minLevel, maxLevel, {});
        if (g_Config.useOfflineBots)
        {
            RefreshOfflinePoolIfStale(nowMs);
            std::vector<ObjectGuid> const offline = CollectOfflineSources(minLevel, maxLevel, {});
            sources.insert(sources.end(), offline.begin(), offline.end());
        }

        OrderSourcesByTier(sources, matchTier);
        auto const copies = [](std::unordered_map<ObjectGuid, uint32> const& counts, ObjectGuid const& guid)
        {
            auto const itr = counts.find(guid);
            return itr != counts.end() ? itr->second : 0u;
        };
        std::stable_sort(sources.begin(), sources.end(), [&](ObjectGuid const& left, ObjectGuid const& right)
        {
            uint32 const leftOnTeam = copies(copiesOnTeam, left);
            uint32 const rightOnTeam = copies(copiesOnTeam, right);
            if (leftOnTeam != rightOnTeam)
                return leftOnTeam < rightOnTeam;

            return copies(copiesInMatch, left) < copies(copiesInMatch, right);
        });

        WorldSession* callbackSession = nullptr;
        bool callbackSessionLooked = false;
        for (ObjectGuid const& sourceGuid : sources)
        {
            if (added >= wanted || !governorAllows())
                break;

            if (Player* source = ObjectAccessor::FindConnectedPlayer(sourceGuid))
            {
                Player* clone = playerbot::PlayerbotObcCloneManager::CreateCustomGameClone(source, bg, team, "");
                if (!clone)
                    continue;
            }
            else
            {
                if (!callbackSessionLooked)
                {
                    callbackSession = FindCallbackSession(tally, teamIndex);
                    callbackSessionLooked = true;
                }

                if (!callbackSession)
                    continue;

                uint32 const expectedLive = CountCopiesOfSource(instanceId, sourceGuid, clones) + 1;
                if (!playerbot::PlayerbotObcCloneManager::QueueCustomGameClone(sourceGuid, callbackSession, bg, team, ""))
                    continue;

                PendingOfflineClone pending;
                pending.instanceId = instanceId;
                pending.matchSerial = bg->GetMatchSerial();
                pending.team = team;
                pending.sourceGuid = sourceGuid;
                pending.requestedMs = nowMs;
                pending.expectedLive = expectedLive;
                g_PendingOfflineClones.push_back(pending);
            }

            usedSources.insert(sourceGuid);
            ++added;
            ++matchClones;
            ++totalClones;
            TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: dealt {} in again ({} cop(ies) on team {}) for instance {}.",
                sourceGuid.ToString(), copies(copiesOnTeam, sourceGuid) + 1, team, instanceId);
        }
    }

    // 3. Mirrors of the people on the other side, one each. Never at 60: a
    // level-60 match is filled from the PvP-only pool or not at all. (The
    // Obsidian Colosseum's own "Dark" copies are that battleground's design and
    // come from PlayerbotObcCloneManager, not from here.)
    if (added < wanted && g_Config.allowHumanMirrors && maxLevel < 60)
    {
        for (ObjectGuid const& humanGuid : tally.teams[otherIndex].humanGuids)
        {
            if (added >= wanted || !governorAllows())
                break;

            if (usedSources.count(humanGuid))
                continue;

            Player* human = ObjectAccessor::FindConnectedPlayer(humanGuid);
            if (!human || !human->IsInWorld())
                continue;

            Player* clone = playerbot::PlayerbotObcCloneManager::CreateCustomGameClone(human, bg, team, "Dark ");
            if (!clone)
                continue;

            seated(humanGuid, human->GetClass());
            TC_LOG_DEBUG("playerbots.bgfill", "PlayerbotBgFillDriver: no bot fits levels {}-{}; seated a mirror of {} on team {} of instance {}.",
                minLevel, maxLevel, human->GetName(), team, bg->GetInstanceID());
        }
    }

    return added;
}

struct LiveMatch
{
    Battleground* bg = nullptr;
    uint32 instanceId = 0;
    MatchTally tally;
};

// Every live public battleground this driver is responsible for. Instances
// are stored under the type that was queued for, and every such type has a
// queue, so walking the queue types reaches all of them.
std::vector<LiveMatch> CollectLiveMatches()
{
    std::vector<LiveMatch> matches;
    for (int queueType = BATTLEGROUND_QUEUE_NONE + 1; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
    {
        BattlegroundQueueTypeId const queueTypeId = BattlegroundQueueTypeId(queueType);
        BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        uint8 const arenaType = BattlegroundMgr::BGArenaType(queueTypeId);
        if (arenaType ? !sBattlegroundMgr->IsBotFillSkirmishArena(arenaType) && !sBattlegroundMgr->IsBotFillRatedArena(arenaType)
                      : !sBattlegroundMgr->IsBotFillBattleground(bgTypeId))
            continue;

        BattlegroundContainer const* instances = sBattlegroundMgr->GetBattlegroundsByType(bgTypeId);
        if (!instances)
            continue;

        for (auto const& [instanceId, bg] : *instances)
        {
            // All arena sizes are stored under BATTLEGROUND_AA. Visit an
            // instance only from its own size's queue or it would be filled
            // once for every enabled arena queue.
            if (arenaType && bg->GetArenaType() != arenaType)
                continue;

            if (!IsFillCandidate(bg, instanceId))
                continue;

            BattlegroundStatus const status = bg->GetStatus();
            if (status != STATUS_WAIT_JOIN && status != STATUS_IN_PROGRESS)
                continue;

            LiveMatch match;
            match.bg = bg;
            match.instanceId = instanceId;
            match.tally = TallyMatch(bg);
            matches.push_back(std::move(match));
        }
    }

    return matches;
}

// The queue seats people in a preparing arena's open seats
// (Battleground::CountUnclaimedPreparationSeats), so it has to know which of
// them a clone is already loading into.
void PublishSeatsOnTheWay(Battleground* bg, uint32 instanceId)
{
    for (uint32 team : { uint32(ALLIANCE), uint32(HORDE) })
        bg->SetBotFillSeatsOnTheWay(team, CountPendingOfflineClones(instanceId, team));
}

void FillMatch(LiveMatch& match, uint32& totalClones, uint32 nowMs)
{
    Battleground* bg = match.bg;
    uint32 const cap = FillCapForMatch(bg);
    PublishSeatsOnTheWay(bg, match.instanceId);

    if (!bg->IsBotFillMatch())
    {
        bg->SetBotFillMatch(true);
        // The premature-finish rule ends a match whose team is below the
        // template minimum; a cap under that minimum would trip it on purpose.
        if (cap < bg->GetMinPlayersPerTeam())
            bg->SetMinPlayersPerTeam(cap);
    }

    // Nobody real inside yet - invites still pending, or everyone gone. A
    // clone is only worth seating when there is someone to fight, and the
    // stock rules end or delete the shell on their own. The map is also only
    // guaranteed to exist once somebody has actually teleported in.
    if (!match.tally.Humans() || !bg->FindBgMap())
        return;

    if (g_AnnouncedInstances.insert(match.instanceId).second)
    {
        uint8 const matchTier = TierOfBattleground(bg);
        TC_LOG_INFO("playerbots.bgfill", "PlayerbotBgFillDriver: filling {} instance {} (levels {}-{}) around {} real player(s), up to {} a side, tier {}.",
            bg->GetName(), match.instanceId, bg->GetMinLevel(), bg->GetMaxLevel(), match.tally.Humans(), cap,
            matchTier == kNoTier ? "none" : kTierNames[matchTier]);
    }

    std::vector<playerbot::PlayerbotObcCloneManager::CustomGameCloneInfo> const clones =
        playerbot::PlayerbotObcCloneManager::GetCustomGameClones(match.instanceId);
    // This set must live across both team passes. A per-team set lets the
    // same source be selected once for Alliance and once for Horde in the
    // same 2v2/3v3 match, producing two clones with the same displayed name.
    std::unordered_set<ObjectGuid> usedSources = CollectUsedSources(match.instanceId, clones);
    uint32 matchClones = match.tally.Clones();
    bool const arenaRemovalLocked = bg->isArena() &&
        (bg->GetStatus() == STATUS_WAIT_JOIN || bg->GetStatus() == STATUS_IN_PROGRESS);
    bool const arenaAdditionsLocked = bg->isArena() && bg->GetStatus() != STATUS_WAIT_JOIN;

    // An arena deals roles too: each side up to the healers settled for it
    // (per squad in a squad match, so a side holds that many times its squads).
    Optional<uint32> sideHealers;
    if (ConfiguredHealersPerTeam(bg))
    {
        auto const [itr, settling] = g_HealerTargets.try_emplace(match.instanceId, 0);
        if (settling)
            itr->second = SettleHealerTarget(bg, match.tally, usedSources, nowMs);
        sideHealers = itr->second * SquadsPerSide(bg);
    }

    for (uint32 teamIndex = TEAM_ALLIANCE; teamIndex < PVP_TEAMS_COUNT; ++teamIndex)
    {
        uint32 const team = teamIndex == TEAM_ALLIANCE ? ALLIANCE : HORDE;
        TeamTally const& tally = match.tally.teams[teamIndex];

        // Seats that are not the clones' to take: people in, persistent bots
        // in, and invites still outstanding (a person on the way in).
        uint32 const taken = tally.humans + tally.virtualBots + bg->GetInvitedCount(team);
        uint32 const desired = cap > taken ? cap - taken : 0;
        uint32 const present = tally.clones + CountPendingOfflineClones(match.instanceId, team);

        if (present > desired && !arenaRemovalLocked)
            ShedClonesFromTeam(bg, team, present - desired, clones);
        else if (present < desired && !arenaAdditionsLocked)
        {
            uint32 const wanted = std::min(desired - present, std::max<uint32>(g_Config.clonesPerTick, 1));
            Optional<uint32> healersNeeded;
            if (sideHealers)
            {
                uint32 const have = CountHealers(bg, team);
                healersNeeded = *sideHealers > have ? *sideHealers - have : 0;
            }
            AddClonesToTeam(bg, team, wanted, match.tally, usedSources, matchClones, totalClones, nowMs, healersNeeded);
        }
    }

    PublishSeatsOnTheWay(bg, match.instanceId);
}

// Under sustained hard pressure the governor asks for one bot at a time to go;
// the custom-game lobby answers for its matches, this answers for these.
void ShedForGovernor(std::vector<LiveMatch> const& matches)
{
    if (!playerbot::ResourceGovernor::ShouldShedBotNow())
        return;

    LiveMatch const* target = nullptr;
    for (LiveMatch const& match : matches)
        if (match.tally.Humans() && match.tally.Clones() && (!target || match.tally.Clones() > target->tally.Clones()))
            target = &match;

    if (!target)
        return;

    if (playerbot::PlayerbotObcCloneManager::ShedOneCustomGameClone(target->instanceId))
        playerbot::ResourceGovernor::NoteBotShedExecuted();
}

// The queue only re-examines a bracket when something happens in it or on its
// fifteen-second sweep. A person whose wait has run out deserves better than
// the sweep, so nudge the queue for them.
//
// An arena queue holds skirmish parties and rated teams side by side, and the
// queue tells which pass to run by the rating it is handed: 0 is the skirmish
// pass, anything else the rated one (BattlegroundMgr::Update). A rated team
// is woken with its own matchmaker rating, so its bot match does not hang on
// the forced rated update, which Arena.MaxRatingDifference = 0 turns off.
void WakeQueuesForWaitingPlayers(uint32 nowMs)
{
    for (int queueType = BATTLEGROUND_QUEUE_NONE + 1; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
    {
        BattlegroundQueueTypeId const queueTypeId = BattlegroundQueueTypeId(queueType);
        BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        uint8 const arenaType = BattlegroundMgr::BGArenaType(queueTypeId);
        bool const fillsSkirmish = arenaType ? sBattlegroundMgr->IsBotFillSkirmishArena(arenaType) : sBattlegroundMgr->IsBotFillBattleground(bgTypeId);
        // only All Arenas: a rated squad match has queues of its own and is never filled
        bool const fillsRated = arenaType && bgTypeId == BATTLEGROUND_AA && sBattlegroundMgr->IsBotFillRatedArena(arenaType);
        if (!fillsSkirmish && !fillsRated)
            continue;

        uint32 const queueWaitMs = sBattlegroundMgr->GetBotFillQueueWaitMs(arenaType);
        uint32 const ratedWaitMs = sBattlegroundMgr->GetBotFillRatedArenaQueueWaitMs();

        BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
        for (uint32 bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
        {
            bool wakeUnrated = false;
            uint32 wakeRatedMmr = 0;
            for (uint32 index = BG_QUEUE_PREMADE_ALLIANCE; index < BG_QUEUE_GROUP_TYPES_COUNT; ++index)
            {
                for (GroupQueueInfo const* ginfo : queue.m_QueuedGroups[bracket][index])
                {
                    if (ginfo->IsInvitedToBGInstanceGUID || (arenaType && ginfo->ArenaType != arenaType))
                        continue;

                    if (ginfo->IsRated ? (!fillsRated || wakeRatedMmr) : (!fillsSkirmish || wakeUnrated))
                        continue;

                    if (getMSTimeDiff(ginfo->JoinTime, nowMs) < (ginfo->IsRated ? ratedWaitMs : queueWaitMs))
                        continue;

                    for (auto const& [playerGuid, playerInfo] : ginfo->Players)
                    {
                        (void)playerInfo;
                        Player const* player = ObjectAccessor::FindConnectedPlayer(playerGuid);
                        WorldSession const* session = player ? player->GetSession() : nullptr;
                        // Somebody who switched the arena toggle off is waiting
                        // for people, not for this; waking the bracket for them
                        // would only have the queue decline once a tick.
                        if (session && !session->IsVirtualSession() &&
                            !(arenaType && player->HasArenaBotFillOptOut()))
                        {
                            if (ginfo->IsRated)
                                wakeRatedMmr = std::max<uint32>(ginfo->ArenaMatchmakerRating, 1);
                            else
                                wakeUnrated = true;
                            break;
                        }
                    }
                }
            }

            if (wakeUnrated)
                sBattlegroundMgr->ScheduleQueueUpdate(0, arenaType, queueTypeId, bgTypeId, BattlegroundBracketId(bracket));
            if (wakeRatedMmr)
                sBattlegroundMgr->ScheduleQueueUpdate(wakeRatedMmr, arenaType, queueTypeId, bgTypeId, BattlegroundBracketId(bracket));
        }
    }
}

std::string_view TrimToken(std::string_view token)
{
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
        token.remove_prefix(1);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
        token.remove_suffix(1);
    return token;
}

// "100955-100963, 101158" -> every id named or covered by a range. Trinity::StringTo
// does not skip whitespace, so tokens are trimmed first; a token that does not
// parse is reported rather than silently dropped.
std::vector<uint32> ParseIdList(std::string const& key)
{
    std::vector<uint32> ids;
    std::string const raw = sConfigMgr->GetStringDefault(key, "");
    for (std::string_view token : Trinity::Tokenize(raw, ',', false))
    {
        token = TrimToken(token);
        if (token.empty())
            continue;

        std::size_t const dash = token.find('-');
        Optional<uint32> const first = Trinity::StringTo<uint32>(TrimToken(token.substr(0, dash)));
        Optional<uint32> const last = dash == std::string_view::npos ? first : Trinity::StringTo<uint32>(TrimToken(token.substr(dash + 1)));
        if (!first || !last || !*first || *first > *last || *last - *first >= kMaxTierIdRange)
        {
            TC_LOG_ERROR("server.loading", "{}: ignoring '{}', expected an id or a range first-last.", key, token);
            continue;
        }

        for (uint32 offset = 0; offset <= *last - *first; ++offset)
            ids.push_back(*first + offset);
    }

    return ids;
}

// ---------------------------------------------------------------- bot ladder
//
// Playerbot.BgFill.RatedArena.Ladder: arena teams of the level-cap PvP-only bots.
// A rated team the queue gives clones to plays one of them instead of nameless
// clones (BattlegroundQueue::StartBotFilledRatedMatches): the clones on the other
// side are copies of that team's members (LadderRosterFor), and both teams are
// rated. Between those games the ladder's teams play each other on paper
// (SimulateLadderGames), so the standings move while nobody is queued.
//
// A ladder team is any arena team whose captain is a level-cap PvP-only
// character, which is how they are found again after a restart with nothing
// stored but the teams themselves. Teams are only ever added: turning the
// ladder off stops the queue from using them and leaves them where they are.

std::array<char const*, 32> const kLadderAdjectives =
{
    "Crimson", "Frozen", "Silent", "Savage", "Golden", "Shadow", "Iron", "Storm",
    "Burning", "Wicked", "Grim", "Swift", "Hollow", "Ashen", "Scarlet", "Feral",
    "Arcane", "Rusty", "Bitter", "Lucky", "Rowdy", "Mighty", "Sly", "Bold",
    "Thorned", "Wild", "Gilded", "Stone", "Blood", "Moon", "Sun", "Dread"
};

std::array<char const*, 32> const kLadderNouns =
{
    "Vanguard", "Reavers", "Wardens", "Jackals", "Ravens", "Wolves", "Serpents", "Hammers",
    "Blades", "Lancers", "Outlaws", "Zealots", "Marauders", "Sentinels", "Vipers", "Bandits",
    "Mystics", "Brutes", "Gnolls", "Murlocs", "Raptors", "Ogres", "Drakes", "Harpies",
    "Kobolds", "Titans", "Rangers", "Knights", "Vultures", "Scorpids", "Sharks", "Bears"
};

std::array<uint8, 4> const kLadderArenaTypes = { ARENA_TYPE_2v2, ARENA_TYPE_3v3, ARENA_TYPE_4v4, ARENA_TYPE_5v5 };

// "Crimson Ravens" - any pair still free and valid as a charter name.
std::string MakeLadderTeamName()
{
    for (uint32 attempt = 0; attempt < 200; ++attempt)
    {
        std::string const name = Trinity::StringFormat("{} {}",
            kLadderAdjectives[urand(0, uint32(kLadderAdjectives.size()) - 1)], kLadderNouns[urand(0, uint32(kLadderNouns.size()) - 1)]);
        if (ObjectMgr::IsValidCharterName(name) && !sObjectMgr->IsReservedName(name) && !sArenaTeamMgr->GetArenaTeamByName(name))
            return name;
    }

    return {};
}

struct LadderCandidate
{
    ObjectGuid guid;
    uint8 classId = 0;
    bool healer = false;
};

// The level-cap characters of the PvP-only accounts: the pool the arena fill
// copies at the cap, and the only one the ladder's teams are made of.
std::vector<LadderCandidate> CollectLadderCandidates()
{
    uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    auto const& pvpOnlyAccounts = playerbot::PveManager::GetConfig().pvpOnlyAccountIds;

    std::vector<LadderCandidate> candidates;
    for (OfflineCandidate const& entry : g_OfflinePool.entries)
    {
        if (entry.level != maxLevel || !std::binary_search(pvpOnlyAccounts.begin(), pvpOnlyAccounts.end(), entry.accountId))
            continue;

        LadderCandidate candidate;
        candidate.guid = ObjectGuid::Create<HighGuid::Player>(entry.lowGuid);
        candidate.classId = entry.classId;
        candidate.healer = g_OfflinePool.healers.count(entry.lowGuid) != 0;
        candidates.push_back(candidate);
    }

    return candidates;
}

// The roster of a new team of `type`, from the candidates not yet in a team of
// that size: the healers the arena fill deals a side (HealersPerTeam, or
// ...OfFive), then everyone else, each class once while there is a choice.
// Empty when not enough are left.
std::vector<ObjectGuid> DealLadderRoster(std::vector<LadderCandidate> const& candidates, uint8 type)
{
    std::vector<LadderCandidate const*> free;
    for (LadderCandidate const& candidate : candidates)
        if (!sCharacterCache->GetCharacterArenaTeamIdByGuid(candidate.guid, type))
            free.push_back(&candidate);
    Trinity::Containers::RandomShuffle(free);

    uint32 const healers = std::min<uint32>(type >= ARENA_TYPE_5v5 ? g_Config.healersPerTeamOfFive : g_Config.healersPerTeam, type);
    std::vector<ObjectGuid> roster;
    std::array<bool, MAX_CLASSES> classTaken = {};
    auto const take = [&](bool healer, bool repeatClasses, uint32 upTo)
    {
        for (LadderCandidate const*& candidate : free)
        {
            if (!candidate || roster.size() >= upTo || candidate->healer != healer)
                continue;

            bool const knownClass = candidate->classId < MAX_CLASSES;
            if (!repeatClasses && knownClass && classTaken[candidate->classId])
                continue;

            roster.push_back(candidate->guid);
            if (knownClass)
                classTaken[candidate->classId] = true;
            candidate = nullptr;
        }
    };

    take(true, false, healers);
    take(true, true, healers);
    take(false, false, type);
    take(false, true, type);
    // short of anybody else, a healer more
    take(true, false, type);
    take(true, true, type);

    if (roster.size() < type)
        roster.clear();
    return roster;
}

// Finds the ladder's teams, makes the ones the config still asks for, and hands
// the lot to the queue (BattlegroundMgr::SetArenaLadderTeams).
void SyncArenaLadder()
{
    if (!g_Config.enabled || !g_Config.ratedArenasEnabled || !g_Config.ladderEnabled)
    {
        sBattlegroundMgr->SetArenaLadderTeams({});
        return;
    }

    // read fresh: the teams have to be dealt from who exists now
    g_OfflinePool.loaded = false;
    RefreshOfflinePoolIfStale(GameTime::GetGameTimeMS());
    std::vector<LadderCandidate> const candidates = CollectLadderCandidates();

    std::unordered_set<ObjectGuid> candidateGuids;
    for (LadderCandidate const& candidate : candidates)
        candidateGuids.insert(candidate.guid);

    std::vector<uint32> ladder;
    std::array<uint32, ARENA_TYPE_5v5 + 1> teamsOfType = {};
    for (auto const& [teamId, team] : sArenaTeamMgr->GetArenaTeams())
    {
        if (team->GetType() >= teamsOfType.size() || !candidateGuids.count(team->GetCaptain()))
            continue;

        ladder.push_back(teamId);
        ++teamsOfType[team->GetType()];
    }

    uint32 made = 0;
    for (uint8 type : kLadderArenaTypes)
    {
        while (teamsOfType[type] < g_Config.ladderTeams[type])
        {
            std::vector<ObjectGuid> const roster = DealLadderRoster(candidates, type);
            std::string const name = roster.empty() ? std::string() : MakeLadderTeamName();
            if (name.empty())
            {
                TC_LOG_WARN("server.loading", "Playerbot arena ladder: {}v{} has {} of the {} teams asked for; there are not enough level-cap PvP-only bots left to make another.",
                    uint32(type), uint32(type), teamsOfType[type], g_Config.ladderTeams[type]);
                break;
            }

            // an emblem of its own: any background, emblem and border colour
            ArenaTeam* team = new ArenaTeam();
            if (!team->Create(roster.front(), type, name, 0xFF000000 | urand(0, 0xFFFFFF), uint8(urand(0, 99)),
                0xFF000000 | urand(0, 0xFFFFFF), uint8(urand(0, 5)), 0xFF000000 | urand(0, 0xFFFFFF)))
            {
                delete team;
                TC_LOG_ERROR("server.loading", "Playerbot arena ladder: could not make the {}v{} team '{}'.", uint32(type), uint32(type), name);
                break;
            }

            sArenaTeamMgr->AddArenaTeam(team);
            for (std::size_t member = 1; member < roster.size(); ++member)
                team->AddMember(roster[member]);

            ladder.push_back(team->GetId());
            ++teamsOfType[type];
            ++made;
        }
    }

    std::sort(ladder.begin(), ladder.end());
    sBattlegroundMgr->SetArenaLadderTeams(ladder);

    TC_LOG_INFO("server.loading", "Playerbot arena ladder: {} teams (2v2 {}, 3v3 {}, 4v4 {}, 5v5 {}), {} of them new, from {} level-cap PvP-only bots.",
        uint32(ladder.size()), teamsOfType[ARENA_TYPE_2v2], teamsOfType[ARENA_TYPE_3v3], teamsOfType[ARENA_TYPE_4v4],
        teamsOfType[ARENA_TYPE_5v5], made, uint32(candidates.size()));
}

// The members of the ladder team holding `team`'s side of a rated bot match
// (Battleground::GetBotFillSide), or nobody.
std::vector<ObjectGuid> LadderRosterFor(Battleground const* bg, uint32 team)
{
    std::vector<ObjectGuid> roster;
    if (!bg->isRated() || !team || bg->GetBotFillSide() != team)
        return roster;

    if (ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(bg->GetArenaTeamIdForTeam(team)))
        for (auto itr = arenaTeam->m_membersBegin(); itr != arenaTeam->m_membersEnd(); ++itr)
            roster.push_back(itr->Guid);

    return roster;
}

// A team's edge in a game on paper: its members' tier (Playerbot.BgFill.Tier.*),
// worth 150 rating a step either side of medium, a member in no tier counting
// as medium. The ladder starts everybody level; this is what lets the stronger
// rosters work their way up it.
float LadderTierEdge(ArenaTeam* team)
{
    float tiers = 0.0f;
    uint32 members = 0;
    for (auto itr = team->m_membersBegin(); itr != team->m_membersEnd(); ++itr, ++members)
    {
        uint8 const tier = TierOfSource(itr->Guid);
        tiers += tier == kNoTier ? 1.0f : float(tier);
    }

    return members ? (tiers / float(members) - 1.0f) * 150.0f : 0.0f;
}

// One game on paper between two of `teams` (all of one size, none of them in a
// real match): a team at random against one of the three nearest it in
// matchmaker rating - not the one it met last while there is anybody else. The
// winner is rolled from the stock Elo chance on their ratings plus
// LadderTierEdge, and both teams and all their members are rated as a real game
// would rate them. Both are taken out of `teams`, so one round never plays a
// team twice.
void PlayLadderGameOnPaper(std::vector<ArenaTeam*>& teams, uint8 type)
{
    constexpr std::size_t NEAREST = 3;

    ArenaTeam* const first = teams[urand(0, uint32(teams.size()) - 1)];
    uint32 const firstMatchmakerRating = first->GetRosterAverageMMR();

    std::vector<ArenaTeam*> opponents;
    for (ArenaTeam* team : teams)
        if (team != first && (teams.size() <= 2 || team->GetId() != first->GetPreviousOpponents()))
            opponents.push_back(team);

    std::sort(opponents.begin(), opponents.end(), [firstMatchmakerRating](ArenaTeam* left, ArenaTeam* right)
    {
        return std::abs(int32(left->GetRosterAverageMMR()) - int32(firstMatchmakerRating)) <
            std::abs(int32(right->GetRosterAverageMMR()) - int32(firstMatchmakerRating));
    });
    ArenaTeam* const second = opponents[urand(0, uint32(std::min(opponents.size(), NEAREST)) - 1)];
    uint32 const secondMatchmakerRating = second->GetRosterAverageMMR();

    teams.erase(std::remove_if(teams.begin(), teams.end(), [first, second](ArenaTeam* team)
    {
        return team == first || team == second;
    }), teams.end());

    float const firstStrength = std::max(0.0f, float(firstMatchmakerRating) + LadderTierEdge(first));
    float const secondStrength = std::max(0.0f, float(secondMatchmakerRating) + LadderTierEdge(second));
    bool const firstWins = roll_chance_f(first->GetChanceAgainst(uint32(firstStrength), uint32(secondStrength)) * 100.0f);

    ArenaTeam* const winner = firstWins ? first : second;
    ArenaTeam* const loser = firstWins ? second : first;
    uint32 const winnerMatchmakerRating = firstWins ? firstMatchmakerRating : secondMatchmakerRating;
    uint32 const loserMatchmakerRating = firstWins ? secondMatchmakerRating : firstMatchmakerRating;

    int32 winnerChange = 0;
    int32 loserChange = 0;
    int32 const winnerMatchmakerChange = winner->WonAgainst(winnerMatchmakerRating, loserMatchmakerRating, winnerChange);
    int32 const loserMatchmakerChange = loser->LostAgainst(loserMatchmakerRating, winnerMatchmakerRating, loserChange);

    std::vector<ObjectGuid> members;
    for (auto itr = winner->m_membersBegin(); itr != winner->m_membersEnd(); ++itr)
        members.push_back(itr->Guid);
    for (ObjectGuid const& member : members)
        winner->OfflineMemberWon(member, loserMatchmakerRating, winnerMatchmakerChange);

    members.clear();
    for (auto itr = loser->m_membersBegin(); itr != loser->m_membersEnd(); ++itr)
        members.push_back(itr->Guid);
    for (ObjectGuid const& member : members)
        loser->OfflineMemberLost(member, winnerMatchmakerRating, loserMatchmakerChange);

    winner->SetPreviousOpponents(loser->GetId());
    loser->SetPreviousOpponents(winner->GetId());
    winner->SaveToDB();
    loser->SaveToDB();
    winner->NotifyStatsChanged();
    loser->NotifyStatsChanged();

    TC_LOG_INFO("playerbots.bgfill", "Playerbot arena ladder: {}v{} {} ({} {:+}) beat {} ({} {:+}) on paper.",
        uint32(type), uint32(type), winner->GetName(), winner->GetRating(), winnerChange,
        loser->GetName(), loser->GetRating(), loserChange);
}

// A round of games on paper: Ladder.SimulateGames a size, while there are two
// ladder teams of that size left that are in no real match and have not played
// this round.
void SimulateLadderGames()
{
    for (uint8 type : kLadderArenaTypes)
    {
        std::vector<ArenaTeam*> teams;
        for (uint32 teamId : sBattlegroundMgr->GetArenaLadderTeams())
            if (ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId))
                if (team->GetType() == type && !sBattlegroundMgr->IsArenaTeamInMatch(teamId))
                    teams.push_back(team);

        for (uint32 game = 0; game < g_Config.ladderSimulateGames && teams.size() >= 2; ++game)
            PlayLadderGameOnPaper(teams, type);
    }
}
}

namespace playerbot
{
void PlayerbotBgFillDriver::LoadConfig()
{
    BgFillConfig config;
    config.enabled = sConfigMgr->GetBoolDefault("Playerbot.BgFill.Enable", false) && PvpCore::GetConfig().moduleEnabled;
    config.queueWaitMs = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.QueueWaitSeconds", 15), 0)) * IN_MILLISECONDS;
    config.skirmishArenasEnabled = sConfigMgr->GetBoolDefault("Playerbot.BgFill.SkirmishArena.Enable", true);
    config.skirmishArenaQueueWaitMs = uint32(std::max<int32>(
        sConfigMgr->GetIntDefault("Playerbot.BgFill.SkirmishArena.QueueWaitSeconds", 0), 0)) * IN_MILLISECONDS;
    config.ratedArenasEnabled = sConfigMgr->GetBoolDefault("Playerbot.BgFill.RatedArena.Enable", false);
    config.ratedArenaQueueWaitMs = uint32(std::max<int32>(
        sConfigMgr->GetIntDefault("Playerbot.BgFill.RatedArena.QueueWaitSeconds", 30), 0)) * IN_MILLISECONDS;
    config.ratedArenaAutoCreateTeam = sConfigMgr->GetBoolDefault("Playerbot.BgFill.RatedArena.AutoCreateTeam", true);
    config.ladderEnabled = sConfigMgr->GetBoolDefault("Playerbot.BgFill.RatedArena.Ladder.Enable", false);
    config.ladderTeams[ARENA_TYPE_2v2] = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.RatedArena.Ladder.Teams2v2", 12), 0));
    config.ladderTeams[ARENA_TYPE_3v3] = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.RatedArena.Ladder.Teams3v3", 9), 0));
    config.ladderTeams[ARENA_TYPE_4v4] = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.RatedArena.Ladder.Teams4v4", 6), 0));
    config.ladderTeams[ARENA_TYPE_5v5] = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.RatedArena.Ladder.Teams5v5", 6), 0));
    config.ladderSimulateMs = uint32(std::max<int32>(
        sConfigMgr->GetIntDefault("Playerbot.BgFill.RatedArena.Ladder.SimulateMinutes", 5), 0)) * MINUTE * IN_MILLISECONDS;
    config.ladderSimulateGames = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.RatedArena.Ladder.SimulateGames", 1), 0));
    config.maxPerTeam = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.MaxPerTeam", 15), 0));
    config.clonesPerTick = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.ClonesPerTick", 2), 1));
    config.useOfflineBots = sConfigMgr->GetBoolDefault("Playerbot.BgFill.UseOfflineBots", true);
    config.allowHumanMirrors = sConfigMgr->GetBoolDefault("Playerbot.BgFill.AllowHumanMirrors", true);
    config.duplicateMinTeamSize = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.DuplicateSources.MinTeamSize", 40), 0));
    config.duplicateMaxPerTeam = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.DuplicateSources.MaxPerTeam", 0), 0));
    config.classVarietyTeamPenalty = std::max(sConfigMgr->GetFloatDefault("Playerbot.BgFill.ClassVariety.TeamPenalty", 4.0f), 0.0f);
    config.classVarietyMatchPenalty = std::max(sConfigMgr->GetFloatDefault("Playerbot.BgFill.ClassVariety.MatchPenalty", 1.0f), 0.0f);
    config.healersPerTeam = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.SkirmishArena.HealersPerTeam", 1), 0));
    config.healersPerTeamOfFive = uint32(std::max<int32>(sConfigMgr->GetIntDefault("Playerbot.BgFill.SkirmishArena.HealersPerTeamOfFive", 2), 0));

    std::string const types = sConfigMgr->GetStringDefault("Playerbot.BgFill.BattlegroundTypes", "");
    for (std::string_view token : Trinity::Tokenize(types, ',', false))
        if (Optional<uint32> typeId = Trinity::StringTo<uint32>(token))
            if (*typeId)
                config.battlegroundTypes.insert(*typeId);

    uint32 tierCharacters[kTierCount] = {};
    uint32 tierBattlegrounds[kTierCount] = {};
    for (uint8 tier = 0; tier < kTierCount; ++tier)
    {
        std::string const prefix = std::string("Playerbot.BgFill.Tier.") + kTierNames[tier];
        for (uint32 guid : ParseIdList(prefix + ".Characters"))
        {
            auto const [itr, inserted] = config.tierByCharacter.emplace(guid, tier);
            if (inserted)
                ++tierCharacters[tier];
            else if (itr->second != tier)
                TC_LOG_ERROR("server.loading", "{}.Characters: character {} is already in the {} tier; it stays there.",
                    prefix, guid, kTierNames[itr->second]);
        }

        for (uint32 typeId : ParseIdList(prefix + ".BattlegroundTypes"))
        {
            auto const [itr, inserted] = config.tierByBattleground.emplace(typeId, tier);
            if (inserted)
                ++tierBattlegrounds[tier];
            else if (itr->second != tier)
                TC_LOG_ERROR("server.loading", "{}.BattlegroundTypes: battleground type {} is already in the {} tier; it stays there.",
                    prefix, typeId, kTierNames[itr->second]);
        }
    }

    g_Config = std::move(config);
    g_OfflinePool.loaded = false;

    BattlegroundMgr::BotFillPolicy policy;
    policy.enabled = g_Config.enabled;
    policy.battlegroundTypes = g_Config.battlegroundTypes;
    policy.queueWaitMs = g_Config.queueWaitMs;
    policy.skirmishArenasEnabled = g_Config.skirmishArenasEnabled;
    policy.skirmishArenaQueueWaitMs = g_Config.skirmishArenaQueueWaitMs;
    policy.ratedArenasEnabled = g_Config.ratedArenasEnabled;
    policy.ratedArenaQueueWaitMs = g_Config.ratedArenaQueueWaitMs;
    policy.ratedArenaAutoCreateTeam = g_Config.ratedArenaAutoCreateTeam;
    sBattlegroundMgr->SetBotFillPolicy(std::move(policy));

    if (g_Config.enabled)
    {
        PvpCoreConfig const& pvp = PvpCore::GetConfig();
        if (!pvp.pvpCoreEnabled || !pvp.pvpLifecycleEnabled)
            TC_LOG_WARN("server.loading", "Playerbot battleground fill is enabled but the PvP core/lifecycle modules are off; clones would stand idle.");
    }

    TC_LOG_INFO("server.loading", "Playerbot battleground fill config: enabled={}, types={}, queueWait={} ms, skirmishArenas={}, skirmishQueueWait={} ms, ratedArenas={}, ratedQueueWait={} ms, ratedAutoTeam={}, skirmishHealers={}/{}, maxPerTeam={}, clonesPerTick={}, offlineBots={}, humanMirrors={}, duplicatesFromTeamSize={}, duplicateMaxPerTeam={}, classVariety={}/{}.",
        g_Config.enabled ? "true" : "false", g_Config.battlegroundTypes.empty() ? "all" : types,
        g_Config.queueWaitMs, g_Config.skirmishArenasEnabled ? "true" : "false", g_Config.skirmishArenaQueueWaitMs,
        g_Config.ratedArenasEnabled ? "true" : "false", g_Config.ratedArenaQueueWaitMs, g_Config.ratedArenaAutoCreateTeam ? "true" : "false",
        g_Config.healersPerTeam, g_Config.healersPerTeamOfFive,
        g_Config.maxPerTeam, g_Config.clonesPerTick,
        g_Config.useOfflineBots ? "true" : "false", g_Config.allowHumanMirrors ? "true" : "false",
        g_Config.duplicateMinTeamSize, g_Config.duplicateMaxPerTeam,
        g_Config.classVarietyTeamPenalty, g_Config.classVarietyMatchPenalty);

    if (!g_Config.tierByCharacter.empty() || !g_Config.tierByBattleground.empty())
        TC_LOG_INFO("server.loading", "Playerbot battleground fill tiers (bots/battlegrounds): easy {}/{}, medium {}/{}, hard {}/{}.",
            tierCharacters[0], tierBattlegrounds[0], tierCharacters[1], tierBattlegrounds[1], tierCharacters[2], tierBattlegrounds[2]);

    if (g_WorldStarted)
        SyncArenaLadder();
}

void PlayerbotBgFillDriver::OnStartup()
{
    g_WorldStarted = true;
    SyncArenaLadder();
}

void PlayerbotBgFillDriver::OnWorldUpdate(uint32 diffMs)
{
    if (!g_Config.enabled)
        return;

    uint32 const nowMs = GameTime::GetGameTimeMS();

    g_QueueWakeAccumulatorMs += diffMs;
    if (g_QueueWakeAccumulatorMs >= kQueueWakeIntervalMs)
    {
        g_QueueWakeAccumulatorMs = 0;
        WakeQueuesForWaitingPlayers(nowMs);
    }

    if (g_Config.ladderEnabled && g_Config.ratedArenasEnabled && g_Config.ladderSimulateMs)
    {
        g_LadderSimulateAccumulatorMs += diffMs;
        if (g_LadderSimulateAccumulatorMs >= g_Config.ladderSimulateMs)
        {
            g_LadderSimulateAccumulatorMs = 0;
            SimulateLadderGames();
        }
    }

    g_TickAccumulatorMs += diffMs;
    if (g_TickAccumulatorMs < kTickIntervalMs)
        return;
    g_TickAccumulatorMs = 0;

    ReconcilePendingOfflineClones(nowMs);

    std::vector<LiveMatch> matches = CollectLiveMatches();

    std::unordered_set<uint32> liveIds;
    uint32 totalClones = 0;
    for (LiveMatch const& match : matches)
    {
        liveIds.insert(match.instanceId);
        totalClones += match.tally.Clones();
    }
    for (auto itr = g_AnnouncedInstances.begin(); itr != g_AnnouncedInstances.end();)
    {
        if (liveIds.find(*itr) == liveIds.end())
            itr = g_AnnouncedInstances.erase(itr);
        else
            ++itr;
    }
    for (auto itr = g_HealerTargets.begin(); itr != g_HealerTargets.end();)
    {
        if (liveIds.find(itr->first) == liveIds.end())
            itr = g_HealerTargets.erase(itr);
        else
            ++itr;
    }

    ShedForGovernor(matches);

    for (LiveMatch& match : matches)
        FillMatch(match, totalClones, nowMs);
}
}
