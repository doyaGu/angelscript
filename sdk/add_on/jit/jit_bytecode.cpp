#include "jit_bytecode.h"

// Internal engine headers. The JIT must be compiled with the same
// configuration as the engine library (see CMakeLists.txt)
#include "as_scriptfunction.h"
#include "as_scriptengine.h"
#include "as_module.h"

#include <algorithm>

BEGIN_AS_NAMESPACE

CJITByteCode::CJITByteCode()
{
	m_func     = 0;
	m_byteCode = 0;
	m_length   = 0;
	m_retReadsVR = false;
	m_tempMask   = 0;
	m_staticStack = false;
}

bool CJITByteCode::IsBranch(asEBCInstr op)
{
	switch( op )
	{
	case asBC_JMP:
	case asBC_JZ:
	case asBC_JNZ:
	case asBC_JS:
	case asBC_JNS:
	case asBC_JP:
	case asBC_JNP:
	case asBC_JLowZ:
	case asBC_JLowNZ:
		return true;
	default:
		return false;
	}
}

bool CJITByteCode::IsTerminator(asEBCInstr op)
{
	return IsBranch(op) || op == asBC_JMPP || op == asBC_RET;
}

bool CJITByteCode::ReadsVR(asEBCInstr op)
{
	switch( op )
	{
	case asBC_JZ:
	case asBC_JNZ:
	case asBC_JS:
	case asBC_JNS:
	case asBC_JP:
	case asBC_JNP:
	case asBC_JLowZ:
	case asBC_JLowNZ:
	case asBC_TZ:
	case asBC_TNZ:
	case asBC_TS:
	case asBC_TNS:
	case asBC_TP:
	case asBC_TNP:
	case asBC_CpyRtoV4:
	case asBC_CpyRtoV8:
	case asBC_PshRPtr:
	case asBC_INCi16:
	case asBC_INCi8:
	case asBC_DECi16:
	case asBC_DECi8:
	case asBC_INCi:
	case asBC_DECi:
	case asBC_INCf:
	case asBC_DECf:
	case asBC_INCd:
	case asBC_DECd:
	case asBC_INCi64:
	case asBC_DECi64:
	case asBC_WRTV1:
	case asBC_WRTV2:
	case asBC_WRTV4:
	case asBC_WRTV8:
	case asBC_RDR1:
	case asBC_RDR2:
	case asBC_RDR4:
	case asBC_RDR8:
	case asBC_ClrHi:
	case asBC_RET:     // primitive and reference return values, see AnalyseVRLiveness
		return true;
	default:
		return false;
	}
}

bool CJITByteCode::WritesVR(asEBCInstr op)
{
	switch( op )
	{
	case asBC_TZ:
	case asBC_TNZ:
	case asBC_TS:
	case asBC_TNS:
	case asBC_TP:
	case asBC_TNP:
	case asBC_CMPd:
	case asBC_CMPu:
	case asBC_CMPf:
	case asBC_CMPi:
	case asBC_CMPIi:
	case asBC_CMPIf:
	case asBC_CMPIu:
	case asBC_CMPi64:
	case asBC_CMPu64:
	case asBC_CmpPtr:
	case asBC_CpyVtoR4:
	case asBC_CpyVtoR8:
	case asBC_LdGRdR4:
	case asBC_PopRPtr:
	case asBC_LDG:
	case asBC_LDV:
	case asBC_LoadThisR:
	case asBC_LoadRObjR:
	case asBC_LoadVObjR:
	case asBC_ClrHi:
	case asBC_CALL:      // the called function may return a value in the register
	case asBC_CALLSYS:
	case asBC_CALLBND:
	case asBC_CALLINTF:
	case asBC_CallPtr:
	case asBC_ALLOC:
	case asBC_Thiscall1:
		return true;
	default:
		return false;
	}
}

int CJITByteCode::Decode(asCScriptFunction *func)
{
	m_func = func;
	m_instrs.clear();
	m_posToInstr.clear();
	m_blocks.clear();
	m_entries.clear();
	m_slots.clear();
	m_slotIndex.clear();
	m_switchTargets.clear();

	m_byteCode = func->GetByteCode(&m_length);
	if( m_byteCode == 0 || m_length == 0 )
		return asERROR;

	m_posToInstr.assign(m_length + 1, -1);

	// First pass: split the bytecode into instructions
	asUINT pos = 0;
	while( pos < m_length )
	{
		asEBCInstr op = asEBCInstr(*(asBYTE*)(m_byteCode + pos));
		if( op >= asBC_MAXBYTECODE )
			return asERROR;

		asUINT size = asBCTypeSize[asBCInfo[op].type];
		if( size == 0 || pos + size > m_length )
			return asERROR;

		SJITInstr instr;
		instr.bc     = m_byteCode + pos;
		instr.pos    = pos;
		instr.op     = op;
		instr.size   = size;
		instr.target = -1;
		instr.block  = -1;
		instr.flags  = 0;

		m_posToInstr[pos] = int(m_instrs.size());
		m_instrs.push_back(instr);
		pos += size;
	}

	// Second pass: resolve the branch targets, switch tables, and entry points
	bool leading = true;
	int entryLine = func->GetLineNumber(0, 0) & 0xFFFFF;
	std::vector<bool> isTarget(m_instrs.size(), false);
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		SJITInstr &instr = m_instrs[n];

		// The compiler removes the SUSPEND for the first line of a function as the
		// line callback is already invoked when the function is entered. With JIT
		// instructions enabled a JitEntry in between stops it from doing so when
		// the line has more than one statement, which would invoke the callback
		// twice for the same line. Such leading SUSPENDs are skipped here
		if( instr.op == asBC_SUSPEND && leading && (func->GetLineNumber(int(instr.pos), 0) & 0xFFFFF) == entryLine )
			instr.flags |= JIT_INSTR_SKIP;
		else if( instr.op != asBC_JitEntry )
			leading = false;
		if( IsBranch(instr.op) )
		{
			int targetPos = int(instr.pos) + 2 + asBC_INTARG(instr.bc);
			if( targetPos < 0 || targetPos > int(m_length) )
				return asERROR;
			instr.target = m_posToInstr[targetPos];
			if( instr.target < 0 )
				return asERROR;
			m_instrs[instr.target].flags |= JIT_INSTR_BLOCK_START;
			isTarget[instr.target] = true;
		}
		else if( instr.op == asBC_JMPP )
		{
			// The JMPP instruction is followed by a table of JMP instructions, one
			// for each case. The number of cases isn't stored, so the table is
			// assumed to span all consecutive JMP instructions that follow. Over
			// counting is harmless as the compiler guarantees the index is in range
			std::vector<int> &targets = m_switchTargets[n];
			asUINT m = n + 1;
			while( m < m_instrs.size() && m_instrs[m].op == asBC_JMP )
			{
				int targetPos = int(m_instrs[m].pos) + 2 + asBC_INTARG(m_instrs[m].bc);
				if( targetPos < 0 || targetPos > int(m_length) || m_posToInstr[targetPos] < 0 )
					return asERROR;
				targets.push_back(m_posToInstr[targetPos]);
				m_instrs[m_posToInstr[targetPos]].flags |= JIT_INSTR_BLOCK_START;
				isTarget[m_posToInstr[targetPos]] = true;
				m++;
			}
			if( targets.empty() )
				return asERROR;
		}
		else if( instr.op == asBC_JitEntry )
		{
			instr.flags |= JIT_INSTR_ENTRY | JIT_INSTR_BLOCK_START;
			m_entries.push_back(n);
		}

		if( n == 0 || IsTerminator(m_instrs[n-1].op) )
			instr.flags |= JIT_INSTR_BLOCK_START;
	}

	// A loop on the first line may start at one of the leading SUSPENDs, e.g.
	// 'do {} while( cond );', in which case the SUSPENDs from the loop head on
	// are its only suspend points and must be kept
	bool inLoop = false;
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		SJITInstr &instr = m_instrs[n];
		if( instr.op != asBC_JitEntry && !(instr.flags & JIT_INSTR_SKIP) )
			break;
		if( isTarget[n] )
			inLoop = true;
		if( inLoop )
			instr.flags &= ~JIT_INSTR_SKIP;
	}

	// Without entry points the function can never be entered natively, e.g.
	// when the bytecode was compiled without JIT instructions
	if( m_entries.empty() )
		return asNOT_SUPPORTED;

	MarkUnreachable();

	return asSUCCESS;
}

