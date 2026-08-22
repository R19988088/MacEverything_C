import Cocoa
import ServiceManagement

class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var hotkeyManager: HotkeyManager?
    private var statusItem: NSStatusItem?
    private var launchAtLoginItem: NSMenuItem?
    private var mcpMenuItems: [MCPClient: NSMenuItem] = [:]
    private(set) var mainSearchWindow: NSWindow?
    private var currentLanguage: AppLanguage {
        AppLanguage(rawValue: UserDefaults.standard.string(forKey: "app.language") ?? "") ?? .english
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        MacSearchBridge.initializeLogger()

        let shouldMinimize = Self.shouldStartMinimized()

        // Delay by one frame to let SwiftUI create the window
        DispatchQueue.main.async { [weak self] in
            self?.mainSearchWindow = NSApp.windows.first { $0.title == "MacEverything" }
            if let window = self?.mainSearchWindow {
                // Liquid Glass needs the host window to expose the desktop behind it.
                window.isOpaque = false
                window.backgroundColor = .clear
                window.styleMask.insert(.fullSizeContentView)
                window.titlebarAppearsTransparent = true
                window.titleVisibility = .hidden
                window.titlebarSeparatorStyle = .none
                window.isMovableByWindowBackground = false
            }
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

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        DispatchQueue.global(qos: .userInitiated).async {
            MacSearchBridge.shared().prepareForTermination()
            DispatchQueue.main.async {
                NSApp.reply(toApplicationShouldTerminate: true)
            }
        }
        return .terminateLater
    }

    func applicationWillTerminate(_ notification: Notification) {
        // Safety net: shutdown is idempotent (compare_exchange_strong guard)
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
        menu.addItem(NSMenuItem(title: AppText.value("menu.show", language: currentLanguage), action: #selector(toggleWindow), keyEquivalent: ""))
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: AppText.value("settings.title", language: currentLanguage), action: #selector(openSettings), keyEquivalent: ","))
        menu.addItem(NSMenuItem(title: AppText.value("menu.syntax", language: currentLanguage), action: #selector(openSearchSyntaxHelp), keyEquivalent: ""))

        let mcpSubmenu = NSMenu(title: AppText.value("menu.mcp", language: currentLanguage))
        for client in MCPClient.allCases {
            let item = NSMenuItem(title: client.displayName, action: #selector(toggleMCPClient(_:)), keyEquivalent: "")
            item.representedObject = client
            mcpMenuItems[client] = item
            mcpSubmenu.addItem(item)
        }
        let mcpItem = NSMenuItem(title: AppText.value("menu.mcp", language: currentLanguage), action: nil, keyEquivalent: "")
        mcpItem.submenu = mcpSubmenu
        menu.addItem(mcpItem)

        menu.addItem(.separator())
        let loginItem = NSMenuItem(title: AppText.value("menu.login", language: currentLanguage), action: #selector(toggleLaunchAtLogin), keyEquivalent: "")
        launchAtLoginItem = loginItem
        menu.addItem(loginItem)
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: AppText.value("menu.quit", language: currentLanguage), action: #selector(quitApp), keyEquivalent: "q"))
        statusItem?.menu = menu
    }

    // MARK: - NSMenuDelegate

    func menuWillOpen(_ menu: NSMenu) {
        if let item = menu.items.first {
            let isVisible = mainSearchWindow?.isVisible ?? false
            item.title = AppText.value(isVisible ? "menu.hide" : "menu.show", language: currentLanguage)
        }
        if menu.items.count > 2 {
            menu.items[2].title = AppText.value("settings.title", language: currentLanguage)
            menu.items[3].title = AppText.value("menu.syntax", language: currentLanguage)
        }
        if menu.items.count > 6 { menu.items[6].title = AppText.value("menu.login", language: currentLanguage) }
        if menu.items.count > 8 { menu.items[8].title = AppText.value("menu.quit", language: currentLanguage) }
        launchAtLoginItem?.state = SMAppService.mainApp.status == .enabled ? .on : .off
        for (client, item) in mcpMenuItems {
            item.state = MCPConfigManager.isEnabled(for: client) ? .on : .off
        }
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

    @objc private func openSettings() {
        NSApp.activate(ignoringOtherApps: true)
        NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
    }

    @objc private func openSearchSyntaxHelp() {
        NSApp.activate(ignoringOtherApps: true)
        SearchSyntaxHelpWindowController.shared.showWindow()
    }

    @objc private func toggleMCPClient(_ sender: NSMenuItem) {
        guard let client = sender.representedObject as? MCPClient else { return }
        let currentlyEnabled = MCPConfigManager.isEnabled(for: client)
        MCPConfigManager.setEnabled(!currentlyEnabled, for: client)
        sender.state = !currentlyEnabled ? .on : .off
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
