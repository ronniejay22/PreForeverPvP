#!/usr/bin/env python3
"""Grow the level-cap PvP bot pool (Playerbot.Pve.PvpOnlyAccountIds) by cloning
the bots it already has.

Every tier (Playerbot.BgFill.Tier.*) holds one bot of each class and spec, fully
geared and talented. A new bot is a copy of the one with its tier, class and
spec - talents, spells, skills, action bars, gear (as new items), pet, guild -
under a new name, a gender picked at random, and the looks of an existing
character of that race and gender (always a valid combination). The race stays
the donor's, so its racial spells and buttons stay right.

Arena teams need healers more than anything else: each healer spec is copied
HEALER_COPIES times, every damage spec once, tanks (Protection warriors and
paladins) never, until the pool reaches TARGET.

    clone_pvp_bots.py plan                 what would be made, nothing written
    clone_pvp_bots.py apply --confirm      write it (worldserver must be stopped)
    clone_pvp_bots.py reset-ladder --confirm
                                           disband the bot ladder's arena teams and
                                           forget the bots' arena matchmaker ratings,
                                           so the next start deals the ladder afresh

The worldserver has to be stopped for `apply`: it hands out character and item
guids from the highest in the database, read at startup, and reads the character
cache only then. Clone sources it has not cached cannot be copied into matches.
"""

import argparse
import random
import re
import struct
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(r"D:\WoWServer")
MYSQL = r"C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe"
DEFAULTS = ROOT / "secrets" / "mysql-root.cnf"
DBC = ROOT / "data" / "dbc"

ACCOUNT = 79                 # Playerbot.Pve.PvpOnlyAccountIds
TARGET = 200                 # bots on the account once done
HEALER_COPIES = 4            # copies of each healer donor
GUID_BASE = 300001           # first new character guid; tiers get consecutive blocks
TIERS = {                    # Playerbot.BgFill.Tier.*.Characters as they stood
    "Easy": "100955-100963,200001-200018",
    "Medium": "101158-101162,101164-101167,200019-200036",
    "Hard": "101193-101194,101196-101202,200037-200054",
}
TIER_ORDER = ["Easy", "Medium", "Hard"]
HEALING_TREES = {"Holy", "Discipline", "Restoration"}
CLASSES = {1: "Warrior", 2: "Paladin", 3: "Hunter", 4: "Rogue", 5: "Priest",
           7: "Shaman", 8: "Mage", 9: "Warlock", 11: "Druid"}
RACES = {1: "Human", 2: "Orc", 3: "Dwarf", 4: "Night Elf", 5: "Undead",
         6: "Tauren", 7: "Gnome", 8: "Troll"}

# Tables keyed by the character guid, copied as they are.
PER_CHARACTER = ["character_spell", "character_talent", "character_action", "character_skills",
                 "character_reputation", "character_homebind", "character_glyphs"]

# What a new character row gets instead of the donor's.
RESET = {"online": "0", "at_login": "0", "totalKills": "0", "todayKills": "0", "yesterdayKills": "0",
         "totalHonorPoints": "0", "todayHonorPoints": "0", "yesterdayHonorPoints": "0", "arenaPoints": "0",
         "deleteInfos_Account": "NULL", "deleteInfos_Name": "NULL", "deleteDate": "NULL"}

