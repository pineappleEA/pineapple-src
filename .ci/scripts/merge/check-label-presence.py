# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Checks to see if the specified pull request # has the specified tag
# Usage: python check-label-presence.py <Pull Request ID> <Name of Label>

import requests, json, sys

try:
    url = 'https://api.github.com/repos/yuzu-emu/yuzu/issues/%s' % sys.argv[1]
    response = requests.get(url)
    if (response.ok):
        j = json.loads(response.content)
        for label in j["labels"]:
            if label["name"] == sys.argv[2]:
                print('##vso[task.setvariable variable=enabletesting;]true')
                sys.exit()
except:
    sys.exit(-1)

print('##vso[task.setvariable variable=enabletesting;]false')
