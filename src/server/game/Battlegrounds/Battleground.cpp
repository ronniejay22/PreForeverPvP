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

#include "Battleground.h"
#include "Arena.h"
#include "ArenaScore.h"
#include "BattlegroundMgr.h"
#include "BattlegroundScore.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatTextBuilder.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "CreatureTextMgr.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "GameTime.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "GroupMgr.h"
#include "MiscPackets.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ReputationMgr.h"
#include "Miscellaneous/CooldownStash.h"
#include "Miscellaneous/DepletedMarks.h"
#include "Miscellaneous/Surnames.h"
#include "SpellAuras.h"
#include "TemporarySummon.h"
#include "Transport.h"
#include "Util.h"
#include "Weather.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"
#include "WorldSession.h"
#include "Item.h"
#include "Mail.h"
#include "Map.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <limits>
#include <vector>
#include "Miscellaneous/TournamentMode.h"
#include "RiftProgression.h"

namespace
{
constexpr uint32 NO_NON_VIRTUAL_HUMAN_END_DELAY_MS = 15000;

bool HasAnyNonVirtualHumanParticipant(Battleground const* battleground)
{
    if (!battleground)
        return false;

    for (auto const& [participantGuid, participantData] : battleground->GetPlayers())
    {
        (void)participantData;
        Player const* participant = ObjectAccessor::FindConnectedPlayer(participantGuid);
        if (!participant)
            continue;

        WorldSession const* session = participant->GetSession();
        if (session && !battleground->IsBotParticipantSession(session))
            return true;
    }

    // A real custom-game spectator is still actively occupying and observing
    // the match. Do not auto-end a bot-populated battleground merely because
    // the only connected human is stored outside m_Players in m_Spectators.
    if (battleground->IsCustomGame())
        for (Player const* spectator : battleground->GetSpectators())
        {
            if (!spectator || !spectator->IsInWorld())
                continue;

            WorldSession const* session = spectator->GetSession();
            if (session && !session->IsVirtualSession())
                return true;
        }

    return false;
}

ObjectGuid GetFirstNonVirtualMemberGuid(Group const* group)
{
    if (!group)
        return ObjectGuid::Empty;

    for (GroupReference const* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player const* member = itr->GetSource();
        if (!member)
            continue;

        WorldSession const* session = member->GetSession();
        if (session && !session->IsVirtualSession())
            return member->GetGUID();
    }

    return ObjectGuid::Empty;
}
}

void BattlegroundScore::AppendToPacket(WorldPacket& data)
{
    data << uint64(PlayerGuid);

    data << uint32(KillingBlows);
    data << uint32(HonorableKills);
    data << uint32(Deaths);
    data << uint32(BonusHonor);
    data << uint32(DamageDone);
    data << uint32(HealingDone);

    BuildObjectivesBlock(data);
}

template<class Do>
void Battleground::BroadcastWorker(Do& _do)
{
    for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
        if (Player* player = _GetPlayer(itr, "BroadcastWorker"))
            _do(player);

    // Custom-game spectators are not members of m_Players, but they still
    // need localized battleground announcements such as start countdowns
    // and objective pickup/drop/capture messages.
    if (m_IsCustomGame)
        for (Player* spectator : m_Spectators)
            if (spectator && spectator->IsInWorld() && spectator->GetBattlegroundId() == GetInstanceID())
                _do(spectator);
}

Battleground::Battleground()
{
    m_TypeID            = BATTLEGROUND_TYPE_NONE;
    m_RandomTypeID      = BATTLEGROUND_TYPE_NONE;
    m_InstanceID        = 0;
    m_Status            = STATUS_NONE;
    m_ClientInstanceID  = 0;
    m_EndTime           = 0;
    m_LastResurrectTime = 0;
    m_BracketId         = BG_BRACKET_ID_FIRST;
    m_InvitedAlliance   = 0;
    m_InvitedHorde      = 0;
    m_ArenaType         = 0;
    m_IsArena           = false;
    _winnerTeamId       = PVP_TEAM_NEUTRAL;
    m_StartTime         = 0;
    m_ResetStatTimer    = 0;
    m_ValidStartPositionTimer = 0;
    m_Events            = 0;
    m_StartingAreaPrepared = false;
    m_StartDelayTime    = 0;
    m_IsRated           = false;
    m_BuffChange        = false;
    m_HasEverHadNonVirtualHumanParticipant = false;
    m_NoNonVirtualHumanElapsed = 0;
    m_IsRandom          = false;
    m_IsReplay          = false;
    m_ReplayId          = 0;
    m_LevelMin          = 0;
    m_LevelMax          = 0;
    m_InBGFreeSlotQueue = false;
    m_SetDeleteThis     = false;
    m_IsCustomGame      = false;
    m_CustomGameBotOnlyPreparation = false;
    m_CustomGamePendingCloneCount = 0;
    m_CustomRules       = BattlegroundCustomRules();
    m_IsBotFillMatch    = false;
    m_IsBotFillRefused  = false;
    m_IsTournamentPool  = false;

    m_MaxPlayersPerTeam = 0;
    m_MaxPlayers        = 0;
    m_MinPlayersPerTeam = 0;
    m_MinPlayers        = 0;

    m_MapId             = 0;
    m_Map               = nullptr;
    m_StartMaxDist      = 0.0f;
    m_BoundsCheckTimer  = 0;
    m_BoundsElapsed     = 0;
    m_StartExitDone[TEAM_ALLIANCE] = false;
    m_StartExitDone[TEAM_HORDE]    = false;
    m_StartExitElapsed  = 0;
    ScriptId            = 0;

    m_ArenaTeamIds[TEAM_ALLIANCE]   = 0;
    m_ArenaTeamIds[TEAM_HORDE]      = 0;

    m_ArenaTeamMMR[TEAM_ALLIANCE]   = 0;
    m_ArenaTeamMMR[TEAM_HORDE]      = 0;

    for (uint8 slot = 0; slot < MAX_BATTLEGROUND_SQUADS; ++slot)
    {
        m_BgRaids[slot] = nullptr;
        m_SquadArenaTeamIds[slot] = 0;
        m_SquadArenaMMR[slot] = 0;
        m_SquadScoreboardRival[slot] = BATTLEGROUND_NO_SQUAD;
    }
    m_WinnerSquad = BATTLEGROUND_NO_SQUAD;

    m_PlayersCount[TEAM_ALLIANCE]    = 0;
    m_PlayersCount[TEAM_HORDE]       = 0;

    m_TeamScores[TEAM_ALLIANCE]      = 0;
    m_TeamScores[TEAM_HORDE]         = 0;

    m_PrematureCountDown = false;
    m_PrematureCountDownTimer = 0;

    StartDelayTimes[BG_STARTING_EVENT_FIRST]  = BG_START_DELAY_1M;
    StartDelayTimes[BG_STARTING_EVENT_SECOND] = BG_START_DELAY_30S;
    StartDelayTimes[BG_STARTING_EVENT_THIRD]  = BG_START_DELAY_15S;
    StartDelayTimes[BG_STARTING_EVENT_FOURTH] = BG_START_DELAY_NONE;

    StartMessageIds[BG_STARTING_EVENT_FIRST]  = BG_TEXT_START_ONE_MINUTE;
    StartMessageIds[BG_STARTING_EVENT_SECOND] = BG_TEXT_START_HALF_MINUTE;
    StartMessageIds[BG_STARTING_EVENT_THIRD]  = BG_TEXT_START_FIFTEEN_SEC;
    StartMessageIds[BG_STARTING_EVENT_FOURTH] = BG_TEXT_BATTLE_HAS_BEGUN;
}

Battleground::~Battleground()
{
    // remove objects and creatures
    // (this is done automatically in mapmanager update, when the instance is reset after the reset time)
    uint32 size = uint32(BgCreatures.size());
    for (uint32 i = 0; i < size; ++i)
        DelCreature(i);

    size = uint32(BgObjects.size());
    for (uint32 i = 0; i < size; ++i)
        DelObject(i);

    sBattlegroundMgr->RemoveBattleground(GetTypeID(), GetInstanceID());
    // unload map
    if (m_Map)
    {
        m_Map->SetUnload();
        //unlink to prevent crash, always unlink all pointer reference before destruction
        m_Map->SetBG(nullptr);
        m_Map = nullptr;
    }
    // remove from bg free slot queue
    RemoveFromBGFreeSlotQueue();

    for (BattlegroundScoreMap::const_iterator itr = PlayerScores.begin(); itr != PlayerScores.end(); ++itr)
        delete itr->second;
}

void Battleground::Update(uint32 diff)
{
    if (!PreUpdateImpl(diff))
        return;

    if (!GetPlayersSize() && !IsReplay())
    {
        // Bot-only custom matches create their transient combatants after the
        // battleground map exists. Keep the empty shell alive while those
        // asynchronous clone loads are pending instead of deleting it before
        // the first participant can be inserted.
        if (HasCustomGamePendingClones())
            return;

        //BG is empty
        // if there are no players invited, delete BG
        // this will delete arena or bg object, where any player entered
        // [[   but if you use battleground object again (more battles possible to be played on 1 instance)
        //      then this condition should be removed and code:
        //      if (!GetInvitedCount(HORDE) && !GetInvitedCount(ALLIANCE))
        //          AddToFreeBGObjectsQueue(); // not yet implemented
        //      should be used instead of current
        // ]]
        // Battleground Template instance cannot be updated, because it would be deleted
        if (!GetInvitedCount(HORDE) && !GetInvitedCount(ALLIANCE))
            m_SetDeleteThis = true;
        return;
    }

    switch (GetStatus())
    {
        case STATUS_WAIT_JOIN:
            // Do not begin the countdown with a partially provisioned custom
            // roster. In an arena, starting after only one team's first clone
            // arrives can immediately satisfy the opposing-team win check.
            if (GetPlayersSize() && !HasCustomGamePendingClones())
            {
                _ProcessJoin(diff);
                _CheckSafePositions(diff);
            }
            break;
        case STATUS_IN_PROGRESS:
            // Arenas too (skirmishes): one whose last person disconnected, or
            // never came in for the seat they were offered, is not left running
            // for the bots. A rated arena keeps the stock grace for somebody who
            // drops - their rating is at stake - and the last person leaving
            // one padded with bots forfeits it (RemovePlayerAtLeave).
            if ((isBattleground() || (isArena() && !isRated())) && m_HasEverHadNonVirtualHumanParticipant)
            {
                if (HasAnyNonVirtualHumanParticipant(this))
                    m_NoNonVirtualHumanElapsed = 0;
                else if ((m_NoNonVirtualHumanElapsed += diff) >= NO_NON_VIRTUAL_HUMAN_END_DELAY_MS)
                {
                    TC_LOG_INFO("bg.battleground",
                        "Battleground::Update ending map={} instance={} because no non-virtual participants or custom-game spectators remained for {} ms.",
                        GetMapId(), GetInstanceID(), NO_NON_VIRTUAL_HUMAN_END_DELAY_MS);
                    EndNow();
                    return;
                }
            }

            _ProcessOfflineQueue();
            // Ahead of the bounds: a start exit is the way off a start the bounds
            // would otherwise count as outside the arena. Between two rounds
            // everyone is back behind the gates, where neither applies yet.
            if (!IsBetweenRounds())
            {
                _CheckStartExits(diff);
                _CheckBounds(diff);
            }
            // after 20 minutes without one team losing, the arena closes with no winner and no rating change
            if (isArena())
            {
                // A shuffle is several matches in a row: its limit is per round.
                if (IsSoloShuffle())
                    UpdateRounds(diff);
                else if (GetStartTime() >= 20 * MINUTE*IN_MILLISECONDS)
                {
                    EndAtTimeLimit();
                    return;
                }
            }
            else
            {
                // The resurrect queue is what puts a spirit guide's countdown on
                // a dead player's screen. A battleground that dies like an arena
                // has no spirit guide to queue for, so running it only produces a
                // prompt that can never be answered.
                if (!UsesArenaDeathRules())
                    _ProcessResurrect(diff);

                if (AllowsPrematureFinish() && sBattlegroundMgr->GetPrematureFinishTime() && (GetPlayersCountByTeam(ALLIANCE) < GetMinPlayersPerTeam() || GetPlayersCountByTeam(HORDE) < GetMinPlayersPerTeam()))
                    _ProcessProgress(diff);
                else if (m_PrematureCountDown)
                    m_PrematureCountDown = false;
            }
            break;
        case STATUS_WAIT_LEAVE:
            _ProcessLeave(diff);
            break;
        default:
            break;
    }

    // Update start time and reset stats timer
    m_StartTime += diff;
    m_ResetStatTimer += diff;

    PostUpdateImpl(diff);

    sScriptMgr->OnBattlegroundUpdate(this, diff);
}


inline void Battleground::_CheckSafePositions(uint32 diff)
{
    float maxDist = GetStartMaxDist();
    if (!maxDist)
        return;

    m_ValidStartPositionTimer += diff;
    if (m_ValidStartPositionTimer >= CHECK_PLAYER_POSITION_INVERVAL)
    {
        m_ValidStartPositionTimer = 0;

        for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
        {
            if (Player* player = ObjectAccessor::FindPlayer(itr->first))
            {
                // Spectators are not combatants and may intentionally move
                // outside either team's closed-gate preparation area. They
                // must never be snapped to a team start by the preparation
                // anti-exploit check, even if stale state temporarily leaves
                // one present in m_Players during a worldport transition.
                if (player->IsGameMaster() || player->IsSpectator())
                    continue;

                Position pos = player->GetPosition();
                Position const* startPos = GetStartPositionFor(player->GetGUID(), player->GetBGTeam());
                if (pos.GetExactDistSq(startPos) > maxDist)
                {
                    TC_LOG_DEBUG("bg.battleground", "BATTLEGROUND: Sending {} back to start location (map: {}) (possible exploit)", player->GetName(), GetMapId());
                    player->TeleportTo(GetMapId(), startPos->GetPositionX(), startPos->GetPositionY(), startPos->GetPositionZ(), startPos->GetOrientation());
                }

                // An arena without gates relies on this distance alone to keep
                // the teams apart until the start, and a pet sent across is as
                // much an early start as its owner walking over.
                if (isArena())
                {
                    std::vector<Unit*> const controlled(player->m_Controlled.begin(), player->m_Controlled.end());
                    for (Unit* unit : controlled)
                    {
                        if (!unit || unit->GetTypeId() == TYPEID_PLAYER || !unit->IsInWorld() || unit->GetMapId() != GetMapId())
                            continue;

                        if (unit->GetPosition().GetExactDistSq(startPos) > maxDist)
                            unit->NearTeleportTo(startPos->GetPositionX(), startPos->GetPositionY(), startPos->GetPositionZ(), startPos->GetOrientation());
                    }
                }
            }
        }
    }
}

bool Battleground::IsInsideBoundary(float x, float y) const
{
    std::vector<BattlegroundBoundaryPoint> const& poly = m_Bounds.Boundary;
    if (poly.size() < 3)
        return true;

    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        if ((poly[i].Y > y) != (poly[j].Y > y)
            && x < (poly[j].X - poly[i].X) * (y - poly[i].Y) / (poly[j].Y - poly[i].Y) + poly[i].X)
            inside = !inside;
    }
    return inside;
}

// Moves (x, y) to the nearest point of the boundary's edge, then a couple of
// yards in from it, so whoever is put there is not straight back outside.
bool Battleground::_ClosestPointInsideBoundary(float& x, float& y) const
{
    std::vector<BattlegroundBoundaryPoint> const& poly = m_Bounds.Boundary;
    if (poly.size() < 3)
        return true;

    float constexpr inset = 2.0f;
    float bestDistSq = std::numeric_limits<float>::max();
    float bestX = x, bestY = y, edgeX = 0.0f, edgeY = 0.0f;

    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        float const ax = poly[j].X, ay = poly[j].Y;
        float const dx = poly[i].X - ax, dy = poly[i].Y - ay;
        float const lenSq = dx * dx + dy * dy;
        if (lenSq < 0.0001f)
            continue;

        float t = ((x - ax) * dx + (y - ay) * dy) / lenSq;
        t = std::max(0.0f, std::min(1.0f, t));
        float const px = ax + t * dx, py = ay + t * dy;
        float const distSq = (px - x) * (px - x) + (py - y) * (py - y);
        if (distSq < bestDistSq)
        {
            bestDistSq = distSq;
            bestX = px;
            bestY = py;
            edgeX = dx;
            edgeY = dy;
        }
    }

    // Step off the edge along whichever of its two normals lands inside. The
    // outline's winding is not something the rows have to get right.
    float const len = std::sqrt(edgeX * edgeX + edgeY * edgeY);
    if (len < 0.0001f)
        return false;

    float const nx = -edgeY / len, ny = edgeX / len;
    for (float sign : { 1.0f, -1.0f })
    {
        float const cx = bestX + sign * nx * inset, cy = bestY + sign * ny * inset;
        if (IsInsideBoundary(cx, cy))
        {
            x = cx;
            y = cy;
            return true;
        }
    }

    return false;
}

// Returns a unit that is below the arena's floor or outside its outline. Only
// after the gates open: before that everyone is behind their own gate, which in
// the Ring of Valor is a lift shaft well below the floor this would test for.
void Battleground::_CheckBounds(uint32 diff)
{
    if (!m_Bounds.Enabled)
        return;

    m_BoundsElapsed += diff;
    if (m_BoundsElapsed < m_Bounds.GraceMs)
        return;

    m_BoundsCheckTimer += diff;
    if (m_BoundsCheckTimer < BOUNDS_CHECK_INTERVAL)
        return;
    m_BoundsCheckTimer = 0;

    for (auto const& itr : GetPlayers())
    {
        Player* player = ObjectAccessor::FindPlayer(itr.first);
        // A teleport already in flight will fix the position by itself;
        // issuing another one on top of it just fights the first.
        if (!player || !player->IsInWorld() || player->IsBeingTeleported() || player->GetMapId() != GetMapId())
            continue;

        if (player->IsGameMaster() || player->IsSpectator())
            continue;

        if (player->IsAlive())
            _ReturnInsideBounds(player);

        // The server moves these itself, so nothing on a client is keeping
        // them out of the floor. Players among them (a mind-controlled enemy)
        // are on the roster and handled as themselves.
        std::vector<Unit*> const controlled(player->m_Controlled.begin(), player->m_Controlled.end());
        for (Unit* unit : controlled)
            if (unit && unit->GetTypeId() != TYPEID_PLAYER && unit->IsInWorld() && unit->IsAlive() && unit->GetMapId() == GetMapId())
                _ReturnInsideBounds(unit);
    }
}

