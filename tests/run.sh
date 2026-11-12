#!/data/data/com.termux/files/usr/bin/bash
# End-to-end tests in a real terminal (tmux). Uses a throwaway history file.
T=/data/data/com.termux/files/usr
export PATH=$T/bin:$PATH
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d "$T/tmp/ghost-test.XXXXXX")
SOCK=ghost-test-$$
pass=0; fail=0
tm() { "$T/bin/tmux" -L "$SOCK" "$@"; }
trap 'tm kill-server 2>/dev/null; rm -rf "$W"' EXIT

cat > "$W/hist" <<'H'
ls /sdcard/old
ls alpha_file --ghost-marker
pkg install python-numpy
git commit -m first-message
echo hello-world
echo "مرحبا بالعالم"
echo 日本語テキスト
export GH_USER=stonx
export GH_TOKEN=ghp_secret-marker
curl https://example.com/a/very/long/path/that/keeps/going/and/going/until/it/wraps/around/the/narrow/phone/screen/more/than/one/row
H
touch "$W/alpha_file" "$W/alpha_other"
cat > "$W/rc" <<RC
PS1='\$ '
HISTFILE=$W/hist
HISTSIZE=${HISTN:-1000}
cd $W
enable -f $ROOT/ghost.so ghost
RC

start() { # cols rows
  tm kill-server 2>/dev/null
  tm -f /dev/null new-session -d -s t -x "$1" -y "$2" \
    "env -i HOME=$W PREFIX=$T PATH=$T/bin TMPDIR=$T/tmp TERM=xterm-256color LANG=en_US.UTF-8 $T/bin/bash --rcfile $W/rc -i"
  tm set -g status off
  tm set -g escape-time 0
  settle
}
settle() { local a b; for _ in 1 2 3 4 5 6 7 8 9 10; do b=$(tm capture-pane -p -e -t t; tm display -p -t t '#{cursor_x},#{cursor_y}'); [[ $a == "$b" && -n $a ]] && return; a=$b; "$T/bin/sleep" 0.08; done; }
keys() { tm send-keys -t t "$@"; settle; }
lit()  { tm send-keys -t t -l "$1"; settle; }
screen() { tm capture-pane -p -t t; }
escscreen() { tm capture-pane -p -e -t t; }
cursor() { tm display -p -t t '#{cursor_x},#{cursor_y}'; }
ok() { # name, condition result
  if [[ $2 == 0 ]]; then pass=$((pass+1)); echo "ok   $1"; else fail=$((fail+1)); echo "FAIL $1"; echo "$3" | sed 's/^/     | /'; fi
}
has()  { [[ $(screen) == *"$2"* ]]; ok "$1" $? "$(screen)"; }
hasnt(){ [[ $(screen) != *"$2"* ]]; ok "$1" $? "$(screen)"; }
curis(){ [[ $(cursor) == "$2" ]]; ok "$1" $? "cursor=$(cursor) want=$2
$(screen)"; }

start 80 24
lit 'echo h'
has "suggestion appears" 'echo hello-world'
curis "cursor stays after typed text" "8,0"
[[ $(escscreen) == *$'\e[90m'*ello-world* ]]; ok "suggestion is dim" $? "$(escscreen)"

lit 'x'
hasnt "mismatch removes suggestion" 'hello-world'
keys BSpace
has "backspace restores suggestion" 'echo hello-world'
keys Enter
has "enter runs only what was typed" $'echo h\nh'
hasnt "enter leaves no ghost" 'hello-world'

lit 'echo h'; keys Right
curis "right arrow accepts, cursor at end" "18,$(tm display -p -t t '#{cursor_y}')"
keys Enter
has "accepted command runs" $'hello-world\n'

lit 'git'; keys M-Right
has "alt-right: first word shown" 'git commit'
lit ' -m'
has "alt-right then more typing keeps suggestion" 'git commit -m first-message'
keys C-u

lit 'pkg in'; keys End
has "end key accepts" 'pkg install python-numpy'
keys C-u
hasnt "ctrl-u clears everything" 'pkg install'

lit 'git c'; keys C-c
settle
hasnt "ctrl-c leaves no ghost" 'mit -m first'
has "ctrl-c shows new prompt" '^C'

keys C-l
lit 'echo h'; keys Left
hasnt "cursor mid-line hides suggestion" 'hello-world'
keys Right
curis "right arrow still moves in mid-line" "8,$(tm display -p -t t '#{cursor_y}')"
keys C-u

keys C-l
lit 'ls alpha_'
has "suggestion before tab" 'ghost-marker'
keys Tab; keys Tab; keys Tab
has "tab lists completions" 'alpha_other'
n=$(screen | "$T/bin/grep" -c 'ghost-marker')
[[ $n -le 1 ]]; ok "tab list leaves no stale suggestion" $? "$(screen)"
keys C-u C-l

lit 'git c'; keys C-l
curis "ctrl-l keeps line at top" "7,0"
has "ctrl-l repaints suggestion" 'git commit -m first-message'
keys C-u

keys C-r; lit 'pkg'
has "ctrl-r search works" "reverse-i-search"
keys C-g; settle; keys C-u
hasnt "ctrl-r cancel is clean" 'reverse-i-search'

keys C-l
lit 'read -e x'; keys Enter
lit 'echo h'
hasnt "read -e gets no suggestions" 'hello-world'
keys Enter
keys C-u C-l

lit 'echo "a'; keys Enter
lit 'echo h'
hasnt "continuation line gets no suggestions" 'hello-world'
keys C-c
keys C-l

lit 'echo "م'
has "arabic suggestion" 'مرحبا بالعالم'
keys Right; keys Enter
has "arabic accepted intact" $'\nمرحبا بالعالم'
keys C-l
lit 'echo 日'
curis "wide chars keep cursor" "9,0"
keys Right
has "wide accepted" 'echo 日本語テキスト'
keys C-u C-l

# Secrets: lines matching an ignore pattern are skipped, older safe ones shown.
lit 'export GH_'
has "ignored line skipped, older one shown" 'export GH_USER=stonx'
hasnt "token line never shown" 'secret-marker'
keys C-u
lit "ghost ignore '*GH_USER*'"; keys Enter; keys C-l
lit 'export GH_'
hasnt "added pattern hides line" 'GH_USER=stonx'
keys C-u
lit 'ghost ignore --clear'; keys Enter; keys C-l
lit 'export GH_'
has "clear shows everything" 'secret-marker'
keys C-u
lit 'ghost ignore --reset'; keys Enter; keys C-l
lit 'export GH_'
has "reset restores defaults" 'export GH_USER=stonx'
hasnt "reset hides token again" 'secret-marker'
keys C-u C-l

lit 'ghost off'; keys Enter; keys C-l
lit 'echo h'
hasnt "ghost off works" 'hello-world'
keys C-u
lit 'ghost on'; keys Enter; keys C-l
lit 'echo h'
has "ghost on works" 'hello-world'
keys C-u C-l

# TERM reassignment re-reads the terminal database; it must not go dumb.
lit 'TERM=xterm-256color'; keys Enter; lit 'echo before-clear'; keys Enter; keys C-l
hasnt "ctrl-l still clears after TERM=" 'before-clear'
# A held-down paste must arrive complete.
lit 'echo pasted-text-one && echo pasted-text-two'; keys Enter
has "fast input arrives intact" $'pasted-text-one\npasted-text-two'
keys C-l

# Phone-sized: wrapping suggestions, at the very bottom of the screen.
start 40 8
for i in 1 2 3 4 5 6 7 8 9; do lit "echo $i"; keys Enter; done
lit 'curl'
pos=$(cursor)
has "wrapped suggestion visible" 'keeps/going'
[[ ${pos%,*} == 6 ]]; ok "wrap at bottom: right column" $? "cursor=$pos
$(screen)"
keys Enter
hasnt "wrap at bottom: enter clears all rows" 'keeps/going'
lit 'curl'; keys C-c
hasnt "wrap at bottom: ctrl-c clears all rows" 'keeps/going'
lit 'curl'; keys Right
has "wrap: accepted end" 'one/row'
keys C-u
hasnt "wrap: ctrl-u clears accepted" 'one/row'

start 40 8
lit 'curl'
tm resize-window -t t -x 30 -y 8; settle; "$T/bin/sleep" 0.2
lit 'x'
hasnt "resize then mismatch leaves no ghost" 'keeps/going'
keys C-u

# Unload leaves the shell untouched.
start 80 24
lit 'enable -d ghost'; keys Enter; keys C-l
lit 'echo h'
hasnt "enable -d removes it" 'hello-world'
keys Left; keys Right
curis "right arrow normal after unload" "8,0"

echo; echo "passed=$pass failed=$fail"
[[ $fail == 0 ]]