# Syllables a name is made of, by race. ObjectMgr::CheckPlayerName with
# StrictPlayerNames = 1 takes 2-12 basic Latin letters, no letter three times
# in a row (checked below).
SYLLABLES = {
    1: (["Al", "Ber", "Cor", "Ed", "Gar", "Hal", "Jor", "Mar", "Rol", "Ser", "Tor", "Wil", "Ad", "Bri", "El", "Gwen", "Is", "Ro", "Ver", "Lyn", "Os", "Cael"],
        ["ric", "wyn", "den", "ard", "ton", "mund", "ian", "ella", "ora", "wen", "ia", "ys", "rin", "ett", "a", "an"]),
    2: (["Gro", "Kar", "Mor", "Thra", "Dur", "Gar", "Rok", "Zug", "Ush", "Mag", "Nar", "Kro", "Bur", "Gash", "Ur", "Dra"],
        ["gash", "nak", "rok", "thar", "zog", "ka", "ra", "gul", "mar", "rak", "dush", "gor", "za", "mok"]),
    3: (["Brom", "Dur", "Thor", "Gim", "Bal", "Grim", "Hild", "Kel", "Mor", "Thra", "Brun", "Dag", "Mag", "Ror"],
        ["gar", "din", "li", "rek", "ga", "da", "bek", "nir", "hild", "dra", "grim", "nor", "ra"]),
    4: (["Ael", "Ela", "Fan", "Ili", "Mal", "Shan", "Syl", "Tal", "Ya", "Ny", "Aru", "Leth", "Mae", "Ty"],
        ["driel", "ara", "thas", "wyn", "dra", "iel", "rae", "ys", "aris", "nora", "lien", "dor", "sha"]),
    5: (["Mor", "Val", "Cor", "Vex", "Sil", "Dra", "Grav", "Ash", "Lor", "Ner", "Kel", "Bel", "Sar", "Ul"],
        ["ven", "ith", "ius", "mire", "gore", "ella", "ris", "cus", "rot", "dred", "wick", "ane", "is"]),
    6: (["Ta", "Ha", "Ma", "Ka", "Na", "Wa", "Ho", "Ske", "Tu", "Ahe", "Mo", "Sa"],
        ["hono", "lona", "nuk", "kwa", "tanka", "mani", "reth", "ota", "hala", "mok", "ruk", "wa"]),
    7: (["Fiz", "Tink", "Giz", "Nix", "Wob", "Bix", "Pim", "Snig", "Cog", "Dib", "Mek", "Zap", "Tob"],
        ["zle", "kle", "bang", "wick", "nob", "gle", "ton", "ker", "bit", "bo", "zap", "pin", "sy"]),
    8: (["Zal", "Jin", "Ras", "Vol", "Zul", "Kaz", "Tal", "Ula", "Ze", "Mak", "Sen", "Ju"],
        ["jin", "zar", "ko", "mba", "rak", "ji", "zuk", "ka", "tu", "lah", "ni", "jan", "ra"]),
}


# --------------------------------------------------------------------------- db

def query(sql):
    res = subprocess.run([MYSQL, f"--defaults-extra-file={DEFAULTS}", "-N", "--batch", "-e", sql],
                         capture_output=True, text=True, encoding="utf-8")
    if res.returncode:
        sys.exit(f"mysql failed: {res.stderr.strip()}")
    return [line.split("\t") for line in res.stdout.splitlines() if line]


def execute_file(path):
    with open(path, "rb") as sql:
        res = subprocess.run([MYSQL, f"--defaults-extra-file={DEFAULTS}", "--default-character-set=utf8mb4"],
                             stdin=sql, capture_output=True)
    if res.returncode:
        sys.exit(f"mysql failed: {res.stderr.decode('utf-8', 'replace').strip()}")


def columns(table):
    return [row[0] for row in query(
        f"SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'characters' "
        f"AND TABLE_NAME = '{table}' ORDER BY ORDINAL_POSITION")]


def worldserver_running():
    res = subprocess.run(["tasklist", "/FI", "IMAGENAME eq worldserver.exe"], capture_output=True, text=True)
    return "worldserver.exe" in res.stdout


# --------------------------------------------------------------------- donors

def tier_of(guid):
    for name, ranges in TIERS.items():
        for part in ranges.split(","):
            lo, _, hi = part.partition("-")
            if int(lo) <= guid <= int(hi or lo):
                return name
    return None


