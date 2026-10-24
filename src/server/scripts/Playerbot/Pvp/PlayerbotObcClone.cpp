/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PlayerbotObcClone.h"

#include "PlayerbotRandomBotParticipation.h"
#include "AsyncCallbackProcessor.h"
#include "Custom/custom_barracks_hardcore.h"
#include "MotionMaster.h"
#include "Arena.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "Miscellaneous/Surnames.h"
#include "Configuration/Config.h"
#include "DataStores/DBCStores.h"
#include "DatabaseEnv.h"
#include "Duration.h"
#include "GameTime.h"
#include "Globals/ObjectAccessor.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "LoginQueryHolder.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr uint32 kBloodlustSpellId = 2825;
constexpr uint32 kBloodlustDurationMs = 60 * IN_MILLISECONDS;
constexpr uint32 kCloneTickThrottleMs = 1000;
// How long between attempts to find or found the copies' guild. The first
// attempt is on the first world tick, which is where the startup one happens.
constexpr uint32 kPvpBotGuildRetryMs = 60 * IN_MILLISECONDS;

struct ObcCloneConfig
{
    bool enabled = false;
    // Guild worn by every copy made in this file (Playerbot.Pvp.GuildName).
    // Empty leaves them guildless.
    std::string guildName;
};

struct ObcCloneRecord
{
    ObjectGuid cloneGuid;
    uint32 oppositeTeam = 0;
    uint32 battlegroundInstanceId = 0;
};

struct CustomGameCloneRecord
{
    ObjectGuid cloneGuid;
    ObjectGuid sourceGuid;
    uint32 team = 0;
    uint32 battlegroundInstanceId = 0;
    BattlegroundTypeId battlegroundType = BATTLEGROUND_TYPE_NONE;
};

struct CustomGameLobbyCloneRecord
{
    ObjectGuid cloneGuid;
    ObjectGuid sourceGuid;
    uint32 mapId = 0;
    uint32 lobbyInstanceId = 0;
    uint32 rosterSlotId = 0;
    uint32 team = 0;
    bool isPlayerbot = false;
};

struct WorldCloneRecord
{
    ObjectGuid cloneGuid;
    ObjectGuid sourceGuid;
    uint32 mapId = 0;
};

struct PendingCustomGameLobbyClone
{
    ObjectGuid sourceGuid;
    uint32 mapId = 0;
    uint32 lobbyInstanceId = 0;
    uint32 rosterSlotId = 0;
    uint32 team = 0;
    bool isPlayerbot = false;
    Position position;
    std::string displayPrefix;
};

std::mutex g_ObcCloneLock;
ObcCloneConfig g_ObcCloneConfig;

// The sessions are deliberately not registered with World. They only provide
// the Player-facing socketless session interface and are owned here for exactly
// as long as their in-memory clone exists.
std::unordered_map<ObjectGuid, std::unique_ptr<WorldSession>> g_CloneSessions;
std::unordered_map<ObjectGuid, ObcCloneRecord> g_ClonesByHuman;
std::unordered_map<ObjectGuid, ObjectGuid> g_HumanByClone;
std::unordered_map<ObjectGuid, CustomGameCloneRecord> g_CustomGameClones;
std::unordered_map<ObjectGuid, CustomGameLobbyCloneRecord> g_CustomGameLobbyClones;
std::vector<PendingCustomGameLobbyClone> g_PendingCustomGameLobbyClones;
std::unordered_map<ObjectGuid, WorldCloneRecord> g_WorldClones;

// Offline source loads answered on the world thread (LoadOfflineCloneSourceOnWorldThread).
// Only the world thread adds to it and drains it, so it takes no lock.
AsyncCallbackProcessor<SQLQueryHolderCallback> g_WorldThreadSourceLoads;

uint32 g_CloneTickAccumulatorMs = 0;

// The guild a copy wears when its source has none of its own. Read from map
// threads (copies are built there), written only by the world thread.
std::atomic<uint32> g_PvpBotGuildId{ 0 };
// A founder is being loaded; hold the next attempt rather than starting a
// second load for the same guild.
bool g_PvpBotGuildFounderPending = false;
uint32 g_PvpBotGuildRetryMs = kPvpBotGuildRetryMs;
// The "no PvP-only account is configured" notice, said once per uptime.
bool g_PvpBotGuildRosterNoticeSaid = false;

// Only a bot lends its guild to the copy made from it. A person does not: the
// "Dark <name>" mirror of a player is not theirs and has no business wearing
// their guild's tag at them.
bool SourceLendsItsGuild(Player const* source)
{
    if (!source || !source->GetGuildId() || !source->GetSession())
        return false;

    if (playerbot::PveManager::IsPvpOnlyBot(source))
        return true;

    // A snapshot taken under the population lock, so this is safe to ask from
    // the map threads the copies are built on.
    std::vector<uint32> const botAccounts = playerbot::RandomBotParticipationManager::GetConfiguredBotAccountIds();
    return std::binary_search(botAccounts.begin(), botAccounts.end(), source->GetSession()->GetAccountId());
}

// A copy has no character row, so it can never be a real guild member: joining
// it for real would write a guild_member row under a guid that does not exist
// and leave it behind when the copy goes. Writing the id into its guild field
// is all the client needs - it asks for the name with an ordinary guild query,
// which answers for anyone, member or not.
//
// The guild it writes is the one the copy's source really is in, so a copy and
// the bot behind it never wear different tags at the same moment: a world bot
// filling a sub-60 battleground keeps the PvE guild it is a member of, and the
// PvP-only characters bring the PvP guild in with them because that is the
// guild they joined. The PvP guild is the fallback for a source with no guild
// to lend - a human being mirrored, or a bot the PvE guild has not taken in yet.
void ApplyCloneGuild(Player* clone, Player const* source)
{
    if (!clone)
        return;

    // The field itself, not Player::SetInGuild: that also stamps the character
    // cache, which would be telling it a membership that does not exist - and
    // it is reached from map threads, where the cache is not ours to touch.
    if (SourceLendsItsGuild(source))
    {
        clone->SetUInt32Value(PLAYER_GUILDID, source->GetGuildId());
        clone->SetRank(source->GetRank());
        return;
    }

    uint32 const guildId = g_PvpBotGuildId.load(std::memory_order_relaxed);
    if (!guildId)
        return;

    clone->SetUInt32Value(PLAYER_GUILDID, guildId);
    clone->SetRank(GR_INITIATE);
}

// Guild membership is not something LoadFromDB carries - the login handler
// applies it, out of this same holder - so a source loaded for a copy rather
// than logged in has no guild until this puts one on it, and the copy made
// from it would fall back to the PvP guild while the character it was made
// from sits in another one. Fields only: this Player never enters the world.
void ApplyOfflineSourceGuild(Player* source, CharacterDatabaseQueryHolder const& holder)
{
    if (!source)
        return;

    if (PreparedQueryResult guildResult = holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_GUILD))
    {
        Field* guildFields = guildResult->Fetch();
        source->SetUInt32Value(PLAYER_GUILDID, guildFields[0].GetUInt32());
        source->SetRank(guildFields[1].GetUInt8());
    }
}

// The characters the copies are made from that exist only to be PvP bots. They
// are the ones joined for real: the first founds the guild and the rest fill
// its roster. Ordinary managed bots are deliberately left out - they belong to
// the PvE guild (Playerbot.Pve.GuildName), and a bot that has not logged in yet
// is guildless, so sweeping them in here would take them from it.
std::vector<ObjectGuid> SelectPvpBotGuildRoster()
{
    std::vector<uint32> const& accounts = playerbot::PveManager::GetConfig().pvpOnlyAccountIds;
    if (accounts.empty())
        return {};

    std::string accountList;
    for (uint32 accountId : accounts)
    {
        if (!accountList.empty())
            accountList += ',';
        accountList += std::to_string(accountId);
    }

    // Same shape as the battleground fill driver's source pool: the Obcc names
    // are the legacy on-disk Obsidian Colosseum clones, not real characters.
    QueryResult result = CharacterDatabase.PQuery(
        "SELECT guid FROM characters WHERE account IN ({}) AND deleteInfos_Account IS NULL "
        "AND name NOT LIKE 'Obcc%' ORDER BY guid", accountList);
    if (!result)
        return {};

    std::vector<ObjectGuid> roster;
    do
    {
        roster.push_back(ObjectGuid::Create<HighGuid::Player>((*result)[0].GetUInt32()));
    } while (result->NextRow());

    return roster;
}

void JoinPvpBotCharacters(Guild* guild, std::vector<ObjectGuid> const& roster)
{
    if (!guild)
        return;

    uint32 joined = 0;
    for (ObjectGuid guid : roster)
    {
        // Already somewhere - its own guild, or this one from an earlier run.
        if (sCharacterCache->GetCharacterGuildIdByGuid(guid))
            continue;

        CharacterDatabaseTransaction trans(nullptr);
        if (guild->AddMember(trans, guid))
            ++joined;
    }

    if (joined)
        TC_LOG_INFO("playerbots.pvp", "PvP bot guild '{}': {} character(s) joined.", guild->GetName(), joined);
}

void AdoptPvpBotGuild(Guild* guild, std::vector<ObjectGuid> const& roster)
{
    g_PvpBotGuildId.store(guild->GetId(), std::memory_order_relaxed);
    JoinPvpBotCharacters(guild, roster);
}

// World thread only. Cheap once the guild is known.
void EnsurePvpBotGuild()
{
    if (g_PvpBotGuildId.load(std::memory_order_relaxed) || g_PvpBotGuildFounderPending)
        return;

    std::string guildName;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        guildName = g_ObcCloneConfig.guildName;
    }

    if (guildName.empty())
        return;

    std::vector<ObjectGuid> roster = SelectPvpBotGuildRoster();

    if (Guild* guild = sGuildMgr->GetGuildByName(guildName))
    {
        TC_LOG_INFO("playerbots.pvp", "PvP bot guild '{}' (id {}) in use.", guildName, guild->GetId());
        AdoptPvpBotGuild(guild, roster);
        return;
    }

    // Guild::Create needs a real character to make guild master, and the
    // characters the copies stand in for are exactly that. It must be one that
    // is in no guild yet: Create writes the guild rows before it discovers its
    // master cannot join, and what it leaves behind is a guild with nobody in
    // it. Without such a character there is nothing to found a guild around, so
    // the copies stay guildless.
    ObjectGuid founderGuid;
    for (ObjectGuid guid : roster)
        if (!sCharacterCache->GetCharacterGuildIdByGuid(guid))
        {
            founderGuid = guid;
            break;
        }

    if (!founderGuid)
    {
        if (!g_PvpBotGuildRosterNoticeSaid)
        {
            g_PvpBotGuildRosterNoticeSaid = true;
            TC_LOG_INFO("playerbots.pvp", "PvP bot guild '{}' not created: no guildless character on Playerbot.Pve.PvpOnlyAccountIds to found it.",
                guildName);
        }
        return;
    }

    g_PvpBotGuildFounderPending = true;
    bool const queued = playerbot::PlayerbotObcCloneManager::LoadOfflineCloneSourceOnWorldThread(founderGuid,
        [guildName, roster](Player* founder)
    {
        g_PvpBotGuildFounderPending = false;
        if (!founder)
            return;

        // Another founder may have got there first between the load and its answer.
        if (Guild* existing = sGuildMgr->GetGuildByName(guildName))
        {
            AdoptPvpBotGuild(existing, roster);
            return;
        }

        // Guild::Create adds the leader as guild master itself.
        std::unique_ptr<Guild> created = std::make_unique<Guild>();
        if (!created->Create(founder, guildName))
            return;

        Guild* guild = created.release();
        sGuildMgr->AddGuild(guild);
        TC_LOG_INFO("playerbots.pvp", "Created PvP bot guild '{}' (id {}) with {} as guild master.",
            guildName, guild->GetId(), founder->GetName());
        AdoptPvpBotGuild(guild, roster);
    });

    if (!queued)
        g_PvpBotGuildFounderPending = false;
}

