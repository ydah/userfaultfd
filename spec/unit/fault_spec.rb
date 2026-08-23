# frozen_string_literal: true

RSpec.describe UserfaultFD::Fault do
  subject(:fault) do
    described_class.allocate.tap do |event|
      event.instance_variable_set(:@flags, %i[write wp])
    end
  end

  it "reports page-fault flags" do
    expect(fault).to be_write
    expect(fault).to be_wp
    expect(fault).not_to be_minor
  end
end
