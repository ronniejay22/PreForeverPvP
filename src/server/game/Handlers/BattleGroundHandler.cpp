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

#include "WorldSession.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "Chat.h"
#include "Common.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "DisableMgr.h"
#include "GameTime.h"
#include "Group.h"
#include "Language.h"
#include "Log.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "Item.h"
#include "World.h"
#include "WorldPacket.h"
#include "CharacterCache.h"

#include <string>

namespace
{
    uint32 constexpr ChromieEntry = 27915;
    char const* const ChromieName = "Chromie";

    uint32 constexpr GurubashiArenaMapId = 0;
    float constexpr GurubashiArenaX = -13235.707031f;
    float constexpr GurubashiArenaY = 214.336441;
    float constexpr GurubashiArenaZ = 31.276190;
    float constexpr GurubashiArenaO = 1.010225f;

    bool IsPlayerInsideCustomGameLobby(Player const* player)
    {
        return player && player->IsInCustomGameLobby();
    }

    // Which arena queue the arena battlemaster's window is about to show,
    // told to the client's ArenaFrame.lua ahead of the window itself:
    //
    //   CCGAME ARENAQUEUE:0       the ordinary one
    //   CCGAME ARENAQUEUE:<n>     a squad match of up to n squads, rated and
    //                             skirmish, each row a squad size (Baradin Hold:
    //                             Four Corners)
    //   CCGAME ARENAQUEUE:SHUFFLE Buffle Shuffle: the 2v2 and 3v3 skirmish rows
    //                             only, queued alone
    //
    // Sent before every arena window so a client never keeps the last mode.
    //
    //   CCGAME ARENARATED:<0|1>   whether the window's rated rows can be joined
    //                             alone or short-handed - 1 only for the ordinary
    //                             queue where the realm fills rated arenas with
    //                             clones (BattlegroundMgr::IsBotFillRatedArena)
    void SendArenaFrameMessage(Player* player, std::string const& body)
    {
        if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
            return;

        std::string const message = "CCGAME\t" + body;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(),
            message, 0, player->GetName(), player->GetName());
        player->SendDirectMessage(&data);
    }

    void SendArenaQueueMode(Player* player, std::string const& mode)
    {
        SendArenaFrameMessage(player, "ARENAQUEUE:" + mode);
    }

    bool RejectNormalBattlegroundQueueFromCustomLobby(WorldSession* session)
    {
        if (!session || !IsPlayerInsideCustomGameLobby(session->GetPlayer()))
            return false;

        session->SendNotification("You cannot queue for other battlegrounds or arenas while inside a custom game lobby.");
        return true;
    }

    // Artifact quality is what the levelling KIT is marked with on Barracks+:
    // "Recruit's Shirt", "Squire's Boots", "Neophyte's Pants" and 1,141 more,
    // entry 92000 up, every one of them item level 1 and required level 0-11.
    // So this test used to catch an ordinary levelling character wearing the
    // gear the realm itself handed them, teleport them to Gurubashi and whisper
    // that they may not enter - for the battleground they had just queued for
    // and been invited to. Since the kit was reworked there is nothing left to
    // protect against, so the door is open by default.
    //
    // The check is kept behind a key rather than deleted because the same
    // quality also covers a handful of stock GM items - Martin Fury, Frostmourne,
    // the Warglaives of Azzinoth - which is the whole of it on Legionnaire+ (15
    // items there against 1,144 on Barracks+). A realm that ever puts one of
    // those in a player's hands can shut the door again with a config reload
    // instead of a rebuild.
    bool HasArtifactEquipment(Player const* player)
    {
        if (!player)
            return false;

        if (!sConfigMgr->GetBoolDefault("Centurion.Battleground.BlockArtifactGear", false))
            return false;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (ItemTemplate const* proto = item->GetTemplate())
                    if (proto->Quality == ITEM_QUALITY_ARTIFACT)
                        return true;

        return false;
    }

    void TeleportToGurubashiWithChromieWhisper(Player* player)
    {
        if (!player)
            return;

        player->TeleportTo(GurubashiArenaMapId, GurubashiArenaX, GurubashiArenaY, GurubashiArenaZ, GurubashiArenaO);

        std::string message = "You can't enter battlegrounds or arenas while wearing artifact gear.";

        WorldPacket data;
        ObjectGuid chromieGuid = ObjectGuid::Create<HighGuid::Unit>(ChromieEntry, 1);
        ChatHandler::BuildChatPacket(data, CHAT_MSG_MONSTER_WHISPER, LANG_UNIVERSAL, chromieGuid, player->GetGUID(), message,
            0, ChromieName, player->GetName());
        player->SendDirectMessage(&data);
    }
}

void WorldSession::HandleBattlemasterHelloOpcode(WorldPacket& recvData)
{
    ObjectGuid guid;
    recvData >> guid;
    TC_LOG_DEBUG("network", "WORLD: Recvd CMSG_BATTLEMASTER_HELLO Message from {}", guid.ToString());

    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_BATTLEMASTER);
    if (!unit)
        return;

    // Stop the npc if moving
    if (uint32 pause = unit->GetMovementTemplate().GetInteractionPauseTimer())
        unit->PauseMovement(pause);
    unit->SetHomePosition(unit->GetPosition());

    BattlegroundTypeId bgTypeId = sBattlegroundMgr->GetBattleMasterBG(unit->GetEntry());

    if (!_player->GetBGAccessByLevel(bgTypeId))
    {
                                                            // temp, must be gossip message...
        SendNotification(LANG_YOUR_BG_LEVEL_REQ_ERROR);
        return;
    }

    SendBattleGroundList(guid, bgTypeId);
}

