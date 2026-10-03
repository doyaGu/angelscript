#include "jit_codegen.h"
#include "jit_runtime.h"

#include <stddef.h>

BEGIN_AS_NAMESPACE

using namespace asmjit;
using namespace asmjit::ujit;

// Constants loaded by the generated code
static const float  g_oneF32 = 1.0f;
static const double g_oneF64 = 1.0;

// Relations used by the compare instructions
enum ERelation { REL_EQ, REL_NE, REL_LT, REL_GE, REL_GT, REL_LE };

static bool IsCondJump(asEBCInstr op)
{
	switch( op )
	{
	case asBC_JZ: case asBC_JNZ: case asBC_JS: case asBC_JNS: case asBC_JP: case asBC_JNP:
	case asBC_JLowZ: case asBC_JLowNZ:
		return true;
	default:
		return false;
	}
}

static bool IsTest(asEBCInstr op)
{
	switch( op )
	{
	case asBC_TZ: case asBC_TNZ: case asBC_TS: case asBC_TNS: case asBC_TP: case asBC_TNP:
		return true;
	default:
		return false;
	}
}

// The relation a conditional jump or test expresses when applied to the
// result of a compare instruction (0 equal, -1 less, 1 greater)
static ERelation RelationOf(asEBCInstr op)
{
	switch( op )
	{
	case asBC_JZ:  case asBC_TZ:  case asBC_JLowZ:  return REL_EQ;
	case asBC_JNZ: case asBC_TNZ: case asBC_JLowNZ: return REL_NE;
	case asBC_JS:  case asBC_TS:  return REL_LT;
	case asBC_JNS: case asBC_TNS: return REL_GE;
	case asBC_JP:  case asBC_TP:  return REL_GT;
	default:                      return REL_LE;
	}
}

static CondCode CondCodeOf(ERelation rel, bool isUnsigned)
{
	switch( rel )
	{
	case REL_EQ: return CondCode::kEqual;
	case REL_NE: return CondCode::kNotEqual;
	case REL_LT: return isUnsigned ? CondCode::kUnsignedLT : CondCode::kSignedLT;
	case REL_GE: return isUnsigned ? CondCode::kUnsignedGE : CondCode::kSignedGE;
	case REL_GT: return isUnsigned ? CondCode::kUnsignedGT : CondCode::kSignedGT;
	default:     return isUnsigned ? CondCode::kUnsignedLE : CondCode::kSignedLE;
	}
}

static UniCondition MakeCond(ERelation rel, bool isUnsigned, const Gp &a, const Operand &b)
{
	return UniCondition(UniOpCond::kCompare, CondCodeOf(rel, isUnsigned), a, b);
}

//------------------------------------------------------------------------
// Integer arithmetic

