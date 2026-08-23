<div align="center">
  <h1>userfaultfd</h1>
  <p><strong>Ruby bindings for Linux userfaultfd with GVL-safe page fault handling</strong></p>
  <p>
    <a href="https://rubygems.org/gems/userfaultfd"><img src="https://img.shields.io/gem/v/userfaultfd.svg?colorB=319e8c" alt="Gem Version"></a>
    <a href="https://rubygems.org/gems/userfaultfd"><img src="https://img.shields.io/gem/dt/userfaultfd.svg" alt="Downloads"></a>
    <a href="https://github.com/ydah/userfaultfd/actions/workflows/main.yml"><img src="https://github.com/ydah/userfaultfd/actions/workflows/main.yml/badge.svg" alt="CI"></a>
    <img src="https://img.shields.io/badge/ruby-%3E%3D%203.2-ruby.svg" alt="Ruby Version">
    <img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License">
  </p>
</div>

<p align="center">
  <a href="#features">Features</a> ·
  <a href="#installation">Installation</a> ·
  <a href="#quick-start">Quick Start</a> ·
  <a href="#handler-modes">Handler Modes</a> ·
  <a href="#safety-and-the-gvl">Safety</a> ·
  <a href="#development">Development</a>
</p>

---

`userfaultfd` is a C extension for handling Linux user-space page faults from Ruby. It provides mmap-backed regions, negotiates kernel capabilities, and supports both Ruby callbacks and native handlers without blocking Ruby's Global VM Lock (GVL).

## Features

- mmap-backed `Region` objects kept outside Ruby's object heap
- `/dev/userfaultfd` support with automatic syscall fallback
- GVL-safe memory access through `Region#read` and `Region#write`
- Ruby callback handlers for application-defined fault resolution
- Native zero-fill, backing-file, and prefilled-region handlers
- Missing, write-protect, and minor fault modes
- Fork, remap, remove, and unmap events
- Optional `UFFDIO_POISON` and `UFFDIO_MOVE` support
- Portable loading on unsupported platforms through `UserfaultFD.supported?`

## Installation

Add the gem to your Gemfile:

```ruby
gem "userfaultfd"
```

Then install dependencies:

```sh
bundle install
```

### Requirements

- Linux 4.11 or newer with `CONFIG_USERFAULTFD`
- Ruby 3.2 or newer
- A C compiler
- Access through `/dev/userfaultfd`, or permission to use the `userfaultfd(2)` syscall

The extension bundles the Linux 4.11 baseline UAPI for build hosts without `linux/userfaultfd.h`.

## Quick Start

The native zero-fill handler resolves faults without entering Ruby, so it is safe even when the main Ruby thread accesses the region:

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

## Handler Modes

| Mode | Usage | Fault resolution |
|---|---|---|
| Ruby callback | `start_handler { |event| ... }` | Application-defined `Fault#copy`, `#zero`, `#continue`, `#poison`, or `#move` |
| Zero fill | `start_handler(mode: :zero_fill)` | Native `UFFDIO_ZEROPAGE` |
| Backing file | `start_handler(mode: :backing_file, io: file)` | Native page-aligned reads from a file |
| Prefilled region | `start_handler(mode: :prefilled, source: region)` | Native copies from another `Region` |

### Ruby Callback

`Region#read` and `Region#write` release the GVL while accessing memory, allowing the Ruby handler to resolve the fault:

```ruby
page_size = UserfaultFD::Region.allocate.page_size
region = UserfaultFD::Region.new(size: page_size)
uffd = UserfaultFD.new(features: [])
uffd.register(region, mode: :missing)
handler = uffd.start_handler do |event|
  event.copy("x" * page_size) if event.is_a?(UserfaultFD::Fault)
end

region.read(0, page_size) #=> "x" * page_size

handler.stop
uffd.close
region.unmap
```

### Forked Faulting Process

When permitted, `UserfaultFD.new` enables `EVENT_FORK` by default. The parent handler can then resolve faults raised by a child process:

