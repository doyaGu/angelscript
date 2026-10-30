// Measures the time per iteration of scripts that use the features the JIT compiler
// optimizes, with the VM and with the JIT compiler, and checks that both give the
// same results. The scripts are small loops written like the scripts of
// applications, e.g. calls of registered functions, methods on the elements of
// arrays, script-class vectors, casts, and calls through function pointers.
//
//   test_jit_bench [--only <text>] [--log <text>] [--vm] [--jit]
//
// --only runs the tests whose declaration contains the text, --log writes the code
// that the JIT compiler generates for the functions whose declaration contains the
// text, and --vm or --jit only run the scripts with the VM or with the JIT compiler.
// Each test is run three times with each and the fastest time is printed.

#include <angelscript.h>
#include "../../../add_on/scriptstdstring/scriptstdstring.h"
#include "../../../add_on/scriptarray/scriptarray.h"
#include "../../../add_on/scriptmath/scriptmath.h"
#include "../../../add_on/scriptdictionary/scriptdictionary.h"
#include "../../../add_on/jit/jit.h"
#include <chrono>
#include <stdio.h>
#include <string.h>

static double Now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void MessageCallback(const asSMessageInfo *msg, void *)
{
	printf("%s (%d, %d): %s\n", msg->section, msg->row, msg->col, msg->message);
}

static float NativeAdd(float a, float b) { return a + b; }

static void GenericAdd(asIScriptGeneric *gen)
{
	gen->SetReturnFloat(gen->GetArgFloat(0) + gen->GetArgFloat(1));
}

