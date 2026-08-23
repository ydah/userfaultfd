# frozen_string_literal: true

require "timeout"

RSpec.describe UserfaultFD::Handler do
  class FakeUserfaultFD
    def initialize
      @reader, @writer = IO.pipe
    end

    def to_io = @reader
    def fileno = @reader.fileno

    def read_events
      @reader.read_nonblock(1)
      [:fault]
    end

    def signal = @writer.write(".")
    def stop = self

    def close
      @reader.close
      @writer.close
    end

    private

    def start_event_reader = self
  end

  it "dispatches events and stops through a self-pipe" do
    fake = FakeUserfaultFD.new
    events = Queue.new
    handler = nil

    expect do
      handler = described_class.new(fake) { |event| events << event }
    end.to output(/Region#read\/write/).to_stderr
    fake.signal
    expect(Timeout.timeout(1) { events.pop }).to eq(:fault)
    expect(handler.stop).to eq(handler)
    expect(handler).not_to be_running
  ensure
    handler&.stop
    fake&.close
  end

  it "rejects unverifiable Ruby handlers in strict mode" do
    fake = FakeUserfaultFD.new
    UserfaultFD.strict = true

    expect { described_class.new(fake) {} }.to raise_error(UserfaultFD::DeadlockError)
  ensure
    UserfaultFD.strict = false
    fake&.close
  end
end
