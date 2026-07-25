#include "cvm/runtime.h"
#include "cvm/opcode.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CvmFrame {
    uint32_t function_index;
    uint32_t return_instruction;
    uint32_t caller_stack_base;
    uint8_t *storage;
    uint32_t storage_size;
} CvmFrame;

struct CvmModule {
    uint8_t *package;
    size_t package_size;
    CvmPackageHeader header;

    const char *strings;
    uint32_t strings_size;
    const uint8_t *constant_data;
    uint32_t constant_data_size;
    const CvmFunction *functions;
    uint32_t function_count;
    const CvmParameter *parameters;
    uint32_t parameter_count;
    const CvmInstruction *code;
    uint32_t instruction_count;
    const CvmNativeImport *imports;
    uint32_t import_count;
    const CvmNativeSignature *signatures;
    uint32_t signature_count;
    const uint8_t *signature_parameters;
    uint32_t signature_parameter_count;

    uint8_t *globals;
    CvmLimits limits;
};

typedef struct CvmExecution {
    CvmModule *module;
    const CvmHost *host;
    CvmValue *stack;
    uint32_t stack_size;
    uint32_t stack_capacity;
    CvmFrame *frames;
    uint32_t frame_count;
    uint32_t frame_capacity;
    uint32_t instruction;
    uint64_t instructions_left;
} CvmExecution;

static void diagnostic_set(
    CvmDiagnostic *diagnostic,
    CvmStatus status,
    uint32_t function_index,
    uint32_t instruction_index,
    const char *format,
    ...)
{
    va_list arguments;

    if (diagnostic == NULL)
        return;

    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = status;
    diagnostic->function_index = function_index;
    diagnostic->instruction_index = instruction_index;

    va_start(arguments, format);
    (void)vsnprintf(
        diagnostic->message,
        sizeof(diagnostic->message),
        format,
        arguments);
    va_end(arguments);
}

static int checked_range(size_t total, uint32_t offset, uint32_t size)
{
    size_t end = (size_t)offset + (size_t)size;
    return end >= offset && end <= total;
}

static const CvmSectionHeader *find_section(
    const CvmModule *module,
    CvmSectionKind kind)
{
    const CvmSectionHeader *sections =
        (const CvmSectionHeader *)(module->package + sizeof(CvmPackageHeader));
    uint32_t index;

    for (index = 0; index < module->header.section_count; ++index) {
        if (sections[index].kind == (uint16_t)kind)
            return &sections[index];
    }
    return NULL;
}

static int valid_value_type(uint8_t type)
{
    return type <= CVM_TYPE_SIZE;
}

static uint32_t value_type_size(const CvmModule *module, CvmValueType type)
{
    switch (type) {
    case CVM_TYPE_I8:
    case CVM_TYPE_U8:
        return 1;
    case CVM_TYPE_I16:
    case CVM_TYPE_U16:
        return 2;
    case CVM_TYPE_I32:
    case CVM_TYPE_U32:
    case CVM_TYPE_F32:
        return 4;
    case CVM_TYPE_I64:
    case CVM_TYPE_U64:
    case CVM_TYPE_F64:
        return 8;
    case CVM_TYPE_PTR:
    case CVM_TYPE_CSTR:
    case CVM_TYPE_SIZE:
        return module->header.pointer_size;
    default:
        return 0;
    }
}

static int string_valid(const CvmModule *module, uint32_t offset)
{
    if (offset >= module->strings_size)
        return 0;
    return memchr(
        module->strings + offset,
        '\0',
        module->strings_size - offset) != NULL;
}

static CvmStatus bind_section(
    CvmModule *module,
    CvmSectionKind kind,
    uint32_t expected_entry_size,
    const void **data,
    uint32_t *count,
    uint32_t *size,
    CvmDiagnostic *diagnostic)
{
    const CvmSectionHeader *section = find_section(module, kind);

    if (section == NULL) {
        *data = NULL;
        if (count != NULL)
            *count = 0;
        if (size != NULL)
            *size = 0;
        return CVM_STATUS_OK;
    }

    if (!checked_range(module->package_size, section->offset, section->size)) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_INVALID_PACKAGE,
            CVM_NO_INDEX,
            CVM_NO_INDEX,
            "section %u is outside the package",
            (unsigned)kind);
        return CVM_STATUS_INVALID_PACKAGE;
    }

    if (expected_entry_size != 0) {
        if (section->entry_size != expected_entry_size ||
            (uint64_t)section->count * section->entry_size != section->size) {
            diagnostic_set(
                diagnostic,
                CVM_STATUS_INVALID_PACKAGE,
                CVM_NO_INDEX,
                CVM_NO_INDEX,
                "section %u has an invalid entry layout",
                (unsigned)kind);
            return CVM_STATUS_INVALID_PACKAGE;
        }
    }

    *data = module->package + section->offset;
    if (count != NULL)
        *count = section->count;
    if (size != NULL)
        *size = section->size;
    return CVM_STATUS_OK;
}

static int function_contains(
    const CvmFunction *function,
    uint32_t instruction)
{
    uint64_t end =
        (uint64_t)function->first_instruction + function->instruction_count;
    return instruction >= function->first_instruction && instruction < end;
}

static int opcode_stack_effect(
    const CvmModule *module,
    const CvmInstruction *instruction,
    int32_t *required,
    int32_t *delta,
    int *terminal)
{
    CvmOpcode opcode = (CvmOpcode)instruction->opcode;
    *required = 0;
    *delta = 0;
    *terminal = 0;
    switch (opcode) {
    case CVM_OP_NOP:
    case CVM_OP_JUMP:
        return 1;
    case CVM_OP_HALT:
        *terminal = 1;
        return 1;
    case CVM_OP_PUSH_IMMEDIATE:
    case CVM_OP_PUSH_CONSTANT_ADDRESS:
    case CVM_OP_ADDRESS_ARGUMENT:
    case CVM_OP_ADDRESS_LOCAL:
    case CVM_OP_ADDRESS_GLOBAL:
    case CVM_OP_ADDRESS_DATA:
        *delta = 1;
        return 1;
    case CVM_OP_POP:
        *required = 1;
        *delta = -1;
        return 1;
    case CVM_OP_DUP:
        *required = 1;
        *delta = 1;
        return 1;
    case CVM_OP_SWAP:
        *required = 2;
        return 1;
    case CVM_OP_POINTER_ADD:
    case CVM_OP_POINTER_INDEX:
    case CVM_OP_STORE:
    case CVM_OP_COPY_BYTES:
    case CVM_OP_ADD:
    case CVM_OP_SUBTRACT:
    case CVM_OP_MULTIPLY:
    case CVM_OP_DIVIDE_SIGNED:
    case CVM_OP_DIVIDE_UNSIGNED:
    case CVM_OP_MODULO_SIGNED:
    case CVM_OP_MODULO_UNSIGNED:
    case CVM_OP_BIT_AND:
    case CVM_OP_BIT_OR:
    case CVM_OP_BIT_XOR:
    case CVM_OP_LOGICAL_AND:
    case CVM_OP_LOGICAL_OR:
    case CVM_OP_SHIFT_LEFT:
    case CVM_OP_SHIFT_RIGHT_SIGNED:
    case CVM_OP_SHIFT_RIGHT_UNSIGNED:
    case CVM_OP_COMPARE_EQUAL:
    case CVM_OP_COMPARE_NOT_EQUAL:
    case CVM_OP_COMPARE_LESS_SIGNED:
    case CVM_OP_COMPARE_LESS_UNSIGNED:
    case CVM_OP_COMPARE_LESS_EQUAL_SIGNED:
    case CVM_OP_COMPARE_LESS_EQUAL_UNSIGNED:
    case CVM_OP_COMPARE_GREATER_SIGNED:
    case CVM_OP_COMPARE_GREATER_UNSIGNED:
    case CVM_OP_COMPARE_GREATER_EQUAL_SIGNED:
    case CVM_OP_COMPARE_GREATER_EQUAL_UNSIGNED:
        *required = 2;
        *delta = -1;
        return 1;
    case CVM_OP_LOAD:
    case CVM_OP_NEGATE:
    case CVM_OP_BIT_NOT:
    case CVM_OP_CONVERT:
        *required = 1;
        return 1;
    case CVM_OP_JUMP_IF_ZERO:
    case CVM_OP_JUMP_IF_NONZERO:
        *required = 1;
        *delta = -1;
        return 1;
    case CVM_OP_CALL:
        if (instruction->a < 0 ||
            (uint32_t)instruction->a >= module->function_count ||
            instruction->b < 0)
            return 0;
        *required = instruction->b;
        *delta = -instruction->b +
            (instruction->type == CVM_TYPE_VOID ? 0 : 1);
        return 1;
    case CVM_OP_RETURN:
        *required =
            instruction->type == CVM_TYPE_VOID ? 0 : 1;
        *delta = -*required;
        *terminal = 1;
        return 1;
    case CVM_OP_CALL_IMPORT:
        if (instruction->a < 0 ||
            (uint32_t)instruction->a >= module->import_count)
            return 0;
        {
            const CvmNativeImport *import =
                &module->imports[(uint32_t)instruction->a];
            const CvmNativeSignature *signature;
            if (import->signature_index >= module->signature_count)
                return 0;
            signature =
                &module->signatures[import->signature_index];
            *required = signature->parameter_count;
            *delta = -*required +
                (signature->return_type == CVM_TYPE_VOID ? 0 : 1);
        }
        return 1;
    case CVM_OP_CALL_NATIVE_INDIRECT:
        /* Reserved until the native ABI adapter is implemented. */
        return 0;
    default:
        return 0;
    }
}

