# vibecut — TODO

Living roadmap. `CLAUDE.md` is the "pick up where we left off" doc and
`DESIGN_SPECS.md` is the standing rules — this file is just the flat list of
what's actually left, so it doesn't have to be reconstructed from handover
notes every session. Check items off in place; add new ones under the
right section instead of a new file.

## Now / soon — small, concrete, unblocks something real

- [ ] **Subtitle read access.** The immediate trigger for this list: the
      panel can manage subtitles (create the track, import a transcription)
      but has no tool to *read* one back, so it can't answer "where did I
      say X?" without the user quoting the phrase themselves. Add a narrow,
      read-only tool — `subtitles_search` (text -> matching entries with
      timestamps) and/or `subtitles_list` — not shell access; see
      `DESIGN_SPECS.md` §2 for why a raw shell bridge stays out of Native
      mode regardless.
- [ ] **`generate_subtitles` clip scoping.** Currently transcribes the
      whole project unless the model explicitly passes `clip_id`, and it
      doesn't auto-resolve the current selection the way `effect_apply`
      does via `resolveTargetClip()`. On a long project that's a slow
      surprise, not a helpful default — apply the same
      selection-then-single-candidate resolution here, and have the tool
      description push the model to ask about scope up front rather than
      silently assume "everything."
- [ ] **Non-blocking audio export.** `exportZoneAudio()` renders
      synchronously on the GUI thread — a known, documented limitation
      that already caused a real "is this frozen?" moment on a ~74min
      project (see DEVLOG 2026-08-31). Worth moving off the GUI thread
      (background thread + `QFutureWatcher`, or restructure as another
      chained async stage like the Whisper setup pipeline) now that it's
      caused actual confusion, not just a theoretical gap.
- [ ] **Download the `turbo` Whisper model.** Only `tiny` is installed
      right now (from setup verification); `turbo` is the documented
      default and noticeably more accurate — `tiny` produced a garbled
      fragment on a real transcription. Quick win once picked up again.
- [ ] **Clean up vestigial speech config.** The now-unused
      `speech_system_python`/`speech_system_python_path` entries in
      `kdenliverc` (superseded by the vibecut-owned venv) and the manual
      `~/.venvs/vibecut-whisper` test venv on the host — not urgent, not
      yet decided whether to remove or leave as-is.

## Explicitly requested, not started

- [ ] **Contextual next-step suggestions.** Replace the flat "Done" state
      with a Claude/Windsurf-style suggested next action after a tool
      completes. User's own words: *"done is meaningless... it should
      prompt for a suggestion for the next thing to do."*
- [ ] **v1 plan → confirm → execute-with-checkpoints workflow.** Compound
      requests get a reviewable plan before the agent executes, per
      `DESIGN_SPECS.md` §1 and the `vibecut-v1-plan-then-execute-workflow`
      memory. Everything built so far is still single-tool-call-at-a-time.

## Planned subsystems (bigger, design-worthy)

- [ ] **VibeScript** — the `QJSEngine` sandbox for genuine code execution,
      validated output only lands on the real project. This is where a
      shell-like escape hatch actually belongs, per `DESIGN_SPECS.md` §2 —
      not bolted onto Native mode.
- [ ] **Command trust tiers** (Cascade/Windsurf-style Off/Auto/Turbo +
      per-command allow/deny). Every Native-mode tool currently just runs;
      no notion of "safe to always auto-run" vs. "needs confirmation" as a
      property of the tool. `DESIGN_SPECS.md` §2.
- [ ] **Rules/memory layer** — a per-project, user-editable, version
      -controlled instructions file (à la `.windsurfrules`), plus
      agent-generated memories, instead of one hardcoded system-prompt
      string. `DESIGN_SPECS.md` §2.
- [ ] **KWallet key storage** — replace the `ANTHROPIC_API_KEY`
      env-var-only stopgap with KWallet (KDE's keyring) + a settings page.

## Feature wishlist (user's original list, still bucketed)

Bucket A (near-free, Native-mode) is done: background noise removal.
Bucket B (external tool/model orchestration, VibeScript-callable, not deep
C++) and Bucket C (genuinely new subsystems, each its own project) are
untouched:

- [ ] Auto color grading (presets, not wheels)
- [ ] Stock footage/image generation (Pexels + a gen model)
- [ ] CLIP-based style matching against a reference video
- [ ] Ollama/WebUI interop
- [ ] Output/format optimization
- [ ] YouTube upload
- [ ] TUI mode (a whole second frontend) — Bucket C, own project
- [ ] Fusion-style node compositor — Bucket C, own project
- [ ] CapCut-style meme templates — Bucket C, own project

## Open question that should drive priority

**"What takes the longest in a real editing session?"** — the user's own
question, still unanswered. Should decide which Bucket B/C item gets
built next rather than picking by guesswork.
