#!/bin/sh
# Opens a crash minidump from build/logs in cdb, with the PDBs and images the crash handler copied into
# <run>_symbols (the build deletes or overwrites the ones in build/), and runs cdb commands on it.
#   analyze_dump.sh                       newest dump: crash context, stack, locals of the crashing frame
#   analyze_dump.sh DUMP.dmp              that dump, same commands
#   analyze_dump.sh DUMP.dmp "COMMANDS"   that dump, these cdb commands (';' separated)
cd "$(dirname "$0")/../../../.." || exit 1 # term/

Dump="$1"
if [ -z "$Dump" ]; then
    Dump=$(ls -t build/logs/*.dmp 2>/dev/null | head -1)
fi
if [ ! -f "$Dump" ]; then
    echo "analyze_dump: no dump '${Dump}' (looked in build/logs)"
    exit 1
fi
Commands="${2:-.ecxr; kn 30; .frame 0; dv /t /v}"

Symbols="${Dump%.dmp}_symbols"
if [ ! -d "$Symbols" ]; then
    echo "analyze_dump: no $Symbols: term.exe frames will not resolve unless build/ still holds the same build"
    Symbols=build
fi

Cdb=$(command -v cdbX64.exe || command -v cdb.exe)
if [ -z "$Cdb" ] && [ -f "/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe" ]; then
    Cdb="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"
fi
if [ -z "$Cdb" ]; then
    echo "analyze_dump: cdb not found. Install WinDbg: winget install Microsoft.WinDbg (gives cdbX64.exe)"
    exit 1
fi

SymbolsW=$(cygpath -w "$Symbols")
Cache=$(cygpath -w "${LOCALAPPDATA:-$HOME}/cdb_symbols")
echo "dump: $Dump"
echo "symbols: $Symbols"
MSYS_NO_PATHCONV=1 "$Cdb" -z "$(cygpath -w "$Dump")" \
    -y "$SymbolsW;srv*$Cache*https://msdl.microsoft.com/download/symbols" -i "$SymbolsW" \
    -lines -c "$Commands; q" 2>&1 | grep -v "NatVis script"
