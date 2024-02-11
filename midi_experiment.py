
import os
import sys
import subprocess
import traceback
import time

python_libs_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '.py-env'))
os.makedirs(python_libs_dir, exist_ok=True)
sys.path.append(python_libs_dir)

try:
  import mido
except:
  subprocess.run([
    sys.executable, '-m', 'pip', 'install', f'--target={python_libs_dir}', 'mido[ports-rtmidi]'
  ])
  import mido


# aseqdump -p 'WORLDE'

print(f'Input names = {mido.get_input_names()}')
print(f'Output names = {mido.get_output_names()}')

WORLDE_out_name = [x for x in mido.get_output_names() if 'worlde' in x.lower()][0]
print(f'WORLDE_out_name = {WORLDE_out_name}')

outport = mido.open_output(WORLDE_out_name)

msg = mido.Message('note_on', channel=0, note=40, velocity=42, time=0)
outport.send(msg)

time.sleep(1.2)

msg = mido.Message('note_off', note=40)
outport.send(msg)

print('Printing events recieved...')
with mido.open_input(WORLDE_out_name) as inport:
  for msg in inport:
    print(f'msg = {msg}')