// This realm is factionless: a troll can fight for Stormpike. But a reputation
// faction's NPCs are judged by the player's standing with it - the client's
// colouring and attack check, and the NPC's side of the server's - and a race's
// standings are its own side's (a troll starts at war with Stormpike Guard and
// at peace with the Frostwolves). So while somebody plays a match, every
// reputation faction of the Alliance and Horde trees (Faction.dbc parents
// 469/891 and 67/892: the cities and the battleground factions) is forced
// friendly to their side and hostile to the other. team 0 takes it off again.
static void SetBattlegroundSideReactions(Player* player, uint32 team)
{
    static std::vector<std::pair<uint32, uint32>> const sideFactions = []
    {
        auto sideOf = [](FactionEntry const* faction) -> uint32
        {
            for (uint8 depth = 0; faction && depth < 4; ++depth)
            {
                if (faction->ID == 469 || faction->ID == 891)
                    return ALLIANCE;
                if (faction->ID == 67 || faction->ID == 892)
                    return HORDE;
                faction = sFactionStore.LookupEntry(faction->ParentFactionID);
            }
            return 0;
        };

        std::vector<std::pair<uint32, uint32>> factions;
        for (FactionEntry const* faction : sFactionStore)
            if (faction->CanHaveReputation())
                if (uint32 side = sideOf(faction))
                    factions.emplace_back(faction->ID, side);
        return factions;
    }();

    ReputationMgr& reputation = player->GetReputationMgr();
    bool const onSide = team == ALLIANCE || team == HORDE;
    for (auto const& [factionId, side] : sideFactions)
        reputation.ApplyForceReaction(factionId, side == team ? REP_FRIENDLY : REP_HOSTILE, onSide);
    reputation.SendForceReactions();
}

// A start exit carries people across a void, so it is shown as the teleport it
// is, with the flash the Gurubashi ring's teleports use: Teleport.mdx at double
// size on everyone moved, pets included, and on the ground both where they
// vanish and where they come down. The ground flashes are cast by an invisible
// World Trigger.
constexpr uint32 SPELL_START_EXIT_TELEPORT_VISUAL = 64446; // Teleport Visual Only
constexpr uint32 NPC_START_EXIT_VISUAL_TRIGGER = 22515;    // World Trigger

// Once per team, when the gates have been open for the exit's delay: whoever is
// still at the team's start goes to its exit, with what they brought. See
// BattlegroundStartExit.
void Battleground::_CheckStartExits(uint32 diff)
{
    if (!m_StartExits[TEAM_ALLIANCE].Enabled && !m_StartExits[TEAM_HORDE].Enabled)
        return;

    m_StartExitElapsed += diff;

    for (uint8 i = 0; i < PVP_TEAMS_COUNT; ++i)
    {
        BattlegroundStartExit const& exit = m_StartExits[i];
        if (!exit.Enabled || m_StartExitDone[i] || m_StartExitElapsed < exit.DelayMs)
            continue;

        m_StartExitDone[i] = true;

        Position const* start = GetTeamStartPosition(TeamId(i));
        if (!start)
            continue;

        uint32 const team = i == TEAM_ALLIANCE ? ALLIANCE : HORDE;
        uint32 moved = 0;
        for (auto const& [guid, bgPlayer] : GetPlayers())
        {
            if (bgPlayer.Team != team)
                continue;

            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player || !player->IsInWorld() || player->IsBeingTeleported() || player->GetMapId() != GetMapId())
                continue;

            if (player->IsGameMaster() || player->IsSpectator() || !player->IsAlive())
                continue;

            if (exit.BotsOnly && !IsBotParticipantSession(player->GetSession()))
                continue;

            if (player->GetExactDist(start) > exit.Radius)
                continue;

            // Pets first: the server moves them by navmesh, and there is none
            // from the start to the fight. Already standing at the exit, they are
            // also not unsummoned by their owner's teleport for being far away.
            std::vector<Unit*> const controlled(player->m_Controlled.begin(), player->m_Controlled.end());
            for (Unit* unit : controlled)
            {
                if (unit && unit->GetTypeId() != TYPEID_PLAYER && !unit->IsTotem() && unit->IsInWorld() && unit->IsAlive() &&
                    unit->GetMapId() == GetMapId() && unit->GetExactDist(start) <= exit.Radius)
                {
                    unit->NearTeleportTo(exit.Exit.GetPositionX(), exit.Exit.GetPositionY(), exit.Exit.GetPositionZ(), exit.Exit.GetOrientation());
                    unit->CastSpell(unit, SPELL_START_EXIT_TELEPORT_VISUAL, TRIGGERED_FULL_MASK);
                }
            }

            player->CastSpell(player, SPELL_START_EXIT_TELEPORT_VISUAL, TRIGGERED_FULL_MASK);
            player->TeleportTo(GetMapId(), exit.Exit.GetPositionX(), exit.Exit.GetPositionY(), exit.Exit.GetPositionZ(), exit.Exit.GetOrientation());
            ++moved;
        }

        // On the ground where they vanished, straight away, and where they come
        // down. A person lands when their client answers the teleport, a moment
        // after this, so that flash waits for them.
        if (moved)
        {
            if (Map* map = FindBgMap())
            {
                auto groundFlash = [map](Position const& where, Milliseconds delay)
                {
                    if (TempSummon* flash = map->SummonCreature(NPC_START_EXIT_VISUAL_TRIGGER, where, nullptr, 5 * IN_MILLISECONDS))
                        flash->m_Events.AddEventAtOffset([flash]()
                        {
                            flash->CastSpell(flash, SPELL_START_EXIT_TELEPORT_VISUAL, TRIGGERED_FULL_MASK);
                        }, delay);
                };
                groundFlash(*start, Milliseconds(0));
                groundFlash(exit.Exit, Milliseconds(500));
            }
        }

        TC_LOG_DEBUG("bg.battleground", "Battleground::_CheckStartExits: map {} instance {}: moved {} of team {} from their start to ({:.1f}, {:.1f}, {:.1f}).",
            GetMapId(), GetInstanceID(), moved, team, exit.Exit.GetPositionX(), exit.Exit.GetPositionY(), exit.Exit.GetPositionZ());
    }
}

bool Battleground::_ReturnInsideBounds(Unit* unit)
{
    float x = unit->GetPositionX();
    float y = unit->GetPositionY();
    bool const fallen = unit->GetPositionZ() < m_Bounds.MinZ;
    bool const outside = !IsInsideBoundary(x, y);
    if (!fallen && !outside)
        return false;

    Position dest = m_Bounds.SafePosition;
    bool placed = false;

    if (!outside || _ClosestPointInsideBoundary(x, y))
    {
        if (m_Bounds.ProbeZ > m_Bounds.MinZ)
        {
            float const floorZ = GetBgMap()->GetHeight(unit->GetPhaseMask(), x, y, m_Bounds.ProbeZ, true, m_Bounds.ProbeZ - m_Bounds.MinZ);
            if (floorZ > m_Bounds.MinZ && floorZ <= m_Bounds.ProbeZ)
            {
                dest.Relocate(x, y, floorZ + 0.5f);
                placed = true;
            }
        }

        if (!placed && m_Bounds.KeepXY)
        {
            dest.Relocate(x, y, m_Bounds.SafePosition.GetPositionZ());
            placed = true;
        }
    }

    TC_LOG_DEBUG("bg.battleground", "Battleground::_ReturnInsideBounds: map {} instance {}: {} at ({:.1f}, {:.1f}, {:.1f}) {} - returned to ({:.1f}, {:.1f}, {:.1f})",
        GetMapId(), GetInstanceID(), unit->GetName(), unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(),
        fallen ? "fell below the floor" : "left the arena", dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());

    if (Player* player = unit->ToPlayer())
        player->TeleportTo(GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), player->GetOrientation());
    else
        unit->NearTeleportTo(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), unit->GetOrientation());

    return true;
}

bool Battleground::HandlePlayerUnderMap(Player* player)
{
    if (!m_Bounds.Enabled || !player)
        return false;

    Position const& safe = m_Bounds.SafePosition;
    player->TeleportTo(GetMapId(), safe.GetPositionX(), safe.GetPositionY(), safe.GetPositionZ(), player->GetOrientation());
    return true;
}

inline void Battleground::_ProcessOfflineQueue()
{
    // remove offline players from bg after 5 minutes
    if (!m_OfflineQueue.empty())
    {
        BattlegroundPlayerMap::iterator itr = m_Players.find(*(m_OfflineQueue.begin()));
        if (itr != m_Players.end())
        {
            if (itr->second.OfflineRemoveTime <= GameTime::GetGameTime())
            {
                if (isBattleground() && sWorld->getBoolConfig(CONFIG_BATTLEGROUND_TRACK_DESERTERS) &&
                    (GetStatus() == STATUS_IN_PROGRESS || GetStatus() == STATUS_WAIT_JOIN))
                {
                    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_DESERTER_TRACK);
                    stmt->setUInt32(0, itr->first.GetCounter());
                    stmt->setUInt8(1, BG_DESERTION_TYPE_OFFLINE);
                    CharacterDatabase.Execute(stmt);
                }

                RemovePlayerAtLeave(itr->first, true, true);// remove player from BG
                m_OfflineQueue.pop_front();                 // remove from offline queue
                //do not use itr for anything, because it is erased in RemovePlayerAtLeave()
            }
        }
    }
}

inline void Battleground::_ProcessResurrect(uint32 diff)
{
    // *********************************************************
    // ***        BATTLEGROUND RESURRECTION SYSTEM           ***
    // *********************************************************
    // this should be handled by spell system
    m_LastResurrectTime += diff;
    if (m_LastResurrectTime >= GetResurrectionInterval())
    {
        if (GetReviveQueueSize())
        {
            for (std::map<ObjectGuid, GuidVector>::iterator itr = m_ReviveQueue.begin(); itr != m_ReviveQueue.end(); ++itr)
            {
                Creature* sh = nullptr;
                for (GuidVector::const_iterator itr2 = (itr->second).begin(); itr2 != (itr->second).end(); ++itr2)
                {
                    Player* player = ObjectAccessor::FindConnectedPlayer(*itr2);
                    if (!player)
                        continue;

                    if (!sh && player->IsInWorld())
                    {
                        sh = player->GetMap()->GetCreature(itr->first);
                        // only for visual effect
                        if (sh)
                            // Spirit Heal, effect 117
                            sh->CastSpell(sh, SPELL_SPIRIT_HEAL, true);
                    }

                    // Resurrection visual
                    player->CastSpell(player, SPELL_RESURRECTION_VISUAL, true);
                    m_ResurrectQueue.push_back(*itr2);
                }

                // Hard override for managed playerbot avatars (virtual sessions):
                // when a spirit guide resurrection wave happens, include every dead
                // playerbot in this battleground, regardless of graveyard location.
                for (BattlegroundPlayerMap::const_iterator bgPlayerItr = m_Players.begin(); bgPlayerItr != m_Players.end(); ++bgPlayerItr)
                {
                    Player* botPlayer = ObjectAccessor::FindConnectedPlayer(bgPlayerItr->first);
                    if (!botPlayer || botPlayer->IsAlive() || !botPlayer->IsInWorld())
                        continue;

                    WorldSession* session = botPlayer->GetSession();
                    if (!session || !session->IsVirtualSession())
                        continue;

                    if (std::find(m_ResurrectQueue.begin(), m_ResurrectQueue.end(), botPlayer->GetGUID()) != m_ResurrectQueue.end())
                        continue;

                    botPlayer->CastSpell(botPlayer, SPELL_RESURRECTION_VISUAL, true);
                    m_ResurrectQueue.push_back(botPlayer->GetGUID());
                }
                (itr->second).clear();
            }

            m_ReviveQueue.clear();
            m_LastResurrectTime = 0;
        }
        else
            // queue is clear and time passed, just update last resurrection time
            m_LastResurrectTime = 0;
    }
    else if (m_LastResurrectTime > 500)    // Resurrect players only half a second later, to see spirit heal effect on NPC
    {
        for (GuidVector::const_iterator itr = m_ResurrectQueue.begin(); itr != m_ResurrectQueue.end(); ++itr)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(*itr);
            if (!player)
                continue;
            player->ResurrectPlayer(1.0f);
            player->CastSpell(player, 6962, true);
            player->CastSpell(player, SPELL_SPIRIT_HEAL_MANA, true);
            player->SpawnCorpseBones(false);
            // A hunter gets up with his pet beside him, alive and fully happy -
            // the wave would otherwise send him back in alone, casting Revive Pet
            // in the middle of a fight he just died to.
            player->ResurrectPetAtSpiritGuide();
        }
        m_ResurrectQueue.clear();
    }
}

uint32 Battleground::GetPrematureWinner()
{
    uint32 winner = 0;
    if (GetPlayersCountByTeam(ALLIANCE) >= GetMinPlayersPerTeam())
        winner = ALLIANCE;
    else if (GetPlayersCountByTeam(HORDE) >= GetMinPlayersPerTeam())
        winner = HORDE;

    return winner;
}

inline void Battleground::_ProcessProgress(uint32 diff)
{
    // *********************************************************
    // ***           BATTLEGROUND BALLANCE SYSTEM            ***
    // *********************************************************
    // if less then minimum players are in on one side, then start premature finish timer
    if (!m_PrematureCountDown)
    {
        m_PrematureCountDown = true;
        m_PrematureCountDownTimer = sBattlegroundMgr->GetPrematureFinishTime();
    }
    else if (m_PrematureCountDownTimer < diff)
    {
        // time's up!
        EndBattleground(GetPrematureWinner());
        m_PrematureCountDown = false;
    }
    else if (!sBattlegroundMgr->isTesting())
    {
        uint32 newtime = m_PrematureCountDownTimer - diff;
        // announce every minute
        if (newtime > (MINUTE * IN_MILLISECONDS))
        {
            if (newtime / (MINUTE * IN_MILLISECONDS) != m_PrematureCountDownTimer / (MINUTE * IN_MILLISECONDS))
                PSendMessageToAll(LANG_BATTLEGROUND_PREMATURE_FINISH_WARNING, CHAT_MSG_SYSTEM, nullptr, (uint32)(m_PrematureCountDownTimer / (MINUTE * IN_MILLISECONDS)));
        }
        else
        {
            //announce every 15 seconds
            if (newtime / (15 * IN_MILLISECONDS) != m_PrematureCountDownTimer / (15 * IN_MILLISECONDS))
                PSendMessageToAll(LANG_BATTLEGROUND_PREMATURE_FINISH_WARNING_SECS, CHAT_MSG_SYSTEM, nullptr, (uint32)(m_PrematureCountDownTimer / IN_MILLISECONDS));
        }
        m_PrematureCountDownTimer = newtime;
    }
}

bool Battleground::PrepareStartingArea()
{
    if (m_StartingAreaPrepared)
        return true;
    m_StartingAreaPrepared = true;

    if (!FindBgMap())
    {
        TC_LOG_ERROR("bg.battleground", "Battleground::PrepareStartingArea: map (map id: {}, instance id: {}) is not created!", m_MapId, m_InstanceID);
        EndNow();
        return false;
    }

    if (!SetupBattleground())
    {
        EndNow();
        return false;
    }

    StartingEventCloseDoors();
    return true;
}

inline void Battleground::_ProcessJoin(uint32 diff)
{
    // *********************************************************
    // ***           BATTLEGROUND STARTING SYSTEM            ***
    // *********************************************************
    // Somebody invited into a clone's seat is still deciding or still loading
    // in. The gates wait for them; HandOverPreparationSeat restarts the
    // countdown when they arrive, and an invite left to lapse lets it run on.
    PrunePreparationSeatTakers();
    if (m_PreparationSeatTakers.empty())
        ModifyStartDelayTime(diff);

    if (m_ResetStatTimer > 5000)
    {
        m_ResetStatTimer = 0;
        for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
            if (Player* player = ObjectAccessor::FindPlayer(itr->first))
                player->ResetAllPowers();
    }

    if (!(m_Events & BG_STARTING_EVENT_1))
    {
        m_Events |= BG_STARTING_EVENT_1;

        // Normally already done when the map was created; this only catches
        // a battleground whose map came from somewhere else.
        if (!PrepareStartingArea())
            return;

        // Arena replays are not real matches. As soon as the replay viewer has caused the map to be
        // created and SetupBattleground() has succeeded, immediately advance to IN_PROGRESS so the
        // client does not sit through the arena prep countdown.
        // Do this directly instead of calling SkipStartDelay(); SkipStartDelay() is normal match start
        // machinery and can still run extra arena logic that is not useful for replay-only instances.
        if (IsReplay())
        {
            m_Events |= BG_STARTING_EVENT_2;
            m_Events |= BG_STARTING_EVENT_3;
            m_Events |= BG_STARTING_EVENT_4;
            SetStartDelayTime(0);
            StartingEventOpenDoors();
            if (StartMessageIds[BG_STARTING_EVENT_FOURTH])
                SendBroadcastText(StartMessageIds[BG_STARTING_EVENT_FOURTH], CHAT_MSG_BG_SYSTEM_NEUTRAL);
            SetStatus(STATUS_IN_PROGRESS);
            return;
        }

        if (IsCustomGame() && HasCustomGameBotOnlyPreparation())
        {
            // A custom match whose combat roster contains only playerbots or
            // clones does not need the full human preparation window. Mark
            // the longer warning stages complete so they cannot announce
            // misleading 30/60-second countdowns on the following ticks.
            m_Events |= BG_STARTING_EVENT_2;
            m_Events |= BG_STARTING_EVENT_3;
            SetStartDelayTime(BG_START_DELAY_10S);

            // Arenas have an exact ten-second broadcast text. Battleground
            // third-stage texts are commonly fifteen or thirty seconds, so
            // suppress those rather than announce an incorrect duration.
            if (isArena() && StartMessageIds[BG_STARTING_EVENT_THIRD])
                SendBroadcastText(StartMessageIds[BG_STARTING_EVENT_THIRD], CHAT_MSG_BG_SYSTEM_NEUTRAL);
        }
        else
        {
            SetStartDelayTime(StartDelayTimes[BG_STARTING_EVENT_FIRST]);
            // First start warning - 2 or 1 minute
            if (StartMessageIds[BG_STARTING_EVENT_FIRST])
                SendBroadcastText(StartMessageIds[BG_STARTING_EVENT_FIRST], CHAT_MSG_BG_SYSTEM_NEUTRAL);
        }
    }
    // After 1 minute or 30 seconds, warning is signaled
    else if (GetStartDelayTime() <= StartDelayTimes[BG_STARTING_EVENT_SECOND] && !(m_Events & BG_STARTING_EVENT_2))
    {
        m_Events |= BG_STARTING_EVENT_2;
        if (StartMessageIds[BG_STARTING_EVENT_SECOND])
            SendBroadcastText(StartMessageIds[BG_STARTING_EVENT_SECOND], CHAT_MSG_BG_SYSTEM_NEUTRAL);
    }
    // After 30 or 15 seconds, warning is signaled
    else if (GetStartDelayTime() <= StartDelayTimes[BG_STARTING_EVENT_THIRD] && !(m_Events & BG_STARTING_EVENT_3))
    {
        m_Events |= BG_STARTING_EVENT_3;
        if (StartMessageIds[BG_STARTING_EVENT_THIRD])
            SendBroadcastText(StartMessageIds[BG_STARTING_EVENT_THIRD], CHAT_MSG_BG_SYSTEM_NEUTRAL);
    }
    // Delay expired (after 2 or 1 minute)
    else if (GetStartDelayTime() <= 0 && !(m_Events & BG_STARTING_EVENT_4) && m_PreparationSeatTakers.empty())
        SkipStartDelay();
}