bool CJITCodeGen::EmitIntMath(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	const asDWORD *bc = instr.bc;
	int a0 = asBC_SWORDARG0(bc);
	int a1 = asBC_SWORDARG1(bc);
	int a2 = asBC_SWORDARG2(bc);

	switch( instr.op )
	{
	case asBC_TZ:
	case asBC_TNZ:
	case asBC_TS:
	case asBC_TNS:
	case asBC_TP:
	case asBC_TNP:
		{
			Gp t = m_uc.new_gp32();
			LoadVR32(t);
			Gp r = m_uc.new_gp32();
			m_uc.select(r, Imm(1), Imm(0), MakeCond(RelationOf(instr.op), false, t, Imm(0)));
			StoreVR32(r);
		}
		break;

	case asBC_NOT:
		{
			Gp x = Load32(a0);
			Gp r = Dst32(a0);
#if AS_SIZEOF_BOOL == 1
			// Only the lowest byte holds the boolean value
			m_uc.and_(r, x, Imm(0xFF));
			m_uc.select(r, Imm(1), Imm(0), test_z(r));
#else
			m_uc.select(r, Imm(1), Imm(0), test_z(x));
#endif
			Commit32(a0, r);
		}
		break;

	case asBC_ClrHi:
#if AS_SIZEOF_BOOL == 1
		{
			Gp t = m_uc.new_gp32();
			LoadVR32(t);
			m_uc.and_(t, t, Imm(0xFF));
			StoreVR32(t);
		}
#endif
		break;

	case asBC_NEGi:
		{
			Gp x = Load32(a0);
			Gp r = Dst32(a0);
			m_uc.neg(r, x);
			Commit32(a0, r);
		}
		break;

	case asBC_IncVi:
	case asBC_DecVi:
		{
			Gp x = Load32(a0);
			Gp r = Dst32(a0);
			if( instr.op == asBC_IncVi )
				m_uc.add(r, x, Imm(1));
			else
				m_uc.sub(r, x, Imm(1));
			Commit32(a0, r);
		}
		break;

	case asBC_BNOT:
		{
			Gp x = Load32(a0);
			Gp r = Dst32(a0);
			m_uc.not_(r, x);
			Commit32(a0, r);
		}
		break;

	case asBC_BAND:
	case asBC_BOR:
	case asBC_BXOR:
	case asBC_ADDi:
	case asBC_SUBi:
	case asBC_MULi:
		{
			Gp a = Load32(a1);
			Gp b = Load32(a2);
			Gp r = Dst32(a0);
			switch( instr.op )
			{
			case asBC_BAND: m_uc.and_(r, a, b); break;
			case asBC_BOR:  m_uc.or_(r, a, b); break;
			case asBC_BXOR: m_uc.xor_(r, a, b); break;
			case asBC_ADDi: m_uc.add(r, a, b); break;
			case asBC_SUBi: m_uc.sub(r, a, b); break;
			default:        m_uc.mul(r, a, b); break;
			}
			Commit32(a0, r);
		}
		break;

	case asBC_BSLL:
	case asBC_BSRL:
	case asBC_BSRA:
		{
			Gp a = Load32(a1);
			Gp b = Load32(a2);
			Gp r = Dst32(a0);
			if( instr.op == asBC_BSLL )
				m_uc.shl(r, a, b);
			else if( instr.op == asBC_BSRL )
				m_uc.shr(r, a, b);
			else
				m_uc.sar(r, a, b);
			Commit32(a0, r);
		}
		break;

	case asBC_ADDIi:
	case asBC_SUBIi:
	case asBC_MULIi:
		{
			Gp a = Load32(a1);
			Gp r = Dst32(a0);
			Imm imm(asBC_INTARG(bc + 1));
			if( instr.op == asBC_ADDIi )
				m_uc.add(r, a, imm);
			else if( instr.op == asBC_SUBIi )
				m_uc.sub(r, a, imm);
			else
				m_uc.mul(r, a, imm);
			Commit32(a0, r);
		}
		break;

	case asBC_DIVi: EmitDivMod(idx, false, true, false); break;
	case asBC_MODi: EmitDivMod(idx, false, true, true); break;
	case asBC_DIVu: EmitDivMod(idx, false, false, false); break;
	case asBC_MODu: EmitDivMod(idx, false, false, true); break;

	case asBC_POWi:
	case asBC_POWu:
		{
			Gp a = Load32(a1);
			Gp b = Load32(a2);
			Gp dst = m_uc.new_gp_ptr();
			LeaVar(dst, a0);
			SyncAll(idx);
			InvokeNode *call = Invoke(instr.op == asBC_POWi ? (const void*)JIT_POWi : (const void*)JIT_POWu, FuncSignature::build<int, asSVMRegisters*, void*, asDWORD, asDWORD>());
			Gp r = m_uc.new_gp32();
			SetRegsArg(call, 0);
			call->set_arg(1, dst);
			call->set_arg(2, a);
			call->set_arg(3, b);
			call->set_ret(0, r);
			// The result was written to memory
			ReloadCachedSlot(a0);
			EmitLeaveIf(r);
			EmitDematerialize();
		}
		break;

	// 64bit integers
	case asBC_NEGi64:
	case asBC_BNOT64:
		if( Is64Bit() )
		{
			Gp x = Load64(a0);
			Gp r = Dst64(a0);
			if( instr.op == asBC_NEGi64 )
				m_uc.neg(r, x);
			else
				m_uc.not_(r, x);
			Commit64(a0, r);
		}
		else
		{
			Gp lo = m_uc.new_gp32();
			Gp hi = m_uc.new_gp32();
			m_uc.load_u32(lo, Var(a0));
			m_uc.load_u32(hi, Var(a0, 4));
			if( instr.op == asBC_BNOT64 )
			{
				m_uc.not_(lo, lo);
				m_uc.not_(hi, hi);
			}
			else
			{
				Gp borrow = m_uc.new_gp32();
				m_uc.select(borrow, Imm(1), Imm(0), test_nz(lo));
				m_uc.neg(lo, lo);
				m_uc.neg(hi, hi);
				m_uc.sub(hi, hi, borrow);
			}
			m_uc.store_u32(Var(a0), lo);
			m_uc.store_u32(Var(a0, 4), hi);
		}
		break;

	case asBC_ADDi64:
	case asBC_SUBi64:
	case asBC_MULi64:
	case asBC_BAND64:
	case asBC_BOR64:
	case asBC_BXOR64:
	case asBC_BSLL64:
	case asBC_BSRL64:
	case asBC_BSRA64:
	case asBC_DIVi64:
	case asBC_MODi64:
	case asBC_DIVu64:
	case asBC_MODu64:
	case asBC_POWi64:
	case asBC_POWu64:
		if( Is64Bit() && instr.op != asBC_POWi64 && instr.op != asBC_POWu64 )
		{
			switch( instr.op )
			{
			case asBC_DIVi64: EmitDivMod(idx, true, true, false); break;
			case asBC_MODi64: EmitDivMod(idx, true, true, true); break;
			case asBC_DIVu64: EmitDivMod(idx, true, false, false); break;
			case asBC_MODu64: EmitDivMod(idx, true, false, true); break;

			case asBC_BSLL64:
			case asBC_BSRL64:
			case asBC_BSRA64:
				{
					Gp a = Load64(a1);
					Gp b32 = Load32(a2);
					Gp b = m_uc.new_gp64();
					m_uc.mov(b.r32(), b32);
					Gp r = Dst64(a0);
					if( instr.op == asBC_BSLL64 )
						m_uc.shl(r, a, b);
					else if( instr.op == asBC_BSRL64 )
						m_uc.shr(r, a, b);
					else
						m_uc.sar(r, a, b);
					Commit64(a0, r);
				}
				break;

			default:
				{
					Gp a = Load64(a1);
					Gp b = Load64(a2);
					Gp r = Dst64(a0);
					switch( instr.op )
					{
					case asBC_ADDi64: m_uc.add(r, a, b); break;
					case asBC_SUBi64: m_uc.sub(r, a, b); break;
					case asBC_MULi64: m_uc.mul(r, a, b); break;
					case asBC_BAND64: m_uc.and_(r, a, b); break;
					case asBC_BOR64:  m_uc.or_(r, a, b); break;
					default:          m_uc.xor_(r, a, b); break;
					}
					Commit64(a0, r);
				}
				break;
			}
		}
		else if( !Is64Bit() && (instr.op == asBC_ADDi64 || instr.op == asBC_SUBi64) )
		{
			Gp alo = m_uc.new_gp32();
			Gp ahi = m_uc.new_gp32();
			Gp blo = m_uc.new_gp32();
			Gp bhi = m_uc.new_gp32();
			Gp lo = m_uc.new_gp32();
			Gp hi = m_uc.new_gp32();
			Gp carry = m_uc.new_gp32();
			m_uc.load_u32(alo, Var(a1));
			m_uc.load_u32(ahi, Var(a1, 4));
			m_uc.load_u32(blo, Var(a2));
			m_uc.load_u32(bhi, Var(a2, 4));
			if( instr.op == asBC_ADDi64 )
			{
				m_uc.add(lo, alo, blo);
				m_uc.select(carry, Imm(1), Imm(0), ucmp_lt(lo, alo));
				m_uc.add(hi, ahi, bhi);
				m_uc.add(hi, hi, carry);
			}
			else
			{
				m_uc.select(carry, Imm(1), Imm(0), ucmp_lt(alo, blo));
				m_uc.sub(lo, alo, blo);
				m_uc.sub(hi, ahi, bhi);
				m_uc.sub(hi, hi, carry);
			}
			m_uc.store_u32(Var(a0), lo);
			m_uc.store_u32(Var(a0, 4), hi);
		}
		else if( !Is64Bit() && instr.op == asBC_MULi64 )
		{
			Gp alo = m_uc.new_gp32();
			Gp ahi = m_uc.new_gp32();
			Gp blo = m_uc.new_gp32();
			Gp bhi = m_uc.new_gp32();
			Gp aLow16 = m_uc.new_gp32();
			Gp aHigh16 = m_uc.new_gp32();
			Gp bLow16 = m_uc.new_gp32();
			Gp bHigh16 = m_uc.new_gp32();
			Gp w0 = m_uc.new_gp32();
			Gp w1 = m_uc.new_gp32();
			Gp w2 = m_uc.new_gp32();
			Gp t = m_uc.new_gp32();
			Gp lo = m_uc.new_gp32();
			Gp hi = m_uc.new_gp32();
			m_uc.load_u32(alo, Var(a1));
			m_uc.load_u32(ahi, Var(a1, 4));
			m_uc.load_u32(blo, Var(a2));
			m_uc.load_u32(bhi, Var(a2, 4));

			// Compute both halves of alo * blo from four 16bit products. The
			// other two products only contribute their low halves to the result.
			m_uc.and_(aLow16, alo, Imm(0xFFFF));
			m_uc.shr(aHigh16, alo, Imm(16));
			m_uc.and_(bLow16, blo, Imm(0xFFFF));
			m_uc.shr(bHigh16, blo, Imm(16));
			m_uc.mul(w0, aLow16, bLow16);
			m_uc.shr(t, w0, Imm(16));
			m_uc.mul(w1, aHigh16, bLow16);
			m_uc.add(w1, w1, t);
			m_uc.shr(w2, w1, Imm(16));
			m_uc.and_(w1, w1, Imm(0xFFFF));
			m_uc.mul(t, aLow16, bHigh16);
			m_uc.add(w1, w1, t);
			m_uc.mul(hi, aHigh16, bHigh16);
			m_uc.add(hi, hi, w2);
			m_uc.shr(t, w1, Imm(16));
			m_uc.add(hi, hi, t);
			m_uc.shl(lo, w1, Imm(16));
			m_uc.and_(w0, w0, Imm(0xFFFF));
			m_uc.add(lo, lo, w0);
			m_uc.mul(t, alo, bhi);
			m_uc.add(hi, hi, t);
			m_uc.mul(t, ahi, blo);
			m_uc.add(hi, hi, t);
			m_uc.store_u32(Var(a0), lo);
			m_uc.store_u32(Var(a0, 4), hi);
		}
		else if( !Is64Bit() && (instr.op == asBC_BAND64 || instr.op == asBC_BOR64 || instr.op == asBC_BXOR64) )
		{
			Gp alo = m_uc.new_gp32();
			Gp ahi = m_uc.new_gp32();
			Gp blo = m_uc.new_gp32();
			Gp bhi = m_uc.new_gp32();
			m_uc.load_u32(alo, Var(a1));
			m_uc.load_u32(ahi, Var(a1, 4));
			m_uc.load_u32(blo, Var(a2));
			m_uc.load_u32(bhi, Var(a2, 4));
			if( instr.op == asBC_BAND64 )
			{
				m_uc.and_(alo, alo, blo);
				m_uc.and_(ahi, ahi, bhi);
			}
			else if( instr.op == asBC_BOR64 )
			{
				m_uc.or_(alo, alo, blo);
				m_uc.or_(ahi, ahi, bhi);
			}
			else
			{
				m_uc.xor_(alo, alo, blo);
				m_uc.xor_(ahi, ahi, bhi);
			}
			m_uc.store_u32(Var(a0), alo);
			m_uc.store_u32(Var(a0, 4), ahi);
		}
		else if( !Is64Bit() && (instr.op == asBC_BSLL64 || instr.op == asBC_BSRL64 || instr.op == asBC_BSRA64) )
		{
			Gp alo = m_uc.new_gp32();
			Gp ahi = m_uc.new_gp32();
			Gp countSrc = Load32(a2);
			Gp count = m_uc.new_gp32();
			Gp shift = m_uc.new_gp32();
			Gp inverse = m_uc.new_gp32();
			Gp lo = m_uc.new_gp32();
			Gp hi = m_uc.new_gp32();
			Gp loSmall = m_uc.new_gp32();
			Gp hiSmall = m_uc.new_gp32();
			Gp loLarge = m_uc.new_gp32();
			Gp hiLarge = m_uc.new_gp32();
			Gp cross = m_uc.new_gp32();
			Gp crossRaw = m_uc.new_gp32();
			Gp zero = m_uc.new_gp32();
			m_uc.load_u32(alo, Var(a1));
			m_uc.load_u32(ahi, Var(a1, 4));
			m_uc.and_(count, countSrc, Imm(63));
			m_uc.and_(shift, count, Imm(31));
			m_uc.neg(inverse, shift);
			m_uc.and_(inverse, inverse, Imm(31));
			m_uc.mov(zero, Imm(0));

			if( instr.op == asBC_BSLL64 )
			{
				m_uc.shl(loSmall, alo, shift);
				m_uc.shl(hiSmall, ahi, shift);
				m_uc.shr(crossRaw, alo, inverse);
				m_uc.select(cross, zero, crossRaw, test_z(shift));
				m_uc.or_(hiSmall, hiSmall, cross);
				m_uc.mov(loLarge, zero);
				m_uc.shl(hiLarge, alo, shift);
			}
			else
			{
				m_uc.shr(loSmall, alo, shift);
				m_uc.shl(crossRaw, ahi, inverse);
				m_uc.select(cross, zero, crossRaw, test_z(shift));
				m_uc.or_(loSmall, loSmall, cross);
				m_uc.shr(loLarge, ahi, shift);
				if( instr.op == asBC_BSRL64 )
				{
					m_uc.shr(hiSmall, ahi, shift);
					m_uc.mov(hiLarge, zero);
				}
				else
				{
					m_uc.sar(hiSmall, ahi, shift);
					m_uc.sar(loLarge, ahi, shift);
					m_uc.sar(hiLarge, ahi, Imm(31));
				}
			}

			m_uc.select(lo, loLarge, loSmall, ucmp_ge(count, Imm(32)));
			m_uc.select(hi, hiLarge, hiSmall, ucmp_ge(count, Imm(32)));
			m_uc.store_u32(Var(a0), lo);
			m_uc.store_u32(Var(a0, 4), hi);
		}
		else if( instr.op == asBC_POWi64 || instr.op == asBC_POWu64 )
		{
			Gp dst = m_uc.new_gp_ptr();
			Gp pa = m_uc.new_gp_ptr();
			Gp pb = m_uc.new_gp_ptr();
			LeaVar(dst, a0);
			LeaVar(pa, a1);
			LeaVar(pb, a2);
			// The operands are read from memory by the helper
			SyncAll(idx);
			InvokeNode *call = Invoke(instr.op == asBC_POWi64 ? (const void*)JIT_POWi64 : (const void*)JIT_POWu64, FuncSignature::build<int, asSVMRegisters*, void*, const void*, const void*>());
			Gp r = m_uc.new_gp32();
			SetRegsArg(call, 0);
			call->set_arg(1, dst);
			call->set_arg(2, pa);
			call->set_arg(3, pb);
			call->set_ret(0, r);
			ReloadCachedSlot(a0);
			EmitLeaveIf(r);
			EmitDematerialize();
		}
		else
		{
			// 32bit host: the operation is done by a helper on the memory slots
			Gp dst = m_uc.new_gp_ptr();
			Gp pa = m_uc.new_gp_ptr();
			Gp pb = m_uc.new_gp_ptr();
			LeaVar(dst, a0);
			LeaVar(pa, a1);
			LeaVar(pb, a2);
			StoreCachedSlot(a1);
			StoreCachedSlot(a2);
			InvokeNode *call = Invoke((const void*)JIT_I64Op, FuncSignature::build<int, int, void*, const void*, const void*>());
			Gp r = m_uc.new_gp32();
			call->set_arg(0, Imm(int(instr.op)));
			call->set_arg(1, dst);
			call->set_arg(2, pa);
			call->set_arg(3, pb);
			call->set_ret(0, r);
			ReloadCachedSlot(a0);
			m_uc.j(BailLabel(idx), test_nz(r));
		}
		break;

	default:
		return false;
	}

	return true;
}

