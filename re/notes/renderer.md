# SkyRoads renderer & HUD — reverse-engineered specification

Source: SKYROADS.EXE disassembly (re/full.asm, load image re/image.bin, DS data at
file offset 0x66E0). All `[0xNNNN]` are DS-relative (DS==SS, Borland small model).
Claims are backed by instruction addresses; data tables were extracted from
image.bin and from TREKDAT.LZS (decoded with tools/lzs.py + the in-place
expansion documented in §4). Items marked **TBD** are unverified.

Function name corrections vs re/MAP.md:
- **0x2cb2 is NOT the intro sequence** — it is `render_init` (sets renderer
  function pointers, runs TREKDAT second-stage expansion; 0x2cb7-0x2cee).
- **fn_2d03 is not just "draw ship"** — it is `render_frame`, the whole-frame
  3D-viewport renderer (composer + ship + blit).

---

## 1. Buffers and big picture

Mode 13h (VGA) layout: viewport = screen rows 0..137 (320x138), dashboard
rows 129..199 (the dashboard PICT starts at y=129; its top 9 rows form a
trapezoid overlapping the viewport, see §8.3).

| buffer | seg var | size | contents |
|---|---|---|---|
| pristine background | `[0x517e]` | 320x200 | WORLD backdrop (rows 0..137) + dashboard composited at rows 129..199. Allocated by load_world fn_536b: the decoded 320x138 backdrop PICT buffer itself is grown to 0xFA00 bytes (fn_5c57 @0x53ca), dashboard rows memset 0 (0x53e5), then the DASHBRD PICT is composited with 0-transparency over rows 129..199 (loop 0x541f..0x543a calling fn_4184 with dst 0xA140+y*320). |
| back buffer | `[0x5486]` (alias `[0xe42]` inside renderer) | 0xAC80 = 320x138 | composed frame. Allocated in main: `0x2fa push 0xac80 / 0x2ff call 0x3ed8 / 0x305 mov [0x5486],ax`. |
| screen | A000 | | VGA. |
| staging tile | `[0xaf3a]` | small | HUD shape staging; `[0xaf3a]` = normalized segment of ds:0x31b4 (the 4 KB file buffer), set at 0x2b27-0x2b34. |

EGA (mode 0Dh): two hardware pages A000/A200 (plane size 0x1F40), pristine
copy at fixed segment A400 (`0x545b mov word [0x517e],0xa400`), page flip via
int 10h ah=5 with page `[0x9340]` (fn_0e23 @0xe33-0xe38). EGA redraws the full
viewport every frame; VGA uses incremental dirty-region updates (§6).

Per-frame pipeline (one 36 Hz tick), gameplay loop fn_1f2c:

```
0x224c push [bp-0x10]; 0x224f call 0xbe3   ; render viewport+ship (fn_0be3)
0x2255 call 0x124b                          ; update dashboard HUD (fn_124b)
0x225b wait until [0x160c] (36Hz tick) changes
0x2267 call 0xe23                           ; EGA: flip page (VGA: no-op)
```

---

## 2. fn_0be3 — per-frame render orchestrator

`fn_0be3(on_sticky_flag)` gathers everything and calls render_frame fn_2d03.
The argument is fn_1f2c's "standing on an effect-2 (sticky) tile" flag
(computed at 0x238c-0x23a3); it makes the ship render 1 px lower (squat).

Ship state variables used (set by fn_1f2c init 0x1f72-0x1f8a):
- `[0x9628:0x962a]` 32-bit road progress Z, **0x10000 per grid row** (init 0x00030000 = row 3).
- `[0xaf2c]` lateral X (init 0x8000 = center); world tile width = 46*128 X-units
  (grid sampler fn_04c0: col = (X/128 - 95)/46, 0x4d2-0x519).
- `[0xaf3c]` altitude Y; floor top = 0x2800 (init 0x1f84). Screen altitude = Y/128.
- `[0x9342]` vertical velocity; `[0x54b8:0x54ba]` 32-bit forward speed.

What fn_0be3 computes:
1. `inside_tunnel = fn_0533(Z, X, Y)` (0xbf9). fn_0533 returns 1 when the tile
   shape is a tunnel (`& 0xf00` == 0x100/0x300/0x500, 0x54e-0x567) and the ship
   is under the arch: |dist to tunnel centerline| ≤ 37 (in X/128 units) and
   `(Y-0x2200)/128 < (inner[d]+outer[d])/2` with arch profile tables
   **ds:0x46** and **ds:0x92** (38 words each; 0x5ad-0x5db).
   Profiles (units of Y/128 above 0x2200): inner = 16,16,16,16,15,14,13,11,8,...,0(center),...; outer = 32,...,14.
2. Explosion frame: if `[0x4578]` (explosion counter) != 0: sprite index
   `si = [0x4578]/3`, ≥14 → si=-1 i.e. done (0xc0c-0xc20). **Explosion = CARS frames 0..13, 3 ticks/frame.**
