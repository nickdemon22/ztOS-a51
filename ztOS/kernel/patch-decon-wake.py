#!/usr/bin/env python3
"""Wake the panel without showing static.

  patch-decon-wake.py <kernel tree>

After DCS sleep the panel's own frame memory holds noise. The driver sent
sleep-out and display-on back to back, before any new frame had reached that
memory, so every wake began with a flash of static. Now: sleep-out, point
the channel at the client's buffer and let a frame go out, put the
brightness back, and only then display-on. Applied after
s9p-decon-backlight.patch, whose trigger masking and s9p_panel_brightness
it uses.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1]) / "drivers/gpu/drm/s9p/s9p-decon.c"
s = path.read_text()
if "zboot: wake without static" in s:
    print("patch-decon-wake: already applied")
    sys.exit(0)

old_up = """static void s9p_panel_up(struct s9p_decon *priv)
{
	s9p_hw_trigger_mask(priv);
	if (s9p_panel_sleep) {
		s9p_decon_dcs(priv, MIPI_DCS_EXIT_SLEEP_MODE);
		msleep(PANEL_SLEEP_DELAY_MS);
	}
	s9p_decon_dcs(priv, MIPI_DCS_SET_DISPLAY_ON);
}"""
new_up = """/* zboot: wake without static. Sleep-out only; display-on comes once a frame
 * has reached the panel's memory (s9p_panel_on). */
static void s9p_panel_up(struct s9p_decon *priv)
{
	if (!s9p_panel_sleep)
		return;
	s9p_hw_trigger_mask(priv);
	s9p_decon_dcs(priv, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(PANEL_SLEEP_DELAY_MS);
	s9p_hw_trigger_unmask(priv);
}

static void s9p_panel_on(struct s9p_decon *priv)
{
	s9p_hw_trigger_mask(priv);
	s9p_wait_shadow_idle(priv);
	s9p_decon_dcs(priv, MIPI_DCS_SET_DISPLAY_ON);
	s9p_hw_trigger_unmask(priv);
}"""
old_enable = """	priv->enabled = true;
	/* sleep-out reloads the panel's registers from its own memory, so the
	 * brightness has to be sent again */
	if (priv->bl)
		s9p_panel_brightness(priv, priv->bl->props.brightness);
	drm_crtc_vblank_on(&pipe->crtc);"""
new_enable = """	priv->enabled = true;
	/* two refreshes for the client's frame to reach the panel's memory,
	 * which holds noise after sleep */
	msleep(40);
	/* sleep-out reloads the panel's registers from its own memory, so the
	 * brightness has to be sent again, before it lights up */
	if (priv->bl)
		s9p_panel_brightness(priv, priv->bl->props.brightness);
	s9p_panel_on(priv);
	drm_crtc_vblank_on(&pipe->crtc);"""
old_down = """	s9p_hw_trigger_mask(priv);
	s9p_decon_dcs(priv, MIPI_DCS_SET_DISPLAY_OFF);
	if (!s9p_panel_sleep)
		return;"""
new_down = """	s9p_hw_trigger_mask(priv);
	s9p_decon_dcs(priv, MIPI_DCS_SET_DISPLAY_OFF);
	if (!s9p_panel_sleep) {
		s9p_hw_trigger_unmask(priv);	/* frames keep going to the panel */
		return;
	}"""
for old, new in ((old_up, new_up), (old_enable, new_enable), (old_down, new_down)):
    if s.count(old) != 1:
        sys.exit("patch-decon-wake: the driver changed; look at this patch again")
    s = s.replace(old, new)
path.write_text(s)
print("patch-decon-wake: applied")
