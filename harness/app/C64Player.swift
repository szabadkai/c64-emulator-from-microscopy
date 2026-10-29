// C64Player: real-time window + audio + keyboard for the harness (see src/player.h).
//
// usage: c64player [--rom-dir DIR] [--prg FILE] [--cpu fast|compiled|netlist] [--sid-model 6581|8580]
//                  [--warp-frames N] [--type TEXT]
//                  [--headless [--frames N] [--screen-text]]   no window/audio: scripting and tests
//                  [--quit-after SECONDS]                       self-test: report pacing on stderr, exit
//                  [--keymap-test]                              check the Mac -> C64 key mapping, exit
//                  [--key-test]                                 type via synthetic live key events (headless), exit
//
// Pacing is audio-driven: the emulation thread runs a PAL frame whenever the
// audio FIFO holds less than ~3 frames, so playback speed is locked to the
// sound card consuming 44.1 kHz, i.e. exactly 985248 Hz / 19656 = 50.1246 fps.
// --warp-frames skips the first N frames (BASIC boot + loading) unthrottled.
//
// Keyboard (symbolic, like VICE's default): a printable key types the C64 character
// it produces on the Mac, with SHIFT added as needed. Special keys:
//   Return RETURN · Delete DEL (Shift: INST) · arrows CRSR (up/left = SHIFT+CRSR) · F1-F8
//   Home CLR/HOME · Esc RUN/STOP · Tab or Option C= · Control CTRL · Page Up RESTORE · ` ←
// Shift+letter gives SHIFT+letter (graphics characters); Cmd shortcuts stay with the Mac.
// A new key releases any still-held ones first: the KERNAL scan sees one key at a time.
// Text piped into stdin, or given with --type (\n = RETURN), is typed through the
// keyboard matrix once BASIC is up and any autostart has finished.

import AppKit
import AVFoundation

let W = Int(PLAYER_W), H = Int(PLAYER_H)
let samplesPerFrame = 44100.0 * 19656.0 / 985248.0

var romDir = "roms", prgPath = "", sidModel: Int32 = 6581, warpFrames = 160, cpuCore: Int32 = 1, quitAfter = 0.0
var typeText = "", headless = false, headlessFrames = 600, printScreen = false, keymapTest = false, keyTest = false
var argv = Array(CommandLine.arguments.dropFirst())
while !argv.isEmpty {
    let a = argv.removeFirst()
    switch a {
    case "--rom-dir": romDir = argv.removeFirst()
    case "--prg": prgPath = argv.removeFirst()
    case "--sid-model": sidModel = Int32(argv.removeFirst()) ?? 6581
    case "--warp-frames": warpFrames = Int(argv.removeFirst()) ?? 160
    case "--cpu":
        switch argv.removeFirst() {
        case "netlist": cpuCore = 0
        case "compiled": cpuCore = 2
        default: cpuCore = 1
        }
    case "--quit-after": quitAfter = Double(argv.removeFirst()) ?? 0
    case "--type": typeText = argv.removeFirst().replacingOccurrences(of: "\\n", with: "\n")
    case "--headless": headless = true
    case "--frames": headlessFrames = Int(argv.removeFirst()) ?? 600
    case "--screen-text": printScreen = true
    case "--keymap-test": keymapTest = true
    case "--key-test": keyTest = true
    default:
        FileHandle.standardError.write("unknown option \(a)\n".data(using: .utf8)!)
        exit(2)
    }
}

func stderr(_ s: String) { FileHandle.standardError.write((s + "\n").data(using: .utf8)!) }

// ---------------------------------------------------------------- keyboard mapping

typealias Pos = (col: Int32, row: Int32)
let LSHIFT: Pos = (1, 7), CTRL: Pos = (7, 2), CBM: Pos = (7, 5)

enum KeyAction { case keys([Pos]), restore, none }

