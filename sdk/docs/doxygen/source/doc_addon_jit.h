/**

\page doc_addon_jit JIT compiler

<b>Path:</b> /sdk/add_on/jit/

The JIT compiler translates the bytecode of the script functions to native machine
code as they are compiled or loaded, so that the application doesn't have to rely
on the interpreter of the virtual machine for the heavy lifting. It implements the
\ref doc_adv_jit "JIT compiler interface" (version 1) and generates the machine
code with the <a href="https://asmjit.com">AsmJit</a> library, which is fetched
by the CMake project of the add-on.

The add-on supports x86-64, AArch64, and 32bit x86 on Windows, Linux, macOS, and
Android. On other CPUs CJITCompiler::IsSupported() returns false and the scripts
are interpreted as usual. All 201 bytecode instructions are handled, so the JIT
compiled functions never have to hand control back to the VM except where the VM
is needed, i.e. to raise exceptions, to suspend the execution, or when a script
function is called that couldn't be compiled.

The JIT compiler includes the internal headers of the library in order to call
registered functions and script functions directly from the native code, so it
must be compiled together with the library, or linked to a static library that
was compiled with the same options. It requires a C++20 compiler and CMake 3.24
or later for the AsmJit dependency.

\section doc_addon_jit_1 Public C++ interface

\code
class CJITCompiler : public asIJITCompiler
{
public:
  enum EFlags
  {
    JIT_NO_SUSPEND        = 0x01, // don't check for suspension/line callbacks at statement boundaries
    JIT_NO_SCRIPT_CALLS   = 0x02, // return to the VM for script-to-script calls instead of calling natively
    JIT_NO_REGISTER_CACHE = 0x04, // keep all local variables in memory
    JIT_SYNC_EVERY_INSTR  = 0x08, // update the VM registers after every instruction (debugging aid)
    JIT_LOG               = 0x10, // log the bytecode and generated code to the log file
    JIT_DIRECT_SYSTEM_CALLS    = 0x20, // call registered functions directly on all platforms (see below)
    JIT_NO_DIRECT_SYSTEM_CALLS = 0x40, // never call registered functions directly
    JIT_NO_INLINE              = 0x80  // always call the script functions instead of compiling small ones in place
  };

  CJITCompiler(asDWORD flags = 0);

  // Returns true if the JIT compiler can generate code for the current CPU
  static bool IsSupported();

  // asIJITCompiler
  int  CompileFunction(asIScriptFunction *function, asJITFunction *output);
  void ReleaseJITFunction(asJITFunction func);

  // Configuration (should be set before any function is compiled)
  void    SetFlags(asDWORD flags);
  asDWORD GetFlags() const;
  void    SetLogFile(FILE *file, const char *funcNameFilter = 0);
  void    SetCompileFilter(JITCompileFilterFunc_t filter, void *userParam);
  void    SetNativeCallDepth(asUINT depth);
  void    SetBailInstructions(const asEBCInstr *instructions, asUINT count);
  void    SetMaxFunctionSize(asUINT sizeInDWords);
  void    SetMaxInlineSize(asUINT sizeInDWords);

  SJITStatistics GetStatistics() const;

  // Faster versions of the methods of the context for calling script functions
  int Prepare(asIScriptContext *ctx, asIScriptFunction *func);
  int Execute(asIScriptContext *ctx);
};
\endcode

\section doc_addon_jit_2 Usage

The JIT compiler is attached to the engine before any script is compiled. The engine
must also be told to include the JIT instructions in the bytecode, otherwise there
are no entry points for the native code. The compiler object must outlive the engine
since the engine calls it to release the native code when the functions are destroyed.

\code
CJITCompiler *jit = new CJITCompiler();
asIScriptEngine *engine = asCreateScriptEngine();
engine->SetEngineProperty(asEP_INCLUDE_JIT_INSTRUCTIONS, true);
engine->SetJITCompiler(jit);

... compile and execute scripts as usual ...

engine->ShutDownAndRelease();
delete jit;
\endcode

Nothing else changes for the application. Exceptions, line callbacks, suspension,
debugging with \ref asIScriptContext::GetAddressOfVar "GetAddressOfVar", saving
and loading of bytecode, etc all work the same way as with the interpreter. The
only observable difference is that the bytecode contains the JIT instructions, so
for example the positions reported by \ref asIScriptFunction::GetLineEntry
"GetLineEntry" are not the same as without them.

The generated code calls registered functions directly with their native calling
convention whenever the signature allows it (primitives, references, and handles
as arguments; primitives, references, handles, or value types as return value),
instead of going through the code the VM uses, which marshals the arguments for
the calling convention at runtime. The same goes for the AddRef and Release
behaviours of reference types when handles are copied or objects are freed.
A C++ exception thrown by a function called
this way is still caught and turned into a script exception like with the VM. For
that the exception must be able to pass through the generated code, which needs
unwind information for it. The add-on registers the unwind information on 64bit
Windows and 64bit Linux, and 32bit Windows with MSVC doesn't need any. On other
platforms the direct calls are only made when the library is compiled with
AS_NO_EXCEPTIONS, or when the \ref CJITCompiler::JIT_DIRECT_SYSTEM_CALLS flag is
set, in which case a C++ exception thrown by a registered function that was
called directly terminates the application.
\ref CJITCompiler::JIT_NO_DIRECT_SYSTEM_CALLS turns the direct calls off.

The application can call the script functions through \ref CJITCompiler::Prepare
and \ref CJITCompiler::Execute instead of the methods of the context with the same
names. They have the same effects, but Execute enters the native code of the
function directly instead of through the VM, and Prepare only resets what the
previous execution has changed. This makes the calls of short script functions
from the application about twice as fast. The arguments and the return value
are set and read with the methods of the context as usual, and the methods of the
compiler and of the context can be mixed. Execute leaves the execution to the
context if the engine uses another JIT compiler, if the function isn't compiled,
or if a line callback is set.

\code
jit->Prepare(ctx, func);
ctx->SetArgDWord(0, 42);
if( jit->Execute(ctx) == asEXECUTION_FINISHED )
  result = ctx->GetReturnDWord();
\endcode

Note that the JIT functions are compiled when the module is built, so the build
takes a little longer. For scripts that are compiled often but run rarely a
\ref CJITCompiler::SetCompileFilter "compile filter" can be used to only compile
the functions that matter.

\section doc_addon_jit_perf Performance

The table shows the time in seconds for the tests in the test_performance project
when run with the interpreter, with the JIT compiler without direct system calls
(\ref CJITCompiler::JIT_NO_DIRECT_SYSTEM_CALLS), and with the JIT compiler with
the default settings. Measured on an Intel Core i9-14900K with the 64bit release
build from Visual Studio 2022. The tests Call and Call2 measure calls from the
application into the script engine, where the JIT compiler only adds the cost of
entering the native code. Calls between script functions, including virtual and
interface methods, are made natively, which is what speeds up Fib, Intf, and Mthd.

<pre>
Test           VM       No direct  JIT
Basic          0.240    0.106      0.047
Basic2         0.092    0.005      0.005
Call           0.275    0.297      0.298
Call2          0.352    0.385      0.380
Fib            0.349    0.127      0.121
Int            0.050    0.023      0.010
Intf           0.122    0.042      0.041
Mthd           0.118    0.037      0.037
String         0.232    0.215      0.140
String2        0.154    0.123      0.079
StringPooled   0.158    0.133      0.061
ThisProp       0.220    0.024      0.024
Vector3        0.089    0.075      0.015
Assign.1       0.113    0.008      0.008
Assign.2       0.245    0.017      0.017
Assign.3       0.165    0.014      0.013
Assign.4       0.203    0.018      0.018
Assign.5       0.204    0.018      0.018
Array.1        0.305    0.199      0.155
Array.2        0.145    0.095      0.047
GlobalVar      0.087    0.045      0.025
ClassProp      0.139    0.054      0.033
RetObj.1       0.315    0.253      0.251
RetObj.2       0.194    0.158      0.158
RetObj.3       0.075    0.036      0.036
</pre>

The 32bit x86 build gains about as much, except that Call and Call2 are 11 to 14%
slower than with the interpreter. 64bit integer operations are done by helper
functions on 32bit hosts.

\section doc_addon_jit_limits Known limitations

 - Imported functions and delegates are called through a helper function that
   uses the call stack of the VM, so these calls are not faster than with the
   interpreter.
 - Script functions that call registered functions or release objects are not
   compiled in place of their calls.
 - Unwind information for the generated code is only registered on 64bit Windows
   and 64bit Linux. On the other platforms besides 32bit Windows with MSVC, a C++
   exception that passes through the generated code terminates the application,
   which is why direct system calls are opt-in there unless the library is built
   with AS_NO_EXCEPTIONS.
 - Direct system calls are only made for functions with primitive, reference,
   and handle parameters, and primitive, reference, handle, and value type return
   values. Everything else, including asCALL_GENERIC, goes through the same code
   as the VM.
 - All functions are compiled when the module is built. Lazy compilation would
   require the asIJITCompilerV2 interface, which the add-on doesn't implement.
 - Only x86-64, AArch64, and 32bit x86 are supported, as those are the
   architectures supported by AsmJit's UniCompiler.
 - The deprecated asBC_STR instruction is executed by the VM.

The comment at the top of jit.cpp lists the future work, which addresses most of
these, and the TODO comments in the source files describe the details.

\section doc_addon_jit_3 Debugging aids

Should a script behave differently with the JIT compiler the flags can be used to
narrow down the problem. \ref CJITCompiler::JIT_NO_REGISTER_CACHE turns off the
register allocation for the local variables, \ref CJITCompiler::JIT_NO_INLINE turns
off the inlining, \ref CJITCompiler::JIT_SYNC_EVERY_INSTR
makes the native code update the VM registers after each instruction, and
\ref CJITCompiler::SetBailInstructions forces the listed instructions to be
executed by the VM. With \ref CJITCompiler::JIT_LOG the bytecode and the generated
machine code are written to the log file, optionally only for the functions whose
name contains the filter string given to \ref CJITCompiler::SetLogFile.

\section doc_addon_jit_4 How it works

Each script function is compiled to one native function with the signature of
\ref asJITFunction. The argument stored in the JitEntry instructions is the index
of the entry point, and the native function starts by jumping to the code for that
position. The stack pointer and the value register of the VM are kept in machine
registers while executing natively, and so are the primitive local variables that
are never accessed through their address. Everything is written back to the VM
before a registered function or another script function is called, before the
line callback is invoked, and before returning to the VM.

Compare instructions are fused with the following conditional jump or test, so a
condition in the script becomes a single compare-and-branch. Registered functions
are called directly where possible, and otherwise through the same code as the VM
uses. Calls to other script functions are made natively by calling the native
code of the callee directly, falling back to the VM when the callee isn't compiled
or when the depth of nested native calls exceeds the limit set with
\ref CJITCompiler::SetNativeCallDepth.

The calls of short script functions are compiled in place instead, and so are the
calls that those make in turn, down to 4 levels. The virtual and interface methods
are compiled in place for the only class of the module that implements them, which
the object is checked for, and the objects of other classes call them as usual.
Recursive functions, functions with catch blocks, and functions that call
registered functions, release objects, or make other calls that can't be compiled
in place are always called. The code compiled in place works on the stack frame
that the function would have if it was called, so when the VM is needed there,
for example to raise an exception, the call states of the inlined functions are
created and the VM sees the same call stack as without the inlining.
\ref CJITCompiler::SetMaxInlineSize sets the size of the largest function that is
compiled in place.

Exceptions are never raised from the native code. When a division by zero, a null
pointer access, or similar is detected, the native code returns to the VM at the
faulting instruction and the VM raises the exception with the usual message and
line number.

The native functions that can let a C++ exception through, i.e. those that call
registered functions directly or other script functions natively, are entered from
the VM through a helper function with a try/catch block. When it catches an
exception it does what the VM does for an exception thrown by a registered
function: the translate callback set with
\ref asIScriptEngine::SetTranslateAppExceptionCallback "SetTranslateAppExceptionCallback"
is invoked, and a script exception is raised at the call.

*/
