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

#ifndef TRINITY_PLAYERBOT_PVP_CORE_H
#define TRINITY_PLAYERBOT_PVP_CORE_H

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

class Player;
class GameObject;
class Unit;

namespace playerbot
{
constexpr float PLAYERBOT_MOUNT_ENEMY_AWARENESS_RANGE = 100.0f;
constexpr float PLAYERBOT_HUNTER_AUTOSHOT_MIN_SAFETY_MARGIN = 0.50f;

struct PvpCoreConfig
{
    bool moduleEnabled = false;
    bool pvpCoreEnabled = false;
    bool pvpTacticsEnabled = false;
    bool pvpLifecycleEnabled = false;
    // Keep transient OBC/arena clones fighting while allowing persistent
    // open-world bots to remain out of the PvP queue lifecycle.
    bool pvpPersistentLifecycleEnabled = true;
    bool pvpClassSpellsEnabled = false;
    // Bots reaching for a healing or mana potion when a fight turns. Gated
    // because it changes how a bot fights, and a realm should be able to say no
    // without giving up the rest of the class engine.
    bool usePotions = true;
    float spellRange = 30.0f;
    float healRange = 40.0f;
    float meleeRange = 5.0f;
    float closeRange = 15.0f;
    float longRange = 35.0f;
    uint32 interruptReactionMinMs = 200;
    uint32 interruptReactionMaxMs = 1000;
    uint32 interruptJitterMs = 100;
    uint32 interruptCommitWindowMs = 350;
    uint32 interruptOffTargetExtraMs = 500;
    uint32 interruptOffTargetNoticePct = 50;
};

enum class BattlegroundState : uint8
{
    None = 0,
    Queueing,
    WaitingToStart,
    Active
};

struct HunterAutoShotRangeInfo
{
    float exactDistance = 0.0f;
    float dbcMinRange = 0.0f;
    float meleeRange = 0.0f;
    float minRange = 0.0f;
    float maxRange = 0.0f;
};

struct PvpValues
{
    BattlegroundState battlegroundState = BattlegroundState::None;
    BattlegroundTypeId battlegroundTypeId = BATTLEGROUND_TYPE_NONE;
    bool inBattleground = false;
    bool inBattlegroundQueue = false;
    bool hasBattlegroundQueue = false;
    bool hasArenaQueue = false;
    bool hasBattlegroundInvite = false;
    bool hasArenaInvite = false;
    bool hasArenaTeamInvite = false;
    bool playerHasFlag = false;
    bool flagPickupAvailable = false;
    bool flagPickupNearby = false;
    bool nearbyEnemyActive = false;
    bool enemyFlagCarrierActive = false;
    bool enemyFlagCarrierNear = false;
    bool teamFlagCarrierNear = false;
    bool nodeAssaultAvailable = false;
    bool nodeDefenseAvailable = false;
    uint32 nodeObjectiveId = 0;
    // Base-capture team play (NodeCoordinator, Arathi Basin and The Battle for
    // Gilneas). nodeRole is a NodeRole; it is set whenever the coordinator
    // planned for this bot, and nodeUrgent means the base it was sent to is
    // one of ours that the enemy is taking right now.
    uint8 nodeRole = 0;
    bool nodeUrgent = false;
    // An ObjectiveDuty from ObjectiveCoordinator: a gate to bring down or
    // guard, a commander to kill, the neutral flag to take or hunt. 0 = none.
    uint8 objectiveDuty = 0;
    uint32 battlegroundTeamHumanCount = 0;
    bool battlegroundTeamHasHumans = false;

