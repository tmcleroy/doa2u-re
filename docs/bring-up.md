# DOA2 Ultimate bring-up

## Established baseline

The project has public source, synthetic tests, custody checks, a pinned
lifter and a native kernel/graphics/audio runner. The actual game has been lifted;
its generated source and receipt remain private. Library API replacements
and game compatibility remain bring-up work.

Ultimate's launcher and main game are separate executables. Read the ignored
`private/target.json` to obtain the selected game path and its hashes; do not
assume the disc's default executable is the game.

The ISO hash establishes import consistency, not independent authentication
of a retail disc revision. Keep the selected revision provisional until its
identity has been checked against a trusted independent reference.

## Memory and startup

The allocation arena starts at the loaded image's page-aligned end and stops
at `0x03F00000`. The remaining guest RAM holds the bootstrap stack, kernel
exports, PCR and TLS. PCR occupies `0x03FF0800` through `0x03FF0BFF`;
generated FS reads and native thread snapshots use that dedicated block.
Physical page zero remains available to the SDK startup pool and cannot
overwrite PCR. Heap and contiguous allocations grow toward each other and
reject overlap. The loader checks sections, source digests, stack/TLS bounds
and reserved ranges before running code.

Memory statistics report the arena's available pages and reusable allocation
capacity, with image, pool, stack and virtual reservations accounted for.
Raw runtime allocations and alignment gaps stay consumed. Cache pages are
zero because this host has no separate guest cache-page allocator.

Bootstrap TLS data is aligned to 16 bytes, with its pointer slot included in
the stack-base calculation used by XAPI's negative TLS index. The selected
entry creates the startup worker and returns; the host then schedules that
worker. Closing its handle invalidates external use while retaining the
worker until it exits.

The XAPI kernel-compatibility entry is a temporary whole-library replacement:
the native host owns its kernel's executable lifetime, so console kernel
patching and INIT/LDT registration are unnecessary. Its no-argument calling
convention is tested; it consumes the return slot and leaves game globals
untouched. SDK initializer-table callbacks are supplied through the public
seed recipe rather than replaced by success stubs.
Game handlers reached only through data tables (the game-mode table at
0x6959f0 and the four-entry handler groups at 0x7c0040) are seeded the same
way when the lifter misses them; an unresolved indirect call names the next one.
Tail-jump switch cases past a null table slot (0x2a82bc, cases 0x2a8247 and
0x2a8286) are seeded too. A lone in-function arm ended by a null slot
(0x2929f4) resolves in the lifter as a local switch.

`tools/build_game.py` authenticates the selected XBE, lifter revision,
generated source manifest and final runner. CMake rejects partial or wrong
authentication, chunk gaps and uninitialized generated EBP prologues. Missing
function bodies, unsupported imports and untranslated instructions stop
explicitly.

Use `tools/run_game.py` for a bounded probe. It hardlinks the disc files,
preserves saves and logs the runner identity. Private notes hold the observed
frontier; a timeout by itself does not establish gameplay progress.

The first boot copies about 700 MB of disc archives into the `Z:` cache
partition, 32 KB per frame, while drawing only cleared frames. At the native
pace this takes roughly 12 minutes and shows a black window. The cache lives in
`private/play-disc/.recomp-storage/partition5`. A probe stopped before all
three archives finish leaves a partial install that the next boot repeats.

The CRT remainder intrinsic at `0x336dfe` is replaced as a whole with native
`fmod`. It consumes the x87 divisor at ST(0) and dividend at ST(1), returns
one x87 result, and pops only the return address. This avoids unseeded
internal special-value dispatch branches such as `0x338e10`. Numeric results
include NaN for domain errors; guest CRT errno, math-error callbacks and
unmasked floating-point exceptions are not implemented by this seam.

## Graphics API contract

`d3d8_adapter.c` owns the whole 5849 CreateDevice boundary and uses the
committed sister-project D3D11 presenter. The plain device, frame and tile
models remain callable independently of interception. Creation accepts the
verified 720x480 linear BGRA8/D24S8 mode and two caller-provided color headers
plus depth. It validates their storage before creating the host device and
publishes the static guest device only after host creation succeeds.

The device has 5849's embedded 24-byte surface headers and owner/binding
references. Getters add references, Release preserves retained surfaces,
and default Swap rotates Data/Lock fields while keeping header addresses
fixed. Viewport extents are clamped; invalid origins stop. Tiles retain their
six-word API descriptors without exposing hardware. Clear and default Swap
submit semantic commands, with the shared guest-vblank pacing model.

