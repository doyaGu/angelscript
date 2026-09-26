#ifndef JIT_CPPGEN_H
#define JIT_CPPGEN_H

#include "jit_bytecode.h"

#include <string>
#include <vector>

BEGIN_AS_NAMESPACE

// Translates the decoded bytecode of one function to a C++ function with the
// signature JITFunction, which the AOT output compiles into the application, see
// CJITCompiler::SetAOTOutput. The code executes each instruction like the VM does,
// with the frame pointer, the stack pointer, and the value register in local
// variables, and returns to the VM where the VM would raise an exception.
//
// The primitive variables that the analysis would keep in registers are local
// variables of the C++ function too, which the compiler keeps in registers. They
// are stored to the frame where the VM, the engine, or the application may see it,
// and loaded again where it may have been modified, at the same points as the JIT
// compiler does, see CJITByteCode::GetDirtyMask. The code must be generated from
// the analysis of CJITByteCode::AnalyseForAOT.
//
// The operands that differ between modules and engines, i.e. pointers, function
// ids, and type ids, are read from the bytecode at run time, so the same code
// serves every function whose bytecode only differs in them. Everything else is
// part of the key of the function, see JIT_GetAOTKey, and the code must depend on
// nothing that the key leaves out
class CJITCppGen
{
public:
	CJITCppGen(const CJITByteCode &code);

	// Appends the definition of the function with the name. Returns false if the
	// bytecode can't be translated
	bool Generate(const char *name, std::string &out);

	// The object variables on the heap that the function clears when entered, by offset
	static void GetHeapVariables(asCScriptFunction *func, std::vector<int> &offsets);

	// Returns true if the instruction calls script functions, which the code may
	// execute natively
	static bool CallsScript(asEBCInstr op);

protected:
	// A variable kept in a local variable
	struct SLocal
	{
		int         offset;
		int         kind;  // JIT_SLOT_I32, I64, F32, or F64
		int         bit;   // bit in the masks of the analysis
		std::string name;
		bool        used;  // whether the function declares it
		bool        read;
	};

	void Emit(const char *format, ...);
	void Put(const std::string &line);
	void EmitEntry(bool calls);
	bool EmitInstr(asUINT idx);
	void EmitSync(const char *indent = "");
	void EmitReload(const char *indent = "");

	SLocal     *FindLocal(int offset);
	std::string Var(const char *type, int offset);
	std::string SetVar(const char *type, int offset, const std::string &value);
	std::string SetVarLow(const char *type, int offset, const std::string &value);
	std::string VarAddr(int offset);
	std::string Stores(asUINT mask);
	std::string Loads(asUINT mask);
	std::string Bail() const;

	static const char *LocalType(int kind);

	const CJITByteCode &m_code;
	std::string         m_out;
	std::vector<bool>   m_labels; // instructions that are jumped to
	std::vector<SLocal> m_locals;
	bool                m_failed; // an instruction accesses a local variable in a way it can't

	// For the instruction being translated, its position and the statements that
	// store the local variables to the frame before it may be seen, and that load
	// them after it may have been modified
	asUINT              m_pos;
	std::string         m_sync;
	std::string         m_reload;
};

END_AS_NAMESPACE

#endif