void WorldSession::SendBattleGroundList(ObjectGuid guid, BattlegroundTypeId bgTypeId)
{
    // A squad match's battlemaster opens the arena window - the client shows
    // that for the All Arenas list - and the window is told which queue it is
    // for. Its joins name the battlemaster, which is how
    // HandleBattlemasterJoinArena tells the two apart.
    if (bgTypeId == BATTLEGROUND_AA || bgTypeId == BATTLEGROUND_SSH || sBattlegroundMgr->IsSquadBattleground(bgTypeId))
    {
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (bgTypeId == BATTLEGROUND_SSH)
            SendArenaQueueMode(_player, "SHUFFLE");
        else
            SendArenaQueueMode(_player, std::to_string(bgTemplate && bgTemplate->IsSquadMatch() ? bgTemplate->GetSquadCount() : 0));
        SendArenaFrameMessage(_player, std::string("ARENARATED:") +
            (bgTypeId == BATTLEGROUND_AA && sBattlegroundMgr->IsBotFillRatedArenaEnabled() ? "1" : "0"));
        bgTypeId = BATTLEGROUND_AA;
    }

    WorldPacket data;
    sBattlegroundMgr->BuildBattlegroundListPacket(&data, guid, _player, bgTypeId, 0);
    SendPacket(&data);
}

void WorldSession::HandleBattlemasterJoinOpcode(WorldPacket& recvData)
{
    ObjectGuid guid;
    uint32 bgTypeId_;
    uint32 instanceId;
    uint8 joinAsGroup;
    bool isPremade = false;
    Group* grp = nullptr;

    recvData >> guid;                                      // battlemaster guid
    recvData >> bgTypeId_;                                 // battleground type id (DBC id)
    recvData >> instanceId;                                // instance id, 0 if First Available selected
    recvData >> joinAsGroup;                               // join as group

    if (RejectNormalBattlegroundQueueFromCustomLobby(this))
        return;

    if (!sBattlemasterListStore.LookupEntry(bgTypeId_))
    {
        TC_LOG_ERROR("network", "Battleground: invalid bgtype ({}) received. possible cheater? player {}", bgTypeId_, _player->GetGUID().ToString());
        return;
    }

    if (DisableMgr::IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeId_, nullptr))
    {
        ChatHandler(this).PSendSysMessage(LANG_BG_DISABLED);
        return;
    }

    BattlegroundTypeId bgTypeId = BattlegroundTypeId(bgTypeId_);

    TC_LOG_DEBUG("network", "WORLD: Recvd CMSG_BATTLEMASTER_JOIN Message from {}", guid.ToString());

    // can do this, since it's battleground, not arena
    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
    BattlegroundQueueTypeId bgQueueTypeIdRandom = BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_RB, 0);

    // ignore if player is already in BG
    if (_player->InBattleground())
        return;

    // get bg instance or bg template if instance not found
    Battleground* bg = nullptr;
    if (instanceId)
        bg = sBattlegroundMgr->GetBattlegroundThroughClientInstance(instanceId, bgTypeId);

    if (!bg)
        bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bg)
        return;

    // expected bracket entry
    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), _player->GetLevel());
    if (!bracketEntry)
        return;

    GroupJoinBattlegroundResult err;

    // check queue conditions
    if (!joinAsGroup)
    {
        if (GetPlayer()->isUsingLfg())
        {
            // player is using dungeon finder or raid finder
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, ERR_LFG_CANT_USE_BATTLEGROUND);
            GetPlayer()->SendDirectMessage(&data);
            return;
        }

        // check RBAC permissions
        if (!_player->CanJoinToBattleground(bg))
        {
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, ERR_BATTLEGROUND_JOIN_TIMED_OUT);
            GetPlayer()->SendDirectMessage(&data);
            return;
        }

        // check Deserter debuff
        if (_player->IsDeserter())
        {
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS);
            _player->SendDirectMessage(&data);
            return;
        }

        if (_player->GetBattlegroundQueueIndex(bgQueueTypeIdRandom) < PLAYER_MAX_BATTLEGROUND_QUEUES)
        {
            // player is already in random queue
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, ERR_IN_RANDOM_BG);
            _player->SendDirectMessage(&data);
            return;
        }

        if (_player->InBattlegroundQueue(true) && bgTypeId == BATTLEGROUND_RB)
        {
            // player is already in queue, can't start random queue
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, ERR_IN_NON_RANDOM_BG);
            _player->SendDirectMessage(&data);
            return;
        }

        // check if already in queue
        if (_player->GetBattlegroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
            // player is already in this queue
            return;

        // check if has free queue slots
        if (!_player->HasFreeBattlegroundQueueId())
        {
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, ERR_BATTLEGROUND_TOO_MANY_QUEUES);
            _player->SendDirectMessage(&data);
            return;
        }

        // check Freeze debuff
        if (_player->HasAura(9454))
            return;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);

        GroupQueueInfo* ginfo = bgQueue.AddGroup(_player, nullptr, bgTypeId, bracketEntry, 0, false, isPremade, 0, 0);
        uint32 avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
        // already checked if queueSlot is valid, now just get it
        uint32 queueSlot = _player->AddBattlegroundQueueId(bgQueueTypeId);

        WorldPacket data;
                                                            // send status packet (in queue)
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, ginfo->ArenaType, 0);
        SendPacket(&data);
        TC_LOG_DEBUG("bg.battleground", "Battleground: player joined queue for bg queue type {} bg type {}: GUID {}, NAME {}",
                       bgQueueTypeId, bgTypeId, _player->GetGUID().ToString(), _player->GetName());
    }
    else
    {
        grp = _player->GetGroup();
        // no group found, error
        if (!grp)
            return;
        if (grp->GetLeaderGUID() != _player->GetGUID())
            return;
        err = grp->CanJoinBattlegroundQueue(bg, bgQueueTypeId, 0, bg->GetMaxPlayersPerTeam(), false, 0);
        isPremade = (grp->GetMembersCount() >= bg->GetMinPlayersPerTeam());

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        GroupQueueInfo* ginfo = nullptr;
        uint32 avgTime = 0;

        if (err > 0)
        {
            TC_LOG_DEBUG("bg.battleground", "Battleground: the following players are joining as group:");
            ginfo = bgQueue.AddGroup(_player, grp, bgTypeId, bracketEntry, 0, false, isPremade, 0, 0);
            avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
        }

        for (GroupReference* itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member)
                continue;   // this should never happen

            WorldPacket data;

            if (err <= 0)
            {
                sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, err);
                member->SendDirectMessage(&data);
                continue;
            }

            // add to queue
            uint32 queueSlot = member->AddBattlegroundQueueId(bgQueueTypeId);

            // send status packet (in queue)
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, ginfo->ArenaType, 0);
            member->SendDirectMessage(&data);
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, err);
            member->SendDirectMessage(&data);
            TC_LOG_DEBUG("bg.battleground", "Battleground: player joined queue for bg queue type {} bg type {}: GUID {}, NAME {}",
                bgQueueTypeId, bgTypeId, member->GetGUID().ToString(), member->GetName());
        }
        TC_LOG_DEBUG("bg.battleground", "Battleground: group end");
    }
    sBattlegroundMgr->ScheduleQueueUpdate(0, 0, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());
}

