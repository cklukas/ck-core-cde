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
/*
 *      $XConsortium: objxm_strs.c /main/4 1995/11/06 18:47:35 rswiston $
 *
 * @(#)objxm_strs.c	1.19 15 Feb 1994      cde_app_builder/src/libABobjXm
 *
 *      RESTRICTED CONFIDENTIAL INFORMATION:
 *
 *      The information in this document is subject to special
 *      restrictions in a confidential disclosure agreement between
 *      HP, IBM, Sun, USL, SCO and Univel.  Do not distribute this
 *      document outside HP, IBM, Sun, USL, SCO, or Univel without
 *      Sun's specific written approval.  This document and all copies
 *      and derivative works thereof must be returned or destroyed at
 *      Sun's request.
 *
 *      Copyright 1993 Sun Microsystems, Inc.  All rights reserved.
 *
 */


/*
 ***********************************************************************
 * objxm_strs.c - routines to manage Resource & Enum strings &
 *          references
 *    
 *
 ***********************************************************************
 */

#include <stdio.h>
#include <assert.h>
#include <ab_private/XmAll.h>
#include <ab/util_types.h>
#include "objxmP.h"
#include <Dt/Term.h>
#include <Dt/ComboBox.h>
#include <Dt/SpinBox.h>

#define    XM_MAX_RESOURCES    	   500
#define    XM_MAX_CREATE_FUNCS     80
#define	   DEFAULT_UNSPEC	   (0xFFFF)
#define	   DEFAULT_SPEC_FUNC	   (0xFFFE)

/*
 * For building an index on a secondary key into a table.
 */
typedef struct
{
    long		rec_num;	/* record # (offset) in table */
} TableIndexEntryRec, *TableIndexEntry, *TableIndex;


/*************************************************************************
**                                                                      **
**       Private Function Declarations                    **
**                                                                      **
**************************************************************************/
static void    load_res_table(void);
static void    load_enum_table(void);
static void    load_xmcreate_table(void);
static int	build_xm_res_table_xmname_index(void);
static ObjxmRes *find_res_entry_by_xmname(ISTRING xmname);
static ObjxmRes *find_res_entry_by_strname(ISTRING xmname);


/*************************************************************************
**                                                                      **
**       Private Data                                                   **
**                                                                      **
**************************************************************************/
static ObjxmRes         xm_res_table[XM_MAX_RESOURCES];
static int              xm_res_cnt = 0;
static BOOL		xm_res_table_indexes_dirty = TRUE;
static TableIndex	xm_res_table_xmname_index = nullptr;
#define check_xm_res_table_indexes() \
	    (xm_res_table_indexes_dirty? \
		build_xm_res_table_xmname_index() \
	    : \
		0)
#define get_indexed_entry(_table,_index,_offset) \
	    (&((_table)[(_index)[_offset].rec_num]))

static ObjxmCreateFunc  xm_xmcreate_table[XM_MAX_CREATE_FUNCS];
static int              xm_xmcreate_cnt = 0;

/* AB_ARG_LITERAL Resource Enum Value Tables */
static ObjxmEnumTable align_tbl;
static ObjxmEnumTable animate_tbl;
static ObjxmEnumTable arrow_tbl;
static ObjxmEnumTable arrow_layout_tbl;
static ObjxmEnumTable attach_tbl;
static ObjxmEnumTable audible_tbl;
static ObjxmEnumTable blend_tbl;
static ObjxmEnumTable combobox_type_tbl;
static ObjxmEnumTable cw_loc_tbl;
static ObjxmEnumTable dbshadow_tbl;
static ObjxmEnumTable delresp_tbl;
static ObjxmEnumTable defbutton_tbl;
static ObjxmEnumTable dialog_tbl;
static ObjxmEnumTable dlgstyle_tbl;
static ObjxmEnumTable dragp_tbl;
static ObjxmEnumTable dragover_tbl;
static ObjxmEnumTable dropsite_tbl;
static ObjxmEnumTable editmode_tbl;
static ObjxmEnumTable istate_tbl;
static ObjxmEnumTable kbfocus_tbl;
static ObjxmEnumTable lblalign_tbl;
static ObjxmEnumTable lbltype_tbl;
static ObjxmEnumTable list_tbl;
static ObjxmEnumTable listsel_tbl;
static ObjxmEnumTable navtype_tbl;
static ObjxmEnumTable orient_tbl;
static ObjxmEnumTable pack_tbl;
static ObjxmEnumTable rctype_tbl;
static ObjxmEnumTable resize_tbl;
static ObjxmEnumTable sb_tbl;
static ObjxmEnumTable sbplace_tbl;
static ObjxmEnumTable sbscroll_tbl;
static ObjxmEnumTable shadow_tbl;
static ObjxmEnumTable strdir_tbl;
static ObjxmEnumTable swsize_tbl;
static ObjxmEnumTable tbtype_tbl;
static ObjxmEnumTable tearoff_tbl;
static ObjxmEnumTable texttype_tbl;
static ObjxmEnumTable unit_tbl;
static ObjxmEnumTable file_mask_tbl;

/* Resource Default Value Calculation Functions */
static XtArgVal	resize_default(WidgetClass wclass);

/*************************************************************************
**                                                                      **
**       Function Definitions                                           **
**                                                                      **
**************************************************************************/
/*
 * Initialize all Xm string tables
 */
void
objxm_strs_init(void)
{
    load_enum_table();
    load_res_table();
    load_xmcreate_table();

}

/*
 * Add a Resource (XmNfoo/"XmNfoo") to the resource table
 *
 * ASSUMES: xmname, strname_str ARE STRING CONSTANTS!
 */
static void
objxm_register_res(
    String    		xmname,
    STRING    		strname_str,
    AB_ARG_TYPE 	type,
    AB_ARG_CLASS	argClass,
    ObjxmEnumTable	*enum_table,
    XtArgVal		default_val,
    ObjxmResDefaultFunc default_func
)
{
    assert(xm_res_cnt < (XM_MAX_RESOURCES-1));
    xm_res_table[xm_res_cnt].xmname = istr_const(xmname);
    xm_res_table[xm_res_cnt].strname = istr_const(strname_str);
    xm_res_table[xm_res_cnt].type    = type;
    xm_res_table[xm_res_cnt].arg_class = argClass;
    xm_res_table[xm_res_cnt].enum_table = enum_table;
    xm_res_table[xm_res_cnt].default_val = default_val;
    xm_res_table[xm_res_cnt].default_func = default_func;
    ++xm_res_cnt;
    xm_res_table_indexes_dirty = TRUE;
}

/*
 * Add a Create Func (XmCreateFrame/"XmCreateFrame") to the xmcreate table
 */
/*
void
objxm_register_xmcreate_func(
    XmCreateFunc   ftn,
    STRING     ftnstr_str
)
{
    if (xm_xmcreate_cnt < XM_MAX_CREATE_FUNCS)
    {
        xm_xmcreate_table[xm_xmcreate_cnt].ftn = ftn;
        xm_xmcreate_table[xm_xmcreate_cnt++].ftnstr = istr_const(ftnstr_str);
    }
    else
        fprintf(stderr,"objxm_register_xmcreate_func: xmcreate table full\n");
}
*/

/*
 * Return the correct ISTRING for the passed-in xmname
 * i.e. IN: XmNwidth, OUT: "XmNwidth" istring
 */
ISTRING
objxm_get_res_strname(
    String    xmname
)
{
    int		i = 0;
    ISTRING	istr_xmname = istr_dup_existing(xmname);
    ISTRING	istr_name = nullptr;
    check_xm_res_table_indexes();

    for (i=0; i < xm_res_cnt; i++)
        if (istr_equal(istr_xmname, xm_res_table[i].xmname))
	{
            istr_name = istr_dup(xm_res_table[i].strname);
	    break;
	}

    istr_destroy(istr_xmname);
    return(istr_name);
}


/*
 * Return the correct ISTRING for the passed-in xmdef enum
 * i.e. IN: XmWORK_AREA, OUT: "XmWORK_AREA" istring 
 */ 
ISTRING
objxm_get_enum_strdef(
    String	  xmname,
    unsigned char xmdef
)
{
    int			i = 0;
    ObjxmRes		*entry = nullptr;
    ObjxmEnumTable	*enum_table = nullptr;
    ISTRING		istr_xmname = istr_dup_existing(xmname);
    ISTRING		istr_value = nullptr;
    int			enum_count = 0;
    check_xm_res_table_indexes();

    if (   ((entry = find_res_entry_by_xmname(istr_xmname)) != nullptr)
	&& ((enum_table = entry->enum_table) != nullptr) )
    {
	enum_count = enum_table->count;
    	for (i=0; i < enum_count; ++i)
            if (xmdef == enum_table->enums[i].xmdef)
	    {
               istr_value = istr_dup(enum_table->enums[i].strdef);
	       break;
	    }
    }
 
    istr_destroy(istr_xmname);
    return(istr_value);
}

/*
 * Return the correct ISTRING for the passed-in xmcreate ftn
 * i.e. IN: XmCreateFrame, OUT: "XmCreateFrame" istring
 */
ISTRING
objxm_get_xmcreate_ftnstr(
       XmCreateFunc    ftn 
)
{
    int i;
    ISTRING	istr_ftnstr = nullptr;
 
    for (i=0; i < xm_xmcreate_cnt; i++)
        if (ftn ==  xm_xmcreate_table[i].ftn)
	{
            istr_ftnstr = istr_dup(xm_xmcreate_table[i].ftnstr);
	    break;
	}
 
    return(istr_ftnstr);
}


/*
 * Return the correct resource the passed-in ISTRING
 * i.e. IN: "XmNwidth" istring, OUT: XmNwidth
 */
String
objxm_get_res_xmname(
    ISTRING strname
)
{
    int i;
    check_xm_res_table_indexes();
 
    for (i=0; i < xm_res_cnt; i++)
        if (istr_equal(strname, xm_res_table[i].strname))
            return(istr_string(xm_res_table[i].xmname));
 
    return(nullptr);
 
}

/*
 * Return the correct resource type for the passed-in ISTRING 
 * i.e. IN: "XtNwidth" istring, OUT: AB_ARG_INT
 */
AB_ARG_TYPE
objxm_get_istr_res_type(
    ISTRING strname
)
{
    int i;
    check_xm_res_table_indexes();

    for (i=0; i < xm_res_cnt; i++)
        if (istr_equal(strname, xm_res_table[i].strname))
            return(xm_res_table[i].type);

    return((AB_ARG_TYPE)ERROR);

}


/* 
 * Return the correct resource type for the passed-in String 
 * e.g., IN: XtNwidth string, OUT: AB_ARG_INT 
 */ 
AB_ARG_TYPE
objxm_get_res_type( 
    String xmname 
) 
{
    AB_ARG_TYPE		arg_type = AB_ARG_UNDEF;
    ISTRING		istr_xmname = istr_dup_existing(xmname); /* destroy */
    ObjxmRes		*entry;
    check_xm_res_table_indexes();
 
    if ((entry = find_res_entry_by_xmname(istr_xmname)) != nullptr)
    {
	arg_type = entry->type;
    }

    istr_destroy(istr_xmname);
    return(arg_type);
} 

