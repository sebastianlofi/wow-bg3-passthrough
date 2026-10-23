#!/usr/bin/env bash
# build-portable.sh: the portable WoW server's programs, built in Steam's sniper SDK container
# (glibc 2.31, GCC 14), so they run on the host and inside Steam's sniper runtime alike: mangosd and
# realmd (pinned vmangos core plus patches/vmangos, checked against its SHA256SUMS) and the data
# extractors (map/dbc, vmap, vmap assembler, mmap). Client library and headers: the pinned MariaDB
# bintar's libmariadb (downloaded to $MARIADB_TAR when missing). Output: $WOWBG3_SERVER_BUILD/out
# (default $WOWBG3_BUILD/portable-build). Then pack-portable.sh. Needs Docker.
set -euo pipefail
source "$(dirname "$0")/../release/build-env.sh"
source "$WOWBG3_REPO/server/pins.sh"
stage="$WOWBG3_SERVER_BUILD"
[[ ! -e "$stage" ]] || { echo "Refusing: $stage exists (move it aside first)" >&2; exit 1; }
[[ "$SNIPER_SDK" == registry.gitlab.steamos.cloud/steamrt/sniper/sdk@sha256:* ]]
if [[ ! -f "$MARIADB_TAR" ]]; then
    mkdir -p "$(dirname "$MARIADB_TAR")"
    curl -sSfL -o "$MARIADB_TAR.part" "$MARIADB_URL" && mv "$MARIADB_TAR.part" "$MARIADB_TAR"
fi
echo "$MARIADB_SHA256  $MARIADB_TAR" | sha256sum -c -
mkdir -p "$stage/in" "$stage/out"
# Our patches in order, each checked against patches/vmangos/SHA256SUMS.
(cd "$WOWBG3_REPO/patches/vmangos" && sha256sum -c --quiet SHA256SUMS)
awk '{print $2}' "$WOWBG3_REPO/patches/vmangos/SHA256SUMS" | while read -r name; do
    cp "$WOWBG3_REPO/patches/vmangos/$name" "$stage/in/"
done
ls "$stage/in" | grep -c patch
cat > "$stage/build.sh" <<IN
set -euo pipefail
mkdir -p /build/mdb
tar xzf /mdb.tar.gz -C /build/mdb --strip-components=1 --wildcards '*/include/mysql/*' '*/lib/libmariadb.so*'
git init -q /build/core && cd /build/core
git remote add origin $VMANGOS_URL
git fetch -q --depth 1 origin $VMANGOS_COMMIT && git checkout -q FETCH_HEAD
for p in /work/in/*.patch; do echo "Applying \$p"; git apply "\$p"; done
mkdir build && cd build
cmake .. -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release \\
  -DCMAKE_INSTALL_PREFIX=/work/out/server -DBUILD_FOR_HOST_CPU=0 -DBUILD_EXTRACTORS=1 \\
  -DENABLE_MAILSENDER=0 -DENABLE_CPPTRACE=0 -DSUPPORTED_CLIENT_BUILD=5875 -DDEBUG_SYMBOLS=0 \\
  -DMYSQL_INCLUDE_DIR=/build/mdb/include/mysql -DMYSQL_LIBRARY=/build/mdb/lib/libmariadb.so
nice -n 10 make -j4
make install
cd /work/out/server && find . -type f -perm -u+x | sort > ../programs.txt && touch ../.complete
chown -R $(id -u):$(id -g) /work/out
IN
docker run --rm --name "wow-bg3-portable-build-$(date +%Y%m%d%H%M%S)" --cpus 4 --cpu-shares 128 --memory 8g --memory-swap 8g \
    --pids-limit 2048 --security-opt no-new-privileges -v "$stage:/work:Z" -v "$MARIADB_TAR:/mdb.tar.gz:ro,Z" \
    --workdir /work "$SNIPER_SDK" bash /work/build.sh > "$stage/build.log" 2>&1
tail -3 "$stage/build.log"
cat "$stage/out/programs.txt"