inline void Battleground::_ProcessLeave(uint32 diff)
{
    // *********************************************************
    // ***           BATTLEGROUND ENDING SYSTEM              ***
    // *********************************************************
    // remove all players from battleground after 2 minutes
    if (m_EndTime <= diff)
    {
        m_EndTime = 0;
        BattlegroundPlayerMap::iterator itr, next;
        for (itr = m_Players.begin(); itr != m_Players.end(); itr = next)
        {
            next = itr;
            ++next;
            //itr is erased here!
            RemovePlayerAtLeave(itr->first, true, true);// remove player from BG
            // do not change any battleground's private variables
        }
    }
    else
        m_EndTime -= diff;
}

Player* Battleground::_GetPlayer(ObjectGuid guid, bool offlineRemove, char const* context) const
{
    Player* player = nullptr;
    if (!offlineRemove)
    {
        // should this be ObjectAccessor::FindConnectedPlayer() to return players teleporting ?
        player = ObjectAccessor::FindPlayer(guid);
        if (!player)
            TC_LOG_ERROR("bg.battleground", "Battleground::{}: player ({}) not found for BG (map: {}, instance id: {})!",
                context, guid.ToString(), m_MapId, m_InstanceID);
    }
    return player;
}

Player* Battleground::_GetPlayerForTeam(uint32 teamId, BattlegroundPlayerMap::const_iterator itr, char const* context) const
{
    Player* player = _GetPlayer(itr, context);
    if (player)
    {
        uint32 team = itr->second.Team;
        if (!team)
            team = player->GetTeam();
        if (team != teamId)
            player = nullptr;
    }
    return player;
}

void Battleground::SetTeamStartPosition(TeamId teamId, Position const& pos)
{
    ASSERT(teamId < TEAM_NEUTRAL);
    StartPosition[teamId] = pos;
}

Position const* Battleground::GetTeamStartPosition(TeamId teamId) const
{
    ASSERT(teamId < TEAM_NEUTRAL);
    return &StartPosition[teamId];
}

uint32 Battleground::GetSquadSize() const
{
    if (!IsSquadMatch())
        return GetMaxPlayersPerTeam();

    if (GetArenaType())
        return GetArenaType();

    uint32 const squadsPerSide = (GetSquadCount() + 1) / 2;
    return std::max<uint32>(1, GetMaxPlayersPerTeam() / squadsPerSide);
}

std::string const& Battleground::GetSquadName(uint8 squad) const
{
    static std::string const unknown = "unknown";
    return squad < m_SquadStarts.size() ? m_SquadStarts[squad].Name : unknown;
}

uint8 Battleground::GetPlayerSquad(ObjectGuid guid) const
{
    auto const itr = m_SquadSeats.find(guid);
    return itr != m_SquadSeats.end() ? itr->second : BATTLEGROUND_NO_SQUAD;
}

void Battleground::SetSquadSeat(ObjectGuid guid, uint8 squad)
{
    if (squad < GetSquadCount())
        m_SquadSeats[guid] = squad;
}

uint8 Battleground::ClaimSquadSeat(ObjectGuid guid, uint32 team, bool healer)
{
    if (!IsSquadMatch())
        return BATTLEGROUND_NO_SQUAD;

    uint8 const held = GetPlayerSquad(guid);
    if (held != BATTLEGROUND_NO_SQUAD)
        return held;

    // The emptiest squad on this side. A full one still takes the seat when
    // the whole side is full: a crowded corner beats nowhere to stand, and the
    // queue and the bot fill never ask for more seats than a side has.
    // With healers wanted, a squad "fits" a healer while it has a free seat
    // and anyone else while it has a seat more than its missing healers need;
    // fitting squads come first, and among them a healer goes where most are
    // missing. Clones load out of order, so this keeps the last seat of a
    // squad without its healer for the healer still on the way.
    uint32 const size = GetSquadSize();
    uint8 best = BATTLEGROUND_NO_SQUAD;
    uint32 bestTaken = 0;
    uint32 bestMissing = 0;
    bool bestFits = false;
    for (uint8 squad = 0; squad < GetSquadCount(); ++squad)
    {
        if (GetSquadSide(squad) != team)
            continue;

        uint32 const taken = CountSquadSeats(squad);
        uint32 missing = 0;
        bool fits = true;
        if (m_SquadHealersWanted)
        {
            uint32 const healers = CountSquadHealers(squad);
            uint32 const seatsLeft = size > taken ? size - taken : 0;
            missing = m_SquadHealersWanted > healers ? m_SquadHealersWanted - healers : 0;
            fits = healer ? seatsLeft > 0 : seatsLeft > missing;
        }

        bool better;
        if (best == BATTLEGROUND_NO_SQUAD)
            better = true;
        else if (fits != bestFits)
            better = fits;
        else if (healer && missing != bestMissing)
            better = missing > bestMissing;
        else
            better = taken < bestTaken;

        if (better)
        {
            best = squad;
            bestTaken = taken;
            bestMissing = missing;
            bestFits = fits;
        }
    }

    if (best == BATTLEGROUND_NO_SQUAD)
        best = 0;

    m_SquadSeats[guid] = best;
    return best;
}

void Battleground::ReleaseSquadSeat(ObjectGuid guid)
{
    m_SquadSeats.erase(guid);
}

uint32 Battleground::CountSquadSeats(uint8 squad) const
{
    return uint32(std::count_if(m_SquadSeats.begin(), m_SquadSeats.end(),
        [squad](std::pair<ObjectGuid const, uint8> const& seat) { return seat.second == squad; }));
}

uint32 Battleground::CountSquadHealers(uint8 squad) const
{
    // Seat holders are connected wherever they are: people still on their way
    // in, and clones from the moment they are made.
    return uint32(std::count_if(m_SquadSeats.begin(), m_SquadSeats.end(),
        [squad](std::pair<ObjectGuid const, uint8> const& seat)
        {
            return seat.second == squad && Arena::IsSoloShuffleHealer(ObjectAccessor::FindConnectedPlayer(seat.first));
        }));
}

bool Battleground::AreTeammates(ObjectGuid left, ObjectGuid right) const
{
    uint32 const leftTeam = GetPlayerTeam(left);
    uint32 const rightTeam = GetPlayerTeam(right);
    if (!leftTeam || !rightTeam)
        return false;

    if (IsSquadMatch())
    {
        uint8 const leftSquad = GetPlayerSquad(left);
        return leftSquad != BATTLEGROUND_NO_SQUAD && leftSquad == GetPlayerSquad(right);
    }

    return leftTeam == rightTeam;
}

Position const* Battleground::GetStartPositionFor(ObjectGuid guid, uint32 team) const
{
    if (IsSquadMatch())
    {
        uint8 const squad = GetPlayerSquad(guid);
        if (squad < GetSquadCount())
            return &m_SquadStarts[squad].Start;
    }

    return GetTeamStartPosition(GetTeamIndexByTeamId(team));
}

Position const* Battleground::TakeStartPosition(ObjectGuid guid, uint32 team, bool healer)
{
    ClaimSquadSeat(guid, team, healer);
    return GetStartPositionFor(guid, team);
}

Position Battleground::GetSquadArenaCentre() const
{
    if (!IsSquadMatch())
    {
        Position const* alliance = GetTeamStartPosition(TEAM_ALLIANCE);
        Position const* horde = GetTeamStartPosition(TEAM_HORDE);
        return Position((alliance->GetPositionX() + horde->GetPositionX()) / 2.0f,
            (alliance->GetPositionY() + horde->GetPositionY()) / 2.0f,
            (alliance->GetPositionZ() + horde->GetPositionZ()) / 2.0f);
    }

    float x = 0.0f, y = 0.0f, z = 0.0f;
    for (BattlegroundSquadStart const& squad : m_SquadStarts)
    {
        x += squad.Start.GetPositionX();
        y += squad.Start.GetPositionY();
        z += squad.Start.GetPositionZ();
    }

    float const count = float(m_SquadStarts.size());
    return Position(x / count, y / count, z / count);
}

namespace
{
    // The two squads a viewer and someone else play in, when they share a
    // squad match and sit on the same side without being the same squad - the
    // only pairing whose real faction and arena byte read "friendly" when they
    // are not. Squads on opposite sides already carry the enemy's values.
    bool GetSameSideRivalSquads(Player const* other, Player const* viewer, Battleground const*& battleground)
    {
        if (!other || !viewer || other == viewer)
            return false;

        battleground = viewer->GetBattleground();
        if (!battleground || !battleground->IsSquadMatch() || battleground != other->GetBattleground())
            return false;

        uint8 const viewerSquad = battleground->GetPlayerSquad(viewer->GetGUID());
        uint8 const otherSquad = battleground->GetPlayerSquad(other->GetGUID());
        return viewerSquad != BATTLEGROUND_NO_SQUAD && otherSquad != BATTLEGROUND_NO_SQUAD &&
            viewerSquad != otherSquad &&
            Battleground::GetSquadSide(viewerSquad) == Battleground::GetSquadSide(otherSquad);
    }

    // The faction a battleground hands the side `team` plays on: the same race
    // factions Battleground::AddPlayer uses.
    uint32 GetBattlegroundSideFaction(uint32 team)
    {
        ChrRacesEntry const* race = sChrRacesStore.LookupEntry(team == HORDE ? RACE_BLOODELF : RACE_HUMAN);
        return race ? race->FactionID : 0;
    }
}

bool Battleground::GetSquadViewFaction(Unit const* unit, Player const* viewer, uint32& faction)
{
    Player const* owner = unit ? unit->GetAffectingPlayer() : nullptr;
    Battleground const* battleground = nullptr;
    if (!GetSameSideRivalSquads(owner, viewer, battleground))
        return false;

    uint32 const viewerTeam = battleground->GetPlayerTeam(viewer->GetGUID());
    faction = GetBattlegroundSideFaction(viewerTeam == HORDE ? ALLIANCE : HORDE);
    return faction != 0;
}

bool Battleground::GetSquadViewArenaFaction(Player const* player, Player const* viewer, uint8& arenaFaction)
{
    Battleground const* battleground = nullptr;
    if (!GetSameSideRivalSquads(player, viewer, battleground))
        return false;

    // Player::SetBGTeam: Alliance 1, Horde 0. The viewer's client compares
    // everyone against its own, so the rival squad gets the other one.
    arenaFaction = battleground->GetPlayerTeam(viewer->GetGUID()) == ALLIANCE ? 0 : 1;
    return true;
}

void Battleground::SetSquadArenaTeam(uint8 squad, uint32 arenaTeamId, uint32 matchmakerRating)
{
    if (squad >= MAX_BATTLEGROUND_SQUADS)
        return;

    m_SquadArenaTeamIds[squad] = arenaTeamId;
    m_SquadArenaMMR[squad] = matchmakerRating;
}

uint32 Battleground::GetSquadOpponentsMatchmakerRating(uint8 squad) const
{
    uint64 sum = 0;
    uint32 count = 0;
    for (uint8 other = 0; other < MAX_BATTLEGROUND_SQUADS; ++other)
    {
        if (other == squad || !m_SquadArenaTeamIds[other])
            continue;

        sum += m_SquadArenaMMR[other];
        ++count;
    }

    return count ? uint32(sum / count) : GetSquadMatchmakerRating(squad);
}

bool Battleground::IsWinningParticipant(ObjectGuid guid, uint32 team, uint32 winner) const
{
    if (IsSquadMatch())
        return m_WinnerSquad != BATTLEGROUND_NO_SQUAD && GetPlayerSquad(guid) == m_WinnerSquad;

    return team == winner;
}

void Battleground::SendPacketToAll(WorldPacket const* packet)
{
    for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
        if (Player* player = _GetPlayer(itr, "SendPacketToAll"))
            player->SendDirectMessage(packet);

    // Custom-game spectators live outside m_Players. Forward shared BG
    // packets so their WorldStateUI, sounds, and other match-wide state
    // remain synchronized with both custom arenas and custom battlegrounds.
    if (m_IsCustomGame)
        for (Player* spectator : m_Spectators)
            if (spectator && spectator->IsInWorld() && spectator->GetBattlegroundId() == GetInstanceID())
                spectator->SendDirectMessage(packet);
}

void Battleground::SendPacketToTeam(uint32 TeamID, WorldPacket const* packet, Player* sender, bool self)
{
    for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
    {
        if (Player* player = _GetPlayerForTeam(TeamID, itr, "SendPacketToTeam"))
        {
            if (self || sender != player)
                player->SendDirectMessage(packet);
        }
    }
}

void Battleground::SendChatMessage(Creature* source, uint8 textId, WorldObject* target /*= nullptr*/)
{
    sCreatureTextMgr->SendChat(source, textId, target);
}

void Battleground::SendBroadcastText(uint32 id, ChatMsg msgType, WorldObject const* target)
{
    if (!sObjectMgr->GetBroadcastText(id))
    {
        TC_LOG_ERROR("bg.battleground", "Battleground::SendBroadcastText: `broadcast_text` (ID: {}) was not found", id);
        return;
    }

    Trinity::BroadcastTextBuilder builder(nullptr, msgType, id, GENDER_MALE, target);
    Trinity::LocalizedPacketDo<Trinity::BroadcastTextBuilder> localizer(builder);
    BroadcastWorker(localizer);
}

void Battleground::PlaySoundToAll(uint32 soundID)
{
    SendPacketToAll(WorldPackets::Misc::PlaySound(soundID).Write());
}

void Battleground::PlaySoundToTeam(uint32 soundID, uint32 teamID)
{
    for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
    {
        if (Player* player = _GetPlayerForTeam(teamID, itr, "PlaySoundToTeam"))
            player->SendDirectMessage(WorldPackets::Misc::PlaySound(soundID).Write());
    }
}

void Battleground::CastSpellOnTeam(uint32 SpellID, uint32 TeamID)
{
    for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
        if (Player* player = _GetPlayerForTeam(TeamID, itr, "CastSpellOnTeam"))
            player->CastSpell(player, SpellID, true);
}

void Battleground::RemoveAuraOnTeam(uint32 SpellID, uint32 TeamID)
{
    for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
        if (Player* player = _GetPlayerForTeam(TeamID, itr, "RemoveAuraOnTeam"))
            player->RemoveAura(SpellID);
}

// Honor to a team WITHOUT touching the scoreboard - RewardHonorToTeam feeds the
// bonus-honor column, and a payout that is not an objective has no business
// showing up there.
//
// It still pays through RewardHonor rather than ModifyHonorPoints, because that
// is where the realm decides what an honor award becomes: below the level cap it
// is experience instead (Centurion.Battleground.XpInsteadOfHonor), and reaching
// into the purse directly is how a character under the cap ends up holding honor
// nobody meant them to have.
void Battleground::CenturionRewardHonorToTeam(uint32 Honor, uint32 TeamID)
{
    if (m_IsCustomGame)
        return;

    for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
        if (Player* player = _GetPlayerForTeam(TeamID, itr, "CenturionRewardHonorToTeam"))
            player->RewardHonor(nullptr, 1, int32(Honor));
}

// Below the cap a battleground pays its honor as experience, which is a reward
// nobody sees. The spoils chest is the thing you can hold: a chest of the
// bracket's world-drop greens for every PERSON below the cap who was still there
// when the match ended - always for the winners, LoserChancePercent for the rest.
//
// Picked by the character's own level, one chest per ten-level bracket starting
// at FirstItem (10-19). The contents are ordinary item_loot_template rows, so the
// loot is data and changes without a build.
//
// Not paid in arenas, custom games or Violet Hold (whose "enemies" are summoned
// clones), nor to bots, bounty hunters, tournament characters or anyone at the
// cap. A frozen-experience character still gets it: the chest is the reward for
// playing a bracket, not for levelling through it. Bags full -> it is mailed.
void Battleground::AwardSpoilsChest(Player* player, bool won)
{
    if (!sWorld->getBoolConfig(CONFIG_CENTURION_BG_SPOILS_ENABLE))
        return;

    if (m_IsCustomGame || !isBattleground() || GetTypeID(true) == BATTLEGROUND_VHR)
        return;

    WorldSession* session = player->GetSession();
    if (!session || session->IsVirtualSession() || session->IsTransientPlayerSession())
        return;

    if (Tournament::IsTournamentCharacter(player))
        return;

    uint32 const level = player->GetLevel();
    uint32 const cap = player->GetUInt32Value(PLAYER_FIELD_MAX_LEVEL);
    if (level < 10 || level >= cap)
        return;

    // A full run, not the last two minutes of one: whoever arrived late must
    // still have been here for MinPresencePercent of the match.
    BattlegroundPlayerMap::const_iterator const bp = m_Players.find(player->GetGUID());
    if (bp == m_Players.end())
        return;
    uint32 const present = GetStartTime() - std::min(GetStartTime(), bp->second.JoinStartTime);
    if (uint64(present) * 100 < uint64(GetStartTime()) * sWorld->getIntConfig(CONFIG_CENTURION_BG_SPOILS_MIN_PRESENCE))
        return;

    if (!won && !roll_chance_i(int32(sWorld->getIntConfig(CONFIG_CENTURION_BG_SPOILS_LOSER_CHANCE))))
        return;

    uint32 const chestEntry = sWorld->getIntConfig(CONFIG_CENTURION_BG_SPOILS_FIRST_ITEM) + (level / 10 - 1);
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(chestEntry);
    if (!proto)
    {
        TC_LOG_ERROR("bg.battleground", "Battleground::AwardSpoilsChest: spoils chest {} for level {} does not exist", chestEntry, level);
        return;
    }

    ItemPosCountVec dest;
    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, chestEntry, 1) == EQUIP_ERR_OK)
    {
        if (Item* chest = player->StoreNewItem(dest, chestEntry, true))
        {
            // Sent as "created": once the match has a winner, the stock
            // WorldStateFrame CHAT_MSG_LOOT filter drops every loot line but
            // LOOT_ITEM_CREATED_SELF, so "You receive item:" never reaches chat.
            player->SendNewItem(chest, 1, true, true);
            return;
        }
    }

    if (Item* chest = Item::CreateItem(chestEntry, 1, player))
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        chest->SaveToDB(trans);
        MailDraft(proto->Name1, "Your bags were full at the end of the battle, so your spoils were sent here.")
            .AddItem(chest)
            .SendMailTo(trans, player, MailSender(player, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED);
        CharacterDatabase.CommitTransaction(trans);
    }
}

