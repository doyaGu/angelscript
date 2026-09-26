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

  // Memory functions for the engine that are faster for the small objects of the scripts
  static void *AllocMemory(size_t size);
  static void  FreeMemory(void *mem);
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
observable differences are that the bytecode contains the JIT instructions, so
for example the positions reported by \ref asIScriptFunction::GetLineEntry
"GetLineEntry" are not the same as without them, and that the AddRef and Release
behaviours are called less often, as the references that the VM adds and
releases again right after are left out.

The generated code calls registered functions directly with their native calling
convention whenever the signature allows it (primitives, references, and handles
as arguments; primitives, references, handles, or value types as return value),
instead of going through the code the VM uses, which marshals the arguments for
the calling convention at runtime. The same goes for the AddRef and Release
behaviours of reference types when handles are copied or objects are freed.
The handles copied from the temporary variables that are released right after,
for example the result of p.next in @p = p.next, are moved instead, so no
reference is added and released for them. On x86 the generated code counts the
references of the script classes itself, the same way as the engine does, and
only calls their AddRef and Release when an object loses its last reference or
is resurrected while it is being destroyed. The initialization lists that hold only
primitives, enums, or value types without a destructor are freed without going
through their elements. A C++ exception thrown by a function called this way is still caught and turned into a script exception like with the VM. For
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

The engine allocates the script objects, the arrays, and much of its other memory
in small blocks that are freed again soon after. \ref CJITCompiler::AllocMemory
and \ref CJITCompiler::FreeMemory are memory functions for the engine that keep the
freed blocks of up to 1 KB for reuse, which makes the scripts that create many
objects about a third faster with the JIT compiler, and 10 to 20% faster with the
interpreter. Each thread keeps the blocks it frees in lists of its own, so that
most allocations take no lock. The memory of the blocks is never returned to the
system. The functions must be set before the first engine is created, and must
not be changed while memory allocated with them is still in use. They can be used
without attaching the JIT compiler to the engine too.

\code
asSetGlobalMemoryFunctions(CJITCompiler::AllocMemory, CJITCompiler::FreeMemory);
\endcode

The strings of the \ref doc_addon_std_string "string add-on" are allocated by
std::string, which takes the memory from operator new instead. Applications that
execute the scripts in a single thread only can also compile the library with
AS_NO_THREADS, which makes the reference counts of the objects plain integers
instead of atomic ones, and takes the locks out of the memory functions.

Note that the JIT functions are compiled when the module is built, so the build
takes a little longer. For scripts that are compiled often but run rarely a
\ref CJITCompiler::SetCompileFilter "compile filter" can be used to only compile
the functions that matter.

\section doc_addon_jit_perf Performance

The table shows the time in seconds for the tests in the test_performance project
when run with the interpreter, with the JIT compiler without direct system calls
(\ref CJITCompiler::JIT_NO_DIRECT_SYSTEM_CALLS), with the JIT compiler with the
default settings, and with the JIT compiler and the pooled memory functions.
Measured on an Intel Core i9-14900K with the 64bit release build from Visual
Studio 2022. With the JIT compiler the tests Call and Call2 call the script
functions through \ref CJITCompiler::Prepare and \ref CJITCompiler::Execute. Calls
between script functions are made natively, which is what speeds up Fib, and the
methods that Intf and Mthd call are compiled in place. The function that RetObj.3
calls is compiled in place too, and borrows the reference of the handle that it
gets. RetObj.1, RetObj.2, and Array.1 spend much of their time allocating the
objects they create, which the pooled memory functions speed up.

<pre>
Test           VM       No direct  JIT      JIT+pool
Basic          0.252    0.085      0.027    0.027
Basic2         0.092    0.005      0.005    0.005
Call           0.278    0.125      0.124    0.127
Call2          0.368    0.185      0.184    0.195
Fib            0.370    0.086      0.084    0.087
Int            0.055    0.020      0.006    0.006
Intf           0.124    0.006      0.006    0.006
Mthd           0.123    0.006      0.006    0.005
String         0.229    0.202      0.126    0.127
String2        0.152    0.103      0.058    0.058
StringPooled   0.155    0.119      0.043    0.042
ThisProp       0.214    0.017      0.017    0.017
Vector3        0.089    0.072      0.013    0.012
Assign.1       0.114    0.008      0.008    0.008
Assign.2       0.241    0.008      0.008    0.008
Assign.3       0.170    0.011      0.011    0.011
Assign.4       0.207    0.016      0.016    0.016
Assign.5       0.208    0.016      0.016    0.016
Array.1        0.310    0.147      0.104    0.066
Array.2        0.148    0.076      0.032    0.032
GlobalVar      0.089    0.036      0.016    0.016
ClassProp      0.140    0.041      0.018    0.018
RetObj.1       0.328    0.228      0.225    0.136
RetObj.2       0.204    0.111      0.111    0.068
RetObj.3       0.078    0.004      0.003    0.004
</pre>

The 32bit x86 build gains as much or more, as the interpreter is slower there.
RetObj.1 for example takes 0.704 seconds with the interpreter, and 0.160 seconds
with the JIT compiler and the pooled memory functions. 64bit integer operations
are done by helper functions on 32bit hosts.

\section doc_addon_jit_limits Known limitations

 - Imported functions and delegates are called through a helper function that
   uses the call stack of the VM, so these calls are not faster than with the
   interpreter.
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
Recursive functions and functions with catch blocks are always called. The code
compiled in place works on the stack frame that the function would have if it was
called, so when the VM is needed there, for example to raise an exception, and
when it calls registered functions, releases objects, or calls the script
functions that aren't compiled in place, the call states of the inlined functions
are created and the VM and the called functions see the same call stack as
without the inlining. The registered function calls and releases that follow each
other share the call states. The functions compiled in place that only read or
release the handles passed to them borrow the references of the caller on 64bit
hosts, where the variable passed holds a reference that nothing changes until the
function returns, instead of adding and releasing references of their own. They
only get references of their own where the VM takes over their call states.
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
