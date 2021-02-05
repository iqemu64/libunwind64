import lldb

# Usage:
# (lldb) command script import /path/to/unwgetctx.py
# (lldb) showcallstack

regNames = ["rax", "rbx", "rcx", "rdx", "rdi", "rsi", "rbp", "rsp",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
            "rip"]
# these variables are defined in iqemu64
UNWCTX = "dbguc"
CALLSTACK = "dbgucs"
DEPTH = "dbgdepth"


def __lldb_init_module(debugger, internal_dict):
    # cmd = "command script add -f unwgetctx.GetContext pygetcontext"
    # debugger.HandleCommand(cmd)

    cmd = "command script add -f unwgetctx.ShowCallStack showcallstack"
    debugger.HandleCommand(cmd)
    return


def GetContext(debugger, command, exe_ctx, result, internal_dict):
    # `p unw_getcontext()` does not do what I think
    frame = exe_ctx.GetFrame()
    if not frame.IsValid():
        result.SetError("Invalid frame")
        return False

    gprs = frame.regs.GetFirstValueByName("General Purpose Registers")

    target = exe_ctx.GetTarget()
    unwctx_p = target.FindFirstGlobalVariable(UNWCTX)
    unwctx_data_p = unwctx_p.GetChildMemberWithName("data")

    idx = 0
    for name in regNames:
        strVal = gprs.GetChildMemberWithName(name).GetValue()
        if (strVal is None):
            print("Read " + name + " failed! Unwinding may behave oddly.")
            strVal = "0x0"
        unwctx_data_p.child[idx].SetValueFromCString(strVal)
        idx += 1
    return True


def ImageLookupAddress(debugger, command, exe_ctx, result, internal_dict):
    target = exe_ctx.GetTarget()

    depth_p = target.FindFirstGlobalVariable(DEPTH)
    depth_p = int(depth_p.GetValue())

    callstack_p = target.FindFirstGlobalVariable(CALLSTACK)
    for i in range(depth_p):
        addr = lldb.SBAddress(int(callstack_p.child[i].GetValue()), target)
        print()
        print(hex(addr.file_addr), addr)
    return


def ShowCallStack(debugger, command, exe_ctx, result, internal_dict):
    if GetContext(debugger, command, exe_ctx, result, internal_dict):
        cmd = "p _Unwind_CallStack(&" + UNWCTX + "," + CALLSTACK + ",&" + \
              DEPTH + ")"
        debugger.HandleCommand(cmd)
        ImageLookupAddress(debugger, command, exe_ctx, result, internal_dict)
    return
