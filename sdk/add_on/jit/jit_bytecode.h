#ifndef JIT_BYTECODE_H
#define JIT_BYTECODE_H

#ifndef ANGELSCRIPT_H
#include <angelscript.h>
#endif

#include "jit.h"

#include <vector>
#include <map>
#include <memory>

BEGIN_AS_NAMESPACE

class asCScriptEngine;
class asCScriptFunction;
class asCObjectType;
class asCTypeInfo;

// Flags for SJITInstr
enum EJITInstrFlags
{
	JIT_INSTR_BLOCK_START = 0x01, // first instruction of a basic block
	JIT_INSTR_ENTRY       = 0x02, // JitEntry instruction that gets an entry stub
	JIT_INSTR_VR_LIVE     = 0x04, // the value register may be read after this instruction before being written
	JIT_INSTR_BAIL        = 0x08, // the instruction must always return control to the VM
	JIT_INSTR_SKIP        = 0x10, // the instruction has no effect and produces no code
	JIT_INSTR_DEAD        = 0x20, // the instruction can never be reached, no code is generated for it
	JIT_INSTR_INLINE      = 0x40, // asBC_CALL or asBC_CALLINTF whose function is emitted in place, see GetInlinees
	JIT_INSTR_BORROW      = 0x80, // asBC_RefCpyV whose reference is lent to a native script call, see AnalyseBorrows
	JIT_INSTR_MOVE        = 0x100, // asBC_RefCpyV that takes over the reference of the variable it copies, see FindMovedRefs
	JIT_INSTR_MOVED       = 0x200, // asBC_FREE of the variable whose reference has been taken over, which only clears it
	JIT_INSTR_REFCOUNT    = 0x400, // asBC_FREE, asBC_REFCPY, or asBC_RefCpyV of script objects whose references are counted in place, see FindInPlaceRefCounts
	JIT_INSTR_FREE_LIST   = 0x800, // asBC_FREE of an initialization list with nothing to destroy, which only frees the memory, see FindListFrees
	JIT_INSTR_PROFILE     = 0x1000, // call whose receiver class or function-pointer target is noted in the profile, see SJITProfile
	JIT_INSTR_INDEXER     = 0x2000  // asBC_CALLSYS or asBC_Thiscall1 of an indexer that is compiled in place, see GetIndexer
};

// How the handles of a type are copied and released, see CJITByteCode::GetRefKind
enum EJITRefKind
{
	JIT_REF_OTHER,         // anything else
	JIT_REF_COUNTED,       // counted references, which the copies may take over, see FindMovedRefs
	JIT_REF_SCRIPT_OBJECT, // counted references of script objects, which may be counted in place, see FindInPlaceRefCounts
	JIT_REF_PLAIN_LIST     // initialization lists with nothing to destroy, see FindListFrees
};

// One decoded bytecode instruction
struct SJITInstr
{
	const asDWORD *bc;     // address of the instruction in the bytecode
	asUINT         pos;    // offset from the start of the bytecode in dwords
	asEBCInstr     op;
	asUINT         size;   // size of the instruction in dwords
	int            target; // index of the target instruction for branches, or -1
	int            block;  // index of the basic block the instruction belongs to
	asUINT         flags;  // EJITInstrFlags
};

// A basic block, i.e. a straight sequence of instructions
struct SJITBlock
{
	asUINT first;     // index of the first instruction
	asUINT last;      // index of the last instruction
	bool   vrLiveIn;  // the value register is read before written in the block or its successors
	bool   vrLiveOut; // the value register is live at the end of the block
	bool   vrUse;     // the value register is read before written in this block
	bool   vrDef;     // the value register is written in this block
};

// Kind of values stored in a stack slot, as observed from the instructions that use it
enum EJITSlotKind
{
	JIT_SLOT_NONE  = 0x00,
	JIT_SLOT_I32   = 0x01, // 32bit integer or boolean
	JIT_SLOT_I64   = 0x02, // 64bit integer
	JIT_SLOT_F32   = 0x04, // float
	JIT_SLOT_F64   = 0x08, // double
	JIT_SLOT_PTR   = 0x10, // pointer, handle, or object
	JIT_SLOT_ADDR  = 0x20, // the address of the slot is taken
	JIT_SLOT_ANY32 = 0x40, // untyped 32bit copy
	JIT_SLOT_ANY64 = 0x80  // untyped 64bit copy
};

