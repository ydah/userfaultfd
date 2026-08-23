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
end
