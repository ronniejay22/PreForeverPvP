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

#include <atomic>
#include <limits>

#include "Log.h"
#include "Configuration/Config.h"
#include "Chat.h"
#include "Formulas.h"
#include "CharacterCache.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Globals/ObjectAccessor.h"
#include "Item.h"
#include "Map.h"
#include "Miscellaneous/BotPvePower.h"
#include "Miscellaneous/TournamentMode.h"
#include "MotionMaster.h"
#include "Optional.h"
#include "MoveSpline.h"
#include "Movement/AbstractFollower.h"
#include "Player.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"
#include "Playerbot/Pvp/PlayerbotBgFillDriver.h"
#include "Playerbot/Pvp/PlayerbotCtfCoordinator.h"
#include "Playerbot/Pvp/PlayerbotNodeCoordinator.h"
#include "Playerbot/Pvp/PlayerbotObjectiveCoordinator.h"
#include "Playerbot/Pvp/PlayerbotObcClone.h"
#include "Playerbot/Pvp/PlayerbotVhrWaveDriver.h"
#include "Playerbot/Pvp/PlayerbotPvpClassActions.h"
#include "Playerbot/Pvp/PlayerbotPvpCore.h"
#include "Playerbot/Pvp/PlayerbotPvpLifecycleActions.h"
#include "Playerbot/Pvp/PlayerbotRandomBotParticipation.h"
#include "Playerbot/Pvp/PlayerbotResourceGovernor.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellHistory.h"
#include "SpellMgr.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cctype>

using namespace Trinity::ChatCommands;

namespace
{
constexpr uint32 PLAYERBOT_REDUCED_MAGMA_DAMAGE_SPELL_ID = 57634;

bool IsChromieWhisperFacade(Player const* player)
{
    if (!player)
        return false;

    std::string const& name = player->GetName();
    return name == "Chromie" || name == "Chromi";
}

// The two players are seated in the same battleground or arena instance. Used
// to tell an order given inside a match, by somebody playing it, from a
// whisper out in the world.
bool SharesMatch(Player const* sender, Player const* receiver)
{
    if (!sender || !receiver)
        return false;

    Battleground const* battleground = sender->GetBattleground();
    return battleground && receiver->GetBattleground() == battleground;
}

char const* ToString(playerbot::BattlegroundState state)
{
    switch (state)
    {
        case playerbot::BattlegroundState::Queueing: return "queueing";
        case playerbot::BattlegroundState::WaitingToStart: return "waiting";
        case playerbot::BattlegroundState::Active: return "active";
        case playerbot::BattlegroundState::None:
        default: return "none";
    }
}

char const* ToString(playerbot::QueueOperationType op)
{
    switch (op)
    {
        case playerbot::QueueOperationType::Join: return "join";
        case playerbot::QueueOperationType::Leave: return "leave";
        case playerbot::QueueOperationType::None:
        default: return "none";
    }
}

char const* ToString(playerbot::InvitationResponseType response)
{
    switch (response)
    {
        case playerbot::InvitationResponseType::Accept: return "accept";
        case playerbot::InvitationResponseType::Decline: return "decline";
        case playerbot::InvitationResponseType::None:
        default: return "none";
    }
}

char const* ToString(playerbot::PvpClassSpellContext::MovementDirective directive)
{
    switch (directive)
    {
        case playerbot::PvpClassSpellContext::MovementDirective::ReachMeleeRange: return "reach_melee";
        case playerbot::PvpClassSpellContext::MovementDirective::ReachSpellRange: return "reach_spell";
        case playerbot::PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell: return "flee";
        case playerbot::PvpClassSpellContext::MovementDirective::FaceSpellTarget: return "face_target";
        case playerbot::PvpClassSpellContext::MovementDirective::DropInvalidTarget: return "drop_target";
        case playerbot::PvpClassSpellContext::MovementDirective::CheckMountState: return "check_mount";
        case playerbot::PvpClassSpellContext::MovementDirective::LeaveShapeshiftForm: return "leave_form";
        case playerbot::PvpClassSpellContext::MovementDirective::ResetCombatState: return "reset_combat";
        case playerbot::PvpClassSpellContext::MovementDirective::None:
        default: return "none";
    }
}

char const* ToString(playerbot::PvpClassSpellContext::TargetMode mode)
{
    switch (mode)
    {
        case playerbot::PvpClassSpellContext::TargetMode::Enemy: return "enemy";
        case playerbot::PvpClassSpellContext::TargetMode::Ally: return "ally";
        case playerbot::PvpClassSpellContext::TargetMode::Self: return "self";
        case playerbot::PvpClassSpellContext::TargetMode::Pet: return "pet";
        case playerbot::PvpClassSpellContext::TargetMode::None:
        default: return "none";
    }
}

char const* ToString(MovementGeneratorType motionType)
{
    switch (motionType)
    {
        case IDLE_MOTION_TYPE: return "idle";
        case RANDOM_MOTION_TYPE: return "random";
        case WAYPOINT_MOTION_TYPE: return "waypoint";
        case CONFUSED_MOTION_TYPE: return "confused";
        case CHASE_MOTION_TYPE: return "chase";
        case HOME_MOTION_TYPE: return "home";
        case FLIGHT_MOTION_TYPE: return "flight";
        case POINT_MOTION_TYPE: return "point";
        case FLEEING_MOTION_TYPE: return "fleeing";
        case DISTRACT_MOTION_TYPE: return "distract";
        case ASSISTANCE_MOTION_TYPE: return "assist";
        case ASSISTANCE_DISTRACT_MOTION_TYPE: return "assist_distract";
        case TIMED_FLEEING_MOTION_TYPE: return "timed_flee";
        case FOLLOW_MOTION_TYPE: return "follow";
        case ROTATE_MOTION_TYPE: return "rotate";
        case EFFECT_MOTION_TYPE: return "effect";
        case SPLINE_CHAIN_MOTION_TYPE: return "spline_chain";
        case FORMATION_MOTION_TYPE: return "formation";
        case MAX_MOTION_TYPE:
        default:
            return "unknown";
    }
}

struct ManagedBotUpdatePulseState
{
    uint64 tickCount = 0;
    uint32 lastUpdateMs = 0;
    uint32 lastDiff = 0;
    uint32 lastProgressMs = 0;
    uint32 lastLogMs = 0;
    ObjectGuid motionTargetGuid = ObjectGuid::Empty;
    float lastX = 0.0f;
    float lastY = 0.0f;
    float lastZ = 0.0f;
    float lastDistance = 0.0f;
    float lastProgressX = 0.0f;
    float lastProgressY = 0.0f;
    float lastProgressZ = 0.0f;
    MovementGeneratorType lastMotionType = IDLE_MOTION_TYPE;
    bool lastMoving = false;
    bool lastChaseMove = false;
    bool lastFollowMove = false;
    bool lastSplineInitialized = false;
    bool lastSplineFinalized = true;
    bool lastSplineStarted = false;
    int32 lastSplineIndex = 0;
    int32 lastSplineDuration = 0;
    float lastSplineVelocity = 0.0f;
};

std::unordered_map<uint64, ManagedBotUpdatePulseState> g_ManagedBotUpdatePulseByGuid;
std::mutex g_ManagedBotUpdatePulseLock;

// A managed bot's income IS its whole economy: it buys its own gear, bags,
// ammunition, food and auction stock out of what it earns, and it has no
// player behind it to subsidise a bad run. At face value the fleet cannot
// keep pace with the gear it outgrows, which is what left bots grinding in
// white kit.
//
// Applied to world-faucet gains only: looted coin here and NPC-vendor payouts
// in the PvE manager. Auction settlements stay at face value because they have
// a real buyer on the other side. Split into two configurable level bands.
std::atomic<float> g_PlayerbotGoldGainMultiplier{ 2.0f };
std::atomic<float> g_PlayerbotLowLevelGoldGainMultiplier{ 2.0f };
std::atomic<uint32> g_PlayerbotLowLevelGoldBandMaxLevel{ 10 };

void LoadPlayerbotGoldGainMultiplier()
{
    g_PlayerbotGoldGainMultiplier.store(
        std::max(0.0f, sConfigMgr->GetFloatDefault("Playerbot.GoldGainMultiplier", 2.0f)),
        std::memory_order_relaxed);

    g_PlayerbotLowLevelGoldGainMultiplier.store(
        std::max(0.0f, sConfigMgr->GetFloatDefault("Playerbot.GoldGainMultiplier.LowLevel", 2.0f)),
        std::memory_order_relaxed);

    g_PlayerbotLowLevelGoldBandMaxLevel.store(
        uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbot.GoldGainMultiplier.LowLevelMaxLevel", 10))),
        std::memory_order_relaxed);
}

} // anonymous namespace

namespace playerbot
{
// The multiplier for the band this bot's level falls in.
//
// Outside the anonymous namespace above because the vendor payout in the PvE
// manager needs the same answer, and a second copy of the band logic would
// eventually disagree with this one about where the boundary is. The atomics it
// reads stay file-local.
float PlayerbotGoldGainMultiplierFor(Player const* player)
{
    return uint32(player->GetLevel()) <= g_PlayerbotLowLevelGoldBandMaxLevel.load(std::memory_order_relaxed)
        ? g_PlayerbotLowLevelGoldGainMultiplier.load(std::memory_order_relaxed)
        : g_PlayerbotGoldGainMultiplier.load(std::memory_order_relaxed);
}
}

