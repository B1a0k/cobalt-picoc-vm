#include "cvm/native.h"

#if !defined(_WIN32)
#error native_windows.c is only supported on Windows
#endif

#include <stdio.h>
#include <string.h>

#define A0
#define A1 uintptr_t
#define A2 A1, uintptr_t
#define A3 A2, uintptr_t
#define A4 A3, uintptr_t
#define A5 A4, uintptr_t
#define A6 A5, uintptr_t
#define A7 A6, uintptr_t
#define A8 A7, uintptr_t
#define A9 A8, uintptr_t
#define A10 A9, uintptr_t
#define A11 A10, uintptr_t
#define A12 A11, uintptr_t
#define A13 A12, uintptr_t
#define A14 A13, uintptr_t
#define A15 A14, uintptr_t
#define A16 A15, uintptr_t

#define V0
#define V1 values[0]
#define V2 V1, values[1]
#define V3 V2, values[2]
#define V4 V3, values[3]
#define V5 V4, values[4]
#define V6 V5, values[5]
#define V7 V6, values[6]
#define V8 V7, values[7]
#define V9 V8, values[8]
#define V10 V9, values[9]
#define V11 V10, values[10]
#define V12 V11, values[11]
#define V13 V12, values[12]
#define V14 V13, values[13]
#define V15 V14, values[14]
#define V16 V15, values[15]

