-- World rows of our own on top of the Centurion snapshot, kept here so a fresh
-- import puts them back. Applied by ops/import-db.sh; safe to apply again.
-- Ids from 990000 up are ours, clear of Centurion's 9xxxxx custom ranges.

-- 990100: Brakka the Bookmaker, the arena ladder keeper
-- (src/server/scripts/Custom/npc_arena_ladder.cpp). Steamwheedle Sam's look
-- and stats (19856, the Arena Organizer), gossip only, at the realm's cap.
DELETE FROM `creature_template` WHERE `entry` = 990100;
CREATE TEMPORARY TABLE `tmp_arena_ladder_keeper` SELECT * FROM `creature_template` WHERE `entry` = 19856;
UPDATE `tmp_arena_ladder_keeper` SET
    `entry` = 990100,
    `name` = 'Brakka the Bookmaker',
    `subname` = 'Arena Standings',
    `minlevel` = 60,
    `maxlevel` = 60,
    `npcflag` = 1,
    `gossip_menu_id` = 0,
    `AIName` = '',
    `ScriptName` = 'npc_arena_ladder';
INSERT INTO `creature_template` SELECT * FROM `tmp_arena_ladder_keeper`;
DROP TEMPORARY TABLE `tmp_arena_ladder_keeper`;

DELETE FROM `npc_text` WHERE `ID` = 990100;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `BroadcastTextID0`, `lang0`, `Probability0`) VALUES
(990100,
 'Every win and every loss in the pits goes in my ledger, $N. Bots, champions, nobodies - I write them all down. Which bracket do you want to see?',
 'Every win and every loss in the pits goes in my ledger, $N. Bots, champions, nobodies - I write them all down. Which bracket do you want to see?',
 0, 0, 1);
