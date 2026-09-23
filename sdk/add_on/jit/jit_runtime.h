#ifndef JIT_RUNTIME_H
#define JIT_RUNTIME_H

#ifndef ANGELSCRIPT_H
#include <angelscript.h>
#endif

BEGIN_AS_NAMESPACE

class asCObjectType;

// Runtime helpers called from the generated code. This is the only part of the
// JIT that touches the internals of the script context, so the generated code
// itself only depends on the public asSVMRegisters structure.
//
// All helpers that may hand control back to the VM return 0 when the native
// code should continue, and a non-zero value when the native code must return
// to the VM immediately. The VM registers have then been updated by the helper.
//
// Unless otherwise noted the generated code must have stored the current stack
// pointer and value register in the VM registers, and set the program pointer to
// the instruction being executed, before calling a helper.

// Kinds of calls handled by JIT_CallScript
enum EJITCallKind
{
	JIT_CALL_SCRIPT    = 0, // asBC_CALL:     funcId is the function id
	JIT_CALL_INTERFACE = 1, // asBC_CALLINTF: funcId is the function id
	JIT_CALL_BOUND     = 2, // asBC_CALLBND:  funcId is the imported function id
	JIT_CALL_PTR       = 3, // asBC_CallPtr:  extra is the function pointer (may be null)
	JIT_CALL_ALLOC     = 4  // asBC_ALLOC:    funcId is the constructor, extra is the asCObjectType
};

// asBC_CALLSYS
int    JIT_CallSystem(asSVMRegisters *regs, int funcId) noexcept;

// asBC_Thiscall1. The object pointer on the stack must have been verified to be non-null
int    JIT_Thiscall1(asSVMRegisters *regs, int funcId) noexcept;

// Called after a registered function has been called directly by the generated
// code when regs->doProcessSuspend is set, i.e. when an exception was raised, a
// suspension was requested, or a line callback is set
int    JIT_AfterDirectCall(asSVMRegisters *regs, int funcId) noexcept;

// Offset of the member of the context that holds the registered function being
// called. The generated code sets it around direct calls so that the function can
// raise script exceptions
size_t JIT_CallingSystemFunctionOffset() noexcept;

// Script function calls. Performs the call and, if possible, executes the
// called function natively before returning. Returns 0 if the call completed
int    JIT_CallScript(asSVMRegisters *regs, int kind, int funcId, asPWORD extra) noexcept;

// asBC_RET. The generated code must return to its caller afterwards
void   JIT_Return(asSVMRegisters *regs, asUINT popSize) noexcept;

// asBC_SUSPEND, when regs->doProcessSuspend is set
int    JIT_Suspend(asSVMRegisters *regs) noexcept;

// asBC_ALLOC for registered types
int    JIT_Alloc(asSVMRegisters *regs, asCObjectType *objType, int funcId) noexcept;

// asBC_FREE. var is the address of the variable holding the object
void   JIT_Free(asSVMRegisters *regs, asCObjectType *objType, asPWORD *var) noexcept;

// asBC_REFCPY and asBC_RefCpyV
void   JIT_RefCpy(asSVMRegisters *regs, asCObjectType *objType, void **dst, void *src) noexcept;

// asBC_Cast. handle is the address of the handle to cast, may be null
void   JIT_Cast(asSVMRegisters *regs, void **handle, asDWORD typeId) noexcept;

// asBC_AllocMem
void  *JIT_AllocMem(asUINT size) noexcept;

// asBC_COPY
void   JIT_MemCpy(void *dst, const void *src, asUINT sizeInBytes) noexcept;

// Power operators. Like the VM the result is stored even on overflow, so the
// instruction cannot be re-executed to raise the exception. Instead the helper
// raises it and returns non-zero, and the generated code returns to the VM
int    JIT_POWi(asSVMRegisters *regs, int *dst, int a, int b) noexcept;
int    JIT_POWu(asSVMRegisters *regs, asDWORD *dst, asDWORD a, asDWORD b) noexcept;
int    JIT_POWi64(asSVMRegisters *regs, asINT64 *dst, const asINT64 *a, const asINT64 *b) noexcept;
int    JIT_POWu64(asSVMRegisters *regs, asQWORD *dst, const asQWORD *a, const asQWORD *b) noexcept;
int    JIT_POWf(asSVMRegisters *regs, float *dst, float a, float b) noexcept;
int    JIT_POWd(asSVMRegisters *regs, double *dst, double a, double b) noexcept;
int    JIT_POWdi(asSVMRegisters *regs, double *dst, double a, int b) noexcept;

// Floating point modulo. The divisor must have been verified to be non-zero
float  JIT_MODf(float a, float b) noexcept;
double JIT_MODd(double a, double b) noexcept;

// Conversions with no direct machine instruction
asQWORD JIT_fTOu64(float f) noexcept;
asQWORD JIT_dTOu64(double d) noexcept;
float   JIT_u64TOf(asQWORD v) noexcept;
double  JIT_u64TOd(asQWORD v) noexcept;
asUINT  JIT_fTOu(float f) noexcept;
asUINT  JIT_dTOu(double d) noexcept;
float   JIT_uTOf(asUINT v) noexcept;
double  JIT_uTOd(asUINT v) noexcept;

// 64bit integer operations for hosts without 64bit registers. The operands are
// passed by address. The op is the bytecode instruction. Returns non-zero when
// the VM must re-execute the instruction to raise an exception
int    JIT_I64Op(int op, void *dst, const void *a, const void *b) noexcept;

// Depth of native script-to-script calls in the current thread
void   JIT_SetMaxNativeCallDepth(asUINT depth) noexcept;

END_AS_NAMESPACE

#endif
