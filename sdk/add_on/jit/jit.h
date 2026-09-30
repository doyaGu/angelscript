#ifndef JIT_H
#define JIT_H

#ifndef ANGELSCRIPT_H
// Avoid having to inform include path if header is already include before
#include <angelscript.h>
#endif

#include <stdio.h>

BEGIN_AS_NAMESPACE

// Statistics gathered by the JIT compiler
struct SJITStatistics
{
	asUINT functionsCompiled;    // script functions successfully compiled
	asUINT functionsFailed;      // script functions that couldn't be compiled (they are interpreted)
	asUINT functionsReleased;    // compiled functions released again
	asUINT instructionsCompiled; // bytecode instructions translated to native code
	asUINT instructionsBailed;   // bytecode instructions that always return control to the VM
	asUINT callsInlined;         // script calls whose function was compiled in place, see SetMaxInlineSize
	size_t codeSize;             // total size of the native code currently held
	asUINT functionsAOT;         // script functions that use the code generated ahead of time, see AddAOTFunctions
	asUINT functionsDeferred;    // script functions whose compilation was deferred, see SetCompileThresholds
	asUINT functionsRecompiled;  // compiled functions compiled again with the classes seen by their calls, see SetProfileThreshold
	asUINT functionsForLineCallbacks; // compiled functions compiled again with the checks at every statement, see JIT_CHECK_EVERY_STATEMENT
};

// Where a container keeps its elements, see CJITCompiler::AddIndexer. The object
// points to a buffer, which holds the number of elements and then the elements
struct SJITIndexer
{
	int bufferOffset; // offset in the object of the pointer to the buffer, which may be null
	int lengthOffset; // offset in the buffer of the number of elements, an asUINT
	int dataOffset;   // offset in the buffer of the first element
};

// Callback used to decide if a function should be JIT compiled
typedef bool (*JITCompileFilterFunc_t)(asIScriptFunction *func, void *userParam);

// A function generated ahead of time, see CJITCompiler::SetAOTOutput
typedef int (*JITAOTFunction_t)(asSVMRegisters *regs, asPWORD jitArg, asUINT callLimit, asDWORD *stackPointer);
struct SJITAOTFunction
{
	asQWORD          key0; // identifies the bytecode that the function was generated for
	asQWORD          key1;
	JITAOTFunction_t func;
};

#ifdef AS_JIT_AOT_TABLE
// The functions generated ahead of time that the CMake option AS_JIT_AOT_DIR builds
// into the library, for CJITCompiler::AddAOTFunctions
extern const SJITAOTFunction g_jitAOTFunctions[];
extern const asUINT          g_jitAOTFunctionCount;
#endif

// The JIT compiler translates AngelScript bytecode to native machine code with
// the help of the AsmJit library. It implements the version 1 JIT interface, so
// the application only needs to create an instance and register it with the
// engine before compiling any scripts:
//
//   engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, true);
//   engine->SetJITCompiler(jit);
//
// The compiler must outlive the engine, as the engine calls back into it when
// the script functions are destroyed.
class CJITCompiler : public asIJITCompiler
{
public:
	enum EFlags
	{
		JIT_NO_SUSPEND        = 0x01, // don't check for suspension/line callbacks at statement boundaries
		JIT_NO_SCRIPT_CALLS   = 0x02, // return to the VM for script-to-script calls instead of calling natively
		JIT_NO_REGISTER_CACHE = 0x04, // keep all local variables in memory
		JIT_SYNC_EVERY_INSTR  = 0x08, // update the VM registers after every instruction (debugging aid)
		JIT_LOG               = 0x10, // log the bytecode and generated code to the log file

		// Registered functions with simple signatures are called directly with
		// their native calling convention instead of through the engine, which is
		// much faster. C++ exceptions thrown by them are still turned into script
		// exceptions, but that needs unwind information for the generated code,
		// which is only available on some platforms (see CJITUnwindInfo). Elsewhere
		// the direct calls are only made with this flag, and the exceptions can
		// then not pass through the generated code
		JIT_DIRECT_SYSTEM_CALLS = 0x20,

