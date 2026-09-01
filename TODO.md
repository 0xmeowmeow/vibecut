# vibecut — TODO

Living roadmap. `CLAUDE.md` is the "pick up where we left off" doc and
`DESIGN_SPECS.md` is the standing rules — this file is just the flat list of
what's actually left, so it doesn't have to be reconstructed from handover
notes every session. Check items off in place; add new ones under the
right section instead of a new file.

## Now / soon — small, concrete, unblocks something real

- [ ] **`timeline_list_clips` doesn't expose audio/video type.** Found live
      2026-09-01: with no grounded way to know which clip is audio vs.
      video, the model guessed in its own prose and got it backwards,
      tried to denoise the video-only clip, and only found out from
      `effect_apply`'s real rejection. It self-corrected fine that time
      (the rejection is proof the verification discipline works), but the
      model shouldn't have to trial-and-error this — add clip state
      (`PlaylistState::ClipState` - VideoOnly/AudioOnly/Disabled) to the
      listing.
- [ ] **`effect_apply` can add an effect but can't configure it.** Found
      live 2026-09-01: `avfilter.colorlevels` and `avfilter.colorcorrect`
      both landed for real (confirmed against the repository parsing log)
      but with every parameter at its identity default - a real effect
      that's a functional no-op until something can drive its parameters.
      Needs a `parameters` input on `effect_apply` (or a separate
      `effect_set_parameter` tool) wired to the same `EffectStackModel`
      the apply path already uses.
- [ ] **Subtitle read access.** The immediate trigger for this list: the
      panel can manage subtitles (create the track, import a transcription)
      but has no tool to *read* one back, so it can't answer "where did I
      say X?" without the user quoting the phrase themselves. Add a narrow,
      read-only tool — `subtitles_search` (text -> matching entries with
      timestamps) and/or `subtitles_list` — not shell access; see
      `DESIGN_SPECS.md` §2 for why a raw shell bridge stays out of Native
      mode regardless. Worth modelling on
      [browser-use/video-use](https://github.com/browser-use/video-use)'s
      approach rather than plain line search: word-level timestamps +
      speaker id as the structured primary layer, on-demand visual
      composites (filmstrip/waveform) only at actual decision points -
      keeps context small deliberately, same principle already in
      `CLAUDE.md`'s architecture section.
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

## Multi-model backend — started 2026-09-01

`VibeCutAgent` now supports Ollama (`VIBECUT_BACKEND=ollama`) as a second
backend alongside Anthropic, verified end-to-end against `qwen3.8:27b`:
real tool-calling, a real compound multi-step request, and a real
self-corrected mistake (see the 2026-09-01 DEVLOG entry). Env-var
configured only (`VIBECUT_BACKEND`, `VIBECUT_MODEL`, `VIBECUT_OLLAMA_HOST`,
`VIBECUT_OLLAMA_NUM_CTX`, `VIBECUT_OLLAMA_TEMPERATURE`) - this was a
feasibility prototype, not the finished feature.

- [ ] **Settings-panel picker**, replacing the env-var stopgap - which
      provider/model/endpoint, folded into the same config-panel item
      below rather than a separate dialog.
- [ ] **OpenAI-compatible backend** (covers ChatGPT and Kimi/Moonshot in
      one implementation, not two) - Ollama's `/api/chat` is close enough
      to the OpenAI Chat Completions dialect that a lot of the translation
      code in `ollamaMessagesFromHistory()`/`ollamaToolsFromSchemas()`
      should generalise directly; the real new work is a second HTTP
      endpoint/auth path, not new wire-format logic.
- [ ] **Empty-turn-after-tool-call is mitigated, not solved.** Lower
      temperature (0.3) and a per-tool-call retry-budget reset
      (`vibecutagent.cpp`) stopped it from hard-failing a compound
      exchange, but it still happens on most turns with `qwen3.8:27b` -
      each retry costs a full generation (~50-75s on this GPU, partial
      CPU offload since the model doesn't fully fit in 16GB). Worth a
      real root-cause pass later (chat template quirk in this specific
      Ollama model build? a `qwen3.8-obliterated` abliteration side
      effect? genuinely just this model's behaviour at low temperature
      too?) rather than leaning on the retry forever.

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
      Same settings page should also own backend/model selection (see
      "Multi-model backend" above) - one config panel, not two.
- [ ] **A scoped, vibecut-owned folder + config panel + asset
      fetch-and-store**, from `going-forward.md` (2026-09-01): a directory
      the agent has full read/write access to, for LUTs, fonts, style
      references, and fetched stock footage/images. This is the concrete,
      safe answer to "can we give it shell access" - not a raw shell
      bridge (stays out of Native mode per §2 above regardless), a scoped
      directory. Also the prerequisite for the asset/style-library and
      external-fetch items in the wishlist below.
- [ ] **Session-spanning memory**, from `going-forward.md`: a tracked
      `.md` file per project (start-of-session context gathering, compact
      at milestones, prime the next instance without re-chewing tokens) -
      the same mechanism this very project's own `CLAUDE.md`/`DEVLOG.md`/
      `TODO.md` split already uses for *this* codebase, applied instead to
      the user's edited *projects*. Independently validated by
      [browser-use/video-use](https://github.com/browser-use/video-use),
      which keeps a `project.md` for exactly this.

## Project ecosystem (lower priority, from `going-forward.md`)

- [ ] **A GitHub contribution on-ramp** — `CONTRIBUTING.md`, some
      "good first issue"-shaped labeled work, so this can grow past a
      single-user project once there's something worth showing off.
- [ ] **An external API/interop surface** for other programs to drive
      vibecut. Real security shape to decide up front, not default into:
      local-only (a Unix socket / localhost port only this machine can
      reach) vs. network-exposed - pick local-only unless there's a
      concrete reason not to.

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
built next rather than picking by guesswork. One external data point:
[browser-use/video-use](https://github.com/browser-use/video-use) is
built specifically around removing filler words and dead space between
takes - not proof it's *our* answer, but a real signal worth weighing.
