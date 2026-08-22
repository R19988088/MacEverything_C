#!/bin/sh
set -eu

grep -q '"/private/tmp"' MacEverything/App/AppSettings.swift
grep -q 'PRODUCT_NAME = maceverything;' MacEverything.xcodeproj/project.pbxproj
grep -q 'Window("maceverything"' MacEverything/App/MacEverythingApp.swift
grep -q 'window.title == "maceverything"' MacEverything/App/AppDelegate.swift
