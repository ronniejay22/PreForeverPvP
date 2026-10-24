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

#ifndef __BATTLEGROUND_H
#define __BATTLEGROUND_H

#include "ArenaScore.h"
#include "DBCEnums.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "SharedDefines.h"
#include <algorithm>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

namespace WorldPackets
{
    namespace WorldState
    {
        class InitWorldStates;
    }
}

class BattlegroundMap;
class Creature;
class GameObject;
class Group;
class Player;
class Transport;
class Unit;
class WorldObject;
class WorldPacket;
class WorldSession;

struct BattlegroundScore;
struct PvPDifficultyEntry;
struct WorldSafeLocsEntry;

enum class BattlegroundNodeStatus : uint8
{
    Neutral = 0,
    FriendlyControlled,
    EnemyControlled,
    FriendlyContested,
    EnemyContested,
    FriendlyUnderAttack
};

struct BattlegroundNodeObjective
{
    uint32 NodeId = 0;
    BattlegroundNodeStatus Status = BattlegroundNodeStatus::Neutral;
    Position Location;
    ObjectGuid BannerGuid = ObjectGuid::Empty;
    // A base held by standing on it rather than by clicking a banner (the Eye
    // of the Storm towers): the radius around Location a body counts from.
    // Zero for a banner base, which is taken through BannerGuid.
    float CaptureRadius = 0.0f;
};

// The objectives below describe a battleground to anything that has to play it
// without knowing which one it is - today the playerbot module. Each is
// answered for the asking player's team, like the node objectives above.

// A gate or wall that only siege damage brings down (Strand of the Ancients,
// Isle of Conquest).
struct BattlegroundSiegeObjective
{
    uint32 Id = 0;
    ObjectGuid GateGuid = ObjectGuid::Empty;
    // Open ground in front of the gate on the attackers' side, where a charge
    // is planted, a ram stands, and the defenders meet them.
    Position Location;
    // The asking player's team holds it.
    bool Friendly = false;
    // Bringing it down is useful now: it stands, and nothing still standing in
    // front of it has to fall first.
    bool Breachable = false;
};

// Explosives the asking player's team can plant against an enemy gate.
struct BattlegroundSiegeChargeInfo
{
    // The charge, used from the bag next to the gate.
    uint32 ItemId = 0;
    // The game objects that hand one out to this player right now.
    GuidVector Sources;
};

// A commander whose death decides or swings the match.
struct BattlegroundBossObjective
{
    uint32 Id = 0;
    ObjectGuid BossGuid = ObjectGuid::Empty;
    // The asking player's team fights for it.
    bool Friendly = false;
    // The attackers should go for it now.
    bool Engageable = false;
    // A spot to reach before heading for the boss - the gate the attackers
    // broke into its keep through - when the straight route runs into a wall
    // still standing. Ignored while HasApproach is false.
    bool HasApproach = false;
    Position Approach;
};

// A one-way shortcut used by clicking it: a keep teleporter, a defender portal.
struct BattlegroundTeleporter
{
    ObjectGuid GoGuid = ObjectGuid::Empty;
    // Where the user stands to click it, and where it puts them.
    Position Entrance;
    Position Exit;
};

// A ledge players leave by jumping off it, which the navmesh does not join to
// the ground below: Eye of the Storm's start platforms. Somebody on the island
// - within IslandRadius of IslandCentre, horizontally, and no lower than
// IslandMinZ - leaves by Edge and comes down at Landing.
struct BattlegroundDropExit
{
    Position IslandCentre;
    float IslandRadius = 0.0f;
    float IslandMinZ = 0.0f;
    Position Edge;
    Position Landing;
};

// A battleground that plays each bot itself, order by order - a MOBA's laning,
// where the bot's place and its target come from the match (its lane, its wave,
// the turrets) rather than from objectives. Asked every bot tick
// (Battleground::GetBotOrders); a bot with orders does nothing else.
struct BattlegroundBotOrders
{
    Position Post;              // where to be: walk there, hold there
    ObjectGuid Target;          // what to fight at the post, empty for nothing
    bool Retreat = false;       // going home: do not stop to fight
    uint32 RecallSpell = 0;     // cast this on itself now (safe to channel), 0 for none
};

// Where a team is put when the gates open, for a start nothing walkable joins
// to the fight: The Twisting Nether's teams wait on two balconies of the ship,
// across the void from the arena platform. Whoever is still within Radius of
// the team's start once the gates have been open DelayMs is moved to Exit,
// with the pets and guardians they brought. BotsOnly limits it to playerbots,
// for a start people leave on their own. Rows in `battleground_start_exit`,
// one per battleground type and team, read alongside the templates.
struct BattlegroundStartExit
{
    bool Enabled = false;
    Position Exit;
    float Radius = 25.0f;
    uint32 DelayMs = 0;
    bool BotsOnly = false;
};

// Where a match's combatants - and the pets, guardians and totems they brought -
// are allowed to be once the gates are open, and where they go when they are not.
//
// The client keeps a player out of the walls; nothing keeps the server's own
// movers there. A pet knocked through a crack, a bot that missed the elevator
// (GameObject::BoardLiftRiders carries the ones on it), a player who fell
// through a hole in the floor: each is returned rather than left stranded under
// the arena for the rest of the match.
//
// Rows in `battleground_bounds`, one per battleground type; a type with no row
// gets none of this. Read alongside the templates, so `.reload
// battleground_template` picks up an edit.
struct BattlegroundBoundaryPoint
{
    float X;
    float Y;
};

struct BattlegroundBounds
{
    bool Enabled = false;
    // Anyone below this has fallen out of the arena.
    float MinZ = 0.0f;
    // Where to look down from for the floor at the point someone is returned
    // to. 0 = do not look: use the safe point (or its height, see KeepXY).
    float ProbeZ = 0.0f;
    // Keep the returned unit's own X/Y and only correct its height (SafeZ),
    // for a unit that is in the right place but on the wrong floor.
    bool KeepXY = false;
    Position SafePosition;
    // Time after the gates open before any of this applies.
    uint32 GraceMs = 0;
    // Optional outline, in world X/Y. Leaving it returns you just inside it.
    std::vector<BattlegroundBoundaryPoint> Boundary;
};

// A match fought by more than two teams: Baradin Hold: Four Corners puts up to
// four squads in the arena, one per corner, every one of them hostile to the
// others.
//
// The engine underneath stays two-sided. Every queue list, invite counter and
// player count is per side, and so is everything the client is told, so squads
// are a layer over the sides rather than a third and fourth side: even squads
// sit on the Alliance side and odd ones on the Horde side, which keeps the side
// counts honest. What makes them squads:
//
//   * friend or foe is decided by squad (WorldObject::GetReactionTo),
//   * each squad is its own raid group,
//   * each squad starts in its own corner,
//   * the match is won by the last squad standing (Arena::CheckWinConditions),
//   * every client is shown its own squad against everyone else: the faction
//     and arena-team byte of each participant are sent per viewer (Unit's
//     values update), so nameplates, the client's own spell-target checks and
//     the arena frames all agree with the server without a client patch.
//
// A battleground type is a squad match when `battleground_squad_start` has rows
// for it: one per squad, numbered from 0, read with the templates.
uint8 constexpr MAX_BATTLEGROUND_SQUADS = 4;
uint8 constexpr BATTLEGROUND_NO_SQUAD = 0xFF;

struct BattlegroundSquadStart
{
    Position Start;
    // How the squad is named to everyone, e.g. "Blue".
    std::string Name;
    // The team flag its members carry in an arena, in place of the side's
    // gold or green one (Arena::AddPlayer). 0 = the side's.
    uint32 FlagSpell = 0;
};

enum class BattlegroundCustomWeather : uint8
{
    Normal,
    Clear,
    Rain,
    Snow,
    Sandstorm,
    Thunderstorm,
    Fog,
    Max
};

struct BattlegroundCustomRules
{
    uint32 FlagCaptureLimit = 0;
    uint32 ResourceLimit = 0;
    uint32 ResourceGainPercent = 100;
    uint32 DeathmatchKillLimit = 0;
    uint32 ResurrectionIntervalMs = 0;
    uint32 NodeFlagCaptureTimeMs = 0;
    uint32 NodeBaseCaptureTimeMs = 0;
    bool ShowEnemyFlagOnMap = true;
    bool ShowAllyFlagOnMap = true;
    // Fight this one under the tournament's rules: the gear swap and the
    // consumable ban a tournament-pool match runs under (TournamentMode.h).
    // The lobby turns it into SetTournamentPool on the match it creates, which
    // is what every one of those rules actually reads. On unless the host turns
    // it off.
    bool TournamentRules = true;
    BattlegroundCustomWeather Weather = BattlegroundCustomWeather::Normal;
    // Summoner's Rift. Last hitting: minion gold goes only to the player who
    // lands the killing blow (off: it is shared by the killing side's players
    // nearby). Denying (off unless the lobby turns it on): a minion under half
    // health turns hostile to everyone, so its own side can finish it and nobody
    // is paid for it. Shared experience: the whole team earns every award
    // together, as in Heroes of the Storm.
    bool RiftLastHitGold = true;
    bool RiftDenying = false;
    bool RiftSharedExperience = false;
};