`d3d8_state_adapter.c` binds gamma, ten transforms, point/directional light
records and active slots, and fog color. Gamma uses two identity-initialized
SDK shadow banks; Swap consumes the bank selected by preincrement frame
parity. Light-array growth repairs active pointers. HRESULT and void
calling conventions have separate synthetic ABI checks.
CreateDevice replays the SDK render-state defaults from the game's own tables
into the guest shadow and the simple-state model, so state the game saves and
restores (such as the color write mask) keeps its SDK value. Complex-state
defaults reach only the shadow.

The frame-state adapter records semantic depth, stencil, blend and alpha-test
state through the shared render-state model. Simple states use the SDK's
fastcall convention, with separate guest shadows maintained by the caller.
SetVertexShader rebuilds the fixed-function declaration and applies the 5849
dirty bits. Program handles load their CreateVertexShader upload blocks into
the device program shadow and select the object as LoadVertexShader and
SelectVertexShader do; SetVertexShaderConstant1/4 keep the 192-entry constant
shadow. Declaration-only handles and declaration constants stop explicitly.
SetMaterial retains its 17-word device shadow. Spot lights, nondefault raster
modes and runtime multisampling remain explicit limits.

`d3d8_resource_adapter.c` binds registered textures, palettes, vertex/index
buffers and surface views. Binding references retain storage independently
of visible references. Native-created vertex buffers and mip surface wrappers
have owned lifetimes; final destruction of external headers is guarded until
their allocator is verified. Locks validate mip/rectangle spans. Write locks
and texture registration invalidate native texture caches. Queued draws own
all submitted vertex, index, palette and texture bytes, including non-linear
formats. Cube/volume views and displaced swizzled locks remain unsupported.
The XPR loader (FUN_00289f80) registers volume textures (a 64x64x8 A8R8G8B8
map appears after a Story fight), so Register releases a swizzled,
uncompressed volume's full span: width x height x depth per level, each
axis halving to 1. Sampling, locking or viewing a volume still stops.

