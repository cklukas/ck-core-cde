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
/*	Copyright (c) 1995 FUJITSU LIMITED	*/
/*	All Rights Reserved			*/

/*
 * $XConsortium: FolioObject.hh /main/5 1996/09/10 17:26:30 barstow $
 *
 * Copyright (c) 1991 HaL Computer Systems, Inc.  All rights reserved.
 * UNPUBLISHED -- rights reserved under the Copyright Laws of the United
 * States.  Use of a copyright notice is precautionary only and does not
 * imply publication or disclosure.
 * 
 * This software contains confidential information and trade secrets of HaL
 * Computer Systems, Inc.  Use, disclosure, or reproduction is prohibited
 * without the prior express written permission of HaL Computer Systems, Inc.
 * 
 *                         RESTRICTED RIGHTS LEGEND
 * Use, duplication, or disclosure by the Government is subject to
 * restrictions as set forth in subparagraph (c)(l)(ii) of the Rights in
 * Technical Data and Computer Software clause at DFARS 252.227-7013.
 *                        HaL Computer Systems, Inc.
 *                  1315 Dell Avenue, Campbell, CA  95008
 * 
 */

// Base Object for ALL Folio Objects

class Dependent;
class DependOnList;
class FolioObject;

typedef void (FolioObject::* notify_handler_t)
  (FolioObject *object, u_int notify_type,
   void *call_data, void *client_data);

#if defined(__STDC__)
#define DEF_CLASS(C) \
  virtual ClassType type() const; \
  static ClassType C##Class
#define INIT_CLASS(C) \
  ClassType C::type() const { return (C##Class); } \
  ClassType C::C##Class = (size_t) &C::C##Class
#else
#define DEF_CLASS(C) \
  virtual ClassType type() const; \
  static ClassType C/**/Class
#define INIT_CLASS(C) \
  ClassType C::type() const { return (C/**/Class); } \
  ClassType C::C/**/Class = (ClassType) &C::C/**/Class
#endif

  typedef int ClassType;

class FolioObject
{
public: // enums
#define FolioObjectLast 1
  enum notify_types { DESTROYED = 0, _LAST = FolioObjectLast };
  DEF_CLASS (FolioObject);

public: // functions
  FolioObject();
  virtual ~FolioObject();

  virtual const char *display_as();

  template <typename T, void (T::*Fn)()>
  void _folio_thunk0 (FolioObject *, u_int, void *, void *)
    { (reinterpret_cast<T *>(this)->*Fn)(); }
  template <typename T, typename P1, void (T::*Fn)(P1 *, u_int)>
  void _folio_thunk2 (FolioObject *obj, u_int ntype, void *, void *)
    { (reinterpret_cast<T *>(this)->*Fn)((P1 *)obj, ntype); }
  template <typename T, typename P1, typename P3, void (T::*Fn)(P1 *, u_int, P3 *)>
  void _folio_thunk3 (FolioObject *obj, u_int ntype, void *ndata, void *)
    { (reinterpret_cast<T *>(this)->*Fn)((P1 *)obj, ntype, (P3 *)ndata); }
  template <typename T, typename P1, typename P3, typename P4, void (T::*Fn)(P1 *, u_int, P3 *, P4 *)>
  void _folio_thunk4 (FolioObject *obj, u_int ntype, void *ndata, void *cdata)
    { (reinterpret_cast<T *>(this)->*Fn)((P1 *)obj, ntype, (P3 *)ndata, (P4 *)cdata); }

#define AddDependent(CB, TYPE) \
  add_dependent (this, (notify_handler_t)CB, TYPE)
#define AddDependentd(CB, TYPE, DATA) \
  add_dependent (this, (notify_handler_t)CB, TYPE, DATA)

#if defined(SC3) || defined(__SunOS)
  void add_dependent (FolioObject *dependent, notify_handler_t handler,
		      u_int notify_type, void *dependent_data = NULL);
#else
  void add_dependent (void *dependent, notify_handler_t handler,
		      u_int notify_type, void *dependent_data = NULL);
#endif

#define RemoveDependent(CB, TYPE) \
  remove_dependent(this, (notify_handler_t)CB, TYPE)
#define RemoveDependentd(CB, TYPE, DATA) \
  remove_dependent(this, (notify_handler_t)CB, TYPE, DATA)

#if defined(SC3) || defined(__SunOS)
  void remove_dependent (FolioObject *dependent, notify_handler_t handler,
			 u_int notify_type, void *dependent_data = NULL);
#else
  void remove_dependent (void *dependent, notify_handler_t handler,
			 u_int notify_type, void *dependent_data = NULL);
#endif
  void remove_depend_on (FolioObject *target, Dependent *d);
  void release_dependents();
  void release_depend_on_list();

#define Observe(TARG, TYPE, CB) \
  observe (this, TARG, FOLIO_HANDLER_OF(CB), TYPE)
#define Observed(TARG, TYPE, CB, DATA) \
  observe (this, TARG, FOLIO_HANDLER_OF(CB), TYPE, DATA)

#if defined(SC3) || defined(__SunOS)
  void observe (FolioObject *real_this, FolioObject *target,
		notify_handler_t callback, u_int notify_type,
		void *client_data = NULL);
#else
  void observe (void *real_this, FolioObject *target,
		notify_handler_t callback, u_int notify_type,
		void *client_data = NULL);
#endif
  
  bool initialized()
    { return (f_initialized); }
  ErrorCode status()
    { return (f_status); }

protected: // functions
  // Object states that it has changed - dependents notified.
  // notify_type should be an enum provided by the object.  Dependents
  // should know about various object change types that it cares about.
  // Dependents should ignore change types that it doesn't care about. 
  void notify (u_int notify_type, void *notify_data = NULL);

  void self_destruct();
  void setInitialized()
    { f_initialized = TRUE; }
  void unsetInitialized()
    { f_initialized = FALSE; }
  void setStatus (ErrorCode status)
    { f_status = status; }

protected: // variables
  ErrorCode	 f_status;
  bool	 f_initialized;
  Dependent     *f_dependents;
  DependOnList  *f_depend_on_list;
};

template <typename FnType, FnType Fn>
struct _Folio_Handler_Select;

template <typename T, void (T::*Fn)()>
struct _Folio_Handler_Select<void (T::*)(), Fn> {
  static notify_handler_t get() { return &FolioObject::_folio_thunk0<T, Fn>; }
};
template <typename T, typename P1, void (T::*Fn)(P1 *, u_int)>
struct _Folio_Handler_Select<void (T::*)(P1 *, u_int), Fn> {
  static notify_handler_t get() { return &FolioObject::_folio_thunk2<T, P1, Fn>; }
};
template <typename T, typename P1, typename P3, void (T::*Fn)(P1 *, u_int, P3 *)>
struct _Folio_Handler_Select<void (T::*)(P1 *, u_int, P3 *), Fn> {
  static notify_handler_t get() { return &FolioObject::_folio_thunk3<T, P1, P3, Fn>; }
};
template <typename T, typename P1, typename P3, typename P4, void (T::*Fn)(P1 *, u_int, P3 *, P4 *)>
struct _Folio_Handler_Select<void (T::*)(P1 *, u_int, P3 *, P4 *), Fn> {
  static notify_handler_t get() { return &FolioObject::_folio_thunk4<T, P1, P3, P4, Fn>; }
};

#define FOLIO_HANDLER_OF(CB) (_Folio_Handler_Select<decltype(CB), (CB)>::get())