// Integer division and modulo with the checks the VM does. The VM raises the
// exception when the instruction is re-executed after bailing out
void CJITCodeGen::EmitDivMod(asUINT idx, bool is64, bool isSigned, bool isMod)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	int a0 = asBC_SWORDARG0(instr.bc);
	int a1 = asBC_SWORDARG1(instr.bc);
	int a2 = asBC_SWORDARG2(instr.bc);

	Gp a = is64 ? Load64(a1) : Load32(a1);
	Gp b = is64 ? Load64(a2) : Load32(a2);
	Gp r = is64 ? Dst64(a0) : Dst32(a0);
	Gp q = is64 ? m_uc.new_gp64() : m_uc.new_gp32();
	Label bail = BailLabel(idx);

	if( isSigned )
	{
		// b == 0 and b == -1 need special treatment, both are caught with one compare
		Label slow = m_uc.new_label();
		Label done = m_uc.new_label();
		Gp t = is64 ? m_uc.new_gp64() : m_uc.new_gp32();
		m_uc.add(t, b, Imm(1));
		m_uc.j(slow, ucmp_le(t, Imm(1)));

		EmitSignedDiv(q, a, b, isMod);

		BaseNode *cold = BeginCold(slow);
		m_uc.j(bail, test_z(b));
		// b == -1: overflow if a is the smallest negative number
		if( is64 )
		{
			Gp minVal = m_uc.new_gp64();
			m_uc.mov(minVal, Imm(int64_t(asINT64(1) << 63)));
			m_uc.j(bail, cmp_eq(a, minVal));
		}
		else
			m_uc.j(bail, cmp_eq(a, Imm(int(0x80000000))));
		if( isMod )
			m_uc.mov(q, Imm(0));
		else
			m_uc.neg(q, a);
		EndCold(cold, done);

		m_uc.bind(done);
	}
	else
	{
		m_uc.j(bail, test_z(b));
		if( isMod )
			m_uc.umod(q, a, b);
		else
			m_uc.udiv(q, a, b);
	}

	m_uc.mov(r, q);
	if( is64 )
		Commit64(a0, r);
	else
		Commit32(a0, r);
}