// Marks the instructions that can never be executed, e.g. the jump tables of
// switch statements that are only reached through JMPP, so that no code is
// generated for them
void CJITByteCode::MarkUnreachable()
{
	// Native callers enter at the start of the function, see CJITCodeGen::EmitDirectEntry
	std::vector<bool> reached(m_instrs.size(), false);
	std::vector<asUINT> work(m_entries.begin(), m_entries.end());
	work.push_back(0);

	while( !work.empty() )
	{
		asUINT n = work.back();
		work.pop_back();
		if( reached[n] )
			continue;
		reached[n] = true;

		const SJITInstr &instr = m_instrs[n];
		if( instr.op == asBC_JMPP )
		{
			const std::vector<int> &targets = GetSwitchTargets(n);
			for( asUINT t = 0; t < targets.size(); t++ )
				work.push_back(targets[t]);
		}
		else if( instr.op != asBC_RET )
		{
			if( instr.target >= 0 )
				work.push_back(instr.target);
			if( instr.op != asBC_JMP && n + 1 < m_instrs.size() )
				work.push_back(n + 1);
		}
	}

	for( asUINT n = 0; n < m_instrs.size(); n++ )
		if( !reached[n] )
			m_instrs[n].flags |= JIT_INSTR_DEAD;
}

int CJITByteCode::FindInstruction(asUINT pos) const
{
	if( pos >= m_posToInstr.size() )
		return -1;
	return m_posToInstr[pos];
}

const std::vector<int> &CJITByteCode::GetSwitchTargets(asUINT instrIdx) const
{
	std::map<asUINT, std::vector<int> >::const_iterator it = m_switchTargets.find(instrIdx);
	if( it == m_switchTargets.end() )
		return m_noTargets;
	return it->second;
}

void CJITByteCode::SetBailInstructions(const bool bail[asBC_MAXBYTECODE])
{
	for( asUINT n = 0; n < m_instrs.size(); n++ )
		if( bail[m_instrs[n].op] )
			m_instrs[n].flags |= JIT_INSTR_BAIL;
	for( std::map<asUINT, std::shared_ptr<CJITByteCode> >::iterator it = m_inlinees.begin(); it != m_inlinees.end(); ++it )
		it->second->SetBailInstructions(bail);
}

const CJITByteCode *CJITByteCode::GetInlinee(asUINT instrIdx) const
{
	std::map<asUINT, std::shared_ptr<CJITByteCode> >::const_iterator it = m_inlinees.find(instrIdx);
	return it == m_inlinees.end() ? 0 : it->second.get();
}

asCObjectType *CJITByteCode::GetInlineObjectType(asUINT instrIdx) const
{
	std::map<asUINT, asCObjectType*>::const_iterator it = m_inlineObjTypes.find(instrIdx);
	return it == m_inlineObjTypes.end() ? 0 : it->second;
}

int CJITByteCode::GetPopSize(asCScriptFunction *func)
{
	return func->GetSpaceNeededForArguments() + (func->objectType ? AS_PTR_SIZE : 0) + (func->DoesReturnOnStack() ? AS_PTR_SIZE : 0);
}

// The funcdef of the function pointer in the variable that asBC_CallPtr calls, found
// like asCScriptFunction::GetCalledFunction does, or 0. The compiler only lets
// variables of the same type share a slot
static asCScriptFunction *FindFuncdef(asCScriptFunction *func, int var)
{
	const asCArray<asSScriptVariable*> &vars = func->scriptData->variables;
	for( asUINT n = 0; n < vars.GetLength(); n++ )
		if( vars[n]->stackOffset == var )
		{
			asCFuncdefType *type = CastToFuncdefType(vars[n]->type.GetTypeInfo());
			return type ? type->funcdef : 0;
		}

	int offset = -(func->objectType ? AS_PTR_SIZE : 0) - (func->DoesReturnOnStack() ? AS_PTR_SIZE : 0);
	for( asUINT n = 0; n < func->parameterTypes.GetLength(); n++ )
	{
		if( offset == var )
		{
			asCFuncdefType *type = CastToFuncdefType(func->parameterTypes[n].GetTypeInfo());
			return type ? type->funcdef : 0;
		}
		offset -= func->parameterTypes[n].GetSizeOnStackDWords();
	}
	return 0;
}

// The change of the stack depth in dwords by the instruction, as the compiler
// computes it. Returns false if it isn't known, e.g. for the calls of variadic
// functions, whose number of arguments varies
bool CJITByteCode::GetStackInc(const SJITInstr &instr, int &inc) const
{
	asCScriptEngine *engine = m_func->engine;
	asCScriptFunction *callee = 0;
	int id;
	switch( instr.op )
	{
	case asBC_CALL:
	case asBC_CALLINTF:
	case asBC_CALLSYS:
		id = asBC_INTARG(instr.bc);
		if( id > 0 && asUINT(id) < engine->scriptFunctions.GetLength() )
			callee = engine->scriptFunctions[id];
		break;

	case asBC_CALLBND:
		id = asBC_INTARG(instr.bc) & ~FUNC_IMPORTED;
		if( id >= 0 && asUINT(id) < engine->importedFunctions.GetLength() && engine->importedFunctions[id] )
			callee = engine->importedFunctions[id]->importedFunctionSignature;
		break;

	case asBC_CallPtr:
		callee = FindFuncdef(m_func, asBC_SWORDARG1(instr.bc));
		break;

	case asBC_ALLOC:
		// The constructor pops the arguments and the object, which the instruction
		// pushes, and the instruction pops the address of the variable to store the
		// object in, except for script objects, whose factory stubs return right
		// after. For templates the type is one of the arguments
		id = asBC_INTARG(instr.bc + AS_PTR_SIZE);
		if( id == 0 )
		{
			inc = -AS_PTR_SIZE;
			return true;
		}
		if( id > 0 && asUINT(id) < engine->scriptFunctions.GetLength() )
			callee = engine->scriptFunctions[id];
		if( callee && !callee->IsVariadic() && (((asCObjectType*)asBC_PTRARG(instr.bc))->flags & asOBJ_SCRIPT_OBJECT) )
		{
			inc = -GetPopSize(callee) + AS_PTR_SIZE;
			return true;
		}
		break;

	default:
		inc = asBCInfo[instr.op].stackInc;
		return inc != 0xFFFF;
	}

	if( callee == 0 || callee->IsVariadic() )
		return false;
	inc = -GetPopSize(callee);
	return true;
}

