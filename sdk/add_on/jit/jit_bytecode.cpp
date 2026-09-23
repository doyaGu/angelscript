#include "jit_bytecode.h"

// Internal engine headers. The JIT must be compiled with the same
// configuration as the engine library (see CMakeLists.txt)
#include "as_scriptfunction.h"

#include <algorithm>

BEGIN_AS_NAMESPACE

CJITByteCode::CJITByteCode()
{
	m_func     = 0;
	m_byteCode = 0;
	m_length   = 0;
	m_retReadsVR = false;
}

bool CJITByteCode::IsBranch(asEBCInstr op)
{
	switch( op )
	{
	case asBC_JMP:
	case asBC_JZ:
	case asBC_JNZ:
	case asBC_JS:
	case asBC_JNS:
	case asBC_JP:
	case asBC_JNP:
	case asBC_JLowZ:
	case asBC_JLowNZ:
		return true;
	default:
		return false;
	}
}

bool CJITByteCode::IsTerminator(asEBCInstr op)
{
	return IsBranch(op) || op == asBC_JMPP || op == asBC_RET;
}

bool CJITByteCode::ReadsVR(asEBCInstr op)
{
	switch( op )
	{
	case asBC_JZ:
	case asBC_JNZ:
	case asBC_JS:
	case asBC_JNS:
	case asBC_JP:
	case asBC_JNP:
	case asBC_JLowZ:
	case asBC_JLowNZ:
	case asBC_TZ:
	case asBC_TNZ:
	case asBC_TS:
	case asBC_TNS:
	case asBC_TP:
	case asBC_TNP:
	case asBC_CpyRtoV4:
	case asBC_CpyRtoV8:
	case asBC_PshRPtr:
	case asBC_INCi16:
	case asBC_INCi8:
	case asBC_DECi16:
	case asBC_DECi8:
	case asBC_INCi:
	case asBC_DECi:
	case asBC_INCf:
	case asBC_DECf:
	case asBC_INCd:
	case asBC_DECd:
	case asBC_INCi64:
	case asBC_DECi64:
	case asBC_WRTV1:
	case asBC_WRTV2:
	case asBC_WRTV4:
	case asBC_WRTV8:
	case asBC_RDR1:
	case asBC_RDR2:
	case asBC_RDR4:
	case asBC_RDR8:
	case asBC_ClrHi:
	case asBC_RET:     // primitive and reference return values, see AnalyseVRLiveness
		return true;
	default:
		return false;
	}
}

bool CJITByteCode::WritesVR(asEBCInstr op)
{
	switch( op )
	{
	case asBC_TZ:
	case asBC_TNZ:
	case asBC_TS:
	case asBC_TNS:
	case asBC_TP:
	case asBC_TNP:
	case asBC_CMPd:
	case asBC_CMPu:
	case asBC_CMPf:
	case asBC_CMPi:
	case asBC_CMPIi:
	case asBC_CMPIf:
	case asBC_CMPIu:
	case asBC_CMPi64:
	case asBC_CMPu64:
	case asBC_CmpPtr:
	case asBC_CpyVtoR4:
	case asBC_CpyVtoR8:
	case asBC_LdGRdR4:
	case asBC_PopRPtr:
	case asBC_LDG:
	case asBC_LDV:
	case asBC_LoadThisR:
	case asBC_LoadRObjR:
	case asBC_LoadVObjR:
	case asBC_ClrHi:
	case asBC_CALL:      // the called function may return a value in the register
	case asBC_CALLSYS:
	case asBC_CALLBND:
	case asBC_CALLINTF:
	case asBC_CallPtr:
	case asBC_ALLOC:
	case asBC_Thiscall1:
		return true;
	default:
		return false;
	}
}

