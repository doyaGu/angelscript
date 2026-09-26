#include "jit_codegen.h"
#include "jit_runtime.h"

// For the size of the variables, which the stack starts below
#include "as_scriptfunction.h"

#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

BEGIN_AS_NAMESPACE

using namespace asmjit;
using namespace asmjit::ujit;

static const int PTR_BYTES = AS_PTR_SIZE * 4;

CJITCodeGen::CJITCodeGen(UniCompiler &uc, const CJITByteCode &code, const SJITCodeGenOptions &options) :
	m_uc(uc), m_code(&code), m_options(options)
{
	SFrame frame;
	frame.code     = &code;
	frame.base     = 0;
	frame.caller   = -1;
	frame.callIdx  = 0;
	frame.exitUsed = false;
	frame.borrowed = 0;
	m_frames.push_back(frame);
	m_frame      = 0;
	m_frameBase  = 0;
	m_func       = 0;
	m_guarded    = false;
	m_vrInReg    = uc.is_64bit();
	m_spInArg    = uc.is_64bit();
	m_staticStack = code.HasStaticStack();
	m_spOffset   = 0;
	m_vrAddrValid = false;
	m_instrCount = 0;
	m_bailCount  = 0;
	m_callsInlined = 0;
	m_inlineCalls  = false;
	m_materialized = false;
	m_materialDepth = 0;
	m_shareMaterial = false;
	m_bailMaterializedUsed = false;
	m_materialBorrowed = false;
	m_leaveBorrowedUsed = false;
	m_bailMaterializedBorrowedUsed = false;
	m_inlineExtent = 0;
	m_failed     = false;
}

//------------------------------------------------------------------------
// Driver

bool CJITCodeGen::Generate()
{
	const std::vector<SJITInstr> &instrs = m_code->GetInstructions();
	const std::vector<asUINT> &entries = m_code->GetEntries();

	EmitPrologue();

	// Labels for the basic blocks and entry points
	CreateBlockLabels();
	m_entryLabels.resize(entries.size());
	for( asUINT n = 0; n < entries.size(); n++ )
		if( EntryNeedsStub(n) )
			m_entryLabels[n] = m_uc.new_label();

	m_bailCommon = m_uc.new_label();
	m_bailMaterialized = m_uc.new_label();
	m_bailMaterializedBorrowed = m_uc.new_label();
	m_leave = m_uc.new_label();
	m_leaveBorrowed = m_uc.new_label();

	// The instructions in loops, which are the ones up to a backward branch from its target
	std::vector<int> loopDepth(instrs.size() + 1);
	for( asUINT n = 0; n < instrs.size(); n++ )
		if( instrs[n].target >= 0 && asUINT(instrs[n].target) <= n )
		{
			loopDepth[instrs[n].target]++;
			loopDepth[n + 1]--;
		}

	// The room for the inlined functions is checked on entry if they are called in
	// loops, see EmitInlineRoom
	bool inlinedInLoop = false;
	int inLoop = 0;
	for( asUINT n = 0; n < instrs.size(); n++ )
	{
		inLoop += loopDepth[n];
		if( !(instrs[n].flags & JIT_INSTR_INLINE) )
			continue;
		inlinedInLoop = inlinedInLoop || inLoop > 0;
		int extent, depth;
		GetInlineRoom(n, extent, depth);
		if( extent > m_inlineExtent )
			m_inlineExtent = extent;
	}
	if( inlinedInLoop )
		m_inlineRoom = m_uc.new_gp32("inlineRoom");

	Label direct = m_uc.new_label();
	m_uc.j(direct, test_z(m_arg));

	if( m_guarded )
	{
		// Enter again through the helper that catches the C++ exceptions of the direct calls
		Label guarded = m_uc.new_label();
		m_uc.j(guarded, test_nz(m_arg, Imm(JIT_GUARDED_ENTRY)));
		InvokeNode *call = Invoke((const void*)JIT_GuardedEntry, FuncSignature::build<int, asSVMRegisters*, asPWORD>());
		Gp r = m_uc.new_gp32();
		call->set_arg(0, m_regs);
		call->set_arg(1, m_arg);
		call->set_ret(0, r);
		m_uc.ret(r);
		m_uc.bind(guarded);
		m_uc.and_(m_arg, m_arg, Imm(~JIT_GUARDED_ENTRY));
	}

	// Entered by the VM, which has set up the frame. Jump to the requested entry point
	const SJITContextLayout &layout = JIT_GetContextLayout();
	m_uc.load(m_fp, RegsField(offsetof(asSVMRegisters, stackFramePointer)));
	ReloadStack();
	if( m_callLimit.is_valid() )
	{
		// The native calls may push call states as long as the call stack doesn't
		// have to grow, and up to the maximum depth. The capacity is a multiple of
		// the size of a call state and doesn't shrink, see JITFunction
		asQWORD words = asQWORD(m_options.maxNativeCallDepth) * layout.callStackFrameSize;
		Gp capacity = m_uc.new_gp32();
		m_uc.load_u32(m_callLimit, ContextField(layout.callStackLength));
		m_uc.load_u32(capacity, ContextField(layout.callStackCapacity));
		m_uc.add(m_callLimit, m_callLimit, Imm(int(words < 0x40000000 ? words : 0x40000000)));
		m_uc.umin(m_callLimit, m_callLimit, capacity);
	}
	if( m_inlineRoom.is_valid() )
		EmitInlineRoom();
#ifdef JIT_NATIVE_RETURN
	{
		// The function returns to the VM now, also if it was called natively and
		// has left the rest of the call to the VM. The mark is in the upper half of
		// the stack index, where the call states pushed by the VM have zeros, also
		// the ones for nested calls, which have the size of the arguments there
		Label unmarked = m_uc.new_label();
		Gp length = m_uc.new_gp_ptr();
		Gp array  = m_uc.new_gp_ptr();
		m_uc.load_u32(length, ContextField(layout.callStackLength));
		m_uc.j(unmarked, sub_c(length, Imm(layout.callStackFrameSize)));
		m_uc.load(array, ContextField(layout.callStackArray));
		Mem mark = PtrElement(array, length);
		mark.add_offset(4 * PTR_BYTES + 4);
		m_uc.store_zero_u32(mark);
		m_uc.bind(unmarked);
	}
#endif
	EmitEntryDispatch(0, asUINT(entries.size()) - 1);

	// The body. Notes the instructions that call functions
	std::vector<bool> calls(instrs.size());
	EmitBody(calls);

	if( m_failed )
		return false;

	// Callee-saved registers must be saved and restored on each entry, which only
	// pays off for the registers used all through the function if calls are made
	// repeatedly, i.e. in loops. Short functions that are called often, like
	// recursive ones, get slower otherwise. The cached variables read after the calls
	// in loops would be saved and reloaded around each call too
	bool callsInLoop = false;
	asUINT liveAcrossCalls = 0;
	int depth = 0;
	for( asUINT n = 0; n < instrs.size(); n++ )
	{
		depth += loopDepth[n];
		if( depth > 0 && calls[n] )
		{
			callsInLoop = true;
			liveAcrossCalls |= m_code->GetLiveAfterMask(n);
		}
	}

	// Valid bytecode always ends with a RET, but make sure nothing falls into the stubs
	Leave();
	EmitColdCode();

	m_uc.bind(direct);
	EmitDirectEntry();

	EmitEntryStubs();
	EmitBailStubs();

	if( m_leaveBorrowedUsed )
	{
		m_uc.bind(m_leaveBorrowed);
		InvokeNode *call = Invoke((const void*)JIT_OwnBorrowed, FuncSignature::build<void, asSVMRegisters*, asDWORD*, asCScriptFunction*>());
		call->set_arg(0, m_regs);
		call->set_arg(1, m_fp);
		call->set_arg(2, Imm(int64_t(asPWORD(m_code->GetFunction()))));
	}
	m_uc.bind(m_leave);
	Gp one = m_uc.new_gp32();
	m_uc.mov(one, Imm(1));
	m_uc.ret(one);

	m_uc.end_func();
	if( callsInLoop )
		AssignHomeRegs(liveAcrossCalls);
	return true;
}

