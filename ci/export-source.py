"""Overlay the working source on the clean shallow clone, never ignored user files."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

source, destination = map(Path, sys.argv[1:])
files = subprocess.check_output(['git', '-c', f'safe.directory={source}', '-C', str(source),
                                 'ls-files', '--cached', '--others', '--exclude-standard', '-z'])
excluded = {'.git', '.aws', '.agents', '.codex', '.claude', '.tools'}
for raw in set(files.split(b'\0')) - {b''}:
    relative = Path(os.fsdecode(raw))
    if relative.is_absolute() or '..' in relative.parts or relative.parts[0] in excluded:
        continue
    original, target = source / relative, destination / relative
    if not original.exists() and not original.is_symlink():
        target.unlink(missing_ok=True)
        continue
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink(): target.unlink()
    shutil.copy2(original, target, follow_symlinks=False)
