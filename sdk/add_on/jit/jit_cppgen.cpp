#include "jit_cppgen.h"

// Internal engine headers. The JIT must be compiled with the same
// configuration as the engine library (see CMakeLists.txt)
#include "as_scriptfunction.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

BEGIN_AS_NAMESPACE

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

CJITCppGen::CJITCppGen(const CJITByteCode &code) : m_code(code), m_pos(0), m_failed(false)
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

bool CJITCppGen::Generate(const char *name, std::string &out)
{
	const std::vector<SJITInstr> &instrs = m_code.GetInstructions();
	const std::vector<asUINT> &entries = m_code.GetEntries();
	m_out.clear();
	m_failed = false;
	for( asUINT n = 0; n < m_locals.size(); n++ )
		m_locals[n].used = m_locals[n].read = false;

	// Only the instructions that are jumped to get labels, as unused labels give warnings
	bool calls = false;
	m_labels.assign(instrs.size(), false);
	for( asUINT n = 0; n < entries.size(); n++ )
		m_labels[entries[n]] = true;
	for( asUINT n = 0; n < instrs.size(); n++ )
	{
		if( instrs[n].flags & JIT_INSTR_DEAD )
			continue;
		if( instrs[n].target >= 0 )
			m_labels[instrs[n].target] = true;
		if( instrs[n].op == asBC_JMPP )
		{
			const std::vector<int> &targets = m_code.GetSwitchTargets(n);
			for( asUINT t = 0; t < targets.size(); t++ )
				m_labels[targets[t]] = true;
		}
		if( CallsScript(instrs[n].op) )
			calls = true;
	}

	EmitEntry(calls);
	for( asUINT n = 0; n < instrs.size(); n++ )
	{
		if( instrs[n].flags & JIT_INSTR_DEAD )
			continue;
		if( m_labels[n] )
			m_out += Format("L_%u:;\n", n);

		// Where a loop with calls is entered, the variables that it doesn't modify
		// are stored once, see CJITByteCode::AnalyseDirtySlots
		Put(Stores(m_code.GetStoresBefore(n)));
		if( !EmitInstr(n) )
			return false;
		Put(Stores(m_code.GetStoresAfter(n)));
	}
	if( m_failed )
		return false;

	std::string text = Format("int %s(asSVMRegisters *regs, asPWORD jitArg, asUINT callLimit, asDWORD *stackPointer)\n{\n", name);
	text += "\tasCContext *ctx = (asCContext*)regs->ctx;\n";
	text += "\tasDWORD *bc, *fp, *sp;\n";
	text += "\tUJITAOTValue vr;\n";
	text += "\t(void)callLimit;\n";
	text += "\t(void)stackPointer;\n";
	for( asUINT n = 0; n < m_locals.size(); n++ )
	{
		const SLocal &local = m_locals[n];
		if( !local.used )
			continue;
		text += Format("\t%s %s = 0;\n", LocalType(local.kind), local.name.c_str());
		if( !local.read )
			text += Format("\t(void)%s;\n", local.name.c_str());
	}
	text += m_out;
	text += "}\n";

	out += text;
	return true;
}

