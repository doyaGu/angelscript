#if defined(_MSC_VER) && !defined(_WIN32_WCE) && !defined(__S3E__)
#include <direct.h> // For _getcwd()
#endif
#ifdef _WIN32_WCE
#include <windows.h> // For GetModuleFileName()
#endif
#if defined(__S3E__) || defined(__APPLE__) || defined(__GNUC__)
#include <unistd.h> // For getcwd()
#endif


#include "utils.h"

#ifdef _MSC_VER
#pragma warning (disable:4786)
#endif
#include <map>

using namespace std;





void Assert(asIScriptGeneric *gen)
{
	bool expr;
	if( sizeof(bool) == 1 )
		expr = gen->GetArgByte(0) ? true : false;
	else
		expr = gen->GetArgDWord(0) ? true : false;
	if( !expr )
	{
		PRINTF("--- Assert failed ---\n");
		asIScriptContext *ctx = asGetActiveContext();
		if( ctx )
		{
			const asIScriptFunction *function = ctx->GetFunction();
			if( function != 0 )
			{
				PRINTF("func: %s\n", function->GetDeclaration());
				PRINTF("mdle: %s\n", function->GetModuleName());
			}
			const char* section = 0;
			int line = ctx->GetLineNumber(0, 0, &section);
			PRINTF("sect: %s\n", section ? section : "");
			PRINTF("line: %d\n", line);
			ctx->SetException("Assert failed", false);
			PRINTF("---------------------\n");
		}
	}
}

//#define TRACK_LOCATIONS
//#define TRACK_SIZES

static int numAllocs            = 0;
static int numFrees             = 0;
static size_t currentMemAlloc   = 0;
static size_t maxMemAlloc       = 0;
static int maxNumAllocsSameTime = 0;
static asQWORD sumAllocSize     = 0;

static map<void*,size_t> memSize;
static map<void*,int> memCount;

#ifdef TRACK_SIZES
static map<size_t,int> meanSize;
#endif

#ifdef TRACK_LOCATIONS
struct loc
{
	const char *file;
	int line;

	bool operator <(const loc &other) const
	{
		if( file < other.file ) return true;
		if( file == other.file && line < other.line ) return true;
		return false;
	}
};

struct counters
{
	int allocs;
	int frees;
	int totalMemAlloced;
	int totalMemFreed;
};

static map<loc, counters> locCount;
static map<void*, loc> memAllocedFrom;
#endif

// The memory functions that the memory manager takes the memory from
static void *(*g_alloc)(size_t) = malloc;
static void  (*g_free)(void *)  = free;

void *MyAllocWithStats(size_t size, const char *file, int line)
{
	// Avoid compiler warning when variables aren't used
	UNUSED_VAR(line);
	UNUSED_VAR(file);

	// Allocate the memory
	void *ptr = g_alloc(size);
#if !defined(__psp2__) && !defined(__CELLOS_LV2__)
	// Count number of allocations made
	numAllocs++;

	// Count total amount of memory allocated
	sumAllocSize += size;

	// Update currently allocated memory
	currentMemAlloc += size;
	if( currentMemAlloc > maxMemAlloc ) maxMemAlloc = currentMemAlloc;

	// Remember the size of the memory allocated at this pointer
	memSize.insert(map<void*,size_t>::value_type(ptr,size));

	// Remember the currently allocated memory blocks, with the allocation number so that we can debug later
	memCount.insert(map<void*,int>::value_type(ptr,numAllocs));

	// Determine the maximum number of allocations at the same time
	if( numAllocs - numFrees > maxNumAllocsSameTime )
		maxNumAllocsSameTime = numAllocs - numFrees;

#ifdef TRACK_SIZES
	// Determine the mean size of the memory allocations
	map<size_t,int>::iterator i = meanSize.find(size);
	if( i != meanSize.end() )
		i->second++;
	else
		meanSize.insert(map<size_t,int>::value_type(size,1));
#endif

#ifdef TRACK_LOCATIONS
	// Count the number of allocations for each location in the library
	loc l = {file, line};
	map<loc, counters>::iterator i2 = locCount.find(l);
	if (i2 != locCount.end())
	{
		i2->second.allocs++;
		i2->second.totalMemAlloced += size;
	}
	else
	{
		counters c = { 1,0,size,0 };
		locCount.insert(map<loc, counters>::value_type(l, c));
	}

	// Remember where the allocation is from
	memAllocedFrom.insert(map<void*, loc>::value_type(ptr, l));
#endif
#endif
	return ptr;
}

