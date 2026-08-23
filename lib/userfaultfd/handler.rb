# frozen_string_literal: true

class UserfaultFD
  class Handler
    attr_reader :error

    def initialize(uffd, mode: nil, io: nil, source: nil, &block)
      raise ArgumentError, "choose a block or mode, not both" if mode && block
      raise ArgumentError, "a block or mode is required" unless mode || block

      @uffd = uffd
      if block
        raise DeadlockError, "strict mode cannot verify raw pointer faults" if UserfaultFD.strict

        warn "userfaultfd: Ruby handlers require Region#read/write or a separate faulting process"
        start_ruby_handler(block)
      else
        native_source = mode == :backing_file ? io : source
        @native = uffd.__send__(:start_native_handler, mode, native_source)
      end
    end

    def stop
      if @native
        @native.stop
        return self
      end
      return self unless @thread
      raise Error, "handler cannot stop itself" if Thread.current == @thread

      @stop_writer.write_nonblock(".") rescue nil
      @thread.join
      @thread = nil
      raise @error if @error

      self
    end

    def running?
      @native ? @native.running? : !!@thread&.alive?
    end

    private

    def start_ruby_handler(block)
      @stop_reader, @stop_writer = IO.pipe
      @event_reader = @uffd.__send__(:start_event_reader)
      @event_io = IO.for_fd(@event_reader.fileno, autoclose: false)
      @thread = Thread.new do
        loop do
          ready = IO.select([@event_io, @stop_reader])&.first
          break if ready&.include?(@stop_reader)

          @event_reader.read_events.each(&block)
        end
      rescue StandardError => e
        @error = e
      ensure
        @event_reader.stop
        @stop_reader.close
        @stop_writer.close
      end
      @thread.report_on_exception = false
    end
  end
end
