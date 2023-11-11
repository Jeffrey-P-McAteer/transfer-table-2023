
import os
import sys
import subprocess
import traceback
import shutil
import asyncio
import dataclasses
import typing
import ctypes

PMEM_FILE = "/mnt/usb1/pmem.bin"
CMD_FILE = "/mnt/usb1/cmd.bin"
num_positions = 12

python_libs_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '.py-env'))
os.makedirs(python_libs_dir, exist_ok=True)
sys.path.append(python_libs_dir)

try:
  import aiohttp
except:
  subprocess.run([
    sys.executable, '-m', 'pip', 'install', f'--target={python_libs_dir}', 'aiohttp[speedups]'
  ])
  import aiohttp

import aiohttp.web

@dataclasses.dataclass
class PosDat(ctypes.Structure):
  _fields_ = [
    ('step_position', ctypes.c_int32),
    ('cm_position', ctypes.c_double),
  ]

class PMem(ctypes.Structure):
  _fields_ = [
    ('logical_position', ctypes.c_uint32),
    ('step_position', ctypes.c_int32),
    ('positions', num_positions * PosDat ),
  ]

  @staticmethod
  def from_bytes(b):
    return PMem.from_buffer_copy(b)


async def background_t():
  while True:
    await asyncio.sleep(1)
    #print('In background_t!')
    p = await read_pmem()
    #print(f'p.logical_position = {p.logical_position}')
    #print(f'p.step_position = {p.step_position}')
    #print(f'p.positions[2].step_position = {p.positions[2].step_position}')



async def on_startup(app_ref):
  bg_t_task = asyncio.create_task(background_t())


async def read_pmem():
  with open(PMEM_FILE, 'rb') as fd:
    return PMem.from_bytes( fd.read() )


async def index_handle(request):
    return aiohttp.web.Response(text='''
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>McAteer Transfer Table</title>
  </head>
  <body>
    <h2>McAteer Transfer Table</h2>
    <p>todo</p>
    <script>
window.ws = null;

function connect_ws() {
  if (window.ws != null && window.ws.readyState !== WebSocket.CLOSED) {
    return; // Already connected, yay!
  }
  window.ws_url = window.location.origin.replace('http', 'ws')+'/ws';
  console.log('Connecting to '+window.ws_url);
  window.ws = new WebSocket(window.ws_url);
  window.ws.addEventListener("open", (event) => {
    window.ws.send("Hello Server!");
  });
  window.ws.addEventListener("message", (event) => {
    console.log("Message from server ", event.data);
  });
}

setInterval(connect_ws, 2000); // every 2 seconds, re-connect to websocket if not already connected

    </script>
  </body>
</html>
'''.strip(), content_type='text/html')

async def websocket_handle(request):
  ws = aiohttp.web.WebSocketResponse()

  await ws.prepare(request)

  async for msg in ws:
      if msg.type == aiohttp.WSMsgType.TEXT:
          print(f'msg.data = {msg.data}')
          if msg.data == 'close':
              await ws.close()
          else:
              await ws.send_str(msg.data + '/answer')

      elif msg.type == aiohttp.WSMsgType.ERROR:
          print(f'ws connection closed with exception {ws.exception()}')

  print('websocket connection closed')

  return ws



def main(args=sys.argv):
  print(f'args = {args}')
  print(f'aiohttp = {aiohttp}')

  app = aiohttp.web.Application()
  app.on_startup.append(on_startup)
  app.add_routes([
    aiohttp.web.get('/',           index_handle),
    aiohttp.web.get('/index.html', index_handle),
    aiohttp.web.get('/ws',         websocket_handle),

  ])

  aiohttp.web.run_app(app)


if __name__ == '__main__':
  main()
