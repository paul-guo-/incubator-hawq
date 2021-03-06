/*-------------------------------------------------------------------------
 *
 * tqual.c
 *	  POSTGRES "time qualification" code, ie, tuple visibility rules.
 *
 * NOTE: all the HeapTupleSatisfies routines will update the tuple's
 * "hint" status bits if we see that the inserting or deleting transaction
 * has now committed or aborted (and it is safe to set the hint bits).
 * If the hint bits are changed, SetBufferCommitInfoNeedsSave is called on
 * the passed-in buffer.  The caller must hold not only a pin, but at least
 * shared buffer content lock on the buffer containing the tuple.
 *
 * NOTE: must check TransactionIdIsInProgress (which looks in PGPROC array)
 * before TransactionIdDidCommit/TransactionIdDidAbort (which look in
 * pg_clog).  Otherwise we have a race condition: we might decide that a
 * just-committed transaction crashed, because none of the tests succeed.
 * xact.c is careful to record commit/abort in pg_clog before it unsets
 * MyProc->xid in PGPROC array.  That fixes that problem, but it also
 * means there is a window where TransactionIdIsInProgress and
 * TransactionIdDidCommit will both return true.  If we check only
 * TransactionIdDidCommit, we could consider a tuple committed when a
 * later GetSnapshotData call will still think the originating transaction
 * is in progress, which leads to application-level inconsistency.	The
 * upshot is that we gotta check TransactionIdIsInProgress first in all
 * code paths, except for a few cases where we are looking at
 * subtransactions of our own main transaction and so there can't be any
 * race condition.
 *
 * Summary of visibility functions:
 *
 *   HeapTupleSatisfiesMVCC()
 *        visible to supplied snapshot, excludes current command
 *   HeapTupleSatisfiesNow()
 *        visible to instant snapshot, excludes current command
 *   HeapTupleSatisfiesUpdate()
 *        like HeapTupleSatisfiesNow(), but with user-supplied command
 *        counter and more complex result
 *   HeapTupleSatisfiesSelf()
 *        visible to instant snapshot and current command
 *   HeapTupleSatisfiesDirty()
 *        like HeapTupleSatisfiesSelf(), but includes open transactions
 *   HeapTupleSatisfiesVacuum()
 *        visible to any running transaction, used by VACUUM
 *   HeapTupleSatisfiesToast()
 *        visible unless part of interrupted vacuum, used for TOAST
 *   HeapTupleSatisfiesAny()
 *        all tuples are visible
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/time/tqual.c,v 1.100 2006/11/17 18:00:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/multixact.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"

#include "utils/tqual.h"

#include "access/twophase.h"  /*max_prepared_xacts*/
#include "miscadmin.h"
#include "lib/stringinfo.h"

#include "utils/guc.h"
#include "utils/memutils.h"

#include "storage/procarray.h"

#include "catalog/pg_type.h"
#include "funcapi.h"

#include "utils/builtins.h"

#include "cdb/cdbvars.h"

#include "access/clog.h"

#include "storage/buffile.h"
#include "postmaster/primary_mirror_mode.h"

/*
 * These SnapshotData structs are static to simplify memory allocation
 * (see the hack in GetSnapshotData to avoid repeated malloc/free).
 */
static SnapshotData SnapshotDirtyData;
static SnapshotData SerializableSnapshotData;
static SnapshotData LatestSnapshotData;

/* Externally visible pointers to valid snapshots: */
Snapshot	SnapshotDirty = &SnapshotDirtyData;
Snapshot	SerializableSnapshot = NULL;
Snapshot	LatestSnapshot = NULL;

/*
 * This pointer is not maintained by this module, but it's convenient
 * to declare it here anyway.  Callers typically assign a copy of
 * GetTransactionSnapshot's result to ActiveSnapshot.
 */
Snapshot	ActiveSnapshot = NULL;

/* These are updated by GetSnapshotData: */
TransactionId TransactionXmin = InvalidTransactionId;
TransactionId RecentXmin = InvalidTransactionId;
TransactionId RecentGlobalXmin = InvalidTransactionId;

#ifdef WATCH_VISIBILITY_IN_ACTION

uint8 WatchVisibilityFlags[WATCH_VISIBILITY_BYTE_LEN];

#endif

/* local functions */
static bool
XidInSnapshot(TransactionId xid, Snapshot snapshot, bool isXmax,
			  bool distributedSnapshotIgnore, bool *setDistributedSnapshotIgnore);
static bool
XidInSnapshot_Local(TransactionId xid, Snapshot snapshot, bool isXmax);

/* MPP Shared Snapshot. */
typedef struct SharedSnapshotStruct
{
	int 		numSlots;		/* number of valid Snapshot entries */
	int			maxSlots;		/* allocated size of sharedSnapshotArray */
	int 		nextSlot;		/* points to the next avail slot. */

	/*
	 * We now allow direct indexing into this array.
	 *
	 * We allocate the XIPS below.
	 *
	 * Be very careful when accessing fields inside here.
	 */
	SharedSnapshotSlot	   *slots;

	TransactionId	   *xips;		/* VARIABLE LENGTH ARRAY */
} SharedSnapshotStruct;

static volatile SharedSnapshotStruct *sharedSnapshotArray;

static Size slotSize = 0;
static Size slotCount = 0;
static Size xipEntryCount = 0;

/*
 * Report shared-memory space needed by CreateSharedSnapshot.
 */
Size
SharedSnapshotShmemSize(void)
{
	Size		size;

	xipEntryCount = MaxBackends + max_prepared_xacts;

	slotSize = sizeof(SharedSnapshotSlot);
	slotSize += mul_size(sizeof(TransactionId), (xipEntryCount));
	slotSize = MAXALIGN(slotSize);

	/*
	 * We only really need max_prepared_xacts; but for safety we
	 * multiply that by two (to account for slow de-allocation on
	 * cleanup, for instance).
	 */
	slotCount = 2 * max_prepared_xacts;

	size = offsetof(SharedSnapshotStruct, xips);
	size = add_size(size, mul_size(slotSize, slotCount));

	return MAXALIGN(size);
}

/*
 * Initialize the sharedSnapshot array.  This array is used to communicate
 * snapshots between qExecs that are segmates.
 */
void
CreateSharedSnapshotArray(void)
{
	bool	found;
	int		i;
	TransactionId *xip_base=NULL;

	/* Create or attach to the SharedSnapshot shared structure */
	sharedSnapshotArray = (SharedSnapshotStruct *)
		ShmemInitStruct("Shared Snapshot", SharedSnapshotShmemSize(), &found);

	Assert(slotCount != 0);
	Assert(xipEntryCount != 0);

	if (!found)
	{
		/*
		 * We're the first - initialize.
		 */
		sharedSnapshotArray->numSlots = 0;

		/* TODO:  MaxBackends is only somewhat right.  What we really want here
		 *        is the MaxBackends value from the QD.  But this is at least
		 *		  safe since we know we dont need *MORE* than MaxBackends.  But
		 *        in general MaxBackends on a QE is going to be bigger than on a
		 *		  QE by a good bit.  or at least it should be.
		 *
		 * But really, max_prepared_transactions *is* what we want (it
		 * corresponds to the number of connections allowed on the
		 * master).
		 *
		 * slotCount is initialized in SharedSnapshotShmemSize().
		 */
		sharedSnapshotArray->maxSlots = slotCount;
		sharedSnapshotArray->nextSlot = 0;

		sharedSnapshotArray->slots = (SharedSnapshotSlot *)&sharedSnapshotArray->xips;

		/* xips start just after the last slot structure */
		xip_base = (TransactionId *)&sharedSnapshotArray->slots[sharedSnapshotArray->maxSlots];

		for (i=0; i < sharedSnapshotArray->maxSlots; i++)
		{
			SharedSnapshotSlot *tmpSlot = &sharedSnapshotArray->slots[i];

			tmpSlot->slotid = -1;
			tmpSlot->contentid = UNINITIALIZED_GP_IDENTITY_VALUE;
			tmpSlot->slotindex = i;

			/*
			 * Fixup xip array pointer reference space allocated after slot structs:
			 *
			 * Note: xipEntryCount is initialized in SharedSnapshotShmemSize().
			 * So each slot gets (MaxBackends + max_prepared_xacts) transaction-ids.
			 */
			tmpSlot->snapshot.xip = &xip_base[xipEntryCount];
			xip_base += xipEntryCount;
		}
	}
}

/*
 * Used to dump the internal state of the shared slots for debugging.
 */
char *
SharedSnapshotDump(void)
{
	StringInfoData str;
	volatile SharedSnapshotStruct *arrayP = sharedSnapshotArray;
	int index;

    initStringInfo(&str);

	appendStringInfo(&str, "Local SharedSnapshot Slot Dump: currSlots: %d maxSlots: %d ",
					 arrayP->numSlots, arrayP->maxSlots);

	LWLockAcquire(SharedSnapshotLock, LW_EXCLUSIVE);

	for (index=0; index < arrayP->maxSlots; index++)
	{
		/* need to do byte addressing to find the right slot */
		SharedSnapshotSlot *testSlot = &arrayP->slots[index];

		if (testSlot->slotid != -1)
		{
			appendStringInfo(&str, "(SLOT index: %d slotid: %d contentid: %d QDxid: %u QDcid: %u pid: %u)",
							 testSlot->slotindex, testSlot->slotid, testSlot->contentid, testSlot->QDxid, testSlot->QDcid, (int)testSlot->pid);
		}

	}

	LWLockRelease(SharedSnapshotLock);

	return str.data;
}

void
GetSlotTableDebugInfo(void **snapshotArray, int *maxSlots)
{
	*snapshotArray = (void *)sharedSnapshotArray;
	*maxSlots = sharedSnapshotArray->maxSlots;
}

/*
 * Set the buffer dirty after setting t_infomask
 */
