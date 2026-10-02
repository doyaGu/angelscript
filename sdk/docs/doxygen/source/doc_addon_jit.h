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
are interpreted as usual, unless their code was \ref doc_addon_jit_aot "generated ahead of time"
as C++. All 201 bytecode instructions are handled, so the JIT
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
    JIT_NO_INLINE              = 0x80, // always call the script functions instead of compiling small ones in place
    JIT_AOT_ONLY               = 0x100, // only use the code generated ahead of time, and leave the other functions to the VM
    JIT_CHECK_EVERY_STATEMENT  = 0x200  // check for suspension/line callbacks at every statement like the VM
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
  int     SetCompileThresholds(asUINT calls, asUINT iterations);
  void    SetProfileThreshold(asUINT calls);
  int     AddIndexer(const asSFuncPtr &method, const SJITIndexer &indexer);

  // Ahead of time compilation
  void SetAOTOutput(const char *directory);
  int  WriteAOTOutput();
  int  AddAOTFunctions(const SJITAOTFunction *functions, asUINT count);

  SJITStatistics GetStatistics() const;

  // Faster versions of the methods of the context for calling script functions
  int Prepare(asIScriptContext *ctx, asIScriptFunction *func);
  int Execute(asIScriptContext *ctx);

  // Memory functions for the engine that are faster for the small objects of the scripts
  static void *AllocMemory(size_t size);
  static void  FreeMemory(void *mem);
};

// Where a container keeps its elements, see CJITCompiler::AddIndexer
struct SJITIndexer
{
  int bufferOffset; // offset in the object of the pointer to the buffer, which may be null
  int lengthOffset; // offset in the buffer of the number of elements, an asUINT
  int dataOffset;   // offset in the buffer of the first element
};

// Adds the indexers of the array add-on to the compiler (in jit_scriptarray.cpp)
int JIT_AddScriptArrayIndexers(CJITCompiler *jit);
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
as arguments; primitives, references, handles, or value types as return value,
except the value types that the compilers for 32bit x86 other than MSVC return
in memory),
instead of going through the code the VM uses, which marshals the arguments for
the calling convention at runtime. The same goes for the AddRef and Release
behaviours of reference types when handles are copied or objects are freed.
The handles copied from the temporary variables that are released right after,
for example the result of p.next in @p = p.next, are moved instead, so no
reference is added and released for them. On x86 and AArch64 the generated code
counts the references of the script classes itself like the engine does, and only
calls their AddRef and Release when an object loses its last reference or is
resurrected while it is being destroyed. The initialization lists that hold only
primitives, enums, or value types without a destructor are freed without going
through their elements. A C++ exception thrown by a function called this way is
still caught and turned into a script exception like with the VM. For that the
exception must be able to pass through the generated code, which needs unwind
information for it. The add-on registers the unwind information on 64bit Windows,
and for x86-64 on Linux and AArch64 on Linux and macOS (except arm64e), and 32bit Windows
with MSVC doesn't need any. On other platforms the direct calls are only made
when the library is compiled with AS_NO_EXCEPTIONS, or when the
\ref CJITCompiler::JIT_DIRECT_SYSTEM_CALLS flag is set, in which case a C++
exception thrown by a registered function that was called directly terminates the
application.
\ref CJITCompiler::JIT_NO_DIRECT_SYSTEM_CALLS turns the direct calls off. Neither
flag applies to the code generated ahead of time, see \ref doc_addon_jit_aot.

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

A function that the application calls many times with the same context can be
bound to it with \ref CJITCall, whose Prepare and Execute do what the methods of
the compiler check and look up at every call once, when the call is bound, which
makes the calls about twice as fast again. They have the same effects, fall back
to the methods of the compiler whenever the context isn't where the last call of
the function left it, and can be mixed with the methods of the compiler and of
the context. The call must be used by the thread that bound it.

\code
CJITCall call;
call.Bind(jit, ctx, func);
for( int n = 0; n < count; n++ )
{
  call.Prepare();
  ctx->SetArgDWord(0, n);
  if( call.Execute() == asEXECUTION_FINISHED )
    sum += ctx->GetReturnDWord();
}
\endcode

When \ref asEP_AUTO_GARBAGE_COLLECT is set, which is the default, every execution
ends with a step of the garbage collector if it knows any objects, which takes
longer than the call of a short script function itself. Applications that call
script functions often should turn the property off and call
\ref asIScriptEngine::GarbageCollect themselves at a convenient time, e.g. once
per frame.

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

