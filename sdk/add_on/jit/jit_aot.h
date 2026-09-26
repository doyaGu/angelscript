#ifndef JIT_AOT_H
#define JIT_AOT_H

#include "jit_bytecode.h"

#include <map>
#include <string>

BEGIN_AS_NAMESPACE

// Identifies the code of a function, see JIT_GetAOTKey
struct SJITAOTKey
{
	asQWORD key0;
	asQWORD key1;

	bool operator<(const SJITAOTKey &o) const { return key0 < o.key0 || (key0 == o.key0 && key1 < o.key1); }
};

// Hashes everything that the code CJITCppGen generates for the decoded function
// depends on: the instructions with the operands that are the same in every module
// and engine, e.g. the offsets of the variables and the branch targets, the size of
// the frame, the variables cleared on entry, and the version of the engine and the
// generator. Two functions with the same key can share the code
SJITAOTKey JIT_GetAOTKey(const CJITByteCode &code);

// The name of the generated function with the key
std::string JIT_GetAOTName(const SJITAOTKey &key);

// Collects the code generated for the compiled functions, one function per key, and
// writes it to C++ files, see CJITCompiler::SetAOTOutput
class CJITAOTOutput
{
public:
	CJITAOTOutput();

	void               SetDirectory(const char *dir) { m_dir = dir ? dir : ""; }
	const std::string &GetDirectory() const          { return m_dir; }

	enum EResult
	{
		AOT_ADDED,    // the code has been generated
		AOT_EXISTS,   // the same code has been generated for another function
		AOT_CONFLICT, // other code has been generated for the key, and neither is written
		AOT_FAILED    // the bytecode can't be translated
	};

	// Generates the code for the function, which is described by the declaration in
	// the comment of the code
	EResult Add(const CJITByteCode &code, const SJITAOTKey &key, const char *decl);

	// Writes the files that differ from the ones in the directory, and removes the
	// ones that are no longer needed. Returns a negative value on failure
	int Write();

protected:
	struct SFunc
	{
		std::string text;
		std::string decl;
		bool        conflict;
	};

	std::string                  m_dir;
	std::map<SJITAOTKey, SFunc>  m_funcs;
};

END_AS_NAMESPACE

#endif