static inline
void markDirty(Buffer buffer, bool disabled, Relation relation, HeapTupleHeader tuple, bool isXmin)
{
	TransactionId xid;

	if (!disabled)
	{
		WATCH_VISIBILITY_ADDPAIR(
			WATCH_VISIBILITY_GUC_OFF_MARK_BUFFFER_DIRTY_FOR_XMIN, !isXmin);

		SetBufferCommitInfoNeedsSave(buffer);
		return;
	}

	/*
	 * The GUC gp_disable_tuple_hints is on.  Do further evaluation whether we want to write out the
	 * buffer or not.
	 */
	if (relation == NULL)
	{
		WATCH_VISIBILITY_ADDPAIR(
			WATCH_VISIBILITY_NO_REL_MARK_BUFFFER_DIRTY_FOR_XMIN, !isXmin);

		SetBufferCommitInfoNeedsSave(buffer);
		return;
	}

	if (relation->rd_issyscat)
	{
		/* Assume we want to always mark the buffer dirty */

		WATCH_VISIBILITY_ADDPAIR(
			WATCH_VISIBILITY_SYS_CAT_MARK_BUFFFER_DIRTY_FOR_XMIN, !isXmin);

		SetBufferCommitInfoNeedsSave(buffer);
		return;
	}

	/*
	 * Get the xid whose hint bits were just set.
	 */
	if (isXmin)
		xid = HeapTupleHeaderGetXmin(tuple);
	else
		xid = HeapTupleHeaderGetXmax(tuple);

	if (xid == InvalidTransactionId)
	{
		WATCH_VISIBILITY_ADDPAIR(
			WATCH_VISIBILITY_NO_XID_MARK_BUFFFER_DIRTY_FOR_XMIN, !isXmin);

		SetBufferCommitInfoNeedsSave(buffer);
		return;
	}

	/*
	 * Check age of the affected xid.  If it is too old, mark the buffer to be written.
	 */
	if (CLOGTransactionIsOld(xid))
	{
		WATCH_VISIBILITY_ADDPAIR(
			WATCH_VISIBILITY_TOO_OLD_MARK_BUFFFER_DIRTY_FOR_XMIN, !isXmin);

		SetBufferCommitInfoNeedsSave(buffer);
		return;
	}
	else
	{
		WATCH_VISIBILITY_ADDPAIR(
			WATCH_VISIBILITY_YOUNG_DO_NOT_MARK_BUFFFER_DIRTY_FOR_XMIN, !isXmin);
	}
}

#ifdef WATCH_VISIBILITY_IN_ACTION
static char WatchVisibilityXminCurrent[2000];
static char WatchVisibilityXmaxCurrent[2000];
#endif

/*
 * HeapTupleSatisfiesItself
 *		True iff heap tuple is valid "for itself".
 *
 *	Here, we consider the effects of:
 *		all committed transactions (as of the current instant)
 *		previous commands of this transaction
 *		changes made by the current command
 *
 * Note:
 *		Assumes heap tuple is valid.
 *
 * The satisfaction of "itself" requires the following:
 *
 * ((Xmin == my-transaction &&				the row was updated by the current transaction, and
 *		(Xmax is null						it was not deleted
 *		 [|| Xmax != my-transaction)])			[or it was deleted by another transaction]
 * ||
 *
 * (Xmin is committed &&					the row was modified by a committed transaction, and
 *		(Xmax is null ||					the row has not been deleted, or
 *			(Xmax != my-transaction &&			the row was deleted by another transaction
 *			 Xmax is not committed)))			that has not been committed
 */
bool
HeapTupleSatisfiesItself(Relation relation, HeapTupleHeader tuple, Buffer buffer)
{
	const bool disableTupleHints = gp_disable_tuple_hints;

	WATCH_VISIBILITY_CLEAR();

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)
			return false;

		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
					return false;
				}
				tuple->t_infomask |= HEAP_XMIN_COMMITTED;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
			}
		}
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return false;
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_COMMITTED;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
				}
				else
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
					return false;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple)))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return true;

			if (tuple->t_infomask & HEAP_IS_LOCKED)		/* not deleter */
				return true;

			Assert(!(tuple->t_infomask & HEAP_XMAX_IS_MULTI));

			/* deleting subtransaction aborted? */
			if (TransactionIdDidAbort(HeapTupleHeaderGetXmax(tuple)))
			{
				tuple->t_infomask |= HEAP_XMAX_INVALID;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
				return true;
			}

			Assert(TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(tuple)));

			return false;
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetXmin(tuple)))
			return false;
		else if (TransactionIdDidCommit(HeapTupleHeaderGetXmin(tuple)))
		{
			tuple->t_infomask |= HEAP_XMIN_COMMITTED;
			markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
		}
		else
		{
			/* it must have aborted or crashed */
			tuple->t_infomask |= HEAP_XMIN_INVALID;
			markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
			return false;
		}
	}

	/* by here, the inserting transaction has committed */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return true;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (tuple->t_infomask & HEAP_IS_LOCKED)
			return true;
		return false;			/* updated by other */
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/* MultiXacts are currently only allowed to lock tuples */
		Assert(tuple->t_infomask & HEAP_IS_LOCKED);
		return true;
	}

	if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(tuple)))
	{
		if (tuple->t_infomask & HEAP_IS_LOCKED)
			return true;
		return false;
	}

	if (TransactionIdIsInProgress(HeapTupleHeaderGetXmax(tuple)))
		return true;

	if (!TransactionIdDidCommit(HeapTupleHeaderGetXmax(tuple)))
	{
		/* it must have aborted or crashed */
		tuple->t_infomask |= HEAP_XMAX_INVALID;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
		return true;
	}

	/* xmax transaction committed */

	if (tuple->t_infomask & HEAP_IS_LOCKED)
	{
		tuple->t_infomask |= HEAP_XMAX_INVALID;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
		return true;
	}

	tuple->t_infomask |= HEAP_XMAX_COMMITTED;
	markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
	return false;
}

/*
 * HeapTupleSatisfiesNow
 *		True iff heap tuple is valid "now".
 *
 *	Here, we consider the effects of:
 *		all committed transactions (as of the current instant)
 *		previous commands of this transaction
 *
 * Note we do _not_ include changes made by the current command.  This
 * solves the "Halloween problem" wherein an UPDATE might try to re-update
 * its own output tuples.
 *
 * Note:
 *		Assumes heap tuple is valid.
 *
 * The satisfaction of "now" requires the following:
 *
 * ((Xmin == my-transaction &&				inserted by the current transaction
 *	 Cmin < my-command &&					before this command, and
 *	 (Xmax is null ||						the row has not been deleted, or
 *	  (Xmax == my-transaction &&			it was deleted by the current transaction
 *	   Cmax >= my-command)))				but not before this command,
 * ||										or
 *	(Xmin is committed &&					the row was inserted by a committed transaction, and
 *		(Xmax is null ||					the row has not been deleted, or
 *		 (Xmax == my-transaction &&			the row is being deleted by this transaction
 *		  Cmax >= my-command) ||			but it's not deleted "yet", or
 *		 (Xmax != my-transaction &&			the row was deleted by another transaction
 *		  Xmax is not committed))))			that has not been committed
 *
 *		mao says 17 march 1993:  the tests in this routine are correct;
 *		if you think they're not, you're wrong, and you should think
 *		about it again.  i know, it happened to me.  we don't need to
 *		check commit time against the start time of this transaction
 *		because 2ph locking protects us from doing the wrong thing.
 *		if you mess around here, you'll break serializability.  the only
 *		problem with this code is that it does the wrong thing for system
 *		catalog updates, because the catalogs aren't subject to 2ph, so
 *		the serializability guarantees we provide don't extend to xacts
 *		that do catalog accesses.  this is unfortunate, but not critical.
 */
bool
HeapTupleSatisfiesNow(Relation relation, HeapTupleHeader tuple, Buffer buffer)
{
	const bool disableTupleHints = gp_disable_tuple_hints;

	WATCH_VISIBILITY_CLEAR();

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_NOT_HINT_COMMITTED);

		if (tuple->t_infomask & HEAP_XMIN_INVALID)
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_ABORTED);

			return false;
		}

		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_MOVED_AWAY_BY_VACUUM);

			if (TransactionIdIsCurrentTransactionId(xvac))
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_VACUUM_XID_CURRENT);

				return false;
			}
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

					WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_VACUUM_MOVED_INVALID);

					return false;
				}
				tuple->t_infomask |= HEAP_XMIN_COMMITTED;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_COMMITTED);
			}
		}
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_MOVED_IN_BY_VACUUM);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_VACUUM_XID_NOT_CURRENT);

				if (TransactionIdIsInProgress(xvac))
				{
					WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_VACUUM_XID_IN_PROGRESS);

					return false;
				}
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_COMMITTED;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

					WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_COMMITTED);
				}
				else
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

					WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_ABORTED);

					return false;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple)))
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_CURRENT);
#ifdef WATCH_VISIBILITY_IN_ACTION
			strcpy(WatchVisibilityXminCurrent, WatchCurrentTransactionString());
#endif

			if (HeapTupleHeaderGetCmin(tuple) >= GetCurrentCommandId())
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_INSERTED_AFTER_SCAN_STARTED);

				return false;	/* inserted after scan started */
			}

			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_INVALID);

				return true;
			}

			if (tuple->t_infomask & HEAP_IS_LOCKED)		/* not deleter */
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_LOCKED);

				return true;
			}

			Assert(!(tuple->t_infomask & HEAP_XMAX_IS_MULTI));

			/* deleting subtransaction aborted? */
			if (TransactionIdDidAbort(HeapTupleHeaderGetXmax(tuple)))
			{
				tuple->t_infomask |= HEAP_XMAX_INVALID;

				/* If ComboCID format cid, roll it back to normal Cmin */
				if (tuple->t_infomask & HEAP_COMBOCID)
				{
					HeapTupleHeaderSetCmin(tuple, HeapTupleHeaderGetCmin(tuple));
				}

				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);

				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMAX_ABORTED);

				return true;
			}

			Assert(TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(tuple)));

			if (HeapTupleHeaderGetCmax(tuple) >= GetCurrentCommandId())
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_DELETED_AFTER_SCAN_STARTED);

				return true;	/* deleted after scan started */
			}
			else
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_DELETED_BEFORE_SCAN_STARTED);

				return false;	/* deleted before scan started */
			}
		}
		else
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_NOT_CURRENT);
#ifdef WATCH_VISIBILITY_IN_ACTION
			strcpy(WatchVisibilityXminCurrent, WatchCurrentTransactionString());
#endif
			if (TransactionIdIsInProgress(HeapTupleHeaderGetXmin(tuple)))
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_IN_PROGRESS);

				return false;
			}
			else if (TransactionIdDidCommit(HeapTupleHeaderGetXmin(tuple)))
			{
				tuple->t_infomask |= HEAP_XMIN_COMMITTED;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_COMMITTED);
			}
			else
			{
				/* it must have aborted or crashed */
				tuple->t_infomask |= HEAP_XMIN_INVALID;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_ABORTED);

				return false;
			}
		}
	}

	/* by here, the inserting transaction has committed */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_INVALID_OR_ABORTED);

		return true;
	}

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_HINT_COMMITTED);

		if (tuple->t_infomask & HEAP_IS_LOCKED)
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_LOCKED);

			return true;
		}
		return false;
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_MULTIXACT);

		/* MultiXacts are currently only allowed to lock tuples */
		Assert(tuple->t_infomask & HEAP_IS_LOCKED);
		return true;
	}

	if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(tuple)))
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_CURRENT);

		if (tuple->t_infomask & HEAP_IS_LOCKED)
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_LOCKED);

			return true;
		}
		if (HeapTupleHeaderGetCmax(tuple) >= GetCurrentCommandId())
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_DELETED_AFTER_SCAN_STARTED);

			return true;	/* deleted after scan started */
		}
		else
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_DELETED_BEFORE_SCAN_STARTED);

			return false;	/* deleted before scan started */
		}
	}

	if (TransactionIdIsInProgress(HeapTupleHeaderGetXmax(tuple)))
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_IN_PROGRESS);

		return true;
	}

	if (!TransactionIdDidCommit(HeapTupleHeaderGetXmax(tuple)))
	{
		/* it must have aborted or crashed */
		tuple->t_infomask |= HEAP_XMAX_INVALID;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);

		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMAX_ABORTED);

		return true;
	}

	/* xmax transaction committed */

	if (tuple->t_infomask & HEAP_IS_LOCKED)
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_LOCKED);

		tuple->t_infomask |= HEAP_XMAX_INVALID;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
		return true;
	}

	tuple->t_infomask |= HEAP_XMAX_COMMITTED;
	markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);

	WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMAX_COMMITTED);

	return false;
}

