# CDE Toolbar Icon Assets

Place RGBA PNG toolbar icons in `16x16/`, `24x24/`, `32x32/`, and `48x48/` beneath this directory (installed to `/usr/dt/appconfig/icons/C/toolbar/<size>/`). CDE desktop clients (`dtpad`, `dtfile`, `dtcm`) also search `/usr/share/ck-core/icons/toolbar/<size>/` and `$CK_TOOLBAR_ICON_DIR/<size>/`.

## Sources

Each icon is drawn on two pixel grids so that every stem lands on whole pixels: `scalable/<icon-name>.svg` (24-unit grid, rendered to `24x24/` and `48x48/`) and `scalable/compact/<icon-name>.svg` (16-unit grid, rendered to `16x16/` and `32x32/`). Render with `rsvg-convert -w <px> -h <px>`.
