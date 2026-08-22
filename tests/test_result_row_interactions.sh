#!/bin/sh
set -eu

for source in MacEverything/App/ResultRow.swift MacEverything/App/ContentResultRow.swift; do
    if grep -q 'Button(action: onSelect)' "$source"; then
        echo "$source: result row must not be a Button because it consumes file drag and context-menu gestures" >&2
        exit 1
    fi
    grep -q '\.onDrag' "$source"
    grep -q '\.contextMenu' "$source"
    grep -q 'handleTap' "$source"
    ! grep -q '\.onTapGesture(count: 2' "$source"
    grep -q '\.accessibilityAddTraits(.isButton)' "$source"
done
grep -q 'itemsProvider' MacEverything/App/ContentView.swift
grep -q 'currentPreviewItemIndex' MacEverything/App/ContentView.swift
grep -q 'navigationMonitor' MacEverything/App/ContentView.swift
grep -q 'selectionDidChange' MacEverything/App/ContentView.swift
grep -q 'ResultNavigationController' MacEverything/App/ContentView.swift
grep -q 'onMoveCommand' MacEverything/App/ContentView.swift
! grep -q '\.id(item.id)' MacEverything/App/ContentView.swift
