#ifndef JIT_AOT_RUNTIME_H
#define JIT_AOT_RUNTIME_H

// Included by the C++ code that CJITCompiler::SetAOTOutput generates, see jit_cppgen.cpp.
// The generated functions follow the protocol of the ones the JIT generates, see
// JITFunction, and execute the bytecode like asCContext::ExecuteNext does. They must
// be compiled with the same configuration as the engine library, as they access the
// internals of the context like the helpers in jit_runtime.cpp do

#include <angelscript.h>
#include "as_context.h"
#include "as_scriptengine.h"
#include "as_scriptfunction.h"
#include "as_objecttype.h"
#include "as_scriptobject.h"
#include "as_callfunc.h"
#include "jit.h"
#include "jit_runtime.h"

#include <math.h>
#include <string.h>
#include <stddef.h>
#if defined(_MSC_VER) && !defined(AS_NO_THREADS) && !defined(AS_NO_ATOMIC)
#include <intrin.h>
#endif

BEGIN_AS_NAMESPACE

// The value register, kept in the local variable vr of type asQWORD, which the
// compiler keeps in a register. The VM accesses the first bytes in memory through
// casts. The writes of the smaller types clear the rest like the JIT does, as the
// bytecode never reads more than it wrote
#ifdef AS_BIG_ENDIAN
#define AOT_VR_SHIFT(T) (64 - 8 * sizeof(T))
#else
#define AOT_VR_SHIFT(T) 0
#endif
#define AOT_GETVR(T)    ((T)(vr >> AOT_VR_SHIFT(T)))
#define AOT_SETVR(T, x) (vr = (asQWORD)(T)(x) << AOT_VR_SHIFT(T))

// The variables and the stack hold values of any type, which the VM accesses through
// casts. The types used for that may alias anything, and the 64bit ones are only
// aligned to 32 bits like the stack
#if defined(__GNUC__) || defined(__clang__)
typedef asBYTE      __attribute__((may_alias)) aot_u8;
typedef signed char __attribute__((may_alias)) aot_i8;
typedef asWORD      __attribute__((may_alias)) aot_u16;
typedef short       __attribute__((may_alias)) aot_i16;
typedef asDWORD     __attribute__((may_alias)) aot_u32;
typedef int         __attribute__((may_alias)) aot_i32;
typedef float       __attribute__((may_alias)) aot_f32;
typedef asQWORD     __attribute__((may_alias, aligned(4))) aot_u64;
typedef asINT64     __attribute__((may_alias, aligned(4))) aot_i64;
typedef double      __attribute__((may_alias, aligned(4))) aot_f64;
typedef asPWORD     __attribute__((may_alias, aligned(4))) aot_pw;
#else
typedef asBYTE      aot_u8;
typedef signed char aot_i8;
typedef asWORD      aot_u16;
typedef short       aot_i16;
typedef asDWORD     aot_u32;
typedef int         aot_i32;
typedef float       aot_f32;
typedef asQWORD     aot_u64;
typedef asINT64     aot_i64;
typedef double      aot_f64;
typedef asPWORD     aot_pw;
#endif

// A variable of the frame, a value on the stack, and a value where the value register points
#define AOT_V(T, off) (*(aot_##T*)(fp - (off)))
#define AOT_S(T, off) (*(aot_##T*)(sp + (off)))
#define AOT_R(T)      (*(aot_##T*)AOT_GETVR(asPWORD))

// The operands that differ between the functions sharing the code, e.g. pointers
// and function ids, are read from the bytecode like the VM does. pos is the offset
// of the dword in the bytecode
#define AOT_PW(pos)  (*(const aot_pw*)(bc + (pos)))
#define AOT_INT(pos) (*(const aot_i32*)(bc + (pos)))
#define AOT_DW(pos)  (*(const aot_u32*)(bc + (pos)))

// Stores the registers kept in local variables, with the program pointer at the
// instruction at pos, and returns to the VM, which continues there
#define AOT_SYNC(pos) (regs->programPointer = bc + (pos), regs->stackPointer = sp, regs->valueRegister = vr)
#define AOT_BAIL(pos) do { AOT_SYNC(pos); return 1; } while(0)

// Stores the frame of the function, self being the function, which is left to the
// places where the VM or the engine may see it, see JIT_FRAME_BIT
#define AOT_FRAME() (regs->stackFramePointer = fp, ctx->m_currentFunction = self)