    // Two-flag capture the flag (Warsong Gulch, Twin Peaks) team play, filled
    // from CtfCoordinator orders. ctfRole is a CtfRole; 0 means the match is
    // not one the coordinator runs, and every flag value above then keeps its
    // plain meaning. When it is set, flagPickupAvailable/flagPickupNearby
    // describe the flag THIS bot was told to take, not merely any usable flag.
    uint8 ctfRole = 0;
    bool ctfDesignatedRunner = false;
    bool ctfEnemyFlagPickable = false;
    bool ctfPickupIsReturn = false;
    bool ctfOpportunisticPickup = false;
    bool ctfTeamCarrierActive = false;
    float ctfTeamCarrierDistance = 0.0f;
    bool ctfCarrierHolding = false;
    bool ctfHandoffGive = false;
    bool ctfHandoffReceive = false;
};

enum class PvpTrigger : uint8
{
    InBattleground = 0,
    BgQueueing,
    BgWaiting,
    BgActive,
    BgInviteActive,
    InBattlegroundWithoutFlag,
    PlayerHasFlag,
    FlagPickupAvailable,
    EnemyFlagCarrierActive,
    EnemyFlagCarrierNear,
    TeamFlagCarrierNear,
    NodeAssaultAvailable,
    NodeDefenseAvailable
};

enum class BattlegroundObjectiveType : uint8
{
    None = 0,
    AssaultNode,
    DefendNode,
    CaptureFlag,
    AttackFlagCarrier,
    ProtectFlagCarrier
};

enum class BattlegroundMovementPrimitive : uint8
{
    None = 0,
    MoveToObjectivePosition,
    MoveToObjectiveUnit,
    FollowFlagCarrier
};

enum class FlagCarrierDirective : uint8
{
    None = 0,
    AttackEnemyCarrier,
    ProtectTeamCarrier
};

struct BattlegroundObjectiveSelection
{
    BattlegroundObjectiveType type = BattlegroundObjectiveType::None;
    uint32 objectiveId = 0;
};

struct BattlegroundTacticalContext
{
    bool tacticsEnabled = false;
    bool shouldEvaluate = false;
    char const* triggerName = nullptr;
    char const* actionName = nullptr;
    float actionPriority = 0.0f;
    BattlegroundObjectiveSelection objective;
    BattlegroundMovementPrimitive movement = BattlegroundMovementPrimitive::None;
    FlagCarrierDirective flagCarrierDirective = FlagCarrierDirective::None;
    bool nearbyEnemyActive = false;
    uint8 ctfRole = 0;
};

enum class QueueOperationType : uint8
{
    None = 0,
    Join,
    Leave
};

enum class InvitationResponseType : uint8
{
    None = 0,
    Accept,
    Decline
};

enum class ArenaTeamInteractionType : uint8
{
    None = 0,
    AcceptInvite,
    DeclineInvite
};

struct PvpClassSpellContext
{
    enum class MovementDirective : uint8
    {
        None = 0,
        ReachMeleeRange,
        ReachSpellRange,
        FleeTooCloseForSpell,
        FaceSpellTarget,
        DropInvalidTarget,
        CheckMountState,
        LeaveShapeshiftForm,
        ResetCombatState
    };

    bool classSpellsEnabled = false;
    bool shouldExecute = false;
    char const* actionName = nullptr;
    char const* reason = nullptr;
    uint32 spellId = 0;
    bool selfCast = false;
    enum class TargetMode : uint8
    {
        None = 0,
        Enemy,
        Self,
        Ally,
        Pet
    };
    TargetMode targetMode = TargetMode::None;
    ObjectGuid targetGuid = ObjectGuid::Empty;
    ObjectGuid allyTargetGuid = ObjectGuid::Empty;
    MovementDirective movementDirective = MovementDirective::None;
    ObjectGuid movementTargetGuid = ObjectGuid::Empty;
    float movementFollowRange = 0.0f;
    float movementPriority = 0.0f;
    bool preserveFlagObjectiveMovement = false;
    bool preserveFlagCarrierMovement = false;
    // A flag runner's escape or speed move (Blink, a form shift, Concussive
    // Shot at a chaser). It is chosen for the run, so the executor lets it
    // through the flag-route movement guard, turns to fire at a chaser behind
    // the bot, and allows the Blink leap.
    bool flagManeuver = false;
    // Carrying, with our own flag away, and already waiting in our flag room.
    // Still no movement orders, but any spell usable from where it stands may
    // be cast, cast times included.
    bool flagCarrierHolding = false;
    uint32 itemEntry = 0;
};

struct BattlegroundLifecycleContext
{
    bool lifecycleEnabled = false;
    QueueOperationType queueOperation = QueueOperationType::None;
    InvitationResponseType invitationResponse = InvitationResponseType::None;
    bool shouldHandleInProgressStatus = false;
};

struct ArenaLifecycleContext
{
    bool lifecycleEnabled = false;
    QueueOperationType queueOperation = QueueOperationType::None;
    ArenaTeamInteractionType teamInteraction = ArenaTeamInteractionType::None;
};

struct RandomBotParticipationHooks
{
    bool lifecycleEnabled = false;
    bool battlegroundParticipationHook = false;
    bool arenaParticipationHook = false;
};

class PvpCore
{
public:
    static void LoadConfig();
    static PvpCoreConfig const& GetConfig();
    static bool GetHunterAutoShotRange(Player const* player, Unit const* target, HunterAutoShotRangeInfo& rangeInfo);

