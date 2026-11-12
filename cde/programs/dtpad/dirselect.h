/*
 * dirselect.h - the directory that the Save As dialog of dtpad starts in.
 *
 * The dialog's directory and its mask ("[^.]*") share one buffer.  Every step
 * that writes into it checks that the result fits.  This module uses no Motif
 * or Xt types, so it is tested on its own (tests/check-dirselect.py).
 */

#ifndef _DTPAD_DIRSELECT_H
#define _DTPAD_DIRSELECT_H

#include <stddef.h>

#define DIRSELECT_MASK		"[^.]*"
#define DIRSELECT_MASK_LEN	(sizeof(DIRSELECT_MASK) - 1)

/*
 * Stores the first len bytes of text in dst, which has dstsize bytes
 * including the terminating NUL, as a directory: a '/' is appended when the
 * text does not end with one.  Room is kept after the directory for the mask.
 *
 * Returns 1 when the directory is stored.  Returns 0 and leaves dst empty when
 * len is 0, when the text does not fit with the mask, or when dstsize is 0.
 */
int DirSelectStore(char *dst, size_t dstsize, const char *text, size_t len);

/*
 * Appends the directory mask to the NUL-terminated directory in dst, when the
 * mask fits in dstsize.  Returns the length of the directory before the mask,
 * which is where the caller selects the file name part of the dialog.
 */
size_t DirSelectAppendMask(char *dst, size_t dstsize);

/*
 * Stores the current directory of this process in dst, with a trailing '/',
 * the way DirSelectStore does.  If the directory cannot be obtained or does
 * not fit, the root directory is stored.
 */
void DirSelectDefault(char *dst, size_t dstsize);

#endif /* _DTPAD_DIRSELECT_H */
