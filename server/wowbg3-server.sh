#!/usr/bin/env bash
# wowbg3-server.sh: the portable WoW server, no Docker. One folder holds
# the programs (mangosd, realmd, the data extractors, a MariaDB, all built for glibc 2.31 so they run
# on the host and in Steam's sniper runtime) and, under var/, everything it makes: the database, the
# configs, the server data generated from the player's own WoW files, logs and the console pipe.
#
#   wowbg3-server.sh init WOW_DIR         first run: data from WOW_DIR/Data (maps, vmaps, dbc, ~25 s),
#                                         databases (world import ~3 min), configs, ports
#   wowbg3-server.sh start | stop | status
#   wowbg3-server.sh account NAME PASS [GMLEVEL]   a game account (console `account create`)
#   wowbg3-server.sh console 'CMD' [SECONDS]       one line to mangosd's console; prints what it logs
#   wowbg3-server.sh mmtile MAP X Y       build that movement tile if missing (MoveMapGenerator --tile),
#                                         then load it into the running server (.wowbg3 mmtile)
#   wowbg3-server.sh mmtiles MAP X Y [R]  the same for the (2R+1)^2 tiles around X, Y (default R 1)
#   wowbg3-server.sh run WOW_DIR          the release's way (the launcher plugin's
#                                         [server] child): init on the first run, start, stay in the
#                                         foreground; SIGTERM or SIGINT stops it cleanly (saveall, exit 0);
#                                         exit 1 when a daemon dies. Status in var/run/state: init, starting,
#                                         running, stopping, stopped, failed.
#
# var/ is WOWBG3_SERVER_VAR when set (the release: ~/.local/share/wow-bg3/server), else this folder's var/.
#
# Ports (127.0.0.1 only): world WOWBG3_WORLD_PORT (8105), auth WOWBG3_AUTH_PORT (3744); MariaDB has
# no TCP port at all (a unix socket in var/run). Several copies can run side by side with other ports.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
V="${WOWBG3_SERVER_VAR:-$ROOT/var}"
RUN="$V/run"
SOCK="$RUN/mariadb.sock"
WORLD_PORT="${WOWBG3_WORLD_PORT:-8105}"
AUTH_PORT="${WOWBG3_AUTH_PORT:-3744}"
export LD_LIBRARY_PATH="$ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# The core passes no socket path to the client library (host "." only switches to a socket), so
# libmariadb's own default is pointed at ours.
export MYSQL_UNIX_PORT="$SOCK"
MDB="$ROOT/mariadb"
# MariaDB's scripts (mariadb-install-db) break on a base folder with spaces, and the release sits in
# BG3's "Larian Studios/Baldur's Gate 3/Mods": then MariaDB is reached
# through a link in var/ (made where the server runs: init and start).
mdb_link() {
    case "$ROOT" in
        *[[:space:]\'\"]*) mkdir -p "$V"; ln -sfn "$ROOT/mariadb" "$V/mariadb-base"; MDB="$V/mariadb-base";;
    esac
}

say() { echo "wowbg3-server: $*"; }
die() { echo "wowbg3-server: $*" >&2; exit 1; }
alive() { [ -f "$RUN/$1.pid" ] && kill -0 "$(cat "$RUN/$1.pid")" 2>/dev/null; }
secret() { grep "^$1=" "$V/secrets.env" | cut -d= -f2; }
sql() { "$MDB/bin/mariadb" --socket="$SOCK" -uroot -p"$(secret ROOT_PASS)" "$@"; }

db_start() {
    mdb_link
    alive mariadb && return 0
    mkdir -p "$RUN" "$V/logs"
    "$MDB/bin/mariadbd" --no-defaults --basedir="$MDB" --datadir="$V/db" --socket="$SOCK" \
        --skip-networking --pid-file="$RUN/mariadb.pid" --log-error="$V/logs/mariadb.err" \
        --innodb-buffer-pool-size=256M --max-connections=50 --character-set-server=utf8mb3 \
        --collation-server=utf8mb3_general_ci > /dev/null 2>&1 &
    for _ in $(seq 1 60); do [ -S "$SOCK" ] && alive mariadb && return 0; sleep 0.5; done
    die "MariaDB did not start (see var/logs/mariadb.err)"
}
db_stop() {
    alive mariadb || return 0
    kill "$(cat "$RUN/mariadb.pid")"
    for _ in $(seq 1 120); do alive mariadb || return 0; sleep 0.5; done
    die "MariaDB did not stop"
}

# key = value lines of a dist config set to ours (every key must exist once).
set_conf() {
    local file="$1"; shift
    python3 - "$file" "$@" <<'PY'
import re, sys
path, pairs = sys.argv[1], sys.argv[2:]
text = open(path).read()
for pair in pairs:
    key, value = pair.split('=', 1)
    text, n = re.subn(r'^' + re.escape(key) + r'\s*=.*$', key + ' = ' + value, text, flags=re.M)
    if n != 1:
        sys.exit('config key %s found %d times' % (key, n))
open(path, 'w').write(text)
PY
}

init() {
    local wow="${1:?WOW_DIR (the folder with Data/)}"
    [ -d "$wow/Data" ] || die "no $wow/Data"
    [ ! -e "$V/.initialized" ] || die "already initialised ($V); move var/ aside for a new one"
    mdb_link
    mkdir -p "$V"/{db,run,logs,etc,data,honor}
    chmod 700 "$V"
    if [ ! -f "$V/secrets.env" ]; then
        ( umask 077; printf 'ROOT_PASS=%s\nMANGOS_PASS=%s\n' "$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')" \
            "$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')" > "$V/secrets.env" )
    fi
    # 1. Server data from the player's own WoW files (read only; nothing of it is shipped).
    if [ ! -f "$V/data/.complete" ]; then
        say "generating maps, vmaps and dbc from $wow/Data"
        local t0=$SECONDS w="$V/data/work"
        rm -rf "$w"; mkdir -p "$w"; ln -s "$(cd "$wow" && pwd)/Data" "$w/Data"
        ( cd "$w" && "$ROOT/bin/Extractors/MapExtractor" --silent > "$V/logs/extract-maps.log" 2>&1 )
        ( cd "$w" && "$ROOT/bin/Extractors/VMapExtractor" --silent > "$V/logs/extract-vmaps.log" 2>&1 )
        ( cd "$w" && mkdir -p vmaps && "$ROOT/bin/Extractors/VMapAssembler" --silent > "$V/logs/assemble-vmaps.log" 2>&1 )
        mv "$w/maps" "$w/vmaps" "$V/data/"
        mkdir -p "$V/data/5875" && mv "$w/dbc" "$V/data/5875/dbc"
        mkdir -p "$V/data/mmaps"
        rm -rf "$w"                                         # Data link and the vmap extractor's Buildings
        touch "$V/data/.complete"
        say "data ready in $((SECONDS - t0)) s"
    fi
    # 2. Databases.
    say "creating the databases"
    "$MDB/scripts/mariadb-install-db" --no-defaults --basedir="$MDB" --datadir="$V/db" --auth-root-authentication-method=normal \
        --skip-test-db > "$V/logs/install-db.log" 2>&1
    db_start
    "$MDB/bin/mariadb" --socket="$SOCK" -uroot -e "ALTER USER 'root'@'localhost' IDENTIFIED BY '$(secret ROOT_PASS)'; FLUSH PRIVILEGES;"
    local t0=$SECONDS
    for db in mangos characters realmd logs; do
        sql -e "CREATE DATABASE IF NOT EXISTS \`$db\` DEFAULT CHARSET utf8 COLLATE utf8_general_ci;"
    done
    sql -e "CREATE USER IF NOT EXISTS 'mangos'@'localhost' IDENTIFIED BY '$(secret MANGOS_PASS)';
            GRANT ALL ON mangos.* TO 'mangos'@'localhost'; GRANT ALL ON characters.* TO 'mangos'@'localhost';
            GRANT ALL ON realmd.* TO 'mangos'@'localhost'; GRANT ALL ON logs.* TO 'mangos'@'localhost'; FLUSH PRIVILEGES;"
    say "importing the world database (a few minutes)"
    sql mangos < "$ROOT/sql/world.sql"
    sql characters < "$ROOT/sql/characters.sql"
    sql realmd < "$ROOT/sql/logon.sql"
    sql logs < "$ROOT/sql/logs.sql"
    sql mangos < "$ROOT/sql/migrations/world_db_updates.sql"
    sql characters < "$ROOT/sql/migrations/characters_db_updates.sql"
    sql realmd < "$ROOT/sql/migrations/logon_db_updates.sql"
    sql logs < "$ROOT/sql/migrations/logs_db_updates.sql"
    sql realmd -e "DELETE FROM realmlist; INSERT INTO realmlist (id, name, address, localAddress, localSubnetMask, port, icon, realmflags, timezone, allowedSecurityLevel, population, gamebuild_min, gamebuild_max, flag, realmbuilds) VALUES (1, 'WOW-BG3', '127.0.0.1', '127.0.0.1', '255.255.255.0', $WORLD_PORT, 0, 0, 2, 0, 0, 0, 0, 0, '');"
    say "databases ready in $((SECONDS - t0)) s"
    # 3. Configs (the core's own dist files with our settings).
    local db="\".;0;mangos;$(secret MANGOS_PASS)"
    cp "$ROOT/etc/mangosd.conf.dist" "$V/etc/mangosd.conf"
    cp "$ROOT/etc/realmd.conf.dist" "$V/etc/realmd.conf"
    chmod 600 "$V/etc/mangosd.conf" "$V/etc/realmd.conf"
    set_conf "$V/etc/mangosd.conf" "DataDir=\"$V/data\"" "LogsDir=\"$V/logs\"" "HonorDir=\"$V/honor\"" \
        "LoginDatabase.Info=$db;realmd\"" "WorldDatabase.Info=$db;mangos\"" "CharacterDatabase.Info=$db;characters\"" \
        "LogsDatabase.Info=$db;logs\"" "BindIP=\"127.0.0.1\"" "WorldServerPort=$WORLD_PORT" "Console.Enable=1" \
        "Ra.Enable=0" "SOAP.Enabled=0" "PlayerLimit=5" "GM.LoginState=0" "GM.Visible=1" "GM.CheatGod=0" \
        "Warden.WinEnabled=0" "Warden.OSXEnabled=0" "PidFile=\"$RUN/mangosd-self.pid\""
    set_conf "$V/etc/realmd.conf" "LoginDatabaseInfo=$db;realmd\"" "BindIP=\"127.0.0.1\"" \
        "RealmServerPort=$AUTH_PORT" "LogsDir=\"$V/logs\"" "PidFile=\"$RUN/realmd-self.pid\""
    printf 'WORLD_PORT=%s\nAUTH_PORT=%s\n' "$WORLD_PORT" "$AUTH_PORT" > "$V/ports.env"
    db_stop
    touch "$V/.initialized"
    say "initialised; start with: $0 start"
}

# `run` owns its children: they stay in this script's process group, so one signal to the group reaches
# every one of them and nothing is left behind when the script is killed outright. mangosd and realmd both
# shut down cleanly on SIGTERM, and stop() still does the saveall. `start` (a server meant to outlive the
# command that started it) puts each child in its own session, as before.
OWN=0
start() {
    [ -e "$V/.initialized" ] || die "not initialised (init WOW_DIR first)"
    db_start
    sid=setsid
    if [ "$OWN" = 1 ]; then sid=""; fi   # `set -e`: a plain `&&` test that fails would end the script
    # `exec` in each background subshell, so the pid written here is the program's own: without it bash
    # keeps a wrapper shell in front of the program, the pid file names the wrapper, and stop() then finds
    # nothing to stop (a realmd that outlives the game keeps BG3's container, and Steam, busy).
    if ! alive realmd; then
        ( cd "$V" && exec $sid "$ROOT/bin/realmd" -c "$V/etc/realmd.conf" > "$V/logs/realmd.out" 2>&1 < /dev/null ) &
        echo $! > "$RUN/realmd.pid"
    fi
    if ! alive mangosd; then
        # No restart of the server's own: vmangos schedules one for its weekly PvP rank calculation
        # (AutoHonorRestart, on in the dist config), which came mid-session ("[SERVER] Restart in 15
        # Minutes"). Set at every start, so configs written before this get it too.
        set_conf "$V/etc/mangosd.conf" "AutoHonorRestart=0"
        rm -f "$RUN/console"; mkfifo -m 600 "$RUN/console"
        # The console reads the fifo; a holder keeps it open so mangosd never sees end of input.
        ( exec $sid sleep infinity > "$RUN/console" < /dev/null ) &
        echo $! > "$RUN/console-holder.pid"
        ( cd "$V" && exec $sid "$ROOT/bin/mangosd" -c "$V/etc/mangosd.conf" < "$RUN/console" > "$V/logs/mangosd.out" 2>&1 ) &
        echo $! > "$RUN/mangosd.pid"
    fi
    for _ in $(seq 1 240); do
        grep -q "World initialized" "$V/logs/mangosd.out" 2>/dev/null && { say "running: world 127.0.0.1:$(grep WORLD_PORT "$V/ports.env" | cut -d= -f2), auth 127.0.0.1:$(grep AUTH_PORT "$V/ports.env" | cut -d= -f2)"; return 0; }
        alive mangosd || die "mangosd stopped (see var/logs/mangosd.out)"
        sleep 0.5
    done
    die "mangosd did not finish starting in 2 minutes"
}

stop() {
    if alive mangosd; then
        echo "saveall" > "$RUN/console"; sleep 1
        kill -INT "$(cat "$RUN/mangosd.pid")"
        # Its console thread waits in a read on the pipe: end of input lets it finish.
        alive console-holder && kill "$(cat "$RUN/console-holder.pid")"
        for _ in $(seq 1 120); do alive mangosd || break; sleep 0.5; done
        alive mangosd && kill -9 "$(cat "$RUN/mangosd.pid")"
    fi
    alive console-holder && kill "$(cat "$RUN/console-holder.pid")"
    alive realmd && kill "$(cat "$RUN/realmd.pid")"
    for _ in $(seq 1 20); do            # a moment for the polite signals, so reap has nothing to do
        alive realmd || alive console-holder || break
        sleep 0.25
    done
    db_stop
    reap
    rm -f "$RUN"/*.pid
    say "stopped"
}

# Nothing of ours may be left running: BG3 runs inside a container whose own init waits for every process
# in it, so one survivor keeps the game marked as running after the player has quit. Anything still alive
# after the clean stop above is killed here, and said in the log.
reap() {
    local p pid pgid
    for p in mangosd realmd console-holder mariadb; do
        alive "$p" || continue
        pid=$(cat "$RUN/$p.pid")
        # A pid file left by a crash can name a pid the system has since given to something else.
        case "$p:$(cat "/proc/$pid/comm" 2>/dev/null)" in
            mangosd:mangosd|realmd:realmd|console-holder:sleep|mariadb:mariadbd) ;;
            *) say "$p's pid file names pid $pid, which is not $p now: left alone"; continue ;;
        esac
        say "$p (pid $pid) did not stop: killing it"
        kill -9 "$pid" 2>/dev/null || true
    done
    # The last resort, and the one that needs no pid file to be right: anything else still in our own
    # process group. Only `run` has its children there (`start` leaves them a session of their own).
    # Read from /proc: the game's container has no `ps`. In /proc/PID/stat the process group is the
    # third field after the program name's closing bracket (the name itself may hold spaces).
    [ "$OWN" = 1 ] || return 0
    pgid=$(sed 's/.*) //' /proc/self/stat 2>/dev/null | cut -d' ' -f3)
    [ -n "$pgid" ] || return 0
    # Only a group of our own: started without one (from another script), the group is the caller's.
    if [ "$pgid" != "$$" ]; then say "not a process group of our own ($pgid): no sweep"; return 0; fi
    for d in /proc/[0-9]*; do
        pid=${d#/proc/}
        [ "$pid" = "$$" ] && continue
        [ "$(sed 's/.*) //' "$d/stat" 2>/dev/null | cut -d' ' -f3)" = "$pgid" ] || continue
        say "pid $pid is still in our process group: killing it"
        kill -9 "$pid" 2>/dev/null || true
    done
    return 0
}

status() {
    for p in mariadb realmd mangosd; do
        if alive "$p"; then echo "$p running (pid $(cat "$RUN/$p.pid"))"; else echo "$p stopped"; fi
    done
}

console() {
    alive mangosd || die "mangosd is not running"
    local log="$V/logs/mangosd.out" before; before=$(wc -c < "$log")
    printf '%s\n' "$1" > "$RUN/console"
    # The reply comes in one server tick: done once this command's own reply line is there (WOWBG3 and its
    # verb: `wowbg3 vendor ...` answers `WOWBG3 vendor ...`, `wowbg3 HERO VERB ...` `WOWBG3 VERB ...`) and
    # the log has been still for 0.15 s; the wait ($2, default 2 s) is only the most. A fixed sleep cost
    # every inventory call 3 s, so loot and a vendor's wares took seconds to reach BG3 (a playtest report).
    local w0 w1 w2 want=""
    read -r w0 w1 w2 _ <<< "$1"
    if [ "$w0" = wowbg3 ]; then
        case "$w1" in inv|loot|vendor) want="WOWBG3 $w1 ";; *) want="WOWBG3 $w2 ";; esac
    fi
    local most=$(( ${2:-2} * 20 )) ticks=0 still=0 size=$before now
    while [ "$ticks" -lt "$most" ]; do
        sleep 0.05
        ticks=$((ticks + 1))
        now=$(wc -c < "$log")
        if [ "$now" -ne "$size" ]; then size=$now; still=0; else still=$((still + 1)); fi
        if [ -n "$want" ] && [ "$still" -ge 3 ] && [ "$now" -gt "$before" ] && tail -c +"$((before + 1))" "$log" | grep -qF "$want"; then
            break
        fi
    done
    tail -c +"$((before + 1))" "$log" | tr -d '\r' | sed 's/\x1b\[[0-9;]*m//g' | grep -v '^$\|^mangos>$' || true
}

mmtile() {
    local map="$1" x="$2" y="$3"
    local f; f=$(printf '%s/data/mmaps/%03d%02d%02d.mmtile' "$V" "$map" "$y" "$x")
    if [ ! -f "$f" ]; then
        local T="$ROOT/bin/Extractors"
        ( cd "$V/data" && "$T/MoveMapGenerator" "$map" --tile "$x,$y" --silent --threads 1 \
            --configInputPath "$T/config.json" --offMeshInput "$T/offmesh.txt" >> "$V/logs/mmaps.log" 2>&1 ) || true
    fi
    [ -f "$f" ] || { say "no tile $map $x $y (nothing walkable there?)"; return 0; }
    alive mangosd && console "wowbg3 - mmtile $map $x $y" 1 | grep "WOWBG3 mmtile" || true
}

# The release's foreground mode (launcher.conf [server]). A var/ whose init never finished (BG3 closed
# during the first start) is moved aside, never deleted, and made again.
state() { mkdir -p "$RUN"; echo "$1" > "$RUN/state.tmp" && mv -f "$RUN/state.tmp" "$RUN/state"; }
run() {
    local wow="${1:?WOW_DIR (the folder with Data/)}" quit=0
    trap 'quit=1' TERM INT
    if [ ! -e "$V/.initialized" ]; then
        if [ -d "$V/db" ]; then
            local aside="$V.incomplete-$(date +%Y%m%d-%H%M%S)"
            say "an unfinished first start in $V: moved to $aside"
            mv "$V" "$aside"
            mkdir -p "$V"; [ -d "$aside/data" ] && [ -f "$aside/data/.complete" ] && mv "$aside/data" "$V/data"
            [ -f "$aside/secrets.env" ] && mv "$aside/secrets.env" "$V/secrets.env"
        fi
        state init
        ( trap - TERM INT; init "$wow" ) & local ip=$!
        while kill -0 "$ip" 2>/dev/null; do
            [ "$quit" = 1 ] && { say "stopped during the first start"; kill "$ip" 2>/dev/null; wait "$ip" || true; db_stop || true; state stopped; exit 0; }
            sleep 0.5 || true   # a SIGTERM to the process group ends the sleep (set -e must not end us)
        done
        wait "$ip" || { state failed; die "first start failed (see $V/logs)"; }
    fi
    state starting
    OWN=1
    start || { state failed; stop || true; exit 1; }
    state running
    while [ "$quit" = 0 ]; do
        for p in mariadb realmd mangosd; do
            alive "$p" || { say "$p stopped by itself"; state failed; stop || true; exit 1; }
        done
        sleep 1 || true   # a SIGTERM to the process group ends the sleep (set -e must not end us)
    done
    state stopping
    stop
    state stopped
    exit 0
}

case "${1:-}" in
    init) init "${2:-}";;
    run) run "${2:-}";;
    start) start;;
    stop) stop;;
    status) status;;
    account)
        [ -n "${3:-}" ] || die "account NAME PASS [GMLEVEL]"
        console "account create $2 $3" 2 | grep -v "$3" || true
        [ -z "${4:-}" ] || console "account set gmlevel $2 $4" 1;;
    console) console "${2:?command}" "${3:-2}";;
    mmtile) mmtile "${2:?MAP}" "${3:?X}" "${4:?Y}";;
    mmtiles)
        r="${5:-1}"
        for dx in $(seq -"$r" "$r"); do for dy in $(seq -"$r" "$r"); do mmtile "$2" $(( $3 + dx )) $(( $4 + dy )); done; done;;
    *) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 2;;
esac
