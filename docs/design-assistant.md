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
| Inference engine | **Do not bundle, do not link.** Talk to a separate process over HTTP. | llama.cpp and Ollama are MIT, so bundling would be legal. But: builds differ per GPU backend (Metal, CUDA, Vulkan), the Windows build here is a static MinGW cross-build, the binary is large, and models and engines change monthly. A separate process also isolates crashes and out-of-memory, and uses the same code path for local and remote. It follows the rule already in `design.md`: tools are found, not redistributed. |
| Models | **Never bundled, never downloaded without being asked.** | 2–20 GB each, with licences that differ per model (some permissive, some with use restrictions). The user picks and installs them. |
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
- **Context chips** above the input: *this file*, *selection*, *last build errors*, *this register or opcode*; each can be removed before sending. In plain-chat mode these are how context reaches the model.
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
3. **Plain chat:** no agent at all. The context chips attach text, and any code block in the answer has *Apply…*, which opens the same diff card. Always available, and already useful.

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

> **Status (2026-11-10):** phases 1 and 2 are implemented in `src/assistant/` (chat panel, first-start question, settings, detection of local servers, knowledge-base grounding, request inspector, *Apply…*, chats in `.vcsai/chats` with archive and delete). One difference from the text above: the HTTP client is VCSIDE's own small socket client (plain `http://` to a local server, about 300 lines), so no HTTP library was added; `https://` and the per-system TLS backends are phase 5. No tools yet (phase 3).

1. **Spike and plumbing.** `HttpClient` with the loopback path, SSE parser, `OpenAiProvider`, mock-server tests, the first-start dialog and *Settings → Assistant*, endpoint detection for llama-server, Ollama and LM Studio on their default ports (a connection to loopback only when the feature is on and the user presses *Detect*). Checked against a real local model.
2. **Plain chat with the knowledge base.** The panel, context chips, **automatic grounding**, the request inspector, code blocks with *Apply…* and the diff card, undo, `.vcsai/chats` with delete and archive. Already useful with any model.
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
| Local inference with GGUF models, an OpenAI-compatible server, grammar-constrained output | Georgi Gerganov and the llama.cpp contributors, https://github.com/ggml-org/llama.cpp (MIT) | To verify: licence, `llama-server` options, tool-call template support |
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
