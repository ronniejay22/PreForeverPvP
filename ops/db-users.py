"""Create (or reset) the MySQL users the server uses, with the passwords in the
secrets folder. Run after ops/import-db.sh: the table-level grants need the
auth tables to exist. The SQL goes to mysql on stdin, so no password appears
on a command line.

usage: python -I ops/db-users.py [root folder, default D:\\WoWServer]
"""
import configparser
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1] if len(sys.argv) > 1 else r"D:\WoWServer")
secrets = root / "secrets"
MYSQL = r"C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe"


def cnf_password(name: str) -> str:
    cnf = configparser.ConfigParser()
    cnf.read(secrets / name, encoding="ascii")
    return cnf["client"]["password"]


def user(name: str, password: str) -> str:
    return (f"CREATE USER IF NOT EXISTS '{name}'@'localhost' IDENTIFIED BY '{password}';\n"
            f"ALTER USER '{name}'@'localhost' IDENTIFIED BY '{password}';\n")


trinity = (secrets / "mysql-trinity.txt").read_text(encoding="ascii").strip()

sql = (
    user("trinity", trinity)
    + "GRANT ALL PRIVILEGES ON auth.* TO 'trinity'@'localhost';\n"
    + "GRANT ALL PRIVILEGES ON world.* TO 'trinity'@'localhost';\n"
    + "GRANT ALL PRIVILEGES ON characters.* TO 'trinity'@'localhost';\n"
    # the sign-up page can only create accounts
    + user("signup", cnf_password("mysql-signup.cnf"))
    + "GRANT SELECT, INSERT ON auth.account TO 'signup'@'localhost';\n"
    + "GRANT SELECT, INSERT ON auth.realmcharacters TO 'signup'@'localhost';\n"
    + "GRANT SELECT ON auth.realmlist TO 'signup'@'localhost';\n"
    # read-only, for ops/backup.ps1 (mysqldump --single-transaction --routines --no-tablespaces)
    + user("backup", cnf_password("mysql-backup.cnf"))
    + "".join(f"GRANT SELECT, SHOW VIEW, TRIGGER, EVENT, LOCK TABLES ON {db}.* TO 'backup'@'localhost';\n"
              for db in ("auth", "world", "characters"))
    + "GRANT SHOW_ROUTINE ON *.* TO 'backup'@'localhost';\n"
)

subprocess.run([MYSQL, f"--defaults-extra-file={secrets / 'mysql-root.cnf'}"], input=sql, text=True, check=True)
print("users ready: trinity, signup, backup")
