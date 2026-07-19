#!/bin/bash

gst-launch-1.0 \
  nvarguscamerasrc ee-mode=0 ee-strength=0 tnr-mode=0 tnr-strength=0.2 wbmode=0 sensor-id=0 sensor-mode=0 ! \
  'video/x-raw(memory:NVMM),width=1456,height=1088,framerate=60/1' ! \
  nvvidconv ! \
  'video/x-raw,format=I420' ! \
  jpegenc ! \
  rtpjpegpay ! \
  queue ! \
  udpsink host=192.168.1.78 port=5000
