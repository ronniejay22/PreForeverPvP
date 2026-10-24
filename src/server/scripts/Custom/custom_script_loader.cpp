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

// This is where scripts' loading functions should be declared:

// The name of this function should match:
// void Add${NameOfDirectory}Scripts()
#include "BGReplay.cpp"

void AddAutoBalanceScripts();
void LoadHiddenItemsetBonuses();
void AddSC_mod_challenge_modes();
void AddSC_custom_hidden_itemset_bonus();
void AddSC_custom_zone_group_rules();
void AddSC_custom_warmode_zone_gate();
void AddSC_mod_pvp_titles();
void AddSC_custom_diremaul_beads();
void AddSC_custom_barracks_hardcore();
void AddSC_custom_bounty();
void AddSC_custom_weekly_deaths();
void AddSC_custom_bounty_board();
void AddSC_custom_notoriety();
void AddSC_custom_graceful_shutdown();
void AddSC_custom_bot_map_feed();
void AddSC_centurion_bot_stats_feed();
void AddSC_centurion_gm_online_feed();
void AddSC_custom_gurubashi_arena();
void AddSC_custom_depleted_mark_exchange();
void AddSC_custom_pvpve_dungeon();
void AddSC_npc_scarlet_chapel_queue();
void AddSC_npc_account_banker();
void AddSC_npc_arena_ladder();
void AddSC_npc_character_services();
void AddSC_go_lplus_jump_pad();
void AddSC_custom_game_lobby();
void AddSC_npc_transmogrifier();
void AddSC_custom_t1_set_bonuses();
void AddSC_custom_t2_warrior_paladin();
void AddSC_custom_t2_shaman_warlock();
void AddSC_custom_t2_druid_hunter();
void AddSC_custom_t2_priest_mage();
void AddSC_custom_t2_rogue_armor();
void AddSC_custom_wrapped_spells();
void AddSC_custom_southpark_nolife();
void AddSC_custom_sucky_demon();
void AddSC_custom_starter_booster();
void AddSC_custom_recruit_resolve();
void AddSC_GOMove_commandscript();
void AddSC_GOEditor();
void AddSC_rts_building();
void AddSC_custom_rift_progression();
void AddSC_custom_rift_items();
void AddSC_custom_los_blocker();
void AddSC_custom_player_collision();
void AddSC_custom_global_collision();
void AddSC_custom_client_attest();
void AddSC_violet_hold_boons();
void AddSC_custom_spell_propagate();
void AddSC_custom_tournament_mode();
void AddSC_custom_mokgora();
void AddSC_custom_info_command();
void AddSC_custom_name_markers();
void AddSC_custom_absorb_feed();
void AddSC_blood_beneath_arena();

void AddCustomScripts()
{
    AddSC_mod_challenge_modes();
    AddAutoBalanceScripts();
    LoadHiddenItemsetBonuses();
    AddSC_custom_hidden_itemset_bonus();
    AddSC_custom_los_blocker();
    AddSC_custom_player_collision();
    AddSC_custom_global_collision();
    AddSC_custom_client_attest();
    AddSC_custom_spell_propagate();
    AddSC_custom_tournament_mode();
    AddSC_custom_mokgora();
    AddSC_custom_info_command();
    AddSC_custom_name_markers();
    AddSC_custom_absorb_feed();
    AddBGReplayScripts();
    AddSC_custom_zone_group_rules();
    AddSC_custom_warmode_zone_gate();
    AddSC_mod_pvp_titles();
    AddSC_custom_diremaul_beads();
    AddSC_custom_barracks_hardcore();
    AddSC_custom_bounty();
    AddSC_custom_weekly_deaths();
    AddSC_custom_bounty_board();
    AddSC_custom_notoriety();
    AddSC_custom_graceful_shutdown();
    AddSC_custom_bot_map_feed();
    AddSC_centurion_bot_stats_feed();
    AddSC_centurion_gm_online_feed();
    AddSC_custom_gurubashi_arena();
    AddSC_custom_depleted_mark_exchange();
    AddSC_custom_pvpve_dungeon();
    AddSC_npc_scarlet_chapel_queue();
    AddSC_npc_account_banker();
    AddSC_npc_arena_ladder();
    AddSC_npc_character_services();
    AddSC_go_lplus_jump_pad();
    AddSC_custom_game_lobby();
    AddSC_npc_transmogrifier();
    AddSC_custom_t1_set_bonuses();
    AddSC_custom_t2_warrior_paladin();
    AddSC_custom_t2_shaman_warlock();
    AddSC_custom_t2_druid_hunter();
    AddSC_custom_t2_priest_mage();
    AddSC_custom_t2_rogue_armor();
    AddSC_custom_wrapped_spells();
    AddSC_custom_southpark_nolife();
    AddSC_custom_sucky_demon();
    AddSC_custom_starter_booster();
    AddSC_custom_recruit_resolve();
    AddSC_GOMove_commandscript();
    AddSC_GOEditor();
    AddSC_rts_building();
    AddSC_custom_rift_progression();
    AddSC_custom_rift_items();
    AddSC_violet_hold_boons();
    AddSC_blood_beneath_arena();
}
