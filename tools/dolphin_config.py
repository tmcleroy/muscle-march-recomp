#!/usr/bin/env python3
"""Write the settings the reference Dolphin instance needs into a user folder:
frame dump to PNG, unthrottled, no stop confirmation, OSReport logging."""
import configparser, os, sys

SETTINGS = {
    "Dolphin.ini": {
        "Analytics": {"Enabled": "False", "PermissionAsked": "True"},
        "Core": {"EmulationSpeed": "0"},
        "Interface": {"ConfirmStop": "False"},
        "Movie": {"DumpFrames": "True", "DumpFramesSilent": "True"},
    },
    "GFX.ini": {"Settings": {"DumpFramesAsImages": "True"}},
    "Logger.ini": {
        "Options": {"Verbosity": "3", "WriteToFile": "True", "WriteToConsole": "False"},
        "Logs": {"OSREPORT": "True", "OSREPORT_HLE": "True", "BOOT": "True", "IOS_ES": "True"},
    },
}

user = sys.argv[1]
for name, sections in SETTINGS.items():
    path = os.path.join(user, "Config", name)
    cp = configparser.ConfigParser(strict=False, interpolation=None)
    cp.optionxform = str
    if os.path.exists(path):
        cp.read(path)
    for sec, kv in sections.items():
        if not cp.has_section(sec):
            cp.add_section(sec)
        for k, v in kv.items():
            cp.set(sec, k, v)
    with open(path, "w") as f:
        cp.write(f, space_around_delimiters=True)
