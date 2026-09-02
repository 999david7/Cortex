#include "lang.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace lang {
namespace {

// --------------------------------------------------------------- keywords

const char* kEmpty[] = {nullptr};

const char* kCKeywords[] = {
    "auto","break","case","const","continue","default","do","else","enum","extern","for","goto",
    "if","inline","register","restrict","return","sizeof","static","struct","switch","typedef",
    "union","volatile","while","_Atomic","_Bool","_Generic","_Noreturn","_Static_assert", nullptr};

const char* kCTypes[] = {
    "char","double","float","int","long","short","signed","unsigned","void","size_t","ssize_t",
    "ptrdiff_t","int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t",
    "bool","FILE","NULL","true","false", nullptr};

const char* kCppKeywords[] = {
    "alignas","alignof","and","asm","auto","bool","break","case","catch","class","concept",
    "const","consteval","constexpr","constinit","const_cast","continue","co_await","co_return",
    "co_yield","decltype","default","delete","do","dynamic_cast","else","enum","explicit",
    "export","extern","false","final","for","friend","goto","if","inline","mutable","namespace",
    "new","noexcept","not","nullptr","operator","or","override","private","protected","public",
    "register","reinterpret_cast","requires","return","sizeof","static","static_assert",
    "static_cast","struct","switch","template","this","thread_local","throw","true","try",
    "typedef","typeid","typename","union","using","virtual","volatile","while","xor", nullptr};

const char* kCppTypes[] = {
    "char","char8_t","char16_t","char32_t","double","float","int","long","short","signed",
    "unsigned","void","wchar_t","size_t","ssize_t","ptrdiff_t","int8_t","int16_t","int32_t",
    "int64_t","uint8_t","uint16_t","uint32_t","uint64_t","string","vector","map","set",
    "unordered_map","unordered_set","shared_ptr","unique_ptr","weak_ptr","function","pair",
    "array","optional","variant","atomic","thread","mutex","std","string_view","span", nullptr};

const char* kObjCKeywords[] = {
    "@interface","@implementation","@protocol","@end","@property","@synthesize","@dynamic",
    "@selector","@class","@public","@private","@protected","@package","@try","@catch","@finally",
    "@throw","@autoreleasepool","@synchronized","self","super","nil","Nil","YES","NO","id",
    "break","case","const","continue","default","do","else","enum","extern","for","goto","if",
    "inline","return","sizeof","static","struct","switch","typedef","union","void","while",
    "instancetype","nonatomic","atomic","strong","weak","copy","assign","retain","readonly",
    nullptr};

const char* kCSharpKeywords[] = {
    "abstract","as","async","await","base","break","case","catch","checked","class","const",
    "continue","default","delegate","do","else","enum","event","explicit","extern","false",
    "finally","fixed","for","foreach","get","goto","if","implicit","in","init","interface",
    "internal","is","lock","namespace","new","null","operator","out","override","params",
    "private","protected","public","readonly","record","ref","return","sealed","set","sizeof",
    "stackalloc","static","struct","switch","this","throw","true","try","typeof","unchecked",
    "unsafe","using","var","virtual","void","volatile","when","where","while","yield","nameof",
    "with","required","scoped","file", nullptr};

const char* kCSharpTypes[] = {
    "bool","byte","char","decimal","double","dynamic","float","int","long","nint","nuint",
    "object","sbyte","short","string","uint","ulong","ushort","List","Dictionary","Task","Span",
    "IEnumerable","IEnumerator","IList","IDictionary","Console","Exception","Action","Func",
    nullptr};

const char* kJavaKeywords[] = {
    "abstract","assert","break","case","catch","class","const","continue","default","do","else",
    "enum","extends","final","finally","for","goto","if","implements","import","instanceof",
    "interface","native","new","non-sealed","package","permits","private","protected","public",
    "record","return","sealed","static","strictfp","super","switch","synchronized","this",
    "throw","throws","transient","try","var","volatile","while","yield","true","false","null",
    nullptr};

const char* kJavaTypes[] = {
    "boolean","byte","char","double","float","int","long","short","void","String","Integer",
    "Double","Boolean","Character","Long","Float","Short","Byte","Object","List","Map","Set",
    "ArrayList","HashMap","HashSet","Optional","Stream","System","Exception", nullptr};

const char* kKotlinKeywords[] = {
    "abstract","actual","annotation","as","break","by","catch","class","companion","const",
    "constructor","continue","crossinline","data","delegate","do","dynamic","else","enum",
    "expect","external","false","final","finally","for","fun","get","if","import","in","infix",
    "init","inline","inner","interface","internal","is","lateinit","noinline","null","object",
    "open","operator","out","override","package","private","protected","public","reified",
    "return","sealed","set","super","suspend","tailrec","this","throw","true","try","typealias",
    "val","var","vararg","when","where","while", nullptr};

const char* kKotlinTypes[] = {
    "Any","Boolean","Byte","Char","Double","Float","Int","Long","Nothing","Short","String","Unit",
    "Array","List","Map","Set","MutableList","MutableMap","MutableSet","Pair","Triple","Sequence",
    nullptr};

const char* kScalaKeywords[] = {
    "abstract","case","catch","class","def","do","else","enum","export","extends","extension",
    "false","final","finally","for","given","if","implicit","import","lazy","match","new","null",
    "object","override","package","private","protected","return","sealed","super","then","this",
    "throw","trait","true","try","type","using","val","var","while","with","yield", nullptr};

const char* kScalaTypes[] = {
    "Any","AnyRef","AnyVal","Boolean","Byte","Char","Double","Float","Int","Long","Nothing",
    "Short","String","Unit","List","Map","Set","Option","Some","None","Either","Future","Seq",
    nullptr};

const char* kGroovyKeywords[] = {
    "abstract","as","assert","break","case","catch","class","const","continue","def","default",
    "do","else","enum","extends","final","finally","for","goto","if","implements","import","in",
    "instanceof","interface","new","package","private","protected","public","return","static",
    "super","switch","synchronized","this","throw","throws","trait","try","while","true","false",
    "null", nullptr};

const char* kDartKeywords[] = {
    "abstract","as","assert","async","await","break","case","catch","class","const","continue",
    "covariant","default","deferred","do","dynamic","else","enum","export","extends","extension",
    "external","factory","false","final","finally","for","get","hide","if","implements","import",
    "in","interface","is","late","library","mixin","new","null","on","operator","part","required",
    "rethrow","return","sealed","set","show","static","super","switch","sync","this","throw",
    "true","try","typedef","var","void","while","with","yield", nullptr};

const char* kDartTypes[] = {
    "bool","double","int","num","String","List","Map","Set","Object","Future","Stream","Iterable",
    "Widget","BuildContext","Never","Function", nullptr};

const char* kSwiftKeywords[] = {
    "actor","any","as","associatedtype","async","await","borrowing","break","case","catch",
    "class","consuming","continue","convenience","default","defer","deinit","distributed","do",
    "dynamic","else","enum","extension","fallthrough","false","fileprivate","final","for","func",
    "get","guard","if","import","in","indirect","infix","init","inout","internal","is","lazy",
    "let","macro","mutating","nil","nonisolated","nonmutating","open","operator","optional",
    "override","package","postfix","precedencegroup","prefix","private","protocol","public",
    "repeat","required","rethrows","return","self","Self","set","some","static","struct",
    "subscript","super","switch","throw","throws","true","try","typealias","var","weak","where",
    "while","willSet","didSet","unowned", nullptr};

const char* kSwiftTypes[] = {
    "Any","AnyObject","Array","Bool","Character","Dictionary","Double","Error","Float","Int",
    "Int8","Int16","Int32","Int64","Never","Optional","Result","Set","String","Substring","UInt",
    "UInt8","UInt16","UInt32","UInt64","Void","Task","Data","URL","Date", nullptr};

const char* kGoKeywords[] = {
    "break","case","chan","const","continue","default","defer","else","fallthrough","for","func",
    "go","goto","if","import","interface","map","package","range","return","select","struct",
    "switch","type","var","nil","true","false","iota","any", nullptr};

const char* kGoTypes[] = {
    "bool","byte","complex64","complex128","error","float32","float64","int","int8","int16",
    "int32","int64","rune","string","uint","uint8","uint16","uint32","uint64","uintptr","make",
    "new","len","cap","append","copy","delete","panic","recover", nullptr};

const char* kRustKeywords[] = {
    "as","async","await","break","const","continue","crate","dyn","else","enum","extern","false",
    "fn","for","if","impl","in","let","loop","match","mod","move","mut","pub","ref","return",
    "self","Self","static","struct","super","trait","true","type","union","unsafe","use","where",
    "while","yield", nullptr};

const char* kRustTypes[] = {
    "bool","char","f32","f64","i8","i16","i32","i64","i128","isize","str","u8","u16","u32","u64",
    "u128","usize","String","Vec","Option","Some","None","Result","Ok","Err","Box","Rc","Arc",
    "HashMap","HashSet","BTreeMap","RefCell","Cell","Mutex","RwLock","Cow", nullptr};

const char* kZigKeywords[] = {
    "align","allowzero","and","anyframe","anytype","asm","async","await","break","callconv",
    "catch","comptime","const","continue","defer","else","enum","errdefer","error","export",
    "extern","fn","for","if","inline","noalias","nosuspend","opaque","or","orelse","packed","pub",
    "resume","return","struct","suspend","switch","test","threadlocal","try","union","unreachable",
    "usingnamespace","var","volatile","while","true","false","null","undefined", nullptr};

const char* kZigTypes[] = {
    "bool","c_int","c_long","c_short","comptime_float","comptime_int","f16","f32","f64","f128",
    "i8","i16","i32","i64","i128","isize","noreturn","type","u8","u16","u32","u64","u128","usize",
    "void","anyerror", nullptr};

const char* kDKeywords[] = {
    "abstract","alias","align","asm","assert","auto","body","break","case","cast","catch","class",
    "const","continue","debug","default","delegate","delete","deprecated","do","else","enum",
    "export","extern","false","final","finally","for","foreach","function","goto","if","immutable",
    "import","in","inout","interface","invariant","is","lazy","macro","mixin","module","new",
    "nothrow","null","out","override","package","pragma","private","protected","public","pure",
    "ref","return","scope","shared","static","struct","super","switch","synchronized","template",
    "this","throw","true","try","typeid","typeof","union","unittest","version","void","while",
    "with", nullptr};

const char* kSolidityKeywords[] = {
    "abstract","after","anonymous","as","assembly","break","calldata","catch","constant",
    "constructor","continue","contract","delete","do","else","emit","enum","event","external",
    "fallback","false","for","function","if","immutable","import","indexed","interface","internal",
    "is","library","mapping","memory","modifier","new","override","payable","pragma","private",
    "public","pure","receive","return","returns","revert","storage","struct","true","try","type",
    "unchecked","using","view","virtual","while", nullptr};

const char* kSolidityTypes[] = {
    "address","bool","byte","bytes","bytes32","int","int256","string","uint","uint8","uint256",
    "msg","block","tx","this","super","wei","gwei","ether","seconds","minutes","hours","days",
    nullptr};

const char* kVerilogKeywords[] = {
    "always","assign","begin","case","casex","casez","default","defparam","else","end","endcase",
    "endfunction","endmodule","endtask","for","force","forever","fork","function","generate",
    "genvar","if","initial","inout","input","join","localparam","module","negedge","output",
    "parameter","posedge","reg","repeat","task","wait","while","wire","always_ff","always_comb",
    "logic","typedef","enum","struct","package","import","interface","endinterface", nullptr};

const char* kVhdlKeywords[] = {
    "architecture","array","begin","case","component","configuration","constant","downto","else",
    "elsif","end","entity","exit","for","function","generate","generic","if","in","inout","is",
    "library","loop","map","of","others","out","package","port","process","range","record",
    "return","signal","then","to","type","use","variable","wait","when","while","with", nullptr};

const char* kGlslKeywords[] = {
    "attribute","break","buffer","centroid","coherent","const","continue","discard","do","else",
    "flat","for","highp","if","in","inout","layout","lowp","mediump","noperspective","out",
    "patch","precision","readonly","restrict","return","sample","shared","smooth","struct",
    "subroutine","switch","uniform","varying","volatile","while","writeonly","true","false",
    nullptr};

const char* kGlslTypes[] = {
    "bool","bvec2","bvec3","bvec4","double","dvec2","dvec3","dvec4","float","int","ivec2","ivec3",
    "ivec4","mat2","mat3","mat4","sampler1D","sampler2D","sampler3D","samplerCube","uint","uvec2",
    "uvec3","uvec4","vec2","vec3","vec4","void","image2D","gl_Position","gl_FragColor", nullptr};

// ---- scripting -----------------------------------------------------------

const char* kPythonKeywords[] = {
    "and","as","assert","async","await","break","class","continue","def","del","elif","else",
    "except","False","finally","for","from","global","if","import","in","is","lambda","match",
    "None","nonlocal","not","or","pass","raise","return","True","try","while","with","yield",
    "case", nullptr};

const char* kPythonTypes[] = {
    "bool","bytes","bytearray","complex","dict","float","frozenset","int","list","object","set",
    "str","tuple","type","self","cls","super","print","len","range","enumerate","zip","open",
    "Exception","ValueError","TypeError","KeyError","IndexError","map","filter","sorted","sum",
    "min","max","abs","any","all","isinstance","hasattr","getattr","setattr","staticmethod",
    "classmethod","property", nullptr};

const char* kRubyKeywords[] = {
    "alias","and","begin","break","case","class","def","defined?","do","else","elsif","end",
    "ensure","false","for","if","in","module","next","nil","not","or","redo","rescue","retry",
    "return","self","super","then","true","undef","unless","until","when","while","yield",
    "attr_accessor","attr_reader","attr_writer","require","require_relative","include","extend",
    "lambda","proc","puts","raise", nullptr};

const char* kRubyTypes[] = {
    "Array","Class","Comparable","Enumerable","Exception","Float","Hash","Integer","Module",
    "Numeric","Object","Proc","Range","Regexp","String","Struct","Symbol","Time", nullptr};

const char* kPerlKeywords[] = {
    "and","bless","break","continue","cmp","do","else","elsif","eq","eval","exit","for","foreach",
    "given","goto","if","last","local","my","ne","next","no","not","or","our","package","redo",
    "ref","require","return","say","sub","undef","unless","until","use","wantarray","when",
    "while","xor","print","printf","push","pop","shift","unshift","split","join","keys","values",
    nullptr};

const char* kPhpKeywords[] = {
    "abstract","and","array","as","break","callable","case","catch","class","clone","const",
    "continue","declare","default","do","echo","else","elseif","empty","enddeclare","endfor",
    "endforeach","endif","endswitch","endwhile","enum","extends","final","finally","fn","for",
    "foreach","function","global","goto","if","implements","include","include_once","instanceof",
    "insteadof","interface","isset","list","match","namespace","new","or","print","private",
    "protected","public","readonly","require","require_once","return","static","switch","throw",
    "trait","try","unset","use","var","while","xor","yield","true","false","null", nullptr};

const char* kPhpTypes[] = {
    "bool","float","int","iterable","mixed","never","object","string","void","self","parent",
    "Closure","Generator","Exception","ArrayObject","stdClass", nullptr};

const char* kLuaKeywords[] = {
    "and","break","do","else","elseif","end","false","for","function","goto","if","in","local",
    "nil","not","or","repeat","return","then","true","until","while", nullptr};

const char* kLuaTypes[] = {
    "assert","collectgarbage","coroutine","debug","dofile","error","getmetatable","io","ipairs",
    "load","math","next","os","pairs","pcall","print","rawget","rawset","require","select",
    "setmetatable","string","table","tonumber","tostring","type","unpack","xpcall","self",
    nullptr};

const char* kJsKeywords[] = {
    "as","async","await","break","case","catch","class","const","continue","debugger","default",
    "delete","do","else","export","extends","false","finally","for","from","function","get","if",
    "import","in","instanceof","let","new","null","of","return","set","static","super","switch",
    "this","throw","true","try","typeof","undefined","var","void","while","with","yield",
    "interface","type","enum","implements","private","protected","public","readonly","declare",
    "namespace","abstract","satisfies","override","accessor","using", nullptr};

const char* kJsTypes[] = {
    "Array","BigInt","Boolean","Date","Error","Function","JSON","Map","Math","Number","Object",
    "Promise","Proxy","Reflect","RegExp","Set","String","Symbol","WeakMap","WeakSet","console",
    "document","window","globalThis","any","boolean","never","number","string","unknown","bigint",
    "object","void","Record","Partial","Readonly","Pick","Omit", nullptr};

const char* kShellKeywords[] = {
    "if","then","else","elif","fi","for","while","until","do","done","case","esac","function",
    "in","select","return","break","continue","local","export","readonly","declare","source",
    "alias","set","unset","shift","exit","trap","eval","exec","true","false","echo","cd","test",
    nullptr};

const char* kShellTypes[] = {
    "awk","cat","chmod","chown","cp","curl","cut","date","df","diff","du","find","grep","head",
    "kill","ln","ls","mkdir","mv","ps","pwd","rm","rmdir","sed","sleep","sort","tail","tar","tee",
    "touch","tr","uniq","wc","wget","xargs","git","make","sudo","apt","npm","python","docker",
    nullptr};

const char* kPowerShellKeywords[] = {
    "begin","break","catch","class","continue","data","define","do","dynamicparam","else",
    "elseif","end","enum","exit","filter","finally","for","foreach","from","function","hidden",
    "if","in","inlinescript","param","process","return","static","switch","throw","trap","try",
    "until","using","var","while","workflow","true","false","null", nullptr};

const char* kPowerShellTypes[] = {
    "Get-ChildItem","Get-Content","Set-Content","Write-Host","Write-Output","Where-Object",
    "ForEach-Object","Select-Object","New-Item","Remove-Item","Test-Path","Join-Path","Copy-Item",
    "Move-Item","Invoke-Expression","Invoke-WebRequest","Start-Process","Get-Command", nullptr};

const char* kBatchKeywords[] = {
    "call","cd","cls","copy","del","dir","do","echo","else","endlocal","exit","for","goto","if",
    "in","md","move","not","pause","popd","pushd","rd","rem","ren","set","setlocal","shift",
    "start","type","errorlevel","exist","defined", nullptr};

const char* kRKeywords[] = {
    "break","else","for","function","if","in","next","repeat","return","while","TRUE","FALSE",
    "NULL","NA","Inf","NaN","library","require","source", nullptr};

const char* kJuliaKeywords[] = {
    "abstract","baremodule","begin","break","catch","const","continue","do","else","elseif","end",
    "export","false","finally","for","function","global","if","import","let","local","macro",
    "module","mutable","primitive","quote","return","struct","true","try","using","where","while",
    nullptr};

const char* kNimKeywords[] = {
    "addr","and","as","asm","bind","block","break","case","cast","concept","const","continue",
    "converter","defer","discard","distinct","div","do","elif","else","end","enum","except",
    "export","finally","for","from","func","if","import","in","include","interface","is","isnot",
    "iterator","let","macro","method","mixin","mod","nil","not","notin","object","of","or","out",
    "proc","ptr","raise","ref","return","shl","shr","static","template","try","tuple","type",
    "using","var","when","while","xor","yield","true","false", nullptr};

const char* kCrystalKeywords[] = {
    "abstract","alias","annotation","as","asm","begin","break","case","class","def","do","else",
    "elsif","end","ensure","enum","extend","false","for","fun","if","in","include","instance_sizeof",
    "is_a?","lib","macro","module","next","nil","of","out","pointerof","private","protected",
    "require","rescue","return","select","self","sizeof","struct","super","then","true","type",
    "typeof","union","uninitialized","unless","until","verbatim","when","while","with","yield",
    nullptr};

const char* kTclKeywords[] = {
    "after","append","array","break","catch","cd","concat","continue","else","elseif","error",
    "eval","exec","exit","expr","file","for","foreach","format","global","if","incr","info","join",
    "lappend","lindex","list","llength","lrange","lreplace","lsearch","lsort","namespace","open",
    "proc","puts","regexp","regsub","return","set","source","split","string","switch","unset",
    "uplevel","upvar","variable","while", nullptr};

const char* kVimKeywords[] = {
    "if","elseif","else","endif","while","endwhile","for","endfor","function","endfunction",
    "return","let","unlet","call","execute","autocmd","augroup","set","setlocal","map","nmap",
    "imap","vmap","noremap","nnoremap","inoremap","vnoremap","command","source","try","catch",
    "finally","endtry","echo","echom","normal", nullptr};

const char* kAppleScriptKeywords[] = {
    "activate","and","application","considering","copy","display","do","else","end","error",
    "every","exit","false","first","get","global","if","ignoring","in","is","it","local","me",
    "not","of","on","or","property","repeat","return","script","set","tell","then","to","true",
    "try","until","where","while","with","without", nullptr};

// ---- functional ----------------------------------------------------------

const char* kHaskellKeywords[] = {
    "case","class","data","default","deriving","do","else","forall","foreign","if","import","in",
    "infix","infixl","infixr","instance","let","mdo","module","newtype","of","proc","qualified",
    "rec","then","type","where", nullptr};

const char* kHaskellTypes[] = {
    "Bool","Char","Double","Either","Float","Functor","Int","Integer","IO","Maybe","Monad",
    "Nothing","Just","Ord","Ordering","Rational","Show","String","Word","Applicative","Left",
    "Right","True","False", nullptr};

const char* kOCamlKeywords[] = {
    "and","as","assert","asr","begin","class","constraint","do","done","downto","else","end",
    "exception","external","false","for","fun","function","functor","if","in","include","inherit",
    "initializer","land","lazy","let","lor","lsl","lsr","lxor","match","method","mod","module",
    "mutable","new","nonrec","object","of","open","or","private","rec","sig","struct","then","to",
    "true","try","type","val","virtual","when","while","with", nullptr};

const char* kFSharpKeywords[] = {
    "abstract","and","as","assert","base","begin","class","default","delegate","do","done",
    "downcast","downto","elif","else","end","exception","extern","false","finally","for","fun",
    "function","global","if","in","inherit","inline","interface","internal","lazy","let","match",
    "member","module","mutable","namespace","new","not","null","of","open","or","override",
    "private","public","rec","return","select","static","struct","then","to","true","try","type",
    "upcast","use","val","void","when","while","with","yield", nullptr};

const char* kElixirKeywords[] = {
    "after","and","case","catch","cond","def","defexception","defguard","defimpl","defmacro",
    "defmodule","defp","defprotocol","defstruct","do","else","end","false","fn","for","if","import",
    "in","nil","not","or","quote","raise","receive","require","rescue","true","try","unless",
    "unquote","use","when","with","alias","__MODULE__", nullptr};

const char* kErlangKeywords[] = {
    "after","and","andalso","band","begin","bnot","bor","bsl","bsr","bxor","case","catch","cond",
    "div","end","fun","if","let","not","of","or","orelse","receive","rem","try","when","xor",
    "module","export","import","record","behaviour","spec","type", nullptr};

const char* kClojureKeywords[] = {
    "def","defn","defn-","defmacro","defmulti","defmethod","defprotocol","defrecord","deftype",
    "defstruct","do","doseq","dotimes","fn","for","if","if-let","if-not","let","letfn","loop",
    "ns","recur","require","try","catch","finally","throw","when","when-let","when-not","while",
    "cond","condp","case","quote","var","set!","new","true","false","nil", nullptr};

const char* kLispKeywords[] = {
    "defun","defmacro","defvar","defparameter","defconstant","defstruct","defclass","defmethod",
    "let","let*","lambda","if","cond","case","when","unless","progn","prog1","loop","do","dolist",
    "dotimes","setq","setf","quote","function","and","or","not","nil","t","return","return-from",
    nullptr};

const char* kSchemeKeywords[] = {
    "define","define-syntax","lambda","let","let*","letrec","if","cond","case","when","unless",
    "begin","do","and","or","not","set!","quote","quasiquote","unquote","else","delay","force",
    "call/cc","values","apply","map","for-each","display","newline", nullptr};

const char* kElmKeywords[] = {
    "alias","as","case","effect","else","exposing","if","import","in","infix","let","module","of",
    "port","then","type","where", nullptr};

const char* kPrologKeywords[] = {
    "is","not","module","use_module","dynamic","discontiguous","initialization","assert",
    "asserta","assertz","retract","findall","bagof","setof","forall","between","member","append",
    "length","nb_getval","nb_setval","fail","true","halt", nullptr};

// ---- markup / data -------------------------------------------------------

const char* kCssKeywords[] = {
    "important","media","import","charset","namespace","supports","keyframes","font-face","page",
    "layer","container","property","scope","starting-style", nullptr};

const char* kScssKeywords[] = {
    "mixin","include","extend","function","return","if","else","each","for","while","use",
    "forward","at-root","content","debug","warn","error","important","media","supports",
    "keyframes","font-face", nullptr};

const char* kSqlKeywords[] = {
    "select","from","where","insert","into","values","update","set","delete","create","table",
    "alter","drop","index","view","join","left","right","inner","outer","full","cross","on",
    "group","by","order","having","limit","offset","union","all","distinct","as","and","or","not",
    "null","is","in","between","like","exists","case","when","then","else","end","primary","key",
    "foreign","references","default","cascade","constraint","unique","check","with","recursive",
    "begin","commit","rollback","transaction","grant","revoke","truncate","asc","desc","using",
    nullptr};

const char* kSqlTypes[] = {
    "int","integer","bigint","smallint","tinyint","varchar","nvarchar","char","nchar","text",
    "boolean","bool","date","datetime","timestamp","time","decimal","numeric","real","float",
    "double","blob","json","jsonb","uuid","serial","bigserial","bytea","array","interval",
    "count","sum","avg","min","max","coalesce","cast","now", nullptr};

const char* kGraphQlKeywords[] = {
    "query","mutation","subscription","fragment","on","type","input","interface","union","enum",
    "scalar","schema","directive","extend","implements","repeatable","true","false","null",
    nullptr};

const char* kGraphQlTypes[] = {"Int","Float","String","Boolean","ID", nullptr};

const char* kProtoKeywords[] = {
    "syntax","package","import","option","message","enum","service","rpc","returns","repeated",
    "optional","required","oneof","map","reserved","extend","extensions","public","weak","stream",
    nullptr};

const char* kProtoTypes[] = {
    "double","float","int32","int64","uint32","uint64","sint32","sint64","fixed32","fixed64",
    "sfixed32","sfixed64","bool","string","bytes", nullptr};

const char* kYamlKeywords[] = {
    "true","false","null","yes","no","on","off","~", nullptr};

const char* kTomlKeywords[] = {"true","false", nullptr};

const char* kJsonKeywords[] = {"true","false","null", nullptr};

// ---- build / infra -------------------------------------------------------

const char* kDockerKeywords[] = {
    "FROM","RUN","CMD","LABEL","MAINTAINER","EXPOSE","ENV","ADD","COPY","ENTRYPOINT","VOLUME",
    "USER","WORKDIR","ARG","ONBUILD","STOPSIGNAL","HEALTHCHECK","SHELL","AS", nullptr};

const char* kMakeKeywords[] = {
    "ifeq","ifneq","ifdef","ifndef","else","endif","include","-include","define","endef","export",
    "unexport","override","vpath",".PHONY",".SUFFIXES",".DEFAULT","wildcard","patsubst","shell",
    "foreach","subst","filter","sort","dir","notdir", nullptr};

const char* kCmakeKeywords[] = {
    "if","elseif","else","endif","foreach","endforeach","while","endwhile","function",
    "endfunction","macro","endmacro","return","break","continue","set","unset","list","string",
    "file","find_package","find_library","include","include_directories","option","message",
    "install","add_subdirectory","add_executable","add_library","target_link_libraries",
    "target_include_directories","target_compile_options","project","cmake_minimum_required",
    nullptr};

const char* kMesonKeywords[] = {
    "if","elif","else","endif","foreach","endforeach","break","continue","and","or","not","in",
    "true","false","project","executable","library","shared_library","static_library",
    "dependency","subdir","install_headers","test","files","include_directories", nullptr};

const char* kNixKeywords[] = {
    "assert","else","if","in","inherit","let","or","rec","then","with","import","builtins",
    "derivation","true","false","null", nullptr};

const char* kTerraformKeywords[] = {
    "resource","provider","variable","output","module","data","locals","terraform","backend",
    "provisioner","connection","lifecycle","depends_on","count","for_each","dynamic","true",
    "false","null","var","local","each","if","for","in", nullptr};

const char* kBazelKeywords[] = {
    "load","package","cc_library","cc_binary","cc_test","java_library","java_binary","py_library",
    "py_binary","filegroup","genrule","glob","select","exports_files","def","if","else","for",
    "in","return","True","False","None", nullptr};

// ---- docs & misc ---------------------------------------------------------

const char* kAsmKeywords[] = {
    "mov","movq","movl","movb","movw","push","pop","lea","add","sub","mul","imul","div","idiv",
    "inc","dec","and","or","xor","not","neg","shl","shr","sar","cmp","test","jmp","je","jne","jz",
    "jnz","jg","jge","jl","jle","ja","jb","call","ret","leave","enter","nop","int","syscall",
    "section","global","extern","db","dw","dd","dq","equ","times","align","proc","endp", nullptr};

const char* kAsmTypes[] = {
    "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","r8","r9","r10","r11","r12","r13","r14","r15",
    "eax","ebx","ecx","edx","esi","edi","ebp","esp","ax","bx","cx","dx","al","bl","cl","dl",
    "byte","word","dword","qword","ptr", nullptr};

const char* kFortranKeywords[] = {
    "allocate","allocatable","call","case","character","common","contains","continue","cycle",
    "deallocate","dimension","do","else","elseif","end","enddo","endif","function","goto","if",
    "implicit","integer","intent","interface","logical","module","none","only","parameter",
    "pointer","print","private","program","public","read","real","recursive","return","select",
    "stop","subroutine","then","type","use","where","while","write", nullptr};

const char* kMatlabKeywords[] = {
    "break","case","catch","classdef","continue","else","elseif","end","for","function","global",
    "if","otherwise","parfor","persistent","return","spmd","switch","try","while","true","false",
    nullptr};

const char* kPascalKeywords[] = {
    "and","array","begin","case","const","div","do","downto","else","end","file","for","function",
    "goto","if","implementation","in","interface","label","mod","nil","not","of","or","packed",
    "procedure","program","record","repeat","set","then","to","type","unit","until","uses","var",
    "while","with","class","constructor","destructor","private","protected","public","published",
    "property","try","except","finally","raise","inherited", nullptr};

const char* kAdaKeywords[] = {
    "abort","abs","abstract","accept","access","aliased","all","and","array","at","begin","body",
    "case","constant","declare","delay","delta","digits","do","else","elsif","end","entry",
    "exception","exit","for","function","generic","goto","if","in","interface","is","limited",
    "loop","mod","new","not","null","of","or","others","out","overriding","package","pragma",
    "private","procedure","protected","raise","range","record","rem","renames","requeue","return",
    "reverse","select","separate","some","subtype","synchronized","tagged","task","terminate",
    "then","type","until","use","when","while","with","xor", nullptr};

const char* kCobolKeywords[] = {
    "ACCEPT","ADD","CALL","CANCEL","CLOSE","COMPUTE","CONTINUE","DELETE","DISPLAY","DIVIDE",
    "DIVISION","ELSE","END","EVALUATE","EXIT","GO","GOBACK","IDENTIFICATION","IF","INITIALIZE",
    "INSPECT","MOVE","MULTIPLY","OPEN","PERFORM","PROCEDURE","PROGRAM-ID","READ","RETURN",
    "REWRITE","SEARCH","SECTION","SET","SORT","START","STOP","STRING","SUBTRACT","UNSTRING",
    "WORKING-STORAGE","WRITE","DATA","ENVIRONMENT","CONFIGURATION","FILE","PIC","PICTURE","VALUE",
    nullptr};

// -------------------------------------------------------------- the table

// Colors are the community/brand color for each language, used only for the
// small file badge — the rest of the UI stays monochrome.
#define C32(r, g, b) IM_COL32(0x##r, 0x##g, 0x##b, 0xFF)

const Info kInfo[] = {
    {Id::PlainText,  "Plain Text",   "text",        "TXT",  C32(8A,8A,8E)},
    // C family
    {Id::C,          "C",            "c",           "C",    C32(5B,8D,C9)},
    {Id::Cpp,        "C++",          "cpp",         "C++",  C32(65,9A,D2)},
    {Id::ObjC,       "Objective-C",  "objectivec",  "OBJC", C32(43,8E,FF)},
    {Id::CSharp,     "C#",           "csharp",      "C#",   C32(9B,4F,96)},
    {Id::Java,       "Java",         "java",        "JAVA", C32(E7,6F,00)},
    {Id::Kotlin,     "Kotlin",       "kotlin",      "KT",   C32(A9,7B,FF)},
    {Id::Scala,      "Scala",        "scala",       "SC",   C32(DC,32,2D)},
    {Id::Groovy,     "Groovy",       "groovy",      "GVY",  C32(4A,90,D9)},
    {Id::Dart,       "Dart",         "dart",        "DART", C32(00,B4,AB)},
    {Id::Swift,      "Swift",        "swift",       "SWFT", C32(F0,51,38)},
    {Id::Go,         "Go",           "go",          "GO",   C32(00,AD,D8)},
    {Id::Rust,       "Rust",         "rust",        "RS",   C32(D3,84,3D)},
    {Id::Zig,        "Zig",          "zig",         "ZIG",  C32(F7,A4,1D)},
    {Id::D,          "D",            "d",           "D",    C32(B0,30,31)},
    {Id::Vala,       "Vala",         "vala",        "VALA", C32(7D,54,A5)},
    {Id::Haxe,       "Haxe",         "haxe",        "HX",   C32(EA,80,20)},
    {Id::Solidity,   "Solidity",     "solidity",    "SOL",  C32(62,79,88)},
    {Id::Verilog,    "Verilog",      "verilog",     "V",    C32(1B,8A,4C)},
    {Id::Vhdl,       "VHDL",         "vhdl",        "VHDL", C32(54,7A,A8)},
    {Id::Glsl,       "GLSL",         "glsl",        "GLSL", C32(5A,86,C4)},
    {Id::Cuda,       "CUDA",         "cuda",        "CU",   C32(76,B9,00)},
    // scripting
    {Id::Python,     "Python",       "python",      "PY",   C32(4B,8B,BE)},
    {Id::Ruby,       "Ruby",         "ruby",        "RB",   C32(CC,34,2D)},
    {Id::Perl,       "Perl",         "perl",        "PL",   C32(39,45,7E)},
    {Id::Php,        "PHP",          "php",         "PHP",  C32(77,7B,B3)},
    {Id::Lua,        "Lua",          "lua",         "LUA",  C32(00,00,80)},
    {Id::JavaScript, "JavaScript",   "javascript",  "JS",   C32(F0,DB,4F)},
    {Id::TypeScript, "TypeScript",   "typescript",  "TS",   C32(31,78,C6)},
    {Id::Shell,      "Shell",        "bash",        "SH",   C32(89,E0,51)},
    {Id::PowerShell, "PowerShell",   "powershell",  "PS1",  C32(53,91,FE)},
    {Id::Batch,      "Batch",        "batch",       "BAT",  C32(9E,9E,9E)},
    {Id::R,          "R",            "r",           "R",    C32(27,6D,C2)},
    {Id::Julia,      "Julia",        "julia",       "JL",   C32(95,58,B2)},
    {Id::Tcl,        "Tcl",          "tcl",         "TCL",  C32(C1,9B,5E)},
    {Id::Nim,        "Nim",          "nim",         "NIM",  C32(FF,E9,53)},
    {Id::Crystal,    "Crystal",      "crystal",     "CR",   C32(BB,BB,BB)},
    {Id::Vim,        "Vim script",   "vim",         "VIM",  C32(01,9B,33)},
    {Id::AppleScript,"AppleScript",  "applescript", "APPL", C32(8A,8A,8E)},
    // functional
    {Id::Haskell,    "Haskell",      "haskell",     "HS",   C32(5E,50,86)},
    {Id::OCaml,      "OCaml",        "ocaml",       "ML",   C32(EC,67,13)},
    {Id::FSharp,     "F#",           "fsharp",      "F#",   C32(37,8B,BA)},
    {Id::Elixir,     "Elixir",       "elixir",      "EX",   C32(6E,4A,7E)},
    {Id::Erlang,     "Erlang",       "erlang",      "ERL",  C32(A9,00,33)},
    {Id::Clojure,    "Clojure",      "clojure",     "CLJ",  C32(5A,8B,3C)},
    {Id::Lisp,       "Lisp",         "lisp",        "LISP", C32(3F,B0,68)},
    {Id::Scheme,     "Scheme",       "scheme",      "SCM",  C32(9F,1D,20)},
    {Id::Elm,        "Elm",          "elm",         "ELM",  C32(60,B5,CC)},
    {Id::Prolog,     "Prolog",       "prolog",      "PRO",  C32(74,28,3C)},
    // web / markup
    {Id::Html,       "HTML",         "html",        "HTML", C32(E3,4C,26)},
    {Id::Xml,        "XML",          "xml",         "XML",  C32(F1,66,29)},
    {Id::Css,        "CSS",          "css",         "CSS",  C32(26,4D,E4)},
    {Id::Scss,       "SCSS",         "scss",        "SCSS", C32(CD,66,99)},
    {Id::Less,       "Less",         "less",        "LESS", C32(1D,36,5D)},
    {Id::Svelte,     "Svelte",       "svelte",      "SVLT", C32(FF,3E,00)},
    {Id::Vue,        "Vue",          "vue",         "VUE",  C32(41,B8,83)},
    {Id::Astro,      "Astro",        "astro",       "ASTR", C32(FF,5D,01)},
    {Id::Jsx,        "JSX",          "jsx",         "JSX",  C32(61,DA,FB)},
    {Id::Tsx,        "TSX",          "tsx",         "TSX",  C32(31,78,C6)},
    {Id::Handlebars, "Handlebars",   "handlebars",  "HBS",  C32(F0,77,2B)},
    // data / config
    {Id::Json,       "JSON",         "json",        "JSON", C32(CB,CB,41)},
    {Id::Yaml,       "YAML",         "yaml",        "YML",  C32(CB,17,1E)},
    {Id::Toml,       "TOML",         "toml",        "TOML", C32(9C,42,21)},
    {Id::Ini,        "INI",          "ini",         "INI",  C32(8A,8A,8E)},
    {Id::Csv,        "CSV",          "csv",         "CSV",  C32(21,7A,46)},
    {Id::Sql,        "SQL",          "sql",         "SQL",  C32(E3,8C,00)},
    {Id::GraphQl,    "GraphQL",      "graphql",     "GQL",  C32(E1,00,98)},
    {Id::Proto,      "Protobuf",     "protobuf",    "PROT", C32(4A,90,D9)},
    {Id::Properties, "Properties",   "properties",  "PROP", C32(8A,8A,8E)},
    {Id::Env,        "Dotenv",       "dotenv",      "ENV",  C32(ED,D5,4C)},
    // build / infra
    {Id::Dockerfile, "Dockerfile",   "dockerfile",  "DOCK", C32(23,96,ED)},
    {Id::Makefile,   "Makefile",     "makefile",    "MAKE", C32(A0,4A,3C)},
    {Id::Cmake,      "CMake",        "cmake",       "CMAK", C32(06,4F,8C)},
    {Id::Meson,      "Meson",        "meson",       "MESN", C32(4A,90,D9)},
    {Id::Gradle,     "Gradle",       "gradle",      "GRDL", C32(02,30,3A)},
    {Id::Nix,        "Nix",          "nix",         "NIX",  C32(52,77,B7)},
    {Id::Terraform,  "Terraform",    "hcl",         "TF",   C32(7B,42,BC)},
    {Id::Bazel,      "Bazel",        "bazel",       "BZL",  C32(43,A0,47)},
    // docs & misc
    {Id::Markdown,   "Markdown",     "markdown",    "MD",   C32(75,75,75)},
    {Id::Rst,        "reStructured", "rst",         "RST",  C32(8A,8A,8E)},
    {Id::Latex,      "LaTeX",        "latex",       "TEX",  C32(00,80,80)},
    {Id::AsciiDoc,   "AsciiDoc",     "asciidoc",    "ADOC", C32(8A,8A,8E)},
    {Id::Diff,       "Diff",         "diff",        "DIFF", C32(6A,99,55)},
    {Id::Log,        "Log",          "log",         "LOG",  C32(8A,8A,8E)},
    {Id::Assembly,   "Assembly",     "asm",         "ASM",  C32(6E,6E,6E)},
    {Id::Fortran,    "Fortran",      "fortran",     "F90",  C32(73,4F,96)},
    {Id::Matlab,     "MATLAB",       "matlab",      "M",    C32(D9,53,19)},
    {Id::Pascal,     "Pascal",       "pascal",      "PAS",  C32(E3,6D,26)},
    {Id::Ada,        "Ada",          "ada",         "ADA",  C32(19,54,0E)},
    {Id::Cobol,      "COBOL",        "cobol",       "COB",  C32(29,5C,A4)},
    {Id::Text,       "Text",         "text",        "TXT",  C32(8A,8A,8E)},
};

#undef C32

static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == (size_t)Id::Count,
              "kInfo must have exactly one row per Id");

