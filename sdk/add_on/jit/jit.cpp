#include "jit.h"
#include "jit_aot.h"
#include "jit_bytecode.h"
#include "jit_codegen.h"
#include "jit_runtime.h"
#include "jit_unwind.h"

// Internal engine headers. The JIT must be compiled with the same
// configuration as the engine library (see CMakeLists.txt)
#include "as_context.h"
#include "as_scriptobject.h"
#include "as_scriptengine.h"
#include "as_scriptfunction.h"
#include "as_module.h"

#include <asmjit/ujit.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string.h>

// Future work, in rough order of expected benefit. See the TODO comments at the
// respective places in the code for the details.
//
//  - Small, trivial objects passed by value to direct system calls on the hosts besides
//    64bit Windows. Complex and large value objects, which the native ABI passes
//    indirectly, are supported everywhere
//    (jit_codegen_call.cpp, EmitDirectSystemCall, and jit_cppgen.cpp, GetSystemCall),
//    and unwind information on the platforms besides 64bit Windows, 64bit x86 on Linux,
//    and AArch64 on Linux and macOS (jit_unwind.h).
//  - Borrow the references of the handle arguments in the runtime-generated code on
//    32bit hosts, whose call states have no room to note the borrowed parameters, and
//    for the calls that aren't inlined, which would need entry points of the callees
//    that don't release the parameters
//    (jit_bytecode.cpp, AnalyseBorrows).
//  - Register cache for pointer variables and for more than 32 variables (jit_bytecode.cpp, AnalyseSlots),
//    and local variables for the variables in the code generated ahead of time on big endian
//    hosts (jit_aot.cpp, JIT_AOT_MAX_LOCALS).
//  - 32bit x86: keep the value register in a register pair, and inline 64bit integer
//    operations instead of calling JIT_I64Op.
//  - Set the arguments and read the return value of a CJITCall without the checks of
//    the context's methods (jit_runtime.cpp, CJITCall::Prepare).
//  - Project files for the add-on for the IDEs besides CMake.

BEGIN_AS_NAMESPACE

// Records errors reported by AsmJit while emitting code
class CJITErrorHandler : public asmjit::ErrorHandler
{
public:
	CJITErrorHandler() : error(asmjit::Error::kOk) {}
	void handle_error(asmjit::Error err, const char *message, asmjit::BaseEmitter *) override
	{
		if( error == asmjit::Error::kOk )
		{
			error = err;
			this->message = message ? message : "";
		}
	}
	asmjit::Error error;
	std::string   message;
};

struct CJITCompiler::SImpl
{
	asmjit::JitRuntime     runtime;
	std::mutex             mutex;
	asDWORD                flags;
	FILE                  *logFile;
	std::string            logFilter;
	JITCompileFilterFunc_t filter;
	void                  *filterParam;
	asUINT                 maxFunctionSize;
	asUINT                 maxInlineSize;
	asUINT                 maxCachedSlots;
	asUINT                 maxNativeCallDepth;
	bool                   bailOps[asBC_MAXBYTECODE];
	SJITStatistics         stats;
	std::map<asJITFunction, void*> unwindInfo; // registered unwind information by function
	std::map<asJITFunction, size_t> codeSizes; // the sizes of the current and retired code
	CJITAOTOutput          aotOutput;    // the code generated for SetAOTOutput
	std::map<SJITAOTKey, JITAOTFunction_t> aotFunctions; // the functions added with AddAOTFunctions
	std::set<asJITFunction> aotPointers; // the same functions, which aren't released
	std::map<asFUNCTION_t, SJITIndexer> indexers; // the methods added with AddIndexer, by their function
	asUINT                 callThreshold; // see SetCompileThresholds
	asUINT                 loopThreshold;
	std::set<asCScriptFunction*> compiling; // the functions being compiled, see TierUp, Recompile and CompileExact
	asUINT                 profileThreshold; // see SetProfileThreshold
	std::set<asJITFunction> exactCode;   // the code that checks for line callbacks at every statement, see CompileExact

	// The engine keeps the entry function immutable while scripts may execute it.
	// The entry dispatches through this state, whose code pointer and tiering
	// counters can be updated atomically by background compilation.
	struct SEntry
	{
		SEntry(SImpl *owner, asCScriptFunction *function, JITFunction initialCode, const std::vector<asUINT> &initialCounts)
			: impl(owner), func(function), code(initialCode), counts(initialCounts.size()), wrapper(0), profilesVMCalls(false)
		{
			for( size_t n = 0; n < initialCounts.size(); n++ )
				counts[n].store(initialCounts[n], std::memory_order_relaxed);
			vmProfile.countdown = 0;
		}

		SImpl                             *impl;
		asCScriptFunction                 *func;
		std::atomic<JITFunction>           code;
		std::vector<std::atomic<asUINT> > counts;
		asJITFunction                      wrapper;
		SJITProfile                        vmProfile; // classes seen while this function is interpreted
		std::map<const asDWORD*, SJITSeenClasses*> vmCalls; // profile cells by the address after asBC_CALLINTF
		std::atomic<bool>                   profilesVMCalls;
	};
	std::map<asJITFunction, SEntry*> entries; // by the immutable wrapper given to the engine
	std::map<asCScriptFunction*, SEntry*> functionEntries;
	std::atomic<asUINT> deferredProfiles; // deferred entries still noting classes in the VM

	// The profile of the code of a function, which is passed to Recompile when the code
	// has counted down the calls
	struct SProfile : SJITProfile
	{
		SImpl             *impl;
		asCScriptFunction *func;
		asJITFunction      code;       // the code noting the classes
		asUINT             generation; // the times the function has been compiled again before
		bool               recompiled; // Recompile has compiled the function again, or has begun to
		bool               exact;      // the code checks at every statement, and so does the code compiled again
		SJITProfile        compiledWith; // the classes that the code was compiled with
	};

	// The profiles of the code of a function, and the code that the function had before
	// it was compiled again, which the calls under way may still execute. They are
	// released with the current code
	struct SHistory
	{
		std::vector<asJITFunction> retired;
		std::vector<SProfile*>     profiles;
	};
	std::map<asJITFunction, SHistory> histories; // by the current code of the functions

	// What Compile compiles a function for, which the statistics count
	enum ECompile
	{
		COMPILE_FIRST, // the first code of the function
		COMPILE_AGAIN, // the classes in the profile of the source, see Recompile
		COMPILE_EXACT  // the line callbacks, and the classes in the profile of the source if any, see CompileExact
	};

	bool IsLogged(asCScriptFunction *func) const;
	int  Compile(asCScriptFunction *func, CJITByteCode &code, bool log, asJITFunction *output, ECompile purpose = COMPILE_FIRST, const SProfile *source = 0, bool exact = false, bool setEntryArgs = true, const SJITProfile *vmProfile = 0);
	JITFunction TierUp(SEntry *entry, bool exact);
	JITFunction CompileExact(asCScriptFunction *func);
	void Replace(asCScriptFunction *func, asJITFunction code);
	void Release(asJITFunction code);
	int  CreateEntry(asCScriptFunction *func, asJITFunction code, const std::vector<asUINT> &counts, asJITFunction *output, const CJITByteCode *byteCode = 0);
	SEntry *FindEntry(asCScriptFunction *func);
	void PrepareVMProfile(SEntry *entry, const CJITByteCode &code);
	void FinishVMProfile(SEntry *entry);
	void NoteVMClass(asSVMRegisters *regs);
	static asPWORD ResolveEntry(SEntry *entry, asSVMRegisters *regs, asPWORD jitArg);
	static int ContinueInVM(SEntry *entry, asSVMRegisters *regs, asPWORD jitArg, asUINT callLimit, asDWORD *stackPointer);
	static int Recompile(SJITProfile *profile);
	static int ExactEntry(void *impl, asSVMRegisters *regs, asPWORD jitArg);
};