def read_dbc(name):
    data = (DBC / f"{name}.dbc").read_bytes()
    count, fields, size, _ = struct.unpack_from("<4I", data, 4)
    rows = [struct.unpack_from(f"<{fields}I", data, 20 + i * size) for i in range(count)]
    strings = data[20 + count * size:]
    return rows, lambda off: strings[off:strings.index(b"\0", off)].decode("utf-8")


def specs_by_guid(guids):
    """The tree with the most points in each character's active spec."""
    talents, _ = read_dbc("Talent")
    tabs, text = read_dbc("TalentTab")
    tab_name = {row[0]: text(row[1]) for row in tabs}
    spell_talent = {}
    for row in talents:
        for rank, spell in enumerate(row[4:13]):
            if spell:
                spell_talent[spell] = (row[0], row[1], rank + 1)

    held = defaultdict(dict)
    for guid, spell in query(
            "SELECT ct.guid, ct.spell FROM characters.character_talent ct JOIN characters.characters c "
            f"ON c.guid = ct.guid AND ct.talentGroup = c.activeTalentGroup WHERE c.guid IN ({','.join(map(str, guids))})"):
        hit = spell_talent.get(int(spell))
        if hit:
            talent, tab, rank = hit
            held[int(guid)][talent] = (tab, max(held[int(guid)].get(talent, (tab, 0))[1], rank))

    specs = {}
    for guid in guids:
        points = defaultdict(int)
        for tab, rank in held[guid].values():
            points[tab] += rank
        specs[guid] = tab_name[max(points.items(), key=lambda kv: kv[1])[0]] if points else "?"
    return specs


def load_donors():
    rows = query(f"SELECT guid, name, race, class, gender FROM characters.characters WHERE account = {ACCOUNT} AND level = 60")
    guids = [int(r[0]) for r in rows]
    specs = specs_by_guid(guids)
    donors = []
    for guid, name, race, cls, gender in rows:
        guid = int(guid)
        tier = tier_of(guid)
        if not tier:
            continue
        spec = specs[guid]
        donors.append({"guid": guid, "name": name, "race": int(race), "class": int(cls), "gender": int(gender),
                       "tier": tier, "spec": spec, "healer": spec in HEALING_TREES,
                       "tank": spec == "Protection" and int(cls) in (1, 2)})
    return donors


# ----------------------------------------------------------------------- plan

def make_name(race, taken, rng):
    first, last = SYLLABLES[race]
    for _ in range(500):
        name = rng.choice(first) + rng.choice(last)
        if rng.random() < 0.25:
            name += rng.choice(last)
        name = name[:1].upper() + name[1:].lower()
        low = name.lower()
        if not re.fullmatch(r"[A-Za-z]{2,12}", name) or low in taken:
            continue
        if any(low[i] == low[i - 1] == low[i - 2] for i in range(2, len(low))):
            continue
        # "Hildhildli": no three letters twice over
        if any(low.count(low[i:i + 3]) > 1 for i in range(len(low) - 2)):
            continue
        taken.add(low)
        return name
    sys.exit(f"ran out of names for race {race}")


