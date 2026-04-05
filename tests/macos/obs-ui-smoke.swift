import Foundation

struct Options {
    var obsApp = "/Applications/OBS.app/Contents/MacOS/OBS"
    var artifactDir = NSTemporaryDirectory() + "/obs-lottie-ui-smoke"
    var example = FileManager.default.currentDirectoryPath + "/examples/slide-and-mask.json"
}

func parseArgs() -> Options {
    var options = Options()
    var iterator = CommandLine.arguments.dropFirst().makeIterator()
    while let arg = iterator.next() {
        guard let value = iterator.next() else { break }
        switch arg {
        case "--obs-app":
            options.obsApp = value
        case "--artifact-dir":
            options.artifactDir = value
        case "--example":
            options.example = value
        default:
            break
        }
    }
    return options
}

func shell(_ launchPath: String, _ arguments: [String]) throws -> String {
    let task = Process()
    task.executableURL = URL(fileURLWithPath: launchPath)
    task.arguments = arguments
    let pipe = Pipe()
    task.standardOutput = pipe
    task.standardError = pipe
    try task.run()
    task.waitUntilExit()
    return String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
}

func runAppleScript(_ source: String) throws {
    let output = try shell("/usr/bin/osascript", ["-e", source])
    if !output.isEmpty {
        FileHandle.standardOutput.write(output.data(using: .utf8)!)
    }
}

func appendLog(_ artifactDir: String, _ line: String) {
    let file = URL(fileURLWithPath: artifactDir).appendingPathComponent("ui-smoke.log")
    let text = "\(ISO8601DateFormatter().string(from: Date())) \(line)\n"
    if FileManager.default.fileExists(atPath: file.path) {
        if let handle = try? FileHandle(forWritingTo: file) {
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: text.data(using: .utf8)!)
            try? handle.close()
        }
    } else {
        try? text.write(to: file, atomically: true, encoding: .utf8)
    }
}

func screenshot(_ artifactDir: String, _ step: String) {
    let file = URL(fileURLWithPath: artifactDir).appendingPathComponent("\(step).png").path
    _ = try? shell("/usr/sbin/screencapture", ["-x", file])
}

let options = parseArgs()
try FileManager.default.createDirectory(atPath: options.artifactDir, withIntermediateDirectories: true)

appendLog(options.artifactDir, "launching OBS")
let process = Process()
process.executableURL = URL(fileURLWithPath: options.obsApp)
process.arguments = ["--multi"]
try process.run()
sleep(4)
screenshot(options.artifactDir, "01-launched")

let escapedExample = options.example.replacingOccurrences(of: "\"", with: "\\\"")

let activateOBS = """
tell application "OBS" to activate
"""

let addTransition = """
tell application "System Events"
  tell process "OBS"
    set frontmost to true
    click menu item "Add Configurable Transition" of menu 1 of button 1 of group 1 of splitter group 1 of window 1
  end tell
end tell
"""

let setFileAndPreview = """
tell application "System Events"
  tell process "OBS"
    set frontmost to true
    delay 1
    keystroke "\(escapedExample)"
    delay 1
    click button "Preview Transition" of window 1
    delay 1
    click button "Preview Transition" of window 1
    delay 1
    click button "OK" of window 1
  end tell
end tell
"""

let triggerAndDelete = """
tell application "System Events"
  tell process "OBS"
    set frontmost to true
    click button "Transition" of window 1
    delay 1
    click button "Transition" of window 1
    delay 1
    click menu item "Remove Configurable Transition" of menu 1 of button 2 of group 1 of splitter group 1 of window 1
  end tell
end tell
"""

do {
    appendLog(options.artifactDir, "activating OBS")
    try runAppleScript(activateOBS)
    sleep(2)
    screenshot(options.artifactDir, "02-activated")

    appendLog(options.artifactDir, "adding configurable transition")
    try runAppleScript(addTransition)
    sleep(2)
    screenshot(options.artifactDir, "03-add-transition")

    appendLog(options.artifactDir, "setting example path and previewing")
    try runAppleScript(setFileAndPreview)
    sleep(2)
    screenshot(options.artifactDir, "04-previewed")

    appendLog(options.artifactDir, "triggering and deleting transition")
    try runAppleScript(triggerAndDelete)
    sleep(2)
    screenshot(options.artifactDir, "05-trigger-delete")
} catch {
    appendLog(options.artifactDir, "automation failed: \(error)")
    screenshot(options.artifactDir, "99-failure")
}

appendLog(options.artifactDir, "terminating OBS")
process.terminate()