// The times that a function is compiled again at most, see SetProfileThreshold
static const asUINT JIT_MAX_RECOMPILES = 2;
// The count of the profiles whose code doesn't compile the function again
static const int JIT_PROFILE_DONE = 0x7FFFFFFF;

CJITCompiler::CJITCompiler(asDWORD flags)
{
	m_impl = new SImpl;
	m_impl->flags           = flags;
	m_impl->logFile         = stdout;
	m_impl->filter          = 0;
	m_impl->filterParam     = 0;
	m_impl->maxFunctionSize = 100000;
	m_impl->maxInlineSize   = 64;
	m_impl->maxCachedSlots  = 24;
	m_impl->maxNativeCallDepth = 256;
	m_impl->callThreshold   = 0;
	m_impl->loopThreshold   = 0;
	m_impl->profileThreshold = 10000;
	m_impl->deferredProfiles.store(0, std::memory_order_relaxed);
	memset(m_impl->bailOps, 0, sizeof(m_impl->bailOps));
	memset(&m_impl->stats, 0, sizeof(m_impl->stats));
}

CJITCompiler::~CJITCompiler()
{
	// Releasing the runtime frees all code that is still held
	for( std::map<asJITFunction, void*>::iterator it = m_impl->unwindInfo.begin(); it != m_impl->unwindInfo.end(); ++it )
		CJITUnwindInfo::Unregister(it->second);
	for( std::map<asJITFunction, SImpl::SHistory>::iterator it = m_impl->histories.begin(); it != m_impl->histories.end(); ++it )
		for( size_t n = 0; n < it->second.profiles.size(); n++ )
			delete it->second.profiles[n];
	for( std::map<asJITFunction, SImpl::SEntry*>::iterator it = m_impl->entries.begin(); it != m_impl->entries.end(); ++it )
		delete it->second;
	delete m_impl;
}

bool CJITCompiler::IsSupported()
{
#if defined(ASMJIT_UJIT_X86) || defined(ASMJIT_UJIT_AARCH64)
	return true;
#else
	return false;
#endif
}

void CJITCompiler::SetFlags(asDWORD flags)
{
	m_impl->flags = flags;
}

asDWORD CJITCompiler::GetFlags() const
{
	return m_impl->flags;
}

void CJITCompiler::SetLogFile(FILE *file, const char *funcNameFilter)
{
	m_impl->logFile   = file;
	m_impl->logFilter = funcNameFilter ? funcNameFilter : "";
}

void CJITCompiler::SetCompileFilter(JITCompileFilterFunc_t filter, void *userParam)
{
	m_impl->filter      = filter;
	m_impl->filterParam = userParam;
}

void CJITCompiler::SetNativeCallDepth(asUINT depth)
{
	m_impl->maxNativeCallDepth = depth;
	// The functions generated ahead of time share it
	JIT_nativeCallDepth = depth;
}

void CJITCompiler::SetBailInstructions(const asEBCInstr *instructions, asUINT count)
{
	memset(m_impl->bailOps, 0, sizeof(m_impl->bailOps));
	for( asUINT n = 0; n < count; n++ )
		if( instructions[n] < asBC_MAXBYTECODE )
			m_impl->bailOps[instructions[n]] = true;
}

void CJITCompiler::SetMaxFunctionSize(asUINT sizeInDWords)
{
	m_impl->maxFunctionSize = sizeInDWords;
}

void CJITCompiler::SetMaxInlineSize(asUINT sizeInDWords)
{
	m_impl->maxInlineSize = sizeInDWords;
}

int CJITCompiler::SetCompileThresholds(asUINT calls, asUINT iterations)
{
	// The generated calls resolve the current code through immutable entries.
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	if( m_impl->stats.functionsCompiled > 0 || m_impl->stats.functionsDeferred > 0 )
		return asERROR;
	m_impl->callThreshold = calls < JIT_ENTRY_MAX_COUNT ? calls : JIT_ENTRY_MAX_COUNT;
	m_impl->loopThreshold = iterations < JIT_ENTRY_MAX_COUNT ? iterations : JIT_ENTRY_MAX_COUNT;
	return asSUCCESS;
}

void CJITCompiler::SetProfileThreshold(asUINT calls)
{
	m_impl->profileThreshold = calls < asUINT(JIT_PROFILE_DONE) ? calls : asUINT(JIT_PROFILE_DONE);
}

void CJITCompiler::SetAOTOutput(const char *directory)
{
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	m_impl->aotOutput.SetDirectory(directory);
}

int CJITCompiler::WriteAOTOutput()
{
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	return m_impl->aotOutput.Write();
}

int CJITCompiler::AddAOTFunctions(const SJITAOTFunction *functions, asUINT count)
{
	if( functions == 0 && count > 0 )
		return asINVALID_ARG;

	// The code compiled before doesn't set the current function for its native calls,
	// see SJITCodeGenOptions::interop
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	if( m_impl->stats.functionsCompiled > 0 || m_impl->stats.functionsDeferred > 0 )
		return asERROR;
	for( asUINT n = 0; n < count; n++ )
	{
		if( functions[n].func == 0 )
			continue;
		SJITAOTKey key;
		key.key0 = functions[n].key0;
		key.key1 = functions[n].key1;
		m_impl->aotFunctions[key] = functions[n].func;
		m_impl->aotPointers.insert(reinterpret_cast<asJITFunction>(functions[n].func));
	}
	return asSUCCESS;
}

int CJITCompiler::AddIndexer(const asSFuncPtr &method, const SJITIndexer &indexer)
{
	// The methods are found by the function that the engine keeps for them
	if( method.flag != 3 || method.ptr.f.func == 0 || indexer.bufferOffset < 0 || indexer.lengthOffset < 0 || indexer.dataOffset < 0 )
		return asINVALID_ARG;

	std::lock_guard<std::mutex> lock(m_impl->mutex);
	if( m_impl->stats.functionsCompiled > 0 || m_impl->stats.functionsDeferred > 0 )
		return asERROR;
	m_impl->indexers[method.ptr.f.func] = indexer;
	return asSUCCESS;
}

int CJITCompiler::Prepare(asIScriptContext *ctx, asIScriptFunction *func)
{
	return JIT_Prepare(ctx, func);
}

int CJITCompiler::Execute(asIScriptContext *ctx)
{
	return JIT_Execute(ctx, this, m_impl->maxNativeCallDepth);
}

asUINT CJITCompiler::GetNativeCallDepth() const
{
	return m_impl->maxNativeCallDepth;
}