namespace
{

Unit* GetCurrentMotionTarget(Player* bot)
{
    if (!bot)
        return nullptr;

    if (MotionMaster* motionMaster = bot->GetMotionMaster())
        if (MovementGenerator* movement = motionMaster->GetCurrentMovementGenerator())
            if (AbstractFollower* follower = dynamic_cast<AbstractFollower*>(movement))
                return follower->GetTarget();

    return nullptr;
}

void AppendSplineSnapshot(std::ostringstream& os, Player const* bot, char const* prefix)
{
    char const* pfx = prefix ? prefix : "spline";
    if (!bot || !bot->movespline)
    {
        os << ' ' << pfx << "_spline=null";
        return;
    }

    os << ' ' << pfx << "_spline_init=" << (bot->movespline->Initialized() ? "yes" : "no")
       << ' ' << pfx << "_spline_done=" << (bot->movespline->Finalized() ? "yes" : "no")
       << ' ' << pfx << "_spline_started=" << (bot->movespline->HasStarted() ? "yes" : "no")
       << ' ' << pfx << "_spline_idx=" << bot->movespline->currentPathIdx()
       << ' ' << pfx << "_spline_duration=" << bot->movespline->Duration()
       << ' ' << pfx << "_spline_velocity=" << bot->movespline->Velocity();
}

void RecordManagedBotUpdatePulse(Player* bot, uint32 diff)
{
    // Pure diagnostics feeding the stall log and the whisper diag command:
    // per-tick mutex + hash-map + spline bookkeeping for every player. Skip
    // it entirely unless someone is actually watching the motion debug log.
    if (!sLog->ShouldLog("playerbots.pvp.motion", LOG_LEVEL_DEBUG))
        return;

    if (!bot || !playerbot::IsManagedRandomBot(bot))
        return;

    std::lock_guard<std::mutex> pulseLock(g_ManagedBotUpdatePulseLock);

    uint32 const nowMs = GameTime::GetGameTimeMS();
    ManagedBotUpdatePulseState& state = g_ManagedBotUpdatePulseByGuid[bot->GetGUID().GetRawValue()];
    Unit* motionTarget = GetCurrentMotionTarget(bot);
    ObjectGuid const motionTargetGuid = motionTarget ? motionTarget->GetGUID() : ObjectGuid::Empty;
    float const currentDistance = motionTarget ? bot->GetDistance(motionTarget) : 0.0f;
    MovementGeneratorType const motionType = bot->GetMotionMaster() ? bot->GetMotionMaster()->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;

    bool const firstSample = state.tickCount == 0;
    bool const targetChanged = state.motionTargetGuid != motionTargetGuid;
    float const dx = bot->GetPositionX() - state.lastX;
    float const dy = bot->GetPositionY() - state.lastY;
    float const dz = bot->GetPositionZ() - state.lastZ;
    float const posDelta2D = std::sqrt(dx * dx + dy * dy);
    float const posDelta3D = std::sqrt(dx * dx + dy * dy + dz * dz);
    bool const distanceImproved = motionTarget && state.lastDistance > 0.0f && currentDistance + 0.10f < state.lastDistance;
    bool const positionMoved = !firstSample && (posDelta2D >= 0.05f || posDelta3D >= 0.10f);

    if (firstSample || targetChanged || distanceImproved || positionMoved)
    {
        state.lastProgressMs = nowMs;
        state.lastProgressX = bot->GetPositionX();
        state.lastProgressY = bot->GetPositionY();
        state.lastProgressZ = bot->GetPositionZ();
    }

    state.tickCount++;
    state.lastUpdateMs = nowMs;
    state.lastDiff = diff;
    state.motionTargetGuid = motionTargetGuid;
    state.lastX = bot->GetPositionX();
    state.lastY = bot->GetPositionY();
    state.lastZ = bot->GetPositionZ();
    state.lastDistance = currentDistance;
    state.lastMotionType = motionType;
    state.lastMoving = bot->isMoving();
    state.lastChaseMove = bot->HasUnitState(UNIT_STATE_CHASE_MOVE);
    state.lastFollowMove = bot->HasUnitState(UNIT_STATE_FOLLOW_MOVE);
    state.lastSplineInitialized = bot->movespline && bot->movespline->Initialized();
    state.lastSplineFinalized = !bot->movespline || bot->movespline->Finalized();
    state.lastSplineStarted = bot->movespline && bot->movespline->HasStarted();
    state.lastSplineIndex = bot->movespline ? bot->movespline->currentPathIdx() : -1;
    state.lastSplineDuration = bot->movespline ? bot->movespline->Duration() : 0;
    state.lastSplineVelocity = bot->movespline ? bot->movespline->Velocity() : 0.0f;

    bool const targetRelativeMotion = motionType == CHASE_MOTION_TYPE || motionType == FOLLOW_MOTION_TYPE;
    bool const claimsMovement = bot->isMoving() || bot->HasUnitState(UNIT_STATE_CHASE_MOVE) || bot->HasUnitState(UNIT_STATE_FOLLOW_MOVE) || (bot->movespline && !bot->movespline->Finalized());
    uint32 const noProgressMs = state.lastProgressMs != 0 && nowMs >= state.lastProgressMs ? nowMs - state.lastProgressMs : 0;
    if (targetRelativeMotion && claimsMovement && noProgressMs >= 1000 && (state.lastLogMs == 0 || nowMs >= state.lastLogMs + 1000))
    {
        state.lastLogMs = nowMs;
        std::ostringstream diag;
        diag << "PB update pulse stalled: bot=" << bot->GetName()
             << " guid=" << bot->GetGUID().ToString()
             << " diff=" << diff
             << " ticks=" << state.tickCount
             << " no_progress_ms=" << noProgressMs
             << " motion=" << uint32(motionType)
             << " moving=" << (bot->isMoving() ? "yes" : "no")
             << " chase_move=" << (bot->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
             << " follow_move=" << (bot->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
             << " not_move=" << (bot->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
             << " root=" << (bot->HasUnitState(UNIT_STATE_ROOT) ? "yes" : "no")
             << " stunned=" << (bot->HasUnitState(UNIT_STATE_STUNNED) ? "yes" : "no")
             << " casting_prevent=" << (bot->IsMovementPreventedByCasting() ? "yes" : "no")
             << " pos=(" << bot->GetMapId() << ':' << bot->GetPositionX() << ',' << bot->GetPositionY() << ',' << bot->GetPositionZ() << ')';
        if (motionTarget)
            diag << " motion_target=" << motionTarget->GetName() << " motion_target_dist=" << currentDistance;
        else
            diag << " motion_target=none";
        AppendSplineSnapshot(diag, bot, "pulse");
        TC_LOG_DEBUG("playerbots.pvp.motion", "{}", diag.str());
    }
}

std::string BuildManagedBotUpdateDiagnosticLine(Player* bot)
{
    if (!bot)
        return "PB update diag unavailable.";

    uint32 const nowMs = GameTime::GetGameTimeMS();
    ManagedBotUpdatePulseState state;
    {
        std::lock_guard<std::mutex> pulseLock(g_ManagedBotUpdatePulseLock);
        auto itr = g_ManagedBotUpdatePulseByGuid.find(bot->GetGUID().GetRawValue());
        if (itr == g_ManagedBotUpdatePulseByGuid.end())
            return "PB update diag: no-update-pulse-recorded";

        state = itr->second;
    }
    uint32 const updateAgeMs = state.lastUpdateMs != 0 && nowMs >= state.lastUpdateMs ? nowMs - state.lastUpdateMs : 0;
    uint32 const progressAgeMs = state.lastProgressMs != 0 && nowMs >= state.lastProgressMs ? nowMs - state.lastProgressMs : 0;
    float const progressDx = bot->GetPositionX() - state.lastProgressX;
    float const progressDy = bot->GetPositionY() - state.lastProgressY;
    float const progressDz = bot->GetPositionZ() - state.lastProgressZ;
    float const progressDelta2D = std::sqrt(progressDx * progressDx + progressDy * progressDy);
    float const progressDelta3D = std::sqrt(progressDx * progressDx + progressDy * progressDy + progressDz * progressDz);

    std::ostringstream diag;
    diag << "PB update diag:"
         << " ticks=" << state.tickCount
         << " last_update_age_ms=" << updateAgeMs
         << " last_diff=" << state.lastDiff
         << " progress_age_ms=" << progressAgeMs
         << " progress_delta_2d=" << progressDelta2D
         << " progress_delta_3d=" << progressDelta3D
         << " last_motion=" << uint32(state.lastMotionType)
         << " last_moving=" << (state.lastMoving ? "yes" : "no")
         << " last_chase=" << (state.lastChaseMove ? "yes" : "no")
         << " last_follow=" << (state.lastFollowMove ? "yes" : "no")
         << " last_spline_init=" << (state.lastSplineInitialized ? "yes" : "no")
         << " last_spline_done=" << (state.lastSplineFinalized ? "yes" : "no")
         << " last_spline_started=" << (state.lastSplineStarted ? "yes" : "no")
         << " last_spline_idx=" << state.lastSplineIndex
         << " last_spline_duration=" << state.lastSplineDuration
         << " last_spline_velocity=" << state.lastSplineVelocity;

    Unit* motionTarget = GetCurrentMotionTarget(bot);
    if (motionTarget)
        diag << " motion_target=" << motionTarget->GetName() << " motion_target_dist=" << bot->GetDistance(motionTarget);
    else
        diag << " motion_target=none";

    AppendSplineSnapshot(diag, bot, "now");
    return diag.str();
}

std::string BuildManagedBotStatusLine(Player* bot)
{
    if (!bot)
        return "Playerbot status unavailable.";

    playerbot::PvpValues const values = playerbot::PvpCore::CollectValues(bot);
    playerbot::PvpClassSpellContext const classContext = playerbot::PvpCore::BuildClassSpellContext(bot, values);
    playerbot::BattlegroundLifecycleContext const lifecycleContext = playerbot::PvpCore::BuildBattlegroundLifecycleContext(bot, values);
    playerbot::RandomBotParticipationHooks const hooks = playerbot::PvpCore::BuildRandomBotParticipationHooks(bot, values);
    Unit* directiveTarget = classContext.movementTargetGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*bot, classContext.movementTargetGuid);
    std::string const lastClassExecutionStatus = playerbot::PvpClassActions::GetLastExecutionStatus(bot);
    bool const lifecycleEnabled = lifecycleContext.lifecycleEnabled;
    bool const managedRandomBot = playerbot::IsManagedRandomBot(bot);
    bool const canFollowCommands = bot->IsAlive() &&
        !bot->HasUnitState(UNIT_STATE_ROOT) &&
        !bot->HasUnitState(UNIT_STATE_STUNNED) &&
        !bot->HasUnitState(UNIT_STATE_CONFUSED) &&
        !bot->HasUnitState(UNIT_STATE_FLEEING) &&
        !bot->HasUnitState(UNIT_STATE_LOST_CONTROL);

    std::ostringstream status;
    MovementGeneratorType const motionType = bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
    uint32 const movementFlags = bot->GetUnitMovementFlags();
    status << "PB status: "
           << "lifecycle=" << (lifecycleEnabled ? "on" : "off")
           << " managed=" << (managedRandomBot ? "yes" : "no")
           << "combat=" << (bot->IsInCombat() ? "yes" : "no")
           << " alive=" << (bot->IsAlive() ? "yes" : "no")
           << " moving=" << (bot->isMoving() ? "yes" : "no")
           << " can_follow=" << (canFollowCommands ? "yes" : "no")
           << " rooted=" << (bot->HasUnitState(UNIT_STATE_ROOT) ? "yes" : "no")
           << " stunned=" << (bot->HasUnitState(UNIT_STATE_STUNNED) ? "yes" : "no")
           << " confused=" << (bot->HasUnitState(UNIT_STATE_CONFUSED) ? "yes" : "no")
           << " fleeing=" << (bot->HasUnitState(UNIT_STATE_FLEEING) ? "yes" : "no")
           << " lost_control=" << (bot->HasUnitState(UNIT_STATE_LOST_CONTROL) ? "yes" : "no")
           << " stealth=" << (bot->HasStealthAura() ? "yes" : "no")
           << " casting=" << (bot->IsNonMeleeSpellCast(false, false, true) ? "yes" : "no")
           << " not_move=" << (bot->HasUnitState(UNIT_STATE_NOT_MOVE) ? "yes" : "no")
           << " chase_state=" << (bot->HasUnitState(UNIT_STATE_CHASE) ? "yes" : "no")
           << " chase_move=" << (bot->HasUnitState(UNIT_STATE_CHASE_MOVE) ? "yes" : "no")
           << " follow_state=" << (bot->HasUnitState(UNIT_STATE_FOLLOW) ? "yes" : "no")
           << " follow_move=" << (bot->HasUnitState(UNIT_STATE_FOLLOW_MOVE) ? "yes" : "no")
           << " casting_prevent=" << (bot->IsMovementPreventedByCasting() ? "yes" : "no")
           << " bg_state=" << ToString(values.battlegroundState)
           << " class_gate=" << (classContext.classSpellsEnabled ? "on" : "off")
           << " class_exec=" << (classContext.shouldExecute ? "yes" : "no")
           << " class_action=" << (classContext.actionName ? classContext.actionName : "none")
           << " spell=" << classContext.spellId
           << " reason=" << (classContext.reason ? classContext.reason : "none")
           << " target_mode=" << ToString(classContext.targetMode)
           << " target_guid=" << classContext.targetGuid.ToString()
           << " move_directive=" << ToString(classContext.movementDirective)
           << " move_target=" << classContext.movementTargetGuid.ToString()
           << " move_target_resolved=" << (directiveTarget ? "yes" : "no")
           << " move_range=" << classContext.movementFollowRange
           << " lifecycle_q=" << ToString(lifecycleContext.queueOperation)
           << " lifecycle_invite=" << ToString(lifecycleContext.invitationResponse)
           << " hooks(bg=" << (hooks.battlegroundParticipationHook ? "on" : "off")
           << ",arena=" << (hooks.arenaParticipationHook ? "on" : "off") << ")"
           << " last_exec=" << lastClassExecutionStatus
           << " strict_pathing=" << (bot->InBattleground() ? "on" : "off")
           << " motion=" << uint32(motionType)
           << "/" << ToString(motionType)
           << " move_flags=0x" << std::hex << movementFlags << std::dec
           << " pos=(" << bot->GetMapId() << ":" << bot->GetPositionX() << "," << bot->GetPositionY() << "," << bot->GetPositionZ() << ")"
           << " o=" << bot->GetOrientation();

    if (Unit* victim = bot->GetVictim())
        status << " victim=" << victim->GetName() << " victim_dist=" << bot->GetDistance(victim);
    else
        status << " victim=none";

    Unit* motionTarget = nullptr;
    if (MotionMaster* motionMaster = bot->GetMotionMaster())
        if (MovementGenerator* movement = motionMaster->GetCurrentMovementGenerator())
            if (AbstractFollower* follower = dynamic_cast<AbstractFollower*>(movement))
                motionTarget = follower->GetTarget();

    if (motionTarget)
    {
        bool const motionTargetMatchesVictim = bot->GetVictim() && bot->GetVictim()->GetGUID() == motionTarget->GetGUID();
        bool const chaseVictimMismatch = bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE && !motionTargetMatchesVictim;
        float const motionTargetExactDist = bot->GetExactDist(motionTarget);
        float const motionTargetHitboxSum = bot->GetCombatReach() + motionTarget->GetCombatReach();
        status << " motion_target=" << motionTarget->GetName()
               << " motion_target_dist=" << bot->GetDistance(motionTarget)
               << " motion_target_exact=" << motionTargetExactDist
               << " motion_target_hitbox_sum=" << motionTargetHitboxSum
               << " motion_target_edge_over_range=" << (bot->GetDistance(motionTarget) - classContext.movementFollowRange)
               << " motion_target_is_victim=" << (motionTargetMatchesVictim ? "yes" : "no")
               << " chase_victim_mismatch=" << (chaseVictimMismatch ? "yes" : "no");
    }
    else
    {
        status << " motion_target=none";
    }

    if (directiveTarget)
    {
        status << " directive_target=" << directiveTarget->GetName()
               << " directive_target_dist=" << bot->GetDistance(directiveTarget)
               << " directive_target_exact=" << bot->GetExactDist(directiveTarget)
               << " directive_target_hitbox_sum=" << (bot->GetCombatReach() + directiveTarget->GetCombatReach())
               << " directive_target_edge_over_range=" << (bot->GetDistance(directiveTarget) - classContext.movementFollowRange)
               << " directive_target_los=" << (bot->IsWithinLOSInMap(directiveTarget) ? "yes" : "no")
               << " directive_target_attackable=" << (bot->IsValidAttackTarget(directiveTarget) ? "yes" : "no");
    }
    else
    {
        status << " directive_target=none";
    }

    if (Unit* selected = bot->GetSelectedUnit())
    {
        constexpr float kHalfCircleArc = 3.14159265358979323846f;
        status << " selected=" << selected->GetName()
               << " selected_dist=" << bot->GetDistance(selected)
               << " in_front=" << (bot->HasInArc(kHalfCircleArc, selected) ? "yes" : "no");
    }
    else
    {
        status << " selected=none";
    }

    return status.str();
}

std::string BuildManagedBotScmQueueDiagnosticLine(Player* bot)
{
    if (!bot)
        return "PB SCM diag unavailable.";

    constexpr BattlegroundTypeId kScmTypeId = BATTLEGROUND_SCM;
    constexpr uint32 kDeserterSpellId = 26013;
    BattlegroundQueueTypeId const scmQueueTypeId = BattlegroundMgr::BGQueueTypeId(kScmTypeId, 0);
    bool const queuedForScm = scmQueueTypeId != BATTLEGROUND_QUEUE_NONE &&
        bot->GetBattlegroundQueueIndex(scmQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES;
    bool const invitedForScm = scmQueueTypeId != BATTLEGROUND_QUEUE_NONE && bot->IsInvitedForBattlegroundQueueType(scmQueueTypeId);
    bool const hasScmAccessByLevel = bot->GetBGAccessByLevel(kScmTypeId);
    bool const hasAnyFreeQueueSlot = bot->HasFreeBattlegroundQueueId();

    uint8 usedQueueSlots = 0;
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        if (bot->GetBattlegroundQueueTypeId(i) != BATTLEGROUND_QUEUE_NONE)
            ++usedQueueSlots;

    bool hasQueueRecord = false;
    GroupQueueInfo ginfo{};
    if (queuedForScm)
    {
        BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(scmQueueTypeId);
        hasQueueRecord = queue.GetPlayerGroupInfoData(bot->GetGUID(), &ginfo);
    }

    std::string reason = "unknown";
    if (bot->InBattleground())
        reason = "already-in-battleground";
    else if (!bot->IsAlive())
        reason = "dead-outside-bg";
    else if (!hasScmAccessByLevel)
        reason = "no-scm-level-access";
    else if (bot->HasAura(kDeserterSpellId))
        reason = "deserter-aura";
    else if (queuedForScm && invitedForScm)
        reason = "invite-pending-accept";
    else if (queuedForScm && !hasQueueRecord)
        reason = "stale-local-scm-queue-slot";
    else if (queuedForScm)
        reason = "queued-waiting";
    else if (!hasAnyFreeQueueSlot)
        reason = "no-free-queue-slot";
    else if (!bot->IsInWorld())
        reason = "not-in-world";
    else if (bot->IsBeingTeleported())
        reason = "teleporting";
    else
        reason = "eligible-not-queued";

    std::ostringstream diag;
    diag << "PB SCM diag: "
         << "bot=" << bot->GetName()
         << " in_world=" << (bot->IsInWorld() ? "yes" : "no")
         << " in_bg=" << (bot->InBattleground() ? "yes" : "no")
         << " bg_type=" << uint32(bot->GetBattlegroundTypeId())
         << " bg_status=" << (bot->GetBattleground() ? uint32(bot->GetBattleground()->GetStatus()) : 255)
         << " scm_access=" << (hasScmAccessByLevel ? "yes" : "no")
         << " alive=" << (bot->IsAlive() ? "yes" : "no")
         << " deserter=" << (bot->HasAura(kDeserterSpellId) ? "yes" : "no")
         << " queued_scm=" << (queuedForScm ? "yes" : "no")
         << " invited_scm=" << (invitedForScm ? "yes" : "no")
         << " queue_record=" << (hasQueueRecord ? "yes" : "no")
         << " queue_slots_used=" << uint32(usedQueueSlots) << "/" << uint32(PLAYER_MAX_BATTLEGROUND_QUEUES)
         << " free_queue_slot=" << (hasAnyFreeQueueSlot ? "yes" : "no")
         << " teleporting=" << (bot->IsBeingTeleported() ? "yes" : "no")
         << " reason=" << reason;

    return diag.str();
}

BattlegroundTypeId ResolveCurrentBgTypeFromPlayerContext(Player const* player)
{
    if (!player)
        return BATTLEGROUND_TYPE_NONE;

    if (player->InBattleground())
        return player->GetBattlegroundTypeId();

    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId const queueTypeId = player->GetBattlegroundQueueTypeId(i);
        if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
            continue;

        if (BattlegroundMgr::BGArenaType(queueTypeId) != 0)
            continue;

        BattlegroundTypeId const queueBgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        if (queueBgTypeId != BATTLEGROUND_TYPE_NONE)
            return queueBgTypeId;
    }

    return BATTLEGROUND_TYPE_NONE;
}

// ---------------------------------------------------------------------------
// Match diagnostics: the bots seated in the battleground or arena the GM is in.
// A clone carries a generated internal name and its display name lives only in
// the character cache, so everything here matches and prints by both.
// ---------------------------------------------------------------------------

std::string BotDisplayName(Player const* bot)
{
    std::string displayName;
    if (!bot)
        return displayName;
    if (!sCharacterCache->GetCharacterNameByGuid(bot->GetGUID(), displayName) || displayName.empty())
        displayName = bot->GetName();
    return displayName;
}

std::string UnitDisplayName(Unit const* unit)
{
    if (!unit)
        return "none";
    if (Player const* player = unit->ToPlayer())
        return BotDisplayName(player);
    return unit->GetName();
}

bool IsMatchBot(Player const* player)
{
    return playerbot::PlayerbotObcCloneManager::IsActiveClone(player) || playerbot::IsManagedRandomBot(player);
}

char const* TeamTag(uint32 team)
{
    return team == ALLIANCE ? "A" : (team == HORDE ? "H" : "?");
}

std::string SpellLabel(uint32 spellId)
{
    if (!spellId)
        return "none";
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    std::ostringstream label;
    label << (spellInfo && spellInfo->SpellName[0] ? spellInfo->SpellName[0] : "?") << '(' << spellId << ')';
    return label.str();
}

// The spell being cast or channelled right now, or 0.
uint32 CurrentCastSpellId(Unit const* unit)
{
    for (uint32 type : { CURRENT_CHANNELED_SPELL, CURRENT_GENERIC_SPELL })
        if (Spell const* spell = unit->GetCurrentSpell(type))
            if (spell->GetSpellInfo())
                return spell->GetSpellInfo()->Id;
    return 0;
}

std::string LossOfControlSummary(Unit const* unit)
{
    std::string summary;
    auto add = [&summary](bool condition, char const* label)
    {
        if (!condition)
            return;
        if (!summary.empty())
            summary += ',';
        summary += label;
    };
    add(unit->HasUnitState(UNIT_STATE_STUNNED), "stunned");
    add(unit->HasUnitState(UNIT_STATE_ROOT), "rooted");
    add(unit->HasUnitState(UNIT_STATE_FLEEING), "fleeing");
    add(unit->HasUnitState(UNIT_STATE_CONFUSED), "confused");
    add(unit->HasUnitFlag(UNIT_FLAG_SILENCED), "silenced");
    add(unit->HasUnitFlag(UNIT_FLAG_PACIFIED), "pacified");
    add(unit->HasAuraType(SPELL_AURA_MOD_DISARM), "disarmed");
    add(unit->HasStealthAura(), "stealthed");
    add(unit->IsMounted(), "mounted");
    return summary.empty() ? "free" : summary;
}

Battleground* GetObserverMatch(Player* observer)
{
    Map* map = observer ? observer->FindMap() : nullptr;
    if (!map || !map->IsBattlegroundOrArena())
        return nullptr;
    BattlegroundMap* battlegroundMap = map->ToBattlegroundMap();
    return battlegroundMap ? battlegroundMap->GetBG() : nullptr;
}

std::vector<Player*> CollectMatchBots(Player* observer)
{
    std::vector<Player*> bots;
    Map* map = observer ? observer->FindMap() : nullptr;
    if (!map)
        return bots;

    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (candidate && candidate != observer && candidate->IsInWorld() && IsMatchBot(candidate))
            bots.push_back(candidate);
    }

    Battleground const* battleground = GetObserverMatch(observer);
    std::sort(bots.begin(), bots.end(), [battleground](Player const* left, Player const* right)
    {
        uint32 const leftTeam = battleground ? battleground->GetPlayerTeam(left->GetGUID()) : 0;
        uint32 const rightTeam = battleground ? battleground->GetPlayerTeam(right->GetGUID()) : 0;
        if (leftTeam != rightTeam)
            return leftTeam < rightTeam;
        return BotDisplayName(left) < BotDisplayName(right);
    });
    return bots;
}

std::string ToLowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

std::string FormatHistoryEntry(playerbot::PvpClassActions::ExecutionHistoryEntry const& entry, uint32 nowMs)
{
    std::ostringstream line;
    line.setf(std::ios::fixed);
    line.precision(1);
    line << '-' << (float(nowMs - entry.gameTimeMs) / 1000.0f) << "s " << entry.status;
    if (entry.repeats > 1)
        line << " [x" << entry.repeats << ']';
    if (!entry.targetName.empty())
        line << " @" << entry.targetName;
    return line.str();
}

void ReportMatchBot(ChatHandler* handler, Player* observer, Battleground* battleground, Player* bot)
{
    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32 const team = battleground->GetPlayerTeam(bot->GetGUID());

    std::string sourceName = "-";
    for (playerbot::PlayerbotObcCloneManager::CustomGameCloneInfo const& clone :
        playerbot::PlayerbotObcCloneManager::GetCustomGameClones(battleground->GetInstanceID()))
    {
        if (clone.cloneGuid != bot->GetGUID())
            continue;
        if (!sCharacterCache->GetCharacterNameByGuid(clone.sourceGuid, sourceName))
            sourceName = clone.sourceGuid.ToString();
        break;
    }

    handler->PSendSysMessage("== %s (%s) L%u %s, team %s%s, source %s, %s",
        BotDisplayName(bot).c_str(), bot->GetName().c_str(), uint32(bot->GetLevel()),
        GetClassName(bot->GetClass(), handler->GetSessionDbcLocale()),
        TeamTag(team), team && team == battleground->GetPlayerTeam(observer->GetGUID()) ? " (yours)" : "",
        sourceName.c_str(),
        playerbot::PlayerbotObcCloneManager::IsActiveClone(bot) ? "transient clone" : "managed bot");

    handler->PSendSysMessage("vitals: %s hp %u/%u (%.0f%%) power %u/%u (%.0f%%) form=%u dist=%.1f pos=(%.1f,%.1f,%.1f) combat=%s",
        bot->IsAlive() ? "alive" : "DEAD",
        bot->GetHealth(), bot->GetMaxHealth(), bot->GetHealthPct(),
        bot->GetPower(bot->GetPowerType()), bot->GetMaxPower(bot->GetPowerType()), bot->GetPowerPct(bot->GetPowerType()),
        uint32(bot->GetShapeshiftForm()), observer->GetDistance(bot),
        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
        bot->IsInCombat() ? "yes" : "no");

    Unit const* selected = bot->GetTarget().IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*bot, bot->GetTarget());
    Unit const* victim = bot->GetVictim();
    MotionMaster const* motionMaster = bot->GetMotionMaster();
    handler->PSendSysMessage("control: %s casting=%s motion=%s moving=%s",
        LossOfControlSummary(bot).c_str(), SpellLabel(CurrentCastSpellId(bot)).c_str(),
        ToString(motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE),
        bot->isMoving() ? "yes" : "no");

    auto describeTarget = [bot](Unit const* unit) -> std::string
    {
        if (!unit)
            return "none";
        std::ostringstream text;
        text.setf(std::ios::fixed);
        text.precision(1);
        text << UnitDisplayName(unit) << " d=" << bot->GetDistance(unit)
             << " hp=" << unit->GetHealthPct() << "% los=" << (bot->IsWithinLOSInMap(unit) ? "yes" : "no")
             << " front=" << (bot->HasInArc(float(M_PI), unit) ? "yes" : "no")
             << " cc=" << LossOfControlSummary(unit);
        return text.str();
    };
    handler->PSendSysMessage("selected: %s", describeTarget(selected).c_str());
    if (victim != selected)
        handler->PSendSysMessage("melee victim: %s", describeTarget(victim).c_str());

    // Harmful auras in full; helpful ones only by name, they are mostly
    // passives and buffs.
    std::string harmful;
    uint32 helpfulCount = 0;
    for (auto const& [spellId, application] : bot->GetAppliedAuras())
    {
        if (application->IsPositive())
        {
            ++helpfulCount;
            continue;
        }
        Aura const* aura = application->GetBase();
        Unit const* caster = aura->GetCaster();
        std::ostringstream entry;
        entry << SpellLabel(spellId);
        if (aura->GetStackAmount() > 1)
            entry << 'x' << uint32(aura->GetStackAmount());
        if (!aura->IsPermanent())
            entry << ' ' << (aura->GetDuration() / IN_MILLISECONDS) << 's';
        entry << " by " << UnitDisplayName(caster);
        if (!harmful.empty())
            harmful += "; ";
        harmful += entry.str();
    }
    handler->PSendSysMessage("debuffs: %s (+%u helpful auras)", harmful.empty() ? "none" : harmful.c_str(), helpfulCount);

    // What the selector would choose right now. Same call the whisper
    // diagnostic makes; it does not cast.
    playerbot::PvpValues const values = playerbot::PvpCore::CollectValues(bot);
    playerbot::PvpClassSpellContext const context = playerbot::PvpCore::BuildClassSpellContext(bot, values);
    Unit const* decisionTarget = context.targetGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*bot, context.targetGuid);
    Unit const* moveTarget = context.movementTargetGuid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*bot, context.movementTargetGuid);
    handler->PSendSysMessage("now: gate=%s exec=%s action=%s spell=%s reason=%s target(%s)=%s move=%s->%s range=%.1f",
        context.classSpellsEnabled ? "on" : "off", context.shouldExecute ? "yes" : "no",
        context.actionName ? context.actionName : "none", SpellLabel(context.spellId).c_str(),
        context.reason ? context.reason : "none", ToString(context.targetMode), UnitDisplayName(decisionTarget).c_str(),
        ToString(context.movementDirective), UnitDisplayName(moveTarget).c_str(), context.movementFollowRange);

    std::string const moveDiag = playerbot::PvpClassActions::GetLastMovementDebugStatus(bot);
    handler->PSendSysMessage("move: %s", moveDiag.c_str());
    std::string const execDiag = playerbot::PvpClassActions::GetLastExecutionStatus(bot);
    handler->PSendSysMessage("exec: %s", execDiag.c_str());
    if (std::string const emfhDiag = playerbot::PvpCore::GetLastEveryManForHimselfDiagnostic(bot); !emfhDiag.empty())
        handler->PSendSysMessage("emfh: %s", emfhDiag.c_str());

    std::vector<playerbot::PvpClassActions::ExecutionHistoryEntry> const history = playerbot::PvpClassActions::GetExecutionHistory(bot);
    constexpr size_t kHistoryShown = 12;
    size_t const first = history.size() > kHistoryShown ? history.size() - kHistoryShown : 0;
    handler->PSendSysMessage("history (last %u of %u, newest last):", uint32(history.size() - first), uint32(history.size()));
    for (size_t index = first; index < history.size(); ++index)
        handler->PSendSysMessage("  %s", FormatHistoryEntry(history[index], nowMs).c_str());
}

