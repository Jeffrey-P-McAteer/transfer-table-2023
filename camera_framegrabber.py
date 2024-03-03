
CURRENT_FRAME_FILE = '/tmp/camframe.jpg'

import os
import sys
import subprocess
import asyncio
import socket
import traceback
import time
import logging
import struct
import datetime
import random
import base64
import threading
import signal

py_env_dir = os.path.join(os.path.dirname(__file__), '.py-env')
os.makedirs(py_env_dir, exist_ok=True)
sys.path.insert(0, py_env_dir)


try:
  import cv2
except:
  subprocess.run([
    sys.executable, '-m', 'pip', 'install', f'--target={py_env_dir}', 'opencv-python'
  ])
  import cv2

exit_flag = False


def try_to_use_core_2_excl():
  try:
    pid = os.getpid()
    subprocess.run([
      'taskset', '-cp', '2', str(pid)
    ])
  except:
    traceback.print_exc()

def signal_handler(sig, frame):
  global exit_flag
  exit_flag = True
  print(f'exit_flag = {exit_flag}')

def main(args=sys.argv):
  global exit_flag
  signal.signal(signal.SIGINT, signal_handler)
  try_to_use_core_2_excl() # shared w/ webserver
  camera = None
  for cam_num in range(0, 99):
    try:
      camera = cv2.VideoCapture(cam_num)
      test, frame = camera.read()
      if not (frame is None):
        break
    except:
      traceback.print_exc()
  width  = int(camera.get(cv2.CAP_PROP_FRAME_WIDTH))
  height = int(camera.get(cv2.CAP_PROP_FRAME_HEIGHT))
  print(f'cam_num = {cam_num} camera = {camera} size = {width}x{height}')

  frame_num = 0
  num_exceptions = 0
  while not exit_flag:
    try:
      time.sleep(0.09)
      frame_num += 1
      rounded_frame_num = frame_num % 1000
      print(f'frame_num = {frame_num}')

      _, img = camera.read()

      cv2.putText(img, f'{rounded_frame_num}', (8, height-20), cv2.FONT_HERSHEY_SIMPLEX, 1, (10, 10, 10), 3, cv2.LINE_AA) # black outline
      cv2.putText(img, f'{rounded_frame_num}', (8, height-20), cv2.FONT_HERSHEY_SIMPLEX, 1, (240, 240, 240), 2, cv2.LINE_AA) # White text

      jpg_video_frame = cv2.imencode('.jpg', img)[1].tobytes()

      with open(CURRENT_FRAME_FILE, 'wb') as fd:
        fd.write(jpg_video_frame)

    except:
      traceback.print_exc()
      num_exceptions += 1
      if num_exceptions > 10:
        break

  if camera is not None:
    camera.release()

  print('Goodbye!')


if __name__ == '__main__':
  main()