SJITStatistics CJITCompiler::GetStatistics() const
{
	std::lock_guard<std::mutex> lock(m_impl->mutex);
	return m_impl->stats;
}

// Writes the bytecode in a readable form, similar to the engine's own debug output
static void DumpByteCode(FILE *file, const CJITByteCode &code)
{
	const std::vector<SJITInstr> &instrs = code.GetInstructions();
	for( asUINT n = 0; n < instrs.size(); n++ )
	{
		const SJITInstr &instr = instrs[n];
		const asDWORD *bc = instr.bc;
		const asSBCInfo &info = asBCInfo[instr.op];

		fprintf(file, "%s%5d  %-12s", (instr.flags & JIT_INSTR_BLOCK_START) ? ":" : " ", instr.pos, info.name);
		switch( info.type )
		{
		case asBCTYPE_W_ARG:
		case asBCTYPE_wW_ARG:
		case asBCTYPE_rW_ARG:
			fprintf(file, " v%d", asBC_SWORDARG0(bc));
			break;
		case asBCTYPE_DW_ARG:
			if( CJITByteCode::IsBranch(instr.op) )
				fprintf(file, " -> %d", instr.pos + 2 + asBC_INTARG(bc));
			else
				fprintf(file, " %d", asBC_INTARG(bc));
			break;
		case asBCTYPE_rW_DW_ARG:
		case asBCTYPE_wW_DW_ARG:
		case asBCTYPE_W_DW_ARG:
			fprintf(file, " v%d, %d", asBC_SWORDARG0(bc), asBC_INTARG(bc));
			break;
		case asBCTYPE_QW_ARG:
			fprintf(file, " 0x%llx", (unsigned long long)asBC_QWORDARG(bc));
			break;
		case asBCTYPE_DW_DW_ARG:
			fprintf(file, " %d, %d", asBC_INTARG(bc), (int)*(bc + 2));
			break;
		case asBCTYPE_wW_rW_rW_ARG:
			fprintf(file, " v%d, v%d, v%d", asBC_SWORDARG0(bc), asBC_SWORDARG1(bc), asBC_SWORDARG2(bc));
			break;
		case asBCTYPE_wW_QW_ARG:
		case asBCTYPE_rW_QW_ARG:
			fprintf(file, " v%d, 0x%llx", asBC_SWORDARG0(bc), (unsigned long long)asBC_QWORDARG(bc));
			break;
		case asBCTYPE_wW_rW_ARG:
		case asBCTYPE_rW_rW_ARG:
		case asBCTYPE_wW_W_ARG:
		case asBCTYPE_W_rW_ARG:
			fprintf(file, " v%d, v%d", asBC_SWORDARG0(bc), asBC_SWORDARG1(bc));
			break;
		case asBCTYPE_wW_rW_DW_ARG:
		case asBCTYPE_rW_W_DW_ARG:
			fprintf(file, " v%d, v%d, %d", asBC_SWORDARG0(bc), asBC_SWORDARG1(bc), (int)*(bc + 2));
			break;
		case asBCTYPE_QW_DW_ARG:
			fprintf(file, " 0x%llx, %d", (unsigned long long)asBC_QWORDARG(bc), (int)*(bc + 3));
			break;
		case asBCTYPE_rW_DW_DW_ARG:
			fprintf(file, " v%d, %d, %d", asBC_SWORDARG0(bc), asBC_INTARG(bc), (int)*(bc + 2));
			break;
		case asBCTYPE_W_DW_DW_ARG:
			fprintf(file, " %d, %d, %d", asBC_WORDARG0(bc), asBC_INTARG(bc), (int)*(bc + 2));
			break;
		case asBCTYPE_W_QW_DW_ARG:
			fprintf(file, " %d, 0x%llx, %d", asBC_WORDARG0(bc), (unsigned long long)asBC_QWORDARG(bc), (int)*(bc + 3));
			break;
		default:
			break;
		}
		if( instr.flags & JIT_INSTR_INLINE )
			fprintf(file, "   ; inlined");
		if( instr.flags & JIT_INSTR_INDEXER )
			fprintf(file, "   ; indexer");
		if( instr.flags & JIT_INSTR_PROFILE )
			fprintf(file, (instr.flags & JIT_INSTR_INLINE) ? " for the class seen" : "   ; classes noted");
		if( instr.flags & JIT_INSTR_BORROW )
			fprintf(file, "   ; borrowed");
		if( instr.flags & (JIT_INSTR_MOVE | JIT_INSTR_MOVED) )
			fprintf(file, "   ; moved");
		if( instr.flags & JIT_INSTR_REFCOUNT )
			fprintf(file, "   ; counted in place");
		if( instr.flags & JIT_INSTR_FREE_LIST )
			fprintf(file, "   ; memory freed");
		if( instr.flags & JIT_INSTR_VR_LIVE )
			fprintf(file, "   ; vr live");
		if( code.GetDirtyMask(n) )
			fprintf(file, "   ; dirty 0x%X", code.GetDirtyMask(n));
		fprintf(file, "\n");
	}

	const std::vector<SJITSlot> &slots = code.GetSlots();
	for( asUINT n = 0; n < slots.size(); n++ )
	{
		if( slots[n].cacheKind == JIT_SLOT_NONE )
			continue;
		const char *kind = slots[n].cacheKind == JIT_SLOT_I32 ? "int32" :
		                   slots[n].cacheKind == JIT_SLOT_I64 ? "int64" :
		                   slots[n].cacheKind == JIT_SLOT_F32 ? "float" : "double";
		fprintf(file, "; v%d cached in register as %s (%d uses)\n", slots[n].offset, kind, slots[n].useCount);
	}

	const std::vector<SJITField> &fields = code.GetFields();
	for( asUINT n = 0; n < fields.size(); n++ )
	{
		if( fields[n].kept )
			fprintf(file, "; this+%d kept in register as %s (%d reads forwarded)\n", fields[n].offset,
			        fields[n].kind == JIT_SLOT_F32 ? "float" : "int32", fields[n].forwarded);
	}
}

bool CJITCompiler::SImpl::IsLogged(asCScriptFunction *func) const
{
	return (flags & JIT_LOG) && logFile &&
	       (logFilter.empty() || strstr(func->GetDeclaration(true, true), logFilter.c_str()) != 0);
}

CJITCompiler::SImpl::SEntry *CJITCompiler::SImpl::FindEntry(asCScriptFunction *func)
{
	std::map<asCScriptFunction*, SEntry*>::iterator it = functionEntries.find(func);
	return it != functionEntries.end() ? it->second : 0;
}