// One line per bot: side (* = yours), class, vitals, loss of control, cast,
// target, distance and the last thing it decided.
void ReportMatchRoster(ChatHandler* handler, Player* observer, Battleground* battleground, std::vector<Player*> const& bots)
{
    uint32 const myTeam = battleground->GetPlayerTeam(observer->GetGUID());
    handler->PSendSysMessage("[MatchBots] %s (instance %u, map %u): %u bots. '.gm diagnostics on matchbots <name>' follows one.",
        battleground->GetName().c_str(), battleground->GetInstanceID(), observer->GetMapId(), uint32(bots.size()));

    for (Player* bot : bots)
    {
        uint32 const team = battleground->GetPlayerTeam(bot->GetGUID());
        Unit const* target = bot->GetTarget().IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*bot, bot->GetTarget());
        std::string exec = playerbot::PvpClassActions::GetLastExecutionStatus(bot);
        if (exec.size() > 70)
            exec = exec.substr(0, 70) + "...";

        handler->PSendSysMessage("[%s%s] %s %s %s hp%.0f%% pw%.0f%% %s%s cast=%s tgt=%s d=%.0f | %s",
            TeamTag(team), myTeam && team == myTeam ? "*" : "",
            BotDisplayName(bot).c_str(),
            GetClassName(bot->GetClass(), handler->GetSessionDbcLocale()),
            bot->IsAlive() ? "" : "DEAD",
            bot->GetHealthPct(), bot->GetPowerPct(bot->GetPowerType()),
            LossOfControlSummary(bot).c_str(),
            playerbot::PlayerbotObcCloneManager::IsActiveClone(bot) ? "" : " (managed)",
            SpellLabel(CurrentCastSpellId(bot)).c_str(),
            UnitDisplayName(target).c_str(),
            observer->GetDistance(bot),
            exec.c_str());
    }
}