static const char *script = R"(
funcdef int BinOp(int, int);
int add(int a, int b) { return a + b; }
int g_counter = 0;
interface IShape { float area(); }
class Circle : IShape { float r; Circle(float x) { r = x; } float area() { return 3.14f * r * r; } }
interface IRound {}
class Ring : Circle, IRound { Ring(float x) { super(x); } }
class Square : IShape { float s; Square(float x) { s = x; } float area() { return s * s; } }
class Tri : IShape { float b, h; Tri(float x) { b = x; h = 2; } float area() { return 0.5f * b * h; } }
class Bag { array<int> items; int sum() { int s = 0; for( uint i = 0; i < items.length(); i++ ) s += items[i]; return s; } }
enum Op { Add, Sub, Mul, Xor, Shl }
int intArraySum(int n) {
  array<int> a(1000);
  for( uint i = 0; i < a.length(); i++ ) a[i] = i;
  int s = 0;
  for( int k = 0; k < n / 1000; k++ )
    for( uint i = 0; i < a.length(); i++ ) s += a[i];
  return s;
}
int floatArrayDot(int n) {
  array<float> a(1000), b(1000);
  for( uint i = 0; i < a.length(); i++ ) { a[i] = i * 0.5f; b[i] = 1.0f / (i + 1); }
  float s = 0;
  for( int k = 0; k < n / 1000; k++ )
    for( uint i = 0; i < a.length(); i++ ) s += a[i] * b[i];
  return int(s);
}
int doubleMath(int n) {
  double x = 1;
  for( int i = 0; i < n; i++ ) x = x * 1.0000001 + sqrt(x) * 0.000001;
  return int(x);
}
int sqrtSum(int n) {
  float s = 0;
  for( int i = 0; i < n; i++ ) s += sqrt(float(i & 1023));
  return int(s);
}
int switchOps(int n) {
  int s = 1;
  for( int i = 0; i < n; i++ ) {
    switch( i % 5 ) {
      case Add: s += i; break;
      case Sub: s -= i >> 1; break;
      case Mul: s *= 3; break;
      case Xor: s ^= i; break;
      default: s = (s << 1) | (i & 1);
    }
  }
  return s;
}
int funcdefCalls(int n) {
  BinOp@ f = add;
  int s = 0;
  for( int i = 0; i < n; i++ ) s = f(s, i);
  return s;
}
int directCalls(int n) {
  int s = 0;
  for( int i = 0; i < n; i++ ) s = add(s, i);
  return s;
}
int add2(int a, int b) { int r = a; for( int k = 0; k < 1; k++ ) r += b; return r; }
int funcdefCalls2(int n) {
  BinOp@ f = add2;
  int s = 0;
  for( int i = 0; i < n; i++ ) s = f(s, i);
  return s;
}
int directCalls2(int n) {
  int s = 0;
  for( int i = 0; i < n; i++ ) s = add2(s, i);
  return s;
}
int apply(BinOp@ f, int n) { int s = 0; for( int i = 0; i < n; i++ ) s = f(s, i); return s; }
int callbackCalls(int n) { return apply(add, n); }
int callbackCalls2(int n) { int s = 0; for( int k = 0; k < n / 100; k++ ) s += apply(k % 2 == 0 ? @add : @add2, 100); return s; }
int shapes(int n) {
  array<IShape@> list = { Circle(1), Square(2), Tri(3), Circle(4) };
  float s = 0;
  for( int k = 0; k < n / 4; k++ )
    for( uint i = 0; i < list.length(); i++ ) s += list[i].area();
  return int(s);
}
int globals(int n) {
  for( int i = 0; i < n; i++ ) g_counter += i & 3;
  return g_counter;
}
int memberArray(int n) {
  Bag b;
  b.items.resize(100);
  for( uint i = 0; i < 100; i++ ) b.items[i] = i;
  int s = 0;
  for( int k = 0; k < n / 100; k++ ) s += b.sum();
  return s;
}
int int64Math(int n) {
  int64 h = 1469598103934665603;
  for( int i = 0; i < n; i++ ) { h ^= i; h *= 1099511628211; }
  return int(h & 0xFFFF);
}
int handles(int n) {
  Circle c(1); IShape@ s = c; int k = 0;
  for( int i = 0; i < n; i++ ) { Circle@ x = cast<Circle>(s); if( x !is null ) k += 1; }
  return k;
}
int castBase(int n) {
  Ring c(1); IShape@ s = c; int k = 0;
  for( int i = 0; i < n; i++ ) { Circle@ x = cast<Circle>(s); if( x !is null ) k += 1; }
  return k;
}
int castIntf(int n) {
  Ring c(1); IShape@ s = c; int k = 0;
  for( int i = 0; i < n; i++ ) { IRound@ x = cast<IRound>(s); if( x !is null ) k += 1; }
  return k;
}
int stringOps(int n) {
  string s = "hello world";
  int k = 0;
  for( int i = 0; i < n; i++ ) { k += s.findFirst("world") + s.substr(1, 3).length(); }
  return k;
}
int dictionaryOps(int n) {
  dictionary d;
  for( int i = 0; i < 100; i++ ) d.set("k" + i, i);
  int64 s = 0;
  for( int i = 0; i < n; i++ ) { int64 v; d.get("k50", v); s += v; }
  return int(s);
}
int nested(int n) {
  int s = 0;
  for( int i = 0; i < n / 100; i++ )
    for( int j = 0; j < 100; j++ )
      if( (i + j) % 3 == 0 ) s += j; else s -= 1;
  return s;
}
class Vec2 {
  float x, y;
  Vec2() { x = 0; y = 0; }
  Vec2(float ax, float ay) { x = ax; y = ay; }
  Vec2 opAdd(const Vec2 &in o) const { return Vec2(x + o.x, y + o.y); }
  Vec2 opMul(float s) const { return Vec2(x * s, y * s); }
}
float vec2Test(int n) {
  Vec2 a(1, 2), b(0.5f, 0.25f);
  for( int i = 0; i < n; i++ )
    a = a + b * 0.001f;
  return a.x + a.y;
}
float nativeTest(int n) {
  float s = 0;
  for( int i = 0; i < n; i++ )
    s = nadd(s, 0.5f);
  return s;
}
float genericTest(int n) {
  float s = 0;
  for( int i = 0; i < n; i++ )
    s = gadd(s, 0.5f);
  return s;
}
class Entity {
  float x = 0, v = 1;
  void update(float dt) { x += v * dt; if( x > 100 ) x = 0; }
}
float arrayTest(int n) {
  array<Entity@> list;
  for( int i = 0; i < 100; i++ ) list.insertLast(Entity());
  for( int k = 0; k < n / 100; k++ )
    for( uint i = 0; i < list.length(); i++ )
      list[i].update(0.016f);
  return list[0].x;
}
int stringTest(int n) {
  int total = 0;
  for( int i = 0; i < n; i++ ) {
    string s = "item" + i + ": " + (i * 2);
    total += s.length();
  }
  return total;
}
)";

// The functions to measure and how many iterations they make
struct STest
{
	const char *decl;
	int         iterations;
};

static const STest tests[] =
{
	{ "int intArraySum(int)", 20000000 },
	{ "int floatArrayDot(int)", 20000000 },
	{ "int doubleMath(int)", 20000000 },
	{ "int sqrtSum(int)", 20000000 },
	{ "int switchOps(int)", 20000000 },
	{ "int funcdefCalls(int)", 20000000 },
	{ "int directCalls(int)", 20000000 },
	{ "int funcdefCalls2(int)", 20000000 },
	{ "int directCalls2(int)", 20000000 },
	{ "int callbackCalls(int)", 20000000 },
	{ "int callbackCalls2(int)", 20000000 },
	{ "int shapes(int)", 20000000 },
	{ "int globals(int)", 20000000 },
	{ "int memberArray(int)", 20000000 },
	{ "int int64Math(int)", 20000000 },
	{ "int handles(int)", 20000000 },
	{ "int castBase(int)", 20000000 },
	{ "int castIntf(int)", 20000000 },
	{ "int stringOps(int)", 2000000 },
	{ "int dictionaryOps(int)", 2000000 },
	{ "int nested(int)", 20000000 },
	{ "float vec2Test(int)", 2000000 },
	{ "float nativeTest(int)", 20000000 },
	{ "float genericTest(int)", 20000000 },
	{ "float arrayTest(int)", 20000000 },
	{ "int stringTest(int)", 1000000 }
};
static const int NUM_TESTS = int(sizeof(tests) / sizeof(tests[0]));

