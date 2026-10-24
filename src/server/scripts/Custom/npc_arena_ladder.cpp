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

// Brakka the Bookmaker (creature 990100, ops/sql/world-addons.sql): the rated
// arena standings in a gossip window. The first page offers the four brackets
// and the talker's own teams; a bracket lists its teams by rating, as
// `.arena ladder` does (ArenaTeamMgr::GetStandings); a team shows its roster.
// Read-only: nothing here changes a team.

#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Creature.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include <array>
#include <string>
#include <vector>

namespace
{
    enum ArenaLadderGossip : uint32
    {
        NPC_TEXT_ARENA_LADDER   = 990100,

        ACTION_HOME             = 1,
        ACTION_CLOSE            = 2,
        ACTION_BRACKET          = 100,      // + arena size
        ACTION_TEAM             = 1000000   // + arena team id
    };

    // A gossip window shows 32 lines at most; a bracket keeps two for its way back.
    constexpr std::size_t MAX_TEAMS_SHOWN = 30;

    std::array<uint8, 4> const Brackets = { ARENA_TEAM_2v2, ARENA_TEAM_3v3, ARENA_TEAM_4v4, ARENA_TEAM_5v5 };

    char const* ClassName(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_WARRIOR:      return "Warrior";
            case CLASS_PALADIN:      return "Paladin";
            case CLASS_HUNTER:       return "Hunter";
            case CLASS_ROGUE:        return "Rogue";
            case CLASS_PRIEST:       return "Priest";
            case CLASS_DEATH_KNIGHT: return "Death Knight";
            case CLASS_SHAMAN:       return "Shaman";
            case CLASS_MAGE:         return "Mage";
            case CLASS_WARLOCK:      return "Warlock";
            case CLASS_DRUID:        return "Druid";
            default:                 return "Adventurer";
        }
    }

    std::string Size(uint32 size)
    {
        return Trinity::StringFormat("{}v{}", size, size);
    }

    std::string Record(uint32 wins, uint32 games)
    {
        return Trinity::StringFormat("{}-{}", wins, games - wins);
    }

    // 1 for the top of its bracket.
    std::size_t PlaceOf(ArenaTeam const* team, std::vector<ArenaTeam*> const& standings)
    {
        for (std::size_t place = 0; place < standings.size(); ++place)
            if (standings[place] == team)
                return place + 1;
        return standings.size();
    }

    uint32 OwnTeamId(Player const* player, uint8 size)
    {
        return player->GetArenaTeamId(ArenaTeam::GetSlotByType(size));
    }
}

class npc_arena_ladder : public CreatureScript
{
public:
    npc_arena_ladder() : CreatureScript("npc_arena_ladder") { }

    struct npc_arena_ladderAI : public ScriptedAI
    {
        npc_arena_ladderAI(Creature* creature) : ScriptedAI(creature) { }

        bool OnGossipHello(Player* player) override
        {
            ShowHome(player);
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            if (action == ACTION_CLOSE)
                CloseGossipMenuFor(player);
            else if (action >= ACTION_TEAM)
                ShowTeam(player, action - ACTION_TEAM);
            else if (action > ACTION_BRACKET && action <= ACTION_BRACKET + ARENA_TEAM_5v5)
                ShowBracket(player, uint8(action - ACTION_BRACKET));
            else
                ShowHome(player);
            return true;
        }

    private:
        void ShowHome(Player* player)
        {
            ClearGossipMenuFor(player);

            for (uint8 size : Brackets)
            {
                std::size_t const teams = sArenaTeamMgr->GetStandings(size).size();
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Trinity::StringFormat("{} standings ({} teams)", Size(size), teams),
                    GOSSIP_SENDER_MAIN, ACTION_BRACKET + size);
            }

            // the talker's own teams, straight to their rosters
            for (uint8 size : Brackets)
            {
                ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(OwnTeamId(player, size));
                if (!team)
                    continue;

                std::vector<ArenaTeam*> const standings = sArenaTeamMgr->GetStandings(size);
                AddGossipItemFor(player, GOSSIP_ICON_TABARD, Trinity::StringFormat("Your {} team, {}: #{} of {}, {} rating",
                    Size(size), team->GetName(), PlaceOf(team, standings), standings.size(), team->GetRating()),
                    GOSSIP_SENDER_MAIN, ACTION_TEAM + team->GetId());
            }

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Goodbye.", GOSSIP_SENDER_MAIN, ACTION_CLOSE);
            SendGossipMenuFor(player, NPC_TEXT_ARENA_LADDER, me);
        }

        void ShowBracket(Player* player, uint8 size)
        {
            ClearGossipMenuFor(player);

            std::vector<ArenaTeam*> const standings = sArenaTeamMgr->GetStandings(size);
            uint32 const own = OwnTeamId(player, size);
            if (standings.empty())
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Trinity::StringFormat("No {} team has been made yet.", Size(size)),
                    GOSSIP_SENDER_MAIN, ACTION_HOME);

            for (std::size_t place = 0; place < standings.size(); ++place)
            {
                ArenaTeam* team = standings[place];
                if (place >= MAX_TEAMS_SHOWN && team->GetId() != own)
                    continue;

                ArenaTeamStats const& stats = team->GetStats();
                AddGossipItemFor(player, team->GetId() == own ? GOSSIP_ICON_TABARD : GOSSIP_ICON_DOT,
                    Trinity::StringFormat("{}. {} - {} ({}){}", place + 1, team->GetName(), stats.Rating,
                        Record(stats.SeasonWins, stats.SeasonGames), team->GetId() == own ? "  <- you" : ""),
                    GOSSIP_SENDER_MAIN, ACTION_TEAM + team->GetId());
            }

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to the brackets.", GOSSIP_SENDER_MAIN, ACTION_HOME);
            SendGossipMenuFor(player, NPC_TEXT_ARENA_LADDER, me);
        }

        void ShowTeam(Player* player, uint32 teamId)
        {
            ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
            if (!team)
            {
                ShowHome(player);
                return;
            }

            ClearGossipMenuFor(player);

            uint32 const size = team->GetType();
            std::vector<ArenaTeam*> const standings = sArenaTeamMgr->GetStandings(size);
            ArenaTeamStats const& stats = team->GetStats();
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, Trinity::StringFormat("{}: #{} in {}, {} rating, {} this season",
                team->GetName(), PlaceOf(team, standings), Size(size), stats.Rating, Record(stats.SeasonWins, stats.SeasonGames)),
                GOSSIP_SENDER_MAIN, ACTION_TEAM + teamId);

            for (auto itr = team->m_membersBegin(); itr != team->m_membersEnd(); ++itr)
                AddGossipItemFor(player, GOSSIP_ICON_DOT, Trinity::StringFormat("{} - {}, {} personal ({})", itr->Name,
                    ClassName(itr->Class), itr->PersonalRating, Record(itr->SeasonWins, itr->SeasonGames)),
                    GOSSIP_SENDER_MAIN, ACTION_TEAM + teamId);

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, Trinity::StringFormat("Back to the {} standings.", Size(size)),
                GOSSIP_SENDER_MAIN, ACTION_BRACKET + size);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to the brackets.", GOSSIP_SENDER_MAIN, ACTION_HOME);
            SendGossipMenuFor(player, NPC_TEXT_ARENA_LADDER, me);
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_arena_ladderAI(creature);
    }
};

void AddSC_npc_arena_ladder()
{
    new npc_arena_ladder();
}
