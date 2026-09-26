#include "jit_runtime.h"

// Internal engine headers. The JIT must be compiled with the same
// configuration as the engine library (see CMakeLists.txt)
#include "as_context.h"
#include "as_scriptengine.h"
#include "as_scriptfunction.h"
#include "as_scriptobject.h"
#include "as_objecttype.h"
#include "as_callfunc.h"
#include "as_memory.h"
#include "as_texts.h"
#include "as_thread.h"

#include <math.h>
#include <string.h>

BEGIN_AS_NAMESPACE

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
#define JIT_CTX_OFFSET(member) (int(offsetof(asCContext, member)) - int(offsetof(asCContext, m_regs)))
const SJITContextLayout &JIT_GetContextLayout() noexcept
{
	static const SJITContextLayout layout =
	{
		JIT_CTX_OFFSET(m_callStack) + int(offsetof(asCArray<size_t>, array)),
		JIT_CTX_OFFSET(m_callStack) + int(offsetof(asCArray<size_t>, length)),
		JIT_CTX_OFFSET(m_callStack) + int(offsetof(asCArray<size_t>, maxLength)),
		JIT_CTX_OFFSET(m_currentFunction),
		JIT_CTX_OFFSET(m_stackIndex),
		JIT_CTX_OFFSET(m_stackBlocks) + int(offsetof(asCArray<asDWORD*>, array)),
		JIT_CTX_OFFSET(m_callingSystemFunction),
		JIT_CTX_OFFSET(m_status),
		CALLSTACK_FRAME_SIZE,
		RESERVE_STACK
	};
	return layout;
}
#undef JIT_CTX_OFFSET

// The generated code counts the references of the script objects in place, see
// CJITByteCode::FindInPlaceRefCounts. The flags are bit fields, which have no
// offset, so they are looked for in the memory of an object with only one set
struct SJITScriptObject : asCScriptObject
{
	static int RefCountOffset() { return int(offsetof(SJITScriptObject, refCount)); }
	static int FlagByte(bool dead) { int mask; return FindFlag(dead, mask); }
	static int FlagMask(bool dead) { int mask; FindFlag(dead, mask); return mask; }
	static int FindFlag(bool dead, int &mask)
	{
		alignas(asCScriptObject) unsigned char bytes[sizeof(asCScriptObject)];
		memset(bytes, 0, sizeof(bytes));
		SJITScriptObject *obj = reinterpret_cast<SJITScriptObject*>(bytes);
		if( dead )
			obj->hasRefCountReachedZero = true;
		else
			obj->gcFlag = true;
		for( int n = 0; n < int(sizeof(bytes)); n++ )
			if( bytes[n] )
			{
				mask = bytes[n];
				return n;
			}
		mask = 0;
		return 0;
	}
};
static_assert(sizeof(asCAtomic) == sizeof(asDWORD), "the reference counts are changed as 32bit integers");