/*
 * HeapTupleSatisfiesToast
 *		True iff heap tuple is valid as a TOAST row.
 *
 * This is a simplified version that only checks for VACUUM moving conditions.
 * It's appropriate for TOAST usage because TOAST really doesn't want to do
 * its own time qual checks; if you can see the main table row that contains
 * a TOAST reference, you should be able to see the TOASTed value.	However,
 * vacuuming a TOAST table is independent of the main table, and in case such
 * a vacuum fails partway through, we'd better do this much checking.
 *
 * Among other things, this means you can't do UPDATEs of rows in a TOAST
 * table.
 */
bool
HeapTupleSatisfiesToast(Relation relation, HeapTupleHeader tuple, Buffer buffer)
{
	const bool disableTupleHints = gp_disable_tuple_hints;

	WATCH_VISIBILITY_CLEAR();

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)
			return false;

		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
					return false;
				}
				tuple->t_infomask |= HEAP_XMIN_COMMITTED;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
			}
		}
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return false;
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_COMMITTED;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
				}
				else
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
					return false;
				}
			}
		}
	}

	/* otherwise assume the tuple is valid for TOAST. */
	return true;
}

/*
 * HeapTupleSatisfiesUpdate
 *
 *	Same logic as HeapTupleSatisfiesNow, but returns a more detailed result
 *	code, since UPDATE needs to know more than "is it visible?".  Also,
 *	tuples of my own xact are tested against the passed CommandId not
 *	CurrentCommandId.
 *
 *	The possible return codes are:
 *
 *	HeapTupleInvisible: the tuple didn't exist at all when the scan started,
 *	e.g. it was created by a later CommandId.
 *
 *	HeapTupleMayBeUpdated: The tuple is valid and visible, so it may be
 *	updated.
 *
 *	HeapTupleSelfUpdated: The tuple was updated by the current transaction,
 *	after the current scan started.
 *
 *	HeapTupleUpdated: The tuple was updated by a committed transaction.
 *
 *	HeapTupleBeingUpdated: The tuple is being updated by an in-progress
 *	transaction other than the current transaction.  (Note: this includes
 *	the case where the tuple is share-locked by a MultiXact, even if the
 *	MultiXact includes the current transaction.  Callers that want to
 *	distinguish that case must test for it themselves.)
 */
HTSU_Result
HeapTupleSatisfiesUpdate(Relation relation, HeapTupleHeader tuple, CommandId curcid,
						 Buffer buffer)
{
	const bool disableTupleHints = gp_disable_tuple_hints;

	WATCH_VISIBILITY_CLEAR();

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)
			return HeapTupleInvisible;

		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return HeapTupleInvisible;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
					return HeapTupleInvisible;
				}
				tuple->t_infomask |= HEAP_XMIN_COMMITTED;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
			}
		}
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return HeapTupleInvisible;
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_COMMITTED;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
				}
				else
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
					return HeapTupleInvisible;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple)))
		{
			if (HeapTupleHeaderGetCmin(tuple) >= curcid)
				return HeapTupleInvisible;		/* inserted after scan started */

			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return HeapTupleMayBeUpdated;

			if (tuple->t_infomask & HEAP_IS_LOCKED)		/* not deleter */
				return HeapTupleMayBeUpdated;

			Assert(!(tuple->t_infomask & HEAP_XMAX_IS_MULTI));

			/* deleting subtransaction aborted? */
			if (TransactionIdDidAbort(HeapTupleHeaderGetXmax(tuple)))
			{
				tuple->t_infomask |= HEAP_XMAX_INVALID;

				/* If ComboCID format cid, roll it back to normal Cmin */
				if (tuple->t_infomask & HEAP_COMBOCID)
				{
					HeapTupleHeaderSetCmin(tuple, HeapTupleHeaderGetCmin(tuple));
				}

				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
				return HeapTupleMayBeUpdated;
			}

			Assert(TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(tuple)));

			if (HeapTupleHeaderGetCmax(tuple) >= curcid)
				return HeapTupleSelfUpdated;	/* updated after scan started */
			else
				return HeapTupleInvisible;		/* updated before scan started */
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetXmin(tuple)))
			return HeapTupleInvisible;
		else if (TransactionIdDidCommit(HeapTupleHeaderGetXmin(tuple)))
		{
			tuple->t_infomask |= HEAP_XMIN_COMMITTED;
			markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
		}
		else
		{
			/* it must have aborted or crashed */
			tuple->t_infomask |= HEAP_XMIN_INVALID;
			markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
			return HeapTupleInvisible;
		}
	}

	/* by here, the inserting transaction has committed */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return HeapTupleMayBeUpdated;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (tuple->t_infomask & HEAP_IS_LOCKED)
			return HeapTupleMayBeUpdated;
		return HeapTupleUpdated;	/* updated by other */
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/* MultiXacts are currently only allowed to lock tuples */
		Assert(tuple->t_infomask & HEAP_IS_LOCKED);

		if (MultiXactIdIsRunning(HeapTupleHeaderGetXmax(tuple)))
			return HeapTupleBeingUpdated;
		tuple->t_infomask |= HEAP_XMAX_INVALID;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
		return HeapTupleMayBeUpdated;
	}

	if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(tuple)))
	{
		if (tuple->t_infomask & HEAP_IS_LOCKED)
			return HeapTupleMayBeUpdated;
		if (HeapTupleHeaderGetCmax(tuple) >= curcid)
			return HeapTupleSelfUpdated;		/* updated after scan started */
		else
			return HeapTupleInvisible;	/* updated before scan started */
	}

	if (TransactionIdIsInProgress(HeapTupleHeaderGetXmax(tuple)))
		return HeapTupleBeingUpdated;

	if (!TransactionIdDidCommit(HeapTupleHeaderGetXmax(tuple)))
	{
		/* it must have aborted or crashed */
		tuple->t_infomask |= HEAP_XMAX_INVALID;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
		return HeapTupleMayBeUpdated;
	}

	/* xmax transaction committed */

	if (tuple->t_infomask & HEAP_IS_LOCKED)
	{
		tuple->t_infomask |= HEAP_XMAX_INVALID;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
		return HeapTupleMayBeUpdated;
	}

	tuple->t_infomask |= HEAP_XMAX_COMMITTED;
	markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
	return HeapTupleUpdated;	/* updated by other */
}

/*
 * HeapTupleSatisfiesDirty
 *		True iff heap tuple is valid including effects of open transactions.
 *
 *	Here, we consider the effects of:
 *		all committed and in-progress transactions (as of the current instant)
 *		previous commands of this transaction
 *		changes made by the current command
 *
 * This is essentially like HeapTupleSatisfiesItself as far as effects of
 * the current transaction and committed/aborted xacts are concerned.
 * However, we also include the effects of other xacts still in progress.
 *
 * Returns extra information in the global variable SnapshotDirty, namely
 * xids of concurrent xacts that affected the tuple.  SnapshotDirty->xmin
 * is set to InvalidTransactionId if xmin is either committed good or
 * committed dead; or to xmin if that transaction is still in progress.
 * Similarly for SnapshotDirty->xmax.
 */
bool
HeapTupleSatisfiesDirty(Relation relation, HeapTupleHeader tuple, Buffer buffer)
{
	const bool disableTupleHints = gp_disable_tuple_hints;

	WATCH_VISIBILITY_CLEAR();

	SnapshotDirty->xmin = SnapshotDirty->xmax = InvalidTransactionId;

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)
			return false;

		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
					return false;
				}
				tuple->t_infomask |= HEAP_XMIN_COMMITTED;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
			}
		}
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return false;
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_COMMITTED;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
				}
				else
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
					return false;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple)))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return true;

			if (tuple->t_infomask & HEAP_IS_LOCKED)		/* not deleter */
				return true;

			Assert(!(tuple->t_infomask & HEAP_XMAX_IS_MULTI));

			/* deleting subtransaction aborted? */
			if (TransactionIdDidAbort(HeapTupleHeaderGetXmax(tuple)))
			{
				tuple->t_infomask |= HEAP_XMAX_INVALID;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
				return true;
			}

			Assert(TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(tuple)));

			return false;
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetXmin(tuple)))
		{
			SnapshotDirty->xmin = HeapTupleHeaderGetXmin(tuple);
			/* XXX shouldn't we fall through to look at xmax? */
			return true;		/* in insertion by other */
		}
		else if (TransactionIdDidCommit(HeapTupleHeaderGetXmin(tuple)))
		{
			tuple->t_infomask |= HEAP_XMIN_COMMITTED;
			markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
		}
		else
		{
			/* it must have aborted or crashed */
			tuple->t_infomask |= HEAP_XMIN_INVALID;
			markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);
			return false;
		}
	}

	/* by here, the inserting transaction has committed */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return true;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (tuple->t_infomask & HEAP_IS_LOCKED)
			return true;
		return false;			/* updated by other */
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/* MultiXacts are currently only allowed to lock tuples */
		Assert(tuple->t_infomask & HEAP_IS_LOCKED);
		return true;
	}

	if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(tuple)))
	{
		if (tuple->t_infomask & HEAP_IS_LOCKED)
			return true;
		return false;
	}

	if (TransactionIdIsInProgress(HeapTupleHeaderGetXmax(tuple)))
	{
		SnapshotDirty->xmax = HeapTupleHeaderGetXmax(tuple);
		return true;
	}

	if (!TransactionIdDidCommit(HeapTupleHeaderGetXmax(tuple)))
	{
		/* it must have aborted or crashed */
		tuple->t_infomask |= HEAP_XMAX_INVALID;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
		return true;
	}

	/* xmax transaction committed */

	if (tuple->t_infomask & HEAP_IS_LOCKED)
	{
		tuple->t_infomask |= HEAP_XMAX_INVALID;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
		return true;
	}

	tuple->t_infomask |= HEAP_XMAX_COMMITTED;
	markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);
	return false;				/* updated by other */
}