void Battleground::RewardHonorToTeam(uint32 Honor, uint32 TeamID)
{
    if (m_IsCustomGame)
        return;

    for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
        if (Player* player = _GetPlayerForTeam(TeamID, itr, "RewardHonorToTeam"))
            UpdatePlayerScore(player, SCORE_BONUS_HONOR, Honor);
}

void Battleground::RewardReputationToTeam(uint32 faction_id, uint32 Reputation, uint32 TeamID)
{
    if (m_IsCustomGame)
        return;

    FactionEntry const* factionEntry = sFactionStore.LookupEntry(faction_id);
    if (!factionEntry)
        return;

    // switch to the opposite faction
    FactionEntry const* factionEntry2 = sFactionStore.LookupEntry(faction_id);
    switch (faction_id)
    {
        case 729:
            factionEntry2 = sFactionStore.LookupEntry(730);
            break;
        case 730:
            factionEntry2 = sFactionStore.LookupEntry(729);
            break;
        case 889:
            factionEntry2 = sFactionStore.LookupEntry(890);
            break;
        case 890:
            factionEntry2 = sFactionStore.LookupEntry(889);
            break;
        case 509:
            factionEntry2 = sFactionStore.LookupEntry(510);
            break;
        case 510:
            factionEntry2 = sFactionStore.LookupEntry(509);
            break;
        default:
            break;
    }

    if (!factionEntry2)
        return;

    for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
    {
        Player* player = _GetPlayerForTeam(TeamID, itr, "RewardReputationToTeam");
        if (!player)
            continue;

        uint32 repGain = Reputation;
        AddPct(repGain, player->GetTotalAuraModifier(SPELL_AURA_MOD_REPUTATION_GAIN));
        AddPct(repGain, player->GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_FACTION_REPUTATION_GAIN, faction_id));
        player->GetReputationMgr().ModifyReputation(factionEntry, repGain);
    }
}

void Battleground::UpdateWorldState(uint32 variable, uint32 value)
{
    WorldPackets::WorldState::UpdateWorldState worldstate;
    worldstate.VariableID = variable;
    worldstate.Value = value;
    SendPacketToAll(worldstate.Write());
}

void Battleground::EndBattleground(uint32 winner)
{
    RemoveFromBGFreeSlotQueue();

    if (winner == ALLIANCE)
    {
        if (isBattleground())
            SendBroadcastText(BG_TEXT_ALLIANCE_WINS, CHAT_MSG_BG_SYSTEM_NEUTRAL);

        PlaySoundToAll(SOUND_ALLIANCE_WINS);                // alliance wins sound

        SetWinner(PVP_TEAM_ALLIANCE);
    }
    else if (winner == HORDE)
    {
        if (isBattleground())
            SendBroadcastText(BG_TEXT_HORDE_WINS, CHAT_MSG_BG_SYSTEM_NEUTRAL);

        PlaySoundToAll(SOUND_HORDE_WINS);                   // horde wins sound

        SetWinner(PVP_TEAM_HORDE);
    }
    else
    {
        SetWinner(PVP_TEAM_NEUTRAL);
    }

    CharacterDatabasePreparedStatement* stmt = nullptr;
    uint64 battlegroundId = 1;
    if (!m_IsCustomGame && isBattleground() && sWorld->getBoolConfig(CONFIG_BATTLEGROUND_STORE_STATISTICS_ENABLE))
    {
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_PVPSTATS_MAXID);
        PreparedQueryResult result = CharacterDatabase.Query(stmt);

        if (result)
        {
            Field* fields = result->Fetch();
            battlegroundId = fields[0].GetUInt64() + 1;
        }

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_PVPSTATS_BATTLEGROUND);
        stmt->setUInt64(0, battlegroundId);
        stmt->setUInt8(1, GetWinner());
        stmt->setUInt8(2, GetUniqueBracketId());
        stmt->setUInt8(3, GetTypeID(true));
        CharacterDatabase.Execute(stmt);
    }

    SetStatus(STATUS_WAIT_LEAVE);
    //we must set it this way, because end time is sent in packet!
    m_EndTime = TIME_TO_AUTOREMOVE;

    WorldPacket pvpLogData;
    BuildPvPLogDataPacket(pvpLogData);

    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(GetTypeID(), GetArenaType());

    // Only a match that has run long enough pays out (the gate in the loop
    // below), and only such a match is recorded. Its end-of-match honor is
    // what evens its kind out to the realm's honor per hour, from the medians
    // of the matches before it (BattlegroundMgr::GetHourlyHonorPayout).
    bool const paysOut = GetStartDelayTime() <= 0 && GetStartTime() >= 15 * IN_MILLISECONDS;
    bool const recordedMatch = paysOut && !m_IsCustomGame && !IsReplay() && PaysHourlyHonor();
    BattlegroundMgr::HourlyHonorPayout hourlyPayout{};
    bool const hourlyHonor = recordedMatch && sBattlegroundMgr->GetHourlyHonorPayout(this, hourlyPayout);
    uint32 recordedPlayers = 0;
    uint32 recordedHumans = 0;
    std::vector<uint32> winnersMatchHonor;
    std::vector<uint32> losersMatchHonor;

    for (BattlegroundPlayerMap::iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
    {
        uint32 team = itr->second.Team;

        Player* player = _GetPlayer(itr, "EndBattleground");
        if (!player)
            continue;

        ++recordedPlayers;
        if (WorldSession const* session = player->GetSession())
            if (!session->IsVirtualSession() && !session->IsTransientPlayerSession())
                ++recordedHumans;

        // In a squad match only one squad of the winning side won.
        bool const won = IsWinningParticipant(itr->first, team, winner);

        // should remove spirit of redemption
        if (player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
            player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

        if (!player->IsAlive())
        {
            player->ResurrectPlayer(1.0f);
            player->SpawnCorpseBones();
        }
        else
            //needed cause else in av some creatures will kill the players at the end
            player->CombatStop();

        uint32 winner_honor = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_WINNER);
        uint32 loser_honor = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_LOSER);
        uint32 winner_money = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_MONEY_WINNER);
        uint32 loser_money = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_MONEY_LOSER);
        uint32 winner_arena = player->GetRandomWinner() ? sWorld->getIntConfig(CONFIG_BG_REWARD_WINNER_ARENA_LAST) : sWorld->getIntConfig(CONFIG_BG_REWARD_WINNER_ARENA_FIRST);

        bool const transientPlayer = player->GetSession() && player->GetSession()->IsTransientPlayerSession();
        if (!m_IsCustomGame && !transientPlayer && isBattleground() && sWorld->getBoolConfig(CONFIG_BATTLEGROUND_STORE_STATISTICS_ENABLE))
        {
            BattlegroundScoreMap::const_iterator score = PlayerScores.find(player->GetGUID().GetCounter());
            if (score == PlayerScores.end())
            {
                TC_LOG_ERROR("bg.battleground", "Battleground::EndBattleground: Player {} is missing scoreboard data, skipping statistics save.", player->GetGUID().ToString());
            }
            else
            {
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_PVPSTATS_PLAYER);

                stmt->setUInt32(0,  battlegroundId);
                stmt->setUInt32(1,  player->GetGUID().GetCounter());
                stmt->setBool  (2,  won);
                stmt->setUInt32(3,  score->second->GetKillingBlows());
                stmt->setUInt32(4,  score->second->GetDeaths());
                stmt->setUInt32(5,  score->second->GetHonorableKills());
                stmt->setUInt32(6,  score->second->GetBonusHonor());
                stmt->setUInt32(7,  score->second->GetDamageDone());
                stmt->setUInt32(8,  score->second->GetHealingDone());
                stmt->setUInt32(9,  score->second->GetAttr1());
                stmt->setUInt32(10, score->second->GetAttr2());
                stmt->setUInt32(11, score->second->GetAttr3());
                stmt->setUInt32(12, score->second->GetAttr4());
                stmt->setUInt32(13, score->second->GetAttr5());

                CharacterDatabase.Execute(stmt);
            }
        }

        // if we're arena
        if (!isBattleground())
        {
            float arenaMultiplier = sWorld->getFloatConfig(CONFIG_CENTURION_BG_ARENA_REWARD_MULTIPLIER);
            int flag_honor = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_FLAG_CAP) * 3;
            winner_honor += flag_honor;
            winner_honor *= arenaMultiplier;
            loser_honor *= arenaMultiplier;
            winner_money *= arenaMultiplier;
            loser_money *= arenaMultiplier;
        }

        // The hourly payout replaces the flat one outright, arenas included:
        // it already answers for their length (and ArenaEffort for the rest).
        if (hourlyHonor)
        {
            winner_honor = hourlyPayout.Winner;
            loser_honor = hourlyPayout.Loser;
        }
        else if (float const flat = FlatHonorMultiplier(); flat != 1.0f)
        {
            winner_honor = uint32(std::lround(float(winner_honor) * flat));
            loser_honor = uint32(std::lround(float(loser_honor) * flat));
        }

        // Honor earned in play. The payout below is not part of it: the match
        // has already left STATUS_IN_PROGRESS, so NoteMatchHonor ignores it.
        (won ? winnersMatchHonor : losersMatchHonor).push_back(GetMatchHonor(player->GetGUID()));

        ModifyEndOfMatchHonorRewards(winner, team, winner_honor, loser_honor);
        ModifyEndOfMatchMoneyRewards(winner, team, winner_money, loser_money);

        // Rewards
        // only grant rewards if battle has lasted 15 seconds
        if (paysOut)
        {
            if (won)
            {
                // Private custom games retain the normal gold payout, but
                // never grant progression rewards that can be farmed.
                if (!m_IsCustomGame)
                {
                    player->RewardHonor(nullptr, 1, winner_honor);
                    if (CanAwardArenaPoints())
                        player->ModifyArenaPoints(winner_arena);
                    if (!player->GetRandomWinner())
                        player->SetRandomWinner(true);

                    player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_BG, player->GetMapId());

                    // Every arena and battleground win restores one depleted
                    // mark. This used to be an allow-list (arenas, WSG, the
                    // custom maps), and every battleground added after it -
                    // Arathi Basin, Battle for Gilneas, Twin Peaks - restored
                    // nothing while the player kept the depleted mark.
                    //
                    // Violet Hold survival is explicitly barred from restoring
                    // marks: its enemy team is summoned clones, so a "win"
                    // requires no opposing players and must not feed the mark
                    // economy.
                    bool const canRestoreMark = GetTypeID(true) != BATTLEGROUND_VHR;
                    if (canRestoreMark && Trinity::Custom::ConsumeEligibleDepletedMarks(player, 1))
                    {
                        // The depleted mark is already gone, so a full bag (or
                        // the restored mark's carry limit) must not eat the
                        // reward - the Postmaster holds it instead.
                        uint32 const markEntry = Trinity::Custom::GetRestoredMarkEntry();
                        ItemPosCountVec dest;
                        Item* mark = nullptr;
                        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, markEntry, 1) == EQUIP_ERR_OK)
                            mark = player->StoreNewItem(dest, markEntry, true);

                        // "Created" for the same reason as the spoils chest:
                        // the end-of-match loot filter drops a plain receive
                        // line, so the restore used to happen silently.
                        if (mark)
                            player->SendNewItem(mark, 1, true, true);
                        else
                            player->SendItemRetrievalMail(markEntry, 1);
                    }

                    AwardSpoilsChest(player, true);
                }
                player->ModifyMoney(winner_money);
                if (winner_money)
                {
                    // ModifyMoney updates the purse but does not produce the
                    // loot-style chat notification.  Send the same packet
                    // used when a player loots gold so BG/arena payouts are
                    // visible instead of appearing silently.
                    WorldPacket moneyNotify(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                    moneyNotify << uint32(winner_money);
                    moneyNotify << uint8(1); // "You loot..."
                    player->SendDirectMessage(&moneyNotify);
                }
            }
            else
            {
                if (!m_IsCustomGame)
                {
                    player->RewardHonor(nullptr, 1, loser_honor);
                    AwardSpoilsChest(player, false);
                }
                player->ModifyMoney(loser_money);
                if (loser_money)
                {
                    WorldPacket moneyNotify(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                    moneyNotify << uint32(loser_money);
                    moneyNotify << uint8(1); // "You loot..."
                    player->SendDirectMessage(&moneyNotify);
                }
            }
        }
        player->ResetAllPowers();
        player->CombatStopWithPets(true);

        BlockMovement(player);

        if (IsSquadMatch())
        {
            WorldPacket squadLogData;
            BuildPvPLogDataPacket(squadLogData, player);
            player->SendDirectMessage(&squadLogData);
        }
        else
            player->SendDirectMessage(&pvpLogData);

        WorldPacket data;
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, this, player->GetBattlegroundQueueIndex(bgQueueTypeId), STATUS_IN_PROGRESS, TIME_TO_AUTOREMOVE, GetStartTime(), GetArenaType(), player->GetBGTeam());
        player->SendDirectMessage(&data);
        if (!m_IsCustomGame)
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_COMPLETE_BATTLEGROUND, player->GetMapId());
        sScriptMgr->OnBattlegroundEnd(this, winner);
    }

    // Start time runs from the gates' countdown, so the preparation is part of
    // the length: it is part of what a player spends on the match.
    if (recordedMatch)
    {
        auto medianOf = [](std::vector<uint32>& values) -> int32
        {
            if (values.empty())
                return -1;
            std::sort(values.begin(), values.end());
            size_t const middle = values.size() / 2;
            return int32(values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2);
        };

        uint32 const configWinner = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_WINNER);
        uint32 const configLoser = sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_LOSER);
        sBattlegroundMgr->RecordMatchDuration(this, GetStartTime() / IN_MILLISECONDS, recordedHumans, recordedPlayers,
            medianOf(winnersMatchHonor), medianOf(losersMatchHonor),
            hourlyHonor ? hourlyPayout.Winner : configWinner, hourlyHonor ? hourlyPayout.Loser : configLoser);
    }

    // Custom-game spectators live outside m_Players, so the loop above never
    // hands them the final scoreboard. Send it so their client pops the
    // end-of-match score frame (the winner byte in the log packet drives it)
    // just like it does for combatants. Arenas are excluded: see the isArena()
    // guard in SendSpectatorBattlefieldStatusTo for why an ACTIVE-arena status
    // packet breaks spectator nameplate coloring.
    if (m_IsCustomGame && !isArena())
        for (Player* spectator : m_Spectators)
        {
            if (!spectator || !spectator->IsInWorld() || spectator->GetBattlegroundId() != GetInstanceID())
                continue;
            if (!spectator->GetSession() || spectator->GetSession()->IsVirtualSession())
                continue;

            spectator->SendDirectMessage(&pvpLogData);

            WorldPacket data;
            sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, this, 0, STATUS_IN_PROGRESS, TIME_TO_AUTOREMOVE, GetStartTime(), GetArenaType(), spectator->GetBGTeam());
            spectator->SendDirectMessage(&data);
        }
}

uint32 Battleground::GetBonusHonorFromKill(uint32 kills) const
{
    //variable kills means how many honorable kills you scored (so we need kills * honor_for_one_kill)
    uint32 maxLevel = std::min<uint32>(GetMaxLevel(), 80U);
    return Trinity::Honor::hk_honor_at_level(maxLevel, float(kills));
}

void Battleground::BlockMovement(Player* player)
{
    // movement disabled NOTE: the effect will be automatically removed by client when the player is teleported from the battleground, so no need to send with uint8(1) in RemovePlayerAtLeave()
    player->SetClientControl(player, false);
}

// Crowd control does not follow you out of the gate.
//
// Nothing was taking it off. RemoveArenaAuras is keyed on the DESTINATION map,
// so it runs on the way IN, and it only strips POSITIVE auras - a Blind or a
// Fear is neither. So the last second of a match could put a player into the
// world, or back into the custom lobby, still under someone else's control on a
// map that person is no longer on.
//
// That was not merely untidy. A confuse installs a movement generator which
// caches a PathGenerator, and a teleport does not clear it, so the player
// carried a pointer to the arena's navmesh out with him and the arena's
// teardown then freed it. PathGenerator re-resolves across a map change now, so
// the dangling pointer can no longer be dereferenced - but the effect should
// never have crossed the gate at all, which is what this fixes.
//
// Removed by AURA TYPE rather than by mechanic on purpose: the aura type is what
// installs the movement generator, while Mechanic is spell data this realm
// rebuilds and can be wrong. Blind is Mechanic 2, but what matters about it is
// its second effect, SPELL_AURA_MOD_CONFUSE.
void Battleground::ClearCombatControl(Player* player)
{
    if (!player)
        return;

    player->RemoveAurasByType(SPELL_AURA_MOD_CONFUSE);
    player->RemoveAurasByType(SPELL_AURA_MOD_FEAR);
    player->RemoveAurasByType(SPELL_AURA_MOD_STUN);
    player->RemoveAurasByType(SPELL_AURA_MOD_ROOT);
    player->RemoveAurasByType(SPELL_AURA_MOD_CHARM);
    player->RemoveAurasByType(SPELL_AURA_MOD_POSSESS);
    player->RemoveAurasByType(SPELL_AURA_MOD_PACIFY);
    player->RemoveAurasByType(SPELL_AURA_MOD_SILENCE);
    player->RemoveAurasByType(SPELL_AURA_MOD_PACIFY_SILENCE);

    // Then tear the states down explicitly. An aura is not the only way into one
    // of these, the movement generator is what actually holds the stale path,
    // and SetControlled re-checks for a remaining aura of that kind before it
    // clears - so with the auras gone above, this is the part that guarantees
    // nothing is left running.
    player->SetControlled(false, UNIT_STATE_CONFUSED);
    player->SetControlled(false, UNIT_STATE_FLEEING);
    player->SetControlled(false, UNIT_STATE_STUNNED);
    player->SetControlled(false, UNIT_STATE_ROOT);
}