3. Ship sprite index (0xc52-0xc93):
   `pitch b = inside_tunnel ? 0 : fn_0afa([0x9342], [0xaf3c])` — fn_0afa (0xafa):
   returns 2 if vy ≤ -355 or Y < 0x2800 (nose down), 1 if vy ≥ +355 (nose up), else 0.
   `tilt a = fn_0b34([0xaf2c])` — (0xb34): `clamp((X/128 - 95)/46, 0, 6)` = 7 tilt
   classes, 3 = centered.
   `anim = ([0x457c]==4) ? 0 : word[0xea + (([0x160c]/2)%4)*2]` — note the jz
   sense at 0xc2b: the animation runs **always** (suppressed only in the
   unreachable `[0x457c]==4` state). Table ds:0xea = {0,1,2,1}, stepping every
   2 ticks (18 Hz) — this is the constantly-cycling engine-flame animation,
   and it is what the 3 consecutive frames per orientation are for.
   **`sprite = ((a*3 + b)*3 + 14) + anim`** (0xc7c-0xc93), i.e. frame = 14 + 9*tilt + 3*pitch + anim(0..2).
   CARS frames: 0..13 explosion, 14..74 ship = 7 tilts x 3 pitches x 3 flame
   subframes (frames 75,76 unused **TBD**). Byte offset = frame*0x2d0 (720 = 24x30
   PICT cells), added to CARS far ptr `[0xaf44]:[0xaf46]` (0xda9-0xdb6).
4. Ground under ship for shadow: sampled at the ship's row, slightly left and
   right of center — `g = max(fn_0b71(Z, X-0x380, inside_tunnel),
   fn_0b71(Z, X+0x380, inside_tunnel))` (note `0xc98/0xcb3 mov ax,[0xaf2c];
   add ax,±0x380` — X offsets of 7 screen px), clearance = (Y - g)/0x80
   (0xc95-0xcc8, 0xd81-0xda4); exploding → 0x7fff (no shadow, 0xd7b). fn_0b71 (0xb71): samples grid via
   fn_04c0; returns 0x2800 for plain floor, 0 for hole; for shapes 2..5 returns
   block-top height from table **ds:0xde** = {10240, 12800, 12800, 12800, 15360,
   15360, 0, 1}, i.e. floor top 0x2800, low-block/tunnel top 0x3200, high-block
   top 0x3C00; shape 1 (plain tunnel) returns 0 unless `inside_tunnel` forces the
   flat-floor path (0xb97) so the shadow falls on the floor inside tunnels.
5. Render-skip cache: if (X32, Z, Y, sprite, page) all equal cache `[0xe28..0xe32]`,
   skip rendering entirely (0xccb-0xd0e).
6. Calls fn_2d03 (0xe0e) with 8 word args, then `[0x9340] ^= 1`, `[0x4570] = 0`
   (0xe14-0xe19).

fn_2d03 argument list (from 0x2d0b-0x2d38 and call sites 0xd70-0xe0d, 0x2c26-0x2c2f):

| arg | stored at | meaning |
|---|---|---|
| [bp+4] | [0xe34] | ship screen-x anchor = X/0x80 + word[0x38 + tilt*2]; pan table ds:0x38 = {-1,-1,-1,0,1,2,4} (sprite lean nudge). Sprite rect left = [0xe34]-0x6e. |
| [bp+6] | [0xe36] | scroll position = Z/0x2000 (fn_5d4e 32-bit divide @0xde5), i.e. **8 sub-steps (phases) per grid row** |
| [bp+8] | [0xe38] | ship screen altitude = `[0xe2e]`/0x80 (cached Y; minus 0x80 if the sticky-tile arg is set, 0xd25-0xd3a); sprite top y = 0x9d - alt |
| [bp+0xa/0xc] | [0xe3a/0xe3c] | far ptr to CARS sprite cell |
| [bp+0xe] | [0xe3e] | full-redraw flag (= `[0x4570]`, set only at road start; mid-game always 0 — full redraws also trigger when scroll delta ≥ 8, §6) |
| [bp+0x10] | [0xe40] | ground clearance for shadow |
| [bp+0x12] | [0xe42] | destination "back buffer" segment: VGA `[0x5486]`; EGA A000/A200 page (0xd47-0xd6d) |

---

## 3. fn_2d03 — render_frame (composer pass + ship + blit)

Pointers installed once by render_init fn_2cb2:
VGA (0x2cb7-0x2cc9): `[0xe44]=0x348b` (background restore), `[0xe46]=0x3137`
(left-half span fill), `[0xe48]=0x3174` (right-half span fill), `[0xe4a]=0x323f`
(ship draw). EGA (0x2cd6-0x2ce8): 0x3462 / 0x3083 / 0x30d9 / 0x31bf.
fn_2cb2 also runs the TREKDAT expansion fn_3a7a (0x2cb4) and, on EGA, the
span-table planar conversion fn_3a23 (0x2cee).

Flow of fn_2d03:

1. `call [0xe44]` (0x2d3b): VGA = `fn_3492(0)` (restore changed background
   spans from pristine into back buffer, §6); EGA = copy whole pristine plane
   A400→page (0x3462: rep movsb 0x1590 bytes after GC plane setup).
2. `es = [0xe42] + 0x280 paragraphs` (VGA; +0x50 EGA) (0x2d3f-0x2d51) — es:0
   addresses back buffer **+0x2800 bytes = screen row 32**. All road span offsets
   are relative to viewport y=32 (the road never draws above y=32; rows 0..31
   are always backdrop sky).
3. `bp = 0x1638 + (pos>>3)*14 + 0x62` (0x2d53-0x2d63): grid pointer at row
   `(pos>>3)+7`, column 0 (7 u16 per row, 14 bytes). `ds = [0xe82 + (pos&7)*2]`
   (0x2d66-0x2d6f): TREKDAT **phase object** for sub-row phase pos&7.
