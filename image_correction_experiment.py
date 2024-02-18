#!/usr/env/bin python

import os
import sys
import subprocess
import shutil
import datetime
import traceback
import time
import math

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
  cv2.namedWindow("floatme", cv2.WINDOW_NORMAL)

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
        cv2.imshow("floatme", img)

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

        cv2.imshow("floatme", img)

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

# Stolen math https://stackoverflow.com/questions/34372480/rotate-point-about-another-point-in-degrees-python
def rotate(origin, point, angle):
    """
    Rotate a point counterclockwise by a given angle around a given origin.

    The angle should be given in radians.
    """
    ox, oy = origin
    px, py = point

    qx = ox + math.cos(angle) * (px - ox) - math.sin(angle) * (py - oy)
    qy = oy + math.sin(angle) * (px - ox) + math.cos(angle) * (py - oy)
    return qx, qy


def do_track_detection(img, width, height):

  int_a = 120
  if os.path.exists('/tmp/int_a'):
    with open('/tmp/int_a', 'r') as fd:
      try:
        int_a = int(fd.read().strip())
      except:
        traceback.print_exc()

  int_b = 450
  if os.path.exists('/tmp/int_b'):
    with open('/tmp/int_b', 'r') as fd:
      try:
        int_b = int(fd.read().strip())
      except:
        traceback.print_exc()

  # Step one - use two magic numbers (experimentally discovered)
  # to throw out noise in the image
  #img = cv2.Canny(img, int_a, int_b)
  #img = cv2.Canny(img, 120, 450)


  img_gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

  ret,thresh = cv2.threshold(img_gray, 210, 260, 0)
  contours, hierarchy = cv2.findContours(thresh.astype(numpy.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)

  rail_mask_img = img.copy()

  colors = [
    (255, 0, 0),
    (255, 255, 0),
    (255, 255, 255),
    (0, 255, 0),
    (0, 255, 255),
    (0, 0, 255),
  ]

  img_w, img_h, _img_channels = img.shape
  img_center_x = int(img_w / 2.0)
  img_center_y = int(img_h / 2.0)

  def contour_railiness(c):
    xy_list = list()
    for sublist in c:
      for xy_pair in sublist:
        xy_list.append(
          (xy_pair[0], xy_pair[1])
        )

    # we rotate the contour until the xmin-ymax is as small as possible,
    # then throw out (return 99999) oriented contours which are not at least 3x as tall as they are wide.
    best_rotation_angle = 0.0
    smallest_rotation_xdiff = 99999.0
    angles_to_test = [math.pi * (f/32.0) for f in range(0, 32)]
    for rotation_angle in angles_to_test:
      # xy_list
      xmin = 9999.0
      xmax = 0.0
      for pt in xy_list:
        rx, ry = rotate((img_center_x, img_center_y), pt, rotation_angle)
        if rx > xmax:
          xmax = rx
        if rx < xmin:
          xmin = rx
      xdiff = abs(xmax - xmin)
      if xdiff < smallest_rotation_xdiff:
        smallest_rotation_xdiff = xdiff
        best_rotation_angle = rotation_angle

    #print(f'best_rotation_angle = {best_rotation_angle}')
    rotated_xy_list = list()
    for pt in xy_list:
      rotated_xy_list.append(
        rotate((img_center_x, img_center_y), pt, best_rotation_angle)
      )

    xdiff = max([x for (x,y) in rotated_xy_list ]) - min([x for (x,y) in rotated_xy_list ])
    ydiff = max([y for (x,y) in rotated_xy_list ]) - min([y for (x,y) in rotated_xy_list ])

    # Larger ydiffs relative to x will be first in line
    if int(xdiff) == 0:
      xdiff = 1.0

    return float(ydiff) / float(xdiff)


  for i,c in enumerate(sorted(contours, key=contour_railiness, reverse=True)[:6]):
    # c is a [ [[x,y]], [[x,y]], ] of contour points

    # compute the center of the contour
    try:
      M = cv2.moments(c)
      cX = int(M["m10"] / M["m00"])
      cY = int(M["m01"] / M["m00"])

      cv2.drawContours(rail_mask_img, [c], -1, colors[i], 2)

      dbg_s = f'{i}-{contour_railiness(c)}'
      cv2.putText(rail_mask_img, dbg_s, (cX, cY), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 0, 0), 3, cv2.LINE_AA)
      cv2.putText(rail_mask_img, dbg_s, (cX, cY), cv2.FONT_HERSHEY_SIMPLEX, 1, colors[i], 1, cv2.LINE_AA)
    except:
      pass



  dbg_s = f'A: {int_a} B: {int_b}'
  cv2.putText(img, dbg_s, (10, int(height-30)), cv2.FONT_HERSHEY_SIMPLEX, 1, (10, 10, 10), 3, cv2.LINE_AA)
  cv2.putText(img, dbg_s, (10, int(height-30)), cv2.FONT_HERSHEY_SIMPLEX, 1, (240, 240, 240), 1, cv2.LINE_AA)

  img_final = cv2.hconcat(try_convert_to_rgb([
    #img, search_img, search_masked
    img, rail_mask_img
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

