// HldsMonitoring — header-free 32-bit ELF/GOT adapter (plan M-P2, design §19.3).
//
// Uses libc/dl/sys/mman directly and includes NO engine header, so it links
// both into the plugin and into the 32-bit synthetic harness (M-P5, MN-3).
// Policy (b) (MN-1): a lazy-bound object is patched; every rescan re-verifies
// that the slot still holds our wrapper and re-patches it if clobbered.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "modnet_got.h"

#include <cerrno>
#include <cstring>

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <netdb.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace hldsmon {

ssize_t wrap_send(int fd, const void* buf, size_t len, int flags);
ssize_t wrap_recv(int fd, void* buf, size_t len, int flags);
ssize_t wrap_sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* addr, socklen_t alen);
ssize_t wrap_recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* addr, socklen_t* alen);
ssize_t wrap_sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t wrap_recvmsg(int fd, struct msghdr* msg, int flags);
int wrap_connect(int fd, const struct sockaddr* addr, socklen_t len);
int wrap_accept(int fd, struct sockaddr* addr, socklen_t* len);
int wrap_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);

namespace {

// ---- target symbol set (§19.3) ----
enum SymId {
    SYM_SEND = 0,
    SYM_RECV,
    SYM_SENDTO,
    SYM_RECVFROM,
    SYM_SENDMSG,
    SYM_RECVMSG,
    SYM_CONNECT,
    SYM_ACCEPT,
    SYM_GETADDRINFO,
    SYM_COUNT
};

struct SymDef {
    const char* name;
    void*       wrapper;
};

// Resolved originals (C1: from dlsym(RTLD_DEFAULT), never from the GOT slot).
ssize_t (*g_orig_send)(int, const void*, size_t, int) = nullptr;
ssize_t (*g_orig_recv)(int, void*, size_t, int) = nullptr;
ssize_t (*g_orig_sendto)(int, const void*, size_t, int, const struct sockaddr*, socklen_t) = nullptr;
ssize_t (*g_orig_recvfrom)(int, void*, size_t, int, struct sockaddr*, socklen_t*) = nullptr;
ssize_t (*g_orig_sendmsg)(int, const struct msghdr*, int) = nullptr;
ssize_t (*g_orig_recvmsg)(int, struct msghdr*, int) = nullptr;
int (*g_orig_connect)(int, const struct sockaddr*, socklen_t) = nullptr;
int (*g_orig_accept)(int, struct sockaddr*, socklen_t*) = nullptr;
int (*g_orig_getaddrinfo)(const char*, const char*, const struct addrinfo*, struct addrinfo**) = nullptr;

SymDef g_syms[SYM_COUNT] = {
    {"send", reinterpret_cast<void*>(&wrap_send)},
    {"recv", reinterpret_cast<void*>(&wrap_recv)},
    {"sendto", reinterpret_cast<void*>(&wrap_sendto)},
    {"recvfrom", reinterpret_cast<void*>(&wrap_recvfrom)},
    {"sendmsg", reinterpret_cast<void*>(&wrap_sendmsg)},
    {"recvmsg", reinterpret_cast<void*>(&wrap_recvmsg)},
    {"connect", reinterpret_cast<void*>(&wrap_connect)},
    {"accept", reinterpret_cast<void*>(&wrap_accept)},
    {"getaddrinfo", reinterpret_cast<void*>(&wrap_getaddrinfo)},
};

// Reserved counter slot used only by the probe's temporary range.
constexpr uint16_t kProbeObj = static_cast<uint16_t>(HLDSMON_MODNET_MAX_OBJECTS - 1);

// ---- state ----
constexpr size_t HLDSMON_MODNET_MAX_SLOTS = 384;

struct ObjectEntry {
    uintptr_t      base = 0;
    uint16_t       obj = 0;
    bool           used = false;
    bool           seen = false;
    uint16_t       live_slots = 0;
    char           so[64] = {0};
    char           module[64] = {0};
    ModnetSnapshot prev{};
    double         prev_t = 0.0;
    bool           has_prev = false;
};

struct PatchedSlot {
    uintptr_t slot = 0;
    uintptr_t original = 0;
    uintptr_t wrapper = 0;
    uintptr_t base = 0;
    uint16_t  obj = 0;
    int       sym = 0;
    bool      used = false;
    bool      live = false;
    bool      active = false;
};

ObjectEntry g_objects[HLDSMON_MODNET_MAX_OBJECTS];
PatchedSlot g_slots[HLDSMON_MODNET_MAX_SLOTS];

ModnetState            g_state;
bool                   g_self_base_known = false;
uintptr_t              g_self_base = 0;
RangeCache             g_range_buf[2];
int                    g_range_next = 0;
std::atomic<bool>      g_rescan_req{false};

void set_error(const char* msg) noexcept {
    size_t i = 0;
    if (msg != nullptr) {
        for (; msg[i] != '\0' && i + 1 < sizeof(g_state.last_error); ++i) {
            g_state.last_error[i] = msg[i];
        }
    }
    g_state.last_error[i] = '\0';
}

void copy_bounded(char* dst, size_t cap, const char* src) noexcept {
    if (cap == 0) {
        return;
    }
    size_t i = 0;
    if (src != nullptr) {
        for (; src[i] != '\0' && i + 1 < cap; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

const char* base_name(const char* path) noexcept {
    const char* b = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/') {
            b = p + 1;
        }
    }
    return b;
}

void make_module_label(const char* path, char* out, size_t cap) noexcept {
    const char* b = base_name(path);
    size_t len = std::strlen(b);
    if (len > 3 && std::strcmp(b + len - 3, ".so") == 0) {
        len -= 3;
    }
    if (len >= cap) {
        len = cap - 1;
    }
    std::memcpy(out, b, len);
    out[len] = '\0';
}

int sym_index(const char* name) noexcept {
    for (int i = 0; i < SYM_COUNT; ++i) {
        if (std::strcmp(g_syms[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

uintptr_t original_for(int sym) noexcept {
    switch (sym) {
        case SYM_SEND: return reinterpret_cast<uintptr_t>(g_orig_send);
        case SYM_RECV: return reinterpret_cast<uintptr_t>(g_orig_recv);
        case SYM_SENDTO: return reinterpret_cast<uintptr_t>(g_orig_sendto);
        case SYM_RECVFROM: return reinterpret_cast<uintptr_t>(g_orig_recvfrom);
        case SYM_SENDMSG: return reinterpret_cast<uintptr_t>(g_orig_sendmsg);
        case SYM_RECVMSG: return reinterpret_cast<uintptr_t>(g_orig_recvmsg);
        case SYM_CONNECT: return reinterpret_cast<uintptr_t>(g_orig_connect);
        case SYM_ACCEPT: return reinterpret_cast<uintptr_t>(g_orig_accept);
        case SYM_GETADDRINFO: return reinterpret_cast<uintptr_t>(g_orig_getaddrinfo);
        default: return 0;
    }
}

void resolve_originals() noexcept {
    g_orig_send = reinterpret_cast<ssize_t (*)(int, const void*, size_t, int)>(dlsym(RTLD_DEFAULT, "send"));
    g_orig_recv = reinterpret_cast<ssize_t (*)(int, void*, size_t, int)>(dlsym(RTLD_DEFAULT, "recv"));
    g_orig_sendto = reinterpret_cast<ssize_t (*)(int, const void*, size_t, int, const struct sockaddr*, socklen_t)>(dlsym(RTLD_DEFAULT, "sendto"));
    g_orig_recvfrom = reinterpret_cast<ssize_t (*)(int, void*, size_t, int, struct sockaddr*, socklen_t*)>(dlsym(RTLD_DEFAULT, "recvfrom"));
    g_orig_sendmsg = reinterpret_cast<ssize_t (*)(int, const struct msghdr*, int)>(dlsym(RTLD_DEFAULT, "sendmsg"));
    g_orig_recvmsg = reinterpret_cast<ssize_t (*)(int, struct msghdr*, int)>(dlsym(RTLD_DEFAULT, "recvmsg"));
    g_orig_connect = reinterpret_cast<int (*)(int, const struct sockaddr*, socklen_t)>(dlsym(RTLD_DEFAULT, "connect"));
    g_orig_accept = reinterpret_cast<int (*)(int, struct sockaddr*, socklen_t*)>(dlsym(RTLD_DEFAULT, "accept"));
    g_orig_getaddrinfo = reinterpret_cast<int (*)(const char*, const char*, const struct addrinfo*, struct addrinfo**)>(dlsym(RTLD_DEFAULT, "getaddrinfo"));
}

void ensure_self_base() noexcept {
    if (g_self_base_known) {
        return;
    }
    Dl_info di;
    std::memset(&di, 0, sizeof(di));
    if (dladdr(reinterpret_cast<void*>(&modnet_probe), &di) != 0 && di.dli_fbase != nullptr) {
        g_self_base = reinterpret_cast<uintptr_t>(di.dli_fbase);
        g_self_base_known = true;
    }
}

bool is_hard_excluded(const char* name, uintptr_t base) noexcept {
    if (name == nullptr || name[0] != '/') {
        return true; // main executable, vDSO or unnamed object
    }
    if (g_self_base_known && base == g_self_base) {
        return true; // this plugin
    }
    const char* b = base_name(name);
    if (std::strncmp(b, "libc.", 5) == 0 || std::strncmp(b, "libc-", 5) == 0) {
        return true;
    }
    if (std::strncmp(b, "ld-", 3) == 0 || std::strncmp(b, "ld.so", 5) == 0) {
        return true;
    }
    return false;
}

bool basename_in_targets(const char* name) noexcept {
    const char* b = base_name(name);
    const size_t blen = std::strlen(b);
    const char* s = g_state.targets;
    while (*s != '\0') {
        while (*s == ' ' || *s == '\t') {
            ++s;
        }
        if (*s == '\0') {
            break;
        }
        const char* start = s;
        while (*s != '\0' && *s != ' ' && *s != '\t') {
            ++s;
        }
        const size_t len = static_cast<size_t>(s - start);
        if (len == blen && std::strncmp(b, start, len) == 0) {
            return true;
        }
    }
    return false;
}

// ---- /proc/self/maps permission snapshot (M3) ----
bool maps_prot_for(uintptr_t addr, int* prot_out) noexcept {
    const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    static char buf[65536];
    size_t total = 0;
    while (total + 1 < sizeof(buf)) {
        const ssize_t r = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (r <= 0) {
            break;
        }
        total += static_cast<size_t>(r);
    }
    close(fd);
    buf[total] = '\0';

    const char* p = buf;
    while (*p != '\0') {
        uintptr_t start = 0;
        while (*p != '\0' && *p != '-') {
            const char c = *p;
            if (c >= '0' && c <= '9') {
                start = start * 16u + static_cast<uintptr_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                start = start * 16u + static_cast<uintptr_t>(c - 'a' + 10);
            }
            ++p;
        }
        if (*p == '-') {
            ++p;
        }
        uintptr_t end = 0;
        while (*p != '\0' && *p != ' ') {
            const char c = *p;
            if (c >= '0' && c <= '9') {
                end = end * 16u + static_cast<uintptr_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                end = end * 16u + static_cast<uintptr_t>(c - 'a' + 10);
            }
            ++p;
        }
        while (*p == ' ') {
            ++p;
        }
        char perms[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4 && p[i] != '\0'; ++i) {
            perms[i] = p[i];
        }
        if (addr >= start && addr < end) {
            int prot = 0;
            if (perms[0] == 'r') prot |= PROT_READ;
            if (perms[1] == 'w') prot |= PROT_WRITE;
            if (perms[2] == 'x') prot |= PROT_EXEC;
            *prot_out = prot;
            return true;
        }
        while (*p != '\0' && *p != '\n') {
            ++p;
        }
        if (*p == '\n') {
            ++p;
        }
    }
    return false;
}

uintptr_t page_align(uintptr_t addr) noexcept {
    const long ps = sysconf(_SC_PAGESIZE);
    const uintptr_t page_size = (ps > 0) ? static_cast<uintptr_t>(ps) : 4096u;
    return addr & ~(page_size - 1);
}

uintptr_t page_size_bytes() noexcept {
    const long ps = sysconf(_SC_PAGESIZE);
    return (ps > 0) ? static_cast<uintptr_t>(ps) : 4096u;
}

bool page_make_writable(uintptr_t addr, int* orig_prot) noexcept {
    int prot = 0;
    if (!maps_prot_for(addr, &prot)) {
        prot = PROT_READ | PROT_WRITE;
    }
    if (mprotect(reinterpret_cast<void*>(page_align(addr)), static_cast<size_t>(page_size_bytes()),
                 PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    *orig_prot = prot;
    return true;
}

void page_restore(uintptr_t addr, int orig_prot) noexcept {
    (void)mprotect(reinterpret_cast<void*>(page_align(addr)), static_cast<size_t>(page_size_bytes()),
                   orig_prot);
}

// ---- object/slot bookkeeping ----
ObjectEntry* find_or_create_object(uintptr_t base, const char* name) noexcept {
    const char* b = base_name(name);
    for (ObjectEntry& o : g_objects) {
        if (o.used && o.base == base && std::strcmp(o.so, b) == 0) {
            return &o;
        }
    }
    for (ObjectEntry& o : g_objects) {
        if (!o.used) {
            o = ObjectEntry{};
            o.used = true;
            o.base = base;
            o.obj = static_cast<uint16_t>(&o - g_objects);
            copy_bounded(o.so, sizeof(o.so), b);
            make_module_label(name, o.module, sizeof(o.module));
            ModnetCounters* ctr = modnet_counters(o.obj);
            if (ctr != nullptr) {
                ctr->reset();
            }
            return &o;
        }
    }
    set_error("object table overflow");
    return nullptr;
}

ObjectEntry* find_object(uintptr_t base) noexcept {
    for (ObjectEntry& o : g_objects) {
        if (o.used && o.base == base) {
            return &o;
        }
    }
    return nullptr;
}

PatchedSlot* find_or_create_slot(uintptr_t slot) noexcept {
    for (PatchedSlot& s : g_slots) {
        if (s.used && s.slot == slot) {
            return &s;
        }
    }
    for (PatchedSlot& s : g_slots) {
        if (!s.used) {
            s = PatchedSlot{};
            s.used = true;
            s.slot = slot;
            return &s;
        }
    }
    set_error("slot table overflow");
    return nullptr;
}

// ---- dynamic section parsing ----
struct DynInfo {
    const Elf32_Sym* symtab = nullptr;
    const char*      strtab = nullptr;
    size_t           strsz = 0;
    const Elf32_Rel* jmprel = nullptr;
    size_t           jmprelsz = 0;
    const Elf32_Rel* rel = nullptr;
    size_t           relsz = 0;
    bool             bind_now = false;
    bool             valid = false;
};

// Section pointers in the loaded PT_DYNAMIC are already relocated by the
// dynamic loader (verified empirically); relocation offsets are file offsets,
// so a GOT slot is base + r_offset.
void parse_dynamic(uintptr_t base, const struct dl_phdr_info* info, DynInfo& di) noexcept {
    const Elf32_Phdr* phdr = info->dlpi_phdr;
    const Elf32_Dyn* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const Elf32_Dyn*>(base + phdr[i].p_vaddr);
            break;
        }
    }
    if (dyn == nullptr) {
        return;
    }
    for (const Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_SYMTAB: di.symtab = reinterpret_cast<const Elf32_Sym*>(d->d_un.d_ptr); break;
            case DT_STRTAB: di.strtab = reinterpret_cast<const char*>(d->d_un.d_ptr); break;
            case DT_STRSZ: di.strsz = static_cast<size_t>(d->d_un.d_val); break;
            case DT_JMPREL: di.jmprel = reinterpret_cast<const Elf32_Rel*>(d->d_un.d_ptr); break;
            case DT_PLTRELSZ: di.jmprelsz = static_cast<size_t>(d->d_un.d_val); break;
            case DT_REL: di.rel = reinterpret_cast<const Elf32_Rel*>(d->d_un.d_ptr); break;
            case DT_RELSZ: di.relsz = static_cast<size_t>(d->d_un.d_val); break;
            case DT_BIND_NOW: di.bind_now = true; break;
            case DT_FLAGS:
                if ((d->d_un.d_val & DF_BIND_NOW) != 0u) {
                    di.bind_now = true;
                }
                break;
            default: break;
        }
    }
    di.valid = (di.symtab != nullptr) && (di.strtab != nullptr) && (di.strsz > 0);
}

size_t symbol_bound(const DynInfo& di) noexcept {
    size_t bound = 1u << 20;
    const uintptr_t s = reinterpret_cast<uintptr_t>(di.symtab);
    const uintptr_t t = reinterpret_cast<uintptr_t>(di.strtab);
    if (t > s) {
        const size_t est = static_cast<size_t>((t - s) / sizeof(Elf32_Sym));
        if (est > 0 && est < bound) {
            bound = est;
        }
    }
    return bound;
}

const char* reloc_name(const DynInfo& di, unsigned sym) noexcept {
    const Elf32_Sym& se = di.symtab[sym];
    if (se.st_name == 0 || static_cast<size_t>(se.st_name) >= di.strsz) {
        return nullptr;
    }
    return di.strtab + se.st_name;
}

void process_rel(const DynInfo& di, const Elf32_Rel* rels, size_t bytes, uintptr_t base,
                 const ObjectEntry& obj, size_t symcnt) noexcept {
    if (rels == nullptr || bytes < sizeof(Elf32_Rel)) {
        return;
    }
    const size_t n = bytes / sizeof(Elf32_Rel);
    for (size_t k = 0; k < n; ++k) {
        const Elf32_Rel& r = rels[k];
        const unsigned type = ELF32_R_TYPE(r.r_info);
        if (type != R_386_JMP_SLOT && type != R_386_GLOB_DAT) {
            continue;
        }
        const unsigned sym = ELF32_R_SYM(r.r_info);
        if (sym >= symcnt) {
            continue;
        }
        const char* nm = reloc_name(di, sym);
        if (nm == nullptr) {
            continue;
        }
        const int id = sym_index(nm);
        if (id < 0) {
            continue;
        }
        const uintptr_t orig = original_for(id);
        if (orig == 0) {
            continue;
        }
        PatchedSlot* ps = find_or_create_slot(base + r.r_offset);
        if (ps == nullptr) {
            continue;
        }
        ps->obj = obj.obj;
        ps->sym = id;
        ps->original = orig;
        ps->wrapper = reinterpret_cast<uintptr_t>(g_syms[id].wrapper);
        ps->base = base;
        ps->live = true;
    }
}

void mark_object_slots(const struct dl_phdr_info* info, ObjectEntry& obj, uintptr_t base) noexcept {
    DynInfo di;
    parse_dynamic(base, info, di);
    if (!di.valid) {
        return;
    }
    if (!di.bind_now) {
        g_state.lazy_bound = true;
    }
    const size_t symcnt = symbol_bound(di);
    process_rel(di, di.jmprel, di.jmprelsz, base, obj, symcnt);
    process_rel(di, di.rel, di.relsz, base, obj, symcnt);
}

int scan_cb(struct dl_phdr_info* info, size_t, void*) {
    const char* name = info->dlpi_name;
    const uintptr_t base = reinterpret_cast<uintptr_t>(info->dlpi_addr);
    if (is_hard_excluded(name, base) || !basename_in_targets(name)) {
        return 0;
    }
    ObjectEntry* obj = find_or_create_object(base, name);
    if (obj == nullptr) {
        return 0;
    }
    obj->seen = true;
    mark_object_slots(info, *obj, base);
    return 0;
}

void scan_objects() noexcept {
    for (PatchedSlot& s : g_slots) {
        if (s.used) {
            s.live = false;
        }
    }
    for (ObjectEntry& o : g_objects) {
        if (o.used) {
            o.seen = false;
        }
    }
    g_state.lazy_bound = false;
    dl_iterate_phdr(&scan_cb, nullptr);
}

// ---- patch / restore ----
bool patch_one(PatchedSlot& s) noexcept {
    int orig_prot = 0;
    if (!page_make_writable(s.slot, &orig_prot)) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<uintptr_t*>(s.slot), s.wrapper, __ATOMIC_SEQ_CST);
    page_restore(s.slot, orig_prot);
    return true;
}

size_t patch_pending_slots() noexcept {
    size_t n = 0;
    for (PatchedSlot& s : g_slots) {
        if (s.used && s.live && !s.active) {
            if (patch_one(s)) {
                s.active = true;
                ++n;
            } else {
                set_error("mprotect denied");
            }
        }
    }
    return n;
}

struct SlotCheck {
    uintptr_t base;
    uintptr_t slot;
    bool      found;
    bool      valid;
};

int slot_check_cb(struct dl_phdr_info* info, size_t, void* data) {
    SlotCheck* sc = static_cast<SlotCheck*>(data);
    if (reinterpret_cast<uintptr_t>(info->dlpi_addr) != sc->base) {
        return 0;
    }
    sc->found = true;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const Elf32_Phdr& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        const uintptr_t lo = sc->base + ph.p_vaddr;
        const uintptr_t hi = lo + ph.p_memsz;
        if (sc->slot >= lo && sc->slot + sizeof(uintptr_t) <= hi) {
            sc->valid = true;
            break;
        }
    }
    return 1;
}

bool slot_mapped(uintptr_t base, uintptr_t slot) noexcept {
    SlotCheck sc{base, slot, false, false};
    dl_iterate_phdr(&slot_check_cb, &sc);
    return sc.found && sc.valid;
}

bool restore_one(PatchedSlot& s) noexcept {
    if (!slot_mapped(s.base, s.slot)) {
        s.active = false;
        return false;
    }
    const uintptr_t cur = __atomic_load_n(reinterpret_cast<uintptr_t*>(s.slot), __ATOMIC_SEQ_CST);
    if (cur != s.wrapper) {
        s.active = false; // already restored or foreign value (compare-then-store, R2)
        return false;
    }
    int orig_prot = 0;
    if (!page_make_writable(s.slot, &orig_prot)) {
        return false; // fail-closed: leave the wrapper in place
    }
    __atomic_store_n(reinterpret_cast<uintptr_t*>(s.slot), s.original, __ATOMIC_SEQ_CST);
    page_restore(s.slot, orig_prot);
    s.active = false;
    return true;
}

size_t restore_all_slots() noexcept {
    size_t n = 0;
    for (PatchedSlot& s : g_slots) {
        if (s.used && s.active && restore_one(s)) {
            ++n;
        }
    }
    g_state.restored_slots += static_cast<uint32_t>(n);
    return n;
}

void reverify_slots() noexcept {
    for (PatchedSlot& s : g_slots) {
        if (!s.used || !s.live || !s.active) {
            continue;
        }
        const uintptr_t cur = __atomic_load_n(reinterpret_cast<uintptr_t*>(s.slot), __ATOMIC_SEQ_CST);
        if (cur != s.wrapper) {
            if (g_state.conflicts != 0xFFFFFFFFu) {
                ++g_state.conflicts;
            }
            if (!patch_one(s)) {
                s.active = false;
                set_error("mprotect denied");
            }
        }
    }
}

// ---- range cache publication ----
struct RangeCtx {
    RangeCache* rc;
};

int range_cb(struct dl_phdr_info* info, size_t, void* data) {
    RangeCtx* ctx = static_cast<RangeCtx*>(data);
    const uintptr_t base = reinterpret_cast<uintptr_t>(info->dlpi_addr);
    ObjectEntry* obj = find_object(base);
    if (obj == nullptr || obj->live_slots == 0) {
        return 0;
    }
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const Elf32_Phdr& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) {
            continue;
        }
        const uintptr_t lo = base + ph.p_vaddr;
        const uintptr_t hi = lo + ph.p_memsz;
        (void)ctx->rc->add(lo, hi, static_cast<int16_t>(obj->obj));
    }
    return 0;
}

void rebuild_ranges() noexcept {
    RangeCache& rc = g_range_buf[g_range_next];
    rc.reset();
    RangeCtx ctx{&rc};
    dl_iterate_phdr(&range_cb, &ctx);
    if (rc.overflowed()) {
        set_error("range cache overflow");
    }
    modnet_ranges_publish(&rc);
    g_range_next ^= 1;
}

void recount_slots() noexcept {
    for (ObjectEntry& o : g_objects) {
        if (o.used) {
            o.live_slots = 0;
        }
    }
    for (PatchedSlot& s : g_slots) {
        if (s.used && s.active && s.obj < HLDSMON_MODNET_MAX_OBJECTS) {
            ++g_objects[s.obj].live_slots;
        }
    }
}

uint32_t count_active_slots() noexcept {
    uint32_t n = 0;
    for (const PatchedSlot& s : g_slots) {
        if (s.used && s.active) {
            ++n;
        }
    }
    return n;
}

uint32_t count_objects_with_slots() noexcept {
    uint32_t n = 0;
    for (const ObjectEntry& o : g_objects) {
        if (o.used && o.live_slots > 0) {
            ++n;
        }
    }
    return n;
}

int count_targets(const char* s) noexcept {
    int n = 0;
    bool in = false;
    for (; *s != '\0'; ++s) {
        if (*s == ' ' || *s == '\t') {
            in = false;
        } else if (!in) {
            in = true;
            ++n;
        }
    }
    return n;
}

// ---- probe helpers ----
// The probe attributes the wrapper's caller return address to a range faked
// around these helpers, which requires a real `call`. GCC -O2 may emit a
// sibling tail call (`jmp *%reg`), handing the wrapper the helper's caller
// instead — outside the range — so the volatile asm keeps the result live to
// forbid that. (Regression found by the M-P5 harness under GCC 10.)
__attribute__((noinline)) ssize_t probe_call_send(ssize_t (*fn)(int, const void*, size_t, int),
                                                  int fd, const char* b, size_t n) {
    ssize_t r = fn(fd, b, n, 0);
    __asm__ __volatile__("" : "+r"(r));
    return r;
}

__attribute__((noinline)) ssize_t probe_call_recv(ssize_t (*fn)(int, void*, size_t, int),
                                                  int fd, void* b, size_t n) {
    ssize_t r = fn(fd, b, n, 0);
    __asm__ __volatile__("" : "+r"(r));
    return r;
}

bool run_roundtrip_test() noexcept {
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return false;
    }
    uintptr_t send_w = 0;
    uintptr_t recv_w = 0;
    PatchedSlot* send_slot = nullptr;
    PatchedSlot* recv_slot = nullptr;
    for (PatchedSlot& s : g_slots) {
        if (!s.used || !s.live || !s.active) {
            continue;
        }
        if (std::strcmp(g_syms[s.sym].name, "send") == 0) {
            send_w = s.wrapper;
            send_slot = &s;
        } else if (std::strcmp(g_syms[s.sym].name, "recv") == 0) {
            recv_w = s.wrapper;
            recv_slot = &s;
        }
    }
    ModnetCounters* scratch = modnet_counters(kProbeObj);
    if (send_w == 0 || recv_w == 0 || scratch == nullptr) {
        close(sv[0]);
        close(sv[1]);
        return false;
    }
    scratch->reset();

    const char msg[] = "hldsmon-probe";
    char rbuf[sizeof(msg)];
    std::memset(rbuf, 0, sizeof(rbuf));

    auto send_fn = reinterpret_cast<ssize_t (*)(int, const void*, size_t, int)>(send_w);
    auto recv_fn = reinterpret_cast<ssize_t (*)(int, void*, size_t, int)>(recv_w);

    const ssize_t s1 = probe_call_send(send_fn, sv[0], msg, sizeof(msg));
    const ssize_t r1 = probe_call_recv(recv_fn, sv[1], rbuf, sizeof(rbuf));
    const ssize_t s2 = probe_call_send(send_fn, sv[0], msg, sizeof(msg));
    const ssize_t r2 = probe_call_recv(recv_fn, sv[1], rbuf, sizeof(rbuf));

    const uintptr_t slot_send = __atomic_load_n(reinterpret_cast<uintptr_t*>(send_slot->slot), __ATOMIC_SEQ_CST);
    const uintptr_t slot_recv = __atomic_load_n(reinterpret_cast<uintptr_t*>(recv_slot->slot), __ATOMIC_SEQ_CST);

    ModnetSnapshot snap;
    modnet_snapshot(*scratch, snap);

    close(sv[0]);
    close(sv[1]);

    const size_t mlen = sizeof(msg);
    const bool rt_ok = s1 == static_cast<ssize_t>(mlen) && r1 == static_cast<ssize_t>(mlen) &&
                       s2 == static_cast<ssize_t>(mlen) && r2 == static_cast<ssize_t>(mlen) &&
                       std::memcmp(rbuf, msg, mlen) == 0;
    const bool counts_ok = snap.out_bytes == static_cast<uint64_t>(mlen) * 2u &&
                           snap.in_bytes == static_cast<uint64_t>(mlen) * 2u;
    const bool c1_ok = slot_send == send_w && slot_recv == recv_w;
    return rt_ok && counts_ok && c1_ok;
}

} // namespace