// Shorthand for building the spec table: most languages fall into a handful of
// comment/string families.
Spec make(const char* const* kw, const char* const* ty, const char* line,
          const char* block_open = "", const char* block_close = "",
          bool preproc = false, bool chars = false, bool triple = false,
          bool backtick = false, bool nocase = false, bool nested = false,
          const char* line_alt = "") {
    Spec s;
    s.keywords = kw ? kw : kEmpty;
    s.types = ty ? ty : kEmpty;
    s.line_comment = line;
    s.line_comment_alt = line_alt;
    s.block_open = block_open;
    s.block_close = block_close;
    s.preproc_hash = preproc;
    s.char_literals = chars;
    s.triple_quotes = triple;
    s.backtick_strings = backtick;
    s.case_insensitive = nocase;
    s.nested_blocks = nested;
    return s;
}

const Spec& spec_table(Id id) {
    static Spec table[(size_t)Id::Count];
    static bool built = false;
    if (!built) {
        for (size_t i = 0; i < (size_t)Id::Count; ++i) table[i] = make(kEmpty, kEmpty, "");
        auto S = [&](Id which, const Spec& s) { table[(size_t)which] = s; };

        // --- C family: // and /* */, char literals, some with a preprocessor
        S(Id::C,        make(kCKeywords, kCTypes, "//", "/*", "*/", true, true));
        S(Id::Cpp,      make(kCppKeywords, kCppTypes, "//", "/*", "*/", true, true));
        S(Id::ObjC,     make(kObjCKeywords, kCTypes, "//", "/*", "*/", true, true));
        S(Id::Cuda,     make(kCppKeywords, kCppTypes, "//", "/*", "*/", true, true));
        S(Id::Glsl,     make(kGlslKeywords, kGlslTypes, "//", "/*", "*/", true, false));
        S(Id::CSharp,   make(kCSharpKeywords, kCSharpTypes, "//", "/*", "*/", false, true));
        S(Id::Java,     make(kJavaKeywords, kJavaTypes, "//", "/*", "*/", false, true));
        S(Id::Kotlin,   make(kKotlinKeywords, kKotlinTypes, "//", "/*", "*/", false, true));
        S(Id::Scala,    make(kScalaKeywords, kScalaTypes, "//", "/*", "*/", false, true));
        S(Id::Groovy,   make(kGroovyKeywords, kJavaTypes, "//", "/*", "*/", false, true));
        S(Id::Gradle,   make(kGroovyKeywords, kJavaTypes, "//", "/*", "*/", false, true));
        S(Id::Dart,     make(kDartKeywords, kDartTypes, "//", "/*", "*/", false, false));
        S(Id::Swift,    make(kSwiftKeywords, kSwiftTypes, "//", "/*", "*/", false, false,
                             false, false, false, true));
        S(Id::Go,       make(kGoKeywords, kGoTypes, "//", "/*", "*/", false, true, false, true));
        S(Id::Rust,     make(kRustKeywords, kRustTypes, "//", "/*", "*/", false, true, false,
                             false, false, true));
        S(Id::Zig,      make(kZigKeywords, kZigTypes, "//", "", "", false, true));
        S(Id::D,        make(kDKeywords, kCppTypes, "//", "/*", "*/", false, true, false,
                             true, false, true));
        S(Id::Vala,     make(kCSharpKeywords, kCSharpTypes, "//", "/*", "*/", false, true));
        S(Id::Haxe,     make(kJavaKeywords, kJavaTypes, "//", "/*", "*/", false, true));
        S(Id::Solidity, make(kSolidityKeywords, kSolidityTypes, "//", "/*", "*/"));
        S(Id::Verilog,  make(kVerilogKeywords, kEmpty, "//", "/*", "*/"));
        S(Id::Vhdl,     make(kVhdlKeywords, kEmpty, "--", "", "", false, false, false, false, true));
        S(Id::Proto,    make(kProtoKeywords, kProtoTypes, "//", "/*", "*/"));
        S(Id::GraphQl,  make(kGraphQlKeywords, kGraphQlTypes, "#"));

        // --- JS/TS and the frameworks that embed them
        S(Id::JavaScript, make(kJsKeywords, kJsTypes, "//", "/*", "*/", false, false, false, true));
        S(Id::TypeScript, make(kJsKeywords, kJsTypes, "//", "/*", "*/", false, false, false, true));
        S(Id::Jsx,        make(kJsKeywords, kJsTypes, "//", "/*", "*/", false, false, false, true));
        S(Id::Tsx,        make(kJsKeywords, kJsTypes, "//", "/*", "*/", false, false, false, true));
        S(Id::Svelte,     make(kJsKeywords, kJsTypes, "//", "<!--", "-->", false, false, false, true));
        S(Id::Vue,        make(kJsKeywords, kJsTypes, "//", "<!--", "-->", false, false, false, true));
        S(Id::Astro,      make(kJsKeywords, kJsTypes, "//", "<!--", "-->", false, false, false, true));
        S(Id::Json,       make(kJsonKeywords, kEmpty, ""));
        S(Id::Handlebars, make(kEmpty, kEmpty, "", "{{!", "}}"));

        // --- scripting: # comments
        S(Id::Python,   make(kPythonKeywords, kPythonTypes, "#", "", "", false, false, true));
        S(Id::Ruby,     make(kRubyKeywords, kRubyTypes, "#", "=begin", "=end"));
        S(Id::Crystal,  make(kCrystalKeywords, kRubyTypes, "#"));
        S(Id::Perl,     make(kPerlKeywords, kEmpty, "#"));
        S(Id::Php,      make(kPhpKeywords, kPhpTypes, "//", "/*", "*/", false, false, false,
                             false, false, false, "#"));
        S(Id::Shell,    make(kShellKeywords, kShellTypes, "#"));
        S(Id::R,        make(kRKeywords, kEmpty, "#"));
        S(Id::Julia,    make(kJuliaKeywords, kEmpty, "#", "#=", "=#", false, false, true));
        S(Id::Nim,      make(kNimKeywords, kEmpty, "#", "", "", false, false, true));
        S(Id::Tcl,      make(kTclKeywords, kEmpty, "#"));
        S(Id::Lua,      make(kLuaKeywords, kLuaTypes, "--", "--[[", "]]"));
        S(Id::Nix,      make(kNixKeywords, kEmpty, "#", "/*", "*/"));
        S(Id::Terraform,make(kTerraformKeywords, kEmpty, "#", "/*", "*/", false, false, false,
                             false, false, false, "//"));
        S(Id::Bazel,    make(kBazelKeywords, kPythonTypes, "#", "", "", false, false, true));
        S(Id::Meson,    make(kMesonKeywords, kEmpty, "#", "", "", false, false, true));
        S(Id::Cmake,    make(kCmakeKeywords, kEmpty, "#"));
        S(Id::Makefile, make(kMakeKeywords, kShellTypes, "#"));
        S(Id::Dockerfile, make(kDockerKeywords, kShellTypes, "#"));
        S(Id::Yaml,     make(kYamlKeywords, kEmpty, "#"));
        S(Id::Toml,     make(kTomlKeywords, kEmpty, "#"));
        S(Id::Ini,      make(kEmpty, kEmpty, "#", "", "", false, false, false, false, false,
                             false, ";"));
        S(Id::Properties, make(kEmpty, kEmpty, "#", "", "", false, false, false, false, false,
                               false, "!"));
        S(Id::Env,      make(kEmpty, kEmpty, "#"));
        S(Id::Elixir,   make(kElixirKeywords, kEmpty, "#", "", "", false, false, true));
        S(Id::PowerShell, make(kPowerShellKeywords, kPowerShellTypes, "#", "<#", "#>"));
        S(Id::Batch,    make(kBatchKeywords, kEmpty, "rem", "", "", false, false, false, false,
                             true, false, "::"));
        S(Id::Vim,      make(kVimKeywords, kEmpty, "\""));

        // --- functional
        S(Id::Haskell,  make(kHaskellKeywords, kHaskellTypes, "--", "{-", "-}", false, true,
                             false, false, false, true));
        S(Id::Elm,      make(kElmKeywords, kHaskellTypes, "--", "{-", "-}", false, true,
                             false, false, false, true));
        S(Id::OCaml,    make(kOCamlKeywords, kEmpty, "", "(*", "*)", false, true, false, false,
                             false, true));
        S(Id::FSharp,   make(kFSharpKeywords, kEmpty, "//", "(*", "*)", false, true, false,
                             false, false, true));
        S(Id::Erlang,   make(kErlangKeywords, kEmpty, "%"));
        S(Id::Prolog,   make(kPrologKeywords, kEmpty, "%", "/*", "*/"));
        S(Id::Clojure,  make(kClojureKeywords, kEmpty, ";"));
        S(Id::Lisp,     make(kLispKeywords, kEmpty, ";", "#|", "|#"));
        S(Id::Scheme,   make(kSchemeKeywords, kEmpty, ";", "#|", "|#"));

        // --- markup & styling
        S(Id::Html,     make(kEmpty, kEmpty, "", "<!--", "-->"));
        S(Id::Xml,      make(kEmpty, kEmpty, "", "<!--", "-->"));
        S(Id::Css,      make(kCssKeywords, kEmpty, "", "/*", "*/"));
        S(Id::Scss,     make(kScssKeywords, kEmpty, "//", "/*", "*/"));
        S(Id::Less,     make(kScssKeywords, kEmpty, "//", "/*", "*/"));
        S(Id::Markdown, make(kEmpty, kEmpty, "", "<!--", "-->"));
        S(Id::Rst,      make(kEmpty, kEmpty, ".."));
        S(Id::AsciiDoc, make(kEmpty, kEmpty, "//"));
        S(Id::Latex,    make(kEmpty, kEmpty, "%"));

        // --- data & legacy
        S(Id::Sql,      make(kSqlKeywords, kSqlTypes, "--", "/*", "*/", false, false, false,
                             false, true));
        S(Id::Csv,      make(kEmpty, kEmpty, ""));
        S(Id::Diff,     make(kEmpty, kEmpty, ""));
        S(Id::Log,      make(kEmpty, kEmpty, ""));
        S(Id::Assembly, make(kAsmKeywords, kAsmTypes, ";", "", "", false, false, false, false,
                             true, false, "#"));
        S(Id::Fortran,  make(kFortranKeywords, kEmpty, "!", "", "", false, false, false, false,
                             true));
        S(Id::Matlab,   make(kMatlabKeywords, kEmpty, "%", "%{", "%}"));
        S(Id::Pascal,   make(kPascalKeywords, kEmpty, "//", "{", "}", false, false, false,
                             false, true));
        S(Id::Ada,      make(kAdaKeywords, kEmpty, "--", "", "", false, false, false, false, true));
        S(Id::Cobol,    make(kCobolKeywords, kEmpty, "*>", "", "", false, false, false, false,
                             true));
        S(Id::AppleScript, make(kAppleScriptKeywords, kEmpty, "--", "(*", "*)"));

        built = true;
    }
    size_t i = (size_t)id;
    if (i >= (size_t)Id::Count) i = 0;
    return table[i];
}