// The calls of registered functions and the releases of objects return with the
// frame they are made in in the VM registers, unlike the script calls, see
// CJITByteCode::LeavesFrameDirty
static bool SharesMaterialization(asEBCInstr op)
{
	switch( op )
	{
	case asBC_CALLSYS:
	case asBC_Thiscall1:
	case asBC_REFCPY:
	case asBC_RefCpyV:
	case asBC_FREE:
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

void CJITCodeGen::EmitBody(std::vector<bool> &calls)
{
	// The heads of the innermost loops, i.e. the targets of backward branches with no
	// other loop head up to the branch, start a cache line, so that the loops take up
	// as few as possible
	const std::vector<SJITInstr> &instrs = m_code->GetInstructions();
	std::vector<bool> alignHead(instrs.size());
	{
		std::vector<bool> isHead(instrs.size());
		for( asUINT n = 0; n < instrs.size(); n++ )
			if( instrs[n].target >= 0 && asUINT(instrs[n].target) <= n )
				isHead[instrs[n].target] = true;
		std::vector<asUINT> headsBefore(instrs.size() + 1);
		for( asUINT n = 0; n < instrs.size(); n++ )
			headsBefore[n + 1] = headsBefore[n] + (isHead[n] ? 1 : 0);
		for( asUINT n = 0; n < instrs.size(); n++ )
		{
			int t = instrs[n].target;
			if( t >= 0 && asUINT(t) <= n && headsBefore[n + 1] == headsBefore[t + 1] )
				alignHead[t] = true;
		}
	}

	// The calls and releases that follow each other in an inlined function share one
	// materialization of its frame, see EmitMaterialize, unless something in between
	// branches or is branched to, or may leave another frame in the VM registers. The
	// first of them materializes the frame, and the last one pops the call states.
	// Nothing enters the inlined code but through the branches
	std::vector<bool> shareNext(instrs.size());
	if( m_frame != 0 )
	{
		std::vector<bool> isTarget(instrs.size());
		for( asUINT n = 0; n < instrs.size(); n++ )
			if( instrs[n].target >= 0 )
				isTarget[instrs[n].target] = true;
		int last = -1;
		for( asUINT n = 0; n < instrs.size(); n++ )
		{
			const SJITInstr &instr = instrs[n];
			if( instr.flags & JIT_INSTR_DEAD )
				continue;
			if( isTarget[n] )
				last = -1;
			if( IsBorrowed(n) || (instr.flags & JIT_INSTR_MOVED) )
				continue;
			if( SharesMaterialization(instr.op) )
			{
				if( last >= 0 )
					shareNext[last] = true;
				last = int(n);
			}
			else if( CJITByteCode::IsBranch(instr.op) || CJITByteCode::IsTerminator(instr.op) ||
			         CJITByteCode::IsSyncPoint(instr.op) || (instr.flags & JIT_INSTR_INLINE) )
				last = -1;
		}
	}

	asUINT idx = 0;
	while( idx < instrs.size() && !m_failed )
	{
		// No code for instructions that can never be reached
		if( instrs[idx].flags & JIT_INSTR_DEAD )
		{
			idx++;
			continue;
		}

		if( alignHead[idx] )
			m_uc.cc->align(AlignMode::kCode, 64);
		if( instrs[idx].flags & JIT_INSTR_BLOCK_START )
			m_uc.bind(m_labels[idx]);
		if( m_staticStack )
			m_spOffset = StackOffset(idx);

		asUINT consumed = 1;
		const SJITInstr &instr = instrs[idx];
		bool addrPending = m_vrAddrValid;
#ifndef ASMJIT_NO_LOGGING
		if( m_uc.cc->has_logger() )
			m_uc.commentf("%d %s", instr.pos, asBCInfo[instr.op].name);
#endif
		// Variables that the loop entered next doesn't modify are stored before it
		StoreDirtySlots(m_code->GetStoresBefore(idx));

		if( shareNext[idx] && !m_shareMaterial )
		{
			EmitMaterialize();
			m_shareMaterial = true;
		}

		BaseNode *start = m_uc.cc->cursor();
		if( instr.flags & JIT_INSTR_BAIL )
		{
			Bail(idx);
			m_bailCount++;
		}
		else
		{
			bool ok;
			switch( instr.op )
			{
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
				ok = EmitCompare(idx, consumed);
				break;
			default:
				ok = EmitInstruction(idx);
				break;
			}

			if( !ok )
			{
				m_failed = true;
				break;
			}

			// An address left to the next instruction must not outlive it
			assert( !addrPending || !m_vrAddrValid );
			(void)addrPending;

			// The stack pointer must be where the next instruction expects it, unless
			// the instruction returns to the VM
			assert( !m_staticStack || m_options.noScriptCalls || CJITByteCode::IsTerminator(instrs[idx + consumed - 1].op) ||
			        idx + consumed >= instrs.size() || (instrs[idx + consumed].flags & JIT_INSTR_DEAD) ||
			        m_spOffset == StackOffset(idx + consumed) );

			// The stores planned for the second instruction of a group would be missed
			assert( consumed == 1 || m_code->GetStoresBefore(idx + 1) == 0 );
			StoreDirtySlots(m_code->GetStoresAfter(idx + consumed - 1));

			if( m_options.syncEveryInstr && !CJITByteCode::IsTerminator(instrs[idx + consumed - 1].op) && idx + consumed < instrs.size() )
				SyncAllSlots(instrs[idx + consumed].pos);

			// Suspend requests and returns to the VM only call functions on rare paths,
			// and so do the inlined calls unless the inlined function calls functions
			if( instr.flags & JIT_INSTR_INLINE )
				calls[idx] = m_inlineCalls;
			else if( instr.op != asBC_SUSPEND && instr.op != asBC_RET )
				for( BaseNode *node = start->next(); node && !calls[idx]; node = node->next() )
					calls[idx] = node->is_invoke();
		}

		if( m_shareMaterial && SharesMaterialization(instr.op) && !shareNext[idx] && !IsBorrowed(idx) )
		{
			m_shareMaterial = false;
			EmitDematerialize();
		}

		m_instrCount += consumed;
		idx += consumed;
	}

}

// Makes the frame the one being emitted
void CJITCodeGen::SwitchFrame(int frame)
{
	SFrame &from = m_frames[m_frame];
	from.cached.swap(m_cached);
	from.cachedIndex.swap(m_cachedIndex);
	from.labels.swap(m_labels);

	SFrame &to = m_frames[frame];
	to.cached.swap(m_cached);
	to.cachedIndex.swap(m_cachedIndex);
	to.labels.swap(m_labels);
	m_code = to.code;
	m_frameBase = to.base;
	m_frame = frame;
}

// Registers for the cached variables of the frame being emitted. They are loaded by
// the entry paths
void CJITCodeGen::CreateCachedSlots()
{
	const std::vector<SJITSlot> &slots = m_code->GetSlots();
	for( asUINT n = 0; n < slots.size(); n++ )
	{
		if( slots[n].cacheKind == JIT_SLOT_NONE )
			continue;

		char name[32];
		if( m_frame == 0 )
			snprintf(name, sizeof(name), "var%d", slots[n].offset);
		else
			snprintf(name, sizeof(name), "f%d_var%d", m_frame, slots[n].offset);

		SCachedSlot cached;
		cached.offset = slots[n].offset;
		cached.kind   = slots[n].cacheKind;
		switch( cached.kind )
		{
		case JIT_SLOT_I32: cached.gp  = m_uc.new_gp32(name); break;
		case JIT_SLOT_I64: cached.gp  = m_uc.new_gp64(name); break;
		case JIT_SLOT_F32: cached.vec = m_uc.new_vec128_f32x1(name); break;
		case JIT_SLOT_F64: cached.vec = m_uc.new_vec128_f64x1(name); break;
		}
		m_cachedIndex[cached.offset] = asUINT(m_cached.size());
		m_cached.push_back(cached);
	}
}

// Labels for the basic blocks of the frame being emitted
void CJITCodeGen::CreateBlockLabels()
{
	const std::vector<SJITInstr> &instrs = m_code->GetInstructions();
	m_labels.resize(instrs.size());
	for( asUINT n = 0; n < instrs.size(); n++ )
		if( instrs[n].flags & JIT_INSTR_BLOCK_START )
			m_labels[n] = m_uc.new_label();
}

// The code of an inlined function can't hand anything to the VM but through the exit
// of its frame, see Bail, or while its frame is materialized, see EmitMaterialize.
// If it would otherwise, the function is left to the VM
bool CJITCodeGen::FailIfHidden()
{
	if( m_frame == 0 || m_materialized )
		return false;
	assert( !"the VM can't see the frame of an inlined function" );
	m_failed = true;
	return true;
}

// The registers pointer, the frame and stack pointers, and the call limit are used all
// through the function. AsmJit prefers the registers that calls clobber though, so
// they would be saved and reloaded around every call. This gives them callee-saved
// home registers instead. The allocator assigns the arguments to the registers they
// are passed in, so they are copied to the registers used by the function first.
// The cached variables in the mask get the callee-saved registers left over
void CJITCodeGen::AssignHomeRegs(asUINT slotMask)
{
	BaseNode *cursor = m_uc.cc->set_cursor(m_func);
	Gp regsArg = m_uc.new_gp_ptr("regsArg");
	m_func->set_arg(0, regsArg);
	m_uc.mov(m_regs, regsArg);
	if( m_callLimit.is_valid() )
	{
		Gp limitArg = m_uc.new_gp32("callLimitArg");
		m_func->set_arg(2, limitArg);
		m_uc.mov(m_callLimit, limitArg);
	}
	m_uc.cc->set_cursor(cursor);

	SetHomeRegHints(slotMask);
	CopyLiveArgs();
}

// Gives the cached variables in the mask, the most used first, home registers from
// the ones passed as far as the calling convention preserves them whole
void CJITCodeGen::SetSlotHomeHints(asUINT slotMask, const uint32_t *gpIds, asUINT gpCount, const uint32_t *vecIds, asUINT vecCount)
{
	const CallConv &conv = m_func->detail().call_conv();
	const std::vector<SJITSlot> &slots = m_code->GetSlots();
	asUINT gpNext = 0, vecNext = 0;
	for( ;; )
	{
		int best = -1;
		for( asUINT n = 0; n < slots.size(); n++ )
			if( slots[n].cacheBit >= 0 && (slotMask & (asUINT(1) << slots[n].cacheBit)) &&
			    (best < 0 || slots[n].useCount > slots[best].useCount) )
				best = int(n);
		if( best < 0 )
			break;
		slotMask &= ~(asUINT(1) << slots[best].cacheBit);

		const SCachedSlot &cached = m_cached[m_cachedIndex[slots[best].offset]];
		if( cached.gp.is_valid() )
		{
			while( gpNext < gpCount && !(conv.preserved_regs(RegGroup::kGp) & (RegMask(1) << gpIds[gpNext])) )
				gpNext++;
			if( gpNext < gpCount )
				m_uc.cc->virt_reg_by_reg(cached.gp)->set_home_id_hint(gpIds[gpNext++]);
		}
		else
		{
			VirtReg *vreg = m_uc.cc->virt_reg_by_reg(cached.vec);
			if( conv.save_restore_reg_size(RegGroup::kVec) < vreg->virt_size() )
				continue;
			while( vecNext < vecCount && !(conv.preserved_regs(RegGroup::kVec) & (RegMask(1) << vecIds[vecNext])) )
				vecNext++;
			if( vecNext < vecCount )
				vreg->set_home_id_hint(vecIds[vecNext++]);
		}
	}
}

// AsmJit moves a register passed to a call into the argument register, so if it is
// still needed afterwards it is saved and reloaded around the call, even if it has
// a callee-saved home register. So the calls get copies of the registers that live
// through the function, i.e. the VM registers and the cached variables
void CJITCodeGen::CopyLiveArgs()
{
	BaseNode *cursor = m_uc.cc->cursor();
	for( BaseNode *node = m_func; node; node = node->next() )
	{
		if( !node->is_invoke() )
			continue;

		InvokeNode *call = node->as<InvokeNode>();
		for( uint32_t a = 0; a < call->arg_count(); a++ )
		{
			for( uint32_t v = 0; v < Globals::kMaxValuePack; v++ )
			{
				Operand &op = call->arg(a, v);
				if( !op.is_reg() || !IsLiveThrough(op.as<Reg>()) )
					continue;

				m_uc.cc->set_cursor(call->prev());
				if( op.is_gp() )
				{
					Gp copy = m_uc.cc->new_similar_reg(op.as<Gp>());
					m_uc.mov(copy, op.as<Gp>());
					op = copy;
				}
				else
				{
					Vec copy = m_uc.cc->new_similar_reg(op.as<Vec>());
					m_uc.v_mov(copy, op.as<Vec>());
					op = copy;
				}
			}
		}
	}
	m_uc.cc->set_cursor(cursor);
}

bool CJITCodeGen::IsLiveThrough(const Reg &reg) const
{
	if( reg.id() == m_regs.id() || (m_callLimit.is_valid() && reg.id() == m_callLimit.id()) )
		return true;
	for( size_t n = 0; n < m_cached.size(); n++ )
		if( reg.id() == (m_cached[n].gp.is_valid() ? m_cached[n].gp.id() : m_cached[n].vec.id()) )
			return true;
	return false;
}

bool CJITCodeGen::EmitInstruction(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];

	switch( instr.op )
	{
	// Stack
	case asBC_PopPtr:
	case asBC_PshGPtr:
	case asBC_PshC4:
	case asBC_PshV4:
	case asBC_PSF:
	case asBC_SwapPtr:
	case asBC_PshG4:
	case asBC_PshC8:
	case asBC_PshVPtr:
	case asBC_PopRPtr:
	case asBC_PshRPtr:
	case asBC_GETOBJ:
	case asBC_GETOBJREF:
	case asBC_GETREF:
	case asBC_PshNull:
	case asBC_OBJTYPE:
	case asBC_TYPEID:
	case asBC_PGA:
	case asBC_VAR:
	case asBC_FuncPtr:
	case asBC_PshV8:
	case asBC_PshListElmnt:
		return EmitStackOp(instr);

	// Loads and stores
	case asBC_LdGRdR4:
	case asBC_COPY:
	case asBC_RDSPtr:
	case asBC_ClrVPtr:
	case asBC_SetV4:
	case asBC_SetV8:
	case asBC_ADDSi:
	case asBC_CpyVtoV4:
	case asBC_CpyVtoV8:
	case asBC_CpyVtoR4:
	case asBC_CpyVtoR8:
	case asBC_CpyVtoG4:
	case asBC_CpyRtoV4:
	case asBC_CpyRtoV8:
	case asBC_CpyGtoV4:
	case asBC_WRTV1:
	case asBC_WRTV2:
	case asBC_WRTV4:
	case asBC_WRTV8:
	case asBC_RDR1:
	case asBC_RDR2:
	case asBC_RDR4:
	case asBC_RDR8:
	case asBC_LDG:
	case asBC_LDV:
	case asBC_SetG4:
	case asBC_ChkRefS:
	case asBC_ChkNullV:
	case asBC_SetV1:
	case asBC_SetV2:
	case asBC_ChkNullS:
	case asBC_CHKREF:
	case asBC_LoadThisR:
	case asBC_LoadRObjR:
	case asBC_LoadVObjR:
	case asBC_AllocMem:
	case asBC_SetListSize:
	case asBC_SetListType:
		return EmitLoadStore(instr);

	// Branches
	case asBC_JMP:
	case asBC_JZ:
	case asBC_JNZ:
	case asBC_JS:
	case asBC_JNS:
	case asBC_JP:
	case asBC_JNP:
	case asBC_JMPP:
	case asBC_JLowZ:
	case asBC_JLowNZ:
		return EmitBranch(idx);

	// Tests on the value register
	case asBC_TZ:
	case asBC_TNZ:
	case asBC_TS:
	case asBC_TNS:
	case asBC_TP:
	case asBC_TNP:
	case asBC_NOT:
	case asBC_ClrHi:
	case asBC_NEGi:
	case asBC_IncVi:
	case asBC_DecVi:
	case asBC_BNOT:
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
	case asBC_ADDIi:
	case asBC_SUBIi:
	case asBC_MULIi:
	case asBC_POWi:
	case asBC_POWu:
	case asBC_NEGi64:
	case asBC_BNOT64:
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
		return EmitIntMath(idx);

	case asBC_NEGf:
	case asBC_NEGd:
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
	case asBC_ADDIf:
	case asBC_SUBIf:
	case asBC_MULIf:
	case asBC_POWf:
	case asBC_POWd:
	case asBC_POWdi:
		return EmitFloatMath(idx);

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
		return EmitIncDec(instr);

	case asBC_iTOf:
	case asBC_fTOi:
	case asBC_uTOf:
	case asBC_fTOu:
	case asBC_sbTOi:
	case asBC_swTOi:
	case asBC_ubTOi:
	case asBC_uwTOi:
	case asBC_dTOi:
	case asBC_dTOu:
	case asBC_dTOf:
	case asBC_iTOd:
	case asBC_uTOd:
	case asBC_fTOd:
	case asBC_iTOb:
	case asBC_iTOw:
	case asBC_i64TOi:
	case asBC_uTOi64:
	case asBC_iTOi64:
	case asBC_fTOi64:
	case asBC_dTOi64:
	case asBC_fTOu64:
	case asBC_dTOu64:
	case asBC_i64TOf:
	case asBC_u64TOf:
	case asBC_i64TOd:
	case asBC_u64TOd:
		return EmitConversion(idx);

	// Calls
	case asBC_CALL:
	case asBC_RET:
	case asBC_CALLSYS:
	case asBC_CALLBND:
	case asBC_CALLINTF:
	case asBC_CallPtr:
	case asBC_Thiscall1:
		return EmitCall(idx);

	// Objects
	case asBC_ALLOC:
	case asBC_FREE:
	case asBC_LOADOBJ:
	case asBC_STOREOBJ:
	case asBC_REFCPY:
	case asBC_RefCpyV:
	case asBC_Cast:
		return EmitObjectOp(idx);

	// Misc
	case asBC_SUSPEND:
	case asBC_JitEntry:
	case asBC_STR:
		return EmitMisc(idx);

	default:
		return false;
	}
}

//------------------------------------------------------------------------
// Prologue, dispatch, and stubs

void CJITCodeGen::EmitPrologue()
{
	FuncNode *func = m_uc.add_func(FuncSignature::build<int, asSVMRegisters*, asPWORD, asUINT, asDWORD*>());
	m_func = func;
#ifdef JIT_NATIVE_RETURN
	// Native callers get the value register with the result, see asBC_RET
	AddVRReturn(func->detail());
#endif

	// Only 128bit vectors are used, whose VEX encoded instructions clear the upper
	// halves of the AVX registers, so the VZEROUPPER on return isn't needed
	func->frame().reset_avx_auto_cleanup();

	m_regs = m_uc.new_gp_ptr("regs");
	m_arg  = m_uc.new_gp_ptr("jitArg");
	func->set_arg(0, m_regs);
	func->set_arg(1, m_arg);
	if( m_spInArg )
	{
		m_callerSp = m_uc.new_gp_ptr("callerSp");
		func->set_arg(3, m_callerSp);
	}

	// The call limit is only needed for the calls of script functions
	const std::vector<SJITInstr> &instrs = m_code->GetInstructions();
	for( asUINT n = 0; n < instrs.size() && !m_options.noScriptCalls; n++ )
	{
		asEBCInstr op = instrs[n].op;
		if( op == asBC_CALL || op == asBC_CALLINTF || op == asBC_CALLBND || op == asBC_CallPtr || op == asBC_ALLOC )
		{
			m_callLimit = m_uc.new_gp32("callLimit");
			func->set_arg(2, m_callLimit);
			break;
		}
	}

	// A C++ exception can only leave the functions that call registered functions
	// or behaviours directly, or script functions natively
	m_guarded = false;
	for( asUINT n = 0; n < instrs.size() && m_options.guardedEntry && !m_guarded; n++ )
		m_guarded = m_callLimit.is_valid() || instrs[n].op == asBC_CALLSYS || instrs[n].op == asBC_Thiscall1 ||
		            CallsBehaviourDirectly(instrs[n]);

	// The frame and stack pointers are set up by the entry paths
	m_fp = m_uc.new_gp_ptr("fp");
	if( !m_staticStack )
		m_sp = m_uc.new_gp_ptr("sp");

	if( m_vrInReg )
		m_vr = m_uc.new_gp64("vr");

	m_bailPC = m_uc.new_gp_ptr("bailPC");

	CreateCachedSlots();
}

// Whether entering at the entry point needs to load anything, otherwise the
// dispatch goes straight to its instruction
bool CJITCodeGen::EntryNeedsStub(asUINT n) const
{
	if( m_options.syncEveryInstr )
		return m_vrInReg || !m_cached.empty();
	asUINT entry = m_code->GetEntries()[n];
	const SJITBlock &block = m_code->GetBlocks()[m_code->GetInstructions()[entry].block];
	return (m_vrInReg && block.vrLiveIn) || m_code->GetEntryMask(entry) != 0;
}

Label CJITCodeGen::EntryTarget(asUINT n)
{
	return EntryNeedsStub(n) ? m_entryLabels[n] : InstrLabel(m_code->GetEntries()[n]);
}

// Binary search on the 1-based entry index in jitArg. The lowest entry comes
// last, so that the first instruction, which follows, is entered without a jump
void CJITCodeGen::EmitEntryDispatch(asUINT lo, asUINT hi)
{
	if( lo == hi )
	{
		const SJITInstr &first = m_code->GetInstructions()[0];
		if( lo != 0 || EntryNeedsStub(0) || m_code->GetEntries()[0] != 0 || (first.flags & JIT_INSTR_DEAD) )
			m_uc.j(EntryTarget(lo));
		return;
	}

	asUINT mid = (lo + hi) / 2;
	if( hi == mid + 1 )
		m_uc.j(EntryTarget(hi), ucmp_gt(m_arg, Imm(int(mid + 1))));
	else
	{
		Label lower = m_uc.new_label();
		m_uc.j(lower, ucmp_le(m_arg, Imm(int(mid + 1))));
		EmitEntryDispatch(mid + 1, hi);
		m_uc.bind(lower);
	}
	EmitEntryDispatch(lo, mid);
}

void CJITCodeGen::EmitEntryStubs()
{
	const std::vector<asUINT> &entries = m_code->GetEntries();
	const std::vector<SJITInstr> &instrs = m_code->GetInstructions();
	const std::vector<SJITBlock> &blocks = m_code->GetBlocks();
	for( asUINT n = 0; n < entries.size(); n++ )
	{
		if( !EntryNeedsStub(n) )
			continue;
		m_uc.bind(m_entryLabels[n]);

		// Only what may be read before being written or stored needs to be loaded
		const SJITBlock &block = blocks[instrs[entries[n]].block];
		if( m_options.syncEveryInstr )
		{
			ReloadVR();
			ReloadCachedSlots();
		}
		else
		{
			if( block.vrLiveIn )
				ReloadVR();
			ReloadSlots(m_code->GetEntryMask(entries[n]));
		}
		m_uc.j(InstrLabel(entries[n]));
	}
}

void CJITCodeGen::EmitBailStubs()
{
	for( asUINT n = 0; n < m_bails.size(); n++ )
	{
		SwitchFrame(m_bails[n].frame);
		m_materialized = m_bails[n].materialized;
		m_materialBorrowed = m_bails[n].borrowed;
		m_uc.bind(m_bails[n].label);
		Bail(m_bails[n].idx);
	}
	m_materialized = false;
	m_materialBorrowed = false;

	// The exits of the inlined functions store their callers, see EmitInlineExit
	for( asUINT n = 1; n < m_frames.size(); n++ )
		if( m_frames[n].exitUsed )
			EmitInlineExit(int(n));
	SwitchFrame(0);

	// Common tail: the cached variables and the value register have been stored
	// by the bail sites, so only the frame, the stack pointer, and the program
	// pointer remain. A static stack pointer is stored by the bail sites too
	m_uc.bind(m_bailCommon);
	StoreFrame();
	if( !m_staticStack )
		SyncStack();
	m_uc.store(RegsField(offsetof(asSVMRegisters, programPointer)), m_bailPC);
	Leave();

	// The VM registers have the frame of the inlined function already where its frame
	// is materialized, and the call states have been pushed
	if( m_bailMaterializedUsed )
	{
		m_uc.bind(m_bailMaterialized);
		if( !m_staticStack )
			SyncStack();
		m_uc.store(RegsField(offsetof(asSVMRegisters, programPointer)), m_bailPC);
		Leave();
	}
	if( m_bailMaterializedBorrowedUsed )
	{
		m_uc.bind(m_bailMaterializedBorrowed);
		if( !m_staticStack )
			SyncStack();
		m_uc.store(RegsField(offsetof(asSVMRegisters, programPointer)), m_bailPC);
		m_leaveBorrowedUsed = true;
		m_uc.j(m_leaveBorrowed);
	}
}

//------------------------------------------------------------------------
// Memory operands

Mem CJITCodeGen::RegsField(size_t offset)
{
	return mem_ptr(m_regs, int32_t(offset));
}

Mem CJITCodeGen::ContextField(int offset)
{
	return mem_ptr(m_regs, int32_t(offset));
}

Mem CJITCodeGen::VRMem()
{
	return RegsField(offsetof(asSVMRegisters, valueRegister));
}

Mem CJITCodeGen::Var(int offset, int byteDisp)
{
	return mem_ptr(m_fp, -(offset + m_frameBase) * 4 + byteDisp);
}

Mem CJITCodeGen::Stack(int dwordOffset)
{
	if( m_staticStack )
		return mem_ptr(m_fp, m_spOffset + dwordOffset * 4);
	return mem_ptr(m_sp, dwordOffset * 4);
}

int CJITCodeGen::StackOffset(asUINT idx) const
{
	return -int(m_frameBase + m_code->GetFunction()->scriptData->variableSpace + m_code->GetStackDepth(idx)) * 4;
}

void CJITCodeGen::PushStack(int bytes)
{
	if( m_staticStack )
		m_spOffset -= bytes;
	else if( bytes )
		m_uc.sub(m_sp, m_sp, Imm(bytes));
}

void CJITCodeGen::PopStack(int bytes)
{
	if( m_staticStack )
		m_spOffset += bytes;
	else if( bytes )
		m_uc.add(m_sp, m_sp, Imm(bytes));
}

CJITCodeGen::Gp CJITCodeGen::StackPointer()
{
	if( !m_staticStack )
		return m_sp;
	Gp t = m_uc.new_gp_ptr();
	m_uc.lea(t, mem_ptr(m_fp, m_spOffset));
	return t;
}

Mem CJITCodeGen::Global(asPWORD address, Gp &tmp)
{
	tmp = PtrConst(address);
	return mem_ptr(tmp);
}

Gp CJITCodeGen::PtrConst(asPWORD value)
{
	Gp t = m_uc.new_gp_ptr();
	m_uc.mov(t, Imm(int64_t(value)));
	return t;
}

InvokeNode *CJITCodeGen::Invoke(const void *fn, const FuncSignature &sig)
{
	InvokeNode *node = 0;
	m_uc.cc->invoke(Out(node), Imm(int64_t(asPWORD(fn))), sig);
	return node;
}

//------------------------------------------------------------------------
// Variable access

CJITCodeGen::SCachedSlot *CJITCodeGen::FindCached(int offset)
{
	std::map<int, asUINT>::iterator it = m_cachedIndex.find(offset);
	if( it == m_cachedIndex.end() )
		return 0;
	return &m_cached[it->second];
}

Gp CJITCodeGen::Load32(int offset)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_I32 )
		return c->gp;

	Gp t = m_uc.new_gp32();
	if( c && c->kind == JIT_SLOT_F32 )
		m_uc.s_mov_u32(t, c->vec);
	else
		m_uc.load_u32(t, Var(offset));
	return t;
}

