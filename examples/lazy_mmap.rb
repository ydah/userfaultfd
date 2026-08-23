# frozen_string_literal: true

require "userfaultfd"

path = ARGV.fetch(0)
file = File.open(path, "rb")
page_size = UserfaultFD::Region.allocate.page_size
size = [((file.size + page_size - 1) / page_size) * page_size, page_size].max
region = UserfaultFD::Region.new(size: size)
uffd = UserfaultFD.new(features: [])
uffd.register(region, mode: :missing)
handler = uffd.start_handler(mode: :backing_file, io: file)

puts region.read(0, [file.size, page_size].min)

handler.stop
uffd.close
region.unmap
file.close
