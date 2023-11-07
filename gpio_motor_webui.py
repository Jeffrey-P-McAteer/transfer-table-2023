
import os
import sys
import subprocess
import traceback
import shutil


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


async def handle(request):
    name = request.match_info.get('name', "Anonymous")
    text = "Hello, " + name
    return aiohttp.web.Response(text=text)

def main(args=sys.argv):
  print(f'args = {args}')
  print(f'aiohttp = {aiohttp}')

  app = aiohttp.web.Application()
  app.add_routes([
    aiohttp.web.get('/', handle),
    aiohttp.web.get('/{name}', handle)
  ])

  aiohttp.web.run_app(app)


if __name__ == '__main__':
  main()
