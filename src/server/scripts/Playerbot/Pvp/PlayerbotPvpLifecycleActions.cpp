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

#include "PlayerbotPvpLifecycleActions.h"
#include "PlayerbotCtfCoordinator.h"
#include "PlayerbotNodeCoordinator.h"
#include "PlayerbotObcClone.h"
#include "PlayerbotObjectiveCoordinator.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"
#include "PlayerbotPvpClassActions.h"
#include "PlayerbotRandomBotParticipation.h"
#include "PlayerbotSharedStateGuard.h"
#include "SpellHistory.h"
#include "BattlegroundMgr.h"
#include "BattlegroundFence.h"
#include "Battleground.h"
#include "BattlegroundQueue.h"
#include "BattlegroundTP.h"
#include "BattlegroundWS.h"
#include "DBCStores.h"
#include "Time/GameTime.h"
#include "GameObject.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "CharacterCache.h"
#include "Creature.h"
#include "Chat.h"
#include "Vehicle.h"
#include "Log.h"
#include "Map.h"
#include "Miscellaneous/TournamentMode.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "Movement/AbstractFollower.h"
#include "MoveSpline.h"
#include "MovementTypedefs.h"
#include "Opcodes.h"
#include "ObjectAccessor.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Pet.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Util.h"
#include "Containers.h"
#include "CommonHelpers.h"
#include "Configuration/Config.h"
#include "StringConvert.h"

#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <algorithm>
#include <queue>
#include <vector>
#include <list>

namespace playerbot
{
    uint64 BuildBattlegroundInstanceKey(Battleground const* battleground);
    Player* FindNearestEnemyBattlegroundPlayer(Player* player, float maxDistance, uint32* scannedPlayers = nullptr, uint32* attackableEnemies = nullptr);
    bool EngageNearestEnemyPlayer(Player* player, float scanDistance);
    void ApplyDeterministicObjectiveOffset(Battleground const* battleground, Player const* player, Position& destination);
    bool TryGetObjectivePosition(Battleground* battleground, Player* player, Position& destination);
    void WhisperAutoShotDiagnosticToArena(Player* player, std::string const& message, uint32 throttleMs);
}

namespace
{
    using playerbot::WhisperAutoShotDiagnosticToArena;

    bool WantsPlayerbotDiagnostics(Player const* observer)
    {
        WorldSession const* session = observer ? observer->GetSession() : nullptr;
        return session && session->IsGmDiagnosticEnabled(GmDiagnosticCategory::Playerbot);
    }

    std::unordered_map<uint64, uint32> g_HunterAutoShotPauseUntilMs;
    std::unordered_map<uint64, uint32> g_HunterAutoShotPlantFireSequence;
    std::mutex g_HunterAutoShotEventLock;
    std::unordered_map<uint64, uint32> g_HunterAutoShotFireSequence;
    std::unordered_map<uint64, uint32> g_HunterForceFleeUntilMs;
    std::unordered_map<uint64, uint32> g_HunterLastFleeIssueMs;
    std::unordered_map<uint64, uint32> g_HunterKiteHoldUntilMs;
    std::unordered_map<uint64, int8> g_HunterKiteSideByGuid;
    // Set when the core refused Auto Shot; suppresses planting until it expires.
    std::unordered_map<uint64, uint32> g_HunterAutoShotCoreRejectUntilMs;
    // std::unordered_map<uint64, uint32> g_HunterAimedDiagLastMsByGuid; // Aimed Shot whisper diagnostics disabled.

    struct HunterFleeState
    {
        Position destination;
        ObjectGuid targetGuid = ObjectGuid::Empty;
        uint32 untilMs = 0;
    };

    std::unordered_map<uint64, HunterFleeState> g_HunterFleeStateByGuid;
    constexpr uint32 PLAYERBOT_HUNTER_KITE_HOLD_MS = 3500;
    constexpr uint32 PLAYERBOT_HUNTER_FLEE_STICK_MS = 4200;
    constexpr uint32 PLAYERBOT_HUNTER_STUTTER_MAX_PLANT_MS = 2200;
    constexpr uint32 PLAYERBOT_HUNTER_POST_PLANT_FORCE_FLEE_MS = 1150;
    constexpr uint32 PLAYERBOT_HUNTER_STUTTER_PLANT_LEAD_MS = 550;
    constexpr uint32 PLAYERBOT_HUNTER_FLEE_REISSUE_MS = 350;
    // How long to stop trying to plant after the CORE refused the shot. Without
    // a backoff the kite loop re-arms the plant on the very next pass, because
    // spell 75's failed CheckCast never resets the ranged timer - see the
    // core-reject branch in IsHunterAutoShotPlantActiveInternal.
    constexpr uint32 PLAYERBOT_HUNTER_AUTOSHOT_CORE_REJECT_BACKOFF_MS = 5000;
    constexpr uint32 PLAYERBOT_PRIEST_ELUNES_GRACE_SPELL_ID = 81351;
    constexpr uint32 PLAYERBOT_PRIEST_WISP_FORM_SPELL_ID = 81352;
constexpr uint32 kHunterFeignDeathSpellId = 5384;
constexpr uint32 kPlayerbotHunterStationaryCastLockToken = 900006;
constexpr uint32 kPlayerbotShadowmeldGraceToken = 900007;
// Environmental Magma periodically damages units standing on hazardous ground.
// Some custom terrain does not expose the matching liquid flags, so the aura is
// also a generic signal that the bot is currently standing in a hazard.
constexpr uint32 kEnvironmentalMagmaDamageAuraId = 57634;

    bool IsHunterKiteHoldActive(Player const* player, uint32 nowMs = GameTime::GetGameTimeMS())
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return false;

        std::lock_guard<std::mutex> stateGuard(playerbot::SharedBotStateStructureLock());
        auto itr = g_HunterKiteHoldUntilMs.find(player->GetGUID().GetRawValue());
        if (itr == g_HunterKiteHoldUntilMs.end())
            return false;

        if (itr->second <= nowMs)
        {
            g_HunterKiteHoldUntilMs.erase(itr);
            return false;
        }

