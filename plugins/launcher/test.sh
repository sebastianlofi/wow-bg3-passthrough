#!/usr/bin/env bash
# Tests wowbg3-launcher with test_host.c and stub children, no BG3 needed.
#   test.sh PLUGIN.so TEST_HOST     (both built for this machine)
# Checks: pak copy into Mods, child started only once the mod is enabled, request-started child,
# stop on request (SIGTERM reaches the child), loading-screen gate and input hold, release by
# setting, by file and by timeout, SIGTERM to children at exit(), and at SIGKILL of the host
# (PR_SET_PDEATHSIG). Prints PASS/FAIL lines; exit 1 on any FAIL.
set -uo pipefail
plugin="$(readlink -f "$1")"; host="$(readlink -f "$2")"
t="$(mktemp -d)"; fail=0
pass() { echo "PASS: $*"; }
bad() { echo "FAIL: $*"; fail=1; }
export HOME="$t/home" XDG_DATA_HOME="$t/home/.local/share"
prof="$HOME/.local/share/Larian Studios/Baldur's Gate 3"
mkdir -p "$prof/PlayerProfiles/Public" "$t/WOW-BG3"
echo '<save><node id="Mods"></node></save>' > "$prof/PlayerProfiles/Public/modsettings.lsx"
head -c 4096 /dev/urandom > "$t/WOW-BG3/WOW-BG3.pak"
cat > "$t/WOW-BG3/stub.sh" <<'STUB'
#!/usr/bin/env bash
# stub child: logs its start, its environment's essentials and any SIGTERM, then exits
echo "start $1 pid $$ LD_PRELOAD=${LD_PRELOAD:-} HOME=$WOWBG3_HOME DATA=$WOWBG3_DATA EXTRA=${EXTRA:-}"
echo "release env RUN=${WOW_BG3_RUN_ID:-} SHM=${WOWBG3_SHM:-} TAV=${WOWBG3_TAV_DIR:-} CAP=${WOWBG3_CAPTURE_DIR:-}"
trap 'echo "TERM $1"; sleep 1; echo "clean exit $1"; exit 0' TERM
while :; do sleep 0.2; done
STUB
chmod +x "$t/WOW-BG3/stub.sh"
cat > "$t/WOW-BG3/launcher.conf" <<'CONF'
[server]
exec = {home}/stub.sh
arg = server
env = EXTRA=x1
start = enabled
grace = 3
[benilla]
exec = {home}/stub.sh
arg = benilla
start = request
grace = 3
[datagen]
exec = {home}/job.sh
start = enabled
once = 1
CONF
printf '#!/usr/bin/env bash\necho "job ran $$"\nexit 0\n' > "$t/WOW-BG3/job.sh"; chmod +x "$t/WOW-BG3/job.sh"
export WOWBG3_HOME="$t/WOW-BG3" LD_PRELOAD= WOWBG3_ANY_EXE=1  # test_host is not bg3
data="$XDG_DATA_HOME/wow-bg3"
enable() { sed -i 's#<node id="Mods">#<node id="Mods"><attribute id="UUID" value="a97c1258-37c3-434c-9309-c982f6cea252"/>#' "$prof/PlayerProfiles/Public/modsettings.lsx"; }

