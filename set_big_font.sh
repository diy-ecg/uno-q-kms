#!/bin/bash

# Set console font to Terminus 28x14
FONT_PATH="/usr/share/consolefonts/Lat15-Terminus28x14.psf.gz"

if [ ! -f "$FONT_PATH" ]; then
    echo "Font not found: $FONT_PATH"
    exit 1
fi

sudo setfont "$FONT_PATH"