static CvmStatus verify_function_stack(
    CvmModule *module,
    uint32_t function_index,
    CvmDiagnostic *diagnostic)
{
    const CvmFunction *function = &module->functions[function_index];
    int32_t *depths;
    uint32_t *queue;
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t count = function->instruction_count;
    uint32_t index;

    if (count == 0)
        return CVM_STATUS_VERIFICATION_FAILED;
    depths = (int32_t *)malloc(sizeof(*depths) * count);
    queue = (uint32_t *)malloc(sizeof(*queue) * count);
    if (depths == NULL || queue == NULL) {
        free(depths);
        free(queue);
        return CVM_STATUS_OUT_OF_MEMORY;
    }
    for (index = 0; index < count; ++index)
        depths[index] = INT32_MIN;
    depths[0] = 0;
    queue[tail++] = 0;

    while (head < tail) {
        const uint32_t relative = queue[head++];
        const uint32_t pc = function->first_instruction + relative;
        const CvmInstruction *instruction = &module->code[pc];
        int32_t required;
        int32_t delta;
        int terminal;
        int32_t next_depth;
        uint32_t successors[2];
        uint32_t successor_count = 0;
        uint32_t successor;

        if (!opcode_stack_effect(
                module,
                instruction,
                &required,
                &delta,
                &terminal) ||
            depths[relative] < required) {
            diagnostic_set(
                diagnostic,
                CVM_STATUS_VERIFICATION_FAILED,
                function_index,
                pc,
                "instruction has an invalid operand-stack effect");
            free(depths);
            free(queue);
            return CVM_STATUS_VERIFICATION_FAILED;
        }
        if (instruction->opcode == CVM_OP_RETURN &&
            depths[relative] != required) {
            diagnostic_set(
                diagnostic,
                CVM_STATUS_VERIFICATION_FAILED,
                function_index,
                pc,
                "return leaves extra operand-stack values");
            free(depths);
            free(queue);
            return CVM_STATUS_VERIFICATION_FAILED;
        }
        next_depth = depths[relative] + delta;
        if (next_depth < 0 ||
            (uint32_t)next_depth > function->maximum_stack_cells) {
            diagnostic_set(
                diagnostic,
                CVM_STATUS_VERIFICATION_FAILED,
                function_index,
                pc,
                "function operand-stack bound is invalid");
            free(depths);
            free(queue);
            return CVM_STATUS_VERIFICATION_FAILED;
        }

        if (!terminal) {
            if (instruction->opcode == CVM_OP_JUMP) {
                successors[successor_count++] =
                    (uint32_t)instruction->a -
                    function->first_instruction;
            } else if (
                instruction->opcode == CVM_OP_JUMP_IF_ZERO ||
                instruction->opcode == CVM_OP_JUMP_IF_NONZERO) {
                successors[successor_count++] =
                    (uint32_t)instruction->a -
                    function->first_instruction;
                if (relative + 1 < count)
                    successors[successor_count++] = relative + 1;
                else {
                    diagnostic_set(
                        diagnostic,
                        CVM_STATUS_VERIFICATION_FAILED,
                        function_index,
                        pc,
                        "conditional branch falls out of function");
                    free(depths);
                    free(queue);
                    return CVM_STATUS_VERIFICATION_FAILED;
                }
            } else if (relative + 1 < count) {
                successors[successor_count++] = relative + 1;
            } else {
                diagnostic_set(
                    diagnostic,
                    CVM_STATUS_VERIFICATION_FAILED,
                    function_index,
                    pc,
                    "function falls through without a return");
                free(depths);
                free(queue);
                return CVM_STATUS_VERIFICATION_FAILED;
            }
        }

        for (successor = 0; successor < successor_count; ++successor) {
            const uint32_t target = successors[successor];
            if (target >= count) {
                diagnostic_set(
                    diagnostic,
                    CVM_STATUS_VERIFICATION_FAILED,
                    function_index,
                    pc,
                    "control-flow successor is invalid");
                free(depths);
                free(queue);
                return CVM_STATUS_VERIFICATION_FAILED;
            }
            if (depths[target] == INT32_MIN) {
                depths[target] = next_depth;
                queue[tail++] = target;
            } else if (depths[target] != next_depth) {
                diagnostic_set(
                    diagnostic,
                    CVM_STATUS_VERIFICATION_FAILED,
                    function_index,
                    function->first_instruction + target,
                    "control-flow join has inconsistent stack depth");
                free(depths);
                free(queue);
                return CVM_STATUS_VERIFICATION_FAILED;
            }
        }
    }

    free(depths);
    free(queue);
    return CVM_STATUS_OK;
}

