#!/bin/sh
# Qualcomm SDM / MDP5 sysfs mock setup for Pixel 1 (sailfish) on modern Linux 6.18
set -e

echo "[*] Setting up Qualcomm MDSS sysfs mock directory structure..."
mkdir -p /sys/devices/virtual/graphics/fb0/mdp

echo "[*] Populating caps (10 hardware pipes, SDE 5.0 MSM8996 Pro)..."
printf "hw_rev=268894210\nmax_upscale_ratio=20\nmax_downscale_ratio=4\nmax_bandwidth_low=9600000000\nmax_bandwidth_high=9600000000\nmax_pipe_bandwidth=2500000000\nmax_sde_clk=412500000\nclk_fudge_factor=105 100\nscale_factor=1\nxtra_ff_factor=1\nblending_stages=4\nfeatures=qseed3,ubwc,src_split\npipe_count=10\npipe_type=vig pipe_ndx=1 rects=1\npipe_type=vig pipe_ndx=2 rects=1\npipe_type=vig pipe_ndx=3 rects=1\npipe_type=vig pipe_ndx=4 rects=1\npipe_type=dma pipe_ndx=5 rects=1\npipe_type=dma pipe_ndx=6 rects=1\npipe_type=dma pipe_ndx=7 rects=1\npipe_type=dma pipe_ndx=8 rects=1\npipe_type=cursor pipe_ndx=9 rects=1\npipe_type=cursor pipe_ndx=10 rects=1\n" > /sys/devices/virtual/graphics/fb0/mdp/caps

echo "[*] Populating msm_fb_panel_info..."
printf "panel_name=s6e3fa5_cmd\nxres=1080\nyres=1920\nbpp=24\ntype=mipi dsi cmd panel\nfps=60\n" > /sys/devices/virtual/graphics/fb0/msm_fb_panel_info

echo "[*] Populating msm_fb_type and status nodes..."
printf "mipi dsi cmd panel\n" > /sys/devices/virtual/graphics/fb0/msm_fb_type
printf "1\n" > /sys/devices/virtual/graphics/fb0/connected
printf "0\n" > /sys/devices/virtual/graphics/fb0/msm_fb_split

echo "[*] Setting permissions on graphics device nodes..."
chmod 666 /dev/fb* /dev/graphics/* /dev/dri/* 2>/dev/null || true

echo "[+] MDSS sysfs simulation ready! CoreInterface::CreateCore() can now probe successfully."