bool IsObcCloneFeatureConfigured()
{
    return g_ObcCloneConfig.enabled;
}

bool IsRunningArenaClone(CustomGameCloneRecord const& record)
{
    Battleground* bg = sBattlegroundMgr->GetBattleground(record.battlegroundInstanceId, record.battlegroundType);
    if (!bg || !bg->isArena())
        return false;

    BattlegroundStatus const status = bg->GetStatus();
    return status == STATUS_WAIT_JOIN || status == STATUS_IN_PROGRESS;
}

uint32 OppositeTeam(uint32 team)
{
    return team == ALLIANCE ? HORDE : ALLIANCE;
}

Battleground* GetHumanObcBattleground(Player* player)
{
    if (!player || !player->IsInWorld())
        return nullptr;

    WorldSession const* session = player->GetSession();
    if (!session || session->IsVirtualSession())
        return nullptr;

    if (playerbot::IsManagedRandomBot(player))
        return nullptr;

    Battleground* bg = player->GetBattleground();
    if (!bg || bg->GetTypeID(true) != BATTLEGROUND_OBC || bg->IsCustomGame())
        return nullptr;

    if (bg->GetStatus() == STATUS_WAIT_LEAVE || bg->GetStatus() == STATUS_NONE)
        return nullptr;

    return bg;
}

std::string GenerateCloneInternalName()
{
    static std::atomic<uint32> counter{ 0 };

    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        uint32 value = counter.fetch_add(1, std::memory_order_relaxed) ^ (GameTime::GetGameTimeMS() & 0xFFFFu);
        std::string name = "Obcm";
        for (int i = 0; i < 6; ++i)
        {
            name += static_cast<char>('a' + (value % 26));
            value /= 26;
        }

        if (!sCharacterCache->GetCharacterCacheByName(name))
            return name;
    }

    return std::string();
}

bool CopyEquipment(Player* clone, Player const* human)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item const* sourceItem = human->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!sourceItem)
            continue;

        Item* clonedItem = sourceItem->CloneItem(sourceItem->GetCount(), clone);
        if (!clonedItem)
            return false;

        for (uint8 enchantSlot = 0; enchantSlot < MAX_ENCHANTMENT_SLOT; ++enchantSlot)
        {
            EnchantmentSlot const slotId = EnchantmentSlot(enchantSlot);
            uint32 const enchantmentId = sourceItem->GetEnchantmentId(slotId);
            if (enchantmentId)
                clonedItem->SetEnchantment(slotId, enchantmentId, sourceItem->GetEnchantmentDuration(slotId),
                    sourceItem->GetEnchantmentCharges(slotId));
        }

        clonedItem->SetUInt32Value(ITEM_FIELD_DURABILITY, sourceItem->GetUInt32Value(ITEM_FIELD_DURABILITY));
        uint16 const destination = (uint16(INVENTORY_SLOT_BAG_0) << 8) | slot;
        clone->EquipItem(destination, clonedItem, false);
    }

    // Projectiles are not equipment, so the loop above never brings any ammo
    // over. Spell::CheckItems requires the active ammo item to physically
    // exist in the bags: on a miss it zeroes PLAYER_AMMO_ID and rejects every
    // ranged attack with SPELL_FAILED_NO_AMMO -- for spell 75 the core
    // neither interrupts nor resets the ranged timer on that failure, which
    // is invisible on a socketless clone and kept cloned hunters from ever
    // releasing Auto Shot. Stock a full match's worth of the human's ammo,
    // then SetAmmo() writes the field and applies the ammo DPS bonus.
    // Thori'dal-style no-ammo bows leave the source field at 0 and are
    // covered by their own equip aura (46699) instead.
    if (uint32 const sourceAmmoId = human->GetUInt32Value(PLAYER_AMMO_ID))
    {
        clone->StoreNewItemInBestSlots(sourceAmmoId, 2000);
        clone->SetAmmo(sourceAmmoId);
    }

    // The source's EMPTY slots come over as well, and a copy has no login or
    // resurrection of its own to be dressed at - so the field kit fills them
    // here, by the source's rules (a no-op where Centurion.Hardcore.Enable is
    // off). It has to be now: every caller seats the copy after this, and in a
    // battleground only weapons and trinkets may change, which is how copies
    // walked into Warsong Gulch with no chest and no bracers. After the ammo,
    // so a ranged weapon the kit picks suits what the copy carries.
    BarracksHardcore::IssueWhiteFieldKitToCopy(clone, human);

    return true;
}

void CopySkills(Player* clone, Player const* human)
{
    for (uint16 offset = 0; offset < PLAYER_MAX_SKILLS * 3; ++offset)
        clone->SetUInt32Value(PLAYER_SKILL_INFO_1_1 + offset, human->GetUInt32Value(PLAYER_SKILL_INFO_1_1 + offset));
}

void CopySpellsTalentsAndGlyphs(Player* clone, Player* human)
{
    // Replace the creation-template spellbook with the human's live spellbook.
    // AddSpell's learning path is entirely in memory and also applies passive
    // talent auras and spell modifiers.
    clone->RemoveAllAuras();
    clone->GetSpellMap().clear();
    clone->SetSpecsCount(1);
    clone->SetActiveSpec(0);

    for (auto const& [spellId, playerSpell] : human->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED)
            continue;

        clone->AddSpell(spellId, playerSpell.active, true, playerSpell.dependent, playerSpell.disabled);
        if (!playerSpell.disabled && GetTalentSpellCost(spellId) > 0)
            clone->AddTalent(spellId, 0, true);
    }

    clone->SetFreeTalentPoints(human->GetFreeTalentPoints());
    for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
    {
        clone->SetGlyphSlot(slot, human->GetGlyphSlot(slot));
        clone->SetGlyph(slot, human->GetGlyph(slot));
    }
    clone->ApplyGlyphAuras();
}

bool CreateHunterPetMirror(Player* clone, Pet* sourcePet)
{
    if (!clone || !sourcePet || sourcePet->getPetType() != HUNTER_PET)
        return true;

    CreatureTemplate const* creatureTemplate = sourcePet->GetCreatureTemplate();
    if (!creatureTemplate)
        return false;

    Map* map = clone->FindMap();
    if (!map || !clone->IsInWorld())
        return false;

    PetStable& petStable = clone->GetOrInitPetStable();
    if (petStable.CurrentPet)
        return false;

    std::unique_ptr<Pet> pet = std::make_unique<Pet>(clone, HUNTER_PET);
    if (!pet->CreateBaseAtCreatureInfo(creatureTemplate, clone))
        return false;

    pet->SetCreatorGUID(clone->GetGUID());
    pet->SetFaction(clone->GetFaction());
    pet->SetCreatedBySpell(sourcePet->GetUInt32Value(UNIT_CREATED_BY_SPELL));
    pet->ReplaceAllUnitFlags(UNIT_FLAG_PLAYER_CONTROLLED);

    uint8 const petLevel = sourcePet->GetLevel();
    if (!pet->InitStatsForLevel(petLevel))
        return false;

    uint32 const petNumber = sObjectMgr->GeneratePetNumber();
    pet->GetCharmInfo()->SetPetNumber(petNumber, true);
    pet->GetCharmInfo()->InitPetActionBar();
    pet->SetName(sourcePet->GetName());
    pet->SetNativeDisplayId(sourcePet->GetNativeDisplayId());
    pet->SetDisplayId(sourcePet->GetDisplayId());
    pet->SetReactState(sourcePet->GetReactState());
    pet->SetPetNameTimestamp(sourcePet->GetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP));
    pet->SetPetExperience(sourcePet->GetUInt32Value(UNIT_FIELD_PETEXPERIENCE));
    pet->SetPetNextLevelExperience(sourcePet->GetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP));

    pet->RemoveAllAuras();
    pet->m_spells.clear();
    pet->m_autospells.clear();
    pet->m_teachspells = sourcePet->m_teachspells;
    pet->m_usedTalentCount = 0;
    for (auto const& [spellId, petSpell] : sourcePet->m_spells)
    {
        if (petSpell.state == PETSPELL_REMOVED)
            continue;

        pet->addSpell(spellId, petSpell.active, PETSPELL_NEW, petSpell.type);
    }

    for (uint8 slot = ACTION_BAR_INDEX_START; slot < ACTION_BAR_INDEX_END; ++slot)
    {
        UnitActionBarEntry const* sourceAction = sourcePet->GetCharmInfo()->GetActionBarEntry(slot);
        pet->GetCharmInfo()->SetActionBar(slot, sourceAction->GetAction(), sourceAction->GetType());
    }

    pet->CastPetAuras(false);
    pet->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        if (pet->GetMaxPower(Powers(power)))
            pet->SetFullPower(Powers(power));
    pet->SetPower(POWER_HAPPINESS, sourcePet->GetPower(POWER_HAPPINESS));

    Pet* petRaw = pet.get();
    if (!map->AddToMap(petRaw->ToCreature()))
        return false;

    pet->FillPetInfo(&petStable.CurrentPet.emplace());
    clone->SetMinion(petRaw, true);
    pet.release();
    return true;
}

std::unordered_map<ObjectGuid, uint32> g_PendingHunterPetSpellQueryByClone;

// Applies the source's trained pet_spell rows (fetched asynchronously, since
// the CHAR_SEL_PET_SPELL statement is async-connection-only) to the clone's
// hunter pet once the query resolves. Re-validates that the clone still has
// the same pet this query was started for -- the periodic mirror sync could
// have replaced or removed it in the meantime (e.g. the source's real pet
// went live and CreateHunterPetMirror took over).
void ApplyLoadedHunterPetSpells(ObjectGuid cloneGuid, uint32 expectedClonePetNumber, PreparedQueryResult result)
{
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto itr = g_PendingHunterPetSpellQueryByClone.find(cloneGuid);
        if (itr != g_PendingHunterPetSpellQueryByClone.end() && itr->second == expectedClonePetNumber)
            g_PendingHunterPetSpellQueryByClone.erase(itr);
    }

    if (!result)
        return;

    Player* clone = ObjectAccessor::FindConnectedPlayer(cloneGuid);
    if (!clone || !clone->IsInWorld())
        return;

    Pet* pet = clone->GetPet();
    if (!pet || pet->getPetType() != HUNTER_PET || !pet->GetCharmInfo() ||
        pet->GetCharmInfo()->GetPetNumber() != expectedClonePetNumber)
        return;

    do
    {
        Field* fields = result->Fetch();
        pet->addSpell(fields[0].GetUInt32(), ActiveStates(fields[1].GetUInt8()), PETSPELL_UNCHANGED);
    }
    while (result->NextRow());

    // InitPetCreateSpells() already set up the baseline native action bar;
    // rebuild it now that the taught abilities are also known, so trained
    // spells actually show up as usable rather than just being in memory.
    pet->GetCharmInfo()->InitPetActionBar();
    pet->CastPetAuras(false);
}

