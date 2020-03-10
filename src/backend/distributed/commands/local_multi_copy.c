/*-------------------------------------------------------------------------
 *
 * local_multi_copy.c
 *    Commands for running a copy locally
 *
 * For each local placement, we have a buffer. When we receive a slot
 * from a copy, the slot will be put to the corresponding buffer based
 * on the shard id. When the buffer size exceeds the threshold a local
 * copy will be done. Also If we reach to the end of copy, we will send
 * the current buffer for local copy.
 *
 * The existing logic from multi_copy.c and format are used, therefore
 * even if user did not do a copy with binary format, it is possible that
 * we are going to be using binary format internally.
 *
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "commands/copy.h"
#include "catalog/namespace.h"
#include "parser/parse_relation.h"
#include "utils/lsyscache.h"
#include "nodes/makefuncs.h"
#include "safe_lib.h"
#include <netinet/in.h> /* for htons */

#include "distributed/transmit.h"
#include "distributed/commands/multi_copy.h"
#include "distributed/multi_partitioning_utils.h"
#include "distributed/local_executor.h"
#include "distributed/local_multi_copy.h"
#include "distributed/shard_utils.h"

static int ReadFromLocalBufferCallback(void *outbuf, int minread, int maxread);
static void AddSlotToBuffer(TupleTableSlot *slot, CitusCopyDestReceiver *copyDest,
							CopyOutState localCopyOutState);

static bool ShouldSendCopyNow(StringInfo buffer);
static void DoLocalCopy(StringInfo buffer, Oid relationId, int64 shardId,
						CopyStmt *copyStatement, bool isEndOfCopy);
static bool ShouldAddBinaryHeaders(StringInfo buffer, bool isBinary);

/*
 * LocalCopyBuffer is used in copy callback to return the copied rows.
 * The reason this is a global variable is that we cannot pass an additional
 * argument to the copy callback.
 */
static StringInfo LocalCopyBuffer;

/*
 * WriteTupleToLocalShard adds the given slot and does a local copy if
 * this is the end of copy, or the buffer size exceeds the threshold.
 */
void
WriteTupleToLocalShard(TupleTableSlot *slot, CitusCopyDestReceiver *copyDest, int64
					   shardId,
					   CopyOutState localCopyOutState)
{
	/* since we are doing a local copy, the following statements should use local execution to see the changes */
	TransactionAccessedLocalPlacement = true;

	bool isBinaryCopy = localCopyOutState->binary;
	if (ShouldAddBinaryHeaders(localCopyOutState->fe_msgbuf, isBinaryCopy))
	{
		AppendCopyBinaryHeaders(localCopyOutState);
	}

	AddSlotToBuffer(slot, copyDest, localCopyOutState);

	if (ShouldSendCopyNow(localCopyOutState->fe_msgbuf))
	{
		if (isBinaryCopy)
		{
			AppendCopyBinaryFooters(localCopyOutState);
		}
		bool isEndOfCopy = false;
		DoLocalCopy(localCopyOutState->fe_msgbuf, copyDest->distributedRelationId,
					shardId,
					copyDest->copyStatement, isEndOfCopy);
	}
}


/*
 * FinishLocalCopyToShard finishes local copy for the given shard with the shard id.
 */
void
FinishLocalCopyToShard(CitusCopyDestReceiver *copyDest, int64 shardId,
					   CopyOutState localCopyOutState)
{
	bool isBinaryCopy = localCopyOutState->binary;
	if (isBinaryCopy)
	{
		AppendCopyBinaryFooters(localCopyOutState);
	}
	bool isEndOfCopy = true;
	DoLocalCopy(localCopyOutState->fe_msgbuf, copyDest->distributedRelationId, shardId,
				copyDest->copyStatement, isEndOfCopy);
}


/*
 * AddSlotToBuffer serializes the given slot and adds it to the buffer in localCopyOutState.
 */
static void
AddSlotToBuffer(TupleTableSlot *slot, CitusCopyDestReceiver *copyDest, CopyOutState
				localCopyOutState)
{
	Datum *columnValues = slot->tts_values;
	bool *columnNulls = slot->tts_isnull;
	FmgrInfo *columnOutputFunctions = copyDest->columnOutputFunctions;
	CopyCoercionData *columnCoercionPaths = copyDest->columnCoercionPaths;

	AppendCopyRowData(columnValues, columnNulls, copyDest->tupleDescriptor,
					  localCopyOutState, columnOutputFunctions,
					  columnCoercionPaths);
}


/*
 * ShouldSendCopyNow returns true if the given buffer size exceeds the
 * local copy buffer size threshold.
 */
static bool
ShouldSendCopyNow(StringInfo buffer)
{
	return buffer->len > LOCAL_COPY_FLUSH_THRESHOLD;
}


/*
 * DoLocalCopy finds the shard table from the distributed relation id, and copies the given
 * buffer into the shard.
 */
static void
DoLocalCopy(StringInfo buffer, Oid relationId, int64 shardId, CopyStmt *copyStatement,
			bool isEndOfCopy)
{
	LocalCopyBuffer = buffer;

	Oid shardOid = GetShardLocalTableOid(relationId, shardId);
	Relation shard = heap_open(shardOid, RowExclusiveLock);
	ParseState *pState = make_parsestate(NULL);

	/* p_rtable of pState is set so that we can check constraints. */
	pState->p_rtable = CreateRangeTable(shard, ACL_INSERT);

	CopyState cstate = BeginCopyFrom(pState, shard, NULL, false,
									 ReadFromLocalBufferCallback,
									 copyStatement->attlist, copyStatement->options);
	CopyFrom(cstate);
	EndCopyFrom(cstate);

	heap_close(shard, NoLock);
	free_parsestate(pState);
	FreeStringInfo(buffer);
	if (!isEndOfCopy)
	{
		buffer = makeStringInfo();
	}
}


/*
 * ShouldAddBinaryHeaders returns true if the given buffer
 * is empty and the format is binary.
 */
static bool
ShouldAddBinaryHeaders(StringInfo buffer, bool isBinary)
{
	if (!isBinary)
	{
		return false;
	}
	return buffer->len == 0;
}


/*
 * ReadFromLocalBufferCallback is the copy callback.
 * It always tries to copy maxread bytes.
 */
static int
ReadFromLocalBufferCallback(void *outbuf, int minread, int maxread)
{
	int bytesread = 0;
	int avail = LocalCopyBuffer->len - LocalCopyBuffer->cursor;
	int bytesToRead = Min(avail, maxread);
	if (bytesToRead > 0)
	{
		memcpy_s(outbuf, bytesToRead + strlen((char *) outbuf),
				 &LocalCopyBuffer->data[LocalCopyBuffer->cursor], bytesToRead);
	}
	bytesread += bytesToRead;
	LocalCopyBuffer->cursor += bytesToRead;

	return bytesread;
}