// Prepares cells for the classes seen by the virtual and interface calls while a
// deferred function still runs in the VM. The return addresses of the calls are in
// their call states when their script implementations enter ResolveEntry.
void CJITCompiler::SImpl::PrepareVMProfile(SEntry *entry, const CJITByteCode &code)
{
	if( entry->counts.empty() || profileThreshold == 0 || maxInlineSize == 0 ||
	    (flags & (JIT_NO_INLINE | JIT_NO_SCRIPT_CALLS | JIT_SYNC_EVERY_INSTR)) )
		return;

	asCScriptEngine *engine = entry->func->engine;
	const std::vector<SJITInstr> &instrs = code.GetInstructions();
	for( asUINT n = 0; n < instrs.size(); n++ )
	{
		const SJITInstr &instr = instrs[n];
		if( instr.op != asBC_CALLINTF )
			continue;
		int id = asBC_INTARG(instr.bc);
		asCScriptFunction *method = id >= 0 && asUINT(id) < engine->scriptFunctions.GetLength() ? engine->scriptFunctions[id] : 0;
		if( method == 0 || (method->funcType != asFUNC_VIRTUAL && method->funcType != asFUNC_INTERFACE) )
			continue;
		SJITSeenClasses &seen = entry->vmProfile.classes[std::make_pair(entry->func, n)];
		entry->vmCalls[instr.bc + instr.size] = &seen;
	}
	if( !entry->vmCalls.empty() )
	{
		entry->profilesVMCalls.store(true, std::memory_order_release);
		deferredProfiles.fetch_add(1, std::memory_order_relaxed);
	}
}

// Stops looking up the calls of an entry after its first compilation has taken a
// snapshot of the classes seen in the VM, or after the entry is released.
void CJITCompiler::SImpl::FinishVMProfile(SEntry *entry)
{
	if( entry->profilesVMCalls.exchange(false, std::memory_order_acq_rel) )
		deferredProfiles.fetch_sub(1, std::memory_order_relaxed);
}

static void NoteSeenClass(SJITSeenClasses *seen, asCObjectType *type)
{
	for( asUINT n = 0; n < JIT_PROFILE_CLASSES; n++ )
	{
		if( seen->types[n] == type )
			return;
		if( seen->types[n] == 0 )
		{
			seen->types[n] = type;
			return;
		}
	}
}

// Notes the class of the receiver of the interpreted CALLINTF that entered the
// current script function. PushCallState leaves the caller's program pointer just
// after the call and its stack pointer at the receiver in the top call state.
void CJITCompiler::SImpl::NoteVMClass(asSVMRegisters *regs)
{
	asCContext *ctx = static_cast<asCContext*>(regs->ctx);
	asUINT length = ctx->m_callStack.GetLength();
	if( length < CALLSTACK_FRAME_SIZE )
		return;
	asPWORD *state = ctx->m_callStack.AddressOf() + length - CALLSTACK_FRAME_SIZE;
	if( state[0] == 0 || state[1] == 0 || state[3] == 0 )
		return;

	asCScriptFunction *caller = reinterpret_cast<asCScriptFunction*>(state[1]);
	std::map<asCScriptFunction*, SEntry*>::iterator found = functionEntries.find(caller);
	if( found == functionEntries.end() )
		return;
	SEntry *entry = found->second;
	if( !entry->profilesVMCalls.load(std::memory_order_acquire) )
		return;

	const asDWORD *returnAddress = reinterpret_cast<const asDWORD*>(state[2]);
	std::map<const asDWORD*, SJITSeenClasses*>::iterator call = entry->vmCalls.find(returnAddress);
	if( call == entry->vmCalls.end() )
		return;
	asCScriptObject *obj = *reinterpret_cast<asCScriptObject**>(state[3]);
	if( obj )
		NoteSeenClass(call->second, obj->objType);
}

asJITFunction JIT_GetNativeTarget(asCScriptFunction *func)
{
	if( func == 0 || func->engine == 0 || func->engine->jitCompiler == 0 )
		return 0;
	CJITCompiler *compiler = static_cast<CJITCompiler*>(static_cast<asIJITCompiler*>(func->engine->jitCompiler));
	std::lock_guard<std::mutex> lock(compiler->m_impl->mutex);
	CJITCompiler::SImpl::SEntry *entry = compiler->m_impl->FindEntry(func);
	return entry ? reinterpret_cast<asJITFunction>(entry->code.load(std::memory_order_acquire)) : 0;
}

int CJITCompiler::SImpl::CreateEntry(asCScriptFunction *func, asJITFunction code, const std::vector<asUINT> &counts, asJITFunction *output, const CJITByteCode *byteCode)
{
	using namespace asmjit;
	using namespace asmjit::ujit;

	SEntry *entry = new SEntry(this, func, reinterpret_cast<JITFunction>(code), counts);
	if( byteCode )
		PrepareVMProfile(entry, *byteCode);
	CodeHolder holder;
	holder.init(runtime.environment(), runtime.cpu_features());
	CJITErrorHandler errorHandler;
	holder.set_error_handler(&errorHandler);
	BackendCompiler cc;
	holder.attach(&cc);
	UniCompiler uc(&cc, runtime.cpu_features(), CpuHints::kNone);
	uc.init_vec_width(VecWidth::k128);

	FuncSignature signature = FuncSignature::build<int, asSVMRegisters*, asPWORD, asUINT, asDWORD*>();
	FuncNode *wrapperFunc = uc.add_func(signature);
#ifdef JIT_NATIVE_RETURN
	JIT_AddVRReturn(wrapperFunc->detail());
#endif
	Gp regs = uc.new_gp_ptr("regs");
	Gp jitArg = uc.new_gp_ptr("jitArg");
	Gp callLimit = uc.new_gp32("callLimit");
	Gp stackPointer = uc.new_gp_ptr("stackPointer");
	wrapperFunc->set_arg(0, regs);
	wrapperFunc->set_arg(1, jitArg);
	wrapperFunc->set_arg(2, callLimit);
	wrapperFunc->set_arg(3, stackPointer);

	Gp target = uc.new_gp_ptr("target");
	InvokeNode *resolve = JIT_Invoke(uc, (const void*)ResolveEntry, FuncSignature::build<asPWORD, SEntry*, asSVMRegisters*, asPWORD>());
	resolve->set_arg(0, Imm(int64_t(asPWORD(entry))));
	resolve->set_arg(1, regs);
	resolve->set_arg(2, jitArg);
	resolve->set_ret(0, target);

	Label fallback = uc.new_label();
	uc.j(fallback, test_z(target));
	InvokeNode *call = 0;
	uc.cc->invoke(Out(call), target, signature);
	call->set_arg(0, regs);
	call->set_arg(1, jitArg);
	call->set_arg(2, callLimit);
	call->set_arg(3, stackPointer);
	Gp result = uc.new_gp32("result");
	call->set_ret(0, result);
#ifdef JIT_NATIVE_RETURN
	JIT_AddVRReturn(call->detail());
	Gp value = uc.new_gp64("value");
	call->set_ret(1, value);
	uc.ret(result, value);
#else
	uc.ret(result);
#endif

	uc.bind(fallback);
	InvokeNode *leave = JIT_Invoke(uc, (const void*)ContinueInVM, FuncSignature::build<int, SEntry*, asSVMRegisters*, asPWORD, asUINT, asDWORD*>());
	leave->set_arg(0, Imm(int64_t(asPWORD(entry))));
	leave->set_arg(1, regs);
	leave->set_arg(2, jitArg);
	leave->set_arg(3, callLimit);
	leave->set_arg(4, stackPointer);
	leave->set_ret(0, result);
	uc.ret(result);
	uc.end_func();

	bool ok = cc.finalize() == Error::kOk && errorHandler.error == Error::kOk;
	CJITUnwindInfo unwind;
	bool hasUnwind = false;
#ifndef AS_NO_EXCEPTIONS
	if( ok && CJITUnwindInfo::IsSupported() )
	{
		hasUnwind = unwind.Prepare(cc, wrapperFunc);
		ok = hasUnwind;
	}
#endif

	asJITFunction wrapper = 0;
	void *unwindHandle = 0;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if( ok )
			ok = runtime.add(&wrapper, &holder) == Error::kOk;
		if( ok && hasUnwind )
			ok = unwind.Register((void*)wrapper, &unwindHandle);
		if( !ok && wrapper )
			runtime.release(wrapper);
		if( ok )
		{
			entry->wrapper = wrapper;
			entries[wrapper] = entry;
			functionEntries[func] = entry;
			codeSizes[wrapper] = holder.code_size();
			stats.codeSize += codeSizes[wrapper];
			if( unwindHandle )
				unwindInfo[wrapper] = unwindHandle;
		}
	}
	if( !ok )
	{
		if( unwindHandle )
			CJITUnwindInfo::Unregister(unwindHandle);
		FinishVMProfile(entry);
		delete entry;
		std::lock_guard<std::mutex> lock(mutex);
		stats.functionsFailed++;
		return asERROR;
	}

	*output = wrapper;
	return asSUCCESS;
}

