#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
tools="${LANTERN_RE_TOOLS:-E:/re-tools}"
eboot="${LANTERN_EBOOT:-E:/bloodbrone/BloodbornePC/out/eboot.elf}"
work="$tools/work"
export JAVA_HOME="$(ls -d "$tools"/jdk-* | head -1)"
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-6G}"
mkdir -p "$tools/settings"
export JAVA_TOOL_OPTIONS="-Dapplication.settingsdir=$(cygpath -w "$tools/settings")"
headless="$(ls -d "$tools"/ghidra_*_PUBLIC | head -1)/support/analyzeHeadless.bat"
project=(cmd //c "$headless" "$(cygpath -w "$tools/projects")" Bloodborne)
mkdir -p "$tools/projects"

case "${1:-}" in
import)
	python "$here/ghidra_elf.py" "$eboot" "$work"
	"${project[@]}" -import "$(cygpath -w "$work/eboot_ghidra.elf")" -overwrite \
		-loader ElfLoader -processor x86:LE:64:default -cspec gcc \
		-scriptPath "$(cygpath -w "$here/ghidra")" \
		-preScript LanternSeed.java "$(cygpath -w "$work")" \
		-postScript LanternDecompile.java "$(cygpath -w "$work/export")"
	;;
decompile)
	shift
	"${project[@]}" -process eboot_ghidra.elf -noanalysis -readOnly \
		-scriptPath "$(cygpath -w "$here/ghidra")" \
		-postScript LanternDecompile.java "$(cygpath -w "$work/decomp")" "$@"
	;;
dump)
	shift
	"${project[@]}" -process eboot_ghidra.elf -noanalysis -readOnly \
		-scriptPath "$(cygpath -w "$here/ghidra")" \
		-postScript LanternDump.java "$(cygpath -w "$work/dump")" "$@"
	;;
*)
	echo "usage: $0 import | decompile <address>... | dump [from to]" >&2
	exit 1
	;;
esac