enum BattlegroundDesertionType
{
    BG_DESERTION_TYPE_LEAVE_BG        = 0, // player leaves the BG
    BG_DESERTION_TYPE_OFFLINE         = 1, // player is kicked from BG because offline
    BG_DESERTION_TYPE_LEAVE_QUEUE     = 2, // player is invited to join and refuses to do it
    BG_DESERTION_TYPE_NO_ENTER_BUTTON = 3, // player is invited to join and do nothing (time expires)
    BG_DESERTION_TYPE_INVITE_LOGOUT   = 4, // player is invited to join and logs out
};

enum BattlegroundCriteriaId
{
    BG_CRITERIA_CHECK_RESILIENT_VICTORY,
    BG_CRITERIA_CHECK_SAVE_THE_DAY,
    BG_CRITERIA_CHECK_EVERYTHING_COUNTS,
    BG_CRITERIA_CHECK_AV_PERFECTION,
    BG_CRITERIA_CHECK_DEFENSE_OF_THE_ANCIENTS,
    BG_CRITERIA_CHECK_NOT_EVEN_A_SCRATCH,
};

enum BattlegroundBroadcastTexts
{
    BG_TEXT_ALLIANCE_WINS       = 10633,
    BG_TEXT_HORDE_WINS          = 10634,

    BG_TEXT_START_TWO_MINUTES   = 18193,
    BG_TEXT_START_ONE_MINUTE    = 18194,
    BG_TEXT_START_HALF_MINUTE   = 18195,
    BG_TEXT_START_FIFTEEN_SEC   = 77867,
    BG_TEXT_BATTLE_HAS_BEGUN    = 18196,
};

enum BattlegroundSounds
{
    SOUND_HORDE_WINS                = 8454,
    SOUND_ALLIANCE_WINS             = 8455,
    SOUND_BG_START                  = 3439,
    SOUND_BG_START_L70ETC           = 11803
};

enum BattlegroundQuests
{
    SPELL_WS_QUEST_REWARD           = 43483,
    SPELL_AB_QUEST_REWARD           = 43484,
    SPELL_AV_QUEST_REWARD           = 43475,
    SPELL_AV_QUEST_KILLED_BOSS      = 23658,
    SPELL_EY_QUEST_REWARD           = 43477,
    SPELL_SA_QUEST_REWARD           = 61213,
    SPELL_AB_QUEST_REWARD_4_BASES   = 24061,
    SPELL_AB_QUEST_REWARD_5_BASES   = 24064
};

enum BattlegroundMarks
{
    SPELL_WS_MARK_LOSER             = 24950,
    SPELL_WS_MARK_WINNER            = 24951,
    SPELL_AB_MARK_LOSER             = 24952,
    SPELL_AB_MARK_WINNER            = 24953,
    SPELL_AV_MARK_LOSER             = 24954,
    SPELL_AV_MARK_WINNER            = 24955,
    SPELL_SA_MARK_WINNER            = 61160,
    SPELL_SA_MARK_LOSER             = 61159,
    ITEM_AV_MARK_OF_HONOR           = 20560,
    ITEM_WS_MARK_OF_HONOR           = 20558,
    ITEM_AB_MARK_OF_HONOR           = 20559,
    ITEM_EY_MARK_OF_HONOR           = 29024,
    ITEM_SA_MARK_OF_HONOR           = 42425
};

enum BattlegroundMarksCount
{
    ITEM_WINNER_COUNT               = 3,
    ITEM_LOSER_COUNT                = 1
};

enum BattlegroundCreatures
{
    BG_CREATURE_ENTRY_A_SPIRITGUIDE      = 13116,           // alliance
    BG_CREATURE_ENTRY_H_SPIRITGUIDE      = 13117            // horde
};

enum BattlegroundSpells
{
    SPELL_WAITING_FOR_RESURRECT     = 2584,                 // Waiting to Resurrect
    SPELL_SPIRIT_HEAL_CHANNEL       = 22011,                // Spirit Heal Channel
    SPELL_SPIRIT_HEAL               = 22012,                // Spirit Heal
    SPELL_RESURRECTION_VISUAL       = 24171,                // Resurrection Impact Visual
    SPELL_ARENA_PREPARATION         = 32727,                // use this one, 32728 not correct
    SPELL_PREPARATION               = 44521,                // Preparation
    SPELL_INSTANT_CAST              = 45813,                // instant cast spells in arena
    SPELL_SPIRIT_HEAL_MANA          = 44535,                // Spirit Heal
    SPELL_RECENTLY_DROPPED_FLAG     = 42792,                // Recently Dropped Flag
    SPELL_AURA_PLAYER_INACTIVE      = 43681,                // Inactive
    // Centurion "Spectator Mode": hidden SCREEN_EFFECT 0, for the released dead of
    // a battleground that dies like an arena (Player::BuildPlayerRepop).
    SPELL_LAST_STAND_SPECTATOR      = 92335
};

enum BattlegroundTimeIntervals
{
    CHECK_PLAYER_POSITION_INVERVAL  = 1000,                 // ms
    BOUNDS_CHECK_INTERVAL           = 500,                  // ms, see BattlegroundBounds
    RESURRECTION_INTERVAL           = 30000,                // ms
    NODE_FLAG_INTERACTION_TIME      = 10000,                // ms, AB/BFG banner click unless a custom game's lobby sets its own
    //REMIND_INTERVAL                 = 10000,                // ms
    INVITATION_REMIND_TIME          = 10000,                // ms
    INVITE_ACCEPT_WAIT_TIME         = 30000,                // ms
    TIME_TO_AUTOREMOVE              = 60000,               // ms
    MAX_OFFLINE_TIME                = 300,                  // secs
    RESPAWN_ONE_DAY                 = 86400,                // secs
    RESPAWN_IMMEDIATELY             = 0,                    // secs
    BUFF_RESPAWN_TIME               = 180                   // secs
};

enum BattlegroundStartTimeIntervals
{
    BG_START_DELAY_2M               = 120000,               // ms (2 minutes)
    BG_START_DELAY_1M               = 60000,                // ms (1 minute)
    BG_START_DELAY_30S              = 30000,                // ms (30 seconds)
    BG_START_DELAY_20S              = 20000,                // ms (20 seconds)
    BG_START_DELAY_15S              = 15000,                // ms (15 seconds) Used only in arena
    BG_START_DELAY_10S              = 10000,                // ms (10 seconds)
    BG_START_DELAY_NONE             = 0                     // ms
};

enum BattlegroundBuffObjects
{
    BG_OBJECTID_SPEEDBUFF_ENTRY     = 179871,
    BG_OBJECTID_REGENBUFF_ENTRY     = 179904,
    BG_OBJECTID_BERSERKERBUFF_ENTRY = 179905
};

const uint32 Buff_Entries[3] = { BG_OBJECTID_SPEEDBUFF_ENTRY, BG_OBJECTID_REGENBUFF_ENTRY, BG_OBJECTID_BERSERKERBUFF_ENTRY };

enum BattlegroundStatus
{
    STATUS_NONE         = 0,                                // first status, should mean bg is not instance
    STATUS_WAIT_QUEUE   = 1,                                // means bg is empty and waiting for queue
    STATUS_WAIT_JOIN    = 2,                                // this means, that BG has already started and it is waiting for more players
    STATUS_IN_PROGRESS  = 3,                                // means bg is running
    STATUS_WAIT_LEAVE   = 4                                 // means some faction has won BG and it is ending
};

struct BattlegroundPlayer
{
    time_t OfflineRemoveTime;                              // for tracking and removing offline players from queue after 5 minutes
    uint32 Team;                                           // Player's team
    uint32 JoinStartTime = 0;                              // GetStartTime() when they first arrived
};

struct BattlegroundObjectInfo
{
    BattlegroundObjectInfo() : object(nullptr), timer(0), spellid(0) { }

    GameObject  *object;
    int32       timer;
    uint32      spellid;
};

enum ArenaType
{
    ARENA_TYPE_2v2          = 2,
    ARENA_TYPE_3v3          = 3,
    ARENA_TYPE_4v4          = 4,
    ARENA_TYPE_5v5          = 5
};

enum BattlegroundStartingEvents
{
    BG_STARTING_EVENT_NONE  = 0x00,
    BG_STARTING_EVENT_1     = 0x01,
    BG_STARTING_EVENT_2     = 0x02,
    BG_STARTING_EVENT_3     = 0x04,
    BG_STARTING_EVENT_4     = 0x08
};

enum BattlegroundStartingEventsIds
{
    BG_STARTING_EVENT_FIRST     = 0,
    BG_STARTING_EVENT_SECOND    = 1,
    BG_STARTING_EVENT_THIRD     = 2,
    BG_STARTING_EVENT_FOURTH    = 3
};
#define BG_STARTING_EVENT_COUNT 4