`d3d8_draw_adapter.c` binds DrawVertices, DrawVerticesUP and
DrawIndexedVertices to the existing D3D11 presenter. It snapshots fixed-function
layouts, transforms, viewport, depth/blend state, stage-0 texture and palette.
Supported combinations include texture/diffuse modulation, white diffuse alpha
selection, texture alpha selection and texture factors. Linear min/mag filters
support point or linear mip selection with LOD bias encoded as the lazy
texture-state upload (0x34f400) does: 1/256 steps, truncated after +0.5 and
clamped to the 13-bit field, so NaN and out-of-range biases clamp to -16 or
15.99 (D3D11's maximum).
Pixel-shader creation, deletion, binding and
constants retain the 5849 layout; native custom shading supports the sister
presenter's four-tap filter in its doubled and identity-scale variants, with
live C0/C1 factors, and the post-process bright pass. The bright pass matches
its exact combiner words and Gaussian-cubic stage-0 filtering, then computes
saturate(2 * (gauss3x3(t0) - c0) - 1) with the 1-2-1 kernel at level 0. The
bloom composite binds stage-0 MODULATE2X of texture and texture factor over
white vertices; quad strips draw as triangle strips. Quad lists (type 8,
including FUN_0023d2a0's four-vertex draws) expand each group of four indices
to two triangles, preserving vertex order and ignoring an incomplete final
quad. UP, stream and indexed draws share the conversion. The split uses
diagonal 0-2; hardware interpolation parity is unverified. The fight's trail strips
(setup 0x23a42d, FVF 0x102, draw 0x23ca7e) use 1x MODULATE(TEXTURE, TFACTOR)
with a grey factor, stage-0 alpha DISABLE and stages 1-3 off, so they draw
texture times factor with the white diffuse alpha.
The stage distortion pass (FUN_0027b070, definition 0x7af5d8) draws FVF
0x140404 quads, 56 bytes each, with t0 PROJECT2D, t1 DOTPRODUCT and t2 DOT_ST
using the MINUS1_TO_1_D3D dot mapping. One combiner sets r0 = t2, and the final
combiner words are zero, so 0x34f5f0 outputs r0 and r0.a while FogEnable
(0x3553d0) and specular (0x3553fc) are off. The adapter matches those words,
keeps the 48 bytes the shader reads and swaps textures: the stage-2 scene copy
becomes the primary texture and the stage-0 normal map the second. The
presenter decodes n = (b - 128) / 127 from the normal texel and samples the
scene at (dot(tc1.str, n), dot(tc2.str, n)). Fog, specular or different words
keep the vertex-layout stop.
The same effect's overlays bind with fixed-function stages. FUN_0027be10 draws
scene objects (FVF 0x142, unlit). Stage 0 has color SELECTARG1(TEXTURE) and alpha
MODULATE(TEXTURE, CURRENT), where stage-0 CURRENT is diffuse. Its texcoords come
from TCI_CAMERASPACEPOSITION with COUNT3|PROJECTED. For that case the TEXTURE0
upload (0x34f7d0, case 0x331) emits S, T and Q as matrix columns 0, 1 and 2.
The presenter interpolates (s, t) and q and divides per pixel. Stage 1 keeps
the color and applies alpha MODULATE(TFACTOR, CURRENT) with ALPHAKILL. Per
object, FUN_0027be10 binds a stage-1 texture (0x27c370, table 0xeaad20) before
its fan (DrawVerticesUP, 0x27c388) or model (FUN_002878b0 without the material
binder). Stage 1's ops never read it, so it only discards pixels whose texel
alpha at UV0 (TCI 0) is zero; alpha is not multiplied. FUN_0027c400 then draws
a pretransformed quad (FVF 0x244). Its stage-0 alpha takes TFACTOR times diffuse.
Stage 1 applies alpha MODULATE(TEXTURE, CURRENT) from a mask sampled at UV1,
with ALPHAKILL, which discards zero mask alpha. An unbound mask passes
through, as 0x352e46 compiles it. Unlit stage-0 selection reuses the lit
path's per-channel choice of texture and diffuse. A bound stage-1 texture
under the TFACTOR variant on another coordinate set, a mask on another
coordinate set or a linear or paletted mask still stop.
The fixed-function draw snapshot
binds up to eight directional/point light slots, front material and ambient,
normalization, blended world/normal transforms and infinite-view specular.
Missing vertex colors resolve material sources as the SDK does. On stage 0 a
CURRENT argument reads diffuse: the combiner compiler flags the first stage
(0x352aba) and its arg mapper (0x3529c0) then maps CURRENT to V0, so a fight's
lit MODULATE(TEXTURE, CURRENT) binds as texture times lit diffuse. Lit stage 0
also accepts MODULATE(TEXTURE, TFACTOR) in either argument order, independently
for RGB and alpha. The SDK arg mapper (`0x3529c0`) maps TFACTOR to combiner
constant C0. The presenter selects that factor instead of lit diffuse for the
chosen channel and adds lit specular after the texture stages. Unlit colored
vertices use the same selection for MODULATE(TEXTURE, TFACTOR): the factor
replaces vertex diffuse in RGB, while alpha independently uses factor or
diffuse. This includes the FVF 0x142 mesh draw at 0x287120. Adapter tests
check both channels and argument orders; native pixels check tint, alpha and
specular ordering. Material
checks cover only what the SDK uploads: diffuse, ambient and specular RGB
(0x34fb90), emissive RGB and diffuse alpha (0x34fc80), and specular RGB and
power (0x34fe40) only while specular is enabled. Non-finite unused terms draw;
a used one stops with its index and bits. Diffuse alpha is the exception:
0x34fc80 sends it unchanged as the lit alpha, so a NaN there (a play session
wrote 7fc08080) saturates by sign to 1 or 0 as a passthrough vertex color does
on hardware. That rule is unverified for material alpha.
Native pixel
tests cover diffuse/ambient, point range/attenuation, specular, normal transforms
and alpha. The reflection material binds the sister presenter's two-texture
straight or premultiplied texture-alpha blend and final diffuse modulation,
with generated reflection coordinates or mesh UV0. Each stage keeps its own
mip filter, LOD bias and WRAP or CLAMP addressing. The fight's chrome (Zack,
Stage 01) clamps its environment map while the base texture wraps, and
FUN_002807f0 and FUN_00280f80 write a stage-0 LOD bias without touching stage
1's. MAXMIPLEVEL, BORDER and MIRROR addressing stay guarded.
The material binder (FUN_00286d30) takes each stage's ops from table
0x7b0078, indexed through 0x7affc8. Stage 2 MODULATEs by diffuse in mode 5 and
by its own texture in modes 9 and 11. With no stage-2 texture bound, 0x352e46
compiles the texture variant to a CURRENT pass-through, so the blend skips the
final diffuse. A bound stage-2 texture would need a third sampler and still
stops with `d3d8:reflection-stage2-texture`.
Other fight chrome
(draw 0x28708b, FVF 0x112) swaps the coordinate sources: stage 0 samples the
environment through camera-space reflection texgen (TEXTURE0 matrix) and
stage 1 blends the mesh-UV0 base by its own alpha. The combiner is the same,
so the presenter runs stage-0 texgen alongside the mesh-UV blend. Texgen on
both stages (material modes 3, 4 and 5 with coordinate mode 1 on stages 0 and
1, byte +0xf through 0x7b018c) generates one camera-space reflection vector and
transforms it by each stage's own matrix: TEXTURE0 for stage 0, which
FUN_00289680 sets to the 0.5/-0.5 sphere-map scale and offset, and TEXTURE1
for stage 1, which the game leaves at Reset's identity (FUN_0034bc80) apart
from the FUN_00268de0/FUN_00269010 and FUN_0027b700/FUN_0027b900 save and
restore pairs. Stage 2's MODULATE(DIFFUSE,
CURRENT) is the blend's final diffuse multiply. Generated
coordinates on a draw without a normal stream (fight FVF 0x142) take the zero
inline normal: no SDK path writes v2 inline data (0x34aac0 sets only v0 and v9
through SetVertexData2f), so the reflection vector is the eye direction.
Unlit vertex diffuse and lit material diffuse retain their respective alpha
sources. Skinned reflection draws (including FVF 0x116) blend camera-space
positions and normal-matrix results with the position weights and final
remainder. The SDK upload at 0x3502d0 supplies each world-view matrix; with
NORMALIZENORMALS, 0x34b530 supplies signed cofactors before the final normalize.
Native pixel tests cover both endpoint weights and an equal blend. Exact
hardware rounding remains unverified. Vertex color lighting sources,
local-view specular, two-sided/spot lighting, other
combiners and multiple streams stop explicitly. The premultiplied blend clamps
its intermediate result before diffuse modulation and post-stage specular.
Screen-space offsets retain the SDK's biased shadows and snapshot API-relative
pixel translations. The presenter applies them after projection, including
blended and XYZRHW draws, without moving viewport clipping. Nonzero offsets on
programmable vertex draws remain guarded. SetScissors snapshots one allowed
rectangle in target pixels; the presenter scales its edges independently of
the draw's screen-space offset. Zero rectangles restores viewport clipping,
including pretransformed draws. Viewport, target and reset calls clear the
explicit rectangle as the SDK does. Multiple/exclusive rectangles and negative
edges remain guarded.

