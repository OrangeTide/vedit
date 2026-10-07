#!/bin/sh
# Regenerate the README screenshots (docs/shot-*.png) from the real binary.
#
# The shots are captured headlessly so they are reproducible: a throwaway X
# server (Xvfb) runs one xterm per shot, the editor is driven with xdotool,
# and the window is grabbed with ImageMagick's import. Needs these on PATH:
#
#     Xvfb  xterm  xdotool  import   (plus a built ./vedit)
#
# Usage: docs/screenshots.sh [path-to-vedit]
# Default binary is ../vedit relative to this script.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
vedit=${1:-$here/../vedit}

if [ ! -x "$vedit" ]; then
	echo "screenshots: no vedit binary at $vedit (run make first)" >&2
	exit 1
fi
# Resolve to an absolute path: the render step runs xterm from a temp dir, so a
# relative binary path would no longer point at the editor.
vedit=$(CDPATH= cd -- "$(dirname -- "$vedit")" && pwd)/$(basename -- "$vedit")
for tool in Xvfb xterm xdotool import; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "screenshots: missing required tool: $tool" >&2
		exit 1
	}
done

# A unicode char-cell bitmap font: narrow 10x20 cells that match the original
# shots, and full box-drawing/block coverage so the frame and scrollbar render.
font='-misc-fixed-medium-r-normal--20-200-75-75-c-100-iso10646-1'
geom=84x26

# Pick a display number that is not already taken by an X server.
dnum=99
while [ -e "/tmp/.X${dnum}-lock" ] || [ -e "/tmp/.X11-unix/X${dnum}" ]; do
	dnum=$((dnum + 1))
done
disp=":$dnum"

