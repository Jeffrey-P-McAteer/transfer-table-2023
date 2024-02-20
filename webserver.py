#!/usr/bin/env python

import os
import sys
import subprocess
import asyncio
import socket
import traceback
import time
import logging

py_env_dir = os.path.join(os.path.dirname(__file__), '.py-env')
os.makedirs(py_env_dir, exist_ok=True)
sys.path.insert(0, py_env_dir)

try:
  import aiohttp
except:
  subprocess.run([
    sys.executable, '-m', 'pip', 'install', f'--target={py_env_dir}', 'aiohttp'
  ])
  import aiohttp

import aiohttp.web

try:
  import cv2
except:
  subprocess.run([
    sys.executable, '-m', 'pip', 'install', f'--target={py_env_dir}', 'opencv-python'
  ])
  import cv2



def get_loc_ip():
  local_ip = None
  try:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(('1.1.1.1', 80))
    local_ip = s.getsockname()[0]
    s.close()
  except:
    traceback.print_exc()
  return local_ip



async def index_handle(request):
  index_html = '''
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Transfer Table Control</title>
  <style>
html, body {
  margin: 0;
  padding: 0;
}
#camera_stream {
  width: 100vw;
  max-width: 600pt;
  display: block;
}
  </style>
</head>
<body>
  <img src="/video" id="camera_stream" />

</body>
</html>
'''.strip()
  return aiohttp.web.Response(text=index_html, content_type='text/html')

last_video_frame_num = 0
last_video_frame_s = 0
last_video_frame = None
async def read_video_t():
  global last_video_frame_num, last_video_frame_s, last_video_frame
  try:
    frame_delay_s = 0.05

    camera = None
    for cam_num in range(0, 9):
      try:
        camera = cv2.VideoCapture(f'/dev/video{cam_num}')
      except:
        traceback.print_exc()
      if camera is not None:
        break

    if camera is None or not camera.isOpened():
        raise RuntimeError('Cannot open camera')

    while True:
      last_video_frame_s = time.time()
      last_video_frame_num += 1

      _, img = camera.read()
      # img = cv2.resize(img, resolution)
      last_video_frame = cv2.imencode('.jpg', img)[1].tobytes()

      await asyncio.sleep(frame_delay_s) # 50ms, allow other tasks to run

  except:
    traceback.print_exc()
  finally:
    last_video_frame_num = 0
    last_video_frame_s = 0
    last_video_frame = None

async def ensure_video_is_being_read():
  global last_video_frame_num, last_video_frame_s, last_video_frame
  last_frame_age = time.time() - last_video_frame_s
  if last_frame_age > 4.0:
    asyncio.create_task(read_video_t())


async def video_handle(request):
  global last_video_frame_num, last_video_frame_s, last_video_frame

  asyncio.create_task(ensure_video_is_being_read())

  response = aiohttp.web.StreamResponse()
  response.content_type = 'multipart/x-mixed-replace; boundary=frame'

  await response.prepare(request)

  last_read_frame_num = 0
  while True:
    if last_video_frame is not None and last_read_frame_num != last_video_frame:
      await response.write(
        b'--frame\r\nContent-Type: image/jpeg\r\n\r\n'+last_video_frame+b'\r\n'
      )
    await asyncio.sleep(0.05)

  return response


def build_app():
  app = aiohttp.web.Application()
  app.add_routes([
    aiohttp.web.get('/', index_handle),
    aiohttp.web.get('/index.html', index_handle),
    aiohttp.web.get('/video', video_handle),
  ])
  return app

def main(args=sys.argv):
  if len(os.environ.get('DEBUG', '')) > 0:
    logging.basicConfig(level=logging.DEBUG)
  local_ip = get_loc_ip()
  hostname = socket.gethostname()
  if os.geteuid() == 0:
    print(f'Running on http://{local_ip}/')
    print(f'Running on http://{hostname}/')
    aiohttp.web.run_app(build_app(), port=80)
  else:
    print(f'Running on http://{local_ip}:8080/')
    print(f'Running on http://{hostname}.local:8080/')
    aiohttp.web.run_app(build_app(), port=8080)


if __name__ == '__main__':
  main()
