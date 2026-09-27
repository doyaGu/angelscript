//
// Tests for the JIT compiler add-on. The whole test suite runs with the JIT
// compiler attached when AS_TEST_JIT is defined, these tests check the
// details of the interaction between the JIT compiled code and the VM.
//

#include "utils.h"
#include "../../../add_on/jit/jit.h"
#include "../../../add_on/scriptstdstring/scriptstdstring.h"
#include "../../../add_on/scriptarray/scriptarray.h"
#include "../../../add_on/scriptmath/scriptmath.h"
#include <sstream>
#include <stdexcept>
#include <new>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstddef>

namespace TestJIT
{

static std::string g_buf;
int as_powi_test(int base, int exponent, bool &isOverflow);

static void print(const std::string &s)
{
	g_buf += s;
}

static int g_suspendCount = 0;
static int g_lineCount = 0;

static void LineCallback(asIScriptContext *ctx, void *)
{
	g_lineCount++;
	if( g_lineCount == 3 )
		ctx->Suspend();
}

// Returns the number of JitEntry instructions with a non zero argument
static int CountJitEntries(asIScriptFunction *func)
{
	asUINT length;
	asDWORD *bc = func->GetByteCode(&length);
	int count = 0;
	asUINT pos = 0;
	while( pos < length )
	{
		asEBCInstr op = asEBCInstr(*(asBYTE*)(bc + pos));
		if( op == asBC_JitEntry && asBC_PTRARG(bc + pos) != 0 )
			count++;
		pos += asBCTypeSize[asBCInfo[op].type];
	}
	return count;
}

// Runs a script function and returns the result of the execution
static int ExecuteFunction(asIScriptEngine *engine, asIScriptModule *mod, const char *decl, asIScriptContext **outCtx = 0)
{
	asIScriptFunction *func = mod->GetFunctionByDecl(decl);
	if( func == 0 )
		return -1000;
	asIScriptContext *ctx = engine->CreateContext();
	ctx->Prepare(func);
	int r = ctx->Execute();
	if( outCtx )
		*outCtx = ctx;
	else
		ctx->Release();
	return r;
}

static bool TestArithmetic(asIScriptEngine *engine)
{
	bool fail = false;
	COutStream out;
	engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);

	// Every primitive type and operator, compared against values computed by the C++ compiler
	const char *script =
		"int    gi = 0;                                                    \n"
		"uint   gu = 0;                                                    \n"
		"int64  gl = 0;                                                    \n"
		"uint64 gul = 0;                                                   \n"
		"float  gf = 0;                                                    \n"
		"double gd = 0;                                                    \n"
		"int8   gb = 0;                                                    \n"
		"int16  gw = 0;                                                    \n"
		"bool   gbool = false;                                             \n"
		"void run(int a, int b, float x, double y, int64 l, uint64 ul)     \n"
		"{                                                                 \n"
		"  gi = (a + b) * (a - b) / (b | 1) % 7 - (a << 3) + (b >> 1) + (a >>> 2) + (a & b) + (a ^ b) + ~a + -b; \n"
		"  uint ua = uint(a), ub = uint(b);                                \n"
		"  gu = (ua + ub) * (ua - ub) / (ub | 1) % 7 - (ua << 3) + (ub >> 1) + (ua & ub) + (ua ^ ub) + ~ua;      \n"
		"  gl = (l + a) * (l - b) / (l | 1) % 7 - (l << 3) + (l >> 1) + (l >>> 2) + (l & a) + (l ^ b) + ~l + -l;   \n"
		"  gul = (ul + ua) * (ul - ub) / (ul | 1) % 7 - (ul << 3) + (ul >> 1) + (ul & ua) + (ul ^ ub) + ~ul;       \n"
		"  gf = (x + a) * (x - b) / (x + 0.5f) + -x + x % 3.0f + 2.0f * x - 1.0f;                                  \n"
		"  gd = (y + a) * (y - b) / (y + 0.5) + -y + y % 3.0 + double(x) + double(a) + double(uint(b)) + double(l);\n"
		"  gb = int8(a + b); gw = int16(a * b);                            \n"
		"  gbool = (a < b) && !(x > y) || (l == 5) != (ul >= 7);          \n"
		"  gi += int(x) + int(y) + int(l) + int(uint(x)) + int(uint(y));  \n"
		"  gl += int64(x) + int64(y) + int64(uint64(x)) + int64(uint64(y)) + int64(ua) + int64(gbool ? 1 : 0);   \n"
		"  gf += float(l) + float(ul) + float(ua) + float(y);              \n"
		"  gi++; gi--; ++gi; --gi; gl++; gl--; gf++; gf--; gd++; gd--; gb++; gw--;   \n"
		"  gi = (gi % 1000) ** 2 + (a % 100) ** 3; gf = gf ** 2.0f; gd = gd ** 1.5 + gd ** 2; \n"
		"}                                                                 \n";

	asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
	mod->AddScriptSection("test", script);
	int r = mod->Build();
	if( r < 0 )
		TEST_FAILED;

	struct SArgs { int a, b; float x; double y; asINT64 l; asQWORD ul; };
	SArgs args[] =
	{
		{ 12, 5, 3.25f, -7.75, 1234567890123LL, 987654321098ULL },
		{ -100, 33, -0.75f, 12345.678, -42LL, 42ULL },
		{ 0, 0, 0.0f, 0.0, 0LL, 0ULL },
		{ 123456, -654321, 12345.5f, -1e9, 3000000000LL, 0xFFFFFFFFFFFFFFFFULL },
	};

	for( asUINT n = 0; n < sizeof(args)/sizeof(args[0]); n++ )
	{
		int a = args[n].a, b = args[n].b;
		float x = args[n].x;
		double y = args[n].y;
		asINT64 l = args[n].l;
		asQWORD ul = args[n].ul;

		// Same expressions in C++ (the VM defines the semantics through C++ too)
		asUINT ua = asUINT(a), ub = asUINT(b);
		int ei = (a + b) * (a - b) / (b | 1) % 7 - (a << 3) + int(asUINT(b) >> 1) + (a >> 2) + (a & b) + (a ^ b) + ~a + -b;
		asUINT eu = (ua + ub) * (ua - ub) / (ub | 1) % 7 - (ua << 3) + (ub >> 1) + (ua & ub) + (ua ^ ub) + ~ua;
		asINT64 el = (l + a) * (l - b) / (l | 1) % 7 - (l << 3) + asINT64(asQWORD(l) >> 1) + (l >> 2) + (l & a) + (l ^ b) + ~l + -l;
		asQWORD eul = (ul + ua) * (ul - ub) / (ul | 1) % 7 - (ul << 3) + (ul >> 1) + (ul & ua) + (ul ^ ub) + ~ul;
		float ef = (x + a) * (x - b) / (x + 0.5f) + -x + fmodf(x, 3.0f) + 2.0f * x - 1.0f;
		double ed = (y + a) * (y - b) / (y + 0.5) + -y + fmod(y, 3.0) + double(x) + double(a) + double(asUINT(b)) + double(l);
		signed char eb = (signed char)(a + b); short ew = (short)(a * b);
		bool ebool = ((a < b) && !(x > y)) || ((l == 5) != (ul >= 7));
		ei += int(x) + int(y) + int(l) + int(x < 0 ? asUINT(int(x)) : asUINT(x)) + int(y < 0 ? asUINT(int(y)) : asUINT(y));
		el += asINT64(x) + asINT64(y) + asINT64(x < 0 ? asQWORD(asINT64(x)) : asQWORD(x)) + asINT64(y < 0 ? asQWORD(asINT64(y)) : asQWORD(y)) + asINT64(ua) + asINT64(ebool ? 1 : 0);
		ef += float(l) + float(ul) + float(ua) + float(y);
		eb++; ew--;
		bool ov;
		ei = as_powi_test(ei % 1000, 2, ov) + as_powi_test(a % 100, 3, ov);
		ef = powf(ef, 2.0f); ed = pow(ed, 1.5) + pow(ed, 2);

		asIScriptFunction *func = mod->GetFunctionByDecl("void run(int, int, float, double, int64, uint64)");
		asIScriptContext *ctx = engine->CreateContext();
		ctx->Prepare(func);
		ctx->SetArgDWord(0, a);
		ctx->SetArgDWord(1, b);
		ctx->SetArgFloat(2, x);
		ctx->SetArgDouble(3, y);
		ctx->SetArgQWord(4, l);
		ctx->SetArgQWord(5, ul);
		r = ctx->Execute();
		if( r != asEXECUTION_FINISHED )
		{
			// Overflow in the pow operator is a legit exception for some inputs
			if( !(r == asEXECUTION_EXCEPTION && std::string(ctx->GetExceptionString()).find("verflow") != std::string::npos) )
			{
				if( r == asEXECUTION_EXCEPTION )
					PRINTF("case %d: exception '%s' at line %d\n", n, ctx->GetExceptionString(), ctx->GetExceptionLineNumber());
				else
					PRINTF("case %d: execution returned %d\n", n, r);
				TEST_FAILED;
			}
			ctx->Release();
			continue;
		}
		ctx->Release();

		int gi = *(int*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("gi"));
		asUINT gu = *(asUINT*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("gu"));
		asINT64 gl = *(asINT64*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("gl"));
		asQWORD gul = *(asQWORD*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("gul"));
		float gf = *(float*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("gf"));
		double gd = *(double*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("gd"));
		signed char gb = *(signed char*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("gb"));
		short gw = *(short*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("gw"));
		bool gbool = *(bool*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("gbool"));

		if( gi != ei || gu != eu || gl != el || gul != eul || gb != eb || gw != ew || gbool != ebool )
		{
			PRINTF("case %d: int %d/%d uint %u/%u int64 %lld/%lld uint64 %llu/%llu int8 %d/%d int16 %d/%d bool %d/%d\n", n,
				gi, ei, gu, eu, (long long)gl, (long long)el, (unsigned long long)gul, (unsigned long long)eul, gb, eb, gw, ew, gbool, ebool);
			TEST_FAILED;
		}
		if( !(gf == ef || (gf != gf && ef != ef) || fabsf(gf - ef) <= fabsf(ef) * 1e-5f) ||
			!(gd == ed || (gd != gd && ed != ed) || fabs(gd - ed) <= fabs(ed) * 1e-9) )
		{
			PRINTF("case %d: float %g/%g double %g/%g\n", n, gf, ef, gd, ed);
			TEST_FAILED;
		}
	}

	return fail;
}

static bool TestExceptions(asIScriptEngine *engine)
{
	bool fail = false;
	COutStream out;
	engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);

	const char *script =
		"class Obj { int v = 3; }                                          \n"
		"int divi(int a, int b)   { return a / b; }                        \n"
		"int modi(int a, int b)   { return a % b; }                        \n"
		"int64 divl(int64 a, int64 b) { return a / b; }                    \n"
		"uint divu(uint a, uint b)  { return a / b; }                      \n"
		"float divf(float a, float b) { return a / b; }                    \n"
		"double modd(double a, double b) { return a % b; }                 \n"
		"int nullobj(Obj@ o)      { return o.v; }                          \n"
		"int powi(int a, int b)   { return a ** b; }                       \n"
		"int caught(int a, int b) {                                        \n"
		"  int r = -1;                                                     \n"
		"  try { r = a / b; } catch { r = 42; }                            \n"
		"  return r;                                                       \n"
		"}                                                                 \n"
		"int nested(int b) {                                               \n"
		"  int r = 0;                                                      \n"
		"  try { r = divi(10, b) + 1; } catch { r = 77; }                  \n"
		"  return r;                                                       \n"
		"}                                                                 \n"
		// The divisor is zero in a register but not in memory
		"double divz(double a, double b) { double d = 5; d -= b; return a / d; } \n"
		"double modz(double a, double b) { double d = 5; d -= b; return a % d; } \n";

	asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
	mod->AddScriptSection("test", script);
	int r = mod->Build();
	if( r < 0 )
		TEST_FAILED;

	struct SCase { const char *decl; asQWORD a, b; const char *exception; int line; };
	SCase cases[] =
	{
		{ "int divi(int, int)",         10, 0, "Divide by zero", 2 },
		{ "int divi(int, int)",         0x80000000u, asQWORD(-1), "Overflow in integer division", 2 },
		{ "int modi(int, int)",         10, 0, "Divide by zero", 3 },
		{ "int modi(int, int)",         0x80000000u, asQWORD(-1), "Overflow in integer division", 3 },
		{ "int64 divl(int64, int64)",   10, 0, "Divide by zero", 4 },
		{ "int64 divl(int64, int64)",   0x8000000000000000ULL, asQWORD(-1), "Overflow in integer division", 4 },
		{ "uint divu(uint, uint)",      10, 0, "Divide by zero", 5 },
		{ "float divf(float, float)",   0, 0, "Divide by zero", 6 },
		{ "double modd(double, double)", 0, 0, "Divide by zero", 7 },
		{ "double divz(double, double)", 0x3FF0000000000000ULL, 0x4014000000000000ULL, "Divide by zero", 20 },
		{ "double modz(double, double)", 0x3FF0000000000000ULL, 0x4014000000000000ULL, "Divide by zero", 21 },
		{ "int nullobj(Obj@)",          0, 0, "Null pointer access", 8 },
		{ "int powi(int, int)",         1000, 1000, "Overflow in exponent operation", 9 },
	};

	for( asUINT n = 0; n < sizeof(cases)/sizeof(cases[0]); n++ )
	{
		asIScriptFunction *func = mod->GetFunctionByDecl(cases[n].decl);
		if( func == 0 ) { TEST_FAILED; continue; }
		asIScriptContext *ctx = engine->CreateContext();
		ctx->Prepare(func);
		if( func->GetParamCount() == 2 )
		{
			int typeId;
			func->GetParam(0, &typeId);
			if( typeId == asTYPEID_INT64 || typeId == asTYPEID_UINT64 || typeId == asTYPEID_DOUBLE )
			{
				ctx->SetArgQWord(0, cases[n].a);
				ctx->SetArgQWord(1, cases[n].b);
			}
			else
			{
				ctx->SetArgDWord(0, asDWORD(cases[n].a));
				ctx->SetArgDWord(1, asDWORD(cases[n].b));
			}
		}
		else
			ctx->SetArgObject(0, 0);
		r = ctx->Execute();
		if( r != asEXECUTION_EXCEPTION )
		{
			PRINTF("case %d: no exception (%d)\n", n, r);
			TEST_FAILED;
		}
		else
		{
			if( std::string(ctx->GetExceptionString()) != cases[n].exception )
			{
				PRINTF("case %d: wrong exception '%s'\n", n, ctx->GetExceptionString());
				TEST_FAILED;
			}
			if( ctx->GetExceptionLineNumber() != cases[n].line )
			{
				PRINTF("case %d: wrong line %d\n", n, ctx->GetExceptionLineNumber());
				TEST_FAILED;
			}
		}
		ctx->Release();
	}

	// try/catch within the JIT compiled function and in a caller
	{
		asIScriptFunction *func = mod->GetFunctionByDecl("int caught(int, int)");
		asIScriptContext *ctx = engine->CreateContext();
		ctx->Prepare(func);
		ctx->SetArgDWord(0, 10);
		ctx->SetArgDWord(1, 0);
		r = ctx->Execute();
		if( r != asEXECUTION_FINISHED || ctx->GetReturnDWord() != 42 )
			TEST_FAILED;
		ctx->Prepare(func);
		ctx->SetArgDWord(0, 10);
		ctx->SetArgDWord(1, 2);
		r = ctx->Execute();
		if( r != asEXECUTION_FINISHED || ctx->GetReturnDWord() != 5 )
			TEST_FAILED;

		func = mod->GetFunctionByDecl("int nested(int)");
		ctx->Prepare(func);
		ctx->SetArgDWord(0, 0);
		r = ctx->Execute();
		if( r != asEXECUTION_FINISHED || ctx->GetReturnDWord() != 77 )
			TEST_FAILED;
		ctx->Prepare(func);
		ctx->SetArgDWord(0, 5);
		r = ctx->Execute();
		if( r != asEXECUTION_FINISHED || ctx->GetReturnDWord() != 3 )
			TEST_FAILED;
		ctx->Release();
	}

	return fail;
}

static bool TestCallsAndObjects(asIScriptEngine *engine)
{
	bool fail = false;
	COutStream out;
	engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);

	RegisterStdString(engine);
	RegisterScriptArray(engine, true);
	RegisterScriptMath(engine);
	engine->RegisterGlobalFunction("void print(const string &in)", asFUNCTION(print), asCALL_CDECL);

	const char *script =
		"interface I { int get(); }                                        \n"
		"class Base : I { int v; Base(int a) { v = a; } int get() { return v; } int virt() { return 1; } } \n"
		"class Derived : Base { Derived(int a) { super(a * 2); } int virt() { return 2; } }              \n"
		"funcdef int FN(int);                                              \n"
		"int twice(int a) { return a * 2; }                                \n"
		"int fib(int n) { return n < 2 ? n : fib(n-1) + fib(n-2); }        \n"
		"int deep(int n) { return n == 0 ? 0 : 1 + deep(n - 1); }          \n"
		"string join(const array<int> &in arr) {                           \n"
		"  string s;                                                       \n"
		"  for( uint i = 0; i < arr.length(); i++ ) { if( i > 0 ) s += ','; s += formatInt(arr[i]); } \n"
		"  return s;                                                       \n"
		"}                                                                 \n"
		"int test() {                                                      \n"
		"  int r = fib(15);                            /* 610 */           \n"
		"  Base@ b = Derived(3);                                           \n"
		"  I@ i = b;                                                       \n"
		"  r += b.get() + i.get() + b.virt();          /* 6 + 6 + 2 */     \n"
		"  FN@ f = twice;                                                  \n"
		"  r += f(10);                                 /* 20 */            \n"
		"  array<int> arr = {1, 2, 3};                                     \n"
		"  print(join(arr));                                               \n"
		"  string s = 'abc' + 'def';                                       \n"
		"  r += int(s.length());                       /* 6 */             \n"
		"  r += deep(1000);                            /* 1000 */          \n"
		"  switch( r % 5 ) { case 0: r += 100; break; case 1: r += 200; break; case 4: r += 500; break; default: r += 1; } \n"
		"  int c = 0; for( int n = 0; n < 1000000; n++ ) { if( n % 3 == 0 ) c += n; else c -= 1; }  \n"
		"  r += c % 1000;                                                  \n"
		"  return r;                                                       \n"
		"}                                                                 \n";

	asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
	mod->AddScriptSection("test", script);
	int r = mod->Build();
	if( r < 0 )
		TEST_FAILED;

	// All functions must have been compiled with entry points
	if( CountJitEntries(mod->GetFunctionByDecl("int test()")) == 0 && !getenv("AS_JIT_DISABLE") )
		TEST_FAILED;

	g_buf = "";
	asIScriptContext *ctx = 0;
	r = ExecuteFunction(engine, mod, "int test()", &ctx);
	if( r != asEXECUTION_FINISHED )
	{
		if( r == asEXECUTION_EXCEPTION )
			PRINTF("exception: %s at line %d\n", ctx->GetExceptionString(), ctx->GetExceptionLineNumber());
		TEST_FAILED;
	}
	else
	{
		int expected = 610 + 14 + 20 + 6 + 1000;
		int c = 0; for( int n = 0; n < 1000000; n++ ) { if( n % 3 == 0 ) c += n; else c -= 1; }
		switch( expected % 5 ) { case 0: expected += 100; break; case 1: expected += 200; break; case 4: expected += 500; break; default: expected += 1; }
		expected += c % 1000;
		if( (int)ctx->GetReturnDWord() != expected )
		{
			PRINTF("got %d expected %d\n", ctx->GetReturnDWord(), expected);
			TEST_FAILED;
		}
		if( g_buf != "1,2,3" )
			TEST_FAILED;
	}
	if( ctx ) ctx->Release();

	return fail;
}

static bool TestSuspend(asIScriptEngine *engine)
{
	bool fail = false;
	COutStream out;
	engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);

	const char *script =
		"int counter = 0;                                                  \n"
		"int loop() {                                                      \n"
		"  int sum = 0;                                                    \n"
		"  for( int n = 0; n < 10; n++ )                                   \n"
		"  {                                                               \n"
		"    sum += n;                                                     \n"
		"    counter++;                                                    \n"
		"  }                                                               \n"
		"  return sum;                                                     \n"
		"}                                                                 \n";

	asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
	mod->AddScriptSection("test", script);
	int r = mod->Build();
	if( r < 0 )
		TEST_FAILED;

	// Suspend from a line callback in the middle of the loop, modify a
	// variable through the debug interface, and resume
	asIScriptContext *ctx = engine->CreateContext();
	ctx->SetLineCallback(asFUNCTION(LineCallback), 0, asCALL_CDECL);
	ctx->Prepare(mod->GetFunctionByDecl("int loop()"));
	g_lineCount = 0;
	r = ctx->Execute();
	if( r != asEXECUTION_SUSPENDED )
		TEST_FAILED;
	else
	{
		// Find the variable 'sum' and change it
		int sumIdx = -1;
		for( int v = 0; v < ctx->GetVarCount(); v++ )
		{
			const char *name;
			ctx->GetVar(v, 0, &name, 0);
			if( name && std::string(name) == "sum" )
				sumIdx = v;
		}
		if( sumIdx < 0 )
			TEST_FAILED;
		else
		{
			int *sum = (int*)ctx->GetAddressOfVar(sumIdx);
			*sum += 1000;
		}
		ctx->ClearLineCallback();
		r = ctx->Execute();
		if( r != asEXECUTION_FINISHED )
			TEST_FAILED;
		else if( ctx->GetReturnDWord() != 1045 )
		{
			PRINTF("got %d\n", ctx->GetReturnDWord());
			TEST_FAILED;
		}
	}
	ctx->Release();

	// Abort from the line callback
	ctx = engine->CreateContext();
	ctx->Prepare(mod->GetFunctionByDecl("int loop()"));
	struct Abort { static void Callback(asIScriptContext *ctx, void *) { ctx->Abort(); } };
	ctx->SetLineCallback(asFUNCTION(Abort::Callback), 0, asCALL_CDECL);
	r = ctx->Execute();
	if( r != asEXECUTION_ABORTED )
		TEST_FAILED;
	ctx->Release();

	// A loop on the first line of the function starts with the SUSPEND of that
	// line, which must still invoke the line callback on each iteration
	mod->AddScriptSection("spin",
		"int spins = 0;                                                    \n"
		"void spin() { do {} while( ++spins < 100 ); }                     \n");
	r = mod->Build();
	if( r < 0 )
		TEST_FAILED;
	ctx = engine->CreateContext();
	ctx->Prepare(mod->GetFunctionByDecl("void spin()"));
	int lines = 0;
	struct Count { static void Callback(asIScriptContext *, int *count) { (*count)++; } };
	ctx->SetLineCallback(asFUNCTION(Count::Callback), &lines, asCALL_CDECL);
	r = ctx->Execute();
	if( r != asEXECUTION_FINISHED || lines < 100 )
	{
		PRINTF("spin: %d line callbacks\n", lines);
		TEST_FAILED;
	}
	ctx->Release();

	return fail;
}

// Script code executed on the same context while the native code has
// arguments for a pending call on the stack
static bool TestNestedExecution(asIScriptEngine *engine)
{
	bool fail = false;
	COutStream out;
	engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);

	// The arguments are pushed from the last to the first, so the handle
	// assignment that invokes the destructor runs with 2, 3 and 4 on the stack
	const char *script =
		"int g = 0;                                                        \n"
		"class D { ~D() { int x = 7; int y = x * 3; g += x + y; } }        \n"
		"D@ h;                                                             \n"
		"int take4(int a, int b, int c, int d) { return a * 1000 + b * 100 + c * 10 + d; } \n"
		"int global() { @h = D(); return take4((@h = null) is null ? 1 : 0, 2, 3, 4); }  \n"
		"int local() { D@ l = D(); return take4((@l = null) is null ? 1 : 0, 2, 3, 4); } \n";

	asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
	mod->AddScriptSection("test", script);
	int r = mod->Build();
	if( r < 0 )
		TEST_FAILED;

	const char *decls[] = { "int global()", "int local()" };
	for( asUINT n = 0; n < 2; n++ )
	{
		asIScriptContext *ctx = 0;
		r = ExecuteFunction(engine, mod, decls[n], &ctx);
		int g = *(int*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("g"));
		if( r != asEXECUTION_FINISHED || ctx->GetReturnDWord() != 1234 || g != 28 * int(n + 1) )
		{
			PRINTF("%s: returned %d, got %d, g = %d\n", decls[n], r, r == asEXECUTION_FINISHED ? int(ctx->GetReturnDWord()) : 0, g);
			TEST_FAILED;
		}
		if( ctx ) ctx->Release();
	}

	return fail;
}

// Registered functions with various calling conventions and signatures, used
// to test the direct native calls made by the JIT compiler
namespace DirectCalls
{
	static int    g_destroyed = 0;
	static int    g_live = 0;

	// Value types returned by value, in memory or in registers depending on the ABI
	struct CVec  { float x, y, z; };
	struct CPair { int a, b; };
	typedef float CFlt; // asOBJ_APP_FLOAT is for types that are a float in C++
	struct CVal
	{
		CVal() : v(0)                 { g_live++; }
		CVal(const CVal &o) : v(o.v)  { g_live++; }
		~CVal()                       { g_live--; }
		CVal &operator=(const CVal &o) { v = o.v; return *this; }
		int v;
	};