/// Mac key -> C64 matrix positions. `chars` is what the key produces on the Mac
/// (respecting the layout and Shift); `shift` is whether a Shift key is held.
func mapKey(keyCode: UInt16, chars: String, shift: Bool) -> KeyAction {
    func special(_ p: Pos, shifted: Bool = false) -> KeyAction { .keys(shifted || shift ? [p, LSHIFT] : [p]) }
    switch keyCode {
    case 36, 76: return special((0, 1))                 // Return, keypad Enter
    case 51: return special((0, 0))                     // Delete -> DEL (Shift: INST)
    case 117: return special((0, 0), shifted: true)     // forward delete -> INST
    case 124: return special((0, 2))                    // right
    case 123: return special((0, 2), shifted: true)     // left = SHIFT+CRSR right
    case 125: return special((0, 7))                    // down
    case 126: return special((0, 7), shifted: true)     // up = SHIFT+CRSR down
    case 122: return special((0, 4)); case 120: return special((0, 4), shifted: true)   // F1 F2
    case 99: return special((0, 5)); case 118: return special((0, 5), shifted: true)    // F3 F4
    case 96: return special((0, 6)); case 97: return special((0, 6), shifted: true)     // F5 F6
    case 98: return special((0, 3)); case 100: return special((0, 3), shifted: true)    // F7 F8
    case 115: return special((6, 3))                    // Home -> CLR/HOME
    case 53: return special((7, 7))                     // Esc -> RUN/STOP
    case 48: return special(CBM)                        // Tab -> C=
    case 116: return .restore                           // Page Up -> RESTORE
    case 50: return special((7, 1))                     // ` -> left arrow
    case 49: return special((7, 4))                     // Space
    default: break
    }
    guard let ch = chars.unicodeScalars.first, ch.isASCII else { return .none }
    var c: Int32 = 0, r: Int32 = 0, s: Int32 = 0
    if player_char_key(Int32(ch.value), &c, &r, &s) != 0 { return .none }
    let letter = (ch.value >= 65 && ch.value <= 90) || (ch.value >= 97 && ch.value <= 122)
    return .keys(s != 0 || (letter && shift) ? [(c, r), LSHIFT] : [(c, r)])
}

if keymapTest {
    let cases: [(UInt16, String, Bool, [Pos])] = [
        (0, "a", false, [(1, 2)]), (0, "A", true, [(1, 2), LSHIFT]), (18, "1", false, [(7, 0)]),
        (18, "!", true, [(7, 0), LSHIFT]), (19, "@", true, [(5, 6)]), (19, "\"", true, [(7, 3), LSHIFT]),
        (24, "=", false, [(6, 5)]), (24, "+", true, [(5, 0)]), (28, "*", true, [(6, 1)]),
        (41, ":", true, [(5, 5)]), (41, ";", false, [(6, 2)]), (49, " ", false, [(7, 4)]),
        (36, "\r", false, [(0, 1)]), (51, "\u{7f}", false, [(0, 0)]), (123, "", false, [(0, 2), LSHIFT]),
        (126, "", false, [(0, 7), LSHIFT]), (125, "", false, [(0, 7)]), (120, "", false, [(0, 4), LSHIFT]),
        (53, "\u{1b}", false, [(7, 7)]), (53, "\u{1b}", true, [(7, 7), LSHIFT]), (115, "", true, [(6, 3), LSHIFT]),
    ]
    var fails = 0
    for (code, chars, shift, want) in cases {
        guard case .keys(let got) = mapKey(keyCode: code, chars: chars, shift: shift),
              got.count == want.count, zip(got, want).allSatisfy({ $0.col == $1.col && $0.row == $1.row })
        else { stderr("FAIL keyCode \(code) \(chars.debugDescription) shift=\(shift)"); fails += 1; continue }
    }
    if case .restore = mapKey(keyCode: 116, chars: "", shift: false) {} else { stderr("FAIL Page Up -> RESTORE"); fails += 1 }
    stderr("keymap test: \(cases.count + 1 - fails)/\(cases.count + 1) passed")
    exit(fails == 0 ? 0 : 1)
}

// ---------------------------------------------------------------- machine

