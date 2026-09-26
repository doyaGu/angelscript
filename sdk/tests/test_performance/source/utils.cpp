#include "utils.h"

#if defined(WIN32)
// Windows version

#include <math.h>

#include <windows.h>
#include <mmsystem.h>

double ticksPerSecond = 0;
bool timerInitialized = false;
bool usePerformance   = false;
double performanceBase = 0;

asQWORD GetCPUTime()
{
	HANDLE hProcess = GetCurrentProcess();
	FILETIME ftCreation, ftExit, ftKernel, ftUser;

	GetProcessTimes(hProcess, &ftCreation, &ftExit, &ftKernel, &ftUser);

	// user time and kernel time can be greater than elapsed time if for some reason multiple CPUs were used
	asQWORD ns100User = (asQWORD(ftUser.dwHighDateTime)<<32) + ftUser.dwLowDateTime;
	asQWORD ns100Kernel = (asQWORD(ftKernel.dwHighDateTime)<<32) + ftKernel.dwLowDateTime;
	return ns100User+ns100Kernel;
}

double GetSystemTimer()
{
	if( !timerInitialized )
	{
		// We need to know how often the clock is updated
		__int64 tps;
		if( !QueryPerformanceFrequency((LARGE_INTEGER *)&tps) )
			usePerformance = false;
		else
		{
			usePerformance = true;
			ticksPerSecond = (double)tps;

			__int64 ticks;
			QueryPerformanceCounter((LARGE_INTEGER *)&ticks);
			performanceBase = (double)ticks/ticksPerSecond;
		}

		timerInitialized = true;
	}

	if( usePerformance )
	{
		__int64 ticks;
		QueryPerformanceCounter((LARGE_INTEGER *)&ticks);

		double t = (double)ticks/ticksPerSecond - performanceBase;

		// We need to calibrate the performance timer as it is known to jump from time to time
		double t2 = (double)timeGetTime()/1000.0;
		if( fabs(t-t2) > 0.1 )
		{
			performanceBase += t - t2;
			t = t2;
		}

		return t;
	}
	else
        return (double)timeGetTime()/1000.0;

//	return double(asINT64(GetCPUTime()))/10000000;
}

#else
// Linux version

#include <sys/time.h>

double GetSystemTimer()
{
    struct timeval time;
    struct timezone timez;
    int rc;

    double microseconds = 0.0;

    rc = gettimeofday( &time, &timez );
    if ( 0 != rc )
    {
        // Couldn't get time of day, bail.
        printf( "Could not get time of day, exiting.\n" );
        return microseconds;
    }
    microseconds = ( time.tv_sec * 1000000 ) + time.tv_usec;
    microseconds /= 1000000;

    return microseconds;
}

#endif


#ifdef AS_TEST_JIT

#include "../../../add_on/jit/jit.h"
#include <stdlib.h>

bool g_useJit = false;
bool g_useAot = false;
bool g_jitDirectCalls = false;
bool g_jitNoDirectCalls = false;
const char *g_jitLogFilter = 0;
const char *g_aotOutput = 0;
const char *g_jitThresholds = 0;
const char *g_jitProfile = 0;
static CJITCompiler *g_jit = 0;

asIScriptEngine *CreateEngineForTest(asDWORD version)
{
	asIScriptEngine *engine = (asCreateScriptEngine)(version);
	if( engine && (g_useJit || g_useAot) )
	{
		if( g_jit == 0 )
		{
			asDWORD flags = 0;
			if( g_jitDirectCalls )   flags |= CJITCompiler::JIT_DIRECT_SYSTEM_CALLS;
			if( g_jitNoDirectCalls ) flags |= CJITCompiler::JIT_NO_DIRECT_SYSTEM_CALLS;
			if( g_jitLogFilter )     flags |= CJITCompiler::JIT_LOG;
			if( !g_useJit )          flags |= CJITCompiler::JIT_AOT_ONLY;
			g_jit = new CJITCompiler(flags);
			if( g_jitLogFilter )
				g_jit->SetLogFile(stderr, g_jitLogFilter);
			if( g_aotOutput )
				g_jit->SetAOTOutput(g_aotOutput);
			if( g_jitThresholds )
			{
				char *end;
				asUINT calls = asUINT(strtoul(g_jitThresholds, &end, 0));
				g_jit->SetCompileThresholds(calls, *end == ',' ? asUINT(strtoul(end + 1, 0, 0)) : 0);
			}
			if( g_jitProfile )
				g_jit->SetProfileThreshold(asUINT(strtoul(g_jitProfile, 0, 0)));
#ifdef AS_JIT_AOT_TABLE
			if( g_useAot )
				g_jit->AddAOTFunctions(g_jitAOTFunctions, g_jitAOTFunctionCount);
#endif
		}
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, true);
		engine->SetJITCompiler(g_jit);
	}
	return engine;
}

int PrepareForTest(asIScriptContext *ctx, asIScriptFunction *func)
{
	return g_jit ? g_jit->Prepare(ctx, func) : ctx->Prepare(func);
}

int ExecuteForTest(asIScriptContext *ctx)
{
	return g_jit ? g_jit->Execute(ctx) : ctx->Execute();
}

void UsePooledMemory()
{
	asSetGlobalMemoryFunctions(CJITCompiler::AllocMemory, CJITCompiler::FreeMemory);
}

// Must be called after all engines have been released
void ReleaseJitCompiler()
{
	if( g_jit && g_aotOutput && g_jit->WriteAOTOutput() < 0 )
		printf("Failed to write the code generated ahead of time to %s\n", g_aotOutput);
	if( g_jit && (g_jitThresholds || g_jitProfile) )
	{
		SJITStatistics stats = g_jit->GetStatistics();
		printf("JIT: %u functions compiled, %u deferred, %u recompiled\n", stats.functionsCompiled, stats.functionsDeferred, stats.functionsRecompiled);
	}
	delete g_jit;
	g_jit = 0;
}

#endif
