
#ifndef libiqemu_h
#define libiqemu_h

#include <dlfcn.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {

#endif

#define IQEMU_REG_PC        -1
#define IQEMU_REG_CPSR      -2

#define IQEMU_REG_X0        0
#define IQEMU_REG_X1        1
#define IQEMU_REG_X2        2
#define IQEMU_REG_X3        3
#define IQEMU_REG_X4        4
#define IQEMU_REG_X5        5
#define IQEMU_REG_X6        6
#define IQEMU_REG_X7        7
#define IQEMU_REG_X8        8
#define IQEMU_REG_X9        9
#define IQEMU_REG_X10       10
#define IQEMU_REG_X11       11
#define IQEMU_REG_X12       12
#define IQEMU_REG_X13       13
#define IQEMU_REG_X14       14
#define IQEMU_REG_X15       15
#define IQEMU_REG_X16       16
#define IQEMU_REG_X17       17
#define IQEMU_REG_X18       18
#define IQEMU_REG_X19       19
#define IQEMU_REG_X20       20
#define IQEMU_REG_X21       21
#define IQEMU_REG_X22       22
#define IQEMU_REG_X23       23
#define IQEMU_REG_X24       24
#define IQEMU_REG_X25       25
#define IQEMU_REG_X26       26
#define IQEMU_REG_X27       27
#define IQEMU_REG_X28       28
#define IQEMU_REG_X29       29
#define IQEMU_REG_FP        29
#define IQEMU_REG_X30       30
#define IQEMU_REG_LR        30
#define IQEMU_REG_SP        31

typedef bool (*fn_iqemu_is_code_in_jit)(uintptr_t v);
typedef int *(*fn_iqemu_get_jmp_buf)(int index);
typedef int (*fn_iqemu_truncate_CFI)(int index);
typedef void *(*fn_iqemu_get_current_thread_context)(void);
typedef bool (*fn_iqemu_need_emulation)(uintptr_t v);
typedef uintptr_t (*fn_iqemu_get_context_register)(void *context, int num);
typedef void (*fn_iqemu_set_context_register)(void *context, int num, uintptr_t value);
typedef bool (*fn_iqemu_get_x64_CFI)(int index, uintptr_t *sp, uintptr_t *fp);
typedef bool (*fn_iqemu_get_arm64_CFI)(int index, uintptr_t *sp, uintptr_t *fp, uintptr_t *pc);
typedef void (*fn_iqemu_set_xloop_params_by_types)(const char *types);
typedef void (*fn_iqemu_set_xloop_pc)(uintptr_t pc);
typedef void (*fn___iqemu_begin_emulation)(void);

extern fn_iqemu_is_code_in_jit iqemu_is_code_in_jit;
extern fn_iqemu_get_jmp_buf iqemu_get_jmp_buf;
extern fn_iqemu_truncate_CFI iqemu_truncate_CFI;
extern fn_iqemu_get_current_thread_context iqemu_get_current_thread_context;
extern fn_iqemu_need_emulation iqemu_need_emulation;
extern fn_iqemu_get_context_register iqemu_get_context_register;
extern fn_iqemu_set_context_register iqemu_set_context_register;
extern fn_iqemu_get_x64_CFI iqemu_get_x64_CFI;
extern fn_iqemu_get_arm64_CFI iqemu_get_arm64_CFI;
extern fn_iqemu_set_xloop_params_by_types iqemu_set_xloop_params_by_types;
extern fn_iqemu_set_xloop_pc iqemu_set_xloop_pc;

//
// This is an internal stub which does not follow standard ABI, do not use directly.
extern fn___iqemu_begin_emulation __iqemu_begin_emulation;

#define iqemu_begin_emulation(pc, fnType, types, retvalue, ...)  \
    do {    \
        iqemu_set_xloop_params_by_types(types);      \
        iqemu_set_xloop_pc(pc); \
        retvalue = ((fnType)__iqemu_begin_emulation)(__VA_ARGS__); \
    } while(0);

void init_iqemu_routines(void);
int is_iqemu_available(void);

#ifdef __cplusplus
}
#endif

#endif /* libiqemu_h */
