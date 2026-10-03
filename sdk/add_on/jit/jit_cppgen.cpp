#include "jit_cppgen.h"
#include "jit_runtime.h"

// Internal engine headers. The JIT must be compiled with the same
// configuration as the engine library (see CMakeLists.txt)
#include "as_scriptfunction.h"
#include "as_scriptengine.h"
#include "as_objecttype.h"
#include "as_callfunc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

BEGIN_AS_NAMESPACE

// The ABIs whose calls the generated code makes directly, see GetSystemCall, like
// CJITCodeGen::EmitDirectSystemCall. The class methods are called like functions
// with the object pointer first, which excludes the thiscall convention of 32bit
// x86. The hidden pointer for a value returned in memory is passed like the first
// argument, except after the object pointer of class methods with MSVC. The other
// compilers for 32bit x86 let the called function pop it, which the calls can't
// express, and AArch64 passes it in a register that isn't used for arguments. The
// arguments of the calls are all passed in registers there too, as the ABIs of the
// platforms lay out the ones on the stack differently
#if defined(AS_MAX_PORTABILITY) || defined(AS_BIG_ENDIAN)
#elif defined(AS_X64_MSVC)
#define JIT_AOT_ABI "defined(AS_X64_MSVC)"
#elif defined(AS_X64_MINGW)
#define JIT_AOT_ABI "defined(AS_X64_MINGW) && !defined(_MSC_VER)"
#elif defined(AS_X64_GCC)
#define JIT_AOT_ABI "defined(AS_X64_GCC) && !defined(_MSC_VER)"
#elif defined(AS_ARM64)
#define JIT_AOT_ABI "defined(AS_ARM64)"
#define JIT_AOT_REGISTER_ARGS 8
#elif defined(AS_X86) && defined(_MSC_VER)
#define JIT_AOT_ABI "defined(AS_X86) && defined(_MSC_VER)"
#elif defined(AS_X86) && defined(THISCALL_PASS_OBJECT_POINTER_ON_THE_STACK)
#define JIT_AOT_ABI "defined(AS_X86) && !defined(_MSC_VER) && defined(THISCALL_PASS_OBJECT_POINTER_ON_THE_STACK)"
#elif defined(AS_X86)
#define JIT_AOT_ABI "defined(AS_X86) && !defined(_MSC_VER) && !defined(THISCALL_PASS_OBJECT_POINTER_ON_THE_STACK)"
#endif

#if defined(AS_X64_MSVC) || defined(AS_X64_MINGW) || defined(AS_X64_GCC) || defined(AS_ARM64) || \
	(defined(AS_X86) && defined(THISCALL_PASS_OBJECT_POINTER_ON_THE_STACK) && !defined(_MSC_VER))
#define JIT_AOT_THISCALL
#endif
#if defined(AS_X64_MSVC) || defined(AS_X64_MINGW) || defined(AS_X64_GCC) || (defined(AS_X86) && defined(_MSC_VER))
#define JIT_AOT_RETURN_IN_MEMORY
#if defined(_MSC_VER)
#define JIT_AOT_RETURN_AFTER_THIS
#endif
#endif

static std::string FormatV(const char *format, va_list args)
{
	char buffer[256];
	va_list copy;
	va_copy(copy, args);
	int length = vsnprintf(buffer, sizeof(buffer), format, args);

	std::string text;
	if( length >= int(sizeof(buffer)) )
	{
		text.resize(length + 1);
		vsnprintf(&text[0], text.size(), format, copy);
		text.resize(length);
	}
	else if( length > 0 )
		text.assign(buffer, length);
	va_end(copy);
	return text;
}

static std::string Format(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	std::string text = FormatV(format, args);
	va_end(args);
	return text;
}

// Formats a constant of type int
static std::string IntLiteral(int value)
{
	if( value == -2147483647 - 1 )
		return "(-2147483647 - 1)";
	return Format("%d", value);
}

CJITCppGen::CJITCppGen(const CJITByteCode &code, TargetCallback target, void *targetParam) :
	m_code(code), m_target(target), m_targetParam(targetParam), m_direct(false), m_failed(false), m_pos(0), m_frame(false), m_suspendChecked(true)
{
	// The variables that the analysis keeps in registers, in the order of their bits
	const std::vector<SJITSlot> &slots = code.GetSlots();
	for( asUINT n = 0; n < slots.size(); n++ )
	{
		if( slots[n].cacheBit < 0 )
			continue;
		SLocal local;
		local.offset = slots[n].offset;
		local.kind   = slots[n].cacheKind;
		local.bit    = slots[n].cacheBit;
		local.name   = slots[n].offset < 0 ? Format("l_m%d", -slots[n].offset) : Format("l_%d", slots[n].offset);
		local.used   = false;
		local.read   = false;
		m_locals.push_back(local);
	}

	// The fields of the object that the analysis keeps in registers, by their index
	const std::vector<SJITField> &fields = code.GetFields();
	for( asUINT n = 0; n < fields.size(); n++ )
	{
		SLocal field;
		field.offset = fields[n].offset;
		field.kind   = fields[n].kind;
		field.bit    = fields[n].kept ? int(n) : -1;
		field.name   = Format("f_%d", fields[n].offset);
		field.used   = false;
		field.read   = false;
		m_fields.push_back(field);
	}
}

void CJITCppGen::GetHeapVariables(asCScriptFunction *func, std::vector<int> &offsets)
{
	// Like asCContext::PrepareScriptFunction, the others are initialized by their constructors
	offsets.clear();
	const asCArray<asSScriptVariable*> &vars = func->scriptData->variables;
	for( asUINT n = 0; n < vars.GetLength(); n++ )
		if( vars[n]->stackOffset > 0 && vars[n]->onHeap && (vars[n]->type.IsObject() || vars[n]->type.IsFuncdef()) )
			offsets.push_back(vars[n]->stackOffset);
}

bool CJITCppGen::CallsScript(asEBCInstr op)
{
	return op == asBC_CALL || op == asBC_CALLINTF || op == asBC_CALLBND || op == asBC_CallPtr || op == asBC_ALLOC;
}

// True if continuing after the instruction may find the suspend flag set. This
// mirrors the conservative call detection of CJITCodeGen::EmitBody. Releases may
// run script destructors, except where the analysis removed the release or proved
// that it only frees plain memory
static bool MayRequestSuspend(const SJITInstr &instr)
{
	if( instr.flags & (JIT_INSTR_MOVED | JIT_INSTR_FREE_LIST) )
		return false;
	if( instr.op == asBC_FREE || (instr.flags & JIT_INSTR_REFCOUNT) )
		return true;
	return CJITByteCode::IsSyncPoint(instr.op) && !(instr.flags & (JIT_INSTR_BORROW | JIT_INSTR_INDEXER));
}

const char *CJITCppGen::GetABI()
{
#ifdef JIT_AOT_ABI
	return JIT_AOT_ABI;
#else
	return 0;
#endif
}

// With the same restrictions as CJITCodeGen::EmitDirectSystemCall, and those of the
// ABI, see JIT_AOT_ABI. The arguments are passed as the types of their size, like
// the engine does, and so are the values returned
bool CJITCppGen::GetSystemCall(asCScriptEngine *engine, int funcId, SJITSystemCall &call)
{
#ifndef JIT_AOT_ABI
	UNUSED_VAR(engine);
	UNUSED_VAR(funcId);
	UNUSED_VAR(call);
	return false;
#else
	if( funcId < 0 || asUINT(funcId) >= engine->scriptFunctions.GetLength() )
		return false;
	asCScriptFunction *descr = engine->scriptFunctions[funcId];
	if( descr == 0 || descr->funcType != asFUNC_SYSTEM || descr->sysFuncIntf == 0 )
		return false;
	asSSystemFunctionInterface *sysFunc = descr->sysFuncIntf;

	call.obj = SJITSystemCall::OBJ_NONE;
	call.thisFromStack = false;
	call.auxiliaryThis = false;
	call.virtualThis = false;
	call.adjustThis = false;
	switch( sysFunc->callConv )
	{
	case ICC_CDECL:
		break;
#if AS_PTR_SIZE == 2
	case ICC_STDCALL:
		break;
#endif
#ifdef JIT_AOT_THISCALL
	case ICC_THISCALL:
		if( sysFunc->auxiliary )
			call.auxiliaryThis = true;
		else
			call.thisFromStack = true;
		break;
#ifdef GNU_STYLE_VIRTUAL_METHOD
	case ICC_VIRTUAL_THISCALL:
		call.thisFromStack = true;
		call.virtualThis = true;
		break;
#endif
	case ICC_THISCALL_OBJFIRST:
		if( !sysFunc->auxiliary )
			return false;
		call.auxiliaryThis = true;
		call.obj = SJITSystemCall::OBJ_FIRST;
		break;
	case ICC_THISCALL_OBJLAST:
		if( !sysFunc->auxiliary )
			return false;
		call.auxiliaryThis = true;
		call.obj = SJITSystemCall::OBJ_LAST;
		break;
#ifdef GNU_STYLE_VIRTUAL_METHOD
	case ICC_VIRTUAL_THISCALL_OBJFIRST:
		if( !sysFunc->auxiliary )
			return false;
		call.auxiliaryThis = true;
		call.virtualThis = true;
		call.obj = SJITSystemCall::OBJ_FIRST;
		break;
	case ICC_VIRTUAL_THISCALL_OBJLAST:
		if( !sysFunc->auxiliary )
			return false;
		call.auxiliaryThis = true;
		call.virtualThis = true;
		call.obj = SJITSystemCall::OBJ_LAST;
		break;
#endif
#endif
	case ICC_CDECL_OBJFIRST:
		call.obj = SJITSystemCall::OBJ_FIRST;
		break;
	case ICC_CDECL_OBJLAST:
		call.obj = SJITSystemCall::OBJ_LAST;
		break;
	default:
		return false;
	}

	if( sysFunc->takesObjByVal )
		return false;
	call.adjustThis = sysFunc->compositeOffset || sysFunc->isCompositeIndirect || sysFunc->baseOffset;
	if( call.adjustThis && !call.thisFromStack )
		return false;
	if( sysFunc->auxiliary && !call.auxiliaryThis )
		return false;
	int cleanupCount = JIT_GetSystemCallCleanupCount(descr);
	if( cleanupCount < 0 )
		return false;
	call.cleanAutoHandles = cleanupCount != 0;

	// The value returned. A value type returned by value is stored where the caller
	// pushed the location, by the function itself through the hidden pointer or from
	// the registers it is returned in
	const asCDataType &rt = descr->returnType;
	call.retOnStack  = descr->DoesReturnOnStack();
	call.retInMemory = call.retOnStack && sysFunc->hostReturnInMemory;
	call.retAfterThis = false;
	call.returnAutoHandle = sysFunc->returnAutoHandle;
	call.retParts    = 0;
	call.retBytes    = 0;
	int retSize;
#if defined(AS_X64_GCC) || defined(AS_ARM64)
	const int objSize = call.retOnStack ? rt.GetSizeInMemoryBytes() : 0;
#endif
#ifdef AS_ARM64
	// The aggregates of up to four floats or doubles in a floating point register
	// each, like CJITCodeGen::EmitDirectSystemCall calls them
	const asQWORD retFlags = call.retOnStack ? rt.GetTypeInfo()->flags : 0;
	if( (retFlags & asOBJ_APP_CLASS_ALLFLOATS) && !(retFlags & COMPLEX_MASK) &&
		objSize <= 4 * ((retFlags & asOBJ_APP_CLASS_ALIGN8) ? 8 : 4) )
	{
		int partSize = (retFlags & asOBJ_APP_CLASS_ALIGN8) ? 8 : 4;
		if( objSize % partSize )
			return false;
		call.retInMemory = false;
		call.ret = partSize == 4 ? SJITSystemCall::VALUE_F32 : SJITSystemCall::VALUE_F64;
		call.retParts = objSize / partSize;
		call.retBytes = objSize;
		retSize = sysFunc->hostReturnSize;
	}
	else
#endif
#if defined(AS_X64_GCC) || defined(AS_ARM64)
	// The other objects of 9 to 16 bytes that aren't returned in memory, in two
	// registers for integers, or two for floats if the engine says they hold floats
	if( call.retOnStack && !call.retInMemory && (sysFunc->hostReturnSize == 3 || sysFunc->hostReturnSize == 4) )
	{
		call.ret = sysFunc->hostReturnFloat ? SJITSystemCall::VALUE_F64 : SJITSystemCall::VALUE_I64;
		call.retParts = 2;
		call.retBytes = objSize;
		retSize = sysFunc->hostReturnSize;
	}
	else
#endif
	if( call.retInMemory )
	{
#ifdef JIT_AOT_RETURN_IN_MEMORY
#ifdef JIT_AOT_RETURN_AFTER_THIS
		call.retAfterThis = call.thisFromStack || call.auxiliaryThis;
#endif
		call.ret = SJITSystemCall::VALUE_VOID;
		retSize = AS_PTR_SIZE;
#else
		return false;
#endif
	}
	else if( call.retOnStack )
	{
		if( sysFunc->hostReturnSize == 1 )
			call.ret = sysFunc->hostReturnFloat ? SJITSystemCall::VALUE_F32 : SJITSystemCall::VALUE_I32;
		else if( sysFunc->hostReturnSize == 2 )
			call.ret = sysFunc->hostReturnFloat ? SJITSystemCall::VALUE_F64 : SJITSystemCall::VALUE_I64;
		else
			return false;
		retSize = sysFunc->hostReturnSize;
	}
	else if( rt.GetTokenType() == ttVoid && !rt.IsReference() ) { call.ret = SJITSystemCall::VALUE_VOID;   retSize = 0; }
	else if( rt.IsReference() )                                 { call.ret = SJITSystemCall::VALUE_PTR;    retSize = AS_PTR_SIZE; }
	else if( rt.IsObjectHandle() )                              { call.ret = SJITSystemCall::VALUE_HANDLE; retSize = AS_PTR_SIZE; }
	else if( rt.IsObject() || rt.IsFuncdef() )                  return false;
	else if( rt.IsFloatType() )                                 { call.ret = SJITSystemCall::VALUE_F32;    retSize = 1; }
	else if( rt.IsDoubleType() )                                { call.ret = SJITSystemCall::VALUE_F64;    retSize = 2; }
	else if( rt.GetSizeOnStackDWords() == 2 )                   { call.ret = SJITSystemCall::VALUE_I64;    retSize = 2; }
	else                                                        { call.ret = SJITSystemCall::VALUE_I32;    retSize = 1; }
	if( call.returnAutoHandle && call.ret != SJITSystemCall::VALUE_HANDLE )
		return false;
	bool retFloat = call.ret == SJITSystemCall::VALUE_F32 || call.ret == SJITSystemCall::VALUE_F64;
	if( sysFunc->hostReturnSize != retSize || (!call.retParts && sysFunc->hostReturnFloat != retFloat) )
		return false;

	// The arguments, as laid out on the stack
	call.args.clear();
	int size = 0, intArgs = 0, floatArgs = 0;
	for( asUINT n = 0; n < descr->parameterTypes.GetLength(); n++ )
	{
		const asCDataType &pt = descr->parameterTypes[n];
		if( pt.GetTokenType() == ttQuestion )
		{
			// The native signature has an address and a type id for each ?&.
			call.args.push_back(SJITSystemCall::VALUE_PTR);
			call.args.push_back(SJITSystemCall::VALUE_I32);
			size += AS_PTR_SIZE + 1;
			intArgs += 2;
		}
		else if( pt.IsReference() || pt.IsObjectHandle() || pt.IsObject() || pt.IsFuncdef() ) { call.args.push_back(SJITSystemCall::VALUE_PTR); size += AS_PTR_SIZE; intArgs++; }
		else if( pt.IsFloatType() )               { call.args.push_back(SJITSystemCall::VALUE_F32); size += 1; floatArgs++; }
		else if( pt.IsDoubleType() )              { call.args.push_back(SJITSystemCall::VALUE_F64); size += 2; floatArgs++; }
		else if( pt.GetSizeOnStackDWords() == 2 ) { call.args.push_back(SJITSystemCall::VALUE_I64); size += 2; intArgs++; }
		else                                      { call.args.push_back(SJITSystemCall::VALUE_I32); size += 1; intArgs++; }
	}
	if( size != sysFunc->paramSize )
		return false;
#ifdef JIT_AOT_REGISTER_ARGS
	intArgs += (call.obj != SJITSystemCall::OBJ_NONE ? 1 : 0) +
		(call.thisFromStack || call.auxiliaryThis ? 1 : 0) + (call.retInMemory ? 1 : 0);
	if( intArgs > JIT_AOT_REGISTER_ARGS || floatArgs > JIT_AOT_REGISTER_ARGS )
		return false;
#else
	UNUSED_VAR(intArgs);
	UNUSED_VAR(floatArgs);
#endif
	call.popSize = size + (call.obj != SJITSystemCall::OBJ_NONE || call.thisFromStack ? AS_PTR_SIZE : 0) +
		(call.retOnStack ? AS_PTR_SIZE : 0);
	return true;
#endif
}

