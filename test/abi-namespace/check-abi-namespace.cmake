# ==============================================================================
# CTest check: every C++ symbol a built library defines lies in its ABI namespace.
#
#   cmake -DFORMAT=ELF|MACHO|PE -DNM=<nm> | -DDUMPBIN=<dumpbin> -DLIBRARY=<file>
#         -DNAMESPACE=<ns> [-DALLOW_SCOPES=std;nlohmann;...] [-DALLOW_GLOBAL_PREFIXES=ma_;...]
#         -P check-abi-namespace.cmake
#
# LIBRARY is a static archive or a shared library. Every defined symbol whose entity lies in
# namespace thl must lie in thl::<NAMESPACE>, and none may lie at global scope; an entity in
# another outermost namespace must be in ALLOW_SCOPES (the standard library and the
# third-party code a component embeds), a global-scope one must start with one of
# ALLOW_GLOBAL_PREFIXES (third-party C code compiled as C++). Internal names (anonymous
# namespaces, `static` where the compiler marks it) are not linked across objects and are
# skipped, and so are the global-scope functions the toolchain itself defines in a library
# (_tanh_toolchain_global below): reserved names, libc++'s ABI-tagged functions and the C++
# overloads of the C library.
#
# ELF and Mach-O: `nm` over every defined symbol, the local ones of a shared library
# included, read as Itanium names (the scope of the entity, past the special-name prefixes
# of vtables, typeinfo, thunks, guards and local entities). PE: `dumpbin /symbols` of an
# archive, `dumpbin /exports` of a DLL (its other symbols are only in the PDB), read as
# MSVC names, where only the outermost namespace and the global-scope functions and
# variables are checked.
# ==============================================================================
cmake_minimum_required(VERSION 3.25)

if(NOT FORMAT MATCHES "^(ELF|MACHO|PE)$")
    message(FATAL_ERROR "check-abi-namespace: FORMAT must be ELF, MACHO or PE (got '${FORMAT}')")
endif()
if(NOT LIBRARY OR NOT EXISTS "${LIBRARY}")
    message(FATAL_ERROR "check-abi-namespace: LIBRARY '${LIBRARY}' does not exist")
endif()
if(NOT NAMESPACE MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR "check-abi-namespace: NAMESPACE must be a C++ identifier (got '${NAMESPACE}')")
endif()

# ------------------------------------------------------------------------------
# The defined symbols.
# ------------------------------------------------------------------------------
set(_symbols "")
if(FORMAT STREQUAL "PE")
    if(LIBRARY MATCHES "\\.[dD][lL][lL]$")
        set(_args /nologo /exports)
    else()
        set(_args /nologo /symbols)
    endif()
    execute_process(COMMAND "${DUMPBIN}" ${_args} "${LIBRARY}"
        OUTPUT_VARIABLE _raw RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "check-abi-namespace: dumpbin failed on ${LIBRARY}: ${_err}")
    endif()
    string(REPLACE ";" "\\;" _raw "${_raw}")
    string(REPLACE "\n" ";" _lines "${_raw}")
    foreach(_line IN LISTS _lines)
        # /symbols: "00C 00000000 SECT4  notype ()    External     | ?name (undecorated)",
        # defined in a section (not UNDEF or ABS). /exports: "ordinal hint RVA name".
        if(_line MATCHES "^[0-9A-Fa-f]+ [0-9A-Fa-f]+ SECT[0-9A-Fa-f]+ .*\\| ([^ \r]+)")
            list(APPEND _symbols "${CMAKE_MATCH_1}")
        elseif(_line MATCHES "^ +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +([^ \r]+)")
            list(APPEND _symbols "${CMAKE_MATCH_1}")
        endif()
    endforeach()
