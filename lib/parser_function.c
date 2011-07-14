/*
 * pg_bulkload: lib/parser_function.c
 *
 *	  Copyright (c) 2009-2011, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 */

/**
 * @file
 * @brief Binary HeapTuple format handling module implementation.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "logger.h"
#include "pg_profile.h"
#include "pg_strutil.h"
#include "reader.h"
#include "pgut/pgut-be.h"

typedef struct FunctionParser
{
	Parser	base;

	FmgrInfo				flinfo;
	FunctionCallInfoData	fcinfo;
	TupleDesc				desc;
	EState				   *estate;
	ExprContext			   *econtext;
	ExprContext			   *arg_econtext;
	ReturnSetInfo			rsinfo;
	HeapTupleData			tuple;
	TupleTableSlot		   *funcResultSlot;
	bool					tupledesc_matched;
} FunctionParser;

static void	FunctionParserInit(FunctionParser *self, Checker *checker, const char *infile, TupleDesc desc);
static HeapTuple FunctionParserRead(FunctionParser *self, Checker *checker);
static int64	FunctionParserTerm(FunctionParser *self);
static bool FunctionParserParam(FunctionParser *self, const char *keyword, char *value);
static void FunctionParserDumpParams(FunctionParser *self);
static void FunctionParserDumpRecord(FunctionParser *self, FILE *fp, char *bafdile);

/* ========================================================================
 * FunctionParser
 * ========================================================================*/

/**
 * @brief Create a new binary parser.
 */
Parser *
CreateFunctionParser(void)
{
	FunctionParser *self = palloc0(sizeof(FunctionParser));
	self->base.init = (ParserInitProc) FunctionParserInit;
	self->base.read = (ParserReadProc) FunctionParserRead;
	self->base.term = (ParserTermProc) FunctionParserTerm;
	self->base.param = (ParserParamProc) FunctionParserParam;
	self->base.dumpParams = (ParserDumpParamsProc) FunctionParserDumpParams;
	self->base.dumpRecord = (ParserDumpRecordProc) FunctionParserDumpRecord;

	return (Parser *)self;
}

static void
FunctionParserInit(FunctionParser *self, Checker *checker, const char *infile, TupleDesc desc)
{
	int					i;
	ParsedFunction		function;
	int					nargs;
	Oid					funcid;
	HeapTuple			ftup;
	Form_pg_proc		pp;

	if (pg_strcasecmp(infile, "stdin") == 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("cannot load from STDIN in the case of \"TYPE = FUNCTION\"")));

	if (checker->encoding != -1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("does not support parameter \"ENCODING\" in \"TYPE = FUNCTION\"")));

	function = ParseFunction(infile, false);

	funcid = function.oid;
	fmgr_info(funcid, &self->flinfo);

	if (!self->flinfo.fn_retset)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function must return set")));

	ftup = SearchSysCache(PROCOID, ObjectIdGetDatum(funcid), 0, 0, 0);
	pp = (Form_pg_proc) GETSTRUCT(ftup);

	/* Check data type of the function result value */
	if (pp->prorettype == desc->tdtypeid)
	{
		/*
		 * If the return value of the input function is a target table,
		 * lookup_rowtype_tupdesc grab AccessShareLock on the table in the
		 * first call.  We call lookup_rowtype_tupdesc here to avoid deadlock
		 * when lookup_rowtype_tupdesc is called by the internal routine of the
		 * input function, because a parallel writer process holds an
		 * AccessExclusiveLock.
		 */
		TupleDesc	resultDesc = lookup_rowtype_tupdesc(pp->prorettype, -1);
		ReleaseTupleDesc(resultDesc);

		self->tupledesc_matched = true;
	}
	else if (pp->prorettype == RECORDOID)
	{
		TupleDesc	resultDesc = NULL;

		/* Check for OUT parameters defining a RECORD result */
		resultDesc = build_function_result_tupdesc_t(ftup);

		if (resultDesc)
		{
			tupledesc_match(desc, resultDesc);
			self->tupledesc_matched = true;
			FreeTupleDesc(resultDesc);
		}
	}
	else if (get_typtype(pp->prorettype) != TYPTYPE_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function return data type and target table data type do not match")));

	/*
	 * assign arguments
	 */
	nargs = function.nargs;
	for (i = 0;
#if PG_VERSION_NUM >= 80400
		i < nargs - function.nvargs;
#else
		i < nargs;
#endif
		++i)
	{
		if (function.args[i] == NULL)
		{
			if (self->flinfo.fn_strict)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("function is strict, but argument %d is NULL", i)));
			self->fcinfo.argnull[i] = true;
		}
		else
		{
			Oid			typinput;
			Oid			typioparam;

			getTypeInputInfo(pp->proargtypes.values[i], &typinput, &typioparam);
			self->fcinfo.arg[i] = OidInputFunctionCall(typinput,
									(char *) function.args[i], typioparam, -1);
			self->fcinfo.argnull[i] = false;
			pfree(function.args[i]);
		}
	}

	/*
	 * assign variadic arguments
	 */