4. Row loop (0x2d7c-0x2e1e), `[0xe50]` = 11 down to 2 → **10 grid rows are
   processed, far to near**: grid rows pos>>3 +7 ... pos>>3 -2 (7 rows ahead,
   the ship row, 2 rows behind the camera). Directory base `di` advances 0x30
   per row (rows 0..9).
   - Each row is drawn twice: left half (`[0xe54]=0`, fill routine `[0xe46]`,
     grid cols 0,1,2,3, di += 0xc per col) then right half (`[0xe54]=1`,
     routine `[0xe48]`, grid cols 6,5,4,3, same directory cells — shapes are
     stored only for the left half and mirrored, §5.2). Center col 3 is drawn
     by both halves. (0x2da9-0x2dee; col index math 0x2db0/0x2dc0-0x2dcb.)
   - Per tile: `bl = grid_hi_byte & 0xf` = shape code; dispatch
     `call [bx*2 + 0xb7f]` (0x2db0-0x2db8) — composer jump table, §5.
   - **Ship row split** (`[0xe50]==4`, i.e. the row at pos>>3, 8th processed):
     directory base is replaced by 0x210 ("pre-ship" row 11) for pass
     `[0xe56]=0`, then the ship is drawn (`call [0xe4a]` @0x2e09), then the row
     is re-run with directory 0x240 ("post-ship" row 12) (0x2d92-0x2da6,
     0x2df0-0x2e0e). The data decides which geometry lands in front of the
     ship per phase (verified: phase 0 row 12 is empty, phase 4 row 12 holds
     the block/tunnel parts that have scrolled in front; §4.4).
5. `fn_3492(1)` (0x2e21): VGA dirty-blit back buffer → A000 (§6); EGA: no-op
   (fn_3492 EGA path is `ret` @0x3497-0x349e; the page flip displays the frame).
6. Bookkeeping (0x2e2d-0x2e48): `[0xe76]=[0xe36]` (previous scroll pos),
   previous ship/shadow screen offsets `[0xe7a]=[0xe78]`, `[0xe7e]=[0xe7c]`,
   and the 956-byte ship+shadow visibility masks copied 0xe92→0x124f.

---

## 4. TREKDAT.LZS — the 8 road-phase span objects

### 4.1 File & loader (fn_00bb)

File = sequence of `{u16 raw_size, u16 lzs_size, LZS block}` until EOF.
Retail TREKDAT.LZS contains **exactly 8 objects** (raw sizes 24716..27278);
`[0x54b4]` = 8 after load. Loader (0xbb): seg = alloc(raw_size) stored at
`[0xe82 + i*2]` (0x149); word at obj[0] = raw_size - lzs_size (0x155-0x15e);
LZS-decompressed into obj[raw-lzs .. raw) (0x16e-0x177). The 0x7D64 buffer
allocated at 0x106 (with 64 KB DMA-bank check) is the SFX.SND buffer
(`[0x456c]`, later filled by fn_554b @0x55c7-0x55ee), not a TREKDAT object.

All consumers of the segment table `[0xe82]` (complete list): loader 0x149,
render_frame 0x2d6f (`pos&7`), fn_3492 0x3512/0x3522 (old/new phase),
EGA converter fn_3a23 0x3a26, expander fn_3a7a 0x3a7d. **Object i = road span
tables for scroll phase i (0..7). Nothing else lives in TREKDAT** (no fonts,
no sphere sprites — the font is the BIOS ROM 8x8 font, §10; the ship is in
CARS.LZS; there are no sphere objects in the retail game, §9.4).

### 4.2 Second-stage in-place expansion (fn_3a7a, run once from fn_2cb2 @0x2cb4)

For each of the 8 objects (`bx = 0..0xe` @0x3a7b-0x3aa9): `si = word obj[0]`
(the packed-data offset), `di = 0`, then:
- copy 0x270 bytes (the directory) (0x3a8d rep movsw 0x138 + part of first record),
- for each of **0x410 = 1040 records**: copy 3 header bytes; then per span
  entry: copy 1 byte; if it was 0xFF the record ends; else copy 1 more byte and
  **append a zero pad byte** (0x3a94-0x3a9e).

So packed entries are 2 bytes, expanded entries are 3 bytes; expansion ends
exactly at raw_size (verified by emulation: si_end == di_end == raw_size for
all 8 objects).

### 4.3 Expanded object format

```
offset 0x000: directory: 13 rows x 4 columns x 6 u16 record pointers
              (rows 0..10 = perspective rows far->near (row 10 only used by the
               incremental updater's +0x30 lookup, §6); row 11 @0x210 = ship-row
               "pre-ship" set; row 12 @0x240 = ship-row "post-ship" set;
               columns = grid cols 0..3 (left half; right half mirrored))
offset 0x270: 1040 records, each:
              u8  color      (palette index; patched at runtime for colorable faces)
              u16 anchor     (REL offset = y_rel*320 + 160; y_rel = screen_y - 32;
                              x is ALWAYS 160 = screen centerline, verified for
                              all 8320 records)
              span entries (one per scanline, top to bottom), 3 bytes:
                u8 offset    (pixels left of the anchor where the span starts)
                u8 length    (pixels)
                u8 pad       (0 in VGA; rewritten by fn_3a23 on EGA, §11)
              u8 0xFF terminator
```

