# First-Pass PCB — BOM & JLCPCB Price Estimate

**Status:** exploratory / pricing prep. No schematic drawn yet. The point of this
doc is to get a *real JLCPCB number* before investing time in KiCad, and to serve
as the parts list you'd later feed into the schematic.

> ⚠️ **Part numbers below are representative, from memory.** Confirm every LCSC
> number (and its Basic/Extended status) in JLCPCB's parts search before trusting
> a quote — the numbers drift, the *structure* and the math are what's reliable.

---

## 1. What this board is (first-pass spec)

A low-risk "base board" that kills the perfboard's hand-jumpered power problem via
copper planes, and does the minimum else. Deliberately deferred: embedding the
ESP32 chip, the LED level shifter. See [[custom-pcb-direction]] in memory.

- **ESP32-S3: socketed**, not embedded. A DevKitC-style module plugs into female
  headers. (Module keeps the LDO / USB / reset circuit off *our* board = near-zero
  rookie-mistake surface.)
- **Two copper planes** (GND + 5V) — the whole reason to do this.
- Audio amp baked on; everything user-facing on connectors.

Pin map source of truth: [config.h](../firmware/main/config.h).
Current LED count: **81 px** (45 ring + 36 face).

---

## 2. BOM, split by who solders what

### Group A — JLCPCB places (SMD, this is the "PCBA" cost)

Part numbers verified via web 2026-07-24 (see §6a). Confirm the **Basic/Extended
badge + live stock** on each JLCPCB part page before ordering.

| Ref | Part | LCSC # | Pkg | Basic/Ext | ~$ ea | Notes |
|-----|------|--------|-----|-----------|-------|-------|
| U1  | MAX98357AETE+T (I2S DAC+amp) | **C910544** | TQFN-16-EP(3×3) | **Extended** (confirm) | ~1.20 | *Only* comes in QFN → the reason to PCBA rev A |
| J-USB | USB-C recept. (Korean Hroparts TYPE-C-31-M-12) | **C165948** | SMD | **Basic** ✅ | ~0.06 | Power + native flash; Basic = no feeder fee |
| J-QW | JST-SH 1.0mm 4-pin (JST SM04B-SRSS-TB) | **C160404** | SMD | **Extended** (confirm) | ~0.13 | Qwiic bus (encoder + expansion) |
| C1 | 1000µF electrolytic — **pick ≥10V** | C970711 is 6.3V; find a 10–16V | SMD Ø8 | Basic (confirm) | ~0.04 | Bulk at 5V star; 6.3V is too tight on a 5V rail |
| C2–C6 | 0.1µF / 1µF / 10µF | (any Basic) | 0603 | Basic | ~0.02 | Decoupling (amp + module rails) |
| R1 | Gain-set for U1 | (any Basic) | 0603 | Basic | ~0.01 | Sets MAX98357A gain |
| R2,R3 | 5.1k CC resistors | (any Basic) | 0603 | Basic | ~0.01 | USB-C CC1/CC2 |
| R4 | ~330Ω LED data series | (any Basic) | 0603 | Basic | ~0.01 | On WS2812 data out |
| R5 | ~1k LED current-limit | (any Basic) | 0603 | Basic | ~0.01 | For power-on LED |
| D1 | Power-on LED | (any Basic) | 0603 | Basic (confirm) | ~0.02 | "board is alive" |

**Extended parts drive the one-time feeder fee (~$3 each).** Verified: USB-C
(C165948) and the electrolytic are **Basic** (free), so the Extended list is likely
just **U1 + J-QW → ~$6 one-time** — lower than the first guess of $9.

### Group B — you hand-solder (through-hole connectors)

These are cheap, easy by hand, and skipping them from PCBA keeps assembly simple.

| Ref | Part | Qty | Notes |
|-----|------|-----|-------|
| H1,H2 | Female headers for the ESP32 module | 2 strips | Devkit plugs in here |
| J-LED | 3-pin connector (5V/data/GND) | 1 | To the LED strip |
| J-SPK | 2-pin JST-PH | 1 | To speaker |
| J-RC522 | 6-pin header | 1 | Harness to the globe reader |
| J-5V | 2-pin screw terminal (5.08mm) | 1 | **LED power injection** (see §5) |
| J-EXP | 0.1" pin header, spare GPIO + power | 1 | Expansion / future |

### Group C — you buy separately and plug in (NOT in the JLCPCB order)

| Item | ~$ ea | Notes |
|------|-------|-------|
| ESP32-S3 DevKitC-1 module | ~8–12 | The brain; socketed |
| Qwiic rotary encoder (e.g. Adafruit seesaw / SparkFun Qwiic Twist) | ~6 | Volume + push; debounce lives on it |
| Speaker, LED strip, RC522 module | (have) | Existing hardware |

