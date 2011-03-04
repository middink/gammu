/**
 * ODBC database backend
 *
 * Part of Gammu project
 *
 * Copyright (C) 2011 Michal Čihař
 *
 * Licensed under GNU GPL version 2 or later
 */

#include <gammu.h>

#ifdef WIN32
#include <windows.h>
#ifndef __GNUC__
#pragma comment(lib, "libodbc32.lib")
#endif
#endif

#include <stdio.h>
#include <sql.h>
#include <sqlext.h>

#include "../core.h"
#include "sql.h"
#include "sql-core.h"

static void SMSDODBC_LogError(GSM_SMSDConfig * Config, SQLSMALLINT handle_type, SQLHANDLE handle, const char *message)
{
	SQLINTEGER	 i = 0;
	SQLINTEGER	 native;
	SQLCHAR	 state[ 7 ];
	SQLCHAR	 text[256];
	SQLSMALLINT	 len;
	SQLRETURN	 ret;

	SMSD_Log(DEBUG_ERROR, Config, "%s, ODBC diagnostics:", message);

	do {
		ret = SQLGetDiagRec(handle_type, handle, ++i, state, &native, text, sizeof(text), &len );
		if (SQL_SUCCEEDED(ret)) {
			SMSD_Log(DEBUG_ERROR, Config, "%s:%ld:%ld:%s\n", state, (long)i, (long)native, text);
		}
	} while (ret == SQL_SUCCESS);
}

long long SMSDODBC_GetNumber(GSM_SMSDConfig * Config, SQL_result *res, unsigned int field)
{
	SQLRETURN ret;
	SQLINTEGER value;

	ret = SQLGetData(res->odbc, field + 1, SQL_C_SLONG, &value, 0, NULL);
	if (!SQL_SUCCEEDED(ret)) {
		SMSDODBC_LogError(Config, SQL_HANDLE_STMT, res->odbc, "SQLGetData(long) failed");
		return -1;
	}
	return value;
}

time_t SMSDODBC_GetDate(GSM_SMSDConfig * Config, SQL_result *res, unsigned int field)
{
	struct tm timestruct;
	SQL_TIMESTAMP_STRUCT sqltime;
	SQLRETURN ret;

	ret = SQLGetData(res->odbc, field + 1, SQL_C_TYPE_TIMESTAMP, &sqltime, 0, NULL);
	if (!SQL_SUCCEEDED(ret)) {
		SMSDODBC_LogError(Config, SQL_HANDLE_STMT, res->odbc, "SQLGetData(timestamp) failed");
		return -1;
	}

	tzset();

#ifdef HAVE_DAYLIGHT
	timestruct.tm_isdst	= daylight;
#else
	timestruct.tm_isdst	= -1;
#endif
#ifdef HAVE_STRUCT_TM_TM_ZONE
	/* No sqltime zone information */
	timestruct.tm_gmtoff = timezone;
	timestruct.tm_zone = *tzname;
#endif

	timestruct.tm_year = sqltime.year;
	timestruct.tm_mon = sqltime.month;
	timestruct.tm_mday = sqltime.day;
	timestruct.tm_hour = sqltime.hour;
	timestruct.tm_min = sqltime.minute;
	timestruct.tm_sec = sqltime.second;

	return mktime(&timestruct);
}

