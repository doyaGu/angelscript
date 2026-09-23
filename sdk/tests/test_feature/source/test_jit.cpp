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
#include <new>

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
		"}                                                                 \n";

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
		CVal  Five(int a, double b, float c, int d) { CVal r; r.v = int(a + b + c + d) + v; return r; }
		double Four(float a, float b, double c, float d) { return a + b + c + d + v; }
		int   Many(int i1, int i2, int i3, int i4, float f1, float f2, float f3, float f4,
		           int i5, int i6, int i7, int i8, float f5, float f6, float f7, float f8)
		{
			return i1 + i2 * 2 + i3 * 3 + i4 * 4 + int(f1 + f2 * 2 + f3 * 3 + f4 * 4) +
			       i5 * 5 + i6 * 6 + i7 * 7 + i8 * 8 + int(f5 * 5 + f6 * 6 + f7 * 7 + f8 * 8) + v;
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
		if( std::string(ctx->GetFunction()->GetName()) == "leaf" )
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
		"int suspended(int n) { int r = 0, i = 0; while( i < n ) r += leaf(i++); return r; } \n";

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

	// The JIT specific tests are meaningless when the JIT is disabled
	if( getenv("AS_JIT_DISABLE") )
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

	return fail;
}

} // namespace