static CvmStatus verify_module(
    CvmModule *module,
    CvmDiagnostic *diagnostic)
{
    uint32_t index;

    if (module->function_count == 0 ||
        module->header.entry_function >= module->function_count) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_VERIFICATION_FAILED,
            CVM_NO_INDEX,
            CVM_NO_INDEX,
            "entry function is missing or invalid");
        return CVM_STATUS_VERIFICATION_FAILED;
    }

    for (index = 0; index < module->function_count; ++index) {
        const CvmFunction *function = &module->functions[index];
        uint64_t code_end =
            (uint64_t)function->first_instruction + function->instruction_count;
        uint64_t parameter_end =
            (uint64_t)function->first_parameter + function->parameter_count;

        if (code_end > module->instruction_count ||
            parameter_end > module->parameter_count ||
            !valid_value_type(function->return_type) ||
            !string_valid(module, function->name_string) ||
            function->local_bytes > module->limits.maximum_local_bytes ||
            function->maximum_stack_cells >
                module->limits.maximum_stack_cells) {
            diagnostic_set(
                diagnostic,
                CVM_STATUS_VERIFICATION_FAILED,
                index,
                CVM_NO_INDEX,
                "function descriptor is invalid");
            return CVM_STATUS_VERIFICATION_FAILED;
        }
        {
            uint32_t parameter;
            for (parameter = 0;
                 parameter < function->parameter_count;
                 ++parameter) {
                const CvmParameter *descriptor =
                    &module->parameters[
                        function->first_parameter + parameter];
                const uint32_t size = value_type_size(
                    module,
                    (CvmValueType)descriptor->value_type);
                if (!valid_value_type(descriptor->value_type) ||
                    descriptor->value_type == CVM_TYPE_VOID ||
                    !string_valid(
                        module,
                        descriptor->name_string) ||
                    size == 0 ||
                    descriptor->frame_offset >
                        function->local_bytes ||
                    size > function->local_bytes -
                        descriptor->frame_offset) {
                    diagnostic_set(
                        diagnostic,
                        CVM_STATUS_VERIFICATION_FAILED,
                        index,
                        CVM_NO_INDEX,
                        "function parameter descriptor is invalid");
                    return CVM_STATUS_VERIFICATION_FAILED;
                }
            }
        }
    }

    for (index = 0; index < module->instruction_count; ++index) {
        const CvmInstruction *instruction = &module->code[index];

        if (instruction->opcode >= CVM_OP_COUNT ||
            !valid_value_type(instruction->type)) {
            diagnostic_set(
                diagnostic,
                CVM_STATUS_VERIFICATION_FAILED,
                CVM_NO_INDEX,
                index,
                "unknown opcode or value type");
            return CVM_STATUS_VERIFICATION_FAILED;
        }

        if (instruction->opcode == CVM_OP_CALL &&
            ((uint32_t)instruction->a >= module->function_count ||
             instruction->b < 0)) {
            diagnostic_set(
                diagnostic,
                CVM_STATUS_VERIFICATION_FAILED,
                CVM_NO_INDEX,
                index,
                "invalid call target");
            return CVM_STATUS_VERIFICATION_FAILED;
        }
        if (instruction->opcode == CVM_OP_CALL) {
            const CvmFunction *target =
                &module->functions[(uint32_t)instruction->a];
            if ((uint32_t)instruction->b !=
                    target->parameter_count ||
                instruction->type != target->return_type) {
                diagnostic_set(
                    diagnostic,
                    CVM_STATUS_VERIFICATION_FAILED,
                    CVM_NO_INDEX,
                    index,
                    "call instruction does not match target signature");
                return CVM_STATUS_VERIFICATION_FAILED;
            }
        }

        if (instruction->opcode == CVM_OP_CALL_IMPORT &&
            (uint32_t)instruction->a >= module->import_count) {
            diagnostic_set(
                diagnostic,
                CVM_STATUS_VERIFICATION_FAILED,
                CVM_NO_INDEX,
                index,
                "invalid import index");
            return CVM_STATUS_VERIFICATION_FAILED;
        }
        if (instruction->opcode == CVM_OP_CALL_IMPORT &&
            (uint32_t)instruction->a < module->import_count) {
            const CvmNativeImport *import =
                &module->imports[(uint32_t)instruction->a];
            if (import->signature_index < module->signature_count) {
                const CvmNativeSignature *signature =
                    &module->signatures[import->signature_index];
                if (instruction->b < 0 ||
                    (uint32_t)instruction->b !=
                        signature->parameter_count ||
                    instruction->type != signature->return_type) {
                    diagnostic_set(
                        diagnostic,
                        CVM_STATUS_VERIFICATION_FAILED,
                        CVM_NO_INDEX,
                        index,
                        "import call does not match signature");
                    return CVM_STATUS_VERIFICATION_FAILED;
                }
            }
        }
    }

    for (index = 0; index < module->import_count; ++index) {
        const CvmNativeImport *import = &module->imports[index];
        if (!string_valid(module, import->library_string) ||
            !string_valid(module, import->symbol_string) ||
            import->signature_index >= module->signature_count) {
            diagnostic_set(
                diagnostic,
                CVM_STATUS_VERIFICATION_FAILED,
                CVM_NO_INDEX,
                CVM_NO_INDEX,
                "native import descriptor is invalid");
            return CVM_STATUS_VERIFICATION_FAILED;
        }
    }

    for (index = 0; index < module->signature_count; ++index) {
        const CvmNativeSignature *signature = &module->signatures[index];
        uint64_t end =
            (uint64_t)signature->first_parameter_type +
            signature->parameter_count;
        uint32_t parameter;
        if (end > module->signature_parameter_count ||
            !valid_value_type(signature->return_type) ||
            signature->parameter_count >
                module->limits.maximum_native_arguments) {
            diagnostic_set(
                diagnostic,
                CVM_STATUS_VERIFICATION_FAILED,
                CVM_NO_INDEX,
                CVM_NO_INDEX,
                "native signature descriptor is invalid");
            return CVM_STATUS_VERIFICATION_FAILED;
        }
        for (parameter = 0; parameter < signature->parameter_count;
             ++parameter) {
            if (!valid_value_type(
                    module->signature_parameters[
                        signature->first_parameter_type + parameter])) {
                diagnostic_set(
                    diagnostic,
                    CVM_STATUS_VERIFICATION_FAILED,
                    CVM_NO_INDEX,
                    CVM_NO_INDEX,
                    "native signature parameter type is invalid");
                return CVM_STATUS_VERIFICATION_FAILED;
            }
        }
    }

    for (index = 0; index < module->function_count; ++index) {
        const CvmFunction *function = &module->functions[index];
        uint32_t pc;
        for (pc = function->first_instruction;
             pc < function->first_instruction + function->instruction_count;
             ++pc) {
            const CvmInstruction *instruction = &module->code[pc];
            if ((instruction->opcode == CVM_OP_JUMP ||
                 instruction->opcode == CVM_OP_JUMP_IF_ZERO ||
                 instruction->opcode == CVM_OP_JUMP_IF_NONZERO) &&
                (instruction->a < 0 ||
                 !function_contains(function, (uint32_t)instruction->a))) {
                diagnostic_set(
                    diagnostic,
                    CVM_STATUS_VERIFICATION_FAILED,
                    index,
                    pc,
                    "branch target leaves its function");
                return CVM_STATUS_VERIFICATION_FAILED;
            }
            if (instruction->opcode == CVM_OP_RETURN &&
                instruction->type != function->return_type) {
                diagnostic_set(
                    diagnostic,
                    CVM_STATUS_VERIFICATION_FAILED,
                    index,
                    pc,
                    "return type does not match function");
                return CVM_STATUS_VERIFICATION_FAILED;
            }
            if (instruction->opcode == CVM_OP_ADDRESS_ARGUMENT ||
                instruction->opcode == CVM_OP_ADDRESS_LOCAL) {
                if (instruction->a < 0 ||
                    (uint32_t)instruction->a >=
                        function->local_bytes) {
                    diagnostic_set(
                        diagnostic,
                        CVM_STATUS_VERIFICATION_FAILED,
                        index,
                        pc,
                        "local address leaves the function frame");
                    return CVM_STATUS_VERIFICATION_FAILED;
                }
            } else if (
                instruction->opcode == CVM_OP_ADDRESS_GLOBAL) {
                if (instruction->a < 0 ||
                    (uint32_t)instruction->a >=
                        module->header.global_bytes) {
                    diagnostic_set(
                        diagnostic,
                        CVM_STATUS_VERIFICATION_FAILED,
                        index,
                        pc,
                        "global address leaves global storage");
                    return CVM_STATUS_VERIFICATION_FAILED;
                }
            } else if (
                instruction->opcode == CVM_OP_ADDRESS_DATA ||
                instruction->opcode ==
                    CVM_OP_PUSH_CONSTANT_ADDRESS) {
                if (instruction->a < 0 ||
                    (uint32_t)instruction->a >
                        module->constant_data_size) {
                    diagnostic_set(
                        diagnostic,
                        CVM_STATUS_VERIFICATION_FAILED,
                        index,
                        pc,
                        "constant-data address is invalid");
                    return CVM_STATUS_VERIFICATION_FAILED;
                }
            }
            if ((instruction->opcode == CVM_OP_LOAD ||
                 instruction->opcode == CVM_OP_STORE) &&
                (instruction->type == CVM_TYPE_VOID ||
                 value_type_size(
                     module,
                     (CvmValueType)instruction->type) == 0)) {
                diagnostic_set(
                    diagnostic,
                    CVM_STATUS_VERIFICATION_FAILED,
                    index,
                    pc,
                    "load/store value type is invalid");
                return CVM_STATUS_VERIFICATION_FAILED;
            }
        }
        {
            const CvmStatus stack_status =
                verify_function_stack(module, index, diagnostic);
            if (stack_status != CVM_STATUS_OK)
                return stack_status;
        }
    }

    return CVM_STATUS_OK;
}

