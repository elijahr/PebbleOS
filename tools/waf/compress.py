# SPDX-FileCopyrightText: 2024 Google LLC
# SPDX-License-Identifier: Apache-2.0


def compress(task):
    cmd = ["cp", task.inputs[0].abspath(), task.inputs[0].get_bld().abspath()]
    ret = task.exec_command(cmd)
    if ret:
        return ret

    cmd = [
        "xz",
        "--force",
        "--keep",
        "--check=crc32",
        "--lzma2=dict=4KiB",
        task.inputs[0].get_bld().abspath(),
    ]
    return task.exec_command(cmd)