def plan(seed):
    rng = random.Random(seed)
    donors = load_donors()
    existing = int(query(f"SELECT COUNT(*) FROM characters.characters WHERE account = {ACCOUNT}")[0][0])
    wanted = TARGET - existing
    if wanted <= 0:
        sys.exit(f"account {ACCOUNT} already has {existing} characters")

    by_tier = defaultdict(list)
    for donor in sorted(donors, key=lambda d: (d["class"], d["spec"])):
        by_tier[donor["tier"]].append(donor)

    # healers HEALER_COPIES times each, then every damage spec once, tier by tier
    picks = []
    for tier in TIER_ORDER:
        tier_picks = [d for d in by_tier[tier] if d["healer"] for _ in range(HEALER_COPIES)]
        tier_picks += [d for d in by_tier[tier] if not d["healer"] and not d["tank"]]
        picks.append(tier_picks)
    flat = [d for tier_picks in picks for d in tier_picks]
    if len(flat) < wanted:
        sys.exit(f"only {len(flat)} donors' worth of copies for the {wanted} wanted")
    # trim evenly from the end of each tier's damage specs
    while sum(len(p) for p in picks) > wanted:
        longest = max(range(len(picks)), key=lambda i: len(picks[i]))
        picks[longest].pop()

    taken = {row[0].lower() for row in query("SELECT name FROM characters.characters")}
    taken |= {row[0].lower() for row in query("SELECT name FROM characters.reserved_name")}
    looks = defaultdict(list)
    for race, gender, *appearance in query(
            "SELECT race, gender, skin, face, hairStyle, hairColor, facialStyle FROM characters.characters WHERE level > 0"):
        looks[(int(race), int(gender))].append(appearance)

    guid = GUID_BASE
    bots = []
    tier_ranges = {}
    for tier, tier_picks in zip(TIER_ORDER, picks):
        first = guid
        for donor in tier_picks:
            gender = rng.choice((0, 1))
            if not looks[(donor["race"], gender)]:
                gender = donor["gender"]
            bots.append({"guid": guid, "donor": donor, "name": make_name(donor["race"], taken, rng),
                         "gender": gender, "looks": rng.choice(looks[(donor["race"], gender)])})
            guid += 1
        tier_ranges[tier] = (first, guid - 1)
    return bots, tier_ranges


# ---------------------------------------------------------------------- write

def build_sql(bots):
    character_columns = columns("characters")
    per_character = {table: columns(table) for table in PER_CHARACTER}
    item_columns = columns("item_instance")
    pet_columns = columns("character_pet")
    pet_spell_columns = columns("pet_spell")

    next_item = int(query("SELECT COALESCE(MAX(guid), 0) FROM characters.item_instance")[0][0]) + 1000
    next_pet = int(query("SELECT COALESCE(MAX(id), 0) FROM characters.character_pet")[0][0]) + 100

    def col_list(cols):
        return ", ".join(f"`{c}`" for c in cols)

    lines = ["USE characters;", "START TRANSACTION;"]
    for bot in bots:
        new, donor = bot["guid"], bot["donor"]["guid"]
        skin, face, hair_style, hair_color, facial = bot["looks"]
        overrides = dict(RESET, guid=str(new), name=f"'{bot['name']}'", gender=str(bot["gender"]),
                         skin=skin, face=face, hairStyle=hair_style, hairColor=hair_color, facialStyle=facial)
        select = ", ".join(overrides.get(c, f"`{c}`") for c in character_columns)
        lines.append(f"-- {bot['name']} ({new}): {bot['donor']['tier']} {bot['donor']['spec']} "
                     f"{CLASSES[bot['donor']['class']]}, copy of {bot['donor']['name']} ({donor})")
        lines.append(f"INSERT INTO characters ({col_list(character_columns)}) SELECT {select} FROM characters WHERE guid = {donor};")

        for table, cols in per_character.items():
            rest = [c for c in cols if c != "guid"]
            lines.append(f"INSERT INTO {table} ({col_list(['guid'] + rest)}) SELECT {new}, {col_list(rest)} FROM {table} WHERE guid = {donor};")

        # gear: every inventory item becomes a new item; bags keep their contents
        items = query(f"SELECT bag, slot, item FROM characters.character_inventory WHERE guid = {donor}")
        remap = {}
        for _, _, item in items:
            remap[int(item)] = next_item
            next_item += 1
        for old, fresh in remap.items():
            rest = [c for c in item_columns if c not in ("guid", "owner_guid")]
            lines.append(f"INSERT INTO item_instance ({col_list(['guid', 'owner_guid'] + rest)}) "
                         f"SELECT {fresh}, {new}, {col_list(rest)} FROM item_instance WHERE guid = {old};")
        for bag, slot, item in items:
            bag = int(bag)
            lines.append(f"INSERT INTO character_inventory (guid, bag, slot, item) VALUES "
                         f"({new}, {remap.get(bag, 0) if bag else 0}, {slot}, {remap[int(item)]});")

        for (pet,) in query(f"SELECT id FROM characters.character_pet WHERE owner = {donor}"):
            rest = [c for c in pet_columns if c not in ("id", "owner")]
            lines.append(f"INSERT INTO character_pet ({col_list(['id', 'owner'] + rest)}) "
                         f"SELECT {next_pet}, {new}, {col_list(rest)} FROM character_pet WHERE id = {pet};")
            spell_rest = [c for c in pet_spell_columns if c != "guid"]
            lines.append(f"INSERT INTO pet_spell ({col_list(['guid'] + spell_rest)}) "
                         f"SELECT {next_pet}, {col_list(spell_rest)} FROM pet_spell WHERE guid = {pet};")
            next_pet += 1

        lines.append(f"INSERT INTO guild_member (guildid, guid, `rank`, pnote, offnote) "
                     f"SELECT guildid, {new}, `rank`, pnote, offnote FROM guild_member WHERE guid = {donor};")

    lines.append(f"UPDATE auth.realmcharacters SET numchars = (SELECT COUNT(*) FROM characters.characters "
                 f"WHERE account = {ACCOUNT}) WHERE acctid = {ACCOUNT};")
    lines.append("COMMIT;")
    return "\n".join(lines) + "\n"


