/*
 * CDE - Common Desktop Environment
 *
 * Copyright (c) 2026, Dr. Christian Klukas.
 *
 * These libraries and programs are free software; you can
 * redistribute them and/or modify them under the terms of the GNU
 * Lesser General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * These libraries and programs are distributed in the hope that
 * they will be useful, but WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Lesser General Public License for more
 * details.
 */

#ifndef _Dt_WmSettings_h
#define _Dt_WmSettings_h

/*
 * _DT_WM_SETTINGS_V1
 *
 * XA_CARDINAL, format 32, length 2
 *   data[0] = version (1)
 *   data[1] = flags bitfield
 */
#define _XA_DT_WM_SETTINGS_V1 "_DT_WM_SETTINGS_V1"

#define DT_WM_SETTINGS_V1_VERSION 1UL

/* data[1] flags */
#define DT_WM_SETTINGS_V1_SHOW_OPEN_WINDOW_ICONS (1UL << 0)

#endif /* _Dt_WmSettings_h */