        return true;
    }

    void MarkHunterKiteHold(Player const* player, uint32 holdMs = PLAYERBOT_HUNTER_KITE_HOLD_MS)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return;

        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint64 const guid = player->GetGUID().GetRawValue();
        uint32 const untilMs = nowMs + holdMs;
        uint32& existingUntilMs = playerbot::LockedGetOrCreate(g_HunterKiteHoldUntilMs, guid);
        if (existingUntilMs < untilMs)
            existingUntilMs = untilMs;
    }

    uint32 GetHunterAutoShotFireSequence(Player const* player)
    {
        if (!player)
            return 0;

        std::lock_guard<std::mutex> lock(g_HunterAutoShotEventLock);
        auto itr = g_HunterAutoShotFireSequence.find(player->GetGUID().GetRawValue());
        return itr != g_HunterAutoShotFireSequence.end() ? itr->second : 0;
    }

    std::unordered_map<uint64, uint32> g_BattlegroundNoHumanSinceMsByInstance;
    constexpr uint32 PLAYERBOT_BG_NO_HUMAN_END_DELAY_MS = 45000;
    constexpr uint32 PLAYERBOT_BG_WAIT_JOIN_NO_HUMAN_END_DELAY_MS = 15000;
    constexpr uint32 PLAYERBOT_BG_OVERSTACK_MIN_DIFF = 2;
    constexpr uint32 PLAYERBOT_BG_OVERSTACK_REQUEUE_COOLDOWN_MS = 30000;
    constexpr uint32 PLAYERBOT_BG_OVERSTACK_INSTANCE_DEPARTURE_SPACING_MS = 8000;
    constexpr uint32 PLAYERBOT_BG_OVERSTACK_INSTANCE_JITTER_WINDOW_MS = 14000;
    constexpr uint32 PLAYERBOT_BG_HUMAN_INTEREST_REBALANCE_THROTTLE_MS = 5000;
    constexpr uint32 PLAYERBOT_BG_SCM_REFILL_THROTTLE_MS = 3000;
    constexpr uint32 PLAYERBOT_BG_QUEUE_REQUEUE_TIMEOUT_MS = 15000;
    constexpr uint32 PLAYERBOT_BG_RELEASE_SPIRIT_DELAY_MS = 1000;
    constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT = 29073;
    constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK = 22734;
    constexpr uint32 SPELL_WAITING_FOR_RESURRECT = 2584;
    constexpr uint32 SPELL_DESERTER = 26013;
    std::unordered_map<uint64, uint32> g_BattlegroundOverstackRequeueCooldownUntilMsByGuid;
    std::unordered_map<uint64, uint32> g_BattlegroundOverstackInstanceNextDepartureMsByInstance;
    std::unordered_map<uint64, uint32> g_BattlegroundQueuedNoInviteSinceMsByGuid;
    std::unordered_set<uint64> g_BattlegroundDeadBotGuids;
    std::unordered_set<uint64> g_BattlegroundSpiritQueuedBotGuids;
    uint32 g_LastHumanInterestPopulationRebalanceAttemptMs = 0;
    uint32 g_LastScmSlotRefillAttemptMs = 0;
    bool BattlegroundHasAnyRealHumanPlayers(Player const* player);
    bool RemoveMatchingQueues(Player* player, bool arenaOnly, bool invitedOnly, bool scheduleNonArenaUpdate);
    uint32 QueueEligibleManagedBotsForBattleground(BattlegroundTypeId bgTypeId, uint8 arenaType, bool respectQueueOnlySegments);

    bool IsScmManagedBotCandidate(Player const* player)
    {
        if (!player)
            return false;

        // OBC clones use virtual sessions so the shared tactical playerbot code
        // controls them, but their team and lifetime are owned exclusively by
        // PlayerbotObcCloneManager. They must never enter queue, slot-refill, or
        // overstack rebalance paths intended for persistent managed bots.
        if (playerbot::PlayerbotObcCloneManager::IsActiveClone(player))
            return false;

        if (playerbot::IsManagedRandomBot(player))
            return true;

        WorldSession const* session = player->GetSession();
        return session && session->IsVirtualSession();
    }

    std::unordered_set<uint32> ParseAccountIdSetFromConfig(char const* key)
    {
        std::unordered_set<uint32> accountIds;
        std::string const raw = sConfigMgr->GetStringDefault(key, "");
        for (std::string_view token : Trinity::Tokenize(raw, ',', false))
        {
            if (Optional<uint32> accountId = Trinity::StringTo<uint32>(token))
                if (*accountId > 0)
                    accountIds.insert(*accountId);
        }

        return accountIds;
    }

    BattlegroundTypeId ResolveManagedBotQueueTargetForAccount(Player const* player)
    {
        if (!player)
            return BATTLEGROUND_SCM;

        uint32 const accountId = player->GetSession() ? player->GetSession()->GetAccountId() : 0;
        if (!accountId)
            return BATTLEGROUND_SCM;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.ScarletChapelAccounts").count(accountId))
            return BATTLEGROUND_SCM;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.BlackrockThroneAccounts").count(accountId))
            return BATTLEGROUND_BRT;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.ObsidianColosseumAccounts").count(accountId))
            return BATTLEGROUND_OBC;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.TanarisAccounts").count(accountId))
            return BATTLEGROUND_TRT;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.BattleForGilneasAccounts").count(accountId))
            return BATTLEGROUND_BFG;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.TwinPeaksAccounts").count(accountId))
            return BATTLEGROUND_TP;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.ArathiBasinAccounts").count(accountId))
            return BATTLEGROUND_AB;

        if (ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.WarsongGulchAccounts").count(accountId))
            return BATTLEGROUND_WS;

        return BATTLEGROUND_SCM;
    }

    bool IsArenaOnlyManagedBotAccount(Player const* player)
    {
        if (!player || !player->GetSession())
            return false;

        uint32 const accountId = player->GetSession()->GetAccountId();
        if (!accountId)
            return false;

        return ParseAccountIdSetFromConfig("Playerbot.PvpLifecycle.QueueOnly.ArenaAccounts").count(accountId) != 0;
    }

    uint32 ComputeOverstackDepartureJitterMs(Player const* player, Battleground const* battleground)
    {
        if (!player || !battleground)
            return 0;

        uint64 const mix = (player->GetGUID().GetRawValue() * 11400714819323198485ull) ^
            (static_cast<uint64>(battleground->GetInstanceID()) * 7046029254386353131ull);
        return static_cast<uint32>(mix % PLAYERBOT_BG_OVERSTACK_INSTANCE_JITTER_WINDOW_MS);
    }

    bool ShouldManagedBotLeaveForOverstack(Player* player, Battleground* battleground)
    {
        if (!player || !battleground || !IsScmManagedBotCandidate(player))
            return false;

        uint32 const assignedTeam = battleground->GetPlayerTeam(player->GetGUID());
        if (assignedTeam != ALLIANCE && assignedTeam != HORDE)
            return false;

        uint32 const allianceCount = battleground->GetPlayersCountByTeam(ALLIANCE);
        uint32 const hordeCount = battleground->GetPlayersCountByTeam(HORDE);
        if (!allianceCount || !hordeCount)
            return false;

        // Never rebalance on a one-player gap. A departure on 9v8 would immediately
        // flip (or re-flip) stack pressure and cause oscillation between teams.
        uint32 const absoluteTeamDiff = (allianceCount > hordeCount) ? (allianceCount - hordeCount) : (hordeCount - allianceCount);
        if (absoluteTeamDiff <= 1)
            return false;

        uint32 const botTeamCount = assignedTeam == ALLIANCE ? allianceCount : hordeCount;
        uint32 const otherTeamCount = assignedTeam == ALLIANCE ? hordeCount : allianceCount;
        if (botTeamCount <= otherTeamCount)
            return false;

        uint32 const teamDiff = botTeamCount - otherTeamCount;
        if (teamDiff < PLAYERBOT_BG_OVERSTACK_MIN_DIFF)
            return false;

        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint64 const botGuidRaw = player->GetGUID().GetRawValue();
        uint32 const cooldownUntilMs = playerbot::LockedGetOrCreate(g_BattlegroundOverstackRequeueCooldownUntilMsByGuid, botGuidRaw);
        if (cooldownUntilMs && nowMs < cooldownUntilMs)
            return false;

        uint64 const instanceKey = playerbot::BuildBattlegroundInstanceKey(battleground);
        uint32 const nextDepartureEarliestMs = playerbot::LockedGetOrCreate(g_BattlegroundOverstackInstanceNextDepartureMsByInstance, instanceKey);
        if (nextDepartureEarliestMs && nowMs < nextDepartureEarliestMs)
            return false;

        uint32 const jitterMs = ComputeOverstackDepartureJitterMs(player, battleground);
        if (nowMs % PLAYERBOT_BG_OVERSTACK_INSTANCE_JITTER_WINDOW_MS < jitterMs)
            return false;

        playerbot::LockedSet(g_BattlegroundOverstackRequeueCooldownUntilMsByGuid, botGuidRaw, nowMs + PLAYERBOT_BG_OVERSTACK_REQUEUE_COOLDOWN_MS);
        playerbot::LockedSet(g_BattlegroundOverstackInstanceNextDepartureMsByInstance, instanceKey, nowMs + PLAYERBOT_BG_OVERSTACK_INSTANCE_DEPARTURE_SPACING_MS);

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP overstack rebalance trigger: guid={} bgTypeId={} instanceId={} assignedTeam={} teamCount={} otherTeamCount={} diff={}.",
            player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID(), assignedTeam, botTeamCount,
            otherTeamCount, teamDiff);
        return true;
    }

    bool HasQueuedRealHumanForBattleground(BattlegroundTypeId targetBgType)
    {
        if (targetBgType == BATTLEGROUND_TYPE_NONE)
            return false;

        BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(targetBgType, 0);
        if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
            return false;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
        for (auto const& [queuedGuid, queueInfo] : bgQueue.m_QueuedPlayers)
        {
            (void)queueInfo;
            Player* participant = ObjectAccessor::FindConnectedPlayer(queuedGuid);
            if (!participant)
                continue;

            WorldSession const* session = participant->GetSession();
            bool const isVirtualSession = session && session->IsVirtualSession();
            if (isVirtualSession || playerbot::IsManagedRandomBot(participant))
                continue;

            // Queued somewhere no bot goes: nothing the fleet does reaches them.
            if (!Tournament::IsReachableByWorldBots(participant))
                continue;

            return true;
        }

        return false;
    }

    bool ShouldManagedBotLeaveForQueuedHuman(Player* player, Battleground* battleground)
    {
        if (!player || !battleground || !IsScmManagedBotCandidate(player))
            return false;

        if (battleground->GetTypeID() != BATTLEGROUND_SCM)
            return false;

        uint32 const maxPlayers = battleground->GetMaxPlayers();
        if (!maxPlayers || battleground->GetPlayersSize() < maxPlayers)
            return false;

        return HasQueuedRealHumanForBattleground(battleground->GetTypeID());
    }

    bool TryRefillManagedScmSlots(Player* player, Battleground* battleground)
    {
        if (!player || !battleground || battleground->GetTypeID() != BATTLEGROUND_SCM)
            return false;

        uint32 const maxPlayers = battleground->GetMaxPlayers();
        uint32 const playersInInstance = battleground->GetPlayersSize();
        if (!maxPlayers || playersInInstance >= maxPlayers)
            return false;

        // Only force-fill while real humans are participating in SCM.
        if (!BattlegroundHasAnyRealHumanPlayers(player))
            return false;

        uint32 const nowMs = GameTime::GetGameTimeMS();
        if (nowMs < g_LastScmSlotRefillAttemptMs + PLAYERBOT_BG_SCM_REFILL_THROTTLE_MS)
            return false;

        g_LastScmSlotRefillAttemptMs = nowMs;

        bool const rebalanceTriggered = playerbot::RandomBotParticipationManager::TriggerImmediateRebalance();
        uint32 const queuedCount = ::QueueEligibleManagedBotsForBattleground(BATTLEGROUND_SCM, 0, true);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP SCM refill attempt: guid={} instanceId={} players={} maxPlayers={} rebalanceTriggered={} queuedCount={}.",
            player->GetGUID().ToString(), battleground->GetInstanceID(), playersInInstance, maxPlayers, rebalanceTriggered ? 1 : 0, queuedCount);

        return rebalanceTriggered || queuedCount > 0;
    }

    void ForcePlayerbotDismount(Player* player)
    {
        if (!player)
            return;

        if (player->IsMounted())
            player->Dismount();

        player->RemoveAurasByType(SPELL_AURA_MOUNTED);
        player->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);
        player->RemoveAurasByType(SPELL_AURA_MOD_MOUNTED_SPEED_ALWAYS);
        player->RemoveAurasByType(SPELL_AURA_MOD_MOUNTED_SPEED_NOT_STACK);
        player->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
        player->RemoveAurasByType(SPELL_AURA_MOD_MOUNTED_FLIGHT_SPEED_ALWAYS);
        player->RemoveAurasByType(SPELL_AURA_MOD_FLIGHT_SPEED_NOT_STACK);

        player->UpdateSpeed(MOVE_RUN);
        player->UpdateSpeed(MOVE_SWIM);
        player->UpdateSpeed(MOVE_FLIGHT);
    }

    bool IsEffectivelyOutdoors(Player const* player)
    {
        if (!player)
            return false;

        Map const* map = player->FindMap();
        if (!map)
            return player->IsOutdoors();

        PositionFullTerrainStatus terrainStatus;
        map->GetFullTerrainStatusForPosition(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            terrainStatus, MAP_ALL_LIQUIDS, player->GetCollisionHeight());
        return player->IsOutdoors() && terrainStatus.outdoors;
    }

    bool ShouldForceIndoorDismount(Player const* player, bool outdoors, uint32 lingerMs = 1500)
    {
        if (!player)
            return false;

        static std::unordered_map<uint64, uint32> indoorSinceMsByGuid;
        uint64 const guid = player->GetGUID().GetRawValue();

        std::lock_guard<std::mutex> stateGuard(playerbot::SharedBotStateStructureLock());
        if (outdoors)
        {
            indoorSinceMsByGuid.erase(guid);
            return false;
        }

        uint32 const nowMs = GameTime::GetGameTimeMS();
        auto itr = indoorSinceMsByGuid.find(guid);
        if (itr == indoorSinceMsByGuid.end())
        {
            indoorSinceMsByGuid.emplace(guid, nowMs);
            return false;
        }

        return nowMs >= itr->second + lingerMs;
    }

    bool IsRecoveringByEatingOrDrinking(Player const* player)
    {
        if (!player || !player->IsAlive() || player->IsInCombat())
            return false;

        bool const needsFood = player->GetHealthPct() < 100.0f && player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
        bool const needsDrink = player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 100.0f &&
            player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);
        return needsFood || needsDrink;
    }

    void ClearEatDrinkAurasForMovement(Player* player)
    {
        if (!player)
            return;

        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
            player->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);
    }

    void ClearStaleWaitingForResurrectAura(Player* player)
    {
        if (!player || !player->IsAlive())
            return;

        if (player->HasAura(SPELL_WAITING_FOR_RESURRECT))
            player->RemoveAurasDueToSpell(SPELL_WAITING_FOR_RESURRECT);
    }


    bool IsWarsongGulch(Player const* player)
    {
        if (!player)
            return false;

        Battleground const* battleground = player->GetBattleground();
        return battleground && battleground->GetMapId() == 489;
    }

    TeamId ResolveTeamId(uint32 teamValue)
    {
        if (teamValue == TEAM_ALLIANCE || teamValue == ALLIANCE)
            return TEAM_ALLIANCE;
        if (teamValue == TEAM_HORDE || teamValue == HORDE)
            return TEAM_HORDE;
        return TEAM_NEUTRAL;
    }

    TeamId ResolveBotTeamId(Player const* player)
    {
        if (!player)
            return TEAM_NEUTRAL;

        if (Battleground const* battleground = player->GetBattleground())
        {
            uint32 const assignedTeam = battleground->GetPlayerTeam(player->GetGUID());
            if (assignedTeam)
            {
                TeamId const resolved = ResolveTeamId(assignedTeam);
                if (resolved != TEAM_NEUTRAL)
                    return resolved;
            }
        }

        if (uint32 const bgTeam = player->GetBGTeam())
        {
            TeamId const resolved = ResolveTeamId(bgTeam);
            if (resolved != TEAM_NEUTRAL)
                return resolved;
        }

        return ResolveTeamId(player->GetTeam());
    }

    bool MoveTowardUnit(Player* player, Unit* target, float desiredDistance);
    float GetAggressiveCombatScanDistance(Player const* player, float fallbackDistance);
    bool HasPlayerbotGapCloserInFlight(Player const* player);
    bool CanIssueBotMovement(Player* player);
    bool HunterIsHardCastingStationaryShot(Player const* player);
    void WhisperHunterAimedLifecycleDiagnostic(Player* player, Unit* target, char const* phase, char const* extra, uint32 throttleMs);
    bool BreakExpiredHunterFeignDeath(Player* player);
    bool IssueMovePointThrottled(Player* player, Position const& destination, float destinationChangeThreshold = 6.0f, uint32 minReissueMs = 2000);
    Position BuildFollowDestination(Player* player, Unit* target, float desiredDistance);
    bool IssueHumanLikeFollow(Player* player, Unit* target, float desiredDistance, float destinationChangeThreshold = 6.0f, uint32 minReissueMs = 2000);
    void EmitBattlegroundGmDebug(Player* bot, std::string const& detail, uint32 throttleMs);

    bool IsCrowdControlledForAction(Player const* player)
    {
        if (!player)
            return false;

        constexpr uint32 ccMechanicMask =
            (1u << MECHANIC_CHARM) |
            (1u << MECHANIC_DISORIENTED) |
            (1u << MECHANIC_FEAR) |
            (1u << MECHANIC_SLEEP) |
            (1u << MECHANIC_STUN) |
            (1u << MECHANIC_FREEZE) |
            (1u << MECHANIC_POLYMORPH) |
            (1u << MECHANIC_BANISH) |
            (1u << MECHANIC_HORROR) |
            (1u << MECHANIC_SAPPED);

        // LOST_CONTROL includes the normal CHARGING/JUMPING states used by
        // effect movement. Only the actual controlled/possessed subset is CC.
        bool const hasControlState = player->HasUnitState(UNIT_STATE_CONTROLLED | UNIT_STATE_POSSESSED);
        bool const hasHardCcState = player->HasUnitState(UNIT_STATE_STUNNED) ||
            player->HasUnitState(UNIT_STATE_CONFUSED) ||
            player->HasUnitState(UNIT_STATE_FLEEING);
        bool const hasCcAura = player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
            player->HasAuraWithMechanic(ccMechanicMask) ||
            player->IsPolymorphed();

        return hasControlState || hasHardCcState || hasCcAura;
    }

    bool TryPursueNearestEnemyInBattleground(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        float const combatEngageDistance = std::clamp(GetAggressiveCombatScanDistance(player, 100.0f), 25.0f, 60.0f);
        if (playerbot::EngageNearestEnemyPlayer(player, combatEngageDistance))
            return true;

        Player* nearestEnemy = playerbot::FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max(), nullptr, nullptr);
        if (!nearestEnemy)
            return false;

        bool chaseIssued = MoveTowardUnit(player, nearestEnemy, combatEngageDistance);
        if (!chaseIssued && !player->isMoving())
        {
            // If direct pursuit did not issue movement, clear stale motion so a new
            // navmesh-segmented path request can be accepted. The MovePoint helper
            // still validates battleground navigation and refuses no-path segments.
            player->GetMotionMaster()->Clear();
            chaseIssued = IssueMovePointThrottled(player, nearestEnemy->GetPosition(), 30.0f, 700) || player->isMoving();
        }

        return chaseIssued;
    }

    bool RecoverStaleBattlegroundState(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        if (player->GetBattleground())
            return false;

        // Ignore in-flight teleports; let normal worldport handling finish first.
        if (player->IsBeingTeleportedFar() || player->IsBeingTeleportedNear())
            return false;

        uint32 const staleBattlegroundId = player->GetBattlegroundId();
        BattlegroundTypeId const staleBattlegroundTypeId = player->GetBattlegroundTypeId();

        player->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE);
        player->SetBGTeam(0);

        bool const teleported = player->TeleportToBGEntryPoint();
        TC_LOG_INFO("playerbots.pvp.lifecycle",
            "Playerbot PvP stale battleground recovery: guid={} staleInstanceId={} staleTypeId={} teleported={}.",
            player->GetGUID().ToString(), staleBattlegroundId, uint32(staleBattlegroundTypeId), teleported ? 1 : 0);

        return true;
    }

    bool MoveToBattlegroundObjectivePosition(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        Battleground* battleground = player->GetBattleground();
        if (!battleground)
            return false;

        Position destination;
        if (!playerbot::TryGetObjectivePosition(battleground, player, destination))
            return false;

        if (player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f))
        {
            EmitBattlegroundGmDebug(player, "objective-skip reason=already-near-objective range=12", 1000);
            return true;
        }

        return IssueMovePointThrottled(player, destination);
    }

    bool IsLifecycleGateEnabled()
    {
        playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
        return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
    }

    // Whether this bot may be put in a battleground or arena queue by anything
    // running on its own. With Playerbot.PvpLifecycle.PersistentBots.Enable off
    // the persistent, account-backed fleet stays in the open world and instanced
    // PvP is filled entirely by transient clones - so for those bots the answer
    // is simply no, and every automatic queue path has to ask.
    //
    // A transient clone is never refused: it exists for one match, the fill
    // driver seats it, and the switch is what put it there in the first place.
    bool MayQueueForInstancedPvp(Player const* player)
    {
        if (playerbot::PvpCore::GetConfig().pvpPersistentLifecycleEnabled)
            return true;

        WorldSession const* session = player ? player->GetSession() : nullptr;
        return session && session->IsTransientPlayerSession();
    }

    std::array<BattlegroundTypeId, 6> BuildRandomBattlegroundOrder()
    {
        std::array<BattlegroundTypeId, 6> battlegroundTypes =
        {
            BATTLEGROUND_AV,
            BATTLEGROUND_EY,
            BATTLEGROUND_AB,
            BATTLEGROUND_WS,
            BATTLEGROUND_SA,
            BATTLEGROUND_IC
        };

        Trinity::Containers::RandomShuffle(battlegroundTypes);
        return battlegroundTypes;
    }


    std::string BuildQueueDebugSummary(Player* player)
    {
        if (!player)
            return "queue=none";

        std::ostringstream summary;
        summary << "queue_slots=[";
        bool first = true;
        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const queueTypeId = player->GetBattlegroundQueueTypeId(i);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            if (!first)
                summary << ',';
            first = false;
            summary << uint32(queueTypeId) << ":inv=" << (player->IsInvitedForBattlegroundQueueType(queueTypeId) ? 1 : 0);
        }

        if (first)
            summary << "none";

        summary << "] invitedArenaTeamId=" << player->GetArenaTeamIdInvited();
        return summary.str();
    }

    void EmitLifecycleDiagnostic(Player* player, char const* phase, std::string const& detail)
    {
        if (!player)
            return;

        TC_LOG_INFO("playerbots.pvp.lifecycle",
            "Playerbot lifecycle diagnostic: guid={} phase={} inBg={} bgId={} inQueue={} deserter={} {} detail={}",
            player->GetGUID().ToString(), phase ? phase : "none", player->InBattleground() ? 1 : 0, player->GetBattlegroundId(),
            player->InBattlegroundQueue() ? 1 : 0, player->HasAura(SPELL_DESERTER) ? 1 : 0, BuildQueueDebugSummary(player), detail);
    }

    void EmitBattlegroundGmDebug(Player* bot, std::string const& detail, uint32 throttleMs = 3000)
    {
        (void)bot;
        (void)detail;
        (void)throttleMs;
    }

    void EmitRehgarMovementGuardServerDiagnostic(Player* player, char const* phase, uint32 throttleMs = 100)
    {
        if (!playerbot::PvpClassActions::AreRehgarMovementDiagnosticsEnabled() ||
            !player || player->GetClass() != CLASS_SHAMAN || player->GetShapeshiftForm() != FORM_GHOSTWOLF)
            return;

        if (throttleMs)
        {
            static std::mutex throttleLock;
            static std::unordered_map<uint64, uint32> lastWhisperMsByGuid;
            uint32 const nowMs = GameTime::GetGameTimeMS();
            std::lock_guard<std::mutex> lock(throttleLock);
            uint32& lastMs = lastWhisperMsByGuid[player->GetGUID().GetRawValue()];
            if (lastMs && nowMs < lastMs + throttleMs)
                return;
            lastMs = nowMs;
        }

        MotionMaster const* motionMaster = player->GetMotionMaster();
        bool const hasSpline = player->movespline != nullptr;
        std::ostringstream os;
        os << "REHGAR DIAG: lifecycle phase=" << (phase ? phase : "unknown")
           << " bot=" << player->GetName()
           << " guid=" << player->GetGUID().ToString()
           << " motion=" << uint32(motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE)
           << " spline=" << (hasSpline ? "yes" : "no")
           << " initialized=" << (hasSpline && player->movespline->Initialized() ? "yes" : "no")
           << " finalized=" << (hasSpline && player->movespline->Finalized() ? "yes" : "no")
           << " moving=" << (player->isMoving() ? "yes" : "no")
           << " charging=" << (player->HasUnitState(UNIT_STATE_CHARGING) ? "yes" : "no")
           << " jumping=" << (player->HasUnitState(UNIT_STATE_JUMPING) ? "yes" : "no");

        std::string const message = os.str();
        for (SessionMap::value_type const& sessionPair : sWorld->GetAllSessions())
        {
            WorldSession* session = sessionPair.second;
            Player* observer = session ? session->GetPlayer() : nullptr;
            if (WantsPlayerbotDiagnostics(observer))
                ChatHandler(session).SendSysMessage(message);
        }
    }

    void ClearMovementBeforeBattlegroundTeleport(Player* player)
    {
        if (!player)
            return;

        player->AttackStop();
        player->SetSelection(ObjectGuid::Empty);

        if (player->isMoving())
            player->StopMoving();

        if (MotionMaster* motionMaster = player->GetMotionMaster())
            motionMaster->Clear();

        // The teleport/visibility path must not inherit stale client movement
        // flags from whatever the bot was doing before the queue invite. A stale
        // spline flag with an interrupted/finalized MoveSpline can make object
        // create packets assert while HandleMoveWorldportAck() rebuilds visibility.
        player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
        player->RemoveUnitMovementFlag(MOVEMENTFLAG_SPLINE_ENABLED);
        player->RemoveUnitMovementFlag(MOVEMENTFLAG_SPLINE_ELEVATION);
    }


    bool CanIssueMovementCommand(Player const* player, uint32 cooldownMs = 500)
    {
        if (!player)
            return false;

        static std::unordered_map<uint64, uint32> nextAllowedMoveCommandMsByGuid;
        uint64 const botGuid = player->GetGUID().GetRawValue();
        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32& nextAllowedMs = playerbot::LockedGetOrCreate(nextAllowedMoveCommandMsByGuid, botGuid);
        if (nowMs < nextAllowedMs)
            return false;

        nextAllowedMs = nowMs + cooldownMs;
        return true;
    }

    // Slightly tighter than the recovery's own margin so a destination is
    // pulled in before it becomes a breach, rather than landing exactly on the
    // line the recovery is watching.
    constexpr float PLAYERBOT_BG_FENCE_DESTINATION_MARGIN = 4.0f;

    Position BuildCollisionSafeDestination(Player const* player, Position const& destination)
    {
        if (!player)
            return destination;

        Position adjustedDestination = destination;
        float adjustedZ = adjustedDestination.GetPositionZ();
        player->UpdateAllowedPositionZ(adjustedDestination.GetPositionX(), adjustedDestination.GetPositionY(), adjustedZ);

        // Prefer a physically grounded/swimming destination over liquid surface
        // hovering when the location is in water and the bot is not water-walking.
        if (Map const* map = player->FindMap())
        {
            LiquidData liquidData{};
            if (map->GetLiquidStatus(player->GetPhaseMask(), adjustedDestination.GetPositionX(), adjustedDestination.GetPositionY(),
                adjustedZ + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight()))
            {
                bool const canWalkOnWater = player->HasAuraType(SPELL_AURA_WATER_WALK);
                // 0.5 below the surface keeps in-water endpoints inside the
                // stably-swimming band of the virtual-session swim hysteresis
                // (engages above 0.35 depth); anything shallower parks the bot
                // in the dead band where its swim flag never settles.
                if (!canWalkOnWater)
                    adjustedZ = std::max(liquidData.depth_level + 0.05f, std::min(adjustedZ, liquidData.level - 0.5f));
            }
        }

        adjustedDestination.Relocate(adjustedDestination.GetPositionX(), adjustedDestination.GetPositionY(), adjustedZ, adjustedDestination.GetOrientation());

        // Never set off for somewhere the fence recovery would only drag the
        // bot back from. Inert outside a battleground, and inert inside one
        // whose geometry produced no usable fence, so this costs a null check
        // on every open battleground and changes nothing there.
        if (Battleground const* bg = player->GetBattleground())
            BattlegroundFence::ClampToFence(bg, adjustedDestination, PLAYERBOT_BG_FENCE_DESTINATION_MARGIN);

        return adjustedDestination;
    }

    Position BuildFollowDestination(Player* player, Unit* target, float desiredDistance)
    {
        if (!player || !target)
            return Position();

        float x = target->GetPositionX();
        float y = target->GetPositionY();
        float z = target->GetPositionZ();
        float const followDistance = std::max(0.5f, desiredDistance);

        target->GetNearPoint(player, x, y, z, followDistance, target->GetAbsoluteAngle(player));
        Position destination(x, y, z, player->GetOrientation());
        return BuildCollisionSafeDestination(player, destination);
    }

    bool IsForbiddenBattlegroundPathType(PathType pathType)
    {
        uint32 const forbiddenPathFlags = PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH | PATHFIND_NOPATH |
            PATHFIND_SHORT | PATHFIND_FARFROMPOLY;
        return (pathType & forbiddenPathFlags) != 0;
    }

    constexpr float PLAYERBOT_BG_PATH_CALCULATION_LENGTH_LIMIT = 2400.0f;
    constexpr float PLAYERBOT_BG_MOVEMENT_SEGMENT_DISTANCE = 80.0f;

    bool BuildNavPathSegmentDestination(Player const* player, Movement::PointsArray const& points, float orientation, Position& segmentDestination)
    {
        if (!player || points.size() < 2)
            return false;

        G3D::Vector3 previous(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
        G3D::Vector3 lastRoutePoint = previous;
        float traversedDistance = 0.0f;
        bool haveRoutePoint = false;

        for (std::size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const& point = points[i];
            G3D::Vector3 const delta = point - previous;
            float const segmentLength = delta.length();
            if (segmentLength <= 0.01f)
            {
                previous = point;
                continue;
            }

            if (traversedDistance + segmentLength >= PLAYERBOT_BG_MOVEMENT_SEGMENT_DISTANCE)
            {
                // Keep the endpoint on a real Detour route vertex. Interpolating
                // XYZ between corners is unsafe on vertically stacked maps: the
                // intermediate Z can sit between floors and a later height query
                // may resolve the lower surface. Prefer the previous vertex once
                // it represents useful progress; otherwise advance to this one.
                G3D::Vector3 const& selected = haveRoutePoint && traversedDistance >= 8.0f ? lastRoutePoint : point;
                segmentDestination.Relocate(selected.x, selected.y, selected.z, orientation);
                return player->GetDistance(segmentDestination) > 0.5f;
            }

            traversedDistance += segmentLength;
            previous = point;
            lastRoutePoint = point;
            haveRoutePoint = true;
        }

        G3D::Vector3 const& finalPoint = points.back();
        segmentDestination.Relocate(finalPoint.x, finalPoint.y, finalPoint.z, orientation);
        return player->GetDistance(segmentDestination) > 0.5f;
    }

    bool TryBuildBattlegroundSegmentDestination(Player* player, Position const& safeDestination, Position& segmentDestination, PathType* resolvedPathType = nullptr)
    {
        if (!player)
            return false;

        auto const tryResolveDestination = [&](Position const& requestedDestination, Position& resolvedDestination, PathType* outPathType) -> bool
        {
            Position const collisionSafeDestination = BuildCollisionSafeDestination(player, requestedDestination);

            PathGenerator path(player);
            // Allow longer battleground route segments so bots can commit to
            // meaningful navmesh progress toward distant enemies instead of
            // repeatedly selecting tiny local hops that catch on terrain.
            path.SetPathLengthLimit(PLAYERBOT_BG_PATH_CALCULATION_LENGTH_LIMIT);
            // Never force the requested endpoint. If Detour can only build an
            // incomplete but connected route, its actual endpoint is useful
            // progress around the obstacle; forcing the endpoint converts that
            // route into a direct, non-navmesh shortcut.
            bool const pathOk = path.CalculatePath(collisionSafeDestination.GetPositionX(), collisionSafeDestination.GetPositionY(), collisionSafeDestination.GetPositionZ(), false);
            PathType pathType = path.GetPathType();
            Movement::PointsArray points = path.GetPath();
            G3D::Vector3 actualEnd = path.GetActualEndPosition();

            if (!pathOk || IsForbiddenBattlegroundPathType(pathType))
                return false;

            bool haveResolvedDestination = false;
            if (BuildNavPathSegmentDestination(player, points, collisionSafeDestination.GetOrientation(), resolvedDestination))
            {
                haveResolvedDestination = true;
            }
            else
            {
                Position actualEndDestination(actualEnd.x, actualEnd.y, actualEnd.z, collisionSafeDestination.GetOrientation());
                float const destinationDistance = player->GetDistance(collisionSafeDestination);
                float const actualEndDistance = player->GetDistance(actualEndDestination);
                if (actualEndDistance > 1.5f && actualEndDistance + 2.0f < destinationDistance)
                {
                    resolvedDestination = actualEndDestination;
                    haveResolvedDestination = true;
                }
            }

            if (!haveResolvedDestination)
                return false;

            // This endpoint already came from the accepted Detour route. Do
            // not run UpdateAllowedPositionZ over it again: on stacked geometry
            // that generic map/VMap query can replace the navmesh floor with a
            // lower one. MovePoint performs its normal generated-path validation
            // against this exact route vertex.
            float const dx = resolvedDestination.GetPositionX() - player->GetPositionX();
            float const dy = resolvedDestination.GetPositionY() - player->GetPositionY();
            float const planarDelta = std::sqrt(dx * dx + dy * dy);
            float const verticalDelta = std::fabs(resolvedDestination.GetPositionZ() - player->GetPositionZ());
            if (planarDelta < 0.5f || verticalDelta > std::max(8.0f, planarDelta * 0.75f + 2.0f))
                return false;

            if (outPathType)
                *outPathType = pathType;

            return true;
        };

        // Do not probe points along the straight line to the tactical target.
        // A probe on the bot's side of a wall is individually reachable but
        // says nothing about reaching the real destination; repeatedly choosing
        // it makes the bot run into the wall instead of following the full route.
        return tryResolveDestination(safeDestination, segmentDestination, resolvedPathType);
    }

    bool IsInHazardousLiquid(Player const* player)
    {
        if (!player)
            return false;

        Map const* map = player->FindMap();
        if (!map)
            return false;

        if (player->HasAura(kEnvironmentalMagmaDamageAuraId))
            return true;

        LiquidData liquidData{};
        ZLiquidStatus const status = map->GetLiquidStatus(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ() + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight());
        if ((status & MAP_LIQUID_STATUS_IN_CONTACT) == 0)
            return false;

        return (liquidData.type_flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0;
    }

    bool IsHazardousLiquidDestination(Player const* player, Position const& destination)
    {
        if (!player)
            return false;

        Map const* map = player->FindMap();
        if (!map)
            return false;

        LiquidData liquidData{};
        ZLiquidStatus const status = map->GetLiquidStatus(player->GetPhaseMask(), destination.GetPositionX(), destination.GetPositionY(),
            destination.GetPositionZ() + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight());
        return (status & MAP_LIQUID_STATUS_IN_CONTACT) != 0 &&
            (liquidData.type_flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0;
    }

    bool IsValidatedPathingHazardEgress(Player const* player, Position const& destination)
    {
        if (!player || IsHazardousLiquidDestination(player, destination))
            return false;

        float const dx = destination.GetPositionX() - player->GetPositionX();
        float const dy = destination.GetPositionY() - player->GetPositionY();
        float const planarDelta = std::sqrt(dx * dx + dy * dy);
        float const verticalDelta = std::fabs(destination.GetPositionZ() - player->GetPositionZ());

        // This is only an emergency egress for a bot whose liquid position is
        // not represented by the navmesh. Keep it short, reject floor changes,
        // and require collision LOS so it cannot become a wall/drop shortcut.
        if (planarDelta < 0.5f || planarDelta > 24.0f ||
            verticalDelta > std::max(8.0f, planarDelta * 0.75f + 2.0f))
            return false;

        return player->IsWithinLOS(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ() + 0.5f);
    }

    bool TryMoveOutOfHazardousLiquid(Player* player)
    {
        if (!player)
            return false;

        struct HazardEscapeState
        {
            Position lastSafePosition;
            bool hasLastSafePosition = false;
        };

        static std::unordered_map<uint64, HazardEscapeState> stateByGuid;
        HazardEscapeState& state = playerbot::LockedGetOrCreate(stateByGuid, player->GetGUID().GetRawValue());

        if (!IsInHazardousLiquid(player))
        {
            state.lastSafePosition = player->GetPosition();
            state.hasLastSafePosition = true;
            return false;
        }

        // A charge/leap already owns the highest-priority movement slot and is
        // moving the bot away from its current hazardous position. Do not wipe
        // that effect generator to install an escape point, or its arrival
        // MovementInform will never fire.
        if (HasPlayerbotGapCloserInFlight(player))
        {
            EmitRehgarMovementGuardServerDiagnostic(player, "hazard_escape_deferred_effect", 0);
            return true;
        }

        MotionMaster* motionMaster = player->GetMotionMaster();
        if (!motionMaster)
            return true;

        // Preserve an already-launched escape point. Recomputing from the bot's
        // new position and clearing/reissuing every AI tick repeatedly resets
        // the spline at its origin, which looks exactly like standing still.
        if (motionMaster->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        {
            bool const splineLaunched = player->movespline && player->movespline->Initialized();
            if (!splineLaunched || !player->movespline->Finalized())
                return true;
        }

        // A point route may cross hazardous ground when necessary. If it ends
        // while the damage aura remains, backtrack to the last observed safe
        // position instead of treating the hazardous endpoint as a place to
        // stop and cast.
        if (state.hasLastSafePosition && player->GetExactDist(state.lastSafePosition) > 1.0f)
        {
            Position segmentDestination;
            if (TryBuildBattlegroundSegmentDestination(player, state.lastSafePosition, segmentDestination) ||
                (!player->InBattleground() && player->GetDistance(state.lastSafePosition) > 0.5f))
            {
                motionMaster->Clear(MOTION_SLOT_ACTIVE);
                motionMaster->MovePoint(0, player->InBattleground() ? segmentDestination : state.lastSafePosition, true);
                return true;
            }

            if (IsValidatedPathingHazardEgress(player, state.lastSafePosition))
            {
                motionMaster->Clear(MOTION_SLOT_ACTIVE);
                motionMaster->MovePoint(0, state.lastSafePosition, true);
                return true;
            }
        }

        float const baseAngle = player->GetOrientation();
        std::array<float, 12> const probeAngles =
        {
            0.0f, float(M_PI), float(M_PI_2), -float(M_PI_2),
            float(M_PI_4), -float(M_PI_4), float(3.0f * M_PI_4), -float(3.0f * M_PI_4),
            float(M_PI / 6.0f), -float(M_PI / 6.0f), float(5.0f * M_PI / 6.0f), -float(5.0f * M_PI / 6.0f)
        };
        std::array<float, 4> const probeDistances = { 8.0f, 12.0f, 16.0f, 24.0f };

        for (float distance : probeDistances)
        {
            for (float offset : probeAngles)
            {
                Position destination = player->GetPosition();
                float const angle = baseAngle + offset;
                // World space by hand: angle already includes the facing, which
                // RelocateOffset would add a second time.
                destination.Relocate(player->GetPositionX() + std::cos(angle) * distance,
                    player->GetPositionY() + std::sin(angle) * distance, player->GetPositionZ());
                destination = BuildCollisionSafeDestination(player, destination);
                if (IsHazardousLiquidDestination(player, destination))
                    continue;

                Position segmentDestination;
                if (TryBuildBattlegroundSegmentDestination(player, destination, segmentDestination) ||
                    (!player->InBattleground() && player->GetDistance(destination) > 0.5f))
                {
                    motionMaster->Clear(MOTION_SLOT_ACTIVE);
                    motionMaster->MovePoint(0, player->InBattleground() ? segmentDestination : destination, true);
                    return true;
                }
            }
        }

        // A strict path can fail when the bot's current liquid polygon is not
        // present in the navmesh. In that case, permit only a short grounded,
        // collision-visible move to a non-hazardous bank. This is deliberately
        // a second pass so a fully navmeshed route always wins.
        for (float distance : probeDistances)
        {
            for (float offset : probeAngles)
            {
                Position destination = player->GetPosition();
                float const angle = baseAngle + offset;
                // World space by hand: angle already includes the facing, which
                // RelocateOffset would add a second time.
                destination.Relocate(player->GetPositionX() + std::cos(angle) * distance,
                    player->GetPositionY() + std::sin(angle) * distance, player->GetPositionZ());
                destination = BuildCollisionSafeDestination(player, destination);
                if (!IsValidatedPathingHazardEgress(player, destination))
                    continue;

                motionMaster->Clear(MOTION_SLOT_ACTIVE);
                motionMaster->MovePoint(0, destination, true);
                return true;
            }
        }

        // Being in magma/slime always retains movement ownership. Reporting
        // false here used to hand the tick back to class logic, allowing a bot
        // with no accepted probe to stand still and cast until it died.
        return true;
    }

    bool HasActiveTargetRelativeMovementFor(Player const* player, Unit const* target)
    {
        if (!player || !target)
            return false;

        MotionMaster const* motionMaster = player->GetMotionMaster();
        if (!motionMaster)
            return false;

        MovementGeneratorType const movementType = motionMaster->GetCurrentMovementGeneratorType();
        if (movementType != CHASE_MOTION_TYPE && movementType != FOLLOW_MOTION_TYPE)
            return false;

        MovementGenerator const* movement = motionMaster->GetCurrentMovementGenerator();
        AbstractFollower const* follower = movement ? dynamic_cast<AbstractFollower const*>(movement) : nullptr;
        if (!follower || follower->GetTarget() != target)
            return false;

        bool const activeSpline = player->movespline && player->movespline->Initialized() &&
            !player->movespline->Finalized();
        return activeSpline || player->isMoving() ||
            player->HasUnitState(UNIT_STATE_CHASE_MOVE | UNIT_STATE_FOLLOW_MOVE);
    }

    bool IssueHumanLikeFollow(Player* player, Unit* target, float desiredDistance, float destinationChangeThreshold, uint32 minReissueMs)
    {
        if (!player || !target)
            return false;

        return IssueMovePointThrottled(player, BuildFollowDestination(player, target, desiredDistance), destinationChangeThreshold, minReissueMs);
    }

    // Bring a bot that is well below the surface of water too deep to stand in
    // up into the swim band (0.5 under the level - the same band the swim
    // hysteresis and BuildCollisionSafeDestination use). A bot can find itself
    // on a pool bed by being put down there (Violet Hold's flooded cell) or by
    // sinking while stopped; a player in that spot would be treading water at
    // the surface, and that is also where the navmesh water polys are, so
    // route building has to start from there. Same UpdatePosition idiom as
    // the stationary swim resync: observers pick it up with the next spline.
    void SurfacePlayerbotInDeepWater(Player* player)
    {
        if (!player || !player->IsInWorld() || player->HasAuraType(SPELL_AURA_WATER_WALK))
            return;

        Map const* map = player->FindMap();
        if (!map)
            return;

        LiquidData liquidData{};
        ZLiquidStatus const status = map->GetLiquidStatus(player->GetPhaseMask(), player->GetPositionX(),
            player->GetPositionY(), player->GetPositionZ() + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight());
        if (!(status & MAP_LIQUID_STATUS_SWIMMING))
            return;

        bool const tooDeepToStand = (liquidData.level - liquidData.depth_level) > player->GetCollisionHeight();
        if (!tooDeepToStand)
            return;

        float const swimZ = liquidData.level - 0.5f;
        if (swimZ - player->GetPositionZ() < 1.0f)
            return;

        player->UpdatePosition(player->GetPositionX(), player->GetPositionY(), swimZ, player->GetOrientation());
        player->UpdatePositionData();
    }

    bool IssueMovePointThrottled(Player* player, Position const& destination, float destinationChangeThreshold, uint32 minReissueMs)
    {
        if (!player)
            return false;

        MotionMaster* motionMaster = player->GetMotionMaster();
        if (!motionMaster)
            return false;

        MovementGeneratorType const currentMovement = motionMaster->GetCurrentMovementGeneratorType();
        MovementGenerator const* currentGenerator = motionMaster->GetCurrentMovementGenerator();
        bool const activePointSpline = currentMovement == POINT_MOTION_TYPE &&
            player->movespline && player->movespline->Initialized() && !player->movespline->Finalized();
        bool const pointGeneratorOwnsOrder = currentMovement == POINT_MOTION_TYPE && currentGenerator &&
            !currentGenerator->HasFlag(MOVEMENTGENERATOR_FLAG_FINALIZED);
        bool const botCurrentlyMoving = player->isMoving() || activePointSpline ||
            player->HasUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE);

        minReissueMs = std::max<uint32>(minReissueMs, 500);

        struct MoveOrderState
        {
            Position lastDestination;
            uint32 lastIssueMs = 0;
        };

        static std::unordered_map<uint64, MoveOrderState> stateByGuid;
        MoveOrderState& state = playerbot::LockedGetOrCreate(stateByGuid, player->GetGUID().GetRawValue());
        uint32 const nowMs = GameTime::GetGameTimeMS();

        bool const destinationChanged = state.lastIssueMs == 0 ||
            state.lastDestination.GetExactDist(destination) >= destinationChangeThreshold;
        bool const canReissueByTime = state.lastIssueMs == 0 || nowMs >= state.lastIssueMs + minReissueMs;

        // The 50 ms tactical loop can revisit the same objective before a newly
        // queued PointMovementGenerator has initialized its spline. During that
        // window Player::isMoving(), unit movement state, and movespline can all
        // still report false even though MotionMaster already owns the order.
        // Reissuing here replaces the generator at the bot's newer authoritative
        // position; nearby clients render each replacement as a teleport/snap.
        // Preserve the existing point order until it naturally finalizes. This
        // also covers a point generator temporarily interrupted by a cast/root;
        // PointMovementGenerator will relaunch its own spline when movement is
        // available again.
        if (!destinationChanged && pointGeneratorOwnsOrder)
            return true;

        // Never bypass the requested reissue cadence merely because the player
        // movement flags have not caught up yet. The previous forced-stationary
        // path could enqueue a replacement every fast tactical tick, which was
        // especially visible when every OBC bot selected the fixed center flag.
        if (!destinationChanged && !canReissueByTime)
            return true;

        // Consume the shared movement-command throttle only when this helper has
        // actually decided that a new order may be required. Previously, calls
        // that merely preserved an active spline still consumed the token and
        // could starve a real destination change from another tactical action.
        if (!CanIssueMovementCommand(player, 500))
            return botCurrentlyMoving || pointGeneratorOwnsOrder || !destinationChanged;

        ClearEatDrinkAurasForMovement(player);

        if (player->InBattleground() && currentMovement == EFFECT_MOTION_TYPE && HasPlayerbotGapCloserInFlight(player))
        {
            EmitRehgarMovementGuardServerDiagnostic(player, "movepoint_blocked_effect");
            return true;
        }

        if (currentMovement == FOLLOW_MOTION_TYPE || currentMovement == DISTRACT_MOTION_TYPE)
        {
            motionMaster->Clear();
        }
        else if (!botCurrentlyMoving && player->InBattleground() &&
            currentMovement != IDLE_MOTION_TYPE &&
            currentMovement != CHASE_MOTION_TYPE &&
            currentMovement != POINT_MOTION_TYPE &&
            // Charge/Intercept/Rehgar's Fury movement is issued through effect
            // generators. Clear stale effect movement only once its spline has
            // actually ended; some jump-dest effects do not keep
            // UNIT_STATE_CHARGING set for the full visual travel.
            (currentMovement != EFFECT_MOTION_TYPE || !HasPlayerbotGapCloserInFlight(player)))
        {
            EmitBattlegroundGmDebug(player,
                "movepoint=clear-stale-generator motionType=" + std::to_string(uint32(currentMovement)), 1000);
            motionMaster->Clear();
        }

        bool const swimming = !player->IsFlying() && player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
        bool const generatePath = !player->IsFlying() && !swimming;
        Position const safeDestination = generatePath ? BuildCollisionSafeDestination(player, destination) : destination;

        if (swimming && player->InBattleground())
        {
            // A swimmer's raw MovePoint is a straight spline. From the bed of a
            // pool that line climbs through the basin wall and runs along under
            // the surrounding floor until it surfaces next to the target -
            // Violet Hold's flooded cell showed this as bots "teleporting under
            // the arena". Surface the bot first (a player would be treading
            // water there), then walk the same mmap segments the land movers
            // use: water polys are walkable for players and join the shore, so
            // a real route normally exists. A bot riding those polys at the
            // surface counts as swimming (Unit::ProcessTerrainStatusUpdate), so
            // this is every river and lake crossing, not only a bot below the
            // surface. A straight swim used to win whenever line of sight was
            // clear, but that test sees WMOs and gameobjects, not terrain: a
            // line to a target up the bank ran through the bank. Only when
            // Detour has nothing to offer does the direct spline go out.
            SurfacePlayerbotInDeepWater(player);

            Position segmentDestination;
            PathType pathType = PathType(0);
            if (TryBuildBattlegroundSegmentDestination(player, BuildCollisionSafeDestination(player, destination), segmentDestination, &pathType))
            {
                motionMaster->MovePoint(0, segmentDestination, true);
                EmitBattlegroundGmDebug(player,
                    "movepoint=swim-nav-segment pathType=" + std::to_string(uint32(pathType)) +
                    " segDist=" + std::to_string(int32(player->GetDistance(segmentDestination))), 0);
            }
            else
            {
                motionMaster->MovePoint(0, safeDestination, false);
                EmitBattlegroundGmDebug(player,
                    std::string("movepoint=swim-direct reason=no-nav") +
                    " destDist=" + std::to_string(int32(player->GetDistance(safeDestination))), 0);
            }
        }
        else if (generatePath && player->InBattleground())
        {
            Position segmentDestination;
            PathType pathType = PathType(0);
            if (!TryBuildBattlegroundSegmentDestination(player, safeDestination, segmentDestination, &pathType))
            {
                // Never replace a failed navmesh route with a direct spline.
                // On multi-level battleground geometry, a height sample may
                // belong to a disconnected room below the bot, and an unpathed
                // MovePoint can cross both the intervening floor and walls.
                // Preserve any movement already in progress and retry path
                // generation on a later decision tick.
                bool const stillMoving = player->isMoving();
                EmitBattlegroundGmDebug(player,
                    "movepoint=blocked-no-nav fallback=none preserveMoving=" +
                    std::to_string(stillMoving ? 1 : 0) +
                    " destDist=" + std::to_string(int32(player->GetDistance(safeDestination))), 0);
                return stillMoving;
            }

            motionMaster->MovePoint(0, segmentDestination, true);
            EmitBattlegroundGmDebug(player,
                "movepoint=nav-segment pathType=" + std::to_string(uint32(pathType)) +
                " segDist=" + std::to_string(int32(player->GetDistance(segmentDestination))), 0);
        }
        else
        {
            motionMaster->MovePoint(0, safeDestination, generatePath);
        }

        // Throttle against the caller's tactical destination, not the intermediate
        // navmesh segment we just issued. Battleground segment walking intentionally
        // advances in chunks; storing the chunk endpoint here made the next tick see
        // the real objective as a different destination, clear the active spline,
        // and reinstall a fresh segment before bots could make visible progress.
        state.lastDestination = destination;
        state.lastIssueMs = nowMs;
        return true;
    }

    // `forced` is a GM saying so out loud (.playerbot pvp forcequeue), and is the
    // one thing that puts a persistent bot in a queue on a realm that has retired
    // them from instanced PvP.
    bool QueuePlayer(Player* player, BattlegroundTypeId bgTypeId, uint8 arenaType, bool forced = false)
    {
        if (!player || player->InBattleground())
            return false;

        // The one door every automatic queue path comes through, which is why the
        // switch is enforced here rather than at each of them. The per-bot
        // lifecycle already honoured it; the population's own SCM sweep called the
        // lifecycle actions directly and walked straight past it, so a fleet
        // configured never to queue made 227,000 queue additions in twenty minutes
        // the last time a person waited in a battleground queue.
        if (!forced && !MayQueueForInstancedPvp(player))
            return false;

        // A dead bot queues as it lies, and nothing below needs a living body:
        // BattlegroundQueue::AddGroup takes a ghost the same way it takes a
        // person who queued while running back, and the port that follows an
        // invite stands the bot up itself (HandleBattleFieldPortOpcode
        // resurrects whoever it ports, at the battleground's own door).
        //
        // Standing it up HERE was the "bot instantly respawning on his corpse"
        // report. While a human shows battleground interest this sweep runs
        // over every managed bot many times a second, so a bot that died out in
        // the world was back on its feet at full health, on the spot where it
        // fell, inside a second - and then walked into whatever had just killed
        // it and did it again, over and over, in front of whoever killed it.
        //
        // Death in the open world belongs to the PvE manager's recovery:
        // release, graveyard, resurrect, the way a person's corpse run looks.
        // Nothing is skipped by leaving the body where it is.
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate)
            return false;

        // Recover stale local queue slot state before evaluating queue eligibility.
        // Managed bots can occasionally keep orphaned queue ids after lifecycle
        // transitions, which blocks HasFreeBattlegroundQueueId() and prevents
        // requeueing after subsequent battlegrounds.
        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const existingQueueTypeId = player->GetBattlegroundQueueTypeId(i);
            if (existingQueueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            BattlegroundQueue& existingQueue = sBattlegroundMgr->GetBattlegroundQueue(existingQueueTypeId);
            GroupQueueInfo ginfo{};
            if (existingQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
                continue;

            player->RemoveBattlegroundQueueId(existingQueueTypeId);
            EmitLifecycleDiagnostic(player, "queue-prune-stale-slot",
                "Removed stale queueTypeId=" + std::to_string(uint32(existingQueueTypeId)));
        }

        // SCM is intentionally prioritized over other BG queues for managed bots:
        // free any existing queue slots first so SCM can always enqueue.
        if (bgTypeId == BATTLEGROUND_SCM)
        {
            RemoveMatchingQueues(player, false, false, true);
            RemoveMatchingQueues(player, true, false, false);
            player->SetArenaTeamIdInvited(0);
        }

        // Managed random bots can run on disconnected virtual sessions where RBAC
        // battleground permissions are not always populated like live client sessions.
        // Gate queue eligibility by battleground level + free queue slots instead.
        if (player->IsInCustomGameLobby() || !player->GetBGAccessByLevel(bgTypeId) || !player->HasFreeBattlegroundQueueId())
            return false;

        BattlegroundQueueTypeId const bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, arenaType);
        if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            return false;

        if (player->GetBattlegroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
        {
            if (bgTypeId == BATTLEGROUND_SCM)
            {
                if (PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel()))
                {
                    sBattlegroundMgr->ScheduleQueueUpdate(0, arenaType, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());
                    EmitLifecycleDiagnostic(player, "queue-refresh-existing",
                        "Already queued for SCM; forced queue update refresh.");
                    return true;
                }
            }

            return false;
        }

        PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
        if (!bracketEntry)
            return false;

        // Playerbots should never request a fixed Scarlet Chapel side. Only the custom queue NPC
        // should set ALLIANCE/HORDE explicitly. Clear any stale raw override before AddGroup so
        // BattlegroundQueue can assign a synthetic balanced side.
        if (bgTypeId == BATTLEGROUND_SCM)
            player->SetBGTeam(0);

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        GroupQueueInfo* ginfo = bgQueue.AddGroup(player, nullptr, bgTypeId, bracketEntry, arenaType, false, false, 0, 0);
        if (!ginfo)
        {
            EmitLifecycleDiagnostic(player, "queue-add-failed", "BattlegroundQueue::AddGroup returned null.");
            return false;
        }

        player->AddBattlegroundQueueId(bgQueueTypeId);
        sBattlegroundMgr->ScheduleQueueUpdate(ginfo->ArenaMatchmakerRating, ginfo->ArenaType, bgQueueTypeId, bgTypeId,
            bracketEntry->GetBracketId());
        EmitLifecycleDiagnostic(player, "queue-add-success",
            "Queued for bgTypeId=" + std::to_string(uint32(bgTypeId)) + " queueTypeId=" + std::to_string(uint32(bgQueueTypeId)));
        return true;
    }

    bool RemovePlayerFromQueue(Player* player, BattlegroundQueueTypeId bgQueueTypeId, bool scheduleNonArenaUpdate)
    {
        if (!player || bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            return false;

        if (player->GetBattlegroundQueueIndex(bgQueueTypeId) >= PLAYER_MAX_BATTLEGROUND_QUEUES)
            return false;

        BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);

        GroupQueueInfo ginfo{};
        bool const haveGroupInfo = bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo);

        // Always clear the player's local queue slot if it is still present.
        player->RemoveBattlegroundQueueId(bgQueueTypeId);

        if (haveGroupInfo)
            bgQueue.RemovePlayer(player->GetGUID(), true);

        if (scheduleNonArenaUpdate && haveGroupInfo && !ginfo.ArenaType && bgTemplate)
        {
            if (PvPDifficultyEntry const* bracketEntry =
                GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel()))
            {
                sBattlegroundMgr->ScheduleQueueUpdate(
                    ginfo.ArenaMatchmakerRating,
                    ginfo.ArenaType,
                    bgQueueTypeId,
                    bgTypeId,
                    bracketEntry->GetBracketId());
            }
        }

        return true;
    }
    bool RemoveMatchingQueues(Player* player, bool arenaOnly, bool invitedOnly, bool scheduleNonArenaUpdate)
    {
        if (!player || !player->InBattlegroundQueue())
            return false;

        bool removed = false;
        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
            if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            bool const isArenaQueue = BattlegroundMgr::BGArenaType(bgQueueTypeId) != 0;
            if (arenaOnly != isArenaQueue)
                continue;

            if (invitedOnly && !player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
                continue;

            removed = RemovePlayerFromQueue(player, bgQueueTypeId, scheduleNonArenaUpdate) || removed;
        }

        return removed;
    }

    bool AcceptMatchingInvite(Player* player, bool arenaInvite)
    {
        if (!player || player->InBattleground())
            return false;

        // Retired from instanced PvP means retired from the invites too. A stale
        // invite left over from before the switch was thrown is declined by the
        // ordinary expiry rather than accepted here.
        if (!MayQueueForInstancedPvp(player))
            return false;

        // Being dead is no reason to turn an invite down, and no reason to
        // resurrect here either: the HandleBattleFieldPortOpcode call below
        // stands up whoever it ports, on arrival, inside the battleground.
        // Doing it at this point would raise the bot in the open world first -
        // on its corpse, in front of anyone standing over it. See QueuePlayer.
        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
            if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            bool const isArenaQueue = BattlegroundMgr::BGArenaType(bgQueueTypeId) != 0;
            if (arenaInvite != isArenaQueue)
                continue;

            if (!player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
                continue;

            BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
            uint8 const arenaType = BattlegroundMgr::BGArenaType(bgQueueTypeId);
            if ((arenaType != 0) != arenaInvite)
                continue;

            BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
            GroupQueueInfo ginfo;
            if (!bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
            {
                EmitLifecycleDiagnostic(player, "invite-missing-group-info",
                    "No GroupQueueInfo for queueTypeId=" + std::to_string(uint32(bgQueueTypeId)));
                continue;
            }

            BattlegroundTypeId packetBgTypeId = bgTypeId;
            if (arenaType != 0)
            {
                // Arena invites can target dynamic map-specific arena templates rather
                // than BATTLEGROUND_AA; resolve the invited instance type first.
                Battleground* invited = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, BATTLEGROUND_TYPE_NONE);
                if (!invited)
                {
                    EmitLifecycleDiagnostic(player, "invite-arena-instance-missing",
                        "No invited arena instance for guid=" + std::to_string(ginfo.IsInvitedToBGInstanceGUID));
                    continue;
                }

                packetBgTypeId = invited->GetTypeID();
            }

            WorldSession* session = player->GetSession();
            if (!session)
            {
                EmitLifecycleDiagnostic(player, "invite-no-session", "WorldSession is null.");
                continue;
            }

            // Execute invite acceptance directly. Managed random bots can run on
            // disconnected virtual sessions where queued outbound packets are not
            // guaranteed to be pumped like real client traffic.
            ClearMovementBeforeBattlegroundTeleport(player);

            WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
            packet << arenaType << uint8(0) << uint32(packetBgTypeId) << uint16(0x1F90) << uint8(1);
            session->HandleBattleFieldPortOpcode(packet);

            if (player->IsBeingTeleportedFar())
            {
                EmitLifecycleDiagnostic(player, "invite-accept-far-teleport-pending",
                    "Issuing server-side HandleMoveWorldportAck for bot teleport finalization.");
                ClearMovementBeforeBattlegroundTeleport(player);
                session->HandleMoveWorldportAck();
            }

            if (!player->InBattleground() && !player->IsBeingTeleported())
            {
                EmitLifecycleDiagnostic(player, "invite-accept-no-transition",
                    "HandleBattleFieldPortOpcode did not transition to battleground/teleport.");
            }
            else
            {
                EmitLifecycleDiagnostic(player, "invite-accept-transition",
                    "Accepted invite for bgTypeId=" + std::to_string(uint32(packetBgTypeId)));
            }
            return true;
        }

        return false;
    }

    bool HasConflictingBattlegroundLifecycleContext(playerbot::BattlegroundLifecycleContext const& context)
    {
        return (context.queueOperation != playerbot::QueueOperationType::None) &&
            (context.invitationResponse != playerbot::InvitationResponseType::None);
    }

    bool HasPendingBattlegroundInvite(Player const* player)
    {
        if (!player)
            return false;

        for (uint32 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
            if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            if (player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
                return true;
        }

        return false;
    }

    bool ShouldRefreshLongQueuedBot(Player* player)
    {
        if (!player)
            return false;

        uint64 const guidRaw = player->GetGUID().GetRawValue();
        if (player->InBattleground() || !player->InBattlegroundQueue() || HasPendingBattlegroundInvite(player))
        {
            playerbot::LockedErase(g_BattlegroundQueuedNoInviteSinceMsByGuid, guidRaw);
            return false;
        }

        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32& queuedSinceMs = playerbot::LockedGetOrCreate(g_BattlegroundQueuedNoInviteSinceMsByGuid, guidRaw);
        if (!queuedSinceMs)
        {
            queuedSinceMs = nowMs;
            return false;
        }

        if (nowMs < queuedSinceMs + PLAYERBOT_BG_QUEUE_REQUEUE_TIMEOUT_MS)
            return false;

        queuedSinceMs = nowMs;
        return true;
    }

    bool HandleBattlegroundDeathState(Player* player)
    {
        static std::unordered_map<uint64, uint32> queuedSinceMsByGuid;
        static std::unordered_map<uint64, uint32> deadSinceMsByGuid;
        auto resolveSpiritGuide = [](Player* candidate, uint32 preferredEntry) -> Creature*
        {
            if (!candidate)
                return nullptr;

            // Keep spirit-healer lookup local to the graveyard.
            // Some virtual-session bots can temporarily report a neutral BG team;
            // in that case we still search both faction spirit-guide entries.
            float constexpr spiritGuideSearchRadius = 30.0f;

            auto findGuideByEntry = [candidate, spiritGuideSearchRadius](uint32 entry) -> Creature*
            {
                if (!entry)
                    return nullptr;

                std::list<Creature*> guides;
                candidate->GetCreatureListWithEntryInGrid(guides, entry, spiritGuideSearchRadius);

                Creature* nearestGuide = nullptr;
                float nearestDistanceSq = std::numeric_limits<float>::max();
                for (Creature* guide : guides)
                {
                    if (!guide || !guide->IsSpiritService())
                        continue;

                    float const distanceSq = candidate->GetExactDist2dSq(guide);
                    if (distanceSq < nearestDistanceSq)
                    {
                        nearestGuide = guide;
                        nearestDistanceSq = distanceSq;
                    }
                }

                return nearestGuide;
            };

            Creature* spiritGuide = findGuideByEntry(preferredEntry);
            if (!spiritGuide)
            {
                spiritGuide = findGuideByEntry(BG_CREATURE_ENTRY_A_SPIRITGUIDE);
                if (!spiritGuide)
                    spiritGuide = findGuideByEntry(BG_CREATURE_ENTRY_H_SPIRITGUIDE);
            }

            return spiritGuide;
        };

        if (!player)
            return false;

        uint64 const playerGuidRaw = player->GetGUID().GetRawValue();
        if (!player->InBattleground())
        {
            playerbot::LockedErase(g_BattlegroundDeadBotGuids, playerGuidRaw);
            playerbot::LockedErase(g_BattlegroundSpiritQueuedBotGuids, playerGuidRaw);
            playerbot::LockedErase(deadSinceMsByGuid, playerGuidRaw);
            return false;
        }

        if (player->IsAlive())
        {
            bool wasDead = false;
            bool wasSpiritQueued = false;
            {
                std::lock_guard<std::mutex> stateGuard(playerbot::SharedBotStateStructureLock());
                queuedSinceMsByGuid.erase(playerGuidRaw);
                deadSinceMsByGuid.erase(playerGuidRaw);
                wasDead = g_BattlegroundDeadBotGuids.erase(playerGuidRaw) != 0;
                wasSpiritQueued = g_BattlegroundSpiritQueuedBotGuids.erase(playerGuidRaw) != 0;
            }
            if (wasDead && wasSpiritQueued && player->GetMapId() == 726)
            {
                // Twin Peaks' middle graveyards resurrect playerbots on cliff
                // geometry that is disconnected from the useful downhill
                // navmesh. Relocate only bots revived by the spirit queue, and
                // use the explicit team exits supplied for this battleground.
                TeamId const team = ResolveBotTeamId(player);
                if (team == TEAM_NEUTRAL)
                    return false;

                Position const downhillExit = team == TEAM_HORDE ?
                    Position(1827.342041f, 191.699814f, -19.907791f, 4.655410f) :
                    Position(1874.352905f, 422.770020f, -17.006735f, 4.763416f);
                if (MotionMaster* motionMaster = player->GetMotionMaster())
                    motionMaster->Clear();
                ForcePlayerbotDismount(player);
                player->NearTeleportTo(downhillExit);
                TC_LOG_INFO("playerbots.pvp.lifecycle",
                    "Playerbot Twin Peaks graveyard exit relocation: guid={} team={} destination=({},{},{}).",
                    player->GetGUID().ToString(), uint32(team), downhillExit.GetPositionX(), downhillExit.GetPositionY(),
                    downhillExit.GetPositionZ());
                return true;
            }
            return false;
        }

        playerbot::LockedInsert(g_BattlegroundDeadBotGuids, playerGuidRaw);

        if (!player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        {
            uint32 const nowMs = GameTime::GetGameTimeMS();
            uint32& deadSinceMs = playerbot::LockedGetOrCreate(deadSinceMsByGuid, playerGuidRaw);
            if (!deadSinceMs)
                deadSinceMs = nowMs;

            // Give human-like reaction time before auto-releasing spirit so bots
            // don't insta-release the instant they die in BGs/arenas.
            if (nowMs < deadSinceMs + PLAYERBOT_BG_RELEASE_SPIRIT_DELAY_MS)
                return true;

            playerbot::LockedErase(deadSinceMsByGuid, playerGuidRaw);
            player->BuildPlayerRepop();
            player->RepopAtGraveyard();
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP death handling: guid={} action=release-spirit.",
                player->GetGUID().ToString());
            return true;
        }

        Battleground* battleground = player->GetBattleground();
        if (!battleground)
            return true;

        if (battleground->IsPlayerInResurrectQueue(player->GetGUID()))
        {
            playerbot::LockedInsert(g_BattlegroundSpiritQueuedBotGuids, playerGuidRaw);
            uint32 const nowMs = GameTime::GetGameTimeMS();
            uint32& queuedSinceMs = playerbot::LockedGetOrCreate(queuedSinceMsByGuid, playerGuidRaw);
            if (!queuedSinceMs)
                queuedSinceMs = nowMs;

            // Fallback recovery: if a bot remains ghosted in the resurrect queue for too long,
            // refresh its spirit-guide queue registration so it revives on normal BG wave timing.
            if (nowMs >= queuedSinceMs + 12000)
            {
                battleground->RemovePlayerFromResurrectQueue(player->GetGUID());

                uint32 const spiritEntry = ResolveBotTeamId(player) == TEAM_ALLIANCE ? BG_CREATURE_ENTRY_A_SPIRITGUIDE : BG_CREATURE_ENTRY_H_SPIRITGUIDE;
                Creature* spiritGuide = resolveSpiritGuide(player, spiritEntry);

                if (spiritGuide)
                {
                    battleground->AddPlayerToResurrectQueue(spiritGuide->GetGUID(), player->GetGUID());
                    sBattlegroundMgr->SendAreaSpiritHealerQueryOpcode(player, battleground, spiritGuide->GetGUID());
                    queuedSinceMs = nowMs;
                    TC_LOG_WARN("playerbots.pvp.lifecycle",
                        "Playerbot PvP death handling fallback spirit queue refresh: guid={} spiritGuide={}.",
                        player->GetGUID().ToString(), spiritGuide->GetGUID().ToString());
                }
                else
                {
                    queuedSinceMs = nowMs;
                }
            }
            return true;
        }

        uint32 const spiritEntry = ResolveBotTeamId(player) == TEAM_ALLIANCE ? BG_CREATURE_ENTRY_A_SPIRITGUIDE : BG_CREATURE_ENTRY_H_SPIRITGUIDE;

        // Mirror player core BG death handling behavior: once ghosted at a battleground
        // graveyard, register at a nearby spirit guide and wait for the periodic wave rez.
        // Avoid script-driven ghost movement because missed/path-blocked moves can prevent
        // ever getting queued for resurrection.
        Creature* spiritGuide = resolveSpiritGuide(player, spiritEntry);
        if (!spiritGuide)
            return true;

        battleground->AddPlayerToResurrectQueue(spiritGuide->GetGUID(), player->GetGUID());
        playerbot::LockedInsert(g_BattlegroundSpiritQueuedBotGuids, playerGuidRaw);
        playerbot::LockedSet(queuedSinceMsByGuid, player->GetGUID().GetRawValue(), GameTime::GetGameTimeMS());
        sBattlegroundMgr->SendAreaSpiritHealerQueryOpcode(player, battleground, spiritGuide->GetGUID());
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP death handling: guid={} action=queue-resurrect spiritGuide={}.",
            player->GetGUID().ToString(), spiritGuide->GetGUID().ToString());
        return true;
    }

    bool HasConflictingArenaLifecycleContext(playerbot::ArenaLifecycleContext const& context)
    {
        return (context.queueOperation != playerbot::QueueOperationType::None) &&
            (context.teamInteraction != playerbot::ArenaTeamInteractionType::None);
    }

    bool IsTacticalAction(char const* actionName, char const* expected)
    {
        return actionName && expected && std::strcmp(actionName, expected) == 0;
    }

    void StopVirtualPlayerbotMovement(Player* player);

    bool TryAcquireHealthstoneFromSoulwell(Player* player)
    {
        if (!player || player->IsInCombat() || playerbot::PvpCore::HasHealthstone(player))
            return false;

        GameObject* soulwell = playerbot::PvpCore::FindUsableSoulwell(player, 60.0f);
        if (!soulwell)
            return false;

        if (!soulwell->IsAtInteractDistance(player))
            return IssueMovePointThrottled(player, soulwell->GetPosition(), 4.0f, 500) || player->isMoving();

        Player* warlockOwner = soulwell->GetOwner() ? soulwell->GetOwner()->ToPlayer() : nullptr;
        if (!warlockOwner)
            return false;

        // The level-60 Soulwell creates Major Healthstones. Select the exact
        // create-item spell from the summoning warlock's Improved Healthstone
        // rank so the resulting 9421/19012/19013 item matches normal gameplay.
        uint32 createHealthstoneSpellId = 23819;
        if (warlockOwner->HasSpell(18693) || warlockOwner->HasAura(18693))
            createHealthstoneSpellId = 23821;
        else if (warlockOwner->HasSpell(18692) || warlockOwner->HasAura(18692))
            createHealthstoneSpellId = 23820;

        // No dismount here on purpose. This cast passes `true`, which is
        // TRIGGERED_FULL_MASK, and that mask contains
        // TRIGGERED_IGNORE_CASTER_MOUNTED_OR_ON_VEHICLE - Spell::CheckCast wraps
        // its ENTIRE mounted refusal in a test for that flag, so IsMounted() is
        // never even evaluated for this one. The mount was never in the way.
        //
        // Taking it off was a net loss. The helper only runs out of combat, which
        // is exactly when a bot is mounted, so this fired on essentially every
        // soulwell pickup and forced a remount afterwards. And the complete
        // dismount strips the Plainsrunning run-mount too, which the core
        // deliberately defers rather than refuses - making this site MORE
        // aggressive than the general pre-cast rule it looked like a special case
        // of.
        StopVirtualPlayerbotMovement(player);
        SpellCastResult const castResult = player->CastSpell(player, createHealthstoneSpellId, true);
        if (castResult != SPELL_CAST_OK || !playerbot::PvpCore::HasHealthstone(player))
            return false;

        soulwell->AddUse();
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot acquired healthstone from Soulwell: guid={} owner={} createSpell={} uses={}",
            player->GetGUID().ToString(), warlockOwner->GetGUID().ToString(), createHealthstoneSpellId,
            soulwell->GetUseCount());
        return true;
    }

    void ClearActiveMovementForControlLoss(Player* player)
    {
        if (!player)
            return;

        // Taunt pauses the tick like any other CC, but it must not tear down its
        // own effect. Unit::SetTaunted has already installed MoveChase(caster) +
        // Attack(caster) (Unit.cpp:12990); AttackStop/SetSelection/Clear below
        // would erase it every tick and leave the bot standing still. Real
        // players never reach this function, which is why taunt works on them and
        // not on bots.
        if (player->IsTaunted())
            return;

        player->AttackStop();
        player->SetSelection(ObjectGuid::Empty);
        // Preserve server-side confused wander (e.g. polymorph/sheep) and the
        // flee a fear installs. Clearing the active movement slot repeatedly can
        // freeze the expected drifting - and for a fear it does far worse, since
        // finalizing the fleeing generator strips UNIT_FLAG_FLEEING and
        // UNIT_STATE_FLEEING while the aura stays, which hands the bot its
        // autoattacks and its casts back mid-fear. The long version of that is
        // in PlayerbotRandomBotParticipation.cpp's copy of this function.
        if (player->HasUnitState(UNIT_STATE_CONFUSED) || player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
            player->IsPolymorphed() ||
            player->HasUnitState(UNIT_STATE_FLEEING) || player->HasAuraType(SPELL_AURA_MOD_FEAR))
            return;

        if (MotionMaster* motionMaster = player->GetMotionMaster())
            motionMaster->Clear(MOTION_SLOT_ACTIVE);
    }

    // Volley, Rain of Fire, Blizzard, and Tranquility must be treated like Mind
    // Flay/Drain Life for playerbot movement even on DBC/custom data where the
    // generic channel flags look movable - none of these should have the bot
    // walking (and cancelling the channel) mid-cast.
    bool IsForcedStationaryChannelSpell(SpellInfo const* spellInfo)
    {
        if (!spellInfo)
            return false;

        SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
        if (!firstRank)
            return false;

        switch (firstRank->Id)
        {
            case 15407: // Mind Flay
            case 1510:  // Volley
            case 5740:  // Rain of Fire
            case 10:    // Blizzard
            case 740:   // Tranquility
                return true;
            default:
                return false;
        }
    }

    bool HasActiveStationaryChannel(Player const* player)
    {
        if (!player)
            return false;

        Spell const* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!channel || channel->getState() == SPELL_STATE_FINISHED)
            return false;

        SpellInfo const* spellInfo = channel->GetSpellInfo();
        return spellInfo && spellInfo->IsChanneled() && (!spellInfo->IsMoveAllowedChannel() || IsForcedStationaryChannelSpell(spellInfo));
    }

    bool HasPlayerbotGapCloserInFlight(Player const* player)
    {
        if (!player)
            return false;

        MotionMaster const* motionMaster = player->GetMotionMaster();
        if (!motionMaster || motionMaster->GetCurrentMovementGeneratorType() != EFFECT_MOTION_TYPE)
            return false;

        // MotionMaster installs an effect generator before its MoveSpline is
        // initialized. The lifecycle loop runs in that window and used to see
        // "not in flight", then replace the queued jump with Attack/MoveChase.
        // Protect the generator from the moment it becomes current through the
        // end of its spline. This is generic for every effect-driven gap closer.
        bool const splineLaunched = player->movespline && player->movespline->Initialized();
        bool const splineActive = splineLaunched && !player->movespline->Finalized();
        bool const splinePendingLaunch = !splineLaunched;
        return splineActive || splinePendingLaunch ||
            player->HasUnitState(UNIT_STATE_CHARGING | UNIT_STATE_JUMPING) || player->isMoving();
    }

    bool CanIssueBotMovement(Player* player)
    {
        if (!player || !player->IsAlive() || player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            return false;

        if (HasActiveStationaryChannel(player))
            return false;

        if (playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotShadowmeldGraceToken))
        {
            StopVirtualPlayerbotMovement(player);
            return false;
        }

        if (player->GetClass() == CLASS_HUNTER && HunterIsHardCastingStationaryShot(player))
        {
            WhisperHunterAimedLifecycleDiagnostic(player, nullptr, "movement_blocked_active_cast", nullptr, 250);
            return false;
        }

        BreakExpiredHunterFeignDeath(player);

        if (IsCrowdControlledForAction(player))
        {
            ClearActiveMovementForControlLoss(player);
            return false;
        }

        if (player->HasUnitState(UNIT_STATE_STUNNED) ||
            player->HasUnitState(UNIT_STATE_CONFUSED) ||
            player->HasUnitState(UNIT_STATE_FLEEING))
        {
            return false;
        }

        // Charge/Intercept/Heroic Leap travel through EFFECT_MOTION_TYPE. If
        // combat-positioning logic issues MoveChase/MoveFollow while that spline
        // is still in flight, it cancels the leap and replaces it with a normal
        // chase - the bot's stun/damage effect still lands (that resolves at
        // cast, independent of movement), but the bot never visibly performs the
        // charge motion and just appears to run at the target. Withhold
        // positioning movement until the charge spline itself completes.
        if (HasPlayerbotGapCloserInFlight(player))
        {
            EmitRehgarMovementGuardServerDiagnostic(player, "movement_blocked_effect");
            return false;
        }

        // Only roots forbid locomotion. Snares still permit pursuit and
        // retreat; the class executor independently plants for cast-time
        // spells and preserves movement for instant spells.
        if (playerbot::PvpCore::IsMovementPreventedByRoot(player))
        {
            if (MotionMaster* motionMaster = player->GetMotionMaster())
                motionMaster->Clear(MOTION_SLOT_ACTIVE);
            StopVirtualPlayerbotMovement(player);
            return false;
        }

        return true;
    }

    float GetAggressiveCombatScanDistance(Player const* player, float fallbackDistance)
    {
        if (!player)
            return fallbackDistance;

        return std::max(fallbackDistance, player->GetVisibilityRange());
    }

    bool IsMeleePressureTarget(Unit const* unit)
    {
        Player const* player = unit ? unit->ToPlayer() : nullptr;
        if (!player)
            return false;

        switch (player->GetClass())
        {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
            return true;
        case CLASS_SHAMAN:
        case CLASS_PALADIN:
        {
            Item const* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
            ItemTemplate const* mainHandTemplate = mainHand ? mainHand->GetTemplate() : nullptr;
            return mainHandTemplate && mainHandTemplate->InventoryType == INVTYPE_2HWEAPON;
        }
        default:
            return false;
        }
    }

    bool IsActivelyPressuringInMelee(Unit const* attacker, Player const* bot)
    {
        if (!attacker || !bot || !attacker->IsAlive() || !bot->IsAlive())
            return false;

        if (!IsMeleePressureTarget(attacker))
            return false;

        // Only kite if the melee-capable target is actually threatening this bot.
        // Otherwise, ranged casters (e.g. frost mages) should hold position and
        // continue turret casting from their current firing band.
        if (attacker->GetVictim() == bot)
            return true;

        return attacker->IsWithinMeleeRange(bot) || bot->IsWithinMeleeRange(attacker);
    }

    bool ShouldForceMeleeFallbackOnLowMana(Player const* player)
    {
        if (!player || player->GetPowerType() != POWER_MANA)
            return false;

        if (player->GetClass() != CLASS_SHAMAN && player->GetClass() != CLASS_PALADIN)
            return false;

        return player->GetPowerPct(POWER_MANA) <= 10.0f || player->GetPower(POWER_MANA) < 250;
    }

    bool HasHunterKiteControl(Unit const* target)
    {
        if (!target)
            return false;

        return target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
            target->HasAuraWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_STUN));
    }

    void StopVirtualPlayerbotMovement(Player* player)
    {
        if (!player)
            return;

        if (TryMoveOutOfHazardousLiquid(player))
            return;

        player->StopMoving();
        // Both persistent managed bots (virtual session) and OBC/custom-game
        // clone mirrors (transient session) are socketless and need the same
        // synthetic movement-flag clear here -- a real client would send its
        // own stop packet, but neither of these has one. Checking only
        // IsVirtualSession() left clones with a stale MOVEMENTFLAG_MASK_MOVING
        // bit after every stop, which made the engine treat them as still
        // moving on the very next Auto Shot cast attempt and clip it.
        WorldSession* session = player->GetSession();
        bool const syntheticPlayerbotMover =
            playerbot::IsManagedRandomBot(player) ||
            playerbot::PlayerbotObcCloneManager::IsActiveClone(player) ||
            (session && (session->IsVirtualSession() || session->IsTransientPlayerSession()));
        if (syntheticPlayerbotMover)
        {
            player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
            // Throttled/shared swim-state resync; see
            // ResyncPlayerbotSwimStateForMovementStop for the full rationale.
            playerbot::ResyncPlayerbotSwimStateForMovementStop(player);
        }
    }

    // StopMoving() only terminates the current spline. A POINT/CHASE/FOLLOW
    // generator remains installed and can launch a new spline on the next
    // MotionMaster update, which is fatal to Auto Shot's final 434ms stationary
    // release window. Hunter planting/settling therefore needs to relinquish
    // the active ordinary movement generator as well as clear movement flags.
    // Effect movement (charges/leaps), knockbacks, and other high-priority
    // generators are deliberately left alone.
    void StopHunterTargetMovementForPlant(Player* player)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return;

        MotionMaster* motionMaster = player->GetMotionMaster();
        if (motionMaster)
        {
            switch (motionMaster->GetCurrentMovementGeneratorType())
            {
                case POINT_MOTION_TYPE:
                case CHASE_MOTION_TYPE:
                case FOLLOW_MOTION_TYPE:
                    motionMaster->Clear(MOTION_SLOT_ACTIVE);
                    break;
                default:
                    break;
            }
        }

        StopVirtualPlayerbotMovement(player);
    }

    bool IsHunterAutoShotPlantActiveInternal(Player* player, uint32 nowMs = GameTime::GetGameTimeMS())
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return false;

        uint64 const guidRaw = player->GetGUID().GetRawValue();
        uint32 plantUntilMs = 0;
        uint32 plantFireSequence = 0;
        bool hasPlantSequence = false;
        {
            std::lock_guard<std::mutex> stateGuard(playerbot::SharedBotStateStructureLock());
            auto plantUntilItr = g_HunterAutoShotPauseUntilMs.find(guidRaw);
            if (plantUntilItr == g_HunterAutoShotPauseUntilMs.end())
                return false;
            plantUntilMs = plantUntilItr->second;

            auto plantSequenceItr = g_HunterAutoShotPlantFireSequence.find(guidRaw);
            hasPlantSequence = plantSequenceItr != g_HunterAutoShotPlantFireSequence.end();
            if (hasPlantSequence)
                plantFireSequence = plantSequenceItr->second;
        }

        auto clearPlantState = [&]()
        {
            std::lock_guard<std::mutex> stateGuard(playerbot::SharedBotStateStructureLock());
            g_HunterAutoShotPauseUntilMs.erase(guidRaw);
            g_HunterAutoShotPlantFireSequence.erase(guidRaw);
        };

        // The triggered Auto Shot event occurs during Unit::Update, before the
        // playerbot tactical/class passes. Release the cross-module lock on
        // that very next pass rather than freezing until the safety deadline.
        if (hasPlantSequence && GetHunterAutoShotFireSequence(player) != plantFireSequence)
        {
            clearPlantState();
            return false;
        }

        if (plantUntilMs <= nowMs)
        {
            clearPlantState();
            return false;
        }

        // Do not hold a blind 2.2-second freeze if the target moved out of the
        // legal firing band during the 434ms release window. Clear the plant
        // immediately so the same tactical pass can resume ideal-range movement.
        Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        Unit* target = autoRepeat ? autoRepeat->m_targets.GetUnitTarget() : nullptr;
        playerbot::HunterAutoShotRangeInfo rangeInfo;
        bool const legalTarget = autoRepeat && autoRepeat->GetSpellInfo() && autoRepeat->GetSpellInfo()->Id == 75 &&
            target && target->IsAlive() && !playerbot::PvpCore::HasBreakableCrowdControlFor(player, target) &&
            playerbot::PvpCore::GetHunterAutoShotRange(player, target, rangeInfo) &&
            player->IsWithinLOSInMap(target) &&
            rangeInfo.exactDistance > rangeInfo.minRange + playerbot::PLAYERBOT_HUNTER_AUTOSHOT_MIN_SAFETY_MARGIN &&
            rangeInfo.exactDistance <= rangeInfo.maxRange;
        if (!legalTarget)
        {
            clearPlantState();
            return false;
        }

        // TEMPORARY diagnostic: this is the code that actually runs every pass
        // while a plant is held (the kite loop is short-circuited upstream by
        // the IsHunterAutoShotPlantActive guards). Player::Update runs
        // Unit::Update -- and with it Unit::_UpdateAutoRepeatSpell -- right
        // before these AI passes, and nothing in between touches this state.
        // Sampling here, before the stop/face calls below re-clean movement
        // flags, therefore reports the exact gate values the core's
        // auto-repeat update just evaluated: its isMoving()/casting
        // early-return inputs and the live CheckCast(true) verdict. For spell
        // 75 a CheckCast failure neither interrupts nor resets the ranged
        // timer, making it invisible for a socketless bot without this.
        {
            static std::unordered_map<uint64, uint32> coreGateDiagLastMsByGuid;
            uint32& lastDiagMs = playerbot::LockedGetOrCreate(coreGateDiagLastMsByGuid, guidRaw);
            if (!lastDiagMs || nowMs >= lastDiagMs + 750)
            {
                lastDiagMs = nowMs;

                bool const engineSawMoving = player->isMoving();
                uint32 const engineMoveFlags = player->GetUnitMovementFlags();
                bool const engineSawCastBlock = player->IsNonMeleeSpellCast(false, false, true, true);
                uint32 const engineTimerMs = player->getAttackTimer(RANGED_ATTACK);
                Spell* autoRepeatMutable = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
                SpellCastResult const liveCheck = autoRepeatMutable ? autoRepeatMutable->CheckCast(true) : SPELL_CAST_OK;

                std::ostringstream coreGateDiag;
                coreGateDiag << "[AutoShot] core-gate moving=" << (engineSawMoving ? 1 : 0)
                    << " flags=0x" << std::hex << engineMoveFlags << std::dec
                    << " castBlock=" << (engineSawCastBlock ? 1 : 0)
                    << " timer=" << engineTimerMs
                    << " dist=" << rangeInfo.exactDistance
                    << " check=" << (!autoRepeatMutable ? "NO_SPELL" : (liveCheck == SPELL_CAST_OK ? "OK" : EnumUtils::ToString(liveCheck).Title));
                WhisperAutoShotDiagnosticToArena(player, coreGateDiag.str(), 0);
            }
        }

        // Keep the hunter settled and facing a strafing target while every
        // tactical/class movement source yields to this plant. Facing updates
        // do not restart movement, but failing to track the target can make the
        // core reject the release at the end of an otherwise valid plant.
        StopHunterTargetMovementForPlant(player);
        player->SetFacingToObject(target);
        player->SetInFront(target);
        StopHunterTargetMovementForPlant(player);

        // The core is the only authority on whether Auto Shot can actually
        // release, and this plant's own legality model (alive / no breakable CC /
        // range band / IsWithinLOSInMap) is a SUBSET of what CheckCast enforces.
        // Anything CheckCast rejects that this gate cannot see - the fork's
        // LoS-blocker aura is the live example, since IsWithinLOSInMap knows
        // nothing about it - produces a plant that can never end:
        //
        //   Unit::_UpdateAutoRepeatSpell (Unit.cpp:3505) special-cases spell 75.
        //   On a failed CheckCast it neither interrupts the auto-repeat NOR
        //   calls resetAttackTimer(RANGED_ATTACK), so the ranged timer stays at
        //   0. The plant's releases both require the shot to land, the 2.2 s
        //   deadline clears the plant, and DriveHunterKiteLoop re-arms it on the
        //   same pass because the timer is still 0. Forever.
        //
        // Ask the core directly once the plant has outlived a full release
        // window. A hunter that is genuinely shooting clears its plant inside
        // the 434 ms window and never reaches this, so correct kiting is
        // untouched.
        uint32 const plantHeldMs = PLAYERBOT_HUNTER_STUTTER_MAX_PLANT_MS - (plantUntilMs - nowMs);
        if (plantHeldMs >= PLAYERBOT_HUNTER_STUTTER_PLANT_LEAD_MS)
        {
            Spell* autoRepeatLive = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
            SpellCastResult const coreVerdict = autoRepeatLive ? autoRepeatLive->CheckCast(true) : SPELL_FAILED_ERROR;
            if (coreVerdict != SPELL_CAST_OK)
            {
                std::ostringstream rejectDiag;
                rejectDiag << "[AutoShot] core-reject held=" << plantHeldMs
                    << " check=" << (!autoRepeatLive ? "NO_SPELL" : EnumUtils::ToString(coreVerdict).Title)
                    << " - releasing plant and repositioning";
                WhisperAutoShotDiagnosticToArena(player, rejectDiag.str(), 0);

                playerbot::LockedSet(g_HunterAutoShotCoreRejectUntilMs, guidRaw,
                    nowMs + PLAYERBOT_HUNTER_AUTOSHOT_CORE_REJECT_BACKOFF_MS);
                clearPlantState();
                return false;
            }
        }

        return true;
    }

    bool BreakExpiredHunterFeignDeath(Player* player)
    {
        if (!player || player->GetClass() != CLASS_HUNTER || !player->HasAura(kHunterFeignDeathSpellId))
            return false;

        // Give Feign Death a tiny tactical grace window after the cast so it can
        // actually drop combat for a trap, but never let a clipped/non-progressing
        // FD state make the bot fail CanIssueBotMovement forever. Any selected
        // class action will also stand the hunter before casting.
        if (playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kHunterFeignDeathSpellId))
            return false;

        player->RemoveAurasDueToSpell(kHunterFeignDeathSpellId);
        player->SetStandState(UNIT_STAND_STATE_STAND);
        if (MotionMaster* motionMaster = player->GetMotionMaster())
            motionMaster->Clear(MOTION_SLOT_ACTIVE);
        return true;
    }

    bool GetHunterAutoShotRange(Player* player, Unit const* target, playerbot::HunterAutoShotRangeInfo& rangeInfo)
    {
        return playerbot::PvpCore::GetHunterAutoShotRange(player, target, rangeInfo);
    }


    constexpr uint32 kPlayerbotWandShootSpellId = 5019;
    std::unordered_map<uint64, uint32> g_WandLifecycleDiagNextMs;

    bool HasActiveWandAutoRepeat(Player const* player)
    {
        if (!player)
            return false;

        Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        if (!autoRepeat || !autoRepeat->GetSpellInfo())
            return false;

        SpellInfo const* firstRank = autoRepeat->GetSpellInfo()->GetFirstRankSpell();
        uint32 const firstRankSpellId = firstRank ? firstRank->Id : autoRepeat->GetSpellInfo()->Id;
        return firstRankSpellId == kPlayerbotWandShootSpellId;
    }

    bool IsInWandShootRange(Player const* player, Unit const* target)
    {
        if (!player || !target)
            return false;

        SpellInfo const* wandInfo = sSpellMgr->GetSpellInfo(kPlayerbotWandShootSpellId);
        if (!wandInfo)
            return false;

        float const distance = player->GetDistance(target);
        float const minRange = wandInfo->GetMinRange(false);
        float const maxRange = wandInfo->GetMaxRange(false);
        return distance >= minRange && (maxRange <= 0.0f || distance <= maxRange) && player->IsWithinLOSInMap(target);
    }

    void NotifyWandLifecycleDiagnostic(Player*, Unit*, char const*, bool, bool)
    {
        // Intentionally silent. This hook was used for temporary wand movement
        // troubleshooting whispers; keep HoldPositionForWand behavior without
        // player-visible diagnostics.
    }

    bool HoldPositionForWand(Player* player, Unit* target, char const* reason)
    {
        if (!player || !target || !target->IsAlive())
            return false;

        bool const activeWand = HasActiveWandAutoRepeat(player);
        bool const recentWandStart = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotWandShootSpellId);
        if (!activeWand && !recentWandStart)
            return false;

        if (!IsInWandShootRange(player, target))
            return false;

        bool const wasMoving = player->isMoving() || player->HasUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE) || (player->GetUnitMovementFlags() & MOVEMENTFLAG_MASK_MOVING);
        player->StopMoving();
        if (MotionMaster* motionMaster = player->GetMotionMaster())
            motionMaster->Clear(MOTION_SLOT_ACTIVE);

        if (WorldSession* session = player->GetSession(); session && (session->IsVirtualSession() || session->IsTransientPlayerSession()))
        {
            player->ClearUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE);
            player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
            // Throttled/shared swim-state resync; see
            // ResyncPlayerbotSwimStateForMovementStop for the full rationale.
            playerbot::ResyncPlayerbotSwimStateForMovementStop(player);
        }

        if (wasMoving || activeWand || recentWandStart)
            NotifyWandLifecycleDiagnostic(player, target, reason, activeWand, recentWandStart);

        return true;
    }

    struct CombatPositioningProfile
    {
        float preferredMinRange = 0.0f;
        float preferredIdealRange = 0.0f;
        float preferredMaxPressureRange = 0.0f;
        bool primarilyRanged = false;
        bool createDistanceWhenCrowded = false;
        bool meleeFallbackAcceptable = true;
        char const* label = "default";
    };

    bool IsDruidFeralMeleePositioning(Player const* player)
    {
        if (!player || player->GetClass() != CLASS_DRUID)
            return false;

        // Prowl is cat-only in normal 3.3.5, but checking the form/aura makes
        // this robust against custom aura state oddities and delayed form sync.
        switch (player->GetShapeshiftForm())
        {
        case FORM_CAT:
        case FORM_BEAR:
        case FORM_DIREBEAR:
            return true;
        default:
            break;
        }

        return player->HasAura(768) ||   // Cat Form
               player->HasAura(5487) ||  // Bear Form
               player->HasAura(9634) ||  // Dire Bear Form
               player->HasAura(5215) ||  // Prowl rank 1
               player->HasAura(6783) ||  // Prowl rank 2
               player->HasAura(9913);    // Prowl rank 3
    }

    bool IsStealthedMeleeOpener(Player const* player)
    {
        if (!player || !player->HasStealthAura())
            return false;

        return player->GetClass() == CLASS_ROGUE || IsDruidFeralMeleePositioning(player);
    }

    // Same line the paladin spell picker heals at (Flash of Light / Holy Light
    // on allies at or below 85%).
    constexpr float kPaladinHealerStandBackHealthPct = 85.0f;

    // Whether a healer has any reason to hold heal range: a living teammate
    // hurt enough to be healed. Outside a battleground there is no team to
    // read, so the healer spacing holds.
    bool HasTeammateNeedingHeal(Player const* player, float maxHealthPct)
    {
        Battleground const* battleground = player->GetBattleground();
        Map* map = player->FindMap();
        if (!battleground || !map)
            return true;

        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        {
            Player const* other = itr->GetSource();
            if (other && other != player && other->IsAlive() && other->GetHealthPct() <= maxHealthPct &&
                battleground->AreTeammates(player->GetGUID(), other->GetGUID()))
                return true;
        }

        return false;
    }

    CombatPositioningProfile GetCombatPositioningProfile(Player const* player)
    {
        if (!player)
            return {};

        if (IsDruidFeralMeleePositioning(player))
            return { 0.0f, 1.5f, 5.0f, false, false, true, "druid-feral-melee" };

        // A holy paladin is still a plate wearer with a weapon: while nobody
        // on the team needs a heal, hovering at heal range just leaves it idle,
        // so it fights in melee and drops back only when there is healing to do.
        if (player->GetClass() == CLASS_PALADIN && !HasTeammateNeedingHeal(player, kPaladinHealerStandBackHealthPct))
            return { 0.0f, 3.0f, 8.0f, false, false, true, "paladin-nothing-to-heal" };

        if (Trinity::Helpers::Entity::IsPlayerHealer(player))
        {
            switch (player->GetClass())
            {
            case CLASS_PRIEST: return { 0.0f, 25.0f, 34.0f, true, false, false, "priest-healer" };
            case CLASS_SHAMAN: return { 0.0f, 20.0f, 30.0f, true, false, true, "shaman-healer" };
            case CLASS_DRUID: return { 0.0f, 18.0f, 28.0f, true, false, true, "druid-healer" };
            case CLASS_PALADIN: return { 0.0f, 16.0f, 26.0f, true, false, true, "paladin-healer" };
            default: break;
            }
        }

        switch (player->GetClass())
        {
        case CLASS_HUNTER:
            // The actual learned ability, not the detected talent profile, owns
            // hunter spacing. Any hunter that knows Mongoose Bite should hold
            // around 10 yards so it can step in and consume a Sting when the
            // bite is ready; hunters without it hold five yards inside their
            // true modified Auto Shot range in DriveHunterKiteLoop.
            if (player->HasSpell(81285))
                return { 8.0f, 10.0f, 15.0f, true, true, true, "hunter-mongoose-weave" };
            return { 8.0f, 28.0f, 38.0f, true, true, false, "hunter-ranged" };
        case CLASS_MAGE: return { 12.0f, 27.0f, 36.0f, true, true, false, "mage-ranged" };
        case CLASS_PRIEST: return { 10.0f, 25.0f, 34.0f, true, true, false, "priest-ranged" };
        case CLASS_WARLOCK: return { 10.0f, 26.0f, 35.0f, true, true, false, "warlock-ranged" };
        case CLASS_WARRIOR: return { 0.0f, 1.5f, 5.0f, false, false, true, "warrior-melee" };
        case CLASS_ROGUE: return { 0.0f, 1.5f, 5.0f, false, false, true, "rogue-melee" };
        case CLASS_PALADIN: return { 0.0f, 3.0f, 8.0f, false, false, true, "paladin-hybrid" };
        case CLASS_SHAMAN:
            // Enhancement is a melee spec, and the class selector already plays
            // it as one. The hybrid band below parked it twenty to thirty yards
            // out - the hold-band branch stops it there, and the engage call
            // turns its swings off at that range - so whenever an enemy within
            // Purge's thirty yards had a buff to strip, it stood still and
            // purged with nothing else going on. Ask the selector instead of
            // keeping a second opinion here.
            if (playerbot::PvpCore::UsesMeleeSpacing(player))
                return { 0.0f, 1.5f, 5.0f, false, false, true, "shaman-enhancement-melee" };
            return { 5.0f, 20.0f, 30.0f, true, true, true, "shaman-hybrid" };
        case CLASS_DRUID: return { 4.0f, 18.0f, 28.0f, true, true, true, "druid-hybrid" };
        default: return { 0.0f, 3.0f, 8.0f, false, false, true, "default-melee" };
        }
    }

    bool MoveAwayFromUnit(Player* player, Unit* target, float desiredDistance)
    {
        // Do not pre-check CanIssueMovementCommand here: IssueMovePointThrottled
        // below performs the same throttle check internally. GameTime is a
        // per-World::Update cache, not a live clock, so calling the gate twice
        // in the same tick consumes the throttle token on the first call and
        // guarantees the second (real) check fails -- silently turning this
        // into a permanent no-op.
        if (!player || !target || !CanIssueBotMovement(player))
            return false;

        float const angleAway = target->GetAbsoluteAngle(player);
        float const currentDistance = player->GetDistance(target);
        float const moveDistance = std::max(4.0f, desiredDistance - currentDistance + 2.0f);

        Position destination(player->GetPositionX() + std::cos(angleAway) * moveDistance,
            player->GetPositionY() + std::sin(angleAway) * moveDistance,
            player->GetPositionZ(), player->GetOrientation());
        return IssueMovePointThrottled(player, destination, 4.0f, 500);
    }

    bool TryDrivePlayerbotEscapeAura(Player* player, Unit* target)
    {
        if (!player)
            return false;

        // Preserve the completed Elune's Grace invisibility instead of
        // immediately attacking and cancelling it on the next tactical tick.
        if (player->HasInvisibilityAura())
        {
            player->AttackStop();
            player->SetSelection(ObjectGuid::Empty);
            return true;
        }

        bool const fadingFromElunesGrace = player->HasAura(PLAYERBOT_PRIEST_ELUNES_GRACE_SPELL_ID);
        bool const inWispForm = player->GetClass() == CLASS_PRIEST &&
            player->HasAura(PLAYERBOT_PRIEST_WISP_FORM_SPELL_ID);
        if (!fadingFromElunesGrace && !inWispForm)
            return false;

        player->AttackStop();
        if (target && target->IsAlive())
            MoveAwayFromUnit(player, target, std::max(20.0f, playerbot::PvpCore::GetConfig().closeRange + 5.0f));

        // The escape aura owns this tick even when movement is temporarily
        // throttled; falling through to normal combat would replace its route.
        return true;
    }

    bool TryRecoverLineOfSight(Player* player, Unit* target, CombatPositioningProfile const& profile, char const* reason)
    {
        // See MoveAwayFromUnit above: no outer CanIssueMovementCommand check --
        // IssueMovePointThrottled already gates this call, and double-gating
        // guaranteed failure every time this function was reached.
        if (!player || !target || !target->IsAlive() || !CanIssueBotMovement(player))
            return false;

        struct LosRecoveryState
        {
            ObjectGuid targetGuid;
            float orbitAngle = 0.0f;
            bool hasAngle = false;
        };
        static std::unordered_map<uint64, LosRecoveryState> stateByGuid;
        uint64 const botGuid = player->GetGUID().GetRawValue();
        LosRecoveryState& state = playerbot::LockedGetOrCreate(stateByGuid, botGuid);

        if (player->IsWithinLOSInMap(target))
        {
            state.hasAngle = false;
            return false;
        }

        // Pick the orbit angle once per LOS-loss episode instead of
        // re-randomizing every call. A fresh +-0.85 rad jitter each time swings
        // the destination by many yards -- comfortably past the reissue
        // threshold below -- so every successful call looked like the
        // destination changed, forcing a brand new segment every time instead
        // of a smooth orbit toward line of sight.
        if (!state.hasAngle || state.targetGuid != target->GetGUID())
        {
            state.orbitAngle = target->GetAbsoluteAngle(player) + frand(-0.85f, 0.85f);
            state.targetGuid = target->GetGUID();
            state.hasAngle = true;
        }

        float const orbitRange = std::max(profile.preferredMinRange + 2.0f, profile.preferredIdealRange);
        Position reposition(target->GetPositionX() + std::cos(state.orbitAngle) * orbitRange,
            target->GetPositionY() + std::sin(state.orbitAngle) * orbitRange,
            target->GetPositionZ(), player->GetOrientation());
        if (!IssueMovePointThrottled(player, reposition, 4.0f, 500))
            return false;

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP LOS recovery: bot={} target={} profile={} reason={} orbitRange={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString(), profile.label, reason ? reason : "unknown", orbitRange);
        return true;
    }


    bool HunterHasActiveAutoShot(Player const* player)
    {
        if (!player)
            return false;

        Spell const* autoRepeatSpell = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        return autoRepeatSpell && autoRepeatSpell->GetSpellInfo() && autoRepeatSpell->GetSpellInfo()->Id == 75;
    }

    void StopHunterAutoShotForStationaryCast(Player* player, char const* reason)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return;

        if (HunterHasActiveAutoShot(player))
        {
            WhisperHunterAimedLifecycleDiagnostic(player, nullptr, "autoshot_interrupt_for_stationary_cast", reason, 150);
            player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        }

        if (reason)
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP hunter stationary cast suppresses Auto Shot: bot={} reason={}",
                player->GetGUID().ToString(), reason);
    }


    void StopHunterAutoShotForBreakableCrowdControl(Player* player, Unit* target, char const* reason)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return;

        if (HunterHasActiveAutoShot(player))
            player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);

        if (target && player->GetVictim() && player->GetVictim()->GetGUID() == target->GetGUID())
            player->AttackStop();

        if (Pet* pet = player->GetPet())
            if (target && pet->GetVictim() && pet->GetVictim()->GetGUID() == target->GetGUID())
                pet->AttackStop();

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP hunter suppress damage on breakable CC: bot={} target={} reason={}",
            player->GetGUID().ToString(), target ? target->GetGUID().ToString() : ObjectGuid::Empty.ToString(), reason ? reason : "breakable-cc");
    }

