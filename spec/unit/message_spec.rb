# frozen_string_literal: true

RSpec.describe "userfaultfd message parsing" do
  it "parses a fixed page-fault message" do
    skip "Linux-only kernel structure" unless RUBY_PLATFORM.include?("linux")

    page_size = UserfaultFD::Region.allocate.page_size
    bytes = [
      UserfaultFD::UFFD_EVENT_PAGEFAULT, 0, 0, 0,
      UserfaultFD::UFFD_PAGEFAULT_FLAG_WRITE, page_size + 7, 42, 0
    ].pack("CCS<L<Q<Q<L<L<")
    fault = UserfaultFD.__send__(:parse_message, bytes)

    expect(fault.address).to eq(page_size)
    expect(fault.flags).to eq([:write])
    expect(fault.thread_id).to eq(42)
  end

  it "parses fixed memory-layout event messages" do
    skip "Linux-only kernel structure" unless RUBY_PLATFORM.include?("linux")

    remap = UserfaultFD.__send__(
      :parse_message,
      [UserfaultFD::UFFD_EVENT_REMAP, 0, 0, 0, 1, 2, 3].pack("CCS<L<Q<Q<Q<")
    )
    remove = UserfaultFD.__send__(
      :parse_message,
      [UserfaultFD::UFFD_EVENT_REMOVE, 0, 0, 0, 4, 5, 0].pack("CCS<L<Q<Q<Q<")
    )
    unmap = UserfaultFD.__send__(
      :parse_message,
      [UserfaultFD::UFFD_EVENT_UNMAP, 0, 0, 0, 6, 7, 0].pack("CCS<L<Q<Q<Q<")
    )

    expect([remap.from, remap.to, remap.length]).to eq([1, 2, 3])
    expect([remove.start, remove.end]).to eq([4, 5])
    expect([unmap.start, unmap.end]).to eq([6, 7])
  end
end