CvmLimits cvm_default_limits(void)
{
    CvmLimits limits;
    limits.instruction_budget = UINT64_C(10000000);
    limits.maximum_stack_cells = 4096;
    limits.maximum_call_depth = 128;
    limits.maximum_local_bytes = 1024 * 1024;
    limits.maximum_global_bytes = 4 * 1024 * 1024;
    limits.maximum_native_arguments = 32;
    return limits;
}

CvmStatus cvm_module_load(
    const void *package_data,
    size_t package_size,
    const CvmLimits *requested_limits,
    CvmModule **module_out,
    CvmDiagnostic *diagnostic)
{
    CvmModule *module;
    const CvmPackageHeader *header;
    const void *data;
    uint64_t directory_size;
    CvmStatus status;
    uint32_t index;

    if (module_out != NULL)
        *module_out = NULL;
    if (package_data == NULL || module_out == NULL ||
        package_size < sizeof(CvmPackageHeader)) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_INVALID_ARGUMENT,
            CVM_NO_INDEX,
            CVM_NO_INDEX,
            "package input is missing or too small");
        return CVM_STATUS_INVALID_ARGUMENT;
    }

    header = (const CvmPackageHeader *)package_data;
    if (header->magic != CVM_MAGIC) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_INVALID_PACKAGE,
            CVM_NO_INDEX,
            CVM_NO_INDEX,
            "package magic does not match");
        return CVM_STATUS_INVALID_PACKAGE;
    }
    if (header->format_major != CVM_FORMAT_MAJOR) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_UNSUPPORTED_VERSION,
            CVM_NO_INDEX,
            CVM_NO_INDEX,
            "unsupported package format %u.%u",
            header->format_major,
            header->format_minor);
        return CVM_STATUS_UNSUPPORTED_VERSION;
    }
    if (header->endian != 1 ||
        (header->pointer_size != 4 && header->pointer_size != 8) ||
        (header->target_arch != CVM_ARCH_X86 &&
         header->target_arch != CVM_ARCH_X64) ||
        (header->target_arch == CVM_ARCH_X86 &&
         header->pointer_size != 4) ||
        (header->target_arch == CVM_ARCH_X64 &&
         header->pointer_size != 8) ||
        (header->profile != CVM_PROFILE_PICOC_COMPAT &&
         header->profile != CVM_PROFILE_BEACON) ||
        header->package_size != package_size) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_INVALID_PACKAGE,
            CVM_NO_INDEX,
            CVM_NO_INDEX,
            "invalid package target or size");
        return CVM_STATUS_INVALID_PACKAGE;
    }
    if (header->pointer_size != sizeof(uintptr_t)) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_ARCHITECTURE_MISMATCH,
            CVM_NO_INDEX,
            CVM_NO_INDEX,
            "package requires a %u-bit VM, host VM is %u-bit",
            (unsigned)header->pointer_size * 8u,
            (unsigned)sizeof(uintptr_t) * 8u);
        return CVM_STATUS_ARCHITECTURE_MISMATCH;
    }

    directory_size = (uint64_t)header->section_count *
        sizeof(CvmSectionHeader);
    if (directory_size >
        package_size - sizeof(CvmPackageHeader)) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_INVALID_PACKAGE,
            CVM_NO_INDEX,
            CVM_NO_INDEX,
            "section directory is truncated");
        return CVM_STATUS_INVALID_PACKAGE;
    }
    {
        const CvmSectionHeader *sections =
            (const CvmSectionHeader *)(
                (const uint8_t *)package_data +
                sizeof(CvmPackageHeader));
        const uint64_t payload_start =
            sizeof(CvmPackageHeader) + directory_size;
        uint32_t left;
        for (left = 0; left < header->section_count; ++left) {
            uint32_t right;
            const uint64_t left_end =
                (uint64_t)sections[left].offset +
                sections[left].size;
            if ((sections[left].size != 0 &&
                 sections[left].offset < payload_start) ||
                left_end > package_size) {
                diagnostic_set(
                    diagnostic,
                    CVM_STATUS_INVALID_PACKAGE,
                    CVM_NO_INDEX,
                    CVM_NO_INDEX,
                    "section %u has an invalid payload range",
                    sections[left].kind);
                return CVM_STATUS_INVALID_PACKAGE;
            }
            for (right = 0; right < left; ++right) {
                const uint64_t right_end =
                    (uint64_t)sections[right].offset +
                    sections[right].size;
                if (sections[left].kind != CVM_SECTION_NONE &&
                    sections[left].kind == sections[right].kind) {
                    diagnostic_set(
                        diagnostic,
                        CVM_STATUS_INVALID_PACKAGE,
                        CVM_NO_INDEX,
                        CVM_NO_INDEX,
                        "duplicate section kind %u",
                        sections[left].kind);
                    return CVM_STATUS_INVALID_PACKAGE;
                }
                if (sections[left].size != 0 &&
                    sections[right].size != 0 &&
                    sections[left].offset < right_end &&
                    sections[right].offset < left_end) {
                    diagnostic_set(
                        diagnostic,
                        CVM_STATUS_INVALID_PACKAGE,
                        CVM_NO_INDEX,
                        CVM_NO_INDEX,
                        "sections %u and %u overlap",
                        sections[right].kind,
                        sections[left].kind);
                    return CVM_STATUS_INVALID_PACKAGE;
                }
            }
        }
    }

    module = (CvmModule *)calloc(1, sizeof(*module));
    if (module == NULL)
        return CVM_STATUS_OUT_OF_MEMORY;

    module->package = (uint8_t *)malloc(package_size);
    if (module->package == NULL) {
        free(module);
        return CVM_STATUS_OUT_OF_MEMORY;
    }
    memcpy(module->package, package_data, package_size);
    module->package_size = package_size;
    memcpy(&module->header, module->package, sizeof(module->header));
    module->limits = requested_limits != NULL
        ? *requested_limits
        : cvm_default_limits();

    if (module->header.global_bytes > module->limits.maximum_global_bytes ||
        module->header.required_stack_cells >
            module->limits.maximum_stack_cells ||
        module->header.required_call_depth >
            module->limits.maximum_call_depth) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_VERIFICATION_FAILED,
            CVM_NO_INDEX,
            CVM_NO_INDEX,
            "package resource requirements exceed runtime limits");
        cvm_module_destroy(module);
        return CVM_STATUS_VERIFICATION_FAILED;
    }

