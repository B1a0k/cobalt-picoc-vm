#include "cvm/runtime.h"
#include "cvm/native.h"

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct RunnerContext {
    int script_argc;
    char **script_argv;
} RunnerContext;

static int host_memory_access(
    void *opaque,
    uintptr_t address,
    uint32_t size,
    int write)
{
    RunnerContext *context = (RunnerContext *)opaque;
    uintptr_t start;
    uintptr_t end;
    int index;
    if (write || size == 0 || address + size < address)
        return 0;
    start = (uintptr_t)context->script_argv;
    end = start +
        (uintptr_t)context->script_argc * sizeof(char *);
    if (address >= start && address + size <= end)
        return 1;
    for (index = 0; index < context->script_argc; ++index) {
        start = (uintptr_t)context->script_argv[index];
        end = start + strlen(context->script_argv[index]) + 1;
        if (address >= start && address + size <= end)
            return 1;
    }
    return 0;
}

static double host_number_as_double(
    const CvmNativeCall *call,
    uint32_t index)
{
    CvmValueType type = call->parameter_types[index];
    if (type == CVM_TYPE_F64)
        return call->arguments[index].f64;
    if (type == CVM_TYPE_F32) {
        uint32_t bits = (uint32_t)call->arguments[index].u64;
        float value;
        memcpy(&value, &bits, sizeof(value));
        return (double)value;
    }
    if (type == CVM_TYPE_I8 || type == CVM_TYPE_I16 ||
        type == CVM_TYPE_I32 || type == CVM_TYPE_I64)
        return (double)call->arguments[index].i64;
    return (double)call->arguments[index].u64;
}

static int64_t host_number_as_i64(
    const CvmNativeCall *call,
    uint32_t index)
{
    CvmValueType type = call->parameter_types[index];
    if (type == CVM_TYPE_F32 || type == CVM_TYPE_F64)
        return (int64_t)host_number_as_double(call, index);
    return call->arguments[index].i64;
}

static uint64_t host_number_as_u64(
    const CvmNativeCall *call,
    uint32_t index)
{
    CvmValueType type = call->parameter_types[index];
    if (type == CVM_TYPE_F32 || type == CVM_TYPE_F64)
        return (uint64_t)host_number_as_double(call, index);
    return call->arguments[index].u64;
}

static int format_has_long_long(const char *conversion)
{
    return strstr(conversion, "ll") != NULL ||
           strstr(conversion, "I64") != NULL;
}

static int format_has_long(const char *conversion)
{
    return !format_has_long_long(conversion) &&
           strchr(conversion, 'l') != NULL;
}

