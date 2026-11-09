/*
 * CDE - Common Desktop Environment
 *
 * These libraries and programs are free software; you can redistribute them
 * and/or modify them under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * These libraries and programs are distributed without any warranty; without
 * even the implied warranty of merchantability or fitness for a particular
 * purpose. See the GNU Lesser General Public License for more details.
 */

#ifndef DTFILE_FILE_CONFLICT_DIALOG_H
#define DTFILE_FILE_CONFLICT_DIALOG_H

#include <Xm/Xm.h>
#include "FileConflict.h"

typedef struct DtfileConflictDialog DtfileConflictDialog;

/* Consumes the batch and reply descriptor on every path. A returned handle is
 * owned by the operation until Finish, independently of the widget lifetime.
 * Closing the view sends cancellation; it never erases or renames a file.
 */
DtfileConflictDialog *DtfileConflictDialogCreate(Widget parent,
                                               DtfileConflictBatch *batch,
                                               int reply_fd);
int DtfileConflictDialogPending(const DtfileConflictDialog *dialog);
void DtfileConflictDialogFinish(DtfileConflictDialog *dialog);

#endif