/*
 * HeapTupleSatisfiesSnapshot
 *		True iff heap tuple is valid for the given snapshot.
 *
 *	Here, we consider the effects of:
 *		all transactions committed as of the time of the given snapshot
 *		previous commands of this transaction
 *
 *	Does _not_ include:
 *		transactions shown as in-progress by the snapshot
 *		transactions started after the snapshot was taken
 *		changes made by the current command
 *
 * This is the same as HeapTupleSatisfiesNow, except that transactions that
 * were in progress or as yet unstarted when the snapshot was taken will
 * be treated as uncommitted, even if they have committed by now.
 *
 * (Notice, however, that the tuple status hint bits will be updated on the
 * basis of the true state of the transaction, even if we then pretend we
 * can't see it.)
 */
bool
HeapTupleSatisfiesSnapshot(Relation relation, HeapTupleHeader tuple, Snapshot snapshot,
						   Buffer buffer)
{
	const bool disableTupleHints = gp_disable_tuple_hints;
	bool inSnapshot = false;
	bool setDistributedSnapshotIgnore = false;

	WATCH_VISIBILITY_CLEAR();

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_NOT_HINT_COMMITTED);

		if (tuple->t_infomask & HEAP_XMIN_INVALID)
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_ABORTED);

			return false;
		}

		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_MOVED_AWAY_BY_VACUUM);

			if (TransactionIdIsCurrentTransactionId(xvac))
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_VACUUM_XID_CURRENT);

				return false;
			}
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

					WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_VACUUM_MOVED_INVALID);

					return false;
				}
				tuple->t_infomask |= HEAP_XMIN_COMMITTED;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_COMMITTED);
			}
		}
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_MOVED_IN_BY_VACUUM);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_VACUUM_XID_NOT_CURRENT);

				if (TransactionIdIsInProgress(xvac))
				{
					WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_VACUUM_XID_IN_PROGRESS);

					return false;
				}
				if (TransactionIdDidCommit(xvac))
				{
					tuple->t_infomask |= HEAP_XMIN_COMMITTED;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

					WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_COMMITTED);
				}
				else
				{
					tuple->t_infomask |= HEAP_XMIN_INVALID;
					markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

					WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_ABORTED);

					return false;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple)))
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_CURRENT);
#ifdef WATCH_VISIBILITY_IN_ACTION
			strcpy(WatchVisibilityXminCurrent, WatchCurrentTransactionString());
#endif

			if (HeapTupleHeaderGetCmin(tuple) >= snapshot->curcid)
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_INSERTED_AFTER_SCAN_STARTED);

				return false;	/* inserted after scan started */
			}

			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_INVALID);

				return true;
			}

			if (tuple->t_infomask & HEAP_IS_LOCKED)		/* not deleter */
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_LOCKED);

				return true;
			}

			Assert(!(tuple->t_infomask & HEAP_XMAX_IS_MULTI));

			/* deleting subtransaction aborted? */
			/* FIXME -- is this correct w.r.t. the cmax of the tuple? */
			if (TransactionIdDidAbort(HeapTupleHeaderGetXmax(tuple)))
			{
				tuple->t_infomask |= HEAP_XMAX_INVALID;

				/* If ComboCID format cid, roll it back to normal Cmin */
				if (tuple->t_infomask & HEAP_COMBOCID)
				{
					HeapTupleHeaderSetCmin(tuple, HeapTupleHeaderGetCmin(tuple));
				}

				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);

				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMAX_ABORTED);

				return true;
			}

			/*
			 * MPP-8317: cursors can't always *tell* that this is the current transaction.
			 */
			Assert(TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(tuple)));

			if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_DELETED_AFTER_SCAN_STARTED);

				return true;	/* deleted after scan started */
			}
			else
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_DELETED_BEFORE_SCAN_STARTED);

				return false;	/* deleted before scan started */
			}
		}
		else
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_NOT_CURRENT);
#ifdef WATCH_VISIBILITY_IN_ACTION
			strcpy(WatchVisibilityXminCurrent, WatchCurrentTransactionString());
#endif

			if (TransactionIdIsInProgress(HeapTupleHeaderGetXmin(tuple)))
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMIN_IN_PROGRESS);

				return false;
			}
			else if (TransactionIdDidCommit(HeapTupleHeaderGetXmin(tuple)))
			{
				tuple->t_infomask |= HEAP_XMIN_COMMITTED;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_COMMITTED);
			}
			else
			{
				/* it must have aborted or crashed */
				tuple->t_infomask |= HEAP_XMIN_INVALID;
				markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_ABORTED);

				return false;
			}
		}
	}

	/*
	 * By here, the inserting transaction has committed - have to check
	 * when...
	 */
	inSnapshot = XidInSnapshot(
		HeapTupleHeaderGetXmin(tuple),
		snapshot,
		/* isXmax */ false,
		((tuple->t_infomask2 & HEAP_XMIN_DISTRIBUTED_SNAPSHOT_IGNORE) != 0),
		&setDistributedSnapshotIgnore);
	if (setDistributedSnapshotIgnore)
	{
		tuple->t_infomask2 |= HEAP_XMIN_DISTRIBUTED_SNAPSHOT_IGNORE;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ true);

		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMIN_DISTRIBUTED_SNAPSHOT_IGNORE);
	}

	if (inSnapshot)
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SNAPSHOT_SAYS_XMIN_IN_PROGRESS);

		return false;			/* treat as still in progress */
	}

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_INVALID_OR_ABORTED);

		return true;
	}

	if (tuple->t_infomask & HEAP_IS_LOCKED)
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_LOCKED);

		return true;
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_MULTIXACT);

		/* MultiXacts are currently only allowed to lock tuples */
		Assert(tuple->t_infomask & HEAP_IS_LOCKED);
		return true;
	}

	if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_NOT_HINT_COMMITTED);

		if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmax(tuple)))
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_CURRENT);

			if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_DELETED_AFTER_SCAN_STARTED);

				return true;	/* deleted after scan started */
			}
			else
			{
				WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_DELETED_BEFORE_SCAN_STARTED);

				return false;	/* deleted before scan started */
			}
		}

		if (TransactionIdIsInProgress(HeapTupleHeaderGetXmax(tuple)))
		{
			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_XMAX_IN_PROGRESS);

			return true;
		}

		if (!TransactionIdDidCommit(HeapTupleHeaderGetXmax(tuple)))
		{
			/* it must have aborted or crashed */
			tuple->t_infomask |= HEAP_XMAX_INVALID;
			markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);

			WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMAX_ABORTED);

			return true;
		}

		/* xmax transaction committed */
		tuple->t_infomask |= HEAP_XMAX_COMMITTED;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);

		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMAX_COMMITTED);
	}

	/*
	 * OK, the deleting transaction committed too ... but when?
	 */
	inSnapshot = XidInSnapshot(
		HeapTupleHeaderGetXmax(tuple),
		snapshot,
		/* isXmax */ true,
		((tuple->t_infomask2 & HEAP_XMAX_DISTRIBUTED_SNAPSHOT_IGNORE) != 0),
		&setDistributedSnapshotIgnore);
	if (setDistributedSnapshotIgnore)
	{
		tuple->t_infomask2 |= HEAP_XMAX_DISTRIBUTED_SNAPSHOT_IGNORE;
		markDirty(buffer, disableTupleHints, relation, tuple, /* isXmin */ false);

		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SET_XMAX_DISTRIBUTED_SNAPSHOT_IGNORE);
	}

	if (inSnapshot)
	{
		WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SNAPSHOT_SAYS_XMAX_IN_PROGRESS);

		return true;			/* treat as still in progress */
	}

	WATCH_VISIBILITY_ADD(WATCH_VISIBILITY_SNAPSHOT_SAYS_XMAX_VISIBLE);

	return false;
}


/*
 * HeapTupleSatisfiesVacuum
 *
 *	Determine the status of tuples for VACUUM purposes.  Here, what
 *	we mainly want to know is if a tuple is potentially visible to *any*
 *	running transaction.  If so, it can't be removed yet by VACUUM.
 *
 * OldestXmin is a cutoff XID (obtained from GetOldestXmin()).	Tuples
 * deleted by XIDs >= OldestXmin are deemed "recently dead"; they might
 * still be visible to some open transaction, so we can't remove them,
 * even if we see that the deleting transaction has committed.
 */