// Computes the depth of the stack when each instruction is reached, which the
// compiler makes the same on all paths, see asCByteCode::PostProcess. The catch
// blocks start with the depth that the context sets up, see asCContext::CleanStackFrame
void CJITByteCode::AnalyseStackDepth()
{
	m_staticStack = false;
	m_stackDepth.assign(m_instrs.size(), -1);

	std::vector<asUINT> work;
	m_stackDepth[0] = 0;
	work.push_back(0);
	const asCArray<asSTryCatchInfo> &catches = m_func->scriptData->tryCatchInfo;
	for( asUINT n = 0; n < catches.GetLength(); n++ )
	{
		int idx = FindInstruction(catches[n].catchPos);
		int depth = int(catches[n].stackSize);
		if( idx < 0 || (m_stackDepth[idx] >= 0 && m_stackDepth[idx] != depth) )
			return;
		m_stackDepth[idx] = depth;
		work.push_back(asUINT(idx));
	}

	std::vector<int> succ;
	while( !work.empty() )
	{
		asUINT n = work.back();
		work.pop_back();
		const SJITInstr &instr = m_instrs[n];
		if( instr.op == asBC_RET )
			continue;

		int inc;
		if( !GetStackInc(instr, inc) || m_stackDepth[n] + inc < 0 )
			return;
		int depth = m_stackDepth[n] + inc;

		succ.clear();
		if( instr.op == asBC_JMPP )
			succ = GetSwitchTargets(n);
		else
		{
			if( instr.target >= 0 )
				succ.push_back(instr.target);
			if( instr.op != asBC_JMP && n + 1 < m_instrs.size() )
				succ.push_back(int(n + 1));
		}
		for( asUINT s = 0; s < succ.size(); s++ )
		{
			if( m_stackDepth[succ[s]] < 0 )
			{
				m_stackDepth[succ[s]] = depth;
				work.push_back(asUINT(succ[s]));
			}
			else if( m_stackDepth[succ[s]] != depth )
				return;
		}
	}

	// Everything that gets code must have been reached
	for( asUINT n = 0; n < m_instrs.size(); n++ )
		if( m_stackDepth[n] < 0 && !(m_instrs[n].flags & JIT_INSTR_DEAD) )
			return;

	m_staticStack = true;
}

// The code of an inlined function runs with the frame of the caller in the VM
// registers, so it must not call anything that could see them. The instructions
// working on objects call functions on some paths too
bool CJITByteCode::CanBeInlined() const
{
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		if( m_instrs[n].flags & JIT_INSTR_DEAD )
			continue;
		switch( m_instrs[n].op )
		{
		case asBC_FREE:
		case asBC_LOADOBJ:
		case asBC_STOREOBJ:
		case asBC_Cast:
		case asBC_STR:
		case asBC_AllocMem:
		case asBC_SetListSize:
		case asBC_SetListType:
		case asBC_PshListElmnt:
			return false;
		default:
			if( IsSyncPoint(m_instrs[n].op) )
				return false;
		}
	}
	return true;
}

// Returns the implementation of a virtual or interface method in the only class of
// the module of the caller that objects calling it can be of, or null if there are
// more classes. The classes of other modules may derive from the shared classes and
// implement the shared interfaces, so the class of the object must still be checked
static asCScriptFunction *FindOnlyImplementation(asCScriptFunction *caller, asCScriptFunction *method, asCObjectType *&objType)
{
	objType = 0;
	asCObjectType *type = method->objectType;
	if( caller->module == 0 || type == 0 || method->vfTableIdx < 0 )
		return 0;

	asCObjectType *found = 0;
	const asCArray<asCObjectType*> &classes = caller->module->m_classTypes;
	for( asUINT n = 0; n < classes.GetLength(); n++ )
	{
		asCObjectType *cls = classes[n];
		if( !(cls->flags & asOBJ_SCRIPT_OBJECT) || (cls->flags & asOBJ_ABSTRACT) || cls->IsInterface() )
			continue;
		if( method->funcType == asFUNC_INTERFACE ? !cls->Implements(type) : !cls->DerivesFrom(type) )
			continue;
		if( found )
			return 0;
		found = cls;
	}
	if( found == 0 )
		return 0;

	// Like asCContext::CallInterfaceMethod
	asUINT index = asUINT(method->vfTableIdx);
	if( method->funcType == asFUNC_INTERFACE )
	{
		asUINT n = 0;
		while( n < found->interfaces.GetLength() && found->interfaces[n] != type )
			n++;
		if( n == found->interfaces.GetLength() )
			return 0;
		index += found->interfaceVFTOffsets[n];
	}
	if( index >= found->virtualFunctionTable.GetLength() )
		return 0;
	objType = found;
	return found->virtualFunctionTable[index];
}

// Finds the calls whose function can be emitted in place, which is analysed on its
// own then. Its frame starts at the stack pointer of the call, so the depth of the
// stack must be known. The inlined code is limited to a multiple of the size of the
// largest function, so that the functions calling many don't grow without bounds.
// Recursion and functions with catch blocks are left to the calls. The virtual and
// interface methods are inlined for the only class that can implement them, which
// the object is checked for
void CJITByteCode::FindInlinees(bool allowRegisterCache, asUINT maxCachedSlots, const SJITInlineOptions &inlining)
{
	m_inlinees.clear();
	m_inlineObjTypes.clear();
	if( inlining.maxSize == 0 || !m_staticStack )
		return;

	asCScriptEngine *engine = m_func->engine;
	std::map<int, std::shared_ptr<CJITByteCode> > analysed; // by function id, null if it can't be inlined
	asUINT budget = inlining.maxSize * 16;
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		SJITInstr &instr = m_instrs[n];
		if( (instr.op != asBC_CALL && instr.op != asBC_CALLINTF) || (instr.flags & JIT_INSTR_DEAD) )
			continue;

		asCScriptFunction *func = 0;
		asCObjectType *objType = 0;
		int id = asBC_INTARG(instr.bc);
		if( id >= 0 && asUINT(id) < engine->scriptFunctions.GetLength() )
			func = engine->scriptFunctions[id];
		if( func && instr.op == asBC_CALLINTF )
		{
			if( func->funcType == asFUNC_VIRTUAL || func->funcType == asFUNC_INTERFACE )
				func = FindOnlyImplementation(m_func, func, objType);
			else
				func = 0;
		}
		if( func == 0 )
			continue;

		std::map<int, std::shared_ptr<CJITByteCode> >::iterator it = analysed.find(func->GetId());
		if( it == analysed.end() )
		{
			std::shared_ptr<CJITByteCode> callee;
			if( func != m_func && func->funcType == asFUNC_SCRIPT && func->scriptData && !func->IsVariadic() &&
			    func->scriptData->tryCatchInfo.GetLength() == 0 && func->scriptData->byteCode.GetLength() <= inlining.maxSize &&
			    (inlining.filter == 0 || inlining.filter(func, inlining.filterParam)) )
			{
				callee = std::make_shared<CJITByteCode>();
				if( callee->Decode(func) < 0 || !callee->CanBeInlined() )
					callee.reset();
				else
				{
					callee->Analyse(allowRegisterCache, maxCachedSlots);
					if( !callee->HasStaticStack() )
						callee.reset();
				}
			}
			it = analysed.insert(std::make_pair(func->GetId(), callee)).first;
		}

		if( it->second && it->second->GetLength() <= budget )
		{
			budget -= it->second->GetLength();
			instr.flags |= JIT_INSTR_INLINE;
			m_inlinees[n] = it->second;
			if( objType )
				m_inlineObjTypes[n] = objType;
		}
	}
}

