#include "jit_bytecode.h"

// Internal engine headers. The JIT must be compiled with the same
// configuration as the engine library (see CMakeLists.txt)
#include "as_scriptfunction.h"
#include "as_scriptengine.h"
#include "as_module.h"
#include "as_objecttype.h"
#include "as_callfunc.h"

#include <algorithm>
#include <climits>

BEGIN_AS_NAMESPACE

CJITByteCode::CJITByteCode()
{
	m_func     = 0;
	m_byteCode = 0;
	m_length   = 0;
	m_retReadsVR = false;
	m_tempMask   = 0;
	m_thisConstant = false;
	m_staticStack = false;
	m_aot = false;
	m_inlinedLength = 0;
	m_borrowableParams = 0;
	m_releasedParams   = 0;
	m_bail = 0;
	m_hasSyncPoints = false;
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
		// line callback is already invoked when the function is entered. Older
		// versions of the engine kept it after the JitEntry at the start of the
		// function, e.g. in the bytecode saved by them, which would invoke the
		// callback twice for the same line. Such leading SUSPENDs are skipped here
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

	MarkUnreachable(false);

	return asSUCCESS;
}

// Marks the instructions that can never be executed natively, e.g. the jump tables
// of switch statements that are only reached through JMPP, so that no code is
// generated for them. Neither is anything that only follows the instructions that
// always return to the VM, which executes it until it enters the function again at
// one of the entry points. Nothing enters the inlined functions but at their start
void CJITByteCode::MarkUnreachable(bool inlined)
{
	for( asUINT n = 0; n < m_instrs.size(); n++ )
		m_instrs[n].flags &= ~JIT_INSTR_DEAD;

	// Native callers enter at the start of the function, see CJITCodeGen::EmitDirectEntry
	std::vector<bool> reached(m_instrs.size(), false);
	std::vector<asUINT> work;
	if( !inlined )
		work.assign(m_entries.begin(), m_entries.end());
	work.push_back(0);

	while( !work.empty() )
	{
		asUINT n = work.back();
		work.pop_back();
		if( reached[n] )
			continue;
		reached[n] = true;

		const SJITInstr &instr = m_instrs[n];
		if( instr.flags & JIT_INSTR_BAIL )
			continue;
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
	m_bail = bail;
	for( asUINT n = 0; n < m_instrs.size(); n++ )
		if( bail[m_instrs[n].op] )
			m_instrs[n].flags |= JIT_INSTR_BAIL;
}

const std::vector<SJITInlinee> &CJITByteCode::GetInlinees(asUINT instrIdx) const
{
	static const std::vector<SJITInlinee> none;
	std::map<asUINT, std::vector<SJITInlinee> >::const_iterator it = m_inlinees.find(instrIdx);
	return it == m_inlinees.end() ? none : it->second;
}

bool CJITByteCode::InlineesHaveSyncPoints(asUINT instrIdx) const
{
	const std::vector<SJITInlinee> &inlinees = GetInlinees(instrIdx);
	asUINT borrowed = GetBorrowedArgs(instrIdx);
	for( asUINT n = 0; n < inlinees.size(); n++ )
		if( inlinees[n].code->HasSyncPoints(borrowed) )
			return true;
	return false;
}


const SJITIndexerCall *CJITByteCode::GetIndexer(asUINT instrIdx) const
{
	std::map<asUINT, SJITIndexerCall>::const_iterator it = m_indexers.find(instrIdx);
	return it == m_indexers.end() ? 0 : &it->second;
}

asUINT CJITByteCode::GetBorrowedArgs(asUINT instrIdx) const
{
	std::map<asUINT, asUINT>::const_iterator it = m_borrowedArgs.find(instrIdx);
	return it == m_borrowedArgs.end() ? 0 : it->second;
}

const std::vector<int> &CJITByteCode::GetBorrowChecks(asUINT instrIdx) const
{
	std::map<asUINT, std::vector<int> >::const_iterator it = m_borrowChecks.find(instrIdx);
	if( it == m_borrowChecks.end() )
		return m_noChecks;
	return it->second;
}

int CJITByteCode::FindParam(int offset) const
{
	int var = -((m_func->objectType ? AS_PTR_SIZE : 0) + (m_func->DoesReturnOnStack() ? AS_PTR_SIZE : 0));
	for( asUINT n = 0; n < m_func->parameterTypes.GetLength(); n++ )
	{
		if( var == offset )
			return int(n);
		var -= m_func->parameterTypes[n].GetSizeOnStackDWords();
	}
	return -1;
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

// The code of an inlined function runs in the frame of the caller, which is handed
// to the VM together with the frame of the function where it calls anything, see
// CJITCodeGen::EmitMaterialize. Only the deprecated asBC_STR isn't compiled at all
bool CJITByteCode::CanBeInlined() const
{
	for( asUINT n = 0; n < m_instrs.size(); n++ )
		if( m_instrs[n].op == asBC_STR && !(m_instrs[n].flags & JIT_INSTR_DEAD) )
			return false;
	return true;
}

// Returns the implementation of a virtual or interface method in a class, like
// asCContext::CallInterfaceMethod, or null if the class doesn't implement it
static asCScriptFunction *GetImplementation(asCObjectType *cls, asCScriptFunction *method)
{
	asUINT index = asUINT(method->vfTableIdx);
	if( method->funcType == asFUNC_INTERFACE )
	{
		asUINT n = 0;
		while( n < cls->interfaces.GetLength() && cls->interfaces[n] != method->objectType )
			n++;
		if( n == cls->interfaces.GetLength() )
			return 0;
		index += cls->interfaceVFTOffsets[n];
	}
	return index < cls->virtualFunctionTable.GetLength() ? cls->virtualFunctionTable[index] : 0;
}

// Returns true if objects of the class can call the virtual or interface method
static bool CanCall(asCObjectType *cls, asCScriptFunction *method)
{
	if( !(cls->flags & asOBJ_SCRIPT_OBJECT) || (cls->flags & asOBJ_ABSTRACT) || cls->IsInterface() )
		return false;
	return method->funcType == asFUNC_INTERFACE ? cls->Implements(method->objectType) : cls->DerivesFrom(method->objectType);
}

// Returns the implementation of a virtual or interface method in the classes of the
// module of the caller that objects calling it can be of, or null if they implement
// it differently. If only one class can, it is returned in objType, and the class of
// the object is checked. Otherwise the method must be virtual, whose implementation
// is checked in the table of the class of the object. The classes of other modules
// may derive from the shared classes and implement the shared interfaces, so the
// object must still be checked
static asCScriptFunction *FindImplementation(asCScriptFunction *caller, asCScriptFunction *method, asCObjectType *&objType)
{
	objType = 0;
	if( caller->module == 0 || method->objectType == 0 || method->vfTableIdx < 0 )
		return 0;

	asCScriptFunction *found = 0;
	asUINT count = 0;
	const asCArray<asCObjectType*> &classes = caller->module->m_classTypes;
	for( asUINT n = 0; n < classes.GetLength(); n++ )
	{
		asCObjectType *cls = classes[n];
		if( !CanCall(cls, method) )
			continue;
		asCScriptFunction *impl = GetImplementation(cls, method);
		if( impl == 0 || (count > 0 && (impl != found || method->funcType == asFUNC_INTERFACE)) )
			return 0;
		found = impl;
		objType = count++ == 0 ? cls : 0;
	}
	return found;
}

// Returns true if the class that a profile has seen is one of the module of the
// function. The others may have been destroyed, and are only compared with those
static bool IsModuleClass(asCScriptFunction *func, asCObjectType *seen)
{
	if( func->module == 0 )
		return false;
	const asCArray<asCObjectType*> &classes = func->module->m_classTypes;
	for( asUINT n = 0; n < classes.GetLength(); n++ )
		if( classes[n] == seen )
			return true;
	return false;
}

// Returns the implementation of a virtual or interface method in the class that the
// profile has seen at the call, which is returned in objType, if it is a class of the
// module of the caller that objects calling the method can be of
static asCScriptFunction *FindSeenImplementation(asCScriptFunction *caller, asCScriptFunction *method, asCObjectType *seen, asCObjectType *&objType)
{
	objType = 0;
	if( method->objectType == 0 || method->vfTableIdx < 0 || !IsModuleClass(caller, seen) || !CanCall(seen, method) )
		return 0;
	objType = seen;
	return GetImplementation(seen, method);
}

// Returns true if a class of the module of the caller that objects calling a virtual
// or interface method can be of implements it with a script function small enough
// to be inlined, so that the classes that the call sees are worth noting
static bool HasSmallImplementation(asCScriptFunction *caller, asCScriptFunction *method, asUINT maxSize)
{
	if( caller->module == 0 || method->objectType == 0 || method->vfTableIdx < 0 )
		return false;

	const asCArray<asCObjectType*> &classes = caller->module->m_classTypes;
	for( asUINT n = 0; n < classes.GetLength(); n++ )
	{
		asCScriptFunction *impl = CanCall(classes[n], method) ? GetImplementation(classes[n], method) : 0;
		if( impl && impl->funcType == asFUNC_SCRIPT && impl->scriptData && impl->scriptData->byteCode.GetLength() <= maxSize )
			return true;
	}
	return false;
}

const SJITSeenClasses *SJITProfile::Find(asCScriptFunction *func, asUINT instrIdx) const
{
	std::map<std::pair<asCScriptFunction*, asUINT>, SJITSeenClasses>::const_iterator it = classes.find(std::make_pair(func, instrIdx));
	return it == classes.end() ? 0 : &it->second;
}

// The calls that have seen only the classes they were compiled with, or those of
// other modules, or none, gain nothing from compiling the function again
bool SJITProfile::HasNewClass(const SJITProfile &compiledWith) const
{
	std::map<std::pair<asCScriptFunction*, asUINT>, SJITSeenClasses>::const_iterator it;
	for( it = classes.begin(); it != classes.end(); ++it )
	{
		const SJITSeenClasses *before = compiledWith.Find(it->first.first, it->first.second);
		for( asUINT n = 0; n < JIT_PROFILE_CLASSES; n++ )
		{
			asCObjectType *seen = it->second.types[n];
			if( seen && !(before && before->Has(seen)) && IsModuleClass(it->first.first, seen) )
				return true;
		}
	}
	return false;
}

// The search for the functions to inline into a function and into those
struct CJITByteCode::SInlineSearch
{
	bool                     allowRegisterCache;
	asUINT                   maxCachedSlots;
	const SJITInlineOptions *options;
	std::vector<int>         path; // the ids of the functions being inlined into
	std::map<std::pair<int, asUINT>, std::shared_ptr<CJITByteCode> > analysed; // by function id and levels left to it, null if it can't be inlined
};

// Analyses the function for being emitted in place, or returns null if it can't be,
// see FindInlinees. The analyses are shared by the calls of the same function at the
// same level
std::shared_ptr<CJITByteCode> CJITByteCode::AnalyseInlinee(SInlineSearch &search, asCScriptFunction *func, asUINT levels)
{
	const SJITInlineOptions &inlining = *search.options;
	std::pair<int, asUINT> key(func->GetId(), levels - 1);
	std::map<std::pair<int, asUINT>, std::shared_ptr<CJITByteCode> >::iterator it = search.analysed.find(key);
	if( it != search.analysed.end() )
		return it->second;

	std::shared_ptr<CJITByteCode> callee;
	if( func->funcType == asFUNC_SCRIPT && func->scriptData && !func->IsVariadic() &&
	    func->scriptData->tryCatchInfo.GetLength() == 0 && func->scriptData->byteCode.GetLength() <= inlining.maxSize &&
	    (inlining.filter == 0 || inlining.filter(func, inlining.filterParam)) )
	{
		// The functions it calls are inlined into it with a quarter of the
		// budget of the function being compiled
		callee = std::make_shared<CJITByteCode>();
		if( callee->Decode(func) < 0 || !callee->CanBeInlined() )
			callee.reset();
		else
		{
			if( m_bail )
				callee->SetBailInstructions(m_bail);
			callee->MarkUnreachable(true);
			callee->AnalyseStackDepth();
			if( !callee->HasStaticStack() )
				callee.reset();
			else
			{
				search.path.push_back(func->GetId());
				callee->FindInlinees(search, levels - 1, inlining.maxSize * 4);
				search.path.pop_back();
				callee->FindIndexers(inlining.indexers);
				callee->AnalyseBorrows();
				callee->AnalyseBody(search.allowRegisterCache, search.maxCachedSlots);
			}
		}
	}
	search.analysed.insert(std::make_pair(key, callee));
	return callee;
}

// Finds the calls whose function can be emitted in place, which is analysed on its
// own then. Its frame starts at the stack pointer of the call, so the depth of the
// stack must be known. The function can have the functions that it calls emitted in
// its code in turn, down to the levels left. The code inlined into a function is
// limited by the budget, so that the functions calling many don't grow without
// bounds. Recursion and functions with catch blocks are left to the calls. The
// virtual and interface methods are inlined if the classes that can implement them
// all have the same implementation, which the object is checked for, see
// FindImplementation. Otherwise the calls note the classes that they see in the
// profile, and the methods are inlined for the classes that a call has seen when the
// function is compiled again with it, each checked for, where the call notes the
// others then. The classes that share an implementation share its code
void CJITByteCode::FindInlinees(SInlineSearch &search, asUINT levels, asUINT budget)
{
	m_inlinees.clear();
	m_inlinedLength = 0;
	const SJITInlineOptions &inlining = *search.options;
	if( inlining.maxSize == 0 || !m_staticStack || levels == 0 )
		return;

	asCScriptEngine *engine = m_func->engine;
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		SJITInstr &instr = m_instrs[n];
		if( (instr.op != asBC_CALL && instr.op != asBC_CALLINTF) || (instr.flags & JIT_INSTR_DEAD) )
			continue;

		// The implementations to emit, and the class whose objects call each, or null
		// for the only one, which is checked for in the table of the class
		std::vector<std::pair<asCScriptFunction*, asCObjectType*> > impls;
		bool profiled = false;
		int id = asBC_INTARG(instr.bc);
		asCScriptFunction *func = id >= 0 && asUINT(id) < engine->scriptFunctions.GetLength() ? engine->scriptFunctions[id] : 0;
		if( func && instr.op == asBC_CALLINTF )
		{
			asCScriptFunction *method = func;
			if( method->funcType == asFUNC_VIRTUAL || method->funcType == asFUNC_INTERFACE )
			{
				asCObjectType *objType = 0;
				asCScriptFunction *impl = FindImplementation(m_func, method, objType);
				if( impl )
					impls.push_back(std::make_pair(impl, objType));
				else
				{
					const SJITSeenClasses *seen = inlining.classes ? inlining.classes->Find(m_func, n) : 0;
					for( asUINT c = 0; seen && c < JIT_PROFILE_CLASSES && seen->types[c]; c++ )
					{
						impl = FindSeenImplementation(m_func, method, seen->types[c], objType);
						if( impl )
							impls.push_back(std::make_pair(impl, objType));
					}
					if( !impls.empty() )
						profiled = inlining.profile;
					else if( inlining.profile && HasSmallImplementation(m_func, method, inlining.maxSize) )
						instr.flags |= JIT_INSTR_PROFILE;
				}
			}
		}
		else if( func )
			impls.push_back(std::make_pair(func, (asCObjectType*)0));

		// The implementations that can't be emitted, or don't fit, are called
		std::vector<SJITInlinee> inlinees;
		asUINT size = 0;
		for( asUINT i = 0; i < impls.size(); i++ )
		{
			asCScriptFunction *impl = impls[i].first;
			asUINT k = 0;
			while( k < inlinees.size() && inlinees[k].code->GetFunction() != impl )
				k++;
			if( k == inlinees.size() )
			{
				if( std::find(search.path.begin(), search.path.end(), impl->GetId()) != search.path.end() )
					continue;
				std::shared_ptr<CJITByteCode> callee = AnalyseInlinee(search, impl, levels);
				asUINT calleeSize = callee ? callee->GetLength() + callee->m_inlinedLength : 0;
				if( !callee || size + calleeSize > budget )
					continue;
				size += calleeSize;
				SJITInlinee inlinee;
				inlinee.code = callee;
				inlinees.push_back(inlinee);
			}
			if( impls[i].second )
				inlinees[k].types.push_back(impls[i].second);
		}
		if( inlinees.empty() )
			continue;

		budget -= size;
		m_inlinedLength += size;
		instr.flags |= JIT_INSTR_INLINE;
		if( profiled )
			instr.flags |= JIT_INSTR_PROFILE;
		m_inlinees[n] = inlinees;
	}
}

void CJITByteCode::Analyse(bool allowRegisterCache, asUINT maxCachedSlots, const SJITInlineOptions *inlining)
{
	// The instructions returning to the VM may have been set since the decoding
	MarkUnreachable(false);
	m_aot = false;
	AnalyseStackDepth();
	m_inlinees.clear();
	m_inlinedLength = 0;
	if( inlining )
	{
		// Up to 4 levels of calls are inlined. The function inlines at most 16 times
		// the size of the largest function to inline
		SInlineSearch search;
		search.allowRegisterCache = allowRegisterCache;
		search.maxCachedSlots     = maxCachedSlots;
		search.options            = inlining;
		search.path.push_back(m_func->GetId());
		FindInlinees(search, 4, inlining->maxSize * 16);
	}
	FindIndexers(inlining ? inlining->indexers : 0);
	AnalyseBorrows();
	AnalyseBody(allowRegisterCache, maxCachedSlots);
}

// The indexer that the function is, if any, see CJITCompiler::AddIndexer. The
// elements are laid out like those of CScriptArray, whose size depends on the
// subtype, and the objects are stored as pointers to them if the subtype isn't a
// handle. The index must be unsigned, as the negative ones are taken to be out of
// range
bool CJITByteCode::FindIndexer(asCScriptEngine *engine, int funcId, const std::map<asFUNCTION_t, SJITIndexer> *indexers, SJITIndexerCall &call)
{
	if( indexers == 0 || indexers->empty() )
		return false;
	asCScriptFunction *func = funcId >= 0 && asUINT(funcId) < engine->scriptFunctions.GetLength() ? engine->scriptFunctions[funcId] : 0;
	if( func == 0 || func->funcType != asFUNC_SYSTEM || func->sysFuncIntf == 0 || func->objectType == 0 )
		return false;
	const asSSystemFunctionInterface *sysFunc = func->sysFuncIntf;
	std::map<asFUNCTION_t, SJITIndexer>::const_iterator it = indexers->find(sysFunc->func);
	if( it == indexers->end() || sysFunc->callConv != ICC_THISCALL || sysFunc->baseOffset || sysFunc->auxiliary ||
	    sysFunc->compositeOffset || sysFunc->isCompositeIndirect )
		return false;
	if( !func->returnType.IsReference() || func->parameterTypes.GetLength() != 1 ||
	    func->parameterTypes[0].IsReference() || func->parameterTypes[0].GetTokenType() != ttUInt ||
	    func->objectType->templateSubTypes.GetLength() != 1 )
		return false;

	int typeId = engine->GetTypeIdFromDataType(func->objectType->templateSubTypes[0]);
	call.layout      = it->second;
	call.indirect    = (typeId & asTYPEID_MASK_OBJECT) && !(typeId & asTYPEID_OBJHANDLE);
	call.elementSize = (typeId & asTYPEID_MASK_OBJECT) ? int(sizeof(asPWORD)) : engine->GetSizeOfPrimitiveType(typeId);
	return call.elementSize == 1 || call.elementSize == 2 || call.elementSize == 4 || call.elementSize == 8;
}

// Finds the calls of the indexers, which are compiled in place
void CJITByteCode::FindIndexers(const std::map<asFUNCTION_t, SJITIndexer> *indexers)
{
	m_indexers.clear();
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		SJITInstr &instr = m_instrs[n];
		instr.flags &= ~JIT_INSTR_INDEXER;
		SJITIndexerCall call;
		if( (instr.op != asBC_CALLSYS && instr.op != asBC_Thiscall1) || (instr.flags & (JIT_INSTR_DEAD | JIT_INSTR_BAIL)) ||
		    !FindIndexer(m_func->engine, asBC_INTARG(instr.bc), indexers, call) )
			continue;
		instr.flags |= JIT_INSTR_INDEXER;
		m_indexers[n] = call;
	}
}

