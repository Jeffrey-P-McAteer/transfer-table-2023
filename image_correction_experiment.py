#!/usr/env/bin python

import os
import sys
import subprocess
import shutil
import datetime
import traceback
import time

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
        cv2.imshow("webcam", img)

        # wait 1ms for ESC to be pressed
        key = cv2.waitKey(1)
        if (key == 27):
            break
  else:
    #print(f'Opening {image_file}')
    print(f'Opening {sys.argv[1:]}')

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

        cv2.imshow("webcam", img)

        # wait 1ms for ESC to be pressed
        key = cv2.waitKey(1)
        if (key == 27):
            break

        time.sleep(0.25)

  # release resources
  try:
    cv2.destroyAllWindows()
    cap.release()
  except:
    pass


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

  #print(f'contours = {contours}')
  #cv2.drawContours(img_gray, contours, -1, 255, 3)

  # find the biggest countour (c) by the area
  largest_contour = max(contours, key = cv2.contourArea)


  cv2.drawContours(img_gray, [largest_contour], -1, 255, 2)
  cv2.drawContours(img, [largest_contour], -1, (0, 0, 255), 2)

  #x,y,w,h = cv2.boundingRect(largest_contour)
  #print(f'x,y,w,h = {(x,y,w,h)}')

  rect = cv2.minAreaRect(largest_contour)
  box = cv2.boxPoints(rect)
  box = numpy.int0(box)

  print(f'box = {box}')
  cv2.drawContours(img,[box],0, (0,255,0), 2)
  cv2.drawContours(img_gray,[box],0, 20, 2)

  line_x1 = box[0][0]
  line_y1 = box[0][1]

  line_x2 = box[1][0]
  line_y2 = box[1][1]

  cv2.line(img, (line_x1, line_y1), (line_x2, line_y2), (255, 0, 0), thickness=2)
  cv2.line(img_gray, (line_x1, line_y1), (line_x2, line_y2), 120, thickness=2)


  dbg_s = f'A: {int_a} B: {int_b}'
  cv2.putText(img, dbg_s, (10, int(height-30)), cv2.FONT_HERSHEY_SIMPLEX, 1, (10, 10, 10), 3, cv2.LINE_AA)
  cv2.putText(img, dbg_s, (10, int(height-30)), cv2.FONT_HERSHEY_SIMPLEX, 1, (240, 240, 240), 1, cv2.LINE_AA)

  img_final = cv2.hconcat([
    img, cv2.cvtColor(img_gray,cv2.COLOR_GRAY2RGB)
  ])
  return img_final



if __name__ == '__main__':
  main()