Programmable vertex draws snapshot the bound program, the constant shadow and
the viewport scale/offset constants c58/c59 that GetViewportOffsetAndScale
computes. They admit the position/normal/UV declaration and the sister
presenter's stage-0 texture select with an optional stage-1 alpha mask. As the
5849 combiner compiler does, an op that reads an unbound texture passes CURRENT
through. Other declarations, combiners and pixel shaders stop explicitly.
The projected character shadow uses program 0x7af4d0 (created by FUN_00279b30)
with a position/normal declaration and no texcoord stream, drawn as FVF 0x012.
FUN_0027a430 renders the shadow into a linear 512x256 A8R8G8B8 target.
FUN_0027a760 then projects that target onto the stage. The program writes
oT0 = (s, t, r, max(q, 0)) through MAC MAX and SLT, and the presenter
divides s and t by q per pixel in texel units. Stage 0 has color
SELECTARG1(TEXTURE) and alpha MODULATE(TEXTURE, TFACTOR), with the factor's
alpha. FUN_0027aa70 binds the same program but then resets the device
through FUN_00289680, which binds FVF 0x112, so it never draws with it.
Program 0x7ae238 (created by FUN_0026f850 and drawn by FUN_002718e0) declares
position, D3DCOLOR v3 and UV, drawn as FVF 0x142. It writes oPos from
c96-c99, oD0 = c112 and oT0 = v9. Program draws now select stage 0 per
channel, as fixed function does. This draw uses MODULATE(TEXTURE, oD0) for color,
MODULATE(TEXTURE, TFACTOR) for alpha, and its mipped texture's own mip filter
and LOD bias. The other program on that declaration (0x7add80, FUN_0026bd20)
and the stage effect's 0x7afb48 (FUN_0027ffc0) use LIT, SGE, oD1, oFog and
oT2/oT3. The translator follows D3D8 vs.1.1 LIT: y = max(x, 0), and z =
pow(y, w) with w clamped to +-127.9961 only when x and y are positive. oD1 is
passed to the pixel shader for the water combiner; oFog and oT2/oT3 also
reach it as separate fields. Program draws through the fixed-function stage
path still ignore fog and specular.
The stage effect's declaration 0x7afd0c (position, D3DCOLOR at 12, v9-v12 as
float2 at 16-40) binds as FVF 0x442, a 48-byte stride.

