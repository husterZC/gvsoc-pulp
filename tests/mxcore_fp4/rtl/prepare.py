#!/usr/bin/env python3
"""Apply the small, explicit FP4-only calibration variant to pinned MXCore."""
import argparse
import pathlib
import subprocess

REVISION = "edf45a65a9c19dcb901eed2148ab9621573a6621"


def prepare(root):
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    if actual != REVISION:
        raise SystemExit(f"Expected MXCore {REVISION}, found {actual}")
    edits = {
        "mxcore-rtl/src/mxcore_package.sv": [
            ("FP4BlockSize             = 2 * BlockSize", "FP4BlockSize             = BlockSize"),
        ],
        "mxcore-rtl/src/hwpe/mxcore_hwpe_package.sv": [
            ("MXCoreQuantResultDataWidth   = NPE*SRC_WIDTH", "MXCoreQuantResultDataWidth   = NPE*4"),
        ],
        "mxcore-rtl/src/hwpe/mxcore_hwpe_top.sv": [
            ("BlockSize / VectorSize", "FP4BlockSize / (2*VectorSize)"),
            ("mxcore_hwpe_block_quantizer #(", "mxcore_fp4_quantizer #("),
        ],
        "mxcore-rtl/src/hwpe/mxcore_hwpe_ctrl.sv": [
            ("BlockSize / VectorSize", "FP4BlockSize / (2*VectorSize)"),
        ],
    }
    for relative, replacements in edits.items():
        path = root / relative
        data = path.read_text()
        for before, after in replacements:
            if after in data:
                continue
            if before not in data:
                raise SystemExit(f"Cannot apply calibration patch to {relative}: {before}")
            data = data.replace(before, after)
        path.write_text(data)
    # The upstream scale input is signed: unsized 255 sign-extends its -1
    # encoding and misses the reserved E8M0 NaN. Compare at the scale width.
    candidates=list(root.glob('.bender/git/checkouts/fpnew-*/src/mxdotp/fpnew_mxdotp_multi_modules.sv'))
    if len(candidates)!=1:
        raise SystemExit('Run bender checkout first; expected one pinned FPnew checkout')
    path=candidates[0]
    data=path.read_text()
    before="operands_c_in[i] == 2**SCALE_WIDTH-1"
    after="operands_c_in[i] == {SCALE_WIDTH{1'b1}}"
    if before not in data and after not in data:
        raise SystemExit('Cannot apply E8M0 NaN comparison fix')
    path.write_text(data.replace(before,after))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rtl", type=pathlib.Path)
    prepare(parser.parse_args().rtl.resolve())
