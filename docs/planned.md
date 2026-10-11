# Planned: tune history, stability, sharing

Three features that are decided on but not built. This page records the choices made for each, so
that whoever builds one does not have to make them again. Nothing here is in the code yet; the
first step of the same request, testing a saved tune again after a driver update, is
(`core/retest.*`, hardware check 86).

They build on each other in this order, and each is useful without the next.

## 1. History: every run and every tune, best score first

**What the user gets.** A History page with every finished run: when, which profile, on which
driver, what it found, and how the card scored with it. Sorted by score (it/s), highest first. A
tune from the list can be made the saved tune again.

**Choices.**

- **A file of its own, one line per run:** `history.jsonl` in the state folder, appended to and
  flushed like the journal. `gao.json` keeps holding only the one saved tune: boot-apply reads it
  elevated at every logon, and it must stay small and strict.
- **A run is recorded when it ends,** whatever the end: saved, not saved, stopped, at stock after a
  reset. A run that found nothing is in the list too, with why. Without those the list would say
  the card is better than it is.
- **What a line holds:** the time, the profile, the driver version, the card (NVML UUID and its
  name), the tune (power limit, core and memory offset, undervolt, fan curve), the measurement at
  stock and the one at the end (score, clocks, peak temperature, fan speed, power), how it ended,
  and the app's version. Numbers as they were measured; nothing derived is stored.
- **A tune is what a run found, not a row of its own.** Two runs that end on the same settings are
  two lines. The page groups lines with equal settings and shows the best score of the group and
  how many runs found it.
- **Sorted by the score of the final test,** because that is what was asked for. The page says
  next to it what the profile was after: Quiet and Cool & efficient give score away on purpose,
  and a list by score alone would call them the worst.
- **A tune from the history is not applied as it is.** It becomes the saved tune only through the
  five-minute test of `core/retest.*`, on the driver that is installed now. A line from another
  card (the UUID differs) cannot be chosen.
- **Runs from before this feature are not in it.** The profile that is saved when the feature
  first runs becomes the first line, marked as taken over, without measurements.
- **Bounded:** the newest 500 lines are kept.

**Not decided.** Whether a line can be deleted from the page, and whether the page gets a chart.

## 2. Stability: driver resets in normal use, per tune

**What the user gets.** Next to each tune: how long it has been applied in normal use and how
often the driver reset in that time, as a fact ("2 resets in 310 hours"), with the date of the
last one.

**Choices.**

- **Counted from Windows' own log, not only by the tray app.** A driver reset is one `nvlddmkm`
  event 14 in the System log (hardware check 84). The log also has the resets that happened while
  the app was not running, and it cannot miss one because the app was busy.
- **Attributed through a log of what was applied when.** The app appends a line whenever what is
  on the card changes: a tune applied, stock, the app's own run started or ended. A reset counts
  for the tune that was applied at that moment.
- **Left out:** resets during the app's own runs and tests (they are made on purpose), the reset a
  driver installation makes (the version changes within the same minute), and time during which
  nothing is known about what was applied.
- **Stock is counted too.** Resets with nothing applied are the comparison: a card or a driver
  that resets at stock says nothing about a tune.
- **Shown as a number, never as a verdict.** A reset can be the tune's, the driver's, a game's or
  the monitor cable's. The app does not call a tune unstable, does not lower it and does not take
  it off by this count. What it already does stays: four resets within an hour put the card back
  at stock (the watchdog).
- **Hours are hours the tune was applied with the PC awake,** from the same log; sleep and
  hibernation do not count.

**Not decided.** From how many hours a number is shown at all (a tune applied for ten minutes has
"0 resets" and that means nothing), and whether resets under load and idle are told apart.

## 3. Sharing: tunes of the same card, opt-in

**What the user gets.** With the switch on, the app sends the tunes it found to a shared list. A
user with the same card sees the tunes most often found for it, picks one, and the app tests it on
their card for five minutes before it is applied.

**Choices.**

- **Off unless switched on,** and the page shows the exact data before the first upload. No
  account.
- **What is sent:** the card's model as the driver names it, the board's vendor and model, the
  amount and maker of the memory where the driver reports it, the driver version, the tune, the two
  measurements, the stability count of step 2, and the app's version. **Never sent:** the card's
  UUID or serial number, the user's or the PC's name, anything else on the PC. The server stores no
  IP address.
- **"The same card" is model and board,** not only the model: two RTX 4070 boards differ in
  cooler and power limit. Where too few share the board, the model's list is shown and says so.
- **A shared tune is a place to start, never something to trust.** Every chip is different. A
  tune from the list goes through the five-minute test like one from the history, with the journal
  and the reset handling of a run; one that does not hold ends at stock. Nothing a stranger sent is
  applied untested, so a bad or a malicious entry costs five minutes and nothing else.
- **Values outside what the card reports as its range are refused before anything is written,** as
  for the saved profile today.
- **Ranked by how many cards a tune held on,** not by its score: the highest score is the tune
  that worked on one lucky chip.
- **After code signing.** A program that runs elevated, is not signed and sends data is what a
  virus scanner is suspicious of; the first two steps do not have that problem, this one does.

**Not decided, and the owner's to decide.** Who runs the server and pays for it; where it runs;
what happens to the data when the project stops; whether the list is public as a file anyone can
read; how abuse is limited without accounts. These come before any code.