#define BG_AWARD_ARENA_POINTS_MIN_LEVEL 71
#define ARENA_TIMELIMIT_POINTS_LOSS    -16

/*
This class is used to:
1. Add player to battleground
2. Remove player from battleground
3. some certain cases, same for all battlegrounds
4. It has properties same for all battlegrounds
*/
class TC_GAME_API Battleground
{
    public:
        Battleground();
        virtual ~Battleground();

        void Update(uint32 diff);

        virtual bool SetupBattleground()                    // must be implemented in BG subclass
        {
            return true;
        }
        virtual void Reset();                               // resets all common properties for battlegrounds, must be implemented and called in BG subclass
        virtual void StartingEventCloseDoors() { }
        virtual void StartingEventOpenDoors() { }
        virtual void ResetBGSubclass() { }                  // must be implemented in BG subclass

        virtual void DestroyGate(Player* /*player*/, GameObject* /*go*/) { }

        /* achievement req. */
        virtual bool IsAllNodesControlledByTeam(uint32 /*team*/) const { return false; }
        void StartTimedAchievement(AchievementCriteriaTimedTypes type, uint32 entry);
        virtual bool CheckAchievementCriteriaMeet(uint32 /*criteriaId*/, Player const* /*player*/, Unit const* /*target*/ = nullptr, uint32 /*miscvalue1*/ = 0);

        /* Battleground */
        // Get methods:
        std::string const& GetName() const  { return m_Name; }
        BattlegroundTypeId GetTypeID(bool GetRandom = false) const { return GetRandom ? m_RandomTypeID : m_TypeID; }
        BattlegroundBracketId GetBracketId() const { return m_BracketId; }
        uint32 GetInstanceID() const        { return m_InstanceID; }
        BattlegroundStatus GetStatus() const { return m_Status; }
        uint32 GetClientInstanceID() const  { return m_ClientInstanceID; }
        uint32 GetStartTime() const         { return m_StartTime; }
        uint32 GetEndTime() const           { return m_EndTime; }
        uint32 GetLastResurrectTime() const { return m_LastResurrectTime; }
        uint32 GetMaxPlayers() const        { return m_MaxPlayers; }
        uint32 GetMinPlayers() const        { return m_MinPlayers; }

        uint32 GetMinLevel() const          { return m_LevelMin; }
        uint32 GetMaxLevel() const          { return m_LevelMax; }

        uint32 GetMaxPlayersPerTeam() const { return m_MaxPlayersPerTeam; }
        uint32 GetMinPlayersPerTeam() const { return m_MinPlayersPerTeam; }

        int32 GetStartDelayTime() const     { return m_StartDelayTime; }
        uint8 GetArenaType() const          { return m_ArenaType; }
        PvPTeamId GetWinner() const { return _winnerTeamId; }
        uint32 GetScriptId() const          { return ScriptId; }
        uint32 GetFightId() const { return m_FightId; }
        uint32 GetReplayId() const { return m_ReplayId; }
        bool IsReplay() const { return m_IsReplay; }
        void SetReplay(bool isReplay) { m_IsReplay = isReplay; }
        void SetFightId(uint32 FightId) { m_FightId = FightId; }
        void SetReplayId(uint32 ReplayId) { m_ReplayId = ReplayId; }
        uint32 GetBonusHonorFromKill(uint32 kills) const;
        bool IsRandom() const { return m_IsRandom; }

        // Set methods:
        void SetName(std::string const& name) { m_Name = name; }
        void SetTypeID(BattlegroundTypeId TypeID) { m_TypeID = TypeID; }
        void SetRandomTypeID(BattlegroundTypeId TypeID) { m_RandomTypeID = TypeID; }
        //here we can count minlevel and maxlevel for players
        void SetBracket(PvPDifficultyEntry const* bracketEntry);
        void SetInstanceID(uint32 InstanceID) { m_InstanceID = InstanceID; }
        void SetStatus(BattlegroundStatus Status) { m_Status = Status; }
        void SetClientInstanceID(uint32 InstanceID) { m_ClientInstanceID = InstanceID; }
        void SetStartTime(uint32 Time)      { m_StartTime = Time; }
        void SetEndTime(uint32 Time)        { m_EndTime = Time; }
        void SetLastResurrectTime(uint32 Time) { m_LastResurrectTime = Time; }
        void SetMaxPlayers(uint32 MaxPlayers) { m_MaxPlayers = MaxPlayers; }
        void SetMinPlayers(uint32 MinPlayers) { m_MinPlayers = MinPlayers; }
        void SetLevelRange(uint32 min, uint32 max) { m_LevelMin = min; m_LevelMax = max; }
        void SetRated(bool state)           { m_IsRated = state; }
        void SetArenaType(uint8 type)       { m_ArenaType = type; }
        void SetArenaorBGType(bool _isArena) { m_IsArena = _isArena; }
        void SetWinner(PvPTeamId winnerTeamId) { _winnerTeamId = winnerTeamId; }
        void SetScriptId(uint32 scriptId)   { ScriptId = scriptId; }

        void ModifyStartDelayTime(int diff) { m_StartDelayTime -= diff; }
        void SetStartDelayTime(int Time)    { m_StartDelayTime = Time; }
        bool SkipStartDelay();
        // Spawns the battleground's objects and closes its doors. Called as
        // soon as the map exists, so the gates are already shut when the first
        // player's loading screen ends; waiting for a player to be inside (and
        // then for the next once-a-second battleground tick) left the doorways
        // open long enough to walk through. Idempotent.
        bool PrepareStartingArea();

        void SetMaxPlayersPerTeam(uint32 MaxPlayers) { m_MaxPlayersPerTeam = MaxPlayers; }
        void SetMinPlayersPerTeam(uint32 MinPlayers) { m_MinPlayersPerTeam = MinPlayers; }

        void AddToBGFreeSlotQueue();                        //this queue will be useful when more battlegrounds instances will be available
        void RemoveFromBGFreeSlotQueue();                   //this method could delete whole BG instance, if another free is available

        void DecreaseInvitedCount(uint32 team)
        {
            if (team == ALLIANCE)
            {
                if (m_InvitedAlliance > 0)
                    --m_InvitedAlliance;
            }
            else if (m_InvitedHorde > 0)
                --m_InvitedHorde;
        }
        void IncreaseInvitedCount(uint32 team)      { (team == ALLIANCE) ? ++m_InvitedAlliance : ++m_InvitedHorde; }

        void SetRandom(bool isRandom) { m_IsRandom = isRandom; }
        uint32 GetInvitedCount(uint32 team) const   { return (team == ALLIANCE) ? m_InvitedAlliance : m_InvitedHorde; }
        bool HasFreeSlots() const;
        uint32 GetFreeSlotsForTeam(uint32 Team) const;

        bool isArena() const        { return m_IsArena; }

        // Does death here follow the arena rules rather than the battleground
        // ones? Arenas have no spirit guides, no resurrect queue and no way back
        // into the fight: you stay on your corpse until the round ends. A
        // battleground that is built as a last-stand wants exactly that even
        // though it is not an arena, so the question is asked by name instead of
        // being spelled isArena() at each of the places that care.
        virtual bool UsesArenaDeathRules() const { return isArena(); }

