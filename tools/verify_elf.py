#!/usr/bin/env python3
"""Check bare-metal ELF32 load regions, entry, hard-float ABI and exported symbols."""
import argparse, os, struct, subprocess
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('elf',type=Path);parser.add_argument('--readelf',required=True)
args=parser.parse_args();data=args.elf.read_bytes()
if data[:7]!=b"\x7fELF\x01\x01\x01":raise SystemExit("Expected ELF32 little-endian v1")
h=struct.unpack_from('<16sHHIIIIIHHHHHH',data)
if h[1]!=2 or h[2]!=40 or not h[4]&1:raise SystemExit("Expected executable ARM Thumb entry")
flash=(0x08000000,0x08100000);ram=(0x20000000,0x20020000);ccm=(0x10000000,0x10010000)
entry=h[4]&~1
if not flash[0]<=entry<flash[1]:raise SystemExit("Entry outside target Flash")
used_flash=0;used_ram=0;used_ccm=0
for i in range(h[10]):
 ph=struct.unpack_from('<IIIIIIII',data,h[5]+i*h[9])
 typ,offset,virt,physical,filesize,memsize,flags,alignment=ph
 if typ==1 and memsize:
  if filesize>memsize or offset+filesize>len(data) or (flags&3)==3:raise SystemExit("Invalid or writable-executable LOAD segment")
  if flash[0]<=virt and virt+memsize<=flash[1]:used_flash=max(used_flash,virt+memsize-flash[0])
  elif ram[0]<=virt and virt+memsize<=ram[1]:used_ram=max(used_ram,virt+memsize-ram[0])
  elif ccm[0]<=virt and virt+memsize<=ccm[1]:used_ccm=max(used_ccm,virt+memsize-ccm[0])
  else:raise SystemExit("LOAD segment outside STM32F407VG memory")
  if filesize and not (flash[0]<=physical and physical+filesize<=flash[1]):raise SystemExit("Initialized LOAD content must reside in Flash")
  if filesize:used_flash=max(used_flash,physical+filesize-flash[0])
attrs=subprocess.check_output([args.readelf,'-A',str(args.elf)],text=True,env={**os.environ,'LC_ALL':'C'})
for marker in ('v7E-M','VFPv4-D16','Tag_ABI_VFP_args: VFP registers'):
 if marker not in attrs:raise SystemExit("Wrong Cortex-M4F hard-float ABI: "+marker)
symbols=subprocess.check_output([args.readelf,'-Ws',str(args.elf)],text=True)
for name in ('Reset_Handler','SVC_Handler','PendSV_Handler','SysTick_Handler','USART1_IRQHandler','UART5_IRQHandler','TIM1_UP_TIM10_IRQHandler'):
 lines=[line for line in symbols.splitlines() if line.split() and line.split()[-1]==name]
 if len(lines)!=1 or ' UND ' in lines[0] or (name!='Reset_Handler' and ' WEAK ' in lines[0]):raise SystemExit("Required strong handler absent: "+name)
print(f"ELF verified: Cortex-M4F hard-float; Flash {used_flash}/1048576; SRAM with MSP reservation {used_ram}/131072; CCM {used_ccm}/65536; no RWX LOAD")

# These symbols must survive release section collection; a successful link alone
# would not demonstrate the embedded image contains the implemented subsystems.
demangled=subprocess.check_output([args.readelf,'-Ws','--demangle',str(args.elf)],text=True)
features=('eurobot::PumpBank::set','eurobot::Chassis::tick','eurobot::wire::Dispatcher::dispatch',
          'eurobot::gripper::Engine::start','eurobot::application::Application::tick',
          'eurobot::devices::StepperPlanner::move_profile','eurobot::devices::L6474::hard_reset',
          'eurobot::board::leds_write','eurobot::board::debug_write','maintenance_request')
for feature in features:
 if feature not in demangled:raise SystemExit('Required firmware implementation absent: '+feature)
for line in symbols.splitlines():
 words=line.split()
 if words and words[-1] in ('malloc','calloc','realloc','free','pvPortMalloc','vPortFree','__cxa_throw','__cxa_allocate_exception') and ' UND ' not in line:
  raise SystemExit('Unexpected heap/exception implementation in embedded ELF: '+words[-1])
print('Embedded feature symbols present; no linked heap allocation or C++ exception runtime')
