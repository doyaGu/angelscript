#include "jit_codegen.h"
#include "jit_runtime.h"

// The engine internals are only inspected at compile time, to know the object
// type flags and the calling conventions of the registered functions
#include "as_module.h"
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
// which the generated calls can't express. AArch64 passes it in x8, which isn't
// used for arguments, so the generated calls add it as the last argument and
// assign that argument the register, see EmitDirectSystemCall
#if defined(AS_X64_MSVC) || defined(AS_X64_GCC) || defined(AS_X64_MINGW) || (defined(AS_X86) && defined(_MSC_VER))
#define JIT_HIDDEN_RETURN_POINTER
#if defined(_MSC_VER)
#define JIT_HIDDEN_RETURN_POINTER_AFTER_THIS
#endif
#elif defined(AS_ARM64)
#define JIT_HIDDEN_RETURN_POINTER_X8
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
// compiled or the call stack has reached the call limit. extra is the type of the
// object for JIT_CALL_ALLOC, and the variable that holds the function pointer for
// JIT_CALL_PTR
void CJITCodeGen::EmitScriptCall(asUINT idx, int kind, int funcId, asPWORD extra)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	asCScriptFunction *func = m_code->GetFunction();
	asCScriptFunction *callee = 0;
	if( kind == JIT_CALL_PTR )
		callee = func->GetCalledFunction(instr.pos);
	else if( funcId >= 0 && asUINT(funcId) < func->engine->scriptFunctions.GetLength() )
		callee = func->engine->scriptFunctions[funcId];
	else if( kind == JIT_CALL_BOUND )
	{
		asUINT imported = asUINT(funcId & ~FUNC_IMPORTED);
		if( imported < func->engine->importedFunctions.GetLength() && func->engine->importedFunctions[imported] )
			callee = func->engine->importedFunctions[imported]->importedFunctionSignature;
	}
	const std::vector<asCScriptFunction*> *borrowTargets = (kind == JIT_CALL_INTERFACE || kind == JIT_CALL_BOUND || kind == JIT_CALL_PTR) ? &m_code->GetBorrowedTargets(idx) : 0;
	// EmitInlineCall gives borrowed arguments references of their own before its
	// fallback reaches this path, so only separately generated calls pass the mask.
	asUINT borrowed = !(instr.flags & JIT_INSTR_INLINE) &&
	                  (kind == JIT_CALL_SCRIPT || (borrowTargets && !borrowTargets->empty()))
	                  ? m_code->GetBorrowedArgs(idx) : 0;

	bool native = false;
	if( kind == JIT_CALL_SCRIPT || kind == JIT_CALL_ALLOC )
		native = callee && callee->funcType == asFUNC_SCRIPT && callee->scriptData;
	else if( kind == JIT_CALL_INTERFACE )
		native = callee && (callee->funcType == asFUNC_VIRTUAL || callee->funcType == asFUNC_INTERFACE);
	else if( kind == JIT_CALL_BOUND )
		native = callee != 0;
	else if( kind == JIT_CALL_PTR )
		native = true;

	// The object of a script class is allocated first, with everything synced like
	// the VM does, as the allocation may reuse the context for nested calls. Then
	// it is stored in the variable, and pushed for the constructor
	bool synced = false;
	if( native && kind == JIT_CALL_ALLOC )
	{
		SyncForCall(idx);
		Gp obj = m_uc.new_gp_ptr();
		int slot = FreshSlot(idx);
		if( slot >= 0 )
			EmitFrameObject(obj, slot, reinterpret_cast<asCObjectType*>(extra));
		else
		{
			InvokeNode *alloc = Invoke((const void*)JIT_NewScriptObject, FuncSignature::build<void*, void*>());
			alloc->set_arg(0, Imm(int64_t(extra)));
			alloc->set_ret(0, obj);
		}

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
		StoreDirtySlots(m_code->GetDirtyMask(idx).AndNot(JIT_FRAME_BIT));
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
		Label delegate;
		if( (kind == JIT_CALL_SCRIPT || kind == JIT_CALL_CONSTRUCT) && callee != m_frames[0].code->GetFunction() )
		{
			target = m_uc.new_gp_ptr();
			m_uc.load(target, mem_ptr(PtrConst(asPWORD(&callee->scriptData->jitFunction))));
		}
		else if( kind == JIT_CALL_INTERFACE )
		{
			method = EmitFindMethod(callee, slow, (instr.flags & JIT_INSTR_INLINE) ? 0 : ProfileCell(idx));
			if( borrowed )
			{
				Label accepted = m_uc.new_label();
				for( asUINT n = 0; n + 1 < borrowTargets->size(); n++ )
					m_uc.j(accepted, cmp_eq(method, PtrConst(asPWORD((*borrowTargets)[n]))));
				m_uc.j(slow, cmp_ne(method, PtrConst(asPWORD(borrowTargets->back()))));
				m_uc.bind(accepted);
			}
			target = m_uc.new_gp_ptr();
			m_uc.load(target, Addr(method, layout.scriptData));
			m_uc.load(target, Addr(target, layout.jitFunction));
		}
		else if( kind == JIT_CALL_BOUND )
		{
			method = m_uc.new_gp_ptr();
			InvokeNode *resolve = Invoke((const void*)JIT_GetBoundScriptFunction, FuncSignature::build<asCScriptFunction*, asIScriptEngine*, int>());
			resolve->set_arg(0, Imm(int64_t(asPWORD(func->engine))));
			resolve->set_arg(1, Imm(funcId));
			resolve->set_ret(0, method);
			m_uc.j(slow, test_z(method));
			if( borrowed )
			{
				Label accepted = m_uc.new_label();
				for( asUINT n = 0; n + 1 < borrowTargets->size(); n++ )
					m_uc.j(accepted, cmp_eq(method, PtrConst(asPWORD((*borrowTargets)[n]))));
				m_uc.j(slow, cmp_ne(method, PtrConst(asPWORD(borrowTargets->back()))));
				m_uc.bind(accepted);
			}
			target = m_uc.new_gp_ptr();
			m_uc.load(target, Addr(method, layout.scriptData));
			m_uc.load(target, Addr(target, layout.jitFunction));
		}
		else if( kind == JIT_CALL_PTR )
		{
			// The function pointer is loaded from its variable wherever it is needed,
			// so that no register holds it across the call. Script functions are
			// called here, and the delegates of script methods by the cold code of
			// EmitDelegateCall, where the stack is static and passed to the callee.
			// Everything else is left to the helper
			Gp func = LoadPtr(int(extra));
			Gp type = m_uc.new_gp32();
			m_uc.j(slow, test_z(func));
			SJITSeenFunctions *seen = (instr.flags & JIT_INSTR_INLINE) ? 0 : FunctionProfileCell(idx);
			if( seen )
				EmitNoteFunction(seen, func);
			if( borrowed )
			{
				Label accepted = m_uc.new_label();
				for( asUINT n = 0; n + 1 < borrowTargets->size(); n++ )
					m_uc.j(accepted, cmp_eq(func, PtrConst(asPWORD((*borrowTargets)[n]))));
				m_uc.j(slow, cmp_ne(func, PtrConst(asPWORD(borrowTargets->back()))));
				m_uc.bind(accepted);
			}
			m_uc.load_u32(type, Addr(func, layout.funcType));
			if( m_staticStack && m_spInArg )
			{
				delegate = m_uc.new_label();
				m_uc.j(delegate, cmp_eq(type, Imm(int(asFUNC_DELEGATE))));
			}
			m_uc.j(slow, cmp_ne(type, Imm(int(asFUNC_SCRIPT))));
			target = m_uc.new_gp_ptr();
			m_uc.load(target, Addr(func, layout.scriptData));
			m_uc.load(target, Addr(target, layout.jitFunction));
			method = func;
		}
		if( target.is_valid() )
		{
			m_uc.j(slow, test_z(target));
			// jitFunction is an immutable wrapper that dispatches to the current code,
			// and continues in the VM while compilation is still deferred. Call it
			// directly instead of resolving its target through the compiler's locked
			// function map on every script call.
		}
		if( m_options.interop && !method.is_valid() )
			method = PtrConst(asPWORD(callee));

		// The called function has popped the arguments. Unless the size isn't known
		// the stack pointer is adjusted instead of waiting for the one it stored,
		// which the call state must not be marked for then. A static stack pointer
		// is known after any call
		bool reload = !m_staticStack && (kind == JIT_CALL_PTR || callee->IsVariadic());
		bool vrInReg = !callee || kind == JIT_CALL_PTR || CJITByteCode::ReturnsInVR(callee);
		vrReturned = EmitNativeCall(idx, target, method, r, slow, !reload, vrInReg, borrowed);
		if( delegate.is_valid() )
			EmitDelegateCall(idx, int(extra), delegate, slow, r, !reload, vrInReg);
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
		if( m_code->GetDirtyMask(idx).Test(JIT_FRAME_SLOT) )
			StoreFrame();
		SetPC(instr.pos);
	}
	if( borrowed )
	{
		Gp frame = StackPointer();
		InvokeNode *own = Invoke((const void*)JIT_OwnParams, FuncSignature::build<void, asCScriptFunction*, asDWORD*, asUINT>());
		own->set_arg(0, Imm(int64_t(asPWORD(callee))));
		own->set_arg(1, frame);
		own->set_arg(2, Imm(int(borrowed)));
	}
	// The function pointer is loaded again here, see above
	Gp funcPtr;
	if( kind == JIT_CALL_PTR )
		funcPtr = LoadPtr(int(extra));
	InvokeNode *call = Invoke((const void*)JIT_CallScript, FuncSignature::build<int, asSVMRegisters*, int, int, asPWORD, asUINT>());
	SetRegsArg(call, 0);
	call->set_arg(1, Imm(kind));
	call->set_arg(2, Imm(funcId));
	if( kind == JIT_CALL_PTR )
		call->set_arg(3, funcPtr);
	else
		call->set_arg(3, Imm(int64_t(extra)));
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

// Calls the method of a delegate natively, like the VM does for asBC_CallPtr: the
// object of the delegate is pushed below the arguments, and a virtual method is
// looked up in the virtual function table of the object. Interface methods are
// resolved from the object's implementation, and registered functions are left to
// the helper. This is cold code after the call of a plain function pointer, which
// it rejoins
void CJITCodeGen::EmitDelegateCall(asUINT idx, int funcVar, const Label &delegate, const Label &slow, const Gp &result, bool mark, bool vrInReg)
{
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	Label done       = m_uc.new_label();
	Label interfaceMethod = m_uc.new_label();
	Label haveMethod      = m_uc.new_label();
	BaseNode *cold = BeginCold(delegate);
	Gp func   = LoadPtr(funcVar);
	Gp obj    = m_uc.new_gp_ptr();
	Gp method = m_uc.new_gp_ptr();
	Gp type   = m_uc.new_gp32();
	m_uc.load(obj, Addr(func, layout.objForDelegate));
	m_uc.load(method, Addr(func, layout.funcForDelegate));
	m_uc.j(slow, test_z(obj));
	m_uc.load_u32(type, Addr(method, layout.funcType));
	m_uc.j(interfaceMethod, cmp_eq(type, Imm(int(asFUNC_INTERFACE))));
	m_uc.j(haveMethod, cmp_ne(type, Imm(int(asFUNC_VIRTUAL))));
	Gp table = m_uc.new_gp_ptr();
	Gp index = m_uc.new_gp_ptr();
	m_uc.load(table, Addr(obj, layout.objectType));
	m_uc.load(table, Addr(table, layout.virtualFunctionTable));
	m_uc.load_u32(index, Addr(method, layout.vfTableIdx));
	m_uc.load(method, PtrElement(table, index));
	m_uc.load_u32(type, Addr(method, layout.funcType));
	m_uc.j(haveMethod);
	m_uc.bind(interfaceMethod);
	m_uc.load(table, Addr(obj, layout.objectType));
	InvokeNode *resolve = Invoke((const void*)JIT_FindInterfaceMethod, FuncSignature::build<asCScriptFunction*, asCObjectType*, asCScriptFunction*>());
	resolve->set_arg(0, table);
	resolve->set_arg(1, method);
	resolve->set_ret(0, method);
	m_uc.j(slow, test_z(method));
	m_uc.load_u32(type, Addr(method, layout.funcType));
	m_uc.bind(haveMethod);
	m_uc.j(slow, cmp_ne(type, Imm(int(asFUNC_SCRIPT))));
	Gp target = m_uc.new_gp_ptr();
	m_uc.load(target, Addr(method, layout.scriptData));
	m_uc.load(target, Addr(target, layout.jitFunction));
	m_uc.j(slow, test_z(target));
	Gp sp = m_uc.new_gp_ptr();
	m_uc.sub(sp, StackPointer(), Imm(PTR_BYTES));
	m_uc.store(mem_ptr(sp), obj);
	EmitNativeCall(idx, target, method, result, slow, mark, vrInReg, 0, &sp);
	EndCold(cold, done);
	m_uc.bind(done);
}