// Display or internal name starts with the filter; an empty filter takes all.
bool BotMatchesFilter(Player const* bot, std::string const& lowerFilter)
{
    if (lowerFilter.empty())
        return true;
    std::string const displayName = ToLowerCopy(BotDisplayName(bot));
    std::string const internalName = ToLowerCopy(bot->GetName());
    return displayName.compare(0, lowerFilter.size(), lowerFilter) == 0 ||
        internalName.compare(0, lowerFilter.size(), lowerFilter) == 0;
}

// ".gm diagnostics on matchbots [name]". The command itself lives in the core
// scripts, which build without this module, so it only sets a session bit and
// a name filter; this side notices them from the GM's own player update. On
// switching on, entering a match or changing the name it prints a snapshot
// (the full dump when the name picks out one bot, the roster otherwise), then
// streams every new decision the followed bots make.
struct MatchBotDiagnosticState
{
    bool announced = false;
    uint32 instanceId = 0;
    std::string filter;
    uint32 lastPollMs = 0;
    std::unordered_map<ObjectGuid, uint64> lastSequenceByBot;
};

// Keyed by GM; each GM is polled on its own map thread, hence the lock. Held
// for the whole poll - only GMs with the category on ever take it, and nothing
// that runs under it takes this lock back.
std::unordered_map<ObjectGuid, MatchBotDiagnosticState> g_MatchBotDiagnostics;
std::mutex g_MatchBotDiagnosticsLock;
std::atomic<bool> g_AnyMatchBotDiagnostics{ false };