void CJITByteCode::Analyse(bool allowRegisterCache, asUINT maxCachedSlots, const SJITInlineOptions *inlining)
{
	AnalyseStackDepth();
	m_inlinees.clear();
	m_inlineObjTypes.clear();
	if( inlining )
		FindInlinees(allowRegisterCache, maxCachedSlots, *inlining);
	BuildBlocks();
	AnalyseVRLiveness();
	AnalyseSlots(allowRegisterCache, maxCachedSlots);
	AnalyseSlotLiveness();
	AnalyseDirtySlots();
}

bool CJITByteCode::IsSyncPoint(asEBCInstr op)
{
	switch( op )
	{
	case asBC_CALL:
	case asBC_CALLSYS:
	case asBC_CALLBND:
	case asBC_CALLINTF:
	case asBC_CallPtr:
	case asBC_Thiscall1:
	case asBC_ALLOC:
	case asBC_REFCPY:
	case asBC_RefCpyV:
	case asBC_POWi:
	case asBC_POWu:
	case asBC_POWf:
	case asBC_POWd:
	case asBC_POWdi:
	case asBC_POWi64:
	case asBC_POWu64:
		return true;
	default:
		return false;
	}
}

// The script calls that may be native calls, after which the frame hasn't been
// restored, see JIT_NATIVE_RETURN. The function pointers are restored unless the
// stack is static, see CJITCodeGen::EmitScriptCall. So are the constructors of
// script classes
static bool LeavesFrameDirty(const SJITInstr &instr, bool staticStack)
{
#ifdef JIT_NATIVE_RETURN
	asEBCInstr op = instr.op;
	if( op == asBC_ALLOC )
		return (reinterpret_cast<asCObjectType*>(asBC_PTRARG(instr.bc))->flags & asOBJ_SCRIPT_OBJECT) != 0;
	return op == asBC_CALL || op == asBC_CALLINTF || (op == asBC_CallPtr && staticStack);
#else
	(void)instr;
	(void)staticStack;
	return false;
#endif
}

// Only the instructions that can have register cached variables as operands
// matter here, i.e. those working on primitives. Pointers and objects are never
// cached, so their instructions don't have to be described
void CJITByteCode::GetVarAccess(asEBCInstr op, bool &reads0, bool &writes0, bool &reads1, bool &reads2)
{
	reads0 = writes0 = reads1 = reads2 = false;
	switch( op )
	{
	// Read only first operand
	case asBC_PshV4:
	case asBC_PshV8:
	case asBC_CpyVtoR4:
	case asBC_CpyVtoR8:
	case asBC_CpyVtoG4:
	case asBC_WRTV1:
	case asBC_WRTV2:
	case asBC_WRTV4:
	case asBC_WRTV8:
	case asBC_CMPIi:
	case asBC_CMPIf:
	case asBC_CMPIu:
	case asBC_JMPP:
		reads0 = true;
		break;

	// Compares read both
	case asBC_CMPi:
	case asBC_CMPu:
	case asBC_CMPf:
	case asBC_CMPd:
	case asBC_CMPi64:
	case asBC_CMPu64:
		reads0 = reads1 = true;
		break;

	// Modified in place
	case asBC_NOT:
	case asBC_NEGi:
	case asBC_NEGf:
	case asBC_NEGd:
	case asBC_IncVi:
	case asBC_DecVi:
	case asBC_BNOT:
	case asBC_BNOT64:
	case asBC_NEGi64:
	case asBC_iTOf:
	case asBC_fTOi:
	case asBC_uTOf:
	case asBC_fTOu:
	case asBC_sbTOi:
	case asBC_swTOi:
	case asBC_ubTOi:
	case asBC_uwTOi:
	case asBC_iTOb:
	case asBC_iTOw:
	case asBC_dTOi64:
	case asBC_dTOu64:
	case asBC_i64TOd:
	case asBC_u64TOd:
		reads0 = writes0 = true;
		break;

	// Written from the second operand
	case asBC_CpyVtoV4:
	case asBC_CpyVtoV8:
	case asBC_ADDIi:
	case asBC_SUBIi:
	case asBC_MULIi:
	case asBC_ADDIf:
	case asBC_SUBIf:
	case asBC_MULIf:
	case asBC_dTOi:
	case asBC_dTOu:
	case asBC_dTOf:
	case asBC_iTOd:
	case asBC_uTOd:
	case asBC_fTOd:
	case asBC_i64TOi:
	case asBC_uTOi64:
	case asBC_iTOi64:
	case asBC_fTOi64:
	case asBC_fTOu64:
	case asBC_i64TOf:
	case asBC_u64TOf:
		writes0 = reads1 = true;
		break;

	// Written from the second and third operands
	case asBC_BAND:
	case asBC_BOR:
	case asBC_BXOR:
	case asBC_BSLL:
	case asBC_BSRL:
	case asBC_BSRA:
	case asBC_ADDi:
	case asBC_SUBi:
	case asBC_MULi:
	case asBC_DIVi:
	case asBC_MODi:
	case asBC_DIVu:
	case asBC_MODu:
	case asBC_ADDf:
	case asBC_SUBf:
	case asBC_MULf:
	case asBC_DIVf:
	case asBC_MODf:
	case asBC_ADDd:
	case asBC_SUBd:
	case asBC_MULd:
	case asBC_DIVd:
	case asBC_MODd:
	case asBC_POWi:
	case asBC_POWu:
	case asBC_POWf:
	case asBC_POWd:
	case asBC_POWdi:
	case asBC_ADDi64:
	case asBC_SUBi64:
	case asBC_MULi64:
	case asBC_DIVi64:
	case asBC_MODi64:
	case asBC_DIVu64:
	case asBC_MODu64:
	case asBC_BAND64:
	case asBC_BOR64:
	case asBC_BXOR64:
	case asBC_BSLL64:
	case asBC_BSRL64:
	case asBC_BSRA64:
	case asBC_POWi64:
	case asBC_POWu64:
		writes0 = reads1 = reads2 = true;
		break;

	// Written only
	case asBC_SetV1:
	case asBC_SetV2:
	case asBC_SetV4:
	case asBC_SetV8:
	case asBC_CpyRtoV4:
	case asBC_CpyRtoV8:
	case asBC_CpyGtoV4:
	case asBC_LdGRdR4:
	case asBC_RDR1:
	case asBC_RDR2:
	case asBC_RDR4:
	case asBC_RDR8:
		writes0 = true;
		break;

	default:
		break;
	}
}

