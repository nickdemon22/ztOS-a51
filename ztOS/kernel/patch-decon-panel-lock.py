#!/usr/bin/env python3
"""One panel at a time: flips and panel commands take turns.

  patch-decon-panel-lock.py <kernel tree>

A brightness change (s9p_bl_update, from sysfs) holds the frame trigger off
while it sends its commands. A flip running at the same moment waits for a
latch that cannot come while the trigger is held, gives up after 50 ms
("flip did not latch"), and the compositor loses that frame; a brightness
ramp lost a run of them and the shell was taken for hung and restarted.

panel_lock is held by the flip from programming the address to its latch,
by every brightness change, and by enabling and blanking the pipeline, so
each waits for the other to finish instead of breaking it. Applied after
s9p-decon-backlight.patch, patch-decon-wake.py and patch-decon-flip.py.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1]) / "drivers/gpu/drm/s9p/s9p-decon.c"
s = path.read_text()
if "panel_lock" in s:
    print("patch-decon-panel-lock: already applied")
    sys.exit(0)

edits = [
    ("""	struct backlight_device *bl;
};""", """	struct backlight_device *bl;
	struct mutex panel_lock;	/* flips and panel commands take turns */
};"""),
    ("""	priv = devm_drm_dev_alloc(&pdev->dev, &s9p_drm_driver,
				  struct s9p_decon, drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);
""", """	priv = devm_drm_dev_alloc(&pdev->dev, &s9p_drm_driver,
				  struct s9p_decon, drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);
	mutex_init(&priv->panel_lock);
"""),
    ("""	if (!priv->enabled || backlight_is_blank(bl))
		return 0;
	s9p_panel_brightness(priv, bl->props.brightness);
	return 0;""", """	mutex_lock(&priv->panel_lock);
	if (priv->enabled && !backlight_is_blank(bl))
		s9p_panel_brightness(priv, bl->props.brightness);
	mutex_unlock(&priv->panel_lock);
	return 0;"""),
    ("""	bool latched = false;

	if (fb && priv->enabled) {""", """	bool latched = false;

	mutex_lock(&priv->panel_lock);
	if (fb && priv->enabled) {"""),
    ("""			else
				latched = true;	/* zboot: flip latched */
		}
	}
""", """			else
				latched = true;	/* zboot: flip latched */
		}
	}
	mutex_unlock(&priv->panel_lock);
"""),
    ("""	s9p_panel_up(priv);
	obj = to_drm_gem_dma_obj(plane_state->fb->obj[0]);""", """	mutex_lock(&priv->panel_lock);
	s9p_panel_up(priv);
	obj = to_drm_gem_dma_obj(plane_state->fb->obj[0]);"""),
    ("""	s9p_panel_on(priv);
	drm_crtc_vblank_on(&pipe->crtc);""", """	s9p_panel_on(priv);
	mutex_unlock(&priv->panel_lock);
	drm_crtc_vblank_on(&pipe->crtc);"""),
    ("""	s9p_decon_blank(priv);
	s9p_frame_start_irq_set(priv, false);
	priv->enabled = false;""", """	mutex_lock(&priv->panel_lock);
	priv->enabled = false;	/* no brightness to a panel going to sleep */
	s9p_decon_blank(priv);
	mutex_unlock(&priv->panel_lock);
	s9p_frame_start_irq_set(priv, false);"""),
]
for old, new in edits:
    if s.count(old) != 1:
        sys.exit("patch-decon-panel-lock: the driver changed near:\n" + old[:80])
    s = s.replace(old, new)
if "#include <linux/mutex.h>" not in s:
    s = s.replace("#include <linux/backlight.h>", "#include <linux/backlight.h>\n#include <linux/mutex.h>", 1)
path.write_text(s)
print("patch-decon-panel-lock: applied")