#if 0 // Temporary Aimed Shot whisper diagnostics disabled; retain for future troubleshooting.
    char const* GetSpellStateLabelForAimedDiag(Spell const* spell)
    {
        if (!spell)
            return "none";

        switch (spell->getState())
        {
            case SPELL_STATE_NULL: return "null";
            case SPELL_STATE_PREPARING: return "preparing";
            case SPELL_STATE_CASTING: return "casting";
            case SPELL_STATE_FINISHED: return "finished";
            case SPELL_STATE_DELAYED: return "delayed";
            default: return "unknown";
        }
    }

    void WhisperHunterAimedLifecycleDiagnosticImpl(Player* player, Unit* target, char const* phase, char const* extra = nullptr, uint32 throttleMs = 0)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return;

        uint32 const nowMs = GameTime::GetGameTimeMS();
        if (throttleMs)
        {
            uint32& lastMs = g_HunterAimedDiagLastMsByGuid[player->GetGUID().GetRawValue()];
            if (lastMs && nowMs < lastMs + throttleMs)
                return;
            lastMs = nowMs;
        }

        Spell const* generic = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        Spell const* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        SpellInfo const* genericInfo = generic ? generic->GetSpellInfo() : nullptr;
        SpellInfo const* channelInfo = channel ? channel->GetSpellInfo() : nullptr;
        SpellInfo const* autoInfo = autoRepeat ? autoRepeat->GetSpellInfo() : nullptr;
        MotionMaster* motionMaster = player->GetMotionMaster();
        MovementGeneratorType const motionType = motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;

        std::ostringstream os;
        os << "AIMED DIAG: lifecycle phase=" << (phase ? phase : "unknown");
        if (extra && *extra)
            os << " extra=" << extra;

        os << " gen=" << (genericInfo ? genericInfo->Id : 0) << ':' << GetSpellStateLabelForAimedDiag(generic)
           << " chan=" << (channelInfo ? channelInfo->Id : 0) << ':' << GetSpellStateLabelForAimedDiag(channel)
           << " auto=" << (autoInfo ? autoInfo->Id : 0) << ':' << GetSpellStateLabelForAimedDiag(autoRepeat)
           << " moving=" << (player->isMoving() ? "yes" : "no")
           << " move_state=" << (player->HasUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE) ? "yes" : "no")
           << " move_flags=" << uint32(player->GetUnitMovementFlags())
           << " motion=" << uint32(motionType)
           << " nonmelee=" << (player->IsNonMeleeSpellCast(false, false, true) ? "yes" : "no")
           << " ranged_timer=" << player->getAttackTimer(RANGED_ATTACK)
           << " lock=" << (playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotHunterStationaryCastLockToken) ? "yes" : "no");

        if (target)
        {
            os << " target=" << target->GetGUID().ToString()
               << " dist=" << player->GetDistance(target)
               << " exact=" << player->GetExactDist(target)
               << " los=" << (player->IsWithinLOSInMap(target) ? "yes" : "no")
               << " front=" << (player->isInFront(target) ? "yes" : "no");
        }

        std::string const message = os.str();
        if (player->duel && WantsPlayerbotDiagnostics(player->duel->Opponent))
            player->Whisper(message, LANG_UNIVERSAL, player->duel->Opponent);

        Map* map = player->FindMap();
        if (!map)
            return;

        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        {
            Player* observer = itr->GetSource();
            if (!WantsPlayerbotDiagnostics(observer))
                continue;
            if (player->duel && player->duel->Opponent && observer->GetGUID() == player->duel->Opponent->GetGUID())
                continue;
            player->Whisper(message, LANG_UNIVERSAL, observer);
        }
    }
