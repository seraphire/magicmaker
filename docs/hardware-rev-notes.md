# Hardware Rev Notes — Design-for-Assembly Checklist

**Status:** lessons captured from building the **first physical unit** (the gift
build). This is the "don't repeat these" list for the custom PCB + enclosure rev.

> These are real scars, not hypotheticals: a broken end LED, snapped standoff
> posts (one board holding on a single surviving screw), a cracked frame, and a
> USB-C port the shell won't clear. Design them out.

Related: [pcb-first-pass-bom.md](pcb-first-pass-bom.md), and `custom-pcb-direction`
+ `build-assembly-lessons` in memory.

---

## 1. Strain relief — the #1 killer

Every off-board wire on the first unit failed or nearly failed at its termination
because a connection dangled with no support.

- **End LED tore off the strip** — the feed wire hung unsupported and levered the
  last pixel's pad off. (This is *why* `RING_LED_COUNT` is 45 not 46 — see
  [config.h](../firmware/main/config.h): "first pixel died and was cut off.")
- **Audio wire** hot-glued around a post is still weak.
- **RC522 harness** was fought into place under tension.

**Design in, for the rev:**
- Use **connectors, not soldered pigtails**, for LED strip, speaker, and the
  RC522 harness → a tug pulls a connector, not a pad. (Already the plan in the
  BOM — reinforce it.)
- Add **board mounting holes / slots for a zip-tie or adhesive tie-down** right
  next to every off-board connector, so the cable is anchored before it reaches
  the joint.
- Enclosure: give each cable a **service loop** and a physical anchor; nothing
  load-bearing at a solder joint or a pixel pad.

## 2. Wire length — make them LONGER than you think

- Too-short RC522 wires forced the board in **upside-down** to reach → watch
  board orientation, and spec harness length with slack.
- Too-short LED wire contributed to the broken end pixel.
- **Rule:** measure the real routed path in the enclosure, then add margin. A
  service loop is free; a reworked harness is not.

## 3. Standoffs / mounting posts

- **Round the standoff corners** to clear the frame — square corners caused the
  frame to pull off the face mount.
- **Don't spec posts too thin** — screws split the lower board's posts (one unit
  survives on a single screw). Give post walls real margin, and match
  screw/boss sizing.
- Verify screw **length** doesn't bottom out or over-torque into a thin boss.

## 4. Audio out — external speaker is the design, not a workaround

The first unit ships with a **separate external speaker cube** — this is
**intentional and permanent for this enclosure**: the MakerWorld globe has **no
internal room for a driver and no audio opening in the back**. The speaker box is
part of the gift.

**For the rev, decide up front:**
- Keep the **external-speaker** approach → give the board a clean, strain-relieved
  **speaker-out connector** and route it to a panel jack, **or**
- If a future enclosure has room, design an **internal driver pocket + grille**
  (the front face already has the dot-line grille motif to build on).
- Either way, don't assume internal audio — this shell can't do it.

## 5. USB-C port clearance — enclosure-critical

- The shell **won't clear a normal USB-C plug body** — the connector housing is
  too long. The build is currently stuck using a slim programmable cable because
  it's the only plug that fits.
- **Fix options for the rev (pick one):**
  - Design the port cutout as a **recess/pocket** sized for a real-world plug
    body, not just the connector's nominal width; **or**
  - Use a **panel-mount USB-C pigtail** so shell clearance stops mattering; **or**
  - Confirm a specific slim cable and ship it with the unit (fragile — avoid).

## 6. Controls — add them at design time

- Wanted a **second push-button** on the user-control board; it wasn't there and
  retrofitting is painful. Decide button count **now**.
  - Note: the current PCB direction folds pot+button into a **rotary encoder**
    (rotate = volume, push = program/idle). If a discrete 2nd button is still
    wanted, place it in this rev — see `custom-pcb-direction` in memory.

## 7. Dry-fit EVERYTHING, early

- The frame-pull failure surfaced only because **final fit wasn't tested until
  the end**.
- The **rotary knob and other pieces had to swap positions** because of enclosure
  clearances discovered late.
- **Rule:** do a full mechanical dry-fit of the board + controls + connectors in
  the actual enclosure **before** committing the PCB layout. Let the enclosure
  model drive connector/control placement, not the other way round.

---

## Quick pre-layout checklist

- [ ] Enclosure model locked *before* PCB placement finalized
- [ ] Audio path decided: external speaker-out connector, or internal driver pocket
- [ ] USB-C cutout sized for a real plug body (or panel-mount pigtail chosen)
- [ ] Connector (not solder) for LED / speaker / RC522
- [ ] Tie-down anchor beside every off-board connector
- [ ] Harness lengths measured from routed path **+ slack**
- [ ] Standoff corners cleared; post walls thick enough for the screws
- [ ] Button/encoder count decided and placed
- [ ] Full dry-fit of controls + connectors in the shell
