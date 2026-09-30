#include "jit.h"
#include "../scriptarray/scriptarray.h"

#include <string.h>

BEGIN_AS_NAMESPACE

// The buffer of the arrays is a protected member, whose offset is found by the
// pointer to it, which the classes derived from the arrays can take
struct SJITArrayAccess : CScriptArray
{
	static SArrayBuffer *CScriptArray::*Buffer() { return &SJITArrayAccess::buffer; }
};

// Finds the layout of an array of 3 ints with room for 20, and checks it after
// resizing the array, and with an array of objects, which are stored as pointers
static bool FindScriptArrayLayout(asIScriptEngine *engine, SJITIndexer &layout)
{
	asITypeInfo *intArray = engine->GetTypeInfoByDecl("array<int>");
	asITypeInfo *objArray = engine->GetTypeInfoByDecl("array<jit_value>");
	if( intArray == 0 || objArray == 0 )
		return false;

	bool found = false;
	CScriptArray *arr = CScriptArray::Create(intArray);
	CScriptArray *objs = CScriptArray::Create(objArray, asUINT(2));
	arr->Reserve(20);
	arr->Resize(3);
	char *obj = reinterpret_cast<char*>(arr);
	char *buf = reinterpret_cast<char*>(arr->*SJITArrayAccess::Buffer());
	char *data = reinterpret_cast<char*>(arr->GetBuffer());
	layout.bufferOffset = int(reinterpret_cast<char*>(&(arr->*SJITArrayAccess::Buffer())) - obj);
	layout.dataOffset = int(data - buf);
	layout.lengthOffset = -1;
	if( buf && layout.dataOffset >= int(sizeof(asUINT)) && layout.dataOffset <= 64 )
	{
		found = true;
		for( int offset = 0; offset + int(sizeof(asUINT)) <= layout.dataOffset; offset += int(sizeof(asUINT)) )
		{
			asUINT value;
			memcpy(&value, buf + offset, sizeof(value));
			if( value != 3 )
				continue;
			found = found && layout.lengthOffset < 0;
			layout.lengthOffset = offset;
		}
		found = found && layout.lengthOffset >= 0;
	}
	if( found )
	{
		arr->Resize(5);
		asUINT length;
		memcpy(&length, buf + layout.lengthOffset, sizeof(length));
		found = reinterpret_cast<char*>(arr->*SJITArrayAccess::Buffer()) == buf && length == 5 &&
		        arr->At(4) == data + 4 * sizeof(int);
	}
	if( found )
	{
		char *objBuf = reinterpret_cast<char*>(objs->*SJITArrayAccess::Buffer());
		void *elem = 0;
		if( objBuf )
			memcpy(&elem, objBuf + layout.dataOffset + sizeof(void*), sizeof(elem));
		found = objBuf && objs->GetBuffer() == objBuf + layout.dataOffset && elem && objs->At(1) == elem;
	}
	objs->Release();
	arr->Release();
	return found;
}

int JIT_AddScriptArrayIndexers(CJITCompiler *jit)
{
#ifdef AS_NO_CLASS_METHODS
	(void)jit;
	return asNOT_SUPPORTED;
#else
	if( jit == 0 )
		return asINVALID_ARG;

	// The arrays of an engine of its own show the layout
	asIScriptEngine *engine = asCreateScriptEngine();
	if( engine == 0 )
		return asERROR;
	RegisterScriptArray(engine, false);
	int r = engine->RegisterObjectType("jit_value", sizeof(int), asOBJ_VALUE | asOBJ_POD | asOBJ_APP_PRIMITIVE);
	SJITIndexer layout;
	bool found = r >= 0 && FindScriptArrayLayout(engine, layout);
	engine->ShutDownAndRelease();
	if( !found )
		return asNOT_SUPPORTED;

	r = jit->AddIndexer(asMETHODPR(CScriptArray, At, (asUINT), void*), layout);
	if( r >= 0 )
		r = jit->AddIndexer(asMETHODPR(CScriptArray, At, (asUINT) const, const void*), layout);
	return r;
#endif
}

END_AS_NAMESPACE
