# vibecut — TODO

Living roadmap. `CLAUDE.md` is the "pick up where we left off" doc and
`DESIGN_SPECS.md` is the standing rules — this file is just the flat list of
what's actually left, so it doesn't have to be reconstructed from handover
notes every session. Check items off in place; add new ones under the
right section instead of a new file.

## Now / soon — small, concrete, unblocks something real

- [x] **~~`timeline_list_clips` doesn't expose audio/video type~~ — done
      2026-09-02.** Added `type` (video_only/audio_only/av/disabled) to
      every clip in the listing, straight from `PlaylistState::ClipState`.
      Verified live: correctly counted "3 pieces of footage" from 6
      AV-split clip ids without conflating them.
- [x] **~~`effect_apply` can add an effect but can't configure it~~ — done
      2026-09-02, and generalized further than originally scoped.** Added
      a `parameters` input, validated against the effect's real XML
      parameter names (`EffectsRepository::getXml()`) rather than a
      guessed display name, with values auto-formatted to the
      `"start=value"` keyframe form the ~10 keyframable parameter types
      need (mirrors `AssetParameterModel::isAnimated()`'s real type list -
      see `KDENLIVE_INTERNALS.md`). Verified live across `animated`,
      `list`, `color`, and `bool` typed parameters on different effects
      (colortemperature, avgblur, chromahold) - real values landing, real
      rejections for made-up parameter names.
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
      `CLAUDE.md`'s architecture section. Note found live 2026-09-02:
      Kdenlive already has a native "Subtitles" dock (`pCore->
      subtitleWidget()`, `mainwindow.cpp`) for viewing/editing subtitle
      text by hand - hidden by default like our own panel, toggle it on
      via the View menu's dock list. That's a real, separate GUI path;
      it doesn't reduce the need for a chat-callable read tool.
- [x] **~~`generate_subtitles` clip scoping~~ — done 2026-09-02.** No
      explicit `clip_id` now falls back to the current selection before
      the whole-project default, and the tool description pushes the
      model to ask about scope when nothing's selected on a
      multi-clip timeline rather than silently transcribing everything.
- [ ] **Non-blocking audio export.** `exportZoneAudio()` renders
      synchronously on the GUI thread — a known, documented limitation
      that already caused a real "is this frozen?" moment on a ~74min
      project (see DEVLOG 2026-08-31). Worth moving off the GUI thread
      (background thread + `QFutureWatcher`, or restructure as another
      chained async stage like the Whisper setup pipeline) now that it's
      caused actual confusion, not just a theoretical gap.
- [x] **~~Download the `turbo` Whisper model~~ — done 2026-09-02.**
      `large-v3-turbo.pt` downloaded (~1.6GB) via the same
      `whisperquery.py task=download` path the app itself uses. Verified
      live: subsequent `generate_subtitles` calls now use it
      ("using the 'turbo' model on CUDA").
- [x] **~~Clean up vestigial speech config~~ — checked 2026-09-02, not actually
      vestigial, leave as-is.** `speech_system_python_path` in `kdenliverc`
      and the `~/.venvs/vibecut-whisper` venv it points to aren't dead:
      `src/pythoninterfaces/speechtotext.cpp` reads them for Kdenlive's own
      *native* Speech-to-Text menu feature (Settings → Configure Kdenlive →
      Speech to Text / the text-based editor), a completely separate code
      path from vibecut's chat-panel tools. The venv is real and functional
      (torch 2.13+cu130, whisper importable) - removing either would break
      that unrelated, still-working feature for no reason.
