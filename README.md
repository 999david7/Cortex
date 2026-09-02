<<<<<<< HEAD
"# Cortex" 
=======
# AI IDE

A local code editor written entirely in C++ that talks to your own models through
[Ollama](https://ollama.com). No cloud service, no account, no telemetry — the editor
runs on your machine and the model runs on your machine.

![AI IDE](docs/screenshot.png)

---

## What it does

**Editor**
- Code editor written from scratch on top of Dear ImGui draw lists
- Syntax highlighting for ~90 languages (C/C++, C#, Java, Kotlin, Swift, Go, Rust,
  Zig, Python, Ruby, PHP, JS/TS/JSX/TSX, Svelte, Vue, Haskell, OCaml, F#, Elixir,
  Erlang, Clojure, Lua, R, Julia, SQL, GraphQL, Protobuf, HTML/CSS/SCSS, YAML,
  TOML, Terraform, Nix, Dockerfile, Makefile, CMake, Assembly, Fortran, …)
- Every language has its own file mark in the tree and on the tab: a hairline
  square carrying its short code, with the language's colour as a rule along
  the bottom edge
- Source colours for paper — ink for identifiers, brick for keywords, ochre for
  literals, grey for everything structural

**Look**
- Editorial rather than app-like: paper white, ink black, hairline rules, square
  corners everywhere and a 1px frame on every pane
- Nothing is filled unless it is *active* — the open file, the selected model and
  the editor mode are solid black with their type knocked out white, and
  everything else sits on the paper
- Every label in the chrome is small, uppercase and widely tracked; the panes are
  numbered `Ø1 EXPLORER`, `Ø2 EDITOR`, `Ø3 ASSISTANT`, `Ø4 TERMINAL`, and the
  focused one is marked with a filled square and an ink frame
- Open files sit in a rail of numbered cells that carry the index and language
  above the name
- Monospace throughout, chrome included, so everything sits on one character grid
- Beyond the ink there are two hues in the whole interface, and they only ever
  mark data: ochre for a warning, brick for an error
- Line numbers, current-line highlight, indent guides, multi-line selection
- Undo/redo with typing coalescing, auto-indent, bracket and quote auto-closing
- Find with match highlighting and counter (`Ctrl+F`)
- Comment toggling, line duplication, moving lines, indent/outdent of a selection
- Tabbed documents with a modified marker, drag-and-drop file opening

**AI (any Ollama model)**
- Model picker fed live from `/api/tags` — every model you have installed shows up,
  and a model in your settings that is no longer installed is reported and swapped
  for one that is, rather than failing every message
- Streaming chat: tokens appear as they are generated, with a Stop button
- The open file (or just your selection) is attached to each message automatically
- Markdown rendering with headings, lists, inline code and syntax-highlighted code
  blocks, each with **Copy** and **Insert into editor** buttons
- Reasoning models: `thinking` output is captured and shown in a collapsible section
- One-click actions: Explain, Improve, Fix, Write tests
- The model works on the project directly through tools: `read_file`,
  `list_directory`, `search_files`, `edit_file`, `create_file`, `create_directory`.
  Writes are shown as a card with a preview and wait for approval; reads run
  immediately. Every result — including failures — is fed back to the model, so it
  can correct itself instead of inventing an outcome.
- `edit_file` replaces an exact snippet rather than rewriting a whole file, which
  is what makes edits practical for small local models
- Guard rails around what a small model gets wrong when it builds something new:
  a text tool cannot create a `.jpg`, `.png`, `.woff` or any other binary asset,
  so that call is refused with a note to draw in SVG or CSS instead — which is
  what stops a generated site from arriving full of 15-byte "images"
- Every page the model writes is checked against the files it references: a
  missing stylesheet, script, image or linked page comes straight back in the
  tool result, so the model fixes the dead link in the same turn instead of
  reporting success over a broken site
- The transcript is compacted when it approaches the context window, so a long
  build does not silently push its own instructions out of the model's memory
- File work runs at its own low sampling temperature (Settings → Model), because
  the sampling that makes prose readable is what makes a small model drift off
  the instructions it was just given
- The system prompt carries a map of the project, so the model knows what exists
  before it starts guessing
- Inline completion at the caret (`Ctrl+Space`) shown as ghost text, `Tab` accepts
- Tokens/second and token count reported per reply

**Live preview**
- A local web server for the open folder, in the shape of VS Code's Live Server:
  `Alt+L`, the **Go live** button over the editor, or *View → Go Live*
- Serves the current page at `http://127.0.0.1:5500/...` and opens the browser on
  it; any other kind of file opens the site root
- Live reload: a small script is woven into every HTML page the server sends, the
  folder is watched with `ReadDirectoryChangesW`, and the open tab reloads itself
  as soon as a file changes — from the editor or from anywhere else
- `index.html` resolution per directory, a directory listing where there is none,
  correct MIME types, byte ranges (so `<video>` can seek) and no-store caching so
  a reload never shows the previous version
- Bound to `127.0.0.1` only, so the folder is reachable from this machine and
  nowhere else, and the server refuses any path that resolves outside the root.
  A busy port moves up to the next free one
- Port and auto-reload live in *Settings → Preview*; the status bar shows
  `LIVE <port>` while it runs

**Workspace**
- File tree with type-colored icons, lazy directory loading and a filter box
- Integrated terminal panel: run build/test commands in the workspace folder,
  output streamed live
- Dockable, rearrangeable panels; the layout is remembered between sessions
- Custom frameless window with a native feel (snap, resize, rounded Win11 corners)

---

## Requirements

- Windows 10 or 11, 64-bit
- [Ollama](https://ollama.com) running locally with at least one model pulled:
  ```bash
  ollama pull qwen2.5-coder
  ```
- A C++17 compiler: [w64devkit](https://github.com/skeeto/w64devkit/releases)
  extracted to `tools\w64devkit` (so `tools\w64devkit\bin\g++.exe` exists), or any
  MinGW-w64 `g++` on `PATH`. See [`tools/README.md`](tools/README.md).

No other dependencies. Dear ImGui is vendored in `vendor/imgui`; everything else is
the Windows SDK (Direct3D 11, WinHTTP, DWM, COM).

---

## Build

```bash
build.bat
```

That puts the local toolchain on `PATH` and runs `make`. The result is
`build\AI-IDE.exe`, a single self-contained executable (statically linked runtime, so
it also runs on machines without a compiler installed).

Other targets:

```bash
make DEBUG=1
```

```bash
make clean
```

> If the link step fails with a permission error, the IDE is still running — close it first.

---

## Run

```bash
build\AI-IDE.exe
```

A file or folder can be passed as an argument:

```bash
build\AI-IDE.exe src\app.cpp
```

The window renders through Direct3D, so no external screen grabber can capture
it. `--screenshot <path> [frames]` renders that many frames, writes the back
buffer to a BMP and exits:

```bash
build\AI-IDE.exe --screenshot shot.bmp 90
```

On first start, open a folder (`Ctrl+K`) and pick a model in the AI panel. If Ollama is
running, the status dot next to the model name is green and the model list fills itself.

`examples\demo.cpp` is a small playground file for trying the AI features.

---

## Keyboard shortcuts

| Keys | Action |
| --- | --- |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+K` | New file / open file / open folder |
| `Ctrl+S` / `Ctrl+Shift+S` | Save / save as |
| `Ctrl+W` | Close tab |
| `Ctrl+F` | Find in file (`Enter` / `Shift+Enter` to step) |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+D` | Duplicate line or selection |
| `Ctrl+/` | Toggle comment |
| `Tab` / `Shift+Tab` | Indent / outdent selection |
| `Alt+Up` / `Alt+Down` | Move lines |
| `Alt+L` | Go live: serve the folder and open the browser |
| `Ctrl+Space` | AI completion at the caret |
| `Tab` (on a suggestion) | Accept the AI suggestion |
| `Ctrl+L` | Send the file or selection to the chat |
| `Enter` / `Ctrl+Enter` | Send chat message / new line in the message |
| `Ctrl+,` | Settings |

---

## Settings

Everything is configurable from **File → Settings**: Ollama host, chat model,
a separate completion model, temperature, top-p, context window, how much code is
attached to a message, the system prompt, and the editor preferences.

Configuration lives in `%APPDATA%\AI IDE\`:

- `settings.json` — preferences, recent folders, selected models
- `layout.ini` — panel layout

Deleting those files resets the app to defaults.

Pointing `ollama_host` at another machine works too (for example
`http://192.168.1.50:11434`), as does any server that speaks the Ollama HTTP API.

---

## Source layout

| File | Purpose |
| --- | --- |
| `src/main.cpp` | Win32 frameless window, Direct3D 11 device, frame loop |
| `src/app.cpp/.h` | The IDE: layout, explorer, tabs, chat, terminal, settings |
| `src/texteditor.cpp/.h` | The code editor widget and the syntax tokenizer |
| `src/lang.cpp/.h` | Language registry: detection, keywords, badges, colors |
| `src/agenttext.cpp/.h` | Recovering tool calls from reply text; edit_file matching |
| `src/ui.cpp/.h` | Palette, fonts, hand-drawn vector icons, shared widgets |
| `src/ollama.cpp/.h` | Ollama client — model list, streaming chat and completion |
| `src/http.cpp/.h` | Minimal WinHTTP client with streaming callbacks |
| `src/webserver.cpp/.h` | Local live-preview HTTP server |
| `src/json.cpp/.h` | Small JSON parser/writer |
| `src/sys.cpp/.h` | Files, native dialogs, child processes |

Requests run on background threads and hand results to the UI through a mutex-guarded
snapshot, so streaming never blocks the frame loop.

---

## Notes and limitations

- Windows only. The editor, UI and AI layers are portable C++; the window creation,
  file dialogs, process handling and HTTP client are Windows-specific and would each
  need a counterpart on other platforms.
- The font atlas covers Latin-1, Latin Extended-A and common punctuation. Files with
  CJK or other scripts will show placeholder glyphs.
- Every font in the UI is monospace, picked from SF Mono, Cascadia Mono,
  JetBrains Mono, Fira Code, IBM Plex Mono or Consolas, in that order. Only
  Consolas ships with Windows, so installing one of the others is the easiest way
  to change the whole look. A proportional face is used only if no mono is found.
- A model has to fit in memory before it can answer. If a reply fails instantly
  with an allocation error, the model is too large for the machine or the context
  window is set too high — the chat panel says so and suggests the fix.
- Many local models know the tool schema but skip the wrapper tokens their chat
  template needs, so the call arrives as ordinary message text. The IDE recovers
  those (and repairs a missing closing brace, which is common), then reports the
  result back through the normal tool protocol.
- `create_file` refuses to overwrite a file the model has not read during that
  turn. Without it, a small model asked to "add a function" will happily replace
  the whole file with just that function.
- `edit_file` matches the snippet exactly when it can, and otherwise ignores
  trailing whitespace and differences in blank lines. The match must still be
  unique, so a tolerant hit is never a guess about which place was meant.
- Inline completion is deliberately conservative: it asks the model for a short
  continuation and shows it as ghost text; nothing is inserted until you press `Tab`.
- Files larger than 64 MB are refused by the editor.
>>>>>>> 31dc400 (Initial commit of the local Ollama Windows IDE.)
