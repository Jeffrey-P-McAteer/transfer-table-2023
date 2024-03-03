#!/usr/bin/env python

# Keep in sync with gpio-motor-control.zig
PMEM_FILE = "/mnt/usb1/pmem.bin"
GPIO_MOTOR_KEYS_IN_DIR = "/tmp/gpio_motor_keys_in"
PASSWORD_FILE = '/mnt/usb1/webserver-password.txt'
#FRAME_HANDLE_DELAY_S = 0.08
FRAME_HANDLE_DELAY_S = 0.19

CURRENT_FRAME_FILE = '/tmp/camframe.jpg' # shared w/ camera_framegrabber.py

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


try:
  import psutil
except:
  subprocess.run([
    sys.executable, '-m', 'pip', 'install', f'--target={py_env_dir}', 'psutil'
  ])
  import psutil


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

async def get_current_password():
  try:
    if os.path.exists(PASSWORD_FILE):
      with open(PASSWORD_FILE, 'r') as fd:
        return fd.read().strip()
  except:
    traceback.print_exc()
  return None

async def set_current_password(password):
  try:
    with open(PASSWORD_FILE, 'w') as fd:
      fd.write(password.strip())
  except:
    traceback.print_exc()


# Returns None if auth is good, else a aiohttp.web.Response object to be sent back by caller.
async def maybe_redirect_for_auth(request):
  supplied_auth = request.headers.getone('Authorization', 'Basic ==')
  supplied_pw = None
  try:
    supplied_pw = base64.b64decode(supplied_auth[5:].strip())
    if not isinstance(supplied_pw, str):
      supplied_pw = supplied_pw.decode('utf-8')
    if supplied_pw[0] == ':': # trim protocol char off
      supplied_pw = supplied_pw[1:]
    supplied_pw = supplied_pw.strip()
  except:
    traceback.print_exc()
  print(f'supplied_pw = {supplied_pw}')
  current_pw = await get_current_password()
  if current_pw is None:
    return None # cannot do password check, not yet set!
  if supplied_pw is None or not supplied_pw == current_pw:
    return aiohttp.web.Response(
          body=b'',
          status=401,
          reason='UNAUTHORIZED',
          headers={
              aiohttp.hdrs.WWW_AUTHENTICATE: 'Basic realm="Transfer Table Password"',
              aiohttp.hdrs.CONTENT_TYPE: 'text/html; charset=utf-8',
              aiohttp.hdrs.CONNECTION: 'keep-alive',
          },
      )
  return None

async def index_handle(request):
  index_html = ('''
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
#inputForm, #setPasswordForm {
  max-width: 592pt;
  padding: 2pt;
}
h2, #status_iframe, #inputForm, #setPasswordForm {
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
    function submitSetPasswordForm() {
      var frm = document.getElementById('setPasswordForm');
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
    <br/>
    <i>
      Numbers turn into key presses, 'r' becomes a counter-clockwise dial rotation, 'l' becomes a clockwise dial rotation.
      '=' performs the same as '=' on keyboard or numpad.
    </i>
  </form>
  <iframe name="dummyFormFrame" id="dummyFormFrame" style="display: none;"></iframe>
  <br/>
  <br/>
  <br/>
  <details>
    <summary>Table Status</summary>
    <iframe src="/status" id="status_iframe" style="border:1px solid black;border-radius:3pt;"></iframe>
  </details>
  <br/>
  <br/>
  <br/>
  <form id="setPasswordForm" action="/set-control-password" method="POST" target="dummyFormFrame">
    <label for="pw">Set Control Password</label>
    <input name="pw" id="pw" value="" type="password" />
    <br/><br/>
    <input type="button" value="Enter" onclick="submitSetPasswordForm()" style="margin-left:172pt;"/>
    <br/>
    <i>
      If set, the current password is stored at '''+PASSWORD_FILE+''' on the control server. Remove this file to "reset" the password if forgotten. /mnt/usb1 is the USB drive.
    </i>
  </form>
  <br/>
  <br/>
  <br/>

</body>
</html>
''').strip()
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
  auth_resp = await maybe_redirect_for_auth(request)
  if auth_resp is not None:
    return auth_resp

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