bool CJITCppGen::GetConstructorCall(asCScriptEngine *engine, int funcId, SJITSystemCall &call)
{
	if( !GetSystemCall(engine, funcId, call) )
		return false;
	return (call.obj != SJITSystemCall::OBJ_NONE || call.thisFromStack) &&
		!call.retOnStack && call.ret == SJITSystemCall::VALUE_VOID && !call.returnAutoHandle;
}

// asBC_Thiscall1 calls a method with an int, which returns a reference
bool CJITCppGen::GetSystemCall(const SJITInstr &instr, SJITSystemCall &call) const
{
	asCScriptEngine *engine = static_cast<asCScriptEngine*>(m_code.GetFunction()->GetEngine());
	if( !GetSystemCall(engine, asBC_INTARG(instr.bc), call) )
		return false;
	return instr.op != asBC_Thiscall1 || ((call.obj != SJITSystemCall::OBJ_NONE || call.thisFromStack) &&
		!call.retOnStack && call.ret == SJITSystemCall::VALUE_PTR &&
		call.args.size() == 1 && call.args[0] == SJITSystemCall::VALUE_I32);
}

void CJITCppGen::Emit(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	m_out += '\t';
	m_out += FormatV(format, args);
	m_out += '\n';
	va_end(args);
}

void CJITCppGen::Put(const std::string &line)
{
	if( line.empty() )
		return;
	m_out += '\t';
	m_out += line;
	m_out += '\n';
}

bool CJITCppGen::Generate(const char *name, std::string &out, bool direct)
{
	const std::vector<SJITInstr> &instrs = m_code.GetInstructions();
	const std::vector<asUINT> &entries = m_code.GetEntries();
	m_out.clear();
	m_failed = false;
	m_direct = direct;
	for( asUINT n = 0; n < m_locals.size(); n++ )
		m_locals[n].used = m_locals[n].read = false;
	for( asUINT n = 0; n < m_fields.size(); n++ )
		m_fields[n].used = m_fields[n].read = false;

	// Only the instructions that are jumped to get labels, as unused labels give
	// warnings. The VM doesn't enter the direct entry
	bool calls = false, systemCalls = false;
	m_labels.assign(instrs.size(), false);
	std::vector<char> checkedIn(instrs.size(), 1);
	for( asUINT n = 0; n < entries.size() && !direct; n++ )
		m_labels[entries[n]] = true;
	for( asUINT n = 0; n < instrs.size(); n++ )
	{
		if( instrs[n].flags & JIT_INSTR_DEAD )
			continue;
		if( instrs[n].target >= 0 )
		{
			m_labels[instrs[n].target] = true;
			if( asUINT(instrs[n].target) <= n )
				checkedIn[instrs[n].target] = 0;
		}
		if( instrs[n].op == asBC_JMPP )
		{
			const std::vector<int> &targets = m_code.GetSwitchTargets(n);
			for( asUINT t = 0; t < targets.size(); t++ )
			{
				m_labels[targets[t]] = true;
				if( asUINT(targets[t]) <= n )
					checkedIn[targets[t]] = 0;
			}
		}
		if( CallsScript(instrs[n].op) )
			calls = true;
		SJITSystemCall call;
		if( (instrs[n].op == asBC_CALLSYS || instrs[n].op == asBC_Thiscall1) && !(instrs[n].flags & JIT_INSTR_INDEXER) && GetSystemCall(instrs[n], call) )
			systemCalls = true;
	}

	EmitEntry(calls);
	bool fallsIn = true;
	m_suspendChecked = true; // the entry checks the flag, see EmitEntry
	for( asUINT n = 0; n < instrs.size(); n++ )
	{
		if( instrs[n].flags & JIT_INSTR_DEAD )
		{
			fallsIn = false;
			continue;
		}
		m_suspendChecked = (m_suspendChecked || !fallsIn) && checkedIn[n];
		if( m_labels[n] )
			m_out += Format("L_%u:;\n", n);

		// Where a loop with calls is entered, the variables that it doesn't modify
		// are stored once, see CJITByteCode::AnalyseDirtySlots
		Put(Stores(m_code.GetStoresBefore(n)));
		if( !EmitInstr(n) )
			return false;
		Put(Stores(m_code.GetStoresAfter(n)));

		const SJITInstr &instr = instrs[n];
		if( instr.op == asBC_SUSPEND && !(instr.flags & JIT_INSTR_SKIP) )
			m_suspendChecked = true;
		else if( MayRequestSuspend(instr) )
			m_suspendChecked = false;
		if( instr.target > int(n) )
			checkedIn[instr.target] &= char(m_suspendChecked);
		if( instr.op == asBC_JMPP )
		{
			const std::vector<int> &targets = m_code.GetSwitchTargets(n);
			for( asUINT t = 0; t < targets.size(); t++ )
				if( asUINT(targets[t]) > n )
					checkedIn[targets[t]] &= char(m_suspendChecked);
		}
		fallsIn = instr.op != asBC_JMP && instr.op != asBC_JMPP && instr.op != asBC_RET;
	}
	if( m_failed )
		return false;

	// The function being executed, self, is the current function of the context
	// when the function is entered, unless it is entered directly
	std::string text;
	if( direct )
	{
		text = Format("int %s_d(asCContext *ctx, asCScriptFunction *self, asDWORD *fp, asUINT callLimit, asUINT borrowed)\n{\n", name);
		text += "\tasSVMRegisters *regs = &ctx->m_regs;\n";
		text += "\tasDWORD *bc, *sp;\n";
		text += "\tasQWORD vr = 0;\n";
	}
	else
	{
		text = Format("int %s(asSVMRegisters *regs, asPWORD jitArg, asUINT callLimit, asDWORD *stackPointer)\n{\n", name);
		text += "\tasCContext *ctx = (asCContext*)regs->ctx;\n";
		text += "\tasCScriptFunction *self = ctx->m_currentFunction;\n";
		text += "\tasDWORD *bc, *fp, *sp;\n";
		text += "\tasQWORD vr;\n";
		text += "\t(void)stackPointer;\n";
	}
	text += "\t(void)callLimit;\n";
	for( asUINT n = 0; n < m_locals.size(); n++ )
	{
		const SLocal &local = m_locals[n];
		if( !local.used )
			continue;
		text += Format("\t%s %s = 0;\n", LocalType(local.kind), local.name.c_str());
		if( !local.read )
			text += Format("\t(void)%s;\n", local.name.c_str());
	}
	for( asUINT n = 0; n < m_fields.size(); n++ )
	{
		const SLocal &field = m_fields[n];
		if( !field.used )
			continue;
		text += Format("\t%s %s = 0;\n", LocalType(field.kind), field.name.c_str());
		if( !field.read )
			text += Format("\t(void)%s;\n", field.name.c_str());
	}

	// The C++ exceptions of the registered functions called directly are caught
	// where the code is entered, like CallSystemFunction does, which may be in the
	// functions called directly by the code too. The direct entries are called by
	// that code only
	if( (calls || systemCalls) && !direct )
	{
		text += "#ifndef AS_NO_EXCEPTIONS\n\ttry\n#endif\n\t{\n";
		for( size_t pos = 0; pos < m_out.size(); )
		{
			size_t end = m_out.find('\n', pos);
			end = end == std::string::npos ? m_out.size() : end + 1;
			if( m_out[pos] == '\t' )
				text += '\t';
			text.append(m_out, pos, end - pos);
			pos = end;
		}
		text += "\t}\n#ifndef AS_NO_EXCEPTIONS\n";
		text += "\tcatch(...)\n\t{\n\t\tif( !JIT_CatchException(regs) )\n\t\t\tthrow;\n\t\treturn 1;\n\t}\n#endif\n";
	}
	else
		text += m_out;
	text += "}\n";

	out += text;
	return true;
}