struct SJITSlot
{
	int    offset;    // variable offset as used in the bytecode (fp - offset)
	asUINT kinds;     // bit mask of EJITSlotKind
	asUINT useCount;
	asUINT floatUses; // of the uses, those by the float and double operations
	asUINT intUses;   // and those by the integer operations
	int    cacheKind; // JIT_SLOT_I32, I64, F32, F64, or PTR if the slot can be kept in a register, else JIT_SLOT_NONE
	int    cacheBit;  // bit in the dirty masks for cached slots, else -1
};

// A field of the object of a method, which asBC_LoadThisR and asBC_RDR4, asBC_WRTV4,
// or the 32-bit increments read and write. Its value is kept in a register after
// the accesses, so that the next reads don't have to load it, see
// CJITByteCode::AnalyseThisFields
struct SJITField
{
	int    offset;    // from the object pointer, in bytes
	int    kind;      // JIT_SLOT_I32 or JIT_SLOT_F32, the register it is kept in
	asUINT floatUses; // of the accesses, those of variables kept in vector registers
	asUINT forwarded; // the reads that take the value from the register
	bool   kept;      // the field is kept in a register
};

// The bit in the field masks for the object pointer, which its register holds after
// asBC_LoadThisR until a call. It is loaded again from the variable then, so that
// the register isn't saved across the calls. The other 31 bits are for the fields
static const asUINT JIT_THIS_HELD = 0x80000000u;

// One bit per register cached variable. The last bit is reserved for the frame,
// leaving room for up to 63 cached variables.
typedef asQWORD JITSlotMask;

// The frame of the function, i.e. the stack frame pointer in the VM registers and
// the current function of the context, is written back like the register cached
// variables, and has this bit in the dirty masks. The native entry leaves it to
// the first place where the VM or the engine may see it
static const JITSlotMask JIT_FRAME_BIT = JITSlotMask(1) << 63;

// On 64bit hosts the script functions called natively return without restoring
// the frame of the caller, see asBC_RET, so it is dirty after the calls. They mark
// the call states that native callers push, see JITFunction
#if AS_PTR_SIZE == 2
#define JIT_NATIVE_RETURN
#endif

// On x86 and AArch64 the generated code counts the references of the script objects
// itself, see FindInPlaceRefCounts. The atomic operations of the engine are
// compatible with the locked and the atomic instructions used there
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__X86__) || defined(__i386__) || \
    defined(_M_ARM64) || defined(__aarch64__)
#define JIT_INPLACE_REFCOUNT
#endif

// The classes that a call has seen, in the order seen, null after the last. The calls
// that see more classes than there is room for don't note them
static const asUINT JIT_PROFILE_CLASSES = 3;
struct SJITSeenClasses
{
	asCObjectType *types[JIT_PROFILE_CLASSES];

	bool Has(const asCObjectType *type) const
	{
		for( asUINT n = 0; n < JIT_PROFILE_CLASSES; n++ )
			if( types[n] == type )
				return true;
		return false;
	}
};

// The script functions that a function-pointer call has seen, in the order seen,
// null after the last. The calls that see more functions than there is room for
// don't note them
static const asUINT JIT_PROFILE_FUNCTIONS = 3;
struct SJITSeenFunctions
{
	asCScriptFunction *functions[JIT_PROFILE_FUNCTIONS];

	bool Has(const asCScriptFunction *func) const
	{
		for( asUINT n = 0; n < JIT_PROFILE_FUNCTIONS; n++ )
			if( functions[n] == func )
				return true;
		return false;
	}
};

// The receiver classes of virtual and interface calls, and the targets of
// function-pointer calls, marked with JIT_INSTR_PROFILE. They are kept by the
// function and the index of the call. The compiled code counts down the calls, and
// compiles the function again with the targets seen when the count runs out, if a
// call has seen a new one, see SJITCodeGenOptions. Several threads may write the
// cells at once, which only loses counts or targets. The pointers are only compared
// with live classes or functions of the module of the call before they are used.
// The count runs out when it isn't positive, so a count that a thread takes below 0
// while another starts it again runs out at the next call
struct SJITProfile
{
	std::map<std::pair<asCScriptFunction*, asUINT>, SJITSeenClasses> classes;
	std::map<std::pair<asCScriptFunction*, asUINT>, SJITSeenFunctions> functions;
	int countdown;