void WorldSession::HandleBattlegroundPlayerPositionsOpcode(WorldPacket& /*recvData*/)
{
    TC_LOG_DEBUG("network", "WORLD: Recvd MSG_BATTLEGROUND_PLAYER_POSITIONS Message");

    SendBattlegroundPlayerPositions();
}

void WorldSession::SendBattlegroundPlayerPositions()
{
    Battleground* bg = _player->GetBattleground();
    if (!bg)                                                 // can't be received if player not in battleground
        return;

    uint32 flagCarrierCount = 0;
    Player* allianceFlagCarrier = nullptr;
    Player* hordeFlagCarrier = nullptr;

    bool showAllianceFlag = true;
    bool showHordeFlag = true;
    BattlegroundTypeId const actualType = bg->GetTypeID(true);
    if (bg->IsCustomGame() && (actualType == BATTLEGROUND_WS || actualType == BATTLEGROUND_TP))
    {
        // These picker slots are indexed by flag identity, not carrier team.
        showAllianceFlag = bg->ShouldShowFlagOnMapTo(_player, TEAM_ALLIANCE);
        showHordeFlag = bg->ShouldShowFlagOnMapTo(_player, TEAM_HORDE);
    }

    if (showAllianceFlag)
    {
        if (ObjectGuid guid = bg->GetFlagPickerGUID(TEAM_ALLIANCE))
        {
            allianceFlagCarrier = ObjectAccessor::FindPlayer(guid);
            if (allianceFlagCarrier)
                ++flagCarrierCount;
        }
    }

    if (showHordeFlag)
    {
        if (ObjectGuid guid = bg->GetFlagPickerGUID(TEAM_HORDE))
        {
            hordeFlagCarrier = ObjectAccessor::FindPlayer(guid);
            if (hordeFlagCarrier)
                ++flagCarrierCount;
        }
    }

    WorldPacket data(MSG_BATTLEGROUND_PLAYER_POSITIONS, 4 + 4 + 16 * flagCarrierCount);
    // Used to send several player positions (found used in AV)
    data << 0;  // CGBattlefieldInfo__m_numPlayerPositions
    /*
    for (CGBattlefieldInfo__m_numPlayerPositions)
        data << guid << posx << posy;
    */
    data << flagCarrierCount;
    if (allianceFlagCarrier)
    {
        data << uint64(allianceFlagCarrier->GetGUID());
        data << float(allianceFlagCarrier->GetPositionX());
        data << float(allianceFlagCarrier->GetPositionY());
    }

    if (hordeFlagCarrier)
    {
        data << uint64(hordeFlagCarrier->GetGUID());
        data << float(hordeFlagCarrier->GetPositionX());
        data << float(hordeFlagCarrier->GetPositionY());
    }

    SendPacket(&data);
}

void WorldSession::HandlePVPLogDataOpcode(WorldPacket & /*recvData*/)
{
    TC_LOG_DEBUG("network", "WORLD: Recvd MSG_PVP_LOG_DATA Message");

    Battleground* bg = _player->GetBattleground();
    if (!bg)
        return;

    // Prevent players from sending BuildPvpLogDataPacket in an arena except for when sent in BattleGround::EndBattleGround.
    if (bg->isArena())
        return;

    WorldPacket data;
    bg->BuildPvPLogDataPacket(data);
    SendPacket(&data);

    TC_LOG_DEBUG("network", "WORLD: Sent MSG_PVP_LOG_DATA Message");
}