		// Never call registered functions directly
		JIT_NO_DIRECT_SYSTEM_CALLS = 0x40,

		// Always call the script functions instead of compiling small ones in place
		JIT_NO_INLINE = 0x80,

		// Only use the functions generated ahead of time, see AddAOTFunctions, and
		// leave the others to the VM
		JIT_AOT_ONLY = 0x100,

		// Check for suspension and line callbacks at every statement like the VM.
		// Otherwise the code only checks for them where they may have been requested
		// since the last check, i.e. on entry, after calls, and in loops, which is
		// faster, and the functions executed while a line callback is set are compiled
		// again with the checks at every statement. Set this if the line callbacks
		// are always set, e.g. for timeouts, so that the functions are compiled once
		JIT_CHECK_EVERY_STATEMENT = 0x200
	};

	CJITCompiler(asDWORD flags = 0);
	virtual ~CJITCompiler();

	// Returns true if the JIT compiler can generate code for the current CPU
	static bool IsSupported();

	// asIJITCompiler
	virtual int  CompileFunction(asIScriptFunction *function, asJITFunction *output);
	virtual void ReleaseJITFunction(asJITFunction func);

	// Configuration (should be set before any function is compiled)
	void    SetFlags(asDWORD flags);
	asDWORD GetFlags() const;

	// Where to write the log when JIT_LOG is set. If a function name filter is
	// given only functions whose name contains the string are logged
	void SetLogFile(FILE *file, const char *funcNameFilter = 0);

	// Only compile the functions for which the filter returns true
	void SetCompileFilter(JITCompileFilterFunc_t filter, void *userParam);

	// Maximum depth of nested native script-to-script calls. Deeper calls are
	// left to the VM, which unwinds the machine stack. Default is 256
	void SetNativeCallDepth(asUINT depth);

	// Force the listed instructions to always return control to the VM.
	// This is a debugging aid to bisect problems in the code generation
	void SetBailInstructions(const asEBCInstr *instructions, asUINT count);

	// Largest bytecode size (in dwords) that will be compiled
	void SetMaxFunctionSize(asUINT sizeInDWords);

	// Largest bytecode size (in dwords) of the script functions that are compiled
	// in place of their calls. The functions that they call must be compiled in place
	// in theirs, down to 4 levels of calls, and recursive calls are not. The methods
	// called through interfaces and virtual calls are compiled in place for the only
	// class of the module that implements them, or for the classes that inherit a
	// virtual method, and the objects of other classes call them. Each function
	// inlines at most 16 times the size, and each function that is compiled in place
	// 4 times the size. The functions are still compiled on their own for the other
	// calls. Default is 64, 0 disables inlining like JIT_NO_INLINE
	void SetMaxInlineSize(asUINT sizeInDWords);

	// Tiered compilation. With a call threshold the functions aren't compiled when the
	// module is built, but when they have been called that many times, or when one of
	// their loops has run that many iterations, in which case the compiled code goes on
	// with the loop. The VM executes the functions until then. This saves the time and
	// the memory for compiling the functions that are rarely executed. 0 calls compiles
	// all functions when the module is built, which is the default, and 0 iterations
	// doesn't count the loops. The thresholds are limited to 16383. The engine must use
	// this compiler itself, not one that forwards to it. Must be called before any
	// function is compiled. Returns a negative value on failure
	int SetCompileThresholds(asUINT calls, asUINT iterations);

