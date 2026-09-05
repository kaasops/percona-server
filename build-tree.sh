#!/usr/bin/env bash
#
# Build the trees debian/rules builds, without packaging.
#
# debian/rules is the source of truth for how this branch is built; this
# script mirrors its override_dh_auto_configure and override_dh_auto_build
# steps and stops there. No install, no dh_strip, no .deb. The result is a
# configured and compiled tree per requested configuration:
#
#   build-debug/    CMAKE_BUILD_TYPE=Debug          (plugins in .../plugin/debug)
#   build-release/  CMAKE_BUILD_TYPE=RelWithDebInfo (plugins in .../plugin)
#
# Builds are incremental by design. When a tree is already configured,
# cmake is not re-run and make picks up only what changed - that is what
# makes this usable as a per-commit gate ("does this commit build?")
# rather than an hours-long ritual. Use --reconfigure to force cmake.
#
# A consequence, accepted deliberately: the revision baked into the
# binaries comes from COMPILATION_COMMENT, which is fixed at cmake time.
# In an incremental build it therefore lags behind HEAD, and that
# mismatch is fine here - keeping it exact would mean reconfiguring on
# every commit, i.e. the full rebuild this mode exists to avoid. The
# script prints both revisions so the gap is visible, never silent.
# Builds whose binaries ship, or which are bisected for a defect, must be
# configured fresh: there the revision has to mean what it says.
#
# Three deliberate differences from debian/rules, all requested:
#
#   1. ccache is forced on (debian/rules does not mention ccache at all;
#      /usr/lib/ccache holds the compiler symlinks but is not on PATH by
#      default, so a plain dpkg-buildpackage run compiles without it).
#   2. A dirty working tree marks the revision with a "-dirty" suffix at
#      configure time, so a binary built from uncommitted code says so.
#   3. LTO is always off (debian/rules turns it on for most distros, both
#      debug and release). It buys nothing for local iteration and costs
#      build time and link-time memory; the artifact that actually ships
#      is built by debian/rules on a production-* branch, where LTO stays
#      on as Percona intends - this script never produces that artifact.
#
# Usage:
#   ./build-tree.sh --debug                build/refresh the debug tree
#   ./build-tree.sh --release              build/refresh the release tree
#   ./build-tree.sh --debug --release      both
#   ./build-tree.sh --release -j 8         limit parallelism (default: nproc)
#   ./build-tree.sh --debug --configure    cmake only, no compile
#   ./build-tree.sh --release --reconfigure  re-run cmake before building
#
# Without a configuration argument the script does nothing and says so:
# building both trees takes long enough that it should be asked for
# explicitly, never fallen into by running the script bare.
#
set -euo pipefail

MYSQL_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$MYSQL_SRC"

usage() { sed -n '3,45p' "$0" | sed 's/^# \{0,1\}//'; }

# ---------------------------------------------------------------- arguments
DO_DEBUG=0
DO_RELEASE=0
JOBS=""
CONFIGURE_ONLY=0
RECONFIGURE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --debug) DO_DEBUG=1 ;;
    --release) DO_RELEASE=1 ;;
    --configure) CONFIGURE_ONLY=1 ;;
    --reconfigure) RECONFIGURE=1 ;;
    -j) JOBS="$2"; shift ;;
    -j*) JOBS="${1#-j}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; echo >&2; usage >&2; exit 2 ;;
  esac
  shift
done

if [ "$DO_DEBUG" = 0 ] && [ "$DO_RELEASE" = 0 ]; then
  echo "Nothing selected: pass --debug and/or --release."
  echo
  usage
  exit 0
fi

# ------------------------------------------------------------------- ccache
# The tree understands a ccache'd compiler natively (see the
# CMAKE_C_COMPILER MATCHES "ccache$" check in cmake/abi_check.cmake), so
# putting the symlink directory first on PATH is the supported way in.
if [ -d /usr/lib/ccache ]; then
  export PATH="/usr/lib/ccache:$PATH"
else
  echo "REFUSING: /usr/lib/ccache not found, won't build without ccache" >&2
  echo "      (install ccache, or remove this check deliberately)" >&2
  exit 1
fi
command -v ccache >/dev/null || { echo "REFUSING: ccache not on PATH" >&2; exit 1; }

