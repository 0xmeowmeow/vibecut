# vibecut devlog

A fork of [Kdenlive](https://kdenlive.org) that you can talk to — an AI
chat/terminal panel that can script and extend the editor live, not just
call pre-built features. Taking on [@10_X_eng](https://x.com/10_X_eng)'s
challenge — the video-editing sibling to his
[vibecad](https://github.com/10-X-eng/vibecad) (FreeCAD).

Upstream: https://github.com/kde/kdenlive
Fork: https://github.com/0xmeowmeow/vibecut

## 2026-08-29 — kickoff

Cloned upstream `kde/kdenlive` (master), set up the fork with `upstream`/
`origin` remotes so we can rebase cleanly as Kdenlive moves (it's active —
daily `GIT_SILENT` sync commits from their translation/docs pipeline).

**The core idea**, modeled directly on vibecad's two-mode split:

- **Native mode** — the LLM calls a curated set of existing Kdenlive/MLT
  operations (apply an effect, insert a clip, trim). Validated against the
  live project before it runs, no code execution. This is where "remove
  the background noise from this clip" lives — and it turns out Kdenlive
  already ships an RNNoise effect (`data/effects/ladspa/ladspa_librnnoise.xml`),
  so the first proof of concept barely needs new video engineering at all,
  just a way to route natural language into it.
- **VibeScript mode** — the LLM actually writes and runs code, in an
  isolated worker, and only validated output lands on the real project.
  This is the "recode it" part — genuine live extension of the editor,
  not just calling fixed functions. Planned as an embedded `QJSEngine`
  (Qt's own JS engine) rather than Python bindings, since Kdenlive is
  already Qt/KDE and this avoids a whole binding-generation project.
  Saved scripts double as the addon/workflow system — a "workflow" is
  just a named, shareable script.

Kdenlive doesn't have a real third-party plugin API today (the
`plugins/sampleplugin` in the tree is a Qt Designer widget plugin, not a
feature-extension point) — so the addon story has to be built, and
VibeScript is designed to be that mechanism rather than a separate system.

**Build**: needs Qt 6.10+, KF6 6.21+, MLT 7.38+ — all newer than what
Debian trixie ships (Qt 6.8.2, MLT 7.30). Rather than hand-building that
whole stack from source, using Kdenlive's own Flatpak manifest
(`.flatpak-manifest.json`, `org.kde.Sdk//6.11`) — this is the same path
their own CI uses, and sidesteps days of bootstrapping Qt/KF6 by hand.

**Not yet decided**: what the chat panel's actual API surface looks like
beyond the noise-removal proof of concept, and how much of the wishlist
(auto color grading, stock footage/generation, CLIP style-matching,
YouTube upload, Fusion-style node compositor, TUI mode) becomes
VibeScript-callable functions vs. dedicated Native-mode commands.

## 2026-08-29 — fork fixed, build in progress

First fork attempt was wrong: created an empty GitHub repo and tried to
push the full ~4.7GB local history into it directly — GitHub hangs up
past ~2GB in a single push. Fixed by using `gh repo fork kde/kdenlive
--fork-name vibecut` instead, which forks server-side with zero data
transfer, then pushed just the new `vibecut` branch (1.9KB) on top.

Build via `flatpak-builder` against `org.kde.Sdk//6.11` hit two real
snags: the state dir needs to be on the same filesystem as the build
output dir, and the manifest's `org.freedesktop.Sdk.Extension.llvm21`
requirement doesn't currently resolve (KDE's SDK 6.11 wants it at branch
`6.11`, flathub only publishes `25.08`) — a live upstream version-skew
bug, not us. Since `clang`/`llvm` aren't actually invoked by the build,
dropped that extension requirement entirely and the build proceeded past
config into the real dependency compile (gavl, x264, MLT, Intel media
SDK, glaxnimate, ...).

Handed the build off to a fully-detached process (`setsid nohup`,
reparented to PID 1) before closing out this session, specifically so a
multi-hour compile survives a Claude Code restart. See `CLAUDE.md` for
the full operational handover notes.

**Build finished clean.** Installed locally (`flatpak-builder --install`)
and confirmed it actually runs: `flatpak run org.kde.kdenlive//master`
reports `kdenlive 26.11.70`. Shows up in app launchers (wofi etc.) as
"Kdenlive (Nightly)" — the manifest's `desktop-file-name-suffix` keeps it
distinct from any stock Kdenlive install. First milestone done: we have
a working build of our own fork to actually develop against.

Next: the noise-removal proof of concept (chat dock panel + smallest
possible Native-mode command surface calling the existing RNNoise
effect).

## 2026-08-30 — the assistant panel, wired end to end

Read through vibecad's actual AI subsystem (`src/Mod/VibeCAD/`, ~776k
lines) to copy its decisions rather than reinvent them. What carried
over directly: the tiny byte-capped system prompt sent as a cached
system block; the per-tool JSON-Schema spec + `{"ok": bool, ...}` result
contract (their `tool_impl/service/*` modules); an **allowlist** as the
guard rail for what the model can actually touch (their
`NativeActionManifest`); provider work kept off the UI thread with tool
execution marshalled back to it; and `claude-sonnet-5` as the default
model.

The one decision that didn't port: vibecad runs the `anthropic` **Python**
SDK in a child process because FreeCAD is already Python. Kdenlive is
C++/Qt with no Python layer, so the equivalent here is a pure-Qt client —
`QNetworkAccessManager` streams `POST /v1/messages` (SSE), a small
incremental parser rebuilds the assistant message from the event stream,
and on `stop_reason == "tool_use"` the loop runs the requested tools and
feeds results back. It's event-driven on the GUI thread, so no worker
thread is needed. Chose this over a bundled Python sidecar to keep
vibecut single-language and add nothing to the (rebased-often, fragile)
Flatpak manifest.

Landed in `src/vibecut/`:

- `sseparser.h` — header-only, dependency-free SSE stream parser
  (buffers partial records across arbitrary network chunk boundaries).
- `vibecutagent.{h,cpp}` — the Anthropic client + tool-use loop.
- `vibecuttools.{h,cpp}` — the Native-mode tool surface: `timeline_list_clips`,
  `timeline_get_selection`, `effect_apply` (allowlisted; `denoise` →
  `ladspa.9354877`, the RNNoise effect), `ask_user`.
- `vibecutdock.{h,cpp}` — the panel: transcript + prompt line, registered
  on the main window as a KDDockWidgets panel next to Library / Markers /
  Speech Editor.
- `tests/vibecuttest.cpp` — Catch2 tests for the SSE parser, the effect
  allowlist, and the tool-schema shape.

API key for this first cut comes from `ANTHROPIC_API_KEY` in the
environment; KWallet + a settings page come later.

Build snags: one real bug (`qAsConst` needs `<QtGlobal>`; dropped it and
iterated the array directly) and one self-inflicted — adding `--ccache`
to the flatpak-builder invocation changed the module cache key and forced
MLT / OTIO / kddockwidgets to rebuild once. Both fixed; the fork now
builds, installs, and runs (`kdenlive 26.11.70`), and the SSE parser's
tricky cases (split mid-token, multiple events per chunk, CRLF,
multi-line `data:`) are verified.

Not yet done: driving the whole loop against the live API with a real
key and a real project — "remove background noise from the selected
clip" landing the RNNoise effect on the clip's stack.

## 2026-08-30 — DeepFilterNet, because RNNoise wasn't enough

First live run worked — the tool loop fired and dropped the effect on the
clip — but on a real interview recorded in a busy cafe you couldn't hear a
difference. That's RNNoise doing what RNNoise does: it's trained on
*stationary* noise (hiss, hum, HVAC), and cafe babble — other people
talking, cutlery, music — is the exact case it can't touch, because it
can't tell the target voice from background voices.

So bundled **DeepFilterNet** (DFN3, `Rikorose/DeepFilterNet` v0.5.6) — a
deep-learning speech denoiser that handles non-stationary noise. It ships
a **LADSPA plugin**, which means it drops straight into the same slot
Kdenlive already uses for the RNNoise effect — no subprocess, no file
juggling, non-destructive, real-time preview:

- `packaging/flatpak/org.kde.kdenlive-dependencies.json` — new
  `deepfilternet-ladspa` module fetches the prebuilt
  `libdeep_filter_ladspa.so` (glibc build, standard deps only) into
  `/app/lib/ladspa`. Placed last in the dep list so it doesn't invalidate
  the cache of anything else.
- `data/effects/ladspa/ladspa_deepfilternet.xml` — Kdenlive effect
  wrapper, "AI Noise Removal (DeepFilterNet)", LADSPA id `ladspa.7843795`
  (mono), exposing Attenuation Limit + the processing thresholds.
- `vibecuttools.cpp` — `denoise` in the allowlist now maps to DFN;
  the old RNNoise filter stays as `denoise_light`. The `effect_apply`
  tool description tells the model to reach for `denoise` on real-world
  location noise.

Verified end to end with `melt` inside the Flatpak: `analyseplugin` sees
the plugin (id, ports, defaults all as encoded in the XML), and running
`-filter ladspa.7843795` over a test signal processes the audio — on a
non-speech tone+noise mix at max attenuation it drops the output ~70 dB,
i.e. it's aggressively removing everything it doesn't classify as voice.
On a real voice recording it keeps the voice and takes out the room.

Same next step as before, now with a denoiser that will actually show a
difference: run the whole NL loop live against the cafe interview.

## 2026-08-30 — the panel becomes an actual chat surface

First live-use feedback, two things: a bolted-on "Remove background
noise" button wasn't the right shape — the expected pattern for this
kind of tool (Claude Code, Windsurf, "vibe coding" chat panels
generally) is a welcome message with clickable suggestions *inside* the
transcript, not separate buttons. And it wasn't clear a request had even
run — the only feedback for a tool-only turn was a raw
`→ effect_apply {...}` line, and the system prompt's "keep replies
short" instruction meant the model could end a turn with no text at all.

Fixed both. `QTextEdit` → `QTextBrowser` for the transcript so
suggestions render as real clickable links (`anchorClicked`); on load it
shows a greeting plus three suggestions — remove background noise
(still selection-gated, same auto-apply-when-you-click-a-clip flow),
list the timeline's clips, and a generic "what can you help me with".
Clicking one sends that exact prompt. Tool calls now narrate in plain
language ("Adding \"AI Noise Removal (DeepFilterNet)\"…") instead of raw
JSON. An indeterminate progress bar shows next to the status label
while a request is in flight. And critically: a turn that ends with no
assistant text now always leaves a visible "✓ Done." line, so "did it
run?" always has an answer on screen.

## 2026-08-30 — the panel could not have told you either way

Follow-up: still couldn't hear a difference from DeepFilterNet, even
with the "✓ Done." fix. Traced it to a real bug, not a listening
problem — `effect_apply` called `TimelineController::addEffectToClip()`,
which returns `void`, and then unconditionally returned `{"ok": true}`.
The tool had no way to know whether Kdenlive actually accepted the
effect; it just assumed. "✓ Done." was therefore possibly true, possibly
not — no way to tell from the code as it stood.

Fixed by going one layer lower: `TimelineModel::addClipEffect()` returns
the clip ids it actually touched, and `EffectStackModel::hasFilter()`
confirms the effect is really on the stack afterward. `effect_apply` now
only reports success once both check out, includes whether the effect
was already present (skips duplicate-adding), and a
`VibeCutAgent::toolFailed` signal surfaces a failure the instant it
happens rather than risking it get lost in an empty final reply.

Next: rerun against the cafe interview with this fix in and actually
confirm, via the Effect/Composition Stack panel, that the effect is
present — that's the ground truth no chat transcript claim can
substitute for.

## 2026-08-30 — found it: disabled thinking was dropping tool calls

The verification fix immediately paid off — instead of an unfalsifiable
"✓ Done.", the real run showed exactly what happened: `timeline_get_selection`
ran, then the turn ended with empty text and *no `effect_apply` call at
all*. Not called-and-failed — never called. Anthropic's own docs flag
this: with thinking disabled, Claude can end an agentic turn without
emitting a tool_use block it clearly intended to, no error raised. The
POC had shipped with thinking off (to keep the first version of the SSE
stream handler simpler) with a code comment to revisit it "once the loop
is proven" — it just proved itself unproven.

Flipped `thinking` from `{"type":"disabled"}` to `{"type":"adaptive"}`.
The stream handler already reconstructed and replayed thinking blocks
(with their signature) generically, so nothing else needed to change —
that generic handling had been written defensively for exactly this
flip. Rebuilt and installed; next real test is the cafe interview again.
It worked.

## 2026-08-30 — the Windsurf pivot, and subtitles end to end

Asked for subtitle generation next. Kdenlive already has real Whisper/Vosk
speech-to-text — this wasn't starting from nothing — but my first plan was
too timid: point the user at Kdenlive's own Settings dialog to install a
model first, then wire the chat trigger on top. The user pushed back hard,
and rightly: *"the chat panel tells the agent what to do, but the whole
program should be able to be operated through chat... go have a look at how
windsurf functioned."*

That's a real, standing shift in how the Native-mode tool surface should
grow — not a narrow allowlist requiring a GUI detour and a new PR for every
capability, but broad real control, the way Claude Code relates to a
repository rather than a fixed menu. Concretely: when Kdenlive's own code
already knows how to install its own dependencies, the agent should drive
that directly.

Traced the actual (non-dialog) entry points into Kdenlive's Python/pip
installer: `AbstractPythonInterface::installMissingDependencies()` for the
venv/pip bootstrap, `SpeechToTextWhisper::runConcurrentScript()` for the
model download — both genuinely async (`QFuture`-backed), neither pops a
window, unlike the dialog-wrapped `installNewModel()` the Settings UI uses.
One native KDE confirmation dialog is unavoidable the first time (Kdenlive's
own consent gate for an unattended multi-hundred-MB-to-GB download) — kept
that rather than routing around a deliberate upstream safety check, and
taught the model to warn the user about it.

Landed as three new tools:
- `speech_status` — installed/ready state, installed models, in-progress flag.
- `speech_setup` — drives the real installer + model download, returns
  immediately, progress arrives in the panel via a new
  `VibeCutTools::backgroundProgress` signal that isn't tied to any
  particular chat turn (setup can finish long after the turn that started
  it ended).
- `generate_subtitles` — exports the timeline's audio (mirroring
  `SpeechDialog`'s MLT multitrack render, kept the known "runs synchronously
  on the calling thread" limitation upstream already has, since it's
  audio-only and fast relative to the timeline), creates a subtitle track
  if none exists, runs Whisper as an async `QProcess` (this is the part
  that's actually slow, and it never blocks the GUI), and imports the
  resulting SRT via `SubtitleModel::importSubtitle()` on completion.

Next: run it for real on the cafe interview.

## 2026-08-30 — auto-target the clip, stop hiding what tools already know

First live compound request ("remove background noise/conversations and
add subtitles") surfaced two real gaps at once, both diagnosed straight
from the terminal logs the user pasted:

- Nothing was selected, and the timeline actually has an AV-split pair
  (Kdenlive's "split audio to a new track" feature: a video-only clip
  mirrored to an audio-only twin). The model applied the audio denoise
  effect to *both*. The audio one worked; the video-only one was always
  going to fail — `hasFilter()` correctly reported it, but nothing had
  ever taught the tool that a video-only clip can't host an audio
  effect in the first place. `resolveTargetClip()` now filters
  candidates by `PlaylistState` before resolving, and falls back to
  "the one clip on the timeline" instead of demanding a selection when
  there's no real ambiguity.
- The panel showed *that* `timeline_get_selection` and `speech_status`
  ran, never *what they answered*. If the model didn't narrate the
  result, the user had no way to know whether it hit a dead end. Fixed
  with a `toolCompleted` signal the dock renders unconditionally for
  read-only tools — ground truth the code already has, not something
  left to the model's discretion to mention.

Also traced a "why did it try to download again?" question to an actual
filesystem check (`~/.var/app/org.kde.kdenlive` has no venv, no whisper
cache) rather than guessing: nothing had ever actually installed before
— every earlier "checking setup" was a status check that correctly came
back negative, not a wasted repeat.

## 2026-08-31 — the actual root cause: the wrong installer entry point

"Something structural is wrong" was the right call. Repeated live tests
of Whisper install kept landing on the exact same terminal line:
`=== CHECKING SETUP...NO!!!` — never a venv, never a whisper cache,
never a python subprocess, no matter how many times "Install it now"
was clicked. Traced it precisely through Kdenlive's own source instead
of guessing further: `installMissingDependencies()` — the call
`speech_setup` used unconditionally — **cannot create a venv from
scratch**. It calls a private helper whose `forceInstall` parameter
defaults to `false`; with no venv present, that silently no-ops with no
signal and no error. `speech_setup` was reporting `{"started": true}`
turn after turn while genuinely nothing had ever started.

Kdenlive's own "Install" button doesn't call that method unconditionally
either — `PythonDependencyMessage` (in `abstractpythoninterface.cpp`)
switches on `status()`: `NotInstalled`/`Broken`/`Unknown` calls
`checkVenv(false, true)` (the one call that actually creates the venv,
then chains into `installMissingDependencies()` itself once it exists);
`MissingDependencies` calls `installMissingDependencies()` directly,
which is only correct once a venv already exists. `speech_setup` now
mirrors that exact switch instead of guessing at a single call, and
`speech_status` refreshes the cached status before reporting rather than
trusting a stale `Unknown` default.

This is also, most likely, the real explanation behind the empty-turn
chase across several prior commits — a tool reporting "started, installing
in the background" turn after turn with zero real progress ever showing
up is exactly the kind of state that could produce a model with nothing
coherent left to say. Not proven, but far more satisfying than the
hypotheses already ruled out with hard data.

## Whisper: dropped Kdenlive's own installer entirely

Fixing `installMissingDependencies()`'s call path wasn't the end of it.
Same session, three more real failures against the fixed code: a SIGSEGV
mid-`checkpackages.py --upgrade` (confirmed via `coredumpctl`, though
`coredumpctl list` also showed pre-existing crashes on this nightly from
before any vibecut testing — some baseline instability isn't ours), a
5.5-minute pip run that quit having installed nothing, and another
SIGSEGV correlated with a second download call. Also chased, and never
resolved: `KdenliveSettings::speech_system_python` pointed at a fully
verified, working venv (confirmed both via `kreadconfig6` and by
importing torch+whisper by hand inside the sandbox) — Kdenlive's running
process still resolved to its own internal venv anyway, through every
code path tried.

Standing decision, generalized past just this one subsystem: when a
specific Kdenlive subsystem proves unreliable *after* the real bug in it
has already been traced and fixed, stop debugging that subsystem and
wrap it out — drive a small, self-contained, vibecut-owned process for
just that piece instead, reusing only the static parts of Kdenlive's own
implementation (its bundled scripts, called as plain command-line
tools). `AbstractPythonInterface`/`SpeechToTextWhisper` are gone from
`vibecuttools.cpp` entirely now. In their place: a venv vibecut creates
and owns at `QStandardPaths::AppDataLocation +
"/vibecut-whisper-venv"`, driven by three chained `QProcess` stages
(create venv → install deps → download model), each one verified against
the filesystem before advancing to the next rather than trusted on exit
code alone.

Building that turned up one more real bug worth recording, in the
*reused* part this time: the old code's `whisperquery.py task=download`
call passed `model=<name>`, inherited from before without re-checking
the script itself. The actual script wants `url=` and `download_root=`,
full stop — `model=` isn't even a parameter it recognizes. Caught this
the same way as the `installMissingDependencies()` bug: read the real
script instead of trusting a plausible-looking prior call, then verified
every stage live and headless inside the flatpak sandbox
(`flatpak run --command=sh org.kde.kdenlive -c '...'`) before trusting
any of it — venv creation, a real `pip install` landing torch 2.13 with
CUDA available, a real model download (`tiny`, 72MB) landing at the
exact path the C++ now checks for, and `whisper.load_model("tiny")`
actually loading it onto `cuda:0`. Recorded the corrected contract (and
why `<model>.pt` is the wrong thing to check for — several aliases share
one file) in `KDENLIVE_INTERNALS.md` so it doesn't have to be
re-derived.

## 2026-08-31 — "still going" was CPU, not stuck

First real subtitle-generation run after the rewrite came back with "it
says it's still going?" Chased it through real process state rather than
guessing: no live python3 under kdenlive, no crash in `coredumpctl`, but
a leaked ~858MB exported `.wav` sitting in the sandbox's `cache/tmp/` —
and a second one from an earlier run, never cleaned up either. Two real
bugs, both found by reading the actual scripts and settings rather than
trusting the prior call:

1. `generate_subtitles` forwarded `KdenliveSettings::whisperDevice()`,
   which looked like the right setting but defaults to the literal
   string `"cpu"` in `kdenlivesettings.kcfg` — and nothing in vibecut's
   own flow ever offers a way to change it, since the Speech preferences
   page isn't part of this flow anymore. Every transcription was quietly
   running on CPU against a fully verified working CUDA venv. On a
   whole-timeline export (no `clip_id` given) that's tens of minutes of
   audio — slow enough to look indistinguishable from stuck. Fixed by
   ignoring that setting for this call and probing the venv's own
   `torch.cuda.is_available()` directly each time.
2. `QProcess::finished()` never fires if `start()` itself fails — only
   `errorOccurred()` does. The subtitle job had no handler for that, so
   a launch failure would wedge `m_subtitleJobRunning` true forever with
   no failure ever surfaced and the exported audio never cleaned up —
   exactly the state the leaked wav files were evidence of. Added an
   `errorOccurred` handler and routed both failure paths through one
   `finish()` lambda that always clears the flag and removes the temp
   file.

Recorded the device trap in `KDENLIVE_INTERNALS.md` next to the existing
Whisper script notes.

## 2026-09-01 — swapping the brain: a local model stands in for Claude

Read through a `going-forward.md` wishlist (harnesses, skills, an asset
library, a scoped owned folder, persistent memory, a GitHub on-ramp, an
external API) and mapped each item against what `DESIGN_SPECS.md` and
`TODO.md` already covered before touching anything. Almost all of it
already had a home; the one genuinely new, user-picked priority was
first on the list for a reason: **can something other than Claude drive
this agent at all?** Chose `qwen3.8:27b` via a local Ollama install
(already pulled, already running) as the test case.

Verified the wire protocol against the live Ollama server with `curl`
before writing any C++ — real tool-calling (`tool_calls` in the
response), the multi-turn shape (`role: "tool"` messages, no
`tool_call_id` matching needed), and that streaming is NDJSON (one whole
JSON object per line, tool-call arguments arrive complete rather than
incrementally like Anthropic's `input_json_delta`) rather than guessing
from docs. Added a second backend to `VibeCutAgent` chosen by
`VIBECUT_BACKEND=ollama` (default stays `anthropic`, nothing changes for
an unconfigured checkout): all the already-verified turn-management logic
(retry-on-empty-turn, the tool-execution loop, the max-turn cap) stayed
fully shared, since the internal history representation stays
Anthropic-content-block-shaped regardless of backend and only gets
translated to Ollama's flatter shape at the request-building boundary.
Smaller, more reviewable change than a full backend-interface rewrite.

First live test immediately exposed a real, separate bug that had
nothing to do with the new backend: asked it to "fix the colour levels"
and it refused, correctly reporting that only `denoise`/`denoise_light`
were on `effect_apply`'s allowlist. The user's reaction was the right
one — *"It should be able to do anything within the program. It can't be
limited to the things I've requested."* This is exactly what
`DESIGN_SPECS.md` §2 already says the tool surface should be, just never
followed through on past the first two entries. The real fix: stopped
hand-picking effect ids and validated against `EffectsRepository::get()`
instead — the same real repository Kdenlive's own "Add Effect" panel
reads from — plus a new `effect_search` tool (name/id substring search,
capped results) so the model can discover the right id instead of
guessing. `resolveEffectId()` now checks the small friendly-alias map
first, then falls through to "is this a real Kdenlive asset id" via
`exists()`. The audio-only clip-compatibility check generalised the same
way: `EffectsRepository::isAudioEffect()` decides which kind of clip an
effect needs, instead of a hardcoded audio-only assumption baked in from
when denoise was the only option. Hit two dumb brace-matching bugs
writing the new JSON-Schema blocks by hand (dropped a closing brace
each time, both caught by the build failing, not glossed over) — wrote a
small Python brace-balance checker and hand-traced both schema blocks
before the third rebuild rather than guessing a third time.

Second live test (a compound request: list clips, fix color on the video
track, fix audio on the audio track) surfaced the real reliability
problem with this backend: turns kept coming back completely empty (no
text, no tool call) right after a tool result — not a parsing bug (ruled
out with a direct `curl` repro reproducing the exact message shape, both
streamed and non-streamed, which came back with real content both
times), more likely a reasoning-model sampling quirk (`qwen3.8:27b`'s own
defaults are `temperature 1`, easy for a thinking-heavy model to emit an
early stop right after its thinking closes). Bumping `num_ctx` from
Ollama's 4096 default to 32768 was the first guess and didn't fix it —
worth recording since a plausible-sounding first theory (context
exhaustion) turned out to be wrong, confirmed by the bug recurring at
only ~800 bytes of history. Two real fixes instead: dropped the sampling
temperature to 0.3 (standard practice for tool-calling agents regardless
of root cause), and stopped sharing one `m_emptyTurnRetries` budget
across an entire compound exchange — it now resets after every
successful tool call, so a 4-tool-call exchange gets a fresh two-attempt
allowance at each step instead of exhausting a shared pool by its second
tool call (confirmed exhausting it before this fix: a real exchange
ended with tool calls that had genuinely executed but no closing text,
shown to the user as tool results with no wrap-up sentence — not a lie,
but a bad experience).

Third live test (the same compound request) held up under the
reliability fix and it's the best evidence yet this actually works: the
model initially mislabelled which clip was audio vs. video in its own
prose (backwards from the real assignment `effect_apply` had already
proven a session earlier), tried to denoise the video-only clip, got a
real rejection from the tool, and **self-corrected from the error
message alone** — no user intervention needed — before finding `gain`
and `avfilter.colorcorrect` via `effect_search` (neither hand-picked,
both genuinely discovered) and applying them for real (cross-checked
against the app's own effect-repository parsing log, not just trusted
narration). The wrong-clip guess turning into a caught-and-corrected
error rather than a silent wrong action is `DESIGN_SPECS.md` §3's
verification discipline paying for itself on a second backend.

Two gaps found live, deliberately not fixed tonight: `timeline_list_clips`
doesn't return each clip's audio/video type, so the model has no grounded
way to know which is which short of trial and error against
`effect_apply`'s real check — the mislabelling above was this gap, not a
one-off. And `effect_apply` can add an effect but has no way to set its
parameters — `avfilter.colorlevels`/`avfilter.colorcorrect` land with
every value at its identity default (confirmed: user added it, "it didn't
modify the existing levels"), so it's a real effect but a functional
no-op until something can drive its parameters too. Both queued in
`TODO.md` rather than scope-crept into tonight's change.

Also worth a note for later: found
[browser-use/video-use](https://github.com/browser-use/video-use) — an
agent-driven video editor with real design overlap. Its two-layer context
strategy (a structured transcript with word-level timestamps + speaker
ID as the primary data, on-demand filmstrip/waveform composites only at
actual decision points, ~12KB/project total) is a better shape for the
still-queued subtitle-read tool than plain line search. It also keeps a
`project.md` for session continuity — independent validation of the
`going-forward.md` memory-file idea, same mechanism. And it's built
specifically around removing filler words/dead space between takes,
which is a real candidate answer to the still-open "what takes longest in
a real editing session?" question.