void ForgetMatchBotDiagnostics(ObjectGuid observerGuid)
{
    std::lock_guard<std::mutex> lock(g_MatchBotDiagnosticsLock);
    g_MatchBotDiagnostics.erase(observerGuid);
    g_AnyMatchBotDiagnostics.store(!g_MatchBotDiagnostics.empty(), std::memory_order_relaxed);
}

void PollMatchBotDiagnostics(Player* observer, WorldSession* session)
{
    constexpr uint32 kPollIntervalMs = 250;
    constexpr size_t kMaxLinesPerPoll = 8;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    std::lock_guard<std::mutex> lock(g_MatchBotDiagnosticsLock);
    MatchBotDiagnosticState& state = g_MatchBotDiagnostics[observer->GetGUID()];
    g_AnyMatchBotDiagnostics.store(true, std::memory_order_relaxed);
    if (state.announced && nowMs - state.lastPollMs < kPollIntervalMs)
        return;
    state.lastPollMs = nowMs;

    ChatHandler handler(session);
    Battleground* battleground = GetObserverMatch(observer);
    uint32 const instanceId = battleground ? battleground->GetInstanceID() : 0;
    std::string const filter = ToLowerCopy(session->GetGmDiagnosticBotFilter());

    if (!state.announced || state.instanceId != instanceId || state.filter != filter)
    {
        state.announced = true;
        state.instanceId = instanceId;
        state.filter = filter;
        state.lastSequenceByBot.clear();

        if (!battleground)
        {
            handler.PSendSysMessage("[MatchBots] on%s%s - starts when you are in a battleground or arena.",
                filter.empty() ? "" : " for ", filter.c_str());
            return;
        }

        std::vector<Player*> bots = CollectMatchBots(observer);
        bots.erase(std::remove_if(bots.begin(), bots.end(), [&filter](Player const* bot)
        {
            return !BotMatchesFilter(bot, filter);
        }), bots.end());

        if (!filter.empty() && bots.size() == 1)
            ReportMatchBot(&handler, observer, battleground, bots.front());
        else if (!filter.empty() && bots.empty())
            handler.PSendSysMessage("[MatchBots] no bot named '%s' in your match yet; it is followed if one joins.", filter.c_str());
        else
            ReportMatchRoster(&handler, observer, battleground, bots);

        // The snapshot already carries the history; stream only what follows.
        for (Player* bot : bots)
        {
            std::vector<playerbot::PvpClassActions::ExecutionHistoryEntry> const history = playerbot::PvpClassActions::GetExecutionHistory(bot);
            state.lastSequenceByBot[bot->GetGUID()] = history.empty() ? 0 : history.back().sequence;
        }
        return;
    }

    if (!battleground)
        return;

    size_t sent = 0;
    size_t skipped = 0;
    for (Player* bot : CollectMatchBots(observer))
    {
        if (!BotMatchesFilter(bot, filter))
            continue;

        uint64& lastSequence = state.lastSequenceByBot[bot->GetGUID()];
        std::vector<playerbot::PvpClassActions::ExecutionHistoryEntry> const entries =
            playerbot::PvpClassActions::GetExecutionHistory(bot, lastSequence);
        if (entries.empty())
            continue;
        lastSequence = entries.back().sequence;

        std::string const name = BotDisplayName(bot);
        std::string const control = LossOfControlSummary(bot);
        for (playerbot::PvpClassActions::ExecutionHistoryEntry const& entry : entries)
        {
            if (sent >= kMaxLinesPerPoll)
            {
                ++skipped;
                continue;
            }
            handler.PSendSysMessage("[MatchBots] %s %.0f%%hp %s: %s", name.c_str(), bot->GetHealthPct(),
                control.c_str(), FormatHistoryEntry(entry, nowMs).c_str());
            ++sent;
        }
    }

    if (skipped)
        handler.PSendSysMessage("[MatchBots] (%u more decisions skipped; name one bot to follow it alone)", uint32(skipped));
}

class PlayerbotBootstrapWorldScript final : public WorldScript
{
public:
    PlayerbotBootstrapWorldScript() : WorldScript("PlayerbotBootstrapWorldScript") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        playerbot::PvpCore::LoadConfig();
        playerbot::ObjectiveCoordinator::LoadConfig();
        playerbot::RandomBotParticipationManager::ResetCadence();
        playerbot::RandomBotParticipationManager::LoadPopulationConfig();
        playerbot::PlayerbotObcCloneManager::LoadConfig();
        playerbot::ResourceGovernor::LoadConfig();
        playerbot::PlayerbotBgFillDriver::LoadConfig();
        playerbot::PveManager::LoadConfig();
        LoadPlayerbotGoldGainMultiplier();
    }

    void OnStartup() override
    {
        playerbot::PvpCore::LoadConfig();
        playerbot::ObjectiveCoordinator::LoadConfig();
        playerbot::RandomBotParticipationManager::ResetCadence();
        playerbot::RandomBotParticipationManager::LoadPopulationConfig();
        playerbot::RandomBotParticipationManager::OnStartupBootstrap();
        playerbot::PlayerbotObcCloneManager::LoadConfig();
        playerbot::PlayerbotObcCloneManager::OnStartupSweep();
        playerbot::ResourceGovernor::LoadConfig();
        playerbot::PlayerbotBgFillDriver::LoadConfig();
        playerbot::PveManager::LoadConfig();
        LoadPlayerbotGoldGainMultiplier();
        // after PveManager: the ladder's teams come from its PvP-only accounts
        playerbot::PlayerbotBgFillDriver::OnStartup();
        playerbot::PvpCoreConfig const& config = playerbot::PvpCore::GetConfig();
        playerbot::RandomBotPopulationSnapshot const population = playerbot::RandomBotParticipationManager::GetPopulationSnapshot();

        TC_LOG_INFO("server.loading", "Playerbot bootstrap loaded (enabled: {}, pvp core: {}, pvp tactics: {}, pvp lifecycle: {}, pvp class spells: {}).",
            config.moduleEnabled ? "true" : "false", config.pvpCoreEnabled ? "true" : "false",
            config.pvpTacticsEnabled ? "true" : "false", config.pvpLifecycleEnabled ? "true" : "false",
            config.pvpClassSpellsEnabled ? "true" : "false");

        TC_LOG_INFO("server.loading", "Playerbot random population bootstrap (configEnabled: {}, runtimeEnabled: {}, target=[{}, {}], onlineBots: {}, loginOrchestration: {}).",
            population.configEnabled ? "true" : "false", population.runtimeEnabled ? "true" : "false",
            population.targetMin, population.targetMax, population.onlineRandomBots,
            population.supportsLoginOrchestration ? "true" : "false");
    }

    void OnUpdate(uint32 diff) override
    {
        playerbot::ResourceGovernor::NoteWorldUpdate(diff);
        playerbot::RandomBotParticipationManager::OnWorldUpdate(diff);
        playerbot::PlayerbotObcCloneManager::OnWorldUpdate(diff);
        playerbot::PlayerbotVhrWaveDriver::OnWorldUpdate(diff);
        playerbot::PlayerbotBgFillDriver::OnWorldUpdate(diff);
        playerbot::PveManager::OnWorldUpdate(diff);
    }

    void OnShutdown() override
    {
        playerbot::PlayerbotObcCloneManager::OnShutdown();
    }
};

class PlayerbotDamageUnitScript final : public UnitScript
{
public:
    PlayerbotDamageUnitScript() : UnitScript("PlayerbotDamageUnitScript") { }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* /*attacker*/, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!spellInfo || spellInfo->Id != PLAYERBOT_REDUCED_MAGMA_DAMAGE_SPELL_ID)
            return;

        Player* player = target ? target->ToPlayer() : nullptr;
        if (!playerbot::IsManagedRandomBot(player) && !playerbot::PlayerbotObcCloneManager::IsActiveClone(player))
            return;

        damage = 0;
    }
};

class PlayerbotLifecyclePlayerScript final : public PlayerScript
{
public:
    PlayerbotLifecyclePlayerScript() : PlayerScript("PlayerbotLifecyclePlayerScript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        playerbot::RandomBotParticipationManager::NotifyHumanPopulationChanged(player);
    }