// Sets the index of the entry point in the JitEntry instructions, which the VM passes to the function
static void SetEntryArgs(asCScriptFunction *func, const CJITByteCode &code)
{
	asDWORD *byteCode = func->scriptData->byteCode.AddressOf();
	const std::vector<asUINT> &entries = code.GetEntries();
	const std::vector<SJITInstr> &instrs = code.GetInstructions();
	for( asUINT n = 0; n < entries.size(); n++ )
		asBC_PTRARG(byteCode + instrs[entries[n]].pos) = asPWORD(n + 1);
}

// Gets the deferred-compilation counts: calls at the first entry, and iterations
// at the first entry in each loop. Returns false if the entry points don't allow it.
static bool GetDeferredEntryCounts(const CJITByteCode &code, asUINT calls, asUINT iterations, std::vector<asUINT> &counts)
{
	const std::vector<asUINT> &entries = code.GetEntries();
	const std::vector<SJITInstr> &instrs = code.GetInstructions();
	if( entries.empty() || entries[0] != 0 || entries.size() > JIT_ENTRY_INDEX_MASK )
		return false;

	counts.assign(entries.size(), 0);
	counts[0] = calls;
	for( asUINT n = 0; iterations > 0 && n < instrs.size(); n++ )
	{
		if( instrs[n].target < 0 || asUINT(instrs[n].target) > n )
			continue;

		// The loop runs from the target of the backward branch to the branch. The
		// entries after a SUSPEND start the statements, which are always executed
		std::vector<asUINT>::const_iterator first = std::lower_bound(entries.begin(), entries.end(), asUINT(instrs[n].target));
		std::vector<asUINT>::const_iterator head = first;
		while( head != entries.end() && *head <= n && !(*head > 0 && instrs[*head - 1].op == asBC_SUSPEND) )
			++head;
		if( head == entries.end() || *head > n )
			head = first;
		if( head == entries.end() || *head > n )
			continue;
		asUINT &count = counts[head - entries.begin()];
		if( count == 0 || count > iterations )
			count = iterations;
	}

	return true;
}

// The functions are taken from the module, whose list only changes when it is built
// again or discarded. Each is compiled like when its threshold is reached, so the
// threads that execute it meanwhile go on in the VM until the code is installed
int CJITCompiler::CompileDeferred(asIScriptModule *module)
{
	if( module == 0 )
		return asINVALID_ARG;
	asCModule *mod = static_cast<asCModule*>(module);
	std::vector<asCScriptFunction*> funcs;
	for( asUINT n = 0; n < mod->m_scriptFunctions.GetLength(); n++ )
		funcs.push_back(mod->m_scriptFunctions[n]);

	int compiled = 0;
	for( size_t n = 0; n < funcs.size(); n++ )
	{
		asCScriptFunction *func = funcs[n];
		SImpl::SEntry *entry = 0;
		if( func && func->funcType == asFUNC_SCRIPT && func->scriptData )
		{
			std::lock_guard<std::mutex> lock(m_impl->mutex);
			entry = m_impl->FindEntry(func);
		}
		if( entry && !entry->counts.empty() && entry->code.load(std::memory_order_acquire) == 0 )
			if( m_impl->TierUp(entry, false) )
				compiled++;
	}
	return compiled;
}

int CJITCompiler::CompileFunction(asIScriptFunction *function, asJITFunction *output)
{
	*output = 0;

	asCScriptFunction *func = static_cast<asCScriptFunction*>(function);
	if( func->funcType != asFUNC_SCRIPT || func->scriptData == 0 )
		return asERROR;

	// The functions generated ahead of time are found by the bytecode, which is decoded
	// before the filter is asked then, as the code is generated for all functions
	bool aotOutput = !m_impl->aotOutput.GetDirectory().empty();
	bool aot       = aotOutput || !m_impl->aotFunctions.empty();
	if( !aot && ((m_impl->flags & JIT_AOT_ONLY) || !IsSupported() || (m_impl->filter && !m_impl->filter(function, m_impl->filterParam))) )
		return asNOT_SUPPORTED;

	// Decode and analyse the bytecode
	CJITByteCode code;
	int r = code.Decode(func);
	if( r == asNOT_SUPPORTED )
		return asNOT_SUPPORTED;
	if( r < 0 || code.GetLength() > m_impl->maxFunctionSize )
	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		m_impl->stats.functionsFailed++;
		return asERROR;
	}

	bool log = m_impl->IsLogged(func);

	if( aot )
	{
		SJITAOTKey key = JIT_GetAOTKey(code, &m_impl->indexers);
		if( aotOutput )
		{
			CJITAOTOutput::EResult result;
			{
				std::lock_guard<std::mutex> lock(m_impl->mutex);
				result = m_impl->aotOutput.Add(code, key, func->GetDeclaration(true, true), &m_impl->indexers);
			}
			if( result == CJITAOTOutput::AOT_CONFLICT )
			{
				std::string message = "Other code was generated ahead of time with the key of '";
				message += func->GetDeclaration(true, true);
				message += "', neither is written";
				func->GetEngine()->WriteMessage("JIT", 0, 0, asMSGTYPE_WARNING, message.c_str());
			}
		}

		if( m_impl->filter && !m_impl->filter(function, m_impl->filterParam) )
			return asNOT_SUPPORTED;

		std::map<SJITAOTKey, JITAOTFunction_t>::const_iterator it = m_impl->aotFunctions.find(key);
		if( it != m_impl->aotFunctions.end() )
		{
			if( log )
				fprintf(m_impl->logFile, "\n; ---- %s ----\n; generated ahead of time as %s\n", func->GetDeclaration(true, true), JIT_GetAOTName(key).c_str());
			SetEntryArgs(func, code);
			// AOT is also supported on platforms for which AsmJit cannot emit the
			// immutable dispatch wrapper. No runtime replacement is possible there.
			if( !IsSupported() )
			{
				std::lock_guard<std::mutex> lock(m_impl->mutex);
				m_impl->stats.functionsAOT++;
				*output = reinterpret_cast<asJITFunction>(it->second);
				return asSUCCESS;
			}
			int wrapped = m_impl->CreateEntry(func, reinterpret_cast<asJITFunction>(it->second), std::vector<asUINT>(), output);
			if( wrapped >= 0 )
			{
				std::lock_guard<std::mutex> lock(m_impl->mutex);
				m_impl->stats.functionsAOT++;
			}
			return wrapped;
		}
		if( (m_impl->flags & JIT_AOT_ONLY) || !IsSupported() )
		{
			if( log )
				fprintf(m_impl->logFile, "\n; ---- %s ----\n; not generated ahead of time\n", func->GetDeclaration(true, true));
			return asNOT_SUPPORTED;
		}
	}

	// The function is compiled when it is executed often enough, see ResolveEntry.
	std::vector<asUINT> tieredCounts;
	if( m_impl->callThreshold > 0 && GetDeferredEntryCounts(code, m_impl->callThreshold, m_impl->loopThreshold, tieredCounts) )
	{
		SetEntryArgs(func, code);
		if( log )
			fprintf(m_impl->logFile, "\n; ---- %s ----\n; compilation deferred\n", func->GetDeclaration(true, true));
		int wrapped = m_impl->CreateEntry(func, 0, tieredCounts, output, &code);
		if( wrapped >= 0 )
		{
			std::lock_guard<std::mutex> lock(m_impl->mutex);
			m_impl->stats.functionsDeferred++;
		}
		return wrapped;
	}

	asJITFunction compiled = 0;
	int compiledResult = m_impl->Compile(func, code, log, &compiled);
	if( compiledResult < 0 )
		return compiledResult;
	int wrapped = m_impl->CreateEntry(func, compiled, std::vector<asUINT>(), output);
	if( wrapped < 0 )
	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		std::map<asJITFunction, SImpl::SHistory>::iterator history = m_impl->histories.find(compiled);
		if( history != m_impl->histories.end() )
		{
			for( size_t n = 0; n < history->second.profiles.size(); n++ )
				delete history->second.profiles[n];
			m_impl->histories.erase(history);
		}
		m_impl->Release(compiled);
		m_impl->stats.functionsCompiled--;
	}
	return wrapped;
}