# --------------------------------------------------------------- revision
# debian/rules: REVISION = $(shell git rev-parse --short HEAD)
# Dirtiness is checked with git status --porcelain, not git diff: an
# untracked file is not part of the commit either, so a binary built
# beside one cannot be traced back to source with certainty.
REVISION="$(git rev-parse --short HEAD)"
if [ -n "$(git status --porcelain)" ]; then
  REVISION="${REVISION}-dirty"
  echo "WARNING: working tree has uncommitted changes."
  echo "          Revision baked into the binaries: ${REVISION}"
  echo "          It cannot be used to reliably locate the build's source."
fi

# ------------------------------------------------------- variables from rules
PS_VERSION_EXTRA='Custom by @abychko'
COMPILATION_COMMENT_RELEASE="Percona Server (GPL), Release ${PS_VERSION_EXTRA}, Revision ${REVISION}"
COMPILATION_COMMENT_DEBUG="Percona Server - Debug (GPL), Release ${PS_VERSION_EXTRA}, Revision ${REVISION}"
TOKUDB_BACKUP_VERSION="@@TOKUDB_BACKUP_VERSION@@"

NCPU="${JOBS:-$(grep -c proc /proc/cpuinfo)}"
DEB_HOST_MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
DISTRELEASE="$(lsb_release -c -s)"

# Deliberate difference from debian/rules (third one, alongside ccache
# and the dirty-suffix marking documented above): LTO is always off here.
# This script exists for fast local iteration, not for producing the
# artifact that ships - that's debian/rules on a production-* branch,
# where LTO stays on as Percona intends. LTO buys nothing for iteration
# (it mainly serves release-time cross-TU optimization) and actively
# hurts it: link-time memory pressure that made a debug -j16 build take
# 6+ hours and push the machine into swap, and harder-to-follow debugging
# once inlining decisions span translation units.
LTO_FLAG="-DWITH_LTO=OFF"

# debian/rules: exported unconditionally
export CFLAGS=-Wno-error=free-nonheap-object
export CXXFLAGS=-Wno-error=free-nonheap-object
export CPPFLAGS=-Wno-error=free-nonheap-object

TOKUDB_OPTS_DEFAULT=(
  -DWITH_VALGRIND=OFF -DUSE_VALGRIND=OFF -DDEBUG_EXTNAME=OFF
  -DBUILD_TESTING=OFF -DUSE_GTAGS=OFF -DUSE_CTAGS=OFF -DUSE_ETAGS=OFF
  -DUSE_CSCOPE=OFF "-DTOKUDB_BACKUP_PLUGIN_VERSION=${TOKUDB_BACKUP_VERSION}"
)

# Options identical in both configurations.
COMMON_OPTS=(
  -DBUILD_CONFIG=mysql_release
  -DCMAKE_INSTALL_PREFIX=/usr
  -DINSTALL_DOCDIR=share/mysql/docs
  -DINSTALL_DOCREADMEDIR=share/mysql
  -DINSTALL_INCLUDEDIR=include/mysql
  -DINSTALL_INFODIR=share/mysql/docs
  "-DINSTALL_LIBDIR=lib/${DEB_HOST_MULTIARCH}"
  -DINSTALL_MANDIR=share/man
  -DINSTALL_MYSQLSHAREDIR=share/mysql
  -DINSTALL_MYSQLTESTDIR=lib/mysql-test
  -DINSTALL_SBINDIR=sbin
  -DINSTALL_SCRIPTDIR=bin
  -DINSTALL_SUPPORTFILESDIR=share/mysql
  -DSYSCONFDIR=/etc/mysql
  -DMYSQL_UNIX_ADDR=/var/run/mysqld/mysqld.sock
  -DSYSTEM_TYPE=debian-linux-gnu
  -DINSTALL_LAYOUT=DEB
  -DWITH_INNODB_MEMCACHED=ON
  -DWITH_MECAB=system
  -DWITH_ARCHIVE_STORAGE_ENGINE=ON
  -DWITH_BLACKHOLE_STORAGE_ENGINE=ON
  -DWITH_FEDERATED_STORAGE_ENGINE=ON
  -DWITH_PAM=ON
  -DWITH_EXTRA_GDB_DEBUGINFO=ON
  -DFORCE_INSOURCE_BUILD=1
  -DWITH_PACKAGE_FLAGS=OFF
  -DWITH_SYSTEM_LIBS=ON
  -DWITH_PROTOBUF=bundled
  -DWITH_RAPIDJSON=bundled
  -DWITH_ICU=bundled
  -DWITH_ZSTD=bundled
  -DWITH_ZLIB=bundled
  -DWITH_LZ4=bundled
  -DWITH_EDITLINE=bundled
  -DWITH_LIBEVENT=bundled
  -DWITH_FIDO=bundled
  -DWITH_ENCRYPTION_UDF=ON
  -DWITH_NUMA=ON
  # MyRocks off explicitly: storage/rocksdb/CMakeLists.txt builds it
  # whenever WITH_ROCKSDB is undefined, despite its own comment saying
  # otherwise. Dropping the upstream -DWITH_ROCKSDB=ON is not enough.
  -DWITH_ROCKSDB=OFF
  -DWITH_LDAP=system
  -DWITH_PERCONA_TELEMETRY=ON
  "${LTO_FLAG}"
  -DWITH_EXTRA_CHARSETS=all
)

