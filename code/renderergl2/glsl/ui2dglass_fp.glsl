uniform sampler2D u_DiffuseMap;		// the filters' result before the glass: the frost
uniform sampler2D u_GlassStateMap;	// the glass pass: this frame's state; the state pass: the frame before's

// A glass filter's two passes (tr_ui2d_layers.c), written from published
// math: u_GlassPass 0 the glass, the composite's last pass, into its
// destination; 1 its state, the two texels of a glass in the state
// texture, just before. The ideas are two open-source Flutter packages'
// (MIT): a hairline just inside the edge, a second, weaker lobe of light
// facing away, and light or dark chosen with two thresholds; and those of
// a later version of one and an Android library (Apache-2.0): a flat face
// with only the band at the rim bending, along the edge's normal. The notes
// name them
uniform int    u_GlassPass;
uniform vec4   u_Color;			// the composite's opacity
uniform float  u_ColorScale;		// tr.frameLight: the 2D draws display white at this

// the shape, in the 2D target's pixels, rows up: x, y, width and height,
// and its corners' radii by quadrant, bottom left, bottom right, top right
// and top left
uniform vec4   u_GlassBox;
uniform vec4   u_GlassRadii;
// the band's width and its mirror's, in pixels, within the limits for this
// shape (R_GlassLimits2D); the highlight's and hairline's strength
uniform vec4   u_GlassRim;
// a pixel taken back to where it lies on the shape, about its center, rows
// up: a row-major 2x2 matrix, the identity but while it wobbles
uniform vec4   u_GlassDeform;
uniform vec2   u_GlassLight;		// toward the light, a unit vector, rows up
uniform vec3   u_GlassDarkColor;
uniform vec3   u_GlassLightColor;
uniform vec4   u_GlassTint;		// the tint's least and most, its dark goal and its light goal
uniform int    u_GlassMode;		// 0 dark, 1 light, 2 either, by the state
// a press's glow: where, rows up, the pixels it fades over, its strength
uniform vec4   u_GlassGlow;
uniform vec2   u_InvTargetRes;		// 1 / the 2D target's size
uniform vec2   u_GlassStateCoord;	// the glass's first state texel's center; the second is a texel right
uniform float  u_GlassStateStep;	// a state texel's width
// the state pass's steps this frame: the slow brightness's weight, the
// tint's brightness's weight easing back and following a gentle
// brightening, and how far the look turns toward the one chosen
uniform vec4   u_GlassSmoothing;
uniform int    u_GlassStart;		// 1 when the glass wasn't drawn the frame before

varying vec2   var_TexCoord;		// the glass pass's quad covers the target: p / its size

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
// a flash: the backdrop this much brighter than the frame before (darker,
// under a light tint), which the tint meets at once
const float FLASH = 0.2;
// the slow brightness's thresholds: up to light past one, down to dark
// again below the other
const float LIGHT_ABOVE = 0.62, DARK_BELOW = 0.45;

// a texel's color in display units, as if opaque, which the backdrop is
vec3 Straight(vec4 texel)
{
	return texel.a > 0.0 ? texel.rgb / (texel.a * u_ColorScale) : vec3(0.0);
}

// q's signed distance from a box around 0 with corners of radius r, and the
// box's outward normal there
float BoxDistance(vec2 q, vec2 halfSize, float r, out vec2 n)
{
	vec2 e = abs(q) - halfSize + r;
	vec2 s = vec2(q.x < 0.0 ? -1.0 : 1.0, q.y < 0.0 ? -1.0 : 1.0);

	if (e.x > 0.0 && e.y > 0.0)
		n = normalize(e) * s;
	else
		n = e.x > e.y ? vec2(s.x, 0.0) : vec2(0.0, s.y);
	return length(max(e, 0.0)) + min(max(e.x, e.y), 0.0) - r;
}

