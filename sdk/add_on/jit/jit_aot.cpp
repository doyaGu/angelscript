#include "jit_aot.h"
#include "jit_cppgen.h"

// Internal engine headers. The JIT must be compiled with the same
// configuration as the engine library (see CMakeLists.txt)
#include "as_scriptfunction.h"

#include <filesystem>
#include <set>
#include <stdio.h>
#include <string.h>
#include <vector>

BEGIN_AS_NAMESPACE

// Changes whenever the generated code changes, so that the code generated before
// isn't used for the functions anymore
const asQWORD JIT_AOT_FORMAT_VERSION = 30;

// The variables that the code keeps in local variables
const asUINT JIT_AOT_MAX_LOCALS = 31;

// Functions per file
const asUINT JIT_AOT_CHUNK_SIZE = 256;

// Two independent 64bit hashes, so that functions with different code practically
// never get the same key
class CJITAOTHasher
{
public:
	CJITAOTHasher() : m_h0(0xcbf29ce484222325ull), m_h1(0x6a09e667f3bcc909ull) {}

	void Add(asQWORD w)
	{
		// FNV-1a over the bytes
		for( int n = 0; n < 8; n++ )
		{
			m_h0 ^= (w >> (n * 8)) & 0xFF;
			m_h0 *= 0x100000001b3ull;
		}
		m_h1 = (m_h1 ^ w) * 0x9E3779B97F4A7C15ull;
		m_h1 ^= m_h1 >> 29;
	}

	SJITAOTKey Get() const
	{
		SJITAOTKey key;
		key.key0 = m_h0;
		key.key1 = m_h1;
		return key;
	}

protected:
	asQWORD m_h0;
	asQWORD m_h1;
};

static void AddSystemCallKey(CJITAOTHasher &hash, const SJITSystemCall &call)
{
	hash.Add(1 | (call.obj << 1) | (call.thisFromStack << 3) | (call.auxiliaryThis << 4) |
		(call.virtualThis << 5) | (call.adjustThis << 6) | (call.retOnStack << 7) |
		(call.retInMemory << 8) | (call.retAfterThis << 9) | (call.returnAutoHandle << 10) |
		(call.cleanArgs << 11) | (call.ret << 12) | (call.retParts << 15) |
		(call.retBytes << 18));
	hash.Add(call.stdCall);
	hash.Add(call.popSize);
	hash.Add(call.args.size());
	for( asUINT a = 0; a < call.args.size(); a++ )
		hash.Add(call.args[a]);
}

// The operands that are part of the key
enum
{
	KEY_W0    = 0x01, // asBC_WORDARG0
	KEY_W1    = 0x02, // asBC_WORDARG1
	KEY_W2    = 0x04, // asBC_WORDARG2
	KEY_D1    = 0x08, // the dwords after the first one
	KEY_D2    = 0x10,
	KEY_D3    = 0x20,
	KEY_SETG4 = 0x40, // the value of asBC_SetG4, after the pointer
	KEY_ALL   = 0x80  // every dword of the instruction
};

