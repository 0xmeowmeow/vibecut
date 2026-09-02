# Kdenlive internals — reference notes

Not a full architecture graph — targeted notes on the specific subsystems
vibecut has had to reverse-engineer to drive Kdenlive from code, grown as
we go rather than built upfront. Each section: what it is, the real
working call path (not the plausibly-named method), and the traps
already hit. Add a section here whenever a new subsystem gets touched —
that's the payoff, not a one-time exhaustive pass.

## Timeline clips and effects

- A clip on the timeline is an integer id. `TimelineModel::isClip(id)`
  validates it; `getClipName`, `getClipTrackId`, `getClipPosition`,
  `getClipPlaytime`, `getClipBinId` read it.
- **Adding an effect**: `TimelineController::addEffectToClip()` returns
  `void` — it cannot tell you whether Kdenlive actually accepted the
  effect. Call `TimelineModel::addClipEffect(clipId, effectId)` directly
  instead (it returns the clip ids it actually touched), then confirm
  via `TimelineModel::getClipEffectStack(clipId)->hasFilter(effectId)`.
  Never trust "the call didn't throw" as success.
- **AV-split pairs**: Kdenlive's "split audio to a new track" feature
  produces two linked clips from one source — a video-only clip and an
  audio-only mirror (`mirrorTrack` attribute in the project XML), joined
  by an `AVSplit` group. Their `state` is `PlaylistState::ClipState`:
  `VideoOnly = 1`, `AudioOnly = 2`, `Disabled = 3`, `Unknown = 4`
  (`src/definitions.h`). **An audio effect can never land on the
  video-only half** — `TimelineModel::getClipState(clipId).first` tells
  you which is which. Filter candidates by this *before* attempting to
  apply, not after failing.
- When resolving "which clip" with no explicit id and nothing selected:
  if there's exactly one *eligible* clip on the timeline, use it — don't
  force a selection for the common single-clip case. Multiple eligible
  candidates with nothing selected is real ambiguity; list them so the
  question asked back is specific.
- **`EffectsRepository` is the real effect list** (`src/effects/
  effectsrepository.hpp`, singleton via `EffectsRepository::get()`) — the
  same source Kdenlive's own "Add Effect" panel reads from. Found while
  fixing `effect_apply` (2026-09-01): it used to hand-validate against a
  two-entry hardcoded map (`denoise`/`denoise_light`) instead of this,
  which is why it rejected anything else outright rather than a real
  "not found." Useful methods: `exists(assetId)` (the actual validity
  check — anything it accepts is something Kdenlive can really do),
  `getNames()` (returns `QVector<QPair<id, displayName>>` for every
  effect — the base for a search/discovery tool, not something to dump
  unfiltered), `isAudioEffect(assetId)` (real type classification —
  generalises the AV-split video/audio-clip compatibility check above to
  any effect, not just the two that used to be allowlisted),
  `getDescription(assetId)`. Also worth remembering: adding an effect via
  `TimelineModel::addClipEffect()` only ever adds it with its XML-defined
  default parameter values — for an effect like `avfilter.colorlevels` or
  `avfilter.colorcorrect` those defaults are an identity transform, so
  the clip visibly doesn't change even though the effect really is on the
  stack. Confirming `hasFilter()` proves the effect landed, not that it
  does anything yet — a separate, still-open gap (see `TODO.md`).