var errbuf = [CChar](repeating: 0, count: 512)
guard let emu = player_create(romDir, prgPath, sidModel, cpuCore, &errbuf, errbuf.count) else {
    stderr("player: \(String(cString: errbuf))")
    exit(1)
}

/// Text waiting to be typed (stdin, --type); handed to the emulator on its own thread.
final class TextInbox {
    private var text = Data()
    private let lock = NSLock()
    func add(_ d: Data) { lock.lock(); text.append(d); lock.unlock() }
    func take() -> Data { lock.lock(); defer { text.removeAll(); lock.unlock() }; return text }
}
let inbox = TextInbox()
if !typeText.isEmpty { inbox.add(typeText.data(using: .ascii, allowLossyConversion: true)!) }
/// Text is read from stdin only when it is a pipe or a file. A terminal, a closed
/// descriptor or a socket (as some launchers provide) is left alone.
let stdinPiped: Bool = {
    var st = stat()
    guard fstat(0, &st) == 0 else { return false }
    let kind = st.st_mode & S_IFMT
    return kind == S_IFIFO || kind == S_IFREG
}()
/// Next chunk of stdin, nil at EOF or on error (POSIX read: errors are returned, not thrown).
func readStdin() -> Data? {
    var b = [UInt8](repeating: 0, count: 4096)
    let n = read(0, &b, b.count)
    return n > 0 ? Data(b[0..<n]) : nil
}
func feedTyping() {
    let d = inbox.take()
    if !d.isEmpty { d.withUnsafeBytes { player_type(emu, $0.bindMemory(to: CChar.self).baseAddress, d.count) } }
}

func screenText() -> String {
    var buf = [CChar](repeating: 0, count: 25 * 41)
    player_screen_text(emu, &buf)
    return (0..<25).map { r in String(cString: Array(buf[r * 41..<r * 41 + 41])) }.joined(separator: "\n")
}

if headless {
    if stdinPiped { while let d = readStdin() { inbox.add(d) } }
    var frames = 0
    feedTyping()
    // run the requested frames, then until queued text is typed (+ time for BASIC to react)
    while frames < headlessFrames || (player_type_pending(emu) > 0 && frames < 50_000) {
        player_run_frame(emu)
        frames += 1
    }
    for _ in 0..<50 { player_run_frame(emu) }
    if printScreen { print(screenText()) }
    exit(0)
}

// ---------------------------------------------------------------- audio

/// Single-producer / single-consumer sample FIFO between the emulation thread and the audio render thread.
final class AudioFIFO {
    private var buf = [Float](repeating: 0, count: 1 << 15)
    private var r = 0, w = 0
    private var lock = os_unfair_lock()
    private var started = false   // no underrun accounting before the first real samples
    var underruns = 0
    var count: Int {
        os_unfair_lock_lock(&lock); defer { os_unfair_lock_unlock(&lock) }
        return (w - r + buf.count) % buf.count
    }
    func push(_ s: UnsafePointer<Int16>, _ n: Int) {
        os_unfair_lock_lock(&lock); defer { os_unfair_lock_unlock(&lock) }
        if n > 0 { started = true }
        for i in 0..<n {
            if (w + 1) % buf.count == r { break }
            buf[w] = Float(s[i]) / 32768
            w = (w + 1) % buf.count
        }
    }
    func pop(_ out: UnsafeMutablePointer<Float>, _ n: Int) {
        os_unfair_lock_lock(&lock); defer { os_unfair_lock_unlock(&lock) }
        var short = false
        for i in 0..<n {
            if r == w { out[i] = 0; short = true; continue }
            out[i] = buf[r]
            r = (r + 1) % buf.count
        }
        if short && started { underruns += 1 }
    }
}

// ---------------------------------------------------------------- live keys

/// Matrix key events produced on the main thread, applied by the emulation thread
/// between frames. Positions are reference-counted so two Mac keys sharing a C64
/// key (e.g. SHIFT) don't release it early.
enum MatrixEvent { case key(Pos, Bool), restore(Bool) }
var matrixEvents: [MatrixEvent] = []        // main thread only
var held: [Int32: Int] = [:]                // col*8+row -> number of Mac keys holding it
var downByKeyCode: [UInt16: KeyAction] = [:]

