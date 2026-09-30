#version 100
precision mediump float;

uniform sampler2D tex;
uniform sampler2D noisetex;
uniform float texturewidth;
uniform float evil;
uniform float roseglasses;
uniform float wobblephase;
uniform vec2 noisescale;
uniform vec2 noiseoffset;
uniform sampler2D scanlinetex;
#ifdef GL_FRAGMENT_PRECISION_HIGH
	uniform float time;

	highp float rand(vec2 co)
	{
		highp float a = 12.9898;
		highp float b = 78.233;
		highp float c = 43758.5453;
		highp float dt= dot(co.xy ,vec2(a,b));
		highp float sn= mod(dt,3.14);
		return fract(sin(sn) * c);
	}
#endif

varying vec2 uvs;
varying vec2 scanlinecoord;

float intensity = 0.9; //0.9
vec4 tempcolour;
float scanlines;
float scanlinemask;
float gray;
float colourbleed; //4.0 to 8.0
float bleedblending; //0.5 to 1.0
vec2 offset;
float aberred;
float aberblue;
vec3 aberrated;
float noiseintensity; //0.025 to 0.7
float noise;
float wobblefrequency = 40.0; //40
float distortion; //0.08 to 0.24
float wobble;
vec2 distorteduvs;
float curvature = 1.6; //1.6
vec2 curveduvsscreen;
float curvaturesign;
vec2 newuvs;
vec2 curveduvspixel;
float creepiness; // 0 to 1
vec4 quantcolour;
vec4 quantdistcolour;
float colourquantum = 127.0;

vec2 curve( vec2 uv )
	{
	curvaturesign = 1.0;
	//curvaturesign = -1.0;
	//curvaturesign = 0.0;

	uv = (uv - 0.5);
	uv *= vec2(1.1 - 0.1 * (curvaturesign-1.0), 1.1 - 0.1 * (curvaturesign-1.0));
	uv *= curvature;
	uv.x *= 1.0 + (pow((abs(uv.y) / 4.0), 2.0)) * curvaturesign;
	uv.y *= 1.0 + (pow((abs(uv.x) / 3.0), 2.0)) * curvaturesign;
	uv /= curvature;
	uv  += 0.5;
	uv =  uv *0.92 + 0.04;
	return uv;
	}

float quantise(float normal, float maximum)
	{
	normal = normal * maximum;
	normal = floor(normal + 0.5);
	normal = normal / maximum;
	return normal;
	}

vec2 texelcentre(vec2 uv)
	{
	//snaps to the centre of a 640x480 texel, so the main image stays crisp with linear filtering
	return (floor(uv * vec2(640.0, 480.0)) + 0.5) / vec2(640.0, 480.0);
	}

void main(){
	colourbleed = mix(1.8, 3.6, evil);
	bleedblending = mix(0.5, 1.0, evil);
	offset = vec2(colourbleed / texturewidth, 0.0);
	noiseintensity = mix(0.025, 0.7, evil);
	distortion = mix(0.08, 0.24, evil);
	creepiness = mix(0.0, 1.0, evil);

	newuvs = uvs;
	curveduvsscreen = curve(newuvs);
	newuvs.x = floor(newuvs.x*640.0)/640.0;
	newuvs.y = floor(newuvs.y*480.0)/480.0;
	curveduvspixel = curve(newuvs);

	//distort
	wobble = sin(curveduvsscreen.y * wobblefrequency + wobblephase) * distortion * 0.02;
	distorteduvs = curveduvspixel + vec2(wobble,0.0);

	quantcolour = vec4(texture2D(tex, texelcentre(curveduvspixel)).rgb, 1.0);
	quantcolour.rgb = vec3(quantise(quantcolour.r, colourquantum), quantise(quantcolour.g, colourquantum), quantise(quantcolour.b, colourquantum));
	quantdistcolour = vec4(texture2D(tex, distorteduvs).rgb, 1.0);
	quantdistcolour.rgb = vec3(quantise(quantdistcolour.r, colourquantum), quantise(quantdistcolour.g, colourquantum), quantise(quantdistcolour.b, colourquantum));

	tempcolour.rgb = mix(quantcolour.rgb, quantdistcolour.rgb, distortion);

	//chromatic abberation
	aberred = texture2D(tex, curveduvspixel + offset).x;
	aberred = mix(quantise(aberred,colourquantum), quantise(texture2D(tex, distorteduvs + offset).r,colourquantum), distortion);
	aberblue = texture2D(tex, curveduvspixel - offset).y; //note: .y is green, the line below uses .b
	aberblue = mix(quantise(aberblue,colourquantum), quantise(texture2D(tex, distorteduvs - offset).b,colourquantum), distortion);

	aberrated = vec3(aberred, tempcolour.g, aberblue);
	tempcolour.rgb = mix(tempcolour.rgb, aberrated, bleedblending);

	//apply scanlines
	scanlines = texture2D(scanlinetex, scanlinecoord).r; //one scanline per repeat of the texture, already 0 to 1
	scanlinemask = mix(1.0, 0.7, scanlines * intensity);
	tempcolour.rgb *= scanlinemask;

	//gray colour grading
	gray = dot(tempcolour.rgb, vec3(0.3, 0.59, 0.11));
	tempcolour.rgb = mix(tempcolour.rgb, vec3(gray, gray, gray), 0.25);

	//purple colour grading
	tempcolour.rgb = mix(tempcolour.rgb, vec3(1.1, 0.75, 1.1), 0.1);

	//mmm nostalgia
	tempcolour.rgb = mix(tempcolour.rgb, mix(vec3(0.7, 0.0, 0.5), vec3(0.0,1.0,1.0), curveduvspixel.y - 0.4), roseglasses);

	//noise
	#ifdef GL_FRAGMENT_PRECISION_HIGH
		noise = (rand(curveduvsscreen * time * 0.5 + time) - 0.5) * noiseintensity;
	#else
		noise = (texture2D(noisetex, curveduvsscreen * noisescale + noiseoffset).r - 0.5) * noiseintensity;
	#endif
	tempcolour.rgb += noise;

	tempcolour.g = tempcolour.g * (1.0 - creepiness);
	tempcolour.b = tempcolour.b * (1.0 - creepiness);

	tempcolour.a = 1.0;
	tempcolour *= float(!(curveduvsscreen.x>1.0 || curveduvsscreen.x<0.0 || curveduvsscreen.y>1.0 || curveduvsscreen.y<0.0));

	gl_FragColor = tempcolour;
}