FUN_002807f0 draws the stage effect with the pixel shader that FUN_0027ffc0
builds: t0 PROJECT2D, t1-t3 BUMPENVMAP of t0 (texture modes 0x318c1, input
texture 0). Combiners 0-1 square T3.a twice, combiner 2 sets r0 = t2 *
(1 - T3.a), and combiner 3 multiplies by v0. The shader's own final combiner,
which 0x34f5f0 keeps because its words are nonzero, outputs lerp(fog.rgb, r0,
fog.a) with v0.a. T1 is never read, and stage 1 has no texture. The adapter
matches those words exactly and passes stage 0 (one of 17 animated 128x128
V8U8 maps, format 0x28 with COLORSIGN 0xe0000000), the stage-2 scene, the
stage-3 mask, and the stage-2/3 BUMPENVMAT00-11. The presenter reads the map
as R8G8_SNORM with U first and samples t2 and t3 at oT2/oT3 plus M n. Fog
follows 0x34f5f0's parameters: LINEAR stores bias end/(end - start) + 1 and
scale -1/(end - start), and mode 0 stores 1 and 1, so f = bias - 1 + scale *
oFog. With FogEnable off, FOG.a is taken as 1. Exponential fog, other maps or
signs, and other shader words stop. One session reached this draw on a stage
transition with stage state that matches FUN_002807f0, but its bound program
declared 0x112. Its 30-instruction pipeline fits 0x7af0f0 (FUN_002786e0), but
why that handle was bound there is unverified. The host draws the bound
program with this shader, and oT2/oT3 read as zero when it does not write them.

Water draws in an unanalyzed function at 0x26c980 with program 0x7add80
(FVF 0x142; it writes oPos, oD0, oD1, oFog, oT0, oT1 and oT3.xy) and the
pixel shader that FUN_0026bd20 builds: t0 PROJECT2D, t1 and t3 BUMPENVMAP of
t0, no t2 (texture modes 0x300c1). Its seven combiners compute s =
max(0, dot(expand v0, expand t3))^8, r0 = v1 * t1.a, r0 = (s - 0.5) +
(r0 - 0.5), r0 += t3.a * t1 and r0.a = t3.a * v0.a, with the stage effect's
final combiner. The draw binds one of the 17 V8U8 maps on stage 0, the water
reflection target (FUN_0026d610) on stage 1 and a filtered environment map
(0xe5b0a8) on stage 3, with matrices on stages 1 and 3, fog off and
SPECULARENABLE on. The adapter matches those words and reuses the bump path
with stage 1 in place of stage 2; the presenter computes the water combiner.
Register clamps are applied after each step. Water with SPECULARENABLE off
stops, since V1's source without it is unverified.
BGRA8 offscreen color targets and D24S8 custom depth targets bind through the
shared presenter; clears and draws
snapshot their storage descriptors. Binding counts retain parent textures,
and viewport extents follow the target. Front-buffer targets, offscreen sharing
of a supersampled main depth, and partial clears remain guarded. Synthetic draw
tests establish these contracts, not title acceptance.

SetRenderTarget (0x3447d0) programs the depth offset and pitch from the depth
header alone, so an offscreen color can test against the main depth. The fight
bloom source pass (0x2756f0) binds a 720x480 texture with the main depth,
clears color only and draws its glow objects behind the scene's occluders. The
adapter admits that pairing when the guest back buffer is not supersampled. The
host then binds its main depth at scale 1 without MSAA; scaled or multisampled
presentation draws such targets without depth.