const SJITObjectLayout &JIT_GetObjectLayout() noexcept
{
	static const SJITObjectLayout layout =
	{
		int(offsetof(asCScriptObject, objType)),
		int(offsetof(asCObjectType, virtualFunctionTable) + offsetof(asCArray<asCScriptFunction*>, array)),
		int(offsetof(asCObjectType, interfaces) + offsetof(asCArray<asCObjectType*>, array)),
		int(offsetof(asCObjectType, interfaces) + offsetof(asCArray<asCObjectType*>, length)),
		int(offsetof(asCObjectType, interfaceVFTOffsets) + offsetof(asCArray<asUINT>, array)),
		int(offsetof(asCScriptFunction, funcType)),
		int(offsetof(asCScriptFunction, scriptData)),
		int(offsetof(asCScriptFunction::ScriptFunctionData, jitFunction)),
		SJITScriptObject::RefCountOffset(),
		SJITScriptObject::FlagByte(false),
		SJITScriptObject::FlagMask(false),
		SJITScriptObject::FlagByte(true),
		SJITScriptObject::FlagMask(true),
#if defined(AS_NO_THREADS) || defined(AS_NO_ATOMIC)
		0
#else
		1
#endif
	};
	return layout;
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static inline asCContext *GetContext(asSVMRegisters *regs)
{
	return static_cast<asCContext*>(regs->ctx);
}

// Mirrors the suspend/status check the VM does after a system function call
static inline int CheckStatusAfterSystemCall(asSVMRegisters *regs, asCContext *ctx)
{
	if( regs->doProcessSuspend )
	{
		if( ctx->m_doSuspend )
		{
			ctx->m_status = asEXECUTION_SUSPENDED;
			return 1;
		}
		if( ctx->m_status != asEXECUTION_ACTIVE )
			return 1;
	}
	return 0;
}

int JIT_CallSystem(asSVMRegisters *regs, int funcId) noexcept
{
	asCContext *ctx = GetContext(regs);

	// The program pointer must stay on the CALLSYS instruction during the call
	regs->stackPointer += CallSystemFunction(funcId, ctx);
	regs->programPointer += 2;

	return CheckStatusAfterSystemCall(regs, ctx);
}

int JIT_Thiscall1(asSVMRegisters *regs, int funcId) noexcept
{
	asCContext *ctx = GetContext(regs);
	asCScriptEngine *engine = ctx->m_engine;

	void *obj = *(void**)regs->stackPointer;
	regs->stackPointer += AS_PTR_SIZE;
	int arg = *(int*)regs->stackPointer;
	regs->stackPointer++;

	ctx->m_callingSystemFunction = engine->scriptFunctions[funcId];
	void *ptr = 0;
#ifdef AS_NO_EXCEPTIONS
	ptr = engine->CallObjectMethodRetPtr(obj, arg, ctx->m_callingSystemFunction);
#else
	try
	{
		ptr = engine->CallObjectMethodRetPtr(obj, arg, ctx->m_callingSystemFunction);
	}
	catch(...)
	{
		ctx->HandleAppException();
	}
#endif
	ctx->m_callingSystemFunction = 0;
	*(asPWORD*)&regs->valueRegister = (asPWORD)ptr;
	regs->programPointer += 2;

	return CheckStatusAfterSystemCall(regs, ctx);
}

int JIT_AfterDirectCall(asSVMRegisters *regs, int funcId, void *retPointer) noexcept
{
	asCContext *ctx = GetContext(regs);
	asCScriptEngine *engine = ctx->m_engine;
	asCScriptFunction *descr = engine->scriptFunctions[funcId];

	// Like CallSystemFunction, a returned handle is released, and a value returned
	// on the stack is destroyed, if the function raised an exception
	if( ctx->m_status == asEXECUTION_EXCEPTION && retPointer )
	{
		asCObjectType *ot = CastToObjectType(descr->returnType.GetTypeInfo());
		if( ot && ot->beh.destruct )
			engine->CallObjectMethod(retPointer, ot->beh.destruct);
	}
	else if( ctx->m_status == asEXECUTION_EXCEPTION && regs->objectRegister &&
		(descr->returnType.IsObject() || descr->returnType.IsFuncdef()) && !descr->returnType.IsReference() )
	{
		asCObjectType *ot = CastToObjectType(descr->returnType.GetTypeInfo());
		if( ot && ot->beh.release )
			engine->CallObjectMethod(regs->objectRegister, ot->beh.release);
		regs->objectRegister = 0;
	}

	regs->programPointer += 2;
	return CheckStatusAfterSystemCall(regs, ctx);
}

#ifndef AS_NO_EXCEPTIONS
// Turns the C++ exception being caught into a script exception like CallSystemFunction,
// if it was thrown by a registered function that the generated code called directly.
// Returns false otherwise
static bool CatchDirectCallException(asSVMRegisters *regs, asCContext *ctx)
{
	asCScriptFunction *descr = ctx->m_callingSystemFunction;
	if( descr == 0 )
		return false;

	// The VM releases the parameters of the inlined functions on the call stack
	JIT_OwnBorrowed(regs, 0, 0);
	ctx->HandleAppException();
	ctx->m_callingSystemFunction = 0;

	// The VM registers describe the asBC_CALLSYS or asBC_Thiscall1 instruction in
	// the innermost function, with the arguments on the stack. The function hasn't
	// returned anything, so there is nothing to clean up
	asSSystemFunctionInterface *sysFunc = descr->sysFuncIntf;
	int popSize = sysFunc->paramSize;
	if( sysFunc->callConv >= ICC_THISCALL && sysFunc->auxiliary == 0 )
		popSize += AS_PTR_SIZE;
	if( descr->DoesReturnOnStack() )
		popSize += AS_PTR_SIZE;

	bool onStack = descr->DoesReturnOnStack();
	if( asEBCInstr(*(asBYTE*)regs->programPointer) == asBC_CALLSYS )
		regs->objectType = onStack ? 0 : descr->returnType.GetTypeInfo();
	if( !onStack && (descr->returnType.IsObject() || descr->returnType.IsFuncdef()) && !descr->returnType.IsReference() )
		regs->objectRegister = 0;
	else if( !onStack )
		regs->valueRegister = 0;

	regs->stackPointer += popSize;
	regs->programPointer += 2;
	return true;
}
#endif

asUINT JIT_nativeCallDepth = 256;

int JIT_GuardedEntry(asSVMRegisters *regs, asPWORD jitArg)
{
	asCContext *ctx = GetContext(regs);
	JITFunction func = reinterpret_cast<JITFunction>(ctx->m_currentFunction->scriptData->jitFunction);
#ifdef AS_NO_EXCEPTIONS
	return func(regs, jitArg | JIT_GUARDED_ENTRY, 0, 0);
#else
	try
	{
		return func(regs, jitArg | JIT_GUARDED_ENTRY, 0, 0);
	}
	catch(...)
	{
		if( !CatchDirectCallException(regs, ctx) )
			throw;
		return 1;
	}
#endif
}

// Calls a script function. The program pointer must be after the call instruction
// and the arguments on the stack. If the function has been compiled and the call
// stack is below the call limit the function is executed natively, otherwise the VM
// is left to do it.
// Returns 0 if the function has returned already
static int EnterScriptFunction(asSVMRegisters *regs, asCContext *ctx, asCScriptFunction *func, asUINT callLimit)
{
	JITFunction jitFunc = reinterpret_cast<JITFunction>(func->scriptData->jitFunction);
	if( jitFunc == 0 || ctx->m_callStack.GetLength() >= callLimit )
	{
		ctx->CallScriptFunction(func);
		return 1;
	}

	if( ctx->PushCallState() < 0 )
		return 1;
	ctx->m_currentFunction = func;
	return jitFunc(regs, 0, callLimit, regs->stackPointer);
}

// Finds the implementation of a virtual or interface method for the object on
// the stack, like asCContext::CallInterfaceMethod. Returns null after raising an
// exception if there is no object
static asCScriptFunction *ResolveVirtual(asSVMRegisters *regs, asCContext *ctx, asCScriptFunction *func)
{
	asCScriptObject *obj = *(asCScriptObject**)(asPWORD*)regs->stackPointer;
	if( obj == 0 )
	{
		ctx->m_needToCleanupArgs = true;
		ctx->SetInternalException(TXT_NULL_POINTER_ACCESS);
		return 0;
	}

	asCObjectType *objType = obj->objType;
	if( func->funcType != asFUNC_INTERFACE )
		return objType->virtualFunctionTable[func->vfTableIdx];

	asCScriptFunction *real = JIT_FindInterfaceMethod(objType, func);
	if( real )
		return real;

	ctx->m_needToCleanupArgs = true;
	ctx->SetInternalException(TXT_NULL_POINTER_ACCESS);
	return 0;
}

asCScriptFunction *JIT_FindInterfaceMethod(asCObjectType *objType, asCScriptFunction *func) noexcept
{
	for( asUINT n = 0; n < objType->interfaces.GetLength(); n++ )
		if( objType->interfaces[n] == func->objectType )
			return objType->virtualFunctionTable[func->vfTableIdx + objType->interfaceVFTOffsets[n]];
	return 0;
}

int JIT_CallScript(asSVMRegisters *regs, int kind, int funcId, asPWORD extra, asUINT callLimit)
{
	asCContext *ctx = GetContext(regs);
	asCScriptEngine *engine = ctx->m_engine;

	// The program pointer is at the call instruction. The code below mirrors
	// the VM implementation of the respective instructions
	switch( kind )
	{
	case JIT_CALL_SCRIPT:
		regs->programPointer += 2;
		return EnterScriptFunction(regs, ctx, engine->scriptFunctions[funcId], callLimit);

	case JIT_CALL_INTERFACE:
		{
			regs->programPointer += 2;
			asCScriptFunction *func = ResolveVirtual(regs, ctx, engine->scriptFunctions[funcId]);
			if( func == 0 )
				return 1;
			return EnterScriptFunction(regs, ctx, func, callLimit);
		}

	case JIT_CALL_BOUND:
		{
			int boundId = engine->importedFunctions[funcId & ~FUNC_IMPORTED]->boundFunctionId;
			if( boundId == -1 )
			{
				regs->programPointer += 2;
				ctx->m_needToCleanupArgs = true;
				ctx->SetInternalException(TXT_UNBOUND_FUNCTION);
				return 1;
			}

			asCScriptFunction *func = engine->GetScriptFunction(boundId);
			if( func->funcType == asFUNC_SCRIPT )
			{
				regs->programPointer += 2;
				return EnterScriptFunction(regs, ctx, func, callLimit);
			}
			else if( func->funcType == asFUNC_SYSTEM )
			{
				regs->stackPointer += CallSystemFunction(func->id, ctx);
				regs->programPointer += 2;
			}
			else
			{
				// Not supported by the VM either
				return 1;
			}
		}
		break;

	case JIT_CALL_PTR:
		{
			asCScriptFunction *func = reinterpret_cast<asCScriptFunction*>(extra);
			if( func == 0 )
			{
				regs->programPointer += 2;
				ctx->m_needToCleanupArgs = true;
				ctx->SetInternalException(TXT_UNBOUND_FUNCTION);
				return 1;
			}

			if( func->funcType == asFUNC_SCRIPT )
			{
				regs->programPointer += 2;
				return EnterScriptFunction(regs, ctx, func, callLimit);
			}
			else if( func->funcType == asFUNC_DELEGATE )
			{
				regs->stackPointer -= AS_PTR_SIZE;
				*(asPWORD*)regs->stackPointer = asPWORD(func->objForDelegate);
				if( func->funcForDelegate->funcType == asFUNC_SYSTEM )
				{
					regs->stackPointer += CallSystemFunction(func->funcForDelegate->id, ctx);
					regs->programPointer += 2;
				}
				else
				{
					regs->programPointer += 2;
					asCScriptFunction *method = ResolveVirtual(regs, ctx, func->funcForDelegate);
					if( method == 0 )
						return 1;
					return EnterScriptFunction(regs, ctx, method, callLimit);
				}
			}
			else if( func->funcType == asFUNC_SYSTEM )
			{
				regs->stackPointer += CallSystemFunction(func->id, ctx);
				regs->programPointer += 2;
			}
			else if( func->funcType == asFUNC_IMPORTED )
			{
				regs->programPointer += 2;
				int boundId = engine->importedFunctions[func->id & ~FUNC_IMPORTED]->boundFunctionId;
				if( boundId > 0 )
					return EnterScriptFunction(regs, ctx, engine->scriptFunctions[boundId], callLimit);
				else
				{
					ctx->m_needToCleanupArgs = true;
					ctx->SetInternalException(TXT_UNBOUND_FUNCTION);
					return 1;
				}
			}
			else
				return 1;
		}
		break;

	case JIT_CALL_ALLOC:
		{
			asDWORD *mem = (asDWORD*)JIT_NewScriptObject(reinterpret_cast<asCObjectType*>(extra));

			asCScriptFunction *f = engine->scriptFunctions[funcId];
			asDWORD **a = (asDWORD**)*(asPWORD*)(regs->stackPointer + f->GetSpaceNeededForArguments());
			if( a ) *a = mem;

			regs->stackPointer -= AS_PTR_SIZE;
			*(asPWORD*)regs->stackPointer = (asPWORD)mem;
			regs->programPointer += 2 + AS_PTR_SIZE;
			return EnterScriptFunction(regs, ctx, f, callLimit);
		}

	case JIT_CALL_CONSTRUCT:
		regs->programPointer += 2 + AS_PTR_SIZE;
		return EnterScriptFunction(regs, ctx, engine->scriptFunctions[funcId], callLimit);

	default:
		return 1;
	}

	// A system function was called and has returned already
	return ctx->m_status != asEXECUTION_ACTIVE ? 1 : 0;
}

int JIT_PrepareFrame(asSVMRegisters *regs) noexcept
{
	asCContext *ctx = GetContext(regs);
	ctx->PrepareScriptFunction();
	return ctx->m_status != asEXECUTION_ACTIVE ? 1 : 0;
}

int JIT_Suspend(asSVMRegisters *regs) noexcept
{
	asCContext *ctx = GetContext(regs);

	if( ctx->m_lineCallback )
		ctx->CallLineCallback();

	if( ctx->m_doSuspend )
	{
		regs->programPointer += 1;
		ctx->m_status = asEXECUTION_SUSPENDED;
		return 1;
	}

	return 0;
}

void JIT_ExitInlined(asSVMRegisters *regs, asCScriptFunction *func, asDWORD *frame, asDWORD *callerPC, asUINT borrowed) noexcept
{
	asCContext *ctx = GetContext(regs);
	asDWORD *pc = regs->programPointer;
	asDWORD *sp = regs->stackPointer;
	regs->programPointer = callerPC;
	regs->stackPointer   = frame;
	ctx->PushCallState();

	regs->stackFramePointer = frame;
	ctx->m_currentFunction  = func;
	regs->programPointer    = pc;
	regs->stackPointer      = sp;
	if( borrowed )
		JIT_OwnParams(func, frame, borrowed);
}

void JIT_OwnParams(asCScriptFunction *func, asDWORD *frame, asUINT mask) noexcept
{
	int offset = (func->objectType ? AS_PTR_SIZE : 0) + (func->DoesReturnOnStack() ? AS_PTR_SIZE : 0);
	for( asUINT n = 0; n < func->parameterTypes.GetLength(); n++ )
	{
		if( n < 31 && ((mask >> n) & 1) )
		{
			void *obj = *(void**)(frame + offset);
			if( obj )
				func->engine->CallObjectMethod(obj, func->parameterTypes[n].GetBehaviour()->addref);
		}
		offset += func->parameterTypes[n].GetSizeOnStackDWords();
	}
}

void JIT_OwnBorrowed(asSVMRegisters *regs, asDWORD *rootFrame, asCScriptFunction *rootFunc) noexcept
{
	asCContext *ctx = GetContext(regs);
	asCScriptFunction *func = ctx->m_currentFunction;
	asDWORD *frame = regs->stackFramePointer;
	for( asUINT n = ctx->m_callStack.GetLength(); n >= CALLSTACK_FRAME_SIZE; n -= CALLSTACK_FRAME_SIZE )
	{
		if( frame == rootFrame && func == rootFunc )
			break;
		asPWORD *s = ctx->m_callStack.AddressOf() + n - CALLSTACK_FRAME_SIZE;
		if( s[0] == 0 )
			break;
		asQWORD index = asQWORD(s[4]);
		asUINT mask = asUINT(index >> 32) & 0x7FFFFFFF;
		if( mask )
		{
			JIT_OwnParams(func, frame, mask);
			s[4] = asPWORD(index & ~(asQWORD(0x7FFFFFFF) << 32));
		}
		frame = (asDWORD*)s[0];
		func  = (asCScriptFunction*)s[1];
	}
}

void *JIT_NewScriptObject(asCObjectType *objType) noexcept
{
	asDWORD *mem = (asDWORD*)objType->engine->CallAlloc(objType);
	ScriptObject_Construct(objType, (asCScriptObject*)mem);
	return mem;
}

int JIT_Alloc(asSVMRegisters *regs, asCObjectType *objType, int funcId) noexcept
{
	asCContext *ctx = GetContext(regs);
	asCScriptEngine *engine = ctx->m_engine;

	asDWORD *mem = (asDWORD*)engine->CallAlloc(objType);
	if( funcId )
	{
		// Push the object pointer for the constructor
		regs->stackPointer -= AS_PTR_SIZE;
		*(asPWORD*)regs->stackPointer = (asPWORD)mem;
		regs->stackPointer += CallSystemFunction(funcId, ctx);
	}

	// Pop the destination address and store the pointer there
	asDWORD **a = (asDWORD**)*(asPWORD*)regs->stackPointer;
	regs->stackPointer += AS_PTR_SIZE;
	if( a ) *a = mem;

	regs->programPointer += 2 + AS_PTR_SIZE;

	if( regs->doProcessSuspend )
	{
		if( ctx->m_doSuspend )
		{
			ctx->m_status = asEXECUTION_SUSPENDED;
			return 1;
		}
		if( ctx->m_status != asEXECUTION_ACTIVE )
		{
			// The constructor raised an exception, so the memory must be freed
			engine->CallFree(mem);
			if( a ) *a = 0;
			return 1;
		}
	}

	return 0;
}

void JIT_Free(asSVMRegisters *regs, asCObjectType *objType, asPWORD *var) noexcept
{
	if( *var == 0 )
		return;

	asCContext *ctx = GetContext(regs);
	asCScriptEngine *engine = ctx->m_engine;
	asSTypeBehaviour *beh = &objType->beh;

	if( objType->flags & asOBJ_REF )
	{
		if( beh->release )
			engine->CallObjectMethod((void*)*var, beh->release);
	}
	else
	{
		if( beh->destruct )
			engine->CallObjectMethod((void*)*var, beh->destruct);
		else if( objType->flags & asOBJ_LIST_PATTERN )
			engine->DestroyList((asBYTE*)*var, objType);

		engine->CallFree((void*)*var);
	}

	*var = 0;
}

void JIT_RefCpy(asSVMRegisters *regs, asCObjectType *objType, void **dst, void *src) noexcept
{
	asCContext *ctx = GetContext(regs);
	asCScriptEngine *engine = ctx->m_engine;
	asSTypeBehaviour *beh = &objType->beh;

	if( !(objType->flags & (asOBJ_NOCOUNT | asOBJ_VALUE)) )
	{
		if( *dst != 0 && beh->release )
			engine->CallObjectMethod(*dst, beh->release);
		if( src != 0 && beh->addref )
			engine->CallObjectMethod(src, beh->addref);
	}

	*dst = src;
}

void JIT_AddRefScriptObject(void *obj) noexcept
{
	static_cast<asCScriptObject*>(obj)->AddRef();
}

void JIT_ReleaseScriptObject(void *obj) noexcept
{
	static_cast<asCScriptObject*>(obj)->Release();
}

void JIT_Cast(asSVMRegisters *regs, void **handle, asDWORD typeId) noexcept
{
	if( handle == 0 || *handle == 0 )
		return;

	asCContext *ctx = GetContext(regs);
	asCScriptEngine *engine = ctx->m_engine;

	asCScriptObject *obj = (asCScriptObject*)*handle;
	asCObjectType *objType = obj->objType;
	asCObjectType *to = engine->GetObjectTypeFromTypeId(typeId);

	if( objType->Implements(to) || objType->DerivesFrom(to) )
	{
		regs->objectType = 0;
		regs->objectRegister = obj;
		obj->AddRef();
	}
}

void *JIT_AllocMem(asUINT size) noexcept
{
	asBYTE *mem = asNEWARRAY(asBYTE, size);
	memset(mem, 0, size);
	return mem;
}

// Like asCScriptEngine::CallFree
void JIT_FreeMem(void *mem) noexcept
{
#ifndef WIP_16BYTE_ALIGN
	userFree(mem);
#else
	userFreeAligned(mem);
#endif
}

void JIT_MemCpy(void *dst, const void *src, asUINT sizeInBytes) noexcept
{
	memcpy(dst, src, sizeInBytes);
}

// The program pointer must be at the instruction so the exception gets the right line
static int RaisePowOverflow(asSVMRegisters *regs, bool isOverflow)
{
	if( !isOverflow )
		return 0;
	GetContext(regs)->SetInternalException(TXT_POW_OVERFLOW);
	return 1;
}

int JIT_POWi(asSVMRegisters *regs, int *dst, int a, int b) noexcept
{
	bool isOverflow;
	*dst = as_powi(a, b, isOverflow);
	return RaisePowOverflow(regs, isOverflow);
}

int JIT_POWu(asSVMRegisters *regs, asDWORD *dst, asDWORD a, asDWORD b) noexcept
{
	bool isOverflow;
	*dst = as_powu(a, b, isOverflow);
	return RaisePowOverflow(regs, isOverflow);
}

int JIT_POWi64(asSVMRegisters *regs, asINT64 *dst, const asINT64 *a, const asINT64 *b) noexcept
{
	bool isOverflow;
	*dst = as_powi64(*a, *b, isOverflow);
	return RaisePowOverflow(regs, isOverflow);
}

int JIT_POWu64(asSVMRegisters *regs, asQWORD *dst, const asQWORD *a, const asQWORD *b) noexcept
{
	bool isOverflow;
	*dst = as_powu64(*a, *b, isOverflow);
	return RaisePowOverflow(regs, isOverflow);
}

int JIT_POWf(asSVMRegisters *regs, float *dst, float a, float b) noexcept
{
	float r = powf(a, b);
	*dst = r;
	return RaisePowOverflow(regs, r == HUGE_VALF || isinf(r));
}

int JIT_POWd(asSVMRegisters *regs, double *dst, double a, double b) noexcept
{
	double r = pow(a, b);
	*dst = r;
	return RaisePowOverflow(regs, r == HUGE_VAL || isinf(r));
}

int JIT_POWdi(asSVMRegisters *regs, double *dst, double a, int b) noexcept
{
	double r = pow(a, b);
	*dst = r;
	return RaisePowOverflow(regs, r == HUGE_VAL || isinf(r));
}

float JIT_MODf(float a, float b) noexcept
{
	return fmodf(a, b);
}

double JIT_MODd(double a, double b) noexcept
{
	return fmod(a, b);
}

// The conversions below replicate the exact expressions used by the VM

asQWORD JIT_fTOu64(float f) noexcept
{
	if( f < 0 )
		return asQWORD(asINT64(f));
	return asQWORD(f);
}

asQWORD JIT_dTOu64(double d) noexcept
{
	if( d < 0 )
		return asQWORD(asINT64(d));
	return asQWORD(d);
}

float JIT_u64TOf(asQWORD v) noexcept
{
	return float(v);
}

double JIT_u64TOd(asQWORD v) noexcept
{
	return double(v);
}

asUINT JIT_fTOu(float f) noexcept
{
	if( f < 0 )
		return asUINT(int(f));
	return asUINT(f);
}

asUINT JIT_dTOu(double d) noexcept
{
	if( d < 0 )
		return asUINT(int(d));
	return asUINT(d);
}

float JIT_uTOf(asUINT v) noexcept
{
	return float(v);
}

double JIT_uTOd(asUINT v) noexcept
{
	return double(v);
}

int JIT_I64Op(int op, void *dst, const void *a, const void *b) noexcept
{
	// The shift count and the sources of the conversions from 32bit types are
	// only 32bit wide, so they must not be read as 64bit values
	switch( op )
	{
	case asBC_BSLL64:  *(asQWORD*)dst = *(const asQWORD*)a << *(const asDWORD*)b; return 0;
	case asBC_BSRL64:  *(asQWORD*)dst = *(const asQWORD*)a >> *(const asDWORD*)b; return 0;
	case asBC_BSRA64:  *(asINT64*)dst = *(const asINT64*)a >> *(const asDWORD*)b; return 0;
	case asBC_uTOi64:  *(asINT64*)dst = asINT64(*(const asUINT*)a); return 0;
	case asBC_iTOi64:  *(asINT64*)dst = asINT64(*(const int*)a); return 0;
	case asBC_fTOi64:  *(asINT64*)dst = asINT64(*(const float*)a); return 0;
	case asBC_fTOu64:  *(asQWORD*)dst = JIT_fTOu64(*(const float*)a); return 0;
	default: break;
	}

	asINT64 ia = a ? *(const asINT64*)a : 0;
	asINT64 ib = b ? *(const asINT64*)b : 0;
	asQWORD ua = asQWORD(ia), ub = asQWORD(ib);

	switch( op )
	{
	case asBC_NEGi64:  *(asINT64*)dst = -ia; break;
	case asBC_BNOT64:  *(asQWORD*)dst = ~ua; break;
	case asBC_ADDi64:  *(asQWORD*)dst = ua + ub; break;
	case asBC_SUBi64:  *(asQWORD*)dst = ua - ub; break;
	case asBC_MULi64:  *(asQWORD*)dst = ua * ub; break;
	case asBC_BAND64:  *(asQWORD*)dst = ua & ub; break;
	case asBC_BOR64:   *(asQWORD*)dst = ua | ub; break;
	case asBC_BXOR64:  *(asQWORD*)dst = ua ^ ub; break;
	case asBC_DIVi64:
		if( ib == 0 || (ib == -1 && ia == (asINT64(1)<<63)) ) return 1;
		*(asINT64*)dst = ia / ib;
		break;
	case asBC_MODi64:
		if( ib == 0 || (ib == -1 && ia == (asINT64(1)<<63)) ) return 1;
		*(asINT64*)dst = ia % ib;
		break;
	case asBC_DIVu64:
		if( ub == 0 ) return 1;
		*(asQWORD*)dst = ua / ub;
		break;
	case asBC_MODu64:
		if( ub == 0 ) return 1;
		*(asQWORD*)dst = ua % ub;
		break;
	case asBC_CMPi64:  *(int*)dst = ia == ib ? 0 : (ia < ib ? -1 : 1); break;
	case asBC_CMPu64:  *(int*)dst = ua == ub ? 0 : (ua < ub ? -1 : 1); break;
	case asBC_INCi64:  ++*(asQWORD*)dst; break;
	case asBC_DECi64:  --*(asQWORD*)dst; break;
	case asBC_i64TOi:  *(int*)dst = int(ia); break;
	case asBC_dTOi64:  *(asINT64*)dst = asINT64(*(const double*)a); break;
	case asBC_dTOu64:  *(asQWORD*)dst = JIT_dTOu64(*(const double*)a); break;
	case asBC_i64TOf:  *(float*)dst = float(ia); break;
	case asBC_u64TOf:  *(float*)dst = float(ua); break;
	case asBC_i64TOd:  *(double*)dst = double(ia); break;
	case asBC_u64TOd:  *(double*)dst = double(ua); break;
	default:
		return 1;
	}

	return 0;
}

//------------------------------------------------------------------------
// Calls from the application

int JIT_Prepare(asIScriptContext *context, asIScriptFunction *function)
{
	asCContext *ctx = static_cast<asCContext*>(context);
	asCScriptFunction *func = static_cast<asCScriptFunction*>(function);
	asCScriptEngine *engine = ctx->m_engine;
	asSVMRegisters *regs = &ctx->m_regs;

	// This follows asCContext::Prepare for a context that has finished executing,
	// which only has to reset what the execution changed. The context prepares
	// the others, and reports the errors
	if( func == 0 || ctx->m_status != asEXECUTION_FINISHED || func->engine != engine || !engine->isPrepared )
		return ctx->Prepare(func);

	// Only a value returned on the stack or in the object register needs to be released
	if( ctx->m_returnValueSize || regs->objectRegister )
		ctx->CleanReturnObject();

	// Release the object of the previous method, if it is a script object
	asCScriptFunction *prev = ctx->m_initialFunction;
	if( prev->objectType && (prev->objectType->flags & asOBJ_SCRIPT_OBJECT) )
	{
		asCScriptObject *obj = *(asCScriptObject**)regs->stackFramePointer;
		if( obj )
			obj->Release();
		*(asPWORD*)regs->stackFramePointer = 0;
	}

	regs->stackPointer = ctx->m_originalStackPointer;
	ctx->m_stackIndex  = ctx->m_originalStackIndex;

	if( prev != func )
	{
		prev->Release();
		ctx->m_initialFunction = func;
		func->AddRef();

		// Reserve space for the arguments and return value
		ctx->m_argumentsSize = func->GetSpaceNeededForArguments() + (func->objectType ? AS_PTR_SIZE : 0);
		if( func->DoesReturnOnStack() )
		{
			ctx->m_returnValueSize = func->returnType.GetSizeInMemoryDWords();
			ctx->m_argumentsSize += AS_PTR_SIZE;
		}
		else
			ctx->m_returnValueSize = 0;

		// asCContext::ReserveStackSpace is only called if the current stack block may be too small
		asUINT stackSize = ctx->m_argumentsSize + ctx->m_returnValueSize;
		if( func->scriptData )
			stackSize += func->scriptData->stackNeeded;
#ifndef WIP_16BYTE_ALIGN
		if( ctx->m_stackBlocks.GetLength() == 0 || regs->stackPointer - (stackSize + RESERVE_STACK) < ctx->m_stackBlocks[ctx->m_stackIndex] )
#endif
			if( !ctx->ReserveStackSpace(stackSize) )
				return asOUT_OF_MEMORY;

		if( ctx->m_callStack.GetCapacity() < engine->ep.initCallStackSize )
			ctx->m_callStack.AllocateNoConstruct(engine->ep.initCallStackSize * CALLSTACK_FRAME_SIZE, true);
	}
	ctx->m_currentFunction = func;

	// Like asCContext::ClearException, but the string is only assigned if it has to be
	if( ctx->m_exceptionString.GetLength() )
		ctx->m_exceptionString = "";
	ctx->m_exceptionFunction   = 0;
	ctx->m_exceptionLine       = -1;
	ctx->m_exceptionColumn     = -1;
	ctx->m_exceptionSectionIdx = 0;

	// The suspend and abort flags have been reset at the end of the execution
	ctx->m_status = asEXECUTION_PREPARED;
	regs->programPointer = 0;

	regs->stackFramePointer    = regs->stackPointer - ctx->m_argumentsSize - ctx->m_returnValueSize;
	ctx->m_originalStackPointer = regs->stackPointer;
	ctx->m_originalStackIndex   = ctx->m_stackIndex;
	regs->stackPointer         = regs->stackFramePointer;
	if( ctx->m_argumentsSize )
		memset(regs->stackPointer, 0, 4 * ctx->m_argumentsSize);

	if( ctx->m_returnValueSize )
	{
		// The address of the location where the return value should be put
		asDWORD *ptr = regs->stackFramePointer;
		if( func->objectType )
			ptr += AS_PTR_SIZE;
		*(void**)ptr = (void*)(regs->stackFramePointer + ctx->m_argumentsSize);
	}

	return asSUCCESS;
}

// Finds the implementation of a virtual or interface method for the object, like the
// VM. Returns null if there is no object or it doesn't implement the method, which
// the context raises the exception for
static asCScriptFunction *FindMethod(asCScriptFunction *func, asCScriptObject *obj)
{
	if( obj == 0 )
		return 0;

	asCObjectType *objType = obj->objType;
	asCScriptFunction *real = 0;
	if( func->funcType == asFUNC_VIRTUAL )
	{
		if( asUINT(func->vfTableIdx) < objType->virtualFunctionTable.GetLength() )
			real = objType->virtualFunctionTable[func->vfTableIdx];
	}
	else
		real = JIT_FindInterfaceMethod(objType, func);
	return real && real->signatureId == func->signatureId ? real : 0;
}

// Enters the compiled code of the function like the native callers do, but without
// a call state for the caller. The function then finishes the execution when it
// returns, like when it's entered by the VM, and so it does with the call state of
// a nested call on top
static void EnterFromApplication(asSVMRegisters *regs, asCContext *ctx, JITFunction func, asUINT callLimit)
{
#ifdef AS_NO_EXCEPTIONS
	UNUSED_VAR(ctx);
	func(regs, 0, callLimit, regs->stackPointer);
#else
	try
	{
		func(regs, 0, callLimit, regs->stackPointer);
	}
	catch(...)
	{
		if( !CatchDirectCallException(regs, ctx) )
			throw;
	}
#endif
}

// Gives the number of objects known to the garbage collector like
// asCGarbageCollector::GetStatistics, without calling it
struct SGCObjectCount : asCGarbageCollector
{
	static asUINT Get(const asCGarbageCollector &gc)
	{
		return asUINT((gc.*&SGCObjectCount::gcNewObjects).GetLength() + (gc.*&SGCObjectCount::gcOldObjects).GetLength());
	}
};

int JIT_Execute(asIScriptContext *context, const asIJITCompilerAbstract *compiler, asUINT maxNativeCallDepth)
{
	asCContext *ctx = static_cast<asCContext*>(context);
	asCScriptEngine *engine = ctx->m_engine;
	asSVMRegisters *regs = &ctx->m_regs;

	// This follows asCContext::Execute for the script functions compiled by the
	// compiler, when no line callback is set. The context executes the others, and
	// raises the exceptions before the function is entered
	if( ctx->m_status != asEXECUTION_PREPARED || regs->programPointer != 0 || ctx->m_lineCallback || engine->jitCompiler != compiler )
		return ctx->Execute();

	// Find the function to enter like asCContext::SetProgramPointer
	asCScriptFunction *func = ctx->m_currentFunction;
	asCScriptObject *obj;
	bool isDelegate = func->funcType == asFUNC_DELEGATE;
	if( isDelegate )
	{
		obj  = static_cast<asCScriptObject*>(func->objForDelegate);
		func = func->funcForDelegate;
	}
	else
		obj = *(asCScriptObject**)regs->stackFramePointer;
	if( func->funcType == asFUNC_VIRTUAL || func->funcType == asFUNC_INTERFACE )
		func = FindMethod(func, obj);
	if( func == 0 || func->funcType != asFUNC_SCRIPT || func->scriptData->jitFunction == 0 )
		return ctx->Execute();

	// Too many nested calls could fill up the thread call stack
	asCThreadLocalData *tld = asCThreadManager::GetLocalData();
	if( tld == 0 || tld->activeContexts.GetLength() >= engine->ep.maxNestedCalls )
		return ctx->Execute();

	ctx->m_status = asEXECUTION_ACTIVE;
	asCArray<asIScriptContext*> &activeContexts = tld->activeContexts;
	if( activeContexts.length < activeContexts.GetCapacity() )
		activeContexts.array[activeContexts.length++] = ctx;
	else
		activeContexts.PushLast(ctx);

	asUINT gcPreObjects = 0;
	if( engine->ep.autoGarbageCollect )
		gcPreObjects = SGCObjectCount::Get(engine->gc);

	if( isDelegate )
	{
		// Push the object pointer onto the stack
		regs->stackPointer      -= AS_PTR_SIZE;
		regs->stackFramePointer -= AS_PTR_SIZE;
		*(asPWORD*)regs->stackPointer = asPWORD(obj);
	}
	ctx->m_currentFunction = func;
	regs->programPointer   = func->scriptData->byteCode.AddressOf();

	// The native calls may push call states up to the same limit as when the VM enters
	// the function, see JITFunction
	asQWORD words = asQWORD(maxNativeCallDepth) * CALLSTACK_FRAME_SIZE;
	asUINT callLimit = ctx->m_callStack.GetLength() + asUINT(words < 0x40000000 ? words : 0x40000000);
	if( callLimit > ctx->m_callStack.GetCapacity() )
		callLimit = ctx->m_callStack.GetCapacity();
	EnterFromApplication(regs, ctx, reinterpret_cast<JITFunction>(func->scriptData->jitFunction), callLimit);

	// The VM continues where the native code has left it
	for(;;)
	{
		// If an exception was raised that will be caught, then unwind the stack
		// and move the program pointer to the catch block before proceeding
		if( ctx->m_status == asEXECUTION_EXCEPTION && ctx->m_exceptionWillBeCaught )
			ctx->CleanStack(true);
		if( ctx->m_status != asEXECUTION_ACTIVE )
			break;
		ctx->ExecuteNext();
	}

	// A line callback may have been set during the execution
	if( ctx->m_lineCallback )
	{
		ctx->CallLineCallback();
		regs->doProcessSuspend = true;
	}
	else
		regs->doProcessSuspend = false;

	ctx->m_doSuspend = false;

	if( engine->ep.autoGarbageCollect )
	{
		asUINT gcPosObjects = SGCObjectCount::Get(engine->gc);
		if( gcPosObjects > gcPreObjects )
			engine->GarbageCollect(asGC_ONE_STEP | asGC_DESTROY_GARBAGE | asGC_DETECT_GARBAGE, gcPosObjects - gcPreObjects);
		else if( gcPosObjects > 0 )
			engine->GarbageCollect(asGC_ONE_STEP | asGC_DESTROY_GARBAGE | asGC_DETECT_GARBAGE, 1);
	}

	activeContexts.PopLast();

	if( ctx->m_status == asEXECUTION_FINISHED )
	{
		regs->objectType = ctx->m_initialFunction->returnType.GetTypeInfo();
		return asEXECUTION_FINISHED;
	}

	if( ctx->m_doAbort )
	{
		ctx->m_doAbort = false;
		ctx->m_status = asEXECUTION_ABORTED;
		return asEXECUTION_ABORTED;
	}

	if( ctx->m_status == asEXECUTION_SUSPENDED )
		return asEXECUTION_SUSPENDED;

	if( ctx->m_status == asEXECUTION_EXCEPTION )
		return asEXECUTION_EXCEPTION;

	return asERROR;
}

END_AS_NAMESPACE
