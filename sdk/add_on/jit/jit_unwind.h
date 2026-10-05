#ifndef JIT_UNWIND_H
#define JIT_UNWIND_H

#ifndef ANGELSCRIPT_H
#include <angelscript.h>
#endif

#include <asmjit/core.h>
#include <vector>

BEGIN_AS_NAMESPACE

// Unwind information for the generated functions, so that the C++ exceptions
// thrown by registered functions called directly can pass through them on the
// way to JIT_GuardedEntry. AsmJit doesn't produce it, so it is derived from the
// prologue of the function frame:
//
//  - 64bit Windows: an UNWIND_INFO appended to the code and registered with
//    RtlAddFunctionTable.
//  - 64bit x86 on Linux, and AArch64 on Linux and macOS: a DWARF CIE and FDE
//    registered with __register_frame.
//  - 32bit GCC or Clang MinGW with the DWARF unwinder: a 32bit x86 CIE and FDE
//    registered with libgcc.
//  - 32bit x86 with MSVC: nothing is needed, as the exceptions are dispatched
//    through the handlers registered on the stack.
//
// TODO: The BSDs, 32bit MinGW configurations without DWARF, arm64e, and Windows
//       ARM64 could be supported too, but haven't been tested.
class CJITUnwindInfo
{
public:
	CJITUnwindInfo();

	// Returns true if C++ exceptions can pass through the generated code on this platform
	static bool IsSupported();

	// Describes the prologue of the function in the code holder of the compiler,
	// which must have been finalized. On 64bit Windows the unwind data is appended
	// to the code, so this must be done before the code is added to the runtime.
	// Returns false if the prologue can't be described
	bool Prepare(asmjit::BaseCompiler &cc, const asmjit::FuncNode *func);

	// Registers the information once the code has been added to the runtime at the
	// given address. The handle is null if there was nothing to register
	bool Register(void *code, void **handle) const;

	// Unregisters the information before the code is released
	static void Unregister(void *handle);

protected:
	enum EOpKind
	{
		OP_PUSH,     // push of a callee saved register
		OP_ALLOC,    // allocation of the stack frame
		OP_SAVE_GP,  // store of a callee saved general purpose register in the frame
		OP_SAVE_VEC  // store of a callee saved vector register in the frame
	};

	struct SOp
	{
		asUINT end;   // offset after the instruction, relative to the start of the function
		int    kind;  // EOpKind
		asUINT reg;   // physical id of the register pushed or saved
		asUINT value; // bytes allocated, or the offset of the saved register from the stack pointer
	};

	bool AddOps(const asmjit::InstNode *inst, asUINT end);

	std::vector<SOp> m_ops;
	asUINT           m_start;       // offset of the function in the code
	asUINT           m_end;         // end of the function in the code
	asUINT           m_tableOffset; // 64bit Windows: offset of the RUNTIME_FUNCTION in the code
};

END_AS_NAMESPACE

#endif
