# frozen_string_literal: true

require "mkmf"

have_header("linux/userfaultfd.h")
have_header("sys/ioctl.h")
have_header("sys/syscall.h")
have_func("syscall", "unistd.h")
have_library("pthread")

create_header
create_makefile("userfaultfd/userfaultfd")