// ------------------------------------------------------------- detection

struct Entry { const char* key; Id id; };

// Extensions, without the dot. Longest-suffix cases (".tar.gz" style) are not
// needed here — source files are single-extension in practice.
const Entry kExtensions[] = {
    {"c", Id::C}, {"h", Id::C},
    {"cpp", Id::Cpp}, {"cc", Id::Cpp}, {"cxx", Id::Cpp}, {"c++", Id::Cpp},
    {"hpp", Id::Cpp}, {"hh", Id::Cpp}, {"hxx", Id::Cpp}, {"h++", Id::Cpp},
    {"ipp", Id::Cpp}, {"inl", Id::Cpp}, {"tpp", Id::Cpp},
    {"m", Id::ObjC}, {"mm", Id::ObjC},
    {"cs", Id::CSharp}, {"csx", Id::CSharp},
    {"java", Id::Java}, {"jav", Id::Java},
    {"kt", Id::Kotlin}, {"kts", Id::Kotlin},
    {"scala", Id::Scala}, {"sbt", Id::Scala},
    {"groovy", Id::Groovy},
    {"dart", Id::Dart},
    {"swift", Id::Swift},
    {"go", Id::Go},
    {"rs", Id::Rust},
    {"zig", Id::Zig},
    {"d", Id::D}, {"di", Id::D},
    {"vala", Id::Vala}, {"vapi", Id::Vala},
    {"hx", Id::Haxe},
    {"sol", Id::Solidity},
    {"v", Id::Verilog}, {"sv", Id::Verilog}, {"svh", Id::Verilog}, {"vh", Id::Verilog},
    {"vhd", Id::Vhdl}, {"vhdl", Id::Vhdl},
    {"glsl", Id::Glsl}, {"vert", Id::Glsl}, {"frag", Id::Glsl}, {"geom", Id::Glsl},
    {"comp", Id::Glsl}, {"hlsl", Id::Glsl}, {"shader", Id::Glsl}, {"wgsl", Id::Glsl},
    {"cu", Id::Cuda}, {"cuh", Id::Cuda},

    {"py", Id::Python}, {"pyw", Id::Python}, {"pyi", Id::Python}, {"pyx", Id::Python},
    {"rb", Id::Ruby}, {"rake", Id::Ruby}, {"gemspec", Id::Ruby},
    {"pl", Id::Perl}, {"pm", Id::Perl}, {"t", Id::Perl},
    {"php", Id::Php}, {"phtml", Id::Php}, {"php7", Id::Php}, {"php8", Id::Php},
    {"lua", Id::Lua},
    {"js", Id::JavaScript}, {"mjs", Id::JavaScript}, {"cjs", Id::JavaScript},
    {"jsx", Id::Jsx},
    {"ts", Id::TypeScript}, {"mts", Id::TypeScript}, {"cts", Id::TypeScript},
    {"tsx", Id::Tsx},
    {"sh", Id::Shell}, {"bash", Id::Shell}, {"zsh", Id::Shell}, {"ksh", Id::Shell},
    {"fish", Id::Shell}, {"ebuild", Id::Shell},
    {"ps1", Id::PowerShell}, {"psm1", Id::PowerShell}, {"psd1", Id::PowerShell},
    {"bat", Id::Batch}, {"cmd", Id::Batch},
    {"r", Id::R}, {"rmd", Id::R},
    {"jl", Id::Julia},
    {"tcl", Id::Tcl},
    {"nim", Id::Nim}, {"nims", Id::Nim},
    {"cr", Id::Crystal},
    {"vim", Id::Vim},
    {"applescript", Id::AppleScript}, {"scpt", Id::AppleScript},

    {"hs", Id::Haskell}, {"lhs", Id::Haskell},
    {"ml", Id::OCaml}, {"mli", Id::OCaml},
    {"fs", Id::FSharp}, {"fsi", Id::FSharp}, {"fsx", Id::FSharp},
    {"ex", Id::Elixir}, {"exs", Id::Elixir},
    {"erl", Id::Erlang}, {"hrl", Id::Erlang},
    {"clj", Id::Clojure}, {"cljs", Id::Clojure}, {"cljc", Id::Clojure}, {"edn", Id::Clojure},
    {"lisp", Id::Lisp}, {"lsp", Id::Lisp}, {"cl", Id::Lisp}, {"el", Id::Lisp},
    {"scm", Id::Scheme}, {"ss", Id::Scheme}, {"rkt", Id::Scheme},
    {"elm", Id::Elm},
    {"pro", Id::Prolog}, {"plg", Id::Prolog},

    {"html", Id::Html}, {"htm", Id::Html}, {"xhtml", Id::Html},
    {"xml", Id::Xml}, {"svg", Id::Xml}, {"xsl", Id::Xml}, {"xsd", Id::Xml},
    {"plist", Id::Xml}, {"csproj", Id::Xml}, {"vcxproj", Id::Xml}, {"resx", Id::Xml},
    {"css", Id::Css},
    {"scss", Id::Scss}, {"sass", Id::Scss},
    {"less", Id::Less},
    {"svelte", Id::Svelte},
    {"vue", Id::Vue},
    {"astro", Id::Astro},
    {"hbs", Id::Handlebars}, {"handlebars", Id::Handlebars}, {"mustache", Id::Handlebars},

    {"json", Id::Json}, {"jsonc", Id::Json}, {"json5", Id::Json}, {"ipynb", Id::Json},
    {"yaml", Id::Yaml}, {"yml", Id::Yaml},
    {"toml", Id::Toml},
    {"ini", Id::Ini}, {"cfg", Id::Ini}, {"conf", Id::Ini}, {"editorconfig", Id::Ini},
    {"csv", Id::Csv}, {"tsv", Id::Csv},
    {"sql", Id::Sql}, {"ddl", Id::Sql}, {"psql", Id::Sql},
    {"graphql", Id::GraphQl}, {"gql", Id::GraphQl},
    {"proto", Id::Proto},
    {"properties", Id::Properties},
    {"env", Id::Env},

    {"cmake", Id::Cmake},
    {"gradle", Id::Gradle},
    {"nix", Id::Nix},
    {"tf", Id::Terraform}, {"tfvars", Id::Terraform}, {"hcl", Id::Terraform},
    {"bzl", Id::Bazel}, {"bazel", Id::Bazel},

    {"md", Id::Markdown}, {"markdown", Id::Markdown}, {"mdx", Id::Markdown},
    {"rst", Id::Rst},
    {"tex", Id::Latex}, {"sty", Id::Latex}, {"cls", Id::Latex},
    {"adoc", Id::AsciiDoc}, {"asciidoc", Id::AsciiDoc},
    {"diff", Id::Diff}, {"patch", Id::Diff},
    {"log", Id::Log},
    {"asm", Id::Assembly}, {"s", Id::Assembly}, {"nasm", Id::Assembly},
    {"f", Id::Fortran}, {"f90", Id::Fortran}, {"f95", Id::Fortran}, {"f03", Id::Fortran},
    {"for", Id::Fortran},
    {"pas", Id::Pascal}, {"pp", Id::Pascal}, {"dpr", Id::Pascal},
    {"ada", Id::Ada}, {"adb", Id::Ada}, {"ads", Id::Ada},
    {"cob", Id::Cobol}, {"cbl", Id::Cobol},
    {"txt", Id::Text},
};

