import SwiftUI

struct SettingsView: View {
    @ObservedObject private var settings = AppSettings.shared

    var body: some View {
        TabView {
            Form {
                Section {
                    Picker(AppText.value("settings.language", language: settings.language), selection: $settings.language) {
                        ForEach(AppLanguage.allCases) { language in
                            Text(language.displayName).tag(language)
                        }
                    }
                    Picker(AppText.value("settings.launch", language: settings.language), selection: $settings.launchDisplay) {
                        Text(AppText.value("settings.blank", language: settings.language)).tag(LaunchDisplay.blank)
                        Text(AppText.value("settings.last", language: settings.language)).tag(LaunchDisplay.lastResult)
                    }
                }
                Section(AppText.value("settings.theme", language: settings.language)) {
                    LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 10), count: 4), spacing: 8) {
                        ForEach(ThemeColor.allCases) { theme in
                            Button {
                                settings.themeColorRawValue = theme.rawValue
                            } label: {
                                HStack(spacing: 6) {
                                    Circle().fill(Color(hex: theme.hex)).frame(width: 16, height: 16)
                                    Text(theme.name(settings.language)).lineLimit(1)
                                }
                                .frame(maxWidth: .infinity, minHeight: 30, alignment: .leading)
                            }
                            .buttonStyle(.plain)
                            .padding(.horizontal, 5)
                            .background(settings.themeColorRawValue == theme.rawValue ? Color(hex: theme.hex).opacity(0.18) : .clear)
                            .clipShape(RoundedRectangle(cornerRadius: 6))
                        }
                    }
                    HStack(spacing: 10) {
                        Text(AppText.value("settings.themeSource", language: settings.language))
                        Link("zhongguo traditional-colors", destination: URL(string: "https://github.com/nevertoday/zhongguo-traditional-colors")!)
                    }
                }
                Section {
                    Picker(AppText.value("settings.historyRetention", language: settings.language), selection: $settings.historyRetentionDays) {
                        ForEach([5, 10, 15, 30, 90], id: \.self) { days in
                            Text(String(format: AppText.value("history.days", language: settings.language), days))
                                .tag(days)
                        }
                    }
                    Button(AppText.value("settings.rebuild", language: settings.language)) {
                        NotificationCenter.default.post(name: .rebuildIndex, object: nil)
                    }
                }
            }
            .formStyle(.grouped)
            .tabItem { Label(AppText.value("settings.general", language: settings.language), systemImage: "gearshape") }

            ShortcutSettingsView()
                .tabItem { Label(AppText.value("settings.shortcut", language: settings.language), systemImage: "command") }

            ContentSettingsView()
                .tabItem { Label(AppText.value("settings.content", language: settings.language), systemImage: "doc.text.magnifyingglass") }
        }
        .frame(width: 620, height: 460)
        .padding(16)
        .id(settings.language)
        .tint(Color(hex: settings.themeColorHex))
        .accentColor(Color(hex: settings.themeColorHex))
    }
}

extension Color {
    init(hex: String) {
        let value = UInt64(hex, radix: 16) ?? 0
        self.init(.sRGB, red: Double((value >> 16) & 0xff) / 255,
                  green: Double((value >> 8) & 0xff) / 255,
                  blue: Double(value & 0xff) / 255, opacity: 1)
    }
}