// Analyses the decoded bytecode of the function and generates its code. The entry
// points are set in the JitEntry instructions then. The code is compiled again with
// the classes in the profile of the source, see Recompile. With exact the code checks
// for suspension and line callbacks at every statement, and so does the code compiled
// again with its profile
int CJITCompiler::SImpl::Compile(asCScriptFunction *func, CJITByteCode &code, bool log, asJITFunction *output, ECompile purpose, const SProfile *source, bool exact, bool setEntryArgs, const SJITProfile *vmProfile)
{
	using namespace asmjit;
	using namespace asmjit::ujit;

	exact = exact || purpose == COMPILE_EXACT || (source && source->exact);

	// The dirty masks hold one bit per cached slot, and the bit of the frame
	asUINT cachedSlots = maxCachedSlots < 31 ? maxCachedSlots : 31;
	// The functions left to the compile filter are left to their calls too
	SJITInlineOptions inlining;
	inlining.maxSize     = (flags & (JIT_NO_INLINE | JIT_NO_SCRIPT_CALLS | JIT_SYNC_EVERY_INSTR)) ? 0 : maxInlineSize;
	inlining.filter      = filter;
	inlining.filterParam = filterParam;
	// The calls note the classes that they see, and the calls not made yet go on noting
	// them in the code compiled again, up to JIT_MAX_RECOMPILES
	asUINT generation = source ? source->generation + 1 : 0;
	inlining.profile     = profileThreshold > 0 && inlining.maxSize > 0 && generation < JIT_MAX_RECOMPILES;
	// The classes seen before stay in the profile for the next time. The code of the
	// source may go on noting them, and the VM may still finish a call while the first
	// code is compiled, so the analysis takes them from the copy
	const SJITProfile *seenBefore = source ? static_cast<const SJITProfile*>(source) : vmProfile;
	SProfile *profile = 0;
	if( inlining.profile )
	{
		profile = new SProfile;
		if( seenBefore )
			profile->compiledWith.classes = seenBefore->classes;
		profile->classes    = profile->compiledWith.classes;
		profile->countdown  = int(profileThreshold);
		profile->impl       = this;
		profile->func       = func;
		profile->code       = 0;
		profile->generation = generation;
		profile->recompiled = false;
		profile->exact      = exact;
	}
	inlining.classes     = profile ? &profile->compiledWith : seenBefore;
	inlining.indexers    = &indexers;
	code.SetBailInstructions(bailOps);
	code.Analyse((flags & JIT_NO_REGISTER_CACHE) == 0, cachedSlots, &inlining);

	if( log )
	{
		fprintf(logFile, "\n; ---- %s ----\n", func->GetDeclaration(true, true));
		if( purpose == COMPILE_AGAIN )
			fprintf(logFile, "; compiled again with the classes seen by the calls\n");
		else if( purpose == COMPILE_EXACT )
			fprintf(logFile, "; compiled again for the line callbacks\n");
		else if( exact )
			fprintf(logFile, "; compiled for the line callbacks\n");
		DumpByteCode(logFile, code);
	}

	// Generate the code. Everything here is local to the call so that
	// functions can be compiled concurrently and recursively
	CJITErrorHandler errorHandler;
	CodeHolder holder;
	holder.init(runtime.environment(), runtime.cpu_features());
	holder.set_error_handler(&errorHandler);

#ifndef ASMJIT_NO_LOGGING
	FileLogger logger(logFile);
	if( log )
	{
		logger.add_flags(FormatFlags::kMachineCode);
		holder.set_logger(&logger);
	}
#endif

	BackendCompiler cc;
	holder.attach(&cc);

	UniCompiler uc(&cc, runtime.cpu_features(), CpuHints::kNone);
	uc.init_vec_width(VecWidth::k128);

	SJITCodeGenOptions options;
	options.noSuspend      = (flags & JIT_NO_SUSPEND) != 0;
	options.elideSuspend   = !options.noSuspend && !exact && (flags & JIT_CHECK_EVERY_STATEMENT) == 0;
	options.noScriptCalls  = (flags & JIT_NO_SCRIPT_CALLS) != 0;
	options.syncEveryInstr = (flags & JIT_SYNC_EVERY_INSTR) != 0;
	options.maxNativeCallDepth = maxNativeCallDepth;
	options.interop = !aotFunctions.empty();
	options.profile   = profile;
	options.recompile = (const void*)Recompile;
	options.exactEntry = (const void*)ExactEntry;
	options.exactParam = this;
#ifdef AS_NO_EXCEPTIONS
	// Without exception handling in the engine nothing is lost by calling directly
	options.directSystemCalls = (flags & JIT_NO_DIRECT_SYSTEM_CALLS) == 0;
	options.guardedEntry = false;
#else
	// The C++ exceptions thrown by the functions called directly can only be caught
	// if they can pass through the generated code
	options.directSystemCalls = (flags & JIT_NO_DIRECT_SYSTEM_CALLS) == 0 &&
	                            (CJITUnwindInfo::IsSupported() || (flags & JIT_DIRECT_SYSTEM_CALLS) != 0);
	options.guardedEntry = options.directSystemCalls && CJITUnwindInfo::IsSupported();
#endif

	CJITCodeGen gen(uc, code, options);
	bool ok = gen.Generate();
	if( !ok && errorHandler.message.empty() && gen.GetFailedInstruction() )
		errorHandler.message = std::string("no code for ") + gen.GetFailedInstruction();
	if( ok )
		ok = (cc.finalize() == Error::kOk);
	if( ok )
		ok = (errorHandler.error == Error::kOk);
	if( !ok && errorHandler.message.empty() && errorHandler.error != Error::kOk )
		errorHandler.message = stringify_error(errorHandler.error);

	// The functions that C++ exceptions can pass through need unwind information
	CJITUnwindInfo unwind;
	if( ok && gen.IsGuarded() && !unwind.Prepare(cc, gen.GetFuncNode()) )
	{
		ok = false;
		errorHandler.message = "no unwind information for the prologue";
	}

	// The code that notes no classes doesn't need the profile
	if( profile && gen.GetProfiledCallCount() == 0 )
	{
		delete profile;
		profile = 0;
	}

	asJITFunction jitFunc = 0;
	if( ok )
	{
		std::lock_guard<std::mutex> lock(mutex);
		Error err = runtime.add(&jitFunc, &holder);
		ok = (err == Error::kOk);
		if( !ok )
			errorHandler.message = std::string("the code could not be added: ") + stringify_error(err);
		void *unwindHandle = 0;
		if( ok && gen.IsGuarded() && !unwind.Register((void*)jitFunc, &unwindHandle) )
		{
			runtime.release(jitFunc);
			ok = false;
			errorHandler.message = "the unwind information could not be registered";
		}
		if( unwindHandle )
			unwindInfo[jitFunc] = unwindHandle;
		if( ok )
		{
			if( purpose == COMPILE_AGAIN )
				stats.functionsRecompiled++;
			else if( purpose == COMPILE_EXACT )
				stats.functionsForLineCallbacks++;
			else
				stats.functionsCompiled++;
			if( exact )
				exactCode.insert(jitFunc);
			if( profile )
			{
				profile->code = jitFunc;
				histories[jitFunc].profiles.push_back(profile);
			}
			stats.instructionsCompiled += gen.GetInstructionCount();
			stats.instructionsBailed   += gen.GetBailCount();
			stats.callsInlined         += gen.GetInlinedCallCount();
			codeSizes[jitFunc] = holder.code_size();
			stats.codeSize += codeSizes[jitFunc];
		}
	}

	if( !ok )
	{
		if( log )
			fprintf(logFile, "; compilation failed: %s\n", errorHandler.message.c_str());
		delete profile;
		std::lock_guard<std::mutex> lock(mutex);
		stats.functionsFailed++;
		return asERROR;
	}

	if( log )
	{
		fprintf(logFile, "; code at %p, %u bytes\n", (void*)jitFunc, (unsigned)holder.code_size());
		if( profile )
			fprintf(logFile, "; %u calls note their classes\n", gen.GetProfiledCallCount());
	}

	if( setEntryArgs )
		SetEntryArgs(func, code);
	*output = jitFunc;
	return asSUCCESS;
}

