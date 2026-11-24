# AMETHYST

A small Windows DAW built for recording vocals. It hosts VST3 effects and saves channel presets. It is written in C++ on JUCE 8.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The app is built to `build\VocalDAW_artefacts\Release\AMETHYST.exe`.

## macOS (13 Ventura and later)

The same code builds a universal Mac app that runs natively on both Apple Silicon and Intel Macs. On macOS it hosts **VST3 and Audio Units**, and it keeps your sign-in in the **Keychain**. A Mac build has to be compiled on a Mac, and there are two ways to do that:

- **GitHub (no Mac needed):** push this folder to a GitHub repo and add the secrets `SUPABASE_URL` and `SUPABASE_ANON_KEY` in the repo settings. `.github/workflows/macos.yml` then builds on a cloud Mac, runs `--selftest`, and uploads `AMETHYST-macOS.dmg` under the run's **Artifacts**. You can also start it by hand from the Actions tab ("Run workflow").
- **On a Mac:** install Xcode and `brew install cmake`, then run `./build-mac.sh`.

The first time AMETHYST opens, macOS asks for microphone access, and may ask about the Documents folder too. Allow both.

The app is ad-hoc signed, not notarised. Because of that, the first time you open a downloaded copy you need to **right-click → Open** (or use System Settings → Privacy & Security → Open Anyway). Distributing it to other people without that warning needs an Apple Developer ID ($99/year) and notarisation, which can be added to the workflow later.

## First run

1. **Audio**: choose your interface. ASIO or WASAPI with a small buffer keeps latency low. Then enable the inputs your mic uses.
2. **Plug-ins**: click **Options â†’ Scan for new or updated VST3 plug-ins** (the default folder is `C:\Program Files\Common Files\VST3`).
3. If no input signal arrives, turn on Windows **Settings â†’ Privacy â†’ Microphone â†’ Let desktop apps access your microphone**.

## Recording vocals

- Pick the track's input (mono `In 1`, or a stereo pair).
- **R** arms the track. **I** turns on input monitoring, so you hear yourself through the insert chain.
- Press **Rec** (or the `R` key) to record. Press **Stop** to end the take, and Stop again to return to the start.
- Takes are saved as 24-bit WAV files in `Documents\AMETHYST\Takes`. Each take is moved back by the device's reported round-trip latency so it lines up with the backing track. A new take is added on top of any older ones.
- **Click** turns the metronome on (it follows the BPM). Plug-ins also get the tempo and transport position, so tempo-synced delays work.

## Tracks and editing

**Track list.** New sessions start with 8 slim track rows. Each row has:
- the track name (double-click to rename)
- a red **record-arm** light and an **on/off** light (mute)
- a **...** menu with recording input, input monitoring, solo, plug-ins in the mixer, channel presets, overlapping takes, and delete
- small chips that show the current input, MON, SOLO, and the number of FX

Volume and pan are in the **Mixer** (Tab key).

**Tools** (top-left above the tracks):
- **Move (V):** drag a clip to move it. Drag either edge of a clip to trim it.
- **Trim (T):** grab the left half of a clip to trim its start, or the right half to trim its end.
- **Cut (C):** click a clip to split it there. A preview line shows where the cut will go.
- Hold **Shift** while moving, trimming or cutting to snap to beats.

Trimming is non-destructive: drag an edge back out, or right-click a clip → **Restore full length**. Right-click a clip for:
- Split here / Split at playhead
- Trim start or end to playhead
- Delete
- Clip properties

Cut and trimmed edges get tiny automatic fades, so they don't click.

**Recording over old takes.** Earlier takes on a track keep playing while you record a new one, so you can hear what you're doubling. Afterwards, overlapping takes are **layered** (you hear all of them) by default. Switch a track to **Newest take on top** from its **...** menu, or by right-clicking its lane.

## Accounts and AMETHYST Pro

The 10 built-in AMETHYST effects are **Pro** features. Everything else is free: recording, mixing, VST3s, clip effects, presets and export.

