#!/bin/bash

v4l2-ctl \
  --device=/dev/video0 \
  --set-fmt-video=width=1456,height=1088,pixelformat=RG10 \
  --set-ctrl preferred_stride=2944 \
  --stream-mmap \
  --stream-count=1 \
  --stream-to=./frame.raw