void MyFreeWithStats(void *address)
{
#if !defined(__psp2__) && !defined(__CELLOS_LV2__)
	// Count the number of deallocations made
	numFrees++;

	// Remove the memory block from the list of allocated blocks
	size_t allocSize = 0;
	map<void*,size_t>::iterator i = memSize.find(address);
	if( i != memSize.end() )
	{
		// Decrease the current amount of allocated memory
		allocSize = i->second;
		currentMemAlloc -= allocSize;
		memSize.erase(i);
	}
	else
		assert(false);

	// Verify which memory we are currently removing so we know we did the allocation, and where it was allocated
	map<void*,int>::iterator i2 = memCount.find(address);
	if( i2 != memCount.end() )
	{
//		int numAlloc = i2->second;
		memCount.erase(i2);
	}
	else
		assert(false);

#ifdef TRACK_LOCATIONS
	// Find out where the allocation was from
	map<void*, loc>::iterator it = memAllocedFrom.find(address);
	if (it != memAllocedFrom.end())
	{
		map<loc, counters>::iterator i2 = locCount.find(it->second);
		if (i2 != locCount.end())
		{
			i2->second.frees++;
			i2->second.totalMemFreed += allocSize;
		}

		memAllocedFrom.erase(it);
	}
	else
		assert(false);
#endif
#endif
	// Free the actual memory
	g_free(address);
}

#ifdef AS_TEST_JIT
#include "../../../add_on/jit/jit.h"
#endif

void InstallMemoryManager()
{
#ifdef TRACK_LOCATIONS
	assert( strstr(asGetLibraryOptions(), " AS_DEBUG ") );
#endif

#ifdef AS_TEST_JIT
	// AS_TEST_POOLED_MEMORY=1 makes the memory manager take the memory from the
	// memory functions of the JIT compiler, to test them with the engine
	if( getenv("AS_TEST_POOLED_MEMORY") )
	{
		g_alloc = CJITCompiler::AllocMemory;
		g_free  = CJITCompiler::FreeMemory;
	}
#endif

	asSetGlobalMemoryFunctions((asALLOCFUNC_t)MyAllocWithStats, MyFreeWithStats);
}

void PrintAllocIndices()
{
	map<void*,int>::iterator i = memCount.begin();
	while( i != memCount.end() )
	{
		PRINTF("%d\n", i->second);
		i++;
	}
}

void PrintLocationCounters()
{
#ifdef TRACK_LOCATIONS
	// Print allocation counts per location
	map<loc, counters>::iterator i2 = locCount.begin();
	while (i2 != locCount.end())
	{
		const char *file = i2->first.file;
		int         line = i2->first.line;
		int         count = i2->second.allocs;
		int         frees = i2->second.frees;
		int         totalMem = i2->second.totalMemAlloced;
		int         totalFree = i2->second.totalMemFreed;
		PRINTF("%s (%d): %d, %d, %d, %d\n", file, line, count, count-frees, totalMem, totalMem - totalFree);
		i2++;
	}
#endif
}

