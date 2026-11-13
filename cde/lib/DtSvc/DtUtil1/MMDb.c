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
/* $TOG: MMDb.c /main/19 1998/10/23 13:48:52 mgreess $ */
/*
 * +SNOTICE
 * 
 * Copyright 1995 Sun Microsystems, Inc.  All rights reserved.
 * 
 * +ENOTICE
 */
#include <stdio.h>
#include <sys/types.h>

#include <unistd.h>
#include <sys/utsname.h>
#include <stdlib.h>

#include <ctype.h>
#include <string.h>
#include <stdint.h>

#ifdef NLS16
#include <limits.h>
#endif

#include <sys/stat.h>
#include <sys/param.h>		/* MAXPATHLEN, MAXHOSTNAMELEN */
#define X_INCLUDE_DIRENT_H
#define XOS_USE_XT_LOCKING
#include <X11/Xos_r.h>
#include <Dt/DbReader.h>
#include <Dt/DtsDb.h>
#include <Dt/DtsMM.h>
#include <Dt/DtShmDb.h>
#include <Dt/Dts.h>
#include <Dt/ActionP.h>
#include <Dt/ActionDbP.h>
#include <Dt/ActionUtilP.h>
#include <Dt/DtNlUtils.h>
#include <Dt/UserMsg.h>
#include "myassertP.h"
#include "DtSvcLock.h"

static int build_file_list(DtShmProtoIntList, DtDirPaths *,
			   DtDtsMMHeader *, const char *);
extern	int	cde_dc_field_compare(const void *, const void *);
extern	int	cde_dc_compare(const void *, const void *);
static void	_DtMMSortDataTypes(DtShmProtoStrtab str_handle);
static int	_DtMMAddActionsToDataAttribute(DtDtsDbDatabase *db_ptr);
static int	write_db(DtDtsMMHeader *header, void *index, int size,
			 const char *CacheFile);
static int	build_new_db(DtShmProtoStrtab, DtShmProtoIntList, int, DtDtsDbDatabase **);
static int	build_name_list(DtDtsDbDatabase *, DtShmProtoIntList, DtDtsMMHeader *);

static	DtShmProtoStrtab	shm_handle = 0;
static	DtShmProtoIntList	int_handle = 0;

#define QtB(a)	_DtShmProtoAddStrtab(shm_handle, XrmQuarkToString(a), &isnew)

/* DtsMM.c */
extern int _DtDtsMMPathHash(DtDirPaths *dirs);


int
_MMWriteDb(DtDirPaths *dirs, int num_db, DtDtsDbDatabase **db_list,
	   const char *CacheFile)
{
	DtDtsMMHeader		header;
	char			*suffix = ".dt";
	int			tbl_size;
	void			*tbl_data;
	DtDtsDbDatabase	        *db;
	int			returnCode;

	_DtSvcProcessLock();
	memset(&header, '\0', sizeof(header));
	int_handle = _DtShmProtoInitIntLst(50000);
	shm_handle = _DtShmProtoInitStrtab(10000);

	if (!int_handle || !shm_handle ||
	    !build_file_list(int_handle, dirs, &header, suffix))
	{
		returnCode = 0;
		goto done;
	}

	_DtMMSortDataTypes(shm_handle);
	db = (DtDtsDbDatabase	*) _DtDtsDbGet(DtDTS_DA_NAME);
	if (_DtMMAddActionsToDataAttribute(db) < 0)
	{
		returnCode = 0;
		goto done;
	}

	header.pathhash = _DtDtsMMPathHash(dirs);
	header.num_db = num_db;
	header.db_offset = build_new_db(shm_handle, int_handle, num_db,
	db_list);
	if (header.db_offset < 0)
	{
		returnCode = 0;
		goto done;
	}
	db = (DtDtsDbDatabase	*) _DtDtsDbGet("DATA_CRITERIA");
	if (build_name_list(db, int_handle, &header) < 0)
	{
		returnCode = 0;
		goto done;
	}

	tbl_size = _DtShmProtoSizeStrtab(shm_handle);
	tbl_data = (void *) _DtShmProtoAddIntLst(int_handle,
				tbl_size/sizeof(int), &header.str_tbl_offset);
	if (tbl_data == nullptr)
	{
		returnCode = 0;
		goto done;
	}
	_DtShmProtoCopyStrtab(shm_handle, tbl_data);


	tbl_size = _DtShmProtoSizeIntLst(int_handle);
	tbl_data = calloc(1, tbl_size);
	if (tbl_data == nullptr)
	{
		returnCode = 0;
		goto done;
	}
	tbl_data = (void *)_DtShmProtoCopyIntLst(int_handle, tbl_data);

	returnCode = write_db(&header, tbl_data, tbl_size, CacheFile);
	free(tbl_data);

done:
	_DtShmProtoDestroyStrtab(shm_handle);
	_DtShmProtoDestroyIntLst(int_handle);
	_DtSvcProcessUnlock();

	return returnCode;
}