Gp CJITCodeGen::Load64(int offset)
{
	assert( Is64Bit() );
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_I64 )
		return c->gp;

	Gp t = m_uc.new_gp64();
	if( c && c->kind == JIT_SLOT_F64 )
		m_uc.s_mov_u64(t, c->vec);
	else
		m_uc.load_u64(t, Var(offset));
	return t;
}

Gp CJITCodeGen::LoadPtr(int offset)
{
	// Pointers are never cached
	Gp t = m_uc.new_gp_ptr();
	m_uc.load(t, Var(offset));
	return t;
}

Vec CJITCodeGen::LoadF32(int offset)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_F32 )
		return c->vec;

	Vec v = m_uc.new_vec128_f32x1();
	if( c && c->kind == JIT_SLOT_I32 )
		m_uc.s_mov_u32(v, c->gp);
	else
		m_uc.v_loadu32_f32(v, Var(offset));
	return v;
}

Vec CJITCodeGen::LoadF64(int offset)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_F64 )
		return c->vec;

	Vec v = m_uc.new_vec128_f64x1();
	if( c && c->kind == JIT_SLOT_I64 )
		m_uc.s_mov_u64(v, c->gp);
	else
		m_uc.v_loadu64_f64(v, Var(offset));
	return v;
}

Gp CJITCodeGen::Dst32(int offset)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_I32 )
		return c->gp;
	return m_uc.new_gp32();
}