// The registers after a call, which the called function or the helper has updated
#define AOT_RELOAD() (sp = regs->stackPointer, vr = regs->valueRegister)

// The calling convention of the registered functions that the code calls directly,
// see CJITCppGen::GetSystemCall, in case the default is another one
#if defined(AS_X86) && defined(_MSC_VER)
#define AOT_CDECL __cdecl
#elif defined(AS_X86) && defined(__GNUC__)
#define AOT_CDECL __attribute__((cdecl))
#else
#define AOT_CDECL
#endif

// Booleans. The VM writes the byte of a boolean and clears the rest of the dword,
// or of the value register for the tests
#if AS_SIZEOF_BOOL == 1
#define AOT_NOT(off) do { asBYTE v_ = AOT_V(u8, off) == 0 ? VALUE_OF_BOOLEAN_TRUE : 0; AOT_V(u32, off) = 0; AOT_V(u8, off) = v_; } while(0)
#define AOT_NOTL(l) ((l) = ((asBYTE)(l) == 0 ? VALUE_OF_BOOLEAN_TRUE : 0))
#define AOT_TEST(cond) AOT_SETVR(asBYTE, (cond) ? VALUE_OF_BOOLEAN_TRUE : 0)
#define AOT_CLRHI() AOT_SETVR(asBYTE, AOT_GETVR(asBYTE))
#else
#define AOT_NOT(off) (AOT_V(u32, off) = AOT_V(u32, off) == 0 ? VALUE_OF_BOOLEAN_TRUE : 0)
#define AOT_NOTL(l) ((l) = ((l) == 0 ? VALUE_OF_BOOLEAN_TRUE : 0))
#define AOT_TEST(cond) AOT_SETVR(asDWORD, (cond) ? VALUE_OF_BOOLEAN_TRUE : 0)
#define AOT_CLRHI() ((void)0)
#endif

// The requests to suspend, which the loops read on every iteration. The code
// keeps the variables in registers there, so nothing else would make the compiler
// read the flag again
#define AOT_SUSPENDING() (*(volatile bool*)&regs->doProcessSuspend)