	// Returns the classes noted for the call, or null
	const SJITSeenClasses *Find(asCScriptFunction *func, asUINT instrIdx) const;
	// Returns the functions noted for the call, or null
	const SJITSeenFunctions *FindFunctions(asCScriptFunction *func, asUINT instrIdx) const;
	// Returns true if a call has seen a class or function of the module of its
	// function that it hadn't in the profile that the code was compiled with
	bool HasNewTarget(const SJITProfile &compiledWith) const;
	// Returns true if a call has seen a new receiver class in its function's module
	bool HasNewClass(const SJITProfile &compiledWith) const;
};

// The functions that Analyse lets the code generator emit in place of their calls
struct SJITInlineOptions
{
	asUINT maxSize;  // largest bytecode in dwords, 0 inlines nothing
	bool (*filter)(asIScriptFunction *func, void *param); // must accept the function unless null
	void  *filterParam;
	const SJITProfile *classes; // the call targets seen by the code compiled before, or null
	bool   profile;  // mark the calls whose targets are worth noting with JIT_INSTR_PROFILE
	bool   borrowCalls; // borrow handle arguments of non-inlined calls with checked targets
	const std::map<asFUNCTION_t, SJITIndexer> *indexers; // by the native function, see CJITCompiler::AddIndexer, or null
};

class CJITByteCode;

// A function emitted in place of a call, see CJITByteCode::GetInlinees, and the
// classes that the object of an asBC_CALLINTF must be of for it. Empty for asBC_CALL,
// and for the virtual methods that several classes inherit, for which the class of
// the object must have the inlined method in its table
struct SJITInlinee
{
	std::shared_ptr<CJITByteCode> code;
	std::vector<asCObjectType*>   types;
};

// A call of an indexer that is compiled in place, see CJITCompiler::AddIndexer
struct SJITIndexerCall
{
	SJITIndexer layout;
	int  elementSize; // bytes from one element to the next
	bool indirect;    // the elements are pointers to the objects
};

// Decodes the bytecode of a script function and gathers the information
// needed by the code generator: instructions, basic blocks, branch targets,
// switch tables, JIT entry points, variable usage, and value register liveness
class CJITByteCode
{
public:
	CJITByteCode();

	// Decodes the bytecode. Returns a negative value if the bytecode is malformed
	int  Decode(asCScriptFunction *func);

	// Performs the analysis. Must be called after Decode. The small functions called
	// with asBC_CALL are analysed too, so that their code can be emitted in place, and
	// so are the methods called with asBC_CALLINTF if all the classes that can
	// implement them have the same implementation, or if the profile has seen one
	// class at the call, see FindInlinees
	void Analyse(bool allowRegisterCache, asUINT maxCachedSlots, const SJITInlineOptions *inlining = 0);

	// Performs the analysis for the code generated ahead of time, which must depend
	// only on the key of the function, see JIT_GetAOTKey. Nothing is inlined. Script
	// calls are analysed only for the handle arguments that their direct entries can
	// borrow. The calls that the code may make directly leave the frame dirty, see
	// CJITCppGen. The indexers are part of the key too
	void AnalyseForAOT(asUINT maxCachedSlots, const std::map<asFUNCTION_t, SJITIndexer> *indexers);

	// The function that generated AOT code expects a script call to reach. Virtual
	// and interface calls return null when no single implementation can be expected
	static asCScriptFunction *FindAOTCallee(asCScriptFunction *func, bool virtualCall);

	asCScriptFunction             *GetFunction() const     { return m_func; }
	const asDWORD                 *GetByteCode() const     { return m_byteCode; }
	asUINT                         GetLength() const       { return m_length; }
	const std::vector<SJITInstr>  &GetInstructions() const { return m_instrs; }
	const std::vector<SJITBlock>  &GetBlocks() const       { return m_blocks; }
	const std::vector<asUINT>     &GetEntries() const      { return m_entries; }
	const std::vector<SJITSlot>   &GetSlots() const        { return m_slots; }

	// Returns the instruction index for a bytecode position, or -1
	int  FindInstruction(asUINT pos) const;

	// Returns the register cache kind for a variable, or JIT_SLOT_NONE
	int  GetCacheKind(int offset) const;

