#!/usr/bin/env bash
# Builds the Mocktail Plus release package on the maintainer's machine, with
# portable code for the x86-64-v3 level whatever -march the system config uses.
#
# Usage:
#   packaging/plus/build-release.sh --out DIR [--local REPO REF] [--verbose]
#                                   [-- MAKEPKG_ARGS...]
#
#   --out DIR         work directory, best kept outside the source tree:
#                     DIR/pkg gets the package and its .sha256, DIR/log the
#                     makepkg logs, DIR/build the build tree, DIR/src the
#                     downloaded sources (DIR/src-local for --local builds)
#   --local REPO REF  build REF (branch, tag or commit) of the local git
#                     checkout REPO instead of the release tag on GitHub, to
#                     test a release before its tag exists; the PKGBUILD in
#                     this directory is left untouched
#   --verbose         record every compiler command line in the build log
#   -- ARGS           passed to makepkg unchanged (e.g. -- -d, -- --sign)
#
# Environment:
#   PLUS_EXTRA_CFLAGS, PLUS_EXTRA_CXXFLAGS
#                     appended to CFLAGS / CXXFLAGS after the portable flags,
#                     e.g. "-isystem DIR/usr/include" for headers that are not
#                     installed system-wide (only for local test builds); the
#                     script stops if they choose another -march or -mtune
#   MAKEPKG_CONF      system makepkg config to start from (/etc/makepkg.conf)
#   Everything else (CMAKE_BUILD_PARALLEL_LEVEL, CMAKE_PREFIX_PATH, PACKAGER,
#   GPGKEY, ...) reaches makepkg and the build unchanged.
#
# LTO and every other option stay as the system config sets them.

set -euo pipefail

readonly MARCH='x86-64-v3'
readonly MTUNE='generic'
readonly PKG_EXT='.pkg.tar.zst'

usage() {
  sed -n '2,/^$/{s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"
}

die() {
  printf 'build-release.sh: %s\n' "$*" >&2
  exit 1
}

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
out=
local_repo=
local_ref=
verbose=0
makepkg_args=()

