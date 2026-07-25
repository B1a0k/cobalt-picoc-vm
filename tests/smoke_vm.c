#include "cvm/format.h"
#include "cvm/opcode.h"
#include "cvm/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SmokePackage {
    CvmPackageHeader header;
    CvmSectionHeader sections[4];
    char strings[16];
    CvmParameter parameters[2];
    CvmFunction functions[2];
    CvmInstruction code[10];
} SmokePackage;

static CvmInstruction instruction(
    CvmOpcode opcode,
    CvmValueType type,
    int32_t a,
    int32_t b)
{
    CvmInstruction result;
    memset(&result, 0, sizeof(result));
    result.opcode = (uint8_t)opcode;
    result.type = (uint8_t)type;
    result.a = a;
    result.b = b;
    return result;
}

static void section(
    CvmSectionHeader *header,
    CvmSectionKind kind,
    uint32_t offset,
    uint32_t size,
    uint32_t count,
    uint32_t entry_size)
{
    memset(header, 0, sizeof(*header));
    header->kind = (uint16_t)kind;
    header->flags = CVM_SECTION_REQUIRED;
    header->offset = offset;
    header->size = size;
    header->count = count;
    header->entry_size = entry_size;
}

int main(void)
{
    SmokePackage package;
    CvmModule *module = NULL;
    CvmDiagnostic diagnostic;
    CvmValue result;
    CvmStatus status;

    memset(&package, 0, sizeof(package));
    package.header.magic = CVM_MAGIC;
    package.header.format_major = CVM_FORMAT_MAJOR;
    package.header.format_minor = CVM_FORMAT_MINOR;
#if UINTPTR_MAX == UINT32_MAX
    package.header.target_arch = CVM_ARCH_X86;
    package.header.pointer_size = 4;
#elif UINTPTR_MAX == UINT64_MAX
    package.header.target_arch = CVM_ARCH_X64;
    package.header.pointer_size = 8;
#else
#error Unsupported VM host pointer width
#endif
    package.header.endian = 1;
    package.header.profile = CVM_PROFILE_PICOC_COMPAT;
    package.header.section_count = 4;
    package.header.entry_function = 0;
    package.header.package_size = (uint32_t)sizeof(package);
    package.header.required_stack_cells = 16;
    package.header.required_call_depth = 4;

    memcpy(package.strings, "__entry\0add\0a\0b\0", 16);

    section(
        &package.sections[0],
        CVM_SECTION_STRINGS,
        (uint32_t)offsetof(SmokePackage, strings),
        sizeof(package.strings),
        0,
        0);
    section(
        &package.sections[1],
        CVM_SECTION_PARAMETERS,
        (uint32_t)offsetof(SmokePackage, parameters),
        sizeof(package.parameters),
        2,
        sizeof(CvmParameter));
    section(
        &package.sections[2],
        CVM_SECTION_FUNCTIONS,
        (uint32_t)offsetof(SmokePackage, functions),
        sizeof(package.functions),
        2,
        sizeof(CvmFunction));
    section(
        &package.sections[3],
        CVM_SECTION_CODE,
        (uint32_t)offsetof(SmokePackage, code),
        sizeof(package.code),
        10,
        sizeof(CvmInstruction));

    package.parameters[0].name_string = 12;
    package.parameters[0].frame_offset = 0;
    package.parameters[0].value_type = CVM_TYPE_I32;
    package.parameters[1].name_string = 14;
    package.parameters[1].frame_offset = 4;
    package.parameters[1].value_type = CVM_TYPE_I32;

    package.functions[0].name_string = 0;
    package.functions[0].first_instruction = 0;
    package.functions[0].instruction_count = 4;
    package.functions[0].return_type = CVM_TYPE_I32;
    package.functions[0].maximum_stack_cells = 2;

    package.functions[1].name_string = 8;
    package.functions[1].first_instruction = 4;
    package.functions[1].instruction_count = 5;
    package.functions[1].first_parameter = 0;
    package.functions[1].parameter_count = 2;
    package.functions[1].return_type = CVM_TYPE_I32;
    package.functions[1].local_bytes = 8;
    package.functions[1].local_alignment = 4;
    package.functions[1].maximum_stack_cells = 2;

    package.code[0] =
        instruction(CVM_OP_PUSH_IMMEDIATE, CVM_TYPE_I32, 2, 0);
    package.code[1] =
        instruction(CVM_OP_PUSH_IMMEDIATE, CVM_TYPE_I32, 3, 0);
    package.code[2] = instruction(CVM_OP_CALL, CVM_TYPE_I32, 1, 2);
    package.code[3] = instruction(CVM_OP_RETURN, CVM_TYPE_I32, 0, 0);

    package.code[4] =
        instruction(CVM_OP_ADDRESS_ARGUMENT, CVM_TYPE_PTR, 0, 0);
    package.code[5] = instruction(CVM_OP_LOAD, CVM_TYPE_I32, 0, 0);
    package.code[6] =
        instruction(CVM_OP_ADDRESS_ARGUMENT, CVM_TYPE_PTR, 4, 0);
    package.code[7] = instruction(CVM_OP_LOAD, CVM_TYPE_I32, 0, 0);
    package.code[8] = instruction(CVM_OP_ADD, CVM_TYPE_I32, 0, 0);
    package.code[9] = instruction(CVM_OP_RETURN, CVM_TYPE_I32, 0, 0);
    package.functions[1].instruction_count = 6;

    memset(&diagnostic, 0, sizeof(diagnostic));
    status = cvm_module_load(
        &package,
        sizeof(package),
        NULL,
        &module,
        &diagnostic);

    if (status != CVM_STATUS_OK) {
        fprintf(stderr, "load failed: %s\n", diagnostic.message);
        return 1;
    }

    memset(&result, 0, sizeof(result));
    status = cvm_module_execute(module, NULL, &result, &diagnostic);
    cvm_module_destroy(module);

    if (status != CVM_STATUS_OK) {
        fprintf(stderr, "execute failed: %s\n", diagnostic.message);
        return 1;
    }
    if (result.i64 != 5) {
        fprintf(stderr, "expected 5, got %lld\n", (long long)result.i64);
        return 1;
    }

    package.header.magic = 0;
    module = NULL;
    status = cvm_module_load(
        &package,
        sizeof(package),
        NULL,
        &module,
        &diagnostic);
    if (status != CVM_STATUS_INVALID_PACKAGE || module != NULL) {
        fprintf(stderr, "invalid package magic was accepted\n");
        cvm_module_destroy(module);
        return 1;
    }
    package.header.magic = CVM_MAGIC;

    package.code[0] =
        instruction(CVM_OP_JUMP, CVM_TYPE_VOID, 4, 0);
    module = NULL;
    status = cvm_module_load(
        &package,
        sizeof(package),
        NULL,
        &module,
        &diagnostic);
    if (status != CVM_STATUS_VERIFICATION_FAILED || module != NULL) {
        fprintf(stderr, "cross-function branch was accepted\n");
        cvm_module_destroy(module);
        return 1;
    }
    package.code[0] =
        instruction(CVM_OP_PUSH_IMMEDIATE, CVM_TYPE_I32, 2, 0);

    printf(
        "smoke: add(2, 3) = %lld; malformed packages rejected\n",
        (long long)result.i64);
    return 0;
}