/*
 * Return whether or not the specified Resource value is defined as the
 * "default" for that resource within the specified widget class
 */
BOOL
objxm_res_value_is_default(
    String 	xmname,
    WidgetClass	wclass,
    XtArgVal	value
)
{
    XtArgVal		arg_default = DEFAULT_UNSPEC;
    AB_ARG_TYPE		arg_type    = AB_ARG_UNDEF;
    ISTRING             istr_xmname = istr_dup_existing(xmname); /* destroy */
    BOOL		match 	    = False;
    ObjxmRes            *entry;
    check_xm_res_table_indexes();

    if ((entry = find_res_entry_by_xmname(istr_xmname)) != nullptr)
    {
        arg_default = entry->default_val;
	arg_type    = entry->type;

	if (arg_default == DEFAULT_SPEC_FUNC)
	{
	    /* Call the function to calculate the resource's default
	     * based on the widget class
	     */
	    if (entry->default_func != nullptr)
		match = ((*entry->default_func)(wclass) == value);

	}
	else if (arg_default != DEFAULT_UNSPEC) /* Default is directly specified */
    	{
    	    switch(arg_type)
    	    {
	    	case AB_ARG_BOOLEAN:
	    	case AB_ARG_FLOAT:
	    	case AB_ARG_INT:
	    	case AB_ARG_LITERAL:
	    	case AB_ARG_PIXEL:
	    	case AB_ARG_PIXMAP:
	    	case AB_ARG_MNEMONIC:
		    match = (arg_default == value);
		    break;
	    	case AB_ARG_STRING:
		    match = (strcmp((char*)arg_default, (char*)value) == 0);
		    break;
	    	case AB_ARG_XMSTRING:
		    match = XmStringCompare((XmString)arg_default, (XmString)value);
		    break;
	    	default: /* Others do not have defaults specified */
		    break;
	    }
    	}
    }
    istr_destroy(istr_xmname);
    return(match);
}


static AB_ARG_CLASS
adjust_arg_class(
			ObjxmRes		*entry
)
{
    AB_ARG_CLASS	argClass = entry->arg_class;

    if (argClass == AB_ARG_CLASS_UNDEF)
    {
        switch (entry->type)
        {
	    case AB_ARG_FONT:
	    case AB_ARG_STRING:
	    case AB_ARG_XMSTRING:
	        argClass = AB_ARG_CLASS_OTHER_STRING;
	    break;
    
	    default:
	        argClass = AB_ARG_CLASS_OTHER;
	    break;
        }

	entry->arg_class = argClass;
    }

    return argClass;
}


/* 
 * return arg class corresponding to resource strname 
 */
AB_ARG_CLASS
objxm_get_istr_res_class(
                    ISTRING    strname
)
{
    AB_ARG_CLASS	argClass = AB_ARG_CLASS_UNDEF;
    ObjxmRes		*entry;
    check_xm_res_table_indexes();

    if ((entry = find_res_entry_by_strname(strname)) != nullptr)
    {
	/*
	 * The unset values are set to UNDEF. See if we can do
	 * better than that.
	 */
	argClass = entry->arg_class;
        if (argClass == AB_ARG_CLASS_UNDEF)
	{
	    argClass = adjust_arg_class(entry);
	}
    }

    return argClass;
}


/* 
 * return arg class corresponding to resource xmname 
 */
AB_ARG_CLASS
objxm_get_res_class(
                    String      xmname
)
{
    AB_ARG_CLASS	arg_class = AB_ARG_CLASS_UNDEF;
    ISTRING		istr_xmname = istr_dup_existing(xmname); /* destroy */
    ObjxmRes		*entry;
    check_xm_res_table_indexes();
 
    if ((entry = find_res_entry_by_xmname(istr_xmname)) != nullptr)
    {
	arg_class = entry->arg_class;
        if (arg_class == AB_ARG_CLASS_UNDEF)
	{
	    arg_class = adjust_arg_class(entry);
	}
    }

    istr_destroy(istr_xmname);
    return(arg_class);
}


/*
 * Return the correct enum xmdef for the passed-in ISTRING
 * i.e. IN: "XmWORK_AREA" istring, OUT: XmWORK_AREA  
 */  
unsigned char
objxm_get_enum_xmdef(
    String	xmname,
    ISTRING    strdef
)
{
    unsigned char	xmdef = 0;
    int 		i;
    ObjxmEnumTable 	*enum_table = nullptr;
    ISTRING		istr_xmname = istr_dup_existing(xmname);
    check_xm_res_table_indexes();

    for (i=0; i < xm_res_cnt; i++)
        if (istr_equal(istr_xmname, xm_res_table[i].xmname))
        {
            enum_table = xm_res_table[i].enum_table;
            break;
        }

    if (enum_table != nullptr)
    {
        for (i=0; i < enum_table->count; i++) 
            if (istr_equal(strdef, enum_table->enums[i].strdef)) 
	    {
                xmdef = (enum_table->enums[i].xmdef);
		break;
	    }
    } 

    istr_destroy(istr_xmname);
    return(xmdef);
 
} 

/*
 * Return the correct xmcreate ftn for the passed-in ISTRING
 * i.e. IN: "XmCreateFrame" istring, OUT: XmCreateFrame 
 */
XmCreateFunc
objxm_get_xmcreate_ftn(
    ISTRING ftnstr
)
{
    int i;
 
    for (i=0; i < xm_xmcreate_cnt; i++)
        if (istr_equal(ftnstr, xm_xmcreate_table[i].ftnstr))
            return(xm_xmcreate_table[i].ftn);
 
    return(nullptr);
 
}


#ifdef BOGUS
/*
 * I removed this and replaced it with calls to objxm_register_res. This
 * reduced the size of the .o file from 222K to 127K, because this code
 * was being expanded inline
 */
static int
xm_reg_res(
    int			index,
    String		xmName,
    String		stringName,
    AB_ARG_TYPE		argType,
    ObjxmEnumTable	*enumTable,
    void		*default_val,
    ObjxmResDefaultFunc default_func
)
{
#define    XM_DO_REG_RES(i_, xmn_, strn_, type_, enumt_, defv_, deff_) \
          {  xm_res_table[i_].xmname = istr_const(xmn_); \
             xm_res_table[(i_)].strname = istr_const(strn_); \
   	     xm_res_table[(i_)].type = (type_);  \
	     xm_res_table[(i_)].enum_table = (enumt_); \
	     xm_res_table[(i_)].default_val = (defv_); \
	     xm_res_table[(i_)].default_func = (deff_); \
	     xm_res_table_indexes_dirty = TRUE; }
    assert(index < (XM_MAX_RESOURCES-1));
    XM_DO_REG_RES(index, xmName, stringName, argType, 
	enumTable, default_val, default_func);
    return 0;

#undef XM_REG_RES
}
#endif /* BOGUS */


/*
 * Load the resource table with known Motif/Xt resource names
 */