#define BIND(kind, type, field, count_field)                                  \
    do {                                                                       \
        status = bind_section(                                                 \
            module, kind, (uint32_t)sizeof(type), &data,                       \
            &module->count_field, NULL, diagnostic);                           \
        if (status != CVM_STATUS_OK) {                                          \
            cvm_module_destroy(module);                                         \
            return status;                                                      \
        }                                                                       \
        module->field = (const type *)data;                                     \
    } while (0)

    status = bind_section(
        module,
        CVM_SECTION_STRINGS,
        0,
        &data,
        NULL,
        &module->strings_size,
        diagnostic);
    if (status != CVM_STATUS_OK) {
        cvm_module_destroy(module);
        return status;
    }
    module->strings = (const char *)data;

    status = bind_section(
        module,
        CVM_SECTION_DATA,
        0,
        &data,
        NULL,
        &module->constant_data_size,
        diagnostic);
    if (status != CVM_STATUS_OK) {
        cvm_module_destroy(module);
        return status;
    }
    module->constant_data = (const uint8_t *)data;

    BIND(
        CVM_SECTION_FUNCTIONS,
        CvmFunction,
        functions,
        function_count);
    BIND(
        CVM_SECTION_PARAMETERS,
        CvmParameter,
        parameters,
        parameter_count);
    BIND(CVM_SECTION_CODE, CvmInstruction, code, instruction_count);
    BIND(CVM_SECTION_IMPORTS, CvmNativeImport, imports, import_count);
    BIND(
        CVM_SECTION_SIGNATURES,
        CvmNativeSignature,
        signatures,
        signature_count);

#undef BIND

    status = bind_section(
        module,
        CVM_SECTION_SIGNATURE_PARAMETERS,
        sizeof(uint8_t),
        &data,
        &module->signature_parameter_count,
        NULL,
        diagnostic);
    if (status != CVM_STATUS_OK) {
        cvm_module_destroy(module);
        return status;
    }
    module->signature_parameters = (const uint8_t *)data;

    {
        const CvmSectionHeader *sections =
            (const CvmSectionHeader *)(module->package +
                sizeof(CvmPackageHeader));
        for (index = 0; index < module->header.section_count; ++index) {
            if (sections[index].kind > CVM_SECTION_DEBUG &&
                (sections[index].flags & CVM_SECTION_REQUIRED) != 0) {
                diagnostic_set(
                    diagnostic,
                    CVM_STATUS_UNSUPPORTED_VERSION,
                    CVM_NO_INDEX,
                    CVM_NO_INDEX,
                    "unknown required section %u",
                    sections[index].kind);
                cvm_module_destroy(module);
                return CVM_STATUS_UNSUPPORTED_VERSION;
            }
        }
    }

    module->globals = (uint8_t *)calloc(
        module->header.global_bytes == 0 ? 1 : module->header.global_bytes,
        1);
    if (module->globals == NULL) {
        cvm_module_destroy(module);
        return CVM_STATUS_OUT_OF_MEMORY;
    }

    status = verify_module(module, diagnostic);
    if (status != CVM_STATUS_OK) {
        cvm_module_destroy(module);
        return status;
    }

    *module_out = module;
    diagnostic_set(
        diagnostic,
        CVM_STATUS_OK,
        CVM_NO_INDEX,
        CVM_NO_INDEX,
        "ok");
    return CVM_STATUS_OK;
}

void cvm_module_destroy(CvmModule *module)
{
    if (module == NULL)
        return;
    free(module->globals);
    free(module->package);
    free(module);
}

static CvmStatus push(
    CvmExecution *execution,
    CvmValue value,
    CvmDiagnostic *diagnostic)
{
    if (execution->stack_size >= execution->stack_capacity) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_STACK_OVERFLOW,
            execution->frames[execution->frame_count - 1].function_index,
            execution->instruction,
            "operand stack overflow");
        return CVM_STATUS_STACK_OVERFLOW;
    }
    execution->stack[execution->stack_size++] = value;
    return CVM_STATUS_OK;
}

static CvmStatus pop(
    CvmExecution *execution,
    CvmValue *value,
    CvmDiagnostic *diagnostic)
{
    if (execution->stack_size == 0) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_STACK_UNDERFLOW,
            execution->frames[execution->frame_count - 1].function_index,
            execution->instruction,
            "operand stack underflow");
        return CVM_STATUS_STACK_UNDERFLOW;
    }
    *value = execution->stack[--execution->stack_size];
    return CVM_STATUS_OK;
}

static CvmFrame *current_frame(CvmExecution *execution)
{
    return &execution->frames[execution->frame_count - 1];
}

static CvmStatus enter_function(
    CvmExecution *execution,
    uint32_t function_index,
    uint32_t argument_count,
    uint32_t return_instruction,
    CvmDiagnostic *diagnostic)
{
    const CvmFunction *function;
    CvmFrame *frame;
    uint32_t index;

    if (execution->frame_count >= execution->frame_capacity) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_CALL_DEPTH_EXCEEDED,
            function_index,
            execution->instruction,
            "call depth exceeded");
        return CVM_STATUS_CALL_DEPTH_EXCEEDED;
    }
    if (function_index >= execution->module->function_count)
        return CVM_STATUS_RUNTIME_ERROR;

    function = &execution->module->functions[function_index];
    if (argument_count != function->parameter_count ||
        execution->stack_size < argument_count) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_RUNTIME_ERROR,
            function_index,
            execution->instruction,
            "call argument count mismatch");
        return CVM_STATUS_RUNTIME_ERROR;
    }

    frame = &execution->frames[execution->frame_count++];
    memset(frame, 0, sizeof(*frame));
    frame->function_index = function_index;
    frame->return_instruction = return_instruction;
    frame->caller_stack_base = execution->stack_size - argument_count;
    frame->storage_size = function->local_bytes;
    frame->storage = (uint8_t *)calloc(
        function->local_bytes == 0 ? 1 : function->local_bytes,
        1);
    if (frame->storage == NULL) {
        --execution->frame_count;
        return CVM_STATUS_OUT_OF_MEMORY;
    }

    for (index = 0; index < argument_count; ++index) {
        const CvmParameter *parameter =
            &execution->module->parameters[
                function->first_parameter + index];
        uint32_t size = value_type_size(
            execution->module,
            (CvmValueType)parameter->value_type);
        CvmValue value =
            execution->stack[frame->caller_stack_base + index];
        if (size == 0 ||
            parameter->frame_offset > frame->storage_size ||
            size > frame->storage_size - parameter->frame_offset) {
            free(frame->storage);
            --execution->frame_count;
            return CVM_STATUS_RUNTIME_ERROR;
        }
        memcpy(frame->storage + parameter->frame_offset, &value, size);
    }

    execution->stack_size = frame->caller_stack_base;
    execution->instruction = function->first_instruction;
    return CVM_STATUS_OK;
}

static int valid_runtime_range(
    CvmExecution *execution,
    uintptr_t address,
    uint32_t size,
    int write)
{
    uint32_t index;
    uintptr_t start;
    uintptr_t end;

    if (size == 0 || address + size < address)
        return 0;

    start = (uintptr_t)execution->module->globals;
    end = start + execution->module->header.global_bytes;
    if (address >= start && address + size <= end)
        return 1;

    start = (uintptr_t)execution->module->constant_data;
    end = start + execution->module->constant_data_size;
    if (!write && address >= start && address + size <= end)
        return 1;

    for (index = 0; index < execution->frame_count; ++index) {
        start = (uintptr_t)execution->frames[index].storage;
        end = start + execution->frames[index].storage_size;
        if (address >= start && address + size <= end)
            return 1;
    }
    if (execution->host != NULL &&
        execution->host->memory_access != NULL &&
        execution->host->memory_access(
            execution->host->context,
            address,
            size,
            write)) {
        return 1;
    }
    return 0;
}

static uint64_t normalize_integer(uint64_t value, CvmValueType type)
{
    switch (type) {
    case CVM_TYPE_I8:
        return (uint64_t)(int64_t)(int8_t)value;
    case CVM_TYPE_U8:
        return (uint8_t)value;
    case CVM_TYPE_I16:
        return (uint64_t)(int64_t)(int16_t)value;
    case CVM_TYPE_U16:
        return (uint16_t)value;
    case CVM_TYPE_I32:
        return (uint64_t)(int64_t)(int32_t)value;
    case CVM_TYPE_U32:
        return (uint32_t)value;
    default:
        return value;
    }
}