def describe(bots, tier_ranges):
    for bot in bots:
        d = bot["donor"]
        print(f"{bot['guid']}\t{bot['name']}\t{d['tier']}\t{RACES[d['race']]} {'female' if bot['gender'] else 'male'}\t"
              f"{d['spec']} {CLASSES[d['class']]}\t<- {d['name']}")
    healers = sum(1 for b in bots if b["donor"]["healer"])
    print(f"\n{len(bots)} new bots, {healers} of them healers")
    for tier, (lo, hi) in tier_ranges.items():
        print(f"Playerbot.BgFill.Tier.{tier}.Characters += {lo}-{hi}")


def reset_ladder_sql():
    captains = f"SELECT guid FROM characters WHERE account = {ACCOUNT}"
    return "\n".join([
        "USE characters;",
        "START TRANSACTION;",
        f"DELETE FROM arena_team_member WHERE arenaTeamId IN (SELECT arenaTeamId FROM arena_team WHERE captainGuid IN ({captains}));",
        f"DELETE FROM arena_team WHERE captainGuid IN ({captains});",
        f"DELETE FROM character_arena_stats WHERE guid IN ({captains});",
        "COMMIT;",
    ]) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["plan", "apply", "reset-ladder"])
    parser.add_argument("--confirm", action="store_true")
    parser.add_argument("--seed", type=int, default=20261024)
    parser.add_argument("--sql-out", type=Path, help="where to keep the SQL that was (or would be) run")
    args = parser.parse_args()

    if args.command in ("apply", "reset-ladder"):
        if not args.confirm:
            sys.exit("add --confirm to write")
        if worldserver_running():
            sys.exit("stop the worldserver first")

    if args.command == "reset-ladder":
        sql = reset_ladder_sql()
        path = args.sql_out or Path("reset-ladder.sql")
        path.write_text(sql, encoding="utf-8")
        execute_file(path)
        print("bot ladder teams disbanded; the next start deals them afresh")
        return

    bots, tier_ranges = plan(args.seed)
    describe(bots, tier_ranges)
    if args.command == "apply":
        sql = build_sql(bots)
        path = args.sql_out or Path("clone-pvp-bots.sql")
        path.write_text(sql, encoding="utf-8")
        execute_file(path)
        count = query(f"SELECT COUNT(*) FROM characters.characters WHERE account = {ACCOUNT}")[0][0]
        print(f"\nwritten ({path}); account {ACCOUNT} now has {count} characters")


if __name__ == "__main__":
    main()