#endif

    void WhisperHunterAimedLifecycleDiagnostic(Player* player, Unit* target, char const* phase, char const* extra = nullptr, uint32 throttleMs = 0)
    {
        (void)player;
        (void)target;
        (void)phase;
        (void)extra;
        (void)throttleMs;
        // WhisperHunterAimedLifecycleDiagnosticImpl(player, target, phase, extra, throttleMs);
    }

    bool IsHunterAimedShotSpellId(uint32 spellId)
    {
        switch (spellId)
        {
            case 19434: // Aimed Shot rank 1
            case 20900:
            case 20901:
            case 20902:
            case 20903:
            case 20904:
            case 27065:
            case 49049:
            case 49050:
                return true;
            default:
                return false;
        }
    }

    bool IsHunterAimedShotSpell(SpellInfo const* spellInfo)
    {
        if (!spellInfo)
            return false;

        if (IsHunterAimedShotSpellId(spellInfo->Id))
            return true;

        SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
        return firstRank && IsHunterAimedShotSpellId(firstRank->Id);
    }

    bool IsHunterMultiShotSpellId(uint32 spellId)
    {
        switch (spellId)
        {
            case 2643:  // Multi-Shot rank 1
            case 14288:
            case 14289:
            case 14290:
            case 25294:
            case 27021:
            case 49047:
            case 49048:
                return true;
            default:
                return false;
        }
    }

    bool IsHunterMultiShotSpell(SpellInfo const* spellInfo)
    {
        if (!spellInfo)
            return false;

        if (IsHunterMultiShotSpellId(spellInfo->Id))
            return true;

        SpellInfo const* firstRank = spellInfo->GetFirstRankSpell();
        return firstRank && IsHunterMultiShotSpellId(firstRank->Id);
    }

    uint32 GetHunterStationaryCastTimeMs(SpellInfo const* spellInfo)
    {
        if (!spellInfo)
            return 0;

        uint32 castTimeMs = uint32(std::max<int32>(0, spellInfo->CalcCastTime()));
        if (IsHunterAimedShotSpell(spellInfo))
            return std::max<uint32>(castTimeMs, 3000);

        if (spellInfo->Id == 982)
            return std::max<uint32>(castTimeMs, 1000);

        // Multi-Shot is short, but it still has a stationary firing delay/cast
        // window in this branch. It must be protected from stutter movement too;
        // otherwise the hunter starts Multi-Shot, immediately resumes fleeing,
        // and clips the shot before it launches. Use a short explicit guard even
        // when custom DBC data reports zero cast time.
        if (IsHunterMultiShotSpell(spellInfo))
            return std::max<uint32>(castTimeMs, 500);

        return castTimeMs > 0 ? castTimeMs : 0;
    }


    void DelayHunterRangedTimerForStationaryShot(Player* player, SpellInfo const* spellInfo)
    {
        if (!player || player->GetClass() != CLASS_HUNTER || !spellInfo)
            return;

        if (GetHunterStationaryCastTimeMs(spellInfo) == 0)
            return;

        if (player->getAttackTimer(RANGED_ATTACK) < 500)
            player->setAttackTimer(RANGED_ATTACK, 434);
    }

    bool IsActiveHunterStationaryShotSpell(Spell const* spell)
    {
        if (!spell || spell->getState() == SPELL_STATE_FINISHED)
            return false;

        SpellInfo const* spellInfo = spell->GetSpellInfo();
        if (!spellInfo)
            return false;

        return GetHunterStationaryCastTimeMs(spellInfo) > 0 ||
            (spellInfo->IsChanneled() && !spellInfo->IsMoveAllowedChannel());
    }

    bool HunterIsHardCastingStationaryShot(Player const* player)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return false;

        if (IsActiveHunterStationaryShotSpell(player->GetCurrentSpell(CURRENT_GENERIC_SPELL)))
            return true;

        if (IsActiveHunterStationaryShotSpell(player->GetCurrentSpell(CURRENT_CHANNELED_SPELL)))
            return true;

        // Do not use broad IsNonMeleeSpellCast() here: the explicit
        // stationarity helper above covers the hunter shots we actually need
        // to protect (Aimed Shot, Multi-Shot, Revive Pet) without catching
        // unrelated delayed spell states.
        return false;
    }

    bool HoldHunterStationaryCast(Player* player, Unit* target, char const* reason)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return false;

        bool const activeCast = HunterIsHardCastingStationaryShot(player);
        bool const explicitLock = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotHunterStationaryCastLockToken);
        if (!activeCast && !explicitLock)
            return false;

        // Do not let an already-queued Auto Shot live through Aimed Shot/Revive
        // Pet. The decision and movement layers can be held, but auto-repeat is
        // updated by the core independently.
        //
        // Only Aimed Shot/Revive Pet-style hard casts cancel Auto Shot.
        // Multi-Shot still owns a real ~500ms stationary window, but it keeps
        // CURRENT_AUTOREPEAT_SPELL queued and merely delays its release. The
        // explicit residual lock prevents movement from clipping either shot.
        WhisperHunterAimedLifecycleDiagnostic(player, target, reason ? reason : "stationary_hold", activeCast ? "activeCast=1" : "activeCast=0", 250);
        SpellInfo const* activeGenericInfo = nullptr;
        if (Spell const* generic = player->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        {
            activeGenericInfo = generic->GetSpellInfo();
            DelayHunterRangedTimerForStationaryShot(player, activeGenericInfo);
        }
        if (activeCast && !IsHunterMultiShotSpell(activeGenericInfo))
            StopHunterAutoShotForStationaryCast(player, "hunter_stationary_cast_hold_stop_autoshot");

        bool const movedDuringCast = player->isMoving() ||
            player->HasUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE) ||
            (player->GetUnitMovementFlags() & MOVEMENTFLAG_MASK_MOVING);

        // Critical: while a real stationary hunter cast is already active, do
        // not repeatedly call StopMoving(), clear motion, or face the target on
        // every lifecycle tick. Those calls can themselves produce movement /
        // facing updates for virtual bots and clip the client cast bar. Only do
        // an emergency stop if movement actually resumed during the cast.
        if (activeCast && !movedDuringCast)
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP hunter stationary cast hold without movement touch: bot={} target={} reason={} timer={}",
                player->GetGUID().ToString(), target ? target->GetGUID().ToString() : ObjectGuid::Empty.ToString(),
                reason ? reason : "stationary-cast", player->getAttackTimer(RANGED_ATTACK));
            return true;
        }

        if (movedDuringCast)
        {
            WhisperHunterAimedLifecycleDiagnostic(player, target, "movement_detected_during_stationary_cast", reason ? reason : "stationary_hold", 150);
            StopVirtualPlayerbotMovement(player);
            if (Spell const* generic = player->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                DelayHunterRangedTimerForStationaryShot(player, generic->GetSpellInfo());
        }
        else if (!activeCast)
        {
            // Explicit lock race window before CURRENT_GENERIC_SPELL is visible:
            // one stop/facing correction is okay here because the cast is not
            // yet actively preparing.
            StopVirtualPlayerbotMovement(player);
            if (target && target->IsAlive() && player->IsValidAttackTarget(target) && player->IsWithinLOSInMap(target))
            {
                player->SetFacingToObject(target);
                player->SetInFront(target);
            }
        }

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP hunter stationary cast hold: bot={} target={} reason={} activeCast={} explicitLock={} timer={}",
            player->GetGUID().ToString(), target ? target->GetGUID().ToString() : ObjectGuid::Empty.ToString(),
            reason ? reason : "stationary-cast", activeCast ? 1 : 0, explicitLock ? 1 : 0,
            player->getAttackTimer(RANGED_ATTACK));
        return true;
    }

    bool IssueHunterStutterFlee(Player* player, Unit* target, float desiredExactDistance, char const* reason)
    {
        if (player && player->GetClass() == CLASS_HUNTER && HunterIsHardCastingStationaryShot(player))
        {
            WhisperHunterAimedLifecycleDiagnostic(player, target, "flee_blocked_active_cast", reason, 200);
            return false;
        }

        if (!player || player->GetClass() != CLASS_HUNTER || !target || !CanIssueBotMovement(player))
            return false;

        uint64 const guid = player->GetGUID().GetRawValue();
        uint32 const nowMs = GameTime::GetGameTimeMS();

        // Do not let the generic movement throttle turn a flee phase into a
        // stationary turret phase. If the hunter is already moving, preserving
        // that movement is success; if he is stopped, allow a forced flee issue
        // even if a recent stop/face/autoshot command just ran.
        bool const alreadyMoving = player->isMoving() || player->HasUnitState(UNIT_STATE_MOVING | UNIT_STATE_MOVE);
        uint32& lastFleeIssueMs = playerbot::LockedGetOrCreate(g_HunterLastFleeIssueMs, guid);
        MotionMaster* motionMaster = player->GetMotionMaster();
        bool const activeStutterPath = motionMaster &&
            motionMaster->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE && alreadyMoving;
        if (activeStutterPath && nowMs < lastFleeIssueMs + PLAYERBOT_HUNTER_FLEE_REISSUE_MS)
            return true;

        int8& side = playerbot::LockedGetOrCreate(g_HunterKiteSideByGuid, guid);
        if (side == 0)
            side = (guid & 1) ? int8(1) : int8(-1);

        float const currentDistance = player->GetExactDist(target);
        float const angleAway = target->GetAbsoluteAngle(player);
        float const neededDistance = desiredExactDistance > currentDistance ? desiredExactDistance - currentDistance : 0.0f;
        float const moveDistance = std::clamp(neededDistance + 4.0f, 8.0f, 16.0f);

        // Bias only slightly, and keep the side stable, so the hunter travels
        // generally away from the attacker instead of re-picking triangle legs.
        float const edgeBias = 0.18f * float(side);
        float const fleeAngle = angleAway + edgeBias;

        Position destination(player->GetPositionX() + std::cos(fleeAngle) * moveDistance,
            player->GetPositionY() + std::sin(fleeAngle) * moveDistance,
            player->GetPositionZ(), player->GetOrientation());

        // Hunter stutter movement needs to be able to resume immediately after
        // StopMoving()/face/autoshot. The generic movement throttle is shared
        // with stop/follow helpers and can otherwise leave the hunter standing
        // still for the whole post-shot window. Issue this flee segment directly
        // and keep our own lightweight reissue throttle above.
        ClearEatDrinkAurasForMovement(player);
        Position const safeDestination = BuildCollisionSafeDestination(player, destination);
        bool issued = false;
        char const* movementMode = "none";
        PathType pathType = PathType(0);
        if (motionMaster)
        {
            MovementGeneratorType const currentMovement = motionMaster->GetCurrentMovementGeneratorType();
            if (currentMovement == CHASE_MOTION_TYPE || currentMovement == FOLLOW_MOTION_TYPE || currentMovement == DISTRACT_MOTION_TYPE)
                motionMaster->Clear();

            bool const shouldUsePath = !player->IsFlying() && !player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
            Position pathDestination;
            bool const hasRetreatPath = shouldUsePath &&
                TryBuildBattlegroundSegmentDestination(player, safeDestination, pathDestination, &pathType);
            float const retreatSeparation = hasRetreatPath ? target->GetExactDist(pathDestination) : currentDistance;
            bool const retreatMakesProgress = hasRetreatPath && retreatSeparation >= currentDistance + 1.0f;
            if (retreatMakesProgress)
            {
                // Hunters were visually clipping through Blackrock Throne WMO
                // walls because the stutter-flee loop used a raw direct spline.
                // Always prefer a real mmap segment, and only accept a retreat
                // segment that actually creates separation from the attacker.
                motionMaster->MovePoint(0, pathDestination, true);
                movementMode = "mmap-segment";
                issued = true;
            }
            else if (shouldUsePath && currentDistance <= 12.0f)
            {
                // If the hunter is cornered, continuing to request the blocked
                // point behind it only pins it against the wall. Cross through
                // the close attacker and finish well beyond them; after the
                // crossing, the normal away vector points into open space and
                // regular kiting resumes. Unit collision does not block this
                // generated path, while terrain collision still does.
                float const throughAngle = player->GetAbsoluteAngle(target->GetPosition());
                float const throughDistance = currentDistance + 12.0f;
                Position throughDestination(player->GetPositionX() + std::cos(throughAngle) * throughDistance,
                    player->GetPositionY() + std::sin(throughAngle) * throughDistance,
                    player->GetPositionZ(), player->GetOrientation());
                Position const safeThroughDestination = BuildCollisionSafeDestination(player, throughDestination);
                Position throughPathDestination;
                PathType throughPathType = PathType(0);
                if (TryBuildBattlegroundSegmentDestination(player, safeThroughDestination, throughPathDestination, &throughPathType))
                {
                    motionMaster->MovePoint(0, throughPathDestination, true);
                    pathType = throughPathType;
                    movementMode = "corner-run-through-mmap";
                }
                else
                {
                    motionMaster->MovePoint(0, safeThroughDestination, true);
                    movementMode = "corner-run-through-generated";
                }
                issued = true;
            }
            else
            {
                // Strict segment probing can reject a short otherwise usable
                // route. Fall back to normal generated pathing for every stutter
                // phase, not only dead-zone emergencies, so Auto Shot charging
                // never turns the hunter into a stationary turret.
                motionMaster->MovePoint(0, safeDestination, true);
                movementMode = "generated-path-fallback";
                issued = true;
            }
            if (issued)
                lastFleeIssueMs = nowMs;
        }

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP hunter stutter loop: bot={} target={} decision=flee reason={} current={} desired={} move={} timer={} issued={} moving={} mode={} pathType={}",
            player->GetGUID().ToString(), target->GetGUID().ToString(), reason ? reason : "kite",
            currentDistance, desiredExactDistance, moveDistance, player->getAttackTimer(RANGED_ATTACK), issued ? 1 : 0, alreadyMoving ? 1 : 0,
            movementMode, uint32(pathType));
        return issued || alreadyMoving || player->isMoving();
    }

    bool HasAuraFromSpellChainCastBy(Unit const* unit, uint32 baseSpellId, ObjectGuid casterGuid)
    {
        if (!unit || !baseSpellId || casterGuid.IsEmpty())
            return false;

        SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(baseSpellId);
        if (!baseSpellInfo)
            return false;

        for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
            if (unit->HasAura(chainSpellId, casterGuid))
                return true;

        return false;
    }

    // Mirrors HasHunterDamagingStingFromCaster in PlayerbotPvpCore.cpp (a
    // separate translation unit's anonymous namespace, not linkable from
    // here) -- kept in sync manually if the sting spell list ever changes.
    bool HasHunterDamagingStingFromThisCaster(Unit const* unit, ObjectGuid casterGuid)
    {
        return HasAuraFromSpellChainCastBy(unit, 1978, casterGuid) || // Serpent Sting
            HasAuraFromSpellChainCastBy(unit, 3034, casterGuid) ||     // Viper Sting
            HasAuraFromSpellChainCastBy(unit, 3043, casterGuid);       // Scorpid Sting
    }

    bool HunterKnowsMongooseBite(Player const* player)
    {
        // 81285 is the player-facing Mongoose Bite. 81286/81287 are the
        // triggered main/off-hand effects handled by spell_hunter.cpp and
        // must not independently make the AI think the ability is learned
        // or ready while the real 81285 cooldown is active.
        return player && player->HasSpell(81285);
    }

    bool IsHunterMongooseBiteReady(Player* player)
    {
        if (!HunterKnowsMongooseBite(player))
            return false;

        SpellHistory* history = player->GetSpellHistory();
        return history && !history->HasCooldown(81285);
    }

    bool TryCastCommittedMongooseBite(Player* player, Unit* target)
    {
        if (!player || !target || !target->IsAlive() || !player->IsWithinMeleeRange(target) ||
            !HasHunterDamagingStingFromThisCaster(target, player->GetGUID()))
        {
            return false;
        }

        SpellInfo const* mongooseInfo = sSpellMgr->GetSpellInfo(81285);
        SpellHistory* history = player->GetSpellHistory();
        if (!mongooseInfo || !history || history->HasCooldown(81285) || history->HasGlobalCooldown(mongooseInfo) ||
            player->IsNonMeleeSpellCast(false, false, true))
        {
            return false;
        }

        StopHunterTargetMovementForPlant(player);
        player->SetFacingToObject(target);
        player->SetInFront(target);
        StopHunterTargetMovementForPlant(player);

        if (!player->GetVictim() || player->GetVictim()->GetGUID() != target->GetGUID() ||
            !player->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            player->Attack(target, true);
        }

        SpellCastResult const castResult = player->CastSpell(target, 81285, false);
        std::ostringstream diag;
        diag << "[MongooseBite] committed-cast dist=" << player->GetExactDist(target)
             << " stung=" << (HasHunterDamagingStingFromThisCaster(target, player->GetGUID()) ? 1 : 0)
             << " result=" << (castResult == SPELL_CAST_OK ? "OK" : EnumUtils::ToString(castResult).Title)
             << " resultCode=" << uint32(castResult);
        WhisperAutoShotDiagnosticToArena(player, diag.str(), 500);
        return castResult == SPELL_CAST_OK;
    }

    bool DriveHunterKiteLoop(Player* player, Unit* target, CombatPositioningProfile const& profile)
    {
        if (!player || player->GetClass() != CLASS_HUNTER || !target || !target->IsAlive())
            return false;

        playerbot::HunterAutoShotRangeInfo rangeInfo;
        if (!GetHunterAutoShotRange(player, target, rangeInfo))
            return false;

        float const exactDistance = rangeInfo.exactDistance;
        float const minAutoShotRange = rangeInfo.minRange;
        float const maxAutoShotRange = rangeInfo.maxRange;

        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint64 const hunterGuidRaw = player->GetGUID().GetRawValue();
        uint32& plantUntilMs = playerbot::LockedGetOrCreate(g_HunterAutoShotPauseUntilMs, hunterGuidRaw);

        auto clearPlantState = [&]()
        {
            plantUntilMs = 0;
            playerbot::LockedErase(g_HunterAutoShotPlantFireSequence, hunterGuidRaw);
        };
        float const safeShootMin = minAutoShotRange + playerbot::PLAYERBOT_HUNTER_AUTOSHOT_MIN_SAFETY_MARGIN;

        // "Ideal range": a hunter that knows Mongoose Bite holds close
        // (~10y) to weave it between shots. Otherwise it holds five yards
        // inside its own fully modified Auto Shot range (Hawk Eye included).
        bool const hasMongooseBite = HunterKnowsMongooseBite(player);
        float const idealRange = hasMongooseBite
            ? std::max(10.0f, safeShootMin + 1.0f)
            : std::max(safeShootMin + 1.0f, maxAutoShotRange - 5.0f);

        // If Mongoose Bite is actually off cooldown and the kill target is
        // already stung, closing to melee to land the bite takes priority
        // over holding the ranged ideal range.
        bool const mongooseBiteReady = hasMongooseBite && IsHunterMongooseBiteReady(player);
        bool const targetStung = mongooseBiteReady && HasHunterDamagingStingFromThisCaster(target, player->GetGUID());
        bool const shouldCommitForBite = mongooseBiteReady && targetStung;
        bool const shouldCloseForBite = shouldCommitForBite && !player->IsWithinMeleeRange(target);
        // Stay committed at melee distance until the bite consumes the Sting.
        // Switching back to the 10-yard ideal the instant melee range was
        // reached made lifecycle movement retreat before the class tick could
        // actually cast Mongoose Bite.
        float const desiredDistance = shouldCommitForBite ? 3.0f : idealRange;

        bool const hasLos = player->IsWithinLOSInMap(target);

        // The plant released because the CORE refused the shot for a reason this
        // loop cannot see (IsWithinLOSInMap says yes, CheckCast says no - the
        // fork's LoS-blocker aura being the live example). Treat that exactly
        // like having no line of sight: it is the same situation, "I cannot
        // shoot from here", and it already has a purpose-built recovery.
        // Without this the loop re-arms the plant on this very pass, because
        // spell 75's failed CheckCast leaves the ranged timer at 0 forever.
        uint32 const* coreRejectUntilMs = playerbot::LockedFind(g_HunterAutoShotCoreRejectUntilMs, hunterGuidRaw);
        bool const coreRejectsAutoShot = coreRejectUntilMs && *coreRejectUntilMs > nowMs;

        bool const inAutoShotBand = hasLos && exactDistance > safeShootMin && exactDistance <= maxAutoShotRange;
        bool const tooClose = exactDistance <= safeShootMin;

        if (playerbot::PvpCore::HasBreakableCrowdControlFor(player, target))
        {
            clearPlantState();
            StopHunterAutoShotForBreakableCrowdControl(player, target, "kite-loop-breakable-cc");

            // Breakable CC must suppress damage, not suppress movement. If the
            // target is trapped/wyverned/scattered in the hunter's melee or
            // dead-zone band, use that CC window to reposition out to firing
            // range instead of standing still and waiting for the CC to end.
            if (!shouldCloseForBite && (tooClose || exactDistance < safeShootMin + 6.0f))
            {
                MarkHunterKiteHold(player);
                return IssueHunterStutterFlee(player, target, std::max(safeShootMin + 8.0f, 17.0f), "breakable-cc-reposition-too-close");
            }

            return true;
        }

        auto stopFaceAndKeepAutoShot = [&]()
        {
            if (playerbot::PvpCore::HasBreakableCrowdControlFor(player, target))
            {
                StopHunterAutoShotForBreakableCrowdControl(player, target, "plant-suppressed-breakable-cc");
                return;
            }

            StopHunterTargetMovementForPlant(player);
            player->SetFacingToObject(target);
            player->SetInFront(target);
            StopHunterTargetMovementForPlant(player);

            Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
            bool const autoShotActive = autoRepeat && autoRepeat->GetSpellInfo() && autoRepeat->GetSpellInfo()->Id == 75;
            bool const autoShotOnTarget = autoShotActive && autoRepeat->m_targets.GetUnitTargetGUID() == target->GetGUID();
            if (autoShotActive && !autoShotOnTarget)
                player->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
            if (!autoShotOnTarget && player->HasSpell(75))
            {
                SpellCastResult const castResult = player->CastSpell(target, 75, false);
                std::ostringstream activationDiag;
                activationDiag << "[AutoShot] activate dist=" << exactDistance
                    << " effectiveMin=" << minAutoShotRange
                    << " safeMin=" << safeShootMin
                    << " max=" << maxAutoShotRange
                    << " result=" << (castResult == SPELL_CAST_OK ? "OK" : EnumUtils::ToString(castResult).Title)
                    << " resultCode=" << uint32(castResult);
                WhisperAutoShotDiagnosticToArena(player, activationDiag.str(), 0);
            }
        };

        // Actively pursue the current desired distance (ideal range, or
        // melee range when closing for a ready Mongoose Bite). Moving does
        // not reset the ranged attack timer -- it only clamps the last
        // ~0.5s/0.434s of it until the hunter stands still for that long, so
        // it is always safe to keep pursuing the desired distance whenever a
        // shot is not actively planted/about to release (that case is
        // handled separately below, before this is ever reached).
        auto pursueDesiredDistance = [&](char const* reason) -> bool
        {
            if (std::fabs(exactDistance - desiredDistance) <= 2.0f)
            {
                StopHunterTargetMovementForPlant(player);
                if (hasLos)
                {
                    player->SetFacingToObject(target);
                    player->SetInFront(target);
                }
                return true;
            }

            bool const issued = IssueHumanLikeFollow(player, target, desiredDistance, 3.0f, 500);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP hunter ideal-range pursuit: bot={} target={} reason={} distance={} desired={} issued={}",
                player->GetGUID().ToString(), target->GetGUID().ToString(), reason, exactDistance, desiredDistance, issued ? 1 : 0);
            return issued || player->isMoving();
        };

        if (HoldHunterStationaryCast(player, target, "hunter-kite-loop"))
            return true;

        // Once the ready/stung weave has reached true melee range, own the
        // action here instead of waiting for the slower class selector. The
        // selector and movement layers run on different cadences; previously
        // lifecycle could reach melee, then another ranged decision or retreat
        // won before spell 81285 was attempted. Hold melee and retry after the
        // GCD until Mongoose Bite consumes the Sting.
        if (shouldCommitForBite && player->IsWithinMeleeRange(target))
        {
            clearPlantState();
            StopHunterTargetMovementForPlant(player);
            if (hasLos)
            {
                player->SetFacingToObject(target);
                player->SetInFront(target);
            }

            TryCastCommittedMongooseBite(player, target);
            if (!player->GetVictim() || player->GetVictim()->GetGUID() != target->GetGUID() ||
                !player->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            {
                player->Attack(target, true);
            }
            return true;
        }

        if (!hasLos || coreRejectsAutoShot)
        {
            clearPlantState();
            return TryRecoverLineOfSight(player, target, profile,
                hasLos ? "hunter-stutter-core-reject" : "hunter-stutter-los");
        }

        if (tooClose && !shouldCommitForBite)
        {
            clearPlantState();
            MarkHunterKiteHold(player);
            return IssueHunterStutterFlee(player, target, std::max(safeShootMin + 6.0f, 15.0f), "too-close-or-deadzone");
        }

        // A ready, primed Mongoose Bite owns movement immediately. Do not let
        // a plant established on the previous pass hold the hunter at range
        // after the Sting/cooldown state has changed.
        if (shouldCommitForBite && plantUntilMs != 0)
            clearPlantState();

        if (plantUntilMs > nowMs)
        {
            stopFaceAndKeepAutoShot();

            uint32 const fireSequence = GetHunterAutoShotFireSequence(player);
            uint32 const* plantSequencePtr = playerbot::LockedFind(g_HunterAutoShotPlantFireSequence, hunterGuidRaw);
            uint32 const plantFireSequence = plantSequencePtr ? *plantSequencePtr : fireSequence;
            uint32 const rangedTimerMs = player->getAttackTimer(RANGED_ATTACK);

            // During the plant, tactical movement and the hunter class executor
            // are both held. Therefore a reset above the plant lead cannot have
            // come from Arcane Shot/Multi-Shot; it is a reliable second signal
            // that the core launched Auto Shot even if the PlayerScript callback
            // was not emitted for this auto-repeat path.
            bool const fireEventObserved = fireSequence != plantFireSequence;
            bool const rangedTimerReset = rangedTimerMs > PLAYERBOT_HUNTER_STUTTER_PLANT_LEAD_MS;
            if (fireEventObserved || rangedTimerReset)
            {
                std::ostringstream releaseDiag;
                releaseDiag << "[AutoShot] fired-detected timer=" << rangedTimerMs
                    << " sequence=" << fireSequence
                    << " event=" << (fireEventObserved ? 1 : 0)
                    << " timerReset=" << (rangedTimerReset ? 1 : 0);
                WhisperAutoShotDiagnosticToArena(player, releaseDiag.str(), 0);
                clearPlantState();

                // The shot landed. Never reflexively retreat -- a hunter must
                // never move just because a shot succeeded; only pursue the
                // desired distance, exactly like the normal between-shots
                // state below. A stationary target within ideal range should
                // see the hunter never move at all.
                return pursueDesiredDistance("autoshot-fired-pursue-ideal");
            }

            return true;
        }

        // If we somehow reached the max plant deadline without seeing the
        // triggered Auto Shot event, clear the stale plant state so the next tick
        // re-evaluates fresh instead of holding on a stale read forever.
        if (plantUntilMs != 0 && plantUntilMs <= nowMs)
        {
            plantUntilMs = 0;
            playerbot::LockedErase(g_HunterAutoShotPlantFireSequence, hunterGuidRaw);
        }

        uint32 const autoShotTimerMs = player->getAttackTimer(RANGED_ATTACK);
        bool const autoShotActive = HunterHasActiveAutoShot(player);

        // Do not plant (stop and commit to firing) while committed to a
        // Mongoose Bite opportunity. Closing to melee and remaining there
        // long enough for the class action to consume the Sting takes priority.
        bool const shouldPlantForAutoShot = inAutoShotBand && !shouldCommitForBite &&
            (autoShotTimerMs == 0 || autoShotTimerMs <= PLAYERBOT_HUNTER_STUTTER_PLANT_LEAD_MS);

        if (shouldPlantForAutoShot)
        {
            uint32 const maxPlantMs = PLAYERBOT_HUNTER_STUTTER_MAX_PLANT_MS;
            plantUntilMs = nowMs + maxPlantMs;
            playerbot::LockedSet(g_HunterAutoShotPlantFireSequence, hunterGuidRaw, GetHunterAutoShotFireSequence(player));
            playerbot::LockedErase(g_HunterFleeStateByGuid, hunterGuidRaw);
            stopFaceAndKeepAutoShot();

            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP hunter stutter loop: bot={} target={} decision=plant-autoshot distance={} timer={} active={} maxPlantMs={}",
                player->GetGUID().ToString(), target->GetGUID().ToString(), exactDistance, autoShotTimerMs,
                autoShotActive ? 1 : 0, maxPlantMs);

            std::ostringstream plantDiag;
            plantDiag << "[AutoShot] plant dist=" << exactDistance
                      << " effectiveMin=" << minAutoShotRange
                      << " safeMin=" << safeShootMin
                      << " max=" << maxAutoShotRange
                      << " timer=" << autoShotTimerMs
                      << " active=" << (HunterHasActiveAutoShot(player) ? 1 : 0);
            WhisperAutoShotDiagnosticToArena(player, plantDiag.str(), 250);
            return true;
        }

        // Not planting this tick (timer still has time left, out of band, or
        // closing for a bite): actively pursue the desired distance instead
        // of parking or fleeing. Moving is always safe here -- it only
        // clamps the last fraction of the ranged timer, which the plant
        // branch above already accounts for.
        return pursueDesiredDistance(shouldCommitForBite
            ? (shouldCloseForBite ? "closing-for-mongoose-bite" : "hold-for-mongoose-bite")
            : "pursue-ideal-range");
    }

    Player* FindFlagCarrierForDirective(Player* player, playerbot::FlagCarrierDirective directive)
    {
        if (!player || directive == playerbot::FlagCarrierDirective::None || !player->InBattleground())
            return nullptr;

        Battleground* battleground = player->GetBattleground();
        if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
            return nullptr;

        TeamId const botTeam = ResolveBotTeamId(player);
        TeamId const enemyTeam = (botTeam == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;

        if (BattlegroundWS* bgWs = dynamic_cast<BattlegroundWS*>(battleground))
        {
            ObjectGuid carrierGuid = ObjectGuid::Empty;
            if (directive == playerbot::FlagCarrierDirective::AttackEnemyCarrier)
                carrierGuid = bgWs->GetFlagPickerGUID(botTeam);
            else if (directive == playerbot::FlagCarrierDirective::ProtectTeamCarrier)
                carrierGuid = bgWs->GetFlagPickerGUID(enemyTeam);

            if (carrierGuid.IsEmpty())
                return nullptr;

            return ObjectAccessor::FindConnectedPlayer(carrierGuid);
        }

        if (BattlegroundTP* bgTp = dynamic_cast<BattlegroundTP*>(battleground))
        {
            ObjectGuid carrierGuid = ObjectGuid::Empty;
            if (directive == playerbot::FlagCarrierDirective::AttackEnemyCarrier)
                carrierGuid = bgTp->GetFlagPickerGUID(botTeam);
            else if (directive == playerbot::FlagCarrierDirective::ProtectTeamCarrier)
                carrierGuid = bgTp->GetFlagPickerGUID(enemyTeam);

            if (carrierGuid.IsEmpty())
                return nullptr;

            return ObjectAccessor::FindConnectedPlayer(carrierGuid);
        }

        auto findNeutralFlagCarrierForDirective = [&](ObjectGuid const& carrierGuid) -> Player*
        {
            if (carrierGuid.IsEmpty())
                return nullptr;

            Player* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid);
            if (!carrier)
                return nullptr;

            TeamId const carrierTeam = ResolveBotTeamId(carrier);
            if (directive == playerbot::FlagCarrierDirective::AttackEnemyCarrier && carrierTeam != botTeam)
                return carrier;
            if (directive == playerbot::FlagCarrierDirective::ProtectTeamCarrier && carrierTeam == botTeam)
                return carrier;

            return nullptr;
        };

        // Several players can carry at once where a battleground has more than
        // one neutral objective (Temple of Kotmogu's four orbs): go for the
        // nearest carrier the directive applies to.
        std::vector<ObjectGuid> carriers;
        battleground->GetObjectiveCarriers(carriers);
        Player* nearest = nullptr;
        float nearestDistance = 0.0f;
        for (ObjectGuid const& carrierGuid : carriers)
        {
            Player* carrier = findNeutralFlagCarrierForDirective(carrierGuid);
            if (!carrier || carrier->GetMapId() != player->GetMapId())
                continue;

            float const distance = player->GetExactDist2dSq(carrier);
            if (!nearest || distance < nearestDistance)
            {
                nearest = carrier;
                nearestDistance = distance;
            }
        }

        return nearest;
    }

    bool MoveTowardUnit(Player* player, Unit* target, float desiredDistance)
    {
        if (!player || !target)
            return false;
        if (!player->IsAlive() || !target->IsAlive() || player->GetMapId() != target->GetMapId() || !CanIssueBotMovement(player))
        {
            EmitBattlegroundGmDebug(player,
                "move-toward-unit=blocked botAlive=" + std::to_string(player->IsAlive() ? 1 : 0) +
                " targetAlive=" + std::to_string(target->IsAlive() ? 1 : 0) +
                " sameMap=" + std::to_string(player->GetMapId() == target->GetMapId() ? 1 : 0) +
                " canMove=" + std::to_string(CanIssueBotMovement(player) ? 1 : 0) +
                " motionType=" + std::to_string(uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType())), 1200);
            return false;
        }

        float const distanceToTarget = player->GetDistance(target);
        if (player->InBattleground() && distanceToTarget > desiredDistance)
        {
            Position destination = target->GetPosition();
            bool const moved = IssueMovePointThrottled(player, destination, 30.0f, 2000);
            EmitBattlegroundGmDebug(player,
                "move-toward-unit mode=battleground-segmented target=" + target->GetName() +
                " dist=" + std::to_string(int32(distanceToTarget)) +
                " issued=" + std::to_string(moved ? 1 : 0), 1200);
            return moved || player->isMoving();
        }

        CombatPositioningProfile const profile = GetCombatPositioningProfile(player);
        if (!player->IsWithinLOSInMap(target))
            return TryRecoverLineOfSight(player, target, profile, "move-toward-unit");

        if (!player->IsWithinDistInMap(target, desiredDistance))
        {
            // IssueHumanLikeFollow -> IssueMovePointThrottled already gates
            // this via CanIssueMovementCommand; an outer pre-check here
            // double-consumed the same-tick throttle token and made the
            // follow call always fail.
            ClearEatDrinkAurasForMovement(player);
            return IssueHumanLikeFollow(player, target, desiredDistance, 6.0f, 500);
        }

        return true;
    }

    struct DroppedFlagPickupDelay
    {
        ObjectGuid flagGuid = ObjectGuid::Empty;
        uint32 pickupNotBeforeMs = 0;
    };

    std::unordered_map<uint64, DroppedFlagPickupDelay> g_DroppedFlagPickupDelayByBotGuid;
    constexpr uint32 PLAYERBOT_DROPPED_FLAG_PICKUP_DELAY_MS = 1 * IN_MILLISECONDS;
    // A flag dropped at the runner's feet on purpose is not a surprise to it.
    constexpr uint32 PLAYERBOT_HANDOFF_FLAG_PICKUP_DELAY_MS = 150;

    bool TryAdvanceFlagObjective(Player* player, Battleground* battleground)
    {
        if (!player || !battleground || !player->FindMap())
            return false;

        uint64 const botGuidRaw = player->GetGUID().GetRawValue();

        Position capturePosition;
        if (battleground->GetFlagCapturePosition(player->GetGUID(), capturePosition))
        {
            playerbot::LockedErase(g_DroppedFlagPickupDelayByBotGuid, botGuidRaw);

            // Playerbots move entirely on the server and do not send the
            // client's CMSG_AREATRIGGER packet when they enter a capture
            // trigger. Reproduce the normal client path once the carrier is
            // physically inside the trigger the battleground scores in.
            uint32 const captureTriggerId = battleground->GetFlagCaptureAreaTrigger(player->GetGUID());
            if (captureTriggerId)
            {
                AreaTriggerEntry const* captureTrigger = sAreaTriggerStore.LookupEntry(captureTriggerId);
                if (captureTrigger && player->IsInAreaTriggerRadius(captureTrigger))
                {
                    battleground->HandleAreaTrigger(player, captureTriggerId);
                    return true;
                }
            }
            else if (player->IsWithinDist3d(capturePosition.GetPositionX(), capturePosition.GetPositionY(), capturePosition.GetPositionZ(), 8.0f))
                return true;

            bool const moved = IssueMovePointThrottled(player, capturePosition, 6.0f, 500);
            if (moved)
            {
                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP flag capture path: guid={} destination=({}, {}, {}).",
                    player->GetGUID().ToString(), capturePosition.GetPositionX(), capturePosition.GetPositionY(),
                    capturePosition.GetPositionZ());
            }
            return moved || player->isMoving();
        }

        // Warsong Gulch and Twin Peaks: the flag this bot was SENT for, not any
        // flag the battleground would let it use (see CtfCoordinator).
        playerbot::CtfBotOrders ctfOrders;
        bool const ctfOrdered = playerbot::CtfCoordinator::GetOrders(player, ctfOrders);
        ObjectGuid const pickupGuid = ctfOrdered ? ctfOrders.pickupGuid : battleground->GetFlagPickupGUID(player->GetGUID());
        if (pickupGuid.IsEmpty())
        {
            std::lock_guard<std::mutex> stateGuard(playerbot::SharedBotStateStructureLock());
            auto const delayItr = g_DroppedFlagPickupDelayByBotGuid.find(botGuidRaw);
            if (delayItr == g_DroppedFlagPickupDelayByBotGuid.end() ||
                GameTime::GetGameTimeMS() >= delayItr->second.pickupNotBeforeMs)
                g_DroppedFlagPickupDelayByBotGuid.erase(botGuidRaw);
            return false;
        }

        GameObject* flag = player->FindMap()->GetGameObject(pickupGuid);
        if (!flag || !flag->IsInWorld())
            return false;

        bool const isDroppedFlag = flag->GetGoType() == GAMEOBJECT_TYPE_FLAGDROP;
        if (isDroppedFlag)
        {
            uint32 const nowMs = GameTime::GetGameTimeMS();
            DroppedFlagPickupDelay& delay = playerbot::LockedGetOrCreate(g_DroppedFlagPickupDelayByBotGuid, botGuidRaw);
            if (delay.flagGuid != pickupGuid)
            {
                delay.flagGuid = pickupGuid;
                // A whisper-requested drop installs its own longer deadline
                // before the dropped GO necessarily exists. Preserve that
                // deadline when the new object GUID becomes visible; ordinary
                // dropped flags retain the short anti-instant-reclick delay.
                if (nowMs >= delay.pickupNotBeforeMs)
                    delay.pickupNotBeforeMs = nowMs +
                        (ctfOrdered && playerbot::CtfCoordinator::IsHandoffReceiverFor(player, pickupGuid)
                            ? PLAYERBOT_HANDOFF_FLAG_PICKUP_DELAY_MS
                            : PLAYERBOT_DROPPED_FLAG_PICKUP_DELAY_MS);
            }

            if (flag->IsAtInteractDistance(player) && nowMs < delay.pickupNotBeforeMs)
                return true;
        }
        else
            playerbot::LockedErase(g_DroppedFlagPickupDelayByBotGuid, botGuidRaw);

        if (!flag->IsAtInteractDistance(player))
            return IssueMovePointThrottled(player, flag->GetPosition(), 6.0f, 500) || player->isMoving();

        // Use the object through the normal interaction path so flag-drop
        // cleanup, form/stealth handling, and battleground eligibility checks
        // remain identical to a player click.
        flag->Use(player);
        playerbot::LockedErase(g_DroppedFlagPickupDelayByBotGuid, botGuidRaw);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP flag pickup attempted: guid={} flag_guid={}.",
            player->GetGUID().ToString(), pickupGuid.ToString());
        return true;
    }

    // True when the straight line between the two points runs through a gate
    // the battleground still has standing. Gates are gameobjects and the
    // navmesh holds none; a gate is what blocks gameobject collision both at
    // chest height and well above the head near one (the same test the spline
    // guard in MoveSplineInit applies to every bot leg).
    bool CrossesStandingGate(Player const* player, GuidVector const& gates, Position const& from, Position const& to)
    {
        Map* map = player->FindMap();
        if (!map || gates.empty())
            return false;

        constexpr float GateReach = 40.0f;
        float const legX = to.GetPositionX() - from.GetPositionX();
        float const legY = to.GetPositionY() - from.GetPositionY();
        float const legLengthSq = legX * legX + legY * legY;
        bool nearGate = false;
        for (ObjectGuid const& gateGuid : gates)
        {
            GameObject const* gate = map->GetGameObject(gateGuid);
            if (!gate)
                continue;

            float const toGateX = gate->GetPositionX() - from.GetPositionX();
            float const toGateY = gate->GetPositionY() - from.GetPositionY();
            float const t = legLengthSq > 0.0f ? std::clamp((toGateX * legX + toGateY * legY) / legLengthSq, 0.0f, 1.0f) : 0.0f;
            float const offX = toGateX - legX * t;
            float const offY = toGateY - legY * t;
            if (offX * offX + offY * offY <= GateReach * GateReach)
            {
                nearGate = true;
                break;
            }
        }

        if (!nearGate)
            return false;

        auto const blockedAt = [&](float height)
        {
            return !map->isInLineOfSight(from.GetPositionX(), from.GetPositionY(), from.GetPositionZ() + height,
                to.GetPositionX(), to.GetPositionY(), to.GetPositionZ() + height, player->GetPhaseMask(),
                LINEOFSIGHT_CHECK_GOBJECT, VMAP::ModelIgnoreFlags::Nothing);
        };
        return blockedAt(1.5f) && blockedAt(4.0f);
    }

    // How many standing gates the walking route the pathfinder gives from the
    // start to the point runs into, a gate counted once however many legs
    // touch it. When the route has to arrive, -1 unless it reaches the point;
    // otherwise whatever route there is counts, and with none the straight
    // line decides.
    int32 CountRouteGateCrossings(Player* player, GuidVector const& gates, Position const& start,
        Position const& destination, bool mustArrive)
    {
        if (gates.empty() && !mustArrive)
            return 0;

        PathGenerator path(player);
        path.CalculatePath(start.GetPositionX(), start.GetPositionY(), start.GetPositionZ(),
            destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
        Movement::PointsArray const& points = path.GetPath();
        if (mustArrive && (points.size() < 2 || (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE |
            PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT | PATHFIND_FARFROMPOLY_END))))
            return -1;

        if (points.size() < 2)
            return CrossesStandingGate(player, gates, start, destination) ? 1 : 0;

        int32 crossings = 0;
        bool atGate = false;
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            Position const from(points[i - 1].x, points[i - 1].y, points[i - 1].z);
            Position const to(points[i].x, points[i].y, points[i].z);
            bool const crosses = CrossesStandingGate(player, gates, from, to);
            if (crosses && !atGate)
                ++crossings;
            atGate = crosses;
        }

        return crossings;
    }

    // Whether the pathfinder gives this bot a route that actually arrives
    // within a few yards of the point and passes no standing gate. A complete
    // route matters for things picked among many (charge piles): one perched
    // on a crate, or across water the mesh does not cross, comes back partial.
    bool IsWalkableRoute(Player* player, GuidVector const& gates, Position const& destination)
    {
        PathGenerator path(player);
        path.CalculatePath(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
        if (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_NOT_USING_PATH | PATHFIND_SHORTCUT |
            PATHFIND_FARFROMPOLY_END))
            return false;

        Movement::PointsArray const& points = path.GetPath();
        if (points.size() < 2)
            return false;

        G3D::Vector3 const& end = points.back();
        if (destination.GetExactDist(end.x, end.y, end.z) > 4.0f)
            return false;

        for (std::size_t i = 1; i < points.size(); ++i)
        {
            Position const from(points[i - 1].x, points[i - 1].y, points[i - 1].z);
            Position const to(points[i].x, points[i].y, points[i].z);
            if (CrossesStandingGate(player, gates, from, to))
                return false;
        }

        return true;
    }

    // How a bot gets past the gates still standing between it and a place. A
    // bot inside a keep (Isle of Conquest, after a respawn) or behind its own
    // walls (Strand of the Ancients' defenders) would otherwise stand at the
    // closed gate forever: the spline guard refuses the route through it. So
    // when its walking route runs into a standing gate, it takes a teleporter,
    // or a ledge to drop off (a drop exit declared with no island), that it can
    // walk to without meeting a gate and that leaves fewer gates on the rest
    // of the way. Behind several walls it gets past them one hop at a time.
    //
    // Both stretches are walking routes. Straight lines, tested only against
    // the gates, took the Defender's Portals for ways out: each stands outside
    // its gate, and the line to it from inside slips past the gate's edge
    // through a wall the test does not see, so defenders walked into their
    // own Mauve Amethyst and Yellow Moon gates on the way to them.
    struct GateBypassRoute
    {
        uint32 decidedMs = 0;
        Position destination;
        // The walking route to the destination runs into a standing gate.
        bool blocked = false;
        ObjectGuid teleporterGuid;
        int32 ledge = -1;

        bool HasWayPast() const { return !teleporterGuid.IsEmpty() || ledge >= 0; }
    };

    std::unordered_map<uint64, GateBypassRoute> g_GateBypassByBot;
    // A route is decided again this often, or as soon as the destination moves.
    constexpr uint32 PLAYERBOT_GATE_BYPASS_RECHECK_MS = 3 * IN_MILLISECONDS;
    constexpr float PLAYERBOT_GATE_BYPASS_REDECIDE_DISTANCE = 15.0f;
    // Each way tried costs up to two path searches, so only the cheapest few
    // are. The cheap ones are often across the very walls in the way:
    // Strand's defenders reach the Green Emerald's front by the fourth.
    constexpr std::size_t PLAYERBOT_GATE_BYPASS_CANDIDATES = 6;

    GateBypassRoute DecideGateBypass(Player* player, Battleground* battleground, Position const& destination)
    {
        GuidVector gates;
        battleground->GetStandingGates(gates);
        if (gates.empty())
            return GateBypassRoute();

        uint64 const botKey = player->GetGUID().GetRawValue();
        uint32 const nowMs = GameTime::GetGameTimeMS();
        GateBypassRoute previous;
        bool const known = playerbot::LockedGetCopy(g_GateBypassByBot, botKey, previous) &&
            previous.destination.GetExactDist2d(&destination) <= PLAYERBOT_GATE_BYPASS_REDECIDE_DISTANCE;
        if (known && nowMs - previous.decidedMs < PLAYERBOT_GATE_BYPASS_RECHECK_MS)
            return previous;

        GateBypassRoute route;
        route.decidedMs = nowMs;
        route.destination = destination;

        int32 const crossings = CountRouteGateCrossings(player, gates, player->GetPosition(), destination, false);
        route.blocked = crossings > 0;
        if (route.blocked)
        {
            struct Way
            {
                float cost;
                ObjectGuid teleporterGuid;
                int32 ledge;
                Position entrance;
                Position exit;
            };
            std::vector<Way> ways;

            std::vector<BattlegroundTeleporter> teleporters;
            battleground->GetTeleporters(player->GetGUID(), teleporters);
            for (BattlegroundTeleporter const& teleporter : teleporters)
                ways.push_back({ player->GetExactDist(&teleporter.Entrance) + teleporter.Exit.GetExactDist(&destination),
                    teleporter.GoGuid, -1, teleporter.Entrance, teleporter.Exit });

            std::vector<BattlegroundDropExit> exits;
            battleground->GetDropExits(player->GetGUID(), exits);
            for (std::size_t i = 0; i < exits.size(); ++i)
                if (exits[i].IslandRadius <= 0.0f)
                    ways.push_back({ player->GetExactDist(&exits[i].Edge) + exits[i].Landing.GetExactDist(&destination),
                        ObjectGuid::Empty, int32(i), exits[i].Edge, exits[i].Landing });

            // Cheapest first, but the way taken last time ahead of the rest
            // while it still holds: two ledges about as cheap would otherwise
            // trade places as the bot walks between them.
            std::sort(ways.begin(), ways.end(), [](Way const& a, Way const& b) { return a.cost < b.cost; });
            if (known && previous.HasWayPast())
                std::stable_partition(ways.begin(), ways.end(), [&previous](Way const& way)
                    { return way.teleporterGuid == previous.teleporterGuid && way.ledge == previous.ledge; });

            std::size_t tried = 0;
            for (Way const& way : ways)
            {
                if (tried++ >= PLAYERBOT_GATE_BYPASS_CANDIDATES)
                    break;
                if (!IsWalkableRoute(player, gates, way.entrance))
                    continue;

                int32 const after = CountRouteGateCrossings(player, gates, way.exit, destination, true);
                if (after < 0 || after >= crossings)
                    continue;

                route.teleporterGuid = way.teleporterGuid;
                route.ledge = way.ledge;
                break;
            }
        }

        playerbot::LockedSet(g_GateBypassByBot, botKey, route);
        return route;
    }

    // Walks to the teleporter DecideGateBypass chose and uses it. Returns
    // true while it owns the bot's movement.
    bool UseBypassTeleporter(Player* player, ObjectGuid const& teleporterGuid)
    {
        uint64 const botKey = player->GetGUID().GetRawValue();
        GameObject* teleporter = player->FindMap() ? player->FindMap()->GetGameObject(teleporterGuid) : nullptr;
        if (!teleporter || !teleporter->IsInWorld() || !teleporter->isSpawned())
        {
            playerbot::LockedErase(g_GateBypassByBot, botKey);
            return false;
        }

        if (!teleporter->IsAtInteractDistance(player))
            return IssueMovePointThrottled(player, teleporter->GetPosition(), 3.0f, 500) || player->isMoving();

        if (player->IsMounted())
            ForcePlayerbotDismount(player);

        StopVirtualPlayerbotMovement(player);
        teleporter->Use(player);
        // Decided afresh from wherever it puts the bot.
        playerbot::LockedErase(g_GateBypassByBot, botKey);
        EmitBattlegroundGmDebug(player, "objective-route=teleporter guid=" + teleporterGuid.ToString(), 1500);
        return true;
    }

    // Put a bot standing at a ledge's edge down at its landing, with the pets
    // and guardians bringAlong picks. A teleport rather than a fall:
    // server-driven splines have no gravity (see TryLeaveDropIsland).
    template <typename BringAlong>
    void SetDownAtDropLanding(Player* player, BattlegroundDropExit const& exit, BringAlong bringAlong)
    {
        // Settle on the real ground under the landing point; the declared
        // height is the navmesh's, which a slope can put a little off.
        float landingZ = exit.Landing.GetPositionZ() + 1.0f;
        player->UpdateAllowedPositionZ(exit.Landing.GetPositionX(), exit.Landing.GetPositionY(), landingZ);
        if (landingZ > exit.Landing.GetPositionZ() + 3.0f || landingZ < exit.Landing.GetPositionZ() - 3.0f)
            landingZ = exit.Landing.GetPositionZ();

        if (MotionMaster* motionMaster = player->GetMotionMaster())
            motionMaster->Clear();
        StopVirtualPlayerbotMovement(player);
        player->NearTeleportTo(exit.Landing.GetPositionX(), exit.Landing.GetPositionY(), landingZ, player->GetOrientation());

        std::vector<Unit*> const controlled(player->m_Controlled.begin(), player->m_Controlled.end());
        for (Unit* unit : controlled)
            if (unit && unit->GetTypeId() != TYPEID_PLAYER && !unit->IsTotem() && unit->IsInWorld() && unit->IsAlive() &&
                unit->GetMapId() == player->GetMapId() && bringAlong(unit))
                unit->NearTeleportTo(exit.Landing.GetPositionX(), exit.Landing.GetPositionY(), landingZ, unit->GetOrientation());

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot put down from a battleground ledge: guid={} edge=({}, {}, {}) landing=({}, {}, {}).",
            player->GetGUID().ToString(), exit.Edge.GetPositionX(), exit.Edge.GetPositionY(),
            exit.Edge.GetPositionZ(), exit.Landing.GetPositionX(), exit.Landing.GetPositionY(), landingZ);
    }

    // A ledge the battleground says players jump off (Eye of the Storm's
    // start platforms). Nothing a bot is sent for is on such an island and the
    // navmesh has no way off it, so a bot standing on one walks to the ledge
    // and is put down on the ground below. A teleport rather than a fall:
    // server-driven splines have no gravity, and the falls tried before
    // (2026-05 direct-drop shortcut, 2026-07 floor-drop probes) floated bots
    // down the air or dove them through floors. Only a declared island ever
    // takes this path. Returns true while it owns the bot's movement.
    bool TryLeaveDropIsland(Player* player, Battleground* battleground)
    {
        std::vector<BattlegroundDropExit> exits;
        battleground->GetDropExits(player->GetGUID(), exits);
        if (exits.empty())
            return false;

        BattlegroundDropExit const* chosen = nullptr;
        float chosenDistance = 0.0f;
        for (BattlegroundDropExit const& exit : exits)
        {
            if (player->GetPositionZ() < exit.IslandMinZ ||
                player->GetExactDist2d(exit.IslandCentre.GetPositionX(), exit.IslandCentre.GetPositionY()) > exit.IslandRadius)
                continue;

            float const distance = player->GetExactDist2d(exit.Edge.GetPositionX(), exit.Edge.GetPositionY());
            if (!chosen || distance < chosenDistance)
            {
                chosen = &exit;
                chosenDistance = distance;
            }
        }

        if (!chosen)
            return false;

        constexpr float EdgeReach = 3.0f;
        if (chosenDistance > EdgeReach)
            return IssueMovePointThrottled(player, chosen->Edge, 2.0f, 500) || player->isMoving();

        // Its pet comes down with it: a pet follows by navmesh too, and has no
        // more way off the island than the bot did.
        SetDownAtDropLanding(player, *chosen, [chosen](Unit const* unit)
        {
            return unit->GetPositionZ() >= chosen->IslandMinZ &&
                unit->GetExactDist2d(chosen->IslandCentre.GetPositionX(), chosen->IslandCentre.GetPositionY()) <= chosen->IslandRadius;
        });
        return true;
    }

    // A ledge to get past one's own gate while it still stands: Strand of the
    // Ancients' defenders start behind every wall they are sent out of, the
    // Defender's Portals only lead back in, and the way out is off the walls
    // by the turrets. Declared like an island exit but with no island
    // (IslandRadius 0), so TryLeaveDropIsland never takes one. The bot walks
    // to the edge of the ledge DecideGateBypass chose and is set down at its
    // landing. Returns true while it owns the bot's movement.
    bool TakeBypassLedge(Player* player, Battleground* battleground, int32 ledge)
    {
        std::vector<BattlegroundDropExit> exits;
        battleground->GetDropExits(player->GetGUID(), exits);
        uint64 const botKey = player->GetGUID().GetRawValue();
        if (std::size_t(ledge) >= exits.size() || exits[ledge].IslandRadius > 0.0f)
        {
            playerbot::LockedErase(g_GateBypassByBot, botKey);
            return false;
        }

        BattlegroundDropExit const& exit = exits[ledge];
        constexpr float EdgeReach = 3.0f;
        if (player->GetExactDist2d(exit.Edge.GetPositionX(), exit.Edge.GetPositionY()) > EdgeReach)
            return IssueMovePointThrottled(player, exit.Edge, 2.0f, 500) || player->isMoving();

        // Decided afresh from wherever it lands.
        playerbot::LockedErase(g_GateBypassByBot, botKey);
        SetDownAtDropLanding(player, exit, [&exit](Unit const* unit)
        {
            return unit->GetExactDist(&exit.Edge) <= 30.0f;
        });
        EmitBattlegroundGmDebug(player, "objective-route=ledge", 1500);
        return true;
    }

    // Objective travel: on foot, or through the teleporter or off the ledge
    // that gets the bot past a gate standing in its way.
    bool MoveToBattlegroundObjective(Player* player, Battleground* battleground, Position const& destination)
    {
        if (battleground)
        {
            GateBypassRoute const route = DecideGateBypass(player, battleground, destination);
            if (!route.teleporterGuid.IsEmpty() && UseBypassTeleporter(player, route.teleporterGuid))
                return true;
            if (route.ledge >= 0 && TakeBypassLedge(player, battleground, route.ledge))
                return true;
        }

        return IssueMovePointThrottled(player, destination, 6.0f, 500);
    }

    // The base the coordinator planned for this bot, or, where it does not
    // plan - a battleground with capturable bases it does not cover, or a bot
    // it has no assignment for - the battleground's own per-bot answer.
    bool ResolveNodeOrders(Player* player, Battleground* battleground, playerbot::NodeBotOrders& orders)
    {
        if (playerbot::NodeCoordinator::GetOrders(player, orders) && orders.hasNode)
            return true;

        BattlegroundNodeObjective objective;
        if (!battleground->GetNodeObjective(player->GetGUID(), objective))
            return false;

        bool const isDefense = objective.Status == BattlegroundNodeStatus::FriendlyControlled ||
            objective.Status == BattlegroundNodeStatus::FriendlyContested ||
            objective.Status == BattlegroundNodeStatus::FriendlyUnderAttack;

        orders = playerbot::NodeBotOrders();
        orders.hasNode = true;
        orders.nodeId = objective.NodeId;
        orders.status = objective.Status;
        orders.location = objective.Location;
        orders.bannerGuid = objective.BannerGuid;
        orders.captureRadius = objective.CaptureRadius;
        orders.role = isDefense ? playerbot::NodeRole::Defend : playerbot::NodeRole::Assault;
        orders.interact = !isDefense || objective.Status == BattlegroundNodeStatus::FriendlyUnderAttack;
        return true;
    }

    // A bot holding a base walks back once a fight has dragged it off it.
    // Pulling the guard away and clicking the banner behind it is the other
    // half of the one-player capture. Only the movement is taken back: spells
    // still go out at whatever followed the bot home.
    bool TryReturnToLeashedNode(Player* player, Battleground* battleground)
    {
        if (!player || !battleground)
            return false;

        playerbot::NodeBotOrders orders;
        if (!playerbot::NodeCoordinator::GetOrders(player, orders) || !orders.hasNode || !orders.leash)
            return false;

        if (player->IsWithinDist3d(orders.location.GetPositionX(), orders.location.GetPositionY(),
            orders.location.GetPositionZ(), orders.leashRange))
            return false;

        Position destination = orders.location;
        playerbot::ApplyDeterministicObjectiveOffset(battleground, player, destination);
        EmitBattlegroundGmDebug(player, "node-leash=return node=" + std::to_string(orders.nodeId), 1500);
        return IssueMovePointThrottled(player, destination, 6.0f, 500) || player->isMoving();
    }

    bool TryAdvanceNodeObjective(Player* player, Battleground* battleground)
    {
        if (!player || !battleground || !player->FindMap())
            return false;

        playerbot::NodeBotOrders orders;
        if (!ResolveNodeOrders(player, battleground, orders))
            return false;

        Position destination = orders.location;

        // A base that is already ours is defended by holding on it and
        // engaging whoever walks up.
        if (!orders.interact)
        {
            playerbot::ApplyDeterministicObjectiveOffset(battleground, player, destination);
            if (!player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f))
                return MoveToBattlegroundObjective(player, battleground, destination) || player->isMoving();

            return true;
        }

        // A base taken by standing on it (Eye of the Storm) has no banner: the
        // bodies inside its radius are the capture. Stand well inside it, so a
        // step back in a fight does not drop the bot off the count.
        if (orders.bannerGuid.IsEmpty() && orders.captureRadius > 0.0f)
        {
            float const holdRadius = std::min(orders.captureRadius * 0.35f, 20.0f);
            playerbot::ApplyDeterministicObjectiveOffset(battleground, player, destination);
            if (!player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), holdRadius))
                return MoveToBattlegroundObjective(player, battleground, destination) || player->isMoving();

            return true;
        }

        GameObject* banner = player->FindMap()->GetGameObject(orders.bannerGuid);
        if (!banner || !banner->IsInWorld() || !banner->isSpawned())
            return true;

        // Node banners use their lock spell (normally the 10-second Opening
        // spell), not GameObject::Use. Use the spell's real interaction range
        // instead of stopping at an arbitrary eight-yard approximation.
        SpellInfo const* interactionSpell = banner->GetSpellForLock(player);
        bool const isAtInteractDistance = interactionSpell ? banner->IsAtInteractDistance(player, interactionSpell) :
            banner->IsAtInteractDistance(player);
        if (!isAtInteractDistance)
        {
            // Walk to the battleground's spot for the base, which is on the
            // ground; the object itself can float well above it (Strand of the
            // Ancients' relic, 7.7 yards over the floor). For a banner the two
            // are the same place.
            bool const atSpot = player->IsWithinDist2d(&orders.location, 2.5f) &&
                std::fabs(player->GetPositionZ() - orders.location.GetPositionZ()) < 4.0f;
            if (!atSpot)
                return MoveToBattlegroundObjective(player, battleground, orders.location) || player->isMoving();

            // On the spot and still outside the lock's reach of a floating
            // object: a goober is used the way its own click would use it. A
            // banner (a button) never gets here - its spot is its foot.
            if (banner->GetGoType() != GAMEOBJECT_TYPE_GOOBER)
                return true;

            if (player->IsMounted())
                ForcePlayerbotDismount(player);
            StopVirtualPlayerbotMovement(player);
            player->AttackStop();
            banner->Use(player);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP node used from its spot: guid={} node={} object={}.",
                player->GetGUID().ToString(), orders.nodeId, banner->GetEntry());
            return true;
        }

        if (playerbot::PvpClassActions::IsBattlegroundObjectInteractionInProgress(player))
            return true;

        // Mount auras prevent the node's interruptible OPEN_LOCK interaction.
        // Dismount only after the bot has reached the banner so travel to the
        // objective can still benefit from mount speed.
        if (player->IsMounted())
            ForcePlayerbotDismount(player);

        StopVirtualPlayerbotMovement(player);
        player->AttackStop();

        SpellCastResult castResult = SPELL_FAILED_DONT_REPORT;
        if (interactionSpell)
            castResult = player->CastSpell(banner, interactionSpell->Id, false);
        else
            banner->Use(player);

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP node interaction attempted: guid={} node={} status={} role={} banner_guid={} spell={} result={} fallback_use={}.",
            player->GetGUID().ToString(), orders.nodeId, static_cast<uint8>(orders.status),
            playerbot::GetNodeRoleName(orders.role), orders.bannerGuid.ToString(),
            interactionSpell ? interactionSpell->Id : 0, static_cast<uint32>(castResult), interactionSpell == nullptr);
        return true;
    }

    std::unordered_map<uint64, uint32> g_WsgReturnAttemptNotBeforeMsByGuid;

    GameObject* GetFriendlyDroppedWsgFlag(Player* player, BattlegroundWS* bgWs)
    {
        if (!player || !bgWs || !player->FindMap())
            return nullptr;

        TeamId const botBgTeam = ResolveBotTeamId(player);
        if (bgWs->GetFlagState(botBgTeam) != BG_WS_FLAG_STATE_ON_GROUND)
            return nullptr;

        ObjectGuid const droppedFlagGuid = bgWs->GetDroppedFlagGUID(botBgTeam);
        if (droppedFlagGuid.IsEmpty())
            return nullptr;

        return player->FindMap()->GetGameObject(droppedFlagGuid);
    }

    bool HumanTeammateNearDroppedFlag(Player* player, GameObject const* droppedFlag, float veryCloseDistance)
    {
        if (!player || !droppedFlag || !player->FindMap())
            return false;

        TeamId const botBgTeam = ResolveBotTeamId(player);
        float const botDistance = player->GetDistance(droppedFlag);

        Map::PlayerList const& players = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* teammate = itr->GetSource();
            if (!teammate || teammate == player || !teammate->IsAlive())
                continue;
            if (teammate->GetBattlegroundId() != player->GetBattlegroundId())
                continue;

            TeamId const teammateBgTeam = ResolveBotTeamId(teammate);
            if (teammateBgTeam != botBgTeam || playerbot::IsManagedRandomBot(teammate))
                continue;

            float const teammateDistance = teammate->GetDistance(droppedFlag);
            if (teammateDistance <= veryCloseDistance && teammateDistance <= botDistance + 1.0f)
                return true;
        }

        return false;
    }



    bool BattlegroundHasAnyRealHumanPlayers(Player const* player)
    {
        if (!player || !player->InBattleground() || !player->FindMap())
            return false;

        uint32 const battlegroundId = player->GetBattlegroundId();
        Map::PlayerList const& players = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player const* participant = itr->GetSource();
            if (!participant || participant->GetBattlegroundId() != battlegroundId)
                continue;

            WorldSession const* session = participant->GetSession();
            bool const isVirtualSession = session && session->IsVirtualSession();
            if (!isVirtualSession && !playerbot::IsManagedRandomBot(participant))
                return true;
        }

        return false;
    }


    bool HasAnyRealHumanInterestInBattleground(BattlegroundTypeId targetBgType)
    {
        if (targetBgType == BATTLEGROUND_TYPE_NONE)
            return false;

        BattlegroundQueueTypeId const targetQueueType = BattlegroundMgr::BGQueueTypeId(targetBgType, 0);

        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, participant] : ObjectAccessor::GetPlayers())
        {
            if (!participant)
                continue;

            WorldSession const* session = participant->GetSession();
            bool const isVirtualSession = session && session->IsVirtualSession();
            if (isVirtualSession || playerbot::IsManagedRandomBot(participant))
                continue;

            if (participant->InBattleground() && participant->GetBattlegroundTypeId() == targetBgType)
                return true;

            for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
            {
                if (participant->GetBattlegroundQueueTypeId(i) == targetQueueType)
                    return true;
            }
        }

        return false;
    }

    bool HasAnyRealHumanPlayerInBattleground(BattlegroundTypeId targetBgType)
    {
        if (targetBgType == BATTLEGROUND_TYPE_NONE)
            return false;

        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, participant] : ObjectAccessor::GetPlayers())
        {
            if (!participant || !participant->InBattleground() || participant->GetBattlegroundTypeId() != targetBgType)
                continue;

            WorldSession const* session = participant->GetSession();
            bool const isVirtualSession = session && session->IsVirtualSession();
            if (!isVirtualSession && !playerbot::IsManagedRandomBot(participant))
                return true;
        }

        return false;
    }

    uint32 QueueEligibleManagedBotsForBattleground(BattlegroundTypeId bgTypeId, uint8 arenaType, bool respectQueueOnlySegments)
    {
        // Every candidate this sweep can find is a persistent, account-backed bot
        // (IsScmManagedBotCandidate), so with those retired from instanced PvP the
        // whole pass has nothing to do. Answered before the roster scan rather than
        // per bot: this runs from each bot's own tick, and a fleet of 178 walking
        // the full player list for each of them is the O(n^2) that filled the log
        // with a thousand lines a second.
        //
        // respectQueueOnlySegments is false only for the GM forcequeue command,
        // which is allowed to reach for them anyway.
        if (respectQueueOnlySegments && !playerbot::PvpCore::GetConfig().pvpPersistentLifecycleEnabled)
            return 0;

        std::vector<ObjectGuid> managedBotGuids;
        {
            std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
            for (auto const& [guid, participant] : ObjectAccessor::GetPlayers())
            {
                if (!participant || !participant->IsInWorld())
                    continue;

                if (!IsScmManagedBotCandidate(participant))
                    continue;

                // Dedicated PvP-only accounts are the cap-level pool and the only
                // one at 60: pre-60 queues use ordinary random bots from the
                // matching level band, and no random-population bot is queued
                // at 60 (the PvP-only characters take part as transient copies).
                if ((participant->GetLevel() < 60) == playerbot::PveManager::IsPvpOnlyBot(participant))
                    continue;

                managedBotGuids.push_back(guid);
            }
        }

        Trinity::Containers::RandomShuffle(managedBotGuids);

        uint32 queuedCount = 0;
        for (ObjectGuid const& guid : managedBotGuids)
        {
            Player* managedBot = ObjectAccessor::FindConnectedPlayer(guid);
            if (!managedBot)
                continue;

            // Mass-queue paths should also normalize stale battleground state for
            // each candidate; otherwise a subset of bots can remain perpetually
            // ineligible until their own lifecycle tick reaches recovery.
            RecoverStaleBattlegroundState(managedBot);

            // Normal lifecycle/refill sweeps must honor account queue segmentation.
            // GM forcequeue intentionally passes respectQueueOnlySegments=false so
            // the command still does exactly what it says: force all eligible
            // managed bots into the caller's current/queued battleground.
            if (respectQueueOnlySegments)
            {
                if (IsArenaOnlyManagedBotAccount(managedBot))
                {
                    EmitLifecycleDiagnostic(managedBot, "queue-skip-arena-only-account",
                        "Skipped battleground mass queue because account is configured arena-only.");
                    continue;
                }

                BattlegroundTypeId const configuredTarget = ResolveManagedBotQueueTargetForAccount(managedBot);
                if (configuredTarget != bgTypeId)
                {
                    EmitLifecycleDiagnostic(managedBot, "queue-skip-segment-mismatch",
                        "Requested bgTypeId=" + std::to_string(uint32(bgTypeId)) +
                        " but account target bgTypeId=" + std::to_string(uint32(configuredTarget)));
                    continue;
                }
            }

            if (QueuePlayer(managedBot, bgTypeId, arenaType, !respectQueueOnlySegments))
                ++queuedCount;
        }

        return queuedCount;
    }

}