// The VM enters at the JitEntry instructions with the frame set up, see JITFunction.
// Native callers enter at the start, and the frame is set up like
// asCContext::PrepareScriptFunction does when the stack block has room and the VM
// has nothing to do, otherwise by JIT_PrepareFrame. The variables kept in local
// variables are loaded from the frame then, see CJITByteCode::GetEntryMask. The
// frame is left to the first place where it may be seen, see JIT_FRAME_BIT
void CJITCppGen::EmitEntry(bool calls)
{
	const std::vector<SJITInstr> &instrs = m_code.GetInstructions();
	const std::vector<asUINT> &entries = m_code.GetEntries();
	asCScriptFunction *func = m_code.GetFunction();
	std::vector<int> heap;
	GetHeapVariables(func, heap);

	if( m_direct )
	{
		Emit("bc = self->scriptData->byteCode.AddressOf();");
		Emit("if( fp - (%u + RESERVE_STACK) < ctx->m_stackBlocks[ctx->m_stackIndex] || regs->doProcessSuspend )", func->scriptData->stackNeeded);
		Emit("{");
		EmitOwnBorrowed("\t");
		Emit("\tctx->m_currentFunction = self;");
		Emit("\tregs->stackPointer = fp;");
		Emit("\tregs->programPointer = bc;");
		Emit("\tif( JIT_PrepareFrame(regs) )");
		Emit("\t\treturn 1;");
		Emit("\tfp = regs->stackFramePointer;");
		Emit("\tsp = regs->stackPointer;");
		Emit("}");
		Emit("else");
		Emit("{");
		for( asUINT n = 0; n < heap.size(); n++ )
			Emit("\tAOT_V(pw, %d) = 0;", heap[n]);
		Emit("\tsp = fp - %u;", func->scriptData->variableSpace);
		Emit("}");
		if( !instrs.empty() )
			Put(Loads(m_code.GetEntryMask(0)));
		return;
	}

	Emit("if( jitArg != 0 )");
	Emit("{");
	Emit("\tfp = regs->stackFramePointer;");
	Emit("\tsp = regs->stackPointer;");
	Emit("\tvr = regs->valueRegister;");
	if( calls )
		Emit("\tcallLimit = AOT_CallLimit(ctx);");
	Emit("\tswitch( jitArg )");
	Emit("\t{");
	for( asUINT n = 0; n < entries.size(); n++ )
	{
		std::string loads = Loads(m_code.GetEntryMask(entries[n]));
		std::string fields = FieldLoads(m_code.GetFieldMask(entries[n]));
		if( !fields.empty() )
			loads += loads.empty() ? fields : " " + fields;
		Emit("\tcase %u: bc = regs->programPointer - %u; %s%sgoto L_%u;", n + 1, instrs[entries[n]].pos, loads.c_str(), loads.empty() ? "" : " ", entries[n]);
	}
	Emit("\t}");
	Emit("\tregs->programPointer += 1 + AS_PTR_SIZE;");
	Emit("\treturn 1;");
	Emit("}");

	Emit("bc = self->scriptData->byteCode.AddressOf();");
#if AS_PTR_SIZE == 1
	Emit("fp = regs->stackPointer;");
#else
	Emit("fp = stackPointer;");
#endif
	Emit("if( fp - (%u + RESERVE_STACK) < ctx->m_stackBlocks[ctx->m_stackIndex] || regs->doProcessSuspend )", func->scriptData->stackNeeded);
	Emit("{");
	Emit("\tregs->stackPointer = fp;");
	Emit("\tregs->programPointer = bc;");
	Emit("\tif( JIT_PrepareFrame(regs) )");
	Emit("\t\treturn 1;");
	Emit("\tfp = regs->stackFramePointer;");
	Emit("\tsp = regs->stackPointer;");
	Emit("}");
	Emit("else");
	Emit("{");
	for( asUINT n = 0; n < heap.size(); n++ )
		Emit("\tAOT_V(pw, %d) = 0;", heap[n]);
	Emit("\tsp = fp - %u;", func->scriptData->variableSpace);
	Emit("}");
	Emit("vr = regs->valueRegister;");
	if( !instrs.empty() )
		Put(Loads(m_code.GetEntryMask(0)));
}

// The type of the local variables of a kind of the analysis, see EJITSlotKind
const char *CJITCppGen::LocalType(int kind)
{
	switch( kind )
	{
	case JIT_SLOT_I32: return "asDWORD";
	case JIT_SLOT_I64: return "asQWORD";
	case JIT_SLOT_F32: return "float";
	default:           return "double";
	}
}

// The type of the variable in the frame, see AOT_V
static const char *FrameType(int kind)
{
	switch( kind )
	{
	case JIT_SLOT_I32: return "u32";
	case JIT_SLOT_I64: return "u64";
	case JIT_SLOT_F32: return "f32";
	default:           return "f64";
	}
}

CJITCppGen::SLocal *CJITCppGen::FindLocal(int offset)
{
	for( asUINT n = 0; n < m_locals.size(); n++ )
		if( m_locals[n].offset == offset )
			return &m_locals[n];
	return 0;
}

// The value of a variable as the type of AOT_V. The instructions access the local
// variables with the types of their kind, with the integers or the floats of the
// same size, see CJITByteCode::AnalyseSlots, or with the untyped copies. Anything
// else can't happen, as the analysis wouldn't keep the variable in a register, but
// the function isn't generated then
std::string CJITCppGen::Var(const char *type, int offset)
{
	SLocal *local = FindLocal(offset);
	if( local == 0 )
		return Format("AOT_V(%s, %d)", type, offset);

	local->used = local->read = true;
	const char *name = local->name.c_str();
	switch( local->kind )
	{
	case JIT_SLOT_I32:
		if( strcmp(type, "u32") == 0 ) return name;
		if( strcmp(type, "i32") == 0 ) return Format("((int)%s)", name);
		if( strcmp(type, "u16") == 0 ) return Format("((asWORD)%s)", name);
		if( strcmp(type, "i16") == 0 ) return Format("((short)%s)", name);
		if( strcmp(type, "u8") == 0 )  return Format("((asBYTE)%s)", name);
		if( strcmp(type, "i8") == 0 )  return Format("((signed char)%s)", name);
		if( strcmp(type, "f32") == 0 ) return Format("aot_f32bits(%s)", name);
		break;
	case JIT_SLOT_F32:
		if( strcmp(type, "f32") == 0 ) return name;
		if( strcmp(type, "u32") == 0 ) return Format("aot_bits32(%s)", name);
		if( strcmp(type, "i32") == 0 ) return Format("((int)aot_bits32(%s))", name);
		if( strcmp(type, "u16") == 0 ) return Format("((asWORD)aot_bits32(%s))", name);
		if( strcmp(type, "i16") == 0 ) return Format("((short)aot_bits32(%s))", name);
		if( strcmp(type, "u8") == 0 )  return Format("((asBYTE)aot_bits32(%s))", name);
		if( strcmp(type, "i8") == 0 )  return Format("((signed char)aot_bits32(%s))", name);
		break;
	case JIT_SLOT_I64:
		if( strcmp(type, "u64") == 0 ) return name;
		if( strcmp(type, "i64") == 0 ) return Format("((asINT64)%s)", name);
		if( strcmp(type, "f64") == 0 ) return Format("aot_f64bits(%s)", name);
		break;
	case JIT_SLOT_F64:
		if( strcmp(type, "f64") == 0 ) return name;
		if( strcmp(type, "u64") == 0 ) return Format("aot_bits64(%s)", name);
		if( strcmp(type, "i64") == 0 ) return Format("((asINT64)aot_bits64(%s))", name);
		break;
	}
	m_failed = true;
	return name;
}

// The statement that writes the value to a variable, see Var
std::string CJITCppGen::SetVar(const char *type, int offset, const std::string &value)
{
	SLocal *local = FindLocal(offset);
	if( local == 0 )
		return Format("AOT_V(%s, %d) = %s;", type, offset, value.c_str());

	local->used = true;
	const char *name = local->name.c_str();
	bool is32 = strcmp(type, "u32") == 0 || strcmp(type, "i32") == 0;
	bool is64 = strcmp(type, "u64") == 0 || strcmp(type, "i64") == 0;
	switch( local->kind )
	{
	case JIT_SLOT_I32:
		if( is32 ) return Format("%s = (asDWORD)(%s);", name, value.c_str());
		if( strcmp(type, "f32") == 0 ) return Format("%s = aot_bits32(%s);", name, value.c_str());
		break;
	case JIT_SLOT_F32:
		if( strcmp(type, "f32") == 0 ) return Format("%s = %s;", name, value.c_str());
		if( is32 ) return Format("%s = aot_f32bits((asDWORD)(%s));", name, value.c_str());
		break;
	case JIT_SLOT_I64:
		if( is64 ) return Format("%s = (asQWORD)(%s);", name, value.c_str());
		if( strcmp(type, "f64") == 0 ) return Format("%s = aot_bits64(%s);", name, value.c_str());
		break;
	case JIT_SLOT_F64:
		if( strcmp(type, "f64") == 0 ) return Format("%s = %s;", name, value.c_str());
		if( is64 ) return Format("%s = aot_f64bits((asQWORD)(%s));", name, value.c_str());
		break;
	}
	m_failed = true;
	return "";
}

// The statement that writes the value to the low byte or word of a variable, u8 or
// u16, and clears the rest of the dword, like the VM does for 8 and 16bit values.
// The local variables are only used on little endian hosts, see JIT_AOT_MAX_LOCALS
std::string CJITCppGen::SetVarLow(const char *type, int offset, const std::string &value)
{
	const char *ctype = strcmp(type, "u8") == 0 ? "asBYTE" : "asWORD";
	SLocal *local = FindLocal(offset);
	if( local == 0 )
		return Format("{ %s v_ = (%s)(%s); AOT_V(u32, %d) = 0; AOT_V(%s, %d) = v_; }", ctype, ctype, value.c_str(), offset, type, offset);

	local->used = true;
	if( local->kind == JIT_SLOT_F32 )
		return Format("%s = aot_f32bits((%s)(%s));", local->name.c_str(), ctype, value.c_str());
	if( local->kind != JIT_SLOT_I32 )
		m_failed = true;
	return Format("%s = (%s)(%s);", local->name.c_str(), ctype, value.c_str());
}

// The address of a variable, which the analysis doesn't keep in a register then
std::string CJITCppGen::VarAddr(int offset)
{
	if( FindLocal(offset) )
		m_failed = true;
	return Format("(fp - %d)", offset);
}

// The statements that store the local variables in the mask to the frame, or load
// them from it, and store the frame for JIT_FRAME_BIT. The masks are those of the
// analysis, see CJITByteCode::GetDirtyMask
std::string CJITCppGen::Stores(asUINT mask)
{
	std::string text;
	for( asUINT n = 0; n < m_locals.size(); n++ )
	{
		SLocal &local = m_locals[n];
		if( !(mask & (asUINT(1) << local.bit)) )
			continue;
		local.used = local.read = true;
		if( !text.empty() )
			text += ' ';
		text += Format("AOT_V(%s, %d) = %s;", FrameType(local.kind), local.offset, local.name.c_str());
	}
	if( mask & JIT_FRAME_BIT )
		text += text.empty() ? "AOT_FRAME();" : " AOT_FRAME();";
	return text;
}

std::string CJITCppGen::Loads(asUINT mask)
{
	std::string text;
	for( asUINT n = 0; n < m_locals.size(); n++ )
	{
		SLocal &local = m_locals[n];
		if( !(mask & (asUINT(1) << local.bit)) )
			continue;
		local.used = true;
		if( !text.empty() )
			text += ' ';
		text += Format("%s = AOT_V(%s, %d);", local.name.c_str(), FrameType(local.kind), local.offset);
	}
	return text;
}

// The value of a field of the object kept in a local variable as u32 or f32, and the
// statement that writes it, see CJITByteCode::AnalyseThisFields. The fields are
// written through to the object
std::string CJITCppGen::Field(int f, const char *type)
{
	SLocal &field = m_fields[f];
	field.used = field.read = true;
	bool isFloat = strcmp(type, "f32") == 0;
	if( (field.kind == JIT_SLOT_F32) == isFloat )
		return field.name;
	return Format(isFloat ? "aot_f32bits(%s)" : "aot_bits32(%s)", field.name.c_str());
}

std::string CJITCppGen::SetField(int f, const char *type, const std::string &value)
{
	SLocal &field = m_fields[f];
	field.used = true;
	bool isFloat = strcmp(type, "f32") == 0;
	if( (field.kind == JIT_SLOT_F32) == isFloat )
		return Format("%s = %s;", field.name.c_str(), value.c_str());
	return Format(isFloat ? "%s = aot_bits32(%s);" : "%s = aot_f32bits(%s);", field.name.c_str(), value.c_str());
}