// ---- wrappers (INV-M1: real call, real return, errno preserved) ----
ssize_t wrap_send(int fd, const void* buf, size_t len, int flags) {
    const ssize_t ret = g_orig_send(fd, buf, len, flags);
    const int e = errno;
    if (ret >= 0) {
        const int16_t obj = modnet_ranges_lookup(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        if (obj >= 0) {
            ModnetCounters* c = modnet_counters(static_cast<uint16_t>(obj));
            if (c != nullptr) {
                c->add_out(static_cast<uint64_t>(ret));
            }
        }
    }
    errno = e;
    return ret;
}

ssize_t wrap_recv(int fd, void* buf, size_t len, int flags) {
    const ssize_t ret = g_orig_recv(fd, buf, len, flags);
    const int e = errno;
    if (ret >= 0) {
        const int16_t obj = modnet_ranges_lookup(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        if (obj >= 0) {
            ModnetCounters* c = modnet_counters(static_cast<uint16_t>(obj));
            if (c != nullptr) {
                c->add_in(static_cast<uint64_t>(ret));
            }
        }
    }
    errno = e;
    return ret;
}

ssize_t wrap_sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* addr, socklen_t alen) {
    const ssize_t ret = g_orig_sendto(fd, buf, len, flags, addr, alen);
    const int e = errno;
    if (ret >= 0) {
        const int16_t obj = modnet_ranges_lookup(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        if (obj >= 0) {
            ModnetCounters* c = modnet_counters(static_cast<uint16_t>(obj));
            if (c != nullptr) {
                c->add_out(static_cast<uint64_t>(ret));
            }
        }
    }
    errno = e;
    return ret;
}

ssize_t wrap_recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* addr, socklen_t* alen) {
    const ssize_t ret = g_orig_recvfrom(fd, buf, len, flags, addr, alen);
    const int e = errno;
    if (ret >= 0) {
        const int16_t obj = modnet_ranges_lookup(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        if (obj >= 0) {
            ModnetCounters* c = modnet_counters(static_cast<uint16_t>(obj));
            if (c != nullptr) {
                c->add_in(static_cast<uint64_t>(ret));
            }
        }
    }
    errno = e;
    return ret;
}

ssize_t wrap_sendmsg(int fd, const struct msghdr* msg, int flags) {
    const ssize_t ret = g_orig_sendmsg(fd, msg, flags);
    const int e = errno;
    if (ret >= 0) {
        const int16_t obj = modnet_ranges_lookup(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        if (obj >= 0) {
            ModnetCounters* c = modnet_counters(static_cast<uint16_t>(obj));
            if (c != nullptr) {
                c->add_out(static_cast<uint64_t>(ret));
            }
        }
    }
    errno = e;
    return ret;
}

ssize_t wrap_recvmsg(int fd, struct msghdr* msg, int flags) {
    const ssize_t ret = g_orig_recvmsg(fd, msg, flags);
    const int e = errno;
    if (ret >= 0) {
        const int16_t obj = modnet_ranges_lookup(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        if (obj >= 0) {
            ModnetCounters* c = modnet_counters(static_cast<uint16_t>(obj));
            if (c != nullptr) {
                c->add_in(static_cast<uint64_t>(ret));
            }
        }
    }
    errno = e;
    return ret;
}

int wrap_connect(int fd, const struct sockaddr* addr, socklen_t len) {
    const int ret = g_orig_connect(fd, addr, len);
    const int e = errno;
    const int16_t obj = modnet_ranges_lookup(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (obj >= 0) {
        ModnetCounters* c = modnet_counters(static_cast<uint16_t>(obj));
        if (c != nullptr) {
            c->add_connect_attempt();
            switch (modnet_classify_connect(ret, e)) {
                case ConnectClass::Blocking: c->add_conn(); break;
                case ConnectClass::Pending: break;
                case ConnectClass::Failed: c->add_conn_fail(); break;
            }
        }
    }
    errno = e;
    return ret;
}

int wrap_accept(int fd, struct sockaddr* addr, socklen_t* len) {
    const int ret = g_orig_accept(fd, addr, len);
    const int e = errno;
    if (ret >= 0) {
        const int16_t obj = modnet_ranges_lookup(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        if (obj >= 0) {
            ModnetCounters* c = modnet_counters(static_cast<uint16_t>(obj));
            if (c != nullptr) {
                c->add_conn();
            }
        }
    }
    errno = e;
    return ret;
}

int wrap_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res) {
    const int ret = g_orig_getaddrinfo(node, service, hints, res);
    const int e = errno;
    const int16_t obj = modnet_ranges_lookup(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (obj >= 0) {
        ModnetCounters* c = modnet_counters(static_cast<uint16_t>(obj));
        if (c != nullptr) {
            c->add_dns_call();
        }
    }
    errno = e;
    return ret;
}

// ---- public adapter API ----
const char* modnet_got_builtin_targets() noexcept {
    return "easy_http_amxx_i386.so sockets_amxx_i386.so";
}

void modnet_got_configure_targets(const char* space_separated) noexcept {
    const bool empty = (space_separated == nullptr || space_separated[0] == '\0');
    copy_bounded(g_state.targets, sizeof(g_state.targets),
                 empty ? modnet_got_builtin_targets() : space_separated);
    g_state.target_count = count_targets(g_state.targets);
}

bool modnet_probe() noexcept {
    ensure_self_base();
    g_state.probe_ok = false;
    g_state.last_error[0] = '\0';
    resolve_originals();

    scan_objects();

    bool have_send = false;
    bool have_recv = false;
    for (const PatchedSlot& s : g_slots) {
        if (!s.used || !s.live) {
            continue;
        }
        if (std::strcmp(g_syms[s.sym].name, "send") == 0) {
            have_send = true;
        } else if (std::strcmp(g_syms[s.sym].name, "recv") == 0) {
            have_recv = true;
        }
    }
    if (!have_send || !have_recv || g_orig_send == nullptr || g_orig_recv == nullptr) {
        set_error("relocations not found");
        return false;
    }

    bool mprotect_ok = true;
    for (PatchedSlot& s : g_slots) {
        if (!s.used || !s.live) {
            continue;
        }
        if (!patch_one(s)) {
            mprotect_ok = false;
            break;
        }
        s.active = true;
    }
    if (!mprotect_ok) {
        restore_all_slots();
        set_error("mprotect denied");
        return false;
    }

    static RangeCache probe_rc;
    probe_rc.reset();
    const uintptr_t send_page = reinterpret_cast<uintptr_t>(&probe_call_send) & ~static_cast<uintptr_t>(0xFFF);
    const uintptr_t recv_page = reinterpret_cast<uintptr_t>(&probe_call_recv) & ~static_cast<uintptr_t>(0xFFF);
    (void)probe_rc.add(send_page, send_page + 0x2000, static_cast<int16_t>(kProbeObj));
    (void)probe_rc.add(recv_page, recv_page + 0x2000, static_cast<int16_t>(kProbeObj));
    modnet_ranges_publish(&probe_rc);

    const bool ok = run_roundtrip_test();

    modnet_ranges_publish(nullptr);
    ModnetCounters* scratch = modnet_counters(kProbeObj);
    if (scratch != nullptr) {
        scratch->reset();
    }
    restore_all_slots();
    recount_slots();

    g_state.probe_ok = ok;
    if (!ok) {
        set_error("round-trip failed");
    }
    return ok;
}

void modnet_enable() noexcept {
    if (g_state.enabled) {
        return;
    }
    ensure_self_base();
    if (!modnet_probe()) {
        g_state.enabled = false;
        return;
    }
    scan_objects();
    (void)patch_pending_slots();
    recount_slots();
    rebuild_ranges();
    g_state.enabled = true;
    g_state.patched_slots = count_active_slots();
    g_state.patched_objects = count_objects_with_slots();
}

void modnet_disable() noexcept {
    (void)restore_all_slots();
    recount_slots();
    modnet_ranges_publish(nullptr);
    g_state.enabled = false;
    g_state.patched_slots = 0;
    g_state.patched_objects = 0;
}

void modnet_rescan() noexcept {
    if (!g_state.enabled) {
        return;
    }
    ensure_self_base();
    scan_objects();
    reverify_slots();
    (void)patch_pending_slots();
    recount_slots();
    rebuild_ranges();
    g_state.patched_slots = count_active_slots();
    g_state.patched_objects = count_objects_with_slots();
}

void modnet_request_rescan() noexcept {
    g_rescan_req.store(true, std::memory_order_release);
}

bool modnet_rescan_requested() noexcept {
    return g_rescan_req.exchange(false, std::memory_order_acq_rel);
}

ModnetState modnet_got_status() noexcept {
    recount_slots();
    g_state.patched_slots = count_active_slots();
    g_state.patched_objects = count_objects_with_slots();
    g_state.target_count = count_targets(g_state.targets);
    return g_state;
}

size_t modnet_got_collect_emit(std::span<ModnetEmitItem> out, double now) noexcept {
    if (out.empty()) {
        return 0;
    }
    const size_t cap = out.size();
    size_t n = 0;
    for (ObjectEntry& o : g_objects) {
        if (!o.used || o.live_slots == 0 || n >= cap) {
            continue;
        }
        ModnetCounters* ctr = modnet_counters(o.obj);
        if (ctr == nullptr) {
            continue;
        }
        ModnetEmitItem& it = out[n++];
        copy_bounded(it.so, sizeof(it.so), o.so);
        copy_bounded(it.module, sizeof(it.module), o.module);
        modnet_snapshot(*ctr, it.cur);
        it.prev = o.has_prev ? o.prev : ModnetSnapshot{};
        it.dt = o.has_prev ? (now - o.prev_t) : 0.0;
        o.prev = it.cur;
        o.prev_t = now;
        o.has_prev = true;
    }
    return n;
}

} // namespace hldsmon
