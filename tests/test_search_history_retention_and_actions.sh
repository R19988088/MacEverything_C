#!/bin/sh
set -eu

history=MacEverything/App/SearchHistoryStore.swift
settings=MacEverything/App/AppSettings.swift
view_model=MacEverything/App/SearchViewModel.swift
settings_view=MacEverything/App/SettingsView.swift
content=MacEverything/App/ContentView.swift

grep -q 'var isPinned: Bool' "$history"
grep -q 'func prune(olderThanDays' "$history"
grep -q 'func setPinned' "$history"
grep -q 'func deleteQuery' "$history"
grep -q '@Published var historyRetentionDays' "$settings"
grep -q 'historyRetentionDays' "$settings_view"
grep -q 'Picker' "$settings_view"
grep -q 'historyStore.prune' "$view_model"
grep -q 'historyQuery' "$content"
grep -q 'history.pin' "$content"
grep -q 'history.delete' "$content"