// Compiles a deferred function, unless another thread does. Returns the code of the
// function, or null if it isn't compiled. With exact the code checks at every
// statement, see CompileExact
JITFunction CJITCompiler::SImpl::TierUp(SEntry *entry, bool exact)
{
	asCScriptFunction *func = entry->func;
	{
		std::lock_guard<std::mutex> lock(mutex);
		JITFunction current = entry->code.load(std::memory_order_acquire);
		if( current )
			return current;
		if( !compiling.insert(func).second )
			return 0;
	}

	CJITByteCode code;
	asJITFunction jitFunc = 0;
	const SJITProfile *vmProfile = entry->profilesVMCalls.load(std::memory_order_acquire) ? &entry->vmProfile : 0;
	bool ok = code.Decode(func) >= 0 && Compile(func, code, IsLogged(func), &jitFunc, COMPILE_FIRST, 0, exact, false, vmProfile) >= 0;

	std::lock_guard<std::mutex> lock(mutex);
	compiling.erase(func);
	if( !ok )
	{
		FinishVMProfile(entry);
		for( size_t n = 0; n < entry->counts.size(); n++ )
			entry->counts[n].store(0, std::memory_order_relaxed);
		return 0;
	}
	entry->code.store(reinterpret_cast<JITFunction>(jitFunc), std::memory_order_release);
	FinishVMProfile(entry);
	return reinterpret_cast<JITFunction>(jitFunc);
}

// Compiles the function of the profile again with the classes that the calls of its
// code have seen, unless another thread does or has, and installs the new code. The
// entry points stay the same, so the VM may enter either code at them. Called by the
// code when it has counted down the calls, which leaves the call to the VM if 1 is
// returned, i.e. the function has new code, so that the VM goes on in that after the
// call
int CJITCompiler::SImpl::Recompile(SJITProfile *jitProfile)
{
	SProfile *profile = static_cast<SProfile*>(jitProfile);
	SImpl *impl = profile->impl;
	asCScriptFunction *func = profile->func;
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		SEntry *entry = impl->FindEntry(func);
		bool current = entry && entry->code.load(std::memory_order_acquire) == reinterpret_cast<JITFunction>(profile->code);
		if( profile->recompiled || !current )
		{
			// The calls of the code don't come here again for a long time
			profile->countdown = JIT_PROFILE_DONE;
			return profile->recompiled && !current;
		}
		// The code goes on noting the classes unless the new code would inline more,
		// and while another thread compiles the function for the line callbacks
		if( !profile->HasNewClass(profile->compiledWith) || !impl->compiling.insert(func).second )
		{
			profile->countdown = int(impl->profileThreshold);
			return 0;
		}
		profile->countdown = JIT_PROFILE_DONE;
		profile->recompiled = true;
	}

	CJITByteCode code;
	asJITFunction jitFunc = 0;
	bool ok = code.Decode(func) >= 0 && impl->Compile(func, code, impl->IsLogged(func), &jitFunc, COMPILE_AGAIN, profile, false, false) >= 0;

	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->compiling.erase(func);
	if( !ok )
		return 0;
	impl->Replace(func, jitFunc);
	return 1;
}

// Compiles the function again with the checks for suspension and line callbacks at
// every statement, unless another thread does, and installs the new code, which the
// function keeps. The code compiled again with the profile of the new code checks at
// every statement too. Returns the code of the function, or null if the VM goes on
JITFunction CJITCompiler::SImpl::CompileExact(asCScriptFunction *func)
{
	const SProfile *source = 0;
	{
		std::lock_guard<std::mutex> lock(mutex);
		SEntry *entry = FindEntry(func);
		asJITFunction current = entry ? reinterpret_cast<asJITFunction>(entry->code.load(std::memory_order_acquire)) : 0;
		if( !entry )
			return 0;
		if( exactCode.count(current) )
			return reinterpret_cast<JITFunction>(current);
		if( !compiling.insert(func).second )
			return 0;
		// The new code inlines the methods for the classes that the calls have seen, and
		// is compiled again like the code of the newest profile, see SHistory
		std::map<asJITFunction, SHistory>::iterator it = histories.find(current);
		if( it != histories.end() && !it->second.profiles.empty() )
			source = it->second.profiles[0];
	}

	CJITByteCode code;
	asJITFunction jitFunc = 0;
	bool ok = code.Decode(func) >= 0 && Compile(func, code, IsLogged(func), &jitFunc, COMPILE_EXACT, source, false, false) >= 0;

	std::lock_guard<std::mutex> lock(mutex);
	compiling.erase(func);
	if( !ok )
		return 0;
	Replace(func, jitFunc);
	return reinterpret_cast<JITFunction>(jitFunc);
}

