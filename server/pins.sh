# Pinned downloads for the portable server. Sourced by build-portable.sh and pack-portable.sh
# (after release/build-env.sh, which sets MARIADB_TAR: where the bintar is kept).
MARIADB_VERSION=11.4.8
MARIADB_URL="https://archive.mariadb.org/mariadb-11.4.8/bintar-linux-systemd-x86_64/mariadb-11.4.8-linux-systemd-x86_64.tar.gz"
MARIADB_SHA256=c29f9b8e935a30375df3ac61da1b71638633d3a408237cbb956df2430789d47a
# vmangos' database SQL is not shipped: the player's PC downloads it at the first start (fetch-sql.py),
# from the pinned sources in sql.lock (WORLD_DB_URL, CORE_SQL_URL, the sha256 of every file made).
source "$(dirname "${BASH_SOURCE[0]}")/sql.lock"
# Where build-portable.sh builds and pack-portable.sh packs (release/licences/collect.py reads it too).
: "${WOWBG3_SERVER_BUILD:=$WOWBG3_BUILD/portable-build}"
export WOWBG3_SERVER_BUILD