The 6 record pointers per cell are **chains** (consecutive records; the fill
routine leaves si after the record it drew, so composers draw "the next
record" by calling the fill again):

| kind (word idx) | contents (record chain) | baked colors (obj0 samples) |
|---|---|---|
| 0 | floor slab: top, inner-side edge, front edge | 1 / 31 / 16 (patched at runtime: c, c+30, c+15) |
| 1 | tunnel entrance interior | 65 (patched 0x41/0x43) |
| 2 | low-block tier: front, side | 61 (patched block color) / 63 (fixed) |
| 3 | block top faces: 3 variants (plain top / top split around tunnel arch x2) | 62 / 62 / 62 (never patched) |
| 4 | tunnel body: 6 arch gradient records + 2 inner-rim records | 68,69,70,71,72,73 / 66,66 (never patched) |
| 5 | high-block tier: front, side | 61 (patched) / 63 |

### 4.4 The projection (all baked into the data)

There is **no projection math at runtime** — the perspective is entirely
pre-computed in the span tables. Extracted row-top screen y (kind-0 record 0
anchor, center column) per phase object:

```
phase:  r0  r1  r2  r3  r4  r5  r6  r7  r8   (r9/r10 anchors=158+ : off-screen)
  0:    32  34  37  41  46  52  61  76 102
  1:    34  35  38  41  46  53  63  79 107
  2:    34  35  38  42  47  54  65  81 112
  3:    34  35  39  43  48  55  66  84 118
  4:    34  36  39  43  49  56  68  87 124
  5:    34  36  39  44  49  58  70  91 158(-)
  6:    34  37  40  44  50  59  72  94 158(-)
  7:    34  37  40  45  51  60  74  98 158(-)
```

- Horizon: road vanishes at y=32-34. Visible floor rows ≈ 8 (rows 9/10 carry
  empty span lists at phase 0; lower rows scroll in via the bottom as phase
  advances — the data simply contains no spans for off-screen scanlines).
- Ship row = directory row 7. At phase 0 it spans y 76..101; the ship on the
  floor is drawn at y = 0x9d - 0x2800/0x80 = 157-80 = **77**, i.e. exactly
  sitting in its row band.
- Spans grow with proximity: center-column top-face half-width per row
  (phase 0): r1≈1-2px, r2≈2-3, r3≈3-5, r4≈5-7, r5≈7-10, r6≈10-15, r7≈15-24,
  r8≈24-25. Column x extents at row 7, top scanline: col0 x58..87, col1
  87..116, col2 116..146, col3 (center, left half) 145..160 — i.e. 7 tiles of
  ~29-31 px at that depth, widening per scanline. Tile k covers
  `x ≈ 160 + (k-3.5)*w(y)` with w(y) growing linearly down the screen.
- Bottom clip is baked: no span reaches below y=137, and all spans within
  y 129..137 are **pre-clipped against the dashboard cowl trapezoid**
  (verified: 988 spans in that band across 8 phases, zero penetrations of the
  region |x-160| < halfwidth[y] from table ds:0x44a, §7.2).
- Ship-row pre/post split: row 11 = full row geometry (identical to row 7
  cells at phase 0); row 12 = only the geometry that is nearer than the ship's
  fixed screen position for that phase (empty at phase 0; at phase 4 holds
  block fronts/tunnel arch parts at y≥82).

---

## 5. Tile codes and the shape composers

Grid tile = u16 (7 per row at ds:0x1638): low byte = colors
(`lo & 0xf` = floor color 1..15, 0 = hole; `lo >> 4` = block color, 0 → default
61), high byte low nibble = shape code (dispatch 0x2db0), `hi & 7` also indexes
the height-class table.

- Height classes ds:0xb77 = {1,2,3,3,4,4,1,1} (flat=1, tunnel=2, low block /
  tunneled low block=3, high block / tunneled high block=4).
- Composer jump table ds:0xb7f (16 words):
  shape 0 → 0x2e50 (flat floor), 1 → 0x303d (tunnel on floor), 2 → 0x2e9f
  (low block), 3 → 0x2ee1 (tunnel through low block), 4 → 0x2f3c (high block),
  5 → 0x2fb0 (tunnel through high block), 6..15 → 0x3aad (`ret`, draw nothing).
- The floor-color nibble doubles as the tile effect (fn_1a9c @0x1b40-0x1b5d):
  2 = sticky (speed -0x12F/tick), 9 = supplies (refill fuel+oxy to 30000),
  0xA = boost (+0x12F/tick), 0xC = burning (explode); code 8 tested at 0x2375
  (slippery **TBD**). Speed clamped 0..0x2AAA (0x1b63-0x1bab).

Composer behavior (all run with ds = phase object, di = directory cell,
bp = grid tile ptr, si advanced through record chains by the fill routine;
`[bp-0xe]` = next-nearer row's tile, inner-neighbor tile = `[bp ± 2]`
depending on `[0xe54]`, computed at e.g. 0x2e61-0x2e6e):

- **fn_2e50 (floor slab)**: if floor color 0: nothing. Else patch record color
  and fill: top (`[si]=c` @0x2e59), then inner-side edge with `c+0x1e` if the
  inner neighbor has no floor (0x2e77-0x2e85) else skip the record via fn_31b5;
  then front edge with `c+0xf` if the nearer row has no floor (0x2e90-0x2e9d).
  **Road palette layout implied: 1..14 floor colors, +15 = front-edge shades
  (16..29), +30 = side-edge shades (31..45).**
- **0x303d (tunnel)**: floor via fn_2e50; if nearer row's shape < 1: tunnel
  front wall, color forced 0x43=67 (0x3049); then 6 fills of the kind-4 chain
  (arch gradient, baked colors 68..73) (0x3054-0x306d); 2 more fills (inner
  rim, color 66) if nearer shape < 1 (0x3078-0x307d).
- **0x2e9f (low block)**: floor; top faces (kind 3 chain, baked 62) if nearer
  shape < 2 (0x2ea8); front (kind 2, patched `lo>>4` or 0x3d=61 @0x2eb3-0x2ebf);
  side (next record) if inner neighbor shape < 2 (0x2ec4-0x2edb).
- **0x2ee1 (tunneled low block)**: floor; tunnel entrance (kind 1, color
  forced 0x41=65) if nearer shape < 2 (0x2eea-0x2ef0); front+side as above;
  if nearer shape < 2: kind-3 chain with first record skipped, draw records
  1+2 (top split around the arch) (0x2f2b-0x2f36).
- **0x2f3c (high block)**: floor; top (kind 3) if nearer shape < 2; kind 2:
  skip front record, draw side if inner neighbor shape < 2 (0x2f50-0x2f6a);
  upper tier (kind 5): front patched (0x2f6f-0x2f7e), side if neighbor
  shape < 4 else skip (0x2f83-0x2fa1), extra record if nearer shape < 4 (0x2fa4-0x2faa).
- **0x2fb0 (tunneled high block)**: combination of 0x2ee1 + 0x2f3c upper tier
  (entrance 0x41 @0x2fbc, arch via kind-4-like fills, upper tier kind 5).

The "nearer row / inner neighbor" tests are painter's-algorithm occlusion
shortcuts: faces that the (drawn-later) nearer/inner geometry would fully
cover are skipped.

### 5.1 Span fill routines

**VGA left half, 0x3137** (`[0xe46]`, installed into `[0xe4c]` @0x2d8a):
```
lodsb color index k; fill byte = [ss:k*4 + 0x322]      (0x313b-0x3148)
lodsw anchor -> di (es already biased to row 32)        (0x314a)
per scanline: lodsb off (0xFF=end); di = anchor-off; lodsb len; inc si (skip pad)
              odd-align stosb then rep stosw of the fill byte; anchor += 320
                                                        (0x314f-0x316f)
```
**VGA right half, 0x3174** (`[0xe48]`): identical but `di = anchor-1+off`,
fills backwards (`std`, 0x319c) — mirror image around x=160; fill byte =
`[k*4 + 0x323]`.

**Color quad table ds:0x322** (74 entries x 4 bytes = {VGA-left, VGA-right,
EGA-left, EGA-right}): entries 0..30 symmetric (k,k); **31..45 (side-edge
shades) map left→31..45 / right→46..60** — left and right tile sides get
different shading; 61 (default block) → 61/61; 62 (block top) → 62/62;
63 (block side) → 63/64; 65/66/67 (tunnel colors) symmetric; **68..73 (arch
gradient) swap order between halves: left {71,70,69,68,69,70} / right
{70,69,68,69,70,71}** — a symmetric tunnel-interior gradient.
Hence the road palette (72 colors, base 0): 1..14 floor, 16..29 front edges,
31..45/46..60 left/right side edges, 45..59 also = shadowed floor (§7.3),
61 block default, 62 block top, 63/64 block sides, 64 shadowed block, 65..67
tunnel walls, 68..73 arch gradient.

**fn_31b5 = skip_record**: `si += 3` until `[si]==0xFF`, then `si++`
(0x31b5-0x31be) — confirms the record layout.

---

## 6. fn_3492 / fn_38a3 — incremental update (VGA)

`fn_3492(ax)` (EGA: immediate ret @0x349e). Two modes via `[0xe72]`(src) /
`[0xe74]`(dst) / per-tile routine `[0xe4e]` (0x34a1-0x34c9):
- ax=0 (called as `[0xe44]` via 0x348b): src = pristine `[0x517e]`, dst = back
  buffer, routine 0x3633 — **erase pass** (restore background over areas the
  road geometry is vacating).
- ax=1 (called at 0x2e21): src = back buffer, dst = A000, routine 0x36d7 —
  **blit pass** (copy changed areas to screen).

Paths:
- `[0xe3e]` (full flag) set, or scroll delta `[0xe36]-[0xe76]` ≥ 8: full
  `rep movsw` — normally 0x4240 words from offset 0x2800 (rows 32..137 only;
  the sky band 0..31 never changes), with `[0xe3e]`: 0x5640 words from 0
  (full viewport incl. sky) (0x3610-0x3631).
- delta == 0: skip to ship handling fn_39b8 (0x34e0-0x34e2).
- 0 < delta < 8 (same-frame sub-row scroll): the span-diff machinery:
  - `[0xe6e]` = phase object of OLD pos, `[0xe6c]` = NEW pos (0x3509-0x3526);
    `[0xe70] = 0x30` iff old and new are in the same integer row (0x34f3-0x3504).
  - Iterates the same 10 rows x (4+4 mirrored cols) as the composer
    (0x3543-0x3608, counters `[0xe50]/[0xe52]/[0xe54]`), reading for each cell:
    cur tile floor `[0xe62]`, block color `[0xe68]`, height class `[0xe5a]`,
    nearer row's `[0xe64]/[0xe6a]/[0xe5c]`, inner neighbor's `[0xe66]/[0xe5e]`,
    neighbor-nearer `[0xe60]` (0x354f-0x35d9), then `call [0xe4e]`.
  - 0x3633 (erase) / 0x36d7 (blit) call **fn_38a3(selector)** for each region
    kind whose visibility could have changed, comparing heights/colors of the
    current vs nearer row (e.g. blit: kind 0 when floor appears @0x36f4-0x3713
    with sub-records 0/1/2, kind 2 when height-2/3 block faces change
    @0x3733-0x3756, kind 4 sub 0..5 when a tunnel pops @0x3846-0x3867, kind 5
    for height-4 tiers @0x37a4-0x37ef; erase: the complementary
    "tile disappeared / got shorter" conditions @0x3633-0x36d6).
- fn_38a3(selector): selector = kind<<8 | subrecord | 0x8000(no-clip).
  `di = (kind*2) + (row*4 + coldist)*12` (0x38a9-0x38cc); looks up the OLD
  record (`ds=[0xe6e]`, `di+[0xe70]` — one directory row nearer when the
  integer row didn't change; directory row 10 exists for exactly this
  overflow) and the NEW record (`ds=[0xe6c]`, di), skipping `selector&0xff`
  records (0x38da-0x3915). Unless bit15: skips leading scanlines of the NEW
  record while `new_anchor_row < old_anchor` (0x3924-0x3936) — clipping the
  copy to the band where old and new shapes differ. Then copies the NEW
  record's spans **src→dst at identical offsets** (word-aligned rep movsw,
  std-mirrored for the right half) (0x393d-0x39b1). +0x2800 row-32 bias added
  at 0x3938.
  **TBD**: the precise minimality argument of the old-anchor clip (which world
  row the old-phase lookup addresses) — the code behavior above is exact, but
  a port should verify pixel-equality against a full recompose.
