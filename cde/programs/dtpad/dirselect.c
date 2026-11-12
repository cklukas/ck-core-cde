/*
 * dirselect.c - the directory that the Save As dialog of dtpad starts in.
 * See dirselect.h.
 */

#include <string.h>
#include <unistd.h>
#include "dirselect.h"

int
DirSelectStore(char *dst, size_t dstsize, const char *text, size_t len)
{
    size_t total;
    int needSlash;

    if (dstsize == 0)
	return 0;
    dst[0] = '\0';
    if (len == 0)
	return 0;

    needSlash = (text[len - 1] != '/');
    total = len + (needSlash ? 1 : 0);
    /* The mask follows the directory, and the NUL follows the mask. */
    if (total + DIRSELECT_MASK_LEN >= dstsize)
	return 0;

    memcpy(dst, text, len);
    if (needSlash)
	dst[len] = '/';
    dst[total] = '\0';
    return 1;
}

size_t
DirSelectAppendMask(char *dst, size_t dstsize)
{
    size_t len = strlen(dst);

    if (dstsize > len + DIRSELECT_MASK_LEN)
	memcpy(dst + len, DIRSELECT_MASK, DIRSELECT_MASK_LEN + 1);
    return len;
}

void
DirSelectDefault(char *dst, size_t dstsize)
{
    size_t len;

    if (dstsize <= DIRSELECT_MASK_LEN + 1) {
	if (dstsize > 0)
	    dst[0] = '\0';
	return;
    }
    /*
     * getcwd() may write the directory, with the NUL, into all of the room
     * that the mask and its separator leave free.
     */
    if (getcwd(dst, dstsize - DIRSELECT_MASK_LEN - 1) != NULL) {
	len = strlen(dst);
	if (len > 0 && dst[len - 1] != '/') {
	    dst[len] = '/';
	    dst[len + 1] = '\0';
	}
	return;
    }
    (void) DirSelectStore(dst, dstsize, "/", 1);
}
