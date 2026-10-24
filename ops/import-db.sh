#!/usr/bin/env bash
# Loads Centurion's database snapshot (centurion/sql/import.sh) into the local
# MySQL 8.4 as root. Run from Git Bash. Passes through BOTS=0 and FORCE=1.
#
# import.sh expands $MYSQL unquoted, so nothing in it may contain a space, and
# mysql's -p prompt doesn't work under MinTTY: hence MySQL's bin on PATH and the
# root password from a defaults file.
set -euo pipefail
ROOT=${ROOT:-/d/WoWServer}
export PATH="/c/Program Files/MySQL/MySQL Server 8.4/bin:$PATH"
cd "$ROOT/src/centurion/sql"
MYSQL="mysql --defaults-extra-file=$(cygpath -m "$ROOT/secrets/mysql-root.cnf") --default-character-set=utf8mb4"
MYSQL="$MYSQL" ./import.sh

# reference data the snapshot leaves out (see the comment in the file)
$MYSQL "${CHAR_DB:-characters}" < "$ROOT/src/ops/sql/characters-addons.sql"
echo "applied ops/sql/characters-addons.sql"

# world rows of our own (see the comment in the file)
$MYSQL "${WORLD_DB:-world}" < "$ROOT/src/ops/sql/world-addons.sql"
echo "applied ops/sql/world-addons.sql"