void Battleground::RemovePlayerAtLeave(ObjectGuid guid, bool Transport, bool SendPacket)
{
    uint32 team = GetPlayerTeam(guid);
    bool participant = false;
    // Remove from lists/maps
    BattlegroundPlayerMap::iterator itr = m_Players.find(guid);
    if (itr != m_Players.end())
    {
        UpdatePlayersCountByTeam(team, true);               // -1 player
        m_Players.erase(itr);
        // check if the player was a participant of the match, or only entered through gm command (goname)
        participant = true;
    }

    BattlegroundScoreMap::iterator itr2 = PlayerScores.find(guid.GetCounter());
    if (itr2 != PlayerScores.end())
    {
        delete itr2->second;                                // delete player's score
        PlayerScores.erase(itr2);
    }

    RemovePlayerFromResurrectQueue(guid);

    Player* player = ObjectAccessor::FindPlayer(guid);
    bool const removedNonVirtualHuman = player && player->GetSession() && !IsBotParticipantSession(player->GetSession());

    if (player)
    {
        // Alive before the gear goes back on: Player::EquipItem gives a dead
        // character an item but not its stats, while taking the loadout off
        // takes its stats away all the same - so leaving a match dead left the
        // character short its own gear's stats until it relogged, and the next
        // match's loadout took them off a second time (Stonedsoul fought a
        // shuffle at 2k health). Spirit of Redemption is a body that is not
        // dead yet; it goes first, as below.
        if (player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
            player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        if (!player->IsAlive())
        {
            player->ResurrectPlayer(1.0f);
            player->SpawnCorpseBones();
        }

        // A Summoner's Rift character gets its real level (and talents, money)
        // back first: its own gear needs that level to be worn again.
        RiftProgression::Leave(player);

        // Its own gear back, before anything else undoes the join. A character
        // that is not here to be handed it - logged out, disconnected, cut off
        // by a crash - is given it at its next login instead.
        Tournament::RestoreBattlegroundLoadout(player);

        // And the cooldowns it walked in with, on top of whatever the match
        // itself left it holding (Miscellaneous/CooldownStash.h). Same rule for
        // a character that is not here to be handed them: its next login.
        CooldownStash::RestoreAfterMatch(player);

        SetBattlegroundSideReactions(player, 0);

        RemoveSpectator(player);

        player->RemoveAurasByType(SPELL_AURA_MOUNTED);

        ClearCombatControl(player);
    }
    else
    {
        CharacterDatabaseTransaction trans(nullptr);
        Player::OfflineResurrect(guid, trans);
    }

    RemovePlayer(player, guid, team);                           // BG subclass specific code

    if (participant) // if the player was a match participant, remove auras, calc rating, update queue
    {
        BattlegroundTypeId bgTypeId = GetTypeID();
        BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(GetTypeID(), GetArenaType());
        if (player)
        {
            player->ClearAfkReports();

            // if arena, remove the specific arena auras
            if (isArena())
            {
                bgTypeId = BATTLEGROUND_AA;                   // set the bg type to all arenas (it will be used for queue refreshing)

                // unsummon current and summon old pet if there was one and there isn't a current pet
                player->RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT);
                player->ResummonPetTemporaryUnSummonedIfAny();
            }

            if (SendPacket)
            {
                WorldPacket data;
                sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, this, player->GetBattlegroundQueueIndex(bgQueueTypeId), STATUS_NONE, 0, 0, 0, 0);
                player->SendDirectMessage(&data);
            }

            // this call is important, because player, when joins to battleground, this method is not called, so it must be called when leaving bg
            player->RemoveBattlegroundQueueId(bgQueueTypeId);
        }

        // remove from raid group if player is member
        uint8 const raidSlot = GetBgRaidSlot(guid, team);
        if (Group* group = GetBgRaidBySlot(raidSlot))
        {
            if (!group->RemoveMember(guid))                // group was disbanded
                SetBgRaidBySlot(raidSlot, nullptr);
        }
        DecreaseInvitedCount(team);
        //we should update battleground queue, but only if bg isn't ending
        if (isBattleground() && GetStatus() < STATUS_WAIT_LEAVE)
        {
            // a player has left the battleground, so there are free slots -> add to queue
            AddToBGFreeSlotQueue();
            sBattlegroundMgr->ScheduleQueueUpdate(0, 0, bgQueueTypeId, bgTypeId, GetBracketId());
        }
        // Let others know
        WorldPacket data;
        sBattlegroundMgr->BuildPlayerLeftBattlegroundPacket(&data, guid);
        SendPacketToTeam(team, &data, player, false);

        if (isBattleground() && removedNonVirtualHuman && m_HasEverHadNonVirtualHumanParticipant &&
            (GetStatus() == STATUS_IN_PROGRESS || GetStatus() == STATUS_WAIT_JOIN) &&
            !HasAnyNonVirtualHumanParticipant(this))
        {
            TC_LOG_DEBUG("bg.battleground",
                "Battleground::RemovePlayerAtLeave forced end: map={} instance={} no non-virtual participants or custom-game spectators remain.",
                GetMapId(), GetInstanceID());
            EndBattleground(PVP_TEAM_NEUTRAL);
        }
        // An arena left to bots is torn down on the spot, gates shut or open.
        // Kept alive, a bot-filled one still in preparation is offered straight
        // back to whoever walked out of it (BattlegroundQueue::OfferPreparationSeats),
        // so queueing again lands them in the same map and line-up they just
        // left. Not while somebody else is still on the way in to a seat.
        else if (isArena() && !isRated() && removedNonVirtualHuman && m_HasEverHadNonVirtualHumanParticipant &&
            (GetStatus() == STATUS_IN_PROGRESS || GetStatus() == STATUS_WAIT_JOIN) &&
            !HasAnyNonVirtualHumanParticipant(this))
        {
            PrunePreparationSeatTakers();
            if (m_PreparationSeatTakers.empty())
            {
                TC_LOG_DEBUG("bg.battleground",
                    "Battleground::RemovePlayerAtLeave closing arena: map={} instance={} has only bots left.",
                    GetMapId(), GetInstanceID());
                EndNow();
            }
        }
        // A rated arena padded with bots is its people's to play. When the last
        // of them is gone from a side with an arena team - walked out, or dropped
        // and not back before the offline timer gave up - that team forfeits,
        // instead of its clones winning or losing its rating for it. RemovePlayer
        // above may already have ended the match (the side had nobody alive).
        else if (isArena() && isRated() && IsBotFillMatch() && GetArenaTeamIdForTeam(team) &&
            (GetStatus() == STATUS_IN_PROGRESS || GetStatus() == STATUS_WAIT_JOIN) &&
            !HasPersonOnTeam(team))
        {
            TC_LOG_DEBUG("bg.battleground",
                "Battleground::RemovePlayerAtLeave: map={} instance={} - arena team {} has nobody left; it forfeits.",
                GetMapId(), GetInstanceID(), GetArenaTeamIdForTeam(team));
            EndBattleground(GetOtherTeam(team));
        }
    }

    // After everything above that reads the squad: the raid it played in, and
    // RemovePlayer's last-squad-standing check, which must still count this
    // player's squad as one that took part.
    ReleaseSquadSeat(guid);

    if (player)
    {
        // Do next only if found in battleground
        player->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE);  // We're not in BG.
        // reset destination bg team
        player->SetBGTeam(0);

        if (HasCustomWeatherOverride())
            Weather::SendFineWeatherUpdateToPlayer(player);

        // remove all criterias on bg leave
        player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_BG_MAP, GetMapId(), true);

        if (Transport)
            player->TeleportToBGEntryPoint();

        TC_LOG_DEBUG("bg.battleground", "Removed player {} from Battleground.", player->GetName());
    }

    //battleground object will be deleted next Battleground::Update() call
}

// this method is called when no players remains in battleground
void Battleground::Reset()
{
    SetWinner(PVP_TEAM_NEUTRAL);
    SetStatus(STATUS_WAIT_QUEUE);
    SetStartTime(0);
    SetEndTime(0);
    SetLastResurrectTime(0);
    m_Events = 0;
    m_MatchHonor.clear();

    if (m_InvitedAlliance > 0 || m_InvitedHorde > 0)
        TC_LOG_ERROR("bg.battleground", "Battleground::Reset: one of the counters is not 0 (alliance: {}, horde: {}) for BG (map: {}, instance id: {})!",
            m_InvitedAlliance, m_InvitedHorde, m_MapId, m_InstanceID);

    m_InvitedAlliance = 0;
    m_InvitedHorde = 0;
    m_InBGFreeSlotQueue = false;
    m_HasEverHadNonVirtualHumanParticipant = false;
    m_NoNonVirtualHumanElapsed = 0;
    m_BotFillOpponentsFielded = false;
    m_PreparationSeatTakers.clear();
    m_SquadSeats.clear();
    m_WinnerSquad = BATTLEGROUND_NO_SQUAD;
    for (uint8 squad = 0; squad < MAX_BATTLEGROUND_SQUADS; ++squad)
    {
        m_SquadArenaTeamIds[squad] = 0;
        m_SquadArenaMMR[squad] = 0;
        m_SquadScoreboardRival[squad] = BATTLEGROUND_NO_SQUAD;
        _squadArenaScores[squad].Reset();
    }

    m_Players.clear();

    for (BattlegroundScoreMap::const_iterator itr = PlayerScores.begin(); itr != PlayerScores.end(); ++itr)
        delete itr->second;
    PlayerScores.clear();

    for (uint8 i = 0; i < PVP_TEAMS_COUNT; ++i)
        _arenaTeamScores[i].Reset();

    ResetBGSubclass();
}


void Battleground::StartBattleground()
{
    SetStartTime(0);
    SetLastResurrectTime(0);
    // add BG to free slot queue
    if (!m_IsCustomGame)
        AddToBGFreeSlotQueue();

    // add bg to update list
    // This must be done here, because we need to have already invited some players when first BG::Update() method is executed
    // and it doesn't matter if we call StartBattleground() more times, because m_Battlegrounds is a map and instance id never changes
    sBattlegroundMgr->AddBattleground(this);

    if (m_IsRated)
        TC_LOG_DEBUG("bg.arena", "Arena match type: {} for Team1Id: {} - Team2Id: {} started.", m_ArenaType, m_ArenaTeamIds[TEAM_ALLIANCE], m_ArenaTeamIds[TEAM_HORDE]);
}

bool Battleground::SkipStartDelay()
{
    if (GetStatus() != STATUS_WAIT_JOIN || (m_Events & BG_STARTING_EVENT_4))
        return false;

    // Ensure setup is complete if this is forced very early, before the first regular countdown update.
    if (!(m_Events & BG_STARTING_EVENT_1))
    {
        m_Events |= BG_STARTING_EVENT_1;

        if (!PrepareStartingArea())
            return false;
    }

    // Skip remaining countdown warnings.
    m_Events |= BG_STARTING_EVENT_2;
    m_Events |= BG_STARTING_EVENT_3;
    m_Events |= BG_STARTING_EVENT_4;

    StartingEventOpenDoors();

    if (StartMessageIds[BG_STARTING_EVENT_FOURTH])
        SendBroadcastText(StartMessageIds[BG_STARTING_EVENT_FOURTH], CHAT_MSG_BG_SYSTEM_NEUTRAL);
    SetStatus(STATUS_IN_PROGRESS);
    SetStartDelayTime(StartDelayTimes[BG_STARTING_EVENT_FOURTH]);

    // Remove preparation
    if (isArena())
    {
        /// @todo add arena sound PlaySoundToAll(SOUND_ARENA_START);
        for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
            if (Player* player = ObjectAccessor::FindPlayer(itr->first))
            {
                // BG Status packet
                WorldPacket status;
                BattlegroundQueueTypeId bgQueueTypeId = sBattlegroundMgr->BGQueueTypeId(m_TypeID, GetArenaType());
                uint32 queueSlot = player->GetBattlegroundQueueIndex(bgQueueTypeId);
                sBattlegroundMgr->BuildBattlegroundStatusPacket(&status, this, queueSlot, STATUS_IN_PROGRESS, 0, GetStartTime(), GetArenaType(), player->GetBGTeam());
                player->SendDirectMessage(&status);

                ClearArenaPreparation(player);
            }

        CheckWinConditions();
    }
    else
    {
        PlaySoundToAll(SOUND_BG_START);

        for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
            if (Player* player = ObjectAccessor::FindPlayer(itr->first))
            {
                player->RemoveAurasDueToSpell(SPELL_PREPARATION);
                player->RemoveAurasDueToSpell(SPELL_INSTANT_CAST);
                player->ResetAllPowers();
            }
        // Announce BG starting
        if (sWorld->getBoolConfig(CONFIG_BATTLEGROUND_QUEUE_ANNOUNCER_ENABLE))
            sWorld->SendWorldText(LANG_BG_STARTED_ANNOUNCE_WORLD, GetName().c_str(), GetMinLevel(), GetMaxLevel());
    }

    return true;
}

void Battleground::ClearArenaPreparation(Player* player)
{
    player->RemoveAurasDueToSpell(SPELL_ARENA_PREPARATION);
    player->RemoveAurasDueToSpell(SPELL_INSTANT_CAST);
    player->ResetAllPowers();
    if (player->IsGameMaster())
        return;

    // remove auras with duration lower than 30s
    player->RemoveAppliedAuras([](AuraApplication const* aurApp)
    {
        Aura* aura = aurApp->GetBase();
        return !aura->IsPermanent()
            && aura->GetDuration() <= 30 * IN_MILLISECONDS
            && aurApp->IsPositive()
            && !aura->GetSpellInfo()->HasAttribute(SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY)
            && !aura->HasEffectType(SPELL_AURA_MOD_INVISIBILITY);
    });
}

void Battleground::SendInProgressStatusTo(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    WorldPacket status;
    uint32 const queueSlot = player->GetBattlegroundQueueIndex(BattlegroundMgr::BGQueueTypeId(m_TypeID, GetArenaType()));
    sBattlegroundMgr->BuildBattlegroundStatusPacket(&status, this, queueSlot, STATUS_IN_PROGRESS, 0, GetStartTime(), GetArenaType(), player->GetBGTeam());
    player->SendDirectMessage(&status);
}

void Battleground::MoveParticipantToTeam(ObjectGuid guid, Player* player, uint32 team)
{
    BattlegroundPlayerMap::iterator const itr = m_Players.find(guid);
    if (itr == m_Players.end() || (team != ALLIANCE && team != HORDE))
        return;

    uint32 const oldTeam = itr->second.Team;
    if (oldTeam != team)
    {
        // Out of the old side's raid first: AddOrSetPlayerToCorrectBgGroup
        // keeps whatever raid a player is already in.
        uint8 const oldSlot = GetBgRaidSlot(guid, oldTeam);
        if (Group* group = GetBgRaidBySlot(oldSlot))
            if (group->IsMember(guid) && !group->RemoveMember(guid))   // group was disbanded
                SetBgRaidBySlot(oldSlot, nullptr);

        itr->second.Team = team;
        UpdatePlayersCountByTeam(oldTeam, true);
        UpdatePlayersCountByTeam(team, false);
    }

    BattlegroundScoreMap::iterator const score = PlayerScores.find(guid.GetCounter());
    if (score != PlayerScores.end())
        if (ArenaScore* arenaScore = dynamic_cast<ArenaScore*>(score->second))
            arenaScore->TeamId = team == ALLIANCE ? PVP_TEAM_ALLIANCE : PVP_TEAM_HORDE;

    if (!player)
        return;

    // What the port handler and AddPlayer give someone arriving on this side:
    // the arena byte, the side's race faction, and its reputation reactions.
    player->SetBGTeam(team);
    player->SetFactionForRace(team == HORDE ? RACE_BLOODELF : RACE_HUMAN);
    SetBattlegroundSideReactions(player, team);

    // A pet or totem took its owner's faction when it was summoned.
    std::vector<Unit*> const controlled(player->m_Controlled.begin(), player->m_Controlled.end());
    for (Unit* unit : controlled)
        if (unit && unit->GetTypeId() != TYPEID_PLAYER)
            unit->SetFaction(player->GetFaction());

    // Into the new side's raid - only when the side changed: someone staying
    // put is already in the right one.
    if (oldTeam != team)
        AddOrSetPlayerToCorrectBgGroup(player, team);
}

void Battleground::ResetStartingAreaForRound()
{
    StartingEventCloseDoors();

    m_ValidStartPositionTimer = 0;
    m_BoundsElapsed = 0;
    m_BoundsCheckTimer = 0;
    m_StartExitElapsed = 0;
    for (uint8 i = 0; i < PVP_TEAMS_COUNT; ++i)
        m_StartExitDone[i] = false;
}

void Battleground::KeepPlayersAtStarts(uint32 diff)
{
    _CheckSafePositions(diff);
}

