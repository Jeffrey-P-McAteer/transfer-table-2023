#!/usr/bin/env python

# Keep in sync with gpio-motor-control.zig
PMEM_FILE = "/mnt/usb1/pmem.bin"
GPIO_MOTOR_KEYS_IN_DIR = "/tmp/gpio_motor_keys_in"

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
#status_iframe {
  width: 90vw;
  max-width: 592pt;
  min-height: 400pt;
  display: block;
  padding: 2pt;
}
#inputForm {
  max-width: 592pt;
  padding: 2pt;
}
h2, #status_iframe, #inputForm {
  margin: 2pt;
}
input, label {
  font-size: 16pt;
}
  </style>
</head>
<body>
  <script>
    function submitInputForm() {
      var frm = document.getElementById('inputForm');
       frm.submit();
       frm.reset();
       return false;
    }
  </script>
  <img src="/video" id="camera_stream" />
  <h2>Table Input</h2>
  <form id="inputForm" action="/input" method="POST" target="dummyFormFrame">
    <label for="number">Number</label>
    <input name="number" id="number" value="" type="text" />
    <br/><br/>
    <input type="button" value="Enter" onclick="submitInputForm()" style="margin-left:172pt;"/>
    <i>
      Numbers turn into key presses, 'r' becomes a clockwise dial rotation, 'l' becomes a counter-clockwise dial rotation.
      '=' performs the same as '=' on keyboard or numpad.
    </i>
  </form>
  <iframe name="dummyFormFrame" id="dummyFormFrame" style="display: none;"></iframe>
  <br/>
  <br/>
  <br/>
  <h2>Table Status</h2>
  <iframe src="/status" id="status_iframe" style="border:1px solid black;border-radius:3pt;"/>

</body>
</html>
'''.strip()
  return aiohttp.web.Response(text=index_html, content_type='text/html')


async def status_handle(request):
  track_data = 'ERROR FETCHING TABLE POSITIONS'
  try:
    pmem_bytes = b''
    with open(PMEM_FILE, 'rb') as fd:
      pmem_bytes = fd.read()

    # pmem_bytes has structure
    #   logical_position: u32,
    #   step_position: i32,
    #   positions: [12]pos_dat align(1),

    if len(pmem_bytes) > 104:
      pmem_bytes = pmem_bytes[:104] # todo better

    pmem_data = struct.unpack(
      'Ii'+('if'*12),
      pmem_bytes
    )
    logical_position = pmem_data[0]
    track_data = '================================='+os.linesep
    track_data += f'logical_position = {logical_position}'+os.linesep
    step_position = pmem_data[1]
    track_data += f'step_position = {step_position}'+os.linesep
    track_data += '================================='+os.linesep

    for pos_num in range(0, 12):
      step_position = pmem_data[2+(pos_num*2)]
      track_data += f'Position {pos_num+1} step_position = {step_position}'+os.linesep
    track_data += '================================='+os.linesep

    track_data += '==== Zero position init code ===='+os.linesep
    for pos_num in range(0, 12):
      step_position = pmem_data[2+(pos_num*2)]
      track_data += f'pmem.positions[{pos_num}].step_position = {step_position};'+os.linesep
    track_data += os.linesep

  except:
    traceback.print_exc()
    track_data += '\n'+traceback.format_exc()

  index_html = f'''
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="refresh" content="2" />
  <style>
html, body {{
  margin: 0;
  padding: 0;
}}
  </style>
</head>
<body>
  <p><i>Status at {datetime.datetime.now()}</i></p>
  <pre>{track_data}</pre>
</body>
</html>
'''.strip()
  return aiohttp.web.Response(text=index_html, content_type='text/html')


async def input_handle(request):
  data = await request.post()
  print(f'input_handle data = {data}')

  input_file_keycode_s = ''
  number_val = data['number'].lower()

  for number in number_val:
    # convert int format to linux keycode number
    try:
      # See https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h
      if number == '0': # KEY_KP0 == 82
        input_file_keycode_s += f'82,'
      elif number == '1': # KEY_KP1 == 79
        input_file_keycode_s += f'79,'
      elif number == '2': # KEY_KP2 == 82
        input_file_keycode_s += f'80,'
      elif number == '3': # KEY_KP3 == 82
        input_file_keycode_s += f'83,'
      elif number == '4': # KEY_KP4 == 75
        input_file_keycode_s += f'75,'
      elif number == '5': # KEY_KP5 == 76
        input_file_keycode_s += f'76,'
      elif number == '6': # KEY_KP6 == 77
        input_file_keycode_s += f'77,'
      elif number == '7': # KEY_KP7 == 71
        input_file_keycode_s += f'71,'
      elif number == '8': # KEY_KP8 == 72
        input_file_keycode_s += f'72,'
      elif number == '9': # KEY_KP9 == 73
        input_file_keycode_s += f'73,'
      elif number == 'r': # right/clockwise rotation dial
        input_file_keycode_s += f'114,'
      elif number == 'l': # left/counte-clockwise rotation dial
        input_file_keycode_s += f'115,'
      elif number == '=': # equals is 13
        input_file_keycode_s += f'13,'
    except:
      traceback.print_exc()

  # and add an <enter> keycode
  input_file_keycode_s += '96'

  # Find first non-existent file under GPIO_MOTOR_KEYS_IN_DIR
  for _ in range(0, 100):
    input_num = random.randrange(1000, 9000)
    input_f_name = os.path.join(GPIO_MOTOR_KEYS_IN_DIR, f'{input_num}.txt')
    if os.path.exists(input_f_name):
      continue
    with open(input_f_name, 'w') as fd:
      fd.write(input_file_keycode_s)
    break

  return aiohttp.web.Response(text=f'Done, input_file_keycode_s={input_file_keycode_s}', content_type='text/plain')


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
    aiohttp.web.get('/status', status_handle),
    aiohttp.web.post('/input', input_handle),
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