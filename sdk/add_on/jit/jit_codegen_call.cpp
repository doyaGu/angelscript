#include "jit_codegen.h"
#include "jit_runtime.h"

// The engine internals are only inspected at compile time, to know the object
// type flags and the calling conventions of the registered functions
#include "as_objecttype.h"
#include "as_scriptengine.h"
#include "as_scriptfunction.h"
#include "as_callfunc.h"

#include <stddef.h>

BEGIN_AS_NAMESPACE

using namespace asmjit;
using namespace asmjit::ujit;

static const int PTR_BYTES = AS_PTR_SIZE * 4;

// The nth pointer from the address, e.g. of a call state on the call stack
static Mem PtrAt(const Mem &mem, int n)
{
	Mem m = mem;
	m.add_offset(n * PTR_BYTES);
	return m;
}

// The compilers for 32bit x86 that pass the object pointer of class methods in ECX
// also let the called function pop the arguments, i.e. use the thiscall convention.
// These are MSVC and MinGW since version 4.7. The others pass it on the stack like
// the first argument of a cdecl function
#if defined(AS_X86) && !defined(THISCALL_PASS_OBJECT_POINTER_ON_THE_STACK)
#define JIT_X86_THISCALL
#endif

// Where the hidden pointer for a value returned in memory is passed, when the ABI
// allows the direct calls to pass it like an ordinary argument. It is the first
// argument, except that MSVC passes it after the object pointer of class methods.
// Other compilers for 32bit x86 let the called function pop it off the stack,
// which the generated calls can't express, and AArch64 passes it in a register
// that isn't used for arguments
#if defined(AS_X64_MSVC) || defined(AS_X64_GCC) || defined(AS_X64_MINGW) || (defined(AS_X86) && defined(_MSC_VER))
#define JIT_HIDDEN_RETURN_POINTER
#if defined(_MSC_VER)
#define JIT_HIDDEN_RETURN_POINTER_AFTER_THIS
#endif
#endif

// After a helper that may hand control back to the VM: leave if requested,
// otherwise pick up the registers the helper may have changed
void CJITCodeGen::EmitAfterHelperCall(const Gp &result, asUINT idx)
{
	EmitLeaveIf(result);
	EmitDematerialize();
	ReloadStackAfter(idx);
	EmitReloadAfterCall(idx);
}

// After a call that has completed the instruction: picks up the value register if
// it is read later, and the variables if a debugger may have modified them
void CJITCodeGen::EmitReloadAfterCall(asUINT idx, bool reloadVR)
{
	if( reloadVR && m_code->IsVRLiveAfter(idx) )
		ReloadVR();

	// With a debugger attached the variables may have been modified through the
	// context, also those of the callers of an inlined function
	bool reload = !m_cached.empty() && (m_options.syncEveryInstr || m_code->GetReloadMask(idx));
	for( int f = m_frame; f != 0 && !reload; f = m_frames[f].caller )
		reload = m_frames[m_frames[f].caller].code->GetReloadMask(m_frames[f].callIdx) != 0;
	if( reload )
	{
		Gp t = m_uc.new_gp32();
		m_uc.load_u8(t, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
		Label reload = m_uc.new_label();
		Label cont = m_uc.new_label();
		m_uc.j(reload, test_nz(t));
		BaseNode *cold = BeginCold(reload);
		ReloadLiveSlots(idx);
		EndCold(cold, cont);
		m_uc.bind(cont);
	}
}

// Script function call through the runtime helper, which also executes the
// called function natively when possible. Calls of script functions, methods,
// function pointers, and the constructors of script classes push the call state
// inline and call the compiled function directly instead, unless it hasn't been
// compiled or the call stack has reached the call limit
void CJITCodeGen::EmitScriptCall(asUINT idx, int kind, int funcId, const Gp *extra, asPWORD extraImm)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	asCScriptFunction *func = m_code->GetFunction();
	asCScriptFunction *callee = 0;
	if( funcId >= 0 && asUINT(funcId) < func->engine->scriptFunctions.GetLength() )
		callee = func->engine->scriptFunctions[funcId];

	bool native = false;
	if( kind == JIT_CALL_SCRIPT || kind == JIT_CALL_ALLOC )
		native = callee && callee->funcType == asFUNC_SCRIPT && callee->scriptData;
	else if( kind == JIT_CALL_INTERFACE )
		native = callee && (callee->funcType == asFUNC_VIRTUAL || callee->funcType == asFUNC_INTERFACE);
	else if( kind == JIT_CALL_PTR )
		native = true;

	// The object of a script class is allocated first, with everything synced like
	// the VM does, as the allocation may reuse the context for nested calls. Then
	// it is stored in the variable, and pushed for the constructor
	bool synced = false;
	if( native && kind == JIT_CALL_ALLOC )
	{
		SyncForCall(idx);
		InvokeNode *alloc = Invoke((const void*)JIT_NewScriptObject, FuncSignature::build<void*, void*>());
		Gp obj = m_uc.new_gp_ptr();
		alloc->set_arg(0, Imm(int64_t(extraImm)));
		alloc->set_ret(0, obj);

		Gp var = m_uc.new_gp_ptr();
		Label noVar = m_uc.new_label();
		m_uc.load(var, Stack(int(callee->GetSpaceNeededForArguments())));
		m_uc.j(noVar, test_z(var));
		m_uc.store(mem_ptr(var), obj);
		m_uc.bind(noVar);
		PushStack(PTR_BYTES);
		m_uc.store(Stack(0), obj);
		kind = JIT_CALL_CONSTRUCT;
		synced = true;
	}

	// The native calls push the frame and the stack pointer on the call stack and
	// pass the stack pointer, only the helper needs them in the VM registers.
	// Arguments passed on the stack cost a store too though. The frames of the
	// inlined calls are pushed for both
	if( !synced )
		StoreDirtySlots(m_code->GetDirtyMask(idx) & ~JIT_FRAME_BIT);
	int spOffset = m_spOffset;
	if( !m_spInArg )
		SyncStack();
	EmitMaterialize();

	Gp r = m_uc.new_gp32();
	Label slow = m_uc.new_label();
	Label done = m_uc.new_label();
	BaseNode *cold = 0;
	bool vrReturned = false;
	if( native )
	{
		const SJITObjectLayout &layout = JIT_GetObjectLayout();

		// A recursive call enters this code, which exists as it is being executed. The
		// called function itself is only needed in interop mode
		Gp target;
		Gp method;
		if( (kind == JIT_CALL_SCRIPT || kind == JIT_CALL_CONSTRUCT) && callee != m_frames[0].code->GetFunction() )
		{
			target = m_uc.new_gp_ptr();
			m_uc.load(target, mem_ptr(PtrConst(asPWORD(&callee->scriptData->jitFunction))));
		}
		else if( kind == JIT_CALL_INTERFACE )
		{
			method = EmitFindMethod(callee, slow, (instr.flags & JIT_INSTR_INLINE) ? 0 : ProfileCell(idx));
			target = m_uc.new_gp_ptr();
			m_uc.load(target, Addr(method, layout.scriptData));
			m_uc.load(target, Addr(target, layout.jitFunction));
		}
		else if( kind == JIT_CALL_PTR )
		{
			// Everything but script functions is left to the helper
			Gp type = m_uc.new_gp32();
			m_uc.j(slow, test_z(*extra));
			m_uc.load_u32(type, Addr(*extra, layout.funcType));
			m_uc.j(slow, cmp_ne(type, Imm(int(asFUNC_SCRIPT))));
			target = m_uc.new_gp_ptr();
			m_uc.load(target, Addr(*extra, layout.scriptData));
			m_uc.load(target, Addr(target, layout.jitFunction));
			method = *extra;
		}
		if( target.is_valid() )
		{
			m_uc.j(slow, test_z(target));
			// The code of the functions whose compilation is deferred finds them as the
			// current function, which the helper sets
			if( m_options.tieredEntry && !m_options.interop )
				m_uc.j(slow, cmp_eq(target, PtrConst(asPWORD(m_options.tieredEntry))));
		}
		if( m_options.interop && !method.is_valid() )
			method = PtrConst(asPWORD(callee));

		// The called function has popped the arguments. Unless the size isn't known
		// the stack pointer is adjusted instead of waiting for the one it stored,
		// which the call state must not be marked for then. A static stack pointer
		// is known after any call
		bool reload = !m_staticStack && (kind == JIT_CALL_PTR || callee->IsVariadic());
		bool vrInReg = !callee || kind == JIT_CALL_PTR || CJITByteCode::ReturnsInVR(callee);
		vrReturned = EmitNativeCall(idx, target, method, r, slow, !reload, vrInReg);
		EmitLeaveIf(r);
		if( reload || m_staticStack )
			ReloadStackAfter(idx);
		else
			PopStack(CJITByteCode::GetPopSize(callee) * 4);

		// The call through the helper is the rare path then
		cold = BeginCold(slow);
	}

	m_spOffset = spOffset;
	if( m_spInArg )
		SyncStack();
	if( !synced )
	{
		if( m_code->GetDirtyMask(idx) & JIT_FRAME_BIT )
			StoreFrame();
		SetPC(instr.pos);
	}
	InvokeNode *call = Invoke((const void*)JIT_CallScript, FuncSignature::build<int, asSVMRegisters*, int, int, asPWORD, asUINT>());
	call->set_arg(0, m_regs);
	call->set_arg(1, Imm(kind));
	call->set_arg(2, Imm(funcId));
	if( extra )
		call->set_arg(3, *extra);
	else
		call->set_arg(3, Imm(int64_t(extraImm)));
	call->set_arg(4, m_callLimit);
	call->set_ret(0, r);
	EmitLeaveIf(r);
	ReloadStackAfter(idx);
	if( vrReturned )
		ReloadVR();
	if( cold )
		EndCold(cold, done);

	m_uc.bind(done);
	EmitDematerialize();
	EmitReloadAfterCall(idx, !vrReturned);
}

