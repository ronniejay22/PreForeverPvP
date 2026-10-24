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

#include "ArenaTeamMgr.h"
#include "BattlegroundMgr.h"
#include "BattlegroundAV.h"
#include "BattlegroundAB.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "BattlegroundNA.h"
#include "BattlegroundNL.h"
#include "BattlegroundBE.h"
#include "BattlegroundRL.h"
#include "BattlegroundSA.h"
#include "BattlegroundDS.h"
#include "BattlegroundRV.h"
#include "BattlegroundIC.h"
#include "BattlegroundSCM.h"
#include "BattlegroundBRT.h"
#include "BattlegroundOBC.h"
#include "BattlegroundTRT.h"
#include "BattlegroundVHR.h"
#include "BattlegroundSRF.h"
#include "BattlegroundTOK.h"
#include "BattlegroundSV.h"
#include "BattlegroundTP.h"
#include "BattlegroundBFG.h"
#include "BattlegroundTTP.h"
#include "BattlegroundTV.h"
#include "BattlegroundCustomArena.h"
#include "Common.h"
#include "Containers.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "DisableMgr.h"
#include "Formulas.h"
#include "GameEventMgr.h"
#include "GameTime.h"
#include "Language.h"
#include "Map.h"
#include "MapManager.h"
#include "MiscPackets.h"
#include "SharedDefines.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "StringConvert.h"
#include "Util.h"
#include <algorithm>
#include <array>

bool BattlegroundTemplate::IsArena() const
{
    return BattlemasterEntry->InstanceType == MAP_ARENA;
}