const char *SMSDODBC_GetString(GSM_SMSDConfig * Config, SQL_result *res, unsigned int field)
{
	SQLLEN size;
	SQLRETURN ret;

	if (field > SMSD_ODBC_MAX_RETURN_STRINGS) {
		SMSD_Log(DEBUG_ERROR, Config, "Field %d returning NULL, too many fields!", field);
		return NULL;
	}

	/* Figure out string length */
	ret = SQLGetData(res->odbc, field + 1, SQL_C_CHAR, NULL, 0, &size);
	if (!SQL_SUCCEEDED(ret)) {
		SMSDODBC_LogError(Config, SQL_HANDLE_STMT, res->odbc, "SQLGetData(string,NULL) failed");
		return NULL;
	}

	/* Did not we get NULL? */
	if ((long)size == (long)SQL_NULL_DATA) {
		SMSD_Log(DEBUG_INFO, Config, "Field %d returning NULL", field);
		return NULL;
	}

	/* Allocate string */
	if (Config->conn.odbc.retstr[field] == NULL) {
		Config->conn.odbc.retstr[field] = malloc(size + 1);
	} else {
		Config->conn.odbc.retstr[field] = realloc(Config->conn.odbc.retstr[field], size + 1);
	}
	if (Config->conn.odbc.retstr[field] == NULL) {
		SMSD_Log(DEBUG_ERROR, Config, "Field %d returning NULL, failed to allocate %ld bytes of memory", field, (long)size + 1);
		return NULL;
	}

	/* Actually grab result from database */
	ret = SQLGetData(res->odbc, field + 1, SQL_C_CHAR, Config->conn.odbc.retstr[field], size + 1, &size);
	if (!SQL_SUCCEEDED(ret)) {
		SMSDODBC_LogError(Config, SQL_HANDLE_STMT, res->odbc, "SQLGetData(string) failed");
		return NULL;
	}

	SMSD_Log(DEBUG_INFO, Config, "Field %d returning string \"%s\"", field, Config->conn.odbc.retstr[field]);

	return Config->conn.odbc.retstr[field];
}

gboolean SMSDODBC_GetBool(GSM_SMSDConfig * Config, SQL_result *res, unsigned int field)
{
	long long intval;
	const char * charval;

	/* Try to get numeric value first */
	intval = SMSDODBC_GetNumber(Config, res, field);
	if (intval == -1) {
		/* If that fails, fall back to string and parse it */
		charval = SMSDODBC_GetString(Config, res, field);
		return GSM_StringToBool(charval);
	}
	return intval ? TRUE : FALSE;
}

/* Disconnects from a database */
void SMSDODBC_Free(GSM_SMSDConfig * Config)
{
	int field;

	SQLDisconnect(Config->conn.odbc.dbc);
	SQLFreeHandle(SQL_HANDLE_ENV, Config->conn.odbc.env);

	for (field = 0; field < SMSD_ODBC_MAX_RETURN_STRINGS; field++) {
		if (Config->conn.odbc.retstr[field] != NULL) {
			free(Config->conn.odbc.retstr[field]);
			Config->conn.odbc.retstr[field] = NULL;
		}
	}
}

