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

#include "Miscellaneous/TournamentMode.h"
#include "Miscellaneous/RiftProgression.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PetDefines.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Timer.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Tournament
{
namespace
{
    struct InnateSpell
    {
        uint32 SpellId = 0;
        uint32 ClassMask = 0;   // 0 = every class
    };

    struct Settings
    {
        bool Enabled = false;
        bool WaiveReagents = false;
        bool TournamentWaiveReagents = true;
        uint8 StartLevel = 60;
        uint32 QueueMinLevel = 60;
        bool ForceQueueAtMinLevel = true;
        bool HasHome = false;
        uint32 HomeMap = 0;
        float HomeX = 0.0f;
        float HomeY = 0.0f;
        float HomeZ = 0.0f;
        float HomeO = 0.0f;
        std::unordered_set<uint32> AllowedZones;
        std::unordered_set<uint32> AllowedCreatures;
        std::unordered_set<uint32> AllowedLootObjects;
        std::unordered_set<uint32> OpenDungeons;
        std::vector<InnateSpell> InnateSpells;
        uint32 PhaseMask = 0;
        uint32 WorldPhaseMask = 0;
        std::vector<std::pair<uint32 /*questId*/, uint32 /*phaseMask*/>> WorldQuestPhases;
        uint32 WorldQuestPhaseMask = 0;   // every bit WorldQuestPhases can hand out
        bool MaxWeaponSkill = true;
        bool MaxSkillForLevel = true;
        int32 DeathSicknessLevel = 61;
        bool ConsumeAmmo = false;
        bool PetHappinessDecay = false;
        bool ResetDuelCooldowns = true;
        bool ResetDuelHealthMana = true;
        uint32 WorldMaxHonorPoints = uint32(std::numeric_limits<int32>::max());
        bool WorldPricesEnabled = false;
        // ExtendedCost id the world hub vendors were built with -> honor it costs today.
        std::unordered_map<uint32, uint32> WorldVendorHonor;
        // Quest id -> Warchief's Socks its first objective asks for.
        std::unordered_map<uint32, uint32> WorldQuestSocks;
        // The text the two lists were parsed from, and a counter that moves when
        // it does. The prices are applied where the vendor and quest caches are
        // filled, so World has to know when a `.reload config` made them stale.
        std::string WorldPriceText;
        uint32 WorldPriceRevision = 0;
    };

    Settings Config;

    // A character this close to the home location is never outside the grounds.
    constexpr float HomeRadius = 40.0f;
    // A typo such as "1-900000" must not allocate a set the size of a DBC.
    constexpr uint32 MaxIdRange = 100000;
    // createTournamentKit clones the class template's saved spellbook. Those
    // templates are Orcs, so these active racials must be removed again for
    // every other race instead of surviving beside the recipient's own racial.
    constexpr std::array<uint32, 3> OrcBloodFurySpells = { 20572, 33697, 33702 };

    // Startup data, read-only once loaded.
    std::unordered_map<uint16, CreateInfo> CreateInfoStore;   // race << 8 | class
    bool KitProcedureExists = false;

    uint16 CreateInfoKey(uint32 race, uint32 playerClass)
    {
        return uint16((race << 8) | playerClass);
    }

    std::string_view Trim(std::string_view token)
    {
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.remove_prefix(1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.remove_suffix(1);
        return token;
    }

    // "1, 2,3, 10-12" -> {1, 2, 3, 10, 11, 12}. Neither Trinity::StringTo nor
    // std::from_chars tolerates whitespace, so every token is trimmed first; a
    // token that does not parse is reported rather than silently dropped.
    std::vector<uint32> ParseIdList(char const* key, std::string const& raw)
    {
        std::vector<uint32> ids;
        for (std::string_view token : Trinity::Tokenize(raw, ',', false))
        {
            token = Trim(token);
            if (token.empty())
                continue;

            std::size_t const dash = token.find('-');
            if (dash == std::string_view::npos)
            {
                if (Optional<uint32> id = Trinity::StringTo<uint32>(token))
                    ids.push_back(*id);
                else
                    TC_LOG_ERROR("server.loading", "{}: ignoring '{}', not a number.", key, token);
                continue;
            }

            Optional<uint32> const first = Trinity::StringTo<uint32>(Trim(token.substr(0, dash)));
            Optional<uint32> const last = Trinity::StringTo<uint32>(Trim(token.substr(dash + 1)));
            if (!first || !last || *first > *last || *last - *first >= MaxIdRange)
            {
                TC_LOG_ERROR("server.loading", "{}: ignoring '{}', expected a range first-last of at most {} ids.", key, token, MaxIdRange);
                continue;
            }

            for (uint32 offset = 0; offset <= *last - *first; ++offset)
                ids.push_back(*first + offset);
        }
        return ids;
    }

    // "3006:200, 60007:1500" -> {{3006, 200}, {60007, 1500}}. Same trimming and
    // the same "say what was dropped" rule as ParseIdList. A price of zero is
    // refused rather than stored: for a vendor tier it would mean free, and for
    // a quest it would leave an objective asking for none of an item.
    std::unordered_map<uint32, uint32> ParsePairList(char const* key, std::string const& raw)
    {
        std::unordered_map<uint32, uint32> pairs;
        for (std::string_view token : Trinity::Tokenize(raw, ',', false))
        {
            token = Trim(token);
            if (token.empty())
                continue;

            std::size_t const colon = token.find(':');
            Optional<uint32> const id = colon == std::string_view::npos ? Optional<uint32>() : Trinity::StringTo<uint32>(Trim(token.substr(0, colon)));
            Optional<uint32> const value = colon == std::string_view::npos ? Optional<uint32>() : Trinity::StringTo<uint32>(Trim(token.substr(colon + 1)));
            if (!id || !value || !*id)
            {
                TC_LOG_ERROR("server.loading", "{}: ignoring '{}', expected id:price.", key, token);
                continue;
            }

            if (!*value)
            {
                TC_LOG_ERROR("server.loading", "{}: ignoring '{}', zero is not a price.", key, token);
                continue;
            }

            pairs[*id] = *value;
        }
        return pairs;
    }

    // Every ItemExtendedCost row that asks for honor and nothing else, indexed
    // by the honor it asks for. This is the whole reason world prices can move
    // without a client patch: the honor number lives in the DBC, so re-pricing a
    // tier means finding another row that already carries the wanted number. The
    // lowest id wins, which prefers the stock rows the tournament vendors
    // themselves use over anything added later for the same number.
    //
    // Built once. The DBC store is read-only after startup, and everything here
    // runs on the world thread (ObjectMgr::LoadVendors, at startup or from
    // `.reload`), never under a map tick.
    //
    // Ordered, so FindHonorOnlyCost can step up to the next rung when no row
    // carries a computed price exactly.
    std::map<uint32, uint32> HonorOnlyCosts;
    bool HonorOnlyCostsBuilt = false;

    void BuildHonorOnlyCosts()
    {
        HonorOnlyCostsBuilt = true;
        for (uint32 i = 0; i < sItemExtendedCostStore.GetNumRows(); ++i)
        {
            ItemExtendedCostEntry const* cost = sItemExtendedCostStore.LookupEntry(i);
            if (!cost || !cost->HonorPoints || cost->ArenaPoints || cost->ArenaBracket || cost->RequiredArenaRating)
                continue;

            bool wantsAnItem = false;
            for (uint8 slot = 0; slot < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++slot)
                if (cost->ItemID[slot])
                    wantsAnItem = true;
            if (wantsAnItem)
                continue;

            auto const placed = HonorOnlyCosts.try_emplace(cost->HonorPoints, cost->ID);
            if (!placed.second && cost->ID < placed.first->second)
                placed.first->second = cost->ID;
        }
    }

    // Centurion.WorldPrices.VendorHonor resolved against that index: the id a
    // world hub vendor row was built with -> the id it is served as. Built on
    // first use rather than in LoadConfig, because at startup the config is read
    // before the DBCs are loaded and there would be nothing to search.
    std::unordered_map<uint32, uint32> ResolvedVendorCosts;
    uint32 ResolvedVendorCostRevision = 0;

    void ResolveVendorCosts()
    {
        ResolvedVendorCosts.clear();
        ResolvedVendorCostRevision = Config.WorldPriceRevision;

        if (!Config.WorldPricesEnabled || Config.WorldVendorHonor.empty())
            return;

        if (!HonorOnlyCostsBuilt)
            BuildHonorOnlyCosts();

        // Plain locals rather than a structured binding: the log macros the loop
        // body uses take their arguments by lambda capture, which C++17 refuses
        // for a binding name and the Linux build would reject.
        for (auto const& tier : Config.WorldVendorHonor)
        {
            uint32 const costId = tier.first;
            uint32 const honor = tier.second;

            auto const row = HonorOnlyCosts.find(honor);
            if (row == HonorOnlyCosts.end())
            {
                TC_LOG_ERROR("server.loading", "Centurion.WorldPrices.VendorHonor: no ItemExtendedCost row asks for exactly {} honor and nothing else, "
                    "so tier {} keeps the price it shipped with. An honor amount cannot be invented at runtime - add the row to ItemExtendedCost.dbc "
                    "(server AND client) or pick a number that already has one.", honor, costId);
                continue;
            }

            ResolvedVendorCosts[costId] = row->second;
            TC_LOG_INFO("server.loading", "Centurion world prices: vendor tier {} charges {} honor (ItemExtendedCost {}).", costId, honor, row->second);
        }
    }

    bool IsPlainPlayer(Player const* player)
    {
        return player && player->GetSession() && !player->GetSession()->IsVirtualSession();
    }
}

void LoadConfig()
{
    Settings loaded;
    loaded.Enabled = sConfigMgr->GetBoolDefault("Centurion.Tournament.Enable", false);
    loaded.WaiveReagents = sConfigMgr->GetBoolDefault("Centurion.Pvp.WaiveReagentsAndAmmo", false);
    loaded.TournamentWaiveReagents = sConfigMgr->GetBoolDefault("Centurion.Tournament.WaiveReagents", true);
    loaded.StartLevel = uint8(std::clamp<int32>(sConfigMgr->GetIntDefault("Centurion.Tournament.StartLevel", 60), 1, STRONG_MAX_LEVEL));
    loaded.QueueMinLevel = uint32(std::max<int32>(1, sConfigMgr->GetIntDefault("Centurion.Tournament.QueueMinLevel", 60)));
    loaded.ForceQueueAtMinLevel = sConfigMgr->GetBoolDefault("Centurion.Tournament.ForceQueueAtMinLevel", true);

    for (uint32 id : ParseIdList("Centurion.Tournament.AllowedZones", sConfigMgr->GetStringDefault("Centurion.Tournament.AllowedZones", "")))
        loaded.AllowedZones.insert(id);

    for (uint32 id : ParseIdList("Centurion.Tournament.AllowedCreatures", sConfigMgr->GetStringDefault("Centurion.Tournament.AllowedCreatures", "")))
        loaded.AllowedCreatures.insert(id);

    for (uint32 id : ParseIdList("Centurion.Tournament.AllowedLootObjects", sConfigMgr->GetStringDefault("Centurion.Tournament.AllowedLootObjects", "")))
        loaded.AllowedLootObjects.insert(id);

    for (uint32 id : ParseIdList("Centurion.Tournament.OpenDungeons", sConfigMgr->GetStringDefault("Centurion.Tournament.OpenDungeons", "")))
        loaded.OpenDungeons.insert(id);

    // "spell[:classmask]"
    std::string const rawInnate = sConfigMgr->GetStringDefault("Centurion.Tournament.InnateSpells", "29073,18610,22734:1494");
    for (std::string_view token : Trinity::Tokenize(rawInnate, ',', false))
    {
        token = Trim(token);
        if (token.empty())
            continue;

        InnateSpell innate;
        std::string_view spellPart = token;
        std::string_view maskPart;
        if (std::size_t colon = token.find(':'); colon != std::string_view::npos)
        {
            spellPart = Trim(token.substr(0, colon));
            maskPart = Trim(token.substr(colon + 1));
        }

        Optional<uint32> spellId = Trinity::StringTo<uint32>(spellPart);
        Optional<uint32> classMask = maskPart.empty() ? Optional<uint32>(0) : Trinity::StringTo<uint32>(maskPart);
        if (!spellId || !classMask)
        {
            TC_LOG_ERROR("server.loading", "Centurion.Tournament.InnateSpells: ignoring '{}', expected spell[:classmask].", token);
            continue;
        }

        innate.SpellId = *spellId;
        innate.ClassMask = *classMask;
        loaded.InnateSpells.push_back(innate);
    }

    loaded.PhaseMask = uint32(sConfigMgr->GetIntDefault("Centurion.Tournament.PhaseMask", 0));
    if (loaded.PhaseMask == uint32(PHASEMASK_ANYWHERE))
    {
        TC_LOG_ERROR("server.loading", "Centurion.Tournament.PhaseMask cannot be every phase; tournament phasing is off.");
        loaded.PhaseMask = 0;
    }
    else if (loaded.PhaseMask & PHASEMASK_NORMAL)
    {
        // The normal phase is everyone's: NPCs spawned there are not hidden
        // from world characters, so the bit adds nothing but confusion.
        TC_LOG_WARN("server.loading", "Centurion.Tournament.PhaseMask includes the normal phase (1); ignoring that bit.");
        loaded.PhaseMask &= ~uint32(PHASEMASK_NORMAL);
    }

    // The mirror image: a phase only world characters carry, for world creatures
    // tournament characters must not see (New Hearthglen's stock town under the
    // tournament vendors). The normal phase and the tournament phase are both
    // shared or taken, so neither bit can be in it.
    loaded.WorldPhaseMask = uint32(sConfigMgr->GetIntDefault("Centurion.Tournament.WorldPhaseMask", 0));
    if (loaded.WorldPhaseMask == uint32(PHASEMASK_ANYWHERE))
    {
        TC_LOG_ERROR("server.loading", "Centurion.Tournament.WorldPhaseMask cannot be every phase; world-only phasing is off.");
        loaded.WorldPhaseMask = 0;
    }
    else if (loaded.WorldPhaseMask & (PHASEMASK_NORMAL | loaded.PhaseMask))
    {
        TC_LOG_WARN("server.loading", "Centurion.Tournament.WorldPhaseMask shares bits with the normal or tournament phase; ignoring those bits.");
        loaded.WorldPhaseMask &= ~(uint32(PHASEMASK_NORMAL) | loaded.PhaseMask);
    }

    // "quest:phase" pairs. A world character that has been rewarded the quest
    // carries that phase from then on, so a hub spawned in it appears the moment
    // its teleport is earned and never before - the socks chain opens Gurubashi,
    // then Blackrock and Coldarra, then New Hearthglen, then the Caverns. A
    // tournament character carries PhaseMask instead and sees none of them.
    std::string const rawQuestPhases = sConfigMgr->GetStringDefault("Centurion.Tournament.WorldQuestPhases", "");
    for (std::string_view token : Trinity::Tokenize(rawQuestPhases, ',', false))
    {
        token = Trim(token);
        if (token.empty())
            continue;

        std::size_t const colon = token.find(':');
        Optional<uint32> const quest = colon == std::string_view::npos ? Optional<uint32>() : Trinity::StringTo<uint32>(Trim(token.substr(0, colon)));
        Optional<uint32> const phase = colon == std::string_view::npos ? Optional<uint32>() : Trinity::StringTo<uint32>(Trim(token.substr(colon + 1)));
        if (!quest || !phase || !*quest || !*phase || *phase == uint32(PHASEMASK_ANYWHERE))
        {
            TC_LOG_ERROR("server.loading", "Centurion.Tournament.WorldQuestPhases: ignoring '{}', expected quest:phase.", token);
            continue;
        }

        // The normal phase is everyone's and the tournament phase is taken, so a
        // quest can hand out neither: doing so would show the hub to a character
        // that has not earned it, or to the other mode entirely.
        uint32 const bits = *phase & ~(uint32(PHASEMASK_NORMAL) | loaded.PhaseMask);
        if (bits != *phase)
            TC_LOG_WARN("server.loading", "Centurion.Tournament.WorldQuestPhases: quest {} asks for the normal or tournament phase; ignoring those bits.", *quest);
        if (!bits)
            continue;

        loaded.WorldQuestPhases.emplace_back(*quest, bits);
        loaded.WorldQuestPhaseMask |= bits;
    }

    loaded.MaxWeaponSkill = sConfigMgr->GetBoolDefault("Centurion.Tournament.AlwaysMaxWeaponSkill", true);
    loaded.MaxSkillForLevel = sConfigMgr->GetBoolDefault("Centurion.Tournament.AlwaysMaxSkillForLevel", true);
    loaded.DeathSicknessLevel = sConfigMgr->GetIntDefault("Centurion.Tournament.DeathSicknessLevel", 61);
    loaded.ConsumeAmmo = sConfigMgr->GetBoolDefault("Centurion.Tournament.ConsumeAmmo", false);
    loaded.PetHappinessDecay = sConfigMgr->GetBoolDefault("Centurion.Tournament.PetHappinessDecay", false);
    loaded.ResetDuelCooldowns = sConfigMgr->GetBoolDefault("Centurion.Tournament.ResetDuelCooldowns", true);
    loaded.ResetDuelHealthMana = sConfigMgr->GetBoolDefault("Centurion.Tournament.ResetDuelHealthMana", true);

    // A config int stops at 2147483647 (a larger number falls back to this
    // default), which is also as far as the client's signed honor field goes.
    int32 const worldMaxHonor = sConfigMgr->GetIntDefault("Centurion.Tournament.WorldMaxHonorPoints", std::numeric_limits<int32>::max());
    if (worldMaxHonor < 0)
        TC_LOG_ERROR("server.loading", "Centurion.Tournament.WorldMaxHonorPoints ({}) can't be negative; world characters follow MaxHonorPoints.", worldMaxHonor);
    loaded.WorldMaxHonorPoints = uint32(std::max<int32>(0, worldMaxHonor));

    // "map x y z o"
    std::string const rawHome = sConfigMgr->GetStringDefault("Centurion.Tournament.HomeLocation", "");
    if (!rawHome.empty())
    {
        std::vector<std::string_view> parts;
        for (std::string_view token : Trinity::Tokenize(rawHome, ' ', false))
            if (!Trim(token).empty())
                parts.push_back(Trim(token));

        Optional<uint32> map = parts.size() >= 4 ? Trinity::StringTo<uint32>(parts[0]) : Optional<uint32>();
        Optional<float> x = parts.size() >= 4 ? Trinity::StringTo<float>(parts[1]) : Optional<float>();
        Optional<float> y = parts.size() >= 4 ? Trinity::StringTo<float>(parts[2]) : Optional<float>();
        Optional<float> z = parts.size() >= 4 ? Trinity::StringTo<float>(parts[3]) : Optional<float>();
        Optional<float> o = parts.size() >= 5 ? Trinity::StringTo<float>(parts[4]) : Optional<float>(0.0f);

        // At startup the config is read before the DBCs load, so the map store
        // is still empty and every map id would look invalid: check only the
        // coordinates then, and the map too on a `.reload config`.
        // LoadCreateInfo checks the map once the DBCs are in.
        bool const mapStoreLoaded = sMapStore.GetNumRows() != 0;
        if (map && x && y && z && o && Trinity::IsValidMapCoord(*x, *y, *z, *o)
            && (!mapStoreLoaded || MapManager::IsValidMapCoord(*map, *x, *y, *z, *o)))
        {
            loaded.HasHome = true;
            loaded.HomeMap = *map;
            loaded.HomeX = *x;
            loaded.HomeY = *y;
            loaded.HomeZ = *z;
            loaded.HomeO = *o;
        }
        else
            TC_LOG_ERROR("server.loading", "Centurion.Tournament.HomeLocation: '{}' is not a valid \"map x y z o\" position.", rawHome);
    }

    // What the world hub charges. Its own switch, not Tournament.Enable's: the
    // prices are world-character rules, and the owner re-tunes them far more
    // often than anything else here. Off = the vendor rows and quest_template
    // speak for themselves, which is the economy the hub shipped with.
    loaded.WorldPricesEnabled = sConfigMgr->GetBoolDefault("Centurion.WorldPrices.Enable", false);
    std::string const rawVendorHonor = sConfigMgr->GetStringDefault("Centurion.WorldPrices.VendorHonor", "");
    std::string const rawQuestSocks = sConfigMgr->GetStringDefault("Centurion.WorldPrices.QuestSocks", "");
    loaded.WorldVendorHonor = ParsePairList("Centurion.WorldPrices.VendorHonor", rawVendorHonor);
    loaded.WorldQuestSocks = ParsePairList("Centurion.WorldPrices.QuestSocks", rawQuestSocks);

    loaded.WorldPriceText = (loaded.WorldPricesEnabled ? "1|" : "0|") + rawVendorHonor + "|" + rawQuestSocks;
    loaded.WorldPriceRevision = Config.WorldPriceRevision + (loaded.WorldPriceText != Config.WorldPriceText ? 1 : 0);

    if (loaded.WorldPricesEnabled)
        TC_LOG_INFO("server.loading", "Centurion world prices enabled: {} vendor tier(s), {} quest price(s).",
            loaded.WorldVendorHonor.size(), loaded.WorldQuestSocks.size());

    if (loaded.Enabled)
    {
        if (loaded.AllowedZones.empty())
            TC_LOG_WARN("server.loading", "Centurion.Tournament.AllowedZones is empty: tournament characters are not confined to any zone.");
        else if (!loaded.HasHome)
            TC_LOG_WARN("server.loading", "Centurion.Tournament.HomeLocation is not set: tournament characters found outside their zones cannot be sent back.");
        if (loaded.AllowedCreatures.empty())
            TC_LOG_WARN("server.loading", "Centurion.Tournament.AllowedCreatures is empty: tournament characters may talk to every NPC.");

        TC_LOG_INFO("server.loading", "Tournament characters enabled: {} zone(s), {} creature(s), {} innate spell(s), phase mask {}.",
            loaded.AllowedZones.size(), loaded.AllowedCreatures.size(), loaded.InnateSpells.size(), loaded.PhaseMask);
    }

    Config = std::move(loaded);

    // The battleground loadout reads its own keys; it is part of the same
    // feature and follows the same `.reload config`.
    LoadLoadoutConfig();
}

bool IsEnabled()
{
    return Config.Enabled;
}

bool IsTournamentCharacter(Player const* player)
{
    return Config.Enabled && player && player->HasTournamentModeFlag();
}

bool IsTournamentCharacter(ObjectGuid guid)
{
    if (!Config.Enabled || !guid.IsPlayer())
        return false;

    if (Player const* player = ObjectAccessor::FindConnectedPlayer(guid))
        return player->HasTournamentModeFlag();

    return sCharacterCache->IsTournamentCharacterByGuid(guid);
}

bool AreSeparated(Player const* a, Player const* b)
{
    if (!Config.Enabled || !a || !b)
        return false;

    return IsTournamentCharacter(a) != IsTournamentCharacter(b);
}

bool AreSeparated(ObjectGuid a, ObjectGuid b)
{
    if (!Config.Enabled)
        return false;

    return IsTournamentCharacter(a) != IsTournamentCharacter(b);
}

bool AreKeptFromFighting(Player const* a, Player const* b)
{
    if (a == b || !AreSeparated(a, b))
        return false;

    if (Map const* map = a->FindMap())
        if (map->IsBattlegroundOrArena())
            return false;

    return !(a->IsInGurubashiBattleRing() && b->IsInGurubashiBattleRing());
}

bool QueuesInTournamentPool(Player const* player)
{
    if (!Config.Enabled || !player)
        return false;

    if (IsTournamentCharacter(player))
        return true;

    if (player->GetLevel() < Config.QueueMinLevel)
        return false;

    // At the tournament level there is no world queue left to join: every
    // battleground and arena a world character queues for from here on is
    // fought under tournament rules. The opt-in flag is still read and still
    // kept, so turning the rule back off hands everybody the choice they last
    // made, exactly as they left it.
    return Config.ForceQueueAtMinLevel || player->HasTournamentQueueFlag();
}

QueueLockReason GetQueueLockReason(Player const* player)
{
    if (!Config.Enabled || !player)
        return QUEUE_LOCK_DISABLED;

    if (IsTournamentCharacter(player))
        return QUEUE_LOCK_TOURNAMENT_CHARACTER;

    if (player->GetLevel() < Config.QueueMinLevel)
        return QUEUE_LOCK_BELOW_LEVEL;

    if (Config.ForceQueueAtMinLevel)
        return QUEUE_LOCK_FORCED;

    // A queued group keeps the pool it joined with, so the switch waits until
    // the character is out of every queue and every match.
    if (player->InBattlegroundQueue() || player->InBattleground())
        return QUEUE_LOCK_BUSY;

    return QUEUE_LOCK_NONE;
}

bool CanToggleTournamentQueue(Player const* player)
{
    return GetQueueLockReason(player) == QUEUE_LOCK_NONE;
}

bool IsReachableByWorldBots(Player const* player)
{
    if (!Config.Enabled || !player)
        return true;

    // A tournament character is the one thing the fleet can never meet: no bot
    // is one.
    if (IsTournamentCharacter(player))
        return false;

    // Everything else is a world character, and a bot of the same level follows
    // the very same pool rule - forced in at the tournament level, out below it.
    // Only a world character who opted in by hand (the rule off) is somewhere
    // the fleet is not.
    if (Config.ForceQueueAtMinLevel && player->GetLevel() >= Config.QueueMinLevel)
        return true;

    return !QueuesInTournamentPool(player);
}

uint32 GetQueueMinLevel()
{
    return Config.QueueMinLevel;
}

namespace
{
    // The GUID overload: the Player* one rewrites LANG_ADDON to Universal and
    // the addon data would show up as a whisper.
    void SendAddonLine(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(),
            message, 0, player->GetName(), player->GetName());
        player->SendDirectMessage(&data);
    }
}

void SendQueueState(Player* player)
{
    // A realm without character modes answers nothing, and the Battlegrounds
    // tab keeps the toggle hidden.
    if (!Config.Enabled || !player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    QueueLockReason const reason = GetQueueLockReason(player);
    SendAddonLine(player, Trinity::StringFormat("CCGAME\tTQUEUE:{}:{}",
        QueuesInTournamentPool(player) ? 1 : 0, reason != QUEUE_LOCK_NONE ? 1 : 0));

    // Why it reads the way it does, and which match rules this realm is
    // actually running, so the tooltip promises nothing that is switched off.
    // Its own line on purpose: a client built before this existed matches the
    // TQUEUE line whole and would lose the toggle altogether if fields were
    // added to it, while a line it has never heard of it simply ignores.
    uint32 rules = 0;
    if (IsBgLoadoutEnabled())
        rules |= QUEUE_RULE_LOADOUT;
    if (AreMatchConsumablesBanned())
        rules |= QUEUE_RULE_BAN_CONSUMABLES;

    SendAddonLine(player, Trinity::StringFormat("CCGAME\tTQUEUEWHY:{}:{}:{}", uint32(reason), Config.QueueMinLevel, rules));
}

void SendGurubashiChestState(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    SendAddonLine(player, Trinity::StringFormat("CCGAME\tGURUCHEST:{}", player->HasGurubashiChestOptOut() ? 0 : 1));
}

void SendArenaBotFillState(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    // A realm that never fills arenas - skirmish or rated - has nothing to
    // offer here, and an unanswered request leaves the box hidden rather than
    // lying about it.
    if (!sBattlegroundMgr->IsBotFillSkirmishArenaEnabled() && !sBattlegroundMgr->IsBotFillRatedArenaEnabled())
        return;

    SendAddonLine(player, Trinity::StringFormat("CCGAME\tARENABOTS:{}", player->HasArenaBotFillOptOut() ? 0 : 1));
}

namespace
{
    // Written from the world thread (the hourly check, the despawn) and from the
    // chest's map thread (looting), read by whichever thread answers the request.
    std::atomic<int64> GurubashiChestNextCheck{ 0 };
    std::atomic<int64> GurubashiChestExpiresAt{ 0 };
}

void SetGurubashiChestClock(time_t nextCheck, time_t chestExpiresAt)
{
    GurubashiChestNextCheck = int64(nextCheck);
    GurubashiChestExpiresAt = int64(chestExpiresAt);
}

void SendGurubashiChestTimer(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    int64 const now = int64(GameTime::GetGameTime());
    int64 const expiresAt = GurubashiChestExpiresAt;
    bool const chestOut = expiresAt > now;
    int64 const until = chestOut ? expiresAt : int64(GurubashiChestNextCheck);
    if (until <= 0)
        return;

    SendAddonLine(player, Trinity::StringFormat("CCGAME\tGURUTIMER:{}:{}", chestOut ? 1 : 0, std::max<int64>(0, until - now)));
}

bool HandleAddonRequest(Player* sender, uint32 lang, std::string const& msg)
{
    if (!sender || lang != LANG_ADDON)
        return false;

    // The Gurubashi chest's clock beside that toggle (read only).
    if (msg == "CCGAMEREQ\tGURUTIMER")
    {
        SendGurubashiChestTimer(sender);
        return true;
    }

    // The Gurubashi chest toggle (Battlegrounds tab): whether the hourly chest
    // counts this character and pulls it into the Battle Ring. Realm-agnostic -
    // every realm on the branch runs the chest.
    static constexpr std::string_view ChestRequest = "CCGAMEREQ\tGURUCHEST";
    if (StringStartsWith(msg, ChestRequest))
    {
        std::string_view const argument = std::string_view(msg).substr(ChestRequest.size());
        if (argument == ":1" || argument == ":0")
            sender->SetGurubashiChestOptOut(argument == ":0");

        SendGurubashiChestState(sender);
        return true;
    }

    // The arena bot-fill toggle (Battlegrounds tab): whether a skirmish or
    // rated arena queue this character is waiting in may be started as a
    // clone-filled match. Also realm-agnostic, and answered only where the
    // realm fills arenas at all. The switch is free at any time - it is read when a match is about
    // to be made, so flipping it mid-queue simply decides the next pop.
    static constexpr std::string_view ArenaBotsRequest = "CCGAMEREQ\tARENABOTS";
    if (StringStartsWith(msg, ArenaBotsRequest))
    {
        std::string_view const argument = std::string_view(msg).substr(ArenaBotsRequest.size());
        if (argument == ":1" || argument == ":0")
            sender->SetArenaBotFillOptOut(argument == ":0");

        SendArenaBotFillState(sender);
        return true;
    }

    static constexpr std::string_view Request = "CCGAMEREQ\tTQUEUE";
    if (!StringStartsWith(msg, Request))
        return false;

    // No answer on a realm without character modes: the Battlegrounds tab only
    // shows the tournament-queue checkbox once the server has answered.
    if (!Config.Enabled)
        return true;

    std::string_view const argument = std::string_view(msg).substr(Request.size());
    if (argument == ":1" || argument == ":0")
    {
        bool const on = argument == ":1";
        // A locked toggle is simply answered with its real state.
        if (CanToggleTournamentQueue(sender))
            sender->SetTournamentQueueFlag(on);
    }

    SendQueueState(sender);
    return true;
}

bool HasConfinement()
{
    return Config.Enabled && !Config.AllowedZones.empty();
}

bool IsOpenDungeon(uint32 mapId)
{
    return Config.Enabled && Config.OpenDungeons.count(mapId);
}

bool IsLocationAllowed(uint32 mapId, uint32 zoneId, uint32 areaId)
{
    if (!HasConfinement())
        return true;

    // Queued matches carry their own rules; a battleground or arena is never
    // "leaving" the tournament grounds. Neither is an open dungeon.
    if (MapEntry const* entry = sMapStore.LookupEntry(mapId))
        if (entry->IsBattlegroundOrArena())
            return true;
    if (IsOpenDungeon(mapId))
        return true;

    return Config.AllowedZones.count(zoneId) || Config.AllowedZones.count(areaId);
}

bool IsLocationAllowed(uint32 mapId, float x, float y, float z)
{
    if (!HasConfinement())
        return true;

    if (MapEntry const* entry = sMapStore.LookupEntry(mapId))
        if (entry->IsBattlegroundOrArena())
            return true;
    if (IsOpenDungeon(mapId))
        return true;

    // The home location is always a legal destination, so a misconfigured zone
    // list can never bounce a character between two refusals.
    if (Config.HasHome && mapId == Config.HomeMap &&
        std::abs(x - Config.HomeX) < 1.0f && std::abs(y - Config.HomeY) < 1.0f && std::abs(z - Config.HomeZ) < 5.0f)
        return true;

    if (!MapManager::IsValidMapCoord(mapId, x, y))
        return false;

    // An area inside a WMO (the Caverns of Time hub is WMO area 1941/2300 under
    // Tanaris terrain) comes from the grid's collision tile, and Map::GetAreaInfo
    // queries that tile before Map::GetGrid creates the grid that loads it. For a
    // grid nobody stands in, the first lookup saw only the terrain and refused a
    // teleport into an allowed area. GetGridHeight creates the grid first.
    uint32 zoneId = 0;
    uint32 areaId = 0;
    Map const* map = sMapMgr->CreateBaseMap(mapId);
    if (!map)
        return false;

    map->GetGridHeight(x, y);
    map->GetZoneAndAreaId(PHASEMASK_NORMAL, zoneId, areaId, x, y, z);
    return IsLocationAllowed(mapId, zoneId, areaId);
}

bool GetHomeLocation(WorldLocation& out)
{
    if (!Config.HasHome)
        return false;

    out.WorldRelocate(Config.HomeMap, Config.HomeX, Config.HomeY, Config.HomeZ, Config.HomeO);
    return true;
}

bool EnforceConfinement(Player* player)
{
    if (!HasConfinement() || !IsTournamentCharacter(player) || player->IsGameMaster())
        return false;

    if (!player->IsInWorld() || player->IsBeingTeleported() || player->IsInFlight() || !IsPlainPlayer(player))
        return false;

    // Custom games are open to tournament characters, lobby included (it is a
    // private copy of Kalimdor, not a battleground map).
    if (player->IsInCustomGameLobby())
        return false;

    if (IsLocationAllowed(player->GetMapId(), player->GetZoneId(), player->GetAreaId()))
        return false;

    WorldLocation home;
    if (!GetHomeLocation(home))
        return false;

    // Home is never outside: a zone list that leaves it out must not send a
    // character home again every update.
    if (player->GetMapId() == home.GetMapId() && player->GetExactDist2dSq(home.GetPositionX(), home.GetPositionY()) < HomeRadius * HomeRadius)
        return false;

    SendRefusal(player, "leave the tournament grounds");
    player->TeleportTo(home);
    return true;
}

uint32 GetExtraPhaseMask(Player const* player)
{
    if (!Config.Enabled || !player)
        return 0;

    if (IsTournamentCharacter(player))
        return Config.PhaseMask;

    // Plus one phase per hub the character has paid for. The bit test first
    // keeps this to one comparison per quest once a phase is already granted,
    // and two quests may share a bit (Blackrock and Coldarra open together).
    uint32 mask = Config.WorldPhaseMask;
    for (auto const& [questId, phase] : Config.WorldQuestPhases)
        if ((mask & phase) != phase && player->IsQuestRewarded(questId))
            mask |= phase;
    return mask;
}

void RefreshPhase(Player* player)
{
    // Game masters see every phase until they leave GM mode, which recomputes
    // the phase through Player::SetPhaseMask anyway.
    if (!player || player->IsGameMaster())
        return;

    // The computation AuraEffect::HandlePhase and Player::SetGameMaster make;
    // Player::SetPhaseMask adds the tournament phase on top.
    uint32 phase = 0;
    for (AuraEffect const* effect : player->GetAuraEffectsByType(SPELL_AURA_PHASE))
        phase |= uint32(effect->GetMiscValue());
    if (!phase)
        phase = PHASEMASK_NORMAL;

    player->SetPhaseMask(phase, player->IsInWorld());
}

bool IsPhaseStale(Player const* player)
{
    if (!player || player->IsGameMaster())
        return false;

    // Carries its own mode's phase, and the hub phases it has earned, and
    // neither the other mode's phase nor a hub it has not earned yet.
    uint32 const wanted = GetExtraPhaseMask(player);
    uint32 const unwanted = (Config.PhaseMask | Config.WorldPhaseMask | Config.WorldQuestPhaseMask) & ~wanted;
    return (player->GetPhaseMask() & wanted) != wanted || (player->GetPhaseMask() & unwanted) != 0;
}

bool CanInteractWithCreature(Player const* player, Creature const* creature, uint32 npcFlags)
{
    if (!IsTournamentCharacter(player) || player->IsGameMaster() || !creature)
        return true;

    // Battleground spirit guides, battlemasters inside the match and other
    // match furniture belong to the match, not to the world; the custom-game
    // lobby's staff belong to custom games, which are open to everyone.
    if (Map const* map = player->FindMap())
        if (map->IsBattlegroundOrArena() || IsOpenDungeon(map->GetId()))
            return true;
    if (player->IsInCustomGameLobby())
        return true;

    // A character's own pet or vehicle is not an NPC in any sense that matters.
    if (creature->GetOwnerGUID() == player->GetGUID() || creature->GetCharmerOrOwnerGUID() == player->GetGUID())
        return true;

    // The auction house and flight masters are closed to tournament characters
    // whatever the list says.
    if (npcFlags & UNIT_NPC_FLAG_AUCTIONEER)
    {
        SendRefusal(player, "use the auction house");
        return false;
    }
    if (npcFlags & UNIT_NPC_FLAG_FLIGHTMASTER)
    {
        SendRefusal(player, "use flight masters");
        return false;
    }

    if (Config.AllowedCreatures.empty())
        return true;

    // Nobody is left unable to resurrect or to queue.
    if (creature->HasNpcFlag(NPCFlags(UNIT_NPC_FLAG_SPIRITHEALER | UNIT_NPC_FLAG_SPIRITGUIDE | UNIT_NPC_FLAG_BATTLEMASTER)))
        return true;

    if (Config.AllowedCreatures.count(creature->GetEntry()))
        return true;

    SendRefusal(player, "deal with this NPC");
    return false;
}

bool CanOpenLoot(Player const* player, ObjectGuid lootGuid)
{
    if (!IsTournamentCharacter(player) || player->IsGameMaster())
        return true;

    // Its own items: containers, disenchanting, prospecting, milling. Gear on
    // the tournament vendors arrives in boxes.
    if (lootGuid.IsItem())
        return true;

    // Player bones are spoils of a fight between people, not PvE loot.
    if (lootGuid.IsCorpse())
        return true;

    // Everything inside a battleground or arena belongs to the match.
    if (MayReceiveGroupLoot(player))
        return true;

    if (lootGuid.IsGameObject() && !Config.AllowedLootObjects.empty())
        if (GameObject const* go = player->GetMap()->GetGameObject(lootGuid))
            if (Config.AllowedLootObjects.count(go->GetEntry()))
                return true;

    SendRefusal(player, "loot anything outside battlegrounds and arenas");
    return false;
}

bool MayReceiveGroupLoot(Player const* player)
{
    if (!IsTournamentCharacter(player) || player->IsGameMaster())
        return true;

    // ... and everything inside an open dungeon (Centurion.Tournament.OpenDungeons).
    Map const* map = player->FindMap();
    return map && (map->IsBattlegroundOrArena() || IsOpenDungeon(map->GetId()));
}

bool CanUseGuildBank(Player const* player)
{
    if (!IsTournamentCharacter(player) || player->IsGameMaster())
        return true;

    SendRefusal(player, "use the guild bank");
    return false;
}

uint8 GetStartLevel()
{
    return Config.StartLevel;
}

bool IsTournamentNameMarker(std::string const& rawName)
{
    std::wstring name;
    if (!Utf8toWStr(rawName, name) || name.size() < 2)
        return false;

    // A letter is lowercase when it has an uppercase form, and the other way
    // round; scripts without case never match, so they always create a world
    // character.
    auto const isLower = [](wchar_t c) { return wcharToUpper(c) != c; };
    auto const isUpper = [](wchar_t c) { return wcharToLower(c) != c; };

    if (!isLower(name[0]))
        return false;

    for (std::size_t i = 1; i < name.size(); ++i)
        if (!isUpper(name[i]))
            return false;

    return true;
}

static void StripForeignBloodFury(Player* player)
{
    if (!player || player->GetRace() == RACE_ORC)
        return;

    uint32 removedSpells = 0;
    for (uint32 spellId : OrcBloodFurySpells)
    {
        if (!player->HasSpell(spellId))
            continue;

        player->RemoveSpell(spellId, false, false);
        ++removedSpells;
    }

    uint32 removedButtons = 0;
    for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
    {
        ActionButton const* action = player->GetActionButton(button);
        if (!action || action->GetType() != ACTION_BUTTON_SPELL ||
            std::find(OrcBloodFurySpells.begin(), OrcBloodFurySpells.end(), action->GetAction()) == OrcBloodFurySpells.end())
            continue;

        player->removeActionButton(button);
        ++removedButtons;
    }

    if (removedSpells || removedButtons)
    {
        player->SendInitialActionButtons();
        TC_LOG_INFO("entities.player", "Tournament racial repair: removed {} foreign Blood Fury spell(s) and {} action button(s) from {} (race {}, class {}).",
            removedSpells, removedButtons, player->GetName(), uint32(player->GetRace()), uint32(player->GetClass()));
    }
}

void ApplyCharacterKit(Player* player)
{
    if (!IsTournamentCharacter(player))
        return;

    StripForeignBloodFury(player);

    uint32 const classMask = player->GetClassMask();
    for (InnateSpell const& innate : Config.InnateSpells)
    {
        if (innate.ClassMask && !(innate.ClassMask & classMask))
            continue;

        if (player->HasSpell(innate.SpellId))
            continue;

        if (!sSpellMgr->GetSpellInfo(innate.SpellId))
        {
            TC_LOG_ERROR("entities.player", "Centurion.Tournament.InnateSpells: spell {} does not exist.", innate.SpellId);
            continue;
        }

        player->LearnSpell(innate.SpellId, false);
    }

    // Tournament characters never visit a trainer, so they hold every rank
    // their talents bring, up to their level - including characters built from
    // a kit template or ported from Legionnaire+, whose talents never went
    // through LearnTalent here (Bloodthirst stayed at Rank 1 that way).
    player->LearnHeldTalentRanks();
}

void ClearCharacterKit(Player* player)
{
    if (!Config.Enabled || !player)
        return;

    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
    for (InnateSpell const& innate : Config.InnateSpells)
    {
        if (!player->HasSpell(innate.SpellId))
            continue;

        player->RemoveSpell(innate.SpellId, false, false);

        // In the same breath as the un-learn rather than at the character's next
        // save: a crash in between would otherwise hand the spell back at the
        // next login, with nothing left to say it was never the character's.
        CharacterDatabase.PExecute("DELETE FROM character_spell WHERE guid = {} AND spell = {}", guid, innate.SpellId);
    }
}

void ClearCharacterKit(ObjectGuid guid)
{
    if (!Config.Enabled || !guid.IsPlayer())
        return;

    for (InnateSpell const& innate : Config.InnateSpells)
        CharacterDatabase.PExecute("DELETE FROM character_spell WHERE guid = {} AND spell = {}", guid.GetCounter(), innate.SpellId);
}

void LoadCreateInfo()
{
    uint32 const oldMSTime = getMSTime();
    CreateInfoStore.clear();
    KitProcedureExists = false;

    // LoadConfig could only check HomeLocation's coordinates: the DBCs had not
    // loaded yet at startup.
    if (Config.HasHome && !MapManager::IsValidMapCoord(Config.HomeMap, Config.HomeX, Config.HomeY, Config.HomeZ, Config.HomeO))
    {
        TC_LOG_ERROR("server.loading", "Centurion.Tournament.HomeLocation: map {} does not exist on this realm.", Config.HomeMap);
        Config.HasHome = false;
    }

    // Probe before querying: a missing table aborts the server
    // (MySQLConnection, ER_NO_SUCH_TABLE), and only the Centurion realms have
    // these. Every other realm on the branch stops right here.
    std::unordered_set<std::string> tables;
    if (QueryResult result = WorldDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN "
        "('playercreateinfo_tournament', 'playercreateinfo_item_tournament', 'playercreateinfo_spell_custom_tournament', 'playercreateinfo_outfit_tournament')"))
    {
        do
            tables.insert(result->Fetch()[0].GetString());
        while (result->NextRow());
    }

    if (CharacterDatabase.Query("SELECT 1 FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() AND ROUTINE_TYPE = 'PROCEDURE' AND ROUTINE_NAME = 'createTournamentKit'"))
        KitProcedureExists = true;

    if (tables.empty())
    {
        TC_LOG_INFO("server.loading", ">> No tournament create data on this realm (playercreateinfo_*_tournament absent).");
        return;
    }

    bool const hasItems = tables.count("playercreateinfo_item_tournament") != 0;
    bool const hasSpells = tables.count("playercreateinfo_spell_custom_tournament") != 0;

    // An entry for every pair the world create data can make, so a table that
    // exists but has no row for a pair still replaces the world list - with an
    // empty one - rather than falling back to it.
    for (uint32 race = RACE_HUMAN; race < MAX_RACES; ++race)
    {
        for (uint32 playerClass = CLASS_WARRIOR; playerClass < MAX_CLASSES; ++playerClass)
        {
            if (!sObjectMgr->GetPlayerInfo(race, playerClass))
                continue;

            CreateInfo& info = CreateInfoStore[CreateInfoKey(race, playerClass)];
            info.HasItems = hasItems;
            info.HasCustomSpells = hasSpells;
        }
    }

    auto const forEachPair = [](uint32 race, uint32 playerClass, std::function<void(CreateInfo&)> const& apply)
    {
        for (auto& [key, info] : CreateInfoStore)
        {
            if ((race && uint32(key >> 8) != race) || (playerClass && uint32(key & 0xFF) != playerClass))
                continue;

            apply(info);
        }
    };

    uint32 positions = 0;
    if (tables.count("playercreateinfo_tournament"))
    {
        //                                                  0     1      2    3           4           5           6
        if (QueryResult result = WorldDatabase.Query("SELECT race, class, map, position_x, position_y, position_z, orientation FROM playercreateinfo_tournament"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const race = fields[0].GetUInt8();
                uint32 const playerClass = fields[1].GetUInt8();
                uint32 const mapId = fields[2].GetUInt16();
                float const x = fields[3].GetFloat();
                float const y = fields[4].GetFloat();
                float const z = fields[5].GetFloat();
                float const o = fields[6].GetFloat();

                // Legionnaire+ lists pairs this realm cannot create; those are
                // simply not needed here.
                auto itr = CreateInfoStore.find(CreateInfoKey(race, playerClass));
                if (itr == CreateInfoStore.end())
                    continue;

                if (!MapManager::IsValidMapCoord(mapId, x, y, z, o) || sMapStore.LookupEntry(mapId)->Instanceable())
                {
                    TC_LOG_ERROR("sql.sql", "Invalid or instanced start position for race {} class {} in `playercreateinfo_tournament`, ignoring.", race, playerClass);
                    continue;
                }

                CreateInfo& info = itr->second;
                info.HasPosition = true;
                info.MapId = mapId;
                info.PositionX = x;
                info.PositionY = y;
                info.PositionZ = z;
                info.Orientation = o;
                ++positions;
            }
            while (result->NextRow());
        }
    }

    uint32 items = 0;
    if (hasItems)
    {
        //                                                  0     1      2       3
        if (QueryResult result = WorldDatabase.Query("SELECT race, class, itemid, amount FROM playercreateinfo_item_tournament"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const race = fields[0].GetUInt8();
                uint32 const playerClass = fields[1].GetUInt8();
                uint32 const itemId = fields[2].GetUInt32();
                int32 const amount = fields[3].GetInt8();

                if (!sObjectMgr->GetItemTemplate(itemId))
                {
                    TC_LOG_ERROR("sql.sql", "Item {} (race {} class {}) in `playercreateinfo_item_tournament` does not exist, ignoring.", itemId, race, playerClass);
                    continue;
                }

                if (!amount)
                {
                    TC_LOG_ERROR("sql.sql", "Item {} (race {} class {}) in `playercreateinfo_item_tournament` has amount 0, ignoring.", itemId, race, playerClass);
                    continue;
                }

                // The world loader answers a negative amount by editing the DBC
                // starting outfit itself, for every character of the pair; a
                // tournament row may only take the item out of tournament
                // characters' outfits.
                forEachPair(race, playerClass, [&](CreateInfo& info)
                {
                    if (amount > 0)
                        info.Items.emplace_back(itemId, uint32(amount));
                    else
                        info.RemovedOutfitItems.push_back(itemId);
                });
                ++items;
            }
            while (result->NextRow());
        }
    }

    uint32 spells = 0;
    if (hasSpells)
    {
        //                                                  0          1          2
        if (QueryResult result = WorldDatabase.Query("SELECT racemask, classmask, Spell FROM playercreateinfo_spell_custom_tournament"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const raceMask = fields[0].GetUInt32();
                uint32 const classMask = fields[1].GetUInt32();
                uint32 const spellId = fields[2].GetUInt32();

                if ((raceMask && !(raceMask & RACEMASK_ALL_PLAYABLE)) || (classMask && !(classMask & CLASSMASK_ALL_PLAYABLE)))
                {
                    TC_LOG_ERROR("sql.sql", "Wrong race/class mask {}/{} for spell {} in `playercreateinfo_spell_custom_tournament`, ignoring.", raceMask, classMask, spellId);
                    continue;
                }

                if (!sSpellMgr->GetSpellInfo(spellId))
                {
                    TC_LOG_ERROR("sql.sql", "Spell {} in `playercreateinfo_spell_custom_tournament` does not exist, ignoring.", spellId);
                    continue;
                }

                for (auto& [key, info] : CreateInfoStore)
                {
                    uint32 const race = key >> 8;
                    uint32 const playerClass = key & 0xFF;
                    if ((raceMask && !(raceMask & (1 << (race - 1)))) || (classMask && !(classMask & (1 << (playerClass - 1)))))
                        continue;

                    info.CustomSpells.push_back(spellId);
                }
                ++spells;
            }
            while (result->NextRow());
        }
    }

    // Legionnaire+'s CharStartOutfit.dbc, which geared its characters at creation;
    // Centurion's own DBC outfits are Barracks+'s. Item ids are already mapped to
    // the tournament copies in the table.
    uint32 outfitItems = 0;
    if (tables.count("playercreateinfo_outfit_tournament"))
    {
        //                                                  0     1      2       3
        if (QueryResult result = WorldDatabase.Query("SELECT race, class, gender, itemid FROM playercreateinfo_outfit_tournament ORDER BY race, class, gender, slot"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const race = fields[0].GetUInt8();
                uint32 const playerClass = fields[1].GetUInt8();
                uint8 const gender = fields[2].GetUInt8();
                uint32 const itemId = fields[3].GetUInt32();

                // Pairs this realm cannot create are simply not needed.
                auto itr = CreateInfoStore.find(CreateInfoKey(race, playerClass));
                if (itr == CreateInfoStore.end())
                    continue;

                if (gender >= itr->second.Outfit.size())
                {
                    TC_LOG_ERROR("sql.sql", "Invalid gender {} for race {} class {} in `playercreateinfo_outfit_tournament`, ignoring.", uint32(gender), race, playerClass);
                    continue;
                }

                if (!sObjectMgr->GetItemTemplate(itemId))
                {
                    TC_LOG_ERROR("sql.sql", "Item {} (race {} class {} gender {}) in `playercreateinfo_outfit_tournament` does not exist, ignoring.", itemId, race, playerClass, uint32(gender));
                    continue;
                }

                itr->second.Outfit[gender].push_back(itemId);
                ++outfitItems;
            }
            while (result->NextRow());
        }
    }

    TC_LOG_INFO("server.loading", ">> Loaded tournament create data: {} start position(s), {} item row(s), {} spell row(s), {} outfit item(s) for {} race/class pair(s); kit procedure {} in {} ms",
        positions, items, spells, outfitItems, CreateInfoStore.size(), KitProcedureExists ? "present" : "ABSENT", GetMSTimeDiffToNow(oldMSTime));
}

CreateInfo const* GetCreateInfo(uint8 race, uint8 playerClass)
{
    auto itr = CreateInfoStore.find(CreateInfoKey(race, playerClass));
    return itr != CreateInfoStore.end() ? &itr->second : nullptr;
}

bool GetStartLocation(uint8 race, uint8 playerClass, WorldLocation& out)
{
    if (CreateInfo const* info = GetCreateInfo(race, playerClass))
    {
        if (info->HasPosition)
        {
            out.WorldRelocate(info->MapId, info->PositionX, info->PositionY, info->PositionZ, info->Orientation);
            return true;
        }
    }

    return GetHomeLocation(out);
}

std::vector<uint32> const* GetCustomSpells(Player const* player)
{
    if (!IsTournamentCharacter(player))
        return nullptr;

    CreateInfo const* info = GetCreateInfo(player->GetRace(), player->GetClass());
    return (info && info->HasCustomSpells) ? &info->CustomSpells : nullptr;
}

void RunKitProcedure(Player const* newCharacter)
{
    if (!newCharacter)
        return;

    if (!KitProcedureExists)
    {
        TC_LOG_ERROR("entities.player.character", "Tournament character {} ({}) was created without its template kit: the characters database has no createTournamentKit procedure.",
            newCharacter->GetName(), newCharacter->GetGUID().ToString());
        return;
    }

    // Pet numbers come from the server's generator, never from MAX(id)+1 in
    // SQL, which would race every pet tamed or summoned since the last save.
    // A hunter template holds at most its current pet and a full stable.
    std::string petIds;
    if (newCharacter->GetClass() == CLASS_HUNTER)
    {
        for (uint8 i = 0; i <= MAX_PET_STABLES; ++i)
        {
            if (i)
                petIds += ',';
            petIds += std::to_string(sObjectMgr->GeneratePetNumber());
        }
    }

    CharacterDatabase.DirectPExecute("CALL createTournamentKit({}, {}, {}, '{}')",
        uint32(newCharacter->GetClass()), uint32(newCharacter->GetRace()), newCharacter->GetGUID().GetCounter(), petIds);

    // The stored procedure copies an Orc class template before installing the
    // recipient's own racial action button. Do not leave the template's active
    // Blood Fury spell (or one of its copied buttons) on a non-Orc until their
    // first login repair.
    if (newCharacter->GetRace() != RACE_ORC)
    {
        uint32 const guid = newCharacter->GetGUID().GetCounter();
        CharacterDatabase.DirectPExecute(
            "DELETE FROM character_spell WHERE guid = {} AND spell IN ({}, {}, {})",
            guid, OrcBloodFurySpells[0], OrcBloodFurySpells[1], OrcBloodFurySpells[2]);
        CharacterDatabase.DirectPExecute(
            "DELETE FROM character_action WHERE guid = {} AND type = 0 AND action IN ({}, {}, {})",
            guid, OrcBloodFurySpells[0], OrcBloodFurySpells[1], OrcBloodFurySpells[2]);
    }
}

bool AlwaysMaxWeaponSkill(Player const* player)
{
    if (IsTournamentCharacter(player))
        return Config.MaxWeaponSkill;
    // Summoner's Rift: a world character's weapons as a tournament character's while
    // it plays a match; RiftProgression puts its real skills back afterwards.
    if (RiftProgression::UsesMatchSkills(player))
        return true;

    return sWorld->getBoolConfig(CONFIG_ALWAYS_MAXSKILL);
}

bool AlwaysMaxSkillForLevel(Player const* player)
{
    if (IsTournamentCharacter(player))
        return Config.MaxSkillForLevel;
    if (RiftProgression::UsesMatchSkills(player))
        return true;

    return sWorld->getBoolConfig(CONFIG_ALWAYS_MAX_SKILL_FOR_LEVEL);
}

namespace
{
    // The item links, indexed from both sides. Built whole and swapped in, never
    // edited in place: `.tournament reloaditems` runs on the world thread while
    // map threads are looking items up, so a reader takes a reference to the
    // store it started with and the replaced one dies with the last reader.
    struct ItemLinkStore
    {
        std::unordered_map<uint32, ItemLink> ByTournamentEntry;
        std::unordered_map<uint32, uint32> WorldToTournament;
    };

    std::mutex ItemLinkMutex;
    std::shared_ptr<ItemLinkStore const> ItemLinkData;

    std::shared_ptr<ItemLinkStore const> GetItemLinkData()
    {
        std::lock_guard<std::mutex> guard(ItemLinkMutex);
        return ItemLinkData;
    }
}

void LoadItemLinks()
{
    uint32 const oldMSTime = getMSTime();

    auto store = std::make_shared<ItemLinkStore>();
    auto const publish = [&store]()
    {
        std::shared_ptr<ItemLinkStore const> loaded = std::move(store);
        std::lock_guard<std::mutex> guard(ItemLinkMutex);
        ItemLinkData = std::move(loaded);
    };

    // Probe before querying: a missing table aborts the server (see
    // LoadCreateInfo), and only the Centurion realms have this one.
    if (!WorldDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'item_tournament_link'"))
    {
        publish();
        TC_LOG_INFO("server.loading", ">> No tournament item links on this realm (`item_tournament_link` absent).");
        return;
    }

    //                                                   0                 1            2
    if (QueryResult result = WorldDatabase.Query("SELECT tournament_entry, world_entry, crunched FROM item_tournament_link"))
    {
        do
        {
            Field* fields = result->Fetch();

            ItemLink link;
            link.TournamentEntry = fields[0].GetUInt32();
            link.WorldEntry = fields[1].IsNull() ? 0 : fields[1].GetUInt32();
            link.Crunched = fields[2].GetBool();

            if (!sObjectMgr->GetItemTemplate(link.TournamentEntry))
            {
                TC_LOG_ERROR("sql.sql", "Item {} in `item_tournament_link`.tournament_entry does not exist, ignoring.", link.TournamentEntry);
                continue;
            }

            if (link.WorldEntry == link.TournamentEntry)
            {
                TC_LOG_ERROR("sql.sql", "Item {} in `item_tournament_link` is its own twin, ignoring.", link.TournamentEntry);
                continue;
            }

            if (link.WorldEntry && !sObjectMgr->GetItemTemplate(link.WorldEntry))
            {
                TC_LOG_ERROR("sql.sql", "World item {} in `item_tournament_link` (tournament item {}) does not exist; the tournament item is kept unlinked.", link.WorldEntry, link.TournamentEntry);
                link.WorldEntry = 0;
            }

            // The table's unique key already forbids this; a realm whose table
            // was made by hand without it would otherwise get a silent winner.
            if (link.WorldEntry)
            {
                auto const inserted = store->WorldToTournament.try_emplace(link.WorldEntry, link.TournamentEntry);
                if (!inserted.second)
                {
                    TC_LOG_ERROR("sql.sql", "World item {} in `item_tournament_link` is claimed by tournament items {} and {}; keeping the first, leaving {} unlinked.", link.WorldEntry, inserted.first->second, link.TournamentEntry, link.TournamentEntry);
                    link.WorldEntry = 0;
                }
            }

            store->ByTournamentEntry[link.TournamentEntry] = link;
        }
        while (result->NextRow());
    }

    size_t const items = store->ByTournamentEntry.size();
    size_t const linked = store->WorldToTournament.size();
    publish();

    TC_LOG_INFO("server.loading", ">> Loaded {} tournament item(s), {} of them tied to a world item, in {} ms", items, linked, GetMSTimeDiffToNow(oldMSTime));
}

uint32 GetItemLinkCount()
{
    std::shared_ptr<ItemLinkStore const> const store = GetItemLinkData();
    return store ? uint32(store->ByTournamentEntry.size()) : 0;
}

Optional<ItemLink> GetItemLink(uint32 entry)
{
    std::shared_ptr<ItemLinkStore const> const store = GetItemLinkData();
    if (!store || !entry)
        return {};

    auto itr = store->ByTournamentEntry.find(entry);
    if (itr == store->ByTournamentEntry.end())
    {
        auto worldItr = store->WorldToTournament.find(entry);
        if (worldItr == store->WorldToTournament.end())
            return {};

        itr = store->ByTournamentEntry.find(worldItr->second);
        if (itr == store->ByTournamentEntry.end())
            return {};
    }

    return itr->second;
}

bool IsTournamentItem(uint32 entry)
{
    std::shared_ptr<ItemLinkStore const> const store = GetItemLinkData();
    return store && store->ByTournamentEntry.count(entry) != 0;
}

uint32 GetTournamentItem(uint32 worldEntry)
{
    std::shared_ptr<ItemLinkStore const> const store = GetItemLinkData();
    if (!store)
        return 0;

    auto itr = store->WorldToTournament.find(worldEntry);
    return itr != store->WorldToTournament.end() ? itr->second : 0;
}

uint32 GetWorldItem(uint32 tournamentEntry)
{
    std::shared_ptr<ItemLinkStore const> const store = GetItemLinkData();
    if (!store)
        return 0;

    auto itr = store->ByTournamentEntry.find(tournamentEntry);
    return itr != store->ByTournamentEntry.end() ? itr->second.WorldEntry : 0;
}

uint32 GetCounterpartItem(uint32 entry)
{
    std::shared_ptr<ItemLinkStore const> const store = GetItemLinkData();
    if (!store || !entry)
        return 0;

    auto itr = store->ByTournamentEntry.find(entry);
    if (itr != store->ByTournamentEntry.end())
        return itr->second.WorldEntry;

    auto worldItr = store->WorldToTournament.find(entry);
    return worldItr != store->WorldToTournament.end() ? worldItr->second : 0;
}

uint32 GetItemForMode(uint32 entry, bool tournamentMode)
{
    if (!entry)
        return 0;

    std::shared_ptr<ItemLinkStore const> const store = GetItemLinkData();
    if (!store)
        return tournamentMode ? 0 : entry;

    // A tournament item stays as it is in tournament mode and becomes its world
    // twin - if it has one - in world mode.
    auto itr = store->ByTournamentEntry.find(entry);
    if (itr != store->ByTournamentEntry.end())
        return tournamentMode ? entry : itr->second.WorldEntry;

    // Everything else is a world item, linked or not.
    if (!tournamentMode)
        return entry;

    auto worldItr = store->WorldToTournament.find(entry);
    return worldItr != store->WorldToTournament.end() ? worldItr->second : 0;
}

int32 GetDeathSicknessLevel(Player const* player)
{
    if (IsTournamentCharacter(player))
        return Config.DeathSicknessLevel;

    return int32(sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL));
}

bool ConsumesAmmo(Player const* player)
{
    if (IsTournamentCharacter(player))
        return Config.ConsumeAmmo;

    return sWorld->getBoolConfig(CONFIG_CENTURION_CLASSIC_CONSUME_AMMO);
}

bool PetHappinessDecays(Player const* owner)
{
    if (IsTournamentCharacter(owner))
        return Config.PetHappinessDecay;

    return sWorld->getBoolConfig(CONFIG_CENTURION_CLASSIC_PET_HAPPINESS_DECAY);
}

bool ResetsDuelCooldowns(Player const* a, Player const* b)
{
    if (IsTournamentCharacter(a) || IsTournamentCharacter(b))
        return Config.ResetDuelCooldowns;

    return sWorld->getBoolConfig(CONFIG_RESET_DUEL_COOLDOWNS);
}

bool ResetsDuelHealthMana(Player const* a, Player const* b)
{
    if (IsTournamentCharacter(a) || IsTournamentCharacter(b))
        return Config.ResetDuelHealthMana;

    return sWorld->getBoolConfig(CONFIG_RESET_DUEL_HEALTH_MANA);
}

uint32 GetWorldMaxHonorPoints(Player const* player)
{
    if (!Config.Enabled || !player || IsTournamentCharacter(player))
        return 0;

    return Config.WorldMaxHonorPoints;
}

uint32 GetWorldVendorCost(uint32 extendedCost)
{
    // A tier the list does not name, a gold price, or the feature switched off:
    // the row is served exactly as the database wrote it.
    if (!extendedCost)
        return extendedCost;

    if (ResolvedVendorCostRevision != Config.WorldPriceRevision)
        ResolveVendorCosts();

    auto const itr = ResolvedVendorCosts.find(extendedCost);
    return itr != ResolvedVendorCosts.end() ? itr->second : extendedCost;
}

uint32 FindHonorOnlyCost(uint32 honor, uint32* charged)
{
    if (!HonorOnlyCostsBuilt)
        BuildHonorOnlyCosts();

    auto const row = HonorOnlyCosts.lower_bound(honor);
    if (row == HonorOnlyCosts.end())
        return 0;

    if (charged)
        *charged = row->first;
    return row->second;
}

uint32 GetWorldQuestItemCount(uint32 questId, uint32 dbValue)
{
    if (!Config.WorldPricesEnabled)
        return dbValue;

    auto const itr = Config.WorldQuestSocks.find(questId);
    return itr != Config.WorldQuestSocks.end() ? itr->second : dbValue;
}

uint32 GetWorldPriceRevision()
{
    return Config.WorldPriceRevision;
}

bool IsInTournamentMatch(Player const* player)
{
    if (!Config.Enabled || !player)
        return false;

    Battleground const* battleground = player->GetBattleground();
    return battleground && battleground->PlaysByTournamentRules();
}

bool RefuseTalentChangeInMatch(Player* player, char const* what)
{
    if (!IsInTournamentMatch(player))
        return false;

    if (WorldSession* session = player->GetSession())
        if (!session->IsVirtualSession())
            session->SendNotification("You cannot %s in a tournament match.", what);

    return true;
}

bool RefuseTalentSpendInMatch(Player* player)
{
    if (Battleground const* battleground = player ? player->GetBattleground() : nullptr)
        if (battleground->GetTypeID() == BATTLEGROUND_VHR)
            return false;

    return RefuseTalentChangeInMatch(player, "spend talent points");
}

bool IsFreeReagentContext(Player const* player)
{
    if (!Config.WaiveReagents || !player)
        return false;

    // The tournament's matches only. A world-queue battleground or arena is
    // fought with the ammunition and the reagents the character brought, the
    // way the rest of the world is; the waiver is one of the things that makes
    // a tournament match a tournament match.
    if (IsInTournamentMatch(player))
        return true;

    return player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS;
}

bool HasReagentWaiver(Player const* player)
{
    if (Config.TournamentWaiveReagents && IsTournamentCharacter(player))
        return true;

    return IsFreeReagentContext(player);
}

bool IsReagentWaived(Player const* player, SpellInfo const* spellInfo)
{
    if (!spellInfo || !HasReagentWaiver(player))
        return false;

    // Combat spells only. The waiver is there so a fight is not decided by who
    // remembered their Blinding Powder - it must not turn a duel into a free
    // workshop, a free enchanter or a free portal.
    if (spellInfo->HasAttribute(SPELL_ATTR0_TRADESPELL))
        return false;

    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
    {
        switch (effect.Effect)
        {
            case SPELL_EFFECT_CREATE_ITEM:
            case SPELL_EFFECT_CREATE_ITEM_2:
            case SPELL_EFFECT_CREATE_RANDOM_ITEM:
            case SPELL_EFFECT_CREATE_MANA_GEM:
            case SPELL_EFFECT_ENCHANT_ITEM:
            case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
            case SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC:
            case SPELL_EFFECT_ENCHANT_HELD_ITEM:
            case SPELL_EFFECT_DISENCHANT:
            case SPELL_EFFECT_PROSPECTING:
            case SPELL_EFFECT_MILLING:
            case SPELL_EFFECT_TELEPORT_UNITS:
            case SPELL_EFFECT_TRANS_DOOR:
            case SPELL_EFFECT_SUMMON_OBJECT_WILD:
            case SPELL_EFFECT_SUMMON_PLAYER:
                return false;
            default:
                break;
        }
    }

    return true;
}

void SendRefusal(Player const* player, char const* what)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    // One notice per character per two seconds: several checks can refuse the
    // same click (a banker is looked up twice), and confinement retries every
    // update while a teleport is being set up. Handlers run on map threads,
    // hence the lock; this is only ever reached for a refusal.
    uint32 const nowMs = GameTime::GetGameTimeMS();
    {
        static std::mutex lock;
        static std::unordered_map<ObjectGuid::LowType, uint32> lastSentMs;

        std::lock_guard<std::mutex> guard(lock);
        uint32& last = lastSentMs[player->GetGUID().GetCounter()];
        if (last && getMSTimeDiff(last, nowMs) < 2000)
            return;
        last = nowMs;
    }

    player->GetSession()->SendNotification("Tournament characters cannot %s.", what);
}
}