// Builds a working hunter pet for the clone directly from the source's
// persisted pet stable data, without requiring the source to have a live,
// currently-summoned Pet at all. An online player's PetStable stays populated
// (their last-known current/unslotted pet) regardless of whether the pet is
// actually summoned right now, so this covers the common case where the
// source simply hasn't called their pet out yet this match.
//
// Trained pet_spell rows (Beast Training) are not part of PetStable::PetInfo
// and are loaded afterward via an async query keyed on the source's pet
// number (see ApplyLoadedHunterPetSpells); this function gives the mirror
// baseline native family abilities immediately (via Pet::InitPetCreateSpells)
// so it is not left with zero attacks while that query is in flight.
bool CreateHunterPetMirrorFromStable(Player* clone, PetStable::PetInfo const& info, uint32& outClonePetNumber)
{
    outClonePetNumber = 0;
    if (!clone || info.Type != HUNTER_PET || !info.CreatureId)
        return true;

    CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(info.CreatureId);
    if (!creatureTemplate)
        return false;

    Map* map = clone->FindMap();
    if (!map || !clone->IsInWorld())
        return false;

    PetStable& petStable = clone->GetOrInitPetStable();
    if (petStable.CurrentPet)
        return false;

    std::unique_ptr<Pet> pet = std::make_unique<Pet>(clone, HUNTER_PET);
    if (!pet->CreateBaseAtCreatureInfo(creatureTemplate, clone))
        return false;

    pet->SetCreatorGUID(clone->GetGUID());
    pet->SetFaction(clone->GetFaction());
    pet->SetCreatedBySpell(info.CreatedBySpellId);
    pet->ReplaceAllUnitFlags(UNIT_FLAG_PLAYER_CONTROLLED);

    uint8 const petLevel = info.Level > 0 ? info.Level : uint8(clone->GetLevel());
    if (!pet->InitStatsForLevel(petLevel))
        return false;

    uint32 const petNumber = sObjectMgr->GeneratePetNumber();
    outClonePetNumber = petNumber;
    pet->GetCharmInfo()->SetPetNumber(petNumber, true);
    if (!info.Name.empty())
        pet->SetName(info.Name);
    if (info.DisplayId)
    {
        pet->SetNativeDisplayId(info.DisplayId);
        pet->SetDisplayId(info.DisplayId);
    }
    if (info.ReactState)
        pet->SetReactState(info.ReactState);

    pet->InitPetCreateSpells();

    pet->CastPetAuras(false);
    pet->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        if (pet->GetMaxPower(Powers(power)))
            pet->SetFullPower(Powers(power));
    pet->SetPower(POWER_HAPPINESS, 166500);

    Pet* petRaw = pet.get();
    if (!map->AddToMap(petRaw->ToCreature()))
        return false;

    pet->FillPetInfo(&petStable.CurrentPet.emplace());
    clone->SetMinion(petRaw, true);
    pet.release();
    return true;
}

void SynchronizeHunterPetMirror(Player* human, Player* clone)
{
    if (!human || !clone || human->GetClass() != CLASS_HUNTER || clone->GetClass() != CLASS_HUNTER)
        return;

    // A dead hunter keeps no pet. This runs on a timer, so a mirror that the
    // core dismissed on death was simply resummoned on the next pass - a live
    // pet standing over its owner's corpse, never despawning. Mirror the
    // dismissal here and stay out until the clone is alive again; the normal
    // path below then rebuilds the pet on res.
    if (!clone->IsAlive())
    {
        if (Pet* orphanedPet = clone->GetPet())
            clone->RemovePet(orphanedPet, PET_SAVE_NOT_IN_SLOT);

        // A real hunter gets the pet back free at the spirit guide
        // (ResurrectPetAtSpiritGuide), which skips clones. Forget the old
        // record - dead or alive - so the dead-pet check below cannot leave
        // the clone petless after its own res; it gets a fresh mirror instead.
        if (PetStable* cloneStable = clone->GetPetStable())
            cloneStable->UnslottedPets.clear();
        return;
    }

    Pet* sourcePet = human->GetPet();
    if (sourcePet && sourcePet->getPetType() != HUNTER_PET)
        sourcePet = nullptr;

    // Resolve the stable fallback FIRST, because it is a legitimate source of
    // the mirror pet, not just a last resort at creation time.
    PetStable const* stable = human->GetPetStable();
    PetStable::PetInfo const* info = nullptr;
    if (!sourcePet && stable)
    {
        std::pair<PetStable::PetInfo const*, PetSaveMode> const loadInfo =
            Pet::GetLoadPetInfo(*stable, 0, 0, false);
        info = loadInfo.first;
        if (info && (info->Type != HUNTER_PET || info->Health == 0))
            info = nullptr;
    }

    // Which pet SHOULD the clone have? Deciding this before touching the pet
    // it already has is the whole point: the previous order removed the
    // clone's pet whenever the source had none *summoned*, then recreated it
    // from the stable on the very same pass. For a hunter whose pet lives in
    // the stable (every playerbot that has not called its pet out) that meant
    // destroy-and-respawn on every sync tick - which is exactly the pet
    // blinking in and out of existence with a flickering unit frame.
    uint32 const desiredEntry = sourcePet ? sourcePet->GetEntry()
                                          : (info ? info->CreatureId : 0);

    Pet* clonePet = clone->GetPet();
    if (clonePet && clonePet->GetEntry() != desiredEntry)   // 0 => none wanted
    {
        clone->RemovePet(clonePet, PET_SAVE_AS_DELETED);
        clonePet = nullptr;
    }

    // Already mirroring the right pet: leave it completely alone.
    if (clonePet)
        return;

    // No pet out, and the clone's own record says it died (its corpse
    // despawned, or it was parked dead by a mount): it stays dead until the
    // clone casts Revive Pet. Rebuilding here handed back a full-health pet
    // for free, which is what Call Pet did too before Pet::SavePetToDB kept
    // a clone's record current.
    if (PetStable* cloneStable = clone->GetPetStable())
    {
        PetStable::PetInfo const* ownInfo = Pet::GetLoadPetInfo(*cloneStable, 0, 0, false).first;
        if (ownInfo && ownInfo->Type == HUNTER_PET && !ownInfo->Health)
            return;

        // Whatever is left over is an earlier mirror. Call Pet and Revive Pet
        // load the OLDEST unslotted pet, so a stale live one there would be
        // a free resurrection of its own; the clone only ever owns one pet.
        cloneStable->UnslottedPets.clear();
    }

    if (sourcePet)
    {
        CreateHunterPetMirror(clone, sourcePet);
        return;
    }

    if (!info)
        return;

    uint32 const sourcePetNumber = info->PetNumber;
    uint32 clonePetNumber = 0;
    if (!CreateHunterPetMirrorFromStable(clone, *info, clonePetNumber) || !clonePetNumber)
        return;

    // Fetch the source's trained pet_spell rows (Beast Training) so the
    // mirror ends up with the abilities the hunter actually taught it, not
    // just baseline native attacks. CHAR_SEL_PET_SPELL is async-connection
    // only, so this cannot be a blocking Query() call.
    ObjectGuid const cloneGuid = clone->GetGUID();
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto& pending = g_PendingHunterPetSpellQueryByClone[cloneGuid];
        if (pending == clonePetNumber)
            return;
        pending = clonePetNumber;
    }

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_PET_SPELL);
    stmt->setUInt32(0, sourcePetNumber);
    if (WorldSession* session = human->GetSession())
    {
        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(stmt).WithPreparedCallback(
            [cloneGuid, clonePetNumber](PreparedQueryResult result)
            {
                ApplyLoadedHunterPetSpells(cloneGuid, clonePetNumber, result);
            }));
    }
}

void DestroyUnseatedClone(std::unique_ptr<WorldSession>& session, Player* clone)
{
    if (!clone)
        return;

    // Offline clone sources are populated through Player::LoadFromDB, which
    // restores persistent auras and may establish cross-unit references even
    // though the temporary Player is never added to the world. Run the same
    // pre-destruction cleanup used by normal logout/map removal before the
    // Unit destructor asserts that every applied aura has been detached.
    if (clone->GetGUID())
        clone->CleanupsBeforeDelete();

    delete clone;
    session->SetPlayer(nullptr);
}

bool ProvisionCloneForHuman(Player* human, Battleground* bg)
{
    if (!human || !bg)
        return false;

    ObjectGuid const humanGuid = human->GetGUID();
    uint32 const oppositeTeam = OppositeTeam(human->GetBGTeam());
    std::string const internalName = GenerateCloneInternalName();
    if (internalName.empty())
        return false;

    uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
    auto session = std::make_unique<WorldSession>(0, "obc_in_memory_clone", nullptr, SEC_PLAYER, expansion, 0,
        Minutes(0), LOCALE_enUS, 0, false);
    session->SetTransientPlayerSession();

    Player* clone = new Player(session.get());

    CharacterCreateInfo createInfo;
    createInfo.SetName(internalName)
        .SetRace(human->GetRace())
        .SetClass(human->GetClass())
        .SetGender(human->GetNativeGender())
        .SetSkin(human->GetSkinId())
        .SetFace(human->GetFaceId())
        .SetHairStyle(human->GetHairStyleId())
        .SetHairColor(human->GetHairColorId())
        .SetFacialHair(human->GetFacialStyle())
        .SetOutfitId(0);

    ObjectGuid::LowType const cloneLowGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
    // This mirrors an existing character's live appearance as it is. Even the
    // barbershop check refuses some of them (a non-death-knight wearing a
    // death-knight face, a tauren hair style CharSections lacks), and every
    // copy refused that way silently left a seat empty.
    if (!clone->Create(cloneLowGuid, &createInfo, false, Player::AppearanceCheck::None))
    {
        DestroyUnseatedClone(session, clone);
        return false;
    }

    // Player::Create initializes display IDs before it writes PLAYER_BYTES_3,
    // which is where Player::GetNativeGender reads from. Persistent characters
    // get another display initialization during loading, but these transient
    // clones do not, so explicitly synchronize gender and rebuild the native
    // model after creation.
    Gender const nativeGender = human->GetNativeGender();
    clone->SetGender(nativeGender);
    clone->SetNativeGender(nativeGender);
    clone->InitDisplayIds();
    // Server-created player: LoadFromDB never runs, so PlayerSocial is
    // null unless we make one. Channel::SendToAll and friends call
    // GetSocial()->HasIgnore() on every player they touch.
    clone->EnsureSocial();

    // WorldSession::SetPlayer reads the player's GUID immediately. Player's
    // update-field storage and GUID do not exist until Player::Create succeeds.
    session->SetPlayer(clone);

    ApplyCloneGuild(clone, human);

    clone->GetMotionMaster()->Initialize();
    clone->SetLevel(human->GetLevel(), false);
    clone->InitStatsForLevel();
    clone->InitTalentForLevel();
    clone->InitGlyphsForLevel();
    CopySkills(clone, human);
    CopySpellsTalentsAndGlyphs(clone, human);

    if (!CopyEquipment(clone, human))
    {
        DestroyUnseatedClone(session, clone);
        return false;
    }

    // Player::LoadFromDB turns this on before its own UpdateAllStats, and a
    // server-built clone never runs LoadFromDB - so without it Unit::UpdateUnitMod
    // returns early for the rest of the clone's life and every LATER stat or
    // damage percent modifier (arena preparation buffs, the Violet Hold
    // Diminished debuff, copied boons, Reinforced) is stored and never applied.
    clone->SetCanModifyStats(true);
    clone->UpdateAllStats();
    clone->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        clone->SetFullPower(Powers(power));

    BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_OBC, 0);
    Position const* start = bg->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(oppositeTeam));
    if (queueTypeId == BATTLEGROUND_QUEUE_NONE || !start ||
        clone->AddBattlegroundQueueId(queueTypeId) >= PLAYER_MAX_BATTLEGROUND_QUEUES)
    {
        DestroyUnseatedClone(session, clone);
        return false;
    }

    clone->SetInviteForBattlegroundQueueType(queueTypeId, bg->GetInstanceID());
    clone->SetBattlegroundId(bg->GetInstanceID(), BATTLEGROUND_OBC);
    clone->SetFactionForRace(oppositeTeam == HORDE ? RACE_BLOODELF : RACE_HUMAN);
    clone->SetBGTeam(oppositeTeam);
    clone->ResetMap();
    clone->Relocate(*start);
    clone->SetMap(bg->GetBgMap());

    bg->IncreaseInvitedCount(oppositeTeam);
    if (!bg->GetBgMap()->AddPlayerToMap(clone))
    {
        bg->DecreaseInvitedCount(oppositeTeam);
        DestroyUnseatedClone(session, clone);
        return false;
    }

    ObjectGuid const cloneGuid = clone->GetGUID();
    std::string const displayName = "Dark " + human->GetName();
    sCharacterCache->AddCharacterCacheEntry(cloneGuid, 0, displayName, clone->GetNativeGender(), clone->GetRace(),
        clone->GetClass(), clone->GetLevel());
    // It plays under the mirrored player's name, so it wears their family name
    // too (Miscellaneous/Surnames.h).
    Surnames::AdoptTransient(cloneGuid, human->GetGUID());
    ObjectAccessor::AddObject(clone);
    bg->AddPlayer(clone);
    // Battleground::AddPlayer runs UnsummonPetTemporaryIfAny() on everyone who
    // enters, which records a "temporarily unsummoned" pet number for later
    // restoration. That restore path (ResummonPetTemporaryUnSummonedIfAny) is
    // DB-backed - it calls Pet::LoadPetFromDB - and a mirror pet only ever
    // exists in memory under a clone GUID with no character_pet rows behind
    // it. Leaving the number set makes the arena machinery chase a pet that
    // cannot be loaded while SynchronizeHunterPetMirror independently builds
    // the real mirror, so the two fight over one pet slot. The mirror is the
    // only owner of this clone's pet: clear the core's claim on it.
    clone->SetTemporaryUnsummonedPetNumber(0);
    clone->SetInGameTime(GameTime::GetGameTimeMS());
    SynchronizeHunterPetMirror(human, clone);

    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_CloneSessions.emplace(cloneGuid, std::move(session));
        g_ClonesByHuman[humanGuid] = { cloneGuid, oppositeTeam, bg->GetInstanceID() };
        g_HumanByClone[cloneGuid] = humanGuid;
    }

    TC_LOG_INFO("playerbots.population", "OBC clone: created in-memory '{}' ({}) for human {} in bg instance {}.",
        displayName, cloneGuid.ToString(), humanGuid.ToString(), bg->GetInstanceID());
    return true;
}