// The depth of the stack depends on the callees, and the borrows on the objects.
// The moved references, those counted in place, and the list frees only depend on
// the kinds of the types, which are part of the key, see GetRefKind
//
// TODO: runtime optimize: Inline the calls, and borrow the references of the handle
//                         arguments, with the bytecode of the callees and the kinds of
//                         the objects in the key. Most of the difference to the JIT
//                         compiled code is in these calls.
void CJITByteCode::AnalyseForAOT(asUINT maxCachedSlots, const std::map<asFUNCTION_t, SJITIndexer> *indexers)
{
	MarkUnreachable(false);
	m_aot = true;
	m_staticStack = false;
	m_stackDepth.assign(m_instrs.size(), -1);
	m_inlinees.clear();
	m_inlinedLength = 0;
	FindIndexers(indexers);
	ClearBorrows();
	FindMovedRefs();
	FindInPlaceRefCounts();
	FindListFrees();
	AnalyseBody(true, maxCachedSlots);
}

// The variables in the operands of an instruction. Returns their number
static int GetVarOperands(const SJITInstr &instr, int vars[3])
{
	switch( asBCInfo[instr.op].type )
	{
	case asBCTYPE_wW_ARG:
	case asBCTYPE_rW_ARG:
	case asBCTYPE_rW_DW_ARG:
	case asBCTYPE_wW_DW_ARG:
	case asBCTYPE_wW_QW_ARG:
	case asBCTYPE_rW_QW_ARG:
	case asBCTYPE_rW_W_DW_ARG:
	case asBCTYPE_rW_DW_DW_ARG:
	case asBCTYPE_wW_W_ARG:
		vars[0] = asBC_SWORDARG0(instr.bc);
		return 1;
	case asBCTYPE_wW_rW_ARG:
	case asBCTYPE_wW_rW_DW_ARG:
	case asBCTYPE_rW_rW_ARG:
		vars[0] = asBC_SWORDARG0(instr.bc);
		vars[1] = asBC_SWORDARG1(instr.bc);
		return 2;
	case asBCTYPE_wW_rW_rW_ARG:
		vars[0] = asBC_SWORDARG0(instr.bc);
		vars[1] = asBC_SWORDARG1(instr.bc);
		vars[2] = asBC_SWORDARG2(instr.bc);
		return 3;
	case asBCTYPE_W_rW_ARG:
		vars[0] = asBC_SWORDARG1(instr.bc);
		return 1;
	default:
		return 0;
	}
}