static void
load_res_table(void)
{
#define REG(xmn_, strn_, type_, enumt_, defv_, deff_) \
	  (objxm_register_res(xmn_, strn_, type_, AB_ARG_CLASS_UNDEF, enumt_, defv_, deff_));
#define REGC(xmn_, strn_, type_, class_, enumt_, defv_, deff_) \
	  (objxm_register_res(xmn_, strn_, type_, class_, enumt_, defv_, deff_));

 xm_res_cnt = 0;
 REG(DtNarrowLayout, "DtNarrowLayout", AB_ARG_LITERAL, &arrow_layout_tbl,(XtArgVal)istr_const("DtARROWS_END"),nullptr)
 REG(DtNcomboBoxType, "DtNcomboBoxType", AB_ARG_LITERAL, &combobox_type_tbl,(XtArgVal)istr_const("DtDROP_DOWN_LIST"),nullptr)
 REG(DtNincrementValue, "DtNincrementValue", AB_ARG_INT, nullptr, (XtArgVal)1, nullptr)
 REGC(DtNmaximumValue, "DtNmaximumValue", AB_ARG_INT,AB_ARG_CLASS_VALUE,nullptr, (XtArgVal)10, nullptr)
 REGC(DtNminimumValue, "DtNminimumValue", AB_ARG_INT,AB_ARG_CLASS_VALUE,nullptr,(XtArgVal)0, nullptr)
 REGC(DtNposition, "DtNposition", AB_ARG_INT, AB_ARG_CLASS_GEOMETRY, nullptr,(XtArgVal)0, nullptr)
 REGC(DtNselectedPosition, "DtNselectedPosition", AB_ARG_INT, AB_ARG_CLASS_VALUE, nullptr,(XtArgVal)1, nullptr)
 REG(DtNselectionCallback, "DtNselectionCallback", AB_ARG_CALLBACK, nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(DtNspinBoxChildType, "DtNspinBoxChildType", AB_ARG_LITERAL,&texttype_tbl,(XtArgVal)istr_const("DtSTRING"),nullptr)
 REGC(DtNsubprocessCmd, "DtNsubprocessCmd", AB_ARG_STRING, AB_ARG_CLASS_OTHER_STRING, nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(DtNverticalScrollBar, "DtNverticalScrollBar", AB_ARG_WIDGET, nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG("slidingMode", "\"slidingMode\"",AB_ARG_BOOLEAN,nullptr,(XtArgVal)0, nullptr)
 REG(XmNallowShellResize, "XmNallowShellResize", AB_ARG_BOOLEAN,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNbackground, "XmNbackground", AB_ARG_PIXEL,AB_ARG_CLASS_COLOR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNbackgroundPixmap, "XmNbackgroundPixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNbitmap, "XmNbitmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNborderColor , "XmNborderColor", AB_ARG_PIXEL,AB_ARG_CLASS_COLOR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNborderPixmap, "XmNborderPixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNborderWidth , "XmNborderWidth", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdestroyCallback, "XmNdestroyCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNfont, "XmNfont", AB_ARG_FONT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNforeground, "XmNforeground", AB_ARG_PIXEL,AB_ARG_CLASS_COLOR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNgeometry, "XmNgeometry", AB_ARG_STRING,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNheight, "XmNheight", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNheightInc, "XmNheightInc", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNiconMask, "XmNiconMask", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNiconName, "XmNiconName", AB_ARG_STRING,AB_ARG_CLASS_NON_GLYPH_LABEL,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNiconNameEncoding, "XmNiconNameEncoding", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNiconPixmap, "XmNiconPixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNiconWindow, "XmNiconWindow", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNiconX, "XmNiconX", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNiconY, "XmNiconY", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNiconic, "XmNiconic", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False, nullptr)
 REG(XmNinitialState, "XmNinitialState", AB_ARG_LITERAL,&istate_tbl,(XtArgVal)istr_const("NormalState"), nullptr)
 REG(XmNmappedWhenManaged, "XmNmappedWhenManaged", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REGC(XmNmaxAspectX, "XmNmaxAspectX", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNmaxAspectY, "XmNmaxAspectY", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNmaxHeight, "XmNmaxHeight", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNmaxWidth, "XmNmaxWidth", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNminAspectX, "XmNminAspectX", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNminAspectY, "XmNminAspectY", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNminHeight, "XmNminHeight", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNminWidth, "XmNminWidth", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNorientation, "XmNorientation", AB_ARG_LITERAL,&orient_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNpopdownCallback , "XmNpopdownCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNpopupCallback, "XmNpopupCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsensitive, "XmNsensitive", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REGC(XmNspace, "XmNspace", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNtitle, "XmNtitle", AB_ARG_STRING,AB_ARG_CLASS_NON_GLYPH_LABEL,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtitleEncoding, "XmNtitleEncoding", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtranslations, "XmNtranslations", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNvalue, "XmNvalue", AB_ARG_VARIABLE, AB_ARG_CLASS_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNwidth, "XmNwidth", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNwidthInc, "XmNwidthInc", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNx, "XmNx", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNy, "XmNy", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNaccelerator, "XmNaccelerator", AB_ARG_STRING,AB_ARG_CLASS_OTHER_STRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNacceleratorText,  "XmNacceleratorText", AB_ARG_XMSTRING,AB_ARG_CLASS_OTHER_STRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNactivateCallback,  "XmNactivateCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNadjustLast, "XmNadjustLast", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True, nullptr)
 REG(XmNadjustMargin,  "XmNadjustMargin", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REGC(XmNalignment, "XmNalignment", AB_ARG_LITERAL, AB_ARG_CLASS_GEOMETRY,&lblalign_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNallowOverlap,  "XmNallowOverlap", AB_ARG_BOOLEAN,nullptr, (XtArgVal)True, nullptr)
 REG(XmNallowResize, "XmNallowResize", AB_ARG_BOOLEAN, nullptr, (XtArgVal)False, nullptr)
 REG(XmNanimationMask,  "XmNanimationMask", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNanimationPixmap,  "XmNanimationPixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNanimationPixmapDepth, "XmNanimationPixmapDepth", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNanimationStyle,  "XmNanimationStyle", AB_ARG_LITERAL,&animate_tbl,(XtArgVal)istr_const("XmDRAG_UNDER_HIGHLIGHT"),nullptr)
 REG(XmNapplyCallback,  "XmNapplyCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNapplyLabelString,  "XmNapplyLabelString", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNarrowDirection,  "XmNarrowDirection", AB_ARG_LITERAL,&arrow_tbl,(XtArgVal)istr_const("XmARROW_UP"),nullptr)
 REG(XmNaudibleWarning,  "XmNaudibleWarning", AB_ARG_LITERAL,&audible_tbl,(XtArgVal)istr_const("XmBELL"),nullptr)
 REG(XmNautoShowCursorPosition, "XmNautoShowCursorPosition", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNautoUnmanage,  "XmNautoUnmanage", AB_ARG_BOOLEAN,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNblendModel, "XmNblendModel", AB_ARG_LITERAL,&blend_tbl,(XtArgVal)istr_const("XmBLEND_ALL"),nullptr)
 REG(XmNblinkRate, "XmNblinkRate", AB_ARG_INT,nullptr,(XtArgVal)500,nullptr)
 REGC(XmNbottomAttachment, "XmNbottomAttachment", AB_ARG_LITERAL,AB_ARG_CLASS_GEOMETRY,&attach_tbl,
	(XtArgVal)istr_const("XmATTACH_NONE"),nullptr)
 REGC(XmNbottomOffset, "XmNbottomOffset", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNbottomPosition,  "XmNbottomPosition", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNbottomShadowColor,  "XmNbottomShadowColor", AB_ARG_PIXEL,AB_ARG_CLASS_COLOR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNbottomShadowPixmap,  "XmNbottomShadowPixmap", AB_ARG_PIXMAP,AB_ARG_CLASS_COLOR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNbottomWidget,  "XmNbottomWidget", AB_ARG_WIDGET, AB_ARG_CLASS_GEOMETRY,nullptr,0,nullptr)
 REG(XmNbrowseSelectionCallback, "XmNbrowseSelectionCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNbuttonAcceleratorText, "XmNbuttonAcceleratorText", AB_ARG_STRING,AB_ARG_CLASS_OTHER_STRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNcommandWindowLocation, "XmNcommandWindowLocation",AB_ARG_LITERAL,&cw_loc_tbl,0,nullptr)
 REGC(XmNbuttonAccelerators,  "XmNbuttonAccelerators", AB_ARG_STRING,AB_ARG_CLASS_OTHER_STRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNbuttonCount, "XmNbuttonCount", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNbuttonFontList,  "XmNbuttonFontList", AB_ARG_FONT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNbuttonMnemonicCharSets, "XmNbuttonMnemonicCharSets", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNbuttonMnemonics,  "XmNbuttonMnemonics", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNcancelButton,  "XmNcancelButton", AB_ARG_WIDGET,nullptr,0,nullptr)
 REG(XmNcancelCallback,  "XmNcancelCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNcancelLabelString,  "XmNcancelLabelString", AB_ARG_XMSTRING, AB_ARG_CLASS_NON_GLYPH_LABEL,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNcascadePixmap,  "XmNcascadePixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNcascadingCallback,  "XmNcascadingCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNchildPlacement,  "XmNchildPlacement", AB_ARG_UNDEF,nullptr,(XtArgVal)istr_const("XmPLACE_ABOVE_SELECTION"),nullptr)
 REG(XmNclientData, "XmNclientData", AB_ARG_VOID_PTR,nullptr,0,nullptr)
 REGC(XmNcolumns, "XmNcolumns", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)20, nullptr)
 REG(XmNcommand, "XmNcommand", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNcommandChangedCallback, "XmNcommandChangedCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNcommandEnteredCallback, "XmNcommandEnteredCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNcommandWindow,  "XmNcommandWindow", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNconvertProc,      "XmNconvertProc", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNcursorBackground,  "XmNcursorBackground", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNcursorForeground,  "XmNcursorForeground", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNcursorPosition,  "XmNcursorPosition", AB_ARG_INT,nullptr,(XtArgVal)0, nullptr)
 REG(XmNcursorPositionVisible, "XmNcursorPositionVisible", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNdecimalPoints,  "XmNdecimalPoints", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNdecrementCallback,  "XmNdecrementCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdefaultActionCallback, "XmNdefaultActionCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdefaultButton,  "XmNdefaultButton", AB_ARG_WIDGET,nullptr,0,nullptr)
 REG(XmNdefaultButtonShadowThickness, "XmNdefaultButtonShadowThickness", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdefaultButtonType,  "XmNdefaultButtonType", AB_ARG_LITERAL,&defbutton_tbl,(XtArgVal)istr_const("XmDIALOG_OK_BUTTON"),nullptr)
 REG(XmNdefaultCopyCursorIcon, "XmNdefaultCopyCursorIcon", AB_ARG_WIDGET,nullptr,0,nullptr)
 REG(XmNdefaultFontList,  "XmNdefaultFontList", AB_ARG_FONT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdefaultInvalidCursorIcon, "XmNdefaultInvalidCursorIcon", AB_ARG_WIDGET,nullptr,0,nullptr)
 REG(XmNdefaultLinkCursorIcon, "XmNdefaultLinkCursorIcon", AB_ARG_WIDGET,nullptr,0,nullptr)
 REG(XmNdefaultMoveCursorIcon, "XmNdefaultMoveCursorIcon", AB_ARG_WIDGET,nullptr,0,nullptr)
 REG(XmNdefaultMoveCursorIcon, "XmNdefaultMoveCursorIcon", AB_ARG_WIDGET,nullptr,0,nullptr)
 REG(XmNdefaultNoneCursorIcon, "XmNdefaultNoneCursorIcon", AB_ARG_WIDGET,nullptr,0,nullptr)
 REG(XmNdefaultPosition,  "XmNdefaultPosition", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNdefaultSourceCursorIcon, "XmNdefaultSourceCursorIcon", AB_ARG_WIDGET,nullptr,0,nullptr)
 REG(XmNdefaultValidCursorIcon, "defaultValidCursorIcon", AB_ARG_WIDGET,nullptr,0,nullptr)
 REG(XmNdeleteResponse, "XmNdeleteResponse", AB_ARG_LITERAL,&delresp_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdialogStyle, "XmNdialogStyle", AB_ARG_LITERAL, &dlgstyle_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNdialogTitle, "XmNdialogTitle", AB_ARG_XMSTRING,AB_ARG_CLASS_NON_GLYPH_LABEL,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdialogType, "XmNdialogType", AB_ARG_LITERAL, &dialog_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdirListItemCount, "XmNdirListItemCount", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdirListItems, "XmNdirListItems", AB_ARG_XMSTRING_TBL,nullptr,0,nullptr)
 REG(XmNdirListLabelString, "XmNdirListLabelString", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdirMask, "XmNdirMask", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdirSearchProc, "XmNdirSearchProc", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdirSpec, "XmNdirSpec", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNdirectory, "XmNdirectory", AB_ARG_XMSTRING,AB_ARG_CLASS_VALUE,nullptr,0,nullptr)
 REG(XmNdirectoryValid, "XmNdirectoryValid", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNdisarmCallback, "XmNdisarmCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdoubleClickInterval, "XmNdoubleClickInterval", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdragCallback, "XmNdragCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdragContextClass, "XmNdragContextClass", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdragDropFinishCallback, "XmNdragDropFinishCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdragIconClass, "XmNdragIconClass", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdragInitiatorProtocolStyle, "XmNdragInitiatorProtocolStyle", AB_ARG_LITERAL,
			&dragp_tbl,(XtArgVal)istr_const("XmDRAG_PREFER_PREREGISTER"),nullptr)
 REG(XmNdragMotionCallback, "XmNdragMotionCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdragOverMode, "XmNdragOverMode", AB_ARG_LITERAL, &dragover_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdragProc, "XmNdragProc", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdragReceiverProtocolStyle, "XmNdragReceiverProtocolStyle", AB_ARG_LITERAL,
			&dragp_tbl,(XtArgVal)istr_const("XmDRAG_PREFER_PREREGISTER"),nullptr)
 REG(XmNdropFinishCallback, "XmNdropFinishCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdropProc, "XmNdropProc", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdropRectangles, "XmNdropRectangles", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdropSiteActivity, "XmNdropSiteActivity", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdropSiteEnterCallback, "XmNdropSiteEnterCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdropSiteLeaveCallback, "XmNdropSiteLeaveCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdropSiteManagerClass, "XmNdropSiteManagerClass", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdropSiteOperations, "XmNdropSiteOperations", AB_ARG_UNDEF,nullptr,
	(XtArgVal)XmDROP_MOVE | XmDROP_COPY,nullptr)
 REG(XmNdropSiteType, "XmNdropSiteType", AB_ARG_LITERAL, &dropsite_tbl,(XtArgVal)istr_const("XmDROP_SITE_SIMPLE"),nullptr)
 REG(XmNdropStartCallback, "XmNdropStartCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdropTransferClass, "XmNdropTransferClass", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNdropTransfers, "XmNdropTransfers", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNeditMode, "XmNeditMode", AB_ARG_LITERAL, &editmode_tbl,(XtArgVal)istr_const("XmSINGLE_LINE_EDIT"),nullptr)
 REG(XmNeditable, "XmNeditable", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNentryAlignment, "XmNentryAlignment", AB_ARG_LITERAL, &lblalign_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNentryCallback, "XmNentryCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNentryClass, "XmNentryClass", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNentryVerticalAlignment, "XmNentryVerticalAlignment", AB_ARG_LITERAL,
			&align_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNexportTargets, "XmNexportTargets", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNexposeCallback, "XmNexposeCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNextendedSelectionCallback, "XmNextendedSelectionCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNfileListItemCount, "XmNfileListItemCount", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNfileListItems, "XmNfileListItems", AB_ARG_XMSTRING_TBL,nullptr,0,nullptr)
 REG(XmNfileListLabelString, "XmNfileListLabelString", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNfileSearchProc, "XmNfileSearchProc", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNfileTypeMask, "XmNfileTypeMask", AB_ARG_LITERAL,&file_mask_tbl,(XtArgVal)istr_const("XmFILE_REGULAR"),nullptr)
 REG(XmNfillOnArm, "XmNfillOnArm", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNfillOnSelect, "XmNfillOnSelect", AB_ARG_BOOLEAN,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNfilterLabelString, "XmNfilterLabelString", AB_ARG_XMSTRING,AB_ARG_CLASS_NON_GLYPH_LABEL,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNfocusCallback, "XmNfocusCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNfocusMovedCallback, "XmNfocusMovedCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNfontList, "XmNfontList", AB_ARG_FONT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNfractionBase, "XmNfractionBase", AB_ARG_INT,nullptr,(XtArgVal)100,nullptr)
 REG(XmNgainPrimaryCallback, "XmNgainPrimaryCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhelpCallback, "XmNhelpCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhelpLabelString, "XmNhelpLabelString", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNhighlightColor, "XmNhighlightColor", AB_ARG_PIXEL,AB_ARG_CLASS_COLOR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhighlightOnEnter, "XmNhighlightOnEnter", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNhighlightPixmap, "XmNhighlightPixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhighlightThickness, "XmNhighlightThickness", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhistoryItemCount, "XmNhistoryItemCount", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhistoryItems, "XmNhistoryItems", AB_ARG_XMSTRING_TBL,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhistoryMaxItems, "XmNhistoryMaxItems", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhistoryVisibleItemCount, "XmNhistoryVisibleItemCount", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhorizontalFontUnit, "XmNhorizontalFontUnit", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhorizontalScrollBar, "XmNhorizontalScrollBar", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhorizontalSpacing, "XmNhorizontalSpacing", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNhotX, "XmNhotX", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNhotY, "XmNhotY", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNiccHandle, "XmNiccHandle", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNimportTargets, "XmNimportTargets", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNincrement, "XmNincrement", AB_ARG_INT,nullptr,(XtArgVal)1,nullptr)
 REG(XmNincrementCallback, "XmNincrementCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNincremental, "XmNincremental", AB_ARG_BOOLEAN,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNindicatorOn, "XmNindicatorOn", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNindicatorSize, "XmNindicatorSize", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNindicatorType, "XmNindicatorType", AB_ARG_LITERAL, &tbtype_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNinitialDelay, "XmNinitialDelay", AB_ARG_INT,nullptr,(XtArgVal)250,nullptr)
 REG(XmNinitialFocus, "XmNinitialFocus", AB_ARG_BOOLEAN,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNinputCallback, "XmNinputCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNinputCreate, "XmNinputCreate", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNinputMethod, "XmNinputMethod", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNinvalidCursorForeground, "XmNinvalidCursorForeground", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNisAligned, "XmNisAligned", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNisHomogeneous, "XmNisHomogeneous", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNitemCount, "XmNitemCount", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNitems, "XmNitems", AB_ARG_XMSTRING_TBL,nullptr,0,nullptr)
 REG(XmNkeyboardFocusPolicy, "XmNkeyboardFocusPolicy", AB_ARG_LITERAL, &kbfocus_tbl,(XtArgVal)istr_const("XmEXPLICIT"),nullptr)
 REG(XmNlabelFontList, "XmNlabelFontList", AB_ARG_FONT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNlabelInsensitivePixmap, "XmNlabelInsensitivePixmap", AB_ARG_PIXMAP,AB_ARG_CLASS_GLYPH,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNlabelPixmap, "XmNlabelPixmap", AB_ARG_PIXMAP,AB_ARG_CLASS_GLYPH,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNlabelString, "XmNlabelString", AB_ARG_XMSTRING, AB_ARG_CLASS_NON_GLYPH_LABEL,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNlabelType, "XmNlabelType", AB_ARG_LITERAL, AB_ARG_CLASS_GLYPH, &lbltype_tbl,(XtArgVal)istr_const("XmSTRING"),nullptr)
 REGC(XmNleftAttachment, "XmNleftAttachment", AB_ARG_LITERAL,AB_ARG_CLASS_GEOMETRY,&attach_tbl,(XtArgVal)istr_const("XmATTACH_NONE"),nullptr)
 REGC(XmNleftOffset, "XmNleftOffset", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNleftPosition, "XmNleftPosition", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNleftWidget, "XmNleftWidget", AB_ARG_WIDGET,AB_ARG_CLASS_GEOMETRY,nullptr,0,nullptr)
 REG(XmNlistItemCount, "XmNlistItemCount", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNlistItems, "XmNlistItems", AB_ARG_XMSTRING_TBL,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNlistLabelString, "XmNlistLabelString", AB_ARG_XMSTRING,AB_ARG_CLASS_NON_GLYPH_LABEL, nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNlistMarginHeight, "XmNlistMarginHeight", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REG(XmNlistMarginWidth, "XmNlistMarginWidth", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNlistSizePolicy, "XmNlistSizePolicy", AB_ARG_LITERAL, &swsize_tbl,(XtArgVal)istr_const("XmVARIABLE"),nullptr)
 REG(XmNlistSpacing, "XmNlistSpacing", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNlistUpdated, "XmNlistUpdated", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REGC(XmNlistVisibleItemCount, "XmNlistVisibleItemCount", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)8,nullptr)
 REG(XmNlosePrimaryCallback, "XmNlosePrimaryCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNlosingFocusCallback, "XmNlosingFocusCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmainWindowMarginHeight, "XmNmainWindowMarginHeight", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNmainWindowMarginWidth, "XmNmainWindowMarginWidth", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNmapCallback, "XmNmapCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmappingDelay, "XmNmappingDelay", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNmargin, "XmNmargin", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNmarginBottom, "XmNmarginBottom", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNmarginHeight, "XmNmarginHeight", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNmarginLeft, "XmNmarginLeft", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNmarginRight, "XmNmarginRight", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNmarginTop, "XmNmarginTop", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNmarginWidth, "XmNmarginWidth", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmask, "XmNmask", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmaxLength, "XmNmaxLength", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmaximum, "XmNmaximum", AB_ARG_INT,nullptr,(XtArgVal)100,nullptr)
 REGC(XmNmenuAccelerator, "XmNmenuAccelerator", AB_ARG_STRING,AB_ARG_CLASS_OTHER_STRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmenuBar, "XmNmenuBar", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmenuCursor, "XmNmenuCursor", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmenuHelpWidget, "XmNmenuHelpWidget", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmenuHistory, "XmNmenuHistory", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNmenuPost, "XmNmenuPost", AB_ARG_STRING,AB_ARG_CLASS_OTHER_STRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmessageAlignment, "XmNmessageAlignment", AB_ARG_LITERAL, &lblalign_tbl,(XtArgVal)istr_const("XmALIGNMENT_BEGINNING"),nullptr)
 REG(XmNmessageProc, "XmNmessageProc", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNmessageString, "XmNmessageString", AB_ARG_XMSTRING,AB_ARG_CLASS_NON_GLYPH_LABEL,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmessageWindow, "XmNmessageWindow", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNminimizeButtons, "XmNminimizeButtons", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REGC(XmNminimum, "XmNminimum", AB_ARG_INT,AB_ARG_CLASS_VALUE,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNmnemonic, "XmNmnemonic", AB_ARG_MNEMONIC,AB_ARG_CLASS_OTHER_STRING, nullptr, (XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNmnemonicCharSet, "XmNmnemonicCharSet", AB_ARG_UNDEF,AB_ARG_CLASS_OTHER_STRING,nullptr,(XtArgVal)XmFONTLIST_DEFAULT_TAG,nullptr)
 REG(XmNmodifyVerifyCallback, "XmNmodifyVerifyCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmodifyVerifyCallbackWcs, "XmNmodifyVerifyCallbackWcs", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmotionVerifyCallback, "XmNmotionVerifyCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmoveOpaque, "XmNmoveOpaque", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNmultiClick, "XmNmultiClick", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmultipleSelectionCallback, "XmNmultipleSelectionCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmustMatch, "XmNmustMatch", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNmwmDecorations, "XmNmwmDecorations", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmwmFunctions, "XmNmwmFunctions", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmwmInputMode, "XmNmwmInputMode", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmwmMenu, "XmNmwmMenu", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNmwmMessages, "XmNmwmMessages", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNnavigationType, "XmNnavigationType", AB_ARG_LITERAL, &navtype_tbl,(XtArgVal)istr_const("XmTAB_GROUP"),nullptr)
 REG(XmNneedsMotion, "XmNneedsMotion", AB_ARG_BOOLEAN,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNnoMatchCallback, "XmNnoMatchCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNnoMatchString, "XmNnoMatchString", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNnoResize, "XmNnoResize", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNnoneCursorForeground, "XmNnoneCursorForeground", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNnotifyProc, "XmNnotifyProc", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNnumColumns, "XmNnumColumns", AB_ARG_INT,nullptr,(XtArgVal)1,nullptr)
 REG(XmNnumDropRectangles, "XmNnumDropRectangles", AB_ARG_INT,nullptr,(XtArgVal)1,nullptr)
 REG(XmNnumDropTransfers, "XmNnumDropTransfers", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNnumExportTargets, "XmNnumExportTargets", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNnumImportTargets, "XmNnumImportTargets", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNoffsetX, "XmNoffsetX", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNoffsetY, "XmNoffsetY", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REG(XmNokCallback, "XmNokCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNokLabelString, "XmNokLabelString", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNoperationChangedCallback, "XmNoperationChangedCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNoperationCursorIcon, "XmNoperationCursorIcon", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNoptionLabel, "XmNoptionLabel", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNoptionMnemonic, "XmNoptionMnemonic", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNoutputCreate, "XmNoutputCreate", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNpacking, "XmNpacking", AB_ARG_LITERAL, &pack_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNpageDecrementCallback, "XmNpageDecrementCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNpageIncrement, "XmNpageIncrement", AB_ARG_INT,nullptr,(XtArgVal)10,nullptr)
 REG(XmNpageIncrementCallback, "XmNpageIncrementCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNpaneMaximum, "XmNpaneMaximum", AB_ARG_INT,nullptr,(XtArgVal)1000,nullptr)
 REG(XmNpaneMinimum, "XmNpaneMinimum", AB_ARG_INT,nullptr,(XtArgVal)1,nullptr)
 REGC(XmNpattern, "XmNpattern", AB_ARG_XMSTRING,AB_ARG_CLASS_VALUE,nullptr,0,nullptr)
 REG(XmNpendingDelete, "XmNpendingDelete", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNpopupEnabled, "XmNpopupEnabled", AB_ARG_BOOLEAN,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNpositionIndex, "XmNpositionIndex", AB_ARG_INT,nullptr,(XtArgVal)XmLAST_POSITION,nullptr)
 REGC(XmNpreeditType, "XmNpreeditType", AB_ARG_STRING,AB_ARG_CLASS_OTHER_STRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNprocessingDirection, "XmNprocessingDirection", AB_ARG_LITERAL, &sb_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNpromptString, "XmNpromptString", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNprotocolCallback, "XmNprotocolCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNpushButtonEnabled, "XmNpushButtonEnabled", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNqualifySearchDataProc, "XmNqualifySearchDataProc", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNradioAlwaysOne, "XmNradioAlwaysOne", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNradioBehavior, "XmNradioBehavior", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNrealizeCallback, "XmNrealizeCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNrecomputeSize, "XmNrecomputeSize", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNrepeatDelay, "XmNrepeatDelay", AB_ARG_INT,nullptr,(XtArgVal)50,nullptr)
 REG(XmNresizable, "XmNresizable", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNresizeCallback, "XmNresizeCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNresizeHeight, "XmNresizeHeight", AB_ARG_INT,nullptr,(XtArgVal)True,nullptr)
 REG(XmNresizePolicy, "XmNresizePolicy", AB_ARG_LITERAL, &resize_tbl,(XtArgVal)DEFAULT_SPEC_FUNC,resize_default)
 REG(XmNresizeWidth, "XmNresizeWidth", AB_ARG_INT,nullptr,(XtArgVal)True,nullptr)
 REGC(XmNrightAttachment, "XmNrightAttachment", AB_ARG_LITERAL,AB_ARG_CLASS_GEOMETRY,&attach_tbl,(XtArgVal)istr_const("XmATTACH_NONE"),nullptr)
 REGC(XmNrightOffset, "XmNrightOffset", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNrightPosition, "XmNrightPosition", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNrightWidget, "XmNrightWidget", AB_ARG_WIDGET,AB_ARG_CLASS_GEOMETRY,nullptr,0,nullptr)
 REG(XmNrowColumnType, "XmNrowColumnType", AB_ARG_LITERAL, &rctype_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNrows, "XmNrows", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)1,nullptr)
 REG(XmNrubberPositioning, "XmNrubberPositioning", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNsashHeight, "XmNsashHeight", AB_ARG_INT,nullptr,(XtArgVal)10,nullptr)
 REG(XmNsashIndent, "XmNsashIndent", AB_ARG_INT,nullptr,(XtArgVal)-10,nullptr)
 REG(XmNsashShadowThickness, "XmNsashShadowThickness", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsashWidth, "XmNsashWidth", AB_ARG_INT,nullptr,(XtArgVal)10,nullptr)
 REGC(XmNscaleHeight, "XmNscaleHeight", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REG(XmNscaleMultiple, "XmNscaleMultiple", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNscaleWidth, "XmNscaleWidth", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REG(XmNscrollBarDisplayPolicy, "XmNscrollBarDisplayPolicy", AB_ARG_LITERAL,
			&sbscroll_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNscrollBarPlacement, "XmNscrollBarPlacement", AB_ARG_LITERAL, &sbplace_tbl,(XtArgVal)istr_const("XmBOTTOM_RIGHT"),nullptr)
 REG(XmNscrollHorizontal, "XmNscrollHorizontal", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNscrollLeftSide, "XmNscrollLeftSide", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNscrollTopSide, "XmNscrollTopSide", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNscrollVertical, "XmNscrollVertical", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNscrolledWindowMarginHeight, "XmNscrolledWindowMarginHeight", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNscrolledWindowMarginWidth, "XmNscrolledWindowMarginWidth", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REG(XmNscrollingPolicy, "XmNscrollingPolicy", AB_ARG_LITERAL, &sbscroll_tbl,(XtArgVal)istr_const("XmAPPLICATION_DEFINED"),nullptr)
 REGC(XmNselectColor, "XmNselectColor", AB_ARG_PIXEL,AB_ARG_CLASS_COLOR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNselectInsensitivePixmap, "XmNselectInsensitivePixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNselectPixmap, "XmNselectPixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNselectThreshold, "XmNselectThreshold", AB_ARG_INT,nullptr,(XtArgVal)5,nullptr)
 REG(XmNselectedItemCount, "XmNselectedItemCount", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNselectedItems, "XmNselectedItems", AB_ARG_XMSTRING_TBL,nullptr,0,nullptr)
 REG(XmNselectionArrayCount, "XmNselectionArrayCount", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNselectionLabelString, "XmNselectionLabelString", AB_ARG_XMSTRING,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNselectionPolicy, "XmNselectionPolicy", AB_ARG_LITERAL, &listsel_tbl,(XtArgVal)istr_const("XmBROWSE_SELECT"),nullptr)
 REG(XmNseparatorOn, "XmNseparatorOn", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNseparatorType, "XmNseparatorType", AB_ARG_LITERAL, &shadow_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNset, "XmNset", AB_ARG_BOOLEAN,AB_ARG_CLASS_VALUE,nullptr,(XtArgVal)False,nullptr)
 REGC(XmNshadowThickness, "XmNshadowThickness", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNshadowType, "XmNshadowType", AB_ARG_LITERAL, &dbshadow_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNshowArrows, "XmNshowArrows", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNshowAsDefault, "XmNshowAsDefault", AB_ARG_BOOLEAN,nullptr,(XtArgVal)0,nullptr)
 REG(XmNshowSeparator, "XmNshowSeparator", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNshowValue, "XmNshowValue", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNsimpleCallback, "XmNsimpleCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsingleSelectionCallback, "XmNsingleSelectionCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNskipAdjust, "XmNskipAdjust", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REGC(XmNsliderSize, "XmNsliderSize", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsource, "XmNsource", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsourceCursorIcon, "XmNsourceCursorIcon", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsourceIsExternal, "XmNsourceIsExternal", AB_ARG_BOOLEAN,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsourcePixmapIcon, "XmNsourcePixmapIcon", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsourceWidget, "XmNsourceWidget", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsourceWindow, "XmNsourceWindow", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNspacing, "XmNspacing", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNspotLocation, "XmNspotLocation", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNstartTime, "XmNstartTime", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNstateCursorIcon, "XmNstateCursorIcon", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNstringDirection, "XmNstringDirection", AB_ARG_LITERAL, &strdir_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsubMenuId, "XmNsubMenuId", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNsymbolPixmap, "XmNsymbolPixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtearOffMenuActivateCallback, "XmNtearOffMenuActivateCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtearOffMenuDeactivateCallback, "XmNtearOffMenuDeactivateCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtearOffModel, "XmNtearOffModel", AB_ARG_LITERAL, &tearoff_tbl,(XtArgVal)istr_const("XmTEAR_OFF_DISABLED"),nullptr)
 REGC(XmNtextAccelerators, "XmNtextAccelerators", AB_ARG_STRING,AB_ARG_CLASS_OTHER_STRING,nullptr,0,nullptr)
 REGC(XmNtextColumns, "XmNtextColumns", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtextFontList, "XmNtextFontList", AB_ARG_FONT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNtextString, "XmNtextString", AB_ARG_XMSTRING,AB_ARG_CLASS_VALUE,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNtextTranslations, "XmNtextTranslations", AB_ARG_STRING,AB_ARG_CLASS_OTHER_STRING,nullptr,0,nullptr)
 REGC(XmNtitleString, "XmNtitleString", AB_ARG_XMSTRING,AB_ARG_CLASS_NON_GLYPH_LABEL,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtoBottomCallback, "XmNtoBottomCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtoPositionCallback, "XmNtoPositionCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtoTopCallback, "XmNtoTopCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNtopAttachment, "XmNtopAttachment", AB_ARG_LITERAL, AB_ARG_CLASS_GEOMETRY,&attach_tbl,(XtArgVal)istr_const("XmATTACH_NONE"),nullptr)
 REG(XmNtopCharacter, "XmNtopCharacter", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNtopItemPosition, "XmNtopItemPosition", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNtopLevelEnterCallback, "XmNtopLevelEnterCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtopLevelLeaveCallback, "XmNtopLevelLeaveCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNtopOffset, "XmNtopOffset", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNtopPosition, "XmNtopPosition", AB_ARG_INT,AB_ARG_CLASS_GEOMETRY,nullptr,(XtArgVal)0,nullptr)
 REGC(XmNtopShadowColor, "XmNtopShadowColor", AB_ARG_PIXEL,AB_ARG_CLASS_COLOR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtopShadowPixmap, "XmNtopShadowPixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNtopWidget, "XmNtopWidget", AB_ARG_WIDGET,AB_ARG_CLASS_GEOMETRY,nullptr,0,nullptr)
 REG(XmNtransferProc, "XmNtransferProc", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtransferStatus, "XmNtransferStatus", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtraversalCallback, "XmNtraversalCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtraversalOn, "XmNtraversalOn", AB_ARG_BOOLEAN,nullptr,(XtArgVal)True,nullptr)
 REG(XmNtraverseObscuredCallback, "XmNtraverseObscuredCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNtreeUpdateProc, "XmNtreeUpdateProc", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNtroughColor, "XmNtroughColor", AB_ARG_PIXEL,AB_ARG_CLASS_COLOR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNunitType, "XmNunitType", AB_ARG_LITERAL, &unit_tbl,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNunmapCallback, "XmNunmapCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNunselectPixmap, "XmNunselectPixmap", AB_ARG_PIXMAP,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNuseAsyncGeometry, "XmNuseAsyncGeometry", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNuserData, "XmNuserData", AB_ARG_VOID_PTR,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNvalidCursorForeground, "XmNvalidCursorForeground", AB_ARG_INT,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNvalueChangedCallback, "XmNvalueChangedCallback", AB_ARG_CALLBACK,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REGC(XmNvalueWcs, "XmNvalueWcs", AB_ARG_UNDEF,AB_ARG_CLASS_VALUE,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNverifyBell, "XmNverifyBell", AB_ARG_BOOLEAN,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNverticalFontUnit, "XmNverticalFontUnit", AB_ARG_UNDEF,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNverticalScrollBar, "XmNverticalScrollBar", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNverticalSpacing, "XmNverticalSpacing", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNvisibleItemCount, "XmNvisibleItemCount", AB_ARG_INT,nullptr,(XtArgVal)0,nullptr)
 REG(XmNvisibleWhenOff, "XmNvisibleWhenOff", AB_ARG_BOOLEAN,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNvisualPolicy, "XmNvisualPolicy", AB_ARG_LITERAL, &swsize_tbl,(XtArgVal)istr_const("XmVARIABLE"),nullptr)
 REG(XmNwhichButton, "XmNwhichButton", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)
 REG(XmNwordWrap, "XmNwordWrap", AB_ARG_BOOLEAN,nullptr,(XtArgVal)False,nullptr)
 REG(XmNworkWindow, "XmNworkWindow", AB_ARG_WIDGET,nullptr,(XtArgVal)DEFAULT_UNSPEC,nullptr)

#undef XM_REG_RES
}


static int
xm_res_table_entry_xmname_compare(const void *leftEntry, const void *rightEntry)
{
    return 
	((long)xm_res_table[((TableIndexEntry)leftEntry)->rec_num].xmname)
	    - ((long)xm_res_table[((TableIndexEntry)rightEntry)->rec_num].xmname);
}


static int
build_xm_res_table_xmname_index(void)
{
    TableIndex	new_index = nullptr;
    int		new_index_size = xm_res_cnt * sizeof(TableIndexEntryRec);
    int		i = 0;

    /*
     * Build b-searchable index from ISTRING back to enum
     */

    new_index = (TableIndex)realloc(
				xm_res_table_xmname_index, new_index_size);
    if (new_index == nullptr)
    {
	return -1;
    }
    xm_res_table_xmname_index = new_index;
    memset(xm_res_table_xmname_index, 0, new_index_size);
    for (i = 0; i < xm_res_cnt; ++i)
    {
	xm_res_table_xmname_index[i].rec_num = i;
    }
    qsort(xm_res_table_xmname_index, 
		xm_res_cnt,
		sizeof(TableIndexEntryRec),
		xm_res_table_entry_xmname_compare);

    xm_res_table_indexes_dirty = FALSE;
    return 0;
}


/*
 * ASSUMES: index is up-to-date!
 */
static ObjxmRes *
find_res_entry_by_xmname(ISTRING istr_xmname)
{
    int        midDiff;
    int        midIndex;
    int	minIndex, maxIndex;
    ObjxmRes	*entry;

    /*
     * Perform binary search for ISTRING value
     */
    minIndex = 0;
    maxIndex = xm_res_cnt-1;
    while (minIndex < maxIndex)
    {
        midIndex = ((minIndex + maxIndex)>>1);
	entry = get_indexed_entry(
		    xm_res_table,xm_res_table_xmname_index,midIndex);
        midDiff = ((long)(entry->xmname)) - ((long)istr_xmname);
        if (midDiff < 0)
        {
            /* mid is too small - take upper half */
            minIndex = midIndex + 1;
	    continue;
        }
        else if (midDiff > 0)
        {
            /* mid is too large - take bottom half */
            maxIndex = midIndex - 1;
	    continue;
        }

	/* it's a perfect match! return this entry */
	goto epilogue;
    }
    
    /* 
     * After exiting loop, min=max 
     */
    entry = get_indexed_entry(xm_res_table,xm_res_table_xmname_index,minIndex);
    if (!istr_equal(istr_xmname, entry->xmname))
    {
	entry = nullptr;
    }

epilogue:
    return entry;
}


static ObjxmRes *
find_res_entry_by_strname(ISTRING strname)
{
    int		i;

    for (i=0; i < xm_res_cnt; i++)
        if (istr_equal(strname, xm_res_table[i].strname))
            return(&(xm_res_table[i]));

    return nullptr;
}


/* 
 * Load the enum table with known Motif/Xt enum definition/strings
 */ 
static void
load_enum_table(void)
{
#define XM_REG_ENUM(tbl_, i_, xmd_, strd_) \
              {  (tbl_).enums[i_].xmdef = (xmd_); \
                 (tbl_).enums[(i_)++].strdef = istr_const(strd_); }

#define ALLOC_ENUM_TBL(tbl_, n_) \
		{ (tbl_).count = (n_); \
		  (tbl_).enums = (ObjxmEnum*)calloc((n_), sizeof(ObjxmEnum)); }
    int n;

    n = 0;
    ALLOC_ENUM_TBL(cw_loc_tbl, 2)
    XM_REG_ENUM(cw_loc_tbl, n, XmCOMMAND_ABOVE_WORKSPACE, "XmCOMMAND_ABOVE_WORKSPACE")
    XM_REG_ENUM(cw_loc_tbl, n, XmCOMMAND_BELOW_WORKSPACE, "XmCOMMAND_BELOW_WORKSPACE")

    n = 0;
    ALLOC_ENUM_TBL(kbfocus_tbl, 2)
    XM_REG_ENUM(kbfocus_tbl, n, XmEXPLICIT, "XmEXPLICIT")
    XM_REG_ENUM(kbfocus_tbl, n, XmPOINTER, "XmPOINTER")

    n = 0;
    ALLOC_ENUM_TBL(navtype_tbl, 4)
    XM_REG_ENUM(navtype_tbl, n, XmNONE, "XmNONE")
    XM_REG_ENUM(navtype_tbl, n, XmTAB_GROUP, "XmTAB_GROUP")
    XM_REG_ENUM(navtype_tbl, n, XmSTICKY_TAB_GROUP, "XmSTICKY_TAB_GROUP")
    XM_REG_ENUM(navtype_tbl, n, XmEXCLUSIVE_TAB_GROUP, "XmEXCLUSIVE_TAB_GROUP")

    n = 0;
    ALLOC_ENUM_TBL(strdir_tbl, 3)
    XM_REG_ENUM(strdir_tbl, n, XmSTRING_DIRECTION_L_TO_R, "XmSTRING_DIRECTION_L_TO_R")
    XM_REG_ENUM(strdir_tbl, n, XmSTRING_DIRECTION_R_TO_L, "XmSTRING_DIRECTION_R_TO_L")
    XM_REG_ENUM(strdir_tbl, n, XmSTRING_DIRECTION_DEFAULT, "XmSTRING_DIRECTION_DEFAULT")
 
    n = 0;
    ALLOC_ENUM_TBL(tearoff_tbl, 2)
    XM_REG_ENUM(tearoff_tbl, n, XmTEAR_OFF_ENABLED, "XmTEAR_OFF_ENABLED")
    XM_REG_ENUM(tearoff_tbl, n, XmTEAR_OFF_DISABLED, "XmTEAR_OFF_DISABLED")

    n = 0;
    ALLOC_ENUM_TBL(texttype_tbl, 2)
    XM_REG_ENUM(texttype_tbl, n, XmSTRING, "XmSTRING")
    XM_REG_ENUM(texttype_tbl, n, DtNUMERIC, "DtNUMERIC")

    n = 0;
    ALLOC_ENUM_TBL(rctype_tbl, 5) 
    XM_REG_ENUM(rctype_tbl, n, XmWORK_AREA, "XmWORK_AREA")
    XM_REG_ENUM(rctype_tbl, n, XmMENU_BAR, "XmMENU_BAR")
    XM_REG_ENUM(rctype_tbl, n, XmMENU_PULLDOWN, "XmMENU_PULLDOWN")
    XM_REG_ENUM(rctype_tbl, n, XmMENU_POPUP, "XmMENU_POPUP")
    XM_REG_ENUM(rctype_tbl, n, XmMENU_OPTION, "XmMENU_OPTION")

    n = 0;
    ALLOC_ENUM_TBL(lbltype_tbl, 2) 
    XM_REG_ENUM(lbltype_tbl, n, XmPIXMAP, "XmPIXMAP")
    XM_REG_ENUM(lbltype_tbl, n, XmSTRING, "XmSTRING")

    n = 0;
    ALLOC_ENUM_TBL(dbshadow_tbl, 9) 
    XM_REG_ENUM(dbshadow_tbl, n, XmNO_LINE, "XmNO_LINE")
    XM_REG_ENUM(dbshadow_tbl, n, XmSINGLE_LINE, "XmSINGLE_LINE")
    XM_REG_ENUM(dbshadow_tbl, n, XmDOUBLE_LINE, "XmDOUBLE_LINE")
    XM_REG_ENUM(dbshadow_tbl, n, XmSINGLE_DASHED_LINE, "XmSINGLE_DASHED_LINE")
    XM_REG_ENUM(dbshadow_tbl, n, XmDOUBLE_DASHED_LINE, "XmDOUBLE_DASHED_LINE")
    XM_REG_ENUM(dbshadow_tbl, n, XmSHADOW_ETCHED_IN, "XmSHADOW_ETCHED_IN")
    XM_REG_ENUM(dbshadow_tbl, n, XmSHADOW_ETCHED_OUT, "XmSHADOW_ETCHED_OUT")
    XM_REG_ENUM(dbshadow_tbl, n, XmSHADOW_IN, "XmSHADOW_IN")
    XM_REG_ENUM(dbshadow_tbl, n, XmSHADOW_OUT, "XmSHADOW_OUT")

    n = 0;
    ALLOC_ENUM_TBL(shadow_tbl, 9)
    XM_REG_ENUM(shadow_tbl, n, XmNO_LINE, "XmNO_LINE")
    XM_REG_ENUM(shadow_tbl, n, XmSINGLE_LINE, "XmSINGLE_LINE")
    XM_REG_ENUM(shadow_tbl, n, XmDOUBLE_LINE, "XmDOUBLE_LINE")
    XM_REG_ENUM(shadow_tbl, n, XmSINGLE_DASHED_LINE, "XmSINGLE_DASHED_LINE")
    XM_REG_ENUM(shadow_tbl, n, XmDOUBLE_DASHED_LINE, "XmDOUBLE_DASHED_LINE")
    XM_REG_ENUM(shadow_tbl, n, XmSHADOW_ETCHED_IN, "XmSHADOW_ETCHED_IN")
    XM_REG_ENUM(shadow_tbl, n, XmSHADOW_ETCHED_OUT, "XmSHADOW_ETCHED_OUT")
    XM_REG_ENUM(shadow_tbl, n, XmSHADOW_ETCHED_IN_DASH, "XmSHADOW_ETCHED_IN_DASH")
    XM_REG_ENUM(shadow_tbl, n, XmSHADOW_ETCHED_OUT_DASH, "XmSHADOW_ETCHED_OUT_DASH")

    n = 0;
    ALLOC_ENUM_TBL(resize_tbl, 3) 
    XM_REG_ENUM(resize_tbl, n, XmRESIZE_NONE, "XmRESIZE_NONE")
    XM_REG_ENUM(resize_tbl, n, XmRESIZE_GROW, "XmRESIZE_GROW")
    XM_REG_ENUM(resize_tbl, n, XmRESIZE_ANY, "XmRESIZE_ANY")

    n = 0;
    ALLOC_ENUM_TBL(orient_tbl, 2)
    XM_REG_ENUM(orient_tbl, n, XmVERTICAL, "XmVERTICAL")
    XM_REG_ENUM(orient_tbl, n, XmHORIZONTAL, "XmHORIZONTAL")

    n = 0;
    ALLOC_ENUM_TBL(listsel_tbl, 4)
    XM_REG_ENUM(listsel_tbl, n, XmSINGLE_SELECT, "XmSINGLE_SELECT")
    XM_REG_ENUM(listsel_tbl, n, XmMULTIPLE_SELECT, "XmMULTIPLE_SELECT")
    XM_REG_ENUM(listsel_tbl, n, XmEXTENDED_SELECT, "XmEXTENDED_SELECT")
    XM_REG_ENUM(listsel_tbl, n, XmBROWSE_SELECT, "XmBROWSE_SELECT")

    n = 0;
    ALLOC_ENUM_TBL(editmode_tbl, 2)
    XM_REG_ENUM(editmode_tbl, n, XmMULTI_LINE_EDIT, "XmMULTI_LINE_EDIT")
    XM_REG_ENUM(editmode_tbl, n, XmSINGLE_LINE_EDIT, "XmSINGLE_LINE_EDIT")
 
    n = 0;
    ALLOC_ENUM_TBL(istate_tbl, 2)
    XM_REG_ENUM(istate_tbl, n, NormalState, "NormalState")
    XM_REG_ENUM(istate_tbl, n, IconicState, "IconicState")

    n = 0;
    ALLOC_ENUM_TBL(swsize_tbl, 3)
    XM_REG_ENUM(swsize_tbl, n, XmVARIABLE, "XmVARIABLE")
    XM_REG_ENUM(swsize_tbl, n, XmCONSTANT, "XmCONSTANT")
    XM_REG_ENUM(swsize_tbl, n, XmRESIZE_IF_POSSIBLE, "XmRESIZE_IF_POSSIBLE")

    n = 0;
    ALLOC_ENUM_TBL(align_tbl, 5)
    XM_REG_ENUM(align_tbl, n, XmALIGNMENT_BASELINE_BOTTOM, "XmALIGNMENT_BASELINE_BOTTOM")
    XM_REG_ENUM(align_tbl, n, XmALIGNMENT_BASELINE_TOP, "XmALIGNMENT_BASELINE_TOP")
    XM_REG_ENUM(align_tbl, n, XmALIGNMENT_CONTENTS_BOTTOM, "XmALIGNMENT_CONTENTS_BOTTOM")
    XM_REG_ENUM(align_tbl, n, XmALIGNMENT_CONTENTS_TOP, "XmALIGNMENT_CONTENTS_TOP")
    XM_REG_ENUM(align_tbl, n, XmALIGNMENT_CENTER, "XmALIGNMENT_CENTER")

    n = 0;
    ALLOC_ENUM_TBL(pack_tbl, 3)
    XM_REG_ENUM(pack_tbl, n, XmPACK_COLUMN, "XmPACK_COLUMN")
    XM_REG_ENUM(pack_tbl, n, XmPACK_NONE, "XmPACK_NONE")
    XM_REG_ENUM(pack_tbl, n, XmPACK_TIGHT, "XmPACK_TIGHT")

    n = 0;
    ALLOC_ENUM_TBL(unit_tbl, 5)
    XM_REG_ENUM(unit_tbl, n, XmPIXELS, "XmPIXELS")
    XM_REG_ENUM(unit_tbl, n, Xm100TH_MILLIMETERS, "Xm100TH_MILLIMETERS")
    XM_REG_ENUM(unit_tbl, n, Xm1000TH_INCHES, "Xm1000TH_INCHES")
    XM_REG_ENUM(unit_tbl, n, Xm100TH_POINTS, "Xm100TH_POINTS")
    XM_REG_ENUM(unit_tbl, n, Xm100TH_FONT_UNITS, "Xm100TH_FONT_UNITS")

    n = 0;
    ALLOC_ENUM_TBL(delresp_tbl, 3)
    XM_REG_ENUM(delresp_tbl, n, XmDESTROY, "XmDESTROY")
    XM_REG_ENUM(delresp_tbl, n, XmUNMAP, "XmUNMAP")
    XM_REG_ENUM(delresp_tbl, n, XmDO_NOTHING, "XmDO_NOTHING")

    n = 0;
    ALLOC_ENUM_TBL(lblalign_tbl, 3)
    XM_REG_ENUM(lblalign_tbl, n, XmALIGNMENT_BEGINNING, "XmALIGNMENT_BEGINNING")
    XM_REG_ENUM(lblalign_tbl, n, XmALIGNMENT_CENTER, "XmALIGNMENT_CENTER")
    XM_REG_ENUM(lblalign_tbl, n, XmALIGNMENT_END, "XmALIGNMENT_END")

    n = 0; 
    ALLOC_ENUM_TBL(tbtype_tbl, 2)
    XM_REG_ENUM(tbtype_tbl, n, XmN_OF_MANY, "XmN_OF_MANY")
    XM_REG_ENUM(tbtype_tbl, n, XmONE_OF_MANY, "XmONE_OF_MANY")

    n = 0; 
    ALLOC_ENUM_TBL(attach_tbl, 7)
    XM_REG_ENUM(attach_tbl, n, XmATTACH_NONE, "XmATTACH_NONE")
    XM_REG_ENUM(attach_tbl, n, XmATTACH_FORM, "XmATTACH_FORM")
    XM_REG_ENUM(attach_tbl, n, XmATTACH_OPPOSITE_FORM, "XmATTACH_OPPOSITE_FORM")
    XM_REG_ENUM(attach_tbl, n, XmATTACH_OPPOSITE_WIDGET, "XmATTACH_OPPOSITE_WIDGET")
    XM_REG_ENUM(attach_tbl, n, XmATTACH_WIDGET, "XmATTACH_WIDGET")
    XM_REG_ENUM(attach_tbl, n, XmATTACH_POSITION, "XmATTACH_POSITION")
    XM_REG_ENUM(attach_tbl, n, XmATTACH_SELF, "XmATTACH_SELF")

    n = 0; 
    ALLOC_ENUM_TBL(arrow_tbl, 4)
    XM_REG_ENUM(arrow_tbl, n, XmARROW_UP, "XmARROW_UP")
    XM_REG_ENUM(arrow_tbl, n, XmARROW_DOWN, "XmARROW_DOWN")
    XM_REG_ENUM(arrow_tbl, n, XmARROW_LEFT, "XmARROW_LEFT")
    XM_REG_ENUM(arrow_tbl, n, XmARROW_RIGHT, "XmARROW_RIGHT")

    n = 0;
    ALLOC_ENUM_TBL(sb_tbl, 4)
    XM_REG_ENUM(sb_tbl, n, XmMAX_ON_TOP, "XmMAX_ON_TOP")
    XM_REG_ENUM(sb_tbl, n, XmMAX_ON_BOTTOM, "XmMAX_ON_BOTTOM")
    XM_REG_ENUM(sb_tbl, n, XmMAX_ON_LEFT, "XmMAX_ON_LEFT")
    XM_REG_ENUM(sb_tbl, n, XmMAX_ON_RIGHT, "XmMAX_ON_RIGHT")

    n = 0;
    ALLOC_ENUM_TBL(sbplace_tbl, 4)
    XM_REG_ENUM(sbplace_tbl, n, XmTOP_LEFT, "XmTOP_LEFT")
    XM_REG_ENUM(sbplace_tbl, n, XmBOTTOM_LEFT, "XmBOTTOM_LEFT")
    XM_REG_ENUM(sbplace_tbl, n, XmBOTTOM_RIGHT, "XmBOTTOM_RIGHT")
    XM_REG_ENUM(sbplace_tbl, n, XmTOP_RIGHT, "XmTOP_RIGHT")

    n = 0;
    ALLOC_ENUM_TBL(list_tbl, 2)
    XM_REG_ENUM(list_tbl, n, XmSTATIC, "XmSTATIC")
    XM_REG_ENUM(list_tbl, n, XmDYNAMIC, "XmDYNAMIC")

    n = 0;
    ALLOC_ENUM_TBL(sbscroll_tbl, 4)
    XM_REG_ENUM(sbscroll_tbl, n, XmAUTOMATIC, "XmAUTOMATIC")
    XM_REG_ENUM(sbscroll_tbl, n, XmSTATIC, "XmSTATIC")
    XM_REG_ENUM(sbscroll_tbl, n, XmAPPLICATION_DEFINED, "XmAPPLICATION_DEFINED")
    XM_REG_ENUM(sbscroll_tbl, n, XmAS_NEEDED, "XmAS_NEEDED")
 
    n = 0;
    ALLOC_ENUM_TBL(dialog_tbl, 7)
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_TEMPLATE, "XmDIALOG_TEMPLATE")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_ERROR, "XmDIALOG_ERROR")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_INFORMATION, "XmDIALOG_INFORMATION")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_MESSAGE, "XmDIALOG_MESSAGE")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_QUESTION, "XmDIALOG_QUESTION")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_WARNING, "XmDIALOG_WARNING")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_WORKING, "XmDIALOG_WORKING")

    n = 0;
    ALLOC_ENUM_TBL(dlgstyle_tbl, 6)
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_SYSTEM_MODAL, "XmDIALOG_SYSTEM_MODAL")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_PRIMARY_APPLICATION_MODAL, "XmDIALOG_PRIMARY_APPLICATION_MODAL")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_APPLICATION_MODAL, "XmDIALOG_APPLICATION_MODAL")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_FULL_APPLICATION_MODAL, "XmDIALOG_FULL_APPLICATION_MODAL")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_MODELESS, "XmDIALOG_MODELESS")
    XM_REG_ENUM(dialog_tbl, n, XmDIALOG_WORK_AREA, "XmDIALOG_WORK_AREA")

    n = 0;
    ALLOC_ENUM_TBL(animate_tbl, 5)
    XM_REG_ENUM(animate_tbl, n, XmDRAG_UNDER_NONE, "XmDRAG_UNDER_NONE")
    XM_REG_ENUM(animate_tbl, n, XmDRAG_UNDER_PIXMAP, "XmDRAG_UNDER_PIXMAP")
    XM_REG_ENUM(animate_tbl, n, XmDRAG_UNDER_SHADOW_IN, "XmDRAG_UNDER_SHADOW_IN")
    XM_REG_ENUM(animate_tbl, n, XmDRAG_UNDER_SHADOW_OUT, "XmDRAG_UNDER_SHADOW_OUT")
    XM_REG_ENUM(animate_tbl, n, XmDRAG_UNDER_HIGHLIGHT, "XmDRAG_UNDER_HIGHLIGHT")

    n = 0;
    ALLOC_ENUM_TBL(dragp_tbl, 7)
    XM_REG_ENUM(dragp_tbl, n, XmDRAG_NONE, "XmDRAG_NONE")
    XM_REG_ENUM(dragp_tbl, n, XmDRAG_DROP_ONLY, "XmDRAG_DROP_ONLY")
    XM_REG_ENUM(dragp_tbl, n, XmDRAG_PREFER_PREREGISTER, "XmDRAG_PREFER_PREREGISTER")
    XM_REG_ENUM(dragp_tbl, n, XmDRAG_PREREGISTER, "XmDRAG_PREREGISTER")
    XM_REG_ENUM(dragp_tbl, n, XmDRAG_PREFER_DYNAMIC, "XmDRAG_PREFER_DYNAMIC")
    XM_REG_ENUM(dragp_tbl, n, XmDRAG_DYNAMIC, "XmDRAG_DYNAMIC")
    XM_REG_ENUM(dragp_tbl, n, XmDRAG_PREFER_RECEIVER, "XmDRAG_PREFER_RECEIVER")

    n = 0;
    ALLOC_ENUM_TBL(dragover_tbl, 3)
    XM_REG_ENUM(dragover_tbl, n, XmWINDOW, "XmWINDOW")
    XM_REG_ENUM(dragover_tbl, n, XmPIXMAP, "XmPIXMAP")
    XM_REG_ENUM(dragover_tbl, n, XmCURSOR, "XmCURSOR")

    n = 0;
    ALLOC_ENUM_TBL(dropsite_tbl, 4)
    XM_REG_ENUM(dropsite_tbl, n, XmDROP_SITE_SIMPLE, "XmDROP_SITE_SIMPLE")
    XM_REG_ENUM(dropsite_tbl, n, XmDROP_SITE_COMPOSITE, "XmDROP_SITE_COMPOSITE")
    XM_REG_ENUM(dropsite_tbl, n, XmDROP_SITE_SIMPLE_CLIP_ONLY, "XmDROP_SITE_SIMPLE_CLIP_ONLY")
    XM_REG_ENUM(dropsite_tbl, n, XmDROP_SITE_COMPOSITE_CLIP_ONLY, "XmDROP_SITE_COMPOSITE_CLIP_ONLY")

    n = 0;
    ALLOC_ENUM_TBL(arrow_layout_tbl, 5)
    XM_REG_ENUM(arrow_layout_tbl, n, DtARROWS_FLAT_BEGINNING, "DtARROWS_FLAT_BEGINNING")
    XM_REG_ENUM(arrow_layout_tbl, n, DtARROWS_FLAT_END, "DtARROWS_FLAT_END")
    XM_REG_ENUM(arrow_layout_tbl, n, DtARROWS_BEGINNING, "DtARROWS_BEGINNING")
    XM_REG_ENUM(arrow_layout_tbl, n, DtARROWS_END, "DtARROWS_END")
    XM_REG_ENUM(arrow_layout_tbl, n, DtARROWS_SPLIT, "DtARROWS_SPLIT")

    n = 0;
    ALLOC_ENUM_TBL(audible_tbl, 2)
    XM_REG_ENUM(audible_tbl, n, XmBELL, "XmBELL")
    XM_REG_ENUM(audible_tbl, n, XmNONE, "XmNONE")

    n = 0;
    ALLOC_ENUM_TBL(blend_tbl, 4)
    XM_REG_ENUM(blend_tbl, n, XmBLEND_ALL, "XmBLEND_ALL")
    XM_REG_ENUM(blend_tbl, n, XmBLEND_STATE_SOURCE, "XmBLEND_STATE_SOURCE")
    XM_REG_ENUM(blend_tbl, n, XmBLEND_JUST_SOURCE, "XmBLEND_JUST_SOURCE")
    XM_REG_ENUM(blend_tbl, n, XmBLEND_NONE, "XmBLEND_NONE")

    n = 0;
    ALLOC_ENUM_TBL(combobox_type_tbl, 2)
    XM_REG_ENUM(combobox_type_tbl, n, DtDROP_DOWN_COMBO_BOX, "DtDROP_DOWN_COMBO_BOX")
    XM_REG_ENUM(combobox_type_tbl, n, DtDROP_DOWN_LIST, "DtDROP_DOWN_LIST")

    n = 0;
    ALLOC_ENUM_TBL(defbutton_tbl, 17)
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_NONE, "XmDIALOG_NONE")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_APPLY_BUTTON, "XmDIALOG_APPLY_BUTTON")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_CANCEL_BUTTON, "XmDIALOG_CANCEL_BUTTON")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_DEFAULT_BUTTON, "XmDIALOG_DEFAULT_BUTTON")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_OK_BUTTON, "XmDIALOG_OK_BUTTON")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_FILTER_LABEL, "XmDIALOG_FILTER_LABEL")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_FILTER_TEXT, "XmDIALOG_FILTER_TEXT")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_HELP_BUTTON, "XmDIALOG_HELP_BUTTON")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_LIST, "XmDIALOG_LIST")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_LIST_LABEL, "XmDIALOG_LIST_LABEL")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_MESSAGE_LABEL, "XmDIALOG_MESSAGE_LABEL")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_SELECTION_LABEL, "XmDIALOG_SELECTION_LABEL")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_SYMBOL_LABEL, "XmDIALOG_SYMBOL_LABEL")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_DIR_LIST_LABEL, "XmDIALOG_DIR_LIST_LABEL")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_TEXT, "XmDIALOG_TEXT")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_SEPARATOR, "XmDIALOG_SEPARATOR")
    XM_REG_ENUM(defbutton_tbl, n, XmDIALOG_DIR_LIST, "XmDIALOG_DIR_LIST")

    n = 0;
    ALLOC_ENUM_TBL(file_mask_tbl, 3)
    XM_REG_ENUM(file_mask_tbl, n, XmFILE_REGULAR, "XmFILE_REGULAR")
    XM_REG_ENUM(file_mask_tbl, n, XmFILE_DIRECTORY, "XmFILE_DIRECTORY")
    XM_REG_ENUM(file_mask_tbl, n, XmFILE_ANY_TYPE, "XmFILE_ANY_TYPE")

#undef  XM_REG_ENUM
#undef  ALLOC_ENUM_TABLE
 
}


/*
 * Load the xmcreate table with known Motif/Xt create routines/strings
 */
static void
load_xmcreate_table(void)
{
#define XM_REG_XMCREATE(i_, ftn_, fstr_) \
              {  xm_xmcreate_table[i_].ftn = (ftn_); \
                 xm_xmcreate_table[(i_)++].ftnstr = istr_const(fstr_); }

    int n = 0;

    XM_REG_XMCREATE(n, XmCreateArrowButton, "XmCreateArrowButton")
    XM_REG_XMCREATE(n, XmCreateArrowButtonGadget, "XmCreateArrowButtonGadget")
    XM_REG_XMCREATE(n, XmCreateBulletinBoard, "XmCreateBulletinBoard")
    XM_REG_XMCREATE(n, XmCreateBulletinBoardDialog, "XmCreateBulletinBoardDialog")
    XM_REG_XMCREATE(n, XmCreateCascadeButton, "XmCreateCascadeButton")
    XM_REG_XMCREATE(n, XmCreateCascadeButtonGadget, "XmCreateCascadeButtonGadget")
    XM_REG_XMCREATE(n, XmCreateCommand, "XmCreateCommand")
    XM_REG_XMCREATE(n, XmCreateDialogShell, "XmCreateDialogShell")
    XM_REG_XMCREATE(n, XmCreateDragIcon, "XmCreateDragIcon")
    XM_REG_XMCREATE(n, XmCreateDrawingArea, "XmCreateDrawingArea")
    XM_REG_XMCREATE(n, XmCreateDrawnButton, "XmCreateDrawnButton")
    XM_REG_XMCREATE(n, XmCreateErrorDialog, "XmCreateErrorDialog")
    XM_REG_XMCREATE(n, XmCreateFileSelectionBox, "XmCreateFileSelectionBox")
    XM_REG_XMCREATE(n, XmCreateFileSelectionDialog, "XmCreateFileSelectionDialog")
    XM_REG_XMCREATE(n, XmCreateForm, "XmCreateForm")
    XM_REG_XMCREATE(n, XmCreateFormDialog, "XmCreateFormDialog")
    XM_REG_XMCREATE(n, XmCreateFrame, "XmCreateFrame")
    XM_REG_XMCREATE(n, XmCreateInformationDialog, "XmCreateInformationDialog")
    XM_REG_XMCREATE(n, XmCreateLabel, "XmCreateLabel")
    XM_REG_XMCREATE(n, XmCreateLabelGadget, "XmCreateLabelGadget")
    XM_REG_XMCREATE(n, XmCreateList, "XmCreateList")
    XM_REG_XMCREATE(n, XmCreateMainWindow, "XmCreateMainWindow")
    XM_REG_XMCREATE(n, XmCreateMenuBar, "XmCreateMenuBar")
    XM_REG_XMCREATE(n, XmCreateMenuShell, "XmCreateMenuShell")
    XM_REG_XMCREATE(n, XmCreateMessageBox, "XmCreateMessageBox")
    XM_REG_XMCREATE(n, XmCreateMessageDialog, "XmCreateMessageDialog")
    XM_REG_XMCREATE(n, XmCreateOptionMenu, "XmCreateOptionMenu")
    XM_REG_XMCREATE(n, XmCreatePanedWindow, "XmCreatePanedWindow")
    XM_REG_XMCREATE(n, XmCreatePopupMenu, "XmCreatePopupMenu")
    XM_REG_XMCREATE(n, XmCreatePromptDialog, "XmCreatePromptDialog")
    XM_REG_XMCREATE(n, XmCreatePulldownMenu, "XmCreatePulldownMenu")
    XM_REG_XMCREATE(n, XmCreatePushButton, "XmCreatePushButton")
    XM_REG_XMCREATE(n, XmCreatePushButtonGadget, "XmCreatePushButtonGadget")
    XM_REG_XMCREATE(n, XmCreateQuestionDialog, "XmCreateQuestionDialog")
    XM_REG_XMCREATE(n, XmCreateRadioBox, "XmCreateRadioBox")
    XM_REG_XMCREATE(n, XmCreateRowColumn, "XmCreateRowColumn")
    XM_REG_XMCREATE(n, XmCreateScale, "XmCreateScale")
    XM_REG_XMCREATE(n, XmCreateScrollBar, "XmCreateScrollBar")
    XM_REG_XMCREATE(n, XmCreateScrolledList, "XmCreateScrolledList")
    XM_REG_XMCREATE(n, XmCreateScrolledText, "XmCreateScrolledText")
    XM_REG_XMCREATE(n, XmCreateScrolledWindow, "XmCreateScrolledWindow")
    XM_REG_XMCREATE(n, XmCreateSelectionBox, "XmCreateSelectionBox")
    XM_REG_XMCREATE(n, XmCreateSelectionDialog, "XmCreateSelectionDialog")
    XM_REG_XMCREATE(n, XmCreateSeparator, "XmCreateSeparator")
    XM_REG_XMCREATE(n, XmCreateSeparatorGadget, "XmCreateSeparatorGadget")
    XM_REG_XMCREATE(n, XmCreateSimpleCheckBox, "XmCreateSimpleCheckBox")
    XM_REG_XMCREATE(n, XmCreateSimpleMenuBar, "XmCreateSimpleMenuBar")
    XM_REG_XMCREATE(n, XmCreateSimpleOptionMenu, "XmCreateSimpleOptionMenu")
    XM_REG_XMCREATE(n, XmCreateSimplePopupMenu, "XmCreateSimplePopupMenu")
    XM_REG_XMCREATE(n, XmCreateSimplePulldownMenu, "XmCreateSimplePulldownMenu")
    XM_REG_XMCREATE(n, XmCreateSimpleRadioBox, "XmCreateSimpleRadioBox")
    XM_REG_XMCREATE(n, XmCreateTemplateDialog, "XmCreateTemplateDialog")
    XM_REG_XMCREATE(n, XmCreateText, "XmCreateText")
    XM_REG_XMCREATE(n, XmCreateTextField, "XmCreateTextField")
    XM_REG_XMCREATE(n, XmCreateToggleButton, "XmCreateToggleButton")
    XM_REG_XMCREATE(n, XmCreateToggleButtonGadget, "XmCreateToggleButtonGadget")
    XM_REG_XMCREATE(n, XmCreateWarningDialog, "XmCreateWarningDialog")
    XM_REG_XMCREATE(n, XmCreateWorkArea, "XmCreateWorkArea")
    XM_REG_XMCREATE(n, XmCreateWorkingDialog, "XmCreateWorkingDialog")

    assert(n <= XM_MAX_CREATE_FUNCS);
    xm_xmcreate_cnt = n;

#undef  XM_REG_XMCREATE

}

static XtArgVal
resize_default(
    WidgetClass	wclass
)
{
    XtArgVal	default_val = DEFAULT_UNSPEC;

    if (wclass == xmBulletinBoardWidgetClass ||
	wclass == xmDrawingAreaWidgetClass ||
	wclass == xmFormWidgetClass)
	default_val = (XtArgVal)XmRESIZE_ANY;
    else if (wclass == xmCommandWidgetClass)
	default_val = (XtArgVal)XmRESIZE_NONE;
	
    return default_val;
}
