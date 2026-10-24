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

#include "GameTime.h"
#include "PlayerbotPvpCore.h"
#include "PlayerbotCtfCoordinator.h"
#include "PlayerbotNodeCoordinator.h"
#include "PlayerbotObjectiveCoordinator.h"
#include "PlayerbotPvpClassActions.h"
#include "PlayerbotRandomBotParticipation.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"
#include "PlayerbotSharedStateGuard.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundTP.h"
#include "BattlegroundWS.h"
#include "CharacterCache.h"
#include "Configuration/Config.h"
#include "Bag.h"
#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "DBCStores.h"
#include "SpellMgr.h"
#include "SpellHistory.h"
#include "Totem.h"
#include "Unit.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
struct SpellDecision;

// Whether a bot or the teammates around it are in a fight - which is no time to
// sit down to eat or drink (EvaluateTeamDanger).
struct TeamDanger
{
    bool inDanger = false;
    ObjectGuid endangeredAlly;      // the lowest on health of the teammates in danger, when asked for
};
TeamDanger EvaluateTeamDanger(Player const* player, bool findAlly);

bool HasHostileTarget(Player const* player, Unit const* target);
void SetWarriorGapCloserDiagnostic(Player const* player, std::string const& diagnostic);
void SetEveryManForHimselfDiagnostic(Player const* player, std::string const& diagnostic);
uint32 ResolveKnownPlayerSpellInChain(Player const* player, uint32 spellId);
bool IsPetSpellReady(Player const* player, uint32 spellId);
bool IsSpellReady(Player const* player, uint32 spellId);
bool MeetsCasterAuraStateRequirements(Player const* player, uint32 spellId);
bool IsFriendlySupportTarget(Player const* player, Unit const* target);
bool HasAuraFromSpellChain(Unit const* unit, uint32 baseSpellId);
bool HasBreakableCrowdControl(Player const* observer, Unit const* unit);
uint32 CountNearbyEnemies(Player const* player, float maxDistance);
SpellDecision SelectOutOfCombatEatDrinkOrMountSpell(Player const* player);
SpellDecision SelectRacialSpell(Player const* player, Unit const* target, Unit const* allyTarget);
bool HasActiveMovementEffectSpline(Player const* player);
bool IsDispelThrottleExemptCast(Player const* player, uint32 spellId);

constexpr float kReferenceHunterMeleeDistance = 5.0f;
constexpr float kReferenceHunterSwitchDistance = 8.0f;
constexpr float kRangedSpacingEnterOutOfRangeBuffer = 2.0f;
constexpr float kRangedSpacingEnterTooCloseBuffer = 1.0f;
// Shared nearby-enemy-count gate for the "mass battle" aoe spells (Arcane
// Explosion, Blizzard, Hurricane, Rain of Fire, Volley, and the Whirlwind
// swarm-priority boost). Bump this to tune how large a fight needs to be
// before bots reach for these instead of single-target rotation.
constexpr uint32 kMassAoeEnemyCountThreshold = 7;
constexpr uint32 kHunterAutoShotSpellId = 75;
constexpr uint32 kHunterCallPetSpellId = 883;
constexpr uint32 kHunterRevivePetSpellId = 982;
constexpr uint32 kPlayerbotHunterStationaryCastLockToken = 900006;
constexpr uint32 kPlayerbotShadowmeldGraceToken = 900007;
// Armed by the executor after a flag runner changes form; see
// SelectDruidFlagRunSpell. Same value in PlayerbotPvpClassActions.cpp.
constexpr uint32 kPlayerbotFlagRunFormShiftToken = 900008;
// Armed when an open-world demon summon the bot stopped for is refused anyway;
// see TryCastOpenWorldBuff.
constexpr uint32 kWarlockOpenWorldSummonRetryToken = 900009;
// Armed by the executor when the core refuses a charge's path. Same value in
// PlayerbotPvpClassActions.cpp.
constexpr uint32 kPlayerbotChargePathRefusedToken = 900010;
constexpr uint32 kWandShootSpellId = 5019;
constexpr uint32 kPlayerbotDispelCooldownToken = 900004;
constexpr uint32 kPlayerbotHandOfSacrificeCooldownToken = 900005;
constexpr uint32 kPaladinSacrificialAuraSpellId = 83256;
constexpr uint32 kDruidCasterFaerieFireSpellId = 9907;
constexpr uint32 kDruidThornsSpellId = 9910;
constexpr uint32 kDruidMassThornsSpellId = 89762;
constexpr uint32 kPriestWeakenedSoulSpellId = 6788;
constexpr uint32 kPriestShadowWordPainSpellId = 589;
constexpr uint32 kPriestDevouringCurseSpellId = 19280;
constexpr uint32 kPriestElunesGraceSpellId = 81351;
constexpr uint32 kPriestWispFormSpellId = 81352;
constexpr uint32 kPriestWyrmsShadowSpellId = 81357;
constexpr uint32 kPriestLightwellSpellId = 27871;
constexpr uint32 kPriestLightwellRenewSpellId = 27874;
constexpr uint32 kPriestLightwellGameObjectEntry = 181106;
constexpr uint32 kWarlockCreateSoulwellSpellId = 29886;
constexpr uint32 kWarlockRitualOfSoulsSpellId = 29893;
constexpr std::array<uint32, 6> kWarlockSoulwellGameObjectEntries = { 181621, 183510, 183511, 193169, 193170, 193171 };
constexpr std::array<uint32, 24> kHealthstoneItemEntries =
{
    36894, 36893, 36892, 36891, 36890, 36889,
    22105, 22104, 22103,
    19013, 19012, 9421,
    19011, 19010, 5510,
    19009, 19008, 5509,
    19007, 19006, 5511,
    19005, 19004, 5512
};
constexpr uint32 kWarlockFirestoneItemEntry = 13701;
constexpr uint32 kWarlockFirestoneUseSpellId = 81334;
constexpr uint32 kMageManaRubyUseSpellId = 22044;
constexpr uint32 kMageManaRubyItemId = 8008;
// Blood Fury has three class-flavored variants in this server's skill_line_ability
// data (skillline 125), all Orc-only: 20572 is the base physical-attack-power
// version (Warrior/Hunter/Rogue and anything else not covered below), 33697 is
// the Shaman melee-AP-plus-spell-damage hybrid, and 33702 is the Warlock pure
// spell-power version.
constexpr uint32 kRacialOrcBloodFurySpellId = 20572;
constexpr uint32 kRacialOrcBloodFuryShamanSpellId = 33697;
constexpr uint32 kRacialOrcBloodFuryWarlockSpellId = 33702;
constexpr uint32 kRacialTrollBloodlustSpellId = 20554;
constexpr uint32 kRacialNightElfShadowmeldSpellId = 20580;
constexpr uint32 kRacialTaurenWarStompSpellId = 20549;
constexpr uint32 kRacialUndeadWillOfTheForsakenSpellId = 7744;
constexpr uint32 kRacialDwarfStoneformSpellId = 20594;
constexpr uint32 kRacialGnomeSurpriseSpellId = 89160;
constexpr uint32 kRacialHumanPerceptionSpellId = 20600;
// Custom trinket-equivalent "Every Man for Himself" abilities - still the
// Human racial, just implemented as five class-flavored variants instead of
// one spell. Granted to Human characters via character_action keyed on class
// group - see the class-group mapping in GetCustomEveryManForHimselfSpellId
// below.
constexpr uint32 kEveryManForHimselfWarlockGroupSpellId = 89148; // Rogue/Warlock
constexpr uint32 kEveryManForHimselfHunterGroupSpellId = 89149;  // Warrior/Hunter/Shaman
constexpr uint32 kEveryManForHimselfPriestGroupSpellId = 89150;  // Paladin/Priest
constexpr uint32 kEveryManForHimselfMageGroupSpellId = 89151;    // Mage
constexpr uint32 kEveryManForHimselfDruidGroupSpellId = 89152;   // Druid and anything else
constexpr float kPlayerbotTotemRefreshDistance = 30.0f;
// Carrier auras are a second source of truth for the short interval between a
// flag click and the next collected battleground-state snapshot. Keep this
// centralized rather than teaching class selectors about individual BGs.
constexpr std::array<uint32, 4> kBattlegroundFlagCarrierAuraIds = { 23333, 23335, 34976, 89798 };
std::unordered_map<ObjectGuid, bool> g_HunterRangedModeByBot;
std::mutex g_HunterRangedModeByBotLock;
std::unordered_map<ObjectGuid, uint8> g_CombatNoTargetTicksByBot;
std::mutex g_CombatNoTargetTicksByBotLock;
std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, bool>> g_HumanPerceptionStealthStateByBot;
std::mutex g_HumanPerceptionStealthStateLock;
// Per-bot interrupt reaction tracking (see ApplyHumanInterruptReaction). Each
// enemy cast the bot has seen is watched on its own, keyed by the caster, so
// two call sites with different ranges (Pummel at 8 yards, Scatter Shot at 5)
// share one reaction per cast instead of overwriting each other's.
struct InterruptCastWatch
{
    uint32 castSpellId = 0;
    // GetCastTimeElapsed() at the last sighting. It only ever grows during one
    // cast (pushback aside), so a drop means the caster started over.
    uint32 lastSeenCastElapsedMs = 0;
    uint32 lastSeenCastingMs = 0;
    // The moment the bot presses its interrupt for this cast.
    uint32 pressAtMs = 0;
    // False when the cast slipped past the bot's attention altogether: it is
    // never answered, and never baits a press either.
    bool noticed = false;
    // Noticed as the bot's own target (full speed), not a caster off to the side.
    bool focused = false;
};
struct InterruptReactionState
{
    std::unordered_map<ObjectGuid, InterruptCastWatch> watchesByCaster;
    // The casters SelectEnemyCastingTarget answered with on its last tick (one
    // per ability that asked), and when. Lets the battleground fast tick deliver
    // that press on time (IsInterruptPressDecision).
    std::vector<ObjectGuid> pressCasterGuids;
    uint32 pressReturnedAtMs = 0;
};
std::unordered_map<ObjectGuid, InterruptReactionState> g_InterruptReactionStateByBot;
std::mutex g_InterruptReactionStateLock;
// Last taunt diagnostic line whispered per bot, so the tick-rate diagnostic only
// speaks on a state change instead of repeating itself (see SelectTauntingEnemyGuid).
std::unordered_map<ObjectGuid, std::string> g_TauntDiagnosticLastLineByBot;
std::mutex g_TauntDiagnosticLastLineLock;
thread_local ObjectGuid g_CurrentDecisionBotGuid = ObjectGuid::Empty;
// Spells the current decision pass has already tried and had refused.
//
// This was a single id, overwritten each attempt - which made the retry loop
// ping-pong between the top two rejected spells and burn all eight attempts
// without ever surfacing a third. Each attempt rebuilds the whole candidate
// vector, grid searches and line-of-sight raycasts included, so that was eight
// full rebuilds to enumerate two spells. A set makes eight attempts mean eight
// distinct spells, which matters most for exactly the bot this immunity work is
// for: one whose entire kit is refused, and which therefore never breaks early.
//
// A pointer to the caller's vector rather than a container of its own, so the
// nesting stays scope-based and nothing allocates on the hot path.
thread_local std::vector<uint32> const* g_SuppressedDecisionSpellIds = nullptr;

    bool IsPriestFlashHealSpell(uint32 spellId)
    {
        if (!spellId)
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        uint32 const firstRankSpellId = spellInfo && spellInfo->GetFirstRankSpell() ? spellInfo->GetFirstRankSpell()->Id : spellId;
        return firstRankSpellId == 2061; // Flash Heal (rank 1)
    }

bool IsPriestInSpiritOfRedemption(Player const* player)
{
    return player && player->GetClass() == CLASS_PRIEST &&
        (player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION) || HasAuraFromSpellChain(player, 27827));
}

    bool IsSpiritOfRedemptionFreeHeal(Player const* player, uint32 spellId)
    {
        return IsPriestInSpiritOfRedemption(player) && IsPriestFlashHealSpell(spellId);
    }

    // The highest rank of a chain the player actually knows, or 0. Spell ids
    // for later ranks are unrelated numbers, so naming one in code silently
    // restricts a behaviour to whoever has trained that exact rank.
    uint32 HighestKnownRankInChain(Player const* player, uint32 firstRankSpellId)
    {
        uint32 best = 0;
        for (uint32 spellId = firstRankSpellId; spellId; spellId = sSpellMgr->GetNextSpellInChain(spellId))
            if (player->HasSpell(spellId))
                best = spellId;
        return best;
    }

    bool IsHunterTrapSpell(uint32 spellId)
    {
        if (!spellId)
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        uint32 const firstRankSpellId = spellInfo && spellInfo->GetFirstRankSpell() ? spellInfo->GetFirstRankSpell()->Id : spellId;

        switch (firstRankSpellId)
        {
        case 1499:  // Freezing Trap
        case 13795: // Immolation Trap
        case 13809: // Frost Trap
        case 13813: // Explosive Trap
            return true;
        default:
            return false;
        }
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

        // Aimed Shot is the hunter hard-cast this lock primarily exists for. Some
        // custom DBC/rank data reports it inconsistently, so recognize known ranks
        // explicitly and give them a real stationary guard.
        if (IsHunterAimedShotSpell(spellInfo))
            return std::max<uint32>(castTimeMs, 3000);

        // Revive Pet is also a real cast-time action and should not be clipped by
        // the stutter/flee loop.
        if (spellInfo->Id == 982)
            return std::max<uint32>(castTimeMs, 1000);

        // Multi-Shot is short, but it still has a stationary firing delay/cast
        // window in this branch. It must be protected from stutter movement too;
        // otherwise the hunter starts Multi-Shot, immediately resumes fleeing, and
        // clips the shot before it launches. Use a short explicit guard even when
        // custom DBC data reports zero cast time.
        if (IsHunterMultiShotSpell(spellInfo))
            return std::max<uint32>(castTimeMs, 500);

        return castTimeMs > 0 ? castTimeMs : 0;
    }

    bool IsActiveHunterCastTimeSpell(Spell const* spell)
    {
        if (!spell || spell->getState() == SPELL_STATE_FINISHED)
            return false;

        SpellInfo const* spellInfo = spell->GetSpellInfo();
        if (!spellInfo)
            return false;

        return GetHunterStationaryCastTimeMs(spellInfo) > 0 ||
            (spellInfo->IsChanneled() && !spellInfo->IsMoveAllowedChannel());
    }

    bool IsHunterCastTimeActionLocked(Player const* player)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return false;

        if (IsActiveHunterCastTimeSpell(player->GetCurrentSpell(CURRENT_GENERIC_SPELL)))
            return true;

        if (IsActiveHunterCastTimeSpell(player->GetCurrentSpell(CURRENT_CHANNELED_SPELL)))
            return true;

        // Do not use broad IsNonMeleeSpellCast() here: the explicit stationarity
        // helper above covers the hunter shots we actually need to protect
        // (Aimed Shot, Multi-Shot, Revive Pet) without catching unrelated delayed
        // spell states.
        return playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotHunterStationaryCastLockToken);
    }

    SpellInfo const* GetFirstOnUseItemSpellInfo(Item const* item)
    {
        if (!item)
            return nullptr;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!itemTemplate)
            return nullptr;

        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            _Spell const& spellData = itemTemplate->Spells[i];
            if (spellData.SpellId <= 0 || spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;

            if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellData.SpellId))
                return spellInfo;
        }

        return nullptr;
    }

    bool IsOnUseItemReady(Player const* player, uint32 itemEntry)
    {
        if (!player || !itemEntry)
            return false;

        Item* item = player->GetItemByEntry(itemEntry);
        if (!item)
            return false;

        if (player->CanUseItem(item) != EQUIP_ERR_OK)
            return false;

        SpellInfo const* itemSpellInfo = GetFirstOnUseItemSpellInfo(item);
        if (!itemSpellInfo)
            return false;

        if (player->GetSpellHistory()->HasCooldown(itemSpellInfo->Id) ||
            player->GetSpellHistory()->HasCooldown(itemSpellInfo, item->GetEntry()) ||
            player->GetSpellHistory()->HasGlobalCooldown(itemSpellInfo))
            return false;

        return true;
    }

uint32 SelectReadyHealthstoneItemEntry(Player const* player)
{
    if (!player)
        return 0;

    for (uint32 itemEntry : kHealthstoneItemEntries)
        if (IsOnUseItemReady(player, itemEntry))
            return itemEntry;

    return 0;
}

// The best restorative potion in the bags, or 0.
//
// Discovered from the SPELL rather than from a list of item ids. A hardcoded
// table would be wrong the moment the realm adds a potion, and this realm adds
// items constantly - the same reason the food/drink picker asks the spell what
// it restores instead of matching names.
//
// Only a DIRECT restore counts. SPELL_EFFECT_HEAL and SPELL_EFFECT_ENERGIZE are
// what a potion does; a periodic-heal aura is a First Aid bandage, which is a
// channel that the next hit cancels, and food and drink are regeneration auras
// that do not work in combat at all. Selecting on the effect separates all three
// without needing to know a single item id.
//
// Largest first, because this is only ever asked when something has gone wrong.
// IsOnUseItemReady covers usability and every cooldown that matters, including
// the shared potion category, so a bot cannot chain two.
uint32 SelectReadyRestorePotionItemEntry(Player const* player, bool wantMana)
{
    // Arena rules here ban restorative health/mana potions only. Other item
    // consumables remain valid: food/drink, healthstones, poisons,
    // spellstones/firestones, and mana gems are intentionally not filtered.
    if (!player || !playerbot::PvpCore::GetConfig().usePotions || player->InArena())
        return 0;

    uint32 bestEntry = 0;
    int32 bestAmount = 0;

    auto const consider = [&](Item const* item)
    {
        if (!item)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE)
            return;

        SpellInfo const* spellInfo = GetFirstOnUseItemSpellInfo(item);
        if (!spellInfo)
            return;

        int32 amount = 0;
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            if (!effect.IsEffect())
                continue;

            if (!wantMana && effect.Effect == SPELL_EFFECT_HEAL)
                amount = std::max(amount, effect.CalcValue(player));
            else if (wantMana && effect.Effect == SPELL_EFFECT_ENERGIZE && effect.MiscValue == POWER_MANA)
                amount = std::max(amount, effect.CalcValue(player));
        }

        if (amount <= 0 || amount <= bestAmount)
            return;

        if (!IsOnUseItemReady(player, proto->ItemId))
            return;

        bestEntry = proto->ItemId;
        bestAmount = amount;
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 bagIndex = 0; bagIndex < bag->GetBagSize(); ++bagIndex)
                consider(bag->GetItemByPos(uint8(bagIndex)));

    return bestEntry;
}

bool IsWarlockSpellstoneItemEntry(uint32 itemEntry)
{
    switch (itemEntry)
    {
        case 5522:  // Spellstone
        case 13602: // Greater Spellstone
        case 13603: // Major Spellstone
        case 22646: // Master Spellstone
        case 41196: // Demonic Spellstone / later-client spellstone variant
            return true;
        default:
            return false;
        }
    }

    uint32 SelectReadyWarlockSpellstoneItemEntry(Player const* player)
    {
        if (!player || player->GetClass() != CLASS_WARLOCK)
            return 0;

        // Prefer the actually equipped wand/relic slot item first. Spellstones are
        // commonly equipped there on this branch, and GetItemByEntry can also find
        // bag copies, so slot-first avoids selecting an older loose stone.
        if (Item const* rangedItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
        {
            uint32 const rangedEntry = rangedItem->GetEntry();
            if (IsWarlockSpellstoneItemEntry(rangedEntry) && IsOnUseItemReady(player, rangedEntry))
                return rangedEntry;
        }

        // Highest-rank-first fallback for spellstones in bags/equipment.
        static constexpr std::array<uint32, 5> kSpellstoneEntries = { 41196, 22646, 13603, 13602, 5522 };
        for (uint32 itemEntry : kSpellstoneEntries)
            if (IsOnUseItemReady(player, itemEntry))
                return itemEntry;

        return 0;
    }


    bool HasPoisonEffect(Player const* player)
    {
        if (!player)
            return false;

    for (Unit::AuraApplicationMap::value_type const& appliedAura : player->GetAppliedAuras())
    {
        AuraApplication const* aurApp = appliedAura.second;
        SpellInfo const* spellInfo = aurApp ? aurApp->GetBase()->GetSpellInfo() : nullptr;
        if (!spellInfo)
            continue;

        // Blind (2094) is classified as a Poison-dispel effect, so Stoneform
        // should clear it exactly like any other poison even if this server's
        // spell data doesn't carry Dispel == DISPEL_POISON on it directly.
        if (spellInfo->Dispel == DISPEL_POISON || spellInfo->Id == 2094)
            return true;
    }

        return false;
    }

    bool HasWillOfTheForsakenBreakableControl(Player const* player)
    {
        if (!player)
            return false;

        constexpr uint32 wotfMechanicMask =
            (1u << MECHANIC_FEAR) |
            (1u << MECHANIC_CHARM) |
            (1u << MECHANIC_SLEEP);

        return player->HasUnitState(UNIT_STATE_FLEEING) ||
            player->HasAuraWithMechanic(wotfMechanicMask);
    }

// Returns the aura that makes Every Man for Himself worth using, or nullptr.
// Deliberately named rather than boolean: "it fired with nothing on me" cannot
// be investigated without knowing what the check actually matched, and the
// mechanic mask is broad enough that an unexpected aura carrying a snare or
// root mechanic looks identical to real crowd control from outside.
SpellInfo const* FindEveryManForHimselfBreakableAura(Player const* player)
{
    if (!player)
        return nullptr;

    for (auto const& auraPair : player->GetAppliedAuras())
    {
        AuraApplication const* application = auraPair.second;
        if (!application || !application->GetBase())
            continue;

        // Helpful auras are never something to trinket out of. This is not a
        // theoretical guard: the arena team markers ("Gold Team" 32724, "Green
        // Team" 32725, and the 35774/35775 pair) carry MECHANIC_TURN in stock
        // spell data, and MECHANIC_TURN sits inside the loss-of-control mask.
        // Without this, every human in every arena reads as permanently crowd
        // controlled and burns Every Man for Himself the moment the gate opens.
        if (application->IsPositive())
            continue;

        SpellInfo const* spellInfo = application->GetBase()->GetSpellInfo();
        if (!spellInfo)
            continue;

        if (spellInfo->Mechanic &&
            (IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK & (1 << spellInfo->Mechanic)))
            return spellInfo;

        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
            if (application->HasEffect(effect.EffectIndex) && effect.IsEffect() && effect.Mechanic &&
                (IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK & (1 << effect.Mechanic)))
                return spellInfo;
    }

    return nullptr;
}

bool HasEveryManForHimselfBreakableControl(Player const* player)
{
    if (!player)
        return false;

    // Same mechanic mask real spell 59752 grants immunity against (see
    // SpellInfo::_InitializeExplicitTargetMask's SPELL_AURA_MECHANIC_IMMUNITY
    // handling for 59752, which the custom 89148-89152 trinket spells are
    // modeled on), so this stays in lockstep with what they actually clear.
    //
    // Routed through FindEveryManForHimselfBreakableAura rather than calling
    // HasAuraWithMechanic directly, because that helper also filters out
    // helpful auras - without which the arena team markers alone are enough to
    // make this permanently true. See the note there.
    return player->HasUnitState(UNIT_STATE_FLEEING) ||
        FindEveryManForHimselfBreakableAura(player) != nullptr;
}

// The custom Every Man for Himself trinket equivalents are granted to Human
// characters via a character_action insert keyed on class - see the class
// groupings alongside the kEveryManForHimself*GroupSpellId constants above.
// Caller is responsible for checking RACE_HUMAN before using this.
uint32 GetCustomEveryManForHimselfSpellId(Player const* player)
{
    if (!player)
        return 0;

    switch (player->GetClass())
    {
        case CLASS_ROGUE:
        case CLASS_WARLOCK:
            return kEveryManForHimselfWarlockGroupSpellId;
        case CLASS_WARRIOR:
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
            return kEveryManForHimselfHunterGroupSpellId;
        case CLASS_PALADIN:
        case CLASS_PRIEST:
            return kEveryManForHimselfPriestGroupSpellId;
        case CLASS_MAGE:
            return kEveryManForHimselfMageGroupSpellId;
        default:
            return kEveryManForHimselfDruidGroupSpellId;
    }
}

// Orc-only racial - see the kRacialOrcBloodFury* constants above for the
// three class-flavored variants this server's skill_line_ability data grants.
// Caller is responsible for checking RACE_ORC before using this.
uint32 GetOrcBloodFurySpellId(Player const* player)
{
    if (!player)
        return kRacialOrcBloodFurySpellId;

    switch (player->GetClass())
    {
        case CLASS_SHAMAN:
            return kRacialOrcBloodFuryShamanSpellId;
        case CLASS_WARLOCK:
            return kRacialOrcBloodFuryWarlockSpellId;
        default:
            return kRacialOrcBloodFurySpellId;
    }
}

bool HasCastTimeSpellTargetingPlayer(Unit const* caster, Player const* target)
{
    if (!caster || !target)
        return false;

        auto isCastTimeSpellTargetingPlayer = [target](Spell const* spell) -> bool
        {
            if (!spell)
                return false;

            SpellInfo const* spellInfo = spell->GetSpellInfo();
            if (!spellInfo || spellInfo->CalcCastTime() <= 0)
                return false;

            return spell->m_targets.GetUnitTargetGUID() == target->GetGUID();
        };

        return isCastTimeSpellTargetingPlayer(caster->GetCurrentSpell(CURRENT_GENERIC_SPELL)) ||
            isCastTimeSpellTargetingPlayer(caster->GetCurrentSpell(CURRENT_CHANNELED_SPELL));
    }

    Unit const* SelectEnemyCastingAtPlayer(Player const* player, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (HasCastTimeSpellTargetingPlayer(candidate, player))
                return candidate;
        }

        return nullptr;
    }

    Unit const* SelectRandomEnemyWithoutBreakableCrowdControl(Player const* player, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        std::vector<Unit const*> candidates;
        candidates.reserve(8);

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (!candidate->IsAlive() || !player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (HasBreakableCrowdControl(player, candidate))
                continue;
            candidates.push_back(candidate);
        }

        if (candidates.empty())
            return nullptr;

        return candidates[urand(0u, static_cast<uint32>(candidates.size() - 1))];
    }

    bool HumanRecentlySawStealthTransition(Player const* player, float maxDistance)
    {
        if (!player || player->GetRace() != RACE_HUMAN || !player->FindMap())
            return false;

        bool shouldPerceive = false;
        std::lock_guard<std::mutex> lock(g_HumanPerceptionStealthStateLock);
        std::unordered_map<ObjectGuid, bool>& seenStealthByEnemy = g_HumanPerceptionStealthStateByBot[player->GetGUID()];

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate) || !candidate->IsAlive() || !player->IsWithinDistInMap(candidate, maxDistance))
                continue;

            bool const currentlyStealthed = candidate->HasStealthAura();
            auto knownItr = seenStealthByEnemy.find(candidate->GetGUID());
            bool const wasKnown = knownItr != seenStealthByEnemy.end();
            bool const wasStealthed = wasKnown ? knownItr->second : currentlyStealthed;

            if (wasKnown && !wasStealthed && currentlyStealthed)
                shouldPerceive = true;

            seenStealthByEnemy[candidate->GetGUID()] = currentlyStealthed;
        }

        return shouldPerceive;
    }

    bool WantsNightElfShadowmeldUtility(Player const* player)
    {
        if (!player || !player->IsInCombat())
            return false;

        // Dropping out of a fight to drink or eat leaves a teammate in it alone.
        bool const teammateInDanger = !EvaluateTeamDanger(player, true).endangeredAlly.IsEmpty();
        bool const wantsDrink = !teammateInDanger && player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 35.0f;
        bool const wantsEat = !teammateInDanger && player->HealthBelowPct(50);
        bool const hunterWantsTrap = player->GetClass() == CLASS_HUNTER &&
            (IsSpellReady(player, 1499) || IsSpellReady(player, 14311) || IsSpellReady(player, 13809));
        bool const rogueWantsStealth = player->GetClass() == CLASS_ROGUE && !player->HasStealthAura() && IsSpellReady(player, 1784);

        return wantsDrink || wantsEat || hunterWantsTrap || rogueWantsStealth;
    }


    bool IsPlayerbotDispelSpell(uint32 spellId)
    {
        if (!spellId)
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        uint32 const firstRankSpellId = spellInfo && spellInfo->GetFirstRankSpell() ? spellInfo->GetFirstRankSpell()->Id : spellId;

        switch (firstRankSpellId)
        {
        case 370:  // Purge
        case 475:  // Remove Lesser Curse
        case 527:  // Dispel Magic
        case 2782: // Remove Curse
        case 2893: // Abolish Poison
        case 4987: // Cleanse
        // The T2 Purge wrappers (81324/81325). Effect 0 is TRIGGER_SPELL, not
        // DISPEL, and spell_ranks makes them their OWN chain rather than ranks
        // of 370 - so neither test below sees them, and restoration's purge
        // (which casts the wrapper) answered to no throttle at all.
        case 81324:
            return true;
        default:
            break;
    }

    if (spellInfo)
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
            if (effect.Effect == SPELL_EFFECT_DISPEL)
                return true;

    return false;
}

    // A spell whose ONLY effect is "cast that other spell" - the shape of this
    // fork's Purge wrappers (81324 -> 370, 81325 -> 8012). Every declarative
    // cast requirement lives on the payload; the wrapper itself declares
    // nothing, so a check that reads the wrapper waves through a cast the real
    // spell would refuse. Returns null for anything that also does work of its
    // own, so a spell that triggers something ALONGSIDE its own effects is
    // still judged on its own terms.
    SpellInfo const* GetPureTriggerPayload(SpellInfo const* spellInfo)
    {
        if (!spellInfo)
            return nullptr;

        SpellInfo const* payload = nullptr;
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            if (!effect.IsEffect())
                continue;

            if (effect.Effect != SPELL_EFFECT_TRIGGER_SPELL || !effect.TriggerSpell || payload)
                return nullptr;

            payload = sSpellMgr->GetSpellInfo(effect.TriggerSpell);
        }

        return payload;
    }

    // Both Purge chains: the stock 370/8012 and the T2 wrappers 81324/81325
    // that trigger it. The bot's shaman table reaches for either one.
    bool IsPurgeFamilySpell(uint32 spellId)
    {
        if (!spellId)
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        uint32 const firstRankSpellId = spellInfo && spellInfo->GetFirstRankSpell() ? spellInfo->GetFirstRankSpell()->Id : spellId;

        return firstRankSpellId == 370 || firstRankSpellId == 81324;
    }

    bool IsHunterInRangedMode(Player const* player)
    {
        if (!player)
            return true;

        std::lock_guard<std::mutex> lock(g_HunterRangedModeByBotLock);
        auto itr = g_HunterRangedModeByBot.find(player->GetGUID());
        if (itr == g_HunterRangedModeByBot.end())
            return true;

        return itr->second;
    }

    void UpdateHunterCombatMode(Player const* player, Unit const* target)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return;

        bool rangedMode = IsHunterInRangedMode(player);
        if (!target || !target->IsAlive())
        {
            {
                std::lock_guard<std::mutex> lock(g_HunterRangedModeByBotLock);
                g_HunterRangedModeByBot[player->GetGUID()] = true;
            }
            TC_LOG_DEBUG("playerbots.pvp.classspell",
                "Hunter mode reset to ranged: botGuid={} reason=no-valid-target.",
                player->GetGUID().ToString());
            return;
        }

        float const distance = player->GetExactDist(target);
        bool const previousRangedMode = rangedMode;
        if (rangedMode)
        {
            // Do not collapse the 5-8y hunter dead-zone into melee mode. Melee
            // pressure is only valid at true melee distance; the dead-zone must be
            // handled by explicit retreat movement before ranged spell selection.
            if (distance <= kReferenceHunterMeleeDistance)
                rangedMode = false;
        }
        else
        {
            // Once the hunter has created real ranged-weapon separation, immediately
            // return to ranged mode even if the enemy is still targeting/chasing the
            // hunter. The old victim check kept hunters stuck in melee mode while a
            // warrior/rogue followed them, so they would Wing Clip once and never
            // resume shooting.
            if (distance >= kReferenceHunterSwitchDistance)
                rangedMode = true;
        }

        {
            std::lock_guard<std::mutex> lock(g_HunterRangedModeByBotLock);
            g_HunterRangedModeByBot[player->GetGUID()] = rangedMode;
        }

        if (previousRangedMode != rangedMode)
        {
            TC_LOG_DEBUG("playerbots.pvp.classspell",
                "Hunter mode switch: botGuid={} targetGuid={} previousMode={} newMode={} distance={} targetVictimIsBot={}.",
                player->GetGUID().ToString(), target->GetGUID().ToString(), previousRangedMode ? "ranged" : "melee",
                rangedMode ? "ranged" : "melee", distance, target->GetVictim() == player ? 1 : 0);
        }
    }

float GetHunterDeadZoneMaxRange(Player const* player, Unit const* target)
{
    playerbot::HunterAutoShotRangeInfo rangeInfo;
    if (!playerbot::PvpCore::GetHunterAutoShotRange(player, target, rangeInfo))
        return kReferenceHunterSwitchDistance;

    return rangeInfo.minRange;
}

bool IsHunterWithinAutoShotBand(Player const* player, Unit const* target)
{
    playerbot::HunterAutoShotRangeInfo rangeInfo;
    if (!playerbot::PvpCore::GetHunterAutoShotRange(player, target, rangeInfo))
        return false;

    return player->IsWithinLOSInMap(target) &&
        rangeInfo.exactDistance > rangeInfo.minRange + playerbot::PLAYERBOT_HUNTER_AUTOSHOT_MIN_SAFETY_MARGIN &&
        rangeInfo.exactDistance <= rangeInfo.maxRange;
}

    uint8 IncrementCombatNoTargetTicks(Player const* player)
    {
        if (!player)
            return 0;

        std::lock_guard<std::mutex> lock(g_CombatNoTargetTicksByBotLock);
        uint8& ticks = g_CombatNoTargetTicksByBot[player->GetGUID()];
        ticks = std::min<uint8>(static_cast<uint8>(ticks + 1), static_cast<uint8>(20));
        return ticks;
    }

    void ResetCombatNoTargetTicks(Player const* player)
    {
        if (!player)
            return;

        std::lock_guard<std::mutex> lock(g_CombatNoTargetTicksByBotLock);
        g_CombatNoTargetTicksByBot.erase(player->GetGUID());
    }

    SpellDecision MaybeSelectUtilitySpell(Player const* player, Unit const* hostileTarget);

    playerbot::PvpCoreConfig g_PvpCoreConfig;
    bool CanAttemptMount(Player const* player, SpellInfo const* mountSpellInfo);
    bool IsHardControlled(Player const* player);
    bool IsEffectivelyOutdoors(Player const* player);
    bool IsStrictlyOutdoorsForMount(Player const* player);
    bool HasNearbyAttackableEnemyPlayer(Player const* player, float maxDistance);

    float GetConfiguredSpellRange() { return g_PvpCoreConfig.spellRange; }
    float GetConfiguredHealRange() { return g_PvpCoreConfig.healRange; }
    float GetConfiguredMeleeRange() { return g_PvpCoreConfig.meleeRange; }
    float GetConfiguredCloseRange() { return g_PvpCoreConfig.closeRange; }
    float GetConfiguredLongRange() { return g_PvpCoreConfig.longRange; }

bool IsHunterExactDeadZone(Player const* player, Unit const* target)
{
    playerbot::HunterAutoShotRangeInfo rangeInfo;
    if (!playerbot::PvpCore::GetHunterAutoShotRange(player, target, rangeInfo))
        return false;

    // SPELL_RANGE_RANGED adds the caster/target melee range to the raw DBC
    // minimum. For normal players this is the familiar 3 + 5 = 8 yard Auto
    // Shot floor, but using the core-equivalent values also handles combat
    // reach and custom melee-range modifiers correctly.
    return rangeInfo.exactDistance > rangeInfo.meleeRange &&
        rangeInfo.exactDistance < rangeInfo.minRange;
}

    Unit const* SelectHunterDeadZoneEnemy(Player const* player, Unit const* preferredTarget)
    {
        if (!player || player->GetClass() != CLASS_HUNTER)
            return nullptr;

        if (HasHostileTarget(player, preferredTarget) &&
            player->IsWithinLOSInMap(preferredTarget) &&
            IsHunterExactDeadZone(player, preferredTarget))
            return preferredTarget;

        if (!player->FindMap())
            return nullptr;

        Unit const* bestTarget = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate) || !player->IsWithinLOSInMap(candidate) || !IsHunterExactDeadZone(player, candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestTarget = candidate;
            }
        }

        return bestTarget;
    }

    float ComputeHunterDeadZoneRetreatStep(Player const* player, Unit const* target)
    {
        if (!player || !target)
            return 6.0f;

    float const currentDistance = player->GetExactDist(target);
    float const desiredExactDistance = std::max(GetHunterDeadZoneMaxRange(player, target) + 4.0f, 12.0f);
    return std::clamp(desiredExactDistance - currentDistance, 4.0f, 10.0f);
}

    float ComputeHunterMeleeKiteRetreatStep(Player const* player, Unit const* target)
    {
        if (!player || !target)
            return 10.0f;

    float const currentDistance = player->GetExactDist(target);
    float const desiredExactDistance = std::max(GetHunterDeadZoneMaxRange(player, target) + 5.0f, 13.0f);
    return std::clamp(desiredExactDistance - currentDistance, 8.0f, 14.0f);
}

    bool IsHunterMeleeKiteTargetControlled(Unit const* target)
    {
        if (!target)
            return false;

        return HasAuraFromSpellChain(target, 14268) || // Wing Clip
            target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
            target->HasAuraWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_STUN));
    }

    float GetConfiguredCombatRange()
    {
        float range = std::max(GetConfiguredLongRange(), GetConfiguredSpellRange());
        range = std::max(range, GetConfiguredCloseRange());
        range = std::max(range, GetConfiguredMeleeRange());
        return std::max(range, 0.0f);
    }

    bool IsLifecycleGateEnabled(playerbot::PvpCoreConfig const& config)
    {
        return config.moduleEnabled && config.pvpCoreEnabled && config.pvpLifecycleEnabled;
    }

    bool IsClassSpellGateEnabled(playerbot::PvpCoreConfig const& config)
    {
        return config.moduleEnabled && config.pvpCoreEnabled && config.pvpClassSpellsEnabled;
    }

    bool IsLowOrOutOfManaForFallback(Player const* player)
    {
        if (!player || player->GetPowerType() != POWER_MANA)
            return false;

        return player->GetPowerPct(POWER_MANA) <= 10.0f || player->GetPower(POWER_MANA) < 250;
    }

    bool HasWandEquipped(Player const* player)
    {
        if (!player)
            return false;

        Item const* ranged = player->GetWeaponForAttack(RANGED_ATTACK, true);
        if (!ranged || !ranged->GetTemplate())
            return false;

        return ranged->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_WAND;
    }

    bool IsAutoRepeatRangedSpellActive(Player const* player, uint32 spellId)
    {
        if (!player || !spellId)
            return false;

        Spell const* autoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        if (!autoRepeat)
            return false;

        SpellInfo const* activeAutoRepeatInfo = autoRepeat->GetSpellInfo();
        return activeAutoRepeatInfo && activeAutoRepeatInfo->Id == spellId;
    }

    bool IsWandShootReadyForDecision(Player const* player)
    {
        return HasWandEquipped(player) &&
            IsSpellReady(player, kWandShootSpellId) &&
            !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kWandShootSpellId) &&
            !IsAutoRepeatRangedSpellActive(player, kWandShootSpellId);
    }


    // Mounting parks the pet (Unit::Mount -> UnsummonPetTemporaryIfAny) and
    // dismounting brings it back by itself, so a mounted owner whose pet is
    // parked is not missing one. Answering "summon/call it" was a loop: the
    // summon is a cast the mount refuses, PvpClassActions::Execute dismounts
    // for exactly that, the dismount returns the pet, and the out-of-combat
    // mount rule puts the bot straight back in the saddle. Hunters rode at a
    // far target doing nothing else; warlocks did it all through preparation
    // and, mid-match, burned Fel Domination or stood through a ten-second
    // replacement summon on every enemy that came near.
    bool IsPetParkedByMount(Player const* player)
    {
        return player->IsMounted() && player->GetTemporaryUnsummonedPetNumber();
    }

    struct HunterPetDecisionState
    {
        bool hasActivePet = false;
        bool hasLivingPet = false;
        bool hasDeadPet = false;
        bool hasLoadableHunterPet = false;
        bool loadablePetDead = false;
        bool loadablePetTameable = false;
        bool canCallPet = false;
        bool shouldRevivePet = false;
    };

    HunterPetDecisionState GetHunterPetDecisionState(Player const* player)
    {
        HunterPetDecisionState state;
        if (!player || player->GetClass() != CLASS_HUNTER)
            return state;

        if (Pet const* pet = player->GetPet())
        {
            state.hasActivePet = true;
            state.hasLivingPet = pet->IsAlive();
            state.hasDeadPet = !pet->IsAlive();
            state.shouldRevivePet = state.hasDeadPet;
            return state;
        }

        // Parked by the mount: nothing to call or revive (IsPetParkedByMount).
        if (IsPetParkedByMount(player))
            return state;

        PetStable const* petStable = player->GetPetStable();
        if (!petStable)
            return state;

        std::pair<PetStable::PetInfo const*, PetSaveMode> const loadInfo = Pet::GetLoadPetInfo(*petStable, 0, 0, false);
        PetStable::PetInfo const* petInfo = loadInfo.first;
        if (!petInfo || petInfo->Type != HUNTER_PET)
            return state;

        state.hasLoadableHunterPet = true;
        state.loadablePetDead = petInfo->Health == 0;

        CreatureTemplate const* creatureInfo = sObjectMgr->GetCreatureTemplate(petInfo->CreatureId);
        state.loadablePetTameable = creatureInfo && creatureInfo->IsTameable(player->CanTameExoticPets());
        state.canCallPet = !state.loadablePetDead && state.loadablePetTameable;
        state.shouldRevivePet = state.loadablePetDead;
        return state;
    }

    bool IsFlagCarrierNear(Player const* player, ObjectGuid const& carrierGuid, float maxDistance)
    {
        if (!player || carrierGuid.IsEmpty())
            return false;

        Player const* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid);
        if (!carrier || !carrier->IsAlive() || carrier->GetMapId() != player->GetMapId())
            return false;

        return player->IsWithinDistInMap(carrier, maxDistance);
    }

    void PopulateObjectiveStateTriggers(Player const* player, playerbot::PvpValues& values)
    {
        if (!player || !values.inBattleground)
            return;

        Battleground* battleground = player->GetBattleground();
        if (!battleground || battleground->GetStatus() != STATUS_IN_PROGRESS)
            return;

    uint32 const botTeamValue = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
    TeamId const botTeam = (botTeamValue == ALLIANCE || botTeamValue == TEAM_ALLIANCE) ? TEAM_ALLIANCE :
        (botTeamValue == HORDE || botTeamValue == TEAM_HORDE) ? TEAM_HORDE : player->GetTeamId();
    TeamId const enemyTeam = (botTeam == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;
    ObjectGuid const playerGuid = player->GetGUID();
    ObjectGuid const flagPickupGuid = battleground->GetFlagPickupGUID(playerGuid);
    values.flagPickupAvailable = !flagPickupGuid.IsEmpty();
    if (values.flagPickupAvailable)
        if (Map* map = player->FindMap())
            if (GameObject* flag = map->GetGameObject(flagPickupGuid))
                values.flagPickupNearby = flag->IsInWorld() && player->IsWithinDistInMap(flag, 10.0f);

    // Use one local-pressure boundary for tactical objective selection and
    // class movement ownership. A midfield bot should break off its flag route
    // to fight nearby enemies, while distant enemies must not pull it into a
    // map-wide chase. LOS is intentionally not required: an enemy immediately
    // around a corner is still local pressure that combat pathing should handle.
    float const nearbyEnemyBoundary = std::max(GetConfiguredLongRange(), 35.0f);
    if (Map const* map = player->FindMap())
    {
        Map::PlayerList const& mapPlayers = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player const* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate) ||
                !player->IsWithinDistInMap(candidate, nearbyEnemyBoundary))
                continue;

            values.nearbyEnemyActive = true;
            break;
        }
    }

    // Warsong Gulch and Twin Peaks are played as a team: one designated flag
    // runner, everyone else near a flag, returning ours, escorting or holding.
    // From here on flagPickupAvailable/Nearby describe the flag THIS bot was
    // told to take; the battleground's own pickup answer is any usable flag,
    // which sent the whole team after it at once.
    playerbot::CtfBotOrders ctfOrders;
    if (playerbot::CtfCoordinator::GetOrders(player, ctfOrders))
    {
        values.ctfRole = uint8(ctfOrders.role);
        values.ctfDesignatedRunner = ctfOrders.isDesignatedRunner;
        values.ctfEnemyFlagPickable = ctfOrders.enemyFlagPickable;
        values.ctfPickupIsReturn = ctfOrders.pickupIsReturn;
        values.ctfOpportunisticPickup = ctfOrders.pickupIsOpportunistic;
        values.ctfCarrierHolding = ctfOrders.carrierHolding;
        values.ctfHandoffGive = ctfOrders.handoffGive;
        values.ctfHandoffReceive = ctfOrders.handoffReceive;
        values.flagPickupAvailable = !ctfOrders.pickupGuid.IsEmpty();
        values.flagPickupNearby = ctfOrders.pickupNearby;

        if (!ctfOrders.teamCarrierGuid.IsEmpty() && ctfOrders.teamCarrierGuid != playerGuid)
        {
            if (Player const* teamCarrier = ObjectAccessor::GetPlayer(*player, ctfOrders.teamCarrierGuid);
                teamCarrier && teamCarrier->IsAlive())
            {
                values.ctfTeamCarrierActive = true;
                values.ctfTeamCarrierDistance = player->GetDistance(teamCarrier);
            }
        }
    }

    // Arathi Basin and The Battle for Gilneas are played as a team: the
    // coordinator hands out the bases, with a quota of bodies per base driven
    // by who is standing on it, so a base of ours under attack pulls the
    // nearest bots back instead of the fixed third of the roster a guid hash
    // used to park there. Every other node battleground keeps the plain
    // per-bot answer below.
    playerbot::NodeBotOrders nodeOrders;
    if (playerbot::NodeCoordinator::GetOrders(player, nodeOrders) && nodeOrders.hasNode)
    {
        values.nodeObjectiveId = nodeOrders.nodeId;
        values.nodeRole = uint8(nodeOrders.role);
        values.nodeUrgent = nodeOrders.role == playerbot::NodeRole::Recapture;
        values.nodeDefenseAvailable = !nodeOrders.interact;
        values.nodeAssaultAvailable = nodeOrders.interact;
    }
    else if (BattlegroundNodeObjective nodeObjective; battleground->GetNodeObjective(playerGuid, nodeObjective))
    {
        values.nodeObjectiveId = nodeObjective.NodeId;
        bool const isDefense = nodeObjective.Status == BattlegroundNodeStatus::FriendlyControlled ||
            nodeObjective.Status == BattlegroundNodeStatus::FriendlyContested ||
            nodeObjective.Status == BattlegroundNodeStatus::FriendlyUnderAttack;
        values.nodeDefenseAvailable = isDefense;
        values.nodeAssaultAvailable = !isDefense;
    }

    // Gates, commanders and the neutral flag of a base match (Strand of the
    // Ancients, Isle of Conquest, Alterac Valley, Eye of the Storm). The bots
    // given a duty were left out of the base plan above.
    playerbot::ObjectiveBotOrders dutyOrders;
    if (playerbot::ObjectiveCoordinator::GetOrders(player, dutyOrders))
        values.objectiveDuty = uint8(dutyOrders.duty);

    if (BattlegroundWS* bgWs = dynamic_cast<BattlegroundWS*>(battleground))
    {
        ObjectGuid const enemyCarrierGuid = bgWs->GetFlagPickerGUID(botTeam);
        ObjectGuid const teamCarrierGuid = bgWs->GetFlagPickerGUID(enemyTeam);

            values.playerHasFlag = (teamCarrierGuid == playerGuid);
            values.enemyFlagCarrierActive = !enemyCarrierGuid.IsEmpty();
            values.enemyFlagCarrierNear = IsFlagCarrierNear(player, enemyCarrierGuid, 100.0f);

            bool const bothFlagsNotAtBase =
                bgWs->GetFlagState(ALLIANCE) != BG_WS_FLAG_STATE_ON_BASE &&
                bgWs->GetFlagState(HORDE) != BG_WS_FLAG_STATE_ON_BASE;
            if (!bothFlagsNotAtBase)
                values.teamFlagCarrierNear = IsFlagCarrierNear(player, teamCarrierGuid, 200.0f);

            return;
        }

        if (BattlegroundTP* bgTp = dynamic_cast<BattlegroundTP*>(battleground))
        {
            ObjectGuid const enemyCarrierGuid = bgTp->GetFlagPickerGUID(botTeam);
            ObjectGuid const teamCarrierGuid = bgTp->GetFlagPickerGUID(enemyTeam);

            values.playerHasFlag = (teamCarrierGuid == playerGuid);
            values.enemyFlagCarrierActive = !enemyCarrierGuid.IsEmpty();
            values.enemyFlagCarrierNear = IsFlagCarrierNear(player, enemyCarrierGuid, 100.0f);
            values.teamFlagCarrierNear = IsFlagCarrierNear(player, teamCarrierGuid, 200.0f);
            return;
        }

    // Called once per carrier, so every value only ever turns on: a battleground
    // with several carriers at once (Temple of Kotmogu's orbs) must not let a
    // later carrier clear what an earlier one set.
    auto populateNeutralFlagCarrierValues = [&](ObjectGuid const& carrierGuid)
    {
        if (carrierGuid.IsEmpty())
            return;

            if (carrierGuid == playerGuid)
                values.playerHasFlag = true;
            Player const* carrier = ObjectAccessor::FindConnectedPlayer(carrierGuid);
            if (!carrier || !carrier->IsAlive() || carrier->GetMapId() != player->GetMapId())
                return;

        uint32 const assignedCarrierTeam = battleground->GetPlayerTeam(carrierGuid);
        TeamId const carrierTeam = assignedCarrierTeam == ALLIANCE ? TEAM_ALLIANCE :
            assignedCarrierTeam == HORDE ? TEAM_HORDE : carrier->GetTeamId();
        if (carrierTeam == botTeam)
        {
            if (player->IsWithinDistInMap(carrier, 200.0f))
                values.teamFlagCarrierNear = true;
        }
        else
        {
            values.enemyFlagCarrierActive = true;
            if (player->IsWithinDistInMap(carrier, 100.0f))
                values.enemyFlagCarrierNear = true;
        }
    };

    std::vector<ObjectGuid> carriers;
    battleground->GetObjectiveCarriers(carriers);
    for (ObjectGuid const& carrierGuid : carriers)
        populateNeutralFlagCarrierValues(carrierGuid);

    // The neutral flag of a base match (Eye of the Storm) is played by the
    // bots the duty plan sends for it. Everybody else stays on the bases,
    // which are where that flag is scored: it picks the flag up only when it
    // is standing on it, turns on an enemy carrier only when he is close, and
    // escorts ours only when it is already beside him.
    if (playerbot::NodeCoordinator::IsNodeBattleground(battleground) &&
        !values.playerHasFlag && values.objectiveDuty != uint8(playerbot::ObjectiveDuty::FlagRunner))
    {
        values.flagPickupAvailable = values.flagPickupNearby;
        values.enemyFlagCarrierActive = values.objectiveDuty == uint8(playerbot::ObjectiveDuty::FlagHunt) ||
            values.enemyFlagCarrierNear;
        values.teamFlagCarrierNear = values.teamFlagCarrierNear &&
            IsFlagCarrierNear(player, battleground->GetFlagPickerGUID(), 40.0f);
    }
}

    struct SpellDecision
    {
        char const* actionName = nullptr;
        char const* reason = nullptr;
        uint32 spellId = 0;
        playerbot::PvpClassSpellContext::TargetMode targetMode = playerbot::PvpClassSpellContext::TargetMode::None;
        ObjectGuid targetGuid = ObjectGuid::Empty;
        uint32 itemEntry = 0;
        char const* triggerName = nullptr;
    };

    struct PrioritizedSpellDecision
    {
        float priority = 0.0f;
        SpellDecision decision;
    };

    SpellDecision SelectRacialSpell(Player const* player, Unit const* target, Unit const* allyTarget)
    {
        if (!player || !player->IsAlive())
            return {};

        // Avoid breaking rogue/druid stealth openers or Shadowmeld recovery windows
        // with racial utility. Stealthed bots should commit to their opener instead
        // of spending a racial before they engage.
        if (player->HasStealthAura())
            return {};

    // Every Man for Himself is still the Human racial - it is just implemented
    // as five custom class-flavored variants (89148-89152) instead of the
    // single real spell 59752, granted to Human characters via
    // character_action based on class. Check this before the per-race switch
    // below since it needs the class-based spell ID lookup rather than a
    // single constant.
    if (player->GetRace() == RACE_HUMAN && HasEveryManForHimselfBreakableControl(player))
    {
        if (uint32 const emfhSpellId = GetCustomEveryManForHimselfSpellId(player); emfhSpellId && IsSpellReady(player, emfhSpellId))
        {
            // Name what triggered it. Snares and roots are legitimate targets
            // here, so the mask is not the suspect - but if this fires with the
            // bot visibly untouched, the aura it matched is the only thing that
            // can explain it.
            {
                SpellInfo const* trigger = FindEveryManForHimselfBreakableAura(player);
                std::ostringstream emfhDiag;
                emfhDiag << "spell=" << emfhSpellId
                         << " trigger=" << (trigger ? std::to_string(trigger->Id) : std::string("none"))
                         << " trigger_mechanic=" << (trigger ? uint32(trigger->Mechanic) : 0u)
                         << " fleeing=" << (player->HasUnitState(UNIT_STATE_FLEEING) ? "yes" : "no")
                         << " combat=" << (player->IsInCombat() ? "yes" : "no")
                         << " aura_count=" << uint32(player->GetAppliedAuras().size());
                SetEveryManForHimselfDiagnostic(player, emfhDiag.str());
            }
            return { "custom every man for himself", "break stun/root/fear/charm and other loss-of-control effects", emfhSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
        }
    }

    switch (player->GetRace())
    {
        case RACE_ORC:
        {
            bool const wantsThroughput = player->IsInCombat() &&
                ((target && HasHostileTarget(player, target) && !HasBreakableCrowdControl(player, target)) ||
                 (allyTarget && IsFriendlySupportTarget(player, allyTarget) && allyTarget->GetHealthPct() < 85.0f));
            uint32 const bloodFurySpellId = GetOrcBloodFurySpellId(player);
            if (wantsThroughput && IsSpellReady(player, bloodFurySpellId))
                return { "racial blood fury", "orc racial throughput for healing or damage", bloodFurySpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        }
        case RACE_TROLL:
        {
            bool const activelyDoingAnything = player->IsInCombat() ||
                (target && HasHostileTarget(player, target)) ||
                (allyTarget && IsFriendlySupportTarget(player, allyTarget) && allyTarget->GetHealthPct() < 95.0f);
            if (activelyDoingAnything && IsSpellReady(player, kRacialTrollBloodlustSpellId))
                return { "racial bloodlust", "troll racial haste while actively fighting or casting", kRacialTrollBloodlustSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        }
        case RACE_NIGHTELF:
        {
            Unit const* casterTargetingMe = SelectEnemyCastingAtPlayer(player, 45.0f);
            if ((casterTargetingMe || WantsNightElfShadowmeldUtility(player)) && IsSpellReady(player, kRacialNightElfShadowmeldSpellId))
                return { "racial shadowmeld", casterTargetingMe ? "break incoming cast target with shadowmeld" : "drop combat for recovery or setup", kRacialNightElfShadowmeldSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        }
        case RACE_TAUREN:
            if (CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, kRacialTaurenWarStompSpellId))
                return { "racial war stomp", "stomp clustered nearby enemies", kRacialTaurenWarStompSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        case RACE_UNDEAD_PLAYER:
            if (HasWillOfTheForsakenBreakableControl(player) && IsSpellReady(player, kRacialUndeadWillOfTheForsakenSpellId))
                return { "racial will of the forsaken", "break fear charm or sleep", kRacialUndeadWillOfTheForsakenSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        case RACE_DWARF:
            if (HasPoisonEffect(player) && IsSpellReady(player, kRacialDwarfStoneformSpellId))
                return { "racial stoneform", "remove poison effects", kRacialDwarfStoneformSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        case RACE_GNOME:
        {
            // 30 yards is a deliberate subset of the spell's real 45 yard range,
            // not a bug - the castability gate in SelectClassOrUtilitySpell is
            // what stops an unusable pick from starving class selection.
            Unit const* surpriseTarget = IsSpellReady(player, kRacialGnomeSurpriseSpellId) ? SelectRandomEnemyWithoutBreakableCrowdControl(player, 30.0f) : nullptr;
            if (surpriseTarget)
                return { "racial surprise", "throw surprise grenade at random non-cc enemy", kRacialGnomeSurpriseSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, surpriseTarget->GetGUID() };
            break;
        }
        case RACE_HUMAN:
            if (HumanRecentlySawStealthTransition(player, 50.0f) && IsSpellReady(player, kRacialHumanPerceptionSpellId))
                return { "racial perception", "enemy just entered stealth nearby", kRacialHumanPerceptionSpellId, playerbot::PvpClassSpellContext::TargetMode::Self };
            break;
        default:
            break;
        }

        return {};
    }


    bool IsDecisionImmediatelyCastable(Player const* player, SpellDecision const& decision, Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget);
    SpellDecision SelectHighestPriorityCastableDecision(std::vector<PrioritizedSpellDecision>& candidates, Player const* player,
        Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget);

    SpellDecision MaybeSelectUtilitySpell(Player const* player, Unit const* hostileTarget)
    {
        if (!player)
            return {};

        // Spirit of Redemption is a short free-cast healing window. Never let
        // food/drink/mount utility preempt the priest healing selector here, even
        // if the priest is technically out of combat or at 0 mana.
        if (IsPriestInSpiritOfRedemption(player))
            return {};

        constexpr uint32 kPlayerbotDrinkSpell = 22734;
        bool const maintainExistingDrink = !player->IsInCombat() &&
            player->GetMaxPower(POWER_MANA) > 0 &&
            player->HasAura(kPlayerbotDrinkSpell) &&
            player->GetPowerPct(POWER_MANA) < 50.0f &&
            !EvaluateTeamDanger(player, false).inDanger;

    // A selected enemy can remain valid across most of a battleground. Only
    // let it suppress out-of-combat utility when it is actually close enough
    // to matter; the mount selector separately scans for every nearby enemy.
    //
    // Exception: if the bot is already drinking and still below the 50% mana
    // floor, keep utility selection available so drink remains sticky instead
    // of immediately breaking back into combat posture.
    if (HasHostileTarget(player, hostileTarget) &&
        player->IsWithinDistInMap(hostileTarget, playerbot::PLAYERBOT_MOUNT_ENEMY_AWARENESS_RANGE) && !maintainExistingDrink)
        return {};

        return SelectOutOfCombatEatDrinkOrMountSpell(player);
    }

    class DecisionEvaluationScope
    {
    public:
        DecisionEvaluationScope(Player const* player, std::vector<uint32> const* suppressedSpellIds)
            : _previousBotGuid(g_CurrentDecisionBotGuid), _previousSuppressedSpellIds(g_SuppressedDecisionSpellIds)
        {
            g_CurrentDecisionBotGuid = player ? player->GetGUID() : ObjectGuid::Empty;
            g_SuppressedDecisionSpellIds = suppressedSpellIds;
        }

        ~DecisionEvaluationScope()
        {
            g_CurrentDecisionBotGuid = _previousBotGuid;
            g_SuppressedDecisionSpellIds = _previousSuppressedSpellIds;
        }

    private:
        ObjectGuid _previousBotGuid;
        std::vector<uint32> const* _previousSuppressedSpellIds = nullptr;
    };

    void AddDecisionCandidate(std::vector<PrioritizedSpellDecision>& candidates, bool condition, float priority, SpellDecision const& decision)
    {
        if (!condition || (!decision.spellId && !decision.itemEntry))
            return;

        if (g_SuppressedDecisionSpellIds && decision.spellId != 0 &&
            std::find(g_SuppressedDecisionSpellIds->begin(), g_SuppressedDecisionSpellIds->end(),
                decision.spellId) != g_SuppressedDecisionSpellIds->end())
            return;

        candidates.push_back({ priority, decision });
    }

    struct SpellTriggerRule
    {
        char const* triggerName = nullptr;
        bool condition = false;
        float priority = 0.0f;
        SpellDecision decision;
    };

    SpellDecision SelectFromTriggerGraph(Player const* player, Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget,
        std::initializer_list<SpellTriggerRule> rules)
    {
        std::vector<PrioritizedSpellDecision> candidates;
        candidates.reserve(rules.size());

        for (SpellTriggerRule const& rule : rules)
        {
            SpellDecision decision = rule.decision;
            if (!decision.triggerName)
                decision.triggerName = rule.triggerName;
            AddDecisionCandidate(candidates, rule.condition, rule.priority, decision);
        }

        return SelectHighestPriorityCastableDecision(candidates, player, defaultEnemyTarget, defaultAllyTarget);
    }

    SpellDecision SelectHighestPriorityCastableDecision(std::vector<PrioritizedSpellDecision>& candidates, Player const* player,
        Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget)
    {
        if (candidates.empty())
            return {};

        std::stable_sort(candidates.begin(), candidates.end(), [](PrioritizedSpellDecision const& left, PrioritizedSpellDecision const& right)
        {
            return left.priority > right.priority;
        });

        if (!player)
            return candidates.front().decision;

        // Cast-time hunter actions are exclusive. Do not use the normal fallback
        // behavior here: if every candidate is rejected because Aimed Shot/Revive
        // Pet is already preparing, returning candidates.front() would re-cast the
        // same action and cancel the original cast.
        if (IsHunterCastTimeActionLocked(player))
            return {};

        for (PrioritizedSpellDecision const& candidate : candidates)
            if (IsDecisionImmediatelyCastable(player, candidate.decision, defaultEnemyTarget, defaultAllyTarget))
                return candidate.decision;

        // Preserve highest-priority fallback so execution can still drive movement/range correction.
        return candidates.front().decision;
    }

    bool IsDecisionImmediatelyCastable(Player const* player, SpellDecision const& decision, Unit const* defaultEnemyTarget, Unit const* defaultAllyTarget)
    {
        if (!player)
            return false;

    if (decision.itemEntry)
        return !playerbot::PvpCore::ShouldSeekLightwell(player) && IsOnUseItemReady(player, decision.itemEntry);

        if (!decision.spellId)
            return false;

        if (IsHunterCastTimeActionLocked(player))
            return false;

    bool const canUseRitualSoulwellEffect = player->GetClass() == CLASS_WARLOCK &&
        decision.spellId == kWarlockCreateSoulwellSpellId && player->HasSpell(kWarlockRitualOfSoulsSpellId);
    uint32 const knownPlayerSpellId = canUseRitualSoulwellEffect ? kWarlockCreateSoulwellSpellId :
        ResolveKnownPlayerSpellInChain(player, decision.spellId);
    bool const knownByPlayer = knownPlayerSpellId != 0;
    bool const knownByPet = IsPetSpellReady(player, decision.spellId);
    if (!knownByPlayer && !knownByPet)
        return false;

        if (knownByPlayer && player->GetSpellHistory()->HasCooldown(knownPlayerSpellId))
            return false;

        // Treat the shared playerbot dispel cooldown as an immediate castability
        // failure so the same decision pass suppresses this dispel and selects the
        // next available action instead of returning an idle cooldown attempt.
        if (IsPlayerbotDispelSpell(decision.spellId) &&
            !IsDispelThrottleExemptCast(player, decision.spellId) &&
            playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken))
            return false;

        if (decision.spellId == kDruidCasterFaerieFireSpellId &&
            playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kDruidCasterFaerieFireSpellId))
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(knownByPlayer ? knownPlayerSpellId : decision.spellId);
        if (!spellInfo)
            return false;

    // A spell whose equipped-item requirement isn't met (Shield Slam without
    // a shield) fails Spell::CheckItems on every attempt; treating it as
    // castable commits the decision pass to an impossible pick and starves
    // the whole rotation behind it.
    if (knownByPlayer && spellInfo->EquippedItemClass >= 0 && !player->HasItemFitToSpellRequirements(spellInfo))
        return false;

    // Same failure family for the combo-point mechanism: rogue finishers and
    // the warrior reactive strikes (Overpower rides on combo points granted
    // by the target's dodge) hard-fail with SPELL_FAILED_NO_COMBO_POINTS when
    // the live combo state lags the selector's own window tracking.
    if (knownByPlayer && spellInfo->NeedsComboPoints())
    {
        ObjectGuid const intendedTargetGuid = !decision.targetGuid.IsEmpty()
            ? decision.targetGuid
            : (defaultEnemyTarget ? defaultEnemyTarget->GetGUID() : ObjectGuid::Empty);
        if (intendedTargetGuid.IsEmpty() || !player->GetComboPoints(intendedTargetGuid))
            return false;
    }

    // Stance and form locks (Whirlwind demands Berserker Stance):
    // SPELL_FAILED_ONLY_SHAPESHIFT on every attempt. CheckShapeshift is the
    // core's own CheckCast rule, so a blocked pick could never land.
    if (knownByPlayer && spellInfo->CheckShapeshift(player->GetShapeshiftForm()) != SPELL_CAST_OK)
        return false;

    // Declarative requirements the core hard-enforces in CheckCast; a pick
    // that fails one of these fails it on EVERY attempt and starves the
    // rotation behind it (the Shield Slam / Overpower / Purge family).
    if (knownByPlayer && decision.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy)
    {
        Unit const* enemyTarget = !decision.targetGuid.IsEmpty()
            ? ObjectAccessor::GetUnit(*player, decision.targetGuid)
            : defaultEnemyTarget;

        // Offensive dispels (Purge, Spellsteal): SPELL_FAILED_NOTHING_TO_DISPEL
        // against a target carrying nothing of the matching dispel type - the
        // default state of every grind creature.
        //
        // Read the mask off the payload when the pick is a pure wrapper: the
        // shaman casts 81324/81325, whose own effect list is a single
        // TRIGGER_SPELL and so declares no dispel type at all. Without this the
        // guard finds an empty mask, skips itself, and the bot spends every
        // global purging a target with nothing to take.
        SpellInfo const* dispelSourceInfo = spellInfo;
        if (SpellInfo const* triggerPayload = GetPureTriggerPayload(spellInfo))
            dispelSourceInfo = triggerPayload;

        uint32 dispelMask = 0;
        for (SpellEffectInfo const& effect : dispelSourceInfo->GetEffects())
            if (effect.Effect == SPELL_EFFECT_DISPEL)
                dispelMask |= SpellInfo::GetDispelMask(DispelType(effect.MiscValue));

        if (dispelMask)
        {
            if (!enemyTarget)
                return false;

            DispelChargesList dispelList;
            enemyTarget->GetDispellableAuraList(player, dispelMask, dispelList);
            if (dispelList.empty())
                return false;
        }

        // Target aura state (Conflagrate's immolate, sub-20% execute range):
        // SPELL_FAILED_TARGET_AURASTATE.
        if (spellInfo->TargetAuraState &&
            (!enemyTarget || !enemyTarget->HasAuraState(AuraStateType(spellInfo->TargetAuraState), spellInfo, player)))
            return false;

        // Positional behind-target attribute (Backstab, Shred):
        // SPELL_FAILED_NOT_BEHIND.
        if (spellInfo->HasAttribute(SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET) &&
            (!enemyTarget || enemyTarget->HasInArc(static_cast<float>(M_PI), player)))
            return false;

        // Creature-type restrictions (Repentance only works on humanoids and
        // demons; a grind beast fails SPELL_FAILED_BAD_TARGETS forever).
        if (spellInfo->TargetCreatureType && enemyTarget)
        {
            uint32 const creatureTypeMask = enemyTarget->GetCreatureTypeMask();
            if (creatureTypeMask && !(creatureTypeMask & spellInfo->TargetCreatureType))
                return false;
        }

        if (spellInfo->HasAttribute(SPELL_ATTR3_ONLY_TARGET_PLAYERS) &&
            (!enemyTarget || enemyTarget->GetTypeId() != TYPEID_PLAYER))
            return false;
    }

    // Charge/leap-motion effects hard-fail while rooted
    // (SPELL_FAILED_ROOTED): a rooted warrior otherwise spams Intercept or a
    // priority-boosted Heroic Leap for the root's whole duration.
    if (knownByPlayer && player->HasUnitState(UNIT_STATE_ROOT))
    {
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            switch (effect.Effect)
            {
                case SPELL_EFFECT_CHARGE:
                case SPELL_EFFECT_CHARGE_DEST:
                case SPELL_EFFECT_LEAP:
                case SPELL_EFFECT_JUMP:
                case SPELL_EFFECT_JUMP_DEST:
                case SPELL_EFFECT_LEAP_BACK:
                    return false;
                default:
                    break;
            }
        }
    }

    // Health-cost spells (Bloodrage): Spell::CheckPower fails whenever
    // current health is at or below the cost, and the affordability check
    // below only covers real power types.
    if (knownByPlayer && int32(spellInfo->PowerType) == POWER_HEALTH &&
        player->GetHealth() <= uint32(std::max(0, spellInfo->CalcPowerCost(player, spellInfo->GetSchoolMask()))))
        return false;

    // Life Tap's script refuses the conversion when health is too low;
    // approximate the same floor so a dying warlock falls through to its
    // wand instead of fizzling the tap forever.
    if (knownByPlayer && spellInfo->GetFirstRankSpell()->Id == 1454 && player->GetHealthPct() < 20.0f)
        return false;

    // Pet-target spells (Soul Link) with no living pet: SPELL_FAILED_NO_PET.
    {
        Pet const* pet = player->GetPet();
        bool const hasLivingPet = pet && pet->IsAlive();
        if (knownByPlayer && !hasLivingPet)
            for (SpellEffectInfo const& effect : spellInfo->GetEffects())
                if (effect.TargetA.GetTarget() == TARGET_UNIT_PET || effect.TargetB.GetTarget() == TARGET_UNIT_PET)
                    return false;
    }

    // Reagents (warlock summons, soulstones): SPELL_FAILED_REAGENTS.
    if (knownByPlayer && !player->CanNoReagentCast(spellInfo))
        for (uint8 reagentIndex = 0; reagentIndex < MAX_SPELL_REAGENTS; ++reagentIndex)
            if (spellInfo->Reagent[reagentIndex] > 0 &&
                !player->HasItemCount(uint32(spellInfo->Reagent[reagentIndex]), std::max<uint32>(1, spellInfo->ReagentCount[reagentIndex])))
                return false;

    if (playerbot::PvpCore::ShouldSeekLightwell(player))
    {
        if (spellInfo->CalcCastTime() > 0 || spellInfo->IsChanneled() || spellInfo->IsAutoRepeatRangedSpell())
            return false;

        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            switch (effect.Effect)
            {
                case SPELL_EFFECT_TELEPORT_UNITS:
                case SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER:
                case SPELL_EFFECT_LEAP:
                case SPELL_EFFECT_JUMP:
                case SPELL_EFFECT_JUMP_DEST:
                case SPELL_EFFECT_LEAP_BACK:
                case SPELL_EFFECT_CHARGE:
                case SPELL_EFFECT_CHARGE_DEST:
                    return false;
                default:
                    break;
            }
        }
    }

    bool const breaksFlagCarry = playerbot::PvpCore::SpellWouldBreakFlagCarry(spellInfo->Id);
    if (breaksFlagCarry && playerbot::PvpCore::IsBattlegroundFlagCarrier(player))
        return false;

    // A shapeshift form (Moonkin, Ghost Wolf, Bear/Cat, ...) makes most other
    // spells uncastable. Without this check the selector marks something like
    // Lightning Shield as "castable" while the caster is a Ghost Wolf, attempts
    // it, and it fails server-side with SPELL_FAILED_NOT_SHAPESHIFT instead of
    // the loop moving on to a candidate that actually works.
    // Rehgar's Fury (82419) is exempted: its whole purpose (spell_sha_ghost_wolf_charge
    // / Unit::CompleteGhostWolfCharge) is to be cast while Ghost Wolf is active, but
    // if its spell_template Stances field was never authored to explicitly allow the
    // Ghost Wolf stance, this generic DBC-driven check would wrongly reject it too -
    // leaving nothing valid to do while shifted except immediately unshift again,
    // which reads as the shaman flickering in and out of Ghost Wolf and never leaping.
    if (knownByPlayer && decision.spellId != 82419 && player->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT) &&
        spellInfo->CheckShapeshift(player->GetShapeshiftForm()) != SPELL_CAST_OK)
        return false;

    // Classic hunter traps cannot be placed while the hunter is still in combat.
    // Feign Death may clear combat and make the next trap valid, but the selector
    // must not return an in-combat trap attempt that would spam
    // SPELL_FAILED_AFFECTING_COMBAT.
    if (player->GetClass() == CLASS_HUNTER && IsHunterTrapSpell(knownByPlayer ? knownPlayerSpellId : decision.spellId) && player->IsInCombat())
        return false;

        // Reactive spells like Revenge can be known and off cooldown while still
        // being unusable because the required caster aura state is not active yet.
        // Treat that as not immediately castable so the selector picks another
        // candidate instead of logging SPELL_FAILED_CASTER_AURASTATE every tick.
        if (knownByPlayer && !MeetsCasterAuraStateRequirements(player, knownPlayerSpellId))
            return false;

        Unit const* resolvedTarget = nullptr;
        switch (decision.targetMode)
        {
        case playerbot::PvpClassSpellContext::TargetMode::Self:
            resolvedTarget = player;
            break;
        case playerbot::PvpClassSpellContext::TargetMode::Enemy:
            resolvedTarget = defaultEnemyTarget;
            break;
        case playerbot::PvpClassSpellContext::TargetMode::Ally:
            resolvedTarget = defaultAllyTarget;
            break;
        case playerbot::PvpClassSpellContext::TargetMode::Pet:
            resolvedTarget = player->GetPet();
            break;
        case playerbot::PvpClassSpellContext::TargetMode::None:
        default:
            return false;
        }

    if (decision.targetMode != playerbot::PvpClassSpellContext::TargetMode::Pet && !decision.targetGuid.IsEmpty())
        if (Unit const* explicitTarget = ObjectAccessor::GetUnit(*player, decision.targetGuid))
            resolvedTarget = explicitTarget;

        if (!resolvedTarget || !resolvedTarget->IsAlive())
            return false;

    if (breaksFlagCarry)
        if (Player const* targetPlayer = resolvedTarget->ToPlayer())
            if (playerbot::PvpCore::IsBattlegroundFlagCarrier(targetPlayer))
                return false;

    if (decision.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy && !player->IsValidAttackTarget(resolvedTarget, spellInfo))
        return false;
    if (decision.targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally && !IsFriendlySupportTarget(player, resolvedTarget))
        return false;
    if (decision.targetMode == playerbot::PvpClassSpellContext::TargetMode::Pet && resolvedTarget != player->GetPet())
        return false;

    // A cast the target is wholly immune to is a wasted global cooldown, and the
    // caller takes the FIRST candidate that passes this function - so refusing
    // here is already "do something else instead", for every class at once, with
    // no per-class table to keep in step.
    //
    // Scoped to Enemy decisions because targetMode is the only polarity this
    // function has, and it is load-bearing twice: it keeps heals, shields and
    // self-buffs out, several of which carry a damage school (Power Word: Shield
    // is Holy, Lightning Shield is Nature), and it neutralises the explicit-guid
    // override above, which can hand a Self decision an enemy's guid.
    //
    // Placed before the line-of-sight raycast, which is the most expensive call
    // left in the function.
    if (decision.targetMode == playerbot::PvpClassSpellContext::TargetMode::Enemy &&
        playerbot::PvpClassActions::IsCastWastedOnTargetImmunity(
            (knownByPet && !knownByPlayer) ? static_cast<Unit const*>(player->GetPet())
                                           : static_cast<Unit const*>(player),
            resolvedTarget, spellInfo))
        return false;

        if (!player->IsWithinLOSInMap(resolvedTarget))
            return false;

        float const maxRange = spellInfo->GetMaxRange(false);
        if (maxRange > 0.0f && !player->IsWithinDistInMap(resolvedTarget, maxRange))
            return false;

        float const minRange = spellInfo->GetMinRange(false);
        if (minRange > 0.0f && player->IsWithinDistInMap(resolvedTarget, minRange))
            return false;

        // Skip decisions we cannot currently pay for so the fallback chain can
        // choose a castable alternative (wand, movement, etc.) instead of
        // repeatedly selecting an OOM support spell.
        Unit const* powerCaster = knownByPet ? static_cast<Unit const*>(player->GetPet()) : static_cast<Unit const*>(player);
        if (powerCaster && !IsSpiritOfRedemptionFreeHeal(player, decision.spellId) && spellInfo->PowerType >= 0 && spellInfo->PowerType < MAX_POWERS)
        {
            Powers const powerType = Powers(spellInfo->PowerType);
            int32 const powerCost = spellInfo->CalcPowerCost(powerCaster, spellInfo->GetSchoolMask());
            if (powerCost > 0 && powerCaster->GetPower(powerType) < powerCost)
                return false;
        }

        return true;
    }

constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT = 29073;
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK = 22734;
constexpr uint32 SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT = 22328;
constexpr uint32 kEnvironmentalMagmaDamageAuraId = 57634;

// Bots must never sit down to eat/drink while standing in lava/slime, and
// should not select eat/drink while actually submerged in water either --
// mirrors the hazard check used for movement escape in
// PlayerbotPvpLifecycleActions::IsInHazardousLiquid.
bool IsInHazardousLiquidForRecovery(Player const* player)
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

struct TacticalDecision
{
    char const* triggerName = nullptr;
    char const* actionName = nullptr;
    float priority = 0.0f;
};

    enum class ClassicClassProfile : uint8
    {
        UnknownClassic = 0,
        PrimaryClassic,
        SecondaryClassic,
        TertiaryClassic
    };

    struct ClassicProfileSelection
    {
        ClassicClassProfile profile = ClassicClassProfile::UnknownClassic;
        char const* profileLabel = "UnknownClassic";
        bool usedFallback = true;
        bool unsupportedClass = false;
    };

    enum class HunterPvpSpec : uint8
    {
        BeastMastery = 0,
        Marksmanship,
        Survival
    };

    HunterPvpSpec GetHunterPvpSpec(ClassicProfileSelection const& profileSelection)
    {
        switch (profileSelection.profile)
        {
        case ClassicClassProfile::PrimaryClassic:
            return HunterPvpSpec::BeastMastery;
        case ClassicClassProfile::SecondaryClassic:
            return HunterPvpSpec::Marksmanship;
        case ClassicClassProfile::TertiaryClassic:
            return HunterPvpSpec::Survival;
        case ClassicClassProfile::UnknownClassic:
        default:
            return HunterPvpSpec::Marksmanship;
        }
    }

    // One capstone talent per tree, per class. These identify a spec cheaply,
    // but ONLY once the bot is deep enough to have taken that exact talent.
    struct ClassicProfileMarkers
    {
        uint8 classId;
        uint32 primarySpell;
        char const* primaryLabel;
        uint32 secondarySpell;
        char const* secondaryLabel;
        uint32 tertiarySpell;
        char const* tertiaryLabel;
    };

    constexpr ClassicProfileMarkers kClassicProfileMarkers[] = {
        { CLASS_WARRIOR, 12294, "Arms-like",          81273, "Fury-like",        23922, "Prot-like"        },
        { CLASS_PALADIN, 20473, "Holy-like",          20925, "Prot-like",        20375, "Ret-like"         },
        { CLASS_HUNTER,  81300, "BM-like",            19506, "MM-like",          19386, "SV-like"          },
        { CLASS_ROGUE,   81302, "Assassination-like", 13750, "Combat-like",      14185, "Subtlety-like"    },
        { CLASS_PRIEST,  10060, "Discipline-like",    724,   "Holy-like",        15473, "Shadow-like"      },
        { CLASS_SHAMAN,  16166, "Elemental-like",     17364, "Enhancement-like", 16188, "Restoration-like" },
        { CLASS_MAGE,    12042, "Arcane-like",        33041, "Fire-like",        11426, "Frost-like"       },
        { CLASS_WARLOCK, 48181, "Affliction-like",    19028, "Demonology-like",  17962, "Destruction-like" },
        // Druid is deliberately NOT in DBC tab order here: Restoration is the
        // secondary profile and Feral the tertiary. Anything deriving a tree
        // order generically must go through this table, not through the DBC.
        { CLASS_DRUID,   24858, "Balance-like",       18562, "Restoration-like", 17007, "Feral-like"       },
    };

    ClassicProfileMarkers const* FindClassicProfileMarkers(uint8 classId)
    {
        for (ClassicProfileMarkers const& markers : kClassicProfileMarkers)
            if (markers.classId == classId)
                return &markers;

        return nullptr;
    }

    // When no capstone matches, ask the talent spend which tree this is.
    //
    // A level 50 mage seventeen talents deep into fire, holding Improved
    // Scorch, that had simply not taken the ONE fire talent the table looks
    // for, matched nothing and fell out as the default - and the default is
    // read as frost. It opened every fight with Frostbolt while specced fire.
    // Counting the points is the honest answer and costs one pass over the
    // talent store, only for bots no capstone identified.
    ClassicProfileSelection DetectProfileByTalentSpend(Player const* player, ClassicProfileMarkers const& markers)
    {
        uint8 const activeSpec = player->GetActiveSpec();
        uint32 const markerSpells[3] = { markers.primarySpell, markers.secondarySpell, markers.tertiarySpell };
        uint32 markerTabs[3] = { 0, 0, 0 };
        std::unordered_map<uint32, uint32> pointsByTab;

        for (TalentEntry const* talent : sTalentStore)
        {
            if (!talent)
                continue;

            uint32 highestKnownRank = 0;
            for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
            {
                uint32 const rankSpell = talent->SpellRank[rank];
                if (!rankSpell)
                    continue;

                for (uint32 marker = 0; marker < 3; ++marker)
                    if (markerSpells[marker] && rankSpell == markerSpells[marker])
                        markerTabs[marker] = talent->TabID;

                // Ranks are cumulative: the highest one known IS the spend.
                if (player->HasTalent(rankSpell, activeSpec))
                    highestKnownRank = uint32(rank) + 1;
            }

            if (highestKnownRank)
                pointsByTab[talent->TabID] += highestKnownRank;
        }

        uint32 bestTab = 0;
        uint32 bestPoints = 0;
        for (auto const& [tabId, points] : pointsByTab)
        {
            if (points > bestPoints)
            {
                bestPoints = points;
                bestTab = tabId;
            }
        }

        // No talents spent at all, or the winning tree is not one of this
        // class's three - either way there is nothing honest to say.
        if (!bestPoints)
            return {};

        if (bestTab == markerTabs[0])
            return { ClassicClassProfile::PrimaryClassic, markers.primaryLabel, false, false };
        if (bestTab == markerTabs[1])
            return { ClassicClassProfile::SecondaryClassic, markers.secondaryLabel, false, false };
        if (bestTab == markerTabs[2])
            return { ClassicClassProfile::TertiaryClassic, markers.tertiaryLabel, false, false };

        return {};
    }

    ClassicProfileSelection DetectClassicClassProfile(Player const* player)
    {
        ClassicProfileSelection selection;
        if (!player)
            return selection;

        uint8 const activeSpec = player->GetActiveSpec();

        if (ClassicProfileMarkers const* markers = FindClassicProfileMarkers(player->GetClass()))
        {
            if (player->HasTalent(markers->primarySpell, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, markers->primaryLabel, false, false };
            if (player->HasTalent(markers->secondarySpell, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, markers->secondaryLabel, false, false };
            if (player->HasTalent(markers->tertiarySpell, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, markers->tertiaryLabel, false, false };

            if (ClassicProfileSelection const spend = DetectProfileByTalentSpend(player, *markers);
                spend.profile != ClassicClassProfile::UnknownClassic)
                return spend;
        }

        switch (player->GetClass())
        {
        case CLASS_WARRIOR:
            if (player->HasTalent(12294, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Arms-like", false, false };
            if (player->HasTalent(81273, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Fury-like", false, false };
            if (player->HasTalent(23922, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Prot-like", false, false };
            break;
        case CLASS_PALADIN:
            if (player->HasTalent(20473, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Holy-like", false, false };
            if (player->HasTalent(20925, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Prot-like", false, false };
            if (player->HasTalent(20375, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Ret-like", false, false };
            break;
        case CLASS_HUNTER:
            if (player->HasTalent(81300, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "BM-like", false, false };
            if (player->HasTalent(19506, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "MM-like", false, false };
            if (player->HasTalent(19386, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "SV-like", false, false };
            break;
        case CLASS_ROGUE:
            if (player->HasTalent(81302, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Assassination-like", false, false };
            if (player->HasTalent(13750, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Combat-like", false, false };
            if (player->HasTalent(14185, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Subtlety-like", false, false };
            break;
        case CLASS_PRIEST:
            if (player->HasTalent(10060, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Discipline-like", false, false };
            if (player->HasTalent(724, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Holy-like", false, false };
            if (player->HasTalent(15473, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Shadow-like", false, false };
            break;
        case CLASS_SHAMAN:
            if (player->HasTalent(16166, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Elemental-like", false, false };
            if (player->HasTalent(17364, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Enhancement-like", false, false };
            if (player->HasTalent(16188, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Restoration-like", false, false };
            break;
        case CLASS_MAGE:
            if (player->HasTalent(12042, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Arcane-like", false, false };
            if (player->HasTalent(33041, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Fire-like", false, false };
            if (player->HasTalent(11426, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Frost-like", false, false };
            break;
        case CLASS_WARLOCK:
            if (player->HasTalent(48181, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Affliction-like", false, false };
            if (player->HasTalent(19028, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Demonology-like", false, false };
            if (player->HasTalent(17962, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Destruction-like", false, false };
            break;
        case CLASS_DRUID:
            if (player->HasTalent(24858, activeSpec))
                return { ClassicClassProfile::PrimaryClassic, "Balance-like", false, false };
            if (player->HasTalent(18562, activeSpec))
                return { ClassicClassProfile::SecondaryClassic, "Restoration-like", false, false };
            if (player->HasTalent(17007, activeSpec))
                return { ClassicClassProfile::TertiaryClassic, "Feral-like", false, false };
            break;
        case CLASS_DEATH_KNIGHT:
            return { ClassicClassProfile::UnknownClassic, "UnsupportedClassicClass", true, true };
        default:
            break;
        }

        return selection;
    }

// The shared dispel throttle exists so a bot cannot burn every global casting
// into a protected or undispellable aura. Enhancement is the one spec whose
// whole job in a fight is to purge, so its Purge is let through: it neither
// waits on the throttle nor arms it, since arming it on the first cast is the
// same as waiting on it for the second.
//
// Scoped to Purge on purpose. A shaman's OTHER dispels (Cure Poison, Cure
// Disease) still answer to the throttle like everyone else's, and because the
// exemption is asked per spell rather than per bot, one of those arming the
// token never locks Purge out.
bool IsDispelThrottleExemptCast(Player const* player, uint32 spellId)
{
    // Cheap gates first: spec detection can fall through to a full pass over
    // the talent store, and this is asked from the castability check.
    if (!player || player->GetClass() != CLASS_SHAMAN || !IsPurgeFamilySpell(spellId))
        return false;

    return DetectClassicClassProfile(player).profile == ClassicClassProfile::SecondaryClassic;
}

bool PartyBenefitsFromWindfuryTotem(Player const* player)
{
    Group const* group = player ? player->GetGroup() : nullptr;
    if (!group)
        return false;

    for (GroupReference const* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player const* member = itr->GetSource();
        if (!member || member == player)
            continue;

        ClassicProfileSelection const memberProfile = DetectClassicClassProfile(member);
        if (member->GetClass() == CLASS_WARRIOR &&
            (memberProfile.profile == ClassicClassProfile::PrimaryClassic ||
             memberProfile.profile == ClassicClassProfile::SecondaryClassic))
            return true;

        if (member->GetClass() == CLASS_PALADIN && memberProfile.profile == ClassicClassProfile::TertiaryClassic)
            return true;
    }

    return false;
}

uint32 ResolveKnownPlayerSpellInChain(Player const* player, uint32 spellId)
{
    if (!player || !spellId)
        return 0;

        SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!baseSpellInfo)
            return 0;

        // Walk DOWN from the top rank and stop at the first known one. The old
        // loop started at rank 1, never broke, and reached the next rank through
        // sSpellMgr->GetNextSpellInChain - an mSpellChains hash lookup per rank
        // on top of the HasSpell lookup, so a sixteen rank chain like Frostbolt
        // cost thirty-two hash lookups to answer a question the first hit
        // settles. This walks pointers instead and typically stops after one or
        // two. "Last known scanning up" and "first known scanning down" are the
        // same spell whether or not the known ranks are contiguous.
        //
        // Deliberately NOT cached: this is also called from
        // IsDecisionImmediatelyCastable, CanAffordSpellPowerCost and
        // MeetsCasterAuraStateRequirements, none of which go through
        // BuildClassSpellContext, so anything keyed on less than the player
        // would leak one bot's spellbook into another's decision.
        for (SpellInfo const* rank = baseSpellInfo->ChainEntry ? baseSpellInfo->ChainEntry->last : baseSpellInfo;
            rank; rank = rank->ChainEntry ? rank->ChainEntry->prev : nullptr)
        {
            if (player->HasSpell(rank->Id))
                return rank->Id;
        }

        return 0;
    }

    bool IsSpellReady(Player const* player, uint32 spellId)
    {
        uint32 const resolvedSpellId = ResolveKnownPlayerSpellInChain(player, spellId);
        if (!resolvedSpellId)
            return false;

        return !player->GetSpellHistory()->HasCooldown(resolvedSpellId);
    }

    // The demon a warlock can summon before its spec's own is trained:
    // Voidwalker (10), else the Imp (1).
    uint32 SelectWarlockFallbackSummonSpell(Player const* player)
    {
        if (ResolveKnownPlayerSpellInChain(player, 697))
            return 697;
        return ResolveKnownPlayerSpellInChain(player, 688) ? 688 : 0;
    }

    // The demon the warlock's spec runs with, else the best one trained so
    // far - the same pick SelectWarlockSpell makes.
    uint32 SelectWarlockSpecSummonSpell(Player const* player)
    {
        ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
        uint32 const specPetSpell = profileSelection.profile == ClassicClassProfile::PrimaryClassic ? 691 :
            (profileSelection.profile == ClassicClassProfile::TertiaryClassic ? 712 : 697);
        return ResolveKnownPlayerSpellInChain(player, specPetSpell) ? specPetSpell : SelectWarlockFallbackSummonSpell(player);
    }

    // Shadow Bolt lives in two unrelated rank chains: the stock one (686 up
    // to 25307) and the T2 wrapper (90420 up to 90429). Warlocks hold only the
    // wrapper now (WrappedSpells.h); the stock chain is still read so a
    // character the startup move missed keeps its Shadow Bolt.
    uint32 SelectWarlockShadowBoltSpell(Player const* player)
    {
        auto knownRank = [player](uint32 chainSpellId) -> uint8
        {
            uint32 const known = ResolveKnownPlayerSpellInChain(player, chainSpellId);
            SpellInfo const* info = known ? sSpellMgr->GetSpellInfo(known) : nullptr;
            return info ? info->GetRank() : 0;
        };
        return knownRank(90429) >= knownRank(25307) && knownRank(90429) ? 90429 : 25307;
    }

// IsSpellReady only answers "off cooldown". Affordability is enforced later in
// IsDecisionImmediatelyCastable, which is fine for picking an action but not for
// gating conditions that feed *other* decisions - a gap closer that reads as
// "ready" while unaffordable can suppress the very rage generation needed to pay
// for it, and the bot then has no way out of that state.
bool CanAffordSpellPowerCost(Player const* player, uint32 spellId)
{
    uint32 const resolvedSpellId = ResolveKnownPlayerSpellInChain(player, spellId);
    if (!player || !resolvedSpellId)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(resolvedSpellId);
    if (!spellInfo)
        return false;

    if (spellInfo->PowerType < 0 || spellInfo->PowerType >= MAX_POWERS)
        return true;

    int32 const powerCost = spellInfo->CalcPowerCost(player, spellInfo->GetSchoolMask());
    return powerCost <= 0 || player->GetPower(Powers(spellInfo->PowerType)) >= powerCost;
}

bool IsZeroManaCostSpellReady(Player const* player, uint32 spellId)
{
    uint32 const resolvedSpellId = ResolveKnownPlayerSpellInChain(player, spellId);
    if (!player || !resolvedSpellId || player->GetSpellHistory()->HasCooldown(resolvedSpellId))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(resolvedSpellId);
    if (!spellInfo || spellInfo->PowerType != POWER_MANA)
        return false;

    return spellInfo->CalcPowerCost(player, spellInfo->GetSchoolMask()) == 0;
}

bool MeetsCasterAuraStateRequirements(Player const* player, uint32 spellId)
{
    uint32 const resolvedSpellId = ResolveKnownPlayerSpellInChain(player, spellId);
    if (!player || !resolvedSpellId)
        return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(resolvedSpellId);
        if (!spellInfo)
            return false;

        if (spellInfo->CasterAuraState && !player->HasAuraState(AuraStateType(spellInfo->CasterAuraState), spellInfo, player))
            return false;

        if (spellInfo->CasterAuraStateNot && player->HasAuraState(AuraStateType(spellInfo->CasterAuraStateNot), spellInfo, player))
            return false;

        if (spellInfo->CasterAuraSpell && !player->HasAura(sSpellMgr->GetSpellIdForDifficulty(spellInfo->CasterAuraSpell, player)))
            return false;

        if (spellInfo->ExcludeCasterAuraSpell && player->HasAura(sSpellMgr->GetSpellIdForDifficulty(spellInfo->ExcludeCasterAuraSpell, player)))
            return false;

        return true;
    }

    bool IsSpellReadyAndCasterAuraAllowed(Player const* player, uint32 spellId)
    {
        return IsSpellReady(player, spellId) && MeetsCasterAuraStateRequirements(player, spellId);
    }

    bool IsPetSpellReady(Player const* player, uint32 spellId)
    {
        if (!player || !spellId)
            return false;

        Pet const* pet = player->GetPet();
        if (!pet || !pet->IsAlive())
            return false;

        SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!baseSpellInfo)
            return false;

        uint32 resolvedSpellId = 0;
        for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
            if (pet->HasSpell(chainSpellId))
                resolvedSpellId = chainSpellId;

        if (!resolvedSpellId)
            return false;

        return !pet->GetSpellHistory()->HasCooldown(resolvedSpellId);
    }

    bool IsDruidCasterForm(Player const* player)
    {
        return player && player->GetClass() == CLASS_DRUID && player->GetShapeshiftForm() == FORM_NONE;
    }

    float GetPlayerbotTotemRefreshDistance(Creature const* totem)
    {
        if (!totem || !totem->IsTotem())
            return kPlayerbotTotemRefreshDistance;

        float maxRadius = 0.0f;
        Totem const* totemUnit = totem->ToTotem();
        for (uint8 spellSlot = 0; spellSlot < MAX_CREATURE_SPELLS; ++spellSlot)
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(totemUnit->GetSpell(spellSlot));
            if (!spellInfo)
                continue;

            for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
                if (spellEffectInfo.IsEffect() && spellEffectInfo.HasRadius())
                    maxRadius = std::max(maxRadius, spellEffectInfo.CalcRadius(const_cast<Creature*>(totem)));
        }

        return maxRadius > 0.0f ? maxRadius : kPlayerbotTotemRefreshDistance;
    }

    bool HasActiveTotemInSlot(Player const* player, uint8 slot)
    {
        if (!player || slot >= MAX_SUMMON_SLOT)
            return false;

        ObjectGuid const& summonGuid = player->m_SummonSlot[slot];
        if (!summonGuid)
            return false;

        Creature* totem = ObjectAccessor::GetCreature(*player, summonGuid);
        if (!totem || !totem->IsAlive())
            return false;

        return player->IsWithinDistInMap(totem, GetPlayerbotTotemRefreshDistance(totem));
    }

    bool HasActiveEarthTotem(Player const* player)
    {
        return HasActiveTotemInSlot(player, SUMMON_SLOT_TOTEM_EARTH);
    }

    bool HasActiveWaterTotem(Player const* player)
    {
        return HasActiveTotemInSlot(player, SUMMON_SLOT_TOTEM_WATER);
    }

    bool HasActiveAirTotem(Player const* player)
    {
        return HasActiveTotemInSlot(player, SUMMON_SLOT_TOTEM_AIR);
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
    // Mounting must be conservative around WMO boundaries. A stale cached
    // outdoor bit must not override fresh terrain/WMO indoor classification (or
    // vice versa), otherwise bots mount inside bases and tunnels.
    return player->IsOutdoors() && terrainStatus.outdoors;
}

    bool IsStrictlyOutdoorsForMount(Player const* player)
    {
        if (!player)
            return false;

        // Keep mount checks aligned with reference behavior: require IsOutdoors and
        // reject cases where the unit is clipping slightly below floor level (which
        // can misreport outdoor state in battleground tunnels/bases).
        if (!player->IsOutdoors())
            return false;

        float const posZ = player->GetPositionZ();
        float const groundLevel = player->GetMapWaterOrGroundLevel(player->GetPositionX(), player->GetPositionY(), posZ);
        if (!player->HasAuraType(SPELL_AURA_WATER_WALK) && posZ < groundLevel)
            return false;

        return true;
    }

    bool ShouldForceIndoorDismount(Player const* player, bool outdoors, uint32 lingerMs = 1500)
    {
        if (!player)
            return false;

        static std::unordered_map<uint64, uint32> indoorSinceMsByGuid;
        uint64 const guid = player->GetGUID().GetRawValue();

        // One map, every managed bot, four map-update threads: the erase below and the
        // emplace further down are structural mutations, so without this the bucket
        // array can be reallocated out from under a sibling thread mid-lookup.
        //
        // The identical function in PlayerbotPvpLifecycleActions.cpp:459 has always
        // taken this lock. This copy did not, which is an oversight rather than a
        // deliberate exemption - there is nothing different about the state it keeps.
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

    bool HasNearbyAttackableEnemyPlayer(Player const* player, float maxDistance)
    {
        if (!player || !player->IsInWorld())
            return false;

        Map const* map = player->FindMap();
        if (!map)
            return false;

        float const checkDistance = std::max(maxDistance, 0.0f);
        for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!candidate || candidate == player || !candidate->IsAlive())
                continue;
            if (!player->IsWithinDistInMap(candidate, checkDistance))
                continue;
            if (!player->IsValidAttackTarget(candidate))
                continue;
            return true;
        }

        return false;
    }

    // Both Warsong Gulch keeps are areas of their own under the gulch, flag
    // rooms included, and every group of both keep models resolves to one of
    // them. The outdoors flag cannot answer "am I inside the base": WMOAreaTable
    // forces indoors on only part of each keep (the flag room among them), and
    // the groups it leaves alone carry the WMO outdoor bit - so the ramp down
    // and the tunnel read as open sky. Spell::CheckCast lets a druid shift into
    // Travel Form standing in there, and Player::CheckAreaExploreAndOutdoor
    // never takes it off again, because both read that same flag. To everyone
    // else in the match it is a cheetah trotting around inside a building.
    bool IsInsideWarsongKeep(Player const* player)
    {
        if (!player)
            return false;

        switch (player->GetAreaId())
        {
            case 3320: // Warsong Lumber Mill
            case 3321: // Silverwing Hold
            case 4571: // Silverwing Flag Room
            case 4572: // Warsong Flag Room
                return true;
            default:
                return false;
        }
    }

    // Where a form that belongs under open sky may be worn. Stricter than the
    // core's own outdoors test by exactly the keeps above.
    bool IsUnderOpenSkyForDruidForm(Player const* player)
    {
        return player && player->IsOutdoors() && !IsInsideWarsongKeep(player);
    }

    bool CanAttemptMount(Player const* player, SpellInfo const* mountSpellInfo)
    {
        if (!player || !mountSpellInfo)
            return false;

        uint32 zoneId = 0;
        uint32 areaId = 0;
        player->GetZoneAndAreaId(zoneId, areaId);
        if (mountSpellInfo->CheckLocation(player->GetMapId(), zoneId, areaId, player, false) != SPELL_CAST_OK)
            return false;

        Map const* map = player->FindMap();
        if (!map)
            return false;

        bool allowMount = !map->IsDungeon() || map->IsBattlegroundOrArena();
        if (InstanceTemplate const* instanceTemplate = sObjectMgr->GetInstanceTemplate(player->GetMapId()))
            allowMount = instanceTemplate->AllowMount;

        return allowMount || mountSpellInfo->AreaGroupId;
    }

    bool CanCastMountSpellAtCurrentLocation(Player const* player, SpellInfo const* mountSpellInfo)
    {
        if (!CanAttemptMount(player, mountSpellInfo))
            return false;

    // A real client refuses to summon a mount while swimming. Bots have no
    // such client-side gate, so without this check they could cast a ground
    // mount while already standing in water and then never get dismounted
    // until their next relocation tick re-evaluates terrain status.
    if (player->IsInWater())
        return false;

    // Playerbot mount selection must honor current terrain/WMO classification
    // even for custom mount spells that omitted the normal outdoors-only spell
    // attribute. Otherwise the AI repeatedly chooses those spells in indoor
    // battleground bases although a normal player cannot mount there.
    return IsEffectivelyOutdoors(player);
}

    bool IsHardControlled(Player const* player)
    {
        if (!player)
            return false;

        return player->HasUnitState(UNIT_STATE_STUNNED) ||
            player->HasUnitState(UNIT_STATE_CONFUSED) ||
            player->HasUnitState(UNIT_STATE_FLEEING) ||
            player->HasUnitState(UNIT_STATE_ROOT);
    }

    uint32 SelectReadyKnownMountSpell(Player const* player)
    {
        if (!player)
            return 0;

        for (PlayerSpellMap::value_type const& knownSpellPair : player->GetSpellMap())
        {
            uint32 const spellId = knownSpellPair.first;
            PlayerSpell const& knownSpell = knownSpellPair.second;
            if (!knownSpell.active || knownSpell.disabled)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || spellInfo->IsPassive())
                continue;
            if (!spellInfo->HasAura(SPELL_AURA_MOUNTED) && spellInfo->Mechanic != MECHANIC_MOUNT)
                continue;
            if (!IsSpellReady(player, spellId))
                continue;
            if (!CanCastMountSpellAtCurrentLocation(player, spellInfo))
                continue;

            // Fast (100%) mounts are a level-60 privilege under classic rules.
            if (player->GetLevel() < 60)
            {
                bool fastMount = false;
                for (SpellEffectInfo const& effect : spellInfo->GetEffects())
                    if (effect.IsAura(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED) && effect.CalcValue() > 60)
                        fastMount = true;
                if (fastMount)
                    continue;
            }

            return spellId;
        }

        return 0;
    }

    SpellDecision SelectMountSpell(Player const* player, char const* reason)
    {
        SpellDecision decision;
        if (!player || !player->IsAlive() || player->IsInCombat() || player->IsMounted())
            return decision;

        // Classic rules: no mounts of any kind before level 40.
        if (player->GetLevel() < 40)
            return decision;

        if (IsHardControlled(player))
            return decision;

        // The free bot mount is a 100% mount: level 60 only.
        if (player->GetLevel() >= 60 && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT))
            if (SpellInfo const* defaultMountInfo = sSpellMgr->GetSpellInfo(SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT))
                if (CanCastMountSpellAtCurrentLocation(player, defaultMountInfo))
                    return { "mount", reason, SPELL_PLAYERBOT_OUT_OF_COMBAT_MOUNT, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

        if (uint32 const knownMountSpellId = SelectReadyKnownMountSpell(player))
            return { "mount", reason, knownMountSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

        return decision;
    }

    SpellDecision SelectOutOfCombatEatDrinkOrMountSpell(Player const* player)
    {
        SpellDecision decision;
        if (!player || !player->IsAlive() || player->IsInCombat() || player->IsMounted())
            return decision;

        // Spirit of Redemption makes priest healing free and time-limited. Do not
        // waste that window by selecting drink/eat/mount recovery.
        if (IsPriestInSpiritOfRedemption(player))
            return decision;

        // Do not attempt recovery/mount actions while hard controlled. This avoids
        // mount selections during fear/polymorph/stun/root states.
        if (IsHardControlled(player))
            return decision;

    // Never sit down to eat/drink while standing in lava/slime, or while
    // in water at all -- a real player cannot use food/drink items while
    // swimming (not just while fully submerged), and bots have no
    // client-side check to enforce this on their own.
    if (IsInHazardousLiquidForRecovery(player) || player->IsInWater())
        return decision;

    // Nobody sits down while the team is in a fight - however low on mana,
    // however sticky a drink already started (EvaluateTeamDanger). Mounting
    // waits too: a teammate under attack nearby is somewhere to run, not ride.
    if (EvaluateTeamDanger(player, false).inDanger)
        return decision;

    // A map that refused the free eat/drink (PvpClassActions::IsRefreshmentRefused)
    // offers neither: the bot keeps walking instead of stopping to sit down again.
    // In an arena a bot sits down for a real loss only, not a few points: every
    // sit is a moment it is slow to answer the next fight.
    bool const refreshmentRefused = playerbot::PvpClassActions::IsRefreshmentRefused(player);
    bool const inArena = player->InArena();
    bool const usesMana = player->GetMaxPower(POWER_MANA) > 0;
    float const manaPct = usesMana ? player->GetPowerPct(POWER_MANA) : 100.0f;
    bool const needsDrink = !refreshmentRefused && usesMana && manaPct < (inArena ? 60.0f : 100.0f);
    bool const keepDrinkingFloor = usesMana && manaPct < 50.0f;
    bool const urgentlyNeedsDrink = needsDrink && (player->GetPowerPct(POWER_MANA) < 35.0f || IsLowOrOutOfManaForFallback(player));
    bool const needsFood = !refreshmentRefused && player->GetHealthPct() < (inArena ? 70.0f : 100.0f);
    bool const hasEatAura = player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
    bool const hasDrinkAura = player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);

        // Keep a single nearby-enemy boundary for "switch to combat posture".
        // Inside this range we should avoid out-of-combat utility behaviors
        // (eat/drink/mount), so movement + combat targeting can take over cleanly.
        //
        // Exception: when mana is critically low and the bot is already out of
        // combat, allow drink selection so they can recover instead of idling in a
        // perpetual "combat posture" loop.
        float const nearbyHostileCombatBoundary = std::max(GetConfiguredLongRange(), 35.0f);
        if (player->FindMap())
        {
            Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
            for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            {
                Player* candidate = itr->GetSource();
                if (!HasHostileTarget(player, candidate))
                    continue;
                if (!player->IsWithinDistInMap(candidate, nearbyHostileCombatBoundary) || !player->IsWithinLOSInMap(candidate))
                    continue;

                // If the bot is already drinking, keep that decision sticky until
                // we reach the configured recovery floor instead of breaking to
                // combat posture after only a small mana tick.
                bool const shouldMaintainDrink = hasDrinkAura && keepDrinkingFloor;
                if (!urgentlyNeedsDrink && !shouldMaintainDrink)
                    return decision;

                break;
            }
        }

        // If already drinking, never decide away from drinking until at least 50%
        // mana has been recovered.
        if (hasDrinkAura && keepDrinkingFloor)
        {
            if (!hasEatAura && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
                return { "eat", "pair food with active drink", SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT, playerbot::PvpClassSpellContext::TargetMode::Self };
            return { "drink", "maintain drink until 50% mana", SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK, playerbot::PvpClassSpellContext::TargetMode::Self };
        }

        // Recovery auras should naturally break on movement and should not linger
        // once the corresponding resource has fully recovered.
        if (Player* mutablePlayer = const_cast<Player*>(player))
        {
            if (hasEatAura && (mutablePlayer->isMoving() || (!needsFood && !needsDrink)))
                mutablePlayer->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
            if (hasDrinkAura && (mutablePlayer->isMoving() || !needsDrink))
                mutablePlayer->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);
        }

        // When both health and mana are missing, mirror real player behavior:
        // apply both food and drink so recovery happens in parallel.
        if (needsFood && needsDrink)
        {
            if (!hasEatAura && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
                return { "eat", "recover health out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT, playerbot::PvpClassSpellContext::TargetMode::Self };

            if (!hasDrinkAura && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
                return { "drink", "recover mana out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK, playerbot::PvpClassSpellContext::TargetMode::Self };
        }

        if (needsFood && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            return { "eat", "recover health out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT, playerbot::PvpClassSpellContext::TargetMode::Self };

        if (needsDrink)
        {
            if (!hasEatAura && IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
                return { "eat", "pair food with mana recovery", SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT, playerbot::PvpClassSpellContext::TargetMode::Self };

            if (IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
                return { "drink", "recover mana out of combat", SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK, playerbot::PvpClassSpellContext::TargetMode::Self };
        }

        bool const inBattlegroundPreparation = player->InBattleground() &&
            (player->HasAura(SPELL_PREPARATION) || player->HasAura(SPELL_ARENA_PREPARATION) || player->HasUnitFlag(UNIT_FLAG_PREPARATION));
        bool const inActiveBattleground = player->InBattleground() && !inBattlegroundPreparation;
        bool const inArenaMap = player->InArena();
        bool const inActiveDuel = player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS;

        // Keep arena and duel behavior conservative; battleground out-of-combat
        // mounting is intentionally allowed for parity with reference bots.
        if (inArenaMap || inActiveDuel)
            return decision;

        // During battleground preparation this selector only runs after
        // SelectPreparationBuffSpell() has no pet/buff actions left. At that point,
        // mount if the current spot is legal for the selected mount spell so bots
        // are ready to move as soon as the gates open.
        char const* mountReason = inBattlegroundPreparation ? "mount during preparation after prep actions" : "mount while out of combat";

    // Keep pressure logic responsive outside the prep phase: don't choose an
    // out-of-combat mount action while hostile players are already within
    // practical engage range.
    if (!inBattlegroundPreparation && HasNearbyAttackableEnemyPlayer(player, playerbot::PLAYERBOT_MOUNT_ENEMY_AWARENESS_RANGE))
        return decision;

        // A warlock summons its demon before it rides. The class pass only
        // summons with an enemy in sight, so one that lost its pet rode off
        // without it and then got off in front of the enemy to stand through
        // a ten-second summon. A pet the mount parked is not lost.
        if (player->GetClass() == CLASS_WARLOCK)
        {
            Pet const* pet = player->GetPet();
            if ((!pet || !pet->IsAlive()) && !IsPetParkedByMount(player))
                if (uint32 const summonSpellId = SelectWarlockSpecSummonSpell(player); summonSpellId && IsSpellReady(player, summonSpellId))
                    return { "warlock summon demon before mounting", "summon the demon before riding off without it", summonSpellId,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };
        }

        return SelectMountSpell(player, mountReason);
    }

    bool HasHostileTarget(Player const* player, Unit const* target)
    {
        if (!player || !target || target == player || !target->IsAlive() ||
            !player->IsValidAttackTarget(target))
            return false;

        // Legal to fight is not the same as free to start.
        //
        // Roughly sixty places in this file ask this question, and they split
        // two ways: map-wide scans looking for somebody to open on, and checks
        // about a target the bot already has. Gating the scans one by one would
        // be thirty edits and thirty chances to gate a retaliation path by
        // mistake, so the rule lives here with the waiver written next to it.
        //
        // A person who has not armed War Mode and carries no bounty is not
        // somebody a bot may pick. That is decided by the PvE manager, which
        // owns the snapshot and the consent rules (battlegrounds, arenas, an
        // accepted duel), so there is one answer rather than two.
        Player const* person = target->ToPlayer();
        if (!person)
            if (Unit const* owner = target->GetCharmerOrOwner())
                person = owner->ToPlayer();   // and not their pet either
        if (!person)
            return true;                      // creatures are unchanged

        // THE WAIVER. Already in the fight - the bot's own victim, or somebody
        // (or something of theirs) swinging at the bot. Self-defence is never
        // filtered, whatever the target's settings say.
        if (player->GetVictim() == target)
            return true;
        for (Unit* attacker : player->getAttackers())
            if (attacker == target ||
                (attacker && attacker->GetCharmerOrOwnerPlayerOrPlayerItself() == person))
                return true;

        // The lock behind this is only reached for a real person the bot is not
        // already fighting, which in a battleground is answered before it and in
        // the open world is a handful of people at most.
        return playerbot::PveManager::MayProactivelyEngage(player, person);
    }

    // A person within reach whom the bot is fighting or free to start on -
    // HasHostileTarget's rule, consent included. HasNearbyAttackableEnemyPlayer
    // asks only IsValidAttackTarget, which an armed bot in the open world
    // passes against everybody, War Mode or not.
    bool HasNearbyHostilePlayer(Player const* player, float maxDistance, bool requireLineOfSight)
    {
        if (!player || !player->IsInWorld() || !player->FindMap())
            return false;

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!candidate || candidate == player || !player->IsWithinDistInMap(candidate, maxDistance))
                continue;
            if (!HasHostileTarget(player, candidate))
                continue;
            if (requireLineOfSight && !player->IsWithinLOSInMap(candidate))
                continue;
            return true;
        }

        return false;
    }

    bool IsFriendlySupportTarget(Player const* player, Unit const* target)
    {
        if (!player || !target || !target->IsAlive())
            return false;

    Player const* targetPlayer = target->ToPlayer();
    if (targetPlayer && targetPlayer->IsSpectator())
        return false;

    if (target == player)
        return true;

        if (player->IsValidAssistTarget(target))
            return true;

    if (!targetPlayer || !player->InBattleground() || !targetPlayer->InBattleground())
        return false;

        if (player->GetBattlegroundId() != targetPlayer->GetBattlegroundId())
            return false;

        // A squad match seats two squads a side; the squad is the team.
        if (Battleground const* battleground = player->GetBattleground(); battleground && battleground->IsSquadMatch())
            return battleground->AreTeammates(player->GetGUID(), targetPlayer->GetGUID());

        uint32 const playerTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
        uint32 const targetTeam = targetPlayer->GetBGTeam() ? targetPlayer->GetBGTeam() : targetPlayer->GetTeam();
        return playerTeam == targetTeam;
    }

    bool HasAnyAura(Unit const* unit, std::initializer_list<uint32> spellIds)
    {
        if (!unit)
            return false;

        for (uint32 spellId : spellIds)
            if (unit->HasAura(spellId))
                return true;

        return false;
    }

    bool IsPhysicalDamageClass(uint8 classId)
    {
        return classId == CLASS_WARRIOR || classId == CLASS_ROGUE || classId == CLASS_HUNTER;
    }

    bool IsUsingTwoHander(Player const* player)
    {
        if (!player)
            return false;

        Item* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
        return mainHand && mainHand->GetTemplate() && mainHand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;
    }

    bool IsMeleeClass(Unit const* unit)
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
            return IsUsingTwoHander(player);
        default:
            return false;
        }
    }

    bool HasAuraFromSpellChain(Unit const* unit, uint32 baseSpellId)
    {
        if (!unit || !baseSpellId)
            return false;

        SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(baseSpellId);
        if (!baseSpellInfo)
            return false;

        for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
            if (unit->HasAura(chainSpellId))
                return true;

        return false;
    }

    // The item an in-chain rank ACTUALLY conjures. Mana Agate, Jade, Citrine
    // and Ruby are four ranks of one spell that each create a DIFFERENT item,
    // so testing for the top rank's gem while the cast resolves to a lower
    // rank is a condition that can never clear: the mage reconjures forever,
    // and because every attempt clears its movement to cast, it reads in game
    // as a bot stutter-stepping toward a target it never reaches. Ask the
    // resolved rank what it makes instead of hardcoding one item id.
    uint32 ResolveConjuredItemForKnownRank(Player const* player, uint32 chainSpellId)
    {
        uint32 const resolvedSpellId = ResolveKnownPlayerSpellInChain(player, chainSpellId);
        SpellInfo const* spellInfo = resolvedSpellId ? sSpellMgr->GetSpellInfo(resolvedSpellId) : nullptr;
        if (!spellInfo)
            return 0;

        for (uint8 effectIndex = 0; effectIndex < MAX_SPELL_EFFECTS; ++effectIndex)
        {
            SpellEffectInfo const& effect = spellInfo->GetEffect(SpellEffIndex(effectIndex));
            if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType)
                return effect.ItemType;
        }

        return 0;
    }

    // The aura for whichever RANK of a chain is actually on the unit. Asking
    // for one exact rank and reading its duration is a silent failure: the
    // arm simply never runs for anyone carrying a different rank.
    Aura* GetAuraFromSpellChain(Unit const* unit, uint32 baseSpellId)
    {
        if (!unit || !baseSpellId)
            return nullptr;

        SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(baseSpellId);
        if (!baseSpellInfo)
            return nullptr;

        for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
            if (Aura* aura = unit->GetAura(chainSpellId))
                return aura;

        return nullptr;
    }

    bool HasAuraFromSpellChain(Unit const* unit, uint32 baseSpellId, ObjectGuid casterGuid)
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

    bool HasAnyAuraFromSpellChain(Unit const* unit, std::initializer_list<uint32> baseSpellIds)
    {
        for (uint32 baseSpellId : baseSpellIds)
            if (HasAuraFromSpellChain(unit, baseSpellId))
                return true;

        return false;
    }

    // Lightning, Water and Earth Shield share one elemental-shield slot, so
    // casting Lightning Shield over an Earth Shield (the shaman's own or one
    // a resto shaman placed on it) throws the better shield away. Matched by
    // spell specific rather than chain ids so custom wrappers count too.
    bool HasElementalShield(Unit const* unit)
    {
        if (!unit)
            return false;

        for (auto const& [spellId, aurApp] : unit->GetAppliedAuras())
            if (aurApp->GetBase()->GetSpellInfo()->GetSpellSpecific() == SPELL_SPECIFIC_ELEMENTAL_SHIELD)
                return true;

        return false;
    }

bool HasHunterDamagingStingFromCaster(Unit const* unit, ObjectGuid casterGuid)
{
    return HasAuraFromSpellChain(unit, 1978, casterGuid) || // Serpent Sting
        HasAuraFromSpellChain(unit, 3034, casterGuid) ||     // Viper Sting
        HasAuraFromSpellChain(unit, 3043, casterGuid);       // Scorpid Sting
}

bool HasHunterStingFromCaster(Unit const* unit, ObjectGuid casterGuid)
{
    return HasHunterDamagingStingFromCaster(unit, casterGuid) ||
        HasAuraFromSpellChain(unit, 19386, casterGuid);      // Wyvern Sting
}

    bool HasActivePaladinSeal(Player const* player)
    {
        if (!player)
            return false;

        return HasAuraFromSpellChain(player, 21084) || // Seal of Righteousness
            HasAuraFromSpellChain(player, 20164) ||    // Seal of Justice
            HasAuraFromSpellChain(player, 20165) ||    // Seal of Light
            HasAuraFromSpellChain(player, 20166) ||    // Seal of Wisdom
            HasAuraFromSpellChain(player, 20375) ||    // Seal of Command
            HasAuraFromSpellChain(player, 31801) ||    // Seal of Vengeance
            HasAuraFromSpellChain(player, 53736) ||    // Seal of Corruption
            HasAuraFromSpellChain(player, 31892);      // Seal of Blood / Seal of the Martyr
    }

ObjectGuid SelectFriendlyWithoutManaAndAuraFromSpellChain(Player const* player, uint32 baseSpellId, float maxDistance)
{
    if (!player || !player->FindMap() || !baseSpellId)
        return ObjectGuid::Empty;

    Player* bestTarget = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive())
            continue;
        if (candidate == player || candidate->GetMaxPower(POWER_MANA) > 0)
            continue;
        if (!IsFriendlySupportTarget(player, candidate))
            continue;
        if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
            continue;
        if (HasAuraFromSpellChain(candidate, baseSpellId))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestTarget = candidate;
        }
    }

    return bestTarget ? bestTarget->GetGUID() : ObjectGuid::Empty;
}

ObjectGuid SelectFriendlyWithManaAndWithoutAuraFromSpellChain(Player const* player, uint32 baseSpellId, float maxDistance, bool includeSelf)
{
    if (!player || !player->FindMap() || !baseSpellId)
        return ObjectGuid::Empty;

    auto isEligible = [&](Player* candidate)
    {
        return candidate && candidate->IsAlive() && candidate->GetMaxPower(POWER_MANA) > 0 &&
            IsFriendlySupportTarget(player, candidate) &&
            player->IsWithinDistInMap(candidate, maxDistance) && player->IsWithinLOSInMap(candidate) &&
            !HasAuraFromSpellChain(candidate, baseSpellId);
    };

    if (includeSelf && isEligible(const_cast<Player*>(player)))
        return player->GetGUID();

    Player* bestTarget = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        // includeSelf gates the shortcut above, but this sweep walks every
        // player on the map - the caster included, at distance 0, so it always
        // wins "closest". Without this the flag is meaningless here: a ret
        // paladin looking for a mana ally to bless picked HIMSELF, Wisdom
        // replaced his own Might (one blessing per caster per target), the
        // next pass saw Might missing and recast it over Wisdom, and the two
        // buffs cycled forever during the buff phase.
        if (!includeSelf && candidate == player)
            continue;
        if (!isEligible(candidate))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestTarget = candidate;
        }
    }

    return bestTarget ? bestTarget->GetGUID() : ObjectGuid::Empty;
}

// Every raid buff below is TARGET_UNIT_CASTER_AREA_RAID: it lands on the
// caster's raid members in range that the spell's own target check accepts,
// whoever the bot "aimed" it at. The support-target test the other selectors
// use also takes same-team players that check refuses - outside the caster's
// raid, a GM, someone the caster cannot see or assist - so one of them in
// range kept the buff "missing" forever. Every buffer then recast it on every
// pass, and each cast replaced the copy the rest of the raid already had:
// Greater Blessing of Kings changing hands between paladins nonstop on
// Strand's boats. Only a player the cast would actually reach counts.
ObjectGuid SelectRaidBuffRecipientWithoutAnyAuraFromSpellChain(Player const* player, uint32 castSpellId, std::initializer_list<uint32> baseSpellIds, float maxDistance)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(castSpellId);
    if (!player || !player->FindMap() || !spellInfo || baseSpellIds.size() == 0)
        return ObjectGuid::Empty;

    if (player->IsAlive() && !HasAnyAuraFromSpellChain(player, baseSpellIds))
        return player->GetGUID();

    Player* bestTarget = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || candidate == player || !candidate->IsAlive() || candidate->IsSpectator())
            continue;
        if (!player->IsInRaidWith(candidate) || !player->IsValidAssistTarget(candidate, spellInfo))
            continue;
        if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
            continue;
        if (HasAnyAuraFromSpellChain(candidate, baseSpellIds))
            continue;

        float const distance = player->GetDistance(candidate);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestTarget = candidate;
        }
    }

    return bestTarget ? bestTarget->GetGUID() : ObjectGuid::Empty;
}

SpellDecision SelectMissingBattlegroundRaidBuff(Player const* player)
{
    if (!player || !player->InBattleground())
        return {};

    // Same relaxation as SelectPreparationBuffSpell: holding a preparation
    // aura or flag states "not fighting yet" outright, which the in-combat
    // inference gets wrong for a Violet Hold wave preparing mid-match.
    bool const preparing = player->HasAura(SPELL_ARENA_PREPARATION) || player->HasAura(SPELL_PREPARATION)
        || player->HasUnitFlag(UNIT_FLAG_PREPARATION);
    if (player->IsInCombat() && !preparing)
        return {};

    auto makeDecision = [player](char const* actionName, char const* reason, uint32 spellId,
        playerbot::PvpClassSpellContext::TargetMode targetMode, ObjectGuid targetGuid)
    {
        SpellDecision decision;
        if (targetMode == playerbot::PvpClassSpellContext::TargetMode::Ally && targetGuid.IsEmpty())
            return decision;

        decision.actionName = actionName;
        decision.reason = reason;
        decision.spellId = spellId;
        decision.targetMode = targetMode;
        decision.targetGuid = targetGuid;
        return decision;
    };

    // Raid/group buffs are intentionally selected only while their actual
    // post-aura mana cost is zero. Battleground Preparation and the brief
    // post-resurrection mana-cost aura make these casts free; outside those
    // windows bots conserve mana instead of topping off buffs.
    //
    // Do not fall back to the cheaper single-target versions. If a raid buff
    // is free, always use the broader, normally more expensive version.
    switch (player->GetClass())
    {
        case CLASS_DRUID:
        {
            if (IsZeroManaCostSpellReady(player, 21850))
                if (ObjectGuid targetGuid = SelectRaidBuffRecipientWithoutAnyAuraFromSpellChain(player, 21850, { 9885, 21850 }, 45.0f); !targetGuid.IsEmpty())
                    return makeDecision("druid gift of the wild raid", "apply free raid-wide stat buff", 21850,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            if (IsZeroManaCostSpellReady(player, kDruidMassThornsSpellId))
                if (ObjectGuid targetGuid = SelectRaidBuffRecipientWithoutAnyAuraFromSpellChain(player, kDruidMassThornsSpellId,
                    { kDruidThornsSpellId, kDruidMassThornsSpellId }, 45.0f); !targetGuid.IsEmpty())
                    return makeDecision("druid mass thorns raid", "apply free raid-wide thorns buff", kDruidMassThornsSpellId,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            break;
        }
        case CLASS_PRIEST:
        {
            if (IsZeroManaCostSpellReady(player, 21564))
                if (ObjectGuid targetGuid = SelectRaidBuffRecipientWithoutAnyAuraFromSpellChain(player, 21564, { 10938, 21564 }, 45.0f); !targetGuid.IsEmpty())
                    return makeDecision("priest prayer of fortitude raid", "apply free raid-wide fortitude buff", 21564,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            if (IsZeroManaCostSpellReady(player, 27683))
                if (ObjectGuid targetGuid = SelectRaidBuffRecipientWithoutAnyAuraFromSpellChain(player, 27683, { 10958, 27683 }, 45.0f); !targetGuid.IsEmpty())
                    return makeDecision("priest prayer of shadow protection raid", "apply free raid-wide shadow protection buff", 27683,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            if (IsZeroManaCostSpellReady(player, 27681))
                if (ObjectGuid targetGuid = SelectRaidBuffRecipientWithoutAnyAuraFromSpellChain(player, 27681, { 27841, 27681 }, 45.0f); !targetGuid.IsEmpty())
                    return makeDecision("priest prayer of spirit raid", "apply free raid-wide spirit buff", 27681,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            break;
        }
        case CLASS_MAGE:
        {
            if (IsZeroManaCostSpellReady(player, 23028))
                if (ObjectGuid targetGuid = SelectRaidBuffRecipientWithoutAnyAuraFromSpellChain(player, 23028, { 10157, 23028 }, 45.0f); !targetGuid.IsEmpty())
                    return makeDecision("mage arcane brilliance raid", "apply free raid-wide intellect buff", 23028,
                        playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID());

            break;
        }
        case CLASS_PALADIN:
        {
            bool const isRetPaladin = DetectClassicClassProfile(player).profile == ClassicClassProfile::TertiaryClassic;
            if (!isRetPaladin && IsZeroManaCostSpellReady(player, 25898))
                if (ObjectGuid targetGuid = SelectRaidBuffRecipientWithoutAnyAuraFromSpellChain(player, 25898, { 20217, 25898 }, 45.0f); !targetGuid.IsEmpty())
                    return makeDecision("paladin greater blessing of kings raid", "apply free greater blessing to an unbuffed class", 25898,
                        playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid);

            // Ret keeps its deliberate single-target blessing behavior in the
            // normal paladin selector below. It never substitutes a Greater
            // Blessing here merely because the raid version is free.
            break;
        }
        default:
            break;
    }

    return {};
}

SpellDecision SelectPreparationBuffSpell(Player const* player)
{
    SpellDecision decision;
    if (!player)
        return decision;

    // The combat check is a proxy for "the fight has not started yet", which
    // holds at a normal battleground or arena start because nobody is in combat
    // behind the gates. It does not hold in Violet Hold: waves are prepared
    // mid-match, and a wave's clones are playerbots whose AI picks a target and
    // enters combat during their own countdown. Immunity stops them being
    // attacked, not from attacking. The gate would then deny them every opening
    // buff - the exact thing the preparation window exists to give them.
    //
    // Holding a preparation aura states "not fighting yet" outright, so it wins
    // over the inference. Behaviour at ordinary starts is unchanged, since
    // nothing there is in combat to begin with.
    bool const preparing = player->HasAura(SPELL_ARENA_PREPARATION) || player->HasAura(SPELL_PREPARATION);
    if (player->IsInCombat() && !preparing)
        return decision;

        auto hasUnimbuedWeapon = [player]()
        {
            Item const* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
            if (mainHand && !mainHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                return true;

            Item const* offHand = player->GetWeaponForAttack(OFF_ATTACK, true);
            if (offHand && !offHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                return true;

            return false;
        };

        switch (player->GetClass())
        {
        case CLASS_ROGUE:
        {
            if (DetectClassicClassProfile(player).profile == ClassicClassProfile::PrimaryClassic)
            {
                // Assassination: Wound Poison on the mainhand, Mind-Numbing
                // Poison on the offhand, instead of double Crippling Poison.
                // CastDirectSpell's temporary-weapon-imbue handling always
                // fills the mainhand slot first, then the offhand, so gating
                // each poison on the other slot's enchant state sequences
                // them onto the correct weapon.
                Item const* mainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
                bool const mainHandUnenchanted = mainHand && !mainHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
                Item const* offHand = player->GetWeaponForAttack(OFF_ATTACK, true);
                bool const offHandUnenchanted = offHand && !offHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);

                if (mainHandUnenchanted && IsSpellReady(player, 13227))
                    return { "rogue wound poison prep", "coat mainhand with wound poison before gates open", 13227, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

                if (!mainHandUnenchanted && offHandUnenchanted && IsSpellReady(player, 11399))
                    return { "rogue mind-numbing poison prep", "coat offhand with mind-numbing poison before gates open", 11399, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

                break;
            }

            if (IsSpellReady(player, 11202) && hasUnimbuedWeapon())
                return { "rogue crippling poison prep", "coat both weapons before gates open", 11202, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        case CLASS_WARRIOR:
        {
            if (IsSpellReady(player, 2687) && !HasAuraFromSpellChain(player, 2687))
                return { "warrior bloodrage prep", "generate opening rage before gates open", 2687, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (IsSpellReady(player, 25289) && !HasAuraFromSpellChain(player, 25289))
                return { "warrior battle shout prep", "maintain battle shout before gates open", 25289, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        case CLASS_HUNTER:
        {
            HunterPetDecisionState const petState = GetHunterPetDecisionState(player);
            bool const hasDeadPet = petState.hasDeadPet || petState.shouldRevivePet;
            if (hasDeadPet &&
                !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kHunterRevivePetSpellId) &&
                IsSpellReady(player, kHunterRevivePetSpellId))
                return { "hunter revive pet prep", "revive dead hunter pet before gates open", kHunterRevivePetSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (!petState.hasLivingPet && !hasDeadPet && petState.canCallPet &&
                !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kHunterCallPetSpellId) &&
                IsSpellReady(player, kHunterCallPetSpellId))
                return { "hunter call pet prep", "call active stable pet before gates open", kHunterCallPetSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        case CLASS_DRUID:
        case CLASS_PRIEST:
            // Raid buffs are handled by SelectMissingBattlegroundRaidBuff()
            // after class-specific preparation actions.
            break;
        case CLASS_MAGE:
        {
            if (uint32 const manaGemItemId = ResolveConjuredItemForKnownRank(player, 10054);
                manaGemItemId && IsSpellReady(player, 10054) && !player->HasItemCount(manaGemItemId))
                return { "create mana gem prep", "conjure a mana gem before gates open", 10054, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            if (!HasAuraFromSpellChain(player, 10220) && IsSpellReady(player, 10220))
                return { "frost armor prep", "maintain armor before gates open", 10220, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        case CLASS_PALADIN:
        {
            ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
            bool const isRetPaladin = profileSelection.profile == ClassicClassProfile::TertiaryClassic;

            if (!isRetPaladin)
                break;

            if (IsSpellReady(player, 25290))
            {
                if (ObjectGuid targetGuid = SelectFriendlyWithManaAndWithoutAuraFromSpellChain(player, 25290, 45.0f, false); !targetGuid.IsEmpty())
                    return { "paladin blessing of wisdom prep", "ret paladin manually buffs nearby mana allies", 25290, playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid };
            }

            if (IsSpellReady(player, 25291))
            {
                if (!HasAuraFromSpellChain(player, 25291))
                    return { "paladin blessing of might self prep", "ret paladin prefers might before gates open", 25291, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

                if (ObjectGuid targetGuid = SelectFriendlyWithoutManaAndAuraFromSpellChain(player, 25291, 45.0f); !targetGuid.IsEmpty())
                    return { "paladin blessing of might prep", "buff nearby non-mana allies before gates open", 25291, playerbot::PvpClassSpellContext::TargetMode::Ally, targetGuid };
            }

            break;
        }
        case CLASS_WARLOCK:
        {
            Pet const* pet = player->GetPet();
            if ((!pet || !pet->IsAlive()) && !IsPetParkedByMount(player))
            {
                ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
                bool const isAfflictionWarlock = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
                bool const isDestructionWarlock = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
                uint32 const summonPetSpell = isAfflictionWarlock ? 691 : (isDestructionWarlock ? 712 : 697);
                char const* summonPetName = isAfflictionWarlock ? "warlock summon felhunter prep" : (isDestructionWarlock ? "warlock summon succubus prep" : "warlock summon voidwalker prep");
                char const* summonPetReason = isAfflictionWarlock ? "summon felhunter before gates open" : (isDestructionWarlock ? "summon succubus before gates open" : "summon voidwalker before gates open");
                if (IsSpellReady(player, summonPetSpell))
                    return { summonPetName, summonPetReason, summonPetSpell, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };
                // The spec's demon is not trained yet (Felhunter 30, Succubus 20).
                if (uint32 const fallbackPetSpell = SelectWarlockFallbackSummonSpell(player))
                    return { "warlock summon fallback demon prep", "summon the best demon trained so far before gates open", fallbackPetSpell, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };
            }

            // The player knows Ritual of Souls; its triggered Create Soulwell
            // effect is selected directly so virtual teammates do not have to
            // emulate a three-client summoning ritual during the short prep
            // window. The normal summoned Soulwell object, ownership, lifetime,
            // and charge counter are still used.
            if (player->HasSpell(kWarlockRitualOfSoulsSpellId) &&
                !playerbot::PvpCore::FindUsableSoulwell(player, 60.0f))
                return { "warlock create soulwell prep", "create a team healthstone well before gates open",
                    kWarlockCreateSoulwellSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };

            break;
        }
        default:
            break;
        }

    if (SpellDecision const raidBuffDecision = SelectMissingBattlegroundRaidBuff(player); raidBuffDecision.spellId)
        return raidBuffDecision;

    if (SpellDecision const mountDecision = SelectMountSpell(player, "mount during preparation after prep actions"); mountDecision.spellId)
        return mountDecision;

        return decision;
    }

    bool IsCasterClass(Unit const* unit)
    {
        Player const* player = unit ? unit->ToPlayer() : nullptr;
        if (!player)
            return false;

        switch (player->GetClass())
        {
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
        case CLASS_DRUID:
        case CLASS_SHAMAN:
        case CLASS_PALADIN:
            return true;
        default:
            return false;
        }
    }

    bool ShouldUseCurseOfTongues(Unit const* unit)
    {
        Player const* player = unit ? unit->ToPlayer() : nullptr;
        if (!player)
            return false;

        // Curse of Tongues should focus on true caster targets and avoid hunters or melee hybrids.
        if (player->GetClass() == CLASS_HUNTER)
            return false;

        return IsCasterClass(player) && !IsMeleeClass(player);
    }

bool IsTargetEffectivelyImmune(Player const* player, Unit const* target);
bool IsTargetInvalidByImmunity(Player const* player, Unit const* target);

    uint8 GetArmorPriority(Unit const* unit)
    {
        if (!unit)
            return 4;

        switch (unit->GetClass())
        {
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
            return 0; // Cloth
        case CLASS_ROGUE:
        case CLASS_DRUID:
            return 1; // Leather
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
            return 2; // Mail
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_DEATH_KNIGHT:
            return 3; // Plate
        default:
            return 4;
        }
    }

    Unit const* SelectWarriorPriorityTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto isCandidateUsable = [&](Unit const* candidate)
        {
            return player->IsWithinDistInMap(candidate, maxDistance) &&
                HasHostileTarget(player, candidate) &&
                !IsTargetInvalidByImmunity(player, candidate) &&
                player->IsWithinLOSInMap(candidate);
        };

        Unit const* best = nullptr;
        uint8 bestArmorPriority = std::numeric_limits<uint8>::max();
        float bestDistance = std::numeric_limits<float>::max();

        if (isCandidateUsable(preferredTarget))
        {
            best = preferredTarget;
            bestArmorPriority = GetArmorPriority(preferredTarget);
            bestDistance = player->GetDistance(preferredTarget);
        }

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!isCandidateUsable(candidate))
                continue;

            uint8 const armorPriority = GetArmorPriority(candidate);
            float const distance = player->GetDistance(candidate);
            if (armorPriority < bestArmorPriority || (armorPriority == bestArmorPriority && distance < bestDistance))
            {
                best = candidate;
                bestArmorPriority = armorPriority;
                bestDistance = distance;
            }
        }

        return best;
    }

    Unit const* SelectWarriorTauntTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto isCandidateUsable = [&](Unit const* candidate)
        {
            return player->IsWithinDistInMap(candidate, maxDistance) &&
                HasHostileTarget(player, candidate) &&
                !IsTargetInvalidByImmunity(player, candidate) &&
                player->IsWithinLOSInMap(candidate);
        };

        auto isPressuringFriendly = [&](Unit const* candidate)
        {
            if (!isCandidateUsable(candidate))
                return false;

            Unit const* victim = candidate->GetVictim();
            return victim && victim != player && IsFriendlySupportTarget(player, victim);
        };

        if (isPressuringFriendly(preferredTarget))
            return preferredTarget;

        Unit const* bestPressureTarget = nullptr;
        float bestPressureDistance = std::numeric_limits<float>::max();
        Unit const* bestFallbackTarget = nullptr;
        float bestFallbackDistance = std::numeric_limits<float>::max();

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!isCandidateUsable(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (isPressuringFriendly(candidate) && distance < bestPressureDistance)
            {
                bestPressureTarget = candidate;
                bestPressureDistance = distance;
            }

            if (candidate->GetVictim() != player && distance < bestFallbackDistance)
            {
                bestFallbackTarget = candidate;
                bestFallbackDistance = distance;
            }
        }

        if (bestPressureTarget)
            return bestPressureTarget;

        if (isCandidateUsable(preferredTarget) && preferredTarget->GetVictim() != player)
            return preferredTarget;

        return bestFallbackTarget;
    }

    Unit const* SelectNearbyMeleeTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto isCandidateUsable = [&](Unit const* candidate)
        {
            return HasHostileTarget(player, candidate) &&
                !IsTargetInvalidByImmunity(player, candidate) &&
                IsMeleeClass(candidate) &&
                player->IsWithinLOSInMap(candidate) &&
                player->IsWithinDistInMap(candidate, maxDistance);
        };

        if (isCandidateUsable(preferredTarget))
            return preferredTarget;

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!isCandidateUsable(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }

    Unit const* SelectNearbyEnemyTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto isCandidateUsable = [&](Unit const* candidate)
        {
            return player->IsWithinDistInMap(candidate, maxDistance) &&
                HasHostileTarget(player, candidate) &&
                !IsTargetInvalidByImmunity(player, candidate) &&
                player->IsWithinLOSInMap(candidate);
        };

        if (isCandidateUsable(preferredTarget))
            return preferredTarget;

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!isCandidateUsable(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }

    Unit const* SelectEnemyTargetInSpellRange(Player const* player, Unit const* preferredTarget, uint32 spellId)
    {
        if (!player || !player->FindMap())
            return nullptr;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return nullptr;

        float const minRange = spellInfo->GetMinRange(false);
        float const maxRange = spellInfo->GetMaxRange(false);

        auto isCandidateUsable = [&](Unit const* candidate)
        {
            if (!HasHostileTarget(player, candidate) || IsTargetInvalidByImmunity(player, candidate))
                return false;
            if (!player->IsWithinLOSInMap(candidate))
                return false;
            if (minRange > 0.0f && player->IsWithinDistInMap(candidate, minRange))
                return false;
            if (maxRange > 0.0f && !player->IsWithinDistInMap(candidate, maxRange))
                return false;

            return player->IsValidAttackTarget(candidate, spellInfo);
        };

        if (isCandidateUsable(preferredTarget))
            return preferredTarget;

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!isCandidateUsable(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }

    Unit const* SelectEnemyGapCloserTarget(Player const* player, Unit const* preferredTarget, float minDistance, float maxDistance, bool preferFarthest)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto isCandidateUsable = [&](Unit const* candidate)
        {
            if (!HasHostileTarget(player, candidate) || IsTargetInvalidByImmunity(player, candidate))
                return false;
            if (HasBreakableCrowdControl(player, candidate))
                return false;
            if (!player->IsWithinLOSInMap(candidate))
                return false;

            float const distance = player->GetExactDist(candidate);
            return distance >= minDistance && distance <= maxDistance;
        };

        if (isCandidateUsable(preferredTarget))
            return preferredTarget;

        Unit const* best = nullptr;
        float bestDistance = preferFarthest ? -1.0f : std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!isCandidateUsable(candidate))
                continue;

            float const distance = player->GetExactDist(candidate);
            if ((preferFarthest && distance > bestDistance) || (!preferFarthest && distance < bestDistance))
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }

    Unit const* SelectNearbyEnemyManaTarget(Player const* player, Unit const* preferredTarget, float maxDistance, float minManaPct)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto isCandidateUsable = [&](Unit const* candidate)
        {
            if (!HasHostileTarget(player, candidate) || IsTargetInvalidByImmunity(player, candidate))
                return false;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                return false;
            if (candidate->GetPowerType() != POWER_MANA)
                return false;

            return candidate->GetPowerPct(POWER_MANA) > minManaPct;
        };

        if (isCandidateUsable(preferredTarget))
            return preferredTarget;

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!isCandidateUsable(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }

    uint32 CountNearbyUnsNaredEnemies(Player const* player, float maxDistance)
    {
        if (!player || !player->FindMap())
            return 0;

        uint32 count = 0;
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (IsTargetInvalidByImmunity(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            // Hamstring, any rank (1715, 7372, 7373) - screening only rank 1
            // meant a target snared with rank 2 or 3 read as unsnared.
            if (HasAuraFromSpellChain(candidate, 1715))
                continue;

            ++count;
        }

        return count;
    }

    bool HasBreakableCrowdControl(Player const* observer, Unit const* unit)
    {
        return playerbot::PvpCore::HasBreakableCrowdControlFor(observer, unit);
    }

    bool IsPolymorphed(Unit const* unit)
    {
        return HasAnyAura(unit, { 118, 12824, 12825, 12826, 28272, 28271, 61305, 61721 });
    }

    bool IsWyvernStung(Unit const* unit)
    {
        return HasAuraFromSpellChain(unit, 19386);
    }

    bool HasDotAura(Unit const* unit)
    {
        if (!unit)
            return false;

        return unit->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE) ||
            unit->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
    }

    bool IsInterruptibleCast(Unit const* unit)
    {
        if (!unit)
            return false;

        auto isInterruptibleCurrentSpell = [&](CurrentSpellTypes spellType)
        {
            Spell const* currentSpell = unit->GetCurrentSpell(spellType);
            if (!currentSpell)
                return false;

            SpellInfo const* spellInfo = currentSpell->GetSpellInfo();
            if (!spellInfo || spellInfo->PreventionType != SPELL_PREVENTION_TYPE_SILENCE)
                return false;

            uint32 const state = currentSpell->getState();
            bool const isInInterruptiblePhase = state == SPELL_STATE_CASTING ||
                (state == SPELL_STATE_PREPARING && currentSpell->GetCastTime() > 0.0f);
            if (!isInInterruptiblePhase)
                return false;

            if (spellType == CURRENT_GENERIC_SPELL)
                return (spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) != 0;

            if (spellType == CURRENT_CHANNELED_SPELL)
                return (spellInfo->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT) != 0;

            return false;
        };

        return isInterruptibleCurrentSpell(CURRENT_GENERIC_SPELL) ||
            isInterruptibleCurrentSpell(CURRENT_CHANNELED_SPELL);
    }

bool IsTargetEffectivelyImmune(Player const* player, Unit const* target)
{
    if (!player || !target)
        return true;

        if (Player const* targetPlayer = target->ToPlayer())
        {
            if (targetPlayer->isTotalImmune())
                return true;
        }

        // Divine Shield, EITHER rank (642, 1020). Testing only rank 1 left
        // bots hammering away at a bubbled paladin who had trained rank 2.
        if (HasAuraFromSpellChain(target, 642))
            return true;

        if (target->HasAura(11958)) // Ice Block
            return true;

        if (IsPhysicalDamageClass(player->GetClass()) && HasAnyAura(target, { 1022, 5599, 10278 })) // Blessing of Protection ranks
            return true;

    return false;
}

bool IsTargetInvalidByImmunity(Player const* player, Unit const* target)
{
    return IsTargetEffectivelyImmune(player, target) || HasBreakableCrowdControl(player, target);
}

// A ledge the battleground says players jump off (Eye of the Storm's start
// platforms) has no way back up it on the navmesh. Something standing on one
// is out of reach of anybody who is not on it too: a bot put down off its
// platform at the start, without the healthstone it would have taken in a full
// preparation, otherwise walks back for the platform's soulwell 50 yards up
// and stands under the ledge for the rest of the match. A fight picked with
// somebody waiting up there ends the same way.
static bool IsOnDropIslandApartFrom(Player const* player, WorldObject const* object)
{
    Battleground const* battleground = player->GetBattleground();
    if (!battleground)
        return false;

    std::vector<BattlegroundDropExit> exits;
    battleground->GetDropExits(player->GetGUID(), exits);
    auto const isOn = [](BattlegroundDropExit const& exit, WorldObject const* who)
    {
        return who->GetPositionZ() >= exit.IslandMinZ &&
            who->GetExactDist2d(exit.IslandCentre.GetPositionX(), exit.IslandCentre.GetPositionY()) <= exit.IslandRadius;
    };

    for (BattlegroundDropExit const& exit : exits)
        if (isOn(exit, object) && !isOn(exit, player))
            return true;

    return false;
}

Unit const* SelectClosestEnemyTarget(Player const* player, bool requireReachable)
{
    if (!player || !player->FindMap())
        return nullptr;

    // Gather attackable candidates with the cheap checks first, then spend
    // immunity scans and line-of-sight raycasts nearest-first. The selected
    // target is identical to evaluating every candidate, but usually only the
    // closest one or two ever pay for a raycast.
    std::vector<std::pair<float, Player const*>> candidates;
    candidates.reserve(16);
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive() || candidate == player)
            continue;
        if (!player->IsValidAttackTarget(candidate))
            continue;

        // IsValidAttackTarget says this person CAN be fought - the hardcore
        // pseudo-faction makes every armed bot and every real player mutually
        // attackable - but this scan is the bot choosing a fight for itself, so
        // it also has to ask whether it MAY start one. War Mode or a bounty
        // outside consented ground; inside a battleground or against a duel
        // opponent the answer is always yes and nothing changes.
        //
        // Retaliation never comes through here: this whole scan only runs when
        // the bot has no selected target AND no victim, so anyone already
        // swinging at it has been picked several branches earlier.
        if (!playerbot::PveManager::MayProactivelyEngage(player, candidate))
            continue;

            float const distance = player->GetDistance(candidate);
            if (requireReachable && distance > 35.0f)
                continue;

        if (IsOnDropIslandApartFrom(player, candidate))
            continue;

        candidates.emplace_back(distance, candidate);
    }

    std::sort(candidates.begin(), candidates.end(),
        [](std::pair<float, Player const*> const& left, std::pair<float, Player const*> const& right)
    {
        return left.first < right.first;
    });

    for (auto const& [distance, candidate] : candidates)
    {
        if (IsTargetInvalidByImmunity(player, candidate))
            continue;
        if (!player->IsWithinLOSInMap(candidate))
            continue;

        return candidate;
    }

    return nullptr;
}

// The nearest living unit the bot is in real PvP combat with: someone who hit
// it, or whom it hit, inside the core's five second PvP combat window.
//
// Target selection cannot answer "who is fighting me". A caster pelting the bot
// with Frostbolts never auto-attacks, so it is not the bot's victim or one of
// its attackers, and the acquisition scan skips it for range, line of sight or
// engagement consent. The combat references are the core's own record of the
// fight, so an opponent found here is one the bot really cannot rest through.
Unit const* FindEngagedPvpOpponent(Player const* player)
{
    if (!player)
        return nullptr;

    Unit const* nearest = nullptr;
    float nearestDistance = 0.0f;
    for (auto const& [guid, ref] : player->GetCombatManager().GetPvPCombatRefs())
    {
        if (!ref || ref->IsSuppressedFor(player))
            continue;

        Unit const* other = ref->GetOther(player);
        if (!other || !other->IsAlive() || !other->IsInMap(player))
            continue;
        if (!player->IsValidAttackTarget(other))
            continue;

        float const distance = player->GetDistance(other);
        if (!nearest || distance < nearestDistance)
        {
            nearest = other;
            nearestDistance = distance;
        }
    }

    return nearest;
}

// EvaluateTeamDanger. A teammate this close is one to look after (all of any
// arena); one in a fight below this much health, or with anybody on it, is in
// danger; an enemy this close to the bot, or to a teammate, keeps the bot on
// its feet; and a scare lasts this long after it was last seen, so an enemy
// stepping behind a pillar does not sit the bot straight back down.
constexpr float TEAM_DANGER_ALLY_RANGE = 60.0f;
constexpr float TEAM_DANGER_ALLY_HEALTH_PCT = 80.0f;
constexpr float TEAM_DANGER_ENEMY_NEAR_SELF = 40.0f;
constexpr float TEAM_DANGER_ENEMY_NEAR_ALLY = 30.0f;
constexpr uint32 TEAM_DANGER_HOLD_TOKEN = 900011;   // a PvpClassActions caster cooldown slot
constexpr std::chrono::milliseconds TEAM_DANGER_HOLD = std::chrono::milliseconds(4000);

// Whether the bot, or a teammate near it, is in a fight: the bot is in combat
// or engaged, an enemy is on it, casting at it or close by; or a teammate has
// attackers, is in combat and hurt, is the victim or the cast target of an
// enemy, or has an enemy close by. Read off the whole map, line of sight or
// not - a teammate dying behind a pillar is exactly the one that matters.
//
// Every guard on eating and drinking used to look at the bot alone, and PvP
// combat lapses five seconds after the last blow: a healer nobody was hitting
// sat down to drink while its partner died. Only in battlegrounds and arenas;
// with `findAlly` it also names the teammate most in need.
TeamDanger EvaluateTeamDanger(Player const* player, bool findAlly)
{
    TeamDanger danger;
    if (!player || !player->IsAlive() || !player->InBattleground())
        return danger;

    Map* map = player->FindMap();
    if (!map)
        return danger;

    bool seen = player->IsInCombat() || FindEngagedPvpOpponent(player) != nullptr;

    std::vector<Player const*> enemies;
    std::vector<Player const*> allies;
    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        Player const* other = itr->GetSource();
        if (!other || other == player || !other->IsAlive() || !other->IsInMap(player))
            continue;

        if (HasHostileTarget(player, other))
            enemies.push_back(other);
        else if (player->IsWithinDistInMap(other, TEAM_DANGER_ALLY_RANGE) && IsFriendlySupportTarget(player, other))
            allies.push_back(other);
    }

    if (!seen)
        for (Player const* enemy : enemies)
            if (player->IsWithinDistInMap(enemy, TEAM_DANGER_ENEMY_NEAR_SELF) || enemy->GetVictim() == player ||
                HasCastTimeSpellTargetingPlayer(enemy, player))
            {
                seen = true;
                break;
            }

    float lowestHealth = 101.0f;
    for (Player const* ally : allies)
    {
        if (seen && !findAlly)
            break;

        bool allyInDanger = !ally->getAttackers().empty() ||
            (ally->IsInCombat() && ally->GetHealthPct() < TEAM_DANGER_ALLY_HEALTH_PCT);
        for (Player const* enemy : enemies)
        {
            if (allyInDanger)
                break;
            allyInDanger = ally->IsWithinDistInMap(enemy, TEAM_DANGER_ENEMY_NEAR_ALLY) || enemy->GetVictim() == ally ||
                HasCastTimeSpellTargetingPlayer(enemy, ally);
        }

        if (!allyInDanger)
            continue;

        seen = true;
        if (findAlly && ally->GetHealthPct() < lowestHealth)
        {
            lowestHealth = ally->GetHealthPct();
            danger.endangeredAlly = ally->GetGUID();
        }
    }

    if (seen)
        playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, TEAM_DANGER_HOLD_TOKEN, TEAM_DANGER_HOLD);
    danger.inDanger = seen || playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, TEAM_DANGER_HOLD_TOKEN);
    return danger;
}

uint32 GetCurrentCastSpellId(Unit const* unit)
{
    if (!unit)
        return 0;

    if (Spell const* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (SpellInfo const* spellInfo = spell->GetSpellInfo())
            return spellInfo->Id;

    if (Spell const* spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        if (SpellInfo const* spellInfo = spell->GetSpellInfo())
            return spellInfo->Id;

    return 0;
}

// Milliseconds the unit's current cast has spent on its cast bar, or -1 when
// it is not on a timed cast bar (a channel, or nothing at all).
int32 GetCurrentCastElapsedMs(Unit const* unit)
{
    Spell const* spell = unit ? unit->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
    if (!spell || spell->GetCastTime() <= 0)
        return -1;

    return std::max(spell->GetCastTimeElapsed(), 0);
}

// How long a committed "button press" stays live waiting for a decision tick
// to actually deliver it. Must cover at least one battleground fast tick, but
// stay short enough that a stale press never fires visibly late.
constexpr uint32 kInterruptPressSlopMs = 300;
// Maximum unobserved gap after which a same-spell cast by the same enemy is
// treated as a brand-new cast (fresh reaction) instead of a continuation.
// Sized above the 500ms world decision cadence so coarse ticking alone never
// splits one cast into two.
constexpr uint32 kInterruptCastContinuityGapMs = 750;
// Bots that stop asking - a battleground copy whose match ended - leave their
// last watches behind. Past this many tracked bots, the idle ones are swept.
constexpr size_t kInterruptReactionSweepThreshold = 256;
constexpr uint32 kInterruptReactionIdleMs = 30000;

// This bot's own steady reaction time: where in [ReactionMinMs, ReactionMaxMs]
// it sits. Keyed on the name players see rather than the guid - a battleground
// fill copy is a fresh guid under an internal name every match, and the point
// is that the same mage kicks the same way every time players meet it, so they
// can learn its timing and fake it out.
uint32 GetInterruptBaseReactionMs(Player const* player)
{
    uint32 const reactionMaxMs = g_PvpCoreConfig.interruptReactionMaxMs;
    uint32 const reactionMinMs = std::min(g_PvpCoreConfig.interruptReactionMinMs, reactionMaxMs);
    if (reactionMaxMs == reactionMinMs)
        return reactionMinMs;

    uint32 hash = 2166136261u; // FNV-1a
    for (char const c : Battleground::GetPlayerDisplayName(player))
    {
        hash ^= uint8(c);
        hash *= 16777619u;
    }

    return reactionMinMs + hash % (reactionMaxMs - reactionMinMs + 1);
}

// One cast's reaction: the bot's steady time give or take the jitter, kept
// inside [ReactionMinMs, ReactionMaxMs]. A caster that is not the bot's own
// target is noticed later on top of that.
uint32 RollInterruptReactionMs(Player const* player, bool focused)
{
    uint32 const reactionMaxMs = g_PvpCoreConfig.interruptReactionMaxMs;
    uint32 const reactionMinMs = std::min(g_PvpCoreConfig.interruptReactionMinMs, reactionMaxMs);
    int32 const jitterMs = int32(g_PvpCoreConfig.interruptJitterMs);
    int32 const reactionMs = std::clamp(
        int32(GetInterruptBaseReactionMs(player)) + (jitterMs ? irand(-jitterMs, jitterMs) : 0),
        int32(reactionMinMs), int32(reactionMaxMs));
    return uint32(reactionMs) + (focused ? 0 : g_PvpCoreConfig.interruptOffTargetExtraMs);
}

// Humanizes bot interrupt reflexes. Every enemy cast the bot sees is watched
// on its own, and the bot presses its interrupt at its own steady reaction
// time (GetInterruptBaseReactionMs), not the instant the cast appears.
//
// Attention: the bot's own target (`focus`) is always noticed, at full speed.
// A caster it is not fighting is noticed only Interrupt.OffTargetNoticePct of
// the time, and Interrupt.OffTargetExtraMs later, so a mage busy with a warrior
// does not lock out the healer behind it on reflex. Turning to a caster
// mid-cast counts as noticing it as the target from that moment.
//
// Commitment: once the press moment arrives the button is on its way down. If
// the cast vanishes within the commit window just before that moment, the
// interrupt still goes out at the now-idle caster and is wasted; if the caster
// restarts without a pause, the press lands on the new cast. A player who has
// learned a bot's timing can fake it out, while cancelling early just makes it
// hold the kick.
//
// `casters` is every caster usable for this ability, the focus first. Returns
// the one to interrupt now, if any.
Unit const* ApplyHumanInterruptReaction(Player const* player, Unit const* focus,
    std::vector<Unit const*> const& casters, float maxDistance)
{
    // The bot's own target first, then the nearest.
    Unit const* pick = nullptr;
    float pickRank = std::numeric_limits<float>::max();
    auto consider = [&](Unit const* caster)
    {
        float const rank = caster == focus ? -1.0f : player->GetDistance(caster);
        if (rank < pickRank)
        {
            pick = caster;
            pickRank = rank;
        }
    };

    if (g_PvpCoreConfig.interruptReactionMaxMs == 0)
    {
        for (Unit const* caster : casters)
            consider(caster);
        return pick; // humanization disabled: legacy instant reflexes
    }

    uint32 const now = GameTime::GetGameTimeMS();
    uint32 const commitWindowMs = g_PvpCoreConfig.interruptCommitWindowMs;
    std::lock_guard<std::mutex> lock(g_InterruptReactionStateLock);

    if (g_InterruptReactionStateByBot.size() > kInterruptReactionSweepThreshold)
    {
        for (auto itr = g_InterruptReactionStateByBot.begin(); itr != g_InterruptReactionStateByBot.end();)
        {
            InterruptReactionState const& idleCandidate = itr->second;
            bool const active = now - idleCandidate.pressReturnedAtMs <= kInterruptReactionIdleMs ||
                std::any_of(idleCandidate.watchesByCaster.begin(), idleCandidate.watchesByCaster.end(),
                    [now](auto const& entry) { return now - entry.second.lastSeenCastingMs <= kInterruptReactionIdleMs; });
            itr = active ? std::next(itr) : g_InterruptReactionStateByBot.erase(itr);
        }
    }

    auto stateItr = g_InterruptReactionStateByBot.find(player->GetGUID());
    if (stateItr == g_InterruptReactionStateByBot.end())
    {
        if (casters.empty())
            return nullptr;
        stateItr = g_InterruptReactionStateByBot.try_emplace(player->GetGUID()).first;
    }
    InterruptReactionState& state = stateItr->second;

    // Whether the caster is still on the very cast this watch was opened for:
    // the same spell, not out of sight too long, and begun before the last
    // sighting (cast-bar time only ever grows within one cast, pushback aside).
    auto continuesWatchedCast = [now](InterruptCastWatch const& watch, Unit const* caster)
    {
        if (GetCurrentCastSpellId(caster) != watch.castSpellId)
            return false;

        uint32 const unseenMs = now - watch.lastSeenCastingMs;
        if (unseenMs > kInterruptCastContinuityGapMs)
            return false;

        int32 const castElapsedMs = GetCurrentCastElapsedMs(caster);
        return castElapsedMs < 0 || uint32(castElapsedMs) >= unseenMs;
    };
    // The cast was still up when the press moment came, or left inside the
    // commit window just before it.
    auto isCommittedAtCastEnd = [commitWindowMs](InterruptCastWatch const& watch)
    {
        return watch.noticed && watch.pressAtMs <= watch.lastSeenCastingMs + commitWindowMs;
    };

    for (Unit const* caster : casters)
    {
        bool const isFocus = caster == focus;
        auto [watchItr, isNewWatch] = state.watchesByCaster.try_emplace(caster->GetGUID());
        InterruptCastWatch& watch = watchItr->second;
        if (isNewWatch || !continuesWatchedCast(watch, caster))
        {
            // A new cast. A press already on its way lands on it anyway.
            bool const pressInFlight = !isNewWatch && isCommittedAtCastEnd(watch) &&
                (now <= watch.pressAtMs || now - watch.pressAtMs <= kInterruptPressSlopMs);
            uint32 const pressAtMs = pressInFlight ? watch.pressAtMs : now + RollInterruptReactionMs(player, isFocus);
            watch = InterruptCastWatch();
            watch.castSpellId = GetCurrentCastSpellId(caster);
            watch.pressAtMs = pressAtMs;
            watch.noticed = pressInFlight || isFocus ||
                roll_chance_i(int32(g_PvpCoreConfig.interruptOffTargetNoticePct));
            watch.focused = isFocus;
        }
        else if (isFocus && !watch.focused)
        {
            // The bot turned to this caster mid-cast: from here on it is
            // watching the cast bar directly.
            uint32 const focusedPressAtMs = now + RollInterruptReactionMs(player, true);
            watch.pressAtMs = watch.noticed ? std::min(watch.pressAtMs, focusedPressAtMs) : focusedPressAtMs;
            watch.noticed = true;
            watch.focused = true;
        }

        watch.lastSeenCastingMs = now;
        if (watch.noticed && now >= watch.pressAtMs)
            consider(caster);
    }

    for (auto itr = state.watchesByCaster.begin(); itr != state.watchesByCaster.end();)
    {
        InterruptCastWatch& watch = itr->second;
        if (watch.lastSeenCastingMs == now)
        {
            ++itr; // seen casting this tick, by this call or one for another ability
            continue;
        }

        Unit const* tracked = ObjectAccessor::GetUnit(*player, itr->first);
        if (tracked && continuesWatchedCast(watch, tracked) && IsInterruptibleCast(tracked))
        {
            // Still on the same cast, merely outside this ability's range or
            // filter. Keep watching without deciding.
            watch.lastSeenCastingMs = now;
            ++itr;
            continue;
        }

        // The watched cast is gone: finished, cancelled, or the caster left.
        if (!tracked || !isCommittedAtCastEnd(watch) || now > watch.pressAtMs + kInterruptPressSlopMs)
        {
            itr = state.watchesByCaster.erase(itr); // saw the cancel in time (hold it) or the moment passed
            continue;
        }

        // The cast left inside the commit window: the press happens anyway, at
        // its moment. Keep the watch - the enclosing context build may be one
        // that never executes a decision, so keep answering until the slop runs
        // out; spell cooldown gating prevents a double cast.
        if (now >= watch.pressAtMs && HasHostileTarget(player, tracked) && !IsTargetInvalidByImmunity(player, tracked) &&
            player->IsWithinDistInMap(tracked, maxDistance) && player->IsWithinLOSInMap(tracked))
            consider(tracked);
        ++itr;
    }

    if (pick)
    {
        if (state.pressReturnedAtMs != now)
            state.pressCasterGuids.clear();
        state.pressReturnedAtMs = now;
        if (std::find(state.pressCasterGuids.begin(), state.pressCasterGuids.end(), pick->GetGUID()) == state.pressCasterGuids.end())
            state.pressCasterGuids.push_back(pick->GetGUID());
    }
    else if (state.watchesByCaster.empty() && state.pressReturnedAtMs != now)
        g_InterruptReactionStateByBot.erase(stateItr);

    return pick;
}

Unit const* SelectEnemyCastingTarget(Player const* player, float maxDistance, Unit const* preferredTarget = nullptr)
{
    if (!player || !player->FindMap())
        return nullptr;

        auto isCandidateUsable = [&](Unit const* candidate)
        {
            return HasHostileTarget(player, candidate) &&
                !IsTargetInvalidByImmunity(player, candidate) &&
                candidate->HasUnitState(UNIT_STATE_CASTING) &&
                IsInterruptibleCast(candidate) &&
                player->IsWithinDistInMap(candidate, maxDistance) &&
                player->IsWithinLOSInMap(candidate);
        };

    // Every caster this ability could answer, the bot's own target first. When
    // - and whether - the bot answers one is the reaction model's call, its own
    // target included: a usable preferred target used to be returned straight
    // from here, so a bot kicked whoever it was fighting on its next decision
    // tick and the reaction delay only ever applied to bystanders.
    //
    // While its own target's cast is one this ability can answer, the bot has
    // eyes for that cast bar alone: nobody else's cast is noticed meanwhile.
    std::vector<Unit const*> casters;
    if (isCandidateUsable(preferredTarget))
        casters.push_back(preferredTarget);
    else
    {
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            if (Player const* candidate = itr->GetSource(); isCandidateUsable(candidate))
                casters.push_back(candidate);
    }

    // Route the pick through the reaction delay / jukeable commit window. A
    // playerbot pvp core merge once dropped this call, and with no caller bots
    // kicked frame-perfect and unjukeable while the
    // Playerbot.PvpClassSpells.Interrupt.* keys were silent no-ops.
    return ApplyHumanInterruptReaction(player, preferredTarget, casters, maxDistance);
}

    bool AnyEnemyPolymorphed(Player const* player, float maxDistance)
    {
        if (!player || !player->FindMap())
            return false;

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (IsPolymorphed(candidate))
                return true;
        }

        return false;
    }

    Unit const* SelectPolymorphTarget(Player const* player, Unit const* primaryTarget, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        SpellInfo const* polymorphInfo = sSpellMgr->GetSpellInfo(12826);
        DiminishingGroup const polymorphDrGroup = polymorphInfo ? polymorphInfo->GetDiminishingReturnsGroupForSpell(false) : DIMINISHING_NONE;

        std::vector<Unit const*> preferredTargets;
        std::vector<Unit const*> fallbackTargets;
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (primaryTarget && candidate == primaryTarget)
                continue;
            if (candidate->GetClass() == CLASS_DRUID)
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (HasDotAura(candidate) || IsPolymorphed(candidate))
                continue;
            if (IsTargetInvalidByImmunity(player, candidate))
                continue;
            if (polymorphDrGroup != DIMINISHING_NONE && candidate->GetDiminishing(polymorphDrGroup) > DIMINISHING_LEVEL_0)
                continue;

            if (candidate->GetClass() == CLASS_PALADIN || candidate->GetClass() == CLASS_PRIEST)
                preferredTargets.push_back(candidate);
            else
                fallbackTargets.push_back(candidate);
        }

        if (!preferredTargets.empty())
            return preferredTargets[urand(0, preferredTargets.size() - 1)];

        if (!fallbackTargets.empty())
            return fallbackTargets[urand(0, fallbackTargets.size() - 1)];

        return nullptr;
    }

    bool AnyEnemyWyvernStung(Player const* player, float maxDistance)
    {
        if (!player || !player->FindMap())
            return false;

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (IsWyvernStung(candidate))
                return true;
        }

        return false;
    }

    Unit const* SelectWyvernStingTarget(Player const* player, Unit const* primaryTarget, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        SpellInfo const* wyvernInfo = sSpellMgr->GetSpellInfo(24133);
        DiminishingGroup const wyvernDrGroup = wyvernInfo ? wyvernInfo->GetDiminishingReturnsGroupForSpell(false) : DIMINISHING_NONE;

        std::vector<Unit const*> preferredTargets;
        std::vector<Unit const*> fallbackTargets;
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (primaryTarget && candidate == primaryTarget)
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (HasDotAura(candidate) || IsWyvernStung(candidate))
                continue;
            if (IsTargetInvalidByImmunity(player, candidate))
                continue;
            if (wyvernDrGroup != DIMINISHING_NONE && candidate->GetDiminishing(wyvernDrGroup) > DIMINISHING_LEVEL_0)
                continue;

            if (candidate->GetClass() == CLASS_SHAMAN || candidate->GetClass() == CLASS_DRUID || candidate->GetClass() == CLASS_PALADIN)
                preferredTargets.push_back(candidate);
            else
                fallbackTargets.push_back(candidate);
        }

        if (!preferredTargets.empty())
            return preferredTargets[urand(0, preferredTargets.size() - 1)];

        if (!fallbackTargets.empty())
            return fallbackTargets[urand(0, fallbackTargets.size() - 1)];

        return nullptr;
    }

    Unit const* SelectFriendlyCurseTarget(Player const* player, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto hasDispellableCurse = [&](Unit const* target)
        {
            if (!target || !target->IsAlive())
                return false;

            DispelChargesList dispelList;
            target->GetDispellableAuraList(player, (1 << DISPEL_CURSE), dispelList);
            return !dispelList.empty();
        };

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!candidate || candidate == player || !candidate->IsAlive())
                continue;
            if (!IsFriendlySupportTarget(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (!hasDispellableCurse(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        if (best)
            return best;

        return hasDispellableCurse(player) ? player : nullptr;
    }

    Unit const* SelectRogueBlindTarget(Player const* player, Unit const* primaryTarget, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto isPriorityBlindTarget = [&](Unit const* candidate)
        {
            if (!HasHostileTarget(player, candidate))
                return false;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                return false;
            if (IsTargetInvalidByImmunity(player, candidate))
                return false;
            if (!(candidate->GetClass() == CLASS_DRUID || candidate->GetClass() == CLASS_SHAMAN || candidate->GetClass() == CLASS_PALADIN))
                return false;
            if (HasAnyAura(candidate, { 2893 })) // Abolish Poison
                return false;
            if (HasBreakableCrowdControl(player, candidate) || HasDotAura(candidate))
                return false;
            return true;
        };

        Unit const* bestSecondary = nullptr;
        float bestSecondaryDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!isPriorityBlindTarget(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (primaryTarget && candidate->GetGUID() == primaryTarget->GetGUID())
                continue;

            if (distance < bestSecondaryDistance)
            {
                bestSecondary = candidate;
                bestSecondaryDistance = distance;
            }
        }

        return bestSecondary;
    }

    Unit const* SelectWarlockFearTarget(Player const* player, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto hasFearFromPlayer = [&](Player const* candidate)
        {
            if (!candidate)
                return false;

            Unit::AuraEffectList const& fearAuras = candidate->GetAuraEffectsByType(SPELL_AURA_MOD_FEAR);
            for (AuraEffect const* auraEffect : fearAuras)
            {
                if (!auraEffect)
                    continue;
                if (auraEffect->GetCasterGUID() == player->GetGUID())
                    return true;
            }

            return false;
        };

        SpellInfo const* fearInfo = sSpellMgr->GetSpellInfo(6215);
        DiminishingGroup const fearDrGroup = fearInfo ? fearInfo->GetDiminishingReturnsGroupForSpell(false) : DIMINISHING_NONE;

        auto isFearInvalidTarget = [&](Player const* candidate)
        {
            if (!candidate)
                return true;

            if (fearInfo && candidate->IsImmunedToSpell(fearInfo, player))
                return true;

            if (fearDrGroup != DIMINISHING_NONE && candidate->GetDiminishing(fearDrGroup) > DIMINISHING_LEVEL_0)
                return true;

            return false;
        };

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (hasFearFromPlayer(candidate))
                return nullptr;
        }

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        std::vector<Unit const*> fallbackCandidates;
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (IsTargetInvalidByImmunity(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (isFearInvalidTarget(candidate))
                continue;

            if (!(candidate->GetClass() == CLASS_PALADIN || candidate->GetClass() == CLASS_PRIEST))
            {
                fallbackCandidates.push_back(candidate);
                continue;
            }

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        if (best)
            return best;

        if (fallbackCandidates.empty())
            return nullptr;

        return fallbackCandidates[urand(0u, static_cast<uint32>(fallbackCandidates.size() - 1))];
    }

    Unit const* SelectEnemyClassTarget(Player const* player, uint8 classId, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (candidate->GetClass() != classId)
                continue;
            if (IsTargetInvalidByImmunity(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }

    // Health as a healer should rank it. A flag carrier counts as twenty points
    // more hurt than it is: the whole match rides on keeping that one player
    // up. Whether it needs healing at all still reads the real health.
    float GetHealTriageHealthPct(Unit const* candidate)
    {
        constexpr float kFlagCarrierTriageBonusPct = 20.0f;
        float const healthPct = candidate->GetHealthPct();
        Player const* candidatePlayer = candidate->ToPlayer();
        return candidatePlayer && playerbot::PvpCore::IsBattlegroundFlagCarrier(candidatePlayer)
            ? healthPct - kFlagCarrierTriageBonusPct
            : healthPct;
    }

    Unit const* SelectFriendlyHealthTarget(Player const* player, float maxDistance, float maxHealthPct, uint32 excludedAuraId = 0)
    {
        if (!player || !player->FindMap())
            return nullptr;

        Unit const* best = nullptr;
        Unit const* selfCandidate = nullptr;
        float bestHealth = 101.0f;
        float bestDistance = std::numeric_limits<float>::max();

        auto evaluateCandidate = [&](Unit const* candidate)
        {
            if (!candidate || !candidate->IsAlive())
                return;
            if (!IsFriendlySupportTarget(player, candidate))
                return;
            if (excludedAuraId && candidate->HasAura(excludedAuraId))
                return;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                return;

            float const healthPct = candidate->GetHealthPct();
            if (healthPct > maxHealthPct)
                return;

            float const distance = player->GetDistance(candidate);
            if (candidate == player)
            {
                selfCandidate = player;
                return;
            }

            float const triageHealth = GetHealTriageHealthPct(candidate);
            if (triageHealth < bestHealth || (std::abs(triageHealth - bestHealth) < 0.1f && distance < bestDistance))
            {
                best = candidate;
                bestHealth = triageHealth;
                bestDistance = distance;
            }
        };

        evaluateCandidate(player);

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            evaluateCandidate(itr->GetSource());

        return best ? best : selfCandidate;
    }

    // Defined further down with the other range helpers; needed by the ally
    // target search below it.
    bool CanUseHealRangeSpacing(uint8 classId);

    Unit const* SelectFriendlyLowestHealthTarget(Player const* player, float maxDistance, float maxHealthPct, uint32 excludedAuraId = 0, bool includeSelf = true)
    {
        if (!player || !player->FindMap())
            return nullptr;

        Unit const* best = nullptr;
        float bestHealth = 101.0f;
        float bestDistance = std::numeric_limits<float>::max();

        auto evaluateCandidate = [&](Unit const* candidate)
        {
            if (!candidate || !candidate->IsAlive())
                return;
            if (!includeSelf && candidate == player)
                return;
            if (!IsFriendlySupportTarget(player, candidate))
                return;
            if (excludedAuraId && candidate->HasAura(excludedAuraId))
                return;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                return;

            float const healthPct = candidate->GetHealthPct();
            if (healthPct > maxHealthPct)
                return;

            float const distance = player->GetDistance(candidate);
            float const triageHealth = GetHealTriageHealthPct(candidate);
            if (triageHealth < bestHealth || (std::abs(triageHealth - bestHealth) < 0.1f && distance < bestDistance))
            {
                best = candidate;
                bestHealth = triageHealth;
                bestDistance = distance;
            }
        };

        evaluateCandidate(player);

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            evaluateCandidate(itr->GetSource());

        return best;
    }

// Finds the ally with the greatest distance strictly inside the supplied range,
// i.e. the ally closest to the range cap without violating the spell's minimum.
// Used by escape/leap abilities that need a valid friendly destination.
Unit const* SelectFriendlyNearRangeCapTarget(Player const* player, float minDistance, float maxDistance)
{
    if (!player || !player->FindMap())
        return nullptr;

    Unit const* best = nullptr;
    float bestDistance = -1.0f;

    auto evaluateCandidate = [&](Unit const* candidate)
    {
        if (!candidate || !candidate->IsAlive() || candidate == player)
            return;
        if (!IsFriendlySupportTarget(player, candidate))
            return;

        float const distance = player->GetDistance(candidate);
        if (distance <= minDistance || distance >= maxDistance || !player->IsWithinLOSInMap(candidate))
            return;

        if (distance > bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    };

    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        evaluateCandidate(itr->GetSource());

    return best;
}

Unit const* SelectFriendlyCasterTarget(Player const* player, float maxDistance, float maxHealthPct)
{
    if (!player || !player->FindMap())
        return nullptr;

        Unit const* best = nullptr;
        Unit const* selfCandidate = nullptr;
        float bestHealth = 101.0f;
        float bestDistance = std::numeric_limits<float>::max();

        auto evaluateCandidate = [&](Unit const* candidate)
        {
            if (!candidate || !candidate->IsAlive())
                return;
            if (!IsCasterClass(candidate))
                return;
            if (!IsFriendlySupportTarget(player, candidate))
                return;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                return;

            float const healthPct = candidate->GetHealthPct();
            if (healthPct > maxHealthPct)
                return;

            float const distance = player->GetDistance(candidate);
            if (candidate == player)
            {
                selfCandidate = player;
                return;
            }

            if (healthPct < bestHealth || (std::abs(healthPct - bestHealth) < 0.1f && distance < bestDistance))
            {
                best = candidate;
                bestHealth = healthPct;
                bestDistance = distance;
            }
        };

        evaluateCandidate(player);

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            evaluateCandidate(itr->GetSource());

        return best ? best : selfCandidate;
    }

    Unit const* SelectFriendlyDispelTarget(Player const* player, DispelType dispelType, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto hasDispellableAura = [&](Unit const* target)
        {
            if (!target || !target->IsAlive())
                return false;

            DispelChargesList dispelList;
            target->GetDispellableAuraList(player, (1 << dispelType), dispelList);
            return !dispelList.empty();
        };

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!candidate || candidate == player || !candidate->IsAlive())
                continue;
            if (!IsFriendlySupportTarget(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (!hasDispellableAura(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        if (best)
            return best;

        return hasDispellableAura(player) ? player : nullptr;
    }

    Unit const* SelectEnemyNonBreakableCrowdControlTarget(Player const* player, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        uint32 constexpr mechanicMask =
            (1 << MECHANIC_ROOT) |
            (1 << MECHANIC_STUN) |
            (1 << MECHANIC_FREEZE) |
            (1 << MECHANIC_SNARE);

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (IsTargetInvalidByImmunity(player, candidate))
                continue;
            if (!candidate->HasAuraWithMechanic(mechanicMask))
                continue;
            if (HasBreakableCrowdControl(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }

    Unit const* SelectEnemyDispelTarget(Player const* player, DispelType dispelType, Unit const* preferredTarget, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto hasDispellableAura = [&](Unit const* target)
        {
            if (!target || !target->IsAlive())
                return false;
            if (!HasHostileTarget(player, target))
                return false;
            if (!player->IsWithinDistInMap(target, maxDistance) || !player->IsWithinLOSInMap(target))
                return false;

            DispelChargesList dispelList;
            target->GetDispellableAuraList(player, (1 << dispelType), dispelList);
            for (DispelableAura const& dispelable : dispelList)
            {
                Aura const* aura = dispelable.GetAura();
                if (!aura)
                    continue;

                // Sweeping Strikes should never be a valid offensive-dispel target
                // for playerbots, even if external spell data marks it dispellable.
                if (HasAuraFromSpellChain(target, 12328) && HasAuraFromSpellChain(target, aura->GetId()))
                    continue;

                return true;
            }

            return false;
        };

        if (hasDispellableAura(preferredTarget))
            return preferredTarget;

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!hasDispellableAura(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }

    Unit const* SelectFriendlyLowManaTarget(Player const* player, float maxDistance, float maxManaPct)
    {
        if (!player || !player->FindMap())
            return nullptr;

        Unit const* best = nullptr;
        float bestMana = 101.0f;
        float bestDistance = std::numeric_limits<float>::max();

        auto evaluateCandidate = [&](Unit const* candidate)
        {
            if (!candidate || !candidate->IsAlive())
                return;
            if (!IsFriendlySupportTarget(player, candidate))
                return;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                return;
            if (candidate->GetMaxPower(POWER_MANA) <= 0)
                return;

            float const manaPct = candidate->GetPowerPct(POWER_MANA);
            if (manaPct > maxManaPct)
                return;

            float const distance = player->GetDistance(candidate);
            if (manaPct < bestMana || (std::abs(manaPct - bestMana) < 0.1f && distance < bestDistance))
            {
                best = candidate;
                bestMana = manaPct;
                bestDistance = distance;
            }
        };

        evaluateCandidate(player);
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            evaluateCandidate(itr->GetSource());

        return best;
    }

    Unit const* SelectFriendlySnaredTarget(Player const* player, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto isSnared = [](Unit const* target)
        {
            if (!target || target->HasStealthAura())
                return false;

            return target && (target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) || target->HasAuraWithMechanic(1 << MECHANIC_ROOT));
        };

        if (isSnared(player))
            return player;

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!candidate || !candidate->IsAlive())
                continue;
            if (!IsFriendlySupportTarget(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (!isSnared(candidate))
                continue;

            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }


    bool IsStealthMovementPenalty(SpellInfo const* spellInfo)
    {
        return spellInfo && spellInfo->HasAura(SPELL_AURA_MOD_STEALTH);
    }

    bool HasNonStealthDecreaseSpeedAura(Unit const* unit)
    {
        Unit::AuraEffectList const& slowAuras = unit->GetAuraEffectsByType(SPELL_AURA_MOD_DECREASE_SPEED);
        for (AuraEffect const* slowAura : slowAuras)
            if (slowAura && !IsStealthMovementPenalty(slowAura->GetSpellInfo()))
                return true;

        return false;
    }

    bool HasNonStealthRootOrSnareMechanic(Unit const* unit)
    {
        uint32 const rootOrSnareMask = (1 << MECHANIC_ROOT) | (1 << MECHANIC_SNARE);

        for (Unit::AuraApplicationMap::value_type const& appliedAura : unit->GetAppliedAuras())
        {
            AuraApplication const* aurApp = appliedAura.second;
            SpellInfo const* spellInfo = aurApp ? aurApp->GetBase()->GetSpellInfo() : nullptr;
            if (!spellInfo || IsStealthMovementPenalty(spellInfo))
                continue;

            if (spellInfo->Mechanic && (rootOrSnareMask & (1 << spellInfo->Mechanic)))
                return true;

            for (SpellEffectInfo const& spellEffectInfo : spellInfo->GetEffects())
                if (aurApp->HasEffect(spellEffectInfo.EffectIndex) && spellEffectInfo.IsEffect() && spellEffectInfo.Mechanic &&
                    (rootOrSnareMask & (1 << spellEffectInfo.Mechanic)))
                    return true;
        }

        return false;
    }

    bool IsRootedOrSnared(Unit const* unit)
    {
        if (!unit)
            return false;

        return unit->HasUnitState(UNIT_STATE_ROOT) ||
            unit->HasAuraType(SPELL_AURA_MOD_ROOT) ||
            HasNonStealthDecreaseSpeedAura(unit) ||
            HasNonStealthRootOrSnareMechanic(unit);
    }


    bool IsMageBlinkableControl(Player const* player)
    {
        if (!player || player->GetClass() != CLASS_MAGE)
            return false;

        constexpr uint32 blinkableMechanicMask =
            (1u << MECHANIC_STUN) |
            (1u << MECHANIC_ROOT);

        constexpr uint32 nonBlinkableControlMask =
            (1u << MECHANIC_CHARM) |
            (1u << MECHANIC_DISORIENTED) |
            (1u << MECHANIC_FEAR) |
            (1u << MECHANIC_SLEEP) |
            (1u << MECHANIC_FREEZE) |
            (1u << MECHANIC_POLYMORPH) |
            (1u << MECHANIC_BANISH) |
            (1u << MECHANIC_HORROR) |
            (1u << MECHANIC_SAPPED);

        bool const blinkableControl =
            player->HasUnitState(UNIT_STATE_STUNNED) ||
            player->HasUnitState(UNIT_STATE_ROOT) ||
            player->HasAuraType(SPELL_AURA_MOD_ROOT) ||
            player->HasAuraWithMechanic(blinkableMechanicMask);

        if (!blinkableControl)
            return false;

        bool const hardNonBlinkableControl =
            player->HasUnitState(UNIT_STATE_CONFUSED) ||
            player->HasUnitState(UNIT_STATE_FLEEING) ||
            player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
            player->HasAuraWithMechanic(nonBlinkableControlMask) ||
            player->IsPolymorphed();

        return !hardNonBlinkableControl;
    }

bool IsHunterBestialWrathBreakableControl(Player const* player)
{
    if (!player || player->GetClass() != CLASS_HUNTER ||
        !player->HasTalent(81300, player->GetActiveSpec()))
        return false;

    // Only genuine incapacitating crowd control -- stun/fear/confuse/polymorph
    // and similar states that stop the bot from acting at all -- should burn
    // this. IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK is the real
    // PvP-trinket mechanic mask, so it (correctly, for a trinket) also
    // includes MECHANIC_ROOT and MECHANIC_SNARE; combined with the separate
    // IsRootedOrSnared() check below, a mere root or snare counted as
    // "breakable control" here. Arena Preparation itself applies a root to
    // hold players in place before the gates open, so this fired Bestial
    // Wrath the instant the match started rather than only during real CC.
    return player->HasUnitState(UNIT_STATE_CONTROLLED | UNIT_STATE_POSSESSED | UNIT_STATE_STUNNED |
            UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) ||
        player->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
        player->IsPolymorphed();
}

bool HasShieldEquipped(Player const* player)
{
    if (!player)
        return false;

        Item const* offHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        return offHand && offHand->GetTemplate() && offHand->GetTemplate()->InventoryType == INVTYPE_SHIELD;
    }

    Unit const* SelectFriendlyMeleePressureTarget(Player const* player, float maxDistance, float maxHealthPct)
    {
        if (!player || !player->FindMap())
            return nullptr;

        Unit const* best = nullptr;
        float bestHealth = 101.0f;
        auto evaluateCandidate = [&](Unit const* candidate)
        {
            if (!candidate || !candidate->IsAlive() || candidate->GetHealthPct() > maxHealthPct)
                return;
            if (!IsFriendlySupportTarget(player, candidate))
                return;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                return;

            bool underMeleePressure = false;
            Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
            for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            {
                Player* enemy = itr->GetSource();
                if (!HasHostileTarget(player, enemy) || !IsMeleeClass(enemy))
                    continue;
                if (enemy->IsWithinMeleeRange(candidate))
                {
                    underMeleePressure = true;
                    break;
                }
            }

            if (!underMeleePressure)
                return;

            if (candidate->GetHealthPct() < bestHealth)
            {
                best = candidate;
                bestHealth = candidate->GetHealthPct();
            }
        };

        evaluateCandidate(player);
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            evaluateCandidate(itr->GetSource());

        return best;
    }

    Unit const* SelectStunnedEnemyTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        auto usable = [&](Unit const* candidate)
        {
            return HasHostileTarget(player, candidate) &&
                !IsTargetInvalidByImmunity(player, candidate) &&
                candidate->HasAuraWithMechanic(1 << MECHANIC_STUN) &&
                player->IsWithinLOSInMap(candidate) &&
                player->IsWithinDistInMap(candidate, maxDistance);
        };

        if (usable(preferredTarget))
            return preferredTarget;

        Unit const* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!usable(candidate))
                continue;
            float const distance = player->GetDistance(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }

    Unit const* SelectUnstunDREnemyTarget(Player const* player, Unit const* preferredTarget, float maxDistance, uint32 stunSpellId)
    {
        if (!player || !player->FindMap())
            return nullptr;

        SpellInfo const* stunInfo = sSpellMgr->GetSpellInfo(stunSpellId);
        DiminishingGroup const stunDrGroup = stunInfo ? stunInfo->GetDiminishingReturnsGroupForSpell(false) : DIMINISHING_NONE;
        auto usable = [&](Unit const* candidate)
        {
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !HasHostileTarget(player, candidate) || !player->IsWithinLOSInMap(candidate))
                return false;
            if (IsTargetInvalidByImmunity(player, candidate))
                return false;
            return stunDrGroup == DIMINISHING_NONE || candidate->GetDiminishing(stunDrGroup) == DIMINISHING_LEVEL_0;
        };

        if (usable(preferredTarget))
            return preferredTarget;

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
            if (usable(itr->GetSource()))
                return itr->GetSource();

        return nullptr;
    }

    Unit const* SelectPredatorsSwiftnessRootTarget(Player const* player, Unit const* preferredTarget, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        Unit const* bestMelee = nullptr;
        Unit const* bestFallback = nullptr;
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !HasHostileTarget(player, candidate) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (IsTargetInvalidByImmunity(player, candidate) || HasAuraFromSpellChain(candidate, 1044))
                continue;
            if (IsMeleeClass(candidate))
                bestMelee = candidate;
            else if (!bestFallback)
                bestFallback = candidate;
        }

        if (bestMelee)
            return bestMelee;
        return bestFallback ? bestFallback : preferredTarget;
    }

    bool AllFriendlyPlayersHealthy(Player const* player, float maxDistance, float minHealthPct)
    {
        if (!player || !player->FindMap())
            return true;

        if (player->GetHealthPct() < minHealthPct)
            return false;

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!candidate || !candidate->IsAlive() || !IsFriendlySupportTarget(player, candidate))
                continue;
            if (player->IsWithinDistInMap(candidate, maxDistance) && player->IsWithinLOSInMap(candidate) && candidate->GetHealthPct() < minHealthPct)
                return false;
        }

        return true;
    }

    Unit const* SelectFriendlyMissingBuffTarget(Player const* player, uint32 baseSpellId, float maxDistance)
    {
        if (!player || !player->FindMap())
            return nullptr;

        std::vector<Unit const*> candidates;
        if (!HasAuraFromSpellChain(player, baseSpellId))
            candidates.push_back(player);

        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!candidate || !candidate->IsAlive() || !IsFriendlySupportTarget(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            if (HasAuraFromSpellChain(candidate, baseSpellId))
                continue;
            candidates.push_back(candidate);
        }

        if (candidates.empty())
            return nullptr;

        return candidates[urand(0, candidates.size() - 1)];
    }


bool HasActiveMovementEffectSpline(Player const* player)
{
    if (!player)
        return false;

    MotionMaster const* motionMaster = player->GetMotionMaster();
    if (!motionMaster || motionMaster->GetCurrentMovementGeneratorType() != EFFECT_MOTION_TYPE)
        return false;

    bool const hasActiveSpline = player->movespline && player->movespline->Initialized() && !player->movespline->Finalized();
    return hasActiveSpline || player->HasUnitState(UNIT_STATE_CHARGING) || player->isMoving();
}

uint32 CountNearbyEnemies(Player const* player, float maxDistance)
{
    if (!player || !player->FindMap())
        return 0;

        uint32 count = 0;
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate))
                continue;
            if (IsTargetInvalidByImmunity(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            ++count;
        }

        return count;
    }

    uint32 CountNearbyMeleeThreats(Player const* player, float maxDistance)
    {
        if (!player || !player->FindMap())
            return 0;

        uint32 count = 0;
        Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
        {
            Player* candidate = itr->GetSource();
            if (!HasHostileTarget(player, candidate) || !IsMeleeClass(candidate))
                continue;
            if (IsTargetInvalidByImmunity(player, candidate))
                continue;
            if (!player->IsWithinDistInMap(candidate, maxDistance) || !player->IsWithinLOSInMap(candidate))
                continue;
            ++count;
        }

        return count;
    }

uint32 CountNearbyFriendlyPlayers(Player const* player, float maxDistance, bool includeSelf = true, bool requireLineOfSight = true)
{
    if (!player || !player->FindMap())
        return 0;

    uint32 count = 0;
    Map::PlayerList const& mapPlayers = player->FindMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = mapPlayers.begin(); itr != mapPlayers.end(); ++itr)
    {
        Player* candidate = itr->GetSource();
        if (!candidate || !candidate->IsAlive())
            continue;
        if (!includeSelf && candidate == player)
            continue;
        if (!IsFriendlySupportTarget(player, candidate))
            continue;
        if ((requireLineOfSight && !player->IsWithinLOSInMap(candidate)) || !player->IsWithinDistInMap(candidate, maxDistance))
            continue;
        ++count;
    }

    return count;
}

ObjectGuid SelectCombatTargetGuid(Player const* player)
{
    if (!player)
        return ObjectGuid::Empty;

    if (ObjectGuid const selectedGuid = player->GetTarget(); !selectedGuid.IsEmpty())
        if (Unit const* selectedTarget = ObjectAccessor::GetUnit(*player, selectedGuid); HasHostileTarget(player, selectedTarget) && !IsTargetInvalidByImmunity(player, selectedTarget))
            return selectedGuid;

        return ObjectGuid::Empty;
    }

    ObjectGuid SelectAllyTargetGuid(Player const* player)
    {
        if (!player)
            return ObjectGuid::Empty;

        ObjectGuid const selectedGuid = player->GetTarget();
        if (!selectedGuid.IsEmpty() && selectedGuid != player->GetGUID())
        {
            Unit const* selected = ObjectAccessor::GetUnit(*player, selectedGuid);
            if (selected && selected->IsAlive() && IsFriendlySupportTarget(player, selected) &&
                player->IsWithinLOSInMap(selected))
                return selectedGuid;
        }

        // Nothing friendly under the cursor, so go and LOOK for somebody hurt.
        //
        // Reading the current selection was the whole of this function, and a bot
        // in a fight has its enemy selected - so the answer was "nobody", always,
        // and every caller that keys off an ally target behaved as if the bot were
        // alone. That is fine in a battleground, where a healer is pointed at its
        // team; it is useless in the open world, where nothing ever points a bot
        // at a friend.
        //
        // Only for the classes that can actually do something about it. This runs
        // on the decision tick for every bot on the realm, and the scan walks the
        // map's player list - there is no reason for a rogue to pay for it.
        if (!CanUseHealRangeSpacing(player->GetClass()))
            return ObjectGuid::Empty;

        // Lowest health first, self excluded: a bot that heals itself does not
        // need an ally target to do it, and letting self win here would mask the
        // wounded friend standing next to it.
        if (Unit const* wounded = SelectFriendlyLowestHealthTarget(player,
                GetConfiguredHealRange(), 90.0f, 0, false))
            return wounded->GetGUID();

        return ObjectGuid::Empty;
    }

    SpellDecision SelectHunterSpell(Player const* player, Unit const* target, bool inMelee, ClassicProfileSelection const& profileSelection)
    {
        SpellDecision decision;
        if (!player)
            return decision;

        Unit const* activeTarget = target;

        if (!HasHostileTarget(player, activeTarget))
            return decision;

        UpdateHunterCombatMode(player, activeTarget);

        // Exact 5-8y is the classic hunter dead-zone: melee cannot reach, but
        // ranged weapon shots also fail. Still allow Feign Death -> trap setup here;
        // otherwise the movement layer owns the high-priority retreat directive.
        Unit const* activeDeadZoneTarget = SelectHunterDeadZoneEnemy(player, activeTarget);
        bool const activeTargetDeadZone = activeDeadZoneTarget != nullptr;

        HunterPetDecisionState const petState = GetHunterPetDecisionState(player);
        bool const hasLivingPet = petState.hasLivingPet;
        bool const hasDeadPet = petState.hasDeadPet || petState.shouldRevivePet;
        bool const canCallPet = !hasLivingPet && !hasDeadPet && petState.canCallPet &&
            !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kHunterCallPetSpellId);
        bool const shouldRevivePet = hasDeadPet &&
            !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kHunterRevivePetSpellId);

    Unit const* enemyOnTopTarget = SelectNearbyEnemyTarget(player, activeTarget, 5.0f);
    Unit const* nearbyCastingTarget = SelectEnemyCastingTarget(player, 20.0f, activeTarget);
    Unit const* scatterPointBlankTarget = IsSpellReady(player, 19503) ?
        SelectEnemyCastingTarget(player, 5.0f, enemyOnTopTarget) : nullptr;
    Unit const* closeMeleeThreat = SelectNearbyMeleeTarget(player, enemyOnTopTarget, 5.0f);
    Unit const* trapSetupTarget = SelectNearbyEnemyTarget(player, enemyOnTopTarget ? enemyOnTopTarget : activeTarget, 8.0f);
    Unit const* rogueTarget = SelectEnemyClassTarget(player, CLASS_ROGUE, GetConfiguredLongRange());
    HunterPvpSpec const hunterSpec = GetHunterPvpSpec(profileSelection);
    bool const isSurvivalHunter = hunterSpec == HunterPvpSpec::Survival;
    bool const isMarksmanshipHunter = hunterSpec == HunterPvpSpec::Marksmanship;
    bool const isBeastMasteryHunter = hunterSpec == HunterPvpSpec::BeastMastery;
    Pet* bmPet = isBeastMasteryHunter ? player->GetPet() : nullptr;
    bool const bmPetAttacking = bmPet && bmPet->IsAlive() && bmPet->GetVictim();
    float const bmPetDistance = bmPet && bmPet->IsAlive() ? player->GetDistance(bmPet) : -1.0f;
    bool const bmPetAtUsefulSwapPosition = bmPetDistance >= 8.0f && bmPetDistance <= 30.0f &&
        player->IsWithinLOSInMap(bmPet) &&
        std::abs(player->GetPositionZ() - bmPet->GetPositionZ()) <= 20.0f;
    // Outmaneuver (81297) has a MINIMUM range of 8 yd (SpellRange 163: 8-30),
    // so the pet must be that far out for the swap to be castable at all. A BM
    // pet normally sits on whatever is beating on the hunter - inside 8 yd -
    // which is why pairing "enemy in melee on me" with "pet 8-30 yd away"
    // practically never came true and the bot never got the spell off.
    // Require instead that the pet is busy with a DIFFERENT enemy than the one
    // pressuring us: only then is the pet standing somewhere worth porting to,
    // rather than next to the very enemy we are trying to escape. The distance
    // that matters is the PET's (already checked above, and it is what the
    // spell actually ranges against) - where its victim happens to stand is
    // irrelevant.
    Unit const* bmPetVictim = bmPet && bmPet->IsAlive() ? bmPet->GetVictim() : nullptr;
    bool const bmPetOnDifferentEnemy = bmPetVictim && bmPetVictim != enemyOnTopTarget;
    bool const bmCrowdControlled = isBeastMasteryHunter && IsHunterBestialWrathBreakableControl(player);
    bool const hasMongooseBite = ResolveKnownPlayerSpellInChain(player, 81285) != 0;
    bool const hasBitePrimerOnKillTarget = hasMongooseBite && activeTarget &&
        HasHunterDamagingStingFromCaster(activeTarget, player->GetGUID());
    // Must also require melee range: this flag suppresses every ranged
    // filler below (Concussive Shot, Arcane Shot, Multi-Shot, Viper Sting)
    // whenever it is true. Without the range check it would go true purely
    // from having a Sting up, making the hunter hold all ranged pressure for
    // a bite it had not reached yet. The movement layer closes whenever the
    // learned bite is ready; fillers remain available until melee is reached.
    bool const readyToBiteKillTarget = hasBitePrimerOnKillTarget && !IsRootedOrSnared(player) &&
        activeTarget && player->IsWithinMeleeRange(activeTarget);
    // Outmaneuver is an opener as well as an escape. When the pet is already
    // in melee with the kill target and the hunter is not, swapping places
    // lands the hunter in melee range - which is exactly what Mongoose Bite
    // needs. Cheaper and instant compared to running the gap down.
    bool const bmSwapToReachBite = isBeastMasteryHunter && hasMongooseBite && activeTarget &&
        IsSpellReady(player, 81285) && bmPetVictim == activeTarget &&
        !player->IsWithinMeleeRange(activeTarget);
    Unit const* manaTarget = isSurvivalHunter
        ? SelectNearbyEnemyManaTarget(player, activeTarget, GetConfiguredLongRange(), 0.0f)
        : SelectNearbyEnemyTarget(player, activeTarget, GetConfiguredLongRange());
    Unit const* wyvernTarget = (isSurvivalHunter && IsSpellReady(player, 24133) && !AnyEnemyWyvernStung(player, 40.0f))
        ? SelectWyvernStingTarget(player, activeTarget, 30.0f)
        : nullptr;

    target = activeTarget;
    bool const targetBreakableCrowdControl = target && HasBreakableCrowdControl(player, target);
    bool const targetClose = player->IsWithinDistInMap(target, kReferenceHunterSwitchDistance);
    bool const enemyOnTop = HasHostileTarget(player, enemyOnTopTarget);
    bool const trapSetupThreat = HasHostileTarget(player, trapSetupTarget);
    bool const enemyNear = player->IsWithinDistInMap(target, GetConfiguredCloseRange());
    bool const rangedMode = IsHunterInRangedMode(player);
    // Freezing Trap at whatever rank this hunter has trained, not rank three.
    // Naming 14311 restricted the entire behaviour to level fifty and above:
    // on the live fleet that was one hunter in thirty-five, so the other
    // thirty-four feigned, found no trap they were allowed to cast, and sat
    // down to eat instead - which is precisely what it looked like from the
    // outside.
    uint32 const freezingTrapSpellId = HighestKnownRankInChain(player, 1499);
    uint32 const frostTrapSpellId = HighestKnownRankInChain(player, 13809);

    // A damage-over-time effect breaks a Freezing Trap the moment it ticks, so
    // a dotted target gets Frost Trap instead. Either way fall back to the one
    // the hunter actually knows rather than casting nothing.
    uint32 preferredTrapSpellId = trapSetupTarget && HasDotAura(trapSetupTarget)
        ? frostTrapSpellId : freezingTrapSpellId;
    if (!preferredTrapSpellId)
        preferredTrapSpellId = freezingTrapSpellId ? freezingTrapSpellId : frostTrapSpellId;

    bool const preferredTrapReady = preferredTrapSpellId && IsSpellReady(player, preferredTrapSpellId);
    // Traps are only legal out of combat. Feign Death is allowed as a quick
    // defensive/trap-setup attempt, but it must never put hunter movement into a
    // "wait for trap" state. If combat does not actually drop, the next tick
    // should immediately fall through to Wing Clip / Counterattack / flee / shots.
    // Feign-to-set-a-trap is a DUEL opener: drop combat, lay the trap, re-open
    // on your terms. Against a creature it just sends the mob home, and since
    // this candidate carries the highest priority of any hunter decision and
    // asks nothing about health, a PvE hunter feigned the instant anything
    // touched it - pull, play dead, mob walks away, pull again. Bailing out of
    // a creature fight is the PvE manager's business, and it only does it when
    // the fight is actually lost.
    // Feign the trap opener only against a PERSON. The rule was written as
    // "not while PvE engaged", but that registry means "the PvE manager has a
    // target", and the PvE manager engages whatever is being fought - a player
    // included, since its attacker branch resolves an attacking pet to its
    // owner and fights the owner. So a hunter attacked by a player was flagged
    // PvE engaged and the trap opener was suppressed for the whole fight,
    // leaving only the PvE manager's low-health escape hatch: feign, stand up
    // out of combat, sit down and eat. Which is exactly what it looked like.
    //
    // Asking about the target directly keeps the behaviour the old check was
    // reaching for - no feigning at creatures, which only sends them home and
    // restarts the pull - without catching player fights in the same net.
    bool const trapSetupTargetIsPlayer = trapSetupTarget &&
        trapSetupTarget->GetTypeId() == TYPEID_PLAYER;
    bool const canFeignUnderPressure = trapSetupThreat && player->IsInCombat() &&
        trapSetupTargetIsPlayer &&
        IsSpellReady(player, 5384) && !HasAuraFromSpellChain(player, 5384);
    bool const canDropTrapNow = trapSetupThreat && trapSetupTargetIsPlayer &&
        !player->IsInCombat() && preferredTrapReady;

        bool const targetSnaredOrStunned = target &&
            (target->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
                target->HasAuraWithMechanic((1 << MECHANIC_ROOT) | (1 << MECHANIC_STUN)));

    // Hunters hold the T2 wrapper chain (90460-90471), not the stock ranks it
    // casts (WrappedSpells.h); the stock fallback covers a character the
    // startup move missed.
    uint32 const serpentStingSpellId = ResolveKnownPlayerSpellInChain(player, 90468) ? 90468 : 25295;
    // Marksmanship is also where an untalented hunter lands, and its own
    // shots (Aimed Shot is a talent, Rapid Fire 26) arrive late. Until Aimed
    // Shot is learned it shoots with the shared kit like the other specs.
    bool const usesSharedShotKit = isSurvivalHunter || isBeastMasteryHunter ||
        (isMarksmanshipHunter && !ResolveKnownPlayerSpellInChain(player, 20904));
    // Raptor Strike (1) is the melee every hunter owns; Mongoose Bite is the
    // custom 81285 that only beast mastery learns. Without Raptor Strike a
    // hunter in melee had nothing but one Wing Clip.
    Unit const* hunterMeleeTarget = activeTarget && player->IsWithinMeleeRange(activeTarget) ? activeTarget :
        (enemyOnTopTarget && player->IsWithinMeleeRange(enemyOnTopTarget) ? enemyOnTopTarget : nullptr);

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, player->HealthBelowPct(35) && IsSpellReady(player, 19263), 35.0f,
        { "hunter deterrence", "defensive cooldown under sustained melee pressure", 19263, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, canFeignUnderPressure, 36.0f,
        { "hunter feign death", "drop combat under melee/dead-zone pressure", 5384, playerbot::PvpClassSpellContext::TargetMode::Self, trapSetupTarget ? trapSetupTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, canDropTrapNow, 35.75f,
        { preferredTrapSpellId == frostTrapSpellId ? "hunter frost trap" : "hunter freezing trap", "drop trap after feign death setup", preferredTrapSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, trapSetupTarget ? trapSetupTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isSurvivalHunter && IsSpellReady(player, 23989) && !HasAuraFromSpellChain(player, 19263) && !IsSpellReady(player, 19263), 34.0f,
        { "hunter readiness", "reset cooldowns after deterrence has fallen", 23989, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isSurvivalHunter && enemyOnTop && enemyOnTopTarget && player->IsWithinMeleeRange(enemyOnTopTarget) && IsSpellReadyAndCasterAuraAllowed(player, 20910), 34.5f,
        { "hunter counterattack", "strike any enemy in melee range after a parry", 20910, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isSurvivalHunter && wyvernTarget && IsSpellReady(player, 24133), 30.0f,
        { "hunter wyvern sting", "crowd-control a non-dotted enemy support target", 24133, playerbot::PvpClassSpellContext::TargetMode::Enemy, wyvernTarget ? wyvernTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, 14325) && IsSpellReady(player, 14325), 29.5f,
        { "hunter mark", "mark rogue targets for anti-stealth pressure", 14325, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && !HasAuraFromSpellChain(player, 20906) && IsSpellReady(player, 20906), 27.5f,
        { "hunter trueshot aura", "maintain personal buff aura", 20906, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, canCallPet && IsSpellReady(player, kHunterCallPetSpellId), 26.0f,
        { "hunter call pet", "summon active stable pet when no living pet is present", kHunterCallPetSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, shouldRevivePet && IsSpellReady(player, kHunterRevivePetSpellId), 25.0f,
        { "hunter revive pet", "revive dead hunter pet instead of repeatedly calling it", kHunterRevivePetSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && scatterPointBlankTarget && !HasBreakableCrowdControl(player, scatterPointBlankTarget), 23.0f,
        { "hunter scatter shot", "scatter interrupt against nearby cast", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, scatterPointBlankTarget ? scatterPointBlankTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && nearbyCastingTarget && !HasBreakableCrowdControl(player, nearbyCastingTarget) && IsSpellReady(player, 19503), 23.0f,
        { "hunter scatter shot", "scatter interrupt against nearby cast", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget ? nearbyCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates,
        enemyOnTop && enemyOnTopTarget && player->IsWithinMeleeRange(enemyOnTopTarget) &&
            IsSpellReady(player, 14268) && !HasAuraFromSpellChain(enemyOnTopTarget, 14268),
        21.0f,
        { "hunter wing clip", "close-range fallback snare", 14268, playerbot::PvpClassSpellContext::TargetMode::Enemy,
            enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !readyToBiteKillTarget && !activeTargetDeadZone && !targetBreakableCrowdControl && !targetClose && !targetSnaredOrStunned && IsSpellReady(player, 5116), 20.0f,
        { "hunter concussive shot", "kite or chase control", 5116, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !activeTargetDeadZone && !targetBreakableCrowdControl && usesSharedShotKit && activeTarget && activeTarget->GetPowerType() != POWER_MANA &&
        !HasHunterStingFromCaster(activeTarget, player->GetGUID()) && IsSpellReady(player, serpentStingSpellId), 19.75f,
        { "hunter serpent sting", "apply ranged dot pressure to non-mana kill target", serpentStingSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !activeTargetDeadZone && rogueTarget && !HasBreakableCrowdControl(player, rogueTarget) && !HasAuraFromSpellChain(rogueTarget, 25295) && IsSpellReady(player, serpentStingSpellId), 19.5f,
        { "hunter serpent sting", "apply ranged dot pressure", serpentStingSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !activeTargetDeadZone && !targetBreakableCrowdControl && isMarksmanshipHunter && rangedMode && !enemyNear && IsSpellReady(player, 20904), 18.0f,
        { "hunter aimed shot", "long cast pressure from range", 20904, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, hasBitePrimerOnKillTarget && activeTarget && player->IsWithinMeleeRange(activeTarget) && IsSpellReady(player, 81285), 27.0f,
        { "hunter mongoose bite", "consume hunter sting for melee burst", 81285, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !activeTargetDeadZone && !targetBreakableCrowdControl && rangedMode && !inMelee &&
            CountNearbyEnemies(player, 15.0f) >= kMassAoeEnemyCountThreshold && IsSpellReady(player, 1510), 22.0f,
        { "hunter volley", "prioritize channeled ranged aoe over single-target shots against a clustered enemy group", 1510, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !readyToBiteKillTarget && !activeTargetDeadZone && !targetBreakableCrowdControl && usesSharedShotKit && rangedMode && !inMelee && activeTarget && IsSpellReady(player, 14287), 17.5f,
        { "hunter arcane shot", "instant pressure on kill target", 14287, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !readyToBiteKillTarget && !activeTargetDeadZone && !targetBreakableCrowdControl && rangedMode && !inMelee && IsSpellReady(player, 25294), 17.0f,
        { "hunter multi-shot", "ranged burst pressure", 25294, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && rangedMode && !inMelee && IsSpellReady(player, 3045), 16.0f,
        { "hunter rapid fire", "burst cooldown while freecasting at range", 3045, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !readyToBiteKillTarget && !activeTargetDeadZone && manaTarget && !HasBreakableCrowdControl(player, manaTarget) && manaTarget->GetPowerType() == POWER_MANA && !HasAuraFromSpellChain(manaTarget, 14280) && IsSpellReady(player, 14280), 15.0f,
        { "hunter viper sting", "drain mana on mana users", 14280, playerbot::PvpClassSpellContext::TargetMode::Enemy, manaTarget ? manaTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isMarksmanshipHunter && enemyOnTop && (!IsSpellReady(player, 5384) || !IsSpellReady(player, 14311)) && IsSpellReady(player, 19503) && !HasBreakableCrowdControl(player, enemyOnTopTarget), 14.0f,
        { "hunter scatter shot", "fallback peel when trap setup unavailable", 19503, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, enemyOnTop && closeMeleeThreat && (isSurvivalHunter || !IsSpellReady(player, 19503)) && (!IsSpellReady(player, 5384) || !preferredTrapReady) && IsSpellReady(player, 19263), 13.0f,
        { "hunter deterrence", "defensive cooldown under sustained melee pressure", 19263, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, bmCrowdControlled && IsSpellReady(player, 81300), 100.0f,
        { "hunter bestial wrath", "break any removable crowd-control effect", 81300, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isBeastMasteryHunter && bmPetAttacking && activeTarget && IsSpellReady(player, 19577), 28.5f,
        { "hunter intimidate", "stun the kill target whenever the pet is attacking", 19577, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    // Two reasons to swap, and they are different situations: get OUT (the pet
    // holds someone else while we are pressured/stuck), or get IN (the pet is
    // on our kill target and we want to be in melee for Mongoose Bite).
    AddDecisionCandidate(candidates, isBeastMasteryHunter && bmPetAtUsefulSwapPosition &&
        ((bmPetOnDifferentEnemy && (enemyOnTop || activeTargetDeadZone || IsRootedOrSnared(player))) ||
            bmSwapToReachBite) && IsSpellReady(player, 81297), 36.5f,
        // TargetMode::Pet, NOT None: the castability check resolves a target
        // per mode and treats None as "no target -> not castable", so this
        // candidate was rejected before any of its conditions were even
        // considered - the real reason the bot never got Outmaneuver off.
        // Spell 81297 targets TARGET_UNIT_PET, which is exactly what Pet mode
        // resolves to.
        { "hunter outmaneuver", "swap out to the pet while it holds a different enemy", 81297, playerbot::PvpClassSpellContext::TargetMode::Pet });
    AddDecisionCandidate(candidates, hasMongooseBite && enemyOnTop && enemyOnTopTarget && player->IsWithinMeleeRange(enemyOnTopTarget) && IsSpellReady(player, 81285), 24.0f,
        { "hunter mongoose bite", "bite the nearest attacker under melee pressure", 81285, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyOnTopTarget ? enemyOnTopTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !hasMongooseBite && hunterMeleeTarget && !HasBreakableCrowdControl(player, hunterMeleeTarget) &&
        IsSpellReady(player, 2973), 20.5f,
        { "hunter raptor strike", "melee attack while an enemy is in melee range", 2973, playerbot::PvpClassSpellContext::TargetMode::Enemy, hunterMeleeTarget ? hunterMeleeTarget->GetGUID() : ObjectGuid::Empty });
    // The old unconditional fallback candidate here (no melee-range check,
    // relying on SelectHighestPriorityCastableDecision's uncastable-decision
    // fallback to force approach movement) is gone: readyToBiteKillTarget
    // now requires melee range itself, which makes it identical to the
    // melee-range-gated candidate above minus the root/snare check, so it
    // never added anything and only forced the bot to path in from any
    // range instead of holding the BM weave profile (see
    // GetCombatPositioningProfile/DriveHunterKiteLoop in
    // PlayerbotPvpLifecycleActions.cpp) and closing to melee naturally.

        return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
    }

    SpellDecision SelectMageSpell(Player const* player, Unit const* target, bool inMelee, ClassicProfileSelection const& profileSelection)
    {
        SpellDecision decision;
        if (!player)
            return decision;

    bool const hasHostileTarget = HasHostileTarget(player, target);
    bool const closePressure = hasHostileTarget && player->IsWithinDistInMap(target, GetConfiguredMeleeRange());
    bool const blinkableControl = IsMageBlinkableControl(player);
    float const manaPct = player->GetPowerPct(POWER_MANA);
    bool const isFireMage = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    bool const isArcaneMage = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
    bool const isFrostMage = !isFireMage && !isArcaneMage;
    // Mage armor by CHAIN, never by a hardcoded rank id. The old entry cast
    // rank id 10220 and gated on HasAura(10220): a level 50 mage knows Ice
    // Armor rank 3 (10219), the executor resolved the cast down to the rank
    // it actually knew, the buff landed as 10219 - and the condition, still
    // asking for 10220, never cleared. So it recast Ice Armor on every single
    // pass, forever. Any rank of either chain now satisfies the slot, exactly
    // as the open-world buff pass already did it.
    uint32 const manaGemItemId = ResolveConjuredItemForKnownRank(player, 10054);
    uint32 mageArmorSpellId = ResolveKnownPlayerSpellInChain(player, 7302); // Ice Armor
    if (!mageArmorSpellId)
        mageArmorSpellId = ResolveKnownPlayerSpellInChain(player, 168);     // else Frost Armor
    bool const mageArmorMissing = !HasAuraFromSpellChain(player, 7302) && !HasAuraFromSpellChain(player, 168);
    Unit const* cursedTarget = IsSpellReady(player, 475) ? SelectFriendlyCurseTarget(player, 40.0f) : nullptr;
    Unit const* castingTarget = IsSpellReady(player, 2139) ? SelectEnemyCastingTarget(player, 30.0f, target) : nullptr;
    Unit const* polymorphTarget =
        (IsSpellReady(player, 12826) && !AnyEnemyPolymorphed(player, 40.0f)) ? SelectPolymorphTarget(player, target, 30.0f) : nullptr;
    // Arcane Power and Presence of Mind are off the global cooldown, so a
    // human player fires all three of these back to back. This engine can
    // only return one decision per Execute() call, so the three steps are
    // sequenced by priority/aura-state instead: each becomes castable only
    // once the previous one has landed. ProcessLifecycleEntryPoint's cadence
    // bypass (triggered on spellId 12042/12043) collapses those separate
    // calls back-to-back within the same overall update instead of waiting
    // out the normal decision cadence between them.
    bool const arcaneBurstWindow = isArcaneMage && target && target->HealthBelowPct(50);
    bool const arcanePowerReady = arcaneBurstWindow && !HasAuraFromSpellChain(player, 12042) && IsSpellReady(player, 12042);
    bool const presenceOfMindReady = arcaneBurstWindow && player->HasAura(12042) && !player->HasAura(12043) && IsSpellReady(player, 12043);
    bool const burstPyroblastReady = arcaneBurstWindow && player->HasAura(12043) && IsSpellReady(player, 18809);
    // The spec's preferred filler, then Frostbolt, then Fireball.
    //
    // Fireball is the floor because it is the one nuke a mage can never be
    // without: it is the level 1 spell. Frostbolt is not learnable until 4
    // and Scorch not until 22, so a chain that stopped at Frostbolt still
    // left the first three levels with no filler and nothing to do but
    // melee - the same hole this rule exists to close, just narrower.
    uint32 const magePreferredNuke = (isFireMage || isArcaneMage) ? uint32(10207) : uint32(25304);
    uint32 mageDefaultNuke = magePreferredNuke;
    if (!IsSpellReady(player, mageDefaultNuke))
        mageDefaultNuke = uint32(25304);      // Frostbolt, learned at 4
    if (!IsSpellReady(player, mageDefaultNuke))
        mageDefaultNuke = uint32(133);        // Fireball, learned at 1

    return SelectFromTriggerGraph(player, target, nullptr,
    {
        { "critical health", !isFireMage && player->HealthBelowPct(25) && IsSpellReady(player, 11958), 60.0f,
            { "mage ice block", "self-preservation emergency", 11958, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "mage is stunned or rooted", blinkableControl && IsSpellReady(player, 1953), 61.0f,
            { "mage blink", "escape stun/root control", 1953, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "arcane burst window", arcanePowerReady, 57.0f,
            { "mage arcane power", "open the sub-50-percent burst window", 12042, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "arcane burst window", presenceOfMindReady, 56.8f,
            { "mage presence of mind", "queue an instant pyroblast during the burst window", 12043, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "arcane burst window", burstPyroblastReady, 56.6f,
            { "mage pyroblast", "instant burst finisher under presence of mind", 18809, playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "enemy too close for spell", closePressure && IsSpellReady(player, 1953), 45.0f,
            { "mage blink", "escape melee pressure", 1953, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "enemy is casting", castingTarget && IsSpellReady(player, 2139), 44.0f,
            { "mage counterspell", "interrupt any enemy cast in range", 2139, playerbot::PvpClassSpellContext::TargetMode::Enemy, castingTarget ? castingTarget->GetGUID() : ObjectGuid::Empty } },
        { "enemy too close for spell", closePressure && IsSpellReady(player, 10230), 43.0f,
            { "mage frost nova", "close defensive peel", 10230, playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "enemy too close for spell", closePressure && target && IsMeleeClass(target) && IsSpellReady(player, isFireMage ? uint32(33041) : uint32(10161)), 42.0f,
            { isFireMage ? "mage dragon's breath" : "mage cone of cold", isFireMage ? "disorient nearby melee pressure" : "defensive snare versus nearby melee", isFireMage ? uint32(33041) : uint32(10161), playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "low mana", manaPct < 25.0f && IsSpellReady(player, 12051), 41.0f,
            { "mage evocation", "recover mana below 25 percent", 12051, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "high mana", manaPct < 50.0f && IsOnUseItemReady(player, kMageManaRubyItemId) &&
            !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kMageManaRubyUseSpellId), 40.0f,
            { "use mana ruby", "consume mana ruby below 50 percent mana", kMageManaRubyUseSpellId, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), kMageManaRubyItemId } },
        { "remove curse", cursedTarget != nullptr, 39.0f,
            { "remove lesser curse", "dispel curse from friendly target", 475, (cursedTarget == player) ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget ? cursedTarget->GetGUID() : ObjectGuid::Empty } },
        { "ice barrier", !isFireMage && !HasAuraFromSpellChain(player, 13033) && IsSpellReady(player, 13033), 35.0f,
            { "mage ice barrier", "maintain defensive absorb shield", 13033, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "mana shield", isArcaneMage && closePressure && !HasAuraFromSpellChain(player, 10193) && IsSpellReady(player, 10193), 34.5f,
            { "mage mana shield", "absorb physical damage while being hit in melee", 10193, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "enemy low health", hasHostileTarget && target && IsSpellReady(player, 10199), 30.0f,
            { "mage fire blast", isFireMage ? "instant fire blast pressure on cooldown" : "instant execute pressure on low health target", 10199, playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "clustered enemies", isFireMage && CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 13021), 29.5f,
            { "mage blast wave", "area fire pressure against nearby enemies", 13021, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "swarmed in melee", closePressure && CountNearbyEnemies(player, 10.0f) >= kMassAoeEnemyCountThreshold && IsSpellReady(player, 1449), 29.7f,
            { "mage arcane explosion", "melee-range aoe burst when surrounded in a large fight", 1449, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "large enemy cluster", isFrostMage && hasHostileTarget && CountNearbyEnemies(player, 15.0f) >= kMassAoeEnemyCountThreshold && IsSpellReady(player, 10), 29.6f,
            { "mage blizzard", "sustained ranged aoe against a clustered enemy group", 10, playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "polymorph", polymorphTarget && !polymorphTarget->HealthBelowPct(75), 29.0f,
            { "mage polymorph", "priority crowd control on non-dotted paladin/priest targets", 12826, playerbot::PvpClassSpellContext::TargetMode::Enemy, polymorphTarget ? polymorphTarget->GetGUID() : ObjectGuid::Empty } },
        // Frostbolt is the FALLBACK, not the frost spec's private spell.
        //
        // This asked fire and arcane mages for Scorch and nobody else for
        // anything, so a mage who does not know Scorch had no filler at all -
        // and Scorch is not learnable until level 22. Every fire-spec mage
        // below that walked up and hit things with its staff, because the
        // graph returned no decision and melee is what happens when it does.
        // (Spec is guid % 3, so a third of the fleet is fire at every level.)
        //
        // Rank does not matter here: IsSpellReady resolves down the chain to
        // whatever rank the bot actually owns, so naming the top rank is just
        // naming the spell.
        { "default ranged", hasHostileTarget && IsSpellReady(player, mageDefaultNuke), 18.0f,
            { mageDefaultNuke == uint32(10207) ? "mage scorch" :
                (mageDefaultNuke == uint32(133) ? "mage fireball" : "mage frostbolt"),
              mageDefaultNuke == uint32(10207) ? "default fire pressure" : "default ranged pressure",
              mageDefaultNuke, playerbot::PvpClassSpellContext::TargetMode::Enemy } },
        { "maintain buff", !player->IsInCombat() && mageArmorSpellId && mageArmorMissing && IsSpellReady(player, mageArmorSpellId), 9.0f,
            { "mage armor", "maintain ice or frost armor", mageArmorSpellId, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "mana gem missing", !player->IsInCombat() && manaGemItemId && IsSpellReady(player, 10054) && !player->HasItemCount(manaGemItemId), 8.0f,
            { "create mana gem", "conjure a mana gem outside combat", 10054, playerbot::PvpClassSpellContext::TargetMode::Self } },
        { "defensive reset", !isFireMage && !IsSpellReady(player, 11958) && IsSpellReady(player, 12472), 7.0f,
            { "mage cold snap", "reset frost defenses when ice block unavailable", 12472, playerbot::PvpClassSpellContext::TargetMode::Self } }
    });
}

    SpellDecision SelectPriestSpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
    {
        SpellDecision decision;
        if (!player)
            return decision;

        bool const hasHostileTarget = HasHostileTarget(player, target);

        // Spirit of Redemption is a short pure-healing window. Do not let the normal
        // priest fallback graph pick buffs, offensive casts, or arbitrary explicit
        // ally targets here. Always pick the lowest-health injured ally in heal range,
        // and do nothing if nobody is actually injured.
        if (IsPriestInSpiritOfRedemption(player))
        {
            Unit const* spiritHealTarget = IsSpellReady(player, 10917) ? SelectFriendlyLowestHealthTarget(player, GetConfiguredHealRange(), 99.0f, 0, false) : nullptr;
            if (spiritHealTarget)
                return { "priest flash heal", "heal lowest-health ally during spirit of redemption", 10917, playerbot::PvpClassSpellContext::TargetMode::Ally, spiritHealTarget->GetGUID() };

            return {};
        }

    // While Fade/Shadow Wraith (89784) is active, the priest's own body is
    // rooted and control has transferred to a possessed wraith creature (see
    // spell_pri_shadow_wraith_aura in spell_priest.cpp). The whole point of
    // using it is to escape melee pressure, but nothing in the normal
    // decision graph below ever issues the wraith a movement order - only
    // the (now rooted) player - so steer it explicitly here instead. The
    // drain life visual/channel during this window is already handled by
    // the aura script itself, so there is nothing else useful to decide
    // while shifted.
    if (player->HasAura(89784))
    {
        Unit const* wraithThreat = SelectNearbyMeleeTarget(player, target, 20.0f);
        playerbot::PvpClassActions::TryIssueShadowWraithFleeMovement(const_cast<Player*>(player),
            const_cast<Unit*>(wraithThreat ? wraithThreat : target));
        return decision;
    }

    bool const dispelThrottleActive = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken);
    Unit const* debuffedAlly = (!dispelThrottleActive && IsSpellReady(player, 988)) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, GetConfiguredHealRange()) : nullptr;
    Unit const* enemyBuffedTarget = (!dispelThrottleActive && IsSpellReady(player, 988) && hasHostileTarget) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, GetConfiguredSpellRange()) : nullptr;
    Unit const* shieldTarget = IsSpellReady(player, 10901) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 50.0f, kPriestWeakenedSoulSpellId) : nullptr;
    Unit const* renewTarget = IsSpellReady(player, 10929) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 80.0f) : nullptr;
    // Flash Heal is trained at 20, and Heal (16) and Lesser Heal (1) are
    // separate rank chains, so below 20 a priest otherwise had no direct heal
    // at all. Use the best one it actually owns.
    uint32 const priestHealSpellId = ResolveKnownPlayerSpellInChain(player, 10917) ? 10917 :
        (ResolveKnownPlayerSpellInChain(player, 2054) ? 2054 : 2050);
    char const* const priestHealName = priestHealSpellId == 10917 ? "priest flash heal" :
        (priestHealSpellId == 2054 ? "priest heal" : "priest lesser heal");
    Unit const* healTarget = IsSpellReady(player, priestHealSpellId) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 75.0f) : nullptr;
    Unit const* emergencyLowAlly = IsSpellReady(player, priestHealSpellId) ? SelectFriendlyHealthTarget(player, GetConfiguredHealRange(), 75.0f) : nullptr;
    Unit const* casterAlly = (player->IsInCombat() && IsSpellReady(player, 10060)) ? SelectFriendlyCasterTarget(player, GetConfiguredHealRange(), 100.0f) : nullptr;
    bool const shadowWordPainReady = IsSpellReady(player, kPriestShadowWordPainSpellId);
    Unit const* controlledTarget = shadowWordPainReady ? SelectEnemyNonBreakableCrowdControlTarget(player, 30.0f) : nullptr;
    Unit const* manaBurnTarget = IsSpellReady(player, 10876) ? SelectNearbyEnemyManaTarget(player, target, GetConfiguredLongRange(), 25.0f) : nullptr;
    Unit const* selectedRogueTarget = (shadowWordPainReady && HasHostileTarget(player, target) && target->GetClass() == CLASS_ROGUE &&
        !IsTargetInvalidByImmunity(player, target) && player->IsWithinLOSInMap(target) && player->IsWithinDistInMap(target, GetConfiguredLongRange())) ? target : nullptr;
    Unit const* rogueTarget = selectedRogueTarget ? selectedRogueTarget : (shadowWordPainReady ? SelectEnemyClassTarget(player, CLASS_ROGUE, GetConfiguredLongRange()) : nullptr);
    bool const isHolyPriest = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    bool const isShadowPriest = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const holySchoolLocked = player->GetSpellHistory()->IsSchoolLocked(SPELL_SCHOOL_MASK_HOLY);
    bool const shadowSchoolLocked = player->GetSpellHistory()->IsSchoolLocked(SPELL_SCHOOL_MASK_SHADOW);
    bool const shadowHealingFallback = isShadowPriest && shadowSchoolLocked;
    bool const isHealingPriest = profileSelection.profile == ClassicClassProfile::PrimaryClassic || isHolyPriest || shadowHealingFallback;

    // A shadow-locked shadow priest cannot heal while Shadowform remains active.
    // Drop the form immediately, reuse the normal healing-priest decision tree
    // for the lockout window, then the high-priority Shadowform candidate below
    // restores normal behavior as soon as Shadow becomes available again.
    if (shadowHealingFallback && player->HasAura(15473))
        const_cast<Player*>(player)->RemoveAurasDueToSpell(15473);

    if (isHolyPriest && holySchoolLocked)
    {
        Unit const* mindBlastTarget = IsSpellReady(player, 10947) ? SelectEnemyTargetInSpellRange(player, target, 10947) : nullptr;
        if (mindBlastTarget)
            return { "priest mind blast", "use shadow offense during a holy-school lockout", 10947,
                playerbot::PvpClassSpellContext::TargetMode::Enemy, mindBlastTarget->GetGUID() };
    }
    bool const isHumanPriest = player->GetRace() == RACE_HUMAN;
    bool const isUndeadPriest = player->GetRace() == RACE_UNDEAD_PLAYER;
    bool const isNightElfPriest = player->GetRace() == RACE_NIGHTELF;
    Unit const* chastiseTarget = (isHumanPriest && IsSpellReady(player, 81350)) ?
        SelectEnemyTargetInSpellRange(player, target, 81350) : nullptr;
    Unit const* devouringCurseTarget = (isUndeadPriest && IsSpellReady(player, kPriestDevouringCurseSpellId)) ?
        SelectEnemyTargetInSpellRange(player, target, kPriestDevouringCurseSpellId) : nullptr;
    if (devouringCurseTarget && (HasBreakableCrowdControl(player, devouringCurseTarget) ||
        HasAuraFromSpellChain(devouringCurseTarget, kPriestDevouringCurseSpellId)))
        devouringCurseTarget = nullptr;
    Unit const* wyrmsShadowTarget = (isUndeadPriest && IsSpellReady(player, kPriestWyrmsShadowSpellId)) ?
        SelectEnemyCastingTarget(player, 13.0f, target) : nullptr;
    Unit const* wispFormThreat = isNightElfPriest ? SelectNearbyMeleeTarget(player, target, 8.0f) : nullptr;
    Unit const* elunesGraceTarget = (isNightElfPriest && IsSpellReady(player, kPriestElunesGraceSpellId)) ?
        SelectFriendlyMeleePressureTarget(player, 30.0f, 60.0f) : nullptr;
    if (elunesGraceTarget && (elunesGraceTarget->HasAura(kPriestElunesGraceSpellId) ||
        elunesGraceTarget->HasInvisibilityAura() ||
        playerbot::PvpCore::IsBattlegroundFlagCarrier(elunesGraceTarget->ToPlayer())))
        elunesGraceTarget = nullptr;
    bool const shouldCastLightwell = isHolyPriest && player->IsInCombat() &&
        CountNearbyFriendlyPlayers(player, 10.0f, false, false) >= 2 && IsSpellReady(player, kPriestLightwellSpellId);
    Unit const* spiritHealTarget = (isHolyPriest && IsPriestInSpiritOfRedemption(player) && IsSpellReady(player, 10917)) ? SelectFriendlyLowestHealthTarget(player, 40.0f, 99.5f, 0, false) : nullptr;
    Unit const* fearWardTarget = (player->GetRace() == RACE_DWARF && IsSpellReady(player, 6346)) ? SelectFriendlyMissingBuffTarget(player, 6346, 40.0f) : nullptr;
    Unit const* shadowSilenceTarget = isShadowPriest && IsSpellReady(player, 15487) ? SelectEnemyCastingTarget(player, GetConfiguredSpellRange(), target) : nullptr;
    bool const shadowUnderMeleePressure = isShadowPriest && CountNearbyMeleeThreats(player, 8.0f) >= 1;
    // Troll racial addition: keep Hex of Weakness on the kill target and
    // Shadowguard on self, regardless of priest spec.
    bool const isTrollPriest = player->GetRace() == RACE_TROLL;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, isHumanPriest && player->HealthBelowPct(35) && IsSpellReady(player, 19243), 61.0f,
        { "priest desperate prayer", "emergency human-priest self heal below 35 percent health", 19243, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isNightElfPriest && wispFormThreat && player->HealthBelowPct(45) &&
        IsSpellReady(player, kPriestWispFormSpellId), 60.9f,
        { "priest wisp form", "escape melee pressure while below 45 percent health", kPriestWispFormSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, spiritHealTarget, 60.5f,
        { "priest flash heal", "spam flash heal during spirit of redemption", 10917, spiritHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, spiritHealTarget ? spiritHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, fearWardTarget, 60.2f,
        { "priest fear ward", "place fear ward on a random unwarded ally", 6346, fearWardTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, fearWardTarget ? fearWardTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, wyrmsShadowTarget, 59.9f,
        { "priest wyrms shadow", "interrupt an enemy caster with the shadow cone", kPriestWyrmsShadowSpellId,
            playerbot::PvpClassSpellContext::TargetMode::Enemy, wyrmsShadowTarget ? wyrmsShadowTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, elunesGraceTarget, 47.5f,
        { "priest elunes grace", "fade a low-health ally under melee pressure into invisibility", kPriestElunesGraceSpellId,
            elunesGraceTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally,
            elunesGraceTarget ? elunesGraceTarget->GetGUID() : ObjectGuid::Empty });

    if (isHealingPriest)
    {
        AddDecisionCandidate(candidates, shouldCastLightwell, 48.0f,
            { "priest lightwell", "place a lightwell in combat when at least two allies are within ten yards", kPriestLightwellSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, emergencyLowAlly, 47.0f,
            { priestHealName, "prioritize healing for any nearby ally below 75 percent health", priestHealSpellId, emergencyLowAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, emergencyLowAlly ? emergencyLowAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !emergencyLowAlly && debuffedAlly, 46.0f,
            { "priest dispel magic ally", "prioritize dispelling magic debuffs from allies", 988, debuffedAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, debuffedAlly ? debuffedAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !emergencyLowAlly && enemyBuffedTarget, 45.0f,
            { "priest dispel magic enemy", "prioritize dispelling magic buffs from enemies", 988, playerbot::PvpClassSpellContext::TargetMode::Enemy, enemyBuffedTarget ? enemyBuffedTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, shieldTarget && !HasAuraFromSpellChain(shieldTarget, 10901), 48.0f,
            { "priest power word shield ally", "protect ally below 50 percent health", 10901, shieldTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, shieldTarget ? shieldTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !isHolyPriest && casterAlly, 30.0f,
            { "priest power infusion", "boost nearby caster throughput in combat", 10060, casterAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, casterAlly ? casterAlly->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 1006) && IsSpellReady(player, 1006), 12.0f,
            { "priest inner fire", "maintain inner fire out of combat", 1006, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, renewTarget && !HasAuraFromSpellChain(renewTarget, 10929), 28.0f,
            { "priest renew", "maintain renew on moderately injured allies", 10929, renewTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, renewTarget ? renewTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, healTarget, 27.0f,
            { priestHealName, "heal party members below 75 percent health", priestHealSpellId, healTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, healTarget ? healTarget->GetGUID() : ObjectGuid::Empty });
    }

    if (isShadowPriest && !shadowHealingFallback)
    {
        AddDecisionCandidate(candidates, !HasAuraFromSpellChain(player, 15473) && IsSpellReady(player, 15473), 58.0f,
            { "priest shadowform", "stay in shadowform", 15473, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, shadowSilenceTarget, 38.5f,
            { "priest silence", "silence a nearby enemy caster", 15487, playerbot::PvpClassSpellContext::TargetMode::Enemy, shadowSilenceTarget ? shadowSilenceTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, shadowUnderMeleePressure && IsSpellReady(player, 89784), 37.0f,
            { "priest fade", "drop threat and disengage under melee pressure", 89784, playerbot::PvpClassSpellContext::TargetMode::Self });
        AddDecisionCandidate(candidates, !HasAuraFromSpellChain(player, 15286) && IsSpellReady(player, 15286), 24.0f,
            { "priest vampiric embrace", "maintain vampiric embrace on the kill target", 15286, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(candidates, hasHostileTarget && target && IsSpellReady(player, 10947), 22.5f,
            { "priest mind blast", "burst damage on cooldown", 10947, playerbot::PvpClassSpellContext::TargetMode::Enemy });
        AddDecisionCandidate(candidates, hasHostileTarget && target && IsSpellReady(player, 18807), 16.0f,
            { "priest mind flay", "default shadow damage filler", 18807, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    }

    AddDecisionCandidate(candidates, isTrollPriest && hasHostileTarget && target && !HasAuraFromSpellChain(target, 9035), 23.5f,
        { "priest hex of weakness", "troll racial debuff on the kill target", 9035, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isTrollPriest && !HasAuraFromSpellChain(player, 19312) && IsSpellReady(player, 19312), 23.0f,
        { "priest shadowguard", "troll racial self-buff upkeep", 19312, playerbot::PvpClassSpellContext::TargetMode::Self });

    AddDecisionCandidate(candidates, rogueTarget && !HasAuraFromSpellChain(rogueTarget, kPriestShadowWordPainSpellId), 35.0f,
        { "priest shadow word pain", "maintain dot pressure on rogues", kPriestShadowWordPainSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, chastiseTarget, 39.0f,
        { "priest chastise", "human priest damage and root on an enemy target", 81350, playerbot::PvpClassSpellContext::TargetMode::Enemy, chastiseTarget ? chastiseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, devouringCurseTarget, 36.0f,
        { "priest devouring curse", "undead priest racial dot used ahead of shadow word pain whenever its cooldown is ready",
            kPriestDevouringCurseSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy,
            devouringCurseTarget ? devouringCurseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isHolyPriest && manaBurnTarget, 21.0f,
        { "priest mana burn", "burn mana from enemy casters", 10876, playerbot::PvpClassSpellContext::TargetMode::Enemy, manaBurnTarget ? manaBurnTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isHolyPriest && hasHostileTarget && IsSpellReady(player, 10934), 21.0f,
        { "priest smite", "holy fallback damage instead of mana burn", 10934, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, CountNearbyEnemies(player, 10.0f) >= 2 && CountNearbyFriendlyPlayers(player, 10.0f) >= 2 && IsSpellReady(player, 27801), 20.0f,
        { "priest holy nova", "aoe pressure and splash healing in melee cluster", 27801, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, CountNearbyEnemies(player, 8.0f) >= 2 && IsSpellReady(player, 10890), 19.5f,
        { "priest psychic scream", "fear nearby enemies when surrounded", 10890, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && target && shadowWordPainReady && !HasBreakableCrowdControl(player, target) && !HasAuraFromSpellChain(target, kPriestShadowWordPainSpellId), 19.0f,
        { "priest shadow word pain", "fallback pressure on non-breakable crowd-controlled or open targets", kPriestShadowWordPainSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, controlledTarget && !HasAuraFromSpellChain(controlledTarget, kPriestShadowWordPainSpellId), 18.0f,
        { "priest shadow word pain", "fallback pressure on non-breakable crowd-controlled targets", kPriestShadowWordPainSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, controlledTarget ? controlledTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, hasHostileTarget && target && IsLowOrOutOfManaForFallback(player) && IsWandShootReadyForDecision(player), 18.5f,
        { "priest shoot wand", "fallback to wand pressure while low on mana", kWandShootSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !isShadowPriest && IsSpellReady(player, priestHealSpellId) && player->HealthBelowPct(85), 17.0f,
        { priestHealName, "fallback self-healing while under pressure", priestHealSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, hasHostileTarget && target && !HasBreakableCrowdControl(player, target) && IsWandShootReadyForDecision(player), 8.0f,
        { "priest shoot wand", "default offensive fallback when no better priest action is available", kWandShootSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Below the wand on purpose: only a priest with no wand and nothing else
    // to do reaches it. Discipline had no nuke at all before Mana Burn (24),
    // and shadow none before Mind Flay (20) outside Mind Blast's cooldown.
    AddDecisionCandidate(candidates, !isHolyPriest && hasHostileTarget && target && !HasBreakableCrowdControl(player, target) &&
        !(isShadowPriest && !shadowHealingFallback && IsSpellReady(player, 18807)) && IsSpellReady(player, 10934), 7.5f,
        { "priest smite", "filler nuke when no wand and nothing better is available", 10934, playerbot::PvpClassSpellContext::TargetMode::Enemy });

        return SelectHighestPriorityCastableDecision(candidates, player, target, allyTarget);
    }

    // How long a Backstab rogue or a Shred cat walks for its target's back
    // before settling for the frontal builder, and how long it then builds from
    // the front before trying again.
    constexpr uint32 kFlankAttemptMs = 3000;
    constexpr uint32 kFlankRetryAfterGiveUpMs = 10000;
    // Inside this the walk round to the back is short. Farther out the ordinary
    // melee chase closes the gap first.
    constexpr float kFlankEngageRange = 10.0f;
    // A try the selector stopped asking about for this long - stealth held for
    // an opener after a Vanish, a stretch in bear form - is over, and the next
    // one gets its full kFlankAttemptMs.
    constexpr uint32 kFlankAttemptStaleMs = 2000;

    struct FlankAttemptState
    {
        ObjectGuid targetGuid;
        bool attempting = false;
        uint32 attemptStartMs = 0;
        uint32 lastAskedMs = 0;
        uint32 retryNotBeforeMs = 0;
    };

    std::unordered_map<uint64, FlankAttemptState> g_FlankAttemptByGuid;

    // Whether a Backstab or Shred bot should hold back its frontal builder this
    // tick and walk for the target's back instead.
    //
    // The selectors used to offer the positional strike only once the bot was
    // ALREADY behind. From the front the frontal builder (Sinister Strike,
    // Claw) was always castable, so it always won, and the walk round to the
    // back - which CastDirectSpell only does when the chosen strike fails "not
    // behind" - was never asked for. Holding the frontal builder back makes the
    // positional strike the decision, and its not-behind failure does the
    // walking.
    //
    // A target that keeps turning would hold the builder back forever, so each
    // try gets kFlankAttemptMs, then the bot builds from the front for
    // kFlankRetryAfterGiveUpMs. Getting behind, by the walk or by luck, ends
    // both.
    bool ShouldHoldFrontalBuilderForFlank(Player const* player, Unit const* target, bool positionalStrikeReady)
    {
        if (!player || !target)
            return false;

        FlankAttemptState& state = playerbot::LockedGetOrCreate(g_FlankAttemptByGuid, player->GetGUID().GetRawValue());
        if (state.targetGuid != target->GetGUID())
        {
            state = FlankAttemptState();
            state.targetGuid = target->GetGUID();
        }

        uint32 const nowMs = GameTime::GetGameTimeMS();
        if (state.attempting && nowMs - state.lastAskedMs > kFlankAttemptStaleMs)
            state.attempting = false;
        state.lastAskedMs = nowMs;

        if (!target->HasInArc(static_cast<float>(M_PI), player))
        {
            state.attempting = false;
            state.retryNotBeforeMs = 0;
            return false;
        }

        if (!positionalStrikeReady || !player->IsWithinDistInMap(target, kFlankEngageRange) ||
            !playerbot::PvpClassActions::CanFlankTarget(player, target))
        {
            state.attempting = false;
            return false;
        }

        if (nowMs < state.retryNotBeforeMs)
            return false;

        if (!state.attempting)
        {
            state.attempting = true;
            state.attemptStartMs = nowMs;
        }

        if (nowMs - state.attemptStartMs < kFlankAttemptMs)
            return true;

        state.attempting = false;
        state.retryNotBeforeMs = nowMs + kFlankRetryAfterGiveUpMs;
        return false;
    }

    SpellDecision SelectDruidSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
    {
        SpellDecision decision;
        if (!player)
            return decision;

        bool const recoveredFromPolymorph =
            playerbot::PvpClassActions::GetLastExecutionStatus(player) == "cast_failed_crowd_controlled_polymorph" &&
            !player->HasUnitState(UNIT_STATE_CONFUSED) &&
            !player->HasAuraType(SPELL_AURA_MOD_CONFUSE) &&
            !player->IsPolymorphed();

    bool const isFeralDruid = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const isBalanceDruid = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
    bool const isCasterForm = IsDruidCasterForm(player);
    bool const feralMayUseCasterUtility = !isFeralDruid || isCasterForm;

        Unit const* lowManaAlly = (feralMayUseCasterUtility && IsSpellReady(player, 29166)) ? SelectFriendlyLowManaTarget(player, 40.0f, 10.0f) : nullptr;
        bool const dispelThrottleActive = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken);
        Unit const* cursedTarget = (feralMayUseCasterUtility && !dispelThrottleActive && IsSpellReady(player, 2782)) ? SelectFriendlyDispelTarget(player, DISPEL_CURSE, 40.0f) : nullptr;
        Unit const* poisonedTarget = (feralMayUseCasterUtility && !dispelThrottleActive && IsSpellReady(player, 2893)) ? SelectFriendlyDispelTarget(player, DISPEL_POISON, 40.0f) : nullptr;
        Unit const* swiftmendTarget = IsSpellReady(player, 18562) ? SelectFriendlyHealthTarget(player, 40.0f, 50.0f) : nullptr;
        Unit const* emergencyLowTarget = (IsSpellReady(player, 17116) && IsSpellReady(player, 25297)) ? SelectFriendlyHealthTarget(player, 40.0f, 25.0f) : nullptr;
        Unit const* emergencyTarget = IsSpellReady(player, 25297) ? SelectFriendlyHealthTarget(player, 40.0f, 50.0f) : nullptr;
        Unit const* regrowthTarget = IsSpellReady(player, 9858) ? SelectFriendlyHealthTarget(player, 40.0f, 85.0f) : nullptr;
        Unit const* rejuvTarget = IsSpellReady(player, 25299) ? SelectFriendlyHealthTarget(player, 40.0f, 90.0f) : nullptr;
        // Regrowth is trained at 12; below that Healing Touch is the druid's
        // only direct heal, and it was otherwise used only behind Nature's
        // Swiftness (a 30-point talent).
        bool const knowsRegrowth = ResolveKnownPlayerSpellInChain(player, 9858) != 0;
        Unit const* lowLevelHealingTouchTarget = (!knowsRegrowth && IsSpellReady(player, 25297)) ?
            SelectFriendlyHealthTarget(player, 40.0f, 75.0f) : nullptr;
        // Cat Form is trained at 20 and Bear Form at 10. A feral druid with
        // neither form has nothing on the feral path and plays as a caster.
        bool const knowsCatForm = ResolveKnownPlayerSpellInChain(player, 768) != 0;
        bool const knowsFeralForm = knowsCatForm || ResolveKnownPlayerSpellInChain(player, 5487) != 0;
        bool const feralAsCaster = isFeralDruid && !knowsFeralForm;
        Unit const* rogueTarget = SelectEnemyClassTarget(player, CLASS_ROGUE, 30.0f);
        Unit const* meleeThreat = SelectNearbyMeleeTarget(player, target, 8.0f);
        uint32 const nearbyMeleeThreats = CountNearbyMeleeThreats(player, 8.0f);
        bool const heavyMeleePressure = nearbyMeleeThreats >= 2;
        Unit const* moonfireExecuteTarget = nullptr;
        if (IsSpellReady(player, 8921))
        {
            Unit const* nearbyTarget = SelectNearbyEnemyTarget(player, target, 25.0f);
            if (nearbyTarget && nearbyTarget->HealthBelowPct(20))
                moonfireExecuteTarget = nearbyTarget;
        }

        if (isFeralDruid && !feralAsCaster && HasHostileTarget(player, target))
        {
            bool const inCat = HasAuraFromSpellChain(player, 768);
            bool const inBear = HasAuraFromSpellChain(player, 5487);
            bool const inFeralForm = inCat || inBear;
            bool const isProwling = HasAuraFromSpellChain(player, 9913);
            Unit const* lowAlly = SelectFriendlyHealthTarget(player, 40.0f, 80.0f);
            Unit const* rootTarget = SelectPredatorsSwiftnessRootTarget(player, target, 30.0f);
            Unit const* feralChargeBearTarget = IsSpellReady(player, 16979) ? SelectEnemyGapCloserTarget(player, target, 8.0f, 25.0f, false) : nullptr;
            Unit const* feralChargeCatTarget = IsSpellReady(player, 49376) ? SelectEnemyGapCloserTarget(player, target, 8.0f, 25.0f, false) : nullptr;
            bool const safeAgain = !player->HealthBelowPct(60) || !heavyMeleePressure;

            std::vector<PrioritizedSpellDecision> feralCandidates;
            AddDecisionCandidate(feralCandidates, isCasterForm && lowManaAlly && !lowManaAlly->HasAura(29166), 72.0f,
                { "druid innervate", "feral caster-form utility on low-mana ally", 29166, lowManaAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lowManaAlly ? lowManaAlly->GetGUID() : ObjectGuid::Empty });
            AddDecisionCandidate(feralCandidates, isCasterForm && cursedTarget, 71.5f,
                { "druid remove curse", "feral caster-form curse removal", 2782, cursedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget ? cursedTarget->GetGUID() : ObjectGuid::Empty });
            AddDecisionCandidate(feralCandidates, isCasterForm && poisonedTarget && !HasAuraFromSpellChain(poisonedTarget, 2893), 71.0f,
                { "druid abolish poison", "feral caster-form poison removal", 2893, poisonedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, poisonedTarget ? poisonedTarget->GetGUID() : ObjectGuid::Empty });
            AddDecisionCandidate(feralCandidates, !player->IsInCombat() && !isProwling && !HasAuraFromSpellChain(player, 16864) && IsSpellReady(player, 16864), 70.9f,
                { "druid omen of clarity", "maintain omen of clarity before prowling or engaging", 16864, playerbot::PvpClassSpellContext::TargetMode::Self });
            AddDecisionCandidate(feralCandidates, inCat && !player->IsInCombat() && !isProwling && IsSpellReady(player, 9913), 70.8f,
                { "druid prowl", "stealth before opening as feral cat", 9913, playerbot::PvpClassSpellContext::TargetMode::Self });
            AddDecisionCandidate(feralCandidates, inFeralForm && !isProwling && target && !HasAuraFromSpellChain(target, 17392) && !HasAuraFromSpellChain(target, 9907) && IsSpellReady(player, 17392), 70.5f,
                { "druid faerie fire feral", "feral-form armor debuff", 17392, playerbot::PvpClassSpellContext::TargetMode::Enemy });
            AddDecisionCandidate(feralCandidates, player->HasAura(69369) && lowAlly && IsSpellReady(player, 9858), 70.0f,
                { "druid regrowth", "predator's swiftness regrowth on lowest ally", 9858, lowAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lowAlly ? lowAlly->GetGUID() : ObjectGuid::Empty });
            AddDecisionCandidate(feralCandidates, player->HasAura(69369) && AllFriendlyPlayersHealthy(player, 40.0f, 80.0f) && rootTarget && IsSpellReady(player, 9853), 69.0f,
                { "druid entangling roots", "predator's swiftness root when allies are healthy", 9853, playerbot::PvpClassSpellContext::TargetMode::Enemy, rootTarget ? rootTarget->GetGUID() : ObjectGuid::Empty });
            AddDecisionCandidate(feralCandidates, player->HealthBelowPct(60) && heavyMeleePressure && !inBear && IsSpellReady(player, 5487), 68.0f,
                { "druid bear form", "swap bear under heavy melee pressure", 5487, playerbot::PvpClassSpellContext::TargetMode::Self, meleeThreat ? meleeThreat->GetGUID() : ObjectGuid::Empty });
            AddDecisionCandidate(feralCandidates, inBear && player->GetComboPoints() >= 5 && player->GetPowerPct(POWER_MANA) < 50.0f && !player->HasAura(89758) && IsSpellReady(player, 89758), 67.0f,
                { "druid thinnervate", "bear combo point thinnervate", 89758, playerbot::PvpClassSpellContext::TargetMode::Enemy });
            AddDecisionCandidate(feralCandidates, inBear && feralChargeBearTarget, 70.7f,
                { "druid feral charge bear", "bear gap close / interrupt from charge range", 16979, playerbot::PvpClassSpellContext::TargetMode::Enemy, feralChargeBearTarget ? feralChargeBearTarget->GetGUID() : ObjectGuid::Empty });
            AddDecisionCandidate(feralCandidates, inBear && IsSpellReady(player, 22842), 65.0f,
                { "druid frenzied regeneration", "bear survival recovery", 22842, playerbot::PvpClassSpellContext::TargetMode::Self });
            AddDecisionCandidate(feralCandidates, inBear && IsSpellReady(player, 5229), 64.0f,
                { "druid enrage", "generate bear rage", 5229, playerbot::PvpClassSpellContext::TargetMode::Self });
            AddDecisionCandidate(feralCandidates, inBear && IsSpellReady(player, 9898) && !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, 9898), 63.0f,
                { "druid demoralizing roar", "debuff melee attackers", 9898, playerbot::PvpClassSpellContext::TargetMode::Self });
            AddDecisionCandidate(feralCandidates, inBear && player->GetPower(POWER_RAGE) >= 150 && IsSpellReady(player, 9881), 62.0f,
                { "druid maul", "dump extra bear rage", 9881, playerbot::PvpClassSpellContext::TargetMode::Enemy });
            AddDecisionCandidate(feralCandidates, inBear && safeAgain && IsSpellReady(player, 768), 61.0f,
                { "druid cat form", "return to cat form once safe", 768, playerbot::PvpClassSpellContext::TargetMode::Self });
            AddDecisionCandidate(feralCandidates, !inCat && !inBear && IsSpellReady(player, 768), 60.0f,
                { "druid cat form", "prefer cat form for feral pressure", 768, playerbot::PvpClassSpellContext::TargetMode::Self });
            AddDecisionCandidate(feralCandidates, !knowsCatForm && !inBear && IsSpellReady(player, 5487), 60.0f,
                { "druid bear form", "fight in bear form until cat form is trained", 5487, playerbot::PvpClassSpellContext::TargetMode::Self });
            AddDecisionCandidate(feralCandidates, inCat && IsRootedOrSnared(player) && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 768), 59.0f,
                { "druid cat form", "powershift root or snare", 768, playerbot::PvpClassSpellContext::TargetMode::Self, target ? target->GetGUID() : ObjectGuid::Empty });
            AddDecisionCandidate(feralCandidates, inCat && feralChargeCatTarget, 70.6f,
                { "druid feral charge cat", "cat gap close / interrupt from charge range", 49376, playerbot::PvpClassSpellContext::TargetMode::Enemy, feralChargeCatTarget ? feralChargeCatTarget->GetGUID() : ObjectGuid::Empty });
            AddDecisionCandidate(feralCandidates, inCat && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 9821), 58.0f,
                { "druid dash", "catch target in cat form", 9821, playerbot::PvpClassSpellContext::TargetMode::Self });
            AddDecisionCandidate(feralCandidates, inCat && player->GetComboPoints() >= 5 && player->GetPowerPct(POWER_MANA) < 50.0f && !player->HasAura(89758) && IsSpellReady(player, 89758), 56.0f,
                { "druid thinnervate", "restore mana with combo points", 89758, playerbot::PvpClassSpellContext::TargetMode::Enemy });
            AddDecisionCandidate(feralCandidates, inCat && player->GetComboPoints() >= 5 && IsSpellReady(player, 9896), 55.0f,
                { "druid rip", "feral combo point bleed finisher", 9896, playerbot::PvpClassSpellContext::TargetMode::Enemy });
            // Shred is positional like Backstab, and a mob the cat holds
            // aggro on re-faces it every AI tick - momentary "behind"
            // windows just bait an endless flanking dance. Claw is the
            // builder whenever a mob is fixated on the bot; against a
            // player, or a mob busy with someone else, the cat walks for the
            // back (ShouldHoldFrontalBuilderForFlank) and Shreds.
            //
            // Worked out here, above the mangle Claw, because that Claw
            // outranks Shred and no character on the realm knows Mangle
            // (checked 2026-10-15): it won every time, so Shred never went
            // off at all.
            bool const flankForShred = inCat &&
                ShouldHoldFrontalBuilderForFlank(player, target, IsSpellReady(player, 9830));
            bool const shredViable = flankForShred ||
                (!target->HasInArc(static_cast<float>(M_PI), player) && playerbot::PvpClassActions::CanFlankTarget(player, target));
            AddDecisionCandidate(feralCandidates, inCat && !shredViable && !HasAuraFromSpellChain(target, 33876) && IsSpellReady(player, 9850), 54.0f,
                { "druid claw", "build combo points when mangle is missing", 9850, playerbot::PvpClassSpellContext::TargetMode::Enemy });
            AddDecisionCandidate(feralCandidates, inCat && !HasAuraFromSpellChain(target, 9904) && IsSpellReady(player, 9904), 53.0f,
                { "druid rake", "maintain rake bleed", 9904, playerbot::PvpClassSpellContext::TargetMode::Enemy });
            AddDecisionCandidate(feralCandidates, inFeralForm && !isProwling && target && !HasAuraFromSpellChain(target, 17392) && !HasAuraFromSpellChain(target, 9907) && IsSpellReady(player, 17392), 52.0f,
                { "druid faerie fire feral", "feral armor debuff filler", 17392, playerbot::PvpClassSpellContext::TargetMode::Enemy });
            // While walking for the back, Shred cannot land from the front, so
            // it stays the decision and its not-behind failure in
            // CastDirectSpell walks the cat round.
            AddDecisionCandidate(feralCandidates, inCat && shredViable && IsSpellReady(player, 9830), 51.0f,
                { "druid shred", flankForShred ? "walk behind the target to shred" : "behind-target combo point builder", 9830, playerbot::PvpClassSpellContext::TargetMode::Enemy });
            AddDecisionCandidate(feralCandidates, inCat && !shredViable && IsSpellReady(player, 9850), 50.8f,
                { "druid claw", "frontal fallback builder while holding aggro", 9850, playerbot::PvpClassSpellContext::TargetMode::Enemy });

            return SelectHighestPriorityCastableDecision(feralCandidates, player, target, nullptr);
        }

    // Balance stays in Moonkin Form at all times, so melee-pressure responses
    // are Moon Bash / Nature's Grasp / Leap instead of shifting to Bear Form.
    bool const balanceUnderMeleePressure = isBalanceDruid && CountNearbyEnemies(player, 10.0f) >= 1;
    Unit const* balanceMeleeRangeTarget = isBalanceDruid ? SelectNearbyMeleeTarget(player, target, 5.0f) : nullptr;
    Unit const* balanceEscapeAlly = (isBalanceDruid && balanceMeleeRangeTarget) ? SelectFriendlyNearRangeCapTarget(player, 8.0f, 25.0f) : nullptr;
    // 81342/81343 are the moonkin talents that permit casting Regrowth/
    // Rejuvenation while shapeshifted; without them Moonkin Form blocks both.
    Unit const* balanceRegrowthTarget = (isBalanceDruid && player->HasAura(81342)) ? SelectFriendlyLowestHealthTarget(player, 40.0f, 80.0f) : nullptr;
    Unit const* balanceRejuvTarget = (isBalanceDruid && player->HasAura(81343)) ? SelectFriendlyLowestHealthTarget(player, 40.0f, 80.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, recoveredFromPolymorph && IsUnderOpenSkyForDruidForm(player) && IsSpellReady(player, 783), 55.0f,
        { "druid travel form recovery", "recovering from polymorph by travel-form reposition", 783, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isBalanceDruid && !HasAuraFromSpellChain(player, 24858) && IsSpellReady(player, 24858), 54.8f,
        { "druid moonkin form", "always stay in moonkin form", 24858, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isFeralDruid && !player->IsInCombat() && !player->HasStealthAura() && !HasAuraFromSpellChain(player, 16864) && IsSpellReady(player, 16864), 54.5f,
        { "druid omen of clarity", "maintain omen of clarity out of combat", 16864, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, feralMayUseCasterUtility && lowManaAlly && !lowManaAlly->HasAura(29166), 50.0f,
        { "druid innervate", "stabilize low-mana ally with innervate", 29166, lowManaAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lowManaAlly ? lowManaAlly->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, cursedTarget, 49.0f,
        { "druid remove curse", "remove curses from allies", 2782, cursedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cursedTarget ? cursedTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, poisonedTarget && !HasAuraFromSpellChain(poisonedTarget, 2893), 48.0f,
        { "druid abolish poison", "remove poison pressure from allies", 2893, poisonedTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, poisonedTarget ? poisonedTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, swiftmendTarget && (HasAuraFromSpellChain(swiftmendTarget, 9858) || HasAuraFromSpellChain(swiftmendTarget, 25299)), 47.0f,
        { "druid swiftmend", "consume hot for emergency heal under 50 percent", 18562, swiftmendTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, swiftmendTarget ? swiftmendTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, emergencyLowTarget, 46.0f,
        { "druid natures swiftness", "prepare instant healing touch for critical ally", 17116, playerbot::PvpClassSpellContext::TargetMode::Self, emergencyLowTarget ? emergencyLowTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->HasAura(17116) && emergencyTarget, 45.0f,
        { "druid healing touch", "consume natures swiftness with healing touch", 25297, emergencyTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, emergencyTarget ? emergencyTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isBalanceDruid && !isFeralDruid && CountNearbyFriendlyPlayers(player, 30.0f) >= 7 &&
            !AllFriendlyPlayersHealthy(player, 30.0f, 70.0f) && IsSpellReady(player, 740), 44.3f,
        { "druid tranquility", "raid-wide heal channel when multiple nearby allies are hurt", 740, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !isBalanceDruid && regrowthTarget && !HasAuraFromSpellChain(regrowthTarget, 9858), 44.0f,
        { "druid regrowth", "maintain regrowth on injured allies", 9858, regrowthTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, regrowthTarget ? regrowthTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isBalanceDruid && lowLevelHealingTouchTarget, 43.5f,
        { "druid healing touch", "direct heal before regrowth is trained", 25297, lowLevelHealingTouchTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lowLevelHealingTouchTarget ? lowLevelHealingTouchTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isBalanceDruid && rejuvTarget && !HasAuraFromSpellChain(rejuvTarget, 25299), 43.0f,
        { "druid rejuvenation", "maintain rejuvenation on injured allies", 25299, rejuvTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, rejuvTarget ? rejuvTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isBalanceDruid && moonfireExecuteTarget, 42.0f,
        { "druid moonfire execute", "spam moonfire pressure on nearby low-health enemies", 8921, playerbot::PvpClassSpellContext::TargetMode::Enemy, moonfireExecuteTarget ? moonfireExecuteTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, balanceRegrowthTarget, 45.5f,
        { "druid regrowth", "moonkin talent allows regrowth on lowest-health ally below 80 percent", 9858, balanceRegrowthTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, balanceRegrowthTarget ? balanceRegrowthTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, balanceRejuvTarget, 45.4f,
        { "druid rejuvenation", "moonkin talent allows rejuvenation on lowest-health ally below 80 percent", 25299, balanceRejuvTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, balanceRejuvTarget ? balanceRejuvTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isBalanceDruid && target && !HasAuraFromSpellChain(target, 8921), 44.5f,
        { "druid moonfire", "maintain moonfire on the kill target", 8921, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Escalation while a melee attacker is on top of the moonkin: bash first,
    // root if it is still pressured, then disengage toward a nearby ally.
    AddDecisionCandidate(candidates, isBalanceDruid && balanceMeleeRangeTarget && IsSpellReady(player, 82423), 41.5f,
        { "druid moon bash", "strike an enemy that has closed to melee range", 82423, playerbot::PvpClassSpellContext::TargetMode::Enemy, balanceMeleeRangeTarget ? balanceMeleeRangeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isBalanceDruid && balanceMeleeRangeTarget && !HasAuraFromSpellChain(player, 17329) && IsSpellReady(player, 17329), 41.0f,
        { "druid natures grasp", "root the next melee attacker while still pressured", 17329, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isBalanceDruid && balanceMeleeRangeTarget && balanceEscapeAlly && IsSpellReady(player, 83111), 40.5f,
        { "druid leap", "disengage toward a nearby ally while still under melee pressure", 83111, playerbot::PvpClassSpellContext::TargetMode::Ally, balanceEscapeAlly ? balanceEscapeAlly->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isBalanceDruid && target && balanceUnderMeleePressure && IsSpellReady(player, 5176), 40.0f,
        { "druid wrath", "ranged pressure while enemies are within 10 yards", 5176, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isBalanceDruid && target && !balanceUnderMeleePressure && CountNearbyEnemies(player, 15.0f) >= kMassAoeEnemyCountThreshold && IsSpellReady(player, 16914), 39.7f,
        { "druid hurricane", "channel aoe damage against a clustered enemy group while clear of melee pressure", 16914, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isBalanceDruid && target && !balanceUnderMeleePressure && IsSpellReady(player, 2912), 39.5f,
        { "druid starfire", "primary nuke while clear of melee pressure; castable while moving", 2912, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Starfire is trained at 20. Below that Wrath is balance's only nuke, and
    // it was reserved for melee pressure, so the druid stood idle at range.
    AddDecisionCandidate(candidates, isBalanceDruid && target && !balanceUnderMeleePressure &&
        !ResolveKnownPlayerSpellInChain(player, 2912) && IsSpellReady(player, 5176), 39.4f,
        { "druid wrath", "primary nuke before starfire is trained", 5176, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isFeralDruid && (HasAuraFromSpellChain(player, 768) || HasAuraFromSpellChain(player, 5487)) && rogueTarget && !HasAuraFromSpellChain(rogueTarget, 17392) && !HasAuraFromSpellChain(rogueTarget, 9907) && IsSpellReady(player, 17392), 30.5f,
        { "druid faerie fire feral", "apply feral faerie fire to nearby rogues", 17392, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isFeralDruid && feralMayUseCasterUtility && rogueTarget && !HasAuraFromSpellChain(rogueTarget, kDruidCasterFaerieFireSpellId) &&
        IsSpellReady(player, kDruidCasterFaerieFireSpellId) && !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kDruidCasterFaerieFireSpellId), 30.0f,
        { "druid faerie fire", "apply faerie fire to nearby rogues", kDruidCasterFaerieFireSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, rogueTarget ? rogueTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, ((!isFeralDruid && !isBalanceDruid && meleeThreat) || (isFeralDruid && player->HealthBelowPct(60) && heavyMeleePressure)) && IsSpellReady(player, 5487), 29.0f,
        { "druid bear form", isFeralDruid ? "swap to bear only under heavy melee pressure below 60 percent health" : "swap to bear under physical melee pressure", 5487, playerbot::PvpClassSpellContext::TargetMode::Self, meleeThreat ? meleeThreat->GetGUID() : ObjectGuid::Empty });
    // Dire Bear Form (9634) is rank 2 of Bear Form, so any druid past level 40
    // holds 9634 and never 5487 - this gate was permanently false and Feral
    // Charge could not fire at all off the non-feral path. The feral fast path
    // two hundred lines up already uses the chain form of exactly this test.
    Unit const* bearChargeTarget = HasAuraFromSpellChain(player, 5487) && IsSpellReady(player, 16979) ? SelectEnemyGapCloserTarget(player, target, 8.0f, 25.0f, false) : nullptr;
    AddDecisionCandidate(candidates, bearChargeTarget, 28.0f,
        { "druid feral charge", "bear gap close / interrupt from charge range", 16979, playerbot::PvpClassSpellContext::TargetMode::Enemy, bearChargeTarget ? bearChargeTarget->GetGUID() : ObjectGuid::Empty });
    // What a restoration druid does when there is nothing to heal. Every
    // offensive spell above it belongs to Balance, and its own moonfire is an
    // execute that waits for the target to fall below a fifth of its health,
    // so a resto druid with a healthy enemy in front of it had NOTHING it
    // could pick and simply stood there watching a base being capped.
    //
    // Deliberately the bottom of the list: healing, dispels, faerie fire on a
    // rogue and dropping to bear under melee all still come first. This is
    // only the filler for a druid that would otherwise do nothing at all.
    AddDecisionCandidate(candidates, !isBalanceDruid && (!isFeralDruid || feralAsCaster) && HasHostileTarget(player, target) &&
        !HasAuraFromSpellChain(target, 8921) && IsSpellReady(player, 8921), 27.5f,
        { "druid moonfire", "put moonfire on the target with nothing to heal", 8921, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !isBalanceDruid && (!isFeralDruid || feralAsCaster) && HasHostileTarget(player, target) &&
        IsSpellReady(player, 5176), 27.0f,
        { "druid wrath", "filler nuke with nothing to heal", 5176, playerbot::PvpClassSpellContext::TargetMode::Enemy });

        return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
    }

    SpellDecision SelectPaladinSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
    {
        SpellDecision decision;
        if (!player)
            return decision;

    bool const isRetPaladin = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const isProtPaladin = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    bool const knowsSacrificialAura = player->HasSpell(kPaladinSacrificialAuraSpellId);
    // Flash of Light is trained at 20. Below that Holy Light is the only heal
    // a paladin owns, and leaving it out left low-level holy paladins with
    // nothing to cast at all.
    bool const knowsFlashOfLight = ResolveKnownPlayerSpellInChain(player, 19943) != 0;
    uint32 const healSpellId = knowsFlashOfLight ? 19943 : 635;
    char const* const healActionName = knowsFlashOfLight ? "paladin flash of light" : "paladin holy light";
    Unit const* emergencyLowAlly = isRetPaladin ? nullptr : SelectFriendlyHealthTarget(player, 15.0f, 25.0f);
    Unit const* rebukeTarget = (isProtPaladin && IsSpellReady(player, 81276)) ? SelectEnemyCastingTarget(player, 10.0f, target) : nullptr;
    Unit const* cleanseTarget = nullptr;
    if (IsSpellReady(player, 4987))
    {
        cleanseTarget = SelectFriendlyDispelTarget(player, DISPEL_POISON, 40.0f);
        if (!cleanseTarget)
            cleanseTarget = SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 40.0f);
    }
    Unit const* freedomTarget = IsSpellReady(player, 1044) ? SelectFriendlySnaredTarget(player, 40.0f) : nullptr;
    Unit const* sacrificeTarget = IsSpellReady(player, 6940) ? SelectFriendlyHealthTarget(player, 40.0f, 95.0f) : nullptr;
    Unit const* executeTarget = SelectNearbyEnemyTarget(player, target, 30.0f);
    Unit const* compelTarget = (isProtPaladin && IsSpellReady(player, 62124)) ?
        SelectEnemyTargetInSpellRange(player, target, 62124) : nullptr;
    Unit const* stunTarget = IsSpellReady(player, 10308) ? SelectEnemyCastingTarget(player, 10.0f, executeTarget) : nullptr;
    Unit const* repentanceTarget = (isRetPaladin && IsSpellReady(player, 20066)) ? SelectEnemyCastingTarget(player, 20.0f, executeTarget) : nullptr;
    Unit const* stunnedJudgementTarget = (isRetPaladin && HasAuraFromSpellChain(player, 20375)) ? SelectStunnedEnemyTarget(player, executeTarget, 30.0f) : nullptr;
    Unit const* protectionTarget = IsSpellReady(player, 10278) ? SelectFriendlyMeleePressureTarget(player, 40.0f, 50.0f) : nullptr;
    // Hand of Protection and Divine Shield apply the same Forbearance debuff,
    // so self-casting Hand of Protection under moderate melee pressure would
    // lock out Divine Shield for a full minute if health later crashes below
    // its own emergency threshold. Self-protection stays Divine Shield's job.
    if (protectionTarget == player)
        protectionTarget = nullptr;
    if (protectionTarget && playerbot::PvpCore::IsBattlegroundFlagCarrier(protectionTarget->ToPlayer()))
        protectionTarget = nullptr;
    Unit const* holyStrikeFlashHealTarget = (isRetPaladin && player->HasAura(89796) && IsSpellReady(player, 19943)) ? SelectFriendlyLowestHealthTarget(player, 40.0f, 100.0f) : nullptr;
    ObjectGuid const wisdomTargetGuid = (isRetPaladin && IsSpellReady(player, 25290)) ?
        SelectFriendlyWithManaAndWithoutAuraFromSpellChain(player, 25290, 45.0f, false) : ObjectGuid::Empty;
    ObjectGuid const mightTargetGuid = (isRetPaladin && IsSpellReady(player, 25291)) ?
        SelectFriendlyWithoutManaAndAuraFromSpellChain(player, 25291, 45.0f) : ObjectGuid::Empty;
    Unit const* flashHealTarget = (!isRetPaladin && IsSpellReady(player, healSpellId)) ? SelectFriendlyHealthTarget(player, 40.0f, 85.0f) : nullptr;
    // Holy Light is otherwise never used; once Flash of Light is learned it covers every heal tier.
    Unit const* bigFlashHealTarget = (!isRetPaladin && IsSpellReady(player, healSpellId)) ? SelectFriendlyHealthTarget(player, 40.0f, 60.0f) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, isRetPaladin && holyStrikeFlashHealTarget, 63.0f,
        { "paladin flash of light holy strike", "consume holy strike buff on the lowest-health friendly target", 19943, holyStrikeFlashHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, holyStrikeFlashHealTarget ? holyStrikeFlashHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRetPaladin && executeTarget && IsSpellReady(player, 89796), 62.5f,
        { "paladin holy strike", "use holy strike with very high priority", 89796, playerbot::PvpClassSpellContext::TargetMode::Enemy, executeTarget ? executeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, knowsSacrificialAura && !player->HasAura(kPaladinSacrificialAuraSpellId) &&
        IsSpellReady(player, kPaladinSacrificialAuraSpellId), 61.2f,
        { "paladin sacrificial aura", "prefer sacrificial aura when learned", kPaladinSacrificialAuraSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !knowsSacrificialAura && isRetPaladin && !HasAuraFromSpellChain(player, 20218) && IsSpellReady(player, 20218), 61.0f,
        { "paladin sanctity aura", "maintain sanctity aura for ret pressure", 20218, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRetPaladin && !HasAuraFromSpellChain(player, 20375) && IsSpellReady(player, 20375), 60.8f,
        { "paladin seal of command", "maintain seal of command", 20375, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->HealthBelowPct(20) && IsSpellReady(player, 1020), 60.0f,
        { "paladin divine shield", "emergency immunity under lethal pressure", 1020, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, rebukeTarget, 59.7f,
        { "paladin rebuke", "interrupt a nearby enemy cast", 81276, playerbot::PvpClassSpellContext::TargetMode::Enemy, rebukeTarget ? rebukeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, compelTarget, 58.5f,
        { "paladin compel", "pull a target from Compel range onto the tank", 62124, playerbot::PvpClassSpellContext::TargetMode::Enemy, compelTarget ? compelTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isProtPaladin && executeTarget && IsSpellReady(player, 32699), 45.5f,
        { "paladin avengers shield", "ranged pressure and silence on the kill target", 32699, playerbot::PvpClassSpellContext::TargetMode::Enemy, executeTarget ? executeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, emergencyLowAlly && IsSpellReady(player, healSpellId), 56.0f,
        { healActionName, "emergency heal for nearby ally below 25 percent health", healSpellId, emergencyLowAlly == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, emergencyLowAlly ? emergencyLowAlly->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !emergencyLowAlly && cleanseTarget, 55.0f,
        { "paladin cleanse", "prioritize cleansing allies", 4987, cleanseTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, cleanseTarget ? cleanseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, freedomTarget, 54.0f,
        { "paladin hand of freedom", "free snared or rooted ally", 1044, freedomTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, freedomTarget ? freedomTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates,
        sacrificeTarget && sacrificeTarget != player &&
        !HasAuraFromSpellChain(player, 6940) &&
        !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotHandOfSacrificeCooldownToken) &&
        !HasAuraFromSpellChain(sacrificeTarget, 6940) &&
        !HasAuraFromSpellChain(sacrificeTarget, 1022) &&
        !HasAuraFromSpellChain(sacrificeTarget, 1044), 53.0f,
        { "paladin hand of sacrifice", "keep hand of sacrifice cycling on allies", 6940, playerbot::PvpClassSpellContext::TargetMode::Ally, sacrificeTarget ? sacrificeTarget->GetGUID() : ObjectGuid::Empty });
    // 90526, not 26573: paladins hold the wrapper chain 90526-90531 (WrappedSpells.h).
    AddDecisionCandidate(candidates, CountNearbyEnemies(player, 8.0f) >= 2 && IsSpellReady(player, 90526), 52.0f,
        { "paladin consecration", "aoe pressure under close melee collapse", 90526, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, protectionTarget, 53.5f,
        { "paladin hand of protection", "protect low-health ally under melee pressure", 10278, playerbot::PvpClassSpellContext::TargetMode::Ally, protectionTarget ? protectionTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, repentanceTarget, 52.5f,
        { "paladin repentance", "interrupt enemy spellcast", 20066, playerbot::PvpClassSpellContext::TargetMode::Enemy, repentanceTarget ? repentanceTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, executeTarget && executeTarget->HealthBelowPct(20) && IsSpellReady(player, 24239), 51.0f,
        { "paladin hammer of wrath", "execute low-health enemy", 24239, playerbot::PvpClassSpellContext::TargetMode::Enemy, executeTarget ? executeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, stunTarget, 50.0f,
        { "paladin hammer of justice", "stun nearby cast target", 10308, playerbot::PvpClassSpellContext::TargetMode::Enemy, stunTarget ? stunTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, IsSpellReady(player, 20216) && player->IsInCombat(), 49.0f,
        { "paladin divine favor", "increase emergency heal throughput", 20216, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, flashHealTarget, 48.0f,
        { healActionName, "heal injured allies efficiently", healSpellId, flashHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, flashHealTarget ? flashHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, bigFlashHealTarget, 47.0f,
        { healActionName, "large heal for heavily injured ally", healSpellId, bigFlashHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, bigFlashHealTarget ? bigFlashHealTarget->GetGUID() : ObjectGuid::Empty });
    // Only ret ever put a seal up (Seal of Command), so every other paladin's
    // Judgement below waited on a seal that never came.
    AddDecisionCandidate(candidates, executeTarget && !HasActivePaladinSeal(player) &&
        !(isRetPaladin && IsSpellReady(player, 20375)) && IsSpellReady(player, 21084), 46.8f,
        { "paladin seal of righteousness", "hold a seal so judgement has something to release", 21084, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRetPaladin && stunnedJudgementTarget && IsSpellReady(player, 20271), 46.5f,
        { "paladin judgement", "judge nearby stunned enemy while seal of command is active", 20271, playerbot::PvpClassSpellContext::TargetMode::Enemy, stunnedJudgementTarget ? stunnedJudgementTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, executeTarget && HasActivePaladinSeal(player) && IsSpellReady(player, 20271), 46.0f,
        { "paladin judgement", "default offensive pressure when a seal is active", 20271, playerbot::PvpClassSpellContext::TargetMode::Enemy, executeTarget ? executeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !knowsSacrificialAura && !isRetPaladin && !player->HasAura(19746) && IsSpellReady(player, 19746), 20.0f,
        { "paladin concentration aura", "maintain concentration aura", 19746, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !player->IsInCombat() && isRetPaladin && IsSpellReady(player, 25291) && !HasAuraFromSpellChain(player, 25291), 19.0f,
        { "paladin blessing of might self", "ret paladin manually maintains lesser might on self", 25291, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() });
    AddDecisionCandidate(candidates, !player->IsInCombat() && isRetPaladin && !wisdomTargetGuid.IsEmpty(), 18.5f,
        { "paladin blessing of wisdom", "ret paladin manually maintains lesser wisdom on nearby mana allies", 25290, playerbot::PvpClassSpellContext::TargetMode::Ally, wisdomTargetGuid });
    AddDecisionCandidate(candidates, !player->IsInCombat() && isRetPaladin && !mightTargetGuid.IsEmpty(), 18.0f,
        { "paladin blessing of might", "ret paladin manually maintains lesser might on nearby non-mana allies", 25291, playerbot::PvpClassSpellContext::TargetMode::Ally, mightTargetGuid });

        return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
    }

    SpellDecision SelectWarlockSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
    {
        SpellDecision decision;
        if (!player)
            return decision;

    bool const isAfflictionWarlock = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
    bool const isDestructionWarlock = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    Pet const* pet = player->GetPet();
    bool const hasLivingPet = pet && pet->IsAlive();
    bool const needsPetSummon = !hasLivingPet && !IsPetParkedByMount(player);
    uint32 const specPetSpell = isAfflictionWarlock ? 691 : (isDestructionWarlock ? 712 : 697);
    // Felhunter is trained at 30 and Succubus at 20; until then summon the
    // best demon this warlock actually has.
    uint32 const summonPetSpell = ResolveKnownPlayerSpellInChain(player, specPetSpell) ? specPetSpell : SelectWarlockFallbackSummonSpell(player);
    char const* summonPetName = summonPetSpell != specPetSpell ? "warlock summon fallback demon" :
        (isAfflictionWarlock ? "warlock summon felhunter" : (isDestructionWarlock ? "warlock summon succubus" : "warlock summon voidwalker"));
    char const* summonPetReasonIdle = isAfflictionWarlock ? "maintain felhunter pet while out of combat" : (isDestructionWarlock ? "maintain succubus pet while out of combat" : "maintain voidwalker pet while out of combat");
    char const* summonPetReasonRecover = isAfflictionWarlock ? "recover felhunter out of combat when absent" : (isDestructionWarlock ? "recover succubus out of combat when absent" : "recover voidwalker out of combat when absent");
    bool const hasHostileTarget = HasHostileTarget(player, target);
    // A summon is a ten second cast, and the recovery below outranks every
    // offensive spell, so a warlock that came into a fight without its demon
    // stood through the whole cast in front of whoever it had just found -
    // a hunter at thirty yards killed it before the Voidwalker arrived. With
    // somebody that close it fights without the pet instead. Fel Domination
    // is how a warlock resummons under pressure, so a summon it has already
    // shortened still goes ahead.
    constexpr float kWarlockSummonThreatYards = 60.0f;
    bool const summonUnderThreat = needsPetSummon && !player->HasAura(18708) &&
        HasNearbyHostilePlayer(player, kWarlockSummonThreatYards, true);

    if (!hasHostileTarget)
    {
        if (needsPetSummon && !summonUnderThreat && !player->IsInCombat() && summonPetSpell && IsSpellReady(player, summonPetSpell))
            return { summonPetName, summonPetReasonIdle, summonPetSpell, playerbot::PvpClassSpellContext::TargetMode::Self };

            return decision;
        }

    bool const closePressure = player->IsWithinDistInMap(target, 8.0f);
    Unit const* fearTarget = IsSpellReady(player, 6215) ? SelectWarlockFearTarget(player, 20.0f) : nullptr;
    bool const petSpellLockReady = isAfflictionWarlock ? IsPetSpellReady(player, 19647) : IsPetSpellReady(player, 19244);
    Unit const* spellLockTarget = petSpellLockReady ? SelectEnemyCastingTarget(player, 30.0f, target) : nullptr;
    Unit const* devourEnemyTarget = (isAfflictionWarlock && IsPetSpellReady(player, 19736)) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, 30.0f) : nullptr;
    Unit const* devourFriendlyTarget = (isAfflictionWarlock && IsPetSpellReady(player, 19736)) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 30.0f) : nullptr;
    Item const* equippedOffhand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    bool const firestoneEquipped = equippedOffhand && equippedOffhand->GetEntry() == kWarlockFirestoneItemEntry;
    SpellInfo const* firestoneSpellInfo = sSpellMgr->GetSpellInfo(kWarlockFirestoneUseSpellId);
    float const firestoneRange = firestoneSpellInfo ? firestoneSpellInfo->GetMaxRange(false) : 0.0f;
    Unit const* firestoneTarget = (isDestructionWarlock && firestoneEquipped && IsOnUseItemReady(player, kWarlockFirestoneItemEntry) &&
        firestoneRange > 0.0f && !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken)) ?
        SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, firestoneRange) : nullptr;
    uint32 const spellstoneItemEntry = isAfflictionWarlock ? SelectReadyWarlockSpellstoneItemEntry(player) : 0;
    bool const hasSelfMagicDebuff = SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 0.0f) == player;
    bool const shouldUseSpellstone = spellstoneItemEntry != 0 && hasSelfMagicDebuff;

    bool const canUseVoidwalkerSacrifice = !isAfflictionWarlock && !isDestructionWarlock && player->HealthBelowPct(25) && !HasAuraFromSpellChain(player, 19443) &&
        hasLivingPet && IsPetSpellReady(player, 19443);
    Unit const* seduceCandidate = (isDestructionWarlock && IsPetSpellReady(player, 6358)) ? SelectRandomEnemyWithoutBreakableCrowdControl(player, 30.0f) : nullptr;
    Unit const* seduceTarget = (seduceCandidate && !HasDotAura(seduceCandidate)) ? seduceCandidate : nullptr;
    uint32 const shadowBoltSpellId = SelectWarlockShadowBoltSpell(player);

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, canUseVoidwalkerSacrifice, 70.0f,
        { "warlock sacrifice", "emergency voidwalker sacrifice at or below 25 percent health", 19443, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && spellLockTarget && IsPetSpellReady(player, 19244), 54.0f,
        { "warlock spell lock", "pet interrupt when available", 19244, playerbot::PvpClassSpellContext::TargetMode::Enemy, spellLockTarget ? spellLockTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isAfflictionWarlock && spellLockTarget, 57.0f,
        { "warlock spell lock", "felhunter interrupt on any nearby caster", 19647, playerbot::PvpClassSpellContext::TargetMode::Enemy, spellLockTarget ? spellLockTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, devourFriendlyTarget, 56.5f,
        { "warlock devour magic ally", "felhunter dispels friendly magic debuffs", 19736, playerbot::PvpClassSpellContext::TargetMode::Ally, devourFriendlyTarget ? devourFriendlyTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, devourEnemyTarget, 56.0f,
        { "warlock devour magic enemy", "felhunter dispels enemy magic buffs", 19736, playerbot::PvpClassSpellContext::TargetMode::Enemy, devourEnemyTarget ? devourEnemyTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, firestoneTarget, 56.2f,
        { "warlock firestone", "use the equipped offhand firestone to purge enemy magic buffs", kWarlockFirestoneUseSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, firestoneTarget ? firestoneTarget->GetGUID() : ObjectGuid::Empty, kWarlockFirestoneItemEntry });
    AddDecisionCandidate(candidates, fearTarget, 53.0f,
        { "warlock fear", "prioritize fear control on paladin/priest targets in range", 6215, playerbot::PvpClassSpellContext::TargetMode::Enemy, fearTarget ? fearTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && !player->IsInCombat() && needsPetSummon && !player->HasAura(18708) && IsSpellReady(player, 18708), 52.0f,
        { "warlock fel domination", "prepare instant out-of-combat pet recovery before demon summon", 18708, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, needsPetSummon && !summonUnderThreat && !player->IsInCombat() && summonPetSpell && IsSpellReady(player, summonPetSpell), 51.0f,
        { summonPetName, summonPetReasonRecover, summonPetSpell, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !isAfflictionWarlock && !player->HasAura(25228) && IsSpellReady(player, 19028), 45.0f,
        { "warlock soul link", "maintain soul link when pet is available", 19028, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, seduceTarget, 52.5f,
        { "warlock seduce", "succubus crowd control on a non-cced, non-dotted target", 6358, playerbot::PvpClassSpellContext::TargetMode::Enemy, seduceTarget ? seduceTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, target->GetPowerType() == POWER_MANA && ShouldUseCurseOfTongues(target) && !HasAuraFromSpellChain(target, 11719) &&
            !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11719) && IsSpellReady(player, 11719), 36.0f,
        { "warlock curse of tongues", "slow enemy casting throughput", 11719, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isDestructionWarlock && !HasAuraFromSpellChain(target, 11722) &&
            !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11722) && IsSpellReady(player, 11722), 35.5f,
        { "warlock curse of the elements", "keep the kill target cursed", 11722, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    bool const shouldApplyCurseOfAgony = !isDestructionWarlock && !IsCasterClass(target) && !HasAuraFromSpellChain(target, 11713) && !HasAuraFromSpellChain(target, 11719) &&
        !playerbot::PvpClassActions::IsWarlockCurseTargetCooldownActive(player, target, 11713) && IsSpellReady(player, 11713);
    // Amplify Curse boosts whatever curse lands next and is off the global
    // cooldown, so it should land immediately before Curse of Agony rather
    // than being sequenced across ticks like a normal candidate - see the
    // cadence-bypass whitelist in ProcessLifecycleEntryPoint (18288 there).
    AddDecisionCandidate(candidates, shouldApplyCurseOfAgony && !HasAuraFromSpellChain(player, 18288) && IsSpellReady(player, 18288), 35.2f,
        { "warlock amplify curse", "boost the curse about to land on the kill target", 18288, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, shouldApplyCurseOfAgony, 35.0f,
        { "warlock curse of agony", "apply curse of agony pressure to non-caster players", 11713, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, shouldUseSpellstone, 56.7f,
        { "warlock spellstone", "use spellstone to remove magic debuffs", 0, playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), spellstoneItemEntry });
    AddDecisionCandidate(candidates, isAfflictionWarlock && !HasAuraFromSpellChain(target, 48181) && IsSpellReady(player, 48181), 34.5f,
        { "warlock haunt", "maintain haunt on kill target", 48181, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isDestructionWarlock && HasAuraFromSpellChain(target, 25309) && IsSpellReady(player, 18932), 34.4f,
        { "warlock conflagrate", "consume immolate for burst damage", 18932, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isDestructionWarlock && !HasAuraFromSpellChain(target, 25309) && IsSpellReady(player, 25309), 34.2f,
        { "warlock immolate", "apply immolate to enable conflagrate", 25309, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isDestructionWarlock && target && IsSpellReady(player, 18871), 33.8f,
        { "warlock shadowburn", "instant execute pressure on the kill target", 18871, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !isDestructionWarlock && !HasAuraFromSpellChain(target, 11672) && IsSpellReady(player, 11672), 34.0f,
        { "warlock corruption", "maintain corruption dot", 11672, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, (target->HealthBelowPct(20) || (closePressure && IsMeleeClass(target))) && IsSpellReady(player, 17926), 33.0f,
        { "warlock death coil", "peel melee or finish low enemy target", 17926, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isAfflictionWarlock && CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 17928), 32.5f,
        { "warlock howl of terror", "fear clustered nearby enemies", 17928, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isDestructionWarlock && target && CountNearbyEnemies(player, 15.0f) >= kMassAoeEnemyCountThreshold && IsSpellReady(player, 5740), 32.7f,
        { "warlock rain of fire", "ground-targeted aoe against a clustered enemy group", 5740, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, player->GetPower(POWER_MANA) < 400 && IsSpellReady(player, 11689), 32.0f,
        { "warlock life tap", "convert health to mana for sustained casting", 11689, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, IsLowOrOutOfManaForFallback(player) && IsWandShootReadyForDecision(player), 31.5f,
        { "warlock shoot wand", "fallback to wand pressure while low on mana", kWandShootSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, player->HasAura(17941) && IsSpellReady(player, shadowBoltSpellId), 20.0f,
        { "warlock shadow bolt", "consume nightfall proc for instant pressure", shadowBoltSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Destruction should never default-cast Shadow Bolt - only ever on a
    // Nightfall proc (above). Its own filler is Searing Pain once Immolate
    // is already ticking and both Conflagrate and Shadowburn are on
    // cooldown, not a Shadow Bolt spam loop.
    AddDecisionCandidate(candidates, isDestructionWarlock && HasAuraFromSpellChain(target, 25309) &&
            !IsSpellReady(player, 18932) && !IsSpellReady(player, 18871) && IsSpellReady(player, 17923), 19.5f,
        { "warlock searing pain", "filler pressure while conflagrate and shadowburn are both on cooldown", 17923, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Before Searing Pain (18) Destruction has nothing but Immolate, and
    // before Drain Life (14) Affliction has only its dots. Shadow Bolt is the
    // level-one nuke both fall back to until their own filler is trained.
    bool const shadowBoltIsSpecFallback =
        (isDestructionWarlock && HasAuraFromSpellChain(target, 25309) && !ResolveKnownPlayerSpellInChain(player, 17923)) ||
        (isAfflictionWarlock && !ResolveKnownPlayerSpellInChain(player, 11700));
    AddDecisionCandidate(candidates, (shadowBoltIsSpecFallback || (!isAfflictionWarlock && !isDestructionWarlock)) && IsSpellReady(player, shadowBoltSpellId), 19.0f,
        { "warlock shadow bolt", shadowBoltIsSpecFallback ? "nuke before the spec filler is trained" : "default ranged pressure", shadowBoltSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isAfflictionWarlock && IsSpellReady(player, 11700), 18.0f,
        { "warlock drain life", "fallback affliction channel", 11700, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, HasHostileTarget(player, target) && target && !HasBreakableCrowdControl(player, target) && IsWandShootReadyForDecision(player), 8.0f,
        { "warlock shoot wand", "default offensive fallback when no better warlock action is available", kWandShootSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });

        return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
    }

struct WarriorGapCloserDiagnosticEntry
{
    uint32 recordedMs = 0;
    std::string text;
};

std::unordered_map<uint64, WarriorGapCloserDiagnosticEntry> g_WarriorGapCloserDiagnosticByGuid;

// Every Man for Himself gets its own slot rather than sharing the gap-closer
// one. It fires once and the shared slot is rewritten on the very next decision
// tick, so by the time anyone can type a command the evidence is gone. This
// records only when the racial is actually chosen, so it persists until the
// next time it happens.
std::unordered_map<uint64, WarriorGapCloserDiagnosticEntry> g_EveryManForHimselfDiagnosticByGuid;

void SetEveryManForHimselfDiagnostic(Player const* player, std::string const& diagnostic)
{
    if (!player)
        return;

    WarriorGapCloserDiagnosticEntry& entry =
        playerbot::LockedGetOrCreate(g_EveryManForHimselfDiagnosticByGuid, player->GetGUID().GetRawValue());
    entry.recordedMs = GameTime::GetGameTimeMS();
    entry.text = diagnostic;
}

// The timestamp is not decoration. This snapshot is only refreshed when the
// warrior selector actually runs, so a stale entry looks exactly like a live
// one and silently describes a moment that has passed - which is precisely how
// a frozen "cannot afford intercept" reading got mistaken for the live state.
void SetWarriorGapCloserDiagnostic(Player const* player, std::string const& diagnostic)
{
    if (!player)
        return;

    WarriorGapCloserDiagnosticEntry& entry =
        playerbot::LockedGetOrCreate(g_WarriorGapCloserDiagnosticByGuid, player->GetGUID().GetRawValue());
    entry.recordedMs = GameTime::GetGameTimeMS();
    entry.text = diagnostic;
}

SpellDecision SelectWarriorSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
        return decision;

    // Record the early exits too. A selector that never reaches the gap-closer
    // logic leaves the previous snapshot in place, which reads as live data and
    // hides the fact that nothing evaluated at all this tick.
    if (!HasHostileTarget(player, target))
    {
        SetWarriorGapCloserDiagnostic(player, std::string("early_exit=no_hostile_target target=") +
            (target ? target->GetName() : "null"));
        return decision;
    }

    Unit const* activeTarget = SelectWarriorPriorityTarget(player, target, 25.0f);
    if (!HasHostileTarget(player, activeTarget))
        activeTarget = target;

    // Charge, Intercept, and Heroic Leap drive their own effect-motion spline.
    // While that movement is resolving the warrior is locked in, so defer all
    // spell decisions until the native gap-closer motion finishes.
    if (HasActiveMovementEffectSpline(player))
    {
        SetWarriorGapCloserDiagnostic(player, "early_exit=movement_effect_spline");
        return decision;
    }

    bool const isProtWarrior = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const isFuryWarrior = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    bool const inBattleStance = player->HasAura(2457);
    bool const inDefensiveStance = player->HasAura(71);
    bool const inBerserkerStance = player->HasAura(2458);
    // Charge, Intercept, and Heroic Leap drive their own effect-motion spline.
    // Do not pick stance-swap actions while that spline is still resolving.
    bool const warriorGapCloserInFlight = HasActiveMovementEffectSpline(player);
    Unit const* gapCloseTarget = HasHostileTarget(player, target) ? target : activeTarget;
    SpellInfo const* chargeInfo = sSpellMgr->GetSpellInfo(11578);
    SpellInfo const* interceptInfo = sSpellMgr->GetSpellInfo(20617);
    float const chargeMinRange = chargeInfo ? chargeInfo->GetMinRange(false) : 8.0f;
    float const interceptMinRange = interceptInfo ? interceptInfo->GetMinRange(false) : 8.0f;
    bool const canChargeByRange = !player->IsWithinDistInMap(gapCloseTarget, chargeMinRange);
    bool const canInterceptByRange = !player->IsWithinDistInMap(gapCloseTarget, interceptMinRange);
    // The core refused a charge's path a moment ago (CastDirectSpell arms
    // this). Picked again it is refused again, and the bot stands still at
    // range instead of walking in.
    bool const chargePathRefused = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotChargePathRefusedToken);
    bool const shouldUseChargeGapCloser = !chargePathRefused && !player->IsWithinMeleeRange(gapCloseTarget) && !player->IsInCombat() && canChargeByRange && IsSpellReady(player, 11578);
    // "Wants" = range and cooldown allow it; "should" additionally requires the
    // rage to pay for it. Intercept and Heroic Leap cost 10 rage, and a warrior
    // pulled into combat at range (a Moonfire landing before Charge goes off) has
    // none. Keeping these separate matters because a stance change caps rage to
    // the Tactical Mastery value - 0 without the talent - so the swap that
    // Intercept requires destroys any rage generated before it. The stance swap
    // is therefore driven by "wants", and rage generation is sequenced after it.
    // Fury is routed to Heroic Leap rather than Intercept. Heroic Leap is a
    // custom spell (81271) the bot may simply not have been granted, and when
    // that happens Fury loses Intercept to the spec check and Heroic Leap to
    // the unknown-spell check - leaving it with no in-combat gap closer at all,
    // since Charge requires being out of combat. Fall back to Intercept rather
    // than jogging the whole way.
    bool const knowsHeroicLeap = ResolveKnownPlayerSpellInChain(player, 81271) != 0;
    bool const interceptIsThisSpecGapCloser = !isFuryWarrior || !knowsHeroicLeap;
    bool const wantsInterceptGapCloser = !chargePathRefused && interceptIsThisSpecGapCloser && !player->IsWithinMeleeRange(gapCloseTarget) && player->IsInCombat() && canInterceptByRange && IsSpellReady(player, 20617);
    bool const shouldUseInterceptGapCloser = wantsInterceptGapCloser && CanAffordSpellPowerCost(player, 20617);
    // Fury uses Heroic Leap (ground-targeted, same range band as Intercept)
    // instead of Intercept. This was missing the same min-range gate
    // Intercept has above - close-but-not-melee distances inside Intercept's
    // min range would try to fire Heroic Leap anyway and fail the spell's own
    // range validation, silently doing nothing.
    bool const wantsHeroicLeapGapCloser = isFuryWarrior && !player->IsWithinMeleeRange(gapCloseTarget) && player->IsInCombat() && canInterceptByRange && IsSpellReady(player, 81271);
    bool const shouldUseHeroicLeapGapCloser = wantsHeroicLeapGapCloser && CanAffordSpellPowerCost(player, 81271);
    bool const furyInDanger = isFuryWarrior && player->HealthBelowPct(35) && SelectNearbyMeleeTarget(player, activeTarget, 8.0f) && IsSpellReady(player, 81271);
    bool const furyRecentGapCloser = isFuryWarrior && (player->GetSpellHistory()->HasCooldown(11578) || player->GetSpellHistory()->HasCooldown(81271));
    uint32 const meleeFinisherSpellId = isProtWarrior ? uint32(23925) : (isFuryWarrior ? uint32(23881) : uint32(21553));
    // Bloodrage/Battle Shout are self-targeted, so the immediate-castability
    // check never fails them for range - they are always "ready" whenever
    // their own condition holds. Sitting above the gap closers in priority,
    // they permanently starve Charge/Intercept/Heroic Leap from ever being
    // selected while out of melee range, since the selector always returns
    // the first immediately-castable candidate scanning top-down rather than
    // preferring an out-of-range action that would trigger approach movement.
    bool const gapCloseUrgent = shouldUseChargeGapCloser || shouldUseInterceptGapCloser || shouldUseHeroicLeapGapCloser;
    // Intent, regardless of whether the rage is there yet. Used to keep the
    // ordinary rotational Bloodrage from firing a tick before a stance swap
    // would wipe the rage it just made.
    bool const gapCloseWanted = shouldUseChargeGapCloser || wantsInterceptGapCloser || wantsHeroicLeapGapCloser;
    // The deadlock this breaks: warrior is pulled into combat at range, so
    // Charge is off the table and Intercept/Heroic Leap is the only gap closer
    // left - but both cost rage the warrior does not have, and at range there is
    // nothing to hit to make more. Bloodrage is the only source, and it is off
    // the GCD, so spending a decision tick on it costs nothing but the tick.
    // Gated on already being in Berserker Stance because the swap into it caps
    // rage back to 0; generating first would just be thrown away.
    bool const gapCloserRageStarved = (wantsInterceptGapCloser || wantsHeroicLeapGapCloser) &&
        !gapCloseUrgent && inBerserkerStance && IsSpellReady(player, 2687);

    {
        // Note gapclose_target: this is the decision layer's target, which is
        // not necessarily the unit shown as the bot's victim, and it may be a
        // creature or pet rather than a player. A nearby one collapses
        // intercept_by_range and silently removes the gap closer.
        std::ostringstream gapDiag;
        gapDiag << "fury=" << (isFuryWarrior ? "yes" : "no")
                << " prot=" << (isProtWarrior ? "yes" : "no")
                << " knows_leap=" << (knowsHeroicLeap ? "yes" : "no")
                << " gapclose_target=" << (gapCloseTarget ? gapCloseTarget->GetName() : "none")
                << " gapclose_dist=" << (gapCloseTarget ? player->GetDistance(gapCloseTarget) : -1.0f)
                << " in_melee=" << (gapCloseTarget && player->IsWithinMeleeRange(gapCloseTarget) ? "yes" : "no")
                << " combat=" << (player->IsInCombat() ? "yes" : "no")
                << " icept_minrange=" << interceptMinRange
                << " by_range=" << (canInterceptByRange ? "yes" : "no")
                << " icept_ready=" << (IsSpellReady(player, 20617) ? "yes" : "no")
                << " icept_afford=" << (CanAffordSpellPowerCost(player, 20617) ? "yes" : "no")
                << " wants_icept=" << (wantsInterceptGapCloser ? "yes" : "no")
                << " should_icept=" << (shouldUseInterceptGapCloser ? "yes" : "no")
                << " berserker=" << (inBerserkerStance ? "yes" : "no")
                << " stance_ready=" << (IsSpellReady(player, 2458) ? "yes" : "no")
                << " spline_inflight=" << (warriorGapCloserInFlight ? "yes" : "no")
                << " charge_ok=" << (shouldUseChargeGapCloser ? "yes" : "no");
        SetWarriorGapCloserDiagnostic(player, gapDiag.str());
    }
    Unit const* nearbyMeleeTarget = SelectNearbyMeleeTarget(player, activeTarget, 8.0f);
    Unit const* nearbyCastingTarget = SelectEnemyCastingTarget(player, 8.0f, activeTarget);
    Unit const* tauntTarget = isProtWarrior && IsSpellReady(player, 355) ? SelectWarriorTauntTarget(player, activeTarget, 30.0f) : nullptr;
    // Intervene requires Defensive Stance (never Berserker). Prot
    // warriors already live in Defensive Stance via the stance rules below,
    // so no dedicated stance-swap prerequisite is needed here.
    Unit const* interveneTarget = isProtWarrior && inDefensiveStance && IsSpellReady(player, 3411) ?
        SelectFriendlyMeleePressureTarget(player, 25.0f, 100.0f) : nullptr;
    if (interveneTarget == player || (interveneTarget && player->IsWithinMeleeRange(interveneTarget)))
        interveneTarget = nullptr;
    bool const revengeReady = isProtWarrior && player->IsWithinMeleeRange(activeTarget) && IsSpellReadyAndCasterAuraAllowed(player, 25288);
    // Overpower: reactive, Battle Stance only, and usable only in the short
    // window after the target dodges - IsSpellReadyAndCasterAuraAllowed is
    // what tests that aura state, the same way Revenge does above. No stance
    // dance for it: arms already fights in Battle Stance, and forcing fury or
    // prot out of theirs to land a 5-rage hit is a losing trade.
    bool const overpowerReady = inBattleStance && activeTarget &&
        player->IsWithinMeleeRange(activeTarget) &&
        IsSpellReadyAndCasterAuraAllowed(player, 7384);
    bool const executeReady = activeTarget && activeTarget->HealthBelowPct(20) && IsSpellReady(player, 20662) && player->GetPower(POWER_RAGE) >= 150;
    bool const hasNearbyMeleeThreat = HasHostileTarget(player, nearbyMeleeTarget);
    bool const nearbyMeleeThreatSnared = hasNearbyMeleeThreat && nearbyMeleeTarget->HasAuraWithMechanic(1 << MECHANIC_SNARE);
    bool const canDisarmNearbyMeleeThreat = hasNearbyMeleeThreat &&
        nearbyMeleeThreatSnared &&
        player->IsWithinMeleeRange(nearbyMeleeTarget) &&
        nearbyMeleeTarget->CanUseAttackType(BASE_ATTACK) &&
        !HasAuraFromSpellChain(nearbyMeleeTarget, 676);

    std::vector<PrioritizedSpellDecision> candidates;
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && player->HasAuraWithMechanic(1 << MECHANIC_FEAR) && !inBerserkerStance && IsSpellReady(player, 2458), 60.5f,
        { "warrior berserker stance", "swap to berserker stance to break fear", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->HasAuraWithMechanic(1 << MECHANIC_FEAR) && inBerserkerStance && IsSpellReady(player, 18499), 60.0f,
        { "warrior berserker rage", "break fear-like control while in berserker stance", 18499, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isProtWarrior && player->HealthBelowPct(25) && IsSpellReady(player, 12975), 61.0f,
        { "warrior last stand", "emergency health cooldown below 25 percent", 12975, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isProtWarrior && HasHostileTarget(player, nearbyCastingTarget) && (inDefensiveStance || inBattleStance) && HasShieldEquipped(player) && IsSpellReady(player, 1672), 60.0f,
        { "warrior shield bash", "shield bash nearby spellcasts", 1672, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget ? nearbyCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isProtWarrior && HasHostileTarget(player, nearbyCastingTarget) && IsSpellReady(player, 6552), 59.0f,
        { "warrior pummel", "interrupt nearby spellcasts", 6552, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget ? nearbyCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, canDisarmNearbyMeleeThreat && inDefensiveStance && IsSpellReady(player, 81492), 58.0f,
        { "warrior disarm", "disarm threatening melee weapon users", 81492, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyMeleeTarget ? nearbyMeleeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && canDisarmNearbyMeleeThreat && !inDefensiveStance && IsSpellReady(player, 81492) && IsSpellReady(player, 71) && player->GetPower(POWER_RAGE) >= 200, 57.0f,
        { "warrior defensive stance", "swap defensive before disarm against melee", 71, playerbot::PvpClassSpellContext::TargetMode::Self });
    // Not while a gap closer is wanted. SelectWarriorTauntTarget falls back to
    // the kill target whenever it is not attacking the warrior - the whole of
    // an out-of-combat approach - and this swap outranks the Battle Stance one
    // Charge waits on. Taunt needs Defensive Stance and Charge needs Battle, so
    // a protection warrior swapped back and forth all the way in and arrived
    // without ever charging. Intercept was already exempt, but only once the
    // rage was there; "wanted" is what drives the stance swaps (see above).
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && tauntTarget && !inDefensiveStance && !gapCloseWanted && IsSpellReady(player, 71), 57.2f,
        { "warrior defensive stance", "swap defensive before taunt", 71, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && revengeReady && !inDefensiveStance && IsSpellReady(player, 71), 57.1f,
        { "warrior defensive stance", "swap defensive before revenge", 71, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, interveneTarget, 56.85f,
        { "warrior intervene", "rush to an ally under melee pressure and draw their attacker's aggro", 3411, playerbot::PvpClassSpellContext::TargetMode::Ally, interveneTarget ? interveneTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, tauntTarget && inDefensiveStance, 56.8f,
        { "warrior taunt", "taunt enemy pressuring an ally or current kill target", 355, playerbot::PvpClassSpellContext::TargetMode::Enemy, tauntTarget ? tauntTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, CountNearbyUnsNaredEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 12323), 56.0f,
        { "warrior piercing howl", "apply area snare when multiple enemies are unsnared in melee range", 12323, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, HasHostileTarget(player, activeTarget) && CountNearbyEnemies(player, 10.0f) >= 2 && IsSpellReady(player, 5246), 55.5f,
        { "warrior intimidating shout", "aoe fear around the current target when outnumbered", 5246, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    // Heroic Strike is on the list for the low-level warrior: everything else
    // here arrives at 30 or later, so below that Bloodrage never fired even
    // though the whole rotation was waiting on rage.
    AddDecisionCandidate(candidates, !gapCloseWanted && (IsSpellReady(player, 6552) || IsSpellReady(player, 81492) || IsSpellReady(player, 20617) || IsSpellReady(player, 1680) || IsSpellReady(player, meleeFinisherSpellId) ||
            (player->IsInCombat() && activeTarget && player->IsWithinMeleeRange(activeTarget) && IsSpellReady(player, 78))) &&
            player->GetPower(POWER_RAGE) < 150 && IsSpellReady(player, 2687), 54.0f,
        { "warrior bloodrage", "generate rage to unlock rotational abilities", 2687, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && !isProtWarrior && inDefensiveStance && (!IsSpellReady(player, 81492) || !hasNearbyMeleeThreat) && IsSpellReady(player, 2458), 53.0f,
        { "warrior berserker stance", "leave defensive stance when disarm is unavailable or no melee threat is nearby", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && shouldUseChargeGapCloser && !inBattleStance && IsSpellReady(player, 2457), 52.5f,
        { "warrior battle stance", "switch to battle stance before out-of-combat charge", 2457, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, shouldUseChargeGapCloser, 52.0f,
        { "warrior charge", "close gap to target from out of combat", 11578, playerbot::PvpClassSpellContext::TargetMode::Enemy, gapCloseTarget ? gapCloseTarget->GetGUID() : ObjectGuid::Empty });
    // Driven by intent rather than affordability: the swap itself wipes rage, so
    // waiting until Intercept is affordable would mean swapping and immediately
    // being unable to pay again. Still requires a way to end up with rage - if
    // Bloodrage is down and the warrior is broke, skip the swap entirely and let
    // the ordinary chase take him into melee instead of stalling at range.
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && wantsInterceptGapCloser && !inBerserkerStance && IsSpellReady(player, 2458) &&
            (CanAffordSpellPowerCost(player, 20617) || IsSpellReady(player, 2687)), 51.5f,
        { "warrior berserker stance", "switch to berserker stance before intercept gap close", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, gapCloserRageStarved, 51.25f,
        { "warrior bloodrage", "generate rage to pay for the gap closer after the stance swap", 2687, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, shouldUseInterceptGapCloser, 51.0f,
        { "warrior intercept", "close gap to target while in combat", 20617, playerbot::PvpClassSpellContext::TargetMode::Enemy, gapCloseTarget ? gapCloseTarget->GetGUID() : ObjectGuid::Empty });
    // Heroic Leap's ShapeshiftMask requires Berserker Stance (same shapeshift
    // mechanism as Druid forms/Ghost Wolf), and there was no stance-swap
    // candidate for it - unlike Intercept, which already has one just above.
    // Without this, the generic shapeshift-compatibility check silently
    // rejects Heroic Leap whenever the warrior isn't already in Berserker
    // Stance, and the bot falls back to Charge (which doesn't need it).
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && (wantsHeroicLeapGapCloser || furyInDanger) && !inBerserkerStance && IsSpellReady(player, 2458) &&
            (CanAffordSpellPowerCost(player, 81271) || IsSpellReady(player, 2687)), 1000.0f,
        { "warrior berserker stance", "switch to berserker stance before heroic leap", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    // TEMPORARY: forced to top priority (above everything, including
    // emergency defensives) at the user's explicit request to isolate and
    // test whether Heroic Leap fires at all once the stance prerequisite is
    // met. Revert to a normal tier once confirmed working.
    AddDecisionCandidate(candidates, shouldUseHeroicLeapGapCloser, 999.0f,
        { "warrior heroic leap", "close gap to target with heroic leap instead of intercept", 81271, playerbot::PvpClassSpellContext::TargetMode::Enemy, gapCloseTarget ? gapCloseTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, furyInDanger, 999.5f,
        { "warrior heroic leap", "leap away from danger instead of intercept", 81271, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, furyRecentGapCloser && !HasAuraFromSpellChain(player, 12328) && IsSpellReady(player, 12328), 49.5f,
        { "warrior death wish", "pop death wish right after closing with charge or heroic leap", 12328, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && !isProtWarrior && player->IsWithinMeleeRange(activeTarget) && !inBerserkerStance &&
            IsSpellReady(player, 1680) && IsSpellReady(player, 2458), 50.4f,
        { "warrior berserker stance", "switch to berserker stance to enable whirlwind in melee", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    // Execute works in Battle Stance too. Swapping out of it would throw the
    // rage away, and before Berserker Stance (30) it was the only way the
    // warrior could ever Execute.
    AddDecisionCandidate(candidates, !warriorGapCloserInFlight && executeReady && !inBerserkerStance && !inBattleStance && IsSpellReady(player, 2458), 50.2f,
        { "warrior berserker stance", "switch to berserker stance before execute", 2458, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, executeReady && (inBerserkerStance || inBattleStance), 50.0f,
        { "warrior execute", "finisher at low enemy health", 20662, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !gapCloseUrgent && !HasAuraFromSpellChain(player, 25289) && IsSpellReady(player, 25289), 40.0f,
        { "warrior battle shout", "maintain attack power buff", 25289, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, revengeReady && inDefensiveStance, 40.5f,
        { "warrior revenge", "use reactive revenge whenever available", 25288, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    // Above Mortal Strike/Slam tier: the dodge window is only a few seconds
    // and Overpower cannot be dodged, parried or blocked, so it is always the
    // better rage spend while it is up.
    AddDecisionCandidate(candidates, overpowerReady, 40.6f,
        { "warrior overpower", "reactive strike in the window after the target dodges", 7384, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    Unit const* concussionTarget = isProtWarrior && IsSpellReady(player, 12809) ? SelectUnstunDREnemyTarget(player, activeTarget, 5.0f, 12809) : nullptr;
    AddDecisionCandidate(candidates, concussionTarget, 39.6f,
        { "warrior concussion blow", "stun a target without stun diminishing returns", 12809, playerbot::PvpClassSpellContext::TargetMode::Enemy, concussionTarget ? concussionTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && CountNearbyEnemies(player, 10.0f) >= kMassAoeEnemyCountThreshold &&
            player->GetPower(POWER_RAGE) >= 500 && IsSpellReady(player, 1680), 44.0f,
        { "warrior whirlwind", "prioritize aoe cleave over single-target pressure when surrounded", 1680, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) &&
            (isProtWarrior ? !HasAuraFromSpellChain(activeTarget, 11597) : (!HasAuraFromSpellChain(activeTarget, 7373) ||
                    [&]() { Aura const* snare = GetAuraFromSpellChain(activeTarget, 7373); return snare && snare->GetDuration() < 2000; }())) &&
            IsSpellReady(player, isProtWarrior ? uint32(11597) : uint32(7373)), 39.0f,
        { isProtWarrior ? "warrior sunder armor" : "warrior hamstring", isProtWarrior ? "apply sunder armor as protection filler" : "maintain stickiness snare", isProtWarrior ? uint32(11597) : uint32(7373), playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    char const* meleeFinisherName = isProtWarrior ? "warrior shield slam" : (isFuryWarrior ? "warrior bloodthirst" : "warrior mortal strike");
    char const* meleeFinisherReason = isProtWarrior ? "protection kill target pressure" : (isFuryWarrior ? "fury primary strike instead of mortal strike" : "arms-like burst pressure");
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && (isProtWarrior || isFuryWarrior || !HasAuraFromSpellChain(activeTarget, 21553)) &&
            IsSpellReady(player, meleeFinisherSpellId), 38.0f,
        { meleeFinisherName, meleeFinisherReason, meleeFinisherSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
    // Rogues always get Rend (it keeps them out of stealth). Anyone else gets
    // it only while the spec strike is untrained: until 40 the warrior has
    // little else to spend rage on.
    AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) &&
            (activeTarget->GetClass() == CLASS_ROGUE || !ResolveKnownPlayerSpellInChain(player, meleeFinisherSpellId)) &&
            !HasAuraFromSpellChain(activeTarget, 11574) && IsSpellReady(player, 11574), 37.0f,
            { "warrior rend", activeTarget->GetClass() == CLASS_ROGUE ? "apply anti-stealth bleed pressure on rogues" : "bleed the target before the spec strike is trained",
                11574, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });
        AddDecisionCandidate(candidates, player->IsWithinMeleeRange(activeTarget) && player->GetPower(POWER_RAGE) >= 500 && IsSpellReady(player, 1680), 36.0f,
            { "warrior whirlwind", "fallback aoe melee pressure", 1680, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });

    // Heroic Strike: the rage dump, and the lowest-priority thing a warrior
    // ever does - which is exactly why it was missing. Every other entry here
    // is a talent, a stance ability or a reactive proc, so a warrior below the
    // level where those arrive had NOTHING in the table at all and just
    // auto-attacked. It sits under everything else, so it only ever spends
    // rage nothing better wanted; at low level nothing better exists and it
    // becomes the rotation, which is correct for the bracket.
    AddDecisionCandidate(candidates, activeTarget && player->IsWithinMeleeRange(activeTarget) &&
            player->GetPower(POWER_RAGE) >= 150 && IsSpellReady(player, 78), 12.0f,
        { "warrior heroic strike", "spend surplus rage on the next swing", 78, playerbot::PvpClassSpellContext::TargetMode::Enemy, activeTarget ? activeTarget->GetGUID() : ObjectGuid::Empty });

        return SelectHighestPriorityCastableDecision(candidates, player, activeTarget, nullptr);
    }

    // The spell a stealthed rogue opens with, or 0 when it knows none.
    //
    // Asphyxiate (81302, the Assassination capstone) makes Garrote silence and
    // carry both weapons' poisons, so a rogue who took it opens with Garrote.
    // Everyone else opens with Cheap Shot, and a rogue not yet trained in it
    // (26; Garrote comes at 14) opens with Garrote instead. A rogue with
    // neither gets 0 and fights from stealth off the ordinary table, which
    // means Sinister Strike.
    //
    // It is the TALENT that decides, not the spec guess. The Assassination
    // profile also covers a rogue that merely has most of its points in the
    // tree, and that rogue was being held to Garrote - a positional opener
    // that anyone facing it refuses - with no Cheap Shot on offer at all.
    //
    // Asked of the whole rank chain: Garrote's later ranks are unrelated ids,
    // so HasSpell(703) is false for anyone who trained past rank 1. Cooldown
    // is deliberately left out, so which opener is "mine" cannot flicker.
    uint32 SelectRogueStealthOpenerSpellId(Player const* player)
    {
        if (!player || player->GetClass() != CLASS_ROGUE)
            return 0;

        constexpr uint32 kRogueGarroteSpellId = 703;
        constexpr uint32 kRogueCheapShotSpellId = 1833;
        constexpr uint32 kRogueAsphyxiateTalentSpellId = 81302;

        bool const knowsGarrote = ResolveKnownPlayerSpellInChain(player, kRogueGarroteSpellId) != 0;
        bool const knowsCheapShot = ResolveKnownPlayerSpellInChain(player, kRogueCheapShotSpellId) != 0;

        if (knowsGarrote && player->HasTalent(kRogueAsphyxiateTalentSpellId, player->GetActiveSpec()))
            return kRogueGarroteSpellId;
        if (knowsCheapShot)
            return kRogueCheapShotSpellId;
        return knowsGarrote ? kRogueGarroteSpellId : 0;
    }

    SpellDecision SelectRogueSpell(Player const* player, Unit const* target, ClassicProfileSelection const& profileSelection)
    {
        SpellDecision decision;
        if (!player)
            return decision;

        if (!HasHostileTarget(player, target))
            return decision;

    bool const isCombatRogue = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    bool const isAssassinationRogue = profileSelection.profile == ClassicClassProfile::PrimaryClassic;
    bool const hasImprovedBackstab = isAssassinationRogue && player->HasTalent(13866, player->GetActiveSpec());
    Unit const* blindTarget = IsSpellReady(player, 2094) ? SelectRogueBlindTarget(player, target, 15.0f) : nullptr;
    Unit const* nearbyCastingTarget = IsSpellReady(player, 1766) ? SelectEnemyCastingTarget(player, 5.0f, target) : nullptr;
    Unit const* nearbyMeleeTarget = SelectNearbyMeleeTarget(player, target, 5.0f);
    bool const rootedOrSnared = IsRootedOrSnared(player);

        std::vector<PrioritizedSpellDecision> candidates;
        // Disabled: weapon-poison automation from PvP decision loop.
        // This avoids touching weapon-enchant mutation paths while investigating combat-time crashes.

    AddDecisionCandidate(candidates, !player->IsInCombat() && !HasAuraFromSpellChain(player, 1784) && IsSpellReady(player, 1784), 50.0f,
        { "rogue stealth", "enter stealth before engagement", 1784, playerbot::PvpClassSpellContext::TargetMode::Self });
    // The stealth opener: Garrote with Asphyxiate, otherwise Cheap Shot, and
    // Garrote for a rogue too young for Cheap Shot (SelectRogueStealthOpenerSpellId).
    // Garrote carries SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET, so the shared
    // cast-failure handling in CastDirectSpell repositions the bot behind the
    // target (IssueBehindTargetMeleeMovement) before retrying the cast.
    //
    // No IsWithinMeleeRange gate, on purpose. That test is centre to centre
    // with no allowance for two units running, while the castability test and
    // the core's own range check both allow more - so the gate hid the opener
    // for the last few yards of every chase, which is exactly where Sinister
    // Strike (never gated) went off instead. Out of reach the opener is simply
    // not castable yet, and as the fallback decision it becomes the walk in.
    uint32 const stealthOpenerSpellId = SelectRogueStealthOpenerSpellId(player);
    bool const holdStealthForOpener = player->HasStealthAura() && stealthOpenerSpellId != 0;
    AddDecisionCandidate(candidates, holdStealthForOpener && IsSpellReady(player, stealthOpenerSpellId), 49.0f,
        { stealthOpenerSpellId == 703 ? "rogue garrote" : "rogue cheap shot", "stealth opener",
          stealthOpenerSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, nearbyCastingTarget && IsSpellReady(player, 1766), 48.0f,
        { "rogue kick", isCombatRogue ? "interrupt nearby enemy cast" : "interrupt enemy cast", 1766, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyCastingTarget ? nearbyCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, player->HealthBelowPct(40) && IsSpellReady(player, 5277), 47.0f,
        { "rogue evasion", "defensive survival in melee", 5277, playerbot::PvpClassSpellContext::TargetMode::Self });
    // Shadowstep (36554, the Subtlety tier-6 talent) puts the rogue behind its
    // target from up to 25 yards without breaking stealth, so it is both the gap
    // closer and the way into the opener: from stealth it lands the rogue behind
    // the target and the opener, out of reach until then, fires next. It sits
    // just over Sprint so the step goes first and Sprint is kept for when it is
    // down. Only ever the current target - stepping to whoever else is nearest
    // would abandon the kill. A rogue without the talent gets false from
    // IsSpellReady, and a bot's near teleport lands inline (Player::TeleportTo).
    constexpr uint32 kRogueShadowstepSpellId = 36554;
    bool const shadowstepToTarget = IsSpellReady(player, kRogueShadowstepSpellId) &&
        SelectEnemyGapCloserTarget(player, target, 8.0f, 25.0f, false) == target;
    AddDecisionCandidate(candidates, shadowstepToTarget, 46.5f,
        { "rogue shadowstep", holdStealthForOpener ? "step behind the target for the stealth opener" : "close the gap behind the target",
          kRogueShadowstepSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy, target->GetGUID() });
    // Sprint is the T2 wrapper chain 90514 -> 90515 -> 90516; the stock chain
    // 2983 -> 8696 -> 11305 it casts is never held (WrappedSpells.h). Ask for
    // the top rank and let the resolver walk down to the one the rogue owns.
    uint32 const sprintSpellId = 90516;
    AddDecisionCandidate(candidates, (isCombatRogue ? rootedOrSnared : (!player->HealthBelowPct(50) && !player->IsWithinMeleeRange(target) && player->IsWithinDistInMap(target, 30.0f))) && IsSpellReady(player, sprintSpellId), 46.0f,
        { "rogue sprint", "close gap for melee pressure", sprintSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isCombatRogue && rootedOrSnared && !IsSpellReady(player, sprintSpellId) && IsSpellReady(player, 26889), 45.8f,
        { "rogue vanish", "escape root or snare when sprint is unavailable", 26889, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isCombatRogue && player->IsWithinMeleeRange(target) && IsSpellReady(player, 13750), 45.7f,
        { "rogue adrenaline rush", "combat burst when in melee", 13750, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isCombatRogue && player->IsWithinMeleeRange(target) && IsSpellReady(player, 13877), 45.6f,
        { "rogue blade flurry", "cleave burst when in melee", 13877, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isCombatRogue && nearbyMeleeTarget && IsSpellReady(player, 51722), 45.5f,
        { "rogue dismantle", "disarm nearby melee threat", 51722, playerbot::PvpClassSpellContext::TargetMode::Enemy, nearbyMeleeTarget ? nearbyMeleeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, blindTarget, 45.0f,
        { "rogue blind", "prioritize druid/shaman/paladin secondary targets without abolish poison", 2094, playerbot::PvpClassSpellContext::TargetMode::Enemy, blindTarget ? blindTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, (isCombatRogue ? player->GetComboPoints() >= 5 : player->GetComboPoints() >= 5) && IsSpellReady(player, 8643), 44.0f,
        { "rogue kidney shot", "primary stun finisher at full combo points", 8643, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isCombatRogue && player->GetComboPoints() >= 2 && !HasAuraFromSpellChain(player, 6774) && IsSpellReady(player, 6774), 43.5f,
        { "rogue slice and dice", "maintain slice and dice at two combo points", 6774, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isCombatRogue && !player->IsWithinMeleeRange(target) && IsSpellReady(player, 81308), 43.2f,
        { "rogue deadly shot", "ranged fallback when kill target is out of melee", 81308, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Cold Blood guarantees the next finisher crits; use it right before
    // dumping a full combo point bar into Eviscerate.
    AddDecisionCandidate(candidates, isAssassinationRogue && player->GetComboPoints() >= 5 && !HasAuraFromSpellChain(player, 14177) && IsSpellReady(player, 14177), 43.1f,
        { "rogue cold blood", "guarantee a critical finisher at full combo points", 14177, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, player->GetComboPoints() >= 5 && IsSpellReady(player, 11300), 43.0f,
        { "rogue eviscerate", "combo finisher pressure", 11300, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // A low-level rogue builds five points slowly enough that the fight is
    // often over first; on a target that is nearly dead, three will do.
    AddDecisionCandidate(candidates, player->GetComboPoints() >= 3 && player->GetComboPoints() < 5 && target->HealthBelowPct(35) &&
        IsSpellReady(player, 11300), 42.9f,
        { "rogue eviscerate", "finish a low-health target before full combo points", 11300, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Improved Backstab lets Assassination use Backstab as its combo builder
    // instead of Hemorrhage. Backstab is positional: only usable from behind.
    // A rogue whose target is fighting it (and so keeps facing it) must build
    // with Sinister Strike instead of spamming an impossible Backstab.
    bool const behindTarget = target && !target->HasInArc(static_cast<float>(M_PI), player);
    // Plain Backstab (level 4) is the better builder for any dagger rogue
    // that finds itself behind the target; it no longer waits on the talent.
    Item const* rogueMainHand = player->GetWeaponForAttack(BASE_ATTACK, true);
    bool const holdsDagger = rogueMainHand && rogueMainHand->GetTemplate() &&
        rogueMainHand->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER;
    // From the front, walk for the back (ShouldHoldFrontalBuilderForFlank).
    // Only with a dagger in hand - Backstab cannot be cast without one, so
    // there would be no strike at the end of the walk - and never while
    // stealth is being held for the opener.
    bool const flankForBackstab = holdsDagger && !holdStealthForOpener &&
        ShouldHoldFrontalBuilderForFlank(player, target, IsSpellReady(player, 53));
    AddDecisionCandidate(candidates, (hasImprovedBackstab || holdsDagger) &&
        ((behindTarget && player->IsWithinMeleeRange(target)) || flankForBackstab) && IsSpellReady(player, 53), 20.5f,
        { "rogue backstab", flankForBackstab ? "walk behind the target to backstab" :
            (hasImprovedBackstab ? "improved backstab combo point builder" : "backstab from behind with a dagger"),
          53, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Hemorrhage if the rogue actually HAS it, rather than if its spec suggests
    // it should. IsSpellReady resolves the rank chain and returns false for a
    // spell the player never learned, so this asks the only question that
    // matters and needs no talent guess.
    AddDecisionCandidate(candidates, IsSpellReady(player, 16511), 20.2f,
        { "rogue hemorrhage", "subtlety combo point builder when known", 16511, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // And Sinister Strike as the FLOOR, for every rogue of every spec.
    //
    // This is what was missing. The builder used to be chosen by inference -
    // Improved Backstab, else combat gets Sinister Strike and everyone else gets
    // Hemorrhage - and Hemorrhage is a SUBTLETY talent. An Assassination rogue
    // without Improved Backstab therefore matched no builder at all and stood
    // there auto-attacking. Sinister Strike needs no talent and every rogue has
    // it from level one, so nothing can fall through this.
    AddDecisionCandidate(candidates, IsSpellReady(player, 11294), 20.0f,
        { "rogue sinister strike", "universal combo point builder", 11294, playerbot::PvpClassSpellContext::TargetMode::Enemy });

    // While walking for the back, Backstab is the only builder on offer. It
    // cannot land from the front, so it stays the decision, and its not-behind
    // failure in CastDirectSpell is what walks the rogue round. Everything that
    // does not care about facing - Kick, Kidney Shot, Eviscerate, Blind - still
    // goes first.
    if (flankForBackstab)
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
            [](PrioritizedSpellDecision const& candidate)
            {
                return candidate.decision.spellId == 11294 || candidate.decision.spellId == 16511;
            }), candidates.end());

    // While the opener is on offer, nothing else is.
    //
    // Every other strike here breaks stealth, and each one used to win the
    // moment the opener was merely not castable THIS tick: Sinister Strike
    // with energy between its cost and Cheap Shot's (45 against 60), Sinister
    // Strike again while Garrote was refused for facing, Deadly Shot while
    // still closing. That is how Quinfyn walked up to a player and opened with
    // Sinister Strike. Now the rogue opens or keeps closing: the fallback keeps
    // the opener as the decision, which drives the approach and waits out the
    // energy. Sprint and Shadowstep stay - both close the gap without breaking
    // stealth.
    if (holdStealthForOpener)
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
            [stealthOpenerSpellId, sprintSpellId](PrioritizedSpellDecision const& candidate)
            {
                return candidate.decision.spellId != stealthOpenerSpellId && candidate.decision.spellId != sprintSpellId &&
                    candidate.decision.spellId != kRogueShadowstepSpellId;
            }), candidates.end());

        return SelectHighestPriorityCastableDecision(candidates, player, target, nullptr);
    }

    SpellDecision SelectShamanSpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
    {
        SpellDecision decision;
        if (!player)
            return decision;

    bool const hasHostileTarget = HasHostileTarget(player, target);
    bool const inCombat = player->IsInCombat();
    bool const isRestoShaman = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
    bool const isEnhancementShaman = profileSelection.profile == ClassicClassProfile::SecondaryClassic;
    if (!hasHostileTarget && !allyTarget && !isRestoShaman && !isEnhancementShaman)
        return decision;

    bool const partyBenefitsFromWindfury = PartyBenefitsFromWindfuryTotem(player);
    // Counter-totems should only go down when there's an actual reason nearby
    // for them - otherwise shamans burn every totem slot maintaining totems
    // nobody needs.
    bool const manaEnemyNearby = SelectNearbyEnemyManaTarget(player, nullptr, 50.0f, 0.0f) != nullptr;
    bool const fearCasterEnemyNearby = SelectEnemyClassTarget(player, CLASS_WARRIOR, 50.0f) ||
        SelectEnemyClassTarget(player, CLASS_WARLOCK, 50.0f) ||
        SelectEnemyClassTarget(player, CLASS_PRIEST, 50.0f);
    bool const poisonEnemyNearby = SelectEnemyClassTarget(player, CLASS_ROGUE, 50.0f) ||
        SelectEnemyClassTarget(player, CLASS_HUNTER, 50.0f);

    bool const dispelThrottleActive = playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotDispelCooldownToken);
    // Lesser Healing Wave is trained at 20 and is its own rank chain; below
    // that Healing Wave (level 1) is the shaman's only heal, and restoration
    // had nothing at all before it (Chain Heal is 40).
    uint32 const quickHealSpellId = ResolveKnownPlayerSpellInChain(player, 10468) ? 10468 : 25357;
    char const* const quickHealName = quickHealSpellId == 10468 ? "shaman lesser healing wave" : "shaman healing wave";
    // 89745 lets an enhancement shaman weave in a low-priority heal.
    Unit const* enhLowHealTarget = (isEnhancementShaman && player->HasAura(89745) && IsSpellReady(player, quickHealSpellId)) ? SelectFriendlyHealthTarget(player, 40.0f, 50.0f) : nullptr;
    // No throttle test: enhancement purges on every global it can. The dead
    // cast that the throttle protects everyone else from cannot happen here
    // anyway - SelectEnemyDispelTarget only answers with a target that is
    // actually carrying a dispellable magic aura.
    Unit const* enhPurgeTarget = (isEnhancementShaman && hasHostileTarget && IsSpellReady(player, 81325)) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, 30.0f) : nullptr;
    Unit const* chainHealTarget = isRestoShaman && IsSpellReady(player, 10623) ? SelectFriendlyHealthTarget(player, 40.0f, 95.0f) : nullptr;
    Unit const* lesserHealTarget = isRestoShaman && IsSpellReady(player, quickHealSpellId) ? SelectFriendlyHealthTarget(player, 40.0f, 90.0f) : nullptr;
    Unit const* nsHealTarget = isRestoShaman && IsSpellReady(player, 16188) && IsSpellReady(player, 25357) ? SelectFriendlyHealthTarget(player, 40.0f, 35.0f) : nullptr;
    Unit const* earthShieldTarget = isRestoShaman && IsSpellReady(player, 32593) && !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, 32593) ? SelectFriendlyHealthTarget(player, 40.0f, 100.0f) : nullptr;
    Unit const* purgeTarget = isRestoShaman && hasHostileTarget && IsSpellReady(player, 81325) ? SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, 30.0f) : nullptr;
    Unit const* allyMagicTarget = isRestoShaman && IsSpellReady(player, 81325) ? SelectFriendlyDispelTarget(player, DISPEL_MAGIC, 40.0f) : nullptr;
    // Rehgar's Fury (82419) is a charge that requires Ghost Wolf form active
    // (see spell_sha_ghost_wolf_charge / Unit::CompleteGhostWolfCharge) - it
    // consumes the form on impact. Enhancement shifts into Ghost Wolf first,
    // then charges the kill target with Rehgar's Fury to close the gap.
    bool const enhNeedsGapClose = isEnhancementShaman && hasHostileTarget && target && !player->IsWithinMeleeRange(target);
    bool const shamanInGhostWolf = HasAuraFromSpellChain(player, 2645);
    bool const enhInGhostWolf = isEnhancementShaman && shamanInGhostWolf;
    SpellInfo const* rehgarsFuryInfo = sSpellMgr->GetSpellInfo(82419);
    bool const canRehgarsFuryTarget = isEnhancementShaman && hasHostileTarget && target && rehgarsFuryInfo &&
        player->IsValidAttackTarget(target, rehgarsFuryInfo) && player->IsWithinLOSInMap(target) &&
        (rehgarsFuryInfo->GetMaxRange(false) <= 0.0f || player->IsWithinDistInMap(target, rehgarsFuryInfo->GetMaxRange(false))) &&
        (rehgarsFuryInfo->GetMinRange(false) <= 0.0f || !player->IsWithinDistInMap(target, rehgarsFuryInfo->GetMinRange(false)));
    bool const canUseRehgarsFury = canRehgarsFuryTarget && IsSpellReady(player, 82419);

    if (shamanInGhostWolf)
    {
        if (canUseRehgarsFury)
            return { "shaman rehgar's fury", "only allowed action while in ghost wolf form", 82419, playerbot::PvpClassSpellContext::TargetMode::Enemy };

        // Do not cancel Ghost Wolf just because Rehgar's Fury isn't castable
        // on this exact tick (its own cooldown, a brief LOS blip, target
        // sitting inside the charge's min-range band, etc.) while the shaman
        // still has a genuine reason to be closing distance - that reads as
        // flickering in and out of the form instead of committing to the
        // chase. Only unshift once the gap-close reason itself is gone
        // (target dead/invalid or already in melee range). Rehgar's Fury
        // landing still removes the form on its own via
        // Unit::CompleteGhostWolfCharge.
        if (enhNeedsGapClose)
            return decision;

        const_cast<Player*>(player)->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        return decision;
    }

    Unit const* earthShockCastingTarget = IsSpellReady(player, 10414) ?
        SelectEnemyCastingTarget(player, 20.0f, target) : nullptr;

    std::vector<PrioritizedSpellDecision> candidates;
    // Disabled: auto-casting Windfury Weapon from PvP loop while investigating weapon-dependent aura crashes.
    AddDecisionCandidate(candidates, !player->IsInCombat() && !HasElementalShield(player) && IsSpellReady(player, 10432), 34.0f,
        { "shaman lightning shield", "maintain shield buff out of combat", 10432, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, earthShockCastingTarget && IsSpellReady(player, 10414), 60.0f,
        { "shaman earth shock", "interrupt enemy cast with shock", 10414, playerbot::PvpClassSpellContext::TargetMode::Enemy, earthShockCastingTarget ? earthShockCastingTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, inCombat && !isRestoShaman && !isEnhancementShaman && !partyBenefitsFromWindfury && hasHostileTarget && manaEnemyNearby && !HasActiveAirTotem(player) && IsSpellReady(player, 81478), 59.0f,
        { "shaman grounding totem", "counter incoming caster pressure from a mana user within 50 yards", 81478, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, !isRestoShaman && IsSpellReady(player, 16166), 58.0f,
        { "shaman elemental mastery", "trigger burst throughput cooldown", 16166, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && nsHealTarget && !player->HasAura(16188), 59.5f,
        { "shaman nature's swiftness", "prepare instant emergency healing wave", 16188, playerbot::PvpClassSpellContext::TargetMode::Self, nsHealTarget ? nsHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRestoShaman && player->HasAura(16188) && nsHealTarget, 59.4f,
        { "shaman healing wave", "consume nature's swiftness on emergency target", 25357, nsHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, nsHealTarget ? nsHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRestoShaman && chainHealTarget, 58.5f,
        { "shaman chain heal", "primary restoration heal", 10623, chainHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, chainHealTarget ? chainHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRestoShaman && earthShieldTarget && !HasAuraFromSpellChain(earthShieldTarget, 32593), 58.0f,
        { "shaman earth shield", "protect lowest health ally", 32593, earthShieldTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, earthShieldTarget ? earthShieldTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, !isRestoShaman && !isEnhancementShaman && IsSpellReady(player, 10605), 57.0f,
        { "shaman chain lightning", "primary burst cast on kill target", 10605, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, isEnhancementShaman && hasHostileTarget && target && IsSpellReady(player, 17364), 56.5f,
        { "shaman stormstrike", "primary melee burst on the kill target", 17364, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Enhancement leans on purge harder than the default cross-spec candidate below.
    AddDecisionCandidate(candidates, enhPurgeTarget, 54.5f,
        { "shaman purge", "heavier purge priority for enhancement", 81325, playerbot::PvpClassSpellContext::TargetMode::Enemy, enhPurgeTarget ? enhPurgeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, enhLowHealTarget, 30.5f,
        { quickHealName, "weave a heal on a low-health ally", quickHealSpellId, enhLowHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, enhLowHealTarget ? enhLowHealTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, inCombat && hasHostileTarget && IsMeleeClass(target) && player->IsWithinDistInMap(target, 10.0f) && !HasActiveEarthTotem(player) && IsSpellReady(player, 2484), 56.0f,
        { "shaman earthbind totem", "kite nearby melee pressure", 2484, playerbot::PvpClassSpellContext::TargetMode::Self });
    // Flame Shock, which was missing from this table entirely - and it is the
    // shaman's ONLY spell that is not Nature or physical.
    //
    // That absence was invisible until the immunity rule arrived. Every other
    // offensive candidate here is Nature (Earth Shock, Chain Lightning, Purge,
    // Lightning Bolt) and Earth Shock only ever fires as an interrupt, so an
    // elemental or restoration shaman had nothing whatsoever to say to a
    // nature-immune target - and this realm has 70 creature templates that are,
    // 88 of them spawned in the Badlands alone. Refusing the Nature casts without
    // this would have left that shaman walking in to melee for the whole fight.
    //
    // Applied as a DoT rather than spammed: the candidate withdraws once OUR
    // Flame Shock is ticking on the target, which is also how it is meant to be
    // played. Ranked below Chain Lightning so it does not displace the primary
    // nuke, and above Frost Shock because the three shocks share a cooldown and
    // this is the better use of it.
    AddDecisionCandidate(candidates, !isEnhancementShaman && hasHostileTarget && target &&
        IsSpellReady(player, 8050) && !HasAuraFromSpellChain(target, 8050, player->GetGUID()),
        isRestoShaman ? 5.5f : 55.5f,
        { "shaman flame shock", "keep the fire damage-over-time on the kill target",
            8050, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Frost Shock could never fire against a creature: IsMeleeClass resolves the
    // unit to a Player and answers false for everything else, so both arms of the
    // old condition were PvP-only. The snare reasoning still holds against players;
    // against a mob this is simply the shaman's other off-school shock, and worth
    // having for the same reason Flame Shock is.
    AddDecisionCandidate(candidates, hasHostileTarget && IsSpellReady(player, 10473) &&
        ((isEnhancementShaman && enhNeedsGapClose) || (IsMeleeClass(target) && player->IsWithinDistInMap(target, 20.0f)) ||
            (target && target->GetTypeId() != TYPEID_PLAYER && player->IsWithinDistInMap(target, 20.0f))), 55.0f,
        { "shaman frost shock", isEnhancementShaman && enhNeedsGapClose ? "snare the kill target while chasing" : "snare medium-range melee threats",
            10473, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    Unit const* poisonedAllyInTotemRange = inCombat && IsSpellReady(player, 8170) ? SelectFriendlyDispelTarget(player, DISPEL_POISON, 20.0f) : nullptr;
    AddDecisionCandidate(candidates, inCombat && poisonedAllyInTotemRange && poisonEnemyNearby && !HasActiveWaterTotem(player), 54.0f,
        { "shaman poison cleansing totem", "answer rogue/hunter poison pressure with a nearby water totem", 8170, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && fearCasterEnemyNearby && !HasActiveEarthTotem(player) && IsSpellReady(player, 81476), 53.0f,
        { "shaman tremor totem", "mitigate fear pressure from a warrior/warlock/priest within 50 yards", 81476, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && isRestoShaman && player->GetPowerPct(POWER_MANA) < 50.0f && !HasActiveWaterTotem(player) && IsSpellReady(player, 16190), 52.8f,
        { "shaman mana tide totem", "restore mana below half", 16190, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && fearCasterEnemyNearby && !HasActiveEarthTotem(player) && IsSpellReady(player, 81476), 52.7f,
        { "shaman tremor totem", "maintain a nearby tremor totem while a warrior/warlock/priest is within 50 yards", 81476, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && poisonEnemyNearby && !HasActiveWaterTotem(player) && IsSpellReady(player, 81477), 52.6f,
        { "shaman poison cleansing totem", "maintain a nearby poison cleansing totem while a rogue/hunter is within 50 yards", 81477, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && !partyBenefitsFromWindfury && manaEnemyNearby && !HasActiveAirTotem(player) && IsSpellReady(player, 81478), 52.5f,
        { "shaman grounding totem", "maintain a nearby grounding totem while a mana user is within 50 yards", 81478, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, inCombat && partyBenefitsFromWindfury && !HasActiveAirTotem(player) && IsSpellReady(player, 10614), 52.5f,
        { "shaman windfury totem", "support an arms/fury warrior or retribution paladin", 10614, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && SelectNearbyMeleeTarget(player, target, 8.0f) && player->HealthBelowPct(50) && IsSpellReady(player, 2645), 52.4f,
        { "shaman ghost wolf", "escape melee pressure while endangered", 2645, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, enhNeedsGapClose && !enhInGhostWolf && canUseRehgarsFury && IsSpellReady(player, 2645), 59.6f,
        { "shaman ghost wolf", "shift to close the gap with rehgar's fury", 2645, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, enhNeedsGapClose && enhInGhostWolf && canUseRehgarsFury, 59.5f,
        { "shaman rehgar's fury", "charge the kill target while in ghost wolf form", 82419, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    // Enhancement only gets Lesser Healing Wave weaving from the Maelstrom
    // Weapon-style talent (89745, "blurry") that makes it a fast/free-ish
    // proc-consumer - without that aura up, this was letting an Enhancement
    // shaman self-heal on the normal, full-cost cast whenever health dropped
    // below 50 percent regardless of whether the proc was actually active.
    AddDecisionCandidate(candidates, (!isEnhancementShaman || player->HasAura(89745)) && player->HealthBelowPct(50) && IsSpellReady(player, quickHealSpellId), 52.0f,
        { quickHealName, "self-sustain while focused", quickHealSpellId, playerbot::PvpClassSpellContext::TargetMode::Self });
    AddDecisionCandidate(candidates, isRestoShaman && purgeTarget, 53.5f,
        { "shaman purge", "purge enemy magic buffs", 81325, playerbot::PvpClassSpellContext::TargetMode::Enemy, purgeTarget ? purgeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRestoShaman && allyMagicTarget, 53.2f,
        { "shaman purge ally", "purge sheep or fear magic effects from allies", 81325, allyMagicTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, allyMagicTarget ? allyMagicTarget->GetGUID() : ObjectGuid::Empty });
    // Only at an enemy actually carrying something to strip. This used to fire
    // at the kill target with no check at all, so an enhancement shaman (which
    // skips the dispel throttle) could spend every global purging nothing.
    Unit const* defaultPurgeTarget = (isEnhancementShaman || !dispelThrottleActive) && hasHostileTarget && IsSpellReady(player, 81325) ?
        (enhPurgeTarget ? enhPurgeTarget : SelectEnemyDispelTarget(player, DISPEL_MAGIC, target, 30.0f)) : nullptr;
    AddDecisionCandidate(candidates, defaultPurgeTarget, 40.0f,
        { "shaman purge", "strip enemy magical effects by default", 81325, playerbot::PvpClassSpellContext::TargetMode::Enemy, defaultPurgeTarget ? defaultPurgeTarget->GetGUID() : ObjectGuid::Empty });
    AddDecisionCandidate(candidates, isRestoShaman && lesserHealTarget, 51.0f,
        { quickHealName, "restoration fallback heal", quickHealSpellId, lesserHealTarget == player ? playerbot::PvpClassSpellContext::TargetMode::Self : playerbot::PvpClassSpellContext::TargetMode::Ally, lesserHealTarget ? lesserHealTarget->GetGUID() : ObjectGuid::Empty });
    // Before Stormstrike (a level-40 talent) enhancement has no attack spell
    // of its own; Earth Shock (4) is otherwise only used as an interrupt.
    AddDecisionCandidate(candidates, isEnhancementShaman && hasHostileTarget && target && player->IsWithinMeleeRange(target) &&
        !ResolveKnownPlayerSpellInChain(player, 17364) && IsSpellReady(player, 10414), 39.5f,
        { "shaman earth shock", "melee-range shock before stormstrike is trained", 10414, playerbot::PvpClassSpellContext::TargetMode::Enemy });
    AddDecisionCandidate(candidates, !isEnhancementShaman && hasHostileTarget && IsSpellReady(player, 15208), isRestoShaman ? 5.0f : 39.0f,
        { "shaman lightning bolt", "fallback ranged damage cast", 15208, playerbot::PvpClassSpellContext::TargetMode::Enemy });

        return SelectHighestPriorityCastableDecision(candidates, player, target, allyTarget);
    }

SpellDecision SelectClassicClassSpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
{
    SpellDecision decision;
    if (!player)
    {
        decision.reason = "missing-player";
        return decision;
    }

    if (profileSelection.unsupportedClass)
    {
        decision.reason = "unsupported-class";
        return decision;
    }

        bool const inMelee = target && player->IsWithinMeleeRange(target);
        switch (player->GetClass())
        {
        case CLASS_HUNTER:
            return SelectHunterSpell(player, target, inMelee, profileSelection);
        case CLASS_MAGE:
        {
            SpellDecision mageDecision = SelectMageSpell(player, target, inMelee, profileSelection);
            if (!mageDecision.spellId && HasHostileTarget(player, target) && IsLowOrOutOfManaForFallback(player) && IsWandShootReadyForDecision(player))
                return { "mage shoot wand", "fallback to wand pressure while low on mana", kWandShootSpellId, playerbot::PvpClassSpellContext::TargetMode::Enemy };
            return mageDecision;
        }
        case CLASS_PRIEST:
            return SelectPriestSpell(player, target, allyTarget, profileSelection);
        case CLASS_PALADIN:
            return SelectPaladinSpell(player, target, profileSelection);
        case CLASS_WARLOCK:
            return SelectWarlockSpell(player, target, profileSelection);
        case CLASS_DRUID:
            return SelectDruidSpell(player, target, profileSelection);
        case CLASS_WARRIOR:
            return SelectWarriorSpell(player, target, profileSelection);
        case CLASS_ROGUE:
            return SelectRogueSpell(player, target, profileSelection);
        case CLASS_SHAMAN:
            return SelectShamanSpell(player, target, allyTarget, profileSelection);
        default:
            decision.reason = "class-not-in-this-pass";
            return decision;
        }
    }

SpellDecision SelectClassOrUtilitySpell(Player const* player, Unit const* target, Unit const* allyTarget, ClassicProfileSelection const& profileSelection)
{
    if (playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotShadowmeldGraceToken))
    {
        SpellDecision holdDecision;
        holdDecision.reason = "shadowmeld grace window";
        return holdDecision;
    }

    if (player && player->IsInCombat() && player->HealthBelowPct(50))
    {
        if (uint32 const healthstoneItemEntry = SelectReadyHealthstoneItemEntry(player))
        {
            if (player->GetClass() == CLASS_WARRIOR)
                SetWarriorGapCloserDiagnostic(player, "early_exit=healthstone");
            return { "use healthstone", "restore health below fifty percent", 0,
                playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), healthstoneItemEntry };
        }

        // After the healthstone, because that one is free and a potion is not.
        //
        // Nothing on this realm ever drank one. The only item entries reachable
        // from the decision table were warlock healthstones and stones, so a bot
        // carrying five kinds of healing potion died holding all of them - which
        // is also why they pile up until the bags are full and it stops mining.
        if (uint32 const potionItemEntry = SelectReadyRestorePotionItemEntry(player, false))
        {
            if (player->GetClass() == CLASS_WARRIOR)
                SetWarriorGapCloserDiagnostic(player, "early_exit=healing_potion");
            return { "use healing potion", "restore health below fifty percent", 0,
                playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), potionItemEntry };
        }
    }

    // Mana second, and lower, because being out of mana is a losing fight
    // whereas being out of health is a lost one. Only for a class that has a
    // mana bar to empty.
    //
    // IN COMBAT ONLY, like the health arm above it. A potion is a combat
    // resource on a two minute cooldown; a bot standing around at twenty
    // percent mana should sit down and drink, which the rest of the engine
    // already handles, not burn a potion it will want in the next fight. It
    // also keeps the bag walk below out of the idle path entirely - this runs on
    // the decision tick for every bot, and a hunter holding its kite at low mana
    // would otherwise pay for a full inventory scan every tick of every fight it
    // is winning.
    if (player && player->IsInCombat() && player->GetMaxPower(POWER_MANA) > 0 &&
        player->GetPowerPct(POWER_MANA) < 25.0f)
        if (uint32 const manaItemEntry = SelectReadyRestorePotionItemEntry(player, true))
            return { "use mana potion", "restore mana below twenty-five percent", 0,
                playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID(), manaItemEntry };

    // Spirit of Redemption must run the priest healing selector even with no
    // selected hostile/ally target; the selector finds the lowest-health ally
    // itself and Flash Heal is free during this aura.
    if (IsPriestInSpiritOfRedemption(player))
        return SelectClassicClassSpell(player, target, allyTarget, profileSelection);

    // School-lockout fallbacks are combat recovery, not ordinary utility.
    // Run them before racial/mount/eat-drink selection so a shadow-locked
    // shadow priest drops Shadowform and begins healing immediately, and a
    // holy-locked holy priest can answer with Mind Blast without losing a
    // decision tick to unrelated utility.
    if (player && player->GetClass() == CLASS_PRIEST)
    {
        bool const holyPriestHolyLocked = profileSelection.profile == ClassicClassProfile::SecondaryClassic &&
            player->GetSpellHistory()->IsSchoolLocked(SPELL_SCHOOL_MASK_HOLY);
        bool const isShadowPriest = profileSelection.profile == ClassicClassProfile::TertiaryClassic;
        bool const shadowPriestShadowLocked = isShadowPriest &&
            player->GetSpellHistory()->IsSchoolLocked(SPELL_SCHOOL_MASK_SHADOW);
        bool const shadowPriestNeedsForm = isShadowPriest && !shadowPriestShadowLocked &&
            !HasAuraFromSpellChain(player, 15473) && IsSpellReady(player, 15473);
        if (holyPriestHolyLocked || shadowPriestShadowLocked || shadowPriestNeedsForm)
            return SelectClassicClassSpell(player, target, allyTarget, profileSelection);
    }

    // Hunter cast-time actions (especially Aimed Shot and Revive Pet) must be
    // exclusive. Lifecycle movement already holds the hunter still, but the
    // selector also has to stop returning other instant actions like Rapid Fire
    // while the accepted cast is preparing/channeling. Otherwise the next AI
    // tick can attempt another action during Aimed Shot and clip/interrupt it on
    // some branches.
    if (IsHunterCastTimeActionLocked(player))
    {
        SpellDecision holdDecision;
        holdDecision.reason = "hunter cast-time spell in progress";
        return holdDecision;
    }

    // Racials take priority over the class rotation, but only if the pick can
    // actually be cast this instant. SelectRacialSpell filters on IsSpellReady
    // alone, and racials bypass AddDecisionCandidate, so the caller's retry loop
    // cannot suppress a bad pick - without this gate the same uncastable racial
    // is re-chosen every attempt and class selection never runs at all. The
    // playerbot pvp core merge dropped the check; see the comment on the gnome
    // branch of SelectRacialSpell, which still refers to it.
    //
    // LOS is not re-tested here: IsDecisionImmediatelyCastable already calls
    // IsWithinLOSInMap, and the merge-base's separate DecisionTargetIsInLineOfSight
    // helper no longer exists.
    SpellDecision const racialDecision = SelectRacialSpell(player, target, allyTarget);
    if (racialDecision.spellId && IsDecisionImmediatelyCastable(player, racialDecision, target, allyTarget))
        return racialDecision;

    if (SpellDecision const utilityDecision = MaybeSelectUtilitySpell(player, target); utilityDecision.spellId)
        return utilityDecision;

    if (!HasHostileTarget(player, target) && !allyTarget)
    {
        if (SpellDecision const racialDecision = SelectRacialSpell(player, target, allyTarget); racialDecision.spellId)
            return racialDecision;

        return {};
    }

    return SelectClassicClassSpell(player, target, allyTarget, profileSelection);
}

    char const* GetClassLabel(uint8 classId)
    {
        switch (classId)
        {
        case CLASS_WARRIOR: return "Warrior";
        case CLASS_PALADIN: return "Paladin";
        case CLASS_HUNTER: return "Hunter";
        case CLASS_ROGUE: return "Rogue";
        case CLASS_PRIEST: return "Priest";
        case CLASS_SHAMAN: return "Shaman";
        case CLASS_MAGE: return "Mage";
        case CLASS_WARLOCK: return "Warlock";
        case CLASS_DRUID: return "Druid";
        case CLASS_DEATH_KNIGHT: return "DeathKnight";
        default: return "UnknownClass";
        }
    }

    bool IsPrimaryRangedClassForSpacing(uint8 classId)
    {
        switch (classId)
        {
        case CLASS_HUNTER:
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_WARLOCK:
        case CLASS_SHAMAN:
            return true;
        default:
            return false;
        }
    }

    bool IsDruidMeleeForm(Player const* player)
    {
        if (!player || player->GetClass() != CLASS_DRUID)
            return false;

        switch (player->GetShapeshiftForm())
        {
        case FORM_CAT:
        case FORM_BEAR:
        case FORM_DIREBEAR:
            return true;
        default:
            return false;
        }
    }

    bool UsesRangedSpacingProfile(Player const* player, ClassicProfileSelection const& profileSelection)
    {
        if (!player)
            return false;

        switch (player->GetClass())
        {
        case CLASS_DRUID:
            if (IsDruidMeleeForm(player))
                return false;
            return profileSelection.profile != ClassicClassProfile::TertiaryClassic;
        case CLASS_PALADIN:
            return profileSelection.profile == ClassicClassProfile::PrimaryClassic;
        case CLASS_SHAMAN:
            return profileSelection.profile == ClassicClassProfile::PrimaryClassic ||
                profileSelection.profile == ClassicClassProfile::TertiaryClassic;
        default:
            break;
        }

        return IsPrimaryRangedClassForSpacing(player->GetClass());
    }

    bool CanUseHealRangeSpacing(uint8 classId)
    {
        switch (classId)
        {
        case CLASS_PRIEST:
        case CLASS_PALADIN:
        case CLASS_DRUID:
        case CLASS_SHAMAN:
            return true;
        default:
            return false;
        }
    }

    float ComputeApproachFollowRange(float nominalRange)
    {
        // Keep an extra movement buffer so ranged bots do not settle in a
        // dead-zone where spell-selection still reports out-of-range but chase
        // motion does not re-engage because the desired distance is too close to
        // edge tolerances.
        return std::max(1.0f, nominalRange - 3.0f);
    }

    bool IsPrimaryMeleeClassForSpacing(uint8 classId)
    {
        switch (classId)
        {
        case CLASS_WARRIOR:
        case CLASS_ROGUE:
        case CLASS_PALADIN:
        case CLASS_DRUID:
        case CLASS_DEATH_KNIGHT:
            return true;
        default:
            return false;
        }
    }

    bool UsesMeleeSpacingProfile(Player const* player, ClassicProfileSelection const& profileSelection)
    {
        if (!player)
            return false;

        switch (player->GetClass())
        {
        case CLASS_DRUID:
            if (IsDruidMeleeForm(player))
                return true;
            return profileSelection.profile == ClassicClassProfile::TertiaryClassic;
        case CLASS_PALADIN:
            return profileSelection.profile != ClassicClassProfile::PrimaryClassic;
        case CLASS_SHAMAN:
            return profileSelection.profile == ClassicClassProfile::SecondaryClassic;
        default:
            break;
        }

        return IsPrimaryMeleeClassForSpacing(player->GetClass());
    }

    // Flag running (Warsong Gulch, Twin Peaks).
    //
    // What a player does with their hands while carrying a flag, or while the
    // designated runner is on its way to one. Tactical movement owns the route;
    // these are the casts made along it. Each class returns its first usable
    // move, in priority order.
    //
    // Castability is decided here and not by IsDecisionImmediatelyCastable,
    // which refuses every druid form change ("not while shapeshifted"). The
    // executor cancels the current form and casts the new one, which is how a
    // real swap between forms works - and how a powershift out of a root works,
    // since entering a form strips roots and snares (RemoveAurasByShapeShift).
    struct FlagRunThreats
    {
        Unit const* nearestEnemy = nullptr;
        float nearestEnemyDistance = std::numeric_limits<float>::max();
        Unit const* nearestMeleeEnemy = nullptr;
        float nearestMeleeDistance = std::numeric_limits<float>::max();
        uint32 enemiesWithin10 = 0;
    };

    bool IsMeleeChaser(Player const* enemy)
    {
        if (IsMeleeClass(enemy))
            return true;

        if (enemy->GetClass() != CLASS_DRUID)
            return false;

        ShapeshiftForm const form = enemy->GetShapeshiftForm();
        return form == FORM_CAT || form == FORM_BEAR || form == FORM_DIREBEAR;
    }

    FlagRunThreats ScanFlagRunThreats(Player const* player)
    {
        constexpr float kThreatScanRange = 45.0f;

        FlagRunThreats threats;
        Map const* map = player->FindMap();
        if (!map)
            return threats;

        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player const* enemy = itr->GetSource();
            if (!enemy || enemy == player || !player->IsWithinDistInMap(enemy, kThreatScanRange) || !HasHostileTarget(player, enemy))
                continue;

            float const distance = player->GetDistance(enemy);
            if (distance <= 10.0f)
                ++threats.enemiesWithin10;

            if (distance < threats.nearestEnemyDistance)
            {
                threats.nearestEnemy = enemy;
                threats.nearestEnemyDistance = distance;
            }

            if (distance < threats.nearestMeleeDistance && IsMeleeChaser(enemy))
            {
                threats.nearestMeleeEnemy = enemy;
                threats.nearestMeleeDistance = distance;
            }
        }

        return threats;
    }

    bool IsFlagRunSpellCastable(Player const* player, uint32 spellId, Unit const* target, bool formChangeAllowed)
    {
        uint32 const knownSpellId = ResolveKnownPlayerSpellInChain(player, spellId);
        SpellInfo const* spellInfo = knownSpellId ? sSpellMgr->GetSpellInfo(knownSpellId) : nullptr;
        if (!spellInfo)
            return false;

        SpellHistory const* history = player->GetSpellHistory();
        if (history->HasCooldown(knownSpellId) || history->HasGlobalCooldown(spellInfo) ||
            player->IsNonMeleeSpellCast(false, false, true))
            return false;

        // It has to go off at a run.
        if (spellInfo->CalcCastTime() > 0 || spellInfo->IsChanneled())
            return false;

        if (playerbot::PvpCore::IsBattlegroundFlagCarrier(player) && playerbot::PvpCore::SpellWouldBreakFlagCarry(knownSpellId))
            return false;

        // Silence, pacify and a school lockout refuse the cast without starting
        // the global cooldown, so an unusable pick here would be picked again
        // every tick for the whole effect - and hold back the healthstone.
        if ((spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE && player->HasUnitFlag(UNIT_FLAG_SILENCED)) ||
            (spellInfo->PreventionType == SPELL_PREVENTION_TYPE_PACIFY && player->HasUnitFlag(UNIT_FLAG_PACIFIED)) ||
            history->IsSchoolLocked(spellInfo->GetSchoolMask()))
            return false;

        if (spellInfo->EquippedItemClass >= 0 && !player->HasItemFitToSpellRequirements(spellInfo))
            return false;

        SpellCastResult const formResult = spellInfo->CheckShapeshift(player->GetShapeshiftForm());
        if (formResult != SPELL_CAST_OK && !(formChangeAllowed && formResult == SPELL_FAILED_NOT_SHAPESHIFT))
            return false;

        if (!MeetsCasterAuraStateRequirements(player, knownSpellId))
            return false;

        if (spellInfo->HasAttribute(SPELL_ATTR0_OUTDOORS_ONLY) && !player->IsOutdoors())
            return false;

        if (spellInfo->PowerType >= 0 && spellInfo->PowerType < MAX_POWERS)
        {
            int32 const cost = spellInfo->CalcPowerCost(player, spellInfo->GetSchoolMask());
            if (cost > 0 && player->GetPower(Powers(spellInfo->PowerType)) < uint32(cost))
                return false;
        }

        if (!target || target == player)
            return true;

        if (!target->IsAlive() || !player->IsValidAttackTarget(target, spellInfo))
            return false;

        float const maxRange = spellInfo->GetMaxRange(false);
        float const minRange = spellInfo->GetMinRange(false);
        if ((maxRange > 0.0f && !player->IsWithinDistInMap(target, maxRange)) ||
            (minRange > 0.0f && player->IsWithinDistInMap(target, minRange)))
            return false;

        return player->IsWithinLOSInMap(target);
    }

    // Blink leaps twenty yards straight ahead; the spell works the landing
    // out from the facing (TARGET_DEST_CASTER_FRONT_LEAP). It is only worth it
    // on a straight stretch the bot is already running down, facing along it,
    // with room to land.
    bool IsBlinkDownRouteClear(Player const* player)
    {
        constexpr float kBlinkDistance = 20.0f;
        constexpr float kMinimumStraightLeg = 18.0f;
        constexpr float kMaximumFacingDeviation = 0.35f;

        Movement::MoveSpline const* spline = player->movespline;
        if (!spline || !spline->Initialized() || spline->Finalized())
            return false;

        G3D::Vector3 const next = spline->CurrentDestination();
        float const dx = next.x - player->GetPositionX();
        float const dy = next.y - player->GetPositionY();
        if (dx * dx + dy * dy < kMinimumStraightLeg * kMinimumStraightLeg)
            return false;

        float const deviation = std::abs(Position::NormalizeOrientation(
            std::atan2(dy, dx) - player->GetOrientation() + float(M_PI)) - float(M_PI));
        if (deviation > kMaximumFacingDeviation)
            return false;

        Position const landing = const_cast<Player*>(player)->GetFirstCollisionPosition(kBlinkDistance, 0.0f);
        return player->GetExactDist2d(landing) >= 12.0f &&
            std::abs(landing.GetPositionZ() - player->GetPositionZ()) <= 6.0f;
    }

    // A slow or root that entering a form actually strips: exactly the test
    // Unit::RemoveAurasByShapeShift applies. A daze or any crowd-control-flagged
    // slow survives the shift, and powershifting at one only burns mana.
    bool HasShapeshiftRemovableMovementImpairment(Unit const* unit)
    {
        constexpr uint32 kRemovableMechanicMask = (1 << MECHANIC_SNARE) | (1 << MECHANIC_ROOT);
        for (auto const& appliedAura : unit->GetAppliedAuras())
        {
            Aura const* aura = appliedAura.second ? appliedAura.second->GetBase() : nullptr;
            SpellInfo const* spellInfo = aura ? aura->GetSpellInfo() : nullptr;
            if (spellInfo && (spellInfo->GetAllEffectsMechanicMask() & kRemovableMechanicMask) &&
                !spellInfo->HasAttribute(SPELL_ATTR0_CU_AURA_CC))
                return true;
        }

        return false;
    }

    // A flag run is expensive to a shapeshifter: every root and snare on the
    // route buys a powershift, and every powershift buys the shift back, so a
    // druid was arriving at the flag with an empty bar and nothing left to run
    // it home with. On a clear route it now does what a player does - sits
    // down, drinks it back, and carries on.
    //
    // The clear radius is deliberately far wider than the 35 yard combat-posture
    // boundary the ordinary recovery selector uses. A runner caught sitting is a
    // lost flag, so the rule is not "nobody is on me" but "nobody is anywhere
    // near the route".
    // Starting is this rule's own decision; ending is deliberately the same
    // "nothing left to recover, or something hit me" the ordinary recovery
    // selector uses. Two selectors that disagree about when a drink should end
    // would take the aura off each other's hands on alternate ticks and stutter
    // the bot, so while a flag-route bot is recovering, this branch owns it end
    // to end and the ordinary selector never sees it.
    constexpr float kFlagRunRecoveryClearRadius = 100.0f;
    constexpr float kFlagRunRecoveryStartPct = 50.0f;

    bool IsRecoveringOnFlagRun(Player const* player)
    {
        return player && (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT) ||
            player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK));
    }

    bool WantsFlagRunRecovery(Player const* player)
    {
        if (!player || !player->IsAlive() || player->IsInCombat() || IsHardControlled(player))
            return false;

        // A real player cannot eat or drink while swimming, and would not sit
        // down in lava to do it either.
        if (player->IsInWater() || IsInHazardousLiquidForRecovery(player))
            return false;

        float const manaPct = player->GetMaxPower(POWER_MANA) > 0 ? player->GetPowerPct(POWER_MANA) : 100.0f;
        if (manaPct >= kFlagRunRecoveryStartPct && player->GetHealthPct() >= kFlagRunRecoveryStartPct)
            return false;

        return !HasNearbyAttackableEnemyPlayer(player, kFlagRunRecoveryClearRadius);
    }

    bool CanContinueFlagRunRecovery(Player const* player)
    {
        if (!player || !player->IsAlive() || player->IsInCombat())
            return false;

        bool const needsDrink = player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 100.0f;
        return needsDrink || player->GetHealthPct() < 100.0f;
    }

    void ClearFlagRunRecovery(Player const* player)
    {
        if (!player)
            return;

        Player* mutablePlayer = const_cast<Player*>(player);
        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            mutablePlayer->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
        if (player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
            mutablePlayer->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);
    }

    SpellDecision SelectFlagRunRecoverySpell(Player const* player)
    {
        using TargetMode = playerbot::PvpClassSpellContext::TargetMode;

        bool const needsFood = player->GetHealthPct() < 100.0f;
        bool const needsDrink = player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 100.0f;

        // Food first, then drink beside it, mirroring the ordinary recovery
        // selector: both auras run at once so the one pause covers both bars.
        if (needsFood && !player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT) &&
            IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT))
            return { "eat", "flag route is clear enough to eat", SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT, TargetMode::Self, player->GetGUID() };

        if (needsDrink && !player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK) &&
            IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK))
            return { "drink", "flag route is clear enough to drink back the shift cost", SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK, TargetMode::Self, player->GetGUID() };

        return {};
    }

    // Travel Form, Aquatic Form, or a carrier's Cat Form, while on a flag run.
    bool IsDruidInFlagRunForm(Player const* player, playerbot::PvpValues const& values)
    {
        if (!player || player->GetClass() != CLASS_DRUID || values.ctfCarrierHolding)
            return false;

        bool const carrying = values.playerHasFlag;
        bool const runningForFlag = values.ctfDesignatedRunner && values.flagPickupAvailable && !values.ctfPickupIsReturn;
        if (!carrying && !runningForFlag)
            return false;

        ShapeshiftForm const form = player->GetShapeshiftForm();
        return form == FORM_TRAVEL || form == FORM_AQUA || (carrying && form == FORM_CAT);
    }

    SpellDecision SelectDruidFlagRunSpell(Player const* player, FlagRunThreats const& threats, bool carrying)
    {
        constexpr uint32 kTravelForm = 783;
        constexpr uint32 kCatForm = 768;
        constexpr uint32 kAquaticForm = 1066;
        constexpr uint32 kDash = 1850;
        constexpr uint32 kRejuvenation = 774;
        using TargetMode = playerbot::PvpClassSpellContext::TargetMode;

        ShapeshiftForm const form = player->GetShapeshiftForm();

        // The fastest form for what is underfoot: water, open ground, or the
        // keep, where Travel Form is refused and a carrier runs as a cat for
        // Dash. A runner not carrying yet only shifts where it gains speed,
        // and the flag click takes the form off again anyway.
        uint32 movementForm = 0;
        ShapeshiftForm wantedForm = FORM_NONE;
        char const* formAction = nullptr;
        if (player->IsInWater())
        {
            movementForm = kAquaticForm;
            wantedForm = FORM_AQUA;
            formAction = "druid aquatic form";
        }
        else if (IsUnderOpenSkyForDruidForm(player))
        {
            movementForm = kTravelForm;
            wantedForm = FORM_TRAVEL;
            formAction = "druid travel form";
        }
        else if (carrying)
        {
            movementForm = kCatForm;
            wantedForm = FORM_CAT;
            formAction = "druid cat form";
        }

        // Shifting is on a short leash: a slow standing in a Frost Trap comes
        // straight back, and a doorway can flip indoors/outdoors every step.
        bool const mayShift = movementForm != 0 &&
            !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kPlayerbotFlagRunFormShiftToken);

        if (mayShift && HasShapeshiftRemovableMovementImpairment(player) && IsFlagRunSpellCastable(player, movementForm, nullptr, true))
            return { formAction, "powershift out of a root or snare on the flag run", movementForm, TargetMode::Self, player->GetGUID() };

        if (carrying && player->GetHealthPct() < 55.0f && threats.nearestEnemyDistance > 25.0f &&
            !HasAuraFromSpellChain(player, kRejuvenation) && IsFlagRunSpellCastable(player, kRejuvenation, nullptr, true))
            return { "druid rejuvenation", "flag carrier heals while nobody is close", kRejuvenation, TargetMode::Self, player->GetGUID() };

        if (mayShift && form != wantedForm && (carrying || !player->IsMounted()) &&
            IsFlagRunSpellCastable(player, movementForm, nullptr, true))
            return { formAction, carrying ? "flag carrier shifts into its fastest form" : "flag runner shifts for speed on the way to the flag",
                movementForm, TargetMode::Self, player->GetGUID() };

        if (carrying && form == FORM_CAT && threats.nearestEnemyDistance <= 35.0f && !HasAuraFromSpellChain(player, kDash) &&
            IsFlagRunSpellCastable(player, kDash, nullptr, false))
            return { "druid dash", "flag carrier sprints away from pursuit", kDash, TargetMode::Self, player->GetGUID() };

        return {};
    }

    SpellDecision SelectMageFlagRunSpell(Player const* player, FlagRunThreats const& threats, bool carrying)
    {
        constexpr uint32 kBlink = 1953;
        constexpr uint32 kFrostNova = 122;
        constexpr uint32 kIceBarrier = 11426;
        constexpr uint32 kManaShield = 1463;
        using TargetMode = playerbot::PvpClassSpellContext::TargetMode;

        if (!carrying)
            return {};

        // Stunned or rooted, Blink is the answer whichever way it points, and
        // nothing else may be picked while it is ready: the stun fast path
        // (TryMageBlinkOutOfControl) only acts on a Blink decision, and a Frost
        // Nova picked ahead of it would leave the carrier standing in the stun.
        if (IsMageBlinkableControl(player) && IsSpellReady(player, kBlink))
        {
            if (IsFlagRunSpellCastable(player, kBlink, nullptr, false))
                return { "mage blink", "flag carrier blinks out of a stun or root", kBlink, TargetMode::Self, player->GetGUID() };
            return {};
        }

        bool const pursued = threats.nearestEnemyDistance <= 25.0f;
        bool const slowed = IsRootedOrSnared(player);

        // Blink is the mage's flag run: whenever something is chasing, down every
        // straight stretch of the route. With nobody near it is kept, so it is
        // ready for the stun that ends most runs.
        if ((pursued || slowed) && IsFlagRunSpellCastable(player, kBlink, nullptr, false) && IsBlinkDownRouteClear(player))
            return { "mage blink", slowed ? "flag carrier blinks out of a slow" : "flag carrier blinks away from pursuit",
                kBlink, TargetMode::Self, player->GetGUID() };

        if (threats.enemiesWithin10 > 0 && IsFlagRunSpellCastable(player, kFrostNova, nullptr, false))
            return { "mage frost nova", "flag carrier roots the chasers in place", kFrostNova, TargetMode::Self, player->GetGUID() };

        if (pursued && !HasAuraFromSpellChain(player, kIceBarrier) && IsFlagRunSpellCastable(player, kIceBarrier, nullptr, false))
            return { "mage ice barrier", "flag carrier shields up under pursuit", kIceBarrier, TargetMode::Self, player->GetGUID() };

        if (threats.nearestEnemyDistance <= 15.0f && player->GetHealthPct() < 80.0f &&
            !HasAuraFromSpellChain(player, kIceBarrier) && !HasAuraFromSpellChain(player, kManaShield) &&
            IsFlagRunSpellCastable(player, kManaShield, nullptr, false))
            return { "mage mana shield", "flag carrier absorbs hits it cannot outrun", kManaShield, TargetMode::Self, player->GetGUID() };

        return {};
    }

    SpellDecision SelectHunterFlagRunTrap(Player const* player, FlagRunThreats const& threats)
    {
        constexpr uint32 kFrostTrap = 13809;
        constexpr uint32 kFreezingTrap = 1499;

        if (player->GetClass() != CLASS_HUNTER || player->IsInCombat() || threats.nearestEnemyDistance > 35.0f)
            return {};

        uint32 const trapSpellId = IsSpellReady(player, kFrostTrap) ? kFrostTrap : kFreezingTrap;
        if (!IsFlagRunSpellCastable(player, trapSpellId, nullptr, false))
            return {};

        return { trapSpellId == kFrostTrap ? "hunter frost trap" : "hunter freezing trap",
            "flag carrier leaves a trap for the chasers", trapSpellId,
            playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };
    }

    SpellDecision SelectHunterFlagRunSpell(Player const* player, FlagRunThreats const& threats, bool carrying)
    {
        constexpr uint32 kAspectOfTheCheetah = 5118;
        constexpr uint32 kConcussiveShot = 5116;
        constexpr uint32 kWingClip = 2974;
        constexpr uint32 kScatterShot = 19503;
        using TargetMode = playerbot::PvpClassSpellContext::TargetMode;

        Unit const* chaser = threats.nearestEnemy;
        Unit const* meleeChaser = threats.nearestMeleeEnemy;

        if (carrying)
        {
            if (meleeChaser && threats.nearestMeleeDistance <= 5.0f && !IsRootedOrSnared(meleeChaser) &&
                IsFlagRunSpellCastable(player, kWingClip, meleeChaser, false))
                return { "hunter wing clip", "flag carrier clips the chaser that caught it", kWingClip, TargetMode::Enemy, meleeChaser->GetGUID() };

            if (meleeChaser && threats.nearestMeleeDistance <= 15.0f && !HasBreakableCrowdControl(player, meleeChaser) &&
                IsFlagRunSpellCastable(player, kScatterShot, meleeChaser, false))
                return { "hunter scatter shot", "flag carrier disorients a melee chaser", kScatterShot, TargetMode::Enemy, meleeChaser->GetGUID() };

            // Traps only go down out of combat on this realm, and any shot puts
            // the hunter in combat - so the trap goes on the route first, while
            // the chasers are still closing.
            if (SpellDecision const trap = SelectHunterFlagRunTrap(player, threats); trap.spellId)
                return trap;

            if (chaser && threats.nearestEnemyDistance <= 30.0f && !IsRootedOrSnared(chaser) && !HasBreakableCrowdControl(player, chaser) &&
                IsFlagRunSpellCastable(player, kConcussiveShot, chaser, false))
                return { "hunter concussive shot", "flag carrier slows the nearest chaser", kConcussiveShot, TargetMode::Enemy, chaser->GetGUID() };
        }

        // Cheetah dazes on every hit, so it is for open ground with nobody in
        // range. SelectBattlegroundFlagRunSpell takes it off again, for any
        // hunter, before anybody closes.
        if (!HasAuraFromSpellChain(player, kAspectOfTheCheetah) && threats.nearestEnemyDistance > 30.0f &&
            (carrying || !player->IsMounted()) && IsFlagRunSpellCastable(player, kAspectOfTheCheetah, nullptr, false))
            return { "hunter aspect of the cheetah", carrying ? "flag carrier runs in cheetah with nobody in range" :
                "flag runner runs in cheetah on the way to the flag", kAspectOfTheCheetah, TargetMode::Self, player->GetGUID() };

        return {};
    }

    // Monkey, else Hawk, in place of Cheetah once an enemy is close enough to
    // land the hit that would daze.
    SpellDecision SelectHunterCheetahDrop(Player const* player, FlagRunThreats const& threats)
    {
        constexpr uint32 kAspectOfTheCheetah = 5118;
        constexpr uint32 kAspectOfTheMonkey = 13163;
        constexpr uint32 kAspectOfTheHawk = 13165;

        if (threats.nearestEnemyDistance > 20.0f || !HasAuraFromSpellChain(player, kAspectOfTheCheetah))
            return {};

        uint32 const combatAspect = ResolveKnownPlayerSpellInChain(player, kAspectOfTheMonkey) ? kAspectOfTheMonkey : kAspectOfTheHawk;
        if (!IsFlagRunSpellCastable(player, combatAspect, nullptr, false))
            return {};

        return { "hunter aspect swap", "drop cheetah before a hit dazes", combatAspect,
            playerbot::PvpClassSpellContext::TargetMode::Self, player->GetGUID() };
    }

    SpellDecision SelectWarriorFlagRunSpell(Player const* player, FlagRunThreats const& threats)
    {
        constexpr uint32 kLastStand = 12975;
        constexpr uint32 kShieldWall = 871;
        constexpr uint32 kShieldBlock = 2565;
        constexpr uint32 kIntimidatingShout = 5246;
        constexpr uint32 kPiercingHowl = 12323;
        constexpr uint32 kHamstring = 1715;
        using TargetMode = playerbot::PvpClassSpellContext::TargetMode;

        float const healthPct = player->GetHealthPct();
        bool const underAttack = threats.nearestEnemyDistance <= 30.0f;
        Unit const* meleeChaser = threats.nearestMeleeEnemy;

        if (underAttack && healthPct < 30.0f && IsFlagRunSpellCastable(player, kLastStand, nullptr, false))
            return { "warrior last stand", "flag carrier buys health at the brink", kLastStand, TargetMode::Self, player->GetGUID() };

        if (underAttack && healthPct < 40.0f && IsFlagRunSpellCastable(player, kShieldWall, nullptr, false))
            return { "warrior shield wall", "flag carrier walls up while low", kShieldWall, TargetMode::Self, player->GetGUID() };

        if (meleeChaser && threats.nearestMeleeDistance <= 8.0f && (threats.enemiesWithin10 >= 2 || healthPct < 60.0f) &&
            IsFlagRunSpellCastable(player, kIntimidatingShout, meleeChaser, false))
            return { "warrior intimidating shout", "flag carrier fears off the chasers", kIntimidatingShout, TargetMode::Enemy, meleeChaser->GetGUID() };

        if (threats.enemiesWithin10 > 0 && meleeChaser && !IsRootedOrSnared(meleeChaser) &&
            IsFlagRunSpellCastable(player, kPiercingHowl, nullptr, false))
            return { "warrior piercing howl", "flag carrier slows everything around it", kPiercingHowl, TargetMode::Self, player->GetGUID() };

        if (meleeChaser && threats.nearestMeleeDistance <= 5.0f && !IsRootedOrSnared(meleeChaser) &&
            IsFlagRunSpellCastable(player, kHamstring, meleeChaser, false))
            return { "warrior hamstring", "flag carrier hamstrings the chaser on it", kHamstring, TargetMode::Enemy, meleeChaser->GetGUID() };

        if (meleeChaser && threats.nearestMeleeDistance <= 5.0f && IsFlagRunSpellCastable(player, kShieldBlock, nullptr, false))
            return { "warrior shield block", "flag carrier blocks the chaser on it", kShieldBlock, TargetMode::Self, player->GetGUID() };

        return {};
    }

    // The classes without a named flag-run kit still use what they have.
    SpellDecision SelectGenericFlagRunSpell(Player const* player, FlagRunThreats const& threats)
    {
        using TargetMode = playerbot::PvpClassSpellContext::TargetMode;
        Unit const* chaser = threats.nearestEnemy;
        bool const pursued = threats.nearestEnemyDistance <= 30.0f;

        switch (player->GetClass())
        {
            case CLASS_ROGUE:
            {
                // Cast through the wrapper chain; the aura is the stock Sprint it casts.
                constexpr uint32 kSprint = 90516;
                constexpr uint32 kSprintAura = 2983;
                constexpr uint32 kEvasion = 5277;
                if (pursued && !HasAuraFromSpellChain(player, kSprintAura) && IsFlagRunSpellCastable(player, kSprint, nullptr, false))
                    return { "rogue sprint", "flag carrier sprints away from pursuit", kSprint, TargetMode::Self, player->GetGUID() };
                if (threats.nearestMeleeDistance <= 5.0f && player->GetHealthPct() < 70.0f &&
                    IsFlagRunSpellCastable(player, kEvasion, nullptr, false))
                    return { "rogue evasion", "flag carrier dodges the chaser on it", kEvasion, TargetMode::Self, player->GetGUID() };
                break;
            }
            case CLASS_PALADIN:
            {
                constexpr uint32 kFreedom = 1044;
                if (IsRootedOrSnared(player) && IsFlagRunSpellCastable(player, kFreedom, nullptr, false))
                    return { "paladin freedom", "flag carrier frees itself from a root or snare", kFreedom, TargetMode::Self, player->GetGUID() };
                break;
            }
            case CLASS_PRIEST:
            {
                constexpr uint32 kPowerWordShield = 17;
                constexpr uint32 kRenew = 139;
                if (pursued && !HasAuraFromSpellChain(player, kPowerWordShield) && !player->HasAura(kPriestWeakenedSoulSpellId) &&
                    IsFlagRunSpellCastable(player, kPowerWordShield, nullptr, false))
                    return { "priest power word: shield", "flag carrier shields up under pursuit", kPowerWordShield, TargetMode::Self, player->GetGUID() };
                if (player->GetHealthPct() < 80.0f && !HasAuraFromSpellChain(player, kRenew) &&
                    IsFlagRunSpellCastable(player, kRenew, nullptr, false))
                    return { "priest renew", "flag carrier heals on the move", kRenew, TargetMode::Self, player->GetGUID() };
                break;
            }
            case CLASS_SHAMAN:
            {
                constexpr uint32 kFrostShock = 8056;
                if (chaser && threats.nearestEnemyDistance <= 20.0f && !IsRootedOrSnared(chaser) &&
                    IsFlagRunSpellCastable(player, kFrostShock, chaser, false))
                    return { "shaman frost shock", "flag carrier slows the nearest chaser", kFrostShock, TargetMode::Enemy, chaser->GetGUID() };
                break;
            }
            case CLASS_WARLOCK:
            {
                constexpr uint32 kDeathCoil = 6789;
                Unit const* meleeChaser = threats.nearestMeleeEnemy;
                if (meleeChaser && threats.nearestMeleeDistance <= 20.0f && player->GetHealthPct() < 60.0f &&
                    !HasBreakableCrowdControl(player, meleeChaser) && IsFlagRunSpellCastable(player, kDeathCoil, meleeChaser, false))
                    return { "warlock death coil", "flag carrier horrifies the chaser on it", kDeathCoil, TargetMode::Enemy, meleeChaser->GetGUID() };
                break;
            }
            default:
                break;
        }

        return {};
    }

    SpellDecision SelectBattlegroundFlagRunSpell(Player const* player, playerbot::PvpValues const& values)
    {
        bool const carrying = values.playerHasFlag;
        // The runner's trip to the enemy flag gets the speed moves only.
        bool const runningForFlag = !carrying && values.ctfDesignatedRunner && values.flagPickupAvailable &&
            !values.ctfPickupIsReturn;
        // Nothing else in the engine touches aspects, so a hunter that ran in
        // Cheetah and has since captured, stopped to hold, or become an escort
        // is still wearing it - and would be dazed by every hit.
        bool const hunterInCheetah = player->GetClass() == CLASS_HUNTER && HasAuraFromSpellChain(player, 5118);
        if (!carrying && !runningForFlag && !hunterInCheetah)
            return {};

        FlagRunThreats const threats = ScanFlagRunThreats(player);

        if (hunterInCheetah)
            if (SpellDecision const cheetahDrop = SelectHunterCheetahDrop(player, threats); cheetahDrop.spellId)
                return cheetahDrop;

        if (!carrying && !runningForFlag)
            return {};

        // A carrier waiting in its own flag room is not running anywhere; the
        // rotation fights from there. A hunter still traps the doorway.
        if (values.ctfCarrierHolding)
            return SelectHunterFlagRunTrap(player, threats);
        switch (player->GetClass())
        {
            case CLASS_DRUID:
                return SelectDruidFlagRunSpell(player, threats, carrying);
            case CLASS_MAGE:
                return SelectMageFlagRunSpell(player, threats, carrying);
            case CLASS_HUNTER:
                return SelectHunterFlagRunSpell(player, threats, carrying);
            case CLASS_WARRIOR:
                return carrying ? SelectWarriorFlagRunSpell(player, threats) : SpellDecision{};
            default:
                return carrying ? SelectGenericFlagRunSpell(player, threats) : SpellDecision{};
        }
    }

    void ConsiderMovementDirective(playerbot::PvpClassSpellContext& context, playerbot::PvpClassSpellContext::MovementDirective directive,
        ObjectGuid targetGuid, float followRange, char const* actionName, char const* reason, float priority)
    {
        if (priority < context.movementPriority)
            return;

        context.movementDirective = directive;
        context.movementTargetGuid = targetGuid;
        context.movementFollowRange = followRange;
        context.actionName = actionName;
        context.reason = reason;
        context.shouldExecute = true;
        context.movementPriority = priority;
    }

    TacticalDecision SelectBattlegroundTacticalDecision(Player const* player, playerbot::PvpValues const& values)
    {
        TacticalDecision decision;
        if (!player)
            return decision;

        bool const lowHealth = player->HealthBelowPct(35);
        bool const lowMana = player->GetPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 25.0f;
        bool const bgActive = values.battlegroundState == playerbot::BattlegroundState::Active;
        bool const bgWaiting = values.battlegroundState == playerbot::BattlegroundState::WaitingToStart;

        struct TacticalRule
        {
            char const* triggerName;
            bool condition;
            char const* actionName;
            float priority;
        };

        // Keep pressure behavior generic, but elevate flag-carrier directives when
        // objective triggers report an active carrier. This ensures CTF matches
        // chase carriers instead of defaulting to midfield skirmishes.
        bool const enemyFlagCarrierActive = values.enemyFlagCarrierActive;
        bool const teamFlagCarrierNear = values.teamFlagCarrierNear;

        // A battleground that plays its bots itself (Battleground::GetBotOrders -
        // Summoner's Rift's laning) outranks every generic rule: its orders are
        // the whole of what the bot does.
        if (bgActive)
            if (Battleground* battleground = player->GetBattleground())
            {
                BattlegroundBotOrders orders;
                if (battleground->GetBotOrders(const_cast<Player*>(player), orders))
                {
                    decision.triggerName = "bg orders";
                    decision.actionName = "bg follow orders";
                    decision.priority = 102.0f;
                    return decision;
                }
            }

        // Warsong Gulch and Twin Peaks, played as a team (CtfCoordinator). Only
        // the designated runner heads for the enemy flag from afar; a bot takes
        // a flag it is standing near, returns ours when it is one of the closest,
        // and otherwise does its role: escorts stay on our carrier (or ride with
        // the runner), defenders hold the flag room, and everyone but an escort
        // with a carrier to guard turns on an enemy carrier.
        if (values.ctfRole != 0)
        {
            playerbot::CtfRole const role = playerbot::CtfRole(values.ctfRole);
            bool const isRunner = role == playerbot::CtfRole::Runner;
            bool const isEscort = role == playerbot::CtfRole::Escort;
            bool const isDefender = role == playerbot::CtfRole::Defender;
            bool const teamCarrierClose = values.ctfTeamCarrierActive && values.ctfTeamCarrierDistance <= 60.0f;

            std::array<TacticalRule, 14> const ctfRules =
            {{
                { "flag handoff give", bgActive && values.ctfHandoffGive, "bg flag handoff", 101.0f },
                { "player has flag", bgActive && values.playerHasFlag, "bg move to objective", 100.0f },
                { "flag pickup nearby", bgActive && values.flagPickupNearby, "bg move to objective", 99.0f },
                { "flag handoff receive", bgActive && values.ctfHandoffReceive, "bg flag handoff", 98.0f },
                { "own flag return", bgActive && values.flagPickupAvailable && values.ctfPickupIsReturn, "bg move to objective", 97.0f },
                { "flag runner goes for the flag", bgActive && isRunner && values.flagPickupAvailable, "bg move to objective", 96.0f },
                { "flag near", bgActive && values.flagPickupAvailable, "bg move to objective", 95.5f },
                { "escort team carrier", bgActive && isEscort && values.ctfTeamCarrierActive, "bg escort flag runner", 95.2f },
                { "enemy flag carrier active", bgActive && enemyFlagCarrierActive, "attack enemy flag carrier", 95.0f },
                { "flag runner joins team carrier", bgActive && isRunner && values.ctfTeamCarrierActive, "bg escort flag runner", 90.0f },
                { "flag runner waits at enemy flag", bgActive && isRunner, "bg flag runner stage", 88.0f },
                { "escort flag runner", bgActive && isEscort, "bg escort flag runner", 85.0f },
                { "defend flag room", bgActive && isDefender, "bg defend flag room", 80.0f },
                { "team flag carrier near", bgActive && teamCarrierClose, "bg escort flag runner", 70.0f }
            }};

            for (TacticalRule const& rule : ctfRules)
            {
                if (rule.condition)
                {
                    decision.triggerName = rule.triggerName;
                    decision.actionName = rule.actionName;
                    decision.priority = rule.priority;
                    return decision;
                }
            }

            if (bgActive)
            {
                decision.triggerName = "bg active";
                decision.actionName = "bg pursue enemy";
                decision.priority = 60.0f;
            }

            return decision;
        }

    // A gate to bring down or hold and a commander to kill are what the bot
    // was sent for; the flag duties travel through the flag rules instead.
    bool const siegeOrCommanderDuty =
        values.objectiveDuty == uint8(playerbot::ObjectiveDuty::CarryCharge) ||
        values.objectiveDuty == uint8(playerbot::ObjectiveDuty::DriveSiege) ||
        values.objectiveDuty == uint8(playerbot::ObjectiveDuty::GuardGate) ||
        values.objectiveDuty == uint8(playerbot::ObjectiveDuty::AssaultBoss);

    std::array<TacticalRule, 13> const rules =
    {{
        { "bg waiting", bgWaiting, "bg move to start", 50.0f },
        { "player has flag", bgActive && values.playerHasFlag, "bg move to objective", 100.0f },
        { "flag pickup nearby", bgActive && values.flagPickupNearby, "bg move to objective", 99.0f },
        { "enemy flag carrier active", bgActive && enemyFlagCarrierActive, "attack enemy flag carrier", 95.0f },
        { "objective duty", bgActive && siegeOrCommanderDuty, "bg objective duty", 93.0f },
        { "flag pickup available", bgActive && values.flagPickupAvailable, "bg move to objective", 90.0f },
        // A base of ours being taken is the cheapest base on the map and it is
        // lost on a timer, so it outranks escorting a carrier around.
        { "node under attack", bgActive && values.nodeUrgent, "bg move to objective", 86.0f },
        { "team flag carrier near", bgActive && teamFlagCarrierNear, "bg protect fc", 80.0f },
        { "node assault available", bgActive && values.nodeAssaultAvailable, "bg move to objective", 75.0f },
        { "node defense available", bgActive && values.nodeDefenseAvailable, "bg move to objective", 74.0f },
        { "bg active", bgActive, "bg pursue enemy", 60.0f },
        { "low health", lowHealth, "bg use buff", 45.0f },
        { "low mana", lowMana, "bg use buff", 45.0f }
    }};

        for (TacticalRule const& rule : rules)
        {
            if (rule.condition)
            {
                decision.triggerName = rule.triggerName;
                decision.actionName = rule.actionName;
                decision.priority = rule.priority;
                return decision;
            }
        }

        return decision;
    }
}

namespace playerbot
{
    uint32 PvpCore::CountHumanPlayersOnBattlegroundTeam(Player const* player)
    {
        if (!player || !player->InBattleground() || !player->FindMap())
            return 0;

        Battleground const* battleground = player->GetBattleground();
        if (!battleground)
            return 0;

        uint32 const botBgTeam = player->GetBGTeam() ? player->GetBGTeam() : player->GetTeam();
        uint32 humanCount = 0;

        Map::PlayerList const& players = player->FindMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player const* teammate = itr->GetSource();
            if (!teammate || teammate->GetBattlegroundId() != player->GetBattlegroundId())
                continue;

            uint32 const teammateBgTeam = teammate->GetBGTeam() ? teammate->GetBGTeam() : teammate->GetTeam();
            if (battleground->IsSquadMatch() ? !battleground->AreTeammates(player->GetGUID(), teammate->GetGUID()) :
                teammateBgTeam != botBgTeam)
                continue;

            if (!IsManagedRandomBot(teammate))
                ++humanCount;
        }

        return humanCount;
    }

bool PvpCore::TeamHasHumanPlayers(Player const* player)
{
    return CountHumanPlayersOnBattlegroundTeam(player) > 0;
}

GameObject* PvpCore::FindUsableLightwell(Player const* player, float maxDistance)
{
    if (!player || !player->IsAlive() || !player->FindMap() || maxDistance <= 0.0f)
        return nullptr;

    std::list<GameObject*> lightwells;
    player->GetGameObjectListWithEntryInGrid(lightwells, kPriestLightwellGameObjectEntry, maxDistance);

    GameObject* nearest = nullptr;
    float nearestDistance = maxDistance;
    for (GameObject* lightwell : lightwells)
    {
        if (!lightwell || !lightwell->IsInWorld() || !lightwell->isSpawned() ||
            lightwell->GetGoType() != GAMEOBJECT_TYPE_SPELLCASTER)
            continue;

        GameObjectTemplate const* lightwellTemplate = lightwell->GetGOInfo();
        if (!lightwellTemplate || (lightwellTemplate->spellcaster.charges &&
            lightwell->GetUseCount() >= lightwellTemplate->spellcaster.charges))
            continue;

        Unit* owner = lightwell->GetOwner();
        Player* ownerPlayer = owner ? owner->ToPlayer() : nullptr;
        if (!ownerPlayer || !player->IsInSameRaidWith(ownerPlayer))
            continue;

        float const distance = player->GetExactDist(lightwell);
        if (distance <= nearestDistance)
        {
            nearest = lightwell;
            nearestDistance = distance;
        }
    }

    return nearest;
}

bool PvpCore::ShouldSeekLightwell(Player const* player)
{
    return player && player->InBattleground() && player->IsAlive() &&
        !IsBattlegroundFlagCarrier(player) && player->GetHealthPct() < 65.0f &&
        !(player->GetHealthPct() < 50.0f && SelectReadyHealthstoneItemEntry(player)) &&
        !player->HasAura(kPriestLightwellRenewSpellId) && FindUsableLightwell(player, 20.0f) != nullptr;
}

bool PvpCore::IsTeamInDanger(Player const* player)
{
    return EvaluateTeamDanger(player, false).inDanger;
}

bool PvpCore::HasHealthstone(Player const* player)
{
    if (!player)
        return false;

    for (uint32 itemEntry : kHealthstoneItemEntries)
        if (player->GetItemByEntry(itemEntry))
            return true;

    return false;
}

GameObject* PvpCore::FindUsableSoulwell(Player const* player, float maxDistance)
{
    if (!player || !player->IsAlive() || !player->FindMap() || maxDistance <= 0.0f)
        return nullptr;

    GameObject* nearest = nullptr;
    float nearestDistance = maxDistance;
    for (uint32 soulwellEntry : kWarlockSoulwellGameObjectEntries)
    {
        std::list<GameObject*> soulwells;
        player->GetGameObjectListWithEntryInGrid(soulwells, soulwellEntry, maxDistance);
        for (GameObject* soulwell : soulwells)
        {
            if (!soulwell || !soulwell->IsInWorld() || !soulwell->isSpawned() ||
                soulwell->GetGoType() != GAMEOBJECT_TYPE_SPELLCASTER)
                continue;

            GameObjectTemplate const* soulwellTemplate = soulwell->GetGOInfo();
            if (!soulwellTemplate || (soulwellTemplate->spellcaster.charges &&
                soulwell->GetUseCount() >= soulwellTemplate->spellcaster.charges))
                continue;

            Unit* owner = soulwell->GetOwner();
            Player* ownerPlayer = owner ? owner->ToPlayer() : nullptr;
            if (!ownerPlayer || !player->IsInSameRaidWith(ownerPlayer))
                continue;

            if (IsOnDropIslandApartFrom(player, soulwell))
                continue;

            float const distance = player->GetExactDist(soulwell);
            if (distance <= nearestDistance)
            {
                nearest = soulwell;
                nearestDistance = distance;
            }
        }
    }

    return nearest;
}

bool PvpCore::IsBattlegroundFlagCarrier(Player const* player)
{
    if (!player || !player->InBattleground())
        return false;

    Battleground const* battleground = player->GetBattleground();
    if (!battleground)
        return false;

    std::vector<ObjectGuid> carriers;
    battleground->GetObjectiveCarriers(carriers);
    if (std::find(carriers.begin(), carriers.end(), player->GetGUID()) != carriers.end())
        return true;

    return std::any_of(kBattlegroundFlagCarrierAuraIds.begin(), kBattlegroundFlagCarrierAuraIds.end(),
        [player](uint32 spellId) { return player->HasAura(spellId); });
}

bool PvpCore::SpellWouldBreakFlagCarry(uint32 spellId)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    return spellInfo && spellInfo->WouldDropBattlegroundFlag();
}

uint8 PvpCore::GetFlagRunnerPriority(Player const* player)
{
    if (!player)
        return 0;

    switch (player->GetClass())
    {
        case CLASS_DRUID:
            return 4;
        case CLASS_WARRIOR:
            return DetectClassicClassProfile(player).profile == ClassicClassProfile::TertiaryClassic ? 3 : 0;
        case CLASS_MAGE:
            return 2;
        case CLASS_HUNTER:
            return 1;
        default:
            return 0;
    }
}

// Guarded through PlayerbotSharedStateGuard: the PvE manager flips engagement
// from the bot's own map-update thread, but battleground entry cleanup and GM
// commands can touch it cross-map.
static std::unordered_set<uint64> g_PveCombatEngagedGuids;

void PvpCore::SetPveCombatEngagement(ObjectGuid const& botGuid, bool engaged)
{
    if (botGuid.IsEmpty())
        return;

    std::lock_guard<std::mutex> guard(SharedBotStateStructureLock());
    if (engaged)
        g_PveCombatEngagedGuids.insert(botGuid.GetRawValue());
    else
        g_PveCombatEngagedGuids.erase(botGuid.GetRawValue());
}

bool PvpCore::IsPveCombatEngaged(Player const* player)
{
    if (!player)
        return false;

    std::lock_guard<std::mutex> guard(SharedBotStateStructureLock());
    return g_PveCombatEngagedGuids.find(player->GetGUID().GetRawValue()) != g_PveCombatEngagedGuids.end();
}

bool PvpCore::TryCastOpenWorldBuff(Player* player)
{
    if (!player || !player->IsAlive() || player->IsInCombat() || !player->IsInWorld())
        return false;

    // Never clobber an in-flight cast - pet summons take ten seconds and this
    // runs on a sub-second cadence.
    if (player->HasUnitState(UNIT_STATE_CASTING))
        return false;

    // Pet upkeep before buffs: a petless warlock or hunter in the open world
    // is missing half its class (battleground preparation used to be the only
    // place pets were summoned).
    if (player->GetClass() == CLASS_WARLOCK && !player->GetPet() && !IsPetParkedByMount(player))
    {
        // Voidwalker tanks the grind; the Imp is the level-1 fallback. There
        // are no soul shard costs on this realm, so no shard check.
        uint32 const summonSpellId = SelectWarlockFallbackSummonSpell(player);
        SpellInfo const* summonSpellInfo = summonSpellId ? sSpellMgr->GetSpellInfo(summonSpellId) : nullptr;

        // Ten seconds of cast time, and Spell::prepare refuses cast time on the
        // move. A bot out here is nearly always walking somewhere, so this
        // failed almost every time and the warlock met its next fight without
        // a demon. Stop for it the way the bot stops to eat - only with nobody
        // around who could use the ten seconds (the mount rule's range), never
        // in the saddle, mid-fall or on a flight path, and not inside the
        // global cooldown the last buff started. A stop the core refuses
        // anyway is not tried again for a while: one repeated every tick would
        // hold the bot still until its walk gave up on it.
        if (summonSpellInfo && IsSpellReady(player, summonSpellId) &&
            !player->GetSpellHistory()->HasGlobalCooldown(summonSpellInfo) &&
            !player->IsMounted() && !player->IsInFlight() && !player->Unit::IsFalling() &&
            !playerbot::PvpClassActions::IsCasterSpellCooldownActive(player, kWarlockOpenWorldSummonRetryToken) &&
            !HasNearbyHostilePlayer(player, playerbot::PLAYERBOT_MOUNT_ENEMY_AWARENESS_RANGE, false))
        {
            // Only the spline: the walk's own movement order waits out the
            // cast and carries on by itself afterwards.
            if (player->isMoving())
                player->StopMoving();

            if (player->CastSpell(player, summonSpellId, false) == SPELL_CAST_OK)
                return true;

            playerbot::PvpClassActions::RegisterCasterSpellCooldown(player, kWarlockOpenWorldSummonRetryToken, std::chrono::seconds(30));
        }
    }

    if (player->GetClass() == CLASS_HUNTER && !player->GetPet() && !IsPetParkedByMount(player) &&
        player->HasSpell(883) && IsSpellReady(player, 883))
    {
        if (player->CastSpell(player, 883, false) == SPELL_CAST_OK)
            return true;
    }

    // {primary chain, alternate chain}: the alternate is the lower-level
    // stand-in (Demon Skin before Demon Armor, Monkey before Hawk); holding
    // EITHER version's aura satisfies the slot.
    struct OpenWorldBuff
    {
        uint32 chainSpellId;
        uint32 alternateChainSpellId;
    };

    std::vector<OpenWorldBuff> candidates;
    switch (player->GetClass())
    {
        case CLASS_WARRIOR:
            // Battle Shout costs rage; only worth trying with leftover rage
            // from the last fight.
            if (player->GetPower(POWER_RAGE) >= 100)
                candidates.push_back({ 6673, 0 });
            break;
        case CLASS_PALADIN:
            candidates.push_back({ 19740, 0 });  // Blessing of Might
            candidates.push_back({ 465, 7294 }); // Devotion Aura unless a Retribution Aura is up
            break;
        case CLASS_HUNTER:
            candidates.push_back({ 13165, 13163 }); // Aspect of the Hawk, else Monkey
            break;
        case CLASS_PRIEST:
            candidates.push_back({ 1243, 0 });   // Power Word: Fortitude
            candidates.push_back({ 588, 0 });    // Inner Fire
            candidates.push_back({ 14752, 0 });  // Divine Spirit
            break;
        case CLASS_SHAMAN:
            if (!HasElementalShield(player))     // never over an Earth Shield
                candidates.push_back({ 324, 0 }); // Lightning Shield
            break;
        case CLASS_MAGE:
            candidates.push_back({ 1459, 0 });   // Arcane Intellect
            candidates.push_back({ 7302, 168 }); // Ice Armor, else Frost Armor
            break;
        case CLASS_WARLOCK:
            candidates.push_back({ 706, 687 });  // Demon Armor, else Demon Skin
            break;
        case CLASS_DRUID:
            candidates.push_back({ 1126, 0 });   // Mark of the Wild
            candidates.push_back({ 467, 0 });    // Thorns
            break;
        default:
            break;
    }

    if (candidates.empty())
        return false;

    // Keep clear of the eat/drink loop: rest starts under 50% mana, so a
    // buff that spends mana only fires comfortably above it.
    bool const isManaUser = player->GetMaxPower(POWER_MANA) > 0;
    if (isManaUser && player->GetPowerPct(POWER_MANA) < 60.0f)
        return false;

    for (OpenWorldBuff const& candidate : candidates)
    {
        uint32 chainId = candidate.chainSpellId;
        uint32 resolved = ResolveKnownPlayerSpellInChain(player, chainId);
        if (!resolved && candidate.alternateChainSpellId)
        {
            chainId = candidate.alternateChainSpellId;
            resolved = ResolveKnownPlayerSpellInChain(player, chainId);
        }

        if (!resolved)
            continue;

        if (HasAuraFromSpellChain(player, candidate.chainSpellId) ||
            (candidate.alternateChainSpellId && HasAuraFromSpellChain(player, candidate.alternateChainSpellId)))
            continue;

        if (!IsSpellReady(player, chainId))
            continue;

        if (player->CastSpell(player, resolved, false) == SPELL_CAST_OK)
            return true;
    }

    return false;
}

void PvpCore::LoadConfig()
{
    g_PvpCoreConfig.moduleEnabled = sConfigMgr->GetBoolDefault("Playerbot.Enable", false);
    g_PvpCoreConfig.pvpCoreEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpCore.Enable", false);
    g_PvpCoreConfig.pvpTacticsEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpTactics.Enable", false);
    g_PvpCoreConfig.pvpLifecycleEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpLifecycle.Enable", false);
    g_PvpCoreConfig.pvpPersistentLifecycleEnabled = sConfigMgr->GetBoolDefault(
        "Playerbot.PvpLifecycle.PersistentBots.Enable", true);
    g_PvpCoreConfig.pvpClassSpellsEnabled = sConfigMgr->GetBoolDefault("Playerbot.PvpClassSpells.Enable", false);
    g_PvpCoreConfig.usePotions = sConfigMgr->GetBoolDefault("Playerbot.PvpClassSpells.UsePotions", true);
    g_PvpCoreConfig.spellRange = sConfigMgr->GetFloatDefault("Playerbot.PvpClassSpells.Range.Spell", 30.0f);
    g_PvpCoreConfig.healRange = sConfigMgr->GetFloatDefault("Playerbot.PvpClassSpells.Range.Heal", 40.0f);
    g_PvpCoreConfig.meleeRange = sConfigMgr->GetFloatDefault("Playerbot.PvpClassSpells.Range.Melee", 8.0f);
    g_PvpCoreConfig.closeRange = sConfigMgr->GetFloatDefault("Playerbot.PvpClassSpells.Range.Close", 15.0f);
    g_PvpCoreConfig.longRange = sConfigMgr->GetFloatDefault("Playerbot.PvpClassSpells.Range.Long", 35.0f);
    g_PvpCoreConfig.interruptReactionMinMs = uint32(std::clamp(
        sConfigMgr->GetIntDefault("Playerbot.PvpClassSpells.Interrupt.ReactionMinMs", 200), 0, 5000));
    g_PvpCoreConfig.interruptReactionMaxMs = uint32(std::clamp(
        sConfigMgr->GetIntDefault("Playerbot.PvpClassSpells.Interrupt.ReactionMaxMs", 1000), 0, 5000));
    if (g_PvpCoreConfig.interruptReactionMaxMs < g_PvpCoreConfig.interruptReactionMinMs)
        g_PvpCoreConfig.interruptReactionMaxMs = g_PvpCoreConfig.interruptReactionMinMs;
    g_PvpCoreConfig.interruptJitterMs = uint32(std::clamp(
        sConfigMgr->GetIntDefault("Playerbot.PvpClassSpells.Interrupt.JitterMs", 100), 0, 1000));
    g_PvpCoreConfig.interruptCommitWindowMs = uint32(std::clamp(
        sConfigMgr->GetIntDefault("Playerbot.PvpClassSpells.Interrupt.CommitWindowMs", 350), 0, 2000));
    g_PvpCoreConfig.interruptOffTargetExtraMs = uint32(std::clamp(
        sConfigMgr->GetIntDefault("Playerbot.PvpClassSpells.Interrupt.OffTargetExtraMs", 500), 0, 5000));
    g_PvpCoreConfig.interruptOffTargetNoticePct = uint32(std::clamp(
        sConfigMgr->GetIntDefault("Playerbot.PvpClassSpells.Interrupt.OffTargetNoticePct", 50), 0, 100));
}

bool PvpCore::IsInterruptPressDecision(Player const* player, PvpClassSpellContext const& context)
{
    if (!player || !context.shouldExecute || !context.spellId || context.targetGuid.IsEmpty())
        return false;

    // Interrupts, plus the stuns, silences and incapacitates the class selectors
    // also aim at a caster (Hammer of Justice, Silence, Repentance, Scatter
    // Shot). Anything else aimed at that caster keeps the normal cadence.
    uint32 constexpr castStoppingMechanics = (1 << MECHANIC_INTERRUPT) | (1 << MECHANIC_SILENCE) |
        (1 << MECHANIC_STUN) | (1 << MECHANIC_KNOCKOUT) | (1 << MECHANIC_DISORIENTED);
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(context.spellId);
    if (!spellInfo || (!spellInfo->HasEffect(SPELL_EFFECT_INTERRUPT_CAST) &&
        !(spellInfo->GetAllEffectsMechanicMask() & castStoppingMechanics)))
        return false;

    // Only when the press can actually go out now. A bot finishing its own
    // cast, or waiting out the global cooldown, presses on the first tick after.
    if (player->IsNonMeleeSpellCast(false, false, true) || player->GetSpellHistory()->HasGlobalCooldown(spellInfo))
        return false;

    std::lock_guard<std::mutex> lock(g_InterruptReactionStateLock);
    auto const stateItr = g_InterruptReactionStateByBot.find(player->GetGUID());
    if (stateItr == g_InterruptReactionStateByBot.end() ||
        stateItr->second.pressReturnedAtMs != GameTime::GetGameTimeMS())
        return false;

    std::vector<ObjectGuid> const& pressCasterGuids = stateItr->second.pressCasterGuids;
    return std::find(pressCasterGuids.begin(), pressCasterGuids.end(), context.targetGuid) != pressCasterGuids.end();
}

    PvpCoreConfig const& PvpCore::GetConfig()
    {
        return g_PvpCoreConfig;
    }

bool PvpCore::GetHunterAutoShotRange(Player const* player, Unit const* target, HunterAutoShotRangeInfo& rangeInfo)
{
    rangeInfo = {};
    if (!player || player->GetClass() != CLASS_HUNTER || !target || !target->IsAlive() || !player->HasSpell(kHunterAutoShotSpellId))
        return false;

    SpellInfo const* autoShotInfo = sSpellMgr->GetSpellInfo(kHunterAutoShotSpellId);
    if (!autoShotInfo || !autoShotInfo->RangeEntry)
        return false;

    rangeInfo.exactDistance = player->GetExactDist(target);
    rangeInfo.dbcMinRange = player->GetSpellMinRangeForTarget(target, autoShotInfo);
    if (autoShotInfo->RangeEntry->Flags & SPELL_RANGE_RANGED)
        rangeInfo.meleeRange = player->GetMeleeRange(target);
    rangeInfo.minRange = rangeInfo.dbcMinRange + rangeInfo.meleeRange;

    // Keep this in the same order as Spell::GetMinMaxRange(): target-sensitive
    // base range, ranged-weapon range modifier, caster spell-range modifiers,
    // then combat reach (plus the moving-target tolerance used by the core).
    rangeInfo.maxRange = player->GetSpellMaxRangeForTarget(target, autoShotInfo);
    if (autoShotInfo->HasAttribute(SPELL_ATTR0_REQ_AMMO))
        if (Item* rangedWeapon = player->GetWeaponForAttack(RANGED_ATTACK, true))
            rangeInfo.maxRange *= rangedWeapon->GetTemplate()->RangedModRange * 0.01f;

    if (Player* modOwner = player->GetSpellModOwner())
        modOwner->ApplySpellMod(autoShotInfo->Id, SPELLMOD_RANGE, rangeInfo.maxRange, nullptr);

    float maxRangeMod = player->GetCombatReach() + target->GetCombatReach();
    if (player->isMoving() && target->isMoving() && !player->IsWalking() && !target->IsWalking() && target->GetTypeId() == TYPEID_PLAYER)
        maxRangeMod += 8.0f / 3.0f;
    rangeInfo.maxRange += maxRangeMod;

    return rangeInfo.minRange > 0.0f && rangeInfo.maxRange > rangeInfo.minRange;
}

bool PvpCore::CanMageBlinkOutOfControl(Player const* player)
{
    return IsMageBlinkableControl(player) && IsSpellReady(player, 1953);
}

bool PvpCore::CanHunterBestialWrathOutOfControl(Player const* player)
{
    return IsHunterBestialWrathBreakableControl(player) && IsSpellReady(player, 81300);
}

bool PvpCore::IsEffectivelyImmuneTarget(Player const* player, Unit const* target)
{
    return IsTargetEffectivelyImmune(player, target);
}

// The core's test, Unit::HasBreakableByDamageCrowdControlAura, only knows CCs
// carrying AURA_INTERRUPT_FLAG_TAKE_DAMAGE. Fear, Howl of Terror, Psychic
// Scream, Intimidating Shout and Scare Beast carry none on this realm: they
// break through their damage-taken proc (AuraEffect::HandleBreakableCCAuraProc)
// instead, so every bot read a fear as unbreakable and hit straight through it.
// With Centurion.BreakableCC.FearOthersBreakInstantly on, the first point of
// anybody else's damage ends the fear outright, so to every bot but its caster
// a fear is as breakable as a Polymorph. The caster only rolls against
// FearDamageAtCap and keeps fighting it the way it always has. Pets follow the
// same rule in PetAI (FindFearOthersBreak), where a pet counts as its owner.
bool PvpCore::HasBreakableCrowdControlFor(Player const* observer, Unit const* target)
{
    if (!target)
        return false;

    if (target->HasBreakableByDamageCrowdControlAura())
        return true;

    if (!observer || !sWorld->getBoolConfig(CONFIG_CENTURION_FEAR_OTHERS_BREAK_INSTANTLY))
        return false;

    for (AuraEffect const* fear : target->GetAuraEffectsByType(SPELL_AURA_MOD_FEAR))
    {
        if (fear->GetCasterGUID() == observer->GetGUID())
            continue;

        // A fear with no damage-taken proc never reaches the break rule at all.
        SpellProcEntry const* procEntry = sSpellMgr->GetSpellProcEntry(fear->GetId());
        if (procEntry && (procEntry->ProcFlags & TAKEN_HIT_PROC_FLAG_MASK))
            return true;
    }

    return false;
}

bool PvpCore::IsDispelThrottleExempt(Player const* player, uint32 spellId)
{
    return IsDispelThrottleExemptCast(player, spellId);
}

bool PvpCore::UsesMeleeSpacing(Player const* player)
{
    return player && UsesMeleeSpacingProfile(player, DetectClassicClassProfile(player));
}

uint32 PvpCore::GetRogueStealthOpenerSpellId(Player const* player)
{
    return SelectRogueStealthOpenerSpellId(player);
}

bool PvpCore::IsMovementPreventedByRoot(Player const* player)
{
    return player &&
        (player->HasUnitState(UNIT_STATE_ROOT) ||
            player->HasAuraType(SPELL_AURA_MOD_ROOT) ||
            player->HasAuraWithMechanic(1u << MECHANIC_ROOT));
}

PvpValues PvpCore::CollectValues(Player const* player)
{
    PvpValues values;
    if (!player)
        return values;

        values.inBattleground = player->InBattleground();
        values.inBattlegroundQueue = IsInBattlegroundQueue(player);
        values.battlegroundState = DetectBattlegroundState(player, values.inBattlegroundQueue);

        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            BattlegroundQueueTypeId const bgQueueTypeId = player->GetBattlegroundQueueTypeId(i);
            if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            bool const isArenaQueue = BattlegroundMgr::BGArenaType(bgQueueTypeId) != 0;
            bool const isInvited = player->IsInvitedForBattlegroundQueueType(bgQueueTypeId);

            values.hasArenaQueue = values.hasArenaQueue || isArenaQueue;
            values.hasBattlegroundQueue = values.hasBattlegroundQueue || !isArenaQueue;
            values.hasArenaInvite = values.hasArenaInvite || (isArenaQueue && isInvited);
            values.hasBattlegroundInvite = values.hasBattlegroundInvite || (!isArenaQueue && isInvited);
        }

        values.hasArenaTeamInvite = player->GetArenaTeamIdInvited() != 0;

        if (values.inBattleground)
            values.battlegroundTypeId = player->GetBattlegroundTypeId();

    PopulateObjectiveStateTriggers(player, values);
    // Re-evaluate from live keeper/aura state as well. The values object can be
    // collected immediately before a flag click and then reused later in the
    // same update, so relying only on its original objective snapshot lets a
    // pre-pickup stealth/Feign decision survive after the bot becomes carrier.
    values.playerHasFlag = values.playerHasFlag || IsBattlegroundFlagCarrier(player);
    values.battlegroundTeamHumanCount = CountHumanPlayersOnBattlegroundTeam(player);
    values.battlegroundTeamHasHumans = values.battlegroundTeamHumanCount > 0;

        return values;
    }

    bool PvpCore::IsTriggerActive(PvpTrigger trigger, PvpValues const& values)
    {
        switch (trigger)
        {
        case PvpTrigger::InBattleground:
            return values.inBattleground;
        case PvpTrigger::BgQueueing:
            return values.battlegroundState == BattlegroundState::Queueing;
        case PvpTrigger::BgWaiting:
            return values.battlegroundState == BattlegroundState::WaitingToStart;
        case PvpTrigger::BgActive:
            return values.battlegroundState == BattlegroundState::Active;
        case PvpTrigger::BgInviteActive:
            return values.hasBattlegroundInvite;
        case PvpTrigger::InBattlegroundWithoutFlag:
            return values.inBattleground;
        case PvpTrigger::PlayerHasFlag:
            return values.playerHasFlag;
        case PvpTrigger::FlagPickupAvailable:
            return values.flagPickupAvailable;
        case PvpTrigger::EnemyFlagCarrierActive:
            return values.enemyFlagCarrierActive;
        case PvpTrigger::EnemyFlagCarrierNear:
            return values.enemyFlagCarrierNear;
        case PvpTrigger::TeamFlagCarrierNear:
            return values.teamFlagCarrierNear;
        case PvpTrigger::NodeAssaultAvailable:
            return values.nodeAssaultAvailable;
        case PvpTrigger::NodeDefenseAvailable:
            return values.nodeDefenseAvailable;
        default:
            break;
        }

        return false;
    }

    BattlegroundTacticalContext PvpCore::BuildBattlegroundTacticalContext(Player const* player, PvpValues const& values)
    {
        BattlegroundTacticalContext context;
        context.tacticsEnabled = g_PvpCoreConfig.moduleEnabled && g_PvpCoreConfig.pvpCoreEnabled && g_PvpCoreConfig.pvpTacticsEnabled;
        if (!context.tacticsEnabled || !player)
            return context;

        if (!IsTriggerActive(PvpTrigger::BgWaiting, values) && !IsTriggerActive(PvpTrigger::BgActive, values))
            return context;

    context.shouldEvaluate = true;
    TacticalDecision const decision = SelectBattlegroundTacticalDecision(player, values);
    context.triggerName = decision.triggerName;
    context.actionName = decision.actionName;
    context.actionPriority = decision.priority;
    context.objective = SelectObjectiveSkeleton(values);
    context.movement = SelectMovementPrimitiveSkeleton(values, context.objective);
    context.flagCarrierDirective = SelectFlagCarrierDirectiveSkeleton(values);
    context.nearbyEnemyActive = values.nearbyEnemyActive;
    context.ctfRole = values.ctfRole;
    TC_LOG_DEBUG("playerbots.pvp.lifecycle",
        "Playerbot PvP human-first context: guid={} human_count={} has_humans={} player_has_flag={} flag_pickup_available={} nearby_enemy={} directive={} action={}.",
        player->GetGUID().ToString(), values.battlegroundTeamHumanCount, values.battlegroundTeamHasHumans, values.playerHasFlag,
        values.flagPickupAvailable, values.nearbyEnemyActive, static_cast<uint8>(context.flagCarrierDirective),
        context.actionName ? context.actionName : "none");
    return context;
}

    BattlegroundLifecycleContext PvpCore::BuildBattlegroundLifecycleContext(Player const* player, PvpValues const& values)
    {
        BattlegroundLifecycleContext context;
        context.lifecycleEnabled = IsLifecycleEnabled();
        if (!context.lifecycleEnabled || !player)
            return context;

        context.queueOperation = SelectBattlegroundQueueOperationSkeleton(values);
        context.invitationResponse = SelectBattlegroundInvitationResponseSkeleton(values);
        context.shouldHandleInProgressStatus = ShouldHandleBattlegroundInProgressStatusSkeleton(values);
        return context;
    }

    ArenaLifecycleContext PvpCore::BuildArenaLifecycleContext(Player const* player, PvpValues const& values)
    {
        ArenaLifecycleContext context;
        context.lifecycleEnabled = IsLifecycleEnabled();
        if (!context.lifecycleEnabled || !player)
            return context;

        context.queueOperation = SelectArenaQueueOperationSkeleton(values);
        context.teamInteraction = SelectArenaTeamInteractionSkeleton(values);
        return context;
    }

    PvpClassSpellContext PvpCore::BuildClassSpellContext(Player const* player, PvpValues const& values)
    {
        PvpClassSpellContext context;
        context.classSpellsEnabled = IsClassSpellGateEnabled(g_PvpCoreConfig);
        if (!context.classSpellsEnabled || !player)
            return context;

    // A bot in a vehicle seat fights with the vehicle - its objective duty
    // drives it - and its own spells and movement mean nothing in a seat.
    if (player->GetVehicle())
        return context;

    bool const inActiveBattleground = values.inBattleground && IsTriggerActive(PvpTrigger::BgActive, values);
    // A carrier always owns its capture route. A midfield bot owns its live
    // pickup route only while local combat pressure is clear; nearby enemies
    // deliberately hand movement back to combat. Without this shared decision,
    // class range/facing and tactical navmesh movement replace each other on
    // alternating cadences and produce the visible run/turn/stop loop.
    //
    // Warsong Gulch and Twin Peaks give flag orders instead (ctfRole set). A
    // bot sent for a flag - the runner, a bot standing near one, a bot
    // returning ours - was sent for it BECAUSE it matters more than the fight
    // around it, so it keeps its route and fires what it can on the move.
    //
    // A bot fetching or carrying a siege charge, or on its way to the siege
    // vehicle it is to drive, was sent the same way: the gate it is to bring
    // down matters more than the defenders it passes, who otherwise pull it
    // into every fight on the way and it never gets there.
    bool const siegeDutyOwnsRoute =
        values.objectiveDuty == uint8(playerbot::ObjectiveDuty::CarryCharge) ||
        values.objectiveDuty == uint8(playerbot::ObjectiveDuty::DriveSiege);
    context.preserveFlagObjectiveMovement = inActiveBattleground && (siegeDutyOwnsRoute ||
        (values.ctfRole != 0
            ? (values.playerHasFlag || values.flagPickupAvailable)
            : (values.playerHasFlag || values.flagPickupNearby ||
                (values.flagPickupAvailable && !values.nearbyEnemyActive))));
    context.preserveFlagCarrierMovement = inActiveBattleground && values.playerHasFlag;
    // A carrier waiting in its own flag room for our flag to come home still
    // must not wander off, but it is not running either: it may stop and cast.
    context.flagCarrierHolding = context.preserveFlagCarrierMovement && values.ctfCarrierHolding;
    bool const inBattlegroundPreparation = player->InBattleground() &&
        (player->HasAura(SPELL_PREPARATION) || player->HasAura(SPELL_ARENA_PREPARATION) || player->HasUnitFlag(UNIT_FLAG_PREPARATION));
    bool const inActiveDuel = player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS;
    // Open-world creature combat driven by the PvE manager. Battleground and
    // duel handling always win: engagement is only honored outside instanced
    // PvP so a stale flag can never alter battleground behavior.
    bool const inPveEngagement = !player->InBattleground() && !inActiveDuel && IsPveCombatEngaged(player);
    if (!inActiveBattleground && !inBattlegroundPreparation && !inActiveDuel && !inPveEngagement)
        return context;

    if (inBattlegroundPreparation)
    {
        SpellDecision const prepDecision = SelectPreparationBuffSpell(player);
        if (prepDecision.spellId)
        {
            context.actionName = prepDecision.actionName;
            context.reason = prepDecision.reason;
            context.spellId = prepDecision.spellId;
            context.targetMode = prepDecision.targetMode;
            context.targetGuid = prepDecision.targetGuid;
            context.selfCast = context.targetMode == PvpClassSpellContext::TargetMode::Self;
            context.shouldExecute = true;
            return context;
        }

        // Preparation is a closed-gates support phase. Once consumables,
        // buffs, pets, and mounting are complete, do not fall through into
        // the ordinary enemy/pet combat graph before the match starts.
        return context;
    }

    // A flag run does not stop to buff the team: a druid would leave Travel
    // Form for every Mark of the Wild and shift straight back.
    bool const onFlagRun = values.playerHasFlag ||
        (values.ctfDesignatedRunner && values.flagPickupAvailable && !values.ctfPickupIsReturn);
    if (!player->IsInCombat() && !inPveEngagement && !onFlagRun)
    {
        SpellDecision const raidBuffDecision = SelectMissingBattlegroundRaidBuff(player);
        if (raidBuffDecision.spellId)
        {
            context.actionName = raidBuffDecision.actionName;
            context.reason = raidBuffDecision.reason;
            context.spellId = raidBuffDecision.spellId;
            context.targetMode = raidBuffDecision.targetMode;
            context.targetGuid = raidBuffDecision.targetGuid;
            context.selfCast = context.targetMode == PvpClassSpellContext::TargetMode::Self;
            context.shouldExecute = true;
            return context;
        }
    }

    ClassicProfileSelection const profileSelection = DetectClassicClassProfile(player);
    Unit const* combatVictim = player->GetVictim();
    bool const hasEffectivelyImmuneCombatVictim = combatVictim && HasHostileTarget(player, combatVictim) &&
        IsTargetEffectivelyImmune(player, combatVictim);
    ObjectGuid const selectedTargetGuid = SelectCombatTargetGuid(player);
    ObjectGuid activeTargetGuid = selectedTargetGuid;
    if (activeTargetGuid.IsEmpty())
        if (HasHostileTarget(player, combatVictim) && !hasEffectivelyImmuneCombatVictim)
            activeTargetGuid = combatVictim->GetGUID();
    // In PvE engagement the manager owns target acquisition through the bot's
    // selected target; the fallback below scans the map player list and would
    // make an open-world bot acquire hostile-faction players on its own.
    if (activeTargetGuid.IsEmpty() && !inPveEngagement)
    {
        bool const allowLongAcquire =
            UsesRangedSpacingProfile(player, profileSelection) ||
            CanUseHealRangeSpacing(player->GetClass());
        if (Unit const* fallbackTarget = SelectClosestEnemyTarget(player, !allowLongAcquire))
            activeTargetGuid = fallbackTarget->GetGUID();
    }

        auto resolveTargetByGuid = [&](ObjectGuid const& guid) -> Unit const*
        {
            if (guid.IsEmpty() || guid == player->GetGUID())
                return nullptr;

            Unit const* resolved = ObjectAccessor::GetUnit(*player, guid);
            if (!resolved || !resolved->IsAlive())
                return nullptr;

            return resolved;
        };
        bool const hasValidTarget = resolveTargetByGuid(activeTargetGuid) != nullptr;
        Unit const* selectedTargetByGuid = resolveTargetByGuid(activeTargetGuid);
        bool const hasInvalidSelectedTarget = !selectedTargetGuid.IsEmpty() &&
            (!selectedTargetByGuid || !HasHostileTarget(player, selectedTargetByGuid));
        ObjectGuid const selectedAllyGuid = SelectAllyTargetGuid(player);
        bool const hasValidAllyTarget = resolveTargetByGuid(selectedAllyGuid) != nullptr;

        // Gating the shift is not enough on its own: a druid shifts out on the
        // field and then runs the flag home through its own keep, where the core
        // will not strip an outdoors-only aura it has been told is under open
        // sky. Take the form off on arrival, the way a mount comes off indoors.
        // No linger here - an area id changes once at the door, it does not
        // flicker step to step the way a terrain query does. Both tests read a
        // cached field, so this costs an unshifted bot nothing.
        if (player->GetShapeshiftForm() == FORM_TRAVEL && IsInsideWarsongKeep(player))
        {
            context.movementDirective = PvpClassSpellContext::MovementDirective::LeaveShapeshiftForm;
            context.actionName = "leave travel form";
            context.reason = "travel form inside the keep";
            context.shouldExecute = true;
            return context;
        }

        // Reference parity guard: never allow mounted state indoors. In addition,
        // while in combat always force mount-state correction immediately. When a
        // mounted bot is simply traveling, do not let class spell selection break
        // the mount unless an enemy has actually entered the combat envelope.
        // IsEffectivelyOutdoors runs a full terrain query - grid fetch, VMAP
        // area/liquid lookup, dynamic tree, WMOAreaTable and AreaTable - and its
        // result has exactly one reader, on the next line. Folding it into the
        // && lets an unmounted bot skip the query altogether instead of paying
        // for an answer it discards.
        bool const sustainedIndoorMounted = player->IsMounted() && ShouldForceIndoorDismount(player, IsEffectivelyOutdoors(player));
        if (player->IsMounted())
        {
            if (sustainedIndoorMounted || player->IsInCombat())
            {
                context.movementDirective = PvpClassSpellContext::MovementDirective::CheckMountState;
                context.actionName = "check mount state";
                context.reason = player->IsInCombat() ? "mounted in combat" : "mounted indoors";
                context.shouldExecute = true;
                return context;
            }

            if (hasInvalidSelectedTarget)
            {
                context.movementDirective = PvpClassSpellContext::MovementDirective::DropInvalidTarget;
                context.actionName = "drop target";
                context.reason = "invalid target";
                context.shouldExecute = true;
                context.movementTargetGuid = selectedTargetGuid;
                return context;
            }

            // PvE engagement has no enemy players to probe for; the resolved
            // creature target decides whether a traveling mount should break.
            if (inPveEngagement ? !hasValidTarget : !HasNearbyAttackableEnemyPlayer(player, GetConfiguredCombatRange()))
                return context;
        }

    // Wisp Form and the Elune's Grace fade/invisibility sequence are escape
    // windows, not opportunities to continue the normal spell rotation. The
    // tactical movement layer owns the retreat while these auras are active;
    // yielding here prevents class facing/range directives or a fresh cast
    // from replacing that movement and immediately breaking invisibility.
    if (player->HasAura(kPriestElunesGraceSpellId) || player->HasInvisibilityAura() ||
        (player->GetClass() == CLASS_PRIEST && player->HasAura(kPriestWispFormSpellId)))
        return context;

    // The flag run's own moves - forms, Blink, aspects, slows on the chasers -
    // come before the rotation. They are picked and checked by the flag-run
    // selectors, so they bypass the castability retry loop below, which would
    // refuse every druid form change.
    if (inActiveBattleground && values.ctfRole != 0)
    {
        // Before the run's own moves, because the run is what empties the bar:
        // a clear route is the only chance the bot gets to put mana back, and
        // the shift it would pick on this tick is part of what drained it.
        //
        // The outbound leg only, plus a carrier parked in its own flag room
        // waiting for its flag to come back. A carrier actually running one home
        // has somewhere to be and does not stop for a drink.
        bool const flagRouteAllowsRecovery =
            (values.ctfDesignatedRunner && values.flagPickupAvailable && !values.ctfPickupIsReturn && !values.playerHasFlag) ||
            values.ctfCarrierHolding;
        if (flagRouteAllowsRecovery)
        {
            bool const recovering = IsRecoveringOnFlagRun(player);
            if (recovering && !CanContinueFlagRunRecovery(player))
            {
                // Hit, or both bars back. Drop the auras here rather than leave
                // them holding the bot still, and let the run resume this tick.
                ClearFlagRunRecovery(player);
            }
            else if (recovering || WantsFlagRunRecovery(player))
            {
                // Nothing is eaten or drunk in a form, so the form comes off
                // first - the same order a player has to do it in.
                if (player->GetShapeshiftForm() != FORM_NONE)
                {
                    context.movementDirective = PvpClassSpellContext::MovementDirective::LeaveShapeshiftForm;
                    context.actionName = "leave form";
                    context.reason = "sit down to recover on a clear flag route";
                    context.shouldExecute = true;
                    return context;
                }

                if (SpellDecision const recoveryDecision = SelectFlagRunRecoverySpell(player); recoveryDecision.spellId)
                {
                    context.actionName = recoveryDecision.actionName;
                    context.reason = recoveryDecision.reason;
                    context.spellId = recoveryDecision.spellId;
                    context.targetMode = recoveryDecision.targetMode;
                    context.targetGuid = recoveryDecision.targetGuid;
                    context.selfCast = true;
                    context.shouldExecute = true;
                    return context;
                }

                // Sitting through one already. Hold the tick here: the run's own
                // selector would shift the bot straight back into Travel Form
                // and pull it up off the food it just sat down to.
                return context;
            }
        }

        SpellDecision const flagRunDecision = SelectBattlegroundFlagRunSpell(player, values);
        if (flagRunDecision.spellId)
        {
            context.actionName = flagRunDecision.actionName;
            context.reason = flagRunDecision.reason;
            context.spellId = flagRunDecision.spellId;
            context.targetMode = flagRunDecision.targetMode;
            context.targetGuid = flagRunDecision.targetGuid;
            context.selfCast = context.targetMode == PvpClassSpellContext::TargetMode::Self;
            context.flagManeuver = true;
            context.shouldExecute = true;
            return context;
        }

        // A druid in its running form stays in it. Every rotation pick from
        // Travel, Aquatic or a carrier's Cat Form - Moonkin Form, Cat Form for
        // a target, a Rejuvenation on a teammate - cancels the form first, and
        // the run only shifts back a global cooldown and a spacing token later,
        // so the druid would spend most of its run on foot paying for two forms.
        // Only a healthstone or potion still goes down.
        if (IsDruidInFlagRunForm(player, values))
        {
            if (player->IsInCombat() && player->HealthBelowPct(50))
            {
                uint32 itemEntry = SelectReadyHealthstoneItemEntry(player);
                if (!itemEntry)
                    itemEntry = SelectReadyRestorePotionItemEntry(player, false);

                if (itemEntry)
                {
                    context.actionName = "use healing consumable";
                    context.reason = "flag run heals without leaving its form";
                    context.itemEntry = itemEntry;
                    context.targetMode = PvpClassSpellContext::TargetMode::Self;
                    context.targetGuid = player->GetGUID();
                    context.selfCast = true;
                    context.shouldExecute = true;
                }
            }

            return context;
        }
    }

    // A bot still eating or drinking when its team comes under attack gets up
    // (EvaluateTeamDanger), so this decision is a heal or an attack. The
    // tactical tick does the same ahead of this; this covers any path past it.
    if ((player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT) || player->HasAura(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK)) &&
        EvaluateTeamDanger(player, false).inDanger)
    {
        if (Player* mutablePlayer = const_cast<Player*>(player))
        {
            mutablePlayer->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_EAT);
            mutablePlayer->RemoveAurasDueToSpell(SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK);
            if (mutablePlayer->IsSitState())
                mutablePlayer->SetStandState(UNIT_STAND_STATE_STAND);
        }
    }

    bool const inSpiritOfRedemption = IsPriestInSpiritOfRedemption(player);
    bool const movementPreventedByRoot = PvpCore::IsMovementPreventedByRoot(player);
    bool const criticalLowMana = !inSpiritOfRedemption &&
        !UsesMeleeSpacingProfile(player, profileSelection) &&
        player->GetClass() != CLASS_HUNTER &&
        player->GetClass() != CLASS_WARLOCK &&
        player->GetMaxPower(POWER_MANA) > 0 && player->GetPowerPct(POWER_MANA) < 10.0f;

    // Hard-priority mana preservation policy for ranged/healing profiles: below
    // 10% mana, disengage above all other class behavior, then drink as soon as
    // combat drops. Melee mana users keep pressure with attacks instead of
    // oscillating between chase and retreat as their mana crosses the threshold.
    // Spirit of Redemption is exempt because its healing casts are free and the
    // aura duration is too short to spend on drinking or disengaging.
    // PvE combat is a commitment: no kiting, no low-mana disengage, no
    // too-close retreats. A fleeing bot never finishes a creature fight -
    // the mob just chases - so outside PvP the bot stands and fights until
    // one side dies; mana troughs fall through to wand/melee fallbacks.
    if (criticalLowMana && !movementPreventedByRoot && !inPveEngagement)
    {
        if (player->IsInCombat())
        {
            Unit const* disengageTarget = resolveTargetByGuid(activeTargetGuid);
            if (!disengageTarget)
                disengageTarget = resolveTargetByGuid(selectedTargetGuid);
            if (!disengageTarget && player->GetVictim() && player->GetVictim()->IsAlive())
                disengageTarget = player->GetVictim();
            // Nobody targeted, but somebody may still be shooting at it. Run
            // from them rather than fall through to the reset below, which is
            // only for a combat flag with no fight behind it.
            if (!disengageTarget)
                disengageTarget = FindEngagedPvpOpponent(player);

                if (disengageTarget)
                {
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, disengageTarget->GetGUID(),
                        std::max(GetConfiguredLongRange(), GetConfiguredCloseRange() + 8.0f),
                        "flee", "critical low mana disengage", 99.0f);
                    return context;
                }

                context.movementDirective = PvpClassSpellContext::MovementDirective::ResetCombatState;
                context.actionName = "reset";
                context.reason = "critical low mana force combat reset";
                context.shouldExecute = true;
                return context;
            }

            // Not while the team is in a fight (EvaluateTeamDanger): a dry
            // healer standing is still a target to peel and a body in the way,
            // and the wand and melee fallbacks below still have something to do.
            if (IsSpellReady(player, SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK) && !EvaluateTeamDanger(player, false).inDanger)
            {
                context.actionName = "drink";
                context.reason = "critical low mana recover";
                context.spellId = SPELL_PLAYERBOT_OUT_OF_COMBAT_DRINK;
                context.targetMode = PvpClassSpellContext::TargetMode::Self;
                context.targetGuid = player->GetGUID();
                context.selfCast = true;
                context.shouldExecute = true;
                return context;
            }
        }

    if (player->IsInCombat() && !hasValidTarget && !hasValidAllyTarget)
    {
        if (IncrementCombatNoTargetTicks(player) >= 3)
        {
            context.movementDirective = PvpClassSpellContext::MovementDirective::ResetCombatState;
            context.actionName = "reset";
            context.reason = "combat stuck";
            context.shouldExecute = true;
            ResetCombatNoTargetTicks(player);
            return context;
        }
    }
    else
    {
        ResetCombatNoTargetTicks(player);
    }

    if (hasInvalidSelectedTarget)
    {
        context.movementDirective = PvpClassSpellContext::MovementDirective::DropInvalidTarget;
        context.actionName = "drop target";
        context.reason = "invalid target";
        context.shouldExecute = true;
        context.movementTargetGuid = selectedTargetGuid;
        return context;
    }

        if (player->GetClass() == CLASS_HUNTER)
        {
            // Do not emit hunter flee directives from the spell selector. Hunter
            // movement is timer-driven in PlayerbotPvpLifecycleActions: run while
            // Auto Shot is charging, plant briefly to fire, then resume running.
            // Returning FleeTooCloseForSpell here created a second movement owner
            // that fought the lifecycle loop and produced triangle/orbit movement.
        }

        if (player->GetClass() == CLASS_HUNTER)
        {
            TC_LOG_DEBUG("playerbots.pvp.classspell",
                "BuildClassSpellContext snapshot: botGuid={} inBg={} bgActive={} inPrep={} inDuel={} hasValidTarget={} targetGuid={} allyGuid={}.",
                player->GetGUID().ToString(), values.inBattleground ? 1 : 0, IsTriggerActive(PvpTrigger::BgActive, values) ? 1 : 0,
                inBattlegroundPreparation ? 1 : 0, inActiveDuel ? 1 : 0, hasValidTarget ? 1 : 0,
                hasValidTarget ? selectedTargetGuid.ToString() : "none", hasValidAllyTarget ? selectedAllyGuid.ToString() : "none");
        }

        SpellDecision decision;
        SpellDecision firstDecision;
        std::vector<uint32> suppressedSpellIds;
        uint32 attempts = 0;
        constexpr uint32 kMaxDecisionAttempts = 8;
        suppressedSpellIds.reserve(kMaxDecisionAttempts);
        while (attempts++ < kMaxDecisionAttempts)
        {
            DecisionEvaluationScope decisionScope(player, &suppressedSpellIds);
            Unit const* decisionTarget = resolveTargetByGuid(activeTargetGuid);
            Unit const* decisionAllyTarget = resolveTargetByGuid(selectedAllyGuid);
            SpellDecision const candidate = SelectClassOrUtilitySpell(player, decisionTarget, decisionAllyTarget, profileSelection);
            if (!candidate.spellId && !candidate.itemEntry)
                break;

            if (!firstDecision.spellId && !firstDecision.itemEntry)
                firstDecision = candidate;

            Unit const* immediateCastTarget = resolveTargetByGuid(activeTargetGuid);
            Unit const* immediateCastAllyTarget = resolveTargetByGuid(selectedAllyGuid);
            if (IsDecisionImmediatelyCastable(player, candidate, immediateCastTarget, immediateCastAllyTarget))
            {
                decision = candidate;
                if (!suppressedSpellIds.empty())
                {
                    TC_LOG_DEBUG("playerbots.pvp.classspell",
                        "Class spell fallback chain selected castable spell: botGuid={} fallbackSpell={} refused={} attempts={} targetGuid={} allyGuid={}.",
                        player->GetGUID().ToString(), candidate.spellId, suppressedSpellIds.size(), attempts,
                        hasValidTarget ? selectedTargetGuid.ToString() : "none", hasValidAllyTarget ? selectedAllyGuid.ToString() : "none");
                }
                break;
            }

            if (!candidate.spellId)
                break;

            suppressedSpellIds.push_back(candidate.spellId);
        }

        // If no immediately castable spell or item was found, keep the first decision so execution
        // can still drive movement/position correction (for example out-of-range follow).
        if (!decision.spellId && !decision.itemEntry)
            decision = firstDecision;

        context.actionName = decision.actionName;
        context.reason = decision.reason;
        context.spellId = decision.spellId;
        if (context.spellId)
        {
            if (SpellInfo const* baseSpellInfo = sSpellMgr->GetSpellInfo(context.spellId))
            {
                uint32 resolvedSpellId = 0;
                for (uint32 chainSpellId = baseSpellInfo->GetFirstRankSpell()->Id; chainSpellId != 0; chainSpellId = sSpellMgr->GetNextSpellInChain(chainSpellId))
                {
                    if (player->HasSpell(chainSpellId))
                        resolvedSpellId = chainSpellId;
                }

                if (resolvedSpellId)
                    context.spellId = resolvedSpellId;
            }
        }
        context.targetMode = decision.targetMode;
        context.selfCast = context.targetMode == PvpClassSpellContext::TargetMode::Self;
        context.itemEntry = decision.itemEntry;
        context.targetGuid = hasValidTarget ? activeTargetGuid : ObjectGuid::Empty;
        context.allyTargetGuid = hasValidAllyTarget ? selectedAllyGuid : ObjectGuid::Empty;
        if (!decision.targetGuid.IsEmpty())
            context.targetGuid = decision.targetGuid;
        if (context.targetMode == PvpClassSpellContext::TargetMode::Ally && context.targetGuid.IsEmpty())
            context.targetGuid = context.allyTargetGuid;
        else if (context.targetMode == PvpClassSpellContext::TargetMode::Self)
            context.targetGuid = player->GetGUID();

    // The selector deliberately keeps its highest-priority pick when nothing was
    // castable, so execution can still drive movement and range correction. That
    // is right, except when the reason nothing was castable is that the target is
    // immune to the bot's entire kit: an elemental shaman's every offensive
    // candidate is Nature, so against a nature-immune elemental it would answer
    // the refusal by casting the MORE expensive Chain Lightning at it.
    //
    // Clearing the spell hands the bot to the existing "no spell" cascade below,
    // which closes the distance and swings - and white damage has no school to be
    // immune to. No movement directive is installed here on purpose: setting one
    // would out-prioritise the healer's reach-an-ally arm and drag a healer toward
    // the enemy it cannot hurt instead of the ally it can heal.
    if (context.spellId && context.targetMode == PvpClassSpellContext::TargetMode::Enemy)
    {
        if (Unit const* immunityTarget = resolveTargetByGuid(context.targetGuid))
            if (SpellInfo const* committedSpellInfo = sSpellMgr->GetSpellInfo(context.spellId))
                if (PvpClassActions::IsCastWastedOnTargetImmunity(player, immunityTarget, committedSpellInfo))
                {
                    TC_LOG_DEBUG("playerbots.pvp.classspell",
                        "Immunity veto: botGuid={} spell={} target={} action={}.",
                        player->GetGUID().ToString(), context.spellId,
                        context.targetGuid.ToString(), context.actionName ? context.actionName : "-");
                    context.spellId = 0;
                    context.itemEntry = 0;
                    context.targetMode = PvpClassSpellContext::TargetMode::None;
                    context.targetGuid = ObjectGuid::Empty;
                    context.selfCast = false;
                }
    }

    if (context.spellId &&
        (context.targetMode == PvpClassSpellContext::TargetMode::Enemy || context.targetMode == PvpClassSpellContext::TargetMode::Ally))
    {
        Unit const* losRecoveryTarget = resolveTargetByGuid(context.targetGuid);
        if (losRecoveryTarget && !player->IsWithinLOSInMap(losRecoveryTarget) && !movementPreventedByRoot)
        {
            SpellInfo const* recoverySpellInfo = sSpellMgr->GetSpellInfo(context.spellId);
            float const spellMaxRange = recoverySpellInfo ? player->GetSpellMaxRangeForTarget(losRecoveryTarget, recoverySpellInfo) : 0.0f;
            float const currentDistance = player->GetDistance(losRecoveryTarget);
            float const maxFollowRange = spellMaxRange > 0.0f
                ? std::max(1.5f, spellMaxRange - 1.0f)
                : std::max(1.5f, GetConfiguredSpellRange() - 1.0f);
            float const desiredRange = std::clamp(currentDistance - 2.0f, 1.5f, maxFollowRange);

                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, losRecoveryTarget->GetGUID(),
                    desiredRange, "recover los", "selected spell target out of line of sight", 86.0f);
                context.spellId = 0;
                context.itemEntry = 0;
                context.targetMode = PvpClassSpellContext::TargetMode::None;
                context.targetGuid = ObjectGuid::Empty;
                context.selfCast = false;
            }
        }

    // A selected spell owns its facing at execution time. Cast-time spells
    // stop and face before casting; instant/move-allowed spells update facing
    // without replacing their active retreat spline. Converting the spell to
    // a separate FaceSpellTarget directive here made casting wait for movement
    // to finish and caused move/face spline oscillation.

    if (context.spellId && !movementPreventedByRoot &&
        context.targetMode == PvpClassSpellContext::TargetMode::Enemy && UsesMeleeSpacingProfile(player, profileSelection))
    {
        Unit const* meleeTarget = resolveTargetByGuid(context.targetGuid);
        // This check zeroes context.spellId and substitutes a plain walk into
        // melee, so anything it wrongly captures is silently never cast.
        //
        // It used to be a hardcoded list of gap-closer ids (Charge, Intercept,
        // Heroic Leap, Feral Charge, Rehgar's Fury, Hand of Reckoning, Frost
        // Shock). Any other ranged ability a melee bot selected fell through it
        // and was replaced by a walk - the gnome grenade (89160, 45 yards) being
        // the case that surfaced it: repeatedly selected at range, zeroed every
        // tick, never cast so never put on cooldown, and therefore re-selected
        // forever, which starved the warrior of its whole rotation. It only ever
        // landed at point blank, once the walk had finished.
        //
        // Ask the spell what its range is instead of maintaining a list. Melee
        // abilities have melee-length ranges and still correctly trigger the
        // walk; everything else is cast from where it can actually be cast.
        bool canCastOutsideMelee = false;
        if (meleeTarget)
        {
            if (SpellInfo const* pendingSpellInfo = sSpellMgr->GetSpellInfo(context.spellId))
            {
                float const pendingMaxRange = pendingSpellInfo->GetMaxRange(false);
                float const pendingMinRange = pendingSpellInfo->GetMinRange(false);
                float const targetDistance = player->GetDistance(meleeTarget);
                canCastOutsideMelee = pendingMaxRange > 0.0f && targetDistance <= pendingMaxRange &&
                    (pendingMinRange <= 0.0f || targetDistance >= pendingMinRange);
            }
        }

        if (meleeTarget && !player->IsWithinMeleeRange(meleeTarget) && !canCastOutsideMelee)
        {
            ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachMeleeRange, meleeTarget->GetGUID(),
                std::max(1.0f, GetConfiguredMeleeRange() - 1.0f), "reach melee", "enemy out of melee", 85.0f);
            context.spellId = 0;
            context.itemEntry = 0;
            context.targetMode = PvpClassSpellContext::TargetMode::None;
            context.targetGuid = ObjectGuid::Empty;
            context.selfCast = false;
        }
    }

        if (!context.spellId && hasValidTarget)
        {
            Unit const* facingFallbackTarget = resolveTargetByGuid(activeTargetGuid);
            if (facingFallbackTarget && !player->HasInArc(static_cast<float>(M_PI), facingFallbackTarget))
            {
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FaceSpellTarget, facingFallbackTarget->GetGUID(), 0.0f,
                    "set facing", "not facing target", 72.0f);
            }
        }

    // If the selected spell is not immediately castable due spacing, switch this
    // tick into movement-directive execution to mirror reference trigger flow.
    if (context.spellId && !movementPreventedByRoot && UsesRangedSpacingProfile(player, profileSelection))
    {
        Unit const* spacingTarget = nullptr;
        if (context.targetMode == PvpClassSpellContext::TargetMode::Enemy ||
            context.targetMode == PvpClassSpellContext::TargetMode::Ally)
        {
            spacingTarget = resolveTargetByGuid(context.targetGuid);
        }

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(context.spellId);
            if (spellInfo && spacingTarget)
            {
                float const distance = player->GetDistance(spacingTarget);
                float const maxRange = spellInfo->GetMaxRange(false);
                float const minRange = spellInfo->GetMinRange(false);
                if (maxRange > 0.0f && distance > maxRange)
                {
                    if (!(player->GetClass() == CLASS_HUNTER && context.targetMode == PvpClassSpellContext::TargetMode::Enemy &&
                        IsHunterWithinAutoShotBand(player, spacingTarget)))
                    {
                        ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, spacingTarget->GetGUID(),
                            ComputeApproachFollowRange(maxRange), "reach spell", "selected spell out of range", 84.0f);
                    }
                    context.spellId = 0;
                    context.itemEntry = 0;
                    context.targetMode = PvpClassSpellContext::TargetMode::None;
                    context.targetGuid = ObjectGuid::Empty;
                    context.selfCast = false;
                }
                else if (maxRange > 0.0f &&
                    spellInfo->CalcCastTime() > 0 &&
                    distance > std::max(1.0f, maxRange - 2.0f))
                {
                    // Hard-cast edge guard: repeated casts at the absolute spell
                    // ceiling can stutter on movement jitter and LOS drift. Step
                    // in slightly so caster bots do not idle at max-range fringe.
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, spacingTarget->GetGUID(),
                        ComputeApproachFollowRange(maxRange), "reach spell", "selected spell near max range edge", 83.0f);
                    context.spellId = 0;
                    context.itemEntry = 0;
                    context.targetMode = PvpClassSpellContext::TargetMode::None;
                    context.targetGuid = ObjectGuid::Empty;
                    context.selfCast = false;
                }
                else if (minRange > 0.0f && distance < std::max(0.0f, minRange + kRangedSpacingEnterTooCloseBuffer))
                {
                    if (player->GetClass() != CLASS_HUNTER && !inPveEngagement)
                    {
                        // Enter too-close movement before strict dead-zone boundaries so
                        // ranged users do not idle in 5-8y style min-range gaps.
                        // Hunters are excluded because their movement is owned by the
                        // Auto Shot stutter loop in lifecycle; emitting spell-selector
                        // flee directives here creates competing movement vectors.
                        float const fleeFollowRange = std::max(std::max(1.0f, GetConfiguredCloseRange()), minRange + 2.0f);
                        ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, spacingTarget->GetGUID(),
                            fleeFollowRange, "flee", "selected spell minimum range violation", 84.0f);
                    }
                    context.spellId = 0;
                    context.itemEntry = 0;
                    context.targetMode = PvpClassSpellContext::TargetMode::None;
                    context.targetGuid = ObjectGuid::Empty;
                    context.selfCast = false;
                }
                else if (context.targetMode == PvpClassSpellContext::TargetMode::Enemy &&
                    distance > (GetConfiguredSpellRange() + kRangedSpacingEnterOutOfRangeBuffer) &&
                    !IsHunterWithinAutoShotBand(player, spacingTarget))
                {
                    // Some classic spell entries report atypical range metadata,
                    // which can leave ranged bots idling at ~35-40y while still
                    // selecting enemy casts. Keep a config-based engage floor so
                    // they always step in to practical casting distance. Hunters are
                    // the exception: if they are already in Auto Shot range, do not
                    // pull them back inward while kiting just because an instant shot
                    // would prefer a slightly shorter configured spell distance.
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, spacingTarget->GetGUID(),
                        ComputeApproachFollowRange(GetConfiguredSpellRange()), "reach spell", "enemy outside configured cast distance", 82.0f);
                    context.spellId = 0;
                    context.itemEntry = 0;
                    context.targetMode = PvpClassSpellContext::TargetMode::None;
                    context.targetGuid = ObjectGuid::Empty;
                    context.selfCast = false;
                }
            }
        }

        bool const healerHasLivingAllyTarget = hasValidAllyTarget && CanUseHealRangeSpacing(player->GetClass());

        if (!context.spellId && context.movementDirective == PvpClassSpellContext::MovementDirective::None &&
            healerHasLivingAllyTarget)
        {
            Unit const* allyMovementTarget = resolveTargetByGuid(selectedAllyGuid);
            if (allyMovementTarget)
            {
                float const distance = player->GetDistance(allyMovementTarget);
                if (distance > GetConfiguredHealRange())
                {
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, allyMovementTarget->GetGUID(),
                        ComputeApproachFollowRange(GetConfiguredHealRange()), "reach party member to heal", "party member to heal out of spell range", 72.0f);
                }
            }
        }

        // An arena healer with nothing to cast goes to a teammate in danger it
        // cannot heal from where it stands - behind a pillar, or too far
        // (EvaluateTeamDanger reads the whole map, line of sight or not; heal
        // targeting needs it). On arrival the heal selectors see the teammate.
        // Out of sight it closes in, as "recover los" does, since a teammate
        // within heal range behind a pillar is no nearer to being healed.
        if (!context.spellId && context.movementDirective == PvpClassSpellContext::MovementDirective::None &&
            player->InArena() && CanUseHealRangeSpacing(player->GetClass()) && !movementPreventedByRoot)
        {
            TeamDanger const danger = EvaluateTeamDanger(player, true);
            Unit const* endangeredAlly = danger.endangeredAlly.IsEmpty() ? nullptr : resolveTargetByGuid(danger.endangeredAlly);
            if (endangeredAlly)
            {
                float const distance = player->GetDistance(endangeredAlly);
                float const healReach = ComputeApproachFollowRange(GetConfiguredHealRange());
                if (!player->IsWithinLOSInMap(endangeredAlly))
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, endangeredAlly->GetGUID(),
                        std::clamp(distance - 2.0f, 1.5f, healReach), "reach endangered ally", "teammate in danger out of sight", 80.0f);
                else if (distance > GetConfiguredHealRange())
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, endangeredAlly->GetGUID(),
                        healReach, "reach endangered ally", "teammate in danger out of heal range", 80.0f);
            }
        }

        // Reference parity bridge: provide trigger-like movement directives even
        // when we do not have a castable spell yet ("enemy out of spell" / "enemy
        // too close for spell"). Keep classic spell IDs untouched.
        if (!context.spellId && hasValidTarget && player->GetClass() != CLASS_HUNTER &&
            UsesRangedSpacingProfile(player, profileSelection) && !healerHasLivingAllyTarget)
        {
            Unit const* movementTarget = resolveTargetByGuid(activeTargetGuid);
            if (movementTarget)
            {
                float const distance = player->GetDistance(movementTarget);
                if (distance > (GetConfiguredSpellRange() + kRangedSpacingEnterOutOfRangeBuffer) &&
                    !IsHunterWithinAutoShotBand(player, movementTarget))
                {
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachSpellRange, movementTarget->GetGUID(),
                        ComputeApproachFollowRange(GetConfiguredSpellRange()), "reach spell", "enemy out of spell range", 70.0f);
                }
                else if (IsHunterExactDeadZone(player, movementTarget))
                {
                    // No castable spell + hunter dead-zone often left the bot in a
                    // bow-raise idle loop. Force retreat out past ranged minimum
                    // instead of collapsing into melee.
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, movementTarget->GetGUID(),
                        ComputeHunterDeadZoneRetreatStep(player, movementTarget),
                        "hunter deadzone retreat", "enemy in hunter dead-zone", 100.0f);
                }
                else if (!inPveEngagement && distance < std::max(0.0f, GetConfiguredMeleeRange() + kRangedSpacingEnterTooCloseBuffer))
                {
                    ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, movementTarget->GetGUID(),
                        std::max(1.0f, GetConfiguredCloseRange()), "flee", "enemy too close for spell", 71.0f);
                }
            }
        }

        if (!context.spellId && context.movementDirective == PvpClassSpellContext::MovementDirective::None &&
            hasValidTarget && UsesMeleeSpacingProfile(player, profileSelection))
        {
            Unit const* meleeMovementTarget = resolveTargetByGuid(selectedTargetGuid);
            if (meleeMovementTarget && !player->IsWithinMeleeRange(meleeMovementTarget))
            {
                ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachMeleeRange, meleeMovementTarget->GetGUID(),
                    std::max(1.0f, GetConfiguredMeleeRange() - 1.0f), "reach melee", "enemy out of melee", 69.0f);
            }
        }

        if (!context.spellId && context.movementDirective == PvpClassSpellContext::MovementDirective::None &&
            hasValidTarget && IsLowOrOutOfManaForFallback(player))
        {
            Unit const* fallbackTarget = resolveTargetByGuid(selectedTargetGuid);
            if (fallbackTarget)
            {
                if (IsWandShootReadyForDecision(player) && !HasBreakableCrowdControl(player, fallbackTarget))
                {
                    context.actionName = "fallback wand";
                    context.reason = "low mana fallback to wand pressure";
                    context.spellId = kWandShootSpellId;
                    context.targetMode = PvpClassSpellContext::TargetMode::Enemy;
                    context.targetGuid = fallbackTarget->GetGUID();
                    context.selfCast = false;
                }
                else
                {
                    // PvE fights are a commitment: no low-mana disengage
                    // dancing against creatures (same rule as the other
                    // flee arms).
                    if (!inPveEngagement && UsesRangedSpacingProfile(player, profileSelection) && player->GetClass() != CLASS_HUNTER)
                    {
                        ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::FleeTooCloseForSpell, fallbackTarget->GetGUID(),
                            std::max(1.0f, GetConfiguredCloseRange()), "flee", "low mana fallback disengage to recover", 67.0f);
                    }
                    else if (player->GetClass() != CLASS_HUNTER)
                    {
                        ConsiderMovementDirective(context, PvpClassSpellContext::MovementDirective::ReachMeleeRange, fallbackTarget->GetGUID(),
                            std::max(1.0f, GetConfiguredMeleeRange() - 1.0f), "reach melee", "low mana fallback to auto-attack", 67.0f);
                    }
                }
            }
        }

        // PvP insignia/class-trinket crowd-control breaks and holy-priest Spirit
        // of Redemption are handled by per-player fast paths in
        // RandomBotParticipationManager::ProcessPlayerLifecycle. Keep them out of
        // the normal class-spell decision graph so they are not throttled by
        // decision cadence and do not consume the class action tick/GCD.

        context.shouldExecute = context.shouldExecute || context.spellId != 0 || context.itemEntry != 0;

        char const* targetModeLabel = "none";
        switch (context.targetMode)
        {
        case PvpClassSpellContext::TargetMode::Enemy:
            targetModeLabel = "enemy";
            break;
        case PvpClassSpellContext::TargetMode::Self:
            targetModeLabel = "self";
            break;
        case PvpClassSpellContext::TargetMode::Ally:
            targetModeLabel = "ally";
            break;
        case PvpClassSpellContext::TargetMode::Pet:
            targetModeLabel = "pet";
            break;
        case PvpClassSpellContext::TargetMode::None:
        default:
            break;
        }

        TC_LOG_DEBUG("playerbots.pvp.class",
            "Playerbot PvP class context: class={} profile={} fallback={} unsupported={} has_enemy_target={} enemy_target_guid={} ally_target_guid={} target_mode={} spell={} action={} reason={}.",
            GetClassLabel(player->GetClass()), profileSelection.profileLabel, profileSelection.usedFallback,
            profileSelection.unsupportedClass, hasValidTarget, hasValidTarget ? selectedTargetGuid.ToString() : ObjectGuid::Empty.ToString(),
            context.allyTargetGuid.ToString(), targetModeLabel, context.spellId, context.actionName ? context.actionName : "none",
            context.reason ? context.reason : "none");
        return context;
    }

    RandomBotParticipationHooks PvpCore::BuildRandomBotParticipationHooks(Player const* player, PvpValues const& values)
    {
        RandomBotParticipationHooks hooks;
        hooks.lifecycleEnabled = IsLifecycleEnabled();
        if (!hooks.lifecycleEnabled || !player)
            return hooks;

        BattlegroundLifecycleContext bgContext = BuildBattlegroundLifecycleContext(player, values);
        ArenaLifecycleContext arenaContext = BuildArenaLifecycleContext(player, values);

        hooks.battlegroundParticipationHook = (bgContext.queueOperation != QueueOperationType::None) ||
            (bgContext.invitationResponse != InvitationResponseType::None) || bgContext.shouldHandleInProgressStatus;
        hooks.arenaParticipationHook = (arenaContext.queueOperation != QueueOperationType::None) ||
            (arenaContext.teamInteraction != ArenaTeamInteractionType::None);

        return hooks;
    }

    bool PvpCore::IsLifecycleEnabled()
    {
        return IsLifecycleGateEnabled(g_PvpCoreConfig);
    }

    bool PvpCore::IsInBattlegroundQueue(Player const* player)
    {
        if (!player)
            return false;

        for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
            if (player->GetBattlegroundQueueTypeId(i) != BATTLEGROUND_QUEUE_NONE)
                return true;

        return false;
    }

    BattlegroundState PvpCore::DetectBattlegroundState(Player const* player, bool inQueue)
    {
        if (!player)
            return BattlegroundState::None;

        if (!player->InBattleground())
            return inQueue ? BattlegroundState::Queueing : BattlegroundState::None;

        if (Battleground const* battleground = player->GetBattleground())
        {
            if (battleground->GetStatus() == STATUS_WAIT_JOIN)
                return BattlegroundState::WaitingToStart;

            if (battleground->GetStatus() == STATUS_IN_PROGRESS)
                return BattlegroundState::Active;
        }

        return BattlegroundState::None;
    }

    BattlegroundObjectiveSelection PvpCore::SelectObjectiveSkeleton(PvpValues const& values)
    {
        BattlegroundObjectiveSelection objective;

    if (IsTriggerActive(PvpTrigger::PlayerHasFlag, values) || IsTriggerActive(PvpTrigger::FlagPickupAvailable, values))
        objective.type = BattlegroundObjectiveType::CaptureFlag;
    else if (IsTriggerActive(PvpTrigger::EnemyFlagCarrierActive, values))
        objective.type = BattlegroundObjectiveType::AttackFlagCarrier;
    else if (IsTriggerActive(PvpTrigger::TeamFlagCarrierNear, values))
        objective.type = BattlegroundObjectiveType::ProtectFlagCarrier;
    else if (IsTriggerActive(PvpTrigger::NodeAssaultAvailable, values))
    {
        objective.type = BattlegroundObjectiveType::AssaultNode;
        objective.objectiveId = values.nodeObjectiveId;
    }
    else if (IsTriggerActive(PvpTrigger::NodeDefenseAvailable, values))
    {
        objective.type = BattlegroundObjectiveType::DefendNode;
        objective.objectiveId = values.nodeObjectiveId;
    }

        return objective;
    }

    BattlegroundMovementPrimitive PvpCore::SelectMovementPrimitiveSkeleton(PvpValues const& values,
        BattlegroundObjectiveSelection const& objective)
    {
        if (!IsTriggerActive(PvpTrigger::BgActive, values))
            return BattlegroundMovementPrimitive::None;

        switch (objective.type)
        {
        case BattlegroundObjectiveType::AttackFlagCarrier:
        case BattlegroundObjectiveType::ProtectFlagCarrier:
            return BattlegroundMovementPrimitive::MoveToObjectiveUnit;
        case BattlegroundObjectiveType::AssaultNode:
        case BattlegroundObjectiveType::DefendNode:
        case BattlegroundObjectiveType::CaptureFlag:
            return BattlegroundMovementPrimitive::MoveToObjectivePosition;
        case BattlegroundObjectiveType::None:
        default:
            return BattlegroundMovementPrimitive::MoveToObjectivePosition;
        }
    }

FlagCarrierDirective PvpCore::SelectFlagCarrierDirectiveSkeleton(PvpValues const& values)
{
    if (IsTriggerActive(PvpTrigger::PlayerHasFlag, values))
        return FlagCarrierDirective::None;

    if (IsTriggerActive(PvpTrigger::EnemyFlagCarrierActive, values))
        return FlagCarrierDirective::AttackEnemyCarrier;

    if (IsTriggerActive(PvpTrigger::TeamFlagCarrierNear, values))
        return FlagCarrierDirective::ProtectTeamCarrier;

        return FlagCarrierDirective::None;
    }

    QueueOperationType PvpCore::SelectBattlegroundQueueOperationSkeleton(PvpValues const& values)
    {
        if (values.inBattleground)
            return QueueOperationType::None;

        if (values.hasBattlegroundQueue && values.hasArenaInvite)
            return QueueOperationType::Leave;

        if (!values.inBattlegroundQueue && !values.hasBattlegroundQueue && !values.hasBattlegroundInvite &&
            !values.hasArenaQueue && !values.hasArenaInvite)
            return QueueOperationType::Join;

        return QueueOperationType::None;
    }

    InvitationResponseType PvpCore::SelectBattlegroundInvitationResponseSkeleton(PvpValues const& values)
    {
        if (!values.hasBattlegroundInvite)
            return InvitationResponseType::None;

        if (values.inBattleground)
            return InvitationResponseType::Decline;

        return InvitationResponseType::Accept;
    }

    bool PvpCore::ShouldHandleBattlegroundInProgressStatusSkeleton(PvpValues const& values)
    {
        // Keep lifecycle handling active for bots that are still flagged in a battleground,
        // including STATUS_WAIT_LEAVE so they can execute LeaveBattleground() and return
        // to their recorded queue entry point.
        return values.inBattleground;
    }

    QueueOperationType PvpCore::SelectArenaQueueOperationSkeleton(PvpValues const& values)
    {
        if (values.inBattleground)
            return QueueOperationType::None;

        // Managed PvP lifecycle policy: never auto-fill arena. Any lingering arena
        // queue or invite state should be actively removed so battleground
        // participation can converge back to Warsong-only behavior.
        if (values.hasArenaQueue || values.hasArenaInvite)
            return QueueOperationType::Leave;

        return QueueOperationType::None;
    }

    ArenaTeamInteractionType PvpCore::SelectArenaTeamInteractionSkeleton(PvpValues const& values)
    {
        // Managed PvP lifecycle policy: never join arena teams automatically.
        // Decline any arena team invite regardless of current queue state.
        if (values.hasArenaTeamInvite)
            return ArenaTeamInteractionType::DeclineInvite;

        return ArenaTeamInteractionType::None;
    }

std::string PvpCore::GetLastEveryManForHimselfDiagnostic(Player const* player)
{
    if (!player)
        return std::string();

    if (WarriorGapCloserDiagnosticEntry const* entry =
            playerbot::LockedFind(g_EveryManForHimselfDiagnosticByGuid, player->GetGUID().GetRawValue()))
    {
        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32 const ageMs = entry->recordedMs != 0 && nowMs >= entry->recordedMs ? nowMs - entry->recordedMs : 0;
        std::ostringstream aged;
        aged << "age_ms=" << ageMs << ' ' << entry->text;
        return aged.str();
    }

    return std::string();
}

std::string PvpCore::GetLastWarriorGapCloserDiagnostic(Player const* player)
{
    if (!player)
        return std::string();

    if (WarriorGapCloserDiagnosticEntry const* entry =
            playerbot::LockedFind(g_WarriorGapCloserDiagnosticByGuid, player->GetGUID().GetRawValue()))
    {
        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32 const ageMs = entry->recordedMs != 0 && nowMs >= entry->recordedMs ? nowMs - entry->recordedMs : 0;
        std::ostringstream aged;
        aged << "age_ms=" << ageMs << ' ' << entry->text;
        return aged.str();
    }

    return std::string();
}
}
