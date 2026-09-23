#include "jit.h"
#include "jit_bytecode.h"
#include "jit_codegen.h"
#include "jit_runtime.h"
#include "jit_unwind.h"

// Internal engine headers. The JIT must be compiled with the same
// configuration as the engine library (see CMakeLists.txt)
#include "as_scriptfunction.h"

#include <asmjit/ujit.h>
#include <map>
#include <mutex>
#include <string>
#include <string.h>

// Future work, in rough order of expected benefit. See the TODO comments at the
// respective places in the code for the details.
//
//  - Inline calls of script constructors, imported functions, and delegates. They still
//    go through JIT_CallScript (jit_codegen_call.cpp, EmitScriptCall).
//  - Lazy or tiered compilation through asIJITCompilerV2 (CompileFunction below). The
//    engine currently compiles every function when the module is built. A class cannot
//    implement both interface versions, so this would be a second compiler class.
//  - More signatures for direct system calls, and unwind information for the generated
//    code so that C++ exceptions can pass through it (jit_codegen_call.cpp, EmitDirectSystemCall).
//  - Inline reference counting for script objects in REFCPY/FREE (jit_codegen_call.cpp, EmitObjectOp).
//  - Register cache for pointer variables and for more than 32 variables (jit_bytecode.cpp, AnalyseSlots).
//  - Jump tables for switch statements instead of the binary search (jit_codegen.cpp, EmitBranch).
//  - 32bit x86: keep the value register in a register pair, and inline 64bit integer
//    operations instead of calling JIT_I64Op.
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
	asUINT                 maxCachedSlots;
	asUINT                 maxNativeCallDepth;
	bool                   bailOps[asBC_MAXBYTECODE];
	SJITStatistics         stats;
	std::map<asJITFunction, void*> unwindInfo; // registered unwind information by function
};

CJITCompiler::CJITCompiler(asDWORD flags)
{
	m_impl = new SImpl;
	m_impl->flags           = flags;
	m_impl->logFile         = stdout;
	m_impl->filter          = 0;
	m_impl->filterParam     = 0;
	m_impl->maxFunctionSize = 100000;
	m_impl->maxCachedSlots  = 24;
	m_impl->maxNativeCallDepth = 256;
	memset(m_impl->bailOps, 0, sizeof(m_impl->bailOps));
	memset(&m_impl->stats, 0, sizeof(m_impl->stats));
}

CJITCompiler::~CJITCompiler()
{
	// Releasing the runtime frees all code that is still held
	for( std::map<asJITFunction, void*>::iterator it = m_impl->unwindInfo.begin(); it != m_impl->unwindInfo.end(); ++it )
		CJITUnwindInfo::Unregister(it->second);
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
}

