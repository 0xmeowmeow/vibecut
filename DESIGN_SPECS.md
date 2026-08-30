# vibecut — design specs

Standing rules for how vibecut's agent is built, distilled from what
actually broke during the first working sessions. `CLAUDE.md` is the
operational handover doc; `DEVLOG.md` is the narrative history. This file
is the reference: read it before adding a tool, changing the agent loop,
or touching the panel UI, and update it when a new class of bug teaches
something durable.

## 1. North star: plan → confirm → execute-with-checkpoints

The target v1 session looks like this:

1. Welcome message, a few clickable suggestions.
2. The user describes a real job in one rich, compound message — e.g.
   "turn this noisy two-speaker interview into a YouTube video: add
   subtitles, cut the section where I said I wanted it cut (I only
   said that out loud during the recording), flag every place he names
   one of his own projects so I can drop in a clip, flag where stock
   footage would help, do noise and color correction, and tell me what
   export settings to use."
3. The agent does **not** start executing immediately. It thinks and
   produces a plan — a markdown task list, the same shape as Claude
   Code's own plan mode.
4. The user reviews and edits the plan before anything runs.
5. The agent executes it, narrating each step as it happens, adding
   tools/capabilities along the way if the task calls for them, and
   stopping at genuinely ambiguous points instead of guessing —
   confirming a Whisper mis-transcription, asking about a visual style
   for text/zooms, getting the user to actually watch the result.
6. Optional hand-off to publishing (YouTube upload, etc.) as a final
   step, if configured.

Every architecture decision below should be judged against whether it
moves toward this, not just "does one tool call work." This is a
multi-session build, not a single feature — but it's the destination.

## 2. Tool-surface philosophy: broad, not gatekept

Native mode was originally planned as a small allowlist added one
reviewed tool at a time, mirroring vibecad. That was too timid in
practice — first case was speech-to-text setup, where the instinct was
to send the user to a Settings dialog instead of driving Kdenlive's own
installer from chat. Corrected direction: **the agent should operate
Kdenlive the way an agentic IDE operates a codebase** — real capability
over the app's own internals, not a narrow menu requiring a GUI detour
for every capability.

Checked this against Cascade (Windsurf, now folded into Devin Desktop
under Cognition): its own docs state the same principle plainly —
*"Cascade operates across the entire codebase by default"* rather than a
curated tool menu. That validates the direction. Two places Cascade goes
further that we don't have yet, worth building toward:

- **Command trust tiers.** Cascade has Off/Auto/Turbo plus explicit
  allow/deny lists per command — some things always auto-run, some
  always need confirmation. Vibecut currently has no equivalent; every
  tool just runs. Worth a real design pass: which tools are safe to
  always auto-run vs. which need confirmation, as a property of the
  tool, not a blanket policy.
- **A rules/memory layer.** `.windsurfrules` — user-authored,
  version-controlled, project-scoped instructions, plus
  agent-auto-generated memories. Vibecut's system prompt is one
  hardcoded C++ string today. A per-project, editable rules file (e.g.
  "always prefer DeepFilterNet over RNNoise," "never touch track 3") is
  a real gap once the tool surface grows.

What stays out of scope for Native mode regardless: a raw "run any shell
command" bridge. That escape hatch belongs to VibeScript, not here.

## 3. Verification discipline — this is the one that bit us twice

**A tool must never report `ok:true` without confirming the underlying
Kdenlive state actually changed.** Not "the function was called" —
"the state changed and we checked." `effect_apply`'s first version
called `TimelineController::addEffectToClip()` (returns `void`) and
unconditionally reported success; the fix calls the model method that
reports which clips it actually touched and cross-checks
`EffectStackModel::hasFilter()` afterward.

**An agent turn must never be presented as "done" just because it
produced no error.** Two distinct failure shapes were found live, not
in testing:

- *Truncation disguised as success*: a turn stopping for any reason
  other than `end_turn` (hitting `max_tokens` mid-thought, a stop
  sequence) is not "finished," it's cut off — check `stop_reason`
  explicitly.
- *Genuine silence*: even with adaptive thinking on, Claude can end a
  turn with **zero content blocks at all** — no text, no tool call,
  nothing. This was caught directly in production logs
  (`blocks: []`). The fix: don't record that as history, retry once
  automatically, and if it's still empty, surface a real error — never
  a "Done."

Corollary: **a text-editing agent verifies by treating the artifact as
text** (lint it, run tests). Vibecut can't — a video project has no
"run the file" equivalent — so verification has to mean **introspecting
the live in-memory object graph** (`hasFilter()`, effect stack row
counts, `stop_reason`). This is a structural difference from how
Windsurf/Cascade verify (their own docs admit auto-lint "won't catch
logic regressions" and recommend driving a live browser via MCP for
real verification) — vibecut gets that same rigor for free by checking
the live document directly, no extra bridge needed. Lean into that.

## 4. Async tool execution

Anything not near-instant (model downloads, transcription, a render)
must return `{"ok": true, "started": true, ...}` immediately and report
completion out-of-band — a signal into the panel
(`VibeCutTools::backgroundProgress`), not tied to the chat turn that
started it, since the work can finish long after that turn ended. Never
block the GUI thread synchronously for one of these. (`speech_setup` and
`generate_subtitles` are the reference implementations.)

## 5. Chat UI conventions

- Welcome message + clickable suggestions **inline in the transcript**,
  not separate buttons (the standard vibe-coding chat shape — Claude
  Code, Cascade).
- Tool activity narrated in plain language as it happens
  ("Adding \"AI Noise Removal (DeepFilterNet)\"…"), not raw JSON.
- Explicit busy/ready state: a progress indicator while a request is in
  flight, input re-enabled and focused when it's genuinely ready for
  the next one.
- A failed tool call shows immediately (red line, plain error text) —
  never wait for end-of-turn to surface a failure that already happened.
- A closing "done" marker is only ever shown when there's real evidence
  something happened (a tool ran and its result was narrated above it);
  otherwise show a real error, not a green checkmark.
- Read-only tool results (a selection, a status check, a clip list) show
  a plain "→ ..." summary line unconditionally, the moment the code
  knows the answer — never dependent on the model choosing to narrate
  it. Same principle, one level further: when the code discovers a
  routine, fixable blocker (a missing dependency, nothing selected),
  it should proactively offer the fix as a clickable one-click action
  in the transcript that runs by calling the tool layer directly — not
  wait for the model to decide to mention it, and never require a trip
  to a Settings dialog to do something routine. A settings-dialog link
  can exist as a secondary option; it must never be the only path.

## 6. Build/ops rules

- The `flatpak-builder` invocation (flags, module order in
  `packaging/flatpak/org.kde.kdenlive-dependencies.json`) is
  load-bearing for build-cache validity — changing flags between runs,
  or inserting a new module mid-list, invalidates the cache for
  everything after it and forces a full dependency rebuild. Append new
  runtime-only modules (like a bundled LADSPA plugin) at the **end** of
  the dependency list so nothing else invalidates; keep the
  `flatpak-builder` command itself byte-for-byte stable across sessions
  (see `CLAUDE.md`).
- Prefer a headless repro over a GUI rebuild-and-click cycle when
  debugging: a small script replaying the exact system prompt/tools
  against the live API, or `melt`/`analyseplugin` for an MLT effect,
  finds real bugs faster than guessing from a rebuild. Both real bugs
  this project has hit so far were found this way, not by staring at
  the code.
- Long builds/installs: use backgrounded commands with a completion
  notification, not chained `sleep`/`until` foreground waits (they hit
  the harness's foreground timeout and get orphaned).
