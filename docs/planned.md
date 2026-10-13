# Planned: tune history, stability, sharing

Three features that are decided on but not built, one open design point, and the design that answers it. This page records the choices made for each, so
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

## Open design point: "stock" on a card that is not run at stock

Not a feature and not decided: a finding from the first card other than the reference card, kept
here so that it is not lost. Nothing is built for it.

**What was seen** (an RTX 3080 Ti Founders Edition with replaced thermal pads, driver 610.88,
version 0.4.3):

- Its owner runs it day to day at a power limit of 70 % (245 W of 350 W), set in MSI Afterburner.
- Every run starts by measuring the card at stock, and stock is the power limit at 100 %. There the
  card drew 290 W and reached 86 C after 148 s, with the NVIDIA driver holding the fans at 66 %.
- With the fans at 100 % it still ran at 82 to 83 C at about 280 W.
- To get under the profile's 75 C the search lowered the power limit to 65 %: 4547 it/s, where the
  30 s baseline at stock scored 4818 and the measurement at stock 5013, with the fans at 86 %
  instead of 66 %. A "Best of my GPU" result that is slower and louder than stock.

**What that puts in question.**

- **The "before" is a state this owner never uses.** The result compares with stock at 100 %
  power; what they would want to know is how it compares with what the card ran when they pressed
  the button. The app reads those settings at the start ("Set by another program") and does
  nothing with them.
- **Stock is hotter and louder than what the card is used at,** so the first five minutes of every
  run are the worst the card has seen in a long time. Whether a run should start from the settings
  it found, or from stock, is open; starting from settings another program made means trusting
  them.
- **A fixed temperature ceiling does not fit a card that is over it at stock.** 75 C is a choice
  that suits the reference card. On this card it can only be met by giving up speed, which is the
  opposite of what the profile promises. A ceiling relative to the card's own measurement at
  stock ("never hotter than it was") is one answer; refusing to save a result that is slower than
  stock, and saying why, is another. They do not exclude each other.

**Not decided.** All of the above. It is recorded before a second card of this kind is tested,
not after.

## Design: never hotter than at stock

The answer proposed to the open point above. A design, not built for the profiles that promise
speed. What is built already is the backstop next to it (a profile that promises speed keeps no
result that is slower than stock) and this same rule for the undervolt profile, where it costs
nothing: an undervolt has no power limit to lower and only makes a card cooler. For that profile
the fan curve moves with the ceiling as well, as described below, since the first undervolt on the
tester's card held it at 74 C with the fans at 93 %, where it had run 86 C at 66 % at stock
(`docs/development.md`, hardware check 97).

**The rule.** A run's temperature ceiling is the profile's limit, or what the card reached in the
measurement at stock when that is more, and never more than 85 C:

    ceiling = max(profile limit, min(peak at stock, top))

where `top` is the card's own temperature target when it reports one, and 85 C when it does not or
when its target is higher than that (`temp_ceiling`). The target is the temperature from which a
card slows itself down instead of running its fans faster; NVML gives it without administrator
rights (`nvmlDeviceGetTemperatureThreshold`, the "acoustic" threshold in force: 84 C on the
reference card). A run has no business above it, and a fan curve that reaches 100 % there keeps
the card from getting to it. That this threshold is what MSI Afterburner shows and sets as "temp
limit" is assumed from its range (65 to 88 C on the reference card, in both) and **not confirmed**:
it takes `nvidia-smi -q -d TEMPERATURE` on a card with that limit set, with Afterburner and
without.

On the reference card nothing changes: it reaches 67 C at stock, and every profile's limit is above
that. On the tester's RTX 3080 Ti (86 C at stock) Best of my GPU would run with a ceiling of 85 C
instead of 75. The power step then has no reason to go to 65 %, and the clocks are searched at the
power the card has.

**Where the ceiling is used.** Everywhere the profile's limit is used today: the power step (the
highest power limit whose probe stays under it), the clock probes, the soak, and the fan
controller's "100 % from here". The stock measurement and the health probes keep their fixed 85 C.