namespace
{
    // `battleground_bounds` is optional: a world database that has never had it
    // just runs every battleground without bounds. Asked through
    // information_schema first because querying a table that does not exist is
    // not an empty result in this core - errno 1146 aborts the worldserver.
    std::unordered_map<uint32, BattlegroundBounds> LoadBattlegroundBounds()
    {
        std::unordered_map<uint32, BattlegroundBounds> bounds;

        if (!WorldDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'battleground_bounds' LIMIT 1"))
        {
            TC_LOG_INFO("server.loading", ">> Loaded 0 battleground bounds. DB table `battleground_bounds` does not exist.");
            return bounds;
        }

        //                                                   0         1     2       3       4      5      6      7        8
        QueryResult result = WorldDatabase.Query("SELECT BgTypeId, MinZ, ProbeZ, KeepXY, SafeX, SafeY, SafeZ, GraceMs, Boundary FROM battleground_bounds");
        if (!result)
        {
            TC_LOG_INFO("server.loading", ">> Loaded 0 battleground bounds. DB table `battleground_bounds` is empty.");
            return bounds;
        }

        do
        {
            Field* fields = result->Fetch();
            uint32 const bgTypeId = fields[0].GetUInt32();

            BattlegroundBounds entry;
            entry.Enabled = true;
            entry.MinZ    = fields[1].GetFloat();
            entry.ProbeZ  = fields[2].GetFloat();
            entry.KeepXY  = fields[3].GetUInt8() != 0;
            entry.SafePosition.Relocate(fields[4].GetFloat(), fields[5].GetFloat(), fields[6].GetFloat());
            entry.GraceMs = fields[7].GetUInt32();

            // "x y, x y, x y ..." in world coordinates; fewer than three points
            // is no outline at all, and a malformed one is refused whole rather
            // than half-used.
            std::string const outline = fields[8].GetString();
            bool valid = true;
            for (std::string_view point : Trinity::Tokenize(outline, ',', false))
            {
                std::vector<std::string_view> xy = Trinity::Tokenize(point, ' ', false);
                Optional<float> x = xy.size() == 2 ? Trinity::StringTo<float>(xy[0]) : Optional<float>();
                Optional<float> y = xy.size() == 2 ? Trinity::StringTo<float>(xy[1]) : Optional<float>();
                if (!x || !y)
                {
                    valid = false;
                    break;
                }
                entry.Boundary.push_back({ *x, *y });
            }

            if (!valid || (!entry.Boundary.empty() && entry.Boundary.size() < 3))
            {
                TC_LOG_ERROR("sql.sql", "Table `battleground_bounds` has an unusable Boundary for BgTypeId {}: \"{}\". The outline is ignored; the height check still applies.", bgTypeId, outline);
                entry.Boundary.clear();
            }

            bounds[bgTypeId] = std::move(entry);
        }
        while (result->NextRow());

        TC_LOG_INFO("server.loading", ">> Loaded {} battleground bounds", uint32(bounds.size()));
        return bounds;
    }

    // `battleground_drop_exit`: ledges that bots are put down from (see
    // BattlegroundDropExit). Optional the same way `battleground_bounds` is.
    std::unordered_map<uint32, std::vector<BattlegroundDropExit>> LoadBattlegroundDropExits()
    {
        std::unordered_map<uint32, std::vector<BattlegroundDropExit>> exits;

        if (!WorldDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'battleground_drop_exit' LIMIT 1"))
        {
            TC_LOG_INFO("server.loading", ">> Loaded 0 battleground drop exits. DB table `battleground_drop_exit` does not exist.");
            return exits;
        }

        //                                                   0         1        2        3             4           5      6      7      8         9         10
        QueryResult result = WorldDatabase.Query("SELECT BgTypeId, IslandX, IslandY, IslandRadius, IslandMinZ, EdgeX, EdgeY, EdgeZ, LandingX, LandingY, LandingZ FROM battleground_drop_exit ORDER BY BgTypeId, Idx");
        if (!result)
        {
            TC_LOG_INFO("server.loading", ">> Loaded 0 battleground drop exits. DB table `battleground_drop_exit` is empty.");
            return exits;
        }

        uint32 count = 0;
        do
        {
            Field* fields = result->Fetch();

            BattlegroundDropExit exit;
            exit.IslandCentre.Relocate(fields[1].GetFloat(), fields[2].GetFloat(), fields[4].GetFloat());
            exit.IslandRadius = fields[3].GetFloat();
            exit.IslandMinZ   = fields[4].GetFloat();
            exit.Edge.Relocate(fields[5].GetFloat(), fields[6].GetFloat(), fields[7].GetFloat());
            exit.Landing.Relocate(fields[8].GetFloat(), fields[9].GetFloat(), fields[10].GetFloat());

            exits[fields[0].GetUInt32()].push_back(exit);
            ++count;
        }
        while (result->NextRow());

        TC_LOG_INFO("server.loading", ">> Loaded {} battleground drop exits for {} battlegrounds", count, uint32(exits.size()));
        return exits;
    }

    // `battleground_start_exit`: where a team is put when the gates open (see
    // BattlegroundStartExit). Optional the same way `battleground_bounds` is.
    std::unordered_map<uint32, std::array<BattlegroundStartExit, PVP_TEAMS_COUNT>> LoadBattlegroundStartExits()
    {
        std::unordered_map<uint32, std::array<BattlegroundStartExit, PVP_TEAMS_COUNT>> exits;

        if (!WorldDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'battleground_start_exit' LIMIT 1"))
        {
            TC_LOG_INFO("server.loading", ">> Loaded 0 battleground start exits. DB table `battleground_start_exit` does not exist.");
            return exits;
        }

        //                                                   0         1       2  3  4  5  6       7        8
        QueryResult result = WorldDatabase.Query("SELECT BgTypeId, TeamId, X, Y, Z, O, Radius, DelayMs, BotsOnly FROM battleground_start_exit");
        if (!result)
        {
            TC_LOG_INFO("server.loading", ">> Loaded 0 battleground start exits. DB table `battleground_start_exit` is empty.");
            return exits;
        }

        uint32 count = 0;
        do
        {
            Field* fields = result->Fetch();
            uint32 const bgTypeId = fields[0].GetUInt32();
            uint8 const teamId = fields[1].GetUInt8();
            if (teamId >= PVP_TEAMS_COUNT)
            {
                TC_LOG_ERROR("sql.sql", "Table `battleground_start_exit` has TeamId {} for BgTypeId {}; only 0 (Alliance start) and 1 (Horde start) exist. Skipped.", uint32(teamId), bgTypeId);
                continue;
            }

            BattlegroundStartExit& exit = exits[bgTypeId][teamId];
            exit.Enabled  = true;
            exit.Exit.Relocate(fields[2].GetFloat(), fields[3].GetFloat(), fields[4].GetFloat(), fields[5].GetFloat());
            exit.Radius   = fields[6].GetFloat();
            exit.DelayMs  = fields[7].GetUInt32();
            exit.BotsOnly = fields[8].GetUInt8() != 0;
            ++count;
        }
        while (result->NextRow());

        TC_LOG_INFO("server.loading", ">> Loaded {} battleground start exits for {} battlegrounds", count, uint32(exits.size()));
        return exits;
    }

    // `battleground_squad_start`: the squads of a battleground played by more
    // than two teams (see BattlegroundSquadStart), one row per squad. Optional
    // the same way `battleground_bounds` is. A type is only given squads when
    // its rows are 0..N-1 with no gap and 2 <= N <= MAX_BATTLEGROUND_SQUADS;
    // anything else is reported and the type stays a two-team battleground.
    std::unordered_map<uint32, std::vector<BattlegroundSquadStart>> LoadBattlegroundSquadStarts()
    {
        std::unordered_map<uint32, std::vector<BattlegroundSquadStart>> squads;

        if (!WorldDatabase.Query("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'battleground_squad_start' LIMIT 1"))
        {
            TC_LOG_INFO("server.loading", ">> Loaded 0 battleground squads. DB table `battleground_squad_start` does not exist.");
            return squads;
        }

        //                                                   0         1      2  3  4  5  6     7
        QueryResult result = WorldDatabase.Query("SELECT BgTypeId, Squad, X, Y, Z, O, Name, FlagSpell FROM battleground_squad_start ORDER BY BgTypeId, Squad");
        if (!result)
        {
            TC_LOG_INFO("server.loading", ">> Loaded 0 battleground squads. DB table `battleground_squad_start` is empty.");
            return squads;
        }

        std::unordered_map<uint32, bool> broken;
        do
        {
            Field* fields = result->Fetch();
            uint32 const bgTypeId = fields[0].GetUInt32();
            uint8 const squad = fields[1].GetUInt8();
            std::vector<BattlegroundSquadStart>& starts = squads[bgTypeId];
            if (squad != starts.size() || squad >= MAX_BATTLEGROUND_SQUADS)
            {
                TC_LOG_ERROR("sql.sql", "Table `battleground_squad_start` has Squad {} for BgTypeId {}; squads must be numbered 0..{} without gaps. The battleground keeps two teams.",
                    uint32(squad), bgTypeId, MAX_BATTLEGROUND_SQUADS - 1);
                broken[bgTypeId] = true;
                continue;
            }

            BattlegroundSquadStart start;
            start.Start.Relocate(fields[2].GetFloat(), fields[3].GetFloat(), fields[4].GetFloat(), fields[5].GetFloat());
            start.Name = fields[6].GetString();
            start.FlagSpell = fields[7].GetUInt32();
            if (start.FlagSpell && !sSpellMgr->GetSpellInfo(start.FlagSpell))
            {
                TC_LOG_ERROR("sql.sql", "Table `battleground_squad_start` has FlagSpell {} for BgTypeId {} squad {}, which does not exist. The squad carries its side's flag.",
                    start.FlagSpell, bgTypeId, uint32(squad));
                start.FlagSpell = 0;
            }
            starts.push_back(std::move(start));
        }
        while (result->NextRow());

        uint32 count = 0;
        for (auto itr = squads.begin(); itr != squads.end();)
        {
            if (broken.count(itr->first) || itr->second.size() < 2)
            {
                if (!broken.count(itr->first))
                    TC_LOG_ERROR("sql.sql", "Table `battleground_squad_start` has a single squad for BgTypeId {}; a squad match needs at least two. The battleground keeps two teams.", itr->first);
                itr = squads.erase(itr);
                continue;
            }

            count += uint32(itr->second.size());
            ++itr;
        }

        TC_LOG_INFO("server.loading", ">> Loaded {} battleground squads for {} battlegrounds", count, uint32(squads.size()));
        return squads;
    }
}

/*********************************************************/
/***            BATTLEGROUND MANAGER                   ***/
/*********************************************************/

BattlegroundMgr::BattlegroundMgr() :
    m_NextPeriodicQueueUpdateTime(BATTLEGROUND_QUEUE_UPDATE_INTERVAL),
    m_NextRatedArenaUpdate(sWorld->getIntConfig(CONFIG_ARENA_RATED_UPDATE_TIMER)),
    m_NextAutoDistributionTime(0),
    m_AutoDistributionTimeChecker(0), m_UpdateTimer(0), m_SquadFeedTimer(0), m_ArenaTesting(false), m_Testing(false)
{ }

BattlegroundMgr::~BattlegroundMgr()
{
    DeleteAllBattlegrounds();
}

void BattlegroundMgr::DeleteAllBattlegrounds()
{
    for (BattlegroundDataContainer::iterator itr1 = bgDataStore.begin(); itr1 != bgDataStore.end(); ++itr1)
    {
        BattlegroundData& data = itr1->second;

        while (!data.m_Battlegrounds.empty())
            delete data.m_Battlegrounds.begin()->second;
        data.m_Battlegrounds.clear();

        while (!data.BGFreeSlotQueue.empty())
            delete data.BGFreeSlotQueue.front();
    }

    bgDataStore.clear();
}

BattlegroundMgr* BattlegroundMgr::instance()
{
    static BattlegroundMgr instance;
    return &instance;
}

// used to update running battlegrounds, and delete finished ones
void BattlegroundMgr::Update(uint32 diff)
{
    m_UpdateTimer += diff;
    if (m_UpdateTimer > BATTLEGROUND_OBJECTIVE_UPDATE_INTERVAL)
    {
        for (BattlegroundDataContainer::iterator itr1 = bgDataStore.begin(); itr1 != bgDataStore.end(); ++itr1)
        {
            BattlegroundContainer& bgs = itr1->second.m_Battlegrounds;
            BattlegroundContainer::iterator itrDelete = bgs.begin();
            // first one is template and should not be deleted
            for (BattlegroundContainer::iterator itr = ++itrDelete; itr != bgs.end();)
            {
                itrDelete = itr++;
                Battleground* bg = itrDelete->second;

                bg->Update(m_UpdateTimer);
                if (bg->ToBeDeleted())
                {
                    itrDelete->second = nullptr;
                    bgs.erase(itrDelete);
                    BattlegroundClientIdsContainer& clients = itr1->second.m_ClientBattlegroundIds[bg->GetBracketId()];
                    if (!clients.empty())
                        clients.erase(bg->GetClientInstanceID());

                    delete bg;
                }
            }
        }

        m_UpdateTimer = 0;
    }

    // A squad match's enemy frames want health more often than the objective
    // tick above gives it. Only the squad types are walked; the template of
    // each has no map and is skipped by the feed itself.
    m_SquadFeedTimer += diff;
    if (m_SquadFeedTimer >= BATTLEGROUND_SQUAD_FEED_INTERVAL)
    {
        m_SquadFeedTimer = 0;
        for (auto const& [bgTypeId, data] : bgDataStore)
        {
            // Buffle Shuffle feeds the same frames: its two sides change every
            // round, which the stock arena frames cannot follow.
            if (!IsSquadBattleground(bgTypeId) && bgTypeId != BATTLEGROUND_SSH)
                continue;

            for (auto const& [instanceId, bg] : data.m_Battlegrounds)
            {
                (void)instanceId;
                if (bg && !bg->ToBeDeleted())
                    bg->UpdateSquadFeed();
            }
        }
    }

    // update events timer
    for (int qtype = BATTLEGROUND_QUEUE_NONE; qtype < MAX_BATTLEGROUND_QUEUE_TYPES; ++qtype)
        m_BattlegroundQueues[qtype].UpdateEvents(diff);

    // update scheduled queues
    if (!m_QueueUpdateScheduler.empty())
    {
        std::vector<uint64> scheduled;
        std::swap(scheduled, m_QueueUpdateScheduler);

        for (uint8 i = 0; i < scheduled.size(); i++)
        {
            uint32 arenaMMRating = scheduled[i] >> 32;
            uint8 arenaType = scheduled[i] >> 24 & 255;
            BattlegroundQueueTypeId bgQueueTypeId = BattlegroundQueueTypeId(scheduled[i] >> 16 & 255);
            if (bgQueueTypeId >= MAX_BATTLEGROUND_QUEUE_TYPES)
            {
                TC_LOG_ERROR("bg.battleground", "BattlegroundMgr::Update: scheduled update for queue {} (of {}) dropped", uint32(bgQueueTypeId), uint32(MAX_BATTLEGROUND_QUEUE_TYPES));
                continue;
            }
            BattlegroundTypeId bgTypeId = BattlegroundTypeId((scheduled[i] >> 8) & 255);
            // Only the low byte of the type id fits the schedule. A queue that
            // serves one type names it whole, which is what restores ids past
            // 255 (Four Corners is 887); All Arenas and every type under 256
            // decode exactly as before.
            if (BattlegroundTypeId const queueTemplate = BGTemplateId(bgQueueTypeId);
                queueTemplate != BATTLEGROUND_TYPE_NONE && (uint32(queueTemplate) & 255) == uint32(bgTypeId))
                bgTypeId = queueTemplate;
            BattlegroundBracketId bracket_id = BattlegroundBracketId(scheduled[i] & 255);
            m_BattlegroundQueues[bgQueueTypeId].BattlegroundQueueUpdate(diff, bgTypeId, bracket_id, arenaType, arenaMMRating > 0, arenaMMRating);
        }
    }

    // periodic queue update for battlegrounds to keep invitations flowing when
    // slots open up without an explicit queue update trigger.
    if (m_NextPeriodicQueueUpdateTime <= diff)
    {
        for (int qtype = BATTLEGROUND_QUEUE_NONE; qtype < MAX_BATTLEGROUND_QUEUE_TYPES; ++qtype)
        {
            BattlegroundTypeId const bgTypeId = BattlegroundMgr::BGTemplateId(BattlegroundQueueTypeId(qtype));
            // A squad match's queue waits for squads to form (UpdateSquadPool),
            // and Buffle Shuffle's for a lobby to form (UpdateSoloShufflePool),
            // so both need the sweep, as battlegrounds do, to notice the wait
            // running out when nobody else joins.
            if (bgTypeId == BATTLEGROUND_TYPE_NONE ||
                (IsArenaType(bgTypeId) && !IsSquadBattleground(bgTypeId) && bgTypeId != BATTLEGROUND_SSH))
                continue;

            for (int bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
            {
                m_BattlegroundQueues[qtype].BattlegroundQueueUpdate(BATTLEGROUND_QUEUE_UPDATE_INTERVAL, bgTypeId,
                    BattlegroundBracketId(bracket), BGArenaType(BattlegroundQueueTypeId(qtype)));
            }
        }

        m_NextPeriodicQueueUpdateTime = BATTLEGROUND_QUEUE_UPDATE_INTERVAL;
    }
    else
        m_NextPeriodicQueueUpdateTime -= diff;

    // if rating difference counts, maybe force-update queues
    if (sWorld->getIntConfig(CONFIG_ARENA_MAX_RATING_DIFFERENCE) && sWorld->getIntConfig(CONFIG_ARENA_RATED_UPDATE_TIMER))
    {
        // it's time to force update
        if (m_NextRatedArenaUpdate < diff)
        {
            // forced update for rated arenas (scan all, but skipped non rated)
            TC_LOG_TRACE("bg.arena", "BattlegroundMgr: UPDATING ARENA QUEUES");
            for (int qtype = BATTLEGROUND_QUEUE_2v2; qtype <= BATTLEGROUND_QUEUE_5v5; ++qtype)
                for (int bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
                    m_BattlegroundQueues[qtype].BattlegroundQueueUpdate(diff,
                        BATTLEGROUND_AA, BattlegroundBracketId(bracket),
                        BattlegroundMgr::BGArenaType(BattlegroundQueueTypeId(qtype)), true, 0);

            m_NextRatedArenaUpdate = sWorld->getIntConfig(CONFIG_ARENA_RATED_UPDATE_TIMER);
        }
        else
            m_NextRatedArenaUpdate -= diff;
    }

    if (sWorld->getBoolConfig(CONFIG_ARENA_AUTO_DISTRIBUTE_POINTS))
    {
        if (m_AutoDistributionTimeChecker < diff)
        {
            if (GameTime::GetGameTime() > m_NextAutoDistributionTime)
            {
                sArenaTeamMgr->DistributeArenaPoints();

                time_t arenaDistributionTime = sWorld->getWorldState(WS_ARENA_DISTRIBUTION_TIME) == 0 ? m_NextAutoDistributionTime : time_t(sWorld->getWorldState(WS_ARENA_DISTRIBUTION_TIME));
                m_NextAutoDistributionTime = arenaDistributionTime + BATTLEGROUND_ARENA_POINT_DISTRIBUTION_DAY * sWorld->getIntConfig(CONFIG_ARENA_AUTO_DISTRIBUTE_INTERVAL_DAYS);
                sWorld->setWorldState(WS_ARENA_DISTRIBUTION_TIME, uint64(m_NextAutoDistributionTime));
            }
            m_AutoDistributionTimeChecker = 600000; // check 10 minutes
        }
        else
            m_AutoDistributionTimeChecker -= diff;
    }
}

void BattlegroundMgr::BuildBattlegroundStatusPacket(WorldPacket* data, Battleground* bg, uint8 QueueSlot, uint8 StatusID, uint32 Time1, uint32 Time2, uint8 arenatype, uint32 arenaFaction)
{
    // we can be in 2 queues in same time...

    if (StatusID == 0 || !bg)
    {
        data->Initialize(SMSG_BATTLEFIELD_STATUS, 4+8);
        *data << uint32(QueueSlot);                         // queue id (0...1)
        *data << uint64(0);
        return;
    }

    data->Initialize(SMSG_BATTLEFIELD_STATUS, (4+8+1+1+4+1+4+4+4));
    *data << uint32(QueueSlot);                             // queue id (0...1) - player can be in 2 queues in time
    // The following segment is read as uint64 in client but can be appended as their original type.
    *data << uint8(arenatype);
    TC_LOG_DEBUG("network", "BattlegroundMgr::BuildBattlegroundStatusPacket: arenatype = {} for bg instanceID {}, TypeID {}.", arenatype, bg->GetClientInstanceID(), bg->GetTypeID());
    *data << uint8(bg->isArena() ? 0xE : 0x0);
    *data << uint32(bg->GetTypeID());
    *data << uint16(0x1F90);
    // End of uint64 segment, decomposed this way for simplicity
    *data << uint8(bg->GetMinLevel());
    *data << uint8(bg->GetMaxLevel());
    *data << uint32(bg->GetClientInstanceID());
    // alliance/horde for BG and skirmish/rated for Arenas
    // following displays the minimap-icon 0 = faction icon 1 = arenaicon
    *data << uint8(bg->isRated());                              // 1 for rated match, 0 for bg or non rated match

    *data << uint32(StatusID);                                  // status
    switch (StatusID)
    {
        case STATUS_WAIT_QUEUE:                                 // status_in_queue
            *data << uint32(Time1);                             // average wait time, milliseconds
            *data << uint32(Time2);                             // time in queue, updated every minute!, milliseconds
            break;
        case STATUS_WAIT_JOIN:                                  // status_invite
            *data << uint32(bg->GetMapId());                    // map id
            *data << uint64(0);                                 // 3.3.5, unknown
            *data << uint32(Time1);                             // time to remove from queue, milliseconds
            break;
        case STATUS_IN_PROGRESS:                                // status_in_progress
            *data << uint32(bg->GetMapId());                    // map id
            *data << uint64(0);                                 // 3.3.5, unknown
            *data << uint32(Time1);                             // time to bg auto leave, 0 at bg start, 120000 after bg end, milliseconds
            *data << uint32(Time2);                             // time from bg start, milliseconds
            *data << uint8(arenaFaction == ALLIANCE ? 1 : 0);   // arenafaction (0 for horde, 1 for alliance)
            break;
        default:
            TC_LOG_ERROR("bg.battleground", "Unknown BG status!");
            break;
    }
}

void BattlegroundMgr::BuildGroupJoinedBattlegroundPacket(WorldPacket* data, GroupJoinBattlegroundResult result)
{
    data->Initialize(SMSG_GROUP_JOINED_BATTLEGROUND, 4);
    *data << int32(result);
    if (result == ERR_BATTLEGROUND_JOIN_TIMED_OUT || result == ERR_BATTLEGROUND_JOIN_FAILED)
        *data << uint64(0);                                 // player guid
}

void BattlegroundMgr::BuildPlayerLeftBattlegroundPacket(WorldPacket* data, ObjectGuid guid)
{
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_LEFT, 8);
    *data << uint64(guid);
}

void BattlegroundMgr::BuildPlayerJoinedBattlegroundPacket(WorldPacket* data, Player* player)
{
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_JOINED, 8);
    *data << uint64(player->GetGUID());
}

Battleground* BattlegroundMgr::GetBattlegroundThroughClientInstance(uint32 instanceId, BattlegroundTypeId bgTypeId)
{
    //cause at HandleBattlegroundJoinOpcode the clients sends the instanceid he gets from
    //SMSG_BATTLEFIELD_LIST we need to find the battleground with this clientinstance-id
    Battleground* bg = GetBattlegroundTemplate(bgTypeId);
    if (!bg)
        return nullptr;

    if (bg->isArena())
        return GetBattleground(instanceId, bgTypeId);

    BattlegroundDataContainer::const_iterator it = bgDataStore.find(bgTypeId);
    if (it == bgDataStore.end())
        return nullptr;

    for (BattlegroundContainer::const_iterator itr = it->second.m_Battlegrounds.begin(); itr != it->second.m_Battlegrounds.end(); ++itr)
    {
        if (itr->second->GetClientInstanceID() == instanceId)
            return itr->second;
    }

    return nullptr;
}

Battleground* BattlegroundMgr::GetBattleground(uint32 instanceId, BattlegroundTypeId bgTypeId)
{
    if (!instanceId)
        return nullptr;

    BattlegroundDataContainer::const_iterator begin, end;

    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
    {
        begin = bgDataStore.begin();
        end = bgDataStore.end();
    }
    else
    {
        end = bgDataStore.find(bgTypeId);
        if (end == bgDataStore.end())
            return nullptr;
        begin = end++;
    }

    for (BattlegroundDataContainer::const_iterator it = begin; it != end; ++it)
    {
        BattlegroundContainer const& bgs = it->second.m_Battlegrounds;
        BattlegroundContainer::const_iterator itr = bgs.find(instanceId);
        if (itr != bgs.end())
           return itr->second;
    }

    return nullptr;
}

Battleground* BattlegroundMgr::GetBattlegroundTemplate(BattlegroundTypeId bgTypeId)
{
    BattlegroundDataContainer::const_iterator itr = bgDataStore.find(bgTypeId);
    if (itr == bgDataStore.end())
        return nullptr;

    BattlegroundContainer const& bgs = itr->second.m_Battlegrounds;
    //map is sorted and we can be sure that lowest instance id has only BG template
    return bgs.empty() ? nullptr : bgs.begin()->second;
}

uint32 BattlegroundMgr::CreateClientVisibleInstanceId(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id)
{
    if (IsArenaType(bgTypeId))
        return 0;                                           //arenas don't have client-instanceids

    // we create here an instanceid, which is just for
    // displaying this to the client and without any other use..
    // the client-instanceIds are unique for each battleground-type
    // the instance-id just needs to be as low as possible, beginning with 1
    // the following works, because std::set is default ordered with "<"
    // the optimalization would be to use as bitmask std::vector<uint32> - but that would only make code unreadable

    BattlegroundClientIdsContainer& clientIds = bgDataStore[bgTypeId].m_ClientBattlegroundIds[bracket_id];
    uint32 lastId = 0;
    for (BattlegroundClientIdsContainer::const_iterator itr = clientIds.begin(); itr != clientIds.end();)
    {
        if ((++lastId) != *itr)                             //if there is a gap between the ids, we will break..
            break;
        lastId = *itr;
    }

    clientIds.insert(++lastId);
    return lastId;
}

// create a new battleground that will really be used to play
Battleground* BattlegroundMgr::CreateNewBattleground(BattlegroundTypeId originalBgTypeId, PvPDifficultyEntry const* bracketEntry,
    uint8 arenaType, bool isRated, bool isPrivate, bool hasBotParticipants)
{
    BattlegroundTypeId bgTypeId = GetRandomBG(originalBgTypeId, hasBotParticipants && !isPrivate);

    // get the template BG
    Battleground* bg_template = GetBattlegroundTemplate(bgTypeId);

    if (!bg_template)
    {
        TC_LOG_ERROR("bg.battleground", "Battleground: CreateNewBattleground - bg template not found for {}", bgTypeId);
        return nullptr;
    }

    Battleground* bg = nullptr;

    // The data-driven arenas are a contiguous id range served by one class, so
    // they are handled before the switch rather than as fourteen identical case
    // labels. Adding another arena does not touch this function.
    if (IsDataDrivenArena(bgTypeId))
        bg = new BattlegroundCustomArena(*(BattlegroundCustomArena*)bg_template);
    else switch (bgTypeId) // create a copy of the BG template
    {
        case BATTLEGROUND_AV:
            bg = new BattlegroundAV(*(BattlegroundAV*)bg_template);
            break;
        case BATTLEGROUND_WS:
            bg = new BattlegroundWS(*(BattlegroundWS*)bg_template);
            break;
        case BATTLEGROUND_AB:
            bg = new BattlegroundAB(*(BattlegroundAB*)bg_template);
            break;
        case BATTLEGROUND_NA:
            bg = new BattlegroundNA(*(BattlegroundNA*)bg_template);
            break;
        case BATTLEGROUND_BE:
            bg = new BattlegroundBE(*(BattlegroundBE*)bg_template);
            break;
        case BATTLEGROUND_EY:
            bg = new BattlegroundEY(*(BattlegroundEY*)bg_template);
            break;
        case BATTLEGROUND_RL:
            bg = new BattlegroundRL(*(BattlegroundRL*)bg_template);
            break;
        case BATTLEGROUND_NL:
            bg = new BattlegroundNL(*(BattlegroundNL*)bg_template);
            break;
        case BATTLEGROUND_SA:
            bg = new BattlegroundSA(*(BattlegroundSA*)bg_template);
            break;
        case BATTLEGROUND_DS:
            bg = new BattlegroundDS(*(BattlegroundDS*)bg_template);
            break;
        case BATTLEGROUND_RV:
            bg = new BattlegroundRV(*(BattlegroundRV*)bg_template);
            break;
        case BATTLEGROUND_IC:
            bg = new BattlegroundIC(*(BattlegroundIC*)bg_template);
            break;
        case BATTLEGROUND_SCM:
            bg = new BattlegroundSCM(*(BattlegroundSCM*)bg_template);
            break;
        case BATTLEGROUND_BRT:
            bg = new BattlegroundBRT(*(BattlegroundBRT*)bg_template);
            break;
        case BATTLEGROUND_OBC:
            bg = new BattlegroundOBC(*(BattlegroundOBC*)bg_template);
            break;
        case BATTLEGROUND_TRT:
            bg = new BattlegroundTRT(*(BattlegroundTRT*)bg_template);
            break;
        case BATTLEGROUND_VHR:
            bg = new BattlegroundVHR(*(BattlegroundVHR*)bg_template);
            break;
        case BATTLEGROUND_TOK:
            bg = new BattlegroundTOK(*(BattlegroundTOK*)bg_template);
            break;
        case BATTLEGROUND_SRF:
            bg = new BattlegroundSRF(*(BattlegroundSRF*)bg_template);
            break;
        case BATTLEGROUND_SV:
            bg = new BattlegroundSV(*(BattlegroundSV*)bg_template);
            break;
        case BATTLEGROUND_TP:
            bg = new BattlegroundTP(*(BattlegroundTP*)bg_template);
            break;
        case BATTLEGROUND_BFG:
            bg = new BattlegroundBFG(*(BattlegroundBFG*)bg_template);
            break;
        case BATTLEGROUND_TV:
            bg = new BattlegroundTV(*(BattlegroundTV*)bg_template);
            break;
        case BATTLEGROUND_TTP:
            bg = new BattlegroundTTP(*(BattlegroundTTP*)bg_template);
            break;
        case BATTLEGROUND_RB:
        case BATTLEGROUND_AA:
        default:
            return nullptr;
    }

    bool isRandom = bgTypeId != originalBgTypeId && !bg->isArena();

    bg->SetBracket(bracketEntry);
    bg->SetInstanceID(sMapMgr->GenerateInstanceId());
    bg->SetMatchSerial(++_lastMatchSerial);
    // Private matches still need a non-zero ID in status packets, but their
    // ID is never inserted into m_ClientBattlegroundIds and therefore never
    // appears in the public battleground list.
    bg->SetClientInstanceID(isPrivate ? bg->GetInstanceID() : CreateClientVisibleInstanceId(originalBgTypeId, bracketEntry->GetBracketId()));
    bg->Reset();                     // reset the new bg (set status to status_wait_queue from status_none)
    bg->SetStatus(STATUS_WAIT_JOIN); // start the joining of the bg
    bg->SetArenaType(arenaType);
    bg->SetTypeID(originalBgTypeId);
    bg->SetRandomTypeID(bgTypeId);
    bg->SetRated(isRated);
    bg->SetRandom(isRandom);

    // Set up correct min/max player counts for scoreboards
    if (bg->isArena())
    {
        uint32 maxPlayersPerTeam = 0;
        switch (arenaType)
        {
            case ARENA_TYPE_2v2:
                maxPlayersPerTeam = 2;
                break;
            case ARENA_TYPE_3v3:
                maxPlayersPerTeam = 3;
                break;
            case ARENA_TYPE_4v4:
                maxPlayersPerTeam = 4;
                break;
            case ARENA_TYPE_5v5:
                maxPlayersPerTeam = 5;
                break;
        }

        // The arena size is a squad's. A side holds every squad seated on it:
        // two of the four in Four Corners.
        if (bg->IsSquadMatch())
            maxPlayersPerTeam *= (bg->GetSquadCount() + 1) / 2;

        bg->SetMaxPlayersPerTeam(maxPlayersPerTeam);
        bg->SetMaxPlayers(maxPlayersPerTeam * 2);
    }

    return bg;
}

// used to create the BG templates
bool BattlegroundMgr::CreateBattleground(BattlegroundTemplate const* bgTemplate)
{
    Battleground* bg = GetBattlegroundTemplate(bgTemplate->Id);
    if (!bg)
    {
        // Create the BG
        if (IsDataDrivenArena(bgTemplate->Id))
            bg = new BattlegroundCustomArena();
        else switch (bgTemplate->Id)
        {
            case BATTLEGROUND_AV:
                bg = new BattlegroundAV();
                break;
            case BATTLEGROUND_WS:
                bg = new BattlegroundWS();
                break;
            case BATTLEGROUND_AB:
                bg = new BattlegroundAB();
                break;
            case BATTLEGROUND_NA:
                bg = new BattlegroundNA();
                break;
            case BATTLEGROUND_BE:
                bg = new BattlegroundBE();
                break;
            case BATTLEGROUND_EY:
                bg = new BattlegroundEY();
                break;
            case BATTLEGROUND_RL:
                bg = new BattlegroundRL();
                break;
            case BATTLEGROUND_NL:
                bg = new BattlegroundNL();
                break;
            case BATTLEGROUND_SA:
                bg = new BattlegroundSA();
                break;
            case BATTLEGROUND_DS:
                bg = new BattlegroundDS();
                break;
            case BATTLEGROUND_RV:
                bg = new BattlegroundRV();
                break;
            case BATTLEGROUND_IC:
                bg = new BattlegroundIC();
                break;
            case BATTLEGROUND_SCM:
                bg = new BattlegroundSCM();
                break;
            case BATTLEGROUND_BRT:
                bg = new BattlegroundBRT();
                break;
            case BATTLEGROUND_OBC:
                bg = new BattlegroundOBC();
                break;
            case BATTLEGROUND_TRT:
                bg = new BattlegroundTRT();
                break;
            case BATTLEGROUND_VHR:
                bg = new BattlegroundVHR();
                break;
            case BATTLEGROUND_TOK:
                bg = new BattlegroundTOK();
                break;
            case BATTLEGROUND_SRF:
                bg = new BattlegroundSRF();
                break;
            case BATTLEGROUND_SV:
                bg = new BattlegroundSV();
                break;
            case BATTLEGROUND_TP:
                bg = new BattlegroundTP();
                break;
            case BATTLEGROUND_BFG:
                bg = new BattlegroundBFG();
                break;
            case BATTLEGROUND_TV:
                bg = new BattlegroundTV();
                break;
            case BATTLEGROUND_TTP:
                bg = new BattlegroundTTP();
                break;
            case BATTLEGROUND_AA:
            case BATTLEGROUND_SSH:
                bg = new Battleground();
                break;
            case BATTLEGROUND_RB:
                bg = new Battleground();
                bg->SetRandom(true);
                break;
            default:
                return false;
        }

        bg->SetTypeID(bgTemplate->Id);
        bg->SetInstanceID(0);
        AddBattleground(bg);
    }

    bg->SetMapId(bgTemplate->BattlemasterEntry->MapID[0]);
    bg->SetName(bgTemplate->BattlemasterEntry->Name[sWorld->GetDefaultDbcLocale()]);
    bg->SetArenaorBGType(bgTemplate->IsArena());
    bg->SetMinPlayersPerTeam(bgTemplate->MinPlayersPerTeam);
    bg->SetMaxPlayersPerTeam(bgTemplate->MaxPlayersPerTeam);
    bg->SetMinPlayers(bgTemplate->MinPlayersPerTeam * 2);
    bg->SetMaxPlayers(bgTemplate->MaxPlayersPerTeam * 2);
    bg->SetTeamStartPosition(TEAM_ALLIANCE, bgTemplate->StartLocation[TEAM_ALLIANCE]);
    bg->SetTeamStartPosition(TEAM_HORDE,    bgTemplate->StartLocation[TEAM_HORDE]);
    bg->SetStartMaxDist(bgTemplate->MaxStartDistSq);
    bg->SetBounds(bgTemplate->Bounds);
    bg->SetDropExits(bgTemplate->DropExits);
    bg->SetStartExit(TEAM_ALLIANCE, bgTemplate->StartExits[TEAM_ALLIANCE]);
    bg->SetStartExit(TEAM_HORDE, bgTemplate->StartExits[TEAM_HORDE]);
    bg->SetSquadStarts(bgTemplate->SquadStarts);
    bg->SetLevelRange(bgTemplate->MinLevel, bgTemplate->MaxLevel);
    bg->SetScriptId(bgTemplate->ScriptId);

    return true;
}

void BattlegroundMgr::LoadBattlegroundTemplates()
{
    uint32 oldMSTime = getMSTime();

    // Loaded alongside the templates, and therefore also refreshed by
    // `.reload battleground_template`. Deliberately not given reload commands of
    // their own: that would need new rbac_permissions rows in the auth database,
    // and these three tables are always edited together anyway.
    LoadRandomBattlegroundPools();
    BattlegroundCustomArena::LoadObjects();
    std::unordered_map<uint32, BattlegroundBounds> const bounds = LoadBattlegroundBounds();
    std::unordered_map<uint32, std::vector<BattlegroundDropExit>> const dropExits = LoadBattlegroundDropExits();
    std::unordered_map<uint32, std::array<BattlegroundStartExit, PVP_TEAMS_COUNT>> const startExits = LoadBattlegroundStartExits();
    std::unordered_map<uint32, std::vector<BattlegroundSquadStart>> const squadStarts = LoadBattlegroundSquadStarts();

    _battlegroundMapTemplates.clear();
    _battlegroundTemplates.clear();

    //                                               0   1                  2                  3       4       5                 6               7              8            9             10      11
    QueryResult result = WorldDatabase.Query("SELECT ID, MinPlayersPerTeam, MaxPlayersPerTeam, MinLvl, MaxLvl, AllianceStartLoc, AllianceStartO, HordeStartLoc, HordeStartO, StartMaxDist, Weight, ScriptName FROM battleground_template");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 battlegrounds. DB table `battleground_template` is empty.");
        return;
    }

    uint32 count = 0;

    do
    {
        Field* fields = result->Fetch();

        BattlegroundTypeId bgTypeId = BattlegroundTypeId(fields[0].GetUInt32());

        if (DisableMgr::IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeId, nullptr))
            continue;

        // can be overwrite by values from DB
        BattlemasterListEntry const* bl = sBattlemasterListStore.LookupEntry(bgTypeId);
        if (!bl)
        {
            TC_LOG_ERROR("bg.battleground", "Battleground ID {} could not be found in BattlemasterList.dbc. The battleground was not created.", bgTypeId);
            continue;
        }

        BattlegroundTemplate bgTemplate;
        bgTemplate.Id                = bgTypeId;
        bgTemplate.MinPlayersPerTeam = fields[1].GetUInt16();
        bgTemplate.MaxPlayersPerTeam = fields[2].GetUInt16();
        bgTemplate.MinLevel          = fields[3].GetUInt8();
        bgTemplate.MaxLevel          = fields[4].GetUInt8();
        float dist                   = fields[9].GetFloat();
        bgTemplate.MaxStartDistSq    = dist * dist;
        bgTemplate.Weight            = fields[10].GetUInt8();
        bgTemplate.ScriptId          = sObjectMgr->GetScriptId(fields[11].GetString());
        bgTemplate.BattlemasterEntry = bl;

        if (auto itr = bounds.find(uint32(bgTypeId)); itr != bounds.end())
            bgTemplate.Bounds = itr->second;

        if (auto itr = dropExits.find(uint32(bgTypeId)); itr != dropExits.end())
            bgTemplate.DropExits = itr->second;

        if (auto itr = startExits.find(uint32(bgTypeId)); itr != startExits.end())
            for (uint8 i = 0; i < PVP_TEAMS_COUNT; ++i)
                bgTemplate.StartExits[i] = itr->second[i];

        if (auto itr = squadStarts.find(uint32(bgTypeId)); itr != squadStarts.end())
            bgTemplate.SquadStarts = itr->second;

        if (bgTemplate.MaxPlayersPerTeam == 0 || bgTemplate.MinPlayersPerTeam > bgTemplate.MaxPlayersPerTeam)
        {
            TC_LOG_ERROR("sql.sql", "Table `battleground_template` for id {} contains bad values for MinPlayersPerTeam ({}) and MaxPlayersPerTeam({}).",
                bgTemplate.Id, bgTemplate.MinPlayersPerTeam, bgTemplate.MaxPlayersPerTeam);
            continue;
        }

        if (bgTemplate.MinLevel == 0 || bgTemplate.MaxLevel == 0 || bgTemplate.MinLevel > bgTemplate.MaxLevel)
        {
            TC_LOG_ERROR("sql.sql", "Table `battleground_template` for id {} contains bad values for MinLevel ({}) and MaxLevel ({}).",
                bgTemplate.Id, bgTemplate.MinLevel, bgTemplate.MaxLevel);
            continue;
        }

        if (bgTemplate.Id != BATTLEGROUND_AA && bgTemplate.Id != BATTLEGROUND_RB && bgTemplate.Id != BATTLEGROUND_SSH)
        {
            uint32 startId = fields[5].GetUInt32();
            if (WorldSafeLocsEntry const* start = sWorldSafeLocsStore.LookupEntry(startId))
            {
                bgTemplate.StartLocation[TEAM_ALLIANCE].Relocate(start->Loc.X, start->Loc.Y, start->Loc.Z, fields[6].GetFloat());
            }
            else
            {
                TC_LOG_ERROR("sql.sql", "Table `battleground_template` for id {} contains a non-existing WorldSafeLocs.dbc id {} in field `AllianceStartLoc`. BG not created.", bgTemplate.Id, startId);
                continue;
            }

            startId = fields[7].GetUInt32();
            if (WorldSafeLocsEntry const* start = sWorldSafeLocsStore.LookupEntry(startId))
            {
                bgTemplate.StartLocation[TEAM_HORDE].Relocate(start->Loc.X, start->Loc.Y, start->Loc.Z, fields[8].GetFloat());
            }
            else
            {
                TC_LOG_ERROR("sql.sql", "Table `battleground_template` for id {} contains a non-existing WorldSafeLocs.dbc id {} in field `HordeStartLoc`. BG not created.", bgTemplate.Id, startId);
                continue;
            }
        }

        if (!CreateBattleground(&bgTemplate))
            continue;

        _battlegroundTemplates[bgTypeId] = bgTemplate;

        // in this case we have only one mapId. A squad match is the second way
        // to play a map that already has its own two-team battleground (Four
        // Corners is Baradin Hold's), so the map stays that one's.
        if (bgTemplate.BattlemasterEntry->MapID[1] == -1 && bgTemplate.SquadStarts.empty())
            _battlegroundMapTemplates[bgTemplate.BattlemasterEntry->MapID[0]] = &_battlegroundTemplates[bgTypeId];

        ++count;
    }
    while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} battlegrounds in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void BattlegroundMgr::InitAutomaticArenaPointDistribution()
{
    if (!sWorld->getBoolConfig(CONFIG_ARENA_AUTO_DISTRIBUTE_POINTS) || !sWorld->getBoolConfig(CONFIG_ARENA_SEASON_IN_PROGRESS))
        return;

    time_t wstime = time_t(sWorld->getWorldState(WS_ARENA_DISTRIBUTION_TIME));
    time_t curtime = GameTime::GetGameTime();
    TC_LOG_DEBUG("bg.battleground", "Initializing Automatic Arena Point Distribution");
    if (wstime < curtime)
    {
        m_NextAutoDistributionTime = curtime;           // reset will be called in the next update
        TC_LOG_DEBUG("bg.battleground", "Battleground: Next arena point distribution time in the past, reseting it now.");
    }
    else
        m_NextAutoDistributionTime = wstime;
    TC_LOG_DEBUG("bg.battleground", "Automatic Arena Point Distribution initialized.");
}

void BattlegroundMgr::BuildBattlegroundListPacket(WorldPacket* data, ObjectGuid guid, Player* player, BattlegroundTypeId bgTypeId, uint8 fromWhere)
{
    if (!player)
        return;

    uint32 winner_kills = player->GetRandomWinner() ? sWorld->getIntConfig(CONFIG_BG_REWARD_WINNER_HONOR_LAST) : sWorld->getIntConfig(CONFIG_BG_REWARD_WINNER_HONOR_FIRST);
    uint32 winner_arena = player->GetRandomWinner() ? sWorld->getIntConfig(CONFIG_BG_REWARD_WINNER_ARENA_LAST) : sWorld->getIntConfig(CONFIG_BG_REWARD_WINNER_ARENA_FIRST);
    uint32 loser_kills = player->GetRandomWinner() ? sWorld->getIntConfig(CONFIG_BG_REWARD_LOSER_HONOR_LAST) : sWorld->getIntConfig(CONFIG_BG_REWARD_LOSER_HONOR_FIRST);

    winner_kills = Trinity::Honor::hk_honor_at_level(player->GetLevel(), float(winner_kills));
    winner_kills = 100;
    loser_kills = Trinity::Honor::hk_honor_at_level(player->GetLevel(), float(loser_kills));
    loser_kills = 10;

    data->Initialize(SMSG_BATTLEFIELD_LIST);
    *data << uint64(guid);                                  // battlemaster guid
    *data << uint8(fromWhere);                              // from where you joined
    *data << uint32(bgTypeId);                              // battleground id
    *data << uint8(0);                                      // unk
    *data << uint8(0);                                      // unk

    // Rewards
    *data << uint8(player->GetRandomWinner());              // 3.3.3 hasWin
    *data << uint32(winner_kills);                          // 3.3.3 winHonor
    *data << uint32(winner_arena);                          // 3.3.3 winArena
    *data << uint32(loser_kills);                           // 3.3.3 lossHonor

    uint8 isRandom = bgTypeId == BATTLEGROUND_RB;

    *data << uint8(isRandom);                               // 3.3.3 isRandom
    if (isRandom)
    {
        // Rewards (random)
        *data << uint8(player->GetRandomWinner());          // 3.3.3 hasWin_Random
        *data << uint32(winner_kills);                      // 3.3.3 winHonor_Random
        *data << uint32(winner_arena);                      // 3.3.3 winArena_Random
        *data << uint32(loser_kills);                       // 3.3.3 lossHonor_Random
    }

    if (bgTypeId == BATTLEGROUND_AA)                        // arena
        *data << uint32(0);                                 // unk (count?)
    else                                                    // battleground
    {
        size_t count_pos = data->wpos();
        *data << uint32(0);                                 // number of bg instances

        BattlegroundDataContainer::iterator it = bgDataStore.find(bgTypeId);
        if (it != bgDataStore.end())
        {
            // expected bracket entry
            if (PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(it->second.m_Battlegrounds.begin()->second->GetMapId(), player->GetLevel()))
            {
                uint32 count = 0;
                BattlegroundBracketId bracketId = bracketEntry->GetBracketId();
                BattlegroundClientIdsContainer& clientIds = it->second.m_ClientBattlegroundIds[bracketId];
                for (BattlegroundClientIdsContainer::const_iterator itr = clientIds.begin(); itr != clientIds.end(); ++itr)
                {
                    *data << uint32(*itr);
                    ++count;
                }
                data->put<uint32>(count_pos, count);
            }
        }
    }
}

void BattlegroundMgr::SendToBattleground(Player* player, uint32 instanceId, BattlegroundTypeId bgTypeId)
{
    if (Battleground* bg = GetBattleground(instanceId, bgTypeId))
    {
        uint32 mapid = bg->GetMapId();
        uint32 team = player->GetBGTeam();
        if (team == 0)
            team = player->GetTeam();

        // A squad match: the corner of the squad the queue seated them in.
        Position const* pos = bg->TakeStartPosition(player->GetGUID(), team);
        TC_LOG_DEBUG("bg.battleground", "BattlegroundMgr::SendToBattleground: Sending {} to map {}, {} (bgType {})", player->GetName(), mapid, pos->ToString(), bgTypeId);
        player->TeleportTo(mapid, pos->GetPositionX(), pos->GetPositionY(), pos->GetPositionZ(), pos->GetOrientation());
    }
    else
        TC_LOG_ERROR("bg.battleground", "BattlegroundMgr::SendToBattleground: Instance {} (bgType {}) not found while trying to teleport player {}", instanceId, bgTypeId, player->GetName());
}

void BattlegroundMgr::SendAreaSpiritHealerQueryOpcode(Player* player, Battleground* bg, ObjectGuid guid)
{
    WorldPacket data(SMSG_AREA_SPIRIT_HEALER_TIME, 12);

    data << guid << bg->GetTimeUntilResurrectionFor(player);
    player->SendDirectMessage(&data);
}

bool BattlegroundMgr::IsBotFillBattleground(BattlegroundTypeId bgTypeId) const
{
    if (!_botFillPolicy.enabled || bgTypeId == BATTLEGROUND_TYPE_NONE)
        return false;

    // Violet Hold summons its own enemies wave by wave; padding its teams
    // would break the survival curve, so it stays out even of "everything".
    if (bgTypeId == BATTLEGROUND_VHR || IsArenaType(bgTypeId))
        return false;

    auto templateItr = _battlegroundTemplates.find(bgTypeId);
    if (templateItr == _battlegroundTemplates.end() || templateItr->second.IsArena())
        return false;

    return _botFillPolicy.battlegroundTypes.empty() || _botFillPolicy.battlegroundTypes.count(uint32(bgTypeId)) != 0;
}

bool BattlegroundMgr::IsBotFillSkirmishArena(uint8 arenaType) const
{
    if (!_botFillPolicy.enabled || !_botFillPolicy.skirmishArenasEnabled)
        return false;

    return arenaType == ARENA_TYPE_2v2 || arenaType == ARENA_TYPE_3v3 ||
        arenaType == ARENA_TYPE_4v4 || arenaType == ARENA_TYPE_5v5;
}

bool BattlegroundMgr::IsArenaLadderTeam(uint32 arenaTeamId) const
{
    return arenaTeamId && std::find(_arenaLadderTeams.begin(), _arenaLadderTeams.end(), arenaTeamId) != _arenaLadderTeams.end();
}

bool BattlegroundMgr::IsArenaTeamInMatch(uint32 arenaTeamId) const
{
    if (!arenaTeamId)
        return false;

    if (BattlegroundContainer const* arenas = GetBattlegroundsByType(BATTLEGROUND_AA))
        for (auto const& [instanceId, arena] : *arenas)
            if (instanceId && arena && arena->isRated() && arena->GetStatus() < STATUS_WAIT_LEAVE &&
                (arena->GetArenaTeamIdForTeam(ALLIANCE) == arenaTeamId || arena->GetArenaTeamIdForTeam(HORDE) == arenaTeamId))
                return true;

    return false;
}

bool BattlegroundMgr::IsBotFillRatedArena(uint8 arenaType) const
{
    if (!IsBotFillRatedArenaEnabled())
        return false;

    return arenaType == ARENA_TYPE_2v2 || arenaType == ARENA_TYPE_3v3 ||
        arenaType == ARENA_TYPE_4v4 || arenaType == ARENA_TYPE_5v5;
}

// Honor per hour, the same for every kind of match.
//
// The end-of-match payout (RewardHonorWinner/Loser) used to be one flat figure
// for every match, so a short 2v2 paid exactly what a long Warsong Gulch did,
// and a battleground with rich objective honor paid more per hour than one
// without. Here each kind of match is assumed to last its median length and to
// pay its median in-match honor, and its end-of-match payout is whatever tops
// that up to HonorPerHour for that length. The match's own timer plays no
// part: a short or long game of a kind pays the kind's payout. The medians are
// re-measured with every match played, so the payouts follow the realm.
namespace
{
    // Kept in memory per kind. Window is applied when the medians are taken,
    // so it can be changed by a config reload in either direction.
    constexpr size_t MATCH_DURATION_MEMORY = 1000;
}

BattlegroundMgr::MatchDurationKey BattlegroundMgr::GetMatchDurationKey(Battleground const* bg)
{
    // A battleground by the map actually played (Random Battleground rolls
    // one); an arena by the format queued for, since one All Arenas map plays
    // much like the next and splitting them by map would starve every median.
    uint32 const typeId = bg->isArena() ? uint32(bg->GetTypeID()) : uint32(bg->GetTypeID(true));
    return { typeId, bg->GetArenaType(), uint8(bg->GetMinLevel()) };
}

time_t BattlegroundMgr::GetMatchDurationCutoff()
{
    uint32 const maxAgeDays = sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_MAX_AGE_DAYS);
    if (!maxAgeDays)
        return 0;

