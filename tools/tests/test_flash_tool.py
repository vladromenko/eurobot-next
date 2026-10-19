#!/usr/bin/env python3
"""Programmer integration tests use ONLY a generated local fake CLI, never ST-LINK."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL=Path(__file__).resolve().parents[1]/"flash.py"
sys.path.insert(0, str(TOOL.parent))
from source_fingerprint import fingerprint
MOCK=r"""#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
args=sys.argv[1:]
with open(os.environ['MOCK_LOG'],'a') as f:f.write(json.dumps(args)+'\n')
scenario=os.environ.get('MOCK_SCENARIO','ok')
if scenario=='timeout':time.sleep(10)
if scenario=='error_zero':print('Error: simulated failure');sys.exit(0)
if args[:2]==['-l','st-link-only']:
 if scenario!='no_probe':print('ST-LINK SN  : TESTPROBE123')
 if scenario=='multiple':print('ST-LINK SN  : OTHERPROBE456')
 sys.exit(0)
print('Device ID   : '+('0x419' if scenario=='wrong_id' else '0x413'))
print('Flash size  : 1024 KBytes')
if '-u' in args:
 at=args.index('-u');address=int(args[at+1],0);size=int(args[at+2],0);path=Path(args[at+3])
 if address==0xE0042000:data=(0x10016413).to_bytes(4,'little')
 elif address==0x1FFF7A22:data=(512 if scenario=='wrong_size' else 1024).to_bytes(2,'little')
 else:
  if size==1048576 and scenario=='backup_fail':print('Error: read protection');sys.exit(1)
  data=Path(os.environ['MOCK_MEMORY']).read_bytes()[:size]
  if size==1048576 and scenario=='short_backup':data=data[:-1]
 path.write_bytes(data)
 if size==1048576 and scenario=='image_changed':Path(os.environ['MOCK_IMAGE']).write_bytes(b'changed during backup')
if '-w' in args:
 if scenario=='write_fail':print('Error: write failed');sys.exit(1)
 at=args.index('-w')
 if scenario!='verify_mismatch':
  memory=bytearray(Path(os.environ['MOCK_MEMORY']).read_bytes());image=Path(args[at+1]).read_bytes();memory[:len(image)]=image;Path(os.environ['MOCK_MEMORY']).write_bytes(memory)
