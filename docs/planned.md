# Planned: tune history, stability, sharing

Three features that are decided on but not built, one open design point, the design that answers it, and a proposed design for a fair before and after (#84). This page records the choices made for each, so
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

## Proposed design: a fair before and after (#84)

A proposal, waiting for the owner's approval. Nothing is built, and nothing a run saves changes
until it is approved. It answers #84: the "before" and "after" a run shows are not always measured
under the same conditions, and nothing judges an undervolt against stock at all.

**What is compared today.**

- **Before:** `measure_stock` (`src/app/common_run_steps.cpp`). 300 s (`kLongRunS`) with the fans
  left to the NVIDIA driver. It stops at `kStockTempC` (85 C), and a measurement that stopped there
  after at least `kStockMinS` (30 s) is still the "before". It starts from an idle card.
- **After (Undervolt):** the soak in `find_undervolt` (`kUvSoakS`, 300 s). The fans follow the run's
  curve, moved up with the ceiling (`temp_ceiling`). It stops at that ceiling, and it starts on a
  card that is already warm from the search.
- **The numbers:** score, clock and power are averages over the whole measurement, warm-up
  included. The temperature is the peak, and the fan speed is the last reading.
- **The verdict:** an undervolt is kept when its soak holds. No rule compares it with stock. The
  speed profiles keep no result slower than 99 % of the stock score (`kNotSlowerScore`, hardware
  check 93), measured against that same "before".

Two cards show what that costs:

- **GTX 1080 Ti (rc1, #83):** stock stopped after 119 s at 86 C, on the driver's fans (34 % rising
  to 79 %). The soak ran 300 s on the curve at 89 %. The table read -3.2 % score and +10.7 % power.
  It cannot say whether the undervolt is better or worse.
- **Reference RTX 4070:** the fan control alone moves the numbers. One set of settings drew 176 W
  at 63 C with the driver's fans at about 68 %, and 180 W at 71 C on the tuned curve at 62 %
  (hardware check 76).

### What makes a measurement valid

- **A complete measurement** is Stable for its full length. A measurement at stock that the
  temperature stopped after 30 s or more is **partial**. It is still good for what it gives today:
  the ceiling, and the clock an undervolt keeps. It is not good for the comparison.
- **The result** is valid when its soak passed. That is already required.
- **A comparison** is valid when both sides are complete and ran under the same fan control: both on
  the same curve, or both on the driver when the app cannot set the fans (`gpu.set_fan_pct`
  empty).
- **Not valid:** the before/after lines are replaced by one line, `not compared: <why>`, on screen
  and in `run.log`. The reason names the side that failed and how: for example "the measurement at
  stock stopped at 86 C after 119 s", or "at stock the driver had the fans, after it the app's
  curve".

### How the two sides are made comparable

1. **The measurement at stock follows the run's fan curve,** the curve the run would use without a
   raised ceiling. Only the clocks are at stock. This settles the "which temperature at stock" point
   of the design above in the other direction: the ceiling is computed from the peak on that curve.
   The tester's RTX 3080 Ti, for example, reached 82 to 83 C with its fans at 100 %, and 86 C on the
   driver's fans.
2. **When the ceiling moves the curve** (a card that was hotter at stock than the profile allows),
   the measurement at stock is repeated on the moved curve after the cool-down. That second
   measurement is the "before". It costs five minutes, only on such cards, and only it makes the
   comparison fair there. Without it the before would run on a more aggressive curve than the
   after, and a moved curve's quieter fans would read as a benefit of the undervolt.
3. **The same length:** 300 s on both sides, as today.
4. **The part that counts:** score, clock, power, temperature and fan speed are averaged over the
   last half of each measurement (the last 150 s). By then the card has run under load for minutes,
   on both sides. The peak temperature is still taken over the whole measurement, for the safety
   stops. The cool-down that follows an early stop at stock is not part of either.
5. **Throttling and early ends:** a measurement that the temperature ends early is partial, see
   above, and is never averaged against a complete one. A card that holds its temperature by
   slowing down shows it in the clock and the score of the counted half. That is a real effect of
   the settings, and it is compared as one.

### The keep/discard rule for the Undervolt profile

The rule looks at three things separately, over the counted half: performance (score), power, and
thermals (average temperature together with average fan speed). With a valid comparison:

- **Slower:** a score under 99 % of stock discards the result. This is the share `kNotSlowerScore`
  already uses, and the profile promises the same speed.
- **A thermal regression:** the result is discarded when the card runs more than 2 C warmer and its
  fans are not at least 5 points slower, or when its fans run more than 5 points faster and the card
  is not at least 2 C cooler.
- **No benefit:** the result is discarded unless at least one of these holds:
  - power is at least 3 % lower;
  - the card is at least 2 C cooler;
  - the fans are at least 5 points slower.
- **Otherwise it is kept.** Higher power alone never discards a result, as long as it is cooler or
  quieter for it. Finishing the soak alone never keeps a result either.

A result that is discarded is neither saved nor left on the card. The card goes back to how it was
before the run: stock, or the saved tune. The `RESULT:` line gives the reason with both numbers, as
the slower-than-stock refusal of the speed profiles does today.

**Without a valid comparison** (proposed): the result is discarded, and the line says why. The
principle of #84 is "kept only when it shows a benefit", and an incomplete comparison shows none.
The cost: on a card whose fans cannot be set and which overheats at stock, the Undervolt profile
saves nothing. The other choice is to save the result marked "not compared". That is the owner's
decision.

**Tolerances:** the 1 %, 3 %, 2 C and 5 points above are provisional, set at about twice the
scatter seen so far:

- **Two clean measurements at stock on the reference card,** days apart (checks 76 and 77):
  5728 and 5731 it/s, 196 and 195 W, peak 67 and 67 C, fans 73 and 72 %.
- **Two All in one results** (+1.6 % and +0.9 % score, -8.2 % and -10.8 % power) are no measure of
  scatter: their memory overclocks differed (+1500 and +1100).

Hardware check 1 below measures the scatter properly, and the values are fixed from it.

### Where it applies

- **The fair "before"** (stock on the run's curve, repeated on a moved curve, the counted half) is
  the "before" of every run. The speed profiles keep their own rule, `kNotSlowerScore`, which then
  compares on equal fans.
- **The keep/discard rule** above is for the Undervolt profile.
- **On top of the saved tune** (`gao --undervolt` on a tune, the undervolt step of All in one), the
  reference is that overclock's own soak. Both sides are soaks on the run's curve, so the rule
  applies as it is. Whether All in one is judged against stock as a whole
  is still the open point of the design above.

### What changes per card

- **A card that is cool at stock** (the reference RTX 4070, 67 C): the measurement at stock runs on
  the Undervolt curve instead of the driver's fans. Going by check 76, it reads a few degrees warmer
  at lower fan speeds than today. There is no second measurement and no extra time. An undervolt
  there drew 11 % less power at a higher clock (check 75, step 3: 172 W against 194 W), so it is
  kept for its power.
- **A card that is hot at stock** (this GTX 1080 Ti, the RTX 3080 Ti of check 97): the first
  measurement at stock runs on the profile's curve. That curve reaches 100 % at 75 C, so it is loud
  for five minutes, and it is expected to complete instead of stopping at 85 C. The ceiling comes
  from its peak, capped at the card's target as today. The curve moves up, and stock is measured
  again on the moved curve: about five minutes more for the run, which then compares like for like.

### How it would be built (for the PR that follows approval)

- `src/core/`:
  - the averages over the counted half in `run_stability`, or next to it, from the readings it
    already takes;
  - one pure function that judges a result against stock and returns keep or discard, with the
    reason;
  - unit tests with the 1080 Ti and RTX 4070 numbers above.
- `src/app/`:
  - `measure_stock` drives the fans with the run's curve, through the `FanDriver` the search already
    uses;
  - the second measurement when the curve moves;
  - the `not compared:` line and the reasons for a discard in `result_lines`.
- Nothing about the ceiling, the stops or the journal is weakened. The measurement at stock still
  stops at `kStockTempC`.

### Hardware checks that would prove it

1. **Scatter, reference card:**
   - three measurements at stock in a row on the run's curve;
   - three soaks of one saved undervolt;
   - the spread of the score, the power, the average temperature and the average fan speed over the
     counted half.

   The tolerances are set from this. About 45 minutes, nothing applied beyond a saved tune.
2. **An Undervolt run on the reference card:**
   - the measurement at stock runs on the curve and completes;
   - there is no second measurement;
   - the comparison is valid, `RESULT:` keeps the result, and `run.log` gives the counted-half
     numbers of both sides.
3. **An Undervolt run on a card that is hot at stock** (a tester's GTX 1080 Ti or RTX 3080 Ti):
   - the first measurement completes on the curve;
   - the second measurement runs on the moved curve and is the "before";
   - the verdict is given with its reasons.
4. **All in one on the reference card:** the speed check (`kNotSlowerScore`) still passes against the
   new "before".
5. **The discard path and `not compared:`** are proven by unit tests. No card is known that gives
   them on demand.

### Decisions for the owner

1. **Stock on the run's curve, with a second measurement on a moved curve** (proposed). The
   alternatives:
   - **Always two measurements:** one on the driver's fans as the user's own reference, one on the
     curve. That is five minutes more on every card.
   - **Keep the driver's fans at stock:** the difference is only stated, and power and temperature
     are never judged.
2. **No valid comparison:** discard (proposed), or keep it marked "not compared".
3. **The counted half:** the last 150 s (proposed), or a fixed warm-up to skip.
4. **The tolerances:** provisional, until check 1.
5. **The screen's "Before" heading:** it would say the fans followed the app's curve.

### Against the acceptance criteria of #84

- **A valid stock measurement and a valid result:** "What makes a measurement valid", including the
  `not compared:` line.
- **The before/after line under the same conditions:** "How the two sides are made comparable".
- **The keep/discard rule over performance, power and thermals, with tolerances:** "The
  keep/discard rule". The tolerances are provisional and fixed by check 1.
- **A cool card and a hot card:** "What changes per card".
- **The hardware checks:** "Hardware checks that would prove it".