// The statement that loads the fields in the mask from the object, where the VM
// enters and after the line callbacks. The null pointer is found by asBC_LoadThisR
// before the fields are read
std::string CJITCppGen::FieldLoads(asUINT mask)
{
	std::string text;
	for( asUINT f = 0; f < m_fields.size(); f++ )
	{
		SLocal &field = m_fields[f];
		if( field.bit < 0 || !((mask >> f) & 1) )
			continue;
		field.used = true;
		text += Format(" %s = *(aot_%s*)(t_ + %d);", field.name.c_str(), FrameType(field.kind), field.offset);
	}
	if( text.empty() )
		return text;
	return "{ asPWORD t_ = AOT_V(pw, 0); if( t_ != 0 ) {" + text + " } }";
}

// The statement that returns to the VM at the instruction, which raises the exception
std::string CJITCppGen::Bail() const
{
	std::string sync = m_sync;
	if( m_direct )
		sync = "if( borrowed ) { JIT_OwnParams(self, fp, borrowed); borrowed = 0; }" +
		       (sync.empty() ? "" : " " + sync);
	if( m_frame )
		sync += sync.empty() ? "AOT_FRAME();" : " AOT_FRAME();";
	if( sync.empty() )
		return Format("AOT_BAIL(%u);", m_pos);
	return Format("{ %s AOT_BAIL(%u); }", sync.c_str(), m_pos);
}

// Gives the direct entry's borrowed handle parameters references of their own
// before the VM, the engine, or the application can see its frame. Their normal
// FREE instructions release those references later.
void CJITCppGen::EmitOwnBorrowed(const char *indent)
{
	if( m_direct )
		Emit("%sif( borrowed ) { JIT_OwnParams(self, fp, borrowed); borrowed = 0; }", indent);
}

// Where the VM, the engine, or the application may see the frame, i.e. at the
// calls, the helpers, and the returns to the VM, the local variables whose frame
// variables are older are stored like the JIT does, and so is the frame if it
// hasn't been. The calls leave them in the frame, which is loaded again where they
// are read later, and so do the instructions where the frame may have been
// modified, see GetReloadMask
void CJITCppGen::EmitSync(const char *indent)
{
	EmitOwnBorrowed(indent);
	if( !m_sync.empty() )
		Emit("%s%s", indent, m_sync.c_str());
	if( m_frame )
		Emit("%sAOT_FRAME();", indent);
	Emit("%sAOT_SYNC(%u);", indent, m_pos);
}

void CJITCppGen::EmitScriptCall(asUINT idx, const SJITInstr &instr)
{
	asUINT pos = m_pos;
	bool virtualCall = instr.op == asBC_CALLINTF;
	asCScriptEngine *engine = static_cast<asCScriptEngine*>(m_code.GetFunction()->GetEngine());
	int id = asBC_INTARG(instr.bc);
	asCScriptFunction *callee = id >= 0 && asUINT(id) < engine->scriptFunctions.GetLength() ? engine->scriptFunctions[id] : 0;
	callee = CJITByteCode::FindAOTCallee(callee, virtualCall);

	Emit("{");
	EmitOwnBorrowed("\t");
	Put(m_sync.empty() ? "" : "\t" + m_sync);
	if( virtualCall )
	{
		Emit("\tasCScriptObject *o_ = (asCScriptObject*)AOT_S(pw, 0);");
		Emit("\tasCScriptFunction *f_ = o_ ? AOT_Virtual(o_, ctx->m_engine->scriptFunctions[AOT_INT(%u)]) : 0;", pos + 1);
		Emit("\tJITFunction t_ = f_ ? (JITFunction)f_->scriptData->jitFunction : 0;");
	}
	else
	{
		Emit("\tasCScriptFunction *f_ = ctx->m_engine->scriptFunctions[AOT_INT(%u)];", pos + 1);
		Emit("\tJITFunction t_ = (JITFunction)f_->scriptData->jitFunction;");
	}
	EmitCall(callee, pos + 2, "\t",
	         Format("JIT_CallScript(regs, %s, AOT_INT(%u), 0, callLimit)",
	                virtualCall ? "JIT_CALL_INTERFACE" : "JIT_CALL_SCRIPT", pos + 1),
	         m_code.GetBorrowedArgs(idx));
	EmitReload("\t");
	Emit("}");
}

// Calls the script function f_, whose code is t_, with the arguments pushed. The
// function is called natively if it has been compiled and the call stack has room,
// like JIT_CallScript does, which is left the rest and called by slow. The code of
// the function expected is called directly if the function has it, see AOT_PopCall.
// The call state gets the frame, which is only stored for JIT_CallScript. next is
// the position after the instruction
void CJITCppGen::EmitCall(asCScriptFunction *callee, asUINT next, const char *indent, const std::string &slow, asUINT borrowed)
{
	std::string target;
	if( m_target && callee && callee->funcType == asFUNC_SCRIPT && callee->scriptData )
		target = m_target(callee, m_targetParam);

	Emit("%sasUINT n_ = ctx->m_callStack.GetLength();", indent);
	if( !target.empty() )
	{
		// The function pops the arguments like asBC_RET does
		m_out += JIT_CPPGEN_REGION;
		m_out += target;
		m_out += '\n';
		Emit("%sif( t_ == %s && n_ < callLimit )", indent, target.c_str());
		Emit("%s{", indent);
		Emit("%s\tAOT_PushCall(ctx, n_, self, fp, bc + %u, sp);", indent, next);
		Emit("%s\tif( %s_d(ctx, f_, sp, callLimit, %uu) )", indent, target.c_str(), borrowed);
		Emit("%s\t\treturn 1;", indent);
		Emit("%s\tsp += %d;", indent, CJITByteCode::GetPopSize(callee));
		Emit("%s\tvr = regs->valueRegister;", indent);
		Emit("%s}", indent);
		Emit("%selse", indent);
		m_out += JIT_CPPGEN_REGION_END;
		m_out += '\n';
	}
	Emit("%sif( t_ && n_ < callLimit )", indent);
	Emit("%s{", indent);
	if( borrowed )
		Emit("%s\tJIT_OwnParams(f_, sp, %uu);", indent, borrowed);
	Emit("%s\tif( AOT_CallNative(regs, ctx, n_, self, f_, t_, fp, bc + %u, sp, callLimit) )", indent, next);
	Emit("%s\t\treturn 1;", indent);
	Emit("%s\tAOT_RELOAD();", indent);
	Emit("%s}", indent);
	Emit("%selse", indent);
	Emit("%s{", indent);
	if( borrowed )
		Emit("%s\tJIT_OwnParams(f_, sp, %uu);", indent, borrowed);
	if( m_frame )
		Emit("%s\tAOT_FRAME();", indent);
	Emit("%s\tAOT_SYNC(%u);", indent, m_pos);
	Emit("%s\tif( %s )", indent, slow.c_str());
	Emit("%s\t\treturn 1;", indent);
	Emit("%s\tAOT_RELOAD();", indent);
	Emit("%s}", indent);
}

void CJITCppGen::EmitReload(const char *indent)
{
	if( !m_reload.empty() )
		Emit("%s%s", indent, m_reload.c_str());
}

// Loads the address of the element instead of calling the indexer, see
// CJITByteCode::FindIndexer. The code returns to the VM for a null object or buffer
// and an index out of range, and the VM calls the method, which raises the
// exception. The variables are only stored then, like the JIT does, as the call
// isn't a sync point
void CJITCppGen::EmitIndexer(asUINT idx)
{
	const SJITIndexerCall &indexer = *m_code.GetIndexer(idx);
	const int P = AS_PTR_SIZE;
	Emit("{");
	Emit("\tasPWORD o_ = AOT_S(pw, 0), b_ = o_ ? *(aot_pw*)(o_ + %d) : 0;", indexer.layout.bufferOffset);
	Emit("\tasDWORD i_ = AOT_S(u32, %d);", P);
	Emit("\tif( b_ == 0 || i_ >= *(aot_u32*)(b_ + %d) )", indexer.layout.lengthOffset);
	Emit("\t\t%s", Bail().c_str());
	Emit("\tAOT_SETVR(asPWORD, %s(b_ + %d + (asPWORD)i_ * %d));", indexer.indirect ? "*(aot_pw*)" : "", indexer.layout.dataOffset, indexer.elementSize);
	Emit("\tsp += %d;", P + 1);
	Emit("}");
}

// Calls the registered function like CallSystemFunction does, see GetSystemCall,
// with the function pointer cast to the types of the values. A null object pointer is
// an exception raised by the VM. The VM registers are stored like for the calls
// through the engine, as the function may raise script exceptions or inspect the
// context, except the value register, which JIT_AfterDirectCall gets like the VM
// would, and the variables are loaded again only after it, like the JIT does
void CJITCppGen::EmitSystemCall(asUINT idx, const SJITSystemCall &call)
{
	static const char *const types[] = { "void", "asDWORD", "asQWORD", "float", "double", "void*", "void*" };
	static const char *const stack[] = { "", "u32", "u64", "f32", "f64", "pw", "pw" };
	static const int sizes[] = { 0, 1, 2, 1, 2, AS_PTR_SIZE, AS_PTR_SIZE };
	asUINT pos = m_pos;
	bool obj = call.obj != SJITSystemCall::OBJ_NONE || call.thisFromStack;
	int retOff = obj ? AS_PTR_SIZE : 0;
	int off = retOff + (call.retOnStack ? AS_PTR_SIZE : 0);

	std::vector<std::string> params, args;
	if( call.retInMemory && !call.retAfterThis ) { params.push_back("void*"); args.push_back("r_"); }
	if( call.thisFromStack )                    { params.push_back("void*"); args.push_back(call.adjustThis ? "t_" : "o_"); }
	if( call.auxiliaryThis )                    { params.push_back("void*"); args.push_back("h_"); }
	if( call.retAfterThis )                     { params.push_back("void*"); args.push_back("r_"); }
	if( call.obj == SJITSystemCall::OBJ_FIRST ) { params.push_back("void*"); args.push_back("o_"); }
	for( asUINT n = 0; n < call.args.size(); n++ )
	{
		int kind = call.args[n];
		params.push_back(types[kind]);
		args.push_back(Format(kind == SJITSystemCall::VALUE_PTR ? "(void*)AOT_S(%s, %d)" : "AOT_S(%s, %d)", stack[kind], off));
		off += sizes[kind];
	}
	if( call.obj == SJITSystemCall::OBJ_LAST )  { params.push_back("void*"); args.push_back("o_"); }
	std::string paramList, argList;
	for( asUINT n = 0; n < params.size(); n++ )
	{
		paramList += (n ? ", " : "") + params[n];
		argList += (n ? ", " : "") + args[n];
	}

	Emit("{");
	Emit("\tasCScriptFunction *d_ = ctx->m_engine->scriptFunctions[AOT_INT(%u)];", pos + 1);
	if( call.auxiliaryThis )
		Emit("\tvoid *h_ = d_->sysFuncIntf->auxiliary;");
	if( obj )
	{
		Emit("\tvoid *o_ = (void*)AOT_S(pw, 0);");
		Emit("\tif( o_ == 0 )");
		Emit("\t\t%s", Bail().c_str());
	}
	if( call.adjustThis )
	{
		Emit("\tvoid *t_ = (void*)((char*)o_ + d_->sysFuncIntf->compositeOffset);");
		Emit("\tif( d_->sysFuncIntf->isCompositeIndirect ) t_ = *(void**)t_;");
#if defined(__GNUC__) && defined(AS_ARM64)
		Emit("\tt_ = (void*)((char*)t_ + (d_->sysFuncIntf->baseOffset >> 1));");
#else
		Emit("\tt_ = (void*)((char*)t_ + d_->sysFuncIntf->baseOffset);");
#endif
	}
	if( call.retOnStack )
		Emit("\tvoid *r_ = (void*)AOT_S(pw, %d);", retOff);
	EmitOwnBorrowed("\t");
	Put(m_sync.empty() ? "" : "\t" + m_sync);
	if( m_frame )
		Emit("\tAOT_FRAME();");
	Emit("\tregs->programPointer = bc + %u;", pos);
	Emit("\tregs->stackPointer = sp;");
	Emit("\tctx->m_callingSystemFunction = d_;");
	std::string retType = call.retParts ? Format("aot_parts<%s, %d>", types[call.ret], call.retParts) : types[call.ret];
	std::string target = "d_->sysFuncIntf->func";
	if( call.virtualThis )
	{
		Emit("\tasFUNCTION_t f_ = (*(asFUNCTION_t**)%s)[FuncPtrToUInt(d_->sysFuncIntf->func) / sizeof(void*)];",
			call.adjustThis ? "t_" : call.thisFromStack ? "o_" : "h_");
		target = "f_";
	}
	std::string func = Format("((%s (AOT_CDECL*)(%s))%s)(%s)", retType.c_str(), paramList.c_str(), target.c_str(), argList.c_str());
	if( call.ret == SJITSystemCall::VALUE_VOID )
		Emit("\t%s;", func.c_str());
	else
		Emit("\t%s x_ = %s;", retType.c_str(), func.c_str());
	Emit("\tctx->m_callingSystemFunction = 0;");
	if( call.returnAutoHandle )
		Emit("\tJIT_AddRefObject(regs, (asCObjectType*)d_->returnType.GetTypeInfo(), x_);");

	// The value is stored like the VM does, but the value register only if it is read
	bool vrLive = m_code.IsVRLiveAfter(idx);
	if( call.retParts )
		Emit("\tmemcpy(r_, &x_, %d);", call.retBytes);
	else if( call.retOnStack && call.ret != SJITSystemCall::VALUE_VOID )
		Emit("\t*(aot_%s*)r_ = x_;", stack[call.ret]);
	else if( call.ret == SJITSystemCall::VALUE_HANDLE )
	{
		Emit("\tregs->objectRegister = x_;");
		Emit("\tregs->objectType = d_->returnType.GetTypeInfo();");
	}
	else if( vrLive ) switch( call.ret )
	{
	case SJITSystemCall::VALUE_I32: Emit("\tAOT_SETVR(asDWORD, x_);"); break;
	case SJITSystemCall::VALUE_I64: Emit("\tvr = x_;"); break;
	case SJITSystemCall::VALUE_F32: Emit("\tAOT_SETVR(asDWORD, aot_bits32(x_));"); break;
	case SJITSystemCall::VALUE_F64: Emit("\tvr = aot_bits64(x_);"); break;
	case SJITSystemCall::VALUE_PTR: Emit("\tAOT_SETVR(asPWORD, (asPWORD)x_);"); break;
	default: break;
	}
	if( call.cleanAutoHandles )
		Emit("\tJIT_CleanupSystemCallArgs(regs, d_);");
	Emit("\tsp += %d;", call.popSize);

	// Exceptions, suspend requests, and line callbacks
	Emit("\tif( AOT_SUSPENDING() )");
	Emit("\t{");
	Emit("\t\tregs->stackPointer = sp;");
	if( vrLive )
		Emit("\t\tregs->valueRegister = vr;");
	Emit("\t\tif( JIT_AfterDirectCall(regs, AOT_INT(%u), %s) )", pos + 1, call.retOnStack ? "r_" : "0");
	Emit("\t\t\treturn 1;");
	EmitReload("\t\t");
	Emit("\t}");
	Emit("}");
}