        // Take every form of combat control off a player who is leaving a match.
        // Static because not every exit has a Battleground to call it on: a
        // custom-lobby return can happen after the match object is already gone.
        static void ClearCombatControl(Player* player);
        bool isBattleground() const { return !m_IsArena; }
        bool isRated() const        { return m_IsRated; }
        bool IsCustomGame() const   { return m_IsCustomGame; }
        // Honor paid to a participant during the match - objectives, kills,
        // anything through Player::RewardHonor - counted before it is turned
        // into experience below the cap. The end-of-match payout is not
        // included; EndBattleground reads this before paying it.
        void NoteMatchHonor(ObjectGuid guid, uint32 honor) { m_MatchHonor[guid] += honor; }
        uint32 GetMatchHonor(ObjectGuid guid) const
        {
            auto itr = m_MatchHonor.find(guid);
            return itr != m_MatchHonor.end() ? itr->second : 0;
        }
        // A public match padded with transient clones by the playerbot module
        // (see PlayerbotBgFillDriver). In such a match the clones are bots for
        // every purpose the queue and the empty-match rules care about, so a
        // queued person displaces one and the match ends when the last person
        // is gone. Ordinary matches keep the stock reading: a transient clone
        // there (an Obsidian Colosseum mirror, a Violet Hold wave) is somebody
        // else's business and counts as a participant.
        bool IsBotFillMatch() const { return m_IsBotFillMatch; }
        // Seated like a bot-filled skirmish: people from the queue take its
        // clones' seats while it prepares (BattlegroundQueue::OfferPreparationSeats)
        // and nobody joins once its gates are open - no free-slot backfill.
        virtual bool TakesSeatsInPreparationOnly() const { return false; }
        // Whether the queue may still put somebody into this match's free slots.
        bool TakesQueueBackfill() const
        {
            if (isArena() && IsBotFillMatch())
                return false;
            return !TakesSeatsInPreparationOnly() || (!IsBotFillMatch() && GetStatus() == STATUS_WAIT_JOIN);
        }
        void SetBotFillMatch(bool enabled) { m_IsBotFillMatch = enabled; }
        // A rated arena the queue gave one arena team to play against clones
        // (BattlegroundQueue::StartBotFilledRatedMatches): the side that has
        // the team, ALLIANCE or HORDE, else 0 - two real teams paired
        // short-handed are both teams and are rated as stock.
        uint32 GetBotFillRatedTeam() const
        {
            if (!isArena() || !isRated() || !IsBotFillMatch() || IsSquadMatch())
                return 0;
            bool const alliance = m_ArenaTeamIds[TEAM_ALLIANCE] != 0;
            bool const horde = m_ArenaTeamIds[TEAM_HORDE] != 0;
            return alliance == horde ? 0 : (alliance ? ALLIANCE : HORDE);
        }
        // Whether a clone has ever been seated opposite that team; until one
        // has, the match is no contest and its result counts for nothing.
        bool HasBotFillOpponentsFielded() const { return m_BotFillOpponentsFielded; }
        // Whether a person - not a bot - still holds a seat on `team`; one who
        // dropped keeps theirs until the offline timer gives up on them.
        bool HasPersonOnTeam(uint32 team) const;
        // A match the queue made for people who would not have bots (a squad
        // match of real squads only): the bot fill leaves its empty seats empty.
        bool IsBotFillRefused() const { return m_IsBotFillRefused; }
        void SetBotFillRefused(bool refused) { m_IsBotFillRefused = refused; }
        // Until its gates open, a bot-filled match keeps its clones' seats open
        // to the queue (BattlegroundQueue::OfferPreparationSeats). The queue
        // reserves one clone per person it invites; the clone stands down when
        // that person arrives, and the preparation countdown starts over. The
        // countdown is held while anyone with a reservation is still on the way
        // in, which the invite's own expiry bounds.
        //
        // Seats a person could still take from a clone on `team`: its clones
        // less the ones already promised. Zero once the gates are open.
        uint32 CountPreparationSeatsHeldByClones(uint32 team);
        // Seats on `team` that nobody holds yet, until the gates open: no
        // person in or invited, no bot in, and no clone the bot fill has asked
        // for and is still loading (SetBotFillSeatsOnTheWay). A person can be
        // invited straight into one; no clone has to stand down for them.
        uint32 CountUnclaimedPreparationSeats(uint32 team) const;
        // How many clones the bot fill has requested for `team` that have not
        // arrived yet (PlayerbotBgFillDriver keeps it current).
        void SetBotFillSeatsOnTheWay(uint32 team, uint32 count);
        void ReservePreparationSeat(ObjectGuid guid, uint32 team) { m_PreparationSeatTakers[guid] = team; }
        bool HasPreparationSeatReservation(ObjectGuid guid) const { return m_PreparationSeatTakers.count(guid) != 0; }
        // CENTURION queue pool (Miscellaneous/TournamentMode.h): the queue only
        // ever seats tournament-pool groups in a tournament-pool match and world
        // groups in a world match. Set by BattlegroundQueue when it creates the
        // match; everything else (custom games, templates) stays world.
        bool IsTournamentPool() const { return m_IsTournamentPool; }
        void SetTournamentPool(bool tournament) { m_IsTournamentPool = tournament; }
        // Whether the tournament's rules hold in this match - no talent changes,
        // the armour lock, the consumable ban, free reagents (Tournament::
        // IsInTournamentMatch). A tournament-pool match plays by them, unless the
        // battleground has its own for everything a character brings in and
        // spends (Summoner's Rift), which keeps the pool for its matchmaking only.
        virtual bool PlaysByTournamentRules() const { return IsTournamentPool(); }
        // True when the session belongs to a bot occupant of this match rather
        // than a person: any socketless virtual session, and a transient
        // in-memory clone when the match is bot-filled. The human accounting,
        // the free-slot queue and the displacement-on-invite path all ask this
        // so the two kinds of bot are treated alike. A null session is not a bot.
        bool IsBotParticipantSession(WorldSession const* session) const;
        // The name clients know a participant by. A transient clone (bot fill,
        // "Dark" mirror) plays under an internal "Obcm..." Player name and is
        // shown under the one in the character cache, and every name query
        // answers with the family name after it ("Jahzir Frostmask",
        // Miscellaneous/Surnames.h), so any name sent in an addon payload must
        // come from here or the client can neither show it nor /targetexact it.
        static std::string GetPlayerDisplayName(Player const* player);
        // The other direction: the transient clone seated in this match that
        // clients know as `name`. Nothing indexes that name - the name map holds
        // the internal one, and the cache entry has no name index because the
        // clone's source character usually owns the name - so a by-name request
        // aimed at a clone (a whisper) finds nobody, or the source out in the
        // world, unless it asks the roster. Compared without case: client names
        // arrive normalized. A clone on `preferredTeam` wins a tie.
        Player* FindTransientPlayerByDisplayName(std::string_view name, uint32 preferredTeam) const;
        void ConfigureCustomGame(BattlegroundCustomRules const& rules) { m_IsCustomGame = true; m_CustomRules = rules; }
        void SetCustomGameBotOnlyPreparation(bool enabled) { m_CustomGameBotOnlyPreparation = enabled; }
        bool HasCustomGameBotOnlyPreparation() const { return m_CustomGameBotOnlyPreparation; }
        void SetCustomGamePendingCloneCount(uint32 count) { m_CustomGamePendingCloneCount = count; }
        void ResolveCustomGamePendingClone()
        {
            if (m_CustomGamePendingCloneCount)
                --m_CustomGamePendingCloneCount;
        }
        bool HasCustomGamePendingClones() const { return m_IsCustomGame && m_CustomGamePendingCloneCount != 0; }
        BattlegroundCustomRules const& GetCustomRules() const { return m_CustomRules; }
        void SendCustomGameRulesTo(Player* player) const;
        void SendSpectatorBattlefieldStatusTo(Player* player);
        bool HasCustomWeatherOverride() const { return m_IsCustomGame && m_CustomRules.Weather != BattlegroundCustomWeather::Normal; }
        bool ShouldShowFlagOnMapTo(Player const* viewer, TeamId flagTeam) const;
        void SendFlagCarrierPositionsToPlayers();
        uint32 GetFlagCaptureLimit(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.FlagCaptureLimit ? m_CustomRules.FlagCaptureLimit : defaultValue; }
        uint32 GetResourceLimit(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.ResourceLimit ? m_CustomRules.ResourceLimit : defaultValue; }
        uint32 GetDeathmatchKillLimit(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.DeathmatchKillLimit ? m_CustomRules.DeathmatchKillLimit : defaultValue; }
        uint32 GetNodeFlagCaptureTime(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.NodeFlagCaptureTimeMs ? m_CustomRules.NodeFlagCaptureTimeMs : defaultValue; }
        uint32 GetNodeBaseCaptureTime(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.NodeBaseCaptureTimeMs ? m_CustomRules.NodeBaseCaptureTimeMs : defaultValue; }
        uint32 ScaleResourceGain(uint32 value) const { return m_IsCustomGame ? std::max<uint32>(1, value * m_CustomRules.ResourceGainPercent / 100) : value; }

        typedef std::map<ObjectGuid, BattlegroundPlayer> BattlegroundPlayerMap;
        BattlegroundPlayerMap const& GetPlayers() const { return m_Players; }
        Player* _GetPlayer(ObjectGuid guid, bool offlineRemove, char const* context) const;
        Player* _GetPlayer(BattlegroundPlayerMap::iterator itr, char const* context) { return _GetPlayer(itr->first, itr->second.OfflineRemoveTime != 0, context); }
        Player* _GetPlayer(BattlegroundPlayerMap::const_iterator itr, char const* context) const { return _GetPlayer(itr->first, itr->second.OfflineRemoveTime != 0, context); }
        uint32 GetPlayersSize() const { return m_Players.size(); }

        typedef std::map<uint32, BattlegroundScore*> BattlegroundScoreMap;
        uint32 GetPlayerScoresSize() const { return PlayerScores.size(); }

        uint32 GetReviveQueueSize() const { return m_ReviveQueue.size(); }

        void AddPlayerToResurrectQueue(ObjectGuid npc_guid, ObjectGuid player_guid);
        void RemovePlayerFromResurrectQueue(ObjectGuid player_guid);
        uint32 GetTimeUntilResurrection() const;
        // What a ghost's "Resurrection in" window counts down from
        // (SMSG_AREA_SPIRIT_HEALER_TIME): the next wave, or a battleground's own
        // per-champion timer (Summoner's Rift).
        virtual uint32 GetTimeUntilResurrectionFor(Player const* /*player*/) const { return GetTimeUntilResurrection(); }
        bool IsPlayerInResurrectQueue(ObjectGuid player_guid) const;