int CJITByteCode::Decode(asCScriptFunction *func)
{
	m_func = func;
	m_instrs.clear();
	m_posToInstr.clear();
	m_blocks.clear();
	m_entries.clear();
	m_slots.clear();
	m_slotIndex.clear();
	m_switchTargets.clear();

	m_byteCode = func->GetByteCode(&m_length);
	if( m_byteCode == 0 || m_length == 0 )
		return asERROR;

	m_posToInstr.assign(m_length + 1, -1);

	// First pass: split the bytecode into instructions
	asUINT pos = 0;
	while( pos < m_length )
	{
		asEBCInstr op = asEBCInstr(*(asBYTE*)(m_byteCode + pos));
		if( op >= asBC_MAXBYTECODE )
			return asERROR;

		asUINT size = asBCTypeSize[asBCInfo[op].type];
		if( size == 0 || pos + size > m_length )
			return asERROR;

		SJITInstr instr;
		instr.bc     = m_byteCode + pos;
		instr.pos    = pos;
		instr.op     = op;
		instr.size   = size;
		instr.target = -1;
		instr.block  = -1;
		instr.flags  = 0;

		m_posToInstr[pos] = int(m_instrs.size());
		m_instrs.push_back(instr);
		pos += size;
	}

	// Second pass: resolve the branch targets, switch tables, and entry points
	bool leading = true;
	int entryLine = func->GetLineNumber(0, 0) & 0xFFFFF;
	std::vector<bool> isTarget(m_instrs.size(), false);
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		SJITInstr &instr = m_instrs[n];

		// The compiler removes the SUSPEND for the first line of a function as the
		// line callback is already invoked when the function is entered. With JIT
		// instructions enabled a JitEntry in between stops it from doing so when
		// the line has more than one statement, which would invoke the callback
		// twice for the same line. Such leading SUSPENDs are skipped here
		if( instr.op == asBC_SUSPEND && leading && (func->GetLineNumber(int(instr.pos), 0) & 0xFFFFF) == entryLine )
			instr.flags |= JIT_INSTR_SKIP;
		else if( instr.op != asBC_JitEntry )
			leading = false;
		if( IsBranch(instr.op) )
		{
			int targetPos = int(instr.pos) + 2 + asBC_INTARG(instr.bc);
			if( targetPos < 0 || targetPos > int(m_length) )
				return asERROR;
			instr.target = m_posToInstr[targetPos];
			if( instr.target < 0 )
				return asERROR;
			m_instrs[instr.target].flags |= JIT_INSTR_BLOCK_START;
			isTarget[instr.target] = true;
		}
		else if( instr.op == asBC_JMPP )
		{
			// The JMPP instruction is followed by a table of JMP instructions, one
			// for each case. The number of cases isn't stored, so the table is
			// assumed to span all consecutive JMP instructions that follow. Over
			// counting is harmless as the compiler guarantees the index is in range
			std::vector<int> &targets = m_switchTargets[n];
			asUINT m = n + 1;
			while( m < m_instrs.size() && m_instrs[m].op == asBC_JMP )
			{
				int targetPos = int(m_instrs[m].pos) + 2 + asBC_INTARG(m_instrs[m].bc);
				if( targetPos < 0 || targetPos > int(m_length) || m_posToInstr[targetPos] < 0 )
					return asERROR;
				targets.push_back(m_posToInstr[targetPos]);
				m_instrs[m_posToInstr[targetPos]].flags |= JIT_INSTR_BLOCK_START;
				isTarget[m_posToInstr[targetPos]] = true;
				m++;
			}
			if( targets.empty() )
				return asERROR;
		}
		else if( instr.op == asBC_JitEntry )
		{
			instr.flags |= JIT_INSTR_ENTRY | JIT_INSTR_BLOCK_START;
			m_entries.push_back(n);
		}

		if( n == 0 || IsTerminator(m_instrs[n-1].op) )
			instr.flags |= JIT_INSTR_BLOCK_START;
	}

	// A loop on the first line may start at one of the leading SUSPENDs, e.g.
	// 'do {} while( cond );', in which case the SUSPENDs from the loop head on
	// are its only suspend points and must be kept
	bool inLoop = false;
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		SJITInstr &instr = m_instrs[n];
		if( instr.op != asBC_JitEntry && !(instr.flags & JIT_INSTR_SKIP) )
			break;
		if( isTarget[n] )
			inLoop = true;
		if( inLoop )
			instr.flags &= ~JIT_INSTR_SKIP;
	}

	// Without entry points the function can never be entered natively, e.g.
	// when the bytecode was compiled without JIT instructions
	if( m_entries.empty() )
		return asNOT_SUPPORTED;

	MarkUnreachable();

	return asSUCCESS;
}

// Marks the instructions that can never be executed, e.g. the jump tables of
// switch statements that are only reached through JMPP, so that no code is
// generated for them
void CJITByteCode::MarkUnreachable()
{
	// Native callers enter at the start of the function, see CJITCodeGen::EmitDirectEntry
	std::vector<bool> reached(m_instrs.size(), false);
	std::vector<asUINT> work(m_entries.begin(), m_entries.end());
	work.push_back(0);

	while( !work.empty() )
	{
		asUINT n = work.back();
		work.pop_back();
		if( reached[n] )
			continue;
		reached[n] = true;

		const SJITInstr &instr = m_instrs[n];
		if( instr.op == asBC_JMPP )
		{
			const std::vector<int> &targets = GetSwitchTargets(n);
			for( asUINT t = 0; t < targets.size(); t++ )
				work.push_back(targets[t]);
		}
		else if( instr.op != asBC_RET )
		{
			if( instr.target >= 0 )
				work.push_back(instr.target);
			if( instr.op != asBC_JMP && n + 1 < m_instrs.size() )
				work.push_back(n + 1);
		}
	}

	for( asUINT n = 0; n < m_instrs.size(); n++ )
		if( !reached[n] )
			m_instrs[n].flags |= JIT_INSTR_DEAD;
}

