#  SPDX-License-Identifier: LGPL-2.1-or-later
#
#  This file is part of systemd.
#
#  systemd is free software; you can redistribute it and/or modify it
#  under the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation; either version 2.1 of the License, or
#  (at your option) any later version.

ACTION=="remove", GOTO="uaccess_end"
ENV{MAJOR}=="", GOTO="uaccess_end"

# PTP/MTP protocol devices, cameras, portable media players
SUBSYSTEM=="usb", ENV{ID_USB_INTERFACES}=="*:060101:*", TAG+="uaccess"

# Digicams with proprietary protocol
ENV{ID_GPHOTO2}=="?*", TAG+="uaccess"

# SCSI and USB scanners
ENV{libsane_matched}=="yes", TAG+="uaccess"

# HPLIP devices (necessary for ink level check and HP tool maintenance)
ENV{ID_HPLIP}=="1", TAG+="uaccess"

# optical drives
SUBSYSTEM=="block", ENV{ID_CDROM}=="1", TAG+="uaccess"
SUBSYSTEM=="scsi_generic", SUBSYSTEMS=="scsi", ATTRS{type}=="4|5", TAG+="uaccess"

# Sound devices
SUBSYSTEM=="sound", TAG+="uaccess", \
  OPTIONS+="static_node=snd/timer", OPTIONS+="static_node=snd/seq"

# ffado is an userspace driver for firewire sound cards
SUBSYSTEM=="firewire", ENV{ID_FFADO}=="1", TAG+="uaccess"

# Webcams, frame grabber, TV cards
SUBSYSTEM=="video4linux", TAG+="uaccess"
SUBSYSTEM=="dvb", TAG+="uaccess"
SUBSYSTEM=="media", TAG+="uaccess"

# IIDC devices: industrial cameras and some webcams
SUBSYSTEM=="firewire", ATTR{units}=="*0x00a02d:0x00010*",  TAG+="uaccess"
SUBSYSTEM=="firewire", ATTR{units}=="*0x00b09d:0x00010*",  TAG+="uaccess"
# AV/C devices: camcorders, set-top boxes, TV sets, audio devices, and more
SUBSYSTEM=="firewire", ATTR{units}=="*0x00a02d:0x010001*", TAG+="uaccess"
SUBSYSTEM=="firewire", ATTR{units}=="*0x00a02d:0x014001*", TAG+="uaccess"

# DRI video devices
SUBSYSTEM=="drm", KERNEL=="card*", TAG+="uaccess"
m4_ifdef(`GROUP_RENDER_UACCESS',``
# DRI render nodes
SUBSYSTEM=="drm", KERNEL=="renderD*", TAG+="uaccess"''
)m4_dnl
m4_ifdef(`DEV_KVM_UACCESS',``
# KVM
SUBSYSTEM=="misc", KERNEL=="kvm", TAG+="uaccess"''
)m4_dnl

# smart-card readers
ENV{ID_SMARTCARD_READER}=="?*", TAG+="uaccess"

# (USB) authentication devices
ENV{ID_SECURITY_TOKEN}=="?*", TAG+="uaccess"

# PDA devices
ENV{ID_PDA}=="?*", TAG+="uaccess"

# Programmable remote control
ENV{ID_REMOTE_CONTROL}=="1", TAG+="uaccess"

# joysticks
SUBSYSTEM=="input", ENV{ID_INPUT_JOYSTICK}=="?*", TAG+="uaccess"

# color measurement devices
ENV{COLOR_MEASUREMENT_DEVICE}=="?*", TAG+="uaccess"

# DDC/CI device, usually high-end monitors such as the DreamColor
ENV{DDC_DEVICE}=="?*", TAG+="uaccess"

# media player raw devices (for user-mode drivers, Android SDK, etc.)
SUBSYSTEM=="usb", ENV{ID_MEDIA_PLAYER}=="?*", TAG+="uaccess"

# software-defined radio communication devices
ENV{ID_SOFTWARE_RADIO}=="?*", TAG+="uaccess"

# 3D printers, CNC machines, laser cutters, 3D scanners, etc.
ENV{ID_MAKER_TOOL}=="?*", TAG+="uaccess"

# Protocol analyzers
ENV{ID_SIGNAL_ANALYZER}=="?*", ENV{DEVTYPE}=="usb_device", TAG+="uaccess"

# rfkill / radio killswitches
KERNEL=="rfkill", SUBSYSTEM=="misc", TAG+="uaccess"

LABEL="uaccess_end"
