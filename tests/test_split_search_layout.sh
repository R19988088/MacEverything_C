#!/bin/sh
set -eu

content_view=MacEverything/App/ContentView.swift
history_store=MacEverything/App/SearchHistoryStore.swift
view_model=MacEverything/App/SearchViewModel.swift
app=MacEverything/App/MacEverythingApp.swift

grep -q 'private var mainWorkspace' "$content_view"
grep -q 'private var leftSidebar' "$content_view"
grep -q 'private var rightContent' "$content_view"
! grep -q 'customTitleBar' "$content_view"
! sed -n '/private var mainWorkspace/,/^    }/p' "$content_view" | grep -q 'Divider()'
sed -n '/private var leftSidebar/,/^    }/p' "$content_view" | grep -q 'Color.black.opacity(0.06)'
sed -n '/private var leftSidebar/,/^    }/p' "$content_view" | grep -q '\.padding(\.top, 38)'
grep -q 'private var bottomControlBar' "$content_view"
! sed -n '/private var leftSidebar/,/^    }/p' "$content_view" | grep -q 'ActionBar'
sed -n '/private var bottomControlBar/,/^    }/p' "$content_view" | grep -q 'actionButton'
grep -q 'ForEach(viewModel.searchHistoryQueries' "$content_view"
grep -q 'viewModel.selectHistoryQuery(query)' "$content_view"
grep -q 'if #available(macOS 26.0, \*)' "$content_view"
grep -q '\.buttonStyle(\.glass)' "$content_view"
grep -q '\.frame(minWidth: 1080, minHeight: 600)' "$content_view"
grep -q '\.defaultSize(width: 1200, height: 720)' "$app"

grep -q 'func recentQueries(limit: Int)' "$history_store"
grep -q '@Published private(set) var searchHistoryQueries' "$view_model"
grep -q 'func selectHistoryQuery(_ query: String)' "$view_model"

category_line=$(grep -n 'categoryBar' "$content_view" | tail -1 | cut -d: -f1)
results_line=$(grep -n 'resultsArea' "$content_view" | tail -1 | cut -d: -f1)
[ "$category_line" -lt "$results_line" ]