static bool UsesVar(const SJITInstr &instr, int var)
{
	int vars[3];
	int count = GetVarOperands(instr, vars);
	for( int n = 0; n < count; n++ )
		if( vars[n] == var )
			return true;
	return false;
}

// The reference types whose references are counted with the behaviours
static bool IsCountedRef(asCTypeInfo *type)
{
	asCObjectType *ot = CastToObjectType(type);
	return ot && (ot->flags & asOBJ_REF) && !(ot->flags & asOBJ_NOCOUNT) && ot->beh.addref && ot->beh.release;
}

// The initialization lists whose elements are primitives, enums, or value types
// without a destructor have nothing to destroy
static bool IsPlainList(asCScriptEngine *engine, asCObjectType *listType)
{
	if( !listType || !(listType->flags & asOBJ_LIST_PATTERN) || listType->beh.destruct || listType->templateSubTypes.GetLength() == 0 )
		return false;
	asCObjectType *type = CastToObjectType(listType->templateSubTypes[0].GetTypeInfo());
	int factory = type ? type->beh.listFactory : 0;
	if( factory <= 0 || asUINT(factory) >= engine->scriptFunctions.GetLength() || !engine->scriptFunctions[factory] )
		return false;

	for( asSListPatternNode *node = engine->scriptFunctions[factory]->listPattern; node; node = node->next )
	{
		if( node->type != asLPT_TYPE )
			continue;
		const asCDataType &dt = static_cast<asSListPatternDataTypeNode*>(node)->dataType;
		asCTypeInfo *ti = dt.GetTypeInfo();
		if( dt.GetTokenType() == ttQuestion )
			return false;
		if( ti && !(ti->flags & asOBJ_ENUM) )
		{
			asCObjectType *ot = CastToObjectType(ti);
			if( !ot || !(ot->flags & asOBJ_VALUE) || ot->beh.destruct )
				return false;
		}
	}
	return true;
}