func pressKeys(_ ps: [Pos]) {
    for p in ps {
        let k = p.col * 8 + p.row
        held[k, default: 0] += 1
        if held[k] == 1 { matrixEvents.append(.key(p, true)) }
    }
}
func releaseKeys(_ ps: [Pos]) {
    for p in ps {
        let k = p.col * 8 + p.row
        guard let n = held[k], n > 0 else { continue }
        held[k] = n - 1
        if n == 1 { matrixEvents.append(.key(p, false)) }
    }
}

/// Apply queued events, but never a release in the same frame as its press:
/// the KERNAL scans the keyboard ~60 times a second, so a key must stay down
/// for at least one frame to be seen. The rest waits for the next frame.
func applyMatrixEvents() {
    var pressedNow = Set<Int32>()   // matrix keys (col*8+row) and RESTORE (-1) pressed this frame
    var n = 0
    loop: for e in matrixEvents {
        let k: Int32, down: Bool
        switch e {
        case .key(let p, let d): k = p.col * 8 + p.row; down = d
        case .restore(let d): k = -1; down = d
        }
        if !down && pressedNow.contains(k) { break loop }
        switch e {
        case .key(let p, let d): player_key(emu, p.col, p.row, d)
        case .restore(let d): player_restore(emu, d)
        }
        if down { pressedNow.insert(k) }
        n += 1
    }
    matrixEvents.removeFirst(n)
}

final class Screen: NSView {
    private var pending: CGImage?
    private var lock = os_unfair_lock()
    private var modifiersHeld: [Pos] = []
    override var acceptsFirstResponder: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.magnificationFilter = .nearest
        layer?.contentsGravity = .resize
        layer?.backgroundColor = NSColor.black.cgColor
    }
    required init?(coder: NSCoder) { fatalError() }
    func post(_ rgb: UnsafePointer<UInt8>) {
        let data = Data(bytes: rgb, count: W * H * 3)
        guard let provider = CGDataProvider(data: data as CFData),
              let img = CGImage(width: W, height: H, bitsPerComponent: 8, bitsPerPixel: 24, bytesPerRow: W * 3,
                                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: 0),
                                provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { return }
        os_unfair_lock_lock(&lock); pending = img; os_unfair_lock_unlock(&lock)
        DispatchQueue.main.async { [weak self] in self?.show() }
    }
    private func show() {
        os_unfair_lock_lock(&lock); let img = pending; pending = nil; os_unfair_lock_unlock(&lock)
        if let img { layer?.contents = img }
    }
    override func keyDown(with e: NSEvent) {
        if e.modifierFlags.contains(.command) { super.keyDown(with: e); return }
        if e.isARepeat || downByKeyCode[e.keyCode] != nil { return }   // the C64 does its own key repeat
        let ctrl = e.modifierFlags.contains(.control)
        let chars = (ctrl ? e.charactersIgnoringModifiers : e.characters) ?? ""
        let action = mapKey(keyCode: e.keyCode, chars: chars, shift: e.modifierFlags.contains(.shift))
        switch action {
        case .keys(let ps):
            // The KERNAL scan reports one key, the last in scan order, so a key pressed while an
            // earlier one is still held is delayed or lost. Release the held keys first (rollover).
            for (code, held) in downByKeyCode {
                if case .keys(let qs) = held { releaseKeys(qs); downByKeyCode[code] = KeyAction.none }
            }
            pressKeys(ps)
        case .restore: matrixEvents.append(.restore(true))
        case .none: return
        }
        downByKeyCode[e.keyCode] = action
    }
    override func keyUp(with e: NSEvent) {
        guard let action = downByKeyCode.removeValue(forKey: e.keyCode) else { return }
        switch action {
        case .keys(let ps): releaseKeys(ps)
        case .restore: matrixEvents.append(.restore(false))
        case .none: break
        }
    }
    // Control -> CTRL, Option -> C= (with Shift held: SHIFT+C=, the charset toggle)
    override func flagsChanged(with e: NSEvent) {
        var want: [Pos] = []
        if e.modifierFlags.contains(.control) { want.append(CTRL) }
        if e.modifierFlags.contains(.option) {
            want.append(CBM)
            if e.modifierFlags.contains(.shift) { want.append(LSHIFT) }
        }
        let same = { (a: Pos, b: Pos) in a.col == b.col && a.row == b.row }
        releaseKeys(modifiersHeld.filter { m in !want.contains { same($0, m) } })
        pressKeys(want.filter { w in !modifiersHeld.contains { same($0, w) } })
        modifiersHeld = want
    }
}