void TeardownCloneForHuman(ObjectGuid humanGuid)
{
    ObcCloneRecord record;
    std::unique_ptr<WorldSession> session;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto itr = g_ClonesByHuman.find(humanGuid);
        if (itr == g_ClonesByHuman.end())
            return;

        record = itr->second;
        g_ClonesByHuman.erase(itr);
        g_HumanByClone.erase(record.cloneGuid);

        auto sessionItr = g_CloneSessions.find(record.cloneGuid);
        if (sessionItr != g_CloneSessions.end())
        {
            session = std::move(sessionItr->second);
            g_CloneSessions.erase(sessionItr);
        }
    }

    Player* clone = session ? session->GetPlayer() : nullptr;
    if (clone)
    {
        if (Battleground* bg = sBattlegroundMgr->GetBattleground(record.battlegroundInstanceId, BATTLEGROUND_OBC))
            if (bg->IsPlayerInBattleground(record.cloneGuid))
                bg->RemovePlayerAtLeave(record.cloneGuid, false, false);

        std::string cachedName;
        sCharacterCache->GetCharacterNameByGuid(record.cloneGuid, cachedName);

        if (Map* map = clone->FindMap())
            map->RemovePlayerFromMap(clone, true);
        else
        {
            ObjectAccessor::RemoveObject(clone);
            delete clone;
        }
        session->SetPlayer(nullptr);

        if (!cachedName.empty())
            sCharacterCache->DeleteCharacterCacheEntry(record.cloneGuid, cachedName);
            Surnames::ForgetTransient(record.cloneGuid);
    }

    TC_LOG_INFO("playerbots.population", "OBC clone: destroyed in-memory clone {} for human {}.",
        record.cloneGuid.ToString(), humanGuid.ToString());
}

void TeardownAllClones()
{
    std::vector<ObjectGuid> humanGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        humanGuids.reserve(g_ClonesByHuman.size());
        for (auto const& [humanGuid, record] : g_ClonesByHuman)
            humanGuids.push_back(humanGuid);
    }

    for (ObjectGuid const& humanGuid : humanGuids)
        TeardownCloneForHuman(humanGuid);
}

void TeardownCustomGameClone(ObjectGuid cloneGuid)
{
    CustomGameCloneRecord record;
    std::unique_ptr<WorldSession> session;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto recordItr = g_CustomGameClones.find(cloneGuid);
        if (recordItr == g_CustomGameClones.end())
            return;

        record = recordItr->second;
        g_CustomGameClones.erase(recordItr);

        auto sessionItr = g_CloneSessions.find(cloneGuid);
        if (sessionItr != g_CloneSessions.end())
        {
            session = std::move(sessionItr->second);
            g_CloneSessions.erase(sessionItr);
        }
    }

    Player* clone = session ? session->GetPlayer() : nullptr;
    if (!clone)
        return;

    playerbot::RandomBotParticipationManager::OnPlayerLogout(clone);

    if (Battleground* bg = sBattlegroundMgr->GetBattleground(record.battlegroundInstanceId, record.battlegroundType))
        if (bg->IsPlayerInBattleground(cloneGuid))
            bg->RemovePlayerAtLeave(cloneGuid, false, false);

    std::string cachedName;
    sCharacterCache->GetCharacterNameByGuid(cloneGuid, cachedName);
    if (Map* map = clone->FindMap())
        map->RemovePlayerFromMap(clone, true);
    else
    {
        ObjectAccessor::RemoveObject(clone);
        delete clone;
    }
    session->SetPlayer(nullptr);

    if (!cachedName.empty())
        sCharacterCache->DeleteCharacterCacheEntry(cloneGuid, cachedName);
        Surnames::ForgetTransient(cloneGuid);
}

void TeardownAllCustomGameClones()
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        cloneGuids.reserve(g_CustomGameClones.size());
        for (auto const& [cloneGuid, record] : g_CustomGameClones)
            cloneGuids.push_back(cloneGuid);
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownCustomGameClone(cloneGuid);
}

bool MatchesLobbyClone(CustomGameLobbyCloneRecord const& record, uint32 lobbyInstanceId, uint32 rosterSlotId)
{
    return record.lobbyInstanceId == lobbyInstanceId && record.rosterSlotId == rosterSlotId;
}

bool MatchesLobbyClone(PendingCustomGameLobbyClone const& record, uint32 lobbyInstanceId, uint32 rosterSlotId)
{
    return record.lobbyInstanceId == lobbyInstanceId && record.rosterSlotId == rosterSlotId;
}

bool HasCustomGameLobbyClone(uint32 lobbyInstanceId, uint32 rosterSlotId)
{
    std::lock_guard<std::mutex> lock(g_ObcCloneLock);
    for (auto const& [cloneGuid, record] : g_CustomGameLobbyClones)
        if (MatchesLobbyClone(record, lobbyInstanceId, rosterSlotId))
            return true;

    return std::any_of(g_PendingCustomGameLobbyClones.begin(), g_PendingCustomGameLobbyClones.end(),
        [&](PendingCustomGameLobbyClone const& record)
        {
            return MatchesLobbyClone(record, lobbyInstanceId, rosterSlotId);
        });
}