- Finally fn_39b8: erase the previous frame's ship (29x24) and shadow (29x9)
  rects via fn_3a06 using the saved masks at 0x124f/0x1507 (only pixels whose
  mask byte == 2 are copied, 0x3a0b-0x3a13), and when dst==A000 also blit the
  NEW ship/shadow rects using masks 0xe92/0x114a (0x39df-0x3a01).

---

## 7. Ship sprite, occlusion mask, shadow

### 7.1 Sprite draw (VGA `[0xe4a]` = 0x323f, invoked mid-ship-row @0x2e09)

- First builds the visibility mask: **fn_32a5**, see 7.2.
- `di = (0x9d - alt)*320 + [0xe34] - 0x6e`, saved to `[0xe78]` (0x324e-0x3263).
- Sprite cell is read **column-major**: 29 screen columns (dx=0x1d @0x326d) x
  24 screen rows (cx=0x18), source advancing linearly (0x3270-0x329b) — i.e.
  each 24-byte row of the 24-px-wide CARS PICT is one screen *column*; a
  24x30 PICT cell (720 bytes) yields a **29-wide x 24-tall screen sprite**
  (last PICT row unused). Pixel 0 transparent; a pixel is drawn only if its
  mask byte at `[bx+0xe92]` is nonzero, and the mask is set to 2 ("drawn")
  (0x3273-0x3286).