    return GameTime::GetGameTime() - time_t(maxAgeDays) * DAY;
}

uint32 BattlegroundMgr::Median(std::vector<uint32>& values)
{
    if (values.empty())
        return 0;

    std::sort(values.begin(), values.end());
    size_t const middle = values.size() / 2;
    if (values.size() % 2)
        return values[middle];

    return (values[middle - 1] + values[middle]) / 2;
}

void BattlegroundMgr::CollectRecentSamples(MatchDurationWindow const& window, time_t cutoff, std::vector<MatchDurationSample>& out)
{
    size_t const limit = std::max<uint32>(1, sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_WINDOW));
    size_t taken = 0;
    for (auto itr = window.Samples.rbegin(); itr != window.Samples.rend() && taken < limit; ++itr)
    {
        if (itr->EndedAt < cutoff)
            break;

        out.push_back(*itr);
        ++taken;
    }
}

BattlegroundMgr::MatchDurationStats BattlegroundMgr::MakeStats(MatchDurationKey const& key, MatchDurationWindow const& window,
    std::vector<MatchDurationSample> const& samples)
{
    std::vector<uint32> seconds, winHonor, lossHonor;
    for (MatchDurationSample const& sample : samples)
    {
        seconds.push_back(sample.Seconds);
        if (sample.WinHonor >= 0)
            winHonor.push_back(uint32(sample.WinHonor));
        if (sample.LossHonor >= 0)
            lossHonor.push_back(uint32(sample.LossHonor));
    }

    uint32 const samplesCount = uint32(samples.size());
    return { key, window.MaxLevel, window.Arena, samplesCount, Median(seconds), Median(winHonor), Median(lossHonor) };
}

