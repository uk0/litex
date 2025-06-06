#
# This file is part of LiteX.
#
# Copyright (c) 2023 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabubucayre.com>
# SPDX-License-Identifier: BSD-2-Clause

import os
import sys
import math
import subprocess
from shutil import which, copyfile

from migen.fhdl.structure import _Fragment

from litex.build.generic_platform import *
from litex.build import tools
from litex.build.generic_toolchain import GenericToolchain
from litex.build.yosys_wrapper import YosysWrapper, yosys_args, yosys_argdict

# Constraints (.ccf) -------------------------------------------------------------------------------
def _build_ccf(named_sc, named_pc):
    ccf = []

    flat_sc = []
    for name, pins, other, resource in named_sc:
        if len(pins) > 1:
            for i, p in enumerate(pins):
                flat_sc.append((f"{name}[{i}]", p, other))
        else:
            flat_sc.append((name, pins[0], other))

    for name, pin, other in flat_sc:
        pin_cst = ""
        if pin != "X":
            pin_cst = f"Net \"{name}\" Loc = \"{pin}\""

        for c in other:
            if isinstance(c, Misc):
                pin_cst += f" | {c.misc}"
        pin_cst += ";"
        ccf.append(pin_cst)

    if named_pc:
        ccf.extend(named_pc)

    return ccf

# check if CFG IO pins are used
def _check_cfg_io_used(named_sc):
    for _, pins, _, _ in named_sc:
        for p in pins:
            if p.startswith("IO_WA_"):
                return True
    return False

# CologneChipToolchain -----------------------------------------------------------------------------

class CologneChipToolchain(GenericToolchain):
    attr_translate = {}
    supported_build_backend = ["litex", "edalize"]

    def __init__(self):
        super().__init__()
        self._yosys      = None
        # CologneChip does not have distributed RAM
        self._yosys_cmds = [
            "hierarchy -top {build_name}",
            "setattr -unset ram_style a:ram_style=distributed",
        ]
        self._synth_opts = "-nomx8 "

    def finalize(self):
        self._yosys = YosysWrapper(
            platform     = self.platform,
            build_name   = self._build_name,
            target       = "gatemate",
            output_name  = self._build_name+"_synth",
            template     = [],
            yosys_opts   = self._synth_opts,
            yosys_cmds   = self._yosys_cmds,
            synth_format = "v",
        )

    # IO Constraints (.ccf) ------------------------------------------------------------------------

    def build_io_constraints(self):
        ccf = _build_ccf(self.named_sc, self.named_pc)
        tools.write_to_file(f"{self._build_name}.ccf", "\n".join(ccf))
        return (f"{self._build_name}.ccf", "CCF")

    # Project (.ys) --------------------------------------------------------------------------------

    def build_project(self):
        """ create project files (mainly Yosys ys file)
        """
        self._yosys.build_script()

        # p_r complains about missing cc_worst_spd_dly.dly -> copy it in gateware directory
        if which("p_r"):
            p_r_path              = which("p_r")
            cc_worst_spd_dly_path = os.path.join(os.path.dirname(p_r_path), "cc_worst_spd_dly.dly")
            copyfile(cc_worst_spd_dly_path, os.path.join(self._build_dir, "cc_worst_spd_dly.dly"))
 
    # Script ---------------------------------------------------------------------------------------

    def build_script(self):
        """ create build_xxx.yy by using Yosys and p_r instances.
            Return
            ======
                the script name (str)
        """

        if sys.platform in ("win32", "cygwin"):
            script_ext      = ".bat"
            script_contents = "@echo off\nrem Autogenerated by LiteX / git: " + tools.get_litex_git_revision() + "\n\n"
            fail_stmt       = " || exit /b"
        else:
            script_ext      = ".sh"
            script_contents = "# Autogenerated by LiteX / git: " + tools.get_litex_git_revision() + "\nset -e\n"
            fail_stmt       = ""
        fail_stmt += "\n"

        # yosys call
        script_contents += self._yosys.get_yosys_call("script") + fail_stmt
        # use CFG IOs as user GPIOs
        cfg_io = "+uCIO" if _check_cfg_io_used(self.named_sc) else ""
        # p_r call
        script_contents += "p_r -ccf {build_name}.ccf -cCP {cfg_io} -A 1 -i {build_name}_synth.v -o {build_name} -lib ccag\n".format(
            build_name = self._build_name, cfg_io = cfg_io)

        script_file = "build_" + self._build_name + script_ext
        tools.write_to_file(script_file, script_contents, force_unix=False)

        return script_file

    def run_script(self, script):
        """ run build_xxx.yy script
        Parameters
        ==========
        script: str
            script name to use
        """
        if sys.platform in ("win32", "cygwin"):
            shell = ["cmd", "/c"]
        else:
            shell = ["bash"]

        if which("yosys") is None or which("p_r") is None:
            msg = "Unable to find CologneChip toolchain, please:\n"
            msg += "- Add Yosys/p_r toolchain to your $PATH."
            raise OSError(msg)

        if subprocess.call(shell + [script]) != 0:
            raise OSError("Error occured during Yosys/p_r's script execution.")


    def add_period_constraint(self, platform, clk, period):
        pass

def colognechip_args(parser):
    # TODO: yosys (default's yosys aren't supported
    # TODO: p_r args
    pass

def colognechip_argdict(args):
    # TODO: ditto
    return {}
