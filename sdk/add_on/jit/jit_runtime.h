#ifndef JIT_RUNTIME_H
#define JIT_RUNTIME_H

#ifndef ANGELSCRIPT_H
#include <angelscript.h>
#endif

BEGIN_AS_NAMESPACE

class asCObjectType;
class asCScriptFunction;

// Runtime helpers called from the generated code. This is the only part of the
// JIT that touches the internals of the script context, so the generated code
// itself only depends on the public asSVMRegisters structure.
//
// All helpers that may hand control back to the VM return 0 when the native
// code should continue, and a non-zero value when the native code must return
// to the VM immediately. The VM registers have then been updated by the helper.
//
// Unless otherwise noted the generated code must have stored the frame, see
// JITFunction, and the current stack pointer and value register in the VM
// registers, and set the program pointer to the instruction being executed, before
// calling a helper.

// Signature of the generated functions. The VM calls them as asJITFunction, with
// the 1-based index of the entry point in jitArg, and ignores the return value.
// Native callers push the call state of the caller like asCContext::PushCallState,
// set the current function of the context to the called function, and call them
// with jitArg 0, the arguments on the script stack, and their stack pointer in
// stackPointer, which the VM registers may not have on 64bit hosts. Only the
// functions generated ahead of time need the current function, as they are shared
// by the functions with the same bytecode, so the generated code only sets it if
// the compiler has them, see SJITCodeGenOptions::interop. On 32bit hosts the
// generated code leaves stackPointer out and has the stack pointer in the VM
// registers. The VM leaves callLimit and stackPointer undefined. The function then
// sets up its frame the way asCContext::PrepareScriptFunction does, but writes the
// frame, i.e. the stack frame pointer and the current function of the context,
// back only where the VM or the engine may see it. On 64bit hosts native callers
// may mark the call state by setting the sign bit of the stack index, whose upper
// half the VM ignores. The function then returns without restoring the frame, the
// program pointer, the stack pointer, and the length of the call stack of the
// caller, which keeps them itself.
// The VM entry clears the mark of the call state on top, as the function doesn't
// return to a native caller once the VM has executed it. The functions generated
// ahead of time restore everything whether the call state is marked or not, and
// don't return the value register natively, so the generated code doesn't mark the
// call states if the compiler has them. callLimit is the length of the call stack
// up to which further native calls may push call states. The VM entry sets it to
// the capacity of the call stack, which is a multiple of the size of a call state
// and doesn't shrink, or less to allow no more than the maximum number of nested
// native calls. The return value is 0 if the function returned to its caller, and
// non-zero if the VM must take over, in which case the VM registers describe where
// to continue. The engine calls an immutable wrapper that dispatches to the current
// code and counts entries while compilation is deferred.
typedef int (*JITFunction)(asSVMRegisters *regs, asPWORD jitArg, asUINT callLimit, asDWORD *stackPointer);

// Returns the code behind the immutable wrapper of a compiled script function,
// or null while its compilation is still deferred.
asJITFunction JIT_GetNativeTarget(asCScriptFunction *func);

// Maximum depth of nested native calls when the VM enters one of the functions
// generated ahead of time, see CJITCompiler::SetNativeCallDepth. It is shared by all
// compilers, like the functions
extern asUINT JIT_nativeCallDepth;

// Layout of the context members that the generated code accesses directly. The
// offsets are relative to the VM registers, which are embedded in the context
struct SJITContextLayout
{
	int callStackArray;        // asPWORD*, the call stack
	int callStackLength;       // asUINT, used length of the call stack in words
	int callStackCapacity;     // asUINT
	int currentFunction;       // asCScriptFunction*
	int stackIndex;            // asUINT, index of the current stack block
	int stackBlocks;           // asDWORD**, the stack blocks
	int callingSystemFunction; // asCScriptFunction*, the registered function being called
	int status;                // asEContextState, 32 bits
	int callStackFrameSize;    // words per call state
	int reserveStack;          // dwords that must remain free on the stack block
};