work=$(mktemp -d)
xvfb_pid=
cleanup() {
	[ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null || true
	rm -rf "$work"
}
trap cleanup EXIT INT TERM

# Sample files, authored to fill the frame the way the README text describes.
cat > "$work/ring.c" <<'EOF'
/* ring buffer: a tiny fixed-capacity queue of ints */
#include <stdlib.h>

typedef struct ring {
	int	*buf;
	size_t	cap, head, len;
} Ring;

/* Push one value, dropping the oldest when the ring is full. */
int ring_push(Ring *r, int v)
{
	size_t i = (r->head + r->len) % r->cap;

	r->buf[i] = v;
	if (r->len < r->cap)
		return r->len++, 0;
	r->head = (r->head + 1) % r->cap;	/* overwrite oldest */
	return 1;
}
EOF

cat > "$work/pipeline.txt" <<'EOF'
Pipeline

+----------+     +----------+     +--------+
|  lexer   |---->|  parser  |---->|  eval  |
+----------+     +----------+     +--------+
     |                                |
     v                                v
   tokens                          result
EOF

# A short colored "build log" for the terminal-buffer shot: an OSC title, SGR
# colors, and a truecolor bar, written as raw bytes so that `cat` prints them
# verbatim and the embedded VT emulator is what parses them.
{
	printf '\033]2;build: ring\007'
	printf '\033[1;32m==>\033[0m building \033[1mring\033[0m\n'
	printf '    \033[36mcc\033[0m -Wall -O2 -c ring.c\n'
	printf '    \033[36mcc\033[0m -o ring ring.o\n'
	printf '\033[1;32m==>\033[0m \033[32mok\033[0m, no warnings\n\n'
	printf 'truecolor: '
	i=0
	while [ "$i" -lt 36 ]; do
		r=$((i * 7))
		printf '\033[48;2;%d;90;%dm \033[0m' "$r" "$((252 - r))"
		i=$((i + 1))
	done
	printf '\n'
} > "$work/term.txt"

# A spreadsheet-ish CSV for the table view: a header row, quoted fields with
# commas inside, and columns of different widths for :colwidth fit to size.
cat > "$work/parts.csv" <<'EOF'
sku,part,qty,unit,supplier,notes
RB-1024,"Ring buffer, 1K",12,4.50,Acme Metals,"spare, boxed"
RB-4096,"Ring buffer, 4K",3,9.75,Acme Metals,
LX-0001,Lexer,1,120.00,"Parsers, Inc.",prototype
PS-0002,Parser,1,180.00,"Parsers, Inc.","needs lexer"
EV-0003,Evaluator,2,95.00,Northwind,
CB-0100,Cable,40,1.20,Northwind,"1 m, black"
CB-0300,Cable,15,2.40,Northwind,"3 m, black"
SW-0010,Switch,8,3.10,Acme Metals,momentary
LD-0020,LED,100,0.08,Lumen Co,red
LD-0021,LED,100,0.08,Lumen Co,green
PB-0007,Power brick,4,22.00,Northwind,"12 V, 2 A"
EOF

# A small piece of coloured text art for the art view, written as the SGR
# sequences a .ans file carries (UTF-8 glyphs, since the view edits Unicode).
# The letters are three cells wide and three rows tall with one cell between
# them (19 cells), so the double-line box is 23 cells across with its borders.
{
	Y='\033[1;33m' C='\033[1;36m' c='\033[36m' R='\033[0m'
	TL='\342\225\224' TR='\342\225\227' BL='\342\225\232' BR='\342\225\235'
	H='\342\225\220' V='\342\225\221'
	F='\342\226\210' U='\342\226\200' L='\342\226\204'	# full, upper, lower block
	HH="$H$H$H$H$H$H$H$H$H$H$H$H$H$H$H$H$H$H$H$H$H"		# 21 of them
	#        V         E         D         I         T
	r1="$F $F $F$U$U $F$U$L $U$F$U $U$F$U"
	r2="$F $F $F$U$U $F $F  $F   $F "
	r3=" $U  $U$U$U $U$U  $U$U$U  $U "
	printf "$Y$TL$HH$TR$R\n"
	printf "$Y$V$R $C$r1$R $Y$V$R\n"
	printf "$Y$V$R $C$r2$R $Y$V$R\n"
	printf "$Y$V$R $c$r3$R $Y$V$R\n"
	printf "$Y$BL$HH$BR$R\n"
	printf '\033[41m  \033[43m  \033[42m  \033[46m  \033[44m  \033[45m  \033[47m  \033[0m \033[1;31mr\033[1;33me\033[1;32md\033[1;36m \033[1;34mb\033[1;35mo\033[1;37mx\033[0m\n'
	printf '\033[2m\342\226\221\342\226\221\033[0m\342\226\222\342\226\222\033[1m\342\226\223\342\226\223\342\226\210\342\226\210\033[0m shades\n'
} > "$work/logo.ans"

# A build command for C, so the context-sensitive Compile and Run menus are
# present in the shots (they hide when no command is configured for the file).
cat > "$work/veditrc" <<'EOF'
[command "c"]
    compile = cc -Wall -c $(filename)
    build   = make
    run     = ./$(filenoext)
EOF

# A small Maildir++ tree for the mail shot: INBOX with three messages (one
# unread, one answered) and a Sent folder. mail.dir points the editor at it.
mkdir -p "$work/Maildir/new" "$work/Maildir/cur" "$work/Maildir/tmp" \
    "$work/Maildir/.Sent/cur" "$work/Maildir/.Sent/new" "$work/Maildir/.Sent/tmp"
cat > "$work/Maildir/new/1791300000.a.host" <<'EOF2'
From: Ann Example <ann@example.org>
To: jon@example.org
Subject: lunch tomorrow?
Date: Tue, 7 Oct 2026 10:00:00 +0000
Message-ID: <m1@example.org>

Soup or noodles? The place on the corner has both now.

Ann
EOF2
cat > "$work/Maildir/cur/1791200000.b.host:2,RS" <<'EOF2'
From: Bob Builder <bob@example.org>
To: jon@example.org
Subject: Re: build is green again
Date: Mon, 6 Oct 2026 09:30:00 +0000

All targets pass on the arm runner now.
EOF2
cat > "$work/Maildir/new/1791100000.c.host" <<'EOF2'
From: lists <vedit@lists.example.org>
To: vedit@lists.example.org
Subject: [vedit] 1.3.0 released
Date: Sun, 5 Oct 2026 18:12:00 +0000

The 1.3.0 release is up: pane, table view, art view.
EOF2

# Run the X server in its own session so its lifetime is not tied to this
# shell's process group, then wait for the socket to appear.
setsid Xvfb "$disp" -screen 0 1600x1200x24 >/dev/null 2>&1 &
xvfb_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -e "/tmp/.X11-unix/X${dnum}" ] && break
	sleep 0.3
done

export DISPLAY=$disp
# xterm sets TERM itself (see -tn below) and strips COLORTERM, so the editor's
# colour depth comes from its own VEDIT_COLORS, which xterm passes through.
export VEDIT_COLORS=256
export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
export VEDIT_CONFIG=$work/veditrc
# and no fallback to the developer's own files, should VEDIT_CONFIG ever be
# dropped from a render
export XDG_CONFIG_HOME=$work/xdg
export HOME=$work/home
mkdir -p "$work/xdg" "$work/home"