Gp CJITCodeGen::Dst64(int offset)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_I64 )
		return c->gp;
	return m_uc.new_gp64();
}

Vec CJITCodeGen::DstF32(int offset)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_F32 )
		return c->vec;
	return m_uc.new_vec128_f32x1();
}

Vec CJITCodeGen::DstF64(int offset)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_F64 )
		return c->vec;
	return m_uc.new_vec128_f64x1();
}

void CJITCodeGen::Commit32(int offset, const Gp &value)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_I32 )
	{
		if( c->gp.id() != value.id() )
			m_uc.mov(c->gp, value);
	}
	else if( c && c->kind == JIT_SLOT_F32 )
		m_uc.s_mov_u32(c->vec, value);
	else
		m_uc.store_u32(Var(offset), value);
}

void CJITCodeGen::Commit64(int offset, const Gp &value)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_I64 )
	{
		if( c->gp.id() != value.id() )
			m_uc.mov(c->gp, value);
	}
	else if( c && c->kind == JIT_SLOT_F64 )
		m_uc.s_mov_u64(c->vec, value);
	else
		m_uc.store_u64(Var(offset), value);
}

void CJITCodeGen::CommitF32(int offset, const Vec &value)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_F32 )
	{
		if( c->vec.id() != value.id() )
			m_uc.v_mov(c->vec, value);
	}
	else if( c && c->kind == JIT_SLOT_I32 )
		m_uc.s_mov_u32(c->gp, value);
	else
		m_uc.v_storeu32_f32(Var(offset), value);
}