EJITRefKind CJITByteCode::GetRefKind(asCScriptEngine *engine, asCTypeInfo *ti)
{
	asCObjectType *type = CastToObjectType(ti);
	if( IsPlainList(engine, type) )
		return JIT_REF_PLAIN_LIST;
	if( !IsCountedRef(type) )
		return JIT_REF_OTHER;
	const asSTypeBehaviour &beh = engine->scriptTypeBehaviours.beh;
	if( (type->flags & asOBJ_SCRIPT_OBJECT) && type->beh.addref == beh.addref && type->beh.release == beh.release )
		return JIT_REF_SCRIPT_OBJECT;
	return JIT_REF_COUNTED;
}

// The calls copy the handles that they pass, which adds a reference that the called
// function releases when it returns. An inlined function that only reads a handle
// parameter can borrow the reference of the variable that the caller copies it
// from instead, if the variable holds its own reference and isn't modified until
// the call has returned, which keeps the object alive. Neither is the reference
// added nor released then. The frames of the inlined functions that are handed to
// the VM get references of their own, see CJITCodeGen::EmitInlineExit and
// JIT_OwnBorrowed, for which the call states of the materialized frames note the
// borrowed parameters in the upper half of the stack index. Only 64bit hosts have
// room there
void CJITByteCode::AnalyseBorrows()
{
	ClearBorrows();
#ifdef JIT_NATIVE_RETURN
	if( m_staticStack )
	{
		FindBorrowableParams();
		FindBorrowedArgs();
	}
#endif
	FindMovedRefs();
	FindInPlaceRefCounts();
	FindListFrees();
}

void CJITByteCode::ClearBorrows()
{
	for( asUINT n = 0; n < m_instrs.size(); n++ )
		m_instrs[n].flags &= ~(JIT_INSTR_BORROW | JIT_INSTR_MOVE | JIT_INSTR_MOVED | JIT_INSTR_REFCOUNT | JIT_INSTR_FREE_LIST);
	m_borrowableParams = 0;
	m_releasedParams   = 0;
	m_borrowedArgs.clear();
	m_borrowChecks.clear();
}

// The handles copied from variables that are released right after the copy, e.g.
// the temporary variables holding the results of expressions, are moved instead:
//
//   PshVPtr vT; RefCpyV vD; PopPtr; FREE vT
//
// The copy doesn't add a reference to the object then, and the release of vT only
// clears it. The copy still releases the old object of vD first, like the VM. The
// handle on the stack is the one in vT, as nothing is in between that could enter
// the code or return to the VM, which would release vT. The code generated ahead
// of time only knows the kinds of the types, see GetRefKind, and moves the
// references between the types of the same kind, e.g. from a script class to its
// base class
void CJITByteCode::FindMovedRefs()
{
	asCScriptEngine *engine = m_func->engine;
	for( asUINT n = 1; n + 1 < m_instrs.size(); n++ )
	{
		SJITInstr &copy = m_instrs[n];
		const SJITInstr &push = m_instrs[n - 1];
		if( copy.op != asBC_RefCpyV || push.op != asBC_PshVPtr || (copy.flags & JIT_INSTR_BORROW) )
			continue;
		asUINT f = n + 1;
		if( m_instrs[f].op == asBC_PopPtr && f + 1 < m_instrs.size() )
			f++;
		SJITInstr &release = m_instrs[f];

		// The parameters may borrow the references of the callers
		int temp = asBC_SWORDARG0(push.bc);
		asCObjectType *type = reinterpret_cast<asCObjectType*>(asBC_PTRARG(copy.bc));
		if( release.op != asBC_FREE || asBC_SWORDARG0(release.bc) != temp || temp == asBC_SWORDARG0(copy.bc) || temp <= 0 || !IsCountedRef(type) )
			continue;
		asCObjectType *freed = reinterpret_cast<asCObjectType*>(asBC_PTRARG(release.bc));
		if( m_aot ? GetRefKind(engine, freed) != GetRefKind(engine, type) : freed != type )
			continue;

		bool plain = !(push.flags & (JIT_INSTR_BAIL | JIT_INSTR_DEAD));
		for( asUINT k = n; k <= f && plain; k++ )
			plain = !(m_instrs[k].flags & (JIT_INSTR_BLOCK_START | JIT_INSTR_ENTRY | JIT_INSTR_BAIL | JIT_INSTR_DEAD));
		if( !plain )
			continue;

		copy.flags    |= JIT_INSTR_MOVE;
		release.flags |= JIT_INSTR_MOVED;
	}
}

// The script objects count their references like the generated code can do in
// place of calling asCScriptObject::AddRef and Release: the flag of the GC is
// cleared, and the counter is incremented or decremented. AddRef is only called
// for the objects that are being destroyed, to report the error, and Release for
// the last reference, which destroys the object. The copies of the handles aren't
// sync points then, as the cached variables are only stored on the rare paths. The
// code generated ahead of time counts them in place on every host, see AOT_AddRef
void CJITByteCode::FindInPlaceRefCounts()
{
#ifndef JIT_INPLACE_REFCOUNT
	if( !m_aot )
		return;
#endif
	asCScriptEngine *engine = m_func->engine;
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		SJITInstr &instr = m_instrs[n];
		if( (instr.op != asBC_FREE && instr.op != asBC_REFCPY && instr.op != asBC_RefCpyV) ||
		    (instr.flags & (JIT_INSTR_BAIL | JIT_INSTR_DEAD | JIT_INSTR_BORROW | JIT_INSTR_MOVED)) )
			continue;
		if( GetRefKind(engine, reinterpret_cast<asCTypeInfo*>(asBC_PTRARG(instr.bc))) == JIT_REF_SCRIPT_OBJECT )
			instr.flags |= JIT_INSTR_REFCOUNT;
	}
}

// The initialization lists that have nothing to destroy, see IsPlainList. Their
// memory is freed right away, instead of asCScriptEngine::DestroyList going through
// the list pattern for each element, and as nothing is executed the VM registers
// aren't synced for it
void CJITByteCode::FindListFrees()
{
	asCScriptEngine *engine = m_func->engine;
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		SJITInstr &instr = m_instrs[n];
		if( instr.op == asBC_FREE && !(instr.flags & (JIT_INSTR_BAIL | JIT_INSTR_DEAD)) &&
		    GetRefKind(engine, reinterpret_cast<asCTypeInfo*>(asBC_PTRARG(instr.bc))) == JIT_REF_PLAIN_LIST )
			instr.flags |= JIT_INSTR_FREE_LIST;
	}
}

