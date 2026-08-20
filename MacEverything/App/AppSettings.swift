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
    @Published private(set) var lastQuery: String? {
        didSet { defaults.set(lastQuery, forKey: Keys.lastQuery) }
    }

    private let defaults: UserDefaults

    private enum Keys {
        static let language = "app.language"
        static let launchDisplay = "app.launchDisplay"
        static let lastQuery = "app.lastQuery"
        static let iconScale = "app.iconScale"
    }

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        language = AppLanguage(rawValue: defaults.string(forKey: Keys.language) ?? "") ?? .english
        launchDisplay = LaunchDisplay(rawValue: defaults.string(forKey: Keys.launchDisplay) ?? "") ?? .blank
        iconScale = min(max(defaults.double(forKey: Keys.iconScale), 1), 4)
        lastQuery = defaults.string(forKey: Keys.lastQuery)
    }

    func recordSuccessfulQuery(_ query: String) {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        lastQuery = trimmed.isEmpty ? nil : trimmed
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
        "search.placeholder": "Search files... (infile: for content search)",
        "category.applications": "Apps", "category.files": "Files", "category.folders": "Folders", "category.images": "Images",
        "category.videos": "Videos", "category.audio": "Music", "category.archives": "Archives",
        "action.open": "Open", "action.reveal": "Open in Finder", "action.delete": "Delete", "action.undo": "Undo",
        "settings.title": "Settings", "settings.general": "General", "settings.shortcut": "Shortcuts", "settings.content": "Content",
        "settings.language": "Language", "settings.launch": "Launch display", "settings.blank": "Show nothing", "settings.last": "Show last result",
        "status.scanning": "Scanning", "status.live": "Live", "status.indexed": "indexed", "status.matches": "matches",
        "empty.noResults": "No results found", "empty.start": "Type to search your files", "empty.content": "No content matches found"
        ,"shortcut.title": "Global Hotkey Settings", "shortcut.description": "Set the global hotkey for showing or hiding MacEverything.",
        "shortcut.current": "Current Shortcut", "shortcut.record": "Press a key combination...", "shortcut.reset": "Reset to Default",
        "content.indexing": "Content Indexing", "content.indexed": "Indexed Files", "content.maxSize": "Max File Size",
        "content.extensions": "File Extensions", "content.addPlaceholder": "Add extension...", "content.add": "Add", "content.apply": "Apply",
        "result.name": "Name", "result.path": "Path", "result.size": "Size", "result.modified": "Date Modified"
    ]
    private static let chinese: [String: String] = [
        "search.placeholder": "搜索文件…（使用 infile: 搜索内容）",
        "category.applications": "应用", "category.files": "文件", "category.folders": "文件夹", "category.images": "图片",
        "category.videos": "视频", "category.audio": "音乐", "category.archives": "压缩包",
        "action.open": "打开", "action.reveal": "打开所在文件夹", "action.delete": "删除", "action.undo": "撤销",
        "settings.title": "设置", "settings.general": "通用", "settings.shortcut": "快捷键", "settings.content": "内容",
        "settings.language": "语言", "settings.launch": "启动显示", "settings.blank": "不显示", "settings.last": "显示最后的结果",
        "status.scanning": "正在扫描", "status.live": "实时", "status.indexed": "已索引", "status.matches": "个匹配",
        "empty.noResults": "没有找到结果", "empty.start": "输入内容搜索文件", "empty.content": "没有匹配的内容"
        ,"shortcut.title": "全局快捷键设置", "shortcut.description": "设置显示或隐藏 MacEverything 的全局快捷键。", "shortcut.current": "当前快捷键",
        "shortcut.record": "按下快捷键组合…", "shortcut.reset": "恢复默认", "content.indexing": "内容索引", "content.indexed": "已索引文件",
        "content.maxSize": "最大文件大小", "content.extensions": "文件扩展名", "content.addPlaceholder": "添加扩展名…", "content.add": "添加", "content.apply": "应用",
        "result.name": "名字", "result.path": "位置", "result.size": "尺寸", "result.modified": "修改日期"
    ]
    private static let japanese: [String: String] = [
        "search.placeholder": "ファイルを検索…（内容検索は infile:）",
        "category.applications": "アプリ", "category.files": "ファイル", "category.folders": "フォルダ", "category.images": "画像",
        "category.videos": "ビデオ", "category.audio": "音楽", "category.archives": "アーカイブ",
        "action.open": "開く", "action.reveal": "Finderで表示", "action.delete": "削除", "action.undo": "取り消す",
        "settings.title": "設定", "settings.general": "一般", "settings.shortcut": "ショートカット", "settings.content": "内容",
        "settings.language": "言語", "settings.launch": "起動時の表示", "settings.blank": "何も表示しない", "settings.last": "最後の結果を表示",
        "status.scanning": "スキャン中", "status.live": "ライブ", "status.indexed": "件を索引", "status.matches": "件の一致",
        "empty.noResults": "結果がありません", "empty.start": "入力してファイルを検索", "empty.content": "内容に一致する結果がありません"
        ,"shortcut.title": "グローバルショートカット設定", "shortcut.description": "MacEverything の表示と非表示を切り替えるショートカットを設定します。", "shortcut.current": "現在のショートカット",
        "shortcut.record": "キーの組み合わせを押してください…", "shortcut.reset": "デフォルトに戻す", "content.indexing": "内容の索引", "content.indexed": "索引済みファイル",
        "content.maxSize": "最大ファイルサイズ", "content.extensions": "ファイル拡張子", "content.addPlaceholder": "拡張子を追加…", "content.add": "追加", "content.apply": "適用",
        "result.name": "名前", "result.path": "場所", "result.size": "サイズ", "result.modified": "変更日"
    ]
}