void CJITCodeGen::CommitF64(int offset, const Vec &value)
{
	SCachedSlot *c = FindCached(offset);
	if( c && c->kind == JIT_SLOT_F64 )
	{
		if( c->vec.id() != value.id() )
			m_uc.v_mov(c->vec, value);
	}
	else if( c && c->kind == JIT_SLOT_I64 )
		m_uc.s_mov_u64(c->gp, value);
	else
		m_uc.v_storeu64_f64(Var(offset), value);
}

void CJITCodeGen::StorePtr(int offset, const Gp &value)
{
	m_uc.store(Var(offset), value);
}

void CJITCodeGen::Copy32(const Mem &dst, const Mem &src)
{
	Gp t = m_uc.new_gp32();
	m_uc.load_u32(t, src);
	m_uc.store_u32(dst, t);
}

void CJITCodeGen::Copy64(const Mem &dst, const Mem &src)
{
	if( Is64Bit() )
	{
		Gp t = m_uc.new_gp64();
		m_uc.load_u64(t, src);
		m_uc.store_u64(dst, t);
	}
	else
	{
		Vec v = m_uc.new_vec128_f64x1();
		m_uc.v_loadu64_f64(v, src);
		m_uc.v_storeu64_f64(dst, v);
	}
}

//------------------------------------------------------------------------
// Value register

void CJITCodeGen::LoadVR32(const Gp &dst)
{
	if( m_vrInReg )
		m_uc.mov(dst, m_vr.r32());
	else
		m_uc.load_u32(dst, VRMem());
}

void CJITCodeGen::LoadVR64(const Gp &dst)
{
	assert( Is64Bit() );
	if( m_vrInReg )
		m_uc.mov(dst, m_vr);
	else
		m_uc.load_u64(dst, VRMem());
}

void CJITCodeGen::LoadVRPtr(const Gp &dst)
{
	if( m_vrInReg )
		m_uc.mov(dst, m_vr);
	else
		m_uc.load(dst, VRMem());
}

void CJITCodeGen::StoreVR32(const Gp &src)
{
	// A 32bit write zero extends the register. The VM leaves the upper bits
	// unchanged, but the compiler never reads more than it wrote
	if( m_vrInReg )
		m_uc.mov(m_vr.r32(), src);
	else
		m_uc.store_u32(VRMem(), src);
}

void CJITCodeGen::StoreVR64(const Gp &src)
{
	assert( Is64Bit() );
	if( m_vrInReg )
		m_uc.mov(m_vr, src);
	else
		m_uc.store_u64(VRMem(), src);
}

void CJITCodeGen::StoreVRPtr(const Gp &src)
{
	if( m_vrInReg )
		m_uc.mov(m_vr, src);
	else
		m_uc.store(VRMem(), src);
}

void CJITCodeGen::StoreVRImm32(int value)
{
	if( m_vrInReg )
		m_uc.mov(m_vr.r32(), Imm(value));
	else
	{
		Gp t = m_uc.new_gp32();
		m_uc.mov(t, Imm(value));
		m_uc.store_u32(VRMem(), t);
	}
}

void CJITCodeGen::SyncVR()
{
	if( m_vrInReg )
		m_uc.store_u64(VRMem(), m_vr);
}

void CJITCodeGen::ReloadVR()
{
	if( m_vrInReg )
		m_uc.load_u64(m_vr, VRMem());
}

bool CJITCodeGen::CanFoldVRAddr(asUINT idx) const
{
	// The next instruction must be emitted right after this one without anything
	// in between that may give the value register to the VM
	const std::vector<SJITInstr> &instrs = m_code->GetInstructions();
	if( m_options.syncEveryInstr || idx + 1 >= instrs.size() ||
		(instrs[idx + 1].flags & (JIT_INSTR_BLOCK_START | JIT_INSTR_BAIL | JIT_INSTR_SKIP | JIT_INSTR_DEAD)) ||
		m_code->IsVRLiveAfter(idx + 1) )
		return false;

	switch( instrs[idx + 1].op )
	{
	case asBC_RDR1: case asBC_RDR2: case asBC_RDR4: case asBC_RDR8:
	case asBC_WRTV1: case asBC_WRTV2: case asBC_WRTV4: case asBC_WRTV8:
	case asBC_INCi8: case asBC_DECi8: case asBC_INCi16: case asBC_DECi16:
	case asBC_INCi: case asBC_DECi: case asBC_INCi64: case asBC_DECi64:
	case asBC_INCf: case asBC_DECf: case asBC_INCd: case asBC_DECd:
		return true;
	default:
		return false;
	}
}

void CJITCodeGen::SetVRAddr(asUINT idx, const Mem &addr)
{
	if( CanFoldVRAddr(idx) )
	{
		m_vrAddr = addr;
		m_vrAddrValid = true;
		return;
	}

	Gp t = m_uc.new_gp_ptr();
	m_uc.lea(t, addr);
	StoreVRPtr(t);
}

Mem CJITCodeGen::VRAddr()
{
	if( m_vrAddrValid )
	{
		m_vrAddrValid = false;
		return m_vrAddr;
	}

	Gp p = m_uc.new_gp_ptr();
	LoadVRPtr(p);
	return mem_ptr(p);
}

//------------------------------------------------------------------------
// Synchronization with the VM

void CJITCodeGen::SetPC(asUINT pos)
{
	if( FailIfHidden() )
		return;
	Gp t = PtrConst(asPWORD(m_code->GetByteCode() + pos));
	m_uc.store(RegsField(offsetof(asSVMRegisters, programPointer)), t);
}

void CJITCodeGen::SyncStack()
{
	m_uc.store(RegsField(offsetof(asSVMRegisters, stackPointer)), StackPointer());
}

// A static stack pointer doesn't need the one the VM or the helper has left, which
// is lower if the callee leaves something on the stack, like the default copy
// constructors do with their argument
void CJITCodeGen::ReloadStack()
{
	if( !m_staticStack )
		m_uc.load(m_sp, RegsField(offsetof(asSVMRegisters, stackPointer)));
}

// After a call has completed the instruction, the static stack pointer is the one
// of the next instruction, which exists as calls don't end the bytecode
void CJITCodeGen::ReloadStackAfter(asUINT idx)
{
	if( m_staticStack )
		m_spOffset = StackOffset(idx + 1);
	ReloadStack();
}

// Writes back the frame, see JIT_FRAME_BIT. The frames of the inlined functions
// are written by EmitMaterialize
void CJITCodeGen::StoreFrame()
{
	if( m_frame != 0 )
		return;
	m_uc.store(RegsField(offsetof(asSVMRegisters, stackFramePointer)), m_fp);
	m_uc.store(ContextField(JIT_GetContextLayout().currentFunction), PtrConst(asPWORD(m_code->GetFunction())));
}

void CJITCodeGen::StoreCachedSlots()
{
	for( asUINT n = 0; n < m_cached.size(); n++ )
		StoreCachedSlot(m_cached[n].offset);
}

// Reloading all cached slots is only valid right after they have all been
// stored, otherwise registers holding newer values would be overwritten
void CJITCodeGen::ReloadCachedSlots()
{
	for( asUINT n = 0; n < m_cached.size(); n++ )
		ReloadCachedSlot(m_cached[n].offset);
}

void CJITCodeGen::StoreCachedSlot(int offset)
{
	SCachedSlot *c = FindCached(offset);
	if( c == 0 )
		return;
	switch( c->kind )
	{
	case JIT_SLOT_I32: m_uc.store_u32(Var(c->offset), c->gp); break;
	case JIT_SLOT_I64: m_uc.store_u64(Var(c->offset), c->gp); break;
	case JIT_SLOT_F32: m_uc.v_storeu32_f32(Var(c->offset), c->vec); break;
	case JIT_SLOT_F64: m_uc.v_storeu64_f64(Var(c->offset), c->vec); break;
	}
}