Player* CreateCustomGameLobbyClone(Player* source, uint32 mapId, uint32 lobbyInstanceId, uint32 team,
    uint32 rosterSlotId, bool isPlayerbot, Position const& position, std::string const& displayPrefix)
{
    if (!source || (team != ALLIANCE && team != HORDE))
        return nullptr;

    Map* lobbyMap = sMapMgr->FindMap(mapId, lobbyInstanceId);
    if (!lobbyMap || HasCustomGameLobbyClone(lobbyInstanceId, rosterSlotId))
        return nullptr;

    std::string const internalName = GenerateCloneInternalName();
    if (internalName.empty())
        return nullptr;

    uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
    auto session = std::make_unique<WorldSession>(0, "custom_game_lobby_preview", nullptr, SEC_PLAYER, expansion, 0,
        Minutes(0), LOCALE_enUS, 0, false);
    session->SetTransientPlayerSession();

    Player* clone = new Player(session.get());
    CharacterCreateInfo createInfo;
    createInfo.SetName(internalName)
        .SetRace(source->GetRace()).SetClass(source->GetClass()).SetGender(source->GetNativeGender())
        .SetSkin(source->GetSkinId()).SetFace(source->GetFaceId()).SetHairStyle(source->GetHairStyleId())
        .SetHairColor(source->GetHairColorId()).SetFacialHair(source->GetFacialStyle()).SetOutfitId(0);

    ObjectGuid::LowType const cloneLowGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
    if (!clone->Create(cloneLowGuid, &createInfo, false, Player::AppearanceCheck::None))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    clone->SetGender(source->GetNativeGender());
    clone->SetNativeGender(source->GetNativeGender());
    clone->InitDisplayIds();
    // Server-created player: LoadFromDB never runs, so PlayerSocial is
    // null unless we make one. Channel::SendToAll and friends call
    // GetSocial()->HasIgnore() on every player they touch.
    clone->EnsureSocial();
    session->SetPlayer(clone);

    // Roster mannequins stand in for people too; only the ones standing in
    // for a bot wear a bot's guild.
    if (isPlayerbot)
        ApplyCloneGuild(clone, source);

    clone->GetMotionMaster()->Initialize();
    clone->SetLevel(source->GetLevel(), false);
    clone->InitStatsForLevel();
    clone->InitTalentForLevel();
    clone->InitGlyphsForLevel();
    CopySkills(clone, source);
    CopySpellsTalentsAndGlyphs(clone, source);
    if (!CopyEquipment(clone, source))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    // Player::LoadFromDB turns this on before its own UpdateAllStats, and a
    // server-built clone never runs LoadFromDB - so without it Unit::UpdateUnitMod
    // returns early for the rest of the clone's life and every LATER stat or
    // damage percent modifier (arena preparation buffs, the Violet Hold
    // Diminished debuff, copied boons, Reinforced) is stored and never applied.
    clone->SetCanModifyStats(true);
    clone->UpdateAllStats();
    clone->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        clone->SetFullPower(Powers(power));

    clone->SetBGTeam(team);
    // Lobby previews are roster mannequins: players must be able to target them,
    // see their names, and inspect their copied gear/talents.  A friendly faction
    // exposes the normal player interaction menu regardless of the source race;
    // the team flag remains the visual indication of the assigned custom team.
    clone->SetFaction(FACTION_FRIENDLY);
    clone->SetUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
    clone->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_ATTACKABLE_1 |
        UNIT_FLAG_NON_ATTACKABLE_2 | UNIT_FLAG_UNINTERACTIBLE);
    clone->ClearUnitState(UNIT_STATE_UNATTACKABLE);
    clone->AddUnitState(UNIT_STATE_ROOT);

    // Attach the team flag visual BEFORE the mannequin becomes visible, so
    // observers build the character and its attached flag model from a single
    // update. Applying it after AddPlayerToMap made every client rebuild the
    // composited model a second time, and a picking raycast landing inside
    // that rebuild is the leading suspect for the lobby client crash at
    // WoW.exe+0x41D559 (submesh index below its batch base vertex).
    // AddAura, not CastSpell: the mannequin is not in the world yet.
    uint32 const teamFlagSpell = team == ALLIANCE ? 32609 : 32610;
    clone->AddAura(teamFlagSpell, clone);

    clone->SetWorldSubMap(mapId, lobbyInstanceId);
    clone->ResetMap();
    clone->Relocate(position);
    clone->SetMap(lobbyMap);
    if (!lobbyMap->AddPlayerToMap(clone))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    ObjectGuid const cloneGuid = clone->GetGUID();
    std::string displayName = displayPrefix + source->GetName();
    sCharacterCache->AddCharacterCacheEntry(cloneGuid, 0, displayName, clone->GetNativeGender(), clone->GetRace(),
        clone->GetClass(), clone->GetLevel(), false);
    Surnames::AdoptTransient(cloneGuid, source->GetGUID());
    ObjectAccessor::AddObject(clone);
    clone->SetInGameTime(GameTime::GetGameTimeMS());
    // Fallback only: the pre-map AddAura above is the normal path, so this
    // fires just if applying the aura off-map was refused. Casting it here
    // costs observers a second model rebuild, which is what we are avoiding.
    if (!clone->HasAura(teamFlagSpell))
        clone->CastSpell(clone, teamFlagSpell, true);
    // Reassert this after the flag aura is applied as well. Any aura-side
    // targeting state must not turn the roster mannequin into scenery.
    clone->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_ATTACKABLE_1 |
        UNIT_FLAG_NON_ATTACKABLE_2 | UNIT_FLAG_UNINTERACTIBLE);
    clone->ClearUnitState(UNIT_STATE_UNATTACKABLE);

    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_CloneSessions.emplace(cloneGuid, std::move(session));
        g_CustomGameLobbyClones.emplace(cloneGuid, CustomGameLobbyCloneRecord{
            cloneGuid, source->GetGUID(), mapId, lobbyInstanceId, rosterSlotId, team, isPlayerbot });
    }

    // Spawn record for the lobby crash hunt: client crash reports carry a
    // wall-clock timestamp, so this is what lets a crash be matched to the
    // exact mannequin that was materialising at that moment. Everything the
    // client has to build a model from is listed.
    {
        std::string gear;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item const* worn = clone->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!worn)
                continue;
            if (!gear.empty())
                gear += ",";
            gear += std::to_string(slot) + ":" + std::to_string(worn->GetEntry());
        }

        TC_LOG_INFO("custom.lobby", "MANNEQUIN spawn name='{}' source='{}' srcGuid={} "
            "race={} gender={} class={} level={} team={} playerbot={} map={} inst={} "
            "pos=({:.2f},{:.2f},{:.2f}) flagSpell={} flagApplied={} gear=[{}]",
            displayName, source->GetName(), source->GetGUID().ToString(),
            uint32(clone->GetRace()), uint32(clone->GetNativeGender()),
            uint32(clone->GetClass()), uint32(clone->GetLevel()), team,
            isPlayerbot ? 1 : 0, mapId, lobbyInstanceId,
            position.GetPositionX(), position.GetPositionY(), position.GetPositionZ(),
            teamFlagSpell, clone->HasAura(teamFlagSpell) ? 1 : 0, gear);
    }

    return clone;
}

void TeardownCustomGameLobbyClone(ObjectGuid cloneGuid)
{
    std::unique_ptr<WorldSession> session;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto recordItr = g_CustomGameLobbyClones.find(cloneGuid);
        if (recordItr == g_CustomGameLobbyClones.end())
            return;
        g_CustomGameLobbyClones.erase(recordItr);

        auto sessionItr = g_CloneSessions.find(cloneGuid);
        if (sessionItr != g_CloneSessions.end())
        {
            session = std::move(sessionItr->second);
            g_CloneSessions.erase(sessionItr);
        }
    }

    Player* clone = session ? session->GetPlayer() : nullptr;
    if (!clone)
        return;

    std::string cachedName;
    sCharacterCache->GetCharacterNameByGuid(cloneGuid, cachedName);
    if (Map* map = clone->FindMap())
        map->RemovePlayerFromMap(clone, true);
    else
    {
        ObjectAccessor::RemoveObject(clone);
        delete clone;
    }
    session->SetPlayer(nullptr);

    if (!cachedName.empty())
        sCharacterCache->DeleteCharacterCacheEntry(cloneGuid, cachedName);
        Surnames::ForgetTransient(cloneGuid);
}

void TeardownAllCustomGameLobbyClones()
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        cloneGuids.reserve(g_CustomGameLobbyClones.size());
        for (auto const& [cloneGuid, record] : g_CustomGameLobbyClones)
            cloneGuids.push_back(cloneGuid);
        g_PendingCustomGameLobbyClones.clear();
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownCustomGameLobbyClone(cloneGuid);
}

void TeardownWorldClone(ObjectGuid cloneGuid)
{
    std::unique_ptr<WorldSession> session;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto recordItr = g_WorldClones.find(cloneGuid);
        if (recordItr == g_WorldClones.end())
            return;
        g_WorldClones.erase(recordItr);
        // A hunter pet mirrored from an offline source asks for its trained
        // spells on the source's session, which is gone before the answer.
        g_PendingHunterPetSpellQueryByClone.erase(cloneGuid);

        auto sessionItr = g_CloneSessions.find(cloneGuid);
        if (sessionItr != g_CloneSessions.end())
        {
            session = std::move(sessionItr->second);
            g_CloneSessions.erase(sessionItr);
        }
    }

    Player* clone = session ? session->GetPlayer() : nullptr;
    if (!clone)
        return;

    playerbot::RandomBotParticipationManager::OnPlayerLogout(clone);

    std::string cachedName;
    sCharacterCache->GetCharacterNameByGuid(cloneGuid, cachedName);
    if (Map* map = clone->FindMap())
        map->RemovePlayerFromMap(clone, true);
    else
    {
        ObjectAccessor::RemoveObject(clone);
        delete clone;
    }
    session->SetPlayer(nullptr);

    if (!cachedName.empty())
        sCharacterCache->DeleteCharacterCacheEntry(cloneGuid, cachedName);
        Surnames::ForgetTransient(cloneGuid);
}

void TeardownAllWorldClones()
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        cloneGuids.reserve(g_WorldClones.size());
        for (auto const& [cloneGuid, record] : g_WorldClones)
            cloneGuids.push_back(cloneGuid);
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownWorldClone(cloneGuid);
}
}

namespace playerbot
{
void PlayerbotObcCloneManager::LoadConfig()
{
    int32 const legacyEnableValue = std::max<int32>(0,
        sConfigMgr->GetIntDefault("Playerbot.PvpLifecycle.ObsidianColosseum.CloneAccountId", 0));

    std::string guildName = sConfigMgr->GetStringDefault("Playerbot.Pvp.GuildName", "The Robot Masters");
    // Held to the same standard as the guild a person could found: a name the
    // database column cannot hold, or one the realm reserves, is refused here
    // rather than at the INSERT.
    if (!guildName.empty() && (sObjectMgr->IsReservedName(guildName) || !sObjectMgr->IsValidCharterName(guildName)))
    {
        TC_LOG_ERROR("server.loading", "Playerbot.Pvp.GuildName '{}' is not a usable guild name; PvP copies stay guildless.",
            guildName);
        guildName.clear();
    }

    std::lock_guard<std::mutex> lock(g_ObcCloneLock);
    g_ObcCloneConfig.enabled = sConfigMgr->GetBoolDefault(
        "Playerbot.PvpLifecycle.ObsidianColosseum.Clone.Enable", legacyEnableValue != 0);

    // A renamed guild is a different guild: drop the resolved id so the next
    // tick finds or founds the new one. Copies already standing keep the old
    // one until they go, which is not long.
    if (g_ObcCloneConfig.guildName != guildName)
    {
        g_ObcCloneConfig.guildName = guildName;
        g_PvpBotGuildId.store(0, std::memory_order_relaxed);
        g_PvpBotGuildRosterNoticeSaid = false;
    }

    TC_LOG_INFO("server.loading", "OBC in-memory clone mirror config: enabled={}, pvp copy guild='{}'.",
        g_ObcCloneConfig.enabled ? "true" : "false", g_ObcCloneConfig.guildName);
}

uint32 PlayerbotObcCloneManager::GetBotGuildId()
{
    return g_PvpBotGuildId.load(std::memory_order_relaxed);
}

void PlayerbotObcCloneManager::OnStartupSweep()
{
    // In-memory clones cannot survive a process restart and have no persistent
    // rows to sweep.
}

void PlayerbotObcCloneManager::OnShutdown()
{
    TeardownAllWorldClones();
    TeardownAllCustomGameLobbyClones();
    TeardownAllCustomGameClones();
    TeardownAllClones();
}

void PlayerbotObcCloneManager::OnWorldUpdate(uint32 diffMs)
{
    // Offline loads for open-world copies, answered here because no map is
    // updating now. Ahead of every gate below: none of them is about these.
    g_WorldThreadSourceLoads.ProcessReadyCallbacks();

    // The copies' guild. Resolved off the tick rather than at config load
    // because guilds are not loaded yet when the configuration is, and because
    // founding one waits on a character load. Cheap once it is known; retried
    // slowly while it is not, since each attempt is a query.
    if (!g_PvpBotGuildId.load(std::memory_order_relaxed))
    {
        g_PvpBotGuildRetryMs += diffMs;
        if (g_PvpBotGuildRetryMs >= kPvpBotGuildRetryMs)
        {
            g_PvpBotGuildRetryMs = 0;
            EnsurePvpBotGuild();
        }
    }

    // An open-world copy that has left the world without being destroyed -
    // its map unloaded under it - is finished. The PvE manager notices its
    // hunter is gone on its own next pass.
    std::vector<ObjectGuid> lostWorldClones;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        for (auto const& [cloneGuid, record] : g_WorldClones)
        {
            auto sessionItr = g_CloneSessions.find(cloneGuid);
            Player* clone = sessionItr != g_CloneSessions.end() && sessionItr->second ? sessionItr->second->GetPlayer() : nullptr;
            if (!clone || !clone->IsInWorld())
                lostWorldClones.push_back(cloneGuid);
        }
    }
    for (ObjectGuid cloneGuid : lostWorldClones)
        TeardownWorldClone(cloneGuid);