void RemoveMemoryManager()
{
	asThreadCleanup();

	PrintAllocIndices();

//	assert( numAllocs == numFrees );
//	assert( currentMemAlloc == 0 );

	PRINTF("---------\n");
	PRINTF("MEMORY STATISTICS\n");
	PRINTF("number of allocations                 : %d\n", numAllocs);                   // 125744
	PRINTF("max allocated memory at any one time  : %d\n", (int)maxMemAlloc);                 // 121042
	PRINTF("max number of simultaneous allocations: %d\n", maxNumAllocsSameTime);        // 2134
	PRINTF("total amount of allocated memory      : %d\n", (int)sumAllocSize);                // 10106765
	PRINTF("medium size of allocations            : %d\n", numAllocs ? (int)sumAllocSize/numAllocs : 0);

#ifdef TRACK_SIZES
	// Find the mean size of allocations
	map<size_t,int>::iterator i = meanSize.begin();
	int n = 0;
	int meanAllocSize = 0;
	while( i != meanSize.end() )
	{
		if( n + i->second > numAllocs / 2 )
		{
			meanAllocSize = (int)i->first;
			break;
		}

		n += i->second;
		i++;
	}
	PRINTF("mean size of allocations              : %d\n", meanAllocSize);
	PRINTF("smallest allocation size              : %d\n", meanSize.begin()->first);
	PRINTF("largest allocation size               : %d\n", meanSize.rbegin()->first);
	PRINTF("number of different allocation sizes  : %d\n", meanSize.size());

	// Print allocation sizes
	i = meanSize.begin();
	while( i != meanSize.end() )
	{
		if( i->second >= 1000 )
			PRINTF("alloc size %d: %d\n", i->first, i->second);
		i++;
	}
#endif

	PrintLocationCounters();

	asResetGlobalMemoryFunctions();
}

int GetNumAllocs()
{
	return numAllocs;
}

int GetAllocedMem()
{
	return int(currentMemAlloc);
}


asDWORD ComputeCRC32(const asBYTE *buf, asUINT length)
{
	// Compute the lookup table
	asDWORD lup[256];
	for( asUINT pos = 0; pos < 256; pos++ )
	{
		asDWORD val = pos;
		for( int i = 8; i > 0; i-- )
		{
			if( val & 1 )
				val = (val >> 1) ^ 0xEDB88320;
			else
				val >>= 1;
		}
		lup[pos] = val;
	}

	// Calculate the CRC32 value
	asDWORD crc = 0xFFFFFFFF;
	for( asUINT i = 0; i < length; i++ )
	{
		crc = ((crc) >> 8) ^ lup[*buf++ ^ (crc & 0x000000FF)];
	}

	return ~crc;
}

#ifdef AS_TEST_JIT
// With JIT instructions enabled the bytecode also contains the JitEntry
// instructions, and the SUSPEND for the first line isn't removed. These are
// removed from both the actual and the expected bytecode before comparing
static void NormalizeByteCode(std::vector<asBYTE> &ops)
{
	std::vector<asBYTE> out;
	bool leading = true;
	for( size_t n = 0; n < ops.size(); n++ )
	{
		asBYTE c = ops[n];
		if( c == asBC_JitEntry || (c == asBC_SUSPEND && leading) )
			continue;
		leading = false;
		out.push_back(c);
	}
	ops = out;
}
#endif

bool ValidateByteCode(asIScriptFunction *func, asBYTE *expect)
{
	if (func == 0) return false;
	asUINT len;
	asDWORD *bc = func->GetByteCode(&len);

	std::vector<asBYTE> actual, expected;
	for( asUINT n = 0; n < len; )
	{
		asBYTE c = *(asBYTE*)(&bc[n]);
		actual.push_back(c);
		n += asBCTypeSize[asBCInfo[c].type];
	}
	for( asUINT i = 0; ; i++ )
	{
		expected.push_back(expect[i]);
		if( expect[i] == asBC_RET )
			break;
	}

#ifdef AS_TEST_JIT
	NormalizeByteCode(actual);
	NormalizeByteCode(expected);
#endif

	return actual == expected;
}