/*
 * Make room for COUNT entries in both lists of the file scan.  A list keeps
 * its old, valid block when the larger one cannot be allocated.
 */
static int
grow_file_lists(DtShmBoson **boson_list, time_t **mtime_list, int count)
{
	DtShmBoson	*new_bosons;
	time_t		*new_mtimes;

	if (count < 1 ||
	    (size_t)count > SIZE_MAX / sizeof(DtShmBoson) ||
	    (size_t)count > SIZE_MAX / sizeof(time_t))
		return 0;
	new_bosons = (DtShmBoson *)realloc(*boson_list,
					   count*sizeof(DtShmBoson));
	if (new_bosons == nullptr)
		return 0;
	*boson_list = new_bosons;
	new_mtimes = (time_t *)realloc(*mtime_list, count*sizeof(time_t));
	if (new_mtimes == nullptr)
		return 0;
	*mtime_list = new_mtimes;
	return 1;
}

/*
 * Make room for COUNT record numbers in LIST.
 */
static int
grow_record_list(int **list, int count)
{
	int	*new_list;

	if (count < 1 || (size_t)count > SIZE_MAX / sizeof(int))
		return 0;
	new_list = (int *)realloc(*list, count*sizeof(int));
	if (new_list == nullptr)
		return 0;
	*list = new_list;
	return 1;
}

/* Returns 1 on success and 0, with nothing left allocated, on failure. */
static int
build_file_list(DtShmProtoIntList int_handle, DtDirPaths *dirs,
		DtDtsMMHeader *header, const char *suffix)
{
	DIR 			*dirp = nullptr;
	struct dirent		*dp = nullptr;
	struct stat		buf;
	char			cur_path[MAXPATHLEN+1];
	void			*data;
	int			size = sizeof(buf.st_mtime);
	int			i;
	int			isnew;
	DtShmBoson		*boson_list = 0;
	time_t			*mtime_list = 0;
	int			count = 0;
	_Xreaddirparams		dirEntryBuf;
	struct dirent		*result;

	/* Theses here to make sure it gets into the string tables
	   because actions uses it in its "types" field. */
	if (!_DtShmProtoAddStrtab(shm_handle, DtDTS_DT_UNKNOWN, &isnew) ||
	    !_DtShmProtoAddStrtab(shm_handle, DtDTS_DT_RECURSIVE_LINK, &isnew) ||
	    !_DtShmProtoAddStrtab(shm_handle, DtDTS_DT_BROKEN_LINK, &isnew))
		return 0;

	if (getcwd(cur_path, sizeof(cur_path)) == nullptr)
		return 0;
	for(i = 0; dirs->paths[i]; i++)
	{
		/* a directory that cannot be entered has no files to list */
		if (chdir(dirs->paths[i]) != 0)
			continue;
		if (stat(".", &buf) != 0)
			continue;
		count++;
		if (!grow_file_lists(&boson_list, &mtime_list, count))
			goto fail;

		mtime_list[count-1] = buf.st_mtime;
		boson_list[count-1] = _DtShmProtoAddStrtab(shm_handle, dirs->paths[i], &isnew);
		if (boson_list[count-1] == 0)
			goto fail;
		dirp = opendir(".");
		if (dirp == nullptr)
			continue;
		while ((result = _XReaddir(dirp, dirEntryBuf)) != nullptr)
		{
			char	*c = strrchr(result->d_name, suffix[0]);
			if(c && strcmp(c, suffix) == 0)
			{
				char	pathname[MAXPATHLEN+1];
				snprintf(pathname, sizeof(pathname), "%s/%s", dirs->paths[i], result->d_name);
				stat(result->d_name, &buf);
				count++;
				if (!grow_file_lists(&boson_list, &mtime_list, count))
					goto fail;
				mtime_list[count-1] = buf.st_mtime;
				boson_list[count-1] = _DtShmProtoAddStrtab(shm_handle,
					pathname, &isnew);
				if (boson_list[count-1] == 0)
					goto fail;
				continue;
			}
		}
		(void)closedir( dirp );
		dirp = nullptr;
	}
	if (chdir(cur_path) != 0)
		goto fail;
	data = _DtShmProtoAddIntLst(int_handle, count, &header->files_offset);
	if (data == nullptr)
		goto fail;
	memcpy(data, boson_list, count*sizeof(int));
	data = _DtShmProtoAddIntLst(int_handle, count*sizeof(time_t)/sizeof(int), &header->mtimes_offset);
	if (data == nullptr)
		goto fail;
	memcpy(data, mtime_list, count*sizeof(time_t));
	header->files_count = count;
	free(boson_list);
	free(mtime_list);
	return 1;

fail:
	if (dirp)
		(void)closedir(dirp);
	if (chdir(cur_path) != 0)
	{
		/* nothing more can be done; the list is dropped anyway */
	}
	free(boson_list);
	free(mtime_list);
	return 0;
}

