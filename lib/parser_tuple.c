/*
 * pg_bulkload: lib/parser_tuple.c
 *
 *	  Copyright(C) 2009, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 */

/**
 * @file
 * @brief Binary HeapTuple format handling module implementation.
 */
#include "postgres.h"

#include <unistd.h>

#include "access/htup.h"
#include "utils/rel.h"

#include "reader.h"
#include "pg_profile.h"
#include "pgut/pgut-ipc.h"

typedef struct TupleParser
{
	Parser	base;

	Queue		   *queue;
	HeapTupleData	tuple;
	char		   *buffer;
	uint32			buflen;
} TupleParser;

static void	TupleParserInit(TupleParser *self, const char *infile, Oid relid);
static HeapTuple TupleParserRead(TupleParser *self);
static int64 TupleParserTerm(TupleParser *self);
static bool TupleParserParam(TupleParser *self, const char *keyword, char *value);
static void TupleParserDumpParams(TupleParser *self);
static void TupleParserDumpRecord(TupleParser *self, FILE fp, char *filename);

/**
 * @brief Create a new binary parser.
 */
Parser *
CreateTupleParser(void)
{
	TupleParser *self = palloc0(sizeof(TupleParser));
	self->base.init = (ParserInitProc) TupleParserInit;
	self->base.read = (ParserReadProc) TupleParserRead;
	self->base.term = (ParserTermProc) TupleParserTerm;
	self->base.param = (ParserParamProc) TupleParserParam;
	self->base.dumpParams = (ParserDumpParamsProc) TupleParserDumpParams;
	self->base.dumpRecord = (ParserDumpRecordProc) TupleParserDumpRecord;

	return (Parser *)self;
}

static void
TupleParserInit(TupleParser *self, const char *infile, Oid relid)
{
	unsigned		key;
	char			junk[2];

	if (sscanf(infile, ":%u%1s", &key, junk) != 1)
		elog(ERROR, "invalid shmem key format: %s", infile);

	self->queue = QueueOpen(key);
	self->buflen = BLCKSZ;
	self->buffer = palloc(self->buflen);
}

static int64
TupleParserTerm(TupleParser *self)
{
	if (self->queue)
		QueueClose(self->queue);
	if (self->buffer)
		pfree(self->buffer);
	pfree(self);

	return 0;
}

static HeapTuple
TupleParserRead(TupleParser *self)
{
	uint32		len;

	BULKLOAD_PROFILE(&prof_reader_parser);

	if (QueueRead(self->queue, &len, sizeof(uint32), false) == sizeof(uint32) && len > 0)
	{
		if (self->buflen < len)
		{
			repalloc(self->buffer, len);
			self->buflen = len;
		}
		if (QueueRead(self->queue, self->buffer, len, false) == len)
		{
			BULKLOAD_PROFILE(&prof_reader_source);
			self->tuple.t_len = len;
			self->tuple.t_data = (HeapTupleHeader) self->buffer;
			return &self->tuple;
		}
	}

	return NULL;
}

static bool
TupleParserParam(TupleParser *self, const char *keyword, char *value)
{
	/* TupleParser does not support OFFSET */
	return false;	/* no parameters supported */
}

static void
TupleParserDumpParams(TupleParser *self)
{
	/* no parameters supported */
}

static void
TupleParserDumpRecord(TupleParser *self, FILE fp, char *filename)
{
	/* parse error does not happen in TupleParser. */
}
