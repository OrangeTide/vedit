# version.mk -- single source of truth for the vedit version.
# Policy: bump this on every commit. Raise the patch level at least; raise
# the minor level (and reset the patch to 0) for a big change or one that
# breaks the config file format. A release tags the commit as v$(VEDIT_VERSION).
VEDIT_VERSION := 1.12.1
