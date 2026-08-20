import SwiftUI

struct GlassCompatibility: ViewModifier {
    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            content.glassEffect(.regular, in: Capsule())
        } else {
            content.background(.ultraThinMaterial)
        }
    }
}

extension View {
    func macEverythingGlass() -> some View { modifier(GlassCompatibility()) }
}
