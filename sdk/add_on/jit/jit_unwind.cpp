#include "jit_unwind.h"

#include <string.h>

#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
	#define JIT_UNWIND_WIN64
#elif defined(_MSC_VER) && defined(_M_IX86)
	#define JIT_UNWIND_HANDLER_CHAIN
#elif defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
	#define JIT_UNWIND_DWARF
#endif

#if defined(JIT_UNWIND_WIN64) || defined(JIT_UNWIND_DWARF)
#include <asmjit/x86.h>
#endif

#ifdef JIT_UNWIND_WIN64
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef JIT_UNWIND_DWARF
// libgcc and libunwind, also the one of macOS. Both accept the address of a
// single FDE, which is followed by a zero terminator for libgcc
extern "C" void __register_frame(void *fde);
extern "C" void __deregister_frame(void *fde);
#endif

BEGIN_AS_NAMESPACE

CJITUnwindInfo::CJITUnwindInfo()
{
	m_start       = 0;
	m_end         = 0;
	m_tableOffset = 0;
}

bool CJITUnwindInfo::IsSupported()
{
#if defined(JIT_UNWIND_WIN64) || defined(JIT_UNWIND_DWARF) || defined(JIT_UNWIND_HANDLER_CHAIN)
	return true;
#else
	return false;
#endif
}

#if defined(JIT_UNWIND_WIN64)
// Operations of the Win64 unwind codes
enum
{
	UWOP_PUSH_NONVOL     = 0,
	UWOP_ALLOC_LARGE     = 1,
	UWOP_ALLOC_SMALL     = 2,
	UWOP_SAVE_XMM128     = 8,
	UWOP_SAVE_XMM128_FAR = 9
};

static inline asWORD UnwindCode(asUINT offset, asUINT op, asUINT info)
{
	return asWORD(offset | (op << 8) | (info << 12));
}
#endif

#if defined(JIT_UNWIND_DWARF)
// DWARF call frame instructions
enum
{
	DW_CFA_nop             = 0x00,
	DW_CFA_advance_loc1    = 0x02,
	DW_CFA_advance_loc2    = 0x03,
	DW_CFA_advance_loc4    = 0x04,
	DW_CFA_def_cfa         = 0x0c,
	DW_CFA_def_cfa_offset  = 0x0e,
	DW_CFA_advance_loc     = 0x40,
	DW_CFA_offset          = 0x80
};

// DWARF numbers of the x86-64 general purpose registers, by physical id
static const asBYTE g_dwarfGpRegs[16] = { 0, 2, 1, 3, 7, 6, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15 };
static const asBYTE DWARF_REG_RSP = 7;
static const asBYTE DWARF_REG_RA  = 16;

static void PutU8(std::vector<asBYTE> &out, asUINT v)
{
	out.push_back(asBYTE(v));
}

static void PutU16(std::vector<asBYTE> &out, asUINT v)
{
	PutU8(out, v);
	PutU8(out, v >> 8);
}

static void PutU32(std::vector<asBYTE> &out, asUINT v)
{
	PutU16(out, v);
	PutU16(out, v >> 16);
}

static void PutU64(std::vector<asBYTE> &out, asQWORD v)
{
	PutU32(out, asUINT(v));
	PutU32(out, asUINT(v >> 32));
}

static void PutULEB(std::vector<asBYTE> &out, asUINT v)
{
	do
	{
		asBYTE b = asBYTE(v & 0x7F);
		v >>= 7;
		out.push_back(v ? asBYTE(b | 0x80) : b);
	} while( v );
}

// Pads the entry that starts at start to a multiple of 8 bytes, and sets its length
static void EndEntry(std::vector<asBYTE> &out, size_t start)
{
	while( (out.size() - start) % 8 )
		PutU8(out, DW_CFA_nop);
	asUINT length = asUINT(out.size() - start - 4);
	memcpy(&out[start], &length, 4);
}
#endif

