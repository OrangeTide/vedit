# Render the manual (docs/*.md) to a static site with pandoc.
#
#   make -C docs/site          build the site into _out/site
#   make -C docs/site clean
#
# The site uses relative links only, so _out/site/index.html opens over
# file:// for a local preview. Requires pandoc.

PANDOC := pandoc

DOCS := ..
OUT  := ../../_out/site

TEMPLATE := template.html
FILTER   := links.lua
CSS      := style.css
INDEXGEN := index.sh

# The chapters, in reading order, then the developer chapter.
MD   := $(sort $(wildcard $(DOCS)/[0-9][0-9]-*.md)) $(DOCS)/developing.md
HTML := $(patsubst $(DOCS)/%.md,$(OUT)/%.html,$(MD))
PNG  := $(patsubst $(DOCS)/%,$(OUT)/%,$(wildcard $(DOCS)/shot-*.png))

PANDOC_HTML := --template=$(TEMPLATE) --lua-filter=$(FILTER) \
	--toc --toc-depth=2 --standalone --from=gfm --to=html5

.PHONY: site clean
site: $(HTML) $(PNG) $(OUT)/index.html $(OUT)/$(CSS) $(OUT)/demo.html

# One page. The title is the first level-1 heading, stripped from the body
# so the template can place it. The previous and next chapters come from
# the position in $(MD).
$(OUT)/%.html: $(DOCS)/%.md $(TEMPLATE) $(FILTER)
	@mkdir -p $(OUT)
	@title=$$(sed -n 's/^# //p' $< | head -1); \
	 prev=$$(printf '%s\n' $(MD) | grep -B1 -x -- '$<' | head -1); \
	 next=$$(printf '%s\n' $(MD) | grep -A1 -x -- '$<' | tail -1); \
	 [ "$$prev" = '$<' ] && prev=; \
	 [ "$$next" = '$<' ] && next=; \
	 set --; \
	 if [ -n "$$prev" ]; then \
	   set -- "$$@" -V "prev-file=$$(basename $$prev .md).html" \
	     -V "prev-title=$$(sed -n 's/^# //p' $$prev | head -1)"; fi; \
	 if [ -n "$$next" ]; then \
	   set -- "$$@" -V "next-file=$$(basename $$next .md).html" \
	     -V "next-title=$$(sed -n 's/^# //p' $$next | head -1)"; fi; \
	 sed '1{/^# /d;}' $< | \
	   $(PANDOC) $(PANDOC_HTML) -M "title=$$title" "$$@" -o $@ -
	@echo "  HTML  $@"

$(OUT)/index.html: $(INDEXGEN) $(MD)
	@mkdir -p $(OUT)
	@sh $(INDEXGEN) $(MD) > $@
	@echo "  INDEX $@"

$(OUT)/$(CSS): $(CSS)
	@mkdir -p $(OUT)
	@cp $< $@

$(OUT)/shot-%.png: $(DOCS)/shot-%.png
	@mkdir -p $(OUT)
	@cp $< $@

$(OUT)/demo.html: $(DOCS)/demo.html
	@mkdir -p $(OUT)
	@cp $< $@

clean:
	rm -rf $(OUT)