        /// Relocate all players in ReviveQueue to the closest graveyard
        void RelocateDeadPlayers(ObjectGuid guideGuid);

        void StartBattleground();

        void toggleReplay(uint32 replayId) { m_IsReplay = replayId != 0; m_ReplayId = replayId; }

        GameObject* GetBGObject(uint32 type, bool logError = true);
        Creature* GetBGCreature(uint32 type, bool logError = true);

        // Location
        void SetMapId(uint32 MapID) { m_MapId = MapID; }
        uint32 GetMapId() const { return m_MapId; }

        // Map pointers
        void SetBgMap(BattlegroundMap* map) { m_Map = map; }
        BattlegroundMap* GetBgMap() const { ASSERT(m_Map); return m_Map; }
        BattlegroundMap* FindBgMap() const { return m_Map; }

        void SetTeamStartPosition(TeamId teamId, Position const& pos);
        Position const* GetTeamStartPosition(TeamId teamId) const;

        void SetStartMaxDist(float startMaxDist) { m_StartMaxDist = startMaxDist; }
        float GetStartMaxDist() const { return m_StartMaxDist; }

        void SetBounds(BattlegroundBounds const& bounds) { m_Bounds = bounds; }
        BattlegroundBounds const& GetBounds() const { return m_Bounds; }
        bool IsInsideBoundary(float x, float y) const;

        void SetDropExits(std::vector<BattlegroundDropExit> const& exits) { m_DropExits = exits; }
        void SetStartExit(TeamId teamId, BattlegroundStartExit const& exit) { m_StartExits[teamId] = exit; }

        // Squads - see BattlegroundSquadStart. Everything below answers for a
        // two-team match exactly as before: no squads, sides decide.
        void SetSquadStarts(std::vector<BattlegroundSquadStart> const& starts) { m_SquadStarts = starts; }
        bool IsSquadMatch() const { return !m_SquadStarts.empty(); }
        uint8 GetSquadCount() const { return uint8(m_SquadStarts.size()); }
        // Seats in one squad: the arena size the match was made for.
        uint32 GetSquadSize() const;
        static uint32 GetSquadSide(uint8 squad) { return (squad % 2) == 0 ? ALLIANCE : HORDE; }
        std::string const& GetSquadName(uint8 squad) const;
        uint32 GetSquadFlagSpell(uint8 squad) const { return squad < m_SquadStarts.size() ? m_SquadStarts[squad].FlagSpell : 0; }
        // The squad a participant plays in, or BATTLEGROUND_NO_SQUAD. A seat is
        // held from the moment the queue invites someone (or a clone is placed)
        // until they leave, so this already answers on the way in.
        uint8 GetPlayerSquad(ObjectGuid guid) const;
        // Seat `guid` in `squad`. The queue does this for each group it invites
        // so a party arrives together.
        void SetSquadSeat(ObjectGuid guid, uint8 squad);
        // Seat `guid` in the emptiest squad on `team`'s side, unless it already
        // has a seat. Returns the squad. With healers wanted in every squad
        // (SetSquadHealersWanted), a `healer` goes where one is missing most and
        // anyone else leaves each squad the seats its missing healers need.
        uint8 ClaimSquadSeat(ObjectGuid guid, uint32 team, bool healer = false);
        void ReleaseSquadSeat(ObjectGuid guid);
        // Seats in `squad` held by somebody, arrived or not.
        uint32 CountSquadSeats(uint8 squad) const;
        // Seat holders in `squad` who play a healer (Arena::IsSoloShuffleHealer).
        uint32 CountSquadHealers(uint8 squad) const;
        // Healers the bot fill means every squad to have; 0 = roles ignored.
        void SetSquadHealersWanted(uint8 count) { m_SquadHealersWanted = count; }
        uint8 GetSquadHealersWanted() const { return m_SquadHealersWanted; }
        // Whether two participants fight on the same side: the same squad in a
        // squad match, the same team otherwise. False when either one is not a
        // participant (a spectator, someone gone).
        bool AreTeammates(ObjectGuid left, ObjectGuid right) const;
        // Where `guid` starts: its squad's corner in a squad match, its team's
        // start otherwise. Take... claims a squad seat on the way.
        Position const* GetStartPositionFor(ObjectGuid guid, uint32 team) const;
        Position const* TakeStartPosition(ObjectGuid guid, uint32 team, bool healer = false);
        // The middle of the squads' corners, where a squad match converges.
        Position GetSquadArenaCentre() const;
        // What `viewer` is told about `unit` in a squad match (see
        // BattlegroundSquadStart): the faction a squad-mate or anything they
        // control shows, and a player's arena-team byte. False when this is not
        // a squad match the two share, and the real value stands.
        static bool GetSquadViewFaction(Unit const* unit, Player const* viewer, uint32& faction);
        static bool GetSquadViewArenaFaction(Player const* player, Player const* viewer, uint8& arenaFaction);
        uint8 GetWinnerSquad() const { return m_WinnerSquad; }
        // A rated squad match: each squad's arena team and matchmaker rating,
        // set by the queue as it invites them.
        void SetSquadArenaTeam(uint8 squad, uint32 arenaTeamId, uint32 matchmakerRating);
        uint32 GetSquadArenaTeamId(uint8 squad) const { return squad < MAX_BATTLEGROUND_SQUADS ? m_SquadArenaTeamIds[squad] : 0; }
        uint32 GetSquadMatchmakerRating(uint8 squad) const { return squad < MAX_BATTLEGROUND_SQUADS ? m_SquadArenaMMR[squad] : 0; }
        // The average matchmaker rating of the other rated squads: what someone
        // leaving or refusing the match is counted as losing to.
        uint32 GetSquadOpponentsMatchmakerRating(uint8 squad) const;
        // The multi-team arena enemy frames and the scoreboard's squad colours
        // (client CenturionSquadFrames.lua) learn every squad's roster and state
        // from CCGAME addon lines, since arena1-5 cannot hold three enemy
        // squads. Arena implements both; BattlegroundMgr ticks the feed and a
        // client that has just loaded asks for everything (CCGAMEREQ SQUADS).
        // Anywhere else the answer is "SQ:-", which clears the frames.
        virtual void UpdateSquadFeed() { }
        virtual void SendSquadFeedTo(Player* player);
        static void SendSquadFeedLine(Player* player, std::string const& line);

        // Buffle Shuffle (BATTLEGROUND_SSH): one arena, several rounds, the teams
        // dealt again before each (Arena.h has the rounds). A shuffle stays
        // STATUS_IN_PROGRESS from the first gates to the end; between rounds
        // it shuts the gates and counts down again on its own.
        bool IsSoloShuffle() const { return m_TypeID == BATTLEGROUND_SSH; }
        // Between two rounds of a match fought in rounds: the last one is
        // decided and the next has not begun. The start-of-match checks (bounds,
        // start exits) wait for the gates as they do before the first round,
        // and the playerbots hold where they stand (OwnsPreparationState).
        virtual bool IsBetweenRounds() const { return false; }
        // Puts a participant on `team` partway through a match, which is what a
        // shuffle does between rounds: the roster and its counts, the raid, the
        // side's faction and reputation reactions, the arena-team byte and the
        // scoreboard row, and the faction of everything they control. `player`
        // may be null for someone offline; their seat moves all the same.
        void MoveParticipantToTeam(ObjectGuid guid, Player* player, uint32 team);
        // The in-progress status again, carrying the side `player` is on: the
        // client reads its own arena side from it, nowhere else.
        void SendInProgressStatusTo(Player* player);
        // What leaving the starting room takes off a combatant: the preparation
        // auras, and the short buffs cast behind the gates.
        static void ClearArenaPreparation(Player* player);

