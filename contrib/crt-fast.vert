#version 100

/*
 * Vertex half of crt-fast, for use as -S contrib/crt-fast.frag -V
 * contrib/crt-fast.vert. The same picture as crt.frag, rearranged for GPUs
 * that cannot afford it per pixel, such as the Mali-400/450.
 *
 * Everything in crt.frag that depends only on where a pixel is, or only on
 * the uniforms, is worked out here, once per mesh vertex, and interpolated:
 * the curvature, the wobble, every texture coordinate and the whole colour
 * grade. Kage's mesh has a vertex every 8 virtual pixels across and 4 down,
 * which keeps the interpolated curvature within about a hundredth of a pixel
 * of the real one, and the wobble within a few hundredths.
 *
 * On the Lima compiler this takes the fragment shader from 93 instruction
 * words with register spills to 45 without, and the picture is closer to
 * crt.frag's exact arithmetic than crt.frag itself manages at mediump.
 */

attribute vec2 cg_position;

uniform highp vec2 texturesize;
uniform float scanlinerepeat;
uniform mediump float evil;
uniform mediump float roseglasses;
uniform float wobblephase;
uniform vec2 noisescale;
uniform vec2 noiseoffset;

varying highp vec2 screenuv;
varying highp vec2 pixeluv;
varying highp vec2 wobblyuv;
varying highp vec2 bleedr;
varying highp vec2 bleedrw;
varying highp vec2 bleedb;
varying highp vec2 bleedbw;
varying highp vec2 scanlinecoord;
varying highp vec2 noisecoord;
varying mediump vec2 blend;
varying mediump vec3 gradescale;
varying mediump vec3 gradeoffset;
varying mediump vec3 noiseweight;

const float curvature = 1.6;
const float wobblefrequency = 40.0;

/* crt.frag's curve(), with pow(x, 2.0) written as x * x. */
vec2 curve(vec2 uv)
{
	uv = (uv - 0.5) * 1.1;
	uv *= curvature;
	uv.x *= 1.0 + (uv.y * uv.y) / 16.0;
	uv.y *= 1.0 + (uv.x * uv.x) / 9.0;
	uv /= curvature;
	uv += 0.5;
	return uv * 0.92 + 0.04;
}

void main()
{
	/* crt.frag curves both the pixel centre and, for sampling, the pixel's
	 * top left corner. */
	screenuv = curve(cg_position);
	pixeluv = curve(cg_position - 0.5 / texturesize);

	float distortion = mix(0.08, 0.24, evil);
	vec2 wobble = vec2(sin(screenuv.y * wobblefrequency + wobblephase) * distortion * 0.02, 0.0);
	vec2 offset = vec2(mix(1.8, 3.6, evil) / texturesize.x, 0.0);

	wobblyuv = pixeluv + wobble;
	bleedr = pixeluv + offset;
	bleedrw = wobblyuv + offset;
	bleedb = pixeluv - offset;
	bleedbw = wobblyuv - offset;

	scanlinecoord = vec2(cg_position.x, cg_position.y * scanlinerepeat);
	noisecoord = screenuv * noisescale + noiseoffset;

	blend = vec2(distortion, mix(0.5, 1.0, evil));

	/*
	 * After the 25% grey mix, the rest of crt.frag's grade - 10% purple,
	 * the rose gradient, the noise and the red-only creepiness - is affine
	 * in the colour, so it folds into
	 * scale * colour + offset + noise * weight.
	 */
	vec3 keep = vec3(1.0, 1.0 - evil, 1.0 - evil);
	vec3 rose = mix(vec3(0.7, 0.0, 0.5), vec3(0.0, 1.0, 1.0), pixeluv.y - 0.4);
	gradescale = keep * (1.0 - roseglasses) * 0.9;
	noiseweight = keep * mix(0.025, 0.7, evil);
	gradeoffset = keep * ((1.0 - roseglasses) * 0.1 * vec3(1.1, 0.75, 1.1) + roseglasses * rose) - 0.5 * noiseweight;

	gl_Position = vec4(cg_position * 2.0 - 1.0, 0.0, 1.0);
}
