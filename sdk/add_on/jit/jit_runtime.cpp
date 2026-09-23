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
		CALLSTACK_FRAME_SIZE,
		RESERVE_STACK
	};
	return layout;
}
#undef JIT_CTX_OFFSET

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
		int(offsetof(asCScriptFunction::ScriptFunctionData, jitFunction))
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

// Calls a script function. The program pointer must be after the call instruction
// and the arguments on the stack. If the function has been compiled and the depth
// allows it the function is executed natively, otherwise the VM is left to do it.
// Returns 0 if the function has returned already
static int EnterScriptFunction(asSVMRegisters *regs, asCContext *ctx, asCScriptFunction *func, asUINT depth)
{
	JITFunction jitFunc = reinterpret_cast<JITFunction>(func->scriptData->jitFunction);
	if( jitFunc == 0 || depth == 0 )
	{
		ctx->CallScriptFunction(func);
		return 1;
	}

	if( ctx->PushCallState() < 0 )
		return 1;
	return jitFunc(regs, 0, depth - 1);
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

	for( asUINT n = 0; n < objType->interfaces.GetLength(); n++ )
		if( objType->interfaces[n] == func->objectType )
			return objType->virtualFunctionTable[func->vfTableIdx + objType->interfaceVFTOffsets[n]];

	ctx->m_needToCleanupArgs = true;
	ctx->SetInternalException(TXT_NULL_POINTER_ACCESS);
	return 0;
}

int JIT_CallScript(asSVMRegisters *regs, int kind, int funcId, asPWORD extra, asUINT depth) noexcept
{
	asCContext *ctx = GetContext(regs);
	asCScriptEngine *engine = ctx->m_engine;

	// The program pointer is at the call instruction. The code below mirrors
	// the VM implementation of the respective instructions
	switch( kind )
	{
	case JIT_CALL_SCRIPT:
		regs->programPointer += 2;
		return EnterScriptFunction(regs, ctx, engine->scriptFunctions[funcId], depth);

	case JIT_CALL_INTERFACE:
		{
			regs->programPointer += 2;
			asCScriptFunction *func = ResolveVirtual(regs, ctx, engine->scriptFunctions[funcId]);
			if( func == 0 )
				return 1;
			return EnterScriptFunction(regs, ctx, func, depth);
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
				return EnterScriptFunction(regs, ctx, func, depth);
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
				return EnterScriptFunction(regs, ctx, func, depth);
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
					return EnterScriptFunction(regs, ctx, method, depth);
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
					return EnterScriptFunction(regs, ctx, engine->scriptFunctions[boundId], depth);
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
			asCObjectType *objType = reinterpret_cast<asCObjectType*>(extra);
			asDWORD *mem = (asDWORD*)engine->CallAlloc(objType);
			ScriptObject_Construct(objType, (asCScriptObject*)mem);

			asCScriptFunction *f = engine->scriptFunctions[funcId];
			asDWORD **a = (asDWORD**)*(asPWORD*)(regs->stackPointer + f->GetSpaceNeededForArguments());
			if( a ) *a = mem;

			regs->stackPointer -= AS_PTR_SIZE;
			*(asPWORD*)regs->stackPointer = (asPWORD)mem;
			regs->programPointer += 2 + AS_PTR_SIZE;
			return EnterScriptFunction(regs, ctx, f, depth);
		}

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

void JIT_Return(asSVMRegisters *regs, asUINT popSize) noexcept
{
	asCContext *ctx = GetContext(regs);

	asUINT length = ctx->m_callStack.GetLength();
	if( length == 0 || ctx->m_callStack[length - CALLSTACK_FRAME_SIZE] == 0 )
	{
		// The function was called by the application, or as a nested call
		ctx->m_status = asEXECUTION_FINISHED;
		return;
	}

	ctx->PopCallState();
	regs->stackPointer += popSize;
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

END_AS_NAMESPACE
