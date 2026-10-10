# modnet harness — 32-bit synthetic GOT/PLT test

Deterministic end-to-end check of the `modnet` GOT/PLT interposer on a
**synthetic 32-bit `.so`** — no ReHLDS, no network egress (socketpair/loopback
only). It closes the gap that 64-bit host tests cannot cover: real GOT
patching, lazy (`-z lazy`) binding, restore, rescan and a fail-closed probe.

## What it builds

- `target_lazy.so` — `target.c` built `-shared -fPIC -m32` with **default lazy
  binding**; exports `modnet_target_{send,recv,connect,accept,getaddrinfo}`,
  each forwarding to the real libc call through the PLT/GOT
  (`R_386_JUMP_SLOT`).
- `target_now.so` — same source built with `-Wl,-z,now` (BIND_NOW). Used to
  prove a newly `dlopen`ed target is picked up on rescan.
- `harness` — 32-bit driver linking **only** `src/modnet.cpp` +
  `src/modnet_got.cpp` (`-ldl`), no engine header.

`target.c` is compiled with `-fno-optimize-sibling-calls` so each forwarding is
a real `call` (not a tail `jmp`); otherwise the interposer's caller-return-
address attribution would see the harness frame instead of the target object.

## What it asserts

| Check | Scenario |
|---|---|
| attribution (QAS-M5) | bytes sent/received via the target are counted against the target object's counter (`.so` label), not libc |
| lazy / C1 (QAS-M9) | lazy target is patched; ≥2 consecutive calls keep incrementing (no one-shot `_dl_runtime_resolve` clobber) |
| restore-on-disable (QAS-M3) | after `modnet_disable()` the slot is restored and the counter stops growing |
| rescan (QAS-M7) | a target `dlopen`ed after `modnet_enable()` is picked up by `modnet_rescan()` and starts being counted |
| fail-closed (QAS-M2) | no matching relocations ⇒ `probe_ok=false`, stays OFF, no crash |
| R1 | non-blocking `connect` (`EINPROGRESS`) = pending (`connect_attempts` only); blocking success ⇒ `conns`; refusal ⇒ `conn_fail`; `accept` ⇒ `conns`; `getaddrinfo` ⇒ `dns_calls` (no bytes) |
| non-interference (QAS-M1) | return value + `errno` identical with modnet ON vs OFF (incl. `EAGAIN` on a non-blocking fd) |

The process exits non-zero if any assertion fails.

## Run

Needs a working i686 toolchain (`gcc/g++` multilib):

```bash
make test-modnet-harness
```

On hosts without multilib, run it inside the same i686 image CI uses:

```bash
docker run --rm -v "$PWD":/src -w /src quay.io/pypa/manylinux2014_i686 \
  bash -lc 'make test-modnet-harness'
```

This target is intentionally **not** part of the host `make test` (it is
`-m32`-only) and is wired into the `plugin` CI job.
