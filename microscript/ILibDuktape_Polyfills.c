/*
Copyright 2006 - 2018 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "duktape.h"
#include "ILibDuktape_Helpers.h"
#include "ILibDuktapeModSearch.h"
#include "ILibDuktape_DuplexStream.h"
#include "ILibDuktape_EventEmitter.h"
#include "ILibDuktape_Debugger.h"
#include "../microstack/ILibParsers.h"
#include "../microstack/ILibCrypto.h"
#include "../microstack/ILibRemoteLogging.h"


#define ILibDuktape_Timer_Ptrs					"\xFF_DuktapeTimer_PTRS"
#define ILibDuktape_Queue_Ptr					"\xFF_Queue"
#define ILibDuktape_Stream_Buffer				"\xFF_BUFFER"
#define ILibDuktape_Stream_ReadablePtr			"\xFF_ReadablePtr"
#define ILibDuktape_Stream_WritablePtr			"\xFF_WritablePtr"
#define ILibDuktape_Console_Destination			"\xFF_Console_Destination"
#define ILibDuktape_Console_LOG_Destination		"\xFF_Console_Destination"
#define ILibDuktape_Console_WARN_Destination	"\xFF_Console_WARN_Destination"
#define ILibDuktape_Console_ERROR_Destination	"\xFF_Console_ERROR_Destination"
#define ILibDuktape_Console_INFO_Level			"\xFF_Console_INFO_Level"
#define ILibDuktape_Console_SessionID			"\xFF_Console_SessionID"

#define ILibDuktape_DescriptorEvents_Table		"\xFF_DescriptorEvents_Table"
#define ILibDuktape_DescriptorEvents_FD			"\xFF_DescriptorEvents_FD"
#define ILibDuktape_DescriptorEvents_Options	"\xFF_DescriptorEvents_Options"

typedef enum ILibDuktape_Console_DestinationFlags
{
	ILibDuktape_Console_DestinationFlags_DISABLED		= 0,
	ILibDuktape_Console_DestinationFlags_StdOut			= 1,
	ILibDuktape_Console_DestinationFlags_ServerConsole	= 2,
	ILibDuktape_Console_DestinationFlags_WebLog			= 4,
	ILibDuktape_Console_DestinationFlags_LogFile		= 8
}ILibDuktape_Console_DestinationFlags;

int g_displayStreamPipeMessages = 0;
int g_displayFinalizerMessages = 0;

duk_ret_t ILibDuktape_Pollyfills_Buffer_slice(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	char *buffer;
	char *out;
	duk_size_t bufferLen;
	int offset = 0;
	duk_push_this(ctx);

	buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);
	if (nargs >= 1)
	{
		offset = duk_require_int(ctx, 0);
		bufferLen -= offset;
	}
	if (nargs == 2)
	{
		bufferLen = (duk_size_t)duk_require_int(ctx, 1) - offset;
	}
	duk_push_fixed_buffer(ctx, bufferLen);
	out = Duktape_GetBuffer(ctx, -1, NULL);
	memcpy_s(out, bufferLen, buffer + offset, bufferLen);
	return 1;
}
duk_ret_t ILibDuktape_Polyfills_Buffer_toString(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	char *buffer, *tmpBuffer;
	duk_size_t bufferLen = 0;
	char *cType;

	duk_push_this(ctx);									// [buffer]
	buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);

	if (nargs == 0)
	{
		if (bufferLen == 0 || buffer == NULL)
		{
			duk_push_null(ctx);
		}
		else
		{
			// Just convert to a string
			duk_push_lstring(ctx, buffer, strnlen_s(buffer, bufferLen));			// [buffer][string]
		}
	}
	else
	{
		cType = (char*)duk_require_string(ctx, 0);
		if (strcmp(cType, "base64") == 0)
		{
			duk_push_fixed_buffer(ctx, ILibBase64EncodeLength((int)bufferLen));
			tmpBuffer = Duktape_GetBuffer(ctx, -1, NULL);
			ILibBase64Encode((unsigned char*)buffer, (int)bufferLen, (unsigned char**)&tmpBuffer);
			duk_push_string(ctx, tmpBuffer);
		}
		else if (strcmp(cType, "hex") == 0)
		{
			duk_push_fixed_buffer(ctx, 1 + (bufferLen * 2));
			tmpBuffer = Duktape_GetBuffer(ctx, -1, NULL);
			util_tohex(buffer, (int)bufferLen, tmpBuffer);
			duk_push_string(ctx, tmpBuffer);
		}
		else if (strcmp(cType, "hex:") == 0)
		{
			duk_push_fixed_buffer(ctx, 1 + (bufferLen * 3));
			tmpBuffer = Duktape_GetBuffer(ctx, -1, NULL);
			util_tohex2(buffer, (int)bufferLen, tmpBuffer);
			duk_push_string(ctx, tmpBuffer);
		}
		else
		{
			return(ILibDuktape_Error(ctx, "Unrecognized parameter"));
		}
	}
	return 1;
}
duk_ret_t ILibDuktape_Polyfills_Buffer_from(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	char *str;
	duk_size_t strlength;
	char *encoding;
	char *buffer;
	int bufferLen;

	if (nargs == 1)
	{
		str = (char*)duk_get_lstring(ctx, 0, &strlength);
		buffer = duk_push_fixed_buffer(ctx, strlength);
		memcpy_s(buffer, strlength, str, strlength);
		duk_push_buffer_object(ctx, -1, 0, strlength, DUK_BUFOBJ_NODEJS_BUFFER);
		return(1);
	}
	else if(!(nargs == 2 && duk_is_string(ctx, 0) && duk_is_string(ctx, 1)))
	{
		return(ILibDuktape_Error(ctx, "usage not supported yet"));
	}

	str = (char*)duk_get_lstring(ctx, 0, &strlength);
	encoding = (char*)duk_require_string(ctx, 1);

	if (strcmp(encoding, "base64") == 0)
	{
		// Base64		
		buffer = duk_push_fixed_buffer(ctx, ILibBase64DecodeLength((int)strlength));
		bufferLen = ILibBase64Decode((unsigned char*)str, (int)strlength, (unsigned char**)&buffer);
		duk_push_buffer_object(ctx, -1, 0, bufferLen, DUK_BUFOBJ_NODEJS_BUFFER);
	}
	else if (strcmp(encoding, "hex") == 0)
	{		
		if (ILibString_StartsWith(str, (int)strlength, "0x", 2) != 0)
		{
			str += 2;
			strlength -= 2;
		}
		buffer = duk_push_fixed_buffer(ctx, strlength / 2);
		bufferLen = util_hexToBuf(str, (int)strlength, buffer);
		duk_push_buffer_object(ctx, -1, 0, bufferLen, DUK_BUFOBJ_NODEJS_BUFFER);
	}
	else
	{
		return(ILibDuktape_Error(ctx, "unsupported encoding"));
	}
	return 1;
}
duk_ret_t ILibDuktape_Polyfills_Buffer_readInt32BE(duk_context *ctx)
{
	int offset = duk_require_int(ctx, 0);
	char *buffer;
	duk_size_t bufferLen;

	duk_push_this(ctx);
	buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);

	duk_push_int(ctx, ntohl(((int*)(buffer + offset))[0]));
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Buffer_alloc(duk_context *ctx)
{
	int sz = duk_require_int(ctx, 0);
	int fill = 0;

	if (duk_is_number(ctx, 1)) { fill = duk_require_int(ctx, 1); }

	duk_push_fixed_buffer(ctx, sz);
	char *buffer = Duktape_GetBuffer(ctx, -1, NULL);
	memset(buffer, fill, sz);
	duk_push_buffer_object(ctx, -1, 0, sz, DUK_BUFOBJ_NODEJS_BUFFER);
	return(1);
}

void ILibDuktape_Polyfills_Buffer(duk_context *ctx)
{
	char extras[] =
		"Object.defineProperty(Buffer.prototype, \"swap32\",\
	{\
		value: function swap32()\
		{\
			var a = this.readUInt16BE(0);\
			var b = this.readUInt16BE(2);\
			this.writeUInt16LE(a, 2);\
			this.writeUInt16LE(b, 0);\
			return(this);\
		}\
	});";

	duk_eval_string(ctx, extras); duk_pop(ctx);

	// Polyfill Buffer.from()
	duk_get_prop_string(ctx, -1, "Buffer");											// [g][Buffer]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Buffer_from, DUK_VARARGS);		// [g][Buffer][func]
	duk_put_prop_string(ctx, -2, "from");											// [g][Buffer]
	duk_pop(ctx);																	// [g]

	// Polyfill Buffer.alloc() for Node Buffers)
	duk_get_prop_string(ctx, -1, "Buffer");											// [g][Buffer]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Buffer_alloc, DUK_VARARGS);		// [g][Buffer][func]
	duk_put_prop_string(ctx, -2, "alloc");											// [g][Buffer]
	duk_pop(ctx);																	// [g]


	// Polyfill Buffer.toString() for Node Buffers
	duk_get_prop_string(ctx, -1, "Buffer");											// [g][Buffer]
	duk_get_prop_string(ctx, -1, "prototype");										// [g][Buffer][prototype]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Buffer_toString, DUK_VARARGS);	// [g][Buffer][prototype][func]
	duk_put_prop_string(ctx, -2, "toString");										// [g][Buffer][prototype]
	duk_pop_2(ctx);																	// [g]
}
duk_ret_t ILibDuktape_Polyfills_String_startsWith(duk_context *ctx)
{
	duk_size_t tokenLen;
	char *token = Duktape_GetBuffer(ctx, 0, &tokenLen);
	char *buffer;
	duk_size_t bufferLen;

	duk_push_this(ctx);
	buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);

	if (ILibString_StartsWith(buffer, (int)bufferLen, token, (int)tokenLen) != 0)
	{
		duk_push_true(ctx);
	}
	else
	{
		duk_push_false(ctx);
	}

	return 1;
}
duk_ret_t ILibDuktape_Polyfills_String_endsWith(duk_context *ctx)
{
	duk_size_t tokenLen;
	char *token = Duktape_GetBuffer(ctx, 0, &tokenLen);
	char *buffer;
	duk_size_t bufferLen;

	duk_push_this(ctx);
	buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);
	
	if (ILibString_EndsWith(buffer, (int)bufferLen, token, (int)tokenLen) != 0)
	{
		duk_push_true(ctx);
	}
	else
	{
		duk_push_false(ctx);
	}

	return 1;
}
duk_ret_t ILibDuktape_Polyfills_String_padStart(duk_context *ctx)
{
	int totalLen = (int)duk_require_int(ctx, 0);

	duk_size_t padcharLen;
	duk_size_t bufferLen;

	char *padchars;
	if (duk_get_top(ctx) > 1)
	{
		padchars = (char*)duk_get_lstring(ctx, 1, &padcharLen);
	}
	else
	{
		padchars = " ";
		padcharLen = 1;
	}

	duk_push_this(ctx);
	char *buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);

	if ((int)bufferLen > totalLen)
	{
		duk_push_lstring(ctx, buffer, bufferLen);
		return(1);
	}
	else
	{
		duk_size_t needs = totalLen - bufferLen;

		duk_push_array(ctx);											// [array]
		while(needs > 0)
		{
			if (needs > padcharLen)
			{
				duk_push_string(ctx, padchars);							// [array][pad]
				duk_put_prop_index(ctx, -2, duk_get_length(ctx, -2));	// [array]
				needs -= padcharLen;
			}
			else
			{
				duk_push_lstring(ctx, padchars, needs);					// [array][pad]
				duk_put_prop_index(ctx, -2, duk_get_length(ctx, -2));	// [array]
				needs = 0;
			}
		}
		duk_push_lstring(ctx, buffer, bufferLen);						// [array][pad]
		duk_put_prop_index(ctx, -2, duk_get_length(ctx, -2));			// [array]
		duk_get_prop_string(ctx, -1, "join");							// [array][join]
		duk_swap_top(ctx, -2);											// [join][this]
		duk_push_string(ctx, "");										// [join][this]['']
		duk_call_method(ctx, 1);										// [result]
		return(1);
	}
}
void ILibDuktape_Polyfills_String(duk_context *ctx)
{
	// Polyfill 'String.startsWith'
	duk_get_prop_string(ctx, -1, "String");											// [string]
	duk_get_prop_string(ctx, -1, "prototype");										// [string][proto]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_String_startsWith, DUK_VARARGS);	// [string][proto][func]
	duk_put_prop_string(ctx, -2, "startsWith");										// [string][proto]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_String_endsWith, DUK_VARARGS);	// [string][proto][func]
	duk_put_prop_string(ctx, -2, "endsWith");										// [string][proto]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_String_padStart, DUK_VARARGS);				// [string][proto][func]
	duk_put_prop_string(ctx, -2, "padStart");
	duk_pop_2(ctx);
}
duk_ret_t ILibDuktape_Polyfills_Console_log(duk_context *ctx)
{
	int numargs = duk_get_top(ctx);
	int i, x;
	int len = 0;
	duk_size_t strLen;
	char *str;
	char *PREFIX = NULL;
	char *DESTINATION = NULL;
	duk_push_current_function(ctx);
	ILibDuktape_LogTypes logType = (ILibDuktape_LogTypes)Duktape_GetIntPropertyValue(ctx, -1, "logType", ILibDuktape_LogType_Normal);
	switch (logType)
	{
		case ILibDuktape_LogType_Warn:
			PREFIX = (char*)"WARNING: "; // LENGTH MUST BE <= 9
			DESTINATION = ILibDuktape_Console_WARN_Destination;
			break;
		case ILibDuktape_LogType_Error:
			PREFIX = (char*)"ERROR: "; // LENGTH MUST BE <= 9
			DESTINATION = ILibDuktape_Console_ERROR_Destination;
			break;
		case ILibDuktape_LogType_Info1:
		case ILibDuktape_LogType_Info2:
		case ILibDuktape_LogType_Info3:
			duk_push_this(ctx);
			i = Duktape_GetIntPropertyValue(ctx, -1, ILibDuktape_Console_INFO_Level, 0);
			duk_pop(ctx);
			PREFIX = NULL;
			if (i >= (((int)logType + 1) - (int)ILibDuktape_LogType_Info1))
			{
				DESTINATION = ILibDuktape_Console_LOG_Destination;
			}
			else
			{
				return(0);
			}
			break;
		default:
			PREFIX = NULL;
			DESTINATION = ILibDuktape_Console_LOG_Destination;
			break;
	}
	duk_pop(ctx);

	// Calculate total length of string
	for (i = 0; i < numargs; ++i)
	{
		if (duk_is_string(ctx, i))
		{
			len += (i == 0 ? 0 : 2);
			duk_get_lstring(ctx, i, &strLen);
			len += (int)strLen;
		}
		else
		{
			duk_dup(ctx, i);
			if (strcmp("[object Object]", duk_to_string(ctx, -1)) == 0)
			{
				duk_pop(ctx);
				duk_dup(ctx, i);
				len += (i == 0 ? 1 : 3);
				duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);
				int propNum = 0;
				while (duk_next(ctx, -1, 1))
				{
					len += 2;
					len += (propNum++ == 0 ? 1 : 2);
					duk_to_lstring(ctx, -2, &strLen); len += (int)strLen;
					duk_to_lstring(ctx, -1, &strLen); len += (int)strLen;
					duk_pop_2(ctx);
				}
				duk_pop(ctx);
				len += 2;
			}
			else
			{
				len += (i == 0 ? 0 : 2);
				duk_get_lstring(ctx, -1, &strLen); len += (int)strLen;
			}
		}
	}
	len += 2; // NULL Terminator and final carriage return
	strLen = len;

	str = ILibMemory_AllocateA(strLen + ((PREFIX != NULL) ? strnlen_s(PREFIX, 9) : 0));
	x = (int)(ILibMemory_AllocateA_Size(str) - strLen);
	if (x != 0)
	{
		strLen += sprintf_s(str, strLen, PREFIX);
	}
	for (i = 0; i < numargs; ++i)
	{
		if (duk_is_string(ctx, i))
		{
			x += sprintf_s(str + x, strLen - x, "%s%s", (i == 0 ? "" : ", "), duk_require_string(ctx, i));
		}
		else
		{
			duk_dup(ctx, i);
			if (strcmp("[object Object]", duk_to_string(ctx, -1)) == 0)
			{
				duk_pop(ctx);
				duk_dup(ctx, i);
				x += sprintf_s(str+x, strLen - x, "%s", (i == 0 ? "{" : ", {"));
				duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);
				int propNum = 0;
				while (duk_next(ctx, -1, 1))
				{
					x += sprintf_s(str + x, strLen - x, "%s%s: %s", ((propNum++ == 0) ? " " : ", "), (char*)duk_to_string(ctx, -2), (char*)duk_to_string(ctx, -1));
					duk_pop_2(ctx);
				}
				duk_pop(ctx);
				x += sprintf_s(str + x, strLen - x, " }");
			}
			else
			{
				x += sprintf_s(str + x, strLen - x, "%s%s", (i == 0 ? "" : ", "), duk_to_string(ctx, -1));
			}
		}
	}
	x += sprintf_s(str + x, strLen - x, "\n");

	duk_push_this(ctx);		// [console]
	int dest = Duktape_GetIntPropertyValue(ctx, -1, DESTINATION, ILibDuktape_Console_DestinationFlags_StdOut);

	if ((dest & ILibDuktape_Console_DestinationFlags_StdOut) == ILibDuktape_Console_DestinationFlags_StdOut)
	{
#ifdef WIN32
		DWORD writeLen;
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), (void*)str, x, &writeLen, NULL);
#else
		ignore_result(write(STDOUT_FILENO, str, x));
#endif
	}
	if ((dest & ILibDuktape_Console_DestinationFlags_WebLog) == ILibDuktape_Console_DestinationFlags_WebLog)
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(Duktape_GetChain(ctx)), ILibRemoteLogging_Modules_Microstack_Generic, ILibRemoteLogging_Flags_VerbosityLevel_1, "%s", str);
	}
	if ((dest & ILibDuktape_Console_DestinationFlags_ServerConsole) == ILibDuktape_Console_DestinationFlags_ServerConsole)
	{
		if (duk_peval_string(ctx, "require('MeshAgent');") == 0)
		{
			duk_get_prop_string(ctx, -1, "SendCommand");	// [console][agent][SendCommand]
			duk_swap_top(ctx, -2);							// [console][SendCommand][this]
			duk_push_object(ctx);							// [console][SendCommand][this][options]
			duk_push_string(ctx, "msg"); duk_put_prop_string(ctx, -2, "action");
			duk_push_string(ctx, "console"); duk_put_prop_string(ctx, -2, "type");
			duk_push_string(ctx, str); duk_put_prop_string(ctx, -2, "value");
			if (duk_has_prop_string(ctx, -4, ILibDuktape_Console_SessionID))
			{
				duk_get_prop_string(ctx, -4, ILibDuktape_Console_SessionID);
				duk_put_prop_string(ctx, -2, "sessionid");
			}
			duk_call_method(ctx, 1);
		}
	}
	if ((dest & ILibDuktape_Console_DestinationFlags_LogFile) == ILibDuktape_Console_DestinationFlags_LogFile)
	{
		duk_size_t pathLen;
		char *path;
		char *tmp = ILibMemory_AllocateA(x + 32);
		int tmpx = ILibGetLocalTime(tmp + 1, (int)ILibMemory_AllocateA_Size(tmp) - 1) + 1;
		tmp[0] = '[';
		tmp[tmpx] = ']';
		tmp[tmpx + 1] = ':';
		tmp[tmpx + 2] = ' ';
		memcpy_s(tmp + tmpx + 3, ILibMemory_AllocateA_Size(tmp) - tmpx - 3, str, x);
		duk_eval_string(ctx, "require('fs');");
		duk_get_prop_string(ctx, -1, "writeFileSync");						// [fs][writeFileSync]
		duk_swap_top(ctx, -2);												// [writeFileSync][this]
		duk_push_heapptr(ctx, ILibDuktape_GetProcessObject(ctx));			// [writeFileSync][this][process]
		duk_get_prop_string(ctx, -1, "execPath");							// [writeFileSync][this][process][execPath]
		path = (char*)duk_get_lstring(ctx, -1, &pathLen);
		if (path != NULL)
		{
			if (ILibString_EndsWithEx(path, (int)pathLen, ".exe", 4, 0))
			{
				duk_get_prop_string(ctx, -1, "substring");						// [writeFileSync][this][process][execPath][substring]
				duk_swap_top(ctx, -2);											// [writeFileSync][this][process][substring][this]
				duk_push_int(ctx, 0);											// [writeFileSync][this][process][substring][this][0]
				duk_push_int(ctx, (int)(pathLen - 4));							// [writeFileSync][this][process][substring][this][0][len]
				duk_call_method(ctx, 2);										// [writeFileSync][this][process][path]
			}
			duk_get_prop_string(ctx, -1, "concat");								// [writeFileSync][this][process][path][concat]
			duk_swap_top(ctx, -2);												// [writeFileSync][this][process][concat][this]
			duk_push_string(ctx, ".jlog");										// [writeFileSync][this][process][concat][this][.jlog]
			duk_call_method(ctx, 1);											// [writeFileSync][this][process][logPath]
			duk_remove(ctx, -2);												// [writeFileSync][this][logPath]
			duk_push_string(ctx, tmp);											// [writeFileSync][this][logPath][log]
			duk_push_object(ctx);												// [writeFileSync][this][logPath][log][options]
			duk_push_string(ctx, "a"); duk_put_prop_string(ctx, -2, "flags");
			duk_call_method(ctx, 3);
		}
	}
	return 0;
}
duk_ret_t ILibDuktape_Polyfills_Console_enableWebLog(duk_context *ctx)
{
#ifdef _REMOTELOGGING
	void *chain = Duktape_GetChain(ctx);
	int port = duk_require_int(ctx, 0);
	duk_size_t pLen;
	if (duk_peval_string(ctx, "process.argv0") != 0) { return(ILibDuktape_Error(ctx, "console.enableWebLog(): Couldn't fetch argv0")); }
	char *p = (char*)duk_get_lstring(ctx, -1, &pLen);
	if (ILibString_EndsWith(p, (int)pLen, ".js", 3) != 0)
	{
		memcpy_s(ILibScratchPad2, sizeof(ILibScratchPad2), p, pLen - 3);
		sprintf_s(ILibScratchPad2 + (pLen - 3), sizeof(ILibScratchPad2) - 3, ".wlg");
	}
	else if (ILibString_EndsWith(p, (int)pLen, ".exe", 3) != 0)
	{
		memcpy_s(ILibScratchPad2, sizeof(ILibScratchPad2), p, pLen - 4);
		sprintf_s(ILibScratchPad2 + (pLen - 3), sizeof(ILibScratchPad2) - 4, ".wlg");
	}
	else
	{
		sprintf_s(ILibScratchPad2, sizeof(ILibScratchPad2), "%s.wlg", p);
	}
	ILibStartDefaultLoggerEx(chain, (unsigned short)port, ILibScratchPad2);
#endif
	return (0);
}
duk_ret_t ILibDuktape_Polyfills_Console_displayStreamPipe_getter(duk_context *ctx)
{
	duk_push_int(ctx, g_displayStreamPipeMessages);
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Console_displayStreamPipe_setter(duk_context *ctx)
{
	g_displayStreamPipeMessages = duk_require_int(ctx, 0);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_Console_displayFinalizer_getter(duk_context *ctx)
{
	duk_push_int(ctx, g_displayFinalizerMessages);
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Console_displayFinalizer_setter(duk_context *ctx)
{
	g_displayFinalizerMessages = duk_require_int(ctx, 0);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_Console_logRefCount(duk_context *ctx)
{
	printf("Reference Count => %s[%p]:%d\n", Duktape_GetStringPropertyValue(ctx, 0, ILibDuktape_OBJID, "UNKNOWN"), duk_require_heapptr(ctx, 0), ILibDuktape_GetReferenceCount(ctx, 0) - 1);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_Console_setDestination(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	int dest = duk_require_int(ctx, 0);

	duk_push_this(ctx);						// console
	if ((dest & ILibDuktape_Console_DestinationFlags_ServerConsole) == ILibDuktape_Console_DestinationFlags_ServerConsole)
	{
		// Mesh Server Console
		if (duk_peval_string(ctx, "require('MeshAgent');") != 0) { return(ILibDuktape_Error(ctx, "Unable to set destination to Mesh Console ")); }
		duk_pop(ctx);
		if (nargs > 1)
		{
			duk_dup(ctx, 1);
			duk_put_prop_string(ctx, -2, ILibDuktape_Console_SessionID);
		}
		else
		{
			duk_del_prop_string(ctx, -1, ILibDuktape_Console_SessionID);
		}
	}
	duk_dup(ctx, 0);
	duk_put_prop_string(ctx, -2, ILibDuktape_Console_Destination);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_Console_setInfoLevel(duk_context *ctx)
{
	int val = duk_require_int(ctx, 0);
	if (val < 0) { return(ILibDuktape_Error(ctx, "Invalid Info Level: %d", val)); }

	duk_push_this(ctx);
	duk_push_int(ctx, val);
	duk_put_prop_string(ctx, -2, ILibDuktape_Console_INFO_Level);

	return(0);
}
void ILibDuktape_Polyfills_Console(duk_context *ctx)
{
	// Polyfill console.log()
#ifdef WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif

	if (duk_has_prop_string(ctx, -1, "console"))
	{
		duk_get_prop_string(ctx, -1, "console");									// [g][console]
	}
	else
	{
		duk_push_object(ctx);														// [g][console]
		duk_dup(ctx, -1);															// [g][console][console]
		duk_put_prop_string(ctx, -3, "console");									// [g][console]
	}

	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Normal, "log", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Warn, "warn", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Error, "error", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Info1, "info1", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Info2, "info2", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Info3, "info3", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);


	ILibDuktape_CreateInstanceMethod(ctx, "enableWebLog", ILibDuktape_Polyfills_Console_enableWebLog, 1);
	ILibDuktape_CreateEventWithGetterAndSetterEx(ctx, "displayStreamPipeMessages", ILibDuktape_Polyfills_Console_displayStreamPipe_getter, ILibDuktape_Polyfills_Console_displayStreamPipe_setter);
	ILibDuktape_CreateEventWithGetterAndSetterEx(ctx, "displayFinalizerMessages", ILibDuktape_Polyfills_Console_displayFinalizer_getter, ILibDuktape_Polyfills_Console_displayFinalizer_setter);
	ILibDuktape_CreateInstanceMethod(ctx, "logReferenceCount", ILibDuktape_Polyfills_Console_logRefCount, 1);
	
	ILibDuktape_CreateInstanceMethod(ctx, "setDestination", ILibDuktape_Polyfills_Console_setDestination, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "setInfoLevel", ILibDuktape_Polyfills_Console_setInfoLevel, 1);

	duk_push_object(ctx);
	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_DISABLED); duk_put_prop_string(ctx, -2, "DISABLED");
	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_StdOut); duk_put_prop_string(ctx, -2, "STDOUT");
	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_ServerConsole); duk_put_prop_string(ctx, -2, "SERVERCONSOLE");
	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_WebLog); duk_put_prop_string(ctx, -2, "WEBLOG");
	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_LogFile); duk_put_prop_string(ctx, -2, "LOGFILE");
	ILibDuktape_CreateReadonlyProperty(ctx, "Destinations");

	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_StdOut | ILibDuktape_Console_DestinationFlags_LogFile);
	duk_put_prop_string(ctx, -2, ILibDuktape_Console_ERROR_Destination);

	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_StdOut | ILibDuktape_Console_DestinationFlags_LogFile);
	duk_put_prop_string(ctx, -2, ILibDuktape_Console_WARN_Destination);

	duk_push_int(ctx, 0); duk_put_prop_string(ctx, -2, ILibDuktape_Console_INFO_Level);

	duk_pop(ctx);																	// [g]
}
duk_ret_t ILibDuktape_ntohl(duk_context *ctx)
{
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 0, &bufferLen);
	int offset = duk_require_int(ctx, 1);

	if ((int)bufferLen < (4 + offset)) { return(ILibDuktape_Error(ctx, "buffer too small")); }
	duk_push_int(ctx, ntohl(((unsigned int*)(buffer + offset))[0]));
	return 1;
}
duk_ret_t ILibDuktape_ntohs(duk_context *ctx)
{
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 0, &bufferLen);
	int offset = duk_require_int(ctx, 1);

	if ((int)bufferLen < 2 + offset) { return(ILibDuktape_Error(ctx, "buffer too small")); }
	duk_push_int(ctx, ntohs(((unsigned short*)(buffer + offset))[0]));
	return 1;
}
duk_ret_t ILibDuktape_htonl(duk_context *ctx)
{
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 0, &bufferLen);
	int offset = duk_require_int(ctx, 1);
	unsigned int val = (unsigned int)duk_require_int(ctx, 2);

	if ((int)bufferLen < (4 + offset)) { return(ILibDuktape_Error(ctx, "buffer too small")); }
	((unsigned int*)(buffer + offset))[0] = htonl(val);
	return 0;
}
duk_ret_t ILibDuktape_htons(duk_context *ctx)
{
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 0, &bufferLen);
	int offset = duk_require_int(ctx, 1);
	unsigned int val = (unsigned int)duk_require_int(ctx, 2);

	if ((int)bufferLen < (2 + offset)) { return(ILibDuktape_Error(ctx, "buffer too small")); }
	((unsigned short*)(buffer + offset))[0] = htons(val);
	return 0;
}
void ILibDuktape_Polyfills_byte_ordering(duk_context *ctx)
{
	ILibDuktape_CreateInstanceMethod(ctx, "ntohl", ILibDuktape_ntohl, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "ntohs", ILibDuktape_ntohs, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "htonl", ILibDuktape_htonl, 3);
	ILibDuktape_CreateInstanceMethod(ctx, "htons", ILibDuktape_htons, 3);
}

typedef enum ILibDuktape_Timer_Type
{
	ILibDuktape_Timer_Type_TIMEOUT = 0,
	ILibDuktape_Timer_Type_INTERVAL = 1,
	ILibDuktape_Timer_Type_IMMEDIATE = 2
}ILibDuktape_Timer_Type;
typedef struct ILibDuktape_Timer
{
	duk_context *ctx;
	void *object;
	void *callback;
	void *args;
	int timeout;
	ILibDuktape_Timer_Type timerType;
}ILibDuktape_Timer;

duk_ret_t ILibDuktape_Polyfills_timer_finalizer(duk_context *ctx)
{
	// Make sure we remove any timers just in case, so we don't leak resources
	ILibDuktape_Timer *ptrs;
	if (duk_has_prop_string(ctx, 0, ILibDuktape_Timer_Ptrs))
	{
		duk_get_prop_string(ctx, 0, ILibDuktape_Timer_Ptrs);
		if (duk_has_prop_string(ctx, 0, "\xFF_callback"))
		{
			duk_del_prop_string(ctx, 0, "\xFF_callback");
		}
		if (duk_has_prop_string(ctx, 0, "\xFF_argArray"))
		{
			duk_del_prop_string(ctx, 0, "\xFF_argArray");
		}
		ptrs = (ILibDuktape_Timer*)Duktape_GetBuffer(ctx, -1, NULL);

		ILibLifeTime_Remove(ILibGetBaseTimer(Duktape_GetChain(ctx)), ptrs);
	}
	return 0;
}
void ILibDuktape_Polyfills_timer_elapsed(void *obj)
{
	ILibDuktape_Timer *ptrs = (ILibDuktape_Timer*)obj;
	int argCount, i;
	duk_context *ctx = ptrs->ctx;
	char *funcName;

	if (!ILibMemory_CanaryOK(ptrs)) { return; }
	if (duk_check_stack(ctx, 3) == 0) { return; }

	duk_push_heapptr(ctx, ptrs->callback);				// [func]
	funcName = Duktape_GetStringPropertyValue(ctx, -1, "name", "unknown_method");
	duk_push_heapptr(ctx, ptrs->object);				// [func][this]
	duk_push_heapptr(ctx, ptrs->args);					// [func][this][argArray]

	if (ptrs->timerType == ILibDuktape_Timer_Type_INTERVAL)
	{
		ILibLifeTime_AddEx(ILibGetBaseTimer(Duktape_GetChain(ctx)), ptrs, ptrs->timeout, ILibDuktape_Polyfills_timer_elapsed, NULL);
	}
	else
	{
		if (ptrs->timerType == ILibDuktape_Timer_Type_IMMEDIATE)
		{
			duk_push_heap_stash(ctx);
			duk_del_prop_string(ctx, -1, Duktape_GetStashKey(ptrs->object));
			duk_pop(ctx);
		}

		duk_del_prop_string(ctx, -2, "\xFF_callback");
		duk_del_prop_string(ctx, -2, "\xFF_argArray");
		duk_del_prop_string(ctx, -2, ILibDuktape_Timer_Ptrs);
	}

	argCount = (int)duk_get_length(ctx, -1);
	for (i = 0; i < argCount; ++i)
	{
		duk_get_prop_index(ctx, -1, i);					// [func][this][argArray][arg]
		duk_swap_top(ctx, -2);							// [func][this][arg][argArray]
	}
	duk_pop(ctx);										// [func][this][...arg...]
	if (duk_pcall_method(ctx, argCount) != 0) { ILibDuktape_Process_UncaughtExceptionEx(ctx, "timers.onElapsed() callback handler on '%s()' ", funcName); }
	duk_pop(ctx);										// ...
}
duk_ret_t ILibDuktape_Polyfills_timer_set(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	ILibDuktape_Timer *ptrs;
	ILibDuktape_Timer_Type timerType;
	void *chain = Duktape_GetChain(ctx);
	int argx;

	duk_push_current_function(ctx);
	duk_get_prop_string(ctx, -1, "type");
	timerType = (ILibDuktape_Timer_Type)duk_get_int(ctx, -1);

	duk_push_object(ctx);																	//[retVal]
	switch (timerType)
	{
	case ILibDuktape_Timer_Type_IMMEDIATE:
		ILibDuktape_WriteID(ctx, "Timers.immediate");										
		// We're only saving a reference for immediates
		duk_push_heap_stash(ctx);															//[retVal][stash]
		duk_dup(ctx, -2);																	//[retVal][stash][immediate]
		duk_put_prop_string(ctx, -2, Duktape_GetStashKey(duk_get_heapptr(ctx, -1)));		//[retVal][stash]
		duk_pop(ctx);																		//[retVal]
		break;
	case ILibDuktape_Timer_Type_INTERVAL:
		ILibDuktape_WriteID(ctx, "Timers.interval");
		break;
	case ILibDuktape_Timer_Type_TIMEOUT:
		ILibDuktape_WriteID(ctx, "Timers.timeout");
		break;
	}
	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_Polyfills_timer_finalizer);
	
	ptrs = (ILibDuktape_Timer*)Duktape_PushBuffer(ctx, sizeof(ILibDuktape_Timer));	//[retVal][ptrs]
	duk_put_prop_string(ctx, -2, ILibDuktape_Timer_Ptrs);							//[retVal]

	ptrs->ctx = ctx;
	ptrs->object = duk_get_heapptr(ctx, -1);
	ptrs->timerType = timerType;
	ptrs->timeout = timerType == ILibDuktape_Timer_Type_IMMEDIATE ? 0 : (int)duk_require_int(ctx, 1);
	ptrs->callback = duk_require_heapptr(ctx, 0);

	duk_push_array(ctx);																			//[retVal][argArray]
	for (argx = ILibDuktape_Timer_Type_IMMEDIATE == timerType ? 1 : 2; argx < nargs; ++argx)
	{
		duk_dup(ctx, argx);																			//[retVal][argArray][arg]
		duk_put_prop_index(ctx, -2, argx - (ILibDuktape_Timer_Type_IMMEDIATE == timerType ? 1 : 2));//[retVal][argArray]
	}
	ptrs->args = duk_get_heapptr(ctx, -1);															//[retVal]
	duk_put_prop_string(ctx, -2, "\xFF_argArray");

	duk_dup(ctx, 0);																				//[retVal][callback]
	duk_put_prop_string(ctx, -2, "\xFF_callback");													//[retVal]

	ILibLifeTime_AddEx(ILibGetBaseTimer(chain), ptrs, ptrs->timeout, ILibDuktape_Polyfills_timer_elapsed, NULL);
	return 1;
}
duk_ret_t ILibDuktape_Polyfills_timer_clear(duk_context *ctx)
{
	ILibDuktape_Timer *ptrs;
	ILibDuktape_Timer_Type timerType;
	
	duk_push_current_function(ctx);
	duk_get_prop_string(ctx, -1, "type");
	timerType = (ILibDuktape_Timer_Type)duk_get_int(ctx, -1);

	if(!duk_has_prop_string(ctx, 0, ILibDuktape_Timer_Ptrs)) 
	{
		switch (timerType)
		{
			case ILibDuktape_Timer_Type_TIMEOUT:
				return(ILibDuktape_Error(ctx, "timers.clearTimeout(): Invalid Parameter"));
			case ILibDuktape_Timer_Type_INTERVAL:
				return(ILibDuktape_Error(ctx, "timers.clearInterval(): Invalid Parameter"));
			case ILibDuktape_Timer_Type_IMMEDIATE:
				return(ILibDuktape_Error(ctx, "timers.clearImmediate(): Invalid Parameter"));
		}
	}

	duk_get_prop_string(ctx, 0, ILibDuktape_Timer_Ptrs);
	ptrs = (ILibDuktape_Timer*)Duktape_GetBuffer(ctx, -1, NULL);

	if (ptrs->timerType == ILibDuktape_Timer_Type_IMMEDIATE)
	{
		duk_push_heap_stash(ctx);
		duk_del_prop_string(ctx, -1, Duktape_GetStashKey(ptrs->object));
		duk_pop(ctx);
	}

	ILibLifeTime_Remove(ILibGetBaseTimer(Duktape_GetChain(ctx)), ptrs);
	return 0;
}
void ILibDuktape_Polyfills_timer(duk_context *ctx)
{
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_TIMEOUT, "setTimeout", ILibDuktape_Polyfills_timer_set, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_INTERVAL, "setInterval", ILibDuktape_Polyfills_timer_set, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_IMMEDIATE, "setImmediate", ILibDuktape_Polyfills_timer_set, DUK_VARARGS);

	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_TIMEOUT, "clearTimeout", ILibDuktape_Polyfills_timer_clear, 1);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_INTERVAL, "clearInterval", ILibDuktape_Polyfills_timer_clear, 1);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_IMMEDIATE, "clearImmediate", ILibDuktape_Polyfills_timer_clear, 1);
}
duk_ret_t ILibDuktape_Polyfills_getJSModule(duk_context *ctx)
{
	if (ILibDuktape_ModSearch_GetJSModule(ctx, (char*)duk_require_string(ctx, 0)) == 0)
	{
		return(ILibDuktape_Error(ctx, "getJSModule(): (%s) not found", (char*)duk_require_string(ctx, 0)));
	}
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_addModule(duk_context *ctx)
{
	duk_size_t moduleLen;
	char *module = (char*)Duktape_GetBuffer(ctx, 1, &moduleLen);
	char *moduleName = (char*)duk_require_string(ctx, 0);

	if (ILibDuktape_ModSearch_AddModule(ctx, moduleName, module, (int)moduleLen) != 0)
	{
		return(ILibDuktape_Error(ctx, "Cannot add module: %s", moduleName));
	}
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_addModuleObject(duk_context *ctx)
{
	void *module = duk_require_heapptr(ctx, 1);
	char *moduleName = (char*)duk_require_string(ctx, 0);

	ILibDuktape_ModSearch_AddModuleObject(ctx, moduleName, module);
	return(0);
}
duk_ret_t ILibDuktape_Queue_Finalizer(duk_context *ctx)
{
	duk_get_prop_string(ctx, 0, ILibDuktape_Queue_Ptr);
	ILibQueue_Destroy((ILibQueue)duk_get_pointer(ctx, -1));
	return(0);
}
duk_ret_t ILibDuktape_Queue_EnQueue(duk_context *ctx)
{
	ILibQueue Q;
	int i;
	int nargs = duk_get_top(ctx);
	duk_push_this(ctx);																// [queue]
	duk_get_prop_string(ctx, -1, ILibDuktape_Queue_Ptr);							// [queue][ptr]
	Q = (ILibQueue)duk_get_pointer(ctx, -1);
	duk_pop(ctx);																	// [queue]

	ILibDuktape_Push_ObjectStash(ctx);												// [queue][stash]
	duk_push_array(ctx);															// [queue][stash][array]
	for (i = 0; i < nargs; ++i)
	{
		duk_dup(ctx, i);															// [queue][stash][array][arg]
		duk_put_prop_index(ctx, -2, i);												// [queue][stash][array]
	}
	ILibQueue_EnQueue(Q, duk_get_heapptr(ctx, -1));
	duk_put_prop_string(ctx, -2, Duktape_GetStashKey(duk_get_heapptr(ctx, -1)));	// [queue][stash]
	return(0);
}
duk_ret_t ILibDuktape_Queue_DeQueue(duk_context *ctx)
{
	duk_push_current_function(ctx);
	duk_get_prop_string(ctx, -1, "peek");
	int peek = duk_get_int(ctx, -1);

	duk_push_this(ctx);										// [Q]
	duk_get_prop_string(ctx, -1, ILibDuktape_Queue_Ptr);	// [Q][ptr]
	ILibQueue Q = (ILibQueue)duk_get_pointer(ctx, -1);
	void *h = peek == 0 ? ILibQueue_DeQueue(Q) : ILibQueue_PeekQueue(Q);
	if (h == NULL) { return(ILibDuktape_Error(ctx, "Queue is empty")); }
	duk_pop(ctx);											// [Q]
	ILibDuktape_Push_ObjectStash(ctx);						// [Q][stash]
	duk_push_heapptr(ctx, h);								// [Q][stash][array]
	int length = (int)duk_get_length(ctx, -1);
	int i;
	for (i = 0; i < length; ++i)
	{
		duk_get_prop_index(ctx, -i - 1, i);				   // [Q][stash][array][args]
	}
	if (peek == 0) { duk_del_prop_string(ctx, -length - 2, Duktape_GetStashKey(h)); }
	return(length);
}
duk_ret_t ILibDuktape_Queue_isEmpty(duk_context *ctx)
{
	duk_push_this(ctx);
	duk_push_boolean(ctx, ILibQueue_IsEmpty((ILibQueue)Duktape_GetPointerProperty(ctx, -1, ILibDuktape_Queue_Ptr)));
	return(1);
}
duk_ret_t ILibDuktape_Queue_new(duk_context *ctx)
{
	duk_push_object(ctx);									// [queue]
	duk_push_pointer(ctx, ILibQueue_Create());				// [queue][ptr]
	duk_put_prop_string(ctx, -2, ILibDuktape_Queue_Ptr);	// [queue]

	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_Queue_Finalizer);
	ILibDuktape_CreateInstanceMethod(ctx, "enQueue", ILibDuktape_Queue_EnQueue, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "peek", 0, "deQueue", ILibDuktape_Queue_DeQueue, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "peek", 1, "peekQueue", ILibDuktape_Queue_DeQueue, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "isEmpty", ILibDuktape_Queue_isEmpty, 0);

	return(1);
}
void ILibDuktape_Queue_Push(duk_context *ctx, void* chain)
{
	duk_push_c_function(ctx, ILibDuktape_Queue_new, 0);
}

typedef struct ILibDuktape_DynamicBuffer_data
{
	int start;
	int end;
	int unshiftBytes;
	char *buffer;
	int bufferLen;
}ILibDuktape_DynamicBuffer_data;

typedef struct ILibDuktape_DynamicBuffer_ContextSwitchData
{
	void *chain;
	void *heapptr;
	ILibDuktape_DuplexStream *stream;
	ILibDuktape_DynamicBuffer_data *data;
	int bufferLen;
	char buffer[];
}ILibDuktape_DynamicBuffer_ContextSwitchData;

ILibTransport_DoneState ILibDuktape_DynamicBuffer_WriteSink(ILibDuktape_DuplexStream *stream, char *buffer, int bufferLen, void *user);
void ILibDuktape_DynamicBuffer_WriteSink_ChainThread(void *chain, void *user)
{
	ILibDuktape_DynamicBuffer_ContextSwitchData *data = (ILibDuktape_DynamicBuffer_ContextSwitchData*)user;
	if(ILibMemory_CanaryOK(data->stream))
	{
		ILibDuktape_DynamicBuffer_WriteSink(data->stream, data->buffer, data->bufferLen, data->data);
		ILibDuktape_DuplexStream_Ready(data->stream);
	}
	free(user);
}
ILibTransport_DoneState ILibDuktape_DynamicBuffer_WriteSink(ILibDuktape_DuplexStream *stream, char *buffer, int bufferLen, void *user)
{
	ILibDuktape_DynamicBuffer_data *data = (ILibDuktape_DynamicBuffer_data*)user;
	if (ILibIsRunningOnChainThread(stream->readableStream->chain) == 0)
	{
		ILibDuktape_DynamicBuffer_ContextSwitchData *tmp = (ILibDuktape_DynamicBuffer_ContextSwitchData*)ILibMemory_Allocate(sizeof(ILibDuktape_DynamicBuffer_ContextSwitchData) + bufferLen, 0, NULL, NULL);
		tmp->chain = stream->readableStream->chain;
		tmp->heapptr = stream->ParentObject;
		tmp->stream = stream;
		tmp->data = data;
		tmp->bufferLen = bufferLen;
		memcpy_s(tmp->buffer, bufferLen, buffer, bufferLen);
		ILibChain_RunOnMicrostackThread(tmp->chain, ILibDuktape_DynamicBuffer_WriteSink_ChainThread, tmp);
		return(ILibTransport_DoneState_INCOMPLETE);
	}


	if ((data->bufferLen - data->start - data->end) < bufferLen)
	{
		if (data->end > 0)
		{
			// Move the buffer first
			memmove_s(data->buffer, data->bufferLen, data->buffer + data->start, data->end);
			data->start = 0;
		}
		if ((data->bufferLen - data->end) < bufferLen)
		{
			// Need to resize buffer first
			int tmpSize = data->bufferLen;
			while ((tmpSize - data->end) < bufferLen)
			{
				tmpSize += 4096;
			}
			if ((data->buffer = (char*)realloc(data->buffer, tmpSize)) == NULL) { ILIBCRITICALEXIT(254); }
			data->bufferLen = tmpSize;
		}
	}


	memcpy_s(data->buffer + data->start + data->end, data->bufferLen - data->start - data->end, buffer, bufferLen);
	data->end += bufferLen;

	int unshifted = 0;
	do
	{
		duk_push_heapptr(stream->readableStream->ctx, stream->ParentObject);		// [ds]
		duk_get_prop_string(stream->readableStream->ctx, -1, "emit");				// [ds][emit]
		duk_swap_top(stream->readableStream->ctx, -2);								// [emit][this]
		duk_push_string(stream->readableStream->ctx, "readable");					// [emit][this][readable]
		if (duk_pcall_method(stream->readableStream->ctx, 1) != 0) { ILibDuktape_Process_UncaughtExceptionEx(stream->readableStream->ctx, "DynamicBuffer.WriteSink => readable(): "); }
		duk_pop(stream->readableStream->ctx);										// ...

		ILibDuktape_DuplexStream_WriteData(stream, data->buffer + data->start, data->end);
		if (data->unshiftBytes == 0)
		{
			// All the data was consumed
			data->start = data->end = 0;
		}
		else
		{
			unshifted = (data->end - data->unshiftBytes);
			if (unshifted > 0)
			{
				data->start += unshifted;
				data->end = data->unshiftBytes;
				data->unshiftBytes = 0;
			}
		}
	} while (unshifted != 0);

	return(ILibTransport_DoneState_COMPLETE);
}
void ILibDuktape_DynamicBuffer_EndSink(ILibDuktape_DuplexStream *stream, void *user)
{
	ILibDuktape_DuplexStream_WriteEnd(stream);
}
duk_ret_t ILibDuktape_DynamicBuffer_Finalizer(duk_context *ctx)
{
	duk_get_prop_string(ctx, 0, "\xFF_buffer");
	ILibDuktape_DynamicBuffer_data *data = (ILibDuktape_DynamicBuffer_data*)Duktape_GetBuffer(ctx, -1, NULL);
	free(data->buffer);
	return(0);
}

int ILibDuktape_DynamicBuffer_unshift(ILibDuktape_DuplexStream *sender, int unshiftBytes, void *user)
{
	ILibDuktape_DynamicBuffer_data *data = (ILibDuktape_DynamicBuffer_data*)user;
	data->unshiftBytes = unshiftBytes;
	return(unshiftBytes);
}
duk_ret_t ILibDuktape_DynamicBuffer_read(duk_context *ctx)
{
	ILibDuktape_DynamicBuffer_data *data;
	duk_push_this(ctx);															// [DynamicBuffer]
	duk_get_prop_string(ctx, -1, "\xFF_buffer");								// [DynamicBuffer][buffer]
	data = (ILibDuktape_DynamicBuffer_data*)Duktape_GetBuffer(ctx, -1, NULL);
	duk_push_external_buffer(ctx);												// [DynamicBuffer][buffer][extBuffer]
	duk_config_buffer(ctx, -1, data->buffer + data->start, data->bufferLen - (data->start + data->end));
	duk_push_buffer_object(ctx, -1, 0, data->bufferLen - (data->start + data->end), DUK_BUFOBJ_NODEJS_BUFFER);
	return(1);
}
duk_ret_t ILibDuktape_DynamicBuffer_new(duk_context *ctx)
{
	ILibDuktape_DynamicBuffer_data *data;
	int initSize = 4096;
	if (duk_get_top(ctx) != 0)
	{
		initSize = duk_require_int(ctx, 0);
	}

	duk_push_object(ctx);					// [stream]
	duk_push_fixed_buffer(ctx, sizeof(ILibDuktape_DynamicBuffer_data));
	data = (ILibDuktape_DynamicBuffer_data*)Duktape_GetBuffer(ctx, -1, NULL);
	memset(data, 0, sizeof(ILibDuktape_DynamicBuffer_data));
	duk_put_prop_string(ctx, -2, "\xFF_buffer");

	data->bufferLen = initSize;
	data->buffer = (char*)malloc(initSize);

	ILibDuktape_DuplexStream_InitEx(ctx, ILibDuktape_DynamicBuffer_WriteSink, ILibDuktape_DynamicBuffer_EndSink, NULL, NULL, ILibDuktape_DynamicBuffer_unshift, data);
	ILibDuktape_EventEmitter_CreateEventEx(ILibDuktape_EventEmitter_GetEmitter(ctx, -1), "readable");
	ILibDuktape_CreateInstanceMethod(ctx, "read", ILibDuktape_DynamicBuffer_read, DUK_VARARGS);
	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_DynamicBuffer_Finalizer);

	return(1);
}

void ILibDuktape_DynamicBuffer_Push(duk_context *ctx, void *chain)
{
	duk_push_c_function(ctx, ILibDuktape_DynamicBuffer_new, DUK_VARARGS);
}

duk_ret_t ILibDuktape_Polyfills_debugCrash(duk_context *ctx)
{
	void *p = NULL;
	((int*)p)[0] = 55;
	return(0);
}


void ILibDuktape_Stream_PauseSink(struct ILibDuktape_readableStream *sender, void *user)
{
}
void ILibDuktape_Stream_ResumeSink(struct ILibDuktape_readableStream *sender, void *user)
{
	int skip = 0;
	duk_size_t bufferLen;

	duk_push_heapptr(sender->ctx, sender->object);			// [stream]
	void *func = Duktape_GetHeapptrProperty(sender->ctx, -1, "_read");
	duk_pop(sender->ctx);									// ...

	while (func != NULL && sender->paused == 0)
	{
		duk_push_heapptr(sender->ctx, sender->object);									// [this]
		if (!skip && duk_has_prop_string(sender->ctx, -1, ILibDuktape_Stream_Buffer))
		{
			duk_get_prop_string(sender->ctx, -1, ILibDuktape_Stream_Buffer);			// [this][buffer]
			if ((bufferLen = duk_get_length(sender->ctx, -1)) > 0)
			{
				// Buffer is not empty, so we need to 'PUSH' it
				duk_get_prop_string(sender->ctx, -2, "push");							// [this][buffer][push]
				duk_dup(sender->ctx, -3);												// [this][buffer][push][this]
				duk_dup(sender->ctx, -3);												// [this][buffer][push][this][buffer]
				duk_remove(sender->ctx, -4);											// [this][push][this][buffer]
				duk_call_method(sender->ctx, 1);										// [this][boolean]
				sender->paused = !duk_get_boolean(sender->ctx, -1);
				duk_pop(sender->ctx);													// [this]

				if (duk_has_prop_string(sender->ctx, -1, ILibDuktape_Stream_Buffer))
				{
					duk_get_prop_string(sender->ctx, -1, ILibDuktape_Stream_Buffer);	// [this][buffer]
					if (duk_get_length(sender->ctx, -1) == bufferLen)
					{
						// All the data was unshifted
						skip = !sender->paused;					
					}
					duk_pop(sender->ctx);												// [this]
				}
				duk_pop(sender->ctx);													// ...
			}
			else
			{
				// Buffer is empty
				duk_pop(sender->ctx);													// [this]
				duk_del_prop_string(sender->ctx, -1, ILibDuktape_Stream_Buffer);
				duk_pop(sender->ctx);													// ...
			}
		}
		else
		{
			// We need to 'read' more data
			duk_push_heapptr(sender->ctx, func);										// [this][read]
			duk_swap_top(sender->ctx, -2);												// [read][this]
			if (duk_pcall_method(sender->ctx, 0) != 0) { ILibDuktape_Process_UncaughtException(sender->ctx); duk_pop(sender->ctx); break; }
			//																			// [buffer]
			duk_push_heapptr(sender->ctx, sender->object);								// [buffer][this]
			duk_swap_top(sender->ctx, -2);												// [this][buffer]
			if (duk_has_prop_string(sender->ctx, -2, ILibDuktape_Stream_Buffer))
			{
				duk_push_global_object(sender->ctx);									// [this][buffer][g]
				duk_get_prop_string(sender->ctx, -1, "Buffer");							// [this][buffer][g][Buffer]
				duk_remove(sender->ctx, -2);											// [this][buffer][Buffer]
				duk_get_prop_string(sender->ctx, -1, "concat");							// [this][buffer][Buffer][concat]
				duk_swap_top(sender->ctx, -2);											// [this][buffer][concat][this]
				duk_push_array(sender->ctx);											// [this][buffer][concat][this][Array]
				duk_get_prop_string(sender->ctx, -1, "push");							// [this][buffer][concat][this][Array][push]
				duk_dup(sender->ctx, -2);												// [this][buffer][concat][this][Array][push][this]
				duk_get_prop_string(sender->ctx, -7, ILibDuktape_Stream_Buffer);		// [this][buffer][concat][this][Array][push][this][buffer]
				duk_call_method(sender->ctx, 1); duk_pop(sender->ctx);					// [this][buffer][concat][this][Array]
				duk_get_prop_string(sender->ctx, -1, "push");							// [this][buffer][concat][this][Array][push]
				duk_dup(sender->ctx, -2);												// [this][buffer][concat][this][Array][push][this]
				duk_dup(sender->ctx, -6);												// [this][buffer][concat][this][Array][push][this][buffer]
				duk_remove(sender->ctx, -7);											// [this][concat][this][Array][push][this][buffer]
				duk_call_method(sender->ctx, 1); duk_pop(sender->ctx);					// [this][concat][this][Array]
				duk_call_method(sender->ctx, 1);										// [this][buffer]
			}
			duk_put_prop_string(sender->ctx, -2, ILibDuktape_Stream_Buffer);			// [this]
			duk_pop(sender->ctx);														// ...
			skip = 0;
		}
	}
}
int ILibDuktape_Stream_UnshiftSink(struct ILibDuktape_readableStream *sender, int unshiftBytes, void *user)
{
	duk_push_fixed_buffer(sender->ctx, unshiftBytes);									// [buffer]
	memcpy_s(Duktape_GetBuffer(sender->ctx, -1, NULL), unshiftBytes, sender->unshiftReserved, unshiftBytes);
	duk_push_heapptr(sender->ctx, sender->object);										// [buffer][stream]
	duk_push_buffer_object(sender->ctx, -2, 0, unshiftBytes, DUK_BUFOBJ_NODEJS_BUFFER);	// [buffer][stream][buffer]
	duk_put_prop_string(sender->ctx, -2, ILibDuktape_Stream_Buffer);					// [buffer][stream]
	duk_pop_2(sender->ctx);																// ...

	return(unshiftBytes);
}
duk_ret_t ILibDuktape_Stream_Push(duk_context *ctx)
{
	duk_push_this(ctx);																					// [stream]
	ILibDuktape_readableStream *RS = (ILibDuktape_readableStream*)Duktape_GetPointerProperty(ctx, -1, ILibDuktape_Stream_ReadablePtr);

	duk_size_t bufferLen;
	char *buffer = (char*)Duktape_GetBuffer(ctx, 0, &bufferLen);

	duk_push_boolean(ctx, !ILibDuktape_readableStream_WriteDataEx(RS, 0, buffer, (int)bufferLen));		// [stream][buffer][retVal]
	return(1);
}
duk_ret_t ILibDuktape_Stream_EndSink(duk_context *ctx)
{
	duk_push_this(ctx);												// [stream]
	ILibDuktape_readableStream *RS = (ILibDuktape_readableStream*)Duktape_GetPointerProperty(ctx, -1, ILibDuktape_Stream_ReadablePtr);
	ILibDuktape_readableStream_WriteEnd(RS);
	return(0);
}
duk_idx_t ILibDuktape_Stream_newReadable(duk_context *ctx)
{
	ILibDuktape_readableStream *RS;
	duk_push_object(ctx);							// [Readable]
	ILibDuktape_WriteID(ctx, "stream.readable");
	RS = ILibDuktape_ReadableStream_InitEx(ctx, ILibDuktape_Stream_PauseSink, ILibDuktape_Stream_ResumeSink, ILibDuktape_Stream_UnshiftSink, NULL);
	RS->paused = 1;

	duk_push_pointer(ctx, RS);
	duk_put_prop_string(ctx, -2, ILibDuktape_Stream_ReadablePtr);
	ILibDuktape_CreateInstanceMethod(ctx, "push", ILibDuktape_Stream_Push, DUK_VARARGS);
	ILibDuktape_EventEmitter_AddOnceEx3(ctx, -1, "end", ILibDuktape_Stream_EndSink);

	if (duk_is_object(ctx, 0))
	{
		void *h = Duktape_GetHeapptrProperty(ctx, 0, "read");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_read"); }
	}
	return(1);
}
duk_ret_t ILibDuktape_Stream_Writable_WriteSink_Flush(duk_context *ctx)
{
	duk_push_current_function(ctx);
	ILibTransport_DoneState *retVal = (ILibTransport_DoneState*)Duktape_GetPointerProperty(ctx, -1, "retval");
	if (retVal != NULL)
	{
		*retVal = ILibTransport_DoneState_COMPLETE;
	}
	else
	{
		ILibDuktape_WritableStream *WS = (ILibDuktape_WritableStream*)Duktape_GetPointerProperty(ctx, -1, ILibDuktape_Stream_WritablePtr);
		ILibDuktape_WritableStream_Ready(WS);
	}
	return(0);
}
ILibTransport_DoneState ILibDuktape_Stream_Writable_WriteSink(struct ILibDuktape_WritableStream *stream, char *buffer, int bufferLen, void *user)
{
	void *h;
	ILibTransport_DoneState retVal = ILibTransport_DoneState_INCOMPLETE;

	duk_push_this(stream->ctx);																		// [writable]
	duk_get_prop_string(stream->ctx, -1, "_write");													// [writable][_write]
	duk_swap_top(stream->ctx, -2);																	// [_write][this]
	if (stream->Reserved == 0)
	{
		duk_push_external_buffer(stream->ctx);														// [_write][this][extBuffer]
		duk_insert(stream->ctx, -3);																// [extBuffer][_write][this]
		duk_config_buffer(stream->ctx, -3, buffer, (duk_size_t)bufferLen);
		duk_push_buffer_object(stream->ctx, -3, 0, (duk_size_t)bufferLen, DUK_BUFOBJ_NODEJS_BUFFER);// [extBuffer][_write][this][buffer]
	}
	else
	{
		duk_push_lstring(stream->ctx, buffer, (duk_size_t)bufferLen);								// [_write][this][string]
	}
	duk_push_c_function(stream->ctx, ILibDuktape_Stream_Writable_WriteSink_Flush, DUK_VARARGS);		// [_write][this][string/buffer][callback]
	h = duk_get_heapptr(stream->ctx, -1);
	duk_push_heap_stash(stream->ctx);																// [_write][this][string/buffer][callback][stash]
	duk_dup(stream->ctx, -2);																		// [_write][this][string/buffer][callback][stash][callback]
	duk_put_prop_string(stream->ctx, -2, Duktape_GetStashKey(h));									// [_write][this][string/buffer][callback][stash]
	duk_pop(stream->ctx);																			// [_write][this][string/buffer][callback]
	duk_push_pointer(stream->ctx, stream); duk_put_prop_string(stream->ctx, -2, ILibDuktape_Stream_WritablePtr);

	duk_push_pointer(stream->ctx, &retVal);															// [_write][this][string/buffer][callback][retval]
	duk_put_prop_string(stream->ctx, -2, "retval");													// [_write][this][string/buffer][callback]
	if (duk_pcall_method(stream->ctx, 2) != 0)
	{
		ILibDuktape_Process_UncaughtExceptionEx(stream->ctx, "stream.writable.write(): "); retVal = ILibTransport_DoneState_ERROR;
	}
	else
	{
		retVal = duk_to_boolean(stream->ctx, -1) ? ILibTransport_DoneState_COMPLETE : ILibTransport_DoneState_INCOMPLETE;
	}
	duk_pop(stream->ctx);																			// ...

	duk_push_heapptr(stream->ctx, h);																// [callback]
	duk_del_prop_string(stream->ctx, -1, "retval");
	duk_pop(stream->ctx);																			// ...
	
	duk_push_heap_stash(stream->ctx);
	duk_del_prop_string(stream->ctx, -1, Duktape_GetStashKey(h));
	duk_pop(stream->ctx);

	return(retVal);
}
duk_ret_t ILibDuktape_Stream_Writable_EndSink_finish(duk_context *ctx)
{
	duk_push_current_function(ctx);
	ILibDuktape_WritableStream *ws = (ILibDuktape_WritableStream*)Duktape_GetPointerProperty(ctx, -1, "ptr");
	if (ILibMemory_CanaryOK(ws))
	{
		ILibDuktape_WritableStream_Finish(ws);
	}
	return(0);
}
void ILibDuktape_Stream_Writable_EndSink(struct ILibDuktape_WritableStream *stream, void *user)
{
	duk_push_this(stream->ctx);															// [writable]
	duk_get_prop_string(stream->ctx, -1, "_final");										// [writable][_final]
	duk_swap_top(stream->ctx, -2);														// [_final][this]
	duk_push_c_function(stream->ctx, ILibDuktape_Stream_Writable_EndSink_finish, 0);	// [_final][this][callback]
	duk_push_pointer(stream->ctx, stream); duk_put_prop_string(stream->ctx, -2, "ptr");
	if (duk_pcall_method(stream->ctx, 1) != 0) { ILibDuktape_Process_UncaughtExceptionEx(stream->ctx, "stream.writable._final(): "); }
	duk_pop(stream->ctx);								// ...
}
duk_ret_t ILibDuktape_Stream_newWritable(duk_context *ctx)
{
	ILibDuktape_WritableStream *WS;
	duk_push_object(ctx);						// [Writable]
	ILibDuktape_WriteID(ctx, "stream.writable");
	WS = ILibDuktape_WritableStream_Init(ctx, ILibDuktape_Stream_Writable_WriteSink, ILibDuktape_Stream_Writable_EndSink, NULL);
	WS->JSCreated = 1;

	duk_push_pointer(ctx, WS);
	duk_put_prop_string(ctx, -2, ILibDuktape_Stream_WritablePtr);

	if (duk_is_object(ctx, 0))
	{
		void *h = Duktape_GetHeapptrProperty(ctx, 0, "write");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_write"); }
		h = Duktape_GetHeapptrProperty(ctx, 0, "final");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_final"); }
	}
	return(1);
}
void ILibDuktape_Stream_Duplex_PauseSink(ILibDuktape_DuplexStream *stream, void *user)
{
	ILibDuktape_Stream_PauseSink(stream->readableStream, user);
}
void ILibDuktape_Stream_Duplex_ResumeSink(ILibDuktape_DuplexStream *stream, void *user)
{
	ILibDuktape_Stream_ResumeSink(stream->readableStream, user);
}
int ILibDuktape_Stream_Duplex_UnshiftSink(ILibDuktape_DuplexStream *stream, int unshiftBytes, void *user)
{
	return(ILibDuktape_Stream_UnshiftSink(stream->readableStream, unshiftBytes, user));
}
ILibTransport_DoneState ILibDuktape_Stream_Duplex_WriteSink(ILibDuktape_DuplexStream *stream, char *buffer, int bufferLen, void *user)
{
	return(ILibDuktape_Stream_Writable_WriteSink(stream->writableStream, buffer, bufferLen, user));
}
void ILibDuktape_Stream_Duplex_EndSink(ILibDuktape_DuplexStream *stream, void *user)
{
	ILibDuktape_Stream_Writable_EndSink(stream->writableStream, user);
}
duk_ret_t ILibDuktape_Stream_newDuplex(duk_context *ctx)
{
	ILibDuktape_DuplexStream *DS;
	duk_push_object(ctx);						// [Duplex]
	ILibDuktape_WriteID(ctx, "stream.Duplex");
	DS = ILibDuktape_DuplexStream_InitEx(ctx, ILibDuktape_Stream_Duplex_WriteSink, ILibDuktape_Stream_Duplex_EndSink, ILibDuktape_Stream_Duplex_PauseSink, ILibDuktape_Stream_Duplex_ResumeSink, ILibDuktape_Stream_Duplex_UnshiftSink, NULL);
	DS->writableStream->JSCreated = 1;

	duk_push_pointer(ctx, DS->writableStream);
	duk_put_prop_string(ctx, -2, ILibDuktape_Stream_WritablePtr);

	duk_push_pointer(ctx, DS->readableStream);
	duk_put_prop_string(ctx, -2, ILibDuktape_Stream_ReadablePtr);
	ILibDuktape_CreateInstanceMethod(ctx, "push", ILibDuktape_Stream_Push, DUK_VARARGS);
	ILibDuktape_EventEmitter_AddOnceEx3(ctx, -1, "end", ILibDuktape_Stream_EndSink);

	if (duk_is_object(ctx, 0))
	{
		void *h = Duktape_GetHeapptrProperty(ctx, 0, "write");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_write"); }
		h = Duktape_GetHeapptrProperty(ctx, 0, "final");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_final"); }
		h = Duktape_GetHeapptrProperty(ctx, 0, "read");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_read"); }
	}
	return(1);
}
void ILibDuktape_Stream_Init(duk_context *ctx, void *chain)
{
	duk_push_object(ctx);					// [stream
	ILibDuktape_WriteID(ctx, "stream");
	ILibDuktape_CreateInstanceMethod(ctx, "Readable", ILibDuktape_Stream_newReadable, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "Writable", ILibDuktape_Stream_newWritable, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "Duplex", ILibDuktape_Stream_newDuplex, DUK_VARARGS);
}
void ILibDuktape_Polyfills_debugGC2(duk_context *ctx, void ** args, int argsLen)
{
	if (g_displayFinalizerMessages) { printf("=> GC();\n"); }
	duk_gc(ctx, 0);
	duk_gc(ctx, 0);
}
duk_ret_t ILibDuktape_Polyfills_debugGC(duk_context *ctx)
{
	ILibDuktape_Immediate(ctx, (void*[]) { NULL }, 0, ILibDuktape_Polyfills_debugGC2);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_debug(duk_context *ctx)
{
#ifdef WIN32
	if (IsDebuggerPresent()) { __debugbreak(); }
#elif defined(_POSIX)
	raise(SIGTRAP);
#endif
	return(0);
}
#ifndef MICROSTACK_NOTLS
duk_ret_t ILibDuktape_PKCS7_getSignedDataBlock(duk_context *ctx)
{
	char *hash = ILibMemory_AllocateA(UTIL_SHA256_HASHSIZE);
	char *pkeyHash = ILibMemory_AllocateA(UTIL_SHA256_HASHSIZE);
	unsigned int size, r;
	BIO *out = NULL;
	PKCS7 *message = NULL;
	char* data2 = NULL;
	STACK_OF(X509) *st = NULL;

	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 0, &bufferLen);

	message = d2i_PKCS7(NULL, (const unsigned char**)&buffer, (long)bufferLen);
	if (message == NULL) { return(ILibDuktape_Error(ctx, "PKCS7 Error")); }

	// Lets rebuild the original message and check the size
	size = i2d_PKCS7(message, NULL);
	if (size < (unsigned int)bufferLen) { PKCS7_free(message); return(ILibDuktape_Error(ctx, "PKCS7 Error")); }

	out = BIO_new(BIO_s_mem());

	// Check the PKCS7 signature, but not the certificate chain.
	r = PKCS7_verify(message, NULL, NULL, NULL, out, PKCS7_NOVERIFY);
	if (r == 0) { PKCS7_free(message); BIO_free(out); return(ILibDuktape_Error(ctx, "PKCS7 Verify Error")); }

	// If data block contains less than 32 bytes, fail.
	size = (unsigned int)BIO_get_mem_data(out, &data2);
	if (size <= ILibMemory_AllocateA_Size(hash)) { PKCS7_free(message); BIO_free(out); return(ILibDuktape_Error(ctx, "PKCS7 Size Mismatch Error")); }


	duk_push_object(ctx);												// [val]
	duk_push_fixed_buffer(ctx, size);									// [val][fbuffer]
	duk_dup(ctx, -1);													// [val][fbuffer][dup]
	duk_put_prop_string(ctx, -3, "\xFF_fixedbuffer");					// [val][fbuffer]
	duk_swap_top(ctx, -2);												// [fbuffer][val]
	duk_push_buffer_object(ctx, -2, 0, size, DUK_BUFOBJ_NODEJS_BUFFER); // [fbuffer][val][buffer]
	ILibDuktape_CreateReadonlyProperty(ctx, "data");					// [fbuffer][val]
	memcpy_s(Duktape_GetBuffer(ctx, -2, NULL), size, data2, size);


	// Get the certificate signer
	st = PKCS7_get0_signers(message, NULL, PKCS7_NOVERIFY);
	
	// Get a full certificate hash of the signer
	X509_digest(sk_X509_value(st, 0), EVP_sha256(), (unsigned char*)hash, NULL);
	X509_pubkey_digest(sk_X509_value(st, 0), EVP_sha256(), (unsigned char*)pkeyHash, NULL); 

	sk_X509_free(st);
	
	// Check certificate hash with first 32 bytes of data.
	if (memcmp(hash, Duktape_GetBuffer(ctx, -2, NULL), ILibMemory_AllocateA_Size(hash)) != 0) { PKCS7_free(message); BIO_free(out); return(ILibDuktape_Error(ctx, "PKCS7 Certificate Hash Mismatch Error")); }
	char *tmp = ILibMemory_AllocateA(1 + (ILibMemory_AllocateA_Size(hash) * 2));
	util_tohex(hash, (int)ILibMemory_AllocateA_Size(hash), tmp);
	duk_push_object(ctx);												// [fbuffer][val][cert]
	ILibDuktape_WriteID(ctx, "certificate");
	duk_push_string(ctx, tmp);											// [fbuffer][val][cert][fingerprint]
	ILibDuktape_CreateReadonlyProperty(ctx, "fingerprint");				// [fbuffer][val][cert]
	util_tohex(pkeyHash, (int)ILibMemory_AllocateA_Size(pkeyHash), tmp);
	duk_push_string(ctx, tmp);											// [fbuffer][val][cert][publickeyhash]
	ILibDuktape_CreateReadonlyProperty(ctx, "publicKeyHash");			// [fbuffer][val][cert]

	ILibDuktape_CreateReadonlyProperty(ctx, "signingCertificate");		// [fbuffer][val]

	// Approved, cleanup and return.
	BIO_free(out);
	PKCS7_free(message);

	return(1);
}
duk_ret_t ILibDuktape_PKCS7_signDataBlockFinalizer(duk_context *ctx)
{
	char *buffer = Duktape_GetPointerProperty(ctx, 0, "\xFF_signature");
	if (buffer != NULL) { free(buffer); }
	return(0);
}
duk_ret_t ILibDuktape_PKCS7_signDataBlock(duk_context *ctx)
{
	duk_get_prop_string(ctx, 1, "secureContext");
	duk_get_prop_string(ctx, -1, "\xFF_SecureContext2CertBuffer");
	struct util_cert *cert = (struct util_cert*)Duktape_GetBuffer(ctx, -1, NULL);
	duk_size_t bufferLen;
	char *buffer = (char*)Duktape_GetBuffer(ctx, 0, &bufferLen);

	BIO *in = NULL;
	PKCS7 *message = NULL;
	char *signature = NULL;
	int signatureLength = 0;

	// Sign the block
	in = BIO_new_mem_buf(buffer, (int)bufferLen);
	message = PKCS7_sign(cert->x509, cert->pkey, NULL, in, PKCS7_BINARY);
	if (message != NULL)
	{
		signatureLength = i2d_PKCS7(message, (unsigned char**)&signature);
		PKCS7_free(message);
	}
	if (in != NULL) BIO_free(in);
	if (signatureLength <= 0) { return(ILibDuktape_Error(ctx, "PKCS7_signDataBlockError: ")); }

	duk_push_external_buffer(ctx);
	duk_config_buffer(ctx, -1, signature, signatureLength);
	duk_push_buffer_object(ctx, -1, 0, signatureLength, DUK_BUFOBJ_NODEJS_BUFFER);
	duk_push_pointer(ctx, signature);
	duk_put_prop_string(ctx, -2, "\xFF_signature");
	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_PKCS7_signDataBlockFinalizer);

	return(1);
}
void ILibDuktape_PKCS7_Push(duk_context *ctx, void *chain)
{
	duk_push_object(ctx);
	ILibDuktape_CreateInstanceMethod(ctx, "getSignedDataBlock", ILibDuktape_PKCS7_getSignedDataBlock, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "signDataBlock", ILibDuktape_PKCS7_signDataBlock, DUK_VARARGS);
}

extern uint32_t crc32c(uint32_t crc, const unsigned char* buf, uint32_t len);
duk_ret_t ILibDuktape_Polyfills_crc32c(duk_context *ctx)
{
	duk_size_t len;
	char *buffer = Duktape_GetBuffer(ctx, 0, &len);
	duk_push_int(ctx, crc32c(0, (unsigned char*)buffer, (uint32_t)len));
	return(1);
}
#endif
duk_ret_t ILibDuktape_Polyfills_Object_hashCode(duk_context *ctx)
{
	duk_push_this(ctx);
	duk_push_string(ctx, Duktape_GetStashKey(duk_get_heapptr(ctx, -1)));
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Array_peek(duk_context *ctx)
{
	duk_push_this(ctx);				// [Array]
	duk_get_prop_index(ctx, -1, (duk_uarridx_t)duk_get_length(ctx, -1) - 1);
	return(1);
}
void ILibDuktape_Polyfills_object(duk_context *ctx)
{
	// Polyfill Object._hashCode() 
	duk_get_prop_string(ctx, -1, "Object");											// [g][Object]
	duk_get_prop_string(ctx, -1, "prototype");										// [g][Object][prototype]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Object_hashCode, 0);				// [g][Object][prototype][func]
	ILibDuktape_CreateReadonlyProperty(ctx, "_hashCode");							// [g][Object][prototype]
	duk_pop_2(ctx);																	// [g]

	duk_get_prop_string(ctx, -1, "Array");											// [g][Array]
	duk_get_prop_string(ctx, -1, "prototype");										// [g][Array][prototype]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Array_peek, 0);					// [g][Array][prototype][peek]
	ILibDuktape_CreateReadonlyProperty(ctx, "peek");								// [g][Array][prototype]
	duk_pop_2(ctx);																	// [g]
}


#ifndef MICROSTACK_NOTLS
void ILibDuktape_bignum_addBigNumMethods(duk_context *ctx, BIGNUM *b);
duk_ret_t ILibDuktape_bignum_toString(duk_context *ctx)
{
	duk_push_this(ctx);
	BIGNUM *b = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	if (b != NULL)
	{
		char *numstr = BN_bn2dec(b);
		duk_push_string(ctx, numstr);
		OPENSSL_free(numstr);
		return(1);
	}
	else
	{
		return(ILibDuktape_Error(ctx, "Invalid BIGNUM"));
	}
}
duk_ret_t ILibDuktape_bignum_add(duk_context* ctx)
{
	BIGNUM *ret = BN_new();
	BIGNUM *r1, *r2;

	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");

	BN_add(ret, r1, r2);
	ILibDuktape_bignum_addBigNumMethods(ctx, ret);
	return(1);
}
duk_ret_t ILibDuktape_bignum_sub(duk_context* ctx)
{
	BIGNUM *ret = BN_new();
	BIGNUM *r1, *r2;

	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");

	BN_sub(ret, r1, r2);
	ILibDuktape_bignum_addBigNumMethods(ctx, ret);
	return(1);
}
duk_ret_t ILibDuktape_bignum_mul(duk_context* ctx)
{
	BN_CTX *bx = BN_CTX_new();
	BIGNUM *ret = BN_new();
	BIGNUM *r1, *r2;

	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");
	BN_mul(ret, r1, r2, bx);
	BN_CTX_free(bx);
	ILibDuktape_bignum_addBigNumMethods(ctx, ret);
	return(1);
}
duk_ret_t ILibDuktape_bignum_div(duk_context* ctx)
{
	BN_CTX *bx = BN_CTX_new();
	BIGNUM *ret = BN_new();
	BIGNUM *r1, *r2;

	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");
	BN_div(ret, NULL, r1, r2, bx);

	BN_CTX_free(bx);
	ILibDuktape_bignum_addBigNumMethods(ctx, ret);
	return(1);
}
duk_ret_t ILibDuktape_bignum_mod(duk_context* ctx)
{
	BN_CTX *bx = BN_CTX_new();
	BIGNUM *ret = BN_new();
	BIGNUM *r1, *r2;

	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");
	BN_div(NULL, ret, r1, r2, bx);

	BN_CTX_free(bx);
	ILibDuktape_bignum_addBigNumMethods(ctx, ret);
	return(1);
}

duk_ret_t ILibDuktape_bignum_finalizer(duk_context *ctx)
{
	BIGNUM *b = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");
	if (b != NULL)
	{
		BN_free(b);
	}
	return(0);
}
void ILibDuktape_bignum_addBigNumMethods(duk_context *ctx, BIGNUM *b)
{
	duk_push_object(ctx);
	duk_push_pointer(ctx, b); duk_put_prop_string(ctx, -2, "\xFF_BIGNUM");
	ILibDuktape_CreateProperty_InstanceMethod(ctx, "toString", ILibDuktape_bignum_toString, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "add", ILibDuktape_bignum_add, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "sub", ILibDuktape_bignum_sub, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "mul", ILibDuktape_bignum_mul, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "div", ILibDuktape_bignum_div, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "mod", ILibDuktape_bignum_mod, 1);
	duk_push_c_function(ctx, ILibDuktape_bignum_finalizer, 1); duk_set_finalizer(ctx, -2);
	duk_eval_string(ctx, "(function toNumber(){return(parseInt(this.toString()));})"); duk_put_prop_string(ctx, -2, "toNumber");
}
duk_ret_t ILibDuktape_bignum_fromBuffer(duk_context *ctx)
{
	char *endian = duk_get_top(ctx) > 1 ? Duktape_GetStringPropertyValue(ctx, 1, "endian", "big") : "big";
	duk_size_t len;
	char *buffer = Duktape_GetBuffer(ctx, 0, &len);
	BIGNUM *b;

	if (strcmp(endian, "big") == 0)
	{
		b = BN_bin2bn((unsigned char*)buffer, (int)len, NULL);
	}
	else if (strcmp(endian, "little") == 0)
	{
		b = BN_lebin2bn((unsigned char*)buffer, (int)len, NULL);
	}
	else
	{
		return(ILibDuktape_Error(ctx, "Invalid endian specified"));
	}

	ILibDuktape_bignum_addBigNumMethods(ctx, b);
	return(1);
}

duk_ret_t ILibDuktape_bignum_func(duk_context *ctx)
{	
	BIGNUM *b = NULL;
	BN_dec2bn(&b, duk_require_string(ctx, 0));
	ILibDuktape_bignum_addBigNumMethods(ctx, b);
	return(1);
}
void ILibDuktape_bignum_Push(duk_context *ctx, void *chain)
{
	duk_push_c_function(ctx, ILibDuktape_bignum_func, DUK_VARARGS);
	duk_push_c_function(ctx, ILibDuktape_bignum_fromBuffer, DUK_VARARGS); duk_put_prop_string(ctx, -2, "fromBuffer");
}
void ILibDuktape_dataGenerator_onPause(struct ILibDuktape_readableStream *sender, void *user)
{

}
void ILibDuktape_dataGenerator_onResume(struct ILibDuktape_readableStream *sender, void *user)
{
	SHA256_CTX shctx;

	char *buffer = (char*)user;
	size_t bufferLen = ILibMemory_Size(buffer);
	int val;

	while (sender->paused == 0)
	{
		duk_push_heapptr(sender->ctx, sender->object);
		val = Duktape_GetIntPropertyValue(sender->ctx, -1, "\xFF_counter", 0);
		duk_push_int(sender->ctx, (val + 1) < 255 ? (val+1) : 0); duk_put_prop_string(sender->ctx, -2, "\xFF_counter");
		duk_pop(sender->ctx);

		//util_random((int)(bufferLen - UTIL_SHA256_HASHSIZE), buffer + UTIL_SHA256_HASHSIZE);
		memset(buffer + UTIL_SHA256_HASHSIZE, val, bufferLen - UTIL_SHA256_HASHSIZE);


		SHA256_Init(&shctx);
		SHA256_Update(&shctx, buffer + UTIL_SHA256_HASHSIZE, bufferLen - UTIL_SHA256_HASHSIZE);
		SHA256_Final((unsigned char*)buffer, &shctx);
		ILibDuktape_readableStream_WriteData(sender, buffer, (int)bufferLen);
	}
}
duk_ret_t ILibDuktape_dataGenerator_const(duk_context *ctx)
{
	int bufSize = (int)duk_require_int(ctx, 0);
	void *buffer;

	if (bufSize <= UTIL_SHA256_HASHSIZE)
	{
		return(ILibDuktape_Error(ctx, "Value too small. Must be > %d", UTIL_SHA256_HASHSIZE));
	}

	duk_push_object(ctx);
	duk_push_int(ctx, 0); duk_put_prop_string(ctx, -2, "\xFF_counter");
	buffer = Duktape_PushBuffer(ctx, bufSize);
	duk_put_prop_string(ctx, -2, "\xFF_buffer");
	ILibDuktape_ReadableStream_Init(ctx, ILibDuktape_dataGenerator_onPause, ILibDuktape_dataGenerator_onResume, buffer)->paused = 1;
	return(1);
}
void ILibDuktape_dataGenerator_Push(duk_context *ctx, void *chain)
{
	duk_push_c_function(ctx, ILibDuktape_dataGenerator_const, DUK_VARARGS);
}
#endif

void ILibDuktape_Polyfills_JS_Init(duk_context *ctx)
{
	// The following can be overriden by calling addModule() or by having a .js file in the module path

	// http-digest. Refer to /modules/http-digest.js for a human readable version
	duk_peval_string_noresult(ctx, "addModule('http-digest', Buffer.from('LyoNCkNvcHlyaWdodCAyMDE5IEludGVsIENvcnBvcmF0aW9uDQoNCkxpY2Vuc2VkIHVuZGVyIHRoZSBBcGFjaGUgTGljZW5zZSwgVmVyc2lvbiAyLjAgKHRoZSAiTGljZW5zZSIpOw0KeW91IG1heSBub3QgdXNlIHRoaXMgZmlsZSBleGNlcHQgaW4gY29tcGxpYW5jZSB3aXRoIHRoZSBMaWNlbnNlLg0KWW91IG1heSBvYnRhaW4gYSBjb3B5IG9mIHRoZSBMaWNlbnNlIGF0DQoNCiAgICBodHRwOi8vd3d3LmFwYWNoZS5vcmcvbGljZW5zZXMvTElDRU5TRS0yLjANCg0KVW5sZXNzIHJlcXVpcmVkIGJ5IGFwcGxpY2FibGUgbGF3IG9yIGFncmVlZCB0byBpbiB3cml0aW5nLCBzb2Z0d2FyZQ0KZGlzdHJpYnV0ZWQgdW5kZXIgdGhlIExpY2Vuc2UgaXMgZGlzdHJpYnV0ZWQgb24gYW4gIkFTIElTIiBCQVNJUywNCldJVEhPVVQgV0FSUkFOVElFUyBPUiBDT05ESVRJT05TIE9GIEFOWSBLSU5ELCBlaXRoZXIgZXhwcmVzcyBvciBpbXBsaWVkLg0KU2VlIHRoZSBMaWNlbnNlIGZvciB0aGUgc3BlY2lmaWMgbGFuZ3VhZ2UgZ292ZXJuaW5nIHBlcm1pc3Npb25zIGFuZA0KbGltaXRhdGlvbnMgdW5kZXIgdGhlIExpY2Vuc2UuDQoqLw0KDQoNCnZhciB3cml0YWJsZSA9IHJlcXVpcmUoJ3N0cmVhbScpLldyaXRhYmxlOw0KdmFyIG1kNSA9IHJlcXVpcmUoJ01ENVN0cmVhbScpLmNyZWF0ZSgpOw0KDQpmdW5jdGlvbiBjaGVja0V2ZW50Rm9yd2FyZGluZyhkaWdlc3RSZXF1ZXN0LCBldmVudE5hbWUpDQp7DQogICAgaWYgKGRpZ2VzdFJlcXVlc3QubGlzdGVuZXJDb3VudChldmVudE5hbWUpID4gMCkNCiAgICB7DQogICAgICAgIHZhciBlRm9yd2FyZCA9IGZ1bmN0aW9uIF9lRm9yd2FyZCgpDQogICAgICAgIHsNCiAgICAgICAgICAgIHZhciBwID0gW2VGb3J3YXJkLl9ldmVudE5hbWVdOw0KICAgICAgICAgICAgZm9yICh2YXIgaSA9IDA7IGkgPCBhcmd1bWVudHMubGVuZ3RoOyArK2kpIHsgcC5wdXNoKGFyZ3VtZW50c1tpXSk7IH0NCiAgICAgICAgICAgIF9lRm9yd2FyZC5fZGlnZXN0UmVxdWVzdC5lbWl0LmFwcGx5KF9lRm9yd2FyZC5fZGlnZXN0UmVxdWVzdCwgcCk7DQogICAgICAgIH07DQogICAgICAgIGVGb3J3YXJkLl9ldmVudE5hbWUgPSBldmVudE5hbWU7DQogICAgICAgIGVGb3J3YXJkLl9kaWdlc3RSZXF1ZXN0ID0gZGlnZXN0UmVxdWVzdDsNCiAgICAgICAgZGlnZXN0UmVxdWVzdC5fcmVxdWVzdC5vbihldmVudE5hbWUsIGVGb3J3YXJkKTsNCiAgICB9DQp9DQoNCmZ1bmN0aW9uIGdlbmVyYXRlQXV0aEhlYWRlcnMoaW1zZywgb3B0aW9ucywgZGlnZXN0KQ0Kew0KICAgIHZhciBhdXRoOw0KDQogICAgaWYgKGltc2cgIT0gbnVsbCkNCiAgICB7DQogICAgICAgIGF1dGggPSB7IHJlYWxtOiBudWxsLCBub25jZTogbnVsbCwgb3BhcXVlOiBudWxsLCBxb3A6IG51bGwgfTsNCiAgICAgICAgdmFyIHd3dyA9IGltc2cuaGVhZGVyc1snV1dXLUF1dGhlbnRpY2F0ZSddOw0KICAgICAgICB2YXIgdG9rZW5zID0gd3d3LnNwbGl0KCcsJyk7DQoNCiAgICAgICAgdmFyIHBhaXJzOw0KICAgICAgICBmb3IgKHZhciBpIGluIHRva2VucykNCiAgICAgICAgew0KICAgICAgICAgICAgcGFpcnMgPSB0b2tlbnNbaV0uc3BsaXQoJz0nKTsNCiAgICAgICAgICAgIGlmIChwYWlycy5sZW5ndGggPT0gMikNCiAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICBzd2l0Y2ggKHBhaXJzWzBdLnRvTG93ZXJDYXNlKCkudHJpbSgpKQ0KICAgICAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICAgICAgY2FzZSAnZGlnZXN0IHJlYWxtJzoNCiAgICAgICAgICAgICAgICAgICAgICAgIGF1dGgucmVhbG0gPSBwYWlyc1sxXTsNCiAgICAgICAgICAgICAgICAgICAgICAgIGlmIChhdXRoLnJlYWxtWzBdID09ICciJykgeyBhdXRoLnJlYWxtID0gYXV0aC5yZWFsbS5zdWJzdHJpbmcoMSwgYXV0aC5yZWFsbS5sZW5ndGggLSAxKTsgfQ0KICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7DQogICAgICAgICAgICAgICAgICAgIGNhc2UgJ25vbmNlJzoNCiAgICAgICAgICAgICAgICAgICAgICAgIGF1dGgubm9uY2UgPSBwYWlyc1sxXTsNCiAgICAgICAgICAgICAgICAgICAgICAgIGlmIChhdXRoLm5vbmNlWzBdID09ICciJykgeyBhdXRoLm5vbmNlID0gYXV0aC5ub25jZS5zdWJzdHJpbmcoMSwgYXV0aC5ub25jZS5sZW5ndGggLSAxKTsgfQ0KICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7DQogICAgICAgICAgICAgICAgICAgIGNhc2UgJ29wYXF1ZSc6DQogICAgICAgICAgICAgICAgICAgICAgICBhdXRoLm9wYXF1ZSA9IHBhaXJzWzFdOw0KICAgICAgICAgICAgICAgICAgICAgICAgaWYgKGF1dGgub3BhcXVlWzBdID09ICciJykgeyBhdXRoLm9wYXF1ZSA9IGF1dGgub3BhcXVlLnN1YnN0cmluZygxLCBhdXRoLm9wYXF1ZS5sZW5ndGggLSAxKTsgfQ0KICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7DQogICAgICAgICAgICAgICAgICAgIGNhc2UgJ3FvcCc6DQogICAgICAgICAgICAgICAgICAgICAgICBhdXRoLnFvcCA9IHBhaXJzWzFdOw0KICAgICAgICAgICAgICAgICAgICAgICAgaWYgKGF1dGgucW9wWzBdID09ICciJykgeyBhdXRoLnFvcCA9IGF1dGgucW9wLnN1YnN0cmluZygxLCBhdXRoLnFvcC5sZW5ndGggLSAxKTsgfQ0KICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7DQogICAgICAgICAgICAgICAgfQ0KICAgICAgICAgICAgfQ0KICAgICAgICB9DQogICAgICAgIGRpZ2VzdC5fYXV0aCA9IGF1dGg7DQogICAgfQ0KICAgIGVsc2UNCiAgICB7DQogICAgICAgIGlmICghKGF1dGggPSBkaWdlc3QuX2F1dGgpKSB7IHJldHVybjsgfQ0KICAgIH0NCg0KICAgIHZhciBzdGVwMSA9IGRpZ2VzdC5fb3B0aW9ucy51c2VybmFtZSArICc6JyArIGF1dGgucmVhbG0gKyAnOicgKyBkaWdlc3QuX29wdGlvbnMucGFzc3dvcmQ7DQogICAgYXV0aC5zdGVwMSA9IG1kNS5zeW5jSGFzaChzdGVwMSkudG9TdHJpbmcoJ2hleCcpLnRvTG93ZXJDYXNlKCk7DQoNCiAgICB2YXIgc3RlcDIgPSBvcHRpb25zLm1ldGhvZCArICc6JyArIG9wdGlvbnMucGF0aDsNCiAgICBhdXRoLnN0ZXAyID0gbWQ1LnN5bmNIYXNoKHN0ZXAyKS50b1N0cmluZygnaGV4JykudG9Mb3dlckNhc2UoKTsNCg0KDQogICAgaWYgKGF1dGgucW9wID09IG51bGwpDQogICAgew0KICAgICAgICB2YXIgc3RlcDMgPSBhdXRoLnN0ZXAxICsgJzonICsgYXV0aC5ub25jZSArICc6JyArIGF1dGguc3RlcDI7DQogICAgICAgIGF1dGguc3RlcDMgPSBtZDUuc3luY0hhc2goc3RlcDMpLnRvU3RyaW5nKCdoZXgnKS50b0xvd2VyQ2FzZSgpOw0KICAgIH0NCiAgICBlbHNlDQogICAgew0KICAgICAgICBkaWdlc3QuX05DICs9IDE7DQogICAgICAgIHZhciBzdGVwMyA9IGF1dGguc3RlcDEgKyAnOicgKyBhdXRoLm5vbmNlICsgJzonICsgZGlnZXN0Ll9OQy50b1N0cmluZygxNikudG9Mb3dlckNhc2UoKS5wYWRTdGFydCg4LCAnMCcpICsgJzonICsgZGlnZXN0Ll9DTk9OQ0UgKyAnOicgKyBhdXRoLnFvcCArICc6JyArIGF1dGguc3RlcDI7DQogICAgICAgIGF1dGguc3RlcDMgPSBtZDUuc3luY0hhc2goc3RlcDMpLnRvU3RyaW5nKCdoZXgnKS50b0xvd2VyQ2FzZSgpOw0KICAgIH0NCg0KICAgIHZhciByZXQgPSAnRGlnZXN0IHVzZXJuYW1lPSInICsgZGlnZXN0Ll9vcHRpb25zLnVzZXJuYW1lICsgJyIscmVhbG09IicgKyBhdXRoLnJlYWxtICsgJyIsbm9uY2U9IicgKyBhdXRoLm5vbmNlICsgJyIsdXJpPSInICsgb3B0aW9ucy5wYXRoICsgJyInOw0KICAgIGlmIChhdXRoLm9wYXF1ZSAhPSBudWxsKSB7IHJldCArPSAoJyxvcGFxdWU9IicgKyBhdXRoLm9wYXF1ZSArICciJyk7IH0NCiAgICByZXQgKz0gKCcscmVzcG9uc2U9IicgKyBhdXRoLnN0ZXAzICsgJyInKTsNCg0KICAgIGlmIChhdXRoLnFvcCAhPSBudWxsKQ0KICAgIHsNCiAgICAgICAgcmV0ICs9ICgnLHFvcD0iJyArIGF1dGgucW9wICsgJyIsbmM9IicgKyBkaWdlc3QuX05DLnRvU3RyaW5nKDE2KS50b0xvd2VyQ2FzZSgpLnBhZFN0YXJ0KDgsICcwJykgKyAnIixjbm9uY2U9IicgKyBkaWdlc3QuX0NOT05DRSArICciJyk7DQogICAgfQ0KDQoNCiAgICBpZiAoIW9wdGlvbnMuaGVhZGVycykgeyBvcHRpb25zLmhlYWRlcnMgPSB7fTsgfQ0KICAgIG9wdGlvbnMuaGVhZGVyc1snQXV0aG9yaXphdGlvbiddID0gcmV0Ow0KICAgIHJldHVybiAocmV0KTsNCn0NCg0KZnVuY3Rpb24gaHR0cF9kaWdlc3QoKQ0Kew0KICAgIHRoaXMuX09iamVjdElEID0gImh0dHAtZGlnZXN0IjsNCiAgICB0aGlzLmNyZWF0ZSA9IGZ1bmN0aW9uKCkNCiAgICB7DQogICAgICAgIGlmKGFyZ3VtZW50cy5sZW5ndGggPT0gMSAmJiB0eXBlb2YoYXJndW1lbnRzWzBdID09ICdvYmplY3QnKSkNCiAgICAgICAgew0KICAgICAgICAgICAgcmV0dXJuIChuZXcgaHR0cF9kaWdlc3RfaW5zdGFuY2UoYXJndW1lbnRzWzBdKSk7DQogICAgICAgIH0NCiAgICAgICAgaWYoYXJndW1lbnRzLmxlbmd0aCA9PSAyICYmIHR5cGVvZihhcmd1bWVudHNbMF0pID09ICdzdHJpbmcnICYmIHR5cGVvZihhcmd1bWVudHNbMV0pID09ICdzdHJpbmcnKQ0KICAgICAgICB7DQogICAgICAgICAgICByZXR1cm4gKG5ldyBodHRwX2RpZ2VzdF9pbnN0YW5jZSh7dXNlcm5hbWU6IGFyZ3VtZW50c1swXSwgcGFzc3dvcmQ6IGFyZ3VtZW50c1sxXX0pKTsNCiAgICAgICAgfQ0KICAgICAgICB0aHJvdyAoJ0ludmFsaWQgUGFyYW1ldGVycycpOw0KICAgIH0NCn0NCg0KZnVuY3Rpb24gaHR0cF9kaWdlc3RfaW5zdGFuY2Uob3B0aW9ucykNCnsNCiAgICB0aGlzLl9PYmplY3RJRCA9ICdodHRwLWRpZ2VzdC5pbnN0YW5jZSc7DQogICAgdGhpcy5fb3B0aW9ucyA9IG9wdGlvbnM7DQogICAgdGhpcy5odHRwID0gbnVsbDsNCiAgICB0aGlzLl9OQyA9IDA7DQogICAgdGhpcy5fQ05PTkNFID0gcmVxdWlyZSgnaHR0cCcpLmdlbmVyYXRlTm9uY2UoMTYpOw0KDQogICAgdGhpcy5nZXQgPSBmdW5jdGlvbih1cmkpDQogICAgew0KICAgICAgICByZXR1cm4gKHRoaXMucmVxdWVzdCh1cmkpKTsNCiAgICB9DQogICAgdGhpcy5yZXF1ZXN0ID0gZnVuY3Rpb24gKHBhcjEpDQogICAgew0KICAgICAgICB2YXIgY2FsbGVuZCA9IGZhbHNlOw0KICAgICAgICB2YXIgcmV0ID0gbmV3IHdyaXRhYmxlKA0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIHdyaXRlOiBmdW5jdGlvbiAoY2h1bmssIGZsdXNoKQ0KICAgICAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICAgICAgaWYgKHRoaXMuX2VuZGVkKSB7IHRocm93ICgnU3RyZWFtIGFscmVhZHkgZW5kZWQnKTsgfQ0KICAgICAgICAgICAgICAgICAgICBpZighdGhpcy5fYnVmZmVyZWQpIA0KICAgICAgICAgICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgICAgICAgICB0aGlzLl9idWZmZXJlZCA9IEJ1ZmZlci5hbGxvYyhjaHVuay5sZW5ndGgpOw0KICAgICAgICAgICAgICAgICAgICAgICAgY2h1bmsuY29weSh0aGlzLl9idWZmZXJlZCk7DQogICAgICAgICAgICAgICAgICAgIH0NCiAgICAgICAgICAgICAgICAgICAgZWxzZQ0KICAgICAgICAgICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgICAgICAgICB0aGlzLl9idWZmZXJlZCA9IEJ1ZmZlci5jb25jYXQoW3RoaXMuX2J1ZmZlcmVkLCBjaHVua10sIHRoaXMuX2J1ZmZlcmVkLmxlbmd0aCArIGNodW5rLmxlbmd0aCk7DQogICAgICAgICAgICAgICAgICAgIH0NCg0KICAgICAgICAgICAgICAgICAgICBpZiAodGhpcy5fcmVxdWVzdCkgeyB0aGlzLl9yZXF1ZXN0LndyaXRlKGNodW5rKTsgfQ0KICAgICAgICAgICAgICAgICAgICBpZiAoZmx1c2ggIT0gbnVsbCkgeyBmbHVzaCgpOyB9DQogICAgICAgICAgICAgICAgICAgIHJldHVybiAodHJ1ZSk7DQogICAgICAgICAgICAgICAgfSwNCiAgICAgICAgICAgICAgICBmaW5hbDogZnVuY3Rpb24gKGZsdXNoKQ0KICAgICAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICAgICAgaWYgKHRoaXMuX2VuZGVkKSB7IHRocm93ICgnU3RyZWFtIGFscmVhZHkgZW5kZWQnKTsgfQ0KICAgICAgICAgICAgICAgICAgICB0aGlzLl9lbmRlZCA9IHRydWU7DQogICAgICAgICAgICAgICAgICAgIGlmICh0aGlzLl9yZXF1ZXN0KSB7IHRoaXMuX3JlcXVlc3QuZW5kKCk7IH0NCiAgICAgICAgICAgICAgICAgICAgaWYgKGZsdXNoICE9IG51bGwpIHsgZmx1c2goKTsgfQ0KICAgICAgICAgICAgICAgIH0NCiAgICAgICAgICAgIH0pOw0KICAgICAgICByZXQuX2J1ZmZlcmVkID0gbnVsbDsNCiAgICAgICAgcmV0Ll9lbmRlZCA9IGZhbHNlOw0KICAgICAgICBzd2l0Y2ggKHR5cGVvZiAocGFyMSkpDQogICAgICAgIHsNCiAgICAgICAgICAgIGRlZmF1bHQ6DQogICAgICAgICAgICAgICAgdGhyb3cgKCdJbnZhbGlkIFBhcmFtZXRlcicpOw0KICAgICAgICAgICAgICAgIGJyZWFrOw0KICAgICAgICAgICAgY2FzZSAnc3RyaW5nJzoNCiAgICAgICAgICAgICAgICByZXQub3B0aW9ucyA9IHRoaXMuaHR0cC5wYXJzZVVyaShwYXIxKTsNCiAgICAgICAgICAgICAgICBjYWxsZW5kID0gdHJ1ZTsNCiAgICAgICAgICAgICAgICBicmVhazsNCiAgICAgICAgICAgIGNhc2UgJ29iamVjdCc6DQogICAgICAgICAgICAgICAgcmV0Lm9wdGlvbnMgPSBwYXIxOw0KICAgICAgICAgICAgICAgIGJyZWFrOw0KICAgICAgICB9DQogICAgICAgIHJlcXVpcmUoJ2V2ZW50cycpLkV2ZW50RW1pdHRlci5jYWxsKHJldCwgdHJ1ZSkNCiAgICAgICAgICAgIC5jcmVhdGVFdmVudCgncmVzcG9uc2UnKQ0KICAgICAgICAgICAgLmNyZWF0ZUV2ZW50KCdlcnJvcicpDQogICAgICAgICAgICAuY3JlYXRlRXZlbnQoJ3VwZ3JhZGUnKQ0KICAgICAgICAgICAgLmNyZWF0ZUV2ZW50KCdjb250aW51ZScpDQogICAgICAgICAgICAuY3JlYXRlRXZlbnQoJ3RpbWVvdXQnKTsNCiAgICAgICAgcmV0Ll9kaWdlc3QgPSB0aGlzOw0KDQogICAgICAgIGlmIChhcmd1bWVudHMubGVuZ3RoID4gMSAmJiB0eXBlb2YgKGFyZ3VtZW50c1sxXSkgPT0gJ2Z1bmN0aW9uJykNCiAgICAgICAgew0KICAgICAgICAgICAgcmV0Lm9uY2UoJ3Jlc3BvbnNlJywgYXJndW1lbnRzWzFdKTsNCiAgICAgICAgfQ0KDQogICAgICAgIC8vDQogICAgICAgIC8vIENoZWNrIGlmIHdlIGNhbiBhZGQgQXV0aEhlYWRlcnMgbm93DQogICAgICAgIC8vDQogICAgICAgIGdlbmVyYXRlQXV0aEhlYWRlcnMobnVsbCwgcmV0Lm9wdGlvbnMsIHRoaXMpOw0KDQogICAgICAgIC8vIFdoZW4gc29tZWJvZHkgaG9va3MgdXAgZXZlbnRzIHRvIGRpZ2VzdC5jbGllbnRSZXF1ZXN0LCB3ZSBuZWVkIHRvIGhvb2sgdGhlIHJlYWwgZXZlbnQgb24gaHR0cC5jbGllbnRSZXF1ZXN0DQogICAgICAgIHJldC5fcmVxdWVzdCA9IHRoaXMuaHR0cC5yZXF1ZXN0KHJldC5vcHRpb25zKTsNCiAgICAgICAgcmV0Ll9yZXF1ZXN0LmRpZ1JlcXVlc3QgPSByZXQ7DQogICAgICAgIHJldC5vbignbmV3TGlzdGVuZXInLCBmdW5jdGlvbiAoZXZOYW1lLCBjYWxsYmFjaykNCiAgICAgICAgew0KICAgICAgICAgICAgaWYgKGV2TmFtZSAhPSAndXBncmFkZScgJiYgZXZOYW1lICE9ICdlcnJvcicgJiYgZXZOYW1lICE9ICdjb250aW51ZScgJiYgZXZOYW1lICE9ICd0aW1lb3V0JyAmJiBldk5hbWUgIT0gJ2RyYWluJykgeyByZXR1cm47IH0NCiAgICAgICAgICAgIGlmICh0aGlzLl9yZXF1ZXN0Lmxpc3RlbmVyQ291bnQoZXZOYW1lKSA9PSAwKQ0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIHZhciBldlNpbmsgPSBmdW5jdGlvbiBfZXZTaW5rKCkNCiAgICAgICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgICAgIHZhciBwYXJtcyA9IFtfZXZTaW5rLmV2ZW50TmFtZV07DQogICAgICAgICAgICAgICAgICAgIGZvcih2YXIgaT0wO2k8YXJndW1lbnRzLmxlbmd0aDsrK2kpIHtwYXJtcy5wdXNoKGFyZ3VtZW50c1tpXSk7fQ0KICAgICAgICAgICAgICAgICAgICB0aGlzLmRpZ1JlcXVlc3QuZW1pdC5hcHBseSh0aGlzLmRpZ1JlcXVlc3QsIHBhcm1zKTsNCiAgICAgICAgICAgICAgICB9Ow0KICAgICAgICAgICAgICAgIGV2U2luay5ldmVudE5hbWUgPSBldk5hbWU7DQogICAgICAgICAgICAgICAgdGhpcy5fcmVxdWVzdC5vbihldk5hbWUsIGV2U2luayk7DQogICAgICAgICAgICB9DQogICAgICAgIH0pOw0KDQogICAgICAgIHJldC5fcmVxdWVzdC5vbmNlKCdyZXNwb25zZScsIGZ1bmN0aW9uIChpbXNnKQ0KICAgICAgICB7DQogICAgICAgICAgICBpZiAoaW1zZy5zdGF0dXNDb2RlID09IDQwMSkNCiAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICB2YXIgY2FsbGVuZCA9IHRoaXMuZGlnUmVxdWVzdC5fcmVxdWVzdC5fY2FsbGVuZDsNCiAgICAgICAgICAgICAgICB2YXIgYXV0aCA9IGdlbmVyYXRlQXV0aEhlYWRlcnMoaW1zZywgdGhpcy5kaWdSZXF1ZXN0Lm9wdGlvbnMsIHRoaXMuZGlnUmVxdWVzdC5fZGlnZXN0KTsNCg0KICAgICAgICAgICAgICAgIHRoaXMuZGlnUmVxdWVzdC5fcmVxdWVzdCA9IHRoaXMuZGlnUmVxdWVzdC5fZGlnZXN0Lmh0dHAucmVxdWVzdCh0aGlzLmRpZ1JlcXVlc3Qub3B0aW9ucyk7DQogICAgICAgICAgICAgICAgdGhpcy5kaWdSZXF1ZXN0Ll9yZXF1ZXN0LmRpZ1JlcXVlc3QgPSB0aGlzLmRpZ1JlcXVlc3Q7DQogICAgICAgICAgICAgICAgdGhpcy5kaWdSZXF1ZXN0Ll9yZXF1ZXN0Lm9uY2UoJ3Jlc3BvbnNlJywgZnVuY3Rpb24gKGltc2cpDQogICAgICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgICAgICBzd2l0Y2goaW1zZy5zdGF0dXNDb2RlKQ0KICAgICAgICAgICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgICAgICAgICBjYXNlIDQwMToNCiAgICAgICAgICAgICAgICAgICAgICAgICAgICB0aGlzLmRpZ1JlcXVlc3QuZW1pdCgnZXJyb3InLCAnRGlnZXN0IGZhaWxlZCB0b28gbWFueSB0aW1lcycpOw0KICAgICAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOw0KICAgICAgICAgICAgICAgICAgICAgICAgZGVmYXVsdDoNCiAgICAgICAgICAgICAgICAgICAgICAgICAgICB0aGlzLmRpZ1JlcXVlc3QuZW1pdCgncmVzcG9uc2UnLCBpbXNnKTsNCiAgICAgICAgICAgICAgICAgICAgICAgICAgICBicmVhazsNCiAgICAgICAgICAgICAgICAgICAgfQ0KICAgICAgICAgICAgICAgIH0pOw0KICAgICAgICAgICAgICAgIGNoZWNrRXZlbnRGb3J3YXJkaW5nKHRoaXMuZGlnUmVxdWVzdCwgJ3VwZ3JhZGUnKTsNCiAgICAgICAgICAgICAgICBjaGVja0V2ZW50Rm9yd2FyZGluZyh0aGlzLmRpZ1JlcXVlc3QsICdlcnJvcicpOw0KICAgICAgICAgICAgICAgIGNoZWNrRXZlbnRGb3J3YXJkaW5nKHRoaXMuZGlnUmVxdWVzdCwgJ2NvbnRpbnVlJyk7DQogICAgICAgICAgICAgICAgY2hlY2tFdmVudEZvcndhcmRpbmcodGhpcy5kaWdSZXF1ZXN0LCAndGltZW91dCcpOw0KICAgICAgICAgICAgICAgIGNoZWNrRXZlbnRGb3J3YXJkaW5nKHRoaXMuZGlnUmVxdWVzdCwgJ2RyYWluJyk7DQogICAgICAgICAgICAgICAgaWYgKGNhbGxlbmQpDQogICAgICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgICAgICB0aGlzLmRpZ1JlcXVlc3QuX3JlcXVlc3QuZW5kKCk7DQogICAgICAgICAgICAgICAgfQ0KICAgICAgICAgICAgICAgIGVsc2UNCiAgICAgICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgICAgIGlmICh0aGlzLmRpZ1JlcXVlc3QuX2J1ZmZlcmVkKSB7IHRoaXMuZGlnUmVxdWVzdC5fcmVxdWVzdC53cml0ZSh0aGlzLmRpZ1JlcXVlc3QuX2J1ZmZlcmVkKTsgfQ0KICAgICAgICAgICAgICAgICAgICBpZiAodGhpcy5kaWdSZXF1ZXN0Ll9lbmRlZCkgeyB0aGlzLmRpZ1JlcXVlc3QuX3JlcXVlc3QuZW5kKCk7IH0NCiAgICAgICAgICAgICAgICB9DQogICAgICAgICAgICB9DQogICAgICAgICAgICBlbHNlDQogICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgdGhpcy5kaWdSZXF1ZXN0LmVtaXQoJ3Jlc3BvbnNlJywgaW1zZyk7DQogICAgICAgICAgICB9DQogICAgICAgIH0pOw0KICAgICAgICBpZiAoY2FsbGVuZCkNCiAgICAgICAgew0KICAgICAgICAgICAgcmV0Ll9yZXF1ZXN0Ll9jYWxsZW5kID0gdHJ1ZTsgcmV0Ll9yZXF1ZXN0LmVuZCgpOw0KICAgICAgICB9DQogICAgICAgIGVsc2UNCiAgICAgICAgew0KICAgICAgICAgICAgaWYgKHJldC5fYnVmZmVyZWQpIHsgcmV0Ll9yZXF1ZXN0LndyaXRlKHJldC5fYnVmZmVyZWQpOyB9DQogICAgICAgICAgICBpZiAocmV0Ll9lbmRlZCkgeyByZXQuX3JlcXVlc3QuZW5kKCk7IH0NCiAgICAgICAgfQ0KICAgICAgICByZXR1cm4gKHJldCk7DQogICAgfTsNCn0NCg0KDQptb2R1bGUuZXhwb3J0cyA9IG5ldyBodHRwX2RpZ2VzdCgpOw0KDQo=', 'base64').toString());");

	// Clipboard. Refer to /modules/clipboard.js for a human readable version
	duk_peval_string_noresult(ctx, "addModule('clipboard', Buffer.from('LyoKQ29weXJpZ2h0IDIwMTkgSW50ZWwgQ29ycG9yYXRpb24KCkxpY2Vuc2VkIHVuZGVyIHRoZSBBcGFjaGUgTGljZW5zZSwgVmVyc2lvbiAyLjAgKHRoZSAiTGljZW5zZSIpOwp5b3UgbWF5IG5vdCB1c2UgdGhpcyBmaWxlIGV4Y2VwdCBpbiBjb21wbGlhbmNlIHdpdGggdGhlIExpY2Vuc2UuCllvdSBtYXkgb2J0YWluIGEgY29weSBvZiB0aGUgTGljZW5zZSBhdAoKICAgIGh0dHA6Ly93d3cuYXBhY2hlLm9yZy9saWNlbnNlcy9MSUNFTlNFLTIuMAoKVW5sZXNzIHJlcXVpcmVkIGJ5IGFwcGxpY2FibGUgbGF3IG9yIGFncmVlZCB0byBpbiB3cml0aW5nLCBzb2Z0d2FyZQpkaXN0cmlidXRlZCB1bmRlciB0aGUgTGljZW5zZSBpcyBkaXN0cmlidXRlZCBvbiBhbiAiQVMgSVMiIEJBU0lTLApXSVRIT1VUIFdBUlJBTlRJRVMgT1IgQ09ORElUSU9OUyBPRiBBTlkgS0lORCwgZWl0aGVyIGV4cHJlc3Mgb3IgaW1wbGllZC4KU2VlIHRoZSBMaWNlbnNlIGZvciB0aGUgc3BlY2lmaWMgbGFuZ3VhZ2UgZ292ZXJuaW5nIHBlcm1pc3Npb25zIGFuZApsaW1pdGF0aW9ucyB1bmRlciB0aGUgTGljZW5zZS4KKi8KCnZhciBwcm9taXNlID0gcmVxdWlyZSgncHJvbWlzZScpOwoKZnVuY3Rpb24gbmF0aXZlQWRkTW9kdWxlKG5hbWUpCnsKICAgIHZhciB2YWx1ZSA9IGdldEpTTW9kdWxlKG5hbWUpOwogICAgdmFyIHJldCA9ICJkdWtfcGV2YWxfc3RyaW5nX25vcmVzdWx0KGN0eCwgXCJhZGRNb2R1bGUoJyIgKyBuYW1lICsgIicsIEJ1ZmZlci5mcm9tKCciICsgQnVmZmVyLmZyb20odmFsdWUpLnRvU3RyaW5nKCdiYXNlNjQnKSArICInLCAnYmFzZTY0JykudG9TdHJpbmcoKSk7XCIpOyI7CiAgICBpZiAocmV0Lmxlbmd0aCA+IDE2MzAwKQogICAgew0KICAgICAgICAvLyBNUyBWaXN1YWwgU3R1ZGlvIGhhcyBhIG1heHNpemUgbGltaXRhdGlvbg0KICAgICAgICB2YXIgdG1wID0gQnVmZmVyLmZyb20odmFsdWUpLnRvU3RyaW5nKCdiYXNlNjQnKTsNCiAgICAgICAgcmV0ID0gJ2NoYXIgKl8nICsgbmFtZS5zcGxpdCgnLScpLmpvaW4oJycpICsgJyA9IElMaWJNZW1vcnlfQWxsb2NhdGUoJyArICh0bXAubGVuZ3RoICsgdmFsdWUubGVuZ3RoICsgMikgKyAnLCAwLCBOVUxMLCBOVUxMKTtcbic7DQogICAgICAgIHZhciBpID0gMDsNCiAgICAgICAgd2hpbGUgKGkgPCB0bXAubGVuZ3RoKQ0KICAgICAgICB7DQogICAgICAgICAgICB2YXIgY2h1bmsgPSB0bXAuc3Vic3RyaW5nKGksIGkrMTYwMDApOw0KICAgICAgICAgICAgcmV0ICs9ICgnbWVtY3B5X3MoXycgKyBuYW1lLnNwbGl0KCctJykuam9pbignJykgKyAnICsgJyArIGkgKyAnLCAnICsgKHRtcC5sZW5ndGggLSBpKSArICcsICInICsgY2h1bmsgKyAnIiwgJyArIGNodW5rLmxlbmd0aCArICcpO1xuJyk7DQogICAgICAgICAgICBpICs9IGNodW5rLmxlbmd0aDsNCiAgICAgICAgfQ0KICAgICAgICByZXQgKz0gKCdJTGliQmFzZTY0RGVjb2RlRXgoXycgKyBuYW1lLnNwbGl0KCctJykuam9pbignJykgKyAnLCAnICsgdG1wLmxlbmd0aCArICcsIF8nICsgbmFtZS5zcGxpdCgnLScpLmpvaW4oJycpICsgJyArICcgKyB0bXAubGVuZ3RoICsgJyk7XG4nKTsNCiAgICAgICAgcmV0ICs9ICgnZHVrX3B1c2hfZ2xvYmFsX29iamVjdChjdHgpO2R1a19nZXRfcHJvcF9zdHJpbmcoY3R4LCAtMSwgImFkZE1vZHVsZSIpO2R1a19zd2FwX3RvcChjdHgsIC0yKTtkdWtfcHVzaF9zdHJpbmcoY3R4LCAiJyArIG5hbWUgKyAnIik7ZHVrX3B1c2hfc3RyaW5nKGN0eCwgXycgKyBuYW1lLnNwbGl0KCctJykuam9pbignJykgKyAnICsgJyArIHRtcC5sZW5ndGggKyAnKTtcbicpOw0KICAgICAgICByZXQgKz0gKCdkdWtfcGNhbGxfbWV0aG9kKGN0eCwgMik7IGR1a19wb3AoY3R4KTtcbicpOw0KICAgICAgICByZXQgKz0gKCdmcmVlKF8nICsgbmFtZS5zcGxpdCgnLScpLmpvaW4oJycpICsgJyk7XG4nKTsNCiAgICB9CiAgICBtb2R1bGUuZXhwb3J0cyhyZXQpOwp9CgpmdW5jdGlvbiBsaW5fcmVhZHRleHQoKQp7CiAgICB2YXIgcmV0ID0gbmV3IHByb21pc2UoZnVuY3Rpb24gKHJlcywgcmVqKSB7IHRoaXMuX3JlcyA9IHJlczsgdGhpcy5fcmVqID0gcmVqOyB9KTsKICAgIHRyeQogICAgewogICAgICAgIHJlcXVpcmUoJ21vbml0b3ItaW5mbycpCiAgICB9CiAgICBjYXRjaChleGMpCiAgICB7CiAgICAgICAgcmV0Ll9yZWooZXhjKTsKICAgICAgICByZXR1cm4gKHJldCk7CiAgICB9CgogICAgdmFyIFgxMSA9IHJlcXVpcmUoJ21vbml0b3ItaW5mbycpLl9YMTE7CiAgICBpZiAoIVgxMSkKICAgIHsKICAgICAgICByZXQuX3JlaignWDExIHJlcXVpcmVkIGZvciBDbGlwYm9hcmQgTWFuaXB1bGF0aW9uJyk7CiAgICB9CiAgICBlbHNlCiAgICB7CiAgICAgICAgdmFyIFNlbGVjdGlvbk5vdGlmeSA9IDMxOwogICAgICAgIHZhciBBbnlQcm9wZXJ0eVR5cGUgPSAwOwogICAgICAgIHZhciBHTSA9IHJlcXVpcmUoJ21vbml0b3ItaW5mbycpLl9nbTsKCiAgICAgICAgcmV0Ll9nZXRJbmZvUHJvbWlzZSA9IHJlcXVpcmUoJ21vbml0b3ItaW5mbycpLmdldEluZm8oKTsKICAgICAgICByZXQuX2dldEluZm9Qcm9taXNlLl9tYXN0ZXJQcm9taXNlID0gcmV0OwogICAgICAgIHJldC5fZ2V0SW5mb1Byb21pc2UudGhlbihmdW5jdGlvbiAobW9uKQogICAgICAgIHsKICAgICAgICAgICAgaWYgKG1vbi5sZW5ndGggPiAwKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICB2YXIgd2hpdGUgPSBYMTEuWFdoaXRlUGl4ZWwobW9uWzBdLmRpc3BsYXksIG1vblswXS5zY3JlZW5JZCkuVmFsOwoKICAgICAgICAgICAgICAgIHRoaXMuX21hc3RlclByb21pc2UuQ0xJUElEID0gWDExLlhJbnRlcm5BdG9tKG1vblswXS5kaXNwbGF5LCBHTS5DcmVhdGVWYXJpYWJsZSgnQ0xJUEJPQVJEJyksIDApOwogICAgICAgICAgICAgICAgdGhpcy5fbWFzdGVyUHJvbWlzZS5GTVRJRCA9IFgxMS5YSW50ZXJuQXRvbShtb25bMF0uZGlzcGxheSwgR00uQ3JlYXRlVmFyaWFibGUoJ1VURjhfU1RSSU5HJyksIDApOwogICAgICAgICAgICAgICAgdGhpcy5fbWFzdGVyUHJvbWlzZS5QUk9QSUQgPSBYMTEuWEludGVybkF0b20obW9uWzBdLmRpc3BsYXksIEdNLkNyZWF0ZVZhcmlhYmxlKCdYU0VMX0RBVEEnKSwgMCk7CiAgICAgICAgICAgICAgICB0aGlzLl9tYXN0ZXJQcm9taXNlLklOQ1JJRCA9IFgxMS5YSW50ZXJuQXRvbShtb25bMF0uZGlzcGxheSwgR00uQ3JlYXRlVmFyaWFibGUoJ0lOQ1InKSwgMCk7CiAgICAgICAgICAgICAgICB0aGlzLl9tYXN0ZXJQcm9taXNlLlJPT1RXSU4gPSBYMTEuWFJvb3RXaW5kb3cobW9uWzBdLmRpc3BsYXksIG1vblswXS5zY3JlZW5JZCk7CiAgICAgICAgICAgICAgICB0aGlzLl9tYXN0ZXJQcm9taXNlLkZBS0VXSU4gPSBYMTEuWENyZWF0ZVNpbXBsZVdpbmRvdyhtb25bMF0uZGlzcGxheSwgdGhpcy5fbWFzdGVyUHJvbWlzZS5ST09UV0lOLCAwLCAwLCBtb25bMF0ucmlnaHQsIDUsIDAsIHdoaXRlLCB3aGl0ZSk7CgogICAgICAgICAgICAgICAgWDExLlhTeW5jKG1vblswXS5kaXNwbGF5LCAwKTsKICAgICAgICAgICAgICAgIFgxMS5YQ29udmVydFNlbGVjdGlvbihtb25bMF0uZGlzcGxheSwgdGhpcy5fbWFzdGVyUHJvbWlzZS5DTElQSUQsIHRoaXMuX21hc3RlclByb21pc2UuRk1USUQsIHRoaXMuX21hc3RlclByb21pc2UuUFJPUElELCB0aGlzLl9tYXN0ZXJQcm9taXNlLkZBS0VXSU4sIDApOwogICAgICAgICAgICAgICAgWDExLlhTeW5jKG1vblswXS5kaXNwbGF5LCAwKTsKCiAgICAgICAgICAgICAgICB0aGlzLl9tYXN0ZXJQcm9taXNlLkRlc2NyaXB0b3JFdmVudCA9IHJlcXVpcmUoJ0Rlc2NyaXB0b3JFdmVudHMnKS5hZGREZXNjcmlwdG9yKFgxMS5YQ29ubmVjdGlvbk51bWJlcihtb25bMF0uZGlzcGxheSkuVmFsLCB7IHJlYWRzZXQ6IHRydWUgfSk7CiAgICAgICAgICAgICAgICB0aGlzLl9tYXN0ZXJQcm9taXNlLkRlc2NyaXB0b3JFdmVudC5fbWFzdGVyUHJvbWlzZSA9IHRoaXMuX21hc3RlclByb21pc2U7CiAgICAgICAgICAgICAgICB0aGlzLl9tYXN0ZXJQcm9taXNlLkRlc2NyaXB0b3JFdmVudC5fZGlzcGxheSA9IG1vblswXS5kaXNwbGF5OwogICAgICAgICAgICAgICAgdGhpcy5fbWFzdGVyUHJvbWlzZS5EZXNjcmlwdG9yRXZlbnQub24oJ3JlYWRzZXQnLCBmdW5jdGlvbiAoZmQpCiAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgdmFyIFhFID0gR00uQ3JlYXRlVmFyaWFibGUoMTAyNCk7CiAgICAgICAgICAgICAgICAgICAgd2hpbGUgKFgxMS5YUGVuZGluZyh0aGlzLl9kaXNwbGF5KS5WYWwpCiAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICBYMTEuWE5leHRFdmVudFN5bmModGhpcy5fZGlzcGxheSwgWEUpOwogICAgICAgICAgICAgICAgICAgICAgICBpZihYRS5EZXJlZigwLCA0KS50b0J1ZmZlcigpLnJlYWRVSW50MzJMRSgpID09IFNlbGVjdGlvbk5vdGlmeSkKICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgdmFyIGlkID0gR00uQ3JlYXRlUG9pbnRlcigpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgdmFyIGJpdHMgPSBHTS5DcmVhdGVQb2ludGVyKCk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB2YXIgc3ogPSBHTS5DcmVhdGVQb2ludGVyKCk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB2YXIgdGFpbCA9IEdNLkNyZWF0ZVBvaW50ZXIoKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHZhciByZXN1bHQgPSBHTS5DcmVhdGVQb2ludGVyKCk7CgogICAgICAgICAgICAgICAgICAgICAgICAgICAgWDExLlhHZXRXaW5kb3dQcm9wZXJ0eSh0aGlzLl9kaXNwbGF5LCB0aGlzLl9tYXN0ZXJQcm9taXNlLkZBS0VXSU4sIHRoaXMuX21hc3RlclByb21pc2UuUFJPUElELCAwLCA2NTUzNSwgMCwgQW55UHJvcGVydHlUeXBlLCBpZCwgYml0cywgc3osIHRhaWwsIHJlc3VsdCk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB0aGlzLl9tYXN0ZXJQcm9taXNlLl9yZXMocmVzdWx0LkRlcmVmKCkuU3RyaW5nKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIFgxMS5YRnJlZShyZXN1bHQuRGVyZWYoKSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBYMTEuWERlc3Ryb3lXaW5kb3codGhpcy5fZGlzcGxheSwgdGhpcy5fbWFzdGVyUHJvbWlzZS5GQUtFV0lOKTsKCiAgICAgICAgICAgICAgICAgICAgICAgICAgICB0aGlzLnJlbW92ZURlc2NyaXB0b3IoZmQpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICB9KTsKICAgICAgICAgICAgfQogICAgICAgIH0pOwogICAgfQogICAgcmV0dXJuIChyZXQpOwp9CmZ1bmN0aW9uIGxpbl9jb3B5dGV4dCgpCnsKfQoKZnVuY3Rpb24gd2luX3JlYWR0ZXh0KCkKewogICAgdmFyIHJldCA9ICcnOwogICAgdmFyIENGX1RFWFQgPSAxOwogICAgdmFyIEdNID0gcmVxdWlyZSgnX0dlbmVyaWNNYXJzaGFsJyk7CiAgICB2YXIgdXNlcjMyID0gR00uQ3JlYXRlTmF0aXZlUHJveHkoJ3VzZXIzMi5kbGwnKTsKICAgIHZhciBrZXJuZWwzMiA9IEdNLkNyZWF0ZU5hdGl2ZVByb3h5KCdrZXJuZWwzMi5kbGwnKTsKICAgIGtlcm5lbDMyLkNyZWF0ZU1ldGhvZCgnR2xvYmFsQWxsb2MnKTsKICAgIGtlcm5lbDMyLkNyZWF0ZU1ldGhvZCgnR2xvYmFsTG9jaycpOwogICAga2VybmVsMzIuQ3JlYXRlTWV0aG9kKCdHbG9iYWxVbmxvY2snKTsKICAgIHVzZXIzMi5DcmVhdGVNZXRob2QoJ09wZW5DbGlwYm9hcmQnKTsKICAgIHVzZXIzMi5DcmVhdGVNZXRob2QoJ0Nsb3NlQ2xpcGJvYXJkJyk7CiAgICB1c2VyMzIuQ3JlYXRlTWV0aG9kKCdHZXRDbGlwYm9hcmREYXRhJyk7CgogICAgdXNlcjMyLk9wZW5DbGlwYm9hcmQoMCk7CiAgICB2YXIgaCA9IHVzZXIzMi5HZXRDbGlwYm9hcmREYXRhKENGX1RFWFQpOwogICAgaWYoaC5WYWwhPTApCiAgICB7CiAgICAgICAgdmFyIGhidWZmZXIgPSBrZXJuZWwzMi5HbG9iYWxMb2NrKGgpOwogICAgICAgIHJldCA9IGhidWZmZXIuU3RyaW5nOwogICAgICAgIGtlcm5lbDMyLkdsb2JhbFVubG9jayhoKTsKICAgIH0KICAgIHVzZXIzMi5DbG9zZUNsaXBib2FyZCgpOwoKICAgIHZhciBwID0gbmV3IHByb21pc2UoZnVuY3Rpb24gKHJlcywgcmVqKSB7IHRoaXMuX3JlcyA9IHJlczsgdGhpcy5fcmVqID0gcmVqOyB9KTsKICAgIHAuX3JlcyhyZXQpOwogICAgcmV0dXJuIChwKTsKfQoKZnVuY3Rpb24gd2luX2NvcHl0ZXh0KHR4dCkKewogICAgdmFyIEdNRU1fTU9WRUFCTEUgPSAweDAwMDI7CiAgICB2YXIgQ0ZfVEVYVCA9IDE7CgogICAgdmFyIEdNID0gcmVxdWlyZSgnX0dlbmVyaWNNYXJzaGFsJyk7CiAgICB2YXIgdXNlcjMyID0gR00uQ3JlYXRlTmF0aXZlUHJveHkoJ3VzZXIzMi5kbGwnKTsKICAgIHZhciBrZXJuZWwzMiA9IEdNLkNyZWF0ZU5hdGl2ZVByb3h5KCdrZXJuZWwzMi5kbGwnKTsKICAgIGtlcm5lbDMyLkNyZWF0ZU1ldGhvZCgnR2xvYmFsQWxsb2MnKTsKICAgIGtlcm5lbDMyLkNyZWF0ZU1ldGhvZCgnR2xvYmFsTG9jaycpOwogICAga2VybmVsMzIuQ3JlYXRlTWV0aG9kKCdHbG9iYWxVbmxvY2snKTsKICAgIHVzZXIzMi5DcmVhdGVNZXRob2QoJ09wZW5DbGlwYm9hcmQnKTsKICAgIHVzZXIzMi5DcmVhdGVNZXRob2QoJ0VtcHR5Q2xpcGJvYXJkJyk7CiAgICB1c2VyMzIuQ3JlYXRlTWV0aG9kKCdDbG9zZUNsaXBib2FyZCcpOwogICAgdXNlcjMyLkNyZWF0ZU1ldGhvZCgnU2V0Q2xpcGJvYXJkRGF0YScpOwoKICAgIHZhciBoID0ga2VybmVsMzIuR2xvYmFsQWxsb2MoR01FTV9NT1ZFQUJMRSwgdHh0Lmxlbmd0aCArIDIpOwogICAgaC5hdXRvRnJlZShmYWxzZSk7CiAgICB2YXIgaGJ1ZmZlciA9IGtlcm5lbDMyLkdsb2JhbExvY2soaCk7CiAgICBoYnVmZmVyLmF1dG9GcmVlKGZhbHNlKTsKICAgIHZhciB0bXAgPSBCdWZmZXIuYWxsb2ModHh0Lmxlbmd0aCArIDEpOwogICAgQnVmZmVyLmZyb20odHh0KS5jb3B5KHRtcCk7CiAgICB0bXAuY29weShoYnVmZmVyLkRlcmVmKDAsIHR4dC5sZW5ndGggKyAxKS50b0J1ZmZlcigpKTsKICAgIGtlcm5lbDMyLkdsb2JhbFVubG9jayhoKTsKCiAgICB1c2VyMzIuT3BlbkNsaXBib2FyZCgwKTsKICAgIHVzZXIzMi5FbXB0eUNsaXBib2FyZCgpOwogICAgdXNlcjMyLlNldENsaXBib2FyZERhdGEoQ0ZfVEVYVCwgaCk7CiAgICB1c2VyMzIuQ2xvc2VDbGlwYm9hcmQoKTsKfQoKc3dpdGNoKHByb2Nlc3MucGxhdGZvcm0pCnsKICAgIGNhc2UgJ3dpbjMyJzoKICAgICAgICBtb2R1bGUuZXhwb3J0cyA9IHdpbl9jb3B5dGV4dDsKICAgICAgICBtb2R1bGUuZXhwb3J0cy5yZWFkID0gd2luX3JlYWR0ZXh0OwogICAgICAgIGJyZWFrOwogICAgY2FzZSAnbGludXgnOgogICAgICAgIG1vZHVsZS5leHBvcnRzID0gbGluX2NvcHl0ZXh0OwogICAgICAgIG1vZHVsZS5leHBvcnRzLnJlYWQgPSBsaW5fcmVhZHRleHQ7CiAgICAgICAgYnJlYWs7CiAgICBjYXNlICdkYXJ3aW4nOgogICAgICAgIGJyZWFrOwp9Cm1vZHVsZS5leHBvcnRzLm5hdGl2ZUFkZE1vZHVsZSA9IG5hdGl2ZUFkZE1vZHVsZTs=', 'base64').toString());");
	
	// Promise: This is very important, as it is used everywhere. Refer to /modules/promise.js to see a human readable version of promise.js
	duk_peval_string_noresult(ctx, "addModule('promise', Buffer.from('LyoNCkNvcHlyaWdodCAyMDE4IEludGVsIENvcnBvcmF0aW9uDQoNCkxpY2Vuc2VkIHVuZGVyIHRoZSBBcGFjaGUgTGljZW5zZSwgVmVyc2lvbiAyLjAgKHRoZSAiTGljZW5zZSIpOw0KeW91IG1heSBub3QgdXNlIHRoaXMgZmlsZSBleGNlcHQgaW4gY29tcGxpYW5jZSB3aXRoIHRoZSBMaWNlbnNlLg0KWW91IG1heSBvYnRhaW4gYSBjb3B5IG9mIHRoZSBMaWNlbnNlIGF0DQoNCiAgICBodHRwOi8vd3d3LmFwYWNoZS5vcmcvbGljZW5zZXMvTElDRU5TRS0yLjANCg0KVW5sZXNzIHJlcXVpcmVkIGJ5IGFwcGxpY2FibGUgbGF3IG9yIGFncmVlZCB0byBpbiB3cml0aW5nLCBzb2Z0d2FyZQ0KZGlzdHJpYnV0ZWQgdW5kZXIgdGhlIExpY2Vuc2UgaXMgZGlzdHJpYnV0ZWQgb24gYW4gIkFTIElTIiBCQVNJUywNCldJVEhPVVQgV0FSUkFOVElFUyBPUiBDT05ESVRJT05TIE9GIEFOWSBLSU5ELCBlaXRoZXIgZXhwcmVzcyBvciBpbXBsaWVkLg0KU2VlIHRoZSBMaWNlbnNlIGZvciB0aGUgc3BlY2lmaWMgbGFuZ3VhZ2UgZ292ZXJuaW5nIHBlcm1pc3Npb25zIGFuZA0KbGltaXRhdGlvbnMgdW5kZXIgdGhlIExpY2Vuc2UuDQoqLw0KDQp2YXIgcmVmVGFibGUgPSB7fTsNCg0KZnVuY3Rpb24gZXZlbnRfc3dpdGNoZXJfaGVscGVyKGRlc2lyZWRfY2FsbGVlLCB0YXJnZXQpDQp7DQogICAgdGhpcy5fT2JqZWN0SUQgPSAnZXZlbnRfc3dpdGNoZXInOw0KICAgIHRoaXMuZnVuYyA9IGZ1bmN0aW9uIGZ1bmMoKQ0KICAgIHsNCiAgICAgICAgdmFyIGFyZ3MgPSBbXTsNCiAgICAgICAgZm9yKHZhciBpIGluIGFyZ3VtZW50cykNCiAgICAgICAgew0KICAgICAgICAgICAgYXJncy5wdXNoKGFyZ3VtZW50c1tpXSk7DQogICAgICAgIH0NCiAgICAgICAgcmV0dXJuIChmdW5jLnRhcmdldC5hcHBseShmdW5jLmRlc2lyZWQsIGFyZ3MpKTsNCiAgICB9Ow0KICAgIHRoaXMuZnVuYy5kZXNpcmVkID0gZGVzaXJlZF9jYWxsZWU7DQogICAgdGhpcy5mdW5jLnRhcmdldCA9IHRhcmdldDsNCiAgICB0aGlzLmZ1bmMuc2VsZiA9IHRoaXM7DQp9DQpmdW5jdGlvbiBldmVudF9zd2l0Y2hlcihkZXNpcmVkX2NhbGxlZSwgdGFyZ2V0KQ0Kew0KICAgIHJldHVybiAobmV3IGV2ZW50X3N3aXRjaGVyX2hlbHBlcihkZXNpcmVkX2NhbGxlZSwgdGFyZ2V0KSk7DQp9DQoNCmZ1bmN0aW9uIFByb21pc2UocHJvbWlzZUZ1bmMpDQp7DQogICAgdGhpcy5fT2JqZWN0SUQgPSAncHJvbWlzZSc7DQogICAgdGhpcy5wcm9taXNlID0gdGhpczsNCiAgICB0aGlzLl9pbnRlcm5hbCA9IHsgX09iamVjdElEOiAncHJvbWlzZS5pbnRlcm5hbCcsIHByb21pc2U6IHRoaXMsIGZ1bmM6IHByb21pc2VGdW5jLCBjb21wbGV0ZWQ6IGZhbHNlLCBlcnJvcnM6IGZhbHNlLCBjb21wbGV0ZWRBcmdzOiBbXSB9Ow0KICAgIHJlcXVpcmUoJ2V2ZW50cycpLkV2ZW50RW1pdHRlci5jYWxsKHRoaXMuX2ludGVybmFsKTsNCiAgICB0aGlzLl9pbnRlcm5hbC5vbignbmV3TGlzdGVuZXInLCBmdW5jdGlvbiAoZXZlbnROYW1lLCBldmVudENhbGxiYWNrKQ0KICAgIHsNCiAgICAgICAgLy9jb25zb2xlLmxvZygnbmV3TGlzdGVuZXInLCBldmVudE5hbWUsICdlcnJvcnMvJyArIHRoaXMuZXJyb3JzICsgJyBjb21wbGV0ZWQvJyArIHRoaXMuY29tcGxldGVkKTsNCiAgICAgICAgdmFyIHIgPSBudWxsOw0KDQogICAgICAgIGlmIChldmVudE5hbWUgPT0gJ3Jlc29sdmVkJyAmJiAhdGhpcy5lcnJvcnMgJiYgdGhpcy5jb21wbGV0ZWQpDQogICAgICAgIHsNCiAgICAgICAgICAgIHIgPSBldmVudENhbGxiYWNrLmFwcGx5KHRoaXMsIHRoaXMuY29tcGxldGVkQXJncyk7DQogICAgICAgICAgICBpZihyIT1udWxsKQ0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIHRoaXMuZW1pdF9yZXR1cm5WYWx1ZSgncmVzb2x2ZWQnLCByKTsNCiAgICAgICAgICAgIH0NCiAgICAgICAgfQ0KICAgICAgICBpZiAoZXZlbnROYW1lID09ICdyZWplY3RlZCcgJiYgdGhpcy5lcnJvcnMgJiYgdGhpcy5jb21wbGV0ZWQpDQogICAgICAgIHsNCiAgICAgICAgICAgIGV2ZW50Q2FsbGJhY2suYXBwbHkodGhpcywgdGhpcy5jb21wbGV0ZWRBcmdzKTsNCiAgICAgICAgfQ0KICAgICAgICBpZiAoZXZlbnROYW1lID09ICdzZXR0bGVkJyAmJiB0aGlzLmNvbXBsZXRlZCkNCiAgICAgICAgew0KICAgICAgICAgICAgZXZlbnRDYWxsYmFjay5hcHBseSh0aGlzLCBbXSk7DQogICAgICAgIH0NCiAgICB9KTsNCiAgICB0aGlzLl9pbnRlcm5hbC5yZXNvbHZlciA9IGZ1bmN0aW9uIF9yZXNvbHZlcigpDQogICAgew0KICAgICAgICBfcmVzb2x2ZXIuX3NlbGYuZXJyb3JzID0gZmFsc2U7DQogICAgICAgIF9yZXNvbHZlci5fc2VsZi5jb21wbGV0ZWQgPSB0cnVlOw0KICAgICAgICBfcmVzb2x2ZXIuX3NlbGYuY29tcGxldGVkQXJncyA9IFtdOw0KICAgICAgICB2YXIgYXJncyA9IFsncmVzb2x2ZWQnXTsNCiAgICAgICAgaWYgKHRoaXMuZW1pdF9yZXR1cm5WYWx1ZSAmJiB0aGlzLmVtaXRfcmV0dXJuVmFsdWUoJ3Jlc29sdmVkJykgIT0gbnVsbCkNCiAgICAgICAgew0KICAgICAgICAgICAgX3Jlc29sdmVyLl9zZWxmLmNvbXBsZXRlZEFyZ3MucHVzaCh0aGlzLmVtaXRfcmV0dXJuVmFsdWUoJ3Jlc29sdmVkJykpOw0KICAgICAgICAgICAgYXJncy5wdXNoKHRoaXMuZW1pdF9yZXR1cm5WYWx1ZSgncmVzb2x2ZWQnKSk7DQogICAgICAgIH0NCiAgICAgICAgZWxzZQ0KICAgICAgICB7DQogICAgICAgICAgICBmb3IgKHZhciBhIGluIGFyZ3VtZW50cykNCiAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICBfcmVzb2x2ZXIuX3NlbGYuY29tcGxldGVkQXJncy5wdXNoKGFyZ3VtZW50c1thXSk7DQogICAgICAgICAgICAgICAgYXJncy5wdXNoKGFyZ3VtZW50c1thXSk7DQogICAgICAgICAgICB9DQogICAgICAgIH0NCiAgICAgICAgX3Jlc29sdmVyLl9zZWxmLmVtaXQuYXBwbHkoX3Jlc29sdmVyLl9zZWxmLCBhcmdzKTsNCiAgICAgICAgX3Jlc29sdmVyLl9zZWxmLmVtaXQoJ3NldHRsZWQnKTsNCiAgICB9Ow0KICAgIHRoaXMuX2ludGVybmFsLnJlamVjdG9yID0gZnVuY3Rpb24gX3JlamVjdG9yKCkNCiAgICB7DQogICAgICAgIF9yZWplY3Rvci5fc2VsZi5lcnJvcnMgPSB0cnVlOw0KICAgICAgICBfcmVqZWN0b3IuX3NlbGYuY29tcGxldGVkID0gdHJ1ZTsNCiAgICAgICAgX3JlamVjdG9yLl9zZWxmLmNvbXBsZXRlZEFyZ3MgPSBbXTsNCiAgICAgICAgdmFyIGFyZ3MgPSBbJ3JlamVjdGVkJ107DQogICAgICAgIGZvciAodmFyIGEgaW4gYXJndW1lbnRzKQ0KICAgICAgICB7DQogICAgICAgICAgICBfcmVqZWN0b3IuX3NlbGYuY29tcGxldGVkQXJncy5wdXNoKGFyZ3VtZW50c1thXSk7DQogICAgICAgICAgICBhcmdzLnB1c2goYXJndW1lbnRzW2FdKTsNCiAgICAgICAgfQ0KDQogICAgICAgIF9yZWplY3Rvci5fc2VsZi5lbWl0LmFwcGx5KF9yZWplY3Rvci5fc2VsZiwgYXJncyk7DQogICAgICAgIF9yZWplY3Rvci5fc2VsZi5lbWl0KCdzZXR0bGVkJyk7DQogICAgfTsNCiAgICB0aGlzLmNhdGNoID0gZnVuY3Rpb24oZnVuYykNCiAgICB7DQogICAgICAgIHRoaXMuX2ludGVybmFsLm9uY2UoJ3JlamVjdGVkJywgZXZlbnRfc3dpdGNoZXIodGhpcywgZnVuYykuZnVuYyk7DQogICAgfQ0KICAgIHRoaXMuZmluYWxseSA9IGZ1bmN0aW9uIChmdW5jKQ0KICAgIHsNCiAgICAgICAgdGhpcy5faW50ZXJuYWwub25jZSgnc2V0dGxlZCcsIGV2ZW50X3N3aXRjaGVyKHRoaXMsIGZ1bmMpLmZ1bmMpOw0KICAgIH07DQogICAgdGhpcy50aGVuID0gZnVuY3Rpb24gKHJlc29sdmVkLCByZWplY3RlZCkNCiAgICB7DQogICAgICAgIGlmIChyZXNvbHZlZCkgeyB0aGlzLl9pbnRlcm5hbC5vbmNlKCdyZXNvbHZlZCcsIGV2ZW50X3N3aXRjaGVyKHRoaXMsIHJlc29sdmVkKS5mdW5jKTsgfQ0KICAgICAgICBpZiAocmVqZWN0ZWQpIHsgdGhpcy5faW50ZXJuYWwub25jZSgncmVqZWN0ZWQnLCBldmVudF9zd2l0Y2hlcih0aGlzLCByZWplY3RlZCkuZnVuYyk7IH0NCg0KICAgICAgICB2YXIgcmV0VmFsID0gbmV3IFByb21pc2UoZnVuY3Rpb24gKHIsIGopIHsgfSk7DQogICAgICAgIHRoaXMuX2ludGVybmFsLm9uY2UoJ3Jlc29sdmVkJywgcmV0VmFsLl9pbnRlcm5hbC5yZXNvbHZlcik7DQogICAgICAgIHRoaXMuX2ludGVybmFsLm9uY2UoJ3JlamVjdGVkJywgcmV0VmFsLl9pbnRlcm5hbC5yZWplY3Rvcik7DQogICAgICAgIHJldFZhbC5wYXJlbnRQcm9taXNlID0gdGhpczsNCiAgICAgICAgcmV0dXJuIChyZXRWYWwpOw0KICAgIH07DQoNCiAgICB0aGlzLl9pbnRlcm5hbC5yZXNvbHZlci5fc2VsZiA9IHRoaXMuX2ludGVybmFsOw0KICAgIHRoaXMuX2ludGVybmFsLnJlamVjdG9yLl9zZWxmID0gdGhpcy5faW50ZXJuYWw7Ow0KDQogICAgdHJ5DQogICAgew0KICAgICAgICBwcm9taXNlRnVuYy5jYWxsKHRoaXMsIHRoaXMuX2ludGVybmFsLnJlc29sdmVyLCB0aGlzLl9pbnRlcm5hbC5yZWplY3Rvcik7DQogICAgfQ0KICAgIGNhdGNoKGUpDQogICAgew0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5lcnJvcnMgPSB0cnVlOw0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5jb21wbGV0ZWQgPSB0cnVlOw0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5jb21wbGV0ZWRBcmdzID0gW2VdOw0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5lbWl0KCdyZWplY3RlZCcsIGUpOw0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5lbWl0KCdzZXR0bGVkJyk7DQogICAgfQ0KDQogICAgaWYoIXRoaXMuX2ludGVybmFsLmNvbXBsZXRlZCkNCiAgICB7DQogICAgICAgIC8vIFNhdmUgcmVmZXJlbmNlIG9mIHRoaXMgb2JqZWN0DQogICAgICAgIHJlZlRhYmxlW3RoaXMuX2ludGVybmFsLl9oYXNoQ29kZSgpXSA9IHRoaXMuX2ludGVybmFsOw0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5vbmNlKCdzZXR0bGVkJywgZnVuY3Rpb24gKCkgeyByZWZUYWJsZVt0aGlzLl9oYXNoQ29kZSgpXSA9IG51bGw7IH0pOw0KICAgIH0NCn0NCg0KUHJvbWlzZS5yZXNvbHZlID0gZnVuY3Rpb24gcmVzb2x2ZSgpDQp7DQogICAgdmFyIHJldFZhbCA9IG5ldyBQcm9taXNlKGZ1bmN0aW9uIChyLCBqKSB7IH0pOw0KICAgIHZhciBhcmdzID0gW107DQogICAgZm9yICh2YXIgaSBpbiBhcmd1bWVudHMpDQogICAgew0KICAgICAgICBhcmdzLnB1c2goYXJndW1lbnRzW2ldKTsNCiAgICB9DQogICAgcmV0VmFsLl9pbnRlcm5hbC5yZXNvbHZlci5hcHBseShyZXRWYWwuX2ludGVybmFsLCBhcmdzKTsNCiAgICByZXR1cm4gKHJldFZhbCk7DQp9Ow0KUHJvbWlzZS5yZWplY3QgPSBmdW5jdGlvbiByZWplY3QoKSB7DQogICAgdmFyIHJldFZhbCA9IG5ldyBQcm9taXNlKGZ1bmN0aW9uIChyLCBqKSB7IH0pOw0KICAgIHZhciBhcmdzID0gW107DQogICAgZm9yICh2YXIgaSBpbiBhcmd1bWVudHMpIHsNCiAgICAgICAgYXJncy5wdXNoKGFyZ3VtZW50c1tpXSk7DQogICAgfQ0KICAgIHJldFZhbC5faW50ZXJuYWwucmVqZWN0b3IuYXBwbHkocmV0VmFsLl9pbnRlcm5hbCwgYXJncyk7DQogICAgcmV0dXJuIChyZXRWYWwpOw0KfTsNClByb21pc2UuYWxsID0gZnVuY3Rpb24gYWxsKHByb21pc2VMaXN0KQ0Kew0KICAgIHZhciByZXQgPSBuZXcgUHJvbWlzZShmdW5jdGlvbiAocmVzLCByZWopDQogICAgew0KICAgICAgICB0aGlzLl9fcmVqZWN0b3IgPSByZWo7DQogICAgICAgIHRoaXMuX19yZXNvbHZlciA9IHJlczsNCiAgICAgICAgdGhpcy5fX3Byb21pc2VMaXN0ID0gcHJvbWlzZUxpc3Q7DQogICAgICAgIHRoaXMuX19kb25lID0gZmFsc2U7DQogICAgICAgIHRoaXMuX19jb3VudCA9IDA7DQogICAgfSk7DQoNCiAgICBmb3IgKHZhciBpIGluIHByb21pc2VMaXN0KQ0KICAgIHsNCiAgICAgICAgcHJvbWlzZUxpc3RbaV0udGhlbihmdW5jdGlvbiAoKQ0KICAgICAgICB7DQogICAgICAgICAgICAvLyBTdWNjZXNzDQogICAgICAgICAgICBpZigrK3JldC5fX2NvdW50ID09IHJldC5fX3Byb21pc2VMaXN0Lmxlbmd0aCkNCiAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICByZXQuX19kb25lID0gdHJ1ZTsNCiAgICAgICAgICAgICAgICByZXQuX19yZXNvbHZlcihyZXQuX19wcm9taXNlTGlzdCk7DQogICAgICAgICAgICB9DQogICAgICAgIH0sIGZ1bmN0aW9uIChhcmcpDQogICAgICAgIHsNCiAgICAgICAgICAgIC8vIEZhaWx1cmUNCiAgICAgICAgICAgIGlmKCFyZXQuX19kb25lKQ0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIHJldC5fX2RvbmUgPSB0cnVlOw0KICAgICAgICAgICAgICAgIHJldC5fX3JlamVjdG9yKGFyZyk7DQogICAgICAgICAgICB9DQogICAgICAgIH0pOw0KICAgIH0NCiAgICBpZiAocHJvbWlzZUxpc3QubGVuZ3RoID09IDApDQogICAgew0KICAgICAgICByZXQuX19yZXNvbHZlcihwcm9taXNlTGlzdCk7DQogICAgfQ0KICAgIHJldHVybiAocmV0KTsNCn07DQoNCm1vZHVsZS5leHBvcnRzID0gUHJvbWlzZTsNCm1vZHVsZS5leHBvcnRzLmV2ZW50X3N3aXRjaGVyID0gZXZlbnRfc3dpdGNoZXI7', 'base64').toString());");

#ifdef WIN32
	// Adding win-registry, since it is very useful for windows... Refer to /modules/win-registry.js to see a human readable version
	duk_peval_string_noresult(ctx, "addModule('win-registry', Buffer.from('LyoKQ29weXJpZ2h0IDIwMTggSW50ZWwgQ29ycG9yYXRpb24KCkxpY2Vuc2VkIHVuZGVyIHRoZSBBcGFjaGUgTGljZW5zZSwgVmVyc2lvbiAyLjAgKHRoZSAiTGljZW5zZSIpOwp5b3UgbWF5IG5vdCB1c2UgdGhpcyBmaWxlIGV4Y2VwdCBpbiBjb21wbGlhbmNlIHdpdGggdGhlIExpY2Vuc2UuCllvdSBtYXkgb2J0YWluIGEgY29weSBvZiB0aGUgTGljZW5zZSBhdAoKICAgIGh0dHA6Ly93d3cuYXBhY2hlLm9yZy9saWNlbnNlcy9MSUNFTlNFLTIuMAoKVW5sZXNzIHJlcXVpcmVkIGJ5IGFwcGxpY2FibGUgbGF3IG9yIGFncmVlZCB0byBpbiB3cml0aW5nLCBzb2Z0d2FyZQpkaXN0cmlidXRlZCB1bmRlciB0aGUgTGljZW5zZSBpcyBkaXN0cmlidXRlZCBvbiBhbiAiQVMgSVMiIEJBU0lTLApXSVRIT1VUIFdBUlJBTlRJRVMgT1IgQ09ORElUSU9OUyBPRiBBTlkgS0lORCwgZWl0aGVyIGV4cHJlc3Mgb3IgaW1wbGllZC4KU2VlIHRoZSBMaWNlbnNlIGZvciB0aGUgc3BlY2lmaWMgbGFuZ3VhZ2UgZ292ZXJuaW5nIHBlcm1pc3Npb25zIGFuZApsaW1pdGF0aW9ucyB1bmRlciB0aGUgTGljZW5zZS4KKi8KCnZhciBLRVlfUVVFUllfVkFMVUUgPSAweDAwMDE7CnZhciBLRVlfRU5VTUVSQVRFX1NVQl9LRVlTID0gMHgwMDA4Owp2YXIgS0VZX1dSSVRFID0gMHgyMDAwNjsKCnZhciBLRVlfREFUQV9UWVBFUyA9CiAgICB7CiAgICAgICAgUkVHX05PTkU6IDAsCiAgICAgICAgUkVHX1NaOiAxLAogICAgICAgIFJFR19FWFBBTkRfU1o6IDIsCiAgICAgICAgUkVHX0JJTkFSWTogMywKICAgICAgICBSRUdfRFdPUkQ6IDQsCiAgICAgICAgUkVHX0RXT1JEX0JJR19FTkRJQU46IDUsCiAgICAgICAgUkVHX0xJTks6IDYsCiAgICAgICAgUkVHX01VTFRJX1NaOiA3LAogICAgICAgIFJFR19SRVNPVVJDRV9MSVNUOiA4LAogICAgICAgIFJFR19GVUxMX1JFU09VUkNFX0RFU0NSSVBUT1I6IDksCiAgICAgICAgUkVHX1JFU09VUkNFX1JFUVVJUkVNRU5UU19MSVNUOiAxMCwKICAgICAgICBSRUdfUVdPUkQ6IDExCiAgICB9OwoKZnVuY3Rpb24gd2luZG93c19yZWdpc3RyeSgpCnsKICAgIHRoaXMuX09iamVjdElkID0gJ3dpbi1yZWdpc3RyeSc7CiAgICB0aGlzLl9tYXJzaGFsID0gcmVxdWlyZSgnX0dlbmVyaWNNYXJzaGFsJyk7CiAgICB0aGlzLl9BZHZBcGkgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZU5hdGl2ZVByb3h5KCdBZHZhcGkzMi5kbGwnKTsKICAgIHRoaXMuX0FkdkFwaS5DcmVhdGVNZXRob2QoJ1JlZ0NyZWF0ZUtleUV4QScpOwogICAgdGhpcy5fQWR2QXBpLkNyZWF0ZU1ldGhvZCgnUmVnRW51bUtleUV4QScpOwogICAgdGhpcy5fQWR2QXBpLkNyZWF0ZU1ldGhvZCgnUmVnRW51bVZhbHVlQScpOwogICAgdGhpcy5fQWR2QXBpLkNyZWF0ZU1ldGhvZCgnUmVnT3BlbktleUV4QScpOwogICAgdGhpcy5fQWR2QXBpLkNyZWF0ZU1ldGhvZCgnUmVnUXVlcnlJbmZvS2V5QScpOwogICAgdGhpcy5fQWR2QXBpLkNyZWF0ZU1ldGhvZCgnUmVnUXVlcnlWYWx1ZUV4QScpOwogICAgdGhpcy5fQWR2QXBpLkNyZWF0ZU1ldGhvZCgnUmVnQ2xvc2VLZXknKTsKICAgIHRoaXMuX0FkdkFwaS5DcmVhdGVNZXRob2QoJ1JlZ0RlbGV0ZUtleUEnKTsKICAgIHRoaXMuX0FkdkFwaS5DcmVhdGVNZXRob2QoJ1JlZ0RlbGV0ZVZhbHVlQScpOwogICAgdGhpcy5fQWR2QXBpLkNyZWF0ZU1ldGhvZCgnUmVnU2V0VmFsdWVFeEEnKTsKICAgIHRoaXMuSEtFWSA9IHsgUm9vdDogQnVmZmVyLmZyb20oJzgwMDAwMDAwJywgJ2hleCcpLnN3YXAzMigpLCBDdXJyZW50VXNlcjogQnVmZmVyLmZyb20oJzgwMDAwMDAxJywgJ2hleCcpLnN3YXAzMigpLCBMb2NhbE1hY2hpbmU6IEJ1ZmZlci5mcm9tKCc4MDAwMDAwMicsICdoZXgnKS5zd2FwMzIoKSwgVXNlcnM6IEJ1ZmZlci5mcm9tKCc4MDAwMDAwMycsICdoZXgnKS5zd2FwMzIoKSB9OwoKICAgIHRoaXMuUXVlcnlLZXkgPSBmdW5jdGlvbiBRdWVyeUtleShoa2V5LCBwYXRoLCBrZXkpCiAgICB7CiAgICAgICAgdmFyIGVycjsKICAgICAgICB2YXIgaCA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlUG9pbnRlcigpOwogICAgICAgIHZhciBsZW4gPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKDQpOwogICAgICAgIHZhciB2YWxUeXBlID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVWYXJpYWJsZSg0KTsKICAgICAgICB2YXIgSEsgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZVBvaW50ZXIoaGtleSk7CiAgICAgICAgdmFyIHJldFZhbCA9IG51bGw7CiAgICAgICAgaWYgKGtleSkgeyBrZXkgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKGtleSk7IH0KICAgICAgICBpZiAoIXBhdGgpIHsgcGF0aCA9ICcnOyB9CgoKICAgICAgICBpZiAoKGVyciA9IHRoaXMuX0FkdkFwaS5SZWdPcGVuS2V5RXhBKEhLLCB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKHBhdGgpLCAwLCBLRVlfUVVFUllfVkFMVUUgfCBLRVlfRU5VTUVSQVRFX1NVQl9LRVlTLCBoKS5WYWwpICE9IDApCiAgICAgICAgewogICAgICAgICAgICB0aHJvdyAoJ09wZW5pbmcgUmVnaXN0cnkgS2V5OiAnICsgcGF0aCArICcgPT4gUmV0dXJuZWQgRXJyb3I6ICcgKyBlcnIpOwogICAgICAgIH0KICAKICAgICAgICBpZiAoKHBhdGggPT0gJycgJiYgIWtleSkgfHwgIWtleSkKICAgICAgICB7DQogICAgICAgICAgICB2YXIgcmVzdWx0ID0geyBzdWJrZXlzOiBbXSwgdmFsdWVzOiBbXSB9Ow0KDQogICAgICAgICAgICAvLyBFbnVtZXJhdGUgIGtleXMNCiAgICAgICAgICAgIHZhciBhY2hDbGFzcyA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUoMTAyNCk7DQogICAgICAgICAgICB2YXIgYWNoS2V5ID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVWYXJpYWJsZSgxMDI0KTsNCiAgICAgICAgICAgIHZhciBhY2hWYWx1ZSA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUoMzI3NjgpOw0KICAgICAgICAgICAgdmFyIGFjaFZhbHVlU2l6ZSA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUoNCk7DQogICAgICAgICAgICB2YXIgbmFtZVNpemUgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKDQpOyANCiAgICAgICAgICAgIHZhciBhY2hDbGFzc1NpemUgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKDQpOyBhY2hDbGFzc1NpemUudG9CdWZmZXIoKS53cml0ZVVJbnQzMkxFKDEwMjQpOw0KICAgICAgICAgICAgdmFyIG51bVN1YktleXMgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKDQpOw0KICAgICAgICAgICAgdmFyIG51bVZhbHVlcyA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUoNCk7DQogICAgICAgICAgICB2YXIgbG9uZ2VzdFN1YmtleVNpemUgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKDQpOw0KICAgICAgICAgICAgdmFyIGxvbmdlc3RDbGFzc1N0cmluZyA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUoNCk7DQogICAgICAgICAgICB2YXIgbG9uZ2VzdFZhbHVlTmFtZSA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUoNCk7DQogICAgICAgICAgICB2YXIgbG9uZ2VzdFZhbHVlRGF0YSA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUoNCk7DQogICAgICAgICAgICB2YXIgc2VjdXJpdHlEZXNjcmlwdG9yID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVWYXJpYWJsZSg0KTsNCiAgICAgICAgICAgIHZhciBsYXN0V3JpdGVUaW1lID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVWYXJpYWJsZSg4KTsNCg0KICAgICAgICAgICAgcmV0VmFsID0gdGhpcy5fQWR2QXBpLlJlZ1F1ZXJ5SW5mb0tleUEoaC5EZXJlZigpLCBhY2hDbGFzcywgYWNoQ2xhc3NTaXplLCAwLA0KICAgICAgICAgICAgICAgIG51bVN1YktleXMsIGxvbmdlc3RTdWJrZXlTaXplLCBsb25nZXN0Q2xhc3NTdHJpbmcsIG51bVZhbHVlcywNCiAgICAgICAgICAgICAgICBsb25nZXN0VmFsdWVOYW1lLCBsb25nZXN0VmFsdWVEYXRhLCBzZWN1cml0eURlc2NyaXB0b3IsIGxhc3RXcml0ZVRpbWUpOw0KICAgICAgICAgICAgaWYgKHJldFZhbC5WYWwgIT0gMCkgeyB0aHJvdyAoJ1JlZ1F1ZXJ5SW5mb0tleUEoKSByZXR1cm5lZCBlcnJvcjogJyArIHJldFZhbC5WYWwpOyB9DQogICAgICAgICAgICBmb3IodmFyIGkgPSAwOyBpIDwgbnVtU3ViS2V5cy50b0J1ZmZlcigpLnJlYWRVSW50MzJMRSgpOyArK2kpDQogICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgbmFtZVNpemUudG9CdWZmZXIoKS53cml0ZVVJbnQzMkxFKDEwMjQpOw0KICAgICAgICAgICAgICAgIHJldFZhbCA9IHRoaXMuX0FkdkFwaS5SZWdFbnVtS2V5RXhBKGguRGVyZWYoKSwgaSwgYWNoS2V5LCBuYW1lU2l6ZSwgMCwgMCwgMCwgbGFzdFdyaXRlVGltZSk7DQogICAgICAgICAgICAgICAgaWYocmV0VmFsLlZhbCA9PSAwKQ0KICAgICAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICAgICAgcmVzdWx0LnN1YmtleXMucHVzaChhY2hLZXkuU3RyaW5nKTsNCiAgICAgICAgICAgICAgICB9DQogICAgICAgICAgICB9DQogICAgICAgICAgICBmb3IgKHZhciBpID0gMDsgaSA8IG51bVZhbHVlcy50b0J1ZmZlcigpLnJlYWRVSW50MzJMRSgpIDsgKytpKQ0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIGFjaFZhbHVlU2l6ZS50b0J1ZmZlcigpLndyaXRlVUludDMyTEUoMzI3NjgpOw0KICAgICAgICAgICAgICAgIGlmKHRoaXMuX0FkdkFwaS5SZWdFbnVtVmFsdWVBKGguRGVyZWYoKSwgaSwgYWNoVmFsdWUsIGFjaFZhbHVlU2l6ZSwgMCwgMCwgMCwgMCkuVmFsID09IDApDQogICAgICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgICAgICByZXN1bHQudmFsdWVzLnB1c2goYWNoVmFsdWUuU3RyaW5nKTsNCiAgICAgICAgICAgICAgICB9DQogICAgICAgICAgICB9DQogICAgICAgICAgICByZXR1cm4gKHJlc3VsdCk7DQogICAgICAgIH0KCiAgICAgICAgaWYodGhpcy5fQWR2QXBpLlJlZ1F1ZXJ5VmFsdWVFeEEoaC5EZXJlZigpLCBrZXksIDAsIDAsIDAsIGxlbikuVmFsID09IDApCiAgICAgICAgewogICAgICAgICAgICB2YXIgZGF0YSA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUobGVuLnRvQnVmZmVyKCkucmVhZFVJbnQzMkxFKCkpOwogICAgICAgICAgICBpZiAodGhpcy5fQWR2QXBpLlJlZ1F1ZXJ5VmFsdWVFeEEoaC5EZXJlZigpLCBrZXksIDAsIHZhbFR5cGUsIGRhdGEsIGxlbikuVmFsID09IDApCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIHN3aXRjaCh2YWxUeXBlLnRvQnVmZmVyKCkucmVhZFVJbnQzMkxFKCkpCiAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgY2FzZSBLRVlfREFUQV9UWVBFUy5SRUdfRFdPUkQ6CiAgICAgICAgICAgICAgICAgICAgICAgIHJldFZhbCA9IGRhdGEudG9CdWZmZXIoKS5yZWFkVUludDMyTEUoKTsKICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICAgICAgY2FzZSBLRVlfREFUQV9UWVBFUy5SRUdfRFdPUkRfQklHX0VORElBTjoKICAgICAgICAgICAgICAgICAgICAgICAgcmV0VmFsID0gZGF0YS50b0J1ZmZlcigpLnJlYWRVSW50MzJCRSgpOwogICAgICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgICAgICBjYXNlIEtFWV9EQVRBX1RZUEVTLlJFR19TWjoKICAgICAgICAgICAgICAgICAgICAgICAgcmV0VmFsID0gZGF0YS5TdHJpbmc7CiAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgIGNhc2UgS0VZX0RBVEFfVFlQRVMuUkVHX0JJTkFSWToKICAgICAgICAgICAgICAgICAgICBkZWZhdWx0OgogICAgICAgICAgICAgICAgICAgICAgICByZXRWYWwgPSBkYXRhLnRvQnVmZmVyKCk7CiAgICAgICAgICAgICAgICAgICAgICAgIHJldFZhbC5fZGF0YSA9IGRhdGE7CiAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgfQogICAgICAgICAgICB9CiAgICAgICAgfQogICAgICAgIGVsc2UKICAgICAgICB7CiAgICAgICAgICAgIHRoaXMuX0FkdkFwaS5SZWdDbG9zZUtleShoLkRlcmVmKCkpOwogICAgICAgICAgICB0aHJvdyAoJ05vdCBGb3VuZCcpOwogICAgICAgIH0KICAgICAgICB0aGlzLl9BZHZBcGkuUmVnQ2xvc2VLZXkoaC5EZXJlZigpKTsKICAgICAgICByZXR1cm4gKHJldFZhbCk7CiAgICB9OwogICAgdGhpcy5Xcml0ZUtleSA9IGZ1bmN0aW9uIFdyaXRlS2V5KGhrZXksIHBhdGgsIGtleSwgdmFsdWUpCiAgICB7CiAgICAgICAgdmFyIHJlc3VsdDsKICAgICAgICB2YXIgaCA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlUG9pbnRlcigpOwoKICAgICAgICBpZiAodGhpcy5fQWR2QXBpLlJlZ0NyZWF0ZUtleUV4QSh0aGlzLl9tYXJzaGFsLkNyZWF0ZVBvaW50ZXIoaGtleSksIHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUocGF0aCksIDAsIDAsIDAsIEtFWV9XUklURSwgMCwgaCwgMCkuVmFsICE9IDApCiAgICAgICAgewogICAgICAgICAgICB0aHJvdyAoJ0Vycm9yIE9wZW5pbmcgUmVnaXN0cnkgS2V5OiAnICsgcGF0aCk7CiAgICAgICAgfQoKICAgICAgICB2YXIgZGF0YTsKICAgICAgICB2YXIgZGF0YVR5cGU7CgogICAgICAgIHN3aXRjaCh0eXBlb2YodmFsdWUpKQogICAgICAgIHsKICAgICAgICAgICAgY2FzZSAnYm9vbGVhbic6CiAgICAgICAgICAgICAgICBkYXRhVHlwZSA9IEtFWV9EQVRBX1RZUEVTLlJFR19EV09SRDsKICAgICAgICAgICAgICAgIGRhdGEgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKDQpOwogICAgICAgICAgICAgICAgZGF0YS50b0J1ZmZlcigpLndyaXRlVUludDMyTEUodmFsdWUgPyAxIDogMCk7CiAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgY2FzZSAnbnVtYmVyJzoKICAgICAgICAgICAgICAgIGRhdGFUeXBlID0gS0VZX0RBVEFfVFlQRVMuUkVHX0RXT1JEOwogICAgICAgICAgICAgICAgZGF0YSA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUoNCk7CiAgICAgICAgICAgICAgICBkYXRhLnRvQnVmZmVyKCkud3JpdGVVSW50MzJMRSh2YWx1ZSk7CiAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgY2FzZSAnc3RyaW5nJzoKICAgICAgICAgICAgICAgIGRhdGFUeXBlID0gS0VZX0RBVEFfVFlQRVMuUkVHX1NaOwogICAgICAgICAgICAgICAgZGF0YSA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUodmFsdWUpOwogICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgIGRlZmF1bHQ6CiAgICAgICAgICAgICAgICBkYXRhVHlwZSA9IEtFWV9EQVRBX1RZUEVTLlJFR19CSU5BUlk7CiAgICAgICAgICAgICAgICBkYXRhID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVWYXJpYWJsZSh2YWx1ZS5sZW5ndGgpOwogICAgICAgICAgICAgICAgdmFsdWUuY29weShkYXRhLnRvQnVmZmVyKCkpOwogICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgfQoKICAgICAgICBpZih0aGlzLl9BZHZBcGkuUmVnU2V0VmFsdWVFeEEoaC5EZXJlZigpLCB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKGtleSksIDAsIGRhdGFUeXBlLCBkYXRhLCBkYXRhLl9zaXplKS5WYWwgIT0gMCkKICAgICAgICB7ICAgICAgICAgICAKICAgICAgICAgICAgdGhpcy5fQWR2QXBpLlJlZ0Nsb3NlS2V5KGguRGVyZWYoKSk7CiAgICAgICAgICAgIHRocm93ICgnRXJyb3Igd3JpdGluZyByZWcga2V5OiAnICsga2V5KTsKICAgICAgICB9CiAgICAgICAgdGhpcy5fQWR2QXBpLlJlZ0Nsb3NlS2V5KGguRGVyZWYoKSk7CiAgICB9OwogICAgdGhpcy5EZWxldGVLZXkgPSBmdW5jdGlvbiBEZWxldGVLZXkoaGtleSwgcGF0aCwga2V5KQogICAgewogICAgICAgIGlmKCFrZXkpCiAgICAgICAgewogICAgICAgICAgICBpZih0aGlzLl9BZHZBcGkuUmVnRGVsZXRlS2V5QSh0aGlzLl9tYXJzaGFsLkNyZWF0ZVBvaW50ZXIoaGtleSksIHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUocGF0aCkpLlZhbCAhPSAwKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICB0aHJvdyAoJ0Vycm9yIERlbGV0aW5nIEtleTogJyArIHBhdGgpOwogICAgICAgICAgICB9CiAgICAgICAgfQogICAgICAgIGVsc2UKICAgICAgICB7CiAgICAgICAgICAgIHZhciBoID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVQb2ludGVyKCk7CiAgICAgICAgICAgIHZhciByZXN1bHQ7CiAgICAgICAgICAgIGlmICh0aGlzLl9BZHZBcGkuUmVnT3BlbktleUV4QSh0aGlzLl9tYXJzaGFsLkNyZWF0ZVBvaW50ZXIoaGtleSksIHRoaXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUocGF0aCksIDAsIEtFWV9RVUVSWV9WQUxVRSB8IEtFWV9XUklURSwgaCkuVmFsICE9IDApCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIHRocm93ICgnRXJyb3IgT3BlbmluZyBSZWdpc3RyeSBLZXk6ICcgKyBwYXRoKTsKICAgICAgICAgICAgfQogICAgICAgICAgICBpZiAoKHJlc3VsdCA9IHRoaXMuX0FkdkFwaS5SZWdEZWxldGVWYWx1ZUEoaC5EZXJlZigpLCB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKGtleSkpLlZhbCkgIT0gMCkKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgdGhpcy5fQWR2QXBpLlJlZ0Nsb3NlS2V5KGguRGVyZWYoKSk7CiAgICAgICAgICAgICAgICB0aHJvdyAoJ0Vycm9yWycgKyByZXN1bHQgKyAnXSBEZWxldGluZyBLZXk6ICcgKyBwYXRoICsgJy4nICsga2V5KTsKICAgICAgICAgICAgfQogICAgICAgICAgICB0aGlzLl9BZHZBcGkuUmVnQ2xvc2VLZXkoaC5EZXJlZigpKTsKICAgICAgICB9CiAgICB9Owp9Cgptb2R1bGUuZXhwb3J0cyA9IG5ldyB3aW5kb3dzX3JlZ2lzdHJ5KCk7Cgo=', 'base64').toString());");
#endif

	// service-manager, which on linux has a dependency on user-sessions and process-manager. Refer to /modules folder for human readable versions.
	duk_peval_string_noresult(ctx, "addModule('process-manager', Buffer.from('LyoKQ29weXJpZ2h0IDIwMTggSW50ZWwgQ29ycG9yYXRpb24KCkxpY2Vuc2VkIHVuZGVyIHRoZSBBcGFjaGUgTGljZW5zZSwgVmVyc2lvbiAyLjAgKHRoZSAiTGljZW5zZSIpOwp5b3UgbWF5IG5vdCB1c2UgdGhpcyBmaWxlIGV4Y2VwdCBpbiBjb21wbGlhbmNlIHdpdGggdGhlIExpY2Vuc2UuCllvdSBtYXkgb2J0YWluIGEgY29weSBvZiB0aGUgTGljZW5zZSBhdAoKICAgIGh0dHA6Ly93d3cuYXBhY2hlLm9yZy9saWNlbnNlcy9MSUNFTlNFLTIuMAoKVW5sZXNzIHJlcXVpcmVkIGJ5IGFwcGxpY2FibGUgbGF3IG9yIGFncmVlZCB0byBpbiB3cml0aW5nLCBzb2Z0d2FyZQpkaXN0cmlidXRlZCB1bmRlciB0aGUgTGljZW5zZSBpcyBkaXN0cmlidXRlZCBvbiBhbiAiQVMgSVMiIEJBU0lTLApXSVRIT1VUIFdBUlJBTlRJRVMgT1IgQ09ORElUSU9OUyBPRiBBTlkgS0lORCwgZWl0aGVyIGV4cHJlc3Mgb3IgaW1wbGllZC4KU2VlIHRoZSBMaWNlbnNlIGZvciB0aGUgc3BlY2lmaWMgbGFuZ3VhZ2UgZ292ZXJuaW5nIHBlcm1pc3Npb25zIGFuZApsaW1pdGF0aW9ucyB1bmRlciB0aGUgTGljZW5zZS4KKi8KCgp2YXIgR00gPSByZXF1aXJlKCdfR2VuZXJpY01hcnNoYWwnKTsKCi8vIFVzZWQgb24gV2luZG93cyBhbmQgTGludXggdG8gZ2V0IGluZm9ybWF0aW9uIGFib3V0IHJ1bm5pbmcgcHJvY2Vzc2VzCmZ1bmN0aW9uIHByb2Nlc3NNYW5hZ2VyKCkgewogICAgdGhpcy5fT2JqZWN0SUQgPSAncHJvY2Vzcy1tYW5hZ2VyJzsgLy8gVXNlZCBmb3IgZGVidWdnaW5nLCBhbGxvd3MgeW91IHRvIGdldCB0aGUgb2JqZWN0IHR5cGUgYXQgcnVudGltZS4KICAgIAogICAgLy8gU2V0dXAgdGhlIHBsYXRmb3JtIHNwZWNpZmljIGNhbGxzLgogICAgc3dpdGNoIChwcm9jZXNzLnBsYXRmb3JtKQogICAgewogICAgICAgIGNhc2UgJ3dpbjMyJzoKICAgICAgICAgICAgdGhpcy5fa2VybmVsMzIgPSBHTS5DcmVhdGVOYXRpdmVQcm94eSgna2VybmVsMzIuZGxsJyk7CiAgICAgICAgICAgIHRoaXMuX2tlcm5lbDMyLkNyZWF0ZU1ldGhvZCgnR2V0TGFzdEVycm9yJyk7CiAgICAgICAgICAgIHRoaXMuX2tlcm5lbDMyLkNyZWF0ZU1ldGhvZCgnQ3JlYXRlVG9vbGhlbHAzMlNuYXBzaG90Jyk7CiAgICAgICAgICAgIHRoaXMuX2tlcm5lbDMyLkNyZWF0ZU1ldGhvZCgnUHJvY2VzczMyRmlyc3QnKTsKICAgICAgICAgICAgdGhpcy5fa2VybmVsMzIuQ3JlYXRlTWV0aG9kKCdQcm9jZXNzMzJOZXh0Jyk7CiAgICAgICAgICAgIGJyZWFrOwogICAgICAgIGNhc2UgJ2xpbnV4JzoKICAgICAgICBjYXNlICdkYXJ3aW4nOgogICAgICAgICAgICB0aGlzLl9jaGlsZFByb2Nlc3MgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJyk7CiAgICAgICAgICAgIGJyZWFrOwogICAgICAgIGRlZmF1bHQ6CiAgICAgICAgICAgIHRocm93IChwcm9jZXNzLnBsYXRmb3JtICsgJyBub3Qgc3VwcG9ydGVkJyk7CiAgICAgICAgICAgIGJyZWFrOwogICAgfQogICAgdGhpcy5lbnVtZXJhdGVQcm9jZXNzZXMgPSBmdW5jdGlvbiBlbnVtZXJhdGVQcm9jZXNzZXMoKQogICAgewogICAgICAgIHZhciBwcm9taXNlID0gcmVxdWlyZSgncHJvbWlzZScpOwogICAgICAgIHZhciByZXQgPSBuZXcgcHJvbWlzZShmdW5jdGlvbiAocmVzLCByZWopIHsgdGhpcy5fcmVzID0gcmVzOyB0aGlzLl9yZWogPSByZWo7IH0pOwogICAgICAgIHRoaXMuZ2V0UHJvY2Vzc2VzKGZ1bmN0aW9uIChwcywgcHJvbSkgeyBwcm9tLl9yZXMocHMpOyB9LCByZXQpOwogICAgICAgIHJldHVybiAocmV0KTsKICAgIH0KICAgIC8vIFJldHVybiBhIG9iamVjdCBvZjogcGlkIC0+IHByb2Nlc3MgaW5mb3JtYXRpb24uCiAgICB0aGlzLmdldFByb2Nlc3NlcyA9IGZ1bmN0aW9uIGdldFByb2Nlc3NlcyhjYWxsYmFjaykKICAgIHsKICAgICAgICBzd2l0Y2gocHJvY2Vzcy5wbGF0Zm9ybSkKICAgICAgICB7CiAgICAgICAgICAgIGRlZmF1bHQ6CiAgICAgICAgICAgICAgICB0aHJvdyAoJ0VudW1lcmF0aW5nIHByb2Nlc3NlcyBvbiAnICsgcHJvY2Vzcy5wbGF0Zm9ybSArICcgbm90IHN1cHBvcnRlZCcpOwogICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgIGNhc2UgJ3dpbjMyJzogLy8gV2luZG93cyBwcm9jZXNzZXMKICAgICAgICAgICAgICAgIHZhciByZXRWYWwgPSB7fTsKICAgICAgICAgICAgICAgIHZhciBoID0gdGhpcy5fa2VybmVsMzIuQ3JlYXRlVG9vbGhlbHAzMlNuYXBzaG90KDIsIDApOwogICAgICAgICAgICAgICAgdmFyIGluZm8gPSBHTS5DcmVhdGVWYXJpYWJsZSgzMDQpOwogICAgICAgICAgICAgICAgaW5mby50b0J1ZmZlcigpLndyaXRlVUludDMyTEUoMzA0LCAwKTsKICAgICAgICAgICAgICAgIHZhciBuZXh0UHJvY2VzcyA9IHRoaXMuX2tlcm5lbDMyLlByb2Nlc3MzMkZpcnN0KGgsIGluZm8pOwogICAgICAgICAgICAgICAgd2hpbGUgKG5leHRQcm9jZXNzLlZhbCkgCiAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgcmV0VmFsW2luZm8uRGVyZWYoOCwgNCkudG9CdWZmZXIoKS5yZWFkVUludDMyTEUoMCldID0geyBwaWQ6IGluZm8uRGVyZWYoOCwgNCkudG9CdWZmZXIoKS5yZWFkVUludDMyTEUoMCksIGNtZDogaW5mby5EZXJlZihHTS5Qb2ludGVyU2l6ZSA9PSA0ID8gMzYgOiA0NCwgMjYwKS5TdHJpbmcgfTsKICAgICAgICAgICAgICAgICAgICBuZXh0UHJvY2VzcyA9IHRoaXMuX2tlcm5lbDMyLlByb2Nlc3MzMk5leHQoaCwgaW5mbyk7CiAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICBpZiAoY2FsbGJhY2spIHsgY2FsbGJhY2suYXBwbHkodGhpcywgW3JldFZhbF0pOyB9CiAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgY2FzZSAnbGludXgnOiAvLyBMaW51eCBwcm9jZXNzZXMKICAgICAgICAgICAgICAgIGlmICghdGhpcy5fcHNwKSB7IHRoaXMuX3BzcCA9IHt9OyB9CiAgICAgICAgICAgICAgICB2YXIgcCA9IHRoaXMuX2NoaWxkUHJvY2Vzcy5leGVjRmlsZSgiL2Jpbi9wcyIsIFsicHMiLCAiLXV4YSJdLCB7IHR5cGU6IHRoaXMuX2NoaWxkUHJvY2Vzcy5TcGF3blR5cGVzLlRFUk0gfSk7CiAgICAgICAgICAgICAgICB0aGlzLl9wc3BbcC5waWRdID0gcDsKICAgICAgICAgICAgICAgIHAuUGFyZW50ID0gdGhpczsKICAgICAgICAgICAgICAgIHAucHMgPSAnJzsKICAgICAgICAgICAgICAgIHAuY2FsbGJhY2sgPSBjYWxsYmFjazsKICAgICAgICAgICAgICAgIHAuYXJncyA9IFtdOwogICAgICAgICAgICAgICAgZm9yICh2YXIgaSA9IDE7IGkgPCBhcmd1bWVudHMubGVuZ3RoOyArK2kpIHsgcC5hcmdzLnB1c2goYXJndW1lbnRzW2ldKTsgfQogICAgICAgICAgICAgICAgcC5vbignZXhpdCcsIGZ1bmN0aW9uIG9uR2V0UHJvY2Vzc2VzKCkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICBkZWxldGUgdGhpcy5QYXJlbnQuX3BzcFt0aGlzLnBpZF07IAogICAgICAgICAgICAgICAgICAgIHZhciByZXRWYWwgPSB7fSwgbGluZXMgPSB0aGlzLnBzLnNwbGl0KCdceDBEXHgwQScpLCBrZXkgPSB7fSwga2V5aSA9IDA7CiAgICAgICAgICAgICAgICAgICAgZm9yICh2YXIgaSBpbiBsaW5lcykKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIHZhciB0b2tlbnMgPSBsaW5lc1tpXS5zcGxpdCgnICcpOwogICAgICAgICAgICAgICAgICAgICAgICB2YXIgdG9rZW5MaXN0ID0gW107CiAgICAgICAgICAgICAgICAgICAgICAgIGZvcih2YXIgeCBpbiB0b2tlbnMpCiAgICAgICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIGlmIChpID09IDAgJiYgdG9rZW5zW3hdKSB7IGtleVt0b2tlbnNbeF1dID0ga2V5aSsrOyB9CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBpZiAoaSA+IDAgJiYgdG9rZW5zW3hdKSB7IHRva2VuTGlzdC5wdXNoKHRva2Vuc1t4XSk7fQogICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgIGlmIChpID4gMCkgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgaWYgKHRva2VuTGlzdFtrZXkuUElEXSkgeyByZXRWYWxbdG9rZW5MaXN0W2tleS5QSURdXSA9IHsgcGlkOiBrZXkuUElELCB1c2VyOiB0b2tlbkxpc3Rba2V5LlVTRVJdLCBjbWQ6IHRva2VuTGlzdFtrZXkuQ09NTUFORF0gfTsgfQogICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgIGlmICh0aGlzLmNhbGxiYWNrKQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgdGhpcy5hcmdzLnVuc2hpZnQocmV0VmFsKTsKICAgICAgICAgICAgICAgICAgICAgICAgdGhpcy5jYWxsYmFjay5hcHBseSh0aGlzLnBhcmVudCwgdGhpcy5hcmdzKTsKICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICB9KTsKICAgICAgICAgICAgICAgIHAuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IHRoaXMucGFyZW50LnBzICs9IGNodW5rLnRvU3RyaW5nKCk7IH0pOwogICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgIGNhc2UgJ2Rhcndpbic6CiAgICAgICAgICAgICAgICB2YXIgcHJvbWlzZSA9IHJlcXVpcmUoJ3Byb21pc2UnKTsKICAgICAgICAgICAgICAgIHZhciBwID0gbmV3IHByb21pc2UoZnVuY3Rpb24gKHJlcywgcmVqKSB7IHRoaXMuX3JlcyA9IHJlczsgdGhpcy5fcmVqID0gcmVqOyB9KTsKICAgICAgICAgICAgICAgIHAucG0gPSB0aGlzOwogICAgICAgICAgICAgICAgcC5jYWxsYmFjayA9IGNhbGxiYWNrOwogICAgICAgICAgICAgICAgcC5hcmdzID0gW107CiAgICAgICAgICAgICAgICBmb3IgKHZhciBpID0gMTsgaSA8IGFyZ3VtZW50cy5sZW5ndGg7ICsraSkgeyBwLmFyZ3MucHVzaChhcmd1bWVudHNbaV0pOyB9CiAgICAgICAgICAgICAgICBwLmNoaWxkID0gdGhpcy5fY2hpbGRQcm9jZXNzLmV4ZWNGaWxlKCIvYmluL3BzIiwgWyJwcyIsICIteGEiXSk7CiAgICAgICAgICAgICAgICBwLmNoaWxkLnByb21pc2UgPSBwOwogICAgICAgICAgICAgICAgcC5jaGlsZC5zdGRvdXQucHMgPSAnJzsKICAgICAgICAgICAgICAgIHAuY2hpbGQuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IHRoaXMucHMgKz0gY2h1bmsudG9TdHJpbmcoKTsgfSk7CiAgICAgICAgICAgICAgICBwLmNoaWxkLm9uKCdleGl0JywgZnVuY3Rpb24gKCkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICB2YXIgbGluZXMgPSB0aGlzLnN0ZG91dC5wcy5zcGxpdCgnXG4nKTsKICAgICAgICAgICAgICAgICAgICB2YXIgcGlkWCA9IGxpbmVzWzBdLnNwbGl0KCdQSUQnKVswXS5sZW5ndGggKyAzOwogICAgICAgICAgICAgICAgICAgIHZhciBjbWRYID0gbGluZXNbMF0uc3BsaXQoJ0NNRCcpWzBdLmxlbmd0aDsKICAgICAgICAgICAgICAgICAgICB2YXIgcmV0ID0ge307CiAgICAgICAgICAgICAgICAgICAgZm9yICh2YXIgaSA9IDE7IGkgPCBsaW5lcy5sZW5ndGg7ICsraSkKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIGlmIChsaW5lc1tpXS5sZW5ndGggPiAwKQogICAgICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICByZXRbbGluZXNbaV0uc3Vic3RyaW5nKDAsIHBpZFgpLnRyaW0oKV0gPSB7IHBpZDogbGluZXNbaV0uc3Vic3RyaW5nKDAsIHBpZFgpLnRyaW0oKSwgY21kOiBsaW5lc1tpXS5zdWJzdHJpbmcoY21kWCkgfTsKICAgICAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICB0aGlzLnByb21pc2UuX3JlcyhyZXQpOwogICAgICAgICAgICAgICAgfSk7CiAgICAgICAgICAgICAgICBwLnRoZW4oZnVuY3Rpb24gKHBzKQogICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgIHRoaXMuYXJncy51bnNoaWZ0KHBzKTsKICAgICAgICAgICAgICAgICAgICB0aGlzLmNhbGxiYWNrLmFwcGx5KHRoaXMucG0sIHRoaXMuYXJncyk7CiAgICAgICAgICAgICAgICB9KTsKICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgIH0KICAgIH07CgogICAgLy8gR2V0IGluZm9ybWF0aW9uIGFib3V0IGEgc3BlY2lmaWMgcHJvY2VzcyBvbiBMaW51eAogICAgdGhpcy5nZXRQcm9jZXNzSW5mbyA9IGZ1bmN0aW9uIGdldFByb2Nlc3NJbmZvKHBpZCkKICAgIHsKICAgICAgICBzd2l0Y2gocHJvY2Vzcy5wbGF0Zm9ybSkKICAgICAgICB7CiAgICAgICAgICAgIGRlZmF1bHQ6CiAgICAgICAgICAgICAgICB0aHJvdyAoJ2dldFByb2Nlc3NJbmZvKCkgbm90IHN1cHBvcnRlZCBmb3IgJyArIHByb2Nlc3MucGxhdGZvcm0pOwogICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgIGNhc2UgJ2xpbnV4JzoKICAgICAgICAgICAgICAgIHZhciBzdGF0dXMgPSByZXF1aXJlKCdmcycpLnJlYWRGaWxlU3luYygnL3Byb2MvJyArIHBpZCArICcvc3RhdHVzJyk7CiAgICAgICAgICAgICAgICB2YXIgaW5mbyA9IHt9OwogICAgICAgICAgICAgICAgdmFyIGxpbmVzID0gc3RhdHVzLnRvU3RyaW5nKCkuc3BsaXQoJ1xuJyk7CiAgICAgICAgICAgICAgICBmb3IodmFyIGkgaW4gbGluZXMpCiAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgdmFyIHRva2VucyA9IGxpbmVzW2ldLnNwbGl0KCc6Jyk7CiAgICAgICAgICAgICAgICAgICAgaWYgKHRva2Vucy5sZW5ndGggPiAxKSB7IHRva2Vuc1sxXSA9IHRva2Vuc1sxXS50cmltKCk7IH0KICAgICAgICAgICAgICAgICAgICBpbmZvW3Rva2Vuc1swXV0gPSB0b2tlbnNbMV07CiAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICByZXR1cm4gKGluZm8pOwogICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgfQogICAgfTsKfQoKbW9kdWxlLmV4cG9ydHMgPSBuZXcgcHJvY2Vzc01hbmFnZXIoKTs=', 'base64').toString());");
	char *_servicemanager = ILibMemory_Allocate(93737, 0, NULL, NULL);
	memcpy_s(_servicemanager + 0, 53564, "LyoKQ29weXJpZ2h0IDIwMTggSW50ZWwgQ29ycG9yYXRpb24KCkxpY2Vuc2VkIHVuZGVyIHRoZSBBcGFjaGUgTGljZW5zZSwgVmVyc2lvbiAyLjAgKHRoZSAiTGljZW5zZSIpOwp5b3UgbWF5IG5vdCB1c2UgdGhpcyBmaWxlIGV4Y2VwdCBpbiBjb21wbGlhbmNlIHdpdGggdGhlIExpY2Vuc2UuCllvdSBtYXkgb2J0YWluIGEgY29weSBvZiB0aGUgTGljZW5zZSBhdAoKICAgIGh0dHA6Ly93d3cuYXBhY2hlLm9yZy9saWNlbnNlcy9MSUNFTlNFLTIuMAoKVW5sZXNzIHJlcXVpcmVkIGJ5IGFwcGxpY2FibGUgbGF3IG9yIGFncmVlZCB0byBpbiB3cml0aW5nLCBzb2Z0d2FyZQpkaXN0cmlidXRlZCB1bmRlciB0aGUgTGljZW5zZSBpcyBkaXN0cmlidXRlZCBvbiBhbiAiQVMgSVMiIEJBU0lTLApXSVRIT1VUIFdBUlJBTlRJRVMgT1IgQ09ORElUSU9OUyBPRiBBTlkgS0lORCwgZWl0aGVyIGV4cHJlc3Mgb3IgaW1wbGllZC4KU2VlIHRoZSBMaWNlbnNlIGZvciB0aGUgc3BlY2lmaWMgbGFuZ3VhZ2UgZ292ZXJuaW5nIHBlcm1pc3Npb25zIGFuZApsaW1pdGF0aW9ucyB1bmRlciB0aGUgTGljZW5zZS4KKi8KCgpmdW5jdGlvbiBleHRyYWN0RmlsZU5hbWUoZmlsZVBhdGgpCnsKICAgIHZhciB0b2tlbnMgPSBmaWxlUGF0aC5zcGxpdCgnXFwnKS5qb2luKCcvJykuc3BsaXQoJy8nKTsKICAgIHZhciBuYW1lOwoKICAgIHdoaWxlICgobmFtZSA9IHRva2Vucy5wb3AoKSkgPT0gJycpOwogICAgcmV0dXJuIChuYW1lKTsKfQoKZnVuY3Rpb24gcGFyc2VTZXJ2aWNlU3RhdHVzKHRva2VuKQp7CiAgICB2YXIgaiA9IHt9OwogICAgdmFyIHNlcnZpY2VUeXBlID0gdG9rZW4uRGVyZWYoMCwgNCkuSW50VmFsOwogICAgai5pc0ZpbGVTeXN0ZW1Ecml2ZXIgPSAoKHNlcnZpY2VUeXBlICYgMHgwMDAwMDAwMikgPT0gMHgwMDAwMDAwMik7CiAgICBqLmlzS2VybmVsRHJpdmVyID0gKChzZXJ2aWNlVHlwZSAmIDB4MDAwMDAwMDEpID09IDB4MDAwMDAwMDEpOwogICAgai5pc1NoYXJlZFByb2Nlc3MgPSAoKHNlcnZpY2VUeXBlICYgMHgwMDAwMDAyMCkgPT0gMHgwMDAwMDAyMCk7CiAgICBqLmlzT3duUHJvY2VzcyA9ICgoc2VydmljZVR5cGUgJiAweDAwMDAwMDEwKSA9PSAweDAwMDAwMDEwKTsKICAgIGouaXNJbnRlcmFjdGl2ZSA9ICgoc2VydmljZVR5cGUgJiAweDAwMDAwMTAwKSA9PSAweDAwMDAwMTAwKTsKICAgIHN3aXRjaCAodG9rZW4uRGVyZWYoKDEgKiA0KSwgNCkudG9CdWZmZXIoKS5yZWFkVUludDMyTEUoKSkKICAgIHsKICAgICAgICBjYXNlIDB4MDAwMDAwMDU6CiAgICAgICAgICAgIGouc3RhdGUgPSAnQ09OVElOVUVfUEVORElORyc7CiAgICAgICAgICAgIGJyZWFrOwogICAgICAgIGNhc2UgMHgwMDAwMDAwNjoKICAgICAgICAgICAgai5zdGF0ZSA9ICdQQVVTRV9QRU5ESU5HJzsKICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgY2FzZSAweDAwMDAwMDA3OgogICAgICAgICAgICBqLnN0YXRlID0gJ1BBVVNFRCc7CiAgICAgICAgICAgIGJyZWFrOwogICAgICAgIGNhc2UgMHgwMDAwMDAwNDoKICAgICAgICAgICAgai5zdGF0ZSA9ICdSVU5OSU5HJzsKICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgY2FzZSAweDAwMDAwMDAyOgogICAgICAgICAgICBqLnN0YXRlID0gJ1NUQVJUX1BFTkRJTkcnOwogICAgICAgICAgICBicmVhazsKICAgICAgICBjYXNlIDB4MDAwMDAwMDM6CiAgICAgICAgICAgIGouc3RhdGUgPSAnU1RPUF9QRU5ESU5HJzsKICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgY2FzZSAweDAwMDAwMDAxOgogICAgICAgICAgICBqLnN0YXRlID0gJ1NUT1BQRUQnOwogICAgICAgICAgICBicmVhazsKICAgIH0KICAgIHZhciBjb250cm9sc0FjY2VwdGVkID0gdG9rZW4uRGVyZWYoKDIgKiA0KSwgNCkudG9CdWZmZXIoKS5yZWFkVUludDMyTEUoKTsKICAgIGouY29udHJvbHNBY2NlcHRlZCA9IFtdOwogICAgaWYgKChjb250cm9sc0FjY2VwdGVkICYgMHgwMDAwMDAxMCkgPT0gMHgwMDAwMDAxMCkKICAgIHsKICAgICAgICBqLmNvbnRyb2xzQWNjZXB0ZWQucHVzaCgnU0VSVklDRV9DT05UUk9MX05FVEJJTkRBREQnKTsKICAgICAgICBqLmNvbnRyb2xzQWNjZXB0ZWQucHVzaCgnU0VSVklDRV9DT05UUk9MX05FVEJJTkRSRU1PVkUnKTsKICAgICAgICBqLmNvbnRyb2xzQWNjZXB0ZWQucHVzaCgnU0VSVklDRV9DT05UUk9MX05FVEJJTkRFTkFCTEUnKTsKICAgICAgICBqLmNvbnRyb2xzQWNjZXB0ZWQucHVzaCgnU0VSVklDRV9DT05UUk9MX05FVEJJTkRESVNBQkxFJyk7CiAgICB9CiAgICBpZiAoKGNvbnRyb2xzQWNjZXB0ZWQgJiAweDAwMDAwMDA4KSA9PSAweDAwMDAwMDA4KSB7IGouY29udHJvbHNBY2NlcHRlZC5wdXNoKCdTRVJWSUNFX0NPTlRST0xfUEFSQU1DSEFOR0UnKTsgfQogICAgaWYgKChjb250cm9sc0FjY2VwdGVkICYgMHgwMDAwMDAwMikgPT0gMHgwMDAwMDAwMikgeyBqLmNvbnRyb2xzQWNjZXB0ZWQucHVzaCgnU0VSVklDRV9DT05UUk9MX1BBVVNFJyk7IGouY29udHJvbHNBY2NlcHRlZC5wdXNoKCdTRVJWSUNFX0NPTlRST0xfQ09OVElOVUUnKTsgfQogICAgaWYgKChjb250cm9sc0FjY2VwdGVkICYgMHgwMDAwMDEwMCkgPT0gMHgwMDAwMDEwMCkgeyBqLmNvbnRyb2xzQWNjZXB0ZWQucHVzaCgnU0VSVklDRV9DT05UUk9MX1BSRVNIVVRET1dOJyk7IH0KICAgIGlmICgoY29udHJvbHNBY2NlcHRlZCAmIDB4MDAwMDAwMDQpID09IDB4MDAwMDAwMDQpIHsgai5jb250cm9sc0FjY2VwdGVkLnB1c2goJ1NFUlZJQ0VfQ09OVFJPTF9TSFVURE9XTicpOyB9CiAgICBpZiAoKGNvbnRyb2xzQWNjZXB0ZWQgJiAweDAwMDAwMDAxKSA9PSAweDAwMDAwMDAxKSB7IGouY29udHJvbHNBY2NlcHRlZC5wdXNoKCdTRVJWSUNFX0NPTlRST0xfU1RPUCcpOyB9CiAgICBpZiAoKGNvbnRyb2xzQWNjZXB0ZWQgJiAweDAwMDAwMDIwKSA9PSAweDAwMDAwMDIwKSB7IGouY29udHJvbHNBY2NlcHRlZC5wdXNoKCdTRVJWSUNFX0NPTlRST0xfSEFSRFdBUkVQUk9GSUxFQ0hBTkdFJyk7IH0KICAgIGlmICgoY29udHJvbHNBY2NlcHRlZCAmIDB4MDAwMDAwNDApID09IDB4MDAwMDAwNDApIHsgai5jb250cm9sc0FjY2VwdGVkLnB1c2goJ1NFUlZJQ0VfQ09OVFJPTF9QT1dFUkVWRU5UJyk7IH0KICAgIGlmICgoY29udHJvbHNBY2NlcHRlZCAmIDB4MDAwMDAwODApID09IDB4MDAwMDAwODApIHsgai5jb250cm9sc0FjY2VwdGVkLnB1c2goJ1NFUlZJQ0VfQ09OVFJPTF9TRVNTSU9OQ0hBTkdFJyk7IH0KICAgIGoucGlkID0gdG9rZW4uRGVyZWYoKDcgKiA0KSwgNCkudG9CdWZmZXIoKS5yZWFkVUludDMyTEUoKTsKICAgIHJldHVybiAoaik7Cn0KCmZ1bmN0aW9uIHNlcnZpY2VNYW5hZ2VyKCkKewogICAgdGhpcy5fT2JqZWN0SUQgPSAnc2VydmljZS1tYW5hZ2VyJzsKICAgIGlmIChwcm9jZXNzLnBsYXRmb3JtID09ICd3aW4zMicpIAogICAgewogICAgICAgIHRoaXMuR00gPSByZXF1aXJlKCdfR2VuZXJpY01hcnNoYWwnKTsKICAgICAgICB0aGlzLnByb3h5ID0gdGhpcy5HTS5DcmVhdGVOYXRpdmVQcm94eSgnQWR2YXBpMzIuZGxsJyk7CiAgICAgICAgdGhpcy5wcm94eS5DcmVhdGVNZXRob2QoJ09wZW5TQ01hbmFnZXJBJyk7CiAgICAgICAgdGhpcy5wcm94eS5DcmVhdGVNZXRob2QoJ0VudW1TZXJ2aWNlc1N0YXR1c0V4QScpOwogICAgICAgIHRoaXMucHJveHkuQ3JlYXRlTWV0aG9kKCdPcGVuU2VydmljZUEnKTsKICAgICAgICB0aGlzLnByb3h5LkNyZWF0ZU1ldGhvZCgnUXVlcnlTZXJ2aWNlU3RhdHVzRXgnKTsKICAgICAgICB0aGlzLnByb3h5LkNyZWF0ZU1ldGhvZCgnQ29udHJvbFNlcnZpY2UnKTsKICAgICAgICB0aGlzLnByb3h5LkNyZWF0ZU1ldGhvZCgnU3RhcnRTZXJ2aWNlQScpOwogICAgICAgIHRoaXMucHJveHkuQ3JlYXRlTWV0aG9kKCdDbG9zZVNlcnZpY2VIYW5kbGUnKTsKICAgICAgICB0aGlzLnByb3h5LkNyZWF0ZU1ldGhvZCgnQ3JlYXRlU2VydmljZUEnKTsKICAgICAgICB0aGlzLnByb3h5LkNyZWF0ZU1ldGhvZCgnQ2hhbmdlU2VydmljZUNvbmZpZzJBJyk7CiAgICAgICAgdGhpcy5wcm94eS5DcmVhdGVNZXRob2QoJ0RlbGV0ZVNlcnZpY2UnKTsKICAgICAgICB0aGlzLnByb3h5LkNyZWF0ZU1ldGhvZCgnQWxsb2NhdGVBbmRJbml0aWFsaXplU2lkJyk7CiAgICAgICAgdGhpcy5wcm94eS5DcmVhdGVNZXRob2QoJ0NoZWNrVG9rZW5NZW1iZXJzaGlwJyk7CiAgICAgICAgdGhpcy5wcm94eS5DcmVhdGVNZXRob2QoJ0ZyZWVTaWQnKTsKCiAgICAgICAgdGhpcy5wcm94eTIgPSB0aGlzLkdNLkNyZWF0ZU5hdGl2ZVByb3h5KCdLZXJuZWwzMi5kbGwnKTsKICAgICAgICB0aGlzLnByb3h5Mi5DcmVhdGVNZXRob2QoJ0dldExhc3RFcnJvcicpOwoKICAgICAgICB0aGlzLmlzQWRtaW4gPSBmdW5jdGlvbiBpc0FkbWluKCkgewogICAgICAgICAgICB2YXIgTlRBdXRob3JpdHkgPSB0aGlzLkdNLkNyZWF0ZVZhcmlhYmxlKDYpOwogICAgICAgICAgICBOVEF1dGhvcml0eS50b0J1ZmZlcigpLndyaXRlSW50OCg1LCA1KTsKICAgICAgICAgICAgdmFyIEFkbWluaXN0cmF0b3JzR3JvdXAgPSB0aGlzLkdNLkNyZWF0ZVBvaW50ZXIoKTsKICAgICAgICAgICAgdmFyIGFkbWluID0gZmFsc2U7CgogICAgICAgICAgICBpZiAodGhpcy5wcm94eS5BbGxvY2F0ZUFuZEluaXRpYWxpemVTaWQoTlRBdXRob3JpdHksIDIsIDMyLCA1NDQsIDAsIDAsIDAsIDAsIDAsIDAsIEFkbWluaXN0cmF0b3JzR3JvdXApLlZhbCAhPSAwKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICB2YXIgbWVtYmVyID0gdGhpcy5HTS5DcmVhdGVJbnRlZ2VyKCk7CiAgICAgICAgICAgICAgICBpZiAodGhpcy5wcm94eS5DaGVja1Rva2VuTWVtYmVyc2hpcCgwLCBBZG1pbmlzdHJhdG9yc0dyb3VwLkRlcmVmKCksIG1lbWJlcikuVmFsICE9IDApCiAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgaWYgKG1lbWJlci50b0J1ZmZlcigpLnJlYWRVSW50MzJMRSgpICE9IDApIHsgYWRtaW4gPSB0cnVlOyB9CiAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICB0aGlzLnByb3h5LkZyZWVTaWQoQWRtaW5pc3RyYXRvcnNHcm91cC5EZXJlZigpKTsKICAgICAgICAgICAgfQogICAgICAgICAgICByZXR1cm4gYWRtaW47CiAgICAgICAgfTsKICAgICAgICB0aGlzLmdldFByb2dyYW1Gb2xkZXIgPSBmdW5jdGlvbiBnZXRQcm9ncmFtRm9sZGVyKCkKICAgICAgICB7CiAgICAgICAgICAgIGlmIChyZXF1aXJlKCdvcycpLmFyY2goKSA9PSAneDY0JykKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgLy8gNjQgYml0IFdpbmRvd3MKICAgICAgICAgICAgICAgIGlmICh0aGlzLkdNLlBvaW50ZXJTaXplID09IDQpCiAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgcmV0dXJuIHByb2Nlc3MuZW52WydQcm9ncmFtRmlsZXMoeDg2KSddOyAgICAvLyAzMiBCaXQgQXBwCiAgICAgICAgICAgICAgICB9IAogICAgICAgICAgICAgICAgcmV0dXJuIHByb2Nlc3MuZW52WydQcm9ncmFtRmlsZXMnXTsgICAgICAgICAgICAgLy8gNjQgYml0IEFwcAogICAgICAgICAgICB9CgogICAgICAgICAgICAvLyAzMiBiaXQgV2luZG93cwogICAgICAgICAgICByZXR1cm4gcHJvY2Vzcy5lbnZbJ1Byb2dyYW1GaWxlcyddOyAgICAgICAgICAgICAgICAgCiAgICAgICAgfTsKICAgICAgICB0aGlzLmdldFNlcnZpY2VGb2xkZXIgPSBmdW5jdGlvbiBnZXRTZXJ2aWNlRm9sZGVyKCkgeyByZXR1cm4gdGhpcy5nZXRQcm9ncmFtRm9sZGVyKCkgKyAnXFxtZXNoJzsgfTsKCiAgICAgICAgdGhpcy5lbnVtZXJhdGVTZXJ2aWNlID0gZnVuY3Rpb24gKCkgewogICAgICAgICAgICB2YXIgbWFjaGluZU5hbWUgPSB0aGlzLkdNLkNyZWF0ZVBvaW50ZXIoKTsKICAgICAgICAgICAgdmFyIGRiTmFtZSA9IHRoaXMuR00uQ3JlYXRlUG9pbnRlcigpOwogICAgICAgICAgICB2YXIgaGFuZGxlID0gdGhpcy5wcm94eS5PcGVuU0NNYW5hZ2VyQSgweDAwLCAweDAwLCAweDAwMDEgfCAweDAwMDQpOwoKICAgICAgICAgICAgdmFyIGJ5dGVzTmVlZGVkID0gdGhpcy5HTS5DcmVhdGVQb2ludGVyKCk7CiAgICAgICAgICAgIHZhciBzZXJ2aWNlc1JldHVybmVkID0gdGhpcy5HTS5DcmVhdGVQb2ludGVyKCk7CiAgICAgICAgICAgIHZhciByZXN1bWVIYW5kbGUgPSB0aGlzLkdNLkNyZWF0ZVBvaW50ZXIoKTsKICAgICAgICAgICAgLy92YXIgc2VydmljZXMgPSB0aGlzLnByb3h5LkNyZWF0ZVZhcmlhYmxlKDI2MjE0NCk7CiAgICAgICAgICAgIHZhciBzdWNjZXNzID0gdGhpcy5wcm94eS5FbnVtU2VydmljZXNTdGF0dXNFeEEoaGFuZGxlLCAwLCAweDAwMDAwMDMwLCAweDAwMDAwMDAzLCAweDAwLCAweDAwLCBieXRlc05lZWRlZCwgc2VydmljZXNSZXR1cm5lZCwgcmVzdW1lSGFuZGxlLCAweDAwKTsKICAgICAgICAgICAgaWYgKGJ5dGVzTmVlZGVkLkludFZhbCA8PSAwKSB7CiAgICAgICAgICAgICAgICB0aHJvdyAoJ2Vycm9yIGVudW1lcmF0aW5nIHNlcnZpY2VzJyk7CiAgICAgICAgICAgIH0KICAgICAgICAgICAgdmFyIHN6ID0gYnl0ZXNOZWVkZWQuSW50VmFsOwogICAgICAgICAgICB2YXIgc2VydmljZXMgPSB0aGlzLkdNLkNyZWF0ZVZhcmlhYmxlKHN6KTsKICAgICAgICAgICAgdGhpcy5wcm94eS5FbnVtU2VydmljZXNTdGF0dXNFeEEoaGFuZGxlLCAwLCAweDAwMDAwMDMwLCAweDAwMDAwMDAzLCBzZXJ2aWNlcywgc3osIGJ5dGVzTmVlZGVkLCBzZXJ2aWNlc1JldHVybmVkLCByZXN1bWVIYW5kbGUsIDB4MDApOwogICAgICAgICAgICBjb25zb2xlLmxvZygic2VydmljZXNSZXR1cm5lZCIsIHNlcnZpY2VzUmV0dXJuZWQuSW50VmFsKTsKCiAgICAgICAgICAgIHZhciBwdHJTaXplID0gZGJOYW1lLl9zaXplOwogICAgICAgICAgICB2YXIgYmxvY2tTaXplID0gMzYgKyAoMiAqIHB0clNpemUpOwogICAgICAgICAgICBibG9ja1NpemUgKz0gKChwdHJTaXplIC0gKGJsb2NrU2l6ZSAlIHB0clNpemUpKSAlIHB0clNpemUpOwogICAgICAgICAgICB2YXIgcmV0VmFsID0gW107CiAgICAgICAgICAgIGZvciAodmFyIGkgPSAwOyBpIDwgc2VydmljZXNSZXR1cm5lZC5JbnRWYWw7ICsraSkgewogICAgICAgICAgICAgICAgdmFyIHRva2VuID0gc2VydmljZXMuRGVyZWYoaSAqIGJsb2NrU2l6ZSwgYmxvY2tTaXplKTsKICAgICAgICAgICAgICAgIHZhciBqID0ge307CiAgICAgICAgICAgICAgICBqLm5hbWUgPSB0b2tlbi5EZXJlZigwLCBwdHJTaXplKS5EZXJlZigpLlN0cmluZzsKICAgICAgICAgICAgICAgIGouZGlzcGxheU5hbWUgPSB0b2tlbi5EZXJlZihwdHJTaXplLCBwdHJTaXplKS5EZXJlZigpLlN0cmluZzsKICAgICAgICAgICAgICAgIGouc3RhdHVzID0gcGFyc2VTZXJ2aWNlU3RhdHVzKHRva2VuLkRlcmVmKDIgKiBwdHJTaXplLCAzNikpOwogICAgICAgICAgICAgICAgcmV0VmFsLnB1c2goaik7CiAgICAgICAgICAgIH0KICAgICAgICAgICAgdGhpcy5wcm94eS5DbG9zZVNlcnZpY2VIYW5kbGUoaGFuZGxlKTsKICAgICAgICAgICAgcmV0dXJuIChyZXRWYWwpOwogICAgICAgIH0KICAgICAgICB0aGlzLmdldFNlcnZpY2UgPSBmdW5jdGlvbiAobmFtZSkgewogICAgICAgICAgICB2YXIgc2VydmljZU5hbWUgPSB0aGlzLkdNLkNyZWF0ZVZhcmlhYmxlKG5hbWUpOwogICAgICAgICAgICB2YXIgcHRyID0gdGhpcy5HTS5DcmVhdGVQb2ludGVyKCk7CiAgICAgICAgICAgIHZhciBieXRlc05lZWRlZCA9IHRoaXMuR00uQ3JlYXRlVmFyaWFibGUocHRyLl9zaXplKTsKICAgICAgICAgICAgdmFyIGhhbmRsZSA9IHRoaXMucHJveHkuT3BlblNDTWFuYWdlckEoMHgwMCwgMHgwMCwgMHgwMDAxIHwgMHgwMDA0IHwgMHgwMDIwIHwgMHgwMDEwKTsKICAgICAgICAgICAgaWYgKGhhbmRsZS5WYWwgPT0gMCkgeyB0aHJvdyAoJ2NvdWxkIG5vdCBvcGVuIFNlcnZpY2VNYW5hZ2VyJyk7IH0KICAgICAgICAgICAgdmFyIGggPSB0aGlzLnByb3h5Lk9wZW5TZXJ2aWNlQShoYW5kbGUsIHNlcnZpY2VOYW1lLCAweDAwMDQgfCAweDAwMjAgfCAweDAwMTAgfCAweDAwMDEwMDAwKTsKICAgICAgICAgICAgaWYgKGguVmFsICE9IDApIHsKICAgICAgICAgICAgICAgIHZhciBzdWNjZXNzID0gdGhpcy5wcm94eS5RdWVyeVNlcnZpY2VTdGF0dXNFeChoLCAwLCAwLCAwLCBieXRlc05lZWRlZCk7CiAgICAgICAgICAgICAgICB2YXIgc3RhdHVzID0gdGhpcy5HTS5DcmVhdGVWYXJpYWJsZShieXRlc05lZWRlZC50b0J1ZmZlcigpLnJlYWRVSW50MzJMRSgpKTsKICAgICAgICAgICAgICAgIHN1Y2Nlc3MgPSB0aGlzLnByb3h5LlF1ZXJ5U2VydmljZVN0YXR1c0V4KGgsIDAsIHN0YXR1cywgc3RhdHVzLl9zaXplLCBieXRlc05lZWRlZCk7CiAgICAgICAgICAgICAgICBpZiAoc3VjY2VzcyAhPSAwKSB7CiAgICAgICAgICAgICAgICAgICAgcmV0VmFsID0ge307CiAgICAgICAgICAgICAgICAgICAgcmV0VmFsLnN0YXR1cyA9IHBhcnNlU2VydmljZVN0YXR1cyhzdGF0dXMpOwogICAgICAgICAgICAgICAgICAgIHJldFZhbC5fc2NtID0gaGFuZGxlOwogICAgICAgICAgICAgICAgICAgIHJldFZhbC5fc2VydmljZSA9IGg7CiAgICAgICAgICAgICAgICAgICAgcmV0VmFsLl9HTSA9IHRoaXMuR007CiAgICAgICAgICAgICAgICAgICAgcmV0VmFsLl9wcm94eSA9IHRoaXMucHJveHk7CiAgICAgICAgICAgICAgICAgICAgcmVxdWlyZSgnZXZlbnRzJykuaW5oZXJpdHMocmV0VmFsKTsKICAgICAgICAgICAgICAgICAgICByZXRWYWwub24oJ34nLCBmdW5jdGlvbiAoKSB7IHRoaXMuX3Byb3h5LkNsb3NlU2VydmljZUhhbmRsZSh0aGlzKTsgdGhpcy5fcHJveHkuQ2xvc2VTZXJ2aWNlSGFuZGxlKHRoaXMuX3NjbSk7IH0pOwogICAgICAgICAgICAgICAgICAgIHJldFZhbC5uYW1lID0gbmFtZTsKICAgICAgICAgICAgICAgICAgICByZXRWYWwuaXNSdW5uaW5nID0gZnVuY3Rpb24gKCkKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIHZhciBieXRlc05lZWRlZCA9IHRoaXMuX0dNLkNyZWF0ZVZhcmlhYmxlKHRoaXMuX0dNLlBvaW50ZXJTaXplKTsKICAgICAgICAgICAgICAgICAgICAgICAgdGhpcy5fcHJveHkuUXVlcnlTZXJ2aWNlU3RhdHVzRXgodGhpcy5fc2VydmljZSwgMCwgMCwgMCwgYnl0ZXNOZWVkZWQpOwogICAgICAgICAgICAgICAgICAgICAgICB2YXIgc3QgPSB0aGlzLl9HTS5DcmVhdGVWYXJpYWJsZShieXRlc05lZWRlZC50b0J1ZmZlcigpLnJlYWRVSW50MzJMRSgpKTsKICAgICAgICAgICAgICAgICAgICAgICAgaWYodGhpcy5fcHJveHkuUXVlcnlTZXJ2aWNlU3RhdHVzRXgodGhpcy5fc2VydmljZSwgMCwgc3QsIHN0Ll9zaXplLCBieXRlc05lZWRlZCkuVmFsICE9IDApCiAgICAgICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHZhciBzdGF0ZSA9IHBhcnNlU2VydmljZVN0YXR1cyhzdCk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICByZXR1cm4gKHN0YXRlLnN0YXRlID09ICdSVU5OSU5HJyk7CiAgICAgICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICAgICAgcmV0dXJuIChmYWxzZSk7CiAgICAgICAgICAgICAgICAgICAgfTsKICAgICAgICAgICAgICAgICAgICByZXRWYWwuc3RvcCA9IGZ1bmN0aW9uICgpIHsKICAgICAgICAgICAgICAgICAgICAgICAgaWYgKHRoaXMuc3RhdHVzLnN0YXRlID09ICdSVU5OSU5HJykgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgdmFyIG5ld3N0YXRlID0gdGhpcy5fR00uQ3JlYXRlVmFyaWFibGUoMzYpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgdmFyIHN1Y2Nlc3MgPSB0aGlzLl9wcm94eS5Db250cm9sU2VydmljZSh0aGlzLl9zZXJ2aWNlLCAweDAwMDAwMDAxLCBuZXdzdGF0ZSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBpZiAoc3VjY2VzcyA9PSAwKSB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgdGhyb3cgKHRoaXMubmFtZSArICcuc3RvcCgpIGZhaWxlZCcpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgIGVsc2UgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgdGhyb3cgKCdjYW5ub3QgY2FsbCAnICsgdGhpcy5uYW1lICsgJy5zdG9wKCksIHdoZW4gY3VycmVudCBzdGF0ZSBpczogJyArIHRoaXMuc3RhdHVzLnN0YXRlKTsKICAgICAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICByZXRWYWwuc3RhcnQgPSBmdW5jdGlvbiAoKSB7CiAgICAgICAgICAgICAgICAgICAgICAgIGlmICh0aGlzLnN0YXR1cy5zdGF0ZSA9PSAnU1RPUFBFRCcpIHsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHZhciBzdWNjZXNzID0gdGhpcy5fcHJveHkuU3RhcnRTZXJ2aWNlQSh0aGlzLl9zZXJ2aWNlLCAwLCAwKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIGlmIChzdWNjZXNzID09IDApIHsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICB0aHJvdyAodGhpcy5uYW1lICsgJy5zdGFydCgpIGZhaWxlZCcpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgIGVsc2UgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgdGhyb3cgKCdjYW5ub3QgY2FsbCAnICsgdGhpcy5uYW1lICsgJy5zdGFydCgpLCB3aGVuIGN1cnJlbnQgc3RhdGUgaXM6ICcgKyB0aGlzLnN0YXR1cy5zdGF0ZSk7CiAgICAgICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgcmV0dXJuIChyZXRWYWwpOwogICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgZWxzZSB7CgogICAgICAgICAgICAgICAgfQogICAgICAgICAgICB9CgogICAgICAgICAgICB0aGlzLnByb3h5LkNsb3NlU2VydmljZUhhbmRsZShoYW5kbGUpOwogICAgICAgICAgICB0aHJvdyAoJ2NvdWxkIG5vdCBmaW5kIHNlcnZpY2U6ICcgKyBuYW1lKTsKICAgICAgICB9CiAgICB9CiAgICBlbHNlCiAgICB7CiAgICAgICAgdGhpcy5pc0FkbWluID0gZnVuY3Rpb24gaXNBZG1pbigpIAogICAgICAgIHsKICAgICAgICAgICAgcmV0dXJuIChyZXF1aXJlKCd1c2VyLXNlc3Npb25zJykuaXNSb290KCkpOwogICAgICAgIH0KICAgICAgICBpZihwcm9jZXNzLnBsYXRmb3JtID09ICdsaW51eCcpCiAgICAgICAgewogICAgICAgICAgICB0aGlzLmdldFNlcnZpY2UgPSBmdW5jdGlvbiAobmFtZSwgcGxhdGZvcm0pCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIGlmICghcGxhdGZvcm0pIHsgcGxhdGZvcm0gPSB0aGlzLmdldFNlcnZpY2VUeXBlKCk7IH0KICAgICAgICAgICAgICAgIHZhciByZXQgPSB7IG5hbWU6IG5hbWUgfTsKICAgICAgICAgICAgICAgIHN3aXRjaChwbGF0Zm9ybSkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICBjYXNlICdpbml0JzoKICAgICAgICAgICAgICAgICAgICBjYXNlICd1cHN0YXJ0JzoKICAgICAgICAgICAgICAgICAgICAgICAgaWYgKChwbGF0Zm9ybSA9PSAnaW5pdCcgJiYgcmVxdWlyZSgnZnMnKS5leGlzdHNTeW5jKCcvZXRjL2luaXQuZC8nICsgbmFtZSkpIHx8CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAocGxhdGZvcm0gPT0gJ3Vwc3RhcnQnICYmIHJlcXVpcmUoJ2ZzJykuZXhpc3RzU3luYygnL2V0Yy9pbml0LycgKyBuYW1lICsgJy5jb25mJykpKQogICAgICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICByZXQuaXNSdW5uaW5nID0gZnVuY3Rpb24gaXNSdW5uaW5nKCkKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICB2YXIgY2hpbGQgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy9iaW4vc2gnLCBbJ3NoJ10pOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5zdHIg", 16000);
	memcpy_s(_servicemanager + 16000, 37564, "PSAnJzsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC5zdGRvdXQub24oJ2RhdGEnLCBmdW5jdGlvbiAoY2h1bmspIHsgdGhpcy5zdHIgKz0gY2h1bmsudG9TdHJpbmcoKTsgfSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoInNlcnZpY2UgIiArIHRoaXMubmFtZSArICIgc3RhdHVzIHwgYXdrICd7cHJpbnQgJDJ9JyB8IGF3ayAtRiwgJ3twcmludCAkMX0nXG5leGl0XG4iKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC53YWl0RXhpdCgpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJldHVybiAoY2hpbGQuc3Rkb3V0LnN0ci50cmltKCkgPT0gJ3N0YXJ0L3J1bm5pbmcnKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIH07CiAgICAgICAgICAgICAgICAgICAgICAgICAgICByZXQuc3RhcnQgPSBmdW5jdGlvbiBzdGFydCgpCiAgICAgICAgICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgdmFyIGNoaWxkID0gcmVxdWlyZSgnY2hpbGRfcHJvY2VzcycpLmV4ZWNGaWxlKCcvYmluL3NoJywgWydzaCddKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC5zdGRvdXQub24oJ2RhdGEnLCBmdW5jdGlvbiAoY2h1bmspIHsgfSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ3NlcnZpY2UgJyArIHRoaXMubmFtZSArICcgc3RhcnRcbmV4aXRcbicpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLndhaXRFeGl0KCk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB9OwogICAgICAgICAgICAgICAgICAgICAgICAgICAgcmV0LnN0b3AgPSBmdW5jdGlvbiBzdG9wKCkKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICB2YXIgY2hpbGQgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy9iaW4vc2gnLCBbJ3NoJ10pOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5vbignZGF0YScsIGZ1bmN0aW9uIChjaHVuaykgeyB9KTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC5zdGRpbi53cml0ZSgnc2VydmljZSAnICsgdGhpcy5uYW1lICsgJyBzdG9wXG5leGl0XG4nKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC53YWl0RXhpdCgpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgfTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJldC5yZXN0YXJ0ID0gZnVuY3Rpb24gcmVzdGFydCgpCiAgICAgICAgICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgdmFyIGNoaWxkID0gcmVxdWlyZSgnY2hpbGRfcHJvY2VzcycpLmV4ZWNGaWxlKCcvYmluL3NoJywgWydzaCddKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC5zdGRvdXQub24oJ2RhdGEnLCBmdW5jdGlvbiAoY2h1bmspIHsgfSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ3NlcnZpY2UgJyArIHRoaXMubmFtZSArICcgcmVzdGFydFxuZXhpdFxuJyk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQud2FpdEV4aXQoKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIH07CiAgICAgICAgICAgICAgICAgICAgICAgICAgICByZXQuc3RhdHVzID0gZnVuY3Rpb24gc3RhdHVzKCkKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICB2YXIgY2hpbGQgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy9iaW4vc2gnLCBbJ3NoJ10pOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5fc3RyID0gJyc7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IHRoaXMuX3N0ciArPSBjaHVuay50b1N0cmluZygpOyB9KTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC5zdGRpbi53cml0ZSgnc2VydmljZSAnICsgdGhpcy5uYW1lICsgJyBzdGF0dXNcbmV4aXRcbicpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLndhaXRFeGl0KCk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgcmV0dXJuIChjaGlsZC5zdGRvdXQuX3N0cik7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB9OwogICAgICAgICAgICAgICAgICAgICAgICAgICAgcmV0dXJuIChyZXQpOwogICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgIGVsc2UKICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgdGhyb3cgKHBsYXRmb3JtICsgJyBTZXJ2aWNlICgnICsgbmFtZSArICcpIE5PVCBGT1VORCcpOwogICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgIGNhc2UgJ3N5c3RlbWQnOgogICAgICAgICAgICAgICAgICAgICAgICBpZiAocmVxdWlyZSgnZnMnKS5leGlzdHNTeW5jKCcvbGliL3N5c3RlbWQvc3lzdGVtLycgKyBuYW1lICsgJy5zZXJ2aWNlJykgfHwKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykuZXhpc3RzU3luYygnL3Vzci9saWIvc3lzdGVtZC9zeXN0ZW0vJyArIG5hbWUgKyAnLnNlcnZpY2UnKSkKICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgcmV0LmlzUnVubmluZyA9IGZ1bmN0aW9uIGlzUnVubmluZygpCiAgICAgICAgICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgdmFyIGNoaWxkID0gcmVxdWlyZSgnY2hpbGRfcHJvY2VzcycpLmV4ZWNGaWxlKCcvYmluL3NoJywgWydzaCddKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC5zdGRvdXQuc3RyID0gJyc7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IHRoaXMuc3RyICs9IGNodW5rLnRvU3RyaW5nKCk7IH0pOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLnN0ZGluLndyaXRlKCJzeXN0ZW1jdGwgc3RhdHVzICIgKyB0aGlzLm5hbWUgKyAiIHwgZ3JlcCAnQWN0aXZlOicgfCBhd2sgJ3twcmludCAkMn0nXG5leGl0XG4iKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC53YWl0RXhpdCgpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJldHVybiAoY2hpbGQuc3Rkb3V0LnN0ci50cmltKCkgPT0gJ2FjdGl2ZScpOyAgICAgICAgIAogICAgICAgICAgICAgICAgICAgICAgICAgICAgfTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJldC5zdGFydCA9IGZ1bmN0aW9uIHN0YXJ0KCkgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHZhciBjaGlsZCA9IHJlcXVpcmUoJ2NoaWxkX3Byb2Nlc3MnKS5leGVjRmlsZSgnL2Jpbi9zaCcsIFsnc2gnXSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IH0pOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLnN0ZGluLndyaXRlKCdzeXN0ZW1jdGwgc3RhcnQgJyArIHRoaXMubmFtZSArICdcbmV4aXRcbicpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLndhaXRFeGl0KCk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB9OwogICAgICAgICAgICAgICAgICAgICAgICAgICAgcmV0LnN0b3AgPSBmdW5jdGlvbiBzdG9wKCkgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHZhciBjaGlsZCA9IHJlcXVpcmUoJ2NoaWxkX3Byb2Nlc3MnKS5leGVjRmlsZSgnL2Jpbi9zaCcsIFsnc2gnXSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IH0pOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLnN0ZGluLndyaXRlKCdzeXN0ZW1jdGwgc3RvcCAnICsgdGhpcy5uYW1lICsgJ1xuZXhpdFxuJyk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQud2FpdEV4aXQoKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIH07CiAgICAgICAgICAgICAgICAgICAgICAgICAgICByZXQucmVzdGFydCA9IGZ1bmN0aW9uIHJlc3RhcnQoKSB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgdmFyIGNoaWxkID0gcmVxdWlyZSgnY2hpbGRfcHJvY2VzcycpLmV4ZWNGaWxlKCcvYmluL3NoJywgWydzaCddKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC5zdGRvdXQub24oJ2RhdGEnLCBmdW5jdGlvbiAoY2h1bmspIHsgfSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ3N5c3RlbWN0bCByZXN0YXJ0ICcgKyB0aGlzLm5hbWUgKyAnXG5leGl0XG4nKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC53YWl0RXhpdCgpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgfTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJldC5zdGF0dXMgPSBmdW5jdGlvbiBzdGF0dXMoKSB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgdmFyIGNoaWxkID0gcmVxdWlyZSgnY2hpbGRfcHJvY2VzcycpLmV4ZWNGaWxlKCcvYmluL3NoJywgWydzaCddKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjaGlsZC5zdGRvdXQuX3N0ciA9ICcnOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5vbignZGF0YScsIGZ1bmN0aW9uIChjaHVuaykgeyB0aGlzLl9zdHIgKz0gY2h1bmsudG9TdHJpbmcoKTsgfSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ3N5c3RlbWN0bCBzdGF0dXMgJyArIHRoaXMubmFtZSArICdcbmV4aXRcbicpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLndhaXRFeGl0KCk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgcmV0dXJuIChjaGlsZC5zdGRvdXQuX3N0cik7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB9OwogICAgICAgICAgICAgICAgICAgICAgICAgICAgcmV0dXJuIChyZXQpOwogICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgIGVsc2UKICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgdGhyb3cgKHBsYXRmb3JtICsgJyBTZXJ2aWNlICgnICsgbmFtZSArICcpIE5PVCBGT1VORCcpOwogICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgIGRlZmF1bHQ6CiAgICAgICAgICAgICAgICAgICAgICAgIHRocm93ICgnVW5rbm93biBTZXJ2aWNlIFBsYXRmb3JtOiAnICsgcGxhdGZvcm0pOwogICAgICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgfTsKICAgICAgICB9CiAgICB9CiAgICB0aGlzLmluc3RhbGxTZXJ2aWNlID0gZnVuY3Rpb24gaW5zdGFsbFNlcnZpY2Uob3B0aW9ucykKICAgIHsKICAgICAgICBpZiAocHJvY2Vzcy5wbGF0Zm9ybSA9PSAnd2luMzInKQogICAgICAgIHsKICAgICAgICAgICAgaWYgKCF0aGlzLmlzQWRtaW4oKSkgeyB0aHJvdyAoJ0luc3RhbGxpbmcgYXMgU2VydmljZSwgcmVxdWlyZXMgYWRtaW4nKTsgfQoKICAgICAgICAgICAgLy8gQmVmb3JlIHdlIHN0YXJ0LCB3ZSBuZWVkIHRvIGNvcHkgdGhlIGJpbmFyeSB0byB0aGUgcmlnaHQgcGxhY2UKICAgICAgICAgICAgdmFyIGZvbGRlciA9IHRoaXMuZ2V0U2VydmljZUZvbGRlcigpOwogICAgICAgICAgICBpZiAoIXJlcXVpcmUoJ2ZzJykuZXhpc3RzU3luYyhmb2xkZXIpKSB7IHJlcXVpcmUoJ2ZzJykubWtkaXJTeW5jKGZvbGRlcik7IH0KICAgICAgICAgICAgaWYgKCFyZXF1aXJlKCdmcycpLmV4aXN0c1N5bmMoZm9sZGVyICsgJ1xcJyArIG9wdGlvbnMubmFtZSkpIHsgcmVxdWlyZSgnZnMnKS5ta2RpclN5bmMoZm9sZGVyICsgJ1xcJyArIG9wdGlvbnMubmFtZSk7IH0KCiAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykuY29weUZpbGVTeW5jKG9wdGlvbnMuc2VydmljZVBhdGgsIGZvbGRlciArICdcXCcgKyBvcHRpb25zLm5hbWUgKyAnXFwnICsgb3B0aW9ucy5uYW1lICsgJy5leGUnKTsKICAgICAgICAgICAgb3B0aW9ucy5zZXJ2aWNlUGF0aCA9IGZvbGRlciArICdcXCcgKyBvcHRpb25zLm5hbWUgKyAnXFwnICsgb3B0aW9ucy5uYW1lICsgJy5leGUnOwoKICAgICAgICAgICAgdmFyIHNlcnZpY2VQYXRoID0gdGhpcy5HTS5DcmVhdGVWYXJpYWJsZSgnIicgKyBvcHRpb25zLnNlcnZpY2VQYXRoICsgJyInKTsKICAgICAgICAgICAgdmFyIGhhbmRsZSA9IHRoaXMucHJveHkuT3BlblNDTWFuYWdlckEoMHgwMCwgMHgwMCwgMHgwMDAyKTsKICAgICAgICAgICAgaWYgKGhhbmRsZS5WYWwgPT0gMCkgeyB0aHJvdyAoJ2Vycm9yIG9wZW5pbmcgU0NNYW5hZ2VyJyk7IH0KICAgICAgICAgICAgdmFyIHNlcnZpY2VOYW1lID0gdGhpcy5HTS5DcmVhdGVWYXJpYWJsZShvcHRpb25zLm5hbWUpOwogICAgICAgICAgICB2YXIgZGlzcGxheU5hbWUgPSB0aGlzLkdNLkNyZWF0ZVZhcmlhYmxlKG9wdGlvbnMubmFtZSk7CiAgICAgICAgICAgIHZhciBhbGxBY2Nlc3MgPSAweDAwMEYwMUZGOwogICAgICAgICAgICB2YXIgc2VydmljZVR5cGU7CiAgICAgICAgICAgIAoKICAgICAgICAgICAgc3dpdGNoIChvcHRpb25zLnN0YXJ0VHlwZSkgewogICAgICAgICAgICAgICAgY2FzZSAnQk9PVF9TVEFSVCc6CiAgICAgICAgICAgICAgICAgICAgc2VydmljZVR5cGUgPSAweDAwOwogICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgY2FzZSAnU1lTVEVNX1NUQVJUJzoKICAgICAgICAgICAgICAgICAgICBzZXJ2aWNlVHlwZSA9IDB4MDE7CiAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICBjYXNlICdBVVRPX1NUQVJUJzoKICAgICAgICAgICAgICAgICAgICBzZXJ2aWNlVHlwZSA9IDB4MDI7CiAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICBjYXNlICdERU1BTkRfU1RBUlQnOgogICAgICAgICAgICAgICAgICAgIHNlcnZpY2VUeXBlID0gMHgwMzsKICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgIGRlZmF1bHQ6CiAgICAgICAgICAgICAgICAgICAgc2VydmljZVR5cGUgPSAweDA0OyAvLyBEaXNhYmxlZAogICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICB9CgogICAgICAgICAgICB2YXIgaCA9IHRoaXMucHJveHkuQ3JlYXRlU2VydmljZUEoaGFuZGxlLCBzZXJ2aWNlTmFtZSwgZGlzcGxheU5hbWUsIGFsbEFjY2VzcywgMHgxMCB8IDB4MTAwLCBzZXJ2aWNlVHlwZSwgMCwgc2VydmljZVBhdGgsIDAsIDAsIDAsIDAsIDApOwogICAgICAgICAgICBpZiAoaC5WYWwgPT0gMCkgeyB0aGlzLnByb3h5LkNsb3NlU2VydmljZUhhbmRsZShoYW5kbGUpOyB0aHJvdyAoJ0Vycm9yIENyZWF0aW5nIFNlcnZpY2U6ICcgKyB0aGlzLnByb3h5Mi5HZXRMYXN0RXJyb3IoKS5WYWwpOyB9CiAgICAgICAgICAgIGlmIChvcHRpb25zLmRlc2NyaXB0aW9uKSB7CiAgICAgICAgICAgICAgICBjb25zb2xlLmxvZyhvcHRpb25zLmRlc2NyaXB0aW9uKTsKCiAgICAgICAgICAgICAgICB2YXIgZHNjUHRyID0gdGhpcy5HTS5DcmVhdGVQb2ludGVyKCk7CiAgICAgICAgICAgICAgICBkc2NQdHIuVmFsID0gdGhpcy5HTS5DcmVhdGVWYXJpYWJsZShvcHRpb25zLmRlc2NyaXB0aW9uKTsKCiAgICAgICAgICAgICAgICBpZiAodGhpcy5wcm94eS5DaGFuZ2VTZXJ2aWNlQ29uZmlnMkEoaCwgMSwgZHNjUHRyKSA9PSAwKSB7CiAgICAgICAgICAgICAgICAgICAgdGhpcy5wcm94eS5DbG9zZVNlcnZpY2VIYW5kbGUoaCk7CiAgICAgICAgICAgICAgICAgICAgdGhpcy5wcm94eS5DbG9zZVNlcnZpY2VIYW5kbGUoaGFuZGxlKTsKICAgICAgICAgICAgICAgICAgICB0aHJvdyAoJ1VuYWJsZSB0byBzZXQgZGVzY3JpcHRpb24nKTsKICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgfQogICAgICAgICAgICB0aGlzLnByb3h5LkNsb3NlU2VydmljZUhhbmRsZShoKTsKICAgICAgICAgICAgdGhpcy5wcm94eS5DbG9zZVNlcnZpY2VIYW5kbGUoaGFuZGxlKTsKCiAgICAgICAgICAgIGlmIChvcHRpb25zLmZpbGVzKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICBmb3IodmFyIGkgaW4gb3B0aW9ucy5maWxlcykKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICBjb25zb2xlLmxvZygnY29weWluZyAnICsgb3B0aW9ucy5maWxlc1tpXSk7CiAgICAgICAgICAgICAgICAgICAgcmVxdWlyZSgnZnMnKS5jb3B5RmlsZVN5bmMob3B0aW9ucy5maWxlc1tpXSwgZm9sZGVyICsgJ1xcJyArIG9wdGlvbnMubmFtZSArICdcXCcgKyBleHRyYWN0RmlsZU5hbWUob3B0aW9ucy5maWxlc1tpXSkpOwogICAgICAgICAgICAgICAgfQogICAgICAgICAgICB9CiAgICAgICAgICAgIGlmIChvcHRpb25zLnBhcmFtZXRlcnMpCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIHZhciByZWcgPSByZXF1aXJlKCd3aW4tcmVnaXN0cnknKTsKICAgICAgICAgICAgICAgIHZhciBpbWFnZVBhdGggPSByZWcuUXVlcnlLZXkocmVnLkhLRVkuTG9jYWxNYWNoaW5lLCAnU1lTVEVNXFxDdXJyZW50Q29udHJvbFNldFxcU2VydmljZXNcXCcgKyBvcHRpb25zLm5hbWUsICdJbWFnZVBhdGgnKTsKICAgICAgICAgICAgICAgIGltYWdlUGF0aCArPSAoJyAnICsgb3B0aW9ucy5wYXJhbWV0ZXJzLmpvaW4oJyAnKSk7CiAgICAgICAgICAgICAgICByZWcuV3JpdGVLZXkocmVnLkhLRVkuTG9jYWxNYWNoaW5lLCAnU1lTVEVNXFxDdXJyZW50Q29udHJvbFNldFxcU2VydmljZXNcXCcgKyBvcHRpb25zLm5hbWUsICdJbWFnZVBhdGgnLCBpbWFnZVBhdGgpOwogICAgICAgICAgICB9CgogICAgICAgICAgICByZXR1cm4gKHRoaXMuZ2V0U2VydmljZShvcHRpb25zLm5hbWUpKTsKICAgICAgICB9CiAgICAgICAgaWYocHJvY2Vzcy5wbGF0Zm9ybSA9PSAnbGludXgnKQogICAgICAgIHsKICAgICAgICAgICAgaWYgKCF0aGlzLmlzQWRtaW4oKSkgeyBjb25zb2xlLmxvZygnSW5zdGFsbGluZyBhIFNlcnZpY2UgcmVxdWlyZXMgcm9vdCcpOyB0aHJvdyAoJ0luc3RhbGxpbmcgYXMgU2VydmljZSwgcmVxdWlyZXMgcm9vdCcpOyB9CiAgICAgICAgICAgIHZhciBwYXJhbWV0ZXJzID0gb3B0aW9ucy5wYXJhbWV0ZXJzID8gb3B0aW9ucy5wYXJhbWV0ZXJzLmpvaW4oJyAnKSA6ICcnOwogICAgICAgICAgICB2YXIgY29uZjsKICAgICAgICAgICAgaWYgKCFvcHRpb25zLnNlcnZpY2VQbGF0Zm9ybSkgeyBvcHRpb25zLnNlcnZpY2VQbGF0Zm9ybSA9IHRoaXMuZ2V0U2VydmljZVR5cGUoKTsgfQogICAgICAgICAgIAogICAgICAgICAgICBzd2l0Y2ggKG9wdGlvbnMuc2VydmljZVBsYXRmb3JtKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICBjYXNlICdpbml0JzoKICAgICAgICAgICAgICAgICAgICBpZiAoIXJlcXVpcmUoJ2ZzJykuZXhpc3RzU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycpKSB7IHJlcXVpcmUoJ2ZzJykubWtkaXJTeW5jKCcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMnKTsgfQogICAgICAgICAgICAgICAgICAgIGlmICghcmVxdWlyZSgnZnMnKS5leGlzdHNTeW5jKCcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSkpIHsgcmVxdWlyZSgnZnMnKS5ta2RpclN5bmMoJy91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcy8nICsgb3B0aW9ucy5uYW1lKTsgfQoKICAgICAgICAgICAgICAgICAgICByZXF1aXJlKCdmcycpLmNvcHlGaWxlU3luYyhvcHRpb25zLnNlcnZpY2VQYXRoLCAnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBvcHRpb25zLm5hbWUgKyAnLycgKyBvcHRpb25zLm5hbWUpOwogICAgICAgICAgICAgICAgICAgIGNvbnNvbGUubG9nKCdjb3B5aW5nICcgKyBvcHRpb25zLnNlcnZpY2VQYXRoKTsKCiAgICAgICAgICAgICAgICAgICAgdmFyIG0gPSByZXF1aXJlKCdmcycpLnN0YXRTeW5jKCcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSArICcvJyArIG9wdGlvbnMubmFtZSkubW9kZTsKICAgICAgICAgICAgICAgICAgICBtIHw9IChyZXF1aXJlKCdmcycpLkNITU9EX01PREVTLlNfSVhVU1IgfCByZXF1aXJlKCdmcycpLkNITU9EX01PREVTLlNfSVhHUlApOwogICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykuY2htb2RTeW5jKCcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSArICcvJyArIG9wdGlvbnMubmFtZSwgbSk7CgogICAgICAgICAgICAgICAgICAgIC8vIFRoZSBmb2xsb3dpbmcgaXMgdGhlIGluaXQuZCBzY3JpcHQgSSB3cm90ZS4gUmF0aGVyIHRoYW4gaGF2aW5nIHRvIGRlYWwgd2l0aCBlc2NhcGluZyB0aGUgdGhpbmcsIEkganVzdCBCYXNlNjQgZW5jb2RlZCBpdCB0byBwcmV2ZW50IGlzc3Vlcy4KICAgICAgICAgICAgICAgICAgICBjb25mID0gcmVxdWlyZSgnZnMnKS5jcmVhdGVXcml0ZVN0cmVhbSgnL2V0Yy9pbml0LmQvJyArIG9wdGlvbnMubmFtZSwgeyBmbGFnczogJ3diJyB9KTsKICAgICAgICAgICAgICAgICAgICBjb25mLndyaXRlKEJ1ZmZlci5mcm9tKCdJeUV2WW1sdUwzTm9DZ29LVTBOU1NWQlVQUzkxYzNJdmJHOWpZV3d2YldWemFGOXpaWEoyYVdObGN5OVlXRmhZV0M5WVdGaFlXQXBTVlU1QlV6MXliMjkwQ2dwUVNVUkdTVXhGUFM5MllYSXZjblZ1TDFoWVdGaFlMbkJwWkFwTVQwZEdTVXhGUFM5MllYSXZiRzluTDFoWVdGaFlMbXh2WndvS2MzUmhjblFvS1NCN0NpQWdhV1lnV3lBdFppQWlKRkJKUkVaSlRFVWlJRjBnSmlZZ2EybHNiQ0F0TUNBa0tHTmhkQ0FpSkZCSlJFWkpURVVpS1NBeVBpOWtaWFl2Ym5Wc2JEc2dkR2hsYmdvZ0lDQWdaV05vYnlBblUyVnlkbWxqWlNCaGJISmxZV1I1SUhKMWJtNXBibWNuSUQ0bU1nb2dJQ0FnY21WMGRYSnVJREVLSUNCbWFRb2dJR1ZqYUc4Z0oxTjBZWEowYVc1bklITmxjblpwWTJYaWdLWW5JRDRtTWdvZ0lHeHZZMkZzSUVOTlJEMGlKRk5EVWtsUVZDQjdlMUJCVWsxVGZYMGdKajRnWENJa1RFOUhSa2xNUlZ3aUlDWWdaV05vYnlCY0pDRWlDaUFnYzNVZ0xXTWdJaVJEVFVRaUlDUlNWVTVCVXlBK0lDSWtVRWxFUmtsTVJTSUtJQ0JsWTJodklDZFRaWEoyYVdObElITjBZWEowWldRbklENG1NZ3A5Q2dwemRHOXdLQ2tnZXdvZ0lHbG1JRnNnSVNBdFppQWlKRkJKUkVaSlRFVWlJRjA3SUhSb1pXNEtJQ0FnSUdWamFHOGdKMU5sY25acFkyVWdibTkwSUhKMWJtNXBibWNuSUQ0bU1nb2dJQ0FnY21WMGRYSnVJREVLSUNCbGJITmxDZ2x3YVdROUpDZ2dZMkYwSUNJa1VFbEVSa2xNUlNJZ0tRb0phV1lnYTJsc2JDQXRNQ0FrY0dsa0lESStMMlJsZGk5dWRXeHNPeUIwYUdWdUNpQWdJQ0FnSUdWamFHOGdKMU4wYjNCd2FXNW5JSE5sY25acFkyWGlnS1luSUQ0bU1nb2dJQ0FnSUNCcmFXeHNJQzB4TmlBa2NHbGtDaUFnSUNBZ0lHVmphRzhnSjFObGNuWnBZMlVnYzNSdmNIQmxaQ2NnUGlZeUNnbGxiSE5sQ2drZ0lHVmphRzhnSjFObGNuWnBZMlVnYm05MElISjFibTVwYm1jbkNnbG1hUW9KY20wZ0xXWWdKQ0pRU1VSR1NVeEZJZ29nSUdacENuMEtjbVZ6ZEdGeWRDZ3Bld29KYzNSdmNBb0pjM1JoY25RS2ZRcHpkR0YwZFhNb0tYc0tDV2xtSUZzZ0xXWWdJaVJRU1VSR1NVeEZJaUJkQ2dsMGFHVnVD", 16000);
	memcpy_s(_servicemanager + 32000, 21564, "Z2tKY0dsa1BTUW9JR05oZENBaUpGQkpSRVpKVEVVaUlDa0tDUWxwWmlCcmFXeHNJQzB3SUNSd2FXUWdNajR2WkdWMkwyNTFiR3c3SUhSb1pXNEtDUWtKWldOb2J5QWlXRmhZV0ZnZ2MzUmhjblF2Y25WdWJtbHVaeXdnY0hKdlkyVnpjeUFrY0dsa0lnb0pDV1ZzYzJVS0NRa0paV05vYnlBbldGaFlXRmdnYzNSdmNDOTNZV2wwYVc1bkp3b0pDV1pwQ2dsbGJITmxDZ2tKWldOb2J5QW5XRmhZV0ZnZ2MzUnZjQzkzWVdsMGFXNW5Kd29KWm1rS0NuMEtDZ3BqWVhObElDSWtNU0lnYVc0S0NYTjBZWEowS1FvSkNYTjBZWEowQ2drSk96c0tDWE4wYjNBcENna0pjM1J2Y0FvSkNUczdDZ2x5WlhOMFlYSjBLUW9KQ1hOMGIzQUtDUWx6ZEdGeWRBb0pDVHM3Q2dsemRHRjBkWE1wQ2drSmMzUmhkSFZ6Q2drSk96c0tDU29wQ2drSlpXTm9ieUFpVlhOaFoyVTZJSE5sY25acFkyVWdXRmhZV0ZnZ2UzTjBZWEowZkhOMGIzQjhjbVZ6ZEdGeWRIeHpkR0YwZFhOOUlnb0pDVHM3Q21WellXTUtaWGhwZENBd0Nnbz0nLCAnYmFzZTY0JykudG9TdHJpbmcoKS5zcGxpdCgnWFhYWFgnKS5qb2luKG9wdGlvbnMubmFtZSkucmVwbGFjZSgne3tQQVJNU319JywgcGFyYW1ldGVycykpOwogICAgICAgICAgICAgICAgICAgIGNvbmYuZW5kKCk7CgogICAgICAgICAgICAgICAgICAgIG0gPSByZXF1aXJlKCdmcycpLnN0YXRTeW5jKCcvZXRjL2luaXQuZC8nICsgb3B0aW9ucy5uYW1lKS5tb2RlOwogICAgICAgICAgICAgICAgICAgIG0gfD0gKHJlcXVpcmUoJ2ZzJykuQ0hNT0RfTU9ERVMuU19JWFVTUiB8IHJlcXVpcmUoJ2ZzJykuQ0hNT0RfTU9ERVMuU19JWEdSUCk7CiAgICAgICAgICAgICAgICAgICAgcmVxdWlyZSgnZnMnKS5jaG1vZFN5bmMoJy9ldGMvaW5pdC5kLycgKyBvcHRpb25zLm5hbWUsIG0pOwogICAgICAgICAgICAgICAgICAgIHN3aXRjaCAob3B0aW9ucy5zdGFydFR5cGUpCiAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICBjYXNlICdCT09UX1NUQVJUJzoKICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSAnU1lTVEVNX1NUQVJUJzoKICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSAnQVVUT19TVEFSVCc6CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB2YXIgY2hpbGQgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy9iaW4vc2gnLCBbJ3NoJ10pOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IH0pOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ3VwZGF0ZS1yYy5kICcgKyBvcHRpb25zLm5hbWUgKyAnIGRlZmF1bHRzXG5leGl0XG4nKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNoaWxkLndhaXRFeGl0KCk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgICAgICAgICAgZGVmYXVsdDoKICAgICAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgIGNhc2UgJ3Vwc3RhcnQnOgogICAgICAgICAgICAgICAgICAgIGlmICghcmVxdWlyZSgnZnMnKS5leGlzdHNTeW5jKCcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJykpIHsgcmVxdWlyZSgnZnMnKS5ta2RpclN5bmMoJy91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcycpOyB9CiAgICAgICAgICAgICAgICAgICAgaWYgKCFyZXF1aXJlKCdmcycpLmV4aXN0c1N5bmMoJy91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcy8nICsgb3B0aW9ucy5uYW1lKSkgeyByZXF1aXJlKCdmcycpLm1rZGlyU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBvcHRpb25zLm5hbWUpOyB9CgogICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykuY29weUZpbGVTeW5jKG9wdGlvbnMuc2VydmljZVBhdGgsICcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSArICcvJyArIG9wdGlvbnMubmFtZSk7CiAgICAgICAgICAgICAgICAgICAgY29uc29sZS5sb2coJ2NvcHlpbmcgJyArIG9wdGlvbnMuc2VydmljZVBhdGgpOwoKICAgICAgICAgICAgICAgICAgICB2YXIgbSA9IHJlcXVpcmUoJ2ZzJykuc3RhdFN5bmMoJy91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcy8nICsgb3B0aW9ucy5uYW1lICsgJy8nICsgb3B0aW9ucy5uYW1lKS5tb2RlOwogICAgICAgICAgICAgICAgICAgIG0gfD0gKHJlcXVpcmUoJ2ZzJykuQ0hNT0RfTU9ERVMuU19JWFVTUiB8IHJlcXVpcmUoJ2ZzJykuQ0hNT0RfTU9ERVMuU19JWEdSUCk7CiAgICAgICAgICAgICAgICAgICAgcmVxdWlyZSgnZnMnKS5jaG1vZFN5bmMoJy91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcy8nICsgb3B0aW9ucy5uYW1lICsgJy8nICsgb3B0aW9ucy5uYW1lLCBtKTsKCiAgICAgICAgICAgICAgICAgICAgY29uZiA9IHJlcXVpcmUoJ2ZzJykuY3JlYXRlV3JpdGVTdHJlYW0oJy9ldGMvaW5pdC8nICsgb3B0aW9ucy5uYW1lICsgJy5jb25mJywgeyBmbGFnczogJ3diJyB9KTsKICAgICAgICAgICAgICAgICAgICBzd2l0Y2ggKG9wdGlvbnMuc3RhcnRUeXBlKQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSAnQk9PVF9TVEFSVCc6CiAgICAgICAgICAgICAgICAgICAgICAgIGNhc2UgJ1NZU1RFTV9TVEFSVCc6CiAgICAgICAgICAgICAgICAgICAgICAgIGNhc2UgJ0FVVE9fU1RBUlQnOgogICAgICAgICAgICAgICAgICAgICAgICAgICAgY29uZi53cml0ZSgnc3RhcnQgb24gcnVubGV2ZWwgWzIzNDVdXG4nKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgICAgICBkZWZhdWx0OgogICAgICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgIGNvbmYud3JpdGUoJ3N0b3Agb24gcnVubGV2ZWwgWzAxNl1cblxuJyk7CiAgICAgICAgICAgICAgICAgICAgY29uZi53cml0ZSgncmVzcGF3blxuXG4nKTsKICAgICAgICAgICAgICAgICAgICBjb25mLndyaXRlKCdjaGRpciAvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSArICdcbicpOwogICAgICAgICAgICAgICAgICAgIGNvbmYud3JpdGUoJ2V4ZWMgL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBvcHRpb25zLm5hbWUgKyAnLycgKyBvcHRpb25zLm5hbWUgKyAnICcgKyBwYXJhbWV0ZXJzICsgJ1xuXG4nKTsKICAgICAgICAgICAgICAgICAgICBjb25mLmVuZCgpOwogICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgY2FzZSAnc3lzdGVtZCc6CiAgICAgICAgICAgICAgICAgICAgdmFyIHNlcnZpY2VEZXNjcmlwdGlvbiA9IG9wdGlvbnMuZGVzY3JpcHRpb24gPyBvcHRpb25zLmRlc2NyaXB0aW9uIDogJ01lc2hDZW50cmFsIEFnZW50JzsKCiAgICAgICAgICAgICAgICAgICAgaWYgKCFyZXF1aXJlKCdmcycpLmV4aXN0c1N5bmMoJy91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcy8nKSkgeyByZXF1aXJlKCdmcycpLm1rZGlyU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzJyk7IH0KICAgICAgICAgICAgICAgICAgICBpZiAoIXJlcXVpcmUoJ2ZzJykuZXhpc3RzU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBvcHRpb25zLm5hbWUpKSB7IHJlcXVpcmUoJ2ZzJykubWtkaXJTeW5jKCcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSk7IH0KCiAgICAgICAgICAgICAgICAgICAgY29uc29sZS5sb2coJ2NvcHlpbmcgJyArIG9wdGlvbnMuc2VydmljZVBhdGgpOwogICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykuY29weUZpbGVTeW5jKG9wdGlvbnMuc2VydmljZVBhdGgsICcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSArICcvJyArIG9wdGlvbnMubmFtZSk7CgogICAgICAgICAgICAgICAgICAgIHZhciBtID0gcmVxdWlyZSgnZnMnKS5zdGF0U3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBvcHRpb25zLm5hbWUgKyAnLycgKyBvcHRpb25zLm5hbWUpLm1vZGU7CiAgICAgICAgICAgICAgICAgICAgbSB8PSAocmVxdWlyZSgnZnMnKS5DSE1PRF9NT0RFUy5TX0lYVVNSIHwgcmVxdWlyZSgnZnMnKS5DSE1PRF9NT0RFUy5TX0lYR1JQKTsKICAgICAgICAgICAgICAgICAgICByZXF1aXJlKCdmcycpLmNobW9kU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBvcHRpb25zLm5hbWUgKyAnLycgKyBvcHRpb25zLm5hbWUsIG0pOwoKICAgICAgICAgICAgICAgICAgICBpZiAocmVxdWlyZSgnZnMnKS5leGlzdHNTeW5jKCcvbGliL3N5c3RlbWQvc3lzdGVtJykpCiAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICBjb25mID0gcmVxdWlyZSgnZnMnKS5jcmVhdGVXcml0ZVN0cmVhbSgnL2xpYi9zeXN0ZW1kL3N5c3RlbS8nICsgb3B0aW9ucy5uYW1lICsgJy5zZXJ2aWNlJywgeyBmbGFnczogJ3diJyB9KTsKICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgZWxzZSBpZiAocmVxdWlyZSgnZnMnKS5leGlzdHNTeW5jKCcvdXNyL2xpYi9zeXN0ZW1kL3N5c3RlbScpKQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgY29uZiA9IHJlcXVpcmUoJ2ZzJykuY3JlYXRlV3JpdGVTdHJlYW0oJy91c3IvbGliL3N5c3RlbWQvc3lzdGVtLycgKyBvcHRpb25zLm5hbWUgKyAnLnNlcnZpY2UnLCB7IGZsYWdzOiAnd2InIH0pOwogICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICBlbHNlCiAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICB0aHJvdyAoJ3Vua25vd24gbG9jYXRpb24gZm9yIHN5c3RlbWQgY29uZmlndXJhdGlvbiBmaWxlcycpOwogICAgICAgICAgICAgICAgICAgIH0KCiAgICAgICAgICAgICAgICAgICAgY29uZi53cml0ZSgnW1VuaXRdXG5EZXNjcmlwdGlvbj0nICsgc2VydmljZURlc2NyaXB0aW9uICsgJ1xuJyk7CiAgICAgICAgICAgICAgICAgICAgY29uZi53cml0ZSgnW1NlcnZpY2VdXG4nKTsKICAgICAgICAgICAgICAgICAgICBjb25mLndyaXRlKCdXb3JraW5nRGlyZWN0b3J5PS91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcy8nICsgb3B0aW9ucy5uYW1lICsgJ1xuJyk7CiAgICAgICAgICAgICAgICAgICAgY29uZi53cml0ZSgnRXhlY1N0YXJ0PS91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcy8nICsgb3B0aW9ucy5uYW1lICsgJy8nICsgb3B0aW9ucy5uYW1lICsgJyAnICsgcGFyYW1ldGVycyArICdcbicpOwogICAgICAgICAgICAgICAgICAgIGNvbmYud3JpdGUoJ1N0YW5kYXJkT3V0cHV0PW51bGxcbicpOwogICAgICAgICAgICAgICAgICAgIGNvbmYud3JpdGUoJ1Jlc3RhcnQ9b24tZmFpbHVyZVxuJyk7CiAgICAgICAgICAgICAgICAgICAgY29uZi53cml0ZSgnUmVzdGFydFNlYz0zXG4nKTsKICAgICAgICAgICAgICAgICAgICBzd2l0Y2ggKG9wdGlvbnMuc3RhcnRUeXBlKQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSAnQk9PVF9TVEFSVCc6CiAgICAgICAgICAgICAgICAgICAgICAgIGNhc2UgJ1NZU1RFTV9TVEFSVCc6CiAgICAgICAgICAgICAgICAgICAgICAgIGNhc2UgJ0FVVE9fU1RBUlQnOgogICAgICAgICAgICAgICAgICAgICAgICAgICAgY29uZi53cml0ZSgnW0luc3RhbGxdXG4nKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNvbmYud3JpdGUoJ1dhbnRlZEJ5PW11bHRpLXVzZXIudGFyZ2V0XG4nKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNvbmYud3JpdGUoJ0FsaWFzPScgKyBvcHRpb25zLm5hbWUgKyAnLnNlcnZpY2VcbicpOyBicmVhazsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHRoaXMuX3VwZGF0ZSA9IHJlcXVpcmUoJ2NoaWxkX3Byb2Nlc3MnKS5leGVjRmlsZSgnL2Jpbi9zaCcsIFsnc2gnXSwgeyB0eXBlOiByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuU3Bhd25UeXBlcy5URVJNIH0pOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgdGhpcy5fdXBkYXRlLl9tb2R1bGVOYW1lID0gb3B0aW9ucy5uYW1lOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgdGhpcy5fdXBkYXRlLnN0ZG91dC5vbignZGF0YScsIGZ1bmN0aW9uIChjaHVuaykgeyB9KTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHRoaXMuX3VwZGF0ZS5zdGRpbi53cml0ZSgnc3lzdGVtY3RsIGVuYWJsZSAnICsgb3B0aW9ucy5uYW1lICsgJy5zZXJ2aWNlXG4nKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHRoaXMuX3VwZGF0ZS5zdGRpbi53cml0ZSgnZXhpdFxuJyk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB0aGlzLl91cGRhdGUud2FpdEV4aXQoKTsKICAgICAgICAgICAgICAgICAgICAgICAgZGVmYXVsdDoKICAgICAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICBjb25mLmVuZCgpOwoKICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgIGRlZmF1bHQ6IC8vIHVua25vd24gcGxhdGZvcm0gc2VydmljZSB0eXBlCiAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgIH0KICAgICAgICB9CiAgICAgICAgaWYocHJvY2Vzcy5wbGF0Zm9ybSA9PSAnZGFyd2luJykKICAgICAgICB7CiAgICAgICAgICAgIGlmICghdGhpcy5pc0FkbWluKCkpIHsgdGhyb3cgKCdJbnN0YWxsaW5nIGFzIFNlcnZpY2UsIHJlcXVpcmVzIHJvb3QnKTsgfQoKICAgICAgICAgICAgLy8gTWFjIE9TCiAgICAgICAgICAgIHZhciBzdGRvdXRwYXRoID0gKG9wdGlvbnMuc3Rkb3V0ID8gKCc8a2V5PlN0YW5kYXJkT3V0UGF0aDwva2V5PlxuPHN0cmluZz4nICsgb3B0aW9ucy5zdGRvdXQgKyAnPC9zdHJpbmc+JykgOiAnJyk7CiAgICAgICAgICAgIHZhciBhdXRvU3RhcnQgPSAob3B0aW9ucy5zdGFydFR5cGUgPT0gJ0FVVE9fU1RBUlQnID8gJzx0cnVlLz4nIDogJzxmYWxzZS8+Jyk7CiAgICAgICAgICAgIHZhciBwYXJhbXMgPSAgJyAgICAgPGtleT5Qcm9ncmFtQXJndW1lbnRzPC9rZXk+XG4nOwogICAgICAgICAgICBwYXJhbXMgKz0gJyAgICAgPGFycmF5PlxuJzsKICAgICAgICAgICAgcGFyYW1zICs9ICgnICAgICAgICAgPHN0cmluZz4vdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSArICcvJyArIG9wdGlvbnMubmFtZSArICc8L3N0cmluZz5cbicpOwogICAgICAgICAgICBpZihvcHRpb25zLnBhcmFtZXRlcnMpCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIGZvcih2YXIgaXRtIGluIG9wdGlvbnMucGFyYW1ldGVycykKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICBwYXJhbXMgKz0gKCcgICAgICAgICA8c3RyaW5nPicgKyBvcHRpb25zLnBhcmFtZXRlcnNbaXRtXSArICc8L3N0cmluZz5cbicpOwogICAgICAgICAgICAgICAgfQogICAgICAgICAgICB9ICAgICAgICAKICAgICAgICAgICAgcGFyYW1zICs9ICcgICAgIDwvYXJyYXk+XG4nOwogICAgICAgICAgICAKICAgICAgICAgICAgdmFyIHBsaXN0ID0gJzw/eG1sIHZlcnNpb249IjEuMCIgZW5jb2Rpbmc9IlVURi04Ij8+XG4nOwogICAgICAgICAgICBwbGlzdCArPSAnPCFET0NUWVBFIHBsaXN0IFBVQkxJQyAiLS8vQXBwbGUgQ29tcHV0ZXIvL0RURCBQTElTVCAxLjAvL0VOIiAiaHR0cDovL3d3dy5hcHBsZS5jb20vRFREcy9Qcm9wZXJ0eUxpc3QtMS4wLmR0ZCI+XG4nOwogICAgICAgICAgICBwbGlzdCArPSAnPHBsaXN0IHZlcnNpb249IjEuMCI+XG4nOwogICAgICAgICAgICBwbGlzdCArPSAnICA8ZGljdD5cbic7CiAgICAgICAgICAgIHBsaXN0ICs9ICcgICAgICA8a2V5PkxhYmVsPC9rZXk+XG4nOwogICAgICAgICAgICBwbGlzdCArPSAoJyAgICAgPHN0cmluZz4nICsgb3B0aW9ucy5uYW1lICsgJzwvc3RyaW5nPlxuJyk7CiAgICAgICAgICAgIHBsaXN0ICs9IChwYXJhbXMgKyAnXG4nKTsKICAgICAgICAgICAgcGxpc3QgKz0gJyAgICAgIDxrZXk+V29ya2luZ0RpcmVjdG9yeTwva2V5PlxuJzsKICAgICAgICAgICAgcGxpc3QgKz0gKCcgICAgIDxzdHJpbmc+L3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBvcHRpb25zLm5hbWUgKyAnPC9zdHJpbmc+XG4nKTsKICAgICAgICAgICAgcGxpc3QgKz0gKHN0ZG91dHBhdGggKyAnXG4nKTsKICAgICAgICAgICAgcGxpc3QgKz0gJyAgICAgIDxrZXk+UnVuQXRMb2FkPC9rZXk+XG4nOwogICAgICAgICAgICBwbGlzdCArPSAoYXV0b1N0YXJ0ICsgJ1xuJyk7CiAgICAgICAgICAgIHBsaXN0ICs9ICcgIDwvZGljdD5cbic7CiAgICAgICAgICAgIHBsaXN0ICs9ICc8L3BsaXN0Pic7CgogICAgICAgICAgICBpZiAoIXJlcXVpcmUoJ2ZzJykuZXhpc3RzU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzJykpIHsgcmVxdWlyZSgnZnMnKS5ta2RpclN5bmMoJy91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcycpOyB9CiAgICAgICAgICAgIGlmICghcmVxdWlyZSgnZnMnKS5leGlzdHNTeW5jKCcvTGlicmFyeS9MYXVuY2hEYWVtb25zLycgKyBvcHRpb25zLm5hbWUgKyAnLnBsaXN0JykpCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIGlmICghcmVxdWlyZSgnZnMnKS5leGlzdHNTeW5jKCcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSkpIHsgcmVxdWlyZSgnZnMnKS5ta2RpclN5bmMoJy91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcy8nICsgb3B0aW9ucy5uYW1lKTsgfQogICAgICAgICAgICAgICAgaWYgKG9wdGlvbnMuYmluYXJ5KQogICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykud3JpdGVGaWxlU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBvcHRpb25zLm5hbWUgKyAnLycgKyBvcHRpb25zLm5hbWUsIG9wdGlvbnMuYmluYXJ5KTsKICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIGVsc2UKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICByZXF1aXJlKCdmcycpLmNvcHlGaWxlU3luYyhvcHRpb25zLnNlcnZpY2VQYXRoLCAnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBvcHRpb25zLm5hbWUgKyAnLycgKyBvcHRpb25zLm5hbWUpOwogICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgcmVxdWlyZSgnZnMnKS53cml0ZUZpbGVTeW5jKCcvTGlicmFyeS9MYXVuY2hEYWVtb25zLycgKyBvcHRpb25zLm5hbWUgKyAnLnBsaXN0JywgcGxpc3QpOwogICAgICAgICAgICAgICAgdmFyIG0gPSByZXF1aXJlKCdmcycpLnN0YXRTeW5jKCcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSArICcvJyArIG9wdGlvbnMubmFtZSkubW9kZTsKICAgICAgICAgICAgICAgIG0gfD0gKHJlcXVpcmUoJ2ZzJykuQ0hNT0RfTU9ERVMuU19JWFVTUiB8IHJlcXVpcmUoJ2ZzJykuQ0hNT0RfTU9ERVMuU19JWEdSUCk7CiAgICAgICAgICAgICAgICByZXF1aXJlKCdmcycpLmNobW9kU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBvcHRpb25zLm5hbWUgKyAnLycgKyBvcHRpb25zLm5hbWUsIG0pOwogICAgICAgICAgICB9CiAgICAgICAgICAgIGVsc2UKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgdGhyb3cgKCdTZXJ2aWNlOiAnICsgb3B0aW9ucy5uYW1lICsgJyBhbHJlYWR5IGV4aXN0cycpOwogICAgICAgICAgICB9CiAgICAgICAgfQoKICAgICAgICBpZiAob3B0aW9ucy5maWxlcykgewogICAgICAgICAgICBmb3IgKHZhciBpIGluIG9wdGlvbnMuZmlsZXMpIHsKICAgICAgICAgICAgICAgIGNvbnNvbGUubG9nKCdjb3B5aW5nICcgKyBvcHRpb25zLmZpbGVzW2ldKTsKICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykuY29weUZpbGVTeW5jKG9wdGlvbnMuZmlsZXNbaV0sICcvdXNyL2xvY2FsL21lc2hfc2VydmljZXMvJyArIG9wdGlvbnMubmFtZSArICcvJyArIGV4dHJhY3RGaWxlTmFtZShvcHRpb25zLmZpbGVzW2ldKSk7CiAgICAgICAgICAgIH0KICAgICAgICB9CiAgICB9CiAgICB0aGlzLnVuaW5zdGFsbFNlcnZpY2UgPSBmdW5jdGlvbiB1bmluc3RhbGxTZXJ2aWNlKG5hbWUpCiAgICB7CiAgICAgICAgaWYgKCF0aGlzLmlzQWRtaW4oKSkgeyB0aHJvdyAoJ1VuaW5zdGFsbGluZyBhIHNlcnZpY2UsIHJlcXVpcmVzIGFkbWluJyk7IH0KCiAgICAgICAgaWYgKHR5cGVvZiAobmFtZSkgPT0gJ29iamVjdCcpIHsgbmFtZSA9IG5hbWUubmFtZTsgfQogICAgICAgIGlmIChwcm9jZXNzLnBsYXRmb3JtID09ICd3aW4zMicpCiAgICAgICAgewogICAgICAgICAgICB2YXIgc2VydmljZSA9IHRoaXMuZ2V0U2VydmljZShuYW1lKTsKICAgICAgICAgICAgaWYgKHNlcnZpY2Uuc3RhdHVzLnN0YXRlID09IHVuZGVmaW5lZCB8fCBzZXJ2aWNlLnN0YXR1cy5zdGF0ZSA9PSAnU1RPUFBFRCcpCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIGlmICh0aGlzLnByb3h5LkRlbGV0ZVNlcnZpY2Uoc2VydmljZS5fc2VydmljZSkgPT0gMCkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICB0aHJvdyAoJ1VuaW5zdGFsbCBTZXJ2aWNlIGZvcjogJyArIG5hbWUgKyAnLCBmYWlsZWQgd2l0aCBlcnJvcjogJyArIHRoaXMucHJveHkyLkdldExhc3RFcnJvcigpKTsKICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIGVsc2UKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICB0cnkKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykudW5saW5rU3luYyh0aGlzLmdldFNlcnZpY2VGb2xkZXIoKSArICdcXCcgKyBuYW1lICsgJy5leGUnKTsKICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgY2F0Y2goZSkKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgfQogICAgICAgICAgICB9CiAgICAgICAgICAgIGVsc2UKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgdGhyb3cgKCdDYW5ub3QgdW5pbnN0YWxsIHNlcnZpY2U6ICcgKyBuYW1lICsgJywgYmVjYXVzZSBpdCBpczogJyArIHNlcnZpY2Uuc3RhdHVzLnN0YXRlKTsKICAgICAgICAgICAgfQogICAgICAgIH0KICAgICAgICBlbHNlIGlmKHByb2Nlc3MucGxhdGZvcm0gPT0gJ2xpbnV4JykKICAgICAgICB7CiAgICAgICAgICAgIHN3aXRjaCAodGhpcy5nZXRTZXJ2aWNlVHlwZSgpKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICBjYXNlICdpbml0JzoKICAgICAgICAgICAgICAgICAgICB0aGlzLl91cGRhdGUgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy9iaW4vc2gnLCBbJ3NoJ10pOwogICAgICAgICAgICAgICAgICAgIHRoaXMuX3VwZGF0ZS5zdGRvdXQub24oJ2RhdGEnLCBmdW5jdGlvbiAoY2h1bmspIHsgfSk7CiAgICAgICAgICAgICAgICAgICAgdGhpcy5fdXBkYXRlLnN0ZGluLndyaXRlKCdzZXJ2aWNlICcgKyBuYW1lICsgJyBzdG9wXG4nKTsKICAgICAgICAgICAgICAgICAgICB0aGlzLl91cGRhdGUuc3RkaW4ud3JpdGUoJ3VwZGF0ZS1yYy5kIC1mICcgKyBuYW1lICsgJyByZW1vdmVcbicpOwogICAgICAgICAgICAgICAgICAgIHRoaXMuX3VwZGF0ZS5zdGRpbi53cml0ZSgnZXhpdFxuJyk7CiAgICAgICAgICAgICAgICAgICAgdGhpcy5fdXBkYXRlLndhaXRFeGl0KCk7CiAgICAgICAgICAgICAgICAgICAgdHJ5IHsKICAgICAgICAgICAgICAgICAgICAgICAgcmVxdWlyZSgnZnMnKS51bmxpbmtTeW5jKCcvZXRjL2luaXQuZC8nICsgbmFtZSk7CiAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykudW5saW5rU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBuYW1lICsgJy8nICsgbmFtZSk7CiAgICAgICAgICAgICAgICAgICAgICAgIGNvbnNvbGUubG9n", 16000);
	memcpy_s(_servicemanager + 48000, 5564, "KG5hbWUgKyAnIHVuaW5zdGFsbGVkJyk7CiAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgIGNhdGNoIChlKSB7CiAgICAgICAgICAgICAgICAgICAgICAgIGNvbnNvbGUubG9nKG5hbWUgKyAnIGNvdWxkIG5vdCBiZSB1bmluc3RhbGxlZCcsIGUpCiAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgY2FzZSAndXBzdGFydCc6CiAgICAgICAgICAgICAgICAgICAgdGhpcy5fdXBkYXRlID0gcmVxdWlyZSgnY2hpbGRfcHJvY2VzcycpLmV4ZWNGaWxlKCcvYmluL3NoJywgWydzaCddKTsKICAgICAgICAgICAgICAgICAgICB0aGlzLl91cGRhdGUuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IH0pOwogICAgICAgICAgICAgICAgICAgIHRoaXMuX3VwZGF0ZS5zdGRpbi53cml0ZSgnc2VydmljZSAnICsgbmFtZSArICcgc3RvcFxuJyk7CiAgICAgICAgICAgICAgICAgICAgdGhpcy5fdXBkYXRlLnN0ZGluLndyaXRlKCdleGl0XG4nKTsKICAgICAgICAgICAgICAgICAgICB0aGlzLl91cGRhdGUud2FpdEV4aXQoKTsKICAgICAgICAgICAgICAgICAgICB0cnkKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykudW5saW5rU3luYygnL2V0Yy9pbml0LycgKyBuYW1lICsgJy5jb25mJyk7CiAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykudW5saW5rU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBuYW1lICsgJy8nICsgbmFtZSk7CiAgICAgICAgICAgICAgICAgICAgICAgIGNvbnNvbGUubG9nKG5hbWUgKyAnIHVuaW5zdGFsbGVkJyk7CiAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgIGNhdGNoIChlKQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgY29uc29sZS5sb2cobmFtZSArICcgY291bGQgbm90IGJlIHVuaW5zdGFsbGVkJywgZSkKICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICBjYXNlICdzeXN0ZW1kJzoKICAgICAgICAgICAgICAgICAgICB0aGlzLl91cGRhdGUgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy9iaW4vc2gnLCBbJ3NoJ10sIHsgdHlwZTogcmVxdWlyZSgnY2hpbGRfcHJvY2VzcycpLlNwYXduVHlwZXMuVEVSTSB9KTsKICAgICAgICAgICAgICAgICAgICB0aGlzLl91cGRhdGUuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IH0pOwogICAgICAgICAgICAgICAgICAgIHRoaXMuX3VwZGF0ZS5zdGRpbi53cml0ZSgnc3lzdGVtY3RsIHN0b3AgJyArIG5hbWUgKyAnLnNlcnZpY2VcbicpOwogICAgICAgICAgICAgICAgICAgIHRoaXMuX3VwZGF0ZS5zdGRpbi53cml0ZSgnc3lzdGVtY3RsIGRpc2FibGUgJyArIG5hbWUgKyAnLnNlcnZpY2VcbicpOwogICAgICAgICAgICAgICAgICAgIHRoaXMuX3VwZGF0ZS5zdGRpbi53cml0ZSgnZXhpdFxuJyk7CiAgICAgICAgICAgICAgICAgICAgdGhpcy5fdXBkYXRlLndhaXRFeGl0KCk7CiAgICAgICAgICAgICAgICAgICAgdHJ5CiAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICByZXF1aXJlKCdmcycpLnVubGlua1N5bmMoJy91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcy8nICsgbmFtZSArICcvJyArIG5hbWUpOwogICAgICAgICAgICAgICAgICAgICAgICBpZiAocmVxdWlyZSgnZnMnKS5leGlzdHNTeW5jKCcvbGliL3N5c3RlbWQvc3lzdGVtLycgKyBuYW1lICsgJy5zZXJ2aWNlJykpIHsgcmVxdWlyZSgnZnMnKS51bmxpbmtTeW5jKCcvbGliL3N5c3RlbWQvc3lzdGVtLycgKyBuYW1lICsgJy5zZXJ2aWNlJyk7IH0KICAgICAgICAgICAgICAgICAgICAgICAgaWYgKHJlcXVpcmUoJ2ZzJykuZXhpc3RzU3luYygnL3Vzci9saWIvc3lzdGVtZC9zeXN0ZW0vJyArIG5hbWUgKyAnLnNlcnZpY2UnKSkgeyByZXF1aXJlKCdmcycpLnVubGlua1N5bmMoJy91c3IvbGliL3N5c3RlbWQvc3lzdGVtLycgKyBuYW1lICsgJy5zZXJ2aWNlJyk7IH0KICAgICAgICAgICAgICAgICAgICAgICAgY29uc29sZS5sb2cobmFtZSArICcgdW5pbnN0YWxsZWQnKTsKICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgY2F0Y2ggKGUpCiAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICBjb25zb2xlLmxvZyhuYW1lICsgJyBjb3VsZCBub3QgYmUgdW5pbnN0YWxsZWQnLCBlKQogICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgIGRlZmF1bHQ6IC8vIHVua25vd24gcGxhdGZvcm0gc2VydmljZSB0eXBlCiAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgIH0KICAgICAgICB9CiAgICAgICAgZWxzZSBpZihwcm9jZXNzLnBsYXRmb3JtID09ICdkYXJ3aW4nKQogICAgICAgIHsKICAgICAgICAgICAgaWYgKHJlcXVpcmUoJ2ZzJykuZXhpc3RzU3luYygnL0xpYnJhcnkvTGF1bmNoRGFlbW9ucy8nICsgbmFtZSArICcucGxpc3QnKSkKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgdmFyIGNoaWxkID0gcmVxdWlyZSgnY2hpbGRfcHJvY2VzcycpLmV4ZWNGaWxlKCcvYmluL3NoJywgWydzaCddKTsKICAgICAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5vbignZGF0YScsIGZ1bmN0aW9uIChjaHVuaykgeyB9KTsKICAgICAgICAgICAgICAgIGNoaWxkLnN0ZGluLndyaXRlKCdsYXVuY2hjdGwgc3RvcCAnICsgbmFtZSArICdcbicpOwogICAgICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ2xhdW5jaGN0bCB1bmxvYWQgL0xpYnJhcnkvTGF1bmNoRGFlbW9ucy8nICsgbmFtZSArICcucGxpc3RcbicpOwogICAgICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ2V4aXRcbicpOwogICAgICAgICAgICAgICAgY2hpbGQud2FpdEV4aXQoKTsKCiAgICAgICAgICAgICAgICB0cnkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICByZXF1aXJlKCdmcycpLnVubGlua1N5bmMoJy91c3IvbG9jYWwvbWVzaF9zZXJ2aWNlcy8nICsgbmFtZSArICcvJyArIG5hbWUpOwogICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ2ZzJykudW5saW5rU3luYygnL0xpYnJhcnkvTGF1bmNoRGFlbW9ucy8nICsgbmFtZSArICcucGxpc3QnKTsKICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIGNhdGNoKGUpCiAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgdGhyb3cgKCdFcnJvciB1bmluc3RhbGxpbmcgc2VydmljZTogJyArIG5hbWUgKyAnID0+ICcgKyBlKTsKICAgICAgICAgICAgICAgIH0KCiAgICAgICAgICAgICAgICB0cnkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICByZXF1aXJlKCdmcycpLnJtZGlyU3luYygnL3Vzci9sb2NhbC9tZXNoX3NlcnZpY2VzLycgKyBuYW1lKTsKICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIGNhdGNoKGUpCiAgICAgICAgICAgICAgICB7fQogICAgICAgICAgICB9CiAgICAgICAgICAgIGVsc2UKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgdGhyb3cgKCdTZXJ2aWNlOiAnICsgbmFtZSArICcgZG9lcyBub3QgZXhpc3QnKTsKICAgICAgICAgICAgfQogICAgICAgIH0KICAgIH0KICAgIGlmKHByb2Nlc3MucGxhdGZvcm0gPT0gJ2xpbnV4JykKICAgIHsKICAgICAgICB0aGlzLmdldFNlcnZpY2VUeXBlID0gZnVuY3Rpb24gZ2V0U2VydmljZVR5cGUoKQogICAgICAgIHsKICAgICAgICAgICAgdmFyIHBsYXRmb3JtID0gcmVxdWlyZSgncHJvY2Vzcy1tYW5hZ2VyJykuZ2V0UHJvY2Vzc0luZm8oMSkuTmFtZTsKICAgICAgICAgICAgaWYgKHBsYXRmb3JtID09ICdpbml0JykKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgaWYocmVxdWlyZSgnZnMnKS5leGlzdHNTeW5jKCcvZXRjL2luaXQnKSkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICBwbGF0Zm9ybSA9ICd1cHN0YXJ0JzsKICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgfQogICAgICAgICAgICByZXR1cm4gKHBsYXRmb3JtKTsKICAgICAgICB9OwogICAgfQp9Cgptb2R1bGUuZXhwb3J0cyA9IHNlcnZpY2VNYW5hZ2VyO/==", 5564);
	ILibBase64DecodeEx(_servicemanager, 53564, _servicemanager + 53564);
	duk_push_global_object(ctx); duk_get_prop_string(ctx, -1, "addModule"); duk_swap_top(ctx, -2); duk_push_string(ctx, "service-manager"); duk_push_string(ctx, _servicemanager + 53564);
	duk_pcall_method(ctx, 2); duk_pop(ctx);
	free(_servicemanager);

	char *_usersessions = ILibMemory_Allocate(71892, 0, NULL, NULL);
	memcpy_s(_usersessions + 0, 41080, "LyoKQ29weXJpZ2h0IDIwMTggSW50ZWwgQ29ycG9yYXRpb24KCkxpY2Vuc2VkIHVuZGVyIHRoZSBBcGFjaGUgTGljZW5zZSwgVmVyc2lvbiAyLjAgKHRoZSAiTGljZW5zZSIpOwp5b3UgbWF5IG5vdCB1c2UgdGhpcyBmaWxlIGV4Y2VwdCBpbiBjb21wbGlhbmNlIHdpdGggdGhlIExpY2Vuc2UuCllvdSBtYXkgb2J0YWluIGEgY29weSBvZiB0aGUgTGljZW5zZSBhdAoKICAgIGh0dHA6Ly93d3cuYXBhY2hlLm9yZy9saWNlbnNlcy9MSUNFTlNFLTIuMAoKVW5sZXNzIHJlcXVpcmVkIGJ5IGFwcGxpY2FibGUgbGF3IG9yIGFncmVlZCB0byBpbiB3cml0aW5nLCBzb2Z0d2FyZQpkaXN0cmlidXRlZCB1bmRlciB0aGUgTGljZW5zZSBpcyBkaXN0cmlidXRlZCBvbiBhbiAiQVMgSVMiIEJBU0lTLApXSVRIT1VUIFdBUlJBTlRJRVMgT1IgQ09ORElUSU9OUyBPRiBBTlkgS0lORCwgZWl0aGVyIGV4cHJlc3Mgb3IgaW1wbGllZC4KU2VlIHRoZSBMaWNlbnNlIGZvciB0aGUgc3BlY2lmaWMgbGFuZ3VhZ2UgZ292ZXJuaW5nIHBlcm1pc3Npb25zIGFuZApsaW1pdGF0aW9ucyB1bmRlciB0aGUgTGljZW5zZS4KKi8KCnZhciBOT1RJRllfRk9SX1RISVNfU0VTU0lPTiA9IDA7CnZhciBOT1RJRllfRk9SX0FMTF9TRVNTSU9OUyA9IDE7CnZhciBXTV9XVFNTRVNTSU9OX0NIQU5HRSA9IDB4MDJCMTsKdmFyIFdNX1BPV0VSQlJPQURDQVNUID0gMHgyMTg7CnZhciBQQlRfUE9XRVJTRVRUSU5HQ0hBTkdFID0gMHg4MDEzOwp2YXIgUEJUX0FQTVNVU1BFTkQgPSAweDQ7CnZhciBQQlRfQVBNUkVTVU1FU1VTUEVORCA9IDB4NzsKdmFyIFBCVF9BUE1SRVNVTUVBVVRPTUFUSUMgPSAweDEyOwp2YXIgUEJUX0FQTVBPV0VSU1RBVFVTQ0hBTkdFID0gMHhBOwoKdmFyIFdUU19DT05TT0xFX0NPTk5FQ1QgICAgICAgICA9ICgweDEpOwp2YXIgV1RTX0NPTlNPTEVfRElTQ09OTkVDVCAgICAgID0gKDB4Mik7CnZhciBXVFNfUkVNT1RFX0NPTk5FQ1QgICAgICAgICAgPSAoMHgzKTsKdmFyIFdUU19SRU1PVEVfRElTQ09OTkVDVCAgICAgICA9ICgweDQpOwp2YXIgV1RTX1NFU1NJT05fTE9HT04gICAgICAgICAgID0gKDB4NSk7CnZhciBXVFNfU0VTU0lPTl9MT0dPRkYgICAgICAgICAgPSAoMHg2KTsKdmFyIFdUU19TRVNTSU9OX0xPQ0sgICAgICAgICAgICA9ICgweDcpOwp2YXIgV1RTX1NFU1NJT05fVU5MT0NLICAgICAgICAgID0gKDB4OCk7CnZhciBXVFNfU0VTU0lPTl9SRU1PVEVfQ09OVFJPTCAgPSAoMHg5KTsKdmFyIFdUU19TRVNTSU9OX0NSRUFURSAgICAgICAgICA9ICgweEEpOwp2YXIgV1RTX1NFU1NJT05fVEVSTUlOQVRFICAgICAgID0gKDB4Qik7Cgp2YXIgR1VJRF9BQ0RDX1BPV0VSX1NPVVJDRTsKdmFyIEdVSURfQkFUVEVSWV9QRVJDRU5UQUdFX1JFTUFJTklORzsKdmFyIEdVSURfQ09OU09MRV9ESVNQTEFZX1NUQVRFOwoKZnVuY3Rpb24gVXNlclNlc3Npb25zKCkKewogICAgdGhpcy5fT2JqZWN0SUQgPSAndXNlci1zZXNzaW9ucyc7CiAgICByZXF1aXJlKCdldmVudHMnKS5FdmVudEVtaXR0ZXIuY2FsbCh0aGlzLCB0cnVlKQogICAgICAgIC5jcmVhdGVFdmVudCgnY2hhbmdlZCcpCiAgICAgICAgLmNyZWF0ZUV2ZW50KCdsb2NrZWQnKQogICAgICAgIC5jcmVhdGVFdmVudCgndW5sb2NrZWQnKTsKCiAgICB0aGlzLmVudW1lcmF0ZVVzZXJzID0gZnVuY3Rpb24gZW51bWVyYXRlVXNlcnMoKQogICAgewogICAgICAgIHZhciBwcm9taXNlID0gcmVxdWlyZSgncHJvbWlzZScpOwogICAgICAgIHZhciBwID0gbmV3IHByb21pc2UoZnVuY3Rpb24gKHJlcywgcmVqKQogICAgICAgIHsKICAgICAgICAgICAgdGhpcy5fX3Jlc29sdmVyID0gcmVzOwogICAgICAgICAgICB0aGlzLl9fcmVqZWN0b3IgPSByZWo7CiAgICAgICAgfSk7CiAgICAgICAgcC5fX2hhbmRsZXIgPSBmdW5jdGlvbiBfX2hhbmRsZXIodXNlcnMpCiAgICAgICAgewogICAgICAgICAgICBwLl9fcmVzb2x2ZXIodXNlcnMpOwogICAgICAgIH07CiAgICAgICAgdHJ5CiAgICAgICAgewogICAgICAgICAgICB0aGlzLkN1cnJlbnQocC5fX2hhbmRsZXIpOwogICAgICAgIH0KICAgICAgICBjYXRjaChlKQogICAgICAgIHsKICAgICAgICAgICAgcC5fX3JlamVjdG9yKGUpOwogICAgICAgIH0KICAgICAgICBwLnBhcmVudCA9IHRoaXM7CiAgICAgICAgcmV0dXJuIChwKTsKICAgIH0KCiAgICBpZiAocHJvY2Vzcy5wbGF0Zm9ybSA9PSAnd2luMzInKQogICAgewogICAgICAgIHRoaXMuX3NlcnZpY2VIb29rZWQgPSBmYWxzZTsKICAgICAgICB0aGlzLl9tYXJzaGFsID0gcmVxdWlyZSgnX0dlbmVyaWNNYXJzaGFsJyk7CiAgICAgICAgdGhpcy5fa2VybmVsMzIgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZU5hdGl2ZVByb3h5KCdLZXJuZWwzMi5kbGwnKTsKICAgICAgICB0aGlzLl9rZXJuZWwzMi5DcmVhdGVNZXRob2QoJ0dldExhc3RFcnJvcicpOwogICAgICAgIAogICAgICAgIHRyeQogICAgICAgIHsKICAgICAgICAgICAgdGhpcy5fd3RzID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVOYXRpdmVQcm94eSgnV3RzYXBpMzIuZGxsJyk7CiAgICAgICAgICAgIHRoaXMuX3d0cy5DcmVhdGVNZXRob2QoJ1dUU0VudW1lcmF0ZVNlc3Npb25zQScpOwogICAgICAgICAgICB0aGlzLl93dHMuQ3JlYXRlTWV0aG9kKCdXVFNRdWVyeVNlc3Npb25JbmZvcm1hdGlvbkEnKTsKICAgICAgICAgICAgdGhpcy5fd3RzLkNyZWF0ZU1ldGhvZCgnV1RTUmVnaXN0ZXJTZXNzaW9uTm90aWZpY2F0aW9uJyk7CiAgICAgICAgICAgIHRoaXMuX3d0cy5DcmVhdGVNZXRob2QoJ1dUU1VuUmVnaXN0ZXJTZXNzaW9uTm90aWZpY2F0aW9uJyk7CiAgICAgICAgICAgIHRoaXMuX3d0cy5DcmVhdGVNZXRob2QoJ1dUU0ZyZWVNZW1vcnknKTsKICAgICAgICB9CiAgICAgICAgY2F0Y2goZXhjKQogICAgICAgIHsKICAgICAgICB9CgogICAgICAgIHRoaXMuX2FkdmFwaSA9IHRoaXMuX21hcnNoYWwuQ3JlYXRlTmF0aXZlUHJveHkoJ0FkdmFwaTMyLmRsbCcpOwogICAgICAgIHRoaXMuX2FkdmFwaS5DcmVhdGVNZXRob2QoJ0FsbG9jYXRlQW5kSW5pdGlhbGl6ZVNpZCcpOwogICAgICAgIHRoaXMuX2FkdmFwaS5DcmVhdGVNZXRob2QoJ0NoZWNrVG9rZW5NZW1iZXJzaGlwJyk7CiAgICAgICAgdGhpcy5fYWR2YXBpLkNyZWF0ZU1ldGhvZCgnRnJlZVNpZCcpOwoKICAgICAgICB0aGlzLl91c2VyMzIgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZU5hdGl2ZVByb3h5KCd1c2VyMzIuZGxsJyk7CiAgICAgICAgdGhpcy5fdXNlcjMyLkNyZWF0ZU1ldGhvZCh7IG1ldGhvZDogJ1JlZ2lzdGVyUG93ZXJTZXR0aW5nTm90aWZpY2F0aW9uJywgdGhyZWFkRGlzcGF0Y2g6IDF9KTsKICAgICAgICB0aGlzLl91c2VyMzIuQ3JlYXRlTWV0aG9kKCdVbnJlZ2lzdGVyUG93ZXJTZXR0aW5nTm90aWZpY2F0aW9uJyk7CiAgICAgICAgdGhpcy5fcnBjcnQgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZU5hdGl2ZVByb3h5KCdScGNydDQuZGxsJyk7CiAgICAgICAgdGhpcy5fcnBjcnQuQ3JlYXRlTWV0aG9kKCdVdWlkRnJvbVN0cmluZ0EnKTsKICAgICAgICB0aGlzLl9ycGNydC5TdHJpbmdUb1VVSUQgPSBmdW5jdGlvbiBTdHJpbmdUb1VVSUQoZ3VpZCkKICAgICAgICB7CiAgICAgICAgICAgIHZhciByZXRWYWwgPSBTdHJpbmdUb1VVSUQudXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUoMTYpOwogICAgICAgICAgICBpZihTdHJpbmdUb1VVSUQudXMuX3JwY3J0LlV1aWRGcm9tU3RyaW5nQShTdHJpbmdUb1VVSUQudXMuX21hcnNoYWwuQ3JlYXRlVmFyaWFibGUoZ3VpZCksIHJldFZhbCkuVmFsID09IDApCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIHJldHVybiAocmV0VmFsKTsKICAgICAgICAgICAgfQogICAgICAgICAgICBlbHNlCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIHRocm93ICgnQ291bGQgbm90IGNvbnZlcnQgc3RyaW5nIHRvIFVVSUQnKTsKICAgICAgICAgICAgfQogICAgICAgIH0KICAgICAgICB0aGlzLl9ycGNydC5TdHJpbmdUb1VVSUQudXMgPSB0aGlzOwoKICAgICAgICBHVUlEX0FDRENfUE9XRVJfU09VUkNFID0gdGhpcy5fcnBjcnQuU3RyaW5nVG9VVUlEKCc1ZDNlOWE1OS1lOUQ1LTRiMDAtYTZiZC1mZjM0ZmY1MTY1NDgnKTsKICAgICAgICBHVUlEX0JBVFRFUllfUEVSQ0VOVEFHRV9SRU1BSU5JTkcgPSB0aGlzLl9ycGNydC5TdHJpbmdUb1VVSUQoJ2E3YWQ4MDQxLWI0NWEtNGNhZS04N2EzLWVlY2JiNDY4YTllMScpOwogICAgICAgIEdVSURfQ09OU09MRV9ESVNQTEFZX1NUQVRFID0gdGhpcy5fcnBjcnQuU3RyaW5nVG9VVUlEKCc2ZmU2OTU1Ni03MDRhLTQ3YTAtOGYyNC1jMjhkOTM2ZmRhNDcnKTsKCiAgICAgICAgdGhpcy5TZXNzaW9uU3RhdGVzID0gWydBY3RpdmUnLCAnQ29ubmVjdGVkJywgJ0Nvbm5lY3RRdWVyeScsICdTaGFkb3cnLCAnRGlzY29ubmVjdGVkJywgJ0lkbGUnLCAnTGlzdGVuaW5nJywgJ1Jlc2V0JywgJ0Rvd24nLCAnSW5pdCddOwogICAgICAgIHRoaXMuSW5mb0NsYXNzID0KICAgICAgICAgICAgewogICAgICAgICAgICAgICAgJ1dUU0luaXRpYWxQcm9ncmFtJzogMCwKICAgICAgICAgICAgICAgICdXVFNBcHBsaWNhdGlvbk5hbWUnOiAxLAogICAgICAgICAgICAgICAgJ1dUU1dvcmtpbmdEaXJlY3RvcnknOiAyLAogICAgICAgICAgICAgICAgJ1dUU09FTUlkJzogMywKICAgICAgICAgICAgICAgICdXVFNTZXNzaW9uSWQnOiA0LAogICAgICAgICAgICAgICAgJ1dUU1VzZXJOYW1lJzogNSwKICAgICAgICAgICAgICAgICdXVFNXaW5TdGF0aW9uTmFtZSc6IDYsCiAgICAgICAgICAgICAgICAnV1RTRG9tYWluTmFtZSc6IDcsCiAgICAgICAgICAgICAgICAnV1RTQ29ubmVjdFN0YXRlJzogOCwKICAgICAgICAgICAgICAgICdXVFNDbGllbnRCdWlsZE51bWJlcic6IDksCiAgICAgICAgICAgICAgICAnV1RTQ2xpZW50TmFtZSc6IDEwLAogICAgICAgICAgICAgICAgJ1dUU0NsaWVudERpcmVjdG9yeSc6IDExLAogICAgICAgICAgICAgICAgJ1dUU0NsaWVudFByb2R1Y3RJZCc6IDEyLAogICAgICAgICAgICAgICAgJ1dUU0NsaWVudEhhcmR3YXJlSWQnOiAxMywKICAgICAgICAgICAgICAgICdXVFNDbGllbnRBZGRyZXNzJzogMTQsCiAgICAgICAgICAgICAgICAnV1RTQ2xpZW50RGlzcGxheSc6IDE1LAogICAgICAgICAgICAgICAgJ1dUU0NsaWVudFByb3RvY29sVHlwZSc6IDE2LAogICAgICAgICAgICAgICAgJ1dUU0lkbGVUaW1lJzogMTcsCiAgICAgICAgICAgICAgICAnV1RTTG9nb25UaW1lJzogMTgsCiAgICAgICAgICAgICAgICAnV1RTSW5jb21pbmdCeXRlcyc6IDE5LAogICAgICAgICAgICAgICAgJ1dUU091dGdvaW5nQnl0ZXMnOiAyMCwKICAgICAgICAgICAgICAgICdXVFNJbmNvbWluZ0ZyYW1lcyc6IDIxLAogICAgICAgICAgICAgICAgJ1dUU091dGdvaW5nRnJhbWVzJzogMjIsCiAgICAgICAgICAgICAgICAnV1RTQ2xpZW50SW5mbyc6IDIzLAogICAgICAgICAgICAgICAgJ1dUU1Nlc3Npb25JbmZvJzogMjQsCiAgICAgICAgICAgICAgICAnV1RTU2Vzc2lvbkluZm9FeCc6IDI1LAogICAgICAgICAgICAgICAgJ1dUU0NvbmZpZ0luZm8nOiAyNiwKICAgICAgICAgICAgICAgICdXVFNWYWxpZGF0aW9uSW5mbyc6IDI3LAogICAgICAgICAgICAgICAgJ1dUU1Nlc3Npb25BZGRyZXNzVjQnOiAyOCwKICAgICAgICAgICAgICAgICdXVFNJc1JlbW90ZVNlc3Npb24nOiAyOQogICAgICAgICAgICB9OwoKICAgICAgICB0aGlzLmlzUm9vdCA9IGZ1bmN0aW9uIGlzUm9vdCgpCiAgICAgICAgewogICAgICAgICAgICB2YXIgTlRBdXRob3JpdHkgPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZVZhcmlhYmxlKDYpOwogICAgICAgICAgICBOVEF1dGhvcml0eS50b0J1ZmZlcigpLndyaXRlSW50OCg1LCA1KTsKCiAgICAgICAgICAgIHZhciBBZG1pbmlzdHJhdG9yc0dyb3VwID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVQb2ludGVyKCk7CiAgICAgICAgICAgIHZhciBhZG1pbiA9IGZhbHNlOwoKICAgICAgICAgICAgaWYgKHRoaXMuX2FkdmFwaS5BbGxvY2F0ZUFuZEluaXRpYWxpemVTaWQoTlRBdXRob3JpdHksIDIsIDMyLCA1NDQsIDAsIDAsIDAsIDAsIDAsIDAsIEFkbWluaXN0cmF0b3JzR3JvdXApLlZhbCAhPSAwKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICB2YXIgbWVtYmVyID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVJbnRlZ2VyKCk7CiAgICAgICAgICAgICAgICBpZiAodGhpcy5fYWR2YXBpLkNoZWNrVG9rZW5NZW1iZXJzaGlwKDAsIEFkbWluaXN0cmF0b3JzR3JvdXAuRGVyZWYoKSwgbWVtYmVyKS5WYWwgIT0gMCkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICBpZiAobWVtYmVyLnRvQnVmZmVyKCkucmVhZFVJbnQzMkxFKCkgIT0gMCkgeyBhZG1pbiA9IHRydWU7IH0KICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIHRoaXMuX2FkdmFwaS5GcmVlU2lkKEFkbWluaXN0cmF0b3JzR3JvdXAuRGVyZWYoKSk7CiAgICAgICAgICAgIH0KICAgICAgICAgICAgcmV0dXJuIGFkbWluOwogICAgICAgIH0KCiAgICAgICAgdGhpcy5nZXRTZXNzaW9uQXR0cmlidXRlID0gZnVuY3Rpb24gZ2V0U2Vzc2lvbkF0dHJpYnV0ZShzZXNzaW9uSWQsIGF0dHIpCiAgICAgICAgewogICAgICAgICAgICB2YXIgYnVmZmVyID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVQb2ludGVyKCk7CiAgICAgICAgICAgIHZhciBieXRlc1JldHVybmVkID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVWYXJpYWJsZSg0KTsKCiAgICAgICAgICAgIGlmICh0aGlzLl93dHMuV1RTUXVlcnlTZXNzaW9uSW5mb3JtYXRpb25BKDAsIHNlc3Npb25JZCwgYXR0ciwgYnVmZmVyLCBieXRlc1JldHVybmVkKS5WYWwgPT0gMCkKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgdGhyb3cgKCdFcnJvciBjYWxsaW5nIFdUU1F1ZXJ5U2Vzc2lvbkluZm9ybWF0aW9uOiAnICsgdGhpcy5fa2VybmVsMzIuR2V0TGFzdEVycm9yLlZhbCk7CiAgICAgICAgICAgIH0KCiAgICAgICAgICAgIHZhciByZXRWYWwgPSBidWZmZXIuRGVyZWYoKS5TdHJpbmc7CgogICAgICAgICAgICB0aGlzLl93dHMuV1RTRnJlZU1lbW9yeShidWZmZXIuRGVyZWYoKSk7CiAgICAgICAgICAgIHJldHVybiAocmV0VmFsKTsKICAgICAgICB9OwoKICAgICAgICB0aGlzLkN1cnJlbnQgPSBmdW5jdGlvbiBDdXJyZW50KGNiKQogICAgICAgIHsKICAgICAgICAgICAgdmFyIHJldFZhbCA9IHt9OwogICAgICAgICAgICB2YXIgcGluZm8gPSB0aGlzLl9tYXJzaGFsLkNyZWF0ZVBvaW50ZXIoKTsKICAgICAgICAgICAgdmFyIGNvdW50ID0gdGhpcy5fbWFyc2hhbC5DcmVhdGVWYXJpYWJsZSg0KTsKICAgICAgICAgICAgaWYgKHRoaXMuX3d0cy5XVFNFbnVtZXJhdGVTZXNzaW9uc0EoMCwgMCwgMSwgcGluZm8sIGNvdW50KS5WYWwgPT0gMCkKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgdGhyb3cgKCdFcnJvciBjYWxsaW5nIFdUU0VudW1lcmF0ZVNlc3Npb25zQTogJyArIHRoaXMuX2tlcm5lbDMyLkdldExhc3RFcnJvcigpLlZhbCk7CiAgICAgICAgICAgIH0KCiAgICAgICAgICAgIGZvciAodmFyIGkgPSAwOyBpIDwgY291bnQudG9CdWZmZXIoKS5yZWFkVUludDMyTEUoKSA7ICsraSkKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgdmFyIGluZm8gPSBwaW5mby5EZXJlZigpLkRlcmVmKGkgKiAodGhpcy5fbWFyc2hhbC5Qb2ludGVyU2l6ZSA9PSA0ID8gMTIgOiAyNCksIHRoaXMuX21hcnNoYWwuUG9pbnRlclNpemUgPT0gNCA/IDEyIDogMjQpOwogICAgICAgICAgICAgICAgdmFyIGogPSB7IFNlc3Npb25JZDogaW5mby50b0J1ZmZlcigpLnJlYWRVSW50MzJMRSgpIH07CiAgICAgICAgICAgICAgICBqLlN0YXRpb25OYW1lID0gaW5mby5EZXJlZih0aGlzLl9tYXJzaGFsLlBvaW50ZXJTaXplID09IDQgPyA0IDogOCwgdGhpcy5fbWFyc2hhbC5Qb2ludGVyU2l6ZSkuRGVyZWYoKS5TdHJpbmc7CiAgICAgICAgICAgICAgICBqLlN0YXRlID0gdGhpcy5TZXNzaW9uU3RhdGVzW2luZm8uRGVyZWYodGhpcy5fbWFyc2hhbC5Qb2ludGVyU2l6ZSA9PSA0ID8gOCA6IDE2LCA0KS50b0J1ZmZlcigpLnJlYWRVSW50MzJMRSgpXTsKICAgICAgICAgICAgICAgIGlmIChqLlN0YXRlID09ICdBY3RpdmUnKSB7CiAgICAgICAgICAgICAgICAgICAgai5Vc2VybmFtZSA9IHRoaXMuZ2V0U2Vzc2lvbkF0dHJpYnV0ZShqLlNlc3Npb25JZCwgdGhpcy5JbmZvQ2xhc3MuV1RTVXNlck5hbWUpOwogICAgICAgICAgICAgICAgICAgIGouRG9tYWluID0gdGhpcy5nZXRTZXNzaW9uQXR0cmlidXRlKGouU2Vzc2lvbklkLCB0aGlzLkluZm9DbGFzcy5XVFNEb21haW5OYW1lKTsKICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIHJldFZhbFtqLlNlc3Npb25JZF0gPSBqOwogICAgICAgICAgICB9CgogICAgICAgICAgICB0aGlzLl93dHMuV1RTRnJlZU1lbW9yeShwaW5mby5EZXJlZigpKTsKCiAgICAgICAgICAgIE9iamVjdC5kZWZpbmVQcm9wZXJ0eShyZXRWYWwsICdBY3RpdmUnLCB7IHZhbHVlOiBzaG93QWN0aXZlT25seShyZXRWYWwpIH0pOwogICAgICAgICAgICBpZiAoY2IpIHsgY2IocmV0VmFsKTsgfQogICAgICAgICAgICByZXR1cm4gKHJldFZhbCk7CiAgICAgICAgfTsKCgogICAgICAgIC8vIFdlIG5lZWQgdG8gc3BpbiB1cCBhIG1lc3NhZ2UgcHVtcCwgYW5kIGZldGNoIGEgd2luZG93IGhhbmRsZQogICAgICAgIHZhciBtZXNzYWdlX3B1bXAgPSByZXF1aXJlKCd3aW4tbWVzc2FnZS1wdW1wJyk7CiAgICAgICAgdGhpcy5fbWVzc2FnZXB1bXAgPSBuZXcgbWVzc2FnZV9wdW1wKHsgZmlsdGVyOiBXTV9XVFNTRVNTSU9OX0NIQU5HRSB9KTsgdGhpcy5fbWVzc2FnZXB1bXAucGFyZW50ID0gdGhpczsgICAgIAogICAgICAgIHRoaXMuX21lc3NhZ2VwdW1wLm9uKCdleGl0JywgZnVuY3Rpb24gKGNvZGUpIHsgdGhpcy5wYXJlbnQuX3d0cy5XVFNVblJlZ2lzdGVyU2Vzc2lvbk5vdGlmaWNhdGlvbih0aGlzLnBhcmVudC5od25kKTsgfSk7CiAgICAgICAgdGhpcy5fbWVzc2FnZXB1bXAub24oJ2h3bmQnLCBmdW5jdGlvbiAoaCkKICAgICAgICB7CiAgICAgICAgICAgIHRoaXMucGFyZW50Lmh3bmQgPSBoOwoKICAgICAgICAgICAgLy8gV2UgbmVlZCB0byB5aWVsZCwgYW5kIGRvIHRoaXMgaW4gdGhlIG5leHQgZXZlbnQgbG9vcCBwYXNzLCBiZWN1YXNlIHdlIGRvbid0IHdhbnQgdG8gY2FsbCAnUmVnaXN0ZXJQb3dlclNldHRpbmdOb3RpZmljYXRpb24nCiAgICAgICAgICAgIC8vIGZyb20gdGhlIG1lc3NhZ2VwdW1wICd0aHJlYWQnLCBiZWNhdXNlIHdlIGFyZSBhY3R1YWxseSBvbiB0aGUgbWljcm9zdGFjayB0aHJlYWQsIHN1Y2ggdGhhdCB0aGUgbWVzc2FnZSBwdW1wIHRocmVhZCwgaXMgaG9sZGluZwogICAgICAgICAgICAvLyBvbiBhIHNlbWFwaG9yZSBmb3IgdXMgdG8gcmV0dXJuLiBJZiB3ZSBjYWxsIG5vdywgd2UgbWF5IGRlYWRsb2NrIG9uIFdpbmRvd3MgNywgYmVjdWFzZSBpdCB3aWxsIHRyeSB0byBub3RpZnkgaW1tZWRpYXRlbHkKICAgICAgICAgICAgdGhpcy5pbW1lZGlhdGUgPSBzZXRJbW1lZGlhdGUoZnVuY3Rpb24gKHNlbGYpCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIC8vIE5vdyB0aGF0IHdlIGhhdmUgYSB3aW5kb3cgaGFuZGxlLCB3ZSBjYW4gcmVnaXN0ZXIgaXQgdG8gcmVjZWl2ZSBXaW5kb3dzIE1lc3NhZ2VzCiAgICAgICAgICAgICAgICBpZiAoc2VsZi5wYXJlbnQuX3d0cykgeyBzZWxmLnBhcmVudC5fd3RzLldUU1JlZ2lzdGVyU2Vzc2lvbk5vdGlmaWNhdGlvbihzZWxmLnBhcmVudC5od25kLCBOT1RJRllfRk9SX0FMTF9TRVNTSU9OUyk7IH0KICAgICAgICAgICAgICAgIHNlbGYucGFyZW50Ll91c2VyMzIuQUNEQ19IID0gc2VsZi5wYXJlbnQuX3VzZXIzMi5SZWdpc3RlclBvd2VyU2V0dGluZ05vdGlmaWNhdGlvbihzZWxmLnBhcmVudC5od25kLCBHVUlEX0FDRENfUE9XRVJfU09VUkNFLCAwKTsKICAgICAgICAgICAgICAgIHNlbGYucGFyZW50Ll91c2VyMzIuQkFUVF9IID0gc2VsZi5wYXJlbnQuX3VzZXIzMi5SZWdpc3RlclBvd2VyU2V0dGluZ05vdGlmaWNhdGlvbihzZWxmLnBhcmVudC5od25kLCBHVUlEX0JBVFRFUllfUEVSQ0VOVEFHRV9SRU1BSU5JTkcsIDApOwogICAgICAgICAgICAgICAgc2VsZi5wYXJlbnQuX3VzZXIzMi5ESVNQX0ggPSBzZWxmLnBhcmVudC5fdXNlcjMyLlJlZ2lzdGVyUG93ZXJTZXR0aW5nTm90aWZpY2F0aW9uKHNlbGYucGFyZW50Lmh3bmQsIEdVSURfQ09OU09MRV9ESVNQTEFZX1NUQVRFLCAwKTsKICAgICAgICAgICAgICAgIC8vY29uc29sZS5sb2coc2VsZi5wYXJlbnQuX3VzZXIzMi5BQ0RDX0guVmFsLCBzZWxmLnBhcmVudC5fdXNlcjMyLkJBVFRfSC5WYWwsIHNlbGYucGFyZW50Ll91c2VyMzIuRElTUF9ILlZhbCk7CiAgICAgICAgICAgIH0sIHRoaXMpOwogICAgICAgIH0pOwogICAgICAgIHRoaXMuX21lc3NhZ2VwdW1wLm9uKCdtZXNzYWdlJywgZnVuY3Rpb24gKG1zZykKICAgICAgICB7CiAgICAgICAgICAgIHN3aXRjaChtc2cubWVzc2FnZSkKICAgICAgICAgICAgewogICAgICAgICAgICAgICAgY2FzZSBXTV9XVFNTRVNTSU9OX0NIQU5HRToKICAgICAgICAgICAgICAgICAgICBzd2l0Y2gobXNnLndwYXJhbSkKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIGNhc2UgV1RTX1NFU1NJT05fTE9DSzoKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHRoaXMucGFyZW50LmVudW1lcmF0ZVVzZXJzKCkudGhlbihmdW5jdGlvbiAodXNlcnMpCiAgICAgICAgICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgaWYgKHVzZXJzW21zZy5scGFyYW1dKSB7IHRoaXMucGFyZW50LmVtaXQoJ2xvY2tlZCcsIHVzZXJzW21zZy5scGFyYW1dKTsgfQogICAgICAgICAgICAgICAgICAgICAgICAgICAgfSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSBXVFNfU0VTU0lPTl9VTkxPQ0s6CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB0aGlzLnBhcmVudC5lbnVtZXJhdGVVc2VycygpLnRoZW4oZnVuY3Rpb24gKHVzZXJzKQogICAgICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGlmICh1c2Vyc1ttc2cubHBhcmFtXSkgeyB0aGlzLnBhcmVudC5lbWl0KCd1bmxvY2tlZCcsIHVzZXJzW21zZy5scGFyYW1dKTsgfQogICAgICAgICAgICAgICAgICAgICAgICAgICAgfSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSBXVFNfU0VTU0lPTl9MT0dPTjoKICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSBXVFNfU0VTU0lPTl9MT0dPRkY6CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB0aGlzLnBhcmVudC5lbWl0KCdjaGFuZ2VkJyk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICBjYXNlIFdNX1BPV0VSQlJPQURDQVNUOgogICAgICAgICAgICAgICAgICAgIHN3aXRjaChtc2cud3BhcmFtKQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgZGVmYXVsdDoKICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNvbnNvbGUubG9nKCdXTV9QT1dFUkJST0FEQ0FTVCBbVU5LTk9XTiB3cGFyYW1dOiAnICsgbXNnLndwYXJhbSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSBQQlRfQVBNU1VTUEVORDoKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ3Bvd2VyLW1vbml0b3InKS5lbWl0KCdzeCcsICdTTEVFUCcpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICAgICAgICAgIGNhc2UgUEJUX0FQTVJFU1VNRUFVVE9NQVRJQzoKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ3Bvd2VyLW1v", 16000);
	memcpy_s(_usersessions + 16000, 25080, "bml0b3InKS5lbWl0KCdzeCcsICdSRVNVTUVfTk9OX0lOVEVSQUNUSVZFJyk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSBQQlRfQVBNUkVTVU1FU1VTUEVORDoKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ3Bvd2VyLW1vbml0b3InKS5lbWl0KCdzeCcsICdSRVNVTUVfSU5URVJBQ1RJVkUnKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgICAgICBjYXNlIFBCVF9BUE1QT1dFUlNUQVRVU0NIQU5HRToKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ3Bvd2VyLW1vbml0b3InKS5lbWl0KCdjaGFuZ2VkJyk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSBQQlRfUE9XRVJTRVRUSU5HQ0hBTkdFOgogICAgICAgICAgICAgICAgICAgICAgICAgICAgdmFyIGxwYXJhbSA9IHRoaXMucGFyZW50Ll9tYXJzaGFsLkNyZWF0ZVBvaW50ZXIoQnVmZmVyLmZyb20obXNnLmxwYXJhbV9oZXgsICdoZXgnKSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB2YXIgZGF0YSA9IGxwYXJhbS5EZXJlZigyMCwgbHBhcmFtLkRlcmVmKDE2LCA0KS50b0J1ZmZlcigpLnJlYWRVSW50MzJMRSgwKSkudG9CdWZmZXIoKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHN3aXRjaChscGFyYW0uRGVyZWYoMCwgMTYpLnRvQnVmZmVyKCkudG9TdHJpbmcoJ2hleCcpKQogICAgICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNhc2UgR1VJRF9BQ0RDX1BPV0VSX1NPVVJDRS5EZXJlZigwLCAxNikudG9CdWZmZXIoKS50b1N0cmluZygnaGV4Jyk6CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHN3aXRjaChkYXRhLnJlYWRVSW50MzJMRSgwKSkKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSAwOgogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ3Bvd2VyLW1vbml0b3InKS5lbWl0KCdhY2RjJywgJ0FDJyk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjYXNlIDE6CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgcmVxdWlyZSgncG93ZXItbW9uaXRvcicpLmVtaXQoJ2FjZGMnLCAnQkFUVEVSWScpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSAyOgogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ3Bvd2VyLW1vbml0b3InKS5lbWl0KCdhY2RjJywgJ0hPVCcpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNhc2UgR1VJRF9CQVRURVJZX1BFUkNFTlRBR0VfUkVNQUlOSU5HLkRlcmVmKDAsIDE2KS50b0J1ZmZlcigpLnRvU3RyaW5nKCdoZXgnKToKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgcmVxdWlyZSgncG93ZXItbW9uaXRvcicpLmVtaXQoJ2JhdHRlcnlMZXZlbCcsIGRhdGEucmVhZFVJbnQzMkxFKDApKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSBHVUlEX0NPTlNPTEVfRElTUExBWV9TVEFURS5EZXJlZigwLCAxNikudG9CdWZmZXIoKS50b1N0cmluZygnaGV4Jyk6CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHN3aXRjaChkYXRhLnJlYWRVSW50MzJMRSgwKSkKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSAwOgogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ3Bvd2VyLW1vbml0b3InKS5lbWl0KCdkaXNwbGF5JywgJ09GRicpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSAxOgogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJlcXVpcmUoJ3Bvd2VyLW1vbml0b3InKS5lbWl0KCdkaXNwbGF5JywgJ09OJyk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjYXNlIDI6CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgcmVxdWlyZSgncG93ZXItbW9uaXRvcicpLmVtaXQoJ2Rpc3BsYXknLCAnRElNTUVEJyk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBicmVhazsKICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICBkZWZhdWx0OgogICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICB9CiAgICAgICAgfSk7CiAgICB9CiAgICBlbHNlIGlmKHByb2Nlc3MucGxhdGZvcm0gPT0gJ2xpbnV4JykKICAgIHsKICAgICAgICB2YXIgZGJ1cyA9IHJlcXVpcmUoJ2xpbnV4LWRidXMnKTsKICAgICAgICB0aGlzLl9saW51eFdhdGNoZXIgPSByZXF1aXJlKCdmcycpLndhdGNoKCcvdmFyL3J1bi91dG1wJyk7CiAgICAgICAgdGhpcy5fbGludXhXYXRjaGVyLnVzZXJfc2Vzc2lvbiA9IHRoaXM7CiAgICAgICAgdGhpcy5fbGludXhXYXRjaGVyLm9uKCdjaGFuZ2UnLCBmdW5jdGlvbiAoYSwgYikKICAgICAgICB7CiAgICAgICAgICAgIHRoaXMudXNlcl9zZXNzaW9uLmVtaXQoJ2NoYW5nZWQnKTsKICAgICAgICB9KTsKICAgICAgICB0aGlzLl91c2VycyA9IGZ1bmN0aW9uIF91c2VycygpCiAgICAgICAgewogICAgICAgICAgICB2YXIgY2hpbGQgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy9iaW4vc2gnLCBbJ3NoJ10pOwogICAgICAgICAgICBjaGlsZC5zdGRvdXQuc3RyID0gJyc7CiAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5vbignZGF0YScsIGZ1bmN0aW9uIChjaHVuaykgeyB0aGlzLnN0ciArPSBjaHVuay50b1N0cmluZygpOyB9KTsKICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ2F3ayAtRjogXCcoJDMgPj0gMCkge3ByaW50ZiAiJXM6JXNcXG4iLCAkMSwgJDN9XCcgL2V0Yy9wYXNzd2RcbmV4aXRcbicpOwogICAgICAgICAgICBjaGlsZC53YWl0RXhpdCgpOwoKICAgICAgICAgICAgdmFyIGxpbmVzID0gY2hpbGQuc3Rkb3V0LnN0ci5zcGxpdCgnXG4nKTsKICAgICAgICAgICAgdmFyIHJldCA9IHt9LCB0b2tlbnM7CiAgICAgICAgICAgIGZvciAodmFyIGxuIGluIGxpbmVzKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICB0b2tlbnMgPSBsaW5lc1tsbl0uc3BsaXQoJzonKTsKICAgICAgICAgICAgICAgIGlmICh0b2tlbnNbMF0pIHsgcmV0W3Rva2Vuc1swXV0gPSB0b2tlbnNbMV07IH0gICAgICAgICAgIAogICAgICAgICAgICB9CiAgICAgICAgICAgIHJldHVybiAocmV0KTsKICAgICAgICB9CiAgICAgICAgdGhpcy5fdWlkcyA9IGZ1bmN0aW9uIF91aWRzKCkgewogICAgICAgICAgICB2YXIgY2hpbGQgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy9iaW4vc2gnLCBbJ3NoJ10pOwogICAgICAgICAgICBjaGlsZC5zdGRvdXQuc3RyID0gJyc7CiAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5vbignZGF0YScsIGZ1bmN0aW9uIChjaHVuaykgeyB0aGlzLnN0ciArPSBjaHVuay50b1N0cmluZygpOyB9KTsKICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ2F3ayAtRjogXCcoJDMgPj0gMCkge3ByaW50ZiAiJXM6JXNcXG4iLCAkMSwgJDN9XCcgL2V0Yy9wYXNzd2RcbmV4aXRcbicpOwogICAgICAgICAgICBjaGlsZC53YWl0RXhpdCgpOwoKICAgICAgICAgICAgdmFyIGxpbmVzID0gY2hpbGQuc3Rkb3V0LnN0ci5zcGxpdCgnXG4nKTsKICAgICAgICAgICAgdmFyIHJldCA9IHt9LCB0b2tlbnM7CiAgICAgICAgICAgIGZvciAodmFyIGxuIGluIGxpbmVzKSB7CiAgICAgICAgICAgICAgICB0b2tlbnMgPSBsaW5lc1tsbl0uc3BsaXQoJzonKTsKICAgICAgICAgICAgICAgIGlmICh0b2tlbnNbMF0pIHsgcmV0W3Rva2Vuc1sxXV0gPSB0b2tlbnNbMF07IH0KICAgICAgICAgICAgfQogICAgICAgICAgICByZXR1cm4gKHJldCk7CiAgICAgICAgfQogICAgICAgIHRoaXMuU2VsZiA9IGZ1bmN0aW9uIFNlbGYoKQogICAgICAgIHsKICAgICAgICAgICAgdmFyIHByb21pc2UgPSByZXF1aXJlKCdwcm9taXNlJyk7CiAgICAgICAgICAgIHZhciBwID0gbmV3IHByb21pc2UoZnVuY3Rpb24gKHJlcywgcmVqKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICB0aGlzLl9fcmVzb2x2ZXIgPSByZXM7IHRoaXMuX19yZWplY3RvciA9IHJlajsKICAgICAgICAgICAgICAgIHRoaXMuX19jaGlsZCA9IHJlcXVpcmUoJ2NoaWxkX3Byb2Nlc3MnKS5leGVjRmlsZSgnL3Vzci9iaW4vaWQnLCBbJ2lkJywgJy11J10pOwogICAgICAgICAgICAgICAgdGhpcy5fX2NoaWxkLnByb21pc2UgPSB0aGlzOwogICAgICAgICAgICAgICAgdGhpcy5fX2NoaWxkLnN0ZG91dC5fdHh0ID0gJyc7CiAgICAgICAgICAgICAgICB0aGlzLl9fY2hpbGQuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IHRoaXMuX3R4dCArPSBjaHVuay50b1N0cmluZygpOyB9KTsKICAgICAgICAgICAgICAgIHRoaXMuX19jaGlsZC5vbignZXhpdCcsIGZ1bmN0aW9uIChjb2RlKQogICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgIHRyeQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgcGFyc2VJbnQodGhpcy5zdGRvdXQuX3R4dCk7CiAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgIGNhdGNoIChlKQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgdGhpcy5wcm9taXNlLl9fcmVqZWN0b3IoJ2ludmFsaWQgdWlkJyk7CiAgICAgICAgICAgICAgICAgICAgICAgIHJldHVybjsKICAgICAgICAgICAgICAgICAgICB9CgogICAgICAgICAgICAgICAgICAgIHZhciBpZCA9IHBhcnNlSW50KHRoaXMuc3Rkb3V0Ll90eHQpOwogICAgICAgICAgICAgICAgICAgIHRoaXMucHJvbWlzZS5fX3Jlc29sdmVyKGlkKTsKICAgICAgICAgICAgICAgIH0pOwogICAgICAgICAgICB9KTsKICAgICAgICAgICAgcmV0dXJuIChwKTsKICAgICAgICB9OwogICAgICAgIHRoaXMuQ3VycmVudCA9IGZ1bmN0aW9uIEN1cnJlbnQoY2IpCiAgICAgICAgewogICAgICAgICAgICB2YXIgcmV0VmFsID0ge307CiAgICAgICAgICAgIHJldFZhbC5fT2JqZWN0SUQgPSAnVXNlclNlc3Npb24nCiAgICAgICAgICAgIE9iamVjdC5kZWZpbmVQcm9wZXJ0eShyZXRWYWwsICdfY2FsbGJhY2snLCB7IHZhbHVlOiBjYiB9KTsKICAgICAgICAgICAgT2JqZWN0LmRlZmluZVByb3BlcnR5KHJldFZhbCwgJ19jaGlsZCcsIHsgdmFsdWU6IHJlcXVpcmUoJ2NoaWxkX3Byb2Nlc3MnKS5leGVjRmlsZSgnL3Vzci9iaW4vbGFzdCcsIFsnbGFzdCcsICctZicsICcvdmFyL3J1bi91dG1wJ10pIH0pOwoKICAgICAgICAgICAgcmV0VmFsLl9jaGlsZC5QYXJlbnQgPSByZXRWYWw7CiAgICAgICAgICAgIHJldFZhbC5fY2hpbGQuX3R4dCA9ICcnOwogICAgICAgICAgICByZXRWYWwuX2NoaWxkLm9uKCdleGl0JywgZnVuY3Rpb24gKGNvZGUpCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIHZhciBsaW5lcyA9IHRoaXMuX3R4dC5zcGxpdCgnXG4nKTsKICAgICAgICAgICAgICAgIHZhciBzZXNzaW9ucyA9IFtdOwogICAgICAgICAgICAgICAgdmFyIHVzZXJzID0ge307CgogICAgICAgICAgICAgICAgZm9yKHZhciBpIGluIGxpbmVzKQogICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgIGlmIChsaW5lc1tpXSkKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIHZhciB0b2tlbnMgPSBnZXRUb2tlbnMobGluZXNbaV0pOwogICAgICAgICAgICAgICAgICAgICAgICB2YXIgcyA9IHsgVXNlcm5hbWU6IHRva2Vuc1swXSwgU2Vzc2lvbklkOiB0b2tlbnNbMV0gfQogICAgICAgICAgICAgICAgICAgICAgICBpZiAodG9rZW5zWzNdLmluY2x1ZGVzKCdzdGlsbCBsb2dnZWQgaW4nKSkKICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgcy5TdGF0ZSA9ICdBY3RpdmUnOwogICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgIGVsc2UKICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgcy5MYXN0QWN0aXZlID0gdG9rZW5zWzNdOwogICAgICAgICAgICAgICAgICAgICAgICB9CgogICAgICAgICAgICAgICAgICAgICAgICBzZXNzaW9ucy5wdXNoKHMpOwogICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIHNlc3Npb25zLnBvcCgpOwoKCiAgICAgICAgICAgICAgICB2YXIgdXNlcm5hbWVzID0ge307CiAgICAgICAgICAgICAgICB2YXIgcHJvbWlzZXMgPSBbXTsKCiAgICAgICAgICAgICAgICBmb3IgKHZhciBpIGluIHNlc3Npb25zKQogICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgIGlmIChzZXNzaW9uc1tpXS5Vc2VybmFtZSAhPSAncmVib290JykKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIHVzZXJzW3Nlc3Npb25zW2ldLlNlc3Npb25JZF0gPSBzZXNzaW9uc1tpXTsKICAgICAgICAgICAgICAgICAgICAgICAgaWYodXNlcm5hbWVzW3Nlc3Npb25zW2ldLlVzZXJuYW1lXSA9PSBudWxsKQogICAgICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB1c2VybmFtZXNbc2Vzc2lvbnNbaV0uVXNlcm5hbWVdID0gLTE7CiAgICAgICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICB9CgogICAgICAgICAgICAgICAgdHJ5CiAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgcmVxdWlyZSgncHJvbWlzZScpOwogICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgY2F0Y2goZSkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICBPYmplY3QuZGVmaW5lUHJvcGVydHkodXNlcnMsICdBY3RpdmUnLCB7IHZhbHVlOiBzaG93QWN0aXZlT25seSh1c2VycykgfSk7CiAgICAgICAgICAgICAgICAgICAgaWYgKHRoaXMuUGFyZW50Ll9jYWxsYmFjaykgeyB0aGlzLlBhcmVudC5fY2FsbGJhY2suY2FsbCh0aGlzLlBhcmVudCwgdXNlcnMpOyB9CiAgICAgICAgICAgICAgICAgICAgcmV0dXJuOwogICAgICAgICAgICAgICAgfQoKICAgICAgICAgICAgICAgIHZhciBwcm9taXNlID0gcmVxdWlyZSgncHJvbWlzZScpOwogICAgICAgICAgICAgICAgZm9yICh2YXIgbiBpbiB1c2VybmFtZXMpCiAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgdmFyIHAgPSBuZXcgcHJvbWlzZShmdW5jdGlvbiAocmVzLCByZWopCiAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICB0aGlzLl9fdXNlcm5hbWUgPSBuOwogICAgICAgICAgICAgICAgICAgICAgICB0aGlzLl9fcmVzb2x2ZXIgPSByZXM7IHRoaXMuX19yZWplY3RvciA9IHJlajsKICAgICAgICAgICAgICAgICAgICAgICAgdGhpcy5fX2NoaWxkID0gcmVxdWlyZSgnY2hpbGRfcHJvY2VzcycpLmV4ZWNGaWxlKCcvdXNyL2Jpbi9pZCcsIFsnaWQnLCAnLXUnLCBuXSk7CiAgICAgICAgICAgICAgICAgICAgICAgIHRoaXMuX19jaGlsZC5wcm9taXNlID0gdGhpczsKICAgICAgICAgICAgICAgICAgICAgICAgdGhpcy5fX2NoaWxkLnN0ZG91dC5fdHh0ID0gJyc7CiAgICAgICAgICAgICAgICAgICAgICAgIHRoaXMuX19jaGlsZC5zdGRvdXQub24oJ2RhdGEnLCBmdW5jdGlvbiAoY2h1bmspIHsgdGhpcy5fdHh0ICs9IGNodW5rLnRvU3RyaW5nKCk7IH0pOwogICAgICAgICAgICAgICAgICAgICAgICB0aGlzLl9fY2hpbGQub24oJ2V4aXQnLCBmdW5jdGlvbiAoY29kZSkKICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgdHJ5CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgcGFyc2VJbnQodGhpcy5zdGRvdXQuX3R4dCk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgICAgICBjYXRjaChlKQogICAgICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHRoaXMucHJvbWlzZS5fX3JlamVjdG9yKCdpbnZhbGlkIHVpZCcpOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIHJldHVybjsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIH0KCiAgICAgICAgICAgICAgICAgICAgICAgICAgICB2YXIgaWQgPSBwYXJzZUludCh0aGlzLnN0ZG91dC5fdHh0KTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHRoaXMucHJvbWlzZS5fX3Jlc29sdmVyKGlkKTsKICAgICAgICAgICAgICAgICAgICAgICAgfSk7CiAgICAgICAgICAgICAgICAgICAgfSk7CiAgICAgICAgICAgICAgICAgICAgcHJvbWlzZXMucHVzaChwKTsKICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIHByb21pc2UuYWxsKHByb21pc2VzKS50aGVuKGZ1bmN0aW9uIChwbGlzdCkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAvLyBEb25lCiAgICAgICAgICAgICAgICAgICAgdmFyIHRhYmxlID0ge307CiAgICAgICAgICAgICAgICAgICAgZm9yKHZhciBpIGluIHBsaXN0KQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgdGFibGVbcGxpc3RbaV0uX191c2VybmFtZV0gPSBwbGlzdFtpXS5faW50ZXJuYWwuY29tcGxldGVkQXJnc1swXTsKICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgZm9yKHZhciBpIGluIHVzZXJzKQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgdXNlcnNbaV0udWlkID0gdGFibGVbdXNlcnNbaV0uVXNlcm5hbWVdOwogICAgICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgICAgICBPYmplY3QuZGVmaW5lUHJvcGVydHkodXNlcnMsICdBY3RpdmUnLCB7IHZhbHVlOiBzaG93QWN0aXZlT25seSh1c2VycykgfSk7CiAgICAgICAgICAgICAgICAgICAgaWYgKHJldFZhbC5fY2FsbGJhY2spIHsgcmV0VmFsLl9jYWxsYmFjay5jYWxsKHJldFZhbCwgdXNlcnMpOyB9CiAgICAgICAgICAgICAgICB9LCBmdW5jdGlvbiAocmVhc29uKQogICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgIC8vIEZhaWxlZAogICAgICAgICAgICAgICAgICAgIE9iamVjdC5kZWZpbmVQcm9wZXJ0eSh1c2VycywgJ0FjdGl2ZScsIHsgdmFsdWU6IHNob3dBY3RpdmVPbmx5KHVzZXJzKSB9KTsKICAgICAgICAgICAgICAgICAgICBpZiAocmV0VmFsLl9jYWxsYmFjaykgeyByZXRWYWwuX2NhbGxiYWNrLmNhbGwocmV0VmFsLCB1c2Vycyk7IH0KICAgICAgICAgICAgICAgIH0pOwogICAgICAgICAgICB9KTsKICAgICAgICAgICAgcmV0VmFsLl9jaGlsZC5zdGRvdXQuUGFyZW50ID0gcmV0VmFsLl9jaGlsZDsKICAgICAgICAgICAgcmV0VmFsLl9jaGlsZC5zdGRvdXQub24oJ2RhdGEnLCBmdW5jdGlvbiAoY2h1bmspIHsgdGhpcy5QYXJlbnQuX3R4dCArPSBjaHVuay50b1N0cmluZygpOyB9KTsKCiAgICAgICAgICAgIHJldHVybiAocmV0VmFsKTsKICAgICAgICB9CiAgICAgICAgdGhpcy5fcmVjaGVja0xvZ2dlZEluVXNlcnMgPSBmdW5jdGlvbiBfcmVjaGVja0xvZ2dlZEluVXNlcnMoKQogICAgICAgIHsKICAgICAgICAgICAgdGhpcy5lbnVtZXJhdGVVc2VycygpLnRoZW4oZnVuY3Rpb24gKHUpCiAgICAgICAgICAgIHsKCiAgICAgICAgICAgICAgICBpZiAodS5BY3RpdmUubGVuZ3RoID4gMCkKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAvLyBUaGVyZSBpcyBhbHJlYWR5IGEgdXNlciBsb2dnZWQgaW4sIHNvIHdlIGNhbiBtb25pdG9yIERCVVMgZm9yIGxvY2svdW5sb2NrCiAgICAgICAgICAgICAgICAgICAgaWYgKHRoaXMucGFyZW50Ll9saW51eF9sb2NrX3dhdGNoZXIgIT0gbnVsbCAmJiB0aGlzLnBhcmVudC5fbGludXhfbG9ja193YXRjaGVyLnVpZCAhPSB1LkFjdGl2ZVswXS51aWQpCiAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICBkZWxldGUgdGhpcy5wYXJlbnQuX2xpbnV4X2xvY2tfd2F0Y2hlcjsKICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgdGhpcy5wYXJlbnQuX2xpbnV4X2xvY2tfd2F0Y2hlciA9IG5ldyBkYnVzKHByb2Nlc3MuZW52WydYREdfQ1VSUkVOVF9ERVNLVE9QJ10gPT0gJ1VuaXR5JyA/ICdjb20udWJ1bnR1LlVwc3RhcnQwXzYnIDogJ29yZy5nbm9tZS5TY3JlZW5TYXZlcicsIHUuQWN0aXZlWzBdLnVpZCk7CiAgICAgICAgICAgICAgICAgICAgdGhpcy5wYXJlbnQuX2xpbnV4X2xvY2tfd2F0Y2hlci51c2VyX3Nlc3Npb24gPSB0aGlzLnBhcmVudDsKICAgICAgICAgICAgICAgICAgICB0aGlzLnBhcmVudC5fbGludXhfbG9ja193YXRjaGVyLm9uKCdzaWduYWwnLCBmdW5jdGlvbiAocykKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIHZhciBwID0gdGhpcy51c2VyX3Nlc3Npb24uZW51bWVyYXRlVXNlcnMoKTsKICAgICAgICAgICAgICAgICAgICAgICAgcC5zaWduYWxEYXRhID0gcy5kYXRhWzBdOwogICAgICAgICAgICAgICAgICAgICAgICBwLnRoZW4oZnVuY3Rpb24gKHUpCiAgICAgICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgICAgIHN3aXRjaCAodGhpcy5zaWduYWxEYXRhKQogICAgICAgICAgICAgICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNhc2UgdHJ1ZToKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICBjYXNlICdkZXNrdG9wLWxvY2snOgogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICB0aGlzLnBhcmVudC5lbWl0KCdsb2NrZWQnLCB1LkFjdGl2ZVswXSk7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGJyZWFrOwogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIGNhc2UgZmFsc2U6CiAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgY2FzZSAnZGVza3RvcC11bmxvY2snOgogICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICB0aGlzLnBhcmVudC5lbWl0KCd1bmxvY2tlZCcsIHUuQWN0aXZlWzBdKTsKICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgYnJlYWs7CiAgICAgICAgICAgICAgICAgICAgICAgICAgICB9CiAgICAgICAgICAgICAgICAgICAgICAgIH0p", 16000);
	memcpy_s(_usersessions + 32000, 9080, "OwogICAgICAgICAgICAgICAgICAgIH0pOwogICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgZWxzZSBpZiAodGhpcy5wYXJlbnQuX2xpbnV4X2xvY2tfd2F0Y2hlciAhPSBudWxsKQogICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgIGRlbGV0ZSB0aGlzLnBhcmVudC5fbGludXhfbG9ja193YXRjaGVyOwogICAgICAgICAgICAgICAgfQogICAgICAgICAgICB9KTsKCiAgICAgICAgfTsKICAgICAgICB0aGlzLm9uKCdjaGFuZ2VkJywgdGhpcy5fcmVjaGVja0xvZ2dlZEluVXNlcnMpOyAvLyBGb3IgbGludXggTG9jay9VbmxvY2sgbW9uaXRvcmluZywgd2UgbmVlZCB0byB3YXRjaCBmb3IgTG9nT24vTG9nT2ZmLCBhbmQga2VlcCB0cmFjayBvZiB0aGUgVUlELgoKICAgICAgICAKICAgICAgICAvLyBGaXJzdCBzdGVwLCBpcyB0byBzZWUgaWYgdGhlcmUgaXMgYSB1c2VyIGxvZ2dlZCBpbjoKICAgICAgICB0aGlzLl9yZWNoZWNrTG9nZ2VkSW5Vc2VycygpOwogICAgfQogICAgZWxzZSBpZihwcm9jZXNzLnBsYXRmb3JtID09ICdkYXJ3aW4nKQogICAgewogICAgICAgIHRoaXMuX3VzZXJzID0gZnVuY3Rpb24gKCkKICAgICAgICB7CiAgICAgICAgICAgIHZhciBjaGlsZCA9IHJlcXVpcmUoJ2NoaWxkX3Byb2Nlc3MnKS5leGVjRmlsZSgnL3Vzci9iaW4vZHNjbCcsIFsnZHNjbCcsICcuJywgJ2xpc3QnLCAnL1VzZXJzJywgJ1VuaXF1ZUlEJ10pOwogICAgICAgICAgICBjaGlsZC5zdGRvdXQuc3RyID0gJyc7CiAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5vbignZGF0YScsIGZ1bmN0aW9uIChjaHVuaykgeyB0aGlzLnN0ciArPSBjaHVuay50b1N0cmluZygpOyB9KTsKICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ2V4aXRcbicpOwogICAgICAgICAgICBjaGlsZC53YWl0RXhpdCgpOwoKICAgICAgICAgICAgdmFyIGxpbmVzID0gY2hpbGQuc3Rkb3V0LnN0ci5zcGxpdCgnXG4nKTsKICAgICAgICAgICAgdmFyIHRva2VucywgaTsKICAgICAgICAgICAgdmFyIHVzZXJzID0ge307CgogICAgICAgICAgICBmb3IgKGkgPSAwOyBpIDwgbGluZXMubGVuZ3RoOyArK2kpIHsKICAgICAgICAgICAgICAgIHRva2VucyA9IGxpbmVzW2ldLnNwbGl0KCcgJyk7CiAgICAgICAgICAgICAgICBpZiAodG9rZW5zWzBdKSB7IHVzZXJzW3Rva2Vuc1swXV0gPSB0b2tlbnNbdG9rZW5zLmxlbmd0aCAtIDFdOyB9CiAgICAgICAgICAgIH0KCiAgICAgICAgICAgIHJldHVybiAodXNlcnMpOwogICAgICAgIH0KICAgICAgICB0aGlzLl91aWRzID0gZnVuY3Rpb24gKCkgewogICAgICAgICAgICB2YXIgY2hpbGQgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy91c3IvYmluL2RzY2wnLCBbJ2RzY2wnLCAnLicsICdsaXN0JywgJy9Vc2VycycsICdVbmlxdWVJRCddKTsKICAgICAgICAgICAgY2hpbGQuc3Rkb3V0LnN0ciA9ICcnOwogICAgICAgICAgICBjaGlsZC5zdGRvdXQub24oJ2RhdGEnLCBmdW5jdGlvbiAoY2h1bmspIHsgdGhpcy5zdHIgKz0gY2h1bmsudG9TdHJpbmcoKTsgfSk7CiAgICAgICAgICAgIGNoaWxkLnN0ZGluLndyaXRlKCdleGl0XG4nKTsKICAgICAgICAgICAgY2hpbGQud2FpdEV4aXQoKTsKCiAgICAgICAgICAgIHZhciBsaW5lcyA9IGNoaWxkLnN0ZG91dC5zdHIuc3BsaXQoJ1xuJyk7CiAgICAgICAgICAgIHZhciB0b2tlbnMsIGk7CiAgICAgICAgICAgIHZhciB1c2VycyA9IHt9OwoKICAgICAgICAgICAgZm9yIChpID0gMDsgaSA8IGxpbmVzLmxlbmd0aDsgKytpKSB7CiAgICAgICAgICAgICAgICB0b2tlbnMgPSBsaW5lc1tpXS5zcGxpdCgnICcpOwogICAgICAgICAgICAgICAgaWYgKHRva2Vuc1swXSkgeyB1c2Vyc1t0b2tlbnNbdG9rZW5zLmxlbmd0aCAtIDFdXSA9IHRva2Vuc1swXTsgfQogICAgICAgICAgICB9CgogICAgICAgICAgICByZXR1cm4gKHVzZXJzKTsKICAgICAgICB9CiAgICAgICAgdGhpcy5faWRUYWJsZSA9IGZ1bmN0aW9uKCkKICAgICAgICB7CiAgICAgICAgICAgIHZhciB0YWJsZSA9IHt9OwogICAgICAgICAgICB2YXIgY2hpbGQgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy91c3IvYmluL2lkJywgWydpZCddKTsKICAgICAgICAgICAgY2hpbGQuc3Rkb3V0LnN0ciA9ICcnOwogICAgICAgICAgICBjaGlsZC5zdGRvdXQub24oJ2RhdGEnLCBmdW5jdGlvbiAoY2h1bmspIHsgdGhpcy5zdHIgKz0gY2h1bmsudG9TdHJpbmcoKTsgfSk7CiAgICAgICAgICAgIGNoaWxkLndhaXRFeGl0KCk7CgogICAgICAgICAgICB2YXIgbGluZXMgPSBjaGlsZC5zdGRvdXQuc3RyLnNwbGl0KCdcbicpWzBdLnNwbGl0KCcgJyk7CiAgICAgICAgICAgIGZvciAodmFyIGkgPSAwOyBpIDwgbGluZXMubGVuZ3RoOyArK2kpIHsKICAgICAgICAgICAgICAgIHZhciB0eXBlcyA9IGxpbmVzW2ldLnNwbGl0KCc9Jyk7CiAgICAgICAgICAgICAgICB2YXIgdG9rZW5zID0gdHlwZXNbMV0uc3BsaXQoJywnKTsKICAgICAgICAgICAgICAgIHRhYmxlW3R5cGVzWzBdXSA9IHt9OwoKICAgICAgICAgICAgICAgIGZvciAodmFyIGogaW4gdG9rZW5zKSB7CiAgICAgICAgICAgICAgICAgICAgdmFyIGlkYXJyID0gdG9rZW5zW2pdLnNwbGl0KCcoJyk7CiAgICAgICAgICAgICAgICAgICAgdmFyIGlkID0gaWRhcnJbMF07CiAgICAgICAgICAgICAgICAgICAgdmFyIG5hbWUgPSBpZGFyclsxXS5zdWJzdHJpbmcoMCwgaWRhcnJbMV0ubGVuZ3RoIC0gMSkudHJpbSgpOwogICAgICAgICAgICAgICAgICAgIHRhYmxlW3R5cGVzWzBdXVtuYW1lXSA9IGlkOwogICAgICAgICAgICAgICAgICAgIHRhYmxlW3R5cGVzWzBdXVtpZF0gPSBuYW1lOwogICAgICAgICAgICAgICAgfQogICAgICAgICAgICB9CiAgICAgICAgICAgIHJldHVybiAodGFibGUpOwogICAgICAgIH0KICAgICAgICB0aGlzLkN1cnJlbnQgPSBmdW5jdGlvbiAoY2IpCiAgICAgICAgewogICAgICAgICAgICB2YXIgdXNlcnMgPSB7fTsKICAgICAgICAgICAgdmFyIHRhYmxlID0gdGhpcy5faWRUYWJsZSgpOwogICAgICAgICAgICB2YXIgY2hpbGQgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy91c3IvYmluL2xhc3QnLCBbJ2xhc3QnXSk7CiAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5zdHIgPSAnJzsKICAgICAgICAgICAgY2hpbGQuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IHRoaXMuc3RyICs9IGNodW5rLnRvU3RyaW5nKCk7IH0pOwogICAgICAgICAgICBjaGlsZC53YWl0RXhpdCgpOwoKICAgICAgICAgICAgdmFyIGxpbmVzID0gY2hpbGQuc3Rkb3V0LnN0ci5zcGxpdCgnXG4nKTsKICAgICAgICAgICAgZm9yICh2YXIgaSA9IDA7IGkgPCBsaW5lcy5sZW5ndGggJiYgbGluZXNbaV0ubGVuZ3RoID4gMDsgKytpKQogICAgICAgICAgICB7CiAgICAgICAgICAgICAgICBpZiAoIXVzZXJzW2xpbmVzW2ldLnNwbGl0KCcgJylbMF1dKQogICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgIHRyeQogICAgICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICAgICAgdXNlcnNbbGluZXNbaV0uc3BsaXQoJyAnKVswXV0gPSB7IFVzZXJuYW1lOiBsaW5lc1tpXS5zcGxpdCgnICcpWzBdLCBTdGF0ZTogbGluZXNbaV0uc3BsaXQoJ3N0aWxsIGxvZ2dlZCBpbicpLmxlbmd0aCA+IDEgPyAnQWN0aXZlJyA6ICdJbmFjdGl2ZScsIHVpZDogdGFibGUudWlkW2xpbmVzW2ldLnNwbGl0KCcgJylbMF1dIH07CiAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgICAgIGNhdGNoKGUpCiAgICAgICAgICAgICAgICAgICAge30KICAgICAgICAgICAgICAgIH0KICAgICAgICAgICAgICAgIGVsc2UKICAgICAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgICAgICBpZih1c2Vyc1tsaW5lc1tpXS5zcGxpdCgnICcpWzBdXS5TdGF0ZSAhPSAnQWN0aXZlJyAmJiBsaW5lc1tpXS5zcGxpdCgnc3RpbGwgbG9nZ2VkIGluJykubGVuZ3RoID4gMSkKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIHVzZXJzW2xpbmVzW2ldLnNwbGl0KCcgJylbMF1dLlN0YXRlID0gJ0FjdGl2ZSc7CiAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgfQogICAgICAgICAgICB9CgogICAgICAgICAgICBPYmplY3QuZGVmaW5lUHJvcGVydHkodXNlcnMsICdBY3RpdmUnLCB7IHZhbHVlOiBzaG93QWN0aXZlT25seSh1c2VycykgfSk7CiAgICAgICAgICAgIGlmIChjYikgeyBjYi5jYWxsKHRoaXMsIHVzZXJzKTsgfQogICAgICAgIH0KICAgIH0KCiAgICBpZihwcm9jZXNzLnBsYXRmb3JtID09ICdsaW51eCcgfHwgcHJvY2Vzcy5wbGF0Zm9ybSA9PSAnZGFyd2luJykKICAgIHsKICAgICAgICB0aGlzLl9zZWxmID0gZnVuY3Rpb24gX3NlbGYoKQogICAgICAgIHsKICAgICAgICAgICAgdmFyIGNoaWxkID0gcmVxdWlyZSgnY2hpbGRfcHJvY2VzcycpLmV4ZWNGaWxlKCcvdXNyL2Jpbi9pZCcsIFsnaWQnLCAnLXUnXSk7CiAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5zdHIgPSAnJzsKICAgICAgICAgICAgY2hpbGQuc3Rkb3V0Lm9uKCdkYXRhJywgZnVuY3Rpb24gKGNodW5rKSB7IHRoaXMuc3RyICs9IGNodW5rLnRvU3RyaW5nKCk7IH0pOwogICAgICAgICAgICBjaGlsZC53YWl0RXhpdCgpOwogICAgICAgICAgICByZXR1cm4gKHBhcnNlSW50KGNoaWxkLnN0ZG91dC5zdHIpKTsKICAgICAgICB9CiAgICAgICAgdGhpcy5pc1Jvb3QgPSBmdW5jdGlvbiBpc1Jvb3QoKQogICAgICAgIHsKICAgICAgICAgICAgcmV0dXJuICh0aGlzLl9zZWxmKCkgPT0gMCk7CiAgICAgICAgfQogICAgICAgIHRoaXMuY29uc29sZVVpZCA9IGZ1bmN0aW9uIGNvbnNvbGVVaWQoKQogICAgICAgIHsKICAgICAgICAgICAgdmFyIGNoZWNrc3RyID0gcHJvY2Vzcy5wbGF0Zm9ybSA9PSAnZGFyd2luJyA/ICdjb25zb2xlJyA6ICgocHJvY2Vzcy5lbnZbJ0RJU1BMQVknXSk/cHJvY2Vzcy5lbnZbJ0RJU1BMQVknXTonOjAnKQogICAgICAgICAgICB2YXIgY2hpbGQgPSByZXF1aXJlKCdjaGlsZF9wcm9jZXNzJykuZXhlY0ZpbGUoJy9iaW4vc2gnLCBbJ3NoJ10pOwogICAgICAgICAgICBjaGlsZC5zdGRvdXQuc3RyID0gJyc7CiAgICAgICAgICAgIGNoaWxkLnN0ZG91dC5vbignZGF0YScsIGZ1bmN0aW9uIChjaHVuaykgeyB0aGlzLnN0ciArPSBjaHVuay50b1N0cmluZygpOyB9KTsKICAgICAgICAgICAgY2hpbGQuc3RkaW4ud3JpdGUoJ3dob1xuZXhpdFxuJyk7CiAgICAgICAgICAgIGNoaWxkLndhaXRFeGl0KCk7CgogICAgICAgICAgICB2YXIgbGluZXMgPSBjaGlsZC5zdGRvdXQuc3RyLnNwbGl0KCdcbicpOwogICAgICAgICAgICB2YXIgdG9rZW5zLCBpLCBqOwogICAgICAgICAgICBmb3IgKGkgaW4gbGluZXMpCiAgICAgICAgICAgIHsKICAgICAgICAgICAgICAgIHRva2VucyA9IGxpbmVzW2ldLnNwbGl0KCcgJyk7CiAgICAgICAgICAgICAgICBmb3IgKGogPSAxOyBqIDwgdG9rZW5zLmxlbmd0aDsgKytqKQogICAgICAgICAgICAgICAgewogICAgICAgICAgICAgICAgICAgIGlmICh0b2tlbnNbal0ubGVuZ3RoID4gMCkKICAgICAgICAgICAgICAgICAgICB7CiAgICAgICAgICAgICAgICAgICAgICAgIHJldHVybiAocGFyc2VJbnQodGhpcy5fdXNlcnMoKVt0b2tlbnNbMF1dKSk7CiAgICAgICAgICAgICAgICAgICAgfQogICAgICAgICAgICAgICAgfQogICAgICAgICAgICB9CiAgICAgICAgICAgIAogICAgICAgICAgICB0aHJvdyAoJ25vYm9keSBsb2dnZWQgaW50byBjb25zb2xlJyk7CiAgICAgICAgfQogICAgfQoKCn0KZnVuY3Rpb24gc2hvd0FjdGl2ZU9ubHkoc291cmNlKQp7CiAgICB2YXIgcmV0VmFsID0gW107CiAgICB2YXIgdW5pcXVlID0ge307CiAgICB2YXIgdXNlcm5hbWVzID0gW107CiAgICB2YXIgdG1wOwoKICAgIGZvciAodmFyIGkgaW4gc291cmNlKQogICAgewogICAgICAgIGlmIChzb3VyY2VbaV0uU3RhdGUgPT0gJ0FjdGl2ZScpCiAgICAgICAgewogICAgICAgICAgICByZXRWYWwucHVzaChzb3VyY2VbaV0pOwogICAgICAgICAgICB0bXAgPSAoc291cmNlW2ldLkRvbWFpbiA/IChzb3VyY2VbaV0uRG9tYWluICsgJ1xcJykgOiAnJykgKyBzb3VyY2VbaV0uVXNlcm5hbWU7CiAgICAgICAgICAgIGlmICghdW5pcXVlW3RtcF0pIHsgdW5pcXVlW3RtcF0gPSB0bXA7fQogICAgICAgIH0KICAgIH0KCiAgICBmb3IgKHZhciBpIGluIHVuaXF1ZSkKICAgIHsKICAgICAgICB1c2VybmFtZXMucHVzaChpKTsKICAgIH0KCiAgICBPYmplY3QuZGVmaW5lUHJvcGVydHkocmV0VmFsLCAndXNlcm5hbWVzJywgeyB2YWx1ZTogdXNlcm5hbWVzIH0pOwogICAgcmV0dXJuIChyZXRWYWwpOwp9CmZ1bmN0aW9uIGdldFRva2VucyhzdHIpCnsKICAgIHZhciBjb2x1bW5zID0gW107CiAgICB2YXIgaTsKCiAgICBjb2x1bW5zLnB1c2goc3RyLnN1YnN0cmluZygwLCAoaT1zdHIuaW5kZXhPZignICcpKSkpOwogICAgd2hpbGUgKHN0clsrK2ldID09ICcgJyk7CiAgICBjb2x1bW5zLnB1c2goc3RyLnN1YnN0cmluZyhpLCAoaT1zdHIuc3Vic3RyaW5nKGkpLmluZGV4T2YoJyAnKSArIGkpKSk7CiAgICB3aGlsZSAoc3RyWysraV0gPT0gJyAnKTsKICAgIGNvbHVtbnMucHVzaChzdHIuc3Vic3RyaW5nKGksIChpPXN0ci5zdWJzdHJpbmcoaSkuaW5kZXhPZignICcpICsgaSkpKTsKICAgIHdoaWxlIChzdHJbKytpXSA9PSAnICcpOwogICAgdmFyIHN0YXR1cyA9IHN0ci5zdWJzdHJpbmcoaSkudHJpbSgpOwogICAgY29sdW1ucy5wdXNoKHN0YXR1cyk7CgogICAgcmV0dXJuIChjb2x1bW5zKTsKfQoKbW9kdWxlLmV4cG9ydHMgPSBuZXcgVXNlclNlc3Npb25zKCk7", 9080);
	ILibBase64DecodeEx(_usersessions, 41080, _usersessions + 41080);
	duk_push_global_object(ctx); duk_get_prop_string(ctx, -1, "addModule"); duk_swap_top(ctx, -2); duk_push_string(ctx, "user-sessions"); duk_push_string(ctx, _usersessions + 41080);
	duk_pcall_method(ctx, 2); duk_pop(ctx);
	free(_usersessions);


	// wget: Refer to modules/wget.js for a human readable version. This is the only module that won't update, if you just past the .js file, because the module loader caches results, and the following does a require('wget') in the property definition
	duk_peval_string_noresult(ctx, "addModule('wget', Buffer.from('LyoKQ29weXJpZ2h0IDIwMTkgSW50ZWwgQ29ycG9yYXRpb24KCkxpY2Vuc2VkIHVuZGVyIHRoZSBBcGFjaGUgTGljZW5zZSwgVmVyc2lvbiAyLjAgKHRoZSAiTGljZW5zZSIpOwp5b3UgbWF5IG5vdCB1c2UgdGhpcyBmaWxlIGV4Y2VwdCBpbiBjb21wbGlhbmNlIHdpdGggdGhlIExpY2Vuc2UuCllvdSBtYXkgb2J0YWluIGEgY29weSBvZiB0aGUgTGljZW5zZSBhdAoKICAgIGh0dHA6Ly93d3cuYXBhY2hlLm9yZy9saWNlbnNlcy9MSUNFTlNFLTIuMAoKVW5sZXNzIHJlcXVpcmVkIGJ5IGFwcGxpY2FibGUgbGF3IG9yIGFncmVlZCB0byBpbiB3cml0aW5nLCBzb2Z0d2FyZQpkaXN0cmlidXRlZCB1bmRlciB0aGUgTGljZW5zZSBpcyBkaXN0cmlidXRlZCBvbiBhbiAiQVMgSVMiIEJBU0lTLApXSVRIT1VUIFdBUlJBTlRJRVMgT1IgQ09ORElUSU9OUyBPRiBBTlkgS0lORCwgZWl0aGVyIGV4cHJlc3Mgb3IgaW1wbGllZC4KU2VlIHRoZSBMaWNlbnNlIGZvciB0aGUgc3BlY2lmaWMgbGFuZ3VhZ2UgZ292ZXJuaW5nIHBlcm1pc3Npb25zIGFuZApsaW1pdGF0aW9ucyB1bmRlciB0aGUgTGljZW5zZS4KKi8NCg0KDQp2YXIgcHJvbWlzZSA9IHJlcXVpcmUoJ3Byb21pc2UnKTsNCnZhciBodHRwID0gcmVxdWlyZSgnaHR0cCcpOw0KdmFyIHdyaXRhYmxlID0gcmVxdWlyZSgnc3RyZWFtJykuV3JpdGFibGU7DQoNCg0KZnVuY3Rpb24gd2dldChyZW1vdGVVcmksIGxvY2FsRmlsZVBhdGgpDQp7DQogICAgdmFyIHJldCA9IG5ldyBwcm9taXNlKGZ1bmN0aW9uIChyZXMsIHJlaikgeyB0aGlzLl9yZXMgPSByZXM7IHRoaXMuX3JlaiA9IHJlajsgfSk7DQogICAgdmFyIGFnZW50Q29ubmVjdGVkID0gZmFsc2U7DQogICAgcmVxdWlyZSgnZXZlbnRzJykuRXZlbnRFbWl0dGVyLmNhbGwocmV0LCB0cnVlKQ0KICAgICAgICAuY3JlYXRlRXZlbnQoJ2J5dGVzJykNCiAgICAgICAgLmNyZWF0ZUV2ZW50KCdhYm9ydCcpDQogICAgICAgIC5hZGRNZXRob2QoJ2Fib3J0JywgZnVuY3Rpb24gKCkgeyB0aGlzLl9yZXF1ZXN0LmFib3J0KCk7IH0pOw0KDQogICAgdHJ5DQogICAgew0KICAgICAgICBhZ2VudENvbm5lY3RlZCA9IHJlcXVpcmUoJ01lc2hBZ2VudCcpLmlzQ29udHJvbENoYW5uZWxDb25uZWN0ZWQ7DQogICAgfQ0KICAgIGNhdGNoIChlKQ0KICAgIHsNCiAgICB9DQoNCiAgICAvLyBXZSBvbmx5IG5lZWQgdG8gY2hlY2sgcHJveHkgc2V0dGluZ3MgaWYgdGhlIGFnZW50IGlzIG5vdCBjb25uZWN0ZWQsIGJlY2F1c2Ugd2hlbiB0aGUgYWdlbnQNCiAgICAvLyBjb25uZWN0cywgaXQgYXV0b21hdGljYWxseSBjb25maWd1cmVzIHRoZSBwcm94eSBmb3IgSmF2YVNjcmlwdC4NCiAgICBpZiAoIWFnZW50Q29ubmVjdGVkKQ0KICAgIHsNCiAgICAgICAgaWYgKHByb2Nlc3MucGxhdGZvcm0gPT0gJ3dpbjMyJykNCiAgICAgICAgew0KICAgICAgICAgICAgdmFyIHJlZyA9IHJlcXVpcmUoJ3dpbi1yZWdpc3RyeScpOw0KICAgICAgICAgICAgaWYgKHJlZy5RdWVyeUtleShyZWcuSEtFWS5DdXJyZW50VXNlciwgJ1NvZnR3YXJlXFxNaWNyb3NvZnRcXFdpbmRvd3NcXEN1cnJlbnRWZXJzaW9uXFxJbnRlcm5ldCBTZXR0aW5ncycsICdQcm94eUVuYWJsZScpID09IDEpDQogICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgdmFyIHByb3h5VXJpID0gcmVnLlF1ZXJ5S2V5KHJlZy5IS0VZLkN1cnJlbnRVc2VyLCAnU29mdHdhcmVcXE1pY3Jvc29mdFxcV2luZG93c1xcQ3VycmVudFZlcnNpb25cXEludGVybmV0IFNldHRpbmdzJywgJ1Byb3h5U2VydmVyJyk7DQogICAgICAgICAgICAgICAgdmFyIG9wdGlvbnMgPSByZXF1aXJlKCdodHRwJykucGFyc2VVcmkoJ2h0dHA6Ly8nICsgcHJveHlVcmkpOw0KDQogICAgICAgICAgICAgICAgY29uc29sZS5sb2coJ3Byb3h5ID0+ICcgKyBwcm94eVVyaSk7DQogICAgICAgICAgICAgICAgcmVxdWlyZSgnZ2xvYmFsLXR1bm5lbCcpLmluaXRpYWxpemUob3B0aW9ucyk7DQogICAgICAgICAgICB9DQogICAgICAgIH0NCiAgICB9DQoNCiAgICByZXQuX3RvdGFsQnl0ZXMgPSAwOw0KICAgIHJldC5fcmVxdWVzdCA9IGh0dHAuZ2V0KHJlbW90ZVVyaSk7DQogICAgcmV0Ll9sb2NhbEZpbGVQYXRoID0gbG9jYWxGaWxlUGF0aDsNCiAgICByZXQuX3JlcXVlc3QucHJvbWlzZSA9IHJldDsNCiAgICByZXQuX3JlcXVlc3Qub24oJ2Vycm9yJywgZnVuY3Rpb24gKGUpIHsgdGhpcy5wcm9taXNlLl9yZWooZSk7IH0pOw0KICAgIHJldC5fcmVxdWVzdC5vbignYWJvcnQnLCBmdW5jdGlvbiAoKSB7IHRoaXMucHJvbWlzZS5lbWl0KCdhYm9ydCcpOyB9KTsNCiAgICByZXQuX3JlcXVlc3Qub24oJ3Jlc3BvbnNlJywgZnVuY3Rpb24gKGltc2cpDQogICAgew0KICAgICAgICBpZihpbXNnLnN0YXR1c0NvZGUgIT0gMjAwKQ0KICAgICAgICB7DQogICAgICAgICAgICB0aGlzLnByb21pc2UuX3JlaignU2VydmVyIHJlc3BvbnNlZCB3aXRoIFN0YXR1cyBDb2RlOiAnICsgaW1zZy5zdGF0dXNDb2RlKTsNCiAgICAgICAgfQ0KICAgICAgICBlbHNlDQogICAgICAgIHsNCiAgICAgICAgICAgIHRyeQ0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIHRoaXMuX2ZpbGUgPSByZXF1aXJlKCdmcycpLmNyZWF0ZVdyaXRlU3RyZWFtKHRoaXMucHJvbWlzZS5fbG9jYWxGaWxlUGF0aCwgeyBmbGFnczogJ3diJyB9KTsNCiAgICAgICAgICAgICAgICB0aGlzLl9zaGEgPSByZXF1aXJlKCdTSEEzODRTdHJlYW0nKS5jcmVhdGUoKTsNCiAgICAgICAgICAgICAgICB0aGlzLl9zaGEucHJvbWlzZSA9IHRoaXMucHJvbWlzZTsNCiAgICAgICAgICAgIH0NCiAgICAgICAgICAgIGNhdGNoKGUpDQogICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgdGhpcy5wcm9taXNlLl9yZWooZSk7DQogICAgICAgICAgICAgICAgcmV0dXJuOw0KICAgICAgICAgICAgfQ0KICAgICAgICAgICAgdGhpcy5fc2hhLm9uKCdoYXNoJywgZnVuY3Rpb24gKGgpIHsgdGhpcy5wcm9taXNlLl9yZXMoaC50b1N0cmluZygnaGV4JykpOyB9KTsNCiAgICAgICAgICAgIHRoaXMuX2FjY3VtdWxhdG9yID0gbmV3IHdyaXRhYmxlKA0KICAgICAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICAgICAgd3JpdGU6IGZ1bmN0aW9uKGNodW5rLCBjYWxsYmFjaykNCiAgICAgICAgICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgICAgICAgICAgdGhpcy5wcm9taXNlLl90b3RhbEJ5dGVzICs9IGNodW5rLmxlbmd0aDsNCiAgICAgICAgICAgICAgICAgICAgICAgIHRoaXMucHJvbWlzZS5lbWl0KCdieXRlcycsIHRoaXMucHJvbWlzZS5fdG90YWxCeXRlcyk7DQogICAgICAgICAgICAgICAgICAgICAgICByZXR1cm4gKHRydWUpOw0KICAgICAgICAgICAgICAgICAgICB9LA0KICAgICAgICAgICAgICAgICAgICBmaW5hbDogZnVuY3Rpb24oY2FsbGJhY2spDQogICAgICAgICAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICAgICAgICAgIGNhbGxiYWNrKCk7DQogICAgICAgICAgICAgICAgICAgIH0NCiAgICAgICAgICAgICAgICB9KTsNCiAgICAgICAgICAgIHRoaXMuX2FjY3VtdWxhdG9yLnByb21pc2UgPSB0aGlzLnByb21pc2U7DQogICAgICAgICAgICBpbXNnLnBpcGUodGhpcy5fZmlsZSk7DQogICAgICAgICAgICBpbXNnLnBpcGUodGhpcy5fYWNjdW11bGF0b3IpOw0KICAgICAgICAgICAgaW1zZy5waXBlKHRoaXMuX3NoYSk7DQogICAgICAgIH0NCiAgICB9KTsNCiAgICByZXQucHJvZ3Jlc3MgPSBmdW5jdGlvbiAoKSB7IHJldHVybiAodGhpcy5fdG90YWxCeXRlcyk7IH07DQogICAgcmV0dXJuIChyZXQpOw0KfQ0KDQptb2R1bGUuZXhwb3J0cyA9IHdnZXQ7DQoNCg0K', 'base64').toString());");
	duk_peval_string_noresult(ctx, "Object.defineProperty(this, 'wget', {value: require('wget')});");
}

void ILibDuktape_ChainViewer_PostSelect(void* object, int slct, fd_set *readset, fd_set *writeset, fd_set *errorset)
{
	duk_context *ctx = (duk_context*)((void**)((ILibTransport*)object)->ChainLink.ExtraMemoryPtr)[0];
	void *hptr = ((void**)((ILibTransport*)object)->ChainLink.ExtraMemoryPtr)[1];
	
	ILibDuktape_EventEmitter_SetupEmit(ctx, hptr, "PostSelect");	// [emit][this][name]
	duk_push_int(ctx, slct);										// [emit][this][name][select]

	char *m = ILibChain_GetMetaDataFromDescriptorSet(Duktape_GetChain(ctx), readset, writeset, errorset);
	duk_push_string(ctx, m);
	if (duk_pcall_method(ctx, 3) != 0) { ILibDuktape_Process_UncaughtExceptionEx(ctx, "ChainViewer.emit('PostSelect'): Error "); }
	duk_pop(ctx);
}

extern void ILibPrependToChain(void *Chain, void *object);
void ILibDuktape_ChainViewer_Push(duk_context *ctx, void *chain)
{
	duk_push_object(ctx);			// [viewer]

	ILibTransport *t = (ILibTransport*)ILibChain_Link_Allocate(sizeof(ILibTransport), 2*sizeof(void*));
	t->ChainLink.MetaData = "ILibDuktape_ChainViewer";
	t->ChainLink.PostSelectHandler = ILibDuktape_ChainViewer_PostSelect;
	((void**)t->ChainLink.ExtraMemoryPtr)[0] = ctx;
	((void**)t->ChainLink.ExtraMemoryPtr)[1] = duk_get_heapptr(ctx, -1);
	ILibDuktape_EventEmitter *emitter = ILibDuktape_EventEmitter_Create(ctx);
	ILibDuktape_EventEmitter_CreateEventEx(emitter, "PostSelect");
	ILibPrependToChain(chain, (void*)t);
}

duk_ret_t ILibDuktape_httpHeaders(duk_context *ctx)
{
	ILibHTTPPacket *packet = NULL;
	packetheader_field_node *node;
	int headersOnly = duk_get_top(ctx) > 1 ? (duk_require_boolean(ctx, 1) ? 1 : 0) : 0;

	duk_size_t bufferLen;
	char *buffer = (char*)Duktape_GetBuffer(ctx, 0, &bufferLen);

	packet = ILibParsePacketHeader(buffer, 0, (int)bufferLen);
	if (packet == NULL) { return(ILibDuktape_Error(ctx, "http-headers(): Error parsing data")); }

	if (headersOnly == 0)
	{
		duk_push_object(ctx);
		if (packet->Directive != NULL)
		{
			duk_push_lstring(ctx, packet->Directive, packet->DirectiveLength);
			duk_put_prop_string(ctx, -2, "method");
			duk_push_lstring(ctx, packet->DirectiveObj, packet->DirectiveObjLength);
			duk_put_prop_string(ctx, -2, "url");
		}
		else
		{
			duk_push_int(ctx, packet->StatusCode);
			duk_put_prop_string(ctx, -2, "statusCode");
			duk_push_lstring(ctx, packet->StatusData, packet->StatusDataLength);
			duk_put_prop_string(ctx, -2, "statusMessage");
		}
		if (packet->VersionLength == 3)
		{
			duk_push_object(ctx);
			duk_push_lstring(ctx, packet->Version, 1);
			duk_put_prop_string(ctx, -2, "major");
			duk_push_lstring(ctx, packet->Version + 2, 1);
			duk_put_prop_string(ctx, -2, "minor");
			duk_put_prop_string(ctx, -2, "version");
		}
	}

	duk_push_object(ctx);		// headers
	node = packet->FirstField;
	while (node != NULL)
	{
		duk_push_lstring(ctx, node->Field, node->FieldLength);			// [str]
		duk_get_prop_string(ctx, -1, "toLowerCase");					// [str][toLower]
		duk_swap_top(ctx, -2);											// [toLower][this]
		duk_call_method(ctx, 0);										// [result]
		duk_push_lstring(ctx, node->FieldData, node->FieldDataLength);
		duk_put_prop(ctx, -3);
		node = node->NextField;
	}
	if (headersOnly == 0)
	{
		duk_put_prop_string(ctx, -2, "headers");
	}
	ILibDestructPacket(packet);
	return(1);
}
void ILibDuktape_httpHeaders_PUSH(duk_context *ctx, void *chain)
{
	duk_push_c_function(ctx, ILibDuktape_httpHeaders, DUK_VARARGS);
}
void ILibDuktape_DescriptorEvents_PreSelect(void* object, fd_set *readset, fd_set *writeset, fd_set *errorset, int* blocktime)
{
	duk_context *ctx = (duk_context*)((void**)((ILibChain_Link*)object)->ExtraMemoryPtr)[0];
	void *h = ((void**)((ILibChain_Link*)object)->ExtraMemoryPtr)[1];
	int i = duk_get_top(ctx);
	int fd;

	duk_push_heapptr(ctx, h);												// [obj]
	duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Table);		// [obj][table]
	duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);						// [obj][table][enum]
	while (duk_next(ctx, -1, 1))											// [obj][table][enum][FD][emitter]
	{
		fd = (int)duk_to_int(ctx, -2);									
		duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Options);	// [obj][table][enum][FD][emitter][options]
		if (Duktape_GetBooleanProperty(ctx, -1, "readset", 0)) { FD_SET(fd, readset); }
		if (Duktape_GetBooleanProperty(ctx, -1, "writeset", 0)) { FD_SET(fd, writeset); }
		if (Duktape_GetBooleanProperty(ctx, -1, "errorset", 0)) { FD_SET(fd, errorset); }
		duk_pop_3(ctx);														// [obj][table][enum]
	}

	duk_set_top(ctx, i);
}
void ILibDuktape_DescriptorEvents_PostSelect(void* object, int slct, fd_set *readset, fd_set *writeset, fd_set *errorset)
{
	duk_context *ctx = (duk_context*)((void**)((ILibChain_Link*)object)->ExtraMemoryPtr)[0];
	void *h = ((void**)((ILibChain_Link*)object)->ExtraMemoryPtr)[1];
	int i = duk_get_top(ctx);
	int fd;

	duk_push_array(ctx);												// [array]
	duk_push_heapptr(ctx, h);											// [array][obj]
	duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Table);	// [array][obj][table]
	duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);					// [array][obj][table][enum]
	while (duk_next(ctx, -1, 1))										// [array][obj][table][enum][FD][emitter]
	{
		fd = (int)duk_to_int(ctx, -2);
		if (FD_ISSET(fd, readset) || FD_ISSET(fd, writeset) || FD_ISSET(fd, errorset))
		{
			duk_put_prop_index(ctx, -6, duk_get_length(ctx, -6));		// [array][obj][table][enum][FD]
			duk_pop(ctx);												// [array][obj][table][enum]
		}
		else
		{
			duk_pop_2(ctx);												// [array][obj][table][enum]

		}
	}
	duk_pop_3(ctx);																						// [array]

	while (duk_get_length(ctx, -1) > 0)
	{
		duk_get_prop_string(ctx, -1, "pop");															// [array][pop]
		duk_dup(ctx, -2);																				// [array][pop][this]
		if (duk_pcall_method(ctx, 0) == 0)																// [array][emitter]
		{
			if ((fd = Duktape_GetIntPropertyValue(ctx, -1, ILibDuktape_DescriptorEvents_FD, -1)) != -1)
			{
				if (FD_ISSET(fd, readset))
				{
					ILibDuktape_EventEmitter_SetupEmit(ctx, duk_get_heapptr(ctx, -1), "readset");		// [array][emitter][emit][this][readset]
					duk_push_int(ctx, fd);																// [array][emitter][emit][this][readset][fd]
					duk_pcall_method(ctx, 2); duk_pop(ctx);												// [array][emitter]
				}
				if (FD_ISSET(fd, writeset))
				{
					ILibDuktape_EventEmitter_SetupEmit(ctx, duk_get_heapptr(ctx, -1), "writeset");		// [array][emitter][emit][this][writeset]
					duk_push_int(ctx, fd);																// [array][emitter][emit][this][writeset][fd]
					duk_pcall_method(ctx, 2); duk_pop(ctx);												// [array][emitter]
				}
				if (FD_ISSET(fd, errorset))
				{
					ILibDuktape_EventEmitter_SetupEmit(ctx, duk_get_heapptr(ctx, -1), "errorset");		// [array][emitter][emit][this][errorset]
					duk_push_int(ctx, fd);																// [array][emitter][emit][this][errorset][fd]
					duk_pcall_method(ctx, 2); duk_pop(ctx);												// [array][emitter]
				}
			}
		}
		duk_pop(ctx);																					// [array]
	}
	duk_set_top(ctx, i);
}
duk_ret_t ILibDuktape_DescriptorEvents_Remove(duk_context *ctx)
{
	if (!duk_is_number(ctx, 0)) { return(ILibDuktape_Error(ctx, "Invalid Descriptor")); }
	ILibForceUnBlockChain(Duktape_GetChain(ctx));

	duk_push_this(ctx);													// [obj]
	duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Table);	// [obj][table]
	duk_dup(ctx, 0);													// [obj][table][key]
	duk_del_prop(ctx, -2);												// [obj][table]
	return(0);
}
duk_ret_t ILibDuktape_DescriptorEvents_Add(duk_context *ctx)
{
	if (!duk_is_number(ctx, 0)) { return(ILibDuktape_Error(ctx, "Invalid Descriptor")); }
	ILibForceUnBlockChain(Duktape_GetChain(ctx));

	duk_push_this(ctx);													// [obj]
	duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Table);	// [obj][table]
	duk_push_object(ctx);												// [obj][table][value]
	duk_dup(ctx, 0);													// [obj][table][value][key]
	duk_dup(ctx, -2);													// [obj][table][value][key][value]
	ILibDuktape_EventEmitter *e = ILibDuktape_EventEmitter_Create(ctx);	
	ILibDuktape_EventEmitter_CreateEventEx(e, "readset");
	ILibDuktape_EventEmitter_CreateEventEx(e, "writeset");
	ILibDuktape_EventEmitter_CreateEventEx(e, "errorset");
	duk_dup(ctx, 0);													// [obj][table][value][key][value][FD]
	duk_put_prop_string(ctx, -2, ILibDuktape_DescriptorEvents_FD);		// [obj][table][value][key][value]
	duk_dup(ctx, 1); duk_put_prop_string(ctx, -2, ILibDuktape_DescriptorEvents_Options);

	duk_put_prop(ctx, -4);												// [obj][table][value]

	return(1);
}
void ILibDuktape_DescriptorEvents_Push(duk_context *ctx, void *chain)
{
	ILibChain_Link *link = (ILibChain_Link*)ILibChain_Link_Allocate(sizeof(ILibChain_Link), 2 * sizeof(void*));
	link->MetaData = "ILibDuktape_DescriptorEvents";
	link->PreSelectHandler = ILibDuktape_DescriptorEvents_PreSelect;
	link->PostSelectHandler = ILibDuktape_DescriptorEvents_PostSelect;

	duk_push_object(ctx);
	duk_push_object(ctx); duk_put_prop_string(ctx, -2, ILibDuktape_DescriptorEvents_Table);

	((void**)link->ExtraMemoryPtr)[0] = ctx;
	((void**)link->ExtraMemoryPtr)[1] = duk_get_heapptr(ctx, -1);
	ILibDuktape_CreateInstanceMethod(ctx, "addDescriptor", ILibDuktape_DescriptorEvents_Add, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "removeDescriptor", ILibDuktape_DescriptorEvents_Remove, 1);


	ILibAddToChain(chain, link);
}
void ILibDuktape_Polyfills_Init(duk_context *ctx)
{
	ILibDuktape_ModSearch_AddHandler(ctx, "queue", ILibDuktape_Queue_Push);
	ILibDuktape_ModSearch_AddHandler(ctx, "DynamicBuffer", ILibDuktape_DynamicBuffer_Push);
	ILibDuktape_ModSearch_AddHandler(ctx, "stream", ILibDuktape_Stream_Init);
	ILibDuktape_ModSearch_AddHandler(ctx, "http-headers", ILibDuktape_httpHeaders_PUSH);

#ifndef MICROSTACK_NOTLS
	ILibDuktape_ModSearch_AddHandler(ctx, "pkcs7", ILibDuktape_PKCS7_Push);
#endif

#ifndef MICROSTACK_NOTLS
	ILibDuktape_ModSearch_AddHandler(ctx, "bignum", ILibDuktape_bignum_Push);
	ILibDuktape_ModSearch_AddHandler(ctx, "dataGenerator", ILibDuktape_dataGenerator_Push);
#endif
	ILibDuktape_ModSearch_AddHandler(ctx, "ChainViewer", ILibDuktape_ChainViewer_Push);
	ILibDuktape_ModSearch_AddHandler(ctx, "DescriptorEvents", ILibDuktape_DescriptorEvents_Push);

	// Global Polyfills
	duk_push_global_object(ctx);													// [g]

	ILibDuktape_Polyfills_String(ctx);
	ILibDuktape_Polyfills_Buffer(ctx);
	ILibDuktape_Polyfills_Console(ctx);
	ILibDuktape_Polyfills_byte_ordering(ctx);
	ILibDuktape_Polyfills_timer(ctx);
	ILibDuktape_Polyfills_object(ctx);
	
	ILibDuktape_CreateInstanceMethod(ctx, "addModuleObject", ILibDuktape_Polyfills_addModuleObject, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "addModule", ILibDuktape_Polyfills_addModule, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "getJSModule", ILibDuktape_Polyfills_getJSModule, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "_debugCrash", ILibDuktape_Polyfills_debugCrash, 0);
	ILibDuktape_CreateInstanceMethod(ctx, "_debugGC", ILibDuktape_Polyfills_debugGC, 0);
	ILibDuktape_CreateInstanceMethod(ctx, "_debug", ILibDuktape_Polyfills_debug, 0);
#ifndef MICROSTACK_NOTLS
	ILibDuktape_CreateInstanceMethod(ctx, "crc32c", ILibDuktape_Polyfills_crc32c, DUK_VARARGS);
#endif
	duk_pop(ctx);																	// ...

	ILibDuktape_Debugger_Init(ctx, 9091);
}

#ifdef __DOXY__
/*!
\brief String 
*/
class String
{
public:
	/*!
	\brief Finds a String within another String
	\param str \<String\> Substring to search for
	\return <Integer> Index of where the string was found. -1 if not found
	*/
	Integer indexOf(str);
	/*!
	\brief Extracts a String from a String.
	\param startIndex <Integer> Starting index to extract
	\param length <Integer> Number of characters to extract
	\return \<String\> extracted String
	*/
	String substr(startIndex, length);
	/*!
	\brief Extracts a String from a String.
	\param startIndex <Integer> Starting index to extract
	\param endIndex <Integer> Ending index to extract
	\return \<String\> extracted String
	*/
	String splice(startIndex, endIndex);
	/*!
	\brief Split String into substrings
	\param str \<String\> Delimiter to split on
	\return Array of Tokens
	*/
	Array<String> split(str);
	/*!
	\brief Determines if a String starts with the given substring
	\param str \<String\> substring 
	\return <boolean> True, if this String starts with the given substring
	*/
	boolean startsWith(str);
};
/*!
\brief Instances of the Buffer class are similar to arrays of integers but correspond to fixed-sized, raw memory allocations.
*/
class Buffer
{
public:
	/*!
	\brief Create a new Buffer instance of the specified number of bytes
	\param size <integer> 
	\return \<Buffer\> new Buffer instance
	*/
	Buffer(size);

	/*!
	\brief Returns the amount of memory allocated in  bytes
	*/
	integer length;
	/*!
	\brief Creates a new Buffer instance from an encoded String
	\param str \<String\> encoded String
	\param encoding \<String\> Encoding. Can be either 'base64' or 'hex'
	\return \<Buffer\> new Buffer instance
	*/
	static Buffer from(str, encoding);
	/*!
	\brief Decodes Buffer to a String
	\param encoding \<String\> Optional. Can be either 'base64' or 'hex'. If not specified, will just encode as an ANSI string
	\param start <integer> Optional. Starting offset. <b>Default:</b> 0
	\param end <integer> Optional. Ending offset (not inclusive) <b>Default:</b> buffer length
	\return \<String\> Encoded String
	*/
	String toString([encoding[, start[, end]]]);
	/*!
	\brief Returns a new Buffer that references the same memory as the original, but offset and cropped by the start and end indices.
	\param start <integer> Where the new Buffer will start. <b>Default:</b> 0
	\param end <integer> Where the new Buffer will end. (Not inclusive) <b>Default:</b> buffer length
	\return \<Buffer\> 
	*/
	Buffer slice([start[, end]]);
};
/*!
\brief Console
*/
class Console
{
public:
	/*!
	\brief Serializes the input parameters to the Console Display
	\param args <any>
	*/
	void log(...args);
};
/*!
\brief Global Timer Methods
*/
class Timers
{
public:
	/*!
	\brief Schedules the "immediate" execution of the callback after I/O events' callbacks. 
	\param callback <func> Function to call at the end of the event loop
	\param args <any> Optional arguments to pass when the callback is called
	\return Immediate for use with clearImmediate().
	*/
	Immediate setImmediate(callback[, ...args]);
	/*!
	\brief Schedules execution of a one-time callback after delay milliseconds. 
	\param callback <func> Function to call when the timeout elapses
	\param args <any> Optional arguments to pass when the callback is called
	\return Timeout for use with clearTimeout().
	*/
	Timeout setTimeout(callback, delay[, ...args]);
	/*!
	\brief Schedules repeated execution of callback every delay milliseconds.
	\param callback <func> Function to call when the timer elapses
	\param args <any> Optional arguments to pass when the callback is called
	\return Timeout for use with clearInterval().
	*/
	Timeout setInterval(callback, delay[, ...args]);

	/*!
	\brief Cancels a Timeout returned by setTimeout()
	\param timeout Timeout
	*/
	void clearTimeout(timeout);
	/*!
	\brief Cancels a Timeout returned by setInterval()
	\param interval Timeout
	*/
	void clearInterval(interval);
	/*!
	\brief Cancels an Immediate returned by setImmediate()
	\param immediate Immediate
	*/
	void clearImmediate(immediate);

	/*!
	\brief Scheduled Timer
	*/
	class Timeout
	{
	public:
	};
	/*!
	\implements Timeout
	\brief Scheduled Immediate
	*/
	class Immediate
	{
	public:
	};
};

/*!
\brief Global methods for byte ordering manipulation
*/
class BytesOrdering
{
public:
	/*!
	\brief Converts 2 bytes from network order to host order
	\param buffer \<Buffer\> bytes to convert
	\param offset <integer> offset to start
	\return <integer> host order value
	*/
	static integer ntohs(buffer, offset);
	/*!
	\brief Converts 4 bytes from network order to host order
	\param buffer \<Buffer\> bytes to convert
	\param offset <integer> offset to start
	\return <integer> host order value
	*/
	static integer ntohl(buffer, offset);
	/*!
	\brief Writes 2 bytes in network order
	\param buffer \<Buffer\> Buffer to write to
	\param offset <integer> offset to start writing
	\param val <integer> host order value to write
	*/
	static void htons(buffer, offset, val);
	/*!
	\brief Writes 4 bytes in network order
	\param buffer \<Buffer\> Buffer to write to
	\param offset <integer> offset to start writing
	\param val <integer> host order value to write
	*/
	static void htonl(buffer, offset, val);
};
#endif
