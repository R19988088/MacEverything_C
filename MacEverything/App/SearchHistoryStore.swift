import Foundation

/// Persists past search queries and provides prefix-based autocomplete suggestions.
final class SearchHistoryStore {
    private static let defaultsKey = "searchHistory"
    private static let maxEntries = 200
    private static let minQueryLength = 2

    struct Entry: Codable, Equatable, Identifiable {
        var id: String { query.lowercased() }
        var query: String
        var lastUsed: Date
        var count: Int
        var isPinned: Bool

        init(query: String, lastUsed: Date, count: Int, isPinned: Bool = false) {
            self.query = query
            self.lastUsed = lastUsed
            self.count = count
            self.isPinned = isPinned
        }

        enum CodingKeys: String, CodingKey { case query, lastUsed, count, isPinned }

        init(from decoder: Decoder) throws {
            let values = try decoder.container(keyedBy: CodingKeys.self)
            query = try values.decode(String.self, forKey: .query)
            lastUsed = try values.decode(Date.self, forKey: .lastUsed)
            count = try values.decode(Int.self, forKey: .count)
            isPinned = try values.decodeIfPresent(Bool.self, forKey: .isPinned) ?? false
        }
    }

    private var entries: [Entry] = []

    init() {
        load()
    }

    // MARK: - Public API

    func recordQuery(_ query: String) {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.count >= Self.minQueryLength,
              !trimmed.lowercased().hasPrefix("infile:") else { return }

        let lower = trimmed.lowercased()
        if let idx = entries.firstIndex(where: { $0.query.lowercased() == lower }) {
            entries[idx].lastUsed = Date()
            entries[idx].count += 1
            // Keep the most-recently-typed casing
            entries[idx].query = trimmed
        } else {
            entries.append(Entry(query: trimmed, lastUsed: Date(), count: 1))
        }

        // Evict oldest entries when over capacity
        if entries.count > Self.maxEntries {
            entries.sort { $0.lastUsed > $1.lastUsed }
            entries = Array(entries.prefix(Self.maxEntries))
        }

        save()
    }

    func bestMatch(for prefix: String) -> String? {
        guard !prefix.isEmpty else { return nil }
        let lowerPrefix = prefix.lowercased()

        let matches = entries.filter { $0.query.lowercased().hasPrefix(lowerPrefix) }
        // Sort by frequency desc, then recency desc
        let best = matches.max { a, b in
            if a.count != b.count { return a.count < b.count }
            return a.lastUsed < b.lastUsed
        }
        return best?.query
    }

    func recentQueries(limit: Int) -> [String] {
        recentEntries(limit: limit).map(\.query)
    }

    func recentEntries(limit: Int) -> [Entry] {
        entries
            .sorted { lhs, rhs in
                if lhs.isPinned != rhs.isPinned { return lhs.isPinned }
                return lhs.lastUsed > rhs.lastUsed
            }
            .prefix(max(0, limit))
            .map { $0 }
    }

    @discardableResult
    func prune(olderThanDays days: Int, now: Date = Date()) -> Bool {
        let cutoff = now.addingTimeInterval(-Double(max(0, days)) * 86_400)
        let retained = entries.filter { $0.isPinned || $0.lastUsed >= cutoff }
        guard retained.count != entries.count else { return false }
        entries = retained
        save()
        return true
    }

    func setPinned(_ query: String, pinned: Bool) {
        guard let index = entries.firstIndex(where: { $0.query.lowercased() == query.lowercased() }) else { return }
        entries[index].isPinned = pinned
        save()
    }

    func deleteQuery(_ query: String) {
        let originalCount = entries.count
        entries.removeAll { $0.query.lowercased() == query.lowercased() }
        if entries.count != originalCount { save() }
    }

    // MARK: - Persistence

    private func load() {
        guard let data = UserDefaults.standard.data(forKey: Self.defaultsKey) else { return }
        entries = (try? JSONDecoder().decode([Entry].self, from: data)) ?? []
    }

    private func save() {
        guard let data = try? JSONEncoder().encode(entries) else { return }
        UserDefaults.standard.set(data, forKey: Self.defaultsKey)
    }
}
