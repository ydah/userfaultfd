# frozen_string_literal: true

RSpec.describe UserfaultFD do
  it "exposes an empty feature set when unsupported" do
    expect(described_class.features).to eq([]) unless described_class.supported?
  end

  it "raises only when an unsupported descriptor is opened" do
    skip "supported on this host" if described_class.supported?

    expect { described_class.new }.to raise_error(UserfaultFD::UnsupportedError)
  end

  it "rejects unknown feature names before opening a descriptor" do
    skip "feature masks are Linux-only" unless RUBY_PLATFORM.include?("linux")

    expect { described_class.new(features: [:unknown]) }.to raise_error(ArgumentError, /unknown feature/)
  end
end