void BattlegroundMgr::AddMatchDurationSample(MatchDurationKey const& key, uint8 maxLevel, bool arena, MatchDurationSample sample)
{
    MatchDurationWindow& window = _matchDurations[key];
    window.MaxLevel = maxLevel;
    window.Arena = arena;
    window.Samples.push_back(sample);
    while (window.Samples.size() > MATCH_DURATION_MEMORY)
        window.Samples.pop_front();
}

void BattlegroundMgr::LoadMatchDurations()
{
    uint32 oldMSTime = getMSTime();

    std::lock_guard<std::mutex> lock(_matchDurationLock);
    _matchDurations.clear();

    // A query against a missing table is fatal here (errno 1146 aborts the
    // realm), and custom tables are applied by hand, so ask first. Without it
    // nothing is recorded and every match pays its configured honor.
    _matchDurationTableExists = bool(CharacterDatabase.Query(
        "SELECT 1 FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'centurion_pvp_match_duration' LIMIT 1"));
    if (!_matchDurationTableExists)
    {
        TC_LOG_ERROR("server.loading", ">> Table `centurion_pvp_match_duration` is missing: matches are not recorded and match honor stays flat.");
        return;
    }

    // Only matches a real person played in: those are the ones whose honor
    // anybody is paid, and a bot-only match would say nothing about them.
    QueryResult result = CharacterDatabase.PQuery(
        "SELECT type_id, arena_type, min_level, max_level, arena, duration_sec, ended_at, "
        "CAST(IFNULL(honor_win, -1) AS SIGNED), CAST(IFNULL(honor_loss, -1) AS SIGNED) "
        "FROM centurion_pvp_match_duration WHERE humans > 0 AND ended_at >= {} ORDER BY ended_at, id",
        uint64(GetMatchDurationCutoff()));

    uint32 count = 0;
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            MatchDurationKey const key{ fields[0].GetUInt32(), fields[1].GetUInt8(), fields[2].GetUInt8() };
            AddMatchDurationSample(key, fields[3].GetUInt8(), fields[4].GetBool(),
                { time_t(fields[6].GetUInt32()), fields[5].GetUInt32(), int32(fields[7].GetInt64()), int32(fields[8].GetInt64()) });
            ++count;
        } while (result->NextRow());
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} recorded matches for {} kinds of battleground and arena match in {} ms",
        count, _matchDurations.size(), GetMSTimeDiffToNow(oldMSTime));
}

