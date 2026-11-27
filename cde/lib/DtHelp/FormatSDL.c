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
/* $TOG: FormatSDL.c /main/18 1999/10/14 13:18:42 mgreess $ */
/************************************<+>*************************************
 ****************************************************************************
 **
 **   File:	   FormatSDL.c
 **
 **   Project:     CDE Help System
 **
 **   Description: This code formats information in an SDL volume into
 **		   an into internal format.
 **
 **  (c) Copyright 1987, 1988, 1989, 1990, 1991, 1992 Hewlett-Packard Company
 **
 **  (c) Copyright 1993, 1994 Hewlett-Packard Company
 **  (c) Copyright 1993, 1994 International Business Machines Corp.
 **  (c) Copyright 1993, 1994 Sun Microsystems, Inc.
 **  (c) Copyright 1993, 1994 Novell, Inc.
 **
 ****************************************************************************
 ************************************<+>*************************************/

/*
 * system includes
 */
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

/*
 * Core Engine includes
 */
#include "CanvasP.h"
#include "CanvasSegP.h"
#include "LinkMgrP.h"

/*
 * private includes
 */
#include "bufioI.h"
#include "CanvasOsI.h"
#include "CleanUpI.h"
#include "CvStringI.h"
#include "CvtToArrayP.h"
#include "FontAttrI.h"
#include "Access.h"
#include "AccessI.h"
#include "AccessSDLP.h"
#include "AccessSDLI.h"
#include "FormatUtilI.h"
#include "FormatSDLI.h"
#include "StringFuncsI.h"
#include "SDLI.h"
#include "UtilSDLI.h"
#include "RegionI.h"

#ifdef NLS16
#endif

/******************************************************************************
 * Private structures
 ******************************************************************************/
typedef	struct	_snbLinkInfo {
	SDLCdata	xid;		/* also used for data & command */
	SDLCdata	format;
	SDLCdata	method;
	SDLNumber	offset;
} SnbLinkInfo;

typedef struct  _elementInfo {
        unsigned long           enum_values;
        unsigned long           num_values;
        unsigned long           str1_values;
        unsigned long           str2_values;

        enum SdlElement el_type;        /* the current element     */
        SdlOption	timing;         /* the current timing type */
        SdlOption	sdl_type;       /* dynamic, lined, literal, etc */
        SdlOption	window;
        SdlOption	interp;
        union
          {
            SDLNumber   count;
            SDLNumber   offset;
          } nums;
        SDLCdata        language;
        SDLCdata        char_set;
        SDLCdata        id;
        SDLCdata        elstr1;
        SDLCdata        elstr2;
        union
          {
            SDLDocInfo  doc_info;       /* <sdldoc> */
            SDLIdInfo   id_info;        /* <id>     */
            SnbLinkInfo snb_info;       /* <callback>, <crossdoc>,
                                           <man-page>, <textfile>, <sys-cmd> */
            _DtCvSegment   *table_info;    /* <form>   */
	    SDLEntryInfo entry_info;	/* <entry */
          } w;
	SdlMatchData	match;
} ElementInfo, *ElementInfoPtr;

typedef	struct _unresSnref {
	int		 cur_link;
	char		*id;
	_DtCvSegment	*seg;
	_DtHelpFontHints fonts;
} UnresSnref;

typedef struct _formatStruct {
        _DtCvValue       end_flag;
        _DtCvValue       last_was_space;
        _DtCvValue       last_was_mb;
        _DtCvValue       last_was_nl;
        _DtCvValue       snref_used;
        _DtCvValue       faked_end;
        _DtCvValue       save_snref;
        _DtCvValue       save_blank;
        _SdlFontMode     resolve_font;
        enum SdlElement  parsed;
        char            *remember;
        char            *vol_name;
        char            *id_string;
        int              cur_link;
        int              mb_len;
        int              flags;
	int		 malloc_size;
	int		 alloc_size;
	int		 free_cnt;
	int		 free_max;
	int		 snref_cnt;
	const _FrmtUiInfo *ui_info;
	_DtCvSegment	*block_list;
        _DtCvSegment       *add_seg;
        _DtCvSegment       *last_seg;
        _DtCvSegment       *seg_list;
        _DtCvSegment       *prev_data;
        _DtCvSegment       *toss;
        _DtCvSegment       *async_blks;
        _DtCvSegment       *snb;
        _DtCvSegment       *else_prev;
        _DtCvSegment       *then_prev;
        _DtCvContainer	*active_frmt;
        _DtCvLinkDb	 my_links;
	ElementInfo	 el_info;
	_DtHelpFontHints	*my_fonts;
	_DtHelpFontHints	**free_fonts;
	UnresSnref		*un_snrefs;
        BufFilePtr       my_file;
} FormatStruct;

/******************************************************************************
 * Private Function Declarations
 ******************************************************************************/
static	int	AddRowToTable(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	AllocateBlock(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	Cdata(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	CheckForSnb(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	CheckType(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	CleanUpBlock(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	ClearAndCheckSnref(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	ColInfoToTableInfo(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	CopyAnchorId(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	CopyDocInfo(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	CopyIdInfo(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	ResolveSpcInfo(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	CopyEntryInfo(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	CopyTossInfo(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	FakeEnd(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	_DtCvSegment  *FindSnbEntry(
			_DtCvSegment	 *snb,
			char		 *target);
static	char	*GetInterpCmd(SdlOption	interp_type);
static	int	IfInfo(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	InitLast(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	LoadGraphic(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	MarkFound(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	OnlyOne(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	OnlyOneEach(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	OneToN(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	ParseSDL(
			FormatStruct	*my_struct,
			enum SdlElement	 cur_element,
			enum SdlElement	 sig_element,
			SDLMask		*cur_except,
			SDLMask		*process_mask);
static	int	ProcessEnterAttr(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*cur_except,
			SDLMask		*process_mask);
static	int	ProcessExitAttr(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*cur_except,
			SDLMask		*process_mask);
static	int	ProcessSDLMarkup(
			FormatStruct	*my_struct,
			enum SdlElement	 cur_element,
			enum SdlElement	 sig_element,
			SDLMask		*cur_except,
			SDLMask		*process_mask);
static	int	RegisterLink(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	RegisterSnbLink(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	RegisterSwitch(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	ResolveIf(
			FormatStruct	*my_struct,
			_DtCvSegment	*prev_data);
static	int	SaveItemInfo(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	SaveLangCharSet(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	SetSaveSnref(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	SetTransit(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	SetType(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	ZeroOrOne(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);
static	int	ZeroToN(
			FormatStruct	*my_struct,
			SDLMask		*element_types,
			enum SdlElement	 sig_element,
			SDLMask		*exceptions,
			SDLMask		*process_mask);

/********    End Public Function Declarations    ********/

/******************************************************************************
 * Private Defines
 *****************************************************************************/
#define	GROW_SIZE		5
#define	MAX_ATTRIBUTE_LENGTH	30

/******************************************************************************
 * Private Macros
 *****************************************************************************/
#define	DefPercent	10000

#define	ElCharSet(x)	((x)->el_info.char_set)
#define	ElClan(x)	((x)->el_info.match.clan)
#define	ElCount(x)	((x)->el_info.nums.count)
#define	ElFlag1(x)	((x)->el_info.enum_values)
#define	ElFlag2(x)	((x)->el_info.num_values)
#define	ElFlag3(x)	((x)->el_info.str1_values)
#define	ElFlag4(x)	((x)->el_info.str2_values)
#define	ElFrmtType(x)	((x)->el_info.sdl_type)
#define	ElId(x)		((x)->el_info.id)
#define	ElInterp(x)	((x)->el_info.interp)
#define	ElLanguage(x)	((x)->el_info.language)
#define	ElLevel(x)	((x)->el_info.match.level)
#define	ElOffset(x)	((x)->el_info.nums.offset)
#define	ElSsi(x)	((x)->el_info.match.ssi)
#define	ElTiming(x)	((x)->el_info.timing)
#define	ElType(x)	((x)->el_info.el_type)
#define	ElWindow(x)	((x)->el_info.window)

#define	ElInfoClan(x)	((x)->match.clan)

#define	ElDocInfo(x)	((x)->el_info.w.doc_info)
#define	ElEntryInfo(x)	((x)->el_info.w.entry_info)
#define	ElIdInfo(x)	((x)->el_info.w.id_info)
#define	ElSnbXid(x)	((x)->el_info.w.snb_info.xid)
#define	ElSnbFormat(x)	((x)->el_info.w.snb_info.format)
#define	ElSnbMethod(x)	((x)->el_info.w.snb_info.method)
#define	ElSwitchBranches(x)	((x)->el_info.elstr1)

#define	ElTable(x)		((x)->el_info.w.table_info)
#define	ElTableCellIds(x) \
			_DtCvCellIdsOfTableSeg(((x)->el_info.w.table_info))
#define	ElTableColNum(x) \
			_DtCvNumColsOfTableSeg(((x)->el_info.w.table_info))
#define	ElTableColWidths(x) \
			_DtCvColWOfTableSeg(((x)->el_info.w.table_info))
#define	ElTableColJust(x) \
			_DtCvColJustifyOfTableSeg(((x)->el_info.w.table_info))
#define	ElTableList(x) \
			_DtCvCellsOfTableSeg(((x)->el_info.w.table_info))

#define	ElString1(x)		((x)->el_info.elstr1)
#define	ElString2(x)		((x)->el_info.elstr2)

#define	ElAbbrev(x)		((x)->el_info.elstr1)

#define	ElEnter(x)		((x)->el_info.elstr1)
#define	ElExit(x)		((x)->el_info.elstr2)

#define	ElSpcName(x)		((x)->el_info.elstr1)
#define	ElTableCellId(x)	((x)->el_info.elstr1)

#define	ElTableColJStr(x)	((x)->el_info.elstr1)
#define	ElTableColWStr(x)	((x)->el_info.elstr2)

#define	SnbOffset(x)	((x)->offset)

#define	BMarginOfSeg(x)		_DtCvContainerBMarginOfSeg(x)
#define	TMarginOfSeg(x)		_DtCvContainerTMarginOfSeg(x)

#define	MySaveString(seg_list,my_struct,string,cur_link,multi_len,nl_flag)\
	_DtHelpCeSaveString(my_struct->ui_info->client_data,	\
				seg_list,			\
				&(my_struct->last_seg),		\
				&(my_struct->prev_data),	\
				string,				\
				my_struct->my_fonts,            \
				cur_link,			\
				multi_len,			\
				my_struct->flags,               \
				my_struct->ui_info->load_font,  \
				my_struct->resolve_font,        \
				nl_flag)

#define	SDL_WRAPPER		_DtCvAPP_FLAG1
#define	SetSdlWrapper(x)	_DtCvSetAppFlag1(x)
#define	IsSdlWrapper(x)		((x) & SDL_WRAPPER)

#define	SetDupFlag(x) \
		(FrmtPrivInfoPtr(x)->dup_flag = True)
#define	ClearDupFlag(x) \
		(FrmtPrivInfoPtr(x)->dup_flag = False)

#define	AbbrevOfSeg(x)			((FrmtPrivInfoPtr(x))->abbrev)
#define	ContainerPtrToType(x)		((x)->type)
#define	ContainerPtrToVJustify(x)	((x)->vjustify)
#define	ContainerPtrToJustify(x)	((x)->justify)
#define	ClearSegLinks(x) \
		(((x)->type) & ~(_DtCvHYPER_TEXT | _DtCvGHOST_LINK))
#define	SegMatchData(x)			((FrmtPrivInfoPtr(x))->match_info)
#define	SegMatchDataPtr(x) 		((SdlMatchData *) SegMatchData(x))

/******************************************************************************
 * Private Strings
 *****************************************************************************/
static const	char	AllStr[]          = "all";
static const	char	AnchorStr[]       = "<anchor";
static const	char	AnimateStr[]      = "<animate";
static const	char	ASyncStr[]        = "async";
static const	char	AudioStr[]        = "<audio";
static const	char	BlockStr[]        = "<block";
static const	char	ButtonStr[]       = "button";
static const	char	CallbackStr[]     = "<callback";
static const	char	CenterJustifyStr[]= "center-justify";
static const	char	CenterOrientStr[] = "center-orient";
static const	char	CParaStr[]        = "<cp";
static const	char	CrossdocStr[]     = "<crossdoc";
static const	char	DynamicStr[]      = "dynamic";
static const	char	GraphicStr[]      = "<graphic";
static const	char	FormStr[]         = "<form";
static const	char	HeadStr[]         = "<head";
static const	char	IsoStr[]          = "ISO-8859-1";
static const	char	LeftJustifyStr[]  = "left-justify";
static const	char	ManpageStr[]      = "<man-page";
static const	char	NameStr[]         = "name";
static const	char	NegativeOneStr[]  = "-1";
static const	char	NoBorderStr[]     = "no-border";
static const	char	OneStr[]          = "1";
static const	char	ProductStr[]      = "product";
static const	char	ParaStr[]         = "<p";
static const	char	ParentStr[]       = "parent";
static const	char	RightJustifyStr[] = "right-justify";
static const	char	ScriptStr[]       = "<script";
static const	char	SnrefStr[]        = "<snref";
static const	char	SubHeadStr[]      = "<subhead";
static const	char	SwitchStr[]       = "<switch";
static const	char	SyscmdStr[]       = "<sys-cmd";
static const	char	TenStr[]          = "10";
static const	char	TenThousandStr[]  = "10000";
static const	char	TextStr[]         = "<text";
static const	char	TextfileStr[]     = "<textfile";
static const	char	TopVJustStr[]     = "top-vjust";
static const	char	TopVOrientStr[]   = "top-vorient";
static const	char	UdefKeyStr[]      = "udefkey";
static const	char	VideoStr[]        = "<video";
static const	char	VirpageStr[]      = "<virpage";
static const	char	NoWrapStr[]       = "nowrap";
static const	char	ZeroStr[]         = "0";
static const	char	*NullOption       = "null_option";

static const	SDLMask	AllMaskSet[SDL_MASK_LEN] = SDLSetAllBits;

static FormatStruct DefFormatStruct =
    {
	False,			/* end_flag    */
	True,			/* last_was_space */
	False,			/* last_was_mb */
	False,			/* last_was_nl */
	False,			/* snref_used  */
	False,			/* faked_end   */
	_DtCvFALSE,		/* save_snref  */
	_DtCvFALSE,		/* save_blank  */
	_SdlFontModeResolve,	/* resolve_font*/
	SdlElementNone,		/* parsed      */
	nullptr,			/* remember    */
	nullptr,			/* vol_name    */
	nullptr,			/* id_string   */
	-1,			/* cur_link    */
	 1,			/* mb_len      */
	 0,			/* flags       */
	 1,			/* malloc_size */
	 0,			/* alloc_size  */
	 0,			/* free_cnt    */
	 0,			/* free_max    */
	 0,			/* snref_cnt   */
	nullptr,			/* *ui_info    */
	nullptr,			/* block_list  */
	nullptr,			/* add_seg     */
	nullptr,			/* last_seg    */
	nullptr,			/* seg_list    */
	nullptr,			/* prev_data   */
	nullptr,			/* toss        */
	nullptr,			/* snb         */
	nullptr,			/* else_prev   */
	nullptr,			/* then_prev   */
	nullptr,			/* async_blks  */
	nullptr,			/* active_frmt */
	nullptr, 			/* my_links    */
	  {			/* ElementInfo	 el_info; */
	    0,
	    0,
	    0,
	    0,
	    SdlElementNone,		/* enum SdlElement el_type    */
	    SdlTimingSync,		/* SdlOption       timing;    */
	    SdlTypeDynamic,		/* SdlOption       sdl_type   */
	    SdlWindowCurrent,		/* SdlOption       window;    */
	    SdlInterpKsh,		/* SdlOption       interp;    */
	    { 0 },			/* SDLNumber       count/offset; */
	    nullptr,			/* SDLCdata	   language;  */
	    nullptr,			/* SDLCdata	   char_set;  */
	    nullptr,			/* SDLCdata	   id;        */
	    nullptr,			/* SDLCdata	   elstr1;    */
	    nullptr,			/* SDLCdata	   elstr2;    */
	      {				/* SDLDocInfo	   doc_info;  */
		{
	        nullptr,			   /* SDLCdata	   language;  */
	        nullptr,			   /* SDLCdata	   char_set;  */
	        nullptr,			   /* SDLId	   first_pg;  */
	        nullptr,			   /* SDLCdata	   doc-id;    */
	        nullptr,			   /* SDLCdata	   timestamp; */
	        nullptr,			   /* SDLCdata    sdldtd;    */
		},
	      },
	      {				/* SdlMatchData              */
	        SdlClassText,		    /* SdlOption   clan;  */
	        -1,			    /* SDLNumber   level;     */
	        nullptr,			    /* SDLCdata	   ssi;       */
	      },
	  },
	  nullptr,			/* _DtHelpFontHints	 my_fonts;   */
	  nullptr,			/* _DtHelpFontHints	 free_fonts; */
	  nullptr,			/* UnresSnref		*un_snregs;  */
    };

static const	_DtHelpFontHints	DefFontInfo =
  {
	"C",				/* char	*language;  */
	(char *)IsoStr,			/* char	*charset    */
	10,				/* int	 pointsz;   */
	10,				/* int	 set_width  */
	nullptr,				/* char	*color;     */
	nullptr,				/* char	*xlfd;      */
	nullptr,				/* char	*xlfdb;     */
	nullptr,				/* char	*xlfdi;     */
	nullptr,				/* char	*xlfdib;    */
	nullptr,				/* char	*typenam;   */
	nullptr,				/* char	*typenamb;  */
	nullptr,				/* char	*typenami;  */
	nullptr,				/* char	*typenamib; */
	_DtHelpFontStyleSanSerif,	/* char	*style;     */
	_DtHelpFontSpacingMono,		/* char	*spacing;   */
	_DtHelpFontWeightMedium,	/* char	*weight;    */
	_DtHelpFontSlantRoman,		/* char	*slant;     */
	_DtHelpFontSpecialNone,		/* char	*special;   */
	nullptr,		    /* void	*expand;    */
  };

static const	_DtCvContainer	DefFrmtSpecs =
  {
	nullptr,			/* char         *id       */
	nullptr,			/* char         *justify_char */
	_DtCvDYNAMIC,		/* _DtCvFrmtOption  type     */
	_DtCvBORDER_NONE,	/* _DtCvFrmtOption  border   */
	_DtCvINHERIT,		/* _DtCvFrmtOption  justify  */
	_DtCvJUSTIFY_TOP,	/* _DtCvFrmtOption  vjustify */
	_DtCvJUSTIFY_CENTER,	/* _DtCvFrmtOption  orient   */
	_DtCvJUSTIFY_TOP,	/* _DtCvFrmtOption  vorient  */
	_DtCvWRAP_NONE,		/* _DtCvFrmtOption  flow     */
	DefPercent,		/* int           percent  */
	0,			/* _DtCvUnit        leading  */
	0,			/* _DtCvUnit        fmargin  */
	0,			/* _DtCvUnit        lmargin  */
	0,			/* _DtCvUnit        rmargin  */
	0,			/* _DtCvUnit        tmargin  */
	0,			/* _DtCvUnit        bmargin  */
	{0, nullptr},              /* _DtCvLine     bdr_info;      */
	nullptr,			/* _DtCvSegment    *seg_list */
  };

static	const	_FrmtUiInfo	DefUiInfo =
  {
	nullptr,		/* load_graphic */
	nullptr,		/* resolve_spc  */
	nullptr,		/* load_font    */
	nullptr,		/* exec_filter  */
	nullptr,		/* destroy_region */
	nullptr,		/* client_data  */
	0,		/* line_width   */
	0,		/* line_height   */
	0,		/* leading       */
	0,		/* avg_char     */
	True,		/* nl_to_space  */
  };

/******************************************************************************
 * Entity Defines
 *****************************************************************************/
/*-----------------------------------------------------------------------------
<!ENTITY % system-notations "(graphic  | text     | audio    | video    |
			      animate  | crossdoc | man-page | textfile |
			      sys-cmd  | callback | script   | switch)" >
-----------------------------------------------------------------------------*/
#define	SDL_ENTITY_SYSTEM_NOTATIONS \
		SDLInitMaskTwelve(SdlElementGraphic,   \
					SdlElementText,     \
					SdlElementAudio,    \
					SdlElementVideo,    \
					SdlElementAnimate,  \
					SdlElementCrossDoc, \
					SdlElementManPage,  \
					SdlElementTextFile, \
					SdlElementSysCmd,   \
					SdlElementCallback, \
					SdlElementScript,   \
					SdlElementSwitch)

/*-----------------------------------------------------------------------------
<!ENTITY % generated-elements "(loids, toss?, lophrases?, index?, rel-docs?,
				rel-file?, notes?)" >
-----------------------------------------------------------------------------*/
#define	SDL_ENTITY_GENERATED_ELEMENTS \
	{ SDLInitMask(SdlElementLoids)    , OnlyOne   }, \
	{ SDLInitMask(SdlElementToss)     , ZeroOrOne }, \
	{ SDLInitMask(SdlElementLoPhrases), ZeroOrOne }, \
	{ SDLInitMask(SdlElementIndex)    , ZeroOrOne }, \
	{ SDLInitMask(SdlElementRelDocs)  , ZeroOrOne }, \
	{ SDLInitMask(SdlElementRelFile)  , ZeroOrOne }, \
	{ SDLInitMask(SdlElementNotes)    , ZeroOrOne },

/*-----------------------------------------------------------------------------
<!ENTITY % key-class    "acro       | book      | emph     | jargon     |
			 l10n       | name      | quote    | product    |
			 termdef    | term      | mach-in  | mach-out   |
			 mach-cont  | pub-lit   | udefkey" >
-----------------------------------------------------------------------------*/
#define	SDL_ENTITY_CLASSK \
	{ "acro"     , SdlClassAcro     }, \
	{ "book"     , SdlClassBook     }, \
	{ "emph"     , SdlClassEmph     }, \
	{ "jargon"   , SdlClassJargon   }, \
	{ "l10n"     , SdlClassL10n     }, \
	{ NameStr    , SdlClassName     }, \
	{ "quote"    , SdlClassQuote    }, \
	{ ProductStr , SdlClassProduct  }, \
	{ "termdef"  , SdlClassTermdef  }, \
	{ "term"     , SdlClassTerm     }, \
	{ "mach-in"  , SdlClassMachIn   }, \
	{ "mach-out" , SdlClassMachOut  }, \
	{ "mach-cont", SdlClassMachCont }, \
	{ "pub-lit"  , SdlClassPubLit   }, \
	{ UdefKeyStr , SdlClassUdefkey  }  
/*-----------------------------------------------------------------------------
<!ENTITY % head-class    "label     | head      | caption  | annotation |
			  phead     | udefhead" >
-----------------------------------------------------------------------------*/
#define	SDL_ENTITY_CLASSH \
	{ "label"     , SdlClassLabel      }, \
	{ (HeadStr+1) , SdlClassHead       }, \
	{ "caption"   , SdlClassCaption    }, \
	{ "annotation", SdlClassAnnotation }, \
	{ "phead"     , SdlClassPHead      }, \
	{ "udefhead"  , SdlClassUdefhead   }

/*-----------------------------------------------------------------------------
<!ENTITY % format-class  "table     | cell      | list     | item       |
			  text      | udeffrmt" >
-----------------------------------------------------------------------------*/
#define	SDL_ENTITY_CLASSF \
	{ "table"    , SdlClassTable    }, \
	{ "cell"     , SdlClassCell     }, \
	{ "list"     , SdlClassList     }, \
	{ "item"     , SdlClassItem     }, \
	{ (TextStr+1), SdlClassText     }, \
	{ "udeffrmt" , SdlClassUdeffrmt }

/*-----------------------------------------------------------------------------
<!ENTITY % graphic-class "figure    | in-line   | button   | icon       |
			  udefgrph" >
-----------------------------------------------------------------------------*/
#define	SDL_ENTITY_CLASSG \
	{ "figure"    , SdlClassFigure    }, \
	{ "in-line"   , SdlClassInLine    }, \
	{ ButtonStr   , SdlClassButton    }, \
	{ "icon"      , SdlClassIcon      }, \
	{ "udefgraph" , SdlClassUdefgraph }

/*-----------------------------------------------------------------------------
<!ENTITY % phrase-class  "( super   | sub )" >
-----------------------------------------------------------------------------*/
#define	SDL_ENTITY_CLASSP \
	{ "super"   , SdlClassSuper }, \
	{ "sub"     , SdlClassSub   }

/*-----------------------------------------------------------------------------
<!ENTITY % font-styles
	   '-- height of font in points; main body is 10 point --
	    pointsz   NUMBER             #IMPLIED

	    -- width of font in points; defaults to point size --
	    -- similar to point size but refers to relative    --
	    -- width rather than height (e.g., an 8 point font --
	    -- based on a square would be 8 pt, 8 set)         --
	    set-width NUMBER             #IMPLIED

	    -- one of 42 CDE colors or "RGB:rrrr/gggg/bbbb"    --
	    color     CDATA              #IMPLIED

	    -- XLFD typeface name for use on X Window System   --
	    -- e.g., "-adobe-helvetica-bold-i-narrow-*-"       --
	    -- One each of normal, italic, bold and bold       --
	    -- italic must be specified.                       --
	    -- These should only be specified if the author is --
	    -- sure of exactly what font is desired.  In the   --
	    -- usual case, only the logical typeface spec.     --
	    -- defined below will be given.                    --
	    xlfd      CDATA              #IMPLIED
	    xlfdi     CDATA              #IMPLIED
	    xlfdb     CDATA              #IMPLIED
	    xlfdib    CDATA              #IMPLIED

	    -- MS-Windows typeface name (maximum of 32 chars)  --
	    -- One each of normal, italic, bold and bold       --
	    -- italic must be specified.                       --
	    -- As for the XLFD specification above, these      --
	    -- should only be provided if an author knows the  --
	    -- exact font desired.                             --
	    typenam   CDATA              #IMPLIED
	    typenami  CDATA              #IMPLIED
	    typenamb  CDATA              #IMPLIED
	    typenamib CDATA              #IMPLIED

	    -- Logical typeface spec (allows mapping into one  --
	    -- of the 13 PostScript typefaces).  This spec. is --
	    -- used if an exact match XLFD or Windows type-    --
	    -- face name attribute is missing or cannot be     --
	    -- found.  In the usual case, these will be        --
	    -- specified in preference to specifying an exact  --
	    -- font.                                           --

	    -- processor should default style to "sans-serif"  --
	    style     ( serif      |
			sans-serif |
			symbol     )     #IMPLIED

	    -- processor should deflt spacing to "monospace"   --
	    spacing   ( monospace  |
			propspace  )     #IMPLIED

	    -- processor should default weight to "medium"     --
	    weight    ( medium     |
			bold       )     #IMPLIED

	    -- processor should default slant to "roman"       --
	    slant     ( roman      |
			italic     |
			rev-italic )     #IMPLIED

	    -- processor should default special to "none"      --
	    special   ( underline  |
			strikeout  |
			none       )     #IMPLIED'             >

-----------------------------------------------------------------------------*/
#define	font_stylesAttrList \
	{ SDL_ATTR_POINTSZ  , SdlAttrValueImplied , TenStr       }, \
	{ SDL_ATTR_SETWIDTH , SdlAttrValueImplied , TenStr       }, \
	{ SDL_ATTR_COLOR    , SdlAttrValueImpliedDef , NULL         }, \
	{ SDL_ATTR_XLFD     , SdlAttrValueImpliedDef , NULL         }, \
	{ SDL_ATTR_XLFDI    , SdlAttrValueImpliedDef , NULL         }, \
	{ SDL_ATTR_XLFDB    , SdlAttrValueImpliedDef , NULL         }, \
	{ SDL_ATTR_XLFDIB   , SdlAttrValueImpliedDef , NULL         }, \
	{ SDL_ATTR_TYPENAM  , SdlAttrValueImpliedDef , NULL         }, \
	{ SDL_ATTR_TYPENAMI , SdlAttrValueImpliedDef , NULL         }, \
	{ SDL_ATTR_TYPENAMB , SdlAttrValueImpliedDef , NULL         }, \
	{ SDL_ATTR_TYPENAMIB, SdlAttrValueImpliedDef , NULL         }, \
	{ SDL_ATTR_STYLE    , SdlAttrValueImplied , "sans-serif" }, \
	{ SDL_ATTR_SPACING  , SdlAttrValueImplied , "monospace"  }, \
	{ SDL_ATTR_WEIGHT   , SdlAttrValueImplied , "medium"     }, \
	{ SDL_ATTR_SLANT    , SdlAttrValueImplied , "roman"      }, \
	{ SDL_ATTR_SPECIAL  , SdlAttrValueImplied , "none"       }

/*-----------------------------------------------------------------------------
<!ENTITY % format-styles
	   '-- left and right margins are additive and measured    --
	    -- in character widths defaulting to "0"               --
	    l-margin  NUMBER             #IMPLIED
	    r-margin  NUMBER             #IMPLIED

	    -- top and bottom margins merely take the maximum and  --
	    -- are measured in lines defaulting to "0"             --
	    t-margin  NUMBER             #IMPLIED
	    b-margin  NUMBER             #IMPLIED

	    -- border specifies the decoration type                --
	    -- processor should default border to "no-border"      --
	    border   ( no-border         |
		       full-border       |
		       horizontal-border |
		       vertical-border   |
		       top-border        |
		       bottom-border     |
		       left-border       |
		       right-border      ) #IMPLIED

	    -- applies to the text in the element, not the element --
	    -- itself.                                             --
	    -- processor should default vjust to "top-vjust"       --
	    vjust    ( top-vjust    |
		       bottom-vjust |
		       center-vjust )    #IMPLIED' >
-----------------------------------------------------------------------------*/
#define formt_stylesAttrList \
	{ SDL_ATTR_LMARGIN  , SdlAttrValueImplied , ZeroStr        }, \
	{ SDL_ATTR_RMARGIN  , SdlAttrValueImplied , ZeroStr        }, \
	{ SDL_ATTR_TMARGIN  , SdlAttrValueImplied , ZeroStr        }, \
	{ SDL_ATTR_BMARGIN  , SdlAttrValueImplied , ZeroStr        }, \
	{ SDL_ATTR_BORDER   , SdlAttrValueImplied , NoBorderStr    }, \
	{ SDL_ATTR_VJUST    , SdlAttrValueImplied , TopVJustStr    }

/*-----------------------------------------------------------------------------
<!ENTITY % linkage   "anchor | link" >
<!ENTITY % reference "snref"         >
<!ENTITY % simple    "key | sphrase | rev | if | spc | %reference;" >
-----------------------------------------------------------------------------*/
#define	SDL_ENTITY_SIMPLE   \
	SDLInitMaskSeven( \
			SdlElementKey   , SdlElementSphrase, SdlElementRev  , \
			SdlElementIf    , SdlElementSpc    , SdlElementSnRef, \
			SdlElementCdata)

/*-----------------------------------------------------------------------------
<!ENTITY % atomic    "( %simple; | %linkage; )" >
-----------------------------------------------------------------------------*/
#define	SDL_ENTITY_ATOMIC \
	SDLInitMaskNine( \
			SdlElementKey   , SdlElementSphrase, SdlElementRev   , \
			SdlElementIf    , SdlElementSpc    , SdlElementSnRef , \
			SdlElementAnchor, SdlElementLink   , SdlElementCdata)
/*-----------------------------------------------------------------------------
<!NOTATION tcl SYSTEM "embedded tcl interpreter">
<!NOTATION sh  SYSTEM "/bin/sh">
<!NOTATION csh SYSTEM "/bin/csh">
<!NOTATION ksh SYSTEM "/bin/ksh">
-----------------------------------------------------------------------------*/
typedef	struct	_ceInterpData {
	SdlOption	 type;
	char		*cmd;
} _CEInterpData;

static _CEInterpData	InterpData[] =
    {
	{ SdlInterpKsh,	"ksh" },
	{ SdlInterpCsh,	"csh" },
	{ SdlInterpSh ,	"sh"  },
	{ SdlInterpTcl,	"tcl" },
	{ _DtCvOPTION_BAD, nullptr  },
    };

/******************************************************************************
 * Private Variables
 *****************************************************************************/
static OptionList hclass[]  = { SDL_ENTITY_CLASSH, {nullptr, _DtCvOPTION_BAD}};
static OptionList fclass[]  = { SDL_ENTITY_CLASSF, {nullptr, _DtCvOPTION_BAD}};
static OptionList kclass[]  = { SDL_ENTITY_CLASSK, {nullptr, _DtCvOPTION_BAD}};
static OptionList pclass[]  = { SDL_ENTITY_CLASSP, {nullptr, _DtCvOPTION_BAD}};
static OptionList gclass[]  = { SDL_ENTITY_CLASSG, {nullptr, _DtCvOPTION_BAD}};
static OptionList fgclass[] =
	{ SDL_ENTITY_CLASSF, SDL_ENTITY_CLASSG, {nullptr, _DtCvOPTION_BAD}};

static OptionList apps[] =
    {
	{ AllStr    , SdlAppAll      },
	{ "help"    , SdlAppHelp     },
	{ "tutorial", SdlAppTutorial },
	{ "ref"     , SdlAppRef      },
	{ "sys"     , SdlAppSys      },
	{  nullptr     , _DtCvOPTION_BAD   }
    };
static OptionList timing[] =
    {
	{ (ASyncStr+1), SdlTimingSync  },
	{ ASyncStr    , SdlTimingAsync },
	{  nullptr       , _DtCvOPTION_BAD   },
    };

static OptionList frmtTypes[] =
    {
	{ "literal" , SdlTypeLiteral },
	{ "lined"   , SdlTypeLined   },
	{ DynamicStr, SdlTypeDynamic },
	{  nullptr     , _DtCvOPTION_BAD   }
    };

static OptionList idTypes[] =
    {
	{ (VirpageStr+1) , SdlIdVirpage   },
	{ (BlockStr+1)   , SdlIdBlock     },
	{ (FormStr+1)    , SdlIdForm      },
	{ (ParaStr+1)    , SdlIdPara      },
	{ (CParaStr+1)   , SdlIdCPara     },
	{ (HeadStr+1)    , SdlIdHead      },
	{ (SubHeadStr+1) , SdlIdSubHead   },
	{ (AnchorStr+1)  , SdlIdAnchor    },
	{ (SwitchStr+1)  , SdlIdSwitch    },
	{ (SnrefStr+1)   , SdlIdSnRef     },
	{ (GraphicStr+1) , SdlIdGraphic   },
	{ (TextStr+1)    , SdlIdText      },
	{ (AudioStr+1)   , SdlIdAudio     },
	{ (VideoStr+1)   , SdlIdVideo     },
	{ (AnimateStr+1) , SdlIdAnimate   },
	{ (CrossdocStr+1), SdlIdCrossDoc  },
	{ (ManpageStr+1) , SdlIdManPage   },
	{ (TextfileStr+1), SdlIdTextFile  },
	{ (SyscmdStr+1)  , SdlIdSysCmd    },
	{ (ScriptStr+1)  , SdlIdScript    },
	{ (CallbackStr+1), SdlIdCallback  },
	{ nullptr           , _DtCvOPTION_BAD   },
    };

static OptionList bool_types[] =
    {
	{ "YES", SdlBoolYes   },
	{ "NO" , SdlBoolNo    },
	{ nullptr , _DtCvOPTION_BAD },
    };

static OptionList borders[] =
    {
	{ NoBorderStr        , _DtCvBORDER_NONE   },
	{ "full-border"      , _DtCvBORDER_FULL   },
	{ "horizontal-border", _DtCvBORDER_HORZ   },
	{ "vertical-border"  , _DtCvBORDER_VERT   },
	{ "top-border"       , _DtCvBORDER_TOP    },
	{ "bottom-border"    , _DtCvBORDER_BOTTOM },
	{ "left-border"      , _DtCvBORDER_LEFT   },
	{ "right-border"     , _DtCvBORDER_RIGHT  },
	{ nullptr               , _DtCvOPTION_BAD    },
    };

static OptionList justify1[] =
    {
	{ LeftJustifyStr   , _DtCvJUSTIFY_LEFT   },
	{ RightJustifyStr  , _DtCvJUSTIFY_RIGHT  },
	{ CenterJustifyStr , _DtCvJUSTIFY_CENTER },
	{ "numeric-justify", _DtCvJUSTIFY_NUM    },
	{ nullptr             , _DtCvOPTION_BAD    },
    };

static OptionList justify2[] =
    {
	{ LeftJustifyStr   , _DtCvJUSTIFY_LEFT   },
	{ RightJustifyStr  , _DtCvJUSTIFY_RIGHT  },
	{ CenterJustifyStr , _DtCvJUSTIFY_CENTER },
	{ nullptr             , _DtCvOPTION_BAD    },
    };

static OptionList vjust[] =
    {
	{ TopVJustStr   , _DtCvJUSTIFY_TOP    },
	{ "bottom-vjust", _DtCvJUSTIFY_BOTTOM },
	{ "center-vjust", _DtCvJUSTIFY_CENTER },
	{ nullptr          , _DtCvOPTION_BAD     },
    };

static OptionList orient[] =
    {
	{ "left-orient"        , _DtCvJUSTIFY_LEFT        },
	{ CenterOrientStr      , _DtCvJUSTIFY_CENTER      },
	{ "right-orient"       , _DtCvJUSTIFY_RIGHT       },
	{ "left-margin-orient" , _DtCvJUSTIFY_LEFT_MARGIN },
	{ "right-margin-orient", _DtCvJUSTIFY_RIGHT_MARGIN},
	{ "left-corner-orient" , _DtCvJUSTIFY_LEFT_CORNER },
	{ "right-corner-orient", _DtCvJUSTIFY_RIGHT_CORNER},
	{ nullptr                 , _DtCvOPTION_BAD         },
    };

static OptionList vorient[] =
    {
	{ TopVOrientStr   , _DtCvJUSTIFY_TOP    },
	{ "bottom-vorient", _DtCvJUSTIFY_BOTTOM },
	{ "center-vorient", _DtCvJUSTIFY_CENTER },
	{ nullptr            , _DtCvOPTION_BAD     },
    };

static OptionList placement[] =
    {
	{ "object"     , SdlPlaceObject },
	{ ParentStr    , SdlPlaceParent },
	{  nullptr        , _DtCvOPTION_BAD   },
    };

static OptionList stacking[] =
    {
	{ "horiz"  , SdlStackHoriz },
	{ "vert"   , SdlStackVert  },
	{  nullptr    , _DtCvOPTION_BAD  },
    };

static OptionList flow[] =
    {
	{ (NoWrapStr+2), _DtCvWRAP       },
	{  NoWrapStr   , _DtCvWRAP_NONE  },
	{  "join"      , _DtCvWRAP_JOIN  },
	{  nullptr        , _DtCvOPTION_BAD },
    };

static OptionList window[] =
    {
	{ "current", SdlWindowCurrent },
	{ "new"    , SdlWindowNew     },
	{ "popup"  , SdlWindowPopup   },
	{ nullptr    , _DtCvOPTION_BAD },
    };

static OptionList traversal[] =
    {
	{ "return"  , SdlTraversalReturn   },
	{ "noreturn", SdlTraversalNoReturn },
	{ nullptr      , _DtCvOPTION_BAD         },
    };

static OptionList interpreters[] =
    {
	{ "ksh"     , SdlInterpKsh },
	{ "tcl"     , SdlInterpTcl },
	{ "csh"     , SdlInterpCsh },
	{ "sh"      , SdlInterpSh  },
	{ nullptr      , _DtCvOPTION_BAD },
    };

static	SDLAttributeOptions SDLOptionsList[] =
    {
	{SDL_ATTR_CLASSH   , hclass     },
	{SDL_ATTR_CLASSF   , fclass     },
	{SDL_ATTR_CLASSK   , kclass     },
	{SDL_ATTR_CLASSFG  , fgclass    },
	{SDL_ATTR_CLASSP   , pclass     },
	{SDL_ATTR_CLASSG   , gclass     },
	{SDL_ATTR_HDR      , bool_types },
	{SDL_ATTR_APP      , apps       },
	{SDL_ATTR_TIMING   , timing     },
	{SDL_ATTR_TYPEFRMT , frmtTypes  },
	{SDL_ATTR_BORDER   , borders    },
	{SDL_ATTR_JUSTIFY1 , justify1   },
	{SDL_ATTR_JUSTIFY2 , justify2   },
	{SDL_ATTR_VJUST    , vjust      },
	{SDL_ATTR_ORIENT   , orient     },
	{SDL_ATTR_VORIENT  , vorient    },
	{SDL_ATTR_PLACEMENT, placement  },
	{SDL_ATTR_STACK    , stacking   },
	{SDL_ATTR_FLOW     , flow       },
	{SDL_ATTR_WINDOW   , window     },
	{SDL_ATTR_TRAVERSAL, traversal  },
	{SDL_ATTR_TYPEID   , idTypes    },
	{SDL_ATTR_INTERP   , interpreters },
	{(unsigned long) -1, nullptr       }
    };

static FontSpecOption styles[] =
    {
	{ "serif"     , _DtHelpFontStyleSerif    },
	{ "sans-serif", _DtHelpFontStyleSanSerif },
	{ "symbol"    , _DtHelpFontStyleSymbol   },
	{ nullptr        , _DtHelpFontValueBad   },
    };

static FontSpecOption spacing[] =
    {
	{ "monospace" , _DtHelpFontSpacingMono},
	{ "propspace" , _DtHelpFontSpacingProp},
	{ nullptr        , _DtHelpFontValueBad   },
    };

static FontSpecOption weights[] =
    {
	{ "medium"    , _DtHelpFontWeightMedium },
	{ "bold"      , _DtHelpFontWeightBold   },
	{ nullptr        , _DtHelpFontValueBad  },
    };

static FontSpecOption slants[] =
    {
	{ "roman"     , _DtHelpFontSlantRoman    },
	{ "italic"    , _DtHelpFontSlantItalic   },
	{ "rev-italic", _DtHelpFontSlantRevItalic},
	{ nullptr        , _DtHelpFontValueBad   },
    };

static FontSpecOption special[] =
    {
	{ "none"      , _DtHelpFontSpecialNone      },
	{ "underline" , _DtHelpFontSpecialUnderLine },
	{ "strikeout" , _DtHelpFontSpecialStrikeOut },
	{ nullptr        , _DtHelpFontValueBad      },
    };

static	SDLFontSpecList SDLFontList[] =
    {
	{SDL_ATTR_STYLE    , styles     },
	{SDL_ATTR_SPACING  , spacing    },
	{SDL_ATTR_WEIGHT   , weights    },
	{SDL_ATTR_SLANT    , slants     },
	{SDL_ATTR_SPECIAL  , special    },
	{(unsigned long) -1, nullptr       }
    };

/*
 * Make sure the attribute names are in lower case.
 * The read routine to find an attribute automatically changes it to lower
 * case.
 */

#ifndef	_DtCvContainerPtr
typedef	_DtCvContainer *	_DtCvContainerPtr;
#endif

#ifndef	_DtCvSegmentPtr
typedef	_DtCvSegment *	_DtCvSegmentPtr;
#endif

static	SDLAttribute  SDLAttributeList[] =
  {
    { "abbrev"        , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_ABBREV   ,
			SDLAttrOffset(ElementInfoPtr,elstr1)          },

    { "app"           , SdlAttrDataTypeEnum  , SdlIgnore         ,
		 	SDL_ATTR_APP    ,
			0 },
    { "author"        , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_AUTHOR ,
			0 },

    { "b-margin"      , SdlAttrDataTypeNumber, SdlContainerSpecific ,
		 	SDL_ATTR_BMARGIN,
			SDLAttrOffset(_DtCvContainerPtr,bmargin) },
    { "border"        , SdlAttrDataTypeEnum  , SdlContainerSpecific ,
		 	SDL_ATTR_BORDER,
			SDLAttrOffset(_DtCvContainerPtr,border)  },
    { "branches"      , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_BRANCHES ,
			SDLAttrOffset(ElementInfoPtr,elstr1)          },

    { ButtonStr       , SdlAttrDataTypeId    , SdlIgnore         ,
		 	SDL_ATTR_BUTTON,
			0 },

    { "cells"         , SdlAttrDataTypeId    , SdlElementSpecific,
		 	SDL_ATTR_CELLS,
			SDLAttrOffset(ElementInfoPtr,elstr1)          },
    { "charset"       , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_CHARSET,
			SDLAttrOffset(ElementInfoPtr,char_set) },
    { "class"         , SdlAttrDataTypeEnum  , SdlElementSpecific,
		 	SDL_ATTR_CLASS ,
			SDLAttrOffset(ElementInfoPtr,match.clan)      },
    { "colj"          , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_COLJ   ,
			SDLAttrOffset(ElementInfoPtr,elstr1)          },
    { "color"         , SdlAttrDataTypeCdata , SdlFontSpecific,
		 	SDL_ATTR_COLOR  ,
			SDLAttrOffset(_DtHelpFontHintPtr,color) },
    { "colw"          , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_COLW   ,
			SDLAttrOffset(ElementInfoPtr,elstr2)          },
    { "command"       , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_COMMAND ,
			SDLAttrOffset(ElementInfoPtr,w.snb_info.xid)  },
    { "count"         , SdlAttrDataTypeNumber, SdlElementSpecific,
		 	SDL_ATTR_COUNT   ,
			SDLAttrOffset(ElementInfoPtr,nums.count) },
    { "data"          , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_DATA   ,
			SDLAttrOffset(ElementInfoPtr,w.snb_info.xid) },

    { "descript"      , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_DESCRIPT,
			0 },

    { "doc-id"        , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_DOCID  ,
			SDLAttrOffset(ElementInfoPtr,w.doc_info.doc_id) },
    { "enter"         , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_ENTER  ,
			SDLAttrOffset(ElementInfoPtr,elstr1)          },
    { "exit"          , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_EXIT   ,
			SDLAttrOffset(ElementInfoPtr,elstr2)          },
    { "first-page"    , SdlAttrDataTypeId    , SdlElementSpecific,
		 	SDL_ATTR_FRST_PG,
			SDLAttrOffset(ElementInfoPtr,w.doc_info.first_pg) },
    { "flow"          , SdlAttrDataTypeEnum  , SdlContainerSpecific ,
		 	SDL_ATTR_FLOW   ,
			SDLAttrOffset(_DtCvContainerPtr,flow) },
    { "f-margin"      , SdlAttrDataTypeNumber, SdlContainerSpecific ,
		 	SDL_ATTR_FMARGIN,
			SDLAttrOffset(_DtCvContainerPtr,fmargin) },
    { "format"        , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_FORMAT ,
			SDLAttrOffset(ElementInfoPtr,w.snb_info.format) },

    { "hdr"           , SdlAttrDataTypeEnum  , SdlIgnore         ,
		 	SDL_ATTR_HDR    ,
			0 },

    { "headw"         , SdlAttrDataTypeNumber , SdlContainerSpecific ,
		 	SDL_ATTR_HEADWDTH,
			SDLAttrOffset(_DtCvContainerPtr,percent) },
    { "id"            , SdlAttrDataTypeId    , SdlElementSpecific,
		 	SDL_ATTR_ID     ,
			SDLAttrOffset(ElementInfoPtr,id) },

    { "interp"        , SdlAttrDataTypeEnum  , SdlElementSpecific,
		 	SDL_ATTR_INTERP ,
			SDLAttrOffset(ElementInfoPtr,interp) },
    { "justify"       , SdlAttrDataTypeEnum  , SdlContainerSpecific ,
		 	SDL_ATTR_JUSTIFY,
			SDLAttrOffset(_DtCvContainerPtr,justify) },
    { "l-margin"      , SdlAttrDataTypeNumber, SdlContainerSpecific ,
		 	SDL_ATTR_LMARGIN,
			SDLAttrOffset(_DtCvContainerPtr,lmargin) },
    { "language"      , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_LANGUAGE   ,
			SDLAttrOffset(ElementInfoPtr,language) },
    { "length"        , SdlAttrDataTypeNumber, SdlIgnore         ,
		 	SDL_ATTR_LENGTH ,
			0 },

    { "level"         , SdlAttrDataTypeNumber, SdlElementSpecific,
		 	SDL_ATTR_LEVEL  ,
			SDLAttrOffset(ElementInfoPtr,match.level) },

    { "license"       , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_LICENSE  ,
			0 },
    { "linkinfo"      , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_LINKINFO,
			0 },

    { "locs"          , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_LOCS   ,
			SDLAttrOffset(ElementInfoPtr,w.entry_info.locs)    },
    { "main"          , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_MAIN   ,
			SDLAttrOffset(ElementInfoPtr,w.entry_info.main)    },
    { "method"        , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_METHOD ,
			SDLAttrOffset(ElementInfoPtr,w.snb_info.method)    },
    { NameStr         , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_NAME   ,
			SDLAttrOffset(ElementInfoPtr,elstr1)               },
    { "ncols"         , SdlAttrDataTypeNumber, SdlTableSpecific,
		 	SDL_ATTR_NCOLS  ,
			SDLAttrOffset(_DtCvSegmentPtr, handle.table.num_cols) },
    { "offset"        , SdlAttrDataTypeNumber, SdlElementSpecific,
		 	SDL_ATTR_OFFSET ,
			SDLAttrOffset(ElementInfoPtr,nums.offset)          },
    {(TopVOrientStr+5), SdlAttrDataTypeEnum  , SdlContainerSpecific ,
		 	SDL_ATTR_ORIENT ,
			SDLAttrOffset(_DtCvContainerPtr,orient) },

    { "placement"     , SdlAttrDataTypeEnum  , SdlIgnore         ,
		 	SDL_ATTR_PLACEMENT,
			0 },
    { "phrase"        , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_PHRASE ,
			0 },

    { "pointsz"       , SdlAttrDataTypeNumber, SdlFontSpecific,
		 	SDL_ATTR_POINTSZ ,
			SDLAttrOffset(_DtHelpFontHintPtr,pointsz) },

    { "pub-id"        , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_PUBID  ,
			0 },
    { "prodpn"        , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_PRODPN ,
			0 },
    { ProductStr      , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_PRODUCT,
			0 },
    { "prodver"       , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_PRODVER,
			0 },

    { "r-margin"      , SdlAttrDataTypeNumber, SdlContainerSpecific ,
		 	SDL_ATTR_RMARGIN,
			SDLAttrOffset(_DtCvContainerPtr,rmargin) },
    { "rid"           , SdlAttrDataTypeId    , SdlElementSpecific   ,
		 	SDL_ATTR_RID    ,
			SDLAttrOffset(ElementInfoPtr,id) },
    { "rlevel"        , SdlAttrDataTypeNumber, SdlElementSpecific,
		 	SDL_ATTR_RLEVEL ,
			SDLAttrOffset(ElementInfoPtr,match.level) },
    { "rssi"          , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_RSSI   ,
			SDLAttrOffset(ElementInfoPtr,match.ssi) },

    { "sdldtd"        , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_SDLDTD   ,
			SDLAttrOffset(ElementInfoPtr,w.doc_info.sdldtd) },

    { "set-width"     , SdlAttrDataTypeNumber, SdlFontSpecific,
		 	SDL_ATTR_SETWIDTH,
			SDLAttrOffset(_DtHelpFontHintPtr,set_width)},
    { "slant"         , SdlAttrDataTypeFont  , SdlFontSpecific   ,
		 	SDL_ATTR_SLANT,
			SDLAttrOffset(_DtHelpFontHintPtr,slant) },
    { "sort"          , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_SORT   ,
			SDLAttrOffset(ElementInfoPtr,w.entry_info.sort)    },
    { "spacing"       , SdlAttrDataTypeFont  , SdlFontSpecific   ,
		 	SDL_ATTR_SPACING,
			SDLAttrOffset(_DtHelpFontHintPtr,spacing) },
    { "special"       , SdlAttrDataTypeFont  , SdlFontSpecific   ,
		 	SDL_ATTR_SPECIAL,
			SDLAttrOffset(_DtHelpFontHintPtr,special) },

    { "srcdtd"        , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_SRCDTD   ,
			0 },
    { "srch-wt"       , SdlAttrDataTypeNumber, SdlIgnore         ,
		 	SDL_ATTR_SRCHWT ,
			0 },

    { "ssi"           , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_SSI    ,
			SDLAttrOffset(ElementInfoPtr,match.ssi) },

    { "stack"         , SdlAttrDataTypeEnum  , SdlIgnore         ,
		 	SDL_ATTR_STACK,
			0 },

    { "style"         , SdlAttrDataTypeFont  , SdlFontSpecific   ,
		 	SDL_ATTR_STYLE,
			SDLAttrOffset(_DtHelpFontHintPtr,style) },
    { "syns"          , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_SYNS   ,
			SDLAttrOffset(ElementInfoPtr,w.entry_info.syns)    },
    { "t-margin"      , SdlAttrDataTypeNumber, SdlContainerSpecific ,
		 	SDL_ATTR_TMARGIN,
			SDLAttrOffset(_DtCvContainerPtr,tmargin) },

    { (TextStr+1)     , SdlAttrDataTypeCdata , SdlIgnore,
		 	SDL_ATTR_TEXT     ,
			0 },

    { "timestmp"      , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_TIMESTAMP,
			SDLAttrOffset(ElementInfoPtr,w.doc_info.timestamp) },
    { "timing"        , SdlAttrDataTypeEnum  , SdlElementSpecific,
		 	SDL_ATTR_TIMING ,
			SDLAttrOffset(ElementInfoPtr,timing) },
    { "type"          , SdlAttrDataTypeEnum  , SdlElementSpecific,
		 	SDL_ATTR_TYPE   ,
			SDLAttrOffset(ElementInfoPtr,sdl_type) },

    { "traversal"     , SdlAttrDataTypeEnum  , SdlIgnore         ,
		 	SDL_ATTR_TRAVERSAL,
			0 },

    { "typenam"       , SdlAttrDataTypeCdata , SdlFontSpecific   ,
		 	SDL_ATTR_TYPENAM,
			SDLAttrOffset(_DtHelpFontHintPtr,typenam) },
    { "typenamb"      , SdlAttrDataTypeCdata , SdlFontSpecific   ,
		 	SDL_ATTR_TYPENAMB,
			SDLAttrOffset(_DtHelpFontHintPtr,typenamb) },
    { "typenami"      , SdlAttrDataTypeCdata , SdlFontSpecific   ,
		 	SDL_ATTR_TYPENAMI,
			SDLAttrOffset(_DtHelpFontHintPtr,typenami) },
    { "typenamib"     , SdlAttrDataTypeCdata , SdlFontSpecific   ,
		 	SDL_ATTR_TYPENAMIB,
			SDLAttrOffset(_DtHelpFontHintPtr,typenamib) },

    { "version"       , SdlAttrDataTypeCdata , SdlIgnore         ,
		 	SDL_ATTR_VERSION,
			0 },

    {(TopVJustStr+4)  , SdlAttrDataTypeEnum  , SdlContainerSpecific ,
		 	SDL_ATTR_VJUST,
			SDLAttrOffset(_DtCvContainerPtr,vjustify) },

    {(TopVOrientStr+4), SdlAttrDataTypeEnum  , SdlContainerSpecific ,
		 	SDL_ATTR_VORIENT ,
			SDLAttrOffset(_DtCvContainerPtr,vorient) },

    { "weight"        , SdlAttrDataTypeFont  , SdlFontSpecific   ,
		 	SDL_ATTR_WEIGHT,
			SDLAttrOffset(_DtHelpFontHintPtr,weight) },
    { "window"        , SdlAttrDataTypeEnum  , SdlElementSpecific,
		 	SDL_ATTR_WINDOW,
			SDLAttrOffset(ElementInfoPtr,window) },

    { "xid"           , SdlAttrDataTypeCdata , SdlElementSpecific,
		 	SDL_ATTR_XID    ,
			SDLAttrOffset(ElementInfoPtr,w.snb_info.xid) },

    { "xlfd"          , SdlAttrDataTypeCdata , SdlFontSpecific   ,
		 	SDL_ATTR_XLFD   ,
			SDLAttrOffset(_DtHelpFontHintPtr,xlfd) },
    { "xlfdb"         , SdlAttrDataTypeCdata , SdlFontSpecific   ,
		 	SDL_ATTR_XLFDB  ,
			SDLAttrOffset(_DtHelpFontHintPtr,xlfdb) },
    { "xlfdi"         , SdlAttrDataTypeCdata , SdlFontSpecific   ,
		 	SDL_ATTR_XLFDI  ,
			SDLAttrOffset(_DtHelpFontHintPtr,xlfdi) },
    { "xlfdib"        , SdlAttrDataTypeCdata , SdlFontSpecific   ,
		 	SDL_ATTR_XLFDIB ,
			SDLAttrOffset(_DtHelpFontHintPtr,xlfdib) },
    { 0            , SdlAttrDataTypeInvalid, -1,  0                    },
  };

/******************************************************************************
 * Element Content
 *****************************************************************************/
/*-----------------------------------------------------------------------------
<!-- Document Hierarchy  _____________________________________________-->

<!-- The root element is a pageless document, sdldoc.
   -   A sdldoc contains one or more virtual pages.
   -   A Virtual page is the smallest display unit.
   -   A Block is a unit of a given style.
   -   A Paragraph is a unit of character formatting.
  -->

<!ELEMENT sdldoc     - - (vstruct, head*, snb?, virpage+)>
<!ATTLIST sdldoc         pub-id     CDATA    #REQUIRED
                         doc-id     CDATA    #REQUIRED
                         timestmp   CDATA    #REQUIRED
                         first-page IDREF    #IMPLIED
                         product    CDATA    #IMPLIED
                         prodpn     CDATA    #IMPLIED
                         prodver    CDATA    #IMPLIED
                         license    CDATA    #IMPLIED
                         language   CDATA    #IMPLIED
			 charset    CDATA    #IMPLIED
                         author     CDATA    #IMPLIED
                         version    CDATA    #IMPLIED
			 sdldtd     CDATA    #REQUIRED
                         srcdtd     CDATA    #IMPLIED >

-----------------------------------------------------------------------------*/
static SDLContent SdlDocContent[] =
    {
/*
	{ SDLInitMask(SdlElementVStruct), OnlyOne   },
 * Do not want to parse the entire document.
 * Want to only read enought to work with.
 */
	{ SDLInitMask(SdlElementSdlDoc) , FakeEnd     },
	{ SDLInitMask(SdlElementSdlDoc) , CopyDocInfo },
	{ SDLInitMask(SdlElementNone)   , nullptr        },
    };
static SDLElementAttrList  SdlDocAttrList[] =
    {
	{ SDL_ATTR_PUBID    , SdlAttrValueRequired, nullptr   },
	{ SDL_ATTR_DOCID    , SdlAttrValueRequired, nullptr   },
	{ SDL_ATTR_TIMESTAMP, SdlAttrValueRequired, nullptr   },
	{ SDL_ATTR_PRODUCT  , SdlAttrValueImplied , nullptr   },
	{ SDL_ATTR_PRODPN   , SdlAttrValueImplied , nullptr   },
	{ SDL_ATTR_PRODVER  , SdlAttrValueImplied , nullptr   },
	{ SDL_ATTR_LICENSE  , SdlAttrValueImplied , nullptr   },
	{ SDL_ATTR_LANGUAGE , SdlAttrValueImplied , "C"    },
	{ SDL_ATTR_CHARSET  , SdlAttrValueImplied , "UTF-8"},
	{ SDL_ATTR_AUTHOR   , SdlAttrValueImplied , nullptr   },
	{ SDL_ATTR_VERSION  , SdlAttrValueImplied , nullptr   },
	{ SDL_ATTR_FRST_PG  , SdlAttrValueImplied , nullptr   },
	{ SDL_ATTR_SDLDTD   , SdlAttrValueRequired, nullptr   },
	{ SDL_ATTR_SRCDTD   , SdlAttrValueImplied , nullptr   },
	{ -1                , SdlAttrValueBad     , nullptr   }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT vstruct   - - (%generated-elements;) >
<!ATTLIST vstruct       version   CDATA              #IMPLIED
			doc-id    CDATA              #REQUIRED >
-----------------------------------------------------------------------------*/
static SDLContent VStructContent[] =
    {
	SDL_ENTITY_GENERATED_ELEMENTS
	{ SDLInitMask(SdlElementNone)   , nullptr      },
    };
static SDLElementAttrList  VStructAttrList[] =
    {
	{ SDL_ATTR_DOCID   , SdlAttrValueImplied , nullptr },
	{ SDL_ATTR_VERSION , SdlAttrValueImplied , nullptr },
	{ -1               , SdlAttrValueBad     , nullptr }
    };

/*-----------------------------------------------------------------------------

<!ELEMENT virpage   - - (head*, snb?, (block | form)*) >
<!ATTLIST virpage       id        ID                 #REQUIRED
			level     NUMBER             #REQUIRED
			version   CDATA              #IMPLIED
			language  CDATA              #IMPLIED
			charset   CDATA              #IMPLIED
			doc-id    CDATA              #REQUIRED
			ssi       CDATA              #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent VirpageContent[] =
    {
        { SDLInitMask(SdlElementHead)     , SetSaveSnref      },
        { SDLInitMask(SdlElementHead)     , ZeroToN           },
        { SDLInitMask(SdlElementSnb)      , ZeroOrOne         },
        { SDLInitMask(SdlElementHead)     , ClearAndCheckSnref},
        { SDLInitMaskTwo(SdlElementBlock, \
				SdlElementForm), ZeroToN   },
        { SDLInitMask(SdlElementNone)     , nullptr      },
    };
static SDLContent VirpageHeadContent[] =
    {
        { SDLInitMask(SdlElementHead)     , SetSaveSnref},
        { SDLInitMask(SdlElementHead)     , ZeroToN     },
        { SDLInitMask(SdlElementVirpage)  , CheckForSnb },
	{ SDLInitMask(SdlElementTitle)    , FakeEnd     },
        { SDLInitMask(SdlElementNone)     , nullptr        },
    };

static SDLContent HeadAndSnb[] =
    {
        { SDLInitMask(SdlElementHead)     , SetSaveSnref},
        { SDLInitMask(SdlElementHead)     , ZeroToN     },
        { SDLInitMask(SdlElementHead)     , CheckForSnb },
        { SDLInitMask(SdlElementNone)     , nullptr        },
    };

static SDLElementAttrList  VirpageAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueRequired  , nullptr   },
	{ SDL_ATTR_LEVEL   , SdlAttrValueRequired  , nullptr   },
	{ SDL_ATTR_VERSION , SdlAttrValueImplied   , nullptr   },
	{ SDL_ATTR_LANGUAGE, SdlAttrValueImplied   , nullptr   },
	{ SDL_ATTR_CHARSET , SdlAttrValueImplied   , nullptr   },
	{ SDL_ATTR_DOCID   , SdlAttrValueRequired  , nullptr   },
	{ SDL_ATTR_SSI     , SdlAttrValueImpliedDef, nullptr   },
	{ -1               , SdlAttrValueBad       , nullptr   }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT snb       - - (head?, (%system-notations;)+) >
<!ATTLIST snb           version   CDATA              #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent SnbContent[] =
    {
	{ SDLInitMask(SdlElementHead)  , ZeroOrOne },
	{ SDL_ENTITY_SYSTEM_NOTATIONS       , OneToN    },
	{ SDLInitMask(SdlElementNone)  , nullptr      },
    };
static SDLElementAttrList  SnbAttrList[] =
    {
	{ SDL_ATTR_VERSION, SdlAttrValueImplied , nullptr },
	{ -1               ,SdlAttrValueBad        , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT block     - - (head*, (p | cp)*) >
<!ATTLIST block         id        ID                 #IMPLIED

			-- processor should default level to "1"      --
			level     NUMBER             #IMPLIED
			version   CDATA              #IMPLIED
			class     (%format-class;)   #IMPLIED
			language  CDATA              #IMPLIED

			-- processor shld deflt charset to "ISO-8859-1" --
			charset   CDATA              #IMPLIED

			length    NUMBER             #IMPLIED

			-- processor should default app to "all"      --
			app      ( all      |
				   help     |
				   tutorial |
				   ref      |
				   sys      )        #IMPLIED

			-- processor should default timing to "sync"  --
			timing   ( sync | async )    #IMPLIED
			ssi       CDATA              #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent BlockContent[] =
    {
	{ SDLInitMask(SdlElementBlock)                   , SetTransit},
	{ SDLInitMask(SdlElementHead)                    , ZeroToN   },
	{ SDLInitMaskTwo(SdlElementPara, SdlElementCPara), ZeroToN   },
	{ SDLInitMask(SdlElementNone)                    , nullptr      },
    };
static SDLElementAttrList  BlockAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueImpliedDef , nullptr           },
	{ SDL_ATTR_LEVEL   , SdlAttrValueImpliedDef , OneStr         },
	{ SDL_ATTR_VERSION , SdlAttrValueImplied    , nullptr           },
	{ SDL_ATTR_CLASSF  , SdlAttrValueImplied    , (TextStr+1)    },
	{ SDL_ATTR_LANGUAGE, SdlAttrValueImplied    , nullptr           },
	{ SDL_ATTR_CHARSET , SdlAttrValueImplied    , nullptr           },
	{ SDL_ATTR_LENGTH  , SdlAttrValueImplied    , NegativeOneStr },
	{ SDL_ATTR_APP     , SdlAttrValueImplied    , AllStr         },
	{ SDL_ATTR_TIMING  , SdlAttrValueImplied    , (ASyncStr+1)   },
	{ SDL_ATTR_SSI     , SdlAttrValueImpliedDef , nullptr           },
	{ -1               , SdlAttrValueBad        , nullptr           }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT form      - - (head*, ((fstyle, fdata) | (fdata, fstyle))) >
<!ATTLIST form          id        ID                 #IMPLIED

			-- processor shld deflt level to nest of form --
			level     NUMBER             #IMPLIED
			version   CDATA              #IMPLIED
			class     (%format-class;)   #IMPLIED
			language  CDATA              #IMPLIED

			-- processor shld deflt charset to "ISO-8859-1" --
			charset   CDATA              #IMPLIED
			length    NUMBER             #IMPLIED

			-- processor should default app to "all"      --
			app      ( all      |
				   help     |
				   tutorial |
				   ref      |
				   sys      )        #IMPLIED
			ssi       CDATA              #IMPLIED >

-----------------------------------------------------------------------------*/
static SDLContent FormContent[] =
    {
	{ SDLInitMask(SdlElementHead)  , ZeroToN            },
	{ SDLInitMaskTwo(SdlElementFstyle, SdlElementFdata),
						OnlyOneEach },
	{ SDLInitMask(SdlElementForm)  , ColInfoToTableInfo },
	{ SDLInitMask(SdlElementNone)  , nullptr      },
    };
static SDLElementAttrList  FormAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueImpliedDef, nullptr           },
	{ SDL_ATTR_LEVEL   , SdlAttrValueImplied   , OneStr         },
	{ SDL_ATTR_VERSION , SdlAttrValueImplied   , nullptr           },
	{ SDL_ATTR_CLASSF  , SdlAttrValueImplied   , (TextStr+1)    },
	{ SDL_ATTR_LANGUAGE, SdlAttrValueImplied   , nullptr           },
	{ SDL_ATTR_CHARSET , SdlAttrValueImplied   , nullptr           },
	{ SDL_ATTR_LENGTH  , SdlAttrValueImplied   , NegativeOneStr },
	{ SDL_ATTR_APP     , SdlAttrValueImplied   , AllStr         },
	{ SDL_ATTR_SSI     , SdlAttrValueImpliedDef, nullptr           },
	{ -1               ,SdlAttrValueBad        , nullptr           }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT fstyle    - - (frowvec+) >
<!ATTLIST fstyle        -- The number of columns in this form, ncols, --
                        -- should be a number greater than zero.      --
			-- Unless overridden by a "colw" (column      --
			-- width) specification in a formstyle, the   --
			-- available space is divided evenly among    --
			-- the columns.  Unless overriden by a "colj" --
			-- specification in a formstyle, the columns  --
			-- are all left justified.                    --
                        -- processor should default ncols to 1        --
                        ncols     NUMBER             #IMPLIED >

-----------------------------------------------------------------------------*/
static SDLContent FstyleContent[] =
    {
	{ SDLInitMask(SdlElementFrowvec), OneToN  },
	{ SDLInitMask(SdlElementNone)   , nullptr    },
    };
static SDLElementAttrList  FstyleAttrList[] =
    {
	{ SDL_ATTR_NCOLS  , SdlAttrValueImpliedDef , OneStr },
	{ -1              , SdlAttrValueBad        , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT frowvec   - O EMPTY >
<!ATTLIST frowvec       -- processor should default hdr to "NO"       --
			hdr       (YES | NO)         #IMPLIED

			-- Ids of cell contents.  One id per cell.    --
                        -- Each id must refer to either a block or a  --
                        -- form.                                      --
			cells     IDREFS             #REQUIRED >
-----------------------------------------------------------------------------*/
static SDLContent FrowvecContent[] =
    {
	{ SDLInitMask(SdlElementFrowvec), AddRowToTable},
	{ SDLInitMask(SdlElementNone)   , nullptr    },
    };

static SDLElementAttrList  FrowvecAttrList[] =
    {
	{ SDL_ATTR_HDR    , SdlAttrValueImplied , "NO"      },
	{ SDL_ATTR_CELLS  , SdlAttrValueRequired, nullptr      },
	{ -1              , SdlAttrValueBad     , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT fdata     - - ((block | form)*) >
-----------------------------------------------------------------------------*/
static SDLContent FdataContent[] =
    {
	{ SDLInitMaskTwo(SdlElementBlock, SdlElementForm), ZeroToN },
	{ SDLInitMask(SdlElementNone)                    , nullptr    },
    };

/*-----------------------------------------------------------------------------
<!-- Containers ______________________________________________________-->

<!ELEMENT p         - - (head*, (%atomic | #PCDATA)+) >
<!ATTLIST p             id        ID                 #IMPLIED
			version   CDATA              #IMPLIED

			-- processor should default type to "dynamic" --
			type      (literal |
				   lined   |
				   dynamic )         #IMPLIED
			ssi       CDATA              #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent ParaContent[] =
    {
/*
 * This is optional and followed by potential PCDATA, therefore include
 * the SdlElementCdata & SdlPcDataFollows flag.
 */
	{ SDLInitMaskThree(SdlElementHead, SdlPcDataFollows, SdlElementCdata),
							ZeroToN   },
	{ SDLInitMask(SdlElementPara), InitLast  },
/*
 * PCDATA is described as 0 to n characters,
 * therefore, while the content says 'one to n' of (%atomic | #PCDATA)
 * we need to specify ZeroToN to work for PCDATA
	{ SDL_ENTITY_ATOMIC               , OneToN    },
 */
	{ SDL_ENTITY_ATOMIC               , ZeroToN   },
	{ SDLInitMask(SdlElementNone), nullptr      },
    };
static SDLElementAttrList  ParaAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueImpliedDef, nullptr      },
	{ SDL_ATTR_VERSION , SdlAttrValueImplied   , nullptr      },
	{ SDL_ATTR_TYPEFRMT, SdlAttrValueImplied   , DynamicStr},
	{ SDL_ATTR_SSI     , SdlAttrValueImpliedDef, nullptr      },
	{ -1               , SdlAttrValueBad       , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT cp        - - CDATA >
<!ATTLIST cp            id        ID                 #IMPLIED
			version   CDATA              #IMPLIED

			-- processor should default type to "dynamic" --
			type      (literal |
				   lined   |
				   dynamic )         #IMPLIED
			ssi       CDATA              #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent CParaContent[] =
    {
	{ SDLInitMask(SdlElementCPara), InitLast  },
	{ SDLInitMask(SdlElementCdata), ZeroOrOne },
	{ SDLInitMask(SdlElementNone) , nullptr  },
    };
/* uses the same attributes as <p> */

/*-----------------------------------------------------------------------------
<!-- Heads may have multiple sub-heads -->
<!ELEMENT head      - - ((%atomic; | #PCDATA)*, subhead*) >
<!ELEMENT subhead   - - ((%atomic | #PCDATA)*) >
<!ATTLIST (head | subhead)
			id        ID                 #IMPLIED
			version   CDATA              #IMPLIED

			-- processor should default class to "head"   --
			class     (%head-class;)     #IMPLIED
			language  CDATA              #IMPLIED

			-- processor shld deflt charset to "ISO-8859-1" --
			charset   CDATA              #IMPLIED

                        -- processor should default type to "dynamic" --
                        type      (literal |
                                   lined   |
                                   dynamic )         #IMPLIED
                        abbrev    CDATA              #IMPLIED

			ssi       CDATA              #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent HeadContent[] =
    {
	{ SDLInitMask(SdlElementHead)   , InitLast  },
	{ SDL_ENTITY_ATOMIC             , ZeroToN   },
	{ SDLInitMask(SdlElementSubHead), ZeroToN   },
	{ SDLInitMask(SdlElementNone)   , nullptr      },
    };
static SDLContent SubHeadContent[] =
    {
	{ SDLInitMask(SdlElementSubHead), InitLast  },
	{ SDL_ENTITY_ATOMIC             , ZeroToN   },
	{ SDLInitMask(SdlElementNone)   , nullptr      },
    };
static SDLElementAttrList  HeadAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueImpliedDef, nullptr       },
	{ SDL_ATTR_VERSION , SdlAttrValueImplied   , nullptr       },
	{ SDL_ATTR_CLASSH  , SdlAttrValueImplied   , (HeadStr+1)},
	{ SDL_ATTR_LANGUAGE, SdlAttrValueImplied   , nullptr       },
	{ SDL_ATTR_CHARSET , SdlAttrValueImplied   , nullptr       },
	{ SDL_ATTR_TYPEFRMT, SdlAttrValueImplied   , DynamicStr },
	{ SDL_ATTR_ABBREV  , SdlAttrValueImplied   , nullptr       },
	{ SDL_ATTR_SSI     , SdlAttrValueImpliedDef, nullptr       },
	{ -1               , SdlAttrValueBad       , nullptr       }
    };

/*-----------------------------------------------------------------------------
<!-- Atomic Link Elements ____________________________________________-->

<!ELEMENT anchor    - O EMPTY     -- Just marks the spot to jump to   -->
<!ATTLIST anchor        id        ID                 #REQUIRED >
-----------------------------------------------------------------------------*/
static SDLElementAttrList  AnchorAttrList[] =
    {
	{ SDL_ATTR_ID       , SdlAttrValueRequired, nullptr      },
	{ -1                , SdlAttrValueBad     , nullptr }
    };

static SDLContent AnchorContent[] =
    {
	{ SDLInitMask(SdlElementAnchor) , CopyAnchorId },
	{ SDLInitMask(SdlElementNone)   , nullptr         },
    };

/*-----------------------------------------------------------------------------
<!ELEMENT link      - - ((%atomic; | #PCDATA)+) -(link)>
<!ATTLIST link          -- rid is to id in this document or to a link --
			-- type element such as crossdoc in the snb   --
                        -- of the current virpage                     --
			rid       IDREF              #REQUIRED

			-- button should be a graphic in the snb      --
			button    IDREF              #IMPLIED

                        linkinfo  CDATA              #IMPLIED
                        descript  CDATA              #IMPLIED

			-- processor shld default window to "current" --
			window    (current  |
				   new      |
				   popup    )        #IMPLIED

			-- procssr shld dflt traversal to "noreturn"  --
			traversal (return   |
				   noreturn )        #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent LinkContent[] =
    {
	{ SDLInitMask(SdlElementLink) , RegisterLink },
/*
 * PCDATA is described as 0 to n characters,
 * therefore, while the content says 'one to n' of (%atomic | #PCDATA)
 * we need to specify ZeroToN to work for PCDATA
	{ SDL_ENTITY_ATOMIC                , OneToN       },
 */
	{ SDL_ENTITY_ATOMIC                , ZeroToN      },
	{ SDLInitMask(SdlElementNone) , nullptr         },
    };
static SDLElementAttrList  LinkAttrList[] =
    {
	{ SDL_ATTR_RID      , SdlAttrValueRequired  , nullptr      },
	{ SDL_ATTR_BUTTON   , SdlAttrValueImpliedDef, nullptr      },
	{ SDL_ATTR_WINDOW   , SdlAttrValueImplied   , "current" },
	{ SDL_ATTR_TRAVERSAL, SdlAttrValueImplied   , "noreturn"},
	{ SDL_ATTR_DESCRIPT , SdlAttrValueImpliedDef, nullptr      },
	{ SDL_ATTR_LINKINFO , SdlAttrValueImpliedDef, nullptr      },
	{ -1                , SdlAttrValueBad       , nullptr      }
    };

/*-----------------------------------------------------------------------------
<!-- reference to an element or alternates in the system notation block -->
<!ELEMENT snref     - - (refitem+, alttext?) >
<!ATTLIST snref         id       ID          #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent SnrefContent[] =
    {
	{ SDLInitMask(SdlElementSnRef)  , MarkFound  },
	{ SDLInitMask(SdlElementRefItem), OneToN     },
	{ SDLInitMask(SdlElementAltText), ZeroOrOne  },
	{ SDLInitMask(SdlElementNone)   , nullptr       },
    };
static SDLElementAttrList  SnrefAttrList[] =
    {
	{ SDL_ATTR_ID, SdlAttrValueImpliedDef, nullptr      },
	{ -1         , SdlAttrValueBad       , nullptr }
    };

/*-----------------------------------------------------------------------------
<!-- Each refitem is tried in turn until one can be  successfully
   - formatted.  The button is used to request display of the refitem
   - on systems where display of the item would be slow or expensive
   - in some other way, i.e., the button is displayed and the refitem
   - is only displayed on activiation of the button.
  -->
<!ELEMENT refitem   - - (head*) >
<!ATTLIST refitem       -- rid should point to a representational    --
                        -- element in the system notation block      --
                        rid      IDREF               #REQUIRED
                        class    (%graphic-class; |
                                  %format-class;  )  #REQUIRED

                        -- button should be a graphic in the snb      --
                        button    IDREF              #IMPLIED
                        ssi       CDATA              #IMPLIED   >
-----------------------------------------------------------------------------*/
static SDLContent RefItemContent[] =
    {
	{ SDLInitMask(SdlElementHead)    , ZeroOrOne    },
	{ SDLInitMask(SdlElementRefItem) , SaveItemInfo },
	{ SDLInitMask(SdlElementNone)    , nullptr         },
    };
static SDLElementAttrList  RefItemAttrList[] =
    {
	{ SDL_ATTR_RID    , SdlAttrValueRequired  , nullptr      },
	{ SDL_ATTR_CLASSFG, SdlAttrValueRequired  , nullptr      },
	{ SDL_ATTR_BUTTON , SdlAttrValueImplied   , nullptr      },
	{ SDL_ATTR_SSI    , SdlAttrValueImpliedDef, nullptr      },
	{ -1              , SdlAttrValueBad       , nullptr }
    };
/*-----------------------------------------------------------------------------
<!-- simple text to use if all else fails -->
<!ELEMENT alttext   - - CDATA >
-----------------------------------------------------------------------------*/
static SDLContent AltTextContent[] =
    {
	{ SDLInitMask(SdlElementCdata), ZeroOrOne },
	{ SDLInitMask(SdlElementNone) , nullptr    },
    };

/*-----------------------------------------------------------------------------
<!-- Atomic Text Elements ____________________________________________-->

<!-- empty rev implies delete -->
<!ELEMENT rev       - - ((%atomic; | #PCDATA)*) -(rev) >
-----------------------------------------------------------------------------*/
static SDLContent RevContent[] =
    {
	{ SDL_ENTITY_ATOMIC               , ZeroToN },
	{ SDLInitMask(SdlElementNone), nullptr    },
    };

/*-----------------------------------------------------------------------------
<!-- font changes -->
<!ELEMENT key       - - ((%atomic; | #PCDATA)*) -(link) >
<!ATTLIST key           -- processor shld deflt charset to "ISO-8859-1" --
			charset   CDATA              #IMPLIED
			class     (%key-class;)      #REQUIRED
			ssi       CDATA              #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent KeyContent[] =
    {
	{ SDLInitMask(SdlElementKey) , ProcessEnterAttr },
	{ SDL_ENTITY_ATOMIC          , ZeroToN          },
	{ SDLInitMask(SdlElementKey) , ProcessExitAttr  },
	{ SDLInitMask(SdlElementNone), nullptr             },
    };
static SDLElementAttrList  KeyAttrList[] =
    {
	{ SDL_ATTR_CHARSET , SdlAttrValueImplied   , nullptr      },
	{ SDL_ATTR_CLASSK  , SdlAttrValueRequired  , nullptr      },
	{ SDL_ATTR_SSI     , SdlAttrValueImpliedDef, nullptr      },
	{ -1               , SdlAttrValueBad       , nullptr }
    };

/*-----------------------------------------------------------------------------
<!-- super or subscripted phrase -->
<!ELEMENT sphrase   - - (spc | #PCDATA)* >
<!ATTLIST sphrase       class     %phrase-class      #REQUIRED
			ssi       CDATA              #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent SphraseContent[] =
    {
	{ SDLInitMask(SdlElementSphrase), CheckType },
	{ SDLInitMaskTwo(SdlElementSpc, SdlElementCdata), ZeroToN   },
	{ SDLInitMask(SdlElementSphrase), SetType   },
	{ SDLInitMask(SdlElementNone)   , nullptr      },
    };
static SDLElementAttrList  SphraseAttrList[] =
    {
	{ SDL_ATTR_CLASSP  , SdlAttrValueRequired  , nullptr      },
	{ SDL_ATTR_SSI     , SdlAttrValueImpliedDef, nullptr      },
	{ -1               , SdlAttrValueBad       , nullptr }
    };

/*-----------------------------------------------------------------------------
<!-- conditional inclusion of text -->
<!ELEMENT if        - - (cond, then, else?) >
-----------------------------------------------------------------------------*/
static SDLContent IfContent[] =
    {
	{ SDLInitMask(SdlElementIf)  , IfInfo    },
	{ SDLInitMask(SdlElementCond), OnlyOne   },
	{ SDLInitMask(SdlElementThen), OnlyOne   },
	{ SDLInitMask(SdlElementElse), ZeroOrOne },
	{ SDLInitMask(SdlElementNone), nullptr      },
    };

/*-----------------------------------------------------------------------------
<!-- call "interp" passing CDATA -->
<!ELEMENT cond      - - CDATA >
<!ATTLIST cond          -- processor should default interp to "ksh"   --
                        interp    NOTATION
                                  (tcl |
                                   sh  |
                                   ksh |
                                   csh )             #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent CondContent[] =
    {
	{ SDLInitMask(SdlElementCond) , SetType     },
	{ SDLInitMask(SdlElementCdata), ZeroOrOne   },
	{ SDLInitMask(SdlElementNone) , nullptr        },
    };
static SDLElementAttrList  CondAttrList[] =
    {
	{ SDL_ATTR_INTERP  , SdlAttrValueImplied    , "ksh"     },
	{ -1               , SdlAttrValueBad        , nullptr      }
    };

/*-----------------------------------------------------------------------------
<!-- include this text if "cond" returns non-zero -->
<!ELEMENT then      - - ((%atomic; | #PCDATA)*) >

<!-- include this text if "cond" returns zero -->
<!ELEMENT else      - - ((%atomic; | #PCDATA)*) >
-----------------------------------------------------------------------------*/
static SDLContent ThenElseContent[] =
    {
	{ SDLInitMask(SdlElementIf)  , IfInfo    },
	{ SDL_ENTITY_ATOMIC               , ZeroToN   },
	{ SDLInitMask(SdlElementNone), nullptr      },
    };

/*-----------------------------------------------------------------------------
<!-- special characters: the "name" attribute must be one of the special
   - character names, e.g., [bull  ] (bullet), defined in the public
   - character entities such as ISOpub, "ISO 8879:1986//ENTITIES
   - Publishing//EN";  these entities are defined in sections D.4.2.1
   - through D.4.5.6 of The SGML Handbook (Goldfarb).
   -->
<!ELEMENT spc       - O EMPTY >
<!ATTLIST spc           name      CDATA              #REQUIRED >
-----------------------------------------------------------------------------*/
static SDLContent SpcContent[] =
    {
	{ SDLInitMask(SdlElementSpc) , ResolveSpcInfo },
	{ SDLInitMask(SdlElementNone), nullptr        },
    };

static SDLElementAttrList  SpcAttrList[] =
    {
	{ SDL_ATTR_NAME, SdlAttrValueRequired, nullptr        },
	{ -1               ,SdlAttrValueBad        , nullptr }
    };
/*-----------------------------------------------------------------------------
<!-- Notation Elements _______________________________________________-->
<!-- The first group are representaional - referenced via <snref>     -->

<!-- Only one graphic element is declared.  The "class" attribute in
   - the refitem of the referring snref is used to distinguish between
   - figure, in-line or button.
  -->
<!ELEMENT graphic   - O EMPTY    >
<!ATTLIST graphic       id       ID                  #REQUIRED
                        format   CDATA               #IMPLIED
                        method   IDREF               #IMPLIED
                        xid      CDATA               #REQUIRED >
-----------------------------------------------------------------------------*/
static SDLContent GraphicContent[] =
    {
	{ SDLInitMask(SdlElementGraphic), LoadGraphic     },
	{ SDLInitMask(SdlElementNone)   , nullptr            },
    };

static SDLElementAttrList  NotationAttrList[] =
    {
	{ SDL_ATTR_ID    , SdlAttrValueRequired, nullptr        },
	{ SDL_ATTR_FORMAT, SdlAttrValueImplied , nullptr        },
	{ SDL_ATTR_METHOD, SdlAttrValueImplied , nullptr        },
	{ SDL_ATTR_XID   , SdlAttrValueRequired, nullptr        },
	{ -1               ,SdlAttrValueBad        , nullptr }
    };

/*-----------------------------------------------------------------------------
<!-- alternate rich text for use in a list of refitems in snref -->
<!ELEMENT text      - - ((p | cp)*) >
<!ATTLIST text          id       ID          #REQUIRED
			language CDATA       #IMPLIED

			-- processor shld dflt charset to "ISO-8859-1"  --
			charset  CDATA       #IMPLIED   >
-----------------------------------------------------------------------------*/
static SDLContent TextContent[] =
    {
	{ SDLInitMask(SdlElementText)  , SaveLangCharSet             },
	{ SDLInitMaskTwo(SdlElementPara, SdlElementCPara), ZeroToN   },
	{ SDLInitMask(SdlElementNone)                    , nullptr      },
    };
static SDLElementAttrList  TextAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueRequired, nullptr      },
	{ SDL_ATTR_LANGUAGE, SdlAttrValueImplied , nullptr      },
	{ SDL_ATTR_CHARSET , SdlAttrValueImplied , nullptr      },
	{ -1               , SdlAttrValueBad     , nullptr      }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT audio     - O EMPTY >
<!ATTLIST audio         id        ID                 #REQUIRED
			format    CDATA              #IMPLIED
			method    IDREF              #IMPLIED
			xid       CDATA              #REQUIRED >

<!ELEMENT video     - O EMPTY >
<!ATTLIST video         id        ID                 #REQUIRED
			format    CDATA              #IMPLIED
			method    IDREF              #IMPLIED
			xid       CDATA              #REQUIRED >

<!ELEMENT animate   - O EMPTY >
<!ATTLIST animate       id        ID                 #REQUIRED
			format    CDATA              #IMPLIED
			method    IDREF              #IMPLIED
			xid       CDATA              #REQUIRED >

/----
/- The audio, video, and animate use the NotationAttrList
/- structure defined under graphic.
/---
-----------------------------------------------------------------------------*/
/*-----------------------------------------------------------------------------
<!-- Execute the content of this element using the specified "interp"
   - whenever the script element is referenced via an snref.  If the
   - script returns a value, that value effectively becomes the CDATA
   - content of the script element and is interpolated into the document
   - at the point of the snref.
  -->
<!ELEMENT script    - - CDATA >
<!ATTLIST script        id        ID                 #REQUIRED
			-- processor should default interp to "ksh"   --
			interp    NOTATION
				  (tcl |
				   sh  |
				   ksh |
				   csh )             #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent ScriptContent[] =
    {
	{ SDLInitMask(SdlElementScript)    , SetType   },
	{ SDLInitMask(SdlElementCdata)     , ZeroOrOne },
	{ SDLInitMask(SdlElementNone)      , nullptr      },
    };
static SDLElementAttrList  ScriptAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueRequired, nullptr      },
	{ SDL_ATTR_INTERP  , SdlAttrValueImplied , "ksh"     },
	{ -1               ,SdlAttrValueBad        , nullptr }
    };


/*-----------------------------------------------------------------------------
<!-- The second group in the snb is linkage - referenced via <link>   -->
<!ELEMENT crossdoc  - O EMPTY >
<!ATTLIST crossdoc      id        ID                 #REQUIRED

			-- cross document link - doc & id   --
			xid       CDATA              #REQUIRED >

<!ELEMENT man-page  - O EMPTY >
<!ATTLIST man-page      id        ID                 #REQUIRED
			xid       CDATA              #REQUIRED >
-----------------------------------------------------------------------------*/
static SDLContent CrossDocContent[] =
    {
	{ SDLInitMask(SdlElementCrossDoc)  , RegisterSnbLink },
	{ SDLInitMask(SdlElementNone)      , nullptr         },
    };

static SDLContent ManPageContent[] =
    {
	{ SDLInitMask(SdlElementManPage)   , RegisterSnbLink },
	{ SDLInitMask(SdlElementNone)      , nullptr         },
    };

static SDLElementAttrList  IdAndXidAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueRequired, nullptr      },
	{ SDL_ATTR_XID     , SdlAttrValueRequired, nullptr      },
	{ -1               ,SdlAttrValueBad        , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT textfile  - O EMPTY >
<!ATTLIST textfile      id        ID                 #REQUIRED
			xid       CDATA              #REQUIRED
			offset    CDATA              #IMPLIED
			format    CDATA              #IMPLIED  >
-----------------------------------------------------------------------------*/
static SDLContent TextFileContent[] =
    {
	{ SDLInitMask(SdlElementTextFile)  , RegisterSnbLink },
	{ SDLInitMask(SdlElementNone)      , nullptr         },
    };

static SDLElementAttrList  TextFileAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueRequired, nullptr      },
	{ SDL_ATTR_XID     , SdlAttrValueRequired, nullptr      },
	{ SDL_ATTR_OFFSET  , SdlAttrValueImplied , ZeroStr   },
	{ SDL_ATTR_FORMAT  , SdlAttrValueImplied , nullptr      },
	{ -1               ,SdlAttrValueBad        , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT sys-cmd   - O EMPTY >
<!ATTLIST sys-cmd       id        ID                 #REQUIRED
			command   CDATA              #REQUIRED >
-----------------------------------------------------------------------------*/
static SDLContent SysCmdContent[] =
    {
	{ SDLInitMask(SdlElementSysCmd)    , RegisterSnbLink },
	{ SDLInitMask(SdlElementNone)      , nullptr         },
    };

static SDLElementAttrList  SysCmdAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueRequired, nullptr      },
	{ SDL_ATTR_COMMAND , SdlAttrValueRequired, nullptr      },
	{ -1               ,SdlAttrValueBad        , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT callback  - O EMPTY >
<!ATTLIST callback     id         ID                 #REQUIRED
		       data       CDATA              #IMPLIED  >
-----------------------------------------------------------------------------*/
static SDLContent CallbackContent[] =
    {
	{ SDLInitMask(SdlElementCallback)  , RegisterSnbLink },
	{ SDLInitMask(SdlElementNone)      , nullptr         },
    };

static SDLElementAttrList  CallbackAttrList[] =
    {
	{ SDL_ATTR_ID      , SdlAttrValueRequired, nullptr      },
	{ SDL_ATTR_DATA    , SdlAttrValueImplied , nullptr      },
	{ -1               ,SdlAttrValueBad        , nullptr }
    };

/*-----------------------------------------------------------------------------
<!-- The switch element is always hyperlinked to.  The interpreter,
   - interp, is called passing the CDATA content and returning a number
   - 0 to n.  The return value of the interpreter is used to index into
   - the list of branches and the hyperlink is continued to that ID.  A
   - return value less than zero or greater than the number of IDs minus
   - 1 causes the hyperlink to continue to branch 0.
  -->
<!ELEMENT switch    - - CDATA >
<!ATTLIST switch        id       ID          #REQUIRED
                        -- processor should default interp to "ksh"   --
                        interp   NOTATION
                                 (tcl |
                                  sh  |
                                  ksh |
                                  csh )      #IMPLIED
                        branches IDREFS      #REQUIRED >
-----------------------------------------------------------------------------*/
static SDLContent SwitchContent[] =
    {
	{ SDLInitMask(SdlElementSwitch), SetType        },
	{ SDLInitMask(SdlElementCdata) , ZeroOrOne      },
	{ SDLInitMask(SdlElementSwitch), RegisterSwitch },
	{ SDLInitMask(SdlElementNone)  , nullptr           },
    };
static SDLElementAttrList SwitchAttrList[] =
    {
	{ SDL_ATTR_ID       , SdlAttrValueRequired, nullptr      },
	{ SDL_ATTR_INTERP   , SdlAttrValueImplied , "ksh"     },
	{ SDL_ATTR_BRANCHES , SdlAttrValueRequired, nullptr      },
	{ -1                , SdlAttrValueBad     , nullptr      }
    };

/*-----------------------------------------------------------------------------
<!-- Generated Elements ______________________________________________-->

<!ELEMENT rel-docs  - O EMPTY >
<!ELEMENT rel-file  - O EMPTY >
<!ELEMENT notes     - O EMPTY >

<!-- The list of identifiers, loids, element is a list of ids in this
     document in the order they are defined.  The "count" attribute of
     loids is the number of ids it contains.  The higher level DTD to
     SDL translator may precompute "count" to enable the processor to
     preallocate space for the ids.  If "count" is not present, the
     processor must compute the number itself from the document.  The
     "type" attribute of id is the name of the element to which the
     id belongs.  The "rid" (referenced identifier) attribute is the
     identifier being listed.  The "rssi" (referenced source semantic
     identifier) is the "ssi" of the element to which the identifier
     belongs.  Similarly, the "rlevel" (referenced level) attribute
     is the "level" of the element to which the identifier belongs.
     Finally, the "offset" attribute is the byte offset in the document
     to the start of the virtual page containing the identifier. -->
<!ELEMENT loids     - - (id*) >
<!ATTLIST loids         count     NUMBER             #IMPLIED   >
-----------------------------------------------------------------------------*/
static SDLContent LoidsContent[] = {
	{ SDLInitMask(SdlElementLoids), AllocateBlock },
	{ SDLInitMask(SdlElementId)   , ZeroToN       },
	{ SDLInitMask(SdlElementLoids), CleanUpBlock  },
	{ SDLInitMask(SdlElementNone) , nullptr          },
    };

static SDLElementAttrList  LoidsAttrList[] =
    {
	{ SDL_ATTR_COUNT, SdlAttrValueImplied, nullptr },
	{ -1            , SdlAttrValueBad    , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT id        - O EMPTY >
<!ATTLIST id            type     (virpage  |
                                  block    |
                                  form     |
                                  p        |
                                  cp       |
                                  head     |
                                  subhead  |
                                  anchor   |
                                  switch   |
                                  snref    |
                                  graphic  |
                                  text     |
                                  audio    |
                                  video    |
                                  animate  |
                                  crossdoc |
                                  man-page |
                                  textfile |
                                  sys-cmd  |
                                  script   |
                                  callback )         #REQUIRED
                        rid       IDREF              #REQUIRED
                        rssi      CDATA              #IMPLIED
                        rlevel    NUMBER             #IMPLIED
                        offset    NUMBER             #REQUIRED  >
-----------------------------------------------------------------------------*/
static SDLContent IdContent[] = {
	{ SDLInitMask(SdlElementId)   , CopyIdInfo },
	{ SDLInitMask(SdlElementNone) , nullptr       },
    };

static SDLElementAttrList  IdAttrList[] =
    {
	{ SDL_ATTR_TYPEID  , SdlAttrValueRequired, nullptr },
	{ SDL_ATTR_RID     , SdlAttrValueRequired, nullptr },
	{ SDL_ATTR_RLEVEL  , SdlAttrValueImplied , NegativeOneStr },
	{ SDL_ATTR_RSSI    , SdlAttrValueImpliedDef, nullptr },
	{ SDL_ATTR_OFFSET  , SdlAttrValueRequired, nullptr },
	{ -1               , SdlAttrValueBad     , nullptr }
    };

/*-----------------------------------------------------------------------------
<!-- An index consists of zero or more entries.  Each entry contains
     the indexed (rich) text.  The "count" attribute of index is the
     number of entries (recursively) it contains.  The higher level
     DTD to SDL translator may precompute "count" to enable the
     processor to preallocate space for the entries.  If "count" is
     not present, the processor must compute the number itself from
     the document.  The "locs" and "main" attributes of an entry are
     lists of ids where the entry may be found.  Those ids found on
     the "main" list may be highlighted or emphasized in some way to
     indicate a greater importance than the ids found on the "locs"
     list - a definition, for example.  Otherwise, ids found on the
     "locs" list and the "main" list behave identically.  The "syns"
     attribute of an entry is another list of ids that refer to other
     entry elements and correspond to a "See also" or synonym type
     reference in an index.  The "sort" attribute is an optional sort
     key to be used if the indexed entry is to be sorted other than
     by its content.  The index should be pre-sorted although the
     optional sort keys are preserved in case multiple indexes need
     to be merged at some later date.  An entry element may also
     contain other entries to allow a hierarchical index to be
     generated. -->
<!ELEMENT index     - - (entry*)                              >
<!ATTLIST index         head      CDATA  #IMPLIED
			count     NUMBER #IMPLIED              >
-----------------------------------------------------------------------------*/
static SDLContent IndexContent[] = {
	{ SDLInitMask(SdlElementEntry), ZeroToN       },
	{ SDLInitMask(SdlElementNone) , nullptr          },
    };

static SDLElementAttrList  IndexAttrList[] =
    {
	{ SDL_ATTR_COUNT, SdlAttrValueImplied, nullptr },
	{ -1            , SdlAttrValueBad    , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT entry     - - ((%simple; | #PCDATA)*, entry*)       >
<!ATTLIST entry         id      ID     #IMPLIED
                        main    IDREFS #IMPLIED
                        locs    IDREFS #IMPLIED
                        syns    IDREFS #IMPLIED
                        sort    CDATA  #IMPLIED               >
-----------------------------------------------------------------------------*/
static SDLContent EntryContent[] = {
	{ SDLInitMask(SdlElementEntry), CopyEntryInfo },
	{ SDL_ENTITY_SIMPLE           , ZeroToN       },
	{ SDLInitMask(SdlElementEntry), ZeroToN       },
	{ SDLInitMask(SdlElementNone) , nullptr          },
    };

static SDLElementAttrList  EntryAttrList[] =
    {
	{ SDL_ATTR_ID      ,SdlAttrValueImpliedDef , nullptr },
	{ SDL_ATTR_MAIN    ,SdlAttrValueImpliedDef , nullptr },
	{ SDL_ATTR_LOCS    ,SdlAttrValueImpliedDef , nullptr },
	{ SDL_ATTR_SYNS    ,SdlAttrValueImpliedDef , nullptr },
	{ SDL_ATTR_SORT    ,SdlAttrValueImpliedDef , nullptr },
	{ -1               ,SdlAttrValueBad        , nullptr }
    };

/*-----------------------------------------------------------------------------
<!-- The lophrases (list of phrases) element is generated by the
     higher level DTD to SDL translator.  It is a list of phrases used
     from the toss element content in this document.  The phrases are
     used to map the users knowledge domain into the constructs used
     in SDL.  This information can be used to to pass the phrases
     available for structured/semantic searches to a browser or viewer
     to allow that browser or viewer to offer that information to the
     user.  The "count" attribute of lophrases is the number of phrases
     it contains.  The higher level DTD to SDL translator may precompute
     "count" to enable the processor to preallocate space for the
     phrases.  If "count" is not present, the processor must compute
     the number itself from the document.  -->
<!ELEMENT lophrases - - (phrase+) >
<!ATTLIST lophrases     count     NUMBER             #IMPLIED  >
-----------------------------------------------------------------------------*/
static SDLContent LoPhraseContent[] =
    {
	{ SDLInitMask(SdlElementPhrase)                  , OneToN },
	{ SDLInitMask(SdlElementNone)                    , nullptr   },
    };

static SDLElementAttrList  LoPhraseAttrList[] =
    {
	{ SDL_ATTR_COUNT, SdlAttrValueImplied, nullptr },
	{ -1            , SdlAttrValueBad    , nullptr }
    };
/*-----------------------------------------------------------------------------
<!ELEMENT phrase    - O EMPTY     >
<!ATTLIST phrase        text      CDATA              #REQUIRED >
-----------------------------------------------------------------------------*/
static SDLElementAttrList  PhraseAttrList[] =
    {
	{ SDL_ATTR_TEXT    , SdlAttrValueRequired, nullptr },
	{ -1               ,SdlAttrValueBad        , nullptr }
    };

/*-----------------------------------------------------------------------------
<!-- The following element, toss (table of semantics and styles), is
     generated by the higher level DTD to SDL translator.  The "count"
     attribute of toss is the number of styles it contains.  The
     translator may precompute "count" to enable the processor to
     preallocate space for the styles.  If "count" is not present, the
     processor must compute the number itself from the document.  The
     first three attributes of each sub-element are used for lookup in
     the toss.  When formatting an element, the toss is searched for a

     With the exception of the XLFD and Windows typeface name, an
     unspecified attribute implies inheritance.

     The "phrase" attribute is an English (or natural language) phrase
     describing the intended use (semantics) of an element of this
     style.  -->
<!ELEMENT toss      - - (keystyle*,
			headstyle*,
			formstyle*,
			frmtstyle*,
			grphstyle*) >
<!ATTLIST toss          count     NUMBER             #IMPLIED   >
-----------------------------------------------------------------------------*/
static SDLContent TossContent[] = {
	{ SDLInitMask(SdlElementToss)     , AllocateBlock },
	{ SDLInitMask(SdlElementKeyStyle) , ZeroToN       },
	{ SDLInitMask(SdlElementHeadStyle), ZeroToN       },
	{ SDLInitMask(SdlElementFormStyle), ZeroToN       },
	{ SDLInitMask(SdlElementFrmtStyle), ZeroToN       },
	{ SDLInitMask(SdlElementGrphStyle), ZeroToN       },
	{ SDLInitMask(SdlElementToss)     , CleanUpBlock  },
	{ SDLInitMask(SdlElementNone)     , nullptr          },
    };

static SDLElementAttrList  TossAttrList[] =
    {
	{ SDL_ATTR_COUNT, SdlAttrValueImplied, nullptr },
	{ -1            , SdlAttrValueBad    , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT keystyle  - O EMPTY    >
<!ATTLIST keystyle      class     (%key-class;)      #REQUIRED
                        ssi       CDATA              #IMPLIED

                        -- the level of the element being described   --
                        rlevel    NUMBER             #IMPLIED

                        phrase    CDATA              #IMPLIED

                        -- likelihood that this element contains an   --
                        -- actual hit when doing a full text search   --
                        srch-wt   NUMBER             #IMPLIED

                        -- strings to emit on entry and exit from key --
                        enter     CDATA              #IMPLIED
                        exit      CDATA              #IMPLIED

                        %font-styles; >
-----------------------------------------------------------------------------*/
static SDLContent KeyStyleContent[] = {
	{ SDLInitMask(SdlElementKeyStyle) , CopyTossInfo  },
	{ SDLInitMask(SdlElementNone)     , nullptr          },
    };
static SDLElementAttrList  KeyStyleAttrList[] =
    {
	{ SDL_ATTR_CLASSK  , SdlAttrValueRequired  , nullptr },
	{ SDL_ATTR_SSI     , SdlAttrValueImpliedDef, nullptr },
	{ SDL_ATTR_RLEVEL  , SdlAttrValueImplied   , NegativeOneStr },
	{ SDL_ATTR_PHRASE  , SdlAttrValueImpliedDef, nullptr },
	{ SDL_ATTR_SRCHWT  , SdlAttrValueImplied   , NegativeOneStr },
	{ SDL_ATTR_ENTER   , SdlAttrValueImpliedDef, nullptr },
	{ SDL_ATTR_EXIT    , SdlAttrValueImpliedDef, nullptr },
	font_stylesAttrList,
	{ -1               , SdlAttrValueBad       , nullptr }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT headstyle - O EMPTY    >
<!ATTLIST headstyle     class     (%head-class;)     #REQUIRED
                        ssi       CDATA              #IMPLIED

                        -- the level of the element being described   --
                        rlevel    NUMBER             #IMPLIED

                        phrase    CDATA              #IMPLIED

                        -- likelihood that this element contains an   --
                        -- actual hit when doing a full text search   --
                        srch-wt   NUMBER             #IMPLIED

			-- horizontal orientation of the head with    --
                        -- respect to its associated document,        --
			-- vstruct, virpage, snb, block, form, or p;  --
			-- or of the subhead with respect to its      --
			-- head.                                      --
                        -- procsr shld dflt orient to "center-orient" --
                        orient    (left-orient         |
                                   center-orient       |
                                   right-orient        |
                                   left-margin-orient  |
                                   right-margin-orient |
                                   left-corner-orient  |
                                   right-corner-orient )
                                                     #IMPLIED

			-- vertical orientation of the head or        --
                        -- subhead with respect to its parent.        --
                        -- procsor shld dflt vorient to "top-vorient" --
                        vorient   (top-vorient    |
                                   bottom-vorient |
                                   center-vorient )  #IMPLIED

                        -- This attribute applies to head elements    --
                        -- only, since subheads cannot contain more   --
                        -- subheads.  The attribute determines        --
                        -- whether the vorient attribute applies to   --
                        -- the head only or to the entire head object --
                        -- including its subheads.                    --
                        -- processor shld deflt placement to "parent" --
                        placement (object | parent)  #IMPLIED

                        -- Head width is the percent of the           --
                        -- available space for this element that      --
			-- should be given to its head or the percent --
			-- of the head that should be given to a      --
                        -- subhead.  It is expressed as a fixed point --
                        -- number 1 to 10000 with an implied decimal  --
                        -- point two places to the left of the right  --
                        -- side.                                      --
                        -- processor should default headw to "10000"  --
                        headw      CDATA             #IMPLIED

			-- where to put this head or subhead if it    --
                        -- collides with one already placed.          --
                        -- Horizontal stacking means place this one   --
                        -- under the other.  Vertical stacking means  --
                        -- place this one to the right of the other.  --
                        -- processor should default stack to "vert"   --
                        stack     (horiz | vert)     #IMPLIED

                        -- does the body wrap around the head text?   --
			-- "join" implies starting the content of the --
			-- surrounding element immediatly after this  --
			-- head (i.e., on the same line as the bottom --
			-- of this head).
			-- processor should default flow to "nowrap"  --
                        flow       (wrap   |
				    nowrap |
				    join)           #IMPLIED

                        %font-styles;
                        %format-styles;

		        -- applies to the text in the element, not   --
		        -- the element itself.                       --
		        -- prcsr shld dflt justify to "left-justify" --
		        justify  ( left-justify    |
				   right-justify   |
			           center-justify  |
			           numeric-justify ) #IMPLIED >

-----------------------------------------------------------------------------*/
static SDLContent HeadStyleContent[] = {
	{ SDLInitMask(SdlElementHeadStyle), CopyTossInfo  },
	{ SDLInitMask(SdlElementNone)     , nullptr          },
    };
static SDLElementAttrList  HeadStyleAttrList[] =
    {
	{ SDL_ATTR_CLASSH   , SdlAttrValueRequired  , nullptr           },
	{ SDL_ATTR_SSI      , SdlAttrValueImpliedDef, nullptr           },
	{ SDL_ATTR_RLEVEL   , SdlAttrValueImplied   , NegativeOneStr },
	{ SDL_ATTR_PHRASE   , SdlAttrValueImplied   , nullptr           },
	{ SDL_ATTR_SRCHWT   , SdlAttrValueImplied   , NegativeOneStr },
	{ SDL_ATTR_ORIENT   , SdlAttrValueImplied   , CenterOrientStr},
	{ SDL_ATTR_VORIENT  , SdlAttrValueImplied   , TopVOrientStr  },
	{ SDL_ATTR_HEADWDTH , SdlAttrValueImplied   , TenThousandStr },
	{ SDL_ATTR_PLACEMENT, SdlAttrValueImpliedDef, ParentStr      },
	{ SDL_ATTR_STACK    , SdlAttrValueImpliedDef, "vert"         },
	{ SDL_ATTR_FLOW     , SdlAttrValueImplied   , NoWrapStr      },
	font_stylesAttrList ,
	formt_stylesAttrList,
	{ SDL_ATTR_JUSTIFY1 , SdlAttrValueImplied   , LeftJustifyStr }, 
	{ -1                , SdlAttrValueBad       , nullptr           }
    };

/*-----------------------------------------------------------------------------
<!ELEMENT formstyle - O EMPTY    >
<!ATTLIST formstyle     class     (%format-class;)   #REQUIRED
			ssi       CDATA              #IMPLIED

			-- the level of the element being described   --
			rlevel    NUMBER             #IMPLIED

			phrase    CDATA              #IMPLIED

			-- likelihood that this element contains an   --
			-- actual hit when doing a full text search   --
			srch-wt   NUMBER             #IMPLIED

			-- The widths of the columns in this form,    --
			-- colw, is a space separated list of comma   --
			-- separated integral triples.  If only two   --
			-- comma separated numbers in a triple are    --
			-- given, the second is replicated to make    --
			-- three.  If only one number is given, the   --
			-- other two are assumed to be 0. The first   --
			-- number of a triple is the optimally        --
			-- desired width.  The second number is how   --
			-- much the column is willing to grow         --
			-- relative to the other columns.  The third  --
			-- number is how much the column is willing   --
			-- to shrink relative to the other columns.   --
			-- The numbers are summed and the total is    --
			-- taken as 100 percent of available space.   --
			-- That space is then divided among the       --
			-- columns.  The process of summing and       --
			-- dividing is repeated until all the         --
			-- desired/shrink/grow constraints are met or --
			-- it is known to be impossible to meet them. --
			-- If meeting the constraints is impossible,  --
			-- the column is handled similarly to a       --
			-- graphic that will not fit, e.g., a scroll- --
			-- bar may be added to allow the form to be   --
			-- larger than the size of the viewing area.  --
			-- There should be as many triples as given   --
			-- in the value "ncols" in the "fstyle" sub-  --
			-- element of the form element to which this  --
			-- "formstyle" is being applied.  Extra       --
			-- triples are ignored.  If less than "ncols" --
			-- triples are provided, the last triple is   --
			-- replicated.                                --
			colw      CDATA        	      #IMPLIED

			-- Column justification, taken from one of    --
			-- l | r | c | d (left, right, centered and   --
			-- decimal), separated by spaces, for each    --
			-- column of the form.  Extras are ignored.   --
			-- If there are fewer than the number of      --
			-- columns specified by the "ncols" attribute --
			-- of the "fstyle" subelement of the "form"   --
			-- to which this "formstyle" is being         --
			-- applied, the last value is replicated.     --
			-- The value defaults to "l" (left justified) --
			colj      CDATA	              #IMPLIED

                        %font-styles;
                        %format-styles; >

-----------------------------------------------------------------------------*/
static SDLContent FormStyleContent[] = {
	{ SDLInitMask(SdlElementFormStyle), CopyTossInfo  },
	{ SDLInitMask(SdlElementNone)     , nullptr          },
    };
static SDLElementAttrList  FormStyleAttrList[] =
    {
	{ SDL_ATTR_CLASSF  , SdlAttrValueRequired  , nullptr           },
	{ SDL_ATTR_SSI     , SdlAttrValueImpliedDef, nullptr           },
	{ SDL_ATTR_RLEVEL  , SdlAttrValueImplied   , NegativeOneStr },
	{ SDL_ATTR_PHRASE  , SdlAttrValueImplied   , nullptr           },
	{ SDL_ATTR_SRCHWT  , SdlAttrValueImplied   , NegativeOneStr },
	{ SDL_ATTR_COLW    , SdlAttrValueImpliedDef, nullptr           },
	{ SDL_ATTR_COLJ    , SdlAttrValueImpliedDef, nullptr           },
	font_stylesAttrList,
	formt_stylesAttrList,
	{ -1               , SdlAttrValueBad       , nullptr }
    };
/*-----------------------------------------------------------------------------
<!ELEMENT frmtstyle - O EMPTY    >
<!ATTLIST frmtstyle     class     (%format-class;)   #REQUIRED
                        ssi       CDATA              #IMPLIED

                        -- the level of the element being described   --
                        rlevel    NUMBER             #IMPLIED

                        phrase    CDATA              #IMPLIED

			-- first line lmargin may be negative but     --
			-- it's a number used to indicate extension   --
			-- or indentation at start                    --
			-- processor should default f-margin to "0"   --
			f-margin  CDATA              #IMPLIED

                        -- likelihood that this element contains an   --
                        -- actual hit when doing a full text search   --
                        srch-wt   NUMBER             #IMPLIED

                        %font-styles;
                        %format-styles;

		        -- applies to the text in the element, not   --
		        -- the element itself.                       --
		        -- prcsr shld dflt justify to "left-justify" --
		        justify  ( left-justify    |
				   right-justify   |
			           center-justify  |
			           numeric-justify ) #IMPLIED >
-----------------------------------------------------------------------------*/
static SDLContent FrmtStyleContent[] = {
	{ SDLInitMask(SdlElementFrmtStyle), CopyTossInfo  },
	{ SDLInitMask(SdlElementNone)     , nullptr          },
    };
static SDLElementAttrList  FrmtStyleAttrList[] =
    {
	{ SDL_ATTR_CLASSF   , SdlAttrValueRequired  , nullptr           },
	{ SDL_ATTR_SSI      , SdlAttrValueImpliedDef, nullptr           },
	{ SDL_ATTR_RLEVEL   , SdlAttrValueImplied   , NegativeOneStr },
	{ SDL_ATTR_PHRASE   , SdlAttrValueImplied   , nullptr           },
	{ SDL_ATTR_SRCHWT   , SdlAttrValueImplied   , NegativeOneStr },
	{ SDL_ATTR_FMARGIN  , SdlAttrValueImplied   , ZeroStr        },
	font_stylesAttrList ,
	formt_stylesAttrList,
	{ SDL_ATTR_JUSTIFY1 , SdlAttrValueImplied   , LeftJustifyStr }, 
	{ -1                , SdlAttrValueBad       , nullptr           }
    };
/*----------------------------------------------------------------------------- <!ELEMENT grphstyle - O EMPTY    >
<!ATTLIST grphstyle     class     (%graphic-class;)  #REQUIRED
			ssi       CDATA              #IMPLIED

			-- the level of the element being described   --
			rlevel    NUMBER             #IMPLIED

			phrase    CDATA              #IMPLIED

			%format-styles;

		        -- applies to the text in the element, not   --
		        -- the element itself.                       --
		        -- prcsr shld dflt justify to "left-justify" --
		        justify  ( left-justify    |
				   right-justify   |
			           center-justify ) #IMPLIED >

] >
-----------------------------------------------------------------------------*/
static SDLContent GrphStyleContent[] = {
	{ SDLInitMask(SdlElementGrphStyle), CopyTossInfo  },
	{ SDLInitMask(SdlElementNone)     , nullptr          },
    };
static SDLElementAttrList  GrphStyleAttrList[] =
    {
	{ SDL_ATTR_CLASSG   , SdlAttrValueRequired  , nullptr           },
	{ SDL_ATTR_SSI      , SdlAttrValueImpliedDef, nullptr           },
	{ SDL_ATTR_RLEVEL   , SdlAttrValueImplied   , NegativeOneStr },
	{ SDL_ATTR_PHRASE   , SdlAttrValueImplied   , nullptr           },
	formt_stylesAttrList,
	{ SDL_ATTR_JUSTIFY2 , SdlAttrValueImplied   , LeftJustifyStr }, 
	{ -1                , SdlAttrValueBad       , nullptr           }
    };

static SDLContent CdataContent[] = {
	{ SDLInitMask(SdlElementCdata), Cdata },
	{ SDLInitMask(SdlElementNone) , nullptr  },
    };

static SDLContent SDLDocumentContent[] = {
	{ SDLInitMask(SdlElementSgml)   , ZeroOrOne },
	{ SDLInitMask(SdlElementDocType), ZeroOrOne },
	{ SDLInitMask(SdlElementSdlDoc) , OnlyOne   },
	{ SDLInitMask(SdlElementNone)   , nullptr      },
    };

/*
 * entries in this structure:
 *    all 'element strings'      must start with '<'
 *    all 'element end strings' must start with '</'
 *
 * If they don't, _DtHelpCeReturnSdlElement will fail
 */
static	SDLElementInfo  SdlElementList[] =
  {
    {"<alttext"  , "</alttext" , 3, 3,
			SdlElementAltText     ,
			SDLInitMask(SdlElementNone),
			nullptr                  , AltTextContent  ,
			False, False, False   , False, SdlToSnb          },

    {AnchorStr   , nullptr        , 4, 0,
			SdlElementAnchor      ,
			SDLInitMask(SdlElementNone),
			AnchorAttrList        , AnchorContent   ,
			False, True , False   , False, SdlToMarker       },

    {AnimateStr  , nullptr        , 4, 0,
			SdlElementAnimate     ,
			SDLInitMask(SdlElementNone),
			NotationAttrList      , nullptr            ,
			False, True , False   , False, SdlToNone         },

    {AudioStr    , nullptr        , 3, 0,
			SdlElementAudio       ,
			SDLInitMask(SdlElementNone),
			NotationAttrList      , nullptr            ,
			False, True , False   , False, SdlToNone         },

    {BlockStr    , "</block"   , 2, 3,
			SdlElementBlock       ,
			SDLInitMask(SdlElementNone),
			BlockAttrList         , BlockContent    ,
			False, True, True     , True , SdlToContainer    },

    {CallbackStr , nullptr        , 3, 0,
			SdlElementCallback    ,
			SDLInitMask(SdlElementNone),
			CallbackAttrList      , CallbackContent ,
			True , False, False   , False, SdlToContainer    },

    {"<cond"     , "</cond"    , 3, 4,
			SdlElementCond        ,
			SDLInitMask(SdlElementNone),
			CondAttrList          , CondContent     ,
			False, True , False   , False, SdlToContainer    },

    {CParaStr    , "</cp"      , 3, 3,
			SdlElementCPara       ,
			SDLInitMask(SdlElementNone),
			ParaAttrList          , CParaContent    ,
			True , True, True     , True , SdlToContainer    },

    {CrossdocStr , nullptr        , 3, 0,
			SdlElementCrossDoc    ,
			SDLInitMask(SdlElementNone),
			IdAndXidAttrList      , CrossDocContent ,
			True , True , False   , False, SdlToContainer    },

    {"<else"     , "</else"    , 3, 4,
			SdlElementElse        ,
			SDLInitMask(SdlElementNone),
			nullptr                  , ThenElseContent ,
			False, False, False   , False, SdlToContainer    },

    {"<entry"    , "</entry"   , 3, 4,
			SdlElementEntry       ,
			SDLInitMask(SdlElementNone),
			EntryAttrList         , EntryContent    ,
			False, True , False   , False, SdlToContainer    },

    {"<fdata"    , "</fdata"   , 3, 4,
			SdlElementFdata       ,
			SDLInitMask(SdlElementNone),
			nullptr                  , FdataContent    ,
			False, False, False   , False, SdlToNone         },

/*
 * when one element is a short version of another, put the longer element
 * before the short element so that tests will check it first.
 */
    {"<formstyle", nullptr        , 6, 0,
			SdlElementFormStyle   ,
			SDLInitMask(SdlElementNone),
			FormStyleAttrList     , FormStyleContent,
			False, True , False   , False, SdlToContainer    },

    {FormStr     , "</form"    , 5, 4,
			SdlElementForm        ,
			SDLInitMask(SdlElementNone),
			FormAttrList          , FormContent     ,
			False, True , False   , True , SdlToContainer    },

    {"<frmtstyle", nullptr        , 4, 0,
			SdlElementFrmtStyle   ,
			SDLInitMask(SdlElementNone),
			FrmtStyleAttrList     , FrmtStyleContent,
			False, True , False   , False, SdlToContainer    },

    {"<frowvec"  , nullptr        , 4, 0,
			SdlElementFrowvec     ,
			SDLInitMask(SdlElementNone),
			FrowvecAttrList       , FrowvecContent  ,
			False, True , False   , False, SdlToNone         },

    {"<fstyle"   , "</fstyle"  , 3, 4,
			SdlElementFstyle      ,
			SDLInitMask(SdlElementNone),
			FstyleAttrList        , FstyleContent   ,
			False, True , False   , False, SdlToNone         },

    { GraphicStr , nullptr        , 4, 0,
			SdlElementGraphic     ,
			SDLInitMask(SdlElementNone),
			NotationAttrList      , GraphicContent ,
			True , True , False   , False, SdlToSnb          },

    {"<grphstyle", nullptr        , 4, 0,
			SdlElementGrphStyle   ,
			SDLInitMask(SdlElementNone),
			GrphStyleAttrList     , GrphStyleContent,
			False, True , False   , False, SdlToContainer    },

/*
 * when one element is a short version of another, put the longer element
 * before the short element so that tests will check it first.
 */
    {"<headstyle", nullptr        , 6, 0,
			SdlElementHeadStyle   ,
			SDLInitMask(SdlElementNone),
			HeadStyleAttrList     , HeadStyleContent,
			False, True , True    , False, SdlToContainer    },

    { HeadStr    , "</head"    , 5, 6,
			SdlElementHead        ,
			SDLInitMask(SdlElementNone),
			HeadAttrList          , HeadContent     ,
			False, True , True    , True , SdlToContainer    },

    {"<id"       , nullptr        , 3, 0,
			SdlElementId          ,
			SDLInitMask(SdlElementNone),
			IdAttrList            , IdContent       ,
			False, True , False   , False, SdlToContainer    },

    {"<if"       , "</if"      , 3, 4,
			SdlElementIf          ,
			SDLInitMask(SdlElementNone),
			nullptr                  , IfContent       ,
			False, False, False   , False, SdlToContainer    },

    {"<index"    , "</index"    , 3, 4,
			SdlElementIndex       ,
			SDLInitMask(SdlElementNone),
			IndexAttrList         , IndexContent    ,
			False, True , False   , False, SdlToContainer    },

    {"<keystyle" , nullptr        , 6, 0,
			SdlElementKeyStyle    ,
			SDLInitMask(SdlElementNone),
			KeyStyleAttrList      , KeyStyleContent ,
			False, True , False   , False, SdlToContainer    },

    {"<key"      , "</key"     , 4, 3,
			SdlElementKey         ,
			SDLInitMask(SdlElementLink),
			KeyAttrList           , KeyContent      ,
			True , True , False   , True , SdlToContainer    },

    {"<link"     , "</link"    , 3, 4,
			SdlElementLink        ,
			SDLInitMask(SdlElementLink),
			LinkAttrList          , LinkContent,
			False, True , False   , False, SdlToContainer    },

    {"<loids"    , "</loids"   , 4, 5,
			SdlElementLoids       ,
			SDLInitMask(SdlElementNone),
			LoidsAttrList         , LoidsContent,
			False, True , False   , False, SdlToContainer    },

    {"<lophrases", "</lophrases", 4, 5,
			SdlElementLoPhrases   ,
			SDLInitMask(SdlElementNone),
			LoPhraseAttrList      , LoPhraseContent,
			False, True , False   , False, SdlToNone         },

    {ManpageStr  , nullptr        , 2, 0,
			SdlElementManPage     ,
			SDLInitMask(SdlElementNone),
			IdAndXidAttrList      , ManPageContent,
			True , True , False   , False, SdlToContainer    },

    {"<notes"    , nullptr        , 2, 0,
			SdlElementNotes       ,
			SDLInitMask(SdlElementNone),
			nullptr                  , nullptr          ,
			False, True , False   , False, SdlToNone         },

    {"<phrase"   , nullptr        , 3, 0,
			SdlElementPhrase      ,
			SDLInitMask(SdlElementNone),
			PhraseAttrList        , nullptr       ,
			False, True , False   , False, SdlToNone         },

    {ParaStr     , "</p"       , 2, 3,
			SdlElementPara        ,
			SDLInitMask(SdlElementNone),
			ParaAttrList          , ParaContent,
			True , True , False   , True , SdlToContainer    },

    {"<refitem"  , "</refitem" , 4, 5,
			SdlElementRefItem     ,
			SDLInitMask(SdlElementNone),
			RefItemAttrList       , RefItemContent,
			False, True , False   , True , SdlToSnb          },

    {"<rel-docs" , nullptr        , 6, 0,
			SdlElementRelDocs     ,
			SDLInitMask(SdlElementNone),
			nullptr                  , nullptr          ,
			False, False, False   , False, SdlToNone         },

    {"<rel-file" , nullptr        , 6, 0,
			SdlElementRelFile     ,
			SDLInitMask(SdlElementNone),
			nullptr                  , nullptr          ,
			False, False, False   , False, SdlToNone         },

    {"<rev"      , "</rev"     , 4, 5,
			SdlElementRev         ,
			SDLInitMask(SdlElementRev),
			nullptr                  , RevContent    ,
			True , False, False   , False, SdlToNone         },

    {ScriptStr   , "</script"  , 3, 4,
			SdlElementScript      ,
			SDLInitMask(SdlElementNone),
			ScriptAttrList        , ScriptContent ,
			False, True , False   , False, SdlToSnb          },

    {"<sdldoc"   , "</sdldoc"  , 3, 4,
			SdlElementSdlDoc      ,
			SDLInitMask(SdlElementNone),
			SdlDocAttrList        , SdlDocContent ,
			False, True , False   , False, SdlToContainer    },

    {"<snb"      , "</snb"     , 4, 5,
			SdlElementSnb         ,
			SDLInitMask(SdlElementNone),
			SnbAttrList           , SnbContent    ,
			False, True , False   , False, SdlToContainer    },

    {SnrefStr    , "</snref"   , 4, 5,
			SdlElementSnRef       ,
			SDLInitMask(SdlElementNone),
			SnrefAttrList         , SnrefContent  ,
			False, True , False   , False, SdlToContainer    },

    {"<spc"      , nullptr        , 4, 0,
			SdlElementSpc         ,
			SDLInitMask(SdlElementNone),
			SpcAttrList           , SpcContent    ,
			False, True , False   , False, SdlToSpc          },

    {"<sphrase"  , "</sphrase>", 4, 4,
			SdlElementSphrase     ,
			SDLInitMask(SdlElementNone),
			SphraseAttrList       , SphraseContent ,
			False, True , False   , True , SdlToContainer    },

    { SubHeadStr , "</subhead" , 3, 4,
			SdlElementSubHead     ,
			SDLInitMask(SdlElementNone),
			HeadAttrList          , SubHeadContent,
			False, True , True    , True , SdlToContainer    },

    { SwitchStr  , "</switch"  , 3, 4,
			SdlElementSwitch      ,
			SDLInitMask(SdlElementNone),
			SwitchAttrList        , SwitchContent ,
			True , True , False   , False, SdlToContainer    },

    {SyscmdStr   , nullptr        , 3, 0,
			SdlElementSysCmd      ,
			SDLInitMask(SdlElementNone),
			SysCmdAttrList        , SysCmdContent ,
			True , True , False   , False, SdlToContainer    },

    {TextfileStr , nullptr        , 6, 0,
			SdlElementTextFile    ,
			SDLInitMask(SdlElementNone),
			TextFileAttrList      , TextFileContent,
			True , True , False   , False, SdlToContainer    },

    {TextStr     , "</text"    , 4, 4,
			SdlElementText        ,
			SDLInitMask(SdlElementNone),
			TextAttrList          , TextContent   ,
			True , False, False   , False, SdlToSnb          },

    {"<then"     , "</then"    , 3, 4,
			SdlElementThen        ,
			SDLInitMask(SdlElementNone),
			nullptr                  , ThenElseContent,
			False, False, False   , False, SdlToContainer    },

    {"<toss"     , "</toss"    , 3, 4,
			SdlElementToss        ,
			SDLInitMask(SdlElementNone),
			TossAttrList          , TossContent   ,
			False, True , False   , False, SdlToContainer    },

    {VideoStr    , nullptr        , 4, 0,
			SdlElementVideo       ,
			SDLInitMask(SdlElementNone),
			NotationAttrList      , nullptr          ,
			False, True , False   , False, SdlToNone         },

    {VirpageStr  , "</virpage" , 4, 4,
			SdlElementVirpage     ,
			SDLInitMask(SdlElementNone),
			VirpageAttrList       , VirpageContent,
			False, True , False   , True , SdlToContainer    },

    {VirpageStr  , "</virpage" , 4, 4,
			SdlElementTitle     ,
			SDLInitMask(SdlElementNone),
			VirpageAttrList       , VirpageHeadContent,
			False, True , False   , True , SdlToContainer    },

    {"<vstruct"  , "</vstruct" , 3, 4,
			SdlElementVStruct     ,
			SDLInitMask(SdlElementNone),
			VStructAttrList       , VStructContent,
			False, True , False   , False, SdlToNone         },

    {"<!--"      , nullptr        , 3, 0,
			SdlElementComment     ,
			SDLInitMask(SdlElementNone),
			nullptr                  , nullptr          ,
			False, True , False   , False, SdlToNone         },

    {"<!doctype" , nullptr        , 3, 0,
			SdlElementDocType     ,
			SDLInitMask(SdlElementNone),
			nullptr                  , nullptr          ,
			False, True , False   , False, SdlToNone         },

    {"<!sgml"    , nullptr        , 3, 0,
			SdlElementSgml        ,
			SDLInitMask(SdlElementNone),
			nullptr                  , nullptr          ,
			False, True , False   , False, SdlToNone         },

    { nullptr       , nullptr        , 0, 0,
			SdlElementCdata       ,
			SDLInitMask(SdlElementNone),
			nullptr                  , CdataContent  ,
			False, False, False   , False, SdlToNone         },
  };

static	int MaxSDLElements = sizeof (SdlElementList) / sizeof (SDLElementInfo);


/******************************************************************************
 * Private Macros
 *****************************************************************************/
#define	MyFree(x)	if ((char *)(x) != NULL) free(x)
#define	SaveRestoreMask(x,y) \
		{ int myI;\
		  for (myI = 0; myI < SDL_MASK_LEN; myI++) \
		      x[myI] = y[myI]; \
		}
#define	MergeMasks(x,y) \
		{ int myI;\
		  for (myI = 0; myI < SDL_MASK_LEN; myI++) \
		      x[myI] |= y[myI]; \
		}
#define	RemoveMasks(x,y) \
		{ int myI;\
		  for (myI = 0; myI < SDL_MASK_LEN; myI++) \
		      x[myI] &= (~(y[myI])); \
		}
#define	MaskToValue(x,y) \
		{ int myI;\
		  SDLMask myMask;\
		  for (myI = 0, y = 0; myI < SDL_MASK_LEN; myI++) \
		      if (x[myI] == 0) \
		          y += SDL_MASK_SIZE; \
		      else \
			{ \
			  myMask = x[myI]; \
			  while (myMask > 1) \
			    { \
				myMask = myMask >> 1; y++; \
			    } \
			  myI = SDL_MASK_LEN; \
			} \
		}
#define	ClearAttrFlag(pa, attr) \
		{ \
		  int  mask = attr & VALUE_MASK; \
		  int  flag = ~(attr & (~VALUE_MASK)); \
 \
		  if (mask == ENUM_VALUE) \
		      (pa).enum_values = (pa).enum_values & flag; \
		  else if (mask == NUMBER_VALUE) \
		      (pa).num_values = (pa).num_values & flag; \
		  else if (mask == STRING1_VALUE) \
		      (pa).str1_values = (pa).str1_values & flag; \
		  else if (mask == STRING2_VALUE) \
		      (pa).str2_values = (pa).str2_values & flag; \
		}

/******************************************************************************
 *
 * Private Functions
 *
 *****************************************************************************/
/******************************************************************************
 * Function: CompressTable
 *
 *****************************************************************************/
static _DtCvSegment *
CompressTable(
    FormatStruct	*my_struct,
    _DtCvSegment	*seg)
{
    _DtCvSegment  *retSeg = seg;
    _DtCvSegment **tableSegs;
    char         **rowIds;

    /*
     * make sure we're working with a non-null segment.
     */
    if (nullptr != seg)
      {
	/*
	 * get the row ids.
	 */
	rowIds = _DtCvCellIdsOfTableSeg(seg);

	/*
	 * if there is only one column and zero to one rows,
	 * compress out the table
	 */
	if (1 == _DtCvNumColsOfTableSeg(seg) &&
		(nullptr == rowIds || nullptr == *rowIds || nullptr == rowIds[1]))
	  {
	    /*
	     * get the list of table segments.
	     */
	    tableSegs = _DtCvCellsOfTableSeg(seg);

	    /*
	     * now find the correct segment. Start out with a null return.
	     */
	    retSeg = nullptr;
	    if (nullptr != rowIds && nullptr != *rowIds)
	      {
		_DtCvSegment *prevSeg = nullptr;

		/*
		 * look through the table's list for the one segment
		 * to fill the one column/row table.
		 */
		while (nullptr != tableSegs && nullptr != *tableSegs &&
		   _DtCvStrCaseCmpLatin1(*rowIds, _DtCvContainerIdOfSeg(*tableSegs)))
		  {
		    prevSeg = *tableSegs;
		    tableSegs++;
		  }

		/*
		 * does the segment exist in the table's list?
		 */
		if (nullptr != tableSegs && nullptr != *tableSegs)
		  {
		    /*
		     * set the return value.
		     */
		    retSeg = *tableSegs;

		    /*
		     * now propagate the justification.
		     */
		    if (_DtCvINHERIT == _DtCvContainerJustifyOfSeg(retSeg)
				&& nullptr != _DtCvColJustifyOfTableSeg(seg))
			_DtCvContainerJustifyOfSeg(retSeg) =
					*(_DtCvColJustifyOfTableSeg(seg));

		    /*
		     * now move up any other table segments, whereby
		     * eliminating this segment from the list so it
		     * won't be freed via _DtHelpFreeSegments().
		     */
		    while (nullptr != *tableSegs)
		      {
			*tableSegs = tableSegs[1];
			tableSegs++;
		      }
		  }
	      }

	    /*
	     * Now free the table segment
	     */
	    _DtHelpFreeSegments(seg, _DtCvFALSE,
				my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);
	  }
      }

    return retSeg;
}

/******************************************************************************
 * Function: PropagateJustification
 *
 *****************************************************************************/
static void
PropagateJustification(
    _DtCvSegment	 *seg,
    _DtCvFrmtOption	  justify)
{
    if (nullptr != seg && _DtCvIsSegContainer(seg) &&
			_DtCvINHERIT == _DtCvContainerJustifyOfSeg(seg))
	_DtCvContainerJustifyOfSeg(seg) = justify;
}

/******************************************************************************
 * Function:    void FreeAttributes (enum SdlElement element,
 *                              CESDLAttrStruct *attributes)
 *
 * Parameters:
 *
 * Returns:     
 *
 * Purpose:
 *
 ******************************************************************************/
static void
FreeAttributes(
    enum SdlElement	 element,
    ElementInfo		*cur_info,
    _DtHelpFontHints	*font_specs)
{
    int i = 0;
    const SDLElementAttrList *myList;
    const SDLAttribute	*attrib;
    char		*varOffset;
    char		**strPtr;

    while (i < MaxSDLElements && SdlElementList[i].sdl_element != element)
	i++;

    myList = SdlElementList[i].attrib_list;

    if (myList != nullptr &&
		(cur_info->str1_values != 0 || cur_info->str2_values != 0))
      {
	while (myList->sdl_attr_define != -1)
	  {
	    if (SDLIsStrAttrSet(*cur_info, myList->sdl_attr_define) &&
		SDLIsAttrSet(*cur_info, myList->sdl_attr_define))
	      {
		attrib = SDLAttributeList;
		while (attrib->sdl_attr_define != -1 &&
			!ATTRS_EQUAL(attrib->data_type,
					myList->sdl_attr_define,
					attrib->sdl_attr_define))
		    attrib++;

		if (attrib->struct_type == SdlFontSpecific ||
				attrib->struct_type == SdlElementSpecific)
		  {
		    if (attrib->struct_type == SdlFontSpecific)
			varOffset = ((char *) font_specs);
		    else if (attrib->struct_type == SdlElementSpecific)
			varOffset = ((char *) cur_info);

		    varOffset = varOffset + attrib->field_ptr;
		    strPtr    = (char **) varOffset;

		    if (nullptr != *strPtr)
			free (*strPtr);
		  }
	      }
	    myList++;
	  }
      }
}
/******************************************************************************
 * Function: DuplicateElement
 *
 *****************************************************************************/
static _DtCvSegment *
DuplicateElement(
    _DtCvPointer	  client_data,
    _DtCvSegment	 *toss,
    _DtCvSegment	 *src_seg,
    _DtCvSegment	**prev_data,
    _DtCvSegment	**last_seg,
    _DtHelpLoadFontProc   load_font,
    _DtHelpResolveSpcProc resolve_spc,
    _DtCvUnit		  line_height,
    int			  ave_char,
    _DtHelpFontHints	  srcFonts,
    int			  link_idx,
    _DtCvValue		  vis_link)
{
    int			 oldIndex = link_idx;
    _DtCvSegment	*topSeg   = nullptr;
    _DtCvSegment	*prevData = nullptr;
    _DtCvSegment	*lastSeg  = nullptr;
    _DtCvSegment	*newSeg;
    _DtHelpFontHints	 curFonts;
    SdlMatchData	*match;
    _DtHelpDARegion	*region;
    FrmtPrivateInfo	*priv;

    if (nullptr != prev_data)
	prevData = *prev_data;

    if (nullptr != last_seg)
	lastSeg = *last_seg;

    while (src_seg != nullptr)
      {
	curFonts   = srcFonts;
	link_idx = oldIndex;
	if (_DtCvIsSegHyperText(src_seg) || _DtCvIsSegGhostLink(src_seg))
	  {
	    link_idx = src_seg->link_idx;
	    if (_DtCvIsSegHyperText(src_seg))
		vis_link = True;
	  }

	if (_DtHelpCeAllocSegment(1, nullptr, nullptr, &newSeg) != 0)
	    return nullptr;

	/*
	 * copy over the information. (But don't blow away the private
	 * information, except - keep the dup flag.)
	 */
	priv    = FrmtPrivInfoPtr(newSeg);
	*newSeg = *src_seg;
	newSeg->client_use = priv;
	priv->dup_flag     = FrmtPrivInfoPtr(src_seg)->dup_flag;

	/*
	 * set the duplicate on the source seg. That way, allocated memory
	 * now belongs to the new segment.
	 */
	SetDupFlag(src_seg);

	/*
	 * now re-set some information pertinent to this flow of control
	 */
	newSeg->link_idx = link_idx;
	newSeg->type       = ClearSegLinks(newSeg);
	if (link_idx != -1)
	  {
	    if (vis_link == True)
		newSeg->type = _DtCvSetTypeToHyperText(newSeg->type);
	    else
		newSeg->type = _DtCvSetTypeToGhostLink(newSeg->type);
	  }
	_DtCvNextSeg(newSeg) = nullptr;
	newSeg->next_disp  = nullptr;

	/*
	 * put this segment into the list.
	 */
	if (nullptr != lastSeg)
	    _DtCvNextSeg(lastSeg) = newSeg;

	/*
	 * type cast the internal structure to a match data struct
	 * now, since more than one element may use it.
	 */
	match = SegMatchDataPtr(src_seg);

	switch (_DtCvPrimaryTypeOfSeg(newSeg))
	  {
	    case _DtCvCONTAINER:
		/*
		 * merge the attributes into the new elements.
		 */
		if (nullptr != match)
		  {
		    if (nullptr != toss)
		      {
			_DtHelpCeMergeSdlAttribInfo(
						_DtHelpCeMatchSemanticStyle(
							toss,
							match->clan,
							match->level,
							match->ssi),
						newSeg,
						&curFonts,
						nullptr,
						nullptr,
						nullptr);
			/*
			 * set the margins to absolute values
			 */
			if (ave_char > 0)
			  {
			    _DtCvContainerLMarginOfSeg(newSeg) =
				_DtCvContainerLMarginOfSeg(newSeg) * ave_char;
			    _DtCvContainerRMarginOfSeg(newSeg) =
				_DtCvContainerRMarginOfSeg(newSeg) * ave_char;
			    _DtCvContainerFMarginOfSeg(newSeg) =
				_DtCvContainerFMarginOfSeg(newSeg) * ave_char;
			  }
			/*
			 * set the top and bottom margins to absolute values
			 */
			if (line_height > 0)
			  {
			    TMarginOfSeg(newSeg) = 
					TMarginOfSeg(newSeg) * line_height;
			    BMarginOfSeg(newSeg) =
					BMarginOfSeg(newSeg) * line_height;
			  }
		      }
		  }
		_DtCvContainerListOfSeg(newSeg) = DuplicateElement(
					client_data,
					toss,
					_DtCvContainerListOfSeg(src_seg),
					&prevData,
					&lastSeg,
					load_font,
					resolve_spc,
					line_height,
					ave_char,
					curFonts,
					link_idx,
					vis_link);

		if (nullptr == _DtCvContainerListOfSeg(newSeg))
		  {
		    free(newSeg);
		    return nullptr;
		  }

		/*
		 * if this is a wrapper, throw it away
		 */
		if (IsSdlWrapper(newSeg->type))
		  {
		    _DtCvSegment *tmpSeg = newSeg;

		    newSeg = _DtCvContainerListOfSeg(newSeg);
		    free(tmpSeg);
		  }
		break;

	    case _DtCvSTRING:
		/*
		 * now load the font for this element
		 */
		(load_font)(client_data,
					_DtHelpFontHintsLang(curFonts),
					_DtHelpFontHintsCharSet(curFonts),
					curFonts,
					&(_DtCvFontOfStringSeg(newSeg)));

		/*
		 * put this segment in the display list.
		 */
		if (nullptr != prevData)
		    prevData->next_disp = newSeg;

		prevData = newSeg;
		break;

	    case _DtCvREGION:
		region = (_DtHelpDARegion *) _DtCvInfoOfRegionSeg(src_seg);
		if (False == region->inited)
		  {
		    _DtHelpDASpcInfo *spcInfo =
					(_DtHelpDASpcInfo *) region->handle;

		    (resolve_spc)(client_data,
					_DtHelpFontHintsLang(curFonts),
					_DtHelpFontHintsCharSet(curFonts),
					curFonts,
					spcInfo->name,
					&(_DtCvInfoOfRegionSeg(newSeg)),
					&(_DtCvWidthOfRegionSeg(newSeg)),
					&(_DtCvHeightOfRegionSeg(newSeg)),
					&(_DtCvAscentOfRegionSeg(newSeg)));

		    ClearDupFlag(src_seg);
		  }

		/*
		 * check for in-line flags on this item.
		 */
		if (nullptr != match &&
			(SdlClassInLine == match->clan ||
			 SdlClassButton == match->clan ||
			 SdlClassIcon   == match->clan))
		    newSeg->type = _DtCvSetTypeToInLine(newSeg->type);

		/*
		 * put this segment in the display list.
		 */
		if (nullptr != prevData)
		    prevData->next_disp = newSeg;

		prevData = newSeg;
		break;
	  }

	if (topSeg == nullptr)
	    topSeg = newSeg;

	lastSeg = newSeg;
	src_seg = _DtCvNextSeg(src_seg);
      }

    if (nullptr != prev_data)
	*prev_data = prevData;

    if (nullptr != last_seg)
	*last_seg = lastSeg;

    return topSeg;
}

/******************************************************************************
 * Function:    _DtCvSegment *ResolveSnref (FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	_DtCvSegment *
ResolveSnref(
    FormatStruct	*my_struct,
    _DtCvSegment	*snref,
    char		*snref_id)
{
    _DtCvValue	 found      = False;
    _DtCvSegment	*snbEntry   = nullptr;
    _DtCvSegment	*altText    = nullptr;
    _DtCvSegment	*newSeg     = nullptr;
    _DtCvSegment	*refItem    = _DtCvContainerListOfSeg(snref);
    FrmtPrivateInfo	*priv;
    _DtHelpFontHints	 saveFonts  = *(my_struct->my_fonts);

    /*
     * Check to see if a snb has been found yet. If not, put off resolving
     * refitems.
     */
    if (nullptr != my_struct->snb)
      {
	/*
	 * check each refitem to see if it can be resolved
	 */
	while (False == found && nullptr != refItem)
	  {
	    /*
	     * assume the refitem will be found in the snb
	     */
	    found    = True;
	    snbEntry = nullptr;
	    if (nullptr != _DtCvContainerIdOfSeg(refItem))
	        snbEntry = FindSnbEntry(my_struct->snb,
					_DtCvContainerIdOfSeg(refItem));
	    else
		/* the <alttext> container */
		altText  = refItem;

	    if (nullptr == snbEntry)
	      {
		/*
		 * didn't find the refitem, so set the flag
		 * for another loop.
		 */
		found   = False;
		refItem = _DtCvNextSeg(refItem);
	      }
	  }

	/*
	 * was a system notation block item found?
	 */
	if (True == found)
	  {
	    /*
	     * an refitem can be a region (graphic, audio, video, animate),
	     * text or a script. If a script, need to process it differently.
	     */
	    priv = FrmtPrivInfoPtr(snbEntry);
	    if (SdlElementScript == priv->sdl_el_type)
	      {
		/*
		 * NULL variables, get the interpretor.
		 */
		SdlMatchData *match;
		char *runData;
		char *newData   = nullptr;
		char *interpStr = GetInterpCmd((SdlOption) (priv->interp));

		/*
		 * get the data to run
		 */
	        runData = _DtCvStringOfStringSeg(
					_DtCvContainerListOfSeg(snbEntry));

		if (nullptr == interpStr
			|| -1 == _DtCvRunInterp(my_struct->ui_info->exec_filter,
					my_struct->ui_info->client_data,
					interpStr, runData, &newData)
			|| nullptr == newData || 0 == strlen(newData))
		  {
		    free(newData);
		    return nullptr;
		  }

		/*
		 * set the match data for toss lookup
		 */
		match = SegMatchDataPtr(refItem);

		if (nullptr != match && nullptr != my_struct->toss)
		    _DtHelpCeMergeSdlAttribInfo(
					_DtHelpCeMatchSemanticStyle(
							my_struct->toss,
							match->clan,
							match->level,
							match->ssi),
					newSeg,
					my_struct->my_fonts,
					nullptr,
					nullptr,
					nullptr);

		if (0 != MySaveString(&newSeg, my_struct, newData,
					my_struct->cur_link, my_struct->mb_len,
					False))
		  {
		    free(newData);
		    return nullptr;
		  }

		free(newData);
	      }
	    else
	      {
	        /*
	         * strip the container wrapper from the snbEntry before
	         * duplication.
	         *
	         * Also, include the toss match data for this segment
	         * in the snbEntry.
	         */
	        snbEntry = _DtCvContainerListOfSeg(snbEntry);
		if (nullptr != snbEntry)
		  {
	            SegMatchData(snbEntry) = SegMatchData(refItem);

	            newSeg = DuplicateElement(my_struct->ui_info->client_data,
					my_struct->toss,
					snbEntry,
					&(my_struct->prev_data),
					nullptr,
					my_struct->ui_info->load_font,
					my_struct->ui_info->resolve_spc,
					my_struct->ui_info->line_height / 2,
					my_struct->ui_info->avg_char,
					*(my_struct->my_fonts),
					my_struct->cur_link, True);

	            SegMatchData(snbEntry) = nullptr;

	            if (nullptr == newSeg)
		        return nullptr;

	            /*
	             * if the only item in the list for the snb entry is
	             * a region, then assume it is a graphic, video, etc.
	             * Check to see if the region is in-line or a figure.
	             * If in-line, ignore any heads with the refitem. If a
	             * figure, include them (they may be a caption to the
	             * region).
	             */
	            if (_DtCvIsSegRegion(newSeg) && nullptr == _DtCvNextSeg(newSeg))
	              {
		        SdlMatchData *info = SegMatchDataPtr(refItem);

		        /*
		         * is this a figure?
		         */
		        if (SdlClassFigure == info->clan)
		          {
		            _DtCvNextSeg(newSeg) = _DtCvContainerListOfSeg(refItem);
		            _DtCvContainerListOfSeg(refItem) = nullptr;
		          }
		      }
		  }
	      }
	  }

	/*
	 * was there alternate text?
	 */
	else if (nullptr != altText && nullptr != _DtCvContainerListOfSeg(altText))
	  {
	    /*
	     * the new segment becomes the contents of the <alttext>
	     * container - why copy? Just null the <alttext> pointer
	     * to prevent the free from destroying the contents.
	     */
	    newSeg = _DtCvContainerListOfSeg(altText);
	    _DtCvContainerListOfSeg(altText) = nullptr;

	    /*
	     * load the font for this snref.
	     */
	    (my_struct->ui_info->load_font)(my_struct->ui_info->client_data,
				_DtHelpFontHintsLang(*(my_struct->my_fonts)),
				_DtHelpFontHintsCharSet(*(my_struct->my_fonts)),
				*(my_struct->my_fonts),
				&(_DtCvFontOfStringSeg(newSeg)));
	  }

        if (my_struct->prev_data != nullptr)
            my_struct->prev_data->next_disp = newSeg;

        my_struct->last_was_space = False;
        my_struct->last_was_mb    = False;
        my_struct->last_was_nl    = False;
	if (nullptr != newSeg)
            my_struct->prev_data      = newSeg;
      }
    else if (_DtCvTRUE == my_struct->save_snref)
      {
	int count = my_struct->snref_cnt;
	UnresSnref *new_snrefs;

	if (nullptr == my_struct->un_snrefs)
	    new_snrefs = (UnresSnref *) malloc (sizeof(UnresSnref));
	else
	    new_snrefs = (UnresSnref *) realloc (
					(void *) my_struct->un_snrefs,
					(sizeof(UnresSnref) * (count + 1)));
	if (nullptr != new_snrefs)
	  {
	    my_struct->un_snrefs = new_snrefs;
	    my_struct->un_snrefs[count].id       = snref_id;
	    my_struct->un_snrefs[count].seg      = snref;
	    my_struct->un_snrefs[count].cur_link = my_struct->cur_link;
	    my_struct->un_snrefs[count].fonts    = *(my_struct->my_fonts);

	    my_struct->snref_cnt++;

	    /*
	     * return the snref segment as our new segment to act as a
	     * place holder, otherwise it gets squeezed
	     * out.
	     */
	    newSeg = snref;

            if (my_struct->prev_data != nullptr)
                my_struct->prev_data->next_disp = newSeg;

            my_struct->prev_data      = newSeg;

	    /*
	     * make the snref a noop so it gets the proper next/last
	     * display pointers.
	     */
	    newSeg->type = _DtCvSetTypeToNoop(newSeg->type);
	  }
      }

    *(my_struct->my_fonts) = saveFonts;
    return newSeg;

} /* End ResolveSnref */

/******************************************************************************
 * Function:    static CEFontSpecs AllocateFontInfo(my_struct);
 *
 * Parameters:
 *
 * Returns:     non-null  if successful, NULL if errors
 *
 * Purpose:     Allocates a FontInfo structure and intializes it to
 *              the current fonts hints.
 *
 ******************************************************************************/
static	_DtHelpFontHints *
AllocateFontInfo(
    FormatStruct *my_struct)
{
    _DtHelpFontHints *retFont = nullptr;

    if (my_struct->free_cnt > 0)
	retFont = my_struct->free_fonts[--my_struct->free_cnt];
    else
      {
        /*
         * malloc and initialize the starting font information
         */
        retFont = (_DtHelpFontHints *) malloc (sizeof(_DtHelpFontHints));

        if (nullptr == retFont)
	    return nullptr;
      }

    /*
     * initialize the font structure
     */
    if (nullptr != my_struct->my_fonts)
	*retFont = *(my_struct->my_fonts);
    else
        *retFont = DefFontInfo;

    return retFont;
}

/******************************************************************************
 * Function:    static _DtCvSegment **ConvertToList(segs);
 *
 * Parameters:
 *
 * Returns:     non-null  if successful, NULL if errors
 *
 * Purpose:     Allocates a list of null terminated _DtCvSegment pointers.
 *
 ******************************************************************************/
static	_DtCvSegment **
ConvertToList(_DtCvSegment *segs)
{
    _DtCvSegment **ptr  = nullptr;

    /*
     * count the segments
     */
    while (nullptr != segs)
      {
	_DtCvSegment *next;

	ptr = (_DtCvSegment **) _DtCvAddPtrToArray ((void **) ptr,
							(void *) segs);
	next = _DtCvNextSeg(segs);
	_DtCvNextSeg(segs) = nullptr;
	segs = next;
      }

    return (ptr);
}

/******************************************************************************
 * Function:    static int DestroyFontInfo(my_struct);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Places the font structure in the free list for re-use.
 *
 ******************************************************************************/
static	void
DestroyFontInfo(
    FormatStruct *my_struct)
{
    /*
     * free the font structures allocated
     */
    while (0 < my_struct->free_cnt)
      {
	my_struct->free_cnt--;
	free(my_struct->free_fonts[my_struct->free_cnt]);
      }

    if (nullptr != my_struct->free_fonts)
	free(my_struct->free_fonts);

    my_struct->free_fonts = nullptr;

    if (nullptr != my_struct->my_fonts)
	free(my_struct->my_fonts);
}

/******************************************************************************
 * Function:    static int FreeFontInfo(my_struct);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Places the font structure in the free list for re-use.
 *
 ******************************************************************************/
static	int
FreeFontInfo(
    FormatStruct *my_struct)
{
    if (my_struct->free_cnt >= my_struct->free_max)
      {
	int new_max = my_struct->free_max + GROW_SIZE;
	_DtHelpFontHints **new_fonts;

	if (nullptr != my_struct->free_fonts)
            new_fonts = (_DtHelpFontHints **) realloc (
			my_struct->free_fonts,
			sizeof(_DtHelpFontHints *) * new_max);
	else
            new_fonts = (_DtHelpFontHints **) malloc (
			sizeof(_DtHelpFontHints *) * new_max);

        if (nullptr == new_fonts)
	    return -1;

	my_struct->free_fonts = new_fonts;
	my_struct->free_max = new_max;
      }

    my_struct->free_fonts[my_struct->free_cnt++] = my_struct->my_fonts;

    return 0;
}

/******************************************************************************
 * Function:    static int ResolveAsyncBlock(my_struct, table_seg);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     
 *
 ******************************************************************************/
static	int
ResolveAsyncBlock(
    FormatStruct	*my_struct,
    _DtCvSegment	*table_seg)
{
    char        c;
    char       *next;
    char       *start  = nullptr;
    char      **rowIds = _DtCvCellIdsOfTableSeg(table_seg);
    _DtCvFrmtOption *colJ;
    _DtCvSegment **cellSegs;
    _DtCvSegment  *newSeg;
    _DtCvSegment  *asyncSegs;
    _DtCvValue	found;

    while (nullptr != rowIds && nullptr != *rowIds)
      {
	/*
	 * get the start of the row ids
	 */
	next = *rowIds;
	colJ = _DtCvColJustifyOfTableSeg(table_seg);
	while (nullptr != next && '\0' != *next)
	  {
	    /*
	     * skip leading spaces
	     */
	    while (' ' == *next) next++;

	    /*
	     * set the starting pointer.
	     */
	    start = next;

	    /*
	     * skip to the next delimitter.
	     */
	    while (' ' != *next && '\0' != *next) next++;

	    /*
	     * check for the id in the set of segments.
	     */
	    if (start != next)
	      {
		/*
		 * save and replace the last character with a null byte.
		 */
		c = *next;
		*next = '\0';

		/*
		 * get the cells in the table
		 */
		cellSegs = _DtCvCellsOfTableSeg(table_seg);

		/*
		 * check that one of these cells matches the id
		 */
		found   = False;
		if (nullptr != cellSegs)
		  {
		    while (False == found && nullptr != *cellSegs)
		      {
		        if (_DtCvStrCaseCmpLatin1(
					_DtCvContainerIdOfSeg(*cellSegs),
								start) == 0)
			    found = True;
		        else
			    cellSegs++;
		      }
		    newSeg = *cellSegs;
		  }

		/*
		 * the id did not match any of the cells in the table
		 * Look in the async list.
		 */
		if (False == found)
		  {
		    asyncSegs = my_struct->async_blks;
		    while (False == found && nullptr != asyncSegs)
		      {
		        if (_DtCvStrCaseCmpLatin1(
					_DtCvContainerIdOfSeg(asyncSegs),
					start) == 0)
			    found = True;
			else
			    asyncSegs = _DtCvNextSeg(asyncSegs);
		      }

		    /*
		     * found one in the aysnc blocks
		     * Duplicate it.
		     */
		    if (True == found)
		      {
			_DtCvSegment *lastSrc  = nullptr;
			_DtCvSegment *lastNext = nullptr;

			/*
			 * break the link to the next segment
			 * or we'll copy this segment and all
			 * the next segments.
			 */
			_DtCvSegment *nextSeg = _DtCvNextSeg(asyncSegs);
			_DtCvNextSeg(asyncSegs) = nullptr;

			/*
			 * allocate a new segment for this async block.
			 */
			newSeg = DuplicateElement(
					my_struct->ui_info->client_data,
					my_struct->toss,
					asyncSegs, &lastSrc, &lastNext,
					my_struct->ui_info->load_font,
					my_struct->ui_info->resolve_spc,
					my_struct->ui_info->line_height / 2,
					my_struct->ui_info->avg_char,
					*(my_struct->my_fonts),
					-1, False);

			/*
			 * restore the async seg linked list
			 */
			_DtCvNextSeg(asyncSegs) = nextSeg;

			/*
			 * check to see if anything was copied
			 */
			if (nullptr == newSeg)
			    return -1;
			/*
			 * now place this segment in the list.
			 */
			_DtCvCellsOfTableSeg(table_seg) = (_DtCvSegment **)
				_DtCvAddPtrToArray(
				(void **) _DtCvCellsOfTableSeg(table_seg),
					(void *) newSeg);
		      }
		  }
	       /*
		* replace the character.
		*/
	       *next = c;
	      }

	    colJ++;
	  }

	/*
	 * check the next row
	 */
	rowIds++;
      }

    return 0;
}

/******************************************************************************
 * Function:    static int FindSnb(my_struct, id);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     
 *
 ******************************************************************************/
static	_DtCvSegment *
FindSnbEntry(
    _DtCvSegment	 *snb,
    char	 *target)
{
    char      *id;
    _DtCvSegment *retEntry;

    if (nullptr != snb)
      {
	retEntry = _DtCvContainerListOfSeg(snb);
	while (nullptr != retEntry)
	  {
	    id = nullptr;
	    if (_DtCvIsSegContainer(retEntry))
		id = _DtCvContainerIdOfSeg(retEntry);

	    if (nullptr != id && _DtCvStrCaseCmpLatin1(target, id) == 0)
		return retEntry;

	    retEntry = _DtCvNextSeg(retEntry);
	  }
      }

    return nullptr;
}

/* A little helper function, acts like strcpy
 * but safe for overlapping regions.
 */
static void *strmove(void *dest, const void *src) {
    return memmove(dest, src, strlen(src) + 1);
}

/******************************************************************************
 * Function:    static int ProcessString(string, int idx);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     
 *
 ******************************************************************************/
static	int
ProcessString(
    FormatStruct	 *my_struct,
    _DtCvValue		  last_flag,
    _DtCvValue		  nl_flag,
    _DtCvValue		  mod_str,
    _DtCvValue		  cpy_str,
    _DtCvValue		  process_flag,
    char		 *string,
    int			  byte_len,
    int			 *idx)
{
    if (mod_str == True)
        string[*idx] = '\0';

    if (process_flag == True && MySaveString(&(my_struct->seg_list),
				    my_struct, string, my_struct->cur_link,
                                    byte_len, nl_flag) != 0)
      {
        MyFree(string);
        return -1;
      }

    my_struct->last_was_space = last_flag;

    if (cpy_str == True)
      {
        strmove (string, &string[*idx+1]);
        *idx = -1;
      }
    return 0;
}

/******************************************************************************
 * Function:    static void CompressLinkSeg(seg);
 *
 * Parameters:
 *
 * Returns:     nothing
 *
 * Purpose:     Check to see if there is a blank at the beginning of the
 *              segment and if it is needed.
 *
 ******************************************************************************/
static	void
CompressLinkSeg(
    _DtCvSegment *p_seg)
{
    void    *pChar;
    int      wcFlag;

    /*
     * check to see if the first segment is a string.
     */
    while (nullptr != p_seg && _DtCvIsSegNoop(p_seg))
	p_seg = _DtCvNextSeg(p_seg);

    if (nullptr == p_seg)
	return;

    if (_DtCvIsSegString(p_seg))
      {
	wcFlag = _DtCvIsSegWideChar(p_seg);
	pChar  = _DtCvStrPtr(_DtCvStringOfStringSeg(p_seg), wcFlag, 0);

	/*
	 * is this the only segment in the link?
	 * Is it only one character in size?
	 */
	if (nullptr == _DtCvNextDisp(p_seg) && 1 <= _DtCvStrLen(pChar, wcFlag))
	    return;

	/*
	 * is it a blank? (wide char and single char codes are equal
	 * for blanks - ISO standard)
	 */
	if ((_DtCvIsSegWideChar(p_seg) && ' ' == *((wchar_t *) pChar)) ||
		(_DtCvIsSegRegChar(p_seg) && ' ' == *((char *) pChar)))
	  {
	    /*
	     * compress out the blank
	     *
	     * is this a single byte string? If so, use strcpy to move
	     * the string.
	     */
	    if (_DtCvIsSegRegChar(p_seg))
		strmove(((char *)pChar), &(((char *)pChar)[1]));
	    else
	      {
		wchar_t *wcChar = (wchar_t *) pChar;

		while (0 != wcChar[0])
		  {
		    wcChar[0] = wcChar[1];
		    wcChar++;
		  }
	      }
	  }
      }
}

/******************************************************************************
 * Function:    static int ProcessNonBreakChar(string, int idx);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 *
 ******************************************************************************/
static  int
ProcessNonBreakChar(
    FormatStruct	*my_struct,
    _DtCvValue		 process_flag,
    char                *break_str,
    char                *string,
    int                  byte_len,
    int                 *idx)
{
    if (ProcessString(my_struct, False, False, True, False,
				process_flag, string, byte_len, idx) != 0)
        return -1;

    my_struct->flags = _DtCvSetTypeToNonBreak(my_struct->flags);
    if (ProcessString(my_struct, False, False, False, False,
				process_flag, break_str, byte_len, idx) != 0)
        return -1;

    my_struct->flags = my_struct->flags & ~(_DtCvNON_BREAK);
    strmove (string, &string[*idx+1]);
    *idx = -1;
    return 0;
}

/******************************************************************************
 * Function:    static int MoveString(string, int idx);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     
 *
 ******************************************************************************/
static	int
MoveString(
    char	**string,
    int		 *max_len,
    int		 *idx)
{
    int    i     = *idx;
    int    myLen = *max_len;
    char  *src   = *string;

    i--;
    if (i == -1)
      {
        if (strlen(src) == myLen)
          {
	    char *newSrc = (char *) realloc (src, myLen + 2);

	    if (newSrc == nullptr)
	        return -1;	/* *string is still the valid old block */

	    src = newSrc;
	    myLen++;
            *string  = src;
            *max_len = myLen;
          }
        for (i = myLen; i > 0; i--)
	    src[i] = src[i-1];
      }

    *idx = i;
    return 0;
}

/******************************************************************************
 * Function:    int SetUp (_DtHelpVolumeHdl volume,
 *				CESDLVolume **sdl_vol,
 *				FormatStruct *frmt_struct,
 *				_DtCvSegment *toss,
 *				_DtCvSegment *cur_frmt,
 *				_DtCvValue lang
 *
 * Parameters:
 *		volume		Specifies the volume handle. If non-NULL,
 *				create and return the sdl volume pointer
 *				in 'sdl_vol'.
 *		sdl_vol		If not NULL, returns the sdl volume pointer.
 *		frmt_struct	Specifies the formatting structure to use.
 *		toss		Specifies the toss to use. If NULL and
 *				sdl_vol asked for and flag == True, the
 *				sdl_vol->toss will be used.
 *		lang		Specifies if the language/charset should
 *				be set using information from the volume.
 *		flag		Specifies if to get/use the sdl_vol->toss
 *				if toss is NULL.
 *
 * Returns:	0 if no failures, -1 if errors.
 *
 * Purpose:     Set up the formatting structure to use.
 *
 *****************************************************************************/
static	int
SetUp (
    _DtHelpVolumeHdl	  volume,
    CESDLVolume		**sdl_vol,
    FormatStruct	 *frmt_struct,
    _DtCvSegment		 *toss,
    const _FrmtUiInfo	 *ui_info,
    int			  fd,
    _DtCvValue		  lang,
    _DtCvValue		  flag)
{
    *frmt_struct = DefFormatStruct;

    frmt_struct->my_links = _DtLinkDbCreate();
    if (nullptr == frmt_struct->my_links)
	return -1;

    if (volume != nullptr && sdl_vol != nullptr)
      {
        *sdl_vol = _DtHelpCeGetSdlVolumePtr(volume);
	if (*sdl_vol == nullptr)
	  {
	    _DtLinkDbDestroy(frmt_struct->my_links);
	    return -1;
	  }
      }

    /*
     * malloc and initialize the starting font information
     */
    frmt_struct->my_fonts = AllocateFontInfo(frmt_struct);
    if (nullptr == frmt_struct->my_fonts)
      {
	_DtLinkDbDestroy(frmt_struct->my_links);
	return -1;
      }

    if (volume != nullptr)
      {
        frmt_struct->vol_name = _DtHelpCeGetVolumeName(volume);

	if (toss == nullptr && flag == True)
	    toss = _DtHelpCeGetSdlVolToss(volume, fd);

        if (lang == True)
          {
	    _DtHelpFontHintsLang(*(frmt_struct->my_fonts)) =
				_DtHelpCeGetSdlVolLanguage(volume);
	    _DtHelpFontHintsCharSet(*(frmt_struct->my_fonts)) =
				(char *) _DtHelpCeGetSdlVolCharSet(volume);
          }
      }

    /*
     * determine mb_len should be based on lang/charset.
     */
    frmt_struct->mb_len   = MB_CUR_MAX;
    frmt_struct->toss     = toss;
    frmt_struct->ui_info  = ui_info;

    return 0;
}

/******************************************************************************
 * Function:    void AddToAsyncList (_DtCvSegment *seg_list)
 *
 * Parameters:
 *
 * Returns      Nothing
 *
 * Purpose:     Add a segment block to the async list.
 *
 *****************************************************************************/
static  void
AddToAsyncList (
    FormatStruct	*my_struct,
    _DtCvSegment	*block_seg)
{

    if (my_struct->async_blks != nullptr)
      {
        _DtCvSegment  *pSeg = my_struct->async_blks;

	while (_DtCvNextSeg(pSeg) != nullptr)
	    pSeg = _DtCvNextSeg(pSeg);
	
        _DtCvNextSeg(pSeg) = block_seg;
      }
    else
	my_struct->async_blks = block_seg;

}

/******************************************************************************
 * Function:    int CheckOptionList (int attr_value_type, const char *attr,
 *					_DtCvValue check_flag,
 *					SdlOption cur_num,
 *					SdlOption *num);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     If found, returns the enum value.
 *
 ******************************************************************************/
static	int
CheckOptionList(
    int		 attr_value_type,
    const char	*attr,
    _DtCvValue	 check,
    SdlOption	 cur_num,
    SdlOption	*num)
{
    int    i = 0;
    int    lowerChar;
    int    result = -1;
    const OptionList *option;
    _DtCvValue	found  = False;
    _DtCvValue	cmpFnd = False;

    while (SDLOptionsList[i].attr_value != -1 &&
			SDLOptionsList[i].attr_value != attr_value_type)
	i++;

    if (SDLOptionsList[i].attr_value != -1)
      {
	option    = SDLOptionsList[i].options;
	lowerChar = _DtHelpCeToLower(*attr);

	while (option != nullptr && option->string != nullptr &&
					(check == True || cmpFnd == False))
	  {
	    /*
	     * check to see if the current option is in the list
	     * of valid options for this attribute.
	     */
	    if (check && cur_num == option->option_value)
	      {
		/*
		 * okay, it's been found, don't check anymore.
		 */
		found = True;
		check = False;
	      }

	    /*
	     * otherwise, find out if this value is a valid option
	     * for the attribute.
	     */
	    else if (cmpFnd == False &&
			lowerChar == _DtHelpCeToLower(*(option->string)) &&
			_DtCvStrCaseCmpLatin1(option->string, attr) == 0)
	      {
		cmpFnd = True;
		*num   = option->option_value;
	      }
	    option++;
	  }
      }

    /*
     * if found is true, means check was originally set to true and
     * we found the current value in the option list for this attribute.
     */
    if (found == True)
	*num = cur_num;

    /*
     * if we found that the current value was valid or that the string
     * was a valid option in the list for the attribute, return no error.
     */
    if (found == True || cmpFnd == True)
	result = 0;

    return result;
}

/******************************************************************************
 * Function:    int CheckFontList (int attr_value_type, char *attr,
 *					enum CEFontSpec *num);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     If found, returns the enum value.
 *
 ******************************************************************************/
static	int
CheckFontList(
    int			 attr_value_type,
    char		*attr,
    _DtHelpFontValue	*num)
{
    int    i = 0;
    const FontSpecOption *option;

    while (SDLFontList[i].attr_value != -1 &&
			SDLFontList[i].attr_value != attr_value_type)
	i++;

    if (SDLFontList[i].attr_value != -1)
      {
	option = SDLFontList[i].options;

	while (option != nullptr && option->string != nullptr)
	  {
	    if (_DtCvStrCaseCmpLatin1(option->string, attr) == 0)
	      {
		*num = option->option_value;
		return 0;
	      }
	    option++;
	  }
      }
    return -1;
}

/******************************************************************************
 * Function:    int LookAhead (FormatStruct my_struct, int token_value,
 *				int remove_flag);
 *
 * Parameters:
 *		my_struct	Specifies the parsing structure.
 *		token_value	Specifies the token to match
 *		remove_flag	Specifies whether to clear the look ahead
 *				value. If True, will set the look ahead
 *				parsed value to invalid iff token_value
 *				equals the parsed look ahead value.
 *
 * Returns:      0 if token_value matched parsed value,
 *		 1 if parsed value is invalid.
 *		-1 if parsed value is valid but token_value did not match.
 *
 * Purpose:
 *
 ******************************************************************************/
static	int
TestLookAhead(
    FormatStruct	*my_struct,
    enum SdlElement	 token_value,
    _DtCvValue		 end_flag,
    int			 remove_flag)
{
    if (my_struct->parsed == SdlElementNone)
	return 1;

    if (my_struct->parsed == token_value && my_struct->end_flag == end_flag)
      {
	if (remove_flag == True)
	  {
	    my_struct->parsed = SdlElementNone;
	    if (my_struct->remember != nullptr)
	      {
		free (my_struct->remember);
		my_struct->remember = nullptr;
	      }
	  }
	return 0;
      }

    return -1;
}

/******************************************************************************
 * Function:    int MatchSDLElement (FormatStruct my_struct, sdl_string,
 *					int sdl_element, sig_chars)
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the specific element.
 *
 ******************************************************************************/
static	int
MatchSDLElement(
    FormatStruct	*my_struct,
    const char		*sdl_string,
    enum SdlElement	 sdl_element,
    int			 sig_chars,
    _DtCvValue		 end_flag)
{
    int    i;

    i = TestLookAhead(my_struct, sdl_element, end_flag, True);

    if ( i != 1)
	return i;

    return (_DtHelpCeMatchSdlElement(my_struct->my_file,sdl_string,sig_chars));
}

/******************************************************************************
 * Function:    void SetAttributeFlag (
 *
 ******************************************************************************/
static	void
SetAttributeFlag(
    ElementInfoPtr	cur_info,
    unsigned long	attr_define)
{
    unsigned long  flag = attr_define & ~(VALUE_MASK);

    switch (attr_define & VALUE_MASK)
      {
	case ENUM_VALUE:
		cur_info->enum_values |= flag;
		break;
	case STRING1_VALUE:
		cur_info->str1_values |= flag;
		break;
	case STRING2_VALUE:
		cur_info->str2_values |= flag;
		break;
	case NUMBER_VALUE:
		cur_info->num_values |= flag;
		break;

      }
}

/******************************************************************************
 * Function:    void InitAttributes(SDLAttrStruct *as,
 *						SDLElementAttrList *attr_list)
 *
 * Parameters:
 *
 * Returns:     Attribute Processed if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page beginning.
 *
 ******************************************************************************/
static	void
InitAttributes(
    _DtCvSegment	*p_seg,
    _DtHelpFontHints	*font_specs,
    ElementInfo		*element_info,
    const SDLElementAttrList	*attr_list)
{
   char		**strPtr1;
   char		 *varOffset1 = nullptr;
   SDLNumber	 *numPtr1;
   SdlOption	 *enumPtr1;
   SdlOption	  defNum = _DtCvOPTION_BAD;
   const SDLAttribute *pAttr;

   while (attr_list->sdl_attr_define != -1)
     {
	if (attr_list->def_string != nullptr ||
				attr_list->sdl_value == SdlAttrValueImpliedDef)
	  {
	    pAttr = SDLAttributeList;
	    while (pAttr->data_type != SdlAttrDataTypeInvalid &&
		    !(ATTRS_EQUAL(pAttr->data_type,
			pAttr->sdl_attr_define, attr_list->sdl_attr_define)))
		pAttr++;

	    if (pAttr->struct_type != SdlIgnore &&
		pAttr->data_type != SdlAttrDataTypeInvalid &&
		SDLIsAttrSet(*element_info, pAttr->sdl_attr_define) == False)
	      {
		/*
		 * determine which structure the information goes in
		 */
		if (pAttr->struct_type == SdlFontSpecific)
		    varOffset1 = ((char *) font_specs);
		else if (pAttr->struct_type == SdlContainerSpecific)
		    varOffset1 = ((char *)_SdlContainerPtrOfSeg(p_seg));
		else if (pAttr->struct_type == SdlElementSpecific)
		    varOffset1 = ((char *) element_info);
		else if (pAttr->struct_type == SdlTableSpecific)
		    varOffset1 = ((char *) element_info->w.table_info);

		varOffset1 = varOffset1 + pAttr->field_ptr;
		if (pAttr->data_type == SdlAttrDataTypeNumber)
		  {
		    numPtr1  = (SDLNumber *) varOffset1;
		    *numPtr1 = ((SDLNumber)atoi(attr_list->def_string));
		  }
		else if (pAttr->data_type == SdlAttrDataTypeEnum)
		  {
		    enumPtr1  = (SdlOption *) varOffset1;
		    (void) CheckOptionList (attr_list->sdl_attr_define,
					attr_list->def_string,
		((attr_list->sdl_attr_define & SDL_ATTR_CLASS) ? True : False),
					ElInfoClan(element_info),
					&defNum);
		    if ((attr_list->sdl_attr_define & SDL_ATTR_CLASS & ~(VALUE_MASK))
					&& ElInfoClan(element_info) != defNum)
			SetAttributeFlag(element_info,
						attr_list->sdl_attr_define);
		    *enumPtr1 = defNum;
		  }
		else if (SDLIsString1(pAttr->sdl_attr_define)
					||
			 SDLIsString2(pAttr->sdl_attr_define))
		  {
		    strPtr1  = (char **) varOffset1;
		    *strPtr1 = (char *) attr_list->def_string;
		  }
	      }
	  }
       attr_list++;
     }
}

#ifdef	DEBUG
static char *
ElToName(enum SdlElement el)
{
    switch (el)
      {
	case SdlElementNone: return ("None");
	case SdlElementComment: return ("Comment");
	case SdlElementSdlDoc: return ("SdlDoc");
	case SdlElementVStruct: return ("VStruct");
	case SdlElementVirpage: return ("Virpage");
	case SdlElementHead: return ("Head");
	case SdlElementSubHead: return ("SubHead");
	case SdlElementSnb: return ("Snb");
	case SdlElementBlock: return ("Block");
	case SdlElementForm: return ("Form");
	case SdlElementPara: return ("Para");
	case SdlElementCPara: return ("CPara");
	case SdlElementFdata: return ("Fdata");
	case SdlElementFstyle: return ("Fstyle");
	case SdlElementFrowvec: return ("Frowvec");
	case SdlElementKey: return ("Key");
	case SdlElementCdata: return ("Cdata");
	case SdlElementGraphic: return ("Graphic");
	case SdlElementText: return ("Text");
	case SdlElementAudio: return ("Audio");
	case SdlElementVideo: return ("Video");
	case SdlElementAnimate: return ("Animate");
	case SdlElementCrossDoc: return ("CrossDoc");
	case SdlElementManPage: return ("ManPage");
	case SdlElementTextFile: return ("TextFile");
	case SdlElementSysCmd: return ("SysCmd");
	case SdlElementCallback: return ("Callback");
	case SdlElementScript: return ("Script");
	case SdlElementAnchor: return ("Anchor");
	case SdlElementLink: return ("Link");
	case SdlElementSwitch: return ("Switch");
	case SdlElementSnRef: return ("SnRef");
	case SdlElementRefItem: return ("RefItem");
	case SdlElementAltText: return ("AltText");
	case SdlElementSphrase: return ("Sphrase");
	case SdlElementRev: return ("Rev");
	case SdlElementSpc: return ("Spc");
	case SdlElementIf: return ("If");
	case SdlElementCond: return ("Cond");
	case SdlElementThen: return ("Then");
	case SdlElementElse: return ("Else");
	case SdlElementDocument: return ("Document");
	case SdlElementSgml: return ("Sgml");
	case SdlElementDocType: return ("DocType");
	case SdlElementLoids: return ("Loids");
	case SdlElementToss: return ("Toss");
	case SdlElementLoPhrases: return ("LoPhrases");
	case SdlElementPhrase: return ("Phrase");
	case SdlElementIndex: return ("Index");
	case SdlElementEntry: return ("Entry");
	case SdlElementRelDocs: return ("RelDocs");
	case SdlElementRelFile: return ("RelFile");
	case SdlElementNotes: return ("Notes");
	case SdlElementKeyStyle: return ("KeyStyle");
	case SdlElementHeadStyle: return ("HeadStyle");
	case SdlElementFormStyle: return ("FormStyle");
	case SdlElementFrmtStyle: return ("FrmtStyle");
	case SdlElementGrphStyle: return ("GrphStyle");
	case SdlElementId: return ("Id");
	case SdlElementBlockAsync: return ("BlockAsync");
	case SdlElementTitle: return ("Title");
	case SdlPcDataFollows: return ("Follows");
      }
    return ("Unknown");
}
#endif /* DEBUG */

/******************************************************************************
 * Function:    int ProcessSDLAttribute(FormatStruct my_struct,
 *						SDLAttrStruct *cur_attr,
 *						SDLElementAttrList *attr_list,
 *						char     *attr_name,
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page beginning.
 *
 ******************************************************************************/
static	int
ProcessSDLAttribute(
    FormatStruct	*my_struct,
    _DtCvSegment		*p_seg,
    _DtHelpFontHints	*font_specs,
    ElementInfo		*element_info,
    const SDLElementAttrList	*attr_list,
    char		*attr_name)
{
    int          result;
    char	*attrValue;
    char	**strPtr;
    char	 numBuf[DTD_NAMELEN+1];
    char	*varOffset1 = nullptr;
    SDLNumber   *numPtr1;
    SdlOption   *enumPtr1;
    SdlOption	 attrNum;
    _DtHelpFontValue	 fontNum = 0;
    _DtHelpFontValue	*fontPtr1;
    const SDLAttribute *pAttr = SDLAttributeList;

    /*
     * find the attribute in the list of SDL attributes
     * assumes that 'attr_name' has been lower cased, since all the
     * attribute names in SDLAttributeList are already lower case.
     */
    while (pAttr->data_type != SdlAttrDataTypeInvalid &&
	      (*(pAttr->name) != *attr_name || strcmp(pAttr->name, attr_name)))
	pAttr++;

    if (pAttr->data_type != SdlAttrDataTypeInvalid)
      {

        /*
         * Check to see if this element contains an attribute with
         * the found value.
         */
	while (attr_list->sdl_attr_define != -1 &&
			!ATTRS_EQUAL(pAttr->data_type, pAttr->sdl_attr_define,
						attr_list->sdl_attr_define))
	    attr_list++;

        if (attr_list->sdl_attr_define == -1)
	    return -1;
      }

    /*
     * check it against the ones that are allowed for this element
     */
    if (pAttr->data_type != SdlAttrDataTypeInvalid)
      {
	switch (pAttr->data_type)
	  {
	    case SdlAttrDataTypeId:
		    result = _DtHelpCeGetSdlId(my_struct->my_file, &attrValue);
		    break;

	    case SdlAttrDataTypeNumber:
		    result = _DtHelpCeGetSdlNumber(my_struct->my_file, numBuf);
		    break;

	    case SdlAttrDataTypeCdata:
		    result = _DtHelpCeGetSdlAttributeCdata(my_struct->my_file,
						    False, &attrValue);
		    break;

	    case SdlAttrDataTypeFont:
		    result = _DtHelpCeGetSdlAttributeCdata(my_struct->my_file,
						    False, &attrValue);
		    if (result != -1)
		      {
			/* an empty value leaves attrValue unallocated */
			if (attrValue == nullptr)
			    result = -1;
			else
			    result = CheckFontList (attr_list->sdl_attr_define,
						     attrValue, &fontNum);
			if (result == -1)
			  {
			    free(attrValue);
			    attrValue = nullptr;
			  }
		      }
		    break;

	    case SdlAttrDataTypeEnum:
		    result = _DtHelpCeGetSdlAttributeCdata(my_struct->my_file,
						    False, &attrValue);
		    if (result != -1)
		      {
			/* an empty value leaves attrValue unallocated */
			if (attrValue == nullptr)
			    result = -1;
			else
			    result = CheckOptionList (attr_list->sdl_attr_define,
						    attrValue, False,
						    (SdlOption) _DtCvOPTION_BAD,
						    &attrNum);
			if (result == -1)
			  {
			    free(attrValue);
			    attrValue = nullptr;
			  }
		      }
		    break;

	    default:
		    printf ("hit unknown in processing attribute\n");
		    result = -1;
		    break;
	  }

	if (result == 0)
	  {
	    if (pAttr->struct_type != SdlIgnore)
	      {
		/*
		 * determine which structure the information goes in
		 */
		if (pAttr->struct_type == SdlFontSpecific)
		    varOffset1 = ((char *) font_specs);
		else if (pAttr->struct_type == SdlContainerSpecific)
		    varOffset1 = ((char *)_SdlContainerPtrOfSeg(p_seg));
		else if (pAttr->struct_type == SdlElementSpecific)
		    varOffset1 = ((char *) element_info);
		else if (pAttr->struct_type == SdlTableSpecific)
		    varOffset1 = ((char *) element_info->w.table_info);

		varOffset1 = varOffset1 + pAttr->field_ptr;
		if (pAttr->data_type == SdlAttrDataTypeNumber)
		  {
		    numPtr1  = (SDLNumber *) varOffset1;
		    *numPtr1 = ((SDLNumber) atoi(numBuf));
		  }
		else if (pAttr->data_type == SdlAttrDataTypeEnum)
		  {
		    enumPtr1  = (SdlOption *) varOffset1;
		    *enumPtr1 = attrNum;
    
		    free(attrValue);
		  }
		else if (pAttr->data_type == SdlAttrDataTypeFont)
		  {
		    fontPtr1  = (_DtHelpFontValue *) varOffset1;
		    *fontPtr1 = fontNum;
    
		    free(attrValue);
		  }
		else
		  {
		    strPtr = (char **) varOffset1;
		    *strPtr = attrValue;
		  }
	      }
	    else if (SdlAttrDataTypeNumber != pAttr->data_type)
		free(attrValue);
	    SetAttributeFlag(element_info, pAttr->sdl_attr_define);
	  }
	return result;
      }

    return -1;
}

/******************************************************************************
 * Function:    int VerifyAttrList (
 *						SDLElementAttrList *attr_list)
 *
 * Parameters:
 *
 * Returns:     Attribute Processed if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page beginning.
 *
 ******************************************************************************/
static	int
VerifyAttrList(
    ElementInfoPtr		 el_info,
    const SDLElementAttrList	*attr_list)
{
    long  value = 0;
    while (attr_list->sdl_attr_define != -1)
      {
	if (attr_list->sdl_value == SdlAttrValueRequired)
	  {
	    switch (attr_list->sdl_attr_define & VALUE_MASK)
	      {
		case ENUM_VALUE:
				/*
				 * mask off the minor number
				 */
				value = el_info->enum_values & ~(MINOR_MASK);
				break;
		case STRING1_VALUE:
				value = el_info->str1_values;
				break;
		case STRING2_VALUE:
				value = el_info->str2_values;
				break;
		case NUMBER_VALUE:
				value = el_info->num_values;
				break;
	      }
	    if (!(attr_list->sdl_attr_define & value))
		return -1;
	  }
	attr_list++;
      }

    return 0;
}

/******************************************************************************
 * Function:    void MergeTossInfo (
 *				_DtCvSegment *cur_element,
 *				FormatStruct my_struct);
 *
 * Parameters:
 *
 * Returns:     nothing
 *
 * Purpose:     Looks for the virtual page beginning.
 *
 ******************************************************************************/
static	void
MergeTossInfo(
    FormatStruct	*my_struct,
    _DtCvSegment		*cur_contain,
    _DtHelpFontHints	*font_specs)
{
    _DtCvSegment *tossSeg;

    if (my_struct->toss == nullptr)
	return;

    tossSeg = _DtHelpCeMatchSemanticStyle (my_struct->toss,
				ElClan(my_struct),
				ElLevel(my_struct),
				ElSsi(my_struct));
    _DtHelpCeMergeSdlAttribInfo(tossSeg, cur_contain, font_specs,
				&(my_struct->el_info),
				&(my_struct->el_info.str1_values),
				&(my_struct->el_info.str2_values));

    return;
}


/*------------------------- Element Processing ------------------------------*/
/******************************************************************************
 * Function:    int ParseElementStart (
 *				FormatStruct my_struct,
 *				int element, _DtCvValue process_flag);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page beginning.
 *
 ******************************************************************************/
static	int
ParseElementStart(
    FormatStruct	*my_struct,
    SDLElementInfo	 element,
    _DtCvValue		 process_flag)
{
    _DtCvSegment  *segPtr;
    _DtCvSegment  *newSeg;
    FrmtPrivateInfo *priv;

    if (MatchSDLElement (my_struct, element.element_str, element.sdl_element,
					element.str_sig_chars, False) != 0)
	return -1;
	
    /*
     * remember what we are currently parsing
     */
    ElType(my_struct) = element.sdl_element;

    /*
     * does this element possibly pull in different fonts?
     * if so, allocate a new font structure.
     */
    if (True == process_flag && element.new_fonts)
      {
	my_struct->my_fonts = AllocateFontInfo(my_struct);
	if (nullptr == my_struct->my_fonts)
	    return -1;
      }

    /*
     * if Cdata or skipping information don't do anything else
     */
    if (element.sdl_to_gen == SdlToNone || process_flag == False)
	return 0;

    /*
     * allocate a segment for this element
     */
    if (_DtHelpCeAllocSegment(my_struct->malloc_size,
				&(my_struct->alloc_size),
				&(my_struct->block_list), &segPtr) != 0)
	return -1;

    /*
     * add this segment to the list when done
     */
    my_struct->add_seg = segPtr;

    /*
     * Set the flags and initialize variables depending on the
     * type of general segment this element becomes.
     */
    switch (element.sdl_to_gen)
      {
	case SdlToContainer:
		/*
		 * set the container flag and initialize its formatting
		 * information to the default.
		 */
		segPtr->type = _DtCvSetTypeToContainer(segPtr->type);
		_DtCvContainerOfSeg(segPtr) = DefFrmtSpecs;
		_DtCvContainerLeadingOfSeg(segPtr) =
						my_struct->ui_info->leading;
	
		/*
		 * if this is a formatting type container, do some more work.
		 */
		if (element.formatting)
		  {
		    const _DtCvContainer *active = my_struct->active_frmt;

		    if (nullptr == active) active = &DefFrmtSpecs;

		    /*
		     * The margin information - it gets zero'ed
		     */
		    _DtCvContainerLMarginOfSeg(segPtr) = 0;
		    _DtCvContainerRMarginOfSeg(segPtr) = 0;
		    _DtCvContainerFMarginOfSeg(segPtr) = 0;
		    _DtCvContainerTMarginOfSeg(segPtr) = 0;
		    _DtCvContainerBMarginOfSeg(segPtr) = 0;

		    /*
		     * inherit some of the formatting directives
		     */
		    _DtCvContainerVJustifyOfSeg(segPtr) =
					ContainerPtrToVJustify(active);
		    _DtCvContainerJustifyOfSeg(segPtr) =
					ContainerPtrToJustify(active);
		    /*
		     * break the connection with the previous data.
		     */
		    my_struct->prev_data = nullptr;
		    _DtCvContainerListOfSeg(segPtr) = nullptr;
	
		    /*
		     * check to see if these are controllers.
		     */
		    if (element.sdl_element == SdlElementHead ||
				SdlElementSubHead == element.sdl_element)
			segPtr->type = _DtCvSetTypeToController(segPtr->type);
		    
		    /*
		     * make this the currently active formatting information
		     */
		    my_struct->active_frmt = &(_DtCvContainerOfSeg(segPtr));
		  }
		break;

	case SdlToMarker:
		/*
		 * set the type; the attribute processing will set the id.
		 */
		segPtr->type = _DtCvSetTypeToMarker(segPtr->type);
		break;

	case SdlToSnb:
		/*
		 * set the type; the attribute processing will set the id.
		 * graphics only exist in a snb. You have to keep the
		 * id around until the graphic get resolved. So a graphic
		 * is a container with a region.
		 */
		segPtr->type = _DtCvSetTypeToContainer(segPtr->type);

		/*
		 * remember the refitem type.
		 */
		priv = FrmtPrivInfoPtr(segPtr);
		priv->sdl_el_type = ElType(my_struct);
		break;

	case SdlToSpc:
		/*
		 * set the type; the attribute processing will set the id.
		 */
		segPtr->type = _DtCvSetTypeToRegion(segPtr->type);
		break;
      }

    if (my_struct->cur_link != -1)
      {
	segPtr->type = _DtCvSetTypeToHyperText(segPtr->type);
        segPtr->link_idx = my_struct->cur_link;
      }

    switch (element.sdl_element)
      {
	case SdlElementSnb:
		my_struct->resolve_font = _SdlFontModeNone;
		my_struct->snb          = segPtr;
		my_struct->prev_data    = nullptr;
		break;

	case SdlElementAltText:
		segPtr->type = _DtCvSetTypeToNonBreak(segPtr->type);
		break;

	case SdlElementForm:
		/*
		 * allocate a table for all the information
		 */
		if (_DtHelpCeAllocSegment(my_struct->malloc_size,
				&(my_struct->alloc_size),
				&(my_struct->block_list), &newSeg) != 0)
		    return -1;

		newSeg->type = _DtCvSetTypeToTable(newSeg->type);
		_DtCvJustifyCharsOfTableSeg(newSeg) = nullptr;

		/*
		 * set the my_struct pointer to this table so that
		 * as the fstyle and frowec elements are parsed, the
		 * data goes directly into this table.
		 */
		ElTable(my_struct) = newSeg;
		break;
      }

    return 0;

} /* End ParseElementStart */

/******************************************************************************
 * Function:    int ParseElementAttr (
 *					FormatStruct my_struct,
 *					SDLElementAttrList *attribs)
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
ParseElementAttr(
    FormatStruct	*my_struct,
    enum SdlElement	 cur_element,
    const SDLElementAttrList	*attribs,
    _DtCvValue		 flag,
    _DtCvValue		 process_flag)
{
    int           result = 0;
    char          attribName[MAX_ATTRIBUTE_LENGTH + 2];
    _DtCvSegment *mySeg;

    if (attribs == nullptr || process_flag == False)
      {
	/*
	 * No attributes - is there cdata in here?
	 */
	if (flag || attribs != nullptr)
	    result = _DtHelpCeSkipCdata (my_struct->my_file,
			(cur_element == SdlElementDocType ? True : False));

	/*
	 * get the element's end.
	 */
	if (result != -1)
	    result = _DtHelpCeGetSdlAttribute(my_struct->my_file,
						MAX_ATTRIBUTE_LENGTH,
						attribName);

	/*
	 * _DtHelpCeGetSdlAttribute should return 1, meaning it found
	 * the >
	 */
	if (result != 1)
	    return -1;
	return 0;
      }

    /*
     * clean and initialize the attributes for this element.
     */
    my_struct->el_info.enum_values = 0;
    my_struct->el_info.num_values = 0;
    my_struct->el_info.str1_values = 0;
    my_struct->el_info.str2_values = 0;

    mySeg = my_struct->add_seg;
    InitAttributes(mySeg, my_struct->my_fonts, &(my_struct->el_info), attribs);

    /*
     * clear the colJ and colW
     */
    if (SdlElementForm == ElType(my_struct))
      {
	ElTableColJStr(my_struct) = nullptr;
	ElTableColWStr(my_struct) = nullptr;
      }

    /*
     * force the default of subheading orientation to heads as below the head.
     */
    if (ElType(my_struct) == SdlElementSubHead)
        _DtCvContainerVOrientOfSeg(mySeg) = _DtCvJUSTIFY_BOTTOM;

    do {
	result = _DtHelpCeGetSdlAttribute(my_struct->my_file,
						MAX_ATTRIBUTE_LENGTH,
						attribName);
	if (result == 0)
	    result = ProcessSDLAttribute(my_struct, mySeg,
					my_struct->my_fonts,
					&(my_struct->el_info),
					attribs,
					attribName);
    } while (result == 0);

    if (result != -1)
	result = VerifyAttrList(&(my_struct->el_info), attribs);

    if (result != -1)
      {
	if (cur_element == SdlElementVirpage
				||
	    SDLIsAttrSet(my_struct->el_info, SDL_ATTR_CLASS)
				||
	    SDLIsAttrSet(my_struct->el_info, SDL_ATTR_SSI)
				||
	    SDLIsAttrSet(my_struct->el_info, SDL_ATTR_LEVEL))
	MergeTossInfo(my_struct, mySeg, my_struct->my_fonts);

        if (SDLIsStrAttrSet(my_struct->el_info, SDL_ATTR_LANGUAGE)
		|| SDLIsStrAttrSet(my_struct->el_info, SDL_ATTR_CHARSET))
	    my_struct->mb_len = MB_CUR_MAX;

	/*
	 * Do some specific work for containers.
	 */
	if (nullptr != mySeg && _DtCvIsSegContainer(mySeg))
	  {
	    /*
	     * transfer any id or rid that got specified
	     */
	    if (SDLIsStrAttrSet(my_struct->el_info, SDL_ATTR_ID) ||
			SDLIsStrAttrSet(my_struct->el_info, SDL_ATTR_RID))
	      {
	        _DtCvContainerIdOfSeg(mySeg) = ElId(my_struct);
	        ClearAttrFlag(my_struct->el_info, SDL_ATTR_ID);
	        ClearAttrFlag(my_struct->el_info, SDL_ATTR_RID);
	      }

	    /*
	     * set the margins to absolute values
	     */
	    if (my_struct->ui_info->avg_char > 0)
	      {
	        _DtCvContainerLMarginOfSeg(mySeg) =
					_DtCvContainerLMarginOfSeg(mySeg) *
						my_struct->ui_info->avg_char;

	        _DtCvContainerRMarginOfSeg(mySeg) =
					_DtCvContainerRMarginOfSeg(mySeg) *
						my_struct->ui_info->avg_char;

	        _DtCvContainerFMarginOfSeg(mySeg) =
					_DtCvContainerFMarginOfSeg(mySeg) *
						my_struct->ui_info->avg_char;

	      }
	    /*
	     * set the container spacing to absolute values
	     */
	    if (my_struct->ui_info->line_height / 2 > 0)
	      {
/*
 * TODO .... take maximum! of spacing.
 */
	        TMarginOfSeg(mySeg) = TMarginOfSeg(mySeg) *
					(my_struct->ui_info->line_height / 2);
	        BMarginOfSeg(mySeg) = BMarginOfSeg(mySeg) *
					(my_struct->ui_info->line_height / 2);

	      }

	    /*
	     * set the border width information
	     */
	    if (_DtCvBORDER_NONE != _DtCvContainerBorderOfSeg(mySeg))
		_DtCvContainerLineWidthOfSeg(mySeg) =
						my_struct->ui_info->line_width;
	  }
      }

    if (result == 1)
	result = 0;

    return result;

} /* End ParseElementAttr */

/******************************************************************************
 * Function:    int ParseElementEnd (
 *					FormatStruct my_struct,
 *					int el_type);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page beginning.
 *
 ******************************************************************************/
static	int
ParseElementEnd(
    FormatStruct	*my_struct,
    _DtCvSegment	*if_prev,
    SDLElementInfo	 element,
    _DtCvValue		 process_flag,
    _DtCvValue		 end_flag)
{
    char		*id;
    _DtCvSegment	*newSeg;
    _DtCvSegment	*segList;

    if (MatchSDLElement (my_struct, element.element_end_str,
		element.sdl_element, element.end_sig_chars, True) != 0 ||
		(end_flag == False &&
		    _DtHelpCeFindSkipSdlElementEnd(my_struct->my_file) != 0))
	return -1;

    if (process_flag == True)
      {
	_DtCvSegment  *pElement = my_struct->add_seg;

	/*
	 * if we aren't resolving the fonts for this element,
	 * we need to save the toss information for later
	 * when the element gets used in a table or snref.
	 */
	if (_SdlFontModeResolve != my_struct->resolve_font &&
		(SDLIsAttrSet(my_struct->el_info, SDL_ATTR_CLASS)
				||
		 SDLIsAttrSet(my_struct->el_info, SDL_ATTR_SSI)
				||
		 SDLIsAttrSet(my_struct->el_info, SDL_ATTR_LEVEL)))
	  {
	    SdlMatchData *match;

	    /*
	     * was a segment allocated for this element?
	     * if not, we'll have to put our own special segment
	     * around the segment list so the correct font inheritance
	     * occurs. Later, this special segment will have to be
	     * stripped out when the async blocks or snref item
	     * is resolved.
	     */
	    if (nullptr == pElement)
	      {
		if (_DtHelpCeAllocSegment(1, nullptr, nullptr, &pElement) != 0)
		    return -1;

		pElement->type = _DtCvSetTypeToContainer(pElement->type);
		pElement->type = SetSdlWrapper(pElement->type);
		_DtCvContainerListOfSeg(pElement) = my_struct->seg_list;
		my_struct->seg_list = pElement;
	      }

	    /*
	     * has match data already been allocated for this element?
	     */
	    match = SegMatchDataPtr(pElement);
	    if (nullptr == match)
	      {
		match = (SdlMatchData *) malloc(sizeof(SdlMatchData));
		if (nullptr == match)
		    return -1;
	      }

	    /*
	     * save the clan, level and ssi of this element.
	     */
	    *match = my_struct->el_info.match;
	    SegMatchData(pElement) = (void *) match;
	    ClearAttrFlag(my_struct->el_info, SDL_ATTR_SSI);
	  }

	/*
	 * attach the current segment list to the container's list
	 */
	if (nullptr != pElement && _DtCvIsSegContainer(pElement))
	    _DtCvContainerListOfSeg(pElement) = my_struct->seg_list;

	/*
	 * check to see if the element has any data.
	 * If not, can it (the element) be eleminated and free up memory?
	 */
	if (nullptr != pElement && my_struct->seg_list == nullptr
	    && element.elim_flag == True
	    && ElType(my_struct) != SdlElementKey
	    && TMarginOfSeg(pElement) == 0 && BMarginOfSeg(pElement) == 0)
	  {
	    _DtHelpFreeSegments(my_struct->add_seg, _DtCvFALSE,
					my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);

	    if (ElType(my_struct) == SdlElementLink)
		_DtLinkDbRemoveLink(my_struct->my_links,
						    my_struct->cur_link);
	    my_struct->add_seg = nullptr;
	  }
	else
	  {
	    switch(ElType(my_struct))
	      {
		case SdlElementSnb:
		    my_struct->add_seg = nullptr;
		    break;

		case SdlElementBlock:
		    if (ElTiming(my_struct) == SdlTimingAsync)
		      {
			if (nullptr != my_struct->add_seg)
			    AddToAsyncList(my_struct, my_struct->add_seg);

			my_struct->add_seg = nullptr;
		      }
		    break;

		case SdlElementHead:
		case SdlElementSubHead:
		    if (SDLIsAttrSet(my_struct->el_info, SDL_ATTR_ABBREV)
				&& ElAbbrev(my_struct) != nullptr
				&& strlen(ElAbbrev(my_struct)))
		      {
			AbbrevOfSeg(my_struct->add_seg) =
						(void *) ElAbbrev(my_struct);
			ClearAttrFlag(my_struct->el_info, SDL_ATTR_ABBREV);
		      }
		    break;

		case SdlElementFdata:
		    ElTableList(my_struct) = ConvertToList(my_struct->seg_list);
		    break;

		case SdlElementLink:
		    CompressLinkSeg(my_struct->seg_list);
		    /* fall through */

		case SdlElementKey:
		case SdlElementSphrase:
		    /*
		     * move the content of the element up.
		     */
		    my_struct->add_seg = my_struct->seg_list;

		    /*
		     * free the no longer needed segments.
		     */
		    if (nullptr != pElement)
		      {
		        _DtCvContainerListOfSeg(pElement) = nullptr;
		        _DtHelpFreeSegments(pElement, _DtCvFALSE,
					my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);
		      }
		    break;

		case SdlElementForm:
		    /*
		     * get the segment list.
		     */
		    newSeg = ElTable(my_struct);

		    /*
		     * include any async blocks into the table list.
		     */
		    if (ResolveAsyncBlock(my_struct, newSeg) != 0)
			return -1;

		    /*
		     * compress the table if possible down to just
		     * a container.
		     */
		    newSeg = CompressTable(my_struct, newSeg);

		    /*
		     * add the table segment to the form's container list
		     * taking into account any heads for the form.
		     */
		    _DtCvNextSeg(newSeg) = _DtCvContainerListOfSeg(pElement);
		    _DtCvContainerListOfSeg(pElement) = newSeg;
		    break;

		case SdlElementIf:
		    if (ResolveIf (my_struct, if_prev))
			return -1;
		    break;

		case SdlElementSnRef:
		    /*
		     * get the id of snref
		     */
		    id = nullptr;
		    if (SDLIsAttrSet(my_struct->el_info, SDL_ATTR_ID))
			id = _DtCvContainerIdOfSeg(pElement);

		    /*
		     * Resolve the snref to one of its items
		     */
		    newSeg = ResolveSnref(my_struct, pElement, id);

		    /*
		     * if the snref got resolved, process
		     * otherwise, let it pass through. We'll try to
		     * resolve it later.
		     */
		    if (newSeg != pElement)
		      {
		        /*
		         * remember the segment list that we want
		         * to free. This includes the snref.
		         */
		        segList = pElement;

		        /*
		         * if this <snref> had an id with it, then
		         * expand the <snref> to a marker segment
		         * (with the id) and a resolved refitem.
		         *
		         * Otherwise, eliminate the container.
		         */
		        if (nullptr != id)
		          {
			    /*
			     * remember the segment list of the container.
			     * Since we are re-using the snref segment,
			     * we want to eliminate just the refitems of
			     * the snref and not the actual snref.
			     */
			    segList = _DtCvContainerListOfSeg(pElement);

			    /*
			     * Move the refitem to be a sibling of the
			     * container.
			     */
			    _DtCvNextSeg(pElement) = newSeg;

			    /*
			     * transfer the container id to a marker id
			     * and change the container into a marker.
			     */
			    _DtCvIdOfMarkerSeg(pElement) =
					_DtCvContainerIdOfSeg(pElement);
			    pElement->type =
					_DtCvSetTypeToMarker(pElement->type);
			    newSeg = pElement;
		          }

		        /*
		         * free the no longer needed segments.
		         */
		        _DtHelpFreeSegments(segList, _DtCvFALSE,
					my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);
		      }

		    my_struct->add_seg = newSeg;
		    break;
	      }
	  }

	/*
	 * free the attributes for this element
	 */
	FreeAttributes(ElType(my_struct),
				&(my_struct->el_info), my_struct->my_fonts);

	/*
	 * if this element can pull in new fonts, then a new font
	 * structure was allocated for this element. Since we are
	 * leaving the element, place the font structure in the
	 * free list for possible reuse.
	 */
	if (element.new_fonts && FreeFontInfo(my_struct) == -1)
	    return -1;
      }
    else
	my_struct->add_seg = my_struct->seg_list;

    if (element.formatting == True)
	my_struct->prev_data = nullptr;

    return 0;

} /* End ParseElementEnd */

/******************************************************************************
 *
 * SDL Element Content Functions
 *
 *****************************************************************************/
/******************************************************************************
 * Function:    int FindAndFix (
 *					_DtCvSegment *toss,
 *					_DtCvSegment *snb,
 *					_DtCvSegment *seg_list)
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 *
 ******************************************************************************/
static	int
FindAndFix(
    FormatStruct	*my_struct)
{
    int		  curLink  = my_struct->cur_link;
    _DtCvSegment *newSeg;
    _DtCvSegment *prevData = my_struct->prev_data;
    _DtCvSegment *lastSeg  = my_struct->last_seg;
    _DtCvSegment *tmpSeg;

    _DtHelpFontHints *oldFonts = my_struct->my_fonts; 

    /*
     * If the head has an snref in it, get the snb and resolve them
     */
    if (0 < my_struct->snref_cnt)
      {
	UnresSnref	*snref = my_struct->un_snrefs;

	while (my_struct->snref_cnt)
	  {
	    /*
	     * reset the structure to what it was at the time
	     * this snref was encountered.
	     */
	    my_struct->prev_data = nullptr;
	    my_struct->last_seg  = nullptr;
	    my_struct->my_fonts  = &(snref->fonts);
	    my_struct->cur_link  = snref->cur_link;

	    /*
	     * resolve it.
	     */
	    newSeg = ResolveSnref(my_struct, snref->seg, nullptr);

	    /*
	     * free the snref items
	     */
	    _DtHelpFreeSegments(_DtCvContainerListOfSeg(snref->seg), _DtCvFALSE,
					my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);

	    /*
	     * find the end of the new segment's and have it point to
	     * the next segments that the old segment points to.
	     */
	    if (nullptr != newSeg)
	      {
	        tmpSeg = newSeg;
	        while (nullptr != _DtCvNextSeg(tmpSeg))
		    tmpSeg = _DtCvNextSeg(tmpSeg);

	        _DtCvNextSeg(tmpSeg)  = _DtCvNextSeg(snref->seg);
	        _DtCvNextDisp(tmpSeg) = _DtCvNextDisp(snref->seg);
	      }

	    /*
	     * does the snref have an id?
	     */
	    if (nullptr != snref->id)
	      {
		/*
		 * re-use the old snref, turning it into a marker
		 */
		_DtCvIdOfMarkerSeg(snref->seg) = snref->id;
		snref->seg->type = _DtCvSetTypeToMarker(snref->seg->type);

		/*
		 * set the next pointers on the old segment to
		 * the new segment.
		 */
		if (nullptr != newSeg)
		  {
		    _DtCvNextSeg(snref->seg)  = newSeg;
		    _DtCvNextDisp(snref->seg) = newSeg;
		  }
	      }

	    /*
	     * else move the resolved segment on top of the old snref.
	     */
	    else if (nullptr != newSeg)
	      {
		/*
		 * free the private information. No longer needed.
		 */
		free(snref->seg->client_use);

		/*
		 * now trounce the snref segment info.
		 */
		*(snref->seg) = *newSeg;

		/*
		 * free the duplicate new segment.
		 */
		if (_DtCvIsSegContainer(newSeg))
		    _DtCvContainerListOfSeg(newSeg) = nullptr;

		free(newSeg);
	      }
	    /*
	     * else there was no resolution, leave this segment as a NOOP.
	     */

	    /*
	     * go to the next unresolved snref
	     */
	    snref++;
	    my_struct->snref_cnt--;
	  }

	free(my_struct->un_snrefs);
	my_struct->un_snrefs = nullptr;
      }

    my_struct->cur_link  = curLink;
    my_struct->prev_data = prevData;
    my_struct->last_seg  = lastSeg;
    my_struct->my_fonts  = oldFonts;
    return 0;
}

/******************************************************************************
 * Function:    char *GetInterpCmd (SdlOption interp_type);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Return the command associated with the interpreter type.
 *
 ******************************************************************************/
static char *
GetInterpCmd(SdlOption	interp_type)
{
    const _CEInterpData *interp = InterpData;

    while (interp->type != _DtCvOPTION_BAD)
      {
	if (interp->type == interp_type)
	    return interp->cmd;

	interp++;
      }

    return nullptr;

}

/******************************************************************************
 * Function:    int CheckForSnb (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
CheckForSnb(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int    result = 0;

    /*
     * If the head has an snref in it, get the snb and resolve them
     */
    if (my_struct->snref_used == True)
      {
	_DtCvSegment	*oldSegList = my_struct->seg_list;
	_DtCvSegment	*oldAddSeg  = my_struct->add_seg;
	_DtCvSegment	*oldPrevSeg = my_struct->prev_data;
	_DtCvSegment	*oldLastSeg = my_struct->last_seg;

	my_struct->seg_list  = nullptr;
	my_struct->add_seg   = nullptr;
	my_struct->prev_data = nullptr;
	my_struct->last_seg  = nullptr;

	result = ProcessSDLMarkup(my_struct, SdlElementSnb, SdlElementNone,
						exceptions, process_mask);
	my_struct->seg_list  = oldSegList;
	my_struct->add_seg   = oldAddSeg;
	my_struct->prev_data = oldPrevSeg;
	my_struct->last_seg  = oldLastSeg;

	/*
	 * if no problems getting the snb, go through the items and
	 * resolve the snrefs.
	 */
	if (result != -1)
	    result = FindAndFix(my_struct);
      }

    return result;

} /* End CheckForSnb */

/******************************************************************************
 * Function:    int SetSaveSnref (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
SetSaveSnref(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    my_struct->save_snref = _DtCvTRUE;
    return 0;

} /* End SetSaveSnref */

/******************************************************************************
 * Function:    int ClearAndCheckSnref (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
ClearAndCheckSnref(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int result = 0;

    /*
     * reset the flag for saving snref elements
     */
    my_struct->save_snref = _DtCvFALSE;

    /*
     * was any snrefs found? If so, resolve them now.
     */
    if (my_struct->snref_used == True)
	result = FindAndFix(my_struct);

    return result;

} /* End ClearAndCheckSnref */

/******************************************************************************
 * Function:    int CheckType (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
CheckType(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    SdlOption	 newOpt;

    /*
     * cannot honor newlines in super or sub scripts.
     */
    newOpt = ElFrmtType(my_struct);
    if (SdlTypeLiteral == newOpt)
	newOpt = SdlTypeUnlinedLiteral;
    else if (SdlTypeLined == newOpt)
	newOpt = SdlTypeDynamic;

    ElFrmtType(my_struct) = newOpt;
    if (SdlTypeDynamic != newOpt)
	my_struct->flags = _DtCvSetTypeToNonBreak(my_struct->flags);

    if (SdlClassSub == ElClan(my_struct))
        my_struct->flags = _DtCvSetTypeToSubScript(my_struct->flags);
    else if (SdlClassSuper == ElClan(my_struct))
        my_struct->flags = _DtCvSetTypeToSuperScript(my_struct->flags);

    /*
     * strip hypertext links
     */
    my_struct->cur_link = -1;

    return 0;

} /* End CheckType */

/******************************************************************************
 * Function:    int SetType (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
SetType(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    if (ElType(my_struct) == SdlElementSphrase)
      {
        ContainerPtrToType(my_struct->active_frmt) =
				   (_DtCvFrmtOption) SdlTypeLiteral;
        ElFrmtType(my_struct)                      = SdlTypeLiteral;
        my_struct->flags                           = 0;
      }
    else
        ElFrmtType(my_struct) = SdlTypeCdata;

    if (SdlElementScript == ElType(my_struct))
      {
	/*
	 * remember the interpretor value
	 */
	FrmtPrivateInfo *priv = FrmtPrivInfoPtr(my_struct->add_seg);

	priv->interp  = ElInterp(my_struct);
      }

    return 0;

} /* End SetType */

/******************************************************************************
 * Function:    int ElseInfo (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
IfInfo(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    if (ElType(my_struct) == SdlElementIf)
      {
	my_struct->then_prev = nullptr;
	my_struct->else_prev = nullptr;
      }
    else if (ElType(my_struct) == SdlElementThen)
        my_struct->then_prev = my_struct->prev_data;
    else /* if (ElType(my_struct) == SdlElementElse) */
        my_struct->else_prev = my_struct->prev_data;

    return 0;

} /* End IfInfo */

/******************************************************************************
 * Function:    int MarkFound (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
MarkFound(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    my_struct->snref_used = True;

    /*
     * is there a newline hanging around that needs turning into a space?
     */
    if (SDLSearchMask(process_mask, SdlElementCdata)
	&& my_struct->last_was_space == False && my_struct->last_was_nl == True)
      {
	_DtCvSegment *pSeg = my_struct->prev_data;

	/*
	 * tack the space onto the end of previous string if possible.
	 */
	if (pSeg != nullptr && _DtCvIsSegString(pSeg) &&
			_DtCvIsSegRegChar(pSeg) && !(_DtCvIsSegNewLine(pSeg)))
	  {
	    char *strPtr;
	    char *grown;
	    int   len = _DtCvStrLen(_DtCvStringOfStringSeg(pSeg), 0);

	    grown = (char *) realloc(_DtCvStringOfStringSeg(pSeg), len+2);
	    if (grown == nullptr)
		return -1;
	    _DtCvStringOfStringSeg(pSeg) = grown;

	    strPtr    = (char *) _DtCvStrPtr(_DtCvStringOfStringSeg(pSeg),
								0, len);
	    *strPtr++ = ' ';
	    *strPtr   = '\0';
	  }
	else if (MySaveString(&(my_struct->seg_list), my_struct, " ",
				my_struct->cur_link, 1, False ) != 0)
	    return -1;

	my_struct->last_was_space = True;
	my_struct->last_was_nl    = False;
      }

    return 0;

} /* End MarkFound */

/******************************************************************************
 * Function:    int SaveItemInfo (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
SaveItemInfo(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    _DtCvSegment	 *refItem = my_struct->add_seg;
    SdlMatchData *info;

    if (SDLSearchMask(process_mask, SdlElementRefItem) == False)
	return 0;

    info = (SdlMatchData *) malloc (sizeof(SdlMatchData));
    if (nullptr == info)
	return -1;

    /*
     * save the clan, ssi & level of this refitem
     */
    *info = my_struct->el_info.match;
    SegMatchData(refItem) = (void *) info;

    ClearAttrFlag(my_struct->el_info, SDL_ATTR_SSI);

    return 0;

} /* End SaveItemInfo */

/******************************************************************************
 * Function:    int AllocateBlock (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 *
 ******************************************************************************/
static	int
AllocateBlock(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    /*
     * allocate a block of information all at once for later segments.
     * This, hopefully, will save processing time when later freed.
     *
     * Simply change the malloc_size in the formatting struct.
     */
    if (my_struct->add_seg != nullptr)
      {
        if (SDLIsAttrSet(my_struct->el_info, SDL_ATTR_COUNT))
	    my_struct->malloc_size = ElCount(my_struct);
        else
          {
            my_struct->malloc_size = 500;
            if (ElType(my_struct) == SdlElementToss)
                my_struct->malloc_size = 160;
          }
      }

    return 0;

} /* End AllocateBlock */

/******************************************************************************
 * Function:    int CleanUpBlock (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 *
 ******************************************************************************/
static	int
CleanUpBlock(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    /*
     * Go back to piece meal allocation.
     */
    my_struct->malloc_size = 1;
    return 0;

} /* End CleanUpBlock */

/******************************************************************************
 * Function:    int RegisterLink (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
RegisterLink(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int		 winType = _DtCvWindowHint_CurrentWindow;
    SDLId	 rid     = _DtCvContainerIdOfSeg(my_struct->add_seg);

    if (SDLSearchMask(process_mask, SdlElementLink) == False)
	return 0;

    switch (ElWindow(my_struct))
      {
	case SdlWindowNew: winType = _DtCvWindowHint_NewWindow;
			break;
	case SdlWindowPopup: winType = _DtCvWindowHint_PopupWindow;
			break;
      }

    my_struct->cur_link = _DtLinkDbAddLink(my_struct->my_links, nullptr, rid,
				_DtCvLinkType_SameVolume, winType, nullptr);

    if (rid == nullptr || my_struct->cur_link < 0)
	return -1;

    /*
     * indicate that at least a blank should be saved for the link
     */
    my_struct->save_blank = True;

    return 0;

} /* End RegisterLink */

/******************************************************************************
 * Function:    int ResolveIf (FormatStruct my_struct)
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
ResolveIf(
    FormatStruct	*my_struct,
    _DtCvSegment	*prev_data)
{

    _DtCvSegment	*ifSeg       = my_struct->add_seg;

    _DtCvSegment	*condSeg     = _DtCvContainerListOfSeg(ifSeg);
    _DtCvSegment	*condDataSeg = _DtCvContainerListOfSeg(condSeg);

    _DtCvSegment	*thenSeg     = _DtCvNextSeg(condSeg);
    _DtCvSegment	*elseSeg     = _DtCvNextSeg(thenSeg);

    _DtCvSegment	*resolveSeg  = nullptr;
    _DtCvSegment	*nextDisp    = nullptr;
    _DtCvSegment	*el	     = nullptr;
    char		*ifData      = nullptr;
    char		*interpStr;

    interpStr = GetInterpCmd(ElInterp(my_struct));
    if (nullptr == interpStr)
	return -1;

    if (_DtCvRunInterp(my_struct->ui_info->exec_filter,
				my_struct->ui_info->client_data,
				interpStr,
			_DtCvStringOfStringSeg(condDataSeg), &ifData) == 0)
      {
	/*
	 * Get the pointer to the next displayable item in the 'then'
	 * list. This usually will point into the 'then' list,
	 * but may point into the 'else' list.
	 */
	if (my_struct->then_prev != nullptr)
	    nextDisp = my_struct->then_prev->next_disp;

	/*
	 * use the 'then' data
	 */
        if (atoi (ifData) != 0)
	  {
	    el = thenSeg;

	    /*
	     * check to make sure that the next_disp is NOT into the
	     * 'else' list (because it's about to become a dangling
	     * next_disp if it is!).
	     */
	    if (elseSeg != nullptr)
	      {
		/*
		 * if the next displayable segment is in the 'else'
		 * list, null out the next displayable segement since
		 * there isn't anything in the 'then' list.
		 */
	        if (my_struct->then_prev == my_struct->else_prev)
		    nextDisp = nullptr;
		else
		    /*
		     * terminate the displayable segment list
		     * before the 'else' list.
		     */
		    my_struct->else_prev->next_disp = nullptr;
	      }
	  }
        else if (elseSeg != nullptr)
	  {
	    /*
	     * use the 'else' data.
	     */
	    el = elseSeg;

	    /*
	     * Get the next displayable item in the 'else' list.
	     */
	    if (my_struct->else_prev != nullptr)
	        nextDisp = my_struct->else_prev->next_disp;
	  }

        if (el != nullptr)
          {
	    resolveSeg   = _DtCvContainerListOfSeg(el);
	    _DtCvContainerListOfSeg(el) = nullptr;
          }

	free(ifData);
      }

    /*
     * set the true next displayable pointer.
     */
    if (prev_data != nullptr)
	prev_data->next_disp = nextDisp;

    /*
     * set the previous displayable segment to the last displayable
     * segment in the 'if' clause; in case more displayable segments
     * follow.
     */
    my_struct->prev_data = nextDisp;

    /*
     * free the no longer needed if construct
     */
    _DtHelpFreeSegments(my_struct->add_seg, _DtCvFALSE,
					my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);

    /*
     * set the add segment to the result of the if
     */
    my_struct->add_seg = resolveSeg;

    return 0;

} /* End ResolveIf */

/******************************************************************************
 * Function:    int ZeroOrOne (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
ZeroOrOne(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int    result = 0;

    if (my_struct->parsed == SdlElementNone)
        result = _DtHelpCeReturnSdlElement(my_struct->my_file, SdlElementList,
			SDLSearchMask(element_types, SdlElementCdata),
				&(my_struct->parsed), &(my_struct->remember),
				&(my_struct->end_flag));

    if (result == 0 && my_struct->end_flag == False &&
		SDLSearchMask(element_types, my_struct->parsed) == True)
      {
	/*
	 * check to make sure this is *not* Cdata or if it is that the
	 * PcDataFollows flag is *not* set.
	 */
	if (my_struct->parsed != SdlElementCdata ||
		SDLSearchMask(element_types, SdlPcDataFollows) == False)
            result = ParseSDL(my_struct, my_struct->parsed, sig_element,
						exceptions, process_mask);
      }
    else if (result == 1) /* eof on compressed entry/file */
	result = 0;

    return result;
} /* End ZeroOrOne */

/******************************************************************************
 * Function:    int ZeroToN (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
ZeroToN(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    _DtCvValue done   = False;
    int       result = 0;

    while (result == 0 && !done)
      {
        if (my_struct->parsed == SdlElementNone)
            result = _DtHelpCeReturnSdlElement(my_struct->my_file, SdlElementList,
			SDLSearchMask(element_types, SdlElementCdata),
				&(my_struct->parsed), &(my_struct->remember),
				&(my_struct->end_flag));

	if (result == 0 && my_struct->end_flag == False &&
		SDLSearchMask(element_types, my_struct->parsed) == True)
          {
	    /*
	     * check to make sure this is *not* Cdata or if it is that the
	     * PcDataFollows flag is *not* set.
	     */
	    if (my_struct->parsed != SdlElementCdata ||
		      SDLSearchMask(element_types, SdlPcDataFollows) == False)
                result = ParseSDL(my_struct, my_struct->parsed,
					sig_element, exceptions, process_mask);
	    else
		done = True;
	  }
	else
	  {
	    if (result == 1) /* eof on compressed entry/file */
	        result = 0;
	    done   = True;
	  }
      }

    return result;

} /* End ZeroToN */

/******************************************************************************
 * Function:    int OneToN (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
OneToN(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int    found  = False;
    int    result = 0;
    int    done   = False;

    while (result == 0 && !done)
      {
        if (my_struct->parsed == SdlElementNone)
            result = _DtHelpCeReturnSdlElement(my_struct->my_file, SdlElementList,
			SDLSearchMask(element_types, SdlElementCdata),
				&(my_struct->parsed), &(my_struct->remember),
				&(my_struct->end_flag));

	if (result == 0 && my_struct->end_flag == False &&
		SDLSearchMask(element_types, my_struct->parsed) == True)
	  {
	    found = True;
            result = ParseSDL(my_struct, my_struct->parsed, sig_element,
						exceptions, process_mask);
	  }
	else
	    done = True;
      }

    if (!found)
	result = -1;

    return result;

} /* End OneToN */

/******************************************************************************
 * Function:    int OnlyOne (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
OnlyOne(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    _DtCvValue found  = False;
    int       result = 0;

    if (my_struct->parsed == SdlElementNone)
        result = _DtHelpCeReturnSdlElement(my_struct->my_file, SdlElementList,
			SDLSearchMask(element_types, SdlElementCdata),
				&(my_struct->parsed), &(my_struct->remember),
				&(my_struct->end_flag));

    if (result == 0 && my_struct->end_flag == False &&
		SDLSearchMask(element_types, my_struct->parsed) == True)
      {
	found = True;
        result = ParseSDL(my_struct, my_struct->parsed, sig_element,
						exceptions, process_mask);
      }

    if (!found)
	result = -1;

    return result;

} /* End OnlyOne */

/******************************************************************************
 * Function:    int OnlyOneEach (
 *						FormatStruct my_struct,
 *						int element_types,
 *						int exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
OnlyOneEach(
    FormatStruct	*my_struct,
    SDLMask		*element_types,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int       result = 0;
    enum SdlElement myEl;
    SDLMask	myMask[SDL_MASK_LEN];

    SaveRestoreMask(myMask, element_types);

    while (result == 0 && SDLCheckMask(myMask))
      {
        if (my_struct->parsed == SdlElementNone)
            result = _DtHelpCeReturnSdlElement(my_struct->my_file, SdlElementList,
			SDLSearchMask(myMask, SdlElementCdata),
				&(my_struct->parsed), &(my_struct->remember),
				&(my_struct->end_flag));

        if (result == 0 && my_struct->end_flag == False &&
		SDLSearchMask(element_types, my_struct->parsed) == True)
          {
	    myEl   = my_struct->parsed;
            result = ParseSDL(my_struct, my_struct->parsed, sig_element,
						exceptions, process_mask);

	    SDLStripFromMask(myMask, myEl);
          }
	else
	    result = -1;
      }

    if (SDLCheckMask(myMask))
	result = -1;

    return result;

} /* End OnlyOneEach */


/******************************************************************************
 * Function:    int Cdata (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
Cdata(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int		 i;
    int		 reason;
    int		 myLen    = 0;
    int		 curLen   = my_struct->mb_len;
    int		 multiLen = my_struct->mb_len;
    int		 saveLen  = my_struct->mb_len;
    char	 nonBreakChar;
    char	*string   = nullptr;
    char	 spaceStr[] = " ";
    char	 dashStr[]  = "-";
    SdlOption type     = ElFrmtType(my_struct);
    _DtCvSegment	*pSeg       = nullptr;
    _DtCvValue	 nlToSpace   = True;
    _DtCvValue	 processFlag = SDLSearchMask(process_mask, SdlElementCdata);

    if (my_struct->ui_info->nl_to_space == 0)
	nlToSpace = False;

    if (type == SdlTypeCdata)
      {
	/*
	 * the element requires straight cdata for processing - i.e.
	 * the data is going to be passed off to an interperter.
	 * Therefore it doesn't want it broken up into different
	 * byte length segments.
	 *
	 * Therefore, force the string save to put all the data into
	 * one string.
	 */
	saveLen = 1;
      }

    if (my_struct->remember != nullptr)
      {
        i   = 0;
        string  = my_struct->remember;
	myLen   = strlen(string);
        while (string[i] != '\0' && i < myLen)
          {
            if (multiLen != 1)
                curLen = mblen (&string[i], multiLen);

	    if (curLen == 1)
	      {
		if (my_struct->last_was_nl == True)
		  {
		    if (MoveString(&string, &myLen, &i) == -1)
			return -1;

		    string[i++] = ' ';
		    my_struct->last_was_space = True;
		  }

		my_struct->last_was_nl = False;
		my_struct->last_was_mb = False;

                if (string[i] == '\t')
                    string[i] = ' ';
    
                if (string[i] == '&')
                  {
                    strmove (&string[i], &string[i+1]);
                    if (string[i] == '\0')
                      {
                        int rawc = BufFileGet(my_struct->my_file);
                        if (rawc == BUFFILEEOF)
                            return -1;
                        string[i] = (char)rawc;
                        string[i+1] = '\0';
                      }
    
		    /*
		     * is this an SGML numeric character reference
		     * entity?  if so, it should have the format
		     * '&#d[d[d]][;]' where 'ddd' represent characters
		     * of '0' to '9'.  The semi-colon is required iff
		     * the next character is a numeric character of '0'
		     * to '9'.  Otherwise it is optional.
		     */
		    if (string[i] == '#')
		      {
			int j;
			int value;

#define	ESC_STRING_LEN	4
			i++;

			/*
			 * Is there enough to room to process three digits
			 * and a possible semi-colon?
			 */
			if (myLen - i < ESC_STRING_LEN)
			  {
			    /*
			     * lengthen the string so that it can contain
			     * the information
			     */
			    char *newString = (char *) realloc(string,
				sizeof(char) * (myLen + ESC_STRING_LEN + 1));

			    if (newString == nullptr)
			      {
				/*
				 * 'string' is still valid; hand it back
				 * so the clean up frees the right block.
				 */
				my_struct->remember = string;
				return -1;
			      }

			    myLen  += ESC_STRING_LEN;
			    string  = newString;
			  }

			/*
			 * now make sure that the entire numeric entity
			 * exists in the string.
			 */
			j = i;
			while ('0' <= string[i] && string[i] <= '9')
			    i++;

			/*
			 * run into the end of string before running
			 * into a delimiter? Fill out the escaped
			 * numeric character.
			 */
			if (string[i] == '\0')
			  {
			    do
			      {
			        int rawc = BufFileGet(my_struct->my_file);
				if (rawc == BUFFILEEOF)
				    return -1;
			        string[i] = (char)rawc;
				i++;
			      } while (i < myLen && '0' <= string[i-1]
							&& string[i-1] <= '9');
			    /*
			     * end the string and back up to the last
			     * character
			     */
			    string[i] = '\0';
			    i--;
			  }

			/*
			 * the fourth character is a numeric, error
			 */
			if ('0' <= string[i] && string[i] <= '9')
			    return -1;

			if (string[i] == ';')
			    i++;

			value = atoi(&string[j]);
			if (value > 255)
			    return -1;

			/*
			 * smash over the pound sign with the 'real' value
			 * and copy the rest of the string to after it.
			 */
			string[j-1] = (char) value;
                        strmove (&string[j], &string[i]);
			i = j;
		      }

                    if (string[i] == '\n')
                      {
                        if (ProcessString(my_struct, True, True, True,
					True,
					processFlag, string, saveLen, &i) != 0)
			    return -1;
                      }
		    else if (string[i] == ' ')
		      {
			if (ProcessNonBreakChar(my_struct, processFlag,
					spaceStr, string, saveLen, &i) != 0)
			    return -1;
		      }
		    else if (string[i] == '-')
		      {
			if (ProcessNonBreakChar(my_struct, processFlag,
					dashStr, string, saveLen, &i) != 0)
			    return -1;
		      }
		    else
		        my_struct->last_was_space = False;
                    i++;
                  }
                else if (string[i] == '\n')
                  {
		    /*
		     * want to keep the newlines
		     */
		    if (type == SdlTypeCdata)
			i++;
                    else if (type == SdlTypeDynamic ||
						type == SdlTypeUnlinedLiteral)
                      {
                        if (my_struct->last_was_space == False)
			    my_struct->last_was_nl = True;

                        strmove (&string[i], &string[i+1]);
                      }
                    else
                      {
                        string[i] = '\0';
                        if (processFlag == True &&
                                    MySaveString(&(my_struct->seg_list),
					my_struct, string, my_struct->cur_link,
                                        saveLen, True) != 0)
                          {
                            MyFree(string);
                            return -1;
                          }
    
                        strmove (string, &string[i+1]);
                        i = 0;
                      }
                  }
                else if (string[i] == ' ')
                  {
                    if (False == my_struct->save_blank &&
			type != SdlTypeLiteral && type != SdlTypeUnlinedLiteral
					&& my_struct->last_was_space == True)
                        strmove (&string[i], &string[i+1]);
                    else
                        i++;
                    my_struct->last_was_space = True;
                  }
                else
                  {
                    my_struct->last_was_space = False;
                    i++;
                  }
              }
	    else if (curLen > 0)
	      {
		if (my_struct->last_was_nl == True)
		  {
		    if (nlToSpace == True || my_struct->last_was_mb == False)
		      {
		        if (MoveString(&string, &myLen, &i) == -1)
			    return -1;

		        string[i++] = ' ';
		      }
		    else /* the last was a multibyte character, tighten up */
		      {
			i--;
			strmove (&string[i], &string[i+1]);
		      }
		  }

		my_struct->last_was_space = False;
		my_struct->last_was_nl    = False;
		my_struct->last_was_mb    = True;
		i += curLen;
	      }
	    else if (curLen == 0)
		return -1;
	    else /* if (curLen < 0) */
	      {
		/*
		 * must finish up the character
		 */
		int len = 0;
		char *newString;

		myLen = strlen(string);
		newString = (char *) realloc(string, myLen + multiLen + 1);
		if (newString == nullptr)
		  {
		    /*
		     * 'string' is still valid; hand it back so the
		     * clean up frees the right block.
		     */
		    my_struct->remember = string;
		    return -1;
		  }
		string = newString;

		while (curLen < 0 && len < multiLen) {
		    int rawc = BufFileGet(my_struct->my_file);

		    if (rawc == BUFFILEEOF) return -1;
		    string[myLen] = (char)rawc;

		    string[myLen + 1] = '\0';
		    curLen = mblen(&string[i], multiLen);
		    ++len;
		    ++myLen;
		}

		if (curLen < 0)
		    return -1;
	      }
          }

	if (processFlag == False)
	  {
	    free(string);
	    string = nullptr;
	    myLen  = 0;
	  }

	my_struct->remember = nullptr;
      }

    do {
        my_struct->parsed = SdlElementNone;
        reason = _DtHelpCeGetSdlCdata(my_struct->my_file, type, multiLen,
				nlToSpace,
				&my_struct->last_was_space,
				&my_struct->last_was_nl,
				&my_struct->last_was_mb,
				&nonBreakChar,
			(processFlag == True ? &string : ((char**)nullptr)),
				&myLen);
        if (reason < 0)
	    return -1;

	if (string != nullptr && *string != '\0')
	  {
	    /*
	     * save the string.
	     */
	    if (MySaveString(&(my_struct->seg_list), my_struct,
			string, my_struct->cur_link, saveLen,
			(1 == reason ? True : False)) != 0)
              {
	        MyFree(string);
	        return -1;
              }

	    /*
	     * indicate that a string was saved for the current link
	     */
	    my_struct->save_blank = False;

	    /*
	     * null the temp buffer.
	     */
	    string[0] = '\0';
 
	    /*
	     * reset flags if we stopped because of a newline.
	     */
	    if (1 == reason && (SdlTypeLiteral == type || SdlTypeLined == type))
	      {
		my_struct->last_was_space = True;
		my_struct->last_was_nl    = False;
	      }
	  }
	else if (reason == 1) /* stopped because of newline */
	  {
	    pSeg = my_struct->prev_data;

	    if (pSeg == nullptr || _DtCvIsSegNewLine(pSeg))
	      {
		if (_DtHelpCeAllocSegment(my_struct->malloc_size,
				&(my_struct->alloc_size),
				&(my_struct->block_list), &pSeg) != 0)
		    return -1;

		pSeg->type   = _DtCvSetTypeToNoop(pSeg->type);
		if (my_struct->prev_data != nullptr)
		    my_struct->prev_data->next_disp = pSeg;

		my_struct->prev_data = pSeg;
		_DtHelpCeAddSegToList(pSeg, &(my_struct->seg_list),
						&(my_struct->last_seg));
	      }

	    pSeg->type = _DtCvSetTypeToNewLine(pSeg->type);

	    my_struct->last_was_space = True;
	    my_struct->last_was_nl    = False;
	  }

	/*
	 * did we stop because of a non-breaking character?
	 */
	if (2 == reason && True == processFlag)
	  {
	    /*
	     * copy the non breaking character into a buffer.
	     */
	    if (1 > myLen)
	      {
		string = (char *) malloc (sizeof(char) * 32);
		myLen  = 32;
	      }
	    string[0] = nonBreakChar;
	    string[1] = '\0';

	    /*
	     * save the string.
	     */
	    my_struct->flags = _DtCvSetTypeToNonBreak(my_struct->flags);
	    if (MySaveString(&(my_struct->seg_list), my_struct,
			string, my_struct->cur_link, saveLen, False) != 0)
              {
	        MyFree(string);
	        return -1;
              }
	    my_struct->flags = my_struct->flags & ~(_DtCvNON_BREAK);

	    /*
	     * indicate that the non-breaking character is considered
	     * non-white space.
	     */
	    my_struct->last_was_space = False;

	    /*
	     * indicate that a string was saved for the current link
	     */
	    my_struct->save_blank = False;

	    /*
	     * null the temp buffer.
	     */
	    string[0] = '\0';
	  }

      } while (reason > 0);

    MyFree(string);
    return 0;

} /* End Cdata */

/******************************************************************************
 * Function:    int ProcessEnterAttr (
 *					FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 *
 ******************************************************************************/
static	int
ProcessEnterAttr(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    /*
     * save the enter string as part of this element's segment list
     */
    if (SDLIsAttrSet(my_struct->el_info, SDL_ATTR_ENTER) &&
	((int)strlen(ElEnter(my_struct))) > 0 &&
	MySaveString(&(my_struct->seg_list), my_struct, ElEnter(my_struct),
			my_struct->cur_link, my_struct->mb_len, False) != 0)
	    return -1;

    return 0;

} /* End ProcessEnterAttr */

/******************************************************************************
 * Function:    int ProcessExitAttr (
 *					FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 *
 ******************************************************************************/
static	int
ProcessExitAttr(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    /*
     * save the exit string as part of this element's segment list
     */
    if (SDLIsAttrSet(my_struct->el_info, SDL_ATTR_EXIT) &&
	((int)strlen(ElExit(my_struct))) > 0 &&
	MySaveString(&(my_struct->seg_list), my_struct, ElExit(my_struct),
			my_struct->cur_link, my_struct->mb_len, False) != 0)
	    return -1;

    return 0;

} /* End ProcessExitAttr */

/******************************************************************************
 * Function:    int FakeEnd (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
FakeEnd(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{

    my_struct->end_flag  = True;
    my_struct->faked_end = True;
    MaskToValue(cur_element, my_struct->parsed);

    return 0;

} /* End FakeEnd */

/******************************************************************************
 * Function:    int AddRowToTable (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
AddRowToTable(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    if (SDLSearchMask(process_mask, SdlElementFrowvec) != False)
      {
	ElTableCellIds(my_struct) = (char **) _DtCvAddPtrToArray(
					(void **) ElTableCellIds(my_struct),
					ElTableCellId(my_struct));

	if (nullptr == ElTableCellIds(my_struct))
	    return -1;

        ClearAttrFlag(my_struct->el_info, SDL_ATTR_CELLS);
      }

    return 0;

} /* End AddRowToTable */

/******************************************************************************
 * Function:    int SaveLangCharSet (
 *					FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
SaveLangCharSet(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    char **info;

    if (SDLSearchMask(process_mask, SdlElementText) == False ||
	!(SDLIsAttrSet(my_struct->el_info, SDL_ATTR_LANGUAGE) ||
		SDLIsAttrSet(my_struct->el_info, SDL_ATTR_CHARSET)))
	return 0;

    info = (char **) calloc (2, sizeof(char *));
    if (nullptr == info)
        return -1;

    if (SDLIsAttrSet(my_struct->el_info, SDL_ATTR_LANGUAGE))
	info[0] = ElLanguage(my_struct);
    if (SDLIsAttrSet(my_struct->el_info, SDL_ATTR_CHARSET))
	info[1] = ElCharSet(my_struct);

    ClearAttrFlag(my_struct->el_info, SDL_ATTR_LANGUAGE);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_CHARSET);

    _SdlSegLangChar(my_struct->add_seg) = (void *) info;
    return 0;

} /* End SaveLangCharSet */

/******************************************************************************
 * Function:    int CopyDocInfo (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
CopyDocInfo(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    SDLDocInfo *docInfo;

    if (SDLSearchMask(process_mask, SdlElementSdlDoc) == False)
	return 0;

    docInfo = (SDLDocInfo *) malloc (sizeof(SDLDocInfo));
    if (nullptr == docInfo)
        return -1;

    *(docInfo) = ElDocInfo(my_struct);
    _SdlDocInfoPtrLanguage(docInfo) = strdup(ElLanguage(my_struct));
    _SdlDocInfoPtrCharSet(docInfo)  = strdup(ElCharSet(my_struct));

    ClearAttrFlag(my_struct->el_info, SDL_ATTR_DOCID);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_SDLDTD);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_TIMESTAMP);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_FRST_PG);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_VERSION);

    FrmtPrivInfoPtr(my_struct->add_seg)->doc_info = (void *) docInfo;
    return 0;

} /* End CopyDocInfo */

/******************************************************************************
 * Function:    int CopyAnchorId (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
CopyAnchorId(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
     _DtCvSegment        *mySeg = my_struct->add_seg;

    /*
     * if we're not suppose to process this, skip.
     */
    if (SDLSearchMask(process_mask, SdlElementAnchor) == False)
	return 0;

    /*
     * copy the id.
     */
    _DtCvIdOfMarkerSeg(mySeg) = ElId(my_struct);

    /*
     * clear the flag so that it don't get freed.
     */
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_ID);

    return 0;

} /* End CopyDocInfo */

/******************************************************************************
 * Function:    int LoadGraphic (
 *					FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
LoadGraphic(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int	result = 0;

    if (SDLSearchMask(process_mask, SdlElementGraphic) != False)
      {
	/*
	 * get my container segment.
	 */
	_DtCvSegment        *mySeg = my_struct->add_seg;
	_DtCvSegment        *cvRegion;	/* Canvas Engine Region */

	result = -1;

	/*
	 * allocate a Canvas Engine region.
	 */
	if (_DtHelpCeAllocSegment(1, nullptr, nullptr, &cvRegion) == 0)
	  {
	    /*
	     * got memory for a region, now fill out the information.
	     *
	     * even if the load fails, we should just throw away
	     * the graphic and continue.
	     */
	    result = 0;
	    if (nullptr != my_struct->ui_info->load_graphic &&
		(*(my_struct->ui_info->load_graphic))(
				my_struct->ui_info->client_data,
				my_struct->vol_name,
				my_struct->id_string,
				ElSnbXid(my_struct),
				ElSnbFormat(my_struct),
				ElSnbMethod(my_struct),
				&(_DtCvWidthOfRegionSeg(cvRegion)),
				&(_DtCvHeightOfRegionSeg(cvRegion)),
				&(_DtCvInfoOfRegionSeg(cvRegion))) == 0)
	      {
		/*
		 * set the type on the region! And its ascent!
		 */
		cvRegion->type = _DtCvSetTypeToRegion(cvRegion->type);
		_DtCvAscentOfRegionSeg(cvRegion) = -1;

		/*
		 * set the seg list for the wrapper container.
		 */
		_DtCvContainerListOfSeg(mySeg) = cvRegion;

		/*
		 * indicate the link has been fixed up
		 */
		my_struct->save_blank = True;
	      }
	    else
	      {
		/*
		 * problems loading the graphic. Clean up!
		 */
		MyFree(cvRegion);
	      }
	  }
      }
    return result;

} /* End LoadGraphic */

/******************************************************************************
 * Function:    int ColInfoToTableInfo (
 *					FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
ColInfoToTableInfo(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    if (SDLSearchMask(process_mask, SdlElementForm) != False)
      {
	int            i;
	int            mySize = ElTableColNum(my_struct);
	const char    *next;
	const char    *start     = nullptr;
	const char    *last      = nullptr;
	char         **colWidths = nullptr;
	_DtCvFrmtOption  *colJust;

	colWidths = (char **) malloc (sizeof(char *) * mySize);
	colJust   = (_DtCvFrmtOption *) malloc (sizeof(_DtCvFrmtOption)*mySize);

	if (nullptr == colWidths || nullptr == colJust)
	  {
	    MyFree(colWidths);
	    MyFree(colJust);
	    return -1;
	  }

	 /*
	  * now process the column width specification.
	  */
	next = ElTableColWStr(my_struct);
	if (nullptr == next || '\0' == *next)
	    next = "1";

	for (i = 0; i < mySize; i++)
	  {
	    /* skip the leading spaces */
	    while (' ' == *next) next++;

	    /* if the string really moved */
	    if (last != next)
		start = next;

	    /* go to the end of this specification */
	    while (' ' != *next && '\0' != *next) next++;

	    /* duplicate the specification */
	    colWidths[i] = strdup(start);
	    if (nullptr == colWidths[i])
		return -1;

	    /* mark the end of the string */
	    last = next;
	  }

	 /*
	  * now process the column justify specification.
	  */
	next = ElTableColJStr(my_struct);
	if (nullptr == next || '\0' == *next)
	    next = NullOption;

	for (i = 0; i < mySize; i++)
	  {
	    /* skip the leading spaces */
	    while (' ' == *next) next++;

	    /* if the string really moved */
	    if (last != next)
		start = next;

	    /* go to the end of this specification */
	    while (' ' != *next && '\0' != *next) next++;

	    /* determine the justification */
	    switch (*start)
	      {
		case 'r':
		case 'R': colJust[i] = _DtCvJUSTIFY_RIGHT;
			  break;
		case 'c':
		case 'C': colJust[i] = _DtCvJUSTIFY_CENTER;
			  break;
		case 'd':
		case 'D': colJust[i] = _DtCvJUSTIFY_NUM;
			  break;
		case 'l':
		case 'L': colJust[i] = _DtCvJUSTIFY_LEFT;
			  break;

		default : colJust[i] = _DtCvJUSTIFY_LEFT;
			  if (NullOption == start)
			      colJust[i] = _DtCvINHERIT;
			  break;
	      }

	    /* mark the end of the string */
	    last = next;
	  }

	ElTableColWidths(my_struct) = colWidths;
	ElTableColJust(my_struct)   = colJust;
      }

    return 0;

} /* End ColInfoToTableInfo */

/******************************************************************************
 * Function:    int CopyIdInfo (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
CopyIdInfo(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    SDLIdInfo *idInfo;

    if (SDLSearchMask(process_mask, SdlElementId) == False)
	return 0;

    idInfo = (SDLIdInfo *) malloc (sizeof(SDLIdInfo));
    if (nullptr == idInfo)
        return -1;

    *(idInfo) = ElIdInfo(my_struct);
    _SdlIdInfoPtrType(idInfo)   = ElFrmtType(my_struct);
    _SdlIdInfoPtrOffset(idInfo) = ElOffset(my_struct);
    _SdlIdInfoPtrRlevel(idInfo) = ElLevel(my_struct);
    _SdlIdInfoPtrRssi(idInfo)   = ElSsi(my_struct);

    ClearAttrFlag(my_struct->el_info, SDL_ATTR_RID);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_RSSI);

    FrmtPrivInfoPtr(my_struct->add_seg)->id_info = (void *) idInfo;
    return 0;

} /* End CopyIdInfo */

/******************************************************************************
 * Function:    int RegisterSnbLink (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
RegisterSnbLink(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int	  linkType;
    int   len = 0;
    char  buffer[64] = "";
    char *fileSpec = nullptr;

    if (SDLSearchMask(process_mask, ElType(my_struct)) == False)
	return 0;

    fileSpec = ElSnbXid(my_struct);

    switch (ElType(my_struct))
      {
	case SdlElementCrossDoc:
			linkType = _DtCvLinkType_CrossLink;
			break;

	case SdlElementManPage:
			linkType = _DtCvLinkType_ManPage;
			break;

	case SdlElementTextFile:
			if (SDLIsAttrSet(my_struct->el_info, SDL_ATTR_OFFSET))
			  {
			    sprintf(buffer, " %d", ElOffset(my_struct));
			    len += strlen(buffer);
			  }
			if (SDLIsAttrSet(my_struct->el_info, SDL_ATTR_FORMAT))
			    len += strlen(ElSnbFormat(my_struct));

			/*
			 * create a new file spec for the link
			 */
			if (0 < len)
			  {
			    len += strlen(ElSnbXid(my_struct) + 1);

			    fileSpec = malloc(sizeof(char) * len);
			    if (nullptr == fileSpec)
				return -1;
			
			    strcpy(fileSpec, ElSnbXid(my_struct));
			    strcat(fileSpec, buffer);
			    strcat(fileSpec, ElSnbFormat(my_struct));
			  }
			linkType = _DtCvLinkType_TextFile;
			break;

	case SdlElementSysCmd:
			linkType = _DtCvLinkType_Execute;
			break;

	case SdlElementCallback:
			linkType = _DtCvLinkType_AppDefine;
			break;

	default: return -1;
      }

    if (0 > _DtLinkDbAddLink(my_struct->my_links,
				_DtCvContainerIdOfSeg(my_struct->add_seg),
				fileSpec,
				linkType,
				_DtCvWindowHint_Original,
				nullptr))
	return -1;

    if (fileSpec != ElSnbXid(my_struct))
	free(fileSpec);

    return 0;

} /* End RegisterSnbLink */

/******************************************************************************
 * Function:    int RegisterSwitch (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
RegisterSwitch(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int		 result = -1;
    char	*interpStr;

    if (SDLSearchMask(process_mask, ElType(my_struct)) == False)
	return 0;

    interpStr = GetInterpCmd(ElInterp(my_struct));
    if (nullptr != interpStr)
	interpStr = strdup(interpStr);

    if (nullptr != interpStr)
      {
        if (0 == _DtLinkDbAddSwitch(my_struct->my_links,
			_DtCvContainerIdOfSeg(my_struct->add_seg),
			interpStr,
			_DtCvStringOfStringSeg(my_struct->add_seg),
			ElSwitchBranches(my_struct)))
	    result = 0;

	free(my_struct->add_seg);
	my_struct->add_seg = nullptr;
      }

    return result;

} /* End RegisterSwitch */

/******************************************************************************
 * Function:    int ResolveSpcInfo (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
ResolveSpcInfo(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    _DtCvSegment  *mySeg = my_struct->add_seg;

    if (SDLSearchMask(process_mask, ElType(my_struct)) != False)
      {
	/*
	 * set the non break character flag so that the layout routines
	 * will only wrap this to the next line if there is a space
	 * before it.
	 *
	 * Also set the inline flag so that the layout routines don't
	 * think that the region is a figure.
	 */
	mySeg->type = _DtCvSetTypeToNonBreak(mySeg->type);
	mySeg->type = _DtCvSetTypeToInLine(mySeg->type);

	/*
	 * now establish the proper display linking.
	 */
        if (my_struct->prev_data != nullptr)
            my_struct->prev_data->next_disp = mySeg;

	my_struct->prev_data  = mySeg;
	my_struct->save_blank = False;

	/*
	 * resolve the font hints.
	 */
	if (_SdlFontModeResolve == my_struct->resolve_font)
	    (*(my_struct->ui_info->resolve_spc))(
			my_struct->ui_info->client_data,
			ElLanguage(my_struct),
			ElCharSet(my_struct),
			*(my_struct->my_fonts),
			ElSpcName(my_struct),
			&(_DtCvInfoOfRegionSeg(my_struct->add_seg)),
			&(_DtCvWidthOfRegionSeg(my_struct->add_seg)),
			&(_DtCvHeightOfRegionSeg(my_struct->add_seg)),
			&(_DtCvAscentOfRegionSeg(my_struct->add_seg)));
	else
	  {
	    _DtHelpDARegion *pReg;
	    _DtHelpDASpcInfo *spcInfo;

	    /*
	     * malloc a structure to hold the spc
	     */
	    pReg = (_DtHelpDARegion *) malloc (sizeof(_DtHelpDARegion));
	    if (nullptr == pReg)
		return -1;
	    
	    /*
	     * malloc the structure to hold the information needed to
	     * create the spc later.
	     */
	    spcInfo = (_DtHelpDASpcInfo *) calloc (1, sizeof(_DtHelpDASpcInfo));
	    if (nullptr == spcInfo)
	      {
		free(pReg);
		return -1;
	      }

	    /*
	     * remember the spc's name and fonts
	     */
	    spcInfo->name = ElSpcName(my_struct);
	    if (_SdlFontModeSave == my_struct->resolve_font)
	      {
	        spcInfo->spc_fonts = *(my_struct->my_fonts);
		_DtHelpFontHintsLang(spcInfo->spc_fonts) =
							ElLanguage(my_struct);
		_DtHelpFontHintsCharSet(spcInfo->spc_fonts) =
							ElCharSet(my_struct);

		if (-1 == _DtHelpDupFontHints(&(spcInfo->spc_fonts)))
		  {
		    free(spcInfo);
		    free(pReg);
		    return -1;
		  }
	      }

	    /*
	     * remember the spc's name
	     */
	    pReg->inited = False;
	    pReg->type   = _DtHelpDASpc;
	    pReg->handle = (_DtCvPointer) spcInfo;

	    _DtCvInfoOfRegionSeg(mySeg) = (_DtCvPointer) pReg;
            ClearAttrFlag(my_struct->el_info, SDL_ATTR_NAME);
          }

      }

    return 0;

} /* End ResolveSpcInfo */

/******************************************************************************
 * Function:    int CopyTossInfo (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
CopyTossInfo(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    SDLTossInfo *tossInfo;

    if (SDLSearchMask(process_mask, ElType(my_struct)) == False)
	return 0;

    tossInfo = (SDLTossInfo *) malloc (sizeof(SDLTossInfo));
    if (nullptr == tossInfo)
        return -1;

    /*
     * save the flags
     */
    _SdlTossInfoPtrFlag1(tossInfo)     = ElFlag1(my_struct);
    _SdlTossInfoPtrFlag2(tossInfo)     = ElFlag2(my_struct);
    _SdlTossInfoPtrFlag3(tossInfo)     = ElFlag3(my_struct);
    _SdlTossInfoPtrFlag4(tossInfo)     = ElFlag4(my_struct);

    /*
     * save the match data - level, ssi and class/clan.
     */
    _SdlTossInfoPtrRlevel(tossInfo)    = ElLevel(my_struct);
    _SdlTossInfoPtrSsi(tossInfo)       = ElSsi(my_struct);
    _SdlTossInfoPtrClan(tossInfo)      = ElClan(my_struct);

    /*
     * save the table information (colj, colw) or the keystyle
     * enter/exit data.
     */
    _SdlTossInfoPtrStr1(tossInfo)      = ElString1(my_struct);
    _SdlTossInfoPtrStr2(tossInfo)      = ElString2(my_struct);

    /*
     * save the element type.
     */
    _SdlTossInfoPtrType(tossInfo)      = ElType(my_struct);

    /*
     * save the fonts - even if this toss style doesn't specify fonts.
     */
    _SdlTossInfoPtrFontSpecs(tossInfo) = *(my_struct->my_fonts);

    /*
     * now clear string attributes
     */
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_SSI);

    /*
     * table specs.
     */
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_COLW);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_COLJ);

    /*
     * key style specs.
     */
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_ENTER);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_EXIT);

    /*
     * font specs
     */
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_COLOR);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_XLFD);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_XLFDI);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_XLFDB);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_XLFDIB);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_TYPENAM);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_TYPENAMI);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_TYPENAMB);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_TYPENAMIB);

    /*
     * set the internal pointer
     */
    _SdlSegTossInfo(my_struct->add_seg) = (void *) tossInfo;

    /*
     * now re-initialize the font specifications back to the original
     * values
     */
    *(my_struct->my_fonts) = DefFontInfo;

    return 0;

} /* End CopyTossInfo */

/******************************************************************************
 * Function:    int CopyEntryInfo (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
CopyEntryInfo(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    SDLEntryInfo *entryInfo;

    if (SDLSearchMask(process_mask, ElType(my_struct)) == False)
	return 0;

    entryInfo = (SDLEntryInfo *) malloc (sizeof(SDLEntryInfo));
    if (nullptr == entryInfo)
        return -1;

    /*
     * save the strings.
     */
    *entryInfo = ElEntryInfo(my_struct);

    /*
     * now clear the attributes
     */
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_MAIN);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_LOCS);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_SYNS);
    ClearAttrFlag(my_struct->el_info, SDL_ATTR_SORT);

    /*
     * set the internal pointer
     */
    _SdlSegEntryInfo(my_struct->add_seg) = (void *) entryInfo;

    return 0;

} /* End CopyEntryInfo */

/******************************************************************************
 * Function:    int InitLast (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
InitLast(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    /*
     * set the parsing flags
     */
    my_struct->last_was_space = True;
    my_struct->last_was_mb    = False;
    my_struct->last_was_nl    = False;

    /*
     * set the container type correctly.
     */
    if (nullptr != my_struct->add_seg &&
	(SdlTypeLiteral == ElFrmtType(my_struct) ||
					SdlTypeLined == ElFrmtType(my_struct)))
	_DtCvContainerTypeOfSeg(my_struct->add_seg) = _DtCvLITERAL;

    return 0;

} /* End InitLast */

/******************************************************************************
 * Function:    int SetTransit (FormatStruct my_struct,
 *					int cur_element, exceptions);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Looks for the virtual page attributes.
 *
 ******************************************************************************/
static	int
SetTransit(
    FormatStruct	*my_struct,
    SDLMask		*cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    if (ElTiming(my_struct) == SdlTimingAsync)
        my_struct->resolve_font = _SdlFontModeNone;

    return 0;

} /* End SetTransit */


/******************************************************************************
 *
 * Main Parsing Functions
 *
 *****************************************************************************/
/******************************************************************************
 * Function:    int ParseSDL (FormatStruct my_struct,
 *				int cur_element, int cur_execpt);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Parses a set of rules.
 *
 ******************************************************************************/
static	int
ParseSDL(
    FormatStruct	*my_struct,
    enum SdlElement	 cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*cur_except,
    SDLMask		*process_mask)
{
    int    i = 0;
    int    result  = 0;
    int    oldLink = my_struct->cur_link;
    enum SdlElement	 oldElType   = ElType(my_struct);
    _DtCvSegment	*oldCurSeg   = my_struct->last_seg;
    _DtCvSegment	*oldSeglist  = my_struct->seg_list;
    _DtCvSegment	*oldAddSeg   = my_struct->add_seg;
    _DtCvSegment	*ifPrevData  = my_struct->prev_data;
    _SdlFontMode	 saveFontMode = my_struct->resolve_font;
    ElementInfo		 saveElInfo  = my_struct->el_info;
    _DtHelpFontHints    *oldFontInfo = my_struct->my_fonts;
    const SDLContent	*content;
    _DtCvContainer	*activeFrmt = my_struct->active_frmt;
    SDLMask		 oldMask[SDL_MASK_LEN];
    SDLMask		 oldExcept[SDL_MASK_LEN];
    SDLMask		 contentMask[SDL_MASK_LEN];
    _DtCvValue		 oldBlank    = my_struct->save_blank;
    _DtCvValue		 processFlag =
				SDLSearchMask(process_mask, SdlElementCdata);

    /*
     * While this element is allowed normally in the content of
     * the parent element, it it currently allowed?
     */
    if (SDLSearchMask(cur_except, cur_element) == True)
	return -1;

    /*
     * special processing for CDATA elements
     */
    if (cur_element != SdlElementCdata)
      {
        my_struct->seg_list = nullptr;
        my_struct->last_seg = nullptr;
        my_struct->add_seg  = nullptr;
      }

    /*
     * look in the master list for this element
     */
    while (i < MaxSDLElements && SdlElementList[i].sdl_element != cur_element)
	i++;

    /*
     * Didn't find this element in the master list.
     */
    if (i >= MaxSDLElements)
	return -1;

    /*
     * merge this element's exceptions with parent's list.
     * save the old process mask.
     */
    SaveRestoreMask(oldExcept, cur_except);
    MergeMasks(cur_except, SdlElementList[i].exceptions);
    SaveRestoreMask(oldMask, process_mask);

    /*
     * the content for this element is...
     */
    content = SdlElementList[i].content;

    /*
     * check to see if we want to process this element
     * If sig_element is set, parse the element and its content.
     */
    if (sig_element != SdlElementNone)
      {
	processFlag = False;
	if (sig_element == cur_element)
	  {
	    processFlag  = True;
	    sig_element  = SdlElementNone;
	    SaveRestoreMask(process_mask, AllMaskSet);
	  }
      }

    /*
     * If not CDATA, then have to get the element start string, attributes
     * and ending markup tag.
     */
    if (cur_element != SdlElementCdata)
      {
        if (ParseElementStart(my_struct,SdlElementList[i],processFlag) != 0
				||
		ParseElementAttr(my_struct,
					cur_element,
					SdlElementList[i].attrib_list,
					SdlElementList[i].cdata_flag,
					processFlag) != 0)
	    result = -1;
      }

    /*
     * now parse the element's content
     */
    my_struct->faked_end = False;
    ElType(my_struct) = cur_element;
    while (result == 0 && content != nullptr &&
			SDLSearchMask(content->mask, SdlElementNone) == False)
      {
	SaveRestoreMask(contentMask, content->mask);
	result = (*(content->model))(my_struct, contentMask,
					sig_element, cur_except, process_mask);
	content++;
      }

    /*
     * If not CDATA, get the element end markup and adjust some pointers
     */
    SaveRestoreMask(process_mask, oldMask);
    SaveRestoreMask(cur_except  , oldExcept);
    if (cur_element != SdlElementCdata)
      {
        if (result == 0 && SdlElementList[i].element_end_str != nullptr)
	    result = ParseElementEnd(my_struct, ifPrevData,
							SdlElementList[i],
							processFlag,
							my_struct->faked_end);
	else
	  {
	    /*
	     * free the attributes for this element.
	     */
	    FreeAttributes(ElType(my_struct),
				&(my_struct->el_info), my_struct->my_fonts);

	    if (SdlElementList[i].new_fonts && FreeFontInfo(my_struct) == -1)
	        result = -1;
	  }


        my_struct->last_seg = oldCurSeg;
        my_struct->seg_list = oldSeglist;

        if (result == 0 && my_struct->add_seg != nullptr)
	    _DtHelpCeAddSegToList(my_struct->add_seg, &(my_struct->seg_list),
						&(my_struct->last_seg));

        my_struct->add_seg   = oldAddSeg;
	my_struct->cur_link  = oldLink;
	if (SdlElementLink == cur_element)
            my_struct->save_blank = oldBlank;

	my_struct->el_info  = saveElInfo;
	my_struct->my_fonts = oldFontInfo;
      }

    my_struct->active_frmt  = activeFrmt;
    my_struct->resolve_font = saveFontMode;
    ElType(my_struct) = oldElType;
    return result;

} /* End ParseSDL */

/******************************************************************************
 * Function:    int ProcessSDLMarkup (FormatStruct my_struct,
 *				int cur_element, int cur_execpt);
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Parses a set of rules, looks through the result and changes
 *              bad options into good.
 *
 ******************************************************************************/
static	int
ProcessSDLMarkup(
    FormatStruct	*my_struct,
    enum SdlElement	 cur_element,
    enum SdlElement	 sig_element,
    SDLMask		*cur_except,
    SDLMask		*process_mask)
{
    int result = ParseSDL (my_struct, cur_element, sig_element, cur_except,
								process_mask);

    if (-1 != result)
	PropagateJustification(my_struct->seg_list, _DtCvJUSTIFY_LEFT);

    return result;
}

/******************************************************************************
 * Function:    int ProcessContent (
 *				FormatStruct *my_struct,
 *				const SDLContent *content,
 *				SDLMask *exceptions,
 *				SDLMask *process_mask);
 *
 * Parameters:
 *		my_struct	Specifies specific informationg for this parse.
 *		content		Specifies the content model to parse.
 *		exceptions	Specifies the current elements excepted
 *				from being in the current content.
 *		process_mask	Specifies which elements to save in memory.
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 ******************************************************************************/
static int
ProcessContent(
    FormatStruct	*my_struct,
    const SDLContent	*content,
    enum SdlElement	 sig_element,
    SDLMask		*exceptions,
    SDLMask		*process_mask)
{
    int   result = 0;
    SDLMask contentMask[SDL_MASK_LEN];

    while (result == 0 && content != nullptr &&
			SDLSearchMask(content->mask, SdlElementNone) == False)
      {
	SaveRestoreMask(contentMask, content->mask);
	result = (*(content->model))(my_struct, contentMask,
					sig_element, exceptions, process_mask);
	content++;
      }

    if (-1 != result)
	PropagateJustification(my_struct->seg_list, _DtCvJUSTIFY_LEFT);

    return result;
}

/******************************************************************************
 * Function:    int SearchForController (_DtCvSegment p_seg, 
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Parses a set of rules.
 *
 ******************************************************************************/
static	_DtCvSegment *
SearchForController(
    _DtCvSegment		*p_seg)
{
    while (nullptr != p_seg &&
		!(_DtCvIsSegContainer(p_seg) && _DtCvIsSegController(p_seg)))
	p_seg = _DtCvNextSeg(p_seg);

    return p_seg;

} /* End SearchForController */

/******************************************************************************
 * Function:    int FormatSDLTitle (
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     Parses a set of rules.
 *
 ******************************************************************************/
static	int
FormatSDLTitle(
    char		*filename,
    int			 offset,
    int			 fd,
    FormatStruct	*my_struct)
{
    int		result = 0;
    SDLMask	 processMask[SDL_MASK_LEN] = SDLSetAllBits;
/*
    SDLMask	processMask[SDL_MASK_LEN] = SDLInitMaskEleven( \
				SdlElementTitle  , SdlElementHead   , \
				SdlElementKey    , SdlElementSphrase, \
				SdlElementRev    , SdlElementIf     , \
				SdlElementSpc    , SdlElementAnchor , \
				SdlElementLink   , SdlElementSnRef  , \
				SdlElementCdata);
*/

    SDLMask	startExcept[SDL_MASK_LEN] = SDLInitMask(SdlElementNone);

    /*
     * set the volume name for the graphics.
     */
    my_struct->vol_name = filename;

    /*
     * can we seek to the correct place?
     */
    result = _DtHelpCeFileOpenAndSeek(filename, offset, fd,
					&(my_struct->my_file), nullptr);
    if (result != -1)
      {
        result = ProcessSDLMarkup (my_struct, SdlElementTitle, SdlElementNone,
				    startExcept, processMask);
        _DtHelpCeBufFileClose (my_struct->my_file, (fd == -1 ? True : False));
      }

    return result;

} /* End FormatSDLTitle */

/******************************************************************************
 * Function:    void SetGhostLink (
 *
 * Parameters:
 *
 * Returns:
 *
 * Purpose:
 *
 ******************************************************************************/
static	void
SetGhostLink(
    _DtCvLinkDb		 link_data,
    _DtCvSegment	*segments,
    int			 link_idx)
{
    while (segments != nullptr)
      {
	if (!(_DtCvIsSegContainer(segments)))
	  {
	    if (_DtCvIsSegHyperText(segments))
	      {
	        _DtLinkDbRemoveLink(link_data, segments->link_idx);
	        segments->type = ClearSegLinks(segments);
	      }

	    segments->type   = _DtCvSetTypeToGhostLink(segments->type);
	    segments->link_idx = link_idx;
	  }
	else /* if (_DtCvIsSegContainer(segments)) */
	    SetGhostLink (link_data, _DtCvContainerListOfSeg(segments),
								link_idx);
	segments = _DtCvNextSeg(segments);
      }

} /* End SetGhostLink */

/******************************************************************************
 * Function:    int CreateAsciiString ()
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if the string could not grow. In that
 *		case *ret_string is the valid, shorter string built so far.
 *
 * Purpose:
 *
 ******************************************************************************/
static	int
CreateAsciiString(
    _DtCvSegment	 *p_seg,
    _DtCvSegment	 *snb,
    _DtCvValue		 *nl_flag,
    char		**ret_string)
{
    int		 newLen;
    int		 len;
    char	*newString;

    while (p_seg != nullptr)
      {
	len = 1;
	if (_DtCvIsSegString(p_seg))
	  {
	    if (*nl_flag == True)
	      {
		if (*ret_string != nullptr &&
				    (*ret_string)[strlen(*ret_string)-1] != ' ')
		  len++;
		else
		  *nl_flag = False;
	      }

	    newLen = _DtCvStrLen(_DtCvStringOfStringSeg(p_seg),
						_DtCvIsSegWideChar(p_seg));
	    if (_DtCvIsSegWideChar(p_seg))
		newLen = newLen * MB_CUR_MAX;

	    len += newLen;

	    /*
	     * When the string cannot grow, stop here: *ret_string stays
	     * the valid, shorter string built so far (or NULL).
	     */
	    if (*ret_string == nullptr)
	      {
		newString = (char *) malloc (len);
		if (newString == nullptr)
		    return -1;

		*ret_string = newString;
		**ret_string = '\0';
	      }
	    else
	      {
		len += strlen(*ret_string);
		newString = (char *) realloc (*ret_string, len);
		if (newString == nullptr)
		    return -1;

		*ret_string = newString;
	      }

	    if (*nl_flag == True)
		strcat(*ret_string, " ");

	    /*
	     * back up to the insertion point.
	     */
	    len -= newLen;
	    len--;

	    /*
	     * wide char or single byte?
	     */
	    if (_DtCvIsSegWideChar(p_seg))
		wcstombs(&((*ret_string)[len]),
				(wchar_t *) _DtCvStringOfStringSeg(p_seg),
				newLen + 1);
	    else
	        strcpy (&((*ret_string)[len]), _DtCvStringOfStringSeg(p_seg));

	    *nl_flag = False;
	    if (p_seg->next_disp == nullptr)
		*nl_flag = True;
	  }
	else if (_DtCvIsSegContainer(p_seg) && !(_DtCvIsSegController(p_seg)))
	  {
	    if (CreateAsciiString(_DtCvContainerListOfSeg(p_seg),
					snb, nl_flag, ret_string) != 0)
		return -1;
	  }

	p_seg = _DtCvNextSeg(p_seg);
      }

    return 0;
}

/******************************************************************************
 * Function:    void CreateAsciiAbbrev ()
 *
 * Parameters:
 *
 * Returns:
 *
 * Purpose:
 *
 ******************************************************************************/
static	void
CreateAsciiAbbrev(
    _DtCvSegment	 *p_el,
    char		**ret_abbrev)
{
  *ret_abbrev = nullptr;
  if (nullptr != AbbrevOfSeg(p_el) && strlen ((char *) AbbrevOfSeg(p_el)))
	*ret_abbrev = strdup((char *) AbbrevOfSeg(p_el));
}

/******************************************************************************
 * Function:    void CleanUpToc ()
 *
 * Parameters:
 *		my_struct	Specifies current formatting information.
 *		p_seg		Specifies the current segment list to modify.
 *		level		Specifes the parent element's level.
 *		lnk_indx	Specifies the link index to use for the
 *				ghost link.
 *
 * Returns:     nothing
 *
 * Purpose:     Modifies the formatting information for conform to what
 *		it should be for a TOC and sets the ghost link.
 *
 ******************************************************************************/
static	void
CleanUpToc(
    FormatStruct	*my_struct,
    _DtCvSegment		*p_seg,
    int			 level,
    _DtCvValue		 target)
{
    level--;
    if (level < 0)
	level = 0;

    while (p_seg != nullptr)
      {
	if (_DtCvIsSegNewLine(p_seg))
	    p_seg->type = p_seg->type & ~(_DtCvNEW_LINE);

	if (_DtCvIsSegContainer(p_seg))
	  {
	    _DtCvContainerPercentOfSeg(p_seg)   = DefPercent;
	    _DtCvContainerOrientOfSeg(p_seg)    = _DtCvJUSTIFY_LEFT_MARGIN;
	    _DtCvContainerFlowOfSeg(p_seg)      = _DtCvWRAP_NONE;
	    _DtCvContainerTypeOfSeg(p_seg)      = _DtCvLITERAL;
	    _DtCvContainerVJustifyOfSeg(p_seg)  = _DtCvJUSTIFY_TOP;
	    _DtCvContainerJustifyOfSeg(p_seg)   = _DtCvJUSTIFY_LEFT;
	    _DtCvContainerFMarginOfSeg(p_seg)   = 0;
	    _DtCvContainerLMarginOfSeg(p_seg)   =
				level * 2 * my_struct->ui_info->avg_char;
	    _DtCvContainerRMarginOfSeg(p_seg)   = 0;
	    _DtCvContainerTMarginOfSeg(p_seg)   = 0;
	    _DtCvContainerBMarginOfSeg(p_seg)   = 0;

	    CleanUpToc(my_struct, _DtCvContainerListOfSeg(p_seg),
							level, target);
	  }
	else if (_DtCvIsSegString(p_seg))
	  {
	    _DtHelpFontHints *font_specs = (_DtHelpFontHints *)_DtCvFontOfStringSeg(p_seg);

	    _DtHelpFontPtrPtSize(font_specs) = 10;
	    _DtHelpFontPtrWeight(font_specs) = _DtHelpFontWeightMedium;

	    if (target == True)
	        _DtHelpFontPtrWeight(font_specs) = _DtHelpFontWeightBold;

	    (my_struct->ui_info->load_font)(my_struct->ui_info->client_data,
				_DtHelpFontHintsLang(*font_specs),
				_DtHelpFontHintsCharSet(*font_specs),
				*font_specs,
				&(_DtCvFontOfStringSeg(p_seg)));
	    free(font_specs);
	  }
	else if (_DtCvIsSegRegion(p_seg))
	  {
	    int              result;
	    _DtHelpDARegion *pReg = (_DtHelpDARegion *) _DtCvInfoOfRegionSeg(p_seg);

	    if (_DtHelpDASpc == pReg->type)
	      {
		_DtHelpDASpcInfo *pSpc = (_DtHelpDASpcInfo *) pReg->handle;

		_DtHelpFontHintsPtSize(pSpc->spc_fonts) = 10;
	        _DtHelpFontHintsWeight(pSpc->spc_fonts) =
							_DtHelpFontWeightMedium;

		if (True == target)
	            _DtHelpFontHintsWeight(pSpc->spc_fonts) =
							_DtHelpFontWeightBold;

	        result = (*(my_struct->ui_info->resolve_spc))(
				my_struct->ui_info->client_data,
				_DtHelpFontHintsLang(pSpc->spc_fonts),
				_DtHelpFontHintsCharSet(pSpc->spc_fonts),
				pSpc->spc_fonts,
				pSpc->name,
				&(_DtCvInfoOfRegionSeg(p_seg)),
				&(_DtCvWidthOfRegionSeg(p_seg)),
				&(_DtCvHeightOfRegionSeg(p_seg)),
				&(_DtCvAscentOfRegionSeg(p_seg)));
		if (0 != result)
		    p_seg->type = _DtCvSetTypeToNoop(p_seg->type);

		free(pSpc->name);
		_DtHelpFreeFontHints(&(pSpc->spc_fonts));

		free(pSpc);
		free(pReg);
	      }
	  }

	p_seg = _DtCvNextSeg(p_seg);
      }
}

/******************************************************************************
 * Function:    int AddEntryToToc ()
 *
 * Parameters:
 *
 * Returns:
 *
 * Purpose:
 *
 ******************************************************************************/
static	int
AddEntryToToc(
    FormatStruct	*my_struct,
    _DtHelpVolumeHdl	 volume,
    int			 fd,
    int			 level,
    char		*lang,
    const char		*char_set,
    _DtCvSegment	*toss,
    _DtCvSegment	*info_seg,
    _DtCvValue		 target,
    _DtCvSegment	**ret_snb,
    _DtCvSegment	**seg_list,
    _DtCvSegment	**prev_list)
{
    int		result = 0;
    _DtCvLinkDb	saveLinks;
    SDLIdInfo	*info;

    /*
     * initialize the structure
     * save some information that's going to be destroyed in the setup.
     */
    saveLinks    = my_struct->my_links;

    if (SetUp(nullptr,nullptr,my_struct,toss,my_struct->ui_info,fd,False,False) != 0)
	return -1;

    /*
     * Initialize the standard/default to use
     */
    _DtHelpFontHintsLang(*(my_struct->my_fonts))    = lang;
    _DtHelpFontHintsCharSet(*(my_struct->my_fonts)) = (char *) char_set;

    /*
     * free the new link database and restore the old one.
     * set font mode to save and the id for graphics.
     */
    _DtLinkDbDestroy(my_struct->my_links);
    my_struct->my_links     = saveLinks;
    my_struct->resolve_font = _SdlFontModeSave;

    info = FrmtPrivInfoPtr(info_seg)->id_info;
    result = FormatSDLTitle(_DtHelpCeGetVolumeName(volume),
				_SdlIdInfoPtrOffset(info), fd, my_struct);
    /*
     * if no errors, continue
     */
    if (result != -1)
      {
	_DtCvSegment *headEl  = nullptr;

	/*
	 * find the actual head element
	 */
	if (my_struct->seg_list != nullptr)
	    headEl = SearchForController(
				_DtCvContainerListOfSeg(my_struct->seg_list));

	/*
	 * If there isn't a head element, use the id.
	 */
	if (headEl == nullptr)
	  {
	    _DtCvSegment *addSeg = nullptr;

	    if (MySaveString(&(addSeg), my_struct,
					_DtCvContainerIdOfSeg(info_seg),
					-1, my_struct->mb_len, False) != 0)
	      {
		/*
		 * free the segments
		 */
		_DtHelpFreeSegments(my_struct->seg_list, _DtCvTRUE,
					my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);
		my_struct->seg_list = nullptr;
		DestroyFontInfo(my_struct);
		return -1;
	      }

	    /*
	     * if there is a virpage, attach the segment to it.
	     * I.e. just reuse the container.
	     */
	    if (nullptr != my_struct->seg_list)
	      {
		/*
		 * free the container original list
		 */
		_DtHelpFreeSegments(
				_DtCvContainerListOfSeg(my_struct->seg_list),
				_DtCvTRUE,
				my_struct->ui_info->destroy_region,
				my_struct->ui_info->client_data);
	      }
		else
		  {
		/*
		 * create a container for the segment.
		 */
		if (_DtHelpCeAllocSegment(my_struct->malloc_size,
                                &(my_struct->alloc_size),
                                &(my_struct->block_list),
				&(my_struct->seg_list)) != 0)
		  {
		    _DtHelpFreeSegments(addSeg, _DtCvTRUE,
				my_struct->ui_info->destroy_region,
				my_struct->ui_info->client_data);
		    DestroyFontInfo(my_struct);
		    return -1;
		  }

                /*
                 * set the container flag and initialize its formatting
                 * information to the default.
                 */
                my_struct->seg_list->type =
			_DtCvSetTypeToContainer(my_struct->seg_list->type);
                _DtCvContainerOfSeg(my_struct->seg_list) = DefFrmtSpecs;
		_DtCvContainerLeadingOfSeg(my_struct->seg_list) =
						my_struct->ui_info->leading;
	      }

	    _DtCvContainerListOfSeg(my_struct->seg_list) = addSeg;
	    headEl  = my_struct->seg_list;
	  }

	/*
	 * there was a empty head, use the abbreviation or the id.
	 */
	else if (_DtCvContainerListOfSeg(headEl) == nullptr)
	  {
	    char *myPtr = (char *) AbbrevOfSeg(headEl);

	    if (nullptr == myPtr)
	        myPtr = _DtCvContainerIdOfSeg(info_seg);

	    if (MySaveString(&(_DtCvContainerListOfSeg(headEl)), my_struct,
				myPtr, -1, my_struct->mb_len, False) != 0)
	      {
		/*
		 * free the segments
		 */
		_DtHelpFreeSegments(my_struct->seg_list, _DtCvTRUE,
					my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);
		my_struct->seg_list = nullptr;
		DestroyFontInfo(my_struct);
		return -1;
	      }
	  }

	/*
	 * Make sure we only use the first head.
	 * first make sure that the segment list is not the head already.
	 */
	if (headEl != my_struct->seg_list)
	  {
	    _DtCvSegment *prevSeg;

	    /*
	     * destroy the segments after this one.
	     */
	    _DtHelpFreeSegments(_DtCvNextSeg(headEl), _DtCvTRUE,
					my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);
	    /*
	     * break the link to the freed segments
	     */
	    _DtCvNextSeg(headEl) = nullptr;

	    /*
	     * destroy the segments before this one.
	     */
	    prevSeg = _DtCvContainerListOfSeg(my_struct->seg_list);
	    if (prevSeg != headEl)
	      {
		/*
		 * search for the previous segment before the head.
		 */
		while (_DtCvNextSeg(prevSeg) != headEl)
		    prevSeg = _DtCvNextSeg(prevSeg);

		/*
		 * break the link to the head element
		 */
		_DtCvNextSeg(prevSeg) = nullptr;
	      }
	    else
		_DtCvContainerListOfSeg(my_struct->seg_list) = nullptr;

	    /*
	     * free the segments before the head and virpage container.
	     */
	    _DtHelpFreeSegments(my_struct->seg_list, _DtCvTRUE,
					my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);
	    my_struct->seg_list = headEl;
	  }

	if (headEl != nullptr)
	  {
	    int	linkIndex = _DtLinkDbAddLink(my_struct->my_links, nullptr,
				_DtCvContainerIdOfSeg(info_seg),
					_DtCvLinkType_SameVolume,
					_DtCvWindowHint_CurrentWindow, nullptr);

	    CleanUpToc(my_struct, headEl, level, target);
	    SetGhostLink(my_struct->my_links, headEl, linkIndex);
	  }
      }

    if (result != -1)
      {
        /*
         * now tack this segment onto the end of the list
         */
        if ((*seg_list) == nullptr)
	    (*seg_list) = my_struct->seg_list;
        else
	    _DtCvNextSeg((*prev_list)) = my_struct->seg_list;

        *prev_list = my_struct->seg_list;
        while ((*prev_list) != nullptr && _DtCvNextSeg((*prev_list)) != nullptr)
	    *prev_list = _DtCvNextSeg((*prev_list));
      }

    /*
     * if the snb for this topic was read, free it now.
     */
    if (my_struct->snb != nullptr)
      {
	_DtHelpFreeSegments(my_struct->snb, _DtCvTRUE,
					my_struct->ui_info->destroy_region,
					my_struct->ui_info->client_data);
	my_struct->snb = nullptr;
      }

    /*
     * free the font structures allocated
     */
    DestroyFontInfo(my_struct);

    return result;
}

/******************************************************************************
 * Function:    int ExpandToc ()
 *
 * Parameters:
 *
 * Returns:
 *
 * Purpose:
 *
 ******************************************************************************/
static	int
ExpandToc(
    FormatStruct	*my_struct,
    _DtHelpVolumeHdl	 volume,
    int			 fd,
    char		*lang,
    const char		*char_set,
    _DtCvSegment	*toss,
    int			 level,
    char		**path_list,
    _DtCvSegment	**id_seg,
    _DtCvSegment	**ret_snb,
    _DtCvSegment	**seg_list,
    _DtCvSegment	**prev_list)
{
    int		 result   = 0;
    int		 segLev;
    int		 tst      = 1;
    _DtCvSegment *pEl;
    _DtCvValue	 done     = False;
    _DtCvValue	 found    = False;

    /*
     * skip anything that isn't a virpage and of the correct level.
     */
    while ((*id_seg) != nullptr && _SdlSegToSdlIdInfoType(*id_seg) != SdlIdVirpage
		&& _SdlSegToSdlIdInfoLevel(*id_seg) != level)
	*id_seg = _DtCvNextSeg((*id_seg));

    /*
     * process any virpage that has the correct level
     */
    while ((*id_seg) != nullptr && done == False && result == 0)
      {
	pEl    = (*id_seg);
	segLev = _SdlSegToSdlIdInfoLevel(pEl);

	if (_SdlSegToSdlIdInfoType(pEl) == SdlIdVirpage)
	  {
	    if (segLev == level)
	      {
		/*
		 * If the virpage in the path list has not been found,
		 * test the next virpage. Otherwise skip.
		 */
		if (found == False && *path_list != nullptr &&
					_DtCvContainerIdOfSeg(pEl) != nullptr)
		    tst   = _DtCvStrCaseCmpLatin1(*path_list,
						_DtCvContainerIdOfSeg(pEl));
			
		/*
		 * the only time tst == 0 is when the next item in the
		 * path_list matches this element. Check to see if the
		 * next item in the path_list is null. If so, that
		 * means this is the location the user has desired.
		 */
	        result = AddEntryToToc(my_struct, volume, fd,
			segLev, lang, char_set, toss, pEl,
			(tst == 0 && path_list[1] == nullptr ? True : False),
			ret_snb, seg_list, prev_list);

		/*
		 * increment the segment pointer to the next item
		 */
		*id_seg = _DtCvNextSeg((*id_seg));

		/*
		 * The only time tst is zero is if the current virpage
		 * matches the next item in the list. Expand it's children.
		 * and set tst to non-zero so that AddEntryToToc does not
		 * special case the siblings following this one.
		 */
		if (tst == 0)
		  {
		    result = ExpandToc(my_struct, volume, fd, lang,
					char_set, toss,
					segLev + 1,
					&path_list[1],
					id_seg, ret_snb, seg_list, prev_list);
		    found = True;
		    tst   = 1;
		  }
	      }
	    else if (segLev < level)
		done = True;
	    else
	        *id_seg = _DtCvNextSeg((*id_seg));
	  }
	else
	    *id_seg = _DtCvNextSeg((*id_seg));
      }

    return result;

} /* ExpandToc */

/******************************************************************************
 * Function:    int GrowChunks ()
 *
 * Parameters:
 *		chunks		The chunk list, or a NULL list to allocate.
 *		count		The number of entries the list must hold.
 *
 * Returns:     0 if successful, -1 if the list could not be allocated.
 *		In that case *chunks is unchanged and still valid.
 *
 * Purpose:     Allocate or enlarge a chunk list.
 *
 ******************************************************************************/
static	int
GrowChunks(
    void	***chunks,
    int		  count)
{
    void	**newChunks;

    if (count < 1 || (size_t) count > SIZE_MAX / sizeof(void *))
	return -1;

    if (*chunks == nullptr)
	newChunks = (void **) malloc (sizeof(void *) * count);
    else
	newChunks = (void **) realloc (*chunks, sizeof(void *) * count);

    if (newChunks == nullptr)
	return -1;

    *chunks = newChunks;
    return 0;
}

/******************************************************************************
 * Function:    void CreateTitleChunks ()
 *
 * Parameters:
 *
 * Returns:
 *
 * Purpose:
 *
 ******************************************************************************/
static	int
CreateTitleChunks(
    _DtCvSegment	  *toss,
    _DtCvSegment	  *p_seg,
    _DtCvSegment	  *snb,
    SDLMask		  *stop_mask,
    const char		  *lang,
    const char		  *char_set,
    _DtHelpResolveSpcProc resolve_spc,
    _DtCvPointer	  client_data,
    int			  *ret_cnt,
    void		***ret_chunks)
{
    long	 type;
    int		 cnt;
    int		 savedCnt;
    int		 result = 0;
    const char	*myLang;
    const char	*mySet;
    void	*ptr;
    _DtHelpDARegion	*daRegion;

    while (result != -1 && p_seg != nullptr)
      {
	myLang = lang;
	mySet  = char_set;
	cnt = *ret_cnt;
	savedCnt = *ret_cnt;
	if (_DtCvIsSegString(p_seg))
	  {
	    if (*ret_cnt == 0)
		*ret_cnt = 1;
	    else
		cnt--;

	    *ret_cnt = *ret_cnt + 3;

	    /*
	     * on failure the list built so far stays valid, with its
	     * old count.
	     */
	    if (GrowChunks(ret_chunks, *ret_cnt) != 0)
	      {
		*ret_cnt = savedCnt;
		return -1;
	      }

	    type = DT_HELP_CE_FONT_PTR | DT_HELP_CE_STRING;
	    ptr  = _DtCvFontOfStringSeg(p_seg);

	    if (p_seg->next_disp == nullptr || _DtCvIsSegNewLine(p_seg))
		type |= DT_HELP_CE_NEWLINE;

	    (*ret_chunks)[cnt++] = (void *) type;
	    (*ret_chunks)[cnt++] = (void *) ptr;

	    if (_DtCvIsSegWideChar(p_seg))
	      {
		int len = _DtCvStrLen(_DtCvStringOfStringSeg(p_seg), 1)
							* MB_CUR_MAX + 1;

		ptr = malloc (sizeof(char *) * len);
		if (nullptr != ptr)
		    wcstombs((char *) ptr,
				(wchar_t *) _DtCvStringOfStringSeg(p_seg), len);
	      }
	    else
	        ptr = strdup(_DtCvStringOfStringSeg(p_seg));

	    (*ret_chunks)[cnt++] = (void *) ptr;
	    if ((*ret_chunks)[cnt-1] == nullptr)
		return -1;

	    (*ret_chunks)[cnt++] = (void *) DT_HELP_CE_END;
	  }
	else if (_DtCvIsSegRegion(p_seg))
	  {
	    daRegion = (_DtHelpDARegion *) _DtCvInfoOfRegionSeg(p_seg);
	    if (_DtHelpDASpc == daRegion->type)
	      {
		if (False == daRegion->inited)
		  {
		    _DtHelpDASpcInfo *pSpc =
					(_DtHelpDASpcInfo *) daRegion->handle;
		    /*
		     * allocate the spc!
		     */
		    result = (*(resolve_spc))(
				client_data,
				_DtHelpFontHintsLang(pSpc->spc_fonts),
				_DtHelpFontHintsCharSet(pSpc->spc_fonts),
				pSpc->spc_fonts,
				pSpc->name,
				&(_DtCvInfoOfRegionSeg(p_seg)),
				&(_DtCvWidthOfRegionSeg(p_seg)),
				&(_DtCvHeightOfRegionSeg(p_seg)),
				&(_DtCvAscentOfRegionSeg(p_seg)));

		    free(pSpc->name);
		    _DtHelpFreeFontHints(&(pSpc->spc_fonts));

		    free(pSpc);
		    free(daRegion);

		    if (0 != result)
			return -1;

		    daRegion = (_DtHelpDARegion *) _DtCvInfoOfRegionSeg(p_seg);
		  }

	        if (*ret_cnt == 0)
		    *ret_cnt = 1;
	        else
		    cnt--;

	        *ret_cnt = *ret_cnt + 2;
	        if (GrowChunks(ret_chunks, *ret_cnt) != 0)
	          {
		    *ret_cnt = savedCnt;
		    return -1;
		  }

	        type = DT_HELP_CE_SPC;
	        if (p_seg->next_disp == nullptr || _DtCvIsSegNewLine(p_seg))
		    type |= DT_HELP_CE_NEWLINE;

	        (*ret_chunks)[cnt++] = (void *) type;
	        (*ret_chunks)[cnt++] = (void *) daRegion->handle;
	        (*ret_chunks)[cnt++] = (void *) DT_HELP_CE_END;
	      }
	    result = 0;
	  }
	else if (_DtCvIsSegContainer(p_seg) && !(_DtCvIsSegController(p_seg)))
	    result = CreateTitleChunks(toss,
				_DtCvContainerListOfSeg(p_seg), snb,
				stop_mask,
				myLang, mySet,
				resolve_spc,
				client_data,
				ret_cnt, ret_chunks);

	p_seg = _DtCvNextSeg(p_seg);
      }

    return result;
}

/******************************************************************************
 * Function:    int ProcessSegmentsToChunks ()
 *
 * Parameters:
 *
 * Returns:	0 if created a chunk, -1 if errors
 *
 * Purpose:
 *
 ******************************************************************************/
static int
ProcessSegmentsToChunks(
    _DtCvSegment	  *toss,
    _DtCvSegment	  *head_el,
    _DtCvSegment	  *snb_el,
    SDLMask		  *stop_mask,
    const char		  *lang,
    const char		  *char_set,
    _DtHelpResolveSpcProc resolve_spc,
    _DtCvPointer	  client_data,
    void		***ret_chunks)
{
    int cnt    = 0;
    int result = 0;

    result = CreateTitleChunks(toss, _DtCvContainerListOfSeg(head_el),
					snb_el, stop_mask, lang, char_set,
					resolve_spc, client_data,
					&cnt, ret_chunks);
    if ((result != 0 || cnt == 0) && nullptr != AbbrevOfSeg(head_el)
				&& strlen ((char *) AbbrevOfSeg(head_el)))
      {
	*ret_chunks   = (void **) malloc (sizeof(void *) * 4);
	if (*ret_chunks == nullptr)
	    return -1;

	(*ret_chunks)[0] = (void *) DT_HELP_CE_CHARSET;
	(*ret_chunks)[1] = (void *) strdup(char_set);
	(*ret_chunks)[2] = (void *) strdup(AbbrevOfSeg(head_el));
	(*ret_chunks)[3] = (void *) DT_HELP_CE_END;
      }

    return result;
}

/******************************************************************************
 * Function:    _DtCvSegment *GetSdlDocSnb (
 *				_DtHelpVolumeHdl    volume)
 * Parameters:
 *		volume		Specifies the volume.
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:	If the title has been parsed and it used snrefs,
 *		this function will return the snb specified in the
 *		sdldoc element.
 ******************************************************************************/
static _DtCvSegment *
GetSdlDocSnb(
    _DtHelpVolumeHdl	 volume)
{
    _DtCvSegment	*retEl = nullptr;
    CESDLVolume	*sdlVol;

    sdlVol = _DtHelpCeGetSdlVolumePtr(volume);
    if (sdlVol != nullptr && sdlVol->snb != nullptr)
	retEl = (sdlVol->snb);

    return retEl;
}

/******************************************************************************
 * Function:    int GetDocumentTitle (
 *				_DtHelpVolumeHdl    volume,
 *				_DtCvValue	  flag,
 *				_DtCvSegment	**ret_seg)
 * Parameters:
 *		volume		Specifies the volume.
 *		flag		Specifies if the toss is needed.
 *		ret_seg		Returns sdlVol->title.
 *
 * Returns:     0 if successful, -1 if errors, -2 if there is no title.
 *
 * Purpose:	This will fill in the 'title' and 'snb' elements of
 *		the CESDLVolume structure and return 'title' in 'ret_seg'.
 ******************************************************************************/
static int
GetDocumentTitle(
    _DtHelpVolumeHdl	 volume,
    _FrmtUiInfo		*ui_info,
    _SdlFontMode	 mode,
    _DtCvValue		 flag,
    _DtCvSegment	**ret_seg)
{
    int			 result   = -1;
    short		 procFlag = True;
    CESDLVolume		*sdlVol;
    FormatStruct	 frmtStruct;
    _DtCvSegment		*toss = nullptr;
    SDLMask		 skipMask   [SDL_MASK_LEN] = SDLClearAllBits;
    SDLMask		 processMask[SDL_MASK_LEN] = SDLSetAllBits;
    SDLMask		 startExcept[SDL_MASK_LEN] =
					SDLInitMask(SdlElementNone);

    sdlVol = _DtHelpCeGetSdlVolumePtr(volume);
    if (sdlVol == nullptr)
	return -1;

    if (sdlVol->title_processed == False)
      {
	if (flag == True)
	    toss = _DtHelpCeGetSdlVolToss(volume, -1);

	if (SetUp(volume,nullptr,&frmtStruct,toss,ui_info,-1,True,False) !=0 )
	    return -1;

	/*
	 * now set up correct font mode.
	 */
	frmtStruct.resolve_font = mode;

	/*
	 * now get the title.
	 */
	if (_DtHelpCeFileOpenAndSeek(_DtHelpCeGetVolumeName(volume), 0, -1,
					&(frmtStruct.my_file), nullptr) != -1)
          {
	    if (ProcessContent(&frmtStruct, SDLDocumentContent,
				SdlElementNone, startExcept, skipMask) != -1
		&& ProcessSDLMarkup(&frmtStruct, SdlElementVStruct,
				SdlElementNone, startExcept, skipMask) != -1
	        && ProcessContent(&frmtStruct, HeadAndSnb,
				SdlElementNone, startExcept, processMask) != -1)
	      {
		sdlVol->title       = frmtStruct.seg_list;
		sdlVol->snb         = frmtStruct.snb;
		sdlVol->client_data = ui_info->client_data;
		sdlVol->destroy_region = ui_info->destroy_region;
		result = 0;

		/*
		 * if the volume doesn't have a head, set the
		 * appropriate flags.
		 */
		if (nullptr == sdlVol->title)
		  {
		    procFlag = -1;	/* processed with no errors */
		    result   = -2;	/* but no title             */
		  }
	      }
	    else /* free the segments */
		_DtHelpFreeSegments(frmtStruct.seg_list, _DtCvFALSE,
					ui_info->destroy_region,
					ui_info->client_data);

	    _DtHelpCeBufFileClose (frmtStruct.my_file, True);
          }

        /*
         * free the font structures allocated
         */
        DestroyFontInfo(&frmtStruct);

	/*
	 * destroy the link database
	 */
        _DtLinkDbDestroy(frmtStruct.my_links);

        sdlVol->title_processed = procFlag;
      }

    /*
     * if we have a title, good
     */
    else if (sdlVol->title != nullptr)
	result = 0;

    /*
     * we haven't processed a title. Is is because the volume
     * doesn't have one or because of problems accessing the volume?
     */
    else if (-1 == sdlVol->title_processed)
	result = -2;	/* volume doesn't have a title */

    *ret_seg = sdlVol->title;

    return result;
}

/******************************************************************************
 *
 * Semi-Private Functions
 *
 *****************************************************************************/
/******************************************************************************
 * Function:    SDLAttribute *_DtHelpCeGetSdlAttributeList(void)
 *
 * Parameters:
 *
 * Returns:     ptr or NULL;
 *
 * Purpose:
 ******************************************************************************/
const SDLAttribute *
_DtHelpCeGetSdlAttributeList(void)
{
    const SDLAttribute *ptr = SDLAttributeList;

    return ptr;
}

/******************************************************************************
 *
 * Semi-Public Functions
 *
 *****************************************************************************/
/******************************************************************************
 * Function:    int _DtHelpCeFrmtSDLPathAndChildren (
 *				_DtHelpVolumeHdl volume, char *filename,
 *                              int offset, char *id_string,
 *                              _DtCvTopicPtr *ret_handle)
 *
 * Parameters:
 *              volume          Specifies the Help Volume the information
 *                              is associated with.
 *              filename        Specifies the file containing the Help Topic
 *                              desired.
 *              offset          Specifies the offset into 'filename' to
 *                              the Help Topic desired.
 *              id_string       Specifies the location id to look for or NULL.
 *              ret_handle      Returns a handle to the topic information
 *                              including the number of paragraphs and the
 *                              id match segment.
 *
 * Returns:     0 if successful, -1 if errors, 1 if the path is empty.
 *
 ******************************************************************************/
int
_DtHelpCeFrmtSdlPathAndChildren(
    _DtHelpVolumeHdl	 volume,
    _FrmtUiInfo		*ui_info,
    int			 fd,
    char		*target_id,
    _DtCvTopicPtr	*ret_handle)
{
    char		*lang;
    const char		*charSet;
    char		**topicMap;
    int			 result = 0;
    int			 pathCnt;
    FormatStruct	 frmtStruct;
    _DtCvTopicInfo	*topicHandle;
    _DtCvSegment	*mySegList = nullptr;
    _DtCvSegment	*myPrevSeg = nullptr;
    _DtCvSegment	*loids;
    _DtCvSegment	*snb    = nullptr;
    SDLIdInfo		*info;

    *ret_handle = nullptr;

    /*
     * look for the heading for each virpage
     */
    frmtStruct         = DefFormatStruct;
    frmtStruct.ui_info = ui_info;
    frmtStruct.my_links = _DtLinkDbCreate();
    frmtStruct.vol_name = _DtHelpCeGetVolumeName(volume);
    frmtStruct.id_string = target_id;

    /*
     * get the path from the target to the top.
     */
    pathCnt = _DtHelpCeGetSdlIdPath(volume, target_id, &topicMap);
    if (pathCnt == -1)
	return -1;

    /*
     * get the beginning of the path
     */
    loids = _DtHelpCeMapSdlIdToSegment(volume, *topicMap, fd);
    if (loids == nullptr)
      {
        _DtCvFreeArray((void **) topicMap);
        return -1;
      }
    
    /*
     * format the top topic entry.
     */
    info = FrmtPrivInfoPtr(loids)->id_info;
    if (_SdlIdInfoPtrRlevel(info) > 0)
      {
	/*
	 * Put the top topic in the table of contents.
	 */
	lang    = _DtHelpCeGetSdlVolLanguage(volume);
	charSet = _DtHelpCeGetSdlVolCharSet(volume);
	result  = AddEntryToToc(&frmtStruct, volume, fd,
				_SdlIdInfoPtrRlevel(info), lang, charSet,
				_DtHelpCeGetSdlVolToss(volume, fd),
				loids, (topicMap[1] == nullptr ? True : False),
				&snb, &mySegList, &myPrevSeg);
	/*
	 * format the children.
	 */
	if (result != -1 && _DtCvNextSeg(loids) != nullptr)
	  {
	    loids  = _DtCvNextSeg(loids);
	    result = ExpandToc(&frmtStruct, volume, fd, lang, charSet,
				_DtHelpCeGetSdlVolToss(volume, fd),
				_SdlIdInfoPtrRlevel(info) + 1,
				&topicMap[1],
				&loids, &snb, &mySegList, &myPrevSeg);
	  }
      }

    if (result != -1)
      {
	topicHandle = (_DtCvTopicInfo *) malloc (sizeof(_DtCvTopicInfo));
	if (topicHandle != nullptr)
	  {
	    topicHandle->id_str    = nullptr;
	    topicHandle->mark_list = nullptr;
	    topicHandle->link_data = frmtStruct.my_links;

	    topicHandle->seg_list  = (void *) mySegList;

	    /*
	     * let the top level know that there isn't a path
	     */
	    if (nullptr == mySegList)
		result = 1;

	    *ret_handle = (void *) topicHandle;
	  }
	else
	    result = -1;
      }

    _DtCvFreeArray((void **) topicMap);
    return result;

} /* End _DtHelpCeFrmtSDLPathAndChildren */

/******************************************************************************
 * Function:    int _DtHelpCeFrmtSdlVolumeInfo (char *filename,
 *                              _DtCvTopicPtr *ret_handle)
 *
 * Parameters:
 *              filename        Specifies the file containing the Help Topic
 *                              desired.
 *              ret_handle      Returns a handle to the topic information
 *                              including the number of paragraphs and the
 *                              id match segment.
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     _DtHelpCeFrmtSdlVolumeInfo processes the SDL volume looking
 *		for the generated items.
 *
 ******************************************************************************/
int
_DtHelpCeFrmtSdlVolumeInfo(
    char		*filename,
    _DtHelpVolumeHdl	 volume,
    time_t		*ret_time)
{
    int			 result = 0;
    char		*numPtr;
    CESDLVolume		*sdlVol;
    FormatStruct	 frmtStruct;
    SDLMask		 processMask[SDL_MASK_LEN] = SDLSetAllBits;
    SDLMask		 startExcept[SDL_MASK_LEN] =
					SDLInitMask(SdlElementNone);

    if (SetUp(volume,&sdlVol,&frmtStruct,nullptr,&DefUiInfo,-1,True,False) != 0)
	return -1;

    /*
     * now set up correct font mode.
     */
    frmtStruct.resolve_font = _SdlFontModeNone;

    if (result == 0)
      {
	result = _DtHelpCeFileOpenAndSeek(filename, 0, -1,
					&(frmtStruct.my_file), ret_time);
	if (result != -1)
	  {
	    result = ProcessContent(&frmtStruct, SDLDocumentContent,
				SdlElementNone, startExcept, processMask);

	    _DtHelpCeBufFileClose (frmtStruct.my_file, True);

	    if (frmtStruct.remember != nullptr)
		free(frmtStruct.remember);
	  }
    
	if (result != -1)
	  {
	    /*
	     * attach the information to this volume.
	     */
	    sdlVol->sdl_info = (SDLDocInfo *)
			FrmtPrivInfoPtr(frmtStruct.seg_list)->doc_info;

	    /*
	     * check the major and minor numbers.
	     */
	    numPtr = _SdlDocInfoPtrSdlDtd(sdlVol->sdl_info);

	    while (*numPtr < '0' || *numPtr > '9')
	        numPtr++;

	    if (atoi(numPtr) != SDL_DTD_VERSION)
		result = -1;
	    else
	      {
	        while (*numPtr != '.' && *numPtr != '\0')
	            numPtr++;

	        if (*numPtr == '.')
	            numPtr++;

	        sdlVol->minor_no = atoi(numPtr);
	      }

	    /*
	     * free the container
	     */
	    FrmtPrivInfoPtr(frmtStruct.seg_list)->doc_info = nullptr;
	    _DtHelpFreeSegments(frmtStruct.seg_list, _DtCvFALSE, nullptr, nullptr);
	  }
      }

    _DtLinkDbDestroy(frmtStruct.my_links);

    /*
     * free the font structures allocated
     */
    DestroyFontInfo(&frmtStruct);

    return result;
}

/******************************************************************************
 * Function:    int _DtHelpCeFrmtSDLTitleToAscii (
 *				char *filename,
 *                              int offset,
 *                              char **ret_title, char **ret_abbrev)
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 ******************************************************************************/
int
_DtHelpCeFrmtSDLTitleToAscii(
    _DtHelpVolumeHdl        volume,
    int		 offset,
    char	**ret_title,
    char	**ret_abbrev)
{
    int			 result = 0;
    FormatStruct	 frmtStruct;

    if (SetUp(volume,nullptr,&frmtStruct,nullptr,&DefUiInfo,-1,True,False) != 0)
	return -1;

    *ret_title = nullptr;
    if (ret_abbrev != nullptr)
	*ret_abbrev = nullptr;

    /*
     * now set up correct font mode.
     */
    frmtStruct.resolve_font = _SdlFontModeNone;

    /*
     * get the title.
     */
    result = FormatSDLTitle(frmtStruct.vol_name, offset, -1, &frmtStruct);

    if (result != -1 && frmtStruct.seg_list != nullptr)
      {
	_DtCvSegment  *pHeadSeg;
	_DtCvValue   nlFlag = False;

	pHeadSeg = _DtCvContainerListOfSeg(frmtStruct.seg_list);

	if (pHeadSeg != nullptr)
	  {
	    CreateAsciiString(_DtCvContainerListOfSeg(pHeadSeg),
				frmtStruct.snb, &nlFlag, ret_title);
	    CreateAsciiAbbrev(pHeadSeg, ret_abbrev);
	  }
	else
	    result = -1;
      }
    else
	result = -1;

    if (frmtStruct.seg_list != nullptr)
	_DtHelpFreeSegments(frmtStruct.seg_list, _DtCvFALSE, nullptr, nullptr);

    /*
     * free the font structures allocated
     */
    DestroyFontInfo(&frmtStruct);

    return result;

} /* End _DtHelpCeFrmtSDLTitleToAscii */

/******************************************************************************
 * Function:    char *_DtHelpCeFrmtSdlVolumeAbstractToAscii(
 *							_DtHelpVolumeHdl volume)
 *
 * Parameters:
 *              volume          Specifies the Help Volume the information
 *                              is associated with.
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     _DtHelpCeFrmtSdlVolumeAbstractToAscii formats Help Files
 *		with formatting information into a CEVirtualPage
 *
 ******************************************************************************/
char *
_DtHelpCeFrmtSdlVolumeAbstractToAscii(
    _DtHelpVolumeHdl        volume)
{
    int			 offset;
    char		*abstr = nullptr;
    _DtCvSegment	*pSeg;
    _DtCvSegment	*pSnb;
    _DtCvValue		 nlFlag = False;
    _DtHelpCeLockInfo    lockInfo;

    if (_DtHelpCeLockVolume(volume, &lockInfo) != 0)
	return nullptr;

    if (_DtHelpCeFindSdlId(volume,"_abstract",lockInfo.fd,nullptr,&offset) == True)
      {
	int	result = 0;
	SDLMask	startExcept[SDL_MASK_LEN] = SDLInitMask(SdlElementNone);
	SDLMask	processMask[SDL_MASK_LEN] = SDLSetAllBits;
	FormatStruct	 frmtStruct;
	CESDLVolume	*sdlVol;

	if (SetUp(volume, &sdlVol, &frmtStruct, nullptr, &DefUiInfo,
					lockInfo.fd, True, False) != 0)
	    return nullptr;

        /*
         * now set up correct font mode.
         */
        frmtStruct.resolve_font = _SdlFontModeNone;

	/*
	 * open the volume and seek to the virpage
	 */
	result = _DtHelpCeFileOpenAndSeek(_DtHelpCeGetVolumeName(volume),
					offset, lockInfo.fd,
					&(frmtStruct.my_file), nullptr);
	if (result != -1)
	  {
	    result = ProcessSDLMarkup (&frmtStruct, SdlElementVirpage,
				SdlElementNone, startExcept, processMask);
	    _DtHelpCeBufFileClose (frmtStruct.my_file,
					(lockInfo.fd == -1 ? True : False));
	  }

	if (result != -1)
	  {
	    pSeg = frmtStruct.seg_list;
	    pSnb = frmtStruct.snb;

	    CreateAsciiString(pSeg, pSnb, &nlFlag, &abstr);

	    _DtHelpFreeSegments(frmtStruct.seg_list, _DtCvFALSE, nullptr, nullptr);
	    _DtHelpFreeSegments(frmtStruct.snb, _DtCvFALSE, nullptr, nullptr);
	  }

        /*
         * free the font structures allocated
         */
        DestroyFontInfo(&frmtStruct);

      }

    _DtHelpCeUnlockVolume(lockInfo);

    return abstr;

} /* End _DtHelpCeFrmtSdlVolumeAbstractToAscii */

/******************************************************************************
 * Function:    int _DtHelpCeFrmtSDLVolTitleToAscii (
 *				char *filename,
 *                              int offset,
 *                              char **ret_title, char **ret_abbrev)
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 ******************************************************************************/
int
_DtHelpCeFrmtSDLVolTitleToAscii(
    _DtHelpVolumeHdl volume,
    _FrmtUiInfo	*ui_info,
    char	**ret_title)
{
    char		*abbrev  = nullptr;
    int			 result  = 0;
    _DtCvSegment	*pHeadSeg;
    _DtHelpCeLockInfo    lockInfo;

    *ret_title = nullptr;

    /*
     * get the head element
     */
    if (_DtHelpCeLockVolume(volume, &lockInfo) != 0)
	return -1;

    result = GetDocumentTitle(volume,ui_info, _SdlFontModeNone, False, &pHeadSeg);
    _DtHelpCeUnlockVolume(lockInfo);

    if (result == 0)
      {
	/*
	 * get the abbreviation of the head
	 */
	result = -2;
        if (pHeadSeg != nullptr)
          {
	    CreateAsciiAbbrev(pHeadSeg, &abbrev);

	    if (abbrev != nullptr && *abbrev != '\0')
	      {
		*ret_title = abbrev;
		result     = 0;
	      }
          }

	/*
	 * if there wasn't an abbreviation, use the head itself, stripping
	 * all special items and graphics.
	 */
        if (0 != result && pHeadSeg != nullptr &&
				nullptr != _DtCvContainerListOfSeg(pHeadSeg))
	  {
	    _DtCvValue   nlFlag = False;

	    CreateAsciiString(_DtCvContainerListOfSeg(pHeadSeg),
						GetSdlDocSnb(volume),
						&nlFlag, ret_title);
	    if (abbrev != nullptr)
		free(abbrev);

	    result = 0;
	  }
      }

    /*
     * if there isn't an abbreviation on the document, and there isn't
     * a head, then try for the title page. After that, try the
     * hometopic's title.
     */
    if (-2 == result)
      {
	result = 0;
	if (_DtHelpGetTopicTitle(volume, "_title", ret_title) != 0)
	    result = _DtHelpGetTopicTitle(volume, "_hometopic", ret_title);
      }

    return result;

} /* End _DtHelpCeFrmtSDLVolTitleToAscii */

/******************************************************************************
 * Function:    int _DtHelpCeGetSdlTopicTitleChunks (
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 ******************************************************************************/
int
_DtHelpCeGetSdlTitleChunks(
    _DtHelpVolumeHdl	  volume,
    char		  *loc_id,
    _FrmtUiInfo		  *ui_info,
    void		***ret_chunks)
{
    int			 result = 0;
    int			 offset;
    FormatStruct	 frmtStruct;
    CESDLVolume		*sdlVol;
    SDLMask		 stopMask[SDL_MASK_LEN] =
					SDLInitMask(SdlElementSubHead);

    *ret_chunks = nullptr;
    if (_DtHelpCeFindSdlId(volume, loc_id, -1, nullptr, &offset) != True)
	return -1;

    if (SetUp(volume, &sdlVol, &frmtStruct, nullptr, ui_info, -1, True, True) != 0)
	return -1;

    result = FormatSDLTitle(frmtStruct.vol_name, offset, -1, &frmtStruct);

    if (result != -1 && frmtStruct.seg_list != nullptr)
      {
	_DtCvSegment  *headEl;
	_DtCvSegment  *pSnbEl = nullptr;

	result = -1;
	headEl = _DtCvContainerListOfSeg(frmtStruct.seg_list);

	/*
	 * A virpage contains a zero or more heads as it's first
	 * content.  Therefore, if the the first item is not a
	 * container and a controller (heads get the controller flag
	 * put on them), then this virpage does not have a title.
	 */
	if (nullptr != headEl && _DtCvIsSegContainer(headEl) &&
						_DtCvIsSegController(headEl))
	  {
	    result = ProcessSegmentsToChunks(frmtStruct.toss,
					headEl,
					pSnbEl, stopMask,
					_DtHelpCeGetSdlVolLanguage(volume),
					_DtHelpCeGetSdlVolCharSet(volume),
					ui_info->resolve_spc,
					ui_info->client_data,
					ret_chunks);
	  }
      }
    else
	result = -1;

    if (frmtStruct.seg_list != nullptr)
	_DtHelpFreeSegments(frmtStruct.seg_list, _DtCvFALSE,
						ui_info->destroy_region,
						ui_info->client_data);

    if (frmtStruct.snb != nullptr)
	_DtHelpFreeSegments(frmtStruct.snb, _DtCvFALSE,
						ui_info->destroy_region,
						ui_info->client_data);

    /*
     * free the font structures allocated
     */
    DestroyFontInfo(&frmtStruct);

    /*
     * destroy the link database
     */
    _DtLinkDbDestroy(frmtStruct.my_links);

    return result;

} /* End _DtHelpCeGetSdlTitleChunks */

/******************************************************************************
 * Function:    int _DtHelpCeGetSdlVolTitleChunks (
 *
 * Parameters:
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:
 ******************************************************************************/
int
_DtHelpCeGetSdlVolTitleChunks(
    _DtHelpVolumeHdl	 volume_handle,
    _FrmtUiInfo		*ui_info,
    void 		***ret_chunks)
{
    int			 result  = -2;
    _DtCvSegment	*pHeadSeg;
    CESDLVolume		*sdlVol;
    SDLMask		 stopMask[SDL_MASK_LEN] =
					SDLInitMask(SdlElementSubHead);
    /*
     * get the sdl volume pointer.
     */
    sdlVol = _DtHelpCeGetSdlVolumePtr(volume_handle);
    if (sdlVol ==  nullptr)
	return -1;

    /*
     * find the document attributes
     */
    if (nullptr == sdlVol->sdl_info)
	return -1;

    /*
     * get the head element
     */
    if (GetDocumentTitle(volume_handle, ui_info,
				_SdlFontModeResolve, True, &pHeadSeg) == -1)
	return -1;

    /*
     * process it
     */
    if (pHeadSeg != nullptr)
	result = ProcessSegmentsToChunks(
				_DtHelpCeGetSdlVolToss(volume_handle, -1),
				pHeadSeg,
				GetSdlDocSnb(volume_handle), stopMask,
				_DtHelpCeGetSdlVolLanguage(volume_handle),
				_DtHelpCeGetSdlVolCharSet(volume_handle),
				ui_info->resolve_spc,
				ui_info->client_data,
				ret_chunks);
    if (result != 0)
      {
	result = _DtHelpCeGetSdlTitleChunks(volume_handle, "_title",
						ui_info, ret_chunks);
	if (result != 0)
	    result = _DtHelpCeGetSdlTitleChunks(volume_handle, "_hometopic",
						ui_info, ret_chunks);
      }

    return result;

} /* End _DtHelpCeGetSdlVolTitleChunks */

/******************************************************************************
 * Function:    int _DtHelpCeGetSdlVolToss (
 *				_DtHelpVolumeHdl volume,
 * Parameters:
 *		volume		Specifies the volume to read/parse.
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:	Get the toss from a volume.
 ******************************************************************************/
_DtCvSegment *
_DtHelpCeGetSdlVolToss(
    _DtHelpVolumeHdl volume,
    int          fd)
{
    CESDLVolume		*sdlVol;
    FormatStruct	 frmtStruct;
    SDLMask		 skipMask   [SDL_MASK_LEN] = SDLClearAllBits;
    SDLMask		 startExcept[SDL_MASK_LEN] =
					SDLInitMask(SdlElementNone);
    /*
     * get the sdl volume pointer.
     */
    sdlVol = _DtHelpCeGetSdlVolumePtr(volume);
    if (sdlVol == nullptr)
	return nullptr;

    if (sdlVol->toss == nullptr
	&& SetUp(volume,nullptr,&frmtStruct,nullptr,&DefUiInfo,fd,True,False) == 0
	&& _DtHelpCeFileOpenAndSeek(_DtHelpCeGetVolumeName(volume), 0, fd,
					&(frmtStruct.my_file), nullptr) != -1)
      {
	if (ProcessContent(&frmtStruct, SDLDocumentContent,
				SdlElementNone, startExcept, skipMask) != -1
	    && ProcessSDLMarkup(&frmtStruct, SdlElementVStruct,
				SdlElementToss, startExcept, skipMask) != -1)
	    sdlVol->toss = frmtStruct.seg_list;

	_DtHelpCeBufFileClose (frmtStruct.my_file, (fd == -1 ? True : False));

        /*
         * free the font structures allocated
         */
        DestroyFontInfo(&frmtStruct);

	/*
	 * destroy the link database
	 */
        _DtLinkDbDestroy(frmtStruct.my_links);
      }

    if (sdlVol->toss != nullptr)
	return (_DtCvContainerListOfSeg(sdlVol->toss));

    return nullptr;

} /* End _DtHelpCeGetSdlVolToss */

/******************************************************************************
 * Function:    int _DtHelpCeGetSdlVolIndex (
 *				_DtHelpVolumeHdl volume,
 * Parameters:
 *		volume		Specifies the volume to read/parse.
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:	Get the index from a volume.
 ******************************************************************************/
int
_DtHelpCeGetSdlVolIndex(
    _DtHelpVolumeHdl volume)
{
    int			 result = -1;
    CESDLVolume		*sdlVol;
    FormatStruct	 frmtStruct;
    SDLMask		 skipMask   [SDL_MASK_LEN] = SDLClearAllBits;
    SDLMask		 startExcept[SDL_MASK_LEN] =
					SDLInitMask(SdlElementNone);

    sdlVol = _DtHelpCeGetSdlVolumePtr(volume);
    if (sdlVol == nullptr)
	return -1;

    if (sdlVol->index != nullptr)
	return 0;

    if (SetUp(volume, nullptr, &frmtStruct, nullptr, &DefUiInfo, -1, True, True) != 0)
	return -1;

    /*
     * now set up correct font mode.
     */
    frmtStruct.resolve_font = _SdlFontModeNone;

    if (_DtHelpCeFileOpenAndSeek(_DtHelpCeGetVolumeName(volume), 0, -1,
					&(frmtStruct.my_file), nullptr) != -1)
      {
	if (ProcessContent(&frmtStruct, SDLDocumentContent,
				SdlElementNone, startExcept, skipMask) != -1
	    && ProcessSDLMarkup(&frmtStruct, SdlElementVStruct,
				SdlElementIndex, startExcept, skipMask) != -1)
	  {
	    sdlVol->index = frmtStruct.seg_list;
	    result = 0;
	  }

	_DtHelpCeBufFileClose (frmtStruct.my_file, True);
      }

    /*
     * destroy the link database
     */
    _DtLinkDbDestroy(frmtStruct.my_links);

    /*
     * free the font structures allocated
     */
    DestroyFontInfo(&frmtStruct);

    return result;

} /* End _DtHelpCeGetSdlVolIndex */

/******************************************************************************
 * Function:    int _DtHelpCeGetSdlVolIds (
 *				_DtHelpVolumeHdl volume,
 *				_DtCvSegment **ret_ids
 * Parameters:
 *		volume		Specifies the volume to read/parse.
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:	Get the loids from a volume.
 ******************************************************************************/
int
_DtHelpCeGetSdlVolIds(
    _DtHelpVolumeHdl	  volume,
    int			  fd,
    _DtCvSegment		**ret_ids)
{
    int			 result = 0;
    CESDLVolume		*sdlVol;
    FormatStruct	 frmtStruct;
    SDLMask		 skipMask   [SDL_MASK_LEN] = SDLClearAllBits;
    SDLMask		 startExcept[SDL_MASK_LEN] =
					SDLInitMask(SdlElementNone);

    *ret_ids = nullptr;
    sdlVol   = _DtHelpCeGetSdlVolumePtr(volume);
    if (sdlVol == nullptr)
	return -1;

    if (sdlVol->loids == nullptr)
      {
	result = -1;
        if (SetUp(nullptr,nullptr,&frmtStruct,nullptr,&DefUiInfo,-1,False,False) != -1
	    && _DtHelpCeFileOpenAndSeek(_DtHelpCeGetVolumeName(volume), 0, fd,
					&(frmtStruct.my_file), nullptr) != -1)
          {
	    if (ProcessContent(&frmtStruct, SDLDocumentContent,
				SdlElementNone, startExcept, skipMask) != -1)
	      {
	        if (ProcessSDLMarkup(&frmtStruct, SdlElementVStruct,
				SdlElementLoids, startExcept, skipMask) != -1)
	          {
		    sdlVol->loids = frmtStruct.seg_list;
		    result = 0;
	          }
	      }

	    /*
	     * free the font structures allocated
	     */
	    DestroyFontInfo(&frmtStruct);

	    /*
	     * destroy the link database
	     */
            _DtLinkDbDestroy(frmtStruct.my_links);

	    _DtHelpCeBufFileClose(frmtStruct.my_file,(fd == -1 ? True : False));
          }
      }

    if (sdlVol->loids != nullptr)
        *ret_ids = _DtCvContainerListOfSeg(sdlVol->loids);

    return result;

} /* End _DtHelpCeGetSdlVolIds */

/******************************************************************************
 * Function:    int _DtHelpCeParseSdlTopic (_DtHelpVolumeHdl volume,
 *                              int offset, char *id_string,
 *                              _DtCvTopicPtr *ret_handle)
 *
 * Parameters:
 *              volume          Specifies the Help Volume the information
 *                              is associated with.
 *              offset          Specifies the offset into 'filename' to
 *                              the Help Topic desired.
 *              id_string       Specifies the location id to look for or NULL.
 *              ret_handle      Returns a handle to the topic information
 *                              including the number of paragraphs and the
 *                              id match segment.
 *
 * Returns:     0 if successful, -1 if errors
 *
 * Purpose:     _DtHelpCeParseSdlTopic formats Help Files with formatting
 *              information into a CEVirtualPage
 *
 ******************************************************************************/
int
_DtHelpCeParseSdlTopic(
    _DtHelpVolumeHdl	 volume,
    _FrmtUiInfo		*ui_info,
    int			 fd,
    int			 offset,
    char		*id_string,
    int			 rich_text,
    _DtCvTopicPtr	*ret_handle)
{
    int			 result = 0;
    SDLMask		 startExcept[SDL_MASK_LEN] =
					SDLInitMask(SdlElementNone);
    SDLMask		 processMask[SDL_MASK_LEN]  = SDLSetAllBits;
    FormatStruct	 frmtStruct;
    _DtCvTopicInfo	*topicHandle;
    CESDLVolume		*sdlVol;

    *ret_handle = nullptr;

    if (SetUp(volume,&sdlVol,&frmtStruct,nullptr,ui_info,fd,True,rich_text) != 0)
	return -1;

    /*
     * remember the id for graphics
     */
    frmtStruct.id_string = id_string;

    result = _DtHelpCeFileOpenAndSeek(_DtHelpCeGetVolumeName(volume),
					offset, fd,
					&(frmtStruct.my_file), nullptr);
    if (result != -1)
      {
	result = ProcessSDLMarkup (&frmtStruct, SdlElementVirpage,
				SdlElementNone, startExcept, processMask);
	_DtHelpCeBufFileClose (frmtStruct.my_file, (fd == -1 ? True : False));
      }

    /*
     * free the async blocks
     */
    _DtHelpFreeSegments(frmtStruct.async_blks, _DtCvFALSE,
						ui_info->destroy_region,
						ui_info->client_data);

    /*
     * free the system notation blocks
     */
    _DtHelpFreeSegments(frmtStruct.snb, _DtCvFALSE,
						ui_info->destroy_region,
						ui_info->client_data);

    if (result != -1)
      {
	topicHandle = (_DtCvTopicInfo *) malloc (sizeof(_DtCvTopicInfo));
	if (topicHandle != nullptr)
	  {
	    topicHandle->mark_list = nullptr;
	    topicHandle->id_str    = nullptr;
	    if (id_string != nullptr)
		topicHandle->id_str = strdup(id_string);

	    topicHandle->link_data = frmtStruct.my_links;
	    topicHandle->seg_list  = frmtStruct.seg_list;
	    *ret_handle = (void *) topicHandle;
	  }
	else
	    result = -1;
      }

    /*
     * free the allocated font structures
     */
    DestroyFontInfo(&frmtStruct);

    return result;

} /* End _DtHelpCeParseSdlTopic */
