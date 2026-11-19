# CDE Toolbar Icon Assets

Place RGBA PNG toolbar icons in `16x16/`, `24x24/`, `32x32/`, and `48x48/` beneath this directory (installed to `/usr/dt/appconfig/icons/C/toolbar/<size>/`). CDE desktop clients (`dtpad`, `dtfile`) also search `/usr/share/ck-core/icons/toolbar/<size>/` and `$CK_TOOLBAR_ICON_DIR/<size>/`.

## Sources

Each icon is drawn on two pixel grids so that every stem lands on whole pixels: `scalable/<icon-name>.svg` (24-unit grid, rendered to `24x24/` and `48x48/`) and `scalable/compact/<icon-name>.svg` (16-unit grid, rendered to `16x16/` and `32x32/`). The PNGs are rendered from these with hard pixel edges and every pixel snapped to the palette below.

## Palette

The icons use only the 16 static colors of the CDE icon palette, the same set the stock `.pm` desktop icons and `dticon` use: `iconColor1`-`iconColor8` (black, white, red, green, blue, yellow, cyan, magenta) and `iconGray1`-`iconGray8` (`#dedede`, `#bdbdbd`, `#adadad`, `#949494`, `#737373`, `#636363`, `#424242`, `#212121`). Every pixel is either fully transparent or fully opaque, without antialiasing, so the icons match the stock ones and survive reduced color depths unchanged. Shapes carry a black keyline around a light fill; marks that must read on dark palettes are never plain blue.

Larger color areas are not filled with one flat primary. As in the stock icons, they are dithered: a one-pixel checkerboard of two palette colors, for example yellow with white (folder, triangles), red with `iconGray2` (roof, stop sign), blue with cyan (arrows), blue with `iconGray5` (disk), green with `iconGray1` (plus) and cyan with white (sky, selections). The checkerboard is always one output pixel wide, at every size. In the SVG sources such a fill appears as the blend of its two colors; the generator resolves it to the checkerboard.

## Keylines

The keyline of a shape is the border of its own hard-edged silhouette: exactly one pixel wide at 16 and 24 px and two at 32 and 48 px, unbroken all the way round, with no highlight laid along it. Slanted edges run at 45 degrees or at 1:2 or 1:3 through pixel centres, so they step in a regular staircase. An arrow's shaft and head are one silhouette, so no line separates them.
