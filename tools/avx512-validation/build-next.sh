#!/bin/sh
# Build a git ref on the Ice Lake machine into its own build dir, for A/B or bisecting.
#
# usage: tools/avx512-validation/build-next.sh <git-ref> [name]
#   name defaults to the short hash. Creates on the remote:
#     ~/avx512-work/full-<name>/    source (copy of full/ with common, pcsx2, tools replaced by the ref)
#     ~/avx512-work/build-<name>/   Ninja build
#     ~/avx512-work/run-<name>.sh   same as run-pcsx2.sh, but starts that build
#     ~/avx512-work/build-<name>.log
#   then: ~/avx512-work/run-<name>.sh bench-on
#
# Notes:
#  - Run from the repository root in Git Bash. The remote has no git, so the ref is sent as a tar.
#  - full/ supplies 3rdparty, pcsx2/GS/parallel-gs and the other files archives do not carry;
#    only common/, pcsx2/ and tools/ come from the ref. Files the ref lacks but full/ has stay.
#  - The build runs in the background on the remote (nohup). Progress:
#      ssh note@192.168.10.105 'tail -c 300 ~/avx512-work/build-<name>.log'
#  - Reuses an existing full-<name>/build-<name> (incremental) if the same name is given again.
set -eu

HOST=${AVX512_HOST:-note@192.168.10.105}
ref=${1:?usage: build-next.sh <git-ref> [name]}
name=${2:-$(git rev-parse --short "$ref")}

git rev-parse --verify --quiet "$ref^{commit}" >/dev/null || { echo "unknown ref: $ref" >&2; exit 1; }

echo "== $ref ($(git rev-parse --short "$ref")) -> full-$name / build-$name on $HOST"

git archive "$ref" common pcsx2 tools | ssh "$HOST" "sh -c '
set -e
cd ~/avx512-work
[ -d full-$name ] || cp -a full full-$name
tar xf - -C full-$name 2>/dev/null || true
# tar restores old mtimes, which invalidates the PCH and confuses ninja
find full-$name -type f -exec touch {} +
find build-$name -name cmake_pch.hxx.pch -delete 2>/dev/null || true
if [ ! -f build-$name/build.ninja ]; then
  cmake -S full-$name -B build-$name -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_EXE_LINKER_FLAGS_INIT=-fuse-ld=lld -DCMAKE_MODULE_LINKER_FLAGS_INIT=-fuse-ld=lld \
    -DCMAKE_SHARED_LINKER_FLAGS_INIT=-fuse-ld=lld -DCMAKE_PREFIX_PATH=\$HOME/avx512-work/deps \
    > build-$name.cfg.log 2>&1
fi
sed \"s#build-full/bin/pcsx2-qt#build-$name/bin/pcsx2-qt#\" run-pcsx2.sh > run-$name.sh
chmod +x run-$name.sh
nohup ninja -C build-$name pcsx2-qt > build-$name.log 2>&1 &
echo started
'"

echo "Wait for the build (about 10-20 minutes for a fresh dir):"
echo "  ssh $HOST 'grep -c FAILED ~/avx512-work/build-$name.log; tail -c 200 ~/avx512-work/build-$name.log'"
echo "Then run:"
echo "  ~/avx512-work/run-$name.sh bench-on"
