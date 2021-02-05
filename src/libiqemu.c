
#include "libiqemu.h"

#define NULL    0

fn_iqemu_is_code_in_jit iqemu_is_code_in_jit = NULL;
fn_iqemu_get_jmp_buf iqemu_get_jmp_buf = NULL;
fn_iqemu_truncate_CFI iqemu_truncate_CFI = NULL;
fn_iqemu_get_current_thread_context iqemu_get_current_thread_context = NULL;
fn_iqemu_need_emulation iqemu_need_emulation = NULL;
fn_iqemu_get_context_register iqemu_get_context_register = NULL;
fn_iqemu_set_context_register iqemu_set_context_register = NULL;
fn_iqemu_get_x64_CFI iqemu_get_x64_CFI = NULL;
fn_iqemu_get_arm64_CFI iqemu_get_arm64_CFI = NULL;
fn_iqemu_set_xloop_params_by_types iqemu_set_xloop_params_by_types = NULL;
fn_iqemu_set_xloop_pc iqemu_set_xloop_pc = NULL;
fn___iqemu_begin_emulation __iqemu_begin_emulation = NULL;

static int g_is_iqemu_available = 0;

void
init_iqemu_routines() {
    static int iqemu_inited = 0;
    if(__sync_bool_compare_and_swap(&iqemu_inited, 0, 1)) {
        void *p = dlopen("/usr/local/lib/libiqemu.dylib", RTLD_NOLOAD);
        if(NULL == p) {
            return;
        }
        
        iqemu_is_code_in_jit = (fn_iqemu_is_code_in_jit)dlsym(p, "iqemu_is_code_in_jit");
        iqemu_get_jmp_buf = (fn_iqemu_get_jmp_buf)dlsym(p, "iqemu_get_jmp_buf");
        iqemu_truncate_CFI = (fn_iqemu_truncate_CFI)dlsym(p, "iqemu_truncate_CFI");
        iqemu_get_current_thread_context = (fn_iqemu_get_current_thread_context)dlsym(p, "iqemu_get_current_thread_context");
        iqemu_need_emulation = (fn_iqemu_need_emulation)dlsym(p, "iqemu_need_emulation");
        iqemu_get_context_register = (fn_iqemu_get_context_register)dlsym(p, "iqemu_get_context_register");
        iqemu_set_context_register = (fn_iqemu_set_context_register)dlsym(p, "iqemu_set_context_register");
        iqemu_get_x64_CFI = (fn_iqemu_get_x64_CFI)dlsym(p, "iqemu_get_x64_CFI");
        iqemu_get_arm64_CFI = (fn_iqemu_get_arm64_CFI)dlsym(p, "iqemu_get_arm64_CFI");
        iqemu_set_xloop_params_by_types = (fn_iqemu_set_xloop_params_by_types)dlsym(p, "iqemu_set_xloop_params_by_types");
        iqemu_set_xloop_pc = (fn_iqemu_set_xloop_pc)dlsym(p, "iqemu_set_xloop_pc");
        __iqemu_begin_emulation = (fn___iqemu_begin_emulation)dlsym(p, "__iqemu_begin_emulation");
        
        __sync_swap(&g_is_iqemu_available, 1);
        
        dlclose(p);
    }
}

int
is_iqemu_available()
{
    return g_is_iqemu_available;
}