KickPushBuffer publishes queued native commands. WaitForIdle drains them.
InsertFence returns an even sequence after draining; BlockOnFence drains too.
These synchronous fences order captured resource use without a console GPU.
They do not supply render-target readback or hardware timing parity.
PersistDisplay retains the native displayed image, and same-mode Reset replaces
the guest surface headers, viewport and scissors while preserving other state
and frame count. Reset clears depth/stencil only. Resizing and other display modes
need further bindings. The lifter handles WBINVD over coherent host memory;
Reset also accepts the 2x horizontal supersample type 0x2021 used by character
select. The game supplies 1440-wide back and depth headers and a 720-wide front
header; the guest device keeps the SDK's scale, physical size and swap-filter
fields, while the host target and vertex-shader viewport constants stay in
logical 720x480 units. PersistDisplay publishes an SDK-shaped saved surface
without host pixels. Line lists draw natively; pretransformed widths above
one pixel become screen-space quads. Untextured, unlit XYZ|DIFFUSE lines
(FVF 0x42) also support transformed widths and EdgeAntiAlias. The effect at
`0x254dd0` draws those lines with widths 0.3, 0.6, 0.9 and 1.2; the adapter
quantizes them to the SDK's 1/8-pixel steps. A geometry shader clips endpoints
against near/far/W planes and widens after projection in viewport pixels.
Fractional alpha coverage uses feathered strips, with culling disabled and
the ordinary pipeline restored after drawing. The trapezoidal filter and
square caps approximate hardware; exact line-filter calibration remains
unverified. Other transformed wide-line layouts and polygon edge smoothing
remain guarded. Native pixel tests cover fractional coverage, clipping,
both line directions and pipeline restoration.
Point lists draw as Xbox point sprites. With POINTSPRITEENABLE the 5849
combiner compiler (0x352a60) compiles stage 3 only, and that stage's texture
receives the sprite coordinates. The fight's particles (0x2345c0) use
POINTSCALEENABLE. The adapter expands each point to an eye-space quad that is
square in pixels and draws it with the projection alone. Its size is
Vh * Si / sqrt(A + B*De + C*De^2), clamped to [MIN, MAX], which is the D3D8
formula the SDK's point parameters (0x34ef10) encode. Stage 3 admits SELECTARG1
of texture or diffuse, or their MODULATE. Unscaled sizes, lit points, shaders and
projections that mix eye x/y into w stay guarded. Hardware size rounding and
frustum culling of whole points are not modelled: a quad that crosses the
screen edge is clipped instead.
there is no separate guest CPU cache or direct GPU consumer of guest bytes.

Leaving character select, the game (0x282770) widens the old headers, persists
the display, and Resets back to type 0x11. That Reset writes the same SDK fields
for the new mode: scales 1.0, flag 0x4000 clear, MultiSampleMode 0 and swap
filter 2. The game then restores the saved display into the 720-wide back
buffer with CopyRects; the supersampled direction uses a CPU loop instead.
The host's single main target still holds that frame, so CopyRects binds only
this restore. It stops if a color clear or draw has written the target since
the Swap before PersistDisplay. Other copies, such as the render-target
snapshot at 0x277670, need a presenter copy command.

Reset's mode set (0x34d58d) raises device+0x1de0. The next Swap (0x34af40)
then programs the mode and frees the persisted copy (0x34a600). Both mode
switches (0x282770, 0x282880) free their 0x175000-byte scratch arena, persist,
Reset, Swap, and only then allocate the arena again (0x282390). That arena holds
a matrix stack growing down from its end. If the persisted copy were kept, the
arena allocation would fail with guest memory full. The stack would then start
at 0x175000 and overwrite the switch table at 0x174530, a crash seen in fights.

Fixed-function stage 0 also admits D3DTSS_ALPHAKILL ENABLE (4), which discards
fragments whose sampled texel alpha is zero, and camera-space reflection
texgen (TCI 0x30000) with COUNT2. That texgen reuses the reflection inputs
with the TEXTURE0 matrix (0xce0) and writes UV0; without a normal stream it
uses a zero normal, as the stage-1 path does. COUNT2 over mesh UV0, which a
fight stage uses to scroll a texture, follows the texture-matrix upload
(0x34f7d0, case 0x220): s and t take the first two columns with the third row
as translation and q is 1, so UV0 becomes (u, v, 1, 0) times TEXTURE0.
Projected and other counts stop. When a lit world matrix is
singular, the SDK inverse (0x34b530) fails and uploads an unset matrix. The
adapter then uses zero normals, so only ambient and emissive light remain.
MultiSampleMode and MultiSampleRenderTargetMode (0x3477a0/0x3477d0) store
their guest shadow only. The game's 4X stencil pass on a one-sample back
buffer shades fewer pixels, but it covers every sample, as the host's
full-rate rendering does.

Any SDK API that is still unreplaced reserves console push-buffer space and
would then wait forever for GPU hardware. The reservation routine stops
instead and reports the calling API's return address.

SetRenderState_ShadowFunc (`0x346580`, void/stdcall value) preserves state
0x9c at `0x3554d0` without reserving console push-buffer space. Both direct
game callers (`0x27aa70` and `0x280f80`) set the SDK default `0x200`; the
effect state restore (`0x27b900`) can restore its saved shadow. Only that
default is bound. Other values stop, and depth-texture sampling remains
guarded by the draw adapter.