	struct CObj
	{
		int v;
		int arr[4];
		int   Get()              { return v; }
		void  Set(int a)         { v = a; }
		float Mul(float f)       { return v * f; }
		asINT64 Big(asINT64 a, int b) { return a * b + v; }
		double Mix(double a, float b, asINT64 c, int d) { return a + b + double(c) + d + v; }
		int  &At(int i)          { return arr[i]; }
		CVec  Scale(float s)     { CVec r = { v * s, v * s, v * s }; return r; }
		CPair Pair(int b)        { CPair r = { v, b }; return r; }
		int   Throw(int a)       { if( a < 0 ) throw std::runtime_error("method"); return a + v; }
		CVal  Five(int a, double b, float c, int d) { CVal r; r.v = int(a + b + c + d) + v; return r; }
		double Four(float a, float b, double c, float d) { return a + b + c + d + v; }
		int   Many(int i1, int i2, int i3, int i4, float f1, float f2, float f3, float f4,
		           int i5, int i6, int i7, int i8, float f5, float f6, float f7, float f8)
		{
			return i1 + i2 * 2 + i3 * 3 + i4 * 4 + int(f1 + f2 * 2 + f3 * 3 + f4 * 4) +
			       i5 * 5 + i6 * 6 + i7 * 7 + i8 * 8 + int(f5 * 5 + f6 * 6 + f7 * 7 + f8 * 8) + v;
		}
		// More floating point arguments than vector registers for them with the System V x64 ABI
		float Floats(float f1, float f2, float f3, float f4, float f5, float f6, float f7, float f8, float f9, float f10)
		{
			return f1 + f2 * 2 + f3 * 3 + f4 * 4 + f5 * 5 + f6 * 6 + f7 * 7 + f8 * 8 + f9 * 9 + f10 * 10 + v;
		}
		double Doubles(double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8, double d9, double d10)
		{
			return d1 + d2 * 2 + d3 * 3 + d4 * 4 + d5 * 5 + d6 * 6 + d7 * 7 + d8 * 8 + d9 * 9 + d10 * 10 + v;
		}
		// More arguments than AsmJit supports with the object pointer, so called through the engine
		int   Sum32(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11,
		            int a12, int a13, int a14, int a15, int a16, int a17, int a18, int a19, int a20, int a21, int a22,
		            int a23, int a24, int a25, int a26, int a27, int a28, int a29, int a30, int a31)
		{
			return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 +
			       a17 + a18 + a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27 + a28 + a29 + a30 + a31 * 2 + v;
		}
	};
	static CObj g_obj;

	class CRef
	{
	public:
		CRef(int v) : refCount(1), v(v) {}
		void AddRef() { refCount++; }
		void Release() { if( --refCount == 0 ) { g_destroyed++; delete this; } }
		int  Get() { return v; }
		int refCount, v;
	};
	static CRef *Factory(int v) { return new CRef(v); }
	static CRef *MakeAndThrow(int v)
	{
		CRef *r = new CRef(v);
		asGetActiveContext()->SetException("thrown after return value");
		return r;
	}

	static int   Cdecl(int a, int b)                    { return a * 10 + b; }
	static float CdeclF(float a, float b)               { return a * b; }
	static double CdeclD(double a, asINT64 b, int c)    { return a + double(b) + c; }
	static asINT64 CdeclL(asINT64 a, asQWORD b)         { return a - asINT64(b); }
	static bool  CdeclB(bool a, asBYTE b, asWORD c)     { return a && b == 3 && c == 4; }
	static void  ByRef(int &out, const int &in)         { out = in * 2; }
	static int   ObjFirst(CObj *o, int a)               { return o->v + a; }
	static int   ObjLast(int a, CObj *o)                { return o->v - a; }
	static int   STDCALL Std(int a, int b)              { return a - b; }
	static void  Raise()                                { asGetActiveContext()->SetException("raised in registered function"); }
	static void  Susp()                                 { asGetActiveContext()->Suspend(); }
	static int   SuspRet(int a)                         { asGetActiveContext()->Suspend(); return a; }
	static int  &RefRet(CRef &r)                        { return r.v; }
	static int   RefArg(CRef &r)                        { return r.v; }
	static int   Handle(CRef *r)                        { int v = r ? r->v : -1; if( r ) r->Release(); return v; }
	static int  &AtLast(int i, CObj *o)                 { return o->arr[i]; }
	static int  &AtRaise(int i, CObj *o)                { asGetActiveContext()->SetException("raised in AtRaise"); return o->arr[i]; }
	static int  &RefAt(int, CRef *r)                    { return r->v; }
	static CVec  MakeVec(float x, float y, float z)     { CVec r = { x, y, z }; return r; }
	static CVec  VecAdd(const CVec &a, const CVec &b)   { CVec r = { a.x + b.x, a.y + b.y, a.z + b.z }; return r; }
	static CVec  VecMul(float s, const CVec &a)         { CVec r = { a.x * s, a.y * s, a.z * s }; return r; }
	static CPair MakePair(int a, int b)                 { CPair r = { a, b }; return r; }
	static CFlt  MakeFlt(float f)                       { return f + 1; }
	static CVal  MakeVal(int v)                         { CVal r; r.v = v; return r; }
	static CVal  MakeValThrow(int v)                    { CVal r; r.v = v; asGetActiveContext()->SetException("raised in MakeValThrow"); return r; }
	static void  ValConstruct(CVal *p)                  { new(p) CVal(); }
	static void  ValDestruct(CVal *p)                   { p->~CVal(); }

	static void Register(asIScriptEngine *engine)
	{
		int r;
		r = engine->RegisterObjectType("CObj", 0, asOBJ_REF | asOBJ_NOHANDLE); assert( r >= 0 );
		r = engine->RegisterObjectProperty("CObj", "int v", asOFFSET(CObj, v)); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "int Get()", asMETHOD(CObj, Get), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "void Set(int)", asMETHOD(CObj, Set), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "float Mul(float)", asMETHOD(CObj, Mul), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "int64 Big(int64, int)", asMETHOD(CObj, Big), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "double Mix(double, float, int64, int)", asMETHOD(CObj, Mix), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "int ObjFirst(int)", asFUNCTION(ObjFirst), asCALL_CDECL_OBJFIRST); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "int ObjLast(int)", asFUNCTION(ObjLast), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterGlobalProperty("CObj obj", &g_obj); assert( r >= 0 );

