# frozen_string_literal: true

require "fiddle"
require "tempfile"
require "timeout"
require "weakref"

RSpec.describe "UserfaultFD system integration" do
  around do |example|
    Timeout.timeout(10) { example.run }
  end

  before do
    @user_mode_only = if UserfaultFD.supported?
                        true
                      elsif UserfaultFD.supported?(user_mode_only: false)
                        false
                      else
                        skip "userfaultfd is unavailable or not permitted"
                      end
  end

  let(:page_size) { UserfaultFD::Region.allocate.page_size }

  def open_uffd(**options)
    UserfaultFD.new(**options, user_mode_only: @user_mode_only)
  end

  it "opens through the device or syscall backend" do
    uffd = open_uffd(features: [])
    expect(%i[device syscall]).to include(uffd.backend)
    expect(uffd.to_io.autoclose?).to be(false)

    if UserfaultFD.supported?(user_mode_only: false)
      legacy = UserfaultFD.new(features: [], user_mode_only: false)
      expect(%i[device syscall]).to include(legacy.backend)
    end
  ensure
    legacy&.close
    uffd&.close
  end

  it "opens with the default feature negotiation" do
    uffd = open_uffd
    expect([[], [:event_fork]]).to include(uffd.enabled_features)
    UserfaultFD.features(user_mode_only: @user_mode_only).each do |feature|
      expect(UserfaultFD).to be_const_defined("UFFD_FEATURE_#{feature.upcase}")
    end
  ensure
    uffd&.close
  end

  it "zero-fills 64 MiB without holding the GVL" do
    size = 64 * 1024 * 1024
    region = UserfaultFD::Region.new(size: size)
    uffd = open_uffd(features: [])
    uffd.register(region, mode: :missing)
    handler = uffd.start_handler(mode: :zero_fill)

    expect(region.read(0, size)).to eq("\0".b * size)
  ensure
    handler&.stop
    uffd&.close
    region&.unmap
  end

  it "rejects registration modes whose features were not enabled" do
    region = UserfaultFD::Region.new(size: page_size)
    uffd = open_uffd(features: [])

    expect { uffd.register(region, mode: []) }.to raise_error(ArgumentError)
    expect { uffd.register(region, mode: :wp) }
      .to raise_error(UserfaultFD::UnsupportedError, /not enabled/)
    expect { uffd.register(region, mode: :minor) }
      .to raise_error(UserfaultFD::UnsupportedError, /not enabled/)
  ensure
    uffd&.close
    region&.unmap
  end

  it "handles COPY and repeated faults with a Ruby callback" do
    region = UserfaultFD::Region.new(size: page_size)
    uffd = open_uffd(features: [])
    uffd.register(region, mode: :missing)
    faults = 0
    handler = uffd.start_handler do |event|
      next unless event.is_a?(UserfaultFD::Fault)

      faults += 1
      expect(event.thread_id).to be_nil
      event.copy("r" * page_size)
    end

    2.times do
      expect(region.read(0, page_size)).to eq("r" * page_size)
      region.madvise(:remove)
    end
    expect(faults).to eq(2)
  ensure
    handler&.stop
    uffd&.close
    region&.unmap
  end

  it "continues minor shmem faults when supported" do
    skip "minor shmem faults are unavailable" unless UserfaultFD.features.include?(:minor_shmem)

    region = UserfaultFD::Region.new(size: page_size)
    region.write(0, "c" * page_size)
    uffd = open_uffd(features: [:minor_shmem])
    uffd.register(region, mode: :minor)
    continued = false
    handler = uffd.start_handler do |event|
      next unless event.is_a?(UserfaultFD::Fault)

      continued = event.minor?
      event.continue
    end
    region.madvise(:dontneed)

    expect(region.read(0, page_size)).to eq("c" * page_size)
    expect(continued).to be(true)
  ensure
    handler&.stop
    uffd&.close
    region&.unmap
  end

  it "handles faults from a forked process" do
    region = UserfaultFD::Region.new(size: page_size)
    uffd = open_uffd
    skip "EVENT_FORK requires CAP_SYS_PTRACE" unless uffd.enabled_features.include?(:event_fork)

    uffd.register(region, mode: :missing)
    fork_event = nil
    handler = uffd.start_handler do |event|
      fork_event = event if event.is_a?(UserfaultFD::ForkEvent)
      event.copy("f" * page_size) if event.is_a?(UserfaultFD::Fault)
    end
    pid = fork { exit!(region.read(0, page_size) == "f" * page_size ? 0 : 1) }

    Process.wait(pid)
    expect($?.exitstatus).to eq(0)
    handler.stop
    expect(fork_event.child_uffd).to be_closed
  ensure
    Process.kill("KILL", pid) rescue nil if pid
    Process.wait(pid) rescue nil if pid
    handler&.stop
    uffd&.close
    region&.unmap
  end

  it "loads pages from files and prefilled regions in native handlers" do
    {backing_file: "b", prefilled: "p"}.each do |mode, byte|
      region = UserfaultFD::Region.new(size: page_size)
      uffd = open_uffd(features: [])
      uffd.register(region, mode: :missing)
      source = mode == :backing_file ? Tempfile.new("userfaultfd") : UserfaultFD::Region.new(size: page_size)
      mode == :backing_file ? source.write(byte * page_size) && source.flush : source.write(0, byte * page_size)
      handler = uffd.start_handler(mode: mode, io: source, source: source)

      expect(region.read(0, page_size)).to eq(byte * page_size)
      handler.stop
      uffd.close
      region.unmap
      mode == :backing_file ? source.close! : source.unmap
    end
  end

  it "reports and clears write-protect faults when supported" do
    skip "write-protect faults are unavailable" unless UserfaultFD.features.include?(:pagefault_flag_wp)

    region = UserfaultFD::Region.new(size: page_size)
    uffd = open_uffd(features: [:pagefault_flag_wp])
    uffd.register(region, mode: %i[missing wp])
    protected_fault = false
    handler = uffd.start_handler do |event|
      next unless event.is_a?(UserfaultFD::Fault)

      if event.wp?
        protected_fault = true
        uffd.writeprotect(region, enabled: false)
      else
        event.zero
      end
    end
    region.read(0, 1)
    uffd.writeprotect(region, enabled: true)

    expect(region.write(0, "w")).to eq(1)
    expect(protected_fault).to be(true)
  ensure
    handler&.stop
    uffd&.close
    region&.unmap
  end

  it "moves a populated page into a fault when supported" do
    skip "UFFDIO_MOVE is unavailable" unless UserfaultFD.features.include?(:move)

    source = UserfaultFD::Region.new(size: page_size, shared: false)
    region = UserfaultFD::Region.new(size: page_size, shared: false)
    source.write(0, "m" * page_size)
    uffd = open_uffd(features: [:move])
    uffd.register(region, mode: :missing)
    handler = uffd.start_handler do |event|
      event.move(source) if event.is_a?(UserfaultFD::Fault)
    end

    expect(region.read(0, page_size)).to eq("m" * page_size)
  ensure
    handler&.stop
    uffd&.close
    region&.unmap
    source&.unmap
  end

  it "poisons a child fault when supported" do
    required = %i[poison event_fork]
    skip "poison and fork events are unavailable" unless (required - UserfaultFD.features).empty?

    region = UserfaultFD::Region.new(size: page_size)
    uffd = open_uffd(features: required)
    uffd.register(region, mode: :missing)
    handler = uffd.start_handler do |event|
      event.poison if event.is_a?(UserfaultFD::Fault)
    end
    pid = fork do
      signal = Fiddle::Function.new(
        Fiddle::Handle::DEFAULT["signal"],
        [Fiddle::TYPE_INT, Fiddle::TYPE_VOIDP],
        Fiddle::TYPE_VOIDP
      )
      signal.call(Signal.list.fetch("BUS"), 0)
      region.read(0, 1)
    end

    Process.wait(pid)
    expect($?).to be_signaled
    expect($?.termsig).to eq(Signal.list.fetch("BUS"))
  ensure
    Process.kill("KILL", pid) rescue nil if pid
    Process.wait(pid) rescue nil if pid
    handler&.stop
    uffd&.close
    region&.unmap
  end

  it "delivers remove and unmap events when supported" do
    required = %i[event_remove event_unmap]
    skip "remove and unmap events are unavailable" unless (required - UserfaultFD.features).empty?

    region = UserfaultFD::Region.new(size: page_size)
    region.write(0, "e" * page_size)
    uffd = open_uffd(features: required)
    uffd.register(region, mode: :missing)
    events = Queue.new
    handler = uffd.start_handler { |event| events << event }

    region.madvise(:remove)
    expect(events.pop).to be_a(UserfaultFD::RemoveEvent)
    region.unmap
    expect(events.pop).to be_a(UserfaultFD::UnmapEvent)
  ensure
    handler&.stop
    uffd&.close
    region&.unmap
  end

  it "does not leak descriptors or mappings over repeated operations" do
    fd_before = Dir["/proc/self/fd/*"].size
    maps_before = File.readlines("/proc/self/maps").size
    1_000.times { open_uffd(features: []).close }
    1_000.times { UserfaultFD::Region.new(size: page_size).unmap }
    GC.start
    expect(Dir["/proc/self/fd/*"].size).to be <= fd_before + 1
    expect(File.readlines("/proc/self/maps").size).to be <= maps_before + 1
  end

  it "retains every registered region and rejects ambiguous native handlers" do
    first = UserfaultFD::Region.new(size: page_size)
    second = UserfaultFD::Region.new(size: page_size)
    weak_first = WeakRef.new(first)
    uffd = open_uffd(features: [])
    uffd.register(first, mode: :missing)
    uffd.register(second, mode: :missing)
    first = nil
    GC.start

    expect(weak_first).to be_weakref_alive
    expect { uffd.start_handler(mode: :zero_fill) }
      .to raise_error(UserfaultFD::Error, /exactly one/)
  ensure
    uffd&.close
    weak_first&.__getobj__&.unmap if weak_first&.weakref_alive?
    second&.unmap
  end

  it "documents raw-pointer deadlock with an isolated timeout" do
    pid = fork do
      region = UserfaultFD::Region.new(size: page_size)
      uffd = open_uffd(features: [])
      uffd.register(region, mode: :missing)
      uffd.start_handler { |event| event.zero if event.is_a?(UserfaultFD::Fault) }
      Fiddle::Pointer.new(region.to_ptr)[0]
      exit! 1
    end

    expect { Timeout.timeout(2) { Process.wait(pid) } }.to raise_error(Timeout::Error)
  ensure
    Process.kill("KILL", pid) rescue nil if pid
    Process.wait(pid) rescue nil if pid
  end
end