        // Packet Transfer
        // method that should fill worldpacket with actual world states (not yet implemented for all battlegrounds!)
        virtual void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& /*packet*/) { }
        void SendPacketToTeam(uint32 TeamID, WorldPacket const* packet, Player* sender = nullptr, bool self = true);
        void SendPacketToAll(WorldPacket const* packet);

        void SendChatMessage(Creature* source, uint8 textId, WorldObject* target = nullptr);
        void SendBroadcastText(uint32 id, ChatMsg msgType, WorldObject const* target = nullptr);

        template<class Do>
        void BroadcastWorker(Do& _do);

        void SaveReplay();
        void PlaySoundToTeam(uint32 soundID, uint32 teamID);
        void PlaySoundToAll(uint32 soundID);
        void CastSpellOnTeam(uint32 SpellID, uint32 TeamID);
        void RemoveAuraOnTeam(uint32 SpellID, uint32 TeamID);
        void RewardHonorToTeam(uint32 Honor, uint32 TeamID);
        void CenturionRewardHonorToTeam(uint32 Honor, uint32 TeamID);
        void AwardSpoilsChest(Player* player, bool won);
        void RewardReputationToTeam(uint32 faction_id, uint32 Reputation, uint32 TeamID);
        void UpdateWorldState(uint32 variable, uint32 value);
        virtual void EndBattleground(uint32 winner);
        void BlockMovement(Player* player);

        void SendWarningToAll(uint32 entry, ...);
        void SendMessageToAll(uint32 entry, ChatMsg type, Player const* source = nullptr);
        void PSendMessageToAll(uint32 entry, ChatMsg type, Player const* source, ...);

        // Raid Group
        Group* GetBgRaid(uint32 TeamID) const { return TeamID == ALLIANCE ? m_BgRaids[TEAM_ALLIANCE] : m_BgRaids[TEAM_HORDE]; }
        void SetBgRaid(uint32 TeamID, Group* bg_raid);
        // A squad match keeps a raid per squad; otherwise this is the team's.
        // Slots 0 and 1 are the two teams', so a two-team match is unchanged.
        uint8 GetBgRaidSlot(ObjectGuid guid, uint32 team) const;
        Group* GetBgRaidBySlot(uint8 slot) const { return slot < MAX_BATTLEGROUND_SQUADS ? m_BgRaids[slot] : nullptr; }
        void SetBgRaidBySlot(uint8 slot, Group* bg_raid);
        // A raid being deleted lets go of whichever slot holds it. False when
        // no slot did.
        bool UnlinkBgRaid(Group const* bg_raid);

        // `viewer`, when given, is who the packet is for: a squad match is
        // shown to each player as their squad against everyone else.
        void BuildPvPLogDataPacket(WorldPacket& data, Player const* viewer = nullptr);
        virtual bool UpdatePlayerScore(Player* player, uint32 type, uint32 value, bool doAddHonor = true);

        static TeamId GetTeamIndexByTeamId(uint32 Team) { return Team == ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE; }
        uint32 GetPlayersCountByTeam(uint32 Team) const { return m_PlayersCount[GetTeamIndexByTeamId(Team)]; }
        uint32 GetAlivePlayersCountByTeam(uint32 Team) const;   // used in arenas to correctly handle death in spirit of redemption / last stand etc. (killer = killed) cases
        void UpdatePlayersCountByTeam(uint32 Team, bool remove)
        {
            if (remove)
                --m_PlayersCount[GetTeamIndexByTeamId(Team)];
            else
                ++m_PlayersCount[GetTeamIndexByTeamId(Team)];
        }

        virtual void CheckWinConditions() { }

        // used for rated arena battles
        void SetArenaTeamIdForTeam(uint32 Team, uint32 ArenaTeamId) { m_ArenaTeamIds[GetTeamIndexByTeamId(Team)] = ArenaTeamId; }
        uint32 GetArenaTeamIdForTeam(uint32 Team) const             { return m_ArenaTeamIds[GetTeamIndexByTeamId(Team)]; }
        uint32 GetArenaTeamIdByIndex(uint32 index) const { return m_ArenaTeamIds[index]; }
        void SetArenaMatchmakerRating(uint32 Team, uint32 MMR){ m_ArenaTeamMMR[GetTeamIndexByTeamId(Team)] = MMR; }
        uint32 GetArenaMatchmakerRating(uint32 Team) const          { return m_ArenaTeamMMR[GetTeamIndexByTeamId(Team)]; }

        // Triggers handle
        // must be implemented in BG subclass
        virtual void HandleAreaTrigger(Player* /*player*/, uint32 /*Trigger*/);
        // must be implemented in BG subclass if need AND call base class generic code
        virtual void HandleKillPlayer(Player* player, Player* killer);
        virtual void HandleKillUnit(Creature* /*creature*/, Player* /*killer*/) { }

        // Battleground events
        virtual void EventPlayerDroppedFlag(Player* /*player*/) { }
        virtual void EventPlayerClickedOnFlag(Player* /*player*/, GameObject* /*target_obj*/) { }
        void EventPlayerLoggedIn(Player* player);
        void EventPlayerLoggedOut(Player* player);
        virtual void ProcessEvent(WorldObject* /*obj*/, uint32 /*eventId*/, WorldObject* /*invoker*/ = nullptr) { }

        // this function can be used by spell to interact with the BG map
        virtual void DoAction(uint32 /*action*/, ObjectGuid /*var*/) { }

        virtual void HandlePlayerResurrect(Player* /*player*/) { }

        // Death related
        virtual WorldSafeLocsEntry const* GetClosestGraveyard(Player* player);

        virtual void AddPlayer(Player* player);                // must be implemented in BG subclass

        void AddOrSetPlayerToCorrectBgGroup(Player* player, uint32 team);

        virtual void RemovePlayerAtLeave(ObjectGuid guid, bool Transport, bool SendPacket);
                                                            // can be extended in in BG subclass

        void HandleTriggerBuff(ObjectGuid go_guid);

        // Whether this player may set off an environmental powerup - the Speed,
        // Restoration and Berserking runes and any custom one. Consulted by the
        // ownerless-trap search in GameObject::Update, which is the only thing
        // that fires those. Default is "anyone standing on it"; Violet Hold
        // narrows it to the human side so the clone waves cannot eat the
        // rewards the party is meant to be racing them for.
        virtual bool CanPickUpPowerup(Player const* /*player*/) const { return true; }

        // Whether a dead player may walk back to their own corpse and reclaim
        // it. Arenas refuse this outright via Player::InArena, and this is the
        // same rule made available to other modes.
        //
        // Refusing the reclaim does NOT remove the corpse or block being raised
        // by someone else: HandleReclaimCorpse is only the self-service route.
        // Rebirth, soulstones and Reincarnation all still work, exactly as they
        // do in an arena. Public because that handler is the caller.
        virtual bool AllowsCorpseReclaim() const { return true; }

        // False where the battleground's creatures are combatants on equal terms
        // with the players (a MOBA's minions, turrets and camps): Unit::DealDamage's
        // open-world bot-versus-wildlife scaling then stays off between them, so a
        // bot and a player take and deal the same damage there. Public because
        // DealDamage is the caller.
        virtual bool UsesBotCreatureScaling() const { return true; }

        // League's combat (a MOBA's): attacks and spells in the battleground never
        // miss and are never dodged, parried, blocked, glancing or crushing; only
        // champions and their pets crit; armor mitigation is read at
        // GetLeagueCombatLevel whoever attacks, and a resistance takes its average
        // share every time instead of rolling it. Unit.cpp and Object.cpp read it
        // through GetLeagueCombatBattleground.
        virtual bool UsesLeagueCombat() const { return false; }
        virtual uint8 GetLeagueCombatLevel() const { return 0; }

        // False where champions come back on their own timers, not on the spirit
        // guides' waves (Summoner's Rift): a released ghost is not queued at a
        // spirit guide, and the client is never sent the guide's "resurrect in"
        // time (Player::RepopAtGraveyard, the area spirit healer handlers).
        virtual bool UsesSpiritGuideWaves() const { return true; }

        // True while THIS battleground owns `player`'s preparation state: it
        // applied the aura/flag itself and takes it back when it is ready.
        // Violet Hold does that per WAVE, mid-match - and the playerbot
        // lifecycle otherwise strips preparation from every bot the moment a
        // match reads STATUS_IN_PROGRESS, which is Violet Hold's entire life.
        // Strand of the Ancients answers it for its first round's attackers,
        // still at sea while the match already reads in progress. The playerbot
        // lifecycle holds a bot in place for as long as this is true.
        virtual bool OwnsPreparationState(Player const* /*player*/) const { return false; }

        /// @todo make this protected:
        GuidVector BgObjects;
        GuidVector BgCreatures;
        void SpawnBGObject(uint32 type, uint32 respawntime);
        virtual bool AddObject(uint32 type, uint32 entry, float x, float y, float z, float o, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime = 0, GOState goState = GO_STATE_READY);
        bool AddObject(uint32 type, uint32 entry, Position const& pos, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime = 0, GOState goState = GO_STATE_READY);
        virtual Creature* AddCreature(uint32 entry, uint32 type, float x, float y, float z, float o, TeamId teamId = TEAM_NEUTRAL, uint32 respawntime = 0, Transport* transport = nullptr);
        Creature* AddCreature(uint32 entry, uint32 type, Position const& pos, TeamId teamId = TEAM_NEUTRAL, uint32 respawntime = 0, Transport* transport = nullptr);
        bool DelCreature(uint32 type);
        bool DelObject(uint32 type);
        bool RemoveObjectFromWorld(uint32 type);
        virtual bool AddSpiritGuide(uint32 type, float x, float y, float z, float o, TeamId teamId = TEAM_NEUTRAL);
        bool AddSpiritGuide(uint32 type, Position const& pos, TeamId teamId = TEAM_NEUTRAL);
        int32 GetObjectType(ObjectGuid guid);

        void DoorOpen(uint32 type);
        void DoorClose(uint32 type);

        // A battleground with bounds sends anyone who falls out of the world to
        // its safe point; one without has nowhere to put them.
        virtual bool HandlePlayerUnderMap(Player* player);
        // WorldObject::CanSeeOrDetect: true when this battleground hides `target`
        // from `seer` (Summoner's Rift brush). Cheap and side-effect free.
        virtual bool HidesFrom(Player const* /*target*/, WorldObject const* /*seer*/) const { return false; }

        // since arenas can be AvA or Hvh, we have to get the "temporary" team of a player
        uint32 GetPlayerTeam(ObjectGuid guid) const;
        uint32 GetOtherTeam(uint32 teamId) const;
        bool IsPlayerInBattleground(ObjectGuid guid) const;

        bool ToBeDeleted() const { return m_SetDeleteThis; }
        void SetDeleteThis() { m_SetDeleteThis = true; }

        void RewardXPAtKill(Player* killer, Player* victim);
        bool CanAwardArenaPoints() const { return m_LevelMin >= BG_AWARD_ARENA_POINTS_MIN_LEVEL; }

        virtual ObjectGuid GetFlagPickerGUID(int32 /*team*/ = -1) const { return ObjectGuid::Empty; }
        virtual void SetDroppedFlagGUID(ObjectGuid /*guid*/, int32 /*team*/ = -1) { }
        // Everyone carrying an objective right now. The flag battlegrounds hold
        // at most one carrier per team and answer through the picker slots
        // above, which is what this collects by default; Temple of Kotmogu has
        // four orbs any players can hold at once, so code asking "who carries"
        // asks this rather than the picker slots.
        virtual void GetObjectiveCarriers(std::vector<ObjectGuid>& carriers) const
        {
            for (int32 team : { int32(TEAM_ALLIANCE), int32(TEAM_HORDE), -1 })
            {
                ObjectGuid const guid = GetFlagPickerGUID(team);
                if (!guid.IsEmpty() && std::find(carriers.begin(), carriers.end(), guid) == carriers.end())
                    carriers.push_back(guid);
            }
        }
        // Flag battlegrounds can expose their currently available pickup and
        // the scoring destination for a specific carrier. Keeping
        // this in the battleground contract lets objective consumers remain
        // independent of individual battleground layouts.
        virtual ObjectGuid GetFlagPickupGUID(ObjectGuid /*playerGuid*/) const { return ObjectGuid::Empty; }
        virtual bool GetFlagCapturePosition(ObjectGuid /*carrierGuid*/, Position& /*position*/) const { return false; }
        // The area trigger a carrier has to be standing in for the capture to
        // count, or 0 when GetFlagCapturePosition is only a place to wait.
        // Clientless players never send CMSG_AREATRIGGER, so whoever moves them
        // reproduces the client's report once they are inside it.
        virtual uint32 GetFlagCaptureAreaTrigger(ObjectGuid /*carrierGuid*/) const { return 0; }
        virtual uint32 GetDynamicNodeCount() const { return 0; }
        virtual bool GetDynamicNodeInfo(ObjectGuid /*playerGuid*/, uint32 /*nodeId*/, BattlegroundNodeObjective& /*node*/) const { return false; }
        bool GetNodeObjective(ObjectGuid playerGuid, BattlegroundNodeObjective& objective) const;
        // Siege, commanders and shortcuts: see the objective structs above.
        virtual uint32 GetSiegeObjectiveCount() const { return 0; }
        virtual bool GetSiegeObjectiveInfo(ObjectGuid /*playerGuid*/, uint32 /*index*/, BattlegroundSiegeObjective& /*objective*/) const { return false; }
        virtual bool GetSiegeChargeInfo(ObjectGuid /*playerGuid*/, BattlegroundSiegeChargeInfo& /*charge*/) const { return false; }
        // Living siege vehicles the asking player's team may drive at a gate.
        virtual void GetSiegeVehicles(ObjectGuid /*playerGuid*/, GuidVector& /*vehicles*/) const { }
        // Living guns the asking player's team may man, whoever sits in them:
        // vehicles that never move and shoot people (Strand of the Ancients'
        // anti-personnel cannons).
        virtual void GetTurrets(ObjectGuid /*playerGuid*/, GuidVector& /*turrets*/) const { }
        // Every gate still standing that nobody walks through, whichever side
        // holds it. Navmesh does not contain gameobjects, so a route the
        // pathfinder draws through one has to be refused by whoever follows it.
        virtual void GetStandingGates(GuidVector& /*gates*/) const { }
        // Orders for a bot of this battleground (BattlegroundBotOrders); false when
        // the battleground leaves its bots to the generic strategy.
        virtual bool GetBotOrders(Player* /*bot*/, BattlegroundBotOrders& /*orders*/) { return false; }
        virtual uint32 GetBossObjectiveCount() const { return 0; }
        virtual bool GetBossObjectiveInfo(ObjectGuid /*playerGuid*/, uint32 /*index*/, BattlegroundBossObjective& /*boss*/) const { return false; }
        virtual void GetTeleporters(ObjectGuid /*playerGuid*/, std::vector<BattlegroundTeleporter>& /*teleporters*/) const { }
        // Rows in `battleground_drop_exit` for this battleground type (Dalaran
        // Sewers' start pipes); a battleground with its own in code overrides.
        virtual void GetDropExits(ObjectGuid /*playerGuid*/, std::vector<BattlegroundDropExit>& exits) const { exits.insert(exits.end(), m_DropExits.begin(), m_DropExits.end()); }
        virtual void HandleQuestComplete(uint32 /*questid*/, Player* /*player*/) { }
        virtual bool CanActivateGO(int32 /*entry*/, uint32 /*team*/) const { return true; }
        virtual bool IsSpellAllowed(uint32 /*spellId*/, Player const* /*player*/) const { return true; }
        uint32 GetTeamScore(uint32 TeamID) const;

        virtual uint32 GetPrematureWinner();

        // because BattleGrounds with different types and same level range has different m_BracketId
        uint8 GetUniqueBracketId() const;

        //spectators
        typedef std::set<Player*> SpectatorList;
        typedef std::map<ObjectGuid, ObjectGuid> ToBeTeleportedMap;
        void AddSpectator(Player* p) { m_Spectators.insert(p); }
        void RemoveSpectator(Player* p) { m_Spectators.erase(p); }
        bool HaveSpectators() { return !m_Spectators.empty(); }
        [[nodiscard]] const SpectatorList& GetSpectators() const { return m_Spectators; }
        void AddToBeTeleported(ObjectGuid spectator, ObjectGuid participant) { m_ToBeTeleported[spectator] = participant; }
        void RemoveToBeTeleported(ObjectGuid spectator) { ToBeTeleportedMap::iterator itr = m_ToBeTeleported.find(spectator); if (itr != m_ToBeTeleported.end()) m_ToBeTeleported.erase(itr); }
        void SpectatorsSendPacket(WorldPacket& data);

        // this method is called, when BG cannot spawn its own spirit guide, or something is wrong, It correctly ends Battleground
        void EndNow();

    protected:
        void PlayerAddedToBGCheckIfBGIsRunning(Player* player);
        Player* _GetPlayerForTeam(uint32 teamId, BattlegroundPlayerMap::const_iterator itr, char const* context) const;

        void _ProcessOfflineQueue();
        void _ProcessResurrect(uint32 diff);
        void _ProcessProgress(uint32 diff);
        void _ProcessLeave(uint32 diff);
        void _ProcessJoin(uint32 diff);
        void _CheckSafePositions(uint32 diff);
        void _CheckBounds(uint32 diff);
        void _CheckStartExits(uint32 diff);
        bool _ReturnInsideBounds(Unit* unit);
        bool _ClosestPointInsideBoundary(float& x, float& y) const;

        // See CountPreparationSeatsHeldByClones.
        void PrunePreparationSeatTakers();
        void HandOverPreparationSeat(Player* player, uint32 team);
        void RestartPreparationCountdown();

        uint32 GetConfiguredResurrectionInterval(uint32 defaultValue) const { return m_IsCustomGame && m_CustomRules.ResurrectionIntervalMs ? m_CustomRules.ResurrectionIntervalMs : defaultValue; }
        virtual uint32 GetResurrectionInterval() const { return GetConfiguredResurrectionInterval(RESURRECTION_INTERVAL); }
        virtual uint32 GetBuffRespawnTime(uint32 type) const { return BUFF_RESPAWN_TIME; }

        // Whether an under-populated team should start the premature-finish
        // countdown. Modes that legitimately run with one side empty - Violet
        // Hold sits at zero enemies between waves - opt out here.
        virtual bool AllowsPrematureFinish() const { return true; }

        // An arena still undecided when its time runs out. Stock ends it with no
        // winner; a squad match picks the squad that is ahead.
        virtual void EndAtTimeLimit() { EndBattleground(0); }

        void SetWinnerSquad(uint8 squad) { m_WinnerSquad = squad; }
        // Whether the participant `guid` on `team` won a match that ended with
        // `winner`: their squad won, in a squad match; in a shuffle, they won
        // more rounds than they lost.
        virtual bool IsWinningParticipant(ObjectGuid guid, uint32 team, uint32 winner) const;

        // A match fought in rounds keeps its own clock (Arena::UpdateRounds):
        // the time limit is a round's, not the whole match's.
        virtual void UpdateRounds(uint32 /*diff*/) { }
        // The gates shut again and the start-of-match clocks (bounds grace,
        // start exits) back at zero, for another round in the same arena.
        void ResetStartingAreaForRound();
        // Sends anyone who has wandered off their start back to it, the check
        // every match runs behind closed gates.
        void KeepPlayersAtStarts(uint32 diff);


        // Scorekeeping
        BattlegroundScoreMap PlayerScores;                // Player scores
        // must be implemented in BG subclass
        virtual void RemovePlayer(Player* /*player*/, ObjectGuid /*guid*/, uint32 /*team*/) { }
        virtual void ModifyEndOfMatchHonorRewards(uint32 /*winner*/, uint32 /*team*/, uint32& /*winnerHonor*/, uint32& /*loserHonor*/) const { }
        // Same hook for the copper payout: sees the amounts as they will be
        // paid (arena multiplier already applied for arenas), before the
        // 15-second minimum-duration gate.
        virtual void ModifyEndOfMatchMoneyRewards(uint32 /*winner*/, uint32 /*team*/, uint32& /*winnerMoney*/, uint32& /*loserMoney*/) const { }
        // Whether this match is recorded and its end-of-match honor evened out
        // to the realm's honor per hour (BattlegroundMgr::GetHourlyHonorPayout).
        // Off for a mode whose payout already measures its own effort.
        virtual bool PaysHourlyHonor() const { return true; }
        // The flat end-of-match honor times this, while the kind has too few
        // recorded matches for the hourly payout (HourlyHonor.MinSamples) - a
        // kind that runs far longer than a Warsong asks for more until its
        // own medians take over.
        virtual float FlatHonorMultiplier() const { return 1.0f; }

        // Player lists, those need to be accessible by inherited classes
        BattlegroundPlayerMap m_Players;
        SpectatorList m_Spectators;
        ToBeTeleportedMap m_ToBeTeleported;
        // Spirit Guide guid + Player list GUIDS
        std::map<ObjectGuid, GuidVector> m_ReviveQueue;

        // these are important variables used for starting messages
        uint8 m_Events;
        bool m_StartingAreaPrepared;
        BattlegroundStartTimeIntervals StartDelayTimes[BG_STARTING_EVENT_COUNT];
        // this must be filled in constructors!
        uint32 StartMessageIds[BG_STARTING_EVENT_COUNT];

        bool   m_BuffChange;
        bool   m_IsRandom;

        int32 m_TeamScores[PVP_TEAMS_COUNT];

        ArenaTeamScore _arenaTeamScores[PVP_TEAMS_COUNT];
        // A rated squad match's result per squad, and which other squad each
        // squad's scoreboard shows opposite its own (the winner, or for the
        // winner the runner-up).
        ArenaTeamScore _squadArenaScores[MAX_BATTLEGROUND_SQUADS];
        uint8 m_SquadScoreboardRival[MAX_BATTLEGROUND_SQUADS];

    private:
        // Battleground
        BattlegroundTypeId m_TypeID;
        BattlegroundTypeId m_RandomTypeID;
        uint32 m_InstanceID;                                // Battleground Instance's GUID!
        BattlegroundStatus m_Status;
        uint32 m_ClientInstanceID;                          // the instance-id which is sent to the client and without any other internal use
        uint32 m_StartTime;
        uint32 m_ResetStatTimer;
        std::unordered_map<ObjectGuid, uint32> m_MatchHonor;   // NoteMatchHonor
        uint32 m_ValidStartPositionTimer;
        BattlegroundBounds m_Bounds;
        uint32 m_BoundsCheckTimer;
        uint32 m_BoundsElapsed;                             // time since the gates opened
        std::vector<BattlegroundDropExit> m_DropExits;      // from `battleground_drop_exit`, see GetDropExits()
        std::vector<BattlegroundSquadStart> m_SquadStarts;  // from `battleground_squad_start`, see IsSquadMatch()
        std::map<ObjectGuid, uint8> m_SquadSeats;           // participant (or invitee) -> squad
        uint8 m_SquadHealersWanted = 0;                     // see SetSquadHealersWanted()
        uint8 m_WinnerSquad;
        uint32 m_SquadArenaTeamIds[MAX_BATTLEGROUND_SQUADS];
        uint32 m_SquadArenaMMR[MAX_BATTLEGROUND_SQUADS];
        BattlegroundStartExit m_StartExits[PVP_TEAMS_COUNT]; // from `battleground_start_exit`, see _CheckStartExits()
        bool m_StartExitDone[PVP_TEAMS_COUNT];
        uint32 m_StartExitElapsed;                          // time since the gates opened
        int32 m_EndTime;                                    // it is set to 120000 when bg is ending and it decreases itself
        uint32 m_LastResurrectTime;
        BattlegroundBracketId m_BracketId;
        uint8  m_ArenaType;                                 // 2=2v2, 3=3v3, 5=5v5
        bool   m_InBGFreeSlotQueue;                         // used to make sure that BG is only once inserted into the BattlegroundMgr.BGFreeSlotQueue[bgTypeId] deque
        bool   m_SetDeleteThis;                             // used for safe deletion of the bg after end / all players leave
        bool   m_IsCustomGame;
        bool   m_CustomGameBotOnlyPreparation;
        uint32 m_CustomGamePendingCloneCount;
        BattlegroundCustomRules m_CustomRules;
        bool   m_IsBotFillMatch;                            // padded with transient clones, see IsBotFillMatch()
        bool   m_IsBotFillRefused;                          // see IsBotFillRefused()
        bool   m_BotFillOpponentsFielded = false;           // see HasBotFillOpponentsFielded()
        std::map<ObjectGuid, uint32> m_PreparationSeatTakers; // invited into a clone's seat -> team, see CountPreparationSeatsHeldByClones()
        uint32 m_BotFillSeatsOnTheWay[PVP_TEAMS_COUNT] = { }; // see SetBotFillSeatsOnTheWay()
        bool   m_IsTournamentPool;                         // made from the tournament queue pool, see IsTournamentPool()
        bool   m_IsArena;
        PvPTeamId _winnerTeamId;
        int32  m_StartDelayTime;
        bool   m_IsRated;                                   // is this battle rated?
        bool   m_PrematureCountDown;
        bool   m_HasEverHadNonVirtualHumanParticipant;
        uint32 m_NoNonVirtualHumanElapsed;
        bool   m_IsReplay;
        uint32 m_ReplayId;
        uint32 m_FightId;
        uint32 m_PrematureCountDownTimer;
        std::string m_Name;

        /* Pre- and post-update hooks */

        /**
         * @brief Pre-update hook.
         *
         * Will be called before battleground update is started. Depending on
         * the result of this call actual update body may be skipped.
         *
         * @param diff a time difference between two worldserver update loops in
         * milliseconds.
         *
         * @return @c true if update must be performed, @c false otherwise.
         *
         * @see Update(), PostUpdateImpl().
         */
        virtual bool PreUpdateImpl(uint32 /* diff */) { return true; }

        /**
         * @brief Post-update hook.
         *
         * Will be called after battleground update has passed. May be used to
         * implement custom update effects in subclasses.
         *
         * @param diff a time difference between two worldserver update loops in
         * milliseconds.
         *
         * @see Update(), PreUpdateImpl().
         */
        virtual void PostUpdateImpl(uint32 /* diff */) { }

        // Player lists
        GuidVector m_ResurrectQueue;                        // Player GUID
        std::deque<ObjectGuid> m_OfflineQueue;              // Player GUID

        // Invited counters are useful for player invitation to BG - do not allow, if BG is started to one faction to have 2 more players than another faction
        // Invited counters will be changed only when removing already invited player from queue, removing player from battleground and inviting player to BG
        // Invited players counters
        uint32 m_InvitedAlliance;
        uint32 m_InvitedHorde;

        // Raid Group
        Group* m_BgRaids[MAX_BATTLEGROUND_SQUADS];           // 0 - alliance, 1 - horde; a squad match: one per squad

        // Players count by team
        uint32 m_PlayersCount[PVP_TEAMS_COUNT];

        // Arena team ids by team
        uint32 m_ArenaTeamIds[PVP_TEAMS_COUNT];

        uint32 m_ArenaTeamMMR[PVP_TEAMS_COUNT];

        // Limits
        uint32 m_LevelMin;
        uint32 m_LevelMax;
        uint32 m_MaxPlayersPerTeam;
        uint32 m_MaxPlayers;
        uint32 m_MinPlayersPerTeam;
        uint32 m_MinPlayers;

        // Start location
        uint32 m_MapId;
        BattlegroundMap* m_Map;
        Position StartPosition[PVP_TEAMS_COUNT];
        float m_StartMaxDist;
        uint32 ScriptId;
};

// The battleground playing League's combat (Battleground::UsesLeagueCombat) where
// `object` stands, or nullptr.
TC_GAME_API Battleground const* GetLeagueCombatBattleground(WorldObject const* object);
#endif
