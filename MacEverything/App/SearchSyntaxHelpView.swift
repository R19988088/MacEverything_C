import SwiftUI

struct SearchSyntaxHelpView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                basicSearchSection
                booleanSection
                wildcardSection
                regexSection
                extensionFilterSection
                sizeFilterSection
                typeFilterSection
                pathFilterSection
                dateFilterSection
                lengthFilterSection
                modifierSection
                typeMacroSection
                structuredPathSection
                contentSearchSection
                tildeSection
            }
            .padding(24)
        }
    }

    // MARK: - Sections

    private var basicSearchSection: some View {
        SyntaxSection(title: "Basic Search") {
            SyntaxRow("hello", "Substring match (case-insensitive)")
            SyntaxRow("hello world", "AND — both must match")
            SyntaxRow("\"exact phrase\"", "Quoted exact phrase match")
        }
    }

    private var booleanSection: some View {
        SyntaxSection(title: "Boolean Operators") {
            SyntaxRow("A | B", "OR — either matches")
            SyntaxRow("!A", "NOT — exclude matches")
            SyntaxRow("<A | B>", "Grouping with angle brackets")
            SyntaxNote("Example: <hello | world> .txt")
        }
    }

    private var wildcardSection: some View {
        SyntaxSection(title: "Wildcards / Glob") {
            SyntaxRow("*", "Zero or more characters")
            SyntaxRow("?", "Exactly one character")
            SyntaxRow("*.txt", "Files ending with .txt")
            SyntaxRow("test*", "Files starting with test")
            SyntaxRow("*keyword*", "Files containing keyword")
        }
    }

    private var regexSection: some View {
        SyntaxSection(title: "Regex") {
            SyntaxRow("regex:pattern", "ECMAScript regex (case-insensitive)")
            SyntaxNote("Example: regex:^test  regex:\\.cpp$")
        }
    }

    private var extensionFilterSection: some View {
        SyntaxSection(title: "Extension Filter") {
            SyntaxRow("ext:cpp", "Files with .cpp extension")
            SyntaxRow("ext:cpp;h;hpp", "Multiple extensions (semicolon-separated)")
        }
    }

    private var sizeFilterSection: some View {
        SyntaxSection(title: "Size Filter") {
            SyntaxRow("size:>1mb", "Larger than 1 MB")
            SyntaxRow("size:<500kb", "Smaller than 500 KB")
            SyntaxRow("size:>=1gb", "At least 1 GB")
            SyntaxRow("size:100kb..1mb", "Between 100 KB and 1 MB")
            SyntaxNote("Units: b, kb/k, mb/m, gb/g, tb/t")
        }
    }

    private var typeFilterSection: some View {
        SyntaxSection(title: "Type Filter") {
            SyntaxRow("file:", "Match only files")
            SyntaxRow("folder:", "Match only directories")
            SyntaxRow("type:file", "Same as file:")
            SyntaxRow("type:folder", "Same as folder:")
        }
    }

    private var pathFilterSection: some View {
        SyntaxSection(title: "Path Filter") {
            SyntaxRow("path:keyword", "Path contains keyword")
            SyntaxRow("nopath:keyword", "Path does not contain keyword")
            SyntaxRow("parent:dirname", "Immediate parent matches")
            SyntaxRow("depth:<3", "Directory depth less than 3")
            SyntaxRow("depth:>5", "Directory depth greater than 5")
        }
    }

    private var dateFilterSection: some View {
        SyntaxSection(title: "Date Filter") {
            SyntaxNote("Prefixes: dm: (modified)  dc: (created)  da: (accessed)")
            SyntaxRow("dm:today", "Modified today")
            SyntaxRow("dm:yesterday", "Modified yesterday")
            SyntaxRow("dm:thisweek", "Modified this week")
            SyntaxRow("dm:lastmonth", "Modified last month")
            SyntaxRow("dm:last7days", "Modified in the last 7 days")
            SyntaxRow("dm:last3months", "Modified in the last 3 months")
            SyntaxRow("dm:2024-01-15", "Modified on specific date")
            SyntaxRow("dm:>2024-01", "Modified after January 2024")
            SyntaxRow("dm:2024-01..2024-06", "Modified between Jan and Jun 2024")
            SyntaxNote("Also: datemodified:  datecreated:  dateaccessed:")
        }
    }

    private var lengthFilterSection: some View {
        SyntaxSection(title: "Name Length Filter") {
            SyntaxRow("len:>10", "Filename longer than 10 chars")
            SyntaxRow("len:<5", "Filename shorter than 5 chars")
            SyntaxRow("len:>=8", "Filename at least 8 chars")
        }
    }

    private var modifierSection: some View {
        SyntaxSection(title: "Match Modifiers") {
            SyntaxRow("case:term", "Force case-sensitive match")
            SyntaxRow("nocase:term", "Force case-insensitive match")
            SyntaxRow("ww:hello", "Whole word match")
            SyntaxRow("wfn:readme", "Whole filename match (without extension)")
            SyntaxNote("Aliases: wholeword:  wholefilename:")
        }
    }

    private var typeMacroSection: some View {
        SyntaxSection(title: "File Type Macros") {
            SyntaxRow("audio:", "mp3, wav, flac, aac, ogg, m4a, wma, alac")
            SyntaxRow("video:", "mp4, avi, mkv, mov, wmv, flv, webm, m4v")
            SyntaxRow("pic:", "jpg, png, gif, bmp, tiff, webp, svg, heic ...")
            SyntaxRow("doc:", "pdf, doc, docx, xls, xlsx, ppt, pptx, txt, md ...")
            SyntaxRow("exe:", "app, dmg, pkg, sh, command")
            SyntaxRow("zip:", "zip, rar, 7z, tar, gz, bz2, xz, tgz, zst, lz4")
        }
    }

    private var structuredPathSection: some View {
        SyntaxSection(title: "Structured Path Query") {
            SyntaxRow("/abc/def", "Name matches def, path contains abc")
            SyntaxRow("/abc/def/", "Directory named def under abc")
            SyntaxRow("/abc/def/*", "List children of def under abc")
            SyntaxRow("/abc/*/def", "Non-adjacent: abc and def with gaps")
            SyntaxNote("Path segments are matched right-to-left")
        }
    }

    private var contentSearchSection: some View {
        SyntaxSection(title: "Content Search") {
            SyntaxRow("content:keyword", "Search file contents for keyword")
        }
    }

    private var tildeSection: some View {
        SyntaxSection(title: "Tilde Expansion") {
            SyntaxRow("~/Downloads", "Expands ~ to your home directory")
            SyntaxRow("~/*.txt", "Glob in home directory")
            SyntaxNote("Only expanded when ~ is at the start of the query")
        }
    }
}

// MARK: - Helper Components

private struct SyntaxSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: () -> Content

    init(title: String, @ViewBuilder content: @escaping () -> Content) {
        self.title = title
        self.content = content
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.headline)
                .padding(.bottom, 2)
            content()
        }
    }
}

private struct SyntaxRow: View {
    let syntax: String
    let description: String

    init(_ syntax: String, _ description: String) {
        self.syntax = syntax
        self.description = description
    }

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Text(syntax)
                .font(.system(.body, design: .monospaced))
                .foregroundColor(.accentColor)
                .frame(width: 180, alignment: .leading)
            Text(description)
                .foregroundColor(.secondary)
            Spacer()
        }
    }
}

private struct SyntaxNote: View {
    let text: String

    init(_ text: String) {
        self.text = text
    }

    var body: some View {
        Text(text)
            .font(.callout)
            .foregroundColor(.secondary)
            .italic()
            .padding(.leading, 4)
    }
}
