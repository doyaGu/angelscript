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
void CJITCodeGen::EmitAfterHelperCall(const Gp &result, asUINT /*idx*/)
{
	Label cont = m_uc.new_label();
	m_uc.j(cont, test_z(result));
	Leave();
	m_uc.bind(cont);

	ReloadStack();
	ReloadVR();

	// With a debugger attached the variables may have been modified through the context
	if( !m_cached.empty() )
	{
		Gp t = m_uc.new_gp32();
		m_uc.load_u8(t, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
		Label skip = m_uc.new_label();
		m_uc.j(skip, test_z(t));
		ReloadCachedSlots();
		m_uc.bind(skip);
	}
}

// Script function call through the runtime helper, which also executes the
// called function natively when possible. Calls of script functions, methods,
// and function pointers push the call state inline and call the compiled function
// directly instead, unless it hasn't been compiled, the depth is exhausted, or
// the call stack must grow
void CJITCodeGen::EmitScriptCall(asUINT idx, int kind, int funcId, const Gp *extra, asPWORD extraImm)
{
	const SJITInstr &instr = m_code.GetInstructions()[idx];
	asCScriptFunction *func = m_code.GetFunction();
	asCScriptFunction *callee = 0;
	if( funcId >= 0 && asUINT(funcId) < func->engine->scriptFunctions.GetLength() )
		callee = func->engine->scriptFunctions[funcId];

	bool native = false;
	if( kind == JIT_CALL_SCRIPT )
		native = callee && callee->funcType == asFUNC_SCRIPT && callee->scriptData;
	else if( kind == JIT_CALL_INTERFACE )
		native = callee && (callee->funcType == asFUNC_VIRTUAL || callee->funcType == asFUNC_INTERFACE);
	else if( kind == JIT_CALL_PTR )
		native = true;

	StoreDirtySlots(m_code.GetDirtyMask(idx));
	SyncStack();

	Gp r = m_uc.new_gp32();
	Label slow = m_uc.new_label();
	Label done = m_uc.new_label();
	if( native )
	{
		const SJITObjectLayout &layout = JIT_GetObjectLayout();
		m_uc.j(slow, test_z(m_depth));

		// A recursive call enters this code, which exists as it is being executed
		Gp target;
		if( kind == JIT_CALL_SCRIPT && callee != func )
		{
			target = m_uc.new_gp_ptr();
			m_uc.load(target, mem_ptr(PtrConst(asPWORD(&callee->scriptData->jitFunction))));
		}
		else if( kind == JIT_CALL_INTERFACE )
		{
			target = EmitFindMethod(callee, slow);
			m_uc.load(target, mem_ptr(target, layout.scriptData));
			m_uc.load(target, mem_ptr(target, layout.jitFunction));
		}
		else if( kind == JIT_CALL_PTR )
		{
			// Everything but script functions is left to the helper
			Gp type = m_uc.new_gp32();
			m_uc.j(slow, test_z(*extra));
			m_uc.load_u32(type, mem_ptr(*extra, layout.funcType));
			m_uc.j(slow, cmp_ne(type, Imm(int(asFUNC_SCRIPT))));
			target = m_uc.new_gp_ptr();
			m_uc.load(target, mem_ptr(*extra, layout.scriptData));
			m_uc.load(target, mem_ptr(target, layout.jitFunction));
		}
		if( target.is_valid() )
			m_uc.j(slow, test_z(target));

		EmitNativeCall(idx, target, r, slow);
		m_uc.j(done);
	}

	m_uc.bind(slow);
	SetPC(instr.pos);
	InvokeNode *call = Invoke((const void*)JIT_CallScript, FuncSignature::build<int, asSVMRegisters*, int, int, asPWORD, asUINT>());
	call->set_arg(0, m_regs);
	call->set_arg(1, Imm(kind));
	call->set_arg(2, Imm(funcId));
	if( extra )
		call->set_arg(3, *extra);
	else
		call->set_arg(3, Imm(int64_t(extraImm)));
	call->set_arg(4, m_depth);
	call->set_ret(0, r);

	m_uc.bind(done);
	EmitAfterHelperCall(r, idx);
}

// Finds the implementation of a virtual or interface method for the object on the
// stack, like asCContext::CallInterfaceMethod. Jumps to slow if there is no object
// or it doesn't implement the interface, for the VM to raise the exception
CJITCodeGen::Gp CJITCodeGen::EmitFindMethod(asCScriptFunction *method, const Label &slow)
{
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	const uint32_t ptrShift = Is64Bit() ? 3 : 2;
	Gp type = m_uc.new_gp_ptr();
	m_uc.load(type, Stack(0));
	m_uc.j(slow, test_z(type));
	m_uc.load(type, mem_ptr(type, layout.objectType));
	Gp table = m_uc.new_gp_ptr();
	m_uc.load(table, mem_ptr(type, layout.virtualFunctionTable));
	Gp found = m_uc.new_gp_ptr();
	if( method->funcType == asFUNC_VIRTUAL )
	{
		m_uc.load(found, mem_ptr(table, method->vfTableIdx * PTR_BYTES));
		return found;
	}

	// The methods of each interface are at an offset in the table
	Gp list  = m_uc.new_gp_ptr();
	Gp count = m_uc.new_gp_ptr();
	Gp n     = m_uc.new_gp_ptr();
	Gp intf  = PtrConst(asPWORD(method->objectType));
	Label loop  = m_uc.new_label();
	Label match = m_uc.new_label();
	m_uc.load(list, mem_ptr(type, layout.interfaces));
	m_uc.load_u32(count, mem_ptr(type, layout.interfaceCount));
	m_uc.mov(n, Imm(0));
	m_uc.bind(loop);
	m_uc.j(slow, ucmp_ge(n, count));
	m_uc.load(found, mem_ptr(list, n, ptrShift));
	m_uc.j(match, cmp_eq(found, intf));
	m_uc.add(n, n, Imm(1));
	m_uc.j(loop);
	m_uc.bind(match);
	m_uc.load(list, mem_ptr(type, layout.interfaceVFTOffsets));
	m_uc.load_u32(n, mem_ptr(list, n, 2));
	m_uc.add(n, n, Imm(method->vfTableIdx));
	m_uc.load(found, mem_ptr(table, n, ptrShift));
	return found;
}

// Pushes the call state like asCContext::PushCallState and calls the native code
// of a script function, or this function if target isn't valid. Jumps to slow if
// the call stack must grow
void CJITCodeGen::EmitNativeCall(asUINT idx, const Gp &target, const Gp &result, const Label &slow)
{
	const SJITInstr &instr = m_code.GetInstructions()[idx];
	const SJITContextLayout &layout = JIT_GetContextLayout();
	Gp length = m_uc.new_gp_ptr();
	Gp t      = m_uc.new_gp_ptr();
	m_uc.load_u32(length, ContextField(layout.callStackLength));
	m_uc.load_u32(t, ContextField(layout.callStackCapacity));
	m_uc.j(slow, ucmp_ge(length, t));
	Gp state = m_uc.new_gp_ptr();
	m_uc.load(state, ContextField(layout.callStackArray));
	m_uc.add_ext(state, state, length, PTR_BYTES);
	m_uc.store(mem_ptr(state), m_fp);
	m_uc.store(mem_ptr(state, PTR_BYTES), PtrConst(asPWORD(m_code.GetFunction())));
	m_uc.store(mem_ptr(state, 2 * PTR_BYTES), PtrConst(asPWORD(m_code.GetByteCode() + instr.pos + asBCTypeSize[asBCInfo[instr.op].type])));
	m_uc.store(mem_ptr(state, 3 * PTR_BYTES), m_sp);
	m_uc.load_u32(t, ContextField(layout.stackIndex));
	m_uc.store(mem_ptr(state, 4 * PTR_BYTES), t);
	m_uc.add(length, length, Imm(layout.callStackFrameSize));
	m_uc.store_u32(ContextField(layout.callStackLength), length);

	Gp depth = m_uc.new_gp32();
	m_uc.sub(depth, m_depth, Imm(1));
	FuncSignature sig = FuncSignature::build<int, asSVMRegisters*, asPWORD, asUINT>();
	InvokeNode *call = 0;
	if( target.is_valid() )
		m_uc.cc->invoke(Out(call), target, sig);
	else
		m_uc.cc->invoke(Out(call), m_uc.cc->func()->label(), sig);
	call->set_arg(0, m_regs);
	call->set_arg(1, Imm(0));
	call->set_arg(2, depth);
	call->set_ret(0, result);
}

// Entry of native callers, which pass jitArg 0, see JITFunction. The frame is set
// up like asCContext::PrepareScriptFunction does when the current stack block has
// enough space and the VM has nothing to do, otherwise by JIT_PrepareFrame
void CJITCodeGen::EmitDirectEntry()
{
	asCScriptFunction *func = m_code.GetFunction();
	const SJITContextLayout &layout = JIT_GetContextLayout();

	m_uc.store(ContextField(layout.currentFunction), PtrConst(asPWORD(func)));

	Label slow  = m_uc.new_label();
	Label ready = m_uc.new_label();
	Gp block = m_uc.new_gp_ptr();
	Gp index = m_uc.new_gp_ptr();
	Gp limit = m_uc.new_gp_ptr();
	m_uc.load(block, ContextField(layout.stackBlocks));
	m_uc.load_u32(index, ContextField(layout.stackIndex));
	m_uc.add_ext(block, block, index, PTR_BYTES);
	m_uc.load(block, mem_ptr(block));
	m_uc.sub(limit, m_sp, Imm(int(func->scriptData->stackNeeded + layout.reserveStack) * 4));
	m_uc.j(slow, ucmp_lt(limit, block));
	Gp flag = m_uc.new_gp32();
	m_uc.load_u8(flag, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
	m_uc.j(slow, test_nz(flag));

	// Only the object variables on the heap are cleared, the others are initialized by their constructors
	m_uc.mov(m_fp, m_sp);
	const asCArray<asSScriptVariable*> &vars = func->scriptData->variables;
	for( asUINT n = 0; n < vars.GetLength(); n++ )
		if( vars[n]->stackOffset > 0 && vars[n]->onHeap && (vars[n]->type.IsObject() || vars[n]->type.IsFuncdef()) )
			m_uc.store_zero_reg(Var(vars[n]->stackOffset));
	m_uc.sub(m_sp, m_sp, Imm(int(func->scriptData->variableSpace) * 4));
	m_uc.store(RegsField(offsetof(asSVMRegisters, stackFramePointer)), m_fp);

	// Only what may be read before being written needs to be loaded, like in the entry stubs
	m_uc.bind(ready);
	if( m_options.syncEveryInstr )
	{
		ReloadVR();
		ReloadCachedSlots();
	}
	else
	{
		if( m_code.GetBlocks()[m_code.GetInstructions()[0].block].vrLiveIn )
			ReloadVR();
		ReloadSlots(m_code.GetLiveInMask(0));
	}
	m_uc.j(InstrLabel(0));

	m_uc.bind(slow);
	SetPC(0);
	InvokeNode *call = Invoke((const void*)JIT_PrepareFrame, FuncSignature::build<int, asSVMRegisters*>());
	Gp r = m_uc.new_gp32();
	call->set_arg(0, m_regs);
	call->set_ret(0, r);
	EmitLeaveIf(r);
	m_uc.load(m_fp, RegsField(offsetof(asSVMRegisters, stackFramePointer)));
	m_uc.load(m_sp, RegsField(offsetof(asSVMRegisters, stackPointer)));
	m_uc.j(ready);
}

bool CJITCodeGen::EmitCall(asUINT idx)
{
	const SJITInstr &instr = m_code.GetInstructions()[idx];
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
		else
			EmitScriptCall(idx, JIT_CALL_SCRIPT, asBC_INTARG(bc), 0, 0);
		break;

	case asBC_CALLINTF:
		if( m_options.noScriptCalls )
			Bail(idx);
		else
			EmitScriptCall(idx, JIT_CALL_INTERFACE, asBC_INTARG(bc), 0, 0);
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
		{
			// Local variables are dead at this point so only the value register
			// and stack pointer need to be written back
			const SJITContextLayout &layout = JIT_GetContextLayout();
			int popSize = asBC_WORDARG0(bc);
			if( m_code.RetReadsVR() )
				SyncVR();

			// Pop the call state like asCContext::PopCallState, unless the function
			// was called by the application or as a nested call
			Label finish = m_uc.new_label();
			Gp length = m_uc.new_gp_ptr();
			Gp state  = m_uc.new_gp_ptr();
			Gp t      = m_uc.new_gp_ptr();
			m_uc.load_u32(length, ContextField(layout.callStackLength));
			m_uc.j(finish, test_z(length));
			m_uc.sub(length, length, Imm(layout.callStackFrameSize));
			m_uc.load(state, ContextField(layout.callStackArray));
			m_uc.add_ext(state, state, length, PTR_BYTES);
			m_uc.load(t, mem_ptr(state));
			m_uc.j(finish, test_z(t));
			m_uc.store(RegsField(offsetof(asSVMRegisters, stackFramePointer)), t);
			m_uc.load(t, mem_ptr(state, PTR_BYTES));
			m_uc.store(ContextField(layout.currentFunction), t);
			m_uc.load(t, mem_ptr(state, 2 * PTR_BYTES));
			m_uc.store(RegsField(offsetof(asSVMRegisters, programPointer)), t);
			m_uc.load(t, mem_ptr(state, 3 * PTR_BYTES));
			m_uc.add(t, t, Imm(popSize * 4));
			m_uc.store(RegsField(offsetof(asSVMRegisters, stackPointer)), t);
			m_uc.load_u32(t, mem_ptr(state, 4 * PTR_BYTES));
			m_uc.store_u32(ContextField(layout.stackIndex), t);
			m_uc.store_u32(ContextField(layout.callStackLength), length);
			Gp zero = m_uc.new_gp32();
			m_uc.mov(zero, Imm(0));
			m_uc.ret(zero);

			m_uc.bind(finish);
			SyncStack();
			InvokeNode *call = Invoke((const void*)JIT_Return, FuncSignature::build<void, asSVMRegisters*, asUINT>());
			call->set_arg(0, m_regs);
			call->set_arg(1, Imm(popSize));
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
// C++ exceptions thrown by the function cannot be caught by the generated code, so
// this is only used when the JIT_DIRECT_SYSTEM_CALLS flag is set
//
// TODO: runtime optimize: Objects passed by value could be supported by setting up the
//                         argument copies the way CallSystemFunction and as_callfunc_*.cpp
//                         do for each ABI, and value types returned in more than two
//                         registers by reading them the way as_callfunc_x64_gcc.cpp does.
//                         Auto handles would need a release of the parameters after the call
//                         and an AddRef of the returned handle, and asCALL_GENERIC could be
//                         called with an asCGeneric set up inline. Each of these should be
//                         measured against CallSystemFunction before adding the code.
// TODO: The generated code has no unwind information, which is why C++ exceptions cannot
//       pass through it. AsmJit doesn't emit it, but it could be registered separately
//       (RtlAddFunctionTable on Win64, __register_frame with DWARF CFI elsewhere). With
//       that in place the exception could be caught by a C++ function wrapping the entry
//       into the generated code, and turned into a script exception like CallSystemFunction
//       does, so the flag could be on by default.
bool CJITCodeGen::EmitDirectSystemCall(asUINT idx, int funcId)
{
	const SJITInstr &instr = m_code.GetInstructions()[idx];
	asCScriptEngine *engine = m_code.GetFunction()->engine;
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
#if defined(_MSC_VER)
		// Only MSVC (and compatible compilers) has a distinct thiscall convention on 32bit x86
		if( !Is64Bit() ) conv = CallConvId::kThisCall;
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
	// must be up to date. The value register isn't needed until the slow path
	StoreDirtySlots(m_code.GetDirtyMask(idx));
	SyncStack();
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

	// Pop the arguments and store the return value like the VM does
	m_uc.add(m_sp, m_sp, Imm(popSize * 4));
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
				m_uc.store_u32(mem_ptr(retPtr, 4), retGpHi);
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
	else switch( retKind )
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
		{
			Gp bits = m_uc.new_gp64();
			m_uc.s_mov_u64(bits, retVec);
			StoreVR64(bits);
		}
		break;
	case RET_PTR:
		StoreVRPtr(retGp);
		break;
	case RET_HANDLE:
		m_uc.store(RegsField(offsetof(asSVMRegisters, objectRegister)), retGp);
		m_uc.store(RegsField(offsetof(asSVMRegisters, objectType)), PtrConst(asPWORD(rt.GetTypeInfo())));
		break;
	default:
		break;
	}

	// Exceptions, suspend requests, and line callbacks are handled by the helper
	Gp flag = m_uc.new_gp32();
	m_uc.load_u8(flag, RegsField(offsetof(asSVMRegisters, doProcessSuspend)));
	Label skip = m_uc.new_label();
	m_uc.j(skip, test_z(flag));
	// The VM continues with the popped stack pointer if the helper returns non-zero
	SyncStack();
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
	ReloadCachedSlots();
	m_uc.bind(skip);

	return true;
}

bool CJITCodeGen::EmitObjectOp(asUINT idx)
{
	const SJITInstr &instr = m_code.GetInstructions()[idx];
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
		{
			Gp obj = LoadPtr(a0);
			Label skip = m_uc.new_label();
			m_uc.j(skip, test_z(obj));
			// The release may execute a script destructor on the same context,
			// which starts its frame at the stack pointer of the registers
			SyncForCall(idx);
			Gp var = m_uc.new_gp_ptr();
			m_uc.lea(var, Var(a0));
			InvokeNode *call = Invoke((const void*)JIT_Free, FuncSignature::build<void, asSVMRegisters*, void*, void*>());
			call->set_arg(0, m_regs);
			call->set_arg(1, Imm(int64_t(asBC_PTRARG(bc))));
			call->set_arg(2, var);
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
		// TODO: runtime optimize: For script objects the reference counting could be done
		//                         inline: clear gcFlag and increment or decrement refCount
		//                         (with the atomic operations of the engine when built with
		//                         threads), and only call the helper when the release may
		//                         destroy the object, i.e. when refCount is 1. Application
		//                         types could have their AddRef/Release behaviours called
		//                         directly instead of through CallObjectMethod. The same
		//                         applies to FREE above.
		{
			Gp d = m_uc.new_gp_ptr();
			Gp s = m_uc.new_gp_ptr();
			m_uc.load(d, Stack(0));
			m_uc.add(m_sp, m_sp, Imm(PTR_BYTES));
			m_uc.load(s, Stack(0));
			// The release of the old object may execute a script destructor, see FREE
			SyncForCall(idx);
			InvokeNode *call = Invoke((const void*)JIT_RefCpy, FuncSignature::build<void, asSVMRegisters*, void*, void*, void*>());
			call->set_arg(0, m_regs);
			call->set_arg(1, Imm(int64_t(asBC_PTRARG(bc))));
			call->set_arg(2, d);
			call->set_arg(3, s);
		}
		break;

	case asBC_RefCpyV:
		{
			Gp d = m_uc.new_gp_ptr();
			Gp s = m_uc.new_gp_ptr();
			m_uc.lea(d, Var(a0));
			m_uc.load(s, Stack(0));
			SyncForCall(idx);
			InvokeNode *call = Invoke((const void*)JIT_RefCpy, FuncSignature::build<void, asSVMRegisters*, void*, void*, void*>());
			call->set_arg(0, m_regs);
			call->set_arg(1, Imm(int64_t(asBC_PTRARG(bc))));
			call->set_arg(2, d);
			call->set_arg(3, s);
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
			m_uc.add(m_sp, m_sp, Imm(PTR_BYTES));
		}
		break;

	default:
		return false;
	}

	return true;
}

END_AS_NAMESPACE