/* Connects to database */
static SQL_Error SMSDODBC_Connect(GSM_SMSDConfig * Config)
{
	SQLRETURN ret;
	int field;

	for (field = 0; field < SMSD_ODBC_MAX_RETURN_STRINGS; field++) {
		Config->conn.odbc.retstr[field] = NULL;
	}

	ret = SQLAllocHandle (SQL_HANDLE_ENV, SQL_NULL_HANDLE, &Config->conn.odbc.env);
	if (!SQL_SUCCEEDED(ret)) {
		SMSDODBC_LogError(Config, SQL_HANDLE_ENV, Config->conn.odbc.env, "SQLAllocHandle(ENV) failed");
		return SQL_FAIL;
	}

	ret = SQLSetEnvAttr (Config->conn.odbc.env, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
	if (!SQL_SUCCEEDED(ret)) {
		SMSDODBC_LogError(Config, SQL_HANDLE_ENV, Config->conn.odbc.env, "SQLSetEnvAttr failed");
		return SQL_FAIL;
	}

	ret = SQLAllocHandle (SQL_HANDLE_DBC, Config->conn.odbc.env, &Config->conn.odbc.dbc);
	if (!SQL_SUCCEEDED(ret)) {
		SMSDODBC_LogError(Config, SQL_HANDLE_ENV, Config->conn.odbc.env, "SQLAllocHandle(DBC) failed");
		return SQL_FAIL;
	}

	ret = SQLConnect(Config->conn.odbc.dbc,
			  (SQLCHAR*)Config->host, SQL_NTS,
			  (SQLCHAR*)Config->user, SQL_NTS,
			  (SQLCHAR*)Config->password, SQL_NTS);
	if (!SQL_SUCCEEDED(ret)) {
		SMSDODBC_LogError(Config, SQL_HANDLE_DBC, Config->conn.odbc.dbc, "SQLConnect failed");
		return SQL_FAIL;
	}

	return SQL_OK;
}

static SQL_Error SMSDODBC_Query(GSM_SMSDConfig * Config, const char *query, SQL_result * res)
{
	SQLRETURN ret;

	ret = SQLAllocHandle(SQL_HANDLE_STMT, Config->conn.odbc.dbc, &res->odbc);
	if (!SQL_SUCCEEDED(ret)) {
		return SQL_FAIL;
	}

	ret = SQLExecDirect (res->odbc, (SQLCHAR*)query, SQL_NTS);
	if (SQL_SUCCEEDED(ret)) {
		return SQL_OK;
	}

	SMSDODBC_LogError(Config, SQL_HANDLE_STMT, res->odbc, "SQLExecDirect failed");
	return SQL_FAIL;
}

/* free sql results */
void SMSDODBC_FreeResult(GSM_SMSDConfig * Config, SQL_result *res)
{
	SQLFreeHandle (SQL_HANDLE_STMT, res->odbc);
}

/* set pointer to next row */
int SMSDODBC_NextRow(GSM_SMSDConfig * Config, SQL_result *res)
{
	SQLRETURN ret;

	ret = SQLFetch(res->odbc);

	if (!SQL_SUCCEEDED(ret)) {
		if (ret != SQL_NO_DATA) {
			SMSDODBC_LogError(Config, SQL_HANDLE_STMT, res->odbc, "SQLFetch failed");
		}
		return 0;
	}
	return 1;
}

/* quote strings */
char * SMSDODBC_QuoteString(GSM_SMSDConfig * Config, const char *string)
{
	char *encoded_text = NULL;
	size_t i, len, pos = 0;

	len = strlen(string);

	encoded_text = (char *)malloc((len * 2) + 3);
	encoded_text[pos++] = '"';
	for (i = 0; i < len; i++) {
		if (string[i] == '"' || string[i] == '\\') {
			encoded_text[pos++] = '\\';
		}
		encoded_text[pos++] = string[i];
	}
	encoded_text[pos++] = '"';
	encoded_text[pos] = '\0';
	return encoded_text;
}

/* LAST_INSERT_ID */
unsigned long long SMSDODBC_SeqID(GSM_SMSDConfig * Config, const char *id)
{
	SQLRETURN ret;
	SQLHSTMT stmt;
	SQLINTEGER value;

	ret = SQLAllocHandle(SQL_HANDLE_STMT, Config->conn.odbc.dbc, &stmt);
	if (!SQL_SUCCEEDED(ret)) {
		return 0;
	}

	ret = SQLExecDirect (stmt, (SQLCHAR*)"SELECT @@IDENTITY", SQL_NTS);
	if (!SQL_SUCCEEDED(ret)) {
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		return 0;
	}

	ret = SQLFetch(stmt);
	if (!SQL_SUCCEEDED(ret)) {
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		return 0;
	}

	ret = SQLGetData(stmt, 1, SQL_C_SLONG, &value, 0, NULL);
	if (!SQL_SUCCEEDED(ret)) {
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		return 0;
	}
	SQLFreeHandle (SQL_HANDLE_STMT, stmt);

	return value;
}

unsigned long SMSDODBC_AffectedRows(GSM_SMSDConfig * Config, SQL_result *res)
{
	SQLRETURN ret;
	SQLLEN count;

	ret = SQLRowCount (res->odbc, &count);
	if (!SQL_SUCCEEDED(ret)) {
		SMSDODBC_LogError(Config, SQL_HANDLE_DBC, Config->conn.odbc.dbc, "SQLRowCount failed");
		return 0;
	}
	return count;
}

struct GSM_SMSDdbobj SMSDODBC = {
	SMSDODBC_Connect,
	SMSDODBC_Query,
	SMSDODBC_Free,
	SMSDODBC_FreeResult,
	SMSDODBC_NextRow,
	SMSDODBC_SeqID,
	SMSDODBC_AffectedRows,
	SMSDODBC_GetString,
	SMSDODBC_GetNumber,
	SMSDODBC_GetDate,
	SMSDODBC_GetBool,
	SMSDODBC_QuoteString,
};

/* How should editor hadle tabs in this file? Add editor commands here.
 * vim: noexpandtab sw=8 ts=8 sts=8:
 */