void CJITCodeGen::ReloadCachedSlot(int offset)
{
	SCachedSlot *c = FindCached(offset);
	if( c == 0 )
		return;
	switch( c->kind )
	{
	case JIT_SLOT_I32: m_uc.load_u32(c->gp, Var(c->offset)); break;
	case JIT_SLOT_I64: m_uc.load_u64(c->gp, Var(c->offset)); break;
	case JIT_SLOT_F32: m_uc.v_loadu32_f32(c->vec, Var(c->offset)); break;
	case JIT_SLOT_F64: m_uc.v_loadu64_f64(c->vec, Var(c->offset)); break;
	}
}

// Stores the cached slots in the mask, and the frame if it has the bit, see
// CJITByteCode::GetDirtyMask
void CJITCodeGen::StoreDirtySlots(asUINT mask)
{
	if( mask & JIT_FRAME_BIT )
		StoreFrame();
	for( asUINT n = 0; n < m_cached.size() && mask; n++ )
	{
		int bit = m_code->GetCacheBit(m_cached[n].offset);
		if( bit >= 0 && (mask & (asUINT(1) << bit)) )
			StoreCachedSlot(m_cached[n].offset);
	}
}

// Loads the cached slots in the mask from memory
void CJITCodeGen::ReloadSlots(asUINT mask)
{
	for( asUINT n = 0; n < m_cached.size() && mask; n++ )
	{
		int bit = m_code->GetCacheBit(m_cached[n].offset);
		if( bit >= 0 && (mask & (asUINT(1) << bit)) )
			ReloadCachedSlot(m_cached[n].offset);
	}
}

// The value register is only written back where it is live, so that the register
// isn't kept alive for this, e.g. across calls returning nothing
void CJITCodeGen::SyncAll(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	StoreDirtySlots(m_code->GetDirtyMask(idx));
	SyncStack();
	if( m_code->IsVRLiveBefore(idx) )
		SyncVR();
	EmitMaterialize();
	SetPC(instr.pos);
}

// Calls always clobber the value register, so it doesn't have to be written back
void CJITCodeGen::SyncForCall(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	StoreDirtySlots(m_code->GetDirtyMask(idx));
	SyncStack();
	EmitMaterialize();
	SetPC(instr.pos);
}

void CJITCodeGen::SyncAllSlots(asUINT pos)
{
	StoreCachedSlots();
	StoreFrame();
	SyncStack();
	SyncVR();
	SetPC(pos);
}

void CJITCodeGen::ReloadAll(asUINT idx)
{
	ReloadStack();
	if( m_code->IsVRLiveAfter(idx) )
		ReloadVR();
	ReloadLiveSlots(idx);
}

// Temporary variables that won't be read anymore may not have been stored, but
// loading them again doesn't matter. All are loaded when syncing every instruction.
// In an inlined function the callers are loaded too, which the VM has seen while
// the frames were materialized, see EmitMaterialize
void CJITCodeGen::ReloadLiveSlots(asUINT idx)
{
	if( m_options.syncEveryInstr )
		ReloadCachedSlots();
	else
		ReloadSlots(m_code->GetReloadMask(idx));

	int frame = m_frame;
	for( int f = frame; f != 0; f = m_frames[f].caller )
	{
		SwitchFrame(m_frames[f].caller);
		ReloadSlots(m_code->GetReloadMask(m_frames[f].callIdx));
	}
	if( frame != 0 )
		SwitchFrame(frame);
}

//------------------------------------------------------------------------
// Control flow helpers

Label CJITCodeGen::InstrLabel(asUINT idx)
{
	assert( m_labels[idx].is_valid() );
	return m_labels[idx];
}

// Returns to the VM which will re-execute the instruction. The frame is stored by
// the common tail of the bail sites, or by the exit of the frame of an inlined function.
// A materialized frame is in the VM registers already, see EmitMaterialize
void CJITCodeGen::Bail(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	StoreDirtySlots(m_code->GetDirtyMask(idx) & ~JIT_FRAME_BIT);
	if( m_code->IsVRLiveBefore(idx) )
		SyncVR();
	if( m_staticStack )
	{
		// The stubs are emitted after the body
		m_spOffset = StackOffset(idx);
		SyncStack();
	}
	m_uc.mov(m_bailPC, Imm(int64_t(asPWORD(instr.bc))));
	if( m_materialized && m_materialBorrowed )
	{
		m_bailMaterializedBorrowedUsed = true;
		m_uc.j(m_bailMaterializedBorrowed);
	}
	else if( m_materialized )
	{
		m_bailMaterializedUsed = true;
		m_uc.j(m_bailMaterialized);
	}
	else if( m_frame != 0 )
	{
		m_frames[m_frame].exitUsed = true;
		m_uc.j(m_frames[m_frame].exit);
	}
	else
		m_uc.j(m_bailCommon);
}

// Label to a cold stub that bails out at the instruction
Label CJITCodeGen::BailLabel(asUINT idx)
{
	SBail bail;
	bail.label = m_uc.new_label();
	bail.idx   = idx;
	bail.frame = m_frame;
	bail.materialized = m_materialized;
	bail.borrowed = m_materialBorrowed;
	m_bails.push_back(bail);
	return bail.label;
}

// Returns to the VM after a helper has updated the VM registers
void CJITCodeGen::Leave()
{
	if( FailIfHidden() )
		return;
	m_uc.j(LeaveLabel());
}

// Returns to the VM if the helper result is non-zero
void CJITCodeGen::EmitLeaveIf(const Gp &result)
{
	if( FailIfHidden() )
		return;
	m_uc.j(LeaveLabel(), test_nz(result));
}

// Where the VM takes over. The materialized frames of the inlined functions with
// borrowed parameters get references of their own first, which the VM releases,
// see JIT_OwnBorrowed
Label CJITCodeGen::LeaveLabel()
{
	if( !m_materialized || !m_materialBorrowed )
		return m_leave;
	m_leaveBorrowedUsed = true;
	return m_leaveBorrowed;
}

// Starts a cold range at the label. Returns the node to pass to EndCold
BaseNode *CJITCodeGen::BeginCold(const Label &label)
{
	BaseNode *start = m_uc.cc->cursor();
	m_uc.bind(label);
	return start;
}

// Ends the cold range, which continues at cont
void CJITCodeGen::EndCold(BaseNode *start, const Label &cont)
{
	m_uc.j(cont);
	m_cold.push_back(std::pair<BaseNode*, BaseNode*>(start->next(), m_uc.cc->cursor()));
}

// Moves the cold ranges to the cursor, which must follow an unconditional jump.
// The ranges start with a label and end with a jump, so the code is the same,
// only the common paths fall through instead of jumping over the rare ones
void CJITCodeGen::EmitColdCode()
{
	for( size_t n = 0; n < m_cold.size(); n++ )
	{
		BaseNode *node = m_cold[n].first;
		for(;;)
		{
			BaseNode *next = node->next();
			m_uc.cc->remove_node(node);
			m_uc.cc->add_node(node);
			if( node == m_cold[n].second )
				break;
			node = next;
		}
	}
}

//------------------------------------------------------------------------
// Stack operations

