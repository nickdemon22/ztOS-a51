#!/usr/bin/env python3
"""Let s9p-decon complete a page flip in one refresh instead of two.

  patch-decon-flip.py <kernel tree>

The driver programs the new scanout address, then blocks until DECON latches
it at the next TE edge (up to 16.7 ms) -- and only then arms the flip's vblank
event, which fires at the frame start after that. Every flip therefore takes
two refreshes: measured from a compositor, 35 ms from commit to flip-done with
3 ms of rendering, 28 frames a second on a 60 Hz panel.

The latch is itself the moment the flip took effect. Once the wait has seen it
the event is sent straight away; the wait stays, because it is what keeps the
previous framebuffer from being handed back while DECON still reads it.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1]) / "drivers/gpu/drm/s9p/s9p-decon.c"
s = path.read_text()
if "zboot: flip latched" in s:
    print("patch-decon-flip: already applied")
    sys.exit(0)

old_wait = """			if (s9p_wait_shadow_idle(priv))
				drm_err_ratelimited(&priv->drm,
					"flip did not latch within 50 ms\\n");
		}
	}
"""
new_wait = """			if (s9p_wait_shadow_idle(priv))
				drm_err_ratelimited(&priv->drm,
					"flip did not latch within 50 ms\\n");
			else
				latched = true;	/* zboot: flip latched */
		}
	}
"""
old_event = """		if (crtc->state->active &&
		    drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, crtc->state->event);
		else
			drm_crtc_send_vblank_event(crtc, crtc->state->event);
"""
new_event = """		/*
		 * A flip that has latched is on screen now: the TE edge the
		 * wait above saw is this frame's start. Arming the event
		 * instead would report it one refresh late, every time.
		 */
		if (!latched && crtc->state->active &&
		    drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, crtc->state->event);
		else
			drm_crtc_send_vblank_event(crtc, crtc->state->event);
"""
old_decl = """	struct drm_framebuffer *fb = pipe->plane.state->fb;

	if (fb && priv->enabled) {
		if (s9p_wait_shadow_idle(priv)) {"""
new_decl = """	struct drm_framebuffer *fb = pipe->plane.state->fb;
	bool latched = false;

	if (fb && priv->enabled) {
		if (s9p_wait_shadow_idle(priv)) {"""
for old, new in ((old_wait, new_wait), (old_event, new_event), (old_decl, new_decl)):
    if s.count(old) != 1:
        sys.exit(f"patch-decon-flip: anchor not found exactly once: {old[:50]!r}")
    s = s.replace(old, new)
path.write_text(s)
print("patch-decon-flip: flip-done is sent at the latch")
