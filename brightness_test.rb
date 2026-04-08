#!/usr/bin/env ruby
# Brightness test tool for FakeIrisXE

require 'IOKit'

def set_brightness(percent)
  registry = IOKit::open("/FakeIrisXEFramebuffer")
  return false unless registry
  
  begin
    registry.setProperty("brightness", percent)
    puts "Set brightness to #{percent}%"
    return true
  rescue => e
    puts "Error: #{e.message}"
    return false
  ensure
    IOKit.close(registry)
  end
end

def get_brightness
  registry = IOKit.open("/FakeIrisXEFramebuffer")
  return nil unless registry
  
  begin
    return registry.getProperty("brightness")
  rescue => e
    puts "Error: #{e.message}"
    return nil
  ensure
    IOKit.close(registry)
  end
end

if ARGV.empty?
  puts "Usage: brightness_test.rb <percent>"
  puts "  or: brightness_test.rb test"
  exit 1
end

if ARGV[0] == "test"
  [25, 50, 75, 100].each do |p|
    puts "Testing #{p}%..."
    set_brightness(p)
    sleep 1
    puts "  Current: #{get_brightness}"
  end
else
  p = ARGV[0].to_i
  set_brightness(p)
end