// TODO: runtime optimize: With asIJITCompilerV2 the engine only informs the compiler of each
//                         new function with NewFunction, and the native code can be linked
//                         at any time later with SetJITFunction. The VM reads the JIT function
//                         at every JitEntry, so a stub linked in NewFunction could compile the
//                         function when it is first entered, or after it has been entered a
//                         number of times, and replace itself. That would remove the compile
//                         cost for functions that are rarely executed.
int CJITCompiler::CompileFunction(asIScriptFunction *function, asJITFunction *output)
{
	using namespace asmjit;
	using namespace asmjit::ujit;

	*output = 0;

	if( !IsSupported() )
		return asNOT_SUPPORTED;

	asCScriptFunction *func = static_cast<asCScriptFunction*>(function);
	if( func->funcType != asFUNC_SCRIPT || func->scriptData == 0 )
		return asERROR;

	if( m_impl->filter && !m_impl->filter(function, m_impl->filterParam) )
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
	// The dirty masks hold one bit per cached slot
	asUINT maxCachedSlots = m_impl->maxCachedSlots < 32 ? m_impl->maxCachedSlots : 32;
	code.Analyse((m_impl->flags & JIT_NO_REGISTER_CACHE) == 0, maxCachedSlots);
	code.SetBailInstructions(m_impl->bailOps);

	bool log = (m_impl->flags & JIT_LOG) && m_impl->logFile &&
	           (m_impl->logFilter.empty() || strstr(func->GetDeclaration(true, true), m_impl->logFilter.c_str()) != 0);
	if( log )
	{
		fprintf(m_impl->logFile, "\n; ---- %s ----\n", func->GetDeclaration(true, true));
		DumpByteCode(m_impl->logFile, code);
	}

	// Generate the code. Everything here is local to the call so that
	// functions can be compiled concurrently and recursively
	CJITErrorHandler errorHandler;
	CodeHolder holder;
	holder.init(m_impl->runtime.environment(), m_impl->runtime.cpu_features());
	holder.set_error_handler(&errorHandler);

#ifndef ASMJIT_NO_LOGGING
	FileLogger logger(m_impl->logFile);
	if( log )
	{
		logger.add_flags(FormatFlags::kMachineCode);
		holder.set_logger(&logger);
	}
#endif

	BackendCompiler cc;
	holder.attach(&cc);

	UniCompiler uc(&cc, m_impl->runtime.cpu_features(), CpuHints::kNone);
	uc.init_vec_width(VecWidth::k128);

	SJITCodeGenOptions options;
	options.noSuspend      = (m_impl->flags & JIT_NO_SUSPEND) != 0;
	options.noScriptCalls  = (m_impl->flags & JIT_NO_SCRIPT_CALLS) != 0;
	options.syncEveryInstr = (m_impl->flags & JIT_SYNC_EVERY_INSTR) != 0;
	options.directSystemCalls = (m_impl->flags & JIT_DIRECT_SYSTEM_CALLS) != 0;
	options.maxNativeCallDepth = m_impl->maxNativeCallDepth;
#ifdef AS_NO_EXCEPTIONS
	// Without exception handling in the engine nothing is lost by calling directly
	options.directSystemCalls = true;
	options.guardedEntry = false;
#else
	// The C++ exceptions thrown by the functions called directly can only be caught
	// if they can pass through the generated code
	options.guardedEntry = options.directSystemCalls && CJITUnwindInfo::IsSupported();
#endif

	CJITCodeGen gen(uc, code, options);
	bool ok = gen.Generate();
	if( ok )
		ok = (cc.finalize() == Error::kOk);
	if( ok )
		ok = (errorHandler.error == Error::kOk);

	// The functions that C++ exceptions can pass through need unwind information
	CJITUnwindInfo unwind;
	if( ok && gen.IsGuarded() && !unwind.Prepare(cc, gen.GetFuncNode()) )
	{
		ok = false;
		errorHandler.message = "no unwind information for the prologue";
	}

	asJITFunction jitFunc = 0;
	if( ok )
	{
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		ok = (m_impl->runtime.add(&jitFunc, &holder) == Error::kOk);
		void *unwindHandle = 0;
		if( ok && gen.IsGuarded() && !unwind.Register((void*)jitFunc, &unwindHandle) )
		{
			m_impl->runtime.release(jitFunc);
			ok = false;
			errorHandler.message = "the unwind information could not be registered";
		}
		if( unwindHandle )
			m_impl->unwindInfo[jitFunc] = unwindHandle;
		if( ok )
		{
			m_impl->stats.functionsCompiled++;
			m_impl->stats.instructionsCompiled += gen.GetInstructionCount();
			m_impl->stats.instructionsBailed   += gen.GetBailCount();
			m_impl->stats.codeSize             += holder.code_size();
		}
	}

	if( !ok )
	{
		if( log )
			fprintf(m_impl->logFile, "; compilation failed: %s\n", errorHandler.message.c_str());
		std::lock_guard<std::mutex> lock(m_impl->mutex);
		m_impl->stats.functionsFailed++;
		return asERROR;
	}

	if( log )
		fprintf(m_impl->logFile, "; code at %p, %u bytes\n", (void*)jitFunc, (unsigned)holder.code_size());

	// Set the entry point index in the JitEntry instructions
	asDWORD *byteCode = func->scriptData->byteCode.AddressOf();
	const std::vector<asUINT> &entries = code.GetEntries();
	const std::vector<SJITInstr> &instrs = code.GetInstructions();
	for( asUINT n = 0; n < entries.size(); n++ )
		asBC_PTRARG(byteCode + instrs[entries[n]].pos) = asPWORD(n + 1);

	*output = jitFunc;
	return asSUCCESS;
}

void CJITCompiler::ReleaseJITFunction(asJITFunction func)
{
	if( func == 0 )
		return;

	std::lock_guard<std::mutex> lock(m_impl->mutex);
	std::map<asJITFunction, void*>::iterator it = m_impl->unwindInfo.find(func);
	if( it != m_impl->unwindInfo.end() )
	{
		CJITUnwindInfo::Unregister(it->second);
		m_impl->unwindInfo.erase(it);
	}
	m_impl->runtime.release(func);
	m_impl->stats.functionsReleased++;
}

END_AS_NAMESPACE