//------------------------------------------------------------------------
// Floating point arithmetic

bool CJITCodeGen::EmitFloatMath(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	const asDWORD *bc = instr.bc;
	int a0 = asBC_SWORDARG0(bc);
	int a1 = asBC_SWORDARG1(bc);
	int a2 = asBC_SWORDARG2(bc);

	switch( instr.op )
	{
	case asBC_NEGf:
		{
			Vec x = LoadF32(a0);
			Vec r = DstF32(a0);
			m_uc.s_neg_f32(r, x);
			CommitF32(a0, r);
		}
		break;

	case asBC_NEGd:
		{
			Vec x = LoadF64(a0);
			Vec r = DstF64(a0);
			m_uc.s_neg_f64(r, x);
			CommitF64(a0, r);
		}
		break;

	case asBC_ADDf:
	case asBC_SUBf:
	case asBC_MULf:
	case asBC_DIVf:
		{
			if( instr.op == asBC_DIVf )
			{
				// Division by zero (either sign) is an exception
				Gp bits = Load32(a2);
				Gp t = m_uc.new_gp32();
				m_uc.shl(t, bits, Imm(1));
				m_uc.j(BailLabel(idx), test_z(t));
			}
			Vec a = LoadF32(a1);
			Vec b = LoadF32(a2);
			Vec r = DstF32(a0);
			switch( instr.op )
			{
			case asBC_ADDf: m_uc.s_add_f32(r, a, b); break;
			case asBC_SUBf: m_uc.s_sub_f32(r, a, b); break;
			case asBC_MULf: m_uc.s_mul_f32(r, a, b); break;
			default:        m_uc.s_div_f32(r, a, b); break;
			}
			CommitF32(a0, r);
		}
		break;

	case asBC_ADDd:
	case asBC_SUBd:
	case asBC_MULd:
	case asBC_DIVd:
		{
			if( instr.op == asBC_DIVd )
			{
				if( Is64Bit() )
				{
					Gp bits = Load64(a2);
					Gp t = m_uc.new_gp64();
					m_uc.shl(t, bits, Imm(1));
					m_uc.j(BailLabel(idx), test_z(t));
				}
				else
				{
					// The bits are tested in memory, which must hold the cached value
					StoreCachedSlot(a2);
					Gp lo = m_uc.new_gp32();
					Gp hi = m_uc.new_gp32();
					m_uc.load_u32(lo, Var(a2));
					m_uc.load_u32(hi, Var(a2, 4));
					m_uc.shl(hi, hi, Imm(1));
					m_uc.or_(hi, hi, lo);
					m_uc.j(BailLabel(idx), test_z(hi));
				}
			}
			Vec a = LoadF64(a1);
			Vec b = LoadF64(a2);
			Vec r = DstF64(a0);
			switch( instr.op )
			{
			case asBC_ADDd: m_uc.s_add_f64(r, a, b); break;
			case asBC_SUBd: m_uc.s_sub_f64(r, a, b); break;
			case asBC_MULd: m_uc.s_mul_f64(r, a, b); break;
			default:        m_uc.s_div_f64(r, a, b); break;
			}
			CommitF64(a0, r);
		}
		break;

	case asBC_MODf:
		{
			Gp bits = Load32(a2);
			Gp t = m_uc.new_gp32();
			m_uc.shl(t, bits, Imm(1));
			m_uc.j(BailLabel(idx), test_z(t));

			Vec a = LoadF32(a1);
			Vec b = LoadF32(a2);
			Vec r = m_uc.new_vec128_f32x1();
			InvokeNode *call = Invoke((const void*)JIT_MODf, FuncSignature::build<float, float, float>());
			call->set_arg(0, a);
			call->set_arg(1, b);
			call->set_ret(0, r);
			CommitF32(a0, r);
		}
		break;

	case asBC_MODd:
		{
			if( Is64Bit() )
			{
				Gp bits = Load64(a2);
				Gp t = m_uc.new_gp64();
				m_uc.shl(t, bits, Imm(1));
				m_uc.j(BailLabel(idx), test_z(t));
			}
			else
			{
				StoreCachedSlot(a2);
				Gp lo = m_uc.new_gp32();
				Gp hi = m_uc.new_gp32();
				m_uc.load_u32(lo, Var(a2));
				m_uc.load_u32(hi, Var(a2, 4));
				m_uc.shl(hi, hi, Imm(1));
				m_uc.or_(hi, hi, lo);
				m_uc.j(BailLabel(idx), test_z(hi));
			}

			Vec a = LoadF64(a1);
			Vec b = LoadF64(a2);
			Vec r = m_uc.new_vec128_f64x1();
			InvokeNode *call = Invoke((const void*)JIT_MODd, FuncSignature::build<double, double, double>());
			call->set_arg(0, a);
			call->set_arg(1, b);
			call->set_ret(0, r);
			CommitF64(a0, r);
		}
		break;

	case asBC_ADDIf:
	case asBC_SUBIf:
	case asBC_MULIf:
		{
			Vec a = LoadF32(a1);
			Vec b = m_uc.new_vec128_f32x1();
			MoveFloatImm(b, asBC_DWORDARG(bc + 1), false);
			Vec r = DstF32(a0);
			if( instr.op == asBC_ADDIf )
				m_uc.s_add_f32(r, a, b);
			else if( instr.op == asBC_SUBIf )
				m_uc.s_sub_f32(r, a, b);
			else
				m_uc.s_mul_f32(r, a, b);
			CommitF32(a0, r);
		}
		break;

	case asBC_POWf:
		{
			Vec a = LoadF32(a1);
			Vec b = LoadF32(a2);
			Gp dst = m_uc.new_gp_ptr();
			LeaVar(dst, a0);
			SyncAll(idx);
			InvokeNode *call = Invoke((const void*)JIT_POWf, FuncSignature::build<int, asSVMRegisters*, void*, float, float>());
			Gp r = m_uc.new_gp32();
			SetRegsArg(call, 0);
			call->set_arg(1, dst);
			call->set_arg(2, a);
			call->set_arg(3, b);
			call->set_ret(0, r);
			ReloadCachedSlot(a0);
			EmitLeaveIf(r);
			EmitDematerialize();
		}
		break;

	case asBC_POWd:
	case asBC_POWdi:
		{
			Vec a = LoadF64(a1);
			Gp dst = m_uc.new_gp_ptr();
			LeaVar(dst, a0);
			InvokeNode *call;
			if( instr.op == asBC_POWd )
			{
				Vec b = LoadF64(a2);
				SyncAll(idx);
				call = Invoke((const void*)JIT_POWd, FuncSignature::build<int, asSVMRegisters*, void*, double, double>());
				call->set_arg(3, b);
			}
			else
			{
				Gp b = Load32(a2);
				SyncAll(idx);
				call = Invoke((const void*)JIT_POWdi, FuncSignature::build<int, asSVMRegisters*, void*, double, int>());
				call->set_arg(3, b);
			}
			Gp r = m_uc.new_gp32();
			SetRegsArg(call, 0);
			call->set_arg(1, dst);
			call->set_arg(2, a);
			call->set_ret(0, r);
			ReloadCachedSlot(a0);
			EmitLeaveIf(r);
			EmitDematerialize();
		}
		break;

	default:
		return false;
	}

	return true;
}

