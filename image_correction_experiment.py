#!/usr/env/bin python

import os
import sys
import subprocess
import shutil
import datetime


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
  # From https://stackoverflow.com/questions/62576326/python3-process-and-display-webcam-stream-at-the-webcams-fps
  # create display window
  cv2.namedWindow("webcam", cv2.WINDOW_NORMAL)

  # initialize webcam capture object
  cap = cv2.VideoCapture(0)

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

      # draw FPS text and display image
      cv2.putText(img, 'FPS: ' + str(cur_fps), (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2, cv2.LINE_AA)
      cv2.imshow("webcam", img)

      # wait 1ms for ESC to be pressed
      key = cv2.waitKey(1)
      if (key == 27):
          break

  # release resources
  cv2.destroyAllWindows()
  cap.release()




if __name__ == '__main__':
  main()