void WorldSession::HandleBattlefieldListOpcode(WorldPacket &recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Recvd CMSG_BATTLEFIELD_LIST Message");

    uint32 bgTypeId;
    recvData >> bgTypeId;                                  // id from DBC

    uint8 fromWhere;
    recvData >> fromWhere;                                 // 0 - battlemaster (lua: ShowBattlefieldList), 1 - UI (lua: RequestBattlegroundInstanceInfo)

    uint8 canGainXP;
    recvData >> canGainXP;                                 // players with locked xp have their own bg queue on retail

    BattlemasterListEntry const* bl = sBattlemasterListStore.LookupEntry(bgTypeId);
    if (!bl)
    {
        TC_LOG_DEBUG("bg.battleground", "BattlegroundHandler: invalid bgtype ({}) with player (Name: {}, {}) received.", bgTypeId, _player->GetName(), _player->GetGUID().ToString());
        return;
    }

    WorldPacket data;
    sBattlegroundMgr->BuildBattlegroundListPacket(&data, ObjectGuid::Empty, _player, BattlegroundTypeId(bgTypeId), fromWhere);
    SendPacket(&data);
}

void WorldSession::HandleBattleFieldPortOpcode(WorldPacket &recvData)
{
    uint8 type;                                             // arenatype if arena
    uint8 unk2;                                             // unk, can be 0x0 (may be if was invited?) and 0x1
    uint32 bgTypeId_;                                       // type id from dbc
    uint16 unk;                                             // 0x1F90 constant?
    uint8 action;                                           // enter battle 0x1, leave queue 0x0

    recvData >> type >> unk2 >> bgTypeId_ >> unk >> action;

    // Let a lobby occupant decline/leave a stale queue, but never accept an
    // invitation into a normal battleground from inside the private lobby.
    if (action && RejectNormalBattlegroundQueueFromCustomLobby(this))
        return;

    if (!sBattlemasterListStore.LookupEntry(bgTypeId_))
    {
        TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT {} ArenaType: {}, Unk: {}, BgType: {}, Action: {}. Invalid BgType!",
            GetPlayerInfo(), type, unk2, bgTypeId_, action);
        return;
    }

    if (!_player->InBattlegroundQueue())
    {
        TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT {} ArenaType: {}, Unk: {}, BgType: {}, Action: {}. Player not in queue!",
            GetPlayerInfo(), type, unk2, bgTypeId_, action);
        return;
    }

    //get GroupQueueInfo from BattlegroundQueue
    BattlegroundTypeId bgTypeId = BattlegroundTypeId(bgTypeId_);
    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, type);
    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
    //we must use temporary variable, because GroupQueueInfo pointer can be deleted in BattlegroundQueue::RemovePlayer() function
    GroupQueueInfo ginfo;
    if (!bgQueue.GetPlayerGroupInfoData(_player->GetGUID(), &ginfo))
    {
        TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT {} ArenaType: {}, Unk: {}, BgType: {}, Action: {}. Player not in queue (No player Group Info)!",
            GetPlayerInfo(), type, unk2, bgTypeId_, action);
        return;
    }
    // if action == 1, then instanceId is required
    if (!ginfo.IsInvitedToBGInstanceGUID && action == 1)
    {
        TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT {} ArenaType: {}, Unk: {}, BgType: {}, Action: {}. Player is not invited to any bg!",
            GetPlayerInfo(), type, unk2, bgTypeId_, action);
        return;
    }

    Battleground* bg = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, bgTypeId);
    if (!bg)
    {
        if (action)
        {
            TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT {} ArenaType: {}, Unk: {}, BgType: {}, Action: {}. Cant find BG with id {}!",
                GetPlayerInfo(), type, unk2, bgTypeId_, action, ginfo.IsInvitedToBGInstanceGUID);
            return;
        }

        bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bg)
        {
            TC_LOG_ERROR("network", "BattlegroundHandler: bg_template not found for type id {}.", bgTypeId);
            return;
        }
    }

    TC_LOG_DEBUG("bg.battleground", "CMSG_BATTLEFIELD_PORT {} ArenaType: {}, Unk: {}, BgType: {}, Action: {}.",
        GetPlayerInfo(), type, unk2, bgTypeId_, action);

    // The bracket is wanted for one thing only - re-running the queue when
    // somebody leaves it - and it is looked up against the map this match
    // ROLLED ONTO, which is not the map the queue bracketed the player against.
    // "All Arenas" hands out one of thirteen arenas and only the pool's own
    // template is guaranteed to carry a band for every level; Nefarian's Arena
    // shipped with a single 60-69 row, so a level 59 pair who rolled it landed
    // here with no band at all. Returning on that is a dead "Enter Battle"
    // button: no packet, no message, and twenty seconds later the invite expires
    // as though nobody had pressed it.
    //
    // The match is already formed with this player in it, so a map that cannot
    // name their level is no reason to refuse them. Fall back to the bracket the
    // match itself was created with, which is the one the queue used.
    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), _player->GetLevel());
    BattlegroundBracketId const bracketId = bracketEntry ? bracketEntry->GetBracketId() : bg->GetBracketId();

    bool const artifactEquipped = HasArtifactEquipment(_player);

    //some checks if player isn't cheating - it is not exactly cheating, but we cannot allow it
    if (action == 1 && ginfo.ArenaType == 0)
    {
        //if player is trying to enter battleground (not arena!) and he has deserter debuff, we must just remove him from queue
        if (_player->IsDeserter())
        {
            //send bg command result to show nice message
            WorldPacket data2;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data2, ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS);
            _player->SendDirectMessage(&data2);
            action = 0;
            TC_LOG_DEBUG("bg.battleground", "Player {} {} has a deserter debuff, do not port him to battleground!", _player->GetName(), _player->GetGUID().ToString());
        }
        //if player don't match battleground max level, then do not allow him to enter! (this might happen when player leveled up during his waiting in queue
        if (_player->GetLevel() > bg->GetMaxLevel())
        {
            TC_LOG_ERROR("network", "Player {} {} has level ({}) higher than maxlevel ({}) of battleground ({})! Do not port him to battleground!",
                _player->GetName(), _player->GetGUID().ToString(), _player->GetLevel(), bg->GetMaxLevel(), bg->GetTypeID());
            action = 0;
        }
    }
    uint32 queueSlot = _player->GetBattlegroundQueueIndex(bgQueueTypeId);
    WorldPacket data;
    if (action)
    {
        if (artifactEquipped)
        {
            TeleportToGurubashiWithChromieWhisper(_player);

            uint32 remainingTime = ginfo.RemoveInviteTime ? getMSTimeDiff(GameTime::GetGameTimeMS(), ginfo.RemoveInviteTime) : 0;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_JOIN, remainingTime, 0, ginfo.ArenaType, ginfo.Team);
            _player->SendDirectMessage(&data);

            return;
        }

        // check Freeze debuff
        if (_player->HasAura(9454))
            return;

        if (!_player->IsInvitedForBattlegroundQueueType(bgQueueTypeId))
            return;                                 // cheating?

        // A clone's seat is only on offer behind closed gates, and only to
        // somebody with "Arena bot matches" ticked. The queue checked the box
        // when it made the offer, but it can be flipped at any time, so it is
        // read again here. The countdown is held while this invite is out, so
        // the gates can only have opened by a forced start (.gm bgstart).
        // Either way the invite is treated as lapsed.
        if (bg->HasPreparationSeatReservation(_player->GetGUID()) &&
            (bg->GetStatus() != STATUS_WAIT_JOIN || _player->HasArenaBotFillOptOut()))
        {
            if (_player->HasArenaBotFillOptOut())
                ChatHandler(this).SendSysMessage("Arena bot matches is unticked, so you were not seated in place of a bot.");

            _player->RemoveBattlegroundQueueId(bgQueueTypeId);
            bgQueue.RemovePlayer(_player->GetGUID(), true);
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_NONE, 0, 0, 0, 0);
            _player->SendDirectMessage(&data);
            return;
        }

        if (!_player->InBattleground())
            _player->SetBattlegroundEntryPoint();

        // resurrect the player
        if (!_player->IsAlive())
        {
            _player->ResurrectPlayer(1.0f);
            _player->SpawnCorpseBones();
        }
        // stop taxi flight at port
        _player->FinishTaxiFlight();

        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_IN_PROGRESS, 0, bg->GetStartTime(), bg->GetArenaType(), ginfo.Team);
        _player->SendDirectMessage(&data);

        // remove battleground queue status from BGmgr
        bgQueue.RemovePlayer(_player->GetGUID(), false);
        // this is still needed here if battleground "jumping" shouldn't add deserter debuff
        // also this is required to prevent stuck at old battleground after SetBattlegroundId set to new
        if (Battleground* currentBg = _player->GetBattleground())
            currentBg->RemovePlayerAtLeave(_player->GetGUID(), false, true);

        // set the destination instance id
        _player->SetBattlegroundId(bg->GetInstanceID(), bgTypeId);
        // set the destination team
        //ttopper we need to figure out teams before this point
        uint32 tmpTeam = ginfo.Team;
        uint8 tmpRace = _player->GetRaceForDB();

        //get temporary horde race
        //we created duplicates of each race in chrRaces and set them to horde faction
        //the ids are the same except +100
        //(all races start out as alliance in centurion)
        //this is to make scoreboard entries appear red/blue when they should
        _player->SetFactionForRace(RACE_HUMAN);
        if (tmpTeam == HORDE)
        {
            _player->SetFactionForRace(RACE_BLOODELF);
        }
        _player->SetBGTeam(tmpTeam);

        // bg->HandleBeforeTeleportToBattleground(_player);
        sBattlegroundMgr->SendToBattleground(_player, ginfo.IsInvitedToBGInstanceGUID, bgTypeId);
        // add only in HandleMoveWorldPortAck()
        // bg->AddPlayer(_player, team);
        TC_LOG_DEBUG("bg.battleground", "Battleground: player {} {} joined battle for bg {}, bgtype {}, queue type {}.", _player->GetName(), _player->GetGUID().ToString(), bg->GetInstanceID(), bg->GetTypeID(), bgQueueTypeId);
    }
    else // leave queue
    {
        if (bg->isArena() && bg->GetStatus() > STATUS_WAIT_QUEUE)
            return;

        // Leaving a rated arena after the invite counts as a game played and
        // lost. RemovePlayer below charges it, against the team's own arena
        // team id; this used to charge it here too, looking the team up by
        // faction - which never found one, or found somebody else's.
        _player->RemoveBattlegroundQueueId(bgQueueTypeId);  // must be called this way, because if you move this call to queue->removeplayer, it causes bugs
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_NONE, 0, 0, 0, 0);
        bgQueue.RemovePlayer(_player->GetGUID(), true);
        // player left queue, we should update it - do not update Arena Queue
        if (!ginfo.ArenaType)
            sBattlegroundMgr->ScheduleQueueUpdate(ginfo.ArenaMatchmakerRating, ginfo.ArenaType, bgQueueTypeId, bgTypeId, bracketId);
        SendPacket(&data);
        TC_LOG_DEBUG("bg.battleground", "Battleground: player {} {} left queue for bgtype {}, queue type {}.", _player->GetName(), _player->GetGUID().ToString(), bg->GetTypeID(), bgQueueTypeId);

        // track if player refuses to join the BG after being invited
        if (bg->isBattleground() && sWorld->getBoolConfig(CONFIG_BATTLEGROUND_TRACK_DESERTERS) &&
                (bg->GetStatus() == STATUS_IN_PROGRESS || bg->GetStatus() == STATUS_WAIT_JOIN))
        {
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_DESERTER_TRACK);
            stmt->setUInt32(0, _player->GetGUID().GetCounter());
            stmt->setUInt8(1, BG_DESERTION_TYPE_LEAVE_QUEUE);
            CharacterDatabase.Execute(stmt);
        }
    }
}

