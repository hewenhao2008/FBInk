/*
	FBInk: FrameBuffer eInker, a tool to print text & images on eInk devices (Kobo/Kindle)
	Copyright (C) 2018 NiLuJe <ninuje@gmail.com>

	----

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Build w/ ${CROSS_TC}-gcc -O3 -ffast-math -ftree-vectorize -funroll-loops -march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=hard -mthumb -D_GLIBCXX_USE_CXX11_ABI=0 -pipe -fomit-frame-pointer -frename-registers -fweb -flto=9 -fuse-linker-plugin -Wall -Wextra -s tools/button_scan.c -o button_scan

// NOTE: Don't do this at home. This is a quick and rough POC to have some fun w/
//       https://www.mobileread.com/forums/showpost.php?p=3731967&postcount=12
//       No-one should ever, ever, ever include internal headers/code, I'm just re-using bits of private API to isolate this POC.
#include "../fbink.c"
#include "../fbink_device_id.c"

// FBInk always returns negative values on failure
#define ERRCODE(e) (-(e))

#include <linux/input.h>

// c.f., https://github.com/koreader/koreader-base/pull/468/files
#define SEND_INPUT_EVENT(t, c, v)                                                                                        \
	({                                                                                                               \
		gettimeofday(&ev.time, NULL);                                                                            \
		ev.type  = (t);                                                                                          \
		ev.code  = (c);                                                                                          \
		ev.value = (v);                                                                                          \
		write(ifd, &ev, sizeof(ev));                                                                             \
	})

// Application entry point
int
    main(int argc __attribute__((unused)), char* argv[] __attribute__((unused)))
{
	FBInkConfig fbink_config = { 0 };

	// Open framebuffer and keep it around, then setup globals.
	int fbfd = -1;
	if (ERRCODE(EXIT_FAILURE) == (fbfd = fbink_open())) {
		fprintf(stderr, "Failed to open the framebuffer, aborting . . .\n");
		return ERRCODE(EXIT_FAILURE);
	}
	if (fbink_init(fbfd, &fbink_config) == ERRCODE(EXIT_FAILURE)) {
		fprintf(stderr, "Failed to initialize FBInk, aborting . . .\n");
		return ERRCODE(EXIT_FAILURE);
	}

	// mmap the fb if need be...
	if (!g_fbink_isFbMapped) {
		if (memmap_fb(fbfd) != EXIT_SUCCESS) {
			return ERRCODE(EXIT_FAILURE);
		}
	}

	// Wheee! (Default to the proper value on 32bpp FW)
	FBInkColor button_color = { 0xD9, 0xD9, 0xD9 };

	// And handle yet another bit of 16bpp weirdness...
	// NOTE: There *may* be a rounding/conversion error somewhere...
	//       I can vouch for get_pixel_RGB565's accuracy,
	//       and set_pixel_RGB565 looks straightforward enough, so, err, I blame Kobo? :D.
	if (fbink_is_fb_quirky()) {
		button_color.r = 0xDE;
		button_color.g = 0xDB;
		button_color.b = 0xDE;
	}

	FBInkColor         color = { 0U };
	unsigned short int x;
	unsigned short int y;
	unsigned short int j;
	FBInkCoordinates   coords              = { 0U };
	unsigned short int consecutive_matches = 0U;
	unsigned short int match_count         = 0U;
	unsigned short int matched_lines       = 0U;
	bool               gotcha              = false;
	FBInkCoordinates   match_coords        = { 0U };

	// DEBUG: Fake a Glo ;).
	/*
	viewWidth  = 758U;
	viewHeight = 1024U;
	*/

	// Centralize the various thresholds we use...
	// NOTE: Depending on the device's DPI & resolution, a button takes between 17% and 20% of the screen's width.
	//       Possibly less on larger resolutions, and more on smaller resolutions, so try to handle everyone in one fell swoop.
	unsigned short int min_target_pixels = (0.125 * viewWidth);
	unsigned short int max_target_pixels = (0.25 * viewWidth);

	// Recap the various settings as computed for this screen...
	fprintf(stderr, "Button color is expected to be #%hhx%hhx%hhx\n", button_color.r, button_color.g, button_color.b);
	fprintf(stderr,
		"We need to match two buttons each between %hu and %hu pixels wide!\n",
		min_target_pixels,
		max_target_pixels);

	// Only loop on the bottom half of the screen, to save time: buttons are always going to be below that.
	// In the same vein, don't loop 'til the end of the line or the bottom of the screen, limit the search to
	// roughly the area where there's a chance to find the buttons...
	unsigned short int min_height = (viewHeight / 2U);
	unsigned short int max_height = (0.90 * viewHeight);
	unsigned short int max_width  = (0.80 * viewWidth);

	for (y = min_height; y < max_height; y++) {
		if (match_count == 2) {
			// It looks like we found the buttons on the previous line, we can stop looping.
			break;
		}

		// New line, reset counters
		consecutive_matches = 0U;
		match_count         = 0U;

		for (x = 0U; x < max_width; x++) {
			coords.x = x;
			coords.y = y;

			// Handle 16bpp rota (hopefully applies in Nickel, too ;D)
			(*fxpRotateCoords)(&coords);
			(*fxpGetPixel)(&coords, &color);

			if (color.r == button_color.r && color.g == button_color.g && color.b == button_color.b) {
				// Found a pixel of the right color for a button...
				consecutive_matches++;
			} else {
				// Pixel is no longer part of a button...
				if (consecutive_matches >= min_target_pixels &&
				    consecutive_matches <= max_target_pixels) {
					// But we've just finished matching enough pixels in a row to assume we found a button!
					match_count++;
					fprintf(stderr,
						"End of match %hu after %hu consecutive matche @ (%hu, %hu)\n",
						match_count,
						consecutive_matches,
						x,
						y);
					// We only care about the second button, Connect :).
					if (match_count == 2) {
						match_coords.y = y;
						// Last good pixel was the previous one, store that one ;).
						match_coords.x = x - 1;
						// We've got the top-right corner of the Connect button, stop looping.
						break;
					}
				} else {
					if (consecutive_matches > 0U) {
						// And we only matched a few stray pixels of the right color before, not a button.
						fprintf(
						    stderr,
						    "Failed end of match after %hu consecutive matches @ (%hu, %hu)\n",
						    consecutive_matches,
						    x,
						    y);
					}
				}
				// In any case, wrong color, reset the counter.
				consecutive_matches = 0U;
			}
		}
	}

	// If we've got a button corner stored in there, we're not quite done yet...
	if (match_coords.x != 0 && match_coords.y != 0) {
		coords.x = match_coords.x;
		for (j = match_coords.y; j < max_height; j++) {
			coords.y = j;

			(*fxpRotateCoords)(&coords);
			(*fxpGetPixel)(&coords, &color);

			if (color.r == button_color.r && color.g == button_color.g && color.b == button_color.b) {
				// Found a pixel of the right color for a button...
				matched_lines++;
			} else {
				// Pixel is no longer part of a button,
				// which likely means we've now hit the bottom-right of the Connect button.
				// Backtrack from half the height & half the width to get the center of the button.
				match_coords.y = j - (matched_lines / 2U);
				match_coords.x = x - (consecutive_matches / 2U);
				// And we're done!
				gotcha = true;
				break;
			}
		}
	}

	if (gotcha) {
		fprintf(stderr, "Match! :)\n");

		// The touch panel has a fixed origin that differs from the framebuffer's... >_<".
		rotate_coordinates(&match_coords);
		fprintf(stdout, "x=%hu, y=%hu\n", match_coords.x, match_coords.y);

		// NOTE: The H2O²r1 is a special snowflake, input is rotated 90° in the *other* direction
		//       (i.e., origin at the bottom-left instead of top-right).
		//       Hopefully that doesn't apply to the fb itself, too...
		fprintf(stdout,
			"H2O²r1: x=%hu, y=%hu\n",
			(unsigned short int) (viewHeight - match_coords.x - 1),
			(unsigned short int) (viewWidth - match_coords.y - 1));

		// Press it if TOUCH_ME is in the env...
		if (getenv("TOUCH_ME") != NULL) {
			fprintf(stderr, "Pressing the button . . .\n");
			struct input_event ev;
			int                ifd = -1;
			ifd                    = open("/dev/input/event1", O_WRONLY | O_NONBLOCK);
			if (ifd == -1) {
				fprintf(stderr, "Failed to open input device! \n");
				return ERRCODE(EXIT_FAILURE);
			}

			// NOTE: May not be completely right for every model... (OK on H2O)
			//       Double-check on your device w/ hexdump -x /dev/input/event1 (or -d if you prefer decimal).
			SEND_INPUT_EVENT(EV_ABS, ABS_MT_TRACKING_ID, 1);
			SEND_INPUT_EVENT(EV_ABS, ABS_MT_TOUCH_MAJOR, 1);
			SEND_INPUT_EVENT(EV_ABS, ABS_MT_WIDTH_MAJOR, 1);
			SEND_INPUT_EVENT(EV_ABS, ABS_MT_POSITION_X, match_coords.x);
			SEND_INPUT_EVENT(EV_ABS, ABS_MT_POSITION_Y, match_coords.y);
			SEND_INPUT_EVENT(EV_SYN, SYN_MT_REPORT, 0);
			SEND_INPUT_EVENT(EV_SYN, SYN_REPORT, 0);

			SEND_INPUT_EVENT(EV_ABS, ABS_MT_TRACKING_ID, 1);
			SEND_INPUT_EVENT(EV_ABS, ABS_MT_TOUCH_MAJOR, 0);
			SEND_INPUT_EVENT(EV_ABS, ABS_MT_WIDTH_MAJOR, 0);
			SEND_INPUT_EVENT(EV_ABS, ABS_MT_POSITION_X, match_coords.x);
			SEND_INPUT_EVENT(EV_ABS, ABS_MT_POSITION_Y, match_coords.y);
			SEND_INPUT_EVENT(EV_SYN, SYN_MT_REPORT, 0);
			SEND_INPUT_EVENT(EV_SYN, SYN_REPORT, 0);

			close(ifd);
		}
	} else {
		fprintf(stderr, "No match :(\n");
	}

	// Cleanup
	if (g_fbink_isFbMapped) {
		munmap(g_fbink_fbp, g_fbink_screensize);
	}
	close(fbfd);

	return EXIT_SUCCESS;
}