// Whole filenames that decide the language on their own.
const Entry kFilenames[] = {
    {"makefile", Id::Makefile}, {"gnumakefile", Id::Makefile}, {"makefile.am", Id::Makefile},
    {"makefile.in", Id::Makefile},
    {"dockerfile", Id::Dockerfile}, {"containerfile", Id::Dockerfile},
    {"cmakelists.txt", Id::Cmake},
    {"meson.build", Id::Meson}, {"meson_options.txt", Id::Meson},
    {"build", Id::Bazel}, {"build.bazel", Id::Bazel}, {"workspace", Id::Bazel},
    {"gemfile", Id::Ruby}, {"rakefile", Id::Ruby}, {"podfile", Id::Ruby},
    {"package.json", Id::Json}, {"tsconfig.json", Id::Json}, {"composer.json", Id::Json},
    {"cargo.toml", Id::Toml}, {"pyproject.toml", Id::Toml},
    {"go.mod", Id::Go}, {"go.sum", Id::Text},
    {".gitignore", Id::Text}, {".gitattributes", Id::Text}, {".dockerignore", Id::Text},
    {".env", Id::Env}, {".editorconfig", Id::Ini},
    {".bashrc", Id::Shell}, {".bash_profile", Id::Shell}, {".zshrc", Id::Shell},
    {".profile", Id::Shell}, {"configure", Id::Shell}, {"pkgbuild", Id::Shell},
    {".vimrc", Id::Vim}, {"vimrc", Id::Vim},
    {"license", Id::Text}, {"readme", Id::Markdown}, {"changelog", Id::Markdown},
    {"flake.nix", Id::Nix}, {"default.nix", Id::Nix},
};

