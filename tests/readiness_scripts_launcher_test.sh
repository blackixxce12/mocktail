#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0
#
# The readiness gates (scripts/real_bringup_smoke.sh), the crash loop
# (scripts/auto_runtime_loop.sh) and the runtime matrix
# (scripts/runtime_matrix.sh) run mocktail unattended, with a display.
# With launcher.show_on_start on (the default) the settings window would
# open and wait for Play until the gate's timeout killed it, so they pass
# --play. Only tier C's interactive launch, the one with
# MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS=0 that runs until the user closes
# Roblox, keeps the window.

set -Eeuo pipefail

readonly ROOT="${1:?source root is required}"
temporary="$(mktemp -d)"
trap 'rm -rf -- "${temporary}"' EXIT

Fail() {
  printf 'readiness scripts launcher test failed: %s\n' "$*" >&2
  exit 1
}

# A stand-in for build/mocktail that records its arguments, one per line.
cat >"${temporary}/mocktail" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" >"${MOCKTAIL_TEST_ARGUMENTS}"
exit 0
EOF
chmod +x "${temporary}/mocktail"

# A private cookie file only for the NETWORK tier's checks; the stand-in
# never reads it.
cookie_file="${temporary}/cookie"
printf 'test\n' >"${cookie_file}"
chmod 600 "${cookie_file}"

# Prints the arguments the gate gave the binary. Extra NAME=value
# arguments go into its environment.
GateArguments() {
  local -r tier="$1"
  shift
  local -r recorded="${temporary}/arguments"
  : >"${recorded}"
  env -i PATH=/usr/bin:/bin HOME="${temporary}/home" \
    MOCKTAIL_BIN="${temporary}/mocktail" \
    MOCKTAIL_SKIP_UPDATE_CHECK=1 \
    MOCKTAIL_LOG_DIR="${temporary}/logs" \
    MOCKTAIL_WORKING_DIRECTORY="${temporary}" \
    MOCKTAIL_TEST_ARGUMENTS="${recorded}" \
    "$@" \
    bash "${ROOT}/scripts/real_bringup_smoke.sh" "${tier}" \
    >"${temporary}/gate.log" 2>&1 || true
  [[ -s "${recorded}" ]] ||
    Fail "tier ${tier} did not run the binary: $(tail -5 "${temporary}/gate.log")"
  cat "${recorded}"
}

ExpectPlay() {
  local -r tier="$1"
  shift
  GateArguments "${tier}" "$@" | grep -Fxq -- '--play' ||
    Fail "tier ${tier} ($*) would open the settings window and wait"
}

ExpectPlay C
ExpectPlay C MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS=5000
ExpectPlay GAME
ExpectPlay INPUT
ExpectPlay RESIZE
ExpectPlay NETWORK MOCKTAIL_PLACE_ID=1 MOCKTAIL_COOKIE_FILE="${cookie_file}"

if GateArguments C MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS=0 |
    grep -Fxq -- '--play'; then
  Fail 'the interactive tier C launch no longer offers the settings window'
fi

# The crash loop and the runtime matrix build and run build/mocktail
# themselves, so only their command lines are checked.
for script in auto_runtime_loop.sh runtime_matrix.sh; do
  grep -Eq '^[[:space:]]+"\$\{BINARY\}" --play ' \
    "${ROOT}/scripts/${script}" ||
    Fail "scripts/${script} starts mocktail without --play"
done

echo 'readiness scripts skip the settings window'