Note that the JIT functions are compiled when the module is built by default, so
the build takes longer. For scripts that are compiled often but run rarely the
functions can be compiled only once they are executed often enough, see
\ref doc_addon_jit_tiered, and a \ref CJITCompiler::SetCompileFilter "compile filter"
can leave the functions that don't matter to the VM.

\section doc_addon_jit_tiered Tiered compilation

Most scripts spend nearly all of their time in a small part of their functions,
and compiling the others only makes the build slower and takes memory. With
\ref CJITCompiler::SetCompileThresholds "SetCompileThresholds" the compiler leaves
the functions to the VM at first, as HotSpot and LuaJIT leave them to their
interpreters, and compiles each function when it has been called the given number
of times, or when one of its loops has run the given number of iterations.

\code
CJITCompiler *jit = new CJITCompiler();
jit->SetCompileThresholds(1000, 1000);
engine->SetJITCompiler(jit);
\endcode

A function that is compiled in a loop goes on with the loop in the compiled code,
so that a function that loops for a long time, e.g. the main function of a script,
doesn't have to be called again to run natively. The iterations are counted for
each loop across the calls of the function, and a threshold of 0 iterations
doesn't count them. A threshold of 0 calls compiles all functions when the module
is built, which is the default. The thresholds are limited to 16383, and must be
set before any function is compiled, otherwise SetCompileThresholds fails. The
engine must use the CJITCompiler itself, not a compiler that forwards to it, as
the deferred functions call back into it.

Until a function is compiled the VM calls into the compiler to count at the
JitEntry instruction of its first statement and at the first one of each loop,
and the compiled code calls the function through the VM. Once it is compiled,
the compiled code calls it natively. The calls through \ref CJITCompiler::Execute
count as well. The thread that reaches a threshold compiles the function and
continues with its compiled code, while other threads that execute the function
at the same time go on in the VM. A function that can't be compiled stays with
the VM for good. The short functions
are compiled in place in the compiled functions that call them whether they are
compiled themselves or not, and the functions whose code was generated ahead of
time use it right away. In the \ref SJITStatistics "statistics" functionsDeferred
counts the functions whose compilation was deferred, and functionsCompiled the
ones compiled so far, including those compiled when the module was built.
Everything else behaves as with the functions compiled up front: the exceptions,
line callbacks, suspension, saving the bytecode, and the rest work as with the
interpreter.

With the thresholds 2,3 the feature tests of the library compile 636 of their
8756 functions, 11788 instead of 146548 bytecode instructions. The tests of the
test_performance project compile 590 of their 970 functions with the thresholds
2,3, and 330 with 1000,1000. With the thresholds 2,3 the tests take as long as
when all functions are compiled up front, as the functions that run long enough
to matter spend so little of their time in the VM. With 1000,1000 they take 3%
longer by the geometric mean, as the shortest tests spend a larger part of their
time in the VM.

\section doc_addon_jit_profiles Profiles

The virtual and interface methods are compiled in place of their calls if the only
class of the module that implements them, or all the classes that inherit a virtual
method, have the same implementation, see \ref doc_addon_jit_4. Where several classes
of the module implement a method differently, the bytecode doesn't tell which of
them the objects at a call are of, but most calls only ever see objects of one class,
which is what HotSpot builds on too. Such calls note the classes of their objects in
a profile of the compiled code if one of the implementations is small enough to be
compiled in place, and the compiled code counts down their calls. When they have
been made the number of times set with
\ref CJITCompiler::SetProfileThreshold "SetProfileThreshold", 10000 by default, the
function is compiled again if a call has seen only one class since the code was
compiled, with the method of that class compiled in place of the call, which the
object is checked for. The calls are counted again otherwise, and a threshold of 0
doesn't note the classes.

\code
jit->SetProfileThreshold(1000);
\endcode