"""

class FlashToolTest(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(prefix='eurobot flash test ')
  self.path=Path(self.temp.name)
  self.image=self.path/'eurobot_next.bin'
  self.image.write_bytes(struct.pack('<98I',0x20020000,*([0x08000189]*97))+b'\x11\x22'*64)
  self.manifest=self.path/'eurobot_next.manifest.json'
  self.metadata={'device_id':'0x413','flash_base':'0x08000000','flash_size':1048576,'image_size':self.image.stat().st_size,'sha256':hashlib.sha256(self.image.read_bytes()).hexdigest(),'filename':self.image.name}
  self.metadata['source_fingerprint']=fingerprint(TOOL.parent.parent)
  self.save_manifest()
  self.mock=self.path/'mock programmer'
  self.mock.write_text(MOCK);self.mock.chmod(0o755)
  self.memory=self.path/'memory.bin';self.memory.write_bytes(b'\xff'*1048576)
  self.log=self.path/'calls.jsonl';self.backups=self.path/'backup storage'
  self.environment={**os.environ,'MOCK_LOG':str(self.log),'MOCK_MEMORY':str(self.memory),'MOCK_IMAGE':str(self.image)}
 def tearDown(self):self.temp.cleanup()
 def save_manifest(self):self.manifest.write_text(json.dumps(self.metadata))
 def calls(self):return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
 def invoke(self,operation='flash',scenario='ok',extra=()):
  return subprocess.run([sys.executable,str(TOOL),operation,'--image',str(self.image),'--manifest',str(self.manifest),'--programmer',str(self.mock),'--backup-dir',str(self.backups),*extra],env={**self.environment,'MOCK_SCENARIO':scenario},capture_output=True,text=True,timeout=12)
 def permitted(self,scenario='ok'):
  return self.invoke(scenario=scenario,extra=('--allow-flash','--confirm-no-custom-bootloader'))
 def test_offline_default_never_invokes_programmer(self):
  result=self.invoke();self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(self.calls(),[])
 def test_stale_source_revision_never_invokes_programmer(self):
  self.metadata['source_fingerprint']='0'*64;self.save_manifest()
  result=self.permitted();self.assertNotEqual(result.returncode,0);self.assertEqual(self.calls(),[])
 def test_historical_manifest_without_source_revision_is_rejected(self):
  del self.metadata['source_fingerprint'];self.save_manifest()
  result=self.permitted();self.assertNotEqual(result.returncode,0);self.assertEqual(self.calls(),[])
 def test_changed_deployment_profile_is_rejected_before_usb(self):
  profile=self.path/'deployment.hpp';profile.write_text('profile before build')
  self.metadata['deployment_source']=str(profile)
  self.metadata['deployment_sha256']=hashlib.sha256(profile.read_bytes()).hexdigest()
  profile.write_text('profile after build');self.save_manifest()
  result=self.permitted();self.assertNotEqual(result.returncode,0);self.assertEqual(self.calls(),[])
 def test_missing_deployment_profile_is_rejected_before_usb(self):
  self.metadata['deployment_source']=str(self.path/'missing.hpp')
  self.metadata['deployment_sha256']='0'*64;self.save_manifest()
  result=self.permitted();self.assertNotEqual(result.returncode,0);self.assertEqual(self.calls(),[])
 def test_bootloader_confirmation_required_before_usb(self):
  result=self.invoke(extra=('--allow-flash',));self.assertNotEqual(result.returncode,0);self.assertEqual(self.calls(),[])
 def test_bad_digest_blocks_before_usb(self):
  self.metadata['sha256']='0'*64;self.save_manifest();self.assertNotEqual(self.permitted().returncode,0);self.assertEqual(self.calls(),[])
 def test_bad_vectors_block_before_usb(self):
  self.image.write_bytes(struct.pack('<II',0x20020004,0x08000009)+b'\x00'*128)
  self.metadata['sha256']=hashlib.sha256(self.image.read_bytes()).hexdigest();self.save_manifest()
  self.assertNotEqual(self.permitted().returncode,0);self.assertEqual(self.calls(),[])
 def test_invalid_isr_vector_blocks_before_usb(self):
  content=bytearray(self.image.read_bytes());struct.pack_into('<I',content,16*4,0x20000001);self.image.write_bytes(content)
  self.metadata['sha256']=hashlib.sha256(content).hexdigest();self.save_manifest()
  self.assertNotEqual(self.permitted().returncode,0);self.assertEqual(self.calls(),[])
 def test_numeric_manifest_supported(self):
  self.metadata['device_id']=0x413;self.metadata['flash_base']=0x08000000;self.save_manifest()
  self.assertEqual(self.invoke().returncode,0)
 def test_flash_backups_before_write_and_independent_readback(self):
  result=self.permitted();self.assertEqual(result.returncode,0,result.stderr)
  calls=self.calls();write=next(i for i,c in enumerate(calls) if '-w' in c)
  self.assertTrue(any('-u' in c and '1048576' in c for c in calls[:write]))
  self.assertTrue(any('-u' in c and str(self.image.stat().st_size) in c for c in calls[write+1:]))
  backup=next(self.backups.glob('*.bin'));self.assertEqual(backup.read_bytes(),b'\xff'*1048576)
  sidecar=json.loads(backup.with_suffix('.json').read_text());self.assertEqual(sidecar['sha256'],hashlib.sha256(backup.read_bytes()).hexdigest())
  forbidden={'-e','--erase','all','-ob','--optionbytes','-rdu','-rst','-hardRst','-g','--go','-run'}
  self.assertFalse(any(forbidden.intersection(c) for c in calls))
 def test_backup_failure_prevents_write(self):
  self.assertNotEqual(self.permitted('backup_fail').returncode,0);self.assertFalse(any('-w' in c for c in self.calls()));self.assertEqual(list(self.backups.glob('*.bin')),[])
 def test_short_backup_prevents_write(self):
  self.assertNotEqual(self.permitted('short_backup').returncode,0);self.assertFalse(any('-w' in c for c in self.calls()))
 def test_wrong_device_blocks_upload_and_write(self):
  self.assertNotEqual(self.permitted('wrong_id').returncode,0);self.assertFalse(any('-u' in c or '-w' in c for c in self.calls()))
 def test_wrong_capacity_blocks_write(self):
  self.assertNotEqual(self.permitted('wrong_size').returncode,0);self.assertFalse(any('-w' in c for c in self.calls()))
 def test_multiple_probes_require_selection(self):
  self.assertNotEqual(self.permitted('multiple').returncode,0);self.assertEqual(len(self.calls()),1)
 def test_missing_probe_blocks(self):
  self.assertNotEqual(self.permitted('no_probe').returncode,0);self.assertEqual(len(self.calls()),1)
 def test_error_text_even_exit_zero_blocks(self):
  self.assertNotEqual(self.permitted('error_zero').returncode,0);self.assertEqual(len(self.calls()),1)
 def test_write_failure_not_retried(self):
  self.assertNotEqual(self.permitted('write_fail').returncode,0);self.assertEqual(sum('-w' in c for c in self.calls()),1)
 def test_changed_image_after_backup_blocks_write(self):
  self.assertNotEqual(self.permitted('image_changed').returncode,0);self.assertFalse(any('-w' in c for c in self.calls()))
 def test_independent_verify_mismatch_rejected(self):
  self.assertNotEqual(self.permitted('verify_mismatch').returncode,0)
 def test_standalone_verify_only_reads(self):
  self.memory.write_bytes(self.image.read_bytes()+b'\xff'*(1048576-self.image.stat().st_size))
  result=self.invoke('verify',extra=('--allow-connect',));self.assertEqual(result.returncode,0,result.stderr)
  self.assertFalse(any('-w' in c for c in self.calls()));self.assertFalse(self.backups.exists())
 def test_verify_cannot_accept_flash_flag(self):
  self.assertNotEqual(self.invoke('verify',extra=('--allow-flash',)).returncode,0);self.assertEqual(self.calls(),[])
 def test_deadline_kills_programmer(self):
  result=self.invoke(scenario='timeout',extra=('--allow-flash','--confirm-no-custom-bootloader','--timeout','5'))
  self.assertNotEqual(result.returncode,0);self.assertIn('deadline exceeded',result.stderr)

if __name__=='__main__':unittest.main()