#if PG_VERSION_NUM >= 80400
	if (function.nvargs > 0)
	{
		int			nfixedarg;
		Oid			func;
		Oid			element_type;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		char		elmdelim;
		Oid			elmioparam;
		Datum	   *elems;
		bool	   *nulls;
		int			dims[1];
		int			lbs[1];
		ArrayType  *arry;

		nfixedarg = i;
		element_type = pp->provariadic;

		/*
		 * Get info about element type, including its input conversion proc
		 */
		get_type_io_data(element_type, IOFunc_input,
						 &elmlen, &elmbyval, &elmalign, &elmdelim,
						 &elmioparam, &func);

		elems = (Datum *) palloc(function.nvargs * sizeof(Datum));
		nulls = (bool *) palloc0(function.nvargs * sizeof(bool));
		for (i = 0; i < function.nvargs; i++)
		{
			if (function.args[nfixedarg + i] == NULL)
				nulls[i] = true;
			else
			{
				elems[i] = OidInputFunctionCall(func,
								(char *) function.args[nfixedarg + i], elmioparam, -1);
				pfree(function.args[nfixedarg + i]);
			}
		}

		dims[0] = function.nvargs;
		lbs[0] = 1;
		arry = construct_md_array(elems, nulls, 1, dims, lbs, element_type,
								  elmlen, elmbyval, elmalign);
		self->fcinfo.arg[nfixedarg] = PointerGetDatum(arry);
	}

	/*
	 * assign default arguments
	 */
	if (function.ndargs > 0)
	{
		Datum		proargdefaults;
		bool		isnull;
		char	   *str;
		List	   *defaults;
		int			ndelete;
		ListCell   *l;

		/* shouldn't happen, FuncnameGetCandidates messed up */
		if (function.ndargs > pp->pronargdefaults)
			elog(ERROR, "not enough default arguments");

		proargdefaults = SysCacheGetAttr(PROCOID, ftup,
										 Anum_pg_proc_proargdefaults,
										 &isnull);
		Assert(!isnull);
		str = TextDatumGetCString(proargdefaults);
		defaults = (List *) stringToNode(str);
		Assert(IsA(defaults, List));
		pfree(str);
		/* Delete any unused defaults from the returned list */
		ndelete = list_length(defaults) - function.ndargs;
		while (ndelete-- > 0)
			defaults = list_delete_first(defaults);

		self->arg_econtext = CreateStandaloneExprContext();
		foreach(l, defaults)
		{
			Expr	   *expr = (Expr *) lfirst(l);
			ExprState  *argstate;
			ExprDoneCond thisArgIsDone;

			/* probably shouldn't happen ... */
			if (nargs >= FUNC_MAX_ARGS)
				ereport(ERROR,
						(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("cannot pass more than %d arguments to a function", FUNC_MAX_ARGS)));

			argstate = ExecInitExpr(expr, NULL);

			self->fcinfo.arg[nargs] = ExecEvalExpr(argstate,
												   self->arg_econtext,
												   &self->fcinfo.argnull[nargs],
												   &thisArgIsDone);

			if (thisArgIsDone != ExprSingleResult)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("functions and operators can take at most one set argument")));

			nargs++;
		}
	}
#endif

	ReleaseSysCache(ftup);

	InitFunctionCallInfoData(self->fcinfo, &self->flinfo, nargs, NULL,
							 (Node *) &self->rsinfo);

	self->desc = CreateTupleDescCopy(desc);
	for (i = 0; i < desc->natts; i++)
		self->desc->attrs[i]->attnotnull = desc->attrs[i]->attnotnull;

	self->estate = CreateExecutorState();
	self->econtext = GetPerTupleExprContext(self->estate);
	self->rsinfo.type = T_ReturnSetInfo;
	self->rsinfo.econtext = self->econtext;
	self->rsinfo.expectedDesc = self->desc;
	self->rsinfo.allowedModes = (int) (SFRM_ValuePerCall | SFRM_Materialize);
	self->rsinfo.returnMode = SFRM_ValuePerCall;
	self->rsinfo.isDone = ExprSingleResult;
	self->rsinfo.setResult = NULL;
	self->rsinfo.setDesc = NULL;
	self->funcResultSlot = MakeSingleTupleTableSlot(self->desc);
}

