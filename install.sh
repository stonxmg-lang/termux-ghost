#!/data/data/com.termux/files/usr/bin/bash
# Builds termux-ghost and enables it for new interactive bash sessions.
#   ./install.sh             install or update
#   ./install.sh --uninstall remove it
set -euo pipefail
cd "$(dirname "$0")"
DEST="$HOME/.local/lib/termux-ghost"
RC="$HOME/.bashrc"
MARK='# termux-ghost'
LINE="if [[ \$- == *i* ]]; then enable -f \"\$HOME/.local/lib/termux-ghost/ghost.so\" ghost 2>/dev/null; fi $MARK"

remove_line() {
  [[ -f $RC ]] || return 0
  local tmp; tmp=$(mktemp)
  command grep -vF "$MARK" "$RC" > "$tmp" || true
  cat "$tmp" > "$RC"
  rm -f "$tmp"
}

if [[ ${1-} == --uninstall ]]; then
  remove_line
  rm -rf "$DEST"
  echo "removed. open a new session to finish."
  exit 0
fi

P=${PREFIX:-/data/data/com.termux/files/usr}
[[ -f $P/include/readline/readline.h ]] || pkg install -y readline
command -v clang >/dev/null || pkg install -y clang
command -v make >/dev/null || pkg install -y make
make -s PREFIX="$P"
mkdir -p "$DEST"
# Replace rather than overwrite: running shells have the old file mapped.
install -m 700 ghost.so "$DEST/ghost.so.new"
mv -f "$DEST/ghost.so.new" "$DEST/ghost.so"
remove_line
printf '%s\n' "$LINE" >> "$RC"
echo "installed. open a new session, or run: source ~/.bashrc"