int CJITByteCode::FindInstruction(asUINT pos) const
{
	if( pos >= m_posToInstr.size() )
		return -1;
	return m_posToInstr[pos];
}

const std::vector<int> &CJITByteCode::GetSwitchTargets(asUINT instrIdx) const
{
	std::map<asUINT, std::vector<int> >::const_iterator it = m_switchTargets.find(instrIdx);
	if( it == m_switchTargets.end() )
		return m_noTargets;
	return it->second;
}

void CJITByteCode::SetBailInstructions(const bool bail[asBC_MAXBYTECODE])
{
	for( asUINT n = 0; n < m_instrs.size(); n++ )
		if( bail[m_instrs[n].op] )
			m_instrs[n].flags |= JIT_INSTR_BAIL;
}

void CJITByteCode::Analyse(bool allowRegisterCache, asUINT maxCachedSlots)
{
	BuildBlocks();
	AnalyseVRLiveness();
	AnalyseSlots(allowRegisterCache, maxCachedSlots);
	AnalyseDirtySlots();
	AnalyseSlotLiveness();
}

bool CJITByteCode::IsSyncPoint(asEBCInstr op)
{
	switch( op )
	{
	case asBC_CALL:
	case asBC_CALLSYS:
	case asBC_CALLBND:
	case asBC_CALLINTF:
	case asBC_CallPtr:
	case asBC_Thiscall1:
	case asBC_ALLOC:
	case asBC_REFCPY:
	case asBC_RefCpyV:
	case asBC_POWi:
	case asBC_POWu:
	case asBC_POWf:
	case asBC_POWd:
	case asBC_POWdi:
	case asBC_POWi64:
	case asBC_POWu64:
		return true;
	default:
		return false;
	}
}

// Only the instructions that can have register cached variables as operands
// matter here, i.e. those working on primitives. Pointers and objects are never
// cached, so their instructions don't have to be described
void CJITByteCode::GetVarAccess(asEBCInstr op, bool &reads0, bool &writes0, bool &reads1, bool &reads2)
{
	reads0 = writes0 = reads1 = reads2 = false;
	switch( op )
	{
	// Read only first operand
	case asBC_PshV4:
	case asBC_PshV8:
	case asBC_CpyVtoR4:
	case asBC_CpyVtoR8:
	case asBC_CpyVtoG4:
	case asBC_WRTV1:
	case asBC_WRTV2:
	case asBC_WRTV4:
	case asBC_WRTV8:
	case asBC_CMPIi:
	case asBC_CMPIf:
	case asBC_CMPIu:
	case asBC_JMPP:
		reads0 = true;
		break;

	// Compares read both
	case asBC_CMPi:
	case asBC_CMPu:
	case asBC_CMPf:
	case asBC_CMPd:
	case asBC_CMPi64:
	case asBC_CMPu64:
		reads0 = reads1 = true;
		break;

	// Modified in place
	case asBC_NOT:
	case asBC_NEGi:
	case asBC_NEGf:
	case asBC_NEGd:
	case asBC_IncVi:
	case asBC_DecVi:
	case asBC_BNOT:
	case asBC_BNOT64:
	case asBC_NEGi64:
	case asBC_iTOf:
	case asBC_fTOi:
	case asBC_uTOf:
	case asBC_fTOu:
	case asBC_sbTOi:
	case asBC_swTOi:
	case asBC_ubTOi:
	case asBC_uwTOi:
	case asBC_iTOb:
	case asBC_iTOw:
	case asBC_dTOi64:
	case asBC_dTOu64:
	case asBC_i64TOd:
	case asBC_u64TOd:
		reads0 = writes0 = true;
		break;

	// Written from the second operand
	case asBC_CpyVtoV4:
	case asBC_CpyVtoV8:
	case asBC_ADDIi:
	case asBC_SUBIi:
	case asBC_MULIi:
	case asBC_ADDIf:
	case asBC_SUBIf:
	case asBC_MULIf:
	case asBC_dTOi:
	case asBC_dTOu:
	case asBC_dTOf:
	case asBC_iTOd:
	case asBC_uTOd:
	case asBC_fTOd:
	case asBC_i64TOi:
	case asBC_uTOi64:
	case asBC_iTOi64:
	case asBC_fTOi64:
	case asBC_fTOu64:
	case asBC_i64TOf:
	case asBC_u64TOf:
		writes0 = reads1 = true;
		break;

	// Written from the second and third operands
	case asBC_BAND:
	case asBC_BOR:
	case asBC_BXOR:
	case asBC_BSLL:
	case asBC_BSRL:
	case asBC_BSRA:
	case asBC_ADDi:
	case asBC_SUBi:
	case asBC_MULi:
	case asBC_DIVi:
	case asBC_MODi:
	case asBC_DIVu:
	case asBC_MODu:
	case asBC_ADDf:
	case asBC_SUBf:
	case asBC_MULf:
	case asBC_DIVf:
	case asBC_MODf:
	case asBC_ADDd:
	case asBC_SUBd:
	case asBC_MULd:
	case asBC_DIVd:
	case asBC_MODd:
	case asBC_POWi:
	case asBC_POWu:
	case asBC_POWf:
	case asBC_POWd:
	case asBC_POWdi:
	case asBC_ADDi64:
	case asBC_SUBi64:
	case asBC_MULi64:
	case asBC_DIVi64:
	case asBC_MODi64:
	case asBC_DIVu64:
	case asBC_MODu64:
	case asBC_BAND64:
	case asBC_BOR64:
	case asBC_BXOR64:
	case asBC_BSLL64:
	case asBC_BSRL64:
	case asBC_BSRA64:
	case asBC_POWi64:
	case asBC_POWu64:
		writes0 = reads1 = reads2 = true;
		break;

	// Written only
	case asBC_SetV1:
	case asBC_SetV2:
	case asBC_SetV4:
	case asBC_SetV8:
	case asBC_CpyRtoV4:
	case asBC_CpyRtoV8:
	case asBC_CpyGtoV4:
	case asBC_LdGRdR4:
	case asBC_RDR1:
	case asBC_RDR2:
	case asBC_RDR4:
	case asBC_RDR8:
		writes0 = true;
		break;

	default:
		break;
	}
}

