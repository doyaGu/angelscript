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
// nothing that the key leaves out.
//
// The script calls to the functions whose code is generated too call the code
// directly if the function called at run time has that code, see AOT_PopCall. The
// lines of the code for that are a region, which begins with a line of
// JIT_CPPGEN_REGION followed by the name of the code called, and ends with a line of
// JIT_CPPGEN_REGION_END. The output keeps the region, without the two lines, if it
// writes the code called, and removes it otherwise. The code without the regions
// depends on the key only
const char JIT_CPPGEN_REGION     = '\x01';
const char JIT_CPPGEN_REGION_END = '\x02';

class CJITCppGen
{
public:
	// Returns the name of the code generated for the function, or an empty string if
	// there is none
	typedef std::string (*TargetCallback)(asCScriptFunction *func, void *param);

	CJITCppGen(const CJITByteCode &code, TargetCallback target = 0, void *targetParam = 0);

	// Appends the definition of the function with the name, or of its direct entry,
	// which has the name with _d appended, see AOT_PopCall. Returns false if the
	// bytecode can't be translated
	bool Generate(const char *name, std::string &out, bool direct = false);

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
	void EmitScriptCall(const SJITInstr &instr);

	static asCScriptFunction *FindCallee(asCScriptFunction *func, bool virtualCall);

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
	TargetCallback      m_target;
	void               *m_targetParam;
	bool                m_direct; // generating the direct entry
	std::string         m_out;
	std::vector<bool>   m_labels; // instructions that are jumped to
	std::vector<SLocal> m_locals;
	bool                m_failed; // an instruction accesses a local variable in a way it can't

	// For the instruction being translated, its position and the statements that
	// store the local variables to the frame before it may be seen, and that load
	// them after it may have been modified, and whether the frame must be stored
	// before it may be seen, see JIT_FRAME_BIT
	asUINT              m_pos;
	std::string         m_sync;
	std::string         m_reload;
	bool                m_frame;
};

END_AS_NAMESPACE

#endif