static double floating_value(CvmValue value, CvmValueType type)
{
    if (type == CVM_TYPE_F32) {
        uint32_t bits = (uint32_t)value.u64;
        float converted;
        memcpy(&converted, &bits, sizeof(converted));
        return (double)converted;
    }
    return value.f64;
}

static CvmValue make_floating_value(double value, CvmValueType type)
{
    CvmValue result;
    result.u64 = 0;
    if (type == CVM_TYPE_F32) {
        float converted = (float)value;
        uint32_t bits;
        memcpy(&bits, &converted, sizeof(bits));
        result.u64 = bits;
    } else {
        result.f64 = value;
    }
    return result;
}

static int signed_value_type(CvmValueType type)
{
    return type == CVM_TYPE_I8 ||
           type == CVM_TYPE_I16 ||
           type == CVM_TYPE_I32 ||
           type == CVM_TYPE_I64;
}

static CvmStatus execute_binary(
    CvmExecution *execution,
    const CvmInstruction *instruction,
    CvmDiagnostic *diagnostic)
{
    CvmValue left;
    CvmValue right;
    CvmValue result;
    CvmStatus status;

    status = pop(execution, &right, diagnostic);
    if (status != CVM_STATUS_OK)
        return status;
    status = pop(execution, &left, diagnostic);
    if (status != CVM_STATUS_OK)
        return status;
    result.u64 = 0;

    if (instruction->type == CVM_TYPE_F32 ||
        instruction->type == CVM_TYPE_F64) {
        const double left_value = floating_value(
            left,
            (CvmValueType)instruction->type);
        const double right_value = floating_value(
            right,
            (CvmValueType)instruction->type);
        double arithmetic_result = 0.0;
        int comparison = 0;
        int is_comparison = 0;
        switch ((CvmOpcode)instruction->opcode) {
        case CVM_OP_ADD:
            arithmetic_result = left_value + right_value;
            break;
        case CVM_OP_SUBTRACT:
            arithmetic_result = left_value - right_value;
            break;
        case CVM_OP_MULTIPLY:
            arithmetic_result = left_value * right_value;
            break;
        case CVM_OP_DIVIDE_SIGNED:
        case CVM_OP_DIVIDE_UNSIGNED:
            if (right_value == 0.0)
                return CVM_STATUS_DIVIDE_BY_ZERO;
            arithmetic_result = left_value / right_value;
            break;
        case CVM_OP_COMPARE_EQUAL:
            comparison = left_value == right_value;
            is_comparison = 1;
            break;
        case CVM_OP_COMPARE_NOT_EQUAL:
            comparison = left_value != right_value;
            is_comparison = 1;
            break;
        case CVM_OP_COMPARE_LESS_SIGNED:
        case CVM_OP_COMPARE_LESS_UNSIGNED:
            comparison = left_value < right_value;
            is_comparison = 1;
            break;
        case CVM_OP_COMPARE_LESS_EQUAL_SIGNED:
        case CVM_OP_COMPARE_LESS_EQUAL_UNSIGNED:
            comparison = left_value <= right_value;
            is_comparison = 1;
            break;
        case CVM_OP_COMPARE_GREATER_SIGNED:
        case CVM_OP_COMPARE_GREATER_UNSIGNED:
            comparison = left_value > right_value;
            is_comparison = 1;
            break;
        case CVM_OP_COMPARE_GREATER_EQUAL_SIGNED:
        case CVM_OP_COMPARE_GREATER_EQUAL_UNSIGNED:
            comparison = left_value >= right_value;
            is_comparison = 1;
            break;
        default:
            return CVM_STATUS_RUNTIME_ERROR;
        }
        if (is_comparison) {
            result.u64 = (uint64_t)comparison;
        } else {
            result = make_floating_value(
                arithmetic_result,
                (CvmValueType)instruction->type);
        }
        return push(execution, result, diagnostic);
    }

    switch ((CvmOpcode)instruction->opcode) {
    case CVM_OP_ADD:
        result.u64 = left.u64 + right.u64;
        break;
    case CVM_OP_SUBTRACT:
        result.u64 = left.u64 - right.u64;
        break;
    case CVM_OP_MULTIPLY:
        result.u64 = left.u64 * right.u64;
        break;
    case CVM_OP_DIVIDE_SIGNED:
        if (right.i64 == 0)
            return CVM_STATUS_DIVIDE_BY_ZERO;
        result.i64 = left.i64 / right.i64;
        break;
    case CVM_OP_DIVIDE_UNSIGNED:
        if (right.u64 == 0)
            return CVM_STATUS_DIVIDE_BY_ZERO;
        result.u64 = left.u64 / right.u64;
        break;
    case CVM_OP_MODULO_SIGNED:
        if (right.i64 == 0)
            return CVM_STATUS_DIVIDE_BY_ZERO;
        result.i64 = left.i64 % right.i64;
        break;
    case CVM_OP_MODULO_UNSIGNED:
        if (right.u64 == 0)
            return CVM_STATUS_DIVIDE_BY_ZERO;
        result.u64 = left.u64 % right.u64;
        break;
    case CVM_OP_BIT_AND:
        result.u64 = left.u64 & right.u64;
        break;
    case CVM_OP_BIT_OR:
        result.u64 = left.u64 | right.u64;
        break;
    case CVM_OP_BIT_XOR:
        result.u64 = left.u64 ^ right.u64;
        break;
    case CVM_OP_LOGICAL_AND:
        result.u64 = left.u64 != 0 && right.u64 != 0;
        break;
    case CVM_OP_LOGICAL_OR:
        result.u64 = left.u64 != 0 || right.u64 != 0;
        break;
    case CVM_OP_SHIFT_LEFT:
        result.u64 = left.u64 << (right.u64 & 63u);
        break;
    case CVM_OP_SHIFT_RIGHT_SIGNED:
        result.i64 = left.i64 >> (right.u64 & 63u);
        break;
    case CVM_OP_SHIFT_RIGHT_UNSIGNED:
        result.u64 = left.u64 >> (right.u64 & 63u);
        break;
    case CVM_OP_COMPARE_EQUAL:
        result.u64 = left.u64 == right.u64;
        break;
    case CVM_OP_COMPARE_NOT_EQUAL:
        result.u64 = left.u64 != right.u64;
        break;
    case CVM_OP_COMPARE_LESS_SIGNED:
        result.u64 = left.i64 < right.i64;
        break;
    case CVM_OP_COMPARE_LESS_UNSIGNED:
        result.u64 = left.u64 < right.u64;
        break;
    case CVM_OP_COMPARE_LESS_EQUAL_SIGNED:
        result.u64 = left.i64 <= right.i64;
        break;
    case CVM_OP_COMPARE_LESS_EQUAL_UNSIGNED:
        result.u64 = left.u64 <= right.u64;
        break;
    case CVM_OP_COMPARE_GREATER_SIGNED:
        result.u64 = left.i64 > right.i64;
        break;
    case CVM_OP_COMPARE_GREATER_UNSIGNED:
        result.u64 = left.u64 > right.u64;
        break;
    case CVM_OP_COMPARE_GREATER_EQUAL_SIGNED:
        result.u64 = left.i64 >= right.i64;
        break;
    case CVM_OP_COMPARE_GREATER_EQUAL_UNSIGNED:
        result.u64 = left.u64 >= right.u64;
        break;
    default:
        return CVM_STATUS_RUNTIME_ERROR;
    }

    result.u64 = normalize_integer(
        result.u64,
        (CvmValueType)instruction->type);
    return push(execution, result, diagnostic);
}