**The fan curve moves with it.** A profile's curve is drawn against its limit: Normal reaches
100 % at 75 C because 75 is where that profile stops. With a higher ceiling and the same curve the
fans would run at 100 % for good, which is not a tune anybody keeps. So the curve is shifted by the
same amount the ceiling went up: the fan-stop threshold and the first point stay where they are
(the card idles as before), every point above them moves up by the difference, and the last one
sits on the ceiling.

    Normal, as it is:        50 C 35 %   60 C 45 %   70 C 65 %   75 C 100 %
    Normal, ceiling 85 C:    50 C 35 %   70 C 45 %   80 C 65 %   85 C 100 %

The fan tune of All in one aims 5 C under the ceiling, as it aims 5 C under the limit today.

**What it costs, said plainly.**

- **The card stays as warm as it was.** On the tester's card that is 83 to 85 C under load for as
  long as the tune is applied, where the run that was built gave 73 C. The rule does not cool a
  card; it stops the app from paying for coolness with speed in a profile that promises speed.
- **It is not quieter either.** That card needs its fans near 100 % to stay under 85 C at full
  power; the driver's own control held 66 % and accepted 86 C. Faster than stock, as warm, and
  louder: that is what Best of my GPU can be on such a card. Cool, quiet and fast at once it cannot
  be, and the app should say which of the three a profile gives up.
- **The app sees one temperature.** NVML reports the core. The memory and the voltage regulators
  of a card that runs at 85 C are hotter than that, and GDDR6X more than most. A higher ceiling
  with a memory overclock on top heats exactly what cannot be measured.
- **Speed:** expected to end above stock on that card. Not measured.

**What the user sees.** One line when the measurement at stock ends, before anything is searched:
that this card reached N C by itself, that the profile allows 75 C, and that on this card it
allows N. And the result table, which already shows both temperatures and both fan speeds.

**Rejected.**

- **Asking the user to choose a limit.** A question in the middle of a one-click run, about a
  number most people cannot judge.
- **Keeping the fixed limits and relying on the backstop.** Honest, but then the speed profiles do
  nothing at all for such a card.
- **Raising the limits for every card.** Hotter cards for everybody, to help the few that are hot
  already.

**Not decided.**

- **Quiet and Cool & efficient on such a card.** Their limits (80 and 65 C) cannot be met there
  either, and they would lower the power limit a long way to try. They promise less heat, so a
  higher ceiling is the wrong answer for them; saying "this card cannot be kept at 65 C" may be the
  right one.
- **Which temperature "at stock" is.** The measurement at stock leaves the fans to the driver
  (86 C at 66 %). With the fans on the run's curve the same card reaches 83 C. The rule above takes
  the first, capped at 85.
- **Where the measurement at stock stops.** Today at a fixed 85 C (`kStockTempC`), whatever the
  card says about itself. On the tester's card that ends it after 89 to 163 seconds of the five
  minutes, so the "before" of every result is a card that was still warming up, and the clock a
  run keeps is the average of that stretch. The card's own thresholds are readable now: its target
  (where it starts to slow down), its maximum operating temperature (90 C on the reference card),
  and the points where it slows down hard and shuts down (94 and 99 C there). Stopping at the
  target would end the measurement earlier still on a card that sits above it; stopping at the
  maximum operating temperature would let it run its five minutes, on a card whose memory and
  voltage regulators are hotter than the one temperature the app can read. Not changed, and not
  decided.
- **A card whose target is below a profile's own limit** (an owner who set 70 C, a profile that
  allows 75). The profile's limit is kept as it is today, and the card throttles below it by
  itself. Lowering the profile's limit to the card's target would respect the owner's choice, and
  change what the profiles do on such a card.
- **A memory overclock under a raised ceiling:** allowed as today, or left at stock because of
  what cannot be measured.
- **Whether All in one is judged as a whole** against stock, as the two speed profiles are.
