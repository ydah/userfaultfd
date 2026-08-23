# frozen_string_literal: true

RSpec.describe UserfaultFD::Region do
  let(:page_size) { described_class.allocate.page_size }

  it "maps, reads, writes, advises, and unmaps memory" do
    region = described_class.new(size: page_size)

    expect(region.write(1, "ruby")).to eq(4)
    expect(region.read(0, 6)).to eq("\0ruby\0")
    expect(region.to_ptr).to eq(region.address)
    expect(region.madvise(:dontneed)).to be_nil
    expect(region.unmap).to be_nil
    expect(region).to be_unmapped
    expect { region.read(0, 1) }.to raise_error(UserfaultFD::Error, "unmapped region")
  ensure
    region&.unmap
  end

  it "rejects invalid sizes and ranges" do
    expect { described_class.new(size: 0) }.to raise_error(ArgumentError)
    expect { described_class.new(size: page_size - 1) }.to raise_error(ArgumentError)

    region = described_class.new(size: page_size)
    expect { region.read(page_size, 1) }.to raise_error(RangeError)
    expect { region.write(page_size, "x") }.to raise_error(RangeError)
  ensure
    region&.unmap
  end
end
