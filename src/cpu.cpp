// One dynarmic A32 JIT per host thread, all sharing the identity-mapped guest memory.
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include "rr2.h"

static constexpr int kMaxThreads = 8;
static Dynarmic::ExclusiveMonitor g_monitor{kMaxThreads};
static std::atomic<int> g_next_id;

template <class T> static bool cas(u32 a, T v, T expected) {
    return __atomic_compare_exchange_n(ptr<T>(a), &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// Memory callbacks only run when fastmem faults, i.e. on a genuinely bad guest pointer.
struct Cpu final : Dynarmic::A32::UserCallbacks {
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    u32 stack, ret_thunk;

    Cpu() {
        Dynarmic::A32::UserConfig cfg{};
        cfg.callbacks = this;
        cfg.processor_id = g_next_id++ % kMaxThreads;
        cfg.global_monitor = &g_monitor;
        cfg.arch_version = Dynarmic::A32::ArchVersion::v7;
        cfg.fastmem_pointer = 0;  // guest address == host address
        cfg.fastmem_exclusive_access = true;
        cfg.enable_cycle_counting = false;
        cfg.always_little_endian = true;
        jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
        stack = gaddr(guest_malloc(1 << 20)) + (1 << 20);
        u32* t = (u32*)guest_malloc(8);
        t[0] = 0xEF000000 | kReturnSvc;  // svc #kReturnSvc
        ret_thunk = gaddr(t);
    }

    std::uint8_t MemoryRead8(u32 a) override { return *ptr<std::uint8_t>(a); }
    std::uint16_t MemoryRead16(u32 a) override { return *ptr<std::uint16_t>(a); }
    std::uint32_t MemoryRead32(u32 a) override { return *ptr<std::uint32_t>(a); }
    std::uint64_t MemoryRead64(u32 a) override { return *ptr<std::uint64_t>(a); }
    void MemoryWrite8(u32 a, std::uint8_t v) override { *ptr<std::uint8_t>(a) = v; }
    void MemoryWrite16(u32 a, std::uint16_t v) override { *ptr<std::uint16_t>(a) = v; }
    void MemoryWrite32(u32 a, std::uint32_t v) override { *ptr<std::uint32_t>(a) = v; }
    void MemoryWrite64(u32 a, std::uint64_t v) override { *ptr<std::uint64_t>(a) = v; }
    bool MemoryWriteExclusive8(u32 a, std::uint8_t v, std::uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(u32 a, std::uint16_t v, std::uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(u32 a, std::uint32_t v, std::uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(u32 a, std::uint64_t v, std::uint64_t e) override { return cas(a, v, e); }

    void CallSVC(std::uint32_t swi) override {
        if (swi == kReturnSvc) return jit->HaltExecution();
        dispatch(swi, Ctx{jit->Regs(), jit->ExtRegs().data()});
    }
    void ExceptionRaised(u32 pc, Dynarmic::A32::Exception e) override {
        LOG("guest exception %d at 0x%08x", int(e), pc);
        jit->HaltExecution();
    }
    void InterpreterFallback(u32 pc, size_t) override { LOG("interpreter fallback at 0x%08x", pc); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 0; }
};

u32 call_guest(u32 fn, std::initializer_list<u32> args) {
    thread_local Cpu cpu;
    thread_local bool running;
    if (running) return LOG("nested guest call to 0x%08x not supported yet", fn), 0;

    auto& r = cpu.jit->Regs();
    const u32* a = args.begin();
    int n = int(args.size());
    u32 sp = (cpu.stack - 4 * std::max(n - 4, 0)) & ~7u;  // args 5+ go on the stack
    for (int i = 0; i < n; i++) (i < 4 ? r[i] : ptr<u32>(sp)[i - 4]) = a[i];
    r[13] = sp;
    r[14] = cpu.ret_thunk;
    r[15] = fn & ~1u;
    cpu.jit->SetCpsr(0x10 | (fn & 1 ? 0x20 : 0));  // user mode, Thumb if bit 0 set
    running = true;
    cpu.jit->Run();
    running = false;
    return r[0];
}