    void OnUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        playerbot::RandomBotParticipationManager::NotifyHumanPopulationChanged(player);
    }

    // Every open-world death of a world bot buys it a step of PvE power
    // (Miscellaneous/BotPvePower.h). Battlegrounds, arenas and duels are fights it
    // chose, and PvP-only bots never grind wildlife, so none of those count.
    void OnPlayerJustDied(Player* victim, Unit* /*killer*/) override
    {
        if (!BotPvePower::IsEnabled() || !playerbot::IsManagedRandomBot(victim) ||
            playerbot::PveManager::IsPvpOnlyBot(victim) || victim->duel)
            return;

        if (Map const* map = victim->FindMap(); !map || map->IsBattlegroundOrArena())
            return;

        BotPvePower::RecordDeath(victim);
    }

    void OnUpdate(Player* player, uint32 diff) override
    {
        RecordManagedBotUpdatePulse(player, diff);
        // Only sessions above player rank can hold the category, so bots and
        // ordinary players never touch the diagnostic table.
        if (WorldSession* session = player->GetSession(); session && session->GetSecurity() > SEC_PLAYER)
        {
            if (session->IsGmDiagnosticEnabled(GmDiagnosticCategory::MatchBots))
                PollMatchBotDiagnostics(player, session);
            else if (g_AnyMatchBotDiagnostics.load(std::memory_order_relaxed))
                ForgetMatchBotDiagnostics(player->GetGUID());
        }
        playerbot::RandomBotParticipationManager::ProcessPlayerLifecycle(player);
    }

    void OnSpellCast(Player* player, Spell* spell, bool) override
    {
        if (!player || !spell || !spell->IsTriggered())
            return;

        SpellInfo const* spellInfo = spell->GetSpellInfo();
        if (!spellInfo || spellInfo->Id != 75)
            return;

        if (!playerbot::IsManagedRandomBot(player) && !playerbot::PlayerbotObcCloneManager::IsActiveClone(player))
            return;

        playerbot::NotifyHunterAutoShotFired(player);
    }

    void OnMoneyChanged(Player* player, int32& amount) override
    {
        // Gains only. Scaling the loss path too would make every bot pay
        // double at the vendor and the auction house, which is worse than
        // not scaling at all.
        if (!player || amount <= 0)
            return;

        if (!playerbot::IsManagedRandomBot(player))
            return;

        // Killing things only. A loot window is open exactly while money is
        // being taken off a corpse and at no other time, so this admits mob
        // loot and excludes vendor sales, auction settlements, mail and quest
        // rewards - the same test the hardcore reward multiplier uses.
        //
        // It matters because a multiplier on SELLING mints gold rather than
        // paying it out: a bot vendoring a green for 100c would receive 200c,
        // and a bot-to-bot auction would pay the buyer's X out while crediting
        // the seller 2X, so every trade inside the fleet created gold from
        // nothing. Loot is world income; sales are not.
        if (player->GetLootGUID().IsEmpty())
            return;

        // Band is chosen by the bot's CURRENT level, so a bot crossing out of
        // the low band simply starts earning at the other rate.
        float const multiplier = playerbot::PlayerbotGoldGainMultiplierFor(player);
        if (multiplier <= 1.0f)
            return;

        // Saturate rather than wrap: a large auction settlement scaled up
        // must not overflow into a negative "gain" and take the bot's purse
        // with it.
        double const scaled = double(amount) * double(multiplier);
        amount = scaled >= double(std::numeric_limits<int32>::max())
            ? std::numeric_limits<int32>::max()
            : int32(scaled);
    }

    void OnLogout(Player* player) override
    {
        playerbot::RandomBotParticipationManager::NotifyHumanPopulationChanged(player);
        if (player && g_AnyMatchBotDiagnostics.load(std::memory_order_relaxed))
            ForgetMatchBotDiagnostics(player->GetGUID());
        playerbot::PveManager::OnBotLogout(player);
        playerbot::RandomBotParticipationManager::OnPlayerLogout(player);
        playerbot::PlayerbotObcCloneManager::OnPlayerLogout(player);
    }

    void OnLevelChanged(Player* player, uint8 oldLevel) override
    {
        playerbot::PveManager::OnManagedBotLevelChanged(player, oldLevel);
    }

    void OnPVPKill(Player* killer, Player* killed) override
    {
        playerbot::PlayerbotObcCloneManager::OnPvpKill(killer, killed);
    }

    void OnDuelRequest(Player* target, Player* challenger) override
    {
        if (!target || !challenger)
            return;

        if (!playerbot::IsManagedRandomBot(target))
            return;

        // Bots ignore tournament characters: the challenge is left unanswered.
        if (Tournament::IsTournamentCharacter(challenger))
            return;

        if (!target->duel || !challenger->duel || target->duel->State != DUEL_STATE_CHALLENGED)
            return;

        if (target->duel->Opponent != challenger || challenger->duel->Opponent != target)
            return;

        // A bot takes on anybody orange to it or below, and turns down anybody
        // red - five or more levels above it - the way a person would. Refused
        // outright rather than left hanging, so the challenger is not stuck
        // with a request nobody will ever answer. Last line of Spell::EffectDuel,
        // so tearing the duel down from here is safe.
        if (Trinity::XP::GetColorCode(target->GetLevel(), challenger->GetLevel()) == XP_RED)
        {
            target->DuelComplete(DUEL_INTERRUPTED);
            if (WorldSession* session = challenger->GetSession())
                ChatHandler(session).PSendSysMessage("%s looks you over and declines. You are far too strong for them.",
                    target->GetName().c_str());
            return;
        }

        time_t const now = GameTime::GetGameTime();
        target->duel->StartTime = now + 3;
        challenger->duel->StartTime = now + 3;

        target->duel->State = DUEL_STATE_COUNTDOWN;
        challenger->duel->State = DUEL_STATE_COUNTDOWN;

        target->SendDuelCountdown(3000);
        challenger->SendDuelCountdown(3000);
    }

    void OnChat(Player* sender, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (!sender || !receiver)
            return;

        if (type != CHAT_MSG_WHISPER)
            return;

        if (lang == LANG_ADDON)
            return;

        // The hard-coded Chromie whisper responder uses a virtual player so whispers
        // look like real player whispers. Do not treat her as a playerbot or reply
        // with diagnostics, otherwise bot-to-bot whisper hooks can recurse indefinitely.
        if (IsChromieWhisperFacade(sender) || IsChromieWhisperFacade(receiver))
            return;

        bool const senderIsPlayerbot = playerbot::IsManagedRandomBot(sender) ||
            playerbot::PlayerbotObcCloneManager::IsActiveClone(sender);
        bool const receiverIsPlayerbot = playerbot::IsManagedRandomBot(receiver) ||
            playerbot::PlayerbotObcCloneManager::IsActiveClone(receiver);

        // Diagnostic commands are human/GM -> bot only. A reply emitted by one
        // bot must never be interpreted as a fresh command by another bot, or
        // Whisper -> OnPlayerChat -> Whisper recursively re-enters forever.
        if (!receiverIsPlayerbot || senderIsPlayerbot || sender == receiver)
            return;

        // Bots ignore tournament characters - out in the world. Inside a match
        // the two modes are seated together on purpose: tournament characters
        // queue in their own pool and the fill deals bots into it, so a bot
        // that answers nothing at all to the side it is playing for reads as
        // broken. Tournament::AreKeptFromFighting draws the same line, exempting
        // battleground and arena maps from the separation.
        if (Tournament::IsTournamentCharacter(sender) && !sender->IsGameMaster() &&
            !SharesMatch(sender, receiver))
            return;

        std::string command = msg;
        command.erase(command.begin(), std::find_if(command.begin(), command.end(), [](unsigned char character)
        {
            return !std::isspace(character);
        }));
        command.erase(std::find_if(command.rbegin(), command.rend(), [](unsigned char character)
        {
            return !std::isspace(character);
        }).base(), command.end());
        std::transform(command.begin(), command.end(), command.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

        // A battleground order, and it comes before the companion orders below:
        // "drop" is given inside a match by whoever is playing it, while the PvE
        // companion handler answers anybody who is not that bot's own master
        // with a taunt and swallows the line.
        if (command == "drop")
        {
            // An order to a teammate. Now that a clone can be whispered by the
            // name players see, anyone could otherwise give it - including an
            // opponent telling the bot carrying their own flag to let go of it.
            // Both must be seated in the same match on the same side; a GM in
            // GM mode may order any bot in a match.
            //
            // Every refusal answers. A silent one is indistinguishable from a
            // bot that ignored the order, and the whisper can reach a copy's
            // source character out in the world instead of the copy playing the
            // match - which is exactly what the first reply below names.
            Battleground* battleground = receiver->GetBattleground();
            if (!battleground)
            {
                receiver->Whisper("I am not in a battleground.", LANG_UNIVERSAL, sender);
                return;
            }

            uint32 const botTeam = battleground->GetPlayerTeam(receiver->GetGUID());
            bool const fromTeammate = botTeam && battleground->GetPlayerTeam(sender->GetGUID()) == botTeam;
            if (!fromTeammate && !sender->IsGameMaster())
            {
                receiver->Whisper("Give that order to your own side.", LANG_UNIVERSAL, sender);
                return;
            }

            if (!playerbot::PvpCore::IsBattlegroundFlagCarrier(receiver))
            {
                receiver->Whisper("I am not carrying a flag.", LANG_UNIVERSAL, sender);
                return;
            }

            battleground->EventPlayerDroppedFlag(receiver);
            playerbot::BattlegroundTacticalActions::DelayFlagPickup(receiver, 5 * IN_MILLISECONDS);

            receiver->Whisper("Flag dropped.", LANG_UNIVERSAL, sender);
            return;
        }

        // PvE companion orders ("follow", "stay", "attack", "passive", "come",
        // "dismiss") take precedence over the diagnostic dump below.
        if (playerbot::PveManager::HandleWhisperCommand(sender, receiver, command))
            return;

        // Everything below dumps the bot's internals - unit-state flags, spline
        // indices, motion targets, queue slots and the engage verdict. That is a
        // map of how the bot decides, so it belongs to whoever is debugging it,
        // not to whoever whispered it.
        //
        // It used to be gated on IsGameMaster. b5621f4ea4 ("always whisper
        // diagnostics") deleted that line for a debugging session, and d4f071ed40
        // put the gate back on the PvE manager's path only - this fall-through
        // never got it back. With Playerbot.Pve.Enable off, which is the default
        // and what Legionnaire Plus runs, HandleWhisperCommand returns on its
        // first line and every player who whispered any bot anything landed here.
        //
        // Gate on the SENDER's session: bot sessions carry the bot account's own
        // SEC_PLAYER, so asking the receiver would switch this off permanently.
        WorldSession const* senderSession = sender->GetSession();
        if (!senderSession || !senderSession->IsGmDiagnosticEnabled(GmDiagnosticCategory::Playerbot))
            return;

        receiver->Whisper(BuildManagedBotStatusLine(receiver), LANG_UNIVERSAL, sender);
        receiver->Whisper(std::string("PB move diag: ") + playerbot::PvpClassActions::GetLastMovementDebugStatus(receiver), LANG_UNIVERSAL, sender);
        receiver->Whisper(BuildManagedBotUpdateDiagnosticLine(receiver), LANG_UNIVERSAL, sender);

        receiver->Whisper(BuildManagedBotScmQueueDiagnosticLine(receiver), LANG_UNIVERSAL, sender);
    }
};

