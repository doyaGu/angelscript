#ifndef JIT_BYTECODE_H
#define JIT_BYTECODE_H

#ifndef ANGELSCRIPT_H
#include <angelscript.h>
#endif

#include <vector>
#include <map>
#include <memory>

BEGIN_AS_NAMESPACE

class asCScriptFunction;
class asCObjectType;

// Flags for SJITInstr
enum EJITInstrFlags
{
	JIT_INSTR_BLOCK_START = 0x01, // first instruction of a basic block
	JIT_INSTR_ENTRY       = 0x02, // JitEntry instruction that gets an entry stub
	JIT_INSTR_VR_LIVE     = 0x04, // the value register may be read after this instruction before being written
	JIT_INSTR_BAIL        = 0x08, // the instruction must always return control to the VM
	JIT_INSTR_SKIP        = 0x10, // the instruction has no effect and produces no code
	JIT_INSTR_DEAD        = 0x20, // the instruction can never be reached, no code is generated for it
	JIT_INSTR_INLINE      = 0x40  // asBC_CALL or asBC_CALLINTF whose function is emitted in place, see GetInlinee
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
	int    cacheKind; // JIT_SLOT_I32, I64, F32, F64 if the slot can be kept in a register, else JIT_SLOT_NONE
	int    cacheBit;  // bit in the dirty masks for cached slots, else -1
};

// The frame of the function, i.e. the stack frame pointer in the VM registers and
// the current function of the context, is written back like the register cached
// variables, and has this bit in the dirty masks. The native entry leaves it to
// the first place where the VM or the engine may see it
static const asUINT JIT_FRAME_BIT = 0x80000000u;

// On 64bit hosts the script functions called natively return without restoring
// the frame of the caller, see asBC_RET, so it is dirty after the calls. They mark
// the call states that native callers push, see JITFunction
#if AS_PTR_SIZE == 2
#define JIT_NATIVE_RETURN
#endif

// The functions that Analyse lets the code generator emit in place of their calls
struct SJITInlineOptions
{
	asUINT maxSize;  // largest bytecode in dwords, 0 inlines nothing
	bool (*filter)(asIScriptFunction *func, void *param); // must accept the function unless null
	void  *filterParam;
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
	// with asBC_CALL are analysed too, so that their code can be emitted in place if
	// they call nothing but the functions emitted in theirs, and so are such methods
	// called with asBC_CALLINTF if only one class can implement them, see FindInlinees
	void Analyse(bool allowRegisterCache, asUINT maxCachedSlots, const SJITInlineOptions *inlining = 0);

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

	// Returns the mask of the register cached variables whose register may hold
	// a newer value than the memory when the instruction is reached, and of the
	// frame, see JIT_FRAME_BIT. Temporary variables that won't be read anymore are
	// left out
	asUINT GetDirtyMask(asUINT instrIdx) const { return m_dirty[instrIdx]; }

	// Returns the mask of the register cached variables to store before or after
	// the instruction, because a loop is entered in which they aren't modified
	asUINT GetStoresBefore(asUINT instrIdx) const { return m_storeBefore[instrIdx]; }
	asUINT GetStoresAfter(asUINT instrIdx) const  { return m_storeAfter[instrIdx]; }

	// Returns the mask of the register cached variables that may be read before
	// being written when the block of the instruction is entered
	asUINT GetLiveInMask(asUINT instrIdx) const { return m_liveIn[m_instrs[instrIdx].block]; }

	// Returns the mask of the register cached variables that may be read before
	// being written after the instruction
	asUINT GetLiveAfterMask(asUINT instrIdx) const { return m_liveAfter[instrIdx]; }

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

	// Returns the analysis of the function called by an instruction marked with
	// JIT_INSTR_INLINE
	const CJITByteCode *GetInlinee(asUINT instrIdx) const;

	// Returns the class that the object must be of for an inlined asBC_CALLINTF to
	// call the inlined method, or null for asBC_CALL
	asCObjectType *GetInlineObjectType(asUINT instrIdx) const;

	// Marks instructions that must return to the VM, also in the inlined functions
	void SetBailInstructions(const bool bail[asBC_MAXBYTECODE]);

	// Classification of the instructions
	static bool IsBranch(asEBCInstr op);       // conditional or unconditional jump (not JMPP)
	static bool IsTerminator(asEBCInstr op);   // ends a basic block
	static bool ReadsVR(asEBCInstr op);
	static bool WritesVR(asEBCInstr op);

protected:
	void MarkUnreachable();
	void BuildBlocks();
	void AnalyseVRLiveness();
	void AnalyseSlots(bool allowRegisterCache, asUINT maxCachedSlots);
	void AnalyseDirtySlots();
	void AnalyseSlotLiveness();
	void AnalyseStackDepth();
	void AnalyseBody(bool allowRegisterCache, asUINT maxCachedSlots);
	struct SInlineSearch;
	void FindInlinees(SInlineSearch &search, asUINT levels, asUINT budget);
	bool CanBeInlined(bool inlineesFound) const;
	bool GetStackInc(const SJITInstr &instr, int &inc) const;
	void GetSuccessors(asUINT blockIdx, std::vector<asUINT> &succ) const;
	void GetSlotMasks(const SJITInstr &instr, asUINT &uses, asUINT &defs) const;
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
	std::vector<asUINT>     m_dirty;       // per instruction mask of possibly dirty cached slots
	std::vector<asUINT>     m_storeBefore; // per instruction mask of cached slots stored before it
	std::vector<asUINT>     m_storeAfter;  // per instruction mask of cached slots stored after it
	std::vector<asUINT>     m_liveIn;      // per block mask of cached slots live at the start
	std::vector<asUINT>     m_liveAfter;   // per instruction mask of cached slots live after it
	asUINT                  m_tempMask;    // mask of the cached slots that are temporary variables
	std::vector<int>        m_stackDepth;  // per instruction dwords on the stack above the variables, or -1
	std::map<asUINT, std::shared_ptr<CJITByteCode> > m_inlinees; // by instruction, shared by the calls of a function
	std::map<asUINT, asCObjectType*> m_inlineObjTypes; // by instruction, for the inlined asBC_CALLINTF
	asUINT                  m_inlinedLength; // dwords of the functions inlined at the calls and into them
	bool                    m_staticStack;
	bool                    m_retReadsVR;
};

END_AS_NAMESPACE

#endif