//------------------------------------------------------------------------
// Increment/decrement through the pointer in the value register

bool CJITCodeGen::EmitIncDec(const SJITInstr &instr)
{
	asUINT idx = asUINT(&instr - &m_code->GetInstructions()[0]);
	Mem m = VRAddr();

	// The fields of the object held in registers are incremented there and stored
	int f = m_frame == 0 ? m_code->GetFieldAccess(idx) : -1;
	if( f >= 0 && (m_fieldGp[f].is_valid() || m_fieldVec[f].is_valid()) )
	{
		bool held = ((HeldFields(idx) >> f) & 1) != 0;
		bool inc = instr.op == asBC_INCi || instr.op == asBC_INCf;
		if( instr.op == asBC_INCi || instr.op == asBC_DECi )
		{
			Gp t = m_fieldGp[f].is_valid() ? m_fieldGp[f] : m_uc.new_gp32();
			if( !held )
				m_uc.load_u32(t, m);
			else if( !m_fieldGp[f].is_valid() )
				m_uc.s_mov_u32(t, m_fieldVec[f]);
			inc ? m_uc.add(t, t, Imm(1)) : m_uc.sub(t, t, Imm(1));
			m_uc.store_u32(m, t);
			if( m_fieldVec[f].is_valid() )
				m_uc.s_mov_u32(m_fieldVec[f], t);
		}
		else
		{
			Vec v = m_fieldVec[f].is_valid() ? m_fieldVec[f] : m_uc.new_vec128_f32x1();
			Vec one = m_uc.new_vec128_f32x1();
			Gp c = PtrConst(asPWORD(&g_oneF32));
			if( !held )
				m_uc.v_loadu32_f32(v, m);
			else if( !m_fieldVec[f].is_valid() )
				m_uc.s_mov_u32(v, m_fieldGp[f]);
			m_uc.v_loadu32_f32(one, mem_ptr(c));
			inc ? m_uc.s_add_f32(v, v, one) : m_uc.s_sub_f32(v, v, one);
			m_uc.v_storeu32_f32(m, v);
			if( m_fieldGp[f].is_valid() )
				m_uc.s_mov_u32(m_fieldGp[f], v);
		}
		return true;
	}

	switch( instr.op )
	{
	case asBC_INCi8:
	case asBC_DECi8:
	case asBC_INCi16:
	case asBC_DECi16:
	case asBC_INCi:
	case asBC_DECi:
		{
			Gp t = m_uc.new_gp32();
			bool inc = (instr.op == asBC_INCi8 || instr.op == asBC_INCi16 || instr.op == asBC_INCi);
			if( instr.op == asBC_INCi8 || instr.op == asBC_DECi8 )
			{
				m_uc.load_u8(t, m);
				inc ? m_uc.add(t, t, Imm(1)) : m_uc.sub(t, t, Imm(1));
				m_uc.store_u8(m, t);
			}
			else if( instr.op == asBC_INCi16 || instr.op == asBC_DECi16 )
			{
				m_uc.load_u16(t, m);
				inc ? m_uc.add(t, t, Imm(1)) : m_uc.sub(t, t, Imm(1));
				m_uc.store_u16(m, t);
			}
			else
			{
				m_uc.load_u32(t, m);
				inc ? m_uc.add(t, t, Imm(1)) : m_uc.sub(t, t, Imm(1));
				m_uc.store_u32(m, t);
			}
		}
		break;

	case asBC_INCi64:
	case asBC_DECi64:
		if( Is64Bit() )
		{
			Gp t = m_uc.new_gp64();
			m_uc.load_u64(t, m);
			if( instr.op == asBC_INCi64 )
				m_uc.add(t, t, Imm(1));
			else
				m_uc.sub(t, t, Imm(1));
			m_uc.store_u64(m, t);
		}
		else
		{
			Mem hiMem = m;
			hiMem.add_offset(4);
			Gp lo = m_uc.new_gp32();
			Gp hi = m_uc.new_gp32();
			Gp carry = m_uc.new_gp32();
			m_uc.load_u32(lo, m);
			m_uc.load_u32(hi, hiMem);
			if( instr.op == asBC_INCi64 )
			{
				m_uc.add(lo, lo, Imm(1));
				m_uc.select(carry, Imm(1), Imm(0), test_z(lo));
				m_uc.add(hi, hi, carry);
			}
			else
			{
				m_uc.select(carry, Imm(1), Imm(0), test_z(lo));
				m_uc.sub(lo, lo, Imm(1));
				m_uc.sub(hi, hi, carry);
			}
			m_uc.store_u32(m, lo);
			m_uc.store_u32(hiMem, hi);
		}
		break;

	case asBC_INCf:
	case asBC_DECf:
		{
			Vec v = m_uc.new_vec128_f32x1();
			Vec one = m_uc.new_vec128_f32x1();
			Gp c = PtrConst(asPWORD(&g_oneF32));
			m_uc.v_loadu32_f32(v, m);
			m_uc.v_loadu32_f32(one, mem_ptr(c));
			if( instr.op == asBC_INCf )
				m_uc.s_add_f32(v, v, one);
			else
				m_uc.s_sub_f32(v, v, one);
			m_uc.v_storeu32_f32(m, v);
		}
		break;

	case asBC_INCd:
	case asBC_DECd:
		{
			Vec v = m_uc.new_vec128_f64x1();
			Vec one = m_uc.new_vec128_f64x1();
			Gp c = PtrConst(asPWORD(&g_oneF64));
			m_uc.v_loadu64_f64(v, m);
			m_uc.v_loadu64_f64(one, mem_ptr(c));
			if( instr.op == asBC_INCd )
				m_uc.s_add_f64(v, v, one);
			else
				m_uc.s_sub_f64(v, v, one);
			m_uc.v_storeu64_f64(m, v);
		}
		break;

	default:
		return false;
	}

	return true;
}

//------------------------------------------------------------------------
// Compares
//
// The VM stores -1, 0, or 1 in the value register. Most of the time the
// result is only consumed by the following conditional jump or test
// instruction, in which case the branch/boolean is computed directly.

// dst = lt ? -1 : (gt ? 1 : 0)
void CJITCodeGen::EmitCompareResult(const Gp &dst, const Cond &lt, const Cond &gt)
{
	Gp t1 = m_uc.new_gp32();
	Gp t2 = m_uc.new_gp32();
	m_uc.select(t1, Imm(-1), Imm(0), lt);
	m_uc.select(t2, Imm(1), Imm(0), gt);
	m_uc.or_(dst, t1, t2);
}

