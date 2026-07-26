#ifndef COBALT_CVM_FORMAT_H
#define COBALT_CVM_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CVM_MAGIC UINT32_C(0x314D5643) /* "CVM1" in little endian */
#define CVM_FORMAT_MAJOR 1u
#define CVM_FORMAT_MINOR 0u
#define CVM_NO_INDEX UINT32_C(0xFFFFFFFF)

typedef enum CvmTargetArch {
    CVM_ARCH_UNKNOWN = 0,
    CVM_ARCH_X86 = 1,
    CVM_ARCH_X64 = 2
} CvmTargetArch;

typedef enum CvmProfile {
    CVM_PROFILE_PICOC_COMPAT = 1,
    CVM_PROFILE_BEACON = 2
} CvmProfile;

typedef enum CvmFeature {
    CVM_FEATURE_FLOATING_POINT = UINT32_C(1) << 0,
    CVM_FEATURE_NATIVE_IMPORTS = UINT32_C(1) << 1,
    CVM_FEATURE_NATIVE_INDIRECT = UINT32_C(1) << 2,
    CVM_FEATURE_DEBUG_INFO = UINT32_C(1) << 3
} CvmFeature;

typedef enum CvmSectionKind {
    CVM_SECTION_NONE = 0,
    CVM_SECTION_STRINGS = 1,
    CVM_SECTION_CONSTANTS = 2,
    CVM_SECTION_DATA = 3,
    CVM_SECTION_GLOBALS = 4,
    CVM_SECTION_FUNCTIONS = 5,
    CVM_SECTION_PARAMETERS = 6,
    CVM_SECTION_CODE = 7,
    CVM_SECTION_IMPORTS = 8,
    CVM_SECTION_SIGNATURES = 9,
    CVM_SECTION_SIGNATURE_PARAMETERS = 10,
    CVM_SECTION_RELOCATIONS = 11,
    CVM_SECTION_DEBUG = 12
} CvmSectionKind;

enum {
    CVM_SECTION_REQUIRED = 1u << 0
};

#pragma pack(push, 1)

typedef struct CvmPackageHeader {
    uint32_t magic;
    uint16_t format_major;
    uint16_t format_minor;
    uint8_t target_arch;
    uint8_t pointer_size;
    uint8_t endian;
    uint8_t profile;
    uint32_t features;
    uint32_t section_count;
    uint32_t entry_function;
    uint32_t package_size;
    uint32_t global_bytes;
    uint32_t required_stack_cells;
    uint32_t required_call_depth;
    uint32_t checksum;
} CvmPackageHeader;

typedef struct CvmSectionHeader {
    uint16_t kind;
    uint16_t flags;
    uint32_t offset;
    uint32_t size;
    uint32_t count;
    uint32_t entry_size;
} CvmSectionHeader;

typedef enum CvmValueType {
    CVM_TYPE_VOID = 0,
    CVM_TYPE_I8 = 1,
    CVM_TYPE_U8 = 2,
    CVM_TYPE_I16 = 3,
    CVM_TYPE_U16 = 4,
    CVM_TYPE_I32 = 5,
    CVM_TYPE_U32 = 6,
    CVM_TYPE_I64 = 7,
    CVM_TYPE_U64 = 8,
    CVM_TYPE_F32 = 9,
    CVM_TYPE_F64 = 10,
    CVM_TYPE_PTR = 11,
    CVM_TYPE_CSTR = 12,
    CVM_TYPE_SIZE = 13
} CvmValueType;

typedef enum CvmCallingConvention {
    CVM_CALL_DEFAULT = 0,
    CVM_CALL_CDECL = 1,
    CVM_CALL_STDCALL = 2,
    CVM_CALL_WIN64 = 3,
    CVM_CALL_VM = 4
} CvmCallingConvention;

typedef struct CvmFunction {
    uint32_t name_string;
    uint32_t first_instruction;
    uint32_t instruction_count;
    uint32_t first_parameter;
    uint16_t parameter_count;
    uint8_t return_type;
    uint8_t flags;
    uint32_t local_bytes;
    uint16_t local_alignment;
    uint16_t maximum_stack_cells;
} CvmFunction;

typedef struct CvmParameter {
    uint32_t name_string;
    uint32_t frame_offset;
    uint8_t value_type;
    uint8_t flags;
    uint16_t reserved;
} CvmParameter;

typedef struct CvmNativeSignature {
    uint32_t first_parameter_type;
    uint16_t parameter_count;
    uint8_t return_type;
    uint8_t calling_convention;
    uint32_t flags;
} CvmNativeSignature;

typedef struct CvmNativeImport {
    uint32_t library_string;
    uint32_t symbol_string;
    uint32_t signature_index;
    uint32_t flags;
} CvmNativeImport;

typedef struct CvmGlobal {
    uint32_t name_string;
    uint32_t data_offset;
    uint32_t size;
    uint16_t alignment;
    uint8_t value_type;
    uint8_t flags;
} CvmGlobal;

#pragma pack(pop)

#if defined(__cplusplus)
static_assert(sizeof(CvmPackageHeader) == 44, "CvmPackageHeader layout changed");
static_assert(sizeof(CvmSectionHeader) == 20, "CvmSectionHeader layout changed");
#elif defined(_MSC_VER)
typedef char CvmPackageHeader_size_must_be_44[
    sizeof(CvmPackageHeader) == 44 ? 1 : -1];
typedef char CvmSectionHeader_size_must_be_20[
    sizeof(CvmSectionHeader) == 20 ? 1 : -1];
#else
_Static_assert(sizeof(CvmPackageHeader) == 44, "CvmPackageHeader layout changed");
_Static_assert(sizeof(CvmSectionHeader) == 20, "CvmSectionHeader layout changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
