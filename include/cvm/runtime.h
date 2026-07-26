#ifndef COBALT_CVM_RUNTIME_H
#define COBALT_CVM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include "cvm/format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef union CvmValue {
    uint64_t u64;
    int64_t i64;
    double f64;
    uintptr_t pointer;
} CvmValue;

#if defined(__cplusplus)
static_assert(sizeof(CvmValue) == 8, "CvmValue cell layout changed");
#elif defined(_MSC_VER)
typedef char CvmValue_size_must_be_8[
    sizeof(CvmValue) == 8 ? 1 : -1];
#else
_Static_assert(sizeof(CvmValue) == 8, "CvmValue cell layout changed");
#endif

typedef enum CvmStatus {
    CVM_STATUS_OK = 0,
    CVM_STATUS_INVALID_ARGUMENT,
    CVM_STATUS_INVALID_PACKAGE,
    CVM_STATUS_UNSUPPORTED_VERSION,
    CVM_STATUS_ARCHITECTURE_MISMATCH,
    CVM_STATUS_VERIFICATION_FAILED,
    CVM_STATUS_OUT_OF_MEMORY,
    CVM_STATUS_STACK_OVERFLOW,
    CVM_STATUS_STACK_UNDERFLOW,
    CVM_STATUS_CALL_DEPTH_EXCEEDED,
    CVM_STATUS_INSTRUCTION_LIMIT,
    CVM_STATUS_DIVIDE_BY_ZERO,
    CVM_STATUS_MEMORY_FAULT,
    CVM_STATUS_UNRESOLVED_IMPORT,
    CVM_STATUS_NATIVE_CALL_FAILED,
    CVM_STATUS_RUNTIME_ERROR
} CvmStatus;

typedef struct CvmLimits {
    uint64_t instruction_budget;
    uint32_t maximum_stack_cells;
    uint32_t maximum_call_depth;
    uint32_t maximum_local_bytes;
    uint32_t maximum_global_bytes;
    uint32_t maximum_native_arguments;
} CvmLimits;

typedef struct CvmDiagnostic {
    CvmStatus status;
    uint32_t function_index;
    uint32_t instruction_index;
    char message[256];
} CvmDiagnostic;

typedef struct CvmNativeCall {
    uintptr_t address;
    const char *library;
    const char *symbol;
    CvmCallingConvention calling_convention;
    CvmValueType return_type;
    const CvmValueType *parameter_types;
    uint32_t parameter_count;
    const CvmValue *arguments;
} CvmNativeCall;

typedef CvmStatus (*CvmHostCall)(
    void *context,
    const CvmNativeCall *call,
    CvmValue *return_value,
    CvmDiagnostic *diagnostic);

/*
 * Native pointers are not implicitly trusted. A host may expose narrowly
 * scoped foreign ranges (for example argv or an imported API buffer) through
 * this capability check. Beacon can keep this NULL or enforce its own policy.
 */
typedef int (*CvmHostMemoryAccess)(
    void *context,
    uintptr_t address,
    uint32_t size,
    int write);

typedef struct CvmHost {
    void *context;
    CvmHostCall call;
    CvmHostMemoryAccess memory_access;
} CvmHost;

typedef struct CvmModule CvmModule;

CvmLimits cvm_default_limits(void);

CvmStatus cvm_module_load(
    const void *package_data,
    size_t package_size,
    const CvmLimits *limits,
    CvmModule **module_out,
    CvmDiagnostic *diagnostic);

void cvm_module_destroy(CvmModule *module);

CvmStatus cvm_module_execute(
    CvmModule *module,
    const CvmHost *host,
    CvmValue *return_value,
    CvmDiagnostic *diagnostic);

const char *cvm_status_string(CvmStatus status);

#ifdef __cplusplus
}
#endif

#endif
