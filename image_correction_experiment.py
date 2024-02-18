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


  # cv2.drawContours(img_gray, [largest_contour], -1, 255, 2)
  cv2.drawContours(img, [largest_contour], -1, (0, 0, 255), 2)

  #x,y,w,h = cv2.boundingRect(largest_contour)
  #print(f'x,y,w,h = {(x,y,w,h)}')

  rect = cv2.minAreaRect(largest_contour)
  box = cv2.boxPoints(rect)
  box = numpy.int0(box)

  #print(f'box = {box}')
  cv2.drawContours(img,[box],0, (0,255,0), 2)
  # cv2.drawContours(img_gray,[box],0, 20, 2)

  line_x1 = box[0][0]
  line_y1 = box[0][1]

  line_x2 = box[1][0]
  line_y2 = box[1][1]

  # print(f'Line {(line_x1, line_y1)} -> {(line_x2, line_y2)}')

  cv2.line(img, (line_x1, line_y1), (line_x2, line_y2), (255, 0, 0), thickness=2)
  # cv2.line(img_gray, (line_x1, line_y1), (line_x2, line_y2), 120, thickness=2)

  search_h = 520

  line_rise = abs(line_y2 - line_y1)
  line_run = abs(line_x2 - line_x1)
  line_ratio = line_rise / line_run
  line_inv_ratio = line_run / line_rise

  search_delta_x = search_h * line_inv_ratio
  #search_delta_y = search_delta_x * line_inv_ratio

  #print(f'search_delta_x = {search_delta_x}')
  #print(f'search_delta_y = {search_delta_y}')

  # Define points in input image: top-left, top-right, bottom-right, bottom-left
  pts0 = numpy.float32([
    [line_x1 - search_delta_x, line_y1], [line_x1,line_y1],
    [line_x2,line_y2], [line_x2 - search_delta_x, line_y2]
  ])

  cv2.line(img, (int(line_x1 - search_delta_x), int(line_y1)), (int(line_x2 - search_delta_x), int(line_y2)), (255, 255, 0), thickness=2)

  # Define corresponding points in output image - this rotates the image 90 degrees so the right edge of the source
  # becomes the bottom edge of the destination
  pts1 = numpy.float32([
    [0,0], [0, height],
    [width, height],[width,0],
  ])

  # Get perspective transform and apply it
  M = cv2.getPerspectiveTransform(pts0, pts1)
  search_img = cv2.warpPerspective(img, M, (width, height))

  # Now we are working in the rotated search_img space

  layout_track_center_mask = cv2.inRange(search_img, (90, 0, 0), (90+50, 255, 255)) # hue, saturation, value thresholds
  # ^ the cv2.bitwise_not of selected areas include a central rectangle around the table track!

  table_right_rail_mask = cv2.inRange(search_img, (0, 0, 210), (0, 255, 255)) # hue, saturation, value thresholds
  # ^ the cv2.bitwise_not of selected areas include an area covering the central rail in test frames

  rail_mask = cv2.inRange(search_img, (150, 0, 180), (255, 255, 255)) # hue, saturation, value thresholds
  rail_mask_img = cv2.bitwise_and(search_img, search_img, mask=rail_mask)

  # R&D
  #mask = cv2.inRange(search_img, (150, 0, int_a), (255, 255, int_a+int_b)) # hue, saturation, value thresholds
  # inv_mask = cv2.bitwise_not(mask)
  #search_masked = cv2.bitwise_and(search_img, search_img, mask=mask)

  search_latest = cv2.cvtColor(rail_mask_img, cv2.COLOR_BGR2GRAY)

  # Kernel size (a,b) must both be positive & odd
  search_latest = cv2.GaussianBlur(search_latest, (9,9), 0)

  ret,thresh = cv2.threshold(search_latest, 210, 260, 0)
  contours, hierarchy = cv2.findContours(thresh.astype(numpy.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
  # find the biggest countour (c) by the area
  largest_contour = max(contours, key = cv2.contourArea)
  # cv2.drawContours(img_gray, [largest_contour], -1, 255, 2)
  #cv2.drawContours(search_latest, [largest_contour], -1, 255, 2)

  #cv2.drawContours(search_latest, contours, -1, 255, 2)


  colors = [
    (255, 0, 0),
    (255, 255, 0),
    (255, 255, 255),
    (0, 255, 0),
    (0, 255, 255),
    (0, 0, 255),
  ]

  def contour_y_diff(c):
    min_y = 999999.0
    max_y = -1.0
    for sublist in c:
      for xy_pair in sublist:
        if xy_pair[1] < min_y:
          min_y = xy_pair[1]
        if xy_pair[1] > max_y:
          max_y = xy_pair[1]
    return max_y - min_y

  for i,c in enumerate(sorted(contours, key=contour_y_diff, reverse=True)[:6]):
    # c is a [ [[x,y]], [[x,y]], ] of contour points

    # compute the center of the contour
    try:
      M = cv2.moments(c)
      cX = int(M["m10"] / M["m00"])
      cY = int(M["m01"] / M["m00"])

      cv2.drawContours(rail_mask_img, [c], -1, colors[i], 2)

      dbg_s = f'{i}-{contour_y_diff(c)}'
      cv2.putText(rail_mask_img, dbg_s, (cX, cY), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 0, 0), 3, cv2.LINE_AA)
      cv2.putText(rail_mask_img, dbg_s, (cX, cY), cv2.FONT_HERSHEY_SIMPLEX, 1, colors[i], 1, cv2.LINE_AA)
    except:
      pass



  dbg_s = f'A: {int_a} B: {int_b}'
  cv2.putText(img, dbg_s, (10, int(height-30)), cv2.FONT_HERSHEY_SIMPLEX, 1, (10, 10, 10), 3, cv2.LINE_AA)
  cv2.putText(img, dbg_s, (10, int(height-30)), cv2.FONT_HERSHEY_SIMPLEX, 1, (240, 240, 240), 1, cv2.LINE_AA)

  img_final = cv2.hconcat(try_convert_to_rgb([
    #img, search_img, search_masked
    search_img, rail_mask_img, search_latest
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

