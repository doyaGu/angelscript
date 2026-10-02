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
	bool elideSuspend;      // leave out the suspend checks that follow others, see CJITCodeGen::EmitBody
	bool noScriptCalls;     // return to the VM for script-to-script calls
	bool syncEveryInstr;    // update the VM registers after every instruction
	bool directSystemCalls; // call registered functions with their native calling convention
	bool guardedEntry;      // enter through JIT_GuardedEntry when called by the VM
	asUINT maxNativeCallDepth; // nested native calls allowed when entered by the VM
	bool interop;           // set the current function for the native calls and don't mark their call states, for the functions generated ahead of time, see JITFunction
	const void *tieredEntry; // the code of the functions whose compilation is deferred, whose calls are left to the helpers unless interop is set, or null
	SJITProfile *profile;    // where the calls marked with JIT_INSTR_PROFILE note their classes, or null
	const void *recompile;   // int (*)(SJITProfile*), called when the profile has counted down the calls, returns non-zero if the function has new code, which the VM goes on in after the call
	const void *exactEntry;  // int (*)(void *exactParam, asSVMRegisters*, asPWORD jitArg), called in place of the code when the VM enters it while a line callback is set or a suspension is requested, if elideSuspend is set, see CJITCodeGen::Generate
	void       *exactParam;
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
// are never accessed by address may be kept in registers too. The functions
// that call script functions hold the length of the call stack as well.
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
	asUINT GetInlinedCallCount() const { return m_callsInlined; }
	asUINT GetProfiledCallCount() const { return m_callsProfiled; } // the calls that note their classes in the profile

	// The name of the first instruction that no code could be generated for, or null
	const char *GetFailedInstruction() const { return m_failedOp < 0 ? 0 : asBCInfo[m_failedOp].name; }

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

	// The code generation state of a function: the one being compiled, which is
	// frame 0, or one inlined into it. The members for the frame being emitted,
	// m_code, m_cached, m_cachedIndex, m_labels, and m_frameBase, are swapped in
	// by SwitchFrame
	struct SFrame
	{
		const CJITByteCode       *code;
		int                       base;  // dwords from the frame pointer down to the frame of the function
		std::vector<SCachedSlot>  cached;
		std::map<int, asUINT>     cachedIndex;
		std::vector<Label>        labels;
		int                       caller;   // frame of the calling function, -1 for frame 0
		asUINT                    callIdx;  // the call instruction in the caller
		Label                     ret;      // where the code continues after the function has returned
		Label                     exit;     // hands the function to the VM, see EmitInlineExit
		bool                      exitUsed;
		asUINT                    borrowed; // the parameters that borrow the references of the caller, see CJITByteCode::AnalyseBorrows
		bool                      retChecked; // the suspend requests are checked before all returns, see EmitBody
	};

	// A bail stub to emit after the body
	struct SBail
	{
		Label  label;
		asUINT idx;
		int    frame;
		bool   materialized; // the frames of the inlined calls are on the call stack
		bool   borrowed;     // and some of them have borrowed parameters
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
	void TakeHomeReg(const Gp &reg, const uint32_t *gpIds, asUINT &gpCount);
	void CallStackLength(const Gp &dst);
	void CopyLiveArgs();
	bool IsLiveThrough(const asmjit::Reg &reg) const;

	// Emits the instructions of the frame in bytecode order and notes those that
	// call functions in calls. Sets m_failed if one isn't supported
	void EmitBody(std::vector<bool> &calls);
	void SwitchFrame(int frame);
	void CreateCachedSlots();
	void CreateBlockLabels();
	bool FailIfHidden();

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
	void EmitIndexer(asUINT idx);
	bool EmitObjectOp(asUINT idx);
	bool GetDirectBehaviour(int funcId, SDirectBehaviour &beh) const;
	bool CallsBehaviourDirectly(const SJITInstr &instr) const;
	bool IsBorrowed(asUINT idx) const;
	void EmitBehaviourCall(const SDirectBehaviour &beh, const Gp &obj);
	void EmitScriptAddRef(asUINT idx, const Gp &obj);
	void EmitScriptRelease(asUINT idx, const Gp &obj);
	void EmitScriptCall(asUINT idx, int kind, int funcId, const Gp *extra, asPWORD extraImm);
	void EmitInlineCall(asUINT idx);
	void EmitInlineExit(int frame);
	void EmitMaterialize();
	void EmitDematerialize();
	Gp   FramePointer(int base);
	void EmitInlineRoom();
	void EmitStackBlockCheck(int extent, const Label &none);
	void GetInlineRoom(asUINT idx, int &extent, int &depth) const;
	Gp   EmitFindMethod(asCScriptFunction *method, const Label &slow, asCObjectType **seen);
	asCObjectType **ProfileCell(asUINT idx);
	Gp   EmitCountDown();
	void EmitRecompile(asUINT idx);
	bool EmitNativeCall(asUINT idx, const Gp &target, const Gp &callee, const Gp &result, const Label &slow, bool mark, bool vrInReg);
	void EmitAfterHelperCall(const Gp &result, asUINT idx);
	void EmitReloadAfterCall(asUINT idx, bool reloadVR = true);

	// Architecture specific code (jit_codegen_arch.cpp)
	void SetHomeRegHints(asUINT slotMask);
	int  RegsBias() const;
	void EmitSignedDiv(const Gp &dst, const Gp &a, const Gp &b, bool isMod);
	bool EmitFloatCompareBranch(const Vec &a, const Vec &b, bool isDouble, asEBCInstr branch, const Label &target);
	Mem  PtrElement(const Gp &array, const Gp &index);
	Mem  Addr(const Gp &base, int32_t disp);
	void Lea(const Gp &dst, const Mem &src);
	void SetSignBit(const Gp &r);
	void AddVRReturn(asmjit::FuncDetail &detail);
	void AddReturn(asmjit::FuncDetail &detail, int index, asmjit::TypeId type);
	void StoreImm32(const Mem &dst, int value);
	void MoveVec(const Vec &dst, const Vec &src);
	void MoveFloatImm(const Vec &dst, asQWORD bits, bool isDouble);
	void EmitAddRefInPlace(const Gp &obj, const Label &slow);
	bool EmitReleaseInPlace(const Gp &obj, const Label &slow, const Label &race);
	void EmitRefCountInc(const Gp &obj);

	// Calls a C function. Arguments and return value are set on the returned node
	asmjit::InvokeNode *Invoke(const void *fn, const asmjit::FuncSignature &sig);

	// Access to the VM registers
	void SetRegsArg(asmjit::InvokeNode *call, uint32_t index);
	Mem  RegsField(size_t offset);
	Mem  ContextField(int offset);  // offset from SJITContextLayout
	Mem  VRMem();
	Mem  Var(int offset, int byteDisp = 0);
	void LeaVar(const Gp &dst, int offset);
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
	void ReloadThis();
	asUINT HeldFields(asUINT idx) const;
	void ReloadFields(asUINT mask);

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
	Label LeaveLabel();
	Label InstrLabel(asUINT idx);

	// Rare paths, e.g. the calls of the helpers that hand control to the VM, are
	// emitted in place between BeginCold and EndCold, but moved behind the body by
	// EmitColdCode. The code before a cold range continues after it
	asmjit::BaseNode *BeginCold(const Label &label);
	void  EndCold(asmjit::BaseNode *start, const Label &cont);
	void  EmitColdCode();

	// Materializes a pointer/word constant
	Gp   PtrConst(asPWORD value);

	bool Is64Bit() const { return m_uc.is_64bit(); }

	asmjit::ujit::UniCompiler &m_uc;
	const CJITByteCode        *m_code;      // of the frame being emitted
	SJITCodeGenOptions         m_options;
	asmjit::FuncNode          *m_func;

	Gp  m_regs;     // asSVMRegisters*, less m_regsBias bytes
	int m_regsBias; // see RegsBias
	Gp  m_arg;      // jitArg
	Gp  m_callerSp; // stack pointer of a native caller
	Gp  m_callLimit; // call stack length up to which native calls push, only if the function calls script functions
	Gp  m_callStackLength; // length of the call stack while the function executes, see EmitNativeCall, only with m_callLimit
	Gp  m_inlineRoom; // words of room on the call stack for the inlined functions, only if some are called in loops, see EmitInlineRoom
	Gp  m_fp;       // stack frame pointer
	Gp  m_this;     // object pointer of a method that doesn't modify it, see CJITByteCode::IsThisConstant
	std::vector<Gp>  m_fieldGp;  // the fields of the object held in registers, see CJITByteCode::GetFields
	std::vector<Vec> m_fieldVec; // those kept in vector registers
	Gp  m_sp;       // stack pointer, unless the stack is static
	Gp  m_vr;       // value register (64bit hosts only)
	Gp  m_bailPC;   // program pointer to set when bailing
	bool m_vrInReg;
	bool m_spInArg;     // native callers pass the stack pointer as argument (64bit hosts only)
	bool m_staticStack;
	int  m_spOffset;    // offset of the stack pointer from fp if the stack is static
	asUINT m_thisChecked; // the block of frame 0 where m_this has been checked for null, or -1
	bool m_guarded;
	Mem  m_vrAddr;      // the address left to the next instruction by SetVRAddr
	bool m_vrAddrValid;

	std::vector<SFrame>        m_frames;
	int                        m_frame;        // the frame being emitted
	int                        m_frameBase;    // its base, see SFrame
	std::vector<SCachedSlot>   m_cached;
	std::map<int, asUINT>      m_cachedIndex;
	std::vector<Label>         m_labels;       // per instruction, valid for block starts
	std::vector<Label>         m_entryLabels;  // per entry
	std::vector<SBail>         m_bails;        // bail stubs to emit
	std::vector<std::pair<asmjit::BaseNode*, asmjit::BaseNode*> > m_cold;  // first and last nodes of the cold ranges
	Label                      m_bailCommon;
	Label                      m_bailMaterialized; // the tail of the bail sites in materialized frames
	bool                       m_bailMaterializedUsed;
	Label                      m_leave;        // returns 1, i.e. the VM takes over
	Label                      m_leaveBorrowed; // gives the materialized frames their references first, see LeaveLabel
	bool                       m_leaveBorrowedUsed;
	Label                      m_bailMaterializedBorrowed; // m_bailMaterialized for frames with borrowed parameters
	bool                       m_bailMaterializedBorrowedUsed;

	asUINT m_instrCount;
	asUINT m_bailCount;
	asUINT m_callsInlined;
	asUINT m_callsProfiled;
	bool   m_inlineCalls;  // the last inlined function calls functions, see EmitBody
	bool   m_suspendChecked; // the suspend requests are checked before the instruction, see EmitBody
	bool   m_materialized; // the frames of the inlined calls are on the call stack, see EmitMaterialize
	int    m_materialDepth; // the number of call states pushed for them
	bool   m_materialBorrowed; // some of them have borrowed parameters
	bool   m_shareMaterial; // the next calls share the materialization, see EmitBody
	int    m_inlineExtent; // the largest extent of the inlined calls, see GetInlineRoom
	bool   m_failed;
	int    m_failedOp; // the first instruction that failed, or -1
};

END_AS_NAMESPACE

#endif