static CvmStatus host_call(
    void *context,
    const CvmNativeCall *call,
    CvmValue *return_value,
    CvmDiagnostic *diagnostic)
{
    const char *format;
    uint32_t argument = 1;
    int written = 0;
    RunnerContext *runner = (RunnerContext *)context;
    (void)diagnostic;

    if (call->address != 0)
        return cvm_native_invoke_windows(
            call->address,
            call,
            return_value,
            diagnostic);
    if (strcmp(call->library, "PICOC") != 0) {
        char module_name[MAX_PATH];
        HMODULE module;
        FARPROC address;
        size_t length = strlen(call->library);
        if (length + 5 >= sizeof(module_name))
            return CVM_STATUS_UNRESOLVED_IMPORT;
        memcpy(module_name, call->library, length + 1);
        if (strchr(module_name, '.') == NULL)
            strcat(module_name, ".dll");
        module = GetModuleHandleA(module_name);
        if (module == NULL)
            module = LoadLibraryA(module_name);
        if (module == NULL)
            return CVM_STATUS_UNRESOLVED_IMPORT;
        address = GetProcAddress(module, call->symbol);
        if (address == NULL)
            return CVM_STATUS_UNRESOLVED_IMPORT;
        return cvm_native_invoke_windows(
            (uintptr_t)address,
            call,
            return_value,
            diagnostic);
    }
    if (strcmp(call->symbol, "__picoc_argc") == 0 &&
        call->parameter_count == 0) {
        return_value->i64 = runner->script_argc;
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "__picoc_argv") == 0 &&
        call->parameter_count == 0) {
        return_value->pointer = (uintptr_t)runner->script_argv;
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "strcpy") == 0 &&
        call->parameter_count == 2) {
        return_value->pointer = (uintptr_t)strcpy(
            (char *)call->arguments[0].pointer,
            (const char *)call->arguments[1].pointer);
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "strncpy") == 0 &&
        call->parameter_count == 3) {
        return_value->pointer = (uintptr_t)strncpy(
            (char *)call->arguments[0].pointer,
            (const char *)call->arguments[1].pointer,
            (size_t)host_number_as_u64(call, 2));
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "strcat") == 0 &&
        call->parameter_count == 2) {
        return_value->pointer = (uintptr_t)strcat(
            (char *)call->arguments[0].pointer,
            (const char *)call->arguments[1].pointer);
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "strcmp") == 0 &&
        call->parameter_count == 2) {
        return_value->i64 = strcmp(
            (const char *)call->arguments[0].pointer,
            (const char *)call->arguments[1].pointer);
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "strncmp") == 0 &&
        call->parameter_count == 3) {
        return_value->i64 = strncmp(
            (const char *)call->arguments[0].pointer,
            (const char *)call->arguments[1].pointer,
            (size_t)host_number_as_u64(call, 2));
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "strlen") == 0 &&
        call->parameter_count == 1) {
        return_value->u64 = strlen(
            (const char *)call->arguments[0].pointer);
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "memcpy") == 0 &&
        call->parameter_count == 3) {
        return_value->pointer = (uintptr_t)memcpy(
            (void *)call->arguments[0].pointer,
            (const void *)call->arguments[1].pointer,
            (size_t)host_number_as_u64(call, 2));
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "memset") == 0 &&
        call->parameter_count == 3) {
        return_value->pointer = (uintptr_t)memset(
            (void *)call->arguments[0].pointer,
            (int)host_number_as_i64(call, 1),
            (size_t)host_number_as_u64(call, 2));
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "memcmp") == 0 &&
        call->parameter_count == 3) {
        return_value->i64 = memcmp(
            (const void *)call->arguments[0].pointer,
            (const void *)call->arguments[1].pointer,
            (size_t)host_number_as_u64(call, 2));
        return CVM_STATUS_OK;
    }
    if ((strcmp(call->symbol, "index") == 0 ||
         strcmp(call->symbol, "rindex") == 0) &&
        call->parameter_count == 2) {
        const char *string =
            (const char *)call->arguments[0].pointer;
        const int character =
            (int)host_number_as_i64(call, 1);
        return_value->pointer = (uintptr_t)(
            strcmp(call->symbol, "index") == 0
                ? strchr(string, character)
                : strrchr(string, character));
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "sprintf") == 0 &&
        call->parameter_count >= 2) {
        char *destination = (char *)call->arguments[0].pointer;
        const char *source =
            (const char *)call->arguments[1].pointer;
        uint32_t argument_index = 2;
        char *output = destination;
        while (*source != '\0') {
            char conversion[64];
            size_t length = 0;
            char specifier;
            int count;
            if (*source != '%') {
                *output++ = *source++;
                continue;
            }
            conversion[length++] = *source++;
            if (*source == '%') {
                *output++ = *source++;
                continue;
            }
            while (*source != '\0' &&
                   strchr("diuoxXcspfeEgGaA", *source) == NULL) {
                if (length + 2 >= sizeof(conversion))
                    return CVM_STATUS_NATIVE_CALL_FAILED;
                conversion[length++] = *source++;
            }
            if (*source == '\0' ||
                argument_index >= call->parameter_count)
                return CVM_STATUS_NATIVE_CALL_FAILED;
            specifier = *source;
            conversion[length++] = *source++;
            conversion[length] = '\0';
            if (specifier == 'd' || specifier == 'i') {
                const int64_t number =
                    host_number_as_i64(call, argument_index++);
                if (format_has_long_long(conversion))
                    count = sprintf(
                        output, conversion, (long long)number);
                else if (format_has_long(conversion))
                    count = sprintf(
                        output, conversion, (long)number);
                else
                    count = sprintf(
                        output, conversion, (int)number);
            } else if (strchr("uoxX", specifier) != NULL) {
                const uint64_t number =
                    host_number_as_u64(call, argument_index++);
                if (format_has_long_long(conversion))
                    count = sprintf(
                        output,
                        conversion,
                        (unsigned long long)number);
                else if (format_has_long(conversion))
                    count = sprintf(
                        output,
                        conversion,
                        (unsigned long)number);
                else
                    count = sprintf(
                        output,
                        conversion,
                        (unsigned int)number);
            }
            else if (specifier == 'c')
                count = sprintf(
                    output,
                    conversion,
                    (int)host_number_as_i64(
                        call,
                        argument_index++));
            else if (specifier == 's')
                count = sprintf(
                    output,
                    conversion,
                    (const char *)call->arguments[
                        argument_index++].pointer);
            else if (specifier == 'p')
                count = sprintf(
                    output,
                    conversion,
                    (void *)call->arguments[
                        argument_index++].pointer);
            else
                count = sprintf(
                    output,
                    conversion,
                    host_number_as_double(
                        call,
                        argument_index++));
            if (count < 0)
                return CVM_STATUS_NATIVE_CALL_FAILED;
            output += count;
        }
        *output = '\0';
        return_value->i64 = (int64_t)(output - destination);
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "fopen") == 0 &&
        call->parameter_count == 2) {
        return_value->pointer = (uintptr_t)fopen(
            (const char *)call->arguments[0].pointer,
            (const char *)call->arguments[1].pointer);
        return return_value->pointer == 0
            ? CVM_STATUS_NATIVE_CALL_FAILED
            : CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "fclose") == 0 &&
        call->parameter_count == 1) {
        return_value->i64 =
            fclose((FILE *)call->arguments[0].pointer);
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "fwrite") == 0 &&
        call->parameter_count == 4) {
        return_value->u64 = fwrite(
            (const void *)call->arguments[0].pointer,
            (size_t)host_number_as_u64(call, 1),
            (size_t)host_number_as_u64(call, 2),
            (FILE *)call->arguments[3].pointer);
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "fread") == 0 &&
        call->parameter_count == 4) {
        return_value->u64 = fread(
            (void *)call->arguments[0].pointer,
            (size_t)host_number_as_u64(call, 1),
            (size_t)host_number_as_u64(call, 2),
            (FILE *)call->arguments[3].pointer);
        return CVM_STATUS_OK;
    }
    if ((strcmp(call->symbol, "fgetc") == 0 ||
         strcmp(call->symbol, "getc") == 0) &&
        call->parameter_count == 1) {
        return_value->i64 =
            fgetc((FILE *)call->arguments[0].pointer);
        return CVM_STATUS_OK;
    }
    if (strcmp(call->symbol, "fgets") == 0 &&
        call->parameter_count == 3) {
        return_value->pointer = (uintptr_t)fgets(
            (char *)call->arguments[0].pointer,
            (int)host_number_as_i64(call, 1),
            (FILE *)call->arguments[2].pointer);
        return CVM_STATUS_OK;
    }
    {
        double math_result = 0.0;
        int matched = 1;
        const double x =
            call->parameter_count > 0
                ? host_number_as_double(call, 0)
                : 0.0;
        if (strcmp(call->symbol, "sin") == 0) math_result = sin(x);
        else if (strcmp(call->symbol, "cos") == 0) math_result = cos(x);
        else if (strcmp(call->symbol, "tan") == 0) math_result = tan(x);
        else if (strcmp(call->symbol, "asin") == 0) math_result = asin(x);
        else if (strcmp(call->symbol, "acos") == 0) math_result = acos(x);
        else if (strcmp(call->symbol, "atan") == 0) math_result = atan(x);
        else if (strcmp(call->symbol, "sinh") == 0) math_result = sinh(x);
        else if (strcmp(call->symbol, "cosh") == 0) math_result = cosh(x);
        else if (strcmp(call->symbol, "tanh") == 0) math_result = tanh(x);
        else if (strcmp(call->symbol, "exp") == 0) math_result = exp(x);
        else if (strcmp(call->symbol, "fabs") == 0) math_result = fabs(x);
        else if (strcmp(call->symbol, "log") == 0) math_result = log(x);
        else if (strcmp(call->symbol, "log10") == 0) math_result = log10(x);
        else if (strcmp(call->symbol, "sqrt") == 0) math_result = sqrt(x);
        else if (strcmp(call->symbol, "round") == 0) math_result = round(x);
        else if (strcmp(call->symbol, "ceil") == 0) math_result = ceil(x);
        else if (strcmp(call->symbol, "floor") == 0) math_result = floor(x);
        else if (strcmp(call->symbol, "pow") == 0 &&
                 call->parameter_count == 2) {
            math_result = pow(x, host_number_as_double(call, 1));
        } else {
            matched = 0;
        }
        if (matched) {
            return_value->f64 = math_result;
            return CVM_STATUS_OK;
        }
    }
    if (strcmp(call->symbol, "printf") != 0 ||
        call->parameter_count == 0)
        return CVM_STATUS_UNRESOLVED_IMPORT;
    format = (const char *)call->arguments[0].pointer;
    if (format == NULL)
        return CVM_STATUS_NATIVE_CALL_FAILED;

    while (*format != '\0') {
        char conversion[64];
        size_t length = 0;
        char specifier;
        int count;

        if (*format != '%') {
            putchar((unsigned char)*format++);
            ++written;
            continue;
        }
        conversion[length++] = *format++;
        if (*format == '%') {
            putchar('%');
            ++format;
            ++written;
            continue;
        }
        while (*format != '\0' &&
               strchr("diuoxXcspfeEgGaA", *format) == NULL) {
            if (length + 2 >= sizeof(conversion))
                return CVM_STATUS_NATIVE_CALL_FAILED;
            conversion[length++] = *format++;
        }
        if (*format == '\0' || argument >= call->parameter_count)
            return CVM_STATUS_NATIVE_CALL_FAILED;
        specifier = *format;
        conversion[length++] = *format++;
        conversion[length] = '\0';

        switch (specifier) {
        case 'd':
        case 'i':
        {
            const int64_t number =
                host_number_as_i64(call, argument++);
            if (format_has_long_long(conversion))
                count = printf(conversion, (long long)number);
            else if (format_has_long(conversion))
                count = printf(conversion, (long)number);
            else
                count = printf(conversion, (int)number);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        {
            const uint64_t number =
                host_number_as_u64(call, argument++);
            if (format_has_long_long(conversion))
                count = printf(
                    conversion,
                    (unsigned long long)number);
            else if (format_has_long(conversion))
                count = printf(
                    conversion,
                    (unsigned long)number);
            else
                count = printf(
                    conversion,
                    (unsigned int)number);
            break;
        }
        case 'c':
            count = printf(
                conversion,
                (int)host_number_as_i64(call, argument++));
            break;
        case 's':
            count = printf(
                conversion,
                (const char *)call->arguments[argument++].pointer);
            break;
        case 'p':
            count = printf(
                conversion,
                (void *)call->arguments[argument++].pointer);
            break;
        default:
            count = printf(
                conversion,
                host_number_as_double(call, argument++));
            break;
        }
        if (count < 0)
            return CVM_STATUS_NATIVE_CALL_FAILED;
        written += count;
    }
    return_value->i64 = written;
    return CVM_STATUS_OK;
}