void BattlegroundMgr::RecordMatchDuration(Battleground const* bg, uint32 seconds, uint32 humans, uint32 players,
    int32 winHonor, int32 lossHonor, uint32 paidWinner, uint32 paidLoser)
{
    std::lock_guard<std::mutex> lock(_matchDurationLock);
    if (!_matchDurationTableExists)
        return;

    MatchDurationKey const key = GetMatchDurationKey(bg);
    time_t const now = GameTime::GetGameTime();

    std::string const win = winHonor >= 0 ? std::to_string(winHonor) : "NULL";
    std::string const loss = lossHonor >= 0 ? std::to_string(lossHonor) : "NULL";
    CharacterDatabase.PExecute(
        "INSERT INTO centurion_pvp_match_duration (ended_at, type_id, arena_type, min_level, max_level, arena, "
        "duration_sec, winner, humans, players, honor_win, honor_loss, paid_win, paid_loss) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        uint64(now), key.TypeId, uint32(key.ArenaType), uint32(key.MinLevel), bg->GetMaxLevel(), bg->isArena() ? 1 : 0,
        seconds, uint32(bg->GetWinner()), humans, players, win, loss, paidWinner, paidLoser);

    if (humans)
        AddMatchDurationSample(key, uint8(bg->GetMaxLevel()), bg->isArena(), { now, seconds, winHonor, lossHonor });
}

