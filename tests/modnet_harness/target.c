// HldsMonitoring — synthetic 32-bit target object for the GOT/PLT harness
// (plan M-P5). Built twice: lazy (default binding) and BIND_NOW (-Wl,-z,now).
//
// Every exported function forwards to a libc socket call through the PLT/GOT
// (default; NOT -fno-plt), so the object carries R_386_JMP_SLOT relocations for
// send/recv/connect/accept/getaddrinfo. The compiler is told
// -fno-optimize-sibling-calls so each forwarding is a real `call` (not a tail
// jump) — otherwise the interposer's caller-return-address attribution would
// observe the harness frame instead of this object's frame.

#define _GNU_SOURCE

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

ssize_t modnet_target_send(int fd, const void* buf, size_t len) {
    ssize_t n = send(fd, buf, len, 0);
    return n;
}

ssize_t modnet_target_recv(int fd, void* buf, size_t len) {
    ssize_t n = recv(fd, buf, len, 0);
    return n;
}

int modnet_target_connect(int fd, const struct sockaddr* addr, socklen_t len) {
    int r = connect(fd, addr, len);
    return r;
}

int modnet_target_accept(int fd, struct sockaddr* addr, socklen_t* len) {
    int r = accept(fd, addr, len);
    return r;
}

int modnet_target_getaddrinfo(const char* node, const char* service,
                              const struct addrinfo* hints, struct addrinfo** res) {
    int r = getaddrinfo(node, service, hints, res);
    return r;
}