void Battleground::AddPlayer(Player* player)
{
    if (m_IsCustomGame && player->IsSpectator())
    {
        // Keep every custom-game spectator on one deterministic viewing side.
        // This is not participation: spectators remain in m_Spectators only.
        player->SetBGTeam(ALLIANCE);
        player->SetFactionForRace(RACE_HUMAN);
        AddSpectator(player);
        player->SendInitWorldStates(player->GetZoneId(), player->GetAreaId());
        SendSpectatorBattlefieldStatusTo(player);
        return;
    }

    // remove afk from player
    if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_AFK))
        player->ToggleAFK();

    uint32 team = player->GetBGTeam();

    // Somebody coming back to a shuffle may have had their seat moved to the
    // other side while they were away (MoveParticipantToTeam): the seat, not
    // the team they left on, is where they play.
    if (IsSoloShuffle())
    {
        BattlegroundPlayerMap::const_iterator const seat = m_Players.find(player->GetGUID());
        if (seat != m_Players.end() && seat->second.Team && seat->second.Team != team)
        {
            team = seat->second.Team;
            player->SetBGTeam(team);
        }
    }

    player->SetFactionForRace(RACE_HUMAN);

    if (team == HORDE)
    {
        player->SetFactionForRace(RACE_BLOODELF);
    }

    SetBattlegroundSideReactions(player, team);

    // Battleground participation itself is authoritative PvP state. Do not
    // rely on a later zone update to force the flag: transient playerbots and
    // custom-game clones are inserted directly into the map, and a stale PvP
    // end timer can otherwise turn their overhead names blue mid-match.
    player->UpdatePvP(true, true);

    // A player can enter carrying the open-world FFA byte from the area they
    // came from.  Battleground teams (including custom arenas) are not FFA:
    // the client must use the battleground team/raid relationship, and party
    // aura target scans must not inherit the stale FFA state.  OBC clones are
    // created in the arena with a clean byte, which made real players and
    // clones disagree: the real player rendered dark-blue/hostile and party
    // aura behavior diverged even though both were in the same BG raid.
    if (player->IsFFAPvP() || player->pvpInfo.IsInFFAPvPArea || player->pvpInfo.IsInFFAPvPAreaByMap)
    {
        player->pvpInfo.IsInFFAPvPArea = false;
        player->pvpInfo.IsInFFAPvPAreaByMap = false;
        player->RemovePvpFlag(UNIT_BYTE2_FLAG_FFA_PVP);
        player->ForceValuesUpdateAtIndex(UNIT_FIELD_BYTES_2);
        player->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
    }

    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());

    BattlegroundPlayer bp;
    bp.OfflineRemoveTime = 0;
    bp.Team = team;
    // A reconnect keeps the original arrival, so it cannot shorten a stay.
    bp.JoinStartTime = isInBattleground ? m_Players[player->GetGUID()].JoinStartTime : GetStartTime();

    // Add to list/maps
    m_Players[player->GetGUID()] = bp;

    // The first clone seated opposite the team of a rated bot match is what
    // makes it a contest (Arena::EndBotFilledRatedMatch).
    if (uint32 const ratedTeam = GetBotFillRatedTeam(); ratedTeam && team != ratedTeam && IsBotParticipantSession(player->GetSession()))
        m_BotFillOpponentsFielded = true;

    if (IsSquadMatch())
    {
        // The queue seated people when it invited them; anyone who came in
        // another way - a clone placed by its own code, a GM - is seated now.
        ClaimSquadSeat(player->GetGUID(), team, m_SquadHealersWanted && Arena::IsSoloShuffleHealer(player));

        // Faction and arena byte are sent per viewer in a squad match (see
        // BattlegroundSquadStart), and nothing in them changed just because a
        // squad was given out. Resend both ways, so the newcomer and everyone
        // already here see each other as the right squad whichever of them the
        // other's client met first.
        for (auto const& [otherGuid, otherPlayer] : m_Players)
        {
            (void)otherPlayer;
            if (Player* other = ObjectAccessor::FindPlayer(otherGuid))
            {
                other->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
                other->ForceValuesUpdateAtIndex(PLAYER_BYTES_3);
            }
        }
    }

    if (!isInBattleground)
        UpdatePlayersCountByTeam(team, false);                  // +1 player

    // The invited counters represent pending queue invites. Once the player
    // actually joins the battleground, consume one invite slot so refill logic
    // sees the true remaining capacity.
    if (!isInBattleground)
        DecreaseInvitedCount(team);

    // Before the raid is touched: the clone has to leave the team's group to
    // make room in it.
    if (!isInBattleground)
        HandOverPreparationSeat(player, team);

    if (WorldSession const* session = player->GetSession(); session && !IsBotParticipantSession(session))
    {
        m_HasEverHadNonVirtualHumanParticipant = true;
        m_NoNonVirtualHumanElapsed = 0;
        SendCustomGameRulesTo(player);
    }

    WorldPacket data;
    sBattlegroundMgr->BuildPlayerJoinedBattlegroundPacket(&data, player);
    SendPacketToTeam(team, &data, player, false);

    player->RemoveAurasByType(SPELL_AURA_MOUNTED);

    // add arena specific auras
    player->RemoveArenaEnchantments(TEMP_ENCHANTMENT_SLOT);
    player->DestroyConjuredItems(true);
    player->UnsummonPetTemporaryIfAny();

    if ( GetStatus() == STATUS_WAIT_JOIN)                 // not started yet
    {
        if (isArena())
        {
            player->CastSpell(player, SPELL_ARENA_PREPARATION, true);
            player->CastSpell(player, SPELL_INSTANT_CAST, true);
        }
        else
        {
            player->CastSpell(player, SPELL_PREPARATION, true);   // reduces all mana cost of spells.
            player->CastSpell(player, SPELL_INSTANT_CAST, true);
        }
        player->ResetAllPowers();
    }

    // reset all map criterias on map enter
    if (!isInBattleground)
        player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_BG_MAP, GetMapId(), true);

    // setup BG group membership
    PlayerAddedToBGCheckIfBGIsRunning(player);
    AddOrSetPlayerToCorrectBgGroup(player, team);

    // Initialize battleground worldstates immediately on join.
    // Relying only on zone-change driven updates can delay top-frame UI setup
    // on maps that report transitional/non-canonical zone ids near spawn.
    player->SendInitWorldStates(player->GetZoneId(), player->GetAreaId());

    // A tournament-pool match is fought in tournament gear: the swap happens
    // here, with everything else about the join already settled, and is undone
    // in RemovePlayerAtLeave (Miscellaneous/TournamentLoadout.cpp). Every other
    // match returns at once.
    Tournament::ApplyBattlegroundLoadout(player);
}

void Battleground::SendCustomGameRulesTo(Player* player) const
{
    if (!m_IsCustomGame || !player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    uint32 const resurrectionSeconds = (GetResurrectionInterval() + IN_MILLISECONDS - 1) / IN_MILLISECONDS;
    std::string const message = "CCGAME\tREZ:" + std::to_string(resurrectionSeconds);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty, ObjectGuid::Empty, message, 0);
    player->SendDirectMessage(&data);
}

void Battleground::SendSquadFeedTo(Player* player)
{
    SendSquadFeedLine(player, "SQ:-");
}

void Battleground::SendSquadFeedLine(Player* player, std::string const& line)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, ObjectGuid::Empty, ObjectGuid::Empty, "CCGAME\t" + line, 0);
    player->SendDirectMessage(&data);
}

void Battleground::SendSpectatorBattlefieldStatusTo(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    // Arenas are excluded: telling a spectator's client the battlefield is an
    // ACTIVE arena (rather than leaving it in its default not-active state)
    // switches the client's nameplate coloring from plain faction-reaction
    // (green/red, correct here since custom-arena participants keep real
    // Horde/Alliance factions) to Green-Team/Gold-Team arena coloring, which
    // it cannot resolve for a spectator who isn't in either raid group -- the
    // observed symptom was every participant's name rendering blue. Regular
    // (non-arena) custom games don't hit this: isArena() is already false, so
    // the client never enters that coloring mode regardless of this packet.
    if (isArena())
        return;

    // Spectators bypass the whole queue system, so none of the normal senders
    // (queue invite, port opcode, CMSG_BATTLEFIELD_STATUS queue loop) ever tell
    // their client the battleground is active. Without an ACTIVE status the
    // client keeps the scoreboard toggle and the minimap battlefield icon
    // disabled. Queue slot 0 is safe: a spectator holds no real queue entries.
    WorldPacket data;
    sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, this, 0, STATUS_IN_PROGRESS, GetEndTime(), GetStartTime(), GetArenaType(), player->GetBGTeam());
    player->SendDirectMessage(&data);

    // Seed the scoreboard so the first toggle isn't empty.
    BuildPvPLogDataPacket(data);
    player->SendDirectMessage(&data);
}

// this method adds player to his team's bg group, or sets his correct group if player is already in bg group
void Battleground::AddOrSetPlayerToCorrectBgGroup(Player* player, uint32 team)
{
    ObjectGuid playerGuid = player->GetGUID();
    uint8 const raidSlot = GetBgRaidSlot(playerGuid, team);
    Group* group = GetBgRaidBySlot(raidSlot);
    bool const joiningPlayerIsNonVirtual = player->GetSession() && !player->GetSession()->IsVirtualSession();
    if (!group)                                      // first player joined
    {
        group = new Group;
        SetBgRaidBySlot(raidSlot, group);
        group->Create(player);
        sGroupMgr->AddGroup(group);
    }
    else                                            // raid already exist
    {
        if (group->IsMember(playerGuid))
        {
            // Not when they are linked to it already: SetBattlegroundOrBattlefieldRaid
            // keeps the group they are in as the one to go back to, and that
            // would be this raid itself. Leaving it later would then put them
            // straight back into the raid being disbanded, and their client
            // would be sent that raid - with only them in it - in place of
            // their own group, and keep showing it after it is gone.
            if (player->GetGroup() != group)
            {
                uint8 subgroup = group->GetMemberGroup(playerGuid);
                player->SetBattlegroundOrBattlefieldRaid(group, subgroup);
            }
        }
        else
        {
            group->AddMember(player);
            if (Group* originalGroup = player->GetOriginalGroup())
                if (originalGroup->IsLeader(playerGuid))
                {
                    group->ChangeLeader(playerGuid);
                    group->SendUpdate();
                }
        }
    }

    ObjectGuid const currentLeaderGuid = group->GetLeaderGUID();
    Player const* currentLeader = ObjectAccessor::FindConnectedPlayer(currentLeaderGuid);
    bool const currentLeaderIsNonVirtual = currentLeader && currentLeader->GetSession() && !currentLeader->GetSession()->IsVirtualSession();

    if (!currentLeaderIsNonVirtual)
    {
        ObjectGuid const humanLeaderGuid = joiningPlayerIsNonVirtual ? playerGuid : GetFirstNonVirtualMemberGuid(group);
        if (!humanLeaderGuid.IsEmpty() && humanLeaderGuid != currentLeaderGuid)
        {
            group->ChangeLeader(humanLeaderGuid);
            group->SendUpdate();
        }
    }
}

// This method should be called when player logs into running battleground
void Battleground::EventPlayerLoggedIn(Player* player)
{
    ObjectGuid guid = player->GetGUID();
    // player is correct pointer
    for (auto itr = m_OfflineQueue.begin(); itr != m_OfflineQueue.end(); ++itr)
    {
        if (*itr == guid)
        {
            m_OfflineQueue.erase(itr);
            break;
        }
    }
    m_Players[guid].OfflineRemoveTime = 0;
    PlayerAddedToBGCheckIfBGIsRunning(player);
    // if battleground is starting, then add preparation aura
    // we don't have to do that, because preparation aura isn't removed when player logs out
}

// This method should be called when player logs out from running battleground
void Battleground::EventPlayerLoggedOut(Player* player)
{
    ObjectGuid guid = player->GetGUID();
    if (!IsPlayerInBattleground(guid))  // Check if this player really is in battleground (might be a GM who teleported inside)
        return;

    // player is correct pointer, it is checked in WorldSession::LogoutPlayer()
    m_OfflineQueue.push_back(player->GetGUID());
    m_Players[guid].OfflineRemoveTime = GameTime::GetGameTime() + MAX_OFFLINE_TIME;
    if (GetStatus() == STATUS_IN_PROGRESS)
    {
        // drop flag and handle other cleanups
        RemovePlayer(player, guid, GetPlayerTeam(guid));

        // 1 player is logging out, if it is the last alive, then end arena!
        // A shuffle only loses the round that way: RemovePlayer above has
        // already asked it.
        if (isArena() && !IsSoloShuffle() && player->IsAlive())
            if (GetAlivePlayersCountByTeam(player->GetBGTeam()) <= 1 && GetPlayersCountByTeam(GetOtherTeam(player->GetBGTeam())))
                EndBattleground(GetOtherTeam(player->GetBGTeam()));
    }
}

// This method should be called only once ... it adds pointer to queue
void Battleground::AddToBGFreeSlotQueue()
{
    if (!m_IsCustomGame && !m_InBGFreeSlotQueue && isBattleground())
    {
        sBattlegroundMgr->AddToBGFreeSlotQueue(m_TypeID, this);
        m_InBGFreeSlotQueue = true;
    }
}

// This method removes this battleground from free queue - it must be called when deleting battleground
void Battleground::RemoveFromBGFreeSlotQueue()
{
    if (m_InBGFreeSlotQueue)
    {
        sBattlegroundMgr->RemoveFromBGFreeSlotQueue(m_TypeID, m_InstanceID);
        m_InBGFreeSlotQueue = false;
    }
}



// get the number of free slots for team
// returns the number how many players can join battleground to MaxPlayersPerTeam
uint32 Battleground::GetFreeSlotsForTeam(uint32 Team) const
{
    // if BG is starting and CONFIG_BATTLEGROUND_INVITATION_TYPE == BG_QUEUE_INVITATION_TYPE_NO_BALANCE, invite anyone
    if (GetStatus() == STATUS_WAIT_JOIN && sWorld->getIntConfig(CONFIG_BATTLEGROUND_INVITATION_TYPE) == BG_QUEUE_INVITATION_TYPE_NO_BALANCE)
        return (GetInvitedCount(Team) < GetMaxPlayersPerTeam()) ? GetMaxPlayersPerTeam() - GetInvitedCount(Team) : 0;

    // if BG is already started or CONFIG_BATTLEGROUND_INVITATION_TYPE != BG_QUEUE_INVITATION_TYPE_NO_BALANCE, do not allow to join too much players of one faction
    uint32 otherTeamInvitedCount;
    uint32 thisTeamInvitedCount;
    uint32 otherTeamPlayersCount;
    uint32 thisTeamPlayersCount;

    if (Team == ALLIANCE)
    {
        thisTeamInvitedCount  = GetInvitedCount(ALLIANCE);
        otherTeamInvitedCount = GetInvitedCount(HORDE);
        thisTeamPlayersCount  = GetPlayersCountByTeam(ALLIANCE);
        otherTeamPlayersCount = GetPlayersCountByTeam(HORDE);
    }
    else
    {
        thisTeamInvitedCount  = GetInvitedCount(HORDE);
        otherTeamInvitedCount = GetInvitedCount(ALLIANCE);
        thisTeamPlayersCount  = GetPlayersCountByTeam(HORDE);
        otherTeamPlayersCount = GetPlayersCountByTeam(ALLIANCE);
    }
    if (GetStatus() == STATUS_IN_PROGRESS || GetStatus() == STATUS_WAIT_JOIN)
    {
        // difference based on ppl invited (not necessarily entered battle)
        // default: allow 0
        uint32 diff = 0;

        // allow join one person if the sides are equal (to fill up bg to minPlayerPerTeam)
        if (otherTeamInvitedCount == thisTeamInvitedCount)
            diff = 1;
        // allow join more ppl if the other side has more players
        else if (otherTeamInvitedCount > thisTeamInvitedCount)
            diff = otherTeamInvitedCount - thisTeamInvitedCount;

        // difference based on max players per team (don't allow inviting more)
        uint32 diff2 = (thisTeamInvitedCount < GetMaxPlayersPerTeam()) ? GetMaxPlayersPerTeam() - thisTeamInvitedCount : 0;

        // difference based on players who already entered
        // default: allow 0
        uint32 diff3 = 0;
        // allow join one person if the sides are equal (to fill up bg minPlayerPerTeam)
        if (otherTeamPlayersCount == thisTeamPlayersCount)
            diff3 = 1;
        // allow join more ppl if the other side has more players
        else if (otherTeamPlayersCount > thisTeamPlayersCount)
            diff3 = otherTeamPlayersCount - thisTeamPlayersCount;
        // or other side has less than minPlayersPerTeam
        else if (thisTeamInvitedCount <= GetMinPlayersPerTeam())
            diff3 = GetMinPlayersPerTeam() - thisTeamInvitedCount + 1;

        // return the minimum of the 3 differences

        // min of diff and diff 2
        diff = std::min(diff, diff2);
        // min of diff, diff2 and diff3
        return std::min(diff, diff3);
    }
    return 0;
}

bool Battleground::HasFreeSlots() const
{
    if (GetPlayersSize() < GetMaxPlayers())
        return true;

    // Treat bot participants (virtual sessions, and the transient clones of a
    // bot-filled match) as replaceable occupancy for battlegrounds: this keeps
    // full bot-populated matches in the free-slot queue so queued real players
    // can displace one bot on invite.
    if (!isBattleground())
        return false;

    for (auto const& playerEntry : m_Players)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(playerEntry.first);
        if (!player)
            continue;

        if (IsBotParticipantSession(player->GetSession()))
            return true;
    }

    return false;
}

bool Battleground::IsBotParticipantSession(WorldSession const* session) const
{
    if (!session)
        return false;

    if (session->IsVirtualSession())
        return true;

    return m_IsBotFillMatch && session->IsTransientPlayerSession();
}

bool Battleground::HasPersonOnTeam(uint32 team) const
{
    for (auto const& [guid, bgPlayer] : m_Players)
    {
        if (bgPlayer.Team != team)
            continue;

        // Nobody connected under that guid is somebody who dropped: a clone
        // is never offline, it is taken off the roster instead.
        Player const* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (!player || !IsBotParticipantSession(player->GetSession()))
            return true;
    }

    return false;
}

uint32 Battleground::CountPreparationSeatsHeldByClones(uint32 team)
{
    if (!m_IsBotFillMatch || GetStatus() != STATUS_WAIT_JOIN)
        return 0;

    PrunePreparationSeatTakers();
    uint32 promised = 0;
    for (auto const& reservation : m_PreparationSeatTakers)
        if (reservation.second == team)
            ++promised;

    uint32 clones = 0;
    for (auto const& [guid, participant] : m_Players)
    {
        if (participant.Team != team)
            continue;

        Player const* player = ObjectAccessor::FindConnectedPlayer(guid);
        if (player && player->GetSession() && player->GetSession()->IsTransientPlayerSession())
            ++clones;
    }

    return clones > promised ? clones - promised : 0;
}

uint32 Battleground::CountUnclaimedPreparationSeats(uint32 team) const
{
    if (!m_IsBotFillMatch || GetStatus() != STATUS_WAIT_JOIN || (team != ALLIANCE && team != HORDE))
        return 0;

    // Everyone in counts, clones included, then everyone on the way: people
    // invited and clones still loading. The same sum the bot fill keeps under
    // the team size, so a seat counted free here is one it will not fill.
    uint32 held = GetInvitedCount(team) + m_BotFillSeatsOnTheWay[GetTeamIndexByTeamId(team)];
    for (auto const& [guid, participant] : m_Players)
        if (participant.Team == team)
            ++held;

    uint32 const seats = GetMaxPlayersPerTeam();
    return seats > held ? seats - held : 0;
}

