#ifndef JIT_BYTECODE_H
#define JIT_BYTECODE_H

#ifndef ANGELSCRIPT_H
#include <angelscript.h>
#endif

#include <vector>
#include <map>

BEGIN_AS_NAMESPACE

class asCScriptFunction;

// Flags for SJITInstr
enum EJITInstrFlags
{
	JIT_INSTR_BLOCK_START = 0x01, // first instruction of a basic block
	JIT_INSTR_ENTRY       = 0x02, // JitEntry instruction that gets an entry stub
	JIT_INSTR_VR_LIVE     = 0x04, // the value register may be read after this instruction before being written
	JIT_INSTR_BAIL        = 0x08, // the instruction must always return control to the VM
	JIT_INSTR_SKIP        = 0x10, // the instruction has no effect and produces no code
	JIT_INSTR_DEAD        = 0x20  // the instruction can never be reached, no code is generated for it
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

// Decodes the bytecode of a script function and gathers the information
// needed by the code generator: instructions, basic blocks, branch targets,
// switch tables, JIT entry points, variable usage, and value register liveness
class CJITByteCode
{
public:
	CJITByteCode();

	// Decodes the bytecode. Returns a negative value if the bytecode is malformed
	int  Decode(asCScriptFunction *func);

	// Performs the analysis. Must be called after Decode
	void Analyse(bool allowRegisterCache, asUINT maxCachedSlots);

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
	// a newer value than the memory when the instruction is reached
	asUINT GetDirtyMask(asUINT instrIdx) const { return m_dirty[instrIdx]; }

	// Returns the mask of the register cached variables that may be read before
	// being written when the block of the instruction is entered
	asUINT GetLiveInMask(asUINT instrIdx) const { return m_liveIn[m_instrs[instrIdx].block]; }

	// Instructions after which all cached variables have been written to memory
	static bool IsSyncPoint(asEBCInstr op);
	// How an instruction accesses the variables in its operands
	static void GetVarAccess(asEBCInstr op, bool &reads0, bool &writes0, bool &reads1, bool &reads2);

	// Returns the instruction indices targeted by a JMPP instruction, in case order
	const std::vector<int> &GetSwitchTargets(asUINT instrIdx) const;

	// Marks instructions that must return to the VM
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
	std::vector<asUINT>     m_liveIn;      // per block mask of cached slots live at the start
};

END_AS_NAMESPACE

#endif