bool CompareMessages(const std::string &buffer, const char *expected)
{
#ifdef AS_TEST_JIT
	// Remove the digits following 'stream: ' from both
	std::string a = buffer, b = expected;
	for( int pass = 0; pass < 2; pass++ )
	{
		std::string &str = pass == 0 ? a : b;
		size_t pos = 0;
		while( (pos = str.find("stream: ", pos)) != std::string::npos )
		{
			pos += 8;
			size_t end = pos;
			while( end < str.size() && str[end] >= '0' && str[end] <= '9' )
				end++;
			str.erase(pos, end - pos);
		}
	}
	return a == b;
#else
	return buffer == expected;
#endif
}

string GetCurrentDir()
{
	string str;
	char buffer[1024];
#ifdef _MSC_VER
#ifdef _WIN32_WCE
    static TCHAR apppath[MAX_PATH] = TEXT("");
    if (!apppath[0])
    {
        GetModuleFileName(NULL, apppath, MAX_PATH);


        int appLen = _tcslen(apppath);

        // Look for the last backslash in the path, which would be the end
        // of the path itself and the start of the filename.  We only want
        // the path part of the exe's full-path filename
        // Safety is that we make sure not to walk off the front of the
        // array (in case the path is nothing more than a filename)
        while (appLen > 1)
        {
            if (apppath[appLen-1] == TEXT('\\'))
                break;
            appLen--;
        }

        // Terminate the string after the trailing backslash
        apppath[appLen] = TEXT('\0');
    }
#ifdef _UNICODE
    wcstombs(buffer, apppath, min(1024, wcslen(apppath)*sizeof(wchar_t)));
#else
    memcpy(buffer, apppath, min(1024, strlen(apppath)));
#endif

    str = buffer;
#elif defined(__S3E__)
	// Marmalade uses its own portable C library
	str = getcwd(buffer, (int)1024);
#elif _XBOX_VER >= 200
	// XBox 360 doesn't support the getcwd function, just use the root folder
	str = "game:\\";
#elif defined(_M_ARM)
	// TODO: How to determine current working dir on Windows Phone?
	str = ""; 
#else
	str = _getcwd(buffer, (int)1024);
#endif // _MSC_VER
#elif defined(__APPLE__) || defined(__GNUC__)
	str = getcwd(buffer, 1024);
#else
	str = "";
#endif

	// Replace backslashes for forward slashes
	size_t pos = 0;
	while( (pos = str.find("\\", pos)) != string::npos )
		str[pos] = '/';

	return str;
}

bool TestWithJitInstructions()
{
#ifdef AS_TEST_JIT
	const char *disable = getenv("AS_JIT_DISABLE");
	return !(disable && atoi(disable) == 2);
#else
	return false;
#endif
}

#ifdef AS_TEST_JIT

#include "../../../add_on/jit/jit.h"

static CJITCompiler *g_jit = 0;

