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
// $XConsortium: TextTest.cc /main/3 1996/06/11 17:41:57 cde-hal $
/*	Copyright (c) 1995 FUJITSU LIMITED	*/
/*	All Rights Reserved			*/

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <sstream>
using namespace std;

#include "TextParser.hh"

main(int argc, char** argv)
{
    char* patterns = NULL;

    setlocale(LC_CTYPE, "");

    if (! (argc > 1)) {
	fprintf(stderr, "Usage: %s pattern1 pattern2 ...\n", argv[0]);
	return 1;
    }

    int size = 256;
    patterns = (char*) malloc(size);
    if (patterns == NULL)
	return 1;
    *patterns = '\0';
    int npat = argc - 1, i;
    for (i = 1; i <= npat; i++) {
	size_t cur_len = strlen(patterns);
	size_t arg_len = strlen(argv[i]);
	if (cur_len + arg_len + 2 > (size_t)size) { // 2 = '\n'+'\0'
	    int next_size = size;
	    while (cur_len + arg_len + 2 > (size_t)next_size)
		next_size += 256;
	    char* new_patterns = (char*) realloc(patterns, next_size);
	    if (new_patterns == NULL) {
		free(patterns);
		return 1;
	    }
	    patterns = new_patterns;
	    size = next_size;
	}
	snprintf(patterns + cur_len, size - cur_len, "%s\n", argv[i]);
    }

    ostringstream text;

    char ch;
    while (cin.get(ch)) text << ch;

    string textstr = text.str();
    char* buf = (char *)textstr.c_str();

    char* match = StringParser::brute_force(buf, npat, patterns);

    if (match) {
	cout << match;
	delete[] match;
    }
    else
	fprintf(stderr, "match not found for \"%s\"\n", patterns);
	
}