namespace playerbot
{
    bool IsHunterAutoShotPlantActive(Player* player)
    {
        return IsHunterAutoShotPlantActiveInternal(player);
    }

    void NotifyHunterAutoShotFired(Player* player)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return;

        uint32 fireSequence = 0;
        {
            std::lock_guard<std::mutex> lock(g_HunterAutoShotEventLock);
            fireSequence = ++g_HunterAutoShotFireSequence[player->GetGUID().GetRawValue()];
        }

        std::ostringstream diag;
        diag << "[AutoShot] fired timer=" << player->getAttackTimer(RANGED_ATTACK)
             << " sequence=" << fireSequence;
        WhisperAutoShotDiagnosticToArena(player, diag.str(), 0);
    }

    uint32 QueueEligibleManagedBotsForBattleground(BattlegroundTypeId bgTypeId, uint8 arenaType)
    {
        return ::QueueEligibleManagedBotsForBattleground(bgTypeId, arenaType, false);
    }

    void ResyncPlayerbotSwimStateForMovementStop(Player* player)
    {
        if (!player || !player->IsInWorld())
            return;

        // Several independent "freeze movement to cast/hold/plant" call sites
        // (StopPlayerbotForStationaryCast, DriveCombatPositioning's hold-band
        // stop, HoldPositionForWand, StopVirtualPlayerbotMovement) can all
        // fire for the same bot on the same or adjacent decision ticks.
        // Collapse their terrain/swim resyncs into one throttled pass per bot
        // so a bot holding position in water is not resampled and rebroadcast
        // several times per real tick.
        static constexpr uint32 kMinResyncIntervalMs = 200;
        static std::unordered_map<uint64, uint32> lastResyncMsByGuid;

        uint64 const guidRaw = player->GetGUID().GetRawValue();
        uint32 const nowMs = GameTime::GetGameTimeMS();
        {
            std::lock_guard<std::mutex> stateGuard(playerbot::SharedBotStateStructureLock());
            uint32& lastResyncMs = lastResyncMsByGuid[guidRaw];
            if (lastResyncMs != 0 && nowMs - lastResyncMs < kMinResyncIntervalMs)
                return;
            lastResyncMs = nowMs;
        }

        // A bot stopped over water usually sits with Z exactly at the liquid
        // surface, because navmesh water polys ride the liquid level. That is
        // simultaneously the knife edge of the IN_WATER/WATER_WALK liquid
        // classification (Map::GetLiquidStatus flips at delta 0) and the
        // "standing on top of the water" visual. When the water is too deep
        // to stand in, sink the bot just below the surface -- the same idiom
        // BuildCollisionSafeDestination applies to movement destinations --
        // so it classifies stably as swimming and renders treading water.
        // In standable water, settle it onto the bottom instead. Never raise
        // the bot: one already below the stable band is left where it is.
        if (Map const* map = player->FindMap(); map && player->IsAlive() && !player->HasAuraType(SPELL_AURA_WATER_WALK))
        {
            LiquidData liquidData{};
            ZLiquidStatus const liquidStatus = map->GetLiquidStatus(player->GetPhaseMask(), player->GetPositionX(),
                player->GetPositionY(), player->GetPositionZ() + 0.5f, MAP_ALL_LIQUIDS, &liquidData, player->GetCollisionHeight());
            if (liquidStatus & (LIQUID_MAP_WATER_WALK | LIQUID_MAP_IN_WATER | LIQUID_MAP_UNDER_WATER))
            {
                bool const tooDeepToStand = (liquidData.level - liquidData.depth_level) > player->GetCollisionHeight();
                float const stableZ = tooDeepToStand ? liquidData.level - 0.5f : liquidData.depth_level + 0.05f;
                if (player->GetPositionZ() - stableZ > 0.05f)
                    player->UpdatePosition(player->GetPositionX(), player->GetPositionY(), stableZ, player->GetOrientation());
            }
        }

        player->UpdatePositionData();
        // No MSG_MOVE_HEARTBEAT here: observer clients render bots from
        // splines, and a heartbeat kicks them into a client-movement stream
        // that never follows (sporadic porting). The swim toggle above
        // (UpdatePositionData -> ProcessTerrainStatusUpdate -> SetSwim)
        // already publishes SMSG_SPLINE_MOVE_START/STOP_SWIM, and the small
        // stabilization Z-sink reaches observers with the next spline launch.
    }

    void FinalizeManagedBotTeleportIfPending(Player* player)
    {
        if (!player)
            return;

        if (!playerbot::IsManagedRandomBot(player))
            return;

        WorldSession* session = player->GetSession();
        if (!session || !session->IsVirtualSession())
            return;

        if (player->IsBeingTeleportedFar())
            session->HandleMoveWorldportAck();

        if (!player->IsBeingTeleportedNear())
            return;

        WorldPacket teleportAck(MSG_MOVE_TELEPORT_ACK, 20);
        teleportAck << player->GetPackGUID();
        teleportAck << uint32(0);
        teleportAck << uint32(0);
        session->HandleMoveTeleportAck(teleportAck);

        if (!player->IsBeingTeleportedNear())
            return;

        uint32 const oldZone = player->GetZoneId();
        WorldLocation destination = player->GetTeleportDest();
        float safeDestinationZ = destination.GetPositionZ();
        player->UpdateAllowedPositionZ(destination.GetPositionX(), destination.GetPositionY(), safeDestinationZ);
        destination.Relocate(destination.GetPositionX(), destination.GetPositionY(), safeDestinationZ, destination.GetOrientation());
        player->SetSemaphoreTeleportNear(false);
        player->UpdatePosition(destination, true);

        uint32 newZone = 0;
        uint32 newArea = 0;
        player->GetZoneAndAreaId(newZone, newArea);
        player->UpdateZone(newZone, newArea);

        if (oldZone != newZone)
        {
            if (player->pvpInfo.IsHostile)
                player->CastSpell(player, 2479, true);
            else if (player->IsPvP() && !player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
                player->UpdatePvP(false, false);
        }

        player->ResummonPetTemporaryUnSummonedIfAny();
        player->ProcessDelayedOperations();
    }

    uint64 BuildBattlegroundInstanceKey(Battleground const* battleground)
    {
        if (!battleground)
            return 0;

        return (uint64(battleground->GetMapId()) << 32) | uint64(battleground->GetInstanceID());
    }

    bool ShouldDeferBattlegroundLeaveForTeleportAck(Player const* player)
    {
        if (!player)
            return false;

        if (!player->IsBeingTeleportedFar() && !player->IsBeingTeleportedNear())
            return false;

        // Managed bots can retain stale near/far teleport semaphores inside
        // battleground instances; do not deadlock leave/end cleanup on those flags.
        if (player->InBattleground() && playerbot::IsManagedRandomBot(player))
            return false;

        WorldSession const* session = player->GetSession();
        if (session && session->IsVirtualSession() && player->InBattleground())
            return false;

        return true;
    }

    // On a lift that is carrying it from one stop to the next (GameObject::
    // BoardLiftRiders): Ring of Valor's elevators, which take both teams up
    // from their start shafts when the gates open.
    bool IsRidingLift(Player const* player)
    {
        if (!player->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) || !player->m_movementInfo.transport.guid.IsGameObject())
            return false;

        Map* map = player->FindMap();
        GameObject const* lift = map ? map->GetGameObject(player->m_movementInfo.transport.guid) : nullptr;
        return lift && lift->IsCarryingLiftRider(player->GetGUID());
    }

    // Behind a closed gate: every bot before a battleground starts, and, mid-
    // match, the bots a battleground is preparing behind a gate of its own
    // (Violet Hold's next wave in its cells). A gate is a game object, which
    // the navmesh does not know about, so a bot that is handed a destination
    // on the far side - a teammate to buff in the next cell, an enemy to
    // chase - paths straight through it. The movement lock is what holds it.
    //
    // A bot riding a lift is held the same way until the lift stops. The
    // navmesh has no deck under it either: the move the gates opening hands it
    // is drawn from the bottom of the shaft, and walking it would take the bot
    // off the lift that is carrying it.
    bool IsHeldBehindGate(Battleground const* battleground, Player const* player)
    {
        if (!battleground)
            return false;

        if (IsRidingLift(player))
            return true;

        switch (battleground->GetStatus())
        {
            case STATUS_WAIT_JOIN:
                return true;
            case STATUS_IN_PROGRESS:
                return battleground->OwnsPreparationState(player);
            default:
                return false;
        }
    }

    std::unordered_set<uint64> g_WaitJoinLockedBots;

    void SetWaitJoinMovementLock(Player* player, bool locked)
    {
        if (!player)
            return;

        uint64 const botGuid = player->GetGUID().GetRawValue();
        bool const alreadyLocked = playerbot::LockedContains(g_WaitJoinLockedBots, botGuid);

        if (!locked)
        {
            if (alreadyLocked)
            {
                player->SetControlled(false, UNIT_STATE_ROOT);
                playerbot::LockedErase(g_WaitJoinLockedBots, botGuid);
            }
            return;
        }

        player->AttackStop();
        player->SetSelection(ObjectGuid::Empty);

        if (player->isMoving())
            player->StopMoving();

        if (MotionMaster* motionMaster = player->GetMotionMaster())
            motionMaster->Clear();

        // Re-assert the root, not only the bookkeeping. UNIT_STATE_ROOT is one
        // bit shared with root auras: one of those ending, or Battleground::
        // ClearCombatControl, clears it, and a bot this set still called locked
        // would then walk free for the rest of the hold.
        if (!alreadyLocked || !player->HasUnitState(UNIT_STATE_ROOT))
        {
            player->SetControlled(true, UNIT_STATE_ROOT);
            playerbot::LockedInsert(g_WaitJoinLockedBots, botGuid);
        }
    }

    bool ForceHoldPlayerAtStartDuringWaitJoin(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        Battleground* battleground = player->GetBattleground();
        if (!battleground)
            return false;

        if (!IsHeldBehindGate(battleground, player))
        {
            SetWaitJoinMovementLock(player, false);
            return false;
        }

        SetWaitJoinMovementLock(player, true);

        if (player->isMoving())
            player->StopMoving();

        if (MotionMaster* motionMaster = player->GetMotionMaster())
            motionMaster->Clear();

        return true;
    }

    bool TryReturnDroppedFriendlyFlagWithHumanPriority(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;
        if (!CanIssueBotMovement(player))
            return false;

        BattlegroundWS* bgWs = dynamic_cast<BattlegroundWS*>(player->GetBattleground());
        if (!bgWs || bgWs->GetStatus() != STATUS_IN_PROGRESS)
            return false;

        GameObject* droppedFlag = GetFriendlyDroppedWsgFlag(player, bgWs);
        if (!droppedFlag)
            return false;

        uint64 const botRawGuid = player->GetGUID().GetRawValue();
        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32& attemptNotBeforeMs = playerbot::LockedGetOrCreate(g_WsgReturnAttemptNotBeforeMsByGuid, botRawGuid);

        if (HumanTeammateNearDroppedFlag(player, droppedFlag, 7.0f))
        {
            attemptNotBeforeMs = nowMs + urand(700, 1300);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP WSG return yielded: guid={} reason=nearby-human-priority wait_until_ms={}.",
                player->GetGUID().ToString(), attemptNotBeforeMs);
            return false;
        }

        if (attemptNotBeforeMs == 0)
        {
            attemptNotBeforeMs = nowMs + urand(350, 900);
            return false;
        }

        if (nowMs < attemptNotBeforeMs)
            return false;

        if (!player->IsWithinDistInMap(droppedFlag, 10.0f))
        {
            return IssueMovePointThrottled(player, droppedFlag->GetPosition(), 8.0f, 500);
        }

        bgWs->EventPlayerClickedOnFlag(player, droppedFlag);
        attemptNotBeforeMs = nowMs + urand(1200, 2200);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP WSG return attempted: guid={} flag_guid={} next_attempt_ms={}.",
            player->GetGUID().ToString(), droppedFlag->GetGUID().ToString(), attemptNotBeforeMs);
        return true;
    }

    Player* FindNearestEnemyBattlegroundPlayer(Player* player, float maxDistance, uint32* scannedPlayers, uint32* attackableEnemies)
    {
        if (!player || !player->InBattleground() || !player->FindMap())
            return nullptr;

        Battleground* battleground = player->GetBattleground();
        if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
            return nullptr;

        TeamId const playerBgTeam = ResolveBotTeamId(player);
        if (scannedPlayers)
            *scannedPlayers = 0;
        if (attackableEnemies)
            *attackableEnemies = 0;

        float nearestDistance = std::numeric_limits<float>::max();
        Player* nearestEnemy = nullptr;

        Map::PlayerList const& players = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!candidate || candidate == player || !candidate->IsAlive())
                continue;
            if (scannedPlayers)
                ++(*scannedPlayers);
            if (candidate->GetBattlegroundId() != player->GetBattlegroundId())
                continue;
            TeamId const candidateBgTeam = ResolveBotTeamId(candidate);
            // A squad match seats two squads a side, and the other one on this
            // side is as much an enemy as the other side's.
            if (battleground->IsSquadMatch() ? battleground->AreTeammates(player->GetGUID(), candidate->GetGUID()) :
                candidateBgTeam == playerBgTeam)
                continue;
            if (!player->IsValidAttackTarget(candidate))
                continue;
            if (playerbot::PvpCore::IsEffectivelyImmuneTarget(player, candidate))
                continue;
            if (attackableEnemies)
                ++(*attackableEnemies);

            float const distance = player->GetDistance(candidate);
            if (distance > maxDistance || distance >= nearestDistance)
                continue;

            nearestDistance = distance;
            nearestEnemy = candidate;
        }

        return nearestEnemy;
    }

    // Who a bot holding a base should be fighting: the enemy it is already on,
    // for as long as he stays inside pursueRange of the banner, otherwise the
    // nearest one standing inside engageRange of it. Measured from the banner,
    // not from the bot, because the leash is - and a stealthed enemy is left
    // alone, as the coordinator leaves him out of the threat count.
    Player* FindEnemyOnHeldGround(Player* player, playerbot::NodeBotOrders const& orders)
    {
        if (!player || !player->FindMap() || !orders.hasNode || !orders.leash)
            return nullptr;

        Battleground* battleground = player->GetBattleground();
        if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
            return nullptr;

        Position const& banner = orders.location;
        TeamId const playerBgTeam = ResolveBotTeamId(player);
        auto const isFightableEnemy = [&](Player* candidate) -> bool
        {
            return candidate && candidate != player && candidate->IsAlive() &&
                candidate->GetBattlegroundId() == player->GetBattlegroundId() &&
                ResolveBotTeamId(candidate) != playerBgTeam &&
                player->IsValidAttackTarget(candidate) && player->CanSeeOrDetect(candidate) &&
                !playerbot::PvpCore::IsEffectivelyImmuneTarget(player, candidate);
        };

        if (Unit* victim = player->GetVictim())
            if (Player* victimPlayer = victim->ToPlayer(); isFightableEnemy(victimPlayer) &&
                victimPlayer->IsWithinDist3d(banner.GetPositionX(), banner.GetPositionY(), banner.GetPositionZ(),
                    orders.pursueRange))
                return victimPlayer;

        float nearestDistance = std::numeric_limits<float>::max();
        Player* nearestEnemy = nullptr;
        Map::PlayerList const& players = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!isFightableEnemy(candidate))
                continue;
            if (!candidate->IsWithinDist3d(banner.GetPositionX(), banner.GetPositionY(), banner.GetPositionZ(),
                orders.engageRange))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance >= nearestDistance)
                continue;

            nearestDistance = distance;
            nearestEnemy = candidate;
        }

        return nearestEnemy;
    }

    void StopDamageAgainstEffectivelyImmuneTarget(Player* player, Unit* target)
    {
        if (!player || !target)
            return;

        auto interruptSpellTargeting = [target](Unit* attacker, CurrentSpellTypes spellType)
        {
            Spell const* spell = attacker ? attacker->GetCurrentSpell(spellType) : nullptr;
            if (spell && spell->m_targets.GetUnitTargetGUID() == target->GetGUID())
                attacker->InterruptSpell(spellType);
        };

        if (player->GetVictim() && player->GetVictim()->GetGUID() == target->GetGUID())
            player->AttackStop();

        interruptSpellTargeting(player, CURRENT_GENERIC_SPELL);
        interruptSpellTargeting(player, CURRENT_CHANNELED_SPELL);
        interruptSpellTargeting(player, CURRENT_AUTOREPEAT_SPELL);

        if (Pet* pet = player->GetPet())
        {
            if (pet->GetVictim() && pet->GetVictim()->GetGUID() == target->GetGUID())
                pet->AttackStop();
            interruptSpellTargeting(pet, CURRENT_GENERIC_SPELL);
            interruptSpellTargeting(pet, CURRENT_CHANNELED_SPELL);
        }

        if (player->GetTarget() == target->GetGUID())
            player->SetSelection(ObjectGuid::Empty);

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP stopped damage against effectively immune target: bot={} target={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString());
    }

    Unit* AcquireCombatTarget(Player* player, float scanDistance)
    {
        if (!player)
            return nullptr;

        auto isAttackableTarget = [player](Unit* candidate) -> bool
        {
            return candidate && candidate->IsAlive() && player->IsValidAttackTarget(candidate) &&
                !playerbot::PvpCore::IsEffectivelyImmuneTarget(player, candidate);
        };

        Unit* target = player->GetVictim();
        if (target && playerbot::PvpCore::IsEffectivelyImmuneTarget(player, target))
        {
            StopDamageAgainstEffectivelyImmuneTarget(player, target);
            target = nullptr;
        }
        if (!isAttackableTarget(target))
            target = player->GetSelectedUnit();
        if (target && playerbot::PvpCore::IsEffectivelyImmuneTarget(player, target))
        {
            StopDamageAgainstEffectivelyImmuneTarget(player, target);
            target = nullptr;
        }
        if (Pet* pet = player->GetPet())
            if (Unit* petVictim = pet->GetVictim(); petVictim && playerbot::PvpCore::IsEffectivelyImmuneTarget(player, petVictim))
                StopDamageAgainstEffectivelyImmuneTarget(player, petVictim);
        if ((!target || !target->IsAlive()) && player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS)
        {
            Unit* duelOpponent = player->duel->Opponent;
            if (isAttackableTarget(duelOpponent) && duelOpponent->GetMapId() == player->GetMapId())
                target = duelOpponent;
        }
        if (!isAttackableTarget(target) && player->InBattleground())
            target = FindNearestEnemyBattlegroundPlayer(player, scanDistance, nullptr, nullptr);
        if (!isAttackableTarget(target))
            return nullptr;

        player->SetSelection(target->GetGUID());
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP chosen combat target: bot={} target={} class={} distance={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString(), uint32(player->GetClass()), player->GetDistance(target));
        return target;
    }

    bool DriveCombatPositioning(Player* player, Unit* target, CombatPositioningProfile const& profile)
    {
        if (!player || !target || !target->IsAlive())
            return false;

        // Leaving a burning floor tile outranks every combat consideration.
        // The stop-for-cast paths only run TryMoveOutOfHazardousLiquid when a
        // bot is halting; a bot that is meleeing (or holding a recent chase
        // order) never hit them and simply stood in hazardous ground taking
        // periodic damage. Drive the escape here, before the
        // movement gate and recent-order preservation below, so an in-combat
        // bot actually walks out of the hazard.
        if (TryMoveOutOfHazardousLiquid(player))
            return true;

        if (TryDrivePlayerbotEscapeAura(player, target))
            return true;

        // Cast-time hunter actions such as Aimed Shot and Revive Pet must win
        // over every movement system. Check this before CanIssueBotMovement(),
        // recent-order preservation, LOS recovery, or distance-band movement so
        // a stutter/flee tick cannot clip the cast.
        if (HoldHunterStationaryCast(player, target, "drive-combat-positioning"))
            return true;

        if (!CanIssueBotMovement(player))
            return false;

        // Hunter movement is handled by DriveHunterKiteLoop below. Do not issue
        // an early generic flee here; re-issuing ad-hoc destinations every tick
        // was one cause of visible triangle/orbit movement.
        if (player->GetClass() == CLASS_HUNTER)
        {
            playerbot::HunterAutoShotRangeInfo rangeInfo;
            bool const hasHunterRange = GetHunterAutoShotRange(player, target, rangeInfo);
            float const exactDistance = hasHunterRange ? rangeInfo.exactDistance : player->GetExactDist(target);
            bool const targetPressuringHunter = target->GetVictim() == player || player->IsWithinMeleeRange(target);
            bool const inDeadZone = hasHunterRange && exactDistance > rangeInfo.meleeRange && exactDistance < rangeInfo.minRange;
            if ((targetPressuringHunter && exactDistance < 12.0f) || inDeadZone)
                MarkHunterKiteHold(player);
        }

        // Class spell actions can issue target-relative chase/follow in the same
        // scheduler frame. Do not immediately override those orders from lifecycle
        // distance-band helpers (follow/stop), or bots can visibly inch/stop.
        // Hunter exception: stale ranged approach orders must not preserve an
        // inward chase after the hunter has escaped into Auto Shot range or is
        // still inside a kite/dead-zone escape window.
        if (playerbot::PvpClassActions::HasRecentTargetRelativeMovementOrder(player, nullptr, 1500))
        {
            if (player->GetClass() != CLASS_HUNTER)
                return true;

            playerbot::HunterAutoShotRangeInfo rangeInfo;
            bool const hasHunterRange = GetHunterAutoShotRange(player, target, rangeInfo);
            float const exactDistance = rangeInfo.exactDistance;
            bool const safeAutoShotBand = hasHunterRange && player->IsWithinLOSInMap(target) &&
                exactDistance > rangeInfo.minRange + playerbot::PLAYERBOT_HUNTER_AUTOSHOT_MIN_SAFETY_MARGIN && exactDistance <= rangeInfo.maxRange;
            bool const shouldBreakRecentOrder = safeAutoShotBand || IsHunterKiteHoldActive(player) || exactDistance < 12.0f;
            if (!shouldBreakRecentOrder)
                return true;
        }

        float const distance = player->GetDistance(target);
        bool const hasLos = player->IsWithinLOSInMap(target);
        if (!hasLos)
            return TryRecoverLineOfSight(player, target, profile, "drive-combat-positioning");

        if (HoldPositionForWand(player, target, "combat_positioning_hold_for_wand"))
            return true;

        if (profile.primarilyRanged)
        {
            if (ShouldForceMeleeFallbackOnLowMana(player) && player->GetClass() != CLASS_HUNTER)
            {
                // MoveTowardUnit gates internally; see MoveAwayFromUnit above.
                return MoveTowardUnit(player, target, std::max(1.0f, playerbot::PvpCore::GetConfig().meleeRange - 1.0f));
            }

            if (player->GetClass() == CLASS_HUNTER && DriveHunterKiteLoop(player, target, profile))
                return true;

            if (profile.createDistanceWhenCrowded && IsActivelyPressuringInMelee(target, player) && distance < profile.preferredIdealRange)
            {
                if (player->GetClass() == CLASS_HUNTER)
                    MarkHunterKiteHold(player);

                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP distance band: bot={} profile={} decision=create-distance-vs-melee-pressure distance={} min={} ideal={} max={}.",
                    player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                    profile.preferredMaxPressureRange);
                return MoveAwayFromUnit(player, target, player->GetClass() == CLASS_HUNTER ? std::min(profile.preferredIdealRange, 16.0f) : profile.preferredIdealRange);
            }

            if (distance < profile.preferredMinRange && profile.createDistanceWhenCrowded)
            {
                if (player->GetClass() == CLASS_HUNTER)
                    MarkHunterKiteHold(player);

                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP distance band: bot={} profile={} decision=create-distance distance={} min={} ideal={} max={}.",
                    player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                    profile.preferredMaxPressureRange);
                return MoveAwayFromUnit(player, target, player->GetClass() == CLASS_HUNTER ? std::min(profile.preferredIdealRange, 16.0f) : profile.preferredIdealRange);
            }

            if (distance > profile.preferredMaxPressureRange)
            {
                // A hunter with valid Auto Shot range data is fully owned by
                // DriveHunterKiteLoop above. Reaching this generic fallback
                // means range metadata was unavailable, so do not invent a
                // second stop/start policy that can fight the hunter loop.
                if (player->GetClass() == CLASS_HUNTER)
                    return true;

                // IssueHumanLikeFollow gates internally; see MoveAwayFromUnit above.
                if (!IssueHumanLikeFollow(player, target, profile.preferredIdealRange, 6.0f, 500))
                    return true;
                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP distance band: bot={} profile={} decision=close-distance distance={} min={} ideal={} max={}.",
                    player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                    profile.preferredMaxPressureRange);
                return true;
            }

            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP distance band: bot={} profile={} decision=hold-band distance={} min={} ideal={} max={}.",
                player->GetGUID().ToString(), profile.label, distance, profile.preferredMinRange, profile.preferredIdealRange,
                profile.preferredMaxPressureRange);

            // A hunter with usable range metadata was already handled by
            // DriveHunterKiteLoop. Avoid a second in-band Auto Shot owner here.
            if (player->GetClass() == CLASS_HUNTER)
                return true;

            player->StopMoving();
            if (WorldSession* session = player->GetSession(); session && (session->IsVirtualSession() || session->IsTransientPlayerSession()))
            {
                player->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
                // Throttled/shared swim-state resync; see
                // ResyncPlayerbotSwimStateForMovementStop for the full rationale.
                playerbot::ResyncPlayerbotSwimStateForMovementStop(player);
            }
            return true;
        }

        // A rogue whose stealth opener is Garrote - Asphyxiate, or too young
        // for Cheap Shot; PvpCore::GetRogueStealthOpenerSpellId decides, the
        // same answer the class selector uses.  The normal melee distance-band
        // code intentionally does not move a bot that is already in melee
        // range, but Garrote cannot be used while the target still has the
        // rogue in its frontal arc.  Relying exclusively on the class
        // cast-failure recovery leaves a deadlock if that class tick is delayed
        // or rejected: the hidden rogue has no victim and the enemy cannot
        // detect it, so both units simply stand still.  Keep the lifecycle
        // moving toward the required rear arc as a generic rogue opener
        // invariant; the class action will cast Garrote once it arrives.
        bool const isAssassinationStealthOpener =
            player->GetClass() == CLASS_ROGUE &&
            player->HasStealthAura() &&
            playerbot::PvpCore::GetRogueStealthOpenerSpellId(player) == 703;
        if (isAssassinationStealthOpener &&
            player->IsWithinMeleeRange(target) &&
            target->HasInArc(static_cast<float>(M_PI), player))
        {
            // The tactical loop runs every 50 ms. Replacing an angled Follow
            // generator on every tick repeatedly initializes it (which calls
            // StopMoving) and can overwrite the class action's rear-follow.
            // Preserve the existing order long enough to initialize and move.
            if (!CanIssueMovementCommand(player, 500))
                return true;

            ClearEatDrinkAurasForMovement(player);
            player->GetMotionMaster()->MoveFollow(target, 1.5f, static_cast<float>(M_PI));
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP distance band: bot={} target={} profile={} decision=assassination-opener-move-behind distance={}.",
                player->GetGUID().ToString(), target->GetGUID().ToString(), profile.label, distance);
            return true;
        }

        if (distance > profile.preferredMaxPressureRange || !player->IsWithinMeleeRange(target))
        {
            bool const isStealthedMeleeOpener = IsStealthedMeleeOpener(player);

            // Chase/Follow continuously repaths against a moving target. Do
            // not replace an already-running generator on the lifecycle's
            // 500 ms command cadence: MoveSplineInit synchronizes the server
            // to the old spline before launching the replacement, while an
            // observer may still be interpolating the prior packet. That
            // discrepancy renders as a sporadic forward teleport.
            if (HasActiveTargetRelativeMovementFor(player, target))
                return true;

            if (!isStealthedMeleeOpener && !CanIssueMovementCommand(player, 500))
                return true;

            ClearEatDrinkAurasForMovement(player);

            if (isStealthedMeleeOpener)
            {
                // Do not continually replace the same Follow generator. Each
                // replacement initializes with StopMoving and can prevent the
                // current target-relative spline from making progress.
                if (!CanIssueMovementCommand(player, 500))
                    return true;

                // Stealth openers intentionally run without a committed victim for
                // part of the engage. MoveChase can pause when victim linkage is
                // absent, so use follow semantics to keep continuous closing.
                // Feral druids get a small follow range instead of rogue's near-zero
                // range; the tiny range can cause visible pause/repath in BG maps.
                float const stealthFollowRange = player->GetClass() == CLASS_DRUID
                    ? (distance > 8.0f ? 3.0f : 1.5f)
                    : 0.1f;

                player->GetMotionMaster()->MoveFollow(target, stealthFollowRange, player->GetFollowAngle());
                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP distance band: bot={} profile={} decision=stealth-melee-close-follow distance={} max={} followRange={}.",
                    player->GetGUID().ToString(), profile.label, distance, profile.preferredMaxPressureRange, stealthFollowRange);
                return true;
            }

            // Use core chase movement for non-stealth melee stickiness instead of
            // repeatedly recomputing follow points around the target.
            player->GetMotionMaster()->MoveChase(target);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP distance band: bot={} profile={} decision=melee-close distance={} max={}.",
                player->GetGUID().ToString(), profile.label, distance, profile.preferredMaxPressureRange);
            return true;
        }

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP distance band: bot={} profile={} decision=melee-stick distance={} max={}.",
            player->GetGUID().ToString(), profile.label, distance, profile.preferredMaxPressureRange);
        return true;
    }

    // TEMPORARY, narrowly-scoped debugging aid for the Auto Shot investigation.
    // It reaches opted-in playerbot diagnostic observers in the same
    // battleground/arena instance without requiring active GM mode.
    // Remove once Auto Shot is confirmed fixed.
    std::unordered_map<uint64, uint32> g_AutoShotDiagLastMsByGuid;

    void WhisperAutoShotDiagnosticToArena(Player* player, std::string const& message, uint32 throttleMs = 0)
    {
        if (!player || !player->InBattleground())
            return;

        if (throttleMs)
        {
            uint32 const nowMs = GameTime::GetGameTimeMS();
            uint32& lastMs = playerbot::LockedGetOrCreate(g_AutoShotDiagLastMsByGuid, player->GetGUID().GetRawValue());
            if (lastMs && nowMs < lastMs + throttleMs)
                return;
            lastMs = nowMs;
        }

        Map* map = player->FindMap();
        if (!map)
            return;

        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        {
            Player* observer = itr->GetSource();
            bool const observerIsPlayerbot = observer &&
                (playerbot::IsManagedRandomBot(observer) || playerbot::PlayerbotObcCloneManager::IsActiveClone(observer));
            if (!observer || observer == player || observerIsPlayerbot ||
                observer->GetBattlegroundId() != player->GetBattlegroundId() ||
                !WantsPlayerbotDiagnostics(observer))
                continue;

            player->Whisper(message, LANG_UNIVERSAL, observer);
        }
    }

    bool EngageSelectedEnemyPlayer(Player* player, Unit* target, char const* reason)
    {
        if (!player || !player->IsAlive() || !target || !target->IsAlive() || !player->IsValidAttackTarget(target))
            return false;

        if (playerbot::PvpCore::IsEffectivelyImmuneTarget(player, target))
        {
            StopDamageAgainstEffectivelyImmuneTarget(player, target);
            target = player->InBattleground()
                ? FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max(), nullptr, nullptr)
                : nullptr;
            if (!target)
                return false;
        }

        if (IsCrowdControlledForAction(player))
        {
            player->AttackStop();
            return false;
        }

        if (TryDrivePlayerbotEscapeAura(player, target))
            return true;

        player->SetSelection(target->GetGUID());

        // Cast-time hunter actions such as Aimed Shot must be protected before
        // the generic engagement code calls Attack() or starts Auto Shot.
        // Previously the lifecycle reached DriveCombatPositioning() and held the
        // hunter still, but EngageSelectedEnemyPlayer() had already called
        // Attack()/CastSpell(75), which could queue Auto Shot while Aimed Shot
        // was still preparing.
        if (HoldHunterStationaryCast(player, target, "engage-selected-enemy"))
            return true;

        // A map-wide objective (most visibly an enemy flag carrier) can select
        // a target well beyond the range used by mount selection. Keep using
        // mount speed for that travel instead of dismounting merely because a
        // distant target exists; otherwise the mount selector sees a clear
        // 100-yard area and remounts on the next class tick forever.
        if (player->IsMounted() &&
            !player->IsWithinDistInMap(target, playerbot::PLAYERBOT_MOUNT_ENEMY_AWARENESS_RANGE))
        {
            player->AttackStop();
            return MoveTowardUnit(player, target, playerbot::PLAYERBOT_MOUNT_ENEMY_AWARENESS_RANGE) || player->isMoving();
        }

        // Inside the envelope, stay mounted until the target is actually in
        // reach - the same rule the PvE tick uses, and now literally the same
        // function.
        //
        // This used to dismount the moment the bot came inside a hundred yards,
        // which meant it walked the last hundred yards of every engagement. The
        // guard above still keeps it riding beyond that range, so nothing about
        // long-distance travel changes; what changes is that the last stretch is
        // ridden too. The remount loop the guard above warns about cannot start
        // here, because the mount selector is already suppressed by an attackable
        // enemy player inside this same envelope.
        //
        // Tested before any movement is chosen, because if the target is already
        // in reach then riding is over and the positioning below is correct.
        playerbot::PvpClassActions::DismountToFight(player, target);

        // Still mounted means it is NOT in reach yet, so ride the last stretch -
        // but ride it with point movement, not the chase the positioning code
        // below would install.
        //
        // I claimed in the commit that installed this rule that leaving a bot
        // mounted while it closed was free, because DriveCombatPositioning moves
        // it independently of Attack(). That is true for ranged and objective
        // movement, which are point- and follow-based, and FALSE for melee.
        // The melee close is MoveChase, and ChaseMovementGenerator pauses itself
        // whenever owner->GetVictim() is not the target - while Unit::Attack
        // refuses outright for a mounted player. So a mounted melee bot has no
        // victim, and the chase it was just handed calls StopMoving on every
        // update: parked, mounted, motionless, until something puts it in combat.
        //
        // MoveTowardUnit has no such test, which is exactly why the long-distance
        // guard above has always worked. The stop distance is deliberately inside
        // melee range: it is never actually reached, because DismountToFight fires
        // first on the way in - at shoot range for a hunter, melee range for
        // everyone else - and the tick after that this branch is skipped.
        if (player->IsMounted())
        {
            player->AttackStop();
            return MoveTowardUnit(player, target, 3.0f) || player->isMoving();
        }

        // Playerbots run tactical/lifecycle engagement every fast tick. That
        // loop can select the same enemy immediately after a class gap-closer
        // cast (Charge, Intercept, Rehgar's Fury, etc.) and call Attack() below
        // before the EFFECT_MOTION_TYPE spline has delivered MovementInform.
        // Attack() may install normal chase movement for virtual sessions,
        // replacing the still-active leap/charge generator. Let the effect
        // movement finish first; the class action code will start melee after
        // the spline lands.
        if (HasPlayerbotGapCloserInFlight(player))
        {
            EmitRehgarMovementGuardServerDiagnostic(player, "engage_blocked_effect");
            return true;
        }

        CombatPositioningProfile const profile = GetCombatPositioningProfile(player);
        // "Melee fallback acceptable" means a ranged profile may fight back once
        // an enemy has actually reached melee. It must not make a hunter begin
        // a melee chase from its 10-yard Mongoose weave position, because that
        // active chase generator immediately breaks the Auto Shot plant window.
        // A wand is a ranged AUTO-ATTACK, and the two auto-attacks are mutually
        // exclusive: a real client never has both running, because starting one
        // cancels the other. Nothing in the core enforces that - Unit::Attack
        // sets UNIT_STATE_MELEE_ATTACKING and the auto-repeat spell lives in a
        // separate slot - so a caster whose enemy walked into melee range ended
        // up swinging its staff AND firing its wand at the same target.
        //
        // The wand wins. It is what the decision engine deliberately reached for
        // (out of mana, or as the filler when nothing better is up), and a cloth
        // caster's melee swing is worth a fraction of it. Passing false through
        // the existing Attack call below is what actually stops the swinging:
        // on a target already being attacked it clears the melee state and sends
        // SendMeleeAttackStop.
        bool const wandFiring = HasActiveWandAutoRepeat(player);
        bool const useMeleeAttack = !wandFiring &&
            (!profile.primarilyRanged ||
             (profile.meleeFallbackAcceptable && player->IsWithinMeleeRange(target)));
        bool const isStealthedMeleeOpener = IsStealthedMeleeOpener(player);
        bool const targetInBreakableCrowdControl = playerbot::PvpCore::HasBreakableCrowdControlFor(player, target);
        bool const alreadyAttackingTarget = player->GetVictim() && player->GetVictim()->GetGUID() == target->GetGUID();
        bool const meleeAutoAttackActive = player->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
        if (isStealthedMeleeOpener)
            player->AttackStop();
        else if (targetInBreakableCrowdControl)
        {
            if (alreadyAttackingTarget)
                player->AttackStop();
        }
        else if (!alreadyAttackingTarget || meleeAutoAttackActive != useMeleeAttack)
            player->Attack(target, useMeleeAttack);

        // DriveHunterKiteLoop is the sole owner of Auto Shot activation,
        // planting, range transitions, and Mongoose weave movement. Starting
        // or canceling auto-repeat here runs before combat positioning and
        // fought the plant state on every fast tactical tick.
        if (player->GetClass() == CLASS_HUNTER && profile.primarilyRanged && targetInBreakableCrowdControl)
        {
            StopHunterAutoShotForBreakableCrowdControl(player, target, "engage-breakable-cc");
            return DriveCombatPositioning(player, target, profile);
        }

        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP positioning profile: bot={} target={} reason={} profile={} ranged={} createDistance={} meleeFallback={}.",
            player->GetGUID().ToString(), target->GetGUID().ToString(), reason ? reason : "combat", profile.label,
            profile.primarilyRanged, profile.createDistanceWhenCrowded, profile.meleeFallbackAcceptable);

        return DriveCombatPositioning(player, target, profile);
    }

    bool EngageNearestEnemyPlayer(Player* player, float scanDistance)
    {
        if (!player || !player->IsAlive())
            return false;

        Unit* target = AcquireCombatTarget(player, scanDistance);
        if (!target)
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP movement skipped: bot={} reason=no-combat-target scanDistance={}.",
                player ? player->GetGUID().ToString() : ObjectGuid::Empty.ToString(), scanDistance);
            return false;
        }

        return EngageSelectedEnemyPlayer(player, target, "nearest-enemy");
    }

    void ApplyDeterministicObjectiveOffset(Battleground const* battleground, Player const* player, Position& destination)
    {
        if (!battleground || !player)
            return;

        // Keep each bot slightly spread out, but stable across updates, to avoid
        // objective destination churn that causes oscillating movement.
        uint64 const seed = player->GetGUID().GetRawValue() ^ (uint64(battleground->GetMapId()) << 32) ^ battleground->GetInstanceID();
        float const angle = float(seed % 6283) / 1000.0f;
        float const radius = 2.0f + float((seed / 6283) % 600) / 100.0f; // [2.0, 8.0)
        destination.RelocateOffset(Position(std::cos(angle) * radius, std::sin(angle) * radius, 0.0f, 0.0f));
    }

    bool TryGetObjectivePosition(Battleground* battleground, Player* player, Position& destination)
    {
        if (!battleground || !player)
            return false;

        // Violet Hold has no objectives, and its team start positions are not
        // places anyone should walk to: the enemy one is wherever the LAST
        // wave clone was seated, i.e. inside a cell. The fallback below would
        // send a caged wave - and the party's own bots - marching at that
        // cell, straight through the wall between two of them, which is
        // exactly what a two-cell wave looks like on screen. There is nothing
        // to take or defend here; everyone fights what is in front of them,
        // which the enemy-pursuit paths above already handle.
        if (battleground->GetTypeID(true) == BATTLEGROUND_VHR)
            return false;

        playerbot::NodeBotOrders nodeOrders;
        if (playerbot::NodeCoordinator::GetOrders(player, nodeOrders) && nodeOrders.hasNode)
        {
            destination = nodeOrders.location;
            ApplyDeterministicObjectiveOffset(battleground, player, destination);
            return true;
        }

        BattlegroundNodeObjective nodeObjective;
        if (battleground->GetNodeObjective(player->GetGUID(), nodeObjective))
        {
            destination = nodeObjective.Location;
            ApplyDeterministicObjectiveOffset(battleground, player, destination);
            return true;
        }

        if (IsWarsongGulch(player))
        {
            // Midfield brawl behavior for WSG: collapse both teams toward center map.
            Position const midfieldAnchor(1239.40f, 1543.60f, 306.00f, 0.0f);
            destination = midfieldAnchor;
            ApplyDeterministicObjectiveOffset(battleground, player, destination);
            return true;
        }

        // A squad match has no far side: four corners close on the middle,
        // which is where the fights are.
        if (battleground->IsSquadMatch())
        {
            destination = battleground->GetSquadArenaCentre();
            ApplyDeterministicObjectiveOffset(battleground, player, destination);
            return true;
        }

        Position const* allianceStart = battleground->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(TEAM_ALLIANCE));
        Position const* hordeStart = battleground->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(TEAM_HORDE));

        if (allianceStart && hordeStart)
        {
            float const distanceToAllianceStart = player->GetDistance(allianceStart->GetPositionX(), allianceStart->GetPositionY(), allianceStart->GetPositionZ());
            float const distanceToHordeStart = player->GetDistance(hordeStart->GetPositionX(), hordeStart->GetPositionY(), hordeStart->GetPositionZ());
            destination = (distanceToAllianceStart > distanceToHordeStart) ? Position(*allianceStart) : Position(*hordeStart);
            ApplyDeterministicObjectiveOffset(battleground, player, destination);
            return true;
        }

        TeamId const botTeam = ResolveBotTeamId(player);
        TeamId const enemyTeam = (botTeam == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;
        if (Position const* enemyStart = battleground->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(enemyTeam)))
        {
            destination = Position(*enemyStart);
            ApplyDeterministicObjectiveOffset(battleground, player, destination);
            return true;
        }

        return false;
    }
}

