#ifndef JIT_CODEGEN_H
#define JIT_CODEGEN_H

#include "jit_bytecode.h"

// The code generator must agree with the engine on the size of booleans
#include "as_config.h"
#ifndef AS_SIZEOF_BOOL
#error AS_SIZEOF_BOOL must be defined by as_config.h
#endif

#include <asmjit/ujit.h>
#include <vector>
#include <map>

BEGIN_AS_NAMESPACE

struct SJITCodeGenOptions
{
	bool noSuspend;         // don't emit the suspend checks
	bool noScriptCalls;     // return to the VM for script-to-script calls
	bool syncEveryInstr;    // update the VM registers after every instruction
	bool directSystemCalls; // call registered functions with their native calling convention
	bool guardedEntry;      // enter through JIT_GuardedEntry when called by the VM
	asUINT maxNativeCallDepth; // nested native calls allowed when entered by the VM
};

// Translates the analysed bytecode of one function to machine code through
// AsmJit's arch neutral UniCompiler. The generated function has the signature
// JITFunction, see jit_runtime.h. The argument passed with the JitEntry
// instruction is the 1-based index of the entry point, and native callers
// pass 0 to enter at the start of the function.
//
// Register usage: the stack frame pointer, stack pointer, and value register
// are kept in virtual registers while executing natively, except for the stack
// pointer if the depth of the stack is static, and primitive variables that
// are never accessed by address may be kept in registers too.
// They are written back to the VM registers/stack before anything that can
// observe them, and reloaded whenever execution comes back from the VM.
class CJITCodeGen
{
public:
	CJITCodeGen(asmjit::ujit::UniCompiler &uc, const CJITByteCode &code, const SJITCodeGenOptions &options);

	// Emits the whole function. Returns false if something couldn't be compiled
	bool   Generate();

	// The function node, whose frame is final once the compiler has been finalized
	asmjit::FuncNode *GetFuncNode() const { return m_func; }

	// True if the function is entered through JIT_GuardedEntry, in which case C++
	// exceptions may pass through it and it needs unwind information
	bool   IsGuarded() const { return m_guarded; }

	asUINT GetInstructionCount() const { return m_instrCount; }
	asUINT GetBailCount() const        { return m_bailCount; }

protected:
	typedef asmjit::ujit::Gp        Gp;
	typedef asmjit::ujit::Vec       Vec;
	typedef asmjit::ujit::Mem       Mem;
	typedef asmjit::Label           Label;
	typedef asmjit::Imm             Imm;
	typedef asmjit::ujit::UniCondition Cond;

	// Register cache entry for a variable
	struct SCachedSlot
	{
		int  offset;
		int  kind;  // EJITSlotKind
		Gp   gp;
		Vec  vec;
	};

	// A registered behaviour that takes nothing but the object, e.g. AddRef or
	// Release, called directly with its native calling convention
	struct SDirectBehaviour
	{
		const void         *func;      // for virtual methods the offset in the virtual function table plus 1
		bool                isVirtual;
		asmjit::CallConvId  conv;
	};

	// Prologue, entry dispatch, and epilogue
	void EmitPrologue();
	bool  EntryNeedsStub(asUINT n) const;
	Label EntryTarget(asUINT n);
	void EmitEntryDispatch(asUINT lo, asUINT hi);
	void EmitEntryStubs();
	void EmitBailStubs();
	void EmitDirectEntry();
	void AssignHomeRegs(asUINT slotMask);
	void SetSlotHomeHints(asUINT slotMask, const uint32_t *gpIds, asUINT gpCount, const uint32_t *vecIds, asUINT vecCount);
	void CopyLiveArgs();
	bool IsLiveThrough(const asmjit::Reg &reg) const;

	// Emits one instruction. Returns false if the instruction isn't supported
	bool EmitInstruction(asUINT idx);

	// Instruction groups (jit_codegen.cpp)
	bool EmitStackOp(const SJITInstr &instr);
	bool EmitLoadStore(const SJITInstr &instr);
	bool EmitBranch(asUINT idx);
	bool EmitMisc(asUINT idx);