bool CJITUnwindInfo::Prepare(asmjit::BaseCompiler &cc, const asmjit::FuncNode *func)
{
	m_ops.clear();

#if defined(JIT_UNWIND_HANDLER_CHAIN)
	(void)cc;
	(void)func;
	return true;
#elif !defined(JIT_UNWIND_WIN64) && !defined(JIT_UNWIND_DWARF)
	(void)cc;
	(void)func;
	return false;
#else
	using namespace asmjit;

	// Neither is used by the generated code
	const FuncFrame &frame = func->frame();
	if( frame.has_preserved_fp() || frame.has_dynamic_alignment() )
		return false;

	// Emit the prologue again with a builder to see what each instruction does, and
	// bind a label after each instruction to see where it ends
	CodeHolder &holder = *cc.code();
	CodeHolder tmp;
	if( tmp.init(holder.environment(), holder.cpu_features()) != Error::kOk )
		return false;
	x86::Builder b(&tmp);
	b.add_encoding_options(cc.encoding_options());
	if( b.emit_prolog(frame) != Error::kOk )
		return false;

	std::vector<InstNode*> insts;
	for( BaseNode *node = b.first_node(); node; node = node->next() )
		if( node->is_inst() )
			insts.push_back(node->as<InstNode>());
	std::vector<Label> ends(insts.size());
	for( size_t n = 0; n < insts.size(); n++ )
	{
		b.set_cursor(insts[n]);
		ends[n] = b.new_label();
		if( b.bind(ends[n]) != Error::kOk )
			return false;
	}
	if( b.finalize() != Error::kOk )
		return false;

	for( size_t n = 0; n < insts.size(); n++ )
	{
		const InstNode *inst = insts[n];
		SOp op;
		op.end   = asUINT(tmp.label_offset(ends[n]));
		op.reg   = 0;
		op.value = 0;

		Operand a, c;
		if( inst->op_count() > 0 ) a = inst->op(0);
		if( inst->op_count() > 1 ) c = inst->op(1);
		InstId id = inst->inst_id();
		if( id == x86::Inst::kIdEndbr64 )
			continue;
		else if( id == x86::Inst::kIdPush && inst->op_count() == 1 && a.is_gp() )
		{
			op.kind = OP_PUSH;
			op.reg  = a.as<Reg>().id();
		}
		else if( id == x86::Inst::kIdMov && inst->op_count() == 2 && a.is_gp() && c.is_gp() && c.as<Reg>().id() == x86::Gp::kIdSp )
			continue; // copy of the stack pointer to access the arguments
		else if( id == x86::Inst::kIdSub && inst->op_count() == 2 && a.is_gp() && a.as<Reg>().id() == x86::Gp::kIdSp && c.is_imm() )
		{
			op.kind  = OP_ALLOC;
			op.value = asUINT(c.as<Imm>().value());
		}
		else if( inst->op_count() == 2 && a.is_mem() && c.is_vec() && c.as<Reg>().size() == 16 &&
		         a.as<x86::Mem>().has_base_reg() && a.as<x86::Mem>().base_id() == x86::Gp::kIdSp &&
		         !a.as<x86::Mem>().has_index() && a.as<x86::Mem>().offset() >= 0 )
		{
			op.kind  = OP_SAVE_VEC;
			op.reg   = c.as<Reg>().id();
			op.value = asUINT(a.as<x86::Mem>().offset());
		}
		else
			return false;
		m_ops.push_back(op);
	}

	// The prologue must be what the compiler emitted at the start of the function
	const CodeBuffer &text = holder.text_section()->buffer();
	const CodeBuffer &prologue = tmp.text_section()->buffer();
	m_start = asUINT(holder.label_offset(func->label()));
	m_end   = asUINT(text.size());
	if( m_start + prologue.size() > text.size() || memcmp(text.data() + m_start, prologue.data(), prologue.size()) != 0 )
		return false;

#if defined(JIT_UNWIND_WIN64)
	// UNWIND_INFO, with the unwind codes in the reverse order of the prologue
	std::vector<asWORD> codes;
	for( size_t n = m_ops.size(); n-- > 0; )
	{
		const SOp &op = m_ops[n];
		switch( op.kind )
		{
		case OP_PUSH:
			codes.push_back(UnwindCode(op.end, UWOP_PUSH_NONVOL, op.reg));
			break;
		case OP_ALLOC:
			if( op.value % 8 )
				return false;
			if( op.value <= 128 )
				codes.push_back(UnwindCode(op.end, UWOP_ALLOC_SMALL, op.value / 8 - 1));
			else if( op.value <= 512 * 1024 - 8 )
			{
				codes.push_back(UnwindCode(op.end, UWOP_ALLOC_LARGE, 0));
				codes.push_back(asWORD(op.value / 8));
			}
			else
			{
				codes.push_back(UnwindCode(op.end, UWOP_ALLOC_LARGE, 1));
				codes.push_back(asWORD(op.value));
				codes.push_back(asWORD(op.value >> 16));
			}
			break;
		case OP_SAVE_VEC:
			if( op.value % 16 )
				return false;
			if( op.value / 16 <= 0xFFFF )
			{
				codes.push_back(UnwindCode(op.end, UWOP_SAVE_XMM128, op.reg));
				codes.push_back(asWORD(op.value / 16));
			}
			else
			{
				codes.push_back(UnwindCode(op.end, UWOP_SAVE_XMM128_FAR, op.reg));
				codes.push_back(asWORD(op.value));
				codes.push_back(asWORD(op.value >> 16));
			}
			break;
		}
	}
	if( prologue.size() > 255 || codes.size() > 255 )
		return false;

	std::vector<asBYTE> info;
	info.push_back(1); // version 1, no handler
	info.push_back(asBYTE(prologue.size()));
	info.push_back(asBYTE(codes.size()));
	info.push_back(0); // no frame register
	if( codes.size() % 2 )
		codes.push_back(0);
	for( size_t n = 0; n < codes.size(); n++ )
	{
		info.push_back(asBYTE(codes[n]));
		info.push_back(asBYTE(codes[n] >> 8));
	}

	// Append it and the RUNTIME_FUNCTION to the code. Their addresses are relative
	// to the start of the code, so they must be in the same allocation
	x86::Assembler a(&holder);
	if( a.align(AlignMode::kZero, 4) != Error::kOk )
		return false;
	asUINT infoOffset = asUINT(a.offset());
	if( a.embed(info.data(), info.size()) != Error::kOk )
		return false;
	m_tableOffset = asUINT(a.offset());
	asDWORD table[3] = { m_start, m_end, infoOffset };
	if( a.embed(table, sizeof(table)) != Error::kOk )
		return false;
#else
	for( size_t n = 0; n < m_ops.size(); n++ )
		if( m_ops[n].kind == OP_SAVE_VEC || (m_ops[n].kind == OP_PUSH && m_ops[n].reg >= 16) )
			return false;
#endif

	return true;
#endif
}

