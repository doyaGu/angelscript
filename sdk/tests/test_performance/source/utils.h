#ifndef UTILS_H
#define UTILS_H

#include "angelscript.h"

#ifdef AS_TEST_JIT
// When the test is run with --jit the engines are created with the JIT compiler attached
asIScriptEngine *CreateEngineForTest(asDWORD version = ANGELSCRIPT_VERSION);
void             ReleaseJitCompiler();
extern bool      g_useJit;
extern bool      g_jitDirectCalls;
extern bool      g_jitNoDirectCalls;
#define asCreateScriptEngine(...) CreateEngineForTest(__VA_ARGS__)
#endif
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

double GetSystemTimer();

class COutStream
{
public:
	void Callback(asSMessageInfo *msg) 
	{ 
		const char *msgType = 0;
		if( msg->type == 0 ) msgType = "Error  ";
		if( msg->type == 1 ) msgType = "Warning";
		if( msg->type == 2 ) msgType = "Info   ";

		printf("%s (%d, %d) : %s : %s\n", msg->section, msg->row, msg->col, msgType, msg->message);
	}
};

#endif