class PlayerbotLifecycleCommandScript final : public CommandScript
{
public:
    PlayerbotLifecycleCommandScript() : CommandScript("PlayerbotLifecycleCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable playerbotPvpLifecycleTable =
        {
            { "snapshot", HandlePlayerbotPvpLifecycleSnapshotCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };

        static ChatCommandTable playerbotPvpTable =
        {
            { "lifecycle", playerbotPvpLifecycleTable },
            { "forcequeue", HandlePlayerbotPvpForceQueueCurrentCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "movediag", HandlePlayerbotPvpMoveDiagCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "ctf", HandlePlayerbotPvpCtfCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "nodes", HandlePlayerbotPvpNodesCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "duties", HandlePlayerbotPvpDutiesCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
        };

        static ChatCommandTable playerbotRandomPopulationTable =
        {
            { "status", HandlePlayerbotPopulationStatusCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "start", HandlePlayerbotPopulationStartCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "stop", HandlePlayerbotPopulationStopCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "rebalance now", HandlePlayerbotPopulationRebalanceCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "list", HandlePlayerbotPopulationPoolCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };

        static ChatCommandTable playerbotPveTable =
        {
            { "summon", HandlePlayerbotPveSummonCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "dismiss", HandlePlayerbotPveDismissCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "status", HandlePlayerbotPveStatusCommand, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "reset", HandlePlayerbotPveResetCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "respec", HandlePlayerbotPveRespecCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "rehome", HandlePlayerbotPveRehomeCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "wipe", HandlePlayerbotPveWipeCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "clearauctions", HandlePlayerbotPveClearAuctionsCommand, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };

        static ChatCommandTable playerbotTable =
        {
            { "pvp", playerbotPvpTable },
            { "pve", playerbotPveTable },
            { "population", playerbotRandomPopulationTable },
        };

        static ChatCommandTable commandTable =
        {
            { "playerbot", playerbotTable },
        };

        return commandTable;
    }

    static bool HandlePlayerbotPvpLifecycleSnapshotCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        playerbot::LifecycleObservationSnapshot const snapshot = playerbot::RandomBotParticipationManager::GetLifecycleObservationSnapshot();

        handler->PSendSysMessage("Playerbot PvP lifecycle observation snapshot:");
        handler->PSendSysMessage(" - gateDisabled: " UI64FMTD, snapshot.gateDisabled);
        handler->PSendSysMessage(" - cadenceThrottled: " UI64FMTD, snapshot.cadenceThrottled);
        handler->PSendSysMessage(" - invalidPlayerState: " UI64FMTD, snapshot.invalidPlayerState);
        handler->PSendSysMessage(" - noLifecycleHooksActive: " UI64FMTD, snapshot.noLifecycleHooksActive);
        handler->PSendSysMessage(" - battlegroundLifecycleExecuted: " UI64FMTD, snapshot.battlegroundLifecycleExecuted);
        handler->PSendSysMessage(" - arenaLifecycleExecuted: " UI64FMTD, snapshot.arenaLifecycleExecuted);
        return true;
    }

    // Arathi Basin / Battle for Gilneas team play: what each base is worth to
    // each side, how many bodies it asked for and how many were sent. Select a
    // bot to see the base it was given.
    static bool HandlePlayerbotPvpNodesCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Player* selected = handler->getSelectedPlayer();
        Player* observer = selected && selected->GetBattleground() ? selected : player;
        for (std::string const& line : playerbot::NodeCoordinator::DescribeTeams(observer))
            handler->PSendSysMessage("%s", line.c_str());

        playerbot::NodeBotOrders orders;
        if (selected && selected != player && playerbot::NodeCoordinator::GetOrders(selected, orders))
        {
            if (orders.hasNode)
                handler->PSendSysMessage("%s: %s base %u, %s, enemies on it %u%s.",
                    selected->GetName().c_str(), playerbot::GetNodeRoleName(orders.role), orders.nodeId,
                    orders.interact ? "clicking the banner" : "holding it", orders.enemiesAtNode,
                    orders.leash ? " (leashed to it)" : "");
            else
                handler->PSendSysMessage("%s: %s, no base assigned.",
                    selected->GetName().c_str(), playerbot::GetNodeRoleName(orders.role));
        }

        return true;
    }

    // Gates, siege vehicles, commanders and the neutral flag of a base match:
    // which gate each side is breaking, who is on which duty. Select a bot to
    // see its own.
    static bool HandlePlayerbotPvpDutiesCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        // Only a bot on the GM's own map: asking for another map's orders would
        // rebuild its plan from this thread.
        Player* selected = handler->getSelectedPlayer();
        if (selected && selected->FindMap() != player->FindMap())
            selected = nullptr;
        Player* observer = selected && selected->GetBattleground() ? selected : player;
        for (std::string const& line : playerbot::ObjectiveCoordinator::DescribeTeams(observer))
            handler->PSendSysMessage("%s", line.c_str());

        if (selected && selected != player)
        {
            playerbot::ObjectiveBotOrders orders;
            if (playerbot::ObjectiveCoordinator::GetOrders(selected, orders))
                handler->PSendSysMessage("%s: %s, gate %u, vehicle %s, boss %s.", selected->GetName().c_str(),
                    playerbot::GetObjectiveDutyName(orders.duty), orders.siegeId,
                    orders.vehicleGuid.IsEmpty() ? "none" : orders.vehicleGuid.ToString().c_str(),
                    orders.bossGuid.IsEmpty() ? "none" : orders.bossGuid.ToString().c_str());
            else
                handler->PSendSysMessage("%s: no duty.", selected->GetName().c_str());
        }

        return true;
    }

    // Warsong Gulch / Twin Peaks team play: each side's flag runner, escorts,
    // defenders and any handoff under way. Select a bot to see its own orders.
    static bool HandlePlayerbotPvpCtfCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        Player* selected = handler->getSelectedPlayer();
        Player* observer = selected && selected->GetBattleground() ? selected : player;
        for (std::string const& line : playerbot::CtfCoordinator::DescribeTeams(observer))
            handler->PSendSysMessage("%s", line.c_str());

        playerbot::CtfBotOrders orders;
        if (selected && selected != player && playerbot::CtfCoordinator::GetOrders(selected, orders))
        {
            handler->PSendSysMessage("%s: %s%s, pickup %s%s%s, handoff %s",
                selected->GetName().c_str(), playerbot::GetCtfRoleName(orders.role),
                orders.isDesignatedRunner ? " (designated runner)" : "",
                orders.pickupGuid.IsEmpty() ? "none" : (orders.pickupIsReturn ? "our flag" : "their flag"),
                orders.pickupIsOpportunistic ? " (standing near it)" : "",
                orders.pickupNearby ? " (in reach)" : "",
                orders.handoffGive ? "giving" : (orders.handoffReceive ? "receiving" : "none"));
        }

        return true;
    }

    // Reports the last movement directive each nearby bot issued, plus the state
    // that drives it. Exists because the whisper diagnostic is unusable for
    // clones: they carry generated internal names and their display name lives
    // only in the character cache, so there is nothing to type. Reports on the
    // current selection when something is selected, otherwise sweeps the radius.
    static bool HandlePlayerbotPvpMoveDiagCommand(ChatHandler* handler, Optional<float> radiusArg)
    {
        if (!handler)
            return false;

        Player* player = handler->GetPlayer();
        if (!player || !player->FindMap())
            return false;

        float const radius = std::max(1.0f, radiusArg.value_or(80.0f));

        std::vector<Player*> bots;
        if (Player* selected = handler->getSelectedPlayer())
        {
            if (selected != player)
                bots.push_back(selected);
        }

        if (bots.empty())
        {
            Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
            for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            {
                Player* candidate = itr->GetSource();
                if (!candidate || candidate == player || !candidate->IsInWorld())
                    continue;
                if (!candidate->IsWithinDistInMap(player, radius))
                    continue;
                bots.push_back(candidate);
            }

            std::sort(bots.begin(), bots.end(), [player](Player const* left, Player const* right)
            {
                return player->GetDistance(left) < player->GetDistance(right);
            });
        }

        if (bots.empty())
        {
            handler->PSendSysMessage("No players found (nothing selected, none within %.0f yd).", radius);
            return true;
        }

        // Two lines each - cap the sweep so a full battleground does not flood
        // the chat frame and push the interesting rows off screen.
        constexpr size_t maxReported = 8;
        size_t const reported = std::min(bots.size(), maxReported);
        handler->PSendSysMessage("Playerbot move diagnostics (%u of %u within %.0f yd):",
            uint32(reported), uint32(bots.size()), radius);

        for (size_t index = 0; index < reported; ++index)
        {
            Player* bot = bots[index];

            std::string displayName;
            if (!sCharacterCache->GetCharacterNameByGuid(bot->GetGUID(), displayName) || displayName.empty())
                displayName = bot->GetName();

            MotionMaster const* motionMaster = bot->GetMotionMaster();
            Unit const* victim = bot->GetVictim();

            handler->PSendSysMessage("%s (%s) cls=%u dist=%.1f motion=%u moving=%s combat=%s rage=%u victim=%s@%.1f",
                displayName.c_str(), bot->GetName().c_str(), uint32(bot->GetClass()),
                player->GetDistance(bot),
                uint32(motionMaster ? motionMaster->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE),
                bot->isMoving() ? "yes" : "no",
                bot->IsInCombat() ? "yes" : "no",
                uint32(bot->GetPower(POWER_RAGE) / 10),
                victim ? victim->GetName().c_str() : "none",
                victim ? bot->GetDistance(victim) : 0.0f);

            std::string const moveDiag = playerbot::PvpClassActions::GetLastMovementDebugStatus(bot);
            handler->PSendSysMessage("   move: %s", moveDiag.empty() ? "(none recorded)" : moveDiag.c_str());

            // What the bot last actually tried to cast, and why it failed. The
            // movement line only shows the consequence; this shows the cause.
            std::string const execDiag = playerbot::PvpClassActions::GetLastExecutionStatus(bot);
            handler->PSendSysMessage("   exec: %s", execDiag.empty() ? "(none recorded)" : execDiag.c_str());

            // Sticky: only written when Every Man for Himself is actually
            // chosen, so it can be read long after the moment has passed.
            if (std::string const emfhDiag = playerbot::PvpCore::GetLastEveryManForHimselfDiagnostic(bot); !emfhDiag.empty())
                handler->PSendSysMessage("   emfh: %s", emfhDiag.c_str());

            // Warrior gap closers have a lot of independent gates (known rank,
            // cooldown, stance, rage, min/max range, combat) and a failure in
            // any one of them looks identical from outside: the bot just runs.
            // Clones are memory-only, so this is the only way to see which gate
            // is the one saying no.
            if (bot->GetClass() == CLASS_WARRIOR)
            {
                auto knownRank = [bot](std::initializer_list<uint32> ranks) -> uint32
                {
                    uint32 best = 0;
                    for (uint32 rank : ranks)
                        if (bot->HasSpell(rank))
                            best = rank;
                    return best;
                };
                auto readyText = [bot](uint32 spellId) -> char const*
                {
                    if (!spellId)
                        return "unknown";
                    return bot->GetSpellHistory()->HasCooldown(spellId) ? "cooldown" : "ready";
                };

                uint32 const chargeId = knownRank({ 100, 6178, 11578 });
                uint32 const interceptId = knownRank({ 20252, 20616, 20617 });
                uint32 const leapId = knownRank({ 81271 });
                uint32 const bloodrageId = knownRank({ 2687 });

                char const* stance = bot->HasAura(2457) ? "battle" :
                    (bot->HasAura(71) ? "defensive" : (bot->HasAura(2458) ? "berserker" : "none"));

                float const victimDist = victim ? bot->GetDistance(victim) : -1.0f;
                handler->PSendSysMessage("   warrior: charge=%u/%s intercept=%u/%s leap=%u/%s bloodrage=%u/%s stance=%s rage=%u combat=%s victim_dist=%.1f band8-25=%s",
                    chargeId, readyText(chargeId),
                    interceptId, readyText(interceptId),
                    leapId, readyText(leapId),
                    bloodrageId, readyText(bloodrageId),
                    stance,
                    uint32(bot->GetPower(POWER_RAGE) / 10),
                    bot->IsInCombat() ? "yes" : "no",
                    victimDist,
                    (victimDist >= 8.0f && victimDist <= 25.0f) ? "yes" : "no");

                std::string const gapDiag = playerbot::PvpCore::GetLastWarriorGapCloserDiagnostic(bot);
                handler->PSendSysMessage("   gapclose: %s", gapDiag.empty() ? "(warrior selector has not run)" : gapDiag.c_str());
            }
        }

        return true;
    }

