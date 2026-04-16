import Cocoa
import ServiceManagement

class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var hotkeyManager: HotkeyManager?
    private var statusItem: NSStatusItem?
    private var launchAtLoginItem: NSMenuItem?
    private(set) var mainSearchWindow: NSWindow?

    func applicationDidFinishLaunching(_ notification: Notification) {
        MacSearchBridge.initializeLogger()

        let shouldMinimize = Self.shouldStartMinimized()

        // Delay by one frame to let SwiftUI create the window
        DispatchQueue.main.async { [weak self] in
            self?.mainSearchWindow = NSApp.windows.first { $0.title == "MacEverything" }
            if shouldMinimize {
                self?.mainSearchWindow?.orderOut(nil)
                NSApp.hide(nil)
            }
        }
        hotkeyManager = HotkeyManager()
        hotkeyManager?.register()
        setupStatusBar()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return false
    }

    func applicationWillTerminate(_ notification: Notification) {
        MacSearchBridge.shared().prepareForTermination()
    }

    // MARK: - Status Bar

    private func setupStatusBar() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = statusItem?.button {
            button.image = NSImage(systemSymbolName: "magnifyingglass", accessibilityDescription: "MacEverything")
        }

        let menu = NSMenu()
        menu.delegate = self
        menu.addItem(NSMenuItem(title: "Show MacEverything", action: #selector(toggleWindow), keyEquivalent: ""))
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Rebuild Index", action: #selector(rebuildIndex), keyEquivalent: ""))
        menu.addItem(NSMenuItem(title: "Shortcut Settings...", action: #selector(openShortcutSettings), keyEquivalent: ""))
        menu.addItem(NSMenuItem(title: "Content Settings...", action: #selector(openContentSettings), keyEquivalent: ""))
        menu.addItem(.separator())
        let loginItem = NSMenuItem(title: "Launch at Login", action: #selector(toggleLaunchAtLogin), keyEquivalent: "")
        launchAtLoginItem = loginItem
        menu.addItem(loginItem)
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit MacEverything", action: #selector(quitApp), keyEquivalent: "q"))
        statusItem?.menu = menu
    }

    // MARK: - NSMenuDelegate

    func menuWillOpen(_ menu: NSMenu) {
        if let item = menu.items.first {
            let isVisible = mainSearchWindow?.isVisible ?? false
            item.title = isVisible ? "Hide MacEverything" : "Show MacEverything"
        }
        launchAtLoginItem?.state = SMAppService.mainApp.status == .enabled ? .on : .off
    }

    // MARK: - Menu Actions

    @objc private func toggleWindow() {
        if let window = mainSearchWindow, window.isVisible {
            NSApp.hide(nil)
        } else {
            NSApp.activate(ignoringOtherApps: true)
            mainSearchWindow?.makeKeyAndOrderFront(nil)
        }
    }

    @objc private func rebuildIndex() {
        NSApp.activate(ignoringOtherApps: true)
        mainSearchWindow?.makeKeyAndOrderFront(nil)
        NotificationCenter.default.post(name: .rebuildIndex, object: nil)
    }

    @objc private func openShortcutSettings() {
        NSApp.activate(ignoringOtherApps: true)
        ShortcutSettingsWindowController.shared.showWindow()
    }

    @objc private func openContentSettings() {
        NSApp.activate(ignoringOtherApps: true)
        ContentSettingsWindowController.shared.showWindow()
    }

    @objc private func toggleLaunchAtLogin() {
        do {
            if SMAppService.mainApp.status == .enabled {
                try SMAppService.mainApp.unregister()
            } else {
                try SMAppService.mainApp.register()
            }
        } catch {
            AppLogger.error("App", "Failed to toggle launch at login: \(error)")
        }
    }

    @objc private func quitApp() {
        NSApp.terminate(nil)
    }

    // MARK: - Launch Mode Detection

    private static func shouldStartMinimized() -> Bool {
        if CommandLine.arguments.contains("--minimized") {
            return true
        }
        if let event = NSAppleEventManager.shared().currentAppleEvent,
           event.eventID == kAEOpenApplication,
           event.paramDescriptor(forKeyword: keyAEPropData)?.stringValue == "com.apple.loginwindow" {
            return true
        }
        return false
    }
}