- The **Sign in** button (top right) opens the account window. From there you can sign in, create an account, reset your password, refresh your plan, or sign out.
- When signed in, the button shows **FREE** or **PRO**.
- **Profile pictures (Pro only):** in the account window, **Change picture...** lets you pick a PNG or JPG. It's cropped to a circle, uploaded to your account, and shown in the toolbar. Free accounts see their initial instead.
- Free users see the built-in effects with a **PRO** padlock badge. Adding one opens the upgrade prompt.
- If a project or channel preset uses a Pro effect, it opens without that effect and shows a message saying so.
- The session is saved encrypted with Windows DPAPI, so it only works for your Windows user. Pro keeps working offline for 7 days after the last successful check. The app re-checks your plan every 30 minutes.

### Setting up Supabase

1. Create a Supabase project. Under **Authentication → Providers**, make sure Email is enabled. Leaving "Confirm email" on is fine, because the app handles it.
2. Open the **SQL editor** and run `supabase/migrations/20261124000000_entitlements.sql`. Then run `supabase/migrations/20261124010000_avatars.sql`, which sets up **profile pictures**: a private `avatars` storage bucket where only active Pro users can upload, enforced by the database. It creates an `entitlements` table with row-level security: users can read their own row but can't change it. It also adds a trigger that gives every new user a free row.
3. Copy `config/supabase.example.json` to `config/supabase.json` (this file is gitignored). Fill in your **Project URL** and **anon / publishable key** from Project Settings → API. Never use the `service_role` key here.
4. Rebuild. CMake prints `AMETHYST: Supabase configured`.

**Until payments are set up**, you can give someone Pro by hand in the SQL editor:

```sql
update public.entitlements set is_pro = true, plan = 'pro_manual', pro_until = null
where user_id = (select id from auth.users where email = 'someone@example.com');
```

Later, a payment webhook (for example, a Supabase Edge Function using the service-role key) only has to set `is_pro`, `plan` and `pro_until`. The app needs no changes.

**Note:** the Pro check runs inside the app, so a determined person could patch it out. That's normal for desktop apps. Stronger protection, such as server-signed licences, can come with payments.

To test the account client without the real server: `AMETHYST.exe --account-test <url> <anon-key>` runs sign-in, refresh, entitlement, sign-up and reset against whatever server you point it at.

## Built-in effects

AMETHYST ships with 10 effects of its own. They appear at the top of the plug-in browser, marked with the gem icon. Each has:
- factory presets, with **<** and **>** to step through them
- **Save preset...**, which saves to `Documents\AMETHYST\Plugin Presets`
- Randomize and Reset

| Effect | What it is |
|---|---|
| **Facet EQ** | 7-band parametric EQ with a live spectrum. Drag a node to change frequency and gain, scroll on it for Q, double-click it to flatten, right-click it to change the filter type. |
| **Prism Dynamics** | Limiter or compressor, with saturation, a ceiling, attack/release, parallel mix and a noise gate. Shows a scrolling gain-reduction graph. |
| **Geode Reverb** | Pre-delay, early reflections, a modulated tail, low/high cut, width, and dry/early/wet levels. Shows an animated gem. |
| **Crystal Echo** | Delay with Normal, Invert or Ping-pong feedback, a feedback filter, tempo-sync divisions, and stereo offset. |
| **Prism Mod** | Flanger, chorus (3 voices) or vibrato, with stereo spread and feedback. |
| **Lustre** | One-knob enhancer: three-band compression and saturation, with flavours A–D. |
| **Silk De-Esser** | Tames S and T sounds. Split or wide mode, plus a Listen switch to hear what it detects. |
| **Ember** | Tape, tube, fuzz or bit-crush saturation, with tone and mix. Shows the transfer curve. |
| **Twin Voice** | Vocal doubler: two detuned voices with timing variation, spread across the stereo field. |
| **Shift** | Pitch shifter (±12 semitones plus fine-tune) for harmonies and octave effects. |

Running `AMETHYST.exe --selftest` checks every factory preset of every effect, and the clip-effects renderer, without opening the UI.

## Mixer and plug-in slots (VST3)

Click **Mixer** (or press `Tab`) to open the mixer. It has three parts:
- **Left:** a searchable list of your scanned effect plug-ins.
- **Middle:** a vertical strip for the Master channel and for each track. Each strip has a name, a record-arm LED, pan, mute/solo, a fader and meters. Click a strip to select it.
- **Right:** "Mixer - <channel>", showing the input selector and **10 plug-in slots** for the selected channel. The Master channel has its own slots too.

