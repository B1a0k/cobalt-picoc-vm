#ifndef COBALT_CVM_NATIVE_H
#define COBALT_CVM_NATIVE_H

#include "cvm/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Invokes an already-resolved Windows native address using the signature
 * carried by CvmNativeCall. The caller owns symbol resolution.
 */
CvmStatus cvm_native_invoke_windows(
    uintptr_t address,
    const CvmNativeCall *call,
    CvmValue *return_value,
    CvmDiagnostic *diagnostic);

#ifdef __cplusplus
}
#endif

#endif
