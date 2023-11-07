
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
  </body>
</html>
'''.strip(), content_type='text/html')

def main(args=sys.argv):
  print(f'args = {args}')
  print(f'aiohttp = {aiohttp}')

  app = aiohttp.web.Application()
  app.add_routes([
    aiohttp.web.get('/', index_handle),
    aiohttp.web.get('/index.html', index_handle),

  ])

  aiohttp.web.run_app(app)


if __name__ == '__main__':
  main()
