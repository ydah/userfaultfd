# frozen_string_literal: true

require "fiddle"
require "tempfile"
require "timeout"

RSpec.describe "UserfaultFD system integration" do
  around do |example|
    Timeout.timeout(10) { example.run }
  end

  before do
    skip "userfaultfd is unavailable or not permitted" unless UserfaultFD.supported?
  end

  let(:page_size) { UserfaultFD::Region.allocate.page_size }

  it "opens through the device or syscall backend" do
    uffd = UserfaultFD.new(features: [])
    expect(%i[device syscall]).to include(uffd.backend)
    expect(uffd.to_io.autoclose?).to be(false)
  ensure
    uffd&.close
  end

  it "zero-fills 64 MiB without holding the GVL" do
    size = 64 * 1024 * 1024
    region = UserfaultFD::Region.new(size: size)
    uffd = UserfaultFD.new(features: [])
    uffd.register(region, mode: :missing)
    handler = uffd.start_handler(mode: :zero_fill)

    expect(region.read(0, size)).to eq("\0".b * size)
  ensure
    handler&.stop
    uffd&.close
    region&.unmap
  end

  it "handles COPY and repeated faults with a Ruby callback" do
    region = UserfaultFD::Region.new(size: page_size)
    uffd = UserfaultFD.new(features: [])
    uffd.register(region, mode: :missing)
    faults = 0
    handler = uffd.start_handler do |event|
      next unless event.is_a?(UserfaultFD::Fault)

      faults += 1
      event.copy("r" * page_size)
    end

    2.times do
      expect(region.read(0, page_size)).to eq("r" * page_size)
      region.madvise(:dontneed)
    end
    expect(faults).to eq(2)
  ensure
    handler&.stop
    uffd&.close
    region&.unmap
  end

  it "handles faults from a forked process" do
    region = UserfaultFD::Region.new(size: page_size)
    uffd = UserfaultFD.new
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
      uffd = UserfaultFD.new(features: [])
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
    uffd = UserfaultFD.new(features: [:pagefault_flag_wp])
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
    uffd = UserfaultFD.new(features: [:move])
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

  it "does not leak descriptors or mappings over repeated operations" do
    fd_before = Dir["/proc/self/fd/*"].size
    maps_before = File.readlines("/proc/self/maps").size
    1_000.times { UserfaultFD.new(features: []).close }
    1_000.times { UserfaultFD::Region.new(size: page_size).unmap }
    GC.start
    expect(Dir["/proc/self/fd/*"].size).to be <= fd_before + 1
    expect(File.readlines("/proc/self/maps").size).to be <= maps_before + 1
  end

  it "documents raw-pointer deadlock with an isolated timeout" do
    pid = fork do
      region = UserfaultFD::Region.new(size: page_size)
      uffd = UserfaultFD.new(features: [])
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
