#!/usr/bin/env python3
"""Map the runner UID to a private home while preserving image system accounts."""
import json
from pathlib import Path
import sys

CI_HOME = '/tmp/pinhole-ci-home'


def account_files(passwd, group, uid, gid):
    if not 0 < uid < 2**32 - 1 or not 0 <= gid < 2**32 - 1:
        raise ValueError('Run CI with a regular user UID and a valid primary GID')
    users = [line.split(':') for line in passwd.splitlines() if line]
    groups = [line.split(':') for line in group.splitlines() if line]
    current = next((row for row in users if int(row[2]) == uid), None)
    names = {row[0] for row in users}
    name = current[0] if current else 'pinhole-ci'
    if current is None and name in names:
        name = f'pinhole-ci-{uid}'
    mapped = list(current) if current else [name, 'x', str(uid), str(gid), 'Pinhole CI', CI_HOME, '/bin/bash']
    mapped[3], mapped[5] = str(gid), CI_HOME
    users = [mapped if int(row[2]) == uid else row for row in users]
    if current is None:
        users.append(mapped)
    if not any(int(row[2]) == gid for row in groups):
        group_name = name if name not in {row[0] for row in groups} else f'pinhole-ci-{gid}'
        groups.append([group_name, 'x', str(gid), name])
    return '\n'.join(':'.join(row) for row in users) + '\n', '\n'.join(':'.join(row) for row in groups) + '\n'


def main():
    image, directory, uid, gid = sys.argv[1:]
    accounts = json.loads(Path(image).read_text())
    passwd, group = account_files(accounts['passwd'], accounts['group'], int(uid), int(gid))
    for filename, contents in [('passwd', passwd), ('group', group)]:
        path = Path(directory) / filename
        path.write_text(contents)
        path.chmod(0o644)  # The containing directory stays private (0700).


if __name__ == '__main__':
    main()
