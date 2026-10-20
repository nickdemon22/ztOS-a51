-- Hyprland on the Galaxy S9, driven by touch alone.
--
--   bottom edge, swipe up        app launcher
--   left / right edge, swipe in  previous / next window (one window fills the screen)
--   bottom edge, swipe sideways  switch workspace
--   three fingers, swipe down    close the window
--   bar buttons                  launcher, keyboard, close
--
-- Gestures come from hyprgrass (github.com/horriblename/hyprgrass), built
-- at the commit its hyprpm.toml pins for Hyprland 0.56.2.

-- 3.2 gives a 450x925 logical screen: both sides divide cleanly, and it is close
-- to what a phone of this size normally uses.
hl.monitor({ output = "DSI-1", mode = "1440x2960@60", position = "0x0", scale = 3.2 })

-- Mesa's AFBC repacking draws 16-pixel dashes into icons and glyphs on this GPU.
hl.env("PAN_MAX_AFBC_PACKING_RATIO", "0")
hl.env("XCURSOR_SIZE", "24")

hl.config({
    general = {
        gaps_in = 2,
        gaps_out = 4,
        border_size = 2,
        layout = "scrolling",
    },
    scrolling = {
        column_width = 1.0,
        fullscreen_on_one_column = true,
        follow_focus = true,
    },
    decoration = {
        rounding = 8,
        blur = { enabled = false },
        shadow = { enabled = false },
    },
    misc = {
        disable_hyprland_logo = true,
        force_default_wallpaper = 0,
    },
    input = {
        touchdevice = { enabled = true, output = "DSI-1" },
    },
    cursor = {
        hide_on_touch = true,
    },
    gestures = {
        workspace_swipe_touch = true,
        workspace_swipe_cancel_ratio = 0.15,
    },
})

hl.plugin.load("/home/alarm/hyprgrass/build/src/libhyprgrass.so")

local grass = hl.plugin.hyprgrass
if grass then
    hl.config({
        plugin = {
            hyprgrass = {
                sensitivity = 4.0,
                long_press_delay = 400,
                -- logical pixels from the glass edge that still count as "the edge"
                edge_margin = 30,
            },
        },
    })

    grass.bind({
        pattern = { kind = "edge", origin = "d", direction = "u" },
        action = hl.dsp.exec_cmd("pkill fuzzel || fuzzel"),
    })
    grass.bind({
        pattern = { kind = "edge", origin = "l", direction = "r" },
        action = hl.dsp.focus({ direction = "left" }),
    })
    grass.bind({
        pattern = { kind = "edge", origin = "r", direction = "l" },
        action = hl.dsp.focus({ direction = "right" }),
    })
    grass.bind({
        pattern = { kind = "swipe", fingers = 3, direction = "down" },
        action = hl.dsp.window.close(),
    })
    grass.gesture({
        pattern = { kind = "edge", origin = "down", direction = "horizontal" },
        action = "workspace",
    })
end

hl.on("hyprland.start", function()
    hl.exec_cmd("swaybg -m fill -i /usr/share/hypr/wall0.png")
    hl.exec_cmd("waybar")
    hl.exec_cmd("squeekboard")
end)

-- For a USB or Bluetooth keyboard, when there is one.
hl.bind("SUPER + Q", hl.dsp.exec_cmd("foot"))
hl.bind("SUPER + R", hl.dsp.exec_cmd("fuzzel"))
hl.bind("SUPER + C", hl.dsp.window.close())
hl.bind("SUPER + left", hl.dsp.focus({ direction = "left" }))
hl.bind("SUPER + right", hl.dsp.focus({ direction = "right" }))
