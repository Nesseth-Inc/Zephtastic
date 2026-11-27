#!/usr/bin/env python3
"""Materialize pinned dependencies in a NEW directory; never modifies a prior checkout."""
import argparse,hashlib,json,shutil,subprocess,urllib.request
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('destination',type=Path)
p.add_argument('--project', help='prepare only this project path from the dependency lock')
a=p.parse_args()
dest=a.destination.resolve()
if dest.exists():p.error('destination must not exist; shared or active dependencies are forbidden')
app=Path(__file__).resolve().parents[1];lock=json.loads((app/'dependency-lock.json').read_text())
if a.project is not None:
    lock = [project for project in lock if project['path'] == a.project]
    if not lock:
        p.error('unknown project path')
dest.mkdir(parents=True)
for project in lock:
 target=dest/project['path'];target.parent.mkdir(parents=True,exist_ok=True)
 subprocess.run(['git','init',str(target)],check=True)
 subprocess.run(['git','-C',str(target),'fetch','--depth','1',project['url'],project['commit']],check=True)
 subprocess.run(['git','-C',str(target),'checkout','--detach','FETCH_HEAD'],check=True)
 for file in project['overlay']:
  source=app/'dependency-overlay'/project['path']/file['path']
  if hashlib.sha256(source.read_bytes()).hexdigest()!=file['sha256']:raise RuntimeError('Overlay digest mismatch: '+str(source))
  output=target/file['path'];output.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,output)
 for blob in project.get('blobs',[]):
  # The pinned upstream module manifest defines these vendor libraries and license.
  assert (target/blob['license-path']).is_file(), 'Missing vendor blob license'
  output=target/'zephyr/blobs'/blob['path'];output.parent.mkdir(parents=True,exist_ok=True)
  with urllib.request.urlopen(blob['url'],timeout=90) as response:data=response.read()
  if hashlib.sha256(data).hexdigest()!=blob['sha256']:raise RuntimeError('Blob digest mismatch: '+blob['path'])
  output.write_bytes(data)
print('Pinned sources ready at',dest)
print('Install the matching Zephyr SDK 1.0.1 toolchains and Python build dependencies before compiling.')
