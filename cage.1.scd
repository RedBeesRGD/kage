cage(1)

# NAME

cage - a Wayland kiosk compositor

# SYNOPSIS

*cage* [options...] [--] [_application_...]

# DESCRIPTION

Cage runs a single, maximized application. Cage can run multiple applications,
but only a single one is visible at any point in time. User interaction and
activities outside the scope of the running application are prevented.

# OPTIONS

*-d*
	Don't draw client side decorations when possible.

*-D*
	Enable debug logging.

*-h*
	Show the help message.

*-m* <mode>
	Set the multi-monitor behavior. Supported modes are:
	*last* Cage uses only the last connected monitor.
	*extend* Cage extends the display across all connected monitors.

*-r* <width>x<height>
	Publish a single virtual output of exactly _width_x_height_ to clients,
	regardless of the physical outputs and their EDID, and scale its frames
	onto every physical output. Views, the cursor and absolute input all live
	in the virtual coordinate space.

	The physical outputs keep running at the mode they would have used
	anyway. How each frame is placed on them is chosen with *-f*.

*-f* <mode>
	How the virtual output is presented on each physical output. Only
	meaningful together with *-r*.

	_exact_ scales by the largest whole number of times that fits, centred.
	Every source pixel becomes an identical square block, so the picture can
	be reproduced exactly and the remainder is black. This is the default.

	_fit_ scales to the largest rectangle that keeps the virtual output's
	aspect ratio, centred. One axis is filled exactly and the other keeps a
	border; nothing is distorted, but the scale is not a whole number.

	_fill_ stretches the frame to the whole output. No border, at the cost of
	distorting the aspect ratio whenever the virtual output and the physical
	one disagree.

*-F* <filter>
	Which filter to scale with. Only meaningful together with *-r*.

	_auto_ uses nearest-neighbour for *-f* _exact_ and interpolation for the
	other two, on the grounds that only a whole-number scale can be
	reproduced exactly. This is the default.

	_nearest_ never interpolates, whatever the scale. Pixel art stretched to
	fill a screen often wants this even though the scale is fractional.

	_bilinear_ always interpolates, whatever the scale.

	Where the display controller can scale a plane and be told which filter
	to use, every combination is performed by it rather than the GPU.

*-k* <n>
	With *-f* _exact_, never scale by more than _n_, however much room the
	physical output has. Defaults to 3. Only meaningful together with *-r*.

*-P*
	Expose the physical outputs to clients. In upscale mode they are normally
	invisible, which also puts them out of reach of screen capture; this flag
	makes them capturable. It is a debugging aid: with it, clients see
	outputs they are not meant to see.

*-s*
	Allow VT switching

*-v*
	Show the version number and exit.

# ENVIRONMENT

_DISPLAY_
	If compiled with Xwayland support, this will be set to the name of the
	X display used for Xwayland. Otherwise, probe the X11 backend.

_WAYLAND_DISPLAY_
	Specifies the name of the Wayland display that Cage is running on.

_XCURSOR_PATH_
	Directory where cursors are located.

_XCURSOR_SIZE_
	Specifies the configured cursor size.

_XCURSOR_THEME_
	Specifies the configured cursor theme.

_XKB_DEFAULT_RULES_, _XKB_DEFAULT_MODEL_, _XKB_DEFAULT_LAYOUT_,
_XKB_DEFAULT_VARIANT_, _XKB_DEFAULT_OPTIONS_
	Configures the xkb keyboard settings. See *xkeyboard-config*(7).

# SEE ALSO

*xkeyboard-config(7)*

# BUGS

Report bugs at https://github.com/cage-kiosk/cage

# AUTHORS

Jente Hidskes <dev@hjdskes.nl>