if keyTest {
    // Worst case for the live path: every Mac key goes down and up within one frame.
    func tap(_ code: UInt16, _ chars: String, shift: Bool = false) {
        let a = mapKey(keyCode: code, chars: chars, shift: shift)
        if case .keys(let ps) = a { pressKeys(ps); releaseKeys(ps) }
    }
    for _ in 0..<150 { applyMatrixEvents(); player_run_frame(emu) }        // boot to READY.
    let line: [(UInt16, String, Bool)] = [(35, "p", false), (15, "r", false), (34, "i", false), (45, "n", false),
        (17, "t", false), (49, " ", false), (25, "9", false), (28, "*", true), (25, "9", false), (24, "+", true),
        (18, "1", false), (36, "\r", false)]
    for (code, chars, shift) in line { tap(code, chars, shift: shift) }
    var frames = 0
    while !matrixEvents.isEmpty && frames < 2000 { applyMatrixEvents(); player_run_frame(emu); frames += 1 }
    for _ in 0..<60 { applyMatrixEvents(); player_run_frame(emu) }
    let ok = screenText().contains("PRINT 9*9+1") && screenText().contains(" 82")

    // Fast typing with rollover, through the view's own key handlers: each key goes down
    // 3 frames after the previous one and is held for 7, so 2-3 keys are down at once.
    let view = Screen(frame: .zero)
    let rolled: [(UInt16, Character)] = [(17, "t"), (4, "h"), (14, "e"), (49, " "), (12, "q"), (32, "u"), (34, "i"),
        (8, "c"), (40, "k"), (49, " "), (11, "b"), (15, "r"), (31, "o"), (13, "w"), (45, "n"), (49, " "),
        (3, "f"), (31, "o"), (7, "x")]
    func key(_ type: NSEvent.EventType, _ k: (UInt16, Character)) {
        let e = NSEvent.keyEvent(with: type, location: .zero, modifierFlags: [], timestamp: 0, windowNumber: 0, context: nil,
                                 characters: String(k.1), charactersIgnoringModifiers: String(k.1), isARepeat: false, keyCode: k.0)!
        if type == .keyDown { view.keyDown(with: e) } else { view.keyUp(with: e) }
    }
    for f in 0..<(3 * rolled.count + 60) {
        for (i, k) in rolled.enumerated() {
            if f == 3 * i { key(.keyDown, k) }
            if f == 3 * i + 7 { key(.keyUp, k) }
        }
        applyMatrixEvents(); player_run_frame(emu)
    }
    let text = screenText()
    print(text.split(separator: "\n").filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }.joined(separator: "\n"))
    let rollOK = text.contains("THE QUICK BROWN FOX")
    stderr("key test: \(ok ? "PASS" : "FAIL") (\(line.count) taps applied over \(frames) frames)")
    stderr("rollover test: \(rollOK ? "PASS" : "FAIL") (\(rolled.count) keys, each held 7 frames, 3 frames apart)")
    exit(ok && rollOK ? 0 : 1)
}

// ---------------------------------------------------------------- window, audio, emulation thread