// The handle parameters that the function doesn't store or hand over anywhere, but
// only reads, passes as references or objects, and releases. The functions that
// release the parameters on exceptions themselves are left out
void CJITByteCode::FindBorrowableParams()
{
	if( m_func->dontCleanUpOnException )
		return;
	int var = -((m_func->objectType ? AS_PTR_SIZE : 0) + (m_func->DoesReturnOnStack() ? AS_PTR_SIZE : 0));
	for( asUINT p = 0; p < m_func->parameterTypes.GetLength(); var -= m_func->parameterTypes[p].GetSizeOnStackDWords(), p++ )
	{
		const asCDataType &type = m_func->parameterTypes[p];
		if( p >= 31 || !type.IsObjectHandle() || type.IsReference() || !IsCountedRef(type.GetTypeInfo()) )
			continue;

		bool borrowable = true, released = false;
		for( asUINT n = 0; n < m_instrs.size() && borrowable; n++ )
		{
			const SJITInstr &instr = m_instrs[n];
			if( (instr.flags & JIT_INSTR_DEAD) || !UsesVar(instr, var) )
				continue;
			switch( instr.op )
			{
			case asBC_PshVPtr:
			case asBC_ChkNullV:
			case asBC_LoadRObjR:
			case asBC_CmpPtr:
				break;
			case asBC_FREE:
				released = true;
				break;
			case asBC_VAR:
			{
				int consumer = FindVarConsumer(n);
				borrowable = consumer >= 0 && m_instrs[consumer].op == asBC_GETOBJREF;
				break;
			}
			default:
				borrowable = false;
				break;
			}
		}
		if( borrowable )
		{
			m_borrowableParams |= 1u << p;
			if( released )
				m_releasedParams |= 1u << p;
		}
	}
}

// The inlined calls whose handle arguments can borrow the references of the caller,
// see AnalyseBorrows. The compiler copies each handle argument into a temporary
// variable, which is moved into the stack slot of the argument before the call:
//
//   PshVPtr vX; RefCpyV vT; PopPtr; ... VAR vT; ... GETOBJ; ... CALL
//
// The copy lends the reference of vX if vX holds one of its own, which nothing
// modifies up to the call, and vT isn't used but by the copy and the move. Nothing
// in between may return to the VM, which would release vT, or enter the code, so
// only the arguments may be pushed. The copies must not release anything, i.e. the
// temporary variables must be null, which the first of the copies checks for all
void CJITByteCode::FindBorrowedArgs()
{
	struct SCandidate
	{
		asUINT param;
		int    var;    // the asBC_VAR of the temporary variable, after the copy
		int    temp;
		int    source;
		bool   live;
	};

	std::vector<SCandidate> found;
	for( std::map<asUINT, std::vector<SJITInlinee> >::iterator it = m_inlinees.begin(); it != m_inlinees.end(); ++it )
	{
		asUINT call = it->first;
		const std::vector<SJITInlinee> &inlinees = it->second;
		// The implementations of a method have the same parameters, and an argument is
		// borrowed if all of them can
		asCScriptFunction *func = inlinees[0].code->GetFunction();
		asUINT params = ~asUINT(0);
		for( asUINT i = 0; i < inlinees.size(); i++ )
			params &= inlinees[i].code->GetBorrowableParams();
		found.clear();

		// The argument for the parameter at k dwords above the frame of the function
		// has its top at the depth of the stack at the call minus k
		int k = (func->objectType ? AS_PTR_SIZE : 0) + (func->DoesReturnOnStack() ? AS_PTR_SIZE : 0);
		for( asUINT p = 0; p < func->parameterTypes.GetLength(); k += func->parameterTypes[p].GetSizeOnStackDWords(), p++ )
		{
			if( p >= 31 || !((params >> p) & 1) )
				continue;
			int top = m_stackDepth[call] - k;
			int i = int(call) - 1;
			while( i >= 0 && m_stackDepth[i] >= top )
				i--;
			if( i < 3 || m_instrs[i].op != asBC_VAR || m_stackDepth[i] + AS_PTR_SIZE != top )
				continue;

			const SJITInstr &push = m_instrs[i - 3];
			const SJITInstr &copy = m_instrs[i - 2];
			int temp = asBC_SWORDARG0(m_instrs[i].bc);
			if( push.op != asBC_PshVPtr || copy.op != asBC_RefCpyV || m_instrs[i - 1].op != asBC_PopPtr ||
			    asBC_SWORDARG0(copy.bc) != temp || asBC_SWORDARG0(push.bc) == temp )
				continue;
			asCObjectType *type = reinterpret_cast<asCObjectType*>(asBC_PTRARG(copy.bc));
			if( type != func->parameterTypes[p].GetTypeInfo() || !IsCountedRef(type) || !HoldsReference(asBC_SWORDARG0(push.bc)) )
				continue;

			bool plain = true;
			for( asUINT n = asUINT(i - 2); n <= call && plain; n++ )
				plain = !(m_instrs[n].flags & (JIT_INSTR_BLOCK_START | JIT_INSTR_ENTRY | JIT_INSTR_BAIL | JIT_INSTR_DEAD));
			if( !plain )
				continue;

			SCandidate cand;
			cand.param  = p;
			cand.var    = i;
			cand.temp   = temp;
			cand.source = asBC_SWORDARG0(push.bc);
			cand.live   = true;
			found.push_back(cand);
		}

		// The copies of the others may be between the copy and the call. They are
		// checked again when one of them is dropped, down to the first of them
		int first = int(call);
		bool changed = !found.empty();
		while( changed )
		{
			changed = false;
			first = int(call);
			for( asUINT c = 0; c < found.size(); c++ )
				if( found[c].live && found[c].var - 2 < first )
					first = found[c].var - 2;

			for( asUINT c = 0; c < found.size(); c++ )
			{
				SCandidate &cand = found[c];
				if( !cand.live )
					continue;
				int top = m_stackDepth[cand.var] + AS_PTR_SIZE;
				int moves = 0;
				bool ok = true;
				for( int n = first; n < int(call) && ok; n++ )
				{
					const SJITInstr &instr = m_instrs[n];
					if( n != cand.var - 2 && n != cand.var && UsesVar(instr, cand.temp) )
						ok = false;
					if( n <= cand.var || !ok )
						continue;
					if( instr.op != asBC_PshVPtr && UsesVar(instr, cand.source) )
						ok = false;

					switch( instr.op )
					{
					case asBC_PshVPtr:
					case asBC_PopPtr:
					case asBC_VAR:
					case asBC_PshC4:
					case asBC_PshC8:
					case asBC_PshV4:
					case asBC_PshV8:
					case asBC_PshNull:
					case asBC_PSF:
					case asBC_PGA:
					case asBC_PshG4:
					case asBC_PshGPtr:
						break;
					case asBC_GETREF:
					case asBC_GETOBJREF:
						if( m_stackDepth[n] - int(asBC_WORDARG0(instr.bc)) == top )
							ok = false;
						break;
					case asBC_GETOBJ:
					{
						// Other variables may be moved onto the stack, but not the source
						int target = m_stackDepth[n] - int(asBC_WORDARG0(instr.bc));
						if( target == top )
							moves++;
						else
						{
							int pusher = FindPush(asUINT(n), target);
							ok = ok && pusher >= 0 && m_instrs[pusher].op == asBC_VAR && m_stackDepth[pusher] + AS_PTR_SIZE == target &&
							     asBC_SWORDARG0(m_instrs[pusher].bc) != cand.source;
						}
						break;
					}
					case asBC_RefCpyV:
					{
						bool other = false;
						for( asUINT o = 0; o < found.size() && !other; o++ )
							other = found[o].live && found[o].var - 2 == n;
						ok = ok && other;
						break;
					}
					default:
						ok = false;
						break;
					}
				}
				if( !ok || moves != 1 )
				{
					cand.live = false;
					changed = true;
				}
			}
		}

		std::vector<int> checks;
		for( asUINT c = 0; c < found.size(); c++ )
		{
			if( !found[c].live )
				continue;
			m_instrs[found[c].var - 2].flags |= JIT_INSTR_BORROW;
			m_borrowedArgs[call] |= 1u << found[c].param;
			checks.push_back(found[c].temp);
		}
		if( !checks.empty() )
			m_borrowChecks[asUINT(first)] = checks;
	}
}