// Masks of the cached slots read and written by the instruction
void CJITByteCode::GetSlotMasks(const SJITInstr &instr, asUINT &uses, asUINT &defs) const
{
	uses = defs = 0;
	bool reads0, writes0, reads1, reads2;
	GetVarAccess(instr.op, reads0, writes0, reads1, reads2);

	int bit;
	if( (reads0 || writes0) && (bit = GetCacheBit(asBC_SWORDARG0(instr.bc))) >= 0 )
	{
		if( reads0 )  uses |= asUINT(1) << bit;
		if( writes0 ) defs |= asUINT(1) << bit;
	}
	if( reads1 && (bit = GetCacheBit(asBC_SWORDARG1(instr.bc))) >= 0 )
		uses |= asUINT(1) << bit;
	if( reads2 && (bit = GetCacheBit(asBC_SWORDARG2(instr.bc))) >= 0 )
		uses |= asUINT(1) << bit;
}

// Backward data flow to find which cached slots may be read before being
// written in each block and after each instruction. Entry stubs only need to
// load those, and the VM only reads those when it takes over
void CJITByteCode::AnalyseSlotLiveness()
{
	m_liveIn.assign(m_blocks.size(), 0);
	m_liveAfter.assign(m_instrs.size(), 0);

	std::vector<asUINT> use(m_blocks.size(), 0), def(m_blocks.size(), 0);
	for( asUINT b = 0; b < m_blocks.size(); b++ )
	{
		for( asUINT n = m_blocks[b].first; n <= m_blocks[b].last; n++ )
		{
			asUINT uses, defs;
			GetSlotMasks(m_instrs[n], uses, defs);
			use[b] |= uses & ~def[b];
			def[b] |= defs;
		}
	}

	std::vector<asUINT> liveOut(m_blocks.size(), 0);
	std::vector<asUINT> succ;
	bool changed = true;
	while( changed )
	{
		changed = false;
		for( int b = int(m_blocks.size()) - 1; b >= 0; b-- )
		{
			GetSuccessors(b, succ);
			liveOut[b] = 0;
			for( asUINT k = 0; k < succ.size(); k++ )
				liveOut[b] |= m_liveIn[succ[k]];

			asUINT liveIn = use[b] | (liveOut[b] & ~def[b]);
			if( liveIn != m_liveIn[b] )
			{
				m_liveIn[b] = liveIn;
				changed = true;
			}
		}
	}

	for( asUINT b = 0; b < m_blocks.size(); b++ )
	{
		asUINT live = liveOut[b];
		for( asUINT n = m_blocks[b].last + 1; n-- > m_blocks[b].first; )
		{
			m_liveAfter[n] = live;
			asUINT uses, defs;
			GetSlotMasks(m_instrs[n], uses, defs);
			live = uses | (live & ~defs);
		}
	}
}

// The instructions that the code generator may emit together with a following conditional jump
static bool IsCompare(asEBCInstr op)
{
	switch( op )
	{
	case asBC_CMPd:
	case asBC_CMPu:
	case asBC_CMPf:
	case asBC_CMPi:
	case asBC_CMPIi:
	case asBC_CMPIf:
	case asBC_CMPIu:
	case asBC_CMPi64:
	case asBC_CMPu64:
	case asBC_CmpPtr:
		return true;
	default:
		return false;
	}
}

void CJITByteCode::GetSuccessors(asUINT blockIdx, std::vector<asUINT> &succ) const
{
	succ.clear();
	const SJITBlock &block = m_blocks[blockIdx];
	const SJITInstr &last = m_instrs[block.last];
	if( last.op == asBC_JMPP )
	{
		const std::vector<int> &targets = GetSwitchTargets(block.last);
		for( asUINT t = 0; t < targets.size(); t++ )
			succ.push_back(m_instrs[targets[t]].block);
	}
	else if( last.op != asBC_RET )
	{
		if( last.target >= 0 )
			succ.push_back(m_instrs[last.target].block);
		if( last.op != asBC_JMP && block.last + 1 < m_instrs.size() )
			succ.push_back(m_instrs[block.last + 1].block);
	}
}

