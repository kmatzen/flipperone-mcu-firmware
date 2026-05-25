# Ordering a FUSB302 breakout (ReclaimerLabs USB-PD-Breakout)

The off-the-shelf FUSB302 breakouts (WaffleBomb, Reclaimer Labs) are frequently
out of stock. The most reliable way to get a board that drops straight into this
rig is to fab the open-source **ReclaimerLabs USB-PD-Breakout** — it uses the
**FUSB302BMPX (I²C address 0x22)**, which matches the firmware/rig default, and
its J4 header exposes SDA/SCL/INT/VDD/GND with CC/VBUS on the on-board USB-C jack.

- Repo: https://github.com/ReclaimerLabs/USB-PD-Breakout (use the latest revision)
- It contains: `electrical/gerber/` (gerbers), KiCad source (`.kicad_pcb`/`.sch`/
  `.pro`, an older KiCad format), a schematic PDF, and an `.xls` BOM.
- It does **not** ship a JLCPCB-ready BOM.csv or placement/CPL file — see Path B.

First, if you just want a board fast: **check distributor stock for the official
ON Semi `FUSB302BGEVB` eval board** (Mouser / Digi-Key / Newark). If one has it,
skip all of the below.

---

## Path A — bare PCB + hand assembly (simplest order, hardest build)

Best if you have hot air / a hotplate (the FUSB302 is a leadless MLP-14).

1. Download the repo; zip the **contents** of `electrical/gerber/` (the `.gbr`/
   `.drl` files, not the folder).
2. jlcpcb.com → **Add gerber file** → upload the zip. Defaults are fine
   (2-layer, 1.6 mm, HASL). ~$2 for 5 boards.
3. Order parts from LCSC/Mouser/Digi-Key using the repo's BOM (`electrical/
   USB-PD-Breakout_BOM.xlsx`): the **FUSB302BMPX**, a USB-C receptacle, the 0402
   passives, and the J4 header.
4. Hand-solder. Reflow the FUSB302 + USB-C with hot air/stencil; the rest is
   easy 0402 work.

## Path B — full PCBA (easiest build, more order prep)

JLCPCB needs three things: **gerbers**, a **BOM.csv** (designator + LCSC part
number), and a **CPL/placement.csv** (designator, x, y, rotation, layer). The
repo provides gerbers but not the CSVs, so generate them:

1. Open `electrical/USB-PD-Breakout.*` in a current **KiCad** (it will migrate the
   old project). 
2. Easiest: install the **"Fabrication Toolkit"** KiCad plugin (a.k.a. JLCPCB
   tools) and run it — it exports gerbers + BOM + CPL in JLCPCB's format in one
   click. (Manual alternative: PCB Editor → *Fabrication Outputs → Component
   Placement (.pos, CSV, mm, separate files)* for the CPL, and export a BOM CSV.)
3. Fill in the **LCSC part number** for each line in the BOM (search LCSC for each
   part — most importantly the **FUSB302BMPX** and the USB-C connector; 0402
   R/C usually auto-match). Leave designators you'll hand-fit (e.g. the J4
   header) off the assembly BOM.
4. jlcpcb.com → upload gerbers → enable **PCB Assembly** → upload the BOM.csv and
   CPL.csv → match any unmatched parts in the web UI.
5. **Check part rotations** in the JLCPCB placement preview — wrong rotation on
   the FUSB302 or USB-C connector is the #1 PCBA failure. Compare against the
   `.step`/PDF in the repo.
6. Order. Min qty is typically 2 (PCB) / 2–5 (assembled). Expect roughly
   $2 PCB + ~$8 assembly setup + parts, ~1 week + shipping.

---

## After it arrives
Wire it to the Pico 2 per `README.md` (§ "ReclaimerLabs USB-PD-Breakout"). No
firmware change is needed — it's FUSB302BMPX at 0x22, the rig's default.