---

## 3. Board size estimate

The DevKitC-1 footprint (~63.5 × 25.4 mm) sets the floor; amp + connectors ring
the edges. Realistic first-pass outline: **~80 × 90 mm, 2-layer**.

**Keep it under 100 × 100 mm** — that's JLCPCB's cheapest price tier. Crossing
100 mm on either axis bumps you to a higher bracket, so 80 × 90 leaves margin.

---

## 4. Cost model (5 boards, 2026 ballpark)

### Path 1 — Bare board only (you hand-solder *everything*)
| Line | ~$ |
|------|----|
| 5× bare PCB, 2-layer ≤100mm | 4–7 |
| Shipping (Global Standard) | 6–20 |
| **JLCPCB total** | **~$10–27** |

Catch: **the MAX98357A is TQFN — brutal to hand-solder** without hot air. This path
really only works if you reflow the amp yourself (skillet/hot-air) or drop back to
an amp *module* (a daughter board again).

### Path 2 — Bare board + PCBA the SMD side (recommended)
| Line | ~$ |
|------|----|
| 5× bare PCB | 4–7 |
| PCBA setup fee | ~8 |
| Extended-part feeders (~3 × $3) | ~9 |
| SMD components (~$2.25/bd × 5) | ~11 |
| Assembly labor | ~3–5 |
| Shipping | 6–20 |
| **JLCPCB total (5 boards, SMD populated)** | **~$41–60** → ~$8–12/board |

### Then add per working unit (bought separately, any path)
| Item | ~$ |
|------|----|
| DevKitC module | ~10 |
| Qwiic encoder | ~6 |
| Hand-solder connectors (headers, JSTs, screw term) | ~3 |
| **Per-unit adder** | **~$19** |

### All-in, per *complete working* unit
- **Qty 5, Path 2:** ~$10 (board+SMD) + ~$19 (modules) ≈ **~$29/unit** (~$145 for five)
- **Qty 1:** the ~$17 fixed fees have nothing to amortize → **~$60–70 for one** unit
- **Qty 10:** fixed fees spread out → **~$24/unit**

The shape from earlier holds: **fixed fees dominate at low qty**, so one-off is
pricey per unit; a small batch (5–10) is where it feels reasonable.

---

## 5. The LED-power line item (don't skip)

81 px × 60 mA = **~4.9 A** at full white. A plain USB-C port (no PD) gives ~5V at
0.9–3 A. So VBUS alone can't drive full brightness → **J-5V screw terminal** feeds
the strip from a dedicated 5V brick, and/or firmware caps brightness
(`LED_BRIGHTNESS` is 150/255 today, which already helps). The board needs a **fat
5V pour** from J-5V to J-LED, with C1 at that junction.

---

## 6. How to get the *exact* quote (no KiCad needed for a ballpark)

1. **Bare board:** JLCPCB → *PCB* → enter **80 × 90 mm, 2 layers, qty 5** → instant price.
2. **Assembly:** you need a **BOM.csv** (part, LCSC #, qty, designator) and a
   **CPL/pick-and-place** file. For a *ballpark* you can hand-type the Group A parts
   into their assembly quote to see feeder + part costs before drawing anything.
3. Confirm each Group A LCSC number and its Basic/Extended flag — that's what moves
   the price.

---

### 6a. Verified part links (2026-07-24)

Click each and read the **Basic/Preferred vs Extended** badge + stock + live price
straight from JLCPCB — that's the authoritative source, not this doc.

- MAX98357A → JLCPCB C910544: https://jlcpcb.com/partdetail/978950-MAX98357AETET/C910544
- USB-C (TYPE-C-31-M-12, **Basic**) → C165948: https://jlcpcb.com/partdetail/Korean_HropartsElec-TYPE_C_31_M12/C165948
- JST-SH 4-pin Qwiic (SM04B-SRSS-TB) → C160404: https://www.lcsc.com/product-detail/C160404.html
- 1000µF SMD electrolytic (6.3V ref — find a 10–16V) → C970711: https://jlcpcb.com/partdetail/DMBJ-RVT0J102M0810_1000UF_63V/C970711

## 7. Open items to decide before ordering

- [ ] Exact ESP32-S3 module (DevKitC-1 vs a smaller S3 board) → sets socket size + board outline
- [ ] Which Qwiic encoder (fixes I2C address + firmware library)
- [ ] Bulk cap value/voltage (1000µF vs 470µF, 10V+)
- [ ] LED connector style (JST vs screw vs header)
- [ ] Confirm all Group A LCSC part numbers + Basic/Extended status