const SJITContextLayout &JIT_GetContextLayout() noexcept;

// Offsets of the members of other engine objects that the generated code accesses directly
struct SJITObjectLayout
{
	int objectType;           // asCObjectType* in asCScriptObject
	int virtualFunctionTable; // asCScriptFunction** in asCObjectType
	int interfaces;           // asCObjectType** in asCObjectType
	int interfaceCount;       // asUINT in asCObjectType
	int interfaceVFTOffsets;  // asUINT* in asCObjectType, where the methods of each interface start in the virtual function table
	int funcType;             // asEFuncType in asCScriptFunction
	int scriptData;           // ScriptFunctionData* in asCScriptFunction
	int jitFunction;          // asJITFunction in ScriptFunctionData
	int objForDelegate;       // void* in asCScriptFunction, the object of a delegate
	int funcForDelegate;      // asCScriptFunction* in asCScriptFunction, the method of a delegate
	int vfTableIdx;           // int in asCScriptFunction, where a virtual method is in the virtual function table
	int refCount;             // asCAtomic in asCScriptObject
	int gcFlag;               // byte of the gcFlag bit field in asCScriptObject
	int gcFlagMask;
	int deadFlag;             // byte of the hasRefCountReachedZero bit field in asCScriptObject
	int deadFlagMask;
	int atomicRefCount;       // 1 if the engine changes the reference counts with atomic operations
};

const SJITObjectLayout &JIT_GetObjectLayout() noexcept;

// Kinds of calls handled by JIT_CallScript
enum EJITCallKind
{
	JIT_CALL_SCRIPT    = 0, // asBC_CALL:     funcId is the function id
	JIT_CALL_INTERFACE = 1, // asBC_CALLINTF: funcId is the function id
	JIT_CALL_BOUND     = 2, // asBC_CALLBND:  funcId is the imported function id
	JIT_CALL_PTR       = 3, // asBC_CallPtr:  extra is the function pointer (may be null)
	JIT_CALL_ALLOC     = 4, // asBC_ALLOC:    funcId is the constructor, extra is the asCObjectType
	JIT_CALL_CONSTRUCT = 5  // asBC_ALLOC:    funcId is the constructor, the object has been
	                        //                allocated and pushed already, see JIT_NewScriptObject
};

// asBC_CALLSYS
int    JIT_CallSystem(asSVMRegisters *regs, int funcId) noexcept;

// asBC_Thiscall1. The object pointer on the stack must have been verified to be non-null
int    JIT_Thiscall1(asSVMRegisters *regs, int funcId) noexcept;

// Called after a registered function has been called directly by the generated
// code when regs->doProcessSuspend is set, i.e. when an exception was raised, a
// suspension was requested, or a line callback is set. The generated code sets
// callingSystemFunction in the context around the direct calls, so that the
// function can raise script exceptions. retPointer is the location of a value
// type returned by value, or null
int    JIT_AfterDirectCall(asSVMRegisters *regs, int funcId, void *retPointer) noexcept;

// The corresponding status check after a registered constructor called directly
// by the AOT code. The allocation has already been stored at dst. On exception it
// is freed and the destination is cleared, like JIT_Alloc does
int    JIT_AfterDirectAlloc(asSVMRegisters *regs, void *mem, void **dst) noexcept;

// Set in jitArg when the generated code is entered through JIT_GuardedEntry
const asPWORD JIT_GUARDED_ENTRY = 0x40000000;

// The arguments of the JitEntry instructions have the index of the entry point.
const asPWORD JIT_ENTRY_INDEX_MASK  = 0xFFFF;
const asUINT  JIT_ENTRY_MAX_COUNT   = 0x3FFF;

