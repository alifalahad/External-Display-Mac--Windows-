// =============================================================================
// PairingRequestView.swift — Approve a new Windows receiver
// =============================================================================
// Shows the verification code both sides derived from the handshake. The user
// types it on Windows; "Pair" unlocks only after Windows confirmed the code,
// so a device the user isn't pairing (or someone in the middle) can't get in.
// =============================================================================

import SwiftUI

struct PairingRequestView: View {
    let request: PairingRequest
    let onPair: () -> Void
    let onReject: () -> Void

    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: request.windowsConfirmed ? "checkmark.shield.fill" : "key.fill")
                .font(.title2)
                .foregroundStyle(request.windowsConfirmed ? .green : .yellow)

            VStack(alignment: .leading, spacing: 3) {
                Text("“\(request.deviceName)” wants to pair")
                    .font(.headline)
                Text(request.windowsConfirmed
                     ? "Windows entered the correct code. Pair only if you started this pairing."
                     : "Type this code on the Windows PC:")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(Self.grouped(request.verificationCode))
                    .font(.system(.title, design: .monospaced, weight: .bold))
                    .foregroundStyle(request.windowsConfirmed ? .green : .yellow)
                    .accessibilityLabel("Verification code \(request.verificationCode)")
            }

            Spacer()

            Button("Reject", role: .destructive, action: onReject)
                .buttonStyle(.bordered)
            Button("Pair", action: onPair)
                .buttonStyle(.borderedProminent)
                .disabled(!request.windowsConfirmed)
                .help(request.windowsConfirmed ? "Trust this receiver" : "Waiting for the code on Windows")
        }
        .padding(.vertical, 8)
        .padding(.horizontal, 12)
    }

    /// "123456" → "123 456" (easier to read and type)
    static func grouped(_ code: String) -> String {
        guard code.count == 6 else { return code }
        return "\(code.prefix(3)) \(code.suffix(3))"
    }
}
