#!/usr/bin/env bash
# Test-only local handler linked to the actual dispatcher + VFS libraries.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT=$(cd "$HERE/../../.." && pwd)
: "${BUILD_DIR:?set BUILD_DIR to a matching candidate build}"
TMP=${EVIDENCE_DIR:-$(mktemp -d)}
mkdir -p "$TMP"
if [[ -z ${EVIDENCE_DIR:-} ]]; then trap 'rm -rf "$TMP"' EXIT; fi
# Avoid host/guest path rewriting: these files are only fixture construction.
bun -e 'const fs=require("node:fs"),p=process.argv[1];fs.mkdirSync(p+"/a",{recursive:true});fs.mkdirSync(p+"/mounted",{recursive:true});for(const [name,size] of [["input",17011],["empty",0],["max",1048576],["oversize",1048577],["denied",17011]]){const b=Buffer.alloc(size);for(let i=0;i<size;i++)b[i]=(i*37+11)&255;fs.writeFileSync(p+"/a/"+name,b);}fs.writeFileSync(p+"/a/existing","original");' "$TMP/seed"
chmod 755 "$TMP/seed" "$TMP/seed/a" "$TMP/seed/mounted"
chmod 600 "$TMP/seed/a/denied"
cp "$TMP/seed/a/input" "$TMP/seed/mounted/input"
mkfifo "$TMP/seed/a/fifo"
ln -s input "$TMP/seed/a/input-link"
ln -s input "$TMP/seed/a/output-link"
ln -s absent "$TMP/seed/a/dangling"
tar -C "$TMP/seed" -czf "$TMP/seed.tar.gz" .
DEFS=(-D_GNU_SOURCE -DGUEST_ARM64=1 -DENGINE_ASBESTOS=1 -DISH_NATIVE_OFFLOAD_TEST_HANDLERS=1)
if grep -q -- '-DISH_JIT=1' "$BUILD_DIR/compile_commands.json"; then DEFS+=(-DISH_JIT=1); fi
WRAPS=()
for name in native_fs_open fd_close native_handler_check do_exit malloc calloc chdir fchdir pipe opendir unlink unlinkat pthread_create; do WRAPS+=(-Wl,--wrap=$name); done
"${CC:-clang}" -O2 -Wall -Wextra -Werror -Wno-unused-function "${DEFS[@]}" \
    -I"$PROJECT" -I"$BUILD_DIR" -pthread "$HERE/offload-local-copy.c" \
    "$HERE/handlers/local-copy.c" "$PROJECT/kernel/native_offload.c" \
    "${WRAPS[@]}" -Wl,--start-group "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" \
    "$BUILD_DIR/libfakefs.a" -Wl,--end-group -lrt -lm -ldl -lsqlite3 -o "$TMP/offload-local-copy"
MODES=(normal empty max short oversize fifo directory symlink-input missing permission
    mounted umask existing same symlink-output dangling-output open-input open-output no-read no-write
    alloc-input alloc-output inode-input inode-output bind-alloc stat-input stat-output stat-final
    read-error read-eintr read-again read-zero read-over write-error write-eintr write-again write-partial write-zero write-over
    close-input close-output growth shrink ops-limit deadline usage pending
    cancel-read cancel-write cancel-close replace)
for type in real fake; do
    for mode in "${MODES[@]}"; do
        CASE="$TMP/$type-$mode"; mkdir -p "$CASE"
        if [[ $type == fake ]]; then
            "$BUILD_DIR/tools/fakefsify" "$TMP/seed.tar.gz" "$CASE/fake"
            root="$CASE/fake/data"
        else
            mkdir -p "$CASE/root"; tar -C "$CASE/root" -xzf "$TMP/seed.tar.gz"
            root="$CASE/root"
        fi
        timeout -k 3 15 "$TMP/offload-local-copy" "$type" "$root" "$mode" > "$CASE/test.log" 2>&1 || { cat "$CASE/test.log"; exit 1; }
        grep -q '^local-copy-ok ' "$CASE/test.log" || { cat "$CASE/test.log"; exit 1; }
        cat "$CASE/test.log"
    done
done
