# Diagrams

`catalogues.svg` — how Local and Server catalogues, photo library folders and online
photo storage link together. It's shown in the in-app help as
`resources/help/catalogues.png`.

After editing the SVG, re-render the PNG (at 2× its 1400×840 layout, so it stays sharp
on high-DPI screens) with headless Firefox, which draws the text properly:

```sh
mkdir -p /tmp/ff-diagram
firefox --headless --no-remote --profile /tmp/ff-diagram --window-size=2800,1680 \
    --screenshot /tmp/catalogues.png "file://$PWD/docs/diagrams/catalogues.svg"
convert /tmp/catalogues.png -strip -define png:compression-level=9 resources/help/catalogues.png
```

Run it from the repository root, then rebuild so the new image is compiled in.

## Getting Started guide

`guide/*.svg` are the pictures in the Getting Started panel (`resources/guide/*.png`, compiled
in as `:/guide/...`). They're written by `guide/make_svgs.py` — edit that, not the SVGs — and
rendered the same way, each at the size its `width`/`height` give (twice its layout):

```sh
cd docs/diagrams/guide && python3 make_svgs.py
mkdir -p /tmp/ff-guide
for f in *.svg; do
  n=${f%.svg}
  size=$(sed -n 's/.*<svg[^>]* width="\([0-9]*\)" height="\([0-9]*\)".*/\1,\2/p' "$f")
  firefox --headless --no-remote --profile /tmp/ff-guide --window-size="$size" \
      --screenshot "/tmp/$n.png" "file://$PWD/$f"
  convert "/tmp/$n.png" -strip -define png:compression-level=9 "../../../resources/guide/$n.png"
done
```