# Revision currently baked into an already-configured tree, if any.
cached_revision() {
  sed -n 's/^COMPILATION_COMMENT[^=]*=.*Revision \(.*\)$/\1/p' \
    "$1/CMakeCache.txt" 2>/dev/null | head -1
}

configure_tree() {
  local dir="$1" ; shift
  if [ -f "$dir/CMakeCache.txt" ] && [ "$RECONFIGURE" = 0 ]; then
    local cached
    cached="$(cached_revision "$dir")"
    echo "    incremental: cmake not re-run (use --reconfigure to force a full reconfigure)"
    if [ -n "$cached" ] && [ "$cached" != "$REVISION" ]; then
      echo "    revision in tree: ${cached}, HEAD: ${REVISION} - mismatch is expected in this mode"
    fi
    return 0
  fi
  mkdir -p "$dir"
  ( cd "$dir" && cmake "${COMMON_OPTS[@]}" "$@" .. )
}

build_tree() {
  local dir="$1" ; shift
  # debian/rules builds debug with VERBOSE=1 and release without.
  ( cd "$dir" && make -j"${NCPU}" "$@" )
}

echo "=== -j${NCPU}, revision ${REVISION}, ${DISTRELEASE} (LTO ${LTO_FLAG#-DWITH_LTO=}) ==="

if [ "$DO_DEBUG" = 1 ]; then
  echo "--- configure build-debug"
  configure_tree build-debug \
    -DCMAKE_BUILD_TYPE=Debug \
    -DINSTALL_PLUGINDIR=lib/mysql/plugin/debug \
    "-DCOMPILATION_COMMENT=${COMPILATION_COMMENT_DEBUG}" \
    "${TOKUDB_OPTS_DEFAULT[@]}" -DTOKU_DEBUG_PARANOID=ON
  [ "$CONFIGURE_ONLY" = 1 ] || { echo "--- build build-debug"; build_tree build-debug VERBOSE=1; }
fi

if [ "$DO_RELEASE" = 1 ]; then
  echo "--- configure build-release"
  configure_tree build-release \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DINSTALL_PLUGINDIR=lib/mysql/plugin \
    "-DCOMPILATION_COMMENT=${COMPILATION_COMMENT_RELEASE}" \
    "${TOKUDB_OPTS_DEFAULT[@]}" -DTOKU_DEBUG_PARANOID=OFF
  [ "$CONFIGURE_ONLY" = 1 ] || { echo "--- build build-release"; build_tree build-release; }
fi

# ------------------------------------------------------------------ verify
# Two things are worth proving rather than assuming: that the compiler
# really went through ccache, and that the revision reached the binary.
echo
echo "=== verify ==="
for d in build-debug build-release; do
  [ -d "$d" ] || continue
  cc_used="$(grep -m1 '^CMAKE_C_COMPILER:' "$d/CMakeCache.txt" 2>/dev/null | cut -d= -f2-)"
  echo "${d}: compiler = ${cc_used:-?}"
  case "$cc_used" in
    */ccache/*|*ccache) : ;;
    *) echo "  WARNING: compiler path doesn't look like ccache" ;;
  esac
  cached="$(cached_revision "$d")"
  echo "  revision in tree: ${cached:-?}   HEAD: ${REVISION}"
  if [ -x "$d/runtime_output_directory/mysqld" ]; then
    echo -n "  version: "
    "$d/runtime_output_directory/mysqld" --version 2>/dev/null || echo "(fails to run)"
  fi
done