// The operands that the generated code depends on. Pointers, function ids, and type
// ids differ between the modules and engines, and are read from the bytecode at run
// time instead, see CJITCppGen. The types of the pointer operands are the ones of the
// dwords or qwords of the same size, so the instructions with pointers are listed
static asUINT GetKeyOperands(asEBCInstr op)
{
	switch( op )
	{
	case asBC_CALL:
	case asBC_CALLBND:
	case asBC_CALLINTF:
	case asBC_TYPEID:
	case asBC_Cast:
	case asBC_Thiscall1:
	case asBC_STR:
	case asBC_PshGPtr:
	case asBC_PshG4:
	case asBC_REFCPY:
	case asBC_OBJTYPE:
	case asBC_LDG:
	case asBC_PGA:
	case asBC_JitEntry:
	case asBC_FuncPtr:
		return 0;
	case asBC_LdGRdR4:
	case asBC_FREE:
	case asBC_CpyGtoV4:
	case asBC_RefCpyV:
	case asBC_CpyVtoG4:
	case asBC_ALLOC:
		return KEY_W0;
	case asBC_SetG4:
		return KEY_SETG4;
	case asBC_LoadRObjR:
	case asBC_LoadVObjR:
		return KEY_W0 | KEY_W1;
	case asBC_SetListType:
		return KEY_W0 | KEY_D1;
	default:
		break;
	}

	switch( asBCInfo[op].type )
	{
	case asBCTYPE_NO_ARG:
		return 0;
	case asBCTYPE_W_ARG:
	case asBCTYPE_wW_ARG:
	case asBCTYPE_rW_ARG:
	case asBCTYPE_W_DW_ARG: // the dword is a type id or a function id for these
		return KEY_W0;
	case asBCTYPE_DW_ARG:
		return KEY_D1;
	case asBCTYPE_rW_DW_ARG:
	case asBCTYPE_wW_DW_ARG:
		return KEY_W0 | KEY_D1;
	case asBCTYPE_QW_ARG:
	case asBCTYPE_DW_DW_ARG:
		return KEY_D1 | KEY_D2;
	case asBCTYPE_wW_rW_rW_ARG:
		return KEY_W0 | KEY_W1 | KEY_W2;
	case asBCTYPE_wW_QW_ARG:
	case asBCTYPE_rW_QW_ARG:
		return KEY_W0 | KEY_D1 | KEY_D2;
	case asBCTYPE_wW_rW_ARG:
	case asBCTYPE_rW_rW_ARG:
	case asBCTYPE_wW_W_ARG:
	case asBCTYPE_W_rW_ARG:
		return KEY_W0 | KEY_W1;
	case asBCTYPE_wW_rW_DW_ARG:
	case asBCTYPE_rW_W_DW_ARG:
		return KEY_W0 | KEY_W1 | KEY_D2;
	case asBCTYPE_QW_DW_ARG:
		return KEY_D1 | KEY_D2 | KEY_D3;
	case asBCTYPE_rW_DW_DW_ARG:
	case asBCTYPE_W_DW_DW_ARG:
		return KEY_W0 | KEY_D1 | KEY_D2;
	case asBCTYPE_W_QW_DW_ARG:
		return KEY_W0 | KEY_D1 | KEY_D2 | KEY_D3;
	default:
		return KEY_ALL;
	}
}

