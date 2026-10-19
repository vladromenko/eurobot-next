#!/usr/bin/env python3
"""Integrity/publication preflight. Does not grant redistribution rights."""
from __future__ import annotations
import argparse
import csv
import hashlib
import re
from pathlib import Path


def run() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--legacy-root', type=Path, help='Read-only original Eurobot directory')
    parser.add_argument('--publication', action='store_true', help='Require root LICENSE and retained vendor notices; manual rights review still required')
    args = parser.parse_args()
    project = args.project.resolve()
    manifest = project/'docs/legacy_source_manifest.csv'
    failures: list[str] = []
    warnings: list[str] = []
    records = list(csv.DictReader(manifest.open())) if manifest.exists() else []
    originals = {row['sha256']: row['source'] for row in records}
    if args.legacy_root:
        checked = 0
        legacy_root = args.legacy_root.resolve()
        for record in records:
            path = (legacy_root/record['source']).resolve()
            if not path.is_relative_to(legacy_root):
                failures.append('Unsafe path in legacy manifest')
            elif not path.is_file():
                failures.append('Missing legacy source: '+record['source'])
            elif hashlib.sha256(path.read_bytes()).hexdigest() != record['sha256']:
                failures.append('Changed legacy source: '+record['source'])
            else:
                checked += 1
        if not records:
            failures.append('No legacy source manifest records')
        print(f'Legacy reference integrity: {checked}/{len(records)} files match SHA256')
    else:
        warnings.append('Original integrity not checked: pass --legacy-root explicitly')

    secret_patterns = {
        'private key': re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----'),
        'GitHub token': re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,})'),
        'AWS access key': re.compile(rb'(?:AKIA|ASIA)[A-Z0-9]{16}'),
        'OpenAI token': re.compile(rb'sk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{40,}'),
    }
    ignored_dirs = {'.git', 'build', 'install', 'log', '__pycache__', '.cache', 'node_modules', 'RELEASE_PACKAGE'}
    own_extensions = {'.cpp', '.hpp', '.c', '.h', '.s', '.S'}
    scanned = 0
    for path in sorted(project.rglob('*')):
        relative = path.relative_to(project)
        if path.is_file() and not any(part in ignored_dirs or part.startswith('build-') for part in relative.parts):
            scanned += 1
            raw = path.read_bytes()
            third_party = 'third_party' in relative.parts
            if path.suffix in own_extensions and not third_party:
                digest = hashlib.sha256(raw).hexdigest()
                if digest in originals:
                    failures.append(f'Legacy application copied verbatim: {relative} equals {originals[digest]}')
            if path.name == '.env':
                failures.append('Private environment file in publication tree: '+str(relative))
            if not third_party and b'\0' not in raw[:4096] and len(raw) < 8_000_000:
                for label, pattern in secret_patterns.items():
                    match = pattern.search(raw)
                    if match:
                        line = raw[:match.start()].count(b'\n')+1
                        failures.append(f'Suspected {label}: {relative}:{line} (value redacted)')

    vendor_root = project/'firmware/third_party'
    vendors = [p for p in vendor_root.iterdir() if p.is_dir()] if vendor_root.exists() else []
    for vendor in sorted(vendors):
        notices = [p for p in vendor.rglob('*') if p.is_file() and
                   (p.name.lower().startswith(('license','licence','copying','copyright')))]
        if not notices:
            (failures if args.publication else warnings).append('No retained license notice for vendor directory: '+str(vendor.relative_to(project)))
        else:
            print(f'Vendor license notices: {vendor.name}: {len(notices)} file(s) retained')
    if not (project/'LICENSE').is_file():
        (failures if args.publication else warnings).append('Root LICENSE missing: distribution terms require an explicit rights decision')
    print(f'New project files scanned: {scanned}')
    for warning in warnings:
        print('WARNING: '+warning)
    for failure in failures:
        print('FAIL: '+failure)
    print('Manual review remains required for provenance, derivative material, personal data and distribution rights.')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(run())
