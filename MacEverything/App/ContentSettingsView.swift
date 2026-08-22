import SwiftUI

struct ContentSettingsView: View {
    @ObservedObject private var settings = AppSettings.shared
    @State private var extensions: [String] = []
    @State private var newExtension: String = ""
    @State private var maxFileSizeMB: Double = 1.0
    @State private var indexedCount: UInt32 = 0

    @State private var initialExtensions: [String] = []
    @State private var initialMaxFileSizeMB: Double = 1.0

    private let bridge = MacSearchBridge.shared()

    var body: some View {
        Form {
            Section(AppText.value("content.indexing", language: settings.language)) {
                VStack(alignment: .leading, spacing: 8) {
                    Text("\(AppText.value("content.indexed", language: settings.language)): \(indexedCount)")
                        .font(.headline)

                    Divider()

                    Text(AppText.value("content.maxSize", language: settings.language))
                        .font(.subheadline)
                    HStack {
                        Slider(value: $maxFileSizeMB, in: 0.1...10.0, step: 0.1)
                            .focusable(false)
                        Text(String(format: "%.1f MB", maxFileSizeMB))
                            .frame(width: 60)
                    }

                    Divider()

                    Text(AppText.value("content.extensions", language: settings.language))
                        .font(.subheadline)

                    ScrollView {
                        FlowLayout(spacing: 4) {
                            ForEach(extensions, id: \.self) { ext in
                                HStack(spacing: 2) {
                                    Text(".\(ext)")
                                        .font(.caption)
                                    Button {
                                        removeExtension(ext)
                                    } label: {
                                        Image(systemName: "xmark.circle.fill")
                                            .font(.caption2)
                                    }
                                    .buttonStyle(.plain)
                                }
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(Capsule().fill(Color.secondary.opacity(0.2)))
                            }
                        }
                    }
                    .frame(maxHeight: 120)

                    HStack {
                        TextField(AppText.value("content.addPlaceholder", language: settings.language), text: $newExtension)
                            .textFieldStyle(.roundedBorder)
                            .onSubmit { addExtension() }
                        Button(AppText.value("content.add", language: settings.language)) { addExtension() }
                            .disabled(newExtension.isEmpty)
                    }
                }
            }

            Section(AppText.value("settings.displayTypes", language: settings.language)) {
                let groups: [[SearchCategory]] = [
                    [.applications, .images, .archives],
                    [.files, .videos, .brushes],
                    [.folders, .audio]
                ]
                HStack(spacing: 16) {
                    ForEach(Array(groups.enumerated()), id: \.offset) { _, group in
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(group) { category in
                                HStack(spacing: 10) {
                                    Text(AppText.value(category.titleKey, language: settings.language))
                                    Toggle("", isOn: Binding(
                                        get: { settings.isCategoryEnabled(category) },
                                        set: { settings.setCategory(category, enabled: $0) }
                                    ))
                                    .labelsHidden()
                                }
                                .frame(maxWidth: .infinity, minHeight: 34, alignment: .leading)
                                .padding(.horizontal, 8)
                                .background(settings.isCategoryEnabled(category) ? Color(hex: settings.themeColorHex).opacity(0.18) : .clear)
                                .clipShape(RoundedRectangle(cornerRadius: 8))
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }

            Section {
                Button(AppText.value("content.apply", language: settings.language)) {
                    applySettings()
                }
            }

            Section(AppText.value("settings.excludedFolders", language: settings.language)) {
                if settings.excludedFolders.isEmpty {
                    Text(AppText.value("settings.noExcludedFolders", language: settings.language))
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(settings.excludedFolders, id: \.self) { folder in
                        HStack(spacing: 8) {
                            Image(systemName: "folder.fill")
                                .foregroundStyle(.secondary)
                            Text(folder)
                                .lineLimit(1)
                                .truncationMode(.middle)
                            Spacer(minLength: 4)
                            Button {
                                settings.restoreFolder(folder)
                            } label: {
                                Image(systemName: "trash")
                            }
                            .buttonStyle(.plain)
                            .help(AppText.value("settings.restoreFolder", language: settings.language))
                        }
                    }
                }
            }
        }
        .formStyle(.grouped)
        .padding(16)
        .tint(Color(hex: settings.themeColorHex))
        .onAppear { loadSettings() }
    }

    private func loadSettings() {
        indexedCount = bridge.contentIndexedFileCount()
        extensions = (bridge.contentGetExtensions() as? [String]) ?? []
        maxFileSizeMB = Double(bridge.contentGetMaxFileSize()) / (1024.0 * 1024.0)

        initialExtensions = extensions
        initialMaxFileSizeMB = maxFileSizeMB
    }

    private func addExtension() {
        let ext = newExtension.trimmingCharacters(in: .whitespaces).lowercased()
            .replacingOccurrences(of: ".", with: "")
        guard !ext.isEmpty, !extensions.contains(ext) else { return }
        extensions.append(ext)
        newExtension = ""
    }

    private func removeExtension(_ ext: String) {
        extensions.removeAll { $0 == ext }
    }

    private func applySettings() {
        let dirty = extensions != initialExtensions || maxFileSizeMB != initialMaxFileSizeMB
        guard dirty else { return }
        bridge.setContentMaxFileSize(UInt64(maxFileSizeMB * 1024 * 1024))
        bridge.setContentExtensions(extensions)
        DispatchQueue.global().async {
            bridge.rebuildContentIndex()
        }
        initialExtensions = extensions
        initialMaxFileSizeMB = maxFileSizeMB
    }
}

struct FlowLayout: Layout {
    var spacing: CGFloat = 4

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let result = layout(in: proposal.width ?? 0, subviews: subviews)
        return result.size
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let result = layout(in: bounds.width, subviews: subviews)
        for (index, point) in result.positions.enumerated() {
            subviews[index].place(at: CGPoint(x: bounds.minX + point.x, y: bounds.minY + point.y),
                                  proposal: .unspecified)
        }
    }

    private func layout(in width: CGFloat, subviews: Subviews) -> (size: CGSize, positions: [CGPoint]) {
        var positions: [CGPoint] = []
        var x: CGFloat = 0
        var y: CGFloat = 0
        var rowHeight: CGFloat = 0
        var maxWidth: CGFloat = 0

        for subview in subviews {
            let size = subview.sizeThatFits(.unspecified)
            if x + size.width > width && x > 0 {
                x = 0
                y += rowHeight + spacing
                rowHeight = 0
            }
            positions.append(CGPoint(x: x, y: y))
            rowHeight = max(rowHeight, size.height)
            x += size.width + spacing
            maxWidth = max(maxWidth, x)
        }

        return (CGSize(width: maxWidth, height: y + rowHeight), positions)
    }
}