SJITAOTKey JIT_GetAOTKey(const CJITByteCode &code, const std::map<asFUNCTION_t, SJITIndexer> *indexers)
{
	// Borrowing handle arguments depends on the bytecode of the statically called
	// functions. Hash the result of the same analysis that generates the code, rather
	// than engine-local function ids or pointers.
	CJITByteCode analysed(code);
	analysed.AnalyseForAOT(JIT_AOT_MAX_LOCALS, indexers);
	asCScriptFunction *func = analysed.GetFunction();
	CJITAOTHasher hash;
	hash.Add(JIT_AOT_FORMAT_VERSION);
	hash.Add(ANGELSCRIPT_VERSION);
	hash.Add(AS_PTR_SIZE);

	// The direct calls of registered functions depend on the ABI, and on the functions
	const char *abi = CJITCppGen::GetABI();
	hash.Add(abi ? strlen(abi) : 0);
	for( const char *c = abi; c && *c; c++ )
		hash.Add(asBYTE(*c));

	hash.Add(func->scriptData->variableSpace);
	hash.Add(func->scriptData->stackNeeded);

	std::vector<int> heap;
	CJITCppGen::GetHeapVariables(func, heap);
	hash.Add(heap.size());
	for( asUINT n = 0; n < heap.size(); n++ )
		hash.Add(asQWORD(asINT64(heap[n])));

	// The named variables aren't temporary, see CJITByteCode::AnalyseSlots
	std::set<int> named;
	const asCArray<asSScriptVariable*> &vars = func->scriptData->variables;
	for( asUINT n = 0; n < vars.GetLength(); n++ )
		if( vars[n]->stackOffset > 0 && vars[n]->name.GetLength() > 0 )
			named.insert(vars[n]->stackOffset);
	hash.Add(named.size());
	for( std::set<int>::const_iterator it = named.begin(); it != named.end(); ++it )
		hash.Add(asQWORD(asINT64(*it)));

	hash.Add(analysed.GetBorrowableParams());
	hash.Add(analysed.GetLength());
	const std::vector<SJITInstr> &instrs = analysed.GetInstructions();
	for( asUINT n = 0; n < instrs.size(); n++ )
	{
		const SJITInstr &instr = instrs[n];
		const asDWORD *bc = instr.bc;
		hash.Add(asQWORD(instr.op) | ((instr.flags & JIT_INSTR_SKIP) ? 0x100 : 0) |
		         ((instr.flags & JIT_INSTR_BORROW) ? 0x200 : 0));
		if( instr.op == asBC_CALL || instr.op == asBC_CALLINTF )
			hash.Add(analysed.GetBorrowedArgs(n));

		asUINT operands = GetKeyOperands(instr.op);
		if( operands & KEY_ALL )
		{
			hash.Add(bc[0] >> 8);
			for( asUINT d = 1; d < instr.size; d++ )
				hash.Add(bc[d]);
			continue;
		}
		if( operands & KEY_W0 )    hash.Add(asBC_WORDARG0(bc));
		if( operands & KEY_W1 )    hash.Add(asBC_WORDARG1(bc));
		if( operands & KEY_W2 )    hash.Add(asWORD(asBC_SWORDARG2(bc)));
		if( operands & KEY_D1 )    hash.Add(bc[1]);
		if( operands & KEY_D2 )    hash.Add(bc[2]);
		if( operands & KEY_D3 )    hash.Add(bc[3]);
		if( operands & KEY_SETG4 ) hash.Add(bc[1 + AS_PTR_SIZE]);

		// Nor is the type, but how its handles are copied and released
		if( instr.op == asBC_FREE || instr.op == asBC_REFCPY || instr.op == asBC_RefCpyV )
			hash.Add(CJITByteCode::GetRefKind(func->engine, reinterpret_cast<asCTypeInfo*>(asBC_PTRARG(bc))));
		// Registered types are allocated and, where the ABI is supported, constructed
		// directly. The pointers and function ids still come from the bytecode at run time.
		if( instr.op == asBC_ALLOC )
		{
			int id = asBC_INTARG(bc + AS_PTR_SIZE);
			if( id == 0 )
				hash.Add(0);
			else
			{
				SJITSystemCall call;
				if( CJITCppGen::GetConstructorCall(func->engine, id, call) )
				{
					hash.Add(2);
					AddSystemCallKey(hash, call);
				}
				else
					hash.Add(1);
			}
		}

		// The function id isn't, but how the function is called, or where the
		// indexers compiled in place find the element
		if( instr.op == asBC_CALLSYS || instr.op == asBC_Thiscall1 )
		{
			SJITIndexerCall indexer;
			if( CJITByteCode::FindIndexer(func->engine, asBC_INTARG(bc), indexers, indexer) )
			{
				hash.Add(2 | (indexer.indirect << 2) | (asQWORD(indexer.elementSize) << 3));
				hash.Add(asQWORD(indexer.layout.bufferOffset));
				hash.Add(asQWORD(indexer.layout.lengthOffset));
				hash.Add(asQWORD(indexer.layout.dataOffset));
				continue;
			}

			SJITSystemCall call;
			if( !CJITCppGen::GetSystemCall(func->engine, asBC_INTARG(bc), call) )
			{
				hash.Add(0);
				continue;
			}
			AddSystemCallKey(hash, call);
		}
	}

	return hash.Get();
}

std::string JIT_GetAOTName(const SJITAOTKey &key)
{
	char name[64];
	snprintf(name, sizeof(name), "aot_%016llx_%016llx", (unsigned long long)key.key0, (unsigned long long)key.key1);
	return name;
}

