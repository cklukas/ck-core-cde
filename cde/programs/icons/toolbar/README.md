# CDE Toolbar Icon Assets

Place RGBA PNG toolbar icons in `16x16/`, `24x24/`, `32x32/`, and `48x48/` beneath this directory (installed to `/usr/dt/appconfig/icons/C/toolbar/<size>/`). CDE desktop clients (`dtpad`, `dtfile`, `dtcm`) also search `/usr/share/ck-core/icons/toolbar/<size>/` and `$CK_TOOLBAR_ICON_DIR/<size>/`.

## Sources

Each icon is drawn on two pixel grids so that every stem lands on whole pixels: `scalable/<icon-name>.svg` (24-unit grid, rendered to `24x24/` and `48x48/`) and `scalable/compact/<icon-name>.svg` (16-unit grid, rendered to `16x16/` and `32x32/`). The PNGs are rendered from these with hard pixel edges and every pixel snapped to the palette below.

## Palette

The icons use only the 16 static colors of the CDE icon palette, the same set the stock `.pm` desktop icons and `dticon` use: `iconColor1`-`iconColor8` (black, white, red, green, blue, yellow, cyan, magenta) and `iconGray1`-`iconGray8` (`#dedede`, `#bdbdbd`, `#adadad`, `#949494`, `#737373`, `#636363`, `#424242`, `#212121`). Every pixel is either fully transparent or fully opaque, without antialiasing, so the icons match the stock ones and survive reduced color depths unchanged. Shapes carry a black keyline around a light fill; marks that must read on dark palettes use cyan, yellow, white, green or red rather than blue.
