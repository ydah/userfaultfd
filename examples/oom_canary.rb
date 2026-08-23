# frozen_string_literal: true

require "userfaultfd"

required = %i[poison event_fork]
abort "UFFDIO_POISON and EVENT_FORK are required" unless (required - UserfaultFD.features).empty?

page_size = UserfaultFD::Region.allocate.page_size
region = UserfaultFD::Region.new(size: page_size)
uffd = UserfaultFD.new(features: required)
uffd.register(region, mode: :missing)
handler = uffd.start_handler do |event|
  event.poison if event.is_a?(UserfaultFD::Fault)
end

pid = fork do
  region.read(0, 1)
  exit! 0
end
Process.wait(pid)
puts "child status: #{$?}"

handler.stop
uffd.close
region.unmap
