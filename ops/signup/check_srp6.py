"""Checks srp6.py against an account the server itself created.

Make a throwaway account at the worldserver console first
(account create SRP6CHECK <password>), then run:

    python -I ops/signup/check_srp6.py SRP6CHECK <password>

It reads that account's salt and verifier with the signup user's read access
and recomputes the verifier from the salt.
"""
import sys
from pathlib import Path

import pymysql

sys.path.insert(0, str(Path(__file__).parent))
from srp6 import calculate_verifier  # noqa: E402

username, password = sys.argv[1].upper(), sys.argv[2]
conn = pymysql.connect(read_default_file=r"D:\WoWServer\secrets\mysql-signup.cnf", database="auth")
with conn.cursor() as cur:
    cur.execute("SELECT salt, verifier FROM account WHERE username = %s", (username,))
    row = cur.fetchone()
conn.close()
if not row:
    sys.exit(f"no account {username}")

salt, stored = bytes(row[0]), bytes(row[1])
ours = calculate_verifier(username, password, salt)
print("match" if ours == stored else f"MISMATCH\n stored {stored.hex()}\n ours   {ours.hex()}")
sys.exit(0 if ours == stored else 1)