void Battleground::SetBotFillSeatsOnTheWay(uint32 team, uint32 count)
{
    if (team == ALLIANCE || team == HORDE)
        m_BotFillSeatsOnTheWay[GetTeamIndexByTeamId(team)] = count;
}

// A reservation lasts while its holder is still invited here and not yet in:
// through the invite, the click on Enter Battle and the loading screen. A
// lapsed invite, a logout or the arrival itself ends it.
void Battleground::PrunePreparationSeatTakers()
{
    for (auto itr = m_PreparationSeatTakers.begin(); itr != m_PreparationSeatTakers.end();)
    {
        Player const* player = ObjectAccessor::FindConnectedPlayer(itr->first);
        if (!player || !player->IsInvitedForBattlegroundInstance(GetInstanceID()) || IsPlayerInBattleground(itr->first))
            itr = m_PreparationSeatTakers.erase(itr);
        else
            ++itr;
    }
}

// Called on arrival, with the newcomer already on the roster: a team is never
// left empty, which would hand the other side the arena on the spot.
void Battleground::HandOverPreparationSeat(Player* player, uint32 team)
{
    auto const reservation = m_PreparationSeatTakers.find(player->GetGUID());
    if (reservation == m_PreparationSeatTakers.end())
        return;

    m_PreparationSeatTakers.erase(reservation);

    ObjectGuid cloneGuid;
    for (auto const& [guid, participant] : m_Players)
    {
        if (participant.Team != team)
            continue;

        Player const* candidate = ObjectAccessor::FindConnectedPlayer(guid);
        if (candidate && candidate->GetSession() && candidate->GetSession()->IsTransientPlayerSession())
        {
            cloneGuid = guid;
            break;
        }
    }

    if (cloneGuid.IsEmpty())
        TC_LOG_ERROR("bg.battleground", "Battleground::HandOverPreparationSeat: {} arrived in {} instance {} for a clone's seat on team {}, and that team has no clone left to stand down.",
            player->GetName(), GetName(), GetInstanceID(), team);
    else
    {
        // A clone has no entry point and no client, so it is only unseated;
        // the playerbot clone manager takes it down on its next pass. Leaving
        // gives back an invite the clone never held - restore whatever that
        // took from the people still on their way in.
        uint32 const invitedBefore = GetInvitedCount(team);
        RemovePlayerAtLeave(cloneGuid, false, false);
        while (GetInvitedCount(team) < invitedBefore)
            IncreaseInvitedCount(team);

        TC_LOG_DEBUG("bg.battleground", "Battleground::HandOverPreparationSeat: {} took clone {}'s seat on team {} of {} instance {}.",
            player->GetName(), cloneGuid.ToString(), team, GetName(), GetInstanceID());
    }

    RestartPreparationCountdown();
}

// Back to the first warning, as though the gates had just shut behind
// everyone: the full preparation for whoever walked in, later warnings re-armed.
void Battleground::RestartPreparationCountdown()
{
    // Not begun (it starts from the top on its own), or already over.
    if (GetStatus() != STATUS_WAIT_JOIN || !(m_Events & BG_STARTING_EVENT_1) || (m_Events & BG_STARTING_EVENT_4))
        return;

    m_Events &= uint8(~(BG_STARTING_EVENT_2 | BG_STARTING_EVENT_3));
    SetStartDelayTime(StartDelayTimes[BG_STARTING_EVENT_FIRST]);
    if (StartMessageIds[BG_STARTING_EVENT_FIRST])
        SendBroadcastText(StartMessageIds[BG_STARTING_EVENT_FIRST], CHAT_MSG_BG_SYSTEM_NEUTRAL);
}

std::string Battleground::GetPlayerDisplayName(Player const* player)
{
    std::string name;
    if (!sCharacterCache->GetCharacterNameByGuid(player->GetGUID(), name))
        name = player->GetName();
    return Surnames::Decorated(player->GetGUID(), name);
}

Player* Battleground::FindTransientPlayerByDisplayName(std::string_view name, uint32 preferredTeam) const
{
    Player* otherSide = nullptr;
    for (auto const& [guid, participant] : m_Players)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->GetSession() || !player->GetSession()->IsTransientPlayerSession())
            continue;

        // A copy wears its source's family name (Surnames::AdoptTransient), so
        // the name players see and type is "<name> <surname>". The bare first
        // name still answers too, as it does for everyone else.
        std::string displayName;
        if (!sCharacterCache->GetCharacterNameByGuid(guid, displayName))
            continue;
        if (!StringEqualI(displayName, name) && !StringEqualI(Surnames::Decorated(guid, displayName), name))
            continue;

        if (participant.Team == preferredTeam)
            return player;

        if (!otherSide)
            otherSide = player;
    }

    return otherSide;
}

void Battleground::BuildPvPLogDataPacket(WorldPacket& data, Player const* viewer)
{
    uint8 type = (isArena() ? 1 : 0);

    // A squad match is shown to each player as two teams, their squad and
    // everyone else, the same way the rest of their client sees it: the
    // packet only has room for two. Without a viewer the sides are sent.
    bool const squadView = IsSquadMatch() && viewer && GetPlayerSquad(viewer->GetGUID()) != BATTLEGROUND_NO_SQUAD;
    uint8 const viewerSquad = squadView ? GetPlayerSquad(viewer->GetGUID()) : BATTLEGROUND_NO_SQUAD;
    PvPTeamId const viewerSide = squadView && GetPlayerTeam(viewer->GetGUID()) == HORDE ? PVP_TEAM_HORDE : PVP_TEAM_ALLIANCE;
    PvPTeamId const otherSide = viewerSide == PVP_TEAM_ALLIANCE ? PVP_TEAM_HORDE : PVP_TEAM_ALLIANCE;

    // Battleground subclasses may create a score entry after the base AddPlayer
    // rejected a spectator. Only active participants belong in the client
    // scoreboard; custom-game spectators are deliberately kept outside
    // m_Players.
    std::vector<BattlegroundScore*> participantScores;
    participantScores.reserve(PlayerScores.size());
    for (auto const& score : PlayerScores)
        if (m_Players.find(score.second->PlayerGuid) != m_Players.end())
            participantScores.push_back(score.second);

    data.Initialize(MSG_PVP_LOG_DATA, 1 + 1 + 4 + 40 * participantScores.size());
    data << uint8(type);                                // type (battleground = 0 / arena = 1)

    if (type)                                           // arena
    {
        // A rated squad match: the viewer's squad in their side's block, and
        // opposite it the squad its scoreboard is set against.
        ArenaTeamScore* blocks[PVP_TEAMS_COUNT] = { &_arenaTeamScores[0], &_arenaTeamScores[1] };
        if (squadView && isRated())
        {
            blocks[viewerSide] = &_squadArenaScores[viewerSquad];
            uint8 const rival = m_SquadScoreboardRival[viewerSquad];
            if (rival < MAX_BATTLEGROUND_SQUADS)
                blocks[otherSide] = &_squadArenaScores[rival];
        }

        for (uint8 i = 0; i < PVP_TEAMS_COUNT; ++i)
            blocks[i]->BuildRatingInfoBlock(data);

        for (uint8 i = 0; i < PVP_TEAMS_COUNT; ++i)
            blocks[i]->BuildTeamInfoBlock(data);
    }

    if (GetStatus() == STATUS_WAIT_LEAVE)
    {
        data << uint8(1);                      // bg ended
        if (squadView && m_WinnerSquad != BATTLEGROUND_NO_SQUAD)
            data << uint8(m_WinnerSquad == viewerSquad ? viewerSide : otherSide);
        else
            data << uint8(GetWinner());        // who win
    }
    else
        data << uint8(0);                      // bg not ended

    data << uint32(participantScores.size());
    for (BattlegroundScore* score : participantScores)
    {
        if (squadView)
        {
            if (ArenaScore* arenaScore = dynamic_cast<ArenaScore*>(score))
            {
                uint8 const realTeam = arenaScore->TeamId;
                arenaScore->TeamId = GetPlayerSquad(score->PlayerGuid) == viewerSquad ? viewerSide : otherSide;
                score->AppendToPacket(data);
                arenaScore->TeamId = realTeam;
                continue;
            }
        }

        // A battleground row carries no side on the wire, so the client takes
        // it from the race - Alliance for nearly everyone on this realm. Our
        // scoreboard (WorldStateFrame.lua) reads the side from the bonus-honor
        // column instead: 1 for Horde. A few battlegrounds set that marker as
        // they add a player and the rest (Eye of the Storm among them) never
        // did, which showed every row as Alliance; set it here for all.
        if (!type)
            score->BonusHonor = m_Players.find(score->PlayerGuid)->second.Team == HORDE ? 1 : 0;
        score->AppendToPacket(data);
    }
}

bool Battleground::UpdatePlayerScore(Player* player, uint32 type, uint32 value, bool doAddHonor)
{
    BattlegroundScoreMap::const_iterator itr = PlayerScores.find(player->GetGUID().GetCounter());
    if (itr == PlayerScores.end()) // player not found...
        return false;

    if (type == SCORE_BONUS_HONOR && doAddHonor && isBattleground() && !m_IsCustomGame)
        player->RewardHonor(nullptr, 1, value); // RewardHonor calls UpdatePlayerScore with doAddHonor = false
    else
    {
        //don't update bonus honor
        if (type != SCORE_BONUS_HONOR)
            itr->second->UpdateScore(type, value);
    }

    return true;
}

void Battleground::AddPlayerToResurrectQueue(ObjectGuid npc_guid, ObjectGuid player_guid)
{
    m_ReviveQueue[npc_guid].push_back(player_guid);

    Player* player = ObjectAccessor::FindPlayer(player_guid);
    if (!player)
        return;

    player->CastSpell(player, SPELL_WAITING_FOR_RESURRECT, true);
}

void Battleground::RemovePlayerFromResurrectQueue(ObjectGuid player_guid)
{
    for (std::map<ObjectGuid, GuidVector>::iterator itr = m_ReviveQueue.begin(); itr != m_ReviveQueue.end(); ++itr)
    {
        for (GuidVector::iterator itr2 = itr->second.begin(); itr2 != itr->second.end(); ++itr2)
        {
            if (*itr2 == player_guid)
            {
                itr->second.erase(itr2);
                if (Player* player = ObjectAccessor::FindPlayer(player_guid))
                    player->RemoveAurasDueToSpell(SPELL_WAITING_FOR_RESURRECT);
                return;
            }
        }
    }
}

uint32 Battleground::GetTimeUntilResurrection() const
{
    uint32 const interval = GetResurrectionInterval();

    if (m_LastResurrectTime >= interval)
        return 0;

    return interval - m_LastResurrectTime;
}

bool Battleground::IsPlayerInResurrectQueue(ObjectGuid player_guid) const
{
    for (std::map<ObjectGuid, GuidVector>::const_iterator itr = m_ReviveQueue.begin(); itr != m_ReviveQueue.end(); ++itr)
    {
        for (GuidVector::const_iterator itr2 = itr->second.begin(); itr2 != itr->second.end(); ++itr2)
            if (*itr2 == player_guid)
                return true;
    }

    for (GuidVector::const_iterator itr = m_ResurrectQueue.begin(); itr != m_ResurrectQueue.end(); ++itr)
        if (*itr == player_guid)
            return true;

    return false;
}

void Battleground::RelocateDeadPlayers(ObjectGuid guideGuid)
{
    // Those who are waiting to resurrect at this node are taken to the closest own node's graveyard
    GuidVector& ghostList = m_ReviveQueue[guideGuid];
    if (!ghostList.empty())
    {
        WorldSafeLocsEntry const* closestGrave = nullptr;
        for (GuidVector::const_iterator itr = ghostList.begin(); itr != ghostList.end(); ++itr)
        {
            Player* player = ObjectAccessor::FindPlayer(*itr);
            if (!player)
                continue;

            if (!closestGrave)
                closestGrave = GetClosestGraveyard(player);

            if (closestGrave)
                player->TeleportTo(GetMapId(), closestGrave->Loc.X, closestGrave->Loc.Y, closestGrave->Loc.Z, player->GetOrientation());
        }
        ghostList.clear();
    }
}

bool Battleground::AddObject(uint32 type, uint32 entry, float x, float y, float z, float o, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime, GOState goState)
{
    // If the assert is called, means that BgObjects must be resized!
    ASSERT(type < BgObjects.size());

    Map* map = FindBgMap();
    if (!map)
        return false;

    QuaternionData rot(rotation0, rotation1, rotation2, rotation3);
    // Temporally add safety check for bad spawns and send log (object rotations need to be rechecked in sniff)
    if (!rotation0 && !rotation1 && !rotation2 && !rotation3)
    {
        TC_LOG_DEBUG("bg.battleground", "Battleground::AddObject: gameoobject [entry: {}, object type: {}] for BG (map: {}) has zeroed rotation fields, "
            "orientation used temporally, but please fix the spawn", entry, type, m_MapId);

        rot = QuaternionData::fromEulerAnglesZYX(o, 0.f, 0.f);
    }

    // Must be created this way, adding to godatamap would add it to the base map of the instance
    // and when loading it (in go::LoadFromDB()), a new guid would be assigned to the object, and a new object would be created
    // So we must create it specific for this instance
    GameObject* go = new GameObject;
    if (!go->Create(GetBgMap()->GenerateLowGuid<HighGuid::GameObject>(), entry, GetBgMap(), PHASEMASK_NORMAL, Position(x, y, z, o), rot, 255, goState))
    {
        TC_LOG_ERROR("bg.battleground", "Battleground::AddObject: cannot create gameobject (entry: {}) for BG (map: {}, instance id: {})!",
                entry, m_MapId, m_InstanceID);
        delete go;
        return false;
    }

    if (respawnTime)
    {
        go->SetLootState(GO_JUST_DEACTIVATED);
        go->SetRespawnTime(respawnTime);
    }
/*
    uint32 guid = go->GetGUID().GetCounter();

    // without this, UseButtonOrDoor caused the crash, since it tried to get go info from godata
    // iirc that was changed, so adding to go data map is no longer required if that was the only function using godata from GameObject without checking if it existed
    GameObjectData& data = sObjectMgr->NewGOData(guid);

    data.id             = entry;
    data.mapid          = GetMapId();
    data.posX           = x;
    data.posY           = y;
    data.posZ           = z;
    data.orientation    = o;
    data.rotation0      = rotation0;
    data.rotation1      = rotation1;
    data.rotation2      = rotation2;
    data.rotation3      = rotation3;
    data.spawntimesecs  = respawnTime;
    data.spawnMask      = 1;
    data.animprogress   = 100;
    data.go_state       = 1;
*/
    // Add to world, so it can be later looked up from HashMapHolder
    if (!map->AddToMap(go))
    {
        delete go;
        return false;
    }
    BgObjects[type] = go->GetGUID();
    return true;
}

bool Battleground::AddObject(uint32 type, uint32 entry, Position const& pos, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime /*= 0*/, GOState goState /*= GO_STATE_READY*/)
{
    return AddObject(type, entry, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation(), rotation0, rotation1, rotation2, rotation3, respawnTime, goState);
}

// Some doors aren't despawned so we cannot handle their closing in gameobject::update()
// It would be nice to correctly implement GO_ACTIVATED state and open/close doors in gameobject code
void Battleground::DoorClose(uint32 type)
{
    if (GameObject* obj = GetBgMap()->GetGameObject(BgObjects[type]))
    {
        // If doors are open, close it
        if (obj->getLootState() == GO_ACTIVATED && obj->GetGoState() != GO_STATE_READY)
        {
            obj->SetLootState(GO_READY);
            obj->SetGoState(GO_STATE_READY);
        }
    }
    else
        TC_LOG_ERROR("bg.battleground", "Battleground::DoorClose: door gameobject (type: {}, {}) not found for BG (map: {}, instance id: {})!",
            type, BgObjects[type].ToString(), m_MapId, m_InstanceID);
}

void Battleground::DoorOpen(uint32 type)
{
    if (GameObject* obj = GetBgMap()->GetGameObject(BgObjects[type]))
    {
        obj->SetLootState(GO_ACTIVATED);
        obj->SetGoState(GO_STATE_ACTIVE);
    }
    else
        TC_LOG_ERROR("bg.battleground", "Battleground::DoorOpen: door gameobject (type: {}, {}) not found for BG (map: {}, instance id: {})!",
            type, BgObjects[type].ToString(), m_MapId, m_InstanceID);
}

GameObject* Battleground::GetBGObject(uint32 type, bool logError)
{
    GameObject* obj = GetBgMap()->GetGameObject(BgObjects[type]);
    if (!obj)
    {
        if (logError)
            TC_LOG_ERROR("bg.battleground", "Battleground::GetBGObject: gameobject (type: {}, {}) not found for BG (map: {}, instance id: {})!",
                type, BgObjects[type].ToString(), m_MapId, m_InstanceID);
        else
            TC_LOG_INFO("bg.battleground", "Battleground::GetBGObject: gameobject (type: {}, {}) not found for BG (map: {}, instance id: {})!",
                type, BgObjects[type].ToString(), m_MapId, m_InstanceID);
    }
    return obj;
}

Creature* Battleground::GetBGCreature(uint32 type, bool logError)
{
    Creature* creature = GetBgMap()->GetCreature(BgCreatures[type]);
    if (!creature)
    {
        if (logError)
            TC_LOG_ERROR("bg.battleground", "Battleground::GetBGCreature: creature (type: {}, {}) not found for BG (map: {}, instance id: {})!",
                type, BgCreatures[type].ToString(), m_MapId, m_InstanceID);
        else
            TC_LOG_INFO("bg.battleground", "Battleground::GetBGCreature: creature (type: {}, {}) not found for BG (map: {}, instance id: {})!",
                type, BgCreatures[type].ToString(), m_MapId, m_InstanceID);
    }
    return creature;
}

void Battleground::SpawnBGObject(uint32 type, uint32 respawntime)
{
    if (Map* map = FindBgMap())
        if (GameObject* obj = map->GetGameObject(BgObjects[type]))
        {
            if (respawntime)
            {
                obj->SetLootState(GO_JUST_DEACTIVATED);

                if (GameObjectOverride const* goOverride = obj->GetGameObjectOverride())
                    if (goOverride->Flags & GO_FLAG_NODESPAWN)
                    {
                        // This function should be called in GameObject::Update() but in case of
                        // GO_FLAG_NODESPAWN flag the function is never called, so we call it here
                        obj->SendObjectDeSpawnAnim(obj->GetGUID());
                    }
            }
            else if (obj->getLootState() == GO_JUST_DEACTIVATED)
            {
                // Change state from GO_JUST_DEACTIVATED to GO_READY in case battleground is starting again
                obj->SetLootState(GO_READY);
            }
            obj->SetRespawnTime(respawntime);
            if (!obj->IsInWorld())
                map->AddToMap(obj);
            else
                obj->UpdateObjectVisibility(true);
        }
}