static int
db_table_size(int num_db, DtDtsDbDatabase **db_list)
{
	int			db;
	DtDtsDbDatabase		*db_ptr;
	int			rec;
	DtDtsDbRecord		*rec_ptr;
	int			fld;
	DtDtsDbField		*fld_ptr;
	int			size = 0;

	size += num_db*sizeof(DtDtsMMDatabase);
	for(db = 0; db < num_db; db++)
	{
		db_ptr = db_list[db];
		size += db_ptr->recordCount * sizeof(DtDtsMMRecord);
		for(rec = 0; rec < db_ptr->recordCount; rec++)
		{
			rec_ptr = db_ptr->recordList[rec];
			size += rec_ptr->fieldCount * sizeof(DtDtsMMField);
		}
	}
	return(size/sizeof(int));
}
static void
_DtMMSortDataTypes(DtShmProtoStrtab str_handle)
{
	DtDtsDbDatabase	*dc;
	DtDtsDbDatabase	*da;
	int		i;

	_DtSvcProcessLock();       
	dc = (DtDtsDbDatabase *) _DtDtsDbGet(DtDTS_DC_NAME);
	da = (DtDtsDbDatabase *) _DtDtsDbGet(DtDTS_DA_NAME);

/*_DtDtsDbPrintRecords(dc, stdout);*/
	for(i = 0; i < dc->recordCount; i++)
	{
		if(dc->recordList[i]->compare != cde_dc_field_compare)
		{
			_DtDtsDbFieldSort(dc->recordList[i], 
				cde_dc_field_compare);
		}
	}
	_DtDtsDbRecordSort(dc, cde_dc_compare);

	for(i = 0; i < da->recordCount; i++)
	{
		if(da->recordList[i]->compare !=
				_DtDtsDbCompareFieldNames)
		{
			_DtDtsDbFieldSort(da->recordList[i], 
				_DtDtsDbCompareFieldNames);
		}
	}

	_DtDtsDbRecordSort(da, _DtDtsDbCompareRecordNames);
/*_DtDtsDbPrintRecords(dc, stdout);*/
	_DtSvcProcessUnlock();
}

