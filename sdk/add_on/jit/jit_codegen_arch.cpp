// Architecture specific code generation. Everything the UniCompiler can't
// express in an arch neutral way is isolated here, with one implementation
// per supported architecture.

#include "jit_codegen.h"
#include "jit_runtime.h"

BEGIN_AS_NAMESPACE

using namespace asmjit;
using namespace asmjit::ujit;

#if defined(ASMJIT_UJIT_X86)

// Callee-saved home registers for the registers used all through the function,
// see AssignHomeRegs. The allocator uses the scratch registers ECX|RCX and EDI|R15
// for jumps, so they are avoided. Otherwise these are only hints, so if a register
// isn't available, e.g. EBP|RBP as frame pointer, the allocator picks another one.
// The cached variables in the mask get the rest, including the ones of the stack
// pointer and the call limit if the function doesn't need them
void CJITCodeGen::SetHomeRegHints(asUINT slotMask)
{
	x86::Compiler *cc = m_uc.cc;
	uint32_t gpIds[5];
	asUINT gpCount = 0;
	cc->virt_reg_by_reg(m_regs)->set_home_id_hint(x86::Gp::kIdBx);
	if( m_sp.is_valid() )
		cc->virt_reg_by_reg(m_sp)->set_home_id_hint(x86::Gp::kIdBp);
	if( Is64Bit() )
	{
		// RSI and RDI aren't callee-saved on System V, and neither are XMM6-15
		cc->virt_reg_by_reg(m_fp)->set_home_id_hint(x86::Gp::kIdR14);
		if( m_callLimit.is_valid() )
			cc->virt_reg_by_reg(m_callLimit)->set_home_id_hint(x86::Gp::kIdR12);

		static const uint32_t vecIds[] = { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
		gpIds[gpCount++] = x86::Gp::kIdSi;
		gpIds[gpCount++] = x86::Gp::kIdDi;
		gpIds[gpCount++] = x86::Gp::kIdR13;
		if( !m_sp.is_valid() )
			gpIds[gpCount++] = x86::Gp::kIdBp;
		if( !m_callLimit.is_valid() )
			gpIds[gpCount++] = x86::Gp::kIdR12;
		SetSlotHomeHints(slotMask, gpIds, gpCount, vecIds, 10);
	}
	else
	{
		cc->virt_reg_by_reg(m_fp)->set_home_id_hint(x86::Gp::kIdSi);
		if( !m_sp.is_valid() )
			gpIds[gpCount++] = x86::Gp::kIdBp;
		SetSlotHomeHints(slotMask, gpIds, gpCount, 0, 0);
	}
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

// The element of an array of pointers, which x86 addresses with the scaled index
// directly, also with the displacements added to the result
Mem CJITCodeGen::PtrElement(const Gp &array, const Gp &index)
{
	return mem_ptr(array, index, Is64Bit() ? 3 : 2);
}

// x86 takes any 32bit displacement
Mem CJITCodeGen::Addr(const Gp &base, int32_t disp)
{
	return mem_ptr(base, disp);
}

void CJITCodeGen::Lea(const Gp &dst, const Mem &src)
{
	m_uc.lea(dst, src);
}

// AsmJit calls the functions out of the reach of a relative call through its address table
InvokeNode *CJITCodeGen::Invoke(const void *fn, const FuncSignature &sig)
{
	InvokeNode *node = 0;
	m_uc.cc->invoke(Out(node), Imm(int64_t(asPWORD(fn))), sig);
	return node;
}

// The sign bit of a 64bit register isn't an immediate that OR can take
void CJITCodeGen::SetSignBit(const Gp &r)
{
	m_uc.cc->bts(r, Imm(r.size() * 8 - 1));
}

// The second return register, which a 64bit value in two registers would use
void CJITCodeGen::AddVRReturn(FuncDetail &detail)
{
	detail.ret(1).init_reg(RegType::kGp64, x86::Gp::kIdDx, TypeId::kUInt64);
}

// x86 stores an immediate without a register
void CJITCodeGen::StoreImm32(const Mem &dst, int value)
{
	x86::Mem m(dst);
	m.set_size(4);
	m_uc.cc->mov(m, Imm(value));
}

void CJITCodeGen::MoveVec(const Vec &dst, const Vec &src)
{
	m_uc.v_mov(dst, src);
}

// The reference counts of the script objects, see CJITByteCode::FindInPlaceRefCounts.
// Like asCScriptObject::AddRef the flag of the GC is cleared. The objects that are
// being destroyed are left to AddRef, which reports the error
void CJITCodeGen::EmitAddRefInPlace(const Gp &obj, const Label &slow)
{
	x86::Compiler *cc = m_uc.cc;
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	x86::Mem dead = mem_ptr(obj, layout.deadFlag);
	dead.set_size(1);
	cc->test(dead, Imm(layout.deadFlagMask));
	cc->jnz(slow);
	x86::Mem gc = mem_ptr(obj, layout.gcFlag);
	gc.set_size(1);
	cc->and_(gc, Imm(int8_t(~layout.gcFlagMask)));
	EmitRefCountInc(obj);
}

// Like asCScriptObject::Release the flag of the GC is cleared. The last reference
// is left to Release, which destroys the object. With atomic reference counts
// another thread may release a reference between the check and the decrement,
// which jumps to race with the count at 0. Returns whether the code may jump there
bool CJITCodeGen::EmitReleaseInPlace(const Gp &obj, const Label &slow, const Label &race)
{
	x86::Compiler *cc = m_uc.cc;
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	x86::Mem gc = mem_ptr(obj, layout.gcFlag);
	gc.set_size(1);
	cc->and_(gc, Imm(int8_t(~layout.gcFlagMask)));
	x86::Mem count = mem_ptr(obj, layout.refCount);
	count.set_size(4);
	cc->cmp(count, Imm(1));
	cc->jbe(slow);
	if( layout.atomicRefCount )
	{
		cc->lock().dec(count);
		cc->jz(race);
		return true;
	}
	cc->dec(count);
	return false;
}

void CJITCodeGen::EmitRefCountInc(const Gp &obj)
{
	x86::Compiler *cc = m_uc.cc;
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	x86::Mem count = mem_ptr(obj, layout.refCount);
	count.set_size(4);
	if( layout.atomicRefCount )
		cc->lock().inc(count);
	else
		cc->inc(count);
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
// registers are X27 and X28. Only the lower halves of V8-V15 are preserved, which
// doesn't suffice for the 128bit registers of cached float variables
void CJITCodeGen::SetHomeRegHints(asUINT slotMask)
{
	a64::Compiler *cc = m_uc.cc;
	uint32_t gpIds[6] = { 23, 24, 25, 26 };
	asUINT gpCount = 4;
	cc->virt_reg_by_reg(m_regs)->set_home_id_hint(19);
	cc->virt_reg_by_reg(m_fp)->set_home_id_hint(20);
	if( m_sp.is_valid() )
		cc->virt_reg_by_reg(m_sp)->set_home_id_hint(21);
	else
		gpIds[gpCount++] = 21;
	if( m_callLimit.is_valid() )
		cc->virt_reg_by_reg(m_callLimit)->set_home_id_hint(22);
	else
		gpIds[gpCount++] = 22;
	SetSlotHomeHints(slotMask, gpIds, gpCount, 0, 0);
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

// A scaled index can't be combined with a displacement, so the address is computed
Mem CJITCodeGen::PtrElement(const Gp &array, const Gp &index)
{
	Gp p = m_uc.new_gp_ptr();
	m_uc.add_ext(p, array, index, AS_PTR_SIZE * 4);
	return mem_ptr(p);
}

// The loads and stores take the displacements from -256 to 255, and the multiples
// of the size of the access up to 4095 times it, which the multiples of 8 below 4096
// are for any size. The others are added to the base first
Mem CJITCodeGen::Addr(const Gp &base, int32_t disp)
{
	if( (disp >= -256 && disp <= 255) || (disp >= 0 && disp < 4096 && (disp & 7) == 0) )
		return mem_ptr(base, disp);
	Gp p = m_uc.new_gp_ptr();
	m_uc.add(p, base, Imm(disp));
	return mem_ptr(p);
}

// The add of lea only takes 12bit unsigned immediates, unlike that of UniCompiler
void CJITCodeGen::Lea(const Gp &dst, const Mem &src)
{
	if( src.has_index() )
		m_uc.lea(dst, src);
	else
		m_uc.add(dst, src.base_reg().as<Gp>(), Imm(src.offset_lo32()));
}

// bl only reaches 128MB, and the code is usually further away from the functions it
// calls, so they are called through a register
InvokeNode *CJITCodeGen::Invoke(const void *fn, const FuncSignature &sig)
{
	InvokeNode *node = 0;
	m_uc.cc->invoke(Out(node), PtrConst(asPWORD(fn)), sig);
	return node;
}

void CJITCodeGen::SetSignBit(const Gp &r)
{
	m_uc.cc->orr(r, r, Imm(uint64_t(1) << (r.size() * 8 - 1)));
}

void CJITCodeGen::AddVRReturn(FuncDetail &detail)
{
	detail.ret(1).init_reg(RegType::kGp64, 1, TypeId::kUInt64);
}

// Zero is stored from the zero register
void CJITCodeGen::StoreImm32(const Mem &dst, int value)
{
	if( value == 0 )
	{
		m_uc.store_zero_u32(dst);
		return;
	}
	Gp t = m_uc.new_gp32();
	m_uc.mov(t, Imm(value));
	m_uc.store_u32(dst, t);
}

// The whole register is moved even for a scalar. Apple's cores rename the 128bit
// move, but take cycles for the 64bit one that the UniCompiler emits for a scalar,
// which slows down the loops that keep floats in registers
void CJITCodeGen::MoveVec(const Vec &dst, const Vec &src)
{
	m_uc.cc->mov(dst.as<a64::Vec>().b16(), src.as<a64::Vec>().b16());
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

// The reference counts of the script objects, see the x86 version. The flag of the
// GC and the one of the objects being destroyed are loaded from the same byte if
// they share it. The atomic operations use the LSE instructions if the CPU has
// them, else loops of exclusive loads and stores. AsmJit notes the first operand of
// ldadd and the like as written, which they read, so stadd and cas are used
static bool HasLSE(a64::Compiler *cc)
{
	return cc->code()->cpu_features().arm().has_lse();
}

void CJITCodeGen::EmitAddRefInPlace(const Gp &obj, const Label &slow)
{
	a64::Compiler *cc = m_uc.cc;
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	Gp flags = m_uc.new_gp32();
	cc->ldrb(flags, Addr(obj, layout.deadFlag));
	cc->tst(flags, Imm(layout.deadFlagMask));
	cc->b_ne(slow);
	if( layout.gcFlag != layout.deadFlag )
		cc->ldrb(flags, Addr(obj, layout.gcFlag));
	cc->and_(flags, flags, Imm(~uint32_t(layout.gcFlagMask)));
	cc->strb(flags, Addr(obj, layout.gcFlag));
	EmitRefCountInc(obj);
}

// The count is compared with 1 by the same atomic operation that decrements it,
// so the release never jumps to race
bool CJITCodeGen::EmitReleaseInPlace(const Gp &obj, const Label &slow, const Label &)
{
	a64::Compiler *cc = m_uc.cc;
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	Gp flags = m_uc.new_gp32();
	cc->ldrb(flags, Addr(obj, layout.gcFlag));
	cc->and_(flags, flags, Imm(~uint32_t(layout.gcFlagMask)));
	cc->strb(flags, Addr(obj, layout.gcFlag));
	Gp count = m_uc.new_gp32();
	Gp next = m_uc.new_gp32();
	if( !layout.atomicRefCount )
	{
		cc->ldr(count, Addr(obj, layout.refCount));
		cc->cmp(count, Imm(1));
		cc->b_ls(slow);
		cc->sub(next, count, Imm(1));
		cc->str(next, Addr(obj, layout.refCount));
		return false;
	}

	Gp addr = m_uc.new_gp_ptr();
	Label loop = m_uc.new_label();
	m_uc.add(addr, obj, Imm(layout.refCount));
	if( HasLSE(cc) )
	{
		// cas leaves the count it has seen in the register, and stores the next only
		// if that is the one compared
		Gp seen = m_uc.new_gp32();
		cc->ldr(count, mem_ptr(addr));
		m_uc.bind(loop);
		cc->cmp(count, Imm(1));
		cc->b_ls(slow);
		cc->sub(next, count, Imm(1));
		cc->mov(seen, count);
		cc->casal(seen, next, mem_ptr(addr));
		cc->cmp(seen, count);
		cc->mov(count, seen);
		cc->b_ne(loop);
	}
	else
	{
		Gp failed = m_uc.new_gp32();
		m_uc.bind(loop);
		cc->ldaxr(count, mem_ptr(addr));
		cc->cmp(count, Imm(1));
		cc->b_ls(slow);
		cc->sub(next, count, Imm(1));
		cc->stlxr(failed, next, mem_ptr(addr));
		cc->cbnz(failed, loop);
	}
	return false;
}

void CJITCodeGen::EmitRefCountInc(const Gp &obj)
{
	a64::Compiler *cc = m_uc.cc;
	const SJITObjectLayout &layout = JIT_GetObjectLayout();
	Gp count = m_uc.new_gp32();
	if( !layout.atomicRefCount )
	{
		cc->ldr(count, Addr(obj, layout.refCount));
		cc->add(count, count, Imm(1));
		cc->str(count, Addr(obj, layout.refCount));
		return;
	}

	Gp addr = m_uc.new_gp_ptr();
	m_uc.add(addr, obj, Imm(layout.refCount));
	if( HasLSE(cc) )
	{
		m_uc.mov(count, Imm(1));
		cc->staddl(count, mem_ptr(addr));
	}
	else
	{
		Gp failed = m_uc.new_gp32();
		Label loop = m_uc.new_label();
		m_uc.bind(loop);
		cc->ldaxr(count, mem_ptr(addr));
		cc->add(count, count, Imm(1));
		cc->stlxr(failed, count, mem_ptr(addr));
		cc->cbnz(failed, loop);
	}
}

#else

void CJITCodeGen::SetHomeRegHints(asUINT)
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

Mem CJITCodeGen::PtrElement(const Gp &array, const Gp &index)
{
	Gp p = m_uc.new_gp_ptr();
	m_uc.add_ext(p, array, index, AS_PTR_SIZE * 4);
	return mem_ptr(p);
}

Mem CJITCodeGen::Addr(const Gp &base, int32_t disp)
{
	return mem_ptr(base, disp);
}

void CJITCodeGen::Lea(const Gp &dst, const Mem &src)
{
	m_uc.lea(dst, src);
}

InvokeNode *CJITCodeGen::Invoke(const void *fn, const FuncSignature &sig)
{
	InvokeNode *node = 0;
	m_uc.cc->invoke(Out(node), Imm(int64_t(asPWORD(fn))), sig);
	return node;
}

void CJITCodeGen::SetSignBit(const Gp &)
{
	m_failed = true;
}

void CJITCodeGen::AddVRReturn(FuncDetail &)
{
	m_failed = true;
}

void CJITCodeGen::StoreImm32(const Mem &dst, int value)
{
	Gp t = m_uc.new_gp32();
	m_uc.mov(t, Imm(value));
	m_uc.store_u32(dst, t);
}

void CJITCodeGen::MoveVec(const Vec &dst, const Vec &src)
{
	m_uc.v_mov(dst, src);
}

#endif

#if !defined(ASMJIT_UJIT_X86) && !defined(ASMJIT_UJIT_AARCH64)

// The references are only counted in place on x86 and AArch64, see JIT_INPLACE_REFCOUNT
void CJITCodeGen::EmitAddRefInPlace(const Gp &, const Label &)
{
	m_failed = true;
}

bool CJITCodeGen::EmitReleaseInPlace(const Gp &, const Label &, const Label &)
{
	m_failed = true;
	return false;
}

void CJITCodeGen::EmitRefCountInc(const Gp &)
{
	m_failed = true;
}

#endif

END_AS_NAMESPACE