while (($#)); do
  case $1 in
    --out)
      (($# >= 2)) || die '--out needs a directory'
      out=$2
      shift 2
      ;;
    --out=*)
      out=${1#--out=}
      shift
      ;;
    --local)
      (($# >= 3)) || die '--local needs a repository path and a ref'
      local_repo=$2
      local_ref=$3
      shift 3
      ;;
    --verbose)
      verbose=1
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    --)
      shift
      makepkg_args=("$@")
      break
      ;;
    *)
      die "unknown argument '$1' (makepkg arguments go after --)"
      ;;
  esac
done

[[ -n ${out} ]] || die '--out DIR is required (see --help)'
command -v makepkg >/dev/null || die 'makepkg not found'
[[ -f ${here}/PKGBUILD ]] || die "no PKGBUILD in ${here}"

# Checked before anything is created under --out.
if [[ -n ${local_repo} ]]; then
  local_repo=$(realpath -e -- "${local_repo}") ||
    die "--local: no such directory '${local_repo}'"
  # makepkg reads '#' and '?' in a source URL as fragment and query.
  [[ ${local_repo} != *[\#\?]* ]] ||
    die "--local: the path may not contain '#' or '?': ${local_repo}"
  commit=$(git -C "${local_repo}" rev-parse --verify --quiet \
    --end-of-options "${local_ref}^{commit}") ||
    die "--local: '${local_ref}' is not a commit in ${local_repo}"
fi

out=$(realpath -m -- "${out}")
mkdir -p -- "${out}"/{build,log,pkg}
tmp=$(mktemp -d "${out}/.build-release.XXXXXX")
trap 'rm -rf -- "${tmp}"' EXIT

# --- makepkg config: the system's, then portable -march/-mtune ---------------

# The same files makepkg itself reads without --config.
base_conf=${MAKEPKG_CONF:-/etc/makepkg.conf}
# The generated config sources it from makepkg's directory, not this one.
[[ ${base_conf} == /* ]] || base_conf=${PWD}/${base_conf}
[[ -r ${base_conf} ]] || die "${base_conf} not found"
conf_files=("${base_conf}")
for f in "${base_conf}.d"/*.conf; do
  [[ -r ${f} ]] && conf_files+=("${f}")
done
if [[ ${base_conf} == /etc/makepkg.conf ]]; then
  if [[ -r ${XDG_CONFIG_HOME:-${HOME}/.config}/pacman/makepkg.conf ]]; then
    conf_files+=("${XDG_CONFIG_HOME:-${HOME}/.config}/pacman/makepkg.conf")
  elif [[ -r ${HOME}/.makepkg.conf ]]; then
    conf_files+=("${HOME}/.makepkg.conf")
  fi
fi

# Prints one variable as the config files above set it.
system_value() {
  # shellcheck disable=SC1090
  (set +eu; for f in "${@:2}"; do source "${f}"; done; printf '%s' "${!1-}")
}

# Replaces every -march/-mtune/-mcpu in a flag string with the portable pair.
portable_flags() {
  local -a words kept=()
  local word
  read -r -d '' -a words <<<"$1" || true
  for word in "${words[@]}"; do
    case ${word} in
      -march=* | -mtune=* | -mcpu=*) ;;
      *) kept+=("${word}") ;;
    esac
  done
  printf '%s' "-march=${MARCH} -mtune=${MTUNE}${kept[*]+ ${kept[*]}}"
}

cflags=$(portable_flags "$(system_value CFLAGS "${conf_files[@]}")")
cxxflags=$(portable_flags "$(system_value CXXFLAGS "${conf_files[@]}")")
# Source paths compiled into the binaries (__FILE__) name the debug source
# directory, as with makepkg's debug option, instead of this build directory.
prefix_map="-ffile-prefix-map=${out}/build/mocktail-plus/src=/usr/src/debug/mocktail-plus"
cflags+=" ${prefix_map}"
cxxflags+=" ${prefix_map}"
cflags+=${PLUS_EXTRA_CFLAGS:+ ${PLUS_EXTRA_CFLAGS}}
cxxflags+=${PLUS_EXTRA_CXXFLAGS:+ ${PLUS_EXTRA_CXXFLAGS}}

conf=${tmp}/makepkg.conf
{
  printf '# Generated by build-release.sh: the system config, then portable code.\n'
  for f in "${conf_files[@]}"; do
    printf 'source %q\n' "${f}"
  done
  printf 'CFLAGS=%q\n' "${cflags}"
  printf 'CXXFLAGS=%q\n' "${cxxflags}"
  printf 'PKGEXT=%q\n' "${PKG_EXT}"
} >"${conf}"

# Every CPU choice makepkg will pass must be the portable one, including any
# from PLUS_EXTRA_* and from LDFLAGS/LTOFLAGS (with LTO, code is generated at
# link time).
for v in CFLAGS CXXFLAGS LDFLAGS LTOFLAGS; do
  value=$(system_value "${v}" "${conf}")
  read -r -d '' -a words <<<"${value}" || true
  for word in "${words[@]}"; do
    case ${word} in
      "-march=${MARCH}" | "-mtune=${MTUNE}") ;;
      -march=* | -mtune=* | -mcpu=*)
        die "${v} targets another CPU with ${word}: ${value}"
        ;;
    esac
  done
  if [[ ${v} == C*FLAGS ]]; then
    [[ " ${value} " == *" -march=${MARCH} "* ]] ||
      die "${v} lost -march=${MARCH}: ${value}"
  fi
done

# --- where makepkg runs ------------------------------------------------------

srcdest=${out}/src
workdir=${here}
if [[ -n ${local_repo} ]]; then
  # A temporary copy of the PKGBUILD whose mocktail source is the local commit.
  workdir=${tmp}/pkgbuild
  mkdir -p -- "${workdir}"
  cp -- "${here}/PKGBUILD" "${workdir}/PKGBUILD"
  {
    printf '\n# --- build-release.sh --local: %q (%s) of %q ---\n' \
      "${local_ref}" "${commit}" "${local_repo}"
    printf 'for _i in "${!source[@]}"; do\n'
    printf '  if [[ ${source[_i]} == mocktail::* ]]; then\n'
    printf '    source[_i]=%q\n' "mocktail::git+file://${local_repo}#commit=${commit}"
    printf '    sha256sums[_i]=SKIP\n'
    printf '  fi\n'
    printf 'done\n'
    printf 'unset _i\n'
  } >>"${workdir}/PKGBUILD"

  srcinfo=$(cd -- "${workdir}" && makepkg --config "${conf}" --printsrcinfo)
  grep -Fqx $'\tsource = '"mocktail::git+file://${local_repo}#commit=${commit}" \
    <<<"${srcinfo}" || die '--local: the PKGBUILD copy kept its GitHub source'

  # A clone of the local repository can not share the mirror of the GitHub one.
  srcdest=${out}/src-local
fi
mkdir -p -- "${srcdest}"

# --- build -------------------------------------------------------------------

export SRCDEST=${srcdest}
export BUILDDIR=${out}/build
export PKGDEST=${out}/pkg
export LOGDEST=${out}/log
export PKGEXT=${PKG_EXT}
if ((verbose)); then
  export VERBOSE=1 # cmake --build prints every command line
fi

printf '==> CFLAGS:   %s\n' "$(system_value CFLAGS "${conf}")"
printf '==> CXXFLAGS: %s\n' "$(system_value CXXFLAGS "${conf}")"
printf '==> LDFLAGS:  %s\n' "$(system_value LDFLAGS "${conf}")"
printf '==> LTOFLAGS: %s\n' "$(system_value LTOFLAGS "${conf}")"
if [[ -n ${local_repo} ]]; then
  printf '==> Source:   %s at %s (%s), NOT the release tag\n' \
    "${local_repo}" "${local_ref}" "${commit}"
fi

cd -- "${workdir}"
makepkg --config "${conf}" --cleanbuild --force --log "${makepkg_args[@]}"

mapfile -t packages < <(makepkg --config "${conf}" --packagelist)
((${#packages[@]})) || die 'makepkg listed no packages'

cd -- "${PKGDEST}"
for pkg in "${packages[@]}"; do
  [[ -f ${pkg} ]] || die "makepkg did not produce ${pkg}"
  name=${pkg##*/}
  sha256sum -- "${name}" >"${name}.sha256"
  printf '==> Package:  %s\n' "${pkg}"
  printf '==> SHA-256:  %s\n' "${pkg}.sha256"
done
printf '==> Logs:     %s\n' "${LOGDEST}"
if [[ -n ${local_repo} ]]; then
  printf '==> This package was built from %s, not from the release tag: do not publish it.\n' \
    "${commit}"
fi