// Environment variable AS_JIT_FLAGS can be used to change the JIT flags when running the tests
asIScriptEngine *CreateEngineWithJit(asDWORD version)
{
	asIScriptEngine *engine = (asCreateScriptEngine)(version);
	if( engine == 0 )
		return 0;

	// AS_JIT_DISABLE=1 runs the same tests with the VM only (but with the JIT
	// instructions in the bytecode), and AS_JIT_DISABLE=2 without the JIT
	// instructions, to tell test problems from JIT problems
	const char *disable = getenv("AS_JIT_DISABLE");
	if( disable && atoi(disable) == 2 )
		return engine;

	// The bytecode is always compiled with the JIT instructions so the test
	// expectations are the same with and without the JIT compiler attached
	engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, true);
	if( disable )
		return engine;

	if( g_jit == 0 )
	{
		asDWORD flags = 0;
		const char *env = getenv("AS_JIT_FLAGS");
		if( env )
			flags = asDWORD(strtoul(env, 0, 0));
		g_jit = new CJITCompiler(flags);
		const char *filter = getenv("AS_JIT_LOG_FILTER");
		if( filter )
			g_jit->SetLogFile(stdout, filter);

		// AS_JIT_THRESHOLDS=calls[,iterations] compiles the functions when they have been
		// called that many times, or when a loop has run that many iterations
		const char *thresholds = getenv("AS_JIT_THRESHOLDS");
		if( thresholds )
		{
			char *end;
			asUINT calls = asUINT(strtoul(thresholds, &end, 0));
			asUINT iterations = *end == ',' ? asUINT(strtoul(end + 1, 0, 0)) : 0;
			g_jit->SetCompileThresholds(calls, iterations);
		}

		// AS_JIT_PROFILE=calls compiles the functions again when the calls that note
		// the classes of their objects have been made that many times, 0 never
		const char *profile = getenv("AS_JIT_PROFILE");
		if( profile )
			g_jit->SetProfileThreshold(asUINT(strtoul(profile, 0, 0)));

		// AS_JIT_AOT_OUTPUT names a directory for the C++ code of the compiled functions,
		// which the build with the CMake option AS_JIT_AOT_DIR set to it uses. The flag
		// 0x100 in AS_JIT_FLAGS then leaves the other functions to the VM
		const char *aotOutput = getenv("AS_JIT_AOT_OUTPUT");
		if( aotOutput )
			g_jit->SetAOTOutput(aotOutput);
#ifdef AS_JIT_AOT_TABLE
		// AS_JIT_AOT_EVERY=n only adds every nth function, starting with the one at
		// AS_JIT_AOT_OFFSET, so that the JIT compiles the others, which then call the
		// functions generated ahead of time and are called by them
		const char *every  = getenv("AS_JIT_AOT_EVERY");
		const char *offset = getenv("AS_JIT_AOT_OFFSET");
		asUINT step  = every && atoi(every) > 0 ? asUINT(atoi(every)) : 1;
		asUINT first = offset ? asUINT(atoi(offset)) : 0;
		std::vector<SJITAOTFunction> aotFuncs;
		for( asUINT n = first; n < g_jitAOTFunctionCount; n += step )
			aotFuncs.push_back(g_jitAOTFunctions[n]);
		g_jit->AddAOTFunctions(aotFuncs.empty() ? 0 : &aotFuncs[0], asUINT(aotFuncs.size()));
#endif

		// AS_JIT_BAIL_OPS lists bytecode instruction names, separated by commas,
		// that must always be executed by the VM. Used to bisect JIT problems
		const char *bailOps = getenv("AS_JIT_BAIL_OPS");
		if( bailOps )
		{
			std::vector<asEBCInstr> ops;
			std::string list = bailOps;
			size_t start = 0;
			while( start < list.size() )
			{
				size_t end = list.find(',', start);
				if( end == std::string::npos ) end = list.size();
				std::string name = list.substr(start, end - start);
				for( int op = 0; op < asBC_MAXBYTECODE; op++ )
					if( name == asBCInfo[op].name )
						ops.push_back(asEBCInstr(op));
				start = end + 1;
			}
			g_jit->SetBailInstructions(ops.empty() ? 0 : &ops[0], asUINT(ops.size()));
			PRINTF("JIT: %d instructions forced to the VM\n", int(ops.size()));
		}
	}

	engine->SetJITCompiler(g_jit);
	return engine;
}

// Must be called after all engines have been released
void ReleaseJitCompiler()
{
	if( g_jit )
	{
		SJITStatistics stats = g_jit->GetStatistics();
		PRINTF("JIT: %d functions compiled, %d failed, %d released, %d instructions, %d bails, %d calls inlined, %d ahead of time, %d deferred, %d recompiled, %d for line callbacks\n",
			stats.functionsCompiled, stats.functionsFailed, stats.functionsReleased, stats.instructionsCompiled, stats.instructionsBailed, stats.callsInlined, stats.functionsAOT, stats.functionsDeferred, stats.functionsRecompiled, stats.functionsForLineCallbacks);
		if( getenv("AS_JIT_AOT_OUTPUT") && g_jit->WriteAOTOutput() < 0 )
			PRINTF("JIT: the code generated ahead of time could not be written\n");
		delete g_jit;
		g_jit = 0;
	}
}

#endif