// Forward data flow over the blocks to find out which register cached
// variables may hold a newer value than the memory at each instruction, and
// where the frame may not have been written back, see JIT_FRAME_BIT. Only those
// need to be stored when the VM must see the variables
void CJITByteCode::AnalyseDirtySlots()
{
	m_dirty.assign(m_instrs.size(), 0);
	m_storeBefore.assign(m_instrs.size(), 0);
	m_storeAfter.assign(m_instrs.size(), 0);

	asUINT cachedMask = JIT_FRAME_BIT;
	for( asUINT n = 0; n < m_slots.size(); n++ )
		if( m_slots[n].cacheBit >= 0 )
			cachedMask |= 1u << m_slots[n].cacheBit;

	// The calls in a loop store the dirty variables on every iteration, also those
	// that are only modified before the loop. The variables that a loop with calls
	// doesn't modify are stored where it is entered instead: before the branch into
	// it, or the compare emitted together with the branch, or after the instruction
	// that falls into it. Loops are the ranges from the target of a backward branch
	// to the branch
	std::vector<asUINT> keepBefore(m_instrs.size(), 0), keepAfter(m_instrs.size(), 0);
	std::vector<asUINT> succ;
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		int top = m_instrs[n].target;
		if( !IsBranch(m_instrs[n].op) || top < 0 || asUINT(top) > n )
			continue;

		asUINT written = 0;
		bool calls = false;
		for( asUINT k = asUINT(top); k <= n; k++ )
		{
			asUINT uses, defs;
			GetSlotMasks(m_instrs[k], uses, defs);
			written |= defs;
			if( LeavesFrameDirty(m_instrs[k], m_staticStack) )
				written |= JIT_FRAME_BIT;
			calls = calls || (IsSyncPoint(m_instrs[k].op) && !(m_instrs[k].flags & JIT_INSTR_INLINE));
		}
		asUINT keep = cachedMask & ~written;
		if( !calls || keep == 0 )
			continue;

		for( asUINT b = 0; b < m_blocks.size(); b++ )
		{
			const SJITBlock &block = m_blocks[b];
			if( block.last >= asUINT(top) && block.last <= n )
				continue;

			GetSuccessors(b, succ);
			bool enters = false;
			for( asUINT k = 0; k < succ.size(); k++ )
				enters = enters || (m_blocks[succ[k]].first >= asUINT(top) && m_blocks[succ[k]].first <= n);
			if( !enters )
				continue;

			asUINT last = block.last;
			if( IsBranch(m_instrs[last].op) || m_instrs[last].op == asBC_JMPP )
			{
				if( m_instrs[last].op != asBC_JMP && last > block.first && IsCompare(m_instrs[last - 1].op) )
					last--;
				keepBefore[last] |= keep;
			}
			else
				keepAfter[last] |= keep;
		}
	}

	std::vector<asUINT> in(m_blocks.size(), 0);
	if( !m_instrs.empty() )
		in[m_instrs[0].block] = JIT_FRAME_BIT;
	bool changed = true;
	while( changed )
	{
		changed = false;
		for( asUINT b = 0; b < m_blocks.size(); b++ )
		{
			const SJITBlock &block = m_blocks[b];
			asUINT mask = in[b];
			for( asUINT n = block.first; n <= block.last; n++ )
			{
				// Temporary variables that won't be read anymore don't need to be
				// stored, nothing else can see them
				const SJITInstr &instr = m_instrs[n];
				asUINT uses, defs;
				GetSlotMasks(instr, uses, defs);
				mask &= ~m_tempMask | uses | (m_liveAfter[n] & ~defs);

				m_storeBefore[n] = mask & keepBefore[n];
				mask &= ~keepBefore[n];
				m_dirty[n] = mask;

				// The inlined calls only store the variables on the rare path that
				// calls the function, which may leave the frame dirty too
				if( IsSyncPoint(instr.op) && !(instr.flags & JIT_INSTR_INLINE) )
					mask = LeavesFrameDirty(instr, m_staticStack) ? JIT_FRAME_BIT : 0;
				else
				{
					if( LeavesFrameDirty(instr, m_staticStack) )
						defs |= JIT_FRAME_BIT;
					mask = (mask | defs) & (~m_tempMask | m_liveAfter[n]);
				}

				m_storeAfter[n] = mask & keepAfter[n];
				mask &= ~keepAfter[n];
			}

			// Propagate to the successors
			GetSuccessors(b, succ);
			for( asUINT k = 0; k < succ.size(); k++ )
			{
				if( (in[succ[k]] | mask) != in[succ[k]] )
				{
					in[succ[k]] |= mask;
					changed = true;
				}
			}
		}
	}
}

int CJITByteCode::GetCacheBit(int offset) const
{
	std::map<int, int>::const_iterator it = m_slotIndex.find(offset);
	if( it == m_slotIndex.end() )
		return -1;
	return m_slots[it->second].cacheBit;
}

void CJITByteCode::BuildBlocks()
{
	m_blocks.clear();
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		if( m_instrs[n].flags & JIT_INSTR_BLOCK_START )
		{
			SJITBlock block;
			block.first = n;
			block.last  = n;
			block.vrLiveIn = block.vrLiveOut = block.vrUse = block.vrDef = false;
			m_blocks.push_back(block);
		}
		m_blocks.back().last = n;
		m_instrs[n].block = int(m_blocks.size()) - 1;
	}
}

// RET only passes the value register to the caller for primitives and references.
// Handles are returned in the object register and objects in memory
bool CJITByteCode::ReturnsInVR(asCScriptFunction *func)
{
	const asCDataType &rt = func->returnType;
	return rt.IsReference() || (rt.GetTokenType() != ttVoid && !rt.IsObject() && !rt.IsObjectHandle() && !rt.IsFuncdef());
}

void CJITByteCode::AnalyseVRLiveness()
{
	bool retReadsVR = ReturnsInVR(m_func);
	m_retReadsVR = retReadsVR;

	// Local use/def per block
	for( asUINT b = 0; b < m_blocks.size(); b++ )
	{
		SJITBlock &block = m_blocks[b];
		for( asUINT n = block.first; n <= block.last; n++ )
		{
			asEBCInstr op = m_instrs[n].op;
			if( ReadsVR(op) && (op != asBC_RET || retReadsVR) && !block.vrDef )
				block.vrUse = true;
			if( WritesVR(op) )
				block.vrDef = true;
		}
	}

	// Backward data flow until nothing changes
	bool changed = true;
	while( changed )
	{
		changed = false;
		for( int b = int(m_blocks.size()) - 1; b >= 0; b-- )
		{
			SJITBlock &block = m_blocks[b];
			const SJITInstr &last = m_instrs[block.last];

			bool liveOut = false;
			if( last.op == asBC_JMPP )
			{
				const std::vector<int> &targets = GetSwitchTargets(block.last);
				for( asUINT t = 0; t < targets.size(); t++ )
					liveOut = liveOut || m_blocks[m_instrs[targets[t]].block].vrLiveIn;
			}
			else if( last.op == asBC_RET )
			{
				liveOut = false;
			}
			else
			{
				if( last.target >= 0 )
					liveOut = m_blocks[m_instrs[last.target].block].vrLiveIn;
				if( last.op != asBC_JMP && block.last + 1 < m_instrs.size() )
					liveOut = liveOut || m_blocks[m_instrs[block.last + 1].block].vrLiveIn;
			}

			bool liveIn = block.vrUse || (!block.vrDef && liveOut);
			if( liveOut != block.vrLiveOut || liveIn != block.vrLiveIn )
			{
				block.vrLiveOut = liveOut;
				block.vrLiveIn  = liveIn;
				changed = true;
			}
		}
	}

	// Per instruction liveness
	for( asUINT b = 0; b < m_blocks.size(); b++ )
	{
		SJITBlock &block = m_blocks[b];
		bool live = block.vrLiveOut;
		for( int n = int(block.last); n >= int(block.first); n-- )
		{
			if( live )
				m_instrs[n].flags |= JIT_INSTR_VR_LIVE;
			else
				m_instrs[n].flags &= ~JIT_INSTR_VR_LIVE;

			asEBCInstr op = m_instrs[n].op;
			if( WritesVR(op) )
				live = false;
			if( ReadsVR(op) && (op != asBC_RET || retReadsVR) )
				live = true;
		}
	}
}

bool CJITByteCode::IsVRLiveBefore(asUINT instrIdx) const
{
	asEBCInstr op = m_instrs[instrIdx].op;
	if( ReadsVR(op) && (op != asBC_RET || m_retReadsVR) )
		return true;
	return !WritesVR(op) && IsVRLiveAfter(instrIdx);
}

SJITSlot *CJITByteCode::FindSlot(int offset)
{
	std::map<int, int>::iterator it = m_slotIndex.find(offset);
	if( it == m_slotIndex.end() )
		return 0;
	return &m_slots[it->second];
}

void CJITByteCode::AddSlotUse(int offset, asUINT kind)
{
	SJITSlot *slot = FindSlot(offset);
	if( slot == 0 )
	{
		SJITSlot s;
		s.offset    = offset;
		s.kinds     = 0;
		s.useCount  = 0;
		s.cacheKind = JIT_SLOT_NONE;
		s.cacheBit  = -1;
		m_slotIndex[offset] = int(m_slots.size());
		m_slots.push_back(s);
		slot = &m_slots.back();
	}
	slot->kinds |= kind;
	slot->useCount++;
}

