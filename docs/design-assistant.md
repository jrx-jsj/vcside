# VCSIDE — optional AI assistant (proposal)

An agentic assistant inside the IDE that can work with the project's files and with VCSIDE's own tools (build, emulator, knowledge base, graphics and music files). It runs on a small **local** model or, if the user chooses, on a **remote** model through its API. It is off by default, VCSIDE asks once at first start whether you want AI tools at all and which ones (see [First start](#first-start-and-features)), and it works exactly the same without them.

This is a proposal. The decisions the user has already taken (2026-11-10) are listed near the end; what is still open follows them. Nothing here has been implemented. The facts about other projects (APIs, licences, model sizes) come from general knowledge and have **not** yet been checked against their primary sources for this document; each one is marked "to verify" in [Credits](#credits) and must be checked, and recorded in `docs/sources.md`, before the code that depends on it is written.

## What already exists

### Runtimes for local models

| Tool | What it is | Licence | Use for VCSIDE |
|---|---|---|---|
| llama.cpp (Georgi Gerganov and contributors) | C/C++ inference engine for GGUF models. Its `llama-server` serves an OpenAI-compatible HTTP API with streaming and, with a chat template that supports it, tool calls. It can also constrain the output to a grammar or JSON schema. | MIT | The reference local runtime. VCSIDE talks to it over HTTP. |
| Ollama | A runner and model manager built around llama.cpp, with a local server (default port 11434), its own API and an OpenAI-compatible one. | MIT | Same HTTP path. The easiest start for most people: one installer, one command to fetch a model. |
| LM Studio | A desktop app that runs models and offers a local OpenAI-compatible server (default port 1234). | Proprietary, free to use | Same HTTP path. Not redistributable, never bundled. |
| Others (vLLM, KoboldCpp, Jan…) | Also expose an OpenAI-compatible server. | Varies | Work without extra code if the base URL is configurable. |

### Remote APIs

- **OpenAI-style "chat completions"** with `tools` (function calling) and server-sent-event streaming. It has become the common dialect: the three local runtimes above, many hosting services and several vendors' own APIs offer it. One client covers all of them.
- **Anthropic Messages API**: its own request shape, `tool_use` and `tool_result` content blocks and its own streaming events. Different enough to need a second client.
- Both are plain HTTPS with a bearer-style key.

### Tool protocol

**MCP (Model Context Protocol)**, published by Anthropic in 2024: JSON-RPC between a *client* (the agent host) and *servers* that expose tools, resources and prompts, over stdio or HTTP. It is a way to plug tools into an agent, not a way to talk to a model. It matters here in two directions, and neither is needed for a first version:
- VCSIDE as an **MCP server**: external agents could drive the IDE's tools. Cheap if VCSIDE's own tool layer is protocol-neutral (it will be).
- VCSIDE as an **MCP client**: the assistant could use the user's other MCP servers. This runs arbitrary third-party programs with the agent's reach, so it is a bigger security surface and is left for much later.

### Do small local models call tools?

Partly. Models in the 7–9B range and up, trained or fine-tuned for it, usually produce valid single tool calls when the runtime applies the right chat template; below about 4B, calls are unreliable. Whether it works depends on the model *and* the runtime's template, so VCSIDE must **test it**, not assume it (see [Small local models](#small-local-models)).

### Integrate or build?

| Piece | Decision | Why |
|---|---|---|
| Inference engine | **Revised 2026-11-10: ship a helper, still a separate process.** VCSIDE builds llama.cpp's server from source with the IDE, installs it next to the executable, starts it on demand on a loopback port and stops it when the assistant is closed; the user never sets up a server. Ollama, LM Studio and any other server stay as advanced options. (Original decision, kept for the reasons: *do not bundle, do not link; talk to a separate process over HTTP.*) | llama.cpp and Ollama are MIT, so bundling would be legal. But: builds differ per GPU backend (Metal, CUDA, Vulkan), the Windows build here is a static MinGW cross-build, the binary is large, and models and engines change monthly. A separate process also isolates crashes and out-of-memory, and uses the same code path for local and remote. It follows the rule already in `design.md`: tools are found, not redistributed. |
| Models | **Never bundled; downloaded only after the user confirms.** The settings window and the panel offer a catalogue (`data/assistant/models.json`) with each file's size, licence, source host and a pinned SHA-256, checked after every download. | 2–20 GB each, with licences that differ per model (some permissive, some with use restrictions). The user picks and installs them. |
| Agent loop, tool layer, permissions, panel | **Build.** | It is the part that knows VCSIDE: project, editor buffers, build, emulator, knowledge base, `.vcsgfx` and `.vcsmusic`. No existing agent framework is C++17, small and embeddable, and a framework would bring its own interface and ideas into a tool that is meant to look and behave as one thing. The loop itself is simple (see ReAct in the credits). |
| HTTP client | **Build a thin layer on a small library** (below). | |
| MCP | **Server first, if ever; client much later.** | See above. |

## Architecture

New code goes in `src/assistant/`; the panel is the only part that touches ImGui.

```
src/assistant/
  assistant.*        state of the conversation, the agent loop, limits
  provider.*         the Provider interface and its two wire dialects
  http.*             HTTP/SSE client (worker threads only)
  tools.*            the tool registry (name, JSON schema, handler, permission tier)
  tools_project.cpp  read / search / edit files
  tools_build.cpp    build, errors, space report, emulator
  tools_kb.cpp       knowledge base lookup
  permissions.*      what needs approval, path sandbox, session grants
  context.*          prompt assembly, truncation, token estimates
  secrets.*          API keys (platform backends)
  assistant_panel.*  the dockable ImGui panel
data/assistant/      system prompt and tool descriptions (plain text, CC0)
```

A CMake option `VCSIDE_ASSISTANT` (on by default) leaves the whole directory out of the build, for people who want an IDE with no network code at all.

### Provider abstraction

A *provider* is a saved configuration, not code:

| Field | Meaning |
|---|---|
| `name` | Shown to the user. |
| `dialect` | `openai` (chat completions) or `anthropic` (Messages). |
| `base_url` | `http://127.0.0.1:11434/v1`, `https://…`, any compatible endpoint. |
| `model` | Free text, plus a list fetched from the endpoint when it offers one. |
| `key` | A *reference* to a secret (see [Privacy](#privacy-and-secrets)), never the key. |
| `context_tokens` | The model's window; used to trim. Read from the server when it reports it, else asked. |
| `tools` | `auto` (probe), `native`, `prompted` or `none`. |
| `local` | Derived from the host: loopback is *local*, anything else is *remote*. Drives the privacy badge. |

C++ side: one `Provider` interface (`stream(Request, EventSink&, CancelFlag&)`), two implementations (`OpenAiProvider`, `AnthropicProvider`) that translate VCSIDE's neutral messages and tool schemas to their wire format and back. Everything above that layer sees only neutral events: *text delta*, *tool call started / argument fragment / complete*, *usage*, *done*, *error*. A third dialect is one more class.

**Recommendation:** two dialects, no per-vendor code. Ollama's own API is not needed: it speaks the OpenAI dialect too. Google and others that offer a compatible endpoint work through the first dialect.

### HTTP and streaming

Both dialects stream with server-sent events (`data: {json}` lines; the OpenAI one ends with `data: [DONE]`; tool-call arguments arrive as JSON fragments to be concatenated). Options:

| Library | Licence | Notes |
|---|---|---|
| cpp-httplib (yhirose) | MIT | One header, blocking client, content-receiver callback fits SSE and cancellation. TLS only through OpenSSL or mbedTLS, which would be a new dependency. |
| libcurl | curl licence (MIT-style) | Mature, has TLS through the system on macOS and Windows, proxies, redirects. Heavier to build in the static MinGW Docker build. |
| Platform APIs (NSURLSession, WinHTTP, libcurl or OpenSSL on Linux) | System | No TLS library to ship on two systems, but three backends to write and test. |

**Decision:** each system's own TLS, so no crypto library is shipped, updated or audited by VCSIDE and the operating system's certificate store and proxy settings are used. One small `HttpClient` interface, three backends, all streaming with a callback and all cancellable:
- **macOS:** `NSURLSession` (VCSIDE already has an Objective-C++ file for the menu bar).
- **Windows:** WinHTTP, which also works in the static MinGW build.
- **Linux:** the system's **OpenSSL**, loaded with `dlopen` (libssl 3 only, by its soname; the few function prototypes needed are declared in one VCSIDE header, so OpenSSL's headers are not needed to build) only when the feature is enabled and a remote provider is used (local loopback needs no TLS); the distribution keeps it patched and supplies the certificate bundle. OpenSSL 3 is Apache-2.0, compatible with GPL-3.0-or-later. **Why `dlopen` and not a normal link:** a normal link would make VCSIDE refuse to start on a machine without libssl 3, for a feature most users keep off; with `dlopen` the IDE always starts, and a missing library is reported only when a remote provider is used ("install OpenSSL 3 or use a local model"). The price is a thin wrapper to write and test; libssl 1.1 systems are not supported.
- **Plain HTTP to loopback** (the local runtimes) goes through the same interface, so phase 1 needs only the simplest backend path. cpp-httplib (MIT) stays an option for that plain-HTTP path and for the test mock server, where it is already enough.

The cost is writing and testing three backends instead of one; the mock server in the tests exercises all three against the same scripted streams, and TLS itself is tried by hand against a real endpoint on each system (the Windows one only in Wine, like the rest of the Windows build).

The parser for SSE is VCSIDE's own and is tested against chunks cut at every byte.

### Threading

The UI thread must never wait on a network.

- **One worker thread per request**, started like `Job` (`std::thread`, atomics, a mutex-guarded queue). It does the HTTP, parses SSE and pushes neutral events.
- **The UI thread drains the queue once per frame** (the main loop already wakes every 50 ms while something runs) and updates the panel.
- **Tools run on the UI thread**, because they touch `App` state: editor buffers, the open files, the build `Job`. The worker never reads or writes a project file. A tool call becomes an event; the UI thread runs it, or shows an approval card and waits for the click, then posts the result back and the loop continues with a new request. A long tool (a build) is asynchronous: its result is posted when the `Job` finishes.
- **Cancel** is an atomic flag. The HTTP receiver returns `false` when it is set, which closes the connection; a running build is not killed by Stop unless the user also stops it in the Output panel.
- Timeouts: connect 5 s; no data for 120 s (a local model can take long to read a large prompt before the first token).

### The panel

A dockable *Assistant* panel beside *Output* and *Problems*, in VCSIDE's usual look (theme roles, dialog style for approval cards). It does not exist until the feature is enabled in *Settings → Assistant*.

- **Conversation:** user and assistant messages, with fenced code blocks drawn in the editor font with *Copy* and *Apply…* buttons (no full Markdown renderer: headings, lists, code blocks and inline code are enough).
- **Tool calls** appear as one-line rows ("read `main.asm` lines 1–120", "build → 2 errors"), expandable to the exact arguments and result. Nothing the agent does is hidden.
- **Approval cards** for anything that changes something (see [Permissions](#permissions-sandbox-and-undo)).
- **Context as chips (2026-11-10, replaces the switches and the line that named what was sent).** A row above the input shows what goes along with the question, and each chip has a cross. The **open file** is there by itself, or **`main.asm:12-20`** when lines are selected (the selection stands for the file, and brings its own name and lines to the model); **Build errors** appears when the last build has any. A chip taken out stays out until its source changes: another file, another selection or other errors bring it back. Typing **`@`** at the start of a word opens a list of the project's files and folders, filtered as you type (names that start with it first, then names that contain it, then paths, then the letters in order; shallow paths first), with Up, Down and Enter, or the mouse; the choice becomes a chip and the `@name` leaves the text. A folder adds its text files, up to eight, and says so when there are more. Hidden folders, `build/`, binaries and files that may hold secrets are never listed. What the chips come to is cut to the model's window (about two characters per token of it, selection or file first, then errors, then the added files), and the sent message keeps a line, "Sent with: ...", with what was cut. The project index (below) still adds its own excerpts and the map of files; the chips are what the user chose, the index is what the assistant found.
- **Context without switches (first version, replaced by the chips above).**
- **A badge that is always visible:** `local` (loopback) or `remote: <provider>`, because whether the code leaves the machine is the first thing to know.
- **Request inspector:** a button that shows the exact JSON about to be sent (system prompt, tool schemas, messages, attached text), so nobody has to trust a description of what is sent.
- **Stop**, a token counter (estimated, or the server's if it reports usage) and *New conversation*.
- **Conversations are saved per project in `.vcsai/chats/`**, one file each, with *Delete* and *Archive* buttons (archived ones move to `.vcsai/chats/archive/` and leave the list). A conversation holds code and tool results, so VCSIDE writes `.vcsai/.gitignore` ignoring `chats/` when it creates the folder; the entry is rewritten if removed, since conversations must not reach git. Keys and approvals are never written there.

## First start and features

Many people will not want AI in their editor, so the choice is theirs and it is asked **once, at the first start after the version that has the feature**, before any assistant code does anything:

> *VCSIDE can include optional AI tools. Nothing is sent anywhere and nothing is installed unless you turn them on and set up a model.*  
> ☐ Chat (ask questions about your project and the 2600)  
> ☐ Agent (the chat can read files, propose edits, build and run, always asking you first)  
> ☐ Extra code completion (suggestions as you type, from a small local model)  
> ☐ Explain errors (a button on build errors)  
> **[ Use none ]**  **[ Use the selected ]**  — you can change this at any time in *Settings → Assistant*.

*Use none* is the default button. Features are independent switches: *Agent* includes *Chat*, the other two work alone. A feature that is off has no menu entry, no panel and runs no code; choosing *none* also hides the *Assistant* settings page behind a single line, "AI tools are off — turn on". The answer is stored in the user settings (not in any project), and a project can only ever turn things **off** for itself (`.vcsproj` key), never on.

*Extra code completion* is a different kind of use from chat: it needs a model trained to fill in the middle of code, small and fast (the 1–7 B class, local), and sends the code around the cursor on every pause. Because of that, it is allowed only with a **local** provider unless the user explicitly picks a remote one for it, and it is the last phase of this proposal.

## The knowledge base is the assistant's backbone

Models know little about the 2600 and invent registers, bits and cycle counts. VCSIDE has something most assistants lack: a knowledge base (`data/kb`: 6502 opcodes, TIA and RIOT registers, cartridge formats, TV standards, batari Basic, emulator options) in which every datum cites its source and carries a `verified` flag. The assistant is built around it, in three layers:

1. **Automatic grounding.** Before each request VCSIDE looks for names it knows in what the model will see (the user's message, the selection, the open file around the cursor, the build errors): `RESP0`, `LDA`, `F8`, `pfcolors`, `$F000`… Each hit that matches a knowledge-base id is attached, in a compact form (id, summary, bits or cycles, `issues`, `verified`, source ids), up to a budget (default 3 entries, about 600 tokens), most specific first. This works in every mode, **including plain chat with a model that cannot call tools** and with 3 B models, which is where it matters most. The attached entries are visible in the panel as chips ("TIA: RESP0") and in the request inspector.
2. **Tools for more.** `kb_lookup` and `kb_search` let an agent ask for what grounding did not attach. The system prompt says: for any fact about the hardware, the cartridge formats, an instruction's cycles or a batari Basic word, **look it up; if it is not in the knowledge base, say so instead of guessing**.
3. **Answers that cite and admit doubt.** The prompt asks the model to cite the entry it used, and the panel turns a cited id into a link that opens it in the *Docs* panel (the same `showDoc` the editor's hover help uses). An entry with `verified: false`, and the list of unverified facts in `docs/sources.md`, are passed through so the model says "this is not confirmed" rather than stating it as fact.

Details:
- **No vector database.** The knowledge base is a few hundred entries in JSON: an in-memory index on ids, names and words (a simple ranked keyword match) is enough, adds no dependency and works offline. Embeddings could come later if the keyword search proves weak; that would be a separate decision.
- **Project-aware.** Entries are chosen with the project in mind: its cartridge format and TV standard are in `project_info`, so a question about "the bankswitching" or "the frame" is answered for that project's format and standard.
- **Language.** The knowledge base is English; the model answers in the user's language and keeps ids and register names untouched. When `data/kb/<code>/` layers arrive (see `design.md`), the grounding uses the user's language with the same fallback.
- **Licence.** `data/kb` is CC BY-SA 4.0. Entries that reach a prompt keep their ids and source ids; the *About* credit already covers them.
- **Updated for free.** The assistant reads the same files as the editor's completion and hover help, so a corrected datum improves all three.
- **Testing.** A test checks that grounding finds the right entries for a set of questions and error messages, and that no entry is attached twice; the manual evaluation (below) scores answers *with* and *without* grounding on the same models, which also measures how much the knowledge base is worth.

## The tools the agent gets

A tool is a name, a JSON schema, a handler and a permission tier. The registry is protocol-neutral, so the same definitions go out as OpenAI `tools`, Anthropic `tools`, text in a prompted-tools prompt, and later MCP.

| Tool | What it does | Tier |
|---|---|---|
| `project_info` | Language, main file, format, TV, output ROM, tools in use, size of the last build's space report. Small. | read |
| `list_files` | Files and folders under a path (depth-limited), with sizes. | read |
| `read_file` | Lines `from..to` of a file (default the first 200, hard cap per call). | read |
| `search` | Text or regex over project sources, with file, line and one line of context; capped results. | read |
| `symbols` | Labels, subroutines, macros, constants from VCSIDE's existing source scan (`liveSymbols_`), and where each is defined. Cheaper than reading files. | read |
| `kb_lookup` | One entry of the knowledge base by kind and id: opcode, TIA or RIOT register, cartridge format, TV standard, batari Basic word, emulator option. | read |
| `kb_search` | Free-text search over the whole knowledge base (names, descriptions, bits, notes); returns ids and one-line summaries. | read |
| `build_errors` | The diagnostics of the last build (file, line, message), as *Problems* shows them. | read |
| `space_report` | ROM and RAM left per bank, as the *Space* panel shows them. | read |
| `edit_file` | A set of search-and-replace edits to one file, or a new file. **Proposed, never applied directly**: see below. | propose |
| `build` | Builds the project with the configured tools, returns the errors. | run |
| `run_emulator` | Launches the project's ROM in the emulator the project uses. | run |

The graphics and music files need no special first version: `.vcsgfx` and `.vcsmusic` are readable JSON, so `read_file` and `edit_file` work on them. What VCSIDE adds is **validation**: an edit to either is parsed with the real loader before the user sees it, and a file the editor could not open is returned to the model as an error instead of being offered. Later (phase 5) higher-level tools may help small models, for example `gfx_describe` (the rows of a sprite as the editor draws them, with the kernel profile's warnings) and `music_describe` (the registers per frame, as the *Registers* view shows them).

Left out on purpose: a general shell, arbitrary network access, deleting files, reading anything outside the project, changing VCSIDE's settings, and installing or downloading anything.

### How edits are expressed

Small models are poor at writing exact line numbers or unified diffs. `edit_file` takes one or more `{search, replace}` pairs: the text to find (which must match exactly once; if not, the tool returns an error saying why and what is near) and its replacement. This is the approach the Aider project popularised. VCSIDE shows the result as a diff for the user to accept, per file and per hunk.

## Permissions, sandbox and undo

| Tier | Examples | Behaviour |
|---|---|---|
| read | list, read, search, symbols, kb, errors | Runs without asking, **inside the project folder only**, and is shown in the conversation. |
| propose | `edit_file` | Nothing changes until the user accepts. A diff card with *Apply once*, *Apply and allow edits to this file this session* and *Reject*. A session grant covers only the files named, and never a privileged file (below). |
| run | `build`, `run_emulator` | Asks, with two buttons: **Allow once** and **Allow for this session** (this conversation and this tool). The session grant is never saved and ends when the conversation is closed or archived. |
| never | shell, network, delete, outside the project | Not offered to the model at all. |

**Sandbox.** Every path is resolved to a real path and must be inside the project folder: `..`, absolute paths, symlinks that leave the folder and, on Windows and macOS, case and reserved-name tricks are rejected, and tests cover each. Files that commonly hold secrets (`.env*`, `*.pem`, `*.key`, `id_*`, anything the user lists in the project's settings) are never read or sent, whatever the model asks. Writes outside the project's source files (`build/`, ROMs and other binaries) are refused.

**Build-affecting files are privileged.** The `.vcsproj` can set `build_command`, which runs through the shell, and a `Makefile` or build script runs whatever it contains. An edit to any of them is flagged "this changes what *Build* runs", cannot be covered by a session grant, and the next `build` asks again even if builds were allowed for the session. Without this rule, "edit the Makefile, then build" would be arbitrary code execution that skipped the approval card.

**Content is data.** File contents, build output and knowledge-base text are passed to the model labelled as data, and the system prompt tells it so. This does not make injection impossible (a small model can be fooled by a comment in a source file), so the real protection is structural: the agent has no shell or network tool, every change needs a click, and a remote provider only receives what a tool returned.

**Limits per turn:** at most N tool calls (default 12), a cap on bytes returned by each tool, and the loop stops when the same call repeats three times. With a remote provider an optional session budget (estimated tokens) can be set.

**Undo.** An accepted edit is applied to the file's **editor buffer**, so it is one undoable step in the editor like any other change, and the file is saved by the normal rules (a saved `.vcsgfx` regenerates its `.gfx.asm`, as it does today). Before the first change of a conversation, VCSIDE keeps the original text of each touched file in memory, and the panel has *Undo everything this conversation changed* that restores them. Git is recommended but not required.

## Small local models

What to expect, as rough guidance to be checked with real runs in phase 6 (sizes are for 4-bit quantisation; model names change quickly, so the documentation will name the *classes*, not a leaderboard):

| Size | Memory (about) | What it does reasonably | What it does not |
|---|---|---|---|
| 1–4 B | 1–3 GB | Explain an error, explain a register when the knowledge base text is in the prompt, short snippets. | Reliable tool calls; multi-step work. Plain-chat mode only. |
| 7–9 B | 5–7 GB | Single tool calls in sequence (read, search, propose a small edit), fixing a clear assembler or compiler error. | Long plans; staying on task for many steps; remembering TIA timing without help. |
| 14–32 B (including mixture-of-experts models with few active parameters) | 10–24 GB | Multi-step fixes, writing a routine in batari Basic or C for ELF, following the project's conventions. | Cycle-exact 6502 kernels without checking: models know little about the 2600 and invent registers and timings. |

Two consequences shape the design:

1. **Ground the model in `data/kb`, don't trust its memory of the 2600** (see [The knowledge base is the assistant's backbone](#the-knowledge-base-is-the-assistants-backbone)): automatic attachment of the relevant entries, `kb_lookup` and `kb_search`, and the `verified` flag passed through.
2. **Everything is checked by a build.** The most useful loop for a small model is *edit → build → read errors → fix*, because the assembler is an oracle the model does not have to be right about.

### Keeping prompts and schemas small

- **System prompt ≤ ~500 tokens**, plain text in `data/assistant/`, readable and editable by the user.
- **Tool schemas ≤ ~1,000 tokens in total**: short names, one-sentence descriptions, no optional parameters that are rarely used. A *profile* setting offers fewer tools (*read-only*, *read + edit*, *all*) for models that get confused by many.
- **Context is fetched, not preloaded.** The prompt carries `project_info` in a few lines; the model asks for files, symbols and knowledge-base entries as needed.
- **Hard caps:** `read_file` returns at most 200 lines by default; `search` at most 20 results; a build's errors at most 30.
- **Trimming** when the window fills: first old tool results are replaced by a one-line stub ("read `main.asm` 1–120, 4 KB, dropped"), then old assistant turns; the system prompt and the latest turns are never dropped. Token counts are estimated (characters / 3.5) unless the server reports usage.

### Falling back: three levels

The first time a model is used, VCSIDE sends a tiny test ("call `ping` with `x=1`") and records the result for that provider and model. Then:

1. **Native tools:** the dialect's `tools` field. Used when the probe passes.
2. **Prompted tools:** the tools are described in the prompt and the model answers with one fenced JSON block, which VCSIDE parses. Where the runtime supports it (llama.cpp's grammar or JSON-schema constraint), the answer is constrained so it is always valid. Used when native calls fail the probe but the model follows instructions.
3. **Plain chat:** no agent at all. The attached context (selection, errors, open file) is the text, and any code block in the answer has *Apply…*, which opens the same diff card. Always available, and already useful.

The user can force a level per provider.

## A catalogue of local models

**Status: published as provisional.** It is coherent (one recommended default per machine size, below) but unmeasured. Until phase 6, the documentation and website show it with the date and this label.

Models change every few months, so this is a catalogue of **families and classes**, not a ranking; it is dated 2026-11-10, comes from general knowledge (nothing here has been downloaded or run for this proposal), and the phase 6 evaluation replaces it with measurements. The licence column is what each family has been published under as far as known, **to verify before listing any model in the documentation**; VCSIDE never bundles a model, so the licence binds only the user, but the documentation should say which are permissive.

Sizes are for 4-bit GGUF files; add 1–4 GB for a long context. "MoE" = mixture of experts: the file is big but only a few billion parameters are active per token, so they run faster than their size suggests if the memory is there.

| Class | Memory | Families worth trying | Licence (to verify) | Good for |
|---|---|---|---|---|
| **Tiny, 1–4 B** (any laptop) | 1–3 GB | Qwen small sizes, Gemma small sizes, Llama 3.2 1B/3B, Phi mini | Apache-2.0 (Qwen, some sizes differ); Gemma and Llama have their own terms; Phi MIT | Plain chat with grounding; explaining an error. |
| **Small, 7–9 B** (8 GB RAM machines, or a modest GPU) | 5–7 GB | Qwen coder and general 7–8 B, Llama 3.1 8B, Gemma 9B, Granite 8B, DeepSeek-Coder-class 7B | as above; Granite Apache-2.0 | Chat with grounding, single tool calls, fixing clear build errors. The **minimum for the agent**. |
| **Medium, 12–15 B** | 8–11 GB | Mistral Nemo 12B, Phi-4 14B, Qwen 14B | Apache-2.0 (Mistral); MIT (Phi) | Multi-step edits with a build loop. |
| **Large, 20–35 B** (16–32 GB machines, Apple Silicon with 32 GB+) | 14–24 GB | gpt-oss 20B (MoE), Qwen 30B-A3B-class (MoE) and 32B coder, Mistral Small 24B, Devstral-class, Gemma 27B | Apache-2.0 (gpt-oss, Mistral Small, Qwen mostly); Gemma own terms | The best local experience: reliable tool use, writing a routine in C for ELF or batari Basic. |
| **Very large, 70 B+ or big MoE** (64 GB+ or a workstation) | 40 GB+ | gpt-oss 120B, Llama 70B, Qwen 72B-class | as above | Mostly for people who already own the hardware. |
| **Code-completion (fill-in-the-middle), 1–7 B** | 1–5 GB | Qwen coder base models, StarCoder2, DeepSeek-Coder base, Codestral-class (check licence: some are non-commercial) | varies; check | Only for the *Extra code completion* feature. |

**Recommended defaults, by the memory the user can give the model** (a family and a class, not a version; the settings window shows the runtime's installed models and marks the nearest fit):

| Machine | Chat with grounding | Agent | Code completion |
|---|---|---|---|
| 8 GB RAM, no GPU | Small 7–9 B, or Tiny 3–4 B if slow | Small 7–9 B, expect single steps only | Tiny coder base, 1–3 B |
| 16 GB, or a GPU with 8–12 GB | Medium 12–15 B | Medium 12–15 B | Coder base, 3–7 B |
| 32 GB+, or Apple Silicon with 32 GB+ | Large 20–35 B (MoE if available) | Large 20–35 B | Coder base, 7 B |
| 64 GB+ | Large 20–35 B; Very large only if already owned | Very large or Large | Coder base, 7 B |

The same family should be used across a row when possible (one download, one licence to read). The documentation names **permissive licences first** (Apache-2.0, MIT) and says plainly when a family has its own terms.

What each class means in practice:
- The **agent** is offered by default only from the 7–9 B class up; below it the probe usually fails and VCSIDE falls back to plain chat with grounding (level 3 above), which is still useful.
- For a first-time user the documentation recommends **one default per memory size** (what fits in 8, 16 and 32 GB), picked from the evaluation, and the settings window lists the runtime's installed models with a small "fits this machine" hint from the memory VCSIDE can see.
- Specifically for the 2600, **coder-tuned models are better at C and assembly syntax, general models at explaining hardware**; with grounding the difference narrows, which is another thing the evaluation should measure.
- Quantisation below 4 bits and tiny contexts (under 8 K tokens) break tool calling first; the settings window warns when `context_tokens` is under 8 K with the agent on.

### How the catalogue is chosen

A web search on 2026-11-10 for "best local models for coding agents" returned mostly SEO-style roundups that contradict each other on versions and rankings (the same Qwen model under three version numbers, one site's top pick missing from its own other page). They are **not** used as evidence. What they agree on loosely, and what matches earlier knowledge, is the shortlist of families below; which one is best *for VCSIDE* is decided by measuring.

**Shortlist to evaluate** (family level; the exact current version of each is looked up on its official model card when the evaluation is run):

| Tier | Candidates |
|---|---|
| Tiny, 1–4 B | a small Qwen, a small Gemma, Llama 3.2 3B |
| Small, 7–9 B | a Qwen 8–9 B, Granite 8 B, Gemma 9–12 B, LFM-class small MoE |
| Medium, 12–15 B | Mistral Nemo 12 B, Phi-4 14 B, a Qwen 14 B |
| Large, 20–35 B | gpt-oss 20 B, a Qwen 27–32 B (dense), a Qwen 30 B-A3B-class MoE, Devstral Small (24 B), Mistral Small |
| Very large | gpt-oss 120 B, Devstral 2-class, Qwen Coder large MoE |
| Completion | small Qwen coder base, StarCoder2 |

**Method (phase 6).** The same ten or so tasks (fix a DASM error, fix a batari Basic error, explain RESP0's timing, add a joystick move to the template, edit a `.vcsgfx` sprite, answer from the knowledge base, refuse to invent an unknown register…) are run on every candidate, each in four conditions: plain chat or agent, with and without knowledge-base grounding, at 4-bit quantisation and 8 K / 16 K context. Scores: build passes, answer matches the knowledge base, tool calls valid, steps used, time to answer on stated hardware. The results table, with model file names, quantisations, hardware and date, is committed to the repository and replaces the provisional defaults. Tool-call reliability depends on the runtime's chat template as much as on the model (one report says a runtime's parser dropped parallel calls often), so the same models are tried on llama-server, Ollama and LM Studio and the weakest runtime/model pairs are listed. Results are re-run when a major new family appears; nothing is run in CI.

Principles already visible: Qwen-lineage and gpt-oss models are the most likely agent defaults; coder-tuned models need the edit format to be search-and-replace blocks, which small models handle better than diffs; and the agent harness must stay quiet (few tools, short results), because over-calling tools burns a small model's context.

## Privacy and secrets

- **Off by default.** Until *Settings → Assistant → Enable* is turned on there is no panel, no network code runs and nothing is read. Enabling it does not configure a provider.
- **Nothing is sent unless a provider is configured**, and it is sent to that provider only, never to VCSIDE's authors, never as telemetry, crash data or "improvement" data. The first time a **remote** provider is used, a dialog states what leaves the machine (the messages, and whatever the tools return: file contents, build output) and asks for confirmation; the badge stays on screen afterwards.
- **Local means loopback.** A base URL that is not `127.0.0.1`, `::1` or `localhost` counts as remote, whatever the user calls it.
- **API keys never go in the project, in logs, in conversations or in the request inspector.** The safest option that works everywhere is the operating system's secret store, and it is the only place a key is saved:
  1. **macOS Keychain** and **Windows Credential Manager**, through the system APIs (no dependency).
  2. **Linux Secret Service** (GNOME Keyring, KWallet) through `libsecret`, loaded at run time only when a remote key is saved. If there is no keyring, VCSIDE does **not** fall back to a plain file: the key can be given by an **environment variable** (the setting stores only the variable's name) or kept **for the session only**, in memory, asked again next time.
  3. In every case an environment variable name is accepted instead of a saved key.

  The `.vcsproj` can say *which* provider a project prefers, never a key or a URL with credentials in it. Keys are redacted from any text VCSIDE logs, and the settings window shows only the last four characters.
- **Excluded files** are never sent (see the sandbox). A project-level list lets the user add more.
- **The `.vcsai/` folder.** Everything the assistant keeps for a project lives in one subfolder of the project, so it is easy to see, commit or delete:
  - `.vcsai/STATE.md`: a plain-text file about the project's state and goals that both the user and the assistant read and edit (what the game is, what is done, what is next, conventions, things to avoid). It is added to the prompt, kept short (a size cap, shown in the panel), and changes the assistant makes to it go through the same diff card as any other edit. Because it is part of what a **remote** provider receives, the settings window says so.
  - `.vcsai/chats/` and `.vcsai/chats/archive/`: conversations, git-ignored by default (above).
  - `.vcsai/ignore`: extra files and patterns never to read or send, in addition to the built-in secret patterns.
  - Nothing in `.vcsai/` ever holds a key.
- **Generated code and licences.** What a model writes into a project is the user's responsibility; the assistant does not add licence headers or claim authorship. VCSIDE's knowledge base is CC BY-SA 4.0, and passages of it that reach a prompt keep their source ids.

## Internationalisation

- All interface strings are English source strings through the existing `T()` and `F()`; the Spanish translations go in `data/i18n/es.json` and `tools/i18n_check.py` lists what is missing, as for every other panel.
- **The system prompt and tool descriptions stay in English** (models follow instructions best in English) and tell the model to answer in the user's language. VCSIDE passes the interface language as a hint. They are plain files in `data/assistant/`, so they are not translated and not hidden.
- The knowledge base is English-only today; `kb_lookup` returns English, and the model translates in its answer.

## Testing

No test needs a model or a network:
- a **mock provider**, a tiny loopback HTTP server inside the tests that replays scripted SSE streams for both dialects, including tool calls split across chunks, errors mid-stream and cancellation;
- the SSE parser fed one byte at a time;
- the path sandbox (`..`, symlinks, absolute paths, case folding, Windows names);
- `edit_file`: no match, several matches, overlapping edits, CRLF files, a `.vcsgfx` the loader rejects;
- the agent loop's limits and repeat detection;
- that no key ever appears in a log or in the inspector's output.

A manual evaluation set (a handful of tasks such as *fix this DASM error*, *make the sprite follow the joystick*, *explain what RESP0 does at cycle 23*) is run by hand against candidate models in phase 6 to write the "what to expect" table from measurements, not from this document. It is not run in CI: the project has no GitHub Actions.

## Phases

> **Status (2026-11-10):** phases 1 and 2 are implemented in `src/assistant/` (chat panel, first-start question, settings, detection of local servers, knowledge-base grounding, request inspector, *Apply…*, chats in `.vcsai/chats` with archive and delete). One difference from the text above: the HTTP client is VCSIDE's own small socket client (plain `http://` to a local server, about 300 lines), so no HTTP library was added; `https://` and the per-system TLS backends are phase 5. Phases 3 and 4 were added afterwards (see [The agent loop](#the-agent-loop-phases-3-and-4-2026-11-10)).

1. **Spike and plumbing.** `HttpClient` with the loopback path, SSE parser, `OpenAiProvider`, mock-server tests, the first-start dialog and *Settings → Assistant*, endpoint detection for llama-server, Ollama and LM Studio on their default ports (a connection to loopback only when the feature is on and the user presses *Detect*). Checked against a real local model.
2. **Plain chat with the knowledge base.** The panel, attached context, **automatic grounding**, the request inspector, code blocks with *Apply…* and the diff card, undo, `.vcsai/chats` with delete and archive. Already useful with any model.
3. **Read tools and the loop.** `project_info`, `list_files`, `read_file`, `search`, `symbols`, `kb_lookup`, `kb_search`, `build_errors`, `space_report`; the capability probe and the three levels; trimming.
4. **Edit and run tools.** `edit_file` with *once / this session* approvals, the privileged-file rule, `build`, `run_emulator`, per-turn limits, `.vcsai/STATE.md`.
5. **Remote.** The macOS, Windows and Linux TLS backends, `AnthropicProvider`, the secret stores, the first-use dialog and the `remote` badge, optional budget.
6. **Evaluation and the graphics and music files.** The manual evaluation set, with and without grounding; `gfx_describe` and `music_describe`; the written model guidance replacing the catalogue's guesses.
7. **Extra code completion and *Explain errors*.** A fill-in-the-middle request on a pause, ghost text in the editor, local by default; the error explainer is plain chat with the error and its grounding already attached.

The release number is not set; this looks like a 0.3.0 feature once phases 1–5 are done.

## Decisions (2026-11-10)

1. **Local models first.** No remote vendor is preferred; the two dialects stay.
2. **TLS: each system's own** (NSURLSession, WinHTTP, the system OpenSSL on Linux), no TLS library shipped.
3. **Secrets: the safest option.** OS secret stores; no plain-file fallback; environment variable or session-only where there is no keyring.
4. **Approvals like a coding agent's:** allow once, or allow for this session. Build-affecting files are never covered by a session grant.
5. **Conversations are saved per project, with Delete and Archive**, in `.vcsai/chats/`.
6. **A project state file and everything else in `.vcsai/`** (`STATE.md`, chats, ignore list).
7. **MCP: not now.** It adds nothing yet; the tool layer stays protocol-neutral so it can be added.
8. **A catalogue of local models** instead of naming one; to be replaced by measurements.
9. **Asked at first start, and optional by feature:** chat, agent, extra completion, explain errors; none by default.
10. **The knowledge base is built in** to every mode, not only the agent.
11. **Extra code completion stays in this proposal** (phase 7). **`VCSIDE_ASSISTANT=OFF` stays** as a build option.
12. **First release = phases 1–2** (chat with the knowledge base and *Apply…*); the tool loop follows in the next release.
13. **`.vcsai/STATE.md`** is in English and meant to be committed; **`.vcsai/chats/` never goes to git** (VCSIDE writes the `.gitignore` entry and keeps it).

## Open questions for the user

None blocking. The final defaults of the catalogue are fixed by phase 6 (see [How the catalogue is chosen](#how-the-catalogue-is-chosen)).

## Credits

Each idea or technique, with whoever devised or spread it. The assistant's help and About window will credit the same. "To verify" means the entry comes from general knowledge and must be checked in a primary source and recorded in `docs/sources.md` before it is relied on.

| Idea or component | Credit | Status |
|---|---|---|
| Local inference with GGUF models, an OpenAI-compatible server, grammar-constrained output | Georgi Gerganov and the llama.cpp contributors, https://github.com/ggml-org/llama.cpp (MIT) | Licence checked 2026-11-10 (the MIT text ships with the source). The helper is built from it and run as a separate program. Still to verify: tool-call template support |
| The default catalogue's models: Qwen3 4B, 8B and 14B (GGUF) | Alibaba Cloud, Qwen team, https://huggingface.co/Qwen (Apache-2.0 according to each model card, read 2026-11-10) | Downloaded by the user only; the licence is shown before the download |
| Local model runner and server | Ollama, https://github.com/ollama/ollama (MIT) | To verify |
| Local model app with an OpenAI-compatible server | LM Studio, https://lmstudio.ai (proprietary, free to use) | To verify: terms, default port |
| The chat-completions request and streaming shape, and function calling | OpenAI, https://platform.openai.com/docs/api-reference | To verify |
| The Messages API, tool use blocks and streaming events | Anthropic, https://docs.anthropic.com | To verify |
| Model Context Protocol | Anthropic (2024), https://modelcontextprotocol.io | To verify: current governance and transports |
| The reason-act-observe loop that tool-using agents follow | Shunyu Yao and co-authors, "ReAct: Synergizing Reasoning and Acting in Language Models" (2022), https://arxiv.org/abs/2210.03629 | To verify |
| Search-and-replace edit blocks as the edit format for models | Paul Gauthier and the Aider contributors, https://github.com/Aider-AI/aider (Apache-2.0). Only the idea; no code or prompts. | To verify |
| Plain-HTTP server for tests (optional) | Yuji Hirose, cpp-httplib, https://github.com/yhirose/cpp-httplib (MIT) | To verify |
| Edit format degrading gracefully on small models (reported in a 2026 roundup, to be confirmed by our own evaluation) | Paul Gauthier and the Aider contributors, as above | To verify |
| System HTTP and TLS | Apple NSURLSession, Microsoft WinHTTP, OpenSSL (the OpenSSL Project, Apache-2.0 from 3.0) | To verify: OpenSSL licence and the run-time loading approach |
| Secret stores | macOS Keychain, Windows Credential Manager, freedesktop.org Secret Service via libsecret (LGPL-2.1-or-later) | To verify: libsecret licence, loaded at run time only |
| Model families in the catalogue | Alibaba Qwen, Google Gemma, Meta Llama, Mistral AI, Microsoft Phi, IBM Granite, OpenAI gpt-oss, DeepSeek, BigCode StarCoder2 | To verify: each licence and size before it is listed |
| Server-sent events | WHATWG HTML Standard, "Server-sent events" | To verify |
| Direct use of the knowledge base as the model's ground truth | VCSIDE's own `data/kb` and its sources, see `docs/sources.md` | — |

## The bundled helper (decided 2026-11-10, implemented)

The first version asked the user to run a model server. That was too much to ask, so VCSIDE now ships its own.

- **What it is.** `llama-server` from llama.cpp (MIT, Georgi Gerganov and contributors), built from source by `cmake/llm_helper.cmake` in its own CMake run, so its options never touch the rest of the build, and copied next to the IDE (`Contents/Resources/llm` in the macOS bundle, `llm/` elsewhere). `-DVCSIDE_LLM_HELPER=OFF` leaves it out. The tag is pinned (`VCSIDE_LLAMA_TAG`).
- **Lifecycle.** `assistant/engine.*` picks a free loopback port, starts the helper with one chat slot and no web page, waits for `/health`, and stops it when the panel closes or VCSIDE exits. The panel then talks to it with the same OpenAI-style client as for any other server. Chats ask the model not to "think" first (`enable_thinking: false`) so answers start quickly; the phase 6 evaluation will revisit it.
- **GPU.** macOS: Metal, built in. Windows and Linux: one install with each backend as a library loaded at run time (`GGML_BACKEND_DL`), the best CPU variant chosen at start (`GGML_CPU_ALL_VARIANTS`) and Vulkan when the SDK was found at build time, which covers NVIDIA, AMD and Intel GPUs. CUDA and HIP are left out of the first version: they need each vendor's toolkit and Vulkan is close for models of this size. Only macOS has been built and run so far; the Windows and Linux paths are written but **not yet tested**.
- **Models.** `assistant/models.*`: the catalogue, the models folder (inside the settings folder), and the download. It uses the system `curl` (https only, resumable), then checks the SHA-256 (`assistant/sha256.*`) and deletes a file that does not match. Entries without a pinned hash, without https or with a path in the file name are ignored. The first catalogue is the Qwen3 4B, 8B and 14B files (Apache-2.0) from the publisher's Hugging Face repository; sizes and hashes were read from its file listing on 2026-11-10. The model that fits the machine's memory (needing at most 60% of it) is preselected.
- **A helper left behind.** If VCSIDE ends without stopping the helper (a crash, a forced quit), the helper would keep the model in memory. Each running helper is recorded in `llm-helpers/` in the settings folder (a file named after its process id, holding VCSIDE's); on macOS and Linux a small `sh` watchdog started with it ends it within two seconds of VCSIDE being gone, and at every start VCSIDE ends the recorded helpers whose VCSIDE no longer runs. Both check that the process really is `llama-server` first, since a process id can be reused. Windows relies on the start-up clean-up only.
- **Known limits.** There is no download resume across a changed file on the server, and no mirror.

## Measuring the knowledge base (2026-11-10)

`tools/assistant_eval` asks 32 questions of a model twice, without and with the knowledge-base entries the assistant adds, and checks each answer for its key facts (regular expressions; see its README). 18 questions name a register, instruction or format; 14 only describe what the user wants ("how do I make a tone?").

Qwen3 4B (Q4_K_M, temperature 0, no thinking), same prompt as the panel:

| | No knowledge base | Names only (before) | With topic search (now) |
|---|---|---|---|
| Questions that name something (18) | 5 | 18 | 18 |
| Questions that describe a goal (14) | 2 | 1 | 11 |
| All (32) | 7 | 17 | 29 |

What it showed:

- **Without help the 4B model is wrong about the 2600** (it explained WSYNC as "Wave Sync" for audio, and named a register that does not exist), and it cited an entry it had not been given. The prompt now says to cite only the ids it was given.
- **Matching names was not enough.** 14 of 32 questions named nothing, so nothing was added. A small keyword ranking over the registers, formats, TV standards, batari Basic words and the knowledge base's own notes (the idea of BM25, Robertson and Spärck Jones; plus a short list of the manuals' words for what people say: sprite = player, tone = audio) fixed most of them.
- **Remaining misses:** touching the playfield (collision registers not found), "my game is bigger than 4 KB" (no cartridge format found), and one positioning question. These stay in the set as the to-do list.

Read these numbers with care: one model, one run; the questions were written by the author of the knowledge base; the word list was extended after the first run, so part of the improvement is tuning to this set. A held-out set from real users is the next measurement. Some topic hits are noise (a related note that does not answer the question); the minimum score keeps most of them out but not all.

### Default provider and the recommended model (2026-11-10)

- **The built-in model is the default.** It is always the first provider and the active one for a new install and for settings saved before it existed. Other servers (Ollama, LM Studio, remote APIs) are added by the user and chosen explicitly.
- **The model is preselected from what the computer offers.** The first time the panel opens with no model chosen, VCSIDE reads the memory and CPU count at once and asks the helper for its devices (`llama-server --list-devices`, so the answer is what the helper will really use: Metal, Vulkan, CUDA...). Then `recommend()` picks:
  1. if a GPU has room, the largest catalogue model whose `memory_gb` fits in that GPU's free memory (on Apple Silicon this is the unified memory the system lets the GPU use, about 75% of the RAM);
  2. otherwise the largest that fits in 60% of the RAM and needs at most 8 GB, because larger models are too slow on a CPU;
  3. if none fits, the smallest is preselected and the panel warns that it may be slow.
  The choice is saved, shown as "recommended" in the model list and explained in the download card ("It fits in the 11.8 GB free on the GPU Apple M1 Pro"); the user can pick any other model. The thresholds are a first guess, to be replaced by measurements in the phase 6 evaluation.

## Tutor mode (idea for later)

Asked for by the user on 2026-11-10; nothing is designed or built yet. A mode of the assistant that guides someone who wants to make a game, instead of only answering questions:

- **Starts from the goal.** "I want to make a game like X" or "a shooter with a scrolling playfield": it breaks the goal into steps (screen and frame, a sprite that moves, input, collisions, sound, score, levels) and proposes the next one.
- **Adapts to the level.** A few questions at the start and what the user does along the way (the code in the project, the errors, what they ask) tell it whether to explain what a scanline is or to go straight to a kernel. The level is remembered in `.vcsai/` with the project, and the user can correct it.
- **Recommends readings.** For each step it points to the knowledge-base entries, the Docs panel pages and, where the licence allows linking, external material that fits what the user wants to achieve now, with a one-line reason. It links and cites; it does not copy other people's text.
- **Works with the project.** The example projects can be the stepping stones (each new feature of VCSIDE already goes into them), and the tutor can suggest the next small change in the user's own code, shown as a proposed edit like any other.
- **Needs first:** the how-to entries of the knowledge base (started, see below), the agent's read and edit tools (phases 3 and 4), a record of progress per project, and a list of readings with sources and licences checked.
- **To decide later:** whether it is a separate panel or a mode of the chat, how the level is kept (a few labels, not a score), and which readings are listed. Credit every technique and every reading to its author.

## How-to entries and a Spanish search (2026-11-10)

The measurement above showed that most misses were questions that describe a goal. Two changes:

- **`data/kb/techniques.json`**: 11 how-to entries (position an object horizontally, frame structure, fix a rolling picture, read the controls, detect collisions, make sound, set colours, playfield patterns, wait a precise time, bankswitching, the 48-pixel sprite). Each is a short recipe in our own words built from the register, format and TV entries already in the knowledge base and cited to the same sources; `verified: false` marks the two that include a routine that was not run (positioning, 48-pixel). They are found by topic search like the notes, with a small weight in their favour.
- **Spanish questions.** The topic search folds accents and has a short list of Spanish words for the same ideas ("posicionar", "disparo", "tono"...). The question that showed the problem was written in Spanish and found almost nothing.

Qwen3 4B, 46 questions (18 naming something, 14 describing a goal in English, 14 in Spanish):

| | No knowledge base | With name matching | With topic search | With how-to entries and Spanish |
|---|---|---|---|---|
| Spanish, goal (14) | 2 | not measured | 10 | 14 |
| English, goal (14) | 2 | 1 | 11 | 14 |
| Naming something (18) | 5 | 18 | 18 | 18 |
| All | 9 | - | 39 | 46 |

All 46 pass now, **but** these are the same questions the words and entries were tuned against, so the number is optimistic: it shows the mechanism works, not that every question will. The next measurement needs questions the author did not see. Noise remains: related entries sometimes ride along (a how-to about rolling pictures on a question about NTSC lines).

## The agent loop (phases 3 and 4, 2026-11-10)

Implemented in `src/assistant/tools.*` (the tools and the sandbox, no UI, all tested without a model) and in the panel (the loop and the approval cards).

- **How a call is made.** *Prompted tools* only, for now: the system prompt describes the tools (about 700 tokens, inside the 1,000 budget) and asks for one fenced block ```` ```tool {"tool": ..., "args": {...}} ``` ````. The parser also accepts `<tool_call>` elements (what Qwen models were trained to write) and a bare JSON object at the start of the answer, and ignores a call to a tool that does not exist. Native OpenAI `tools` calls and the capability probe of the design are **not** built: the prompted form works with every model and runtime, including the built-in helper, which does not need `--jinja` for it.
- **The loop.** An answer with a call is stored as a message of kind `call`, the tool runs (or waits for a click), its result is stored as kind `result` and sent back as the next turn, and the model answers again, until it gives an answer without a call. The first turn's attachments and reference entries stay in every request of the loop. Limits: 12 calls per user message, and the same call three times stops it.
- **Tools.** `project_info`, `list_files`, `read_file` (numbered lines, 200 per call), `search`, `symbols`, `kb_lookup`, `kb_search`, `build_errors`, `space_report` (read); `edit_file` (propose); `build`, `run_emulator` (run). *Settings → Assistant → Agent* has a profile: read only, read and propose edits, or all.
- **Sandbox.** Every path goes through `resolveInProject`: no absolute paths, no `..`, no link that leaves the folder, nothing in `.git` or `.vcsai`, and files that commonly hold secrets (`.env*`, `*.pem`, `*.key`, `id_*`...) are refused whatever the model asks. Binary files cannot be read or edited, and `build/` cannot be written.
- **Edits.** `edit_file` takes search-and-replace pairs that must each match exactly once (the error says how many times, or where the first line was found), or `content` for a new file. Nothing is written: the panel shows the changed lines with *Apply*, *Apply and allow edits to this file this session* and *Reject*. An accepted edit goes into the editor's buffer (one undo step) or, for a file that is not open, to disk. *Undo everything this conversation changed* restores every file the assistant touched and removes the ones it created. Per-hunk acceptance is not built: the unit is the file.
- **Privileged files.** An edit to a Makefile, `.vcsproj`, shell or Python script cannot be covered by a session grant, and after one the `build` tool asks again even if builds were allowed for the session.
- **Run tools.** `build` and `run_emulator` ask each time (*Allow once* / *Allow for this session*); `build` waits for the build to end and returns its errors.
- **What is not built.** `.vcsai/STATE.md`, the per-turn byte caps beyond the fixed ones, the optional token budget, MCP.

**First measurement (Qwen3, prompted tools, temperature 0, `tools/assistant_eval/agent_run.py`, five small tasks).** The loop, the sandbox and the edit path work with a real model: it reads files, proposes an edit that is applied as a click would, and refuses `.env` (the tool says why and the model relays it). Qwen3 4B passes 3 of 5 and Qwen3 8B 3 of 5, with different misses: the 4B counted `WSYNC` lines wrongly and gave up after guessing a symbol name; the 8B sometimes explains how to do a change instead of calling the tools (the prompt now tells it not to, not re-measured) and guessed a symbol name too. Five tasks and one run each say only that the mechanism works; they do not rank the models. Small models copy the examples in the tool list, so the prompt uses placeholders such as `<file>`.

## Explain errors (phase 7, first half, 2026-11-10)

An option of its own in the first-start question and in the settings. When on, the assistant window shows *Explain the errors* whenever the last build has errors, and each row of *Problems* has *Explain this error* in its right-click menu (it opens the assistant). Both send plain chat with the error attached; the knowledge-base grounding of the error text is already part of every request. Nothing new is needed from the model, so it works with any provider. Extra code completion (the other half of phase 7) is not built.

## Graphics and music files (phase 6, 2026-11-10)

- **`gfx_describe`** reads a `.vcsgfx` with the editor's own loader and answers the way the sprite editor shows it: each layer as rows of `#` and `.` with the row's TIA colour, each playfield as 40 columns, and the warnings `gfx::check` gives for the file's kernel profile. **`music_describe`** does the same for a `.vcsmusic`: target, speed, instruments, order, and the notes of one pattern as a tracker grid (`C-4 0 .` = note, instrument, effect). Both are read tools; small models need them because a table of `$3C` bytes in JSON says little.
- **Edits are validated.** An `edit_file` on a `.vcsgfx` or `.vcsmusic` is parsed with the real loader before the user sees it; a file the editor could not open is returned to the model as an error ("would not load in the editor: ..."), never offered as a diff.
- **Evaluation.** `agent_run.py` has two tasks for them (frames of a sprite, instruments of a song). The model guidance that was to replace the catalogue's guesses is **not** written: five to seven tasks and two models are not enough to rank anything, and the knowledge-base measurement above was run on one model. What the runs do show is stated where they are reported; the catalogue stays marked provisional.

First run with the two new tasks (Qwen3 4B): 4 of 7. `gfx_describe` worked (the model answered "4 frames" from it). The song task failed because the model wrote its call as `` ```tool music_describe {...}``` `` (name in front of the arguments, on one line), a shape the parser did not know; it does now, with tests, but the task was not run again.

## Remote models (phase 5, 2026-11-10; macOS only so far)

- **HTTPS through the system's TLS.** `assistant/http.h` has a hook for an HTTPS backend that a platform file registers at start; without one an `https://` address is refused with a clear message and never tried in the clear. On macOS `src/platform/apple_backends.mm` registers an `NSURLSession` backend (an ephemeral session: no cookies or cache on disk; streaming through the delegate; cancellable; the system's certificate store and proxy settings). Tried by hand against a real server: `https://example.com` answers 200, and `https://expired.badssl.com` is refused with the system's certificate error. **Windows (WinHTTP) and Linux (OpenSSL loaded at run time) are not written**; there remote models are unavailable and the settings window says so through the error message.
- **Keys.** `assistant/secrets.*`: the macOS Keychain (service "VCSIDE assistant", one item per provider name; save, read, remove tried by hand). Where there is no system store the key is kept in memory for the session. The settings window shows only the last four characters; the key is sent in a header, never in the request inspector's body, and `redactKey` exists for any text that could carry it. An environment variable name is still accepted instead of a saved key. Linux Secret Service and the Windows Credential Manager are not written.
- **Anthropic dialect.** `anthropicRequestBody` (the system prompt as its own field, `max_tokens`, `stream`), `parseAnthropicStreamData` (`content_block_delta` with `text_delta`, `message_start` and `message_delta` usage, `message_stop`, `error`; `ping` and thinking deltas ignored) and the headers `x-api-key` and `anthropic-version: 2023-06-01`. The event names, header names and version were read in Anthropic's streaming documentation on 2026-11-10; the usage fields (input tokens in `message_start`, output tokens in `message_delta`) are from memory and **still to verify** against it. **Not tried against the real service** (no key was used); the tests cover the parsing with sample events.
- **Budget.** *Settings → Assistant → Token budget for remote models*: once the tokens the server reported in this session reach it, the panel stops sending to a remote provider and says why. 0 means no limit. Local and built-in models are never counted.
- **Already there from phase 2:** the *remote* badge, the confirmation before the first send to a server, and local meaning loopback.

## Local servers and models already on the machine (2026-11-10)

- **Port 9931.** llama.cpp's release notes for build b10240 announce that `llama-server`'s default port changes from 8080 to 9931 (https://newreleases.io/project/github/ggml-org/llama.cpp/release/b10240; other pages disagree on whether the change is already in effect, so both ports are tried). *Detect local servers* tries 8080, 9931, Ollama's 11434 and LM Studio's 1234.
- **A model that is already downloaded.** The built-in provider's model list has *A model file I already have...*, which opens a file dialog for a `.gguf`; the helper then runs on that file (the provider's model is `file:<path>`). The context size is the one in the settings; the memory recommendation does not apply.

## How the chat sees the project (2026-11-10)

**What it did before.** A question went to the model with: the project's one-line description, the knowledge-base entries for the names it recognised, the selection, the build errors and, with no selection, the open file. Nothing about the rest of the project unless the agent was on and asked for it by reading files.

**How other tools do it** (read 2026-11-10, secondary sources; Cursor's own security post is the primary one for the first point):
- *Cursor* indexes the project in the background: a Merkle tree of file hashes finds what changed, the code is cut into chunks along its syntax tree (tree-sitter), each chunk becomes an embedding vector stored in a vector database with its path and line range, and a question is turned into a vector and matched against them. Source: https://cursor.com/blog/secure-codebase-indexing, with write-ups such as https://read.engineerscodex.com/p/how-cursor-indexes-codebases-fast.
- *Aider* does not use embeddings: it builds a "repo map", the files with their definitions found by tree-sitter, ranks them with a PageRank over who references whom, personalised by the names mentioned in the chat, and fits the result to a token budget. Source: Aider's documentation and `aider/repomap.py` (https://github.com/Aider-AI/aider; only the idea is used, no code).

**What VCSIDE does** (`assistant/project_index.*`). Projects here are tens of files, and the models are small, so the machinery is smaller and needs no model:
- **Change detection:** a list of files with size and time; only a changed file is read again. (A Merkle tree pays off with tens of thousands of files, not with these.)
- **Chunks:** a file is cut at labels and functions (a name in column 0), at most about 50 lines each; the name of the chunk and of the file weigh more than the body.
- **Ranking:** BM25, a keyword ranking, over words split like identifiers (`playerX`, `kernel_loop`, `WSYNC`), with each whole identifier kept too. A question of several words must match at least two of them, or a chunk's name, so one common word is not enough; a question that matches nothing adds nothing. This was the first version; embeddings were added the same day (next section) because keywords alone found the right place for only a quarter of natural-language questions.
- **What goes to the model, for every question:** up to three matching chunks (with file and lines, the open file left out when it is attached whole) and a map of the files with the names they define (names the question mentions first, about 900 characters). The panel says "Sent with your question: ... the parts of the project that match" and *View the request* shows them.
- **For the agent:** a `find_code` tool returns the best matching parts of the whole project when it does not know which file to read.
- **Limits:** 400 files, 256 KB each and 3 MB in all; hidden folders, `build/`, binaries and files that may hold secrets are never indexed. The index lives in memory and is rebuilt when a project is opened; nothing is written to disk. With a remote model, the excerpts leave the machine like any attached text, and the confirmation dialog says so.
- **Not done:** indexing symbols of C by their syntax (chunks are cut by layout, which works for assembly and C written in the usual style), following who calls whom (the Aider ranking), embeddings, keeping the index between runs, and the open editor's unsaved text (the open file is attached from the editor, the index reads the disk).

**First measurement (Qwen3 4B, `agent_run.py`, now with the excerpts and the map put in front of each question as the panel does, and a task that asks which file reads the joystick in a three-file project).** With the index the model named the right file (`src/input.asm`) without calling any tool, and answered the sprite and song questions from the excerpts alone; it picked a nearby label instead of the one the check expected, so the task counts as failed. The run passed 5 of 8 tasks against 6 of 8 on the run before the index, with the other misses in tasks the index does not touch (counting lines, guessing a symbol): one run each of a small model, so the difference is noise, not a result. What it does show is that the mechanism works end to end; a held-out set of questions about real projects is still needed to say how much it helps.

## Search by meaning: embeddings (2026-11-10)

Asked for by the user, following Cursor's model. Each chunk of the project becomes a vector, a question becomes a vector, and the closest chunks are found by cosine similarity.

- **The model runs in the helper that is already built in.** A second copy of `llama-server` is started with `--embedding --pooling last` on its own loopback port when the option is on, the model is downloaded and the assistant is open, and stopped with it. No new runtime, no new dependency: the helper serves `POST /v1/embeddings`.
- **Which model.** Two candidates were downloaded and checked against the publisher's SHA-256 (both Apache-2.0, Hugging Face): `nomic-embed-text-v1.5` Q8_0 (146 MB, English) and `Qwen3-Embedding-0.6B` Q8_0 (639 MB, multilingual). On three assembly routines and five questions in English and Spanish, Qwen3 ranked the right routine first every time, with a clear margin; nomic was wrong on three of five and near chance in Spanish. On twenty questions about a real 2,000-line project (below) Qwen3 found the place in 13 and nomic in 12 but with more noise, so **Qwen3-Embedding-0.6B is the one in the catalogue** (`embedding_models` in `data/assistant/models.json`: file, size, hash, pooling, the instruction that goes in front of a question, and the similarity threshold). It is not the lightest possible, and it is the lightest of the two that works for code and for Spanish.
- **Vectors are cached by the hash of the chunk's text** (and the model), in `<project>/.vcsai/index/<model>.vec`, which git ignores, so only chunks that changed are embedded again; the first indexing of the 136 chunks of the test project took about 9 seconds on an M1 Pro. Embedding runs on a worker thread, eight chunks per request, with a progress line in the panel; until it finishes, questions use the keywords alone.
- **Both rankings are merged** with reciprocal rank fusion (k = 60), which needs no tuning of the two scales. A chunk can enter on meaning alone if its cosine similarity reaches the model's threshold (0.5); one-line chunks are never offered. Questions that match nothing in the project add nothing.
- **Consent and privacy.** *Settings → Assistant → Search the project by meaning* is off by default. Turning it on shows the model's name and size and a *Download* button; nothing is downloaded without the click, and the hash is checked. The text of the chunks goes only to the helper on this computer.

**Measurement** (`tools/assistant_eval/index_run.py` and `vcside_index`; twenty questions, ten in English and ten in Spanish, about a 2,000-line DASM project with six source files, each with the file and line range that answers it; a question counts as found if one of the top three chunks overlaps it; the questions are not in the repository because the project is not public):

| | found in the top 3 | best chunk right | nothing returned |
|---|---|---|---|
| keywords (BM25) | 5 / 20 | 4 / 20 | 8 / 20 |
| meaning (Qwen3-Embedding-0.6B) | 12 / 20 | 8 / 20 | 1 / 20 |
| both | 13 / 20 | 9 / 20 | 1 / 20 |

The keyword-only result is poor because questions are written in words the code never uses ("¿dónde se lee el mando?" against `SWCHA`). Misses are mostly near ones: the chunk next to the right one. A threshold of 0.5 leaves a little noise on greetings (two chunks for "hello"); 0.55 lost two correct answers. One project and twenty questions written by the author are a first measurement, not a result.

**Credits.** Qwen3-Embedding: Alibaba Cloud, Qwen team (https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF, Apache-2.0). The way Cursor indexes (chunks, embeddings, a cache by content hash): see above. Reciprocal rank fusion: Gordon V. Cormack, Charles L. A. Clarke and Stefan Büttcher, "Reciprocal Rank Fusion outperforms Condorcet and individual rank learning methods", SIGIR 2009 (to verify).

## Saying what is going on (2026-11-10)

- **Before the first word** the answer area shows what the model is doing and for how long ("Reading your question, 1 file(s) and 3 part(s) of the project... 4 s", with moving dots) instead of "...". After a tool call it says it is reading the tool's result.
- **The knowledge-base entries** the answer was given are a footer after it, "From the knowledge base: LDA, STA, read-controls", with a tooltip that says what they are; not a line before the answer.
- **When something does not go whole** the "Sent with: ..." line of the question says how much and why: "ositopanda.asm: the first 181 of 1,148 lines - the model's window is 2K tokens". A long open file is cut in whole lines **around the cursor** (alternating below and above, so the cursor stays near the middle), not at its start; a selection, the errors and the files added with @ are cut from their start. A file the sandbox refuses, or one that found no room left, is named with the reason.

## Changes in the chat (2026-11-10)

The first *Apply...* put a code block at the cursor or over the selection without knowing where it belonged. It is gone. A change now says its place, is found in the real file and is shown as a diff before anything is written.

- **The format.** The system prompt asks for one block per file, named with the file's path (the paths the chips show), holding search-and-replace sections: a fence named ```` ```edit path/to/file ````, then `<<<<<<< SEARCH`, the lines as they are, `=======`, the lines as they should be, `>>>>>>> REPLACE`. The SEARCH text must be unique in the file; an empty one creates a new file. Search-and-replace blocks as the edit format for models: the idea Aider popularised (Paul Gauthier and the Aider contributors, https://github.com/Aider-AI/aider, Apache-2.0); the markers and the code here are VCSIDE's own.
- **A block without that form is an example.** It gets the copy icon and nothing else. One that was meant as a change but is cut off or names no file says why it cannot be applied.
- **Finding the place.** The block's sections go through the same `edit_file` tool the agent uses, against the file as the editor holds it (unsaved text included) or, if it is not open, as it is on disk. Exact text first; if it is not there, the same lines with other indentation or spaces at their ends (a model turns tabs into spaces), when there is exactly one such place. More than one place, or none, is not applied and says which: "occurs more than once (lines 3, 7)", "not found; its first line is at line 1136, but what follows differs". The answer is rechecked every second and a half, so a block that could not be placed starts working when the file changes. After an Apply, a block whose new text is already in the file says "Already in the file" and not "not found".
- **What the user sees.** The block shows its lines as a diff (removed in the error colour, added in the info colour of the theme), then "In main.asm, lines 1137 to 1137" and *Review and apply...* only when the place is certain. That opens the same card the agent uses: the real file's lines around the change, the diff and *Apply* or *Discard*. Nothing is written before the click, and a file that changed after the card was shown is never overwritten ("changed after the change was shown, so nothing was written"). An applied change goes into the editor's buffer as one undo step and into *Undo everything this conversation changed*.
- **Tests:** the parser (markers with other lengths, CRLF, several sections, no file, cut off), and the placement (exact, with other indentation, twice, not there, new file, outside the project, a secrets file, nothing written, the lines it touches).

## Round of fixes after a real use (2026-11-10)

A question about making a character jump when the fire button is pressed, asked with the file attached, gave a generic list of steps with an invented `PADDLE` register and the same reference entries as every other question. What changed:

- **The reference entries come from what the question asks.** The names found in a whole attached file (STA, LDA and VSYNC are in every file) were crowding out the question's own topics. Now the entries come from the question, the selection and the errors; for that question the first three are the how-to on reading the controls, INPT4 and INPT5.
- **Names the model made up are listed.** `assistant/validate.*` reads the code of the answer (fenced blocks and `inline code`) and lists the UPPERCASE names that are not a register, an instruction, a directive, a cartridge format or a TV standard of the knowledge base (batari Basic words only in a batari Basic project, where `paddle` is a word), not a name that occurs in the project's files, and not defined by the answer itself (`NAME = x`, `EQU`, `SET`, `DS`, a label). The answer is not changed: a line after it says "Not found in the reference or in your files: PADDLE, JUMP_COUNTER. The model may have made them up". The system prompt also says never to invent a register, label or constant, and to define any new one in the code.
- **What the user types is a bubble:** rounded, tinted with the accent colour and set off to the right; what was sent with it stays under it in small text.
- **Ask and Agent.** A selector under the Send button. *Ask* answers and proposes changes as blocks the user reviews; *Agent* gets the tools and is told to do the task itself, reading the files and changing them with `edit_file`, every change through the same card with the place and the diff. The choice is kept in the settings (it is the old *Agent* option, now one click away).
- **Registers and instructions in the text of an answer are links** to their entry in the Documentation panel, with their one-line summary on hover. In prose only UPPERCASE names count ("and" is not AND); in `inline code` any case. Never inside a code block or a diff.
- **The first pass.** For the first message of a chat the model is asked first, in a short call that returns JSON, for a title of at most five words in the user's language, the language, and the request in English. The title names the tab (the first words of the question stand until it arrives); the English text is added to what the knowledge base and the project index are searched with, since both are English. The message goes out when that comes back, a second or two later; if it fails, the question goes out as it was. Stop during that wait takes the question back into the box.
- **Not built: translating the whole answer back.** Working in English and translating only the prose of the answer (not code, not names) back to the user's language would need a second generation as long as the answer, a way to keep code and names out of the translation, and the history kept in English. It was not built because it roughly doubles the time to a finished answer on a local model and the measurements here (the Spanish questions of the knowledge-base evaluation) do not show that answering in Spanish is the weak point. It is a proposal to be measured with the user's own model.
