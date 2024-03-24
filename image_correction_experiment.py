#!/usr/env/bin python

import os
import sys
import subprocess
import shutil
import datetime
import traceback
import time
import math
from contextlib import contextmanager
import statistics
import random

python_libs_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '.py-env'))
os.makedirs(python_libs_dir, exist_ok=True)
sys.path.append(python_libs_dir)

try:
  import cv2
except:
  subprocess.run([
    sys.executable, '-m', 'pip', 'install', f'--target={python_libs_dir}', 'opencv-python'
  ])
  import cv2

try:
  import numpy
except:
  subprocess.run([
    sys.executable, '-m', 'pip', 'install', f'--target={python_libs_dir}', 'numpy'
  ])
  import numpy

try:
  import numba
except:
  subprocess.run([
    sys.executable, '-m', 'pip', 'install', f'--target={python_libs_dir}', 'numba'
  ])
  import numba

try:
  import skimage
except:
  subprocess.run([
    sys.executable, '-m', 'pip', 'install', f'--target={python_libs_dir}', 'scikit-image'
  ])
  import skimage

from skimage.transform import radon

@contextmanager
def timed(name=''):
  start = time.time()
  try:
    yield
  finally:
    end = time.time()
    s = end - start
    print(f'{name} took {s:.2f}s')


def main():
  video_device_num = 0
  image_file = None
  try:
    if len(sys.argv) > 1:
      video_device_num = int(sys.argv[1])
  except:
    if len(sys.argv) > 1:
      image_file = sys.argv[1]

  # From https://stackoverflow.com/questions/62576326/python3-process-and-display-webcam-stream-at-the-webcams-fps
  # create display window
  cv2.namedWindow('floatme', cv2.WINDOW_NORMAL)
  cv2.resizeWindow('floatme', 1920-50, 1080-300)

  if image_file is None:
    # initialize webcam capture object
    cap = cv2.VideoCapture(video_device_num)

    # retrieve properties of the capture object
    cap_width = cap.get(cv2.CAP_PROP_FRAME_WIDTH)
    cap_height = cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
    cap_fps = cap.get(cv2.CAP_PROP_FPS)
    fps_sleep = int(1000 / cap_fps)
    print('* Capture width:', cap_width)
    print('* Capture height:', cap_height)
    print('* Capture FPS:', cap_fps, 'ideal wait time between frames:', fps_sleep, 'ms')

    # initialize time and frame count variables
    last_time = datetime.datetime.now()
    frames = 0

    # main loop: retrieves and displays a frame from the camera
    while True:
        # blocks until the entire frame is read
        success, img = cap.read()
        frames += 1

        # compute fps: current_time - last_time
        delta_time = datetime.datetime.now() - last_time
        elapsed_time = delta_time.total_seconds()
        cur_fps = numpy.around(frames / elapsed_time, 1)

        img = do_track_detection(img, cap_width, cap_height)

        # draw FPS text and display image
        cv2.putText(img, f'FPS: {cur_fps}', (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (10, 10, 10), 3, cv2.LINE_AA)
        cv2.putText(img, f'FPS: {cur_fps}', (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (240, 240, 240), 2, cv2.LINE_AA)
        cv2.imshow('floatme', img)

        # wait 1ms for ESC to be pressed
        key = cv2.waitKey(1)
        if (key == 27):
            break
  else:
    #print(f'Opening {image_file}')
    print(f'Opening {sys.argv[1:]}')

    sleep_s = float(os.environ.get('SLEEP_S', '0.25'))
    print(f'SLEEP_S = {sleep_s}')

    orig_imgs = []
    for img_f in sys.argv[1:]:
      orig_imgs.append(
        cv2.imread(img_f, cv2.IMREAD_COLOR)
      )

    # Assume all test images are same size
    try:
      height, width = orig_imgs[0].shape
    except:
      height, width, pixel_geometry = orig_imgs[0].shape
    print(f'Image size: {width}, {height} (w,h)')

    while True:

      for orig_img in orig_imgs:
        img = do_track_detection(orig_img.copy(), width, height)

        cv2.imshow('floatme', img)

        # wait 1ms for ESC to be pressed
        key = cv2.waitKey(1)
        if (key == 27):
            break

        time.sleep(sleep_s)

  # release resources
  try:
    cv2.destroyAllWindows()
    cap.release()
  except:
    pass

# @numba.jit(nopython=True, nogil=True, cache=True)
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

# @numba.jit(nopython=True, nogil=True, cache=True)
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

def do_track_detection(img, width, height):

  int_a = 0
  if os.path.exists('/tmp/int_a'):
    with open('/tmp/int_a', 'r') as fd:
      try:
        int_a = int(fd.read().strip())
      except:
        traceback.print_exc()

  int_b = 1
  if os.path.exists('/tmp/int_b'):
    with open('/tmp/int_b', 'r') as fd:
      try:
        int_b = int(fd.read().strip())
      except:
        traceback.print_exc()

  img_w, img_h, _img_channels = img.shape

  # First let's crop to a manually-measured section we want to measure.
  crop_x = 150
  crop_y = 200
  crop_w = 450 - crop_x
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
  # therefore we increase the "average" brightness up by 65% to capture
  # somethig closer to the top 25% brightness values
  avg_table_rail_brightnesses *= 1.65
  avg_layout_rail_brightnesses *= 1.65

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
      table_rail_left_idxs = (x, x+rail_pair_width_px)

    if layout_rail_left_idxs is None and layout_rail_signal[x] and layout_rail_signal[x+rail_pair_width_px]:
      # Found it!
      layout_rail_left_idxs = (x, x+rail_pair_width_px)


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

    if abs(x1_diff) > 2:
      cv2.arrowedLine(debug_adj_img, (table_x1, crop_table_rail_y-10), (layout_x1, crop_table_rail_y-10), (0,0,0), 2)
      cv2.arrowedLine(debug_adj_img, (table_x1, crop_table_rail_y-10), (layout_x1, crop_table_rail_y-10), (0,0,255), 1)

      print(f'x1_diff = {x1_diff}')
      if x1_diff > 0:
        pass

      else: # x1_diff > 0
        pass
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


  img_final = cv2.hconcat(try_convert_to_rgb([
    #img, auto_adj_img
    auto_adj_img, debug_adj_img
  ]))

  return img_final

def try_convert_to_rgb(images):
  for i in range(0, len(images)):
    try:
      images[i] = cv2.cvtColor(images[i], cv2.COLOR_GRAY2RGB)
    except:
      pass
  return images


if __name__ == '__main__':
  main()