    std::vector<ObjectGuid> expiredLobbyClones;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        for (auto const& [cloneGuid, record] : g_CustomGameLobbyClones)
            if (!sMapMgr->FindMap(record.mapId, record.lobbyInstanceId))
                expiredLobbyClones.push_back(cloneGuid);
    }
    for (ObjectGuid cloneGuid : expiredLobbyClones)
        TeardownCustomGameLobbyClone(cloneGuid);

    std::vector<ObjectGuid> expiredCustomClones;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        for (auto const& [cloneGuid, record] : g_CustomGameClones)
        {
            Battleground* bg = sBattlegroundMgr->GetBattleground(record.battlegroundInstanceId, record.battlegroundType);
            if (!bg || bg->GetStatus() == STATUS_NONE || bg->GetStatus() == STATUS_WAIT_LEAVE)
                expiredCustomClones.push_back(cloneGuid);
            // A clone the match itself has unseated - the queue drops one to
            // admit a queued person - is left standing on the map with no
            // roster entry and no way home. It is done; take it down.
            else if (!bg->IsPlayerInBattleground(cloneGuid))
                expiredCustomClones.push_back(cloneGuid);
        }
    }
    for (ObjectGuid cloneGuid : expiredCustomClones)
        TeardownCustomGameClone(cloneGuid);

    // Custom-game clones (see CreateCustomGameClone) only ran the hunter pet
    // mirror once, at clone creation. If the source hunter's pet was not
    // actively summoned at that exact instant (dismissed, still logging in,
    // etc.) the clone was petless for the entire match with no way to catch
    // up. This must run unconditionally here, alongside the custom-game
    // teardown logic above and before the IsObcCloneFeatureConfigured() gate
    // below -- that config flag only controls the dedicated OBC auto-clone
    // queue feature and has nothing to do with custom-game clones, which are
    // driven entirely by custom_game_lobby.cpp. Gating this on that flag (as
    // an earlier version of this fix did) made it dead code for any server
    // running custom games without the OBC auto-queue feature also enabled.
    std::vector<CustomGameCloneRecord> customGameCloneRecords;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        customGameCloneRecords.reserve(g_CustomGameClones.size());
        for (auto const& [cloneGuid, record] : g_CustomGameClones)
            customGameCloneRecords.push_back(record);
    }
    for (CustomGameCloneRecord const& record : customGameCloneRecords)
    {
        Player* source = ObjectAccessor::FindConnectedPlayer(record.sourceGuid);
        Player* clone = ObjectAccessor::FindConnectedPlayer(record.cloneGuid);
        if (source && clone && clone->IsInWorld())
            SynchronizeHunterPetMirror(source, clone);
    }

    if (!IsObcCloneFeatureConfigured())
    {
        TeardownAllClones();
        return;
    }

    g_CloneTickAccumulatorMs += diffMs;
    if (g_CloneTickAccumulatorMs < kCloneTickThrottleMs)
        return;
    g_CloneTickAccumulatorMs = 0;

    std::vector<ObjectGuid> trackedHumans;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        trackedHumans.reserve(g_ClonesByHuman.size());
        for (auto const& [humanGuid, record] : g_ClonesByHuman)
            trackedHumans.push_back(humanGuid);
    }

    for (ObjectGuid const& humanGuid : trackedHumans)
    {
        Player* human = ObjectAccessor::FindConnectedPlayer(humanGuid);
        Battleground* bg = human ? GetHumanObcBattleground(human) : nullptr;
        if (!bg)
        {
            TeardownCloneForHuman(humanGuid);
            continue;
        }

        ObcCloneRecord record;
        bool foundRecord = false;
        {
            std::lock_guard<std::mutex> lock(g_ObcCloneLock);
            auto itr = g_ClonesByHuman.find(humanGuid);
            if (itr != g_ClonesByHuman.end())
            {
                record = itr->second;
                foundRecord = true;
            }
        }

        if (!foundRecord)
            continue;

        Player* clone = ObjectAccessor::FindConnectedPlayer(record.cloneGuid);
        uint32 const expectedCloneTeam = OppositeTeam(human->GetBGTeam());
        bool const wrongInstance = record.battlegroundInstanceId != bg->GetInstanceID();
        bool const wrongTeam = record.oppositeTeam != expectedCloneTeam;
        bool const missingClone = !clone || !clone->IsInWorld();
        bool const wrongMap = clone && clone->FindMap() != bg->GetBgMap();
        bool const missingRosterEntry = !bg->IsPlayerInBattleground(record.cloneGuid);
        bool const wrongRosterTeam = !missingRosterEntry && bg->GetPlayerTeam(record.cloneGuid) != record.oppositeTeam;
        bool const wrongCloneTeam = clone && clone->GetBGTeam() != record.oppositeTeam;

        if (wrongInstance || wrongTeam || missingClone || wrongMap || missingRosterEntry || wrongRosterTeam || wrongCloneTeam)
        {
            TC_LOG_WARN("playerbots.population",
                "OBC clone: rebuilding invalid mirror for human {} (clone={}, instance={}/{}, team={}/{}, missingClone={}, wrongMap={}, rosterPresent={}, rosterTeamOk={}, cloneTeamOk={}).",
                humanGuid.ToString(), record.cloneGuid.ToString(), record.battlegroundInstanceId, bg->GetInstanceID(),
                record.oppositeTeam, expectedCloneTeam, missingClone ? 1 : 0, wrongMap ? 1 : 0,
                missingRosterEntry ? 0 : 1, wrongRosterTeam ? 0 : 1, wrongCloneTeam ? 0 : 1);
            TeardownCloneForHuman(humanGuid);
        }
        else
            SynchronizeHunterPetMirror(human, clone);
    }

    std::vector<ObjectGuid> obcHumans;
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            if (GetHumanObcBattleground(player))
                obcHumans.push_back(guid);
    }

    for (ObjectGuid const& humanGuid : obcHumans)
    {
        {
            std::lock_guard<std::mutex> lock(g_ObcCloneLock);
            if (g_ClonesByHuman.find(humanGuid) != g_ClonesByHuman.end())
                continue;
        }

        Player* human = ObjectAccessor::FindConnectedPlayer(humanGuid);
        Battleground* bg = human ? GetHumanObcBattleground(human) : nullptr;
        if (human && bg)
            ProvisionCloneForHuman(human, bg);
    }
}

void PlayerbotObcCloneManager::OnPvpKill(Player* killer, Player* killed)
{
    if (!killer || !killed || killer == killed)
        return;

    // Dismiss a dead clone's mirror pet here rather than leaving it to the
    // periodic mirror sync: that runs on a timer, so the pet would otherwise
    // outlive its owner for up to a full tick, which is what left mirror pets
    // standing around after the hunter died. SynchronizeHunterPetMirror also
    // refuses to rebuild one while the clone is dead, so this stays dismissed
    // until it actually resurrects.
    if (IsActiveClone(killed))
        if (Pet* mirrorPet = killed->GetPet())
            killed->RemovePet(mirrorPet, PET_SAVE_NOT_IN_SLOT);

    bool areCounterparts = false;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);

        auto humanItr = g_ClonesByHuman.find(killer->GetGUID());
        if (humanItr != g_ClonesByHuman.end() && humanItr->second.cloneGuid == killed->GetGUID())
            areCounterparts = true;

        if (!areCounterparts)
        {
            auto cloneItr = g_HumanByClone.find(killer->GetGUID());
            if (cloneItr != g_HumanByClone.end() && cloneItr->second == killed->GetGUID())
                areCounterparts = true;
        }
    }

    if (areCounterparts)
        if (Aura* aura = killer->AddAura(kBloodlustSpellId, killer))
        {
            aura->SetMaxDuration(kBloodlustDurationMs);
            aura->SetDuration(kBloodlustDurationMs);
        }
}

void PlayerbotObcCloneManager::OnPlayerLogout(Player const* player)
{
    if (!player)
        return;

    bool hasClone = false;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        hasClone = g_ClonesByHuman.find(player->GetGUID()) != g_ClonesByHuman.end();
    }

    if (hasClone)
        TeardownCloneForHuman(player->GetGUID());
}

bool PlayerbotObcCloneManager::IsActiveClone(Player const* player)
{
    if (!player)
        return false;

    // The transient-session marker is assigned before the clone is inserted
    // into the manager maps and remains set while teardown detaches it. It
    // closes both small windows where generic virtual-bot lifecycle code could
    // otherwise mistake the clone for a persistent managed bot.
    if (WorldSession const* session = player->GetSession(); session && session->IsTransientPlayerSession())
        return true;

    std::lock_guard<std::mutex> lock(g_ObcCloneLock);
    return g_HumanByClone.find(player->GetGUID()) != g_HumanByClone.end();
}

