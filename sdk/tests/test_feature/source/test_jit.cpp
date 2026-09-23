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

	struct CObj
	{
		int v;
		int   Get()              { return v; }
		void  Set(int a)         { v = a; }
		float Mul(float f)       { return v * f; }
		asINT64 Big(asINT64 a, int b) { return a * b + v; }
		double Mix(double a, float b, asINT64 c, int d) { return a + b + double(c) + d + v; }
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
		"int suspendArgs() { return Cdecl(SuspRet(3), 4); }            \n";

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

	ctx->Release();
	engine->ShutDownAndRelease();
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

	return fail;
}

} // namespace