The objects of other classes call the method as usual. The calls that haven't seen
an object yet, e.g. those of a later loop, go on noting their classes in the new
code, so that the function can be compiled again for them, 3 times at most. The calls
compiled in place note the objects of other classes too, and if the function is
compiled again for another call, they call the method then, as they have seen
several classes. Only the classes of the module of the function are
compared with, as the other modules and their classes may be discarded while the
function still exists. The thread whose call has counted down compiles the function
again, leaves that call to the VM, and goes on in the new code after it, as do the
next calls of the function, while the calls under way in other threads finish in the
old code. The old code is kept until the function is released. In the
\ref SJITStatistics "statistics" functionsRecompiled counts the functions compiled
again. The functions deferred by the \ref doc_addon_jit_tiered "tiered compilation"
note the classes once they are compiled, not while the VM executes them.

Calling a method that 2 classes override 10 million times in two loops, each with
the objects of one class, took 0.011 seconds instead of 0.025 with the profiles, and
calling an interface method of 2 classes the same way 0.009 seconds instead of 0.027.
The function is compiled 3 times in each, once more for the second loop. None of the
other tests of the test_performance project make such calls, so their times don't
change, and with the threshold 2 the feature tests of the library compile 6 of their
8756 functions again.

\section doc_addon_jit_indexers Indexers

The scripts index the arrays and the other containers through methods that are
registered with the engine, so each access is a call of a registered function, even
though the method does little more than check the index and compute the address of
the element. \ref CJITCompiler::AddIndexer "AddIndexer" tells the compiler how such a
container keeps its elements, so that the compiled code finds the element itself
where the method is called. The object must point to a buffer, which holds the
number of elements as an asUINT and then the elements one after the other, and the
methods must be registered as \ref asCALL_THISCALL "thiscall" methods of a template
type with a single subtype, take one uint by value, and return a reference to the
element. The elements are of the size of the subtype, and the objects that aren't
handles are stored as pointers to them, as the array add-on does. The methods are
found by their native function, so the method given must be the same as the one
registered, and the methods registered through a wrapper, or by other types, are
called as usual. They must be added before any function is compiled.

\code
SJITIndexer indexer = { offsetof(CMyArray, buffer), offsetof(SMyBuffer, length), offsetof(SMyBuffer, data) };
jit->AddIndexer(asMETHODPR(CMyArray, At, (asUINT), void*), indexer);
\endcode

The compiled code calls the method where the pointer to the buffer is null or the
index is out of range, which sets the script exception as usual. The file
jit_scriptarray.cpp of the add-on has JIT_AddScriptArrayIndexers, which adds the
At methods of the \ref doc_addon_array "array add-on", whose opIndex they implement,
with the layout of its buffer found by creating some arrays in an engine of its own.
It must be compiled together with the array add-on. The code generated ahead of
time finds the elements the same way. The indexers are part of the key of that
code, so the application must add the same ones when it generates the code and
when it uses it, otherwise the functions that call them don't find their code.

\code
CJITCompiler *jit = new CJITCompiler();
JIT_AddScriptArrayIndexers(jit);
engine->SetJITCompiler(jit);
\endcode

With the indexers of the arrays, ClassProp of the test_performance project takes
0.011 seconds instead of 0.021, Array.2 0.012 instead of 0.028, and Array.1 0.061
instead of 0.070. With the code generated ahead of time ClassProp takes 0.010
seconds instead of 0.021, Array.2 0.010 instead of 0.030, and Array.1 0.065
instead of 0.075.

\section doc_addon_jit_suspend Suspension and line callbacks

The VM checks at every statement whether the execution is to be suspended or a
line callback is to be called. The JIT compiled code only checks where such a request
may have come since the last check: when the function is entered, after the calls of
registered and script functions, which may call \ref asIScriptContext::Suspend "Suspend",
\ref asIScriptContext::Abort "Abort", or \ref asIScriptContext::SetLineCallback "SetLineCallback",
and once in each iteration of a loop. The code goes on in the VM where it finds a
request, and the VM suspends the execution at the next statement or calls the line
callback for it, so the requests of the functions called are answered at the same
statement as with the VM. Those of other threads, e.g. of a watchdog that aborts the
scripts that run too long, are answered by the next iteration of a loop, call, or
return at the latest.