// Masks of the cached slots read and written by the instruction
void CJITByteCode::GetSlotMasks(const SJITInstr &instr, asUINT &uses, asUINT &defs) const
{
	uses = defs = 0;
	bool reads0, writes0, reads1, reads2;
	GetVarAccess(instr.op, reads0, writes0, reads1, reads2);

	int bit;
	if( (reads0 || writes0) && (bit = GetCacheBit(asBC_SWORDARG0(instr.bc))) >= 0 )
	{
		if( reads0 )  uses |= asUINT(1) << bit;
		if( writes0 ) defs |= asUINT(1) << bit;
	}
	if( reads1 && (bit = GetCacheBit(asBC_SWORDARG1(instr.bc))) >= 0 )
		uses |= asUINT(1) << bit;
	if( reads2 && (bit = GetCacheBit(asBC_SWORDARG2(instr.bc))) >= 0 )
		uses |= asUINT(1) << bit;
}

// Backward data flow to find which cached slots may be read before being
// written in each block. Entry stubs only need to load those
void CJITByteCode::AnalyseSlotLiveness()
{
	m_liveIn.assign(m_blocks.size(), 0);

	std::vector<asUINT> use(m_blocks.size(), 0), def(m_blocks.size(), 0);
	for( asUINT b = 0; b < m_blocks.size(); b++ )
	{
		for( asUINT n = m_blocks[b].first; n <= m_blocks[b].last; n++ )
		{
			asUINT uses, defs;
			GetSlotMasks(m_instrs[n], uses, defs);
			use[b] |= uses & ~def[b];
			def[b] |= defs;
		}
	}

	bool changed = true;
	while( changed )
	{
		changed = false;
		for( int b = int(m_blocks.size()) - 1; b >= 0; b-- )
		{
			const SJITBlock &block = m_blocks[b];
			const SJITInstr &last = m_instrs[block.last];

			asUINT liveOut = 0;
			if( last.op == asBC_JMPP )
			{
				const std::vector<int> &targets = GetSwitchTargets(block.last);
				for( asUINT t = 0; t < targets.size(); t++ )
					liveOut |= m_liveIn[m_instrs[targets[t]].block];
			}
			else if( last.op != asBC_RET )
			{
				if( last.target >= 0 )
					liveOut |= m_liveIn[m_instrs[last.target].block];
				if( last.op != asBC_JMP && block.last + 1 < m_instrs.size() )
					liveOut |= m_liveIn[m_instrs[block.last + 1].block];
			}

			asUINT liveIn = use[b] | (liveOut & ~def[b]);
			if( liveIn != m_liveIn[b] )
			{
				m_liveIn[b] = liveIn;
				changed = true;
			}
		}
	}
}