Player* PlayerbotObcCloneManager::CreateCustomGameClone(Player* source, Battleground* bg, uint32 team, std::string const& displayPrefix)
{
    if (!source || !bg || (team != ALLIANCE && team != HORDE))
        return nullptr;

    std::string const internalName = GenerateCloneInternalName();
    if (internalName.empty())
        return nullptr;

    uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
    auto session = std::make_unique<WorldSession>(0, "custom_game_in_memory_clone", nullptr, SEC_PLAYER, expansion, 0,
        Minutes(0), LOCALE_enUS, 0, false);
    session->SetTransientPlayerSession();

    Player* clone = new Player(session.get());
    CharacterCreateInfo createInfo;
    createInfo.SetName(internalName)
        .SetRace(source->GetRace()).SetClass(source->GetClass()).SetGender(source->GetNativeGender())
        .SetSkin(source->GetSkinId()).SetFace(source->GetFaceId()).SetHairStyle(source->GetHairStyleId())
        .SetHairColor(source->GetHairColorId()).SetFacialHair(source->GetFacialStyle()).SetOutfitId(0);

    ObjectGuid::LowType const cloneLowGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
    if (!clone->Create(cloneLowGuid, &createInfo, false, Player::AppearanceCheck::None))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    clone->SetGender(source->GetNativeGender());
    clone->SetNativeGender(source->GetNativeGender());
    clone->InitDisplayIds();
    // Server-created player: LoadFromDB never runs, so PlayerSocial is
    // null unless we make one. Channel::SendToAll and friends call
    // GetSocial()->HasIgnore() on every player they touch.
    clone->EnsureSocial();
    session->SetPlayer(clone);

    ApplyCloneGuild(clone, source);

    clone->GetMotionMaster()->Initialize();
    clone->SetLevel(source->GetLevel(), false);
    clone->InitStatsForLevel();
    clone->InitTalentForLevel();
    clone->InitGlyphsForLevel();
    CopySkills(clone, source);
    CopySpellsTalentsAndGlyphs(clone, source);
    if (!CopyEquipment(clone, source))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    // Player::LoadFromDB turns this on before its own UpdateAllStats, and a
    // server-built clone never runs LoadFromDB - so without it Unit::UpdateUnitMod
    // returns early for the rest of the clone's life and every LATER stat or
    // damage percent modifier (arena preparation buffs, the Violet Hold
    // Diminished debuff, copied boons, Reinforced) is stored and never applied.
    clone->SetCanModifyStats(true);
    clone->UpdateAllStats();
    clone->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        clone->SetFullPower(Powers(power));

    BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
    if (queueTypeId == BATTLEGROUND_QUEUE_NONE || clone->AddBattlegroundQueueId(queueTypeId) >= PLAYER_MAX_BATTLEGROUND_QUEUES)
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    // The team's start, or in a squad match the corner of the squad on that
    // side with the most seats free - claimed now, so the next clone counts it.
    // Its talents are already copied, so a squad that wants a healer can tell.
    bool const healer = bg->GetSquadHealersWanted() && Arena::IsSoloShuffleHealer(clone);
    Position const* start = bg->TakeStartPosition(clone->GetGUID(), team, healer);
    if (!start)
    {
        bg->ReleaseSquadSeat(clone->GetGUID());
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    clone->SetInviteForBattlegroundQueueType(queueTypeId, bg->GetInstanceID());
    clone->SetBattlegroundId(bg->GetInstanceID(), bg->GetTypeID());
    clone->SetFactionForRace(team == HORDE ? RACE_BLOODELF : RACE_HUMAN);
    clone->SetBGTeam(team);
    clone->ResetMap();
    clone->Relocate(*start);
    clone->SetMap(bg->GetBgMap());
    bg->IncreaseInvitedCount(team);
    if (!bg->GetBgMap()->AddPlayerToMap(clone))
    {
        bg->DecreaseInvitedCount(team);
        bg->ReleaseSquadSeat(clone->GetGUID());
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    ObjectGuid const cloneGuid = clone->GetGUID();
    std::string displayName = displayPrefix + source->GetName();
    sCharacterCache->AddCharacterCacheEntry(cloneGuid, 0, displayName, clone->GetNativeGender(), clone->GetRace(), clone->GetClass(), clone->GetLevel(), false);
    Surnames::AdoptTransient(cloneGuid, source->GetGUID());
    ObjectAccessor::AddObject(clone);
    bg->AddPlayer(clone);
    // Battleground::AddPlayer runs UnsummonPetTemporaryIfAny() on everyone who
    // enters, which records a "temporarily unsummoned" pet number for later
    // restoration. That restore path (ResummonPetTemporaryUnSummonedIfAny) is
    // DB-backed - it calls Pet::LoadPetFromDB - and a mirror pet only ever
    // exists in memory under a clone GUID with no character_pet rows behind
    // it. Leaving the number set makes the arena machinery chase a pet that
    // cannot be loaded while SynchronizeHunterPetMirror independently builds
    // the real mirror, so the two fight over one pet slot. The mirror is the
    // only owner of this clone's pet: clear the core's claim on it.
    clone->SetTemporaryUnsummonedPetNumber(0);
    clone->SetInGameTime(GameTime::GetGameTimeMS());
    SynchronizeHunterPetMirror(source, clone);

    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_CloneSessions.emplace(cloneGuid, std::move(session));
        g_CustomGameClones.emplace(cloneGuid, CustomGameCloneRecord{ cloneGuid, source->GetGUID(), team, bg->GetInstanceID(), bg->GetTypeID() });
    }

    return clone;
}

bool PlayerbotObcCloneManager::QueueCustomGameClone(ObjectGuid sourceGuid, WorldSession* callbackSession, Battleground* bg,
    uint32 team, std::string const& displayPrefix)
{
    if (!sourceGuid || !bg || (team != ALLIANCE && team != HORDE))
    {
        if (bg)
            bg->ResolveCustomGamePendingClone();
        return false;
    }

    if (Player* onlineSource = ObjectAccessor::FindConnectedPlayer(sourceGuid))
    {
        bool const created = CreateCustomGameClone(onlineSource, bg, team, displayPrefix) != nullptr;
        bg->ResolveCustomGamePendingClone();
        return created;
    }

    if (!callbackSession)
    {
        bg->ResolveCustomGamePendingClone();
        return false;
    }

    CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(sourceGuid);
    if (!characterInfo)
    {
        bg->ResolveCustomGamePendingClone();
        return false;
    }

    auto holder = std::make_shared<LoginQueryHolder>(characterInfo->AccountId, sourceGuid);
    if (!holder->Initialize())
    {
        bg->ResolveCustomGamePendingClone();
        return false;
    }

    uint32 const battlegroundInstanceId = bg->GetInstanceID();
    BattlegroundTypeId const battlegroundType = bg->GetTypeID();
    SQLQueryHolderCallback& callback = callbackSession->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder));
    callback.AfterComplete([sourceGuid, battlegroundInstanceId, battlegroundType, team, displayPrefix](SQLQueryHolderBase const& queryHolder)
    {
        Battleground* targetBg = sBattlegroundMgr->GetBattleground(battlegroundInstanceId, battlegroundType);
        if (!targetBg)
            return;

        if (targetBg->GetStatus() != STATUS_NONE && targetBg->GetStatus() != STATUS_WAIT_LEAVE)
        {
            if (Player* onlineSource = ObjectAccessor::FindConnectedPlayer(sourceGuid))
                PlayerbotObcCloneManager::CreateCustomGameClone(onlineSource, targetBg, team, displayPrefix);
            else
            {
                LoginQueryHolder const& loginHolder = static_cast<LoginQueryHolder const&>(queryHolder);
                uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
                auto sourceSession = std::make_unique<WorldSession>(loginHolder.GetAccountId(), "custom_game_offline_clone_source",
                    nullptr, SEC_PLAYER, expansion, 0, Minutes(0), LOCALE_enUS, 0, false);
                sourceSession->SetTransientPlayerSession();

                Player* offlineSource = new Player(sourceSession.get());
                if (offlineSource->LoadFromDB(sourceGuid, loginHolder, false))
                {
                    ApplyOfflineSourceGuild(offlineSource, loginHolder);
                    offlineSource->GetMotionMaster()->Initialize();
                    // Dark copies only read identity, spells, talents, glyph ids, and
                    // equipment from this temporary source. Runtime auras are neither
                    // copied nor safe to retain on a never-world offline Player.
                    offlineSource->RemoveAllAuras();
                    PlayerbotObcCloneManager::CreateCustomGameClone(offlineSource, targetBg, team, displayPrefix);
                }

                DestroyUnseatedClone(sourceSession, offlineSource);
            }
        }

        targetBg->ResolveCustomGamePendingClone();
    });

    return true;
}

bool PlayerbotObcCloneManager::LoadOfflineCloneSource(ObjectGuid sourceGuid, WorldSession* callbackSession,
    std::function<void(Player* source)> onResolved)
{
    if (!sourceGuid || !callbackSession || !onResolved)
        return false;

    CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(sourceGuid);
    if (!characterInfo)
        return false;

    auto holder = std::make_shared<LoginQueryHolder>(characterInfo->AccountId, sourceGuid);
    if (!holder->Initialize())
        return false;

    SQLQueryHolderCallback& callback = callbackSession->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder));
    callback.AfterComplete([sourceGuid, onResolved](SQLQueryHolderBase const& queryHolder)
    {
        // Never build a second Player under the guid of a live one.
        if (Player* onlineSource = ObjectAccessor::FindConnectedPlayer(sourceGuid))
        {
            onResolved(onlineSource);
            return;
        }

        LoginQueryHolder const& loginHolder = static_cast<LoginQueryHolder const&>(queryHolder);
        uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
        auto sourceSession = std::make_unique<WorldSession>(loginHolder.GetAccountId(), "offline_clone_source",
            nullptr, SEC_PLAYER, expansion, 0, Minutes(0), LOCALE_enUS, 0, false);
        sourceSession->SetTransientPlayerSession();

        Player* offlineSource = new Player(sourceSession.get());
        if (offlineSource->LoadFromDB(sourceGuid, loginHolder, false))
        {
            ApplyOfflineSourceGuild(offlineSource, loginHolder);
            offlineSource->GetMotionMaster()->Initialize();
            // Clones read identity, spells, talents, glyph ids and equipment
            // from their source. Runtime auras are neither copied nor safe to
            // keep on a Player that never enters the world.
            offlineSource->RemoveAllAuras();
            onResolved(offlineSource);
        }
        else
            onResolved(nullptr);

        DestroyUnseatedClone(sourceSession, offlineSource);
    });

    return true;
}

bool PlayerbotObcCloneManager::LoadOfflineCloneSourceOnWorldThread(ObjectGuid sourceGuid,
    std::function<void(Player* source)> onResolved)
{
    if (!sourceGuid || !onResolved)
        return false;

    CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(sourceGuid);
    if (!characterInfo)
        return false;

    auto holder = std::make_shared<LoginQueryHolder>(characterInfo->AccountId, sourceGuid);
    if (!holder->Initialize())
        return false;

    g_WorldThreadSourceLoads.AddCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete(
        [sourceGuid, onResolved](SQLQueryHolderBase const& queryHolder)
    {
        // Never build a second Player under the guid of a live one.
        if (Player* onlineSource = ObjectAccessor::FindConnectedPlayer(sourceGuid))
        {
            onResolved(onlineSource);
            return;
        }

        LoginQueryHolder const& loginHolder = static_cast<LoginQueryHolder const&>(queryHolder);
        uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
        auto sourceSession = std::make_unique<WorldSession>(loginHolder.GetAccountId(), "offline_clone_source",
            nullptr, SEC_PLAYER, expansion, 0, Minutes(0), LOCALE_enUS, 0, false);
        sourceSession->SetTransientPlayerSession();

        Player* offlineSource = new Player(sourceSession.get());
        if (offlineSource->LoadFromDB(sourceGuid, loginHolder, false))
        {
            ApplyOfflineSourceGuild(offlineSource, loginHolder);
            offlineSource->GetMotionMaster()->Initialize();
            // Clones read identity, spells, talents, glyph ids and equipment
            // from their source. Runtime auras are neither copied nor safe to
            // keep on a Player that never enters the world.
            offlineSource->RemoveAllAuras();
            onResolved(offlineSource);
        }
        else
            onResolved(nullptr);

        DestroyUnseatedClone(sourceSession, offlineSource);
    });

    return true;
}

Player* PlayerbotObcCloneManager::CreateWorldClone(Player* source, Map* map, Position const& position)
{
    if (!source || !map || map->Instanceable())
        return nullptr;

    std::string const internalName = GenerateCloneInternalName();
    if (internalName.empty())
        return nullptr;

    uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
    auto session = std::make_unique<WorldSession>(0, "world_in_memory_clone", nullptr, SEC_PLAYER, expansion, 0,
        Minutes(0), LOCALE_enUS, 0, false);
    session->SetTransientPlayerSession();

    Player* clone = new Player(session.get());
    CharacterCreateInfo createInfo;
    createInfo.SetName(internalName)
        .SetRace(source->GetRace()).SetClass(source->GetClass()).SetGender(source->GetNativeGender())
        .SetSkin(source->GetSkinId()).SetFace(source->GetFaceId()).SetHairStyle(source->GetHairStyleId())
        .SetHairColor(source->GetHairColorId()).SetFacialHair(source->GetFacialStyle()).SetOutfitId(0);

    ObjectGuid::LowType const cloneLowGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
    if (!clone->Create(cloneLowGuid, &createInfo, false, Player::AppearanceCheck::None))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    clone->SetGender(source->GetNativeGender());
    clone->SetNativeGender(source->GetNativeGender());
    clone->InitDisplayIds();
    // Server-created player: LoadFromDB never runs, so PlayerSocial is
    // null unless we make one. Channel::SendToAll and friends call
    // GetSocial()->HasIgnore() on every player they touch.
    clone->EnsureSocial();
    session->SetPlayer(clone);

    ApplyCloneGuild(clone, source);

    clone->GetMotionMaster()->Initialize();
    clone->SetLevel(source->GetLevel(), false);
    clone->InitStatsForLevel();
    clone->InitTalentForLevel();
    clone->InitGlyphsForLevel();
    CopySkills(clone, source);
    CopySpellsTalentsAndGlyphs(clone, source);
    if (!CopyEquipment(clone, source))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    // See CreateCustomGameClone: without it no later stat or damage modifier
    // is ever applied to a server-built copy.
    clone->SetCanModifyStats(true);
    clone->UpdateAllStats();
    clone->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        clone->SetFullPower(Powers(power));

    // Creatures and this copy are nothing to each other, in both directions:
    // IsValidAttackTarget refuses a creature attacking an IMMUNE_TO_NPC unit
    // and an IMMUNE_TO_NPC unit attacking a creature, so nothing aggroes on it
    // on the way and it never picks a fight with the wildlife. Players are
    // untouched - the flag only answers units that are not player-controlled.
    // After InitStatsForLevel, which clears it.
    clone->SetImmuneToNPC(true);

    clone->ResetMap();
    clone->Relocate(position);
    clone->SetMap(map);
    if (!map->AddPlayerToMap(clone))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    ObjectGuid const cloneGuid = clone->GetGUID();
    sCharacterCache->AddCharacterCacheEntry(cloneGuid, 0, source->GetName(), clone->GetNativeGender(), clone->GetRace(),
        clone->GetClass(), clone->GetLevel(), false);
    Surnames::AdoptTransient(cloneGuid, source->GetGUID());
    ObjectAccessor::AddObject(clone);
    clone->SetInGameTime(GameTime::GetGameTimeMS());
    SynchronizeHunterPetMirror(source, clone);
    if (Pet* pet = clone->GetPet())
        pet->SetImmuneToNPC(true);

    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_CloneSessions.emplace(cloneGuid, std::move(session));
        g_WorldClones.emplace(cloneGuid, WorldCloneRecord{ cloneGuid, source->GetGUID(), map->GetId() });
    }

    return clone;
}

