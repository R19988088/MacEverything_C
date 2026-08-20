import SwiftUI

struct SettingsView: View {
    @ObservedObject private var settings = AppSettings.shared

    var body: some View {
        TabView {
            Form {
                Section {
                    Picker(AppText.value("settings.language"), selection: $settings.language) {
                        ForEach(AppLanguage.allCases) { language in
                            Text(language.displayName).tag(language)
                        }
                    }
                    Picker(AppText.value("settings.launch"), selection: $settings.launchDisplay) {
                        Text(AppText.value("settings.blank")).tag(LaunchDisplay.blank)
                        Text(AppText.value("settings.last")).tag(LaunchDisplay.lastResult)
                    }
                }
            }
            .tabItem { Label(AppText.value("settings.general"), systemImage: "gearshape") }

            ShortcutSettingsView()
                .tabItem { Label(AppText.value("settings.shortcut"), systemImage: "command") }

            ContentSettingsView()
                .tabItem { Label(AppText.value("settings.content"), systemImage: "doc.text.magnifyingglass") }
        }
        .frame(width: 560, height: 430)
        .padding(20)
        .id(settings.language)
    }
}