// Allocates a registered value type and calls its constructor directly. The
// constructor has the stack layout of a system call after the new object pointer
// is pushed. The destination is popped and filled afterwards like asBC_ALLOC does
void CJITCppGen::EmitConstructor(asUINT idx, const SJITSystemCall &call)
{
	static const char *const types[] = { "void", "asDWORD", "asQWORD", "float", "double", "void*", "void*" };
	static const char *const stack[] = { "", "u32", "u64", "f32", "f64", "pw", "pw" };
	static const int sizes[] = { 0, 1, 2, 1, 2, AS_PTR_SIZE, AS_PTR_SIZE };
	const asUINT pos = m_pos;
	const int P = AS_PTR_SIZE;
	int off = P;

	std::vector<std::string> params, args;
	if( call.thisFromStack )                    { params.push_back("void*"); args.push_back(call.adjustThis ? "t_" : "m_"); }
	if( call.auxiliaryThis )                    { params.push_back("void*"); args.push_back("h_"); }
	if( call.obj == SJITSystemCall::OBJ_FIRST ) { params.push_back("void*"); args.push_back("m_"); }
	for( asUINT n = 0; n < call.args.size(); n++ )
	{
		int kind = call.args[n];
		params.push_back(types[kind]);
		args.push_back(Format(kind == SJITSystemCall::VALUE_PTR ? "(void*)AOT_S(%s, %d)" : "AOT_S(%s, %d)", stack[kind], off));
		off += sizes[kind];
	}
	if( call.obj == SJITSystemCall::OBJ_LAST ) { params.push_back("void*"); args.push_back("m_"); }
	std::string paramList, argList;
	for( asUINT n = 0; n < params.size(); n++ )
	{
		paramList += (n ? ", " : "") + params[n];
		argList += (n ? ", " : "") + args[n];
	}

	Emit("\t\t{");
	Emit("\t\t\tasCScriptFunction *d_ = ctx->m_engine->scriptFunctions[AOT_INT(%u)];", pos + 1 + P);
	if( call.auxiliaryThis )
		Emit("\t\t\tvoid *h_ = d_->sysFuncIntf->auxiliary;");
	Emit("\t\t\tvoid *m_ = ctx->m_engine->CallAlloc(o_);");
	Emit("\t\t\tsp -= %d; AOT_S(pw, 0) = (asPWORD)m_;", P);
	if( call.adjustThis )
	{
		Emit("\t\t\tvoid *t_ = (void*)((char*)m_ + d_->sysFuncIntf->compositeOffset);");
		Emit("\t\t\tif( d_->sysFuncIntf->isCompositeIndirect ) t_ = *(void**)t_;");
#if defined(__GNUC__) && defined(AS_ARM64)
		Emit("\t\t\tt_ = (void*)((char*)t_ + (d_->sysFuncIntf->baseOffset >> 1));");
#else
		Emit("\t\t\tt_ = (void*)((char*)t_ + d_->sysFuncIntf->baseOffset);");
#endif
	}
	Emit("\t\t\tregs->programPointer = bc + %u;", pos);
	Emit("\t\t\tregs->stackPointer = sp;");
	Emit("\t\t\tctx->m_callingSystemFunction = d_;");
	std::string target = "d_->sysFuncIntf->func";
	if( call.virtualThis )
	{
		Emit("\t\t\tasFUNCTION_t f_ = (*(asFUNCTION_t**)%s)[FuncPtrToUInt(d_->sysFuncIntf->func) / sizeof(void*)];",
			call.adjustThis ? "t_" : call.thisFromStack ? "m_" : "h_");
		target = "f_";
	}
	Emit("\t\t\t((void (AOT_CDECL*)(%s))%s)(%s);", paramList.c_str(), target.c_str(), argList.c_str());
	Emit("\t\t\tctx->m_callingSystemFunction = 0;");
	if( call.cleanAutoHandles )
		Emit("\t\t\tJIT_CleanupSystemCallArgs(regs, d_);");
	Emit("\t\t\tsp += %d;", call.popSize);
	Emit("\t\t\tvoid **a_ = (void**)AOT_S(pw, 0);");
	Emit("\t\t\tsp += %d;", P);
	Emit("\t\t\tif( a_ ) *a_ = m_;");
	Emit("\t\t\tif( AOT_SUSPENDING() )");
	Emit("\t\t\t{");
	Emit("\t\t\t\tregs->stackPointer = sp;");
	Emit("\t\t\t\tif( JIT_AfterDirectAlloc(regs, m_, a_) )");
	Emit("\t\t\t\t\treturn 1;");
	Emit("\t\t\t}");
	Emit("\t\t}");
}