```ruby
page_size = UserfaultFD::Region.allocate.page_size
region = UserfaultFD::Region.new(size: page_size)
uffd = UserfaultFD.new
raise "EVENT_FORK needs CAP_SYS_PTRACE" unless uffd.enabled_features.include?(:event_fork)

uffd.register(region, mode: :missing)
handler = uffd.start_handler do |event|
  event.copy("child".ljust(page_size, "\0")) if event.is_a?(UserfaultFD::Fault)
end

pid = fork { exit!(region.read(0, 5) == "child" ? 0 : 1) }
Process.wait(pid)

handler.stop
uffd.close
region.unmap
```

## Safety and the GVL

Never access registered memory through a raw pointer while a Ruby callback is expected to resolve the fault. The faulting thread holds the GVL, so the callback cannot run and both threads deadlock.

Use one of these supported arrangements:

- Access memory through `Region#read` or `Region#write`, which release the GVL.
- Fault in a forked child while the parent runs the callback. This requires `EVENT_FORK` and `CAP_SYS_PTRACE`.
- Use a native `:zero_fill`, `:backing_file`, or `:prefilled` handler.

Never register Ruby's object heap. Registered memory must come from `UserfaultFD::Region` so Ruby's GC cannot fault while holding the GVL.

See [GVL and page faults](docs/gvl-and-page-faults.md) for the full deadlock sequence, implementation details, and permission model.

## API Overview

| API | Purpose |
|---|---|
| `UserfaultFD.supported?` | Check whether a descriptor can be opened |
| `UserfaultFD.features` | List capabilities reported by the running kernel |
| `UserfaultFD#enabled_features` | List features enabled on a descriptor |
| `UserfaultFD#register` / `#unregister` | Manage registered regions |
| `UserfaultFD#writeprotect` | Enable or clear write protection |
| `Fault#zero`, `#copy`, `#wake` | Resolve missing faults |
| `Fault#continue` | Resolve minor faults |
| `Fault#poison`, `#move` | Use optional kernel operations |
| `Region#madvise` | Drop or remove mapped pages |

Optional operations raise `UserfaultFD::UnsupportedError` when they are unavailable in the build headers or running kernel.

## Compatibility

The gem first tries `/dev/userfaultfd`, then falls back to the syscall. Requiring it succeeds on unsupported systems; `UserfaultFD.supported?` returns `false`, while opening a descriptor raises `UserfaultFD::UnsupportedError` or the relevant `Errno::*` error.

| Environment | Result |
|---|---|
| macOS, BSD, Windows | Loading succeeds; `supported?` is `false` |
| Linux before 4.11 or without `CONFIG_USERFAULTFD` | Unsupported |
| Linux 4.11–5.10 | A permitted caller must explicitly pass `user_mode_only: false` |
| Docker with the default seccomp profile | The syscall may be blocked; allow `userfaultfd` or expose `/dev/userfaultfd` |
| Docker Desktop / WSL2 | Support depends on the VM kernel, configuration, device, and seccomp policy |
| Linux with `vm.unprivileged_userfaultfd=0` | User-mode-only faults work on supported kernels; fork events still require `CAP_SYS_PTRACE` |
| GitHub-hosted runners | Unsupported or restricted capabilities are skipped by system tests |
| Kernels before 6.6 / 6.8 | `POISON` / `MOVE` are unavailable |

## Examples

| Example | Demonstrates |
|---|---|
| [`lazy_mmap.rb`](examples/lazy_mmap.rb) | Lazy file loading with a native backing-file handler |
| [`dirty_tracking.rb`](examples/dirty_tracking.rb) | Dirty-page tracking with write-protect faults |
| [`post_copy_migration.rb`](examples/post_copy_migration.rb) | Page transfer to a forked process over a socket |
| [`oom_canary.rb`](examples/oom_canary.rb) | Page failure injection with `UFFDIO_POISON` |

## Development

```sh
bundle install
bundle exec rake compile
bundle exec rake test:unit
bundle exec rake test:system
bundle exec rbs -I sig validate
```

System tests use timeouts because an unresolved page fault intentionally blocks its faulting process. The manually dispatched [kernel matrix workflow](.github/workflows/kernel.yml) covers Linux 4.19, 5.15, 6.1, 6.6, and 6.8.

## Contributing

Bug reports and pull requests are welcome at https://github.com/ydah/userfaultfd.

## License

Released under the [MIT License](LICENSE.txt).
