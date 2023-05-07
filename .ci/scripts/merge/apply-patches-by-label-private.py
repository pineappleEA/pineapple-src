# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Download all pull requests as patches that match a specific label
# Usage: python download-patches-by-label.py <Label to Match> <Root Path Folder to DL to>

import requests, sys, json, shutil, subprocess, os, traceback

org = os.getenv("PRIVATEMERGEORG", "yuzu-emu")
repo = os.getenv("PRIVATEMERGEREPO", "yuzu-private")
tagline = sys.argv[3]
user = sys.argv[1]

dl_list = {}

TAG_NAME = sys.argv[2]

def check_individual(repo_id, pr_id):
    url = 'https://%sdev.azure.com/%s/%s/_apis/git/repositories/%s/pullRequests/%s/labels?api-version=5.1-preview.1' % (user, org, repo, repo_id, pr_id)
    response = requests.get(url)
    if (response.ok):
        try:
            js = response.json()
            return any(tag.get('name') == TAG_NAME for tag in js['value'])
        except:
            return False
    return False

def merge_pr(pn, ref):
    print("Matched PR# %s" % pn)
    print(subprocess.check_output(["git", "fetch", "https://%sdev.azure.com/%s/_git/%s" % (user, org, repo), ref, "-f", "--no-recurse-submodules"]))
    print(subprocess.check_output(["git", "merge", "--squash", 'origin/' + ref.replace('refs/heads/','')]))
    print(subprocess.check_output(["git", "commit", "-m\"Merge %s PR %s\"" % (tagline, pn)]))

def main():
    url = 'https://%sdev.azure.com/%s/%s/_apis/git/pullrequests?api-version=5.1' % (user, org, repo)
    response = requests.get(url)
    if (response.ok):
        js = response.json()
        tagged_prs = filter(lambda pr: check_individual(pr['repository']['id'], pr['pullRequestId']), js['value'])
        map(lambda pr: merge_pr(pr['pullRequestId'], pr['sourceRefName']), tagged_prs)

if __name__ == '__main__':
    try:
        main()
    except:
        traceback.print_exc(file=sys.stdout)
        sys.exit(-1)