// Installs the new code of a compiled function. The calls under way may still execute
// the old code, which is released with the new one, along with the code that the
// function had before and the profiles. Must be called with the lock held by the
// thread that is compiling the function, see compiling
void CJITCompiler::SImpl::Replace(asCScriptFunction *func, asJITFunction code)
{
	SEntry *entry = FindEntry(func);
	if( !entry )
		return;
	asJITFunction current = reinterpret_cast<asJITFunction>(entry->code.load(std::memory_order_acquire));
	std::map<asJITFunction, SHistory>::iterator old = histories.find(current);
	SHistory &history = histories[code];
	history.retired.push_back(current);
	if( old != histories.end() )
	{
		history.retired.insert(history.retired.end(), old->second.retired.begin(), old->second.retired.end());
		history.profiles.insert(history.profiles.end(), old->second.profiles.begin(), old->second.profiles.end());
		histories.erase(old);
	}
	entry->code.store(reinterpret_cast<JITFunction>(code), std::memory_order_release);
}

// Called by the code that checks for suspension and line callbacks only where they may
// have been requested when the VM enters it while a line callback is set or a
// suspension is requested, see SJITCodeGenOptions::exactEntry. The function goes on in
// its code compiled again with the checks at every statement for the line callbacks,
// and in the VM otherwise, after the JitEntry instruction
int CJITCompiler::SImpl::ExactEntry(void *impl, asSVMRegisters *regs, asPWORD jitArg)
{
	asCContext *ctx = static_cast<asCContext*>(regs->ctx);
	if( ctx->m_lineCallback )
	{
		JITFunction code = static_cast<SImpl*>(impl)->CompileExact(ctx->m_currentFunction);
		if( code )
			return code(regs, jitArg & JIT_ENTRY_INDEX_MASK, 0, 0);
	}
	regs->programPointer += 1 + AS_PTR_SIZE;
	return 0;
}

// Resolves an immutable function wrapper to its current code. Deferred functions
// count calls and loop iterations here, then publish their code through the entry.
// The entry of a script method also sees the call state of its caller, from which the
// classes at the virtual and interface calls of deferred functions are profiled.
asPWORD CJITCompiler::SImpl::ResolveEntry(SEntry *entry, asSVMRegisters *regs, asPWORD jitArg)
{
	if( entry->impl->deferredProfiles.load(std::memory_order_relaxed) )
		entry->impl->NoteVMClass(regs);
	JITFunction code = entry->code.load(std::memory_order_acquire);
	if( code == 0 && !entry->counts.empty() )
	{
		asUINT index = asUINT(jitArg & JIT_ENTRY_INDEX_MASK);
		asUINT entryIndex = index ? index - 1 : 0;
		if( entryIndex < entry->counts.size() )
		{
			std::atomic<asUINT> &counter = entry->counts[entryIndex];
			asUINT count = counter.load(std::memory_order_relaxed);
			while( count && !counter.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel, std::memory_order_relaxed) ) {}
			if( count == 1 )
				code = entry->impl->TierUp(entry, static_cast<asCContext*>(regs->ctx)->m_lineCallback);
		}
		if( code == 0 )
			code = entry->code.load(std::memory_order_acquire);
	}
	return asPWORD(code);
}

// Continues in the VM when a deferred function has no code yet. The VM enters at
// the JitEntry at programPointer; native callers enter at the first instruction.
int CJITCompiler::SImpl::ContinueInVM(SEntry *entry, asSVMRegisters *regs, asPWORD jitArg, asUINT, asDWORD *stackPointer)
{
	asDWORD *byteCode = entry->func->scriptData->byteCode.AddressOf();
	if( jitArg )
	{
		regs->programPointer += 1 + AS_PTR_SIZE;
		return 0;
	}

	// Set up the frame for the VM like asCContext::CallScriptFunction, which goes on
	// after the first JitEntry unless it must stop.
#if AS_PTR_SIZE == 2
	regs->stackPointer = stackPointer;
#endif
	static_cast<asCContext*>(regs->ctx)->m_currentFunction = entry->func;
	regs->programPointer = byteCode;
	if( JIT_PrepareFrame(regs) == 0 )
		regs->programPointer += 1 + AS_PTR_SIZE;
	return 1;
}

void CJITCompiler::ReleaseJITFunction(asJITFunction func)
{
	if( func == 0 )
		return;

	std::lock_guard<std::mutex> lock(m_impl->mutex);
	// AOT functions are returned directly on platforms where wrappers cannot be
	// generated, and are part of the application rather than the JIT runtime.
	if( m_impl->aotPointers.count(func) )
		return;
	std::map<asJITFunction, SImpl::SEntry*>::iterator found = m_impl->entries.find(func);
	if( found == m_impl->entries.end() )
		return;
	SImpl::SEntry *entry = found->second;
	m_impl->FinishVMProfile(entry);
	asJITFunction code = reinterpret_cast<asJITFunction>(entry->code.load(std::memory_order_acquire));
	bool releasable = code && !m_impl->aotPointers.count(code);

	// The code that the function had before goes with it, see Recompile
	std::map<asJITFunction, SImpl::SHistory>::iterator it = m_impl->histories.find(code);
	if( it != m_impl->histories.end() )
	{
		for( size_t n = 0; n < it->second.retired.size(); n++ )
			if( !m_impl->aotPointers.count(it->second.retired[n]) )
				m_impl->Release(it->second.retired[n]);
		for( size_t n = 0; n < it->second.profiles.size(); n++ )
			delete it->second.profiles[n];
		m_impl->histories.erase(it);
	}
	if( releasable )
		m_impl->Release(code);
	m_impl->Release(func);
	m_impl->functionEntries.erase(entry->func);
	m_impl->entries.erase(found);
	delete entry;
	if( releasable )
		m_impl->stats.functionsReleased++;
}

// Frees the code and its unwind information. Must be called with the lock held
void CJITCompiler::SImpl::Release(asJITFunction code)
{
	exactCode.erase(code);
	std::map<asJITFunction, void*>::iterator it = unwindInfo.find(code);
	if( it != unwindInfo.end() )
	{
		CJITUnwindInfo::Unregister(it->second);
		unwindInfo.erase(it);
	}
	if( runtime.release(code) == asmjit::Error::kOk )
	{
		std::map<asJITFunction, size_t>::iterator size = codeSizes.find(code);
		if( size != codeSizes.end() )
		{
			stats.codeSize -= size->second;
			codeSizes.erase(size);
		}
	}
}

END_AS_NAMESPACE