static CvmStatus run(
    CvmExecution *execution,
    CvmValue *return_value,
    CvmDiagnostic *diagnostic)
{
    CvmModule *module = execution->module;

    for (;;) {
        const CvmInstruction *instruction;
        CvmOpcode opcode;
        CvmValue value;
        CvmValue other;
        CvmStatus status;
        CvmFrame *frame;
        uintptr_t address;
        uint32_t size;

        if (execution->instructions_left-- == 0)
            return CVM_STATUS_INSTRUCTION_LIMIT;
        if (execution->instruction >= module->instruction_count)
            return CVM_STATUS_RUNTIME_ERROR;

        instruction = &module->code[execution->instruction++];
        opcode = (CvmOpcode)instruction->opcode;

        switch (opcode) {
        case CVM_OP_NOP:
            break;
        case CVM_OP_HALT:
            if (return_value != NULL) {
                return_value->u64 =
                    execution->stack_size == 0
                        ? 0
                        : execution->stack[execution->stack_size - 1].u64;
            }
            return CVM_STATUS_OK;
        case CVM_OP_PUSH_IMMEDIATE:
            value.u64 =
                (uint64_t)(uint32_t)instruction->a |
                ((uint64_t)(uint32_t)instruction->b << 32);
            value.u64 = normalize_integer(
                value.u64,
                (CvmValueType)instruction->type);
            status = push(execution, value, diagnostic);
            if (status != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_POP:
            status = pop(execution, &value, diagnostic);
            if (status != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_DUP:
            status = pop(execution, &value, diagnostic);
            if (status != CVM_STATUS_OK)
                return status;
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK ||
                (status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_SWAP:
            if ((status = pop(execution, &value, diagnostic)) != CVM_STATUS_OK ||
                (status = pop(execution, &other, diagnostic)) != CVM_STATUS_OK)
                return status;
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK ||
                (status = push(execution, other, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_ADDRESS_ARGUMENT:
        case CVM_OP_ADDRESS_LOCAL:
            frame = current_frame(execution);
            if (instruction->a < 0 ||
                (uint32_t)instruction->a > frame->storage_size) {
                return CVM_STATUS_MEMORY_FAULT;
            }
            value.pointer =
                (uintptr_t)(frame->storage + (uint32_t)instruction->a);
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_ADDRESS_GLOBAL:
            if (instruction->a < 0 ||
                (uint32_t)instruction->a > module->header.global_bytes) {
                return CVM_STATUS_MEMORY_FAULT;
            }
            value.pointer =
                (uintptr_t)(module->globals + (uint32_t)instruction->a);
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_ADDRESS_DATA:
        case CVM_OP_PUSH_CONSTANT_ADDRESS:
            if (instruction->a < 0 ||
                (uint32_t)instruction->a > module->constant_data_size) {
                return CVM_STATUS_MEMORY_FAULT;
            }
            value.pointer =
                (uintptr_t)(module->constant_data +
                    (uint32_t)instruction->a);
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_POINTER_ADD:
            if ((status = pop(execution, &other, diagnostic)) != CVM_STATUS_OK ||
                (status = pop(execution, &value, diagnostic)) != CVM_STATUS_OK)
                return status;
            value.pointer += (uintptr_t)other.i64;
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_POINTER_INDEX:
            if ((status = pop(execution, &other, diagnostic)) != CVM_STATUS_OK ||
                (status = pop(execution, &value, diagnostic)) != CVM_STATUS_OK)
                return status;
            value.pointer +=
                (uintptr_t)(other.i64 * (int64_t)instruction->a);
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_LOAD:
            if ((status = pop(execution, &value, diagnostic)) != CVM_STATUS_OK)
                return status;
            size = value_type_size(module, (CvmValueType)instruction->type);
            if (!valid_runtime_range(execution, value.pointer, size, 0))
                return CVM_STATUS_MEMORY_FAULT;
            address = value.pointer;
            value.u64 = 0;
            memcpy(&value.u64, (const void *)address, size);
            value.u64 = normalize_integer(
                value.u64,
                (CvmValueType)instruction->type);
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_STORE:
            if ((status = pop(execution, &value, diagnostic)) != CVM_STATUS_OK ||
                (status = pop(execution, &other, diagnostic)) != CVM_STATUS_OK)
                return status;
            size = value_type_size(module, (CvmValueType)instruction->type);
            if (!valid_runtime_range(execution, other.pointer, size, 1))
                return CVM_STATUS_MEMORY_FAULT;
            memcpy((void *)other.pointer, &value.u64, size);
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_COPY_BYTES:
            if ((status = pop(execution, &value, diagnostic)) != CVM_STATUS_OK ||
                (status = pop(execution, &other, diagnostic)) != CVM_STATUS_OK)
                return status;
            if (instruction->a <= 0 ||
                !valid_runtime_range(
                    execution,
                    value.pointer,
                    (uint32_t)instruction->a,
                    0) ||
                !valid_runtime_range(
                    execution,
                    other.pointer,
                    (uint32_t)instruction->a,
                    1)) {
                return CVM_STATUS_MEMORY_FAULT;
            }
            memmove(
                (void *)other.pointer,
                (const void *)value.pointer,
                (size_t)instruction->a);
            if ((status = push(execution, other, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_ADD:
        case CVM_OP_SUBTRACT:
        case CVM_OP_MULTIPLY:
        case CVM_OP_DIVIDE_SIGNED:
        case CVM_OP_DIVIDE_UNSIGNED:
        case CVM_OP_MODULO_SIGNED:
        case CVM_OP_MODULO_UNSIGNED:
        case CVM_OP_BIT_AND:
        case CVM_OP_BIT_OR:
        case CVM_OP_BIT_XOR:
        case CVM_OP_LOGICAL_AND:
        case CVM_OP_LOGICAL_OR:
        case CVM_OP_SHIFT_LEFT:
        case CVM_OP_SHIFT_RIGHT_SIGNED:
        case CVM_OP_SHIFT_RIGHT_UNSIGNED:
        case CVM_OP_COMPARE_EQUAL:
        case CVM_OP_COMPARE_NOT_EQUAL:
        case CVM_OP_COMPARE_LESS_SIGNED:
        case CVM_OP_COMPARE_LESS_UNSIGNED:
        case CVM_OP_COMPARE_LESS_EQUAL_SIGNED:
        case CVM_OP_COMPARE_LESS_EQUAL_UNSIGNED:
        case CVM_OP_COMPARE_GREATER_SIGNED:
        case CVM_OP_COMPARE_GREATER_UNSIGNED:
        case CVM_OP_COMPARE_GREATER_EQUAL_SIGNED:
        case CVM_OP_COMPARE_GREATER_EQUAL_UNSIGNED:
            status = execute_binary(execution, instruction, diagnostic);
            if (status != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_NEGATE:
            if ((status = pop(execution, &value, diagnostic)) != CVM_STATUS_OK)
                return status;
            if (instruction->type == CVM_TYPE_F32 ||
                instruction->type == CVM_TYPE_F64) {
                value = make_floating_value(
                    -floating_value(
                        value,
                        (CvmValueType)instruction->type),
                    (CvmValueType)instruction->type);
            } else {
                value.i64 = -value.i64;
                value.u64 = normalize_integer(
                    value.u64,
                    (CvmValueType)instruction->type);
            }
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_BIT_NOT:
            if ((status = pop(execution, &value, diagnostic)) != CVM_STATUS_OK)
                return status;
            value.u64 = ~value.u64;
            value.u64 = normalize_integer(
                value.u64,
                (CvmValueType)instruction->type);
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_CONVERT:
            if ((status = pop(execution, &value, diagnostic)) != CVM_STATUS_OK)
                return status;
            {
                const CvmValueType source_type =
                    (CvmValueType)instruction->a;
                const CvmValueType target_type =
                    (CvmValueType)instruction->type;
                if (target_type == CVM_TYPE_F32 ||
                    target_type == CVM_TYPE_F64) {
                    double converted;
                    if (source_type == CVM_TYPE_F32 ||
                        source_type == CVM_TYPE_F64) {
                        converted = floating_value(value, source_type);
                    } else if (signed_value_type(source_type)) {
                        converted = (double)value.i64;
                    } else {
                        converted = (double)value.u64;
                    }
                    value = make_floating_value(converted, target_type);
                } else if (source_type == CVM_TYPE_F32 ||
                           source_type == CVM_TYPE_F64) {
                    const double converted =
                        floating_value(value, source_type);
                    if (signed_value_type(target_type))
                        value.i64 = (int64_t)converted;
                    else
                        value.u64 = (uint64_t)converted;
                    value.u64 =
                        normalize_integer(value.u64, target_type);
                } else {
                    value.u64 =
                        normalize_integer(value.u64, target_type);
                }
            }
            if ((status = push(execution, value, diagnostic)) != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_JUMP:
            execution->instruction = (uint32_t)instruction->a;
            break;
        case CVM_OP_JUMP_IF_ZERO:
        case CVM_OP_JUMP_IF_NONZERO:
            if ((status = pop(execution, &value, diagnostic)) != CVM_STATUS_OK)
                return status;
            if ((opcode == CVM_OP_JUMP_IF_ZERO && value.u64 == 0) ||
                (opcode == CVM_OP_JUMP_IF_NONZERO && value.u64 != 0)) {
                execution->instruction = (uint32_t)instruction->a;
            }
            break;
        case CVM_OP_CALL:
            status = enter_function(
                execution,
                (uint32_t)instruction->a,
                (uint32_t)instruction->b,
                execution->instruction,
                diagnostic);
            if (status != CVM_STATUS_OK)
                return status;
            break;
        case CVM_OP_RETURN:
            frame = current_frame(execution);
            value.u64 = 0;
            if (instruction->type != CVM_TYPE_VOID) {
                status = pop(execution, &value, diagnostic);
                if (status != CVM_STATUS_OK)
                    return status;
            }
            execution->stack_size = frame->caller_stack_base;
            execution->instruction = frame->return_instruction;
            free(frame->storage);
            --execution->frame_count;
            if (execution->frame_count == 0) {
                if (return_value != NULL)
                    *return_value = value;
                return CVM_STATUS_OK;
            }
            if (instruction->type != CVM_TYPE_VOID &&
                (status = push(execution, value, diagnostic)) != CVM_STATUS_OK) {
                return status;
            }
            break;
        case CVM_OP_CALL_IMPORT:
        {
            const CvmNativeImport *import;
            const CvmNativeSignature *signature;
            CvmValue arguments[64];
            CvmValueType parameterTypes[64];
            CvmNativeCall call;
            uint32_t argument;

            if (execution->host == NULL || execution->host->call == NULL)
                return CVM_STATUS_UNRESOLVED_IMPORT;
            import = &module->imports[(uint32_t)instruction->a];
            signature = &module->signatures[import->signature_index];
            if (signature->parameter_count > 64)
                return CVM_STATUS_RUNTIME_ERROR;
            for (argument = signature->parameter_count;
                 argument > 0;
                 --argument) {
                status = pop(
                    execution,
                    &arguments[argument - 1],
                    diagnostic);
                if (status != CVM_STATUS_OK)
                    return status;
            }
            for (argument = 0;
                 argument < signature->parameter_count;
                 ++argument) {
                parameterTypes[argument] = (CvmValueType)
                    module->signature_parameters[
                        signature->first_parameter_type + argument];
            }
            memset(&call, 0, sizeof(call));
            call.library = module->strings + import->library_string;
            call.symbol = module->strings + import->symbol_string;
            call.calling_convention =
                (CvmCallingConvention)signature->calling_convention;
            call.return_type =
                (CvmValueType)signature->return_type;
            call.parameter_types = parameterTypes;
            call.parameter_count = signature->parameter_count;
            call.arguments = arguments;
            value.u64 = 0;
            status = execution->host->call(
                execution->host->context,
                &call,
                &value,
                diagnostic);
            if (status != CVM_STATUS_OK)
                return status;
            if (call.return_type != CVM_TYPE_VOID) {
                status = push(execution, value, diagnostic);
                if (status != CVM_STATUS_OK)
                    return status;
            }
            break;
        }
        default:
            diagnostic_set(
                diagnostic,
                CVM_STATUS_RUNTIME_ERROR,
                current_frame(execution)->function_index,
                execution->instruction - 1,
                "opcode %u is not implemented",
                instruction->opcode);
            return CVM_STATUS_RUNTIME_ERROR;
        }
    }
}

CvmStatus cvm_module_execute(
    CvmModule *module,
    const CvmHost *host,
    CvmValue *return_value,
    CvmDiagnostic *diagnostic)
{
    CvmExecution execution;
    CvmStatus status;
    uint32_t index;

    if (module == NULL)
        return CVM_STATUS_INVALID_ARGUMENT;
    if (diagnostic != NULL)
        memset(diagnostic, 0, sizeof(*diagnostic));

    memset(&execution, 0, sizeof(execution));
    execution.module = module;
    execution.host = host;
    execution.stack_capacity = module->limits.maximum_stack_cells;
    execution.frame_capacity = module->limits.maximum_call_depth;
    execution.instructions_left = module->limits.instruction_budget;
    execution.stack = (CvmValue *)calloc(
        execution.stack_capacity,
        sizeof(CvmValue));
    execution.frames = (CvmFrame *)calloc(
        execution.frame_capacity,
        sizeof(CvmFrame));
    if (execution.stack == NULL || execution.frames == NULL) {
        free(execution.stack);
        free(execution.frames);
        return CVM_STATUS_OUT_OF_MEMORY;
    }

    status = enter_function(
        &execution,
        module->header.entry_function,
        0,
        CVM_NO_INDEX,
        diagnostic);
    if (status == CVM_STATUS_OK)
        status = run(&execution, return_value, diagnostic);

    for (index = 0; index < execution.frame_count; ++index)
        free(execution.frames[index].storage);
    free(execution.stack);
    free(execution.frames);

    if (status == CVM_STATUS_OK) {
        diagnostic_set(
            diagnostic,
            CVM_STATUS_OK,
            CVM_NO_INDEX,
            CVM_NO_INDEX,
            "ok");
    } else if (diagnostic != NULL && diagnostic->message[0] == '\0') {
        diagnostic_set(
            diagnostic,
            status,
            CVM_NO_INDEX,
            execution.instruction,
            "%s",
            cvm_status_string(status));
    }
    return status;
}

const char *cvm_status_string(CvmStatus status)
{
    switch (status) {
    case CVM_STATUS_OK: return "ok";
    case CVM_STATUS_INVALID_ARGUMENT: return "invalid argument";
    case CVM_STATUS_INVALID_PACKAGE: return "invalid package";
    case CVM_STATUS_UNSUPPORTED_VERSION: return "unsupported version";
    case CVM_STATUS_ARCHITECTURE_MISMATCH: return "architecture mismatch";
    case CVM_STATUS_VERIFICATION_FAILED: return "verification failed";
    case CVM_STATUS_OUT_OF_MEMORY: return "out of memory";
    case CVM_STATUS_STACK_OVERFLOW: return "stack overflow";
    case CVM_STATUS_STACK_UNDERFLOW: return "stack underflow";
    case CVM_STATUS_CALL_DEPTH_EXCEEDED: return "call depth exceeded";
    case CVM_STATUS_INSTRUCTION_LIMIT: return "instruction limit";
    case CVM_STATUS_DIVIDE_BY_ZERO: return "divide by zero";
    case CVM_STATUS_MEMORY_FAULT: return "memory fault";
    case CVM_STATUS_UNRESOLVED_IMPORT: return "unresolved import";
    case CVM_STATUS_NATIVE_CALL_FAILED: return "native call failed";
    case CVM_STATUS_RUNTIME_ERROR: return "runtime error";
    default: return "unknown status";
    }
}