# 1: pak, enable, request, gate, release by setting, reset at unload, exit()
"$host" "$plugin" > "$t/out1" 2>&1 <<S &
sleep 2
get server_pid
get pak
sleep 2
get mod_enabled
sleep 1
get server_pid
set benilla 1
sleep 1
get benilla_pid
set benilla 0
sleep 2
get benilla_pid
state PrepareRunning
frame
gate LoadLevel StopLoading
set world 1
gate LoadLevel StopLoading
get holding
key
set ready 1
gate LoadLevel StopLoading
get holding
key
state UnloadLevel
frame
get world
state PrepareRunning
frame
set world 1
gate LoadLevel StopLoading
sleep 1
gate LoadLevel StopLoading
get holding
exit
S
hpid=$!
sleep 2.5; enable
wait $hpid
grep -q "clean exit server" "$data/logs/server.log" && pass "exit() waited for the server's own clean shutdown" || bad "exit() did not wait for the server"
# the ready file case is checked mid-run below (run 2)
out="$t/out1"; logs="$data/logs"
[[ -f "$prof/Mods/WOW-BG3.pak" ]] && cmp -s "$prof/Mods/WOW-BG3.pak" "$t/WOW-BG3/WOW-BG3.pak" && pass "pak copied into Mods" || bad "pak not copied"
grep -q "get pak = 1" "$out" && pass "pak setting says copied" || bad "pak setting"
grep -m1 "get server_pid" "$out" | grep -q "= 0" && pass "server not started while the mod is disabled" || bad "server started early"
grep -q "get mod_enabled = 1" "$out" && pass "enabling in modsettings.lsx seen" || bad "enable not seen"
[[ $(grep "get server_pid" "$out" | sed -n 2p | awk '{print $4}') -gt 0 ]] && pass "server started once enabled" || bad "server not started"
[[ $(grep "get benilla_pid" "$out" | sed -n 1p | awk '{print $4}') -gt 0 ]] && pass "benilla started on request" || bad "benilla not started"
grep -q "get benilla_pid = 0" "$out" && grep -q "TERM benilla" "$logs/benilla.log" && pass "benilla stopped on request with SIGTERM" || bad "benilla stop"
grep -q "LD_PRELOAD= HOME=$t/WOW-BG3" "$logs/server.log" && grep -q "EXTRA=x1" "$logs/server.log" && pass "child env: no LD_PRELOAD, WOWBG3_HOME, own env" || bad "child env"
[[ "$(grep 'gate LoadLevel->StopLoading' "$out" | awk '{print $3}' | tr '\n' ' ')" == "1 1 0 1 1 " ]] && pass "gate: hold while unknown, hold world, release on ready, hold again next load" || bad "gate sequence: $(grep 'gate ' "$out" | tr '\n' '|')"
[[ "$(grep 'key kept' "$out" | awk '{print $5}' | tr '\n' ' ')" == "1 0 " ]] && pass "input held only while holding" || bad "input hold"
grep -q "get world = -1" "$out" && pass "world reset at UnloadLevel" || bad "world reset"
sleep 1
grep -q "TERM server" "$logs/server.log" && pass "server got SIGTERM at exit()" || bad "no SIGTERM at exit"

[[ -f "$data/run/datagen.done" && $(grep -c "job ran" "$logs/datagen.log") == 1 ]] && pass "once job ran when enabled and left its marker" || bad "once job"
# 2: release by file, timeout, SIGKILL of the host -> PDEATHSIG
mkdir -p "$t/oldlogs"; mv "$data/logs" "$t/oldlogs/run1"
"$host" "$plugin" > "$t/out2" 2>&1 <<S &
set hold_timeout 5
sleep 2
set world 1
state PrepareRunning
frame
gate LoadLevel StopLoading
sleep 2
gate LoadLevel StopLoading
get holding
state UnloadLevel
frame
state PrepareRunning
frame
set world 1
gate LoadLevel StopLoading
sleep 6
gate LoadLevel StopLoading
get holding
die
S
hpid=$!
sleep 3.2; mkdir -p "$data/run"; touch "$data/run/ready"; sleep 0.1
wait $hpid 2>/dev/null
sleep 1
out="$t/out2"
rm -f "$data/run/ready"
gates="$(grep 'gate LoadLevel->StopLoading' "$out" | awk '{print $3}' | tr '\n' ' ')"
[[ "$gates" == "1 0 1 0 " ]] && pass "release by ready file, then by timeout" || bad "file/timeout gates: $gates"
grep -q "still not ready" "$out" && pass "timeout logged as a warning" || bad "timeout warning"
grep -q "TERM server" "$data/logs/server.log" && pass "server got SIGTERM when the host was SIGKILLed (PDEATHSIG)" || bad "no PDEATHSIG"
[[ ! -f "$data/logs/datagen.log" ]] && pass "once job not run again on the next start" || bad "once job ran again"

