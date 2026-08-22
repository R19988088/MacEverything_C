import Foundation
import Combine

enum AppLanguage: String, CaseIterable, Identifiable {
    case english = "en"
    case simplifiedChinese = "zh-Hans"
    case japanese = "ja"

    var id: String { rawValue }
    var locale: Locale { Locale(identifier: rawValue) }
    var displayName: String {
        switch self {
        case .english: return "English"
        case .simplifiedChinese: return "简体中文"
        case .japanese: return "日本語"
        }
    }
}

enum LaunchDisplay: String, CaseIterable, Identifiable {
    case blank
    case lastResult

    var id: String { rawValue }
}

enum ThemeColor: String, CaseIterable, Identifiable {
    case vermilion, amber, purple, jujube, lotus, indigo, deepBlue, imperialPurple
    case wisteria, mistBlue, pineGreen, rouge, hanGreen, gilded, qingdai, forest

    var id: String { rawValue }
    var hex: String {
        switch self {
        case .vermilion: return "ED5126"; case .amber: return "EEB900"; case .purple: return "7E1671"; case .jujube: return "7A1820"
        case .lotus: return "1A6840"; case .indigo: return "2A3C5C"; case .deepBlue: return "283A4F"; case .imperialPurple: return "7B2F7B"
        case .wisteria: return "BFA8F4"; case .mistBlue: return "2F5F8F"; case .pineGreen: return "A0D6B4"; case .rouge: return "9D4E5C"
        case .hanGreen: return "2E7D32"; case .gilded: return "D4A72C"; case .qingdai: return "1A3A5F"; case .forest: return "0D5E3A"
        }
    }
    func name(_ language: AppLanguage) -> String {
        let chinese = ["朱红", "琥珀黄", "魏紫", "枣红", "荷叶绿", "黛蓝", "玄青", "帝王紫", "浅紫藤萝", "雾蓝", "松花绿", "胭脂泪", "汉绣绿", "流金", "青黛", "深绿"]
        let index = ThemeColor.allCases.firstIndex(of: self)!
        switch language {
        case .simplifiedChinese: return chinese[index]
        case .japanese: return chinese[index]
        case .english: return chinese[index] + " · " + rawValue.replacingOccurrences(of: "([A-Z])", with: " $1", options: .regularExpression).capitalized
        }
    }
}

@MainActor
final class AppSettings: ObservableObject {
    static let shared = AppSettings()

    @Published var language: AppLanguage {
        didSet { defaults.set(language.rawValue, forKey: Keys.language) }
    }
    @Published var launchDisplay: LaunchDisplay {
        didSet { defaults.set(launchDisplay.rawValue, forKey: Keys.launchDisplay) }
    }
    @Published var iconScale: Double {
        didSet { defaults.set(iconScale, forKey: Keys.iconScale) }
    }
    @Published var excludedFolders: [String] {
        didSet { defaults.set(excludedFolders, forKey: Keys.excludedFolders) }
    }
    @Published private(set) var lastQuery: String? {
        didSet { defaults.set(lastQuery, forKey: Keys.lastQuery) }
    }
    @Published var selectedCategoryRawValue: Int {
        didSet { defaults.set(selectedCategoryRawValue, forKey: Keys.selectedCategory) }
    }
    @Published var enabledCategoryRawValues: [Int] {
        didSet { defaults.set(enabledCategoryRawValues, forKey: Keys.enabledCategories) }
    }
    @Published var themeColorRawValue: String {
        didSet { defaults.set(themeColorRawValue, forKey: Keys.themeColor) }
    }

    var themeColorHex: String {
        ThemeColor(rawValue: themeColorRawValue)?.hex ?? ThemeColor.vermilion.hex
    }

    private let defaults: UserDefaults

    private enum Keys {
        static let language = "app.language"
        static let launchDisplay = "app.launchDisplay"
        static let lastQuery = "app.lastQuery"
        static let iconScale = "app.iconScale"
        static let excludedFolders = "app.excludedFolders"
        static let selectedCategory = "app.selectedCategory"
        static let enabledCategories = "app.enabledCategories"
        static let themeColor = "app.themeColor"
    }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        language = AppLanguage(rawValue: defaults.string(forKey: Keys.language) ?? "") ?? .english
        launchDisplay = LaunchDisplay(rawValue: defaults.string(forKey: Keys.launchDisplay) ?? "") ?? .blank
        iconScale = min(max(defaults.double(forKey: Keys.iconScale), 1), 4)
        excludedFolders = defaults.stringArray(forKey: Keys.excludedFolders) ?? []
        lastQuery = defaults.string(forKey: Keys.lastQuery)
        selectedCategoryRawValue = defaults.object(forKey: Keys.selectedCategory) as? Int ?? 1
        enabledCategoryRawValues = defaults.array(forKey: Keys.enabledCategories) as? [Int]
            ?? SearchCategory.allCases.map(\.rawValue)
        themeColorRawValue = defaults.string(forKey: Keys.themeColor) ?? ThemeColor.vermilion.rawValue
    }

    func isCategoryEnabled(_ category: SearchCategory) -> Bool {
        enabledCategoryRawValues.contains(category.rawValue)
    }

    func setCategory(_ category: SearchCategory, enabled: Bool) {
        var values = Set(enabledCategoryRawValues)
        if enabled { values.insert(category.rawValue) } else { values.remove(category.rawValue) }
        enabledCategoryRawValues = SearchCategory.allCases.map(\.rawValue).filter { values.contains($0) }
    }

    func recordSuccessfulQuery(_ query: String) {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        lastQuery = trimmed.isEmpty ? nil : trimmed
    }

    func excludeFolder(_ path: String) {
        let normalized = URL(fileURLWithPath: path).standardizedFileURL.path
        guard normalized != "/", !excludedFolders.contains(normalized) else { return }
        excludedFolders.append(normalized)
    }

    func restoreFolder(_ path: String) {
        excludedFolders.removeAll { $0 == path }
    }

    var launchQuery: String? {
        launchDisplay == .lastResult ? lastQuery : nil
    }
}