bool CJITCodeGen::EmitStackOp(const SJITInstr &instr)
{
	const asDWORD *bc = instr.bc;
	int a0 = asBC_SWORDARG0(bc);

	switch( instr.op )
	{
	case asBC_PopPtr:
		PopStack(PTR_BYTES);
		break;

	case asBC_PshGPtr:
		{
			Gp g;
			Mem src = Global(asBC_PTRARG(bc), g);
			Gp t = m_uc.new_gp_ptr();
			m_uc.load(t, src);
			PushStack(PTR_BYTES);
			m_uc.store(Stack(0), t);
		}
		break;

	case asBC_PshC4:
	case asBC_TYPEID:
		PushStack(4);
		StoreImm32(Stack(0), int(asBC_DWORDARG(bc)));
		break;

	case asBC_PshV4:
		{
			Gp t = Load32(a0);
			PushStack(4);
			m_uc.store_u32(Stack(0), t);
		}
		break;

	case asBC_PshV8:
		{
			PushStack(8);
			SCachedSlot *c = FindCached(a0);
			if( c && c->kind == JIT_SLOT_I64 )
				m_uc.store_u64(Stack(0), c->gp);
			else if( c && c->kind == JIT_SLOT_F64 )
				m_uc.v_storeu64_f64(Stack(0), c->vec);
			else
				Copy64(Stack(0), Var(a0));
		}
		break;

	case asBC_PSF:
		{
			Gp t = m_uc.new_gp_ptr();
			m_uc.lea(t, Var(a0));
			PushStack(PTR_BYTES);
			m_uc.store(Stack(0), t);
		}
		break;

	case asBC_SwapPtr:
		{
			Gp p0 = m_uc.new_gp_ptr();
			Gp p1 = m_uc.new_gp_ptr();
			m_uc.load(p0, Stack(0));
			m_uc.load(p1, Stack(AS_PTR_SIZE));
			m_uc.store(Stack(0), p1);
			m_uc.store(Stack(AS_PTR_SIZE), p0);
		}
		break;

	case asBC_PshG4:
		{
			Gp g;
			Mem src = Global(asBC_PTRARG(bc), g);
			Gp t = m_uc.new_gp32();
			m_uc.load_u32(t, src);
			PushStack(4);
			m_uc.store_u32(Stack(0), t);
		}
		break;

	case asBC_PshC8:
		{
			PushStack(8);
			asQWORD value = asBC_QWORDARG(bc);
			if( Is64Bit() )
			{
				Gp t = m_uc.new_gp64();
				m_uc.mov(t, Imm(int64_t(value)));
				m_uc.store_u64(Stack(0), t);
			}
			else
			{
				Gp t = m_uc.new_gp32();
				m_uc.mov(t, Imm(int(asDWORD(value))));
				m_uc.store_u32(Stack(0), t);
				m_uc.mov(t, Imm(int(asDWORD(value >> 32))));
				m_uc.store_u32(Stack(1), t);
			}
		}
		break;

	case asBC_PshVPtr:
		{
			Gp t = LoadPtr(a0);
			PushStack(PTR_BYTES);
			m_uc.store(Stack(0), t);
		}
		break;

	case asBC_PopRPtr:
		{
			Gp t = m_uc.new_gp_ptr();
			m_uc.load(t, Stack(0));
			StoreVRPtr(t);
			PopStack(PTR_BYTES);
		}
		break;

	case asBC_PshRPtr:
		{
			Gp t = m_uc.new_gp_ptr();
			LoadVRPtr(t);
			PushStack(PTR_BYTES);
			m_uc.store(Stack(0), t);
		}
		break;

	case asBC_GETOBJ:
		{
			// Move the object from the variable to the stack
			int w = asBC_WORDARG0(bc);
			Gp off = m_uc.new_gp_ptr();
			m_uc.load(off, Stack(w));
			if( m_frameBase )
				m_uc.add(off, off, Imm(m_frameBase));
			m_uc.shl(off, off, Imm(2));
			Gp v = m_uc.new_gp_ptr();
			m_uc.sub(v, m_fp, off);
			Gp obj = m_uc.new_gp_ptr();
			m_uc.load(obj, mem_ptr(v));
			m_uc.store(Stack(w), obj);
			m_uc.store_zero_reg(mem_ptr(v));
		}
		break;

	case asBC_GETOBJREF:
		{
			int w = asBC_WORDARG0(bc);
			Gp off = m_uc.new_gp_ptr();
			m_uc.load(off, Stack(w));
			if( m_frameBase )
				m_uc.add(off, off, Imm(m_frameBase));
			m_uc.shl(off, off, Imm(2));
			Gp v = m_uc.new_gp_ptr();
			m_uc.sub(v, m_fp, off);
			Gp obj = m_uc.new_gp_ptr();
			m_uc.load(obj, mem_ptr(v));
			m_uc.store(Stack(w), obj);
		}
		break;

	case asBC_GETREF:
		{
			int w = asBC_WORDARG0(bc);
			Gp off = m_uc.new_gp_ptr();
			m_uc.load(off, Stack(w));
			if( m_frameBase )
				m_uc.add(off, off, Imm(m_frameBase));
			m_uc.shl(off, off, Imm(2));
			Gp v = m_uc.new_gp_ptr();
			m_uc.sub(v, m_fp, off);
			m_uc.store(Stack(w), v);
		}
		break;

	case asBC_PshNull:
		PushStack(PTR_BYTES);
		m_uc.store_zero_reg(Stack(0));
		break;

	case asBC_OBJTYPE:
	case asBC_PGA:
	case asBC_FuncPtr:
		{
			Gp t = PtrConst(asBC_PTRARG(bc));
			PushStack(PTR_BYTES);
			m_uc.store(Stack(0), t);
		}
		break;

	case asBC_VAR:
		{
			Gp t = PtrConst(asPWORD(asPWORD(a0)));
			PushStack(PTR_BYTES);
			m_uc.store(Stack(0), t);
		}
		break;

	case asBC_PshListElmnt:
		{
			Gp var = LoadPtr(a0);
			m_uc.add(var, var, Imm(int(asBC_DWORDARG(bc))));
			PushStack(PTR_BYTES);
			m_uc.store(Stack(0), var);
		}
		break;

	default:
		return false;
	}

	return true;
}

//------------------------------------------------------------------------
// Loads and stores

bool CJITCodeGen::EmitLoadStore(const SJITInstr &instr)
{
	const asDWORD *bc = instr.bc;
	int a0 = asBC_SWORDARG0(bc);
	int a1 = asBC_SWORDARG1(bc);
	asUINT idx = asUINT(&instr - &m_code->GetInstructions()[0]);

	switch( instr.op )
	{
	case asBC_LdGRdR4:
		{
			Gp p = PtrConst(asBC_PTRARG(bc));
			StoreVRPtr(p);
			Gp t = m_uc.new_gp32();
			m_uc.load_u32(t, mem_ptr(p));
			Commit32(a0, t);
		}
		break;

	case asBC_COPY:
		{
			// Both pointers are verified before the stack is changed so the VM
			// can re-execute the instruction to raise the exception
			Gp d = m_uc.new_gp_ptr();
			Gp s = m_uc.new_gp_ptr();
			m_uc.load(d, Stack(0));
			m_uc.load(s, Stack(AS_PTR_SIZE));
			Label bail = BailLabel(idx);
			m_uc.j(bail, test_z(d));
			m_uc.j(bail, test_z(s));

			asUINT bytes = asBC_WORDARG0(bc) * 4;
			if( bytes <= 64 )
			{
				asUINT off = 0;
				while( bytes - off >= 8 && Is64Bit() )
				{
					Copy64(mem_ptr(d, off), mem_ptr(s, off));
					off += 8;
				}
				while( off < bytes )
				{
					Copy32(mem_ptr(d, off), mem_ptr(s, off));
					off += 4;
				}
			}
			else
			{
				InvokeNode *call = Invoke((const void*)JIT_MemCpy, FuncSignature::build<void, void*, const void*, asUINT>());
				call->set_arg(0, d);
				call->set_arg(1, s);
				call->set_arg(2, Imm(int(bytes)));
			}

			PopStack(PTR_BYTES);
			m_uc.store(Stack(0), d);
		}
		break;

	case asBC_RDSPtr:
		{
			Gp a = m_uc.new_gp_ptr();
			m_uc.load(a, Stack(0));
			m_uc.j(BailLabel(idx), test_z(a));
			m_uc.load(a, mem_ptr(a));
			m_uc.store(Stack(0), a);
		}
		break;

	case asBC_ClrVPtr:
		m_uc.store_zero_reg(Var(a0));
		break;

	case asBC_SetV1:
	case asBC_SetV2:
	case asBC_SetV4:
		{
			SCachedSlot *c = FindCached(a0);
			if( c && c->kind == JIT_SLOT_I32 )
				m_uc.mov(c->gp, Imm(int(asBC_DWORDARG(bc))));
			else if( c )
			{
				Gp t = m_uc.new_gp32();
				m_uc.mov(t, Imm(int(asBC_DWORDARG(bc))));
				Commit32(a0, t);
			}
			else
				StoreImm32(Var(a0), int(asBC_DWORDARG(bc)));
		}
		break;

	case asBC_SetV8:
		{
			asQWORD value = asBC_QWORDARG(bc);
			SCachedSlot *c = FindCached(a0);
			if( Is64Bit() )
			{
				if( c && c->kind == JIT_SLOT_I64 )
					m_uc.mov(c->gp, Imm(int64_t(value)));
				else
				{
					Gp t = m_uc.new_gp64();
					m_uc.mov(t, Imm(int64_t(value)));
					Commit64(a0, t);
				}
			}
			else
			{
				Gp t = m_uc.new_gp32();
				m_uc.mov(t, Imm(int(asDWORD(value))));
				m_uc.store_u32(Var(a0), t);
				m_uc.mov(t, Imm(int(asDWORD(value >> 32))));
				m_uc.store_u32(Var(a0, 4), t);
				if( c && c->kind == JIT_SLOT_F64 )
					m_uc.v_loadu64_f64(c->vec, Var(a0));
			}
		}
		break;

	case asBC_ADDSi:
		{
			Gp a = m_uc.new_gp_ptr();
			m_uc.load(a, Stack(0));
			m_uc.j(BailLabel(idx), test_z(a));
			m_uc.add(a, a, Imm(int(asBC_SWORDARG0(bc))));
			m_uc.store(Stack(0), a);
		}
		break;

	case asBC_CpyVtoV4:
		Commit32(a0, Load32(a1));
		break;

	case asBC_CpyVtoV8:
		if( Is64Bit() )
			Commit64(a0, Load64(a1));
		else
			CommitF64(a0, LoadF64(a1));
		break;

	case asBC_CpyVtoR4:
		StoreVR32(Load32(a0));
		break;

	case asBC_CpyVtoR8:
		if( Is64Bit() )
			StoreVR64(Load64(a0));
		else
			m_uc.v_storeu64_f64(VRMem(), LoadF64(a0));
		break;

	case asBC_CpyRtoV4:
		{
			Gp t = m_uc.new_gp32();
			LoadVR32(t);
			Commit32(a0, t);
		}
		break;

	case asBC_CpyRtoV8:
		if( Is64Bit() )
		{
			Gp t = m_uc.new_gp64();
			LoadVR64(t);
			Commit64(a0, t);
		}
		else
		{
			Vec v = m_uc.new_vec128_f64x1();
			m_uc.v_loadu64_f64(v, VRMem());
			CommitF64(a0, v);
		}
		break;

	case asBC_CpyVtoG4:
		{
			Gp g;
			Mem dst = Global(asBC_PTRARG(bc), g);
			m_uc.store_u32(dst, Load32(a0));
		}
		break;

	case asBC_CpyGtoV4:
		{
			Gp g;
			Mem src = Global(asBC_PTRARG(bc), g);
			Gp t = m_uc.new_gp32();
			m_uc.load_u32(t, src);
			Commit32(a0, t);
		}
		break;

	case asBC_SetG4:
		{
			Gp g;
			Mem dst = Global(asBC_PTRARG(bc), g);
			StoreImm32(dst, int(asBC_DWORDARG(bc + AS_PTR_SIZE)));
		}
		break;

	case asBC_WRTV1:
	case asBC_WRTV2:
	case asBC_WRTV4:
		{
			// Floats kept in vector registers are stored from there
			Mem dst = VRAddr();
			SCachedSlot *c = FindCached(a0);
			if( instr.op == asBC_WRTV4 && c && c->kind == JIT_SLOT_F32 )
				m_uc.v_storeu32_f32(dst, c->vec);
			else if( instr.op == asBC_WRTV1 )
				m_uc.store_u8(dst, Load32(a0));
			else if( instr.op == asBC_WRTV2 )
				m_uc.store_u16(dst, Load32(a0));
			else
				m_uc.store_u32(dst, Load32(a0));
		}
		break;

	case asBC_WRTV8:
		{
			Mem dst = VRAddr();
			SCachedSlot *c = FindCached(a0);
			if( Is64Bit() && !(c && c->kind == JIT_SLOT_F64) )
				m_uc.store_u64(dst, Load64(a0));
			else
				m_uc.v_storeu64_f64(dst, LoadF64(a0));
		}
		break;

	case asBC_RDR1:
	case asBC_RDR2:
	case asBC_RDR4:
		{
			Mem src = VRAddr();
			SCachedSlot *c = FindCached(a0);
			if( instr.op == asBC_RDR4 && c && c->kind == JIT_SLOT_F32 )
				m_uc.v_loadu32_f32(c->vec, src);
			else
			{
				Gp t = Dst32(a0);
				if( instr.op == asBC_RDR1 )
					m_uc.load_u8(t, src);
				else if( instr.op == asBC_RDR2 )
					m_uc.load_u16(t, src);
				else
					m_uc.load_u32(t, src);
				Commit32(a0, t);
			}
		}
		break;

	case asBC_RDR8:
		{
			Mem src = VRAddr();
			SCachedSlot *c = FindCached(a0);
			if( Is64Bit() && !(c && c->kind == JIT_SLOT_F64) )
			{
				Gp t = Dst64(a0);
				m_uc.load_u64(t, src);
				Commit64(a0, t);
			}
			else
			{
				Vec v = DstF64(a0);
				m_uc.v_loadu64_f64(v, src);
				CommitF64(a0, v);
			}
		}
		break;

	case asBC_LDG:
		if( CanFoldVRAddr(idx) )
		{
			Gp g;
			SetVRAddr(idx, Global(asBC_PTRARG(bc), g));
		}
		else
			StoreVRPtr(PtrConst(asBC_PTRARG(bc)));
		break;

	case asBC_LDV:
		SetVRAddr(idx, Var(a0));
		break;

	case asBC_ChkRefS:
		{
			Gp a = m_uc.new_gp_ptr();
			m_uc.load(a, Stack(0));
			m_uc.load(a, mem_ptr(a));
			m_uc.j(BailLabel(idx), test_z(a));
		}
		break;

	case asBC_ChkNullV:
		{
			Gp a = LoadPtr(a0);
			m_uc.j(BailLabel(idx), test_z(a));
		}
		break;

	case asBC_ChkNullS:
		{
			Gp a = m_uc.new_gp_ptr();
			m_uc.load(a, Stack(asBC_WORDARG0(bc)));
			m_uc.j(BailLabel(idx), test_z(a));
		}
		break;

	case asBC_CHKREF:
		{
			Gp a = m_uc.new_gp_ptr();
			m_uc.load(a, Stack(0));
			m_uc.j(BailLabel(idx), test_z(a));
		}
		break;

	case asBC_LoadThisR:
		{
			Gp t = m_uc.new_gp_ptr();
			m_uc.load(t, Var(0));
			m_uc.j(BailLabel(idx), test_z(t));
			SetVRAddr(idx, mem_ptr(t, asBC_SWORDARG0(bc)));
		}
		break;

	case asBC_LoadRObjR:
		{
			Gp t = LoadPtr(a0);
			m_uc.j(BailLabel(idx), test_z(t));
			SetVRAddr(idx, mem_ptr(t, asBC_SWORDARG1(bc)));
		}
		break;

	case asBC_LoadVObjR:
		SetVRAddr(idx, Var(a0, asBC_SWORDARG1(bc)));
		break;

	case asBC_AllocMem:
		{
			InvokeNode *call = Invoke((const void*)JIT_AllocMem, FuncSignature::build<void*, asUINT>());
			Gp mem = m_uc.new_gp_ptr();
			call->set_arg(0, Imm(int(asBC_DWORDARG(bc))));
			call->set_ret(0, mem);
			StorePtr(a0, mem);
		}
		break;

	case asBC_SetListSize:
	case asBC_SetListType:
		{
			Gp var = LoadPtr(a0);
			Gp t = m_uc.new_gp32();
			m_uc.mov(t, Imm(int(asBC_DWORDARG(bc + 1))));
			m_uc.store_u32(mem_ptr(var, int(asBC_DWORDARG(bc))), t);
		}
		break;

	default:
		return false;
	}

	return true;
}