// Forward data flow over the blocks to find out which register cached
// variables may hold a newer value than the memory at each instruction. Only
// those need to be stored when the VM must see the variables
void CJITByteCode::AnalyseDirtySlots()
{
	m_dirty.assign(m_instrs.size(), 0);

	bool anyCached = false;
	for( asUINT n = 0; n < m_slots.size(); n++ )
		if( m_slots[n].cacheBit >= 0 )
			anyCached = true;
	if( !anyCached )
		return;

	std::vector<asUINT> in(m_blocks.size(), 0);
	bool changed = true;
	while( changed )
	{
		changed = false;
		for( asUINT b = 0; b < m_blocks.size(); b++ )
		{
			SJITBlock &block = m_blocks[b];
			asUINT mask = in[b];
			for( asUINT n = block.first; n <= block.last; n++ )
			{
				const SJITInstr &instr = m_instrs[n];
				m_dirty[n] = mask;

				if( IsSyncPoint(instr.op) )
					mask = 0;
				else
				{
					asUINT uses, defs;
					GetSlotMasks(instr, uses, defs);
					mask |= defs;
				}
			}

			// Propagate to the successors
			const SJITInstr &last = m_instrs[block.last];
			int succ[2] = { -1, -1 };
			if( last.op == asBC_JMPP )
			{
				const std::vector<int> &targets = GetSwitchTargets(block.last);
				for( asUINT t = 0; t < targets.size(); t++ )
				{
					asUINT s = m_instrs[targets[t]].block;
					if( (in[s] | mask) != in[s] ) { in[s] |= mask; changed = true; }
				}
			}
			else if( last.op != asBC_RET )
			{
				if( last.target >= 0 )
					succ[0] = m_instrs[last.target].block;
				if( last.op != asBC_JMP && block.last + 1 < m_instrs.size() )
					succ[1] = m_instrs[block.last + 1].block;
			}
			for( int k = 0; k < 2; k++ )
			{
				if( succ[k] < 0 ) continue;
				if( (in[succ[k]] | mask) != in[succ[k]] )
				{
					in[succ[k]] |= mask;
					changed = true;
				}
			}
		}
	}
}

int CJITByteCode::GetCacheBit(int offset) const
{
	std::map<int, int>::const_iterator it = m_slotIndex.find(offset);
	if( it == m_slotIndex.end() )
		return -1;
	return m_slots[it->second].cacheBit;
}

void CJITByteCode::BuildBlocks()
{
	m_blocks.clear();
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		if( m_instrs[n].flags & JIT_INSTR_BLOCK_START )
		{
			SJITBlock block;
			block.first = n;
			block.last  = n;
			block.vrLiveIn = block.vrLiveOut = block.vrUse = block.vrDef = false;
			m_blocks.push_back(block);
		}
		m_blocks.back().last = n;
		m_instrs[n].block = int(m_blocks.size()) - 1;
	}
}

void CJITByteCode::AnalyseVRLiveness()
{
	// RET only passes the value register to the caller for primitives and references.
	// Handles are returned in the object register and objects in memory
	const asCDataType &rt = m_func->returnType;
	bool retReadsVR = rt.IsReference() || (rt.GetTokenType() != ttVoid && !rt.IsObject() && !rt.IsObjectHandle() && !rt.IsFuncdef());
	m_retReadsVR = retReadsVR;

	// Local use/def per block
	for( asUINT b = 0; b < m_blocks.size(); b++ )
	{
		SJITBlock &block = m_blocks[b];
		for( asUINT n = block.first; n <= block.last; n++ )
		{
			asEBCInstr op = m_instrs[n].op;
			if( ReadsVR(op) && (op != asBC_RET || retReadsVR) && !block.vrDef )
				block.vrUse = true;
			if( WritesVR(op) )
				block.vrDef = true;
		}
	}

	// Backward data flow until nothing changes
	bool changed = true;
	while( changed )
	{
		changed = false;
		for( int b = int(m_blocks.size()) - 1; b >= 0; b-- )
		{
			SJITBlock &block = m_blocks[b];
			const SJITInstr &last = m_instrs[block.last];

			bool liveOut = false;
			if( last.op == asBC_JMPP )
			{
				const std::vector<int> &targets = GetSwitchTargets(block.last);
				for( asUINT t = 0; t < targets.size(); t++ )
					liveOut = liveOut || m_blocks[m_instrs[targets[t]].block].vrLiveIn;
			}
			else if( last.op == asBC_RET )
			{
				liveOut = false;
			}
			else
			{
				if( last.target >= 0 )
					liveOut = m_blocks[m_instrs[last.target].block].vrLiveIn;
				if( last.op != asBC_JMP && block.last + 1 < m_instrs.size() )
					liveOut = liveOut || m_blocks[m_instrs[block.last + 1].block].vrLiveIn;
			}

			bool liveIn = block.vrUse || (!block.vrDef && liveOut);
			if( liveOut != block.vrLiveOut || liveIn != block.vrLiveIn )
			{
				block.vrLiveOut = liveOut;
				block.vrLiveIn  = liveIn;
				changed = true;
			}
		}
	}

	// Per instruction liveness
	for( asUINT b = 0; b < m_blocks.size(); b++ )
	{
		SJITBlock &block = m_blocks[b];
		bool live = block.vrLiveOut;
		for( int n = int(block.last); n >= int(block.first); n-- )
		{
			if( live )
				m_instrs[n].flags |= JIT_INSTR_VR_LIVE;
			else
				m_instrs[n].flags &= ~JIT_INSTR_VR_LIVE;

			asEBCInstr op = m_instrs[n].op;
			if( WritesVR(op) )
				live = false;
			if( ReadsVR(op) && (op != asBC_RET || retReadsVR) )
				live = true;
		}
	}
}