	// Arithmetic, compare, and conversion (jit_codegen_math.cpp)
	bool EmitIntMath(asUINT idx);
	bool EmitFloatMath(asUINT idx);
	bool EmitIncDec(const SJITInstr &instr);
	bool EmitCompare(asUINT idx, asUINT &consumed);
	bool EmitConversion(asUINT idx);
	void EmitDivMod(asUINT idx, bool is64, bool isSigned, bool isMod);
	void EmitCompareResult(const Gp &dst, const Cond &lt, const Cond &gt);
	void EmitFloatCompareResult(const Gp &dst, const Vec &a, const Vec &b, bool isDouble);
	void EmitFloatTest(const Gp &dst, asEBCInstr test, const Vec &a, const Vec &b, bool isDouble);

	// Calls and objects (jit_codegen_call.cpp)
	bool EmitCall(asUINT idx);
	bool EmitDirectSystemCall(asUINT idx, int funcId);
	bool EmitObjectOp(asUINT idx);
	bool GetDirectBehaviour(int funcId, SDirectBehaviour &beh) const;
	bool CallsBehaviourDirectly(const SJITInstr &instr) const;
	void EmitBehaviourCall(const SDirectBehaviour &beh, const Gp &obj);
	void EmitScriptCall(asUINT idx, int kind, int funcId, const Gp *extra, asPWORD extraImm);
	Gp   EmitFindMethod(asCScriptFunction *method, const Label &slow);
	bool EmitNativeCall(asUINT idx, const Gp &target, const Gp &result, const Label &slow, bool mark, bool vrInReg);
	void EmitAfterHelperCall(const Gp &result, asUINT idx);
	void EmitReloadAfterCall(asUINT idx, bool reloadVR = true);

	// Architecture specific code (jit_codegen_arch.cpp)
	void SetHomeRegHints(asUINT slotMask);
	void EmitSignedDiv(const Gp &dst, const Gp &a, const Gp &b, bool isMod);
	bool EmitFloatCompareBranch(const Vec &a, const Vec &b, bool isDouble, asEBCInstr branch, const Label &target);
	Mem  PtrElement(const Gp &array, const Gp &index);
	void SetSignBit(const Gp &r);
	void AddVRReturn(asmjit::FuncDetail &detail);

	// Access to the VM registers
	Mem  RegsField(size_t offset);
	Mem  ContextField(int offset);  // offset from SJITContextLayout
	Mem  VRMem();
	Mem  Var(int offset, int byteDisp = 0);
	Mem  Stack(int dwordOffset);
	Mem  Global(asPWORD address, Gp &tmp);

	// Variable access respecting the register cache. The Load* functions
	// return either the cached register or a fresh temporary. The Dst*
	// functions return the register to compute the result into, which must be
	// passed to Commit* afterwards to store it if the variable isn't cached
	SCachedSlot *FindCached(int offset);
	Gp   Load32(int offset);
	Gp   Load64(int offset);
	Gp   LoadPtr(int offset);
	Vec  LoadF32(int offset);
	Vec  LoadF64(int offset);
	Gp   Dst32(int offset);
	Gp   Dst64(int offset);
	Vec  DstF32(int offset);
	Vec  DstF64(int offset);
	void Commit32(int offset, const Gp &value);
	void Commit64(int offset, const Gp &value);
	void CommitF32(int offset, const Vec &value);
	void CommitF64(int offset, const Vec &value);
	void StorePtr(int offset, const Gp &value);
	void Copy64(const Mem &dst, const Mem &src);
	void Copy32(const Mem &dst, const Mem &src);

	// Value register access
	void LoadVR32(const Gp &dst);
	void LoadVR64(const Gp &dst);
	void LoadVRPtr(const Gp &dst);
	void StoreVR32(const Gp &src);
	void StoreVR64(const Gp &src);
	void StoreVRPtr(const Gp &src);
	void StoreVRImm32(int value);
	void SyncVR();
	void ReloadVR();

