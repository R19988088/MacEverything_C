#!/bin/sh
set -eu

grep -q 'Brushes' MacEverything/Core/FileCategory.h
for ext in abr tpl ssspreset brushset sut mtd mtc clip artstudio_brushes; do
    grep -q '"'"$ext"'"' MacEverything/Core/FileCategory.h
done
grep -q 'category.brushes' MacEverything/App/AppSettings.swift
grep -q 'selectedCategory' MacEverything/App/AppSettings.swift
grep -q 'isMovableByWindowBackground = false' MacEverything/App/AppDelegate.swift
grep -q 'copySelected' MacEverything/App/ResultActions.swift
grep -q 'ForEach(SearchCategory.allCases)' MacEverything/App/ContentView.swift
! grep -q 'categoryMenu' MacEverything/App/ContentView.swift
grep -q 'dateFormat = "yyyy MM dd HH:mm"' MacEverything/App/ResultRow.swift
grep -q 'frame(width: 88, alignment: .leading)' MacEverything/App/ResultRow.swift
grep -q 'frame(width: 168, alignment: .leading)' MacEverything/App/ResultRow.swift