# 4: release by the lifecycle's phase "up" for this session; bg3-state.json at a load start
io="$prof/Script Extender/wow-bg3"
"$host" "$plugin" > "$t/out4" 2>&1 <<S &
state Running
frame
state UnloadLevel
frame
state PrepareRunning
frame
set world 1
gate LoadLevel StopLoading
sleep 1
gate LoadLevel StopLoading
sleep 1
gate LoadLevel StopLoading
sleep 1
gate LoadLevel StopLoading
exit
S
hpid=$!
sleep 0.5; cp "$io/bg3-state.json" "$t/bg3state-at-unload.json" 2>/dev/null
echo '{"Session": "s2", "Phase": "world"}' > "$io/hero.json"
echo '{"Session": "s2", "Phase": "restoring"}' > "$io/lifecycle-state.json"
sleep 1
echo '{"Session": "s1", "Phase": "up"}' > "$io/lifecycle-state.json"
sleep 1
echo '{"Session": "s2", "Phase": "up"}' > "$io/lifecycle-state.json"
wait $hpid
gates="$(grep 'gate LoadLevel->StopLoading' "$t/out4" | awk '{print $3}' | tr '\n' ' ')"
[[ "$gates" == "1 1 1 0 " ]] && pass "held through phase restoring and another session's up; released at this session's up" || bad "lifecycle gates: $gates"
grep -q '"Client": "UnloadLevel"' "$io/bg3-state.json" 2>/dev/null || grep -q '"Loads": 1' "$io/bg3-state.json" && pass "bg3-state.json reports the load start" || bad "bg3-state.json: $(cat "$io/bg3-state.json" 2>/dev/null)"

# 5: bg3le patch 0004: the client held in PrepareRunning until world mode's Start
# is done (world-status.json Last "Start", OK), never at StopLoading, never Save -> Running.
rm -f "$io/world-status.json"
TEST_HOLD4=1 "$host" "$plugin" > "$t/out5" 2>&1 <<S &
state SwapLevel
frame
set world 1
gate SwapLevel StopLoading
state PrepareRunning
frame
gate PrepareRunning Running
key
gate Save Running
sleep 1
gate PrepareRunning Running
sleep 2
gate PrepareRunning Running
key
exit
S
hpid=$!
sleep 1.5; echo '{"Last": "Stream", "OK": true}' > "$io/world-status.json"
sleep 1; echo '{"RunId": "x", "Last": "Start", "OK": true, "Seq": 1}' > "$io/world-status.json"
wait $hpid
gates="$(grep '^gate ' "$t/out5" | awk '{print $3}' | tr '\n' ' ')"
[[ "$gates" == "0 1 0 1 0 " ]] && pass "patch 0004: held in PrepareRunning (not at StopLoading, not Save), released when world mode's Start is done" || bad "patch 0004 gates: $gates"
keys="$(grep '^key kept' "$t/out5" | awk '{print $5}' | tr '\n' ' ')"
[[ "$keys" == "1 0 " ]] && pass "patch 0004: input held during the hold only" || bad "patch 0004 keys: $keys"
grep -q "hold at PrepareRunning -> Running" "$t/out5" && pass "patch 0004: the plugin says where it holds" || bad "patch 0004 log"

# 6: patch 0004: a failed Start of world mode ends the hold at once (not at its timeout).
rm -f "$io/world-status.json"
TEST_HOLD4=1 "$host" "$plugin" > "$t/out6" 2>&1 <<S &
state PrepareRunning
frame
set world 1
gate PrepareRunning Running
sleep 2
gate PrepareRunning Running
gate PrepareRunning Running
exit
S
hpid=$!
sleep 1; echo '{"RunId": "x", "Last": "Start", "OK": false, "Error": "world mode is off"}' > "$io/world-status.json"
wait $hpid
gates="$(grep '^gate ' "$t/out6" | awk '{print $3}' | tr '\n' ' ')"
[[ "$gates" == "1 0 0 " ]] && grep -q "Start failed" "$t/out6" && pass "patch 0004: a failed Start ends the hold at once, and it stays released" || bad "patch 0004 failed Start: $gates"