// Catches the C++ exceptions thrown by registered functions that the generated
// code calls directly, and turns them into script exceptions like CallSystemFunction
// does. When entered by the VM without JIT_GUARDED_ENTRY in jitArg the generated
// code returns the result of this function, which enters it again with the bit set.
// The exception unwinds all the functions executed natively since, so the VM
// registers are updated here the way the VM does after the call. Exceptions thrown
// outside of the direct calls are passed on
int    JIT_GuardedEntry(asSVMRegisters *regs, asPWORD jitArg);

#ifndef AS_NO_EXCEPTIONS
// Catches the C++ exceptions like JIT_GuardedEntry, for the code generated ahead of
// time, which catches them where it is entered, see CJITCppGen. Returns false if the
// exception must be passed on
bool   JIT_CatchException(asSVMRegisters *regs);
#endif

// Script function calls. Performs the call and, if possible, executes the called
// function natively before returning. callLimit is the one of the calling function,
// see JITFunction. Returns 0 if the call completed. Not noexcept, as the C++
// exceptions caught by JIT_GuardedEntry may pass through it
int    JIT_CallScript(asSVMRegisters *regs, int kind, int funcId, asPWORD extra, asUINT callLimit);

// The implementation of the interface method in the class, or null if the class
// doesn't implement the interface
asCScriptFunction *JIT_FindInterfaceMethod(asCObjectType *objType, asCScriptFunction *func) noexcept;

// The script function currently bound to an imported function, or null if it is
// unbound, bound to a registered function, or has no compiled entry
asCScriptFunction *JIT_GetBoundScriptFunction(asIScriptEngine *engine, int funcId) noexcept;

// Sets up the frame of a function entered natively with jitArg 0, when the stack
// block is too small or regs->doProcessSuspend is set. The stack pointer in the VM
// registers must be the one the function was called with, and the program pointer
// the start of the function
int    JIT_PrepareFrame(asSVMRegisters *regs) noexcept;

// asBC_SUSPEND, when regs->doProcessSuspend is set
int    JIT_Suspend(asSVMRegisters *regs) noexcept;

// Hands a function inlined into the calling function to the VM, which continues it
// at the program pointer with the stack pointer in the VM registers. The frame of the
// caller must have been stored. Pushes its call state like the call would have, with
// callerPC after the call and the frame of the function as stack pointer. The call
// stack must have room for it without growing. The parameters in the mask borrowed
// the references of the caller, see JIT_OwnParams
void   JIT_ExitInlined(asSVMRegisters *regs, asCScriptFunction *func, asDWORD *frame, asDWORD *callerPC, asUINT borrowed) noexcept;

// Adds a reference to the objects of the handle parameters in the mask, in a frame
// of the function whose parameters borrowed the references of the caller, before
// the VM or the called function releases them, see CJITByteCode::AnalyseBorrows
// and CJITByteCode::AnalyseForAOT
void   JIT_OwnParams(asCScriptFunction *func, asDWORD *frame, asUINT mask) noexcept;

// JIT_OwnParams for the frames of the inlined functions on the call stack, whose
// call states note the borrowed parameters in the upper half of the stack index.
// Goes from the innermost frame down to the frame of the function in rootFunc and
// rootFrame, or to a nested call, and clears the notes
void   JIT_OwnBorrowed(asSVMRegisters *regs, asDWORD *rootFrame, asCScriptFunction *rootFunc) noexcept;

// asBC_ALLOC for registered types
int    JIT_Alloc(asSVMRegisters *regs, asCObjectType *objType, int funcId) noexcept;

// asBC_ALLOC for script classes, before the constructor is called. The registers
// must have been synced, as the allocation may reuse the context for nested calls
void  *JIT_NewScriptObject(asCObjectType *objType) noexcept;

// asBC_FREE. var is the address of the variable holding the object
void   JIT_Free(asSVMRegisters *regs, asCObjectType *objType, asPWORD *var) noexcept;