	// Returns the bit of a register cached variable in the dirty masks, or -1
	int  GetCacheBit(int offset) const;

	// Returns true if the function is a method that reads its object pointer with
	// asBC_LoadThisR and never modifies the variable, so that it can be held in a
	// register
	bool IsThisConstant() const { return m_thisConstant; }

	// Returns the fields of the object that are kept in registers, the one read or
	// written by the instruction, or -1, and the mask of the fields whose register
	// holds their value when the instruction is reached, with JIT_THIS_HELD. Where
	// the VM enters, the entry stubs load the fields, see AnalyseThisFields
	const std::vector<SJITField> &GetFields() const { return m_fields; }
	int    GetFieldAccess(asUINT instrIdx) const { return m_fieldAccess.empty() ? -1 : m_fieldAccess[instrIdx]; }
	asUINT GetFieldMask(asUINT instrIdx) const   { return m_fieldMask.empty() ? 0 : m_fieldMask[instrIdx]; }

	// Returns the mask of the register cached variables whose register may hold
	// a newer value than the memory when the instruction is reached, and of the
	// frame, see JIT_FRAME_BIT. Temporary variables that won't be read anymore are
	// left out
	JITSlotMask GetDirtyMask(asUINT instrIdx) const { return m_dirty[instrIdx]; }

	// Returns the mask of the register cached variables to store before or after
	// the instruction, because a loop is entered in which they aren't modified
	JITSlotMask GetStoresBefore(asUINT instrIdx) const { return m_storeBefore[instrIdx]; }
	JITSlotMask GetStoresAfter(asUINT instrIdx) const  { return m_storeAfter[instrIdx]; }

	// Returns the mask of the register cached variables that may be read before
	// being written when the block of the instruction is entered
	JITSlotMask GetLiveInMask(asUINT instrIdx) const { return m_liveIn[m_instrs[instrIdx].block]; }

	// Returns the mask of the register cached variables that may be read before
	// being written after the instruction
	JITSlotMask GetLiveAfterMask(asUINT instrIdx) const { return m_liveAfter[instrIdx]; }

	// Returns the mask of the register cached variables to load where the VM enters
	// at the instruction, or after the instruction if the VM may have modified the
	// variables, e.g. through a debugger. Those that may be stored later are loaded
	// too, see the implementation
	JITSlotMask GetEntryMask(asUINT instrIdx) const;
	JITSlotMask GetReloadMask(asUINT instrIdx) const;

	// Returns true if RET passes the return value in the value register
	bool   RetReadsVR() const { return m_retReadsVR; }
	static bool ReturnsInVR(asCScriptFunction *func);

	// Returns true if the depth of the stack is known at each instruction, i.e. the
	// stack pointer is at a fixed distance from the frame pointer
	bool   HasStaticStack() const { return m_staticStack; }

	// Returns the number of dwords on the stack above the variables when the
	// instruction is reached. Only valid if HasStaticStack
	int    GetStackDepth(asUINT instrIdx) const { return m_stackDepth[instrIdx]; }

	// Returns the number of dwords that a script function pops off the stack when it
	// returns: the arguments, the object pointer, and the pointer to the location of
	// a value returned on the stack. Registered functions pop the same
	static int GetPopSize(asCScriptFunction *func);

	// Returns how the handles of the type in the operand of asBC_FREE, asBC_REFCPY,
	// or asBC_RefCpyV are copied and released. The code generated ahead of time
	// depends on it, as the types aren't part of the key
	static EJITRefKind GetRefKind(asCScriptEngine *engine, asCTypeInfo *type);

	// Returns true if the value register may be read before being written after
	// the instruction, or when the instruction is reached, respectively
	bool   IsVRLiveAfter(asUINT instrIdx) const { return (m_instrs[instrIdx].flags & JIT_INSTR_VR_LIVE) != 0; }
	bool   IsVRLiveBefore(asUINT instrIdx) const;

	// Instructions after which all cached variables have been written to memory
	static bool IsSyncPoint(asEBCInstr op);
	// How an instruction accesses the variables in its operands
	static void GetVarAccess(asEBCInstr op, bool &reads0, bool &writes0, bool &reads1, bool &reads2);

