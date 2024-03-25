#!/usr/bin/env python

# Keep in sync with gpio-motor-control.zig
PMEM_FILE = "/mnt/usb1/pmem.bin"
GPIO_MOTOR_KEYS_IN_DIR = "/tmp/gpio_motor_keys_in"
PASSWORD_FILE = '/mnt/usb1/webserver-password.txt'
#FRAME_HANDLE_DELAY_S = 0.08
FRAME_HANDLE_DELAY_S = 0.05

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
    if ':' in supplied_pw:
      supplied_pw = supplied_pw.split(':', 1)[1].strip()

    supplied_pw = supplied_pw.strip()
  except:
    traceback.print_exc()
  # print(f'supplied_pw = {supplied_pw}')
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
      Numbers turn into key presses, 'r' becomes a clockwise dial rotation, 'l' becomes a counter-clockwise dial rotation.
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
        input_file_keycode_s += f'115,'
      elif number == 'l': # left/counte-clockwise rotation dial
        input_file_keycode_s += f'114,'
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



def calc_alpha_beta_auto_brightness_adj(gray_img):
  clip_hist_percent = 20

  # Calculate grayscale histogram
  hist = cv2.calcHist([gray_img],[0],None,[256],[0,256])
  hist_size = len(hist)

  # Calculate cumulative distribution from the histogram
  accumulator = []
  accumulator.append(float(hist[0]))
  for index in range(1, hist_size):
      accumulator.append(accumulator[index -1] + float(hist[index]))

  # Locate points to clip
  maximum = accumulator[-1]
  clip_hist_percent *= (maximum/100.0)
  clip_hist_percent /= 2.0

  # Locate left cut
  minimum_gray = 0
  while accumulator[minimum_gray] < clip_hist_percent:
      minimum_gray += 1

  # Locate right cut
  maximum_gray = hist_size -1
  while accumulator[maximum_gray] >= (maximum - clip_hist_percent):
      maximum_gray -= 1

  # Calculate alpha and beta values
  alpha = 255 / (maximum_gray - minimum_gray)
  beta = -minimum_gray * alpha

  return alpha, beta

def brightness_from_px(pixel):
  if len(pixel) == 3:
    # Assume BGR
    B = int(pixel[0])
    G = int(pixel[1])
    R = int(pixel[2])
    return int( float(R+R+R+B+G+G+G+G)/6.0 ) # Fast approx from https://stackoverflow.com/a/596241

  elif len(pixel) == 1:
    # Assume gray
    return pixel[0]

  else:
    raise Exception(f'Error, bad pixel value! pixel = {pixel}')

def count_num_true_ahead(signal, begin_i):
  num_true_ahead = 0
  for i in range(begin_i, len(signal)):
    if not signal[i]:
      break
    num_true_ahead += 1
  return num_true_ahead


