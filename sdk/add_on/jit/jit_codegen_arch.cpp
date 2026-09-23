// Architecture specific code generation. Everything the UniCompiler can't
// express in an arch neutral way is isolated here, with one implementation
// per supported architecture.

#include "jit_codegen.h"

BEGIN_AS_NAMESPACE

using namespace asmjit;
using namespace asmjit::ujit;

#if defined(ASMJIT_UJIT_X86)

// Callee-saved home registers for the registers used all through the function,
// see AssignHomeRegs. The allocator uses the scratch registers ECX|RCX and EDI|R15
// for jumps, so they are avoided. Otherwise these are only hints, so if a register
// isn't available, e.g. EBP|RBP as frame pointer, the allocator picks another one
void CJITCodeGen::SetHomeRegHints()
{
	x86::Compiler *cc = m_uc.cc;
	cc->virt_reg_by_reg(m_regs)->set_home_id_hint(x86::Gp::kIdBx);
	cc->virt_reg_by_reg(m_sp)->set_home_id_hint(x86::Gp::kIdBp);
	if( Is64Bit() )
	{
		// RSI and RDI aren't callee-saved on System V
		cc->virt_reg_by_reg(m_fp)->set_home_id_hint(x86::Gp::kIdR14);
		if( m_depth.is_valid() )
			cc->virt_reg_by_reg(m_depth)->set_home_id_hint(x86::Gp::kIdR12);
	}
	else
		cc->virt_reg_by_reg(m_fp)->set_home_id_hint(x86::Gp::kIdSi);
}

// dst = a / b or a % b for signed integers. The operands must have been
// checked for division by zero and overflow already
void CJITCodeGen::EmitSignedDiv(const Gp &dst, const Gp &a, const Gp &b, bool isMod)
{
	x86::Compiler *cc = m_uc.cc;
	Gp lo = a.size() == 8 ? m_uc.new_gp64() : m_uc.new_gp32();
	Gp hi = a.size() == 8 ? m_uc.new_gp64() : m_uc.new_gp32();
	cc->mov(lo, a);
	if( a.size() == 8 )
		cc->cqo(hi, lo);
	else
		cc->cdq(hi, lo);
	cc->idiv(hi, lo, b);
	cc->mov(dst, isMod ? hi : lo);
}

// Branch on the result of a floating point compare. The VM compares as
// (a == b) ? 0 : (a < b) ? -1 : 1, so an unordered compare counts as greater
bool CJITCodeGen::EmitFloatCompareBranch(const Vec &a, const Vec &b, bool isDouble, asEBCInstr branch, const Label &target)
{
	x86::Compiler *cc = m_uc.cc;
	x86::Vec va = a.as<x86::Vec>().xmm();
	x86::Vec vb = b.as<x86::Vec>().xmm();

	switch( branch )
	{
	case asBC_JZ:
	case asBC_JLowZ:
		{
			// Equal and ordered
			Label skip = m_uc.new_label();
			if( isDouble ) cc->ucomisd(va, vb); else cc->ucomiss(va, vb);
			cc->jp(skip);
			cc->je(target);
			cc->bind(skip);
		}
		break;

	case asBC_JNZ:
	case asBC_JLowNZ:
		// Not equal or unordered
		if( isDouble ) cc->ucomisd(va, vb); else cc->ucomiss(va, vb);
		cc->jne(target);
		cc->jp(target);
		break;

	case asBC_JS:
		// a < b, ordered. Compare b to a so that unordered doesn't take the branch
		if( isDouble ) cc->ucomisd(vb, va); else cc->ucomiss(vb, va);
		cc->ja(target);
		break;

	case asBC_JNS:
		// !(a < b) including unordered
		if( isDouble ) cc->ucomisd(vb, va); else cc->ucomiss(vb, va);
		cc->jbe(target);
		break;

	case asBC_JP:
		// a > b or unordered
		if( isDouble ) cc->ucomisd(vb, va); else cc->ucomiss(vb, va);
		cc->jb(target);
		break;

	case asBC_JNP:
		// a <= b, ordered
		if( isDouble ) cc->ucomisd(vb, va); else cc->ucomiss(vb, va);
		cc->jae(target);
		break;

	default:
		return false;
	}

	return true;
}

#elif defined(ASMJIT_UJIT_AARCH64)

// Callee-saved home registers, see the x86 version. The allocator's scratch
// registers are X27 and X28
void CJITCodeGen::SetHomeRegHints()
{
	a64::Compiler *cc = m_uc.cc;
	cc->virt_reg_by_reg(m_regs)->set_home_id_hint(19);
	cc->virt_reg_by_reg(m_fp)->set_home_id_hint(20);
	cc->virt_reg_by_reg(m_sp)->set_home_id_hint(21);
	if( m_depth.is_valid() )
		cc->virt_reg_by_reg(m_depth)->set_home_id_hint(22);
}

void CJITCodeGen::EmitSignedDiv(const Gp &dst, const Gp &a, const Gp &b, bool isMod)
{
	a64::Compiler *cc = m_uc.cc;
	Gp q = a.size() == 8 ? m_uc.new_gp64() : m_uc.new_gp32();
	cc->sdiv(q, a, b);
	if( isMod )
		cc->msub(dst, q, b, a);
	else
		cc->mov(dst, q);
}

bool CJITCodeGen::EmitFloatCompareBranch(const Vec &a, const Vec &b, bool isDouble, asEBCInstr branch, const Label &target)
{
	a64::Compiler *cc = m_uc.cc;
	a64::Vec va = isDouble ? a.as<a64::Vec>().d() : a.as<a64::Vec>().s();
	a64::Vec vb = isDouble ? b.as<a64::Vec>().d() : b.as<a64::Vec>().s();

	// fcmp sets N for less, Z for equal, C for greater or equal, and C|V for unordered
	cc->fcmp(va, vb);
	switch( branch )
	{
	case asBC_JZ:
	case asBC_JLowZ:  cc->b_eq(target); break;
	case asBC_JNZ:
	case asBC_JLowNZ: cc->b_ne(target); break;
	case asBC_JS:     cc->b_mi(target); break;
	case asBC_JNS:    cc->b_pl(target); break;
	case asBC_JP:     cc->b_hi(target); break;
	case asBC_JNP:    cc->b_ls(target); break;
	default:
		return false;
	}

	return true;
}

#else

void CJITCodeGen::SetHomeRegHints()
{
}

void CJITCodeGen::EmitSignedDiv(const Gp &, const Gp &, const Gp &, bool)
{
	m_failed = true;
}

bool CJITCodeGen::EmitFloatCompareBranch(const Vec &, const Vec &, bool, asEBCInstr, const Label &)
{
	return false;
}

#endif

END_AS_NAMESPACE
