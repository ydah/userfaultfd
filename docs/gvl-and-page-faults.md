# GVL and userfaultfd page faults

## Why the naive Ruby callback deadlocks

The kernel suspends a thread that faults on a registered page until another thread resolves the event. If ordinary Ruby code performs that access through a raw pointer, the suspended thread still owns the GVL:

1. Ruby thread A touches the registered address while holding the GVL.
2. The kernel suspends A and publishes `UFFD_EVENT_PAGEFAULT`.
3. Handler thread B reads the event.
4. B needs the GVL before it can invoke the Ruby block.
5. A cannot release the GVL until the fault is resolved, while B cannot resolve it until A releases the GVL.

`spec/system/userfaultfd_spec.rb` fixes this behavior as an intentional test: a child process performs a `Fiddle::Pointer` read, the test confirms it remains blocked for two seconds, then sends `SIGKILL`. The timeout and process isolation prevent a failed test from hanging the suite.

## Arrangement B: GVL-free memory access

`Region#read` and `Region#write` copy memory inside `rb_thread_call_without_gvl`. A native event-reader thread moves kernel messages into a pipe. The Ruby handler can therefore acquire the GVL, create `Fault` objects, and call `UFFDIO_COPY` or `UFFDIO_ZEROPAGE`.

The resolution ioctls also run without the GVL. Strings passed to `Fault#copy` are copied into C-owned memory first, so Ruby GC cannot move or free the source while the ioctl is running.

## Arrangement A: process separation and fork events

Fork handling has an extra cycle: `EVENT_FORK` must be read before `fork(2)` completes, but the Ruby thread invoking `fork` holds the GVL. A Ruby event-reader thread therefore cannot make progress.

The extension uses a C event-reader thread that never enters Ruby while polling the parent and child descriptors. It reads `EVENT_FORK`, starts monitoring the child fd, and writes fixed-size records to a pipe. Once `fork` returns and Ruby can schedule again, the handler converts those records into `ForkEvent` and `Fault` objects.

This requires `UFFD_FEATURE_EVENT_FORK`, which the kernel restricts with `CAP_SYS_PTRACE`. `UserfaultFD.new` tries to enable it and falls back to a descriptor without fork events on `EPERM`; callers must check `enabled_features` before using the fork arrangement.

## Arrangement C: native handlers

The native modes resolve missing faults entirely in a pthread:

- `zero_fill` issues `UFFDIO_ZEROPAGE`.
- `backing_file` uses `pread` and `UFFDIO_COPY`.
- `prefilled` copies from a second mmap-backed `Region`.

No Ruby callback or Ruby-managed buffer is involved. These modes are the appropriate choice for fault rates that a Ruby callback cannot sustain.

## Boundaries

- Do not register the Ruby heap.
- Do not dereference `Region#to_ptr` from Ruby while using a Ruby handler.
- A handler monitors at most 64 fork descendants. An epoll-backed dynamic set is the upgrade path if external process fan-out exceeds that ceiling.
- Native handlers currently service the descriptor on which they started; use the Ruby handler/event reader for fork descendants.
- Always stop handlers before closing descriptors or unmapping regions.

## Permission survey

`vm.unprivileged_userfaultfd` is a host-kernel setting, not a reliable property of the userspace distribution. Current [upstream kernel documentation](https://www.kernel.org/doc/html/latest/admin-guide/sysctl/vm.html#unprivileged-userfaultfd) and [Debian's packaged kernel documentation](https://sources.debian.org/src/linux/6.12.96-1/Documentation/admin-guide/sysctl/vm.rst) document a default of `0`: unprivileged callers must use `UFFD_USER_MODE_ONLY`. Ubuntu's [userfaultfd man page](https://manpages.ubuntu.com/manpages/jammy/man2/userfaultfd.2.html) likewise documents `EPERM` when the value is `0` and the caller lacks `CAP_SYS_PTRACE`.

Cloud images, administrators, and container hosts can override that value, and containers observe the host kernel rather than the image's `/etc/os-release`. The gem therefore probes the real syscall instead of guessing from a distribution name. Diagnose a target with:

```sh
cat /proc/sys/vm/unprivileged_userfaultfd
grep USERFAULTFD /boot/config-"$(uname -r)" 2>/dev/null
```

On Linux 5.11 and newer, the default `user_mode_only: true` remains usable when the sysctl is `0`. Linux 4.11–5.10 requires the explicitly less restrictive `user_mode_only: false` and whatever privilege or sysctl policy that kernel enforces.