	// Addresses in the value register. SetVRAddr leaves the address to the memory
	// operand of the next instruction if that only dereferences it and the register
	// isn't read afterwards, and VRAddr returns the memory the register points to
	bool CanFoldVRAddr(asUINT idx) const;
	void SetVRAddr(asUINT idx, const Mem &addr);
	Mem  VRAddr();

	// Synchronization with the VM
	void SetPC(asUINT pos);
	void SyncStack();
	void ReloadStack();
	void ReloadStackAfter(asUINT idx); // after a call that has completed the instruction

	// The stack pointer. If the stack is static, it is the frame pointer minus a
	// constant, else it is kept in m_sp
	int  StackOffset(asUINT idx) const; // static offset from fp when the instruction is reached
	void PushStack(int bytes);
	void PopStack(int bytes);
	Gp   StackPointer();                // the stack pointer in a register

	void StoreFrame();
	void StoreCachedSlots();
	void StoreDirtySlots(asUINT mask);
	void ReloadCachedSlots();
	void ReloadSlots(asUINT mask);
	void StoreCachedSlot(int offset);
	void ReloadCachedSlot(int offset);
	void SyncAll(asUINT idx);        // writes back what the VM may observe at the instruction
	void SyncForCall(asUINT idx);    // like SyncAll but without the value register, which calls clobber
	void SyncAllSlots(asUINT pos);   // writes back everything, program pointer set to pos
	void ReloadAll(asUINT idx);      // loads what the VM may have changed when continuing after the instruction
	void ReloadLiveSlots(asUINT idx); // loads the cached slots read after the instruction

	// Leaves native code. Bail makes the VM re-execute the instruction
	void  Bail(asUINT idx);
	Label BailLabel(asUINT idx);
	void  Leave();
	void  EmitLeaveIf(const Gp &result);
	Label InstrLabel(asUINT idx);

	// Rare paths, e.g. the calls of the helpers that hand control to the VM, are
	// emitted in place between BeginCold and EndCold, but moved behind the body by
	// EmitColdCode. The code before a cold range continues after it
	asmjit::BaseNode *BeginCold(const Label &label);
	void  EndCold(asmjit::BaseNode *start, const Label &cont);
	void  EmitColdCode();

	// Calls a C function. Arguments and return value are set on the returned node
	asmjit::InvokeNode *Invoke(const void *fn, const asmjit::FuncSignature &sig);

	// Materializes a pointer/word constant
	Gp   PtrConst(asPWORD value);

	bool Is64Bit() const { return m_uc.is_64bit(); }

	asmjit::ujit::UniCompiler &m_uc;
	const CJITByteCode        &m_code;
	SJITCodeGenOptions         m_options;
	asmjit::FuncNode          *m_func;

	Gp  m_regs;     // asSVMRegisters*
	Gp  m_arg;      // jitArg
	Gp  m_callLimit; // call stack length up to which native calls push, only if the function calls script functions
	Gp  m_fp;       // stack frame pointer
	Gp  m_sp;       // stack pointer, unless the stack is static
	Gp  m_vr;       // value register (64bit hosts only)
	Gp  m_bailPC;   // program pointer to set when bailing
	bool m_vrInReg;
	bool m_staticStack;
	int  m_spOffset;    // offset of the stack pointer from fp if the stack is static
	bool m_guarded;
	Mem  m_vrAddr;      // the address left to the next instruction by SetVRAddr
	bool m_vrAddrValid;

	std::vector<SCachedSlot>   m_cached;
	std::map<int, asUINT>      m_cachedIndex;
	std::vector<Label>         m_labels;       // per instruction, valid for block starts
	std::vector<Label>         m_entryLabels;  // per entry
	std::vector<std::pair<Label, asUINT> > m_bails;  // bail stubs to emit
	std::vector<std::pair<asmjit::BaseNode*, asmjit::BaseNode*> > m_cold;  // first and last nodes of the cold ranges
	Label                      m_bailCommon;
	Label                      m_leave;        // returns 1, i.e. the VM takes over

	asUINT m_instrCount;
	asUINT m_bailCount;
	bool   m_failed;
};

END_AS_NAMESPACE

#endif