// lifecycle diagnostics on stderr, so a quiet exit always says why
stderr("c64player: starting (\(["netlist", "fast", "compiled"][Int(cpuCore)]) CPU\(prgPath.isEmpty ? "" : ", " + prgPath)); close the window to quit")
NSSetUncaughtExceptionHandler { e in stderr("c64player: uncaught exception \(e.name.rawValue): \(e.reason ?? "")") }
for sig in [SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGTERM, SIGHUP] {
    signal(sig) { n in
        let msg = "c64player: terminated by signal \(n)\n"
        _ = msg.withCString { write(2, $0, strlen($0)) }
        _exit(128 + n)
    }
}

let launchTime = Date()
let machine = emu   // a true global, so the app delegate class can reach it
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let scale = 2.0, par = 0.936   // PAL C64 pixel aspect ratio
let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: Double(W) * scale * par, height: Double(H) * scale),
                      styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
window.contentAspectRatio = NSSize(width: Double(W) * par, height: Double(H))
let screen = Screen(frame: window.contentLayoutRect)
window.contentView = screen
window.title = "C64 — \(prgPath.isEmpty ? "BASIC" : (prgPath as NSString).lastPathComponent)"
window.center()
window.makeKeyAndOrderFront(nil)
window.makeFirstResponder(screen)

if stdinPiped {
    Thread {
        while let d = readStdin() { inbox.add(d) }
    }.start()
}

let fifo = AudioFIFO()
let engine = AVAudioEngine()
let fmt = AVAudioFormat(standardFormatWithSampleRate: 44100, channels: 1)!
let source = AVAudioSourceNode(format: fmt) { _, _, frameCount, abl -> OSStatus in
    let buffers = UnsafeMutableAudioBufferListPointer(abl)
    for b in buffers { fifo.pop(b.mData!.assumingMemoryBound(to: Float.self), Int(frameCount)) }
    return noErr
}
engine.attach(source)
engine.connect(source, to: engine.mainMixerNode, format: fmt)
do { try engine.start() } catch { stderr("audio: \(error)") }

let emuThread = Thread {
    var frames = 0
    var t0 = Date(), f0 = 0
    var realStart: Date? = nil, realFrames = 0
    while true {
        let warping = frames < warpFrames
        if !warping && Double(fifo.count) > 3 * samplesPerFrame { usleep(1000); continue }
        DispatchQueue.main.sync { applyMatrixEvents() }
        feedTyping()
        player_run_frame(emu)
        frames += 1
        if !warping {
            if realStart == nil { realStart = Date(); realFrames = 0 }
            realFrames += 1
            let el = Date().timeIntervalSince(realStart!)
            if quitAfter > 0 && el >= quitAfter {
                let fps = Double(realFrames) / el
                stderr(String(format: "self-test: %d real-time frames in %.2f s = %.3f fps (PAL 50.125, %.2f%%), audio underruns %d",
                              realFrames, el, fps, 100 * fps / 50.1246, fifo.underruns))
                exit(0)
            }
        }
        var n: Int32 = 0
        let a = player_audio(emu, &n)
        if !warping, let a { fifo.push(a, Int(n)) }
        if !warping || frames % 10 == 0 { screen.post(player_frame_rgb(emu)) }
        if Date().timeIntervalSince(t0) >= 1 {
            let fps = Double(frames - f0) / Date().timeIntervalSince(t0)
            let status = warping ? "warping \(frames)/\(warpFrames)" : String(format: "%.2f fps (%.0f%% of PAL)", fps, 100 * fps / 50.1246)
            let under = fifo.underruns
            DispatchQueue.main.async { window.title = "C64 — \(status), audio underruns \(under)" }
            t0 = Date(); f0 = frames
        }
    }
}
emuThread.qualityOfService = .userInteractive
emuThread.start()

final class Delegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { true }
    func applicationDidFinishLaunching(_ n: Notification) { stderr("c64player: window open") }
    func applicationWillTerminate(_ n: Notification) {
        let secs = Date().timeIntervalSince(launchTime)
        stderr(String(format: "c64player: window closed after %.1f s (%llu frames emulated, audio underruns %d), exiting",
                      secs, player_frame_number(machine), fifo.underruns))
    }
}
let delegate = Delegate()
app.delegate = delegate
app.activate(ignoringOtherApps: true)
app.run()