# render OUT FILE KEYSTEP...   each KEYSTEP is one xdotool key invocation.
render() {
	out=$1 file=$2
	shift 2

	xterm -geometry "$geom" -fn "$font" +sb -bg black -fg white -b 0 -bw 0 \
	    -xrm 'xterm*internalBorder: 2' \
	    -xrm 'XTerm*allowSendEvents: true' \
	    -xrm 'XTerm*metaSendsEscape: true' \
	    -xrm 'XTerm*cursorBlink: false' \
	    -tn xterm-256color \
	    -e "$vedit" "$(basename "$file")" &
	xterm_pid=$!
	sleep 2

	wid=$(xdotool search --class xterm | head -1)
	xdotool windowfocus "$wid" 2>/dev/null || true
	xdotool windowraise "$wid" 2>/dev/null || true
	sleep 0.3

	for step in "$@"; do
		# shellcheck disable=SC2086
		xdotool key --window "$wid" --clearmodifiers $step
		sleep 0.4
	done
	sleep 0.5

	import -window "$wid" "$out"
	kill "$xterm_pid" 2>/dev/null || true
	wait "$xterm_pid" 2>/dev/null || true
	echo "wrote $out"
}

cd "$work"

# A C file, freshly opened: syntax highlighting and the MS-EDIT chrome. No
# keystroke, so the status line keeps its pristine "Press F1 for help" hint.
render "$here/shot-edit.png" ring.c

# The same file with the Edit menu dropped (Alt+E).
render "$here/shot-menu.png" ring.c ctrl+Home alt+e

# A box diagram in draw mode, free cursor parked on line 11.
render "$here/shot-draw.png" pipeline.txt \
    ctrl+Home Insert \
    "Down Down Down Down Down Down Down Down Down Down"

# A terminal buffer: F2 switches to vi keys, then the ex line :terminal runs
# cat on the colored log. The built-in VT emulator parses the child's output
# and the editor draws it in its own frame, with the OSC title as the label.
render "$here/shot-term.png" ring.c \
    F2 "colon t e r m i n a l space c a t space t e r m period t x t Return"

# The pane: the same build log runs under the text with :split cat term.txt.
# A new pane leaves the focus in the file, so the cursor stays on line 1.
render "$here/shot-pane.png" ring.c \
    F2 "colon s p l i t space c a t space t e r m period t x t Return"

# The table view: a .csv opens as a grid. :colwidth fit all sizes every column
# to its cells, then the cursor moves to the quoted supplier on row 4.
render "$here/shot-table.png" parts.csv \
    F2 "colon c o l w i d t h space f i t space a l l Return" \
    "Down Down Down Right Right Right Right"

# The art view: a .ans opens as a cell grid; the status line shows the pen.
render "$here/shot-art.png" logo.ans \
    "Down Down Down Down Down Right Right"


# Version control: a copy of ring.c committed twice in its own repository, so
# the bar carries the VCS menu; :log lists the commits over the file.
if command -v git >/dev/null 2>&1; then
	mkdir -p "$work/repo"
	cp ring.c "$work/repo/ring.c"
	(
		cd "$work/repo"
		# fixed dates, so the hashes and the shot are the same on every run
		export GIT_AUTHOR_DATE="2026-10-07T12:00:00+0000"
		export GIT_COMMITTER_DATE="2026-10-07T12:00:00+0000"
		git init -q -b main
		git -c user.name=Jon -c user.email=jon@example.org add ring.c
		git -c user.name=Jon -c user.email=jon@example.org commit -q \
		    -m "Ring buffer: first cut"
		sed -i 's/ring/ringbuf/' ring.c
		git -c user.name=Jon -c user.email=jon@example.org commit -q -a \
		    -m "Rename ring to ringbuf"
		render "$here/shot-history.png" ring.c F2 "colon l o g Return"
	)
fi
# The mail config goes in only now, so the shots above have no Mail menu.
cat >> "$work/veditrc" <<EOF2
[mail]
    dir = $work/Maildir
    from = Jon Mayo <jon@example.org>
EOF2

# Mail: :mail picks INBOX from the folder list and opens the newest message
# into a buffer (headers on top); :mail . then lists the folder again, so the
# index shows over the message with its unread and answered markers.
render "$here/shot-mail.png" ring.c \
    F2 "colon m a i l Return" Return Return \
    "colon m a i l space period Return"

exit 0
