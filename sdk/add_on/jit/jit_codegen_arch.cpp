// Architecture specific code generation. Everything the UniCompiler can't
// express in an arch neutral way is isolated here, with one implementation
// per supported architecture.

#include "jit_codegen.h"

BEGIN_AS_NAMESPACE

using namespace asmjit;
using namespace asmjit::ujit;

#if defined(ASMJIT_UJIT_X86)

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