While a line callback is set, the functions that the VM enters, including the one it
goes on in when it reaches the next entry point of its compiled code, are compiled
again with the checks at every statement, and keep that code, also when they are
compiled again with the \ref doc_addon_jit_profiles "profiles". The deferred functions
of the \ref doc_addon_jit_tiered "tiered compilation" that are compiled while a line
callback is set are compiled with the checks right away. So once a debugger has set a
line callback the functions it has seen check at every statement, and the others
don't. Applications that always set a line callback, e.g. to time out the scripts,
can set the flag \ref CJITCompiler::JIT_CHECK_EVERY_STATEMENT, with which all the
functions check at every statement from the start, so that they are only compiled
once. In the \ref SJITStatistics "statistics" functionsForLineCallbacks counts the
functions compiled again for the line callbacks.

Leaving out the other checks makes the tests of the test_performance project 1.13
times as fast by the geometric mean as with the checks at every statement, which is
nearly the 1.15 times that \ref CJITCompiler::JIT_NO_SUSPEND gains by leaving out
all of them. Assign.1 for example takes 0.002 seconds instead of 0.007, and Call
0.113 instead of 0.127. The feature tests of the library compile 52 of their 8756
functions again for the line callbacks.

\section doc_addon_jit_aot Ahead of time compilation

Where the application may not generate machine code at run time, for example on
iOS and the game consoles, or on the CPUs that the JIT compiler doesn't support,
the add-on can generate C++ code for the script functions instead, which is then
compiled into the application. The code is generated by running the application
with an output directory set with \ref CJITCompiler::SetAOTOutput "SetAOTOutput",
so that the compiler generates the C++ code for each function it is given, and
\ref CJITCompiler::WriteAOTOutput "WriteAOTOutput" writes it to the directory.

\code
jit->SetAOTOutput("path/to/aot");
... build or load the scripts ...
if( jit->WriteAOTOutput() < 0 )
  ... report the error ...
\endcode

The directory then holds the files jit_aot_NNN.cpp with up to 256 functions each,
and jit_aot_functions.cpp with the table of the functions, g_jitAOTFunctions. Only
the files that have changed are written, and the ones left over from a previous
run with more functions are removed, so that the next build of the application only
compiles what has changed. The CMake option AS_JIT_AOT_DIR of the add-on names the
directory to build the files from, and defines AS_JIT_AOT_TABLE, with which jit.h
declares the table. The application adds the functions to the compiler before any
script is compiled:

\code
#ifdef AS_JIT_AOT_TABLE
jit->AddAOTFunctions(g_jitAOTFunctions, g_jitAOTFunctionCount);
#endif
\endcode

The code of a function is found by a key computed from its bytecode. The key leaves
out the addresses of the functions, types, and global variables, and the ids of the
functions and types, which the generated code reads from the bytecode as it runs.
This way the code generated by one execution of the application works in any other
that compiles the same scripts or loads their saved bytecode, with any number of
engines, and all functions with the same bytecode share the same code. The key also
includes the version of the library and the size of the pointers. The functions
that no code was generated for, e.g. because the script was changed after the code
was generated, are compiled by the JIT compiler as usual, or left to the VM if the
CPU isn't supported or the \ref CJITCompiler::JIT_AOT_ONLY flag is set. Should the
code of two different functions get the same key, which is very unlikely with 128
bits, neither is written and a warning is sent to the message callback.

The generated functions call each other and the JIT compiled functions natively,
and are called natively by them. Like the JIT compiled functions they call the
registered functions directly, count the references of the script classes
themselves, move the handles out of the temporary variables, free the plain
initialization lists without going through their elements, create the objects of
the script classes without the VM, and find the elements of the
\ref doc_addon_jit_indexers "indexers" themselves. They make the direct calls on every
platform whose calling convention they know, regardless of the flags for the
direct system calls, as the C++ compiler provides the unwind information for them
and they catch the C++ exceptions themselves where they are entered from the VM or
the application. Exceptions, suspension, line callbacks, and the rest work the
same way as with the JIT compiled functions. Like the add-on, the generated code
includes the internal headers of the library, so it must be compiled with the
same options as the library.

As the direct calls depend on the calling convention, the code must be generated
by a build of the application for the same pointer size and calling convention
as the build it is compiled into, e.g. by any 64bit ARM build for iOS. Each file
checks the macros of as_config.h for this, and doesn't compile otherwise. The
direct calls are made on x86-64, 64bit ARM, and 32bit x86. On the other CPUs, on
big endian ones, and with AS_MAX_PORTABILITY the generated code makes none.

\section doc_addon_jit_perf Performance

