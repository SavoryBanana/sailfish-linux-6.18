#!/bin/sh
# Qualcomm SDM / MDP5 sysfs mock setup for Pixel 1 (sailfish) on modern Linux 6.18
set -e

echo "[*] Setting up Qualcomm MDSS sysfs mock directory structure..."
mkdir -p /sys/devices/virtual/graphics/fb0/mdp

echo "[*] Populating caps (12 hardware pipes, SDE 5.0 MSM8996 Pro)..."
cat << 'EOF' > /sys/devices/virtual/graphics/fb0/mdp/caps
hw_rev=268894210
max_upscale_ratio=20
max_downscale_ratio=4
max_bandwidth_low=9600000
max_bandwidth_high=9600000
max_pipe_bw=4500000
max_mdp_clk=412500000
clk_fudge_factor=105,100
scale_factor=1
xtra_ff_factor=1
fmt_linear_factor=1
fmt_mt_factor=1
fmt_mt_nv12_factor=8
amortizable_threshold=25
system_overhead_lines=20
blending_stages=8
max_mixer_width=2560
max_pipe_width=2560
max_cursor_size=512
non_scalar_rgb=0
features=bwc,decimation,tile_format,src_split
pipe_count=12
pipe_type=vig pipe_ndx=1 rects=1 fmts_supported=51,224,0,22
pipe_type=vig pipe_ndx=2 rects=1 fmts_supported=51,224,0,22
pipe_type=vig pipe_ndx=3 rects=1 fmts_supported=51,224,0,22
pipe_type=vig pipe_ndx=4 rects=1 fmts_supported=51,224,0,22
pipe_type=rgb pipe_ndx=5 rects=1 fmts_supported=51,224,0,22
pipe_type=rgb pipe_ndx=6 rects=1 fmts_supported=51,224,0,22
pipe_type=rgb pipe_ndx=7 rects=1 fmts_supported=51,224,0,22
pipe_type=rgb pipe_ndx=8 rects=1 fmts_supported=51,224,0,22
pipe_type=dma pipe_ndx=9 rects=1 fmts_supported=51,224,0,22
pipe_type=dma pipe_ndx=10 rects=1 fmts_supported=51,224,0,22
pipe_type=cursor pipe_ndx=11 rects=1 fmts_supported=18,96
pipe_type=cursor pipe_ndx=12 rects=1 fmts_supported=18,96
EOF
printf "0\n" > /sys/devices/virtual/graphics/fb0/mdp/bw_mode_bitmap

echo "[*] Populating msm_fb_panel_info..."
cat << 'EOF' > /sys/devices/virtual/graphics/fb0/msm_fb_panel_info
panel_name=s6e3fa5_cmd
primary_panel=1
is_pluggable=0
xres=1080
yres=1920
bpp=24
type=mipi dsi cmd panel
min_fps=60
max_fps=60
fps=60
dyn_fps_en=0
pu_en=0
EOF

echo "[*] Populating msm_fb_type and status nodes..."
printf "mipi dsi cmd panel\n" > /sys/devices/virtual/graphics/fb0/msm_fb_type
printf "1\n" > /sys/devices/virtual/graphics/fb0/connected
printf "0 0\n" > /sys/devices/virtual/graphics/fb0/msm_fb_split
printf "0\n" > /sys/devices/virtual/graphics/fb0/msm_cmd_autorefresh_en
printf "VSYNC=0\n" > /sys/devices/virtual/graphics/fb0/vsync_event

echo "[*] Setting permissions on graphics device nodes..."
chmod 666 /dev/fb* /dev/graphics/* /dev/dri/* 2>/dev/null || true

echo "[+] MDSS sysfs simulation ready! CoreInterface::CreateCore() can now probe successfully."