BattlegroundMgr::HourlyHonorPayout BattlegroundMgr::GetFlatPayout(bool arena)
{
    uint32 const winner = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_WINNER);
    uint32 const loser = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_LOSER);
    if (!arena)
        return { winner, loser };

    float const arenaMultiplier = sWorld->getFloatConfig(CONFIG_CENTURION_BG_ARENA_REWARD_MULTIPLIER);
    uint32 const flagHonor = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_FLAG_CAP) * 3;
    return { uint32((winner + flagHonor) * arenaMultiplier), uint32(loser * arenaMultiplier) };
}

bool BattlegroundMgr::IsHourlyHonorAnchorArena()
{
    uint32 const anchorType = sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_ANCHOR_TYPE);
    return anchorType ? sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_ANCHOR_ARENA_TYPE) != 0 : false;
}

uint32 BattlegroundMgr::GetHonorPerHourLocked() const
{
    if (uint32 const configured = sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_RATE))
        return configured;

    // What the anchor pays per hour today: its flat payout (averaged over
    // winning and losing) plus the median honor earned in play, over its
    // median length, every level bracket pooled. Only the anchor has to be
    // played enough for the rate to exist; every other kind is then measured
    // against it as its own matches come in. The rate never feeds back into
    // itself: it is built from the FLAT payout, and in-match honor excludes
    // the end-of-match payout.
    //
    // AnchorTypeId 0: every battleground (not arena) match pooled instead.
    uint32 const anchorType = sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_ANCHOR_TYPE);
    uint32 const anchorArenaType = sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_ANCHOR_ARENA_TYPE);
    bool const anchorArena = IsHourlyHonorAnchorArena();

    time_t const cutoff = GetMatchDurationCutoff();
    std::vector<MatchDurationSample> samples;
    for (auto const& [key, window] : _matchDurations)
    {
        bool const isAnchor = anchorType ?
            key.TypeId == anchorType && (!anchorArenaType || key.ArenaType == anchorArenaType) :
            !window.Arena;
        if (isAnchor)
            CollectRecentSamples(window, cutoff, samples);
    }

    if (samples.empty() || samples.size() < sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_MIN_SAMPLES))
        return 0;

    std::vector<uint32> seconds, earned;
    for (MatchDurationSample const& sample : samples)
    {
        seconds.push_back(sample.Seconds);
        if (sample.WinHonor >= 0 && sample.LossHonor >= 0)
            earned.push_back(uint32((sample.WinHonor + sample.LossHonor) / 2));
        else if (sample.WinHonor >= 0 || sample.LossHonor >= 0)
            earned.push_back(uint32(std::max(sample.WinHonor, sample.LossHonor)));
    }

    uint32 const medianSeconds = Median(seconds);
    if (!medianSeconds)
        return 0;

    HourlyHonorPayout const flat = GetFlatPayout(anchorArena);
    double const payout = (double(flat.Winner) + double(flat.Loser)) / 2.0;
    return uint32(std::lround((payout + double(Median(earned))) * double(HOUR) / double(medianSeconds)));
}

uint32 BattlegroundMgr::GetHonorPerHour() const
{
    std::lock_guard<std::mutex> lock(_matchDurationLock);
    return GetHonorPerHourLocked();
}

BattlegroundMgr::HourlyHonorPayout BattlegroundMgr::HourlyHonorPayoutFor(MatchDurationStats const& stats, uint32 honorPerHour) const
{
    // A match of the kind's median length earns honorPerHour pro rata, on
    // average over its two sides. ArenaEffort weighs an arena hour against a
    // battleground hour, relative to whichever the anchor is, so the anchor
    // itself always earns exactly the rate.
    double target = double(honorPerHour) * double(stats.MedianSeconds) / double(HOUR);
    double const effort = sWorld->getFloatConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_ARENA_EFFORT);
    bool const anchorArena = IsHourlyHonorAnchorArena();
    if (stats.Arena && !anchorArena)
        target *= effort;
    else if (!stats.Arena && anchorArena && effort > 0.0)
        target /= effort;

    // What the kind already pays in play counts towards it; the payout is the
    // rest. One top-up for the kind, split in the proportion its flat payout
    // already splits winning and losing, so a side that earns more in play -
    // winners capturing objectives - keeps that lead instead of both sides
    // being levelled to the same total.
    double const earned = (double(stats.MedianWinHonor) + double(stats.MedianLossHonor)) / 2.0;
    double const topUp = target - earned;

    HourlyHonorPayout const flat = GetFlatPayout(stats.Arena);
    double const flatSum = double(flat.Winner) + double(flat.Loser);
    double const winnerShare = flatSum > 0.0 ? 2.0 * flat.Winner / flatSum : 1.0;
    double const loserShare = flatSum > 0.0 ? 2.0 * flat.Loser / flatSum : 1.0;

    // Kept inside MinFactor..MaxFactor of the flat payout. A kind whose
    // in-play honor alone beats the rate is paid the floor: honor already
    // earned is not taken back.
    double const low = sWorld->getFloatConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_MIN_FACTOR);
    double const high = std::max(low, double(sWorld->getFloatConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_MAX_FACTOR)));
    auto bounded = [low, high](double wanted, double flatAmount)
    {
        return uint32(std::lround(std::clamp(wanted, flatAmount * low, flatAmount * high)));
    };

    return { bounded(topUp * winnerShare, flat.Winner), bounded(topUp * loserShare, flat.Loser) };
}