SJITSlot *CJITByteCode::FindSlot(int offset)
{
	std::map<int, int>::iterator it = m_slotIndex.find(offset);
	if( it == m_slotIndex.end() )
		return 0;
	return &m_slots[it->second];
}

void CJITByteCode::AddSlotUse(int offset, asUINT kind)
{
	SJITSlot *slot = FindSlot(offset);
	if( slot == 0 )
	{
		SJITSlot s;
		s.offset    = offset;
		s.kinds     = 0;
		s.useCount  = 0;
		s.cacheKind = JIT_SLOT_NONE;
		s.cacheBit  = -1;
		m_slotIndex[offset] = int(m_slots.size());
		m_slots.push_back(s);
		slot = &m_slots.back();
	}
	slot->kinds |= kind;
	slot->useCount++;
}

void CJITByteCode::CollectSlotUses(const SJITInstr &instr)
{
	const asDWORD *bc = instr.bc;
	int a0 = asBC_SWORDARG0(bc);
	int a1 = asBC_SWORDARG1(bc);
	int a2 = asBC_SWORDARG2(bc);

	switch( instr.op )
	{
	// 32bit integer and boolean operations
	case asBC_NOT:
	case asBC_IncVi:
	case asBC_DecVi:
	case asBC_BNOT:
	case asBC_NEGi:
	case asBC_iTOb:
	case asBC_iTOw:
	case asBC_sbTOi:
	case asBC_swTOi:
	case asBC_ubTOi:
	case asBC_uwTOi:
	case asBC_JMPP:
	case asBC_CMPIi:
	case asBC_CMPIu:
	case asBC_WRTV1:
	case asBC_WRTV2:
		AddSlotUse(a0, JIT_SLOT_I32);
		break;
	case asBC_BAND:
	case asBC_BOR:
	case asBC_BXOR:
	case asBC_BSLL:
	case asBC_BSRL:
	case asBC_BSRA:
	case asBC_ADDi:
	case asBC_SUBi:
	case asBC_MULi:
	case asBC_DIVi:
	case asBC_MODi:
	case asBC_DIVu:
	case asBC_MODu:
	case asBC_POWi:
	case asBC_POWu:
		AddSlotUse(a0, JIT_SLOT_I32);
		AddSlotUse(a1, JIT_SLOT_I32);
		AddSlotUse(a2, JIT_SLOT_I32);
		break;
	case asBC_ADDIi:
	case asBC_SUBIi:
	case asBC_MULIi:
	case asBC_CMPi:
	case asBC_CMPu:
		AddSlotUse(a0, JIT_SLOT_I32);
		AddSlotUse(a1, JIT_SLOT_I32);
		break;

	// 64bit integer operations
	case asBC_NEGi64:
	case asBC_BNOT64:
		AddSlotUse(a0, JIT_SLOT_I64);
		break;
	case asBC_ADDi64:
	case asBC_SUBi64:
	case asBC_MULi64:
	case asBC_DIVi64:
	case asBC_MODi64:
	case asBC_DIVu64:
	case asBC_MODu64:
	case asBC_BAND64:
	case asBC_BOR64:
	case asBC_BXOR64:
	case asBC_POWi64:
	case asBC_POWu64:
		AddSlotUse(a0, JIT_SLOT_I64);
		AddSlotUse(a1, JIT_SLOT_I64);
		AddSlotUse(a2, JIT_SLOT_I64);
		break;
	case asBC_BSLL64:
	case asBC_BSRL64:
	case asBC_BSRA64:
		AddSlotUse(a0, JIT_SLOT_I64);
		AddSlotUse(a1, JIT_SLOT_I64);
		AddSlotUse(a2, JIT_SLOT_I32);
		break;
	case asBC_CMPi64:
	case asBC_CMPu64:
		AddSlotUse(a0, JIT_SLOT_I64);
		AddSlotUse(a1, JIT_SLOT_I64);
		break;

	// float operations
	case asBC_NEGf:
	case asBC_CMPIf:
		AddSlotUse(a0, JIT_SLOT_F32);
		break;
	case asBC_ADDf:
	case asBC_SUBf:
	case asBC_MULf:
	case asBC_DIVf:
	case asBC_MODf:
	case asBC_POWf:
		AddSlotUse(a0, JIT_SLOT_F32);
		AddSlotUse(a1, JIT_SLOT_F32);
		AddSlotUse(a2, JIT_SLOT_F32);
		break;
	case asBC_ADDIf:
	case asBC_SUBIf:
	case asBC_MULIf:
	case asBC_CMPf:
		AddSlotUse(a0, JIT_SLOT_F32);
		AddSlotUse(a1, JIT_SLOT_F32);
		break;

	// double operations
	case asBC_NEGd:
		AddSlotUse(a0, JIT_SLOT_F64);
		break;
	case asBC_ADDd:
	case asBC_SUBd:
	case asBC_MULd:
	case asBC_DIVd:
	case asBC_MODd:
	case asBC_POWd:
		AddSlotUse(a0, JIT_SLOT_F64);
		AddSlotUse(a1, JIT_SLOT_F64);
		AddSlotUse(a2, JIT_SLOT_F64);
		break;
	case asBC_POWdi:
		AddSlotUse(a0, JIT_SLOT_F64);
		AddSlotUse(a1, JIT_SLOT_F64);
		AddSlotUse(a2, JIT_SLOT_I32);
		break;
	case asBC_CMPd:
		AddSlotUse(a0, JIT_SLOT_F64);
		AddSlotUse(a1, JIT_SLOT_F64);
		break;

	// conversions
	case asBC_iTOf:
	case asBC_fTOi:
	case asBC_uTOf:
	case asBC_fTOu:
		AddSlotUse(a0, JIT_SLOT_I32 | JIT_SLOT_F32);
		break;
	case asBC_dTOi:
	case asBC_dTOu:
		AddSlotUse(a0, JIT_SLOT_I32);
		AddSlotUse(a1, JIT_SLOT_F64);
		break;
	case asBC_dTOf:
		AddSlotUse(a0, JIT_SLOT_F32);
		AddSlotUse(a1, JIT_SLOT_F64);
		break;
	case asBC_iTOd:
	case asBC_uTOd:
		AddSlotUse(a0, JIT_SLOT_F64);
		AddSlotUse(a1, JIT_SLOT_I32);
		break;
	case asBC_fTOd:
		AddSlotUse(a0, JIT_SLOT_F64);
		AddSlotUse(a1, JIT_SLOT_F32);
		break;
	case asBC_i64TOi:
		AddSlotUse(a0, JIT_SLOT_I32);
		AddSlotUse(a1, JIT_SLOT_I64);
		break;
	case asBC_uTOi64:
	case asBC_iTOi64:
		AddSlotUse(a0, JIT_SLOT_I64);
		AddSlotUse(a1, JIT_SLOT_I32);
		break;
	case asBC_fTOi64:
	case asBC_fTOu64:
		AddSlotUse(a0, JIT_SLOT_I64);
		AddSlotUse(a1, JIT_SLOT_F32);
		break;
	case asBC_dTOi64:
	case asBC_dTOu64:
	case asBC_i64TOd:
	case asBC_u64TOd:
		AddSlotUse(a0, JIT_SLOT_I64 | JIT_SLOT_F64);
		break;
	case asBC_i64TOf:
	case asBC_u64TOf:
		AddSlotUse(a0, JIT_SLOT_F32);
		AddSlotUse(a1, JIT_SLOT_I64);
		break;

	// untyped 32bit accesses
	case asBC_PshV4:
	case asBC_SetV1:
	case asBC_SetV2:
	case asBC_SetV4:
	case asBC_CpyVtoR4:
	case asBC_CpyRtoV4:
	case asBC_CpyVtoG4:
	case asBC_CpyGtoV4:
	case asBC_LdGRdR4:
	case asBC_WRTV4:
	case asBC_RDR1:
	case asBC_RDR2:
	case asBC_RDR4:
		AddSlotUse(a0, JIT_SLOT_ANY32);
		break;
	case asBC_CpyVtoV4:
		AddSlotUse(a0, JIT_SLOT_ANY32);
		AddSlotUse(a1, JIT_SLOT_ANY32);
		break;

	// untyped 64bit accesses
	case asBC_PshV8:
	case asBC_SetV8:
	case asBC_CpyVtoR8:
	case asBC_CpyRtoV8:
	case asBC_WRTV8:
	case asBC_RDR8:
		AddSlotUse(a0, JIT_SLOT_ANY64);
		break;
	case asBC_CpyVtoV8:
		AddSlotUse(a0, JIT_SLOT_ANY64);
		AddSlotUse(a1, JIT_SLOT_ANY64);
		break;

	// pointers, handles, and objects
	case asBC_PshVPtr:
	case asBC_ChkNullV:
	case asBC_LoadRObjR:
	case asBC_ClrVPtr:
	case asBC_FREE:
	case asBC_LOADOBJ:
	case asBC_STOREOBJ:
	case asBC_RefCpyV:
	case asBC_AllocMem:
	case asBC_SetListSize:
	case asBC_PshListElmnt:
	case asBC_SetListType:
		AddSlotUse(a0, JIT_SLOT_PTR);
		break;
	case asBC_CallPtr:
		AddSlotUse(a1, JIT_SLOT_PTR);
		break;
	case asBC_CmpPtr:
		AddSlotUse(a0, JIT_SLOT_PTR);
		AddSlotUse(a1, JIT_SLOT_PTR);
		break;

	// the address of the variable is taken
	case asBC_PSF:
	case asBC_VAR:
	case asBC_LDV:
	case asBC_LoadVObjR:
		AddSlotUse(a0, JIT_SLOT_ADDR);
		break;

	default:
		// Instruction doesn't reference variables directly
		break;
	}
}

