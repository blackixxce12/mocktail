#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0
#
# Every string of the settings window is translated: each source file with
# _() is listed in po/POTFILES.in, and each catalogue in po/LINGUAS compiles
# cleanly (msgfmt --check) and has a translation for every msgid the sources
# use (msgcmp counts untranslated and fuzzy messages as missing).

set -Eeuo pipefail

readonly source_dir="${1:?source directory is required}"
temporary="$(mktemp -d)"
trap 'rm -rf -- "${temporary}"' EXIT
cd -- "${source_dir}"
export LC_ALL=C

mapfile -t sources < <(grep -v '^#' po/POTFILES.in | grep .)
for file in src/launcher_ui/*.cc; do
  if grep -Eq '(^|[^A-Za-z0-9_])(N?_|ngettext)\("' "${file}" &&
     ! printf '%s\n' "${sources[@]}" | grep -Fxq "${file}"; then
    echo "${file} has translatable strings but is not in po/POTFILES.in" >&2
    exit 1
  fi
done

xgettext --from-code=UTF-8 --language=C++ --keyword=_ --keyword=N_ \
  --keyword=ngettext:1,2 --output="${temporary}/mocktail.pot" \
  "${sources[@]}"

while read -r language; do
  [[ -z "${language}" || "${language}" == \#* ]] && continue
  catalogue="po/${language}.po"
  statistics="$(msgfmt --check --statistics --output-file=/dev/null \
    "${catalogue}" 2>&1)"
  if grep -Eq 'fuzzy|untranslated' <<<"${statistics}"; then
    echo "${catalogue}: ${statistics}" >&2
    exit 1
  fi
  msgcmp "${catalogue}" "${temporary}/mocktail.pot"
done <po/LINGUAS

echo 'settings window translations are complete'