- [x] **~~Clip-move/gap-removal tool~~ — done 2026-09-02, today's "bigger
      item."** New `timeline_close_gaps` tool. Surveyed the real subsystem
      first (`TimelineFunctions`, a `struct` mostly-public-by-default,
      documented in `KDENLIVE_INTERNALS.md`) rather than patching around
      the absence of one - found `TimelineController::removeSpace()` is
      another void-returning UI wrapper (same trap as `addEffectToClip()`)
      around the real `bool`-returning `TimelineFunctions::
      requestDeleteBlankAt()`. Gaps are self-detected (no reachable blank-
      detection API - `TrackModel`'s is `protected`) by comparing sorted
      clip positions per track, closed one at a time with fresh
      re-detection after each (positions shift), `affectAllTracks=true`
      first to keep AV-split pairs in sync with a per-track fallback if
      that's too strict for the project's other tracks. Verified live:
      "Closed 2 gaps on track 2 — one 57 frames at position 1172, another
      90 frames at position 3928. No gaps remain," confirmed by the user
      looking at the actual timeline, not just trusting the report.

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

## UI layouts — started 2026-09-02, layout_switch pulled after real breakage

Added `layout_list`/`layout_switch` tools, then had to pull
`layout_switch` from the active tool surface the same session (see
`KDENLIVE_INTERNALS.md` for the full technical trail). `layout_list`
(read-only) stays. Every built-in layout (`editing`, `effects`, `color`,
`audio`, `logging`) was authored before the `"vibecut"` dock existed, so
switching to one leaves our dock's fate up to KDDockWidgets' undefined
fallback-placement behaviour: confirmed live it doesn't cleanly float or
relocate the panel, it lands overlapping another dock's *actual* saved
slot (e.g. the Waveform scope's on the Color layout, found by checking
`color.json`'s real `itemIndex` list) with a corrupted, full-window-sized
container - and the corruption turned out to be sticky, surviving further
switches, not just the first one. Two mitigations tried and both failed
to fully fix it: floating the panel first (`MainWindow::vibeCutDock()` +
`setFloating()`/`isFloating()`, still there for whoever picks up the real
fix), then a one-click **"VibeCut" toolbar button**
(`MainWindow::slotShowVibeCut()`, `extraToolBar` in `kdenliveui.rc`) to
switch back to `editing` and re-open/raise the dock - user confirmed live
it lands in the same broken state regardless of which layout you're
recovering from. Real, live consequence: at one point the user couldn't
type in the panel because the input box was hidden off-screen - a genuine
usability lockout, not a cosmetic bug. User's call: pull the switch tool
entirely rather than leave a way to get stuck with no reliable escape.
The toolbar button is left in place (harmless, may still help in a
less-corrupted case) but isn't a real fix either.

- [ ] **Real fix for the KDDockWidgets fallback-placement bug.** Chasing
      it further needs actual hands-on UI debugging (dragging edges,
      reading KDDockWidgets' own separator/multisplitter source), not a
      quick patch - see `KDENLIVE_INTERNALS.md`'s UI layouts section for
      what's already been ruled out. Confirmed live 2026-09-02, checking
      all 5 tabs by hand: the panel is present but broken/overlapping in
      *every single one* (Logging, Editing, Audio, Effects, Color), not
      just when leaving Editing - so this isn't "vibecut only survives on
      its home layout," it's "vibecut is broken almost everywhere,
      Editing (its actual construction-time dock slot) included once
      you've switched away and back." The "VibeCut" recovery button and
      the `layout_switch` removal are damage control, not a fix. User's
      call: *"There is no easy solution: we are going to have to just
      work it out and fix it, if not tonight, then tomorrow."*
- [ ] **Position the vibecut dock sensibly in every built-in layout**, if
      still worth doing once the item below exists - a DaVinci-Resolve-
      page-style "this panel always sits here on the Color page" needs one
      layout JSON at a time (6 files: the 5 pages + `editing_vertical`),
      not a code fix. May be superseded by dynamic layout generation
      instead of hand-positioning each static one.
- [ ] **Give the agent real control over layout composition, not just
      switching between presets.** The actual point, per the user
      (2026-09-02): *"let's give full control of the layouts to the
      agent... it can just draw the whole layout contextually, which is
      the whole point of something like vibecad."* Concrete example given:
      user unsure about gain - agent maximizes the video preview, applies
      the effect, brings up just the gain control, tells the user what to
      tweak and what to listen/look for. Only when it actually matters for
      the task, not as a gimmick. Real design work: dynamically generating
      layout JSON (or driving KDDockWidgets' layout API directly) scoped
      to what's relevant to the current task, not picking from
      `layout_list`'s fixed 5. The static layout_list/layout_switch tools
      above are a real, useful stepping stone (same underlying save/
      restore mechanism) but this is the actual destination.
      **Proposed shape, per the user (2026-09-02):**
      1. First, actually work out how to construct a *correct* layout
         file by hand - understand the real KDDockWidgets JSON format well
         enough (item tree, `itemIndex`, `allDockWidgets`) to author one
         that properly includes the vibecut dock, not reverse-engineer it
         under pressure mid-bug like tonight.
      2. Replace all of Kdenlive's default layouts with vibecut's own
         versions once that's solid - every one authored with vibecut
         actually accounted for, not retrofitted.
      3. Give the agent a curated library of multiple known-good layouts
         to pick from (broader than just the 5 originals), not only the
         ability to generate new ones.
      4. Let the agent *also* try generating novel layouts contextually
         (the actual destination above) - but validate before committing:
         if a generated layout doesn't render correctly, fall back to a
         known-good one from the library rather than leaving the user
         stuck. This is the safety net tonight's mitigations didn't have -
         a *validated* escape hatch, not another unreliable recovery
         attempt.

## Explicitly requested, not started

- [x] **~~Contextual next-step suggestions~~ — done 2026-09-02, at the
      prompt level.** System prompt now tells the model to always end a
      finished task with one specific, contextual next step instead of a
      bare "Done." Not yet the fancier clickable-inline-link version from
      the `chat-ui-suggestions-as-inline-links` memory - that's still a
      real UI feature worth doing, this is the words-only first pass.
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
