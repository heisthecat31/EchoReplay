"""Finds the Echo VR install and other optional local inputs for tools and checks.

echovr.exe: REPLAY_ECHOVR_EXE (the file), then ECHOVR_DIR (the install folder, or its bin\\win10),
then the Oculus libraries registered on this PC. A variable that is set but points at nothing means
"not found"; it never falls through to a different install.
"""
import os

NOT_FOUND = ("echovr.exe was not found. Set ECHOVR_DIR to the Echo VR install folder "
             "(the one holding bin\\win10), or REPLAY_ECHOVR_EXE to echovr.exe.")


def _exe_under(folder):
    for tail in (os.path.join("bin", "win10", "echovr.exe"), "echovr.exe"):
        exe = os.path.join(folder, tail)
        if os.path.isfile(exe):
            return exe
    return None


def _oculus_libraries():
    roots = []
    try:
        import winreg
    except ImportError:
        return roots
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Oculus VR, LLC\Oculus\Libraries") as libs:
            i = 0
            while True:
                try:
                    name = winreg.EnumKey(libs, i)
                except OSError:
                    break
                i += 1
                with winreg.OpenKey(libs, name) as lib:
                    for value in ("OriginalPath", "Path"):
                        try:
                            roots.append(winreg.QueryValueEx(lib, value)[0])
                            break
                        except OSError:
                            pass
    except OSError:
        pass
    for key in (r"SOFTWARE\WOW6432Node\Oculus VR, LLC\Oculus", r"SOFTWARE\Oculus VR, LLC\Oculus"):
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
                roots.append(os.path.join(winreg.QueryValueEx(k, "Base")[0], "Software"))
        except OSError:
            pass
    if os.environ.get("ProgramFiles"):
        roots.append(os.path.join(os.environ["ProgramFiles"], "Oculus", "Software"))
    return roots


def find_echovr_exe(game_dir=None):
    """The echovr.exe to use, or None."""
    if game_dir:
        return _exe_under(game_dir)
    exe = os.environ.get("REPLAY_ECHOVR_EXE")
    if exe:
        return exe if os.path.isfile(exe) else None
    if os.environ.get("ECHOVR_DIR"):
        return _exe_under(os.environ["ECHOVR_DIR"])
    for root in _oculus_libraries():
        found = _exe_under(os.path.join(root, "Software", "ready-at-dawn-echo-arena"))
        if found:
            return found
    return None


def optional_input(env_name):
    """A private local input (an IDA database, a capture corpus) named by an env var; None if unset."""
    return os.environ.get(env_name) or None
