# Z-Axis Stepper Wiring (Nano-Owned)

The Z stepper driver moves from the Mega's CNC Shield to the Nano's own prototype board, using
the same **A4988 stepstick** module used for every other axis on this machine. Keeping the same
driver family and microstep setting matters here: `STEPS_PER_MM_Z = 400.0f` in the Mega firmware
(`200 steps/rev * 16 microsteps / 8mm lead`) already assumes 1/16 microstepping, so the Nano-side
Z axis needs to be configured the same way.

## Nano Pin Assignment

| Signal | Nano Pin | Notes |
|---|---|---|
| `PIN_Z_STEP` | D2 | Step pulse output |
| `PIN_Z_DIR` | D5 | Direction output |
| `PIN_Z_EN` | D6 | Driver enable output (active-LOW — confirm against your specific A4988 breakout's silkscreen) |
| *(spare)* | D4 | Freed by moving `PIN_Y_LIMIT` to the Mega; unused for now |

## A4988 Stepstick Pinout

```
                 ┌─────────────────────────────┐
   Motor side →  │  2B    2A    1A    1B        │
                 │  ○     ○     ○     ○         │
                 │                               │
                 │         A4988                 │
                 │        (module)                │
                 │                               │
                 │  ○     ○     ○     ○         │
                 │ VMOT  GND  <trim pot>          │
                 └─────────────────────────────┘
                 ┌─────────────────────────────┐
   Logic side →  │ ENABLE MS1 MS2 MS3 RESET     │
                 │  ○     ○    ○    ○    ○      │
                 │ SLEEP STEP  DIR  GND  VDD     │
                 │  ○     ○    ○    ○    ○      │
                 └─────────────────────────────┘
```

## Connections

| A4988 Pin | Connects To | Notes |
|---|---|---|
| `VDD` | Nano `5V` | Logic supply |
| `GND` (logic side) | Nano `GND` | Common ground — required even though motor power is separate |
| `STEP` | Nano `D2` | |
| `DIR` | Nano `D5` | |
| `ENABLE` | Nano `D6` | Active-LOW: Nano drives LOW to enable the driver, HIGH to de-energize |
| `RESET` + `SLEEP` | Jumpered together, tied to `VDD` | Keeps the driver out of reset/sleep permanently — same convention as the X/Y stepsticks on the CNC Shield |
| `MS1`, `MS2`, `MS3` | All tied to `VDD` (5V) | Selects 1/16 microstepping, matching `STEPS_PER_MM_Z = 400.0f` |
| `VMOT` | `+24V` direct from the PSU | Separate from Nano's 5V logic rail |
| `GND` (power side) | PSU `GND` | Common with logic `GND` |
| `1A` / `1B`, `2A` / `2B` | NEMA 17 stepper coil pairs | Use a multimeter/motor datasheet to confirm which two wire pairs are each coil if unlabeled |

**Decoupling:** place a 100–220µF electrolytic capacitor across `VMOT`/`GND` as close to the
stepstick as physically possible. This is a standard A4988 application-note requirement —
missing/undersized bulk capacitance is a common cause of driver damage on power-up/power-down
voltage spikes.

## Safety Notes

- Common ground between the Nano's 5V logic supply and the 24V motor rail is mandatory, even
  though they're electrically separate supplies — without it the driver's logic-level STEP/DIR/EN
  signals have no reliable reference.
- Confirm `ENABLE` polarity against your specific board before wiring — most A4988 breakouts are
  active-LOW, but double-check the silkscreen/datasheet.
- Set the driver's current-limit trim pot (`Vref`) for the actual Z motor's rated current *before*
  first power-up, per the standard A4988 `Vref = Imax * 8 * Rsense` procedure — running it
  unset/too-high risks overheating the driver or motor.
- Confirm the 4 motor wires are correctly paired into two coils (not one wire from each coil)
  before connecting — an incorrectly paired connection can damage the driver on first step.