### 7.2 Visibility mask fn_32a5 — dashboard-cowl clipping

Clears 0x1de words at 0xe92 (ship mask 29x24 = 696 bytes at 0xe92..0x1149,
shadow mask 29x9 at 0x114a..0x124e). For each of 33 rows (24 ship + 9 shadow;
the y/table index jumps by clearance-8 between the two groups @0x3336-0x3345):
- skip rows outside the viewport (`bx`=0x9d-y must be in [0x14,0x9d] ⇔ y in
  [0,137], 0x32c7-0x32d0);
- default visible x-interval = whole row; if `halfwidth = word[0x44a + y*2]`
  is nonzero: the band `160±halfwidth` is opaque — if the ship anchor is left
  of it, clip right at 160-halfwidth, else clip left at 160+halfwidth
  (0x32da-0x32fe; coordinates stored biased +110);
- fill the visible part of the mask row with 1 (0x3300-0x332b).

**ds:0x44a** = 138 words, zero for y 0..128, then {89,111,123,133,138,143,148,
153,158} for y=129..137 — exactly the opaque silhouette of the dashboard's
trapezoidal top edge (verified against DASHBRD.LZS pixels: y129 opaque
72..247, y137 2..317). So the mask clips the low-flying ship (and shadow)
behind the dashboard cowl; the road spans themselves are pre-clipped in
TREKDAT (§4.4).

### 7.3 Shadow (VGA 0x33e1, called from ship draw @0x329d)

- `map = clearance/5`; ≥5 → no shadow (0x33e7-0x33f2). Shape = 261 bytes
  (29x9, row-major) at **ds:0x65e + map*0x105** — 5 baked
  ship-silhouette shadow maps, shrinking with height (156/106/58/28/12 pixels).
- Position: `y = (0x9d - alt) + 0x10 + clearance`, x = ship rect x; offset
  saved to `[0xe7c]` (0x33fe-0x341a).
- For each set shape pixel with shadow-mask `[bx+0x114a]` nonzero: mask := 2
  and **darken the back-buffer pixel**: floor colors 1..15 → +0x2d (45..59),
  block default 0x3d → 0x40 (0x3426-0x344a). It's a read-modify-write effect,
  not a sprite.

### 7.4 CARS index recap & road-start call

fn_2b21 (road start) renders the first frame directly (0x2bea-0x2c32):
`fn_2d03(0x100, 0x18, 0x50, cars+((a*3+b)*3+14)*0x2d0, 1, 0, [0x5486])` with
`b = fn_0afa(0,0x2800) = 0` and `a = fn_0b34(0x8000) = 3` — centered, level
→ frame 14+9*3+0 = 41, byte offset 41*720; pos 0x18 = row 3 phase 0; altitude
0x50=80 (on floor). The last three args (full=1, ground=0, seg=back buffer)
are words intentionally left on the stack from the preceding fn_0afa call
(push sequence 0x2be9-0x2bf1, only 4 bytes popped at 0x2bf6, `add sp,0x10`
after the fn_2d03 call clears all 8).