// Returns the instruction that replaces the variable pushed by the asBC_VAR with
// the object, its address, or the object reference, or -1 if it isn't in the block
int CJITByteCode::FindVarConsumer(asUINT idx) const
{
	int top = m_stackDepth[idx] + AS_PTR_SIZE;
	for( asUINT n = idx + 1; n < m_instrs.size(); n++ )
	{
		const SJITInstr &instr = m_instrs[n];
		if( (instr.flags & (JIT_INSTR_BLOCK_START | JIT_INSTR_DEAD)) || m_stackDepth[n] < top )
			return -1;
		if( (instr.op == asBC_GETOBJ || instr.op == asBC_GETOBJREF || instr.op == asBC_GETREF) &&
		    m_stackDepth[n] - int(asBC_WORDARG0(instr.bc)) == top )
			return int(n);
	}
	return -1;
}

// Returns the instruction that pushed the stack slot with the top at the depth
// where the instruction is reached, or -1 if it isn't in the block
int CJITByteCode::FindPush(asUINT idx, int top) const
{
	for( int n = int(idx); n >= 0; n-- )
	{
		if( m_stackDepth[n] < top )
			return n;
		if( m_instrs[n].flags & JIT_INSTR_BLOCK_START )
			return -1;
	}
	return -1;
}

// Returns true if the variable holds a reference of its own that the function
// releases, i.e. it is a handle or an object on the heap, or a handle parameter,
// and nothing but the instructions using it can modify it, as its address is never
// taken
bool CJITByteCode::HoldsReference(int var) const
{
	if( var > 0 )
	{
		const asCArray<asSScriptVariable*> &vars = m_func->scriptData->variables;
		bool found = false;
		for( asUINT n = 0; n < vars.GetLength(); n++ )
		{
			if( vars[n]->stackOffset != var )
				continue;
			if( !vars[n]->onHeap || vars[n]->type.IsReference() || !IsCountedRef(vars[n]->type.GetTypeInfo()) )
				return false;
			found = true;
		}
		if( !found )
			return false;
	}
	else
	{
		int p = FindParam(var);
		if( p < 0 || m_func->dontCleanUpOnException )
			return false;
		const asCDataType &type = m_func->parameterTypes[p];
		if( type.IsReference() || !IsCountedRef(type.GetTypeInfo()) )
			return false;
	}

	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		const SJITInstr &instr = m_instrs[n];
		if( (instr.flags & JIT_INSTR_DEAD) || !UsesVar(instr, var) )
			continue;
		if( instr.op == asBC_PSF || instr.op == asBC_LDV )
			return false;
		if( instr.op == asBC_VAR )
		{
			int consumer = FindVarConsumer(n);
			if( consumer < 0 || m_instrs[consumer].op == asBC_GETREF )
				return false;
		}
	}
	return true;
}