bool CJITUnwindInfo::Register(void *code, void **handle) const
{
	*handle = 0;

#if defined(JIT_UNWIND_WIN64)
	RUNTIME_FUNCTION *table = reinterpret_cast<RUNTIME_FUNCTION*>(static_cast<char*>(code) + m_tableOffset);
	if( !RtlAddFunctionTable(table, 1, DWORD64(code)) )
		return false;
	*handle = table;
	return true;
#elif defined(JIT_UNWIND_DWARF)
	std::vector<asBYTE> data;

	// CIE with the state at the entry of a function: the return address is at the
	// stack pointer, and the call frame address is the stack pointer before the call
	PutU32(data, 0);   // length
	PutU32(data, 0);   // CIE id
	PutU8(data, 1);    // version
	PutU8(data, 'z');  // augmentation
	PutU8(data, 'R');
	PutU8(data, 0);
	PutULEB(data, 1);  // code alignment
	PutU8(data, 0x78); // data alignment -8
	PutU8(data, DWARF_REG_RA);
	PutULEB(data, 1);  // augmentation data length
	PutU8(data, 0);    // DW_EH_PE_absptr for the addresses in the FDE
	PutU8(data, DW_CFA_def_cfa);
	PutULEB(data, DWARF_REG_RSP);
	PutULEB(data, 8);
	PutU8(data, DW_CFA_offset | DWARF_REG_RA);
	PutULEB(data, 1);
	EndEntry(data, 0);

	// FDE for the function, following the prologue
	size_t fde = data.size();
	PutU32(data, 0);                                // length
	PutU32(data, asUINT(fde + 4));                  // offset back to the CIE
	PutU64(data, asQWORD(asPWORD(code)) + m_start); // start address
	PutU64(data, m_end - m_start);                  // size
	PutULEB(data, 0);                               // augmentation data length
	asUINT loc = 0, cfa = 8;
	for( size_t n = 0; n < m_ops.size(); n++ )
	{
		const SOp &op = m_ops[n];
		asUINT delta = op.end - loc;
		if( delta < 64 )
			PutU8(data, DW_CFA_advance_loc | delta);
		else if( delta < 256 )
		{
			PutU8(data, DW_CFA_advance_loc1);
			PutU8(data, delta);
		}
		else if( delta < 65536 )
		{
			PutU8(data, DW_CFA_advance_loc2);
			PutU16(data, delta);
		}
		else
		{
			PutU8(data, DW_CFA_advance_loc4);
			PutU32(data, delta);
		}
		loc = op.end;

		cfa += op.kind == OP_PUSH ? 8 : op.value;
		PutU8(data, DW_CFA_def_cfa_offset);
		PutULEB(data, cfa);
		if( op.kind == OP_PUSH )
		{
			PutU8(data, DW_CFA_offset | g_dwarfGpRegs[op.reg]);
			PutULEB(data, cfa / 8);
		}
	}
	EndEntry(data, fde);
	PutU32(data, 0); // terminator

	asBYTE *copy = new asBYTE[data.size()];
	memcpy(copy, data.data(), data.size());
	__register_frame(copy + fde);
	*handle = copy;
	return true;
#else
	(void)code;
	return true;
#endif
}

void CJITUnwindInfo::Unregister(void *handle)
{
	if( handle == 0 )
		return;

#if defined(JIT_UNWIND_WIN64)
	RtlDeleteFunctionTable(static_cast<RUNTIME_FUNCTION*>(handle));
#elif defined(JIT_UNWIND_DWARF)
	// The FDE follows the CIE, whose length doesn't include the length field
	asBYTE *data = static_cast<asBYTE*>(handle);
	asUINT cieLength;
	memcpy(&cieLength, data, 4);
	__deregister_frame(data + 4 + cieLength);
	delete[] data;
#endif
}

END_AS_NAMESPACE
