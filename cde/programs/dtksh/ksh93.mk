KSH93_REVISION = d8d2be2b1bf18144ea5dc32d91539368f1a5c9aa
KSH93_URL = https://codeload.github.com/ksh93/ksh/tar.gz/$(KSH93_REVISION)
KSH93_SHA256 = 5009000cd012d239360021d923f2f7fa9dec2a36d1dc6ba510b8f6363cfa364b
KSH93_PATCHES = patches/struct-stat-probe.patch patches/desktop-shell-options.patch
KSH93_TEST_PATCHES = patches/pipeline-tests.patch patches/tooltalk-version-test.patch \
                     patches/signal-startup-test.patch patches/subshell-startup-test.patch \
                     patches/pty-jobs.patch
