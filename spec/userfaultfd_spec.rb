# frozen_string_literal: true

RSpec.describe UserfaultFD do
  it "has a version number" do
    expect(UserfaultFD::VERSION).to match(/\A\d+\.\d+\.\d+\z/)
  end

  it "loads on unsupported operating systems" do
    expect(described_class.supported?).to be(false) unless RUBY_PLATFORM.include?("linux")
  end
end