#define CALL_CASE(N, CC) \
    case N: \
        raw = ((uintptr_t (CC *)(A##N))address)(V##N); \
        break

static void set_error(
    CvmDiagnostic *diagnostic,
    const char *message)
{
    if (diagnostic == NULL)
        return;
    diagnostic->status = CVM_STATUS_NATIVE_CALL_FAILED;
    strncpy(
        diagnostic->message,
        message,
        sizeof(diagnostic->message) - 1);
    diagnostic->message[sizeof(diagnostic->message) - 1] = '\0';
}

#if defined(_MSC_VER) && defined(_M_IX86)
/*
 * Pushes the already-marshalled argument words in reverse order. Resetting
 * ESP from EBP after the call makes the gate safe for both cdecl (caller
 * cleanup) and stdcall (callee cleanup). EDX:EAX is preserved as the raw
 * 64-bit return cell.
 */
__declspec(naked) static uint64_t __cdecl invoke_x86_words(
    uintptr_t address,
    const uint32_t *words,
    uint32_t word_count)
{
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov ebx, [ebp + 8]
        mov esi, [ebp + 12]
        mov ecx, [ebp + 16]
    push_loop:
        test ecx, ecx
        jz do_call
        dec ecx
        push dword ptr [esi + ecx * 4]
        jmp push_loop
    do_call:
        call ebx
        lea esp, [ebp - 12]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
#endif

#if !(defined(_MSC_VER) && defined(_M_IX86))
static CvmStatus invoke_cdecl(
    uintptr_t address,
    const uintptr_t *values,
    uint32_t count,
    uintptr_t *result)
{
    uintptr_t raw = 0;
    switch (count) {
    CALL_CASE(0, __cdecl);
    CALL_CASE(1, __cdecl);
    CALL_CASE(2, __cdecl);
    CALL_CASE(3, __cdecl);
    CALL_CASE(4, __cdecl);
    CALL_CASE(5, __cdecl);
    CALL_CASE(6, __cdecl);
    CALL_CASE(7, __cdecl);
    CALL_CASE(8, __cdecl);
    CALL_CASE(9, __cdecl);
    CALL_CASE(10, __cdecl);
    CALL_CASE(11, __cdecl);
    CALL_CASE(12, __cdecl);
    CALL_CASE(13, __cdecl);
    CALL_CASE(14, __cdecl);
    CALL_CASE(15, __cdecl);
    CALL_CASE(16, __cdecl);
    default:
        return CVM_STATUS_NATIVE_CALL_FAILED;
    }
    *result = raw;
    return CVM_STATUS_OK;
}

static CvmStatus invoke_stdcall(
    uintptr_t address,
    const uintptr_t *values,
    uint32_t count,
    uintptr_t *result)
{
    uintptr_t raw = 0;
    switch (count) {
    CALL_CASE(0, __stdcall);
    CALL_CASE(1, __stdcall);
    CALL_CASE(2, __stdcall);
    CALL_CASE(3, __stdcall);
    CALL_CASE(4, __stdcall);
    CALL_CASE(5, __stdcall);
    CALL_CASE(6, __stdcall);
    CALL_CASE(7, __stdcall);
    CALL_CASE(8, __stdcall);
    CALL_CASE(9, __stdcall);
    CALL_CASE(10, __stdcall);
    CALL_CASE(11, __stdcall);
    CALL_CASE(12, __stdcall);
    CALL_CASE(13, __stdcall);
    CALL_CASE(14, __stdcall);
    CALL_CASE(15, __stdcall);
    CALL_CASE(16, __stdcall);
    default:
        return CVM_STATUS_NATIVE_CALL_FAILED;
    }
    *result = raw;
    return CVM_STATUS_OK;
}
#endif

CvmStatus cvm_native_invoke_windows(
    uintptr_t address,
    const CvmNativeCall *call,
    CvmValue *return_value,
    CvmDiagnostic *diagnostic)
{
    uintptr_t raw = 0;
    uint64_t raw64 = 0;
    uint32_t index;
#if !(defined(_MSC_VER) && defined(_M_IX86))
    uintptr_t values[16];
    CvmStatus status;
#endif

    if (address == 0 || call == NULL || return_value == NULL ||
        call->parameter_count > 16) {
        set_error(diagnostic, "invalid native call request");
        return CVM_STATUS_NATIVE_CALL_FAILED;
    }
#if defined(_MSC_VER) && defined(_M_IX86)
    {
        uint32_t words[32];
        uint32_t word_count = 0;
        for (index = 0; index < call->parameter_count; ++index) {
            CvmValueType type = call->parameter_types[index];
            if (type == CVM_TYPE_I64 ||
                type == CVM_TYPE_U64 ||
                type == CVM_TYPE_F64) {
                if (word_count + 2 > 32) {
                    set_error(
                        diagnostic,
                        "x86 native argument word limit exceeded");
                    return CVM_STATUS_NATIVE_CALL_FAILED;
                }
                words[word_count++] =
                    (uint32_t)call->arguments[index].u64;
                words[word_count++] =
                    (uint32_t)(call->arguments[index].u64 >> 32);
            } else {
                if (word_count + 1 > 32) {
                    set_error(
                        diagnostic,
                        "x86 native argument word limit exceeded");
                    return CVM_STATUS_NATIVE_CALL_FAILED;
                }
                words[word_count++] =
                    (uint32_t)call->arguments[index].u64;
            }
        }
        raw64 = invoke_x86_words(address, words, word_count);
        raw = (uintptr_t)raw64;
    }
#else
    for (index = 0; index < call->parameter_count; ++index) {
        values[index] = (uintptr_t)call->arguments[index].u64;
    }

    if (call->calling_convention == CVM_CALL_STDCALL)
        status = invoke_stdcall(
            address,
            values,
            call->parameter_count,
            &raw);
    else
        status = invoke_cdecl(
            address,
            values,
            call->parameter_count,
            &raw);
    if (status != CVM_STATUS_OK) {
        set_error(diagnostic, "native call gate rejected the signature");
        return status;
    }
    raw64 = (uint64_t)raw;
#endif

    return_value->u64 = raw64;
    switch (call->return_type) {
    case CVM_TYPE_I8:
        return_value->i64 = (int8_t)raw;
        break;
    case CVM_TYPE_U8:
        return_value->u64 = (uint8_t)raw;
        break;
    case CVM_TYPE_I16:
        return_value->i64 = (int16_t)raw;
        break;
    case CVM_TYPE_U16:
        return_value->u64 = (uint16_t)raw;
        break;
    case CVM_TYPE_I32:
        return_value->i64 = (int32_t)raw;
        break;
    case CVM_TYPE_U32:
        return_value->u64 = (uint32_t)raw;
        break;
    default:
        break;
    }
    return CVM_STATUS_OK;
}