		r = engine->RegisterObjectType("CRef", 0, asOBJ_REF); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("CRef", asBEHAVE_FACTORY, "CRef@ f(int)", asFUNCTION(Factory), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("CRef", asBEHAVE_ADDREF, "void f()", asMETHOD(CRef, AddRef), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("CRef", asBEHAVE_RELEASE, "void f()", asMETHOD(CRef, Release), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectProperty("CRef", "int v", asOFFSET(CRef, v)); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CRef", "int Get()", asMETHOD(CRef, Get), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("CRef@ MakeAndThrow(int)", asFUNCTION(MakeAndThrow), asCALL_CDECL); assert( r >= 0 );

		r = engine->RegisterGlobalFunction("int Cdecl(int, int)", asFUNCTION(Cdecl), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("float CdeclF(float, float)", asFUNCTION(CdeclF), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("double CdeclD(double, int64, int)", asFUNCTION(CdeclD), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("int64 CdeclL(int64, uint64)", asFUNCTION(CdeclL), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("bool CdeclB(bool, uint8, uint16)", asFUNCTION(CdeclB), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("void ByRef(int &out, const int &in)", asFUNCTION(ByRef), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("int Std(int, int)", asFUNCTION(Std), asCALL_STDCALL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("void Raise()", asFUNCTION(Raise), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("void Susp()", asFUNCTION(Susp), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("int SuspRet(int)", asFUNCTION(SuspRet), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("int &RefRet(CRef &inout)", asFUNCTION(RefRet), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("int RefArg(CRef &inout)", asFUNCTION(RefArg), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("int Handle(CRef@)", asFUNCTION(Handle), asCALL_CDECL); assert( r >= 0 );

		// Methods taking an int and returning a reference are called with asBC_Thiscall1
		r = engine->RegisterObjectMethod("CObj", "int &At(int)", asMETHOD(CObj, At), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "int &AtLast(int)", asFUNCTION(AtLast), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "int &AtRaise(int)", asFUNCTION(AtRaise), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CRef", "int &At(int)", asFUNCTION(RefAt), asCALL_CDECL_OBJLAST); assert( r >= 0 );

		r = engine->RegisterObjectType("vec", sizeof(CVec), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS | asOBJ_APP_CLASS_ALLFLOATS); assert( r >= 0 );
		r = engine->RegisterObjectProperty("vec", "float x", asOFFSET(CVec, x)); assert( r >= 0 );
		r = engine->RegisterObjectProperty("vec", "float y", asOFFSET(CVec, y)); assert( r >= 0 );
		r = engine->RegisterObjectProperty("vec", "float z", asOFFSET(CVec, z)); assert( r >= 0 );
		r = engine->RegisterObjectMethod("vec", "vec opAdd(const vec &in) const", asFUNCTION(VecAdd), asCALL_CDECL_OBJFIRST); assert( r >= 0 );
		r = engine->RegisterObjectMethod("vec", "vec opMul(float) const", asFUNCTION(VecMul), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("vec MakeVec(float, float, float)", asFUNCTION(MakeVec), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "vec Scale(float)", asMETHOD(CObj, Scale), asCALL_THISCALL); assert( r >= 0 );

		r = engine->RegisterObjectType("pair", sizeof(CPair), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS | asOBJ_APP_CLASS_ALLINTS); assert( r >= 0 );
		r = engine->RegisterObjectProperty("pair", "int a", asOFFSET(CPair, a)); assert( r >= 0 );
		r = engine->RegisterObjectProperty("pair", "int b", asOFFSET(CPair, b)); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("pair MakePair(int, int)", asFUNCTION(MakePair), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "pair Pair(int)", asMETHOD(CObj, Pair), asCALL_THISCALL); assert( r >= 0 );

		r = engine->RegisterObjectType("flt", sizeof(CFlt), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_FLOAT); assert( r >= 0 );
		r = engine->RegisterObjectProperty("flt", "float f", 0); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("flt MakeFlt(float)", asFUNCTION(MakeFlt), asCALL_CDECL); assert( r >= 0 );

		r = engine->RegisterObjectType("val", sizeof(CVal), asOBJ_VALUE | asOBJ_APP_CLASS_CDAK); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("val", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(ValConstruct), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("val", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(ValDestruct), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectMethod("val", "val &opAssign(const val &in)", asMETHODPR(CVal, operator=, (const CVal &), CVal &), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectProperty("val", "int v", asOFFSET(CVal, v)); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("val MakeVal(int)", asFUNCTION(MakeVal), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("val MakeValThrow(int)", asFUNCTION(MakeValThrow), asCALL_CDECL); assert( r >= 0 );

		// Arguments passed on the machine stack, after the object pointer and the hidden return pointer
		r = engine->RegisterObjectMethod("CObj", "val Five(int, double, float, int)", asMETHOD(CObj, Five), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "double Four(float, float, double, float)", asMETHOD(CObj, Four), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "int Many(int, int, int, int, float, float, float, float, int, int, int, int, float, float, float, float)", asMETHOD(CObj, Many), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "float Floats(float, float, float, float, float, float, float, float, float, float)", asMETHOD(CObj, Floats), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "double Doubles(double, double, double, double, double, double, double, double, double, double)", asMETHOD(CObj, Doubles), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "int Sum32(int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, "
		                                         "int, int, int, int, int, int, int, int, int, int, int, int, int, int, int, int)", asMETHOD(CObj, Sum32), asCALL_THISCALL); assert( r >= 0 );
	}
}

// Registered functions called directly from the native code
static bool TestDirectCalls()
{
	bool fail = false;
	COutStream out;

	// A separate JIT compiler with direct calls enabled, it must outlive the engine
	CJITCompiler jit(CJITCompiler::JIT_DIRECT_SYSTEM_CALLS);
	asIScriptEngine *engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
	engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, true);
	engine->SetJITCompiler(&jit);
	engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);
	engine->RegisterGlobalFunction("void assert(bool)", asFUNCTION(Assert), asCALL_GENERIC);
	DirectCalls::Register(engine);

	const char *script =
		"int test()                                                    \n"
		"{                                                             \n"
		"  int r = 0;                                                  \n"
		"  assert( Cdecl(3, 4) == 34 );                                \n"
		"  assert( CdeclF(1.5f, 4) == 6 );                             \n"
		"  assert( CdeclD(0.5, 1000000000000, 7) == 1000000000007.5 ); \n"
		"  assert( CdeclL(-5, 2) == -7 );                              \n"
		"  assert( CdeclB(true, 3, 4) );                               \n"
		"  assert( !CdeclB(true, 2, 4) );                              \n"
		"  int o; ByRef(o, 21); assert( o == 42 );                     \n"
		"  assert( Std(10, 3) == 7 );                                  \n"
		"  obj.Set(5);                                                 \n"
		"  assert( obj.Get() == 5 );                                   \n"
		"  assert( obj.Mul(2.5f) == 12.5f );                           \n"
		"  assert( obj.Big(3000000000, 3) == 9000000005 );             \n"
		"  assert( obj.Mix(1.5, 2.5f, 3, 4) == 16 );                   \n"
		"  assert( obj.ObjFirst(10) == 15 );                           \n"
		"  assert( obj.ObjLast(10) == -5 );                            \n"
		"  CRef@ ref = CRef(77);                                       \n"
		"  assert( RefArg(ref) == 77 );                                \n"
		"  RefRet(ref) = 9; assert( ref.v == 9 ); ref.v = 77;          \n"
		"  assert( Handle(ref) == 77 );                                \n"
		"  assert( Handle(null) == -1 );                               \n"
		"  for( int n = 0; n < 1000; n++ ) r += Cdecl(n, 1) + obj.Get(); \n"
		"  return r;                                                   \n"
		"}                                                             \n"
		"int raise() { int a = 1; Raise(); return a; }                 \n"
		"int nullThis() { CRef @o; return o.Get(); }                   \n"
		"int suspend() { int a = 5; Susp(); a += 3; return a; }        \n"
		"int leak() { CRef@ r = MakeAndThrow(3); return r.v; }         \n"
		"int suspendArgs() { return Cdecl(SuspRet(3), 4); }            \n"
		"int values()                                                  \n"
		"{                                                             \n"
		"  obj.v = 5;                                                  \n"
		"  obj.At(1) = 11; obj.At(2) = obj.At(1) + 1;                  \n"
		"  assert( obj.At(2) == 12 && obj.AtLast(1) == 11 );           \n"
		"  vec a = MakeVec(1, 2, 3);                                   \n"
		"  vec b = a + a; assert( b.x == 2 && b.y == 4 && b.z == 6 );  \n"
		"  vec c = b * 0.5f; assert( c.x == 1 && c.z == 3 );           \n"
		"  vec d = obj.Scale(2); assert( d.x == 10 && d.z == 10 );     \n"
		"  pair p = MakePair(3, 4); assert( p.a == 3 && p.b == 4 );    \n"
		"  pair q = obj.Pair(7); assert( q.a == 5 && q.b == 7 );       \n"
		"  val w = MakeVal(8); assert( w.v == 8 );                     \n"
		"  flt f = MakeFlt(1.5f); assert( f.f == 2.5f );               \n"
		"  return obj.At(1) + obj.At(2);                               \n"
		"}                                                             \n"
		"int valThrow() { val v = MakeValThrow(3); return v.v; }       \n"
		"int atRaise() { return obj.AtRaise(1); }                      \n"
		"int nullAt() { CRef @r; return r.At(0); }                     \n"
		"int stackArgs()                                               \n"
		"{                                                             \n"
		"  obj.v = 5;                                                  \n"
		"  assert( obj.Four(1, 2, 3, 4) == 15 );                       \n"
		"  val f = obj.Five(1, 2.5, 3.5f, 4); assert( f.v == 16 );     \n"
		"  assert( obj.Sum32(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3) == 42 ); \n"
		"  assert( obj.Floats(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 390 );  \n"
		"  assert( obj.Doubles(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 390 ); \n"
		"  return obj.Many(1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4); \n"
		"}                                                             \n";

	asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
	mod->AddScriptSection("test", script);
	int r = mod->Build();
	if( r < 0 )
		TEST_FAILED;

	asIScriptContext *ctx = engine->CreateContext();
	ctx->Prepare(mod->GetFunctionByDecl("int test()"));
	r = ctx->Execute();
	if( r != asEXECUTION_FINISHED )
	{
		if( r == asEXECUTION_EXCEPTION )
			PRINTF("exception: %s at line %d\n", ctx->GetExceptionString(), ctx->GetExceptionLineNumber());
		TEST_FAILED;
	}
	else
	{
		int expected = 0;
		for( int n = 0; n < 1000; n++ ) expected += n * 10 + 1 + 5;
		if( (int)ctx->GetReturnDWord() != expected )
			TEST_FAILED;
	}

	// Script exception raised by the registered function
	ctx->Prepare(mod->GetFunctionByDecl("int raise()"));
	r = ctx->Execute();
	if( r != asEXECUTION_EXCEPTION || std::string(ctx->GetExceptionString()) != "raised in registered function" || ctx->GetExceptionLineNumber() != 27 )
		TEST_FAILED;

	// Null this pointer
	ctx->Prepare(mod->GetFunctionByDecl("int nullThis()"));
	r = ctx->Execute();
	if( r != asEXECUTION_EXCEPTION || std::string(ctx->GetExceptionString()) != "Null pointer access" || ctx->GetExceptionLineNumber() != 28 )
		TEST_FAILED;

	// Suspend from within the registered function, then resume
	ctx->Prepare(mod->GetFunctionByDecl("int suspend()"));
	r = ctx->Execute();
	if( r != asEXECUTION_SUSPENDED )
		TEST_FAILED;
	r = ctx->Execute();
	if( r != asEXECUTION_FINISHED || ctx->GetReturnDWord() != 8 )
		TEST_FAILED;

	// A returned handle must be released when the function also raised an exception
	DirectCalls::g_destroyed = 0;
	ctx->Prepare(mod->GetFunctionByDecl("int leak()"));
	r = ctx->Execute();
	if( r != asEXECUTION_EXCEPTION || DirectCalls::g_destroyed != 1 )
		TEST_FAILED;

	// Suspend while the second argument of the outer call is already on the stack
	ctx->Prepare(mod->GetFunctionByDecl("int suspendArgs()"));
	r = ctx->Execute();
	if( r != asEXECUTION_SUSPENDED )
		TEST_FAILED;
	r = ctx->Execute();
	if( r != asEXECUTION_FINISHED || ctx->GetReturnDWord() != 34 )
	{
		PRINTF("suspendArgs: returned %d, got %d\n", r, r == asEXECUTION_FINISHED ? int(ctx->GetReturnDWord()) : 0);
		TEST_FAILED;
	}

	// asBC_Thiscall1, and value types returned by value
	DirectCalls::g_live = 0;
	ctx->Prepare(mod->GetFunctionByDecl("int values()"));
	r = ctx->Execute();
	if( r != asEXECUTION_FINISHED || ctx->GetReturnDWord() != 23 )
	{
		if( r == asEXECUTION_EXCEPTION )
			PRINTF("values: exception: %s at line %d\n", ctx->GetExceptionString(), ctx->GetExceptionLineNumber());
		TEST_FAILED;
	}
	ctx->Unprepare();
	if( DirectCalls::g_live != 0 )
		TEST_FAILED;

	// A value returned on the stack must be destroyed when the function also raised an exception
	ctx->Prepare(mod->GetFunctionByDecl("int valThrow()"));
	r = ctx->Execute();
	if( r != asEXECUTION_EXCEPTION || std::string(ctx->GetExceptionString()) != "raised in MakeValThrow" || ctx->GetExceptionLineNumber() != 47 )
		TEST_FAILED;
	ctx->Unprepare();
	if( DirectCalls::g_live != 0 )
		TEST_FAILED;

	ctx->Prepare(mod->GetFunctionByDecl("int atRaise()"));
	r = ctx->Execute();
	if( r != asEXECUTION_EXCEPTION || std::string(ctx->GetExceptionString()) != "raised in AtRaise" || ctx->GetExceptionLineNumber() != 48 )
		TEST_FAILED;

	ctx->Prepare(mod->GetFunctionByDecl("int nullAt()"));
	r = ctx->Execute();
	if( r != asEXECUTION_EXCEPTION || std::string(ctx->GetExceptionString()) != "Null pointer access" || ctx->GetExceptionLineNumber() != 49 )
		TEST_FAILED;

	ctx->Prepare(mod->GetFunctionByDecl("int stackArgs()"));
	r = ctx->Execute();
	if( r != asEXECUTION_FINISHED || ctx->GetReturnDWord() != 205 )
	{
		if( r == asEXECUTION_EXCEPTION )
			PRINTF("stackArgs: exception: %s at line %d\n", ctx->GetExceptionString(), ctx->GetExceptionLineNumber());
		TEST_FAILED;
	}
	ctx->Unprepare();
	if( DirectCalls::g_live != 0 )
		TEST_FAILED;

	ctx->Release();
	engine->ShutDownAndRelease();

	// A function that fails to compile would silently be left to the VM
	SJITStatistics stats = jit.GetStatistics();
	if( stats.functionsCompiled == 0 || stats.functionsFailed != 0 )
	{
		PRINTF("direct calls: %u functions compiled, %u failed\n", stats.functionsCompiled, stats.functionsFailed);
		TEST_FAILED;
	}
	return fail;
}

// Script-to-script calls made natively by the JIT compiled code. The same
// scripts are executed without the JIT compiler, and everything that the
// application can observe must be the same
namespace NativeCalls
{
	static std::stringstream g_trace;
	static int g_lines = 0;

	// Records the call stack and the variables named 'local'
	static int Inspect()
	{
		asIScriptContext *ctx = asGetActiveContext();
		for( asUINT l = 0; l < ctx->GetCallstackSize(); l++ )
		{
			g_trace << ctx->GetFunction(l)->GetName() << ":" << ctx->GetLineNumber(l);
			for( int v = 0; v < ctx->GetVarCount(l); v++ )
			{
				const char *name;
				ctx->GetVar(v, l, &name);
				if( name && std::string(name) == "local" )
					g_trace << "=" << *(int*)ctx->GetAddressOfVar(v, l);
			}
			g_trace << " ";
		}
		return 0;
	}

	static void OnException(asIScriptContext *ctx, void *)
	{
		g_trace << "[" << ctx->GetExceptionString() << " in " << ctx->GetExceptionFunction()->GetName() << ":" <<
		           ctx->GetExceptionLineNumber() << " depth " << ctx->GetCallstackSize() << "] ";
	}

	static void CountLines(asIScriptContext *, void *)
	{
		g_lines++;
	}

	static void SuspendInLeaf(asIScriptContext *ctx, void *)
	{
		g_lines++;
		std::string name = ctx->GetFunction()->GetName();
		if( name == "leaf" || name == "S" )
			ctx->Suspend();
	}

	// The compiler merges the line cues of a statement and a following declaration
	// or block only without the JIT instructions, so the functions that are
	// executed with a line callback avoid those
	static const char *script =
		"interface I { int get(int); }                                                     \n"
		"class A : I { int v; A(int a) { v = a; } int get(int a) { return v + a; } int virt(int a) { return a + 1; } } \n"
		"class B : A { B(int a) { super(a * 2); } int virt(int a) { return a + 2; } }      \n"
		"funcdef int FN(int);                                                              \n"
		"int dtors = 0;                                                                    \n"
		"class D { int v; D(int a) { v = a; } ~D() { dtors += v; } }                       \n"
		"import int imported(int) from 'other';                                            \n"
		"import int unbound(int) from 'other';                                             \n"
		"int twice(int a) { return a * 2; }                                                \n"
		"int deep(int n) { return n == 0 ? 0 : 1 + deep(n - 1); }                          \n"
		"int probe(int n) {                                                                \n"
		"  int local = n * 10;                                                             \n"
		"  if( n == 0 )                                                                    \n"
		"    return inspect();                                                             \n"
		"  return probe(n - 1) + local;                                                    \n"
		"}                                                                                 \n"
		"int throwAt(int n) { D d(1); if( n == 0 ) { int z = 0; return 1 / z; } return throwAt(n - 1) + 1; } \n"
		"int catchAt(int n) { int r = 0; try { r = throwAt(n); } catch { r = -1; } return r; } \n"
		"int objects(int n) { D d(n); A@ a = B(n); return n == 0 ? a.get(0) : objects(n - 1) + a.virt(n); } \n"
		"int step(A@ a, I@ i, FN@ f, FN@ dg, int n) { return i.get(n) + a.virt(n) + f(n) + dg(n) + imported(n); } \n"
		"int dispatch() {                                                                  \n"
		"  A@ a = B(3); I@ i = a;                                                          \n"
		"  FN@ f = twice; FN@ dg = FN(a.virt);                                             \n"
		"  int r = 0, n = 0;                                                               \n"
		"  while( n < 100 )                                                                \n"
		"    r += step(a, i, f, dg, n++);                                                  \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		"int blocks(int m) { int r = 0; for( int n = 0; n < m; n++ ) r += deep(n % 50) + objects(n % 7); return r; } \n"
		"int nullIntf() { I@ i; return i.get(1); }                                         \n"
		"int nullFunc() { FN@ f; return f(1); }                                            \n"
		"int callUnbound() { return unbound(1); }                                          \n"
		"int leaf(int a) { return a + 1; }                                                 \n"
		"int suspended(int n) { int r = 0, i = 0; while( i < n ) r += leaf(i++); return r; } \n"
		// The objects of garbage collected classes are destroyed by the allocation of
		// new ones, which runs their destructors in the context of the allocation
		"class G {                                                                         \n"
		"  G@ next;                                                                        \n"
		"  int v;                                                                          \n"
		"  G(int a) {                                                                      \n"
		"    v = a;                                                                        \n"
		"  }                                                                               \n"
		"  ~G() {                                                                          \n"
		"    dtors += v;                                                                   \n"
		"  }                                                                               \n"
		"}                                                                                 \n"
		"int gcNested(int n) {                                                             \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += G(++i).v * 2;                                                            \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		"class E { int v; E(int a) { D d(10); v = 10 / a; } ~E() { dtors += 1000; } }      \n"
		"int ctorThrow(int n) { D d(1); E e(n); return e.v; }                              \n"
		"class P { int v; P(int n) { int local = n * 7; v = inspect() + local; } }         \n"
		"int ctorInspect(int n) { int local = n; P p(n); return p.v + local; }             \n"
		"class S { int v; S(int a) { v = a + 1; } }                                        \n"
		"int suspendedCtor(int n) {                                                        \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += S(i++).v;                                                                \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n";

	enum EMode { PLAIN, COUNT_LINES, SUSPEND_IN_LEAF };
	struct SCase { const char *decl; int arg; EMode mode; };
	static const SCase cases[] =
	{
		{ "int deep(int)",       5000, PLAIN },
		{ "int deep(int)",        300, COUNT_LINES },
		{ "int probe(int)",       300, PLAIN },
		{ "int probe(int)",         5, COUNT_LINES },
		{ "int catchAt(int)",      10, PLAIN },
		{ "int catchAt(int)",     300, PLAIN },
		{ "int throwAt(int)",     300, PLAIN },
		{ "int objects(int)",      20, PLAIN },
		{ "int objects(int)",     300, COUNT_LINES },
		{ "int dispatch()",         0, PLAIN },
		{ "int dispatch()",         0, COUNT_LINES },
		{ "int blocks(int)",      200, PLAIN },
		{ "int nullIntf()",         0, PLAIN },
		{ "int nullFunc()",         0, PLAIN },
		{ "int callUnbound()",      0, PLAIN },
		{ "int suspended(int)",    20, SUSPEND_IN_LEAF },
		{ "int gcNested(int)",     50, PLAIN },
		{ "int gcNested(int)",     50, COUNT_LINES },
		{ "int ctorThrow(int)",     0, PLAIN },
		{ "int ctorThrow(int)",     2, PLAIN },
		{ "int ctorInspect(int)",   3, PLAIN },
		{ "int suspendedCtor(int)", 20, SUSPEND_IN_LEAF },
	};

	// Engine properties that change how the stack is managed
	struct SConfig { const char *name; asEEngineProp prop; asPWORD value; };
	static const SConfig configs[] =
	{
		{ "default",              asEP_INIT_STACK_SIZE,     4096 },
		{ "small stack blocks",   asEP_INIT_STACK_SIZE,     64 },
		{ "call stack limit",     asEP_MAX_CALL_STACK_SIZE, 100 },
		{ "stack size limit",     asEP_MAX_STACK_SIZE,      4096 },
	};

	// Executes all the cases and returns what was observed, one line per case.
	// The reference is compiled without the JIT instructions, as the compiled
	// code invokes the line callback as the VM does for such bytecode
	static std::string Run(asIScriptEngine *engine, CJITCompiler *jit, const SConfig &config, bool &fail)
	{
		COutStream out;
		engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, jit != 0);
		engine->SetJITCompiler(jit);
		engine->SetEngineProperty(config.prop, config.value);
		engine->RegisterGlobalFunction("int inspect()", asFUNCTION(Inspect), asCALL_CDECL);

		asIScriptModule *other = engine->GetModule("other", asGM_ALWAYS_CREATE);
		other->AddScriptSection("other", "int imported(int a) { return a * 3; }");
		if( other->Build() < 0 )
			TEST_FAILED;
		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", script);
		if( mod->Build() < 0 )
			TEST_FAILED;
		// The function 'unbound' doesn't exist in the other module
		mod->BindAllImportedFunctions();
		int *dtors = (int*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("dtors"));

		std::string result;
		asIScriptContext *ctx = engine->CreateContext();
		ctx->SetExceptionCallback(asFUNCTION(OnException), 0, asCALL_CDECL);
		for( asUINT n = 0; n < sizeof(cases)/sizeof(cases[0]); n++ )
		{
			g_trace.str("");
			g_lines = 0;
			*dtors = 0;

			asIScriptFunction *func = mod->GetFunctionByDecl(cases[n].decl);
			if( func == 0 ) { TEST_FAILED; continue; }
			if( cases[n].mode == COUNT_LINES )
				ctx->SetLineCallback(asFUNCTION(CountLines), 0, asCALL_CDECL);
			else if( cases[n].mode == SUSPEND_IN_LEAF )
				ctx->SetLineCallback(asFUNCTION(SuspendInLeaf), 0, asCALL_CDECL);
			else
				ctx->ClearLineCallback();
			ctx->Prepare(func);
			if( func->GetParamCount() > 0 )
				ctx->SetArgDWord(0, cases[n].arg);
			int r = ctx->Execute();
			int suspends = 0;
			while( r == asEXECUTION_SUSPENDED && suspends < 10000 )
			{
				suspends++;
				r = ctx->Execute();
			}

			std::stringstream s;
			s << cases[n].decl << "(" << cases[n].arg << "): " << r;
			if( r == asEXECUTION_FINISHED )
				s << " returned " << int(ctx->GetReturnDWord());
			s << " lines " << g_lines << " suspends " << suspends << " dtors " << *dtors << " " << g_trace.str() << "\n";
			result += s.str();
		}
		ctx->Release();
		return result;
	}
}

static bool TestNativeCalls()
{
	using namespace NativeCalls;
	bool fail = false;

	// Suspending must work for the results to be the same as the VM's
	asDWORD flags = 0;
	const char *env = getenv("AS_JIT_FLAGS");
	if( env )
		flags = asDWORD(strtoul(env, 0, 0)) & ~asDWORD(CJITCompiler::JIT_NO_SUSPEND | CJITCompiler::JIT_LOG);

	// A native call depth of 3 mixes native calls with calls made by the VM
	const asUINT depths[] = { 256, 3 };

	for( asUINT c = 0; c < sizeof(configs)/sizeof(configs[0]); c++ )
	{
		asIScriptEngine *engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
		std::string expected = Run(engine, 0, configs[c], fail);		engine->ShutDownAndRelease();

		for( asUINT d = 0; d < sizeof(depths)/sizeof(depths[0]); d++ )
		{
			// The JIT compiler must outlive the engine
			CJITCompiler jit(flags);
			jit.SetNativeCallDepth(depths[d]);
			engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
			std::string actual = Run(engine, &jit, configs[c], fail);
			engine->ShutDownAndRelease();

			if( jit.GetStatistics().functionsCompiled == 0 )
				TEST_FAILED;
			if( actual != expected )
			{
				// Show the cases that differ
				std::stringstream e(expected), a(actual);
				std::string el, al;
				while( std::getline(e, el) && std::getline(a, al) )
					if( el != al )
						PRINTF("%s, native call depth %u:\n  VM:  %s\n  JIT: %s\n", configs[c].name, depths[d],
						       el.substr(0, 300).c_str(), al.substr(0, 300).c_str());
				TEST_FAILED;
			}
		}
	}

	return fail;
}

// Script functions compiled in place of their calls. Everything that the application
// can observe must be the same as with the calls made by the VM, also when the
// inlined code returns control to the VM in the middle of the inlined function
namespace Inlining
{
	static std::stringstream g_trace;
	static int g_lines = 0;

	// Returns the variable if it is in scope
	static int *FindVar(asIScriptContext *ctx, asUINT level, const char *var)
	{
		for( int v = 0; v < ctx->GetVarCount(level); v++ )
		{
			const char *name;
			ctx->GetVar(v, level, &name);
			if( name && std::string(name) == var && ctx->IsVarInScope(v, level) )
				return (int*)ctx->GetAddressOfVar(v, level);
		}
		return 0;
	}

	// Records the call stack and the variables named 'local' from the given level.
	// The nested executions have a marker without a function in the call stack
	static void Dump(asIScriptContext *ctx, asUINT varLevel = 0)
	{
		for( asUINT l = 0; l < ctx->GetCallstackSize(); l++ )
		{
			asIScriptFunction *func = ctx->GetFunction(l);
			if( func == 0 )
			{
				g_trace << "- ";
				continue;
			}
			g_trace << func->GetName() << ":" << ctx->GetLineNumber(l);
			int *local = l >= varLevel ? FindVar(ctx, l, "local") : 0;
			if( local )
				g_trace << "=" << *local;
			g_trace << " ";
		}
	}

	static void OnException(asIScriptContext *ctx, void *)
	{
		g_trace << "[" << ctx->GetExceptionString() << " in " << ctx->GetExceptionFunction()->GetName() << ":" <<
		           ctx->GetExceptionLineNumber() << " depth " << ctx->GetCallstackSize() << "] ";

		// The function that didn't get the stack memory for its variables is on top
		Dump(ctx, std::string(ctx->GetExceptionString()) == "Stack overflow" ? 1 : 0);
	}

	static void CountLines(asIScriptContext *, void *)
	{
		g_lines++;
	}

	static void SuspendInAdd(asIScriptContext *ctx, void *)
	{
		g_lines++;
		if( std::string(ctx->GetFunction()->GetName()) == "add" )
			ctx->Suspend();
	}

	// Modifies the variable of the caller of add like a debugger
	static void PokeInAdd(asIScriptContext *ctx, void *)
	{
		g_lines++;
		if( std::string(ctx->GetFunction()->GetName()) != "add" )
			return;
		int *local = FindVar(ctx, 1, "local");
		if( local )
			*local += 100;
		Dump(ctx);
	}

	// Starts counting the lines in the middle of the execution
	static int StartLines(bool start)
	{
		if( start )
			asGetActiveContext()->SetLineCallback(asFUNCTION(CountLines), 0, asCALL_CDECL);
		return 1;
	}

	// The registered functions called by the inlined functions, which see the frames
	// of the inlined calls
	static int Verify(int a)
	{
		if( a == -1 )
			asGetActiveContext()->SetException("negative");
		else if( a == -2 )
			throw std::runtime_error("more negative");
		return a * 3;
	}

	static int Inspect(int a)
	{
		Dump(asGetActiveContext());
		return a;
	}

	static int Pause(int a)
	{
		if( a % 3 == 0 )
			asGetActiveContext()->Suspend();
		return a + 1;
	}

	// Modifies the variables of the function and its callers like a debugger, which
	// is attached for that
	static int Poke(int a)
	{
		asIScriptContext *ctx = asGetActiveContext();
		if( a == 2 )
		{
			ctx->SetLineCallback(asFUNCTION(CountLines), 0, asCALL_CDECL);
			for( asUINT l = 0; l < 3; l++ )
			{
				int *local = FindVar(ctx, l, "local");
				if( local )
					*local += 100 << l;
			}
		}
		return a;
	}

	// The functions that are executed with a line callback avoid the statements
	// followed by a declaration or a block, see NativeCalls
	static const char *script =
		"int g = 0;                                                                        \n"
		"int add(int a, int b) { return a + b; }                                           \n"
		"int sq(int a) { return a * a; }                                                   \n"
		"int absInc(int a) { if( a < 0 ) return 1 - a; return a + 1; }                     \n"
		"int sum3(int a) { int s = 0, k = 0; while( k < 3 ) s += a + k++; return s; }      \n"
		"int bump(int a) { g += a; return g; }                                             \n"
		"double half(double a, int b) { return a * 0.5 + b; }                              \n"
		"int64 wide(int64 a, int b) { return (a << 3) + b; }                               \n"
		"float scale(float a) { return a * 1.5f; }                                         \n"
		"uint8 low(int a) { return uint8(a); }                                             \n"
		"bool odd(int a) { return (a & 1) == 1; }                                          \n"
		"int loop(int n) {                                                                 \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += add(i, sq(i)) + absInc(i - 5) + sum3(i) + bump(1) + int(half(i, 1)) +    \n"
		"         int(wide(i, 2)) + int(scale(i)) + low(i * 50) + (odd(i++) ? 1 : 0);      \n"
		"  return r + g;                                                                   \n"
		"}                                                                                 \n"
		"int div(int a, int b) {                                                           \n"
		"  int local = a + b;                                                              \n"
		"  return a / b + local;                                                           \n"
		"}                                                                                 \n"
		"int divLoop(int n) {                                                              \n"
		"  int local = n * 3, r = 0;                                                       \n"
		"  while( n >= -2 )                                                                \n"
		"    r += div(100, n--);                                                           \n"
		"  return r + local;                                                               \n"
		"}                                                                                 \n"
		"int divCatch(int n) {                                                             \n"
		"  int local = n, r = 0;                                                           \n"
		"  while( n >= -2 ) {                                                              \n"
		"    try { r += div(100, n--); } catch { r -= 1000; }                              \n"
		"  }                                                                               \n"
		"  return r + local;                                                               \n"
		"}                                                                                 \n"
		"int deepLeaf(int n) {                                                             \n"
		"  int local = add(n, 1);                                                          \n"
		"  return n == 0 ? sq(local) : deepLeaf(n - 1) + local;                            \n"
		"}                                                                                 \n"
		"int deepThrow(int n) {                                                            \n"
		"  int local = add(n, 2);                                                          \n"
		"  return n == 0 ? div(1, n) : deepThrow(n - 1) + local;                           \n"
		"}                                                                                 \n"
		"int toggle(int n) {                                                               \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += add(i, lines(i++ == n / 2));                                             \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// The methods are inlined for the only class of the module that implements
		// them, and the objects of the other module call them
		"shared interface IVal { int get(int a); }                                         \n"
		"import IVal@ makeOther() from \"other\";                                          \n"
		"class Acc : IVal {                                                                \n"
		"  int v = 1;                                                                      \n"
		"  int get(int a) { return v + a; }                                                \n"
		"  int add(int a) { v += a; return v; }                                            \n"
		"  int ratio(int a) { int local = v; return local / a; }                           \n"
		"  int get2(int a) { return get(a) + get(a + 1); }                                 \n"
		"  int ratio2(int a) { int local = a * 2; return ratio(a) + local; }               \n"
		"}                                                                                 \n"
		"class Base { int k = 1; int f(int a) { return a + k; } }                          \n"
		"class Derived : Base { int f(int a) override { return a * 2 + k; } }              \n"
		"abstract class Shape { int w = 3; int area(int h) { return w * h; } }             \n"
		"class Rect : Shape {}                                                             \n"
		// The method that several classes inherit is inlined for the objects whose class
		// has it, and the objects of the class of the other module overriding it call theirs
		"shared class SBase { int k = 2; int g(int a) { return a * k; } }                  \n"
		"class SKid : SBase {}                                                             \n"
		"import SBase@ makeOver() from \"other\";                                          \n"
		"int inherited(int n) {                                                            \n"
		"  SBase@ kid = SKid();                                                            \n"
		"  SBase@ base = SBase();                                                          \n"
		"  SBase@ over = makeOver();                                                       \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += kid.g(i) + (i % 3 == 0 ? over : base).g(i++);                            \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		"int methods(int n) {                                                              \n"
		"  Acc@ acc = Acc();                                                               \n"
		"  IVal@ own = acc;                                                                \n"
		"  IVal@ other = makeOther();                                                      \n"
		"  Base@ b = Base();                                                               \n"
		"  Base@ bd = Derived();                                                           \n"
		"  Derived@ d = Derived();                                                         \n"
		"  Shape@ s = Rect();                                                              \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += acc.add(i) + own.get(i) + (i % 3 == 0 ? other : own).get(i) +            \n"
		"         b.f(i) + bd.f(i) + d.f(i) + s.area(i++);                                 \n"
		"  return r + acc.v;                                                               \n"
		"}                                                                                 \n"
		"int nullCall(int n) {                                                             \n"
		"  IVal@ v = Acc();                                                                \n"
		"  int local = v.get(n);                                                           \n"
		"  if( n > 2 ) @v = null;                                                          \n"
		"  return v.get(local);                                                            \n"
		"}                                                                                 \n"
		"int ratioLoop(int n) {                                                            \n"
		"  Acc@ acc = Acc();                                                               \n"
		"  int local = 0;                                                                  \n"
		"  while( n >= -2 )                                                                \n"
		"    local += acc.ratio(n--);                                                      \n"
		"  return local;                                                                   \n"
		"}                                                                                 \n"
		"int deepMethod(int n) {                                                           \n"
		"  Acc@ acc = Acc();                                                               \n"
		"  int local = n;                                                                  \n"
		"  local += acc.add(n);                                                            \n"
		"  return n == 0 ? acc.get(local) : deepMethod(n - 1) + local;                     \n"
		"}                                                                                 \n"
		// The inlined functions have the functions that they call inlined too
		"int addSq(int a) { return add(a, sq(a)); }                                        \n"
		"int inc(int a) { return a + 1; }                                                  \n"
		"int inc2(int a) { return inc(inc(a)); }                                           \n"
		"int inc4(int a) { int local = a; local = inc2(a); return inc2(local) + add(local, 1); }\n"
		"int divIn(int a, int b) { int local = a - b; return div(a, b) + local; }          \n"
		"int nested(int n) {                                                               \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += inc4(i) + addSq(i) + divIn(i * 7, i++ + 1);                              \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		"int divNest(int n) {                                                              \n"
		"  int local = n * 5, r = 0;                                                       \n"
		"  while( n >= -2 )                                                                \n"
		"    r += divIn(100, n--);                                                         \n"
		"  return r + local;                                                               \n"
		"}                                                                                 \n"
		"int deepNest(int n) {                                                             \n"
		"  int local = n;                                                                  \n"
		"  local += addSq(n);                                                              \n"
		"  return n == 0 ? inc4(local) : deepNest(n - 1) + divIn(local, n);                \n"
		"}                                                                                 \n"
		"int deepDiv(int n) {                                                              \n"
		"  int local = n;                                                                  \n"
		"  local += inc2(n);                                                               \n"
		"  return n == 0 ? divIn(1, n) : deepDiv(n - 1) + local;                           \n"
		"}                                                                                 \n"
		"int methods2(int n) {                                                             \n"
		"  Acc@ acc = Acc();                                                               \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += acc.get2(i) + acc.ratio2(i++ + 1) + acc.add(1);                          \n"
		"  return r + acc.v;                                                               \n"
		"}                                                                                 \n"
		"int ratioNest(int n) {                                                            \n"
		"  Acc@ acc = Acc();                                                               \n"
		"  int local = 0;                                                                  \n"
		"  while( n >= -2 )                                                                \n"
		"    local += acc.ratio2(n--);                                                     \n"
		"  return local;                                                                   \n"
		"}                                                                                 \n"
		// The variables that aren't read anymore are still seen by the VM after it has
		// entered the function behind an inlined call, or a debugger has modified them
		"int deadAdd(int n) {                                                              \n"
		"  int local = n * 7;                                                              \n"
		"  int r = add(n, 1);                                                              \n"
		"  return add(r, 2);                                                               \n"
		"}                                                                                 \n"
		"int deadLoop(int n) {                                                             \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += deadAdd(i++);                                                            \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// The inlined functions call registered functions, which raise exceptions,
		// suspend the context, and inspect or modify the variables of the callers
		"int check(int a) { int local = a + 1; return verify(a) + local; }                 \n"
		"int check2(int a) { int local = a * 2; return check(a - 1) + local; }             \n"
		"int checkLoop(int n) {                                                            \n"
		"  int local = n, r = 0;                                                           \n"
		"  while( n >= -2 )                                                                \n"
		"    r += check2(n--);                                                             \n"
		"  return r + local;                                                               \n"
		"}                                                                                 \n"
		"int checkCatch(int n) {                                                           \n"
		"  int local = n, r = 0;                                                           \n"
		"  while( n >= -2 ) {                                                              \n"
		"    try { r += check2(n--); } catch { r -= 1000; }                                \n"
		"  }                                                                               \n"
		"  return r + local;                                                               \n"
		"}                                                                                 \n"
		"int look(int a) { int local = a * 3; return inspect(a) + local; }                 \n"
		"int look2(int a) { int local = a + 7; return look(a) + look(a + 1) + local; }     \n"
		"int lookLoop(int n) {                                                             \n"
		"  int local = n, r = 0, i = 0;                                                    \n"
		"  while( i < n )                                                                  \n"
		"    r += look2(i++);                                                              \n"
		"  return r + local;                                                               \n"
		"}                                                                                 \n"
		"int wait(int a) { int local = a - 1; return pause(a) + local; }                   \n"
		"int wait2(int a) { int local = a; return wait(a) * 2 + local; }                   \n"
		"int waitLoop(int n) {                                                             \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += wait2(i++) + add(i, 1);                                                  \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		"int pokeIn(int a) { int local = a; return poke(a) + local; }                      \n"
		"int pokeOut(int a) { int local = a * 2; return pokeIn(a) + local; }               \n"
		"int pokeLoop(int n) {                                                             \n"
		"  int local = n, r = 0, i = 0;                                                    \n"
		"  while( i < n )                                                                  \n"
		"    r += pokeOut(i++);                                                            \n"
		"  return r + local;                                                               \n"
		"}                                                                                 \n"
		// The inlined functions create and release objects, whose constructors and
		// destructors are called, and call the functions that aren't inlined
		"int drops = 0;                                                                    \n"
		"class Tmp { int v; Tmp(int a) { v = a; } ~Tmp() { drops += v; if( v == 3 ) inspect(v); } } \n"
		"Tmp@ kept;                                                                        \n"
		"int useTmp(int a) { Tmp t(a); int local = t.v * 2; return local + a; }            \n"
		"int keepTmp(int a) { Tmp@ t = Tmp(a); @kept = t; return t.v + 1; }                \n"
		"int tmpLoop(int n) {                                                              \n"
		"  int local = n, r = 0, i = 0;                                                    \n"
		"  while( i < n )                                                                  \n"
		"    r += useTmp(i) + keepTmp(i++);                                                \n"
		"  return r + drops + local;                                                       \n"
		"}                                                                                 \n"
		"class Div { int v; Div(int a) { v = 12 / a; } }                                   \n"
		"int makeDiv(int a) { Div d(a); int local = d.v; return local + a; }               \n"
		"int divObjLoop(int n) {                                                           \n"
		"  int local = n, r = 0;                                                           \n"
		"  while( n >= -2 )                                                                \n"
		"    r += makeDiv(n--);                                                            \n"
		"  return r + local;                                                               \n"
		"}                                                                                 \n"
		"int power(int a, int b) { int local = b; return a ** b + local; }                 \n"
		"int powLoop(int n) {                                                              \n"
		"  int local = n, r = 0, i = 0;                                                    \n"
		"  while( i < n )                                                                  \n"
		"    r += power(3, i++);                                                           \n"
		"  return r + local;                                                               \n"
		"}                                                                                 \n"
		"int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }                          \n"
		"int useFact(int a) { int local = a % 6; return fact(local) + local; }             \n"
		"int factLoop(int n) {                                                             \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += useFact(i++);                                                            \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		"int viaDeep(int n) { int local = n * 2; return deepCall(n) + local; }             \n"
		"int deepCall(int n) {                                                             \n"
		"  int local = n;                                                                  \n"
		"  return n == 0 ? inspect(local) : add(viaDeep(n - 1), local);                    \n"
		"}                                                                                 \n"
		// A debugger modifies the variables that the callers of the inlined functions
		// don't read anymore
		"int pokeDead(int a) { int local = poke(a); return inspect(local) + a; }           \n"
		"int pokeMid(int a) { int local = a * 3; return pokeDead(a) + 1; }                 \n"
		"int pokeDeadLoop(int n) {                                                         \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += pokeMid(i++);                                                            \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// The inlined functions call registered functions one after another, and raise
		// exceptions or return to the VM for the line callback between the calls
		"class Box { int v; Box(int a) { v = a; } }                                        \n"
		"int both(Box@ b, int a) { int local = verify(a); local += b.v; return inspect(local) + a; }\n"
		"int bothLoop(int n) {                                                             \n"
		"  Box@ box = Box(5);                                                              \n"
		"  int local = n, r = 0, i = 0;                                                    \n"
		"  while( i < n )                                                                  \n"
		"    r += both(i == 3 ? null : box, i++);                                          \n"
		"  return r + local;                                                               \n"
		"}                                                                                 \n"
		// The inlined functions borrow the references of the handles that the callers
		// pass, and the frames that the VM takes over get references of their own
		"int freed = 0;                                                                    \n"
		"class Ref { int v; Ref(int a) { v = a; } ~Ref() { freed += v; } int get(int a) { return v / a; } int plus(Ref@ o) { return v + o.v; } }\n"
		"Ref@ gref;                                                                        \n"
		"int readRef(Ref@ r, int a) { int local = a; local += verify(a); return r.v + local; }\n"
		"int refLoop(int n) {                                                              \n"
		"  Ref@ ref = Ref(7);                                                              \n"
		"  int local = n, r = 0;                                                           \n"
		"  while( n >= -2 ) {                                                              \n"
		"    try { r += readRef(ref, n--); } catch { r -= 1000; }                          \n"
		"  }                                                                               \n"
		"  @ref = null;                                                                    \n"
		"  return r + freed + local;                                                       \n"
		"}                                                                                 \n"
		"int refThrow(int n) {                                                             \n"
		"  Ref@ ref = Ref(11);                                                             \n"
		"  int local = n;                                                                  \n"
		"  return readRef(ref, n) + local;                                                 \n"
		"}                                                                                 \n"
		"int getFreed(int n) { return freed + n; }                                         \n"
		"int waitRef(Ref@ r, int a) { int local = a; local += pause(a); return r.v + local; }\n"
		"int waitRefLoop(int n) {                                                          \n"
		"  Ref@ ref = Ref(3);                                                              \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += waitRef(ref, i++) + add(i, 1);                                           \n"
		"  @ref = null;                                                                    \n"
		"  return r + freed;                                                               \n"
		"}                                                                                 \n"
		// The borrowed references are passed on to the functions inlined into them
		"int refV(Ref@ r) { return r.v; }                                                  \n"
		"int refLook(Ref@ r, int a) { int local = a; local += inspect(a); return refV(r) + local; }\n"
		"int refNest(Ref@ r, int a) { int local = a; local += refLook(r, a) + verify(a); return refV(r) * local; }\n"
		"int refGet(Ref@ r, int a) { int local = a; return r.get(a) + local; }             \n"
		"int refNestLoop(int n) {                                                          \n"
		"  Ref@ ref = Ref(5);                                                              \n"
		"  int local = n, r = 0;                                                           \n"
		"  while( n >= -2 ) {                                                              \n"
		"    int k = n--;                                                                  \n"
		"    try { r += refNest(ref, k) + refGet(ref, k); } catch { r -= 1000; }           \n"
		"  }                                                                               \n"
		"  @ref = null;                                                                    \n"
		"  return r + freed + local;                                                       \n"
		"}                                                                                 \n"
		"int refNull(int n) {                                                              \n"
		"  Ref@ ref, one;                                                                  \n"
		"  if( n > 2 ) @ref = Ref(4);                                                      \n"
		"  if( n > 3 ) @one = Ref(1);                                                      \n"
		"  int local = n, r = 0;                                                           \n"
		"  while( n >= 0 ) {                                                               \n"
		"    try { r += refGet(ref, n) + readRef(ref, n) + one.plus(ref) + ref.plus(one); } catch { r -= 1000; }\n"
		"    n--;                                                                          \n"
		"  }                                                                               \n"
		"  @ref = null;                                                                    \n"
		"  @one = null;                                                                    \n"
		"  return r + freed + local;                                                       \n"
		"}                                                                                 \n"
		"int refLines(int n) {                                                             \n"
		"  Ref@ ref = Ref(6);                                                              \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n )                                                                  \n"
		"    r += refNest(ref, i) + twice(ref, ref) + same(ref, i++).v + add(i, 1);        \n"
		"  @ref = null;                                                                    \n"
		"  return r + freed;                                                               \n"
		"}                                                                                 \n"
		// The constructors borrow the references too, and the functions that return the
		// handles add references of their own
		"class Pair { int s; Pair(Ref@ a, Ref@ b, int k) { s = a.v * 10 + verify(k) + b.v; } }\n"
		"int twice(Ref@ a, Ref@ b) { return a.v * 10 + b.v; }                              \n"
		"Ref@ same(Ref@ r, int a) { verify(a); return r; }                                 \n"
		"int pairLoop(int n) {                                                             \n"
		"  Ref@ x = Ref(1), y = Ref(2);                                                    \n"
		"  int local = n, r = 0;                                                           \n"
		"  while( n >= -2 ) {                                                              \n"
		"    int k = n--;                                                                  \n"
		"    try { Pair p(x, y, k); r += p.s + twice(x, x) + twice(y, x) + same(y, k).v; } catch { r -= 1000; }\n"
		"  }                                                                               \n"
		"  @x = null;                                                                      \n"
		"  r += freed;                                                                     \n"
		"  @y = null;                                                                      \n"
		"  return r + freed + local;                                                       \n"
		"}                                                                                 \n"
		// The called functions replace the handles that they get, through globals or
		// output parameters
		"int dropRef(Ref@ r, Ref@ &out h) { @h = null; return r.v + freed; }               \n"
		"int dropG(Ref@ r) { @gref = null; return r.v + freed; }                           \n"
		"int viaRef(Ref@ &in h, int a) { Ref@ r = h; return readRef(r, a) + twice(h, h); } \n"
		"int aliasLoop(int n) {                                                            \n"
		"  Ref@ ref;                                                                       \n"
		"  int r = 0, i = 0;                                                               \n"
		"  while( i < n ) {                                                                \n"
		"    @ref = Ref(i + 1);                                                            \n"
		"    r += dropRef(ref, ref);                                                       \n"
		"    @gref = Ref(i + 2);                                                           \n"
		"    r += dropG(gref) + viaRef(Ref(i + 3), i);                                     \n"
		"    i++;                                                                          \n"
		"  }                                                                               \n"
		"  return r + freed;                                                               \n"
		"}                                                                                 \n";

	// Implements the interface shared with the module of the test
	static const char *otherScript =
		"shared interface IVal { int get(int a); }                                         \n"
		"class Other : IVal { int get(int a) { return a * 100; } }                         \n"
		"IVal@ makeOther() { return Other(); }                                             \n"
		"shared class SBase { int k = 2; int g(int a) { return a * k; } }                  \n"
		"class Over : SBase { int g(int a) override { return a * 1000 + k; } }             \n"
		"SBase@ makeOver() { return Over(); }                                              \n";

	enum EMode { PLAIN, COUNT_LINES, SUSPEND_IN_ADD, POKE_IN_ADD };
	struct SCase { const char *decl; int arg; EMode mode; };
	static const SCase cases[] =
	{
		{ "int loop(int)",      100, PLAIN },
		{ "int loop(int)",       10, COUNT_LINES },
		{ "int loop(int)",       10, SUSPEND_IN_ADD },
		{ "int divLoop(int)",     5, PLAIN },
		{ "int divCatch(int)",    5, PLAIN },
		{ "int deepLeaf(int)",  300, PLAIN },
		{ "int deepLeaf(int)",   50, COUNT_LINES },
		{ "int deepLeaf(int)",   20, SUSPEND_IN_ADD },
		{ "int deepThrow(int)",  10, PLAIN },
		{ "int deepThrow(int)", 300, PLAIN },
		{ "int toggle(int)",     20, PLAIN },
		{ "int methods(int)",   100, PLAIN },
		{ "int methods(int)",    10, COUNT_LINES },
		{ "int methods(int)",    10, SUSPEND_IN_ADD },
		{ "int inherited(int)",  20, PLAIN },
		{ "int inherited(int)",   5, COUNT_LINES },
		{ "int nullCall(int)",    1, PLAIN },
		{ "int nullCall(int)",    5, PLAIN },
		{ "int ratioLoop(int)",   5, PLAIN },
		{ "int deepMethod(int)", 300, PLAIN },
		{ "int deepMethod(int)",  50, COUNT_LINES },
		{ "int deepMethod(int)",  20, SUSPEND_IN_ADD },
		{ "int nested(int)",      50, PLAIN },
		{ "int nested(int)",      10, COUNT_LINES },
		{ "int nested(int)",      10, SUSPEND_IN_ADD },
		{ "int divNest(int)",      5, PLAIN },
		{ "int deepNest(int)",   300, PLAIN },
		{ "int deepNest(int)",    20, SUSPEND_IN_ADD },
		{ "int deepDiv(int)",     10, PLAIN },
		{ "int deepDiv(int)",    300, PLAIN },
		{ "int methods2(int)",    50, PLAIN },
		{ "int methods2(int)",    10, COUNT_LINES },
		{ "int ratioNest(int)",    5, PLAIN },
		{ "int deadAdd(int)",      5, SUSPEND_IN_ADD },
		{ "int deadLoop(int)",     3, SUSPEND_IN_ADD },
		{ "int deadAdd(int)",      5, POKE_IN_ADD },
		{ "int deadLoop(int)",     3, POKE_IN_ADD },
		{ "int checkLoop(int)",    5, PLAIN },
		{ "int checkCatch(int)",   5, PLAIN },
		{ "int lookLoop(int)",     5, PLAIN },
		{ "int lookLoop(int)",     5, COUNT_LINES },
		{ "int waitLoop(int)",    10, PLAIN },
		{ "int waitLoop(int)",    10, SUSPEND_IN_ADD },
		{ "int pokeLoop(int)",     5, PLAIN },
		{ "int tmpLoop(int)",     10, PLAIN },
		{ "int tmpLoop(int)",     10, COUNT_LINES },
		{ "int divObjLoop(int)",   5, PLAIN },
		{ "int powLoop(int)",     25, PLAIN },
		{ "int factLoop(int)",    20, PLAIN },
		{ "int deepCall(int)",   300, PLAIN },
		{ "int deepCall(int)",    20, COUNT_LINES },
		{ "int deepCall(int)",    20, SUSPEND_IN_ADD },
		{ "int pokeDeadLoop(int)", 5, PLAIN },
		{ "int bothLoop(int)",     3, PLAIN },
		{ "int bothLoop(int)",     3, COUNT_LINES },
		{ "int bothLoop(int)",     5, PLAIN },
		{ "int refLoop(int)",      5, PLAIN },
		{ "int refThrow(int)",     4, PLAIN },
		{ "int refThrow(int)",    -1, PLAIN },
		{ "int getFreed(int)",     0, PLAIN },
		{ "int refThrow(int)",    -2, PLAIN },
		{ "int getFreed(int)",     0, PLAIN },
		{ "int waitRefLoop(int)", 10, PLAIN },
		{ "int waitRefLoop(int)", 10, COUNT_LINES },
		{ "int waitRefLoop(int)", 10, SUSPEND_IN_ADD },
		{ "int refNestLoop(int)",  5, PLAIN },
		{ "int refLines(int)",     5, COUNT_LINES },
		{ "int refLines(int)",     5, SUSPEND_IN_ADD },
		{ "int refNull(int)",      5, PLAIN },
		{ "int refNull(int)",      3, PLAIN },
		{ "int refNull(int)",      2, PLAIN },
		{ "int pairLoop(int)",     5, PLAIN },
		{ "int aliasLoop(int)",    5, PLAIN },
	};

	// Executes all the cases and returns what was observed, one line per case
	static std::string Run(asIScriptEngine *engine, CJITCompiler *jit, const NativeCalls::SConfig &config, bool &fail)
	{
		COutStream out;
		engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, jit != 0);
		engine->SetJITCompiler(jit);
		engine->SetEngineProperty(config.prop, config.value);
		engine->RegisterGlobalFunction("int lines(bool)", asFUNCTION(StartLines), asCALL_CDECL);
		engine->RegisterGlobalFunction("int verify(int)", asFUNCTION(Verify), asCALL_CDECL);
		engine->RegisterGlobalFunction("int inspect(int)", asFUNCTION(Inspect), asCALL_CDECL);
		engine->RegisterGlobalFunction("int pause(int)", asFUNCTION(Pause), asCALL_CDECL);
		engine->RegisterGlobalFunction("int poke(int)", asFUNCTION(Poke), asCALL_CDECL);

		asIScriptModule *other = engine->GetModule("other", asGM_ALWAYS_CREATE);
		other->AddScriptSection("other", otherScript);
		if( other->Build() < 0 )
			TEST_FAILED;
		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", script);
		if( mod->Build() < 0 || mod->BindAllImportedFunctions() < 0 )
			TEST_FAILED;

		std::string result;
		asIScriptContext *ctx = engine->CreateContext();
		ctx->SetExceptionCallback(asFUNCTION(OnException), 0, asCALL_CDECL);
		for( asUINT n = 0; n < sizeof(cases)/sizeof(cases[0]); n++ )
		{
			g_trace.str("");
			g_lines = 0;

			asIScriptFunction *func = mod->GetFunctionByDecl(cases[n].decl);
			if( func == 0 ) { TEST_FAILED; continue; }
			if( cases[n].mode == COUNT_LINES )
				ctx->SetLineCallback(asFUNCTION(CountLines), 0, asCALL_CDECL);
			else if( cases[n].mode == SUSPEND_IN_ADD )
				ctx->SetLineCallback(asFUNCTION(SuspendInAdd), 0, asCALL_CDECL);
			else if( cases[n].mode == POKE_IN_ADD )
				ctx->SetLineCallback(asFUNCTION(PokeInAdd), 0, asCALL_CDECL);
			else
				ctx->ClearLineCallback();
			ctx->Prepare(func);
			ctx->SetArgDWord(0, cases[n].arg);
			int r = ctx->Execute();
			int suspends = 0;
			while( r == asEXECUTION_SUSPENDED && suspends < 10000 )
			{
				if( suspends++ < 3 )
				{
					g_trace << "{ ";
					Dump(ctx);
					g_trace << "} ";
				}
				r = ctx->Execute();
			}

			std::stringstream s;
			s << cases[n].decl << "(" << cases[n].arg << "): " << r;
			if( r == asEXECUTION_FINISHED )
				s << " returned " << int(ctx->GetReturnDWord());
			s << " lines " << g_lines << " suspends " << suspends << " " << g_trace.str() << "\n";
			result += s.str();
		}
		ctx->Release();
		return result;
	}

	// The inlined function runs a loop that another thread suspends. It is called
	// directly or through another inlined function
	static const char *spinScript =
		"bool stop = false;                                                                \n"
		"int iters = 0;                                                                    \n"
		"int work(int a) {                                                                 \n"
		"  int s = 0;                                                                      \n"
		"  for( int k = 0; k < 64; k++ )                                                   \n"
		"    s += (k ^ a) & 7;                                                             \n"
		"  return s;                                                                       \n"
		"}                                                                                 \n"
		"int spin() {                                                                      \n"
		"  int r = 0;                                                                      \n"
		"  while( !stop ) {                                                                \n"
		"    r = (r * 31 + work(iters)) & 0xFFFFFF;                                        \n"
		"    iters++;                                                                      \n"
		"  }                                                                               \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		"int step(int a) { return work(a) & 0xFFFFFF; }                                    \n"
		"int spinStep() {                                                                  \n"
		"  int r = 0;                                                                      \n"
		"  while( !stop ) {                                                                \n"
		"    r = (r * 31 + step(iters)) & 0xFFFFFF;                                        \n"
		"    iters++;                                                                      \n"
		"  }                                                                               \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n";

	static int Work(int a, int count)
	{
		int s = 0;
		for( int k = 0; k < count; k++ )
			s += (k ^ a) & 7;
		return s;
	}

	static bool TestSuspendFromThread(asDWORD flags, bool inlines, bool throughStep)
	{
		bool fail = false;

		// The JIT compiler must outlive the engine
		CJITCompiler jit(flags);
		asIScriptEngine *engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
		COutStream out;
		engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, true);
		engine->SetJITCompiler(&jit);

		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", spinScript);
		if( mod->Build() < 0 )
			TEST_FAILED;
		bool *stop = (bool*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("stop"));
		int *iters = (int*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("iters"));

		asIScriptContext *ctx = engine->CreateContext();
		ctx->Prepare(mod->GetFunctionByDecl(throughStep ? "int spinStep()" : "int spin()"));
		int r = asEXECUTION_SUSPENDED;
		int inWork = 0;
		for( int round = 0; round < 10 && r == asEXECUTION_SUSPENDED; round++ )
		{
			// A suspension right before the execution starts would be lost
			std::atomic<bool> done(false);
			std::thread suspender([&]()
			{
				while( !done )
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					ctx->Suspend();
				}
			});
			r = ctx->Execute();
			done = true;
			suspender.join();
			if( r != asEXECUTION_SUSPENDED )
			{
				PRINTF("suspend from thread: execution returned %d\n", r);
				TEST_FAILED;
				break;
			}

			// Suspended in the inlined function, which has the argument and the
			// variables that the loop has computed so far, and its caller has the
			// argument too
			std::string top = ctx->GetFunction(0)->GetName();
			asUINT depth = ctx->GetCallstackSize();
			if( top == "work" )
			{
				inWork++;
				int *a = FindVar(ctx, 0, "a"), *s = FindVar(ctx, 0, "s"), *k = FindVar(ctx, 0, "k");
				int *stepA = throughStep ? FindVar(ctx, 1, "a") : a;
				if( depth != (throughStep ? 3u : 2u) || std::string(ctx->GetFunction(1)->GetName()) != (throughStep ? "step" : "spin") ||
				    a == 0 || *a != *iters || stepA == 0 || *stepA != *iters ||
				    (s && k && (*k < 0 || *k > 64 || (*s != Work(*a, *k) && *s != Work(*a, *k + 1)))) )
				{
					PRINTF("suspend from thread: wrong state in work, depth %u, a %d, iters %d, s %d, k %d\n",
					       depth, a ? *a : -1, *iters, s ? *s : -1, k ? *k : -1);
					TEST_FAILED;
				}
			}
			else if( top == "step" )
			{
				int *a = FindVar(ctx, 0, "a");
				if( !throughStep || depth != 2 || a == 0 || *a != *iters )
					TEST_FAILED;
			}
			else if( depth != 1 )
				TEST_FAILED;
		}

		// Nearly all of the time is spent in the loop of the inlined function
		if( inWork == 0 )
		{
			PRINTF("suspend from thread: never suspended in the inlined function\n");
			TEST_FAILED;
		}

		// The thread may have asked for one more suspension after the last execution
		// returned, which suspends the next one right away
		*stop = true;
		r = ctx->Execute();
		if( r == asEXECUTION_SUSPENDED )
			r = ctx->Execute();
		int expected = 0;
		for( int n = 0; n < *iters; n++ )
			expected = (expected * 31 + Work(n, 64)) & 0xFFFFFF;
		if( r != asEXECUTION_FINISHED || int(ctx->GetReturnDWord()) != expected )
		{
			PRINTF("suspend from thread: execution returned %d, %d instead of %d\n", r, int(ctx->GetReturnDWord()), expected);
			TEST_FAILED;
		}
		ctx->Release();
		engine->ShutDownAndRelease();

		// The function work is inlined into spin and step, and step with it into spinStep
		if( jit.GetStatistics().callsInlined != (inlines ? 4u : 0u) )
		{
			PRINTF("suspend from thread: %u calls inlined\n", jit.GetStatistics().callsInlined);
			TEST_FAILED;
		}
		return fail;
	}

	// Compiles only the function t and the functions and methods f, f1, f2, ... that
	// it calls
	static bool CompileTAndF(asIScriptFunction *func, void *)
	{
		std::string name = func->GetName();
		return name == "t" || name[0] == 'f';
	}

	// The calls that are inlined in all the compiled functions. The methods are those
	// that only one class of the module can implement, and the virtual methods that
	// all the classes that can implement them inherit. The call of an overridden
	// method calls the method of the base class even through the handle of the derived
	// class, and a method called through a global variable has a reference held for the
	// call, which the function releases. The functions calling others are inlined with
	// those, up to 4 levels deep, below which the functions are called, and so are the
	// recursive calls
	static bool TestInlinedCalls(asDWORD flags, bool inlines)
	{
		bool fail = false;
		struct SMethods { const char *script; asUINT inlined; };
		static const SMethods cases[] =
		{
			{ "interface I { int f(); } class A : I { int f() { return 1; } } int t(I@ i) { return i.f(); }", 1 },
			{ "interface I { int f(); } class A : I { int f() { return 1; } } class B : I { int f() { return 2; } } "
			  "int t(I@ i) { return i.f(); }", 0 },
			{ "shared interface I { int f(); } class A : I { int f() { return 1; } } int t(I@ i) { return i.f(); }", 1 },
			{ "class B { int f() { return 1; } } class D : B { int f() override { return 2; } int f(int a) { return a; } } "
			  "int t(B@ b, D@ d) { return b.f() + d.f() + d.f(3); }", 1 },
			{ "abstract class B { int f() { return 1; } } class D : B {} int t(B@ b) { return b.f(); }", 1 },
			{ "interface I { int f(); } abstract class B : I { int f() { return 1; } } class D : B {} "
			  "int t(I@ i, B@ b) { return i.f() + b.f(); }", 2 },
			{ "class B { int f() { return 1; } } class D : B {} class E : D {} "
			  "int t(B@ b, D@ d) { return b.f() + d.f(); }", 2 },
			{ "class B { int f() { return 1; } } class D : B { int f() override { return 2; } } class E : D {} "
			  "int t(B@ b, E@ e) { return b.f() + e.f(); }", 0 },
			{ "interface I { int f(); } class A : I { int f() { return 1; } } class B : A {} "
			  "int t(I@ i) { return i.f(); }", 0 },
			{ "int f2(int a) { return a + 1; } int f1(int a) { return f2(a) * 2; } "
			  "int t(int a) { return f1(a) + f1(a + 1); }", 5 },
			{ "class A { int v = 1; int f2() { return v; } int f1() { return f2() + 1; } } "
			  "int t(A@ a) { return a.f1(); }", 3 },
			{ "interface I { int f2(int a); } class A : I { int f2(int a) { return a; } } I@ g; "
			  "int f1(int a) { return g.f2(a) + 1; } int t(int a) { return f1(a); }", 3 },
			{ "int f5(int a) { return a + 5; } int f4(int a) { return f5(a) + 4; } int f3(int a) { return f4(a) + 3; } "
			  "int f2(int a) { return f3(a) + 2; } int f1(int a) { return f2(a) + 1; } int t(int a) { return f1(a); }", 14 },
			{ "int f(int a) { return a <= 0 ? 0 : f(a - 1) + 1; } int t(int a) { return f(a) + f(a + 1); }", 2 },
			{ "int f2(int a) { return a <= 0 ? 0 : f1(a - 1); } int f1(int a) { return f2(a) + 1; } "
			  "int t(int a) { return f1(a); }", 4 },
		};
		for( asUINT n = 0; n < sizeof(cases)/sizeof(cases[0]); n++ )
		{
			// The JIT compiler must outlive the engine
			CJITCompiler jit(flags);
			jit.SetCompileFilter(CompileTAndF, 0);
			asIScriptEngine *engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
			COutStream out;
			engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);
			engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, true);
			engine->SetJITCompiler(&jit);
			asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
			mod->AddScriptSection("test", cases[n].script);
			if( mod->Build() < 0 )
				TEST_FAILED;
			engine->ShutDownAndRelease();

			SJITStatistics stats = jit.GetStatistics();
			if( stats.callsInlined != (inlines ? cases[n].inlined : 0) )
			{
				PRINTF("inlined methods: %u calls inlined in '%s'\n", stats.callsInlined, cases[n].script);
				TEST_FAILED;
			}
		}
		return fail;
	}
}

static bool TestInlining()
{
	using namespace Inlining;
	using NativeCalls::configs;
	bool fail = false;

	// Suspending must work for the results to be the same as the VM's
	asDWORD flags = 0;
	const char *env = getenv("AS_JIT_FLAGS");
	if( env )
		flags = asDWORD(strtoul(env, 0, 0)) & ~asDWORD(CJITCompiler::JIT_NO_SUSPEND | CJITCompiler::JIT_LOG);
	bool inlines = (flags & (CJITCompiler::JIT_NO_INLINE | CJITCompiler::JIT_NO_SCRIPT_CALLS | CJITCompiler::JIT_SYNC_EVERY_INSTR)) == 0;

	// The forced bails return to the VM in the middle of the inlined functions. A
	// SUSPEND isn't forced, as the VM would call the line callback for the ones
	// that the compiled code skips
	static const asEBCInstr bailAdd[] = { asBC_ADDi };
	static const asEBCInstr bailRet[] = { asBC_RET };
	static const asEBCInstr bailCopy[] = { asBC_RefCpyV };
	static const asEBCInstr bailFree[] = { asBC_FREE };
	struct SVariant { const char *name; asDWORD flags; asUINT maxInlineSize; const asEBCInstr *bails; asUINT bailCount; };
	static const SVariant variants[] =
	{
		{ "inlined",                  0,                           64, 0,       0 },
		{ "not inlined",              CJITCompiler::JIT_NO_INLINE, 64, 0,       0 },
		{ "small inlined functions",  0,                           16, 0,       0 },
		{ "bail at ADDi",             0,                           64, bailAdd, 1 },
		{ "bail at RET",              0,                           64, bailRet, 1 },
		{ "bail at RefCpyV",          0,                           64, bailCopy, 1 },
		{ "bail at FREE",             0,                           64, bailFree, 1 },
	};

	for( asUINT c = 0; c < sizeof(configs)/sizeof(configs[0]); c++ )
	{
		asIScriptEngine *engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
		std::string expected = Inlining::Run(engine, 0, configs[c], fail);
		engine->ShutDownAndRelease();

		for( asUINT v = 0; v < sizeof(variants)/sizeof(variants[0]); v++ )
		{
			// The JIT compiler must outlive the engine
			CJITCompiler jit(flags | variants[v].flags);
			jit.SetMaxInlineSize(variants[v].maxInlineSize);
			if( variants[v].bailCount )
				jit.SetBailInstructions(variants[v].bails, variants[v].bailCount);
			engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
			std::string actual = Inlining::Run(engine, &jit, configs[c], fail);
			engine->ShutDownAndRelease();

			SJITStatistics stats = jit.GetStatistics();
			bool inlined = inlines && (variants[v].flags & CJITCompiler::JIT_NO_INLINE) == 0;
			if( stats.functionsCompiled == 0 || stats.functionsFailed != 0 || (stats.callsInlined != 0) != inlined )
			{
				PRINTF("%s, %s: %u functions compiled, %u failed, %u calls inlined\n", configs[c].name, variants[v].name,
				       stats.functionsCompiled, stats.functionsFailed, stats.callsInlined);
				TEST_FAILED;
			}
			if( actual != expected )
			{
				// Show where the cases differ
				std::stringstream e(expected), a(actual);
				std::string el, al;
				while( std::getline(e, el) && std::getline(a, al) )
				{
					if( el == al )
						continue;
					size_t p = 0;
					while( p < el.size() && p < al.size() && el[p] == al[p] )
						p++;
					p = p > 100 ? p - 100 : 0;
					PRINTF("%s, %s:\n  VM:  %s\n  JIT: %s\n", configs[c].name, variants[v].name,
					       el.substr(p, 300).c_str(), al.substr(p, 300).c_str());
				}
				TEST_FAILED;
			}
		}
	}

	fail = TestSuspendFromThread(flags, inlines, false) || fail;
	fail = TestSuspendFromThread(flags, inlines, true) || fail;
	fail = TestInlinedCalls(flags, inlines) || fail;
	return fail;
}

// C++ exceptions thrown by registered functions are turned into script exceptions.
// When the functions are called directly the exceptions pass through the generated
// code, and everything that the application can observe must be the same as with
// the VM
namespace CppExceptions
{
	using namespace DirectCalls;

	static std::stringstream g_trace;

	static int    ThrowIf(int a)          { if( a < 0 ) throw std::runtime_error("negative"); return a; }
	static int    ThrowUnknown(int a)     { if( a < 0 ) throw a; return a; }
	static int   &AtThrow(int i, CObj *o) { if( i < 0 ) throw std::out_of_range("index"); return o->arr[i]; }
	static CRef  *RefThrow(int v)         { if( v < 0 ) throw std::runtime_error("handle"); return new CRef(v); }
	static CVal   ValThrow(int v)         { CVal r; r.v = v; if( v < 0 ) throw std::runtime_error("value"); return r; }
	static double DblThrow(double d)      { if( d < 0 ) throw std::runtime_error("double"); return d * 2; }

	static void Translate(asIScriptContext *ctx, void *)
	{
		try { throw; }
		catch( std::exception &e ) { ctx->SetException(e.what()); }
		catch( ... ) {}
	}

	static void OnException(asIScriptContext *ctx, void *)
	{
		g_trace << "[" << ctx->GetExceptionString() << " in " << ctx->GetExceptionFunction()->GetName() << ":" <<
		           ctx->GetExceptionLineNumber() << " depth " << ctx->GetCallstackSize() << "] ";
	}

	static const char *script =
		"int dtors = 0;                                                                    \n"
		"class D { int v; D(int a) { v = a; } ~D() { dtors += v; } }                       \n"
		"int direct(int a) { D d(1); int r = ThrowIf(a); return r + 1; }                   \n"
		"int nested(int n) { D d(1); if( n == 0 ) return ThrowIf(-1); return nested(n - 1) + 1; } \n"
		"int caught(int n) { int r = 0; try { r = nested(n); } catch { r = -100; } return r + ThrowIf(5); } \n"
		"int loop(int n) { int r = 0; for( int i = 0; i < n; i++ ) { try { r += ThrowIf(i % 3 == 0 ? -1 : i); } catch { r += 100; } } return r; } \n"
		"int unknown(int a) { return ThrowUnknown(a); }                                    \n"
		"int thiscall1(int i) { return obj.AtThrow(i); }                                   \n"
		"int method(int a) { obj.v = 1; return obj.Throw(a); }                             \n"
		"int handle(int a) { CRef@ r = RefThrow(a); return r is null ? -1 : r.v; }         \n"
		"int value(int a) { val v = ValThrow(a); return v.v; }                             \n"
		"int valueCaught(int a) { int r = 0; try { val v = ValThrow(a); r = v.v; } catch { r = -1; } return r + 1; } \n"
		"int dbl(int a) { return int(DblThrow(a)); }                                       \n"
		"int args(int a) { return Cdecl(ThrowIf(a), 4); }                                  \n";

	struct SCase { const char *decl; int arg; };
	static const SCase cases[] =
	{
		{ "int direct(int)",      -1 },
		{ "int direct(int)",       3 },
		{ "int nested(int)",       5 },
		{ "int nested(int)",     300 },
		{ "int caught(int)",       5 },
		{ "int caught(int)",     300 },
		{ "int loop(int)",        10 },
		{ "int unknown(int)",     -1 },
		{ "int thiscall1(int)",   -1 },
		{ "int thiscall1(int)",    1 },
		{ "int method(int)",      -1 },
		{ "int method(int)",       2 },
		{ "int handle(int)",      -1 },
		{ "int handle(int)",       4 },
		{ "int value(int)",       -1 },
		{ "int value(int)",        6 },
		{ "int valueCaught(int)", -1 },
		{ "int dbl(int)",         -1 },
		{ "int dbl(int)",          2 },
		{ "int args(int)",        -1 },
	};

	// Executes all the cases and returns what was observed, one line per case. With
	// host, the functions are called through the methods of the JIT compiler
	static std::string Run(asIScriptEngine *engine, CJITCompiler *jit, bool host, bool &fail)
	{
		COutStream out;
		engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, jit != 0);
		engine->SetJITCompiler(jit);
		engine->SetTranslateAppExceptionCallback(asFUNCTION(Translate), 0, asCALL_CDECL);
		DirectCalls::Register(engine);
		int r;
		r = engine->RegisterGlobalFunction("int ThrowIf(int)", asFUNCTION(ThrowIf), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("int ThrowUnknown(int)", asFUNCTION(ThrowUnknown), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "int &AtThrow(int)", asFUNCTION(AtThrow), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectMethod("CObj", "int Throw(int)", asMETHOD(CObj, Throw), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("CRef@ RefThrow(int)", asFUNCTION(RefThrow), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("val ValThrow(int)", asFUNCTION(ValThrow), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("double DblThrow(double)", asFUNCTION(DblThrow), asCALL_CDECL); assert( r >= 0 );

		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", script);
		if( mod->Build() < 0 )
			TEST_FAILED;
		int *dtors = (int*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("dtors"));

		std::string result;
		asIScriptContext *ctx = engine->CreateContext();
		ctx->SetExceptionCallback(asFUNCTION(OnException), 0, asCALL_CDECL);
		for( asUINT n = 0; n < sizeof(cases)/sizeof(cases[0]); n++ )
		{
			g_trace.str("");
			*dtors = 0;
			g_live = 0;
			g_destroyed = 0;

			asIScriptFunction *func = mod->GetFunctionByDecl(cases[n].decl);
			if( func == 0 ) { TEST_FAILED; continue; }
			if( host )
				jit->Prepare(ctx, func);
			else
				ctx->Prepare(func);
			ctx->SetArgDWord(0, cases[n].arg);
			r = host ? jit->Execute(ctx) : ctx->Execute();

			std::stringstream s;
			s << cases[n].decl << "(" << cases[n].arg << "): " << r;
			if( r == asEXECUTION_FINISHED )
				s << " returned " << int(ctx->GetReturnDWord());
			else if( r == asEXECUTION_EXCEPTION )
				s << " '" << ctx->GetExceptionString() << "' in " << ctx->GetExceptionFunction()->GetName() << ":" << ctx->GetExceptionLineNumber();
			ctx->Unprepare();
			s << " dtors " << *dtors << " live " << g_live << " destroyed " << g_destroyed << " " << g_trace.str() << "\n";
			result += s.str();
		}
		ctx->Release();
		return result;
	}
}

static bool TestCppExceptions()
{
	using namespace CppExceptions;
	bool fail = false;

	if( strstr(asGetLibraryOptions(), "AS_NO_EXCEPTIONS") )
		return false;

	asDWORD envFlags = 0;
	const char *env = getenv("AS_JIT_FLAGS");
	if( env )
		envFlags = asDWORD(strtoul(env, 0, 0)) & ~asDWORD(CJITCompiler::JIT_LOG);

	asIScriptEngine *engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
	std::string expected = Run(engine, 0, false, fail);
	engine->ShutDownAndRelease();
	if( expected.find("'negative' in direct:3") == std::string::npos )
		TEST_FAILED;

	// The default, a native call depth that mixes native calls with calls made by
	// the VM, and the calls through the engine. The application calls the functions
	// through the context, or through the JIT compiler which enters the compiled
	// code directly
	struct SConfig { const char *name; asDWORD flags; asUINT depth; bool host; };
	const SConfig configs[] =
	{
		{ "default",                 0,                                        256, false },
		{ "native call depth 3",     0,                                        3,   false },
		{ "no direct system calls",  CJITCompiler::JIT_NO_DIRECT_SYSTEM_CALLS, 256, false },
		{ "no native script calls",  CJITCompiler::JIT_NO_SCRIPT_CALLS,        256, false },
		{ "host calls",              0,                                        256, true },
		{ "host calls, native call depth 3", 0,                                3,   true },
		{ "host calls, no direct system calls", CJITCompiler::JIT_NO_DIRECT_SYSTEM_CALLS, 256, true },
	};
	for( asUINT c = 0; c < sizeof(configs)/sizeof(configs[0]); c++ )
	{
		// The JIT compiler must outlive the engine
		CJITCompiler jit(configs[c].flags | envFlags);
		jit.SetNativeCallDepth(configs[c].depth);
		engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
		std::string actual = Run(engine, &jit, configs[c].host, fail);
		engine->ShutDownAndRelease();

		SJITStatistics stats = jit.GetStatistics();
		if( stats.functionsCompiled == 0 || stats.functionsFailed != 0 )
		{
			PRINTF("%s: %u functions compiled, %u failed\n", configs[c].name, stats.functionsCompiled, stats.functionsFailed);
			TEST_FAILED;
		}
		if( actual != expected )
		{
			std::stringstream e(expected), a(actual);
			std::string el, al;
			while( std::getline(e, el) && std::getline(a, al) )
				if( el != al )
					PRINTF("%s:\n  VM:  %s\n  JIT: %s\n", configs[c].name, el.substr(0, 300).c_str(), al.substr(0, 300).c_str());
			TEST_FAILED;
		}
	}

	return fail;
}

// The AddRef and Release behaviours called by asBC_FREE, asBC_REFCPY, and asBC_RefCpyV.
// They are called directly from the generated code when possible, and everything
// that the application can observe must be the same as with the VM
namespace RefCounting
{
	static std::stringstream g_trace;
	static int g_live = 0;

	// An application type whose reference counting is traced. The behaviours of each
	// registered type have another calling convention
	class CRC
	{
	public:
		CRC(char kind, int id) : refCount(1), id(id), kind(kind) { g_live++; }
		virtual ~CRC() { g_trace << "~" << kind << id << " "; g_live--; }
		void Add() { g_trace << "+" << kind << id << " "; refCount++; }
		void Rel() { g_trace << "-" << kind << id << " "; if( --refCount == 0 ) delete this; }
		// Overridden by CVirt, whose objects must get the calls of the overrides
		virtual void VAdd() { g_trace << "base "; Add(); }
		virtual void VRel() { g_trace << "base "; Rel(); }
		int refCount, id;
		char kind;
	};
	class CVirt : public CRC
	{
	public:
		CVirt(int id) : CRC('V', id) {}
		void VAdd() { Add(); }
		void VRel() { Rel(); }
	};
	static CRC g_noCount('N', 0);

	static CRC *MakeR(int id)   { return new CRC('R', id); }
	static CRC *MakeV(int id)   { return new CVirt(id); }
	static CRC *MakeL(int id)   { return new CRC('L', id); }
	static CRC *MakeF(int id)   { return new CRC('F', id); }
	static CRC *MakeG(int id)   { return new CRC('G', id); }
	static CRC *GetN()          { return &g_noCount; }
	static void AddObj(CRC *o)  { o->Add(); }
	static void RelObj(CRC *o)  { o->Rel(); }
	static void AddGen(asIScriptGeneric *gen) { ((CRC*)gen->GetObject())->Add(); }
	static void RelGen(asIScriptGeneric *gen) { ((CRC*)gen->GetObject())->Rel(); }
	static void Mark(int v)     { g_trace << "m" << v << " "; }

	static void RegisterType(asIScriptEngine *engine, const char *name, asDWORD flags)
	{
		int r = engine->RegisterObjectType(name, 0, flags); assert( r >= 0 );
		r = engine->RegisterObjectProperty(name, "int id", asOFFSET(CRC, id)); assert( r >= 0 );
	}

	static const char *script =
		"class Node                                                               \n"
		"{                                                                        \n"
		"  int id;                                                                \n"
		"  Node@ next;                                                            \n"
		"  Node(int i) { id = i; Mark(i); }                                       \n"
		"  ~Node() { Mark(-id); if( id == 3 ) @g_chain = null; }                  \n"
		"}                                                                        \n"
		"Node@ g_chain;                                                           \n"
		"Node@ g_keep;                                                            \n"
		"R@ g_r;                                                                  \n"
		"funcdef int FN(int);                                                     \n"
		"int twice(int a) { return a * 2; }                                       \n"
		// The releases execute the destructors on the same context, the one of
		// node 3 releases another node, and the list is destroyed recursively
		"int scriptObjects(int n)                                                 \n"
		"{                                                                        \n"
		"  Node@ a = Node(1);                                                     \n"
		"  Node@ b = a;                                                           \n"
		"  @b = Node(2);                                                          \n"
		"  @a = b;                                                                \n"
		"  @a.next = Node(3);                                                     \n"
		"  @g_chain = Node(4);                                                    \n"
		"  @g_keep = a;                                                           \n"
		"  @a = null;                                                             \n"
		"  @b = null;                                                             \n"
		"  @g_keep = null;                                                        \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"  {                                                                      \n"
		"    Node@ t = Node(10 + i);                                              \n"
		"    @t.next = g_keep;                                                    \n"
		"    @g_keep = t;                                                         \n"
		"  }                                                                      \n"
		"  int r = 0;                                                             \n"
		"  Node@ p = g_keep;                                                      \n"
		"  while( p !is null ) { r += p.id; @p = p.next; }                        \n"
		"  @g_keep = null;                                                        \n"
		"  return r;                                                              \n"
		"}                                                                        \n"
		"int appTypes(int n)                                                      \n"
		"{                                                                        \n"
		"  R@ r1 = MakeR(1); R@ r2 = r1;                                          \n"
		"  V@ v1 = MakeV(2); V@ v2 = v1;                                          \n"
		"  L@ l1 = MakeL(3); L@ l2 = l1;                                          \n"
		"  F@ f1 = MakeF(4); F@ f2 = f1;                                          \n"
		"  G@ g1 = MakeG(5); G@ g2 = g1;                                          \n"
		"  N@ n1 = GetN();   N@ n2 = n1;                                          \n"
		"  @r2 = null; @v2 = null; @l2 = null; @f2 = null; @g2 = null; @n2 = null; \n"
		"  @g_r = r1;                                                             \n"
		"  int s = 0;                                                             \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"  {                                                                      \n"
		"    R@ tr = r1; V@ tv = v1; L@ tl = l1; F@ tf = f1; G@ tg = g1; N@ tn = n1; \n"
		"    @tr = g_r;                                                           \n"
		"    s += tr.id + tv.id + tl.id + tf.id + tg.id + tn.id;                  \n"
		"  }                                                                      \n"
		"  @g_r = null;                                                           \n"
		"  return s;                                                              \n"
		"}                                                                        \n"
		"int funcHandles(int n)                                                   \n"
		"{                                                                        \n"
		"  FN@ f = twice;                                                         \n"
		"  FN@ g = f;                                                             \n"
		"  @f = null;                                                             \n"
		"  int r = 0;                                                             \n"
		"  for( int i = 0; i < n; i++ ) { FN@ t = g; r += t(i); }                 \n"
		"  return r;                                                              \n"
		"}                                                                        \n"
		// The handles in the temporary variables are moved, also over the same
		// object, and the releases of the old objects execute the destructors
		"R@ idR(R@ r) { return r; }                                               \n"
		"Node@ idNode(Node@ n) { Mark(100 + n.id); return n; }                    \n"
		"int moves(int n)                                                         \n"
		"{                                                                        \n"
		"  R@ a = MakeR(6);                                                       \n"
		"  R@ b = idR(a);                                                         \n"
		"  @a = idR(a);                                                           \n"
		"  @b = MakeR(7);                                                         \n"
		"  @b = idR(b);                                                           \n"
		"  Node@ p = Node(30);                                                    \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"  {                                                                      \n"
		"    @p = Node(31 + i);                                                   \n"
		"    @p = idNode(p);                                                      \n"
		"    if( i % 3 == 0 ) @b = MakeR(40 + i);                                 \n"
		"  }                                                                      \n"
		"  @p = null;                                                             \n"
		"  return a.id + b.id;                                                    \n"
		"}                                                                        \n";

	// The JIT leaves out the references that the handles moved from temporary
	// variables would add and release right after, see CJITByteCode::FindMovedRefs,
	// so the traces are compared without the calls of AddRef that are followed by a
	// release of the same object
	static std::string Collapse(const std::string &trace, size_t &refs)
	{
		std::stringstream in(trace);
		std::vector<std::string> kept;
		std::string t;
		refs = 0;
		while( in >> t )
		{
			if( t[0] == '+' || t[0] == '-' )
				refs++;
			if( t[0] == '-' && !kept.empty() && kept.back() == "+" + t.substr(1) )
				kept.pop_back();
			else
				kept.push_back(t);
		}
		std::string out;
		for( asUINT n = 0; n < kept.size(); n++ )
			out += kept[n] + " ";
		return out;
	}

	// Executes the functions and returns what was observed, one line per function
	static std::string Run(asIScriptEngine *engine, CJITCompiler *jit, bool &fail)
	{
		COutStream out;
		engine->SetMessageCallback(asMETHOD(COutStream, Callback), &out, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, jit != 0);
		engine->SetJITCompiler(jit);
		int r;
		RegisterType(engine, "R", asOBJ_REF);
		r = engine->RegisterObjectBehaviour("R", asBEHAVE_ADDREF, "void f()", asMETHOD(CRC, Add), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("R", asBEHAVE_RELEASE, "void f()", asMETHOD(CRC, Rel), asCALL_THISCALL); assert( r >= 0 );
		RegisterType(engine, "V", asOBJ_REF);
		r = engine->RegisterObjectBehaviour("V", asBEHAVE_ADDREF, "void f()", asMETHOD(CRC, VAdd), asCALL_THISCALL); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("V", asBEHAVE_RELEASE, "void f()", asMETHOD(CRC, VRel), asCALL_THISCALL); assert( r >= 0 );
		RegisterType(engine, "L", asOBJ_REF);
		r = engine->RegisterObjectBehaviour("L", asBEHAVE_ADDREF, "void f()", asFUNCTION(AddObj), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("L", asBEHAVE_RELEASE, "void f()", asFUNCTION(RelObj), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		RegisterType(engine, "F", asOBJ_REF);
		r = engine->RegisterObjectBehaviour("F", asBEHAVE_ADDREF, "void f()", asFUNCTION(AddObj), asCALL_CDECL_OBJFIRST); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("F", asBEHAVE_RELEASE, "void f()", asFUNCTION(RelObj), asCALL_CDECL_OBJFIRST); assert( r >= 0 );
		RegisterType(engine, "G", asOBJ_REF);
		r = engine->RegisterObjectBehaviour("G", asBEHAVE_ADDREF, "void f()", asFUNCTION(AddGen), asCALL_GENERIC); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("G", asBEHAVE_RELEASE, "void f()", asFUNCTION(RelGen), asCALL_GENERIC); assert( r >= 0 );
		RegisterType(engine, "N", asOBJ_REF | asOBJ_NOCOUNT);
		r = engine->RegisterGlobalFunction("R@ MakeR(int)", asFUNCTION(MakeR), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("V@ MakeV(int)", asFUNCTION(MakeV), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("L@ MakeL(int)", asFUNCTION(MakeL), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("F@ MakeF(int)", asFUNCTION(MakeF), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("G@ MakeG(int)", asFUNCTION(MakeG), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("N@ GetN()", asFUNCTION(GetN), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("void Mark(int)", asFUNCTION(Mark), asCALL_CDECL); assert( r >= 0 );

		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", script);
		if( mod->Build() < 0 )
			TEST_FAILED;

		const char *funcs[] = { "int scriptObjects(int)", "int appTypes(int)", "int funcHandles(int)", "int moves(int)" };
		std::string result;
		asIScriptContext *ctx = engine->CreateContext();
		for( asUINT n = 0; n < sizeof(funcs)/sizeof(funcs[0]); n++ )
		{
			g_trace.str("");
			asIScriptFunction *func = mod->GetFunctionByDecl(funcs[n]);
			if( func == 0 ) { TEST_FAILED; continue; }
			ctx->Prepare(func);
			ctx->SetArgDWord(0, 20);
			r = ctx->Execute();

			std::stringstream s;
			s << funcs[n] << ": " << r;
			if( r == asEXECUTION_FINISHED )
				s << " returned " << int(ctx->GetReturnDWord());
			ctx->Unprepare();
			s << " live " << g_live << " " << g_trace.str() << "\n";
			result += s.str();
		}
		ctx->Release();
		return result;
	}
}

static bool TestRefCounting()
{
	using namespace RefCounting;
	bool fail = false;

	asDWORD envFlags = 0;
	const char *env = getenv("AS_JIT_FLAGS");
	if( env )
		envFlags = asDWORD(strtoul(env, 0, 0)) & ~asDWORD(CJITCompiler::JIT_LOG);

	g_live = 0;
	asIScriptEngine *engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
	std::string expected = Run(engine, 0, fail);
	engine->ShutDownAndRelease();
	if( expected.find("-V2 ~V2") == std::string::npos || expected.find("base") != std::string::npos || g_live != 0 )
		TEST_FAILED;

	struct SConfig { const char *name; asDWORD flags; };
	const SConfig configs[] =
	{
		{ "default",                 0 },
		{ "no direct system calls",  CJITCompiler::JIT_NO_DIRECT_SYSTEM_CALLS },
		{ "no native script calls",  CJITCompiler::JIT_NO_SCRIPT_CALLS },
	};
	for( asUINT c = 0; c < sizeof(configs)/sizeof(configs[0]); c++ )
	{
		// The JIT compiler must outlive the engine
		CJITCompiler jit(configs[c].flags | envFlags);
		engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
		std::string actual = Run(engine, &jit, fail);
		engine->ShutDownAndRelease();
		if( g_live != 0 )
			TEST_FAILED;

		// The handles are moved in every configuration
		size_t vmRefs, jitRefs;
		std::string collapsedVM = Collapse(expected, vmRefs), collapsedJIT = Collapse(actual, jitRefs);
		if( jitRefs >= vmRefs )
		{
			PRINTF("%s: %u calls of AddRef and Release, the VM makes %u\n", configs[c].name, unsigned(jitRefs), unsigned(vmRefs));
			TEST_FAILED;
		}

		SJITStatistics stats = jit.GetStatistics();
		if( stats.functionsCompiled == 0 || stats.functionsFailed != 0 )
		{
			PRINTF("%s: %u functions compiled, %u failed\n", configs[c].name, stats.functionsCompiled, stats.functionsFailed);
			TEST_FAILED;
		}
		if( collapsedJIT != collapsedVM )
		{
			std::stringstream e(expected), a(actual);
			std::string el, al;
			while( std::getline(e, el) && std::getline(a, al) )
				if( el != al )
					PRINTF("%s:\n  VM:  %s\n  JIT: %s\n", configs[c].name, el.substr(0, 300).c_str(), al.substr(0, 300).c_str());
			TEST_FAILED;
		}
	}

	return fail;
}

// The references of the script objects are counted in place, see
// CJITByteCode::FindInPlaceRefCounts. The flag of the GC must be cleared, the
// objects resurrected while they are destroyed must be reported, and the counts of
// the objects shared by several threads must be changed atomically
namespace ScriptRefCounts
{
	static std::stringstream g_trace;
	static void Mark(int v) { g_trace << v << " "; }

	// The object that is being destroyed is written into the global handle without
	// a reference, for the script to copy it
	static void *g_raw = 0;
	static void **g_dying = 0;
	static void Keep(void *ref, int typeId) { g_raw = (typeId & asTYPEID_OBJHANDLE) ? *(void**)ref : ref; }
	static void Expose()   { *g_dying = g_raw; }
	static void Unexpose() { *g_dying = 0; }

	static const char *script =
		"class Node                                                               \n"
		"{                                                                        \n"
		"  int id;                                                                \n"
		"  Node@ other;                                                           \n"
		"  Node(int i) { id = i; }                                                \n"
		"  ~Node() { Mark(-id); }                                                 \n"
		"}                                                                        \n"
		"Node@ g_ext;                                                             \n"
		"void makeCycle()                                                         \n"
		"{                                                                        \n"
		"  Node@ a = Node(1);                                                     \n"
		"  Node@ b = Node(2);                                                     \n"
		"  @a.other = b;                                                          \n"
		"  @b.other = a;                                                          \n"
		"  @g_ext = a;                                                            \n"
		"}                                                                        \n"
		// Moves the reference from outside of the cycle to the other object, which
		// the GC must see even if it has counted the references of the object before
		"int flip()                                                               \n"
		"{                                                                        \n"
		"  @g_ext = g_ext.other;                                                  \n"
		"  return g_ext.other.other is g_ext ? g_ext.id : -1;                     \n"
		"}                                                                        \n"
		"void dropCycle() { @g_ext = null; }                                      \n"
		// The child sees the parent being destroyed and copies a handle to it,
		// which the engine reports
		"final class Child                                                        \n"
		"{                                                                        \n"
		"  ~Child()                                                               \n"
		"  {                                                                      \n"
		"    expose();                                                            \n"
		"    Parent@ q = g_dying;                                                 \n"
		"    Mark(q.id);                                                          \n"
		"    @q = null;                                                           \n"
		"    unexpose();                                                          \n"
		"  }                                                                      \n"
		"}                                                                        \n"
		"final class Parent                                                       \n"
		"{                                                                        \n"
		"  int id;                                                                \n"
		"  Child@ c = Child();                                                    \n"
		"  Parent(int i) { id = i; }                                              \n"
		"  ~Parent() { Mark(-id); }                                               \n"
		"}                                                                        \n"
		"Parent@ g_dying;                                                         \n"
		"void resurrect()                                                         \n"
		"{                                                                        \n"
		"  Parent@ p = Parent(7);                                                 \n"
		"  keep(@p);                                                              \n"
		"  @p = null;                                                             \n"
		"  Mark(0);                                                               \n"
		"}                                                                        \n"
		"Node@ g_shared = Node(5);                                                \n"
		"int spin(int n)                                                          \n"
		"{                                                                        \n"
		"  int s = 0;                                                             \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"  {                                                                      \n"
		"    Node@ a = g_shared;                                                  \n"
		"    Node@ b = a;                                                         \n"
		"    s += b.id;                                                           \n"
		"    @a = null;                                                           \n"
		"  }                                                                      \n"
		"  return s;                                                              \n"
		"}                                                                        \n";

	static const int SPIN_THREADS = 4;
	static const int SPIN_COUNT   = 100000;
	static std::mutex g_lock;

	// The memory functions of the tests aren't thread safe, so everything that
	// allocates is done under the lock, including the data of the thread that the
	// first execution allocates. The copies of the handles in spin only change the
	// reference count of the shared object
	static void Spin(asIScriptEngine *engine, asIScriptFunction *func, std::atomic<int> *ready, int *result)
	{
		asIScriptContext *ctx;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			ctx = engine->CreateContext();
			ctx->Prepare(func);
			ctx->SetArgDWord(0, 1);
			ctx->Execute();
			ctx->Prepare(func);
			ctx->SetArgDWord(0, SPIN_COUNT);
		}
		(*ready)++;
		while( *ready < SPIN_THREADS )
			std::this_thread::yield();
		*result = ctx->Execute() == asEXECUTION_FINISHED ? int(ctx->GetReturnDWord()) : -1;

		std::lock_guard<std::mutex> lock(g_lock);
		ctx->Release();
		asThreadCleanup();
	}

	// Returns what was observed
	static std::string Run(asIScriptEngine *engine, CJITCompiler *jit, bool &fail)
	{
		CBufferedOutStream msgs;
		engine->SetMessageCallback(asMETHOD(CBufferedOutStream, Callback), &msgs, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, jit != 0);
		engine->SetJITCompiler(jit);
		int r;
		r = engine->RegisterGlobalFunction("void Mark(int)", asFUNCTION(Mark), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("void keep(?&in)", asFUNCTION(Keep), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("void expose()", asFUNCTION(Expose), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("void unexpose()", asFUNCTION(Unexpose), asCALL_CDECL); assert( r >= 0 );

		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", script);
		if( mod->Build() < 0 )
		{
			PRINTF("%s", msgs.buffer.c_str());
			TEST_FAILED;
			return "";
		}
		g_dying = (void**)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("g_dying"));
		std::stringstream s;
		asIScriptContext *ctx = engine->CreateContext();

		// The GC counts the references of the objects of the cycle in separate steps,
		// and the flips in between must keep them alive. Once or twice per step, so
		// that the steps see either object referenced from outside
		g_trace.str("");
		ctx->Prepare(mod->GetFunctionByDecl("void makeCycle()"));
		ctx->Execute();
		engine->GarbageCollect(asGC_FULL_CYCLE);
		asIScriptFunction *flip = mod->GetFunctionByDecl("int flip()");
		int flips = 0, wrong = 0, expect = 2;
		unsigned rnd = 1;
		for( int n = 0; n < 1000; n++ )
		{
			engine->GarbageCollect(asGC_ONE_STEP | asGC_DETECT_GARBAGE);
			rnd = rnd * 1103515245 + 12345;
			for( unsigned k = (rnd >> 16) & 1; k < 2; k++ )
			{
				ctx->Prepare(flip);
				r = ctx->Execute();
				if( r != asEXECUTION_FINISHED || int(ctx->GetReturnDWord()) != expect )
					wrong++;
				expect = 3 - expect;
				flips++;
			}
		}
		s << "flips " << flips << " wrong " << wrong << " destroyed " << g_trace.str() << "\n";
		if( wrong != 0 || g_trace.str() != "" )
			TEST_FAILED;
		ctx->Prepare(mod->GetFunctionByDecl("void dropCycle()"));
		ctx->Execute();
		engine->GarbageCollect(asGC_FULL_CYCLE);
		if( g_trace.str() != "-1 -2 " && g_trace.str() != "-2 -1 " )
			TEST_FAILED;

		g_trace.str("");
		msgs.buffer = "";
		ctx->Prepare(mod->GetFunctionByDecl("void resurrect()"));
		r = ctx->Execute();
		s << "resurrect " << r << " " << g_trace.str() << msgs.buffer;

		// The contexts run a step of the GC when they finish, which allocates and holds
		// a reference of the shared object from one step to another, so the GC is
		// left out while the threads spin
		asIScriptObject *shared = *(asIScriptObject**)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("g_shared"));
		engine->SetEngineProperty(asEP_AUTO_GARBAGE_COLLECT, false);
		int before = shared->AddRef();
		shared->Release();
		std::atomic<int> ready(0);
		int results[SPIN_THREADS];
		// Looking up the function allocates, so it is done before the threads start
		asIScriptFunction *spin = mod->GetFunctionByDecl("int spin(int)");
		std::vector<std::thread> threads;
		for( int n = 0; n < SPIN_THREADS; n++ )
			threads.push_back(std::thread(Spin, engine, spin, &ready, &results[n]));
		for( int n = 0; n < SPIN_THREADS; n++ )
			threads[n].join();
		int after = shared->AddRef();
		shared->Release();
		engine->SetEngineProperty(asEP_AUTO_GARBAGE_COLLECT, true);
		s << "spin";
		for( int n = 0; n < SPIN_THREADS; n++ )
		{
			s << " " << results[n];
			if( results[n] != 5 * SPIN_COUNT )
				TEST_FAILED;
		}
		s << " references " << after - before << "\n";
		if( after != before )
			TEST_FAILED;

		ctx->Release();
		return s.str();
	}
}

static bool TestScriptRefCounts()
{
	using namespace ScriptRefCounts;
	bool fail = false;

	asDWORD envFlags = 0;
	const char *env = getenv("AS_JIT_FLAGS");
	if( env )
		envFlags = asDWORD(strtoul(env, 0, 0)) & ~asDWORD(CJITCompiler::JIT_LOG);

	asIScriptEngine *engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
	std::string expected = Run(engine, 0, fail);
	engine->ShutDownAndRelease();
	if( expected.find("resurrect 0 -7 7 0  (0, 0) : Error   : The script object of type 'Parent' is being resurrected illegally during destruction") == std::string::npos )
		TEST_FAILED;

	// The JIT compiler must outlive the engine
	CJITCompiler jit(envFlags);
	engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
	std::string actual = Run(engine, &jit, fail);
	engine->ShutDownAndRelease();

	SJITStatistics stats = jit.GetStatistics();
	if( stats.functionsCompiled == 0 || stats.functionsFailed != 0 )
	{
		PRINTF("script reference counts: %u functions compiled, %u failed\n", stats.functionsCompiled, stats.functionsFailed);
		TEST_FAILED;
	}
	if( actual != expected )
	{
		PRINTF("script reference counts:\n  VM:\n%s  JIT:\n%s", expected.c_str(), actual.c_str());
		TEST_FAILED;
	}

	return fail;
}

// The initialization lists with nothing to destroy in them are freed without going
// through the list, see CJITByteCode::FindListFrees. Nothing may be leaked, and the
// values in the other lists must still be destroyed
namespace ListFrees
{
	struct SVec2 { float x, y; };
	static void Vec2Construct(SVec2 *v) { v->x = 0; v->y = 0; }
	static void Vec2Init(float x, float y, SVec2 *v) { v->x = x; v->y = y; }
	static void Vec2List(float *list, SVec2 *v) { v->x = list[0]; v->y = list[1]; }

	// The engine only destroys the values in the lists whose bytes aren't all zero
	static int g_live = 0;
	struct STracked { int v, mark; };
	static void TrackedConstruct(STracked *t) { t->v = 0; t->mark = 1; g_live++; }
	static void TrackedInit(int v, STracked *t) { t->v = v; t->mark = 1; g_live++; }
	static void TrackedDestruct(STracked *t) { t->mark = 0; g_live--; }
	static STracked &TrackedAssign(const STracked &o, STracked *t) { t->v = o.v; return *t; }

	static const char *script =
		"enum Color { Red, Green, Blue }                                          \n"
		"int ints(int n)                                                          \n"
		"{                                                                        \n"
		"  int s = 0;                                                             \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"  {                                                                      \n"
		"    array<int> a = {1, 2, 3, i};                                         \n"
		"    s += a[3] + int(a.length());                                         \n"
		"  }                                                                      \n"
		"  return s;                                                              \n"
		"}                                                                        \n"
		"int colors(int n)                                                        \n"
		"{                                                                        \n"
		"  int s = 0;                                                             \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"  {                                                                      \n"
		"    array<Color> a = {Red, Blue, Color(i % 3)};                          \n"
		"    s += int(a[1]) + int(a[2]);                                          \n"
		"  }                                                                      \n"
		"  return s;                                                              \n"
		"}                                                                        \n"
		"int vecs(int n)                                                          \n"
		"{                                                                        \n"
		"  int s = 0;                                                             \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"  {                                                                      \n"
		"    array<Vec2> a = {Vec2(1, 2), Vec2(i, 3)};                            \n"
		"    Vec2 v = {float(i), 4.0f};                                           \n"
		"    s += int(a[1].x + a[0].y + v.x + v.y);                               \n"
		"  }                                                                      \n"
		"  return s;                                                              \n"
		"}                                                                        \n"
		"int pair(int i) { array<int> a = {i, 1}; return a[0] + a[1]; }           \n"
		"int inlined(int n)                                                       \n"
		"{                                                                        \n"
		"  int s = 0;                                                             \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"    s += pair(i);                                                        \n"
		"  return s;                                                              \n"
		"}                                                                        \n"
		"int nested(int n)                                                        \n"
		"{                                                                        \n"
		"  int s = 0;                                                             \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"  {                                                                      \n"
		"    array<array<int>> a = {{1, 2}, {i}};                                 \n"
		"    s += a[1][0] + int(a[0].length());                                   \n"
		"  }                                                                      \n"
		"  return s;                                                              \n"
		"}                                                                        \n"
		"int strings(int n)                                                       \n"
		"{                                                                        \n"
		"  int s = 0;                                                             \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"  {                                                                      \n"
		"    array<string> a = {\"a\", \"bc\"};                                   \n"
		"    s += int(a[1].length()) + i;                                         \n"
		"  }                                                                      \n"
		"  return s;                                                              \n"
		"}                                                                        \n"
		"int tracked(int n)                                                       \n"
		"{                                                                        \n"
		"  int s = 0;                                                             \n"
		"  for( int i = 0; i < n; i++ )                                           \n"
		"  {                                                                      \n"
		"    array<Tracked> a = {Tracked(i), Tracked(2)};                         \n"
		"    s += a[0].v + a[1].v;                                                \n"
		"  }                                                                      \n"
		"  return s;                                                              \n"
		"}                                                                        \n"
		"int divide(int d)                                                        \n"
		"{                                                                        \n"
		"  array<int> a = {1, 2, 10 / d};                                         \n"
		"  array<Tracked> b = {Tracked(3), Tracked(20 / d)};                      \n"
		"  return a[2] + b[1].v;                                                  \n"
		"}                                                                        \n";

	static const char *tests[] = { "ints", "colors", "vecs", "inlined", "nested", "strings", "tracked", "divide" };

	// Returns the results and the memory that the second round of calls leaves allocated
	static std::string Run(asIScriptEngine *engine, CJITCompiler *jit, bool &fail)
	{
		CBufferedOutStream msgs;
		engine->SetMessageCallback(asMETHOD(CBufferedOutStream, Callback), &msgs, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, jit != 0);
		engine->SetJITCompiler(jit);
		RegisterStdString(engine);
		RegisterScriptArray(engine, false);
		int r;
		r = engine->RegisterObjectType("Vec2", sizeof(SVec2), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS | asOBJ_APP_CLASS_ALLFLOATS); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("Vec2", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(Vec2Construct), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("Vec2", asBEHAVE_CONSTRUCT, "void f(float, float)", asFUNCTION(Vec2Init), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("Vec2", asBEHAVE_LIST_CONSTRUCT, "void f(int &in) {float, float}", asFUNCTION(Vec2List), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectProperty("Vec2", "float x", asOFFSET(SVec2, x)); assert( r >= 0 );
		r = engine->RegisterObjectProperty("Vec2", "float y", asOFFSET(SVec2, y)); assert( r >= 0 );
		r = engine->RegisterObjectType("Tracked", sizeof(STracked), asOBJ_VALUE | asOBJ_APP_CLASS_CDAK); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("Tracked", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(TrackedConstruct), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("Tracked", asBEHAVE_CONSTRUCT, "void f(int)", asFUNCTION(TrackedInit), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectBehaviour("Tracked", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(TrackedDestruct), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectMethod("Tracked", "Tracked &opAssign(const Tracked &in)", asFUNCTION(TrackedAssign), asCALL_CDECL_OBJLAST); assert( r >= 0 );
		r = engine->RegisterObjectProperty("Tracked", "int v", asOFFSET(STracked, v)); assert( r >= 0 );

		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", script);
		if( mod->Build() < 0 )
		{
			PRINTF("%s", msgs.buffer.c_str());
			TEST_FAILED;
			return "";
		}

		// The first round allocates what the context keeps for the later executions
		std::stringstream s;
		asIScriptContext *ctx = engine->CreateContext();
		for( asUINT n = 0; n < sizeof(tests) / sizeof(tests[0]); n++ )
		{
			asIScriptFunction *func = mod->GetFunctionByName(tests[n]);
			int results[2] = { 0, 0 };
			int mem = 0;
			for( int round = 0; round < 2; round++ )
			{
				mem = GetAllocedMem();
				for( int k = 0; k < 2; k++ )
				{
					ctx->Prepare(func);
					ctx->SetArgDWord(0, k == 0 ? 100 : 0);
					r = ctx->Execute();
					results[k] = r == asEXECUTION_FINISHED ? int(ctx->GetReturnDWord()) : -r;
				}
			}
			int leaked = GetAllocedMem() - mem;
			s << tests[n] << " " << results[0] << " " << results[1] << " leaked " << leaked << " live " << g_live << "\n";
			if( leaked != 0 || g_live != 0 )
				TEST_FAILED;
		}

		ctx->Release();
		return s.str();
	}
}

static bool TestListFrees()
{
	using namespace ListFrees;
	bool fail = false;

	asDWORD envFlags = 0;
	const char *env = getenv("AS_JIT_FLAGS");
	if( env )
		envFlags = asDWORD(strtoul(env, 0, 0)) & ~asDWORD(CJITCompiler::JIT_LOG);

	asIScriptEngine *engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
	std::string expected = Run(engine, 0, fail);
	engine->ShutDownAndRelease();

	// The JIT compiler must outlive the engine
	CJITCompiler jit(envFlags);
	engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
	std::string actual = Run(engine, &jit, fail);
	engine->ShutDownAndRelease();

	SJITStatistics stats = jit.GetStatistics();
	if( stats.functionsCompiled == 0 || stats.functionsFailed != 0 )
	{
		PRINTF("list frees: %u functions compiled, %u failed\n", stats.functionsCompiled, stats.functionsFailed);
		TEST_FAILED;
	}
	if( actual != expected )
	{
		PRINTF("list frees:\n  VM:\n%s  JIT:\n%s", expected.c_str(), actual.c_str());
		TEST_FAILED;
	}

	return fail;
}

// The calls from the application through CJITCompiler::Prepare and Execute, which
// enter the compiled code directly. Everything that the application can observe
// must be the same as with the methods of the context
namespace HostCalls
{
	// Calls the methods of the JIT compiler, or of the context without a compiler.
	// When mixed, every third call is made through the context
	struct SApi
	{
		CJITCompiler *jit;
		bool mix;
		int calls;
		bool UseJit() { calls++; return jit && !(mix && calls % 3 == 0); }
		int Prepare(asIScriptContext *ctx, asIScriptFunction *func) { return UseJit() ? jit->Prepare(ctx, func) : ctx->Prepare(func); }
		int Execute(asIScriptContext *ctx) { return UseJit() ? jit->Execute(ctx) : ctx->Execute(); }
	};
	static SApi g_api;
	static std::stringstream g_trace;
	static int g_lines = 0;

	static void OnException(asIScriptContext *ctx, void *)
	{
		g_trace << "[" << ctx->GetExceptionString() << " in " << ctx->GetExceptionFunction()->GetName() << ":" <<
		           ctx->GetExceptionLineNumber() << " depth " << ctx->GetCallstackSize() << "] ";
	}

	static void CountLines(asIScriptContext *, void *) { g_lines++; }
	static void Suspend()     { asGetActiveContext()->Suspend(); }
	static void Abort()       { asGetActiveContext()->Abort(); }
	static void Fail()        { asGetActiveContext()->SetException("failed"); }
	static void StartLines()  { asGetActiveContext()->SetLineCallback(asFUNCTION(CountLines), 0, asCALL_CDECL); }
	static int  Triple(int a) { return a * 3; }

	// Calls the script function twice in a nested state of the active context, and
	// returns the sum of the results
	static int Nested(const std::string &decl, int arg)
	{
		asIScriptContext *ctx = asGetActiveContext();
		asIScriptFunction *func = ctx->GetEngine()->GetModule("test")->GetFunctionByDecl(decl.c_str());
		if( ctx->PushState() < 0 )
			return -1000;
		int sum = 0;
		for( int n = 0; n < 2; n++ )
		{
			int r = g_api.Prepare(ctx, func);
			if( r >= 0 )
			{
				ctx->SetArgDWord(0, arg);
				r = g_api.Execute(ctx);
			}
			if( r == asEXECUTION_FINISHED )
				sum += int(ctx->GetReturnDWord());
			g_trace << "(" << decl << " " << arg << ": " << r << ") ";
		}
		ctx->PopState();
		return sum;
	}

	// The objects of G are known to the garbage collector but have no cycles. The
	// collector breaks the cycles in the order of the addresses, which would change
	// how many objects each step destroys between the runs
	static const char *script =
		"interface I { int get(int); }                                                     \n"
		"int dtors = 0;                                                                    \n"
		"int counter = 0;                                                                  \n"
		"class A : I {                                                                     \n"
		"  int v;                                                                          \n"
		"  A(int a) { v = a; }                                                             \n"
		"  ~A() { dtors += v; }                                                            \n"
		"  int get(int a) { return v + a; }                                                \n"
		"  int virt(int a) { return v * a; }                                               \n"
		"  string str(int a) { return 'A' + (v + a); }                                     \n"
		"}                                                                                 \n"
		"class B : A { B(int a) { super(a + 10); } int virt(int a) { return v * a + 1; } } \n"
		"class G { G@ next; int v; G(int a) { v = a; } ~G() { dtors += v; } }             \n"
		"A@ objA = A(3);                                                                   \n"
		"A@ objB = B(4);                                                                   \n"
		"A@ objNull;                                                                       \n"
		"import int unbound(int) from 'other';                                             \n"
		"int add(int a, int b) { return a + b; }                                           \n"
		"double half(double d) { return d / 2; }                                           \n"
		"int64 wide(int64 a) { return a << 33; }                                           \n"
		"string greet(const string &in s, int n) { string r = s; for( int i = 0; i < n; i++ ) r += '!'; return r; } \n"
		"int &counterRef(int a) { counter += a; return counter; }                          \n"
		"A@ make(int a) { if( a % 2 == 0 ) return A(a); return B(a); }                     \n"
		"int div(int a, int b) { return a / b; }                                           \n"
		"int safeDiv(int a, int b) { int r = 0; try { r = a / b; } catch { r = -1; } return r; } \n"
		"int deep(int n) { if( n == 0 ) return triple(1); return 1 + deep(n - 1); }        \n"
		"int objects(int n) { A a(n); A@ b = make(n + 1); return a.get(1) + b.virt(2); }   \n"
		"int garbage(int n) { int r = 0; for( int i = 0; i < n; i++ ) { G g(i); r += g.v; } return r; } \n"
		"int suspends(int n) { int r = 0; for( int i = 0; i < n; i++ ) { suspend(); r += i; } return r; } \n"
		"int aborts(int n) { A a(n); abort(); return n; }                                  \n"
		"int fails(int n) { A a(n); fail(); return n; }                                    \n"
		"int lines(int n) {                                                                \n"
		"  int r = n;                                                                      \n"
		"  startLines();                                                                   \n"
		"  r += 1;                                                                         \n"
		"  r *= 2;                                                                         \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		"int callUnbound(int a) { return unbound(a); }                                     \n"
		"int recurse(int n) { if( n <= 0 ) return 1; return 1 + nested('int recurse(int)', n - 1); } \n"
		"int thrower(int n) { A a(n); int z = 0; return n / z; }                           \n"
		"int outer(int n) { A a(n); return nested('int thrower(int)', n) + nested('int add(int, int)', n) + a.v; } \n";

	// What is done with the function. The methods are called on the object in the
	// global variable, and so are the delegates created for them
	enum EStep { CALL, METHOD, DELEGATE, PREPARE_TWICE, UNPREPARE, EXECUTE_AGAIN };
	struct SStep { EStep step; const char *type; const char *decl; const char *obj; int a, b; };
	static const SStep steps[] =
	{
		{ CALL,          0,        "int add(int, int)",                   0,         1,   2 },
		{ CALL,          0,        "int add(int, int)",                   0,         3,   4 },
		{ CALL,          0,        "double half(double)",                 0,         5,   0 },
		{ CALL,          0,        "int add(int, int)",                   0,         6,   7 },
		{ CALL,          0,        "int64 wide(int64)",                   0,         8,   0 },
		{ CALL,          0,        "string greet(const string &in, int)", 0,         0,   2 },
		{ CALL,          0,        "string greet(const string &in, int)", 0,         0,   3 },
		{ CALL,          0,        "int &counterRef(int)",                0,         6,   0 },
		{ CALL,          0,        "int &counterRef(int)",                0,         1,   0 },
		{ CALL,          0,        "A@ make(int)",                        0,         7,   0 },
		{ CALL,          0,        "A@ make(int)",                        0,         8,   0 },
		{ CALL,          0,        "int add(int, int)",                   0,         9,   9 },
		{ CALL,          0,        "int div(int, int)",                   0,         7,   0 },
		{ CALL,          0,        "int div(int, int)",                   0,         7,   2 },
		{ CALL,          0,        "int safeDiv(int, int)",               0,         7,   0 },
		{ CALL,          0,        "int add(int, int)",                   0,         1,   1 },
		{ CALL,          0,        "int deep(int)",                       0,       500,   0 },
		{ CALL,          0,        "int objects(int)",                    0,         5,   0 },
		{ CALL,          0,        "int garbage(int)",                    0,        20,   0 },
		{ CALL,          0,        "int garbage(int)",                    0,        20,   0 },
		{ CALL,          0,        "int add(int, int)",                   0,         2,   3 },
		{ CALL,          0,        "int suspends(int)",                   0,         4,   0 },
		{ CALL,          0,        "int aborts(int)",                     0,        30,   0 },
		{ CALL,          0,        "int add(int, int)",                   0,         2,   3 },
		{ CALL,          0,        "int fails(int)",                      0,        40,   0 },
		{ CALL,          0,        "int lines(int)",                      0,         5,   0 },
		{ CALL,          0,        "int add(int, int)",                   0,         2,   3 },
		{ CALL,          0,        "int callUnbound(int)",                0,         1,   0 },
		{ CALL,          0,        "int recurse(int)",                    0,         5,   0 },
		{ CALL,          0,        "int outer(int)",                      0,         3,   0 },
		{ CALL,          "engine", "int triple(int)",                     0,         4,   0 },
		{ CALL,          0,        "int add(int, int)",                   0,         4,   5 },
		{ METHOD,        "A",      "int virt(int)",                       "objA",    2,   0 },
		{ METHOD,        "A",      "int virt(int)",                       "objB",    2,   0 },
		{ METHOD,        "A",      "int virt(int)",                       "objB",    3,   0 },
		{ METHOD,        "I",      "int get(int)",                        "objB",    4,   0 },
		{ METHOD,        "A",      "string str(int)",                     "objA",    5,   0 },
		{ METHOD,        "A",      "string str(int)",                     "objB",    6,   0 },
		{ METHOD,        "A",      "int virt(int)",                       "objNull", 1,   0 },
		{ CALL,          0,        "int add(int, int)",                   0,         5,   6 },
		{ DELEGATE,      "A",      "int virt(int)",                       "objB",    7,   0 },
		{ DELEGATE,      "I",      "int get(int)",                        "objA",    8,   0 },
		{ DELEGATE,      "A",      "int virt(int)",                       "objB",    9,   0 },
		{ CALL,          0,        "int deep(int)",                       0,        50,   0 },
		{ PREPARE_TWICE, 0,        "int add(int, int)",                   0,         2,   2 },
		{ UNPREPARE,     0,        "int add(int, int)",                   0,         3,   3 },
		{ EXECUTE_AGAIN, 0,        "int add(int, int)",                   0,         4,   4 },
		{ CALL,          0,        0,                                     0,         0,   0 },
		{ CALL,          0,        "int add(int, int)",                   0,         5,   5 },
	};

	// Engine properties, and the options of the JIT compiler and of the calls
	struct SConfig { const char *name; asDWORD flags; asUINT depth; asEEngineProp prop; asPWORD value; bool mix; bool other; };
	static const SConfig configs[] =
	{
		{ "default",                0,                                 256, asEP_INIT_STACK_SIZE,      4096, false, false },
		{ "native call depth 3",    0,                                   3, asEP_INIT_STACK_SIZE,      4096, false, false },
		{ "no native script calls", CJITCompiler::JIT_NO_SCRIPT_CALLS, 256, asEP_INIT_STACK_SIZE,      4096, false, false },
		{ "small stack blocks",     0,                                 256, asEP_INIT_STACK_SIZE,        64, false, false },
		{ "few nested calls",       0,                                 256, asEP_MAX_NESTED_CALLS,        3, false, false },
		{ "no automatic GC",        0,                                 256, asEP_AUTO_GARBAGE_COLLECT,    0, false, false },
		{ "mixed with the context", 0,                                 256, asEP_INIT_STACK_SIZE,      4096, true,  false },
		{ "another compiler",       0,                                 256, asEP_INIT_STACK_SIZE,      4096, false, true },
	};

	static std::string ExceptionInfo(asIScriptContext *ctx)
	{
		std::stringstream s;
		const char *str = ctx->GetExceptionString();
		asIScriptFunction *func = ctx->GetExceptionFunction();
		s << "'" << (str ? str : "(null)") << "' in " << (func ? func->GetName() : "none") << ":" << ctx->GetExceptionLineNumber();
		return s.str();
	}

	// Sets the arguments, and the object of the methods
	static void SetArgs(asIScriptContext *ctx, asIScriptFunction *func, const SStep &step, asIScriptObject *obj)
	{
		static const std::string text = "hi";
		if( step.step == METHOD )
			ctx->SetObject(obj);
		for( asUINT p = 0; p < func->GetParamCount(); p++ )
		{
			int typeId;
			func->GetParam(p, &typeId);
			int v = p == 0 ? step.a : step.b;
			if( typeId == asTYPEID_DOUBLE )
				ctx->SetArgDouble(p, v + 0.5);
			else if( typeId == asTYPEID_INT64 )
				ctx->SetArgQWord(p, asQWORD(v));
			else if( typeId == asTYPEID_INT32 )
				ctx->SetArgDWord(p, asDWORD(v));
			else
				ctx->SetArgObject(p, (void*)&text);
		}
	}

	static std::string ReturnValue(asIScriptContext *ctx, asIScriptFunction *func)
	{
		std::stringstream s;
		asDWORD flags;
		int typeId = func->GetReturnTypeId(&flags);
		if( flags & asTM_INOUTREF )
			s << *(int*)ctx->GetReturnAddress();
		else if( typeId == asTYPEID_INT32 )
			s << int(ctx->GetReturnDWord());
		else if( typeId == asTYPEID_INT64 )
			s << asINT64(ctx->GetReturnQWord());
		else if( typeId == asTYPEID_DOUBLE )
			s << ctx->GetReturnDouble();
		else if( typeId & asTYPEID_OBJHANDLE )
		{
			asIScriptObject *obj = (asIScriptObject*)ctx->GetReturnObject();
			if( obj )
				s << obj->GetObjectType()->GetName() << " " << *(int*)obj->GetAddressOfProperty(0);
			else
				s << "null";
		}
		else
			s << "'" << *(std::string*)ctx->GetReturnObject() << "'";
		return s.str();
	}

	// Executes all the steps and returns what was observed, one line per step. The
	// calls are made through the methods of api, and the reference is compiled
	// without the JIT instructions
	static std::string Run(asIScriptEngine *engine, CJITCompiler *jit, CJITCompiler *api, const SConfig &config, bool &fail)
	{
		CBufferedOutStream msgs;
		engine->SetMessageCallback(asMETHOD(CBufferedOutStream, Callback), &msgs, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, jit != 0);
		engine->SetJITCompiler(jit);
		engine->SetEngineProperty(config.prop, config.value);
		RegisterStdString(engine);
		int r;
		r = engine->RegisterGlobalFunction("void suspend()", asFUNCTION(Suspend), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("void abort()", asFUNCTION(Abort), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("void fail()", asFUNCTION(Fail), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("void startLines()", asFUNCTION(StartLines), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("int triple(int)", asFUNCTION(Triple), asCALL_CDECL); assert( r >= 0 );
		r = engine->RegisterGlobalFunction("int nested(const string &in, int)", asFUNCTION(Nested), asCALL_CDECL); assert( r >= 0 );

		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", script);
		if( mod->Build() < 0 )
		{
			PRINTF("%s", msgs.buffer.c_str());
			TEST_FAILED;
			return "";
		}
		int *dtors = (int*)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName("dtors"));

		g_api.jit = api;
		g_api.mix = config.mix;
		g_api.calls = 0;
		std::string result;
		asIScriptContext *ctx = engine->CreateContext();
		ctx->SetExceptionCallback(asFUNCTION(OnException), 0, asCALL_CDECL);
		for( asUINT n = 0; n < sizeof(steps)/sizeof(steps[0]); n++ )
		{
			const SStep &step = steps[n];
			g_trace.str("");
			g_lines = 0;
			*dtors = 0;
			msgs.buffer = "";

			asIScriptFunction *func = 0;
			if( step.type && std::string(step.type) == "engine" )
				func = engine->GetGlobalFunctionByDecl(step.decl);
			else if( step.type )
				func = mod->GetTypeInfoByName(step.type)->GetMethodByDecl(step.decl);
			else if( step.decl )
				func = mod->GetFunctionByDecl(step.decl);
			if( step.decl && func == 0 )
			{
				PRINTF("%s not found\n", step.decl);
				TEST_FAILED;
				continue;
			}
			asIScriptObject *obj = 0;
			if( step.obj )
				obj = *(asIScriptObject**)mod->GetAddressOfGlobalVar(mod->GetGlobalVarIndexByName(step.obj));
			if( step.step == DELEGATE )
				func = engine->CreateDelegate(func, obj);

			std::stringstream s;
			s << (step.decl ? step.decl : "null") << (step.step == DELEGATE ? " delegate" : "") << (step.obj ? " of " : "") <<
			     (step.obj ? step.obj : "") << " (" << step.a << ", " << step.b << "):";
			if( step.step == UNPREPARE )
				ctx->Unprepare();
			r = g_api.Prepare(ctx, func);
			if( step.step == PREPARE_TWICE )
			{
				s << " prepare " << r;
				if( r >= 0 )
					SetArgs(ctx, func, step, obj);
				r = g_api.Prepare(ctx, func);
			}
			s << " prepare " << r << " " << ExceptionInfo(ctx);
			if( r >= 0 )
				SetArgs(ctx, func, step, obj);
			r = g_api.Execute(ctx);
			int suspends = 0;
			while( r == asEXECUTION_SUSPENDED && suspends < 100 )
			{
				suspends++;
				r = g_api.Execute(ctx);
			}
			if( step.step == EXECUTE_AGAIN )
			{
				s << " execute " << r;
				r = g_api.Execute(ctx);
			}
			s << " execute " << r;
			if( r == asEXECUTION_FINISHED )
				s << " returned " << ReturnValue(ctx, func);
			asUINT gcSize, gcDestroyed, gcDetected;
			engine->GetGCStatistics(&gcSize, &gcDestroyed, &gcDetected);
			s << " " << ExceptionInfo(ctx) << " suspends " << suspends << " lines " << g_lines << " dtors " << *dtors <<
			     " gc " << gcSize << "/" << gcDestroyed << "/" << gcDetected << " " << g_trace.str() << msgs.buffer << "\n";
			result += s.str();

			ctx->ClearLineCallback();
			if( step.step == DELEGATE )
				func->Release();
		}
		ctx->Release();
		return result;
	}
}

static bool TestHostCalls()
{
	using namespace HostCalls;
	bool fail = false;

	// Suspending must work for the results to be the same as the VM's
	asDWORD envFlags = 0;
	const char *env = getenv("AS_JIT_FLAGS");
	if( env )
		envFlags = asDWORD(strtoul(env, 0, 0)) & ~asDWORD(CJITCompiler::JIT_NO_SUSPEND | CJITCompiler::JIT_LOG);

	for( asUINT c = 0; c < sizeof(configs)/sizeof(configs[0]); c++ )
	{
		const SConfig &config = configs[c];
		asIScriptEngine *engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
		std::string expected = Run(engine, 0, 0, config, fail);
		engine->ShutDownAndRelease();
		bool nestedFailed = expected.find("Too many nested calls") != std::string::npos;
		if( (c == 0 && (expected.find("int recurse(int) (5, 0): prepare 0 '' in none:-1 execute 0 returned 63") == std::string::npos || nestedFailed)) ||
		    (config.prop == asEP_MAX_NESTED_CALLS && !nestedFailed) )
		{
			PRINTF("%s:\n%s", config.name, expected.c_str());
			TEST_FAILED;
		}

		// The JIT compilers must outlive the engine
		CJITCompiler jit(config.flags | envFlags), other(envFlags);
		jit.SetNativeCallDepth(config.depth);
		engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
		std::string actual = Run(engine, &jit, config.other ? &other : &jit, config, fail);
		engine->ShutDownAndRelease();

		SJITStatistics stats = jit.GetStatistics();
		if( stats.functionsCompiled == 0 || stats.functionsFailed != 0 )
		{
			PRINTF("%s: %u functions compiled, %u failed\n", config.name, stats.functionsCompiled, stats.functionsFailed);
			TEST_FAILED;
		}
		if( actual != expected )
		{
			std::stringstream e(expected), a(actual);
			std::string el, al;
			while( std::getline(e, el) && std::getline(a, al) )
				if( el != al )
					PRINTF("%s:\n  VM:  %s\n  JIT: %s\n", config.name, el.substr(0, 300).c_str(), al.substr(0, 300).c_str());
			TEST_FAILED;
		}
	}

	return fail;
}

// The memory functions of the compiler. The whole test suite uses them with
// AS_TEST_POOLED_MEMORY=1
namespace MemoryFunctions
{
	struct SBlock
	{
		unsigned char *mem;
		size_t         size;
		unsigned       seed;
	};

	static SBlock Alloc(size_t size, unsigned seed)
	{
		SBlock block = { (unsigned char*)CJITCompiler::AllocMemory(size), size, seed };
		if( block.mem )
			for( size_t n = 0; n < size; n++ )
				block.mem[n] = (unsigned char)(seed + n * 31);
		return block;
	}

	// Frees the block if it is intact and aligned like malloc's memory
	static bool Free(const SBlock &block)
	{
		bool ok = block.mem && (size_t)block.mem % alignof(std::max_align_t) == 0;
		for( size_t n = 0; ok && n < block.size; n++ )
			ok = block.mem[n] == (unsigned char)(block.seed + n * 31);
		CJITCompiler::FreeMemory(block.mem);
		return ok;
	}

	static std::atomic<int> g_errors(0);
	static std::mutex       g_lock;
	static std::vector<SBlock> g_handedOver;

	// Frees a block and allocates another one after the thread has handed its
	// blocks back, when it is destroyed after the lists of the thread
	struct SLateFree
	{
		SBlock block;
		~SLateFree()
		{
			if( block.mem && !Free(block) )
				g_errors++;
			SBlock other = Alloc(40, 7);
			if( !Free(other) )
				g_errors++;
		}
	};
	static thread_local SLateFree t_late;

	// Allocates and frees blocks of random sizes, some larger than the pooled
	// ones, and frees blocks that the other threads have allocated
	static void Run(unsigned seed)
	{
		t_late.block.mem = 0;
		std::vector<SBlock> blocks;
		unsigned r = seed;
		for( int n = 0; n < 20000; n++ )
		{
			r = r * 1103515245 + 12345;
			if( blocks.size() < 64 && ((r >> 16) & 3) != 0 )
			{
				size_t size = (r >> 8) % 1500;
				if( (r & 0x300) == 0 )
					size = (r >> 8) % 64;
				blocks.push_back(Alloc(size, r));
				continue;
			}
			if( blocks.empty() )
				continue;
			size_t i = (r >> 20) % blocks.size();
			SBlock block = blocks[i];
			blocks[i] = blocks.back();
			blocks.pop_back();
			if( (r & 0x10000) != 0 )
			{
				std::lock_guard<std::mutex> lock(g_lock);
				g_handedOver.push_back(block);
				if( g_handedOver.size() < 256 )
					continue;
				block = g_handedOver.front();
				g_handedOver.erase(g_handedOver.begin());
			}
			if( !Free(block) )
				g_errors++;
		}
		for( size_t n = 0; n < blocks.size(); n++ )
			if( !Free(blocks[n]) )
				g_errors++;
		t_late.block = Alloc(100, seed);
	}
}

static bool TestMemoryFunctions()
{
	using namespace MemoryFunctions;
	bool fail = false;

	// The blocks of each size don't overlap, and the freed ones are reused
	for( int round = 0; round < 3; round++ )
	{
		std::vector<SBlock> blocks;
		for( size_t size = 0; size <= 1100; size += 7 )
			blocks.push_back(Alloc(size, unsigned(size + round)));
		blocks.push_back(Alloc(100000, 3));
		for( size_t n = 0; n < blocks.size(); n++ )
			if( !Free(blocks[n]) )
				TEST_FAILED;
	}
	CJITCompiler::FreeMemory(0);

	// More blocks of one size than a thread keeps
	std::vector<SBlock> blocks;
	for( unsigned n = 0; n < 5000; n++ )
		blocks.push_back(Alloc(24, n));
	for( size_t n = 0; n < blocks.size(); n++ )
		if( !Free(blocks[n]) )
			TEST_FAILED;

	std::vector<std::thread> threads;
	for( unsigned n = 0; n < 4; n++ )
		threads.push_back(std::thread(Run, n * 7919 + 1));
	for( size_t n = 0; n < threads.size(); n++ )
		threads[n].join();
	for( size_t n = 0; n < g_handedOver.size(); n++ )
		if( !Free(g_handedOver[n]) )
			g_errors++;
	g_handedOver.clear();
	if( g_errors != 0 )
	{
		PRINTF("%d blocks of the memory functions were corrupted\n", int(g_errors));
		TEST_FAILED;
	}

	return fail;
}

// Tiered compilation, see CJITCompiler::SetCompileThresholds. The VM executes the
// functions until they have been called often enough, or one of their loops has run
// long enough, and they go on in the compiled code then. Everything that the
// application can observe must be the same as with the VM
namespace Tiered
{
	static int g_lines = 0;

	static void CountLines(asIScriptContext *, void *) { g_lines++; }
	static void Suspend() { asGetActiveContext()->Suspend(); }

	// Records the named integer variables of the function that are in scope. The line number
	// isn't, as the VM leaves the program pointer at the next statement after a call
	// that suspends, but at the JitEntry after the call with the JIT instructions
	static void DumpVars(asIScriptContext *ctx, std::stringstream &s)
	{
		for( int v = 0; v < ctx->GetVarCount(0); v++ )
		{
			const char *name;
			int typeId;
			ctx->GetVar(v, 0, &name, &typeId);
			if( name && name[0] && typeId == asTYPEID_INT32 && ctx->IsVarInScope(v, 0) )
				s << " " << name << "=" << *(int*)ctx->GetAddressOfVar(v, 0);
		}
	}

	// The warm up functions are left to the VM
	static bool NotWarm(asIScriptFunction *func, void *) { return strncmp(func->GetName(), "warm", 4) != 0; }

	static const char *script =
		"interface I { int get(int); }                                      \n"
		"class A : I                                                        \n"
		"{                                                                  \n"
		"  int v;                                                           \n"
		"  A(int a) { v = a; }                                              \n"
		"  int get(int a) { return v + a; }                                 \n"
		"}                                                                  \n"
		"funcdef int F(int);                                                \n"
		"int rare(int a) { return a + 100; }                                \n"
		"int leaf(int a) { return a * 2 + 1; }                              \n"
		"int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }           \n"
		"int loop(int n)                                                    \n"
		"{                                                                  \n"
		"  int r = 0;                                                       \n"
		"  for( int i = 0; i < n; i++ )                                     \n"
		"  {                                                                \n"
		"    r += i * 3;                                                    \n"
		"    if( (i & 7) == 0 ) r ^= i;                                     \n"
		"  }                                                                \n"
		"  return r;                                                        \n"
		"}                                                                  \n"
		"int shortLoop(int n) { int r = 0; for( int i = 0; i < n; i++ ) r += i; return r; } \n"
		"int nested(int n)                                                  \n"
		"{                                                                  \n"
		"  int r = 0;                                                       \n"
		"  for( int i = 0; i < n; i++ )                                     \n"
		"    for( int j = 0; j < n; j++ )                                   \n"
		"      r += i * j + leaf(j);                                        \n"
		"  return r;                                                        \n"
		"}                                                                  \n"
		"int late(int n)                                                    \n"
		"{                                                                  \n"
		"  int r = 0;                                                       \n"
		"  for( int i = 0; i < n; i++ )                                     \n"
		"    if( i >= 20 ) r += fact(i % 8);                                \n"
		"  return r;                                                        \n"
		"}                                                                  \n"
		"int whileLoop(int n) { int r = 0; while( n > 0 ) { r += n; n--; } return r; } \n"
		"int doLoop(int n) { int r = 0; do { r += n * n; } while( --n > 0 ); return r; } \n"
		"int calls(int n)                                                   \n"
		"{                                                                  \n"
		"  A a(3); I@ intf = @a; F@ f = rare;                               \n"
		"  int r = 0;                                                       \n"
		"  for( int k = 0; k < n; k++ )                                     \n"
		"    r += a.get(k) + intf.get(k) + f(k);                            \n"
		"  return r;                                                        \n"
		"}                                                                  \n"
		"string text(int n) { string s; for( int i = 0; i < n; i++ ) s += i; return s; } \n"
		"int suspended(int n)                                               \n"
		"{                                                                  \n"
		"  int r = 0;                                                       \n"
		"  for( int i = 0; i < n; i++ )                                     \n"
		"  {                                                                \n"
		"    r += i;                                                        \n"
		"    if( i == 5 || i == 50 ) suspend();                             \n"
		"  }                                                                \n"
		"  return r;                                                        \n"
		"}                                                                  \n"
		"int raise(int n, int at)                                           \n"
		"{                                                                  \n"
		"  int r = 0;                                                       \n"
		"  for( int i = 0; i < n; i++ )                                     \n"
		"    r += 100 / (at - i);                                           \n"
		"  return r;                                                        \n"
		"}                                                                  \n"
		"int lines(int n)                                                   \n"
		"{                                                                  \n"
		"  int r = 0;                                                       \n"
		"  for( int i = 0; i < n; i++ )                                     \n"
		"  {                                                                \n"
		"    r += i;                                                        \n"
		"    r ^= 3;                                                        \n"
		"  }                                                                \n"
		"  return r;                                                        \n"
		"}                                                                  \n"
		"int host(int n) { return rare(n) * 2; }                            \n"
		"int warm5() { return 1; }                                          \n"
		"int warm4() { return warm5() + 1; }                                \n"
		"int warm3() { return warm4() + 1; }                                \n"
		"int warm2() { return warm3() + 1; }                                \n"
		"int warm() { return warm2() + 1; }                                 \n"
		"int spinLeaf(int a) { return (a ^ 5) + 1; }                        \n"
		"int spinCall(int a)                                                \n"
		"{                                                                  \n"
		"  int r = 0;                                                       \n"
		"  for( int k = 0; k < 4; k++ ) r += spinLeaf(a + k);               \n"
		"  return r;                                                        \n"
		"}                                                                  \n"
		"int spin(int n)                                                    \n"
		"{                                                                  \n"
		"  int r = 0;                                                       \n"
		"  for( int i = 0; i < n; i++ ) r = (r + spinCall(i)) & 0xFFFFFF;   \n"
		"  return r;                                                        \n"
		"}                                                                  \n";

	struct SConfig
	{
		const char *name;
		asUINT      calls;
		asUINT      iterations;
		asUINT      inlineSize;
	};
	static const SConfig configs[] =
	{
		{ "3,10",           3, 10, 64 },
		{ "3,10 no inline", 3, 10, 0 },
		{ "1,1",            1, 1,  64 },
		{ "5,0",            5, 0,  64 },
	};

	// Executed with the methods of the context, the ones of the JIT compiler, or with a line callback
	enum EMode { CTX, HOST, LINES };

	// The number of functions that each step compiles in each configuration, or -1
	// where it depends on the functions that are compiled in place
	struct SStep
	{
		const char *decl;
		int         args[2];
		EMode       mode;
		int         compiled[4];
	};
	static const SStep steps[] =
	{
		{ "int rare(int)",      { 1 },      CTX,   { 0, 0, 1, 0 } },
		{ "int rare(int)",      { 2 },      CTX,   { 0, 0, 0, 0 } },
		{ "int rare(int)",      { 3 },      CTX,   { 1, 1, 0, 0 } },
		{ "int loop(int)",      { 1000 },   CTX,   { 1, 1, 1, 0 } },
		{ "int shortLoop(int)", { 5 },      CTX,   { 0, 0, 1, 0 } },
		{ "int shortLoop(int)", { 9 },      CTX,   { 1, 1, 0, 0 } },
		{ "int nested(int)",    { 20 },     CTX,   { 2, 2, -1, 1 } },
		{ "int late(int)",      { 40 },     CTX,   { 2, 2, 2, 1 } },
		{ "int whileLoop(int)", { 50 },     CTX,   { 1, 1, 1, 0 } },
		{ "int doLoop(int)",    { 50 },     CTX,   { 1, 1, 1, 0 } },
		{ "int calls(int)",     { 30 },     CTX,   { -1, -1, -1, -1 } },
		{ "string text(int)",   { 30 },     CTX,   { 1, 1, 1, 0 } },
		{ "int suspended(int)", { 100 },    CTX,   { 1, 1, 1, 0 } },
		{ "int raise(int, int)", { 20, 5 },  CTX,   { 0, 0, 1, 0 } },
		{ "int raise(int, int)", { 100, 50 }, CTX, { 1, 1, 0, 0 } },
		{ "int lines(int)",     { 30 },     LINES, { 1, 1, 1, 0 } },
		{ "int host(int)",      { 1 },      HOST,  { 0, 0, 1, 0 } },
		{ "int host(int)",      { 2 },      HOST,  { 0, 0, 0, 0 } },
		{ "int host(int)",      { 3 },      HOST,  { 1, 1, 0, 0 } },
		{ "int host(int)",      { 4 },      HOST,  { 0, 0, 0, 0 } },
		{ "int host(int)",      { 5 },      HOST,  { 0, 0, 0, 1 } },
	};

	static const int THREADS    = 4;
	static const int SPIN_COUNT = 20000;
	static std::mutex g_lock;

	static int Spun(int n)
	{
		int r = 0;
		for( int i = 0; i < n; i++ )
		{
			int c = 0;
			for( int k = 0; k < 4; k++ )
				c += ((i + k) ^ 5) + 1;
			r = (r + c) & 0xFFFFFF;
		}
		return r;
	}

	// The threads compile the functions at the same time. The memory functions of the
	// tests aren't thread safe, so the data of the thread and the call stack of the
	// context are allocated before under the lock, by the warm up functions
	static void Spin(asIScriptEngine *engine, asIScriptFunction *warm, asIScriptFunction *spin, std::atomic<int> *ready, int *result)
	{
		asIScriptContext *ctx;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			ctx = engine->CreateContext();
			ctx->Prepare(warm);
			ctx->Execute();
			ctx->Prepare(spin);
			ctx->SetArgDWord(0, SPIN_COUNT);
		}
		(*ready)++;
		while( *ready < THREADS )
			std::this_thread::yield();
		*result = ctx->Execute() == asEXECUTION_FINISHED ? int(ctx->GetReturnDWord()) : -1;

		std::lock_guard<std::mutex> lock(g_lock);
		ctx->Release();
		asThreadCleanup();
	}

	// Returns what was observed
	static std::string Run(asIScriptEngine *engine, CJITCompiler *jit, asUINT config, bool &fail)
	{
		CBufferedOutStream msgs;
		engine->SetMessageCallback(asMETHOD(CBufferedOutStream, Callback), &msgs, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, jit != 0);
		engine->SetJITCompiler(jit);
		RegisterStdString(engine);
		int r = engine->RegisterGlobalFunction("void suspend()", asFUNCTION(Suspend), asCALL_CDECL); assert( r >= 0 );

		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", script);
		if( mod->Build() < 0 )
		{
			PRINTF("%s", msgs.buffer.c_str());
			TEST_FAILED;
			return "";
		}

		// Nothing is compiled when the module is built, and the thresholds can't change anymore
		const char *name = configs[config].name;
		if( jit )
		{
			SJITStatistics stats = jit->GetStatistics();
			if( stats.functionsCompiled != 0 || stats.functionsDeferred == 0 || jit->SetCompileThresholds(1, 1) >= 0 )
			{
				PRINTF("%s: %u functions compiled and %u deferred by the build\n", name, stats.functionsCompiled, stats.functionsDeferred);
				TEST_FAILED;
			}
		}

		std::stringstream s;
		asIScriptContext *ctx = engine->CreateContext();
		for( asUINT n = 0; n < sizeof(steps)/sizeof(steps[0]); n++ )
		{
			const SStep &step = steps[n];
			asIScriptFunction *func = mod->GetFunctionByDecl(step.decl);
			bool host = jit && step.mode == HOST;
			asUINT compiled = jit ? jit->GetStatistics().functionsCompiled : 0;
			s << step.decl << " " << step.args[0] << ":";

			g_lines = 0;
			if( step.mode == LINES )
				ctx->SetLineCallback(asFUNCTION(CountLines), 0, asCALL_CDECL);
			r = host ? jit->Prepare(ctx, func) : ctx->Prepare(func);
			for( asUINT a = 0; r >= 0 && a < func->GetParamCount(); a++ )
				ctx->SetArgDWord(a, step.args[a]);
			while( r >= 0 )
			{
				r = host ? jit->Execute(ctx) : ctx->Execute();
				if( r != asEXECUTION_SUSPENDED )
					break;
				s << " suspended with";
				DumpVars(ctx, s);
			}
			if( r == asEXECUTION_FINISHED && func->GetReturnTypeId() == asTYPEID_INT32 )
				s << " returned " << int(ctx->GetReturnDWord());
			else if( r == asEXECUTION_FINISHED )
				s << " returned '" << *(std::string*)ctx->GetReturnObject() << "'";
			else if( r == asEXECUTION_EXCEPTION )
				s << " " << ctx->GetExceptionString() << " in " << ctx->GetExceptionFunction()->GetName() << ":" << ctx->GetExceptionLineNumber();
			else
				s << " failed with " << r;
			if( step.mode == LINES )
			{
				s << ", " << g_lines << " lines";
				ctx->ClearLineCallback();
			}
			s << "\n";

			if( jit )
			{
				compiled = jit->GetStatistics().functionsCompiled - compiled;
				if( step.compiled[config] >= 0 && compiled != asUINT(step.compiled[config]) )
				{
					PRINTF("%s: %s %d compiled %u functions instead of %d\n", name, step.decl, step.args[0], compiled, step.compiled[config]);
					TEST_FAILED;
				}
			}
		}
		ctx->Release();

		std::atomic<int> ready(0);
		int results[THREADS];
		// Looking up the functions allocates, so it is done before the threads start
		asIScriptFunction *warm = mod->GetFunctionByDecl("int warm()");
		asIScriptFunction *spin = mod->GetFunctionByDecl("int spin(int)");
		std::vector<std::thread> threads;
		for( int n = 0; n < THREADS; n++ )
			threads.push_back(std::thread(Spin, engine, warm, spin, &ready, &results[n]));
		for( int n = 0; n < THREADS; n++ )
			threads[n].join();
		s << "spin";
		for( int n = 0; n < THREADS; n++ )
		{
			s << " " << results[n];
			if( results[n] != Spun(SPIN_COUNT) )
				TEST_FAILED;
		}
		s << "\n";

		return s.str();
	}
}

static bool TestTiered()
{
	using namespace Tiered;
	bool fail = false;

	// The line callback must be called as by the VM
	asDWORD envFlags = 0;
	const char *env = getenv("AS_JIT_FLAGS");
	if( env )
		envFlags = asDWORD(strtoul(env, 0, 0)) & ~asDWORD(CJITCompiler::JIT_NO_SUSPEND | CJITCompiler::JIT_LOG);

	asIScriptEngine *engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
	std::string expected = Run(engine, 0, 0, fail);
	engine->ShutDownAndRelease();

	for( asUINT c = 0; c < sizeof(configs)/sizeof(configs[0]); c++ )
	{
		// The JIT compiler must outlive the engine
		CJITCompiler jit(envFlags);
		jit.SetMaxInlineSize(configs[c].inlineSize);
		jit.SetCompileFilter(NotWarm, 0);
		if( jit.SetCompileThresholds(configs[c].calls, configs[c].iterations) < 0 )
			TEST_FAILED;
		engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
		std::string actual = Run(engine, &jit, c, fail);
		engine->ShutDownAndRelease();

		SJITStatistics stats = jit.GetStatistics();
		if( stats.functionsCompiled == 0 || stats.functionsFailed != 0 )
		{
			PRINTF("tiered %s: %u functions compiled, %u failed\n", configs[c].name, stats.functionsCompiled, stats.functionsFailed);
			TEST_FAILED;
		}
		if( actual != expected )
		{
			std::stringstream e(expected), a(actual);
			std::string el, al;
			while( std::getline(e, el) && std::getline(a, al) )
				if( el != al )
					PRINTF("tiered %s:\n  VM:  %s\n  JIT: %s\n", configs[c].name, el.substr(0, 300).c_str(), al.substr(0, 300).c_str());
			TEST_FAILED;
		}
	}

	return fail;
}

// Profiles, see CJITCompiler::SetProfileThreshold. The functions are compiled again
// with the methods of the classes that their virtual and interface calls have seen,
// which the objects of other classes must still call. Everything that the application
// can observe must be the same as with the VM
namespace Profiles
{
	static int g_lines = 0;

	static void CountLines(asIScriptContext *, void *) { g_lines++; }
	static void Suspend() { asGetActiveContext()->Suspend(); }

	// Implements the interface shared with the module of the test
	static const char *otherScript =
		"shared interface Shape { int area(int k); }                                       \n"
		"class Hex : Shape { int area(int k) { return k * 6; } }                           \n"
		"Shape@ makeHex() { return Hex(); }                                                \n";

	static const char *script =
		"shared interface Shape { int area(int k); }                                       \n"
		"import Shape@ makeHex() from \"other\";                                           \n"
		"class Sq : Shape { int s; Sq(int a) { s = a; } int area(int k) { return s * s + k; } } \n"
		"class Rect : Shape { int w, h; Rect(int a, int b) { w = a; h = b; } int area(int k) { return w * h - k; } } \n"
		"class Tri : Shape { int b, h; Tri(int a, int c) { b = a; h = c; } int area(int k) { return b * h / 2 + k * 2; } } \n"
		"class Base { int v; Base(int a) { v = a; } int get(int k) { return v + k; } }     \n"
		"class Mid : Base { Mid(int a) { super(a); } int get(int k) override { return v * 2 + k; } } \n"
		"class Leaf : Mid { Leaf(int a) { super(a); } int get(int k) override { return v * 3 - k; } } \n"
		"Shape@ gs = Sq(2);                                                                \n"
		// The call sees one class
		"int mono(int n) {                                                                 \n"
		"  Shape@ s = Sq(3);                                                               \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) r += s.area(i);                                    \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// The call sees several classes, which gains nothing from compiling it again
		"int poly(int n) {                                                                 \n"
		"  Shape@ a = Sq(2), b = Rect(2, 3), c = Tri(4, 5);                                \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) {                                                  \n"
		"    Shape@ s = a;                                                                 \n"
		"    if( i % 3 == 1 ) @s = b;                                                      \n"
		"    else if( i % 3 == 2 ) @s = c;                                                 \n"
		"    r += s.area(i);                                                               \n"
		"  }                                                                               \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// Another class comes after the method has been inlined for the first
		"int turn(int n) {                                                                 \n"
		"  Shape@ s = Sq(3);                                                               \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) {                                                  \n"
		"    if( i == n / 2 ) @s = Rect(2, 5);                                             \n"
		"    r += s.area(i);                                                               \n"
		"  }                                                                               \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// The second call is only made after the function has been compiled again for
		// the first, and it is compiled a third time for the second
		"int late(int n) {                                                                 \n"
		"  Shape@ a = Sq(4), b = Tri(2, 6);                                                \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) {                                                  \n"
		"    r += a.area(i);                                                               \n"
		"    if( i >= n / 2 ) r += b.area(i);                                              \n"
		"  }                                                                               \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// The method that the derived classes override
		"int virt(int n) {                                                                 \n"
		"  Base@ b = Mid(4);                                                               \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) r += b.get(i);                                     \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// The class of the other module isn't compared with
		"int foreign(int n) {                                                              \n"
		"  Shape@ s = makeHex();                                                           \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) r += s.area(i);                                    \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// The inlined method isn't called for the null handle
		"int nullCall(int n) {                                                             \n"
		"  Shape@ s = Sq(1);                                                               \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) {                                                  \n"
		"    if( i == n - 2 ) @s = null;                                                   \n"
		"    r += s.area(i);                                                               \n"
		"  }                                                                               \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// The call is in a function compiled in place
		"int areaOf(Shape@ s, int k) { return s.area(k); }                                 \n"
		"int nested(int n) {                                                               \n"
		"  Shape@ s = Rect(3, 4);                                                          \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) r += areaOf(s, i);                                 \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		"int suspended(int n) {                                                            \n"
		"  Shape@ s = Tri(3, 4);                                                           \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) {                                                  \n"
		"    r += s.area(i);                                                               \n"
		"    if( i % 25 == 0 ) suspend();                                                  \n"
		"  }                                                                               \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// The threads compile the function again at the same time
		"int warm() { return gs.area(1); }                                                 \n"
		"int spin(int n) {                                                                 \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) r = (r + gs.area(i)) & 0xFFFFFF;                   \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n";

	struct SConfig
	{
		const char *name;
		asUINT      calls;
		asUINT      iterations;
		asUINT      inlineSize;
		asUINT      profile;
	};
	static const SConfig configs[] =
	{
		{ "5",           0, 0,  64, 5 },
		{ "3,10 and 5",  3, 10, 64, 5 },
		{ "5 no inline", 0, 0,  0,  5 },
		{ "1",           0, 0,  64, 1 },
	};

	enum EMode { CTX, LINES };

	// The number of functions that each step compiles again in each configuration,
	// or -1 where it depends on when the functions compiled in place are compiled.
	// After one call the calls of poly have only seen one class
	struct SStep
	{
		const char *decl;
		int         arg;
		EMode       mode;
		int         recompiled[4];
	};
	static const SStep steps[] =
	{
		{ "int mono(int)",      100, LINES, { 1, 1, 0, 1 } },
		{ "int mono(int)",      100, CTX,   { 0, 0, 0, 0 } },
		{ "int poly(int)",      100, CTX,   { 0, 0, 0, 1 } },
		{ "int turn(int)",      100, CTX,   { 1, 1, 0, 1 } },
		{ "int turn(int)",      100, LINES, { 0, 0, 0, 0 } },
		{ "int late(int)",      100, CTX,   { 2, 2, 0, 2 } },
		{ "int late(int)",      100, CTX,   { 0, 0, 0, 0 } },
		{ "int virt(int)",      100, CTX,   { 1, 1, 0, 1 } },
		{ "int foreign(int)",   100, CTX,   { 0, 0, 0, 0 } },
		{ "int nullCall(int)",  100, CTX,   { 1, 1, 0, 1 } },
		{ "int nested(int)",    100, CTX,   { 1, -1, 0, 1 } },
		{ "int suspended(int)", 100, CTX,   { 1, 1, 0, 1 } },
	};

	static const int THREADS    = 4;
	static const int SPIN_COUNT = 20000;
	static std::mutex g_lock;

	// gs.area(i) of the script
	static int Spun(int n)
	{
		int r = 0;
		for( int i = 0; i < n; i++ )
			r = (r + 4 + i) & 0xFFFFFF;
		return r;
	}

	// The memory functions of the tests aren't thread safe, so the call stack of the
	// context is allocated before under the lock, by the warm up function
	static void Spin(asIScriptEngine *engine, asIScriptFunction *warm, asIScriptFunction *spin, std::atomic<int> *ready, int *result)
	{
		asIScriptContext *ctx;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			ctx = engine->CreateContext();
			ctx->Prepare(warm);
			ctx->Execute();
			ctx->Prepare(spin);
			ctx->SetArgDWord(0, SPIN_COUNT);
		}
		(*ready)++;
		while( *ready < THREADS )
			std::this_thread::yield();
		*result = ctx->Execute() == asEXECUTION_FINISHED ? int(ctx->GetReturnDWord()) : -1;

		std::lock_guard<std::mutex> lock(g_lock);
		ctx->Release();
		asThreadCleanup();
	}

	// The warm up function is left to the VM
	static bool NotWarm(asIScriptFunction *func, void *) { return strcmp(func->GetName(), "warm") != 0; }

	// Returns what was observed. The functions compiled again are only checked if the
	// calls can be compiled in place
	static std::string Run(asIScriptEngine *engine, CJITCompiler *jit, asUINT config, bool inlining, bool &fail)
	{
		CBufferedOutStream msgs;
		engine->SetMessageCallback(asMETHOD(CBufferedOutStream, Callback), &msgs, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, jit != 0);
		engine->SetJITCompiler(jit);
		int r = engine->RegisterGlobalFunction("void suspend()", asFUNCTION(Suspend), asCALL_CDECL); assert( r >= 0 );

		asIScriptModule *other = engine->GetModule("other", asGM_ALWAYS_CREATE);
		other->AddScriptSection("other", otherScript);
		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", script);
		if( other->Build() < 0 || mod->Build() < 0 || mod->BindAllImportedFunctions() < 0 )
		{
			PRINTF("%s", msgs.buffer.c_str());
			TEST_FAILED;
			return "";
		}

		const char *name = configs[config].name;
		std::stringstream s;
		asIScriptContext *ctx = engine->CreateContext();
		for( asUINT n = 0; n < sizeof(steps)/sizeof(steps[0]); n++ )
		{
			const SStep &step = steps[n];
			asIScriptFunction *func = mod->GetFunctionByDecl(step.decl);
			asUINT recompiled = jit ? jit->GetStatistics().functionsRecompiled : 0;
			s << step.decl << " " << step.arg << ":";

			g_lines = 0;
			if( step.mode == LINES )
				ctx->SetLineCallback(asFUNCTION(CountLines), 0, asCALL_CDECL);
			r = ctx->Prepare(func);
			if( r >= 0 )
				ctx->SetArgDWord(0, step.arg);
			int suspends = 0;
			while( r >= 0 )
			{
				r = ctx->Execute();
				if( r != asEXECUTION_SUSPENDED )
					break;
				suspends++;
			}
			if( r == asEXECUTION_FINISHED )
				s << " returned " << int(ctx->GetReturnDWord());
			else if( r == asEXECUTION_EXCEPTION )
				s << " " << ctx->GetExceptionString() << " in " << ctx->GetExceptionFunction()->GetName() << ":" << ctx->GetExceptionLineNumber();
			else
				s << " failed with " << r;
			s << ", " << suspends << " suspends";
			if( step.mode == LINES )
			{
				s << ", " << g_lines << " lines";
				ctx->ClearLineCallback();
			}
			s << "\n";

			if( jit )
			{
				recompiled = jit->GetStatistics().functionsRecompiled - recompiled;
				int expected = inlining ? step.recompiled[config] : 0;
				if( expected >= 0 && recompiled != asUINT(expected) )
				{
					PRINTF("profiles %s: %s %d compiled %u functions again instead of %d\n", name, step.decl, step.arg, recompiled, expected);
					TEST_FAILED;
				}
			}
		}
		ctx->Release();

		// One of the threads compiles the function again
		asUINT recompiled = jit ? jit->GetStatistics().functionsRecompiled : 0;
		std::atomic<int> ready(0);
		int results[THREADS];
		// Looking up the functions allocates, so it is done before the threads start
		asIScriptFunction *warm = mod->GetFunctionByDecl("int warm()");
		asIScriptFunction *spin = mod->GetFunctionByDecl("int spin(int)");
		std::vector<std::thread> threads;
		for( int n = 0; n < THREADS; n++ )
			threads.push_back(std::thread(Spin, engine, warm, spin, &ready, &results[n]));
		for( int n = 0; n < THREADS; n++ )
			threads[n].join();
		s << "spin";
		for( int n = 0; n < THREADS; n++ )
		{
			s << " " << results[n];
			if( results[n] != Spun(SPIN_COUNT) )
				TEST_FAILED;
		}
		s << "\n";
		if( jit )
		{
			recompiled = jit->GetStatistics().functionsRecompiled - recompiled;
			asUINT expected = inlining && configs[config].inlineSize > 0 ? 1 : 0;
			if( recompiled != expected )
			{
				PRINTF("profiles %s: the threads compiled %u functions again instead of %u\n", name, recompiled, expected);
				TEST_FAILED;
			}
		}

		return s.str();
	}
}

static bool TestProfiles()
{
	using namespace Profiles;
	bool fail = false;

	// The line callback must be called as by the VM, and the calls must be compiled in
	// place for the classes to be noted
	asDWORD envFlags = 0;
	const char *env = getenv("AS_JIT_FLAGS");
	if( env )
		envFlags = asDWORD(strtoul(env, 0, 0)) & ~asDWORD(CJITCompiler::JIT_NO_SUSPEND | CJITCompiler::JIT_LOG);
	bool inlining = (envFlags & (CJITCompiler::JIT_NO_INLINE | CJITCompiler::JIT_NO_SCRIPT_CALLS | CJITCompiler::JIT_SYNC_EVERY_INSTR)) == 0;

	asIScriptEngine *engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
	std::string expected = Run(engine, 0, 0, false, fail);
	engine->ShutDownAndRelease();

	for( asUINT c = 0; c < sizeof(configs)/sizeof(configs[0]); c++ )
	{
		// The JIT compiler must outlive the engine
		CJITCompiler jit(envFlags);
		jit.SetMaxInlineSize(configs[c].inlineSize);
		jit.SetProfileThreshold(configs[c].profile);
		jit.SetCompileFilter(NotWarm, 0);
		if( jit.SetCompileThresholds(configs[c].calls, configs[c].iterations) < 0 )
			TEST_FAILED;
		engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
		std::string actual = Run(engine, &jit, c, inlining, fail);
		engine->ShutDownAndRelease();

		SJITStatistics stats = jit.GetStatistics();
		if( stats.functionsCompiled == 0 || stats.functionsFailed != 0 )
		{
			PRINTF("profiles %s: %u functions compiled, %u failed\n", configs[c].name, stats.functionsCompiled, stats.functionsFailed);
			TEST_FAILED;
		}
		if( actual != expected )
		{
			std::stringstream e(expected), a(actual);
			std::string el, al;
			while( std::getline(e, el) && std::getline(a, al) )
				if( el != al )
					PRINTF("profiles %s:\n  VM:  %s\n  JIT: %s\n", configs[c].name, el.substr(0, 300).c_str(), al.substr(0, 300).c_str());
			TEST_FAILED;
		}
	}

	return fail;
}

// The code only checks for suspension and line callbacks where they may have been
// requested since the last check, and the functions executed while a line callback is
// set are compiled again with the checks at every statement, see
// JIT_CHECK_EVERY_STATEMENT. Everything that the application can observe must be the
// same as with the VM
namespace SuspendChecks
{
	static const char *script =
		"interface Shape { int area(int k); }                                              \n"
		"class Sq : Shape { int area(int k) { return k * k; } }                            \n"
		"class Tri : Shape { int area(int k) { return k + 2; } }                           \n"
		"int add(int a, int b) { return a + b; }                                           \n"
		"int sum(int n) {                                                                  \n"
		"  int s = 0;                                                                      \n"
		"  for( int k = 0; k < n; k++ )                                                    \n"
		"    s = add(s, k);                                                                \n"
		"  return s;                                                                       \n"
		"}                                                                                 \n"
		// The call of the method notes the class of the object, see SetProfileThreshold
		"int run(int n) {                                                                  \n"
		"  Shape@ s = Sq();                                                                \n"
		"  int r = 0;                                                                      \n"
		"  for( int i = 0; i < n; i++ ) {                                                  \n"
		"    r += sum(i % 7) + s.area(i);                                                  \n"
		"    r += note(i);                                                                 \n"
		"  }                                                                               \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n"
		// Only called by the function that the VM goes on in after the line callback
		// has been set in the call before
		"int tail(int n) { return n * 3 + 1; }                                             \n"
		"int other(int n) {                                                                \n"
		"  int r = run(n);                                                                 \n"
		"  r += tail(r);                                                                   \n"
		"  for( int i = 0; i < 3; i++ )                                                    \n"
		"    r += sum(i);                                                                  \n"
		"  return r;                                                                       \n"
		"}                                                                                 \n";

	// Without a line callback, with one, with one set in the middle of the loop, with
	// suspensions requested in the loop, and with both
	enum EMode { PLAIN, LINES, START, PAUSE, BOTH };
	static EMode g_mode  = PLAIN;
	static int   g_lines = 0;

	static void CountLines(asIScriptContext *, void *) { g_lines++; }

	static int Note(int i)
	{
		if( g_mode == START && i == 5 )
			asGetActiveContext()->SetLineCallback(asFUNCTION(CountLines), 0, asCALL_CDECL);
		else if( (g_mode == PAUSE || g_mode == BOTH) && i % 4 == 1 )
			asGetActiveContext()->Suspend();
		return i & 3;
	}

	struct SConfig
	{
		const char *name;
		asDWORD     flags;
		asUINT      inlineSize;
		asUINT      calls;
		asUINT      iterations;
	};
	static const SConfig configs[] =
	{
		{ "inline",          0,                                       64, 0, 0 },
		{ "no inline",       0,                                       0,  0, 0 },
		{ "every statement", CJITCompiler::JIT_CHECK_EVERY_STATEMENT, 64, 0, 0 },
		{ "2,3",             0,                                       64, 2, 3 },
	};

	// The number of functions that each step compiles for the line callbacks in each
	// configuration. The first compiles run and the functions it calls, which the VM
	// enters as it goes on in the functions compiled in place too, and they keep the
	// code, also run when it is compiled again with the class seen by its call. When
	// the line callback is set in the call of run the VM goes on in other, which is
	// compiled for it then, and so is tail. The deferred functions are compiled for
	// the line callbacks right away if one is set, which counts as compiled
	struct SStep
	{
		const char *decl;
		int         arg;
		EMode       mode;
		int         compiled[4];
	};
	static const SStep steps[] =
	{
		{ "int run(int)",   20, LINES, { 6, 6, 0, 0 } },
		{ "int run(int)",   20, LINES, { 0, 0, 0, 0 } },
		{ "int other(int)", 20, PLAIN, { 0, 0, 0, 0 } },
		{ "int other(int)", 20, PAUSE, { 0, 0, 0, 0 } },
		{ "int other(int)", 20, START, { 2, 2, 0, 1 } },
		{ "int other(int)", 20, START, { 0, 0, 0, 0 } },
		{ "int other(int)", 20, BOTH,  { 0, 0, 0, 0 } },
		{ "int other(int)", 20, PLAIN, { 0, 0, 0, 0 } },
		{ "int run(int)",   20, LINES, { 0, 0, 0, 0 } },
	};

	// Returns what was observed. The column of the configuration has the functions
	// expected to be compiled for the line callbacks, or is -1 if it depends
	static std::string Run(asIScriptEngine *engine, CJITCompiler *jit, const char *name, int column, bool &fail)
	{
		CBufferedOutStream msgs;
		engine->SetMessageCallback(asMETHOD(CBufferedOutStream, Callback), &msgs, asCALL_THISCALL);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, jit != 0);
		engine->SetJITCompiler(jit);
		int r = engine->RegisterGlobalFunction("int note(int)", asFUNCTION(Note), asCALL_CDECL); assert( r >= 0 );

		asIScriptModule *mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
		mod->AddScriptSection("test", script);
		if( mod->Build() < 0 )
		{
			PRINTF("%s", msgs.buffer.c_str());
			TEST_FAILED;
			return "";
		}

		std::stringstream s;
		asIScriptContext *ctx = engine->CreateContext();
		for( asUINT n = 0; n < sizeof(steps)/sizeof(steps[0]); n++ )
		{
			const SStep &step = steps[n];
			asUINT compiled = jit ? jit->GetStatistics().functionsForLineCallbacks : 0;
			s << step.decl << " " << step.arg << ":";

			g_mode  = step.mode;
			g_lines = 0;
			if( step.mode == LINES || step.mode == BOTH )
				ctx->SetLineCallback(asFUNCTION(CountLines), 0, asCALL_CDECL);
			r = ctx->Prepare(mod->GetFunctionByDecl(step.decl));
			if( r >= 0 )
				ctx->SetArgDWord(0, step.arg);
			while( r >= 0 )
			{
				r = ctx->Execute();
				if( r != asEXECUTION_SUSPENDED )
					break;
				s << " suspended in " << ctx->GetFunction()->GetName() << ":" << ctx->GetLineNumber() << " after " << g_lines << " lines";
			}
			if( r == asEXECUTION_FINISHED )
				s << " returned " << int(ctx->GetReturnDWord());
			else
				s << " failed with " << r;
			s << ", " << g_lines << " lines\n";
			ctx->ClearLineCallback();

			if( jit )
			{
				compiled = jit->GetStatistics().functionsForLineCallbacks - compiled;
				int expected = column >= 0 ? step.compiled[column] : -1;
				if( expected >= 0 && compiled != asUINT(expected) )
				{
					PRINTF("suspend checks %s: step %u %s %d compiled %u functions for the line callbacks instead of %d\n", name, n, step.decl, step.arg, compiled, expected);
					TEST_FAILED;
				}
			}
		}
		ctx->Release();
		return s.str();
	}
}

static bool TestSuspendChecks()
{
	using namespace SuspendChecks;
	bool fail = false;

	// The line callbacks must be called as by the VM. The functions compiled for them
	// are only checked if the calls are made and compiled in place as usual, and the
	// functions compiled again if the calls can be compiled in place
	asDWORD envFlags = 0;
	const char *env = getenv("AS_JIT_FLAGS");
	if( env )
		envFlags = asDWORD(strtoul(env, 0, 0)) & ~asDWORD(CJITCompiler::JIT_NO_SUSPEND | CJITCompiler::JIT_LOG);
	bool inlining = (envFlags & (CJITCompiler::JIT_NO_INLINE | CJITCompiler::JIT_SYNC_EVERY_INSTR)) == 0;
	bool calls    = (envFlags & CJITCompiler::JIT_NO_SCRIPT_CALLS) == 0;

	asIScriptEngine *engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
	std::string expected = Run(engine, 0, "VM", -1, fail);
	engine->ShutDownAndRelease();

	for( asUINT c = 0; c < sizeof(configs)/sizeof(configs[0]); c++ )
	{
		int column = int(c);
		if( (envFlags | configs[c].flags) & CJITCompiler::JIT_CHECK_EVERY_STATEMENT )
			column = 2;
		else if( !calls )
			column = -1;
		else if( !inlining )
			column = configs[c].calls ? -1 : 1;

		// The JIT compiler must outlive the engine
		CJITCompiler jit(envFlags | configs[c].flags);
		jit.SetMaxInlineSize(configs[c].inlineSize);
		jit.SetProfileThreshold(5);
		if( jit.SetCompileThresholds(configs[c].calls, configs[c].iterations) < 0 )
			TEST_FAILED;
		engine = (asCreateScriptEngine)(ANGELSCRIPT_VERSION);
		std::string actual = Run(engine, &jit, configs[c].name, column, fail);
		engine->ShutDownAndRelease();

		SJITStatistics stats = jit.GetStatistics();
		if( stats.functionsCompiled == 0 || stats.functionsFailed != 0 )
		{
			PRINTF("suspend checks %s: %u functions compiled, %u failed\n", configs[c].name, stats.functionsCompiled, stats.functionsFailed);
			TEST_FAILED;
		}
		asUINT recompiled = inlining && calls && configs[c].inlineSize > 0 ? 1 : 0;
		if( stats.functionsRecompiled != recompiled )
		{
			PRINTF("suspend checks %s: %u functions compiled again instead of %u\n", configs[c].name, stats.functionsRecompiled, recompiled);
			TEST_FAILED;
		}
		if( actual != expected )
		{
			std::stringstream e(expected), a(actual);
			std::string el, al;
			while( std::getline(e, el) && std::getline(a, al) )
				if( el != al )
					PRINTF("suspend checks %s:\n  VM:  %s\n  JIT: %s\n", configs[c].name, el.substr(0, 300).c_str(), al.substr(0, 300).c_str());
			TEST_FAILED;
		}
	}
	return fail;
}

// as_powi from the engine isn't accessible so the same algorithm is repeated here
int as_powi_test(int base, int exponent, bool &isOverflow)
{
	isOverflow = false;
	if( exponent < 0 )
	{
		if( base == 0 )
		{
			isOverflow = true;
			return 0;
		}
		if( base == 1 ) return 1;
		if( base == -1 ) return (exponent & 1) ? -1 : 1;
		return 0;
	}
	long long result = 1;
	for( int n = 0; n < exponent; n++ )
	{
		result *= base;
		if( result > 2147483647LL || result < -2147483648LL )
		{
			isOverflow = true;
			return 0;
		}
	}
	return int(result);
}

bool Test()
{
	bool fail = false;

	// The JIT specific tests are meaningless when the JIT is disabled, or only takes
	// the functions generated ahead of time
	const char *flags = getenv("AS_JIT_FLAGS");
	if( getenv("AS_JIT_DISABLE") || (flags && (strtoul(flags, 0, 0) & CJITCompiler::JIT_AOT_ONLY)) )
		return false;

	// Each engine gets the JIT compiler through CreateEngineWithJit
	asIScriptEngine *engine;

	engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
	fail = TestArithmetic(engine) || fail;
	engine->ShutDownAndRelease();

	engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
	fail = TestExceptions(engine) || fail;
	engine->ShutDownAndRelease();

	engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
	fail = TestCallsAndObjects(engine) || fail;
	engine->ShutDownAndRelease();

	engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
	fail = TestSuspend(engine) || fail;
	engine->ShutDownAndRelease();

	engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
	fail = TestNestedExecution(engine) || fail;
	engine->ShutDownAndRelease();

	fail = TestDirectCalls() || fail;
	fail = TestNativeCalls() || fail;
	fail = TestInlining() || fail;
	fail = TestCppExceptions() || fail;
	fail = TestRefCounting() || fail;
	fail = TestScriptRefCounts() || fail;
	fail = TestListFrees() || fail;
	fail = TestHostCalls() || fail;
	fail = TestMemoryFunctions() || fail;
	fail = TestTiered() || fail;
	fail = TestProfiles() || fail;
	fail = TestSuspendChecks() || fail;

	return fail;
}

} // namespace