async def set_control_password_handle(request):
  auth_resp = await maybe_redirect_for_auth(request)
  if auth_resp is not None:
    return auth_resp

  data = await request.post()
  print(f'set_control_password_handle data = {data}')

  input_file_keycode_s = ''
  pw_val = data['pw'].lower()
  await set_current_password(pw_val)
  return aiohttp.web.Response(text=f'Done, pw_val={"*" * len(pw_val)}', content_type='text/plain')


last_s_checked_video_p_running = 0
video_p_running = False
video_p = None
async def ensure_video_is_being_read():
  global age_s_checked_video_p_running, video_p_running, video_p
  age_s_checked_video_p_running = time.time() - last_s_checked_video_p_running
  if not video_p_running or age_s_checked_video_p_running > 30.0:
    # check & spawn python camera_framegrabber.py if not running
    age_s_checked_video_p_running = time.time()
    proc_pids = psutil.pids()
    video_p_running = False # assume not running by default!
    for pid in proc_pids:
      try:
        if psutil.pid_exists(pid):
          p = psutil.Process(pid)
          cmd_str = ' '.join(p.cmdline())
          if 'camera_framegrabber.py' in cmd_str:
            video_p_running = True
            break
      except:
        traceback.print_exc()
  if not video_p_running:
    video_p_cmd = [
      sys.executable, os.path.join(os.path.dirname(__file__), 'camera_framegrabber.py')
    ]
    print(f'video_p_running={video_p_running}, spawning {video_p_cmd}')
    video_p = subprocess.Popen(video_p_cmd, preexec_fn=os.setpgrp) # we do not kill this process when we exit! see setpgrp
    print(f'video_p = {video_p}')


async def video_handle(request):
  global last_video_frame_num, last_video_frame_s, last_video_frame

  #asyncio.create_task(ensure_video_is_being_read())
  await ensure_video_is_being_read()

  response = aiohttp.web.StreamResponse()
  response.content_type = 'multipart/x-mixed-replace; boundary=frame'

  await response.prepare(request)

  last_frame_file_mtime_ns = 0
  while True:
    if os.path.exists(CURRENT_FRAME_FILE):
      # Limited poll quickly until mtime changes, then read + sleep
      for _ in range(0, 20):
        if os.stat(CURRENT_FRAME_FILE).st_mtime_ns != last_frame_file_mtime_ns:
          break
        await asyncio.sleep(FRAME_HANDLE_DELAY_S / 5.0)

      last_frame_file_mtime_ns = os.stat(CURRENT_FRAME_FILE).st_mtime_ns

      with open(CURRENT_FRAME_FILE, 'rb') as fd:
        frame_jpg_bytes = fd.read()
        await response.write(
          b'--frame\r\nContent-Type: image/jpeg\r\n\r\n'+frame_jpg_bytes+b'\r\n'
        )

    await asyncio.sleep(FRAME_HANDLE_DELAY_S)

  return response

async def on_app_shutdown(app):
  global app_is_shutting_down, video_p
  app_is_shutting_down = True
  print(f'app_is_shutting_down = {app_is_shutting_down}!')
  #if video_p is not None:
  #  video_p.kill()

def build_app():
  app = aiohttp.web.Application()
  app.add_routes([
    aiohttp.web.get('/', index_handle),
    aiohttp.web.get('/index.html', index_handle),
    aiohttp.web.get('/video', video_handle),
    aiohttp.web.get('/status', status_handle),
    aiohttp.web.post('/input', input_handle),
    aiohttp.web.post('/set-control-password', set_control_password_handle)
  ])
  app.on_cleanup.append(on_app_shutdown)
  return app

def try_to_use_core_2_excl():
  try:
    pid = os.getpid()
    subprocess.run([
      'taskset', '-cp', '1,2', str(pid)
    ])
  except:
    traceback.print_exc()

def main(args=sys.argv):
  if len(os.environ.get('DEBUG', '')) > 0:
    logging.basicConfig(level=logging.DEBUG)

  try_to_use_core_2_excl()

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
