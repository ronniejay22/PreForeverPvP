"""Self-service account sign-up for the realm.

Creates accounts the way AccountMgr::CreateAccount does: uppercased name and
password, an SRP6 salt and verifier, then a realmcharacters row per realm.
Runs on 127.0.0.1 behind Caddy, which forwards https://<host>/signup here.
The MySQL user it connects as can only read and insert account rows.

Settings (environment variables):
  SIGNUP_DB_CNF       [client] file with the signup user's credentials
  SIGNUP_SERVER_NAME  name shown on the page
  SIGNUP_REALMLIST    address players put in their launcher
  SIGNUP_LOG          where account creations are logged (never passwords)
"""
import logging
import os
import re
import threading
import time
from collections import defaultdict, deque

import pymysql
from flask import Flask, render_template, request

from srp6 import make_registration_data

USERNAME_RE = re.compile(r"[A-Za-z0-9]{3,16}")        # MAX_ACCOUNT_STR is 16
PASSWORD_RE = re.compile(r"[!-~]{6,16}")              # printable ASCII, no spaces; MAX_PASS_STR is 16

ATTEMPTS_PER_HOUR = 10
ACCOUNTS_PER_DAY = 3

app = Flask(__name__)
SERVER_NAME = os.environ.get("SIGNUP_SERVER_NAME", "Centurion")
REALMLIST = os.environ.get("SIGNUP_REALMLIST", "")
DB_CNF = os.environ.get("SIGNUP_DB_CNF", r"D:\WoWServer\secrets\mysql-signup.cnf")

log = logging.getLogger("signup")
log.setLevel(logging.INFO)
_handler = logging.FileHandler(os.environ.get("SIGNUP_LOG", r"D:\WoWServer\logs\signup.log"), encoding="utf-8")
_handler.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
log.addHandler(_handler)


class RateLimiter:
    """Sliding-window counters per client IP, kept in memory."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._events: dict[tuple[str, str], deque] = defaultdict(deque)

    def allow(self, kind: str, ip: str, limit: int, window: float) -> bool:
        now = time.monotonic()
        with self._lock:
            q = self._events[(kind, ip)]
            while q and now - q[0] > window:
                q.popleft()
            return len(q) < limit

    def record(self, kind: str, ip: str) -> None:
        with self._lock:
            self._events[(kind, ip)].append(time.monotonic())


limits = RateLimiter()


def client_ip() -> str:
    # Caddy appends the real client address to X-Forwarded-For; only trust it
    # when the request came from Caddy on this machine.
    forwarded = request.headers.get("X-Forwarded-For", "")
    if request.remote_addr == "127.0.0.1" and forwarded:
        return forwarded.split(",")[-1].strip()
    return request.remote_addr or "unknown"


def create_account(username: str, password: str) -> bool:
    """Returns False if the name is taken."""
    username = username.upper()
    salt, verifier = make_registration_data(username, password)
    conn = pymysql.connect(read_default_file=DB_CNF, database="auth", autocommit=False)
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT 1 FROM account WHERE username = %s", (username,))
            if cur.fetchone():
                return False
            try:
                cur.execute(
                    "INSERT INTO account(username, salt, verifier, reg_mail, email, joindate) "
                    "VALUES(%s, %s, %s, '', '', NOW())",
                    (username, salt, verifier),
                )
            except pymysql.err.IntegrityError:  # lost a race for the same name
                conn.rollback()
                return False
            cur.execute(
                "INSERT INTO realmcharacters (realmid, acctid, numchars) SELECT id, %s, 0 FROM realmlist",
                (cur.lastrowid,),
            )
        conn.commit()
        return True
    finally:
        conn.close()


@app.route("/signup", methods=["GET", "POST"])
def signup():
    page = dict(server_name=SERVER_NAME, realmlist=REALMLIST)
    if request.method == "GET":
        return render_template("signup.html", **page)

    ip = client_ip()
    if not limits.allow("attempt", ip, ATTEMPTS_PER_HOUR, 3600):
        return render_template("signup.html", error="Too many tries. Wait an hour and try again.", **page), 429
    limits.record("attempt", ip)

    username = request.form.get("username", "")
    password = request.form.get("password", "")
    confirm = request.form.get("confirm", "")

    # bots fill in the hidden field; pretend it worked and create nothing
    if request.form.get("website"):
        log.info("honeypot ip=%s", ip)
        return render_template("signup.html", created=username.upper(), **page)

    error = None
    if not USERNAME_RE.fullmatch(username):
        error = "Account names are 3 to 16 letters and digits."
    elif not PASSWORD_RE.fullmatch(password):
        error = "Passwords are 6 to 16 characters, with no spaces."
    elif password != confirm:
        error = "The two passwords don't match."
    elif not limits.allow("account", ip, ACCOUNTS_PER_DAY, 86400):
        error = "This address has created enough accounts for today."
    if error:
        return render_template("signup.html", error=error, username=username, **page), 400

    try:
        created = create_account(username, password)
    except pymysql.MySQLError:
        log.exception("database error ip=%s", ip)
        return render_template("signup.html", error="Sign-up is unavailable right now. Try again later.", **page), 503

    if not created:
        return render_template("signup.html", error="That account name isn't available.", **page), 409

    limits.record("account", ip)
    log.info("created account=%s ip=%s", username.upper(), ip)
    return render_template("signup.html", created=username.upper(), **page)


if __name__ == "__main__":
    from waitress import serve

    serve(app, host="127.0.0.1", port=int(os.environ.get("SIGNUP_PORT", "8081")), threads=4)