    static bool HandlePlayerbotPvpForceQueueCurrentCommand(ChatHandler* handler)
    {
        if (!handler || !handler->GetSession())
            return false;

        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        BattlegroundTypeId const bgTypeId = ResolveCurrentBgTypeFromPlayerContext(player);
        if (bgTypeId == BATTLEGROUND_TYPE_NONE)
        {
            handler->PSendSysMessage("You must be inside a battleground or queued for one before using this command.");
            return false;
        }

        uint32 const queuedCount = playerbot::QueueEligibleManagedBotsForBattleground(bgTypeId, 0);
        handler->PSendSysMessage("Forced managed playerbots to queue for battleground type %u. Queued bots: %u", uint32(bgTypeId), queuedCount);
        return true;
    }

    // .playerbot pve reset [percent] - rebirth that share of the online
    // managed bots as fresh level-1 characters at their racial start.
    static bool HandlePlayerbotPveResetCommand(ChatHandler* handler, Optional<uint8> percent)
    {
        if (!handler)
            return false;

        uint32 const resetCount = playerbot::PveManager::ResetBotsToLevelOne(percent.value_or(100));
        handler->PSendSysMessage("Reset %u managed playerbots to level 1 and sent them home.", resetCount);
        return true;
    }

    // .playerbot pve rehome - send every online managed bot to the zone its
    // guid assigns it, instead of waiting for relocation to fire on its own.
    static bool HandlePlayerbotPveRehomeCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        uint32 const queued = playerbot::PveManager::RelocateBotsToHomeZones();
        handler->PSendSysMessage("Queued %u managed playerbots to relocate to their own zones.", queued);
        return true;
    }

    // Re-spend every online managed bot's talents against the donor builds.
    // For bots that already spent their points greedily before a recipe was
    // available - a respec is the only thing that fixes those.
    static bool HandlePlayerbotPveRespecCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        uint32 const respecced = playerbot::PveManager::RespecBotsToDonorBuilds();
        handler->PSendSysMessage("Respecced %u managed playerbots onto their donor builds.", respecced);
        return true;
    }

    // The auction half of ".playerbot pve wipe" on its own: empty the house
    // without touching the fleet, for when the market needs a reset but the
    // bots' levels, gear and gold should stand.
    static bool HandlePlayerbotPveClearAuctionsCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        uint32 const removed = playerbot::PveManager::ClearAuctionHouse();
        handler->PSendSysMessage("Cleared %u auctions from every auction house.", removed);
        return true;
    }

    // Full economy reset: an empty auction house and every eligible bot back
    // to level 1, so a fresh run can be measured from zero. The house is
    // cleared FIRST so the reset does not race bots relisting their gear.
    static bool HandlePlayerbotPveWipeCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        uint32 const auctionsRemoved = playerbot::PveManager::ClearAuctionHouse();
        uint32 const resetCount = playerbot::PveManager::ResetBotsToLevelOne(100);
        handler->PSendSysMessage("Wiped %u auctions and reset %u managed playerbots to level 1.",
            auctionsRemoved, resetCount);
        return true;
    }

    static bool HandlePlayerbotPveSummonCommand(ChatHandler* handler, std::string characterName)
    {
        if (!handler || !handler->GetSession())
            return false;

        Player* summoner = handler->GetPlayer();
        if (!summoner)
            return false;

        std::string statusMessage;
        bool const accepted = playerbot::PveManager::RequestCompanionSummon(summoner, characterName, statusMessage);
        handler->PSendSysMessage("%s", statusMessage.c_str());
        return accepted;
    }

    static bool HandlePlayerbotPveDismissCommand(ChatHandler* handler, Optional<std::string> characterName)
    {
        if (!handler || !handler->GetSession())
            return false;

        Player* bot = nullptr;
        if (characterName)
            bot = ObjectAccessor::FindPlayerByName(*characterName);
        else
            bot = handler->getSelectedPlayer();

        if (!bot)
        {
            handler->PSendSysMessage("Select an online playerbot or provide its name.");
            return false;
        }

        std::string statusMessage;
        bool const dismissed = playerbot::PveManager::RequestCompanionDismiss(handler->GetPlayer(), bot, statusMessage);
        handler->PSendSysMessage("%s", statusMessage.c_str());
        return dismissed;
    }

    static bool HandlePlayerbotPveStatusCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        Player* bot = handler->getSelectedPlayer();
        handler->PSendSysMessage("%s", playerbot::PveManager::BuildStatusLine(bot).c_str());
        return true;
    }

    static bool HandlePlayerbotPopulationStatusCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        playerbot::RandomBotPopulationSnapshot const snapshot = playerbot::RandomBotParticipationManager::GetPopulationSnapshot();
        handler->PSendSysMessage("Playerbot random population status:");
        handler->PSendSysMessage(" - configEnabled: %u", snapshot.configEnabled ? 1u : 0u);
        handler->PSendSysMessage(" - runtimeEnabled: %u", snapshot.runtimeEnabled ? 1u : 0u);
        handler->PSendSysMessage(" - loginOrchestrationSupported: %u", snapshot.supportsLoginOrchestration ? 1u : 0u);
        handler->PSendSysMessage(" - targetRange: %u-%u", snapshot.targetMin, snapshot.targetMax);
        handler->PSendSysMessage(" - maxOnlineBotsPerAccount: %u (0 means unlimited)", snapshot.maxOnlineBotsPerAccount);
        handler->PSendSysMessage(" - onlineRandomBots: %u (alliance=%u horde=%u)", snapshot.onlineRandomBots,
            snapshot.onlineAllianceRandomBots, snapshot.onlineHordeRandomBots);
        handler->PSendSysMessage(" - offlinePoolSize: %u", snapshot.offlinePoolSize);
        handler->PSendSysMessage(" - rebalanceTicks: " UI64FMTD, snapshot.rebalanceTicks);
        handler->PSendSysMessage(" - loginAttempts/success: " UI64FMTD "/" UI64FMTD, snapshot.loginAttempts, snapshot.loginSuccess);
        handler->PSendSysMessage(" - logoutAttempts/success: " UI64FMTD "/" UI64FMTD, snapshot.logoutAttempts, snapshot.logoutSuccess);
        handler->PSendSysMessage(" - skippedSafetyRealPlayers: " UI64FMTD, snapshot.skippedSafetyRealPlayers);
        handler->PSendSysMessage(" - skippedNoCandidatePool: " UI64FMTD, snapshot.skippedNoCandidatePool);
        handler->PSendSysMessage(" - skippedIntegrationGap: " UI64FMTD, snapshot.skippedIntegrationGap);
        handler->PSendSysMessage(" - lastRebalanceUnixTime: " UI64FMTD, snapshot.lastRebalanceUnixTime);
        return true;
    }

    static bool HandlePlayerbotPopulationStartCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        playerbot::RandomBotParticipationManager::SetPopulationRuntimeEnabled(true);
        handler->PSendSysMessage("Playerbot random population manager runtime state set to STARTED.");
        return true;
    }

    static bool HandlePlayerbotPopulationStopCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        playerbot::RandomBotParticipationManager::SetPopulationRuntimeEnabled(false);
        handler->PSendSysMessage("Playerbot random population manager runtime state set to STOPPED.");
        return true;
    }

    static bool HandlePlayerbotPopulationRebalanceCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        bool const executed = playerbot::RandomBotParticipationManager::TriggerImmediateRebalance();
        handler->PSendSysMessage("Playerbot random population rebalance executed: %u", executed ? 1u : 0u);
        return true;
    }

    static bool HandlePlayerbotPopulationPoolCommand(ChatHandler* handler)
    {
        if (!handler)
            return false;

        playerbot::RandomBotPopulationSnapshot const snapshot = playerbot::RandomBotParticipationManager::GetPopulationSnapshot();
        handler->PSendSysMessage("Playerbot random population pool stats:");
        handler->PSendSysMessage(" - offlinePoolSize: %u", snapshot.offlinePoolSize);
        handler->PSendSysMessage(" - targetRange: %u-%u", snapshot.targetMin, snapshot.targetMax);
        handler->PSendSysMessage(" - onlineRandomBots: %u", snapshot.onlineRandomBots);
        return true;
    }
};

}

void AddPlayerbotTickCommandScripts();

void AddPlayerbotScripts()
{
    new PlayerbotBootstrapWorldScript();
    new PlayerbotDamageUnitScript();
    new PlayerbotLifecyclePlayerScript();
    new PlayerbotLifecycleCommandScript();
    AddPlayerbotTickCommandScripts();
}
