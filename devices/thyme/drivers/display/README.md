# THYME display

`hakios.thyme_display.c` owns only the panel backlight path:

- verify that GCC and DISPCC left the display register bus and power domain on;
- map DSI0 at `0x0ae94000`;
- send DCS `0x51` through the command DMA path;
- expose `kern.hakios.brightness` in permille (`0..1000`).

The selected THYME panel is `xiaomi 42 02 0a cmd mode dsc dsi panel`, with
`bl_ctrl_dcs` and a 12-bit brightness range of `0..0xfff`.
