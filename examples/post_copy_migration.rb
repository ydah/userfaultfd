# frozen_string_literal: true

require "socket"
require "userfaultfd"

page_size = UserfaultFD::Region.allocate.page_size
region = UserfaultFD::Region.new(size: page_size)
uffd = UserfaultFD.new
abort "EVENT_FORK requires CAP_SYS_PTRACE" unless uffd.enabled_features.include?(:event_fork)

sender, receiver = Socket.pair(:UNIX, :STREAM)
sender.write("migrated".ljust(page_size, "\0"))
uffd.register(region, mode: :missing)
handler = uffd.start_handler do |event|
  event.copy(receiver.read(page_size)) if event.is_a?(UserfaultFD::Fault)
end

pid = fork do
  STDOUT.write("#{region.read(0, 8)}\n")
  STDOUT.flush
  exit! 0
end
Process.wait(pid)

handler.stop
uffd.close
region.unmap
sender.close
receiver.close
