#ifndef COBALT_CVM_OPCODE_H
#define COBALT_CVM_OPCODE_H

#include <stdint.h>
#include "cvm/format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CvmOpcode {
    CVM_OP_NOP = 0,
    CVM_OP_HALT,

    CVM_OP_PUSH_IMMEDIATE,
    CVM_OP_PUSH_CONSTANT_ADDRESS,
    CVM_OP_POP,
    CVM_OP_DUP,
    CVM_OP_SWAP,

    CVM_OP_ADDRESS_ARGUMENT,
    CVM_OP_ADDRESS_LOCAL,
    CVM_OP_ADDRESS_GLOBAL,
    CVM_OP_ADDRESS_DATA,
    CVM_OP_POINTER_ADD,
    CVM_OP_POINTER_INDEX,

    CVM_OP_LOAD,
    CVM_OP_STORE,
    CVM_OP_COPY_BYTES,

    CVM_OP_ADD,
    CVM_OP_SUBTRACT,
    CVM_OP_MULTIPLY,
    CVM_OP_DIVIDE_SIGNED,
    CVM_OP_DIVIDE_UNSIGNED,
    CVM_OP_MODULO_SIGNED,
    CVM_OP_MODULO_UNSIGNED,
    CVM_OP_NEGATE,

    CVM_OP_BIT_AND,
    CVM_OP_BIT_OR,
    CVM_OP_BIT_XOR,
    CVM_OP_BIT_NOT,
    CVM_OP_LOGICAL_AND,
    CVM_OP_LOGICAL_OR,
    CVM_OP_SHIFT_LEFT,
    CVM_OP_SHIFT_RIGHT_SIGNED,
    CVM_OP_SHIFT_RIGHT_UNSIGNED,

    CVM_OP_COMPARE_EQUAL,
    CVM_OP_COMPARE_NOT_EQUAL,
    CVM_OP_COMPARE_LESS_SIGNED,
    CVM_OP_COMPARE_LESS_UNSIGNED,
    CVM_OP_COMPARE_LESS_EQUAL_SIGNED,
    CVM_OP_COMPARE_LESS_EQUAL_UNSIGNED,
    CVM_OP_COMPARE_GREATER_SIGNED,
    CVM_OP_COMPARE_GREATER_UNSIGNED,
    CVM_OP_COMPARE_GREATER_EQUAL_SIGNED,
    CVM_OP_COMPARE_GREATER_EQUAL_UNSIGNED,

    CVM_OP_CONVERT,

    CVM_OP_JUMP,
    CVM_OP_JUMP_IF_ZERO,
    CVM_OP_JUMP_IF_NONZERO,

    CVM_OP_CALL,
    CVM_OP_RETURN,
    CVM_OP_CALL_IMPORT,
    CVM_OP_CALL_NATIVE_INDIRECT,

    CVM_OP_COUNT
} CvmOpcode;

/*
 * Instructions deliberately use a fixed representation in format version 1.
 * This makes structural verification deterministic. A later format version
 * may add a compact encoding without changing opcode semantics.
 *
 * `type` is a CvmValueType when the opcode is typed.
 * `a` and `b` are opcode-specific signed operands or the low/high halves of
 * an immediate value.
 */
#pragma pack(push, 1)
typedef struct CvmInstruction {
    uint8_t opcode;
    uint8_t type;
    uint16_t flags;
    int32_t a;
    int32_t b;
} CvmInstruction;
#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(CvmInstruction) == 12, "CvmInstruction layout changed");
#elif defined(_MSC_VER)
typedef char CvmInstruction_size_must_be_12[
    sizeof(CvmInstruction) == 12 ? 1 : -1];
#else
_Static_assert(sizeof(CvmInstruction) == 12, "CvmInstruction layout changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
