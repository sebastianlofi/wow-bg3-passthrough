#!/usr/bin/env python3
"""Fetch only locked build sources; do not run the upstream installer/fetch wrapper."""
from pathlib import Path
import hashlib,json,os,subprocess,urllib.request
here=Path(__file__).resolve().parent
build=Path(os.environ.get('WOWBG3_BUILD') or here.parents[1]/'build')
src=Path(os.environ.get('BG3LE_SRC') or build/'src/bg3le')
lock=json.loads((here/'externals.lock.json').read_text())
ext=src/'external/third_party'
ext.mkdir(parents=True,exist_ok=True)
for name,source in lock['git'].items():
    dest=ext/name
    if not dest.exists():
        subprocess.run(['git','init',str(dest)],check=True)
        subprocess.run(['git','-C',str(dest),'remote','add','origin',source['url']],check=True)
        subprocess.run(['git','-C',str(dest),'fetch','--depth','1','origin',source['commit']],check=True)
        subprocess.run(['git','-C',str(dest),'checkout','--detach',source['commit']],check=True)
    assert subprocess.check_output(['git','-C',str(dest),'rev-parse','HEAD'],text=True).strip()==source['commit']
    if name=='protobuf':
        subprocess.run(['git','-C',str(dest),'submodule','update','--init','--depth','1'],check=True)
archive=build/'noesis-3.1.7.zip'
if not archive.exists():
    with urllib.request.urlopen(lock['noesis']['url'],timeout=60) as response, archive.open('xb') as out:
        while chunk:=response.read(1048576): out.write(chunk)
assert hashlib.sha256(archive.read_bytes()).hexdigest()==lock['noesis']['sha256']
noesis=ext/'Noesis'
if not noesis.exists():
    noesis.mkdir()
    subprocess.run(['bsdtar','-xf',str(archive),'-C',str(noesis)],check=True)
header=noesis/'NoesisGUI-NativeSDK-win-3.1.7-Indie/Include/NsCore/TypePropertyImpl.h'
text=header.read_text()
old='void Get(const void* ptr, void* dest) const override;'
new='void Get(const void* ptr, void* dest) const;'
assert old in text or new in text
patched=text.replace(old,new)
if patched!=text:header.write_text(patched)
# The sniper builder applies this flag to libc++, abseil, protobuf and bg3le.
stamp=ext/'.libcxx-abi-flags'
flags='-D_LIBCPP_ABI_VARIANT_INDEX_TYPE_OPTIMIZATION\n'
if not stamp.exists() or stamp.read_text()!=flags:stamp.write_text(flags)
print('Locked sources and header-only Noesis SDK ready')
