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
/* $XConsortium: inttab.c /main/5 1996/05/09 04:23:37 drk $ */
/*
  routines to implement a table of int, int value pairs.
  the data is relocatable to allow use at various addresses
  (eg shared memory)

  -1 is an illegal key

  Tables are limited to 64K entries

  */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include "DtHash.h"
#include "DtShmDb.h"

#define NOT_AN_INDEX 0

static void build_it(void *data, void *usr_arg, const unsigned char *key);
static void free_hash_data(void *data, void *usr_arg,
                           const unsigned char *key);

typedef struct inttab {
  int key;
  int data;
  unsigned short first;
  unsigned short next;
} inttab_t;

typedef struct intlist {
  int num_entries;
  void * tbl;
} intlist_t;

typedef struct builder {
  int 	      counter;
  intlist_t * intlist_ptr;
  inttab_t  * inttab_ptr;
} builder_t;


/*
  first entry is header block;
  contains key = size (not counting header)
  */

const int *
_DtShmFindIntTabEntry(DtShmInttab inttab, unsigned int key)
{
  const inttab_t * ptr = (const inttab_t *) inttab;
  
  int i;
  
  if ( !ptr->key)
	return(0);

  i = ptr[key % ptr->key + 1].first;
  
  while(i && key != ptr[i].key)
    i = ptr[i].next;
  
  return(i?(&ptr[i].data):(const int *)nullptr);
}

/*
  check of an integer table that was read from a file, before it is used.

  BYTES is the size of the area that holds the table.  A table is valid when
  its entries lie inside the area, its chains stay inside the table and end,
  and CHECK, called with the data of every entry, accepts it (CHECK may be
  null).
  */

int
_DtShmValidateInttab(DtShmInttab inttab, size_t bytes,
                     int (*check)(int data, void * arg), void * arg)
{
  const inttab_t * ptr = (const inttab_t *) inttab;
  /* the indexes are 16 bit numbers, 0 ends a chain */
  static const size_t max_entries = 0xffff;
  unsigned char * seen;
  size_t i, j, count;
  int valid = 1;

  if(bytes < sizeof(inttab_t) || ptr->key < 0)
    return(0);
  count = (size_t) ptr->key;
  if(count > max_entries || (bytes / sizeof(inttab_t)) - 1 < count)
    return(0);
  if(count == 0)
    return(1);

  seen = (unsigned char *) calloc(count + 1, 1);
  if(!seen)
    return(0);

  /* the entries 1 to count are the buckets and the items of the chains */
  for(i = 1; valid && i <= count; i++) {
    for(j = ptr[i].first; j != NOT_AN_INDEX; j = ptr[j].next) {
      if(j > count || seen[j] || (check && !check(ptr[j].data, arg))) {
        valid = 0;
        break;
      }
      seen[j] = 1;
    }
  }
  free(seen);
  return(valid);
}


DtShmProtoInttab
_DtShmProtoInitInttab(int sizeguess)
{
  intlist_t * ptr = (intlist_t *)malloc(sizeof(*ptr));

  if(!ptr)
    return(nullptr);
  ptr->tbl = _DtUtilMakeIHash(sizeguess);
  if(!ptr->tbl) {
    free(ptr);
    return(nullptr);
  }
  ptr->num_entries = 0;

  return((void*)ptr);
}

int _DtShmProtoAddInttab(DtShmProtoInttab intlist, unsigned int keyin,
                         int datain)
{
  intlist_t * ptr = (intlist_t *) intlist;
  int ** data;

  data = (int**)_DtUtilGetHash(ptr->tbl, (unsigned char *) (intptr_t) keyin);
  if(!data)
    return(-1);

  if(!*data) /* new */ {
    *data = (int *) malloc(sizeof(int));
    if(!*data)
      return(-1);
    ptr->num_entries++;
  }

  **data = datain;
  return(0);
}

int
_DtShmProtoSizeInttab(DtShmProtoInttab intlist)
{
  intlist_t * ptr = (intlist_t * ) intlist;

  return(sizeof(inttab_t) * (ptr->num_entries +1));
}

DtShmInttab
_DtShmProtoCopyInttab(DtShmProtoInttab intlist, void * destination)
{
  builder_t build;

  build.counter = 1;
  build.intlist_ptr = (intlist_t * ) intlist;
  build.inttab_ptr = (inttab_t *) destination;
  
  memset(destination, 0, (build.intlist_ptr->num_entries+1)*sizeof(inttab_t));

  build.inttab_ptr->key = build.intlist_ptr->num_entries;

  _DtUtilOperateHash(build.intlist_ptr->tbl, build_it, &build);

  return(0);
}

int
_DtShmProtoDestroyInttab(DtShmProtoInttab intlist)
{
  intlist_t * ptr = (intlist_t *)intlist;
  if(!ptr)
    return(0);
  _DtUtilDestroyHash(ptr->tbl, free_hash_data, nullptr);
  free(intlist);
  return(0);
}

static void build_it(void *data, void *usr_arg, const unsigned char *key)
{
  builder_t * ptr = (builder_t *) usr_arg;
  int *value = (int *)data;
  int int_key = (int)(intptr_t)key;
  inttab_t * a;
  inttab_t * b;
  unsigned short * add_ptr;

  int bucket = int_key % (ptr->intlist_ptr->num_entries) + 1;
 
  a = ptr->inttab_ptr + ptr->counter;
  a->key 	= int_key;
  a->data	= *value;
  a->next	= NOT_AN_INDEX;

  b = ptr->inttab_ptr + bucket;

  if(b->first == NOT_AN_INDEX) {
    add_ptr = &b->first;
  } else {
    b = ptr->inttab_ptr + b->first;
    while(b->next != NOT_AN_INDEX)
      b = ptr->inttab_ptr + b->next;
    add_ptr = &b->next;	
  }

  *add_ptr = ptr->counter++;
}

static void
free_hash_data(void *data, void *usr_arg, const unsigned char *key)
{
  (void)usr_arg;
  (void)key;
  free(data);
}

