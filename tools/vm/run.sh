#!/bin/sh
set -eu

kernel=${1:-v6.6}
exec vng -v -r "$kernel" -- bundle exec rake compile test:system
