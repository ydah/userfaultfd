# frozen_string_literal: true

require "bundler/gem_tasks"
require "rake/extensiontask"
require "rspec/core/rake_task"

Rake::ExtensionTask.new("userfaultfd") do |ext|
  ext.lib_dir = "lib/userfaultfd"
end

RSpec::Core::RakeTask.new(:spec) do |task|
  task.pattern = "spec/**/*_spec.rb"
end

namespace :test do
  RSpec::Core::RakeTask.new(:unit) do |task|
    task.pattern = "spec/unit/**/*_spec.rb"
  end

  RSpec::Core::RakeTask.new(:system) do |task|
    task.pattern = "spec/system/**/*_spec.rb"
  end
end

task spec: :compile

task default: :spec