- **An effect parameter's real MLT name is not its display name**, and its
  stored value format depends on its XML `type=` attribute - found live
  2026-09-02 building parameter support into `effect_apply`. Two separate
  traps, same underlying cause (guessing instead of reading the XML):
  - `avfilter.colortemperature`'s "Color Temperature" slider is really
    named `av.temperature`, not `temperature` - Kdenlive prefixes many
    avfilter-wrapped parameters with `av.`. `EffectsRepository::getXml
    (assetId)` returns the effect's `<parameter name=... type=... default=
    ... min=... max=...>` list directly (the same file the UI builds its
    sliders from) - read real names from there, never guess from the
    `<name>` display text.
  - Parameters whose `type=` is one of a specific set need a
    `"start=value"` keyframe-list string (e.g. `"0=6500"`), not a bare
    value (`"6500"`) - `AssetParameterModel::setParameter()` will happily
    accept and store the bare form, and it round-trips through
    `getParam()` unchanged, so a naive "read it back and compare" check
    reports success - but MLT's animation parser can't interpret the bare
    form and silently falls back to the parameter's built-in default at
    render/display time. The real list of which types need this comes
    from `AssetParameterModel::isAnimated(ParamType)`
    (`src/assets/model/assetparametermodel.cpp`) - `protected`, so not
    directly callable, but small and stable enough to mirror by XML type
    string: `keyframe`/`animated`, `animatedfakepoint`, `animatedpoint`,
    `animatedrect`/`rect`, `animatedfakerect`, `colorwheel`,
    `roto-spline`, and (surprisingly) plain `color` too. Its sibling
    `getDefaultKeyframes()` builds the real prefix honouring the user's
    configured default interpolation (`=`/`|=`/`~=`) - also `protected`;
    vibecut's own version just always uses linear `=`, which is what
    every observed default in real project files uses.
- **Some parameters display a `factor=`-scaled number in the UI, not the
  raw stored value** - e.g. `frei0r.contrast0r`'s `Contrast`
  (`default="0.5" factor="500"`) and `frei0r.saturat0r`'s `Saturation`
  (`default="0.125" factor="1000"`). Confirmed live 2026-09-02: setting
  `Contrast` to `1.3` via `effect_apply` shows as **650** in Kdenlive's
  effect stack (`1.3 × 500`), and `Saturation` to `0.7` shows as **700**
  (`0.7 × 1000`) - not a bug, that's the UI's own slider convention (same
  number a human dragging that exact position would see). If a value
  looks "wrong" by a clean multiple in the stack UI, check the XML's
  `factor=` attribute before assuming the write failed - also seen on
  `lift_gamma_gain`'s colorwheel params (`factor="100"`). Also worth
  noting: frei0r-native effects (`Contrast`, `Saturation`) and native MLT
  filters (`lift_gamma_gain`'s `gain_b` etc.) don't use the `av.` prefix
  convention at all - that's specific to avfilter-wrapped effects. One
  more reason real names have to come from `effect_search`/`getXml()`,
  never a guessed convention.

## Timeline structural editing (move/trim/cut/gaps)

Genuinely separate subsystem from the effects one above - lives in
`TimelineFunctions` (`src/timeline2/model/timelinefunctions.hpp`), a
`struct` (so everything before its one `private:` block, most of it, is
public by default) of static methods: `requestClipCut`, `pasteClips`,
`requestMultipleClipsInsertion`, `requestDeleteBlankAt`/
`requestDeleteAllBlanksFrom` (gap removal), `requestSpacerStartOperation`/
`requestSpacerEndOperation` (drag-to-close-gap), `extractZone`/`liftZone`,
`requestSplitAudio`/`requestSplitVideo`, and more - real timeline editing,
not effect application.

- **`TimelineController::removeSpace()` is another void-returning UI
  wrapper**, same trap as `addEffectToClip()` - it resolves `trackId`/
  `frame` from GUI cursor state (`getMenuOrTimelinePos()`, `m_activeTrack`)
  when passed `-1`, which doesn't exist in a programmatic caller, then
  calls the real function and discards its `bool` result. Call
  `TimelineFunctions::requestDeleteBlankAt(timeline, trackId, position,
  affectAllTracks)` directly instead - it's public, returns a real
  success/failure, and is exactly what the wrapper calls internally.
- **Gap detection has no reachable API** - `TrackModel::isBlankAt`/
  `getBlankStart`/`getBlankEnd` are `protected`. Derive gaps the same way
  a human looks at the timeline: per track, sort clips by
  `getClipPosition()`, and anywhere `nextStart > prevEnd` there's a gap of
  `nextStart - prevEnd` frames starting at `prevEnd`.
- **`affectAllTracks=true` requires every unlocked track to have a blank
  at that exact position**, not just the track you care about (confirmed
  from the source: it loops all unlocked tracks and returns `false` if any
  one of them isn't blank there) - correct for keeping an AV-split
  video/audio pair in sync (they share position by construction, so a
  real gap between two pieces of footage shows up on both), but it can
  fail on a project with other unrelated tracks that don't happen to have
  a gap at that same frame. Worth falling back to `affectAllTracks=false`
  per-track if the all-tracks call fails, rather than giving up.
- **Closing a gap shifts every later clip left** - positions computed
  before one removal are stale for the next. Re-derive gaps fresh after
  each successful close rather than computing a full gap list once
  upfront (mirrors how Kdenlive's own `requestDeleteAllBlanksFrom` loops
  internally for the subtitle-track case).

## LADSPA audio effects

- Kdenlive's XML effect id convention is `ladspa.<uniqueid>` — the
  LADSPA plugin's unique ID from its `ladspa_descriptor()`. MLT finds the
  plugin by scanning `LADSPA_PATH` (set in `.flatpak-manifest.json`'s
  `finish-args`) for any `.so` whose unique id matches; filename doesn't
  matter.
- Effect XML `<parameter name="N">` uses the **absolute LADSPA port
  index** (0 = first audio in, 1 = first audio out for a mono plugin,
  controls start after the audio ports) — not a control-port-relative
  index. Confirmed against both the shipped RNNoise effect and a custom
  one (DeepFilterNet, `ladspa.7843795`).
- `analyseplugin` (from the `ladspa-sdk` package, present in the Flatpak
  runtime) dumps a `.so`'s exact port layout, ids, and defaults — use it
  to write the effect XML instead of guessing from the plugin's source.
- `melt -filter ladspa.<id> <params> -consumer ...` runs a LADSPA effect
  standalone, headless, outside Kdenlive entirely — the fastest way to
  confirm a new effect actually processes audio before wiring any C++.

## Subtitle track lifecycle

- A project has **no subtitle track by default**;
  `TimelineModel::getSubtitleModel()` returns `nullptr` until one exists.
  `TimelineModel::hasSubtitleModel()` checks; `TimelineItemModel::
  createSubtitleModel()` creates one (mirrors
  `MainWindow::slotEditSubtitle()`'s create path) — also call
  `pCore->subtitleWidget()->setModel(...)` and
  `TimelineWidget::connectSubtitleModel(true)` afterward so the UI picks
  it up.
- Import an SRT with `SubtitleModel::importSubtitle(path, offsetFrames,
  externalImport=true)`.

## Exporting timeline audio to a file (headless)

Mirrors `SpeechDialog::slotProcessSpeech()`'s render step:

1. `TimelineItemModel::sceneList(rootDir, destPath)` writes the
   project's MLT XML scene to `destPath`.
2. Build an `Mlt::Producer(profile, "xml", scenePath)` from it, walk its
   service chain (`Mlt::Service::producer()` repeatedly) to find the
   `Mlt::Multitrack` whose track count matches the project's track
   count, then `set("hide", 3)` on tracks you want silent (video tracks
   have `hide == 1` by convention — hide those; keep all audio tracks
   for a full mixdown, or hide all-but-one for a single track).
3. Render with an `Mlt::Consumer(profile, "avformat", outPath)`,
   `set("properties", "WAV")`, `producer.set_in_and_out(inFrame,
   outFrame)`, `consumer.connect(producer)`, `consumer.run()`.

This render is **synchronous on the calling thread** (a known,
acknowledged limitation in Kdenlive's own code too — audio-only, so fast
relative to project length, but not truly non-blocking). Needs
`#include "mlt++/MltConsumer.h"`, `"mlt++/MltProfile.h"`,
`"mlt++/MltTractor.h"` — `Mlt::Producer`/`Service`/`Multitrack` resolve
transitively from those.

