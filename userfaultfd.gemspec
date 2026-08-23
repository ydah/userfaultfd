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

  # Uncomment the line below to require MFA for gem pushes.
  # This helps protect your gem from supply chain attacks by ensuring
  # no one can publish a new version without multi-factor authentication.
  # See: https://guides.rubygems.org/mfa-requirement-opt-in/
  # spec.metadata["rubygems_mfa_required"] = "true"

  # Specify which files should be added to the gem when it is released.
  # The `git ls-files -z` loads the files in the RubyGem that have been added into git.
  spec.files = Dir["README.md", "LICENSE.txt", "docs/**/*", "examples/**/*", "ext/**/*", "lib/**/*", "sig/**/*"]
  spec.extensions = ["ext/userfaultfd/extconf.rb"]
  spec.require_paths = ["lib"]

  # Uncomment to register a new dependency of your gem
  # spec.add_dependency "example-gem", "~> 1.0"

  # For more information and examples about making a new gem, check out our
  # guide at: https://guides.rubygems.org/make-your-own-gem/
end
