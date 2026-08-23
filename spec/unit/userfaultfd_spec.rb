# frozen_string_literal: true

RSpec.describe UserfaultFD do
  it "exposes an empty feature set when unsupported" do
    expect(described_class.features).to eq([]) unless described_class.supported?
  end

  it "raises only when an unavailable descriptor is opened" do
    skip "supported on this host" if described_class.supported?

    error = begin
      described_class.new
      nil
    rescue StandardError => caught
      caught
    end
    expect(error).to be_a(UserfaultFD::UnsupportedError).or be_a(SystemCallError)
  end

  it "rejects unknown feature names before opening a descriptor" do
    skip "feature masks are Linux-only" unless RUBY_PLATFORM.include?("linux")

    expect { described_class.new(features: [:unknown]) }.to raise_error(ArgumentError, /unknown feature/)
  end

  it "requires an explicit boolean for user-mode-only operation" do
    expect { described_class.supported?(user_mode_only: nil) }
      .to raise_error(ArgumentError, /true or false/)
  end
end