void CJITByteCode::CollectSlotUses(const SJITInstr &instr)
{
	const asDWORD *bc = instr.bc;
	int a0 = asBC_SWORDARG0(bc);
	int a1 = asBC_SWORDARG1(bc);
	int a2 = asBC_SWORDARG2(bc);

	switch( instr.op )
	{
	// 32bit integer and boolean operations
	case asBC_NOT:
	case asBC_IncVi:
	case asBC_DecVi:
	case asBC_BNOT:
	case asBC_NEGi:
	case asBC_iTOb:
	case asBC_iTOw:
	case asBC_sbTOi:
	case asBC_swTOi:
	case asBC_ubTOi:
	case asBC_uwTOi:
	case asBC_JMPP:
	case asBC_CMPIi:
	case asBC_CMPIu:
	case asBC_WRTV1:
	case asBC_WRTV2:
		AddSlotUse(a0, JIT_SLOT_I32);
		break;
	case asBC_BAND:
	case asBC_BOR:
	case asBC_BXOR:
	case asBC_BSLL:
	case asBC_BSRL:
	case asBC_BSRA:
	case asBC_ADDi:
	case asBC_SUBi:
	case asBC_MULi:
	case asBC_DIVi:
	case asBC_MODi:
	case asBC_DIVu:
	case asBC_MODu:
	case asBC_POWi:
	case asBC_POWu:
		AddSlotUse(a0, JIT_SLOT_I32);
		AddSlotUse(a1, JIT_SLOT_I32);
		AddSlotUse(a2, JIT_SLOT_I32);
		break;
	case asBC_ADDIi:
	case asBC_SUBIi:
	case asBC_MULIi:
	case asBC_CMPi:
	case asBC_CMPu:
		AddSlotUse(a0, JIT_SLOT_I32);
		AddSlotUse(a1, JIT_SLOT_I32);
		break;

	// 64bit integer operations
	case asBC_NEGi64:
	case asBC_BNOT64:
		AddSlotUse(a0, JIT_SLOT_I64);
		break;
	case asBC_ADDi64:
	case asBC_SUBi64:
	case asBC_MULi64:
	case asBC_DIVi64:
	case asBC_MODi64:
	case asBC_DIVu64:
	case asBC_MODu64:
	case asBC_BAND64:
	case asBC_BOR64:
	case asBC_BXOR64:
	case asBC_POWi64:
	case asBC_POWu64:
		AddSlotUse(a0, JIT_SLOT_I64);
		AddSlotUse(a1, JIT_SLOT_I64);
		AddSlotUse(a2, JIT_SLOT_I64);
		break;
	case asBC_BSLL64:
	case asBC_BSRL64:
	case asBC_BSRA64:
		AddSlotUse(a0, JIT_SLOT_I64);
		AddSlotUse(a1, JIT_SLOT_I64);
		AddSlotUse(a2, JIT_SLOT_I32);
		break;
	case asBC_CMPi64:
	case asBC_CMPu64:
		AddSlotUse(a0, JIT_SLOT_I64);
		AddSlotUse(a1, JIT_SLOT_I64);
		break;

	// float operations
	case asBC_NEGf:
	case asBC_CMPIf:
		AddSlotUse(a0, JIT_SLOT_F32);
		break;
	case asBC_ADDf:
	case asBC_SUBf:
	case asBC_MULf:
	case asBC_DIVf:
	case asBC_MODf:
	case asBC_POWf:
		AddSlotUse(a0, JIT_SLOT_F32);
		AddSlotUse(a1, JIT_SLOT_F32);
		AddSlotUse(a2, JIT_SLOT_F32);
		break;
	case asBC_ADDIf:
	case asBC_SUBIf:
	case asBC_MULIf:
	case asBC_CMPf:
		AddSlotUse(a0, JIT_SLOT_F32);
		AddSlotUse(a1, JIT_SLOT_F32);
		break;

	// double operations
	case asBC_NEGd:
		AddSlotUse(a0, JIT_SLOT_F64);
		break;
	case asBC_ADDd:
	case asBC_SUBd:
	case asBC_MULd:
	case asBC_DIVd:
	case asBC_MODd:
	case asBC_POWd:
		AddSlotUse(a0, JIT_SLOT_F64);
		AddSlotUse(a1, JIT_SLOT_F64);
		AddSlotUse(a2, JIT_SLOT_F64);
		break;
	case asBC_POWdi:
		AddSlotUse(a0, JIT_SLOT_F64);
		AddSlotUse(a1, JIT_SLOT_F64);
		AddSlotUse(a2, JIT_SLOT_I32);
		break;
	case asBC_CMPd:
		AddSlotUse(a0, JIT_SLOT_F64);
		AddSlotUse(a1, JIT_SLOT_F64);
		break;

	// conversions
	case asBC_iTOf:
	case asBC_fTOi:
	case asBC_uTOf:
	case asBC_fTOu:
		AddSlotUse(a0, JIT_SLOT_I32 | JIT_SLOT_F32);
		break;
	case asBC_dTOi:
	case asBC_dTOu:
		AddSlotUse(a0, JIT_SLOT_I32);
		AddSlotUse(a1, JIT_SLOT_F64);
		break;
	case asBC_dTOf:
		AddSlotUse(a0, JIT_SLOT_F32);
		AddSlotUse(a1, JIT_SLOT_F64);
		break;
	case asBC_iTOd:
	case asBC_uTOd:
		AddSlotUse(a0, JIT_SLOT_F64);
		AddSlotUse(a1, JIT_SLOT_I32);
		break;
	case asBC_fTOd:
		AddSlotUse(a0, JIT_SLOT_F64);
		AddSlotUse(a1, JIT_SLOT_F32);
		break;
	case asBC_i64TOi:
		AddSlotUse(a0, JIT_SLOT_I32);
		AddSlotUse(a1, JIT_SLOT_I64);
		break;
	case asBC_uTOi64:
	case asBC_iTOi64:
		AddSlotUse(a0, JIT_SLOT_I64);
		AddSlotUse(a1, JIT_SLOT_I32);
		break;
	case asBC_fTOi64:
	case asBC_fTOu64:
		AddSlotUse(a0, JIT_SLOT_I64);
		AddSlotUse(a1, JIT_SLOT_F32);
		break;
	case asBC_dTOi64:
	case asBC_dTOu64:
	case asBC_i64TOd:
	case asBC_u64TOd:
		AddSlotUse(a0, JIT_SLOT_I64 | JIT_SLOT_F64);
		break;
	case asBC_i64TOf:
	case asBC_u64TOf:
		AddSlotUse(a0, JIT_SLOT_F32);
		AddSlotUse(a1, JIT_SLOT_I64);
		break;

	// untyped 32bit accesses
	case asBC_PshV4:
	case asBC_SetV1:
	case asBC_SetV2:
	case asBC_SetV4:
	case asBC_CpyVtoR4:
	case asBC_CpyRtoV4:
	case asBC_CpyVtoG4:
	case asBC_CpyGtoV4:
	case asBC_LdGRdR4:
	case asBC_WRTV4:
	case asBC_RDR1:
	case asBC_RDR2:
	case asBC_RDR4:
		AddSlotUse(a0, JIT_SLOT_ANY32);
		break;
	case asBC_CpyVtoV4:
		AddSlotUse(a0, JIT_SLOT_ANY32);
		AddSlotUse(a1, JIT_SLOT_ANY32);
		break;

	// untyped 64bit accesses
	case asBC_PshV8:
	case asBC_SetV8:
	case asBC_CpyVtoR8:
	case asBC_CpyRtoV8:
	case asBC_WRTV8:
	case asBC_RDR8:
		AddSlotUse(a0, JIT_SLOT_ANY64);
		break;
	case asBC_CpyVtoV8:
		AddSlotUse(a0, JIT_SLOT_ANY64);
		AddSlotUse(a1, JIT_SLOT_ANY64);
		break;

	// pointers, handles, and objects
	case asBC_PshVPtr:
	case asBC_ChkNullV:
	case asBC_LoadRObjR:
	case asBC_ClrVPtr:
	case asBC_FREE:
	case asBC_LOADOBJ:
	case asBC_STOREOBJ:
	case asBC_RefCpyV:
	case asBC_AllocMem:
	case asBC_SetListSize:
	case asBC_PshListElmnt:
	case asBC_SetListType:
		AddSlotUse(a0, JIT_SLOT_PTR);
		break;
	case asBC_CallPtr:
		AddSlotUse(a1, JIT_SLOT_PTR);
		break;
	case asBC_CmpPtr:
		AddSlotUse(a0, JIT_SLOT_PTR);
		AddSlotUse(a1, JIT_SLOT_PTR);
		break;

	// the address of the variable is taken
	case asBC_PSF:
	case asBC_VAR:
	case asBC_LDV:
	case asBC_LoadVObjR:
		AddSlotUse(a0, JIT_SLOT_ADDR);
		break;

	default:
		// Instruction doesn't reference variables directly
		break;
	}
}