// The VM enters at the JitEntry instructions with the frame set up, see JITFunction.
// Native callers enter at the start, and the frame is set up like
// asCContext::PrepareScriptFunction does when the stack block has room and the VM
// has nothing to do, otherwise by JIT_PrepareFrame. The variables kept in local
// variables are loaded from the frame then, see CJITByteCode::GetEntryMask
void CJITCppGen::EmitEntry(bool calls)
{
	const std::vector<SJITInstr> &instrs = m_code.GetInstructions();
	const std::vector<asUINT> &entries = m_code.GetEntries();
	asCScriptFunction *func = m_code.GetFunction();

	Emit("if( jitArg != 0 )");
	Emit("{");
	if( calls )
	{
		// The C++ exceptions of the registered functions that the functions called
		// natively call directly are caught there
		m_out += "#ifndef AS_NO_EXCEPTIONS\n";
		Emit("\tif( !(jitArg & JIT_GUARDED_ENTRY) )");
		Emit("\t\treturn JIT_GuardedEntry(regs, jitArg);");
		m_out += "#endif\n";
	}
	Emit("\tfp = regs->stackFramePointer;");
	Emit("\tsp = regs->stackPointer;");
	Emit("\tvr.q = regs->valueRegister;");
	if( calls )
		Emit("\tcallLimit = AOT_CallLimit(ctx);");
	Emit("\tswitch( jitArg & ~JIT_GUARDED_ENTRY )");
	Emit("\t{");
	for( asUINT n = 0; n < entries.size(); n++ )
	{
		std::string loads = Loads(m_code.GetEntryMask(entries[n]));
		Emit("\tcase %u: bc = regs->programPointer - %u; %s%sgoto L_%u;", n + 1, instrs[entries[n]].pos, loads.c_str(), loads.empty() ? "" : " ", entries[n]);
	}
	Emit("\t}");
	Emit("\tregs->programPointer += 1 + AS_PTR_SIZE;");
	Emit("\treturn 1;");
	Emit("}");

	std::vector<int> heap;
	GetHeapVariables(func, heap);
	Emit("bc = ctx->m_currentFunction->scriptData->byteCode.AddressOf();");
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
	Emit("\tregs->stackFramePointer = fp;");
	Emit("\tsp = fp - %u;", func->scriptData->variableSpace);
	Emit("}");
	Emit("vr.q = regs->valueRegister;");
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
// variables with the types of their kind, the conversions between integers of the
// same size aside, or with the untyped copies. Anything else can't happen, as the
// analysis wouldn't keep the variable in a register, but the function isn't
// generated then
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
		break;
	case JIT_SLOT_F32:
		if( strcmp(type, "f32") == 0 ) return name;
		if( strcmp(type, "u32") == 0 ) return Format("aot_bits32(%s)", name);
		if( strcmp(type, "i32") == 0 ) return Format("((int)aot_bits32(%s))", name);
		break;
	case JIT_SLOT_I64:
		if( strcmp(type, "u64") == 0 ) return name;
		if( strcmp(type, "i64") == 0 ) return Format("((asINT64)%s)", name);
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
		break;
	case JIT_SLOT_F32:
		if( strcmp(type, "f32") == 0 ) return Format("%s = %s;", name, value.c_str());
		if( is32 ) return Format("%s = aot_f32bits((asDWORD)(%s));", name, value.c_str());
		break;
	case JIT_SLOT_I64:
		if( is64 ) return Format("%s = (asQWORD)(%s);", name, value.c_str());
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
// them from it. The masks are those of the analysis, see CJITByteCode::GetDirtyMask
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

// The statement that returns to the VM at the instruction, which raises the exception
std::string CJITCppGen::Bail() const
{
	if( m_sync.empty() )
		return Format("AOT_BAIL(%u);", m_pos);
	return Format("{ %s AOT_BAIL(%u); }", m_sync.c_str(), m_pos);
}

// Where the VM, the engine, or the application may see the frame, i.e. at the
// calls, the helpers, and the returns to the VM, the local variables whose frame
// variables are older are stored like the JIT does. The calls leave them in the
// frame, which is loaded again where they are read later, and so do the
// instructions where the frame may have been modified, see GetReloadMask
void CJITCppGen::EmitSync(const char *indent)
{
	if( !m_sync.empty() )
		Emit("%s%s", indent, m_sync.c_str());
	Emit("%sAOT_SYNC(%u);", indent, m_pos);
}

void CJITCppGen::EmitReload(const char *indent)
{
	if( !m_reload.empty() )
		Emit("%s%s", indent, m_reload.c_str());
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
	#define COMPARE(C, T) Emit("vr.i32 = aot_cmp<" C ">(%s, %s);", Var(T, SW0).c_str(), Var(T, SW1).c_str())

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
	case asBC_LdGRdR4:  Emit("vr.pw = AOT_PW(%u); %s", pos + 1, SetVar("u32", SW0, "AOT_R(u32)").c_str()); break;

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
				return false;
		}
		break;

	case asBC_CALL:
		// The function is called natively if it has been compiled and the call stack
		// has room, like JIT_CallScript does, which is left the rest
		Emit("{");
		Put(m_sync.empty() ? "" : "\t" + m_sync);
		Emit("\tasCScriptFunction *f_ = ctx->m_engine->scriptFunctions[AOT_INT(%u)];", pos + 1);
		Emit("\tJITFunction t_ = (JITFunction)f_->scriptData->jitFunction;");
		Emit("\tif( t_ && ctx->m_callStack.GetLength() < callLimit )");
		Emit("\t{");
		Emit("\t\tif( AOT_CallNative(regs, ctx, f_, t_, fp, bc + %u, sp, callLimit) )", pos + 2);
		Emit("\t\t\treturn 1;");
		Emit("\t}");
		Emit("\telse");
		Emit("\t{");
		Emit("\t\tAOT_SYNC(%u);", pos);
		Emit("\t\tif( JIT_CallScript(regs, JIT_CALL_SCRIPT, AOT_INT(%u), 0, callLimit) )", pos + 1);
		Emit("\t\t\treturn 1;");
		Emit("\t}");
		Emit("\tAOT_RELOAD();");
		EmitReload("\t");
		Emit("}");
		break;

	case asBC_RET:
		// Like the VM, and asCContext::PopCallState, unless the function was the first
		// one or a nested call, which finishes the execution
		Emit("{");
		Emit("\tasUINT l_ = ctx->m_callStack.GetLength();");
		Emit("\tif( l_ == 0 || ctx->m_callStack.AddressOf()[l_ - CALLSTACK_FRAME_SIZE] == 0 )");
		Emit("\t{");
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
		Emit("\tregs->valueRegister = vr.q;");
		Emit("\treturn 0;");
		Emit("}");
		break;

	case asBC_JMP:    Emit("goto L_%d;", instr.target); break;
	case asBC_JZ:     Emit("if( vr.i32 == 0 ) goto L_%d;", instr.target); break;
	case asBC_JNZ:    Emit("if( vr.i32 != 0 ) goto L_%d;", instr.target); break;
	case asBC_JS:     Emit("if( vr.i32 < 0 ) goto L_%d;", instr.target); break;
	case asBC_JNS:    Emit("if( vr.i32 >= 0 ) goto L_%d;", instr.target); break;
	case asBC_JP:     Emit("if( vr.i32 > 0 ) goto L_%d;", instr.target); break;
	case asBC_JNP:    Emit("if( vr.i32 <= 0 ) goto L_%d;", instr.target); break;
	case asBC_JLowZ:  Emit("if( vr.bytes[0] == 0 ) goto L_%d;", instr.target); break;
	case asBC_JLowNZ: Emit("if( vr.bytes[0] != 0 ) goto L_%d;", instr.target); break;

	case asBC_TZ:     Emit("AOT_TEST(vr.i32 == 0);"); break;
	case asBC_TNZ:    Emit("AOT_TEST(vr.i32 != 0);"); break;
	case asBC_TS:     Emit("AOT_TEST(vr.i32 < 0);"); break;
	case asBC_TNS:    Emit("AOT_TEST(vr.i32 >= 0);"); break;
	case asBC_TP:     Emit("AOT_TEST(vr.i32 > 0);"); break;
	case asBC_TNP:    Emit("AOT_TEST(vr.i32 <= 0);"); break;

	case asBC_NEGi:   Put(SetVar("u32", SW0, "0u - " + Var("u32", SW0))); break;
	case asBC_NEGf:   Put(SetVar("f32", SW0, "-" + Var("f32", SW0))); break;
	case asBC_NEGd:   Put(SetVar("f64", SW0, "-" + Var("f64", SW0))); break;
	case asBC_NEGi64: Put(SetVar("u64", SW0, "(asQWORD)0 - " + Var("u64", SW0))); break;

	case asBC_INCi16: Emit("++AOT_R(u16);"); break;
	case asBC_INCi8:  Emit("++AOT_R(u8);"); break;
	case asBC_DECi16: Emit("--AOT_R(u16);"); break;
	case asBC_DECi8:  Emit("--AOT_R(u8);"); break;
	case asBC_INCi:   Emit("++AOT_R(u32);"); break;
	case asBC_DECi:   Emit("--AOT_R(u32);"); break;
	case asBC_INCf:   Emit("AOT_R(f32) += 1.0f;"); break;
	case asBC_DECf:   Emit("AOT_R(f32) -= 1.0f;"); break;
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
	case asBC_CMPIi:  Emit("vr.i32 = aot_cmp<int>(%s, %s);", Var("i32", SW0).c_str(), IntLiteral(int(DW1)).c_str()); break;
	case asBC_CMPIf:  Emit("vr.i32 = aot_cmp<float>(%s, aot_f32bits(0x%08xu));", Var("f32", SW0).c_str(), DW1); break;
	case asBC_CMPIu:  Emit("vr.i32 = aot_cmp<asDWORD>(%s, %uu);", Var("u32", SW0).c_str(), DW1); break;

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

	case asBC_PopRPtr: Emit("vr.pw = AOT_S(pw, 0); sp += %d;", P); break;
	case asBC_PshRPtr: Emit("sp -= %d; AOT_S(pw, 0) = vr.pw;", P); break;
	case asBC_STR:     Put(Bail()); break;

	case asBC_CALLSYS:
		EmitSync();
		Emit("if( JIT_CallSystem(regs, AOT_INT(%u)) )", pos + 1);
		Emit("\treturn 1;");
		Emit("AOT_RELOAD();");
		EmitReload();
		break;

	case asBC_CALLBND:
	case asBC_CALLINTF:
		EmitSync();
		Emit("if( JIT_CallScript(regs, %s, AOT_INT(%u), 0, callLimit) )", instr.op == asBC_CALLBND ? "JIT_CALL_BOUND" : "JIT_CALL_INTERFACE", pos + 1);
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
		// The debugger may modify the variables in the line callback
		Emit("if( AOT_SUSPENDING() )");
		Emit("{");
		EmitSync("\t");
		Emit("\tif( JIT_Suspend(regs) )");
		Emit("\t\treturn 1;");
		EmitReload("\t");
		Emit("}");
		break;

	case asBC_ALLOC:
		// Script classes are constructed by a script function
		Emit("{");
		Emit("\tasCObjectType *o_ = (asCObjectType*)AOT_PW(%u);", pos + 1);
		EmitSync("\t");
		Emit("\tif( (o_->flags & asOBJ_SCRIPT_OBJECT) ? JIT_CallScript(regs, JIT_CALL_ALLOC, AOT_INT(%u), (asPWORD)o_, callLimit) : JIT_Alloc(regs, o_, AOT_INT(%u)) )", pos + 1 + P, pos + 1 + P);
		Emit("\t\treturn 1;");
		Emit("\tAOT_RELOAD();");
		EmitReload("\t");
		Emit("}");
		break;

	case asBC_FREE:
		// The release may execute a script destructor, which leaves the variables
		// kept in local variables alone
		Emit("if( %s )", Var("pw", SW0).c_str());
		Emit("{");
		EmitSync("\t");
		Emit("\tJIT_Free(regs, (asCObjectType*)AOT_PW(%u), (asPWORD*)%s);", pos + 1, VarAddr(SW0).c_str());
		Emit("}");
		break;

	case asBC_LOADOBJ:  Emit("regs->objectType = 0; regs->objectRegister = (void*)%s; %s", Var("pw", SW0).c_str(), SetVar("pw", SW0, "0").c_str()); break;
	case asBC_STOREOBJ: Emit("%s regs->objectRegister = 0;", SetVar("pw", SW0, "(asPWORD)regs->objectRegister").c_str()); break;
	case asBC_GETOBJ:   Emit("{ aot_pw *a_ = (aot_pw*)(sp + %u); aot_pw *v_ = (aot_pw*)(fp - (ptrdiff_t)*a_); *a_ = *v_; *v_ = 0; }", W0); break;
	case asBC_GETOBJREF: Emit("{ aot_pw *a_ = (aot_pw*)(sp + %u); *a_ = *(aot_pw*)(fp - (ptrdiff_t)*a_); }", W0); break;
	case asBC_GETREF:   Emit("{ aot_pw *a_ = (aot_pw*)(sp + %u); *a_ = (asPWORD)(fp - (int)*a_); }", W0); break;

	case asBC_REFCPY:
		Emit("{");
		Emit("\tvoid **d_ = (void**)AOT_S(pw, 0);");
		Emit("\tsp += %d;", P);
		EmitSync("\t");
		Emit("\tJIT_RefCpy(regs, (asCObjectType*)AOT_PW(%u), d_, (void*)AOT_S(pw, 0));", pos + 1);
		EmitReload("\t");
		Emit("}");
		break;

	case asBC_RefCpyV:
		EmitSync();
		Emit("JIT_RefCpy(regs, (asCObjectType*)AOT_PW(%u), (void**)%s, (void*)AOT_S(pw, 0));", pos + 1, VarAddr(SW0).c_str());
		EmitReload();
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
	case asBC_CpyVtoR4: Emit("vr.u32 = %s;", Var("u32", SW0).c_str()); break;
	case asBC_CpyVtoR8: Emit("vr.q = %s;", Var("u64", SW0).c_str()); break;
	case asBC_CpyVtoG4: Emit("*(aot_u32*)AOT_PW(%u) = %s;", pos + 1, Var("u32", SW0).c_str()); break;
	case asBC_CpyRtoV4: Put(SetVar("u32", SW0, "vr.u32")); break;
	case asBC_CpyRtoV8: Put(SetVar("u64", SW0, "vr.q")); break;
	case asBC_CpyGtoV4: Put(SetVar("u32", SW0, Format("*(aot_u32*)AOT_PW(%u)", pos + 1))); break;

	case asBC_WRTV1:    Emit("AOT_R(u8) = %s;", Var("u8", SW0).c_str()); break;
	case asBC_WRTV2:    Emit("AOT_R(u16) = %s;", Var("u16", SW0).c_str()); break;
	case asBC_WRTV4:    Emit("AOT_R(u32) = %s;", Var("u32", SW0).c_str()); break;
	case asBC_WRTV8:    Emit("AOT_R(u64) = %s;", Var("u64", SW0).c_str()); break;
	case asBC_RDR1:     Put(SetVarLow("u8", SW0, "AOT_R(u8)")); break;
	case asBC_RDR2:     Put(SetVarLow("u16", SW0, "AOT_R(u16)")); break;
	case asBC_RDR4:     Put(SetVar("u32", SW0, "AOT_R(u32)")); break;
	case asBC_RDR8:     Put(SetVar("u64", SW0, "AOT_R(u64)")); break;
	case asBC_LDG:      Emit("vr.pw = AOT_PW(%u);", pos + 1); break;
	case asBC_LDV:      Emit("vr.p = %s;", VarAddr(SW0).c_str()); break;

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

	case asBC_LoadThisR: Emit("{ asPWORD t_ = %s; if( t_ == 0 ) %s vr.pw = t_ + (asPWORD)(ptrdiff_t)%d; }", Var("pw", 0).c_str(), Bail().c_str(), SW0); break;
	case asBC_LoadRObjR: Emit("{ asPWORD t_ = %s; if( t_ == 0 ) %s vr.pw = t_ + (asPWORD)(ptrdiff_t)%d; }", Var("pw", SW0).c_str(), Bail().c_str(), SW1); break;
	case asBC_LoadVObjR: Emit("vr.pw = (asPWORD)%s + (asPWORD)(ptrdiff_t)%d;", VarAddr(SW0).c_str(), SW1); break;

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