int main(int argc, char **argv)
{
    FILE *input;
    long length;
    unsigned char *bytes;
    CvmModule *module = NULL;
    CvmDiagnostic diagnostic = {0};
    CvmValue result = {0};
    CvmStatus status;
    CvmHost host;
    RunnerContext runner;
    CvmLimits limits;

    int print_result = 0;
    int argument_index = 1;
    const char *package_path;

    limits = cvm_default_limits();
    while (argument_index < argc) {
        if (strcmp(argv[argument_index], "--print-result") == 0) {
            print_result = 1;
            ++argument_index;
            continue;
        }
        if (strcmp(argv[argument_index], "--instruction-budget") == 0) {
            char *end = NULL;
            unsigned long long parsed;
            if (argument_index + 1 >= argc) {
                fprintf(stderr, "--instruction-budget requires a value\n");
                return 2;
            }
            parsed = strtoull(argv[argument_index + 1], &end, 10);
            if (end == argv[argument_index + 1] || *end != '\0' ||
                parsed == 0) {
                fprintf(stderr, "invalid instruction budget\n");
                return 2;
            }
            limits.instruction_budget = (uint64_t)parsed;
            argument_index += 2;
            continue;
        }
        break;
    }
    if (argument_index >= argc) {
        fprintf(
            stderr,
            "usage: cvmrun [--print-result] "
            "[--instruction-budget N] <input.cvm> [script-args...]\n");
        return 2;
    }
    package_path = argv[argument_index++];
    input = fopen(package_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "cannot open %s\n", package_path);
        return 1;
    }
    fseek(input, 0, SEEK_END);
    length = ftell(input);
    fseek(input, 0, SEEK_SET);
    if (length <= 0) {
        fclose(input);
        return 1;
    }
    bytes = (unsigned char *)malloc((size_t)length);
    if (bytes == NULL || fread(bytes, 1, (size_t)length, input) !=
        (size_t)length) {
        free(bytes);
        fclose(input);
        return 1;
    }
    fclose(input);

    status = cvm_module_load(
        bytes,
        (size_t)length,
        &limits,
        &module,
        &diagnostic);
    free(bytes);
    runner.script_argc = argc - argument_index;
    runner.script_argv = argv + argument_index;
    host.context = &runner;
    host.call = host_call;
    host.memory_access = host_memory_access;
    if (status == CVM_STATUS_OK)
        status = cvm_module_execute(module, &host, &result, &diagnostic);
    cvm_module_destroy(module);

    if (status != CVM_STATUS_OK) {
        fprintf(
            stderr,
            "%s at function=%u instruction=%u: %s\n",
            cvm_status_string(status),
            diagnostic.function_index,
            diagnostic.instruction_index,
            diagnostic.message);
        return 1;
    }
    if (print_result)
        printf("%lld\n", (long long)result.i64);
    return 0;
}