    static PvpValues CollectValues(Player const* player);
    // PvE combat engagement: the PvE manager marks a bot as fighting creatures
    // in the open world, which opens the class-spell gate outside battlegrounds
    // and duels. While engaged, player-scan target acquisition and battleground
    // raid buffing are suppressed; the PvE manager owns target selection via
    // the bot's own selected target.
    static void SetPveCombatEngagement(ObjectGuid const& botGuid, bool engaged);
    static bool IsPveCombatEngaged(Player const* player);
    // Open-world buff upkeep for PvE bots: casts at most ONE missing
    // self-buff (highest known rank) per call, at real mana cost, mana-gated
    // so it never fights the eat/drink loop. Returns true when a cast was
    // issued. The battleground preparation selectors are NOT reusable here:
    // their druid/priest/shaman content lives behind a hard InBattleground()
    // gate plus the zero-mana-cost window that only exists under the
    // Preparation aura.
    static bool TryCastOpenWorldBuff(Player* player);
    static bool CanMageBlinkOutOfControl(Player const* player);
    static bool CanHunterBestialWrathOutOfControl(Player const* player);
    static bool IsEffectivelyImmuneTarget(Player const* player, Unit const* target);
    // Whether damage from THIS bot would break crowd control on the target:
    // the core's take-damage CCs, plus - under
    // Centurion.BreakableCC.FearOthersBreakInstantly - any fear somebody else cast.
    static bool HasBreakableCrowdControlFor(Player const* observer, Unit const* target);
    // True when this cast is allowed to ignore - and must not arm - the shared
    // playerbot dispel throttle. Only an enhancement shaman's Purge qualifies:
    // purging on every available global is the point of the spec.
    static bool IsDispelThrottleExempt(Player const* player, uint32 spellId);
    // True when this decision is the interrupt (or stun/silence/incapacitate)
    // the reaction model released this tick, at the caster it released it for.
    // The battleground fast tick delivers it at once instead of waiting for the
    // class decision cadence, so a bot's kick lands at its own steady time.
    static bool IsInterruptPressDecision(Player const* player, PvpClassSpellContext const& context);
    // Whether the class selector fights this bot from melee range (its
    // spec's melee spacing profile). One answer for the selector and the
    // lifecycle's positioning profile, so the two can never disagree about
    // where an enhancement shaman stands.
    static bool UsesMeleeSpacing(Player const* player);
    static bool IsMovementPreventedByRoot(Player const* player);
    // The spell a stealthed rogue opens with (Garrote 703 or Cheap Shot 1833),
    // or 0 when it knows neither. One answer for the class selector and the
    // lifecycle's move-behind, so the two can never disagree about the opener.
    static uint32 GetRogueStealthOpenerSpellId(Player const* player);
    static bool IsTriggerActive(PvpTrigger trigger, PvpValues const& values);
    static BattlegroundTacticalContext BuildBattlegroundTacticalContext(Player const* player, PvpValues const& values);
    static BattlegroundLifecycleContext BuildBattlegroundLifecycleContext(Player const* player, PvpValues const& values);
    static ArenaLifecycleContext BuildArenaLifecycleContext(Player const* player, PvpValues const& values);
    static PvpClassSpellContext BuildClassSpellContext(Player const* player, PvpValues const& values);
    static RandomBotParticipationHooks BuildRandomBotParticipationHooks(Player const* player, PvpValues const& values);
    static uint32 CountHumanPlayersOnBattlegroundTeam(Player const* player);
    static GameObject* FindUsableLightwell(Player const* player, float maxDistance);
    static bool ShouldSeekLightwell(Player const* player);
    // Whether the bot or a teammate near it is in a fight, in a battleground or
    // an arena: no time to sit down and eat or drink, and time to get up.
    static bool IsTeamInDanger(Player const* player);
    static GameObject* FindUsableSoulwell(Player const* player, float maxDistance);
    static bool HasHealthstone(Player const* player);
    static bool IsBattlegroundFlagCarrier(Player const* player);
    static bool SpellWouldBreakFlagCarry(uint32 spellId);
    // How well this bot runs a flag: 4 druid, 3 protection warrior, 2 mage,
    // 1 hunter, 0 anyone else. The team's designated runner is the highest.
    static uint8 GetFlagRunnerPriority(Player const* player);
    static bool TeamHasHumanPlayers(Player const* player);
    // Records why a warrior did or did not pick a gap closer on its last
    // decision pass. Every gate can fail silently and look identical from
    // outside - the bot simply runs - so the values have to be captured where
    // they are computed. Read back through ".playerbot pvp movediag".
    static std::string GetLastWarriorGapCloserDiagnostic(Player const* player);
    // Persists until the next time the racial is chosen, unlike the gap-closer
    // slot which is rewritten every decision tick. Every Man for Himself fires
    // once and is over, so a per-tick slot could never be read in time.
    static std::string GetLastEveryManForHimselfDiagnostic(Player const* player);

private:
    static bool IsLifecycleEnabled();
    static bool IsInBattlegroundQueue(Player const* player);
    static BattlegroundState DetectBattlegroundState(Player const* player, bool inQueue);
    static BattlegroundObjectiveSelection SelectObjectiveSkeleton(PvpValues const& values);
    static BattlegroundMovementPrimitive SelectMovementPrimitiveSkeleton(PvpValues const& values,
        BattlegroundObjectiveSelection const& objective);
    static FlagCarrierDirective SelectFlagCarrierDirectiveSkeleton(PvpValues const& values);
    static QueueOperationType SelectBattlegroundQueueOperationSkeleton(PvpValues const& values);
    static InvitationResponseType SelectBattlegroundInvitationResponseSkeleton(PvpValues const& values);
    static bool ShouldHandleBattlegroundInProgressStatusSkeleton(PvpValues const& values);
    static QueueOperationType SelectArenaQueueOperationSkeleton(PvpValues const& values);
    static ArenaTeamInteractionType SelectArenaTeamInteractionSkeleton(PvpValues const& values);
};
}

#endif