// TODO: runtime optimize: Only primitive variables are kept in registers. Handles and
//                         object pointers that are never address-taken could be cached
//                         the same way, which would help code that indexes arrays or
//                         calls methods on the same handle in a loop. The dirty and
//                         live masks are 32bit, so at most 32 variables can be cached;
//                         larger functions would need a wider mask or a second pass
//                         choosing the variables per loop rather than per function.
void CJITByteCode::AnalyseSlots(bool allowRegisterCache, asUINT maxCachedSlots)
{
	m_slots.clear();
	m_slotIndex.clear();
	m_tempMask = 0;

	for( asUINT n = 0; n < m_instrs.size(); n++ )
		CollectSlotUses(m_instrs[n]);

	if( !allowRegisterCache )
		return;

	// Determine which slots hold a single kind of primitive value and are never
	// accessed through their address. Only those can be kept in registers
	for( asUINT n = 0; n < m_slots.size(); n++ )
	{
		SJITSlot &slot = m_slots[n];
		asUINT kinds = slot.kinds;
		asUINT typed = kinds & (JIT_SLOT_I32 | JIT_SLOT_I64 | JIT_SLOT_F32 | JIT_SLOT_F64);

		slot.cacheKind = JIT_SLOT_NONE;
		if( kinds & (JIT_SLOT_PTR | JIT_SLOT_ADDR) )
			continue;

		// More than one typed kind
		if( typed & (typed - 1) )
			continue;

		if( typed == 0 )
		{
			if( kinds == JIT_SLOT_ANY32 )
				typed = JIT_SLOT_I32;
			else if( kinds == JIT_SLOT_ANY64 )
				typed = JIT_SLOT_I64;
			else
				continue;
		}

		bool is64 = (typed == JIT_SLOT_I64 || typed == JIT_SLOT_F64);
		if( is64 && (kinds & JIT_SLOT_ANY32) )
			continue;
		if( !is64 && (kinds & JIT_SLOT_ANY64) )
			continue;

		// 64bit integers cannot be held in a single register on 32bit hosts
		if( typed == JIT_SLOT_I64 && sizeof(void*) < 8 )
			continue;

		slot.cacheKind = int(typed);
	}

	// 64bit slots also cover the dword below them. Make sure no other slot
	// overlaps, which would be the case for malformed or unexpected bytecode
	for( asUINT n = 0; n < m_slots.size(); n++ )
	{
		SJITSlot &slot = m_slots[n];
		bool is64 = (slot.kinds & (JIT_SLOT_I64 | JIT_SLOT_F64 | JIT_SLOT_ANY64)) != 0;
		if( is64 )
		{
			SJITSlot *other = FindSlot(slot.offset - 1);
			if( other )
			{
				slot.cacheKind  = JIT_SLOT_NONE;
				other->cacheKind = JIT_SLOT_NONE;
			}
		}
	}

	// Limit the number of cached slots to keep the register pressure reasonable.
	// The most used slots are kept
	asUINT count = 0;
	for( asUINT n = 0; n < m_slots.size(); n++ )
		if( m_slots[n].cacheKind != JIT_SLOT_NONE )
			count++;

	if( count > maxCachedSlots )
	{
		std::vector<SJITSlot*> sorted;
		for( asUINT n = 0; n < m_slots.size(); n++ )
			if( m_slots[n].cacheKind != JIT_SLOT_NONE )
				sorted.push_back(&m_slots[n]);
		std::stable_sort(sorted.begin(), sorted.end(), [](const SJITSlot *a, const SJITSlot *b) { return a->useCount > b->useCount; });
		for( asUINT n = maxCachedSlots; n < sorted.size(); n++ )
			sorted[n]->cacheKind = JIT_SLOT_NONE;
	}

	// Give each cached slot a bit for the dirty masks
	int bit = 0;
	for( asUINT n = 0; n < m_slots.size(); n++ )
		if( m_slots[n].cacheKind != JIT_SLOT_NONE )
			m_slots[n].cacheBit = bit++;

	// Temporary variables have no name. Named variables and parameters may be
	// inspected through the debug interface
	const asCArray<asSScriptVariable*> &vars = m_func->scriptData->variables;
	for( asUINT n = 0; n < m_slots.size(); n++ )
	{
		if( m_slots[n].cacheBit < 0 || m_slots[n].offset <= 0 )
			continue;
		bool named = false;
		for( asUINT v = 0; v < vars.GetLength() && !named; v++ )
			named = vars[v]->stackOffset == m_slots[n].offset && vars[v]->name.GetLength() > 0;
		if( !named )
			m_tempMask |= asUINT(1) << m_slots[n].cacheBit;
	}
}

int CJITByteCode::GetCacheKind(int offset) const
{
	std::map<int, int>::const_iterator it = m_slotIndex.find(offset);
	if( it == m_slotIndex.end() )
		return JIT_SLOT_NONE;
	return m_slots[it->second].cacheKind;
}

END_AS_NAMESPACE
