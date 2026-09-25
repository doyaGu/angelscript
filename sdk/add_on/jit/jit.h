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
};

// Callback used to decide if a function should be JIT compiled
typedef bool (*JITCompileFilterFunc_t)(asIScriptFunction *func, void *userParam);

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
		JIT_NO_INLINE = 0x80
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
	// class of the module that implements them, and the objects of other classes call
	// them. Each function inlines at most 16 times the size, and each function that is
	// compiled in place 4 times the size. The functions are still compiled on their
	// own for the other calls. Default is 64, 0 disables inlining like JIT_NO_INLINE
	void SetMaxInlineSize(asUINT sizeInDWords);

	SJITStatistics GetStatistics() const;

protected:
	struct SImpl;
	SImpl *m_impl;
};

END_AS_NAMESPACE

#endif
