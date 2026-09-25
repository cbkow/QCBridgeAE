#!/bin/bash
# Removes the QCBridgeAE Transmit device from MediaCore and the receipt.
set -u
echo "QCBridgeAE uninstaller"
echo
echo "This removes the QCBridgeAE → QCView device from Adobe's MediaCore"
echo "folder. Quit After Effects and Premiere Pro first. It will ask for"
echo "your password."
read -r -p "Continue? [y/N] " yn
case "$yn" in y|Y|yes|YES) ;; *) echo "Cancelled."; exit 0 ;; esac
sudo rm -rf "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/QCBridgeAE"
sudo pkgutil --forget ski.bialkow.qcbridgeae >/dev/null 2>&1 || true
echo "Removed."