/* Returns 0, or -1 when memory is exhausted. */
static int
add_if_missing(DtDtsDbRecord *rec_ptr, XrmQuark name, char *value)
{
	DtDtsDbField	*fld_ptr;
	int		fld;
	int		found = 0;

	for(fld = 0; fld < rec_ptr->fieldCount; fld++)
	{
		fld_ptr = rec_ptr->fieldList[fld];
		if(name == fld_ptr->fieldName)
		{
			found = 1;
			break;
		}
	}
	if(found)
	{
		return 0;
	}

	fld_ptr = _DtDtsDbAddField(rec_ptr);
	if(fld_ptr == nullptr)
	{
		return -1;
	}
	fld_ptr->fieldName = name;
	fld_ptr->fieldValue = value;
	_DtDtsDbFieldSort(rec_ptr, 0);

	return 0;
}

/* Returns 0, or -1 when memory is exhausted. */
static int
_DtMMAddActionsToDataAttribute(DtDtsDbDatabase *db_ptr)
{
	int		rec;
	DtDtsDbRecord	*rec_ptr;
	int		action_flag = 0;
	int		sort_flag = 0;
	int		found_flag = 0;
	int		n;
	const	char	*tmp;
	XrmQuark	desc_qrk = XrmStringToQuark(DtDTS_DA_DESCRIPTION);
	XrmQuark	icon_qrk = XrmStringToQuark(DtDTS_DA_ICON);
	XrmQuark	label_qrk = XrmStringToQuark(DtDTS_DA_LABEL);

	for(rec = 0; rec < db_ptr->recordCount; rec++)
	{
		int	found_des = 0;
		int	found_icon = 0;
		int	found_label = 0;
		char	*obj_type;

		rec_ptr = db_ptr->recordList[rec];
		obj_type = XrmQuarkToString(rec_ptr->recordName);

		if ( _DtDtsDbGetFieldByName(rec_ptr,
				DtDTS_DA_IS_ACTION) == 0 )
		{
			continue;
		}
		if (add_if_missing(rec_ptr, desc_qrk,
				DtActionDescription(obj_type)) < 0 ||
		    add_if_missing(rec_ptr, icon_qrk, DtActionIcon(obj_type)) < 0 ||
		    add_if_missing(rec_ptr, label_qrk, DtActionLabel(obj_type)) < 0)
		{
			return -1;
		}
	}
	return 0;
}