	// Profiles. The virtual and interface calls whose method several classes of the
	// module implement note the classes of their objects, and when the calls of a
	// function have been made that many times, the function is compiled again if a
	// call has seen only one class since the function was compiled, with the method
	// compiled in place for the objects of that class. The calls are counted again
	// otherwise. The objects of other classes call the method, and are noted too, and
	// the calls not made yet go on noting their classes, so a function is compiled at
	// most 3 times. The call that has counted down goes on in the new code, and so do
	// the next calls of the function, and the old code is released with the function.
	// Default is 10000, 0 doesn't note the classes
	void SetProfileThreshold(asUINT calls);

	// Ahead-of-time compilation. With an output directory the compiler generates C++
	// code for every function it is given, which WriteAOTOutput writes to the directory:
	// jit_aot_NNN.cpp with the functions, and jit_aot_functions.cpp with their table,
	// g_jitAOTFunctions. Built into the application, e.g. with the CMake option
	// AS_JIT_AOT_DIR, and added with AddAOTFunctions, the functions are used instead
	// of compiling the script functions with the same bytecode. They only depend on the
	// bytecode, not on the addresses of the functions, types, or variables, so any
	// engine and execution of the application that compiles the same scripts can use
	// them. The functions whose bytecode differs, e.g. as a script has changed, are
	// compiled as usual
	void SetAOTOutput(const char *directory);

	// Only writes the files that have changed, so that they aren't compiled again.
	// Returns a negative value on failure
	int WriteAOTOutput();

	// Must be called before any function is compiled. Returns a negative value on failure
	int AddAOTFunctions(const SJITAOTFunction *functions, asUINT count);

	// Indexers. The registered methods that return a reference to an element of a
	// container, like the opIndex of the arrays, are compiled in place as the loads of
	// the element, with the layout of the container, instead of being called. The
	// methods must be registered with asCALL_THISCALL for a template type with one
	// subtype, take a uint, and return a reference to the element of the index in the
	// buffer. The elements follow each other: the primitives with their size, the
	// handles as pointers, and the other objects as pointers to them. The method is
	// still called for a null buffer or an index out of range, to raise the exception.
	// JIT_AddScriptArrayIndexers adds the opIndex methods of CScriptArray. Must be
	// called before any function is compiled. Returns a negative value on failure
	int AddIndexer(const asSFuncPtr &method, const SJITIndexer &indexer);

	SJITStatistics GetStatistics() const;

	// Faster versions of asIScriptContext::Prepare and Execute, for the application
	// to call the script functions with. They have the same effects, and can be mixed
	// with the methods of the context, which set the arguments and get the return
	// value as usual. Prepare only resets what the last execution changed if it has
	// finished. Execute enters the compiled code of the function directly, instead of
	// through the VM, if the engine uses this compiler and no line callback is set.
	// Otherwise they call the methods of the context
	int Prepare(asIScriptContext *ctx, asIScriptFunction *func);
	int Execute(asIScriptContext *ctx);

	// Memory functions for the engine that are much faster than the default ones for
	// the many small objects of the scripts. The blocks of up to 1 KB that are freed
	// are kept for reuse, and their memory isn't returned to the system. Each thread
	// keeps some of the blocks that it frees. The functions must be set before the
	// first engine is created, and not be reset while memory allocated with them is
	// in use:
	//
	//   asSetGlobalMemoryFunctions(CJITCompiler::AllocMemory, CJITCompiler::FreeMemory);
	static void *AllocMemory(size_t size);
	static void  FreeMemory(void *mem);

protected:
	struct SImpl;
	SImpl *m_impl;

	static int TieredEntry(asSVMRegisters *regs, asPWORD jitArg, asUINT callLimit, asDWORD *stackPointer);
};

// Adds the indexers of the opIndex methods of CScriptArray to the compiler, see
// CJITCompiler::AddIndexer. The layout of the arrays is found by creating some in
// an engine of its own, which doesn't need the add-on to be changed. The function
// is in jit_scriptarray.cpp, which links with the add-on. Must be called before
// any function is compiled. Returns a negative value on failure
int JIT_AddScriptArrayIndexers(CJITCompiler *jit);

END_AS_NAMESPACE

#endif