struct SResult
{
	double time;  // seconds per iteration, the fastest of the runs
	double value; // the value returned, which the VM and the JIT compiler must agree on
	bool   ok;
};

// Builds the script and measures the tests with an engine using the compiler or the VM
static bool Measure(CJITCompiler *jit, const char *only, SResult *results)
{
	asIScriptEngine *engine = asCreateScriptEngine();
	engine->SetMessageCallback(asFUNCTION(MessageCallback), 0, asCALL_CDECL);
	if( jit )
	{
		JIT_AddScriptArrayIndexers(jit);
		engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, true);
		engine->SetJITCompiler(jit);
	}
	RegisterStdString(engine);
	RegisterScriptArray(engine, true);
	RegisterScriptMath(engine);
	RegisterScriptDictionary(engine);
	engine->RegisterGlobalFunction("float nadd(float, float)", asFUNCTION(NativeAdd), asCALL_CDECL);
	engine->RegisterGlobalFunction("float gadd(float, float)", asFUNCTION(GenericAdd), asCALL_GENERIC);

	asIScriptModule *mod = engine->GetModule("bench", asGM_ALWAYS_CREATE);
	mod->AddScriptSection("bench", script);
	if( mod->Build() < 0 )
	{
		engine->ShutDownAndRelease();
		return false;
	}

	asIScriptContext *ctx = engine->CreateContext();
	for( int n = 0; n < NUM_TESTS; n++ )
	{
		results[n].ok = false;
		if( only && !strstr(tests[n].decl, only) )
			continue;
		asIScriptFunction *func = mod->GetFunctionByDecl(tests[n].decl);
		if( func == 0 )
		{
			printf("%s: not found\n", tests[n].decl);
			continue;
		}
		double best = 1e30;
		int r = asEXECUTION_FINISHED;
		for( int run = 0; run < 3 && r == asEXECUTION_FINISHED; run++ )
		{
			ctx->Prepare(func);
			ctx->SetArgDWord(0, asDWORD(tests[n].iterations));
			double start = Now();
			r = ctx->Execute();
			double time = Now() - start;
			if( time < best )
				best = time;
		}
		if( r != asEXECUTION_FINISHED )
		{
			printf("%s: %s\n", tests[n].decl, r == asEXECUTION_EXCEPTION ? ctx->GetExceptionString() : "failed");
			continue;
		}
		results[n].time  = best / tests[n].iterations;
		results[n].value = func->GetReturnTypeId() == asTYPEID_FLOAT ? double(ctx->GetReturnFloat()) : double(int(ctx->GetReturnDWord()));
		results[n].ok    = true;
	}
	ctx->Release();
	engine->ShutDownAndRelease();
	return true;
}

int main(int argc, char **argv)
{
	const char *only = 0;
	const char *log = 0;
	bool vm = true, compiled = true;
	for( int n = 1; n < argc; n++ )
	{
		if( strcmp(argv[n], "--only") == 0 && n + 1 < argc )
			only = argv[++n];
		else if( strcmp(argv[n], "--log") == 0 && n + 1 < argc )
			log = argv[++n];
		else if( strcmp(argv[n], "--vm") == 0 )
			compiled = false;
		else if( strcmp(argv[n], "--jit") == 0 )
			vm = false;
		else
		{
			printf("usage: test_jit_bench [--only <text>] [--log <text>] [--vm] [--jit]\n");
			return 1;
		}
	}

	SResult vmResults[NUM_TESTS], jitResults[NUM_TESTS];
	if( vm && !Measure(0, only, vmResults) )
		return 1;
	if( compiled )
	{
		// The compiler must outlive the engine
		CJITCompiler jit(log ? CJITCompiler::JIT_LOG : 0);
		if( log )
			jit.SetLogFile(stdout, log);
		if( !Measure(&jit, only, jitResults) )
			return 1;
	}

	// The time per iteration in nanoseconds
	bool same = true;
	printf("%-28s %10s %10s %8s\n", "Test", "VM", "JIT", "Speedup");
	for( int n = 0; n < NUM_TESTS; n++ )
	{
		if( only && !strstr(tests[n].decl, only) )
			continue;
		printf("%-28s", tests[n].decl);
		if( vm && vmResults[n].ok ) printf(" %10.2f", vmResults[n].time * 1e9); else printf(" %10s", "-");
		if( compiled && jitResults[n].ok ) printf(" %10.2f", jitResults[n].time * 1e9); else printf(" %10s", "-");
		if( vm && compiled && vmResults[n].ok && jitResults[n].ok )
		{
			printf(" %7.1fx", vmResults[n].time / jitResults[n].time);
			if( vmResults[n].value != jitResults[n].value )
			{
				printf("  returned %g instead of %g", jitResults[n].value, vmResults[n].value);
				same = false;
			}
		}
		printf("\n");
	}
	return same ? 0 : 1;
}