bool BattlegroundMgr::GetHourlyHonorPayout(Battleground const* bg, HourlyHonorPayout& payout) const
{
    if (!sWorld->getBoolConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_ENABLE))
        return false;

    std::lock_guard<std::mutex> lock(_matchDurationLock);
    auto const itr = _matchDurations.find(GetMatchDurationKey(bg));
    if (itr == _matchDurations.end())
        return false;

    std::vector<MatchDurationSample> samples;
    CollectRecentSamples(itr->second, GetMatchDurationCutoff(), samples);
    if (samples.empty() || samples.size() < sWorld->getIntConfig(CONFIG_CENTURION_BG_HOURLY_HONOR_MIN_SAMPLES))
        return false;

    uint32 const honorPerHour = GetHonorPerHourLocked();
    if (!honorPerHour)
        return false;

    payout = HourlyHonorPayoutFor(MakeStats(itr->first, itr->second, samples), honorPerHour);
    return true;
}

std::vector<BattlegroundMgr::MatchDurationStats> BattlegroundMgr::GetMatchDurationStats() const
{
    std::lock_guard<std::mutex> lock(_matchDurationLock);

    time_t const cutoff = GetMatchDurationCutoff();
    std::vector<MatchDurationStats> stats;
    for (auto const& [key, window] : _matchDurations)
    {
        std::vector<MatchDurationSample> samples;
        CollectRecentSamples(window, cutoff, samples);
        if (!samples.empty())
            stats.push_back(MakeStats(key, window, samples));
    }

    return stats;
}

bool BattlegroundMgr::IsArenaType(BattlegroundTypeId bgTypeId)
{
    return bgTypeId == BATTLEGROUND_AA
        || bgTypeId == BATTLEGROUND_SSH
        || bgTypeId == BATTLEGROUND_BE
        || bgTypeId == BATTLEGROUND_NA
        || bgTypeId == BATTLEGROUND_DS
        || bgTypeId == BATTLEGROUND_RV
        || bgTypeId == BATTLEGROUND_RL
        || bgTypeId == BATTLEGROUND_NL
        || IsCustomArena(bgTypeId);
}

BattlegroundQueueTypeId BattlegroundMgr::BGQueueTypeId(BattlegroundTypeId bgTypeId, uint8 arenaType)
{
    switch (bgTypeId)
    {
        case BATTLEGROUND_AB:
            return BATTLEGROUND_QUEUE_AB;
        case BATTLEGROUND_AV:
            return BATTLEGROUND_QUEUE_AV;
        case BATTLEGROUND_EY:
            return BATTLEGROUND_QUEUE_EY;
        case BATTLEGROUND_IC:
            return BATTLEGROUND_QUEUE_IC;
        case BATTLEGROUND_RB:
            return BATTLEGROUND_QUEUE_RB;
        case BATTLEGROUND_SA:
            return BATTLEGROUND_QUEUE_SA;
        case BATTLEGROUND_WS:
            return BATTLEGROUND_QUEUE_WS;
        case BATTLEGROUND_SCM:
            return BATTLEGROUND_QUEUE_SCM;
        case BATTLEGROUND_BRT:
            return BATTLEGROUND_QUEUE_BRT;
        case BATTLEGROUND_OBC:
            return BATTLEGROUND_QUEUE_OBC;
        case BATTLEGROUND_TRT:
            return BATTLEGROUND_QUEUE_TRT;
        case BATTLEGROUND_VHR:
            return BATTLEGROUND_QUEUE_VHR;
        case BATTLEGROUND_TOK:
            return BATTLEGROUND_QUEUE_TOK;
        case BATTLEGROUND_SRF:
            return BATTLEGROUND_QUEUE_SRF;
        case BATTLEGROUND_TP:
            return BATTLEGROUND_QUEUE_TP;
        case BATTLEGROUND_BFG:
            return BATTLEGROUND_QUEUE_BFG;
        case BATTLEGROUND_SV:
            return BATTLEGROUND_QUEUE_SV;
        case BATTLEGROUND_BHF:
            // An arena, but one played by four squads: its own queue per squad
            // size, never the shared skirmish queues below.
            switch (arenaType)
            {
                case ARENA_TYPE_2v2:
                    return BATTLEGROUND_QUEUE_BHF_2;
                case ARENA_TYPE_3v3:
                    return BATTLEGROUND_QUEUE_BHF_3;
                case ARENA_TYPE_4v4:
                    return BATTLEGROUND_QUEUE_BHF_4;
                case ARENA_TYPE_5v5:
                    return BATTLEGROUND_QUEUE_BHF_5;
                default:
                    return BATTLEGROUND_QUEUE_NONE;
            }
        case BATTLEGROUND_SSH:
            // 2v2 or 3v3: four or six people, two teams each round.
            switch (arenaType)
            {
                case ARENA_TYPE_2v2:
                    return BATTLEGROUND_QUEUE_SSH_2;
                case ARENA_TYPE_3v3:
                    return BATTLEGROUND_QUEUE_SSH_3;
                default:
                    return BATTLEGROUND_QUEUE_NONE;
            }
        default:
            // Arenas do not get a queue per arena -- every arena of a given team
            // size shares one queue, which is what lets All Arenas mix them. The
            // custom arenas are matched by range here rather than by listing
            // each id, so a new one needs no edit.
            if (!IsArenaType(bgTypeId))
                return BATTLEGROUND_QUEUE_NONE;

            switch (arenaType)
            {
                case ARENA_TYPE_2v2:
                    return BATTLEGROUND_QUEUE_2v2;
                case ARENA_TYPE_3v3:
                    return BATTLEGROUND_QUEUE_3v3;
                case ARENA_TYPE_4v4:
                    return BATTLEGROUND_QUEUE_4v4;
                case ARENA_TYPE_5v5:
                    return BATTLEGROUND_QUEUE_5v5;
                default:
                    return BATTLEGROUND_QUEUE_NONE;
            }
    }
}

BattlegroundTypeId BattlegroundMgr::BGTemplateId(BattlegroundQueueTypeId bgQueueTypeId)
{
    switch (bgQueueTypeId)
    {
        case BATTLEGROUND_QUEUE_WS:
            return BATTLEGROUND_WS;
        case BATTLEGROUND_QUEUE_AB:
            return BATTLEGROUND_AB;
        case BATTLEGROUND_QUEUE_AV:
            return BATTLEGROUND_AV;
        case BATTLEGROUND_QUEUE_EY:
            return BATTLEGROUND_EY;
        case BATTLEGROUND_QUEUE_SA:
            return BATTLEGROUND_SA;
        case BATTLEGROUND_QUEUE_IC:
            return BATTLEGROUND_IC;
        case BATTLEGROUND_QUEUE_RB:
            return BATTLEGROUND_RB;
        case BATTLEGROUND_QUEUE_SCM:
            return BATTLEGROUND_SCM;
        case BATTLEGROUND_QUEUE_BRT:
            return BATTLEGROUND_BRT;
        case BATTLEGROUND_QUEUE_OBC:
            return BATTLEGROUND_OBC;
        case BATTLEGROUND_QUEUE_TRT:
            return BATTLEGROUND_TRT;
        case BATTLEGROUND_QUEUE_VHR:
            return BATTLEGROUND_VHR;
        case BATTLEGROUND_QUEUE_TOK:
            return BATTLEGROUND_TOK;
        case BATTLEGROUND_QUEUE_SRF:
            return BATTLEGROUND_SRF;
        case BATTLEGROUND_QUEUE_TP:
            return BATTLEGROUND_TP;
        case BATTLEGROUND_QUEUE_BFG:
            return BATTLEGROUND_BFG;
        case BATTLEGROUND_QUEUE_SV:
            return BATTLEGROUND_SV;
        case BATTLEGROUND_QUEUE_2v2:
        case BATTLEGROUND_QUEUE_3v3:
        case BATTLEGROUND_QUEUE_4v4:
        case BATTLEGROUND_QUEUE_5v5:
            return BATTLEGROUND_AA;
        case BATTLEGROUND_QUEUE_BHF_2:
        case BATTLEGROUND_QUEUE_BHF_3:
        case BATTLEGROUND_QUEUE_BHF_4:
        case BATTLEGROUND_QUEUE_BHF_5:
            return BATTLEGROUND_BHF;
        case BATTLEGROUND_QUEUE_SSH_2:
        case BATTLEGROUND_QUEUE_SSH_3:
            return BATTLEGROUND_SSH;
        default:
            return BattlegroundTypeId(0);                   // used for unknown template (it exists and does nothing)
    }
}

uint8 BattlegroundMgr::BGArenaType(BattlegroundQueueTypeId bgQueueTypeId)
{
    switch (bgQueueTypeId)
    {
        case BATTLEGROUND_QUEUE_2v2:
        case BATTLEGROUND_QUEUE_BHF_2:
        case BATTLEGROUND_QUEUE_SSH_2:
            return ARENA_TYPE_2v2;
        case BATTLEGROUND_QUEUE_3v3:
        case BATTLEGROUND_QUEUE_BHF_3:
        case BATTLEGROUND_QUEUE_SSH_3:
            return ARENA_TYPE_3v3;
        case BATTLEGROUND_QUEUE_4v4:
        case BATTLEGROUND_QUEUE_BHF_4:
            return ARENA_TYPE_4v4;
        case BATTLEGROUND_QUEUE_5v5:
        case BATTLEGROUND_QUEUE_BHF_5:
            return ARENA_TYPE_5v5;
        default:
            return 0;
    }
}

bool BattlegroundMgr::IsSquadBattleground(BattlegroundTypeId bgTypeId) const
{
    auto const itr = _battlegroundTemplates.find(bgTypeId);
    return itr != _battlegroundTemplates.end() && !itr->second.SquadStarts.empty();
}

void BattlegroundMgr::ToggleTesting()
{
    m_Testing = !m_Testing;
    sWorld->SendWorldText(m_Testing ? LANG_DEBUG_BG_ON : LANG_DEBUG_BG_OFF);
}

void BattlegroundMgr::ToggleArenaTesting()
{
    m_ArenaTesting = !m_ArenaTesting;
    sWorld->SendWorldText(m_ArenaTesting ? LANG_DEBUG_ARENA_ON : LANG_DEBUG_ARENA_OFF);
}

void BattlegroundMgr::ScheduleQueueUpdate(uint32 arenaMatchmakerRating, uint8 arenaType, BattlegroundQueueTypeId bgQueueTypeId, BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id)
{
    //This method must be atomic, @todo add mutex
    //we will use only 1 number created of bgTypeId and bracket_id
    // The type id gets eight bits: the update loop puts one past 255 back
    // together from its queue. Unmasked, Four Corners' 887 (0x377) ran into
    // the queue byte, so a join updated the wrong queue (2v2v2v2 the
    // 4v4v4v4 one, 5v5v5v5 one past the end of m_BattlegroundQueues) and
    // only the periodic sweep ever popped it.
    uint64 const scheduleId = ((uint64)arenaMatchmakerRating << 32) | ((uint64)arenaType << 24) | ((uint64)bgQueueTypeId << 16) | ((uint64)(uint32(bgTypeId) & 0xFF) << 8) | (uint64)bracket_id;
    if (std::find(m_QueueUpdateScheduler.begin(), m_QueueUpdateScheduler.end(), scheduleId) == m_QueueUpdateScheduler.end())
        m_QueueUpdateScheduler.push_back(scheduleId);
}

