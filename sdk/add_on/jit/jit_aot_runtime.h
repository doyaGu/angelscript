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
#include "jit.h"
#include "jit_runtime.h"

#include <math.h>
#include <string.h>
#include <stddef.h>

BEGIN_AS_NAMESPACE

// The value register, kept in a local variable
union UJITAOTValue
{
	asQWORD     q;
	asINT64     i64;
	double      d;
	float       f;
	asDWORD     u32;
	int         i32;
	asWORD      w;
	short       s;
	asBYTE      b;
	signed char c;
	asPWORD     pw;
	void       *p;
	asBYTE      bytes[8];
};

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
#define AOT_R(T)      (*(aot_##T*)vr.p)

// The operands that differ between the functions sharing the code, e.g. pointers
// and function ids, are read from the bytecode like the VM does. pos is the offset
// of the dword in the bytecode
#define AOT_PW(pos)  (*(const aot_pw*)(bc + (pos)))
#define AOT_INT(pos) (*(const aot_i32*)(bc + (pos)))
#define AOT_DW(pos)  (*(const aot_u32*)(bc + (pos)))

// Stores the registers kept in local variables, with the program pointer at the
// instruction at pos, and returns to the VM, which continues there
#define AOT_SYNC(pos) (regs->programPointer = bc + (pos), regs->stackPointer = sp, regs->valueRegister = vr.q)
#define AOT_BAIL(pos) do { AOT_SYNC(pos); return 1; } while(0)

// The registers after a call, which the called function or the helper has updated
#define AOT_RELOAD() (sp = regs->stackPointer, vr.q = regs->valueRegister)

// Booleans. The VM writes the byte of a boolean and clears the rest of the dword,
// or of the value register for the tests
#if AS_SIZEOF_BOOL == 1
#define AOT_NOT(off) do { asBYTE v_ = AOT_V(u8, off) == 0 ? VALUE_OF_BOOLEAN_TRUE : 0; AOT_V(u32, off) = 0; AOT_V(u8, off) = v_; } while(0)
#define AOT_NOTL(l) ((l) = ((asBYTE)(l) == 0 ? VALUE_OF_BOOLEAN_TRUE : 0))
#define AOT_TEST(cond) do { asBYTE v_ = (cond) ? VALUE_OF_BOOLEAN_TRUE : 0; vr.q = 0; vr.bytes[0] = v_; } while(0)
#define AOT_CLRHI() (vr.bytes[1] = 0, vr.bytes[2] = 0, vr.bytes[3] = 0)
#else
#define AOT_NOT(off) (AOT_V(u32, off) = AOT_V(u32, off) == 0 ? VALUE_OF_BOOLEAN_TRUE : 0)
#define AOT_NOTL(l) ((l) = ((l) == 0 ? VALUE_OF_BOOLEAN_TRUE : 0))
#define AOT_TEST(cond) (vr.i32 = (cond) ? VALUE_OF_BOOLEAN_TRUE : 0)
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

// Pushes the call state like asCContext::PushCallState and calls the code of a
// script function natively, see JITFunction. The call stack must have room for the
// call state, i.e. its length must be below callLimit
inline int AOT_CallNative(asSVMRegisters *regs, asCContext *ctx, asCScriptFunction *callee, JITFunction target,
                          asDWORD *fp, asDWORD *pc, asDWORD *sp, asUINT callLimit)
{
	asUINT length = ctx->m_callStack.GetLength();
	size_t *s = ctx->m_callStack.AddressOf() + length;
	s[0] = (size_t)fp;
	s[1] = (size_t)ctx->m_currentFunction;
	s[2] = (size_t)pc;
	s[3] = (size_t)sp;
	s[4] = (size_t)ctx->m_stackIndex;
	ctx->m_callStack.SetLengthNoAllocate(length + CALLSTACK_FRAME_SIZE);
	ctx->m_currentFunction = callee;
#if AS_PTR_SIZE == 1
	regs->stackPointer = sp;
#endif
	return target(regs, 0, callLimit, sp);
}

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