else()
    if(FORMAT STREQUAL "ELF")
        set(_args --defined-only)
    else()
        set(_args -U)
    endif()
    execute_process(COMMAND "${NM}" ${_args} "${LIBRARY}"
        OUTPUT_VARIABLE _raw RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "check-abi-namespace: nm failed on ${LIBRARY}: ${_err}")
    endif()
    string(REPLACE ";" "\\;" _raw "${_raw}")
    string(REPLACE "\n" ";" _lines "${_raw}")
    foreach(_line IN LISTS _lines)
        # "address type name"; archive member headers and blank lines do not match.
        if(_line MATCHES "^[0-9A-Fa-f]* +[A-Za-z?] +(_?_Z[^ ]+)$")
            list(APPEND _symbols "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endif()

# ------------------------------------------------------------------------------
# What the toolchain defines at global scope.
#
# The C library functions that the C++ headers overload in the global namespace rather than
# in std, as inline functions a debug build emits into the library that calls them. Seen:
# MSVC's <cmath> and the UCRT's <math.h> (?fpclassify@@YAHN@Z, ?signbit@@YA_NN@Z; MSVC also
# defines the float and long double overloads of every <cmath> function there), MSVC's
# <stdlib.h> (abs and div for long and long long), libc++'s <math.h> and <stdlib.h> (the
# same, but ABI-tagged, see below). libstdc++ declares its overloads in std. One list for
# every platform: the names are the C library's, so a tanh function of that name at global
# scope would be an error of its own.
# ------------------------------------------------------------------------------
set(_tanh_c_library_overloads
    # <cstdlib> / <stdlib.h>
    abs labs llabs div ldiv lldiv
    # <cmath> / <math.h>: classification and comparison
    fpclassify signbit isfinite isinf isnan isnormal
    isgreater isgreaterequal isless islessequal islessgreater isunordered
    # <cmath> / <math.h>: the functions with float and long double overloads
    acos acosh asin asinh atan atan2 atanh cbrt ceil copysign cos cosh erf erfc exp exp2 expm1
    fabs fdim floor fma fmax fmin fmod frexp hypot ilogb ldexp lgamma llrint llround log
    log10 log1p log2 logb lrint lround modf nearbyint nextafter nexttoward pow remainder
    remquo rint round scalbln scalbn sin sinh sqrt tan tanh tgamma trunc)

# _tanh_toolchain_global(<id> <after> <out>): <out> is TRUE when the global-scope function
# <id> is the toolchain's, not tanh-lib's. <after> is the mangled text after the name
# (Itanium; empty for MSVC). The toolchain's are
#   * reserved names (`__x`, `_X`), which no program may declare: GCC's
#     __static_initialization_and_destruction_0, MSVC's ?__empty_global_delete@@ helpers of
#     the sized and aligned operator delete;
#   * libc++'s _LIBCPP_HIDE_FROM_ABI functions, whose ABI tag is the libc++ version behind one
#     or two letters (hardening mode and exceptions: B8ne190102, B8nn200100; libc++ 15 and 16:
#     B7v160006), e.g. Apple's global ::abs overloads __Z3absB8ne190102d;
#   * the C++ overloads of the C library above.
function(_tanh_toolchain_global id after out)
    set(${out} FALSE PARENT_SCOPE)
    if(id MATCHES "^(__|_[A-Z])" OR id IN_LIST _tanh_c_library_overloads)
        set(${out} TRUE PARENT_SCOPE)
    elseif(after MATCHES "^B([0-9]+)(.*)$")
        string(SUBSTRING "${CMAKE_MATCH_2}" 0 ${CMAKE_MATCH_1} _tag)
        if(_tag MATCHES "^[a-z][a-z]?[0-9][0-9][0-9][0-9][0-9]+$")
            set(${out} TRUE PARENT_SCOPE)
        endif()
    endif()
endfunction()

# ------------------------------------------------------------------------------
# Itanium: the scope of the entity a mangled name denotes.
#
# _tanh_itanium_scope(<name> <out>) sets <out> to
#   "thl:<rest>"  the entity lies in thl; <rest> is the mangled text after `3thl`
#   "ns:<id>"     the outermost scope is namespace or class <id> (std for St/Sa/Ss/...)
#   "global:<id>" an unscoped name: a function, operator or class at global scope
#   "toolchain"   an unscoped name the toolchain defines (_tanh_toolchain_global)
#   "internal"    internal linkage (anonymous namespace, `static`): not checked
#   "other"       not a name this check reads (typeinfo of a pointer, a builtin, ...)
# ------------------------------------------------------------------------------
function(_tanh_itanium_scope name out)
    string(REGEX REPLACE "^_?_Z" "" _s "${name}")
    # Special names: vtables, VTTs, typeinfo (+ name), construction vtables, thunks, TLS
    # wrappers and init functions, guard variables, reference temporaries, local entities.
    while(TRUE)
        if(_s MATCHES "^T[VTIS](.*)$" OR _s MATCHES "^TC(.*)$")
            set(_s "${CMAKE_MATCH_1}")
            # A type follows: a class type is a name, anything else is not ours to read.
            if(NOT _s MATCHES "^(N|Z|S[tabsiod]|[0-9])")
                set(${out} "other" PARENT_SCOPE)
                return()
            endif()
        elseif(_s MATCHES "^T[hv](n?[0-9]+_)+(.*)$")
            set(_s "${CMAKE_MATCH_2}")
        elseif(_s MATCHES "^(T[WH]|G[VR])(.*)$")
            set(_s "${CMAKE_MATCH_2}")
        elseif(_s MATCHES "^Z(.*)$")
            # A local entity: Z <encoding of the enclosing function> E <entity>; the
            # enclosing function's scope is the entity's.
            set(_s "${CMAKE_MATCH_1}")
        else()
            break()
        endif()
    endwhile()

    if(_s MATCHES "^L")
        set(${out} "internal" PARENT_SCOPE)
    elseif(_s MATCHES "^N[rVK]*[RO]?(.*)$")
        set(_s "${CMAKE_MATCH_1}")
        if(_s MATCHES "^S[tabsiod]")
            set(${out} "ns:std" PARENT_SCOPE)
        elseif(_s MATCHES "^([0-9]+)(.*)$")
            set(_len "${CMAKE_MATCH_1}")
            set(_tail "${CMAKE_MATCH_2}")
            string(SUBSTRING "${_tail}" 0 ${_len} _id)
            string(SUBSTRING "${_tail}" ${_len} -1 _rest)
            if(_id STREQUAL "thl")
                set(${out} "thl:${_rest}" PARENT_SCOPE)
            elseif(_id MATCHES "^_GLOBAL__N")
                set(${out} "internal" PARENT_SCOPE)
            else()
                set(${out} "ns:${_id}" PARENT_SCOPE)
            endif()
        else()
            set(${out} "other" PARENT_SCOPE)
        endif()
    elseif(_s MATCHES "^S[tabsiod]")
        set(${out} "ns:std" PARENT_SCOPE)
    elseif(_s MATCHES "^([0-9]+)(.*)$")
        set(_len "${CMAKE_MATCH_1}")
        string(SUBSTRING "${CMAKE_MATCH_2}" 0 ${_len} _id)
        string(SUBSTRING "${CMAKE_MATCH_2}" ${_len} -1 _after)
        _tanh_toolchain_global("${_id}" "${_after}" _is_toolchain)
        if(_is_toolchain)
            set(${out} "toolchain" PARENT_SCOPE)
        else()
            set(${out} "global:${_id}" PARENT_SCOPE)
        endif()
    elseif(_s MATCHES "^(nw|na|dl|da)")
        # operator new / delete: the standard library's, e.g. the placement forms <new>
        # defines inline, which GCC emits at -O0.
        set(${out} "ns:std" PARENT_SCOPE)
    elseif(_s MATCHES "^(cv|[a-z][a-z])")
        set(${out} "global:operator" PARENT_SCOPE)
    else()
        set(${out} "other" PARENT_SCOPE)
    endif()
endfunction()

# _tanh_has_prefix(<id> <out>): <id> starts with one of ALLOW_GLOBAL_PREFIXES.
function(_tanh_has_prefix id out)
    foreach(_prefix IN LISTS ALLOW_GLOBAL_PREFIXES)
        string(FIND "${id}" "${_prefix}" _at)
        if(_at EQUAL 0)
            set(${out} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out} FALSE PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------------------
# The check.
# ------------------------------------------------------------------------------
string(LENGTH "${NAMESPACE}" _ns_length)
set(_ns_itanium "${_ns_length}${NAMESPACE}")
set(_thl_count 0)
set(_outside "")
set(_global "")
set(_toolchain "")
set(_foreign "")

foreach(_symbol IN LISTS _symbols)
    if(FORMAT STREQUAL "PE")
        if(NOT _symbol MATCHES "^\\?")
            continue()
        endif()
        # The outermost scope closes the scope list with "@@". A thl entity reads
        # "...@<ns>@thl@@", or "...<digit>thl@@" when <ns> is a back-reference to a
        # fragment seen earlier in the same name.
        if(_symbol MATCHES "@thl@@" OR _symbol MATCHES "[0-9]thl@@")
            math(EXPR _thl_count "${_thl_count} + 1")
            string(REGEX REPLACE "@${NAMESPACE}@thl@@" "" _stripped "${_symbol}")
            string(REGEX REPLACE "[0-9]thl@@" "" _stripped "${_stripped}")
            if(_stripped MATCHES "@thl@@")
                list(APPEND _outside "${_symbol}")
            endif()
        elseif(_symbol MATCHES "^\\?([A-Za-z_][A-Za-z0-9_]*)@@[0-9A-Z]")
            # ?name@@<type>: a function or variable at global scope.
            set(_id "${CMAKE_MATCH_1}")
            _tanh_toolchain_global("${_id}" "" _is_toolchain)
            _tanh_has_prefix("${_id}" _allowed)
            if(_is_toolchain)
                list(APPEND _toolchain "${_symbol}")
            elseif(NOT _allowed)
                list(APPEND _global "${_symbol}")
            endif()
        endif()
        continue()
    endif()

    _tanh_itanium_scope("${_symbol}" _scope)
    if(_scope MATCHES "^thl:(.*)$")
        math(EXPR _thl_count "${_thl_count} + 1")
        string(FIND "${CMAKE_MATCH_1}" "${_ns_itanium}" _at)
        if(NOT _at EQUAL 0)
            list(APPEND _outside "${_symbol}")
        endif()
    elseif(_scope STREQUAL "toolchain")
        list(APPEND _toolchain "${_symbol}")
    elseif(_scope MATCHES "^global:(.*)$")
        _tanh_has_prefix("${CMAKE_MATCH_1}" _allowed)
        if(NOT _allowed)
            list(APPEND _global "${_symbol}")
        endif()
    elseif(_scope MATCHES "^ns:(.*)$")
        if(NOT CMAKE_MATCH_1 IN_LIST ALLOW_SCOPES)
            list(APPEND _foreign "${_symbol}")
        endif()
    endif()
endforeach()

function(_tanh_report header items out_var)
    list(LENGTH items _n)
    list(SUBLIST items 0 40 _shown)
    string(REPLACE ";" "\n    " _shown "${_shown}")
    set(${out_var} "${header} (${_n}, first 40):\n    ${_shown}" PARENT_SCOPE)
endfunction()

list(LENGTH _symbols _n_symbols)
list(LENGTH _outside _n_outside)
list(LENGTH _global _n_global)
list(LENGTH _toolchain _n_toolchain)
list(LENGTH _foreign _n_foreign)
message(STATUS "check-abi-namespace: ${LIBRARY}: ${_n_symbols} C++ symbols, ${_thl_count} in thl, "
               "${_n_outside} outside thl::${NAMESPACE}, ${_n_global} at global scope "
               "(and ${_n_toolchain} of the toolchain), ${_n_foreign} in other namespaces")

set(_failures "")
if(_thl_count EQUAL 0)
    list(APPEND _failures "no symbol of namespace thl found: the tool's output was not understood")
endif()
if(_outside)
    _tanh_report("symbols of thl outside thl::${NAMESPACE}" "${_outside}" _msg)
    list(APPEND _failures "${_msg}")
endif()
if(_global)
    _tanh_report("C++ symbols at global scope" "${_global}" _msg)
    list(APPEND _failures "${_msg}")
endif()
if(_foreign)
    _tanh_report("symbols in namespaces other than thl and ${ALLOW_SCOPES}" "${_foreign}" _msg)
    list(APPEND _failures "${_msg}")
endif()
if(_failures)
    string(REPLACE ";" "\n" _failures "${_failures}")
    message(FATAL_ERROR "check-abi-namespace: ${LIBRARY}\n${_failures}")
endif()
message(STATUS "check-abi-namespace: OK")
