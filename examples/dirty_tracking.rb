# frozen_string_literal: true

require "userfaultfd"

abort "write-protect faults are unavailable" unless UserfaultFD.features.include?(:pagefault_flag_wp)

page_size = UserfaultFD::Region.allocate.page_size
region = UserfaultFD::Region.new(size: page_size)
uffd = UserfaultFD.new(features: [:pagefault_flag_wp])
uffd.register(region, mode: %i[missing wp])
dirty_pages = []
handler = uffd.start_handler do |event|
  next unless event.is_a?(UserfaultFD::Fault)

  if event.wp?
    dirty_pages << event.address
    uffd.writeprotect(region, enabled: false)
  else
    event.zero
  end
end

region.read(0, 1)
uffd.writeprotect(region, enabled: true)
region.write(0, "changed")
puts "dirty pages: #{dirty_pages.length}"

handler.stop
uffd.close
region.unmap
