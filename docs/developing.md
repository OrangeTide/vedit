# Developing vedit

How the project is built, tested, and released. Users do not need this
chapter.

## Layout

| Path | What it is |
|---|---|
| `vedit.c` | the whole editor, one translation unit |
| `vedit.h` | the public header for embedding |
| `guterm.h` | the vendored window binding used by gvedit |
| `Makefile` | build, test, install, screenshots |
| `man/vedit.1` | the man page |
| `docs/` | this manual, the screenshots and `screenshots.sh` that renders them, `demo.html`, and the sample `.ans` scenes |
| `docs/site/` | the pandoc build of the manual for GitHub Pages (`make -C docs/site`) |
| `tests/` | the test suite and the vendored `taptest` driver |
| `_out/<triple>/bin/` | build output, one directory per target |

Every build lands in `_out/` under the compiler's target triple.
A `vedit` symlink at the top level points at the most recent build.

## Build targets

```sh
make                    # native build, linked as ./vedit
make RELEASE=1          # optimized build
make static             # static musl build
make install            # install to ~/.local/bin and the man page (PREFIX overrides)
make gvedit             # the SDL3 window build, linked as ./gvedit
make guterm-sync GUTERM_DIR=../guterm   # refresh the vendored guterm.h
make clean
```

Cross builds set `CC`. A mingw compiler produces `vedit.exe`:

```sh
make CC=x86_64-w64-mingw32-gcc
```

Feature knobs are make variables that become `-D` defines:

| Variable | Drops |
|---|---|
| `VEDIT_NO_TERM=1` | terminal buffers, the pane's shell, and the pseudo-terminal code |
| `VEDIT_NO_ART=1` | the art view (the VT emulator goes once both this and TERM are out) |

The other knobs are plain defines passed through `CFLAGS`:
`-DVEDIT_NO_TOOLS` (build commands and the output pane), `-DVEDIT_NO_MAIL`,
`-DVEDIT_NO_MOUSE`, and `-DVEDIT_NO_XDG` (use `~/.veditrc` instead of the
XDG config path). Chapter 10 of the manual lists what each one removes from
the user's point of view.

## Tests

```sh
make test               # unit, render, terminal, and fault tests through taptest
make torture            # pseudo-random fuzz of the parsers and regex engine
make asan               # unit and render tests and torture under AddressSanitizer
make ubsan              # the same under UndefinedBehaviorSanitizer
make cov                # line coverage of vedit.c from the unit tests
make cov-term           # line coverage of the terminal-buffer code
```

The tests have no external dependencies. The driver is a vendored copy of
the `taptest` TAP framework (`tests/taptest.c`, `taptest.h`, and
`taptest_selftest.c`, built to `tests/taptest`); `test.h` and
`testmain.c` are the harness.

Each test file includes `vedit.c` as a single unit with `main` renamed, so
it can call the internal helpers directly. `test_unit.c` covers the pure
helpers: UTF-8, rune width, color mapping, word-wrap layout, the syntax
tokenizer, buffer edits and undo. `test_render.c` drives the whole editor
over an in-memory `vedit_io` (`tests/memio.h`) that feeds scripted
keystrokes and captures the output, so cursor movement, wrap, the gutter,
and status flags are tested without a terminal. `test_term.c` drives the
terminal buffers and the pane over a pipe instead of a PTY, and
`test_termfault.c` wraps the allocator to fail on demand.

`tests/torture.c` fuzzes the untrusted-input surfaces: the regex engine as
search and replace use it, the config parser, the UTF-8 codec, the
clipboard, and random edits with undo. It uses
a deterministic PRNG and checks a few invariants, leaning on the sanitizers
to catch memory and undefined-behavior faults. `TORTURE_ROUNDS` sets the
iteration count. To reproduce a run, pass its seed as the second argument to
`tests/torturet`.

## Continuous integration

`.github/workflows/ci.yml` runs on every push to `main` and on every pull
request against it:

- build and test on gcc and clang
- the asan and ubsan suites
- the torture fuzz
- cross builds (aarch64, armhf, riscv64) run under qemu
- a static musl build
- the slim builds (`VEDIT_NO_TERM`, `VEDIT_NO_ART`, both) with their tests
- macOS, without the fault tests
- gvedit on Linux against the distro SDL3, against a static SDL3 built
  from source as the release ships it, and a cross build for Windows

`.github/workflows/docs.yml` renders `docs/site` and publishes it to GitHub
Pages on a push to `main` that touches `docs/`.

A single `ci-ok` job gates on all of them, so branch protection needs only
that one check.

## Screenshots

`docs/shot-*.png` are captured from the real binary so they can be
regenerated after a change to the chrome:

```sh
make screenshots        # runs docs/screenshots.sh ./vedit
```

The script starts a throwaway X server (Xvfb), runs one xterm per shot,
drives the editor with xdotool, and grabs the window with ImageMagick's
`import`. Those four tools must be on PATH.

## Releases

Pushing a tag of the form `v1.4.0` runs `.github/workflows/release.yml`,
which:

1. checks that the tag matches `VEDIT_VERSION` in `vedit.c` and stops if
   not, before anything is built
2. builds static Linux binaries for x86_64 and aarch64, each packaged with
   the man page and the manual. The x86_64 tarball also gets gvedit,
   linked against a static SDL3 built from source by the
   `.github/actions/sdl3-static` action, so it runs without a system
   libSDL3 (it loads X11 or Wayland at run time)
3. cross-builds the Windows x86_64 console binary and gvedit.exe, the
   latter against the SDL3 mingw package fetched by
   `.github/actions/sdl3-mingw`, and zips them with `SDL3.dll`; both
   are started under wine first
4. publishes the archives and a `SHA256SUMS` file on the GitHub release for
   the tag, creating the release when it does not exist

So a release is: bump `VEDIT_VERSION`, commit, tag, push the tag.

The SDL3 version both actions fetch is the one line in
`.github/sdl3-version`; CI builds gvedit the same two ways on every
push (the `gvedit-windows` and `gvedit-static` jobs), so a bump is
checked before a release uses it.
