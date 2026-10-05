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

# Run the X server in its own session so its lifetime is not tied to this
# shell's process group, then wait for the socket to appear.
setsid Xvfb "$disp" -screen 0 1600x1200x24 >/dev/null 2>&1 &
xvfb_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -e "/tmp/.X11-unix/X${dnum}" ] && break
	sleep 0.3
done

export DISPLAY=$disp
export TERM=xterm-256color COLORTERM=truecolor
export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

# render OUT FILE KEYSTEP...   each KEYSTEP is one xdotool key invocation.
render() {
	out=$1 file=$2
	shift 2

	xterm -geometry "$geom" -fn "$font" +sb -bg black -fg white -b 0 -bw 0 \
	    -xrm 'xterm*internalBorder: 2' \
	    -xrm 'XTerm*allowSendEvents: true' \
	    -xrm 'XTerm*metaSendsEscape: true' \
	    -xrm 'XTerm*cursorBlink: false' \
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

exit 0