// The name of the code for the function that a call calls, see CJITCppGen, with the
// indexers in the parameter. Whether the code is written is known when the output is
static std::string GetTargetName(asCScriptFunction *func, void *indexers)
{
	CJITByteCode code;
	if( code.Decode(func) < 0 )
		return "";
	return JIT_GetAOTName(JIT_GetAOTKey(code, static_cast<const std::map<asFUNCTION_t, SJITIndexer>*>(indexers)));
}

// The code with the regions that call the code in the set, without the lines that
// begin and end them, see JIT_CPPGEN_REGION. The other regions are removed, and so
// are all without a set. The names of the code that the kept regions call are added
// to the calls
static std::string FilterRegions(const std::string &text, const std::set<std::string> *keep, std::set<std::string> *calls)
{
	std::string out;
	bool skip = false;
	size_t pos = 0;
	while( pos < text.size() )
	{
		size_t end = text.find('\n', pos);
		end = end == std::string::npos ? text.size() : end + 1;
		if( text[pos] == JIT_CPPGEN_REGION )
		{
			std::string name = text.substr(pos + 1, end - pos - 1);
			if( !name.empty() && name[name.size() - 1] == '\n' )
				name.erase(name.size() - 1);
			skip = keep == 0 || keep->find(name) == keep->end();
			if( !skip && calls )
				calls->insert(name);
		}
		else if( text[pos] == JIT_CPPGEN_REGION_END )
			skip = false;
		else if( !skip )
			out.append(text, pos, end - pos);
		pos = end;
	}
	return out;
}

CJITAOTOutput::CJITAOTOutput()
{
}

CJITAOTOutput::EResult CJITAOTOutput::Add(const CJITByteCode &code, const SJITAOTKey &key, const char *decl, const std::map<asFUNCTION_t, SJITIndexer> *indexers)
{
	// The code is generated from an analysis of its own, which the JIT compiler
	// doesn't do
	CJITByteCode analysed(code);
	analysed.AnalyseForAOT(JIT_AOT_MAX_LOCALS, indexers);

	std::string name = JIT_GetAOTName(key);
	std::string text;
	CJITCppGen gen(analysed, GetTargetName, const_cast<std::map<asFUNCTION_t, SJITIndexer>*>(indexers));
	if( !gen.Generate(name.c_str(), text) )
		return AOT_FAILED;

	// The calls of the first function with the key are kept
	std::map<SJITAOTKey, SFunc>::iterator it = m_funcs.find(key);
	if( it != m_funcs.end() )
	{
		if( !it->second.conflict && FilterRegions(it->second.text, 0, 0) == FilterRegions(text, 0, 0) )
			return AOT_EXISTS;
		it->second.conflict = true;
		return AOT_CONFLICT;
	}

	std::string direct;
	if( !gen.Generate(name.c_str(), direct, true) )
		return AOT_FAILED;

	// The declaration is only a comment, which must not continue on the next line
	SFunc &f = m_funcs[key];
	f.text     = text;
	f.direct   = direct;
	f.decl     = decl ? decl : "";
	f.conflict = false;
	for( size_t n = 0; n < f.decl.size(); n++ )
		if( (unsigned char)f.decl[n] < 32 || (unsigned char)f.decl[n] >= 128 || f.decl[n] == '\\' )
			f.decl[n] = '?';
	return AOT_ADDED;
}

// Writes the file unless it has the content already, which saves the build from compiling it again
static bool WriteIfChanged(const std::string &path, const std::string &content)
{
	FILE *file = fopen(path.c_str(), "rb");
	if( file )
	{
		std::string old;
		char buffer[4096];
		size_t n;
		while( (n = fread(buffer, 1, sizeof(buffer), file)) > 0 )
			old.append(buffer, n);
		fclose(file);
		if( old == content )
			return true;
	}

	file = fopen(path.c_str(), "wb");
	if( file == 0 )
		return false;
	bool ok = fwrite(content.data(), 1, content.size(), file) == content.size();
	ok = fclose(file) == 0 && ok;
	return ok;
}

static std::string ChunkPath(const std::string &dir, asUINT index)
{
	char name[32];
	snprintf(name, sizeof(name), "jit_aot_%03u.cpp", index);
	return (std::filesystem::path(dir) / name).string();
}