Creature* Battleground::AddCreature(uint32 entry, uint32 type, float x, float y, float z, float o, TeamId /*teamId = TEAM_NEUTRAL*/, uint32 respawntime /*= 0*/, Transport* transport)
{
    // If the assert is called, means that BgCreatures must be resized!
    ASSERT(type < BgCreatures.size());

    Map* map = FindBgMap();
    if (!map)
        return nullptr;

    if (transport)
    {
        if (Creature* creature = transport->SummonPassenger(entry, { x, y, z, o }, TEMPSUMMON_MANUAL_DESPAWN))
        {
            BgCreatures[type] = creature->GetGUID();
            return creature;
        }

        return nullptr;
    }

    Creature* creature = new Creature();

    if (!creature->Create(map->GenerateLowGuid<HighGuid::Unit>(), map, PHASEMASK_NORMAL, entry, { x, y, z, o }))
    {
        TC_LOG_ERROR("bg.battleground", "Battleground::AddCreature: cannot create creature (entry: {}) for BG (map: {}, instance id: {})!",
            entry, m_MapId, m_InstanceID);
        delete creature;
        return nullptr;
    }

    creature->SetHomePosition(x, y, z, o);

    CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(entry);
    if (!cinfo)
    {
        TC_LOG_ERROR("bg.battleground", "Battleground::AddCreature: creature template (entry: {}) does not exist for BG (map: {}, instance id: {})!",
            entry, m_MapId, m_InstanceID);
        delete creature;
        return nullptr;
    }

    if (!map->AddToMap(creature))
    {
        delete creature;
        return nullptr;
    }

    BgCreatures[type] = creature->GetGUID();

    if (respawntime)
        creature->SetRespawnDelay(respawntime);

    return creature;
}

Creature* Battleground::AddCreature(uint32 entry, uint32 type, Position const& pos, TeamId teamId /*= TEAM_NEUTRAL*/, uint32 respawntime /*= 0*/, Transport* transport)
{
    return AddCreature(entry, type, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation(), teamId, respawntime, transport);
}

bool Battleground::DelCreature(uint32 type)
{
    if (!BgCreatures[type])
        return true;

    if (Creature* creature = GetBgMap()->GetCreature(BgCreatures[type]))
    {
        creature->AddObjectToRemoveList();
        BgCreatures[type].Clear();
        return true;
    }

    TC_LOG_ERROR("bg.battleground", "Battleground::DelCreature: creature (type: {}, {}) not found for BG (map: {}, instance id: {})!",
        type, BgCreatures[type].ToString(), m_MapId, m_InstanceID);
    BgCreatures[type].Clear();
    return false;
}

bool Battleground::DelObject(uint32 type)
{
    if (!BgObjects[type])
        return true;

    if (GameObject* obj = GetBgMap()->GetGameObject(BgObjects[type]))
    {
        obj->SetRespawnTime(0);                                 // not save respawn time
        obj->Delete();
        BgObjects[type].Clear();
        return true;
    }
    TC_LOG_ERROR("bg.battleground", "Battleground::DelObject: gameobject (type: {}, {}) not found for BG (map: {}, instance id: {})!",
        type, BgObjects[type].ToString(), m_MapId, m_InstanceID);
    BgObjects[type].Clear();
    return false;
}

bool Battleground::RemoveObjectFromWorld(uint32 type)
{
    if (!BgObjects[type])
        return true;

    if (GameObject* obj = GetBgMap()->GetGameObject(BgObjects[type]))
    {
        obj->RemoveFromWorld();
        BgObjects[type].Clear();
        return true;
    }
    TC_LOG_INFO("bg.battleground", "Battleground::RemoveObjectFromWorld: gameobject (type: {}, {}) not found for BG (map: {}, instance id: {})!",
        type, BgObjects[type].ToString(), m_MapId, m_InstanceID);
    return false;
}

bool Battleground::AddSpiritGuide(uint32 type, float x, float y, float z, float o, TeamId teamId /*= TEAM_NEUTRAL*/)
{
    uint32 entry = (teamId == TEAM_ALLIANCE) ? BG_CREATURE_ENTRY_A_SPIRITGUIDE : BG_CREATURE_ENTRY_H_SPIRITGUIDE;

    if (Creature* creature = AddCreature(entry, type, x, y, z, o, teamId))
    {
        creature->setDeathState(DEAD);
        creature->SetChannelObjectGuid(creature->GetGUID());
        // aura
        /// @todo Fix display here
        // creature->SetVisibleAura(0, SPELL_SPIRIT_HEAL_CHANNEL);
        // casting visual effect
        creature->SetChannelSpellId(SPELL_SPIRIT_HEAL_CHANNEL);
        // correct cast speed
        creature->SetModCastingSpeed(1.0f);
        //creature->CastSpell(creature, SPELL_SPIRIT_HEAL_CHANNEL, true);
        return true;
    }
    TC_LOG_ERROR("bg.battleground", "Battleground::AddSpiritGuide: cannot create spirit guide (type: {}, entry: {}) for BG (map: {}, instance id: {})!",
        type, entry, m_MapId, m_InstanceID);
    EndNow();
    return false;
}

bool Battleground::AddSpiritGuide(uint32 type, Position const& pos, TeamId teamId /*= TEAM_NEUTRAL*/)
{
    return AddSpiritGuide(type, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation(), teamId);
}

void Battleground::SendMessageToAll(uint32 entry, ChatMsg msgType, Player const* source)
{
    if (!entry)
        return;

    Trinity::TrinityStringChatBuilder builder(nullptr, msgType, entry, source);
    Trinity::LocalizedPacketDo<Trinity::TrinityStringChatBuilder> localizer(builder);
    BroadcastWorker(localizer);
}

void Battleground::PSendMessageToAll(uint32 entry, ChatMsg msgType, Player const* source, ...)
{
    if (!entry)
        return;

    va_list ap;
    va_start(ap, source);

    Trinity::TrinityStringChatBuilder builder(nullptr, msgType, entry, source, &ap);
    Trinity::LocalizedPacketDo<Trinity::TrinityStringChatBuilder> localizer(builder);
    BroadcastWorker(localizer);

    va_end(ap);
}

void Battleground::EndNow()
{
    RemoveFromBGFreeSlotQueue();
    SetStatus(STATUS_WAIT_LEAVE);
    SetEndTime(0);
}

// IMPORTANT NOTICE:
// buffs aren't spawned/despawned when players captures anything
// buffs are in their positions when battleground starts
void Battleground::HandleTriggerBuff(ObjectGuid go_guid)
{
    if (!FindBgMap())
    {
        TC_LOG_ERROR("bg.battleground", "Battleground::HandleTriggerBuff called with null bg map, {}", go_guid.ToString());
        return;
    }

    GameObject* obj = GetBgMap()->GetGameObject(go_guid);
    if (!obj || obj->GetGoType() != GAMEOBJECT_TYPE_TRAP || !obj->isSpawned())
        return;

    // Change buff type, when buff is used:
    int32 index = BgObjects.size() - 1;
    while (index >= 0 && BgObjects[index] != go_guid)
        index--;
    if (index < 0)
    {
        TC_LOG_ERROR("bg.battleground", "Battleground::HandleTriggerBuff: cannot find buff gameobject ({}, entry: {}, type: {}) in internal data for BG (map: {}, instance id: {})!",
            go_guid.ToString(), obj->GetEntry(), obj->GetGoType(), m_MapId, m_InstanceID);
        return;
    }

    // Randomly select new buff
    uint8 buff = urand(0, 2);
    uint32 entry = obj->GetEntry();
    if (m_BuffChange && entry != Buff_Entries[buff])
    {
        // Despawn current buff
        SpawnBGObject(index, RESPAWN_ONE_DAY);
        // Set index for new one
        for (uint8 currBuffTypeIndex = 0; currBuffTypeIndex < 3; ++currBuffTypeIndex)
            if (entry == Buff_Entries[currBuffTypeIndex])
            {
                index -= currBuffTypeIndex;
                index += buff;
            }
    }

    SpawnBGObject(index, GetBuffRespawnTime(index));
}

void Battleground::HandleKillPlayer(Player* victim, Player* killer)
{
    // Keep in mind that for arena this will have to be changed a bit

    // Add +1 deaths
    UpdatePlayerScore(victim, SCORE_DEATHS, 1);
    // Add +1 kills to group and +1 killing_blows to killer
    if (killer)
    {
        // Don't reward credit for killing ourselves, like fall damage of hellfire (warlock)
        if (killer == victim)
            return;

        UpdatePlayerScore(killer, SCORE_HONORABLE_KILLS, 1);
        UpdatePlayerScore(killer, SCORE_KILLING_BLOWS, 1);

        for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
        {
            Player* creditedPlayer = ObjectAccessor::FindPlayer(itr->first);
            if (!creditedPlayer || creditedPlayer == killer)
                continue;

            if (creditedPlayer->GetTeam() == killer->GetTeam() && creditedPlayer->IsAtGroupRewardDistance(victim))
                UpdatePlayerScore(creditedPlayer, SCORE_HONORABLE_KILLS, 1);
        }
    }

    if (!isArena())
    {
        // To be able to remove insignia -- ONLY IN Battlegrounds
        victim->SetUnitFlag(UNIT_FLAG_SKINNABLE);
        if (!m_IsCustomGame)
            RewardXPAtKill(killer, victim);
    }
}

// Return the player's team based on battlegroundplayer info
// Used in same faction arena matches mainly
uint32 Battleground::GetPlayerTeam(ObjectGuid guid) const
{
    BattlegroundPlayerMap::const_iterator itr = m_Players.find(guid);
    if (itr != m_Players.end())
        return itr->second.Team;
    return 0;
}

bool Battleground::ShouldShowFlagOnMapTo(Player const* viewer, TeamId flagTeam) const
{
    if (!m_IsCustomGame || !viewer)
        return true;

    uint32 viewerTeam = viewer->GetBGTeam();
    if (viewerTeam != ALLIANCE && viewerTeam != HORDE)
        viewerTeam = viewer->GetTeam();

    TeamId const viewerTeamId = TeamId(GetTeamIndexByTeamId(viewerTeam));
    return flagTeam == viewerTeamId ? m_CustomRules.ShowAllyFlagOnMap : m_CustomRules.ShowEnemyFlagOnMap;
}

// The 3.3.5 client asks for flag carrier positions only from its map UI, at most once
// every 5 seconds (Wow.exe 0x54CF60), and is never told when a flag changes hands, so
// its maps kept showing a flag on the previous carrier. Flag events push fresh
// positions instead; the client applies MSG_BATTLEGROUND_PLAYER_POSITIONS unasked.
void Battleground::SendFlagCarrierPositionsToPlayers()
{
    for (auto const& entry : GetPlayers())
        if (Player* player = ObjectAccessor::FindPlayer(entry.first))
            if (WorldSession* session = player->GetSession())
                session->SendBattlegroundPlayerPositions();
}

bool Battleground::GetNodeObjective(ObjectGuid playerGuid, BattlegroundNodeObjective& objective) const
{
    if (playerGuid.IsEmpty())
        return false;

    std::vector<BattlegroundNodeObjective> assaultNodes;
    std::vector<BattlegroundNodeObjective> defenseNodes;
    std::vector<BattlegroundNodeObjective> urgentDefenseNodes;
    for (uint32 nodeId = 0; nodeId < GetDynamicNodeCount(); ++nodeId)
    {
        BattlegroundNodeObjective node;
        if (!GetDynamicNodeInfo(playerGuid, nodeId, node))
            continue;

        if (node.Status == BattlegroundNodeStatus::FriendlyUnderAttack)
            urgentDefenseNodes.push_back(node);
        else if (node.Status == BattlegroundNodeStatus::FriendlyControlled ||
            node.Status == BattlegroundNodeStatus::FriendlyContested)
            defenseNodes.push_back(node);
        else
            assaultNodes.push_back(node);
    }

    // Keep a stable one-third of participants on defense. Stable role and
    // node selection prevent every update from sending the same bot toward a
    // different base as ownership changes elsewhere on the map.
    uint64 const seed = playerGuid.GetRawValue();
    bool const prefersDefense = (seed % 3) == 0;
    std::vector<BattlegroundNodeObjective> const* candidates = nullptr;
    if (prefersDefense && !urgentDefenseNodes.empty())
        candidates = &urgentDefenseNodes;
    else if (prefersDefense && !defenseNodes.empty())
        candidates = &defenseNodes;
    else if (!assaultNodes.empty())
        candidates = &assaultNodes;
    else if (!defenseNodes.empty())
        candidates = &defenseNodes;
    else if (!urgentDefenseNodes.empty())
        candidates = &urgentDefenseNodes;

    if (!candidates || candidates->empty())
        return false;

    objective = (*candidates)[(seed / 3) % candidates->size()];
    return true;
}

uint32 Battleground::GetOtherTeam(uint32 teamId) const
{
    return teamId ? ((teamId == ALLIANCE) ? HORDE : ALLIANCE) : 0;
}

bool Battleground::IsPlayerInBattleground(ObjectGuid guid) const
{
    BattlegroundPlayerMap::const_iterator itr = m_Players.find(guid);
    if (itr != m_Players.end())
        return true;
    return false;
}

void Battleground::PlayerAddedToBGCheckIfBGIsRunning(Player* player)
{
    if (GetStatus() != STATUS_WAIT_LEAVE)
        return;

    WorldPacket data;
    BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(GetTypeID(), GetArenaType());

    BlockMovement(player);

    BuildPvPLogDataPacket(data);
    player->SendDirectMessage(&data);

    sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, this, player->GetBattlegroundQueueIndex(bgQueueTypeId), STATUS_IN_PROGRESS, GetEndTime(), GetStartTime(), GetArenaType(), player->GetBGTeam());
    player->SendDirectMessage(&data);
}

uint32 Battleground::GetAlivePlayersCountByTeam(uint32 Team) const
{
    int count = 0;
    for (BattlegroundPlayerMap::const_iterator itr = m_Players.begin(); itr != m_Players.end(); ++itr)
    {
        if (itr->second.Team == Team)
        {
            Player* player = ObjectAccessor::FindPlayer(itr->first);
            if (player && player->IsAlive() && player->GetShapeshiftForm() != FORM_SPIRITOFREDEMPTION)
                ++count;
        }
    }
    return count;
}

int32 Battleground::GetObjectType(ObjectGuid guid)
{
    for (uint32 i = 0; i < BgObjects.size(); ++i)
        if (BgObjects[i] == guid)
            return i;
    TC_LOG_ERROR("bg.battleground", "Battleground::GetObjectType: player used gameobject ({}) which is not in internal data for BG (map: {}, instance id: {}), cheating?",
        guid.ToString(), m_MapId, m_InstanceID);
    return -1;
}

void Battleground::SetBgRaid(uint32 TeamID, Group* bg_raid)
{
    SetBgRaidBySlot(uint8(GetTeamIndexByTeamId(TeamID)), bg_raid);
}

uint8 Battleground::GetBgRaidSlot(ObjectGuid guid, uint32 team) const
{
    if (IsSquadMatch())
    {
        uint8 const squad = GetPlayerSquad(guid);
        if (squad < GetSquadCount())
            return squad;
    }

    return uint8(GetTeamIndexByTeamId(team));
}

void Battleground::SetBgRaidBySlot(uint8 slot, Group* bg_raid)
{
    if (slot >= MAX_BATTLEGROUND_SQUADS)
        return;

    Group*& old_raid = m_BgRaids[slot];
    if (old_raid)
        old_raid->SetBattlegroundGroup(nullptr);
    if (bg_raid)
        bg_raid->SetBattlegroundGroup(this);
    old_raid = bg_raid;
}

bool Battleground::UnlinkBgRaid(Group const* bg_raid)
{
    for (Group*& raid : m_BgRaids)
    {
        if (raid && raid == bg_raid)
        {
            raid = nullptr;
            return true;
        }
    }

    return false;
}

WorldSafeLocsEntry const* Battleground::GetClosestGraveyard(Player* player)
{
    return sObjectMgr->GetClosestGraveyard(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), player->GetTeam());
}

void Battleground::StartTimedAchievement(AchievementCriteriaTimedTypes type, uint32 entry)
{
    for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
        if (Player* player = ObjectAccessor::FindPlayer(itr->first))
            player->StartTimedAchievement(type, entry);
}

void Battleground::SetBracket(PvPDifficultyEntry const* bracketEntry)
{
    m_BracketId = bracketEntry->GetBracketId();
    SetLevelRange(bracketEntry->MinLevel, bracketEntry->MaxLevel);
}

void Battleground::RewardXPAtKill(Player* killer, Player* victim)
{
    if (sWorld->getBoolConfig(CONFIG_BG_XP_FOR_KILL) && killer && victim)
        killer->RewardPlayerAndGroupAtKill(victim, true);
}

uint32 Battleground::GetTeamScore(uint32 teamId) const
{
    if (teamId == TEAM_ALLIANCE || teamId == TEAM_HORDE)
        return m_TeamScores[teamId];

    TC_LOG_ERROR("bg.battleground", "GetTeamScore with wrong Team {} for BG {}", teamId, GetTypeID());
    return 0;
}

void Battleground::HandleAreaTrigger(Player* player, uint32 trigger)
{
    TC_LOG_DEBUG("bg.battleground", "Unhandled AreaTrigger {} in Battleground {}. Player coords (x: {}, y: {}, z: {})",
                   trigger, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
}

bool Battleground::CheckAchievementCriteriaMeet(uint32 criteriaId, Player const* /*source*/, Unit const* /*target*/, uint32 /*miscvalue1*/)
{
    TC_LOG_ERROR("bg.battleground", "Battleground::CheckAchievementCriteriaMeet: No implementation for criteria {}", criteriaId);
    return false;
}

uint8 Battleground::GetUniqueBracketId() const
{
    return GetMinLevel() / 10;
}

Battleground const* GetLeagueCombatBattleground(WorldObject const* object)
{
    Map* map = object ? object->FindMap() : nullptr;
    if (!map || !map->IsBattleground())
        return nullptr;
    Battleground const* bg = static_cast<BattlegroundMap*>(map)->GetBG();
    return bg && bg->UsesLeagueCombat() ? bg : nullptr;
}
