#!/usr/bin/env python3
"""Reproducible local validation. Never opens a programmer or moves robot hardware."""
import argparse, datetime, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path
from zoneinfo import ZoneInfo
from source_fingerprint import validation_fingerprint
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--legacy-root',type=Path)
parser.add_argument('--experimental-cxx26',action='store_true')
parser.add_argument('--ros2-docker',action='store_true',help='Build/test Jazzy and Lyrical with Docker; no hardware')
args=parser.parse_args();root=Path(__file__).resolve().parents[1]
initial_revision=validation_fingerprint(root)
out=root/'artifacts/validation';out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ);steps=[]
def run(name,command):
 print(name+': running',flush=True)
 result=subprocess.run(command,cwd=root,env=env,capture_output=True,text=True)
 (out/(name+'.log')).write_text('$ '+' '.join(map(str,command))+'\n'+result.stdout+result.stderr)
 steps.append({'name':name,'command':command,'returncode':result.returncode,'log':name+'.log'})
 print(name+(': PASS' if result.returncode==0 else ': FAIL (see artifacts/validation/'+name+'.log)'),flush=True)
 if result.returncode:finish(False);raise SystemExit(result.returncode)
def finish(success):
 images={}
 for preset in ('stm32-debug','stm32-release','stm32-board8-plan','stm32-robot2026','stm32-robot2025-debug','stm32-robot2025','stm32-diagnostic-debug','stm32-diagnostic','stm32-cxx26-probe'):
  path=root/'build'/preset/'firmware/eurobot_next.bin'
  if path.is_file():
   images[preset]={'image_size':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
 report={'timestamp':datetime.datetime.now(ZoneInfo('Europe/Moscow')).isoformat(),'success':success,'steps':steps,'images':images,'hardware_tests':'NOT_PERFORMED','ros2_colcon':{d:next((('PASS' if step['returncode']==0 else 'FAIL') for step in steps if step['name']=='ros2_'+d),'NOT_PERFORMED') for d in ('jazzy','lyrical')}}
 report['verification_fingerprint']=initial_revision
 report['sources_unchanged']=initial_revision==validation_fingerprint(root)
 report['success']=success and report['sources_unchanged']
 (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
run('format',['bash','tools/format.sh','--check'])
run('source_policy',[sys.executable,'tools/check_policy.py'])
run('dependencies',['bash','tools/dependencies.sh'])
for preset in ('host','host-sanitize'):
 run('configure_'+preset,['cmake','--preset',preset,'--fresh'])
 run('build_'+preset,['cmake','--build','--preset',preset,'--clean-first','--parallel'])
 run('test_'+preset,['ctest','--preset',preset,'--output-on-failure'])
for name in ('core','device','gripper','application','runtime'):
 run(name+'_unit_details',[str(root/'build/host-sanitize'/('eurobot_'+name+'_tests'))])
run('static_analysis',['bash','tools/static_analysis.sh','--build-dir','build/host-sanitize'])
for preset in ('stm32-debug','stm32-release','stm32-board8-plan','stm32-robot2026','stm32-robot2025-debug','stm32-robot2025','stm32-diagnostic-debug','stm32-diagnostic'):
 run('configure_'+preset,['cmake','--preset',preset,'--fresh'])
 run('build_'+preset,['cmake','--build','--preset',preset,'--clean-first','--parallel'])
 run('verify_'+preset,['bash','tools/verify.sh','--image','build/'+preset+'/firmware/eurobot_next.bin'])
run('linked_features',[sys.executable,'tools/audit_elf_features.py'])
if args.experimental_cxx26:
 for preset in ('cxx26-probe','stm32-cxx26-probe'):
  run('configure_'+preset,['cmake','--preset',preset,'--fresh'])
  run('build_'+preset,['cmake','--build','--preset',preset,'--clean-first','--parallel'])
  if preset=='cxx26-probe':run('test_'+preset,['ctest','--preset',preset,'--output-on-failure'])
 run('verify_stm32-cxx26-probe',['bash','tools/verify.sh','--image','build/stm32-cxx26-probe/firmware/eurobot_next.bin'])
if args.ros2_docker:
 for distro in ('jazzy','lyrical'):
  run('ros2_'+distro,['bash','ros2_ws/tools/build_ros2.sh','--docker','--distro',distro])
run('python_syntax',[sys.executable,'-m','compileall','-q','tools','tests/flash_test.py'])
audit=[sys.executable,'tools/audit_sources.py']
if args.legacy_root:audit+=['--legacy-root',str(args.legacy_root)]
run('source_integrity',audit)
finish(True)
print('All available local checks passed. Hardware and full ROS runtime results are separate.',flush=True)