// the depth what the band shows at depth t lies at, below 0 outside the
// edge: within the mirror m, what's outside mirrored in and squeezed; past
// it, the inside from the edge to the face at the band's width b, squeezed
// most next to the mirror (R_GlassLimits2D)
float GlassDepth(float t, float m, float b)
{
	float u = (b - t) / max(b - m, 1e-4);

	if (t < m)
		return -GLASS2D_SQUEEZE * t;
	return t - m * u * u * u;
}

// a value from 0 to 1 in two 8-bit channels, and back
vec2 Pack16(float v)
{
	float hi = floor(v * 255.0) / 255.0;

	return vec2(hi, (v - hi) * 255.0);
}

float Unpack16(vec2 c)
{
	return c.x + c.y / 255.0;
}

float FrostLuma(vec2 at)
{
	return dot(Straight(texture2D(u_DiffuseMap, at)), LUMA);
}

// how much of a tint brings a backdrop whose brightness is y to its goal
float TintAmount(float y, vec3 tint, float goal)
{
	float den = y - dot(tint, LUMA);

	den = den < 0.0 ? min(den, -0.02) : max(den, 0.02);
	return clamp((y - goal) / den, u_GlassTint.x, u_GlassTint.y);
}

void main()
{
	vec2 halfSize = u_GlassBox.zw * 0.5;
	vec4 s0 = texture2D(u_GlassStateMap, u_GlassStateCoord);
	vec4 s1 = texture2D(u_GlassStateMap, u_GlassStateCoord + vec2(u_GlassStateStep, 0.0));

	if (u_GlassPass == 1)
	{
		// the frost's brightness at five points of the shape
		vec2 c = (u_GlassBox.xy + halfSize) * u_InvTargetRes;
		vec2 o = halfSize * 0.5 * u_InvTargetRes;
		float y = (FrostLuma(c) + FrostLuma(c + o) + FrostLuma(c - o) +
			FrostLuma(c + vec2(o.x, -o.y)) + FrostLuma(c + vec2(-o.x, o.y))) * 0.2;
		float slow = y, tintY = y;
		float light = y >= 0.55 ? 1.0 : 0.0;
		float look = light;

		if (u_GlassStart == 0 && s0.a > 0.5)
		{
			// the slow brightness only chooses light or dark, with two
			// thresholds, so a flash never flips it
			slow = Unpack16(s0.rg);
			slow += (y - slow) * u_GlassSmoothing.x;
			light = s0.b > 0.5 ? step(DARK_BELOW, slow) : step(LIGHT_ABOVE, slow);

			// the tint's brightness follows where more tint is needed,
			// brighter under a dark tint and darker under a light one: at
			// once for a flash, the backdrop's jump since the frame before,
			// and quickly for a gentler change; and eases back otherwise
			tintY = Unpack16(s1.rg);
			bool dark = u_GlassMode == 2 ? light < 0.5 : u_GlassMode == 0;
			float before = (s0.a - 0.6) / 0.4;
			float more = dark ? y - tintY : tintY - y;
			float jump = dark ? y - before : before - y;
			tintY += (y - tintY) * (more > 0.0 ? (jump > FLASH ? 1.0 : u_GlassSmoothing.z) : u_GlassSmoothing.y);

			// the look, 0 dark to 1 light, turns toward the choice at an
			// even pace, so a flip cross-fades
			look = Unpack16(s1.ba);
			look += clamp(light - look, -u_GlassSmoothing.w, u_GlassSmoothing.w);
		}

		// which of the glass's two texels this is; the first keeps this
		// frame's brightness, for the next frame's flash, from 0.6 up, so it
		// also says the texel is set
		bool second = gl_FragCoord.x - floor(gl_FragCoord.x / 2.0) * 2.0 >= 1.0;
		gl_FragColor = second ? vec4(Pack16(tintY), Pack16(look)) :
			vec4(Pack16(slow), light, 0.6 + 0.4 * clamp(y, 0.0, 1.0));
		return;
	}

	// the shape's own edge, and the band's shape, rounded at least as much as
	// the band is wide, so a square corner bends smoothly around instead of
	// creasing along its diagonal; a wobble's deformation taken back first,
	// and the band's normal brought forward through it
	vec2 q = gl_FragCoord.xy - (u_GlassBox.xy + halfSize);
	q = vec2(dot(u_GlassDeform.xy, q), dot(u_GlassDeform.zw, q));
	float minHalf = min(halfSize.x, halfSize.y);
	float r = min(q.x < 0.0 ? (q.y < 0.0 ? u_GlassRadii.x : u_GlassRadii.w) : (q.y < 0.0 ? u_GlassRadii.y : u_GlassRadii.z),
		minHalf);
	float rim = u_GlassRim.x;
	vec2 n, nb;
	float d = BoxDistance(q, halfSize, r, n);
	float db = BoxDistance(q, halfSize, min(max(r, rim), minHalf), nb);
	float coverage = clamp(0.5 - d, 0.0, 1.0);
	// how far round a corner: sin 2a for the normal's angle a, 0 along a
	// side and 1 mid-corner, where the mirror widens to GLASS2D_CORNER times
	float mirror = u_GlassRim.y * (1.0 + (GLASS2D_CORNER - 1.0) * 2.0 * abs(nb.x * nb.y));
	nb = normalize(vec2(u_GlassDeform.x * nb.x + u_GlassDeform.z * nb.y, u_GlassDeform.y * nb.x + u_GlassDeform.w * nb.y));

	// in the band each pixel reads the frost at the depth GlassDepth gives
	// for its own, along the normal; on the face, x 0, it reads its own place
	float t = max(-db, 0.0);
	float x = 1.0 - min(t / max(rim, 1e-4), 1.0);
	float s = t < rim ? GlassDepth(t, mirror, rim) : t;
	vec2 at = var_TexCoord + nb * (t - s) * u_InvTargetRes;
	vec3 c = Straight(texture2D(u_DiffuseMap, at));

	// the tint: as much as brings the backdrop's average from its tint
	// brightness to the goal, the dark look and the light one, mixed by the
	// state's look, which eases from one to the other as the choice flips
	float look = u_GlassMode == 2 ? smoothstep(0.0, 1.0, Unpack16(s1.ba)) : (u_GlassMode == 1 ? 1.0 : 0.0);
	float y = Unpack16(s1.rg);
	float darkAmount = TintAmount(y, u_GlassDarkColor, u_GlassTint.z);
	float lightAmount = TintAmount(y, u_GlassLightColor, u_GlassTint.w);
	c = c * (1.0 - mix(darkAmount, lightAmount, look)) +
		mix(darkAmount * u_GlassDarkColor, lightAmount * u_GlassLightColor, look);

	// the highlight, lit from a fixed side: a line just inside the edge,
	// strongest facing the light and weaker facing away, and a glow over the
	// band; and a press's glow, on the rim most, fading with its distance
	float l = dot(nb, u_GlassLight);
	float lit = max(l, 0.0), away = max(-l, 0.0);
	float lobe = lit * lit * lit + 0.45 * away * away * away;
	float band = exp(-(-d - 1.5) * (-d - 1.5) / 1.5) + 0.2 * x;
	vec2 g = (gl_FragCoord.xy - u_GlassGlow.xy) / u_GlassGlow.z;
	float glow = u_GlassGlow.w * exp(-dot(g, g)) * (0.3 + 0.7 * min(band, 1.0));
	c += (1.0 - c) * clamp(u_GlassRim.z * lobe * band + glow, 0.0, 1.0);

	// a dark hairline just inside the edge, over the light, so the edge
	// stays drawn while a press's glow brightens the rim around it
	c *= 1.0 - u_GlassRim.w * (1.0 - smoothstep(0.0, 1.5, -d));

	// premultiplied, by the shape's coverage, at the 2D's scale
	gl_FragColor = vec4(clamp(c, 0.0, 1.0) * u_ColorScale, 1.0) * coverage * u_Color;
}