The table shows the time in seconds for the tests in the test_performance project
when run with the interpreter, with the JIT compiler without direct system calls
(\ref CJITCompiler::JIT_NO_DIRECT_SYSTEM_CALLS), with the JIT compiler with the
default settings, with the JIT compiler and the pooled memory functions, and with
the code generated ahead of time without them. Measured on an Intel Core i9-14900K
with the 64bit release build from Visual Studio 2022, as the median of three runs.
With the JIT compiler and ahead of time the tests Call and Call2 call the script
functions through \ref CJITCompiler::Prepare and \ref CJITCompiler::Execute. Calls
between script functions are made natively, which is what speeds up Fib, and the
methods that Intf and Mthd call are compiled in place. The function that RetObj.3
calls is compiled in place too, and borrows the reference of the handle that it
gets. RetObj.1, RetObj.2, and Array.1 spend much of their time allocating the
objects they create, which the pooled memory functions speed up.

<pre>
Test           VM       No direct  JIT      JIT+pool  AOT
Basic          0.246    0.082      0.026    0.026     0.045
Basic2         0.085    0.005      0.005    0.005     0.009
Call           0.272    0.114      0.113    0.116     0.117
Call2          0.378    0.178      0.180    0.179     0.176
Fib            0.349    0.086      0.085    0.088     0.095
Int            0.053    0.018      0.006    0.006     0.009
Intf           0.126    0.005      0.005    0.005     0.028
Mthd           0.124    0.005      0.005    0.005     0.022
String         0.231    0.198      0.125    0.126     0.137
String2        0.154    0.100      0.057    0.058     0.075
StringPooled   0.152    0.115      0.040    0.040     0.052
ThisProp       0.191    0.017      0.017    0.017     0.022
Vector3        0.090    0.065      0.013    0.013     0.013
Assign.1       0.111    0.002      0.002    0.002     0.009
Assign.2       0.199    0.004      0.004    0.004     0.012
Assign.3       0.164    0.008      0.008    0.008     0.014
Assign.4       0.264    0.014      0.014    0.014     0.019
Assign.5       0.281    0.014      0.014    0.014     0.019
Array.1        0.468    0.149      0.106    0.068     0.119
Array.2        0.182    0.077      0.036    0.036     0.049
GlobalVar      0.109    0.034      0.016    0.016     0.017
ClassProp      0.172    0.042      0.020    0.020     0.026
RetObj.1       0.606    0.226      0.226    0.138     0.242
RetObj.2       0.365    0.110      0.110    0.067     0.146
RetObj.3       0.106    0.002      0.002    0.002     0.027
</pre>

By the geometric mean of the speedups over the interpreter, the tests are 8.8
times as fast with the JIT compiler, 9.2 times with the pooled memory functions
too, and 5.2 times with the code generated ahead of time. Nearly half of the
difference between the JIT compiled code and the generated code is in Intf, Mthd,
and RetObj.3, whose calls the JIT compiler compiles in place. The time the
interpreter takes depends on where its code ends up in the executable, which is
why it was measured with the build of the test without the generated code. In the
build with the generated code the interpreter took 6.5 instead of 5.5 seconds for
all the tests.

The 32bit x86 build gains as much or more, as the interpreter is slower there.
RetObj.1 for example takes 0.704 seconds with the interpreter, and 0.160 seconds
with the JIT compiler and the pooled memory functions. 64bit integer operations
are done by helper functions on 32bit hosts.

The table below shows the same tests measured on an Apple M5 Pro with the arm64
release build from Apple clang 21 on macOS, as the fastest of the ten runs of each
test in six runs of the program. By the geometric mean of the speedups over the
interpreter, the tests are 3.6 times as fast with the JIT compiler without direct
system calls, 4.7 times with the default settings, 5.1 times with the pooled memory
functions too, and 3.9 times with the code generated ahead of time. The JIT
compiler gains less than on x86-64, as the interpreter is faster on this CPU.