namespace playerbot
{
    void BattlegroundTacticalActions::DelayFlagPickup(Player* player, uint32 delayMs)
    {
        if (!player || !delayMs)
            return;

        DroppedFlagPickupDelay& delay = playerbot::LockedGetOrCreate(g_DroppedFlagPickupDelayByBotGuid, player->GetGUID().GetRawValue());
        delay.flagGuid.Clear();
        delay.pickupNotBeforeMs = GameTime::GetGameTimeMS() + delayMs;
    }

    bool BattlegroundLifecycleActions::Execute(Player* player, BattlegroundLifecycleContext const& context)
    {
        if (!player || !context.lifecycleEnabled || !IsLifecycleGateEnabled())
            return false;

        if (RecoverStaleBattlegroundState(player))
            return true;

        if (HasConflictingBattlegroundLifecycleContext(context))
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP battleground lifecycle no-op due to conflicting context: guid={}, queueOperation={}, invitationResponse={}, handleInProgress={}.",
                player->GetGUID().ToString(), static_cast<uint8>(context.queueOperation), static_cast<uint8>(context.invitationResponse),
                context.shouldHandleInProgressStatus ? 1 : 0);
            return false;
        }

        if (ShouldRefreshLongQueuedBot(player))
        {
            EmitLifecycleDiagnostic(player, "queue-timeout-requeue",
                "Queue wait exceeded 15s without invite; leaving queue and requeueing.");
            bool const leftQueue = LeaveQueuePrimitive(player);
            bool const requeued = JoinQueuePrimitive(player);
            return leftQueue || requeued;
        }

        bool didExecute = false;

        switch (context.queueOperation)
        {
        case QueueOperationType::Join:
            didExecute = JoinQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::Leave:
            didExecute = LeaveQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::None:
        default:
            break;
        }

        switch (context.invitationResponse)
        {
        case InvitationResponseType::Accept:
            didExecute = AcceptInvitePrimitive(player) || didExecute;
            break;
        case InvitationResponseType::Decline:
            didExecute = DeclineInvitePrimitive(player) || didExecute;
            break;
        case InvitationResponseType::None:
        default:
            break;
        }

        if (context.shouldHandleInProgressStatus)
            didExecute = HandleInProgressStatusPrimitive(player) || didExecute;

        return didExecute;
    }

    bool BattlegroundLifecycleActions::HandleDeathPrimitive(Player* player)
    {
        return HandleBattlegroundDeathState(player);
    }

    bool BattlegroundLifecycleActions::JoinQueuePrimitive(Player* player)
    {
        if (!player || !IsLifecycleGateEnabled())
            return false;

        // Before the human-interest scan below, which walks every player online:
        // a bot that may not queue has no reason to ask whether anybody wants a
        // battleground.
        if (!MayQueueForInstancedPvp(player))
            return false;

        if (IsArenaOnlyManagedBotAccount(player))
            return false;

        BattlegroundTypeId const kManagedBattleground = ResolveManagedBotQueueTargetForAccount(player);
        uint32 const nowMs = GameTime::GetGameTimeMS();
        bool const hasHumanInterest = HasAnyRealHumanInterestInBattleground(kManagedBattleground);

        if (!hasHumanInterest)
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP lifecycle queue join blocked: no real human interest detected: guid={} bgTypeId={}.",
                player->GetGUID().ToString(), uint32(kManagedBattleground));
            return false;
        }

        // When real human interest exists (especially right after startup), force
        // an immediate population rebalance so additional managed bots can log in.
        if (hasHumanInterest &&
            nowMs >= g_LastHumanInterestPopulationRebalanceAttemptMs + PLAYERBOT_BG_HUMAN_INTEREST_REBALANCE_THROTTLE_MS)
        {
            g_LastHumanInterestPopulationRebalanceAttemptMs = nowMs;
            bool const rebalanceTriggered = RandomBotParticipationManager::TriggerImmediateRebalance();
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP lifecycle human-interest rebalance: guid={} bgTypeId={} triggered={}.",
                player->GetGUID().ToString(), uint32(kManagedBattleground), rebalanceTriggered ? 1 : 0);
        }

        uint32 const massQueued = ::QueueEligibleManagedBotsForBattleground(kManagedBattleground, 0, true);
        TC_LOG_DEBUG("playerbots.pvp.lifecycle",
            "Playerbot PvP lifecycle mass queue attempt: guid={} bgTypeId={} queuedCount={}.",
            player->GetGUID().ToString(), uint32(kManagedBattleground), massQueued);
        if (massQueued > 0)
            return true;

        return QueuePlayer(player, kManagedBattleground, 0);
    }

    bool BattlegroundLifecycleActions::LeaveQueuePrimitive(Player* player)
    {
        if (!player || !IsLifecycleGateEnabled())
            return false;

        return RemoveMatchingQueues(player, false, false, true);
    }

    bool BattlegroundLifecycleActions::AcceptInvitePrimitive(Player* player)
    {
        if (!player || !IsLifecycleGateEnabled())
            return false;

        return AcceptMatchingInvite(player, false);
    }

    bool BattlegroundLifecycleActions::DeclineInvitePrimitive(Player* player)
    {
        if (!player || !IsLifecycleGateEnabled())
            return false;

        return RemoveMatchingQueues(player, false, true, true);
    }

    bool BattlegroundLifecycleActions::HandleInProgressStatusPrimitive(Player* player)
    {
        if (!player || !IsLifecycleGateEnabled())
            return false;

        if (!player->InBattleground())
            return false;

        Battleground* battleground = player->GetBattleground();
        if (!battleground)
            return false;

        // A battleground that prepares part of a team mid-match holds those
        // bots exactly as the whole field is held before the start: Violet
        // Hold's next wave stands in its cells, buffing, while the battleground
        // reads in progress. The fast tick runs this before any tactical or
        // class movement, so the hold is in place before anything can walk the
        // bot out, and comes off on the first tick after the battleground takes
        // the preparation back.
        if (!IsHeldBehindGate(battleground, player))
            SetWaitJoinMovementLock(player, false);
        else if (battleground->GetStatus() == STATUS_IN_PROGRESS)
            SetWaitJoinMovementLock(player, true);

        // A rated arena is never ended here with no winner: that would let a
        // person facing a loss drop out of a match against bots and have it
        // count for nothing. The core holds a dropped person's seat for the
        // offline timer and then has their team forfeit (Battleground::
        // RemovePlayerAtLeave), and the no-humans rules below are left out.
        bool const ratedArena = battleground->isArena() && battleground->isRated();

        if (battleground->GetStatus() == STATUS_WAIT_JOIN)
        {
            // Soulwell pickup temporarily owns prep-room movement; otherwise
            // the ordinary start-position hold would drag bots away from the
            // well every lifecycle tick.
            if (!TryAcquireHealthstoneFromSoulwell(player))
                ForceHoldPlayerAtStartDuringWaitJoin(player);

            if (!ratedArena && !HasAnyRealHumanInterestInBattleground(battleground->GetTypeID()))
            {
                battleground->EndBattleground(PVP_TEAM_NEUTRAL);
                playerbot::LockedErase(g_BattlegroundNoHumanSinceMsByInstance, BuildBattlegroundInstanceKey(battleground));
                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP lifecycle wait-join end due to no real human battleground interest: guid={} bgTypeId={} instanceId={}.",
                    player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID());
                return true;
            }

            uint64 const battlegroundInstanceKey = BuildBattlegroundInstanceKey(battleground);
            if (!ratedArena && !BattlegroundHasAnyRealHumanPlayers(player))
            {
                uint32 const nowMs = GameTime::GetGameTimeMS();
                uint32& noHumanSinceMs = playerbot::LockedGetOrCreate(g_BattlegroundNoHumanSinceMsByInstance, battlegroundInstanceKey);
                if (!noHumanSinceMs)
                    noHumanSinceMs = nowMs;

                if (nowMs >= noHumanSinceMs + PLAYERBOT_BG_WAIT_JOIN_NO_HUMAN_END_DELAY_MS && !ShouldDeferBattlegroundLeaveForTeleportAck(player))
                {
                    battleground->EndBattleground(PVP_TEAM_NEUTRAL);
                    playerbot::LockedErase(g_BattlegroundNoHumanSinceMsByInstance, battlegroundInstanceKey);
                    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                        "Playerbot PvP lifecycle wait-join end due to no real humans: guid={} bgTypeId={} instanceId={}.",
                        player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID());
                    return true;
                }
            }
            else
                playerbot::LockedErase(g_BattlegroundNoHumanSinceMsByInstance, battlegroundInstanceKey);

            TryRefillManagedScmSlots(player, battleground);
            return true;
        }

        if (battleground->GetStatus() == STATUS_WAIT_LEAVE)
        {
            SetWaitJoinMovementLock(player, false);
            playerbot::LockedErase(g_BattlegroundNoHumanSinceMsByInstance, BuildBattlegroundInstanceKey(battleground));

            if (ShouldDeferBattlegroundLeaveForTeleportAck(player))
                return false;

            player->LeaveBattleground();
            FinalizeManagedBotTeleportIfPending(player);
            if (playerbot::IsManagedRandomBot(player))
                player->RemoveAurasDueToSpell(SPELL_DESERTER);
            RemoveMatchingQueues(player, false, false, true);
            RemoveMatchingQueues(player, true, false, false);
            player->SetArenaTeamIdInvited(0);
            EmitLifecycleDiagnostic(player, "wait-leave-cleanup", "Post-leave cleanup complete before returning to scheduler flow.");

            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP lifecycle leave after battleground end: guid={} bgTypeId={} instanceId={}",
                player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID());
            return true;
        }

        if (battleground->GetStatus() != STATUS_IN_PROGRESS)
        {
            playerbot::LockedErase(g_BattlegroundNoHumanSinceMsByInstance, BuildBattlegroundInstanceKey(battleground));
            return false;
        }

        // Secondary guard for non-virtual managed bot accounts: core battleground
        // shutdown treats any non-virtual session as human, so these matches can
        // persist indefinitely after real humans leave.
        uint64 const battlegroundInstanceKey = BuildBattlegroundInstanceKey(battleground);
        if (!ratedArena && !HasAnyRealHumanInterestInBattleground(battleground->GetTypeID()))
        {
            battleground->EndBattleground(PVP_TEAM_NEUTRAL);
            playerbot::LockedErase(g_BattlegroundNoHumanSinceMsByInstance, battlegroundInstanceKey);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP lifecycle end due to no real human battleground interest: guid={} bgTypeId={} instanceId={}.",
                player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID());
            return true;
        }

        if (!ratedArena && !BattlegroundHasAnyRealHumanPlayers(player))
        {
            uint32 const nowMs = GameTime::GetGameTimeMS();
            uint32& noHumanSinceMs = playerbot::LockedGetOrCreate(g_BattlegroundNoHumanSinceMsByInstance, battlegroundInstanceKey);
            if (!noHumanSinceMs)
                noHumanSinceMs = nowMs;

            if (nowMs >= noHumanSinceMs + PLAYERBOT_BG_NO_HUMAN_END_DELAY_MS && !ShouldDeferBattlegroundLeaveForTeleportAck(player))
            {
                battleground->EndBattleground(PVP_TEAM_NEUTRAL);
                playerbot::LockedErase(g_BattlegroundNoHumanSinceMsByInstance, battlegroundInstanceKey);
                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP lifecycle end due to no real human participants: guid={} bgTypeId={} instanceId={}.",
                    player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID());
                return true;
            }
        }
        else
            playerbot::LockedErase(g_BattlegroundNoHumanSinceMsByInstance, battlegroundInstanceKey);

        TryRefillManagedScmSlots(player, battleground);

        if (ShouldManagedBotLeaveForQueuedHuman(player, battleground))
        {
            player->LeaveBattleground();
            FinalizeManagedBotTeleportIfPending(player);
            player->RemoveAurasDueToSpell(SPELL_DESERTER);
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP human-priority departure trigger: guid={} bgTypeId={} instanceId={} players={} maxPlayers={}.",
                player->GetGUID().ToString(), uint32(battleground->GetTypeID()), battleground->GetInstanceID(),
                battleground->GetPlayersSize(), battleground->GetMaxPlayers());
            return true;
        }

        if (ShouldManagedBotLeaveForOverstack(player, battleground))
        {
            player->LeaveBattleground();
            FinalizeManagedBotTeleportIfPending(player);
            player->RemoveAurasDueToSpell(SPELL_DESERTER);
            QueuePlayer(player, ResolveManagedBotQueueTargetForAccount(player), 0);
            return true;
        }

        if (HandleBattlegroundDeathState(player))
            return true;

        ClearStaleWaitingForResurrectAura(player);

        // Stale start-of-match preparation on a bot that is already playing.
        // A battleground that hands out preparation mid-match owns it and
        // takes it back itself (Violet Hold's per-wave gate) - stripping that
        // would delete the window the wave is meant to buff in, on the very
        // next tick.
        if ((player->HasAura(SPELL_PREPARATION) || player->HasAura(SPELL_ARENA_PREPARATION) || player->HasUnitFlag(UNIT_FLAG_PREPARATION))
            && !battleground->OwnsPreparationState(player))
        {
            player->RemoveAurasDueToSpell(SPELL_PREPARATION);
            player->RemoveAurasDueToSpell(SPELL_ARENA_PREPARATION);
            player->RemoveUnitFlag(UNIT_FLAG_PREPARATION);
        }

        // Reference module parity: in-progress movement/combat are handled by
        // tactical actions (enemy pursuit / movement), not lifecycle.
        return false;
    }

    bool BattlegroundTacticalActions::Execute(Player* player, BattlegroundTacticalContext const& context)
    {
        if (!player || !context.tacticsEnabled || !context.shouldEvaluate || !context.actionName)
            return false;

        if (!player->IsAlive())
            return false;

        // In a vehicle seat every primitive below would move the bot itself,
        // which means nothing there; the objective duty drives the vehicle or
        // climbs out of it.
        if (player->GetVehicle())
            return ObjectiveDutyPrimitive(player);

        // The battleground fast tactical tick runs every 50 ms and is a
        // separate entry point from lifecycle combat positioning. It must
        // yield at its top-level boundary while an effect-driven charge/leap
        // owns movement; otherwise objective/pursuit actions can Clear() the
        // effect generator before its arrival MovementInform is delivered.
        if (HasPlayerbotGapCloserInFlight(player))
        {
            EmitRehgarMovementGuardServerDiagnostic(player, "tactical_tick_blocked_effect", 0);
            return true;
        }

        // Tactical actions include buff use, pursuit, and objective movement;
        // none should establish a stationary action position on hazardous
        // ground. Continue an active crossing or escape before doing anything.
        if (TryMoveOutOfHazardousLiquid(player))
            return true;

        // Lifecycle creates the Auto Shot plant after this fast tactical pass.
        // On the next 50 ms pass, every tactical movement source must yield or
        // the core sees movement again before the 434 ms release window ends.
        if (playerbot::IsHunterAutoShotPlantActive(player))
            return true;

        // A root cannot execute objective, pursuit, or spacing movement. A
        // snare can: melee still chases, while ranged retreat continues behind
        // instant casts and is stopped by the class executor for hard casts.
        if (playerbot::PvpCore::IsMovementPreventedByRoot(player))
        {
            if (MotionMaster* motionMaster = player->GetMotionMaster())
                motionMaster->Clear(MOTION_SLOT_ACTIVE);
            StopVirtualPlayerbotMovement(player);
            return false;
        }

        // The node-capture OPEN_LOCK spell owns the bot until it completes or
        // is interrupted by normal spell rules. Tactical movement and combat
        // otherwise restart/cancel the ten-second interaction every fast tick.
        if (playerbot::PvpClassActions::IsBattlegroundObjectInteractionInProgress(player))
            return true;

        // Off a start platform first: nothing on it is worth doing, and no
        // route the navmesh draws can leave it.
        if (Battleground* battleground = player->GetBattleground();
            battleground && battleground->GetStatus() == STATUS_IN_PROGRESS && TryLeaveDropIsland(player, battleground))
            return true;

        // Combat does not make a nearby flag optional. Once an eligible flag
        // is within ten yards, keep its approach/click route ahead of combat
        // positioning. Class execution uses the matching nearby value to allow
        // instant attacks while preventing range movement from replacing this
        // short objective move.
        if (Battleground* battleground = player->GetBattleground();
            battleground && battleground->GetStatus() == STATUS_IN_PROGRESS &&
            !playerbot::PvpCore::IsBattlegroundFlagCarrier(player))
        {
            // In Warsong Gulch and Twin Peaks only a flag this bot was sent for
            // counts: a flag just dropped for the runner is not up for grabs.
            playerbot::CtfBotOrders ctfOrders;
            ObjectGuid const pickupGuid = playerbot::CtfCoordinator::GetOrders(player, ctfOrders)
                ? ctfOrders.pickupGuid
                : battleground->GetFlagPickupGUID(player->GetGUID());
            if (!pickupGuid.IsEmpty())
            {
                GameObject* flag = player->FindMap() ? player->FindMap()->GetGameObject(pickupGuid) : nullptr;
                if (flag && flag->IsInWorld() && player->IsWithinDistInMap(flag, 10.0f))
                {
                    TryAdvanceFlagObjective(player, battleground);
                    return true;
                }
            }
        }

        // Stock up during the preparation window (or shortly afterward if a
        // bot joined late). This uses the summoned object's owner, range, raid
        // restriction, lifetime, and 25-charge counter.
        if (TryAcquireHealthstoneFromSoulwell(player))
            return true;

        // Lightwell recovery owns movement for injured teammates, regardless of
        // their current combat objective. Use the actual summoned spellcaster
        // object so its party restriction, charges, and Renew cast stay in the
        // normal GameObject::Use path. Class execution separately permits only
        // instant spells while this path is active.
        if (playerbot::PvpCore::ShouldSeekLightwell(player))
        {
            if (GameObject* lightwell = playerbot::PvpCore::FindUsableLightwell(player, 20.0f))
            {
                if (!lightwell->IsAtInteractDistance(player))
                    return IssueMovePointThrottled(player, lightwell->GetPosition(), 4.0f, 500) || player->isMoving();

                lightwell->Use(player);
                return true;
            }
        }

        BreakExpiredHunterFeignDeath(player);

        ClearStaleWaitingForResurrectAura(player);

        if (IsRecoveringByEatingOrDrinking(player))
        {
            // Up the moment the team comes under attack (PvpCore::IsTeamInDanger):
            // a seated bot used to finish its drink while its partner died. The
            // class pass later in this tick then picks a heal or an attack.
            if (playerbot::PvpCore::IsTeamInDanger(player))
            {
                ClearEatDrinkAurasForMovement(player);
                if (player->IsSitState())
                    player->SetStandState(UNIT_STAND_STATE_STAND);
                TC_LOG_DEBUG("playerbots.pvp.lifecycle", "Playerbot PvP lifecycle: {} stopped eating/drinking - the team is in a fight.",
                    player->GetName());
            }
            else
            {
                if (player->isMoving())
                    player->StopMoving();
                return true;
            }
        }

        if (Battleground* battleground = player->GetBattleground())
        {
            // During prep phase only process the explicit start-position action.
            // This prevents competing tactical actions from pulling bots back/forth.
            // A bot held behind a gate mid-match gets no tactical order at all:
            // its battleground reads in progress, so the selector hands it live
            // pursuit, and the lifecycle tick is already holding it.
            if (IsHeldBehindGate(battleground, player))
            {
                if (IsTacticalAction(context.actionName, "bg move to start"))
                    return MoveToStartPrimitive(player);
                return false;
            }
        }

        if (IsTacticalAction(context.actionName, "bg move to start"))
            return MoveToStartPrimitive(player);
        if (IsTacticalAction(context.actionName, "bg pursue enemy"))
            return PursueEnemyPrimitive(player);
        if (IsTacticalAction(context.actionName, "bg move to objective"))
            return MoveToObjectivePrimitive(player, context);
        if (IsTacticalAction(context.actionName, "bg reset objective force"))
            return ResetObjectiveForcePrimitive(player);
        if (IsTacticalAction(context.actionName, "bg use buff"))
            return UseBuffPrimitive(player);
        if (IsTacticalAction(context.actionName, "attack enemy flag carrier"))
            return AttackEnemyFlagCarrierPrimitive(player, context);
        if (IsTacticalAction(context.actionName, "bg protect fc"))
            return ProtectFlagCarrierPrimitive(player, context);
        if (IsTacticalAction(context.actionName, "bg flag handoff"))
            return FlagHandoffPrimitive(player);
        if (IsTacticalAction(context.actionName, "bg escort flag runner"))
            return EscortFlagRunnerPrimitive(player);
        if (IsTacticalAction(context.actionName, "bg defend flag room"))
            return DefendFlagRoomPrimitive(player);
        if (IsTacticalAction(context.actionName, "bg flag runner stage"))
            return FlagRunnerStagePrimitive(player);
        if (IsTacticalAction(context.actionName, "bg objective duty"))
            return ObjectiveDutyPrimitive(player);
        if (IsTacticalAction(context.actionName, "bg follow orders"))
            return FollowBattlegroundOrdersPrimitive(player);

        return false;
    }

    bool BattlegroundTacticalActions::MoveToStartPrimitive(Player* player)
    {
        return ForceHoldPlayerAtStartDuringWaitJoin(player);
    }

    bool BattlegroundTacticalActions::MoveToObjectivePrimitive(Player* player, BattlegroundTacticalContext const& context)
    {
        if (!player || !player->InBattleground())
            return false;

        Battleground* battleground = player->GetBattleground();
        if (!battleground || battleground->GetStatus() == STATUS_WAIT_JOIN)
            return false;

        if (!player->IsAlive() || player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
            return false;

        bool const outdoors = IsEffectivelyOutdoors(player);
        if (player->IsMounted() && ShouldForceIndoorDismount(player, outdoors))
            ForcePlayerbotDismount(player);

        switch (player->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
        case IDLE_MOTION_TYPE:
        case CHASE_MOTION_TYPE:
        case POINT_MOTION_TYPE:
        case FOLLOW_MOTION_TYPE:
            // Charge/Intercept traverse through EFFECT_MOTION_TYPE.
        case EFFECT_MOTION_TYPE:
            break;
        default:
        {
            // If the bot is effectively stationary while stuck in a non-tactical
            // movement generator, clear it so objective/pursuit movement can
            // issue a fresh MovePoint.
            if (!player->isMoving())
            {
                EmitBattlegroundGmDebug(player,
                    "move-to-objective=clear-stale-motion motionType=" +
                    std::to_string(uint32(player->GetMotionMaster()->GetCurrentMovementGeneratorType())), 1200);
                player->GetMotionMaster()->Clear();
                break;
            }

            return true;
        }
        }

        if (context.objective.type == BattlegroundObjectiveType::CaptureFlag)
        {
            bool const isFlagCarrier = playerbot::PvpCore::IsBattlegroundFlagCarrier(player);

            // Carrying the flag is unconditional: never replace the capture
            // route with midfield combat, even when enemies are nearby.
            if (isFlagCarrier)
            {
                TryAdvanceFlagObjective(player, battleground);
                return true;
            }

            // A non-carrier heading for a live flag fights local enemies first,
            // but does not abandon the objective for a distant map-wide chase.
            // Not in Warsong Gulch or Twin Peaks: there a bot is only sent for a
            // flag when taking it beats the fight - it is the runner, it is
            // standing near the flag, or the flag is ours and lying loose.
            if (context.nearbyEnemyActive && context.ctfRole == 0)
            {
                float const localCombatRange = std::max(playerbot::PvpCore::GetConfig().longRange, 35.0f);
                if (Player* nearbyEnemy = FindNearestEnemyBattlegroundPlayer(player, localCombatRange, nullptr, nullptr))
                {
                    EngageSelectedEnemyPlayer(player, nearbyEnemy, "flag-runner-midfield-pressure");
                    return true;
                }
            }

            // With midfield clear, commit fluidly to the navmesh flag route.
            // Class actions share the same nearby-enemy decision and therefore
            // cannot replace this MovePoint with alternating combat movement.
            if (TryAdvanceFlagObjective(player, battleground))
                return true;
        }

        // Checked before the combat branch below: a bot holding a base is worth
        // more standing on it than finishing the fight that walked it away, and
        // that branch chases a hundred yards.
        bool const nodeObjective = context.objective.type == BattlegroundObjectiveType::AssaultNode ||
            context.objective.type == BattlegroundObjectiveType::DefendNode;
        if (nodeObjective && TryReturnToLeashedNode(player, battleground))
            return true;

        // A bot holding a base, or taking back one of ours, fights on that base
        // and nowhere else. It used to pick its fight by distance from ITSELF -
        // 35 y out of combat, 100 y in it - while the leash above is measured
        // from the banner, so any fight 45 to 100 y off the base dragged it out,
        // the leash walked it home, and the next tick sent it straight back out:
        // the defender running back and forth. Fights are now picked by distance
        // from the banner, inside the leash, so there is nothing to walk back
        // from; and a base with nobody on it is held from the banner.
        playerbot::NodeBotOrders heldOrders;
        if (nodeObjective && playerbot::NodeCoordinator::GetOrders(player, heldOrders) &&
            heldOrders.hasNode && heldOrders.leash)
        {
            if (Player* intruder = FindEnemyOnHeldGround(player, heldOrders))
                if (EngageSelectedEnemyPlayer(player, intruder, "held-base-intruder"))
                    return true;

            // Whatever it was swinging at has left the base. Stop the swing so
            // the chase it installed stands down; spells still go out at him
            // from here if he is in range.
            if (Unit* victim = player->GetVictim();
                victim && playerbot::NodeCoordinator::IsBeyondHeldGround(player, victim))
                player->AttackStop();

            return TryAdvanceNodeObjective(player, battleground);
        }

        if (player->IsInCombat())
        {
            if (context.flagCarrierDirective == FlagCarrierDirective::AttackEnemyCarrier)
                if (Player* enemyCarrier = FindFlagCarrierForDirective(player, FlagCarrierDirective::AttackEnemyCarrier))
                    if (EngageSelectedEnemyPlayer(player, enemyCarrier, "enemy-flag-carrier"))
                        return true;

            // A bot sent to take a base fights what stands in its way, as it
            // does out of combat below, and then takes the base. The chase
            // that follows scans a hundred yards, and in a fight that never
            // lets up it never let go: Strand of the Ancients' defenders stand
            // at every gate the attackers have just broken, so an attacker
            // sent for the graveyard behind that gate stayed on them and the
            // attackers took no graveyards at all.
            if (context.objective.type == BattlegroundObjectiveType::AssaultNode)
            {
                if (EngageNearestEnemyPlayer(player, 35.0f))
                    return true;
                if (TryAdvanceNodeObjective(player, battleground))
                    return true;
            }

            return EngageNearestEnemyPlayer(player, GetAggressiveCombatScanDistance(player, 100.0f));
        }

        if ((context.objective.type == BattlegroundObjectiveType::AssaultNode ||
            context.objective.type == BattlegroundObjectiveType::DefendNode))
        {
            // Fight threats already near the assigned base, but do not let the
            // generic map-wide enemy chase pull node attackers/defenders away.
            if (EngageNearestEnemyPlayer(player, 35.0f))
                return true;
            if (TryAdvanceNodeObjective(player, battleground))
                return true;
        }

        if (TryPursueNearestEnemyInBattleground(player))
            return true;

        // Evaluate target pursuit before this guard so stale movement flags do not
        // suppress nearest-enemy pathing after battleground resurrection.
        if (player->isMoving())
            return false;

        if (context.objective.type == BattlegroundObjectiveType::None &&
            context.movement == BattlegroundMovementPrimitive::None &&
            context.flagCarrierDirective == FlagCarrierDirective::None)
        {
            if (battleground && (battleground->GetTypeID() == BATTLEGROUND_SCM || battleground->GetTypeID() == BATTLEGROUND_VHR))
            {
                float const engageDistance = GetAggressiveCombatScanDistance(player, 100.0f);
                if (EngageNearestEnemyPlayer(player, engageDistance))
                    return true;

                if (Player* nearestEnemy = FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max(), nullptr, nullptr))
                    return MoveTowardUnit(player, nearestEnemy, 20.0f);
            }

            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP movement skipped: bot={} reason=no-objective-and-no-directive.",
                player->GetGUID().ToString());
            return false;
        }

        if (context.movement == BattlegroundMovementPrimitive::MoveToObjectiveUnit ||
            context.movement == BattlegroundMovementPrimitive::FollowFlagCarrier ||
            context.flagCarrierDirective != FlagCarrierDirective::None)
        {
            if (Player* carrier = FindFlagCarrierForDirective(player, context.flagCarrierDirective))
            {
                TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                    "Playerbot PvP objective support selected: guid={} directive={} target={}.",
                    player->GetGUID().ToString(), static_cast<uint8>(context.flagCarrierDirective), carrier->GetGUID().ToString());
                return MoveTowardUnit(player, carrier, 20.0f);
            }
        }

        if (context.movement == BattlegroundMovementPrimitive::MoveToObjectivePosition)
        {
            if (battleground)
            {
                Position destination;
                if (TryGetObjectivePosition(battleground, player, destination))
                {
                    bool const withinObjectiveRange = player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(), 12.0f);
                    if (!withinObjectiveRange)
                        IssueMovePointThrottled(player, destination);
                    else
                        EmitBattlegroundGmDebug(player, "objective-skip reason=already-near-objective range=12", 1000);
                    return true;
                }

                if (battleground->GetTypeID() == BATTLEGROUND_SCM || battleground->GetTypeID() == BATTLEGROUND_VHR)
                {
                    float const engageDistance = GetAggressiveCombatScanDistance(player, 100.0f);
                    if (EngageNearestEnemyPlayer(player, engageDistance))
                        return true;

                    if (Player* nearestEnemy = FindNearestEnemyBattlegroundPlayer(player, std::numeric_limits<float>::max(), nullptr, nullptr))
                        return MoveTowardUnit(player, nearestEnemy, 20.0f);
                }

                return false;
            }
        }

        return false;
    }

    namespace
    {
        // Within this of the ground in front of a gate a charge goes down, and
        // a vehicle stops to use its siege spells. The battleground puts that
        // ground a few yards off the gate's face.
        constexpr float kDutyGateStandRange = 4.0f;
        constexpr float kDutyGateReachRange = 10.0f;
        // A charge burns for ten seconds and the defenders can click it away
        // (GameObject trap type 2, Seaforium): the bot that planted it stays
        // and fights for it that long.
        constexpr uint32 kDutyChargeWatchMs = 10 * IN_MILLISECONDS;
        constexpr float kDutyChargeWatchRange = 25.0f;
        // A guard holds the ground in front of its gate and fights what comes
        // within this of it.
        constexpr float kDutyGateGuardRange = 45.0f;
        constexpr float kDutyGateGuardHold = 12.0f;
        // On its way past a gate, a guard keeps fighting only what is this close.
        constexpr float kDutyGatePassFightRange = 8.0f;
        // A commander is engaged from here; further out the bot travels.
        constexpr float kDutyBossEngageRange = 40.0f;
        constexpr uint32 kDutyVehicleCastIntervalMs = 1000;

        std::unordered_map<uint64, uint32> g_ChargePlantedMsByBot;
        std::unordered_map<uint64, uint32> g_VehicleCastMsByBot;

        // A drive that stops getting closer to its gate for this long is given
        // up: the vehicle is left (and left alone for a while), so a driver
        // cannot sit in a stuck ram for the rest of the match.
        constexpr uint32 kDutyVehicleStuckMs = 15 * IN_MILLISECONDS;
        constexpr float kDutyVehicleProgressYards = 2.0f;
        constexpr uint32 kDutyVehicleReissueMs = 1000;

        struct VehicleDriveState
        {
            ObjectGuid vehicleGuid;
            float bestDistance = 0.0f;
            uint32 progressMs = 0;
            uint32 issuedMs = 0;
        };
        std::unordered_map<uint64, VehicleDriveState> g_VehicleDriveByBot;

        // The charge source a bot settled on, re-chosen this often: picking one
        // walks a navmesh route to each of the nearest few.
        constexpr uint32 kDutyChargeSourceRecheckMs = 5 * IN_MILLISECONDS;
        constexpr uint32 kDutyChargeSourceCandidates = 4;
        struct ChargeSourceChoice
        {
            ObjectGuid sourceGuid;
            uint32 chosenMs = 0;
        };
        std::unordered_map<uint64, ChargeSourceChoice> g_ChargeSourceByBot;

        // The ground a guard holds: the front of its gate when it can walk
        // there, else the same distance behind the gate - the inside of a wall
        // its own side still holds closed (Strand of the Ancients' defenders,
        // who start behind every gate and have no way out through them).
        struct GuardSpotChoice
        {
            ObjectGuid gateGuid;
            Position spot;
            uint32 chosenMs = 0;
        };
        std::unordered_map<uint64, GuardSpotChoice> g_GuardSpotByBot;
        constexpr uint32 kDutyGuardSpotRecheckMs = 5 * IN_MILLISECONDS;

        // The vehicle spells that damage gameobjects, directly (a ram) or by
        // launching something that does (a boulder).
        void CollectSiegeSpells(Creature const* vehicle, std::vector<SpellInfo const*>& spells)
        {
            for (uint32 spellId : vehicle->m_spells)
            {
                SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
                if (!spellInfo)
                    continue;

                bool damagesObjects = false;
                for (SpellEffectInfo const& effect : spellInfo->GetEffects())
                {
                    if (effect.IsEffect(SPELL_EFFECT_GAMEOBJECT_DAMAGE))
                        damagesObjects = true;
                    else if ((effect.IsEffect(SPELL_EFFECT_TRIGGER_MISSILE) || effect.IsEffect(SPELL_EFFECT_TRIGGER_SPELL)) &&
                        effect.TriggerSpell)
                        if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                            for (SpellEffectInfo const& triggeredEffect : triggered->GetEffects())
                                if (triggeredEffect.IsEffect(SPELL_EFFECT_GAMEOBJECT_DAMAGE))
                                    damagesObjects = true;
                }

                if (damagesObjects)
                    spells.push_back(spellInfo);
            }
        }

        // The nearest enemy at a piece of ground: a vehicle someone is driving
        // first (it is what brings a gate down), then a player. One closer to
        // the ground than minRange is passed over (inside a gun's dead zone).
        Unit* FindEnemyAtGround(Player* player, Position const& ground, float range, float minRange = 0.0f)
        {
            Map* map = player->FindMap();
            if (!map)
                return nullptr;

            Unit* bestVehicle = nullptr;
            Player* bestPlayer = nullptr;
            float bestVehicleDistance = std::numeric_limits<float>::max();
            float bestPlayerDistance = std::numeric_limits<float>::max();
            Map::PlayerList const& players = map->GetPlayers();
            for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
            {
                Player* candidate = itr->GetSource();
                if (!candidate || candidate == player || !candidate->IsAlive() || candidate->IsGameMaster() ||
                    candidate->IsSpectator())
                    continue;
                if (!candidate->IsWithinDist3d(ground.GetPositionX(), ground.GetPositionY(), ground.GetPositionZ(), range))
                    continue;
                if (minRange > 0.0f && candidate->IsWithinDist3d(ground.GetPositionX(), ground.GetPositionY(), ground.GetPositionZ(), minRange))
                    continue;

                // Line of sight counts gates: an enemy on the far side of a gate
                // still standing is somebody else's fight, and chasing him only
                // presses the bot against the gate.
                float const distance = player->GetExactDist(candidate);
                if (Unit* vehicle = candidate->GetVehicleBase())
                {
                    if (vehicle->IsAlive() && player->IsValidAttackTarget(vehicle) && distance < bestVehicleDistance &&
                        player->IsWithinLOSInMap(vehicle))
                    {
                        bestVehicleDistance = distance;
                        bestVehicle = vehicle;
                    }
                    continue;
                }

                if (!player->IsValidAttackTarget(candidate) || !player->CanSeeOrDetect(candidate) ||
                    !player->IsWithinLOSInMap(candidate))
                    continue;

                if (distance < bestPlayerDistance)
                {
                    bestPlayerDistance = distance;
                    bestPlayer = candidate;
                }
            }

            return bestVehicle ? bestVehicle : bestPlayer;
        }

        // Driving: up to the ground in front of the gate, square to it, then
        // every siege spell the vehicle has as it comes off cooldown.
        bool DriveSiegeVehicle(Player* player, Unit* vehicleBase, playerbot::ObjectiveBotOrders const& orders)
        {
            Creature* vehicle = vehicleBase->ToCreature();
            Map* map = player->FindMap();
            // Only the driver moves it: a bot that somehow sits in another seat
            // leaves the vehicle to whoever has its controls.
            if (!vehicle || vehicle->GetCharmerGUID() != player->GetGUID())
                return true;

            uint64 const botKey = player->GetGUID().GetRawValue();
            uint32 const nowMs = GameTime::GetGameTimeMS();

            // Between gates (the one it was hitting just fell) the driver waits
            // in its seat for the plan to name the next one.
            GameObject* gate = map ? map->GetGameObject(orders.gateGuid) : nullptr;
            if (!gate)
            {
                if (vehicle->isMoving())
                    vehicle->StopMoving();
                playerbot::LockedErase(g_VehicleDriveByBot, botKey);
                return true;
            }

            // The front of the gate itself, not merely near the gate: a ram is
            // a cone off the vehicle's nose.
            Position const& ground = orders.gateLocation;
            if (!vehicle->IsWithinDist2d(&ground, kDutyGateStandRange))
            {
                float const distance = vehicle->GetExactDist2d(ground.GetPositionX(), ground.GetPositionY());
                VehicleDriveState drive;
                bool const known = playerbot::LockedGetCopy(g_VehicleDriveByBot, botKey, drive) &&
                    drive.vehicleGuid == vehicle->GetGUID();
                if (!known)
                {
                    drive = VehicleDriveState();
                    drive.vehicleGuid = vehicle->GetGUID();
                    drive.bestDistance = distance;
                    drive.progressMs = nowMs;
                }
                else if (distance < drive.bestDistance - kDutyVehicleProgressYards)
                {
                    drive.bestDistance = distance;
                    drive.progressMs = nowMs;
                }
                else if (nowMs - drive.progressMs >= kDutyVehicleStuckMs)
                {
                    playerbot::LockedErase(g_VehicleDriveByBot, botKey);
                    playerbot::ObjectiveCoordinator::GiveUpVehicle(vehicle->GetGUID());
                    if (MotionMaster* motionMaster = vehicle->GetMotionMaster())
                        motionMaster->Clear();
                    vehicle->StopMoving();
                    player->ExitVehicle();
                    EmitBattlegroundGmDebug(player, "objective-duty=vehicle-stuck entry=" + std::to_string(vehicle->GetEntry()), 1500);
                    return true;
                }

                MotionMaster* motionMaster = vehicle->GetMotionMaster();
                bool const alreadyGoing = motionMaster->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE &&
                    vehicle->isMoving();
                if (!alreadyGoing && nowMs - drive.issuedMs >= kDutyVehicleReissueMs)
                {
                    motionMaster->MovePoint(0, ground);
                    drive.issuedMs = nowMs;
                }
                playerbot::LockedSet(g_VehicleDriveByBot, botKey, drive);
                return true;
            }

            playerbot::LockedErase(g_VehicleDriveByBot, botKey);
            if (vehicle->isMoving())
                vehicle->StopMoving();

            // Squared up in the same tick it fires: a turn that never took on a
            // player-controlled vehicle must not hold its guns forever, and a
            // ram aimed slightly off is simply tried again next time round.
            if (!vehicle->HasInArc(float(M_PI) / 6, gate))
                vehicle->SetFacingToObject(gate);

            uint32& lastCastMs = playerbot::LockedGetOrCreate(g_VehicleCastMsByBot, botKey);
            if (lastCastMs && nowMs - lastCastMs < kDutyVehicleCastIntervalMs)
                return true;
            lastCastMs = nowMs;

            std::vector<SpellInfo const*> spells;
            CollectSiegeSpells(vehicle, spells);
            for (SpellInfo const* spellInfo : spells)
            {
                if (!vehicle->GetSpellHistory()->IsReady(spellInfo))
                    continue;

                // Aimed at the gate; a ram ignores the point and hits what is
                // in front of it.
                SpellCastTargets targets;
                targets.SetSrc(vehicle->GetPositionX(), vehicle->GetPositionY(), vehicle->GetPositionZ());
                targets.SetDst(*gate);
                if (vehicle->CastSpell(CastSpellTargetArg(std::move(targets)), spellInfo->Id) == SPELL_CAST_OK)
                    break;
            }

            return true;
        }

        // Manning a gun: turned on the nearest enemy it can hit, and every
        // spell it has that hurts people as each comes off cooldown, aimed at
        // where he stands - fired the way a driver fires at a gate.
        bool ManTurretVehicle(Player* player, Unit* vehicleBase)
        {
            Creature* turret = vehicleBase->ToCreature();
            if (!turret || turret->GetCharmerGUID() != player->GetGUID())
                return true;

            uint64 const botKey = player->GetGUID().GetRawValue();
            uint32 const nowMs = GameTime::GetGameTimeMS();
            uint32& lastCastMs = playerbot::LockedGetOrCreate(g_VehicleCastMsByBot, botKey);
            if (lastCastMs && nowMs - lastCastMs < kDutyVehicleCastIntervalMs)
                return true;

            std::vector<SpellInfo const*> spells;
            playerbot::CollectTurretSpells(turret, spells);
            float reach = 0.0f;
            float minRange = std::numeric_limits<float>::max();
            for (SpellInfo const* spellInfo : spells)
            {
                reach = std::max(reach, spellInfo->GetMaxRange(false));
                minRange = std::min(minRange, spellInfo->GetMinRange(false));
            }
            if (spells.empty())
                return true;

            Unit* target = FindEnemyAtGround(player, turret->GetPosition(), reach, minRange);
            if (!target)
                return true;

            if (!turret->HasInArc(float(M_PI) / 6, target))
                turret->SetFacingToObject(target);
            lastCastMs = nowMs;

            for (SpellInfo const* spellInfo : spells)
            {
                if (!turret->GetSpellHistory()->IsReady(spellInfo))
                    continue;

                SpellCastTargets targets;
                targets.SetSrc(turret->GetPositionX(), turret->GetPositionY(), turret->GetPositionZ());
                targets.SetDst(*target);
                if (turret->CastSpell(CastSpellTargetArg(std::move(targets)), spellInfo->Id) == SPELL_CAST_OK)
                    break;
            }

            return true;
        }

        // Up to the vehicle a duty gave the bot, and into the seat that drives
        // it: a siege vehicle to drive at a gate, or a gun to man.
        bool BoardDutyVehicle(Player* player, Battleground* battleground, playerbot::ObjectiveBotOrders const& orders)
        {
            Map* map = player->FindMap();
            Creature* vehicle = map ? map->GetCreature(orders.vehicleGuid) : nullptr;
            Vehicle* kit = vehicle && vehicle->IsAlive() ? vehicle->GetVehicleKit() : nullptr;
            int8 const controlSeat = playerbot::GetVehicleControlSeat(vehicle);
            if (!kit || controlSeat < 0)
                return false;

            // The seat that drives it, empty. Not seat 0 by assumption, and an
            // occupied seat is never clicked for: the seat-specific spell-click
            // path asserts on a passenger it cannot resolve.
            auto const seatItr = kit->Seats.find(controlSeat);
            if (seatItr == kit->Seats.end() || !seatItr->second.IsEmpty())
                return false;

            if (!player->IsWithinDistInMap(vehicle, INTERACTION_DISTANCE))
                return MoveToBattlegroundObjective(player, battleground, vehicle->GetPosition()) || player->isMoving();

            if (player->IsMounted())
                ForcePlayerbotDismount(player);
            StopVirtualPlayerbotMovement(player);
            player->AttackStop();

            // The vehicle's own spell-click: its conditions and Ride Vehicle
            // spell, exactly as a player's click.
            vehicle->HandleSpellClick(player, controlSeat);
            EmitBattlegroundGmDebug(player, "objective-duty=board-vehicle entry=" + std::to_string(vehicle->GetEntry()), 1500);
            return true;
        }

        bool AdvanceCarryChargeDuty(Player* player, Battleground* battleground, playerbot::ObjectiveBotOrders const& orders)
        {
            Map* map = player->FindMap();
            GameObject* gate = map ? map->GetGameObject(orders.gateGuid) : nullptr;
            if (!gate)
                return false;

            uint64 const botKey = player->GetGUID().GetRawValue();
            uint32 const nowMs = GameTime::GetGameTimeMS();

            // Stay with a charge just planted: the defenders can click it away.
            uint32 plantedMs = 0;
            if (playerbot::LockedGetCopy(g_ChargePlantedMsByBot, botKey, plantedMs) && nowMs - plantedMs < kDutyChargeWatchMs)
            {
                if (Unit* enemy = FindEnemyAtGround(player, orders.gateLocation, kDutyChargeWatchRange))
                    if (playerbot::EngageSelectedEnemyPlayer(player, enemy, "charge-watch"))
                        return true;
                return true;
            }

            uint32 const itemId = orders.chargeItemId;
            if (itemId && player->HasItemCount(itemId, 1))
            {
                bool const atGate = player->IsWithinDist2d(&orders.gateLocation, kDutyGateStandRange) ||
                    player->IsWithinDist2d(gate, kDutyGateReachRange);
                if (!atGate)
                    return MoveToBattlegroundObjective(player, battleground, orders.gateLocation) || player->isMoving();

                Item* charge = player->GetItemByEntry(itemId);
                if (!charge || player->IsNonMeleeSpellCast(false))
                    return true;

                if (player->IsMounted())
                    ForcePlayerbotDismount(player);
                StopVirtualPlayerbotMovement(player);
                player->SetFacingToObject(gate);

                // The client's own item-use path, so the charge is consumed
                // and the bomb placed exactly as for a player.
                SpellCastTargets targets;
                player->CastItemUseSpell(charge, targets, 1, 0);
                if (!player->HasItemCount(itemId, 1) || player->IsNonMeleeSpellCast(false))
                    playerbot::LockedSet(g_ChargePlantedMsByBot, botKey, nowMs);
                EmitBattlegroundGmDebug(player, "objective-duty=plant-charge item=" + std::to_string(itemId), 1500);
                return true;
            }

            // No room for one: a pile is used up whether or not the charge
            // fits, so a full bag would empty every pile on the beach.
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, orders.chargeItemId, 1) != EQUIP_ERR_OK)
                return false;

            // Fetch one from the nearest source the bot can walk to: the
            // nearest by distance can sit behind a gate still standing.
            GameObject* nearest = nullptr;
            ChargeSourceChoice choice;
            if (playerbot::LockedGetCopy(g_ChargeSourceByBot, botKey, choice) &&
                nowMs - choice.chosenMs < kDutyChargeSourceRecheckMs &&
                std::find(orders.chargeSources.begin(), orders.chargeSources.end(), choice.sourceGuid) != orders.chargeSources.end())
            {
                nearest = map->GetGameObject(choice.sourceGuid);
                if (nearest && (!nearest->IsInWorld() || !nearest->isSpawned()))
                    nearest = nullptr;
            }

            if (!nearest)
            {
                std::vector<std::pair<float, GameObject*>> candidates;
                for (ObjectGuid const& sourceGuid : orders.chargeSources)
                {
                    GameObject* source = map->GetGameObject(sourceGuid);
                    if (source && source->IsInWorld() && source->isSpawned())
                        candidates.emplace_back(player->GetExactDist(source), source);
                }
                std::sort(candidates.begin(), candidates.end(),
                    [](auto const& a, auto const& b) { return a.first < b.first; });

                GuidVector gates;
                battleground->GetStandingGates(gates);
                uint32 checked = 0;
                for (auto const& candidate : candidates)
                {
                    if (checked++ >= kDutyChargeSourceCandidates)
                        break;
                    if (!IsWalkableRoute(player, gates, candidate.second->GetPosition()))
                        continue;
                    nearest = candidate.second;
                    break;
                }

                if (!nearest)
                {
                    playerbot::LockedErase(g_ChargeSourceByBot, botKey);
                    return false;
                }

                playerbot::LockedSet(g_ChargeSourceByBot, botKey, ChargeSourceChoice{ nearest->GetGUID(), nowMs });
            }

            if (!nearest->IsAtInteractDistance(player))
                return MoveToBattlegroundObjective(player, battleground, nearest->GetPosition()) || player->isMoving();

            if (player->IsMounted())
                ForcePlayerbotDismount(player);
            StopVirtualPlayerbotMovement(player);
            nearest->Use(player);
            playerbot::LockedErase(g_ChargeSourceByBot, botKey);
            EmitBattlegroundGmDebug(player, "objective-duty=fetch-charge source=" + std::to_string(nearest->GetEntry()), 1500);
            return true;
        }

        // The ground a guard holds for this gate, re-decided now and then.
        Position ResolveGuardSpot(Player* player, Battleground* battleground, playerbot::ObjectiveBotOrders const& orders)
        {
            uint64 const botKey = player->GetGUID().GetRawValue();
            uint32 const nowMs = GameTime::GetGameTimeMS();
            GuardSpotChoice choice;
            if (playerbot::LockedGetCopy(g_GuardSpotByBot, botKey, choice) && choice.gateGuid == orders.gateGuid &&
                nowMs - choice.chosenMs < kDutyGuardSpotRecheckMs)
                return choice.spot;

            choice = GuardSpotChoice();
            choice.gateGuid = orders.gateGuid;
            choice.chosenMs = nowMs;
            choice.spot = orders.gateLocation;

            GameObject* gate = player->FindMap() ? player->FindMap()->GetGameObject(orders.gateGuid) : nullptr;
            if (gate)
            {
                // A teleporter (Isle of Conquest's keeps) or a ledge off the
                // walls (Strand of the Ancients) past the gates in between
                // counts as a way there: a guard behind its own gate sees none
                // of the attackers planting charges at it.
                GateBypassRoute const route = DecideGateBypass(player, battleground, orders.gateLocation);
                if (route.blocked && !route.HasWayPast())
                    choice.spot.Relocate(2.0f * gate->GetPositionX() - orders.gateLocation.GetPositionX(),
                        2.0f * gate->GetPositionY() - orders.gateLocation.GetPositionY(), orders.gateLocation.GetPositionZ());
            }

            playerbot::LockedSet(g_GuardSpotByBot, botKey, choice);
            return choice.spot;
        }

        bool AdvanceGuardGateDuty(Player* player, Battleground* battleground, playerbot::ObjectiveBotOrders const& orders)
        {
            Position const ground = ResolveGuardSpot(player, battleground, orders);
            Position destination = ground;
            playerbot::ApplyDeterministicObjectiveOffset(battleground, player, destination);
            bool const atGround = player->IsWithinDist3d(destination.GetPositionX(), destination.GetPositionY(),
                destination.GetPositionZ(), kDutyGateGuardHold);

            // Still on its way past a gate of its own (over a wall to the ledge
            // off it, or to a teleporter), the fight at its ground is on the
            // far side of that gate. Taking on an enemy seen from the wall top
            // would only walk the bot back down into the gate; it fights there
            // once it gets there, and only what is at arm's length on the way.
            bool const passingGate = !atGround && DecideGateBypass(player, battleground, destination).HasWayPast();
            if (!passingGate)
                if (Unit* enemy = FindEnemyAtGround(player, ground, kDutyGateGuardRange))
                    if (playerbot::EngageSelectedEnemyPlayer(player, enemy, "gate-guard"))
                        return true;

            // Nobody at the gate: whatever the bot was fighting has gone, so
            // the chase it installed stands down and the bot walks back.
            if (Unit* victim = player->GetVictim(); victim && (passingGate
                ? !player->IsWithinDistInMap(victim, kDutyGatePassFightRange)
                : !victim->IsWithinDist3d(ground.GetPositionX(), ground.GetPositionY(), ground.GetPositionZ(), kDutyGateGuardRange)))
                player->AttackStop();

            if (!atGround)
                return MoveToBattlegroundObjective(player, battleground, destination) || player->isMoving();

            return true;
        }

        bool AdvanceAssaultBossDuty(Player* player, Battleground* battleground, playerbot::ObjectiveBotOrders const& orders)
        {
            Map* map = player->FindMap();
            Creature* boss = map ? map->GetCreature(orders.bossGuid) : nullptr;
            if (!boss || !boss->IsAlive())
                return false;

            // Into the keep through the hole the team broke in it: a bot still
            // further from the commander than that hole goes to the hole first,
            // so it does not path into a wall still standing.
            if (orders.bossHasApproach &&
                player->GetExactDist(boss) > orders.bossApproach.GetExactDist(boss) + 5.0f &&
                !player->IsWithinDist2d(&orders.bossApproach, kDutyGateStandRange))
                return MoveToBattlegroundObjective(player, battleground, orders.bossApproach) || player->isMoving();

            if (player->IsWithinDistInMap(boss, kDutyBossEngageRange) && player->IsWithinLOSInMap(boss))
                if (playerbot::EngageSelectedEnemyPlayer(player, boss, "battleground-commander"))
                    return true;

            return MoveToBattlegroundObjective(player, battleground, boss->GetPosition()) || player->isMoving();
        }
    }

    bool BattlegroundTacticalActions::ObjectiveDutyPrimitive(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        Battleground* battleground = player->GetBattleground();
        if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
            return false;

        playerbot::ObjectiveBotOrders orders;
        bool const hasOrders = playerbot::ObjectiveCoordinator::GetOrders(player, orders);

        // In a seat, the only thing a bot does is drive its siege vehicle. A
        // vehicle it no longer has a duty for it climbs out of - only one of
        // the battleground's, never a mount of its own.
        if (Unit* vehicleBase = player->GetVehicleBase())
        {
            if (hasOrders && orders.duty == playerbot::ObjectiveDuty::DriveSiege && vehicleBase->GetGUID() == orders.vehicleGuid)
                return DriveSiegeVehicle(player, vehicleBase, orders);
            if (hasOrders && orders.duty == playerbot::ObjectiveDuty::ManTurret && vehicleBase->GetGUID() == orders.vehicleGuid)
                return ManTurretVehicle(player, vehicleBase);

            if (vehicleBase->GetTypeId() == TYPEID_UNIT && battleground->GetSiegeObjectiveCount() > 0)
            {
                if (MotionMaster* motionMaster = vehicleBase->GetMotionMaster())
                    motionMaster->Clear();
                vehicleBase->StopMoving();
                player->ExitVehicle();
                return true;
            }

            return false;
        }

        if (!hasOrders)
            return false;

        switch (orders.duty)
        {
            case playerbot::ObjectiveDuty::CarryCharge:
                return AdvanceCarryChargeDuty(player, battleground, orders);
            case playerbot::ObjectiveDuty::DriveSiege:
            case playerbot::ObjectiveDuty::ManTurret:
                return BoardDutyVehicle(player, battleground, orders);
            case playerbot::ObjectiveDuty::GuardGate:
                return AdvanceGuardGateDuty(player, battleground, orders);
            case playerbot::ObjectiveDuty::AssaultBoss:
                return AdvanceAssaultBossDuty(player, battleground, orders);
            default:
                return false;
        }
    }

    namespace
    {
        // A bot sent somewhere that has not taken a step in this long is held by
        // something the orders cannot see. Summoner's Rift showed it as a bot
        // standing two minutes at its fountain after a recall or a respawn, its
        // post 400 yards off, with nothing in any log to say why.
        constexpr uint32 kOrdersStallMs = 6 * IN_MILLISECONDS;
        constexpr uint32 kOrdersStallLogMs = 30 * IN_MILLISECONDS;
        constexpr float kOrdersStallStep = 2.0f;
        // this close to where it was sent, a bot standing still is there
        constexpr float kOrdersStallNear = 6.0f;

        struct OrdersProgress
        {
            Position at;
            uint32 sinceMs = 0;
            uint32 lastMs = 0;
            uint32 loggedMs = 0;
        };

        std::unordered_map<uint64, OrdersProgress> g_ordersProgressByGuid;

        // The bot is not walking anywhere this tick (it fights, casts or holds
        // its post): standing still is right, so the stall clock starts over.
        void RestartOrdersProgress(Player* player)
        {
            if (OrdersProgress* progress = playerbot::LockedFind(g_ordersProgressByGuid, player->GetGUID().GetRawValue()))
                progress->sinceMs = 0;
        }

        // Called each tick the orders send the bot to `destination`. Once it has
        // stood kOrdersStallMs, one line says where it is, what holds its movement
        // and what a fresh route looks like, and the movement it holds is dropped
        // so this tick's order goes out new.
        void WatchOrdersProgress(Player* player, Position const& destination)
        {
            uint32 const nowMs = GameTime::GetGameTimeMS();
            OrdersProgress& progress = playerbot::LockedGetOrCreate(g_ordersProgressByGuid, player->GetGUID().GetRawValue());
            bool const fresh = !progress.sinceMs || nowMs - progress.lastMs > 2 * IN_MILLISECONDS;
            progress.lastMs = nowMs;
            if (fresh || player->GetExactDist2d(&progress.at) >= kOrdersStallStep)
            {
                progress.at = player->GetPosition();
                progress.sinceMs = nowMs;
                return;
            }
            if (nowMs - progress.sinceMs < kOrdersStallMs)
                return;

            // Rooted, stunned or feared: a spell holds it, not the movement.
            if (IsCrowdControlledForAction(player) || player->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED))
            {
                progress.sinceMs = nowMs;
                return;
            }

            uint32 const stoodMs = nowMs - progress.sinceMs;
            progress.sinceMs = nowMs;
            progress.at = player->GetPosition();

            MotionMaster* motionMaster = player->GetMotionMaster();
            if (!progress.loggedMs || nowMs - progress.loggedMs >= kOrdersStallLogMs)
            {
                progress.loggedMs = nowMs;
                MovementGenerator const* generator = motionMaster ? motionMaster->GetCurrentMovementGenerator() : nullptr;
                Position const safe = BuildCollisionSafeDestination(player, destination);
                PathGenerator path(player);
                path.SetPathLengthLimit(PLAYERBOT_BG_PATH_CALCULATION_LENGTH_LIMIT);
                bool const pathOk = path.CalculatePath(safe.GetPositionX(), safe.GetPositionY(), safe.GetPositionZ(), false);
                G3D::Vector3 const end = path.GetActualEndPosition();
                float const short2d = std::sqrt((end.x - safe.GetPositionX()) * (end.x - safe.GetPositionX()) +
                    (end.y - safe.GetPositionY()) * (end.y - safe.GetPositionY()));
                TC_LOG_INFO("playerbots.pvp.lifecycle",
                    "Battleground orders stall: {} ({}) on map {} stood {} s at ({:.1f}, {:.1f}, {:.1f}), {:.0f} yd from ({:.1f}, {:.1f}, {:.1f}); "
                    "motion {} flags 0x{:X}, spline {}, moving {}, casting {}, mounted {}, form {}; a route there: {} type 0x{:X}, {} points, "
                    "ends {:.1f} yd short. Movement dropped.",
                    player->GetName(), player->GetGUID().ToString(), player->GetMapId(), stoodMs / IN_MILLISECONDS,
                    player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetExactDist2d(&destination),
                    destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(),
                    generator ? uint32(generator->GetMovementGeneratorType()) : uint32(MAX_MOTION_TYPE), generator ? uint32(generator->Flags) : 0u,
                    !player->movespline->Initialized() ? "none" : player->movespline->Finalized() ? "finalized" : "running",
                    player->isMoving() ? 1 : 0, player->HasUnitState(UNIT_STATE_CASTING) ? 1 : 0, player->IsMounted() ? 1 : 0,
                    uint32(player->GetShapeshiftForm()), pathOk ? "ok" : "failed", uint32(path.GetPathType()), uint32(path.GetPath().size()), short2d);
            }

            if (motionMaster)
                motionMaster->Clear();
            player->StopMoving();
        }
    }

    // The battleground's orders, carried out: channel the recall it says is safe,
    // walk home without stopping when it says go home, fight what it names, or
    // hold the post it gives. A victim the orders no longer name - one that ran
    // under its turret, or anything at all on the way home - is let go of, so
    // neither the swing nor the class layer's spells (which follow the
    // selection) keep the bot on it.
    bool BattlegroundTacticalActions::FollowBattlegroundOrdersPrimitive(Player* player)
    {
        if (!player || !player->IsAlive() || !player->InBattleground())
            return false;
        Battleground* battleground = player->GetBattleground();
        if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
            return false;
        BattlegroundBotOrders orders;
        if (!battleground->GetBotOrders(player, orders))
            return false;

        Unit* target = !orders.Retreat && !orders.Target.IsEmpty() ? ObjectAccessor::GetUnit(*player, orders.Target) : nullptr;
        if (Unit* victim = player->GetVictim(); victim && victim != target)
            player->AttackStop();
        if (!target && !player->GetTarget().IsEmpty())
            player->SetSelection(ObjectGuid::Empty);

        // A cast in progress (the recall above all) is left to finish.
        if (player->HasUnitState(UNIT_STATE_CASTING))
        {
            RestartOrdersProgress(player);
            return true;
        }

        if (orders.RecallSpell)
        {
            player->StopMoving();
            if (MotionMaster* motionMaster = player->GetMotionMaster())
                motionMaster->Clear();
            if (player->CastSpell(player, orders.RecallSpell, false) == SPELL_CAST_OK)
            {
                RestartOrdersProgress(player);
                return true;
            }
        }

        if (orders.Retreat)
        {
            // home already, waiting to heal or to shop: standing is right
            if (player->IsWithinDist2d(&orders.Post, kOrdersStallNear))
                RestartOrdersProgress(player);
            else
                WatchOrdersProgress(player, orders.Post);
            return MoveToBattlegroundObjective(player, battleground, orders.Post) || player->isMoving();
        }

        if (target && playerbot::EngageSelectedEnemyPlayer(player, target, "battleground-orders"))
        {
            RestartOrdersProgress(player);
            return true;
        }

        if (!player->IsWithinDist2d(&orders.Post, 2.5f))
        {
            // a route's last vertex can stop a few yards short of the post
            if (player->IsWithinDist2d(&orders.Post, kOrdersStallNear))
                RestartOrdersProgress(player);
            else
                WatchOrdersProgress(player, orders.Post);
            return MoveToBattlegroundObjective(player, battleground, orders.Post) || player->isMoving();
        }
        RestartOrdersProgress(player);
        return true;
    }

    bool BattlegroundTacticalActions::PursueEnemyPrimitive(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        return TryPursueNearestEnemyInBattleground(player);
    }

    bool BattlegroundTacticalActions::ResetObjectiveForcePrimitive(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        return true;
    }

    bool BattlegroundTacticalActions::UseBuffPrimitive(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        return false;
    }

    bool BattlegroundTacticalActions::AttackEnemyFlagCarrierPrimitive(Player* player, BattlegroundTacticalContext const& context)
    {
        if (!player || !player->InBattleground())
            return false;

        if (context.flagCarrierDirective != FlagCarrierDirective::AttackEnemyCarrier)
            return false;

        Player* enemyCarrier = FindFlagCarrierForDirective(player, FlagCarrierDirective::AttackEnemyCarrier);
        if (enemyCarrier)
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP enemy-FC support selected: guid={} target={}.",
                player->GetGUID().ToString(), enemyCarrier->GetGUID().ToString());
        }

        return EngageSelectedEnemyPlayer(player, enemyCarrier, "enemy-flag-carrier");
    }

    bool BattlegroundTacticalActions::ProtectFlagCarrierPrimitive(Player* player, BattlegroundTacticalContext const& context)
    {
        if (!player || !player->InBattleground())
            return false;

        if (context.flagCarrierDirective != FlagCarrierDirective::ProtectTeamCarrier)
            return false;

        Player* teamCarrier = FindFlagCarrierForDirective(player, FlagCarrierDirective::ProtectTeamCarrier);
        if (teamCarrier)
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP protect-FC support selected: guid={} target={}.",
                player->GetGUID().ToString(), teamCarrier->GetGUID().ToString());
        }
        return MoveTowardUnit(player, teamCarrier, 18.0f);
    }

    namespace
    {
        constexpr float kCtfEscortFollowDistance = 8.0f;
        // An escort peels whatever threatens the player it guards, but is not
        // led across the map chasing it.
        constexpr float kCtfEscortPeelRange = 30.0f;
        constexpr float kCtfEscortLeashRange = 45.0f;
        constexpr float kCtfHandoffDropDistance = 5.0f;
        constexpr float kCtfHandoffMeetDistance = 2.5f;
        constexpr uint32 kCtfHandoffGiverRegrabDelayMs = 5 * IN_MILLISECONDS;
        // A defender owns our end of the map, not midfield.
        constexpr float kCtfDefenderZoneRadius = 60.0f;
        constexpr float kCtfDefenderScanRange = 45.0f;
        constexpr float kCtfPostTolerance = 8.0f;
        constexpr float kCtfSelfDefenseRange = 10.0f;
        // A runner waiting for the enemy flag fights anyone inside this radius
        // of the flag, from this far away.
        constexpr float kCtfStageRoomRadius = 40.0f;
        constexpr float kCtfStageFightRange = 30.0f;

        Player* FindCtfEscortThreat(Player* escort, Player* escorted)
        {
            Map* map = escort->FindMap();
            if (!map)
                return nullptr;

            TeamId const escortTeam = ResolveBotTeamId(escort);
            Player* best = nullptr;
            float bestScore = std::numeric_limits<float>::max();
            Map::PlayerList const& players = map->GetPlayers();
            for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
            {
                Player* enemy = itr->GetSource();
                if (!enemy || enemy == escort || !enemy->IsAlive() || enemy->GetBattlegroundId() != escort->GetBattlegroundId())
                    continue;
                if (ResolveBotTeamId(enemy) == escortTeam)
                    continue;

                float const distanceToEscorted = enemy->GetDistance(escorted);
                if (distanceToEscorted > kCtfEscortPeelRange || !escort->IsWithinDistInMap(enemy, kCtfEscortLeashRange))
                    continue;
                if (!escort->IsValidAttackTarget(enemy) || PvpCore::IsEffectivelyImmuneTarget(escort, enemy))
                    continue;

                // Whoever is already hitting the escorted player comes first.
                float const score = distanceToEscorted - (enemy->GetVictim() == escorted ? 100.0f : 0.0f);
                if (score < bestScore)
                {
                    best = enemy;
                    bestScore = score;
                }
            }

            return best;
        }
    }

    bool BattlegroundTacticalActions::FlagHandoffPrimitive(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        CtfBotOrders orders;
        if (!CtfCoordinator::GetOrders(player, orders) || orders.handoffPartnerGuid.IsEmpty() ||
            (!orders.handoffGive && !orders.handoffReceive))
            return false;

        Player* partner = ObjectAccessor::GetPlayer(*player, orders.handoffPartnerGuid);
        if (!partner || !partner->IsAlive())
            return false;

        if (orders.handoffGive && player->IsWithinDistInMap(partner, kCtfHandoffDropDistance))
        {
            if (!CtfCoordinator::DropFlagForHandoff(player))
                return false;

            // The runner gets the flag, not a race for it.
            DelayFlagPickup(player, kCtfHandoffGiverRegrabDelayMs);
            StopVirtualPlayerbotMovement(player);
            return true;
        }

        // Walk into each other. The carrier drops on its own tick once they
        // meet; the runner's pickup then runs through the nearby-flag path.
        if (player->GetExactDist(partner) <= kCtfHandoffMeetDistance)
        {
            if (player->isMoving())
                StopVirtualPlayerbotMovement(player);
            return true;
        }

        if (!CanIssueBotMovement(player))
            return false;

        return IssueMovePointThrottled(player, partner->GetPosition(), 2.0f, 500) || player->isMoving();
    }

    bool BattlegroundTacticalActions::EscortFlagRunnerPrimitive(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        CtfBotOrders orders;
        if (!CtfCoordinator::GetOrders(player, orders))
            return TryPursueNearestEnemyInBattleground(player);

        // Our carrier first - human or bot - and otherwise ride with the runner.
        ObjectGuid escortedGuid;
        if (!orders.teamCarrierGuid.IsEmpty() && orders.teamCarrierGuid != player->GetGUID())
            escortedGuid = orders.teamCarrierGuid;
        else if (!orders.runnerGuid.IsEmpty() && orders.runnerGuid != player->GetGUID())
            escortedGuid = orders.runnerGuid;

        Player* escorted = escortedGuid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*player, escortedGuid);
        if (!escorted || !escorted->IsAlive())
            return TryPursueNearestEnemyInBattleground(player);

        if (Player* threat = FindCtfEscortThreat(player, escorted))
            return EngageSelectedEnemyPlayer(player, threat, "escort-flag-runner");

        return MoveTowardUnit(player, escorted, kCtfEscortFollowDistance);
    }

    bool BattlegroundTacticalActions::DefendFlagRoomPrimitive(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        Battleground* battleground = player->GetBattleground();
        CtfBotOrders orders;
        if (!battleground || !CtfCoordinator::GetOrders(player, orders))
            return TryPursueNearestEnemyInBattleground(player);

        // Anything in our end of the map, and anything already on the defender.
        if (Player* intruder = FindNearestEnemyBattlegroundPlayer(player, kCtfDefenderScanRange, nullptr, nullptr))
        {
            bool const inOurEnd = intruder->GetExactDist(orders.ownFlagStand) <= kCtfDefenderZoneRadius;
            bool const onDefender = intruder->GetVictim() == player || player->IsWithinDistInMap(intruder, kCtfSelfDefenseRange);
            if (inOurEnd || onDefender)
                return EngageSelectedEnemyPlayer(player, intruder, "defend-flag-room");
        }

        Position post = orders.ownFlagStand;
        ApplyDeterministicObjectiveOffset(battleground, player, post);
        if (player->IsWithinDist3d(post.GetPositionX(), post.GetPositionY(), post.GetPositionZ(), kCtfPostTolerance))
            return true;

        if (!CanIssueBotMovement(player))
            return false;

        return IssueMovePointThrottled(player, post) || player->isMoving();
    }

    bool BattlegroundTacticalActions::FlagRunnerStagePrimitive(Player* player)
    {
        if (!player || !player->InBattleground())
            return false;

        Battleground* battleground = player->GetBattleground();
        CtfBotOrders orders;
        if (!battleground || !CtfCoordinator::GetOrders(player, orders))
            return TryPursueNearestEnemyInBattleground(player);

        // In the enemy flag room, fight for the room: anyone in it is engaged
        // and stays engaged until they leave it. Holding a spot instead made a
        // melee runner chase a defender off the spot, walk back, and turn
        // around again every couple of seconds.
        if (Player* enemy = FindNearestEnemyBattlegroundPlayer(player, kCtfStageFightRange, nullptr, nullptr))
        {
            bool const inEnemyFlagRoom = enemy->GetExactDist(orders.enemyFlagStand) <= kCtfStageRoomRadius;
            bool const onRunner = enemy->GetVictim() == player && player->IsWithinDistInMap(enemy, kCtfSelfDefenseRange);
            // On the way over, only a fight that is already on the runner is taken.
            if (inEnemyFlagRoom || onRunner)
                return EngageSelectedEnemyPlayer(player, enemy, inEnemyFlagRoom ? "flag-runner-fights-for-flag-room" : "flag-runner-self-defense");
        }

        // Wait at the enemy flag for it to come back.
        Position post = orders.enemyFlagStand;
        ApplyDeterministicObjectiveOffset(battleground, player, post);
        if (player->IsWithinDist3d(post.GetPositionX(), post.GetPositionY(), post.GetPositionZ(), kCtfPostTolerance))
            return true;

        if (!CanIssueBotMovement(player))
            return false;

        return IssueMovePointThrottled(player, post) || player->isMoving();
    }

    bool ArenaLifecycleActions::Execute(Player* player, ArenaLifecycleContext const& context)
    {
        if (!player || !context.lifecycleEnabled || !IsLifecycleGateEnabled())
            return false;

        if (RecoverStaleBattlegroundState(player))
            return true;

        if (HasConflictingArenaLifecycleContext(context))
        {
            TC_LOG_DEBUG("playerbots.pvp.lifecycle",
                "Playerbot PvP arena lifecycle no-op due to conflicting context: guid={}, queueOperation={}, teamInteraction={}.",
                player->GetGUID().ToString(), static_cast<uint8>(context.queueOperation), static_cast<uint8>(context.teamInteraction));
            return false;
        }

        bool didExecute = false;

        switch (context.queueOperation)
        {
        case QueueOperationType::Join:
            didExecute = JoinQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::Leave:
            didExecute = LeaveQueuePrimitive(player) || didExecute;
            break;
        case QueueOperationType::None:
        default:
            break;
        }

        switch (context.teamInteraction)
        {
        case ArenaTeamInteractionType::AcceptInvite:
            didExecute = AcceptTeamInvitePrimitive(player) || didExecute;
            break;
        case ArenaTeamInteractionType::DeclineInvite:
            didExecute = DeclineTeamInvitePrimitive(player) || didExecute;
            break;
        case ArenaTeamInteractionType::None:
        default:
            break;
        }

        // Warsong-only managed bot policy: never auto-accept arena invitations.
        return didExecute;
    }

    bool ArenaLifecycleActions::JoinQueuePrimitive(Player* player)
    {
        (void)player;
        return false;
    }

    bool ArenaLifecycleActions::LeaveQueuePrimitive(Player* player)
    {
        if (!player || !IsLifecycleGateEnabled())
            return false;

        return RemoveMatchingQueues(player, true, false, false);
    }

    bool ArenaLifecycleActions::AcceptTeamInvitePrimitive(Player* player)
    {
        if (!player || !IsLifecycleGateEnabled())
            return false;

        uint32 const invitedArenaTeamId = player->GetArenaTeamIdInvited();
        if (!invitedArenaTeamId)
            return false;

        ArenaTeam* arenaTeam = sArenaTeamMgr->GetArenaTeamById(invitedArenaTeamId);
        if (!arenaTeam)
            return false;

        if (arenaTeam->GetMember(player->GetGUID()))
            return false;

        if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD) &&
            player->GetTeam() != sCharacterCache->GetCharacterTeamByGuid(arenaTeam->GetCaptain()))
            return false;

        return arenaTeam->AddMember(player->GetGUID());
    }

    bool ArenaLifecycleActions::DeclineTeamInvitePrimitive(Player* player)
    {
        if (!player || !IsLifecycleGateEnabled())
            return false;

        if (!player->GetArenaTeamIdInvited())
            return false;

        player->SetArenaTeamIdInvited(0);
        return true;
    }

    bool DuelTacticalActions::Execute(Player* player)
    {
        if (!player || !player->IsAlive())
            return false;

        BreakExpiredHunterFeignDeath(player);

        if (playerbot::IsHunterAutoShotPlantActive(player))
            return true;

        if (!CanIssueBotMovement(player))
            return false;

        if (!player->duel)
            return false;

        // The PvE tick has already let go of the bot, so without this it keeps
        // walking its old path through the countdown - and can carry itself out
        // of the duel area before the fight starts.
        if (player->duel->State == DUEL_STATE_COUNTDOWN)
        {
            // Clear the slot too: a travel/follow generator left in it would
            // simply relaunch its spline on the next update.
            MotionMaster* motionMaster = player->GetMotionMaster();
            bool const hasTravelGenerator = motionMaster->GetCurrentSlot() == MOTION_SLOT_ACTIVE;
            if (hasTravelGenerator)
                motionMaster->Clear(MOTION_SLOT_ACTIVE);
            if (hasTravelGenerator || player->isMoving())
                StopVirtualPlayerbotMovement(player);
            if (Player* opponent = player->duel->Opponent)
                player->SetFacingToObject(opponent);
            return true;
        }

        if (player->duel->State != DUEL_STATE_IN_PROGRESS)
            return false;

        return EngageNearestEnemyPlayer(player, GetAggressiveCombatScanDistance(player, 100.0f));
    }
}
