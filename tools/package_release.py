#!/usr/bin/env python3
"""Package fresh validated software, then unpack/build/test it without hardware."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile
from source_fingerprint import validation_fingerprint
import flash

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'RELEASE_PACKAGE'
ZIP = ROOT / 'EUROBOT_NEXT_COMPLETE.zip'
AREAS = ('firmware', 'ros2_ws', 'tools', 'tests', 'examples', 'docs', '.github')
ROOT_FILES = ('CMakeLists.txt', 'CMakePresets.json', 'Dockerfile.ros2', '.dockerignore', '.clang-format',
              'README.md', 'WHAT_I_NEED_TO_DO.md', 'LICENSE-STATUS.md', '.gitignore')
IGNORED = {'.git', 'build', 'install', 'log', 'third_party', '__pycache__', '.DS_Store', 'backups'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_source(destination):
    for name in ROOT_FILES:
        shutil.copy2(ROOT / name, destination / name)
    for area in AREAS:
        for path in sorted((ROOT / area).rglob('*')):
            relative = path.relative_to(ROOT)
            if path.is_file() and not any(part in IGNORED for part in relative.parts) and path.suffix != '.pyc':
                if path.is_symlink():
                    raise RuntimeError('Source package must not follow symlinks: ' + str(relative))
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)


def copy_image(preset, destination):
    source = ROOT / 'build' / preset / 'firmware/eurobot_next'
    destination.mkdir(parents=True, exist_ok=True)
    for suffix in ('.elf', '.hex', '.bin', '.map'):
        shutil.copy2(source.with_suffix(suffix), destination / ('EUROBOT_NEXT' + suffix))
    metadata = json.loads(source.with_suffix('.manifest.json').read_text())
    metadata['filename'] = 'EUROBOT_NEXT.bin'
    manifest = destination / 'EUROBOT_NEXT.manifest.json'
    manifest.write_text(json.dumps(metadata, indent=2) + '\n')
    flash.image_info(destination / 'EUROBOT_NEXT.bin', manifest)
    flash.check_source_revision(manifest)


def make_zip():
    with zipfile.ZipFile(ZIP, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(PACKAGE.rglob('*')):
            if path.is_file():
                archive.write(path, 'RELEASE_PACKAGE/' + path.relative_to(PACKAGE).as_posix())


def run(command, cwd, log):
    log.write('$ ' + ' '.join(map(str, command)) + '\n'); log.flush()
    subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-unpack', action='store_true', help='Required for final delivery: fresh extracted host/ARM builds')
    args = parser.parse_args(argv)
    evidence = json.loads((ROOT / 'artifacts/validation/results.json').read_text())
    if not evidence['success'] or not evidence['sources_unchanged'] or evidence['verification_fingerprint'] != validation_fingerprint(ROOT):
        raise RuntimeError('Run fresh tools/validate.py successfully before packaging changed sources')
    for preset in ('stm32-diagnostic', 'stm32-diagnostic-debug'):
        image = ROOT / 'build' / preset / 'firmware/eurobot_next.bin'
        if digest(image) != evidence['images'][preset]['sha256']:
            raise RuntimeError('Image differs from final validation: ' + preset)
    if PACKAGE.exists():
        shutil.rmtree(PACKAGE)  # Only this dedicated generated directory is replaced.
    source = PACKAGE / 'source'; source.mkdir(parents=True)
    copy_source(source)
    copy_image('stm32-diagnostic', PACKAGE / 'firmware')
    copy_image('stm32-diagnostic-debug', PACKAGE / 'firmware/debug')
    for suffix in ('.bin', '.manifest.json'):
        shutil.copy2(PACKAGE / 'firmware' / ('EUROBOT_NEXT' + suffix), ROOT / ('EUROBOT_NEXT' + suffix))
    (PACKAGE / 'docs').mkdir()
    shutil.copy2(ROOT / 'docs/FINAL_READINESS_REPORT.md', PACKAGE / 'docs/FINAL_READINESS_REPORT.md')
    guide = (ROOT / 'WHAT_I_NEED_TO_DO.md').read_text().replace('](docs/', '](source/docs/')
    (PACKAGE / 'WHAT_I_NEED_TO_DO.md').write_text(guide)
    (PACKAGE / 'README.md').write_text('''# EUROBOT NEXT — комплект первого диагностического запуска

`firmware/EUROBOT_NEXT.bin` — образ DIAGNOSTIC_MODE для общей платыSTM32F407 без разрешения движения.
`firmware/debug/` — тот же аппаратный кандидат в Debug. Остальные ELF/HEX/MAP предназначены для анализа.
`source/` — полный собственный проект с CMake, ROS2 workspace, tools, tests, examples и закреплёнными зависимостями через tools/dependencies.sh.
`docs/FINAL_READINESS_REPORT.md` — итог; полная документация в `source/docs/`. Начните с WHAT_I_NEED_TO_DO.md; команды для распакованного комплекта даны там отдельным блоком.

ZIP — весь комплект; BIN записывается во внутреннюю Flash. Год прежней прошивки не определяет проводку; профиль2026/PO1…PO16 не выбран автоматически.
Движение в этом образе заблокировано. Для него нужен отдельный проверенный OPERATIONAL_MODE профиль и явный arm.
Никаких USB/SWD/UART операций упаковка не выполняла. Распространение только владельцу; права публичной публикации не установлены.
''')
    copied = PACKAGE / 'validation'; copied.mkdir()
    for name in ('results.json', 'source_integrity.log', 'static_analysis.log', 'ros2_jazzy.log', 'ros2_lyrical.log'):
        shutil.copy2(ROOT / 'artifacts/validation' / name, copied / name)
    shutil.copy2(ROOT / 'artifacts/elf-feature-audit.json', copied / 'elf-feature-audit.json')
    verification = {'hardware_access': 'NOT_PERFORMED', 'source_fingerprint': evidence['verification_fingerprint'],
                    'unpacked_clean_build': 'NOT_PERFORMED', 'dependencies': 'Pinned upstream repositories; run source/tools/dependencies.sh. No legacy application C included.'}
    make_zip()
    if args.check_unpack:
        log_path = ROOT / 'artifacts/package-clean-build.log'
        with tempfile.TemporaryDirectory(prefix='eurobot-clean-package-') as directory, log_path.open('w') as log:
            with zipfile.ZipFile(ZIP) as archive: archive.extractall(directory)
            extracted = Path(directory) / 'RELEASE_PACKAGE/source'
            if validation_fingerprint(extracted) != evidence['verification_fingerprint']:
                raise RuntimeError('Extracted source fingerprint differs')
            run([sys.executable, 'tools/flash.py', 'verify', '--image', '../firmware/EUROBOT_NEXT.bin'], extracted, log)
            run(['bash', 'tools/dependencies.sh'], extracted, log)
            for preset in ('host', 'stm32-diagnostic-debug', 'stm32-diagnostic'):
                run(['cmake', '--preset', preset, '--fresh'], extracted, log)
                run(['cmake', '--build', '--preset', preset, '--parallel'], extracted, log)
                if preset == 'host':
                    run(['ctest', '--preset', 'host', '--output-on-failure'], extracted, log)
            # Freshly relocated manifests must verify against extracted headers, not original paths.
            run([sys.executable, 'tools/flash.py', 'verify', '--image', 'build/stm32-diagnostic/firmware/eurobot_next.bin'], extracted, log)
        verification['unpacked_clean_build'] = 'PASS: extracted host tests,diagnostic Debug and Release ARM builds, relocated manifests'
        shutil.copy2(log_path, copied / 'package-clean-build.log')
    (copied / 'PACKAGE_VERIFICATION.json').write_text(json.dumps(verification, indent=2) + '\n')
    hashes = {path.relative_to(PACKAGE).as_posix(): digest(path) for path in sorted(PACKAGE.rglob('*')) if path.is_file()}
    (PACKAGE / 'SHA256SUMS.json').write_text(json.dumps(hashes, indent=2) + '\n')
    make_zip()
    with zipfile.ZipFile(ZIP) as archive:
        for name, sha in hashes.items():
            if hashlib.sha256(archive.read('RELEASE_PACKAGE/' + name)).hexdigest() != sha:
                raise RuntimeError('ZIP content checksum mismatch: ' + name)
    (ROOT / 'artifacts/package-result.json').write_text(json.dumps({**verification, 'zip_sha256': digest(ZIP),
          'zip_size': ZIP.stat().st_size, 'files': len(hashes), 'image_sha256': digest(ROOT / 'EUROBOT_NEXT.bin')}, indent=2) + '\n')
    print(str(ZIP)); print(str(ROOT / 'EUROBOT_NEXT.bin')); print(verification['unpacked_clean_build'])


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print('Package refused: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