// Floating point compare with the VM semantics: equal 0, less -1, otherwise
// (greater or unordered) 1. Computed with compare masks to stay arch neutral
void CJITCodeGen::EmitFloatCompareResult(const Gp &dst, const Vec &a, const Vec &b, bool isDouble)
{
	Vec m1 = isDouble ? m_uc.new_vec128_f64x1() : m_uc.new_vec128_f32x1();
	Vec m2 = isDouble ? m_uc.new_vec128_f64x1() : m_uc.new_vec128_f32x1();
	if( isDouble )
	{
		m_uc.s_cmp_lt_f64(m1, a, b);
		m_uc.s_cmp_le_f64(m2, a, b);
	}
	else
	{
		m_uc.s_cmp_lt_f32(m1, a, b);
		m_uc.s_cmp_le_f32(m2, a, b);
	}
	Gp t1 = m_uc.new_gp32();
	Gp t2 = m_uc.new_gp32();
	m_uc.s_mov_u32(t1, m1);   // -1 if a < b
	m_uc.s_mov_u32(t2, m2);   // -1 if a <= b
	m_uc.not_(t2, t2);
	m_uc.and_(t2, t2, Imm(1)); // 1 if greater or unordered
	m_uc.or_(dst, t1, t2);
}

// Boolean result of a test instruction applied to a floating point compare. The
// scalar not-equal compares of the UniCompiler emit an invalid mvn on AArch64, so
// the equality is inverted instead
void CJITCodeGen::EmitFloatTest(const Gp &dst, asEBCInstr test, const Vec &a, const Vec &b, bool isDouble)
{
	Vec m = isDouble ? m_uc.new_vec128_f64x1() : m_uc.new_vec128_f32x1();
	bool invert = false;
	switch( RelationOf(test) )
	{
	case REL_EQ: isDouble ? m_uc.s_cmp_eq_f64(m, a, b) : m_uc.s_cmp_eq_f32(m, a, b); break;
	case REL_NE: isDouble ? m_uc.s_cmp_eq_f64(m, a, b) : m_uc.s_cmp_eq_f32(m, a, b); invert = true; break;
	case REL_LT: isDouble ? m_uc.s_cmp_lt_f64(m, a, b) : m_uc.s_cmp_lt_f32(m, a, b); break;
	case REL_GE: isDouble ? m_uc.s_cmp_lt_f64(m, a, b) : m_uc.s_cmp_lt_f32(m, a, b); invert = true; break;
	case REL_GT: isDouble ? m_uc.s_cmp_le_f64(m, a, b) : m_uc.s_cmp_le_f32(m, a, b); invert = true; break;
	default:     isDouble ? m_uc.s_cmp_le_f64(m, a, b) : m_uc.s_cmp_le_f32(m, a, b); break;
	}
	m_uc.s_mov_u32(dst, m);
	m_uc.and_(dst, dst, Imm(1));
	if( invert )
		m_uc.xor_(dst, dst, Imm(1));
}

bool CJITCodeGen::EmitCompare(asUINT idx, asUINT &consumed)
{
	const std::vector<SJITInstr> &instrs = m_code->GetInstructions();
	const SJITInstr &instr = instrs[idx];
	const asDWORD *bc = instr.bc;
	int a0 = asBC_SWORDARG0(bc);
	int a1 = asBC_SWORDARG1(bc);
	consumed = 1;

	// The following instruction can be fused if it isn't a jump target
	const SJITInstr *next = 0;
	if( idx + 1 < instrs.size() && !(instrs[idx + 1].flags & JIT_INSTR_BLOCK_START) && !(instrs[idx + 1].flags & JIT_INSTR_BAIL) )
		next = &instrs[idx + 1];
	bool fuseJump = next && IsCondJump(next->op) && !(next->flags & JIT_INSTR_VR_LIVE);
	bool fuseTest = next && IsTest(next->op);

	bool isFloat = false, isDouble = false, isUnsigned = false, is64 = false, isPtr = false, hasImm = false;
	switch( instr.op )
	{
	case asBC_CMPi:   break;
	case asBC_CMPu:   isUnsigned = true; break;
	case asBC_CMPIi:  hasImm = true; break;
	case asBC_CMPIu:  hasImm = true; isUnsigned = true; break;
	case asBC_CMPi64: is64 = true; break;
	case asBC_CMPu64: is64 = true; isUnsigned = true; break;
	case asBC_CmpPtr: isPtr = true; isUnsigned = true; break;
	case asBC_CMPf:   isFloat = true; break;
	case asBC_CMPIf:  isFloat = true; hasImm = true; break;
	case asBC_CMPd:   isFloat = true; isDouble = true; break;
	default:
		return false;
	}

	if( isFloat )
	{
		Vec a = isDouble ? LoadF64(a0) : LoadF32(a0);
		Vec b;
		if( hasImm )
		{
			b = m_uc.new_vec128_f32x1();
			MoveFloatImm(b, asBC_DWORDARG(bc), false);
		}
		else
			b = isDouble ? LoadF64(a1) : LoadF32(a1);

		if( fuseJump )
		{
			consumed = 2;
			Label target = InstrLabel(next->target);
			if( EmitFloatCompareBranch(a, b, isDouble, next->op, target) )
				return true;

			// Generic fallback using compare masks, see EmitFloatTest
			Vec m = isDouble ? m_uc.new_vec128_f64x1() : m_uc.new_vec128_f32x1();
			bool jumpIfSet = true;
			switch( RelationOf(next->op) )
			{
			case REL_EQ: isDouble ? m_uc.s_cmp_eq_f64(m, a, b) : m_uc.s_cmp_eq_f32(m, a, b); break;
			case REL_NE: isDouble ? m_uc.s_cmp_eq_f64(m, a, b) : m_uc.s_cmp_eq_f32(m, a, b); jumpIfSet = false; break;
			case REL_LT: isDouble ? m_uc.s_cmp_lt_f64(m, a, b) : m_uc.s_cmp_lt_f32(m, a, b); break;
			case REL_GE: isDouble ? m_uc.s_cmp_lt_f64(m, a, b) : m_uc.s_cmp_lt_f32(m, a, b); jumpIfSet = false; break;
			case REL_GT: isDouble ? m_uc.s_cmp_le_f64(m, a, b) : m_uc.s_cmp_le_f32(m, a, b); jumpIfSet = false; break;
			default:     isDouble ? m_uc.s_cmp_le_f64(m, a, b) : m_uc.s_cmp_le_f32(m, a, b); break;
			}
			Gp t = m_uc.new_gp32();
			m_uc.s_mov_u32(t, m);
			if( jumpIfSet )
				m_uc.j(target, test_nz(t));
			else
				m_uc.j(target, test_z(t));
			return true;
		}

		Gp r = m_uc.new_gp32();
		if( fuseTest )
		{
			consumed = 2;
			EmitFloatTest(r, next->op, a, b, isDouble);
		}
		else
			EmitFloatCompareResult(r, a, b, isDouble);
		StoreVR32(r);
		return true;
	}

	// On 32bit hosts compare the high dwords first, signed or unsigned as requested,
	// and the low dwords as unsigned when the high ones are equal
	if( is64 && !Is64Bit() )
	{
		Gp alo = m_uc.new_gp32();
		Gp ahi = m_uc.new_gp32();
		Gp blo = m_uc.new_gp32();
		Gp bhi = m_uc.new_gp32();
		Gp loResult = m_uc.new_gp32();
		Gp hiResult = m_uc.new_gp32();
		Gp r = m_uc.new_gp32();
		m_uc.load_u32(alo, Var(a0));
		m_uc.load_u32(ahi, Var(a0, 4));
		m_uc.load_u32(blo, Var(a1));
		m_uc.load_u32(bhi, Var(a1, 4));
		EmitCompareResult(loResult, ucmp_lt(alo, blo), ucmp_gt(alo, blo));
		EmitCompareResult(hiResult, MakeCond(REL_LT, isUnsigned, ahi, bhi), MakeCond(REL_GT, isUnsigned, ahi, bhi));
		m_uc.select(r, loResult, hiResult, cmp_eq(ahi, bhi));
		if( fuseJump )
		{
			consumed = 2;
			m_uc.j(InstrLabel(next->target), MakeCond(RelationOf(next->op), false, r, Imm(0)));
		}
		else if( fuseTest )
		{
			consumed = 2;
			Gp t = m_uc.new_gp32();
			m_uc.select(t, Imm(1), Imm(0), MakeCond(RelationOf(next->op), false, r, Imm(0)));
			StoreVR32(t);
		}
		else
			StoreVR32(r);
		return true;
	}

	Gp a;
	Operand b;
	if( isPtr )
	{
		a = LoadPtr(a0);
		b = LoadPtr(a1);
	}
	else if( is64 )
	{
		a = Load64(a0);
		b = Load64(a1);
	}
	else
	{
		a = Load32(a0);
		if( hasImm )
			b = Imm(int(asBC_DWORDARG(bc)));
		else
			b = Load32(a1);
	}

	if( fuseJump )
	{
		consumed = 2;
		m_uc.j(InstrLabel(next->target), MakeCond(RelationOf(next->op), isUnsigned, a, b));
		return true;
	}

	Gp r = m_uc.new_gp32();
	if( fuseTest )
	{
		consumed = 2;
		m_uc.select(r, Imm(1), Imm(0), MakeCond(RelationOf(next->op), isUnsigned, a, b));
	}
	else
		EmitCompareResult(r, MakeCond(REL_LT, isUnsigned, a, b), MakeCond(REL_GT, isUnsigned, a, b));
	StoreVR32(r);
	return true;
}