bool CJITCppGen::EmitInstr(asUINT idx)
{
	const SJITInstr &instr = m_code.GetInstructions()[idx];
	const asDWORD *b = instr.bc;
	asUINT pos = instr.pos;
	const int P = AS_PTR_SIZE;

	if( instr.flags & JIT_INSTR_SKIP )
		return true;

	m_pos    = pos;
	m_sync   = Stores(m_code.GetDirtyMask(idx) & ~JIT_FRAME_BIT);
	m_reload = Loads(m_code.GetReloadMask(idx));
	m_frame  = (m_code.GetDirtyMask(idx) & JIT_FRAME_BIT) != 0;

	// The operands, only valid for the instruction types that have them
	#define SW0 int(asBC_SWORDARG0(b))
	#define SW1 int(asBC_SWORDARG1(b))
	#define SW2 int(asBC_SWORDARG2(b))
	#define W0  asUINT(asBC_WORDARG0(b))
	#define DW1 asUINT(asBC_DWORDARG(b))
	#define DW2 asUINT(b[2])
	#define QW1 (unsigned long long)(asBC_QWORDARG(b))

	// The operations on two variables that write the result to the first one
	#define BINARY(T, OP) Put(SetVar(T, SW0, Var(T, SW1) + " " OP " " + Var(T, SW2)))
	#define SHIFT(T, S, BITS) Put(SetVar(T, SW0, Var(T, SW1) + " " S " (" + Var("u32", SW2) + " & " BITS ")"))
	#define COMPARE(C, T) Emit("AOT_SETVR(asDWORD, aot_cmp<" C ">(%s, %s));", Var(T, SW0).c_str(), Var(T, SW1).c_str())

	m_out += Format("\t// %u %s\n", pos, asBCInfo[instr.op].name);

	switch( instr.op )
	{
	case asBC_PopPtr:   Emit("sp += %d;", P); break;
	case asBC_PshGPtr:  Emit("sp -= %d; AOT_S(pw, 0) = *(aot_pw*)AOT_PW(%u);", P, pos + 1); break;
	case asBC_PshC4:    Emit("sp -= 1; AOT_S(u32, 0) = %uu;", DW1); break;
	case asBC_PshV4:    Emit("sp -= 1; AOT_S(u32, 0) = %s;", Var("u32", SW0).c_str()); break;
	case asBC_PSF:      Emit("sp -= %d; AOT_S(pw, 0) = (asPWORD)%s;", P, VarAddr(SW0).c_str()); break;
	case asBC_SwapPtr:  Emit("{ asPWORD t_ = AOT_S(pw, 0); AOT_S(pw, 0) = AOT_S(pw, %d); AOT_S(pw, %d) = t_; }", P, P); break;
	case asBC_PshG4:    Emit("sp -= 1; AOT_S(u32, 0) = *(aot_u32*)AOT_PW(%u);", pos + 1); break;
	case asBC_LdGRdR4:  Emit("AOT_SETVR(asPWORD, AOT_PW(%u)); %s", pos + 1, SetVar("u32", SW0, "AOT_R(u32)").c_str()); break;

	case asBC_NOT:
		{
			SLocal *local = FindLocal(SW0);
			if( local == 0 )
				Emit("AOT_NOT(%d);", SW0);
			else if( local->kind == JIT_SLOT_I32 )
			{
				local->used = local->read = true;
				Emit("AOT_NOTL(%s);", local->name.c_str());
			}
			else
				Emit("{ asDWORD n_ = %s; AOT_NOTL(n_); %s }", Var("u32", SW0).c_str(), SetVar("u32", SW0, "n_").c_str());
		}
		break;

	case asBC_CALL:
	case asBC_CALLINTF:
		EmitScriptCall(idx, instr);
		break;

	case asBC_RET:
		if( m_direct )
		{
			// The caller pops the arguments
			Emit("AOT_PopCall(ctx);");
			if( m_code.RetReadsVR() )
				Emit("regs->valueRegister = vr;");
			Emit("return 0;");
			break;
		}

		// Like the VM, and asCContext::PopCallState, unless the function was the first
		// one or a nested call, which finishes the execution
		Emit("{");
		Emit("\tasUINT l_ = ctx->m_callStack.GetLength();");
		Emit("\tif( l_ == 0 || ctx->m_callStack.AddressOf()[l_ - CALLSTACK_FRAME_SIZE] == 0 )");
		Emit("\t{");
		if( m_frame )
			Emit("\t\tAOT_FRAME();");
		Emit("\t\tAOT_SYNC(%u);", pos);
		Emit("\t\tctx->m_status = asEXECUTION_FINISHED;");
		Emit("\t\treturn 1;");
		Emit("\t}");
		Emit("\tsize_t *s_ = ctx->m_callStack.AddressOf() + l_ - CALLSTACK_FRAME_SIZE;");
		Emit("\tregs->stackFramePointer = (asDWORD*)s_[0];");
		Emit("\tctx->m_currentFunction = (asCScriptFunction*)s_[1];");
		Emit("\tregs->programPointer = (asDWORD*)s_[2];");
		Emit("\tregs->stackPointer = (asDWORD*)s_[3] + %u;", W0);
		Emit("\tctx->m_stackIndex = (int)s_[4];");
		Emit("\tctx->m_callStack.SetLengthNoAllocate(l_ - CALLSTACK_FRAME_SIZE);");
		Emit("\tregs->valueRegister = vr;");
		Emit("\treturn 0;");
		Emit("}");
		break;

	case asBC_JMP:    Emit("goto L_%d;", instr.target); break;
	case asBC_JZ:     Emit("if( (int)AOT_GETVR(asDWORD) == 0 ) goto L_%d;", instr.target); break;
	case asBC_JNZ:    Emit("if( (int)AOT_GETVR(asDWORD) != 0 ) goto L_%d;", instr.target); break;
	case asBC_JS:     Emit("if( (int)AOT_GETVR(asDWORD) < 0 ) goto L_%d;", instr.target); break;
	case asBC_JNS:    Emit("if( (int)AOT_GETVR(asDWORD) >= 0 ) goto L_%d;", instr.target); break;
	case asBC_JP:     Emit("if( (int)AOT_GETVR(asDWORD) > 0 ) goto L_%d;", instr.target); break;
	case asBC_JNP:    Emit("if( (int)AOT_GETVR(asDWORD) <= 0 ) goto L_%d;", instr.target); break;
	case asBC_JLowZ:  Emit("if( AOT_GETVR(asBYTE) == 0 ) goto L_%d;", instr.target); break;
	case asBC_JLowNZ: Emit("if( AOT_GETVR(asBYTE) != 0 ) goto L_%d;", instr.target); break;

	case asBC_TZ:     Emit("AOT_TEST((int)AOT_GETVR(asDWORD) == 0);"); break;
	case asBC_TNZ:    Emit("AOT_TEST((int)AOT_GETVR(asDWORD) != 0);"); break;
	case asBC_TS:     Emit("AOT_TEST((int)AOT_GETVR(asDWORD) < 0);"); break;
	case asBC_TNS:    Emit("AOT_TEST((int)AOT_GETVR(asDWORD) >= 0);"); break;
	case asBC_TP:     Emit("AOT_TEST((int)AOT_GETVR(asDWORD) > 0);"); break;
	case asBC_TNP:    Emit("AOT_TEST((int)AOT_GETVR(asDWORD) <= 0);"); break;

	case asBC_NEGi:   Put(SetVar("u32", SW0, "0u - " + Var("u32", SW0))); break;
	case asBC_NEGf:   Put(SetVar("f32", SW0, "-" + Var("f32", SW0))); break;
	case asBC_NEGd:   Put(SetVar("f64", SW0, "-" + Var("f64", SW0))); break;
	case asBC_NEGi64: Put(SetVar("u64", SW0, "(asQWORD)0 - " + Var("u64", SW0))); break;

	case asBC_INCi16: Emit("++AOT_R(u16);"); break;
	case asBC_INCi8:  Emit("++AOT_R(u8);"); break;
	case asBC_DECi16: Emit("--AOT_R(u16);"); break;
	case asBC_DECi8:  Emit("--AOT_R(u8);"); break;
	case asBC_INCi:
	case asBC_DECi:
	case asBC_INCf:
	case asBC_DECf:
		{
			// The fields of the object kept in local variables are incremented there and stored
			int f = m_code.GetFieldAccess(idx);
			bool inc = instr.op == asBC_INCi || instr.op == asBC_INCf;
			bool isFloat = instr.op == asBC_INCf || instr.op == asBC_DECf;
			if( f < 0 && !isFloat )
				Emit(inc ? "++AOT_R(u32);" : "--AOT_R(u32);");
			else if( f < 0 )
				Emit(inc ? "AOT_R(f32) += 1.0f;" : "AOT_R(f32) -= 1.0f;");
			else
			{
				const char *type = isFloat ? "f32" : "u32";
				const char *step = isFloat ? (inc ? " + 1.0f" : " - 1.0f") : (inc ? " + 1u" : " - 1u");
				if( !((m_code.GetFieldMask(idx) >> f) & 1) )
					Put(SetField(f, type, Format("AOT_R(%s)", type)));
				Put(SetField(f, type, Field(f, type) + step));
				Emit("AOT_R(%s) = %s;", type, Field(f, type).c_str());
			}
		}
		break;
	case asBC_INCd:   Emit("AOT_R(f64) += 1.0;"); break;
	case asBC_DECd:   Emit("AOT_R(f64) -= 1.0;"); break;
	case asBC_INCi64: Emit("++AOT_R(u64);"); break;
	case asBC_DECi64: Emit("--AOT_R(u64);"); break;
	case asBC_IncVi:  Put(SetVar("u32", SW0, Var("u32", SW0) + " + 1u")); break;
	case asBC_DecVi:  Put(SetVar("u32", SW0, Var("u32", SW0) + " - 1u")); break;

	case asBC_BNOT:   Put(SetVar("u32", SW0, "~" + Var("u32", SW0))); break;
	case asBC_BNOT64: Put(SetVar("u64", SW0, "~" + Var("u64", SW0))); break;

	// Arithmetic of integers is done unsigned, which wraps around like the machine
	// instructions the VM ends up with. The shifts are by the low bits like on x86
	case asBC_BAND:   BINARY("u32", "&"); break;
	case asBC_BOR:    BINARY("u32", "|"); break;
	case asBC_BXOR:   BINARY("u32", "^"); break;
	case asBC_BSLL:   SHIFT("u32", "<<", "31"); break;
	case asBC_BSRL:   SHIFT("u32", ">>", "31"); break;
	case asBC_BSRA:   SHIFT("i32", ">>", "31"); break;
	case asBC_BAND64: BINARY("u64", "&"); break;
	case asBC_BOR64:  BINARY("u64", "|"); break;
	case asBC_BXOR64: BINARY("u64", "^"); break;
	case asBC_BSLL64: SHIFT("u64", "<<", "63"); break;
	case asBC_BSRL64: SHIFT("u64", ">>", "63"); break;
	case asBC_BSRA64: SHIFT("i64", ">>", "63"); break;

	case asBC_ADDi:   BINARY("u32", "+"); break;
	case asBC_SUBi:   BINARY("u32", "-"); break;
	case asBC_MULi:   BINARY("u32", "*"); break;
	case asBC_ADDi64: BINARY("u64", "+"); break;
	case asBC_SUBi64: BINARY("u64", "-"); break;
	case asBC_MULi64: BINARY("u64", "*"); break;
	case asBC_ADDf:   BINARY("f32", "+"); break;
	case asBC_SUBf:   BINARY("f32", "-"); break;
	case asBC_MULf:   BINARY("f32", "*"); break;
	case asBC_ADDd:   BINARY("f64", "+"); break;
	case asBC_SUBd:   BINARY("f64", "-"); break;
	case asBC_MULd:   BINARY("f64", "*"); break;

	// The divisions return to the VM where it raises the exceptions
	case asBC_DIVi:
	case asBC_MODi:
		Emit("{ int x_ = %s, y_ = %s; if( y_ == 0 || (y_ == -1 && x_ == (-2147483647 - 1)) ) %s %s }",
			Var("i32", SW1).c_str(), Var("i32", SW2).c_str(), Bail().c_str(), SetVar("i32", SW0, instr.op == asBC_DIVi ? "x_ / y_" : "x_ % y_").c_str());
		break;
	case asBC_DIVi64:
	case asBC_MODi64:
		Emit("{ asINT64 x_ = %s, y_ = %s; if( y_ == 0 || (y_ == -1 && (asQWORD)x_ == ((asQWORD)1 << 63)) ) %s %s }",
			Var("i64", SW1).c_str(), Var("i64", SW2).c_str(), Bail().c_str(), SetVar("i64", SW0, instr.op == asBC_DIVi64 ? "x_ / y_" : "x_ % y_").c_str());
		break;
	case asBC_DIVu:
	case asBC_MODu:
		Emit("{ asDWORD y_ = %s; if( y_ == 0 ) %s %s }", Var("u32", SW2).c_str(), Bail().c_str(),
			SetVar("u32", SW0, Var("u32", SW1) + (instr.op == asBC_DIVu ? " / y_" : " % y_")).c_str());
		break;
	case asBC_DIVu64:
	case asBC_MODu64:
		Emit("{ asQWORD y_ = %s; if( y_ == 0 ) %s %s }", Var("u64", SW2).c_str(), Bail().c_str(),
			SetVar("u64", SW0, Var("u64", SW1) + (instr.op == asBC_DIVu64 ? " / y_" : " % y_")).c_str());
		break;
	case asBC_DIVf:
		Emit("{ float y_ = %s; if( y_ == 0 ) %s %s }", Var("f32", SW2).c_str(), Bail().c_str(), SetVar("f32", SW0, Var("f32", SW1) + " / y_").c_str());
		break;
	case asBC_MODf:
		Emit("{ float y_ = %s; if( y_ == 0 ) %s %s }", Var("f32", SW2).c_str(), Bail().c_str(), SetVar("f32", SW0, "fmodf(" + Var("f32", SW1) + ", y_)").c_str());
		break;
	case asBC_DIVd:
		Emit("{ double y_ = %s; if( y_ == 0 ) %s %s }", Var("f64", SW2).c_str(), Bail().c_str(), SetVar("f64", SW0, Var("f64", SW1) + " / y_").c_str());
		break;
	case asBC_MODd:
		Emit("{ double y_ = %s; if( y_ == 0 ) %s %s }", Var("f64", SW2).c_str(), Bail().c_str(), SetVar("f64", SW0, "fmod(" + Var("f64", SW1) + ", y_)").c_str());
		break;

	case asBC_ADDIi: Put(SetVar("u32", SW0, Var("u32", SW1) + Format(" + %uu", DW2))); break;
	case asBC_SUBIi: Put(SetVar("u32", SW0, Var("u32", SW1) + Format(" - %uu", DW2))); break;
	case asBC_MULIi: Put(SetVar("u32", SW0, Var("u32", SW1) + Format(" * %uu", DW2))); break;
	case asBC_ADDIf: Put(SetVar("f32", SW0, Var("f32", SW1) + Format(" + aot_f32bits(0x%08xu)", DW2))); break;
	case asBC_SUBIf: Put(SetVar("f32", SW0, Var("f32", SW1) + Format(" - aot_f32bits(0x%08xu)", DW2))); break;
	case asBC_MULIf: Put(SetVar("f32", SW0, Var("f32", SW1) + Format(" * aot_f32bits(0x%08xu)", DW2))); break;

	case asBC_COPY:
		// Like the VM, which raises the exception for null pointers
		Emit("{");
		Emit("\tvoid *d_ = (void*)AOT_S(pw, 0);");
		Emit("\tvoid *s_ = (void*)AOT_S(pw, %d);", P);
		Emit("\tif( d_ == 0 || s_ == 0 )");
		Emit("\t\t%s", Bail().c_str());
		Emit("\tmemcpy(d_, s_, %u);", W0 * 4);
		Emit("\tsp += %d;", P);
		Emit("\tAOT_S(pw, 0) = (asPWORD)d_;");
		Emit("}");
		break;

	case asBC_PshC8:   Emit("sp -= 2; AOT_S(u64, 0) = 0x%llxull;", QW1); break;
	case asBC_PshVPtr: Emit("sp -= %d; AOT_S(pw, 0) = %s;", P, Var("pw", SW0).c_str()); break;
	case asBC_RDSPtr:  Emit("{ aot_pw *a_ = (aot_pw*)AOT_S(pw, 0); if( a_ == 0 ) %s AOT_S(pw, 0) = *a_; }", Bail().c_str()); break;

	case asBC_CMPd:   COMPARE("double", "f64"); break;
	case asBC_CMPu:   COMPARE("asDWORD", "u32"); break;
	case asBC_CMPf:   COMPARE("float", "f32"); break;
	case asBC_CMPi:   COMPARE("int", "i32"); break;
	case asBC_CMPi64: COMPARE("asINT64", "i64"); break;
	case asBC_CMPu64: COMPARE("asQWORD", "u64"); break;
	case asBC_CmpPtr: COMPARE("asPWORD", "pw"); break;
	case asBC_CMPIi:  Emit("AOT_SETVR(asDWORD, aot_cmp<int>(%s, %s));", Var("i32", SW0).c_str(), IntLiteral(int(DW1)).c_str()); break;
	case asBC_CMPIf:  Emit("AOT_SETVR(asDWORD, aot_cmp<float>(%s, aot_f32bits(0x%08xu)));", Var("f32", SW0).c_str(), DW1); break;
	case asBC_CMPIu:  Emit("AOT_SETVR(asDWORD, aot_cmp<asDWORD>(%s, %uu));", Var("u32", SW0).c_str(), DW1); break;

	case asBC_JMPP:
		{
			// The table of jumps that follows isn't reached otherwise
			const std::vector<int> &targets = m_code.GetSwitchTargets(idx);
			Emit("switch( %s )", Var("i32", SW0).c_str());
			Emit("{");
			for( asUINT n = 0; n < targets.size(); n++ )
				Emit("case %u: goto L_%d;", n, targets[n]);
			Emit("default: %s", Bail().c_str());
			Emit("}");
		}
		break;

	case asBC_PopRPtr: Emit("AOT_SETVR(asPWORD, AOT_S(pw, 0)); sp += %d;", P); break;
	case asBC_PshRPtr: Emit("sp -= %d; AOT_S(pw, 0) = AOT_GETVR(asPWORD);", P); break;
	case asBC_STR:     Put(Bail()); break;

	case asBC_CALLSYS:
		if( instr.flags & JIT_INSTR_INDEXER )
		{
			EmitIndexer(idx);
			break;
		}
		{
			SJITSystemCall call;
			if( GetSystemCall(instr, call) )
			{
				EmitSystemCall(idx, call);
				break;
			}
		}
		EmitSync();
		Emit("if( JIT_CallSystem(regs, AOT_INT(%u)) )", pos + 1);
		Emit("\treturn 1;");
		Emit("AOT_RELOAD();");
		EmitReload();
		break;

	case asBC_CALLBND:
		EmitSync();
		Emit("if( JIT_CallScript(regs, JIT_CALL_BOUND, AOT_INT(%u), 0, callLimit) )", pos + 1);
		Emit("\treturn 1;");
		Emit("AOT_RELOAD();");
		EmitReload();
		break;

	case asBC_CallPtr:
		EmitSync();
		Emit("if( JIT_CallScript(regs, JIT_CALL_PTR, 0, %s, callLimit) )", Var("pw", SW1).c_str());
		Emit("\treturn 1;");
		Emit("AOT_RELOAD();");
		EmitReload();
		break;

	case asBC_SUSPEND:
		// The debugger may modify the variables in the line callback. The full flag is
		// read only where it may have been set since the last check. A line callback
		// still checks every statement, as AOT functions have no exact-code variant
		Emit("if( %s )", m_suspendChecked ? "ctx->m_lineCallback" : "AOT_SUSPENDING()");
		Emit("{");
		EmitSync("\t");
		Emit("\tif( JIT_Suspend(regs) )");
		Emit("\t\treturn 1;");
		EmitReload("\t");
		{
			std::string fields = FieldLoads(m_code.GetFieldMask(idx));
			if( !fields.empty() )
				Emit("\t%s", fields.c_str());
		}
		Emit("}");
		break;

	case asBC_ALLOC:
		{
			// The objects of script classes are allocated like the VM does, with
			// everything stored, as the allocation may reuse the context for nested
			// calls, and stored in the variable whose address is pushed before the
			// arguments. Then they are pushed for the constructor, a script function
			// called like by asBC_CALL
			//
			// Registered types are allocated here. Constructors whose native ABI is
			// described by GetConstructorCall are called directly; the rest use JIT_Alloc.
			asCScriptEngine *engine = static_cast<asCScriptEngine*>(m_code.GetFunction()->GetEngine());
			int id = asBC_INTARG(instr.bc + P);
			asCScriptFunction *callee = id > 0 && asUINT(id) < engine->scriptFunctions.GetLength() ? engine->scriptFunctions[id] : 0;
			SJITSystemCall constructor;
			bool directConstructor = id != 0 && GetConstructorCall(engine, id, constructor);
			Emit("{");
			Emit("\tasCObjectType *o_ = (asCObjectType*)AOT_PW(%u);", pos + 1);
			EmitSync("\t");
			Emit("\tif( o_->flags & asOBJ_SCRIPT_OBJECT )");
			Emit("\t{");
			Emit("\t\tasCScriptFunction *f_ = ctx->m_engine->scriptFunctions[AOT_INT(%u)];", pos + 1 + P);
			Emit("\t\tvoid *m_ = JIT_NewScriptObject(o_);");
			Emit("\t\tvoid **a_ = (void**)AOT_S(pw, f_->GetSpaceNeededForArguments());");
			Emit("\t\tif( a_ )");
			Emit("\t\t\t*a_ = m_;");
			Emit("\t\tsp -= %d; AOT_S(pw, 0) = (asPWORD)m_;", P);
			Emit("\t\tJITFunction t_ = (JITFunction)f_->scriptData->jitFunction;");
			EmitCall(callee, pos + 2 + P, "\t\t", Format("JIT_CallScript(regs, JIT_CALL_CONSTRUCT, AOT_INT(%u), 0, callLimit)", pos + 1 + P));
			Emit("\t}");
			Emit("\telse");
			Emit("\t{");
			if( id == 0 )
			{
				Emit("\t\tvoid *m_ = ctx->m_engine->CallAlloc(o_);");
				Emit("\t\tvoid **a_ = (void**)AOT_S(pw, 0);");
				Emit("\t\tsp += %d;", P);
				Emit("\t\tif( a_ ) *a_ = m_;");
				Emit("\t\tif( AOT_SUSPENDING() )");
				Emit("\t\t{");
				Emit("\t\t\tregs->programPointer = bc + %u;", pos + instr.size);
				Emit("\t\t\tregs->stackPointer = sp;");
				Emit("\t\t\tif( ctx->m_doSuspend )");
				Emit("\t\t\t{");
				Emit("\t\t\t\tctx->m_status = asEXECUTION_SUSPENDED;");
				Emit("\t\t\t\treturn 1;");
				Emit("\t\t\t}");
				Emit("\t\t\tif( ctx->m_status != asEXECUTION_ACTIVE )");
				Emit("\t\t\t{");
				Emit("\t\t\t\tctx->m_engine->CallFree(m_);");
				Emit("\t\t\t\tif( a_ ) *a_ = 0;");
				Emit("\t\t\t\treturn 1;");
				Emit("\t\t\t}");
				Emit("\t\t}");
			}
			else if( directConstructor )
				EmitConstructor(idx, constructor);
			else
			{
				Emit("\t\tif( JIT_Alloc(regs, o_, AOT_INT(%u)) )", pos + 1 + P);
				Emit("\t\t\treturn 1;");
				Emit("\t\tAOT_RELOAD();");
			}
			Emit("\t}");
			EmitReload("\t");
			Emit("}");
		}
		break;

	case asBC_FREE:
	{
		int borrowedParam = m_direct ? m_code.FindParam(SW0) : -1;
		bool canBorrow = borrowedParam >= 0 && borrowedParam < 31 &&
		                 ((m_code.GetBorrowableParams() >> borrowedParam) & 1) != 0;
		if( canBorrow )
		{
			asUINT bit = 1u << borrowedParam;
			Emit("if( borrowed & %uu )", bit);
			Emit("{");
			Emit("\t%s", SetVar("pw", SW0, "0").c_str());
			Emit("\tborrowed &= ~%uu;", bit);
			Emit("}");
			Emit("else");
			Emit("{");
		}
		if( instr.flags & JIT_INSTR_MOVED )
		{
			// The copy before has taken over the reference, see CJITByteCode::FindMovedRefs
			Put(SetVar("pw", SW0, "0"));
		}
		else
		{
			Emit("if( %s )", Var("pw", SW0).c_str());
			Emit("{");
			if( instr.flags & JIT_INSTR_FREE_LIST )
			{
				// Nothing in the list is destroyed, see CJITByteCode::FindListFrees
				Emit("\tJIT_FreeMem((void*)%s);", Var("pw", SW0).c_str());
				Emit("\t%s", SetVar("pw", SW0, "0").c_str());
			}
			else if( instr.flags & JIT_INSTR_REFCOUNT )
			{
				// The references of the script objects are counted in place, see
				// CJITByteCode::FindInPlaceRefCounts. Like the VM the variable is cleared
				// after the release
				Emit("\tvoid *o_ = (void*)%s;", Var("pw", SW0).c_str());
				Emit("\tif( !AOT_Release(o_) )");
				Emit("\t{");
				EmitSync("\t\t");
				Emit("\t\tJIT_ReleaseScriptObject(o_);");
				Emit("\t}");
				Emit("\t%s", SetVar("pw", SW0, "0").c_str());
			}
			else
			{
				// The release may execute a script destructor, which leaves the variables
				// kept in local variables alone
				EmitSync("\t");
				Emit("\tJIT_Free(regs, (asCObjectType*)AOT_PW(%u), (asPWORD*)%s);", pos + 1, VarAddr(SW0).c_str());
			}
			Emit("}");
		}
		if( canBorrow )
			Emit("}");
		break;
	}

	case asBC_LOADOBJ:  Emit("regs->objectType = 0; regs->objectRegister = (void*)%s; %s", Var("pw", SW0).c_str(), SetVar("pw", SW0, "0").c_str()); break;
	case asBC_STOREOBJ: Emit("%s regs->objectRegister = 0;", SetVar("pw", SW0, "(asPWORD)regs->objectRegister").c_str()); break;
	case asBC_GETOBJ:   Emit("{ aot_pw *a_ = (aot_pw*)(sp + %u); aot_pw *v_ = (aot_pw*)(fp - (ptrdiff_t)*a_); *a_ = *v_; *v_ = 0; }", W0); break;
	case asBC_GETOBJREF: Emit("{ aot_pw *a_ = (aot_pw*)(sp + %u); *a_ = *(aot_pw*)(fp - (ptrdiff_t)*a_); }", W0); break;
	case asBC_GETREF:   Emit("{ aot_pw *a_ = (aot_pw*)(sp + %u); *a_ = (asPWORD)(fp - (int)*a_); }", W0); break;

	case asBC_REFCPY:
	case asBC_RefCpyV:
		// REFCPY pops the address of the destination, RefCpyV takes a variable
		Emit("{");
		if( instr.op == asBC_REFCPY )
		{
			Emit("\tvoid **d_ = (void**)AOT_S(pw, 0);");
			Emit("\tsp += %d;", P);
		}
		else
			Emit("\tvoid **d_ = (void**)%s;", VarAddr(SW0).c_str());
		Emit("\tvoid *s_ = (void*)AOT_S(pw, 0);");
		if( instr.flags & JIT_INSTR_BORROW )
		{
			const std::vector<int> &checks = m_code.GetBorrowChecks(idx);
			if( !checks.empty() )
			{
				std::string any;
				for( asUINT n = 0; n < checks.size(); n++ )
					any += (n ? " | " : "") + Var("pw", checks[n]);
				Emit("\tif( %s )", any.c_str());
				Emit("\t\t%s", Bail().c_str());
			}
			Emit("\t*d_ = s_;");
		}
		else if( instr.flags & JIT_INSTR_REFCOUNT )
		{
			// The references of the script objects are counted in place, see
			// CJITByteCode::FindInPlaceRefCounts. Like the VM the old object is
			// released before the new one gets its reference, which the handle may
			// take over from the variable released next, and the destination is set last
			Emit("\tvoid *o_ = *d_;");
			Emit("\tif( o_ && !AOT_Release(o_) )");
			Emit("\t{");
			EmitSync("\t\t");
			Emit("\t\tJIT_ReleaseScriptObject(o_);");
			Emit("\t}");
			if( !(instr.flags & JIT_INSTR_MOVE) )
			{
				Emit("\tif( s_ && !AOT_AddRef(s_) )");
				Emit("\t{");
				EmitSync("\t\t");
				Emit("\t\tJIT_AddRefScriptObject(s_);");
				Emit("\t}");
			}
			Emit("\t*d_ = s_;");
		}
		else if( instr.flags & JIT_INSTR_MOVE )
		{
			// The handle takes over the reference of the variable released next, see
			// CJITByteCode::FindMovedRefs, and the old object is released like the VM does
			EmitSync("\t");
			Emit("\tJIT_Free(regs, (asCObjectType*)AOT_PW(%u), (asPWORD*)d_);", pos + 1);
			Emit("\t*d_ = s_;");
			EmitReload("\t");
		}
		else
		{
			EmitSync("\t");
			Emit("\tJIT_RefCpy(regs, (asCObjectType*)AOT_PW(%u), d_, s_);", pos + 1);
			EmitReload("\t");
		}
		Emit("}");
		break;

	case asBC_CHKREF:   Emit("if( AOT_S(pw, 0) == 0 ) %s", Bail().c_str()); break;
	case asBC_ChkRefS:  Emit("if( *(aot_pw*)AOT_S(pw, 0) == 0 ) %s", Bail().c_str()); break;
	case asBC_ChkNullV: Emit("if( %s == 0 ) %s", Var("pw", SW0).c_str(), Bail().c_str()); break;
	case asBC_ChkNullS: Emit("if( AOT_S(pw, %u) == 0 ) %s", W0, Bail().c_str()); break;

	case asBC_PshNull:  Emit("sp -= %d; AOT_S(pw, 0) = 0;", P); break;
	case asBC_ClrVPtr:  Put(SetVar("pw", SW0, "0")); break;
	case asBC_OBJTYPE:
	case asBC_PGA:
	case asBC_FuncPtr:  Emit("sp -= %d; AOT_S(pw, 0) = AOT_PW(%u);", P, pos + 1); break;
	case asBC_TYPEID:   Emit("sp -= 1; AOT_S(u32, 0) = AOT_DW(%u);", pos + 1); break;
	case asBC_VAR:      Emit("sp -= %d; AOT_S(pw, 0) = (asPWORD)(ptrdiff_t)%d;", P, SW0); break;
	case asBC_PshV8:    Emit("sp -= 2; AOT_S(u64, 0) = %s;", Var("u64", SW0).c_str()); break;

	case asBC_SetV1:
	case asBC_SetV2:
	case asBC_SetV4:    Put(SetVar("u32", SW0, Format("%uu", DW1))); break;
	case asBC_SetV8:    Put(SetVar("u64", SW0, Format("0x%llxull", QW1))); break;
	case asBC_SetG4:    Emit("*(aot_u32*)AOT_PW(%u) = %uu;", pos + 1, asUINT(b[1 + P])); break;

	case asBC_ADDSi:    Emit("if( AOT_S(pw, 0) == 0 ) %s AOT_S(pw, 0) += (asPWORD)(ptrdiff_t)%d;", Bail().c_str(), SW0); break;

	case asBC_CpyVtoV4: Put(SetVar("u32", SW0, Var("u32", SW1))); break;
	case asBC_CpyVtoV8: Put(SetVar("u64", SW0, Var("u64", SW1))); break;
	case asBC_CpyVtoR4: Emit("AOT_SETVR(asDWORD, %s);", Var("u32", SW0).c_str()); break;
	case asBC_CpyVtoR8: Emit("vr = %s;", Var("u64", SW0).c_str()); break;
	case asBC_CpyVtoG4: Emit("*(aot_u32*)AOT_PW(%u) = %s;", pos + 1, Var("u32", SW0).c_str()); break;
	case asBC_CpyRtoV4: Put(SetVar("u32", SW0, "AOT_GETVR(asDWORD)")); break;
	case asBC_CpyRtoV8: Put(SetVar("u64", SW0, "vr")); break;
	case asBC_CpyGtoV4: Put(SetVar("u32", SW0, Format("*(aot_u32*)AOT_PW(%u)", pos + 1))); break;

	case asBC_WRTV1:    Emit("AOT_R(u8) = %s;", Var("u8", SW0).c_str()); break;
	case asBC_WRTV2:    Emit("AOT_R(u16) = %s;", Var("u16", SW0).c_str()); break;
	case asBC_WRTV4:
		{
			// The fields of the object kept in local variables are stored from there
			int f = m_code.GetFieldAccess(idx);
			if( f < 0 )
			{
				Emit("AOT_R(u32) = %s;", Var("u32", SW0).c_str());
				break;
			}
			const char *type = FrameType(m_fields[f].kind);
			Put(SetField(f, type, Var(type, SW0)));
			Emit("AOT_R(%s) = %s;", type, Field(f, type).c_str());
		}
		break;
	case asBC_WRTV8:    Emit("AOT_R(u64) = %s;", Var("u64", SW0).c_str()); break;
	case asBC_RDR1:     Put(SetVarLow("u8", SW0, "AOT_R(u8)")); break;
	case asBC_RDR2:     Put(SetVarLow("u16", SW0, "AOT_R(u16)")); break;
	case asBC_RDR4:
		{
			// The fields of the object kept in local variables are read from there,
			// and kept there when read from the memory
			int f = m_code.GetFieldAccess(idx);
			if( f < 0 )
			{
				Put(SetVar("u32", SW0, "AOT_R(u32)"));
				break;
			}
			const char *type = FrameType(m_fields[f].kind);
			if( !((m_code.GetFieldMask(idx) >> f) & 1) )
				Put(SetField(f, type, Format("AOT_R(%s)", type)));
			Put(SetVar(type, SW0, Field(f, type)));
		}
		break;
	case asBC_RDR8:     Put(SetVar("u64", SW0, "AOT_R(u64)")); break;
	case asBC_LDG:      Emit("AOT_SETVR(asPWORD, AOT_PW(%u));", pos + 1); break;
	case asBC_LDV:      Emit("AOT_SETVR(asPWORD, (asPWORD)%s);", VarAddr(SW0).c_str()); break;

	// Conversions, like the VM
	case asBC_iTOf:   Put(SetVar("f32", SW0, "(float)" + Var("i32", SW0))); break;
	case asBC_fTOi:   Put(SetVar("i32", SW0, "(int)" + Var("f32", SW0))); break;
	case asBC_uTOf:   Put(SetVar("f32", SW0, "(float)" + Var("u32", SW0))); break;
	case asBC_fTOu:   Emit("{ float f_ = %s; %s }", Var("f32", SW0).c_str(), SetVar("u32", SW0, "f_ < 0 ? (asUINT)(int)f_ : (asUINT)f_").c_str()); break;
	case asBC_sbTOi:  Put(SetVar("i32", SW0, Var("i8", SW0))); break;
	case asBC_swTOi:  Put(SetVar("i32", SW0, Var("i16", SW0))); break;
	case asBC_ubTOi:  Put(SetVar("u32", SW0, Var("u8", SW0))); break;
	case asBC_uwTOi:  Put(SetVar("u32", SW0, Var("u16", SW0))); break;
	case asBC_iTOb:   Put(SetVarLow("u8", SW0, Var("u32", SW0))); break;
	case asBC_iTOw:   Put(SetVarLow("u16", SW0, Var("u32", SW0))); break;
	case asBC_dTOi:   Put(SetVar("i32", SW0, "(int)" + Var("f64", SW1))); break;
	case asBC_dTOu:   Emit("{ double d_ = %s; %s }", Var("f64", SW1).c_str(), SetVar("u32", SW0, "d_ < 0 ? (asUINT)(int)d_ : (asUINT)d_").c_str()); break;
	case asBC_dTOf:   Put(SetVar("f32", SW0, "(float)" + Var("f64", SW1))); break;
	case asBC_iTOd:   Put(SetVar("f64", SW0, "(double)" + Var("i32", SW1))); break;
	case asBC_uTOd:   Put(SetVar("f64", SW0, "(double)" + Var("u32", SW1))); break;
	case asBC_fTOd:   Put(SetVar("f64", SW0, "(double)" + Var("f32", SW1))); break;
	case asBC_i64TOi: Put(SetVar("u32", SW0, "(asDWORD)" + Var("u64", SW1))); break;
	case asBC_uTOi64: Put(SetVar("i64", SW0, "(asINT64)" + Var("u32", SW1))); break;
	case asBC_iTOi64: Put(SetVar("i64", SW0, "(asINT64)" + Var("i32", SW1))); break;
	case asBC_fTOi64: Put(SetVar("i64", SW0, "(asINT64)" + Var("f32", SW1))); break;
	case asBC_dTOi64: Put(SetVar("i64", SW0, "(asINT64)" + Var("f64", SW0))); break;
	case asBC_fTOu64: Emit("{ float f_ = %s; %s }", Var("f32", SW1).c_str(), SetVar("u64", SW0, "f_ < 0 ? (asQWORD)(asINT64)f_ : (asQWORD)f_").c_str()); break;
	case asBC_dTOu64: Emit("{ double d_ = %s; %s }", Var("f64", SW0).c_str(), SetVar("u64", SW0, "d_ < 0 ? (asQWORD)(asINT64)d_ : (asQWORD)d_").c_str()); break;
	case asBC_i64TOf: Put(SetVar("f32", SW0, "(float)" + Var("i64", SW1))); break;
	case asBC_u64TOf: Put(SetVar("f32", SW0, "(float)" + Var("u64", SW1))); break;
	case asBC_i64TOd: Put(SetVar("f64", SW0, "(double)" + Var("i64", SW0))); break;
	case asBC_u64TOd: Put(SetVar("f64", SW0, "(double)" + Var("u64", SW0))); break;

	case asBC_Cast:
		Emit("JIT_Cast(regs, (void**)AOT_S(pw, 0), AOT_DW(%u)); sp += %d;", pos + 1, P);
		break;

	case asBC_ClrHi:    Emit("AOT_CLRHI();"); break;
	case asBC_JitEntry: break;

	case asBC_LoadThisR: Emit("{ asPWORD t_ = %s; if( t_ == 0 ) %s AOT_SETVR(asPWORD, t_ + (asPWORD)(ptrdiff_t)%d); }", Var("pw", 0).c_str(), Bail().c_str(), SW0); break;
	case asBC_LoadRObjR: Emit("{ asPWORD t_ = %s; if( t_ == 0 ) %s AOT_SETVR(asPWORD, t_ + (asPWORD)(ptrdiff_t)%d); }", Var("pw", SW0).c_str(), Bail().c_str(), SW1); break;
	case asBC_LoadVObjR: Emit("AOT_SETVR(asPWORD, (asPWORD)%s + (asPWORD)(ptrdiff_t)%d);", VarAddr(SW0).c_str(), SW1); break;

	case asBC_AllocMem:    Put(SetVar("pw", SW0, Format("(asPWORD)JIT_AllocMem(%uu)", DW1))); break;
	case asBC_SetListSize: Emit("*(aot_u32*)((asBYTE*)%s + %u) = %uu;", Var("pw", SW0).c_str(), DW1, DW2); break;
	case asBC_SetListType: Emit("*(aot_u32*)((asBYTE*)%s + %u) = AOT_DW(%u);", Var("pw", SW0).c_str(), DW1, pos + 2); break;
	case asBC_PshListElmnt: Emit("sp -= %d; AOT_S(pw, 0) = %s + %u;", P, Var("pw", SW0).c_str(), DW1); break;

	// The helpers raise the exceptions, as the result is stored anyway. They write
	// the result to the frame, which is loaded again if it is read later
	case asBC_POWi:
	case asBC_POWu:
	case asBC_POWf:
	case asBC_POWd:
	case asBC_POWdi:
	case asBC_POWi64:
	case asBC_POWu64:
		EmitSync();
		switch( instr.op )
		{
		case asBC_POWi:   Emit("if( JIT_POWi(regs, (int*)(fp - %d), %s, %s) )", SW0, Var("i32", SW1).c_str(), Var("i32", SW2).c_str()); break;
		case asBC_POWu:   Emit("if( JIT_POWu(regs, (asDWORD*)(fp - %d), %s, %s) )", SW0, Var("u32", SW1).c_str(), Var("u32", SW2).c_str()); break;
		case asBC_POWf:   Emit("if( JIT_POWf(regs, (float*)(fp - %d), %s, %s) )", SW0, Var("f32", SW1).c_str(), Var("f32", SW2).c_str()); break;
		case asBC_POWd:   Emit("if( JIT_POWd(regs, (double*)(fp - %d), %s, %s) )", SW0, Var("f64", SW1).c_str(), Var("f64", SW2).c_str()); break;
		case asBC_POWdi:  Emit("if( JIT_POWdi(regs, (double*)(fp - %d), %s, %s) )", SW0, Var("f64", SW1).c_str(), Var("i32", SW2).c_str()); break;
		case asBC_POWi64: Emit("if( JIT_POWi64(regs, (asINT64*)(fp - %d), (const asINT64*)(fp - %d), (const asINT64*)(fp - %d)) )", SW0, SW1, SW2); break;
		default:          Emit("if( JIT_POWu64(regs, (asQWORD*)(fp - %d), (const asQWORD*)(fp - %d), (const asQWORD*)(fp - %d)) )", SW0, SW1, SW2); break;
		}
		Emit("\treturn 1;");
		EmitReload();
		break;

	case asBC_Thiscall1:
		if( instr.flags & JIT_INSTR_INDEXER )
		{
			EmitIndexer(idx);
			break;
		}
		{
			SJITSystemCall call;
			if( GetSystemCall(instr, call) )
			{
				EmitSystemCall(idx, call);
				break;
			}
		}
		Emit("if( AOT_S(pw, 0) == 0 )");
		Emit("\t%s", Bail().c_str());
		EmitSync();
		Emit("if( JIT_Thiscall1(regs, AOT_INT(%u)) )", pos + 1);
		Emit("\treturn 1;");
		Emit("AOT_RELOAD();");
		EmitReload();
		break;

	default:
		return false;
	}

	#undef SW0
	#undef SW1
	#undef SW2
	#undef W0
	#undef DW1
	#undef DW2
	#undef QW1
	#undef BINARY
	#undef SHIFT
	#undef COMPARE

	return !m_failed;
}

END_AS_NAMESPACE