// Emits the code of the called function in place, in a frame of its own that starts
// at the stack pointer, see CJITByteCode::FindInlinees. It executes like after
// asCContext::CallScriptFunction when the call state can be pushed without growing
// the call stack, the stack block has room for the function, and the VM has nothing
// to do. The function is called otherwise. Where it must return to the VM, the exit
// of its frame pushes the call state, see EmitInlineExit, and where it calls
// functions, its frame is materialized, see EmitMaterialize. A method called through
// asBC_CALLINTF is inlined for objects of one class, or of the classes that inherit
// the method, and the others call the method. If the class is the one seen before by
// the call, the others are noted in the profile, and count down its calls.
// The calls in the inlined functions only check the class, as the outermost call
// has checked the room for them, and that the VM has nothing to do if the functions
// called before may have given it something. The VM makes the call otherwise
void CJITCodeGen::EmitInlineCall(asUINT idx)
{
	const CJITByteCode *code = m_code->GetInlinee(idx);
	asCScriptFunction *func = code->GetFunction();
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	bool virtualCall = instr.op == asBC_CALLINTF;
	asCObjectType *objType = m_code->GetInlineObjectType(idx);
	int base = -StackOffset(idx) / 4;
	int caller = m_frame;
	asUINT borrowed = m_code->GetBorrowedArgs(idx);
	bool checkVM = caller == 0 || m_code->HasSyncPoints(m_frames[caller].borrowed);
	if( m_options.elideSuspend && m_suspendChecked )
		checkVM = false;

	Label call;
	if( caller == 0 )
		call = m_uc.new_label();
	else if( virtualCall || checkVM )
		call = BailLabel(idx);

	// The function called instead gets its own references for the arguments that
	// borrow them, see CJITByteCode::AnalyseBorrows
	bool ownArgs = borrowed && call.is_valid();
	Label own = ownArgs ? m_uc.new_label() : call;
	if( virtualCall )
	{
		// The call raises the exception for a null object
		const SJITObjectLayout &layout = JIT_GetObjectLayout();
		Gp type = m_uc.new_gp_ptr();
		m_uc.load(type, Stack(0));
		m_uc.j(own, test_z(type));
		m_uc.load(type, Addr(type, layout.objectType));
		asCObjectType **seen = objType ? ProfileCell(idx) : 0;
		if( seen )
		{
			Label other = m_uc.new_label();
			m_uc.j(other, cmp_ne(type, PtrConst(asPWORD(objType))));
			BaseNode *cold = BeginCold(other);
			Gp many = m_uc.new_gp_ptr();
			m_uc.mov(many, Imm(int64_t(asPWORD(JIT_PROFILE_MANY))));
			m_uc.store(mem_ptr(PtrConst(asPWORD(seen))), many);
			m_uc.j(own, scmp_gt(EmitCountDown(), Imm(0)));
			EmitRecompile(idx);
			EndCold(cold, own);
			m_callsProfiled++;
		}
		else if( objType )
			m_uc.j(own, cmp_ne(type, PtrConst(asPWORD(objType))));
		else
		{
			// The classes that inherit the method have it in the same place of their
			// tables as the class declaring it
			asCScriptFunction *method = func->engine->scriptFunctions[asBC_INTARG(instr.bc)];
			m_uc.load(type, Addr(type, layout.virtualFunctionTable));
			m_uc.load(type, Addr(type, method->vfTableIdx * PTR_BYTES));
			m_uc.j(own, cmp_ne(type, PtrConst(asPWORD(func))));
		}
	}
	if( checkVM )
	{
		// Like the line callback on entry of script functions
		Gp flag = m_uc.new_gp32();
		m_uc.load_u8(flag, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
		m_uc.j(own, test_nz(flag));
	}
	if( caller == 0 )
	{
		// The exits push a call state for each level of inlined calls
		const SJITContextLayout &layout = JIT_GetContextLayout();
		int extent, depth;
		GetInlineRoom(idx, extent, depth);
		Imm words = Imm(depth * int(layout.callStackFrameSize));
		if( m_inlineRoom.is_valid() )
			m_uc.j(own, ucmp_lt(m_inlineRoom, words));
		else
		{
			Gp length = m_uc.new_gp32();
			Gp room   = m_uc.new_gp32();
			m_uc.load_u32(length, ContextField(layout.callStackLength));
			m_uc.load_u32(room, ContextField(layout.callStackCapacity));
			m_uc.sub(room, room, length);
			m_uc.j(own, ucmp_lt(room, words));
			EmitStackBlockCheck(extent, own);
		}
	}
	if( ownArgs )
	{
		BaseNode *cold = BeginCold(own);
		Gp frame = FramePointer(base);
		InvokeNode *ownCall = Invoke((const void*)JIT_OwnParams, FuncSignature::build<void, asCScriptFunction*, asDWORD*, asUINT>());
		ownCall->set_arg(0, Imm(int64_t(asPWORD(func))));
		ownCall->set_arg(1, frame);
		ownCall->set_arg(2, Imm(int(borrowed)));
		EndCold(cold, call);
	}

	SFrame frame;
	frame.code     = code;
	frame.base     = base;
	frame.caller   = caller;
	frame.callIdx  = idx;
	frame.ret      = m_uc.new_label();
	frame.exit     = m_uc.new_label();
	frame.exitUsed = false;
	frame.borrowed = borrowed;
	frame.retChecked = true;
	m_frames.push_back(frame);
	int inlined = int(m_frames.size()) - 1;
	SwitchFrame(inlined);
	CreateCachedSlots();
	CreateBlockLabels();

	// Only the object variables on the heap are cleared, like in EmitDirectEntry
	const asCArray<asSScriptVariable*> &vars = func->scriptData->variables;
	for( asUINT n = 0; n < vars.GetLength(); n++ )
		if( vars[n]->stackOffset > 0 && vars[n]->onHeap && (vars[n]->type.IsObject() || vars[n]->type.IsFuncdef()) )
			m_uc.store_zero_reg(Var(vars[n]->stackOffset));
	ReloadSlots(code->GetLiveInMask(0));

	// The function starts where the suspend requests are checked, if they may have
	// been made, see EmitBody
	if( checkVM )
		m_suspendChecked = true;
	std::vector<bool> calls(code->GetInstructions().size());
	EmitBody(calls);
	m_uc.bind(m_frames[inlined].ret);
	SwitchFrame(caller);
	bool retChecked = m_frames[inlined].retChecked;
	m_inlineCalls = false;
	for( asUINT n = 0; n < calls.size(); n++ )
		m_inlineCalls = m_inlineCalls || calls[n];
	m_callsInlined++;

	if( caller == 0 )
	{
		Label cont = m_uc.new_label();
		BaseNode *cold = BeginCold(call);
		m_spOffset = StackOffset(idx);
		if( virtualCall )
			EmitScriptCall(idx, JIT_CALL_INTERFACE, asBC_INTARG(instr.bc), 0, 0);
		else
			EmitScriptCall(idx, JIT_CALL_SCRIPT, func->GetId(), 0, 0);

		// The VM goes on after the call if the function has set a line callback, so
		// that both paths continue where the suspend requests are checked
		if( m_options.elideSuspend )
		{
			Gp flag = m_uc.new_gp32();
			m_uc.load_u8(flag, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
			m_uc.j(BailLabel(idx + 1), test_nz(flag));
		}
		EndCold(cold, cont);
		m_uc.bind(cont);
	}
	m_suspendChecked = retChecked;
	m_spOffset = StackOffset(idx + 1);
}

// Notes the room on the call stack for the inlined functions, or none if the stack
// block doesn't have room for them. That doesn't shrink while the function runs, as
// the frame and the call stack length are the same whenever it runs, and the capacity
// of the call stack only grows, so the functions that call them in loops check it on
// entry
void CJITCodeGen::EmitInlineRoom()
{
	const SJITContextLayout &layout = JIT_GetContextLayout();
	Label none = m_uc.new_label();
	Gp length = m_uc.new_gp32();
	m_uc.mov(m_inlineRoom, Imm(0));
	EmitStackBlockCheck(m_inlineExtent, none);
	m_uc.load_u32(length, ContextField(layout.callStackLength));
	m_uc.load_u32(m_inlineRoom, ContextField(layout.callStackCapacity));
	m_uc.sub(m_inlineRoom, m_inlineRoom, length);
	m_uc.bind(none);
}

// Jumps to none unless the stack block has room for the variables down to the extent
// in dwords below the frame pointer, like asCContext::CallScriptFunction
void CJITCodeGen::EmitStackBlockCheck(int extent, const Label &none)
{
	const SJITContextLayout &layout = JIT_GetContextLayout();
	Gp blocks = m_uc.new_gp_ptr();
	Gp index  = m_uc.new_gp_ptr();
	Gp limit  = m_uc.new_gp_ptr();
	m_uc.load(blocks, ContextField(layout.stackBlocks));
	m_uc.load_u32(index, ContextField(layout.stackIndex));
	Lea(limit, mem_ptr(m_fp, -(extent + int(layout.reserveStack)) * 4));
	m_uc.j(none, ucmp_lt(limit, PtrElement(blocks, index)));
}

// Adds the frame of an inlined function, which starts at the base in dwords below
// the frame pointer, and the frames of the functions inlined into it at the level
// below, see GetInlineRoom
static void AddInlineRoom(const CJITByteCode *code, int base, int level, int &extent, int &depth)
{
	asCScriptFunction *func = code->GetFunction();
	if( base + int(func->scriptData->stackNeeded) > extent )
		extent = base + int(func->scriptData->stackNeeded);
	if( level > depth )
		depth = level;
	const std::vector<SJITInstr> &instrs = code->GetInstructions();
	for( asUINT n = 0; n < instrs.size(); n++ )
		if( instrs[n].flags & JIT_INSTR_INLINE )
			AddInlineRoom(code->GetInlinee(n), base + int(func->scriptData->variableSpace) + code->GetStackDepth(n), level + 1, extent, depth);
}

// The dwords below the frame pointer that the frames of the function inlined by the
// instruction and of the functions inlined into it reach, and the most call states
// that their exits push
void CJITCodeGen::GetInlineRoom(asUINT idx, int &extent, int &depth) const
{
	extent = 0;
	depth  = 0;
	AddInlineRoom(m_code->GetInlinee(idx), -StackOffset(idx) / 4, 1, extent, depth);
}

// Hands an inlined function to the VM at the program pointer in m_bailPC, with the
// cached variables, the value register, and the stack pointer of the function stored
// by Bail. The callers are stored like for the calls and their call states are
// pushed, the outermost first
void CJITCodeGen::EmitInlineExit(int frame)
{
	m_uc.bind(m_frames[frame].exit);
	std::vector<int> inlined;
	for( int f = frame; f != 0; f = m_frames[f].caller )
	{
		SwitchFrame(m_frames[f].caller);
		StoreDirtySlots(m_code->GetDirtyMask(m_frames[f].callIdx) & ~JIT_FRAME_BIT);
		inlined.insert(inlined.begin(), f);
	}
	StoreFrame();
	m_uc.store(RegsField(offsetof(asSVMRegisters, programPointer)), m_bailPC);

	for( asUINT n = 0; n < inlined.size(); n++ )
	{
		const SFrame &callee = m_frames[inlined[n]];
		const CJITByteCode *code = m_frames[callee.caller].code;
		const SJITInstr &instr = code->GetInstructions()[callee.callIdx];
		Gp fp = m_uc.new_gp_ptr();
		Lea(fp, mem_ptr(m_fp, -callee.base * 4));
		InvokeNode *call = Invoke((const void*)JIT_ExitInlined, FuncSignature::build<void, asSVMRegisters*, asCScriptFunction*, asDWORD*, asDWORD*, asUINT>());
		call->set_arg(0, m_regs);
		call->set_arg(1, Imm(int64_t(asPWORD(callee.code->GetFunction()))));
		call->set_arg(2, fp);
		call->set_arg(3, Imm(int64_t(asPWORD(code->GetByteCode() + instr.pos + instr.size))));
		call->set_arg(4, Imm(int(callee.borrowed)));
	}
	Leave();
}

// Hands the frames of the inlined calls to the VM before the inlined function
// being emitted calls something that may see them, e.g. a registered function that
// raises an exception or inspects the call stack, or a script function. The callers
// are stored like for the calls, and their call states are pushed like the exits
// do, the outermost first. The VM registers are set to the frame of the function,
// which the program pointer and the stack pointer must be set for too. If the VM
// takes over, it continues in the function and returns to the callers, otherwise
// EmitDematerialize pops the call states after the call, or after the last of the
// calls that share the materialization, see EmitBody
void CJITCodeGen::EmitMaterialize()
{
	if( m_frame == 0 || m_materialized )
		return;
	int frame = m_frame;
	std::vector<int> inlined;
	for( int f = frame; f != 0; f = m_frames[f].caller )
	{
		SwitchFrame(m_frames[f].caller);
		StoreDirtySlots(m_code->GetDirtyMask(m_frames[f].callIdx) & ~JIT_FRAME_BIT);
		inlined.insert(inlined.begin(), f);
	}
	SwitchFrame(frame);

	// The capacity of the call stack has been checked for this where the outermost
	// function was inlined
	const SJITContextLayout &layout = JIT_GetContextLayout();
	Gp length = m_uc.new_gp_ptr();
	Gp array  = m_uc.new_gp_ptr();
	Gp index  = m_uc.new_gp_ptr();
	m_uc.load_u32(length, ContextField(layout.callStackLength));
	m_uc.load(array, ContextField(layout.callStackArray));
	m_uc.load_u32(index, ContextField(layout.stackIndex));
	Mem states = PtrElement(array, length);
	bool borrowed = false;
	for( asUINT n = 0; n < inlined.size(); n++ )
	{
		const SFrame &callee = m_frames[inlined[n]];
		const SFrame &caller = m_frames[callee.caller];
		const SJITInstr &instr = caller.code->GetInstructions()[callee.callIdx];
		Mem state = PtrAt(states, int(n * layout.callStackFrameSize));
		m_uc.store(state, FramePointer(caller.base));
		m_uc.store(PtrAt(state, 1), PtrConst(asPWORD(caller.code->GetFunction())));
		m_uc.store(PtrAt(state, 2), PtrConst(asPWORD(caller.code->GetByteCode() + instr.pos + instr.size)));
		m_uc.store(PtrAt(state, 3), FramePointer(callee.base));
		m_uc.store(PtrAt(state, 4), index);
		if( callee.borrowed )
		{
			// The upper half of the stack index notes the borrowed parameters, see
			// JIT_OwnBorrowed
			Mem mask = PtrAt(state, 4);
			mask.add_offset(4);
			StoreImm32(mask, int(callee.borrowed));
			borrowed = true;
		}
	}
	m_uc.add(length, length, Imm(int(inlined.size() * layout.callStackFrameSize)));
	m_uc.store_u32(ContextField(layout.callStackLength), length);
	m_uc.store(RegsField(offsetof(asSVMRegisters, stackFramePointer)), FramePointer(m_frameBase));
	m_uc.store(ContextField(layout.currentFunction), PtrConst(asPWORD(m_code->GetFunction())));
	m_materialized  = true;
	m_materialDepth = int(inlined.size());
	m_materialBorrowed = borrowed;
}

// Pops the call states pushed by EmitMaterialize once the call has returned, which
// has restored the call stack if it pushed anything. The VM registers are left with
// the frame of the inlined function, see CJITByteCode::LeavesFrameDirty
void CJITCodeGen::EmitDematerialize()
{
	if( !m_materialized || m_shareMaterial )
		return;
	const SJITContextLayout &layout = JIT_GetContextLayout();
	Gp length = m_uc.new_gp32();
	m_uc.load_u32(length, ContextField(layout.callStackLength));
	m_uc.sub(length, length, Imm(m_materialDepth * int(layout.callStackFrameSize)));
	m_uc.store_u32(ContextField(layout.callStackLength), length);
	m_materialized = false;
	m_materialBorrowed = false;
}

// The frame pointer of the frame at the base, see SFrame
CJITCodeGen::Gp CJITCodeGen::FramePointer(int base)
{
	if( base == 0 )
		return m_fp;
	Gp fp = m_uc.new_gp_ptr();
	Lea(fp, mem_ptr(m_fp, -base * 4));
	return fp;
}

// Finds the implementation of a virtual or interface method for the object on the
// stack, like asCContext::CallInterfaceMethod. Jumps to slow if there is no object
// or it doesn't implement the interface, for the VM to raise the exception. The class
// of the object is noted in seen unless it is null, see SJITProfile
CJITCodeGen::Gp CJITCodeGen::EmitFindMethod(asCScriptFunction *method, const Label &slow, asCObjectType **seen)
{
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	const uint32_t ptrShift = Is64Bit() ? 3 : 2;
	Gp type = m_uc.new_gp_ptr();
	m_uc.load(type, Stack(0));
	m_uc.j(slow, test_z(type));
	m_uc.load(type, Addr(type, layout.objectType));
	if( seen )
	{
		// The first class is noted, and JIT_PROFILE_MANY once another one comes.
		// Another thread may have noted the same class since the compare
		Gp cell = PtrConst(asPWORD(seen));
		Gp noted = m_uc.new_gp_ptr();
		Label other = m_uc.new_label();
		Label first = m_uc.new_label();
		Label cont  = m_uc.new_label();
		m_uc.j(other, cmp_ne(type, mem_ptr(cell)));
		BaseNode *cold = BeginCold(other);
		m_uc.load(noted, mem_ptr(cell));
		m_uc.j(first, test_z(noted));
		m_uc.j(cont, cmp_eq(noted, type));
		m_uc.mov(noted, Imm(int64_t(asPWORD(JIT_PROFILE_MANY))));
		m_uc.store(mem_ptr(cell), noted);
		m_uc.j(cont);
		m_uc.bind(first);
		m_uc.store(mem_ptr(cell), type);
		EndCold(cold, cont);
		m_uc.bind(cont);
	}
	Gp table = m_uc.new_gp_ptr();
	m_uc.load(table, Addr(type, layout.virtualFunctionTable));
	Gp found = m_uc.new_gp_ptr();
	if( method->funcType == asFUNC_VIRTUAL )
	{
		m_uc.load(found, Addr(table, method->vfTableIdx * PTR_BYTES));
		return found;
	}

	// The methods of each interface are at an offset in the table
	Gp list  = m_uc.new_gp_ptr();
	Gp count = m_uc.new_gp_ptr();
	Gp n     = m_uc.new_gp_ptr();
	Gp intf  = PtrConst(asPWORD(method->objectType));
	Label loop  = m_uc.new_label();
	Label match = m_uc.new_label();
	m_uc.load(list, Addr(type, layout.interfaces));
	m_uc.load_u32(count, Addr(type, layout.interfaceCount));
	m_uc.mov(n, Imm(0));
	m_uc.bind(loop);
	m_uc.j(slow, ucmp_ge(n, count));
	m_uc.load(found, mem_ptr(list, n, ptrShift));
	m_uc.j(match, cmp_eq(found, intf));
	m_uc.add(n, n, Imm(1));
	m_uc.j(loop);
	m_uc.bind(match);
	m_uc.load(list, Addr(type, layout.interfaceVFTOffsets));
	m_uc.load_u32(n, mem_ptr(list, n, 2));
	m_uc.add(n, n, Imm(method->vfTableIdx));
	m_uc.load(found, mem_ptr(table, n, ptrShift));
	return found;
}

// Returns where the call notes the classes that it sees, if it is marked with
// JIT_INSTR_PROFILE and the code has a profile, else null
asCObjectType **CJITCodeGen::ProfileCell(asUINT idx)
{
	if( m_options.profile == 0 || !(m_code->GetInstructions()[idx].flags & JIT_INSTR_PROFILE) )
		return 0;
	return &m_options.profile->classes[std::make_pair(m_code->GetFunction(), idx)];
}

// Counts down the calls for the profile. Returns the count left, which has run out
// if it isn't positive, see SJITProfile
CJITCodeGen::Gp CJITCodeGen::EmitCountDown()
{
	Gp countdown = PtrConst(asPWORD(&m_options.profile->countdown));
	Gp count = m_uc.new_gp32();
	m_uc.load_u32(count, mem_ptr(countdown));
	m_uc.sub(count, count, Imm(1));
	m_uc.store_u32(mem_ptr(countdown), count);
	return count;
}

// Compiles the function again when the profile has counted down the calls, and leaves
// the call to the VM if the function has new code then, which the VM goes on in after
// the call. The call is made here otherwise
void CJITCodeGen::EmitRecompile(asUINT idx)
{
	InvokeNode *call = Invoke(m_options.recompile, FuncSignature::build<int, SJITProfile*>());
	Gp r = m_uc.new_gp32();
	call->set_arg(0, Imm(int64_t(asPWORD(m_options.profile))));
	call->set_ret(0, r);
	m_uc.j(BailLabel(idx), test_nz(r));
}

// Pushes the call state like asCContext::PushCallState and calls the native code
// of a script function, or this function if target isn't valid. Jumps to slow if
// the call stack has reached the call limit, which is passed on. On 64bit hosts
// the call state may be marked with the sign bit of the stack index, so that the
// function doesn't restore the frame and the registers, see JITFunction. It then
// returns the value register too, which is taken if vrInReg is set and the value
// register is live, and true is returned then. In interop mode the current function
// of the context is set to callee, and the call state isn't marked, see JITFunction
bool CJITCodeGen::EmitNativeCall(asUINT idx, const Gp &target, const Gp &callee, const Gp &result, const Label &slow, bool mark, bool vrInReg)
{
	if( FailIfHidden() )
		return false;
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	const SJITContextLayout &layout = JIT_GetContextLayout();
	Gp length = m_uc.new_gp_ptr();
	Gp t      = m_uc.new_gp_ptr();
	m_uc.load_u32(length, ContextField(layout.callStackLength));
	m_uc.j(slow, ucmp_ge(length.r32(), m_callLimit));
	if( m_options.interop )
	{
		m_uc.store(ContextField(layout.currentFunction), callee);
		mark = false;
	}
	Gp array = m_uc.new_gp_ptr();
	m_uc.load(array, ContextField(layout.callStackArray));
	Mem state = PtrElement(array, length);
	m_uc.store(state, FramePointer(m_frameBase));
	m_uc.store(PtrAt(state, 1), PtrConst(asPWORD(m_code->GetFunction())));
	m_uc.store(PtrAt(state, 2), PtrConst(asPWORD(m_code->GetByteCode() + instr.pos + asBCTypeSize[asBCInfo[instr.op].type])));
	Gp sp = StackPointer();
	m_uc.store(PtrAt(state, 3), sp);
	m_uc.load_u32(t, ContextField(layout.stackIndex));
#ifdef JIT_NATIVE_RETURN
	if( mark )
		SetSignBit(t);
#else
	(void)mark;
#endif
	m_uc.store(PtrAt(state, 4), t);
	m_uc.add(length, length, Imm(layout.callStackFrameSize));
	m_uc.store_u32(ContextField(layout.callStackLength), length);

	FuncSignature sig = m_spInArg ? FuncSignature::build<int, asSVMRegisters*, asPWORD, asUINT, asDWORD*>() :
	                                FuncSignature::build<int, asSVMRegisters*, asPWORD, asUINT>();
	InvokeNode *call = 0;
	if( target.is_valid() )
		m_uc.cc->invoke(Out(call), target, sig);
	else
		m_uc.cc->invoke(Out(call), m_uc.cc->func()->label(), sig);
	call->set_arg(0, m_regs);
	call->set_arg(1, Imm(0));
	call->set_arg(2, m_callLimit);
	if( m_spInArg )
		call->set_arg(3, sp);
	call->set_ret(0, result);
#ifdef JIT_NATIVE_RETURN
	if( mark )
	{
		AddVRReturn(call->detail());
		if( vrInReg && m_vr.is_valid() && m_code->IsVRLiveAfter(idx) )
		{
			call->set_ret(1, m_vr);
			return true;
		}
	}
#else
	(void)vrInReg;
#endif
	return false;
}

// Entry of native callers, which pass jitArg 0, see JITFunction. The frame is set
// up like asCContext::PrepareScriptFunction does when the current stack block has
// enough space and the VM has nothing to do, otherwise by JIT_PrepareFrame. It is
// written back where the VM or the engine may see it, see JIT_FRAME_BIT
void CJITCodeGen::EmitDirectEntry()
{
	asCScriptFunction *func = m_code->GetFunction();
	const SJITContextLayout &layout = JIT_GetContextLayout();

	// The frame starts at the stack pointer of the caller, and the arguments are
	// above it. JIT_PrepareFrame takes it from the VM registers, see JITFunction
	Label slow  = m_uc.new_label();
	Label ready = m_uc.new_label();
	Gp blocks = m_uc.new_gp_ptr();
	Gp index  = m_uc.new_gp_ptr();
	Gp limit  = m_uc.new_gp_ptr();
	if( m_spInArg )
		m_uc.mov(m_fp, m_callerSp);
	else
		m_uc.load(m_fp, RegsField(offsetof(asSVMRegisters, stackPointer)));
	if( !m_staticStack )
		m_uc.mov(m_sp, m_fp);
	m_uc.load(blocks, ContextField(layout.stackBlocks));
	m_uc.load_u32(index, ContextField(layout.stackIndex));
	m_uc.sub(limit, m_fp, Imm(int(func->scriptData->stackNeeded + layout.reserveStack) * 4));
	m_uc.j(slow, ucmp_lt(limit, PtrElement(blocks, index)));
	Gp flag = m_uc.new_gp32();
	m_uc.load_u8(flag, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
	m_uc.j(slow, test_nz(flag));

	// Only the object variables on the heap are cleared, the others are initialized by their constructors
	const asCArray<asSScriptVariable*> &vars = func->scriptData->variables;
	for( asUINT n = 0; n < vars.GetLength(); n++ )
		if( vars[n]->stackOffset > 0 && vars[n]->onHeap && (vars[n]->type.IsObject() || vars[n]->type.IsFuncdef()) )
			m_uc.store_zero_reg(Var(vars[n]->stackOffset));
	m_spOffset = 0;
	PushStack(int(func->scriptData->variableSpace) * 4);

	// Only what may be read before being written needs to be loaded, like in the entry stubs
	m_uc.bind(ready);
	if( m_inlineRoom.is_valid() )
		EmitInlineRoom();
	if( m_options.syncEveryInstr )
	{
		ReloadVR();
		ReloadCachedSlots();
	}
	else
	{
		if( m_code->GetBlocks()[m_code->GetInstructions()[0].block].vrLiveIn )
			ReloadVR();
		ReloadSlots(m_code->GetLiveInMask(0));
	}
	m_uc.j(InstrLabel(0));

	m_uc.bind(slow);
	if( m_spInArg )
		m_uc.store(RegsField(offsetof(asSVMRegisters, stackPointer)), m_fp);
	m_uc.store(ContextField(layout.currentFunction), PtrConst(asPWORD(func)));
	SetPC(0);
	InvokeNode *call = Invoke((const void*)JIT_PrepareFrame, FuncSignature::build<int, asSVMRegisters*>());
	Gp r = m_uc.new_gp32();
	call->set_arg(0, m_regs);
	call->set_ret(0, r);
	EmitLeaveIf(r);

	// The VM enters the function again while a line callback is set or a suspension
	// is requested, see Generate
	if( m_options.elideSuspend )
	{
		m_uc.load_u8(flag, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
		EmitLeaveIf(flag);
	}
	m_uc.load(m_fp, RegsField(offsetof(asSVMRegisters, stackFramePointer)));
	ReloadStack();
	m_uc.j(ready);
}

bool CJITCodeGen::EmitCall(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	const asDWORD *bc = instr.bc;

	switch( instr.op )
	{
	case asBC_CALLSYS:
		if( m_options.directSystemCalls && EmitDirectSystemCall(idx, asBC_INTARG(bc)) )
			break;
		{
			SyncForCall(idx);
			InvokeNode *call = Invoke((const void*)JIT_CallSystem, FuncSignature::build<int, asSVMRegisters*, int>());
			Gp r = m_uc.new_gp32();
			call->set_arg(0, m_regs);
			call->set_arg(1, Imm(asBC_INTARG(bc)));
			call->set_ret(0, r);
			EmitAfterHelperCall(r, idx);
		}
		break;

	case asBC_Thiscall1:
		// The instruction is a CALLSYS for methods taking an int and returning a reference
		if( m_options.directSystemCalls && EmitDirectSystemCall(idx, asBC_INTARG(bc)) )
			break;
		{
			// A null object is an exception raised by the VM
			Gp obj = m_uc.new_gp_ptr();
			m_uc.load(obj, Stack(0));
			m_uc.j(BailLabel(idx), test_z(obj));

			SyncForCall(idx);
			InvokeNode *call = Invoke((const void*)JIT_Thiscall1, FuncSignature::build<int, asSVMRegisters*, int>());
			Gp r = m_uc.new_gp32();
			call->set_arg(0, m_regs);
			call->set_arg(1, Imm(asBC_INTARG(bc)));
			call->set_ret(0, r);
			EmitAfterHelperCall(r, idx);
		}
		break;

	case asBC_CALL:
		if( m_options.noScriptCalls )
			Bail(idx);
		else if( instr.flags & JIT_INSTR_INLINE )
			EmitInlineCall(idx);
		else
			EmitScriptCall(idx, JIT_CALL_SCRIPT, asBC_INTARG(bc), 0, 0);
		break;

	case asBC_CALLINTF:
		if( m_options.noScriptCalls )
			Bail(idx);
		else if( instr.flags & JIT_INSTR_INLINE )
			EmitInlineCall(idx);
		else
		{
			// The calls that note their classes count down the calls for the profile
			if( ProfileCell(idx) )
			{
				Label recompile = m_uc.new_label();
				Label cont = m_uc.new_label();
				m_uc.j(recompile, scmp_le(EmitCountDown(), Imm(0)));
				BaseNode *cold = BeginCold(recompile);
				EmitRecompile(idx);
				EndCold(cold, cont);
				m_uc.bind(cont);
				m_callsProfiled++;
			}
			EmitScriptCall(idx, JIT_CALL_INTERFACE, asBC_INTARG(bc), 0, 0);
		}
		break;

	case asBC_CALLBND:
		if( m_options.noScriptCalls )
			Bail(idx);
		else
			EmitScriptCall(idx, JIT_CALL_BOUND, asBC_INTARG(bc), 0, 0);
		break;

	case asBC_CallPtr:
		if( m_options.noScriptCalls )
			Bail(idx);
		else
		{
			Gp func = LoadPtr(asBC_SWORDARG1(bc));
			EmitScriptCall(idx, JIT_CALL_PTR, 0, &func, 0);
		}
		break;

	case asBC_RET:
		if( m_frame != 0 )
		{
			// An inlined function continues in the caller, which finds the value
			// register where the function has put it
			const std::vector<SJITInstr> &instrs = m_code->GetInstructions();
			bool last = true;
			for( asUINT n = idx + 1; n < instrs.size() && last; n++ )
				last = (instrs[n].flags & JIT_INSTR_DEAD) != 0;
			if( !last )
				m_uc.j(m_frames[m_frame].ret);
			m_frames[m_frame].retChecked = m_frames[m_frame].retChecked && m_suspendChecked;
		}
		else
		{
			// Local variables are dead at this point so only the value register
			// and stack pointer need to be written back
			const SJITContextLayout &layout = JIT_GetContextLayout();
			int popSize = asBC_WORDARG0(bc);
			bool vr = m_code->RetReadsVR();

			// Pop the call state like asCContext::PopCallState, unless the function
			// was called by the application or as a nested call, which finishes the
			// execution like the VM does
			Label finish   = m_uc.new_label();
			Label finished = m_uc.new_label();
			Gp length = m_uc.new_gp_ptr();
			Gp array  = m_uc.new_gp_ptr();
			Gp t      = m_uc.new_gp_ptr();
			m_uc.load_u32(length, ContextField(layout.callStackLength));
			m_uc.j(finish, sub_c(length, Imm(layout.callStackFrameSize)));
			m_uc.load(array, ContextField(layout.callStackArray));
			Mem state = PtrElement(array, length);
#ifdef JIT_NATIVE_RETURN
			{
				// Native callers keep their frame and set the program pointer and the
				// stack pointer themselves, so only the call stack is restored for them.
				// They get the value register in the second return register
				Label vm = m_uc.new_label();
				Gp index = m_uc.new_gp_ptr();
				m_uc.load(index, PtrAt(state, 4));
				m_uc.j(vm, scmp_ge(index, Imm(0)));
				m_uc.store_u32(ContextField(layout.stackIndex), index);
				m_uc.store_u32(ContextField(layout.callStackLength), length);
				Gp zero = m_uc.new_gp32();
				m_uc.mov(zero, Imm(0));
				if( vr && m_vr.is_valid() )
					m_uc.ret(zero, m_vr);
				else
					m_uc.ret(zero);
				m_uc.bind(vm);
			}
#endif
			if( vr )
				SyncVR();
			m_uc.load(t, state);
			m_uc.j(finished, test_z(t));
			m_uc.store(RegsField(offsetof(asSVMRegisters, stackFramePointer)), t);
			m_uc.load(t, PtrAt(state, 1));
			m_uc.store(ContextField(layout.currentFunction), t);
			m_uc.load(t, PtrAt(state, 2));
			m_uc.store(RegsField(offsetof(asSVMRegisters, programPointer)), t);
			m_uc.load(t, PtrAt(state, 3));
			if( popSize )
				m_uc.add(t, t, Imm(popSize * 4));
			m_uc.store(RegsField(offsetof(asSVMRegisters, stackPointer)), t);
			m_uc.load_u32(t, PtrAt(state, 4));
			m_uc.store_u32(ContextField(layout.stackIndex), t);
			m_uc.store_u32(ContextField(layout.callStackLength), length);
			Gp zero = m_uc.new_gp32();
			m_uc.mov(zero, Imm(0));
			m_uc.ret(zero);

			m_uc.bind(finish);
			if( vr )
				SyncVR();
			m_uc.bind(finished);
			if( m_code->GetDirtyMask(idx) & JIT_FRAME_BIT )
				StoreFrame();
			SyncStack();
			Gp status = m_uc.new_gp32();
			m_uc.mov(status, Imm(int(asEXECUTION_FINISHED)));
			m_uc.store_u32(ContextField(layout.status), status);
			Leave();
		}
		break;

	default:
		return false;
	}

	return true;
}

// Calls a registered function directly with its native calling convention, instead
// of through the generic CallSystemFunction of the engine. This is only done for
// simple signatures: primitives and pointers as arguments, a primitive, pointer,
// handle, or value type as return value, and nothing to clean up after the call.
// Everything else, e.g. objects passed by value, returns false and is called
// through the engine.
//
// C++ exceptions thrown by the function pass through the generated code and are
// caught by JIT_GuardedEntry. Where the code has no unwind information for that
// (see CJITUnwindInfo) this is only used when the JIT_DIRECT_SYSTEM_CALLS flag is set
//
// TODO: runtime optimize: Objects passed by value could be supported by setting up the
//                         argument copies the way CallSystemFunction and as_callfunc_*.cpp
//                         do for each ABI, and value types returned in more than two
//                         registers by reading them the way as_callfunc_x64_gcc.cpp does.
//                         Auto handles would need a release of the parameters after the call
//                         and an AddRef of the returned handle, and asCALL_GENERIC could be
//                         called with an asCGeneric set up inline. Each of these should be
//                         measured against CallSystemFunction before adding the code.
bool CJITCodeGen::EmitDirectSystemCall(asUINT idx, int funcId)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	asCScriptEngine *engine = m_code->GetFunction()->engine;
	if( funcId < 0 || asUINT(funcId) >= engine->scriptFunctions.GetLength() )
		return false;

	asCScriptFunction *descr = engine->scriptFunctions[funcId];
	if( descr == 0 || descr->funcType != asFUNC_SYSTEM || descr->sysFuncIntf == 0 )
		return false;
	asSSystemFunctionInterface *sysFunc = descr->sysFuncIntf;

	// Calling convention
	bool hasObj = false, objLast = false;
	CallConvId conv = CallConvId::kCDecl;
	switch( sysFunc->callConv )
	{
	case ICC_CDECL:
		break;
	case ICC_STDCALL:
		if( !Is64Bit() ) conv = CallConvId::kStdCall;
		break;
	case ICC_THISCALL:
		hasObj = true;
#if defined(JIT_X86_THISCALL)
		conv = CallConvId::kThisCall;
#endif
		break;
	case ICC_CDECL_OBJFIRST:
		hasObj = true;
		break;
	case ICC_CDECL_OBJLAST:
		hasObj = true;
		objLast = true;
		break;
	default:
		return false;
	}

	if( sysFunc->takesObjByVal || sysFunc->returnAutoHandle || sysFunc->cleanArgs.GetLength() || sysFunc->auxiliary ||
		sysFunc->compositeOffset || sysFunc->isCompositeIndirect || sysFunc->baseOffset )
		return false;
	for( asUINT n = 0; n < sysFunc->paramAutoHandles.GetLength(); n++ )
		if( sysFunc->paramAutoHandles[n] )
			return false;

	// Return value. A value type returned by value is stored at the location that the
	// caller put on the stack, either by the function itself through the hidden pointer,
	// or from the registers it was returned in
	enum { RET_VOID, RET_I32, RET_I64, RET_F32, RET_F64, RET_PTR, RET_HANDLE } retKind;
	TypeId retType;
	const asCDataType &rt = descr->returnType;
	bool retOnStack = descr->DoesReturnOnStack();
	bool retInMemory = retOnStack && sysFunc->hostReturnInMemory;
	int expectedRetSize;
#ifndef JIT_HIDDEN_RETURN_POINTER
	if( retInMemory )
		return false;
#endif
	if( retInMemory )                                            { retKind = RET_VOID;   retType = TypeId::kVoid;    expectedRetSize = AS_PTR_SIZE; }
	else if( retOnStack && sysFunc->hostReturnSize == 1 )        { retKind = sysFunc->hostReturnFloat ? RET_F32 : RET_I32; retType = sysFunc->hostReturnFloat ? TypeId::kFloat32 : TypeId::kInt32; expectedRetSize = 1; }
	else if( retOnStack && sysFunc->hostReturnSize == 2 )        { retKind = sysFunc->hostReturnFloat ? RET_F64 : RET_I64; retType = sysFunc->hostReturnFloat ? TypeId::kFloat64 : TypeId::kInt64; expectedRetSize = 2; }
	else if( retOnStack )                                        return false;
	else if( rt.GetTokenType() == ttVoid && !rt.IsReference() )  { retKind = RET_VOID;   retType = TypeId::kVoid;    expectedRetSize = 0; }
	else if( rt.IsReference() )                                  { retKind = RET_PTR;    retType = TypeId::kUIntPtr; expectedRetSize = AS_PTR_SIZE; }
	else if( rt.IsObjectHandle() )                               { retKind = RET_HANDLE; retType = TypeId::kUIntPtr; expectedRetSize = AS_PTR_SIZE; }
	else if( rt.IsObject() || rt.IsFuncdef() )                   return false;
	else if( rt.IsFloatType() )                                  { retKind = RET_F32;    retType = TypeId::kFloat32; expectedRetSize = 1; }
	else if( rt.IsDoubleType() )                                 { retKind = RET_F64;    retType = TypeId::kFloat64; expectedRetSize = 2; }
	else if( rt.GetSizeOnStackDWords() == 2 )                    { retKind = RET_I64;    retType = TypeId::kInt64;   expectedRetSize = 2; }
	else                                                         { retKind = RET_I32;    retType = TypeId::kInt32;   expectedRetSize = 1; }
	if( sysFunc->hostReturnSize != expectedRetSize || sysFunc->hostReturnFloat != (retKind == RET_F32 || retKind == RET_F64) )
		return false;

	// Arguments, as laid out on the script stack
	enum { ARG_I32, ARG_I64, ARG_F32, ARG_F64, ARG_PTR };
	struct SArg { int kind; int stackOff; TypeId type; };
	std::vector<SArg> args;
	int retOff = hasObj ? AS_PTR_SIZE : 0;
	int firstArg = retOff + (retOnStack ? AS_PTR_SIZE : 0);
	int stackPos = firstArg;
	for( asUINT n = 0; n < descr->parameterTypes.GetLength(); n++ )
	{
		const asCDataType &pt = descr->parameterTypes[n];
		SArg arg;
		arg.stackOff = stackPos;
		if( pt.GetTokenType() == ttQuestion )
			return false;
		else if( pt.IsReference() || pt.IsObjectHandle() || pt.IsObject() || pt.IsFuncdef() ) { arg.kind = ARG_PTR; arg.type = TypeId::kUIntPtr; stackPos += AS_PTR_SIZE; }
		else if( pt.IsFloatType() )                { arg.kind = ARG_F32; arg.type = TypeId::kFloat32; stackPos += 1; }
		else if( pt.IsDoubleType() )               { arg.kind = ARG_F64; arg.type = TypeId::kFloat64; stackPos += 2; }
		else if( pt.GetSizeOnStackDWords() == 2 )  { arg.kind = ARG_I64; arg.type = TypeId::kInt64;   stackPos += 2; }
		else                                       { arg.kind = ARG_I32; arg.type = TypeId::kInt32;   stackPos += 1; }
		args.push_back(arg);
	}
	if( stackPos - firstArg != sysFunc->paramSize )
		return false;
	if( args.size() + (hasObj ? 1 : 0) + (retInMemory ? 1 : 0) > Globals::kMaxFuncArgs )
		return false;
	// With the System V x64 ABI AsmJit gives the floats passed on the stack 4 bytes
	// each instead of 8, so the functions that have any are called through the engine
	const Environment &env = m_uc.cc->environment();
	if( Is64Bit() && !env.is_platform_windows() && !env.is_msvc_abi() )
	{
		asUINT vecArgs = 0;
		for( asUINT n = 0; n < args.size(); n++ )
		{
			if( args[n].kind != ARG_F32 && args[n].kind != ARG_F64 )
				continue;
			if( args[n].kind == ARG_F32 && vecArgs >= 8 )
				return false;
			vecArgs++;
		}
	}
	int popSize = stackPos;

	// The hidden return pointer comes first, except after the object pointer of class methods with MSVC
	bool retFirst = retInMemory, retAfterObj = false;
#ifdef JIT_HIDDEN_RETURN_POINTER_AFTER_THIS
	if( retInMemory && sysFunc->callConv == ICC_THISCALL )
	{
		retFirst = false;
		retAfterObj = true;
	}
#endif

	FuncSignature sig(conv);
	sig.set_ret(retType);
	if( retFirst ) sig.add_arg(TypeId::kUIntPtr);
	if( hasObj && !objLast ) sig.add_arg(TypeId::kUIntPtr);
	if( retAfterObj ) sig.add_arg(TypeId::kUIntPtr);
	for( asUINT n = 0; n < args.size(); n++ ) sig.add_arg(args[n].type);
	if( hasObj && objLast ) sig.add_arg(TypeId::kUIntPtr);

	// A null object pointer is an exception raised by the VM
	Gp obj;
	if( hasObj )
	{
		obj = m_uc.new_gp_ptr();
		m_uc.load(obj, Stack(0));
		m_uc.j(BailLabel(idx), test_z(obj));
	}
	Gp retPtr;
	if( retOnStack )
	{
		retPtr = m_uc.new_gp_ptr();
		m_uc.load(retPtr, Stack(retOff));
	}

	// Load the arguments before anything is written back so the loads can be scheduled freely
	std::vector<Gp>  gpArgs(args.size());
	std::vector<Gp>  gpArgsHi(args.size());
	std::vector<Vec> vecArgs(args.size());
	for( asUINT n = 0; n < args.size(); n++ )
	{
		switch( args[n].kind )
		{
		case ARG_PTR:
			gpArgs[n] = m_uc.new_gp_ptr();
			m_uc.load(gpArgs[n], Stack(args[n].stackOff));
			break;
		case ARG_I32:
			gpArgs[n] = m_uc.new_gp32();
			m_uc.load_u32(gpArgs[n], Stack(args[n].stackOff));
			break;
		case ARG_I64:
			if( Is64Bit() )
			{
				gpArgs[n] = m_uc.new_gp64();
				m_uc.load_u64(gpArgs[n], Stack(args[n].stackOff));
			}
			else
			{
				gpArgs[n] = m_uc.new_gp32();
				gpArgsHi[n] = m_uc.new_gp32();
				m_uc.load_u32(gpArgs[n], Stack(args[n].stackOff));
				m_uc.load_u32(gpArgsHi[n], Stack(args[n].stackOff + 1));
			}
			break;
		case ARG_F32:
			vecArgs[n] = m_uc.new_vec128_f32x1();
			m_uc.v_loadu32_f32(vecArgs[n], Stack(args[n].stackOff));
			break;
		case ARG_F64:
			vecArgs[n] = m_uc.new_vec128_f64x1();
			m_uc.v_loadu64_f64(vecArgs[n], Stack(args[n].stackOff));
			break;
		}
	}

	// The function may raise a script exception, suspend the context, or
	// inspect the variables through the debug interface, so the VM registers
	// must be up to date, and the frames of the inlined calls on the call stack.
	// The value register isn't needed until the slow path
	StoreDirtySlots(m_code->GetDirtyMask(idx));
	SyncStack();
	EmitMaterialize();
	SetPC(instr.pos);

	// Let the context know which function is being called, so that it may raise exceptions
	Mem callingFunc = ContextField(JIT_GetContextLayout().callingSystemFunction);
	m_uc.store(callingFunc, PtrConst(asPWORD(descr)));

	InvokeNode *call = Invoke((const void*)FuncPtrToUInt(sysFunc->func), sig);
	asUINT argIdx = 0;
	if( retFirst )
		call->set_arg(argIdx++, retPtr);
	if( hasObj && !objLast )
		call->set_arg(argIdx++, obj);
	if( retAfterObj )
		call->set_arg(argIdx++, retPtr);
	for( asUINT n = 0; n < args.size(); n++ )
	{
		if( args[n].kind == ARG_F32 || args[n].kind == ARG_F64 )
			call->set_arg(argIdx, vecArgs[n]);
		else if( args[n].kind == ARG_I64 && !Is64Bit() )
		{
			call->set_arg(argIdx, 0, gpArgs[n]);
			call->set_arg(argIdx, 1, gpArgsHi[n]);
		}
		else
			call->set_arg(argIdx, gpArgs[n]);
		argIdx++;
	}
	if( hasObj && objLast )
		call->set_arg(argIdx++, obj);

	Gp  retGp, retGpHi;
	Vec retVec;
	switch( retKind )
	{
	case RET_I32:
		retGp = m_uc.new_gp32();
		call->set_ret(0, retGp);
		break;
	case RET_I64:
		if( Is64Bit() )
		{
			retGp = m_uc.new_gp64();
			call->set_ret(0, retGp);
		}
		else
		{
			retGp = m_uc.new_gp32();
			retGpHi = m_uc.new_gp32();
			call->set_ret(0, retGp);
			call->set_ret(1, retGpHi);
		}
		break;
	case RET_F32:
		retVec = m_uc.new_vec128_f32x1();
		call->set_ret(0, retVec);
		break;
	case RET_F64:
		retVec = m_uc.new_vec128_f64x1();
		call->set_ret(0, retVec);
		break;
	case RET_PTR:
	case RET_HANDLE:
		retGp = m_uc.new_gp_ptr();
		call->set_ret(0, retGp);
		break;
	default:
		break;
	}

	m_uc.store_zero_reg(callingFunc);

	// Pop the arguments and store the return value like the VM does, except
	// that the value register is left alone if it isn't read afterwards
	PopStack(popSize * 4);
	bool vrLive = m_code->IsVRLiveAfter(idx);
	if( retOnStack )
	{
		switch( retKind )
		{
		case RET_I32:
			m_uc.store_u32(mem_ptr(retPtr), retGp);
			break;
		case RET_I64:
			if( Is64Bit() )
				m_uc.store_u64(mem_ptr(retPtr), retGp);
			else
			{
				m_uc.store_u32(mem_ptr(retPtr), retGp);
				m_uc.store_u32(Addr(retPtr, 4), retGpHi);
			}
			break;
		case RET_F32:
			m_uc.v_storeu32_f32(mem_ptr(retPtr), retVec);
			break;
		case RET_F64:
			m_uc.v_storeu64_f64(mem_ptr(retPtr), retVec);
			break;
		default:
			break;
		}
	}
	else if( retKind == RET_HANDLE )
	{
		m_uc.store(RegsField(offsetof(asSVMRegisters, objectRegister)), retGp);
		m_uc.store(RegsField(offsetof(asSVMRegisters, objectType)), PtrConst(asPWORD(rt.GetTypeInfo())));
	}
	else if( vrLive ) switch( retKind )
	{
	case RET_I32:
		StoreVR32(retGp);
		break;
	case RET_I64:
		if( Is64Bit() )
			StoreVR64(retGp);
		else
		{
			m_uc.store_u32(VRMem(), retGp);
			Mem hi = VRMem();
			hi.add_offset(4);
			m_uc.store_u32(hi, retGpHi);
		}
		break;
	case RET_F32:
		{
			Gp bits = m_uc.new_gp32();
			m_uc.s_mov_u32(bits, retVec);
			StoreVR32(bits);
		}
		break;
	case RET_F64:
		if( Is64Bit() )
		{
			Gp bits = m_uc.new_gp64();
			m_uc.s_mov_u64(bits, retVec);
			StoreVR64(bits);
		}
		else
			m_uc.v_storeu64_f64(VRMem(), retVec);
		break;
	case RET_PTR:
		StoreVRPtr(retGp);
		break;
	default:
		break;
	}

	// Exceptions, suspend requests, and line callbacks are handled by the helper
	Gp flag = m_uc.new_gp32();
	m_uc.load_u8(flag, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
	Label check = m_uc.new_label();
	Label cont = m_uc.new_label();
	m_uc.j(check, test_nz(flag));
	BaseNode *cold = BeginCold(check);
	// The VM continues with the popped stack pointer if the helper returns non-zero
	SyncStack();
	if( vrLive )
		SyncVR();
	InvokeNode *after = Invoke((const void*)JIT_AfterDirectCall, FuncSignature::build<int, asSVMRegisters*, int, void*>());
	Gp r = m_uc.new_gp32();
	after->set_arg(0, m_regs);
	after->set_arg(1, Imm(funcId));
	if( retOnStack )
		after->set_arg(2, retPtr);
	else
		after->set_arg(2, Imm(0));
	after->set_ret(0, r);
	EmitLeaveIf(r);
	// With a debugger attached the variables may have been modified through the context
	ReloadLiveSlots(idx);
	EndCold(cold, cont);
	m_uc.bind(cont);
	EmitDematerialize();

	return true;
}

// Resolves a behaviour that takes nothing but the object, like AddRef and Release,
// to a function that the generated code can call instead of going through
// CallObjectMethod of the engine. The same restrictions as for EmitDirectSystemCall
// apply, and any return value is ignored. Returns false if the engine must call it
bool CJITCodeGen::GetDirectBehaviour(int funcId, SDirectBehaviour &beh) const
{
	asCScriptEngine *engine = m_code->GetFunction()->engine;
	if( !m_options.directSystemCalls || funcId <= 0 || asUINT(funcId) >= engine->scriptFunctions.GetLength() )
		return false;

	asCScriptFunction *descr = engine->scriptFunctions[funcId];
	if( descr == 0 || descr->funcType != asFUNC_SYSTEM || descr->sysFuncIntf == 0 )
		return false;
	asSSystemFunctionInterface *sysFunc = descr->sysFuncIntf;

	// A floating point value returned on the x87 stack would have to be popped
	if( descr->parameterTypes.GetLength() || sysFunc->paramSize || descr->DoesReturnOnStack() ||
		sysFunc->hostReturnInMemory || sysFunc->hostReturnFloat || sysFunc->auxiliary ||
		sysFunc->compositeOffset || sysFunc->isCompositeIndirect || sysFunc->baseOffset )
		return false;

	beh.func = (const void*)FuncPtrToUInt(sysFunc->func);
	beh.isVirtual = false;
	beh.conv = CallConvId::kCDecl;
	switch( sysFunc->callConv )
	{
	case ICC_THISCALL:
#if defined(JIT_X86_THISCALL)
		beh.conv = CallConvId::kThisCall;
#endif
		break;
#if defined(GNU_STYLE_VIRTUAL_METHOD) && (defined(AS_X86) || defined(AS_X64_GCC) || defined(AS_X64_MINGW))
	case ICC_VIRTUAL_THISCALL:
		// With the Itanium C++ ABI the method pointer of a virtual method holds its
		// offset in the virtual function table plus 1
		beh.isVirtual = true;
#if defined(JIT_X86_THISCALL)
		beh.conv = CallConvId::kThisCall;
#endif
		break;
#endif
	case ICC_CDECL_OBJFIRST:
	case ICC_CDECL_OBJLAST:
		break;
	default:
		return false;
	}
	return true;
}

// True if the instruction may call AddRef or Release directly, see EmitObjectOp
bool CJITCodeGen::CallsBehaviourDirectly(const SJITInstr &instr) const
{
	if( (instr.op != asBC_FREE && instr.op != asBC_REFCPY && instr.op != asBC_RefCpyV) || (instr.flags & JIT_INSTR_REFCOUNT) )
		return false;
	asCObjectType *objType = (asCObjectType*)asBC_PTRARG(instr.bc);
	SDirectBehaviour beh;
	return GetDirectBehaviour(objType->beh.release, beh) || GetDirectBehaviour(objType->beh.addref, beh);
}

// True if the instruction neither adds nor releases a reference, as the argument of
// an inlined call borrows it, see CJITByteCode::AnalyseBorrows
bool CJITCodeGen::IsBorrowed(asUINT idx) const
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	if( instr.flags & JIT_INSTR_BAIL )
		return false;
	if( instr.op == asBC_RefCpyV )
		return (instr.flags & JIT_INSTR_BORROW) != 0;
	if( instr.op != asBC_FREE || m_frame == 0 )
		return false;
	int p = m_code->FindParam(asBC_SWORDARG0(instr.bc));
	return p >= 0 && p < 31 && ((m_frames[m_frame].borrowed >> p) & 1);
}

// The object pointer must not be null
void CJITCodeGen::EmitBehaviourCall(const SDirectBehaviour &beh, const Gp &obj)
{
	FuncSignature sig(beh.conv);
	sig.set_ret(TypeId::kVoid);
	sig.add_arg(TypeId::kUIntPtr);

	InvokeNode *call = 0;
	if( beh.isVirtual )
	{
		Gp target = m_uc.new_gp_ptr();
		m_uc.load(target, mem_ptr(obj));
		m_uc.load(target, Addr(target, int32_t(asPWORD(beh.func) - 1)));
		m_uc.cc->invoke(Out(call), target, sig);
	}
	else
		call = Invoke(beh.func, sig);
	call->set_arg(0, obj);
}

// Adds a reference to the script object in place, see CJITByteCode::FindInPlaceRefCounts.
// The object pointer must not be null
void CJITCodeGen::EmitScriptAddRef(asUINT idx, const Gp &obj)
{
	Label slow = m_uc.new_label();
	Label done = m_uc.new_label();
	EmitAddRefInPlace(obj, slow);
	BaseNode *cold = BeginCold(slow);
	SyncForCall(idx);
	InvokeNode *call = Invoke((const void*)JIT_AddRefScriptObject, FuncSignature::build<void, void*>());
	call->set_arg(0, obj);
	EmitDematerialize();
	EndCold(cold, done);
	m_uc.bind(done);
}

// Releases a reference of the script object in place. The last one is released by
// Release, which may execute the destructor, see FREE. The object pointer must not
// be null
void CJITCodeGen::EmitScriptRelease(asUINT idx, const Gp &obj)
{
	Label slow = m_uc.new_label();
	Label race = m_uc.new_label();
	Label done = m_uc.new_label();
	EmitReleaseInPlace(obj, slow, race);
	BaseNode *cold;
	if( JIT_GetObjectLayout().atomicRefCount )
	{
		// Another thread has released a reference between the check and the
		// decrement, so the reference is given back for Release to destroy the object
		cold = BeginCold(race);
		EmitRefCountInc(obj);
		m_uc.bind(slow);
	}
	else
		cold = BeginCold(slow);
	SyncForCall(idx);
	InvokeNode *call = Invoke((const void*)JIT_ReleaseScriptObject, FuncSignature::build<void, void*>());
	call->set_arg(0, obj);
	EmitDematerialize();
	EndCold(cold, done);
	m_uc.bind(done);
}

bool CJITCodeGen::EmitObjectOp(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	const asDWORD *bc = instr.bc;
	int a0 = asBC_SWORDARG0(bc);

	switch( instr.op )
	{
	case asBC_ALLOC:
		{
			asCObjectType *objType = (asCObjectType*)asBC_PTRARG(bc);
			int func = asBC_INTARG(bc + AS_PTR_SIZE);
			if( objType->flags & asOBJ_SCRIPT_OBJECT )
			{
				if( m_options.noScriptCalls )
					Bail(idx);
				else
					EmitScriptCall(idx, JIT_CALL_ALLOC, func, 0, asPWORD(objType));
			}
			else
			{
				SyncAll(idx);
				InvokeNode *call = Invoke((const void*)JIT_Alloc, FuncSignature::build<int, asSVMRegisters*, void*, int>());
				Gp r = m_uc.new_gp32();
				call->set_arg(0, m_regs);
				call->set_arg(1, Imm(int64_t(asPWORD(objType))));
				call->set_arg(2, Imm(func));
				call->set_ret(0, r);
				EmitAfterHelperCall(r, idx);
			}
		}
		break;

	case asBC_FREE:
		if( IsBorrowed(idx) || (instr.flags & JIT_INSTR_MOVED) )
		{
			// The caller releases the reference, or the copy before has taken it
			// over, see CJITByteCode::FindMovedRefs
			m_uc.store_zero_reg(Var(a0));
			break;
		}
		if( instr.flags & JIT_INSTR_FREE_LIST )
		{
			// Nothing in the list is destroyed, see CJITByteCode::FindListFrees
			Gp mem = LoadPtr(a0);
			Label skip = m_uc.new_label();
			m_uc.j(skip, test_z(mem));
			InvokeNode *call = Invoke((const void*)JIT_FreeMem, FuncSignature::build<void, void*>());
			call->set_arg(0, mem);
			m_uc.store_zero_reg(Var(a0));
			m_uc.bind(skip);
			break;
		}
		if( instr.flags & JIT_INSTR_REFCOUNT )
		{
			// Like the VM the variable is cleared after the release
			Gp obj = LoadPtr(a0);
			Label skip = m_uc.new_label();
			m_uc.j(skip, test_z(obj));
			EmitScriptRelease(idx, obj);
			m_uc.store_zero_reg(Var(a0));
			m_uc.bind(skip);
			break;
		}
		{
			asCObjectType *objType = (asCObjectType*)asBC_PTRARG(bc);
			SDirectBehaviour release;
			bool direct = (objType->flags & asOBJ_REF) && GetDirectBehaviour(objType->beh.release, release);

			Gp obj = LoadPtr(a0);
			Label skip = m_uc.new_label();
			m_uc.j(skip, test_z(obj));
			// The release may execute a script destructor on the same context,
			// which starts its frame at the stack pointer of the registers
			SyncForCall(idx);
			if( direct )
			{
				// Like the VM the variable is cleared after the release
				EmitBehaviourCall(release, obj);
				m_uc.store_zero_reg(Var(a0));
			}
			else
			{
				Gp var = m_uc.new_gp_ptr();
				LeaVar(var, a0);
				InvokeNode *call = Invoke((const void*)JIT_Free, FuncSignature::build<void, asSVMRegisters*, void*, void*>());
				call->set_arg(0, m_regs);
				call->set_arg(1, Imm(int64_t(asPWORD(objType))));
				call->set_arg(2, var);
			}
			EmitDematerialize();
			m_uc.bind(skip);
		}
		break;

	case asBC_LOADOBJ:
		{
			Gp obj = LoadPtr(a0);
			m_uc.store_zero_reg(RegsField(offsetof(asSVMRegisters, objectType)));
			m_uc.store(RegsField(offsetof(asSVMRegisters, objectRegister)), obj);
			m_uc.store_zero_reg(Var(a0));
		}
		break;

	case asBC_STOREOBJ:
		{
			Gp obj = m_uc.new_gp_ptr();
			m_uc.load(obj, RegsField(offsetof(asSVMRegisters, objectRegister)));
			StorePtr(a0, obj);
			m_uc.store_zero_reg(RegsField(offsetof(asSVMRegisters, objectRegister)));
		}
		break;

	case asBC_REFCPY:
	case asBC_RefCpyV:
		if( IsBorrowed(idx) )
		{
			// Lends the reference to the inlined call. The first of the copies for the
			// call checks that none of them would release anything
			const std::vector<int> &checks = m_code->GetBorrowChecks(idx);
			if( !checks.empty() )
			{
				Gp any = LoadPtr(checks[0]);
				for( asUINT n = 1; n < checks.size(); n++ )
					m_uc.or_(any, any, LoadPtr(checks[n]));
				m_uc.j(BailLabel(idx), test_nz(any));
			}
			Gp s = m_uc.new_gp_ptr();
			m_uc.load(s, Stack(0));
			StorePtr(a0, s);
			break;
		}
		if( instr.flags & JIT_INSTR_REFCOUNT )
		{
			// The references of the script objects are counted in place, see
			// CJITByteCode::FindInPlaceRefCounts. Like the VM the old object is
			// released before the new one gets its reference, which the handle may
			// take over from the variable released next, and the destination is set last
			Mem dst;
			if( instr.op == asBC_REFCPY )
			{
				Gp d = m_uc.new_gp_ptr();
				m_uc.load(d, Stack(0));
				PopStack(PTR_BYTES);
				dst = mem_ptr(d);
			}
			else
				dst = Var(a0);
			Gp s = m_uc.new_gp_ptr();
			m_uc.load(s, Stack(0));
			Gp old = m_uc.new_gp_ptr();
			m_uc.load(old, dst);
			Label noOld = m_uc.new_label();
			m_uc.j(noOld, test_z(old));
			EmitScriptRelease(idx, old);
			m_uc.bind(noOld);
			if( !(instr.flags & JIT_INSTR_MOVE) )
			{
				Label noNew = m_uc.new_label();
				m_uc.j(noNew, test_z(s));
				EmitScriptAddRef(idx, s);
				m_uc.bind(noNew);
			}
			m_uc.store(dst, s);
			break;
		}
		{
			asCObjectType *objType = (asCObjectType*)asBC_PTRARG(bc);
			SDirectBehaviour addref, release;
			bool counted = !(objType->flags & (asOBJ_NOCOUNT | asOBJ_VALUE));
			bool direct = counted && GetDirectBehaviour(objType->beh.release, release) && GetDirectBehaviour(objType->beh.addref, addref);
			// The handle may take over the reference of the variable that is released
			// next, see CJITByteCode::FindMovedRefs
			bool move = (instr.flags & JIT_INSTR_MOVE) != 0;

			// REFCPY pops the address of the destination, RefCpyV takes a variable
			Gp d = m_uc.new_gp_ptr();
			Gp s = m_uc.new_gp_ptr();
			if( instr.op == asBC_REFCPY )
			{
				m_uc.load(d, Stack(0));
				PopStack(PTR_BYTES);
			}
			else
				LeaVar(d, a0);
			m_uc.load(s, Stack(0));
			// The release of the old object may execute a script destructor, see FREE
			SyncForCall(idx);
			if( !counted || direct )
			{
				// Like the VM the old object is released before the new one gets its
				// reference, and the destination is set last
				if( direct )
				{
					Gp old = m_uc.new_gp_ptr();
					m_uc.load(old, mem_ptr(d));
					Label noOld = m_uc.new_label();
					m_uc.j(noOld, test_z(old));
					EmitBehaviourCall(release, old);
					m_uc.bind(noOld);
					if( !move )
					{
						Label noNew = m_uc.new_label();
						m_uc.j(noNew, test_z(s));
						EmitBehaviourCall(addref, s);
						m_uc.bind(noNew);
					}
				}
				m_uc.store(mem_ptr(d), s);
			}
			else if( move )
			{
				// Releases the old object and clears the variable
				InvokeNode *call = Invoke((const void*)JIT_Free, FuncSignature::build<void, asSVMRegisters*, void*, void*>());
				call->set_arg(0, m_regs);
				call->set_arg(1, Imm(int64_t(asPWORD(objType))));
				call->set_arg(2, d);
				m_uc.store(mem_ptr(d), s);
			}
			else
			{
				InvokeNode *call = Invoke((const void*)JIT_RefCpy, FuncSignature::build<void, asSVMRegisters*, void*, void*, void*>());
				call->set_arg(0, m_regs);
				call->set_arg(1, Imm(int64_t(asPWORD(objType))));
				call->set_arg(2, d);
				call->set_arg(3, s);
			}
			EmitDematerialize();
		}
		break;

	case asBC_Cast:
		{
			Gp a = m_uc.new_gp_ptr();
			m_uc.load(a, Stack(0));
			InvokeNode *call = Invoke((const void*)JIT_Cast, FuncSignature::build<void, asSVMRegisters*, void*, asDWORD>());
			call->set_arg(0, m_regs);
			call->set_arg(1, a);
			call->set_arg(2, Imm(int(asBC_DWORDARG(bc))));
			PopStack(PTR_BYTES);
		}
		break;

	default:
		return false;
	}

	return true;
}

END_AS_NAMESPACE
