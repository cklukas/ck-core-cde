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

#ifndef DTFILE_FILE_CONFLICT_WIRE_H
#define DTFILE_FILE_CONFLICT_WIRE_H

#include "FileConflict.h"
#include <sys/types.h>

typedef struct DtfileConflictReader DtfileConflictReader;

/* Request decoding is incremental. ReadAvailable never waits for future pipe
 * data and consumes at most byte_budget bytes per event-loop dispatch. A pipe
 * has one reader; no other callback may consume it until Take/Free.
 * Return -1 on malformed/I/O input, 0 for more input, 1 for a complete request.
 * The reader borrows fd. Take transfers a complete batch to an empty result.
 */
DtfileConflictReader *DtfileConflictReaderNew(void);
int DtfileConflictReadAvailable(DtfileConflictReader *reader, int fd,
                                size_t byte_budget);
int DtfileConflictReaderTake(DtfileConflictReader *reader,
                             DtfileConflictBatch *batch);
void DtfileConflictReaderFree(DtfileConflictReader *reader);

int DtfileConflictWriteRequest(int fd, const DtfileConflictBatch *batch);
/* Write the legacy host-order dispatch tag and complete request under one
 * thread-local SIGPIPE scope. The receiver consumes tag before using Reader.
 */
int DtfileConflictWriteTaggedRequest(int fd, unsigned short tag,
                                     const DtfileConflictBatch *batch);
int DtfileConflictWriteDecisions(int fd, const DtfileConflictBatch *batch,
                                 const DtfileConflictDecision *decisions);
int DtfileConflictWriteCancel(int fd);

/* Worker-only blocking receive. Validate the complete indexed response and its
 * trailer before exposing any decisions. Waiting for the user's first byte has
 * no time limit. Once a packet starts, a stalled partial response fails; death
 * of the original UI parent also aborts, even if an unrelated helper inherited
 * a write descriptor. Return 0 for Cancel, 1 for decisions, -1 on failure.
 * All write functions borrow fd and return 0/-1; the UI closes its owned reply
 * endpoint after sending a decision or cancellation. No process-wide SIGPIPE
 * disposition is changed.
 */
int DtfileConflictReadDecisions(int fd, pid_t owner,
                                const DtfileConflictBatch *batch,
                                DtfileConflictDecision **decisions);

#endif