HTSV_Result
HeapTupleSatisfiesVacuum(HeapTupleHeader tuple, TransactionId OldestXmin,
						 Buffer buffer, bool vacuumFull)
{
	WATCH_VISIBILITY_CLEAR();

	/*
	 * Has inserting transaction committed?
	 *
	 * If the inserting transaction aborted, then the tuple was never visible
	 * to any other transaction, so we can delete it immediately.
	 */
	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)
		{
			return HEAPTUPLE_DEAD;
		}
		else if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return HEAPTUPLE_DELETE_IN_PROGRESS;
			if (TransactionIdIsInProgress(xvac))
				return HEAPTUPLE_DELETE_IN_PROGRESS;
			if (TransactionIdDidCommit(xvac))
			{
				tuple->t_infomask |= HEAP_XMIN_INVALID;
				SetBufferCommitInfoNeedsSave(buffer);
				return HEAPTUPLE_DEAD;
			}
			tuple->t_infomask |= HEAP_XMIN_COMMITTED;
			SetBufferCommitInfoNeedsSave(buffer);
		}
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return HEAPTUPLE_INSERT_IN_PROGRESS;
			if (TransactionIdIsInProgress(xvac))
				return HEAPTUPLE_INSERT_IN_PROGRESS;
			if (TransactionIdDidCommit(xvac))
			{
				tuple->t_infomask |= HEAP_XMIN_COMMITTED;
				SetBufferCommitInfoNeedsSave(buffer);
			}
			else
			{
				tuple->t_infomask |= HEAP_XMIN_INVALID;
				SetBufferCommitInfoNeedsSave(buffer);
				return HEAPTUPLE_DEAD;
			}
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetXmin(tuple)))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return HEAPTUPLE_INSERT_IN_PROGRESS;
			if (tuple->t_infomask & HEAP_IS_LOCKED)
				return HEAPTUPLE_INSERT_IN_PROGRESS;
			/* inserted and then deleted by same xact */
			return HEAPTUPLE_DELETE_IN_PROGRESS;
		}
		else if (TransactionIdDidCommit(HeapTupleHeaderGetXmin(tuple)))
		{
			tuple->t_infomask |= HEAP_XMIN_COMMITTED;
			SetBufferCommitInfoNeedsSave(buffer);
		}
		else
		{
			/*
			 * Not in Progress, Not Committed, so either Aborted or crashed
			 */
			tuple->t_infomask |= HEAP_XMIN_INVALID;
			SetBufferCommitInfoNeedsSave(buffer);
			return HEAPTUPLE_DEAD;
		}
		/* Should only get here if we set XMIN_COMMITTED */
		Assert(tuple->t_infomask & HEAP_XMIN_COMMITTED);
	}

	/*
	 * Okay, the inserter committed, so it was good at some point.	Now what
	 * about the deleting transaction?
	 */
	if (tuple->t_infomask & HEAP_XMAX_INVALID)
		return HEAPTUPLE_LIVE;

	if (tuple->t_infomask & HEAP_IS_LOCKED)
	{
		/*
		 * "Deleting" xact really only locked it, so the tuple is live in any
		 * case.  However, we should make sure that either XMAX_COMMITTED or
		 * XMAX_INVALID gets set once the xact is gone, to reduce the costs
		 * of examining the tuple for future xacts.  Also, marking dead
		 * MultiXacts as invalid here provides defense against MultiXactId
		 * wraparound (see also comments in heap_freeze_tuple()).
		 */
		if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
		{
			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				if (MultiXactIdIsRunning(HeapTupleHeaderGetXmax(tuple)))
					return HEAPTUPLE_LIVE;
			}
			else
			{
				if (TransactionIdIsInProgress(HeapTupleHeaderGetXmax(tuple)))
					return HEAPTUPLE_LIVE;
			}

			/*
			 * We don't really care whether xmax did commit, abort or crash.
			 * We know that xmax did lock the tuple, but it did not and will
			 * never actually update it.
			 */
			tuple->t_infomask |= HEAP_XMAX_INVALID;
			SetBufferCommitInfoNeedsSave(buffer);
		}
		return HEAPTUPLE_LIVE;
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/* MultiXacts are currently only allowed to lock tuples */
		Assert(tuple->t_infomask & HEAP_IS_LOCKED);
		return HEAPTUPLE_LIVE;
	}

	if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
	{
		if (TransactionIdIsInProgress(HeapTupleHeaderGetXmax(tuple)))
		{
			return HEAPTUPLE_DELETE_IN_PROGRESS;
		}
		else if (TransactionIdDidCommit(HeapTupleHeaderGetXmax(tuple)))
		{
			tuple->t_infomask |= HEAP_XMAX_COMMITTED;
			SetBufferCommitInfoNeedsSave(buffer);
		}
		else
		{
			/*
			 * Not in Progress, Not Committed, so either Aborted or crashed
			 */
			tuple->t_infomask |= HEAP_XMAX_INVALID;
			SetBufferCommitInfoNeedsSave(buffer);
			return HEAPTUPLE_LIVE;
		}
		/* Should only get here if we set XMAX_COMMITTED */
		Assert(tuple->t_infomask & HEAP_XMAX_COMMITTED);
	}

	/*
	 * Deleter committed, but check special cases.
	 */

	if (TransactionIdEquals(HeapTupleHeaderGetXmin(tuple),
							HeapTupleHeaderGetXmax(tuple)))
	{
		/*
		 * Inserter also deleted it, so it was never visible to anyone else.
		 * However, we can only remove it early if it's not an updated tuple;
		 * else its parent tuple is linking to it via t_ctid, and this tuple
		 * mustn't go away before the parent does.
		 */
		if (!(tuple->t_infomask & HEAP_UPDATED))
			return HEAPTUPLE_DEAD;
	}

	if (!TransactionIdPrecedes(HeapTupleHeaderGetXmax(tuple), OldestXmin))
	{
		/* deleting xact is too recent, tuple could still be visible */
		return HEAPTUPLE_RECENTLY_DEAD;
	}

	/* Otherwise, it's dead and removable */
	return HEAPTUPLE_DEAD;
}

/*
 * GetTransactionSnapshot
 *		Get the appropriate snapshot for a new query in a transaction.
 *
 * The SerializableSnapshot is the first one taken in a transaction.
 * In serializable mode we just use that one throughout the transaction.
 * In read-committed mode, we take a new snapshot each time we are called.
 *
 * Note that the return value points at static storage that will be modified
 * by future calls and by CommandCounterIncrement().  Callers should copy
 * the result with CopySnapshot() if it is to be used very long.
 */
Snapshot
GetTransactionSnapshot(void)
{
	/* First call in transaction? */
	if (SerializableSnapshot == NULL)
	{
		SerializableSnapshot = GetSnapshotData(&SerializableSnapshotData, true);
		return SerializableSnapshot;
	}

	if (IsXactIsoLevelSerializable)
	{
		return SerializableSnapshot;
	}

	LatestSnapshot = GetSnapshotData(&LatestSnapshotData, false);

	return LatestSnapshot;
}

/*
 * GetLatestSnapshot
 *		Get a snapshot that is up-to-date as of the current instant,
 *		even if we are executing in SERIALIZABLE mode.
 */
Snapshot
GetLatestSnapshot(void)
{
	/* Should not be first call in transaction */
	if (SerializableSnapshot == NULL)
		elog(ERROR, "no snapshot has been set");

	LatestSnapshot = GetSnapshotData(&LatestSnapshotData, false);

	return LatestSnapshot;
}

/*
 * CopySnapshot
 *		Copy the given snapshot.
 *
 * The copy is palloc'd in the current memory context.
 *
 * Note that this will not work on "special" snapshots.
 */
Snapshot
CopySnapshot(Snapshot snapshot)
{
	Snapshot	newsnap;
	Size		subxipoff;
	Size		size;

	if (!IsMVCCSnapshot(snapshot))
		return snapshot;

	/* We allocate any XID arrays needed in the same palloc block. */
	size = subxipoff = sizeof(SnapshotData) +
		snapshot->xcnt * sizeof(TransactionId);
	if (snapshot->subxcnt > 0)
		size += snapshot->subxcnt * sizeof(TransactionId);

	newsnap = (Snapshot) palloc(size);
	memcpy(newsnap, snapshot, sizeof(SnapshotData));

	/* setup XID array */
	if (snapshot->xcnt > 0)
	{
		newsnap->xip = (TransactionId *) (newsnap + 1);
		memcpy(newsnap->xip, snapshot->xip,
			   snapshot->xcnt * sizeof(TransactionId));
	}
	else
		newsnap->xip = NULL;

	/* setup subXID array */
	if (snapshot->subxcnt > 0)
	{
		newsnap->subxip = (TransactionId *) ((char *) newsnap + subxipoff);
		memcpy(newsnap->subxip, snapshot->subxip,
			   snapshot->subxcnt * sizeof(TransactionId));
	}
	else
		newsnap->subxip = NULL;

	return newsnap;
}

/*
 * FreeSnapshot
 *		Free a snapshot previously copied with CopySnapshot.
 *
 * This is currently identical to pfree, but is provided for cleanliness.
 *
 * Do *not* apply this to the results of GetTransactionSnapshot or
 * GetLatestSnapshot.
 */
void
FreeSnapshot(Snapshot snapshot)
{
	if (!IsMVCCSnapshot(snapshot))
		return;

	pfree(snapshot);
}

/*
 * FreeXactSnapshot
 *		Free snapshot(s) at end of transaction.
 */
void
FreeXactSnapshot(void)
{
	/*
	 * We do not free the xip arrays for the static snapshot structs; they
	 * will be reused soon. So this is now just a state change to prevent
	 * outside callers from accessing the snapshots.
	 */
	SerializableSnapshot = NULL;
	LatestSnapshot = NULL;

	ActiveSnapshot = NULL;		/* just for cleanliness */
}

struct mpp_xid_map_entry {
	pid_t				pid;
	TransactionId		global;
	TransactionId		local;
};

struct mpp_xid_map {
	int			size;
	int			cur;
	struct mpp_xid_map_entry map[1];
};

/*
 * mpp_global_xid_map
 */
