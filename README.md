# userfaultfd

Ruby bindings for Linux `userfaultfd(2)`, with mmap-backed regions and handlers that avoid blocking Ruby's Global VM Lock (GVL).

## The deadlock rule

A Ruby thread must never touch registered memory through a raw pointer while a Ruby callback is expected to resolve the fault. The faulting thread keeps the GVL, so the callback cannot run.

Use one of these supported arrangements:

- `Region#read` / `Region#write`: the C extension releases the GVL around memory access.
- A forked faulting process: the parent callback has a separate GVL. `UFFD_FEATURE_EVENT_FORK` also requires `CAP_SYS_PTRACE`; check `enabled_features`.
- A native `:zero_fill`, `:backing_file`, or `:prefilled` handler: the handler never enters Ruby.

See [docs/gvl-and-page-faults.md](docs/gvl-and-page-faults.md) for the full failure sequence and implementation details.

## Requirements

- Linux 4.11 or newer with `CONFIG_USERFAULTFD`
- Ruby 3.2 or newer
- A C compiler; the Linux 4.11 baseline UAPI is bundled for build hosts without `linux/userfaultfd.h`
- Permission through `/dev/userfaultfd`, or the `userfaultfd(2)` syscall with `UFFD_USER_MODE_ONLY`

The gem tries `/dev/userfaultfd` first and falls back to the syscall. Requiring the gem succeeds on unsupported systems; `UserfaultFD.supported?` returns `false`, and opening a descriptor raises `UserfaultFD::UnsupportedError` or the relevant `Errno::*` permission error. Linux before 5.11 does not understand `UFFD_USER_MODE_ONLY`; an appropriately permitted caller can explicitly use `user_mode_only: false` with `.supported?`, `.features`, and `.new`. This opts into kernel-originated faults and is never automatic.

## Installation

```ruby
gem "userfaultfd"
```

Then run `bundle install`. The `userfaultfd` RubyGems name was unclaimed when this project was initialized; until it is published, use the repository source in your Gemfile.

## Native zero fill

This mode is safe even when the main Ruby thread faults:

```ruby
require "userfaultfd"

region = UserfaultFD::Region.new(size: 64 * 1024 * 1024)
uffd = UserfaultFD.new(features: [])
uffd.register(region, mode: :missing)
handler = uffd.start_handler(mode: :zero_fill)

data = region.read(0, region.size)
raise "not zero" unless data == "\0".b * region.size

handler.stop
uffd.close
region.unmap
```

## Ruby callback in one process

`Region#read` releases the GVL, allowing the handler's Ruby block to run:

```ruby
page_size = UserfaultFD::Region.allocate.page_size
region = UserfaultFD::Region.new(size: page_size)
uffd = UserfaultFD.new(features: [])
uffd.register(region, mode: :missing)
handler = uffd.start_handler do |event|
  event.copy("x" * region.page_size) if event.is_a?(UserfaultFD::Fault)
end

region.read(0, region.page_size) #=> "x" * page_size

handler.stop
uffd.close
region.unmap
```

## Forked faulting process

When permitted, `UserfaultFD.new` enables `EVENT_FORK` by default. A native event reader consumes the fork event without waiting for the GVL, then delivers `ForkEvent` and child faults to the Ruby handler.

```ruby
page_size = UserfaultFD::Region.allocate.page_size
region = UserfaultFD::Region.new(size: page_size)
uffd = UserfaultFD.new
raise "EVENT_FORK needs CAP_SYS_PTRACE" unless uffd.enabled_features.include?(:event_fork)

uffd.register(region, mode: :missing)
handler = uffd.start_handler do |event|
  case event
  when UserfaultFD::Fault
    event.copy("child".ljust(region.page_size, "\0"))
  when UserfaultFD::ForkEvent
    # The handler monitors and closes event.child_uffd automatically.
  end
end

pid = fork { exit!(region.read(0, 5) == "child" ? 0 : 1) }
Process.wait(pid)

handler.stop
uffd.close
region.unmap
```

## Other operations

- `Fault#zero`, `#copy`, `#wake`, `#continue`, `#poison`, and `#move`
- `UserfaultFD#writeprotect` with `mode: [:missing, :wp]`
- `Region#madvise(:dontneed)` to drop page-table entries and `:remove` to discard shared pages
- `mode: :backing_file, io: file` for native lazy file loading
- `mode: :prefilled, source: region` for native copies from another region
- `ForkEvent`, `RemapEvent`, `RemoveEvent`, and `UnmapEvent`
- `UserfaultFD.features` for kernel capabilities and `#enabled_features` for this descriptor

Optional ioctls raise `UnsupportedError` when they are absent from the build headers or running kernel.

## Unsupported and constrained environments

| Environment | Result |
|---|---|
| macOS, BSD, Windows | `require` works; `supported?` is `false` |
| Linux before 4.11 or without `CONFIG_USERFAULTFD` | Unsupported |
| Linux 4.11–5.10 | An appropriately permitted caller must explicitly pass `user_mode_only: false` |
| Docker with the default seccomp profile | The syscall is commonly blocked; allow `userfaultfd` or use an accessible `/dev/userfaultfd` |
| Docker Desktop / WSL2 | Depends on the VM kernel, config, device, and seccomp policy rather than the guest userspace alone |
| Linux with `vm.unprivileged_userfaultfd=0` | User-mode-only faults still work on kernels that honor `UFFD_USER_MODE_ONLY`; fork events require `CAP_SYS_PTRACE` |
| GitHub-hosted runners | Unit tests run; system tests skip when the host policy denies userfaultfd |
| Kernels before 6.6 / 6.8 | `POISON` / `MOVE` capabilities are absent |

Never register Ruby's object heap. Registered memory must come from `UserfaultFD::Region`; otherwise GC or ordinary Ruby code can fault while holding the GVL.

## Development

```sh
rake compile
rake test:unit
rake test:system
rbs -I sig validate
```

System examples use timeouts because an unresolved page fault intentionally blocks its faulting process. The kernel matrix runner is in `tools/vm`.

## License

MIT. See [LICENSE.txt](LICENSE.txt).