SetTextureState_BumpEnv (`0x346bc0`, stdcall stage/state/value) stores
BUMPENVMAT00 through BUMPENVLOFFSET (states 22-27) in the deferred stage array
`0x355060`, which the draw adapter reads; its NV2A method has no host use.
Draws do not consume those states yet. SetTextureState_BorderColor
(`0x346c30`, stdcall stage/color) likewise only shadows D3DTSS_BORDERCOLOR
(state 29) there; BORDER addressing itself still stops in the draw sampler
checks. A stage-1 combiner the draw path cannot
express stops with `d3d8:draw-texture-stages` and reports the color/alpha ops
and arguments of stages 0-2, the stage-3 op, the stage-2 binding, and the
stage-0/1 addressing, kill and coordinate sources.

The first Story fight (Stage 01) plays through a 180-second probe. Its stage
fades a light glare by occlusion. Game code `0x276d40` brackets a
draw with BeginVisibilityTest (`0x3457c0`, no arguments) and
EndVisibilityTest(index) (`0x345860`, stdcall), then inserts a WRITE callback
(`0x276b50`) that marks the index ready. Next frame it polls
GetVisibilityTestResult(index, &result, NULL) (`0x344450`, stdcall) while it
returns D3DERR_TESTINCOMPLETE. The visible/total ratio of two tests feeds the
fade, so only relative counts matter. The presenter wraps the draws in a
D3D11 occlusion query per END and resolves it on its worker without waiting.
Counts are normalized to guest target pixels and scaled by the back buffer's
supersample factor, as Xbox reports count samples. A pending poll publishes
queued work, as the SDK's fence kick does. No guest report slot is written.
Indices beyond the SDK's 64 report pages, an End without Begin, and a
requested report timestamp stop explicitly.
InsertCallback (`0x349e10`, stdcall type/callback/context) calls its cdecl
callback immediately: captured commands own their bytes and the host GPU
never writes guest memory, so READ and WRITE conditions already hold.

## Game tasks

Game logic runs as up to 24 XAPI fibers, resumed each frame by a task
scheduler. `fiber_adapter.c` replaces XAPI 5849 CreateFiber, DeleteFiber,
SwitchToFiber and ConvertThreadToFiber with native Windows fibers, ported
from DOA3. 5849 keeps DOA3's 16-byte fiber object but moves the TLS slots:
the current fiber is TLS block +0xC and the converted main fiber +0x10.
Guest FS refers to the PCR block. The guest-thread scheduler saves and
restores each thread's current fiber. Without this binding, the tasks never
run and the game only clears frames.

## Input API contract

`input_adapter.c` binds 5849 XInitDevices, device enumeration/changes,
XInputOpen/Close, capabilities, state and feedback. The shared controller model
retains packets, connection changes and per-port motor state. Packed guest
states have a four-byte packet followed by the 18-byte Xbox gamepad payload.
Feedback writes its final status and signals an optional native event handle
before returning. Rumble is synchronous; console USB allocation is host-owned.
An explicit XInputPoll binding remains unresolved.

The shared SDL host maps four gamepads and keeps a keyboard controller on
port zero. Enter is Start, arrows are the D-pad, Z/X are A/B, A/S are X/Y,
Q/W are White/Black and E/R are the triggers. Keyboard input requires window
focus. `--input-start-pulse-at <poll>` sends a one-sample Start edge through
ordinary controller sampling; it can be repeated up to four times. The bounded
probe accepts the same option. Input scripts do not alter game-owned state.

## Audio API contract

`dsound_api_adapter.c` owns the 5849 DirectSound device singleton and its
public interior handle. Native caller/buffer ownership replaces the SDK's
internal hardware voices while preserving visible reference results. The
shared XAudio2 backend owns copied PCM storage. Explicit mute keeps timing
available; host/device failures cannot report native readiness. Shutdown
requires the COM initialization owner thread.

The buffer adapter binds the 5849 descriptor and public handle layout to
the independent cursor model, PCM/Xbox ADPCM decoding and native output.
Owned and borrowed sample storage have separate lifetimes. Headroom changes
adjust effective volume; API argument cleanup is verified per entry.
Buffers pump synchronously from API polling and DoWork.
The inherited clock drops output spans after a stall over 100 ms while
advancing its cursor. Native endpoint consumption has an opt-in host test;
synthetic API and backend tests remain suitable for public CI.

`dsound_effects_image.c` decodes the encrypted DSP programs; CPU parameter
states are plaintext. The parser owns its payload/maps, validates all spans
and rejects nonempty Y storage. Its 256-map budget bounds allocation and
overlap checks. Fixed decoder initialization comes from the authenticated
executable at runtime. Synthetic vectors use independent initialization;
no retail programs, keys or state are included in public source.