Datum
mpp_global_xid_map(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	struct mpp_xid_map *ctx;
	bool	nulls[3];
	int			i, j;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;
		volatile SharedSnapshotStruct *arrayP = sharedSnapshotArray;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* build tupdesc for result tuples */
		/* this had better match pg_locks view in system_views.sql */
		tupdesc = CreateTemplateTupleDesc(3, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "pid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "localxid",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "globalxid",
						   XIDOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/* figure out how many slots we need */

		ctx = (struct mpp_xid_map *)palloc0(sizeof(struct mpp_xid_map) +
											arrayP->maxSlots * sizeof(struct mpp_xid_map_entry));

		j = 0;
		LWLockAcquire(SharedSnapshotLock, LW_EXCLUSIVE);
		for (i = 0; i < arrayP->maxSlots; i++)
		{
			SharedSnapshotSlot *testSlot = &arrayP->slots[i];

			if (testSlot->slotid != -1)
			{
				ctx->map[j].pid = testSlot->pid;
				ctx->map[j].local = testSlot->xid;
				ctx->map[j].global = testSlot->QDxid;
				j++;
			}
		}
		LWLockRelease(SharedSnapshotLock);
		ctx->size = j;

		funcctx->user_fctx = (void *)ctx;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	ctx = (struct mpp_xid_map *)funcctx->user_fctx;

	MemSet(nulls, false, sizeof(nulls));
	while (ctx->cur < ctx->size)
	{
		Datum		values[3];

		HeapTuple	tuple;
		Datum		result;

		/*
		 * Form tuple with appropriate data.
		 */
		MemSet(values, 0, sizeof(values));

		values[0] = Int32GetDatum(ctx->map[ctx->cur].pid);
		values[1] = TransactionIdGetDatum(ctx->map[ctx->cur].local);
		values[2] = TransactionIdGetDatum(ctx->map[ctx->cur].global);

		ctx->cur++;

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * XidInSnapshot
 *		Is the given XID still-in-progress according to the distributed
 *      and local snapshots?
 */
static bool
XidInSnapshot(
	TransactionId xid, Snapshot snapshot, bool isXmax,
    bool distributedSnapshotIgnore, bool *setDistributedSnapshotIgnore)
{
	*setDistributedSnapshotIgnore = false;
	return XidInSnapshot_Local(xid, snapshot, isXmax);
}

/*
 * XidInSnapshot_Local
 *		Is the given XID still-in-progress according to the local snapshot?
 *
 * Note: GetSnapshotData never stores either top xid or subxids of our own
 * backend into a snapshot, so these xids will not be reported as "running"
 * by this function.  This is OK for current uses, because we actually only
 * apply this for known-committed XIDs.
 */
static bool
XidInSnapshot_Local(TransactionId xid, Snapshot snapshot, bool isXmax)
{
#ifdef WATCH_VISIBILITY_IN_ACTION
	TransactionId saveXid = xid;
#endif
	uint32		i;

	/*
	 * Make a quick range check to eliminate most XIDs without looking at the
	 * xip arrays.	Note that this is OK even if we convert a subxact XID to
	 * its parent below, because a subxact with XID < xmin has surely also got
	 * a parent with XID < xmin, while one with XID >= xmax must belong to a
	 * parent that was not yet committed at the time of this snapshot.
	 */

	/* Any xid < xmin is not in-progress */
	if (TransactionIdPrecedes(xid, snapshot->xmin))
	{
		WATCH_VISIBILITY_ADDPAIR(
			WATCH_VISIBILITY_XMIN_LESS_THAN_SNAPSHOT_XMIN, isXmax);

		return false;
	}
	/* Any xid >= xmax is in-progress */
	if (TransactionIdFollowsOrEquals(xid, snapshot->xmax))
	{
		WATCH_VISIBILITY_ADDPAIR(
			WATCH_VISIBILITY_XMIN_LESS_THAN_SNAPSHOT_XMIN, isXmax);

		return true;
	}

	/*
	 * If the snapshot contains full subxact data, the fastest way to check
	 * things is just to compare the given XID against both subxact XIDs and
	 * top-level XIDs.	If the snapshot overflowed, we have to use pg_subtrans
	 * to convert a subxact XID to its parent XID, but then we need only look
	 * at top-level XIDs not subxacts.
	 */
	if (snapshot->subxcnt >= 0)
	{
		/* full data, so search subxip */
		int32		j;

		for (j = 0; j < snapshot->subxcnt; j++)
		{
			if (TransactionIdEquals(xid, snapshot->subxip[j]))
			{
				WATCH_VISIBILITY_ADDPAIR(
					WATCH_VISIBILITY_XMIN_SNAPSHOT_SUBTRANSACTION, isXmax);
				return true;
			}
		}

		/* not there, fall through to search xip[] */
	}
	else
	{
		/* overflowed, so convert xid to top-level */
		xid = SubTransGetTopmostTransaction(xid);

#ifdef WATCH_VISIBILITY_IN_ACTION
		if (saveXid != xid)
		{
			WATCH_VISIBILITY_ADDPAIR(
				WATCH_VISIBILITY_XMIN_MAPPED_SUBTRANSACTION, isXmax);
		}
#endif
		/*
		 * If xid was indeed a subxact, we might now have an xid < xmin, so
		 * recheck to avoid an array scan.	No point in rechecking xmax.
		 */
		if (TransactionIdPrecedes(xid, snapshot->xmin))
		{
			WATCH_VISIBILITY_ADDPAIR(
				WATCH_VISIBILITY_XMIN_LESS_THAN_SNAPSHOT_XMIN_2, isXmax);

			return false;
		}
	}

	for (i = 0; i < snapshot->xcnt; i++)
	{
		if (TransactionIdEquals(xid, snapshot->xip[i]))
		{
			WATCH_VISIBILITY_ADDPAIR(
				WATCH_VISIBILITY_XMIN_SNAPSHOT_IN_PROGRESS, isXmax);

			return true;
		}
	}

	WATCH_VISIBILITY_ADDPAIR(
		WATCH_VISIBILITY_XMIN_SNAPSHOT_NOT_IN_PROGRESS, isXmax);

	return false;
}

#ifdef WATCH_VISIBILITY_IN_ACTION

bool WatchVisibilityAllZeros(void)
{
	int i;

	for (i = 0; i < WATCH_VISIBILITY_BYTE_LEN; i++)
	{
		if (WatchVisibilityFlags[i] != 0)
			return false;
	}

	return true;
}

static char WatchVisibilityBuffer[2000];

char *
WatchVisibilityInActionString(
	BlockNumber 	page,
	OffsetNumber 	lineoff,
	HeapTuple		tuple,
	Snapshot		snapshot)
{
	int b;
	int count = 0;
	CommandId snapshotCurcid = 0;
	TransactionId snapshotXmin = InvalidTransactionId;
	TransactionId snapshotXmax = InvalidTransactionId;
	DistributedTransactionId xminAllDistributedSnapshots = InvalidTransactionId;
	DistributedTransactionId xminDistributedSnapshot = InvalidTransactionId;
	DistributedTransactionId xmaxDistributedSnapshot = InvalidTransactionId;
	char *snapshotStr = NULL;
	bool atLeastOne = false;

	if (snapshot == SnapshotNow)
		snapshotStr = "SnapshotNow";
	else if (snapshot == SnapshotSelf)
		snapshotStr = "SnapshotSelf";
	else if (snapshot == SnapshotAny)
		snapshotStr = "SnapshotAny";
	else if (snapshot == SnapshotToast)
		snapshotStr = "SnapshotToast";
	else if (snapshot == SnapshotDirty)
		snapshotStr = "SnapshotDirty";
	else
	{
		if (snapshot == LatestSnapshot)
			snapshotStr = "LatestSnapshot";
		else if (snapshot == SerializableSnapshot)
			snapshotStr = "SerializableSnapshot";
		else
			snapshotStr = "OtherSnapshot";

		snapshotCurcid = snapshot->curcid;
		snapshotXmin = snapshot->xmin;
		snapshotXmax = snapshot->xmax;
	}

	count += sprintf(&WatchVisibilityBuffer[count],"(%u,%u)", page, lineoff);
	count += sprintf(&WatchVisibilityBuffer[count]," xmin %u, xmax %u, %s: ",
		             HeapTupleHeaderGetXmin(tuple->t_data),
		             HeapTupleHeaderGetXmax(tuple->t_data),
		             snapshotStr);

	if (WatchVisibilityAllZeros())
	{
		count += sprintf(&WatchVisibilityBuffer[count],"no visiblity path collected");
		return WatchVisibilityBuffer;
	}

	for (b = 0; b < MAX_WATCH_VISIBILITY; b++)
	{
		int nthbyte = (b) >> 3;
		char nthbit  = 1 << ((b) & 7);

		if ((WatchVisibilityFlags[nthbyte] & nthbit) != 0)
		{
			if (atLeastOne)
				count += sprintf(&WatchVisibilityBuffer[count], "; ");

			switch (b)
			{
				case WATCH_VISIBILITY_XMIN_NOT_HINT_COMMITTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin not hint committed");
					break;
				case WATCH_VISIBILITY_XMIN_ABORTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin aborted");
					break;
				case WATCH_VISIBILITY_XMIN_MOVED_AWAY_BY_VACUUM:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin moved away by vacuum");
					break;
				case WATCH_VISIBILITY_VACUUM_XID_CURRENT:
					count += sprintf(&WatchVisibilityBuffer[count],"Vacuum xid current");
					break;
				case WATCH_VISIBILITY_SET_XMIN_VACUUM_MOVED_INVALID:
					count += sprintf(&WatchVisibilityBuffer[count],"Set xmin vacuum moved invalid");
					break;
				case WATCH_VISIBILITY_SET_XMIN_COMMITTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Set xmin committed");
					break;
				case WATCH_VISIBILITY_SET_XMIN_ABORTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Set xmin aborted");
					break;
				case WATCH_VISIBILITY_XMIN_MOVED_IN_BY_VACUUM:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin moved in by vacuum");
					break;
				case WATCH_VISIBILITY_VACUUM_XID_NOT_CURRENT:
					count += sprintf(&WatchVisibilityBuffer[count],"Vacuum xid not current");
					break;
				case WATCH_VISIBILITY_VACUUM_XID_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Vacuum xid in progress");
					break;
				case WATCH_VISIBILITY_XMIN_CURRENT:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin current %s", WatchVisibilityXminCurrent);
					break;
				case WATCH_VISIBILITY_XMIN_NOT_CURRENT:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin not current %s", WatchVisibilityXminCurrent);
					break;
				case WATCH_VISIBILITY_XMIN_INSERTED_AFTER_SCAN_STARTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin inserted after scan started (curcid %d)",
									 snapshotCurcid);
					break;
				case WATCH_VISIBILITY_XMAX_INVALID:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax invalid");
					break;
				case WATCH_VISIBILITY_LOCKED:
					count += sprintf(&WatchVisibilityBuffer[count],"Locked");
					break;
				case WATCH_VISIBILITY_SET_XMAX_ABORTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Set xmax aborted");
					break;
				case WATCH_VISIBILITY_DELETED_AFTER_SCAN_STARTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Deleted after scan started");
					break;
				case WATCH_VISIBILITY_DELETED_BEFORE_SCAN_STARTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Deleted before scan started");
					break;
				case WATCH_VISIBILITY_XMIN_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin in progress");
					break;
				case WATCH_VISIBILITY_SNAPSHOT_SAYS_XMIN_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Snapshot says xmin in progress");
					break;
				case WATCH_VISIBILITY_XMAX_INVALID_OR_ABORTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax invalid or aborted");
					break;
				case WATCH_VISIBILITY_XMAX_MULTIXACT:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax multixact");
					break;
				case WATCH_VISIBILITY_XMAX_NOT_HINT_COMMITTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax not hint committed");
					break;
				case WATCH_VISIBILITY_XMAX_HINT_COMMITTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax hint committed");
					break;
				case WATCH_VISIBILITY_XMAX_CURRENT:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax current");
					break;
				case WATCH_VISIBILITY_XMAX_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax in progress");
					break;
				case WATCH_VISIBILITY_SET_XMAX_COMMITTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Set xmax committed");
					break;
				case WATCH_VISIBILITY_SNAPSHOT_SAYS_XMAX_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax in progress");
					break;
				case WATCH_VISIBILITY_SNAPSHOT_SAYS_XMAX_VISIBLE:
					count += sprintf(&WatchVisibilityBuffer[count],"Snapshot says xmax visible");
					break;
				case WATCH_VISIBILITY_USE_DISTRIBUTED_SNAPSHOT_FOR_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"Use distributed snapshot for xmin");
					break;
				case WATCH_VISIBILITY_USE_DISTRIBUTED_SNAPSHOT_FOR_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"Use distributed snapshot for xmax");
					break;
				case WATCH_VISIBILITY_IGNORE_DISTRIBUTED_SNAPSHOT_FOR_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"Ignore distributed snapshot for xmin");
					break;
				case WATCH_VISIBILITY_IGNORE_DISTRIBUTED_SNAPSHOT_FOR_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"Ignore distributed snapshot for xmax");
					break;
				case WATCH_VISIBILITY_NO_DISTRIBUTED_SNAPSHOT_FOR_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"No distributed snapshot for xmin");
					break;
				case WATCH_VISIBILITY_NO_DISTRIBUTED_SNAPSHOT_FOR_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"No distributed snapshot for xmax");
					break;
				case WATCH_VISIBILITY_XMIN_DISTRIBUTED_SNAPSHOT_IN_PROGRESS_FOUND_BY_LOCAL:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin distributed snapshot in progress -- found by local");
					break;
				case WATCH_VISIBILITY_XMAX_DISTRIBUTED_SNAPSHOT_IN_PROGRESS_FOUND_BY_LOCAL:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax distributed snapshot in progress -- found by local");
					break;
				case WATCH_VISIBILITY_XMIN_LOCAL_DISTRIBUTED_CACHE_RETURNED_LOCAL:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin in local distributed cache returned local");
					break;
				case WATCH_VISIBILITY_XMAX_LOCAL_DISTRIBUTED_CACHE_RETURNED_LOCAL:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax in local distributed cache returned local");
					break;
				case WATCH_VISIBILITY_XMIN_LOCAL_DISTRIBUTED_CACHE_RETURNED_DISTRIB:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin in local distributed cache returned distrib");
					break;
				case WATCH_VISIBILITY_XMAX_LOCAL_DISTRIBUTED_CACHE_RETURNED_DISTRIB:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax in local distributed cache returned distrib");
					break;
				case WATCH_VISIBILITY_XMIN_NOT_KNOWN_BY_LOCAL_DISTRIBUTED_XACT:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin not known by local distributed xact module");
					break;
				case WATCH_VISIBILITY_XMAX_NOT_KNOWN_BY_LOCAL_DISTRIBUTED_XACT:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax not known by local distributed xact module");
					break;
				case WATCH_VISIBILITY_XMIN_DIFF_DTM_START_IN_DISTRIBUTED_LOG:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin different DTM start in distributed log");
					break;
				case WATCH_VISIBILITY_XMAX_DIFF_DTM_START_IN_DISTRIBUTED_LOG:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax different DTM start in distributed log");
					break;
				case WATCH_VISIBILITY_XMIN_FOUND_IN_DISTRIBUTED_LOG:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin found in distributed log");
					break;
				case WATCH_VISIBILITY_XMAX_FOUND_IN_DISTRIBUTED_LOG:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax found in distributed log");
					break;
				case WATCH_VISIBILITY_XMIN_KNOWN_LOCAL_IN_DISTRIBUTED_LOG:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin known local in distributed log");
					break;
				case WATCH_VISIBILITY_XMAX_KNOWN_LOCAL_IN_DISTRIBUTED_LOG:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax known local in distributed log");
					break;
				case WATCH_VISIBILITY_XMIN_KNOWN_BY_LOCAL_DISTRIBUTED_XACT:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin known by local distributed xact module");
					break;
				case WATCH_VISIBILITY_XMAX_KNOWN_BY_LOCAL_DISTRIBUTED_XACT:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax known by local distributed xact module");
					break;
				case WATCH_VISIBILITY_XMIN_LESS_THAN_ALL_CURRENT_DISTRIBUTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin < all current distributed %u",
									 xminAllDistributedSnapshots);
					break;
				case WATCH_VISIBILITY_XMAX_LESS_THAN_ALL_CURRENT_DISTRIBUTED:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax < all current distributed %u",
									 xminAllDistributedSnapshots);
					break;
				case WATCH_VISIBILITY_XMIN_LESS_THAN_DISTRIBUTED_SNAPSHOT_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin < distributed snapshot xmin %u",
									 xminDistributedSnapshot);
					break;
				case WATCH_VISIBILITY_XMAX_LESS_THAN_DISTRIBUTED_SNAPSHOT_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax < distributed snapshot xmin %u",
									 xminDistributedSnapshot);
					break;
				case WATCH_VISIBILITY_XMIN_GREATER_THAN_EQUAL_DISTRIBUTED_SNAPSHOT_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin >= distributed snapshot xmax %u",
									 xmaxDistributedSnapshot);
					break;
				case WATCH_VISIBILITY_XMAX_GREATER_THAN_EQUAL_DISTRIBUTED_SNAPSHOT_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax >= distributed snapshot xmax %u",
									 xmaxDistributedSnapshot);
					break;
				case WATCH_VISIBILITY_XMIN_DISTRIBUTED_SNAPSHOT_IN_PROGRESS_BY_DISTRIB:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin in distributed snapshot in progress by distrib");
					break;
				case WATCH_VISIBILITY_XMAX_DISTRIBUTED_SNAPSHOT_IN_PROGRESS_BY_DISTRIB:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax in distributed snapshot in progress by distrib");
					break;
				case WATCH_VISIBILITY_XMIN_DISTRIBUTED_SNAPSHOT_NOT_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin not in distributed snapshot in progress");
					break;
				case WATCH_VISIBILITY_XMAX_DISTRIBUTED_SNAPSHOT_NOT_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax not in distributed snapshot in progress");
					break;
				case WATCH_VISIBILITY_XMIN_LESS_THAN_SNAPSHOT_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin < snapshot xmin %u",
									 snapshotXmin);
					break;
				case WATCH_VISIBILITY_XMAX_LESS_THAN_SNAPSHOT_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax < snapshot xmin %u",
									 snapshotXmin);
					break;
				case WATCH_VISIBILITY_XMIN_GREATER_THAN_EQUAL_SNAPSHOT_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin >= snapshot xmax %u",
									 snapshotXmax);
					break;
				case WATCH_VISIBILITY_XMAX_GREATER_THAN_EQUAL_SNAPSHOT_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax >= snapshot xmax %u",
									 snapshotXmax);
					break;
				case WATCH_VISIBILITY_XMIN_SNAPSHOT_SUBTRANSACTION:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin snapshot subtransaction");
					break;
				case WATCH_VISIBILITY_XMAX_SNAPSHOT_SUBTRANSACTION:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax snapshot subtransaction");
					break;
				case WATCH_VISIBILITY_XMIN_MAPPED_SUBTRANSACTION:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin mapped subtransaction");
					break;
				case WATCH_VISIBILITY_XMAX_MAPPED_SUBTRANSACTION:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax mapped subtransaction");
					break;
				case WATCH_VISIBILITY_XMIN_LESS_THAN_SNAPSHOT_XMIN_2:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin < snapshot xmin %u (#2)",
									 snapshotXmin);
					break;
				case WATCH_VISIBILITY_XMAX_LESS_THAN_SNAPSHOT_XMIN_2:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax < snapshot xmin %u (#2)",
									 snapshotXmin);
					break;
				case WATCH_VISIBILITY_XMIN_SNAPSHOT_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin snapshot in progress");
					break;
				case WATCH_VISIBILITY_XMAX_SNAPSHOT_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax snapshot in progress");
					break;
				case WATCH_VISIBILITY_XMIN_SNAPSHOT_NOT_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmin snapshot not in progress");
					break;
				case WATCH_VISIBILITY_XMAX_SNAPSHOT_NOT_IN_PROGRESS:
					count += sprintf(&WatchVisibilityBuffer[count],"Xmax snapshot not in progress");
					break;
				case WATCH_VISIBILITY_SET_XMIN_DISTRIBUTED_SNAPSHOT_IGNORE:
					count += sprintf(&WatchVisibilityBuffer[count],"Set xmin distributed snapshot ignore");
					break;
				case WATCH_VISIBILITY_SET_XMAX_DISTRIBUTED_SNAPSHOT_IGNORE:
					count += sprintf(&WatchVisibilityBuffer[count],"Set xmax distributed snapshot ignore");
					break;
				case WATCH_VISIBILITY_GUC_OFF_MARK_BUFFFER_DIRTY_FOR_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints OFF) mark buffer dirty for xmin");
					break;
				case WATCH_VISIBILITY_GUC_OFF_MARK_BUFFFER_DIRTY_FOR_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints OFF) mark buffer dirty for xmax");
					break;
				case WATCH_VISIBILITY_NO_REL_MARK_BUFFFER_DIRTY_FOR_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints ON) no relation -- mark buffer dirty for xmin");
					break;
				case WATCH_VISIBILITY_NO_REL_MARK_BUFFFER_DIRTY_FOR_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints ON) no relation -- mark buffer dirty for xmax");
					break;
				case WATCH_VISIBILITY_SYS_CAT_MARK_BUFFFER_DIRTY_FOR_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints ON) system catalog -- mark buffer dirty for xmin");
					break;
				case WATCH_VISIBILITY_SYS_CAT_MARK_BUFFFER_DIRTY_FOR_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints ON) system catalog -- mark buffer dirty for xmax");
					break;
				case WATCH_VISIBILITY_NO_XID_MARK_BUFFFER_DIRTY_FOR_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints ON) no xid -- mark buffer dirty for xmin");
					break;
				case WATCH_VISIBILITY_NO_XID_MARK_BUFFFER_DIRTY_FOR_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints ON) no xid -- mark buffer dirty for xmax");
					break;
				case WATCH_VISIBILITY_TOO_OLD_MARK_BUFFFER_DIRTY_FOR_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints ON) too old -- mark buffer dirty for xmin");
					break;
				case WATCH_VISIBILITY_TOO_OLD_MARK_BUFFFER_DIRTY_FOR_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints ON) too old -- mark buffer dirty for xmax");
					break;
				case WATCH_VISIBILITY_YOUNG_DO_NOT_MARK_BUFFFER_DIRTY_FOR_XMIN:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints ON) young -- do not mark buffer dirty for xmin");
					break;
				case WATCH_VISIBILITY_YOUNG_DO_NOT_MARK_BUFFFER_DIRTY_FOR_XMAX:
					count += sprintf(&WatchVisibilityBuffer[count],"(gp_disable_tuple_hints ON) young -- do not mark buffer dirty for xmax");
					break;
				default:
					count += sprintf(&WatchVisibilityBuffer[count],"<Unknown %d>", b);
					break;
			}

			atLeastOne = true;
		}

	}

	return WatchVisibilityBuffer;
}

