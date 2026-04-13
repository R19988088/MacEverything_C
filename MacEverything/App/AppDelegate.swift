import Cocoa
import ServiceManagement

class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var hotkeyManager: HotkeyManager?
    private var statusItem: NSStatusItem?
    private var launchAtLoginItem: NSMenuItem?

    func applicationDidFinishLaunching(_ notification: Notification) {
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
            let isVisible = NSApp.windows.first { $0.isVisible && !($0 is NSPanel) } != nil
            item.title = isVisible ? "Hide MacEverything" : "Show MacEverything"
        }
        launchAtLoginItem?.state = SMAppService.mainApp.status == .enabled ? .on : .off
    }

    // MARK: - Menu Actions

    @objc private func toggleWindow() {
        if let window = NSApp.windows.first(where: { !($0 is NSPanel) }), window.isVisible {
            NSApp.hide(nil)
        } else {
            NSApp.activate(ignoringOtherApps: true)
            if let window = NSApp.windows.first(where: { !($0 is NSPanel) }) {
                window.makeKeyAndOrderFront(nil)
            }
        }
    }

    @objc private func rebuildIndex() {
        NSApp.activate(ignoringOtherApps: true)
        if let window = NSApp.windows.first(where: { !($0 is NSPanel) }) {
            window.makeKeyAndOrderFront(nil)
        }
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
            NSLog("Failed to toggle launch at login: \(error)")
        }
    }

    @objc private func quitApp() {
        NSApp.terminate(nil)
    }
}
