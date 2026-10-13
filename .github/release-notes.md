## Cortex BETA 0.02 — now on macOS

**Downloads**
- `Cortex-0.02.dmg` — macOS 11 or later, one universal app for Apple silicon and Intel Macs
- `Cortex-0.02-windows.zip` — Windows 10/11, the same single `AI-IDE.exe` as before

### Installing on a Mac
1. Open `Cortex-0.02.dmg` and drag **Cortex** onto **Applications**.
2. The app is not notarized by Apple, so the first launch needs one extra step:
   open it from Applications, close the warning, then go to
   **System Settings → Privacy & Security** and click **Open Anyway**.
   (On macOS 14 and earlier, Control-click the app and choose **Open** instead.)
   Or, from Terminal: `xattr -dr com.apple.quarantine /Applications/Cortex.app`
3. Install [Ollama](https://ollama.com) and pull a model, for example
   `ollama pull qwen2.5-coder`. Cortex finds it at `http://127.0.0.1:11434`.

On macOS the shortcuts use **Cmd** where Windows uses **Ctrl**, and **Option**
for **Alt**; the menus and the command palette show the Mac keys.

### New in this version
- **macOS app**: native window and file dialogs, Retina text, SF Mono, Cmd
  shortcuts and Mac text-editing keys (Cmd/Option + arrows and Backspace).
- **Cursors**: six caret shapes and five blink styles from VS Code, caret
  width, smooth caret animation and an overtype mode.
- **Multiple cursors**: Alt/Option+Click, Ctrl/Cmd+Alt+Up/Down, Ctrl/Cmd+D,
  select all occurrences, cursors at line ends, column selection and cursor undo.
- **Tools**: command palette, quick open with go to line and go to symbol, find
  and replace with regex, find in files, line commands, case transforms,
  expand selection, bracket matching, minimap, rulers, rendered whitespace and
  font zoom.
- **Workbench**: VS Code-style menus, tab context menu and tab cycling, reopen
  closed tab, save all and a clickable status bar.