To put a plug-in in a slot, you can:
- drag it from the list onto a slot,
- double-click it in the list (it goes into the first free slot),
- drag a `.vst3` file from Explorer onto a slot, or
- click an empty slot and pick from the menu.

Each slot also lets you:
- click it to open the plug-in's editor
- use the **LED** to switch the slot on or off
- use the **knob** to set the wet/dry mix
- drag it onto another slot to swap the two
- right-click it to replace, move or remove the plug-in

**FX** on a track's header jumps to that track in the mixer.

## Look ("Amethyst" glass theme)

The interface uses a custom dark-purple "liquid glass" style throughout:
- frosted, see-through panels
- glossy knobs and faders
- violet-to-pink glow on anything that's switched on

The transport bar includes:
- **Play/Stop/Record**, plus a return-to-start button
- **Tempo:** drag it up or down (hold Shift for fine steps), scroll on it, or double-click to type a value
- **Metronome**
- **Position:** click it to switch between bar:beat:tick and min:sec.ms
- **CPU and memory**, shown live
- **Master** volume and level meter

## Clip properties (double-click a clip)

Each clip has its own non-destructive settings. The original file is never changed: the settings are applied in the background, and the waveform updates to show the result.
- **Header:** clip on/off, Pan, Vol, Pitch (Â±24 semitones).
- **File:** replace the audio, show it in Explorer, or delete the clip.
- **Time stretching:**
  - *Resample* mode: changing speed also changes pitch, like tape.
  - *Stretch* mode: speed and pitch change independently. Fine-tune knob in cents.
- **Shape page:**
  - Switches: Remove DC offset, Normalize, Reverse, Invert polarity, Stereo fade, Swap stereo.
  - Knobs: Start, Length, Fade in, Fade out, Trim (cuts silence below a threshold).
- **Color page:** Boost, Tone (darkâ†”bright), Ring-mod mix and frequency, low-pass Cut and Res, Soft clip, Reverb (A = room, B = hall), tempo-synced ping-pong Delay, and Pogo (a pitch glide across the clip).
- **Render:** declicking (tiny fades at the clip edges), source/result info, and Reset all.

Clips with effects show `*fx` on their label. All of these settings are saved in the project.

## Importing audio (drag and drop)

You can drag WAV, MP3, FLAC, AIFF, OGG or WMA files from Explorer onto the arrangement:
- **On a track lane:** the clip lands on that track, at the point where you dropped it.
- **Below the last track:** a new track is created, named after the file.
- **Several files at once:** the first goes where you dropped it, and each of the others gets its own track.
- **Into the mixer:** the file goes onto the selected track at the playhead.

Once a clip is on a track:
- **Drag** it to move it. Hold Shift while dragging to snap to beats.
- **Right-click** it for Delete, Show in Explorer, or Move to playhead.
- A track can hold any number of clips. Where clips overlap, the newest one plays, so a re-recorded take covers the old one.

## Channel presets

On any track, **Preset â†’ Save channel preset...** saves the following to a `.vchp` file in `Documents\AMETHYST\Channel Presets`:
- the fader and pan settings
- the whole insert chain: which plug-ins, the order, the bypass state, and each plug-in's full internal state

Saved presets appear in the same menu, so you can load one onto any track with a click.

## Projects and export

- **Save/Open** work with `.amethyst` session files (older `.vdaw` projects still open), which hold the tracks, routing, channel strips and takes. Shift-click Save for Save As.
- **Export** renders the master bus to a stereo WAV in real time (with a 2-second tail). The metronome is left out of the export.

Keys: `Space` play/pause Â· `R` record Â· `Home` return to start Â· `Ctrl+S` save Â· `Ctrl+O` open

## Known limits

- You can move and delete clips, but you can't trim or split them yet, and there's no undo.
- Imported files are referenced where they are, not copied into the project.
- No plug-in delay compensation between tracks.
- Plug-ins are scanned in-process. A plug-in that crashes during a scan gets blacklisted the next time you scan.
- Takes recorded at a different sample rate are resampled when they load. Changing the device rate while a session is open does not reload clips that are already loaded.