// asBC_REFCPY and asBC_RefCpyV
void   JIT_RefCpy(asSVMRegisters *regs, asCObjectType *objType, void **dst, void *src) noexcept;

// Returns the size of a value parameter passed inline by a supported native ABI,
// zero if it is passed indirectly, or -1 if direct calls don't support it. When
// requested, floating says whether the bits are passed in a floating-point slot
int    JIT_GetInlineValueArgSize(asCScriptFunction *func, asUINT param, bool *floating = 0) noexcept;

// Returns the number of argument cleanups of a direct system call, or -1 if they
// include value objects that the direct call cannot pass. allowInlineValues is for
// backends that copy supported inline values out of their script objects
int    JIT_GetSystemCallCleanupCount(asCScriptFunction *func, bool allowInlineValues) noexcept;

// Frees the temporary script object after its inline value has been loaded for a
// direct system call. The native copy owns the value from then on
void   JIT_FreeValueArg(asSVMRegisters *regs, void *obj) noexcept;

// Cleans the value objects and auto handles of a direct system call. The stack
// pointer must still point at its arguments, as it did when the function was entered
void   JIT_CleanupSystemCallArgs(asSVMRegisters *regs, asCScriptFunction *func) noexcept;

// Kept so that AOT source generated before the cleanup helper was generalized
// still compiles
void   JIT_CleanupAutoHandles(asSVMRegisters *regs, asCScriptFunction *func) noexcept;

// Adds the reference of a handle returned by a direct system call whose declaration
// uses @+. The object may be null
void   JIT_AddRefObject(asSVMRegisters *regs, asCObjectType *objType, void *obj) noexcept;

// The rare paths of the reference counts of script objects changed in place. The
// registers must have been synced, as Release may execute the destructor
void   JIT_AddRefScriptObject(void *obj) noexcept;
void   JIT_ReleaseScriptObject(void *obj) noexcept;

// asBC_Cast. handle is the address of the handle to cast, may be null
void   JIT_Cast(asSVMRegisters *regs, void **handle, asDWORD typeId) noexcept;

// asBC_AllocMem
void  *JIT_AllocMem(asUINT size) noexcept;

// asBC_FREE of the initialization lists with nothing to destroy
void   JIT_FreeMem(void *mem) noexcept;

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
asINT64 JIT_fTOi64(float f) noexcept;
asINT64 JIT_dTOi64(double d) noexcept;
asQWORD JIT_fTOu64(float f) noexcept;
asQWORD JIT_dTOu64(double d) noexcept;
float   JIT_i64TOf(asINT64 v) noexcept;
double  JIT_i64TOd(asINT64 v) noexcept;
float   JIT_u64TOf(asQWORD v) noexcept;
double  JIT_u64TOd(asQWORD v) noexcept;
asUINT  JIT_fTOu(float f) noexcept;
asUINT  JIT_dTOu(double d) noexcept;
float   JIT_uTOf(asUINT v) noexcept;
double  JIT_uTOd(asUINT v) noexcept;

// 64bit integer division helpers for hosts without 64bit registers. The generated
// code verifies that the divisor is valid before calling them
asINT64 JIT_DIVi64(asINT64 a, asINT64 b) noexcept;
asINT64 JIT_MODi64(asINT64 a, asINT64 b) noexcept;
asQWORD JIT_DIVu64(asQWORD a, asQWORD b) noexcept;
asQWORD JIT_MODu64(asQWORD a, asQWORD b) noexcept;

// CJITCompiler::Prepare and Execute, which aren't called from the generated code.
// JIT_Execute only enters the functions compiled by the compiler, which the engine
// must use, with maxNativeCallDepth as its maximum depth of nested native calls
int    JIT_Prepare(asIScriptContext *ctx, asIScriptFunction *func);
int    JIT_Execute(asIScriptContext *ctx, const asIJITCompilerAbstract *compiler, asUINT maxNativeCallDepth);

END_AS_NAMESPACE

#endif