// Fence tags the models actually emit that do not match a real extension.
const Entry kTagAliases[] = {
    {"c++", Id::Cpp}, {"cplusplus", Id::Cpp}, {"objective-c", Id::ObjC}, {"objc", Id::ObjC},
    {"objectivec", Id::ObjC}, {"c#", Id::CSharp}, {"csharp", Id::CSharp}, {"cs", Id::CSharp},
    {"f#", Id::FSharp}, {"fsharp", Id::FSharp},
    {"python3", Id::Python}, {"py3", Id::Python}, {"node", Id::JavaScript},
    {"shell", Id::Shell}, {"console", Id::Shell}, {"sh", Id::Shell}, {"zsh", Id::Shell},
    {"pwsh", Id::PowerShell}, {"powershell", Id::PowerShell},
    {"golang", Id::Go}, {"rustlang", Id::Rust},
    {"yml", Id::Yaml}, {"dotenv", Id::Env}, {"protobuf", Id::Proto},
    {"htm", Id::Html}, {"vuejs", Id::Vue}, {"react", Id::Jsx},
    {"terraform", Id::Terraform}, {"hcl", Id::Terraform},
    {"docker", Id::Dockerfile}, {"dockerfile", Id::Dockerfile},
    {"make", Id::Makefile}, {"makefile", Id::Makefile}, {"cmake", Id::Cmake},
    {"plaintext", Id::Text}, {"plain", Id::Text}, {"txt", Id::Text}, {"text", Id::Text},
    {"assembly", Id::Assembly}, {"nasm", Id::Assembly}, {"x86asm", Id::Assembly},
    {"jsonc", Id::Json}, {"json5", Id::Json},
    {"bash", Id::Shell}, {"batchfile", Id::Batch}, {"bat", Id::Batch},
};