## The Python plugin install state machine (`AbstractPythonInterface`)

This is the one that cost the most time — traced the hard way instead of
guessed at, twice, before landing on the right call. Read this before
touching *any* optional Python-backed feature (speech-to-text, SAM,
seamless translation — anything under `src/pythoninterfaces/`).

**State**: `InstallStatus { Unknown, NotInstalled, Installed, InProgress,
MissingDependencies, Broken }`, cached in `m_installStatus`. Starts
`Unknown` until something actually probes the venv — don't trust
`status()` without refreshing first.

**The refresh call that's actually correct**: `checkSetup(requestInstall,
&newInstall)`. It resolves `venvPythonExecs()` and, if both python and
pip resolve, sets status to `Installed` — **in either bootstrapped-venv
or `speech_system_python` mode**. `checkVenv()`'s own
`speech_system_python` fast path (`if (useSystemPython()) return true;`)
returns success **without ever updating the cached status** — call
`checkVenv()` directly for a refresh and a system-Python setup reports
`Unknown` forever.

**The real "Install" trigger, mirroring Kdenlive's own button**
(`PythonDependencyMessage` in `abstractpythoninterface.cpp`) — branch on
`status()`, don't call one method unconditionally:

| `status()` | Correct call |
|---|---|
| `Installed` | nothing to do |
| `NotInstalled` / `Broken` / `Unknown` | `checkVenv(false, true)` — creates the venv (blocking `python -m venv` + a smoke-test package install), *then chains into `installMissingDependencies()` itself* once it exists |
| `MissingDependencies` | `installMissingDependencies()` directly — **only valid once a venv already exists** |
| `InProgress` | no-op |

**The trap**: `installMissingDependencies()` alone, called on a system
with no venv, looks like it should work (it pops a confirmation dialog,
sets status to `InProgress`) but silently does nothing. It calls a
private helper (`runPackageScript`) whose `forceInstall` parameter
defaults to `false`; with no venv, that hits `checkVenv(false, false)` →
`if (!forceInstall) return false;` and gives up before ever reaching
`setupVenv()`. No venv gets created, no signal fires, no error — it just
silently reports `{"started": true}` forever. The exact tell in the
terminal log: `"=== CHECKING SETUP...NO!!!"`.

**Superseded**: after this state machine caused three separate real
failures in testing (a crash, a silent no-op, a documented setting that
wasn't honored — see `vibecut-bypass-unreliable-subsystems` in project
memory), vibecut stopped depending on `AbstractPythonInterface` for
Whisper entirely. `src/vibecut/vibecuttools.{h,cpp}` now drives its own
venv (`QStandardPaths::AppDataLocation + "/vibecut-whisper-venv"`) with
plain `QProcess` calls, reusing only the static, stable parts of
Kdenlive's own Whisper support — its bundled scripts under
`scripts/whisper/`, located via the same `QStandardPaths::locate`
pattern `AbstractPythonInterface::runConcurrentScript` uses internally.
Everything below this point in the section is now historical — what the
*old*, no-longer-used code path did — kept for context and in case a
future optional feature (SAM, seamless translation) still goes through
it.

**The scripts' actual CLI contracts** (verified by reading the scripts
directly and running each stage headless via `flatpak run --command=sh
org.kde.kdenlive`, not by trusting the old code's assumptions — one of
which turned out wrong):

- `whisperquery.py task=download` takes `url=<full model URL>` and
  `download_root=<directory>` — **not** `model=<name>`. The old bypass
  code (and this doc, until now) assumed `model=` because that's the
  name `WhisperDownload`'s dialog surfaces to the user; the actual
  script only understands a literal URL. Get it via `task=list` first,
  which prints every `<alias> : <url>` pair straight from
  openai-whisper's own `_MODELS` table (plus a trailing `root_folder :
  <path>` line — skip it), then pass that URL through. This also
  matters for *detecting* whether a model is installed: several aliases
  share one file (`turbo` and `large-v3-turbo` both resolve to
  `large-v3-turbo.pt`; `large` and `large-v3` both resolve to
  `large-v3.pt`), so checking for a literal `<model>.pt` on disk silently
  misses those — check for `QFileInfo(url).fileName()` instead.
- `whisperquery.py task=list`'s output is pure local computation (reads
  `whisper._MODELS`, no network) — safe to call synchronously and often.
- `whispertosrt.py <audio> <model> [kwargs...]` — positional args are
  audio source path and model name; no explicit output-path argument
  despite what its own header comment claims. It writes `<audio
  basename>.srt` into `os.path.dirname(<audio>)` itself (via
  `whispertotext.run_whisper`'s `output_dir=` kwarg it constructs
  internally) — the caller has to know that convention, not just pass a
  desired path.
- Standard OpenAI Whisper model names apply (`tiny`, `base`, `small`,
  `medium`, `turbo`, `large-v3`, ...); `turbo` is Kdenlive's own UI
  default and vibecut's too.
- **`device=` trap**: `whispertotext.run_whisper()`'s own default is
  `device="cpu"`, and the bypass code originally forwarded
  `KdenliveSettings::whisperDevice()` when calling `whispertosrt.py` —
  looked reasonable, since that's the setting Kdenlive's own Speech
  preferences page writes. Its `<entry>` in `kdenlivesettings.kcfg` has
  `<default>cpu</default>` though, and nothing in vibecut's chat flow
  ever opens that preferences page to change it — so every transcription
  silently ran on CPU regardless of a fully verified working CUDA venv,
  on whatever length of audio `generate_subtitles` exported (the whole
  timeline, if no `clip_id` was given — easily tens of minutes, an
  800MB+ WAV). It "worked", just at CPU speed, which reads indistinguishable
  from stuck. Found by checking real process state (no live python3
  under the running kdenlive, no crash in `coredumpctl`, but a leaked
  multi-hundred-MB export `.wav` still sitting in the sandbox's
  `cache/tmp/` from more than one prior run — `finished()` also never
  fires on a `QProcess::start()` failure, so a stuck job never resets
  `m_subtitleJobRunning` or cleans up its export either, which was the
  first candidate ruled out here). Fixed by ignoring
  `KdenliveSettings::whisperDevice()` for this call entirely and probing
  the vibecut venv's own `torch.cuda.is_available()` fresh each time
  instead — same shape of lesson as `installMissingDependencies()`:
  don't trust a Kdenlive-owned setting that vibecut's own flow has no path
  to actually set.

The now-historical `speech_system_python` bypass setting, for reference:
Kdenlive supports pointing `AbstractPythonInterface` at a Python
environment prepared outside its control —
`KdenliveSettings::speech_system_python` (bool) +
`speech_system_python_path` (path to a `python3` with a sibling `pip3`),
KCFG group `[speech]` in `kdenliverc`. In testing this was set correctly
(confirmed via `kreadconfig6`) and the pointed-at venv was independently
verified fully functional, yet Kdenlive's running process kept resolving
to its own internal venv anyway through every tested code path — root
cause never pinned down (likely some KConfig caching or
initialization-order issue). This is exactly the kind of unreliability
that motivated dropping the whole state machine rather than chasing it
further.

## Flatpak build system

- The exact `flatpak-builder` invocation (flags, and module order in
  `packaging/flatpak/org.kde.kdenlive-dependencies.json`) is load-bearing
  for the build cache — changing a flag, or inserting a module mid-list,
  invalidates everything *after* it and forces a full dependency
  rebuild. New runtime-only modules (a bundled plugin `.so`, say) go at
  the **end** of the dependency list. See `CLAUDE.md` for the pinned
  invocation.
- Debug symbols for a coredump aren't in the base flatpak: `flatpak
  install --user kdenlive-origin org.kde.kdenlive.Debug//master` pulls
  them (~750MB). `coredumpctl gdb <pid>` then resolves real function
  names instead of raw addresses — still needs `set solib-search-path`
  pointed at the flatpak runtime's own library tree to resolve
  everything (host libraries have mismatched build-ids), which wasn't
  fully chased down yet.
- `coredumpctl list` is worth checking before assuming a crash is new —
  this exact nightly build had pre-existing crashes logged from days
  before any vibecut testing, suggesting some baseline instability in
  Kdenlive's own master branch independent of anything we've added.