#endif

static char *TupleTransactionStatus_Name(TupleTransactionStatus status)
{
	switch (status)
	{
	case TupleTransactionStatus_None: 				return "None";
	case TupleTransactionStatus_Frozen: 			return "Frozen";
	case TupleTransactionStatus_HintCommitted: 		return "Hint-Committed";
	case TupleTransactionStatus_HintAborted: 		return "Hint-Aborted";
	case TupleTransactionStatus_CLogInProgress: 	return "CLog-In-Progress";
	case TupleTransactionStatus_CLogCommitted: 		return "CLog-Committed";
	case TupleTransactionStatus_CLogAborted:		return "CLog-Aborted";
	case TupleTransactionStatus_CLogSubCommitted:	return "CLog-Sub-Committed";
	default:
		return "Unknown";
	}
}

static char *TupleVisibilityStatus_Name(TupleVisibilityStatus status)
{
	switch (status)
	{
	case TupleVisibilityStatus_Unknown: 	return "Unknown";
	case TupleVisibilityStatus_InProgress: 	return "In-Progress";
	case TupleVisibilityStatus_Aborted: 	return "Aborted";
	case TupleVisibilityStatus_Past: 		return "Past";
	case TupleVisibilityStatus_Now: 		return "Now";
	default:
		return "Unknown";
	}
}

static TupleTransactionStatus GetTupleVisibilityCLogStatus(TransactionId xid)
{
	XidStatus xidStatus;

	xidStatus = TransactionIdGetStatus(xid);
	switch (xidStatus)
	{
	case TRANSACTION_STATUS_IN_PROGRESS:	return TupleTransactionStatus_CLogInProgress;
	case TRANSACTION_STATUS_COMMITTED:		return TupleTransactionStatus_CLogCommitted;
	case TRANSACTION_STATUS_ABORTED:		return TupleTransactionStatus_CLogAborted;
	case TRANSACTION_STATUS_SUB_COMMITTED:	return TupleTransactionStatus_CLogSubCommitted;
	default:
		// Never gets here.  XidStatus is only 2-bits.
		return TupleTransactionStatus_None;
	}
}