/* Returns the index of the database list, or -1 when memory is exhausted. */
static int
build_new_db(DtShmProtoStrtab shm_handle, DtShmProtoIntList int_handle, int num_db, DtDtsDbDatabase **db_list)
{
	DtDtsMMDatabase		*new_db_list;
	int			db;
	DtDtsDbDatabase		*db_ptr;
	DtDtsMMDatabase		*new_db_ptr;
	int			rec;
	DtDtsDbRecord		*rec_ptr;
	DtDtsMMRecord		*new_rec_ptr;
	DtDtsMMRecord		*new_rec_ptr_list;
	int			fld;
	DtDtsDbField		*fld_ptr;
	DtDtsMMField		*new_fld_ptr;
	DtDtsMMField		*new_fld_ptr_list;
	int			index;
	int			db_index;
	int			isnew;
	char			*tmp;
	DtShmProtoInttab	nameIndex = 0;

	/* create a space to hold the list of database structures */
	new_db_list = (DtDtsMMDatabase *)_DtShmProtoAddIntLst(int_handle,
			num_db*sizeof(DtDtsMMDatabase)/sizeof(int),
			&db_index);
	if (new_db_list == nullptr)
		return(-1);
	for(db = 0; db < num_db; db++)
	{
		int	last_boson = -1;
		int	list_count = 0;
		int		size;
		int		*idx;

		new_db_ptr = &new_db_list[db];
		db_ptr = db_list[db];

		new_db_ptr->databaseName = _DtShmProtoAddStrtab(shm_handle, db_ptr->databaseName, &isnew);
		if (new_db_ptr->databaseName == 0)
			goto fail;
		new_db_ptr->recordCount = db_ptr->recordCount;
		/* create space to hold record list */
		new_rec_ptr_list = (DtDtsMMRecord *)_DtShmProtoAddIntLst(int_handle,
				db_ptr->recordCount*sizeof(DtDtsMMRecord)/sizeof(int),
				&index);
		if (new_rec_ptr_list == nullptr)
			goto fail;

		new_db_ptr->recordList = index;
		/* create index to names list */
		nameIndex = _DtShmProtoInitInttab(db_ptr->recordCount);
		if (nameIndex == nullptr)
			goto fail;
		for(rec = 0; rec < db_ptr->recordCount; rec++)
		{
			new_rec_ptr = &new_rec_ptr_list[rec];
			rec_ptr = db_ptr->recordList[rec];
			new_rec_ptr->recordName = QtB(rec_ptr->recordName);
			if (new_rec_ptr->recordName == 0)
				goto fail;

			if(new_rec_ptr->recordName != last_boson)
			{
				/* save name position */
				if (_DtShmProtoAddInttab(nameIndex, new_rec_ptr->recordName, rec) != 0)
					goto fail;

				last_boson = new_rec_ptr->recordName;
			}
			new_rec_ptr->pathId = _DtShmProtoAddStrtab(shm_handle,
				tmp = _DtDbPathIdToString(rec_ptr->pathId),
						 &isnew);
			XtFree(tmp);
			if (new_rec_ptr->pathId == 0)
				goto fail;
			new_rec_ptr->seq = rec_ptr->seq;
			new_rec_ptr->fieldCount = rec_ptr->fieldCount;

			/* create space for field list */
			new_fld_ptr_list = (DtDtsMMField *)_DtShmProtoAddIntLst(int_handle,
				rec_ptr->fieldCount*sizeof(DtDtsMMField)/sizeof(int),
				&index);
			if (new_fld_ptr_list == nullptr)
				goto fail;

			new_rec_ptr->fieldList = index;
			for(fld = 0; fld < rec_ptr->fieldCount; fld++)
			{
				new_fld_ptr = &new_fld_ptr_list[fld];
				fld_ptr = rec_ptr->fieldList[fld];

				new_fld_ptr->fieldName  = QtB(fld_ptr->fieldName);
				if (new_fld_ptr->fieldName == 0)
					goto fail;
				new_fld_ptr->fieldValue = fld_ptr->fieldValue?_DtShmProtoAddStrtab(shm_handle,
					fld_ptr->fieldValue, &isnew):0;
				if (fld_ptr->fieldValue && new_fld_ptr->fieldValue == 0)
					goto fail;
			}
		}
		/* create table for index and save it */
		size = _DtShmProtoSizeInttab(nameIndex);
		idx = _DtShmProtoAddIntLst(int_handle, size/sizeof(int), &new_db_ptr->nameIndex);
		if (idx == nullptr)
			goto fail;
		_DtShmProtoCopyInttab(nameIndex, (void *)idx);
		_DtShmProtoDestroyInttab(nameIndex);
		nameIndex = 0;
	}
	return(db_index);

fail:
	_DtShmProtoDestroyInttab(nameIndex);
	return(-1);
}

struct  list
{
	DtShmBoson	boson;
	int		rec;
};

static int
srch(const void *a, const void *b)
{
	int results = ((struct list *)a)->boson - ((struct list *)b)->boson;

	if(results == 0)
	{
		results = ((struct list *)a)->rec - ((struct list *)b)->rec;
	}
	return(results);
}

static void
showtable(
	DtDtsDbDatabase *db,
	struct list *name_index, 
	struct list *other, 
	DtDtsMMHeader *head,
	int other_break)
{
	int	i;

	printf("============== names =====================\n");
	for(i = 0; name_index[i].boson; i++)
	{
		printf("%20s -> %s\n",
			XrmQuarkToString(db->recordList[name_index[i].rec]->recordName),
			_DtShmProtoLookUpStrtab(shm_handle, 
					name_index[i].boson));
	}
	printf("%d entries\n", i);
	
	printf("============= other ======================\n");
	for(i = 0; i < other_break; i++)
	{
		printf("%s\n",
			XrmQuarkToString(db->recordList[other[i].rec]->recordName));
	}
	printf("%d entries\n", i);			
}