---

## 8. Backdrop, pristine buffer, dashboard

- **The 320x138 WORLDn backdrop does not pan or scroll during gameplay.** All
  copies (full rep movsw @0x3623-0x362f, span copies in fn_38a3, ship-area
  restores fn_3a06) use identical src/dst offsets. The "horizon" is fixed:
  road geometry exists only at y≥32; rows 0..31 are copied from the pristine
  buffer only on full redraws.
- Pristine buffer construction (fn_536b): backdrop PICT (palette base 142)
  buffer reused as the pristine screen; dashboard PICT (320x71 @ y=129,
  palette base 92) composited over it with 0-transparency (fn_4184 loop
  @0x541f). The dashboard rows 138..199 are copied to screen once per road
  (fn_2b21 @0x2b70-0x2b84, 0x4D80 bytes from offset 0xAC80).
- fn_4184/fn_4162: general compositor — fn_4162(bg_seg, dst_seg, t0, t1) sets
  `[0x4520]`(bg)/`[0x962c]`(dst)/`[0xaf4a]`(t0)/`[0x9624]`(t1, self-modifying
  patch @0x418e into cmp at 0x41cb); fn_4184(dst_off, src_off, left_raw,
  right_raw): per row, copies left_raw bytes raw, then for the middle pixels:
  sprite px < t1 → leave dst untouched; < t0 → copy bg pixel; else write
  sprite px; then right_raw raw bytes (0x41a2-0x41de).

---

## 9. HUD / dashboard (fn_124b, every frame)

All small HUD draws go through the staging pipeline: shape bytes →
fn_0eb5(ptr, count, colorA, colorB) renders into staging seg `[0xaf3a]`
(0=transparent, 1=colorA, else colorB; 0xec8-0xed8) → fn_41e5(&{seg, screen_ofs,
h, w}) composites staging → A000 with 0-transparency (fn_4184 rows @0x4220-0x4259;
EGA: also to A200, planar via fn_3cfc).

1. **Speedometer (SPEED.DAT)**: 34 arc-segment records; offsets table
   **ds:0x4580** (34 u16, loaded by fn_5465 from "speed.dat" @0x5514-0x551f),
   blob far ptr `[0x54b0]:[0x54b2]`. Record = {u16 screen_ofs, u8 w, u8 h,
   w*h bytes of 0/1/2} (read by fn_0edf @0xf37-0xf5d). Lit colors VGA
   1→0x5e,2→0x5f; unlit 1→0x5c,2→0x5d (0xef8-0xf15; dashboard palette 92..95).
   Target segments = clamp((speed - [0xaf3e:af40])/0x141, ..34) (0x126e-0x1296;
   321 speed units/segment; [0xaf3e] = this-frame collision speed loss from
   fn_1d4d @0x1f06-0x1f16); fn_124b lights/clears from the cached count
   `[0x41ca]` to the target (0x12b1-0x1307).
2. **Oxygen gauge**: 10 records, offsets **ds:0x9604** (from OXY_DISP.DAT
   @0x54f2-0x5500), blob `[0x5482]:[0x5484]`; value = ceil(`[0xb14c]`/3000)
   clamp 10 (0xbb8=3000 divisor with +0xbb7 @0x130a-0x1323); cached count
   `[0x457a]` (0x1328-0x1394).
3. **Fuel gauge**: 10 records, offsets **ds:0x548c** (from FUL_DISP.DAT
   @0x5503-0x5511), blob `[0x961a]:[0x961c]`; value = ceil(`[0x54a0]`/3000);
   cached `[0x9618]` (0x13f3-0x147d). (Full tank = 30000 units, init 0x1fa1.)
4. **Warning lamps** (blinking via fn_11d5 = rect color-swap 0x63↔0x64 VGA,
   plus beep fn_03c2(3)): `[0x457c]==5` → 7x7 rect at (160,161) @0x13a1-0x13f0;
   `[0x457c]==4` → 16x5 rect at (155,169) @0x1480-0x14d9 (the same state also
   freezes the engine-flame animation, §2.3). Blink phase = (`[0x160c]` mod 9)
   > 4 → 1 else 0 (0x1251-0x126c); the beep fn_03c2(3) fires once per on-phase.
   **`[0x457c]` is only ever assigned 0/1/2 in the binary (0x1fad, 0x27db,
   0x1ab5, 0x22f4) — the 4/5 lamp/flame code appears to be dead/vestigial.**
5. **Progress bar**: 30 steps; progress = clamp((Z-0x30000) /
   ((rows-3)*0x10000/30), ..29) (0x14dc-0x152c, road row count `[0x41ce]`);
   for each new step fn_116b(x): at column x+0x2a, y=143, finds the vertical
   extent of equal-colored slot pixels (probing upward via fn_10e4 = read
   pixel A000) and paints the run with color 0x60=96 (0x116b-0x11d1) — the
   slot height is taken from the dashboard art itself.
6. **`[0x4568]` indicator** at (203,156), 26x5: shape = ds:0x204 + 0x82*state
   (two stamps, values 5/6 → both render as color 0x62=98 stencil text;
   0x1559-0x1573, cache `[0xaf30]`). State 1 is set on collision speed-loss
   (fn_1d4d @0x1f22). Stamp masks (nonzero pixels) spell two different tiny
   labels — exact glyph reading **TBD**.