bool PlayerbotObcCloneManager::DestroyWorldClone(ObjectGuid cloneGuid)
{
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        if (g_WorldClones.find(cloneGuid) == g_WorldClones.end())
            return false;
    }

    TeardownWorldClone(cloneGuid);
    return true;
}

void PlayerbotObcCloneManager::DestroyCustomGameClones(uint32 battlegroundInstanceId, uint32 team)
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        for (auto const& [cloneGuid, record] : g_CustomGameClones)
            if (record.battlegroundInstanceId == battlegroundInstanceId && (!team || record.team == team))
            {
                // Arena rosters are immutable from preparation through the
                // result. A lobby close ends the arena immediately after this
                // call, and the normal WAIT_LEAVE cleanup removes the clones on
                // the following world tick.
                if (IsRunningArenaClone(record))
                    continue;
                cloneGuids.push_back(cloneGuid);
            }
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownCustomGameClone(cloneGuid);
}

bool PlayerbotObcCloneManager::ShedOneCustomGameClone(uint32 battlegroundInstanceId)
{
    std::vector<CustomGameCloneRecord> records;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        for (auto const& [cloneGuid, record] : g_CustomGameClones)
            if (record.battlegroundInstanceId == battlegroundInstanceId)
                records.push_back(record);
    }

    if (records.empty())
        return false;

    // Resource pressure may cull an entire match through EndBattleground, but
    // it must never silently remove one transient combatant from a live arena.
    if (IsRunningArenaClone(records.front()))
        return false;

    uint32 allianceClones = 0;
    uint32 hordeClones = 0;
    for (CustomGameCloneRecord const& record : records)
        record.team == ALLIANCE ? ++allianceClones : ++hordeClones;

    // Shed from the side fielding more clones so successive removals keep the
    // match roughly balanced instead of hollowing out one team.
    uint32 shedTeam = allianceClones >= hordeClones ? ALLIANCE : HORDE;
    if (!allianceClones)
        shedTeam = HORDE;
    else if (!hordeClones)
        shedTeam = ALLIANCE;

    ObjectGuid fallbackGuid;
    ObjectGuid deadCloneGuid;
    for (CustomGameCloneRecord const& record : records)
    {
        if (record.team != shedTeam)
            continue;

        if (fallbackGuid.IsEmpty())
            fallbackGuid = record.cloneGuid;

        if (deadCloneGuid.IsEmpty())
            if (Player* clone = ObjectAccessor::FindConnectedPlayer(record.cloneGuid))
                if (!clone->IsAlive())
                    deadCloneGuid = record.cloneGuid;
    }

    ObjectGuid const target = deadCloneGuid.IsEmpty() ? fallbackGuid : deadCloneGuid;
    if (target.IsEmpty())
        return false;

    TC_LOG_INFO("playerbots.governor",
        "Resource governor shed custom-match clone: bgInstanceId={} team={} clone={} preferredDead={}.",
        battlegroundInstanceId, shedTeam, target.ToString(), deadCloneGuid.IsEmpty() ? 0 : 1);
    TeardownCustomGameClone(target);
    return true;
}

std::vector<PlayerbotObcCloneManager::CustomGameCloneInfo> PlayerbotObcCloneManager::GetCustomGameClones(uint32 battlegroundInstanceId)
{
    std::vector<CustomGameCloneInfo> clones;
    std::lock_guard<std::mutex> lock(g_ObcCloneLock);
    for (auto const& [cloneGuid, record] : g_CustomGameClones)
    {
        if (record.battlegroundInstanceId != battlegroundInstanceId)
            continue;

        CustomGameCloneInfo info;
        info.cloneGuid = cloneGuid;
        info.sourceGuid = record.sourceGuid;
        info.team = record.team;
        clones.push_back(info);
    }

    return clones;
}

bool PlayerbotObcCloneManager::DestroyCustomGameClone(ObjectGuid cloneGuid)
{
    CustomGameCloneRecord record;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto itr = g_CustomGameClones.find(cloneGuid);
        if (itr == g_CustomGameClones.end())
            return false;
        record = itr->second;
    }

    if (IsRunningArenaClone(record))
        return false;

    TeardownCustomGameClone(cloneGuid);
    return true;
}

bool PlayerbotObcCloneManager::QueueCustomGameLobbyClone(ObjectGuid sourceGuid, WorldSession* callbackSession,
    uint32 mapId, uint32 lobbyInstanceId, uint32 rosterSlotId, uint32 team, bool isPlayerbot, Position const& position,
    std::string const& displayPrefix)
{
    if (!sourceGuid || (team != ALLIANCE && team != HORDE) ||
        HasCustomGameLobbyClone(lobbyInstanceId, rosterSlotId))
        return false;

    if (Player* onlineSource = ObjectAccessor::FindConnectedPlayer(sourceGuid))
        return CreateCustomGameLobbyClone(onlineSource, mapId, lobbyInstanceId, team, rosterSlotId, isPlayerbot, position,
            displayPrefix) != nullptr;

    if (!callbackSession)
        return false;

    CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(sourceGuid);
    if (!characterInfo)
        return false;

    auto holder = std::make_shared<LoginQueryHolder>(characterInfo->AccountId, sourceGuid);
    if (!holder->Initialize())
        return false;

    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_PendingCustomGameLobbyClones.push_back({ sourceGuid, mapId, lobbyInstanceId, rosterSlotId, team, isPlayerbot,
            position, displayPrefix });
    }

    SQLQueryHolderCallback& callback = callbackSession->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder));
    callback.AfterComplete([sourceGuid, lobbyInstanceId, rosterSlotId](SQLQueryHolderBase const& queryHolder)
    {
        PendingCustomGameLobbyClone request;
        {
            std::lock_guard<std::mutex> lock(g_ObcCloneLock);
            auto itr = std::find_if(g_PendingCustomGameLobbyClones.begin(), g_PendingCustomGameLobbyClones.end(),
                [&](PendingCustomGameLobbyClone const& pending)
                {
                    return MatchesLobbyClone(pending, lobbyInstanceId, rosterSlotId);
                });
            if (itr == g_PendingCustomGameLobbyClones.end())
                return;

            request = *itr;
            g_PendingCustomGameLobbyClones.erase(itr);
        }

        if (!sMapMgr->FindMap(request.mapId, request.lobbyInstanceId))
            return;

        if (Player* onlineSource = ObjectAccessor::FindConnectedPlayer(sourceGuid))
        {
            CreateCustomGameLobbyClone(onlineSource, request.mapId, request.lobbyInstanceId, request.team,
                request.rosterSlotId, request.isPlayerbot, request.position, request.displayPrefix);
            return;
        }

        LoginQueryHolder const& loginHolder = static_cast<LoginQueryHolder const&>(queryHolder);
        uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
        auto sourceSession = std::make_unique<WorldSession>(loginHolder.GetAccountId(), "custom_game_lobby_offline_source",
            nullptr, SEC_PLAYER, expansion, 0, Minutes(0), LOCALE_enUS, 0, false);
        sourceSession->SetTransientPlayerSession();

        Player* offlineSource = new Player(sourceSession.get());
        if (offlineSource->LoadFromDB(sourceGuid, loginHolder, false))
        {
            ApplyOfflineSourceGuild(offlineSource, loginHolder);
            offlineSource->GetMotionMaster()->Initialize();
            // Equipment loading can still apply passive effects even when
            // persisted and glyph auras are skipped. The preview copy does
            // not consume those effects, so detach them before copying.
            offlineSource->RemoveAllAuras();
            CreateCustomGameLobbyClone(offlineSource, request.mapId, request.lobbyInstanceId, request.team,
                request.rosterSlotId, request.isPlayerbot, request.position, request.displayPrefix);
        }

        DestroyUnseatedClone(sourceSession, offlineSource);
    });

    return true;
}

void PlayerbotObcCloneManager::SetCustomGameLobbyClonePosition(uint32 lobbyInstanceId, uint32 rosterSlotId,
    Position const& position)
{
    ObjectGuid cloneGuid;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        for (auto& pending : g_PendingCustomGameLobbyClones)
            if (MatchesLobbyClone(pending, lobbyInstanceId, rosterSlotId))
                pending.position = position;

        for (auto const& [guid, record] : g_CustomGameLobbyClones)
            if (MatchesLobbyClone(record, lobbyInstanceId, rosterSlotId))
            {
                cloneGuid = guid;
                break;
            }
    }

    if (Player* clone = ObjectAccessor::FindConnectedPlayer(cloneGuid))
    {
        // These server-owned preview players have no client to acknowledge a
        // normal Player::NearTeleportTo.  Relocate them immediately and only
        // when their roster slot actually changed; the teleport packet keeps
        // nearby real clients visually synchronized without setting a pending
        // player-teleport semaphore.
        if (clone->GetExactDistSq(position) > 0.01f)
        {
            clone->SendTeleportPacket(position);
            clone->UpdatePosition(position, true);
            clone->UpdateObjectVisibility();
        }
    }
}

void PlayerbotObcCloneManager::DestroyCustomGameLobbyClone(uint32 lobbyInstanceId, uint32 rosterSlotId)
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_PendingCustomGameLobbyClones.erase(std::remove_if(g_PendingCustomGameLobbyClones.begin(),
            g_PendingCustomGameLobbyClones.end(), [&](PendingCustomGameLobbyClone const& pending)
            {
                return MatchesLobbyClone(pending, lobbyInstanceId, rosterSlotId);
            }), g_PendingCustomGameLobbyClones.end());

        for (auto const& [cloneGuid, record] : g_CustomGameLobbyClones)
            if (MatchesLobbyClone(record, lobbyInstanceId, rosterSlotId))
                cloneGuids.push_back(cloneGuid);
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownCustomGameLobbyClone(cloneGuid);
}

void PlayerbotObcCloneManager::DestroyCustomGameLobbyClones(uint32 lobbyInstanceId)
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_PendingCustomGameLobbyClones.erase(std::remove_if(g_PendingCustomGameLobbyClones.begin(),
            g_PendingCustomGameLobbyClones.end(), [lobbyInstanceId](PendingCustomGameLobbyClone const& pending)
            {
                return pending.lobbyInstanceId == lobbyInstanceId;
            }), g_PendingCustomGameLobbyClones.end());

        for (auto const& [cloneGuid, record] : g_CustomGameLobbyClones)
            if (record.lobbyInstanceId == lobbyInstanceId)
                cloneGuids.push_back(cloneGuid);
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownCustomGameLobbyClone(cloneGuid);
}
}
