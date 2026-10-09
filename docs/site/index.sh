#!/bin/sh
# Generate the manual's landing page to stdout.
#
# Usage: index.sh <chapter.md>...   (in reading order; developing.md last)
#
# Each entry's title is the file's first level-1 heading and its blurb is
# the first paragraph after it, so a chapter describes itself.

set -eu

doc_title() {
    sed -n 's/^# //p' "$1" | head -1
}

# The first paragraph after the heading, joined onto one line.
doc_blurb() {
    awk 'f && /^$/ { exit } f { printf "%s ", $0 } /^# / { f = 1; getline }' "$1"
}

cat <<'HEAD'
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>vedit manual</title>
  <link rel="stylesheet" href="style.css">
  <link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:wght@400;600&family=IBM+Plex+Mono:wght@400;600&display=swap">
</head>
<body>
  <nav class="top-bar">
    <span class="back-link">vedit manual</span>
    <div class="chapter-links">
      <a href="https://github.com/OrangeTide/vedit">GitHub</a>
    </div>
  </nav>
  <div class="page">
    <article class="doc index">
      <header>
        <h1>vedit</h1>
      </header>
      <p class="lede">
        A text editor for terminals, in one C file. It looks and works like
        the MS-DOS EDIT program. Press F2 and it becomes a vi.
      </p>
      <img src="shot-edit.png" alt="Editing a C file, with syntax highlighting and the MS-EDIT chrome">
      <p>
        The chapters are in reading order. The first ones cover what
        everyone uses, the vi keys and the tools come later, and the last
        chapters cover the unusual features.
      </p>
      <ol>
HEAD

extra=
for md in "$@"; do
    base=$(basename "$md" .md)
    title=$(doc_title "$md")
    blurb=$(doc_blurb "$md")
    case "$base" in
    [0-9][0-9]-*)
        printf '        <li><div><a href="%s.html">%s</a><span>%s</span></div></li>\n' \
            "$base" "$title" "$blurb"
        ;;
    *)
        extra="$extra$(printf '        <li><a href="%s.html">%s</a>: %s</li>\n' \
            "$base" "$title" "$blurb")
"
        ;;
    esac
done

printf '      </ol>\n'
if [ -n "$extra" ]; then
    printf '      <h2>Also</h2>\n      <ul class="extra">\n%s      </ul>\n' "$extra"
fi

cat <<'FOOT'
    </article>
  </div>
  <footer class="bottom-bar">
    <a href="https://github.com/OrangeTide/vedit">github.com/OrangeTide/vedit</a>
  </footer>
</body>
</html>
FOOT