// Constants given by their bits, and the bits of the values
inline float aot_f32bits(asDWORD bits)
{
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

inline double aot_f64bits(asQWORD bits)
{
	double d;
	memcpy(&d, &bits, 8);
	return d;
}

inline asDWORD aot_bits32(float f)
{
	asDWORD bits;
	memcpy(&bits, &f, 4);
	return bits;
}

inline asQWORD aot_bits64(double d)
{
	asQWORD bits;
	memcpy(&bits, &d, 8);
	return bits;
}

// The comparisons, which set the value register to -1, 0, or 1
template<class T> inline int aot_cmp(T a, T b)
{
	return a == b ? 0 : (a < b ? -1 : 1);
}

// Pushes the call state like asCContext::PushCallState, with the frame of the
// calling function, which may not have been stored. The call stack must have room
// for it, i.e. its length must be below callLimit
inline void AOT_PushCall(asCContext *ctx, asUINT length, asCScriptFunction *caller, asDWORD *fp, asDWORD *pc, asDWORD *sp)
{
	size_t *s = ctx->m_callStack.AddressOf() + length;
	s[0] = (size_t)fp;
	s[1] = (size_t)caller;
	s[2] = (size_t)pc;
	s[3] = (size_t)sp;
	s[4] = (size_t)ctx->m_stackIndex;
	ctx->m_callStack.SetLengthNoAllocate(length + CALLSTACK_FRAME_SIZE);
}

// Calls the code of a script function natively, see JITFunction. The called
// function restores the frame of the caller when it returns
inline int AOT_CallNative(asSVMRegisters *regs, asCContext *ctx, asUINT length, asCScriptFunction *caller, asCScriptFunction *callee,
                          JITFunction target, asDWORD *fp, asDWORD *pc, asDWORD *sp, asUINT callLimit)
{
	AOT_PushCall(ctx, length, caller, fp, pc, sp);
	ctx->m_currentFunction = callee;
#if AS_PTR_SIZE == 1
	regs->stackPointer = sp;
#endif
	return target(regs, 0, callLimit, sp);
}

// The code generated for a function is also called directly by the code of the
// functions calling it, which pushes the call state and passes the function and the
// arguments. Unless it returns to the VM, the function returns the value register in
// the VM registers and only pops the call state, and the caller pops the arguments
// and stores its frame where it is seen, like JIT_NATIVE_RETURN. The direct entries
// have the signature
//
//   int name_d(asCContext *ctx, asCScriptFunction *self, asDWORD *fp, asUINT callLimit)
inline void AOT_PopCall(asCContext *ctx)
{
	asUINT length = ctx->m_callStack.GetLength() - CALLSTACK_FRAME_SIZE;
	ctx->m_stackIndex = (int)ctx->m_callStack.AddressOf()[length + 4];
	ctx->m_callStack.SetLengthNoAllocate(length);
}

// The implementation of a virtual or interface method for the object, like
// asCContext::CallInterfaceMethod, or null if the object doesn't implement it
inline asCScriptFunction *AOT_Virtual(asCScriptObject *obj, asCScriptFunction *func)
{
	asCObjectType *type = obj->objType;
	if( func->funcType != asFUNC_INTERFACE )
		return type->virtualFunctionTable[func->vfTableIdx];
	return JIT_FindInterfaceMethod(type, func);
}

// The references of the script objects, which the code counts in place like the
// JIT does, see CJITByteCode::FindInPlaceRefCounts. Like asCScriptObject::AddRef
// and Release the flag of the GC is cleared. They return false for what is left to
// those, see JIT_AddRefScriptObject and JIT_ReleaseScriptObject: the objects that
// are being destroyed, which AddRef reports, and the last reference, for which
// Release destroys the object. With atomic reference counts another thread may
// release a reference between the check and the decrement, which is given back
// then. The atomic operations are those of asAtomicInc and asAtomicDec
struct SAOTScriptObject : asCScriptObject
{
	static bool AddRefInPlace(void *obj)
	{
		SAOTScriptObject *o = static_cast<SAOTScriptObject*>(obj);
		if( o->hasRefCountReachedZero )
			return false;
		o->gcFlag = false;
		Inc(o);
		return true;
	}

	static bool ReleaseInPlace(void *obj)
	{
		SAOTScriptObject *o = static_cast<SAOTScriptObject*>(obj);
		o->gcFlag = false;
		if( *Count(o) <= 1 )
			return false;
		if( Dec(o) == 0 )
		{
			Inc(o);
			return false;
		}
		return true;
	}

	static volatile asDWORD *Count(SAOTScriptObject *o)
	{
		return reinterpret_cast<volatile asDWORD*>(&o->refCount);
	}

	static asDWORD Inc(SAOTScriptObject *o)
	{
#if defined(AS_NO_THREADS) || defined(AS_NO_ATOMIC)
		return ++*Count(o);
#elif defined(_MSC_VER)
		return asDWORD(_InterlockedIncrement(reinterpret_cast<volatile long*>(Count(o))));
#elif defined(__GNUC__) || defined(__clang__)
		return __atomic_add_fetch(Count(o), 1, __ATOMIC_SEQ_CST);
#else
		return o->refCount.atomicInc();
#endif
	}

	static asDWORD Dec(SAOTScriptObject *o)
	{
#if defined(AS_NO_THREADS) || defined(AS_NO_ATOMIC)
		return --*Count(o);
#elif defined(_MSC_VER)
		return asDWORD(_InterlockedDecrement(reinterpret_cast<volatile long*>(Count(o))));
#elif defined(__GNUC__) || defined(__clang__)
		return __atomic_sub_fetch(Count(o), 1, __ATOMIC_SEQ_CST);
#else
		return o->refCount.atomicDec();
#endif
	}
};
static_assert(sizeof(asCAtomic) == sizeof(asDWORD), "the reference counts are changed as 32bit integers");

inline bool AOT_AddRef(void *obj)  { return SAOTScriptObject::AddRefInPlace(obj); }
inline bool AOT_Release(void *obj) { return SAOTScriptObject::ReleaseInPlace(obj); }

// The call limit when the VM enters a function, like the JIT computes it
inline asUINT AOT_CallLimit(asCContext *ctx)
{
	asQWORD words = asQWORD(JIT_nativeCallDepth) * CALLSTACK_FRAME_SIZE;
	asUINT limit = ctx->m_callStack.GetLength() + asUINT(words < 0x40000000 ? words : 0x40000000);
	asUINT capacity = ctx->m_callStack.GetCapacity();
	return limit < capacity ? limit : capacity;
}

END_AS_NAMESPACE

#endif