//------------------------------------------------------------------------
// Branches

bool CJITCodeGen::EmitBranch(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];

	if( instr.op == asBC_JMP )
	{
		m_uc.j(InstrLabel(instr.target));
		return true;
	}

	if( instr.op == asBC_JMPP )
	{
		// Switch. The index is compared against the cases with a binary search.
		// Out of range values return to the VM, which will misbehave the same
		// way it would have without the JIT
		//
		// TODO: runtime optimize: Large switches would be faster with a jump table
		//                         embedded in the code (embed_label for the entries,
		//                         and an indirect jump emitted with the arch specific
		//                         compiler in jit_codegen_arch.cpp). The register
		//                         allocator must be told about the successors, i.e.
		//                         the jump needs a JumpAnnotation (new_jump_annotation
		//                         and add_label) listing the case labels
		Gp v = Load32(asBC_SWORDARG0(instr.bc));
		const std::vector<int> &targets = m_code->GetSwitchTargets(idx);

		struct SRange
		{
			asUINT lo, hi;
			Label  label;
		};
		std::vector<SRange> work;
		SRange all = { 0, asUINT(targets.size()), Label() };
		work.push_back(all);

		while( !work.empty() )
		{
			SRange r = work.back();
			work.pop_back();
			if( r.label.is_valid() )
				m_uc.bind(r.label);

			if( r.hi - r.lo <= 4 )
			{
				for( asUINT k = r.lo; k < r.hi; k++ )
					m_uc.j(InstrLabel(targets[k]), cmp_eq(v, Imm(int(k))));
				Bail(idx);
			}
			else
			{
				asUINT mid = (r.lo + r.hi) / 2;
				SRange upper = { mid, r.hi, m_uc.new_label() };
				SRange lower = { r.lo, mid, Label() };
				m_uc.j(upper.label, ucmp_ge(v, Imm(int(mid))));
				// Emit the lower half next (falls through), the upper half after
				work.push_back(upper);
				work.push_back(lower);
			}
		}
		return true;
	}

	Label target = InstrLabel(instr.target);
	Gp t = m_uc.new_gp32();
	LoadVR32(t);

	switch( instr.op )
	{
	case asBC_JZ:     m_uc.j(target, test_z(t)); break;
	case asBC_JNZ:    m_uc.j(target, test_nz(t)); break;
	case asBC_JS:     m_uc.j(target, scmp_lt(t, Imm(0))); break;
	case asBC_JNS:    m_uc.j(target, scmp_ge(t, Imm(0))); break;
	case asBC_JP:     m_uc.j(target, scmp_gt(t, Imm(0))); break;
	case asBC_JNP:    m_uc.j(target, scmp_le(t, Imm(0))); break;
	case asBC_JLowZ:  m_uc.j(target, test_z(t, Imm(0xFF))); break;
	case asBC_JLowNZ: m_uc.j(target, test_nz(t, Imm(0xFF))); break;
	default:
		return false;
	}

	return true;
}

//------------------------------------------------------------------------
// Misc

bool CJITCodeGen::EmitMisc(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];

	switch( instr.op )
	{
	case asBC_JitEntry:
		// Entry stubs jump to the label bound before this instruction
		break;

	case asBC_SUSPEND:
		if( !m_options.noSuspend && !(instr.flags & JIT_INSTR_SKIP) )
		{
			// Only when the VM asks for it, i.e. a line callback is set or a
			// suspension was requested, is the helper called
			Gp t = m_uc.new_gp32();
			m_uc.load_u8(t, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));

			// The VM calls the line callback for an inlined function
			if( m_frame != 0 )
			{
				m_uc.j(BailLabel(idx), test_nz(t));
				break;
			}

			Label suspend = m_uc.new_label();
			Label cont = m_uc.new_label();
			m_uc.j(suspend, test_nz(t));

			BaseNode *cold = BeginCold(suspend);
			SyncAll(idx);
			InvokeNode *call = Invoke((const void*)JIT_Suspend, FuncSignature::build<int, asSVMRegisters*>());
			Gp r = m_uc.new_gp32();
			call->set_arg(0, m_regs);
			call->set_ret(0, r);
			EmitLeaveIf(r);

			// The line callback may have modified variables
			ReloadAll(idx);
			EndCold(cold, cont);
			m_uc.bind(cont);
		}
		break;

	case asBC_STR:
		// Deprecated instruction, never generated by the compiler
		return false;

	default:
		return false;
	}

	return true;
}

END_AS_NAMESPACE