<pre>
Test           VM       No direct  JIT      JIT+pool  AOT
Basic          0.150    0.065      0.028    0.033     0.038
Basic2         0.047    0.006      0.006    0.006     0.014
Call           0.164    0.079      0.077    0.077     0.079
Call2          0.208    0.113      0.114    0.113     0.108
Fib            0.277    0.081      0.081    0.082     0.090
Int            0.045    0.020      0.006    0.006     0.006
Intf           0.095    0.009      0.009    0.009     0.024
Mthd           0.091    0.009      0.009    0.009     0.019
String         0.206    0.171      0.091    0.092     0.103
String2        0.113    0.087      0.044    0.044     0.060
StringPooled   0.130    0.115      0.042    0.043     0.047
ThisProp       0.097    0.008      0.008    0.008     0.006
Vector3        0.080    0.064      0.009    0.009     0.008
Assign.1       0.051    0.005      0.005    0.005     0.011
Assign.2       0.086    0.009      0.012    0.009     0.010
Assign.3       0.072    0.012      0.012    0.012     0.009
Assign.4       0.087    0.019      0.020    0.019     0.018
Assign.5       0.087    0.020      0.020    0.020     0.019
Array.1        0.181    0.092      0.073    0.043     0.077
Array.2        0.091    0.014      0.014    0.014     0.011
GlobalVar      0.039    0.033      0.012    0.012     0.018
ClassProp      0.067    0.011      0.011    0.011     0.010
RetObj.1       0.230    0.147      0.148    0.099     0.162
RetObj.2       0.133    0.073      0.073    0.047     0.083
RetObj.3       0.040    0.003      0.004    0.003     0.011
</pre>

\section doc_addon_jit_limits Known limitations

 - Imported functions, and the delegates of interface methods and of registered
   functions, are called through a helper function that uses the call stack of the
   VM, so these calls are not faster than with the interpreter. The delegates of
   the other script methods are called natively by the JIT compiled functions, but
   not by the code generated ahead of time.
 - Unwind information for the generated code is only registered on 64bit Windows,
   and for x86-64 on Linux and AArch64 on Linux and macOS, but not for x86-64 on macOS,
   arm64e, the BSDs, or 64bit ARM Windows. On the other platforms besides 32bit Windows with MSVC, a C++
   exception that passes through the generated code terminates the application,
   which is why direct system calls are opt-in there unless the library is built
   with AS_NO_EXCEPTIONS.
 - The code generated ahead of time compiles no calls in place and borrows no
   references, which makes the calls of short script functions slower than with
   the JIT compiler. It creates the objects of the registered types through a
   helper function, keeps the variables in memory on big endian CPUs, checks
   for suspension and line callbacks at every statement, and on AArch64 calls the
   functions that return a value type in memory through the engine, as the
   generated C++ can't pass the hidden pointer in x8.
 - Direct system calls are only made for functions with primitive, reference,
   and handle parameters, and primitive, reference, handle, and value type return
   values. Everything else, including asCALL_GENERIC, goes through the same code
   as the VM.
 - The \ref doc_addon_jit_tiered "deferred functions" are compiled by the thread
   that executes them, which waits for the compilation, and not in the background.
   The same goes for the functions compiled again with the
   \ref doc_addon_jit_profiles "profiles". The VM doesn't note the classes of the
   objects whose methods the functions call while it executes them, so the first
   code of the deferred functions compiles none of those methods in place.
 - The profiles only lead to the methods of one class being compiled in place at a
   call. The calls that see objects of two or three classes call the methods.
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

The deferred functions of the \ref doc_addon_jit_tiered "tiered compilation" all
share one native function, which the VM calls at the JitEntry instructions whose
argument holds a count besides the index. It counts down, and when the count runs
out it compiles the function and continues in the compiled code at the entry point
with that index. The other JitEntry instructions have the argument 0 until then,
so the VM goes on past them without leaving the interpreter loop.

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
the object is checked for, and so are the virtual methods that all the classes of
the module implementing them inherit, for which the table of virtual functions of
the class of the object is checked for the method. The objects of other classes,
such as those of other modules that derive from shared classes, call the methods
as usual. Calling a method that 3 classes inherit in a loop took 0.007 seconds
instead of 0.026 for 10 million calls when it is compiled in place. The methods that
several classes implement differently are compiled in place for the class that the
\ref doc_addon_jit_profiles "profile" has seen, once the function is compiled again.
Until then each such call compares the class of the object with the one it has
noted, and notes the class if there is none, or that it has seen several otherwise,
and counts down the count that all the calls of the code share. The call compiled in
place for the class compares the class of the object with it, and only the objects
of other classes are noted and count down. The function is compiled again by a call
of the add-on, which the code makes when the count runs out. Recursive
functions and functions with catch blocks are always called. The code
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
