#version 100
precision mediump float;

/*
 * Fragment half of crt-fast; see crt-fast.vert. What is left per pixel is the
 * sampling, the quantisation, the scanlines and one affine grade.
 *
 * The noise always comes from noisetex, as in crt.frag on GPUs without highp
 * fragment arithmetic.
 */

uniform sampler2D tex;
uniform sampler2D texnearest;
uniform sampler2D noisetex;
uniform sampler2D scanlinetex;

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

const float colourquantum = 127.0;
const float intensity = 0.9;

void main()
{
	/* crt.frag snaps to a texel centre so that the linear filter returns
	 * that texel untouched; a nearest-filtered view does the same in the
	 * texture unit, at full precision and for free. */
	vec3 sharp = texture2D(texnearest, pixeluv).rgb;
	vec3 wobbly = texture2D(tex, wobblyuv).rgb;
	/* crt.frag reads green, not blue, for the undistorted blue fringe. */
	vec4 fringe = vec4(texture2D(tex, bleedr).r, texture2D(tex, bleedrw).r, texture2D(tex, bleedb).g,
			   texture2D(tex, bleedbw).b);

	sharp = floor(sharp * colourquantum + 0.5) / colourquantum;
	wobbly = floor(wobbly * colourquantum + 0.5) / colourquantum;
	fringe = floor(fringe * colourquantum + 0.5) / colourquantum;

	vec3 colour = mix(sharp, wobbly, blend.x);
	colour.rb = mix(colour.rb, mix(fringe.xz, fringe.yw, blend.x), blend.y);

	colour *= mix(1.0, 0.7, texture2D(scanlinetex, scanlinecoord).r * intensity);
	colour = mix(colour, vec3(dot(colour, vec3(0.3, 0.59, 0.11))), 0.25);
	colour = gradescale * colour + gradeoffset + texture2D(noisetex, noisecoord).r * noiseweight;

	bool inside = all(greaterThanEqual(screenuv, vec2(0.0))) && all(lessThanEqual(screenuv, vec2(1.0)));
	gl_FragColor = inside ? vec4(colour, 1.0) : vec4(0.0);
}
