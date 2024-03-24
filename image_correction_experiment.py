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




  img_final = cv2.hconcat(try_convert_to_rgb([
    #img, auto_adj_img
    cropped, auto_adj_img
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