	// Returns the instruction indices targeted by a JMPP instruction, in case order
	const std::vector<int> &GetSwitchTargets(asUINT instrIdx) const;

	// Returns the functions emitted in place of an instruction marked with
	// JIT_INSTR_INLINE: one for asBC_CALL, and for asBC_CALLINTF one for each
	// implementation of the method among the classes that the object is checked for,
	// which are the only class of the module that can call it, or those that the
	// profile has seen
	const std::vector<SJITInlinee> &GetInlinees(asUINT instrIdx) const;

	// True if one of the functions emitted in place of the instruction has sync points
	// with the arguments borrowed from the caller, see HasSyncPoints
	bool InlineesHaveSyncPoints(asUINT instrIdx) const;

	// Returns the indexer called by an instruction marked with JIT_INSTR_INDEXER
	const SJITIndexerCall *GetIndexer(asUINT instrIdx) const;

	// Returns true if the registered function is one of the indexers, which are
	// compiled in place, and how
	static bool FindIndexer(asCScriptEngine *engine, int funcId, const std::map<asFUNCTION_t, SJITIndexer> *indexers, SJITIndexerCall &call);

	// Returns true if the function or one inlined into it calls something that may
	// see the VM registers, or releases objects, where the frames of the calls that
	// inline it are handed to the VM, see CJITCodeGen::EmitMaterialize. The parameters
	// in the mask borrow the references of the caller, which aren't released
	bool   HasSyncPoints(asUINT borrowed = 0) const { return m_hasSyncPoints || (m_releasedParams & ~borrowed) != 0; }

	// Returns true if the instruction stores the cached variables, see IsSyncPoint.
	// The copies of the references lent to the inlined calls don't, and neither do
	// the copies of the script objects counted in place, which only store them on
	// the rare paths, and the indexers compiled in place, which leave those to the VM
	bool   IsSyncPointAt(asUINT instrIdx) const { return IsSyncPoint(m_instrs[instrIdx].op) && !(m_instrs[instrIdx].flags & (JIT_INSTR_BORROW | JIT_INSTR_REFCOUNT | JIT_INSTR_INDEXER)); }

	// Returns the index of the parameter in the variable, or -1
	int    FindParam(int offset) const;

	// Returns the mask of the handle parameters that the function only reads and
	// releases, so that native script calls can lend it their references
	asUINT GetBorrowableParams() const { return m_borrowableParams; }

	// Returns the mask of the parameters of the function called by the instruction
	// that borrow the references of the caller, see AnalyseBorrows and AnalyseForAOT
	asUINT GetBorrowedArgs(asUINT instrIdx) const;

	// Returns the implementations a non-inlined dynamic call may lend its arguments
	// to. The generated code checks the resolved function against every one
	const std::vector<asCScriptFunction*> &GetBorrowedTargets(asUINT instrIdx) const;

	// Returns the dynamic targets whose addresses are embedded in the generated
	// code. The compiler keeps them alive until all code of this function is gone
	const std::vector<asCScriptFunction*> &GetBorrowedDependencies() const { return m_borrowedDependencies; }

	// Returns the variables that must be null for the copies of the references lent
	// to a native script call not to release anything, which the first of the copies
	// checks for all of them
	const std::vector<int> &GetBorrowChecks(asUINT instrIdx) const;

	// Marks instructions that must return to the VM. Must be called before Analyse,
	// which marks them in the inlined functions too
	void SetBailInstructions(const bool bail[asBC_MAXBYTECODE]);