// TODO: runtime optimize: Only primitive variables are kept in registers. Handles and
//                         object pointers that are never address-taken could be cached
//                         the same way, which would help code that indexes arrays or
//                         calls methods on the same handle in a loop. The dirty and
//                         live masks are 32bit, so at most 32 variables can be cached;
//                         larger functions would need a wider mask or a second pass
//                         choosing the variables per loop rather than per function.
void CJITByteCode::AnalyseSlots(bool allowRegisterCache, asUINT maxCachedSlots)
{
	m_slots.clear();
	m_slotIndex.clear();

	for( asUINT n = 0; n < m_instrs.size(); n++ )
		CollectSlotUses(m_instrs[n]);

	if( !allowRegisterCache )
		return;

	// Determine which slots hold a single kind of primitive value and are never
	// accessed through their address. Only those can be kept in registers
	for( asUINT n = 0; n < m_slots.size(); n++ )
	{
		SJITSlot &slot = m_slots[n];
		asUINT kinds = slot.kinds;
		asUINT typed = kinds & (JIT_SLOT_I32 | JIT_SLOT_I64 | JIT_SLOT_F32 | JIT_SLOT_F64);

		slot.cacheKind = JIT_SLOT_NONE;
		if( kinds & (JIT_SLOT_PTR | JIT_SLOT_ADDR) )
			continue;

		// More than one typed kind
		if( typed & (typed - 1) )
			continue;

		if( typed == 0 )
		{
			if( kinds == JIT_SLOT_ANY32 )
				typed = JIT_SLOT_I32;
			else if( kinds == JIT_SLOT_ANY64 )
				typed = JIT_SLOT_I64;
			else
				continue;
		}

		bool is64 = (typed == JIT_SLOT_I64 || typed == JIT_SLOT_F64);
		if( is64 && (kinds & JIT_SLOT_ANY32) )
			continue;
		if( !is64 && (kinds & JIT_SLOT_ANY64) )
			continue;

		// 64bit integers cannot be held in a single register on 32bit hosts
		if( typed == JIT_SLOT_I64 && sizeof(void*) < 8 )
			continue;

		slot.cacheKind = int(typed);
	}

	// 64bit slots also cover the dword below them. Make sure no other slot
	// overlaps, which would be the case for malformed or unexpected bytecode
	for( asUINT n = 0; n < m_slots.size(); n++ )
	{
		SJITSlot &slot = m_slots[n];
		bool is64 = (slot.kinds & (JIT_SLOT_I64 | JIT_SLOT_F64 | JIT_SLOT_ANY64)) != 0;
		if( is64 )
		{
			SJITSlot *other = FindSlot(slot.offset - 1);
			if( other )
			{
				slot.cacheKind  = JIT_SLOT_NONE;
				other->cacheKind = JIT_SLOT_NONE;
			}
		}
	}

	// Limit the number of cached slots to keep the register pressure reasonable.
	// The most used slots are kept
	asUINT count = 0;
	for( asUINT n = 0; n < m_slots.size(); n++ )
		if( m_slots[n].cacheKind != JIT_SLOT_NONE )
			count++;

	if( count > maxCachedSlots )
	{
		std::vector<SJITSlot*> sorted;
		for( asUINT n = 0; n < m_slots.size(); n++ )
			if( m_slots[n].cacheKind != JIT_SLOT_NONE )
				sorted.push_back(&m_slots[n]);
		std::stable_sort(sorted.begin(), sorted.end(), [](const SJITSlot *a, const SJITSlot *b) { return a->useCount > b->useCount; });
		for( asUINT n = maxCachedSlots; n < sorted.size(); n++ )
			sorted[n]->cacheKind = JIT_SLOT_NONE;
	}

	// Give each cached slot a bit for the dirty masks
	int bit = 0;
	for( asUINT n = 0; n < m_slots.size(); n++ )
		if( m_slots[n].cacheKind != JIT_SLOT_NONE )
			m_slots[n].cacheBit = bit++;
}

int CJITByteCode::GetCacheKind(int offset) const
{
	std::map<int, int>::const_iterator it = m_slotIndex.find(offset);
	if( it == m_slotIndex.end() )
		return JIT_SLOT_NONE;
	return m_slots[it->second].cacheKind;
}

END_AS_NAMESPACE
