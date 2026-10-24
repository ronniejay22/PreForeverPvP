"""Generate the local secrets for the server. Never overwrites an existing file.

Files are written as plain ASCII (PowerShell 5.1's `>` would write UTF-16,
which mysql and the TrinityCore config parser can't read).
"""
import secrets
import sys
from pathlib import Path

SECRETS = Path(sys.argv[1] if len(sys.argv) > 1 else r"D:\WoWServer\secrets")


def write_once(name: str, content: str) -> None:
    path = SECRETS / name
    if path.exists():
        print(f"kept     {path}")
        return
    path.write_text(content, encoding="ascii", newline="\n")
    print(f"created  {path}")


def client_cnf(user: str) -> str:
    # hex passwords: no characters that need quoting in .cnf files, SQL or
    # TrinityCore's "host;port;user;password;db" connection strings
    return f"[client]\nuser={user}\npassword={secrets.token_hex(24)}\nhost=127.0.0.1\nport=3306\n"


write_once("mysql-root.cnf", client_cnf("root"))
write_once("mysql-signup.cnf", client_cnf("signup"))
write_once("mysql-backup.cnf", client_cnf("backup"))
write_once("mysql-trinity.txt", secrets.token_hex(24))
