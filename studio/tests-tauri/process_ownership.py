"""Read-only process ownership; a PID alone never establishes identity."""
import ntpath


def identity(process):
    pid = process.get("ProcessId")
    created = process.get("CreationTime")
    path = process.get("ExecutablePath")
    if (type(pid) is not int or pid <= 0 or type(created) is not int or created <= 0
            or not isinstance(path, str) or not ntpath.isabs(path)):
        return None
    # CIM creation timestamps have microsecond precision; Win32 FILETIME also
    # contains a sub-microsecond digit. Compare at the shared precision.
    return pid, created // 10, ntpath.normcase(ntpath.normpath(path))


def observe(snapshot, known):
    """Retain proven identities and discover children of currently proven parents.

    An exited parent's numeric ID is not sufficient. In particular, a child
    older than the current parent was spawned by a previous holder of that PID.
    Missing timestamps/paths fail closed. This function never kills processes.
    """
    live = {}
    for process in snapshot:
        key = identity(process)
        if key is not None:
            if key[0] in live:
                raise ValueError("Ambiguous process snapshot")
            live[key[0]] = (key, process)
    owned = set(known)
    while True:
        added = set()
        for key, process in live.values():
            parent = live.get(process.get("ParentProcessId"))
            if parent and parent[0] in owned and key[1] >= parent[0][1]:
                added.add(key)
        if added <= owned:
            return owned
        owned.update(added)


def surviving(snapshot, known):
    known_pids = {key[0] for key in known}
    remaining = []
    for process in snapshot:
        key = identity(process)
        if process.get('ProcessId') in known_pids and key is None:
            raise RuntimeError('Cannot verify whether an observed process survived')
        if key in known:
            remaining.append(key[0])
    return sorted(remaining)


def require_same_identity(expected, actual):
    if expected is None or identity(actual) != expected:
        raise RuntimeError('Refusing termination: process identity changed or is incomplete')