async def do_image_analysis_processing(img):
  global last_s_when_gpio_motor_is_active
  # if the image is not the same size as our research texts, fix it!
  img_h, img_w, img_channels = img.shape
  if img_w != 640 or img_h != 480:
    print(f'WARNING: input image was {img_w}x{img_h} pixels, we resized to 640x480')
    img = cv2.resize(img, (640, 480))

  # This variable is returned alongside the debug frame.
  # When None indicates no rails detected!
  rail_px_diff = None

  # First let's crop to a manually-measured section we want to measure.
  crop_x = 175
  crop_y = 200
  crop_w = 425 - crop_x
  crop_h = 400 - crop_y

  cropped = img[crop_y:crop_y+crop_h, crop_x:crop_x+crop_w]

  # Next, let's normalize the input images using a common technique
  # to normalize the contrast and brightness incoming images.
  # If the brightness of the room/track changes this will prevent the algorithms
  # below from failing.
  # See https://stackoverflow.com/questions/56905592/automatic-contrast-and-brightness-adjustment-of-a-color-photo-of-a-sheet-of-pape

  gray = cv2.cvtColor(cropped, cv2.COLOR_BGR2GRAY)
  alpha, beta = calc_alpha_beta_auto_brightness_adj(gray)
  auto_adj_img = cv2.convertScaleAbs(cropped, alpha=alpha, beta=beta)
  # For diagnostics, we write to this so our output doesn't change the input (auto_adj_img) being processed
  debug_adj_img = auto_adj_img.copy()

  # We also use these manually measured offsets to insersect the
  # table rail and layout-side rail.
  # Coordinates are measured in absolute units and converted at-time-of-use
  table_rail_y = 330
  layout_rail_y = 350
  rail_pair_width_px = 96 # measured center-to-center

  # Calc crop-space rail_y values
  crop_table_rail_y = table_rail_y-crop_y
  crop_layout_rail_y = layout_rail_y-crop_y

  # Log debug assumptions
  cv2.line(debug_adj_img, (0, table_rail_y-crop_y), (crop_w, crop_table_rail_y), (255, 0, 0), thickness=1)
  cv2.line(debug_adj_img, (0, layout_rail_y-crop_y), (crop_w, crop_layout_rail_y), (0, 255, 0), thickness=1)

  # Scan along table_rail_y to find two high signals approx rail_pair_width_px apart,
  # and record X coords of both.
  table_rail_brightnesses = []
  layout_rail_brightnesses = []
  for x in range(0, crop_w):
    table_px = auto_adj_img[crop_table_rail_y,x]
    layout_px = auto_adj_img[crop_layout_rail_y,x]
    table_rail_brightnesses.append(
      brightness_from_px(table_px)
    )
    layout_rail_brightnesses.append(
      brightness_from_px(layout_px)
    )

  avg_table_rail_brightnesses = sum(table_rail_brightnesses) / len(table_rail_brightnesses)
  avg_layout_rail_brightnesses = sum(layout_rail_brightnesses) / len(layout_rail_brightnesses)

  # The true average segmentation includes too much non-rail material -
  # therefore we increase the "average" brightness up by 50% to capture
  # somethig closer to the top 25% brightness values
  avg_table_rail_brightnesses *= 1.35
  avg_layout_rail_brightnesses *= 1.35

  table_rail_signal = [x > avg_table_rail_brightnesses for x in table_rail_brightnesses]
  layout_rail_signal = [x > avg_layout_rail_brightnesses for x in layout_rail_brightnesses]

  # Log more
  for x in range(0, crop_w):
    debug_adj_img[crop_table_rail_y+1,x] = [255,255,255] if table_rail_signal[x] else [0,0,0]
    debug_adj_img[crop_table_rail_y+2,x] = [255,255,255] if table_rail_signal[x] else [0,0,0]

    debug_adj_img[crop_layout_rail_y+1,x] = [255,255,255] if layout_rail_signal[x] else [0,0,0]
    debug_adj_img[crop_layout_rail_y+2,x] = [255,255,255] if layout_rail_signal[x] else [0,0,0]

  # Now we scan for the FIRST rail from the left ->
  # by checking the signal True values AND reading the same TRUE value
  # rail_pair_width_px items later
  table_rail_left_idxs = None
  layout_rail_left_idxs = None
  for x in range(0, crop_w-rail_pair_width_px):
    if table_rail_left_idxs is None and table_rail_signal[x] and table_rail_signal[x+rail_pair_width_px]:
      # Found it!
      #table_rail_left_idxs = (x, x+rail_pair_width_px)
      center_offset = int(count_num_true_ahead(table_rail_signal, x) // 2)
      table_rail_left_idxs = (x + center_offset, x + rail_pair_width_px + center_offset)

    if layout_rail_left_idxs is None and layout_rail_signal[x] and layout_rail_signal[x+rail_pair_width_px]:
      # Found it!
      #layout_rail_left_idxs = (x, x+rail_pair_width_px)
      center_offset = int(count_num_true_ahead(layout_rail_signal, x) // 2)
      layout_rail_left_idxs = (x + center_offset, x + rail_pair_width_px + center_offset)


  if not (table_rail_left_idxs is None):
    # Log the rail!
    x1, x2 = table_rail_left_idxs
    debug_adj_img[crop_table_rail_y+3, x1] = [0,0,255]
    debug_adj_img[crop_table_rail_y+4, x1] = [0,0,255]

    debug_adj_img[crop_table_rail_y+3, x2] = [0,0,255]
    debug_adj_img[crop_table_rail_y+4, x2] = [0,0,255]

  if not (layout_rail_left_idxs is None):
    # Log the rail!
    x1, x2 = layout_rail_left_idxs
    debug_adj_img[crop_layout_rail_y+3, x1] = [0,0,255]
    debug_adj_img[crop_layout_rail_y+4, x1] = [0,0,255]

    debug_adj_img[crop_layout_rail_y+3, x2] = [0,0,255]
    debug_adj_img[crop_layout_rail_y+4, x2] = [0,0,255]

  if table_rail_left_idxs is not None and layout_rail_left_idxs is not None:
    # Now we can see how much to move the table by!
    table_x1, table_x2 = table_rail_left_idxs
    layout_x1, layout_x2 = layout_rail_left_idxs

    x1_diff = layout_x1 - table_x1
    x2_diff = layout_x2 - table_x2 # this will be identical b/c detection uses rail_pair_width_px

    MAX_ALLOWED_RAIL_OFFSET = 1
    if abs(x1_diff) > MAX_ALLOWED_RAIL_OFFSET:
      cv2.arrowedLine(debug_adj_img, (table_x1, crop_table_rail_y-10), (layout_x1, crop_table_rail_y-10), (0,0,0), 2)
      cv2.arrowedLine(debug_adj_img, (table_x1, crop_table_rail_y-10), (layout_x1, crop_table_rail_y-10), (0,0,255), 1)

      # print(f'x1_diff = {x1_diff}')
      rail_px_diff = x1_diff # Write to our returned variable so processing logic can move table!

    else:
      # Rail position good!
      cv2.arrowedLine(debug_adj_img, (table_x1, max(0, crop_table_rail_y-60) ), (layout_x1, crop_table_rail_y), (0,0,0), 2)
      cv2.arrowedLine(debug_adj_img, (table_x1, max(0, crop_table_rail_y-60) ), (layout_x1, crop_table_rail_y), (0,255,0), 1)

  else:
    # No rails found!
    cv2.putText(debug_adj_img,'[NO RAIL]',
      (int(crop_w/6), max(0, crop_table_rail_y-40)),
      cv2.FONT_HERSHEY_SIMPLEX,
      1, (0,0,0), 2, 2
    )
    cv2.putText(debug_adj_img,'[NO RAIL]',
      (int(crop_w/6), max(0, crop_table_rail_y-40)),
      cv2.FONT_HERSHEY_SIMPLEX,
      1, (0,0,255), 1, 2
    )

  seconds_since_last_table_move = time.time() - last_s_when_gpio_motor_is_active
  if seconds_since_last_table_move > 22.0:
    # Notify user we will not be moving!
    cv2.putText(debug_adj_img,'SAFE TO MOVE',
      (4, 30),
      cv2.FONT_HERSHEY_SIMPLEX,
      1, (0,0,0), 2, 2
    )
    cv2.putText(debug_adj_img,'SAFE TO MOVE',
      (4, 30),
      cv2.FONT_HERSHEY_SIMPLEX,
      1, (0,255,0), 1, 2
    )

  return rail_px_diff, debug_adj_img







last_video_frame_num = 0
last_video_frame_s = 0
last_video_frame = None
async def read_video_t():
  global last_video_frame_num, last_video_frame_s, last_video_frame
  cam_num = 0
  camera = None
  try:
    frame_delay_s = FRAME_HANDLE_DELAY_S
    for cam_num in range(0, 99):
      try:
        camera = cv2.VideoCapture(f'/dev/video{cam_num}')
        if not camera.isOpened():
          raise RuntimeError('Cannot open camera')
      except:
        traceback.print_exc()
      if camera is not None:
        break

    if camera is None:
      try:
        camera = cv2.VideoCapture(-1) # auto-select "best"
      except:
        traceback.print_exc()

    if camera is None or not camera.isOpened():
        raise RuntimeError('Cannot open camera')

    none_reads_count = 0
    while True:

      _, img = camera.read()
      # img = cv2.resize(img, resolution)

      if img is None:
        none_reads_count += 1
        await asyncio.sleep(frame_delay_s) # allow other tasks to run
        if none_reads_count > 20:
          # This indicates we need to re-boot ourselves
          subprocess.run([
            'sudo', 'systemctl', 'restart', 'webserver.service'
          ], check=False)
          raise Exception(f'Read None from camera {none_reads_count} times!')
        continue

      rounded_frame_num = last_video_frame_num % 1000

      img_w, img_h, _img_channels = img.shape

      # Upper-left
      #cv2.putText(img, f'{rounded_frame_num}', (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (10, 10, 10), 3, cv2.LINE_AA) # black outline
      #cv2.putText(img, f'{rounded_frame_num}', (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (240, 240, 240), 2, cv2.LINE_AA) # White text

      # Lower-left
      cv2.putText(img, f'{rounded_frame_num}', (10, img_h-180), cv2.FONT_HERSHEY_SIMPLEX, 1, (10, 10, 10), 3, cv2.LINE_AA) # black outline
      cv2.putText(img, f'{rounded_frame_num}', (10, img_h-180), cv2.FONT_HERSHEY_SIMPLEX, 1, (240, 240, 240), 2, cv2.LINE_AA) # White text

      # last_video_frame = cv2.imencode('.jpg', img)[1].tobytes()
      rail_px_diff = None
      debug_img = img
      try:
        rail_px_diff, debug_img = await do_image_analysis_processing(img)
      except:
        traceback.print_exc()

      # Finally ensure debug_img is the same WIDTH as img
      debug_img = cv2.resize(debug_img, (640, 380))

      # combine images for a single output stream
      combined_img = cv2.vconcat([img, debug_img])
      #combined_img = img

      last_video_frame = cv2.imencode('.jpg', combined_img)[1].tobytes()

      # Signal to other thread images are ready!
      last_video_frame_s = time.time()
      last_video_frame_num += 1

      # Fork off do_automove_with_rail_px_diff to it's own thread,
      # I'd prefer it be as far away from image processing as possible
      # We also do not do automove on the first 4 frames on the assumption the
      # camera may be stabalizing itself, and the image we get will be washed out
      # and unusable for targeting.
      if last_video_frame_num > 4:
        asyncio.create_task(do_automove_with_rail_px_diff(rail_px_diff))

      await asyncio.sleep(frame_delay_s) # allow other tasks to run

  except:
    traceback.print_exc()
  finally:
    last_video_frame_num = 0
    last_video_frame_s = 0
    last_video_frame = None

AUTOMOVE_RESET_PERIOD_S = 30
AUTOMOVE_ADJUSTMENTS_ALLOWED = 28
last_automove_reset_s = 0
automove_remaining_adjustments_allowed = 0
last_s_when_gpio_motor_is_active = 0
async def do_automove_with_rail_px_diff(rail_px_diff):
  global last_automove_reset_s, automove_remaining_adjustments_allowed, last_s_when_gpio_motor_is_active
  try:
    # If we have not reset our safety limit, reset it
    if time.time() - last_automove_reset_s > AUTOMOVE_RESET_PERIOD_S:
      automove_remaining_adjustments_allowed = AUTOMOVE_ADJUSTMENTS_ALLOWED
      last_automove_reset_s = time.time()

    # If we have exhausted our safety limit, leave!
    if automove_remaining_adjustments_allowed < 1:
      if automove_remaining_adjustments_allowed > -1:
        print(f'automove_remaining_adjustments_allowed = {automove_remaining_adjustments_allowed}, leaving b/c < 1')
        automove_remaining_adjustments_allowed = -1 # to silence many errors!
      return

    # No rail detected, leave
    if rail_px_diff is None:
      return

    # Table is moving, leave
    if os.path.exists('/tmp/gpio_motor_is_active'):
      last_s_when_gpio_motor_is_active = time.time()
      print(f'/tmp/gpio_motor_is_active, not performing automove!')
      return

    # We also refuse to move IF it has been >6s since last_s_when_gpio_motor_is_active
    seconds_since_last_table_move = time.time() - last_s_when_gpio_motor_is_active
    if seconds_since_last_table_move > 22.0:
      print(f'seconds_since_last_table_move ({int(seconds_since_last_table_move)}) > 22.0, not performing automove!')
      return


    # Book keeping
    automove_remaining_adjustments_allowed -= 1

    # Perform the move!
    input_file_keycode_s = ''
    if rail_px_diff < 0:
      input_file_keycode_s = '115' # TODO these may be backwards!!!!
    else:
      input_file_keycode_s = '114'

    if os.path.exists('/tmp/no-automove.txt'):
      print(f'Refusing to write {input_file_keycode_s} to controller b/c /tmp/no-automove.txt exists!')
      return

    # Find first non-existent file under GPIO_MOTOR_KEYS_IN_DIR
    for _ in range(0, 100):
      input_num = random.randrange(1000, 9000)
      input_f_name = os.path.join(GPIO_MOTOR_KEYS_IN_DIR, f'{input_num}.txt')
      if os.path.exists(input_f_name):
        continue
      with open(input_f_name, 'w') as fd:
        fd.write(input_file_keycode_s)

      print(f'AutoMove Wrote "{input_file_keycode_s}" to {input_f_name}')

      break

  except:
    traceback.print_exc()

async def ensure_video_is_being_read():
  global last_video_frame_num, last_video_frame_s, last_video_frame
  last_frame_age = time.time() - last_video_frame_s
  if last_frame_age > 90.0:
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
      'taskset', '-cp', '0,1,2', str(pid)
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
