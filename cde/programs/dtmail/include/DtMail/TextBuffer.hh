/*
 * CDE - Common Desktop Environment
 *
 * Copyright (c) 1993-2012, The Open Group. All rights reserved.
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
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with these libraries and programs; if not, write
 * to the Free Software Foundation, Inc., 51 Franklin Street, Fifth
 * Floor, Boston, MA 02110-1301 USA
 */

#ifndef _TEXTBUFFER_HH
#define _TEXTBUFFER_HH

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Text that is made of header values, addresses and file names of a message
// has no length that can be worked out beforehand.  A TextBuffer holds a
// NUL terminated text that grows as it is appended to, so that no buffer has
// to be sized by a guess about the text that goes into it.
class TextBuffer {
  public:
    TextBuffer() : _text(NULL), _length(0), _size(0) {}
    ~TextBuffer() { free(_text); }

    // Append length bytes of text.  Returns false, and leaves the buffer as
    // it was, if the memory for it cannot be had.
    bool append(const char * text, size_t length)
    {
	if (length > maxLength() - _length) {
	    return false;
	}
	size_t need = _length + length + 1;
	if (need > _size) {
	    size_t size = _size ? _size : 64;
	    while (size < need) {
		if (size > SIZE_MAX / 2) {
		    size = need;
		    break;
		}
		size *= 2;
	    }
	    char * grown = (char *)realloc(_text, size);
	    if (grown == NULL) {
		return false;
	    }
	    _text = grown;
	    _size = size;
	}
	memcpy(_text + _length, text, length);
	_length += length;
	_text[_length] = '\0';
	return true;
    }

    bool append(const char * text) { return append(text, strlen(text)); }

    size_t length() const { return _length; }

    // The text so far; an empty string if nothing was appended.
    const char * text() const { return _text ? _text : ""; }

    // A copy of the text in memory from new[], for callers that delete[] it.
    char * release() const
    {
	size_t length = _length < maxLength() ? _length : maxLength();
	char * copy = new char[length + 1];
	memcpy(copy, text(), length);
	copy[length] = '\0';
	return copy;
    }

  private:
    // Far below the largest object, so that the sizes made from a length
    // cannot wrap.
    static size_t maxLength() { return SIZE_MAX / 4; }

    TextBuffer(const TextBuffer &);
    TextBuffer & operator=(const TextBuffer &);

    char *	_text;
    size_t	_length;
    size_t	_size;
};

// Copy as much of text as fits into the size bytes at dst, and terminate it.
// Returns false if the text was cut.
inline bool
copyBounded(char * dst, size_t size, const char * text)
{
    if (size == 0) {
	return false;
    }
    size_t length = strlen(text);
    bool whole = length < size;
    if (!whole) {
	length = size - 1;
    }
    memcpy(dst, text, length);
    dst[length] = '\0';
    return whole;
}

#endif