`dsound_effects_adapter.c` publishes owned guest-readable descriptor, state
and scratch allocations. Failed parsing/allocation leaves the old image
intact. Immediate state writes update the published view; deferred writes
stay in shadow storage and flush with a dirty listener commit. Direct CPU
state edits survive unless explicitly overwritten by a deferred update.
This initial adapter requires an assigned reverb role. Unassigned roles are
rejected before publication, preserving the current native graph and image.

Native I3DL2 reverb runs on a stereo XAudio2 submix, using Microsoft's
[parameter converter](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2fx/nf-xaudio2fx-reverbconverti3dl2tonative).
The source keeps separate dry/wet matrices, per-route volumes and per-bin
headroom. Front stereo, wet bin 10 and muted other speaker routes are bound.
Recorded impulse tests establish actual wet processing, not Xbox bit parity;
the built-in converter ignores room rolloff. DSP programs are not executed.

Recording sends contain decimation, phase and scratch/DMA transfer semantics;
ordinary SourceVoice resampling is insufficient. Those sends remain guarded,
as do packet streams, scheduled StopEx, additional play flags and unsupported
DSP processing. This is an initial native FX subset.
Listener SetPosition/SetVelocity store their vectors and dirty bits in the
settings block. A commit with a dirty position or velocity recomputes every
3D voice.

Story fights play positioned effects through mono CTRL3D ADPCM buffers
(flags `0x40010`, created by game code at `0xfc516`). The six buffer setters
DOA2 calls are bound at their `IDirectSoundBuffer` thunks: SetMaxDistance
`0x365159`, SetMinDistance `0x36517d`, SetPosition `0x3651a1`, SetVelocity
`0x3651d6`, SetDistanceFactor `0x36520b` and SetRolloffFactor `0x36522f`.
Each takes the public handle (+0x1C) and stores into the 5849 3D block. The
adapter follows the 3D calculator (`0x365553`, `0x364236`) with the
full-HRTF routines DirectSoundUseFullHRTF installs (`0x366d1a`):
`-2000·log10(1 + rolloff·(d/min − 1))` distance volume held at max distance,
Doppler pitch `4096·log2(1 − v/342)` added to the base pitch and clamped, and
the game's own HRTF bank (`0x36de70`) and map (`0x37f430`). It renders through
DOA3's `dsound_hrtf.c`, with 5849 truncating quantization. Per-bin terms follow
`0x369565`. On stereo output the front/back split is off, so XTLK front bins
6/7 carry the HRTF ears and back bins 8/9 are silent. DOA2's 3D bins
`{6,8,7,9,2,10,3,22}` place center in slot 4, which enables the center term
(`0x366ae3`). XTLK bins fold to stereo by side, with back bins at −3 dB, and
skip the DSP crosstalk canceller. Bin 10 uses the existing I3DL2 send.
Deferred 3D values apply at once. MUTE3DATMAXDISTANCE, stereo 3D voices, cones,
modes, orientation, listener factors and I3DL2 sources remain unbound; DOA2
sets none of them.
Private notes hold actual graphics/audio startup and boot observations.

The 5849 GetTickCount boundary reads shared monotonic time and offers bounded
cooperative scheduler polling. D3D BlockUntilVerticalBlank and its two CRI
tail aliases use the native shared-vblank service. These platform seams let
the existing worker modules run; they do not force middleware load status.

## Remaining bring-up

1. Confirm the selected executable is the DOA2 portion using its launcher
   transition and code, and authenticate the disc revision. Preserve that
   evidence and the executable identity under `private/`.
2. Resolve the first observed unsupported startup instruction or service at
   its library boundary. Keep the whole-program lift as the temporary
   scaffold; fix translator defects in the lifter and regenerate.
3. Verify additional indirect targets and function bounds against the selected
   Ghidra program and private identity receipt when a run reaches them.
4. Complete native recording/crosstalk routing and CRI services, then XAPI
   input coverage. Verify native audio, movies, polling and worker scheduling
   against natural runs.
5. Bind additional D3D8 resources, shaders and draws through the semantic
   presenter. Verify title/menu entry and the first natural fight.

Online libraries and dashboard/update binaries are present on the disc.
The initial scope is offline DOA2; any online behavior needs its own design.
The verified XNetStartup boundary returns the native offline policy error
WSASYSNOTREADY without creating a console network stack. The game handles
its helper's failure; this seam does not report successful network startup.
PHY initialization reports hardware unavailable, and link queries return
disconnected flags, including after the game's failed network startup.

Record each observed frontier privately. A public model-test pass or a
completed lift alone is not a gameplay result.