static int
build_name_list(DtDtsDbDatabase *db,
		DtShmProtoIntList int_handle,
		DtDtsMMHeader	*head)
{
	struct list 	*other;
	int		i;
	char		*c;
	int		isnew;
	struct list	*name_index;
	int		next = 0;
	int		other_break = 0;
	DtShmProtoInttab	indexList = 0;
	DtShmBoson	last_boson = -1;
	int		*list_of_recs = 0;
	int		list_count = 0;
	int		index = 0;
	int		size;
	void		*space;

	/* create tmp space for two lists */
	name_index = (struct list *)calloc(db->recordCount*2 + 1,
					sizeof(struct list));
	other = (struct list *)calloc(db->recordCount + 1, sizeof(struct list));
	if (name_index == nullptr || other == nullptr)
		goto fail;

	/* step through all records */
	for(i = 0; i < db->recordCount; i++)
	{
		DtShmBoson	boson;
		char	*attr;
		char	*t;

		/* see if a name pattern exist */
		attr = _DtDtsDbGetFieldByName(db->recordList[i],
				DtDTS_NAME_PATTERN);
		if(!attr)
		{
			/* it didn't so check path pattern */
			attr = _DtDtsDbGetFieldByName(db->recordList[i],
				DtDTS_PATH_PATTERN);
			if(!attr)
			{
				/* neither exist so save it as plain buffer */
				if(!head->buffer_start_index)
				{
					head->buffer_start_index = other_break;
				}
				other[other_break++].rec = i;
				continue; /* go to next record */
			}
		}

		/* we have a name now find its final component */
		c = strrchr(attr, '/');
		if(c)
		{
			c++;
		}
		if(!c)
		{
			c = attr;
		}
		else
		{
			attr = c;
		}

		/* now see if that final component has any *,?,[ */
		while(c && *c &&
			  !(*c == '*' ||
			    *c == '[' ||
			    *c == '?' ||
			    *c == '$' ))
		{
			c++;
		}


		if(c && *c == '\0')
		{
			/* it doesn't so save it in the name index */
			name_index[next].boson = 
				_DtShmProtoAddStrtab(shm_handle,
					(const char *)attr, &isnew);
			if (name_index[next].boson == 0)
				goto fail;
			name_index[next++].rec = i;
			continue; /* next record */
		}

		/* the name had something in it now lets get the suffix */
		c = strrchr(attr, '.');
		attr = c;

		/* lets see if the suffix has any  *,?,[ */
		while(c && *c &&
			  !(*c == '*' ||
			    *c == '[' ||
			    *c == '?' ||
			    *c == '$' ))
		{
			c++;
		}
		if(c && *c == '\0')
		{
			/* it doesn't so save it in the name index */
			name_index[next].boson = 
			_DtShmProtoAddStrtab(shm_handle,
					(const char *)attr, &isnew);
			if (name_index[next].boson == 0)
				goto fail;
			name_index[next++].rec = i;
		}
		else
		{
			/* couldn't find any thing so save it as other */
			other[other_break++].rec = i;
		}

	}

	if (next > 0)
	{
		qsort(name_index, next, sizeof(struct list), srch);
	}

/*
showtable(db, name_index, other, head, other_break);
printf("                    next = %d\n", next);
printf("             other_break = %d\n", other_break);
printf("head->buffer_start_index = %d\n", head->buffer_start_index);
*/
	/* create a table and add the records to it. However
	   duplicates need to be in separate lists.
	*/
	indexList = _DtShmProtoInitInttab(next+3);
	if (indexList == nullptr)
		goto fail;
	for(i = 0; i <= next; i++)
	{
		if(i != next && (last_boson == -1 || name_index[i].boson == last_boson))
		{
			/* this a new list of records or an addition to one */
			if (!grow_record_list(&list_of_recs, list_count + 1))
				goto fail;
			list_count++;
			last_boson = name_index[i].boson;
			list_of_recs[list_count-1] = name_index[i].rec;
		}
		else
		{
			/* we reached the end of a list now we check how many
				are in the list. 
			*/
			if(list_count == 1)
			{
				/* if just one just add it in the index */
				if (_DtShmProtoAddInttab(indexList,
						last_boson, list_of_recs[0]) != 0)
					goto fail;
			}
			else
			{
				/* if there are multiple items in the list
				   create a table for them */
				int	*list = _DtShmProtoAddIntLst(int_handle,
						list_count, &index);

				if (list == nullptr)
					goto fail;

				/* write the list to the to the table */
				memcpy(list, list_of_recs,
						list_count*sizeof(int));

				/* then index on the negative of the boson
				   so that we know it is a list */
				if (_DtShmProtoAddInttab(indexList,
						last_boson, -index) != 0)
					goto fail;
				/* only one record is kept from here on; if
				   the block cannot shrink, the larger one
				   stays valid */
				list_count = 1;
				(void)grow_record_list(&list_of_recs, 1);
			}
			if ( i != next )
			{
				/* reset for the next set */
				last_boson = name_index[i].boson;
				list_of_recs[list_count-1] = name_index[i].rec;
			}
		}

	}

	/* same thing but they all go into a separate list */
	if(other_break > 0)
	{
		/* create the space */
		int	*list = _DtShmProtoAddIntLst(int_handle,
				other_break, &head->no_name_offset);

		if (list == nullptr)
			goto fail;

		/* copy it into the list */
		for(i = 0; i < other_break; i++)
		{
			list[i] = other[i].rec;
		}
	}
	else
	{
		head->no_name_offset = -1;
	}

	/* make the real space */
	size = _DtShmProtoSizeInttab(indexList);
	space = _DtShmProtoAddIntLst(int_handle, size/sizeof(int), 
				&head->name_list_offset);
	if (space == nullptr)
		goto fail;
	_DtShmProtoCopyInttab(indexList, space);
	_DtShmProtoDestroyInttab(indexList);
	free(name_index);
	free(list_of_recs);
	free(other);
	return(index);

fail:
	if (indexList)
		_DtShmProtoDestroyInttab(indexList);
	free(name_index);
	free(list_of_recs);
	free(other);
	return(-1);
}

