# frozen_string_literal: true

require_relative "lib/userfaultfd/version"

Gem::Specification.new do |spec|
  spec.name = "userfaultfd"
  spec.version = UserfaultFD::VERSION
  spec.authors = ["Yudai Takada"]
  spec.email = ["t.yudai92@gmail.com"]

  spec.summary = "Ruby bindings for Linux userfaultfd"
  spec.description = "Handle Linux user-space page faults safely without blocking Ruby's GVL."
  spec.homepage = "https://github.com/ydah/userfaultfd"
  spec.license = "MIT"
  spec.required_ruby_version = ">= 3.2.0"
  spec.metadata["allowed_push_host"] = "https://rubygems.org"
  spec.metadata["homepage_uri"] = spec.homepage
  spec.metadata["rubygems_mfa_required"] = "true"

  spec.files = Dir[
    "README.md", "LICENSE.txt", "docs/**/*", "examples/**/*",
    "ext/**/*.{c,h,rb}", "lib/**/*.rb", "sig/**/*.rbs"
  ]
  spec.extensions = ["ext/userfaultfd/extconf.rb"]
  spec.require_paths = ["lib"]
end