# 7 (0.3.0): the release's environment reaches the children; a held loading
# screen waits past hold_timeout while the coordinator reports Busy (the server's first start).
env1="$(grep -m1 '^release env' "$t/oldlogs/run1/server.log")"
run="$(sed 's/.*RUN=\([^ ]*\).*/\1/' <<<"$env1")"; shm="$(sed 's/.*SHM=\([^ ]*\).*/\1/' <<<"$env1")"
[[ "$run" =~ ^[0-9a-f]{32}$ && -d "$shm" && -d "$shm/tav" && "$env1" == *"CAP=$data/capture"* && -d "$data/capture" ]] \
    && pass "release env: run id $run, frames $shm, Tav $shm/tav, capture $data/capture" || bad "release env: $env1"
busy() { while [ -f "$t/busy.on" ]; do printf '{"Format": 1, "Phase": "server-init", "Busy": true}' > "$data/run/coord.json"; sleep 1; done
         printf '{"Format": 1, "Phase": "watching", "Busy": false}' > "$data/run/coord.json"; }
rm -f "$io/world-status.json"; touch "$t/busy.on"; mkdir -p "$data/run"; busy & bpid=$!
TEST_HOLD4=1 "$host" "$plugin" > "$t/out7" 2>&1 <<S &
set hold_timeout 5
state PrepareRunning
frame
set world 1
gate PrepareRunning Running
sleep 7
gate PrepareRunning Running
sleep 4
gate PrepareRunning Running
exit
S
hpid=$!
sleep 8.5; rm -f "$t/busy.on"
wait $hpid; wait $bpid
gates="$(grep '^gate ' "$t/out7" | awk '{print $3}' | tr '\n' ' ')"
[[ "$gates" == "1 1 0 " ]] && pass "coordinator Busy: held past hold_timeout, released by the timeout once it is not" || bad "busy gates: $gates"

# 8: run/stop written while running stops every child cleanly, no restart.
mv "$data/logs" "$t/oldlogs/run7" 2>/dev/null
"$host" "$plugin" > "$t/out8" 2>&1 <<S &
sleep 2
get server_pid
sleep 3
get server_pid
exit
S
hpid=$!
sleep 2.5; touch "$data/run/stop"
wait $hpid
pids="$(grep 'get server_pid' "$t/out8" | awk '{print $4}' | tr '\n' ' ')"
[[ "$pids" =~ ^[1-9][0-9]*\ 0\ $ ]] && grep -q "clean exit server" "$data/logs/server.log" && pass "run/stop: children stopped cleanly and not restarted" || bad "run/stop: $pids"
mv "$data/run/stop" "$t/stop.done"

# 9 (0.4.0): a "start = session" child waits through BG3's menu and starts when the client leaves it.
mv "$data/logs" "$t/oldlogs/run8" 2>/dev/null
cp "$t/WOW-BG3/launcher.conf" "$t/launcher.conf.orig"
sed -i '0,/start = enabled/s//start = session/' "$t/WOW-BG3/launcher.conf"
"$host" "$plugin" > "$t/out9" 2>&1 <<S
state Menu
frame
sleep 2
get server_pid
state StartLoading
frame
sleep 2
get server_pid
exit
S
pids="$(grep 'get server_pid' "$t/out9" | awk '{print $4}' | tr '\n' ' ')"
[[ "$pids" =~ ^0\ [1-9][0-9]*\ $ ]] && grep -q "start session" "$t/out9" && grep -q "left the menu (StartLoading)" "$t/out9" \
    && pass "start = session: not started in the menu, started when the client left it" || bad "start = session: $pids"
cp "$t/launcher.conf.orig" "$t/WOW-BG3/launcher.conf"

# 3: a bg3le without patch 0003: loads, says so, never holds
TEST_OLD_BG3LE=1 "$host" "$plugin" > "$t/out3" 2>&1 <<S
gate LoadLevel StopLoading
exit
S
grep -q "no state gates" "$t/out3" && grep -q "init -> 0" "$t/out3" && pass "stock bg3le: loads and says the hold is off" || bad "stock bg3le"
sleep 1
for f in "$t"/out*; do echo "--- $f"; cat "$f"; done > "$t/all.txt"
echo "transcripts: $t/all.txt"
exit $fail
