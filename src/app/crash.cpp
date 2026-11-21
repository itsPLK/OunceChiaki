// Crash reports: abort() or std::terminate writes logs/crash.txt with a frame
// pointer backtrace. The CPU exception handler is kept for when the loader
// delivers exceptions: under kaiHEN 0.0.4 it doesn't (tested 2026-11-22: a
// deliberate fault killed the app without calling it), so a CPU fault leaves
// only the end of ounce.log. Addresses are runtime ones; "anchor" is the runtime
// address of ounce_crash_anchor, so the load base is anchor minus its address
// in OunceChiaki.elf (aarch64-none-elf-nm), and
//   aarch64-none-elf-addr2line -fe OunceChiaki.elf <address - base>
// names each frame. scripts/crash.py does that.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <switch.h>
#include <unistd.h>

extern "C" {
// Deliver exceptions to the handler even if a debugger could take them.
u32 __nx_exception_ignoredebug = 1;
void ounce_crash_anchor(void) {}
}

namespace
{

constexpr const char* CrashPath = "sdmc:/switch/OunceChiaki/logs/crash.txt";

// Formats with snprintf into a static buffer and writes it with one write():
// no allocation, so it works when the heap is broken.
void writeReport(const char* fmt, ...)
{
    static char buffer[4096];
    va_list args;
    va_start(args, fmt);
    int length = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (length <= 0)
        return;
    if (length > static_cast<int>(sizeof(buffer)))
        length = sizeof(buffer);
    const int fd = open(CrashPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;
    write(fd, buffer, length);
    fsync(fd);
    close(fd);
}

// Walks the frame record chain (x29 -> {previous x29, return address}) while it
// stays inside a plausible stack window above sp.
int backtrace(uintptr_t fp, uintptr_t sp, char* out, int size)
{
    int length = 0;
    for (int depth = 0; depth < 24 && fp && !(fp & 7) && fp >= sp && fp - sp < (8u << 20); depth++)
    {
        const auto* frame = reinterpret_cast<const uintptr_t*>(fp);
        const uintptr_t next = frame[0], ret = frame[1];
        if (!ret)
            break;
        length += std::snprintf(out + length, size - length, " 0x%lx", static_cast<unsigned long>(ret));
        if (length >= size - 24 || next <= fp)
            break;
        fp = next;
    }
    return length;
}

[[noreturn]] void onTerminate()
{
    const char* what = "(no exception)";
    if (auto current = std::current_exception())
    {
        try
        {
            std::rethrow_exception(current);
        }
        catch (const std::exception& e)
        {
            what = e.what();
        }
        catch (...)
        {
            what = "(unknown exception)";
        }
    }
    char trace[512] = "";
    backtrace(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)),
        reinterpret_cast<uintptr_t>(__builtin_frame_address(0)), trace, sizeof(trace));
    writeReport("std::terminate: %s\nanchor 0x%lx\nbacktrace%s\n\n", what,
        reinterpret_cast<unsigned long>(&ounce_crash_anchor), trace);
    std::abort();
}

__attribute__((constructor)) void installTerminate() { std::set_terminate(onTerminate); }

}

extern "C" {

void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
    char trace[512] = "";
    backtrace(ctx->fp.x, ctx->sp.x, trace, sizeof(trace));
    writeReport("CPU exception: desc 0x%x esr 0x%x far 0x%lx\n"
                "pc 0x%lx lr 0x%lx sp 0x%lx fp 0x%lx\n"
                "x0 0x%lx x1 0x%lx x2 0x%lx x3 0x%lx\n"
                "x19 0x%lx x20 0x%lx x21 0x%lx x22 0x%lx\n"
                "anchor 0x%lx\nbacktrace%s\n\n",
        ctx->error_desc, ctx->esr, static_cast<unsigned long>(ctx->far.x),
        static_cast<unsigned long>(ctx->pc.x), static_cast<unsigned long>(ctx->lr.x),
        static_cast<unsigned long>(ctx->sp.x), static_cast<unsigned long>(ctx->fp.x),
        static_cast<unsigned long>(ctx->cpu_gprs[0].x), static_cast<unsigned long>(ctx->cpu_gprs[1].x),
        static_cast<unsigned long>(ctx->cpu_gprs[2].x), static_cast<unsigned long>(ctx->cpu_gprs[3].x),
        static_cast<unsigned long>(ctx->cpu_gprs[19].x), static_cast<unsigned long>(ctx->cpu_gprs[20].x),
        static_cast<unsigned long>(ctx->cpu_gprs[21].x), static_cast<unsigned long>(ctx->cpu_gprs[22].x),
        reinterpret_cast<unsigned long>(&ounce_crash_anchor), trace);
}

void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void)
{
    char trace[512] = "";
    backtrace(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)),
        reinterpret_cast<uintptr_t>(__builtin_frame_address(0)), trace, sizeof(trace));
    writeReport("abort() called from 0x%lx\nanchor 0x%lx\nbacktrace%s\n\n",
        reinterpret_cast<unsigned long>(__builtin_return_address(0)),
        reinterpret_cast<unsigned long>(&ounce_crash_anchor), trace);
    __real_abort();
}

}