static asUINT FindGroup(std::vector<asUINT> &parent, asUINT n)
{
	while( parent[n] != n )
		n = parent[n] = parent[parent[n]];
	return n;
}

int CJITAOTOutput::Write()
{
	if( m_dir.empty() )
		return asINVALID_ARG;

	std::error_code error;
	std::filesystem::create_directories(m_dir, error);
	if( error )
		return asERROR;

	const char *header =
		"// Generated by the AngelScript JIT compiler, see CJITCompiler::SetAOTOutput. Don't edit\n"
		"#include \"jit_aot_runtime.h\"\n\n";
	char sizeCheck[128];
	snprintf(sizeCheck, sizeof(sizeCheck), "static_assert(AS_PTR_SIZE == %d, \"The code was generated for another pointer size\");\n\n", AS_PTR_SIZE);
	std::string check = sizeCheck;
	if( CJITCppGen::GetABI() )
	{
		check += "#if !(";
		check += CJITCppGen::GetABI();
		check += ")\n#error \"The code was generated for another ABI\"\n#endif\n\n";
	}

	// The functions are sorted by key, so that the same functions end up in the same files
	std::vector<const std::pair<const SJITAOTKey, SFunc>*> funcs;
	std::map<std::string, asUINT> index;
	std::set<std::string> written;
	for( std::map<SJITAOTKey, SFunc>::const_iterator it = m_funcs.begin(); it != m_funcs.end(); ++it )
	{
		if( it->second.conflict )
			continue;
		std::string name = JIT_GetAOTName(it->first);
		index[name] = asUINT(funcs.size());
		written.insert(name);
		funcs.push_back(&*it);
	}

	// The calls to the code that is written call it directly, so the direct entries
	// of the functions called so are written too, and those of the functions they call
	std::vector<std::string> texts(funcs.size()), directs(funcs.size());
	std::vector<std::set<std::string> > calls(funcs.size());
	std::vector<bool> called(funcs.size(), false);
	std::vector<asUINT> work;
	for( asUINT n = 0; n < funcs.size() || !work.empty(); )
	{
		std::set<std::string> found;
		if( n < funcs.size() )
		{
			texts[n] = FilterRegions(funcs[n]->second.text, &written, &found);
			calls[n].insert(found.begin(), found.end());
			n++;
		}
		else
		{
			asUINT f = work.back();
			work.pop_back();
			directs[f] = FilterRegions(funcs[f]->second.direct, &written, &found);
			calls[f].insert(found.begin(), found.end());
		}
		for( std::set<std::string>::const_iterator it = found.begin(); it != found.end(); ++it )
		{
			asUINT c = index[*it];
			if( !called[c] )
			{
				called[c] = true;
				work.push_back(c);
			}
		}
	}

	// The functions that call each other are put in the same file, so that the
	// compiler can inline the calls, unless there are too many
	std::vector<asUINT> parent(funcs.size());
	for( asUINT n = 0; n < funcs.size(); n++ )
		parent[n] = n;
	for( asUINT n = 0; n < funcs.size(); n++ )
		for( std::set<std::string>::const_iterator it = calls[n].begin(); it != calls[n].end(); ++it )
		{
			asUINT a = FindGroup(parent, n), b = FindGroup(parent, index[*it]);
			parent[a] = b;
		}

	std::vector<std::vector<asUINT> > groups;
	std::map<asUINT, asUINT> groupOf;
	for( asUINT n = 0; n < funcs.size(); n++ )
	{
		asUINT root = FindGroup(parent, n);
		std::map<asUINT, asUINT>::iterator it = groupOf.find(root);
		if( it == groupOf.end() )
		{
			it = groupOf.insert(std::make_pair(root, asUINT(groups.size()))).first;
			groups.push_back(std::vector<asUINT>());
		}
		groups[it->second].push_back(n);
	}

	std::vector<std::vector<asUINT> > chunkFuncs;
	std::vector<asUINT> chunkOf(funcs.size());
	for( asUINT g = 0; g < groups.size(); g++ )
	{
		if( chunkFuncs.empty() || (!chunkFuncs.back().empty() && chunkFuncs.back().size() + groups[g].size() > JIT_AOT_CHUNK_SIZE) )
			chunkFuncs.push_back(std::vector<asUINT>());
		for( asUINT n = 0; n < groups[g].size(); n++ )
		{
			if( chunkFuncs.back().size() >= JIT_AOT_CHUNK_SIZE )
				chunkFuncs.push_back(std::vector<asUINT>());
			chunkFuncs.back().push_back(groups[g][n]);
			chunkOf[groups[g][n]] = asUINT(chunkFuncs.size() - 1);
		}
	}

	// The direct entries only called in their file are static
	std::vector<bool> local(funcs.size(), true);
	for( asUINT n = 0; n < funcs.size(); n++ )
		for( std::set<std::string>::const_iterator it = calls[n].begin(); it != calls[n].end(); ++it )
			if( chunkOf[index[*it]] != chunkOf[n] )
				local[index[*it]] = false;

	bool ok = true;
	asUINT chunks = 0;
	for( ; chunks < chunkFuncs.size(); chunks++ )
	{
		const std::vector<asUINT> &members = chunkFuncs[chunks];
		std::string content = header;
		content += check;
		content += "BEGIN_AS_NAMESPACE\n\n";

		std::set<std::string> referenced;
		for( asUINT m = 0; m < members.size(); m++ )
			referenced.insert(calls[members[m]].begin(), calls[members[m]].end());
		for( std::set<std::string>::const_iterator it = referenced.begin(); it != referenced.end(); ++it )
		{
			content += "int " + *it + "(asSVMRegisters *, asPWORD, asUINT, asDWORD *);\n";
			content += local[index[*it]] ? "static int " : "int ";
			content += *it + "_d(asCContext *, asCScriptFunction *, asDWORD *, asUINT, asUINT);\n";
		}

		for( asUINT m = 0; m < members.size(); m++ )
		{
			asUINT f = members[m];
			content += "\n// ";
			content += funcs[f]->second.decl;
			content += "\n";
			if( called[f] )
			{
				if( local[f] )
					content += "static ";
				content += directs[f];
			}
			content += texts[f];
		}
		content += "\nEND_AS_NAMESPACE\n";
		ok = WriteIfChanged(ChunkPath(m_dir, chunks), content) && ok;
	}

	// The table of the functions, which the application registers with CJITCompiler::AddAOTFunctions
	std::string table = header;
	table += check;
	table += "BEGIN_AS_NAMESPACE\n\n";
	for( asUINT n = 0; n < funcs.size(); n++ )
	{
		table += "int ";
		table += JIT_GetAOTName(funcs[n]->first);
		table += "(asSVMRegisters *, asPWORD, asUINT, asDWORD *);\n";
	}
	table += "\nextern const SJITAOTFunction g_jitAOTFunctions[] =\n{\n";
	for( asUINT n = 0; n < funcs.size(); n++ )
	{
		char entry[128];
		snprintf(entry, sizeof(entry), "\t{0x%016llxull, 0x%016llxull, ", (unsigned long long)funcs[n]->first.key0, (unsigned long long)funcs[n]->first.key1);
		table += entry;
		table += JIT_GetAOTName(funcs[n]->first);
		table += "},\n";
	}
	if( funcs.empty() )
		table += "\t{0, 0, 0}\n";
	table += "};\n\n";
	char count[64];
	snprintf(count, sizeof(count), "extern const asUINT g_jitAOTFunctionCount = %u;\n", asUINT(funcs.size()));
	table += count;
	table += "\nEND_AS_NAMESPACE\n";
	ok = WriteIfChanged((std::filesystem::path(m_dir) / "jit_aot_functions.cpp").string(), table) && ok;

	// The files of the chunks that are no longer needed
	for( asUINT n = chunks; ; n++ )
		if( !std::filesystem::remove(ChunkPath(m_dir, n), error) )
			break;

	return ok ? asSUCCESS : asERROR;
}

END_AS_NAMESPACE