void WorldSession::HandleBattlefieldLeaveOpcode(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: Recvd CMSG_LEAVE_BATTLEFIELD Message");

    recvData.read_skip<uint8>();                           // unk1
    recvData.read_skip<uint8>();                           // unk2
    recvData.read_skip<uint32>();                          // BattlegroundTypeId
    recvData.read_skip<uint16>();                          // unk3

    // not allow leave battleground in combat
    /*
    if (_player->IsInCombat())
        if (Battleground* bg = _player->GetBattleground())
            if (bg->GetStatus() != STATUS_WAIT_LEAVE)
                return;
                */

    _player->LeaveBattleground();
}

void WorldSession::HandleBattlefieldStatusOpcode(WorldPacket & /*recvData*/)
{
    // empty opcode
    TC_LOG_DEBUG("network", "WORLD: Battleground status");

    WorldPacket data;
    // we must update all queues here
    Battleground* bg = nullptr;
    for (uint8 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId bgQueueTypeId = _player->GetBattlegroundQueueTypeId(i);
        if (!bgQueueTypeId)
            continue;
        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(bgQueueTypeId);
        uint8 arenaType = BattlegroundMgr::BGArenaType(bgQueueTypeId);
        if (bgTypeId == _player->GetBattlegroundTypeId())
        {
            bg = _player->GetBattleground();
            //i cannot check any variable from player class because player class doesn't know if player is in 2v2 / 3v3 / 4v4 or 5v5 arena
            //so i must use bg pointer to get that information
            if (bg && bg->GetArenaType() == arenaType)
            {
                // this line is checked, i only don't know if GetStartTime is changing itself after bg end!
                // send status in Battleground
                sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, i, STATUS_IN_PROGRESS, bg->GetEndTime(), bg->GetStartTime(), arenaType, _player->GetBGTeam());
                SendPacket(&data);
                continue;
            }
        }
        //we are sending update to player about queue - he can be invited there!
        //get GroupQueueInfo for queue status
        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
        GroupQueueInfo ginfo;
        if (!bgQueue.GetPlayerGroupInfoData(_player->GetGUID(), &ginfo))
            continue;
        if (ginfo.IsInvitedToBGInstanceGUID)
        {
            bg = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, bgTypeId);
            if (!bg)
                continue;
            uint32 remainingTime = getMSTimeDiff(GameTime::GetGameTimeMS(), ginfo.RemoveInviteTime);
            // send status invited to Battleground
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, i, STATUS_WAIT_JOIN, remainingTime, 0, arenaType, 0);
            SendPacket(&data);
        }
        else
        {
            bg = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
            if (!bg)
                continue;

            // expected bracket entry
            PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), _player->GetLevel());
            if (!bracketEntry)
                continue;

            uint32 avgTime = bgQueue.GetAverageQueueWaitTime(&ginfo, bracketEntry->GetBracketId());
            // send status in Battleground Queue
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, i, STATUS_WAIT_QUEUE, avgTime, getMSTimeDiff(ginfo.JoinTime, GameTime::GetGameTimeMS()), arenaType, 0);
            SendPacket(&data);
        }
    }

    // Spectators and replay viewers hold no queue entries, so the loop above
    // stays silent for them. The client discards its battlefield status on
    // every loading screen and /reload and rebuilds it from this reply; if we
    // don't answer, a spectator's client never flags the battleground as
    // active, which keeps the scoreboard toggle and the minimap battlefield
    // icon disabled. (This is why pushing the status packet before the
    // spectator's teleport never stuck.) SendSpectatorBattlefieldStatusTo
    // no-ops for arenas -- see its isArena() guard.
    if (_player->IsSpectator())
        if (Battleground* spectatedBg = _player->GetBattleground())
            if (spectatedBg->IsCustomGame() || spectatedBg->IsReplay())
                spectatedBg->SendSpectatorBattlefieldStatusTo(_player);
}