7. **GRAV-O-METER**: drawn once per road from fn_2b21 @0x2ba7-0x2bbd:
   `fn_1067(x=0x60, y=0x9c, value=([0x456e]-3)*100, ndigits=4)` — `[0x456e]` =
   road gravity header word. fn_1067 (0x1067): digit si (0=ones) =
   (value / word[0x308+si*2]) % 10 with divisor table ds:0x308 =
   {1,10,100,1000,...}; each digit drawn via fn_0fc6 at x + (ndigits-1-si)*5,
   4x5 px, shape ds:0x13c + digit*0x14 (10 digit stencils of 0/1/2, colors
   VGA 1→0x61, 2→0x62 @0xfd6); value reduced per digit, **stops when the
   remaining value is 0 (after ≥1 digit) = leading-zero suppression**
   (0x1080-0x108e). It is a 4-digit numeric display, not a needle.

Dashboard pristine copy: gauges draw direct to A000(/A200); the viewport
renderer never touches rows >137, and road spans never overlap the cowl, so
HUD and viewport coexist without re-blitting the dashboard.

---

## 10. Text renderer fn_450a

`fn_450a(x, y, asciiz, color)`: per char fn_44a2(x, y, ch, color), x += 8
(0x4510-0x4540). fn_44a2: font = BIOS 8x8 ROM font pointers saved at startup
(`[0x3194/0x3196]` chars 0-127, `[0x3198/0x319a]` chars 128-255 via int 10h
ax=1130h bh=3/4 @0x22-0x44); bit7 of ch selects the upper half (0x44b6-0x44d2).
VGA glyph blit fn_3c36: si = font + ch*8, es=A000, di = y*320+x; 8 rows x 8
bits, `rol`+set pixel=color for 1-bits (transparent 0-bits), row stride +0x138
(0x3c53-0x3c76). No clipping. Used in-game for "The End" (ds:0xd46, x=0x84) /
"Road Completed" (ds:0xd4e, x=0x68) at y=0x50, color 0x63 (fn_2b21
@0x2c6e-0x2c8a), and by the menus.

---

## 11. Palette fades fn_4b72

`fn_4b72(palette_ptr, dir, steps)` (steps = 0x24 = 36 ticks = 1 s in game use):
- 768 bytes at ds:0x31b4 zeroed = black palette (0x4b78-0x4b80).
- VGA: two far copies via fn_4b27 (descriptor {seg, start=0, count=0x100});
  dir!=0 → fn_4315(black→target) = fade in; dir==0 → fn_4315(target→black) =
  fade out (0x4bad-0x4bd8).
- **fn_4315(from, to, steps) exact stepping** (0x4315):
  `[0x160c]=0`; loop: `t = 100*[0x160c]/steps`, clamped to 100; forced 100 if
  `[0x54ac]` skip flag (set by any keypress when armed, fn_4137 @0x4155);
  for all 768 components: `out[i] = from[i] + (to[i]-from[i])*t/100` (signed
  imul + idiv 100, truncation toward zero; 0x43d8-0x440f); write all 256 DAC
  entries (fn_6233); fn_4137 (poll keys); repeat until t==100. So it free-runs,
  recomputing t from the 36 Hz tick — duration `steps` ticks regardless of DAC
  write speed.
- fn_6233(start, count, farptr) DAC write (0x6233): if BIOS equipment byte
  test fails → int 10h ax=1012h; else manual: wait for vertical retrace start
  (port [0x463]+6 = 3DAh bit 3, 0x6280-0x628e), then out 3C8h index, 3 bytes
  per color to 3C9h, re-synchronizing to retrace every 64 colors
  (0x626f-0x627d) to avoid snow.
- EGA path: no fade — set 16-color palette immediately (fn_60c1 with either
  the menu palette at ds:0xbb0 or black, 0x4be1-0x4bfd).

---

## 12. EGA-specific notes (brief)

- fn_3a23 (render_init, EGA only): rewrites all 8 phase objects in place for
  planar drawing: record anchors >>= 3 (byte address), each 3-byte span entry
  {off,len,pad} → {byte_count, left-edge bit mask, right-edge remainder}
  (0x3a31-0x3a68).
- Span fills 0x3083 (left) / 0x30d9 (right) use the quad-table EGA colors
  (`[k*4+0x324]` @0x3092) through the GC (map mask/bit mask via 3CEh); the
  right-half fill mirrors edge masks with the 256-byte bit-reverse table
  **ds:0x55e** (0x3106).
- Ship draw 0x31bf: same mask logic, planar bit-mask per column
  (0x31fc-0x3234); shadow 0x334f uses GC XOR-ish increment (`inc byte [es:di]`)
  with map 0x1062/0x114a bookkeeping.
- fn_2d03 step 1 copies the full pristine plane every frame (0x3462), and the
  final fn_3492(1) is a no-op; display via page flip (fn_0e23).

---

## 13. Remaining unknowns / TBD

- fn_38a3 old-anchor clip minimality (§6) — code semantics documented exactly;
  recommend pixel-diff verification in a port.
- CARS frames 75/76 (2310/30 = 77 cells; 0..13 explosion, 14..74 ship) —
  unreferenced by the index formula; possibly padding/unused art.
- The exact text of the two 26x5 stamps at ds:0x204/0x286 (§9.6).
- `[0x457c]` 4/5 warning-lamp + flame-anim code is unreachable in retail
  (no assignment writes 4 or 5) — confirm against other releases.
- Tile effect of floor code 8 (`[bp-0xe]`-style flag at 0x2375) — gameplay
  side, slippery/ice suspected.