uint32 BattlegroundMgr::GetMaxRatingDifference() const
{
    // this is for stupid people who can't use brain and set max rating difference to 0
    uint32 diff = sWorld->getIntConfig(CONFIG_ARENA_MAX_RATING_DIFFERENCE);
    if (diff == 0)
        diff = 5000;
    return diff;
}

uint32 BattlegroundMgr::GetRatingDiscardTimer() const
{
    return sWorld->getIntConfig(CONFIG_ARENA_RATING_DISCARD_TIMER);
}

uint32 BattlegroundMgr::GetPrematureFinishTime() const
{
    return sWorld->getIntConfig(CONFIG_BATTLEGROUND_PREMATURE_FINISH_TIMER);
}

void BattlegroundMgr::LoadBattleMastersEntry()
{
    uint32 oldMSTime = getMSTime();

    mBattleMastersMap.clear();                                  // need for reload case

    QueryResult result = WorldDatabase.Query("SELECT entry, bg_template FROM battlemaster_entry");

    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 battlemaster entries. DB table `battlemaster_entry` is empty!");
        return;
    }

    uint32 count = 0;

    do
    {
        ++count;

        Field* fields = result->Fetch();

        uint32 entry = fields[0].GetUInt32();
        if (CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(entry))
        {
            if ((cInfo->npcflag & UNIT_NPC_FLAG_BATTLEMASTER) == 0)
                TC_LOG_ERROR("sql.sql", "Creature (Entry: {}) listed in `battlemaster_entry` is not a battlemaster.", entry);
        }
        else
        {
            TC_LOG_ERROR("sql.sql", "Creature (Entry: {}) listed in `battlemaster_entry` does not exist.", entry);
            continue;
        }

        uint32 bgTypeId  = fields[1].GetUInt32();
        if (!sBattlemasterListStore.LookupEntry(bgTypeId))
        {
            TC_LOG_ERROR("sql.sql", "Table `battlemaster_entry` contains entry {} for a non-existing battleground type {}, ignored.", entry, bgTypeId);
            continue;
        }

        mBattleMastersMap[entry] = BattlegroundTypeId(bgTypeId);
    }
    while (result->NextRow());

    CheckBattleMasters();

    TC_LOG_INFO("server.loading", ">> Loaded {} battlemaster entries in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void BattlegroundMgr::CheckBattleMasters()
{
    CreatureTemplateContainer const& ctc = sObjectMgr->GetCreatureTemplates();
    for (auto const& creatureTemplatePair : ctc)
    {
        if ((creatureTemplatePair.second.npcflag & UNIT_NPC_FLAG_BATTLEMASTER) && !mBattleMastersMap.count(creatureTemplatePair.first))
        {
            TC_LOG_ERROR("sql.sql", "Creature_Template Entry: {} has UNIT_NPC_FLAG_BATTLEMASTER, but no data in the `battlemaster_entry` table. Removing flag.", creatureTemplatePair.first);
            const_cast<CreatureTemplate&>(creatureTemplatePair.second).npcflag &= ~UNIT_NPC_FLAG_BATTLEMASTER;
        }
    }
}

void BattlegroundMgr::LoadRandomBattlegroundPools()
{
    uint32 oldMSTime = getMSTime();

    _randomPools.clear();

    QueryResult result = WorldDatabase.Query("SELECT PoolBgTypeId, MemberBgTypeId, Weight FROM battleground_random_pool WHERE Enabled <> 0 ORDER BY PoolBgTypeId, MemberBgTypeId");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 random battleground pool entries. Falling back to BattlemasterList.dbc.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        BattlegroundTypeId poolId   = BattlegroundTypeId(fields[0].GetUInt32());
        BattlegroundTypeId memberId = BattlegroundTypeId(fields[1].GetUInt32());
        double weight               = fields[2].GetDouble();

        if (weight <= 0.0)
        {
            TC_LOG_ERROR("sql.sql", "Table `battleground_random_pool` entry (pool {}, member {}) has Weight {} which is not positive. Skipped.", uint32(poolId), uint32(memberId), weight);
            continue;
        }

        // Deliberately not validated against _battlegroundTemplates here: this
        // runs before templates are loaded on a cold start. Members that have no
        // template are dropped at selection time instead, so a pool row for an
        // arena whose map is not shipped yet is harmless rather than fatal.
        _randomPools[poolId].emplace_back(memberId, weight);
        ++count;
    }
    while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} random battleground pool entries for {} pools in {} ms",
        count, uint32(_randomPools.size()), GetMSTimeDiffToNow(oldMSTime));
}

bool BattlegroundMgr::IsPoolMemberSelectable(BattlegroundTypeId bgTypeId)
{
    // A row in `battleground_random_pool` says an administrator *wants* this
    // battleground in the rotation. Whether it can actually be run is a separate
    // question, and both answers have to be yes.
    //
    // Dalaran Sewers and the Ring of Valor are the live example: both are listed
    // as arenas, both are disabled in `disables`, and neither therefore has a
    // template. Rolling one would hand back a type id CreateNewBattleground
    // cannot instantiate, and the queue pop would break instead of the arena
    // simply not being offered.
    if (DisableMgr::IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeId, nullptr))
        return false;

    return GetBattlegroundTemplateByTypeId(bgTypeId) != nullptr;
}

std::vector<BattlegroundTypeId> BattlegroundMgr::GetRandomPoolMembers(BattlegroundTypeId poolBgTypeId)
{
    std::vector<BattlegroundTypeId> members;

    auto itr = _randomPools.find(poolBgTypeId);
    if (itr == _randomPools.end())
        return members;

    members.reserve(itr->second.size());
    for (auto const& [memberId, weight] : itr->second)
    {
        (void)weight;
        if (IsPoolMemberSelectable(memberId))
            members.push_back(memberId);
    }

    return members;
}

BattlegroundTypeId BattlegroundMgr::GetRandomBG(BattlegroundTypeId bgTypeId, bool hasBotParticipants)
{
    std::vector<BattlegroundTypeId> ids;
    std::vector<double> weights;
    ids.reserve(32);
    weights.reserve(32);

    // A squad match is never a roll: its one map belongs to another type
    // (Four Corners plays Baradin Hold's), and the BattlemasterList route below
    // finds the template by map, which would hand back that one instead.
    if (IsSquadBattleground(bgTypeId))
        return bgTypeId;

    // Prefer the world-DB pool over BattlemasterList.dbc.
    //
    // The DBC route cannot express more than eight, because BattlemasterListEntry
    // holds a fixed `int32 MapID[8]`. That is a hard cap in the client's own file
    // format, not a limit of this code, so "All Arenas" could never roll a ninth
    // arena however many were installed. The pool table has no such ceiling and
    // is editable live: change a row and `.reload battleground_template` picks it
    // up without a restart or a DBC rebuild.
    //
    // Buffle Shuffle rolls from rows of its own (pool 889): its later rounds shut
    // the gates again, which not every arena can do. With none it rolls All
    // Arenas'.
    bool const arenaRoll = bgTypeId == BATTLEGROUND_AA || bgTypeId == BATTLEGROUND_SSH;
    auto poolItr = _randomPools.find(bgTypeId);
    if (poolItr == _randomPools.end() && bgTypeId == BATTLEGROUND_SSH)
        poolItr = _randomPools.find(BATTLEGROUND_AA);
    if (poolItr != _randomPools.end())
    {
        for (auto const& [memberId, weight] : poolItr->second)
        {
            // Managed bots can leave the playable bounds on these maps.
            // Exclude them only from an All Arenas roll for a public match that
            // contains (or will be filled by) bots. Human-only rolls and manual
            // or private Custom Games selections retain the full arena list.
            if (hasBotParticipants && arenaRoll && IsArenaUnfitForBots(memberId))
                continue;

            // Disabled, or no template: configured in the pool but not runnable.
            // Rolling it would hand back a type id CreateNewBattleground cannot
            // instantiate.
            if (!IsPoolMemberSelectable(memberId))
            {
                TC_LOG_DEBUG("bg.battleground", "GetRandomBG: pool {} lists {}, which is disabled or has no battleground template; skipping it.", uint32(bgTypeId), uint32(memberId));
                continue;
            }

            ids.push_back(memberId);
            weights.push_back(weight);
        }
    }

    if (ids.empty())
    {
        BattlegroundTemplate const* bgTemplate = GetBattlegroundTemplateByTypeId(bgTypeId);
        if (!bgTemplate)
            return BATTLEGROUND_TYPE_NONE;

        for (int32 mapId : bgTemplate->BattlemasterEntry->MapID)
        {
            if (mapId == -1)
                break;

            if (BattlegroundTemplate const* bg = GetBattlegroundTemplateByMapId(mapId))
            {
                if (hasBotParticipants && arenaRoll && IsArenaUnfitForBots(bg->Id))
                    continue;

                ids.push_back(bg->Id);
                weights.push_back(bg->Weight);
            }
        }
    }

    // Both routes can come up empty -- an unconfigured pool and a DBC row whose
    // maps have no templates. SelectRandomWeightedContainerElement dereferences
    // its result, so returning the id unchanged is the only safe answer: the
    // caller then looks up the template for the type actually queued for, which
    // is the pre-existing behaviour for every non-random battleground.
    if (ids.empty())
        return bgTypeId;

    return *Trinity::Containers::SelectRandomWeightedContainerElement(ids, weights);
}

BGFreeSlotQueueContainer& BattlegroundMgr::GetBGFreeSlotQueueStore(BattlegroundTypeId bgTypeId)
{
    return bgDataStore[bgTypeId].BGFreeSlotQueue;
}

void BattlegroundMgr::AddToBGFreeSlotQueue(BattlegroundTypeId bgTypeId, Battleground* bg)
{
    bgDataStore[bgTypeId].BGFreeSlotQueue.push_front(bg);
}

void BattlegroundMgr::RemoveFromBGFreeSlotQueue(BattlegroundTypeId bgTypeId, uint32 instanceId)
{
    BGFreeSlotQueueContainer& queues = bgDataStore[bgTypeId].BGFreeSlotQueue;
    for (BGFreeSlotQueueContainer::iterator itr = queues.begin(); itr != queues.end(); ++itr)
        if ((*itr)->GetInstanceID() == instanceId)
        {
            queues.erase(itr);
            return;
        }
}

BattlegroundContainer const* BattlegroundMgr::GetBattlegroundsByType(BattlegroundTypeId bgTypeId) const
{
    BattlegroundDataContainer::const_iterator itr = bgDataStore.find(bgTypeId);
    if (itr == bgDataStore.end())
        return nullptr;

    return &itr->second.m_Battlegrounds;
}

void BattlegroundMgr::AddBattleground(Battleground* bg)
{
    if (bg)
        bgDataStore[bg->GetTypeID()].m_Battlegrounds[bg->GetInstanceID()] = bg;
}

void BattlegroundMgr::RemoveBattleground(BattlegroundTypeId bgTypeId, uint32 instanceId)
{
    bgDataStore[bgTypeId].m_Battlegrounds.erase(instanceId);
}