	// Classification of the instructions
	static bool IsBranch(asEBCInstr op);       // conditional or unconditional jump (not JMPP)
	static bool IsTerminator(asEBCInstr op);   // ends a basic block
	static bool ReadsVR(asEBCInstr op);
	static bool WritesVR(asEBCInstr op);

protected:
	void MarkUnreachable(bool inlined);
	void BuildBlocks();
	void AnalyseVRLiveness();
	void AnalyseSlots(bool allowRegisterCache, asUINT maxCachedSlots);
	void AnalyseDirtySlots();
	void AnalyseSlotLiveness();
	void AnalyseThisFields();
	void AnalyseStackDepth();
	void AnalyseBody(bool allowRegisterCache, asUINT maxCachedSlots);
	struct SInlineSearch;
	void FindInlinees(SInlineSearch &search, asUINT levels, asUINT budget);
	std::shared_ptr<CJITByteCode> AnalyseInlinee(SInlineSearch &search, asCScriptFunction *func, asUINT levels);
	bool CanBeInlined() const;
	void FindIndexers(const std::map<asFUNCTION_t, SJITIndexer> *indexers);
	void AnalyseBorrows(bool borrowCalls = false);
	void AnalyseBorrows(bool borrowCalls, const SJITProfile *profile, bool profileCalls);
	void ClearBorrows();
	void FindBorrowableParams();
	void FindBorrowedArgs(const std::map<asUINT, std::vector<SJITInlinee> > &callees);
	void FindCalledBorrowedArgs(const SJITProfile *profile, bool profileCalls);
	void FindAOTBorrowedArgs();
	void FindMovedRefs();
	void FindInPlaceRefCounts();
	void FindListFrees();
	int  FindVarConsumer(asUINT idx) const;
	int  FindPush(asUINT idx, int top) const;
	asCObjectType *FindReceiverType(asUINT call) const;
	bool HoldsReference(int var) const;
	bool LeavesFrameDirty(asUINT instrIdx) const;
	bool GetStackInc(const SJITInstr &instr, int &inc) const;
	void GetSuccessors(asUINT blockIdx, std::vector<asUINT> &succ) const;
	void GetSlotMasks(const SJITInstr &instr, JITSlotMask &uses, JITSlotMask &defs) const;
	void AddSlotUse(int offset, asUINT kind);
	void CollectSlotUses(const SJITInstr &instr);
	SJITSlot *FindSlot(int offset);

	asCScriptFunction      *m_func;
	const asDWORD          *m_byteCode;
	asUINT                  m_length;
	std::vector<SJITInstr>  m_instrs;
	std::vector<int>        m_posToInstr;
	std::vector<SJITBlock>  m_blocks;
	std::vector<asUINT>     m_entries;
	std::vector<SJITSlot>   m_slots;
	std::map<int, int>      m_slotIndex;   // offset -> index in m_slots
	std::map<asUINT, std::vector<int> > m_switchTargets;
	std::vector<int>        m_noTargets;
	std::vector<JITSlotMask> m_dirty;       // per instruction mask of possibly dirty cached slots
	std::vector<JITSlotMask> m_storeBefore; // per instruction mask of cached slots stored before it
	std::vector<JITSlotMask> m_storeAfter;  // per instruction mask of cached slots stored after it
	std::vector<JITSlotMask> m_liveIn;      // per block mask of cached slots live at the start
	std::vector<JITSlotMask> m_liveAfter;   // per instruction mask of cached slots live after it
	JITSlotMask              m_tempMask;    // mask of the cached slots that are temporary variables
	bool                    m_thisConstant; // see IsThisConstant
	std::vector<SJITField>  m_fields;      // see GetFields
	std::vector<int>        m_fieldAccess; // per instruction the field read or written, or -1
	std::vector<asUINT>     m_fieldMask;   // per instruction mask of the fields held in registers, and JIT_THIS_HELD
	std::vector<int>        m_stackDepth;  // per instruction dwords on the stack above the variables, or -1
	std::map<asUINT, std::vector<SJITInlinee> > m_inlinees; // by instruction, see GetInlinees
	asUINT                  m_inlinedLength; // dwords of the functions inlined at the calls and into them
	std::map<asUINT, SJITIndexerCall> m_indexers; // by instruction, see GetIndexer
	asUINT                  m_borrowableParams; // see GetBorrowableParams
	asUINT                  m_releasedParams;   // the borrowable parameters that the function releases
	std::map<asUINT, asUINT> m_borrowedArgs;    // by call instruction
	std::map<asUINT, std::vector<asCScriptFunction*> > m_borrowedTargets; // checked targets of dynamic calls
	std::vector<asCScriptFunction*> m_borrowedDependencies; // dynamic targets embedded in generated code
	std::map<asUINT, std::vector<int> > m_borrowChecks; // by instruction, see GetBorrowChecks
	std::vector<int>        m_noChecks;
	const bool             *m_bail;           // see SetBailInstructions
	bool                    m_staticStack;
	bool                    m_aot;            // see AnalyseForAOT
	bool                    m_retReadsVR;
	bool                    m_hasSyncPoints;
};

END_AS_NAMESPACE

#endif