void CJITByteCode::AnalyseBody(bool allowRegisterCache, asUINT maxCachedSlots)
{
	// The functions inlined into this one have been analysed first. The releases of
	// objects may execute script destructors
	m_hasSyncPoints = false;
	for( asUINT n = 0; n < m_instrs.size() && !m_hasSyncPoints; n++ )
	{
		const SJITInstr &instr = m_instrs[n];
		if( instr.flags & JIT_INSTR_DEAD )
			continue;
		if( instr.flags & JIT_INSTR_INLINE )
			m_hasSyncPoints = InlineesHaveSyncPoints(n);
		else if( instr.flags & (JIT_INSTR_MOVED | JIT_INSTR_FREE_LIST) )
			continue;
		else if( instr.op == asBC_FREE )
		{
			// The parameters that may borrow references are left to HasSyncPoints
			int p = FindParam(asBC_SWORDARG0(instr.bc));
			m_hasSyncPoints = p < 0 || p >= 31 || !((m_releasedParams >> p) & 1);
		}
		else if( instr.flags & JIT_INSTR_REFCOUNT )
		{
			// The rare paths call AddRef and Release
			m_hasSyncPoints = true;
		}
		else
			m_hasSyncPoints = IsSyncPointAt(n);
	}

	BuildBlocks();
	AnalyseVRLiveness();
	AnalyseSlots(allowRegisterCache, maxCachedSlots);
	AnalyseSlotLiveness();
	AnalyseDirtySlots();
	AnalyseThisFields();
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
// script classes. The inlined functions with sync points leave their own frame in
// the VM registers, see CJITCodeGen::EmitDematerialize. The code generated ahead of
// time restores the frame after the calls, but those of the functions it calls
// directly, see CJITCppGen::EmitCall, which include the constructors of any type
// allocated, as the type isn't part of the key
bool CJITByteCode::LeavesFrameDirty(asUINT instrIdx) const
{
	const SJITInstr &instr = m_instrs[instrIdx];
	if( m_aot )
		return instr.op == asBC_CALL || instr.op == asBC_CALLINTF || instr.op == asBC_ALLOC;
	if( (instr.flags & JIT_INSTR_INLINE) && InlineesHaveSyncPoints(instrIdx) )
		return true;
#ifdef JIT_NATIVE_RETURN
	asEBCInstr op = instr.op;
	if( op == asBC_ALLOC )
		return (reinterpret_cast<asCObjectType*>(asBC_PTRARG(instr.bc))->flags & asOBJ_SCRIPT_OBJECT) != 0;
	return op == asBC_CALL || op == asBC_CALLINTF || (op == asBC_CallPtr && m_staticStack);
#else
	return false;
#endif
}

// The variables that won't be read anymore may still be stored, if they aren't
// temporary variables, see AnalyseDirtySlots. Where the VM has entered their
// register hasn't been loaded, and where the VM may have modified them it holds the
// old value, so they are loaded like the live ones. The VM has them in memory there
asUINT CJITByteCode::GetEntryMask(asUINT instrIdx) const
{
	return m_liveIn[m_instrs[instrIdx].block] | (m_dirty[instrIdx] & ~JIT_FRAME_BIT);
}

// The sync points store the dirty variables and leave none, but the inlined calls,
// whose rare path stores them, and SUSPEND leave them dirty
asUINT CJITByteCode::GetReloadMask(asUINT instrIdx) const
{
	const SJITInstr &instr = m_instrs[instrIdx];
	asUINT mask = m_liveAfter[instrIdx];
	if( !IsSyncPointAt(instrIdx) || (instr.flags & JIT_INSTR_INLINE) )
		mask |= m_dirty[instrIdx] & ~JIT_FRAME_BIT;
	return mask;
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
	// that are only modified before the loop, and so do the inlined functions that
	// call functions. The variables that a loop with calls
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
			if( LeavesFrameDirty(k) )
				written |= JIT_FRAME_BIT;
			if( m_instrs[k].flags & JIT_INSTR_INLINE )
				calls = calls || InlineesHaveSyncPoints(k);
			else
				calls = calls || IsSyncPointAt(k);
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
				// calls the function, and where the inlined function calls functions,
				// which may leave the frame dirty too
				if( IsSyncPointAt(n) && !(instr.flags & JIT_INSTR_INLINE) )
					mask = LeavesFrameDirty(n) ? JIT_FRAME_BIT : 0;
				else
				{
					if( LeavesFrameDirty(n) )
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
		s.floatUses = 0;
		s.intUses   = 0;
		s.cacheKind = JIT_SLOT_NONE;
		s.cacheBit  = -1;
		m_slotIndex[offset] = int(m_slots.size());
		m_slots.push_back(s);
		slot = &m_slots.back();
	}
	slot->kinds |= kind;
	slot->useCount++;
	if( kind == JIT_SLOT_F32 || kind == JIT_SLOT_F64 )
		slot->floatUses++;
	else if( kind == JIT_SLOT_I32 || kind == JIT_SLOT_I64 )
		slot->intUses++;
}

void CJITByteCode::CollectSlotUses(const SJITInstr &instr)
{
	// Decode an operand only when the opcode below actually uses it, so
	// one-dword instructions aren't read past their end.
	struct SLazyWordArg
	{
		const SJITInstr &instr;
		int offset;
		operator int() const
		{
			asASSERT(offset == 1 || instr.size > 1);
			return *(reinterpret_cast<const short*>(instr.bc) + offset);
		}
	};
	SLazyWordArg a0 = { instr, 1 };
	SLazyWordArg a1 = { instr, 2 };
	SLazyWordArg a2 = { instr, 3 };

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
	m_thisConstant = false;

	for( asUINT n = 0; n < m_instrs.size(); n++ )
		CollectSlotUses(m_instrs[n]);

	if( !allowRegisterCache )
		return;

	// asBC_LoadThisR reads the object pointer without being a use of the variable.
	// The variable holds the same pointer throughout the function if the other
	// instructions only read it, and its address is never taken
	const SJITSlot *self = FindSlot(0);
	m_thisConstant = m_func->objectType != 0 && (!self || self->kinds == JIT_SLOT_PTR);
	bool readsThis = false;
	for( asUINT n = 0; n < m_instrs.size() && m_thisConstant; n++ )
	{
		switch( m_instrs[n].op )
		{
		case asBC_LoadThisR:
			readsThis = true;
			break;
		case asBC_ClrVPtr:
		case asBC_FREE:
		case asBC_LOADOBJ:
		case asBC_STOREOBJ:
		case asBC_RefCpyV:
		case asBC_AllocMem:
			if( asBC_SWORDARG0(m_instrs[n].bc) == 0 )
				m_thisConstant = false;
			break;
		default:
			break;
		}
	}
	m_thisConstant = m_thisConstant && readsThis;

	// Determine which slots hold primitive values of one size and are never accessed
	// through their address. Only those can be kept in registers. The temporary
	// variables are reused for values of other types, and the conversions are done
	// in place, so a slot may hold both integers and floats of the same size. It is
	// kept in the register of the kind that most of the operations use then, and the
	// others move the bits between the registers, see CJITCodeGen::Load32
	for( asUINT n = 0; n < m_slots.size(); n++ )
	{
		SJITSlot &slot = m_slots[n];
		asUINT kinds = slot.kinds;
		asUINT typed = kinds & (JIT_SLOT_I32 | JIT_SLOT_I64 | JIT_SLOT_F32 | JIT_SLOT_F64);

		slot.cacheKind = JIT_SLOT_NONE;
		if( kinds & (JIT_SLOT_PTR | JIT_SLOT_ADDR) )
			continue;

		if( typed == (JIT_SLOT_I32 | JIT_SLOT_F32) )
			typed = slot.floatUses > slot.intUses ? JIT_SLOT_F32 : JIT_SLOT_I32;
		else if( typed == (JIT_SLOT_I64 | JIT_SLOT_F64) )
		{
			// A 64bit integer can't be held in a register on 32bit hosts, where the
			// integer operations read the slot in memory
			if( sizeof(void*) < 8 )
				continue;
			typed = slot.floatUses > slot.intUses ? JIT_SLOT_F64 : JIT_SLOT_I64;
		}

		// Values of different sizes
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

// The instructions that don't modify the memory of objects, across which the object
// pointer is held in its register too. asBC_SUSPEND reloads the fields and the
// pointer after the line callback or the suspension, see CJITCodeGen::EmitMisc
static bool KeepsFields(asEBCInstr op)
{
	switch( op )
	{
	case asBC_RDR1: case asBC_RDR2: case asBC_RDR4: case asBC_RDR8:
	case asBC_JitEntry:
	case asBC_JMP: case asBC_JZ: case asBC_JNZ: case asBC_JS: case asBC_JNS: case asBC_JP: case asBC_JNP:
	case asBC_JLowZ: case asBC_JLowNZ: case asBC_JMPP:
	case asBC_TZ: case asBC_TNZ: case asBC_TS: case asBC_TNS: case asBC_TP: case asBC_TNP:
	case asBC_CMPi: case asBC_CMPu: case asBC_CMPf: case asBC_CMPd: case asBC_CMPi64: case asBC_CMPu64:
	case asBC_CMPIi: case asBC_CMPIu: case asBC_CMPIf: case asBC_CmpPtr:
	case asBC_NOT: case asBC_ClrHi: case asBC_IncVi: case asBC_DecVi:
	case asBC_NEGi: case asBC_NEGf: case asBC_NEGd: case asBC_NEGi64:
	case asBC_BNOT: case asBC_BNOT64:
	case asBC_ADDi: case asBC_SUBi: case asBC_MULi: case asBC_DIVi: case asBC_MODi: case asBC_DIVu: case asBC_MODu:
	case asBC_ADDIi: case asBC_SUBIi: case asBC_MULIi:
	case asBC_BAND: case asBC_BOR: case asBC_BXOR: case asBC_BSLL: case asBC_BSRL: case asBC_BSRA:
	case asBC_ADDi64: case asBC_SUBi64: case asBC_MULi64: case asBC_DIVi64: case asBC_MODi64: case asBC_DIVu64: case asBC_MODu64:
	case asBC_BAND64: case asBC_BOR64: case asBC_BXOR64: case asBC_BSLL64: case asBC_BSRL64: case asBC_BSRA64:
	case asBC_ADDf: case asBC_SUBf: case asBC_MULf: case asBC_DIVf: case asBC_MODf:
	case asBC_ADDIf: case asBC_SUBIf: case asBC_MULIf:
	case asBC_ADDd: case asBC_SUBd: case asBC_MULd: case asBC_DIVd: case asBC_MODd:
	case asBC_iTOb: case asBC_iTOw: case asBC_sbTOi: case asBC_swTOi: case asBC_ubTOi: case asBC_uwTOi:
	case asBC_iTOf: case asBC_fTOi: case asBC_uTOf: case asBC_fTOu: case asBC_dTOi: case asBC_dTOu: case asBC_dTOf:
	case asBC_iTOd: case asBC_uTOd: case asBC_fTOd: case asBC_i64TOi: case asBC_uTOi64: case asBC_iTOi64:
	case asBC_fTOi64: case asBC_fTOu64: case asBC_dTOi64: case asBC_dTOu64: case asBC_i64TOf: case asBC_u64TOf:
	case asBC_i64TOd: case asBC_u64TOd:
	case asBC_SetV1: case asBC_SetV2: case asBC_SetV4: case asBC_SetV8:
	case asBC_CpyVtoV4: case asBC_CpyVtoV8: case asBC_CpyVtoR4: case asBC_CpyVtoR8: case asBC_CpyRtoV4: case asBC_CpyRtoV8:
	case asBC_CpyGtoV4: case asBC_LdGRdR4: case asBC_CpyVtoG4: case asBC_SetG4:
	case asBC_LoadThisR: case asBC_LoadRObjR: case asBC_LoadVObjR: case asBC_SUSPEND:
		return true;
	default:
		return false;
	}
}

// The bytes that an instruction reads or writes where the value register points
static bool GetVRAccess(asEBCInstr op, int &size, bool &writes)
{
	writes = true;
	switch( op )
	{
	case asBC_RDR1: writes = false; size = 1; return true;
	case asBC_RDR2: writes = false; size = 2; return true;
	case asBC_RDR4: writes = false; size = 4; return true;
	case asBC_RDR8: writes = false; size = 8; return true;
	case asBC_WRTV1: case asBC_INCi8: case asBC_DECi8: size = 1; return true;
	case asBC_WRTV2: case asBC_INCi16: case asBC_DECi16: size = 2; return true;
	case asBC_WRTV4: case asBC_INCi: case asBC_DECi: case asBC_INCf: case asBC_DECf: size = 4; return true;
	case asBC_WRTV8: case asBC_INCi64: case asBC_DECi64: case asBC_INCd: case asBC_DECd: size = 8; return true;
	default: return false;
	}
}

// Forward data flow over the blocks to find out which fields of the object hold
// the value that the function has last read or written when an instruction is
// reached. They can be read from the register then. Anything that may modify an
// object invalidates them all: the calls and the writes through other pointers.
// The writes through the object pointer invalidate the fields they overlap. Like
// the variables, the fields aren't reloaded for the other threads, which would
// have to synchronize with the context anyway, but only where the line callback
// or the debugger may have modified them. The object pointer is held from
// asBC_LoadThisR until the next instruction that may call
void CJITByteCode::AnalyseThisFields()
{
	m_fields.clear();
	m_fieldAccess.clear();
	m_fieldMask.clear();
	if( !m_thisConstant || m_instrs.empty() )
		return;

	// The accesses through the value register set by asBC_LoadThisR before in the
	// block, like the read and the write of a compound assignment. The instructions
	// in between leave the value register and the object alone
	const int none = INT_MIN;
	std::vector<int> thisOffset(m_instrs.size(), none);
	std::vector<asUINT> accesses;
	m_fieldAccess.assign(m_instrs.size(), -1);
	int vrOffset = none;
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		const SJITInstr &instr = m_instrs[n];
		const asUINT skipped = JIT_INSTR_BAIL | JIT_INSTR_SKIP;
		if( instr.flags & JIT_INSTR_DEAD )
			continue;
		if( n > 0 && instr.block != m_instrs[n - 1].block )
			vrOffset = none;
		if( instr.flags & skipped )
		{
			vrOffset = none;
			continue;
		}
		int size;
		bool writes;
		thisOffset[n] = vrOffset;
		if( instr.op == asBC_LoadThisR )
			vrOffset = asBC_SWORDARG0(instr.bc);
		else if( !GetVRAccess(instr.op, size, writes) && (WritesVR(instr.op) || !KeepsFields(instr.op)) )
			vrOffset = none;
		if( thisOffset[n] == none )
			continue;
		bool incFloat = instr.op == asBC_INCf || instr.op == asBC_DECf;
		if( instr.op != asBC_RDR4 && instr.op != asBC_WRTV4 && instr.op != asBC_INCi && instr.op != asBC_DECi && !incFloat )
			continue;

		asUINT f = 0;
		while( f < m_fields.size() && m_fields[f].offset != thisOffset[n] )
			f++;
		if( f == m_fields.size() )
		{
			if( f == 31 )
				continue;
			SJITField field = { thisOffset[n], JIT_SLOT_I32, 0, 0, false };
			m_fields.push_back(field);
			accesses.push_back(0);
		}
		m_fieldAccess[n] = int(f);
		accesses[f]++;
		if( incFloat || ((instr.op == asBC_RDR4 || instr.op == asBC_WRTV4) && GetCacheKind(asBC_SWORDARG0(instr.bc)) == JIT_SLOT_F32) )
			m_fields[f].floatUses++;
	}

	// The function may be entered at the first instruction through a loop too,
	// where only the object pointer is held. Where the VM enters, the entry paths
	// load the pointer and the entry stubs the fields, so only the predecessors matter
	std::vector<asUINT> succ;
	std::vector<bool> reached(m_blocks.size(), false);
	for( asUINT b = 0; b < m_blocks.size(); b++ )
	{
		GetSuccessors(b, succ);
		for( asUINT k = 0; k < succ.size(); k++ )
			reached[succ[k]] = true;
	}
	asUINT first = asUINT(m_instrs[0].block);
	std::vector<asUINT> in(m_blocks.size(), 0);
	for( asUINT b = 0; b < m_blocks.size(); b++ )
		in[b] = reached[b] && b != first ? ~asUINT(0) : b == first ? JIT_THIS_HELD : 0;

	m_fieldMask.assign(m_instrs.size(), 0);
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
				const SJITInstr &instr = m_instrs[n];
				m_fieldMask[n] = mask;
				int size;
				bool writes;
				if( instr.flags & JIT_INSTR_DEAD )
					continue;
				if( instr.flags & JIT_INSTR_BAIL )
					mask = 0;
				else if( GetVRAccess(instr.op, size, writes) && thisOffset[n] != none )
				{
					for( asUINT f = 0; f < m_fields.size() && writes; f++ )
						if( m_fields[f].offset < thisOffset[n] + size && thisOffset[n] < m_fields[f].offset + 4 )
							mask &= ~(1u << f);
					if( m_fieldAccess[n] >= 0 )
						mask |= 1u << m_fieldAccess[n];
				}
				else if( !KeepsFields(instr.op) )
					mask = 0;
				else if( instr.op == asBC_LoadThisR )
					mask |= JIT_THIS_HELD;
			}

			GetSuccessors(b, succ);
			for( asUINT k = 0; k < succ.size(); k++ )
			{
				if( (in[succ[k]] & mask) != in[succ[k]] )
				{
					in[succ[k]] &= mask;
					changed = true;
				}
			}
		}
	}

	// Only the fields that are read again are kept. Each takes a register, so
	// those read the most are kept
	std::vector<asUINT> order;
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		int f = m_fieldAccess[n];
		if( f >= 0 && m_instrs[n].op != asBC_WRTV4 && ((m_fieldMask[n] >> f) & 1) )
			m_fields[f].forwarded++;
	}
	// The register is chosen by the type of the property, or else by the variables
	// the value is moved between
	const asCArray<asCObjectProperty*> &props = m_func->objectType->properties;
	for( asUINT f = 0; f < m_fields.size(); f++ )
	{
		m_fields[f].kind = m_fields[f].floatUses * 2 > accesses[f] ? JIT_SLOT_F32 : JIT_SLOT_I32;
		for( asUINT p = 0; p < props.GetLength(); p++ )
		{
			const asCObjectProperty *prop = props[p];
			if( prop->byteOffset != m_fields[f].offset || prop->compositeOffset != 0 || prop->isCompositeIndirect ||
				!prop->type.IsPrimitive() || prop->type.IsReference() || prop->type.GetSizeInMemoryBytes() != 4 )
				continue;
			m_fields[f].kind = prop->type.IsFloatType() ? JIT_SLOT_F32 : JIT_SLOT_I32;
			break;
		}
		if( m_fields[f].forwarded )
			order.push_back(f);
	}
	std::stable_sort(order.begin(), order.end(), [this](asUINT a, asUINT b) { return m_fields[a].forwarded > m_fields[b].forwarded; });
	asUINT kept = 0;
	for( asUINT k = 0; k < order.size() && k < 8; k++ )
	{
		m_fields[order[k]].kept = true;
		kept |= 1u << order[k];
	}
	for( asUINT n = 0; n < m_instrs.size(); n++ )
	{
		m_fieldMask[n] &= kept | JIT_THIS_HELD;
		if( m_fieldAccess[n] >= 0 && !m_fields[m_fieldAccess[n]].kept )
			m_fieldAccess[n] = -1;
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
