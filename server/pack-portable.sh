#!/usr/bin/env bash
# pack-portable.sh [DEST]: the portable server folder from build-portable.sh's output, the pinned
# MariaDB bintar (only what a server needs), wowbg3-server.sh and fetch-sql.py with sql.lock. No game
# data: vmangos' database SQL is downloaded on the player's PC at the first start (fetch-sql.py). DEST defaults to $WOWBG3_SERVER_BUILD/wowbg3-server. Libraries the programs
# need beyond glibc and the sniper runtime's basics are copied next to them (lib/), from the same SDK
# image they were built in. Needs Docker.
set -euo pipefail
source "$(dirname "$0")/../release/build-env.sh"
source "$WOWBG3_REPO/server/pins.sh"
stage="$WOWBG3_SERVER_BUILD"
dest="${1:-$stage/wowbg3-server}"
[[ -f "$stage/out/.complete" ]] || { echo "no finished build in $stage" >&2; exit 1; }
[[ ! -e "$dest" ]] || { echo "Refusing: $dest exists" >&2; exit 1; }
echo "$MARIADB_SHA256  $MARIADB_TAR" | sha256sum -c - >/dev/null
mkdir -p "$dest"/{bin/Extractors,lib,etc,mariadb}
o="$stage/out/server"
cp "$o/bin/mangosd" "$o/bin/realmd" "$dest/bin/"
for t in MapExtractor VMapExtractor VMapAssembler MoveMapGenerator; do
    f="$(find "$o/bin" -name "$t" -type f | head -1)"; [ -n "$f" ] || { echo "no $t" >&2; exit 1; }
    cp "$f" "$dest/bin/Extractors/"
done
for f in config.json offmesh.txt; do
    g="$(find "$o" -name "$f" -type f | head -1)"; [ -n "$g" ] && cp "$g" "$dest/bin/Extractors/"
done
cp "$o"/etc/mangosd.conf.dist "$o"/etc/realmd.conf.dist "$dest/etc/"
# MariaDB: the server, the client, the install script and their data files.
tmpx="$stage/mdb-unpack"; mkdir -p "$tmpx"
tar xzf "$MARIADB_TAR" -C "$tmpx" --strip-components=1 \
    --wildcards '*/bin/mariadbd' '*/bin/mariadb' '*/bin/my_print_defaults' '*/bin/mariadb-dump' \
    '*/bin/resolveip' '*/scripts/mariadb-install-db' '*/share/*' '*/lib/libmariadb.so*' \
    --exclude='*/share/man/*' --exclude='*/share/doc/*' --exclude='*/mysql-test/*'
mv "$tmpx"/bin "$tmpx"/scripts "$tmpx"/share "$tmpx"/lib "$dest/mariadb/"
rmdir "$tmpx"
# The bintar's programs carry their debug info (mariadbd ~300 MB); not needed to run.
strip --strip-debug "$dest"/mariadb/bin/*
cp -P "$dest"/mariadb/lib/libmariadb.so.3 "$dest/lib/"
# Libraries from the SDK the programs link that a plain host or the sniper runtime may lack.
docker run --rm --network none -v "$dest:/p:Z" "$SNIPER_SDK" bash -c '
  set -e
  for l in libssl.so.1.1 libcrypto.so.1.1 libstdc++.so.6 libgcc_s.so.1 libcrypt.so.1; do
    src=$(ldconfig -p | grep -m1 "$l (libc6,x86-64)" | awk "{print \$NF}")
    cp -L "$src" /p/lib/
  done
  chown -R '"$(id -u):$(id -g)"' /p/lib'
cp "$WOWBG3_REPO/server/wowbg3-server.sh" "$WOWBG3_REPO/server/fetch-sql.py" "$WOWBG3_REPO/server/sql.lock" "$dest/"
chmod 755 "$dest/wowbg3-server.sh" "$dest/fetch-sql.py" "$dest"/bin/* "$dest"/bin/Extractors/*
du -sh "$dest"; du -sh "$dest"/*