//------------------------------------------------------------------------
// Conversions

bool CJITCodeGen::EmitConversion(asUINT idx)
{
	const SJITInstr &instr = m_code->GetInstructions()[idx];
	const asDWORD *bc = instr.bc;
	int a0 = asBC_SWORDARG0(bc);
	int a1 = asBC_SWORDARG1(bc);

	switch( instr.op )
	{
	case asBC_iTOf:
		{
			Gp x = Load32(a0);
			Vec r = DstF32(a0);
			m_uc.s_cvt_int_to_f32(r, x);
			CommitF32(a0, r);
		}
		break;

	case asBC_fTOi:
		{
			Vec x = LoadF32(a0);
			Gp r = Dst32(a0);
			m_uc.s_cvt_trunc_f32_to_int(r, x);
			Commit32(a0, r);
		}
		break;

	case asBC_uTOf:
	case asBC_uTOd:
		{
			int src = instr.op == asBC_uTOf ? a0 : a1;
			Gp x = Load32(src);
			if( Is64Bit() )
			{
				// Zero extend to 64bit and use the signed conversion
				Gp x64 = m_uc.new_gp64();
				m_uc.mov(x64.r32(), x);
				if( instr.op == asBC_uTOf )
				{
					Vec r = DstF32(a0);
					m_uc.s_cvt_int_to_f32(r, x64);
					CommitF32(a0, r);
				}
				else
				{
					Vec r = DstF64(a0);
					m_uc.s_cvt_int_to_f64(r, x64);
					CommitF64(a0, r);
				}
			}
			else if( instr.op == asBC_uTOf )
			{
				Vec r = m_uc.new_vec128_f32x1();
				InvokeNode *call = Invoke((const void*)JIT_uTOf, FuncSignature::build<float, asUINT>());
				call->set_arg(0, x);
				call->set_ret(0, r);
				CommitF32(a0, r);
			}
			else
			{
				Vec r = m_uc.new_vec128_f64x1();
				InvokeNode *call = Invoke((const void*)JIT_uTOd, FuncSignature::build<double, asUINT>());
				call->set_arg(0, x);
				call->set_ret(0, r);
				CommitF64(a0, r);
			}
		}
		break;

	case asBC_fTOu:
	case asBC_dTOu:
		{
			Vec x = instr.op == asBC_fTOu ? LoadF32(a0) : LoadF64(a1);
			if( Is64Bit() )
			{
				// Truncate to 64bit and keep the lower 32bits, which matches the
				// VM for all values in range
				Gp t = m_uc.new_gp64();
				if( instr.op == asBC_fTOu )
					m_uc.s_cvt_trunc_f32_to_int(t, x);
				else
					m_uc.s_cvt_trunc_f64_to_int(t, x);
				Commit32(a0, t.r32());
			}
			else
			{
				Gp r = m_uc.new_gp32();
				InvokeNode *call;
				if( instr.op == asBC_fTOu )
					call = Invoke((const void*)JIT_fTOu, FuncSignature::build<asUINT, float>());
				else
					call = Invoke((const void*)JIT_dTOu, FuncSignature::build<asUINT, double>());
				call->set_arg(0, x);
				call->set_ret(0, r);
				Commit32(a0, r);
			}
		}
		break;

	case asBC_sbTOi:
	case asBC_swTOi:
		{
			Gp r = Dst32(a0);
			SCachedSlot *c = FindCached(a0);
			int shift = instr.op == asBC_sbTOi ? 24 : 16;
			if( c )
			{
				Gp x = Load32(a0);
				m_uc.shl(r, x, Imm(shift));
				m_uc.sar(r, r, Imm(shift));
			}
			else if( instr.op == asBC_sbTOi )
				m_uc.load_i8(r, Var(a0));
			else
				m_uc.load_i16(r, Var(a0));
			Commit32(a0, r);
		}
		break;

	case asBC_ubTOi:
	case asBC_iTOb:
		{
			Gp x = Load32(a0);
			Gp r = Dst32(a0);
			m_uc.and_(r, x, Imm(0xFF));
			Commit32(a0, r);
		}
		break;

	case asBC_uwTOi:
	case asBC_iTOw:
		{
			Gp x = Load32(a0);
			Gp r = Dst32(a0);
			m_uc.and_(r, x, Imm(0xFFFF));
			Commit32(a0, r);
		}
		break;

	case asBC_dTOi:
		{
			Vec x = LoadF64(a1);
			Gp r = Dst32(a0);
			m_uc.s_cvt_trunc_f64_to_int(r, x);
			Commit32(a0, r);
		}
		break;

	case asBC_dTOf:
		{
			Vec x = LoadF64(a1);
			Vec r = DstF32(a0);
			m_uc.s_cvt_f64_to_f32(r, x);
			CommitF32(a0, r);
		}
		break;

	case asBC_fTOd:
		{
			Vec x = LoadF32(a1);
			Vec r = DstF64(a0);
			m_uc.s_cvt_f32_to_f64(r, x);
			CommitF64(a0, r);
		}
		break;

	case asBC_iTOd:
		{
			Gp x = Load32(a1);
			Vec r = DstF64(a0);
			m_uc.s_cvt_int_to_f64(r, x);
			CommitF64(a0, r);
		}
		break;

	case asBC_i64TOi:
		if( Is64Bit() )
		{
			Gp x = Load64(a1);
			Gp r = Dst32(a0);
			m_uc.mov(r, x.r32());
			Commit32(a0, r);
		}
		else
		{
			Gp r = Dst32(a0);
			m_uc.load_u32(r, Var(a1));
			Commit32(a0, r);
		}
		break;

	case asBC_uTOi64:
	case asBC_iTOi64:
		if( Is64Bit() )
		{
			Gp x = Load32(a1);
			Gp r = Dst64(a0);
			m_uc.mov(r.r32(), x);
			if( instr.op == asBC_iTOi64 )
			{
				m_uc.shl(r, r, Imm(32));
				m_uc.sar(r, r, Imm(32));
			}
			Commit64(a0, r);
		}
		else
		{
			Gp lo = Load32(a1);
			m_uc.store_u32(Var(a0), lo);
			if( instr.op == asBC_iTOi64 )
			{
				Gp hi = m_uc.new_gp32();
				m_uc.sar(hi, lo, Imm(31));
				m_uc.store_u32(Var(a0, 4), hi);
			}
			else
				m_uc.store_zero_u32(Var(a0, 4));
			ReloadCachedSlot(a0);
		}
		break;

	case asBC_fTOi64:
	case asBC_dTOi64:
		if( Is64Bit() )
		{
			Gp r = Dst64(a0);
			if( instr.op == asBC_fTOi64 )
				m_uc.s_cvt_trunc_f32_to_int(r, LoadF32(a1));
			else
				m_uc.s_cvt_trunc_f64_to_int(r, LoadF64(a0));
			Commit64(a0, r);
		}
		else
		{
			Vec x = instr.op == asBC_fTOi64 ? LoadF32(a1) : LoadF64(a0);
			InvokeNode *call = Invoke(instr.op == asBC_fTOi64 ? (const void*)JIT_fTOi64 : (const void*)JIT_dTOi64,
				instr.op == asBC_fTOi64 ? FuncSignature::build<asINT64, float>() : FuncSignature::build<asINT64, double>());
			Gp lo = m_uc.new_gp32();
			Gp hi = m_uc.new_gp32();
			call->set_arg(0, x);
			call->set_ret(0, lo);
			call->set_ret(1, hi);
			m_uc.store_u32(Var(a0), lo);
			m_uc.store_u32(Var(a0, 4), hi);
			ReloadCachedSlot(a0);
		}
		break;

	case asBC_fTOu64:
	case asBC_dTOu64:
		if( Is64Bit() )
		{
			Gp r = Dst64(a0);
			InvokeNode *call;
			if( instr.op == asBC_fTOu64 )
			{
				Vec x = LoadF32(a1);
				call = Invoke((const void*)JIT_fTOu64, FuncSignature::build<asQWORD, float>());
				call->set_arg(0, x);
			}
			else
			{
				Vec x = LoadF64(a0);
				call = Invoke((const void*)JIT_dTOu64, FuncSignature::build<asQWORD, double>());
				call->set_arg(0, x);
			}
			call->set_ret(0, r);
			Commit64(a0, r);
		}
		else
		{
			Vec x = instr.op == asBC_fTOu64 ? LoadF32(a1) : LoadF64(a0);
			InvokeNode *call = Invoke(instr.op == asBC_fTOu64 ? (const void*)JIT_fTOu64 : (const void*)JIT_dTOu64,
				instr.op == asBC_fTOu64 ? FuncSignature::build<asQWORD, float>() : FuncSignature::build<asQWORD, double>());
			Gp lo = m_uc.new_gp32();
			Gp hi = m_uc.new_gp32();
			call->set_arg(0, x);
			call->set_ret(0, lo);
			call->set_ret(1, hi);
			m_uc.store_u32(Var(a0), lo);
			m_uc.store_u32(Var(a0, 4), hi);
			ReloadCachedSlot(a0);
		}
		break;

	case asBC_i64TOf:
	case asBC_i64TOd:
		if( Is64Bit() )
		{
			if( instr.op == asBC_i64TOf )
			{
				Gp x = Load64(a1);
				Vec r = DstF32(a0);
				m_uc.s_cvt_int_to_f32(r, x);
				CommitF32(a0, r);
			}
			else
			{
				Gp x = Load64(a0);
				Vec r = DstF64(a0);
				m_uc.s_cvt_int_to_f64(r, x);
				CommitF64(a0, r);
			}
		}
		else
		{
			int src = instr.op == asBC_i64TOf ? a1 : a0;
			Gp lo = m_uc.new_gp32();
			Gp hi = m_uc.new_gp32();
			m_uc.load_u32(lo, Var(src));
			m_uc.load_u32(hi, Var(src, 4));
			InvokeNode *call = Invoke(instr.op == asBC_i64TOf ? (const void*)JIT_i64TOf : (const void*)JIT_i64TOd,
				instr.op == asBC_i64TOf ? FuncSignature::build<float, asINT64>() : FuncSignature::build<double, asINT64>());
			call->set_arg(0, 0, lo);
			call->set_arg(0, 1, hi);
			if( instr.op == asBC_i64TOf )
			{
				Vec r = DstF32(a0);
				call->set_ret(0, r);
				CommitF32(a0, r);
			}
			else
			{
				Vec r = DstF64(a0);
				call->set_ret(0, r);
				CommitF64(a0, r);
			}
		}
		break;

	case asBC_u64TOf:
	case asBC_u64TOd:
		if( Is64Bit() )
		{
			if( instr.op == asBC_u64TOf )
			{
				Gp x = Load64(a1);
				Vec r = DstF32(a0);
				InvokeNode *call = Invoke((const void*)JIT_u64TOf, FuncSignature::build<float, asQWORD>());
				call->set_arg(0, x);
				call->set_ret(0, r);
				CommitF32(a0, r);
			}
			else
			{
				Gp x = Load64(a0);
				Vec r = DstF64(a0);
				InvokeNode *call = Invoke((const void*)JIT_u64TOd, FuncSignature::build<double, asQWORD>());
				call->set_arg(0, x);
				call->set_ret(0, r);
				CommitF64(a0, r);
			}
		}
		else
		{
			int src = instr.op == asBC_u64TOf ? a1 : a0;
			Gp lo = m_uc.new_gp32();
			Gp hi = m_uc.new_gp32();
			m_uc.load_u32(lo, Var(src));
			m_uc.load_u32(hi, Var(src, 4));
			InvokeNode *call = Invoke(instr.op == asBC_u64TOf ? (const void*)JIT_u64TOf : (const void*)JIT_u64TOd,
				instr.op == asBC_u64TOf ? FuncSignature::build<float, asQWORD>() : FuncSignature::build<double, asQWORD>());
			call->set_arg(0, 0, lo);
			call->set_arg(0, 1, hi);
			if( instr.op == asBC_u64TOf )
			{
				Vec r = DstF32(a0);
				call->set_ret(0, r);
				CommitF32(a0, r);
			}
			else
			{
				Vec r = DstF64(a0);
				call->set_ret(0, r);
				CommitF64(a0, r);
			}
		}
		break;

	default:
		return false;
	}

	return true;
}

END_AS_NAMESPACE