void WorldSession::HandleBattlemasterJoinArena(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "WORLD: CMSG_BATTLEMASTER_JOIN_ARENA");

    ObjectGuid guid;                                        // arena Battlemaster guid
    uint8 arenaslot;                                        // 2v2, 3v3, 4v4 or 5v5
    uint8 asGroup;                                          // asGroup
    uint8 isRated;                                          // isRated
    Group* grp = nullptr;

    recvData >> guid >> arenaslot >> asGroup >> isRated;

    if (RejectNormalBattlegroundQueueFromCustomLobby(this))
        return;

    // ignore if we already in BG or BG queue
    if (_player->InBattleground())
        return;

    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_BATTLEMASTER);
    if (!unit)
        return;

    uint8 arenatype = 0;
    uint32 arenaRating = 0;
    uint32 matchmakerRating = 0;
    uint32 previousOpponents = 0;

    switch (arenaslot)
    {
        case 0:
            arenatype = ARENA_TYPE_2v2;
            break;
        case 1:
            arenatype = ARENA_TYPE_3v3;
            break;
        case 2:
            arenatype = ARENA_TYPE_4v4;
            break;
        case 3:
            arenatype = ARENA_TYPE_5v5;
            break;
        default:
            TC_LOG_ERROR("network", "Unknown arena slot {} at HandleBattlemasterJoinArena()", arenaslot);
            return;
    }

    // The arena battlemaster queues for All Arenas. One standing for a squad
    // match (Baradin Hold: Four Corners) queues for that, rated or skirmish,
    // each row of the window being a squad size - an arena team of that size
    // is one squad.
    BattlegroundTypeId queueBgTypeId = BATTLEGROUND_AA;
    if (BattlegroundTypeId const masterBgTypeId = sBattlegroundMgr->GetBattleMasterBG(unit->GetEntry());
        sBattlegroundMgr->IsSquadBattleground(masterBgTypeId) || masterBgTypeId == BATTLEGROUND_SSH)
        queueBgTypeId = masterBgTypeId;

    // Buffle Shuffle's battlemaster takes two queues: a 2v2 or a 3v3 skirmish,
    // alone or with a party of up to a whole match - four or six, who are
    // dealt into the teams like anyone else. Its window only offers those
    // rows, so anything else is an old client.
    bool const shuffle = queueBgTypeId == BATTLEGROUND_SSH;
    if (shuffle && (isRated || (arenatype != ARENA_TYPE_2v2 && arenatype != ARENA_TYPE_3v3)))
    {
        ChatHandler(this).SendSysMessage("Buffle Shuffle is a 2v2 or 3v3 skirmish.");
        return;
    }

    // A rated arena where the realm fills rated arenas with clones is queued
    // like a skirmish: alone, or by a party smaller than the arena, and the
    // rest of both sides is made up with clones. Squad matches keep their own
    // rules. Anywhere else a rated arena is only ever queued by a party.
    bool const ratedFill = isRated && queueBgTypeId == BATTLEGROUND_AA && sBattlegroundMgr->IsBotFillRatedArena(arenatype);
    if (ratedFill && asGroup && !_player->GetGroup())
        asGroup = 0;                                        // "Join as Group" with no group: alone
    if (isRated && !asGroup && !ratedFill)
        return;

    // The client numbers the arena rows 2v2, 3v3, 4v4, 5v5; a character's
    // team slots are 2v2, 3v3, 5v5, 4v4 (ArenaTeam::GetSlotByType).
    uint8 const teamSlot = ArenaTeam::GetSlotByType(arenatype);

    //check existance
    Battleground* bg = sBattlegroundMgr->GetBattlegroundTemplate(queueBgTypeId);
    if (!bg)
    {
        TC_LOG_ERROR("network", "Battleground: template bg {} not found", uint32(queueBgTypeId));
        return;
    }

    if (DisableMgr::IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, queueBgTypeId, nullptr))
    {
        ChatHandler(this).PSendSysMessage(LANG_ARENA_DISABLED);
        return;
    }

    BattlegroundTypeId bgTypeId = bg->GetTypeID();
    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, arenatype);
    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bg->GetMapId(), _player->GetLevel());
    if (!bracketEntry)
        return;

    GroupJoinBattlegroundResult err = ERR_GROUP_JOIN_BATTLEGROUND_FAIL;

    if (!asGroup)
    {
        if (_player->isUsingLfg())
        {
            // player is using dungeon finder or raid finder
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, ERR_LFG_CANT_USE_BATTLEGROUND);
            _player->SendDirectMessage(&data);
            return;
        }

        if (!_player->CanJoinToBattleground(bg))
        {
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, ERR_BATTLEGROUND_JOIN_FAILED);
            _player->SendDirectMessage(&data);
            return;
        }

        // check if already in queue
        if (_player->GetBattlegroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
            //player is already in this queue
            return;
        // check if has free queue slots
        if (!_player->HasFreeBattlegroundQueueId())
            return;
    }
    else
    {
        grp = _player->GetGroup();
        // no group found, error
        if (!grp)
            return;
        if (grp->GetLeaderGUID() != _player->GetGUID())
            return;
        uint32 const partySize = shuffle ? uint32(arenatype) * 2 : uint32(arenatype);
        err = grp->CanJoinBattlegroundQueue(bg, bgQueueTypeId, ratedFill ? 1 : partySize, partySize, isRated != 0, teamSlot);
    }

    uint32 ateamId = 0;

    if (isRated)
    {
        ateamId = _player->GetArenaTeamId(teamSlot);
        // check real arenateam existence only here (if it was moved to group->CanJoin .. () then we would ahve to get it twice)
        ArenaTeam* at = sArenaTeamMgr->GetArenaTeamById(ateamId);

        // A side short of the arena size is made up with clones, which
        // somebody who unticked "Arena bots" has said no to. Told now - and
        // before any team is made for them - rather than left to wait for a
        // match that would never be made.
        uint32 const people = asGroup ? grp->GetMembersCount() : 1;
        if (ratedFill && people < arenatype && (!asGroup || err > 0))
        {
            bool optedOut = asGroup ? false : _player->HasArenaBotFillOptOut();
            if (asGroup)
                for (GroupReference* itr = grp->GetFirstMember(); itr != nullptr && !optedOut; itr = itr->next())
                    if (Player* member = itr->GetSource())
                        optedOut = member->HasArenaBotFillOptOut();

            if (optedOut)
            {
                ChatHandler(this).PSendSysMessage("A rated %uv%u with fewer than %u of you is made up with bots, and \"Arena bots\" is unticked in the Battlegrounds tab.",
                    uint32(arenatype), uint32(arenatype), uint32(arenatype));
                return;
            }
        }

        // Alone with no team of this size: where the realm fills rated arenas
        // the character is made a team of one, at the level a charter asks for.
        // A party is never given one as a side effect - its members would not
        // be in it, and the team rule refuses them.
        if (!at && ratedFill && !asGroup && sBattlegroundMgr->AutoCreatesRatedArenaTeams())
        {
            if (!_player->IsMaxLevel())
            {
                ChatHandler(this).PSendSysMessage("Rated arenas are for level %u characters.",
                    sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
                return;
            }

            if ((at = sArenaTeamMgr->CreateSoloArenaTeam(_player, arenatype)))
            {
                ateamId = at->GetId();
                at->Query(this);
                at->Roster(this);
                at->SendStats(this);
                ChatHandler(this).PSendSysMessage("You are now the captain of the %uv%u arena team \"%s\". Its rating is yours to build.",
                    uint32(arenatype), uint32(arenatype), at->GetName().c_str());
            }
        }

        if (!at)
        {
            _player->GetSession()->SendNotInArenaTeamPacket(arenatype);
            if (ratedFill && asGroup)
                ChatHandler(this).PSendSysMessage("A party queues rated in its leader's %uv%u arena team. Queue alone to have one made for you, then invite your party into it.",
                    uint32(arenatype), uint32(arenatype));
            return;
        }

        // get the team rating for queueing
        arenaRating = at->GetRating();
        // A team's average over nobody is 0, which the queue reads as unrated:
        // alone, it is the character's own matchmaker rating.
        if (asGroup)
            matchmakerRating = at->GetAverageMMR(grp);
        else if (ArenaTeamMember const* member = at->GetMember(_player->GetGUID()))
            matchmakerRating = member->MatchMakerRating;
        matchmakerRating = std::max<uint32>(matchmakerRating, 1);
        // the arenateam id must match for everyone in the group

        if (arenaRating <= 0)
            arenaRating = 1;

        previousOpponents = at->GetPreviousOpponents();
    }

    BattlegroundQueue &bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
    if (asGroup)
    {
        uint32 avgTime = 0;
        uint32 shuffleParty = 0;

        if (err > 0)
        {
            TC_LOG_DEBUG("bg.battleground", "Battleground: arena join as group start");
            if (isRated)
            {
                TC_LOG_DEBUG("bg.battleground", "Battleground: arena team id {}, leader {} queued with matchmaker rating {} for type {}", _player->GetArenaTeamId(teamSlot), _player->GetName(), matchmakerRating, arenatype);
                bg->SetRated(true);
            }
            else
                bg->SetRated(false);

            // A shuffle deals its sides per person, so a party is queued as
            // one entry per member below, tied together to arrive in one match.
            if (shuffle)
                shuffleParty = bgQueue.NewShuffleParty();
            else
            {
                GroupQueueInfo* ginfo = bgQueue.AddGroup(_player, grp, bgTypeId, bracketEntry, arenatype, isRated != 0, false, arenaRating, matchmakerRating, ateamId, previousOpponents);
                avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
            }
        }

        for (GroupReference* itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member)
                continue;

            WorldPacket data;

            if (err <= 0)
            {
                sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, err);
                member->SendDirectMessage(&data);
                continue;
            }

            if (shuffleParty)
            {
                GroupQueueInfo* ginfo = bgQueue.AddGroup(member, nullptr, bgTypeId, bracketEntry, arenatype, false, false, 0, 0);
                ginfo->ShuffleParty = shuffleParty;
                avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
            }

            // add to queue
            uint32 queueSlot = member->AddBattlegroundQueueId(bgQueueTypeId);

            // send status packet (in queue)
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, arenatype, 0);
            member->SendDirectMessage(&data);
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, err);
            member->SendDirectMessage(&data);
            TC_LOG_DEBUG("bg.battleground", "Battleground: player joined queue for arena as group bg queue type {} bg type {}: {}, NAME {}", bgQueueTypeId, bgTypeId, member->GetGUID().ToString(), member->GetName());
        }
    }
    else
    {
        // the queue status reads "rated" off the template (a solo rated join
        // exists only where rated arenas are bot-filled)
        bg->SetRated(isRated != 0);
        GroupQueueInfo* ginfo = bgQueue.AddGroup(_player, nullptr, bgTypeId, bracketEntry, arenatype, isRated != 0, false, arenaRating, matchmakerRating, ateamId, previousOpponents);
        uint32 avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
        uint32 queueSlot = _player->AddBattlegroundQueueId(bgQueueTypeId);

        WorldPacket data;
        // send status packet (in queue)
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0, arenatype, 0);
        SendPacket(&data);
        TC_LOG_DEBUG("bg.battleground", "Battleground: player joined queue for arena, skirmish, bg queue type {} bg type {}: {}, NAME {}", bgQueueTypeId, bgTypeId, _player->GetGUID().ToString(), _player->GetName());
    }
    sBattlegroundMgr->ScheduleQueueUpdate(matchmakerRating, arenatype, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());
}

void WorldSession::HandleReportPvPAFK(WorldPacket& recvData)
{
    ObjectGuid playerGuid;
    recvData >> playerGuid;
    Player* reportedPlayer = ObjectAccessor::FindPlayer(playerGuid);

    if (!reportedPlayer)
    {
        TC_LOG_INFO("bg.reportpvpafk", "WorldSession::HandleReportPvPAFK: {} [IP: {}] reported {}", _player->GetName(), _player->GetSession()->GetRemoteAddress(), playerGuid.ToString());
        return;
    }

    TC_LOG_DEBUG("bg.battleground", "WorldSession::HandleReportPvPAFK: {} reported {}", _player->GetName(), reportedPlayer->GetName());

    reportedPlayer->ReportedAfkBy(_player);
}