void GetTupleVisibilitySummary(
	HeapTuple				tuple,
	TupleVisibilitySummary	*tupleVisibilitySummary)
{
	tupleVisibilitySummary->tid = tuple->t_self;
	tupleVisibilitySummary->infomask = tuple->t_data->t_infomask;
	tupleVisibilitySummary->infomask2 = tuple->t_data->t_infomask2;
	tupleVisibilitySummary->updateTid = tuple->t_data->t_ctid;

	tupleVisibilitySummary->xmin = HeapTupleHeaderGetXmin(tuple->t_data);
	if (!TransactionIdIsNormal(tupleVisibilitySummary->xmin))
	{
		if (tupleVisibilitySummary->xmin == FrozenTransactionId)
		{
			tupleVisibilitySummary->xminStatus = TupleTransactionStatus_Frozen;
		}
		else
		{
			tupleVisibilitySummary->xminStatus = TupleTransactionStatus_None;
		}
	}
	else if (tuple->t_data->t_infomask & HEAP_XMIN_COMMITTED)
	{
		tupleVisibilitySummary->xminStatus = TupleTransactionStatus_HintCommitted;
	}
	else if (tuple->t_data->t_infomask & HEAP_XMIN_INVALID)
	{
		tupleVisibilitySummary->xminStatus = TupleTransactionStatus_HintAborted;
	}
	else
	{
		tupleVisibilitySummary->xminStatus =
					GetTupleVisibilityCLogStatus(tupleVisibilitySummary->xmin);
	}
	tupleVisibilitySummary->xmax = HeapTupleHeaderGetXmax(tuple->t_data);
	if (!TransactionIdIsNormal(tupleVisibilitySummary->xmax))
	{
		if (tupleVisibilitySummary->xmax == FrozenTransactionId)
		{
			tupleVisibilitySummary->xmaxStatus = TupleTransactionStatus_Frozen;
		}
		else
		{
			tupleVisibilitySummary->xmaxStatus = TupleTransactionStatus_None;
		}
	}
	else if (tuple->t_data->t_infomask & HEAP_XMAX_COMMITTED)
	{
		tupleVisibilitySummary->xmaxStatus = TupleTransactionStatus_HintCommitted;
	}
	else if (tuple->t_data->t_infomask & HEAP_XMAX_INVALID)
	{
		tupleVisibilitySummary->xmaxStatus = TupleTransactionStatus_HintAborted;
	}
	else
	{
		tupleVisibilitySummary->xmaxStatus =
					GetTupleVisibilityCLogStatus(tupleVisibilitySummary->xmax);
	}

	tupleVisibilitySummary->cid =
					HeapTupleHeaderGetRawCommandId(tuple->t_data);

	/*
	 * Evaluate xmin and xmax status to produce overall visibility.
	 *
	 * UNDONE: Too simplistic?
	 */
	switch (tupleVisibilitySummary->xminStatus)
	{
	case TupleTransactionStatus_None:
	case TupleTransactionStatus_CLogSubCommitted:
		tupleVisibilitySummary->visibilityStatus = TupleVisibilityStatus_Unknown;
		break;

	case TupleTransactionStatus_CLogInProgress:
		tupleVisibilitySummary->visibilityStatus = TupleVisibilityStatus_InProgress;
		break;

	case TupleTransactionStatus_HintAborted:
	case TupleTransactionStatus_CLogAborted:
		tupleVisibilitySummary->visibilityStatus = TupleVisibilityStatus_Aborted;
		break;

	case TupleTransactionStatus_Frozen:
	case TupleTransactionStatus_HintCommitted:
	case TupleTransactionStatus_CLogCommitted:
		{
			switch (tupleVisibilitySummary->xmaxStatus)
			{
			case TupleTransactionStatus_None:
			case TupleTransactionStatus_Frozen:
			case TupleTransactionStatus_CLogInProgress:
			case TupleTransactionStatus_HintAborted:
			case TupleTransactionStatus_CLogAborted:
				tupleVisibilitySummary->visibilityStatus = TupleVisibilityStatus_Now;
				break;

			case TupleTransactionStatus_CLogSubCommitted:
				tupleVisibilitySummary->visibilityStatus = TupleVisibilityStatus_Unknown;
				break;

			case TupleTransactionStatus_HintCommitted:
			case TupleTransactionStatus_CLogCommitted:
				tupleVisibilitySummary->visibilityStatus = TupleVisibilityStatus_Past;
				break;

			default:
				elog(ERROR, "Unrecognized tuple transaction status: %d",
					 (int) tupleVisibilitySummary->xmaxStatus);
				tupleVisibilitySummary->visibilityStatus = TupleVisibilityStatus_Unknown;
				break;
			}
		}
		break;

	default:
		elog(ERROR, "Unrecognized tuple transaction status: %d",
			 (int) tupleVisibilitySummary->xminStatus);
		tupleVisibilitySummary->visibilityStatus = TupleVisibilityStatus_Unknown;
		break;
	}
}

static void TupleVisibilityAddFlagName(
	StringInfoData *buf,

	int16			rawFlag,

	char			*flagName,

	bool			*atLeastOne)
{
	if (rawFlag != 0)
	{
		if (*atLeastOne)
		{
			appendStringInfo(buf, ", ");
		}
		appendStringInfo(buf, "%s", flagName);
		*atLeastOne = true;
	}
}

static char *GetTupleVisibilityInfoMaskSet(
	int16 				infomask,

	int16				infomask2)
{
	StringInfoData buf;

	bool atLeastOne;

	initStringInfo(&buf);
	appendStringInfo(&buf, "{");
	atLeastOne = false;

	TupleVisibilityAddFlagName(&buf, infomask & HEAP_COMBOCID, "HEAP_COMBOCID", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask & HEAP_XMAX_EXCL_LOCK, "HEAP_XMAX_EXCL_LOCK", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask & HEAP_XMAX_SHARED_LOCK, "HEAP_XMAX_SHARED_LOCK", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask & HEAP_XMIN_COMMITTED, "HEAP_XMIN_COMMITTED", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask & HEAP_XMIN_INVALID, "HEAP_XMIN_INVALID", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask & HEAP_XMAX_COMMITTED, "HEAP_XMAX_COMMITTED", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask & HEAP_XMAX_INVALID, "HEAP_XMAX_INVALID", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask & HEAP_XMAX_IS_MULTI, "HEAP_XMAX_IS_MULTI", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask & HEAP_UPDATED, "HEAP_UPDATED", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask & HEAP_MOVED_OFF, "HEAP_MOVED_OFF", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask & HEAP_MOVED_IN, "HEAP_MOVED_IN", &atLeastOne);

	TupleVisibilityAddFlagName(&buf, infomask2 & HEAP_XMIN_DISTRIBUTED_SNAPSHOT_IGNORE, "HEAP_XMIN_DISTRIBUTED_SNAPSHOT_IGNORE", &atLeastOne);
	TupleVisibilityAddFlagName(&buf, infomask2 & HEAP_XMAX_DISTRIBUTED_SNAPSHOT_IGNORE, "HEAP_XMAX_DISTRIBUTED_SNAPSHOT_IGNORE", &atLeastOne);

	appendStringInfo(&buf, "}");
	return buf.data;
}

// 0  gp_tid                    			TIDOID
// 1  gp_xmin                  			INT4OID
// 2  gp_xmin_status        			TEXTOID
// 3  gp_xmin_commit_distrib_id		TEXTOID
// 4  gp_xmax		          		INT4OID
// 5  gp_xmax_status       			TEXTOID
// 6  gp_xmax_distrib_id    			TEXTOID
// 7  gp_command_id	    			INT4OID
// 8  gp_infomask    	   			TEXTOID
// 9  gp_update_tid         			TIDOID
// 10 gp_visibility             			TEXTOID

void GetTupleVisibilitySummaryDatums(
	Datum		*values,
	bool		*nulls,
	TupleVisibilitySummary	*tupleVisibilitySummary)
{
	char *infoMaskSet;

	values[0] = ItemPointerGetDatum(&tupleVisibilitySummary->tid);
	values[1] = Int32GetDatum((int32)tupleVisibilitySummary->xmin);
	values[2] =
		DirectFunctionCall1(textin,
							CStringGetDatum(
								TupleTransactionStatus_Name(
											tupleVisibilitySummary->xminStatus)));
	nulls[3] = true;
	values[4] = Int32GetDatum((int32)tupleVisibilitySummary->xmax);
	values[5] =
		DirectFunctionCall1(textin,
							CStringGetDatum(
								TupleTransactionStatus_Name(
											tupleVisibilitySummary->xmaxStatus)));
	nulls[6] = true;
	values[7] = Int32GetDatum((int32)tupleVisibilitySummary->cid);
	infoMaskSet = GetTupleVisibilityInfoMaskSet(
									tupleVisibilitySummary->infomask,
									tupleVisibilitySummary->infomask2);
	values[8] =
		DirectFunctionCall1(textin,
							CStringGetDatum(
									infoMaskSet));
	pfree(infoMaskSet);
	values[9] = ItemPointerGetDatum(&tupleVisibilitySummary->updateTid);
	values[10] =
		DirectFunctionCall1(textin,
							CStringGetDatum(
								TupleVisibilityStatus_Name(
											tupleVisibilitySummary->visibilityStatus)));
}

char *GetTupleVisibilitySummaryString(
	TupleVisibilitySummary	*tupleVisibilitySummary)
{
	StringInfoData buf;

	char *infoMaskSet;

	initStringInfo(&buf);
	appendStringInfo(&buf, "tid %s",
					 ItemPointerToString(&tupleVisibilitySummary->tid));
	appendStringInfo(&buf, ", xmin %u",
					 tupleVisibilitySummary->xmin);
	appendStringInfo(&buf, ", xmin_status '%s'",
					 TupleTransactionStatus_Name(
							tupleVisibilitySummary->xminStatus));

	appendStringInfo(&buf, ", xmax %u",
					 tupleVisibilitySummary->xmax);
	appendStringInfo(&buf, ", xmax_status '%s'",
					 TupleTransactionStatus_Name(
							tupleVisibilitySummary->xmaxStatus));

	appendStringInfo(&buf, ", command_id %u",
					 tupleVisibilitySummary->cid);
	infoMaskSet = GetTupleVisibilityInfoMaskSet(
									tupleVisibilitySummary->infomask,
									tupleVisibilitySummary->infomask2);
	appendStringInfo(&buf, ", infomask '%s'",
					 infoMaskSet);
	pfree(infoMaskSet);
	appendStringInfo(&buf, ", update_tid %s",
					 ItemPointerToString(&tupleVisibilitySummary->updateTid));
	appendStringInfo(&buf, ", visibility '%s'",
					 TupleVisibilityStatus_Name(
											tupleVisibilitySummary->visibilityStatus));

	return buf.data;
}

