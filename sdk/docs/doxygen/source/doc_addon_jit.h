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
    JIT_DIRECT_SYSTEM_CALLS = 0x20 // call registered functions directly (see below)
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

  SJITStatistics GetStatistics() const;
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

Calls to registered functions normally go through the same code as the VM uses,
which marshals the arguments for the calling convention at runtime. With
\ref CJITCompiler::JIT_DIRECT_SYSTEM_CALLS the generated code instead calls the
registered functions directly with their native calling convention whenever the
signature allows it (primitives, references, and handles as arguments; primitives,
references, or handles as return value). This is a lot faster for scripts that
call registered functions a lot, but a C++ exception thrown by a registered
function can then no longer be caught and turned into a script exception, so
the option is only enabled by default when the library is compiled with
AS_NO_EXCEPTIONS.

Note that the JIT functions are compiled when the module is built, so the build
takes a little longer. For scripts that are compiled often but run rarely a
\ref CJITCompiler::SetCompileFilter "compile filter" can be used to only compile
the functions that matter.

\section doc_addon_jit_perf Performance

The table shows the time in seconds for the tests in the test_performance project
when run with the interpreter, with the JIT compiler, and with the JIT compiler
and direct system calls. Measured on an Intel Core i9-14900K with the 64bit release
build from Visual Studio 2022. The tests Call and Call2 measure calls from the
application into the script engine, which the JIT compiler can't do anything about.
The tests Fib, Intf, Mthd, and RetObj are dominated by the cost of calling script
functions, which is the same in both cases since the JIT compiled code uses the
call stack of the VM to keep exceptions and debugging working.

<pre>
Test           VM       JIT      JIT direct
Basic          0.245    0.179    0.127
Basic2         0.092    0.009    0.009
Call           0.278    0.292    0.296
Call2          0.362    0.383    0.380
Fib            0.352    0.322    0.325
Int            0.051    0.042    0.029
Intf           0.121    0.124    0.129
Mthd           0.119    0.123    0.126
String         0.231    0.228    0.155
String2        0.153    0.132    0.089
StringPooled   0.154    0.145    0.071
ThisProp       0.221    0.029    0.028
Vector3        0.090    0.077    0.041
Assign.1       0.115    0.012    0.012
Assign.2       0.248    0.021    0.021
Assign.3       0.170    0.018    0.017
Assign.4       0.207    0.023    0.023
Assign.5       0.208    0.023    0.023
Array.1        0.310    0.207    0.191
Array.2        0.148    0.093    0.095
GlobalVar      0.090    0.043    0.044
ClassProp      0.139    0.056    0.056
RetObj.1       0.324    0.329    0.335
RetObj.2       0.202    0.202    0.202
RetObj.3       0.076    0.051    0.052
</pre>

On 32bit x86 the gains for computational code are the same, but scripts dominated by
calls to script functions run 5 to 20% slower than with the interpreter, e.g. Fib,
Intf, Mthd, and RetObj. With only seven general purpose registers and all arguments
passed on the stack, the round trip through the native code costs more than the
few instructions it saves per call. 64bit integer operations are also done by helper
functions on 32bit hosts.

\section doc_addon_jit_limits Known limitations

 - Calls between script functions use the call stack of the VM, so they are not
   faster than with the interpreter. This is the main remaining opportunity for
   improvement and is described in the comments in jit_runtime.cpp.
 - The generated code has no unwind information. A C++ exception that passes
   through it will terminate the application, which is why direct system calls
   are opt-in unless the library is built with AS_NO_EXCEPTIONS.
 - Direct system calls are only made for functions with primitive, reference,
   and handle parameters and return values. Everything else, including
   asCALL_GENERIC, goes through the same code as the VM.
 - All functions are compiled when the module is built. Lazy compilation would
   require the asIJITCompilerV2 interface, which the add-on doesn't implement.
 - Only x86-64, AArch64, and 32bit x86 are supported, as those are the
   architectures supported by AsmJit's UniCompiler.
 - The deprecated asBC_STR instruction is executed by the VM.

The TODO comments in the source files describe how each of these could be addressed.

\section doc_addon_jit_3 Debugging aids

Should a script behave differently with the JIT compiler the flags can be used to
narrow down the problem. \ref CJITCompiler::JIT_NO_REGISTER_CACHE turns off the
register allocation for the local variables, \ref CJITCompiler::JIT_SYNC_EVERY_INSTR
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
condition in the script becomes a single compare-and-branch. Calls to registered
functions go through the same code as the VM uses, and calls to other script
functions are made natively by calling the native code of the callee directly,
falling back to the VM when the callee isn't compiled or when the depth of nested
native calls exceeds the limit set with \ref CJITCompiler::SetNativeCallDepth.

Exceptions are never raised from the native code. When a division by zero, a null
pointer access, or similar is detected, the native code returns to the VM at the
faulting instruction and the VM raises the exception with the usual message and
line number.

*/
