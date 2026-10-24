"""Install the patch zips from a downloads\\patches folder into a 3.3.5a client,
following the table in the Centurion README, and record their versions in the
launcher's update-cache.json so it treats them as current.

usage: python -I install-patches.py <patches dir> <client dir>
"""
import json
import shutil
import sys
import zipfile
from pathlib import Path

patches = Path(sys.argv[1])
client = Path(sys.argv[2])

# zip name -> (destination folder, installed file name, update-cache key)
MPQS = {
    "patch-Y": ("Data", "patch-X.MPQ", "patch-X"),
    "patch-Z": ("Data", "patch-Z.MPQ", "patch-Z"),
    "patch-4": ("Data", "patch-4.MPQ", "patch-4"),
    "patch-N": ("Data", "patch-N.MPQ", "patch-N"),
    "patch-dungeon-maps": ("Data", "patch-M.MPQ", "patch-dungeon-maps"),
    "patch-enUS-6": (r"Data\enUS", "patch-enUS-6.MPQ", "patch-enUS-6"),
    "patch-enUS-7": (r"Data\enUS", "patch-enUS-7.MPQ", "patch-enUS-7"),
    "patch-enUS-A": (r"Data\enUS", "patch-enUS-A.MPQ", "patch-enUS-A"),
}


def version(name: str) -> str:
    return (patches / f"{name}.version").read_text(encoding="ascii").strip()


installed = {}

for name, (folder, target, key) in MPQS.items():
    with zipfile.ZipFile(patches / f"{name}.zip") as z:
        (member,) = z.namelist()  # every patch zip holds exactly one MPQ
        dest = client / folder / target
        tmp = dest.with_suffix(".MPQ.tmp")
        with z.open(member) as src, open(tmp, "wb") as out:
            shutil.copyfileobj(src, out, 1 << 20)
        tmp.replace(dest)
    installed[key] = version(name)
    print(f"{name:20} -> {folder}\\{target}  ({installed[key]})")

# addons: replace every folder the zip ships, plus any other CENTURION_* folder
addons_dir = client / "Interface" / "AddOns"
addons_dir.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(patches / "addons.zip") as z:
    shipped = {n.split("/")[0] for n in z.namelist()}
    for d in addons_dir.iterdir():
        if d.is_dir() and (d.name in shipped or d.name.upper().startswith("CENTURION_")):
            shutil.rmtree(d)
    z.extractall(addons_dir)
installed["addons"] = version("addons")
print(f"{'addons':20} -> Interface\\AddOns ({len(shipped)} folders, {installed['addons']})")

with zipfile.ZipFile(patches / "client-tweaks.zip") as z:
    z.extract("dinput8.dll", client)
installed["client-tweaks"] = version("client-tweaks")
print(f"{'client-tweaks':20} -> dinput8.dll  ({installed['client-tweaks']})")

cache_path = client / ".launcher" / "update-cache.json"
cache = json.loads(cache_path.read_text(encoding="utf-8")) if cache_path.exists() else {}
cache.update(installed)
cache_path.write_text(json.dumps(cache, indent=2) + "\n", encoding="utf-8")
print(f"updated {cache_path}")