static int
write_db(DtDtsMMHeader *header, void *index, int size, const char *CacheFile)
{
	int	fd;
	mode_t	cmask = umask((mode_t)077);
	char	*tmpfile;

	if ((tmpfile = malloc(sizeof(_DTDTSMMTEMPDIR) +
	    sizeof(_DTDTSMMTEMPFILE) + 7)) == nullptr) {
		_DtSimpleError(DtProgName, DtError, nullptr, tmpfile, nullptr);
		return 0;
	}

	sprintf(tmpfile, "%s/%sXXXXXX", _DTDTSMMTEMPDIR, _DTDTSMMTEMPFILE);
	fd = mkstemp(tmpfile);

	umask(cmask);

	if(fd ==  -1)
	{
		_DtSimpleError(
			DtProgName, DtError, nullptr,
			(char*) tmpfile, nullptr);
		free(tmpfile);
		return(0);
	}

	/* Remove file on write failure - we don't */
	/* want a partial dtdbcache file. */
	if ((write(fd, header, sizeof(DtDtsMMHeader))
	     != sizeof(DtDtsMMHeader)) ||
	    (write(fd, index, size) != size))
	{
		close(fd);
		unlink(tmpfile);
		free(tmpfile);
		return(0);
	}

	close(fd);

	if(rename((const char *)tmpfile, CacheFile) == -1)
	{
		_DtSimpleError(
			DtProgName, DtError, nullptr,
			(char*) CacheFile, nullptr);
		unlink(CacheFile); /* Just in case? */
		unlink(tmpfile);
		free(tmpfile);
		return(0);
	}
	free(tmpfile);
	return(1);
}


intptr_t _DtActionCompareRecordBoson(
        DtDtsMMRecord *record1,
        DtDtsMMRecord *record2 )
{
	int results = (int)record1->recordName - (int)record2->recordName;

	if (results)
		return(results);

	return((intptr_t)record1 - (intptr_t)record2);
}
