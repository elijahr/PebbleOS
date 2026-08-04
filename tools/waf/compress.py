# SPDX-FileCopyrightText: 2024 Google LLC
# SPDX-License-Identifier: Apache-2.0

from waflib.Errors import WafError


def compress(task):
    cmd = ["cp", task.inputs[0].abspath(), task.inputs[0].get_bld().abspath()]
    result = task.exec_command(cmd)
    if result != 0:
        raise WafError(
            "Failed to copy %s to %s (%s returned %d)!"
            % (
                task.inputs[0].abspath(),
                task.inputs[0].get_bld().abspath(),
                cmd[0],
                result,
            )
        )

    # --force: xz refuses with "File exists" (rc 1) when the output is already
    # there, and waf does not delete a failed task's outputs. Checking the
    # return code without it turns a rebuild into a failure.
    cmd = [
        "xz",
        "--force",
        "--keep",
        "--check=crc32",
        "--lzma2=dict=4KiB",
        task.inputs[0].get_bld().abspath(),
    ]
    result = task.exec_command(cmd)
    if result != 0:
        raise WafError(
            "Failed to compress %s (%s returned %d)!"
            % (task.inputs[0].get_bld().abspath(), cmd[0], result)
        )