enum AppText {
    static func value(_ key: String, language: AppLanguage = .english) -> String {
        switch language {
        case .english:
            return english[key] ?? key
        case .simplifiedChinese:
            return chinese[key] ?? english[key] ?? key
        case .japanese:
            return japanese[key] ?? english[key] ?? key
        }
    }

    private static let english: [String: String] = [
        "search.placeholder": "Search all files...",
        "category.applications": "Apps", "category.files": "Files", "category.folders": "Folders", "category.images": "Images",
        "category.videos": "Videos", "category.audio": "Music", "category.archives": "Archives", "category.brushes": "Brushes",
        "action.open": "Open", "action.reveal": "Open in Finder", "action.delete": "Delete", "action.undo": "Undo", "action.excludeFolder": "Exclude Folder",
        "settings.title": "Settings", "settings.general": "General", "settings.shortcut": "Shortcuts", "settings.content": "Content",
        "settings.language": "Language", "settings.launch": "Launch display", "settings.blank": "Show nothing", "settings.last": "Show last result", "settings.excludedFolders": "Excluded folders", "settings.noExcludedFolders": "No excluded folders", "settings.restoreFolder": "Restore folder",
        "settings.displayTypes": "Display types", "settings.enabledTypes": "Enabled types",
        "settings.theme": "Theme color",
        "settings.themeSource": "Theme color source",
        "settings.rebuild": "Rebuild Index",
        "menu.show": "Show MacEverything", "menu.hide": "Hide MacEverything", "menu.rebuild": "Rebuild Index", "menu.syntax": "Search Syntax Help...", "menu.mcp": "MCP Integration", "menu.login": "Launch at Login", "menu.quit": "Quit MacEverything", "menu.about": "About MacEverything", "menu.services": "Services", "menu.hideOthers": "Hide Others", "menu.showAll": "Show All",
        "permission.openSettings": "Open Settings", "permission.fullDisk": "Full Disk Access is required to scan all files.", "command.search": "Search", "command.regex": "Regex", "command.caseSensitive": "Case Sensitive", "command.wholeWord": "Whole Word", "command.matchFilename": "Match Filename",
        "status.scanning": "Scanning", "status.live": "Live", "status.indexed": "indexed", "status.matches": "matches",
        "empty.noResults": "No results found", "empty.start": "Type to search your files", "empty.content": "No content matches found"
        ,"shortcut.title": "Global Hotkey Settings", "shortcut.description": "Set the global hotkey for showing or hiding MacEverything.",
        "shortcut.current": "Current Shortcut", "shortcut.record": "Press a key combination...", "shortcut.reset": "Reset to Default",
        "content.indexing": "Content Indexing", "content.indexed": "Indexed Files", "content.maxSize": "Max File Size",
        "content.extensions": "File Extensions", "content.addPlaceholder": "Add extension...", "content.add": "Add", "content.apply": "Apply",
        "result.name": "Name", "result.path": "Path", "result.size": "Size", "result.modified": "Date Modified"
    ]
    private static let chinese: [String: String] = [
        "search.placeholder": "搜索所有文件…",
        "category.applications": "应用", "category.files": "文件", "category.folders": "文件夹", "category.images": "图片",
        "category.videos": "视频", "category.audio": "音乐", "category.archives": "压缩包", "category.brushes": "画笔",
        "action.open": "打开", "action.reveal": "打开所在文件夹", "action.delete": "删除", "action.undo": "撤销", "action.excludeFolder": "排除文件夹",
        "settings.title": "设置", "settings.general": "通用", "settings.shortcut": "快捷键", "settings.content": "内容",
        "settings.language": "语言", "settings.launch": "启动显示", "settings.blank": "不显示", "settings.last": "显示最后的结果", "settings.excludedFolders": "排除的文件夹", "settings.noExcludedFolders": "暂无排除的文件夹", "settings.restoreFolder": "恢复文件夹",
        "settings.displayTypes": "显示类型", "settings.enabledTypes": "启用的类型",
        "settings.theme": "主题颜色",
        "settings.themeSource": "主题色资料来源",
        "settings.rebuild": "重建索引",
        "menu.show": "显示 MacEverything", "menu.hide": "隐藏 MacEverything", "menu.rebuild": "重建索引", "menu.syntax": "搜索语法帮助…", "menu.mcp": "MCP 集成", "menu.login": "登录时启动", "menu.quit": "退出 MacEverything", "menu.about": "关于 MacEverything", "menu.services": "服务", "menu.hideOthers": "隐藏其他应用", "menu.showAll": "显示全部",
        "permission.openSettings": "打开设置", "permission.fullDisk": "扫描所有文件需要“完全磁盘访问权限”。", "command.search": "搜索", "command.regex": "正则表达式", "command.caseSensitive": "区分大小写", "command.wholeWord": "完整单词", "command.matchFilename": "匹配文件名",
        "status.scanning": "正在扫描", "status.live": "实时", "status.indexed": "已索引", "status.matches": "个匹配",
        "empty.noResults": "没有找到结果", "empty.start": "输入内容搜索文件", "empty.content": "没有匹配的内容"
        ,"shortcut.title": "全局快捷键设置", "shortcut.description": "设置显示或隐藏 MacEverything 的全局快捷键。", "shortcut.current": "当前快捷键",
        "shortcut.record": "按下快捷键组合…", "shortcut.reset": "恢复默认", "content.indexing": "内容索引", "content.indexed": "已索引文件",
        "content.maxSize": "最大文件大小", "content.extensions": "文件扩展名", "content.addPlaceholder": "添加扩展名…", "content.add": "添加", "content.apply": "应用",
        "result.name": "名字", "result.path": "位置", "result.size": "尺寸", "result.modified": "修改日期"
    ]
    private static let japanese: [String: String] = [
        "search.placeholder": "すべてのファイルを検索…",
        "category.applications": "アプリ", "category.files": "ファイル", "category.folders": "フォルダ", "category.images": "画像",
        "category.videos": "ビデオ", "category.audio": "音楽", "category.archives": "アーカイブ", "category.brushes": "ブラシ",
        "action.open": "開く", "action.reveal": "Finderで表示", "action.delete": "削除", "action.undo": "取り消す", "action.excludeFolder": "フォルダを除外",
        "settings.title": "設定", "settings.general": "一般", "settings.shortcut": "ショートカット", "settings.content": "内容",
        "settings.language": "言語", "settings.launch": "起動時の表示", "settings.blank": "何も表示しない", "settings.last": "最後の結果を表示", "settings.excludedFolders": "除外フォルダ", "settings.noExcludedFolders": "除外フォルダはありません", "settings.restoreFolder": "フォルダを復元",
        "settings.displayTypes": "表示タイプ", "settings.enabledTypes": "有効なタイプ",
        "settings.theme": "テーマカラー",
        "settings.themeSource": "テーマカラーの出典",
        "settings.rebuild": "索引を再構築",
        "menu.show": "MacEverything を表示", "menu.hide": "MacEverything を隠す", "menu.rebuild": "索引を再構築", "menu.syntax": "検索構文ヘルプ…", "menu.mcp": "MCP 統合", "menu.login": "ログイン時に起動", "menu.quit": "MacEverything を終了", "menu.about": "MacEverything について", "menu.services": "サービス", "menu.hideOthers": "ほかを隠す", "menu.showAll": "すべてを表示",
        "permission.openSettings": "設定を開く", "permission.fullDisk": "すべてのファイルをスキャンするにはフルディスクアクセスが必要です。", "command.search": "検索", "command.regex": "正規表現", "command.caseSensitive": "大文字と小文字を区別", "command.wholeWord": "単語全体", "command.matchFilename": "ファイル名に一致",
        "status.scanning": "スキャン中", "status.live": "ライブ", "status.indexed": "件を索引", "status.matches": "件の一致",
        "empty.noResults": "結果がありません", "empty.start": "入力してファイルを検索", "empty.content": "内容に一致する結果がありません"
        ,"shortcut.title": "グローバルショートカット設定", "shortcut.description": "MacEverything の表示と非表示を切り替えるショートカットを設定します。", "shortcut.current": "現在のショートカット",
        "shortcut.record": "キーの組み合わせを押してください…", "shortcut.reset": "デフォルトに戻す", "content.indexing": "内容の索引", "content.indexed": "索引済みファイル",
        "content.maxSize": "最大ファイルサイズ", "content.extensions": "ファイル拡張子", "content.addPlaceholder": "拡張子を追加…", "content.add": "追加", "content.apply": "適用",
        "result.name": "名前", "result.path": "場所", "result.size": "サイズ", "result.modified": "変更日"
    ]
}