static int64
FunctionParserTerm(FunctionParser *self)
{
	if (self->funcResultSlot)
		ExecClearTuple(self->funcResultSlot);
	if (self->rsinfo.setResult)
		tuplestore_end(self->rsinfo.setResult);
	if (self->arg_econtext)
		FreeExprContext(self->arg_econtext, true);
	if (self->econtext)
		FreeExprContext(self->econtext, true);
	if (self->estate)
		FreeExecutorState(self->estate);
	pfree(self);

	return 0;
}

static void
set_datum_tuple(FunctionParser *self, Datum datum)
{
	HeapTupleHeader	td = DatumGetHeapTupleHeader(datum);

	if (!self->tupledesc_matched)
	{
		/*
		 * We must not call lookup_rowtype_tupdesc, because parallel writer
		 * process a deadlock could occur, when typeid same target table's row
		 * type.
		 */
		if (self->desc->tdtypeid != HeapTupleHeaderGetTypeId(td))
		{
			TupleDesc		resultDesc;

			resultDesc = lookup_rowtype_tupdesc(HeapTupleHeaderGetTypeId(td),
												HeapTupleHeaderGetTypMod(td));
			tupledesc_match(self->desc, resultDesc);
			ReleaseTupleDesc(resultDesc);
		}

		self->tupledesc_matched = true;
	}

	self->tuple.t_data = td;
	self->tuple.t_len = HeapTupleHeaderGetDatumLength(td);
	self->base.count++;

	self->base.parsing_field = -1;
}

static HeapTuple
FunctionParserRead(FunctionParser *self, Checker *checker)
{
	Datum		datum;

#if PG_VERSION_NUM >= 80400
	PgStat_FunctionCallUsage	fcusage;
#endif

	/*
	 * If a previous call of the function returned a set result in the form of
	 * a tuplestore, continue reading rows from the tuplestore until it's
	 * empty.
	 */
	if (self->rsinfo.setResult)
	{
		BULKLOAD_PROFILE(&prof_reader_source);

restart:

		/*
		 * Get the next tuple from tuplestore. Return NULL if no more tuples.
		 */
		if (!tuplestore_gettupleslot(self->rsinfo.setResult, true, false,
									 self->funcResultSlot))
			return NULL;

		datum = ExecFetchSlotTupleDatum(self->funcResultSlot);
		set_datum_tuple(self, datum);

		return &self->tuple;
	}

	BULKLOAD_PROFILE(&prof_reader_parser);

	pgstat_init_function_usage(&self->fcinfo, &fcusage);

	self->fcinfo.isnull = false;
	self->rsinfo.isDone = ExprSingleResult;
	datum = FunctionCallInvoke(&self->fcinfo);

	pgstat_end_function_usage(&fcusage,
							  self->rsinfo.isDone != ExprMultipleResult);

	BULKLOAD_PROFILE(&prof_reader_source);

	/* Which protocol does function want to use? */
	if (self->rsinfo.returnMode == SFRM_ValuePerCall)
	{
		/*
		 * Check for end of result set.
		 */
		if (self->rsinfo.isDone == ExprEndResult)
			return NULL;

		/*
		 * For a function returning set, we consider this a protocol violation.
		 */
		if (self->fcinfo.isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("function returning set of rows cannot return null value")));

		set_datum_tuple(self, datum);
	}
	else if (self->rsinfo.returnMode == SFRM_Materialize)
	{
		/* check we're on the same page as the function author */
		if (self->rsinfo.isDone != ExprSingleResult)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
					 errmsg("table-function protocol for materialize mode was not followed")));

		if (self->rsinfo.setResult == NULL)
			return NULL;

		/* back to top to start returning from tuplestore */
		goto restart;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED),
				 errmsg("unrecognized table-function returnMode: %d",
						(int) self->rsinfo.returnMode)));

	return &self->tuple;
}

static bool
FunctionParserParam(FunctionParser *self, const char *keyword, char *value)
{
	/* FunctionParser does not support OFFSET */
	return false;	/* no parameters supported */
}

static void
FunctionParserDumpParams(FunctionParser *self)
{
	LoggerLog(INFO, "TYPE = FUNCTION\n");
	/* no parameters supported */
}

static void
FunctionParserDumpRecord(FunctionParser *self, FILE *fp, char *badfile)
{
	char   *str;

	str = tuple_to_cstring(self->desc, &self->tuple);
	if (fprintf(fp, "%s\n", str) < 0 || fflush(fp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write parse badfile \"%s\": %m",
						badfile)));

	pfree(str);
}
