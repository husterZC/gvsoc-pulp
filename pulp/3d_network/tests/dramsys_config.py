# SPDX-License-Identifier: Apache-2.0
"""Read DRAMSys JSON configuration files, including C/C++ comments."""
import json
import re


def read_json(path):
    # Preserve comment markers inside JSON strings.
    text = re.sub(r'("(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/)',
                  lambda m: m[0] if m[0].startswith('"') else '', path.read_text())
    return json.loads(text)