// Emits the code of the called function in place, in a frame of its own that starts
// at the stack pointer, see CJITByteCode::FindInlinees. It executes like after
// asCContext::CallScriptFunction when the call state can be pushed without growing
// the call stack, the stack block has room for the function, and the VM has nothing
// to do. The function is called otherwise. Where it must return to the VM, the exit
// of its frame pushes the call state, see EmitInlineExit, and where it calls
// functions, its frame is materialized, see EmitMaterialize. A method called through
// asBC_CALLINTF is inlined for the objects of the classes that implement it the same
// way, or that the call has seen, each checked for and emitted once per
// implementation, or of the classes that inherit the method, and the others call the
// method. The objects of other classes than those seen are noted in the profile, and
// count down its calls. A call through asBC_CallPtr is inlined for the functions that
// the pointer may hold, see CJITByteCode::FindPointerTargets, each checked for, and
// the other functions are called, and noted like the classes. The calls in the inlined functions only check the class, as
// the outermost call has checked the room for them, and that the VM has nothing to
// do if the functions called before may have given it something. The VM makes the
// call otherwise
void CJITCodeGen::EmitInlineCall(asUINT idx)
{
	const std::vector<SJITInlinee> &inlinees = m_code->GetInlinees(idx);
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	bool virtualCall = instr.op == asBC_CALLINTF;
	bool pointerCall = instr.op == asBC_CallPtr;
	// The implementations of a method take the same arguments, which is all that is
	// needed of the function until one is chosen
	asCScriptFunction *func = inlinees[0].code->GetFunction();
	int base = -StackOffset(idx) / 4;
	int caller = m_frame;
	asUINT borrowed = m_code->GetBorrowedArgs(idx);
	// The object may have been lent a reference, see CJITByteCode::FindLentRefs
	int lentFree = caller == 0 ? m_code->GetLentFree(idx) : -1;
	// The object that the function returns may be created in the frame, see
	// EmitFrameObject
	int freshSlot = FreshSlot(idx);
	// The parameters are passed in the registers that they have been pushed from, and
	// the object returned in a register for the asBC_STOREOBJ after the call
	std::map<int, SPushedValue> args;
	args.swap(m_pushedAtCall);
	const std::vector<SJITInstr> &callerInstrs = m_code->GetInstructions();
	asUINT reader = idx + 1;
	while( reader < callerInstrs.size() && callerInstrs[reader].op == asBC_JitEntry )
		reader++;
	Gp returnedObj;
	if( reader < callerInstrs.size() && m_code->GetObjectSource(reader) == int(idx) )
		returnedObj = ForwardedObject(caller, reader);
	bool checkVM = caller == 0 || m_code->HasSyncPoints(m_frames[caller].borrowed);
	if( m_options.elideSuspend && m_suspendChecked )
		checkVM = false;

	Label call;
	if( caller == 0 )
		call = m_uc.new_label();
	else if( virtualCall || pointerCall || checkVM )
		call = BailLabel(idx);

	// The function called instead gets its own references for the arguments that
	// borrow them, see CJITByteCode::AnalyseBorrows
	bool ownArgs = borrowed && call.is_valid();
	Label own = ownArgs ? m_uc.new_label() : call;
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	Gp type;
	Gp target;
	if( pointerCall )
		target = LoadPtr(asBC_SWORDARG1(instr.bc));
	if( virtualCall )
	{
		// The call raises the exception for a null object
		type = m_uc.new_gp_ptr();
		m_uc.load(type, Stack(0));
		m_uc.j(own, test_z(type));
		m_uc.load(type, Addr(type, layout.objectType));
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
		const SJITContextLayout &ctxLayout = JIT_GetContextLayout();
		int extent, depth;
		GetInlineRoom(idx, extent, depth);
		Imm words = Imm(depth * int(ctxLayout.callStackFrameSize));
		if( m_inlineRoom.is_valid() )
			m_uc.j(own, ucmp_lt(m_inlineRoom, words));
		else
		{
			Gp length = m_uc.new_gp32();
			Gp room   = m_uc.new_gp32();
			m_uc.load_u32(length, ContextField(ctxLayout.callStackLength));
			m_uc.load_u32(room, ContextField(ctxLayout.callStackCapacity));
			m_uc.sub(room, room, length);
			m_uc.j(own, ucmp_lt(room, words));
			EmitStackBlockCheck(extent, own);
		}
	}

	// Which implementation the object calls. The first class of the first one falls
	// through to its code, the others jump to theirs
	std::vector<Label> bodies(inlinees.size());
	if( virtualCall )
	{
		if( inlinees.size() == 1 && inlinees[0].types.empty() )
		{
			// The classes that inherit the method have it in the same place of their
			// tables as the class declaring it
			asCScriptFunction *method = func->engine->scriptFunctions[asBC_INTARG(instr.bc)];
			Gp impl = m_uc.new_gp_ptr();
			m_uc.load(impl, Addr(type, layout.virtualFunctionTable));
			m_uc.load(impl, Addr(impl, method->vfTableIdx * PTR_BYTES));
			m_uc.j(own, cmp_ne(impl, PtrConst(asPWORD(func))));
		}
		else
		{
			for( asUINT k = 1; k < inlinees.size(); k++ )
			{
				bodies[k] = m_uc.new_label();
				for( asUINT t = 0; t < inlinees[k].types.size(); t++ )
					m_uc.j(bodies[k], cmp_eq(type, PtrConst(asPWORD(inlinees[k].types[t]))));
			}
			const std::vector<asCObjectType*> &first = inlinees[0].types;
			if( first.size() > 1 )
			{
				bodies[0] = m_uc.new_label();
				for( asUINT t = 0; t + 1 < first.size(); t++ )
					m_uc.j(bodies[0], cmp_eq(type, PtrConst(asPWORD(first[t]))));
			}
			Label other = m_uc.new_label();
			m_uc.j(other, cmp_ne(type, PtrConst(asPWORD(first.back()))));

			// An object of another class calls the method, and is noted if the call
			// keeps a profile, which counts down its calls then
			BaseNode *cold = BeginCold(other);
			SJITSeenClasses *seen = ProfileCell(idx);
			if( seen )
			{
				EmitNoteClass(seen, type);
				m_uc.j(own, scmp_gt(EmitCountDown(), Imm(0)));
				EmitRecompile(idx);
				m_callsProfiled++;
			}
			EndCold(cold, own);
		}
	}
	else if( pointerCall )
	{
		// Which function the pointer holds. Any other is called, after it is noted if
		// the call keeps a profile, which counts down its calls then. The VM raises the
		// exception for a null pointer
		for( asUINT k = 1; k < inlinees.size(); k++ )
		{
			bodies[k] = m_uc.new_label();
			m_uc.j(bodies[k], cmp_eq(target, PtrConst(asPWORD(inlinees[k].code->GetFunction()))));
		}
		SJITSeenFunctions *seen = FunctionProfileCell(idx);
		if( seen )
		{
			Label other = m_uc.new_label();
			m_uc.j(other, cmp_ne(target, PtrConst(asPWORD(func))));
			BaseNode *cold = BeginCold(other);
			m_uc.j(own, test_z(target));
			EmitNoteFunction(seen, target);
			m_uc.j(own, scmp_gt(EmitCountDown(), Imm(0)));
			EmitRecompile(idx);
			m_callsProfiled++;
			EndCold(cold, own);
		}
		else
			m_uc.j(own, cmp_ne(target, PtrConst(asPWORD(func))));
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

	// The code of each implementation, which continue together
	Label join = m_uc.new_label();
	bool suspendBefore = m_suspendChecked;
	bool retChecked = true;
	bool inlineCalls = false;
	for( asUINT k = 0; k < inlinees.size(); k++ )
	{
		if( bodies[k].is_valid() )
			m_uc.bind(bodies[k]);
		const CJITByteCode *code = inlinees[k].code.get();
		asCScriptFunction *impl = code->GetFunction();

		SFrame frame;
		frame.code     = code;
		frame.base     = base;
		frame.caller   = caller;
		frame.callIdx  = idx;
		frame.ret      = m_uc.new_label();
		frame.exit     = m_uc.new_label();
		frame.exitUsed = false;
		frame.borrowed = borrowed;
		frame.lentFree = lentFree;
		frame.freshSlot = freshSlot;
		frame.retChecked = true;
		frame.returnedObj = returnedObj;
		frame.args = args;
		m_frames.push_back(frame);
		int inlined = int(m_frames.size()) - 1;
		SwitchFrame(inlined);
		CreateCachedSlots();
		CreateBlockLabels();

		// Only the object variables on the heap are cleared, like in EmitDirectEntry
		const asCArray<asSScriptVariable*> &vars = impl->scriptData->variables;
		for( asUINT n = 0; n < vars.GetLength(); n++ )
			if( vars[n]->stackOffset > 0 && vars[n]->onHeap && (vars[n]->type.IsObject() || vars[n]->type.IsFuncdef()) )
				m_uc.store_zero_reg(Var(vars[n]->stackOffset));
		ReloadParamSlots(code->GetLiveInMask(0));

		// The function starts where the suspend requests are checked, if they may have
		// been made, see EmitBody
		m_suspendChecked = checkVM || suspendBefore;
		std::vector<bool> calls(code->GetInstructions().size());
		EmitBody(calls);
		m_uc.bind(m_frames[inlined].ret);
		SwitchFrame(caller);
		retChecked = retChecked && m_frames[inlined].retChecked;
		for( asUINT n = 0; n < calls.size(); n++ )
			inlineCalls = inlineCalls || calls[n];
		if( k + 1 < inlinees.size() )
			m_uc.j(join);
	}
	m_uc.bind(join);
	m_inlineCalls = inlineCalls;
	m_callsInlined++;

	if( caller == 0 )
	{
		Label cont = m_uc.new_label();
		BaseNode *cold = BeginCold(call);
		m_spOffset = StackOffset(idx);
		// The method called instead may release anything, so the variable gets a
		// reference of its own, which the FREE after the call releases then
		if( lentFree >= 0 )
			OwnLentRef(asUINT(lentFree));
		if( virtualCall )
			EmitScriptCall(idx, JIT_CALL_INTERFACE, asBC_INTARG(instr.bc), 0);
		else if( pointerCall )
			EmitScriptCall(idx, JIT_CALL_PTR, 0, asBC_SWORDARG1(instr.bc));
		else
			EmitScriptCall(idx, JIT_CALL_SCRIPT, func->GetId(), 0);
		if( returnedObj.is_valid() )
			m_uc.load(returnedObj, RegsField(offsetof(asSVMRegisters, objectRegister)));

		// The VM goes on after the call if the function has set a line callback, so
		// that both paths continue where the suspend requests are checked
		if( m_options.elideSuspend )
		{
			Gp flag = m_uc.new_gp32();
			m_uc.load_u8(flag, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
			m_uc.j(BailLabel(idx + 1), test_nz(flag));
		}
		// The object goes on in its register, see asBC_STOREOBJ
		if( returnedObj.is_valid() )
			m_uc.store_zero_reg(RegsField(offsetof(asSVMRegisters, objectRegister)));
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

// Adds the frame of a constructor compiled in place, which starts below the object
// pushed at the stack depth, at the base in dwords below the frame pointer. The VM
// would push its call state at the level
static void AddConstructorRoom(const SJITConstructor *ctor, int base, int level, int &extent, int &depth)
{
	base += AS_PTR_SIZE;
	if( base + int(ctor->stackNeeded) > extent )
		extent = base + int(ctor->stackNeeded);
	if( level > depth )
		depth = level;
}

// Adds the frame of an inlined function, which starts at the base in dwords below
// the frame pointer, and the frames of the functions inlined into it, and of the
// constructors compiled in place in it, at the level below, see GetInlineRoom
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
		{
			const std::vector<SJITInlinee> &inlinees = code->GetInlinees(n);
			for( asUINT k = 0; k < inlinees.size(); k++ )
				AddInlineRoom(inlinees[k].code.get(), base + int(func->scriptData->variableSpace) + code->GetStackDepth(n), level + 1, extent, depth);
		}
		else if( instrs[n].flags & JIT_INSTR_CONSTRUCT )
			AddConstructorRoom(code->GetInPlaceConstructor(n), base + int(func->scriptData->variableSpace) + code->GetStackDepth(n), level + 1, extent, depth);
}

// The dwords below the frame pointer that the frames of the function inlined by the
// instruction and of the functions inlined into it reach, and the most call states
// that their exits push, which the VM pushes for the calls. For a constructor
// compiled in place that is the room for calling it
void CJITCodeGen::GetInlineRoom(asUINT idx, int &extent, int &depth) const
{
	extent = 0;
	depth  = 0;
	if( m_code->GetInstructions()[idx].flags & JIT_INSTR_CONSTRUCT )
	{
		AddConstructorRoom(m_code->GetInPlaceConstructor(idx), -StackOffset(idx) / 4, 1, extent, depth);
		return;
	}
	const std::vector<SJITInlinee> &inlinees = m_code->GetInlinees(idx);
	for( asUINT k = 0; k < inlinees.size(); k++ )
		AddInlineRoom(inlinees[k].code.get(), -StackOffset(idx) / 4, 1, extent, depth);
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
		StoreDirtySlots(m_code->GetDirtyMask(m_frames[f].callIdx).AndNot(JIT_FRAME_BIT));
		inlined.insert(inlined.begin(), f);
	}

	// The variable of the outermost function that holds a lent reference gets one of
	// its own, which the VM releases, see CJITByteCode::FindLentRefs
	if( m_frames[inlined[0]].lentFree >= 0 )
		OwnLentRef(asUINT(m_frames[inlined[0]].lentFree));
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
		SetRegsArg(call, 0);
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
		StoreDirtySlots(m_code->GetDirtyMask(m_frames[f].callIdx).AndNot(JIT_FRAME_BIT));
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
		StorePair(state, FramePointer(caller.base), PtrConst(asPWORD(caller.code->GetFunction())));
		StorePair(PtrAt(state, 2), PtrConst(asPWORD(caller.code->GetByteCode() + instr.pos + instr.size)), FramePointer(callee.base));
#ifdef JIT_NATIVE_RETURN
		StorePair(PtrAt(state, 4), index, Gp());
#else
		m_uc.store(PtrAt(state, 4), index);
#endif
		if( callee.borrowed )
		{
			// The upper half of the stack index, or the first word that ordinary call
			// states leave unused on 32bit hosts, notes the borrowed parameters, see
			// JIT_OwnBorrowed.
#ifdef JIT_NATIVE_RETURN
			Mem mask = PtrAt(state, 4);
			mask.add_offset(4);
			StoreImm32(mask, int(callee.borrowed));
#else
			StoreImm32(PtrAt(state, 5), int(callee.borrowed));
#endif
			borrowed = true;
		}
#ifndef JIT_NATIVE_RETURN
		else
			StoreImm32(PtrAt(state, 5), 0);
#endif
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
CJITCodeGen::Gp CJITCodeGen::EmitFindMethod(asCScriptFunction *method, const Label &slow, SJITSeenClasses *seen)
{
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	const uint32_t ptrShift = Is64Bit() ? 3 : 2;
	Gp type = m_uc.new_gp_ptr();
	m_uc.load(type, Stack(0));
	m_uc.j(slow, test_z(type));
	m_uc.load(type, Addr(type, layout.objectType));
	if( seen )
	{
		// Most calls see the class noted first
		Gp cell = PtrConst(asPWORD(seen));
		Label other = m_uc.new_label();
		Label cont  = m_uc.new_label();
		m_uc.j(other, cmp_ne(type, mem_ptr(cell)));
		BaseNode *cold = BeginCold(other);
		EmitNoteClass(seen, type);
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

// Notes the class of the object in the classes seen by the call, in the first place
// that is free, unless it is noted already or there is no place left. Another thread
// may note a class in the same place at the same time, which loses one of them
void CJITCodeGen::EmitNoteClass(SJITSeenClasses *seen, const Gp &type)
{
	Gp cell  = PtrConst(asPWORD(seen));
	Gp noted = m_uc.new_gp_ptr();
	Label done = m_uc.new_label();
	for( asUINT n = 0; n < JIT_PROFILE_CLASSES; n++ )
	{
		Label next = m_uc.new_label();
		m_uc.load(noted, Addr(cell, int(n * PTR_BYTES)));
		m_uc.j(done, cmp_eq(noted, type));
		m_uc.j(next, test_nz(noted));
		m_uc.store(Addr(cell, int(n * PTR_BYTES)), type);
		m_uc.j(done);
		m_uc.bind(next);
	}
	m_uc.bind(done);
}

// Notes the function pointer seen by a call in the first place that is free,
// unless it is noted already or there is no place left. Another thread may note a
// function in the same place at the same time, which loses one of them
void CJITCodeGen::EmitNoteFunction(SJITSeenFunctions *seen, const Gp &func)
{
	Gp cell  = PtrConst(asPWORD(seen));
	Gp noted = m_uc.new_gp_ptr();
	Label done = m_uc.new_label();
	for( asUINT n = 0; n < JIT_PROFILE_FUNCTIONS; n++ )
	{
		Label next = m_uc.new_label();
		m_uc.load(noted, Addr(cell, int(n * PTR_BYTES)));
		m_uc.j(done, cmp_eq(noted, func));
		m_uc.j(next, test_nz(noted));
		m_uc.store(Addr(cell, int(n * PTR_BYTES)), func);
		m_uc.j(done);
		m_uc.bind(next);
	}
	m_uc.bind(done);
}

// Returns where the call notes the classes that it sees, if it is marked with
// JIT_INSTR_PROFILE and the code has a profile, else null
SJITSeenClasses *CJITCodeGen::ProfileCell(asUINT idx)
{
	if( m_options.profile == 0 || !(m_code->GetInstructions()[idx].flags & JIT_INSTR_PROFILE) )
		return 0;
	return &m_options.profile->classes[std::make_pair(m_code->GetFunction(), idx)];
}

// Returns where the call notes the functions that it sees, if it is marked with
// JIT_INSTR_PROFILE and the code has a profile, else null
SJITSeenFunctions *CJITCodeGen::FunctionProfileCell(asUINT idx)
{
	if( m_options.profile == 0 || !(m_code->GetInstructions()[idx].flags & JIT_INSTR_PROFILE) )
		return 0;
	return &m_options.profile->functions[std::make_pair(m_code->GetFunction(), idx)];
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
// the call. The call is made here otherwise. The function that the VM calls releases
// its handle arguments, so those lent to the call get references of their own first
void CJITCodeGen::EmitRecompile(asUINT idx)
{
	InvokeNode *call = Invoke(m_options.recompile, FuncSignature::build<int, SJITProfile*>());
	Gp r = m_uc.new_gp32();
	call->set_arg(0, Imm(int64_t(asPWORD(m_options.profile))));
	call->set_ret(0, r);
	asUINT borrowed = m_code->GetBorrowedArgs(idx);
	if( borrowed == 0 )
	{
		m_uc.j(BailLabel(idx), test_nz(r));
		return;
	}

	const SJITInstr &instr = m_code->GetInstructions()[idx];
	asCScriptFunction *func = m_code->GetFunction();
	asCScriptFunction *callee = instr.op == asBC_CallPtr ? func->GetCalledFunction(instr.pos) : func->engine->scriptFunctions[asBC_INTARG(instr.bc)];
	Label cont = m_uc.new_label();
	m_uc.j(cont, test_z(r));
	Gp frame = FramePointer(-StackOffset(idx) / 4);
	InvokeNode *own = Invoke((const void*)JIT_OwnParams, FuncSignature::build<void, asCScriptFunction*, asDWORD*, asUINT>());
	own->set_arg(0, Imm(int64_t(asPWORD(callee))));
	own->set_arg(1, frame);
	own->set_arg(2, Imm(int(borrowed)));
	m_uc.j(BailLabel(idx));
	m_uc.bind(cont);
}

// Pushes the call state like asCContext::PushCallState and calls the native code
// of a script function, or this function if target isn't valid, and leaves to the
// VM if it must take over. Jumps to slow if the call stack has reached the call
// limit, which is passed on. On 64bit hosts the call state may be marked
// with the sign bit of the stack index, so that the function doesn't restore the
// frame, the registers, and the length of the call stack, see JITFunction. The
// length is restored here then, from the one held across the call, so that the
// next call doesn't wait for the function to read back what was just stored for
// it. It returns the value register too, which is taken if vrInReg is set and the
// value register is live, and true is returned then. In interop mode the current
// function of the context is set to callee, and the call state isn't marked, see
// JITFunction
bool CJITCodeGen::EmitNativeCall(asUINT idx, const Gp &target, const Gp &callee, const Gp &result, const Label &slow, bool mark, bool vrInReg, asUINT borrowed, const Gp *stackPointer)
{
	if( FailIfHidden() )
		return false;
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	const SJITContextLayout &layout = JIT_GetContextLayout();
	// The length of the call stack is the one the function was entered with, plus
	// the frames of the inlined calls if they are on the call stack: the called
	// functions and the helpers pop what they push, and the VM enters the function
	// anew after it has executed anything else, see Generate. So the length is
	// kept in a register all through the function instead of being loaded here and
	// held across the call, where it would be saved and restored on the stack
	Gp length = m_uc.new_gp_ptr();
	Gp t      = m_uc.new_gp_ptr();
	CallStackLength(length.r32());
	m_uc.j(slow, ucmp_ge(length.r32(), m_callLimit));
	if( m_options.interop )
	{
		m_uc.store(ContextField(layout.currentFunction), callee);
		mark = false;
	}
	Gp array = m_uc.new_gp_ptr();
	m_uc.load(array, ContextField(layout.callStackArray));
	Mem state = PtrElement(array, length);
	StorePair(state, FramePointer(m_frameBase), PtrConst(asPWORD(m_code->GetFunction())));
	// The stack pointer passed to the callee is the current one, unless the caller
	// has pushed something below it, see EmitDelegateCall
	Gp sp = stackPointer ? *stackPointer : StackPointer();
	StorePair(PtrAt(state, 2), PtrConst(asPWORD(m_code->GetByteCode() + instr.pos + asBCTypeSize[asBCInfo[instr.op].type])), sp);
	m_uc.load_u32(t, ContextField(layout.stackIndex));
#ifdef JIT_NATIVE_RETURN
	if( mark )
		SetSignBit(t);
#else
	(void)mark;
#endif
	// Engine call states leave words 5-8 unused. The mark distinguishes this state
	// from one the VM created, and the mask is read by the callee's direct entry.
	Gp note = m_uc.new_gp_ptr();
	m_uc.mov(note, Imm(asPWORD(JIT_NATIVE_CALL_STATE | (borrowed & JIT_BORROWED_ARG_MASK))));
	StorePair(PtrAt(state, 4), t, note);
	Gp top = m_uc.new_gp_ptr();
	m_uc.add(top, length, Imm(layout.callStackFrameSize));
	m_uc.store_u32(ContextField(layout.callStackLength), top);

	FuncSignature sig = m_spInArg ? FuncSignature::build<int, asSVMRegisters*, asPWORD, asUINT, asDWORD*>() :
	                                FuncSignature::build<int, asSVMRegisters*, asPWORD, asUINT>();
	InvokeNode *call = 0;
	if( target.is_valid() )
		m_uc.cc->invoke(Out(call), target, sig);
	else
		m_uc.cc->invoke(Out(call), m_uc.cc->func()->label(), sig);
	SetRegsArg(call, 0);
	call->set_arg(1, Imm(0));
	call->set_arg(2, m_callLimit);
	if( m_spInArg )
		call->set_arg(3, sp);
	call->set_ret(0, result);
	bool vrReturned = false;
#ifdef JIT_NATIVE_RETURN
	if( mark )
	{
		AddVRReturn(call->detail());
		if( vrInReg && m_vr.is_valid() && m_code->IsVRLiveAfter(idx) )
		{
			call->set_ret(1, m_vr);
			vrReturned = true;
		}
	}
#else
	(void)vrInReg;
#endif
	EmitLeaveIf(result);
#ifdef JIT_NATIVE_RETURN
	if( mark )
	{
		Gp restored = m_uc.new_gp32();
		CallStackLength(restored);
		m_uc.store_u32(ContextField(layout.callStackLength), restored);
	}
#endif
	return vrReturned;
}

// The length of the call stack at this point of the function, see EmitNativeCall
void CJITCodeGen::CallStackLength(const Gp &dst)
{
	const SJITContextLayout &layout = JIT_GetContextLayout();
	if( m_materialized )
		m_uc.add(dst, m_callStackLength, Imm(m_materialDepth * int(layout.callStackFrameSize)));
	else
		m_uc.mov(dst, m_callStackLength);
}

// The direct entry may have borrowed handle parameters from a non-inlined caller.
// Before anything can expose this frame to the VM, give the parameters references
// of their own and clear the note so exception unwinding can't count them twice.
void CJITCodeGen::OwnDirectBorrowed()
{
	if( !m_directBorrowed.is_valid() )
		return;
	Label done = m_uc.new_label();
	m_uc.j(done, test_z(m_directBorrowed));
	Gp note = m_uc.new_gp_ptr();
	m_uc.mov(note, Imm(asPWORD(JIT_NATIVE_CALL_STATE)));
	m_uc.store(mem_ptr(m_borrowState), note);
	InvokeNode *call = Invoke((const void*)JIT_OwnParams, FuncSignature::build<void, asCScriptFunction*, asDWORD*, asUINT>());
	call->set_arg(0, Imm(int64_t(asPWORD(m_frames[0].code->GetFunction()))));
	call->set_arg(1, m_fp);
	call->set_arg(2, m_directBorrowed);
	m_uc.mov(m_directBorrowed, Imm(0));
	m_uc.bind(done);
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
	if( m_directBorrowed.is_valid() )
	{
		Label noBorrow = m_uc.new_label();
		Gp length = m_uc.new_gp_ptr();
		Gp array  = m_uc.new_gp_ptr();
		Gp note   = m_uc.new_gp32();
		m_uc.mov(m_directBorrowed, Imm(0));
		m_uc.mov(m_borrowState, Imm(0));
		m_uc.load_u32(length, ContextField(layout.callStackLength));
		m_uc.j(noBorrow, ucmp_lt(length, Imm(layout.callStackFrameSize)));
		m_uc.sub(length, length, Imm(layout.callStackFrameSize));
		m_uc.load(array, ContextField(layout.callStackArray));
		Mem state = PtrElement(array, length);
		Gp caller = m_uc.new_gp_ptr();
		m_uc.load(caller, PtrAt(state, 0));
		m_uc.j(noBorrow, test_z(caller));
		m_uc.load_u32(note, PtrAt(state, 5));
		m_uc.j(noBorrow, test_z(note, Imm(JIT_NATIVE_CALL_STATE)));
		m_uc.and_(m_directBorrowed, note, Imm(int(m_code->GetBorrowableParams())));
		Lea(m_borrowState, PtrAt(state, 5));
		m_uc.bind(noBorrow);
	}
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
	if( m_callStackLength.is_valid() )
		m_uc.load_u32(m_callStackLength, ContextField(layout.callStackLength));
	EmitFrameArena();
	ReloadThis();
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
	OwnDirectBorrowed();
	InvokeNode *call = Invoke((const void*)JIT_PrepareFrame, FuncSignature::build<int, asSVMRegisters*>());
	Gp r = m_uc.new_gp32();
	SetRegsArg(call, 0);
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
		if( instr.flags & JIT_INSTR_ASSIGN )
		{
			EmitInPlaceAssign(idx);
			break;
		}
		if( instr.flags & JIT_INSTR_INDEXER )
		{
			EmitIndexer(idx);
			break;
		}
		if( instr.flags & JIT_INSTR_MATH )
		{
			EmitMathCall(idx);
			break;
		}
		if( m_options.directSystemCalls && EmitDirectSystemCall(idx, asBC_INTARG(bc)) )
			break;
		{
			// The interface that the generic functions get reads the information,
			// which is kept as long as the code
			SJITGenericInfo *generic = 0;
			if( m_options.genericInfos )
			{
				asCScriptEngine *engine = m_code->GetFunction()->engine;
				generic = JIT_CreateGenericInfo(engine->scriptFunctions[asBC_INTARG(bc)]);
				if( generic )
					m_options.genericInfos->push_back(generic);
			}
			if( generic && EmitGenericCall(idx, generic) )
				break;
			SyncForCall(idx);
			InvokeNode *call;
			if( generic )
			{
				call = Invoke((const void*)JIT_CallGeneric, FuncSignature::build<int, asSVMRegisters*, const SJITGenericInfo*>());
				call->set_arg(1, Imm((asPWORD)generic));
			}
			else
			{
				call = Invoke((const void*)JIT_CallSystem, FuncSignature::build<int, asSVMRegisters*, int>());
				call->set_arg(1, Imm(asBC_INTARG(bc)));
			}
			Gp r = m_uc.new_gp32();
			SetRegsArg(call, 0);
			call->set_ret(0, r);
			EmitAfterHelperCall(r, idx);
		}
		break;

	case asBC_Thiscall1:
		// The instruction is a CALLSYS for methods taking an int and returning a reference
		if( instr.flags & JIT_INSTR_INDEXER )
		{
			EmitIndexer(idx);
			break;
		}
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
			SetRegsArg(call, 0);
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
			EmitScriptCall(idx, JIT_CALL_SCRIPT, asBC_INTARG(bc), 0);
		break;

	case asBC_CALLINTF:
		if( m_options.noScriptCalls )
			Bail(idx);
		else if( instr.flags & JIT_INSTR_INLINE )
			EmitInlineCall(idx);
		else
		{
			// The calls that note their receiver classes count down the calls for the profile
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
			EmitScriptCall(idx, JIT_CALL_INTERFACE, asBC_INTARG(bc), 0);
		}
		break;

	case asBC_CALLBND:
		if( m_options.noScriptCalls )
			Bail(idx);
		else
			EmitScriptCall(idx, JIT_CALL_BOUND, asBC_INTARG(bc), 0);
		break;

	case asBC_CallPtr:
		if( m_options.noScriptCalls )
			Bail(idx);
		else if( instr.flags & JIT_INSTR_INLINE )
			EmitInlineCall(idx);
		else
		{
			if( FunctionProfileCell(idx) )
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
			EmitScriptCall(idx, JIT_CALL_PTR, 0, asBC_SWORDARG1(bc));
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
			if( m_callStackLength.is_valid() )
				CallStackLength(length.r32());
			else
				m_uc.load_u32(length, ContextField(layout.callStackLength));
			m_uc.j(finish, sub_c(length, Imm(layout.callStackFrameSize)));
			m_uc.load(array, ContextField(layout.callStackArray));
			Mem state = PtrElement(array, length);
#ifdef JIT_NATIVE_RETURN
			{
				// Native callers keep their frame, and set the program pointer, the
				// stack pointer, and the length of the call stack themselves, see
				// EmitNativeCall, so only the stack index is restored for them.
				// They get the value register in the second return register
				Label vm = m_uc.new_label();
				Gp index = m_uc.new_gp_ptr();
				m_uc.load(index, PtrAt(state, 4));
				m_uc.j(vm, scmp_ge(index, Imm(0)));
				m_uc.store_u32(ContextField(layout.stackIndex), index);
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
			if( m_code->GetDirtyMask(idx).Test(JIT_FRAME_SLOT) )
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

// Computes the math function of the C library in place, see
// CJITByteCode::FindMathCalls. The argument is taken from the register it was pushed
// from, like for the direct calls, and so is the result set to the value register.
// Without the instructions to round the function is called, which doesn't look at
// the context
void CJITCodeGen::EmitMathCall(asUINT idx)
{
	const SJITMathCall &math = *m_code->GetMathCall(idx);
	Vec arg = math.f64 ? m_uc.new_vec128_f64x1() : m_uc.new_vec128_f32x1();
	std::map<int, SPushedValue>::const_iterator pushed = m_staticStack ? m_pushed.find(m_spOffset) : m_pushed.end();
	if( math.f64 )
	{
		if( pushed != m_pushed.end() && pushed->second.kind == SPushedValue::F64 )
			MoveVec(arg, pushed->second.vec);
		else
			m_uc.v_loadu64_f64(arg, Stack(0));
	}
	else if( pushed == m_pushed.end() || !MovePushedF32(arg, pushed->second) )
		m_uc.v_loadu32_f32(arg, Stack(0));

	Vec ret = math.f64 ? m_uc.new_vec128_f64x1() : m_uc.new_vec128_f32x1();
	bool rounding = math.op == JIT_MATH_FLOOR || math.op == JIT_MATH_CEIL;
	if( rounding && !CanRoundInPlace() )
	{
		InvokeNode *call = Invoke((const void*)math.func, math.f64 ? FuncSignature::build<double, double>() : FuncSignature::build<float, float>());
		call->set_arg(0, arg);
		call->set_ret(0, ret);
	}
	else switch( math.op )
	{
	case JIT_MATH_SQRT:
		if( math.f64 ) m_uc.s_sqrt_f64(ret, arg); else m_uc.s_sqrt_f32(ret, arg);
		break;
	case JIT_MATH_ABS:
		if( math.f64 ) m_uc.s_abs_f64(ret, arg); else m_uc.s_abs_f32(ret, arg);
		break;
	case JIT_MATH_FLOOR:
		if( math.f64 ) m_uc.s_floor_f64(ret, arg); else m_uc.s_floor_f32(ret, arg);
		break;
	case JIT_MATH_CEIL:
		if( math.f64 ) m_uc.s_ceil_f64(ret, arg); else m_uc.s_ceil_f32(ret, arg);
		break;
	}
	PopStack(math.f64 ? 8 : 4);

	if( !m_code->IsVRLiveAfter(idx) )
		return;
	if( !math.f64 )
	{
		Gp bits = m_uc.new_gp32();
		m_uc.s_mov_u32(bits, ret);
		StoreVR32(bits);
	}
	else if( Is64Bit() )
	{
		Gp bits = m_uc.new_gp64();
		m_uc.s_mov_u64(bits, ret);
		StoreVR64(bits);
	}
	else
		StoreVRF64(ret);
}

// The indexers are compiled in place as the loads of the address of the element, see
// CJITCompiler::AddIndexer. A null object or buffer, or an index out of range, is
// left to the VM, which calls the method to raise the exception. Nothing else can
// see the variables, so they aren't stored, see CJITByteCode::IsSyncPointAt
void CJITCodeGen::EmitIndexer(asUINT idx)
{
	const SJITIndexerCall &indexer = *m_code->GetIndexer(idx);
	Label bail = BailLabel(idx);
	if( indexer.op != JIT_CONTAINER_INDEX )
	{
		EmitLengthQuery(idx, indexer);
		return;
	}
	Gp obj   = m_uc.new_gp_ptr();
	Gp index = m_uc.new_gp_ptr();
	Gp buf   = m_uc.new_gp_ptr();
	Gp count = m_uc.new_gp32();
	m_uc.load(obj, Stack(0));
	m_uc.load_u32(index, Stack(AS_PTR_SIZE));
	m_uc.j(bail, test_z(obj));
	m_uc.load(buf, Addr(obj, indexer.layout.bufferOffset));
	m_uc.j(bail, test_z(buf));
	m_uc.load_u32(count, Addr(buf, indexer.layout.lengthOffset));
	m_uc.j(bail, ucmp_ge(index.r32(), count));

	Gp elem = m_uc.new_gp_ptr();
	m_uc.add_ext(elem, buf, index, asUINT(indexer.elementSize), indexer.layout.dataOffset);
	if( indexer.indirect )
		m_uc.load(elem, mem_ptr(elem));

	// Pop the object and the index, and set the value register like the VM does. The
	// next instruction may read or write the element through the address itself
	PopStack((AS_PTR_SIZE + 1) * 4);
	if( m_code->IsVRLiveAfter(idx) && CanFoldVRAddr(idx) )
		SetVRAddr(idx, mem_ptr(elem));
	else if( m_code->IsVRLiveAfter(idx) )
		StoreVRPtr(elem);
}

// Where the object of the variable of the outermost function is kept in the memory
// for the objects in its frame, or -1, see CJITByteCode::FindFrameObjects
int CJITCodeGen::FrameSlot(int var) const
{
	if( var == 0 || m_options.frameSlots == 0 )
		return -1;
	std::map<int, int>::const_iterator it = m_options.frameSlots->find(var);
	return it == m_options.frameSlots->end() ? -1 : it->second;
}

// Where the object created by the asBC_ALLOC, or returned by the call compiled in
// place, at the instruction is kept, or -1
int CJITCodeGen::FreshSlot(asUINT idx) const
{
	if( m_frame == 0 )
		return FrameSlot(m_code->GetFreshVar(idx));
	int slot = m_frames[m_frame].freshSlot;
	return slot >= 0 && m_code->GetFreshSource() == int(idx) ? slot : -1;
}

// Gets the memory for the objects kept in the frame when the function is entered,
// which stays the same while it executes, see JIT_GetFrameArena
void CJITCodeGen::EmitFrameArena()
{
	if( !m_objectArena.is_valid() )
		return;
	InvokeNode *call = Invoke((const void*)JIT_GetFrameArena, FuncSignature::build<void*, asSVMRegisters*, asUINT>());
	SetRegsArg(call, 0);
	call->set_arg(1, Imm(int(m_options.frameObjectSize)));
	call->set_ret(0, m_objectArena);

	// The parts of the headers of the objects that never change, the table of
	// virtual functions and the class, are written once the function is entered, as
	// the other functions at the same depth of the call stack use the memory too
	Label none = m_uc.new_label();
	m_uc.j(none, test_z(m_objectArena));
	const std::map<int, asCObjectType*> &objects = m_code->GetFrameObjects();
	for( std::map<int, asCObjectType*>::const_iterator it = objects.begin(); it != objects.end(); ++it )
	{
		int slot = FrameSlot(it->first);
		if( slot >= 0 )
			StoreFrameObjectHeader(m_objectArena, slot, it->second, false);
	}
	m_uc.bind(none);
}

// Whether the bytes of the header of a script object may change while it is kept in
// the frame: the reference count and the flags beside it, which the VM changes
static bool IsHeaderChanging(int offset, int size)
{
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	int bytes[] = { layout.refCount, layout.refCount + 3, layout.gcFlag, layout.deadFlag };
	for( asUINT n = 0; n < sizeof(bytes) / sizeof(bytes[0]); n++ )
		if( bytes[n] >= offset && bytes[n] < offset + size )
			return true;
	return false;
}

// Writes the parts of the header of an object kept in the frame that may change, or
// those that don't, see EmitFrameObject
void CJITCodeGen::StoreFrameObjectHeader(const Gp &base, int offset, asCObjectType *type, bool changing)
{
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	std::vector<asBYTE> header(size_t(layout.scriptObjectSize));
	JIT_GetFrameObjectHeader(type, &header[0]);
	Gp t = Is64Bit() ? m_uc.new_gp64() : m_uc.new_gp32();
	for( int n = 0, width; n + 4 <= layout.scriptObjectSize; n += width )
	{
		width = Is64Bit() && n + 8 <= layout.scriptObjectSize ? 8 : 4;
		if( IsHeaderChanging(n, width) != changing )
			continue;
		asQWORD value = 0;
		memcpy(&value, &header[size_t(n)], size_t(width));
		if( value == 0 && width == 8 )
			m_uc.store_zero_reg(Addr(base, offset + n));
		else if( value == 0 )
			m_uc.store_zero_u32(Addr(base, offset + n));
		else if( width == 8 )
		{
			m_uc.mov(t, Imm(int64_t(value)));
			m_uc.store_u64(Addr(base, offset + n), t);
		}
		else
		{
			m_uc.mov(t.r32(), Imm(int(asDWORD(value))));
			m_uc.store_u32(Addr(base, offset + n), t.r32());
		}
	}
}

// Creates an object of a plain script class in the memory for the objects kept in the
// frame of the outermost function instead of allocating it, as it never leaves the
// function, see CJITByteCode::FindFrameObjects and JIT_GetFrameArena. The header is
// that of a new object, with a reference more, so that the releases by the VM, which
// goes on with the objects like with any other, never free it, and the members are
// cleared like ScriptObject_Construct does. Only the parts of the header that the VM
// may have changed are written here, the others when the function is entered, see
// EmitFrameArena. The variable the object ends up in isn't released by the generated
// code, which only forgets the object, and the next object for it takes its place
void CJITCodeGen::EmitFrameObject(const Gp &obj, int slot, asCObjectType *type)
{
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	Label heap = m_uc.new_label();
	Label done = m_uc.new_label();
	m_uc.j(heap, test_z(m_objectArena));
	m_uc.add(obj, m_objectArena, Imm(slot));
	StoreFrameObjectHeader(obj, 0, type, true);
	int size = int(type->size);
	Gp t = m_uc.new_gp32();
	int offset = layout.scriptObjectSize;
	if( Is64Bit() )
		for( ; offset + 8 <= size; offset += 8 )
			m_uc.store_zero_reg(Addr(obj, offset));
	for( ; offset + 4 <= size; offset += 4 )
		m_uc.store_zero_u32(Addr(obj, offset));
	for( ; offset < size; offset++ )
	{
		m_uc.mov(t.r32(), Imm(0));
		m_uc.store_u8(Addr(obj, offset), t.r32());
	}

	// Without the memory the object is allocated
	BaseNode *cold = BeginCold(heap);
	InvokeNode *alloc = Invoke((const void*)JIT_NewScriptObject, FuncSignature::build<void*, void*>());
	alloc->set_arg(0, Imm(int64_t(asPWORD(type))));
	alloc->set_ret(0, obj);
	EndCold(cold, done);
	m_uc.bind(done);
}

// Copies the members of an object of a plain script class to the other, which the
// default assignment does member by member, see CJITByteCode::FindInPlaceAssigns.
// Both objects must be of the class of the variable, otherwise the VM calls the
// assignment, which also raises the exception for a null object. Pops the objects
// and leaves the reference to the assigned one in the value register
void CJITCodeGen::EmitInPlaceAssign(asUINT idx)
{
	asCObjectType *type = m_code->GetInPlaceAssign(idx);
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	Label bail = BailLabel(idx);
	Gp self  = m_uc.new_gp_ptr();
	Gp other = m_uc.new_gp_ptr();
	Gp t     = m_uc.new_gp_ptr();
	Gp expected = PtrConst(asPWORD(type));
	m_uc.load(self, Stack(0));
	m_uc.load(other, Stack(AS_PTR_SIZE));
	m_uc.j(bail, test_z(self));
	m_uc.j(bail, test_z(other));
	m_uc.load(t, Addr(self, layout.objectType));
	m_uc.j(bail, cmp_ne(t, expected));
	m_uc.load(t, Addr(other, layout.objectType));
	m_uc.j(bail, cmp_ne(t, expected));

	// The members follow the object header. A copy onto itself changes nothing
	int start = layout.scriptObjectSize;
	int size  = int(type->size) - start;
	if( size > 128 )
	{
		Gp dst = m_uc.new_gp_ptr();
		Gp src = m_uc.new_gp_ptr();
		m_uc.add(dst, self, Imm(start));
		m_uc.add(src, other, Imm(start));
		InvokeNode *call = Invoke((const void*)memmove, FuncSignature::build<void*, void*, const void*, size_t>());
		call->set_arg(0, dst);
		call->set_arg(1, src);
		call->set_arg(2, Imm(int64_t(size)));
	}
	else
	{
		int offset = start;
		Gp v = Is64Bit() ? m_uc.new_gp64() : m_uc.new_gp32();
		if( Is64Bit() )
			for( ; size - (offset - start) >= 8; offset += 8 )
			{
				m_uc.load_u64(v, Addr(other, offset));
				m_uc.store_u64(Addr(self, offset), v);
			}
		for( ; size - (offset - start) >= 4; offset += 4 )
		{
			m_uc.load_u32(v.r32(), Addr(other, offset));
			m_uc.store_u32(Addr(self, offset), v.r32());
		}
		for( ; size - (offset - start) >= 2; offset += 2 )
		{
			m_uc.load_u16(v.r32(), Addr(other, offset));
			m_uc.store_u16(Addr(self, offset), v.r32());
		}
		for( ; size - (offset - start) >= 1; offset += 1 )
		{
			m_uc.load_u8(v.r32(), Addr(other, offset));
			m_uc.store_u8(Addr(self, offset), v.r32());
		}
	}

	PopStack(2 * PTR_BYTES);
	if( m_code->IsVRLiveAfter(idx) )
		StoreVRPtr(self);
}

// The register that passes the object stored by an instruction to the asBC_STOREOBJ
// or asBC_LOADOBJ reading it in the frame, see CJITByteCode::FindForwardedObjects.
// The object is still stored, for the VM, which enters the function in between only
// at an asBC_JitEntry, whose entry stub loads the register, see EmitEntryStubs
CJITCodeGen::Gp CJITCodeGen::ForwardedObject(int frame, asUINT reader)
{
	std::pair<int, asUINT> key(frame, reader);
	std::map<std::pair<int, asUINT>, Gp>::iterator it = m_forwardedObjects.find(key);
	if( it != m_forwardedObjects.end() )
		return it->second;
	Gp obj = m_uc.new_gp_ptr();
	m_forwardedObjects[key] = obj;
	return obj;
}

// The vector register that a float or double is returned in for the asBC_CpyRtoV4 or
// asBC_CpyRtoV8 copying it in the frame, see CJITByteCode::FindFloatReturns
CJITCodeGen::Vec CJITCodeGen::ReturnedFloat(int frame, asUINT reader, bool f64)
{
	std::pair<int, asUINT> key(frame, reader);
	std::map<std::pair<int, asUINT>, Vec>::iterator it = m_returnedFloats.find(key);
	if( it != m_returnedFloats.end() )
		return it->second;
	Vec v = f64 ? m_uc.new_vec128_f64x1() : m_uc.new_vec128_f32x1();
	m_returnedFloats[key] = v;
	return v;
}

// Loads the float or double in the value register into a vector register
void CJITCodeGen::LoadVRFloat(const Vec &v, bool f64)
{
	if( !f64 )
	{
		Gp bits = m_uc.new_gp32();
		LoadVR32(bits);
		m_uc.s_mov_u32(v, bits);
	}
	else if( Is64Bit() )
	{
		Gp bits = m_uc.new_gp64();
		LoadVR64(bits);
		m_uc.s_mov_u64(v, bits);
	}
	else
		LoadVRF64(v);
}

// Creates the object of a script class like asBC_ALLOC, in the frame if it is kept
// there, see EmitFrameObject, and sets its members like the constructor does, see
// CJITByteCode::FindInPlaceConstructors. The variable whose address is below the
// arguments gets the object, and the arguments are popped. Where the VM has something
// to do, like calling the line callback when the constructor is entered, or the call
// would grow the call stack or need another stack block, it executes the instruction
void CJITCodeGen::EmitInPlaceConstruct(asUINT idx)
{
	const SJITConstructor *ctor = m_code->GetInPlaceConstructor(idx);
	// The arguments pushed right before are taken from their registers, which spares
	// the loads waiting for the stores
	std::map<int, SPushedValue> pushed;
	pushed.swap(m_pushedAtCall);
	if( m_options.noScriptCalls )
	{
		Bail(idx);
		return;
	}
	Label bail = BailLabel(idx);
	if( !(m_options.elideSuspend && m_suspendChecked) )
	{
		Gp flag = m_uc.new_gp32();
		m_uc.load_u8(flag, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
		m_uc.j(bail, test_nz(flag));
	}
	if( m_frame == 0 )
	{
		// The VM would push the call state of the constructor, and enter its frame,
		// for which there must be room like for the inlined calls. In the inlined
		// functions the outermost call has checked the room for it
		const SJITContextLayout &ctxLayout = JIT_GetContextLayout();
		int extent, depth;
		GetInlineRoom(idx, extent, depth);
		Imm words = Imm(depth * int(ctxLayout.callStackFrameSize));
		if( m_inlineRoom.is_valid() )
			m_uc.j(bail, ucmp_lt(m_inlineRoom, words));
		else
		{
			Gp length = m_uc.new_gp32();
			Gp room   = m_uc.new_gp32();
			m_uc.load_u32(length, ContextField(ctxLayout.callStackLength));
			m_uc.load_u32(room, ContextField(ctxLayout.callStackCapacity));
			m_uc.sub(room, room, length);
			m_uc.j(bail, ucmp_lt(room, words));
			EmitStackBlockCheck(extent, bail);
		}
	}

	Gp obj = m_uc.new_gp_ptr();
	int slot = FreshSlot(idx);
	if( slot >= 0 )
		EmitFrameObject(obj, slot, ctor->type);
	else
	{
		InvokeNode *alloc = Invoke((const void*)JIT_NewScriptObject, FuncSignature::build<void*, void*>());
		alloc->set_arg(0, Imm(int64_t(asPWORD(ctor->type))));
		alloc->set_ret(0, obj);
	}

	Gp v = Is64Bit() ? m_uc.new_gp64() : m_uc.new_gp32();
	for( size_t n = 0; n < ctor->members.size(); n++ )
	{
		const SJITMemberInit &member = ctor->members[n];
		int offset = member.offset;
		std::map<int, SPushedValue>::const_iterator arg = member.arg >= 0 && m_staticStack ? pushed.find(m_spOffset + member.arg * 4) : pushed.end();
		if( arg != pushed.end() && member.size == 4 && arg->second.kind == SPushedValue::F32 )
		{
			m_uc.v_storeu32_f32(Addr(obj, offset), arg->second.vec);
			continue;
		}
		if( arg != pushed.end() && member.size == 8 && arg->second.kind == SPushedValue::F64 )
		{
			m_uc.v_storeu64_f64(Addr(obj, offset), arg->second.vec);
			continue;
		}
#ifdef AS_BIG_ENDIAN
		// The first bytes in memory are the high ones
		bool lowBytes = member.size == 4;
#else
		bool lowBytes = member.size <= 4;
#endif
		if( arg != pushed.end() && lowBytes && (arg->second.kind == SPushedValue::IMM32 || arg->second.kind == SPushedValue::GP32) )
		{
			// The member gets the first bytes of the value in memory, the low ones
			Gp value = v.r32();
			MovePushed32(value, arg->second);
			if( member.size == 1 )
				m_uc.store_u8(Addr(obj, offset), value);
			else if( member.size == 2 )
				m_uc.store_u16(Addr(obj, offset), value);
			else
				m_uc.store_u32(Addr(obj, offset), value);
			continue;
		}
		if( member.arg >= 0 )
		{
			if( member.size == 1 )
			{
				m_uc.load_u8(v.r32(), Stack(member.arg));
				m_uc.store_u8(Addr(obj, offset), v.r32());
			}
			else if( member.size == 2 )
			{
				m_uc.load_u16(v.r32(), Stack(member.arg));
				m_uc.store_u16(Addr(obj, offset), v.r32());
			}
			else if( member.size == 4 || !Is64Bit() )
			{
				for( asUINT k = 0; k < member.size / 4; k++ )
				{
					m_uc.load_u32(v.r32(), Stack(member.arg + int(k)));
					m_uc.store_u32(Addr(obj, offset + int(k) * 4), v.r32());
				}
			}
			else
			{
				m_uc.load_u64(v, Stack(member.arg));
				m_uc.store_u64(Addr(obj, offset), v);
			}
			continue;
		}

		// The constant is in the first bytes of the value, like in memory
		if( member.size == 1 || member.size == 2 )
		{
			asWORD value = 0;
			memcpy(&value, &member.value, member.size);
			m_uc.mov(v.r32(), Imm(int(value)));
			if( member.size == 1 )
				m_uc.store_u8(Addr(obj, offset), v.r32());
			else
				m_uc.store_u16(Addr(obj, offset), v.r32());
		}
		else if( member.size == 8 && Is64Bit() )
		{
			if( member.value == 0 )
				m_uc.store_zero_reg(Addr(obj, offset));
			else
			{
				m_uc.mov(v, Imm(int64_t(member.value)));
				m_uc.store_u64(Addr(obj, offset), v);
			}
		}
		else
		{
			asDWORD values[2] = { 0, 0 };
			memcpy(values, &member.value, member.size);
			for( asUINT k = 0; k < member.size / 4; k++ )
			{
				if( values[k] == 0 )
					m_uc.store_zero_u32(Addr(obj, offset + int(k) * 4));
				else
				{
					m_uc.mov(v.r32(), Imm(int(values[k])));
					m_uc.store_u32(Addr(obj, offset + int(k) * 4), v.r32());
				}
			}
		}
	}

	// The asBC_LOADOBJ of the variable takes the object from its register
	const std::vector<SJITInstr> &instrs = m_code->GetInstructions();
	asUINT reader = idx + 1;
	while( reader < instrs.size() && instrs[reader].op == asBC_JitEntry )
		reader++;
	if( reader < instrs.size() && m_code->GetObjectSource(reader) == int(idx) )
		m_uc.mov(ForwardedObject(m_frame, reader), obj);

	// The variable gets the object, which asBC_ALLOC stores before the constructor
	// is called
	Gp var = m_uc.new_gp_ptr();
	Label none = m_uc.new_label();
	m_uc.load(var, Stack(int(ctor->args)));
	m_uc.j(none, test_z(var));
	m_uc.store(mem_ptr(var), obj);
	m_uc.bind(none);
	PopStack(int(ctor->args) * 4);
}

// A length query pops the object and sets the value register to the number of
// elements, or to whether there are none, like the method returns them. A null
// object or buffer returns to the VM, which calls the method
void CJITCodeGen::EmitLengthQuery(asUINT idx, const SJITIndexerCall &query)
{
	Label bail = BailLabel(idx);
	Gp obj   = m_uc.new_gp_ptr();
	Gp buf   = m_uc.new_gp_ptr();
	Gp count = m_uc.new_gp32();
	m_uc.load(obj, Stack(0));
	m_uc.j(bail, test_z(obj));
	m_uc.load(buf, Addr(obj, query.layout.bufferOffset));
	m_uc.j(bail, test_z(buf));
	PopStack(AS_PTR_SIZE * 4);
	if( !m_code->IsVRLiveAfter(idx) )
		return;
	m_uc.load_u32(count, Addr(buf, query.layout.lengthOffset));
	if( query.op == JIT_CONTAINER_IS_EMPTY )
	{
		Gp empty = m_uc.new_gp32();
		m_uc.select(empty, Imm(1), Imm(0), test_z(count));
		StoreVR32(empty);
	}
	else
		StoreVR32(count);
}

// Calls a registered function directly with its native calling convention, instead
// of through the generic CallSystemFunction of the engine. This is only done for
// simple signatures: primitives and pointers as arguments, a primitive, pointer,
// handle, or value type as return value, and only auto handles to clean up after
// the call.
// Value types returned in memory are left out where the address is neither passed
// like an argument nor in a register of its own (see JIT_HIDDEN_RETURN_POINTER).
// Everything else, e.g. value objects with an unsupported native register layout,
// returns false and is called through the engine. Complex and large value objects
// passed indirectly are already pointers here and can be called directly.
//
// C++ exceptions thrown by the function pass through the generated code and are
// caught by JIT_GuardedEntry. Where the code has no unwind information for that
// (see CJITUnwindInfo) this is only used when the JIT_DIRECT_SYSTEM_CALLS flag is set
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

	// Calling convention. The object on the script stack and the native method's
	// this pointer aren't always the same: functor methods use auxiliary as this
	// and pass the script object as an ordinary first or last argument.
	bool thisFromStack = false, auxiliaryThis = false, virtualThis = false;
	bool objFirst = false, objLast = false;
	CallConvId conv = CallConvId::kCDecl;
	switch( sysFunc->callConv )
	{
	case ICC_CDECL:
		break;
	case ICC_STDCALL:
		if( !Is64Bit() ) conv = CallConvId::kStdCall;
		break;
	case ICC_THISCALL:
		if( sysFunc->auxiliary )
			auxiliaryThis = true;
		else
			thisFromStack = true;
#if defined(JIT_X86_THISCALL)
		conv = CallConvId::kThisCall;
#endif
		break;
#ifdef GNU_STYLE_VIRTUAL_METHOD
	case ICC_VIRTUAL_THISCALL:
		thisFromStack = true;
		virtualThis = true;
#if defined(JIT_X86_THISCALL)
		conv = CallConvId::kThisCall;
#endif
		break;
#endif
	case ICC_CDECL_OBJFIRST:
		objFirst = true;
		break;
	case ICC_CDECL_OBJLAST:
		objLast = true;
		break;
	case ICC_THISCALL_OBJFIRST:
		if( !sysFunc->auxiliary )
			return false;
		auxiliaryThis = true;
		objFirst = true;
#if defined(JIT_X86_THISCALL)
		conv = CallConvId::kThisCall;
#endif
		break;
	case ICC_THISCALL_OBJLAST:
		if( !sysFunc->auxiliary )
			return false;
		auxiliaryThis = true;
		objLast = true;
#if defined(JIT_X86_THISCALL)
		conv = CallConvId::kThisCall;
#endif
		break;
#ifdef GNU_STYLE_VIRTUAL_METHOD
	case ICC_VIRTUAL_THISCALL_OBJFIRST:
		if( !sysFunc->auxiliary )
			return false;
		auxiliaryThis = true;
		virtualThis = true;
		objFirst = true;
#if defined(JIT_X86_THISCALL)
		conv = CallConvId::kThisCall;
#endif
		break;
	case ICC_VIRTUAL_THISCALL_OBJLAST:
		if( !sysFunc->auxiliary )
			return false;
		auxiliaryThis = true;
		virtualThis = true;
		objLast = true;
#if defined(JIT_X86_THISCALL)
		conv = CallConvId::kThisCall;
#endif
		break;
#endif
	default:
		return false;
	}

	// The engine applies these adjustments only to method objects. Functor methods
	// have two object pointers with less useful registration semantics, so retain
	// their existing fallback when either pointer would need adjustment.
	if( (sysFunc->compositeOffset || sysFunc->isCompositeIndirect || sysFunc->baseOffset) && !thisFromStack )
		return false;
	// Auxiliary objects only have defined direct-call semantics for class methods.
	if( sysFunc->auxiliary && !auxiliaryThis )
		return false;
	int cleanupCount = JIT_GetSystemCallCleanupCount(descr, true);
	if( cleanupCount < 0 )
		return false;
	bool cleanupValues = sysFunc->takesObjByVal;

	// Return value. A value type returned by value is stored at the location that the
	// caller put on the stack, either by the function itself through the hidden pointer,
	// or from the registers it was returned in. RET_PARTS is an object returned in up to
	// four registers, which hold retPartSize bytes of it each, in turn
	enum { RET_VOID, RET_I32, RET_I64, RET_F32, RET_F64, RET_PTR, RET_HANDLE, RET_PARTS } retKind;
	TypeId retType;
	const asCDataType &rt = descr->returnType;
	bool retOnStack = descr->DoesReturnOnStack();
	bool retInMemory = retOnStack && sysFunc->hostReturnInMemory;
	int expectedRetSize;
	int retSize = retOnStack ? rt.GetSizeInMemoryBytes() : 0;
	int retParts = 0, retPartSize = 8;
	bool retPartsFloat = false;
#ifdef AS_ARM64
	// AArch64 returns an aggregate of up to four floats or doubles in a floating point
	// register each, e.g. three floats in s0, s1 and s2. hostReturnFloat doesn't tell
	// them, as the engine only sets it where the objects are split by the types of their
	// members, and as_callfunc_arm64.cpp looks at the flags of the type instead, also
	// for those of three or four doubles that the engine takes to be returned in memory
	const asQWORD retFlags = retOnStack ? rt.GetTypeInfo()->flags : 0;
	if( (retFlags & asOBJ_APP_CLASS_ALLFLOATS) && !(retFlags & COMPLEX_MASK) &&
		retSize <= 4 * ((retFlags & asOBJ_APP_CLASS_ALIGN8) ? 8 : 4) )
	{
		retPartSize = (retFlags & asOBJ_APP_CLASS_ALIGN8) ? 8 : 4;
		if( retSize % retPartSize )
			return false;
		retParts = retSize / retPartSize;
		retPartsFloat = true;
		retInMemory = false;
	}
	else
#endif
#if defined(AS_X64_GCC) || defined(AS_ARM64)
	// The other objects of 9 to 16 bytes that aren't returned in memory come in two
	// registers, rax and rdx or x0 and x1, or xmm0 and xmm1 if the engine says they
	// hold floats
	if( retOnStack && !retInMemory && (sysFunc->hostReturnSize == 3 || sysFunc->hostReturnSize == 4) )
	{
		retParts = 2;
		retPartsFloat = sysFunc->hostReturnFloat;
	}
#endif
	// The hidden pointer is passed like an argument, or in x8 on AArch64
	bool retInX8 = false;
#if defined(JIT_HIDDEN_RETURN_POINTER_X8)
	retInX8 = retInMemory;
#elif !defined(JIT_HIDDEN_RETURN_POINTER)
	if( retInMemory )
		return false;
#endif
	if( retParts )                                               { retKind = RET_PARTS;  retType = !retPartsFloat ? TypeId::kInt64 : retPartSize == 4 ? TypeId::kFloat32 : TypeId::kFloat64; expectedRetSize = sysFunc->hostReturnSize; }
	else if( retInMemory )                                       { retKind = RET_VOID;   retType = TypeId::kVoid;    expectedRetSize = AS_PTR_SIZE; }
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
	if( sysFunc->returnAutoHandle && retKind != RET_HANDLE )
		return false;
	SDirectBehaviour returnAddRef;
	if( sysFunc->returnAutoHandle && !GetDirectBehaviour(CastToObjectType(rt.GetTypeInfo())->beh.addref, returnAddRef) )
		return false;
	if( sysFunc->hostReturnSize != expectedRetSize || (retKind != RET_PARTS && sysFunc->hostReturnFloat != (retKind == RET_F32 || retKind == RET_F64)) )
		return false;
	// The float or double is returned in the register that the copy after the call
	// reads, see CJITByteCode::FindFloatReturns
	int floatReader = !retOnStack && (retKind == RET_F32 || retKind == RET_F64) ? m_code->GetFloatReaderAfter(idx) : -1;

	// Arguments, as laid out on the script stack
	enum { ARG_I32, ARG_I64, ARG_F32, ARG_F64, ARG_PTR };
	struct SArg { int kind; int stackOff; int valueSize; int valueOffset; int valueParts; TypeId type; bool valueFloat; bool valueFree; bool autoHandle; SDirectBehaviour release; };
	std::vector<SArg> args;
	bool hasStackObj = thisFromStack || objFirst || objLast;
	bool hasThis = thisFromStack || auxiliaryThis;
	int retOff = hasStackObj ? AS_PTR_SIZE : 0;
	int firstArg = retOff + (retOnStack ? AS_PTR_SIZE : 0);
	int stackPos = firstArg;
	asUINT cleanIdx = 0;
	for( asUINT n = 0; n < descr->parameterTypes.GetLength(); n++ )
	{
		const asCDataType &pt = descr->parameterTypes[n];
		SArg arg = {};
		arg.stackOff = stackPos;
		arg.autoHandle = !cleanupValues && n < sysFunc->paramAutoHandles.GetLength() &&
		                 sysFunc->paramAutoHandles[n];
		if( pt.GetTokenType() == ttQuestion )
		{
			// A variable-type parameter is passed to the application as two arguments:
			// its address followed by the runtime type id.
			if( arg.autoHandle )
				return false;
			arg.kind = ARG_PTR;
			arg.type = TypeId::kUIntPtr;
			args.push_back(arg);
			arg.kind = ARG_I32;
			arg.type = TypeId::kInt32;
			arg.stackOff += AS_PTR_SIZE;
			args.push_back(arg);
			stackPos += AS_PTR_SIZE + 1;
			continue;
		}
		else if( pt.IsReference() || pt.IsObjectHandle() || pt.IsFuncdef() ) { arg.kind = ARG_PTR; arg.type = TypeId::kUIntPtr; stackPos += AS_PTR_SIZE; }
		else if( pt.IsObject() )
		{
			int hfaPartSize = 0;
			int valueSize = JIT_GetInlineValueArgSize(descr, n, &arg.valueFloat, &hfaPartSize);
			if( valueSize > 0 )
			{
				int valuePartSize = hfaPartSize ? hfaPartSize : valueSize > 8 ? 8 : valueSize;
				int valueParts = hfaPartSize ? valueSize / hfaPartSize : valueSize > 8 ? 2 : 1;
				arg.valueParts = valueParts;
				arg.valueSize = valuePartSize;
				arg.valueFree = valueParts == 1;
				if( arg.valueFloat )
				{
					arg.kind = arg.valueSize == 4 ? ARG_F32 : ARG_F64;
					arg.type = arg.valueSize == 4 ? TypeId::kFloat32 : TypeId::kFloat64;
				}
				else
				{
					arg.kind = arg.valueSize <= 4 ? ARG_I32 : ARG_I64;
					arg.type = arg.valueSize == 1 ? TypeId::kUInt8 : arg.valueSize == 2 ? TypeId::kUInt16 :
					           arg.valueSize <= 4 ? TypeId::kUInt32 : TypeId::kUInt64;
				}
				args.push_back(arg);
				for( int part = 1; part < valueParts; part++ )
				{
					arg.valueParts = 0;
					arg.valueOffset = part * valuePartSize;
					arg.valueSize = hfaPartSize ? valuePartSize : valueSize - arg.valueOffset;
					arg.valueFree = part + 1 == valueParts;
					if( arg.valueFloat )
					{
						// A full eightbyte stack slot is needed for the last float of a
						// 12-byte System V aggregate. Its upper bits are zeroed when it is
						// loaded. AArch64 HFAs instead keep their member type here.
						arg.kind = hfaPartSize == 4 ? ARG_F32 : ARG_F64;
						arg.type = hfaPartSize == 4 ? TypeId::kFloat32 : TypeId::kFloat64;
					}
					else
					{
						arg.kind = arg.valueSize <= 4 ? ARG_I32 : ARG_I64;
						arg.type = arg.valueSize == 1 ? TypeId::kUInt8 : arg.valueSize == 2 ? TypeId::kUInt16 :
						           arg.valueSize <= 4 ? TypeId::kUInt32 : TypeId::kUInt64;
					}
					args.push_back(arg);
				}
				stackPos += AS_PTR_SIZE;
				continue;
			}
			else
			{
				arg.kind = ARG_PTR;
				arg.type = TypeId::kUIntPtr;
			}
			stackPos += AS_PTR_SIZE;
		}
		else if( pt.IsFloatType() )                { arg.kind = ARG_F32; arg.type = TypeId::kFloat32; stackPos += 1; }
		else if( pt.IsDoubleType() )               { arg.kind = ARG_F64; arg.type = TypeId::kFloat64; stackPos += 2; }
		else if( pt.GetSizeOnStackDWords() == 2 )  { arg.kind = ARG_I64; arg.type = TypeId::kInt64;   stackPos += 2; }
		else                                       { arg.kind = ARG_I32; arg.type = TypeId::kInt32;   stackPos += 1; }
		if( arg.autoHandle )
		{
			if( arg.kind != ARG_PTR || !GetDirectBehaviour(sysFunc->cleanArgs[cleanIdx++].ot->beh.release, arg.release) )
				return false;
		}
		args.push_back(arg);
	}
	if( !cleanupValues )
		asASSERT(cleanIdx == asUINT(cleanupCount));
	if( stackPos - firstArg != sysFunc->paramSize )
		return false;
	if( args.size() + (hasThis ? 1 : 0) + ((objFirst || objLast) ? 1 : 0) + (retInMemory ? 1 : 0) > Globals::kMaxFuncArgs )
		return false;

	// The hidden return pointer comes first, except after the object pointer of class methods with MSVC
	bool retFirst = retInMemory && !retInX8, retAfterThis = false;
#ifdef JIT_HIDDEN_RETURN_POINTER_AFTER_THIS
	if( retInMemory && hasThis )
	{
		retFirst = false;
		retAfterThis = true;
	}
#endif
	// Keep multi-slot aggregates wholly in registers or wholly on the stack. AsmJit
	// sees their slots as separate primitive arguments and may otherwise split them
	// at the end of an argument register bank.
	struct SArmStackHFA { int arg; int registerParts; int parts; int partSize; };
	std::vector<SArmStackHFA> forceStackArmHFAs;
	std::vector<TypeId> forceStackArmVecDummies;
	const Environment &env = m_uc.cc->environment();
	int forceStackGpArg = -1, forceStackVecArg = -1, forceStackArmGpArg = -1;
#if defined(AS_X64_GCC)
	// The freed System V register is available to the next scalar argument of its
	// class. AsmJit also gives floats passed on the stack 4 bytes each instead of 8,
	// so reject those calls.
	if( Is64Bit() && !env.is_platform_windows() && !env.is_msvc_abi() )
	{
		asUINT gpArgs = (retFirst ? 1 : 0) + (hasThis ? 1 : 0) +
		                (retAfterThis ? 1 : 0) + (objFirst ? 1 : 0);
		asUINT vecArgs = 0;
		for( asUINT n = 0; n < args.size(); n++ )
		{
			bool twoSlotValue = args[n].valueSize && !args[n].valueFree;
			if( args[n].kind == ARG_F32 || args[n].kind == ARG_F64 )
			{
				if( twoSlotValue )
				{
					if( vecArgs <= 6 )
						vecArgs += 2;
					else if( vecArgs == 7 && forceStackVecArg < 0 )
						forceStackVecArg = int(n);
					n++;
				}
				else
				{
					if( args[n].kind == ARG_F32 && vecArgs >= 8 )
						return false;
					if( vecArgs < 8 )
						vecArgs++;
				}
			}
			else if( twoSlotValue )
			{
				if( gpArgs <= 4 )
					gpArgs += 2;
				else if( gpArgs == 5 && forceStackGpArg < 0 )
					forceStackGpArg = int(n);
				n++;
			}
			else
			{
				if( gpArgs < 6 )
					gpArgs++;
			}
		}
	}
#elif defined(AS_ARM64)
	// AAPCS64 stops allocating general-purpose argument registers after a composite
	// does not fit. Reserve x7 with a dummy below when a two-slot value reaches it,
	// and move the complete value and every later argument to the stack.
	UNUSED_VAR(env);
	{
		asUINT gpArgs = (hasThis ? 1 : 0) + (objFirst ? 1 : 0);
		asUINT vecArgs = 0;
		for( asUINT n = 0; n < args.size(); n++ )
		{
			bool twoSlotValue = args[n].valueSize && !args[n].valueFree;
			bool floating = args[n].kind == ARG_F32 || args[n].kind == ARG_F64;
			if( args[n].valueParts && floating )
			{
				// AAPCS64 never splits an HFA between registers and the stack. Note
				// how many members AsmJit put in registers so they can all be moved to
				// packed stack slots below. Later floating arguments stay on the stack.
				if( vecArgs + asUINT(args[n].valueParts) > 8 )
				{
					int registerParts = vecArgs < 8 ? 8 - int(vecArgs) : 0;
					SArmStackHFA hfa = { int(n), registerParts, args[n].valueParts, args[n].valueSize };
					forceStackArmHFAs.push_back(hfa);
					int stackParts = hfa.parts - hfa.registerParts;
					int stackPartSize = env.is_platform_apple() ? hfa.partSize : 8;
					int packedSize = env.is_platform_apple() ? hfa.parts * hfa.partSize :
					                 (hfa.parts * hfa.partSize + 7) & ~7;
					for( int bytes = stackParts * stackPartSize; bytes < packedSize; bytes += stackPartSize )
						forceStackArmVecDummies.push_back(stackPartSize == 4 ? TypeId::kFloat32 : TypeId::kFloat64);
					vecArgs = 8;
				}
				else
					vecArgs += asUINT(args[n].valueParts);
				n += asUINT(args[n].valueParts - 1);
			}
			else if( floating )
			{
				if( vecArgs < 8 )
					vecArgs++;
			}
			else if( twoSlotValue )
			{
				if( gpArgs <= 6 )
					gpArgs += 2;
				else if( gpArgs == 7 )
				{
					forceStackArmGpArg = int(n);
					gpArgs = 8;
				}
				n++;
			}
			else if( !floating && gpArgs < 8 )
				gpArgs++;
		}
	}
#else
	UNUSED_VAR(env);
#endif
	int popSize = stackPos;

	FuncSignature sig(conv);
	sig.set_ret(retType);
	if( retFirst ) sig.add_arg(TypeId::kUIntPtr);
	if( hasThis ) sig.add_arg(TypeId::kUIntPtr);
	if( retAfterThis ) sig.add_arg(TypeId::kUIntPtr);
	if( objFirst ) sig.add_arg(TypeId::kUIntPtr);
	for( asUINT n = 0; n < args.size(); n++ ) sig.add_arg(args[n].type);
	if( objLast ) sig.add_arg(TypeId::kUIntPtr);
	if( retInX8 ) sig.add_arg(TypeId::kUIntPtr); // moved to x8 below
	asUINT realArgCount = asUINT(sig.arg_count());
	int forceStackGpDummy = -1, forceStackVecDummy = -1;
	if( forceStackGpArg >= 0 )
	{
		forceStackGpDummy = int(sig.arg_count());
		sig.add_arg(TypeId::kUInt64); // reserves one outgoing stack slot
	}
	if( forceStackVecArg >= 0 )
	{
		forceStackVecDummy = int(sig.arg_count());
		sig.add_arg(TypeId::kFloat64); // reserves one outgoing stack slot
	}
	int forceStackArmGpDummy = -1;
	if( forceStackArmGpArg >= 0 )
	{
		forceStackArmGpDummy = int(sig.arg_count());
		sig.add_arg(TypeId::kUInt64); // occupies x7 after the aggregate moves to the stack
	}
	for( asUINT n = 0; n < forceStackArmVecDummies.size(); n++ )
		sig.add_arg(forceStackArmVecDummies[n]); // reserves space used to pack a complete HFA

	// A null script object pointer is an exception raised by the VM. Auxiliary is
	// required to be non-null when the function is registered.
	Gp obj;
	if( hasStackObj )
	{
		obj = m_uc.new_gp_ptr();
		m_uc.load(obj, Stack(0));
		m_uc.j(BailLabel(idx), test_z(obj));
	}
	Gp thisObj;
	if( thisFromStack )
	{
		thisObj = obj;
		if( sysFunc->compositeOffset )
		{
			Gp adjusted = m_uc.new_gp_ptr();
			m_uc.add(adjusted, thisObj, Imm(sysFunc->compositeOffset));
			thisObj = adjusted;
		}
		if( sysFunc->isCompositeIndirect )
		{
			Gp adjusted = m_uc.new_gp_ptr();
			m_uc.load(adjusted, mem_ptr(thisObj));
			thisObj = adjusted;
		}
		int baseOffset = sysFunc->baseOffset;
#if defined(__GNUC__) && defined(AS_ARM64)
		// The low bit tags virtual methods on GNU ARM, leaving the byte offset shifted.
		baseOffset >>= 1;
#endif
		if( baseOffset )
		{
			Gp adjusted = m_uc.new_gp_ptr();
			m_uc.add(adjusted, thisObj, Imm(baseOffset));
			thisObj = adjusted;
		}
	}
	else if( auxiliaryThis )
		thisObj = PtrConst(asPWORD(sysFunc->auxiliary));
	Gp retPtr;
	if( retOnStack )
	{
		retPtr = m_uc.new_gp_ptr();
		m_uc.load(retPtr, Stack(retOff));
	}

	// The function may raise a script exception, suspend the context, or inspect the
	// variables through the debug interface, so the variables are written back. That
	// is done before the arguments are loaded, so that the variables passed in
	// registers end where they are moved to the registers of the arguments, which can
	// then be the same, instead of being held across the moves
	StoreDirtySlots(m_code->GetDirtyMask(idx));
	std::vector<Gp>  gpArgs(args.size());
	std::vector<Gp>  gpArgsHi(args.size());
	std::vector<Gp>  valueObjs(args.size());
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
			if( args[n].valueSize )
			{
				valueObjs[n] = m_uc.new_gp_ptr();
				m_uc.load(valueObjs[n], Stack(args[n].stackOff));
				m_uc.mov(gpArgs[n], Imm(0));
				int remaining = args[n].valueSize, partOffset = 0;
				while( remaining )
				{
					int partSize = remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
					Gp part = partOffset ? m_uc.new_gp32() : gpArgs[n];
					Mem source = Addr(valueObjs[n], args[n].valueOffset + partOffset);
					if( partSize == 4 ) m_uc.load_u32(part, source);
					else if( partSize == 2 ) m_uc.load_u16(part, source);
					else m_uc.load_u8(part, source);
					if( partOffset )
					{
						m_uc.shl(part, part, Imm(partOffset * 8));
						m_uc.or_(gpArgs[n], gpArgs[n], part);
					}
					partOffset += partSize;
					remaining -= partSize;
				}
			}
			else
			{
				// The value pushed right before is taken from its register, which
				// spares the load waiting for the store
				std::map<int, SPushedValue>::const_iterator pushed = m_staticStack ? m_pushed.find(m_spOffset + args[n].stackOff * 4) : m_pushed.end();
				if( pushed == m_pushed.end() || !MovePushed32(gpArgs[n], pushed->second) )
					m_uc.load_u32(gpArgs[n], Stack(args[n].stackOff));
			}
			break;
		case ARG_I64:
			if( Is64Bit() )
			{
				gpArgs[n] = m_uc.new_gp64();
				if( args[n].valueSize )
				{
					valueObjs[n] = m_uc.new_gp_ptr();
					m_uc.load(valueObjs[n], Stack(args[n].stackOff));
					m_uc.mov(gpArgs[n], Imm(0));
					int remaining = args[n].valueSize, partOffset = 0;
					while( remaining )
					{
						int partSize = remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
						Gp part = partOffset ? m_uc.new_gp64() : gpArgs[n];
						Mem source = Addr(valueObjs[n], args[n].valueOffset + partOffset);
						if( partSize == 4 ) m_uc.load_u32(part, source);
						else if( partSize == 2 ) m_uc.load_u16(part, source);
						else m_uc.load_u8(part, source);
						if( partOffset )
						{
							m_uc.shl(part, part, Imm(partOffset * 8));
							m_uc.or_(gpArgs[n], gpArgs[n], part);
						}
						partOffset += partSize;
						remaining -= partSize;
					}
				}
				else
					m_uc.load_u64(gpArgs[n], Stack(args[n].stackOff));
			}
			else
			{
				gpArgs[n] = m_uc.new_gp32();
				gpArgsHi[n] = m_uc.new_gp32();
				if( args[n].valueSize )
				{
					valueObjs[n] = m_uc.new_gp_ptr();
					m_uc.load(valueObjs[n], Stack(args[n].stackOff));
					m_uc.load_u32(gpArgs[n], Addr(valueObjs[n], args[n].valueOffset));
					m_uc.mov(gpArgsHi[n], Imm(0));
					int remaining = args[n].valueSize - 4, partOffset = 4;
					while( remaining )
					{
						int partSize = remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
						Gp part = partOffset == 4 ? gpArgsHi[n] : m_uc.new_gp32();
						Mem source = Addr(valueObjs[n], args[n].valueOffset + partOffset);
						if( partSize == 4 ) m_uc.load_u32(part, source);
						else if( partSize == 2 ) m_uc.load_u16(part, source);
						else m_uc.load_u8(part, source);
						if( partOffset != 4 )
						{
							m_uc.shl(part, part, Imm((partOffset - 4) * 8));
							m_uc.or_(gpArgsHi[n], gpArgsHi[n], part);
						}
						partOffset += partSize;
						remaining -= partSize;
					}
				}
				else
				{
					m_uc.load_u32(gpArgs[n], Stack(args[n].stackOff));
					m_uc.load_u32(gpArgsHi[n], Stack(args[n].stackOff + 1));
				}
			}
			break;
		case ARG_F32:
			vecArgs[n] = m_uc.new_vec128_f32x1();
			if( args[n].valueSize )
			{
				valueObjs[n] = m_uc.new_gp_ptr();
				m_uc.load(valueObjs[n], Stack(args[n].stackOff));
				m_uc.v_loadu32_f32(vecArgs[n], Addr(valueObjs[n], args[n].valueOffset));
			}
			else
			{
				// Like the int arguments above, also from the vector register of a float
				std::map<int, SPushedValue>::const_iterator pushed = m_staticStack ? m_pushed.find(m_spOffset + args[n].stackOff * 4) : m_pushed.end();
				if( pushed == m_pushed.end() || !MovePushedF32(vecArgs[n], pushed->second) )
					m_uc.v_loadu32_f32(vecArgs[n], Stack(args[n].stackOff));
			}
			break;
		case ARG_F64:
			vecArgs[n] = m_uc.new_vec128_f64x1();
			if( args[n].valueSize )
			{
				valueObjs[n] = m_uc.new_gp_ptr();
				m_uc.load(valueObjs[n], Stack(args[n].stackOff));
				if( args[n].valueSize == 4 )
					m_uc.v_loadu32_f32(vecArgs[n], Addr(valueObjs[n], args[n].valueOffset));
				else
					m_uc.v_loadu64_f64(vecArgs[n], Addr(valueObjs[n], args[n].valueOffset));
			}
			else
			{
				std::map<int, SPushedValue>::const_iterator pushed = m_staticStack ? m_pushed.find(m_spOffset + args[n].stackOff * 4) : m_pushed.end();
				if( pushed != m_pushed.end() && pushed->second.kind == SPushedValue::F64 )
					MoveVec(vecArgs[n], pushed->second.vec);
				else
					m_uc.v_loadu64_f64(vecArgs[n], Stack(args[n].stackOff));
			}
			break;
		}
	}
	for( asUINT n = 0; n < args.size(); n++ )
		if( args[n].valueSize && args[n].valueFree )
		{
			InvokeNode *freeValue = Invoke((const void*)JIT_FreeValueArg,
				FuncSignature::build<void, asSVMRegisters*, void*>());
			SetRegsArg(freeValue, 0);
			freeValue->set_arg(1, valueObjs[n]);
		}

	// The VM registers must be up to date too, and the frames of the inlined calls on
	// the call stack. The value register isn't needed until the slow path
	SyncStack();
	EmitMaterialize();
	SetPC(instr.pos);

	// Let the context know which function is being called, so that it may raise exceptions
	Mem callingFunc = ContextField(JIT_GetContextLayout().callingSystemFunction);
	m_uc.store(callingFunc, PtrConst(asPWORD(descr)));

	Vec forceStackVecValue;
	if( forceStackVecArg >= 0 )
	{
		forceStackVecValue = m_uc.new_vec128_f64x1();
		m_uc.v_zero_d(forceStackVecValue);
	}
	InvokeNode *call = 0;
	if( virtualThis )
	{
		// Itanium method pointers hold the byte offset in the virtual table plus 1.
		Gp target = m_uc.new_gp_ptr();
		m_uc.load(target, mem_ptr(thisObj));
		m_uc.load(target, Addr(target, int32_t(FuncPtrToUInt(sysFunc->func) - 1)));
		m_uc.cc->invoke(Out(call), target, sig);
	}
	else
		call = Invoke((const void*)FuncPtrToUInt(sysFunc->func), sig);
	if( forceStackGpArg >= 0 || forceStackVecArg >= 0 )
	{
		// Each signature dummy reserves an outgoing stack slot for the half moved out
		// of the last register. Move the first later scalar of the same register class
		// there, if any; otherwise the unused dummy occupies the register. Stack
		// arguments between the aggregate and that value move up by one eightbyte.
		asUINT argBase = (retFirst ? 1 : 0) + (hasThis ? 1 : 0) +
		                 (retAfterThis ? 1 : 0) + (objFirst ? 1 : 0);
		auto forcePairToStack = [&](int forceArg, int dummy, bool floating)
		{
			if( forceArg < 0 )
				return;
			asUINT first = argBase + asUINT(forceArg);
			asUINT second = first + 1;
			asUINT candidate = asUINT(dummy);
			for( asUINT n = asUINT(forceArg + 2); n < args.size(); n++ )
			{
				if( args[n].valueSize && !args[n].valueFree )
				{
					n++;
					continue;
				}
				bool argFloat = args[n].kind == ARG_F32 || args[n].kind == ARG_F64;
				if( argFloat == floating )
				{
					candidate = argBase + n;
					break;
				}
			}
			if( candidate == asUINT(dummy) && objLast && !floating )
				candidate = argBase + asUINT(args.size());

			FuncValue &firstValue = call->detail().arg(first);
			FuncValue &secondValue = call->detail().arg(second);
			FuncValue &candidateValue = call->detail().arg(candidate);
			int32_t firstOffset = secondValue.stack_offset();
			int32_t candidateOffset = candidateValue.stack_offset();
			for( asUINT n = 0; n < call->detail().arg_count(); n++ )
			{
				FuncValue &value = call->detail().arg(n);
				if( value.is_stack() && value.stack_offset() >= firstOffset && value.stack_offset() < candidateOffset )
					value.set_stack_offset(value.stack_offset() + 8);
			}
			firstValue.init_stack(firstOffset, firstValue.type_id());
			RegType candidateType = floating ? RegType::kVec128 :
			                        TypeUtils::size_of(candidateValue.type_id()) <= 4 ? RegType::kGp32 : RegType::kGp64;
			candidateValue.init_reg(candidateType, floating ? 7 : 9, candidateValue.type_id());
		};
		forcePairToStack(forceStackGpArg, forceStackGpDummy, false);
		forcePairToStack(forceStackVecArg, forceStackVecDummy, true);
	}
	if( forceStackArmGpArg >= 0 )
	{
		asUINT argBase = (retFirst ? 1 : 0) + (hasThis ? 1 : 0) +
		                 (retAfterThis ? 1 : 0) + (objFirst ? 1 : 0);
		asUINT first = argBase + asUINT(forceStackArmGpArg);
		FuncValue &firstValue = call->detail().arg(first);
		FuncValue &secondValue = call->detail().arg(first + 1);
		FuncValue &dummyValue = call->detail().arg(asUINT(forceStackArmGpDummy));
		int32_t firstOffset = secondValue.stack_offset();
		int32_t dummyOffset = dummyValue.stack_offset();
		for( asUINT n = 0; n < call->detail().arg_count(); n++ )
		{
			FuncValue &value = call->detail().arg(n);
			if( value.is_stack() && value.stack_offset() >= firstOffset && value.stack_offset() < dummyOffset )
				value.set_stack_offset(value.stack_offset() + 8);
		}
		firstValue.init_stack(firstOffset, firstValue.type_id());
		dummyValue.init_reg(RegType::kGp64, 7, TypeId::kUInt64);
	}
	if( !forceStackArmHFAs.empty() )
	{
		asUINT argBase = (retFirst ? 1 : 0) + (hasThis ? 1 : 0) +
		                 (retAfterThis ? 1 : 0) + (objFirst ? 1 : 0);
		for( asUINT n = 0; n < forceStackArmHFAs.size(); n++ )
		{
			const SArmStackHFA &hfa = forceStackArmHFAs[n];
			asUINT first = argBase + asUINT(hfa.arg);
			asUINT firstStack = first + asUINT(hfa.registerParts);
			int stackParts = hfa.parts - hfa.registerParts;
			int stackPartSize = env.is_platform_apple() ? hfa.partSize : 8;
			int packedSize = env.is_platform_apple() ? hfa.parts * hfa.partSize :
			                 (hfa.parts * hfa.partSize + 7) & ~7;
			int32_t firstOffset = call->detail().arg(firstStack).stack_offset();
			int32_t oldEnd = firstOffset + stackParts * stackPartSize;
			int32_t shift = packedSize - stackParts * stackPartSize;
			for( asUINT arg = 0; arg < realArgCount; arg++ )
			{
				FuncValue &value = call->detail().arg(arg);
				if( value.is_stack() && value.stack_offset() >= oldEnd )
					value.set_stack_offset(value.stack_offset() + shift);
			}
			for( int part = 0; part < hfa.parts; part++ )
			{
				FuncValue &value = call->detail().arg(first + asUINT(part));
				value.init_stack(firstOffset + part * hfa.partSize, value.type_id());
			}
		}
	}
	asUINT argIdx = 0;
	if( retFirst )
		call->set_arg(argIdx++, retPtr);
	if( hasThis )
		call->set_arg(argIdx++, thisObj);
	if( retAfterThis )
		call->set_arg(argIdx++, retPtr);
	if( objFirst )
		call->set_arg(argIdx++, obj);
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
	if( objLast )
		call->set_arg(argIdx++, obj);
	if( retInX8 )
	{
		// The last argument of the signature goes in x8 instead of where the calling
		// convention has put it. With more than eight integer arguments that was a
		// slot on the stack, which stays reserved and unused then
		call->set_arg(argIdx, retPtr);
		call->detail().arg(argIdx).init_reg(RegType::kGp64, 8, TypeId::kUIntPtr);
		argIdx++;
	}
	if( forceStackGpArg >= 0 )
		call->set_arg(argIdx++, Imm(0));
	if( forceStackVecArg >= 0 )
		call->set_arg(argIdx++, forceStackVecValue);
	if( forceStackArmGpArg >= 0 )
		call->set_arg(argIdx++, Imm(0));

	Gp  retGp, retGpHi, retGps[4];
	Vec retVec, retVecs[4];
	switch( retKind )
	{
	case RET_PARTS:
		for( int n = 0; n < retParts; n++ )
		{
			if( n )
				AddReturn(call->detail(), n, retType);
			if( retPartsFloat )
			{
				retVecs[n] = retPartSize == 4 ? m_uc.new_vec128_f32x1() : m_uc.new_vec128_f64x1();
				call->set_ret(n, retVecs[n]);
			}
			else
			{
				retGps[n] = m_uc.new_gp64();
				call->set_ret(n, retGps[n]);
			}
		}
		break;
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
		retVec = floatReader >= 0 ? ReturnedFloat(m_frame, asUINT(floatReader), false) : m_uc.new_vec128_f32x1();
		call->set_ret(0, retVec);
		break;
	case RET_F64:
		retVec = floatReader >= 0 ? ReturnedFloat(m_frame, asUINT(floatReader), true) : m_uc.new_vec128_f64x1();
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
	if( sysFunc->returnAutoHandle )
	{
		// Preserve the return before another native call can reuse its ABI register.
		m_uc.store(RegsField(offsetof(asSVMRegisters, objectRegister)), retGp);
		m_uc.store(RegsField(offsetof(asSVMRegisters, objectType)), PtrConst(asPWORD(rt.GetTypeInfo())));
		Label noHandle = m_uc.new_label();
		m_uc.j(noHandle, test_z(retGp));
		EmitBehaviourCall(returnAddRef, retGp);
		m_uc.bind(noHandle);
	}

	// Store the return value like the VM does before releasing the parameter auto
	// handles. The value register is left alone if it isn't read afterwards
	bool vrLive = m_code->IsVRLiveAfter(idx);
	if( retOnStack )
	{
		switch( retKind )
		{
		case RET_PARTS:
			// The floats of AArch64 take 4 bytes, and so does the last part of an object
			// of 12 bytes returned in two 8 byte registers
			for( int n = 0; n < retParts; n++ )
			{
				Mem part = Addr(retPtr, n * retPartSize);
				bool half = retPartSize == 4 || retSize - n * retPartSize == 4;
				if( retPartsFloat && half )
					m_uc.v_storeu32_f32(part, retVecs[n]);
				else if( retPartsFloat )
					m_uc.v_storeu64_f64(part, retVecs[n]);
				else if( half )
					m_uc.store_u32(part, retGps[n].r32());
				else
					m_uc.store_u64(part, retGps[n]);
			}
			break;
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
	else if( retKind == RET_HANDLE && !sysFunc->returnAutoHandle )
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
			StoreVR64(retGp, retGpHi);
		break;
	case RET_F32:
		{
			Gp bits = m_uc.new_gp32();
			m_uc.s_mov_u32(bits, retVec);
			StoreVR32(bits);
		}
		if( floatReader >= 0 )
			m_floatsReturned.insert(std::make_pair(m_frame, asUINT(floatReader)));
		break;
	case RET_F64:
		if( Is64Bit() )
		{
			Gp bits = m_uc.new_gp64();
			m_uc.s_mov_u64(bits, retVec);
			StoreVR64(bits);
		}
		else
			StoreVRF64(retVec);
		if( floatReader >= 0 )
			m_floatsReturned.insert(std::make_pair(m_frame, asUINT(floatReader)));
		break;
	case RET_PTR:
		StoreVRPtr(retGp);
		break;
	default:
		break;
	}

	// The function owns the value copies and references passed through @+ parameters.
	// Value copies use the engine's prepared cleanup metadata; the common auto-handle
	// case keeps its releases in line.
	if( cleanupValues )
	{
		InvokeNode *cleanup = Invoke((const void*)JIT_CleanupSystemCallArgs,
			FuncSignature::build<void, asSVMRegisters*, asCScriptFunction*>());
		SetRegsArg(cleanup, 0);
		cleanup->set_arg(1, Imm(int64_t(asPWORD(descr))));
	}
	else
	{
		for( asUINT n = 0; n < args.size(); n++ )
		{
			if( !args[n].autoHandle )
				continue;
			Gp handle = m_uc.new_gp_ptr();
			m_uc.load(handle, Stack(args[n].stackOff));
			Label noHandle = m_uc.new_label();
			m_uc.j(noHandle, test_z(handle));
			EmitBehaviourCall(args[n].release, handle);
			m_uc.store_zero_reg(Stack(args[n].stackOff));
			m_uc.bind(noHandle);
		}
	}
	PopStack(popSize * 4);

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
	SetRegsArg(after, 0);
	after->set_arg(1, Imm(funcId));
	if( retOnStack )
		after->set_arg(2, retPtr);
	else
		after->set_arg(2, Imm(0));
	after->set_ret(0, r);
	EmitLeaveIf(r);
	// With a debugger attached the variables may have been modified through the context
	ReloadLiveSlots(idx);
	// The value is taken from the value register again, so that the register it is
	// returned in isn't held across the helper, which would store it on the stack
	if( floatReader >= 0 )
		LoadVRFloat(retVec, retKind == RET_F64);
	EndCold(cold, cont);
	m_uc.bind(cont);
	EmitDematerialize();

	return true;
}

// Calls a registered function with the generic calling convention directly, like
// JIT_CallGeneric does, with the interface on the native stack, which is copied from
// one made for it, see JIT_MakeGeneric. The same restrictions as for
// EmitDirectSystemCall apply. The functions whose arguments are released after the
// call, or whose returned handle gets a reference, are left to the helper, see
// asCContext::CallGeneric, and the methods called on null to the VM, which raises the
// exception. Returns false if the call isn't made here
bool CJITCodeGen::EmitGenericCall(asUINT idx, const SJITGenericInfo *info)
{
	asCScriptFunction *descr = info->func;
	asSSystemFunctionInterface *sysFunc = descr->sysFuncIntf;
	bool method = sysFunc->callConv == ICC_GENERIC_METHOD;
	if( !m_options.directSystemCalls || (sysFunc->callConv != ICC_GENERIC_FUNC && !method) ||
	    sysFunc->cleanArgs.GetLength() || sysFunc->returnAutoHandle )
		return false;

	const SJITInstr &instr = m_code->GetInstructions()[idx];
	const SJITGenericLayout &layout = JIT_GetGenericLayout();
	bool retOnStack = descr->DoesReturnOnStack();
	int argsOff = (method ? AS_PTR_SIZE : 0) + (retOnStack ? AS_PTR_SIZE : 0);
	Gp obj;
	if( method )
	{
		obj = m_uc.new_gp_ptr();
		m_uc.load(obj, Stack(0));
		m_uc.j(BailLabel(idx), test_z(obj));
	}
	Gp retPtr;
	if( retOnStack )
	{
		retPtr = m_uc.new_gp_ptr();
		m_uc.load(retPtr, Stack(method ? AS_PTR_SIZE : 0));
	}

	// The function may raise a script exception, suspend the context, or inspect the
	// variables through the debug interface, see EmitDirectSystemCall
	StoreDirtySlots(m_code->GetDirtyMask(idx));
	SyncStack();
	EmitMaterialize();
	SetPC(instr.pos);

	// The interface of the function, with the object and the arguments. The calls of
	// the function share its slot
	if( !m_hasGenericStack )
	{
		m_genericStack = m_uc.new_stack(uint32_t(layout.size), 16);
		m_hasGenericStack = true;
	}
	std::vector<asBYTE> proto(size_t(layout.size));
	JIT_MakeGeneric(&proto[0], info);
	Gp args = m_uc.new_gp_ptr();
	Lea(args, Stack(argsOff));
	Gp t = m_uc.new_gp_ptr();
	for( int offset = 0; offset + PTR_BYTES <= layout.size; offset += PTR_BYTES )
	{
		Mem slot = m_genericStack.clone_adjusted(offset);
		asPWORD value = 0;
		memcpy(&value, &proto[size_t(offset)], PTR_BYTES);
		if( offset == layout.stackPointer )
			m_uc.store(slot, args);
		else if( offset == layout.currentObject && method )
			m_uc.store(slot, obj);
		else if( value == 0 )
			m_uc.store_zero_reg(slot);
		else
		{
			m_uc.mov(t, Imm(int64_t(value)));
			m_uc.store(slot, t);
		}
	}

	Mem callingFunc = ContextField(JIT_GetContextLayout().callingSystemFunction);
	m_uc.store(callingFunc, PtrConst(asPWORD(descr)));
	Gp gen = m_uc.new_gp_ptr();
	LeaStack(gen, m_genericStack);
	InvokeNode *call = Invoke((const void*)FuncPtrToUInt(sysFunc->func), FuncSignature::build<void, void*>());
	call->set_arg(0, gen);
	m_uc.store_zero_reg(callingFunc);

	// The value register gets the value returned, and the object register the object,
	// like asCContext::CallGeneric does
	bool vrLive = m_code->IsVRLiveAfter(idx);
	if( vrLive && Is64Bit() )
	{
		Gp value = m_uc.new_gp64();
		m_uc.load_u64(value, m_genericStack.clone_adjusted(layout.returnVal));
		StoreVR64(value);
	}
	else if( vrLive )
	{
		Gp lo = m_uc.new_gp32();
		Gp hi = m_uc.new_gp32();
		m_uc.load_u32(lo, m_genericStack.clone_adjusted(layout.returnVal));
		m_uc.load_u32(hi, m_genericStack.clone_adjusted(layout.returnVal + 4));
		StoreVR64(lo, hi);
	}
	const asCDataType &rt = descr->returnType;
	if( (rt.IsObject() || rt.IsFuncdef()) && !rt.IsReference() && !retOnStack )
	{
		Gp returned = m_uc.new_gp_ptr();
		m_uc.load(returned, m_genericStack.clone_adjusted(layout.objectRegister));
		m_uc.store(RegsField(offsetof(asSVMRegisters, objectRegister)), returned);
		m_uc.store(RegsField(offsetof(asSVMRegisters, objectType)), PtrConst(asPWORD(rt.GetTypeInfo())));
	}
	PopStack((sysFunc->paramSize + argsOff) * 4);

	// Exceptions, suspend requests, and line callbacks are handled by the helper
	Gp flag = m_uc.new_gp32();
	m_uc.load_u8(flag, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
	Label check = m_uc.new_label();
	Label cont = m_uc.new_label();
	m_uc.j(check, test_nz(flag));
	BaseNode *cold = BeginCold(check);
	SyncStack();
	if( vrLive )
		SyncVR();
	InvokeNode *after = Invoke((const void*)JIT_AfterDirectCall, FuncSignature::build<int, asSVMRegisters*, int, void*>());
	Gp r = m_uc.new_gp32();
	SetRegsArg(after, 0);
	after->set_arg(1, Imm(descr->GetId()));
	if( retOnStack )
		after->set_arg(2, retPtr);
	else
		after->set_arg(2, Imm(0));
	after->set_ret(0, r);
	EmitLeaveIf(r);
	ReloadLiveSlots(idx);
	EndCold(cold, cont);
	m_uc.bind(cont);
	EmitDematerialize();
	return true;
}

// The classes whose objects asBC_Cast to the type checks for in place: those of the
// module of the function that derive from the class cast to or implement the
// interface, if there are only a few, otherwise the class cast to, as the casts that
// go to other classes are made by the helper. The objects of a class are the same
// pointer for the classes it derives from and the interfaces it implements. The
// classes of the module exist as long as the code of its functions
static const asUINT JIT_CAST_CLASSES = 4;
static void FindCastClasses(asCScriptFunction *func, int typeId, std::vector<asCObjectType*> &classes)
{
	classes.clear();
	asCObjectType *to = func->engine->GetObjectTypeFromTypeId(typeId);
	if( to == 0 )
		return;
	if( func->module )
	{
		const asCArray<asCObjectType*> &types = func->module->m_classTypes;
		for( asUINT n = 0; n < types.GetLength() && classes.size() <= JIT_CAST_CLASSES; n++ )
		{
			asCObjectType *cls = types[n];
			if( (cls->flags & asOBJ_SCRIPT_OBJECT) && !(cls->flags & asOBJ_ABSTRACT) && !cls->IsInterface() &&
			    (to->IsInterface() ? cls->Implements(to) : cls->DerivesFrom(to)) )
				classes.push_back(cls);
		}
		if( classes.size() <= JIT_CAST_CLASSES )
			return;
	}
	classes.clear();
	if( !to->IsInterface() )
		classes.push_back(to);
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
		sysFunc->compositeOffset || sysFunc->isCompositeIndirect )
		return false;

	beh.func = (const void*)FuncPtrToUInt(sysFunc->func);
	beh.thisOffset = sysFunc->baseOffset;
#if defined(__GNUC__) && defined(AS_ARM64)
	beh.thisOffset >>= 1;
#endif
	beh.isVirtual = false;
	beh.conv = CallConvId::kCDecl;
	switch( sysFunc->callConv )
	{
	case ICC_THISCALL:
#if defined(JIT_X86_THISCALL)
		beh.conv = CallConvId::kThisCall;
#endif
		break;
#if defined(GNU_STYLE_VIRTUAL_METHOD) && (defined(AS_X86) || defined(AS_X64_GCC) || defined(AS_X64_MINGW) || defined(AS_ARM64))
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

	Gp thisObj = obj;
	if( beh.thisOffset )
	{
		thisObj = m_uc.new_gp_ptr();
		m_uc.add(thisObj, obj, Imm(beh.thisOffset));
	}

	InvokeNode *call = 0;
	if( beh.isVirtual )
	{
		Gp target = m_uc.new_gp_ptr();
		m_uc.load(target, mem_ptr(thisObj));
		m_uc.load(target, Addr(target, int32_t(asPWORD(beh.func) - 1)));
		m_uc.cc->invoke(Out(call), target, sig);
	}
	else
		call = Invoke(beh.func, sig);
	call->set_arg(0, thisObj);
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
	BaseNode *cold;
	if( EmitReleaseInPlace(obj, slow, race) )
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
		if( instr.flags & JIT_INSTR_CONSTRUCT )
		{
			EmitInPlaceConstruct(idx);
			break;
		}
		{
			asCObjectType *objType = (asCObjectType*)asBC_PTRARG(bc);
			int func = asBC_INTARG(bc + AS_PTR_SIZE);
			if( objType->flags & asOBJ_SCRIPT_OBJECT )
			{
				if( m_options.noScriptCalls )
					Bail(idx);
				else
					EmitScriptCall(idx, JIT_CALL_ALLOC, func, asPWORD(objType));
			}
			else
			{
				SyncAll(idx);
				InvokeNode *call = Invoke((const void*)JIT_Alloc, FuncSignature::build<int, asSVMRegisters*, void*, int>());
				Gp r = m_uc.new_gp32();
				SetRegsArg(call, 0);
				call->set_arg(1, Imm(int64_t(asPWORD(objType))));
				call->set_arg(2, Imm(func));
				call->set_ret(0, r);
				EmitAfterHelperCall(r, idx);
			}
		}
		break;

	case asBC_FREE:
		if( (instr.flags & JIT_INSTR_LENT) && m_frame == 0 )
		{
			// The variable holds the lent reference, which is only cleared, unless the
			// call or the VM has given it a reference of its own, see
			// CJITByteCode::FindLentRefs
			Label owned = m_uc.new_label();
			Label clear = m_uc.new_label();
			m_uc.j(owned, test_nz(m_lentOwned[idx]));
			BaseNode *cold = BeginCold(owned);
			Gp obj = LoadPtr(a0);
			m_uc.j(clear, test_z(obj));
			EmitScriptRelease(idx, obj);
			EndCold(cold, clear);
			m_uc.bind(clear);
			ClearPtr(a0);
			break;
		}
		// A separately generated direct entry gets its borrowed-parameter mask at
		// run time. Clear a still-borrowed parameter so the ordinary release path
		// below sees null. Once a sync point has taken ownership, the mask is zero
		// and the release proceeds normally.
		if( m_frame == 0 && m_directBorrowed.is_valid() )
		{
			int param = m_code->FindParam(a0);
			if( param >= 0 && param < 31 && ((m_code->GetBorrowableParams() >> param) & 1) )
			{
				Label owned = m_uc.new_label();
				m_uc.j(owned, test_z(m_directBorrowed, Imm(1u << param)));
				ClearPtr(a0);
				m_uc.bind(owned);
			}
		}
		if( IsBorrowed(idx) || (instr.flags & JIT_INSTR_MOVED) )
		{
			// The caller releases the reference, or the copy before has taken it
			// over, see CJITByteCode::FindMovedRefs
			ClearPtr(a0);
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
			ClearPtr(a0);
			m_uc.bind(skip);
			break;
		}
		if( instr.flags & JIT_INSTR_REFCOUNT )
		{
			// Like the VM the variable is cleared after the release. An object kept in
			// the frame is only forgotten, see EmitFrameObject
			Gp obj = LoadPtr(a0);
			Label skip = m_uc.new_label();
			m_uc.j(skip, test_z(obj));
			int slot = m_frame == 0 ? FrameSlot(a0) : -1;
			Label clear;
			if( slot >= 0 )
			{
				clear = m_uc.new_label();
				Gp at = m_uc.new_gp_ptr();
				m_uc.add(at, m_objectArena, Imm(slot));
				m_uc.j(clear, cmp_eq(obj, at));
			}
			EmitScriptRelease(idx, obj);
			if( slot >= 0 )
				m_uc.bind(clear);
			ClearPtr(a0);
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
			}
			else
			{
				Gp var = m_uc.new_gp_ptr();
				LeaVar(var, a0);
				InvokeNode *call = Invoke((const void*)JIT_Free, FuncSignature::build<void, asSVMRegisters*, void*, void*>());
				SetRegsArg(call, 0);
				call->set_arg(1, Imm(int64_t(asPWORD(objType))));
				call->set_arg(2, var);
			}
			EmitDematerialize();
			m_uc.bind(skip);
			ClearPtr(a0);
		}
		break;

	case asBC_LOADOBJ:
		{
			// The object created in place is still in its register, and the object
			// returned by an inlined function goes on in a register too. Then the
			// object register isn't needed, unless the VM returns, as nothing else
			// returns to the VM before the asBC_STOREOBJ of the caller, see
			// CJITByteCode::FindForwardedObjects
			Gp obj = m_code->GetObjectSource(idx) >= 0 ? ForwardedObject(m_frame, idx) : LoadPtr(a0);
			const std::vector<SJITInstr> &instrs = m_code->GetInstructions();
			bool forwarded = m_frames[m_frame].returnedObj.is_valid() && idx + 1 < instrs.size() && instrs[idx + 1].op == asBC_RET &&
			                 !(instrs[idx + 1].flags & JIT_INSTR_BAIL);
			if( !forwarded )
			{
				m_uc.store_zero_reg(RegsField(offsetof(asSVMRegisters, objectType)));
				m_uc.store(RegsField(offsetof(asSVMRegisters, objectRegister)), obj);
			}
			// Before the variable, which may hold the object in the same register, is cleared
			if( forwarded )
				m_uc.mov(m_frames[m_frame].returnedObj, obj);
			ClearPtr(a0);
		}
		break;

	case asBC_STOREOBJ:
		{
			// The object returned in a register by an inlined function hasn't been
			// stored in the object register, which the code that loads the register
			// from it clears instead, see EmitInlineCall and EmitEntryStubs
			Gp obj = m_uc.new_gp_ptr();
			if( m_code->GetObjectSource(idx) >= 0 )
				m_uc.mov(obj, ForwardedObject(m_frame, idx));
			else
				m_uc.load(obj, RegsField(offsetof(asSVMRegisters, objectRegister)));
			StorePtr(a0, obj);
			if( m_code->GetObjectSource(idx) < 0 )
				m_uc.store_zero_reg(RegsField(offsetof(asSVMRegisters, objectRegister)));
		}
		break;

	case asBC_REFCPY:
	case asBC_RefCpyV:
		if( (instr.flags & JIT_INSTR_LEND) && m_frame == 0 )
		{
			// Lends the reference of the memory that the handle was read from to the
			// call, see CJITByteCode::FindLentRefs. The VM copies the handle if the
			// copy would release anything
			Gp s   = m_uc.new_gp_ptr();
			Gp old = LoadPtr(a0);
			m_uc.j(BailLabel(idx), test_nz(old));
			m_uc.load(s, Stack(0));
			StorePtr(a0, s);
			m_uc.mov(m_lentOwned[asUINT(m_code->GetLentFree(idx))], Imm(0));
			break;
		}
		if( IsBorrowed(idx) )
		{
			// Lends the reference to the inlined call. The first of the copies for the
			// call checks that none of them would release anything
			const std::vector<int> &checks = m_code->GetBorrowChecks(idx);
			if( !checks.empty() )
			{
				// LoadPtr may return a cached variable, so accumulate in a temporary
				// instead of overwriting the first checked pointer with the OR result.
				Gp any = m_uc.new_gp_ptr();
				m_uc.mov(any, LoadPtr(checks[0]));
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
			Gp old;
			if( instr.op == asBC_RefCpyV )
				old = LoadPtr(a0);
			else
			{
				old = m_uc.new_gp_ptr();
				m_uc.load(old, dst);
			}
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
			if( instr.op == asBC_RefCpyV )
				StorePtr(a0, s);
			else
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
				if( instr.op == asBC_RefCpyV && FindCached(a0) )
					StorePtr(a0, s);
			}
			else if( move )
			{
				// Releases the old object and clears the variable
				InvokeNode *call = Invoke((const void*)JIT_Free, FuncSignature::build<void, asSVMRegisters*, void*, void*>());
				SetRegsArg(call, 0);
				call->set_arg(1, Imm(int64_t(asPWORD(objType))));
				call->set_arg(2, d);
				m_uc.store(mem_ptr(d), s);
				if( instr.op == asBC_RefCpyV && FindCached(a0) )
					StorePtr(a0, s);
			}
			else
			{
				InvokeNode *call = Invoke((const void*)JIT_RefCpy, FuncSignature::build<void, asSVMRegisters*, void*, void*, void*>());
				SetRegsArg(call, 0);
				call->set_arg(1, Imm(int64_t(asPWORD(objType))));
				call->set_arg(2, d);
				call->set_arg(3, s);
				if( instr.op == asBC_RefCpyV && FindCached(a0) )
					StorePtr(a0, s);
			}
			EmitDematerialize();
		}
		break;

	case asBC_Cast:
		{
			// The objects of the classes that can be cast to the class or interface get
			// their reference in place, see CJITByteCode::FindInPlaceRefCounts, or are
			// lent the reference of the variable cast from, see
			// CJITByteCode::FindLentCasts. The classes are looked up here, see
			// FindCastClasses, and JIT_Cast looks at the interfaces and the base classes
			// of the others, or JIT_CastObject where the reference is lent
			std::vector<asCObjectType*> classes;
			if( instr.flags & JIT_INSTR_REFCOUNT )
				FindCastClasses(m_code->GetFunction(), int(asBC_DWORDARG(bc)), classes);
			bool inPlace = !classes.empty();
			bool lent = (instr.flags & JIT_INSTR_LEND) && m_frame == 0;
			if( lent )
				m_uc.mov(m_lentOwned[asUINT(m_code->GetLentFree(idx))], Imm(0));
			Gp a   = m_uc.new_gp_ptr();
			Gp obj = m_uc.new_gp_ptr();
			Label done = m_uc.new_label();
			BaseNode *cold = 0;
			m_uc.load(a, Stack(0));
			if( inPlace || lent )
			{
				m_uc.j(done, test_z(a));
				m_uc.load(obj, mem_ptr(a));
				m_uc.j(done, test_z(obj));
			}
			if( inPlace )
			{
				const SJITObjectLayout &layout = JIT_GetObjectLayout();
				Gp type = m_uc.new_gp_ptr();
				Label other = m_uc.new_label();
				m_uc.load(type, Addr(obj, layout.objectType));
				Label match = m_uc.new_label();
				for( asUINT c = 0; c + 1 < classes.size(); c++ )
					m_uc.j(match, cmp_eq(type, PtrConst(asPWORD(classes[c]))));
				m_uc.j(other, cmp_ne(type, PtrConst(asPWORD(classes.back()))));
				m_uc.bind(match);
				if( !lent )
					EmitScriptAddRef(idx, obj);
				m_uc.store_zero_reg(RegsField(offsetof(asSVMRegisters, objectType)));
				m_uc.store(RegsField(offsetof(asSVMRegisters, objectRegister)), obj);
				cold = BeginCold(other);
			}
			if( lent )
			{
				// The object is read again, as the register allocator keeps it on the
				// stack in the code before otherwise, and returned
				Gp again = m_uc.new_gp_ptr();
				Gp cast  = m_uc.new_gp_ptr();
				m_uc.load(again, mem_ptr(a));
				InvokeNode *call = Invoke((const void*)JIT_CastObject, FuncSignature::build<void*, void*, asDWORD>());
				call->set_arg(0, again);
				call->set_arg(1, Imm(int(asBC_DWORDARG(bc))));
				call->set_ret(0, cast);
				m_uc.j(done, test_z(cast));
				m_uc.store_zero_reg(RegsField(offsetof(asSVMRegisters, objectType)));
				m_uc.store(RegsField(offsetof(asSVMRegisters, objectRegister)), cast);
			}
			else
			{
				InvokeNode *call = Invoke((const void*)JIT_Cast, FuncSignature::build<void, asSVMRegisters*, void*, asDWORD>());
				SetRegsArg(call, 0);
				call->set_arg(1, a);
				call->set_arg(2, Imm(int(asBC_DWORDARG(bc))));
			}
			if( cold )
				EndCold(cold, done);
			m_uc.bind(done);
			PopStack(PTR_BYTES);
		}
		break;

	default:
		return false;
	}

	return true;
}

END_AS_NAMESPACE
