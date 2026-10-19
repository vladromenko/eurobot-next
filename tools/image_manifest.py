#!/usr/bin/env python3
import argparse, hashlib, json, struct
from pathlib import Path
from source_fingerprint import fingerprint
parser=argparse.ArgumentParser()
parser.add_argument('image', type=Path)
parser.add_argument('--deployment-source', default='')
args=parser.parse_args()
image=args.image; data=image.read_bytes()
if len(data)<392 or len(data)>1024*1024: raise SystemExit("Invalid STM32F407VG image size")
msp,reset=struct.unpack_from("<II",data)
if msp%8 or not (0x20000000<msp<=0x20020000) or not(reset&1) or not (0x08000000<=reset-1<0x08000000+len(data)): raise SystemExit("Invalid boot vectors")
manifest={"device_id":"0x413","flash_base":"0x08000000","flash_size":1048576,"image_size":len(data),"sha256":hashlib.sha256(data).hexdigest(),"filename":image.name}
manifest["source_fingerprint"] = fingerprint(Path(__file__).resolve().parents[1])
profile = image.parent / "generated/deployment.hpp"
manifest["deployment_sha256"] = hashlib.sha256(profile.read_bytes()).hexdigest() if profile.is_file() else None
manifest["deployment_source"] = str(Path(args.deployment_source).resolve()) if args.deployment_source else None
if args.deployment_source:
    local_profile = Path(args.deployment_source).resolve()
    project_root = Path(__file__).resolve().parents[1]
    if local_profile.is_relative_to(project_root):
        manifest["deployment_relative"] = local_profile.relative_to(project_root).as_posix()
if args.deployment_source:
    selected=Path(args.deployment_source)
    if not selected.is_file() or hashlib.sha256(selected.read_bytes()).hexdigest() != manifest["deployment_sha256"]:
        raise SystemExit('Deployment source differs from the configured header; configure and build again')
elif manifest["deployment_sha256"] is not None:
    raise SystemExit('A configured deployment header needs an explicit source path')
image.with_suffix(".manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
print("Image manifest verified:",len(data),"bytes")