std::string lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (char)std::tolower((unsigned char)c);
    return out;
}

// Builds a lookup map once, from a table of {key, id} pairs.
const std::unordered_map<std::string, Id>& map_of(const Entry* table, size_t count) {
    static std::unordered_map<const Entry*, std::unordered_map<std::string, Id>> cache;
    auto it = cache.find(table);
    if (it != cache.end()) return it->second;
    std::unordered_map<std::string, Id> m;
    m.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) m.emplace(table[i].key, table[i].id);
    return cache.emplace(table, std::move(m)).first->second;
}

#define MAP_OF(t) map_of(t, sizeof(t) / sizeof(t[0]))

} // namespace

const Info& info(Id id) {
    size_t i = (size_t)id;
    if (i >= (size_t)Id::Count) i = 0;
    return kInfo[i];
}

const Spec& spec(Id id) { return spec_table(id); }

Id from_path(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    const std::string file = lower(slash == std::string::npos ? path : path.substr(slash + 1));
    if (file.empty()) return Id::PlainText;

    // Exact filename wins: "Makefile" and "Dockerfile" carry no extension, and
    // "package.json" should not be confused with a hand-written .json file.
    const auto& names = MAP_OF(kFilenames);
    auto byname = names.find(file);
    if (byname != names.end()) return byname->second;

    size_t dot = file.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= file.size()) return Id::PlainText;
    std::string ext = file.substr(dot + 1);

    const auto& exts = MAP_OF(kExtensions);
    auto byext = exts.find(ext);
    if (byext != exts.end()) return byext->second;

    // "foo.test.ts" / "app.module.css": retry with the extension before the last.
    size_t prev = file.find_last_of('.', dot - 1);
    if (dot > 0 && prev != std::string::npos) {
        byext = exts.find(file.substr(dot + 1));
        if (byext != exts.end()) return byext->second;
    }
    return Id::PlainText;
}

Id from_tag(const std::string& raw) {
    std::string t = lower(raw);
    while (!t.empty() && (t.front() == '.' || t.front() == ' ')) t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\r')) t.pop_back();
    if (t.empty()) return Id::PlainText;

    const auto& aliases = MAP_OF(kTagAliases);
    auto alias = aliases.find(t);
    if (alias != aliases.end()) return alias->second;

    // The canonical tag of each language.
    for (size_t i = 0; i < (size_t)Id::Count; ++i)
        if (t == kInfo[i].tag) return kInfo[i].id;

    const auto& exts = MAP_OF(kExtensions);
    auto byext = exts.find(t);
    if (byext != exts.end()) return byext->second;
    return Id::PlainText;
}

bool is_code(Id id) {
    switch (id) {
        case Id::PlainText: case Id::Text: case Id::Log: case Id::Csv:
        case Id::Markdown: case Id::Rst: case Id::AsciiDoc: case Id::Diff:
            return false;
        default:
            return true;
    }
}

} // namespace lang
